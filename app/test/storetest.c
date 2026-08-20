// SPDX-License-Identifier: GPL-3.0
// storetest.c --- Host tests for the reading history / dedup model
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for hist_insert, the function that decides whether a
 * reading is KEPT.
 *
 * store.c was in no test binary. That matters more than most gaps here: every
 * caller persists to the append-only log only on a NON-ZERO result, so a
 * wrong return silently drops a reading forever -- no error, no log line, and
 * nothing on screen. The invariants below are all documented in store.h and
 * were all, at one point in this file's history, broken:
 *
 *   - dedup is per (source, kind), not global. When it was global, a second
 *     CGM's sample landing within the window was discarded as a duplicate of
 *     the first's -- roughly half of one sensor's data, permanently.
 *   - a BGM fingerstick dedups only on an EXACT timestamp. A fingerstick is
 *     its own event; a 150 s window would merge two real ones.
 *   - kind is part of the key, because id 0 is shared by legacy rows and any
 *     unregistered source, so ids alone do not separate CGM from BGM.
 *   - a restatement must NOT mutate in place: callers persist only on
 *     non-zero, so an in-place update changed memory while the log kept the
 *     original, and after a restart the UI silently reverted.
 *   - NHIST is a DISPLAY cap, not retention. HIST_OLD means "genuinely new but
 *     off the end of the window", and it is distinct from HIST_DUP so the
 *     caller still writes it to the log.
 *
 * Built and run by `make storetest`, which `make check` depends on.
 */
#include "store.h"
#include "alarmlogic.h" /* AL_FRESH_S: the window store_now blanks at */
#include "clock.h"
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "testdir.h" /* test_dir: the per-mode fixture directory */
#include "util.h"    /* log_clear_generation: evidence of a deliberate clear */
#include <stdio.h>
#include <sys/stat.h> /* mkdir: a directory where the log should be */
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

static void reset(void)
{
   hist_clear();
}

