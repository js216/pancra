// SPDX-License-Identifier: GPL-3.0
// weight.c --- Body-weight log: an editable CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* See weight.h. Freestanding like insulin.c, whose shape this follows
 * deliberately: hand parsers with digit caps, every row validated on the way
 * in, and a partial write rolled back so it cannot merge with the next append
 * into one unparseable row. */
#include "weight.h"
#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "log.h"    /* LOGW: an edit that landed but could not be re-read */
#include "thread.h" /* wt_lk: the tail is published, not filled in place */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf, SEEK_END */

/* ---- THE TAIL IS A VALUE, AND IT IS PUBLISHED ---------------
 *
 * Same shape and the same reason as insulin.c's: zeroing the count and
 * refilling the array in place is a torn read waiting for a RESTORE, which
 * calls weight_load on the sync worker while the main thread is drawing the
 * weight table out of it.
 * A reader landing mid-load sees an empty log, or a short one, or rows the
 * count says are there and the previous contents actually left. So the tail
 * is one object behind a leaf lock, and a load builds a separate one and
 * publishes it with a single assignment. */
struct wt_tail {
   struct wt_rec r[NWT];
   int n;
};

static struct wt_tail g_live;

/* A LEAF: taken innermost, never held across another module's call and never
 * across file I/O -- every writer here appends or rewrites the file first
 * and takes this only to bring the tail into line. See app/thread.h. */
static struct mutex wt_lk = MUTEX_INIT;

#ifdef APP_FAULTS
void (*weight_fault_before_reload)(void);
#endif
int wt_count(void)
{
   mutex_lock(&wt_lk);
   int n = g_live.n;
   mutex_unlock(&wt_lk);
   return n;
}

struct wt_rec wt_at(int i)
{
   struct wt_rec z = {0, 0};
   mutex_lock(&wt_lk);
   if (i >= 0 && i < g_live.n)
      z = g_live.r[i];
   mutex_unlock(&wt_lk);
   return z;
}

struct wt_rec wt_newest(void)
{
   /* THE INDEX AND THE READ IN ONE HOLD. Asking wt_count() and then wt_at()
    * is two holds with a restore able to publish between them, which answers
    * with a row that is not the newest -- or with nothing, on a log that has
    * just got shorter. */
   struct wt_rec z = {0, 0};
   mutex_lock(&wt_lk);
   if (g_live.n > 0)
      z = g_live.r[g_live.n - 1];
   mutex_unlock(&wt_lk);
   return z;
}

int wt_copy(struct wt_rec *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&wt_lk);
   for (; n < cap && n < g_live.n; n++)
      out[n] = g_live.r[n];
   mutex_unlock(&wt_lk);
   return n;
}

static char g_wt_path[256];

/* Column header, so an exported log is self-describing. GRAMS is named in the
 * header because the display unit is a preference and the file must not need
 * one to be read decades later. */
static const char g_wt_hdr[] = "# unix_time,grams,tz_offset_s\n";

const char *wt_unit_name(int units)
{
   return units == WT_LB ? "LB" : "KG";
}

long wt_from_tenths(int tenths, int units)
{
   if (tenths <= 0)
      return 0;
   long g = 0;
   if (units == WT_LB) {
      /* 1 lb = 453.59237 g, so a TENTH of a pound is 45.359237 g. Scaled
       * integer arithmetic with a half added before the divide, i.e. round to
       * nearest: 1540 (154.0 lb) -> 69853 g, which wt_to_tenths renders back
       * as exactly 1540. */
      g = (((long)tenths * 4535924L) + 50000L) / 100000L;
   } else {
      g = (long)tenths * 100L; /* tenths of a kg */
   }
   if (g < WT_MIN_G || g > WT_MAX_G)
      return 0;
   return g;
}

int wt_to_tenths(long g, int units)
{
   if (g <= 0)
      return 0;
   if (units == WT_LB) /* g / 45.359237, rounded to nearest */
      return (int)(((g * 100000L) + 2267962L) / 4535924L);
   return (int)((g + 50L) / 100L);
}

