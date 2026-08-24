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
#include "dexlibc.h"
#include "insrow.h" /* INS_*: what a dose row can say */
#include "thread.h" /* ins_lk: the tail is published, not filled in place */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf, SEEK_END */

/* ---- THE TAIL IS A VALUE, AND IT IS PUBLISHED ---------------
 *
 * WHY THE TAIL IS PUBLISHED AND NOT WRITTEN IN PLACE. Four bare globals --
 * the records, their ids, the count and the next id -- written in place by
 * insulin_load and read in place by the screen are fine while only one
 * thread exists, and a RESTORE is a second one: sync_restore rewrites
 * insulin.csv
 * and then calls pancra_logs_reload on the SYNC WORKER, which begins by
 * setting the count to zero and refilling the array a row at a time, while
 * the main thread is copying out of it to draw the log screen. A reader
 * landing in the middle sees an empty log, or half a log, or -- because the
 * count is written before the records it counts -- rows that are whatever
 * the previous contents left behind.
 *
 * So the tail is one object, there is a leaf lock over it, and a load builds
 * a SEPARATE one and publishes it with a single assignment under that lock.
 * A reader sees the log before the restore or the log after it, never a
 * moment in between. Everything in this file that touches the tail takes a
 * pointer to it, which is also what makes the staged copy possible.
 *
 * CROSS-DOMAIN, THIS IS DELIBERATELY NOT ATOMIC. A restore republishes four
 * logs (insulin, weight, food, exercise) and each publishes atomically on
 * its own; a frame drawn between two of them shows the new insulin beside
 * food that has not reloaded yet. Making the set atomic would mean holding all
 * four locks across four file reads on a worker thread, and the frame builder
 * takes every one of them -- that is a repaint waiting on flash, which is the
 * ANR shape this app has already been killed for. One log at a time, each
 * coherent, is the trade; see pancra_logs_reload in app/reading.c. */
struct ins_tail {
   struct ins_rec r[NINS];
   long id[NINS]; /* index-parallel to r; moved in lockstep by ins_sort */
   int n;
   long next; /* next id to mint */
};

static struct ins_tail g_live;

/* A LEAF. Taken innermost, never held across a call into another module and
 * never across file I/O: insulin_append writes its row FIRST and only then
 * takes this to update the tail. See app/thread.h. */
static struct mutex ins_lk = MUTEX_INIT;

int ins_count(void)
{
   mutex_lock(&ins_lk);
   int n = g_live.n;
   mutex_unlock(&ins_lk);
   return n;
}

struct ins_rec ins_at(int i)
{
   struct ins_rec z = {0};
   mutex_lock(&ins_lk);
   if (i >= 0 && i < g_live.n)
      z = g_live.r[i];
   mutex_unlock(&ins_lk);
   return z;
}

int ins_copy(struct ins_rec *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&ins_lk);
   for (; n < cap && n < g_live.n; n++)
      out[n] = g_live.r[n];
   mutex_unlock(&ins_lk);
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

/* (The ids live in struct ins_tail above, index-parallel to the records.
 * Minted ids are positive and strictly increasing; rows in the old
 * four-field form get NEGATIVE ids by file order, so the two spaces can never
 * collide however the file was assembled.) */

const char *insulin_type_name(int type)
{
   return type == INS_FAST ? "FAST" : "SLOW";
}