int main(void)
{
   const long t0 = 1700000000;

   printf("== a genuinely new reading is kept ==\n");
   reset();
   ck(hist_insert(t0, 100, 1, 7, KIND_CGM) == HIST_NEW, "first insert is NEW");
   ck(hist_count() == 1, "history holds it");

   printf("== dedup is per SOURCE, not global ==\n");
   reset();
   hist_insert(t0, 100, 1, 7, KIND_CGM);
   ck(hist_insert(t0 + 60, 105, 1, 7, KIND_CGM) == HIST_DUP,
      "same source inside the window is a duplicate");
   ck(hist_insert(t0 + 60, 105, 1, 8, KIND_CGM) == HIST_NEW,
      "a DIFFERENT source inside the window is kept");
   ck(hist_insert(t0 + 200, 110, 1, 7, KIND_CGM) == HIST_NEW,
      "same source outside the window is kept");

   printf("== kind is part of the dedup key ==\n");
   /* id 0 is shared by legacy rows and any unregistered source, so without
    * this a CGM sample would overwrite a fingerstick and be dropped itself. */
   reset();
   hist_insert(t0, 100, 0, 0, KIND_BGM);
   ck(hist_insert(t0 + 10, 120, 1, 0, KIND_CGM) == HIST_NEW,
      "a CGM sample near a BGM one with the same id is kept");

   printf("== a fingerstick dedups only on an exact timestamp ==\n");
   reset();
   hist_insert(t0, 100, 0, 3, KIND_BGM);
   ck(hist_insert(t0 + 1, 101, 0, 3, KIND_BGM) == HIST_NEW,
      "one second apart, both fingersticks are kept");
   ck(hist_insert(t0, 199, 0, 3, KIND_BGM) == HIST_DUP,
      "the same instant is a duplicate");

   printf("== a restatement does NOT mutate in place ==\n");
   reset();
   hist_insert(t0, 100, 1, 7, KIND_CGM);
   hist_insert(t0 + 60, 155, 1, 7, KIND_CGM); /* HIST_DUP */
   ck(hist_at(0).glu == 100,
      "the first sample stays authoritative, so memory matches the log");

   printf("== history stays newest-first, ties broken by source ==\n");
   reset();
   hist_insert(t0, 100, 1, 7, KIND_CGM);
   hist_insert(t0 + 600, 110, 1, 7, KIND_CGM);
   hist_insert(t0 + 300, 105, 1, 7, KIND_CGM); /* out of order (backfill) */
   ck(hist_at(0).t == t0 + 600 && hist_at(1).t == t0 + 300 &&
          hist_at(2).t == t0,
      "an out-of-order insert lands in the right place");
   reset();
   hist_insert(t0, 100, 1, 9, KIND_CGM);
   hist_insert(t0, 100, 1, 4, KIND_CGM); /* same instant, lower source id */
   ck(hist_at(0).src == 4 && hist_at(1).src == 9,
      "equal timestamps order by source, so the sort is total");

   printf("== full history: evict the oldest, and report HIST_OLD ==\n");
   reset();
   /* Fill with distinct sources so nothing dedups against anything. */
   for (int i = 0; i < NHIST; i++)
      hist_insert(t0 + ((long)i * 300), 100, 0, (i % 60) + 1, KIND_CGM);
   ck(hist_count() == NHIST, "history fills to exactly NHIST");
   long oldest = hist_at(NHIST - 1).t;
   ck(hist_insert(oldest - 300, 100, 0, 61, KIND_CGM) == HIST_OLD,
      "older than the window reports HIST_OLD, not HIST_DUP");
   /* HIST_OLD must be distinct from HIST_DUP: callers persist on any non-zero
    * result, and NHIST is a display cap, never retention. */
   ck(HIST_OLD != HIST_DUP && HIST_DUP == 0 && HIST_OLD != 0,
      "HIST_DUP is the only zero, so HIST_OLD is still persisted");
   long newest = hist_at(0).t;
   ck(hist_insert(newest + 300, 100, 0, 62, KIND_CGM) == HIST_NEW,
      "a newer reading is accepted when full");
   ck(hist_count() == NHIST, "...without growing past NHIST");
   ck(hist_at(NHIST - 1).t > oldest, "...by evicting the oldest");

   printf("== hist_prev_glu: the SAME sensor's previous CGM value ==\n");
   /* This is what the NEW DATAPOINT chirp pitches on, so a wrong answer is an
    * alert that announces a swing the wearer never had. The per-source rule is
    * the whole point: with two CGMs worn at once, the difference between them
    * is a calibration offset between two devices, not a trend. */
   {
      const long win = 450; /* CHIRP_MAX_GAP_S at the real call site */
      reset();
      ck(hist_prev_glu(t0, 7, t0 - win) == -1, "no history at all yields -1");
      (void)hist_insert(t0 - 600, 100, 1, 7, KIND_CGM);
      (void)hist_insert(t0 - 300, 110, 1, 7, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == 110, "the NEWEST older sample wins");
      ck(hist_prev_glu(t0 - 300, 7, t0 - 300 - win) == 100,
         "...measured strictly BEFORE the given instant");
      ck(hist_prev_glu(t0 - 600, 7, t0 - 600 - win) == -1,
         "the oldest sample has no predecessor");
      /* A second sensor's readings must be invisible to the first. */
      (void)hist_insert(t0 - 60, 250, 1, 9, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == 110,
         "another sensor's newer sample is NOT borrowed");
      ck(hist_prev_glu(t0, 9, t0 - win) == 250,
         "...and sensor 9 sees only its own");
      ck(hist_prev_glu(t0, 4, t0 - win) == -1, "an unknown source yields -1");
      /* A fingerstick is a different instrument with its own offset. */
      (void)hist_insert(t0 - 30, 300, 1, 7, KIND_BGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == 110,
         "a fingerstick from the same source is skipped");

      /* THE GAP RULE. Across a dropout the difference is not a rate, so the
       * chirp must fall back to its neutral pitch rather than announce the
       * whole accumulated drift as a rocket. */
      reset();
      (void)hist_insert(t0 - 300, 95, 1, 7, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == 95,
         "one cadence back is inside the window");
      reset();
      (void)hist_insert(t0 - win, 95, 1, 7, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == 95, "exactly at the edge counts");
      reset();
      (void)hist_insert(t0 - win - 1, 95, 1, 7, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == -1,
         "one second past the edge is a GAP, not a previous reading");
      reset();
      (void)hist_insert(t0 - 5400, 95, 1, 7, KIND_CGM);
      ck(hist_prev_glu(t0, 7, t0 - win) == -1,
         "a 90-minute dropout never pitches on the drift across it");
   }

   printf("== the big number belongs to the primary, or to nobody ==\n");
   reset();
   hist_insert(t0, 100, 1, 7, KIND_CGM);       /* old primary sample */
   hist_insert(t0 + 300, 140, 2, 8, KIND_CGM); /* other CGM, newer */
   hist_insert(t0 + 600, 190, 0, 3, KIND_BGM); /* fingerstick, newest */
   hist_refresh_current(7);
   ck(store_now(0).glu == 100 && store_now(0).t == t0,
      "primary's sample wins over a newer secondary and a fingerstick");
   hist_refresh_current(8);
   ck(store_now(0).glu == 140,
      "switching primary re-binds to the new owner's data");
   /* The contract the display promises: a primary with NO data shows no data,
    * never another sensor's. Falling back used to keep the old sensor's value
    * as the big number after the user explicitly promoted a fresh sensor. */
   hist_refresh_current(9);
   ck(store_now(0).glu == -1 && store_now(0).t == 0,
      "a primary with no data CLEARS the current reading");
   /* Only with no primary at all (pre-registry install) may any CGM fill in --
    * and a fingerstick is still never eligible. */
   hist_refresh_current(-1);
   ck(store_now(0).glu == 140, "no primary: newest CGM of any source fills in");
   reset();
   hist_insert(t0, 190, 0, 3, KIND_BGM);
   hist_refresh_current(-1);
   ck(store_now(0).glu == -1,
      "a lone fingerstick never becomes the big number");

   printf("== the big number STOPS being current when it cannot be aged ==\n");
   {
      /* store_now's `stale` is what blanks the big number, and it used to be
       * `now - t > AL_FRESH_S` -- true for every POSITIVE age and false for
       * every negative one. After a backward clock correction (a timezone
       * fix, an NTP step, a date typed in by hand) every stored reading is
       * dated in the future, so the last known value read as current for as
       * long as the skew lasted: the number on screen simply stopped ageing
       * and went on asserting, say, a 58 mg/dL from three hours ago as the
       * reading right now. That is worse than blanking, because the person
       * deciding whether to treat a low cannot tell a live number from a
       * frozen one -- the screen looks identical.
       *
       * It must also agree with alarm_zone, which refuses the same stamps: a
       * screen showing a live value beside an alarm that has decided there is
       * no current reading is two answers to one question. */
      reset();
      hist_insert(t0, 100, 1, 7, KIND_CGM);
      hist_refresh_current(7);
      ck(store_now(t0).stale == 0, "a zero-age reading is current");
      ck(store_now(t0 + AL_FRESH_S).stale == 0,
         "...and at the freshness edge it still is");
      ck(store_now(t0 + AL_FRESH_S + 1).stale == 1,
         "...one second past it, it is not");
      /* THE ROLLBACK. Both sides of the boundary, so a mutant that merely
       * shifts the window cannot pass either. */
      ck(store_now(t0 - 1).stale == 1,
         "ONE SECOND into the future is already not current");
      ck(store_now(t0 - 3600).stale == 1,
         "...and an hour of clock rollback certainly is not");
      /* And "no reading at all" is still stale whatever the clock says, so
       * the guard above cannot be what is carrying these.
       *
       * HONEST LIMIT: this does NOT pin cur_stale's `glu < 0` term. With no
       * reading, hist_refresh_current writes glu -1 and t 0 TOGETHER, so the
       * age is ~55 years and the freshness test refuses the row on its own --
       * verified by mutation, which is how it was found: deleting `glu < 0`
       * leaves the suite green. The pair (glu -1, t fresh) is not reachable
       * through this module's interface at all, so the term is defence in
       * depth and is left honestly unpinned rather than covered by an
       * assertion that would pass either way. */
      reset();
      hist_refresh_current(7);
      ck(store_now(t0).stale == 1, "no reading at all is stale");

      /* THE LOCKED VARIANT MUST ANSWER THE SAME. It exists for callers that
       * already hold the store lock -- the frame builder, which holds it
       * across the whole model build, and the alarm gatherer -- so a
       * divergence here is the screen and the alarm disagreeing about whether
       * there is a current reading at all. Verified by mutation: hard-coding
       * its `stale` to 0 survived the entire suite until this case existed,
       * because nothing called it. */
      reset();
      hist_insert(t0, 100, 1, 7, KIND_CGM);
      hist_refresh_current(7);
      store_lock();
      struct reading_now lk_now    = store_now_locked(t0);
      struct reading_now lk_old    = store_now_locked(t0 + AL_FRESH_S + 1);
      struct reading_now lk_future = store_now_locked(t0 - 3600);
      store_unlock();
      ck(lk_now.stale == 0, "locked: a zero-age reading is current");
      ck(lk_old.stale == 1, "locked: one second past fresh, it is not");
      ck(lk_future.stale == 1, "locked: a rolled-back clock is not current");
      ck(lk_now.glu == 100 && lk_now.t == t0,
         "locked: and it carries the same reading store_now does");
   }

   printf("== the FILE path: append, reload, and the load-time bounds ==\n");
   /* Everything above exercises hist_insert only. store_append, store_load,
    * store_count and rdfield had no coverage at all -- which is how an
    * infinite loop in a sibling parser reached the tree. These are the checks
    * for the guards that were added to this file recently. */
   {
      /* The SAME path-building the app uses, pointed at the scratch
       * directory: the module owns its filename now, so a test that wrote
       * one of its own would stop exercising that. */
      store_paths(test_dir());
      unlink(store_path());
      long now = realtime_s();

      reset();
      store_append(now - 600, 120, 1, -70, 1, 7, 0, 0, KIND_CGM, 1000);
      store_append(now - 300, 130, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      ck(store_count() == 2, "two appended rows are counted");

      reset();
      store_load(-1);
      ck(hist_count() == 2, "both rows load back");
      ck(hist_at(0).glu == 130 && hist_at(1).glu == 120,
         "...newest first, values intact");
      ck(hist_at(0).src == 7, "...with their source id");

      /* WHAT THE LOADER REPORTS, not only what it admits.
       *
       * These three used to check only that the bad row was kept out of the
       * history. That is half the contract: the app decides whether to warn
       * the user from the RESULT, and a load that quietly drops a row and
       * returns success shows a short record as a complete one. */
      ck(store_load(-1) == 0, "a clean log reads whole");
      ck(hist_count() == 2, "...with both rows");

      /* A FILE CUT MID-APPEND. The bytes below parse perfectly -- a plausible
       * glucose at a plausible time -- and are still half a row, because what
       * says a row is finished is the newline the file does not have. It must
       * not reach the history, and the load must say the log is incomplete.
       * The previous version of this case made the row IMPLAUSIBLE, so it
       * proved the range check and never the newline rule. */
      {
         FILE *f = fopen(store_path(), "a");
         if (f) {
            fprintf(f, "%ld,133,1,,0,7,0,0,0", (long)(now - 50));
            fclose(f);
         }
      }
      reset();
      ck(store_load(-1) < 0, "a log that ends mid-row reads INCOMPLETE");
      ck(hist_count() == 2, "...and the rows before it are kept");
      int invented = 0;
      for (int i = 0; i < hist_count(); i++)
         if (hist_at(i).glu == 133)
            invented = 1;
      ck(!invented, "...while the half row is NOT published as a reading");

      /* ...and the same file with the newline restored is whole again, so the
       * rule is about the terminator and not about the row. */
      {
         FILE *f = fopen(store_path(), "a");
         if (f) {
            fputs("\n", f);
            fclose(f);
         }
      }
      reset();
      ck(store_load(-1) == 0, "the same row, terminated, reads whole");
      ck(hist_count() == 3, "...and is published");

      /* A ROW THAT IS NOT A READING is damage too: skipped, and reported. */
      {
         FILE *f = fopen(store_path(), "a");
         if (f) {
            fputs("this is not a reading\n", f);
            fclose(f);
         }
      }
      reset();
      ck(store_load(-1) < 0, "a malformed row makes the load incomplete");
      ck(hist_count() == 3, "...and the real readings are still there");

      /* Back to the two-row log the cases below were written against: they
       * assert on counts, and this block has been appending. */
      unlink(store_path());
      store_append(now - 600, 120, 1, -70, 1, 7, 0, 0, KIND_CGM, 1000);
      store_append(now - 300, 130, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      reset();
      ck(store_load(-1) == 0 && hist_count() == 2, "the log is back to two "
                                                   "whole rows");

      /* The bound added after a meter clock wrote a year-2039 row: a
       * future-dated row sorts to the head of the history permanently and is
       * re-admitted on EVERY restart of a file that is never rewritten. */
      store_append(now + 86400, 150, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      reset();
      store_load(-1);
      ck(hist_count() == 2, "a future-dated row is refused at load");

      /* Glucose is bounded symmetrically with the live path. */
      store_append(now - 100, 5, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      store_append(now - 101, 5000, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      reset();
      store_load(-1);
      ck(hist_count() == 2, "implausible glucose is refused at load");

      /* rdfield saturates rather than overflowing. An over-long digit run is
       * UB if accumulated unbounded, and it happens during parsing, before any
       * range check can reject the row. */
      {
         FILE *f = fopen(store_path(), "a");
         if (f) {
            fputs("99999999999999999999999999,120,1,,0,7,0,0,0\n", f);
            fclose(f);
         }
         reset();
         store_load(-1);
         ck(hist_count() == 2,
            "an over-long timestamp field is refused, not wrapped");
      }

      /* A legacy v1 row (5 fields) must still load: src and kind fall back to
       * 0, which is exactly "pre-registry, continuous". */
      unlink(store_path());
      {
         FILE *f = fopen(store_path(), "w");
         if (f) {
            fprintf(f, "%ld,111,1,,0\n", now - 200);
            fclose(f);
         }
      }
      reset();
      store_load(-1);
      ck(hist_count() == 1 && hist_at(0).glu == 111, "a v1 row still loads");
      ck(hist_at(0).src == 0 && hist_at(0).kind == 0,
         "...defaulting to the legacy source and kind");

      /* A truncated final line must not crash or half-parse. */
      {
         FILE *f = fopen(store_path(), "a");
         if (f) {
            /* The prefix must be IMPLAUSIBLE, or the test proves nothing:
             * a truncated row whose leading fields happen to parse as a valid
             * reading is indistinguishable from a legitimate short v1 row, and
             * admitting it is correct. (store_append's short-write rollback is
             * the mitigation on the write side; the reader cannot tell.) */
            fputs("1700000000,9", f); /* glucose below the plausible floor */
            fclose(f);
         }
      }
      reset();
      store_load(-1);
      /* == not >=: the prior state is 1, so >= passes whether the truncated
       * row is refused (1) or half-parsed and admitted (2) -- it could not
       * fail in the direction it guards. */
      ck(hist_count() == 1,
         "a truncated trailing row is refused, not half-parsed");
   }

   printf("== a RELOAD replaces the history; it does not merge into it ==\n");
   /* WHAT THE USER SAW. They reinstall, tap RESTORE, and their record comes
    * back -- plus rows they had deleted before restoring, which nothing in
    * the app can remove again, and minus the corrections the restored file
    * carried, because hist_insert saw a sample from the same source within
    * 150 s and declined it as a restatement. Both survive until the process
    * does. Neither looks like an error: the restored rows really did appear,
    * so the restore looks exactly like a restore that worked.
    *
    * Every OTHER assertion about the loader in this file passes an insert-only
    * loader, because they all reset() first and then check that the restored
    * rows are PRESENT. These do not reset, and check what is ABSENT. */
   {
      store_paths(test_dir());
      long now = realtime_s();

      /* Three rows, spaced well past the 150 s dedup window. */
      unlink(store_path());
      FILE *f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,111,1,,0,7,0,0,0\n", now - 900);
         fprintf(f, "%ld,122,1,,0,7,0,0,0\n", now - 600);
         fprintf(f, "%ld,133,1,,0,7,0,0,0\n", now - 300);
         fclose(f);
      }
      reset();
      ck(store_load(-1) == 0, "a three-row log loads whole");
      ck(hist_count() == 3, "...and all three are in the history");

      /* THE ISOLATING CASE: the middle row is GONE from the file. A loader
       * that inserts leaves it in memory for ever -- there is no path in the
       * app that removes a row from g_hist -- so the plot and the history
       * list go on showing a reading the record does not contain. */
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,111,1,,0,7,0,0,0\n", now - 900);
         fprintf(f, "%ld,133,1,,0,7,0,0,0\n", now - 300);
         fclose(f);
      }
      /* NO reset() here, deliberately: reloading over a live table is the
       * whole situation. The mutating call is on its own line so nothing can
       * read the count before it runs. */
      int rc_rm = store_load(-1);
      ck(rc_rm == 0, "the shortened log still reads whole");
      ck(hist_count() == 2, "a row REMOVED from the file is gone from history");
      int found = 0;
      for (int i = 0; i < hist_count(); i++)
         if (hist_at(i).glu == 122)
            found = 1;
      ck(!found, "...and it is that row, not merely a smaller count");
      ck(hist_at(0).glu == 133 && hist_at(1).glu == 111,
         "...while the rows the file kept are intact and still newest-first");

      /* THE SECOND HALF: a CORRECTED value at the same timestamp. Inserting
       * hands this to the dedup, which is right for a live arrival -- a
       * restatement must not silently rewrite a logged reading -- and wrong
       * for a reload, where the file IS the new truth. The old value won, and
       * went on being displayed and alarmed on until a restart. */
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,111,1,,0,7,0,0,0\n", now - 900);
         fprintf(f, "%ld,177,1,,0,7,0,0,0\n", now - 300); /* was 133 */
         fclose(f);
      }
      int rc_fix = store_load(-1);
      ck(rc_fix == 0, "the corrected log reads whole");
      ck(hist_at(0).glu == 177,
         "a value CORRECTED in the file wins over the deduped old one");
      ck(hist_count() == 2, "...without leaving both behind");
      /* AND THE BIG NUMBER WITH IT, which is what the person holding the
       * phone actually sees. The table being right is invisible; the number
       * in the middle of the screen is the bug they reported -- the restore
       * completed, the plot redrew, and the headline reading stayed at the
       * value the restored file had corrected away. It is derived from the
       * history, so it must be derived AFTER the swap; deriving it first
       * publishes the reading the OLD table happened to end on, and every
       * assertion above this line still passes (verified by mutation:
       * hoisting hist_refresh_current above the memcpy survived the entire
       * suite until this line existed). */
      ck(store_now(0).glu == 177,
         "...and the big number is the corrected reading, not the old one");

      /* AND THE FAILURE PATH, which is the load-bearing half: a reload that
       * cannot read its source must leave the previous table WHOLE. Neither
       * the old history nor the restored one is the worst possible outcome,
       * and a stage-then-swap loader is only an improvement if the swap
       * cannot happen from a prefix.
       *
       * A DIRECTORY where the log should be: open() succeeds on it and
       * read() fails with EISDIR, which is a genuine mid-load read failure
       * and not something a test can otherwise ask a healthy filesystem for.
       */
      int before_n      = hist_count();
      struct reading b0 = hist_at(0);
      struct reading b1 = hist_at(1);
      /* EVERYTHING THE LOAD PUBLISHES, captured BEFORE the failing call --
       * not just the table. The history is what the plot draws, but the big
       * number and the "readings stored" figure on the settings screen are
       * derived from the same load, and a failure that preserved the array
       * while blanking the number the user is looking at would still leave
       * them between two records. Each capture is its own statement: reading
       * these inside the same ck() as the mutating call would evaluate them
       * in whatever order the compiler chose. */
      struct reading_now bnow = store_now(0);
      int before_stored       = store_appended();
      unlink(store_path());
      if (mkdir(store_path(), 0700) != 0)
         ck(0, "the unreadable-log fixture could not be staged");
      int rc_fail = store_load(-1);
      ck(rc_fail < 0, "a log that cannot be READ reports failure");
      ck(hist_count() == before_n,
         "...and the previous history is still there");
      ck(hist_at(0).glu == b0.glu && hist_at(0).t == b0.t &&
             hist_at(1).glu == b1.glu && hist_at(1).t == b1.t,
         "...every row of it, unchanged");
      struct reading_now anow = store_now(0);
      ck(anow.glu == bnow.glu && anow.t == bnow.t,
         "...and the big number still describes it, not a cleared load");
      ck(store_appended() == before_stored,
         "...and the stored-row count still describes the previous log");
      rmdir(store_path());

      /* The fixture is healthy again, so a later load in this suite is not
       * inheriting a broken path -- and this also shows the failure left
       * nothing wedged. */
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,144,1,,0,7,0,0,0\n", now - 300);
         fclose(f);
      }
      int rc_after = store_load(-1);
      ck(rc_after == 0, "and a good log loads again afterwards");
      ck(hist_count() == 1 && hist_at(0).glu == 144,
         "...replacing the preserved table in its turn");

      /* THE LIMIT CASE OF "REPLACE": a restored log with NO readings in it.
       * A user who cleared their record and then restored it on a new phone
       * gets a file that is a header and nothing else, and "replace" has to
       * mean replace here too. This is the case a defensive `if (n > 0)`
       * around the swap would quietly exempt -- and such a guard reads
       * entirely reasonable ("never wipe the history from an empty file")
       * while passing every other assertion in this block, because every
       * other file here has rows. An empty record is a record. */
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "# unix_time,glucose_mgdl,trend\n");
         fclose(f);
      }
      int rc_empty = store_load(-1);
      ck(rc_empty == 0, "a log holding only a header reads whole");
      ck(hist_count() == 0, "...and REPLACES the history with nothing");
      ck(store_now(0).glu == -1,
         "...clearing the big number rather than latching the last value");

      /* THE LINK RSSI PUBLISHED BY A LOAD BELONGS TO THE NEWEST ROW, and the
       * log is in ARRIVAL order, so the newest row is NOT the last one in the
       * file: importing months of history appends thousands of OLD rows after
       * the live ones. The tracking used to reset per read CHUNK and then
       * assign unconditionally, so whichever chunk last happened to carry an
       * RSSI won -- which on an imported log is the oldest reading in the
       * record, published as the current link quality. Nothing asserted on it
       * (verified by mutation: making the last row always win survived the
       * whole suite), so it is pinned here.
       *
       * Both rows in one file with the OLDER one written last, which is
       * exactly the arrival-order shape. */
      unlink(store_path());
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,150,1,-50,0,7,0,0,0\n", now - 300); /* newest */
         fprintf(f, "%ld,140,1,-88,0,7,0,0,0\n", now - 900); /* older, last */
         fclose(f);
      }
      reset();
      int rc_rssi = store_load(-1);
      ck(rc_rssi == 0, "a log whose oldest row was appended last reads whole");
      struct reading_rssi rr = store_rssi();
      ck(rr.ok == 1, "...and publishes a link RSSI");
      ck(rr.dbm == -50,
         "...the NEWEST row's, not the last one written to the file");

      /* A NEWEST ROW WITH NO RSSI PUBLISHES NOTHING, rather than publishing
       * zero. store.h is explicit that `ok == 0` means "nothing has reported
       * one", precisely because a genuine 0 dBm is not a reading any radio
       * produces -- so a load that forced `ok` would put a 0 dBm signal
       * strength on the DEVICES screen for a link that simply did not record
       * one. Rows written by the meter path and every v1 row have the field
       * empty, so this is the common case, not an exotic one. */
      f = fopen(store_path(), "w");
      if (f) {
         fprintf(f, "%ld,151,1,,0,7,0,0,0\n", now - 200); /* newest, no rssi */
         fprintf(f, "%ld,141,1,-88,0,7,0,0,0\n", now - 800);
         fclose(f);
      }
      reset();
      int rc_norssi = store_load(-1);
      ck(rc_norssi == 0, "a log whose newest row has no RSSI reads whole");
      struct reading_rssi r2 = store_rssi();
      ck(r2.dbm == -50 && r2.ok == 1,
         "...and leaves the last known RSSI alone rather than publishing 0");
      unlink(store_path());
   }

   /* A KIND OUT OF RANGE IS NORMALISED, not stored as given.
    *
    * store.h declares this field to be KIND_CGM or KIND_BGM, and four things
    * downstream assume it: the renderer draws kind == KIND_INS along the
    * bottom edge
    * (so a 2 would render as a phantom insulin dose on the glucose plot),
    * stats count anything != KIND_BGM toward TIR/AVG/A1C, the link
    * watchdogs read it as a live sample, and the dedup below compares kind
    * for equality so a bad one will not dedup against the real row. The
    * loader bounds the timestamp and the glucose beside it but passed the
    * kind straight through, and readings.csv is append-only -- a row
    * admitted once is re-admitted at every launch, for good. */
   {
      reset();
      ck(hist_insert(1700000000L, 100, 0, 5, KIND_INS) == HIST_NEW,
         "a KIND_INS row inserts");
      ck(hist_at(0).kind == KIND_CGM, "...normalised to KIND_CGM, not stored");
      reset();
      hist_insert(1700000000L, 100, 0, 5, 200); /* garbage from a torn write */
      ck(hist_at(0).kind == KIND_CGM, "an out-of-range kind becomes KIND_CGM");
      /* A real fingerstick must still survive the normalisation: forcing
       * everything to CGM would silently reclassify every meter reading. */
      reset();
      hist_insert(1700000000L, 100, 0, 5, KIND_BGM);
      ck(hist_at(0).kind == KIND_BGM, "KIND_BGM is preserved");
      /* And the normalisation must happen BEFORE the dedup, or two rows that
       * differ only in a corrupt kind sit beside each other and double-count.
       */
      reset();
      hist_insert(1700000000L, 100, 0, 5, KIND_CGM);
      ck(hist_insert(1700000000L, 100, 0, 5, 7) == HIST_DUP,
         "a corrupt-kind duplicate dedups against the real row");
   }

   /* EVICTION AND OUT-OF-ORDER INSERTION, INTERLEAVED.
    *
    * The cases above exercise each alone: a backfill into a part-full
    * history, and an eviction from a full one fed in order. Real data does
    * both at once -- readings.csv is in ARRIVAL order, and imported history
    * is appended long after the rows it predates, so a measured log of
    * 37337 rows carried 334 that went backwards while the buffer was
    * already full at NHIST. That is the state where an insertion sort walks
    * a full array and the eviction shifts under it, and no small case
    * reaches it. Assert the documented total order (newest first, ties by
    * ascending source) survives it. */
   {
      reset();
      unsigned rng   = 20260804U;
      const long t0s = 1700000000L;
      for (int i = 0; i < NHIST * 3; i++) {
         rng = (rng * 1103515245U) + 12345U;
         /* Mostly forward, sometimes well backwards -- the shape of an
          * import landing behind the live tail. */
         long back =
             ((rng >> 16U) % 100U) < 10U ? (long)((rng >> 8U) % 50000U) : 0;
         long t  = t0s + ((long)i * 300) - back;
         int src = (int)((rng >> 20U) % 5U) + 1;
         int knd = ((rng >> 24U) % 8U) == 0 ? KIND_BGM : KIND_CGM;
         (void)hist_insert(t, 80 + (int)((rng >> 12U) % 200U), 0, src, knd);
      }
      ck(hist_count() == NHIST, "the shuffled stream fills history to NHIST");
      int ord = 0;
      int dup = 0;
      for (int i = 0; i + 1 < hist_count(); i++) {
         if (!(hist_at(i).t > hist_at(i + 1).t ||
               (hist_at(i).t == hist_at(i + 1).t &&
                hist_at(i).src < hist_at(i + 1).src)))
            ord++;
         if (hist_at(i).t == hist_at(i + 1).t &&
             hist_at(i).src == hist_at(i + 1).src &&
             hist_at(i).kind == hist_at(i + 1).kind)
            dup++;
      }
      ck(ord == 0, "...and stays totally ordered under eviction + backfill");
      ck(dup == 0, "...with no duplicate (t, src, kind) surviving the dedup");
   }

   /* ---- REMOVING THE LAST DEVICE IS A DELETION, AND IT LEAVES EVIDENCE ----
    *
    * slots.csv is one of the five files the phone syncs, and the phone is
    * AUTHORITATIVE over the server's copy of it: a log the phone no longer
    * holds is an instruction to delete the replica. The sync client refuses
    * that instruction when it cannot tell a deliberate emptying from a phone
    * that lost its storage -- and it cannot tell by looking, because both are
    * a log with no rows in it.
    *
    * This file is exactly where that bites. slots_save() rewrites the WHOLE
    * registry every time and writes no header, so removing the last device
    * leaves slots.csv ZERO BYTES LONG. Without the evidence minted below the
    * user's removal never reached the server, the server went on listing a
    * device they had removed, and -- because sync_run stops at the first log
    * that fails -- the readings, doses and weights stopped syncing behind it.
    * A deliberate deletion that can never converge is a second way to lose
    * the record: by making the backup stop.
    *
    * The evidence is durable and versioned (see util.h); what is asserted
    * here is that the DELETION WORKFLOW is what mints it, that it is minted
    * only for a registry that really is empty, and that it does not outlive
    * the emptiness it describes. */
   printf("== emptying the device registry leaves durable evidence ==\n");
   {
      sensors_paths(test_dir());
      unlink(slots_path());
      {
         char cp[600];
         (void)snprintf(cp, sizeof cp, "%s%s", slots_path(), LOG_CLEAR_SUFFIX);
         unlink(cp);
      }
      unlink(sensors_path());
      sensors_load();

      int id = sensor_mint(0, "AA:BB:CC:DD:EE:01", "SER1", "G7", "1.0", 0);
      ck(id > 0, "a sensor is minted");
      ck(sensor_claim_slot(id, 0, "AA:BB:CC:DD:EE:01") >= 0,
         "...and takes a slot, which writes slots.csv");
      ck(slot_count() == 1, "the registry holds one device");
      ck(log_clear_generation(slots_path()) == 0,
         "a registry with a device in it is not deliberately empty");

      /* THE DELETION. Nothing else in this test writes slots.csv, so the
       * evidence that appears can only have been minted by this call. */
      ck(sensor_forget(id) == 0, "the last device is removed");
      ck(slot_count() == 0, "...leaving the registry empty");
      ck(log_clear_generation(slots_path()) == 1,
         "...and the deletion workflow recorded it: generation 1");

      /* AND IT DOES NOT OUTLIVE THE EMPTINESS. A device added back makes the
       * registry non-empty, so evidence of emptiness must go -- otherwise a
       * tombstone minted for one deliberate clear would authorise an
       * accidental one months later. */
      int id2 = sensor_mint(0, "AA:BB:CC:DD:EE:02", "SER2", "G7", "1.0", 0);
      ck(id2 > 0 && sensor_claim_slot(id2, 0, "AA:BB:CC:DD:EE:02") >= 0,
         "a device is added back");
      ck(log_clear_generation(slots_path()) == 0,
         "...and the tombstone is gone: this registry is not empty");
      /* ASKED OF THE FILE, not of log_clear_generation, which answers 0 for a
       * non-empty registry whatever is lying beside it -- so on its own the
       * assertion above would pass over a tombstone still sitting on disk,
       * waiting to authorise an emptiness nobody asked for. */
      {
         char cp[600];
         (void)snprintf(cp, sizeof cp, "%s%s", slots_path(), LOG_CLEAR_SUFFIX);
         FILE *tf = fopen(cp, "rb");
         ck(tf == NULL, "...really gone: the file is not there to be reused");
         if (tf)
            fclose(tf);
      }

      /* Evidence cannot be minted for a state that does not exist, which is
       * what stops a caller from authorising a deletion that never happened.
       */
      ck(log_note_cleared(slots_path()) == REPLACE_FAILED,
         "a registry that still holds a device cannot be declared empty");
      ck(log_clear_generation(slots_path()) == 0, "...and nothing was minted");

      /* ---- WHAT A DAMAGED OR FOREIGN TOMBSTONE ANSWERS ----
       *
       * Every one of these is a file whose bytes are not exactly the format,
       * and every one must read as NO EVIDENCE -- the answer authorises
       * deleting the user's history, so anything short of an exact match is
       * a refusal. The VERSION is the one that matters across builds: a
       * future build that adds a field bumps it, and this build must refuse
       * rather than half-understand it. */
      ck(sensor_forget(id2) == 0, "the registry is emptied once more");
      ck(log_clear_generation(slots_path()) == 1, "...with fresh evidence");
      {
         char cp[600];
         (void)snprintf(cp, sizeof cp, "%s%s", slots_path(), LOG_CLEAR_SUFFIX);
         static const char *const bad[] = {
             "pancra-clear 2 1\n",   /* a version this build does not know */
             "pancra-clear 1 0\n",   /* generation 0 is not a generation */
             "pancra-clear 1 1",     /* no terminator: a torn write */
             "pancra-clear 1 1\nx",  /* trailing bytes: not this file */
             "pancra-clear 1 1x\n",  /* not a number all the way */
             "pancra-clearish 1 1\n" /* not this magic, merely starting like it
                                      */
         };
         static const char *const why[] = {
             "a tombstone from a NEWER format reads as no evidence",
             "...as does a zero generation",
             "...an unterminated line",
             "...trailing bytes after the line",
             "...a generation with rubbish after it",
             "...and a magic that merely starts the same way"};
         for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            FILE *bf = fopen(cp, "wb");
            ck(bf != NULL, "a damaged tombstone can be written");
            if (bf) {
               fputs(bad[i], bf);
               fclose(bf);
            }
            ck(log_clear_generation(slots_path()) == 0, why[i]);
         }
         /* And the well-formed one still reads, so the cases above failed on
          * their damage rather than on the fixture being broken. */
         FILE *gf = fopen(cp, "wb");
         if (gf) {
            fputs("pancra-clear 1 7\n", gf);
            fclose(gf);
         }
         ck(log_clear_generation(slots_path()) == 7,
            "...while a well-formed one is read, generation and all");
         unlink(cp);
      }
      unlink(slots_path());
      unlink(sensors_path());
   }

   printf("\n%s\n", all ? "ALL STORE TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