/* ---- OVERFLOW EVICTS THE OLDEST BY TIME, NOT THE OLDEST BY ARRIVAL ------
 *
 * Dropping element ZERO and letting the caller's wt_sort() tidy up discards
 * whichever row was PUSHED first -- which is the oldest row only when the file
 * happens to be in chronological order. It very often is not. Rows arrive in
 * FILE order, and a user who imports a scale's history, or types in a weigh-in
 * they forgot to log last month, appends rows whose timestamps are older than
 * everything already in the tail.
 *
 * WHAT THAT LOOKS LIKE ON THE PHONE. A full tail, a backdated import, and the
 * weigh-ins from the last few days disappear from the table -- one per
 * imported row -- while the imported month-old ones sit there instead. Nothing
 * is lost from the file, so a restart brings the recent ones back for as long
 * as the tail has room again; it reads exactly like a display bug that fixes
 * itself, which is the hardest kind to be believed about.
 *
 * THE RULE: the tail holds the NEWEST NWT rows BY TIME. Equivalently --
 * and this is how to read the code -- insert the row, sort, drop element zero.
 * Written out rather than actually done that way because the array has no
 * spare slot and the load pushes thousands of times:
 *
 *   - room to spare: take the row, and let the caller's sort file it.
 *   - full, and the new row is strictly older than every row held: the new row
 *     IS what the sort-then-drop would discard, so it is simply not taken. It
 *     displaces nobody.
 *   - full, otherwise: find the oldest row HELD (by time, ties resolved to the
 *     earliest arrival, exactly as a stable sort would), drop that one, take
 *     the new row.
 *
 * A tie goes to the ARRIVING row, which is the same answer "append, stable
 * sort, drop [0]" gives: among equal timestamps the stable sort leaves the
 * earlier arrival first, so the earlier arrival is the one dropped.
 *
 * The scan is linear over a 256-entry array and runs only once the tail is
 * full, which on a load is once per row past the first NWT. That is a few
 * thousand comparisons on a log of years -- far cheaper than shifting the
 * whole array on every one of them. */
static void wt_push(struct wt_tail *t, const struct wt_rec *r)
{
   if (t->n < NWT) {
      t->r[t->n++] = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < t->n; i++)
      if (t->r[i].t < t->r[oldest].t)
         oldest = i;
   if (r->t < t->r[oldest].t)
      return; /* the arriving row is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < t->n; i++)
      t->r[i - 1] = t->r[i];
   t->r[t->n - 1] = *r;
}

/* Oldest first. A backdated entry files into place immediately, so the table
 * never shows one row out of order until the next launch. Insertion sort: the
 * log is already sorted but for the row just added. */
static void wt_sort(struct wt_tail *t)
{
   for (int i = 1; i < t->n; i++) {
      struct wt_rec k = t->r[i];
      int j           = i - 1;
      while (j >= 0 && t->r[j].t > k.t) {
         t->r[j + 1] = t->r[j];
         j--;
      }
      t->r[j + 1] = k;
   }
}

/* ONE READER FOR ONE ROW SHAPE. This and wt_parse_line held separate copies
 * of the same two fields and the same two range checks -- so a bound tightened
 * in one was a bound the other still let through, on the same file. */
static int wt_parse_rec(const char *p, const char *e, struct wt_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   /* No `why`: every answer the cursor could give about these two is already
    * a rejection here. An absent or non-numeric field reads 0, and 0 fails
    * `t <= 0` and `g < WT_MIN_G` alike; a run longer than the digit cap is
    * left holding its leading digits, which cannot land inside a plausible
    * weight or a plausible epoch. */
   r->t = csv_num(&c, 0);
   csv_sep(&c);
   r->g = csv_num(&c, 0);
   if (r->t <= 0 || r->t >= WT_T_MAX)
      return 0;
   if (r->g < WT_MIN_G || r->g > WT_MAX_G)
      return 0;
   return 1;
}

/* 1 = a record was taken, 0 = the line was the header (nothing to take),
 * -1 = A ROW WAS REJECTED.
 *
 * The third answer is the one that is easy to leave out. Skip a corrupt row
 * and report success and the app shows a history with a hole in it and says
 * nothing -- and nothing here ever goes back over the file to find it, so the
 * hole is permanent and silent (the edit path replaces one named row and
 * reads nothing else). Skipping is right (a row that does not parse is not a
 * weight), but the CALLER has to know it happened. */
static int wt_parse_line(struct wt_tail *t, const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0; /* the header */
   /* An empty line is not damage: a trailing newline at end of file produces
    * one, and so does an editor. */
   if (p >= e)
      return 0;
   /* Validate on the way IN. This file is loaded at every launch and never
    * rewritten, so a corrupt row admitted once is shown for good. */
   struct wt_rec r = {0, 0};
   if (!wt_parse_rec(p, e, &r))
      return -1;
   wt_push(t, &r);
   return 1;
}

/* Returns 0 when the file was read WHOLE -- including the first-run case where
 * there is nothing to read -- and -1 when what was loaded is INCOMPLETE.
 *
 * The distinction is the point, and "incomplete" is wider than "the read
 * failed". What is lost is not a file, it is RECORD: every weight the user has
 * logged, and the trend drawn from them. All four of these produce a short
 * history, and only the first is one the read itself reports:
 *
 *   - read(2) failed partway (a dying card);
 *   - a row did not parse, or held a value no scale produces;
 *   - a line was longer than any row can be, so it was skipped rather than
 *     parsed as a truncation of itself;
 *   - the file ends mid-line, which means it was cut while being written.
 *
 * Whatever parsed is KEPT -- a prefix of the truth beats an empty screen --
 * and the caller says so rather than presenting a short history as complete. */
/* THE STAGING TAIL, private to the loader (see insulin.c for the reasoning;
 * it is static for the same reason -- this runs on a service thread). */
static struct wt_tail g_stage;

int weight_load(void)
{
   struct wt_tail *t = &g_stage;
   t->n              = 0;
   int fd            = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0) {
      /* A LOG THAT IS NOT THERE IS A PUBLISHED RESULT TOO: the zeroing used
       * to happen to the live tail, so this path emptied it on the way past
       * and a restore-to-empty must still do that. */
      mutex_lock(&wt_lk);
      g_live = *t;
      mutex_unlock(&wt_lk);
      return errno == ENOENT ? 0 : -1;
   }
   /* Stream the whole file a line at a time (the insulin.c pattern): the tail
    * keeps only the last NWT rows, so memory stays bounded however many years
    * the file has grown. */
   char buf[1024];
   char line[96];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0; /* anything that means "what you have is short" */
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            /* Two ways to lose a row -- longer than any row can be, or a
             * row that does not parse -- and one thing to say about both:
             * what you are looking at is short. */
            if (over || wt_parse_line(t, line, line + llen) < 0)
               damaged = 1;
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
      /* A FINAL LINE WITH NO NEWLINE is a file that was cut while being
       * written -- power loss, or a full disk mid-append. Its bytes may parse
       * perfectly and still be half a record, so it is kept if it parses (the
       * prefix rule) and reported either way -- never accepted in silence. */
      /* NOT PARSED. The bytes may make a perfectly valid pair -- and still be
       * half a record, because what says a row is finished is the newline the
       * file does not have. Publishing it puts a weight the user never
       * logged into the record, where only the user noticing it and deleting
       * it by hand would ever take it out again. */
      damaged = 1;
   }
   close(fd);
   wt_sort(t);
   /* PUBLISHED WHOLE: one assignment under the leaf lock, so a
    * reader holds the log from before this call or the one after it. A
    * damaged file still publishes its prefix -- unchanged rule -- and the
    * caller is still told. */
   mutex_lock(&wt_lk);
   g_live = *t;
   mutex_unlock(&wt_lk);
   return (n < 0 || damaged) ? -1 : 0;
}

