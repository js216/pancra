// SPDX-License-Identifier: GPL-3.0
// weighttest.c --- Host tests for the body-weight log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for weight.c.
 *
 * The unit conversion is the part that needs pinning. The file stores grams
 * and the user types tenths of their chosen display unit, so every entry
 * makes a round trip through two conversions -- and if that trip is not
 * exact, the number shown back is not the number typed. This codebase has
 * already been bitten by precisely that once (the plot-max entry rendered a
 * value the user could not re-type to reproduce), so it is asserted here for
 * every tenth across the whole plausible range rather than at a few points.
 *
 * Built and run by `make weighttest`.
 */
#include "weight.h"
#include "civil.h"
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

/* ---- A ZONE WITH TRANSITIONS IN IT, so a DST boundary can be crossed on
 * demand rather than waited for -----------------------------------------
 *
 * US/Pacific, by the post-2007 rule: daylight time from 02:00 local on the
 * second Sunday in March to 02:00 local on the first Sunday in November. The
 * two edges are what this suite is about:
 *
 *   SPRING FORWARD  local 02:00 PST -> 03:00 PDT. Local times in
 *                   [02:00, 03:00) that day NEVER HAPPENED.
 *   FALL BACK       local 02:00 PDT -> 01:00 PST. Local times in
 *                   [01:00, 02:00) that day happened TWICE, an hour apart.
 *
 * EVERY YEAR, not one hard-coded pair of dates: a fixture with a single
 * year's transitions in it says nothing about an edit that moves a timestamp
 * into a different year, which is exactly one of the edits the keypad makes.
 *
 * The offsets are seconds east of UTC and both are negative, which is the
 * sign that catches truncating division: -28800 is standard, -25200 daylight.
 */
#define STD (-28800L)
#define DST (-25200L)

/* Days since 1970-01-01, by COUNTING. Deliberately NOT the Hinnant formula
 * civil.c uses: ground truth computed with the code under test agrees with it
 * by construction, which is no test at all. */
static long day_of(int y, int m, int d)
{
   static const int L[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
   long n                 = 0;
   for (int yy = 1970; yy < y; yy++)
      n += 365 + ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0);
   int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
   for (int mm = 1; mm < m; mm++)
      n += L[mm - 1] + (mm == 2 && leap);
   return n + d - 1;
}

/* A local clock reading, as civil.h means it: not an instant. */
static long naive_at(int y, int m, int d, int hh, int mi)
{
   return (day_of(y, m, d) * 86400L) + (hh * 3600L) + (mi * 60L);
}

/* The n-th Sunday of a month. 1970-01-01 was a Thursday, so day 0 is weekday
 * 4 counting Sunday as 0. */
static int nth_sunday(int y, int m, int n)
{
   int dow   = (int)(((day_of(y, m, 1) + 4) % 7 + 7) % 7);
   int first = 1 + ((7 - dow) % 7);
   return first + (7 * (n - 1));
}

/* The transition INSTANTS, expressed through the offset in force just before
 * each -- which is how a zone file states them. */
static long spring_utc(int y)
{
   return naive_at(y, 3, nth_sunday(y, 3, 2), 2, 0) - STD;
}

static long fall_utc(int y)
{
   return naive_at(y, 11, nth_sunday(y, 11, 1), 2, 0) - DST;
}

static int zone_calls;

/* WHICH YEAR an instant is in, closely enough to pick the right pair of
 * transitions: the boundaries are in March and November, so a year taken from
 * a plain division is never off by enough to matter. */
static long pacific(void *ctx, long t)
{
   (void)ctx;
   zone_calls++;
   int y = 1970 + (int)(t / (365L * 86400L));
   for (int k = y - 1; k <= y + 1; k++)
      if (t >= spring_utc(k) && t < fall_utc(k))
         return DST;
   return STD;
}

static int all = 1;