/* ---- OVERFLOW EVICTS THE OLDEST BY TIME, NOT THE OLDEST BY ARRIVAL ------
 *
 * Dropping element ZERO and leaving the caller's ins_sort() to tidy up
 * discards whichever row was PUSHED first -- the oldest one only when the
 * file happens to be in chronological order. It very often is not: rows
 * arrive in FILE order, and users legitimately backdate a dose they forgot to
 * log, and import history from another app, both of which append rows whose
 * dose times are older than everything already held.
 *
 * WHAT THAT LOOKS LIKE ON THE PHONE. A full tail, a backdated import, and the
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
 *   - ins_apply mints and advances the tail's `next` from EVERY assertion it
 *     sees,
 *     before it ever reaches this function -- so an id space stays strictly
 *     increasing whether or not the dose it names fits in the window. A push
 *     refused here can never cause an id to be minted twice.
 *   - ins_apply looks the id up with ins_slot FIRST, so an amendment to a dose
 *     that IS in the tail still edits it in place and never arrives here.
 *   - a retraction whose dose is not in the tail is already a no-op, and was
 *     whatever the eviction rule: the tail is a bounded window and the dose
 *     it names is outside it. Eviction cannot resurrect anything, because it
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
static void ins_push(struct ins_tail *t, const struct ins_rec *r, long id)
{
   if (t->n < NINS) {
      t->id[t->n] = id;
      t->r[t->n]  = *r;
      t->n++;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < t->n; i++)
      if (t->r[i].t < t->r[oldest].t)
         oldest = i;
   if (r->t < t->r[oldest].t)
      return; /* the arriving dose is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < t->n; i++) {
      t->r[i - 1]  = t->r[i];
      t->id[i - 1] = t->id[i];
   }
   t->r[t->n - 1]  = *r;
   t->id[t->n - 1] = id;
}

/* Where `id` currently sits in the tail, or -1. */
static int ins_slot(const struct ins_tail *t, long id)
{
   for (int i = 0; i < t->n; i++)
      if (t->id[i] == id)
         return i;
   return -1;
}

static void ins_drop(struct ins_tail *t, int at)
{
   for (int i = at + 1; i < t->n; i++) {
      t->r[i - 1]  = t->r[i];
      t->id[i - 1] = t->id[i];
   }
   t->n--;
}

/* Keep the tail sorted by DOSE TIME (stable insertion sort; the tail is
 * tiny). FILE order is arrival order, and users legitimately backdate,
 * edit timestamps, and import history -- everything that displays or
 * evicts "oldest first" must mean oldest BY TIME, not by arrival. */
static void ins_sort(struct ins_tail *t)
{
   for (int i = 1; i < t->n; i++) {
      struct ins_rec r = t->r[i];
      long id          = t->id[i];
      int j            = i - 1;
      while (j >= 0 && t->r[j].t > r.t) {
         t->r[j + 1]  = t->r[j];
         t->id[j + 1] = t->id[j];
         j--;
      }
      t->r[j + 1]  = r;
      t->id[j + 1] = id;
   }
}

/* One row of the log: an assertion about the dose identified by `id`.
 * `del` retracts it; otherwise the dose IS these values from now on. */
struct ins_assert {
   long id;
   int del;
   struct ins_rec r;
};

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

/* ---- ONE DECODER, SHARED WITH THE SERVER --------------------
 *
 * The grammar, the two dialects and the ranges are lib/insrow.h's now: the
 * server replays this same log to render it, and the two hand-written
 * readers had already drifted (its strtoll took a numeric PREFIX and any
 * nonzero `del` as a retraction, while this one required exact numbers and
 * exactly 0 or 1). What is left here is the shape this module works in. */
static int ins_parse_assert(const char *p, const char *e, long legacy_id,
                            struct ins_assert *a)
{
   struct ins_row row;
   if (!ins_row_decode(p, e, legacy_id, &row))
      return 0;
   a->id      = row.id;
   a->del     = row.del;
   a->r.t     = row.t;
   a->r.type  = row.type;
   a->r.milli = row.milli;
   return 1;
}

/* Replay one assertion onto the tail: last one per id wins. */
static void ins_apply(struct ins_tail *t, const struct ins_assert *a)
{
   if (a->id >= t->next)
      t->next = a->id + 1;
   int at = ins_slot(t, a->id);
   if (a->del) {
      if (at >= 0)
         ins_drop(t, at);
      return;
   }
   if (at >= 0)
      t->r[at] = a->r; /* an edit keeps the dose's place in the tail */
   else
      ins_push(t, &a->r, a->id);
}

/* Returns 0 when the file was read whole -- including the first-run case where
 * there is nothing to read -- and -1 when a read FAILED partway.
 *
 * The distinction is the point. What is lost on a short read is not a file, it
 * is RECORD: every dose the user has logged, which the form
 * pre-populates from and the plot draws. Whatever was parsed before the failure
 * is kept (a prefix of the truth is better than nothing on screen), but the
 * caller is told, so the app can say so rather than presenting a silently short
 * history as complete. store_load has answered this way since the same defect
 * was found in it. */
