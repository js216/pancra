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
#include "clock.h"
#include "sensors.h" /* g_srec: seeded directly to drive the WARMUP rule */
#include "store.h"   /* THE OTHER READER of readings.csv: see the last block */
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include <stdio.h>
#include <sys/stat.h> /* mkdir: a directory where the log should be */
#include <unistd.h>   /* unlink: the registry files this test stages */

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all  = 1;
static int nass = 0;

static void ck(int cond, const char *what)
{
   nass++;
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
      char path[160];
      test_path(path, sizeof path, "st-replay.csv");
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
      /* THROUGH THE REGISTRY'S OWN MINT, not by writing its array: the id a
       * sensor gets is the registry's to choose, and a test that assigns one
       * by hand is testing a state the app cannot produce. The rows below
       * cite whatever id it hands back. */
      sensors_paths(test_dir());
      unlink(sensors_path());
      unlink(slots_path());
      sensors_load();
      int wid = sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:11", "", "", "",
                            base + 100); /* activation = session start */
      ck(wid > 0, "a sensor with a known activation is registered");

      f = fopen(path, "w");
      if (f) {
         /* Inside [activation, activation + 3600) -- so base+100 up to
          * base+3699, NOT base+3599: the window is measured from activation,
          * not from `base`. Both ends, since a half-open window is exactly
          * where an off-by-one hides. */
         fprintf(f, "%ld,400,1,,0,%d,0,0,0\n", base + 100, wid);
         fprintf(f, "%ld,400,1,,0,%d,0,0,0\n", base + 3699, wid);
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
         fprintf(f, "%ld,400,1,,0,%d,0,0,0\n", base + 3700, wid);
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

      /* A KNOWN SENSOR WHOSE ACTIVATION IS NOT YET LEARNED: registered bare,
       * which is exactly what a CGM is between the user committing to it and
       * its first reading anchoring the session clock. Minted here rather
       * than by zeroing the row, because "bare" is a state the registry
       * produces on its own. */
      int bid = sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:12", "", "", "", 0);
      ck(bid > 0, "a sensor with no known activation is registered");
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,%d,0,0,0\n", base + 300, bid);
         fclose(f);
      }
      stat_load(path);
      int t7 = -1;
      int a7 = -1;
      stat_window_at(7, &t7, &a7, rnow);
      ck(a7 > a6, "...as does a sensor whose activation is still unknown");
   }

   /* ---- THE REBUILD A RESTORE PERFORMS ----
    *
    * WHAT THE USER SAW. They reinstall, tap RESTORE, and their record comes
    * back: the history list fills and the plot draws a month of readings. The
    * TIR and AVERAGE printed beside that plot do not change -- they are still
    * the fresh install's own handful of readings, or "--" -- because
    * stat_load ran once at startup and pancra_logs_reload never touched the
    * buckets. Two figures on one screen describing the same data and
    * disagreeing, with the wrong pair looking exactly as authoritative as the
    * right one, until the app was restarted.
    *
    * THIS BLOCK GOES LAST, deliberately: stat_reload_publish REPLACES the
    * ring, so every earlier section's data is gone after it.
    *
    * The assertions are ABSOLUTE, not deltas, and that is what makes them
    * mean something. Every earlier section has already fed this
    * module-static ring -- including a pile of CGM rows at 400 -- so a
    * rebuild that merely ADDED the restored rows on top (which is what
    * calling stat_load alone would do) could not possibly produce the exact
    * TIR and average of the restored file. Asserting the exact pair is
    * therefore an assertion that the ring was CLEARED first. */
   printf("== a RESTORE rebuilds the statistics from the restored log ==\n");
   {
      char path[160];
      test_path(path, sizeof path, "st-restored.csv");
      /* The real clock, for the same reason the block above uses it:
       * stat_load feeds stat_add, which reads realtime_s() internally, so
       * rows dated from this file's fixed NOW would all be rejected as older
       * than the ring and nothing would load at all. */
      long rnow = realtime_s();
      /* 167 h back is just inside the ring and exactly what the 7-day window
       * needs to be reportable (7*86400 - 3600 == 167*3600). */
      long old    = rnow - (167L * 3600);
      long recent = rnow - 3600;

      /* RECORD A: two in-range readings. TIR 100, average 100. */
      FILE *f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,100,1,,0,0,0,0,0\n", old);
         fprintf(f, "%ld,100,1,,0,0,0,0,0\n", recent);
         fclose(f);
      }
      ck(stat_reload_prepare(path) == 1, "the rebuild parses the restored log");
      stat_reload_publish();
      int ta = -1;
      int aa = -1;
      ck(stat_window_at(7, &ta, &aa, rnow) == 1,
         "after the rebuild the 7-day window is reportable");
      ck(aa == 100,
         "the average is the RESTORED log's, not the pre-restore one");
      ck(ta == 100, "...and so is time-in-range");

      /* RECORD B: the same two hours, different values, both OUT of range.
       * TIR 0, average 300. If the rebuild did not clear, record A's two
       * 100s would still be in these buckets and the average would be 200. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,400,1,,0,0,0,0,0\n", old);
         fprintf(f, "%ld,200,1,,0,0,0,0,0\n", recent);
         fclose(f);
      }
      ck(stat_reload_prepare(path) == 1, "...and parses the second one");
      stat_reload_publish();
      int tb = -1;
      int ab = -1;
      ck(stat_window_at(7, &tb, &ab, rnow) == 1, "a second rebuild reports");
      ck(ab == 300, "the previous record's readings are GONE, not merged");
      ck(tb == 0, "...and its time-in-range with them");

      /* IDEMPOTENT / ORDER-INDEPENDENT: restoring A again after B must land
       * back on exactly A's numbers. The result depends on the FILE and on
       * nothing that happened to the ring before it -- which is the whole
       * property "idempotent locked rebuild" is naming. */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,100,1,,0,0,0,0,0\n", old);
         fprintf(f, "%ld,100,1,,0,0,0,0,0\n", recent);
         fclose(f);
      }
      ck(stat_reload_prepare(path) == 1, "...and the first one again");
      stat_reload_publish();
      int tc = -1;
      int ac = -1;
      ck(stat_window_at(7, &tc, &ac, rnow) == 1, "a third rebuild reports");
      ck(ac == aa && tc == ta,
         "reloading the same record twice over lands on the same numbers");

      /* AND THE SAME FILE TWICE IN A ROW, which is the literal reading of
       * "idempotent": a restore retried after a dropped connection must not
       * count the record twice. (Doubling a multiset leaves the mean alone,
       * so this cannot discriminate on its own -- it is here because the
       * retry is a real sequence, and the assertions above are what pin the
       * clear.) */
      stat_reload_prepare(path);
      stat_reload_publish();
      int td = -1;
      int ad = -1;
      stat_window_at(7, &td, &ad, rnow);
      ck(ad == ac && td == tc, "rebuilding twice over is the same as once");

      /* THE SPAN COMES BACK WITH THE BUCKETS, or a window reports over data
       * the restored record does not contain.
       *
       * g_stat_oldest is what stat_window_at measures REPORTABILITY from.
       * Publishing rebuilt buckets while keeping the previous value lets a
       * 7-day figure be computed from a day of restored readings and printed
       * as a 7-day figure -- the same class of lie as the stale average, and
       * harder to notice because the number LOOKS reasonable. Verified by
       * mutation: keeping the old oldest survived every assertion above,
       * because they all restore records that happen to span far enough.
       *
       * 25 h so the pair discriminates in both directions: it is enough for
       * the 1-day window (which needs 23 h) and nowhere near the 7-day one
       * (which needs 167 h). */
      f = fopen(path, "w");
      if (f) {
         fprintf(f, "%ld,120,1,,0,0,0,0,0\n", rnow - (25L * 3600));
         fprintf(f, "%ld,120,1,,0,0,0,0,0\n", rnow - 3600);
         fclose(f);
      }
      stat_reload_prepare(path);
      stat_reload_publish();
      int t1d = -1;
      int a1d = -1;
      ck(stat_window_at(1, &t1d, &a1d, rnow) == 1,
         "a short restored record still reports the window it does cover");
      ck(a1d == 120 && t1d == 100, "...from its own rows and nothing else");
      int t7d = -1;
      int a7d = -1;
      ck(stat_window_at(7, &t7d, &a7d, rnow) == 0,
         "...and REFUSES the window it does not: the span is rebuilt too");

      /* A LOG WE COULD NOT READ IS NOT THE SAME AS A LOG THAT IS NOT THERE,
       * and the two must not produce the same answer.
       *
       * A missing file is a COMPLETE answer -- "there are no readings" -- and
       * the case below asserts it clears the figures, which is right: keeping
       * them would describe a record the app no longer has. A read that FAILS
       * partway is not an answer at all: the buckets hold a prefix of the
       * file, so publishing them puts a TIR and an average on screen that
       * describe part of a record, beside a history store_load has (for the
       * same reason) refused to replace. Then the two numbers disagree again,
       * which is the whole defect this rebuild exists to remove.
       *
       * A DIRECTORY where the log should be: open() succeeds on it and read()
       * fails with EISDIR, which is a genuine mid-load read failure and not
       * something a test can otherwise ask a healthy filesystem for. */
      char baddir[160];
      test_path(baddir, sizeof baddir, "st-unreadable");
      unlink(baddir);
      rmdir(baddir);
      if (mkdir(baddir, 0700) != 0)
         ck(0, "the unreadable-log fixture could not be staged");
      int badprep = stat_reload_prepare(baddir);
      ck(badprep == 0, "a log that cannot be READ prepares nothing");
      stat_reload_publish(); /* must be a no-op: there is nothing prepared */
      int tk = -1;
      int ak = -1;
      ck(stat_window_at(1, &tk, &ak, rnow) == 1,
         "...so the previous record's figures are still reportable");
      ck(ak == a1d && tk == t1d, "...and are exactly the ones it had");
      rmdir(baddir);

      /* A LOG THAT IS NOT THERE LEAVES NO NUMBERS BEHIND. Keeping the
       * previous ones would be the original bug in its purest form: figures
       * on screen that describe a record the app no longer has. "There are no
       * readings" is a COMPLETE answer, so this is a successful rebuild of an
       * empty record -- not the failure the directory above is -- and the two
       * must not be confused, or a missing log would preserve stale figures
       * for ever. */
      char gone[160];
      test_path(gone, sizeof gone, "st-no-such-log.csv");
      unlink(gone);
      int goneprep = stat_reload_prepare(gone);
      ck(goneprep == 1, "a log that is NOT THERE is a complete answer");
      stat_reload_publish();
      /* THE ONE-DAY WINDOW, not the seven-day one. The record before this was
       * 25 h long, so its 7-day window was ALREADY unreportable and an
       * assertion on it passes whether or not the empty rebuild was published
       * -- verified by mutation, which is how this was found: treating a
       * missing log as a failure survived the whole suite. The 1-day window
       * was reportable a moment ago, so it discriminates. */
      int t1e = -1;
      int a1e = -1;
      ck(stat_window_at(1, &t1e, &a1e, rnow) == 0,
         "an absent log leaves no stale TIR/average behind");
      ck(t1e == -1 && a1e == -1,
         "...and writes nothing through the out-params");
      int te = -1;
      int ae = -1;
      ck(stat_window_at(7, &te, &ae, rnow) == 0,
         "...and the longer windows are refused too");
   }


   /* ---- THE TWO READERS OF ONE FILE, ROW BY ROW ----
    *
    * readings.csv has two loaders that matter to the user, and they are
    * written in different files by different hands: store_load builds the
    * HISTORY the plot and the reading list are drawn from, and stat_load
    * builds the TIR and AVERAGE printed beside them. A row one takes and the
    * other refuses puts two figures on one screen describing the same file
    * and disagreeing -- which is the failure store.c's bounds comments cite
    * three times, each time after it had already happened.
    *
    * So the verdicts are asserted TOGETHER, over one table of rows fed to
    * both. Asserting either alone pins one parser's habits; asserting the
    * pair fails when EITHER drifts, which is the property that has to hold.
    *
    * The statistics parser used to be four hand-rolled digit loops and a
    * field walker that treated a missing separator as end-of-row, so it read
    * `<epoch>,100junk` as a glucose of 100 -- the leading digits of a field
    * that says something else -- and folded it into both figures. It now
    * reads every column through csvcur.h, the same bounded cursor
    * plotdata.c, sensors.c, insulin.c and weight.c read theirs through.
    */
   printf("== history and statistics judge the same rows ==\n");
   {
      /* A DIRECTORY OF ITS OWN. store_paths always names the log
       * readings.csv, and the fixture directory is shared with every other
       * suite in the tree -- storetest's log has that exact name. */
      char dir[160];
      test_path(dir, sizeof dir, "st-agree");
      mkdir(dir, 0700);
      ck(store_paths(dir) == 1, "the two readers are pointed at one log");

      long rnow = realtime_s();
      long anch = rnow - (25L * 3600); /* spans the 1-day window */
      long live = rnow - 3600;         /* inside it */
      long cast = rnow - 1800;         /* the row under test, inside it */

      /* THE ANCHOR PAIR IS 400 AND THE CASE ROW IS 100, so "counted" is
       * visible in the average: 400 alone, 250 if the row was taken. A case
       * row sharing the anchors' value could not be seen at all. */
      struct rowcase {
         const char *what;
         const char *tail; /* what follows the timestamp on the line */
         int nl;           /* terminate it: a torn append has no newline */
         int hist;        /* the history keeps it */
         int stat;        /* the statistics count it */
      };
      static const struct rowcase cases[] = {
          {"a complete row", ",100,1,,0,0,0,0,0", 1, 1, 1},
          /* v1 rows predate the registry and carry two columns. The log is
           * append-only, so they are still in every real file. */
          {"a v1 two-column row", ",100", 1, 1, 1},
          /* THE TWO ROWS THE READERS JUDGE DIFFERENTLY, and the direction
           * matters. Their glucose (or source) column is not a number: it is
           * a number with something else attached, so its leading digits are
           * not what the row says. store.c's own field reader takes them --
           * it keeps the digits and then stalls, which for the second row
           * silently gives the reading source 0 and kind 0 whatever the file
           * said -- and store.c is not this change's to alter. Statistics
           * refuse them, and that is the safe side of the disagreement: one
           * excluded row moves an aggregate over thousands by nothing, while
           * one INVENTED value counted into a figure with no per-row detail
           * behind it cannot be seen at all. The pair is pinned here so that
           * tightening store.c's reader to match is a visible, deliberate
           * change to this table rather than a silent one. */
          {"junk after the glucose digits", ",100junk", 1, 1, 0},
          {"junk after the source digits", ",100,1,,0,7x,0,0,0", 1, 1, 0},
          {"two columns run together", "100,100", 1, 0, 0},
          {"a torn final row with no newline", ",100,1,,0,0,0,0,0", 0, 0, 0},
          {"a glucose too long to hold", ",99999999999999999999", 1, 0, 0},
          {"an empty glucose column", ",,1,,0,0,0,0,0", 1, 0, 0},
          /* NOT a parse refusal: a fingerstick is a real, kept reading that
           * time-in-range deliberately excludes. It is here because it is
           * the case that proves the columns are still COUNTED correctly
           * past the empty rssi one -- kind is field 9, and a decoder that
           * mishandled the absent field would read it from the wrong
           * column. */
          {"a fingerstick, kept but not counted", ",100,1,,0,0,0,0,1", 1, 1,
           0},
          /* A FRACTIONAL RESCALE, which is what the last column actually
           * holds. This is the row shape that broke the statistics in the
           * field: csv_num stops AT the '.', so the separator test then found
           * a '.' where it wanted a ',' and threw the whole reading away.
           *
           * Every row written since a rescale factor was first stored was
           * dropped -- 4,189 of 42,000 on a real log -- while the plot, which
           * never walks the tail, drew them all. The screen showed a full
           * trace beside a 1D time-in-range computed only from what the LIVE
           * path had added since launch: 100% on a day that was 90%.
           *
           * The value itself is not read; what matters is that the ROW
           * survives, so its glucose reaches the buckets. */
          {"a fractional rescale in the last column", ",100,1,,0,0,0,0,0,0.830",
           1, 1, 1},
          /* ...and a fraction in a column that IS read, so the skip is not
           * quietly changing which field is which. src is field 6. */
          {"a fraction mid-row still lands in the right columns",
           ",100,1,,0,0.5,0,0,1", 1, 1, 0},
      };
      int safe = 1; /* nothing counted that the history does not hold */
      for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
         FILE *f = fopen(store_path(), "w");
         if (!f) {
            ck(0, "the shared log could not be written");
            break;
         }
         fprintf(f, "%ld,400,1,,0,0,0,0,0\n", anch);
         fprintf(f, "%ld,400,1,,0,0,0,0,0\n", live);
         fprintf(f, "%ld%s", cast, cases[i].tail);
         if (cases[i].nl)
            fputs("\n", f);
         fclose(f);

         hist_clear();
         store_load(-1);
         int in_hist = 0;
         for (int k = 0; k < hist_count(); k++)
            if (hist_at(k).t == cast)
               in_hist = 1;

         stat_reload_prepare(store_path());
         stat_reload_publish();
         int tir      = -1;
         int avg      = -1;
         int reported = stat_window_at(1, &tir, &avg, rnow);
         /* The two anchors alone give 400; the case row at 100 pulls it to
          * 250. A window that cannot be reported at all is neither, and
          * would make every comparison below vacuous. */
         int in_stat = reported && avg != 400;
         ck(reported == 1, "the window is reportable for every case");

         char msg[192];
         (void)snprintf(msg, sizeof msg, "the history %s: %s",
                        cases[i].hist ? "KEEPS" : "refuses", cases[i].what);
         ck(in_hist == cases[i].hist, msg);
         (void)snprintf(msg, sizeof msg, "the statistics %s: %s",
                        cases[i].stat ? "COUNT" : "refuse", cases[i].what);
         ck(in_stat == cases[i].stat, msg);
         if (in_stat && !in_hist)
            safe = 0;
      }
      /* THE INVARIANT, stated over the whole table rather than case by case:
       * a figure computed from rows the app claims not to have is the defect
       * itself, whatever shape the row had. The reverse -- a row kept but not
       * counted -- is legitimate and the table holds three of them: a
       * fingerstick, which time-in-range excludes on purpose, and the two
       * contaminated rows store.c's looser field reader still takes. */
      ck(safe, "no row is counted into TIR that the history does not hold");
   }

   /* ---- THE CONTROL: VALID ROWS ARE UNTOUCHED BY ALL OF THE ABOVE ----
    *
    * A parser made stricter is one edit away from dropping rows it should
    * keep, and NOTHING ELSE IN THIS FILE WOULD SAY SO: every TIR and average
    * here would simply be computed over fewer readings and still look like a
    * reasonable pair of numbers. So this rebuilds from a log of ordinary
    * rows -- one of each shape the app actually writes -- and asserts the
    * exact figures, the same 70/180/69/181 boundary set the arithmetic
    * section at the top of this file uses. */
   printf("== valid rows still produce exactly the same figures ==\n");
   {
      char path[160];
      test_path(path, sizeof path, "st-control.csv");
      long rnow = realtime_s();
      long anch = rnow - (25L * 3600);
      FILE *f   = fopen(path, "w");
      if (f) {
         fprintf(f, "# unix_time,glucose_mgdl,trend\n"); /* the header row */
         fprintf(f, "%ld,100,1,,0,0,0,0,0\n", anch);     /* span, not counted */
         /* The full modern row, the row with an empty rssi column, a row
          * with a trailing separator and a v1 two-column row: every shape
          * this log has ever held, all four counted. */
         fprintf(f, "%ld,70,1,-70,0,0,0,0,0\n", rnow - 3600);
         fprintf(f, "%ld,180,1,,0,0,0,0,0\n", rnow - 3500);
         fprintf(f, "%ld,69,1,,0,0,0,0,0,\n", rnow - 3400);
         fprintf(f, "%ld,181\n", rnow - 3300);
         fclose(f);
      }
      ck(stat_reload_prepare(path) == 1, "the control log parses");
      stat_reload_publish();
      int tir = -1;
      int avg = -1;
      ck(stat_window_at(1, &tir, &avg, rnow) == 1, "...and reports");
      /* 70 and 180 are in the consensus band, 69 and 181 are not: 2 of 4.
       * Exact values, so a dropped row cannot pass -- losing any one of the
       * four moves both figures. */
      ck(tir == 50, "TIR over valid rows is unchanged (50)");
      ck(avg == 125, "...and so is the average (125)");
   }

   printf("\n%d assertions\n", nass);
   printf("%s\n", all ? "ALL STATS TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
