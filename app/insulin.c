// SPDX-License-Identifier: GPL-3.0
// insulin.c --- Insulin dose log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* See insulin.h. Freestanding like sensors.c: no sscanf, hand parsers that
 * stop at the first field they cannot read, and every row is validated on the
 * way in -- a log that is loaded at every launch and never rewritten must not
 * be able to wedge the app or resurrect a corrupt row forever.
 *
 * The file is a log of ASSERTIONS replayed in order (schema v2, see
 * insulin.h). Ids live here rather than in struct ins_rec because they are a
 * storage detail: the UI shows a dose, not the row that last described it. */
#include "insulin.h"
#include "clock.h"
#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf, SEEK_END */

static struct ins_rec g_ins[NINS];
static int g_nins;

int ins_count(void)
{
   return g_nins;
}

struct ins_rec ins_at(int i)
{
   struct ins_rec z = {0};
   if (i < 0 || i >= g_nins)
      return z;
   return g_ins[i];
}

int ins_copy(struct ins_rec *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   for (; n < cap && n < g_nins; n++)
      out[n] = g_ins[n];
   return n;
}

static char g_ins_path[256];

/* Column header, so an exported log is self-describing. tz_offset_s is the
 * offset assumed when the dose was entered, same rationale as readings.csv:
 * a bad local-time conversion stays repairable decades later. `written` is
 * when the assertion was made, which is NOT the dose instant: an edit made
 * today to a dose from March is written today. */
static const char g_ins_hdr[] =
    "# written,id,del,unix_time,type,units,tz_offset_s\n";

/* Id of each tail entry, index-parallel to g_ins (moved in lockstep by
 * ins_sort). Minted ids are positive and strictly increasing; rows in the old
 * four-field form get NEGATIVE ids by file order, so the two spaces can never
 * collide however the file was assembled. */
static long g_ins_id[NINS];
static long g_ins_next = 1; /* next id to mint */

const char *insulin_type_name(int type)
{
   return type == INS_FAST ? "FAST" : "SLOW";
}

/* ---- OVERFLOW EVICTS THE OLDEST BY TIME, NOT THE OLDEST BY ARRIVAL ------
 *
 * This used to drop element ZERO and leave the caller's ins_sort() to tidy up,
 * and element zero is whichever assertion was REPLAYED first -- the oldest row
 * only when the file happens to be in chronological order. It very often is
 * not: rows arrive in FILE order, and users legitimately backdate a dose they
 * forgot to log, and import history from another app, both of which append
 * rows whose dose times are older than everything already held.
 *
 * WHAT THAT LOOKED LIKE ON THE PHONE. A full tail, a backdated import, and the
 * doses from the last few days vanish from the list -- one per imported row --
 * while the month-old imported ones stay. The file still has every dose, so
 * the next launch brings the recent ones back for as long as the tail has room
 * again: it reads as a display bug that heals itself, which is the hardest
 * kind to be believed about. On a dose log it is worse than that, because
 * "what did I take yesterday" is the question this list exists to answer.
 *
 * THE RULE NOW: the tail holds the NEWEST NINS doses BY TIME. Equivalently --
 * and this is how to read the code -- push the dose, sort, drop element zero.
 * Written out rather than done that way because the arrays have no spare slot
 * and a load pushes once per assertion:
 *
 *   - room to spare: take it, and let the caller's sort file it.
 *   - full, and the arriving dose is strictly older than every dose held: it
 *     IS what sort-then-drop would discard, so it is not taken at all. It
 *     displaces nobody.
 *   - full, otherwise: find the oldest dose HELD (by time, ties to the
 *     earliest arrival, exactly as the stable sort would), drop it, take the
 *     new one.
 *
 * WHY THIS DOES NOT DISTURB ASSERTION REPLAY, which is the thing to be careful
 * of here. The file is a log of assertions replayed in FILE order, last one
 * per id wins, and a retraction can name a dose asserted thousands of rows
 * earlier. Three properties keep that intact, and none of them lives in this
 * function:
 *
 *   - ins_apply mints and advances g_ins_next from EVERY assertion it sees,
 *     before it ever reaches this function -- so an id space stays strictly
 *     increasing whether or not the dose it names fits in the window. A push
 *     refused here can never cause an id to be minted twice.
 *   - ins_apply looks the id up with ins_slot FIRST, so an amendment to a dose
 *     that IS in the tail still edits it in place and never arrives here.
 *   - a retraction whose dose is not in the tail is already a no-op, and was
 *     before this change: the tail is a bounded window and the dose it names
 *     is outside it. Eviction cannot resurrect anything, because eviction
 *     REMOVES; what would resurrect a deleted dose is a later row read as an
 *     assertion that it exists, which is ins_parse_assert's `del` check, not
 *     this.
 *
 * One honest limit, unchanged by this and worth naming: ins_apply's edit path
 * writes over the record in place, so an amendment that moves a dose's time
 * far into the past leaves the tail holding a dose older than one this
 * function had refused. The tail is still the last-wins state of every id it
 * holds; it is just not, in that one case, exactly the newest NINS by time.
 * Re-evaluating eviction on an edit would mean an amendment could push a dose
 * out of the list, which is a worse surprise than a slightly wide window. */