int weight_append(long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   char b[64];
   int n = snprintf(b, sizeof b, "%ld,%ld,%ld\n", t, g, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION for the whole append, including the header on a new file:
    * see log_append. Done here as open/lseek/write-header/write-row/close it
    * carries the two holes every hand-rolled log has -- a first record that is
    * not atomic, and a failed rollback reported as a clean failure. */
   int rc = log_append(g_wt_path, g_wt_hdr, (int)sizeof g_wt_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct wt_rec r = {t, g};
   /* THE FILE FIRST, THEN THE TAIL UNDER THE LOCK -- the leaf is never held
    * across the append. */
   mutex_lock(&wt_lk);
   wt_push(&g_live, &r);
   wt_sort(&g_live);
   mutex_unlock(&wt_lk);
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* WHAT MAKES A LINE THE ROW BEING EDITED, and what the replacement says.
 * Everything else about the rewrite -- the two passes, the temporary, the
 * durable publish, the tombstone when a delete empties the file -- is
 * log_edit_last's (see util.h), so there is no copy of it here. */
struct wt_edit {
   struct wt_rec orig; /* the row the user is editing */
   long t, g, tz;      /* what it becomes (unused for a delete) */
};

static int wt_edit_matches(const char *line, const char *end, void *ctx)
{
   const struct wt_edit *e = ctx;
   struct wt_rec r;
   /* BOTH FIELDS. A weight log can hold two rows with the same instant (the
    * same second, two entries) and two with the same value; only the pair
    * names one row. */
   return wt_parse_rec(line, end, &r) && r.t == e->orig.t && r.g == e->orig.g;
}

static int wt_edit_format(char *out, int cap, void *ctx)
{
   const struct wt_edit *e = ctx;
   return snprintf(out, (size_t)cap, "%ld,%ld,%ld", e->t, e->g, e->tz);
}

/* APPLY A COMMITTED EDIT TO THE TAIL, without touching the file.
 *
 * Only ever called when the file rewrite SUCCEEDED and the re-read did not,
 * so the edit it applies is the one already on disk. It finds the row by the
 * same pair wt_edit_matches uses -- the instant AND the value, because either
 * alone can repeat -- and then edits or removes it.
 *
 * WHAT IT CANNOT DO, and what that costs: a delete leaves the tail one row
 * short of the NWT it could hold, because the row that should be pulled in to
 * replace it is older than anything in memory and only the file has it. The
 * next successful load fills it. One missing OLD row for one session is a
 * different order of wrong from a table that contradicts the file. */
/* CALLER HOLDS wt_lk. */
static void wt_tail_patch(const struct wt_rec *orig, int del, long t, long g)
{
   for (int i = 0; i < g_live.n; i++) {
      if (g_live.r[i].t != orig->t || g_live.r[i].g != orig->g)
         continue;
      if (del) {
         for (int j = i + 1; j < g_live.n; j++)
            g_live.r[j - 1] = g_live.r[j];
         g_live.n--;
      } else {
         g_live.r[i].t = t;
         g_live.r[i].g = g;
         wt_sort(&g_live); /* the instant may have moved: keep oldest-first */
      }
      return;
   }
   /* Not in the tail at all -- the user edited a row older than the newest
    * NWT, which the table cannot show anyway. Nothing to do. */
}

static int wt_rewrite(const struct wt_rec *orig, int del, long t, long g,
                      long tz)
{
   if (!orig)
      return -1;
   struct wt_edit e = {*orig, t, g, tz};
   struct log_edit ed;
   ed.matches = wt_edit_matches;
   ed.format  = del ? 0 : wt_edit_format;
   ed.ctx     = &e;
   if (log_edit_last(g_wt_path, &ed) != 0)
      return -1;
   /* ---- THE FILE IS RIGHT; NOW MAKE MEMORY SAY SO --------------------
    *
    * `(void)weight_load();` is not enough, whatever a comment about "memory
    * holding the pre-edit rows, which is the honest state" claims:
    * weight_load clears the tail as its FIRST act, so a reload that then
    * fails to open the file leaves the screen showing an EMPTY weight log
    * after an edit that succeeded. In the milder version -- a partial read --
    * the row the user just changed can still show its pre-edit value, with
    * the edit reported committed. Either way the caller is told the edit
    * landed and the tail disagrees with the file.
    *
    * The rewrite is already validated (log_edit_last matched exactly one row
    * and formatted its replacement, or removed it), so THE TAIL CAN BE
    * PUBLISHED FROM IT rather than re-read: keep a copy, try the reload, and
    * if the reload does not produce a tail, restore the copy and apply the
    * very edit that just went to disk. Memory then agrees with the file by
    * construction rather than by a second I/O that may not happen. */
   struct wt_tail save;
   mutex_lock(&wt_lk);
   save = g_live;
   mutex_unlock(&wt_lk);
#ifdef APP_FAULTS
   /* THE ONE MOMENT THIS FAILURE CAN BE ARRANGED, and the only way a test can
    * reach it: between a rewrite that succeeded and the re-read that must
    * follow it. Never compiled into the app -- nothing that ships defines
    * APP_FAULTS -- and weighttest uses it to make the log unreadable exactly
    * here (test/app/weighttest.c). */
   if (weight_fault_before_reload)
      weight_fault_before_reload();
#endif
   if (weight_load() != 0) {
      mutex_lock(&wt_lk);
      g_live = save;
      wt_tail_patch(orig, del, t, g);
      mutex_unlock(&wt_lk);
      LOGW("weight: the log was rewritten but could not be re-read; the "
           "table was updated from the edit itself");
   }
   return 0;
}

int weight_update(const struct wt_rec *orig, long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   return wt_rewrite(orig, 0, t, g, tz);
}

int weight_delete(const struct wt_rec *orig)
{
   return wt_rewrite(orig, 1, 0, 0, 0);
}

/* ---- the weight-entry form (see weight.h) ---- */

/* What a form with NO history offers. A first weigh-in has nothing to start
 * from, and an empty field is worse than a plausible one: 70 kg is a number
 * the user edits, whereas 0 is one they must type from scratch. */
#define WT_FORM_DEFAULT_G 70000L

void wt_form_open(struct wt_form *f, long last_g, int units, long now)
{
   if (!f)
      return;
   f->t      = now;
   f->tenths = wt_to_tenths(last_g > 0 ? last_g : WT_FORM_DEFAULT_G, units);
   f->edit   = -1;
}

/* The weight log's filename. */
int weight_paths(const char *dir)
{
   int ok = 1;
   if (!(data_path(g_wt_path, sizeof g_wt_path, dir, "/weight.csv")))
      ok = 0;
   return ok;
}

const char *weight_path(void)
{
   return g_wt_path;
}