/* THE STAGING TAIL. Static rather than automatic because it is 6 KB and this
 * runs on a service tick's thread; private to the loader, which is the only
 * thing that touches it, and never published as anything but a copy. */
static struct ins_tail g_stage;

int insulin_load(void)
{
   struct ins_tail *t = &g_stage;
   t->n               = 0;
   t->next            = 1;
   long nlegacy       = 0;
   int fd             = open(g_ins_path, O_RDONLY, 0);
   if (fd < 0) {
      /* NO FILE IS A RESULT, AND IT IS PUBLISHED TOO. Zeroing the live tail
       * at the top of this function would empty the log on the way past for a
       * file that is not there. The zeroing happens on the staged copy, which
       * means the empty tail has
       * to be published here or a deleted log would go on being displayed --
       * exactly what a restore-to-empty must not leave behind. */
      mutex_lock(&ins_lk);
      g_live = *t;
      mutex_unlock(&ins_lk);
      return errno == ENOENT ? 0 : -1;
   }
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
             * rather than returning success. */
            if (!over && ins_line_blank(line, line + llen)) {
               /* nothing to take, nothing wrong */
            } else if (!over && ins_parse_assert(line, line + llen,
                                                 -(nlegacy + 1), &a)) {
               if (a.id < 0)
                  nlegacy++;
               ins_apply(t, &a);
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
   ins_sort(t);
   /* PUBLISHED WHOLE, OR NOT AT ALL. One assignment under the
    * leaf lock: a reader holds the log from before this call or the log from
    * after it.
    *
    * A DAMAGED FILE IS STILL PUBLISHED, and that is the same rule as before
    * -- a prefix of the record beats a blank screen, and the caller is told.
    * What is new is that the prefix becomes visible all at once rather than
    * a row at a time. */
   mutex_lock(&ins_lk);
   g_live = *t;
   mutex_unlock(&ins_lk);
   return (n < 0 || damaged) ? -1 : 0;
}

/* Append one assertion. The log is only ever extended -- there is no rewrite
 * path left, so no crash window in which the file could be truncated. */
static int ins_write_row(long id, int del, long t, int type, int milli,
                         long tz)
{
   char b[96];
   /* THE DOSE AS A PERSON WRITES IT -- "20", "0.5", "16.5". The column has
    * always been a decimal; whole-unit rows simply never had a fraction to
    * show, which is why every row already in the file still reads. */
   char u[16];
   (void)ins_units_str(milli, u, sizeof u);
   int n = snprintf(b, sizeof b, "%ld,%ld,%d,%ld,%d,%s,%ld\n", realtime_s(),
                    id, del, t, type, u, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION, header included: see log_append. */
   int rc = log_append(g_ins_path, g_ins_hdr, (int)sizeof g_ins_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc;     /* LOG_DAMAGED travels: the file may hold a partial row */
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

static int ins_vet(long t, int type, int milli)
{
   if (t <= 0 || t >= INS_T_MAX)
      return 0;
   if (type != INS_SLOW && type != INS_FAST)
      return 0;
   if (milli < INS_MILLI_MIN || milli > INS_MILLI_MAX)
      return 0;
   return 1;
}

int insulin_append(long t, int type, int milli, long tz)
{
   if (!ins_vet(t, type, milli))
      return -1;
   mutex_lock(&ins_lk);
   long id = g_live.next;
   mutex_unlock(&ins_lk);
   /* THE WRITE'S OWN ANSWER TRAVELS. LOG_DAMAGED (a partial row that could
    * not be rolled back) is not the same as "the dose was not logged": the
    * file needs saying so, and a caller that retries is appending onto a
    * half-written line. */
   int rc = ins_write_row(id, 0, t, type, milli, tz);
   if (rc != LOG_OK)
      return rc;
   struct ins_rec r = {t, type, milli};
   /* THE FILE FIRST, THE TAIL UNDER THE LOCK. The write is outside it for
    * the reason app/thread.h gives: a leaf is never held across flash. */
   mutex_lock(&ins_lk);
   g_live.next = id + 1;
   ins_push(&g_live, &r, id);
   ins_sort(&g_live); /* a backdated dose files into place immediately */
   mutex_unlock(&ins_lk);
   return 0;
}

/* The tail slot of the LAST dose matching `orig` by content, or -1. Content,
 * not position, so a stale index from the UI can never touch another dose. */
/* CALLER HOLDS ins_lk: the slot it answers with is used to write the tail,
 * and an index released and re-taken names a different dose after a restore
 * publishes between the two. */
static int ins_match(const struct ins_rec *orig)
{
   for (int i = g_live.n - 1; i >= 0; i--)
      if (g_live.r[i].t == orig->t && g_live.r[i].type == orig->type &&
          g_live.r[i].milli == orig->milli)
         return i;
   return -1;
}

int insulin_update(const struct ins_rec *orig, long t, int type, int milli,
                   long tz)
{
   if (!ins_vet(t, type, milli))
      return -1;
   /* THE ROW ID IS READ UNDER THE LOCK, THE FILE IS WRITTEN WITHOUT IT, AND
    * THE TAIL IS RE-FOUND UNDER IT. The slot cannot be carried across the
    * write: a restore publishing in between would leave `at` pointing at a
    * different dose in a different tail. Re-matching by CONTENT is what the
    * function already does, and doing it twice is cheap on 256 rows. */
   mutex_lock(&ins_lk);
   int at  = ins_match(orig);
   long id = (at >= 0) ? g_live.id[at] : 0;
   mutex_unlock(&ins_lk);
   if (at < 0)
      return -1;
   if (ins_write_row(id, 0, t, type, milli, tz) != 0)
      return -1;
   mutex_lock(&ins_lk);
   at = ins_match(orig);
   if (at >= 0) {
      g_live.r[at].t     = t;
      g_live.r[at].type  = type;
      g_live.r[at].milli = milli;
      ins_sort(&g_live);
   }
   mutex_unlock(&ins_lk);
   return 0;
}

int insulin_delete(const struct ins_rec *orig)
{
   mutex_lock(&ins_lk);
   int at           = ins_match(orig);
   struct ins_rec r = {0, 0, 0};
   long id          = 0;
   if (at >= 0) {
      r  = g_live.r[at];
      id = g_live.id[at];
   }
   mutex_unlock(&ins_lk);
   if (at < 0)
      return -1;
   /* A retraction still carries the dose it retracts, so the row remains
    * readable on its own -- the log is a history, not a diff. */
   if (ins_write_row(id, 1, r.t, r.type, r.milli, 0) != 0)
      return -1;
   mutex_lock(&ins_lk);
   at = ins_match(orig); /* re-found: see insulin_update */
   if (at >= 0)
      ins_drop(&g_live, at);
   mutex_unlock(&ins_lk);
   return 0;
}

int insulin_last_units(int type)
{
   /* The tail is time-sorted, so this is the units of the LATEST dose of
    * `type` -- the value the form pre-populates with. */
   int u = 0;
   mutex_lock(&ins_lk);
   for (int i = g_live.n - 1; i >= 0 && !u; i--)
      if (g_live.r[i].type == type)
         u = g_live.r[i].milli;
   mutex_unlock(&ins_lk);
   return u;
}

/* ---- the dose-entry form (see insulin.h) ---- */

/* The amount to offer for `type`: its last logged value, or one unit when
 * nothing has been logged for it yet. Never zero -- a form offering 0 U
 * invites confirming a dose that records nothing. */
static int form_units_for(int type)
{
   int lu = insulin_last_units(type);
   return lu > 0 ? lu : INS_MILLI;
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
   f->milli = form_units_for(f->type);
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
      f->milli = form_units_for(f->type);
}

/* The dose log's filename. */
int insulin_paths(const char *dir)
{
   int ok = 1;
   if (!(data_path(g_ins_path, sizeof g_ins_path, dir, "/insulin.csv")))
      ok = 0;
   return ok;
}

const char *insulin_path(void)
{
   return g_ins_path;
}