static void ins_push(const struct ins_rec *r, long id)
{
   if (g_nins < NINS) {
      g_ins_id[g_nins] = id;
      g_ins[g_nins++]  = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < g_nins; i++)
      if (g_ins[i].t < g_ins[oldest].t)
         oldest = i;
   if (r->t < g_ins[oldest].t)
      return; /* the arriving dose is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < g_nins; i++) {
      g_ins[i - 1]    = g_ins[i];
      g_ins_id[i - 1] = g_ins_id[i];
   }
   g_ins[g_nins - 1]    = *r;
   g_ins_id[g_nins - 1] = id;
}

/* Where `id` currently sits in the tail, or -1. */
static int ins_slot(long id)
{
   for (int i = 0; i < g_nins; i++)
      if (g_ins_id[i] == id)
         return i;
   return -1;
}

static void ins_drop(int at)
{
   for (int i = at + 1; i < g_nins; i++) {
      g_ins[i - 1]    = g_ins[i];
      g_ins_id[i - 1] = g_ins_id[i];
   }
   g_nins--;
}

/* Keep the tail sorted by DOSE TIME (stable insertion sort; the tail is
 * tiny). FILE order is arrival order, and users legitimately backdate,
 * edit timestamps, and import history -- everything that displays or
 * evicts "oldest first" must mean oldest BY TIME, not by arrival. */
static void ins_sort(void)
{
   for (int i = 1; i < g_nins; i++) {
      struct ins_rec r = g_ins[i];
      long id          = g_ins_id[i];
      int j            = i - 1;
      while (j >= 0 && g_ins[j].t > r.t) {
         g_ins[j + 1]    = g_ins[j];
         g_ins_id[j + 1] = g_ins_id[j];
         j--;
      }
      g_ins[j + 1]    = r;
      g_ins_id[j + 1] = id;
   }
}

/* One row of the log: an assertion about the dose identified by `id`.
 * `del` retracts it; otherwise the dose IS these values from now on. */
struct ins_assert {
   long id;
   int del;
   struct ins_rec r;
};

static int ins_ncommas(const char *p, const char *e)
{
   int n = 0;
   for (const char *q = p; q < e; q++)
      if (*q == ',')
         n++;
   return n;
}

/* Parse one row; 1 = valid. Validate EVERYTHING: a header line parses as
 * t == 0 and is rejected here, and a corrupt row must not load as a plausible
 * dose. The time bounds are generous -- a dose is user-entered and may
 * honestly be backdated or even slightly future-dated -- but an absurd digit
 * run, capped by the cursor at CSV_MAX_DIGITS, must not slip through as a
 * huge "positive epoch".
 *
 * Six or more commas is the v2 form; anything less is read as the original
 * four-field row and given `legacy_id`, which the caller makes NEGATIVE so it
 * can never collide with a minted one. */
/* A comment line, or nothing at all. Neither is a dose and neither is
 * damage: the file starts with a header and a trailing newline leaves an
 * empty last line. */
static int ins_line_blank(const char *p, const char *e)
{
   if (p >= e)
      return 1;
   return *p == '#';
}

static int ins_parse_assert(const char *p, const char *e, long legacy_id,
                            struct ins_assert *a)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   a->del = 0;
   /* THE FIELDS THIS FORMAT WILL NOT GUESS AT. Every other column is caught
    * by a range check below -- an empty `t` reads 0 and fails `t <= 0`, an
    * empty `type` is neither SLOW nor FAST -- but `del` has no such luck: a
    * missing retraction flag reads as 0, which is the valid and much more
    * common answer, so a truncated row would silently resurrect a dose the
    * user deleted. It is asked about explicitly instead. */
   enum csv_field delok = CSV_FIELD_OK;
   if (ins_ncommas(p, e) >= 6) {
      (void)csv_num(&c, 0); /* written: ordering is FILE order, not this */
      csv_sep(&c);
      a->id = csv_num(&c, 0);
      csv_sep(&c);
      a->del = (int)csv_num(&c, &delok);
      csv_sep(&c);
      a->r.t = csv_num(&c, 0);
      csv_sep(&c);
      a->r.type = (int)csv_num(&c, 0);
      csv_sep(&c);
      a->r.units = (int)csv_num(&c, 0);
      if (a->id == 0 || delok != CSV_FIELD_OK || (a->del != 0 && a->del != 1))
         return 0;
      if (a->del)
         return 1; /* a retraction names a dose; it does not describe one */
   } else {
      a->id  = legacy_id;
      a->r.t = csv_num(&c, 0);
      csv_sep(&c);
      a->r.type = (int)csv_num(&c, 0);
      csv_sep(&c);
      a->r.units = (int)csv_num(&c, 0);
   }
   if (a->r.t <= 0 || a->r.t >= INS_T_MAX)
      return 0;
   if (a->r.type != INS_SLOW && a->r.type != INS_FAST)
      return 0;
   if (a->r.units < INS_UNITS_MIN || a->r.units > INS_UNITS_MAX)
      return 0;
   return 1;
}