/* Bytes on disk, or -1 when the file is not there. */
static long fsize(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

int main(void)
{
   weight_paths(test_dir()); /* the app's own path-building */
   unlink(weight_path());

   printf("== a partial header must never comment out the first row ==\n");
   /* THE DEFECT, exactly: the column header goes in ahead of the first row on
    * a brand-new file, and it was written best-effort. A SHORT write leaves
    * half a comment line with no newline -- and the first real weight is then
    * appended onto that same line, so the '#' comments the user's first entry
    * out of their own log. Nothing reports anything wrong.
    *
    * RLIMIT_FSIZE produces a genuine short write: the kernel writes up to the
    * limit, returns that count, and fails after it. SIGXFSZ is ignored so the
    * process survives to be tested. The assertion is on the FILE -- a failed
    * create must leave nothing behind, not a fragment for the next append to
    * land on. */
   {
      struct rlimit keep;
      getrlimit(RLIMIT_FSIZE, &keep);
      signal(SIGXFSZ, SIG_IGN);
      char tiny[160];
      test_path(tiny, sizeof tiny, "tiny");
      (void)mkdir(tiny, 0777);
      weight_paths(tiny);
      unlink(weight_path());

      struct rlimit small = {20, keep.rlim_max}; /* shorter than the header */
      if (setrlimit(RLIMIT_FSIZE, &small) == 0) {
         ck(weight_append(1700000000L, 70000L, 0) < 0,
            "an append that cannot write the header fails");
         /* NO FILE AT ALL, not an empty one: the first record is written to
          * a temporary and renamed, so a failure leaves nothing behind and
          * the retry below starts from a clean slate rather than appending
          * its row to a file that may already hold one. */
         ck(fsize(weight_path()) < 0, "...and leaves NO file behind at all");
         setrlimit(RLIMIT_FSIZE, &keep);

         /* The proof that it matters: the next append, with room again, must
          * produce a log whose first row is a row -- not a comment. */
         ck(weight_append(1700000000L, 70000L, 0) == 0,
            "with room again the append succeeds");
         ck(weight_load() == 0 && wt_count() == 1,
            "...and the first entry is READ BACK, not commented out");
         /* EXACTLY ONE. The failure above must not have left the row in the
          * file, or this retry -- which is what a caller does with a reported
          * failure -- would put a second copy of it in the log. */
         ck(wt_count() == 1, "...exactly once, with no duplicate from the "
                             "failed attempt");
         unlink(weight_path());
      } else {
         ck(0, "could not set RLIMIT_FSIZE for the short-write case");
      }
      setrlimit(RLIMIT_FSIZE, &keep);
      weight_paths(test_dir());
   }

   printf("== an equal-length edit is still a change worth syncing ==\n");
   /* THE DEFECT THIS PROVES. The sync scheduler asked "is there anything
    * new?" by adding up the SIZES of the synced files. A weight corrected
    * from 70.4 to 70.5 kg rewrites its row at exactly the same length, so
    * every size is identical and the phone concluded it had nothing to send
    * -- leaving the value the user had just fixed wrong on the server until
    * the six-hour safety sync, or until something else changed a size.
    *
    * The assertions are deliberately BOTH: that the file size really is
    * unchanged (so this is the case that used to be missed), and that the
    * mutation counter moved anyway. */
   {
      weight_paths(test_dir());
      unlink(weight_path());
      weight_load(); /* the tail follows the file: see weight.h */
      weight_append(1700000000L, 70400L, 0);
      weight_append(1700000600L, 70500L, 0);
      weight_load();
      struct wt_rec orig = wt_at(0);
      long size_before   = fsize(weight_path());
      long gen_before    = record_generation();

      /* Same number of digits: 70400 -> 70600. */
      ck(weight_update(&orig, orig.t, 70600L, 0) == 0, "an equal-length edit "
                                                       "succeeds");
      ck(fsize(weight_path()) == size_before,
         "...and the log is exactly as long as before (the missed case)");
      ck(record_generation() > gen_before,
         "...but the change is still announced to the sync scheduler");

      /* A DELETION changes the size, but it must announce itself too --
       * a delete and an append between two ticks can cancel out. */
      weight_load();
      struct wt_rec gone = wt_at(0);
      long gen_del       = record_generation();
      ck(weight_delete(&gone) == 0, "a delete succeeds");
      ck(record_generation() > gen_del, "...and is announced as well");
      unlink(weight_path());
   }

   printf("== a rewrite that fails leaves the ORIGINAL log intact ==\n");
   /* An edit or a delete rewrites the whole log to <path>.tmp and renames it
    * over the original. The failure that matters is running out of room
    * partway: the temporary must go, the original must stay, and the call
    * must say it failed. A rewrite that renamed a short temp into place would
    * silently delete every weight after the one being edited. */
   {
      struct rlimit keep;
      getrlimit(RLIMIT_FSIZE, &keep);
      signal(SIGXFSZ, SIG_IGN);
      weight_paths(test_dir());
      unlink(weight_path());
      weight_load(); /* the tail follows the file: see weight.h */
      for (int i = 0; i < 6; i++)
         weight_append(1700000000L + (i * 600L), 70000L + i, 0);
      long full = fsize(weight_path());
      ck(full > 0, "a log to rewrite");
      weight_load();
      struct wt_rec orig = wt_at(0);

      /* Room for a fraction of the rewrite, so it fails partway through. */
      struct rlimit small = {(rlim_t)(full / 2), keep.rlim_max};
      if (setrlimit(RLIMIT_FSIZE, &small) == 0) {
         ck(weight_update(&orig, orig.t, orig.g + 1, 0) < 0,
            "a rewrite that cannot be written whole fails");
         setrlimit(RLIMIT_FSIZE, &keep);
         ck(fsize(weight_path()) == full,
            "...and the original log is byte-for-byte intact");
         char tmp[320];
         snprintf(tmp, sizeof tmp, "%s.tmp", weight_path());
         ck(fsize(tmp) < 0, "...with no temporary left behind");
      } else {
         ck(0, "could not set RLIMIT_FSIZE for the rewrite case");
      }
      setrlimit(RLIMIT_FSIZE, &keep);
      unlink(weight_path());
   }

   printf("== what the loader REPORTS, not just what it parses ==\n");
   /* A loader that returns void cannot distinguish "no file yet" from "the
    * file could not be read", and the app then presents a silently short
    * record as a complete one -- the same defect store_load was fixed for.
    * These three cases are the whole contract. */
   {
      weight_paths(test_dir());
      unlink(weight_path());
      ck(weight_load() == 0, "a first run with no file is not an error");
      ck(wt_count() == 0, "...and loads nothing");

      /* A TRUNCATED LAST LINE IS DAMAGE, and this used to assert the
       * opposite -- "still reads whole" -- which is how a file cut in the
       * middle of an append came to be presented as a complete record. The
       * bytes may even parse: "1700000600,71" is a valid pair, and 71 grams
       * is not a weight, but "1700000600,71000" would have been. What makes
       * it damage is that the file ENDS without the newline that says the row
       * is finished. */
      FILE *f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "1700000600,71"); /* no newline, half a row */
         fclose(f);
      }
      ck(weight_load() < 0, "a file that ends mid-row is reported as "
                            "INCOMPLETE");
      ck(wt_count() >= 1,
         "...and the rows before it are kept, not thrown away");

      /* THE CASE THAT ACTUALLY PINS THE RULE: a final record that is
       * COMPLETE in every way except the newline. The row above is
       * syntactically broken, so a loader that merely rejected unparseable
       * rows would pass it -- and the defect this rule exists for is the
       * other one: bytes that parse perfectly and are still half a record,
       * because the file was cut between the last field and the newline that
       * says the row is finished. Withheld, not published, and reported. */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "%ld,70500,0", (long)1700000600); /* valid, no newline */
         fclose(f);
      }
      ck(weight_load() < 0, "a VALID final row with no newline is incomplete");
      ck(wt_count() == 1, "...and the prefix before it is kept whole");
      int published = 0;
      for (int i = 0; i < wt_count(); i++)
         if (wt_at(i).g == 70500)
            published = 1;
      ck(!published, "...while the unterminated row is NOT published");

      /* ...and the same file with the newline restored is whole, so the rule
       * is about the terminator and not about the row. */
      f = fopen(weight_path(), "a");
      if (f) {
         fputs("\n", f);
         fclose(f);
      }
      ck(weight_load() == 0, "the same row, terminated, reads whole");
      ck(wt_count() == 2, "...and IS published");

      /* A MALFORMED ROW in the middle: skipped (it is not a weight) and
       * reported (the history now has a hole in it, for ever -- this file is
       * never rewritten). */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "not-a-row\n");
         fprintf(f, "%ld,70500,0\n", (long)1700000600);
         fclose(f);
      }
      ck(weight_load() < 0, "a malformed row makes the load incomplete");
      ck(wt_count() == 2, "...and both good rows are still there");

      /* AN IMPOSSIBLE VALUE is the same case: 900 kg is not a weight, and a
       * row admitted once is drawn for ever. */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "%ld,900000,0\n", (long)1700000600);
         fclose(f);
      }
      ck(weight_load() < 0, "a value no scale produces makes it incomplete");
      ck(wt_count() == 1, "...and is not among the rows kept");

      /* AND THE OTHER END OF THE SAME BOUND. The ceiling above was pinned and
       * the floor was not, so WT_MIN_G could be lowered to zero -- or deleted
       * outright -- and every test here still passed. That mattered little
       * while the loader and the rewriter each held their own copy of the
       * check, since a slip in one was still a slip in one file; now there is
       * ONE reader for this row shape, so the floor loosened here is the floor
       * loosened in the loader, the edit and the delete alike. One gram under
       * it, so the test pins the constant and not merely its sign. */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "%ld,%ld,0\n", (long)1700000600, WT_MIN_G - 1);
         fclose(f);
      }
      ck(weight_load() < 0, "a weight under the floor makes it incomplete too");
      ck(wt_count() == 1, "...and it is not among the rows kept either");

      /* AN OVERLONG LINE: skipped rather than parsed as a truncation of
       * itself -- and said so. */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         for (int i = 0; i < 400; i++)
            fputc('7', f);
         fprintf(f, "\n%ld,70500,0\n", (long)1700000600);
         fclose(f);
      }
      ck(weight_load() < 0, "a line longer than any row makes it incomplete");
      ck(wt_count() == 2, "...and the rows either side survive");

      /* ...and a well-formed file is still whole, which is what stops all of
       * the above from being satisfied by returning -1 always. */
      f = fopen(weight_path(), "w");
      if (f) {
         fprintf(f, "# weight log\n");
         fprintf(f, "%ld,70000,0\n", (long)1700000000);
         fprintf(f, "%ld,70500,0\n", (long)1700000600);
         fclose(f);
      }
      ck(weight_load() == 0, "a whole file reads whole");
      ck(wt_count() == 2, "...with every row");

      /* ...AND ONE THE APP WROTE ITSELF. A rule that calls the app's own
       * files damaged would warn on every launch, and a warning that is
       * always there is a warning nobody reads. */
      unlink(weight_path());
      ck(weight_append(1700000000, 70000, 0) == 0, "a weight is logged");
      ck(weight_append(1700000600, 70500, 0) == 0, "...and another");
      ck(weight_load() == 0, "the file the app just wrote reads whole");
      ck(wt_count() == 2, "...with both weights");

      /* A READ THAT FAILS. open() on a directory succeeds and read() then
       * returns EISDIR -- a real errno from a real syscall, with no mocking
       * layer between the test and the code under test. */
      /* A SUBDIRECTORY OF THIS SUITE'S OWN TREE. It used to be "build/app",
       * shared by every build mode -- and this suite runs under ASan as well
       * as plain, so two processes created and rmdir'd build/app/weight.csv at
       * once. Not the fixture directory itself: the name staged here has to BE
       * a directory, while the cases either side need a real file called
       * weight.csv. */
      char eis[160];
      test_path(eis, sizeof eis, "eisdir");
      (void)mkdir(test_dir(), 0755);
      (void)mkdir(eis, 0777);
      weight_paths(eis);
      unlink(weight_path());
      if (mkdir(weight_path(), 0777) == 0 || access(weight_path(), F_OK) == 0) {
         ck(weight_load() < 0, "a read that FAILS is reported, not silently "
                               "treated as end of file");
         rmdir(weight_path());
      } else {
         ck(0, "could not stage the unreadable-file case");
      }
      weight_paths(test_dir());
      unlink(weight_path());
   }

   printf("== units: kilograms are exact ==\n");
   ck(wt_from_tenths(700, WT_KG) == 70000, "70.0 kg is 70000 g");
   ck(wt_to_tenths(70000, WT_KG) == 700, "...and back");
   ck(strcmp(wt_unit_name(WT_KG), "KG") == 0, "KG names itself");
   ck(strcmp(wt_unit_name(WT_LB), "LB") == 0, "LB names itself");

   printf("== units: pounds convert and ROUND TRIP ==\n");
   /* 154.0 lb is 69853 g. Truncating instead of rounding gave 69841, which
    * renders back as 153.9 -- a weight the user typed and cannot re-enter to
    * reproduce the same record. */
   ck(wt_from_tenths(1540, WT_LB) == 69853, "154.0 lb is 69853 g");
   ck(wt_to_tenths(69853, WT_LB) == 1540, "...and back to 154.0");

   printf("== EVERY typed weight survives the round trip ==\n");
   {
      int bad = 0;
      int n   = 0;
      for (int u = 0; u < 2; u++)
         for (int t = 1; t < 10000; t++) {
            long g = wt_from_tenths(t, u);
            if (g <= 0)
               continue; /* outside the stored range: refused, not converted */
            n++;
            if (wt_to_tenths(g, u) != t)
               bad++;
         }
      printf("  (checked %d values across both units)\n", n);
      ck(n > 5000, "the sweep actually covered the range");
      ck(bad == 0, "no typed weight renders back as a different number");
   }

   printf("== the stored range is enforced, not clamped ==\n");
   ck(wt_from_tenths(0, WT_KG) == 0, "zero is refused");
   ck(wt_from_tenths(-5, WT_KG) == 0, "negative is refused");
   ck(wt_from_tenths(10, WT_KG) == 0, "1.0 kg is below the floor, refused");
   ck(wt_from_tenths(9999, WT_KG) == 0, "999.9 kg is above the ceiling");
   /* REFUSE, never clamp: silently storing a different weight than the one
    * typed is worse than not storing it. */
   ck(wt_from_tenths(200, WT_KG) == 20000, "exactly the floor IS accepted");

   printf("== append, reload, order ==\n");
   ck(weight_append(1700000000L, 70000L, 0) == 0, "an entry appends");
   ck(wt_count() == 1 && wt_at(0).g == 70000, "...and lands in the tail");
   ck(weight_append(1699000000L, 69000L, 0) == 0, "a BACKDATED entry appends");
   ck(wt_count() == 2, "both are held");
   ck(wt_at(0).t < wt_at(1).t,
      "the tail is oldest-first even when entered out of order");
   weight_load();
   ck(wt_count() == 2 && wt_at(0).g == 69000 && wt_at(1).g == 70000,
      "a reload reproduces the same log");

   printf("== out-of-range rows are refused on the way IN ==\n");
   /* The file is loaded at every launch and never rewritten, so a corrupt row
    * admitted once is shown for good. */
   ck(weight_append(0, 70000L, 0) < 0, "a zero timestamp is refused");
   ck(weight_append(WT_T_MAX, 70000L, 0) < 0, "an absurd timestamp is refused");
   ck(weight_append(1700000001L, 5L, 0) < 0, "5 g is refused");
   ck(weight_append(1700000001L, 999999L, 0) < 0, "1 tonne is refused");
   ck(wt_count() == 2, "and none of them reached the tail");
   weight_load();
   ck(wt_count() == 2, "...nor the file");

   printf("== a corrupt file cannot wedge or resurrect a bad row ==\n");
   {
      FILE *f = fopen(weight_path(), "a");
      if (f) {
         fputs("garbage,not,numbers\n", f);
         fputs("99999999999999999999999,99999999999999999999\n", f);
         fputs("1700000002,70500\n", f);
         fclose(f);
      }
      weight_load(); /* must terminate */
      ck(wt_count() == 3, "the two bad rows are dropped, the good one kept");
      ck(wt_at(2).g == 70500, "and it is the one that parsed");
   }

   printf("== update rewrites the RIGHT row, and only that row ==\n");
   {
      unlink(weight_path());
      weight_load(); /* the tail follows the file: see weight.h */
      for (int i = 0; i < 5; i++)
         weight_append(1700000000L + i, 70000L + ((long)i * 100), 0);
      struct wt_rec orig = wt_at(2);
      ck(weight_update(&orig, orig.t, 71234L, 0) == 0, "an update succeeds");
      weight_load();
      ck(wt_count() == 5, "the row count is unchanged");
      ck(wt_at(2).g == 71234, "the target row carries the new value");
      ck(wt_at(0).g == 70000 && wt_at(4).g == 70400,
         "its neighbours are untouched");
      /* Matching is by CONTENT: an update whose original is not in the file
       * must change nothing rather than rewrite an arbitrary row. */
      struct wt_rec ghost = {1600000000L, 65000L};
      ck(weight_update(&ghost, 1600000000L, 66000L, 0) < 0,
         "an update for a row that is not there is refused");
      weight_load();
      ck(wt_count() == 5 && wt_at(2).g == 71234,
         "...and the file is unchanged");
      ck(weight_update(&orig, orig.t, 5L, 0) < 0,
         "an out-of-range update is refused");
   }

   printf("== delete removes exactly one row ==\n");
   {
      struct wt_rec d = wt_at(1);
      ck(weight_delete(&d) == 0, "a delete succeeds");
      weight_load();
      ck(wt_count() == 4, "one row fewer");
      for (int i = 0; i < wt_count(); i++)
         if (wt_at(i).t == d.t && wt_at(i).g == d.g)
            ck(0, "the deleted row is really gone");
      ck(weight_delete(&d) < 0, "deleting it again is refused");
      weight_load();
      ck(wt_count() == 4, "...and removes nothing else");
   }

   printf("== duplicates: the LAST match is the one touched ==\n");
   {
      unlink(weight_path());
      weight_load(); /* the tail follows the file: see weight.h */
      /* Two rows can be genuinely identical -- two weigh-ins recorded at the
       * same second. The rewrite must touch exactly one of them, or a delete
       * silently removes both. */
      weight_append(1700000000L, 70000L, 0);
      weight_append(1700000000L, 70000L, 0);
      weight_append(1700000100L, 71000L, 0);
      ck(wt_count() == 3, "three rows, two of them identical");
      struct wt_rec dup = {1700000000L, 70000L};
      ck(weight_delete(&dup) == 0, "deleting a duplicated row succeeds");
      weight_load();
      ck(wt_count() == 2, "exactly ONE row went, not both");
      int left = 0;
      for (int i = 0; i < wt_count(); i++)
         if (wt_at(i).t == dup.t && wt_at(i).g == dup.g)
            left++;
      ck(left == 1, "...and the other copy survived");
   }

   printf("== a BACKDATED row never evicts a NEWER one ==\n");
   /* THE DEFECT, in the words of the person holding the phone: they import a
    * month of weigh-ins from a scale app, or type in one they forgot to log
    * last week, and the entries from the last few days DISAPPEAR from the
    * table -- one per backdated row -- while the old imported ones sit there
    * instead. Nothing is lost from the file, so a restart brings the recent
    * ones back for as long as the tail has room again. It reads exactly like a
    * display bug that heals itself, which is the hardest kind to be believed
    * about.
    *
    * The cause was one line: overflow dropped element ZERO and left the sort
    * to tidy up afterwards, and element zero is whichever row was PUSHED
    * first. That is the oldest row only when the file happens to be in
    * chronological order, and a backdated row is precisely the case where it
    * is not. */
   {
      const long tb = 1700000000L;
      unlink(weight_path());
      weight_load(); /* the tail follows the file: see weight.h */
      for (int i = 0; i < NWT; i++)
         (void)weight_append(tb + i, 70000L + i, 0);
      ck(wt_count() == NWT, "a full tail");
      long oldest_held = wt_at(0).t;
      long newest_held = wt_at(NWT - 1).t;

      /* A weigh-in from a year ago, appended now. It is older than every row
       * held, so it is what a sort-then-drop-the-oldest would discard: it must
       * displace nobody. */
      ck(weight_append(tb - 31536000L, 65000L, 0) == 0,
         "a year-old weigh-in is logged");
      ck(wt_count() == NWT, "...the tail is still exactly full");
      /* THE ISOLATING ASSERTION. With arrival-order eviction the oldest row
       * HELD is the one that goes, and the backdated row takes its place --
       * so the tail loses a row newer than the one it gained. */
      ck(wt_at(0).t == oldest_held,
         "...and the oldest row HELD is still held: a backdated arrival does "
         "not evict a row newer than itself");
      int backdated_in_tail = 0;
      for (int i = 0; i < wt_count(); i++)
         if (wt_at(i).t == tb - 31536000L)
            backdated_in_tail = 1;
      ck(!backdated_in_tail,
         "...and the backdated row is not sitting in the tail having pushed a "
         "newer one out");
      /* NOT isolating on its own -- the newest row survives either way -- but
       * it is the row the user looks at, so it is asserted. */
      ck(wt_at(NWT - 1).t == newest_held,
         "...the newest weigh-in is untouched");

      /* AND IT IS IN THE FILE. The tail is a bounded VIEW; nothing here is
       * allowed to be a reason a logged weight is not recorded. */
      {
         FILE *f  = fopen(weight_path(), "rb");
         int rows = 0;
         char lb[128];
         while (f && fgets(lb, (int)sizeof lb, f))
            if (lb[0] != '#')
               rows++;
         if (f)
            fclose(f);
         ck(rows == NWT + 1, "every logged weight is in the file, including "
                             "the one the tail has no room for");
      }

      /* THE LOAD PATH, which the append path cannot express. During a load the
       * tail is NOT yet sorted -- wt_sort runs once at the end -- so element
       * zero is simply the first row of the file. Put the NEWEST row first and
       * arrival order and time order disagree completely. */
      {
         FILE *f = fopen(weight_path(), "w");
         ck(f != NULL, "a hand-built log opens");
         if (f) {
            /* the newest row of all, FIRST in the file */
            fprintf(f, "%ld,70000,0\n", tb + 100000L);
            for (int i = 1; i < NWT; i++)
               fprintf(f, "%ld,%ld,0\n", tb + i, 70000L + i);
            /* one more row, so the tail overflows exactly once */
            fprintf(f, "%ld,71000,0\n", tb + 200000L);
            fclose(f);
         }
         ck(weight_load() == 0, "it reads whole");
         ck(wt_count() == NWT, "...filling the tail");
         int newest_first_kept = 0;
         int true_oldest_kept  = 0;
         for (int i = 0; i < wt_count(); i++) {
            if (wt_at(i).t == tb + 100000L)
               newest_first_kept = 1;
            if (wt_at(i).t == tb + 1)
               true_oldest_kept = 1;
         }
         ck(newest_first_kept,
            "a row that ARRIVED first but is the NEWEST of all is kept");
         ck(!true_oldest_kept,
            "...and the row evicted is the oldest BY TIME, not by arrival");
      }
      unlink(weight_path());
      weight_load();
   }

   printf("== the weight-entry form ==\n");
   {
      /* THE RULE THAT MATTERS: a fresh form opens on the LAST weight, because
       * a weigh-in moves by ounces. Starting from zero would make every entry
       * a full retype -- the friction this form exists to remove. */
      struct wt_form f;
      wt_form_open(&f, 82400L, WT_KG, 1700000000L);
      ck(f.t == 1700000000L, "the form opens at now");
      ck(f.edit < 0, "...as a NEW entry");
      ck(f.tenths == wt_to_tenths(82400L, WT_KG),
         "...pre-populated with the last logged weight");

      /* The value is in tenths of the DISPLAY unit, so the same weight opens
       * as a different number in pounds -- which is what the keypad shows. */
      struct wt_form lb;
      wt_form_open(&lb, 82400L, WT_LB, 1700000000L);
      ck(lb.tenths == wt_to_tenths(82400L, WT_LB),
         "...in the display unit, not in grams");
      ck(lb.tenths != f.tenths, "...so kg and lb differ");

      /* With no history at all it must still offer something editable: an
       * empty field is worse than a plausible one. */
      struct wt_form first;
      wt_form_open(&first, 0, WT_KG, 1700000000L);
      ck(first.tenths > 0, "a first-ever entry still offers a real number");
   }

   printf("== a delete that removes the last line leaves evidence ==\n");
   /* WHY THIS MATTERS, and it is not about the weight log's contents.
    *
    * This log is SYNCED, and the phone is authoritative over the server's
    * copy: a log the phone no longer holds is an instruction to delete the
    * replica. The sync client refuses that instruction when it cannot tell a
    * deliberate emptying from a phone that lost its storage -- both are "a
    * log with no rows". So a delete that empties the file, with nothing said
    * about it, wedges the sync for ever: the deletion never converges, the
    * server keeps the copy, and sync_run stops at this log and takes the
    * readings and doses down with it.
    *
    * The '#' header normally keeps this file non-empty, so the case is a
    * HEADER-LESS log -- one written by an older build, or copied in by hand,
    * which is exactly the log a user would be surprised to find had broken
    * their backup. Written here by hand for that reason. */
   {
      weight_paths(test_dir());
      unlink(weight_path());
      char cp[600];
      (void)snprintf(cp, sizeof cp, "%s%s", weight_path(), LOG_CLEAR_SUFFIX);
      unlink(cp);
      {
         FILE *f = fopen(weight_path(), "wb");
         ck(f != NULL, "a header-less weight log can be written");
         if (f) {
            fputs("1700000000,70400,0\n", f);
            fclose(f);
         }
      }
      weight_load();
      ck(wt_count() == 1, "...and reads back as one entry");
      ck(log_clear_generation(weight_path()) == 0,
         "a log with a row in it is not deliberately empty");
      struct wt_rec last = wt_at(0);
      ck(weight_delete(&last) == 0, "deleting the only entry succeeds");
      ck(fsize(weight_path()) == 0, "...and leaves the log empty");
      ck(log_clear_generation(weight_path()) == 1,
         "...with the deletion recorded, so the sync may act on it");

      /* AND THE EVIDENCE DOES NOT OUTLIVE THE EMPTINESS. A weigh-in logged
       * afterwards makes the file non-empty; a tombstone left beside it would
       * authorise a LATER emptiness that nobody asked for. */
      ck(weight_append(1700000600L, 70500L, 0) == 0,
         "a weight is logged again");
      weight_load();
      struct wt_rec back = wt_at(0);
      ck(weight_update(&back, back.t, 70600L, 0) == 0,
         "...and a rewrite that keeps lines runs");
      {
         FILE *f = fopen(cp, "rb");
         ck(f == NULL, "...which dropped the tombstone: this log is not empty");
         if (f)
            fclose(f);
      }
      unlink(weight_path());
      unlink(cp);
   }

   printf("== a backdated weigh-in carries ITS OWN offset, not today's ==\n");
   /* TODO 131, at the end it is observable from: the tz_offset_s column. The
    * weight form split and recombined the entry's instant with g_tz_off --
    * the offset TODAY -- so a weigh-in moved to a date on the far side of a
    * DST boundary was stored an hour wrong AND stamped with today's offset,
    * which is the one field that could have shown it. Both halves are
    * checked: the instant, and the column that describes it. */
   {
      unlink(weight_path());

      /* Opened in November (PST) and backdated to a July morning (PDT), which
       * is what the WEIGHT keypad's date field does. */
      long t_nov = naive_at(2025, 11, 15, 8, 0) - STD;
      struct civil_res r =
          civil_reaim(t_nov, CIVIL_EDIT_MONTHDAY, 7, 4, pacific, 0);
      ck(r.t == naive_at(2025, 7, 4, 8, 0) - DST,
         "the backdated weigh-in lands on July's offset");
      ck(r.off == DST, "...and resolves July's offset to store with it");
      ck(r.off != pacific(0, t_nov),
         "...which is NOT the offset in force when the form was opened");

      ck(weight_append(r.t, 70000L, r.off) == 0, "it is logged");
      {
         char row[128] = {0};
         FILE *f       = fopen(weight_path(), "rb");
         ck(f != NULL, "the log opens");
         if (f) {
            /* Past the '#' header line to the row itself. */
            while (fgets(row, sizeof row, f))
               if (row[0] != '#')
                  break;
            fclose(f);
         }
         char want[128];
         (void)snprintf(want, sizeof want, "%ld,%ld,%ld\n", r.t, 70000L, DST);
         ck(!strcmp(row, want),
            "...with the TARGET date's offset in its tz_offset_s column");
      }

      /* The WEIGHT form's other two fields, on the same instant. */
      struct civil_res yr =
          civil_reaim(r.t, CIVIL_EDIT_YEAR, 2026, 0, pacific, 0);
      ck(yr.t == naive_at(2026, 7, 4, 8, 0) - DST,
         "changing only the year keeps the civil time and re-resolves it");
      struct civil_res tm =
          civil_reaim(r.t, CIVIL_EDIT_TIME, 23, 45, pacific, 0);
      ck(tm.t == naive_at(2025, 7, 4, 23, 45) - DST,
         "changing only the time keeps the civil DATE it was resolved on");
      unlink(weight_path());
   }

   printf("== the two transition hours, on the weight form's own fields ==\n");
   {
      /* AMBIGUOUS: the repeated hour resolves to the earlier instant and says
       * so, rather than picking one silently. */
      long base = naive_at(2025, 11, 2, 12, 0) - STD;
      struct civil_res amb =
          civil_reaim(base, CIVIL_EDIT_TIME, 1, 15, pacific, 0);
      ck(amb.fix == CIVIL_AMBIGUOUS, "01:15 on the fall-back day is ambiguous");
      ck(amb.t == naive_at(2025, 11, 2, 1, 15) - DST,
         "...and the earlier of the two instants is what is stored");
      ck(amb.t_alt == naive_at(2025, 11, 2, 1, 15) - STD,
         "...with the other one reported, not discarded");

      /* NONEXISTENT: the skipped hour moves forward by the gap. */
      long sp = naive_at(2025, 3, 9, 12, 0) - DST;
      struct civil_res gap =
          civil_reaim(sp, CIVIL_EDIT_TIME, 2, 15, pacific, 0);
      ck(gap.fix == CIVIL_NONEXISTENT,
         "02:15 on the spring-forward day never existed");
      ck(gap.t + gap.off == naive_at(2025, 3, 9, 3, 15),
         "...so the entry redisplays as 03:15, forward by the gap");
      ck(gap.t != naive_at(2025, 3, 9, 2, 15) - DST,
         "...and not an hour BACKWARD, which taking it as valid would give");
   }

   printf("\n%s\n", all ? "ALL WEIGHT TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
