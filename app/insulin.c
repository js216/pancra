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
#include "dexlibc.h"
#include "util.h"
#include <stdio.h> /* snprintf, SEEK_END */

struct ins_rec g_ins[NINS];
int g_nins;
char g_ins_path[256];

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

/* Decimal reader, same shape (and same digit cap rationale) as sensors.c. */
static long rdnum(const char **p, const char *e)
{
   long v        = 0;
   int nd        = 0;
   int neg       = 0;
   const char *q = *p;
   if (q < e && *q == '-') {
      neg = 1;
      q++;
   }
   while (q < e && *q >= '0' && *q <= '9') {
      if (nd < 18) {
         v = (v * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   *p = q;
   return neg ? -v : v;
}

static void rdsep(const char **p, const char *e)
{
   if (*p < e && **p == ',')
      (*p)++;
}

/* Overflow drops the FIRST ROW IN THE TAIL, which is the oldest by ARRIVAL,
 * not necessarily the oldest by time: rows are pushed in file order and the
 * sort runs afterwards, so a backdated entry loaded near the end of the file
 * can evict a row older than itself. Display-only -- the file keeps
 * everything and the next launch re-reads it -- but the tail is a window on
 * arrival order, which is what the file is, not a strict time window. */
static void ins_push(const struct ins_rec *r, long id)
{
   if (g_nins == NINS) {
      for (int i = 1; i < NINS; i++) {
         g_ins[i - 1]    = g_ins[i];
         g_ins_id[i - 1] = g_ins_id[i];
      }
      g_nins--;
   }
   g_ins_id[g_nins] = id;
   g_ins[g_nins++]  = *r;
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
 * run, capped by rdnum at 18 digits, must not slip through as a huge
 * "positive epoch".
 *
 * Six or more commas is the v2 form; anything less is read as the original
 * four-field row and given `legacy_id`, which the caller makes NEGATIVE so it
 * can never collide with a minted one. */
static int ins_parse_assert(const char *p, const char *e, long legacy_id,
                            struct ins_assert *a)
{
   const char *q = p;
   a->del        = 0;
   if (ins_ncommas(p, e) >= 6) {
      (void)rdnum(&q, e); /* written: ordering is FILE order, not this */
      rdsep(&q, e);
      a->id = rdnum(&q, e);
      rdsep(&q, e);
      a->del = (int)rdnum(&q, e);
      rdsep(&q, e);
      a->r.t = rdnum(&q, e);
      rdsep(&q, e);
      a->r.type = (int)rdnum(&q, e);
      rdsep(&q, e);
      a->r.units = (int)rdnum(&q, e);
      if (a->id == 0 || (a->del != 0 && a->del != 1))
         return 0;
      if (a->del)
         return 1; /* a retraction names a dose; it does not describe one */
   } else {
      a->id  = legacy_id;
      a->r.t = rdnum(&q, e);
      rdsep(&q, e);
      a->r.type = (int)rdnum(&q, e);
      rdsep(&q, e);
      a->r.units = (int)rdnum(&q, e);
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

void insulin_load(void)
{
   g_nins       = 0;
   g_ins_next   = 1;
   long nlegacy = 0;
   int fd       = open(g_ins_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* Stream the whole file a line at a time (sensors.c pattern): the tail
    * buffer keeps only the last NINS doses, so memory stays bounded no matter
    * how many years -- or how many corrections -- the file has grown. */
   char buf[1024];
   char line[128];
   int llen = 0;
   int over = 0; /* over-long line: skip, never parse a truncation */
   long n   = 0;
   struct ins_assert a;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (!over &&
                ins_parse_assert(line, line + llen, -(nlegacy + 1), &a)) {
               if (a.id < 0)
                  nlegacy++;
               ins_apply(&a);
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
   if (llen > 0 && !over && /* final line with no trailing newline */
       ins_parse_assert(line, line + llen, -(nlegacy + 1), &a))
      ins_apply(&a);
   close(fd);
   ins_sort();
}

/* Append one assertion. The log is only ever extended -- there is no rewrite
 * path left, so no crash window in which the file could be truncated. */
static int ins_write_row(long id, int del, long t, int type, int units, long tz)
{
   int fd = open(g_ins_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return -1;
   if (lseek(fd, 0, SEEK_END) == 0) { /* self-describing header when new */
      if (write(fd, g_ins_hdr, sizeof g_ins_hdr - 1) < 0) { /* best effort */
      }
   }
   char b[96];
   int n = snprintf(b, sizeof b, "%ld,%ld,%d,%ld,%d,%d,%ld\n", realtime_s(), id,
                    del, t, type, units, tz);
   n     = clampn(n, sizeof b);
   long w = write(fd, b, n);
   if (w != n) {
      /* Roll a partial line back so it cannot merge with the next append
       * into one unparseable row (the sensors.csv rule). */
      if (w > 0)
         (void)ftruncate(fd, lseek(fd, 0, SEEK_END) - w);
      close(fd);
      return -1;
   }
   close(fd);
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
   if (ins_write_row(id, 0, t, type, units, tz) != 0)
      return -1;
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