/* Replay one assertion onto the tail: last one per id wins. */
static void ins_apply(const struct ins_assert *a)
{
   if (a->id >= g_ins_next)
      g_ins_next = a->id + 1;
   int at = ins_slot(a->id);
   if (a->del) {
      if (at >= 0)
         ins_drop(at);
      return;
   }
   if (at >= 0)
      g_ins[at] = a->r; /* an edit keeps the dose's place in the tail */
   else
      ins_push(&a->r, a->id);
}

/* Returns 0 when the file was read whole -- including the first-run case where
 * there is nothing to read -- and -1 when a read FAILED partway.
 *
 * The distinction is the point. What is lost on a short read is not a file, it
 * is RECORD: every dose the user has logged, which the form
 * pre-populates from and the plot draws. Whatever was parsed before the failure
 * is kept (a prefix of the truth is better than nothing on screen), but the
 * caller is told, so the app can say so instead of presenting a silently short
 * history as complete. store_load has answered this way since the same defect
 * was found in it. */
int insulin_load(void)
{
   g_nins       = 0;
   g_ins_next   = 1;
   long nlegacy = 0;
   int fd       = open(g_ins_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   /* Stream the whole file a line at a time (sensors.c pattern): the tail
    * buffer keeps only the last NINS doses, so memory stays bounded no matter
    * how many years -- or how many corrections -- the file has grown. */
   char buf[1024];
   char line[128];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0; /* anything that means "what you have is short" */
   long n      = 0;
   struct ins_assert a;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            /* A header or a blank line is neither a dose nor damage; a line
             * longer than any assertion, or one that does not parse, is
             * damage. Skipping the row is right -- it is not a dose -- but
             * this file is the record of what was INJECTED and it is never
             * rewritten, so a hole in it is permanent, and the load says so
             * instead of returning success. */
            if (!over && ins_line_blank(line, line + llen)) {
               /* nothing to take, nothing wrong */
            } else if (!over && ins_parse_assert(line, line + llen,
                                                 -(nlegacy + 1), &a)) {
               if (a.id < 0)
                  nlegacy++;
               ins_apply(&a);
            } else {
               damaged = 1;
            }
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0) {
      /* NO TRAILING NEWLINE: the file was cut while being appended to, and
       * the bytes are NOT applied. They may parse -- an assertion is six
       * numbers -- and still be half of one, and this file is the record of
       * what was injected. A dose that was never given, replayed at every
       * launch, is the worst row this app can invent. */
      damaged = 1;
   }
   close(fd);
   ins_sort();
   return (n < 0 || damaged) ? -1 : 0;
}

/* Append one assertion. The log is only ever extended -- there is no rewrite
 * path left, so no crash window in which the file could be truncated. */
