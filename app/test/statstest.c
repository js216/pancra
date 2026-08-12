// SPDX-License-Identifier: GPL-3.0
// statstest.c --- Host tests for the rolling TIR / average buckets
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for stats.c, which was in no test binary.
 *
 * The failure mode here is quiet: the buckets are a ring keyed by
 * `hour % STAT_HOURS`, so a reading the ring cannot represent does not get
 * dropped -- it ALIASES onto a live bucket, and because the stored hour then
 * mismatches, that bucket is zeroed and re-dated. A single months-old row
 * silently erased a whole hour of real CGM readings from TIR and the average,
 * and did it again on every restart while the row stayed in the tail. The
 * meter's first sync appends weeks of fingersticks at the END of the arrival
 * log, so stat_load replays exactly such rows last.
 *
 * The guards against that are pure boundary conditions, which is precisely
 * what a hand-check reads past. Built and run by `make statstest`.
 */
#include "stats.h"
#include "sensors.h" /* g_srec: seeded directly to drive the WARMUP rule */
#include "util.h"    /* realtime_s: stat_load uses the real clock */
#include <stdio.h>

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

/* A fixed, plausible "now" on an exact hour boundary. */
#define NOW (1700000000L - (1700000000L % 3600))

int main(void)
{
   int tir = -1;
   int avg = -1;

   /* SEED ENOUGH SPAN FOR THE 7-DAY WINDOW FIRST.
    *
    * stat_window refuses a window the data does not reach back across, and it
    * leaves the caller's tir/avg UNTOUCHED when it refuses. Without this seed
    * every 7-day assertion below compared two untouched -1s and passed no
    * matter what the code did -- verified by mutation: the aliasing and
    * future-timestamp guards could both be deleted with the suite still green.
    * 167 h is just inside the ring and just past the 166 h the 7-day window
    * needs. */
   stat_add_at(NOW - (167L * 3600), 100, NOW);

   printf("== TIR band and average arithmetic ==\n");
   /* The reading a day back exists only to make the window REPORTABLE (it sets
    * g_stat_oldest); it is deliberately NOT counted, because a "1 day" window
    * is 24 buckets -- nowh back to nowh-23 -- and that reading sits at nowh-24.
    * So the window holds exactly the four boundary samples.
    *
    * 70 and 180 are IN range, 69 and 181 are out: 2 of 4 = TIR 50, mean
    * (70+180+69+181)/4 = 125. Exact values, so shifting either boundary or
    * changing the divisor cannot pass -- excluding 70 or 180 would give 25 or
    * 0, including 69 or 181 would give 75 or 100. */
   stat_add_at(NOW - (24L * 3600), 100, NOW);
   stat_add_at(NOW, 70, NOW);
   stat_add_at(NOW, 180, NOW);
   stat_add_at(NOW, 69, NOW);
   stat_add_at(NOW, 181, NOW);
   ck(stat_window_at(1, &tir, &avg, NOW) == 1, "a 1-day window is reportable");
   ck(tir == 50, "70 and 180 in range, 69 and 181 not (TIR 50)");
   ck(avg == 125, "average is the mean of the samples in the window");

   printf("== a reading older than the ring is REJECTED, not aliased ==\n");
   {
      /* Put a known value in the current hour, then feed a reading exactly
       * STAT_HOURS old -- which maps to the SAME ring slot. If it is not
       * rejected, it zeroes and re-dates that bucket and the current hour's
       * data vanishes. */
      long h = NOW - (100L * 3600); /* a hour with nothing else in it */
      stat_add_at(h, 100, NOW);
      stat_add_at(h, 100, NOW);
      int before_t = -1;
      int before_a = -1;
      stat_window_at(7, &before_t, &before_a, NOW);
      /* Same ring slot, STAT_HOURS earlier. */
      stat_add_at(h - ((long)STAT_HOURS * 3600), 400, NOW);
      int after_t = -1;
      int after_a = -1;
      stat_window_at(7, &after_t, &after_a, NOW);
      ck(before_t == after_t && before_a == after_a,
         "an aliasing over-old reading changes nothing");
   }

   printf("== a timestamp that overflows the hour index is refused ==\n");
   {
      /* t/3600 narrowed to int overflows NEGATIVE, and the old age check
       * (nowh - hour, in int) overflowed a second time and wrapped negative,
       * so the guard passed and hour % STAT_HOURS indexed BEFORE the ring --
       * an out-of-bounds WRITE from one corrupt row, at every launch. */
      int before_t = -1;
      int before_a = -1;
      stat_window_at(7, &before_t, &before_a, NOW);
      stat_add_at(7730941136400L, 400, NOW); /* index would be -2047 */
      stat_add_at(900000000000000000L, 400, NOW);
      int after_t = -1;
      int after_a = -1;
      stat_window_at(7, &after_t, &after_a, NOW);
      ck(before_t == after_t && before_a == after_a,
         "an hour-overflowing timestamp is refused and changes nothing");
   }

   printf("== a future reading is rejected too ==\n");
   {
      /* The offset must WRAP the ring, or this proves nothing: a reading a few
       * hours ahead lands in a future bucket that no window ever reads, so
       * removing the guard changes no output. Verified by mutation -- the
       * first version of this test used +5 h and passed with the future check
       * deleted. Exactly one ring period ahead maps to the CURRENT hour's
       * slot, which is what a bad sensor clock would silently zero. */
      /* INSIDE the 7-day window (168 h) and distinct from the hour the
       * aliasing test above uses -- at 200 h back the bucket was outside the
       * window, so clobbering it changed no output and the test proved
       * nothing. */
      long h = NOW - (120L * 3600);
      stat_add_at(h, 100, NOW);
      stat_add_at(h, 100, NOW);
      int before_t = -1;
      int before_a = -1;
      stat_window_at(7, &before_t, &before_a, NOW);
      stat_add_at(h + ((long)STAT_HOURS * 3600), 400, NOW);
      int after_t = -1;
      int after_a = -1;
      stat_window_at(7, &after_t, &after_a, NOW);
      ck(before_t == after_t && before_a == after_a,
         "a future reading one ring-period ahead changes nothing");
   }

   printf("== a window is refused until the data spans it ==\n");
   ck(stat_window_at(90, &tir, &avg, NOW) == 0,
      "90 days is refused when only ~2 days exist");

   printf("== a pre-epoch timestamp cannot index before the ring ==\n");
   {
      /* C's % keeps the sign of the dividend, so hour = -2 gives index -2 --
       * an out-of-bounds WRITE. Reachable with an unset device clock, where
       * t = realtime_s() - age goes negative. Nothing here can assert on the
       * corruption directly; what it pins is that such a reading is refused,
       * so the stats are unchanged by it. */
      int before_t = -1;
      int before_a = -1;
      stat_window_at(7, &before_t, &before_a, NOW);
      stat_add_at(-7200, 400, 0); /* clock unset: negative timestamp */
      stat_add_at(-1, 400, 0);    /* just before the epoch */
      stat_add_at(0, 400, 0);     /* exactly the epoch */
      int after_t = -1;
      int after_a = -1;
      stat_window_at(7, &after_t, &after_a, NOW);
      ck(before_t == after_t && before_a == after_a,
         "pre-epoch readings are refused and change nothing");
      /* The assertion above cannot see the corruption itself -- an index of -2
       * writes outside every bucket a window reads, so it passed with the
       * guard deleted (verified by mutation). This one can: an ACCEPTED
       * pre-epoch reading would drag g_stat_oldest back to 1970, and window
       * reportability is measured from it, so every window would suddenly
       * claim to be covered. */
      int t90 = -1;
      int a90 = -1;
      ck(stat_window_at(90, &t90, &a90, NOW) == 0,
         "a refused pre-epoch reading does not make a 90-day window "
         "reportable");
   }

   printf("== a refused window leaves the caller's outputs untouched ==\n");
   {
      /* This block previously claimed to "pin" stat_window_at's `hour < 0`
       * guard. It did not, and the claim was proven false by mutation:
       * deleting that guard leaves the suite green, because with now = 0 the
       * loop indexes buckets that never match, cnt stays 0, and the function
       * returns 0 either way. An assertion that cannot discriminate is worse
       * than none -- it reads as coverage.
       *
       * The guard is genuinely unreachable from app data (the reportability
       * early return forces nowh >= hours-1), so no input exercises it; it
       * stays as defence in depth, honestly unpinned. What IS checkable, and
       * is checked here, is the contract every other assertion in this file
       * depends on: a refused window must not write through its out-params.
       * Without that, comparisons of before/after pairs elsewhere would be
       * comparing two untouched initialisers. */
      int t3 = -1;
      int a3 = -1;
      ck(stat_window_at(90, &t3, &a3, NOW) == 0,
         "a 90-day window is refused with only days of data");
      ck(t3 == -1 && a3 == -1, "...and tir/avg are left exactly as they were");
   }

   printf("== stat_load: the replay parser had NO coverage at all ==\n");
   /* Every mutant of stat_load survived before this block existed -- including
    * moving the cursor advance inside the digit cap, which is the exact
    * infinite-loop shape that shipped in sensors.c. A parser that runs at every
    * launch and no test executes is the same gap that made that bug possible.
    */
   {
      /* THIS BLOCK USES THE REAL CLOCK, not the file's fixed NOW.
       *
       * stat_load calls stat_add, which reads realtime_s() internally -- so
       * rows dated from a fixed constant years in the past are rejected as
       * older than the ring and nothing is loaded at all. The first version of
       * this test did exactly that and asserted on three identical numbers. */
      char path[64];
      (void)snprintf(path, sizeof path, "build/app/test/st-replay.csv");
      long rnow = realtime_s();
      long base = rnow - (10L * 3600);

      /* Span so the window is reportable, then measure the DELTA a load makes:
       * absolute values cannot be asserted because the ring is module-static
       * and earlier sections here have already populated it. */
      stat_add_at(rnow - (167L * 3600), 100, rnow);
      int t0 = -1;
      int a0 = -1;
      ck(stat_window_at(7, &t0, &a0, rnow) == 1, "the window is reportable");

      /* Nothing but BGM fingersticks at 400. A fingerstick is an irregular
       * point sample and people test precisely when they suspect a high or a
       * low, so counting it skews time-in-range; the exclusion must hold on
       * replay as well as live. */
      FILE *f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,3,0,0,1\n", base);
         fprintf(f, "%ld,400,1,,0,3,0,0,1\n", base + 1);
         fclose(f);
      }
      stat_load(path);
      int t1 = -1;
      int a1 = -1;
      stat_window_at(7, &t1, &a1, rnow);
      ck(a1 == a0 && t1 == t0,
         "BGM fingersticks in the log do not move TIR or the average");

      /* The same values as CGM must move them, or the check above proves
       * nothing. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,3,0,0,0\n", base + 2);
         fprintf(f, "%ld,400,1,,0,3,0,0,0\n", base + 3);
         fclose(f);
      }
      stat_load(path);
      int t2 = -1;
      int a2 = -1;
      stat_window_at(7, &t2, &a2, rnow);
      ck(a2 > a1, "...whereas CGM rows at 400 do raise the average");

      /* An absurd digit run in the kind field must terminate and not be read
       * as BGM. This is the exact infinite-loop shape that shipped in
       * sensors.c: a cursor advance moved inside a digit cap. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,100,1,,0,7,0,0,99999999999999999999\n", base + 4);
         fclose(f);
      }
      stat_load(path); /* must terminate */
      int t3 = -1;
      int a3 = -1;
      ck(stat_window_at(7, &t3, &a3, rnow) == 1,
         "an absurd kind field parses without hanging");

      /* WARMUP: a sensor's first hour is real data but UNCALIBRATED, so it is
       * shown and stored yet never counted. Confirmed live on 2026-08-07 -- a
       * fresh Stelo answered 0x4e with state=0x02, clock=495 and glucose=271
       * while the G7 beside it read 158, so counting warmup would have moved
       * the average by a value that was never true.
       *
       * The rule is per-sensor and anchored on sensor_rec.activation, and it
       * has to hold on BOTH paths: this exercises the replay half, which is
       * the one that had no coverage at all. */
      g_nsrec              = 1;
      g_srec[0].id         = 11;
      g_srec[0].activation = base + 100; /* session start */

      f = fopen(path, "w");
      if (f) {
         /* Inside [activation, activation + 3600) -- so base+100 up to
          * base+3699, NOT base+3599: the window is measured from activation,
          * not from `base`. Both ends, since a half-open window is exactly
          * where an off-by-one hides. */
         fprintf(f, "%ld,400,1,,0,11,0,0,0\n", base + 100);
         fprintf(f, "%ld,400,1,,0,11,0,0,0\n", base + 3699);
         fclose(f);
      }
      stat_load(path);
      int t4 = -1;
      int a4 = -1;
      stat_window_at(7, &t4, &a4, rnow);
      ck(a4 == a3 && t4 == t3,
         "a sensor's warmup hour does not move TIR or the average");

      /* One second past the window, the SAME sensor and value must count --
       * otherwise the assertion above passes for any reason at all. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,11,0,0,0\n", base + 3700);
         fclose(f);
      }
      stat_load(path);
      int t5 = -1;
      int a5 = -1;
      stat_window_at(7, &t5, &a5, rnow);
      ck(a5 > a4, "...and the second the warmup hour ends, it counts again");

      /* FAILS OPEN. An UNKNOWN source id resolves to no activation, and a
       * reading that cannot be proven to be warmup is ordinary data -- the
       * legacy rows in every existing log carry ids this test never seeded,
       * and dropping them from TIR would rewrite history on the next launch.
       * Same for a known sensor whose activation was never learned. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,99,0,0,0\n", base + 200); /* unknown id */
         fclose(f);
      }
      stat_load(path);
      int t6 = -1;
      int a6 = -1;
      stat_window_at(7, &t6, &a6, rnow);
      ck(a6 > a5, "an unknown sensor counts: the rule fails OPEN");

      g_srec[0].activation = 0; /* known sensor, session start never learned */
      f                    = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,11,0,0,0\n", base + 300);
         fclose(f);
      }
      stat_load(path);
      int t7 = -1;
      int a7 = -1;
      stat_window_at(7, &t7, &a7, rnow);
      ck(a7 > a6, "...as does a sensor whose activation is still unknown");
      g_nsrec = 0;
   }

   printf("\n%s\n", all ? "ALL STATS TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
