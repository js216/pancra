// SPDX-License-Identifier: GPL-3.0
// steps.c --- the step log and its sampler (see steps.h)
// Copyright 2026 Jakob Kastelic

#include "steps.h"

#include "csvcur.h" /* the shared CSV cursor */
#include "dexlibc.h"
#include "thread.h" /* the tail is read by the renderer, written by a tick */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf */

static char g_steps_path[256];
static const char g_steps_hdr[] = "# unix_time,steps,tz_offset_s\n";

/* THE TAIL IS A VALUE, AND IT IS PUBLISHED -- the same object and the same
 * reason as insulin.c's and weight.c's: a restore rewrites the file and
 * reloads it on the SYNC WORKER while the main thread draws the plot out of
 * the tail. So the tail is one object behind a leaf lock and a load publishes
 * a separately-built one with a single assignment. */
struct step_tail {
   struct step_rec r[NSTEPS];
   int n;
};

static struct step_tail g_live;
static struct step_tail g_stage;
static struct mutex steptail_lk = MUTEX_INIT;

/* Append to a tail, dropping the OLDEST when it is full. Steps are a rolling
 * picture rather than a permanent record the app must hold entire -- the file
 * keeps everything, and the plot's longest span is thirty days. */
static void step_push(struct step_tail *t, const struct step_rec *r)
{
   if (t->n < NSTEPS) {
      t->r[t->n++] = *r;
      return;
   }
   for (int i = 1; i < NSTEPS; i++)
      t->r[i - 1] = t->r[i];
   t->r[NSTEPS - 1] = *r;
}

int steps_count(void)
{
   mutex_lock(&steptail_lk);
   const int n = g_live.n;
   mutex_unlock(&steptail_lk);
   return n;
}

struct step_rec steps_at(int i)
{
   struct step_rec r = {0, 0};
   mutex_lock(&steptail_lk);
   if (i >= 0 && i < g_live.n)
      r = g_live.r[i];
   mutex_unlock(&steptail_lk);
   return r;
}

int steps_copy(struct step_rec *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&steptail_lk);
   n = g_live.n < cap ? g_live.n : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_live.r[i];
   mutex_unlock(&steptail_lk);
   return n;
}

const char *steps_path(void)
{
   return g_steps_path;
}

int steps_paths(const char *dir)
{
   return data_path(g_steps_path, sizeof g_steps_path, dir, "/steps.csv") ? 1
                                                                          : 0;
}

/* One row, or 0 when it is not one. The grammar is the file's: three fields,
 * each a number, with the instant and the count inside their domains. A row
 * this program did not write is refused rather than clamped. */
static int step_parse(const char *p, const char *e, struct step_rec *out)
{
   struct csv_cur c;
   enum csv_field why = CSV_FIELD_OK;
   csv_open(&c, p, e);
   const long t = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   const long n = csv_num(&c, &why);
   if (why != CSV_FIELD_OK)
      return 0;
   if (t <= 0 || t >= STEP_T_MAX)
      return 0;
   /* ZERO IS REFUSED, not stored: steps_append never writes one, so a row
    * carrying it did not come from here. */
   if (n < 1 || n > STEP_MAX)
      return 0;
   out->t = t;
   out->n = (int)n;
   return 1;
}

int steps_load(void)
{
   struct step_tail *t = &g_stage;
   t->n                = 0;
   const int fd        = open(g_steps_path, O_RDONLY, 0);
   if (fd < 0) {
      /* A log that is not there is a published result too: see weight.c. */
      mutex_lock(&steptail_lk);
      g_live = *t;
      mutex_unlock(&steptail_lk);
      return errno == ENOENT ? 0 : -1;
   }
   char buf[1024];
   char line[64];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0;
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            struct step_rec r = {0, 0};
            if (over)
               damaged = 1;
            else if (llen > 0 && line[0] != '#') {
               if (step_parse(line, line + llen, &r))
                  step_push(t, &r);
               else
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
      /* A FINAL LINE WITH NO NEWLINE is a file cut while being written. Its
       * bytes may parse perfectly and still be half a record, because what
       * says a row is finished is the newline the file does not have. */
      damaged = 1;
   }
   close(fd);
   /* PUBLISHED WHOLE. */
   mutex_lock(&steptail_lk);
   g_live = *t;
   mutex_unlock(&steptail_lk);
   return (n < 0 || damaged) ? -1 : 0;
}

