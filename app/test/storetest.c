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
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "util.h"
#include <stdio.h>
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
   g_nhist = 0;
}

int main(void)
{
   const long t0 = 1700000000;

   printf("== a genuinely new reading is kept ==\n");
   reset();
   ck(hist_insert(t0, 100, 1, 7, KIND_CGM) == HIST_NEW, "first insert is NEW");
   ck(g_nhist == 1, "history holds it");

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
   ck(g_hist[0].glu == 100,
      "the first sample stays authoritative, so memory matches the log");

   printf("== history stays newest-first, ties broken by source ==\n");
   reset();
   hist_insert(t0, 100, 1, 7, KIND_CGM);
   hist_insert(t0 + 600, 110, 1, 7, KIND_CGM);
   hist_insert(t0 + 300, 105, 1, 7, KIND_CGM); /* out of order (backfill) */
   ck(g_hist[0].t == t0 + 600 && g_hist[1].t == t0 + 300 && g_hist[2].t == t0,
      "an out-of-order insert lands in the right place");
   reset();
   hist_insert(t0, 100, 1, 9, KIND_CGM);
   hist_insert(t0, 100, 1, 4, KIND_CGM); /* same instant, lower source id */
   ck(g_hist[0].src == 4 && g_hist[1].src == 9,
      "equal timestamps order by source, so the sort is total");

   printf("== full history: evict the oldest, and report HIST_OLD ==\n");
   reset();
   /* Fill with distinct sources so nothing dedups against anything. */
   for (int i = 0; i < NHIST; i++)
      hist_insert(t0 + ((long)i * 300), 100, 0, (i % 60) + 1, KIND_CGM);
   ck(g_nhist == NHIST, "history fills to exactly NHIST");
   long oldest = g_hist[NHIST - 1].t;
   ck(hist_insert(oldest - 300, 100, 0, 61, KIND_CGM) == HIST_OLD,
      "older than the window reports HIST_OLD, not HIST_DUP");
   /* HIST_OLD must be distinct from HIST_DUP: callers persist on any non-zero
    * result, and NHIST is a display cap, never retention. */
   ck(HIST_OLD != HIST_DUP && HIST_DUP == 0 && HIST_OLD != 0,
      "HIST_DUP is the only zero, so HIST_OLD is still persisted");
   long newest = g_hist[0].t;
   ck(hist_insert(newest + 300, 100, 0, 62, KIND_CGM) == HIST_NEW,
      "a newer reading is accepted when full");
   ck(g_nhist == NHIST, "...without growing past NHIST");
   ck(g_hist[NHIST - 1].t > oldest, "...by evicting the oldest");

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
   ck(g_cur_glu == 100 && g_cur_time == t0,
      "primary's sample wins over a newer secondary and a fingerstick");
   hist_refresh_current(8);
   ck(g_cur_glu == 140, "switching primary re-binds to the new owner's data");
   /* The contract the display promises: a primary with NO data shows no data,
    * never another sensor's. Falling back used to keep the old sensor's value
    * as the big number after the user explicitly promoted a fresh sensor. */
   hist_refresh_current(9);
   ck(g_cur_glu == -1 && g_cur_time == 0,
      "a primary with no data CLEARS the current reading");
   /* Only with no primary at all (pre-registry install) may any CGM fill in --
    * and a fingerstick is still never eligible. */
   hist_refresh_current(-1);
   ck(g_cur_glu == 140, "no primary: newest CGM of any source fills in");
   reset();
   hist_insert(t0, 190, 0, 3, KIND_BGM);
   hist_refresh_current(-1);
   ck(g_cur_glu == -1, "a lone fingerstick never becomes the big number");

   printf("== the FILE path: append, reload, and the load-time bounds ==\n");
   /* Everything above exercises hist_insert only. store_append, store_load,
    * store_count and rdfield had no coverage at all -- which is how an
    * infinite loop in a sibling parser reached the tree. These are the checks
    * for the guards that were added to this file recently. */
   {
      (void)snprintf(g_store_path, sizeof g_store_path,
                     "build/app/test/rt-read.csv");
      unlink(g_store_path);
      long now = realtime_s();

      reset();
      store_append(now - 600, 120, 1, -70, 1, 7, 0, 0, KIND_CGM, 1000);
      store_append(now - 300, 130, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      ck(store_count() == 2, "two appended rows are counted");

      reset();
      store_load();
      ck(g_nhist == 2, "both rows load back");
      ck(g_hist[0].glu == 130 && g_hist[1].glu == 120,
         "...newest first, values intact");
      ck(g_hist[0].src == 7, "...with their source id");

      /* The bound added after a meter clock wrote a year-2039 row: a
       * future-dated row sorts to the head of the history permanently and is
       * re-admitted on EVERY restart of a file that is never rewritten. */
      store_append(now + 86400, 150, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      reset();
      store_load();
      ck(g_nhist == 2, "a future-dated row is refused at load");

      /* Glucose is bounded symmetrically with the live path. */
      store_append(now - 100, 5, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      store_append(now - 101, 5000, 1, 0, 0, 7, 0, 0, KIND_CGM, 1000);
      reset();
      store_load();
      ck(g_nhist == 2, "implausible glucose is refused at load");

      /* rdfield saturates rather than overflowing. An over-long digit run is
       * UB if accumulated unbounded, and it happens during parsing, before any
       * range check can reject the row. */
      {
         FILE *f = fopen(g_store_path, "a");
         if (f) {
            fputs("99999999999999999999999999,120,1,,0,7,0,0,0\n", f);
            fclose(f);
         }
         reset();
         store_load();
         ck(g_nhist == 2,
            "an over-long timestamp field is refused, not wrapped");
      }

      /* A legacy v1 row (5 fields) must still load: src and kind fall back to
       * 0, which is exactly "pre-registry, continuous". */
      unlink(g_store_path);
      {
         FILE *f = fopen(g_store_path, "w");
         if (f) {
            fprintf(f, "%ld,111,1,,0\n", now - 200);
            fclose(f);
         }
      }
      reset();
      store_load();
      ck(g_nhist == 1 && g_hist[0].glu == 111, "a v1 row still loads");
      ck(g_hist[0].src == 0 && g_hist[0].kind == 0,
         "...defaulting to the legacy source and kind");

      /* A truncated final line must not crash or half-parse. */
      {
         FILE *f = fopen(g_store_path, "a");
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
      store_load();
      /* == not >=: the prior state is 1, so >= passes whether the truncated
       * row is refused (1) or half-parsed and admitted (2) -- it could not
       * fail in the direction it guards. */
      ck(g_nhist == 1, "a truncated trailing row is refused, not half-parsed");
   }

   /* A KIND OUT OF RANGE IS NORMALISED, not stored as given.
    *
    * store.h declares this field to be KIND_CGM or KIND_BGM, and four things
    * downstream assume it: ui.c draws kind == KIND_INS along the bottom edge
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
      ck(g_hist[0].kind == KIND_CGM, "...normalised to KIND_CGM, not stored");
      reset();
      hist_insert(1700000000L, 100, 0, 5, 200); /* garbage from a torn write */
      ck(g_hist[0].kind == KIND_CGM, "an out-of-range kind becomes KIND_CGM");
      /* A real fingerstick must still survive the normalisation: forcing
       * everything to CGM would silently reclassify every meter reading. */
      reset();
      hist_insert(1700000000L, 100, 0, 5, KIND_BGM);
      ck(g_hist[0].kind == KIND_BGM, "KIND_BGM is preserved");
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
      ck(g_nhist == NHIST, "the shuffled stream fills history to NHIST");
      int ord = 0;
      int dup = 0;
      for (int i = 0; i + 1 < g_nhist; i++) {
         if (!(g_hist[i].t > g_hist[i + 1].t ||
               (g_hist[i].t == g_hist[i + 1].t &&
                g_hist[i].src < g_hist[i + 1].src)))
            ord++;
         if (g_hist[i].t == g_hist[i + 1].t &&
             g_hist[i].src == g_hist[i + 1].src &&
             g_hist[i].kind == g_hist[i + 1].kind)
            dup++;
      }
      ck(ord == 0, "...and stays totally ordered under eviction + backfill");
      ck(dup == 0, "...with no duplicate (t, src, kind) surviving the dedup");
   }

   printf("\n%s\n", all ? "ALL STORE TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
