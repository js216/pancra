// SPDX-License-Identifier: GPL-3.0
// exercise.c --- Exercise intensity log: an editable CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

#include "exercise.h"

#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "log.h"    /* LOGW: an edit that landed but could not be re-read */
#include "thread.h" /* the pending value is touched from two threads */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf */

/* ---- THE TAIL IS A VALUE, AND IT IS PUBLISHED ---------------
 *
 * The same object and the same reason as insulin.c's and weight.c's: a
 * RESTORE rewrites exercise.csv and reloads it, and once that reload runs
 * it runs on the SYNC WORKER while the main thread draws
 * the log out of the tail. So the tail is one object behind a leaf lock and
 * a load publishes a separately-built one with a single assignment. */
struct ex_tail {
   struct ex_rec r[NEX];
   int n;
};

static struct ex_tail g_live;

/* A LEAF, and NOT ex_lk below: that one guards the BUTTON's pending state
 * and is held across a decision, while this one is taken around the tail
 * only. Two locks in one file, each taken alone, is what keeps both leaves:
 * nothing here ever holds one and takes the other. */
static struct mutex extail_lk = MUTEX_INIT;

static char g_ex_path[256];
static const char g_ex_hdr[] = "# unix_time,level,tz_offset_s,duration_s\n";

int ex_count(void)
{
   mutex_lock(&extail_lk);
   int n = g_live.n;
   mutex_unlock(&extail_lk);
   return n;
}

struct ex_rec ex_at(int i)
{
   struct ex_rec z = {0, 0, 0, 0};
   mutex_lock(&extail_lk);
   if (i >= 0 && i < g_live.n)
      z = g_live.r[i];
   mutex_unlock(&extail_lk);
   return z;
}

int ex_copy(struct ex_rec *out, int cap)
{
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&extail_lk);
   int n = g_live.n < cap ? g_live.n : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_live.r[i];
   mutex_unlock(&extail_lk);
   return n;
}

/* The newest NEX rows BY TIME -- weight.c's wt_push, and the long argument for
 * why "newest by time" rather than "last seen" is there. An import of older
 * rows must not evict today's. */