static int ins_write_row(long id, int del, long t, int type, int units, long tz)
{
   char b[96];
   int n = snprintf(b, sizeof b, "%ld,%ld,%d,%ld,%d,%d,%ld\n", realtime_s(), id,
                    del, t, type, units, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION, header included: see log_append. */
   int rc = log_append(g_ins_path, g_ins_hdr, (int)sizeof g_ins_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc;     /* LOG_DAMAGED travels: the file may hold a partial row */
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

static int ins_vet(long t, int type, int units)
{
   if (t <= 0 || t >= INS_T_MAX)
      return 0;
   if (type != INS_SLOW && type != INS_FAST)
      return 0;
   if (units < INS_UNITS_MIN || units > INS_UNITS_MAX)
      return 0;
   return 1;
}

int insulin_append(long t, int type, int units, long tz)
{
   if (!ins_vet(t, type, units))
      return -1;
   long id = g_ins_next;
   /* THE WRITE'S OWN ANSWER TRAVELS. LOG_DAMAGED (a partial row that could
    * not be rolled back) is not the same as "the dose was not logged": the
    * file needs saying so, and a caller that retries is appending onto a
    * half-written line. */
   int rc = ins_write_row(id, 0, t, type, units, tz);
   if (rc != LOG_OK)
      return rc;
   struct ins_rec r = {t, type, units};
   g_ins_next       = id + 1;
   ins_push(&r, id);
   ins_sort(); /* a backdated dose files into place immediately */
   return 0;
}

/* The tail slot of the LAST dose matching `orig` by content, or -1. Content,
 * not position, so a stale index from the UI can never touch another dose. */
static int ins_match(const struct ins_rec *orig)
{
   for (int i = g_nins - 1; i >= 0; i--)
      if (g_ins[i].t == orig->t && g_ins[i].type == orig->type &&
          g_ins[i].units == orig->units)
         return i;
   return -1;
}

int insulin_update(const struct ins_rec *orig, long t, int type, int units,
                   long tz)
{
   if (!ins_vet(t, type, units))
      return -1;
   int at = ins_match(orig);
   if (at < 0)
      return -1;
   if (ins_write_row(g_ins_id[at], 0, t, type, units, tz) != 0)
      return -1;
   g_ins[at].t     = t;
   g_ins[at].type  = type;
   g_ins[at].units = units;
   ins_sort();
   return 0;
}

int insulin_delete(const struct ins_rec *orig)
{
   int at = ins_match(orig);
   if (at < 0)
      return -1;
   /* A retraction still carries the dose it retracts, so the row remains
    * readable on its own -- the log is a history, not a diff. */
   if (ins_write_row(g_ins_id[at], 1, g_ins[at].t, g_ins[at].type,
                     g_ins[at].units, 0) != 0)
      return -1;
   ins_drop(at);
   return 0;
}

int insulin_last_units(int type)
{
   /* The tail is time-sorted, so this is the units of the LATEST dose of
    * `type` -- the value the form pre-populates with. */
   for (int i = g_nins - 1; i >= 0; i--)
      if (g_ins[i].type == type)
         return g_ins[i].units;
   return 0;
}

/* ---- the dose-entry form (see insulin.h) ---- */

/* The amount to offer for `type`: its last logged value, or one unit when
 * nothing has been logged for it yet. Never zero -- a form offering 0 U
 * invites confirming a dose that records nothing. */
static int form_units_for(int type)
{
   int lu = insulin_last_units(type);
   return lu > 0 ? lu : 1;
}

void ins_form_open(struct ins_form *f, int type, long now)
{
   if (!f)
      return;
   if (type >= 0)
      f->type = type;
   /* The WHOLE minute. Seconds would make two doses logged moments apart two
    * distinct instants, and the log dedups on the instant. */
   f->t     = now - (now % 60);
   f->units = form_units_for(f->type);
   f->edit  = -1;
}

void ins_form_toggle_type(struct ins_form *f)
{
   if (!f)
      return;
   f->type = (f->type == INS_FAST) ? INS_SLOW : INS_FAST;
   /* A NEW dose's amount follows the type; an EDIT keeps the amount on
    * record, because that is the number the user is correcting. */
   if (f->edit < 0)
      f->units = form_units_for(f->type);
}

/* The dose log's filename. */
int insulin_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_ins_path, sizeof g_ins_path, dir, "/insulin.csv");
   return ok;
}

const char *insulin_path(void)
{
   return g_ins_path;
}
