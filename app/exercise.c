// SPDX-License-Identifier: GPL-3.0
// exercise.c --- Exercise intensity log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

#include "exercise.h"

#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "thread.h" /* the pending value is touched from two threads */
#include "dexlibc.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf */

static struct ex_rec g_ex[NEX];
static int g_nex;
static char g_ex_path[256];
static const char g_ex_hdr[] = "# unix_time,level,tz_offset_s\n";

int ex_count(void)
{
   return g_nex;
}

struct ex_rec ex_at(int i)
{
   struct ex_rec z = {0, 0};
   if (i < 0 || i >= g_nex)
      return z;
   return g_ex[i];
}

struct ex_rec ex_newest(void)
{
   struct ex_rec z = {0, 0};
   return g_nex > 0 ? g_ex[g_nex - 1] : z;
}

int ex_copy(struct ex_rec *out, int cap)
{
   int n = g_nex < cap ? g_nex : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_ex[i];
   return n;
}

/* The newest NEX rows BY TIME -- weight.c's wt_push, and the long argument for
 * why "newest by time" rather than "last seen" is there. An import of older
 * rows must not evict today's. */
static void ex_push(const struct ex_rec *r)
{
   if (g_nex < NEX) {
      g_ex[g_nex++] = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < g_nex; i++)
      if (g_ex[i].t < g_ex[oldest].t)
         oldest = i;
   if (r->t < g_ex[oldest].t)
      return; /* the arriving row is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < g_nex; i++)
      g_ex[i - 1] = g_ex[i];
   g_ex[g_nex - 1] = *r;
}

/* Oldest first; insertion sort, because the log is already sorted but for the
 * row just added. */
static void ex_sort(void)
{
   for (int i = 1; i < g_nex; i++) {
      struct ex_rec k = g_ex[i];
      int j           = i - 1;
      while (j >= 0 && g_ex[j].t > k.t) {
         g_ex[j + 1] = g_ex[j];
         j--;
      }
      g_ex[j + 1] = k;
   }
}

/* ONE READER FOR ONE ROW SHAPE, shared by the loader and by anything that
 * needs to know whether a row is a row. */
static int ex_parse_rec(const char *p, const char *e, struct ex_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   /* NO `why` ON EITHER FIELD, and the level is worth spelling out because
    * the obvious reading says it needs one.
    *
    * csvcur.h's typed answer exists because an empty field reading 0 is
    * indistinguishable from a written 0 -- which matters wherever 0 is a
    * legitimate value (sensors.c's `activation`, where it means "session start
    * unknown"). Here it is not one: the domain is 1..3, so the range check
    * below rejects a 0 whatever produced it, and an over-long digit run keeps
    * only its leading digits, which cannot land in 1..3 either. Asking would
    * add a branch that no input can reach.
    *
    * Measured, not assumed: a mutant deleting a `why != CSV_FIELD_OK` guard
    * here survived the whole suite, because every input it would have caught
    * is already caught one line further down. The instant is the same story
    * against `t <= 0`. */
   r->t = csv_num(&c, 0);
   csv_sep(&c);
   long lv = csv_num(&c, 0);
   if (r->t <= 0 || r->t >= EX_T_MAX)
      return 0;
   if (lv < EX_MIN_LEVEL || lv > EX_MAX_LEVEL)
      return 0;
   r->level = (int)lv;
   return 1;
}

/* 1 = a record was taken, 0 = nothing to take (header or blank line),
 * -1 = A ROW WAS REJECTED, which the caller has to know about because this
 * file is never rewritten and a silent hole is permanent. */
static int ex_parse_line(const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0; /* the header */
   if (p >= e)
      return 0; /* a trailing newline, or an editor's blank line */
   struct ex_rec r = {0, 0};
   if (!ex_parse_rec(p, e, &r))
      return -1;
   ex_push(&r);
   return 1;
}

/* 0 when the file was read WHOLE (including the first-run case where there is
 * nothing to read), -1 when what was loaded is INCOMPLETE. weight_load carries
 * the full argument for why those are different answers and why a prefix is
 * kept rather than discarded. */
int exercise_load(void)
{
   g_nex  = 0;
   int fd = open(g_ex_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char buf[1024];
   char line[96];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0;
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (over || ex_parse_line(line, line + llen) < 0)
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
      /* A FINAL LINE WITH NO NEWLINE is a file cut while being written. Its
       * bytes may parse perfectly and still be half a record, because what
       * says a row is finished is the newline the file does not have. Not
       * parsed, and reported. */
      damaged = 1;
   }
   close(fd);
   ex_sort();
   return (n < 0 || damaged) ? -1 : 0;
}

int exercise_append(long t, int level, long tz)
{
   if (t <= 0 || t >= EX_T_MAX)
      return -1;
   /* THE RESTING POSITION IS REFUSED HERE TOO, not only by the settling rule.
    * ex_tick is what should keep a 0 from ever reaching this function, but a
    * bound that exists only in the caller is a bound that holds until the next
    * caller. A 0 in this file would mean "exercise of intensity nothing",
    * which is not a thing anybody logged. */
   if (level < EX_MIN_LEVEL || level > EX_MAX_LEVEL)
      return -1;
   char b[64];
   int n = snprintf(b, sizeof b, "%ld,%d,%ld\n", t, level, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION for the whole append, including the header on a new file:
    * see log_append. */
   int rc = log_append(g_ex_path, g_ex_hdr, (int)sizeof g_ex_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct ex_rec r = {t, level};
   ex_push(&r);
   ex_sort();
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* ---------------- the settling rule ---------------- */

void ex_press(struct ex_pending *p, long mono_now)
{
   p->level = (p->level + 1) % (EX_MAX_LEVEL + 1);
   /* THE CLOCK RESTARTS ON EVERY PRESS, which is what makes cycling past a
    * value free: the countdown measures how long the CURRENT value has stood,
    * not how long the button has been touched. Without the restart, pressing
    * four times in a second would commit whatever the value happened to be
    * when the first press's minute ran out. */
   p->since = mono_now;
   /* Back at rest: nothing is pending, and nothing that was pending should be
    * written. Cycling all the way round is how a user CANCELS. */
   p->armed = p->level != 0;
}

enum ex_verdict ex_tick(const struct ex_pending *p, long mono_now)
{
   if (!p->armed || p->level == 0)
      return EX_IDLE;
   /* `>=`, so the value commits AT the settling mark rather than one tick
    * after it -- the same inclusive-bound rule the alarm and its banner
    * share, and for the same reason: two places that describe one threshold
    * must not disagree about the instant it is reached.
    *
    * A BACKWARD monotonic step cannot happen by definition, but a caller that
    * passes wall time by mistake would make `mono_now - since` negative and
    * park the value forever. Negative is treated as "no time has passed",
    * which fails safe (it delays a write) rather than committing early. */
   long el = mono_now - p->since;
   if (el < 0)
      return EX_HOLD;
   return el >= EX_SETTLE_S ? EX_COMMIT : EX_HOLD;
}

void ex_committed(struct ex_pending *p)
{
   /* THE LEVEL STAYS ON THE BUTTON; only the arming is cleared. What was
    * committed is still what the user chose, and the button showing it is how
    * they know it took. Clearing the level here would blank the button the
    * moment the record was written, which reads as the press being lost. */
   p->armed = 0;
}

int ex_remaining(const struct ex_pending *p, long mono_now)
{
   if (!p->armed || p->level == 0)
      return 0;
   long el = mono_now - p->since;
   if (el < 0)
      el = 0;
   long r = EX_SETTLE_S - el;
   if (r < 0)
      r = 0;
   return (int)r;
}

/* ---- THE LIVE BUTTON ----
 *
 * ONE pending value for the process, behind a leaf lock. See exercise.h for
 * why it is here and not in forms.c: the commit happens on a timer, and the
 * timer that matters most is the SERVICE's, which runs on its own thread so
 * that it keeps running when the activity is gone.
 *
 * `ex_lk` is a leaf in every sense the lock order cares about -- taken
 * innermost, never held across a call into another module. The one call made
 * under it would be exercise_append, which writes a file, so it is
 * deliberately made OUTSIDE: the decision is taken under the lock, the write
 * happens without it, and the bookkeeping is done under it again. */
static struct mutex ex_lk = MUTEX_INIT;
static struct ex_pending g_btn;

void exercise_button_press(long mono_now)
{
   mutex_lock(&ex_lk);
   ex_press(&g_btn, mono_now);
   mutex_unlock(&ex_lk);
}

int exercise_button_tick(long now, long mono_now, long tz)
{
   mutex_lock(&ex_lk);
   enum ex_verdict v = ex_tick(&g_btn, mono_now);
   int level         = g_btn.level;
   mutex_unlock(&ex_lk);
   if (v != EX_COMMIT)
      return 0;
   /* WRITTEN OUTSIDE THE LOCK. exercise_append opens, writes and flushes a
    * file; holding a lock across that would put a leaf above the append lock
    * inside util.c and make this the one place in the app where a UI tap can
    * block on an fsync. */
   if (exercise_append(now, level, tz) != 0)
      return 0; /* left armed: the next tick tries again */
   mutex_lock(&ex_lk);
   /* RE-CHECKED, because the lock was not held across the write. A press
    * during it changed the level, restarted the clock, and re-armed -- so the
    * value that was just written is no longer the value waiting to be
    * written, and clearing the arming here would silently discard the newer
    * one. Only disarm when what settled is still what is showing. */
   if (g_btn.level == level)
      ex_committed(&g_btn);
   mutex_unlock(&ex_lk);
   return 1;
}

void exercise_button_get(long mono_now, int *level, int *remaining)
{
   mutex_lock(&ex_lk);
   /* BOTH UNDER ONE LOCK, so the number drawn and the bar drawn beside it
    * describe the same instant. Two calls would let a press land between them
    * and draw a full bar under the previous level. */
   if (level)
      *level = g_btn.level;
   if (remaining)
      *remaining = ex_remaining(&g_btn, mono_now);
   mutex_unlock(&ex_lk);
}

/* The exercise log's filename. */
int exercise_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_ex_path, sizeof g_ex_path, dir, "/exercise.csv");
   return ok;
}

const char *exercise_path(void)
{
   return g_ex_path;
}