static void ex_push(struct ex_tail *t, const struct ex_rec *r)
{
   if (t->n < NEX) {
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

/* Oldest first; insertion sort, because the log is already sorted but for the
 * row just added. */
static void ex_sort(struct ex_tail *t)
{
   for (int i = 1; i < t->n; i++) {
      struct ex_rec k = t->r[i];
      int j           = i - 1;
      while (j >= 0 && t->r[j].t > k.t) {
         t->r[j + 1] = t->r[j];
         j--;
      }
      t->r[j + 1] = k;
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
   /* THE THIRD AND FOURTH FIELDS MAY NOT BE THERE, and both cases are
    * ordinary rather than damage. csv_sep answers 0 when the row simply ends,
    * and a row written before the duration column existed ends after the
    * offset -- so an absent field reads as 0, which is exactly what "no end
    * was ever recorded" means. Asking csv_num why it read 0 would not
    * distinguish an empty field from a written 0 here either, and both are
    * the same answer for this column. */
   csv_sep(&c);
   r->tz = csv_num(&c, 0);
   csv_sep(&c);
   r->dur = csv_num(&c, 0);
   if (r->t <= 0 || r->t >= EX_T_MAX)
      return 0;
   if (lv < EX_MIN_LEVEL || lv > EX_MAX_LEVEL)
      return 0;
   /* A NEGATIVE OR ABSURD DURATION IS A CORRUPT ROW, not a long workout: it
    * would draw a line backwards across the plot or across every plot the app
    * has. Rejected here, where every other field's domain is checked. */
   if (r->dur < 0 || r->dur > EX_DUR_MAX)
      return 0;
   r->level = (int)lv;
   return 1;
}

/* 1 = a record was taken, 0 = nothing to take (header or blank line),
 * -1 = A ROW WAS REJECTED, which the caller has to know about: nothing here
 * revisits the file looking for what it skipped, so a silent hole stays. */
static int ex_parse_line(struct ex_tail *t, const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0; /* the header */
   if (p >= e)
      return 0; /* a trailing newline, or an editor's blank line */
   struct ex_rec r = {0, 0, 0, 0};
   if (!ex_parse_rec(p, e, &r))
      return -1;
   ex_push(t, &r);
   return 1;
}

/* 0 when the file was read WHOLE (including the first-run case where there is
 * nothing to read), -1 when what was loaded is INCOMPLETE. weight_load carries
 * the full argument for why those are different answers and why a prefix is
 * kept rather than discarded. */
/* THE STAGING TAIL, private to the loader (see insulin.c). */
static struct ex_tail g_stage;

int exercise_load(void)
{
   struct ex_tail *t = &g_stage;
   t->n              = 0;
   int fd            = open(g_ex_path, O_RDONLY, 0);
   if (fd < 0) {
      /* A log that is not there is a published result too: see weight.c. */
      mutex_lock(&extail_lk);
      g_live = *t;
      mutex_unlock(&extail_lk);
      return errno == ENOENT ? 0 : -1;
   }
   char buf[1024];
   char line[96];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0;
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (over || ex_parse_line(t, line, line + llen) < 0)
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
   ex_sort(t);
   /* PUBLISHED WHOLE. */
   mutex_lock(&extail_lk);
   g_live = *t;
   mutex_unlock(&extail_lk);
   /* THE BUTTON IS A VIEW OF WHAT WAS JUST PUBLISHED. At launch that is what
    * relights a session the process was killed in the middle of -- the row is
    * open, so a session IS running, and without this the only control that
    * could close it sat at rest. A cloud restore lands here too, replacing
    * the whole file under a button that was describing the old one. */
   exercise_button_sync();
   return (n < 0 || damaged) ? -1 : 0;
}

/* ---- THE ONE RUNNING SESSION, AND WHAT MAY SHARE TIME WITH IT --------
 *
 * The LOG is the truth and the button is a view of it. Everything below
 * answers one of two questions about the file -- which row is running, and
 * whether a proposed row could coexist with the others -- so that the writers
 * and the button all read the same answer instead of each keeping an opinion.
 */

/* Same row? The key exercise_update itself matches on (ex_edit_matches): an
 * instant and an intensity, because two sessions do not begin in the same
 * second at the same level. */
static int ex_same(const struct ex_rec *a, const struct ex_rec *b)
{
   return a && b && a->t == b->t && a->level == b->level;
}

/* THE SPAN A ROW OCCUPIES, half-open [from, to).
 *
 * A closed row spans its own length. The RUNNING row spans from its start to
 * now, because that is how much of it has happened. A row that is open and is
 * NOT the running one never got an end recorded (struct ex_rec) and its
 * extent is simply unknown -- so it spans nothing, and an unknown length
 * cannot be said to clash with anything. Inventing one here would reject
 * edits on the strength of a guess, and the oldest rows in a log written
 * before the duration column existed all read that way. */
static void ex_span(const struct ex_rec *r, int running, long now, long *from,
                    long *to)
{
   *from = r->t;
   if (r->dur > 0)
      *to = r->t + r->dur;
   else if (running)
      *to = now > r->t ? now : r->t;
   else
      *to = r->t;
}

/* Do two half-open spans share an instant? A zero-length span occupies none,
 * so it clashes with nothing. */
static int ex_spans_clash(long a0, long a1, long b0, long b1)
{
   if (a1 <= a0 || b1 <= b0)
      return 0;
   return a0 < b1 && b0 < a1;
}

int exercise_active(struct ex_rec *out)
{
   /* ONLY THE NEWEST ROW CAN BE RUNNING, which is the same rule exercise_end
    * closes by -- one place to change if it ever stops being true. */
   const int n = ex_count();
   if (n <= 0)
      return 0;
   struct ex_rec last = ex_at(n - 1);
   if (last.t <= 0 || last.dur != 0)
      return 0;
   if (out)
      *out = last;
   return 1;
}

int exercise_row_running(const struct ex_rec *row)
{
   struct ex_rec act;
   return row && exercise_active(&act) && ex_same(&act, row);
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
   /* NOT WHILE ONE IS ALREADY RUNNING. A press on a lit button ENDS the
    * session rather than starting another, so the settling rule cannot reach
    * here in that state -- but two open rows is the one shape this log must
    * never hold, and a bound that lives only in the caller is a bound that
    * holds until the next caller. */
   if (exercise_active(0))
      return -1;
   char b[64];
   /* WRITTEN OPEN. The duration is 0 because at the moment a level settles
    * nothing knows how long the exercise will last; exercise_end fills it in
    * when the user says they have stopped. */
   int n = snprintf(b, sizeof b, "%ld,%d,%ld,0\n", t, level, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION for the whole append, including the header on a new file:
    * see log_append. */
   int rc = log_append(g_ex_path, g_ex_hdr, (int)sizeof g_ex_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct ex_rec r = {t, level, 0, tz};
   /* THE FILE FIRST, THE TAIL UNDER THE LOCK. */
   mutex_lock(&extail_lk);
   ex_push(&g_live, &r);
   ex_sort(&g_live);
   mutex_unlock(&extail_lk);
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* ---- EDITING AN ENTRY: rewrite-and-rename, exactly as the food log does ----
 *
 * THE MATCH KEY IS THE CONTENT, not a row number.
 *
 * The tail this is called with is a snapshot the renderer took some frames
 * ago, and an append can have landed since; an index into the file would then
 * name a different row than the one the user tapped. Matching on (instant,
 * level) names the row itself. Two rows CAN be identical -- the same intensity
 * settled twice in the same second is absurd but not impossible -- so the LAST
 * such row is the one edited, which is the one a person looking at a
 * newest-first table would mean.
 *
 * `del` = 1 removes the row rather than rewriting it. One function for both
 * because the two passes, the tombstone rule and the reload are identical and
 * the only difference is whether the matched line is copied.
 *
 * A NOTE ON WHY THIS FILE IS REWRITTEN AT ALL. Everything the SETTLING RULE
 * writes only ever ADDS -- that is the ordinary tier this log belongs to
 * (exercise.h). Editing is the user correcting a record after the fact, which
 * is a different act with a different guarantee -- the same one the weight and
 * food logs make -- and it goes through a temporary file and a rename so a
 * failure leaves the original whole. The insulin log is the other tier
 * entirely: it is never rewritten, and a correction there is a new assertion
 * about an existing dose. */
/* WHAT MAKES A LINE THE ROW BEING EDITED, and what the replacement says. The
 * rewrite itself is log_edit_last's (util.h): two passes, a temporary, a
 * durable publish, and the tombstone when a delete empties the file. This
 * A per-file copy of all of that is what this avoids. */
struct ex_edit {
   struct ex_rec orig; /* the row the user is editing */
   long t;             /* what it becomes (unused for a delete) */
   int level;
   long tz, dur;
};

static int ex_edit_matches(const char *line, const char *end, void *ctx)
{
   const struct ex_edit *e = ctx;
   struct ex_rec r;
   /* THE INSTANT AND THE INTENSITY. A session is one row and two sessions do
    * not begin in the same second at the same level. */
   return ex_parse_rec(line, end, &r) && r.t == e->orig.t &&
          r.level == e->orig.level;
}

static int ex_edit_format(char *out, int cap, void *ctx)
{
   const struct ex_edit *e = ctx;
   /* THE DURATION IS WRITTEN BACK, not dropped: a rewrite that omitted the
    * column would erase the line the plot draws. */
   return snprintf(out, (size_t)cap, "%ld,%d,%ld,%ld", e->t, e->level, e->tz,
                   e->dur);
}

/* APPLY A COMMITTED EDIT TO THE TAIL, without touching the file.
 *
 * The same reasoning as weight.c's wt_tail_patch: only ever
 * called when the file rewrite SUCCEEDED and the re-read did not, so the edit
 * it applies is the one already on disk. It finds the row by the same key the
 * matcher above uses, then edits or removes it.
 *
 * A DELETE LEAVES THE TAIL ONE ROW SHORT of what it could hold: the row that
 * should be pulled in is older than anything in memory and only the file has
 * it. The next successful load fills it. One missing old row for one session
 * is a different order of wrong from a table that contradicts the file. */
/* CALLER HOLDS extail_lk. */
static void ex_tail_patch(const struct ex_rec *orig, int del, long t, int level,
                          long tz, long dur)
{
   for (int i = 0; i < g_live.n; i++) {
      if (g_live.r[i].t != orig->t || g_live.r[i].level != orig->level)
         continue;
      if (del) {
         for (int j = i + 1; j < g_live.n; j++)
            g_live.r[j - 1] = g_live.r[j];
         g_live.n--;
      } else {
         g_live.r[i].t     = t;
         g_live.r[i].level = level;
         g_live.r[i].tz    = tz;
         g_live.r[i].dur   = dur;
         ex_sort(&g_live); /* the instant may have moved: oldest-first */
      }
      return;
   }
   /* Older than the newest NEX rows: not in the tail, nothing to do. */
}

static int ex_rewrite(const struct ex_rec *orig, int del, long t, int level,
                      long tz, long dur)
{
   if (!orig)
      return -1;
   struct ex_edit e = {*orig, t, level, tz, dur};
   struct log_edit ed;
   ed.matches = ex_edit_matches;
   ed.format  = del ? 0 : ex_edit_format;
   ed.ctx     = &e;
   if (log_edit_last(g_ex_path, &ed) != 0)
      return -1;
   /* THE FILE IS RIGHT; NOW MAKE MEMORY SAY SO. See fd_rewrite and
    * wt_rewrite: exercise_load clears the tail first, so a failed reload
    * showed an empty log after a successful edit. It matters most here --
    * closing a session is a rewrite, so an unnoticed stale tail leaves a
    * session drawn as still running. */
   struct ex_tail save;
   mutex_lock(&extail_lk);
   save = g_live;
   mutex_unlock(&extail_lk);
   if (exercise_load() != 0) {
      mutex_lock(&extail_lk);
      g_live = save;
      ex_tail_patch(orig, del, t, level, tz, dur);
      mutex_unlock(&extail_lk);
      LOGW("exercise: the log was rewritten but could not be re-read; the "
           "table was updated from the edit itself");
   }
   return 0;
}

enum ex_update_result exercise_update(const struct ex_rec *orig, long t,
                                      int level, long dur, long tz, long now)
{
   if (!orig || t <= 0 || t >= EX_T_MAX)
      return EX_UPD_RANGE;
   /* The same bound exercise_append enforces, for the same reason: 0 is the
    * button's resting position, not an intensity anybody logged. */
   if (level < EX_MIN_LEVEL || level > EX_MAX_LEVEL)
      return EX_UPD_RANGE;
   /* THE DURATION IS THE CALLER'S, and 0 is a value rather than a refusal: it
    * is the column's own "not known", which is what an open session reads as.
    * The ceiling is exercise_end's, so a row cannot be edited into a length
    * the log would not have recorded in the first place. */
   if (dur < 0 || dur > EX_DUR_MAX)
      return EX_UPD_RANGE;

   struct ex_rec act;
   const int have_act = exercise_active(&act);
   const int is_act   = have_act && ex_same(&act, orig);

   /* NO SESSION HAPPENS IN THE FUTURE, at either end. A start after now is
    * not a thing that has occurred, and a finished session whose end is still
    * to come is the same claim wearing a length.
    *
    * This is also what keeps the LIVE button safe without a rule of its own:
    * a new session always begins at now, so as long as nothing in the log
    * reaches past now, nothing the button starts can land inside an existing
    * row. The no-overlap rule below would otherwise be enforced on edits and
    * quietly broken by the next press. */
   if (t > now)
      return EX_UPD_FUTURE;
   if (dur > 0 && t + dur > now)
      return EX_UPD_FUTURE;
   /* REOPENING A ROW IS NOT AN EDIT. dur == 0 means "still running", and the
    * running session is the newest row and only ever that one -- so clearing
    * the length of any other row would put a second open session in the log
    * with nothing to say which of them the button is holding. (The form does
    * not offer the field on a running row at all; this is the writer refusing
    * it rather than trusting that it never will.) */
   if (dur == 0 && !is_act)
      return EX_UPD_REOPEN;

   /* AND NO TWO SESSIONS MAY SHARE AN INSTANT. Overlapping rows are not a
    * display problem: the plot draws each as a band, the model reads them as
    * concurrent effort, and "when was I exercising" stops having one answer.
    * An edit is the only way to produce one -- live sessions cannot overlap,
    * because each begins at a now that has already moved past the last -- so
    * this is where it is caught. */
   long a0 = 0;
   long a1 = 0;
   {
      const struct ex_rec want = {t, level, dur, tz};
      ex_span(&want, is_act, now, &a0, &a1);
   }
   const int n = ex_count();
   for (int i = 0; i < n; i++) {
      struct ex_rec r = ex_at(i);
      if (ex_same(&r, orig))
         continue; /* the row being changed is not its own neighbour */
      long b0 = 0;
      long b1 = 0;
      ex_span(&r, have_act && ex_same(&r, &act), now, &b0, &b1);
      if (ex_spans_clash(a0, a1, b0, b1))
         return EX_UPD_OVERLAP;
   }

   if (ex_rewrite(orig, 0, t, level, tz, dur) != 0)
      return EX_UPD_FAILED;
   /* THE EDIT MAY HAVE CHANGED WHAT IS RUNNING -- its level, or whether it is
    * still the newest open row at all -- so the button is made to agree
    * before anybody draws it. */
   exercise_button_sync();
   return EX_UPD_OK;
}

int exercise_delete(const struct ex_rec *orig)
{
   if (!orig)
      return -1;
   const int r = ex_rewrite(orig, 1, 0, 0, 0, 0);
   /* THE BUTTON AND THE OPEN ROW ARE ONE FACT, so deleting the row puts the
    * button down.
    *
    * Left lit, it claims a session the log no longer has -- and that is not
    * merely a wrong-looking label. The next press takes it to zero, which is
    * the gesture meaning "close the running row", and exercise_end closes THE
    * NEWEST OPEN ROW: with this session's row deleted that is some older
    * unclosed one, which would be stamped with a length measured from a
    * session that is not it. With no open row at all the close fails instead,
    * and a failed close relights the button so it can be retried -- so the
    * button could never be put down again, and every press would retry a
    * close that cannot succeed.
    *
    * A FAILED REWRITE LEAVES THE ROW, so the session really is still running;
    * the sync reads the file either way and lights or rests accordingly. */
   exercise_button_sync();
   return r;
}

int exercise_end(long now)
{
   /* THE NEWEST OPEN ROW, from the tail rather than the file: the tail is
    * this file's own mirror of it, and the row being closed was written
    * seconds or minutes ago, so it is always in there. */
   struct ex_rec open_row = {0, 0, 0, 0};
   int found              = 0;
   for (int i = ex_count() - 1; i >= 0; i--) {
      struct ex_rec r = ex_at(i);
      if (r.dur == 0) {
         open_row = r;
         found    = 1;
      }
      break; /* only the NEWEST row can be the running one */
   }
   if (!found)
      return -1;
   const long dur = now - open_row.t;
   /* A SESSION OF ZERO SECONDS IS NOT A SESSION, and a negative one is a
    * clock that moved under us. Both leave the row open, which reads as "no
    * end was recorded" -- true, and better than a length that is a fiction.
    * The upper bound is EX_DUR_MAX for the reason exercise.h gives. */
   if (dur <= 0 || dur > EX_DUR_MAX)
      return -1;
   return ex_rewrite(&open_row, 0, open_row.t, open_row.level, open_row.tz,
                     dur);
}

/* ---------------- the settling rule ---------------- */

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

enum ex_press_result exercise_button_press(long now, long mono_now)
{
   mutex_lock(&ex_lk);
   const int was   = g_btn.level;
   const int armed = g_btn.armed;
   ex_press(&g_btn, mono_now);
   const int ending = (was != 0 && !armed && g_btn.level == 0);
   mutex_unlock(&ex_lk);
   if (!ending)
      return EX_PRESS_NONE;
   /* OUTSIDE THE LOCK, for the reason exercise_button_tick gives at length:
    * this rewrites a file, and a lock held across that would put a UI tap
    * behind an fsync. */
   const int closed = exercise_end(now) == 0;
   /* AND THEN THE LOG DECIDES WHAT THE BUTTON SHOWS, on both outcomes.
    *
    * Closed, and it rests. Still open because the rewrite failed, and it
    * lights again at the level the row holds -- which is what makes the close
    * RETRYABLE: the next press is the same end attempt, and the row closes
    * when the write does. Leaving it at rest would state the opposite of the
    * record, and since exercise_end only ever looks at the NEWEST row, the
    * next session would bury this one for good. */
   exercise_button_sync();
   return closed ? EX_PRESS_ENDED : EX_PRESS_FAILED;
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
    * value that was just written is not the value waiting to be written, and
    * clearing the arming here would silently discard the newer one. Only disarm
    * when what settled is still what is showing. */
   if (g_btn.level == level)
      ex_committed(&g_btn);
   mutex_unlock(&ex_lk);
   return 1;
}

void exercise_button_sync(void)
{
   struct ex_rec act;
   /* READ THE FILE'S ANSWER FIRST, then take the lock: ex_count and ex_at
    * reach the tail under its own lock, and this one is a leaf that is never
    * held across another module's. */
   const int have = exercise_active(&act);
   mutex_lock(&ex_lk);
   /* AN ARMED BUTTON IS THE USER'S, NOT THE LOG'S. Nothing has been written
    * while a value is still settling, so there is nothing in the file for it
    * to agree with, and overwriting it here would discard a choice being made
    * right now. Everything else the button shows is a statement ABOUT the
    * log, and this is where that statement is made true again -- after an
    * edit, a delete, an end, a restore, or a launch. */
   if (!g_btn.armed) {
      g_btn.level = have ? act.level : 0;
      if (!have)
         g_btn.since = 0;
   }
   mutex_unlock(&ex_lk);
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
   if (!(data_path(g_ex_path, sizeof g_ex_path, dir, "/exercise.csv")))
      ok = 0;
   return ok;
}

const char *exercise_path(void)
{
   return g_ex_path;
}

void ex_committed(struct ex_pending *p)
{
   /* THE LEVEL STAYS ON THE BUTTON; only the arming is cleared. What was
    * committed is still what the user chose, and the button showing it is how
    * they know it took. Clearing the level here would blank the button the
    * moment the record was written, which reads as the press being lost. */
   p->armed = 0;
}

void ex_press(struct ex_pending *p, long mono_now)
{
   if (p->level != 0 && !p->armed) {
      /* A SESSION THAT IS RUNNING ENDS ON ONE PRESS, whatever level it is at.
       *
       * Cycling is what the settling window is for: while a value is still
       * armed, pressing past it costs nothing because nothing has been
       * written. Once it has committed the button is no longer a chooser --
       * it is the only control that says "this is over", and the thing the
       * user wants from it is to stop. Continuing the cycle from a committed
       * 1 made them press three times to end a light session, changing the
       * recorded level twice on the way past, and each of those presses
       * restarted a settling window that could commit a level nobody meant.
       *
       * The caller reads exactly this transition to decide whether to close
       * the row (see exercise_button_press's `ending`), so ending here and
       * closing there stay one decision. */
      p->level = 0;
   } else {
      p->level = (p->level + 1) % (EX_MAX_LEVEL + 1);
   }
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