int steps_append(long t, int n, long tz)
{
   if (t <= 0 || t >= STEP_T_MAX)
      return -1;
   if (n < 1 || n > STEP_MAX)
      return -1;
   char b[64];
   int len = snprintf(b, sizeof b, "%ld,%d,%ld\n", t, n, tz);
   len     = clampn(len, sizeof b);
   /* ONE OPERATION for the whole append, including the header on a new file:
    * see log_append. */
   const int rc =
       log_append(g_steps_path, g_steps_hdr, (int)sizeof g_steps_hdr - 1, b,
                  len);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct step_rec r = {t, n};
   /* THE FILE FIRST, THE TAIL UNDER THE LOCK. */
   mutex_lock(&steptail_lk);
   step_push(&g_live, &r);
   mutex_unlock(&steptail_lk);
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* ---- THE SAMPLER ----
 *
 * Two numbers carried between calls: where the current window ends, and what
 * the hardware counter read when it began. Both are in-memory only. A restart
 * therefore drops the window in progress rather than inventing one, which is
 * the same trade the reboot case makes and for the same reason: the counter
 * this arithmetic is a difference OF does not survive either. */
static long g_win_end;  /* the instant the open window closes; 0 = none yet */
static long g_win_base; /* the counter's value when it opened; -1 = unknown */
static int g_have_cum;  /* has the sensor answered at all this run? */

int steps_live(void)
{
   return g_have_cum;
}

void steps_tick(long now, long tz, long cum)
{
   if (cum < 0)
      return; /* the sensor has not answered yet: nothing to difference */
   g_have_cum = 1;
   if (g_win_end <= 0 || now < g_win_end - STEP_BUCKET_S) {
      /* FIRST CALL, or a clock that moved backwards far enough that the open
       * window is in the future. Start a fresh one rather than closing the
       * old against a `now` that has nothing to do with it. */
      g_win_end  = now + STEP_BUCKET_S;
      g_win_base = cum;
      return;
   }
   if (now < g_win_end)
      return; /* the window is still open: this is the no-op case */

   const long delta = cum - g_win_base;
   /* DID WE ACTUALLY WATCH THIS WINDOW? A tick that arrives within one window
    * of the boundary did; one that arrives much later did not -- the phone
    * was asleep, or the process frozen, and the counter has been running the
    * whole time. */
   const int watched = now - g_win_end < STEP_BUCKET_S;
   /* A NEGATIVE DELTA IS A REBOOT -- the counter restarts at zero -- and one
    * over the ceiling is a counter that is not ours to interpret. Either way
    * the window is dropped and the next one starts from what was just read,
    * so a single reboot costs five minutes rather than corrupting the log. */
   if (delta > 0 && delta <= STEP_MAX)
      /* AND AN UNWATCHED DELTA IS STAMPED `now`, NOT THE STALE BOUNDARY.
       * Those steps happened somewhere between the boundary and this tick --
       * an hour of Doze, say -- and we cannot say where. Filing them at a
       * boundary that may be hours back would state a precise past window
       * they were not observed in; filing them at the instant they were first
       * seen is the one honest end of that interval. */
      (void)steps_append(watched ? g_win_end : now, (int)delta, tz);
   /* THE NEXT WINDOW STARTS WHERE THIS ONE ENDED, not at `now`.
    *
    * Chaining off the boundary keeps the samples on a fixed five-minute grid
    * however late a tick happens to run -- and the grid is what lets a row's
    * instant be compared with a CGM reading's. Falling far behind would
    * otherwise take an hour of ticks to catch up, so an unwatched gap
    * re-bases the grid on `now` instead. */
   g_win_end  = watched ? g_win_end + STEP_BUCKET_S : now + STEP_BUCKET_S;
   g_win_base = cum;
}
