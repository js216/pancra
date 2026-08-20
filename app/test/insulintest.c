// SPDX-License-Identifier: GPL-3.0
// insulintest.c --- Host tests for the insulin dose log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for insulin.c. A dose is a user-entered fact in an
 * append-only file the app reloads at every launch, so the properties that
 * matter are: what was confirmed is durably on disk, what loads back is
 * exactly what was written, a corrupt row can never load as a plausible dose,
 * the tail is TIME-SORTED however doses were entered or edited, and the
 * form's pre-population (last units per type) follows dose time.
 *
 * Built and run by `make insulintest`, which `make check` depends on. */
#include "insulin.h"
#include "civil.h"
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h"
#include <stdio.h>
#include <string.h> /* memset: the over-long-row regression builds one */
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

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* The append-only property is checked by SIZE: an edit that shrank or held
 * the file steady would mean the log had been rewritten. */
static long fsize(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   (void)fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fclose(f);
   return n;
}

static void fresh(void)
{
   insulin_paths(test_dir()); /* the app's own path-building */
   unlink(insulin_path());
   insulin_load();
}

int main(void)
{
   const long t0 = 1700000000;

   printf("== a corrected dose is a change worth syncing ==\n");
   /* The clinical version of the same defect: a dose corrected from 12 units
    * to 13 rewrites the row at the same length, so the file sizes the sync
    * scheduler compared were identical and the correction was not sent. */
   {
      insulin_paths(test_dir());
      unlink(insulin_path());
      insulin_append(1700000000L, 0, 12, 0);
      long gen_before     = record_generation();
      struct ins_rec orig = ins_at(0);
      ck(insulin_update(&orig, orig.t, orig.type, 13, 0) == 0,
         "a same-length dose correction succeeds");
      ck(record_generation() > gen_before,
         "...and is announced to the sync scheduler");
      long gen_del        = record_generation();
      struct ins_rec gone = ins_at(0);
      ck(insulin_delete(&gone) == 0, "a retraction succeeds");
      ck(record_generation() > gen_del, "...and is announced too");
      unlink(insulin_path());
   }

   printf("== what the loader REPORTS, not just what it parses ==\n");
   /* Same contract as weight and readings: a first run, a truncated tail and
    * a read that FAILS are three different answers, and only the third means
    * the doses on screen are short. */
   {
      insulin_paths(test_dir());
      unlink(insulin_path());
      ck(insulin_load() == 0, "a first run with no file is not an error");
      ck(ins_count() == 0, "...and loads nothing");

      /* written,id,del,unix_time,type,units,tz_offset_s -- one good row, then
       * a row that stops mid-number because the process died writing it. */
      FILE *f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "1700000600,2,0,17000006"); /* half a row, no newline */
         fclose(f);
      }
      /* A FILE CUT MID-APPEND IS DAMAGE. This asserted the opposite --
       * "still reads whole" -- which is how a dose log missing its most
       * recent entries came to be presented as the record of what was
       * injected. The prefix is still kept; what changes is that the app is
       * told. */
      ck(insulin_load() < 0, "a file that ends mid-row is reported as "
                             "INCOMPLETE");
      ck(ins_count() >= 1,
         "...and the rows before it are kept, not thrown away");

      /* THE CASE THAT ACTUALLY PINS THE RULE: a final assertion that is
       * COMPLETE except for the newline. The row above stops mid-number, so a
       * loader that only rejected unparseable rows would pass it -- and the
       * defect this rule exists for is the other one: seven fields that parse
       * perfectly and are still half a record, because the file was cut
       * before the newline. A dose that was never given, replayed at every
       * launch, is the worst row this app can invent. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "1700000600,2,0,1700000600,1,5,0"); /* valid, no newline */
         fclose(f);
      }
      ck(insulin_load() < 0, "a VALID final row with no newline is "
                             "incomplete");
      ck(ins_count() == 1, "...and the prefix before it is kept whole");
      int published = 0;
      for (int i = 0; i < ins_count(); i++)
         if (ins_at(i).units == 5)
            published = 1;
      ck(!published, "...while the unterminated dose is NOT published");

      /* ...and terminated, the same row is whole and does appear. */
      f = fopen(insulin_path(), "a");
      if (f) {
         fputs("\n", f);
         fclose(f);
      }
      ck(insulin_load() == 0, "the same row, terminated, reads whole");
      ck(ins_count() == 2, "...and IS published");

      /* AN EMPTY RETRACTION FLAG IS NOT "NOT RETRACTED".
       *
       * Every other column of this row is protected by a range check -- an
       * absent `t` reads 0 and fails `t <= 0`, an absent `type` is neither
       * SLOW nor FAST -- but `del` reads 0, and 0 is the valid and by far the
       * commoner answer. So a row truncated or corrupted in exactly that one
       * byte used to load as a live dose, silently RESURRECTING a dose the
       * user had deleted. This file is append-only and replayed at every
       * launch, so it would come back every time.
       *
       * The file below is the whole story in three rows: the dose is logged,
       * it is RETRACTED, and then a row whose only fault is an empty flag
       * names it again. Read as del == 0 that third row is an assertion that
       * the dose exists, so the deleted dose comes back -- which is why the
       * count below is the assertion that matters, and why the fixture needs
       * the retraction in the middle: without it, a third row read as del == 0
       * merely re-states a dose that was never gone, and the test would pass
       * with the flag check removed. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "1700000300,1,1,1700000000,0,3,0\n"); /* retracted */
         fprintf(f, "1700000600,1,,1700000000,0,3,0\n");  /* del field empty */
         fclose(f);
      }
      ck(insulin_load() < 0, "a retraction with an EMPTY del flag is refused");
      ck(ins_count() == 0,
         "...and the dose it named STAYS deleted, not resurrected");

      /* A MALFORMED ROW in the middle: skipped, and reported. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "this is not a dose\n");
         fprintf(f, "1700000600,2,0,1700000600,1,5,0\n");
         fclose(f);
      }
      ck(insulin_load() < 0, "a malformed row makes the load incomplete");
      ck(ins_count() == 2, "...and both real doses are still there");

      /* AN IMPOSSIBLE DOSE is the same case: a units value outside what a pen
       * can deliver is not a dose, and this log is never rewritten. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "1700000600,2,0,1700000600,0,99999,0\n");
         fclose(f);
      }
      ck(insulin_load() < 0, "a dose no pen delivers makes it incomplete");
      ck(ins_count() == 1, "...and is not among the doses kept");

      /* AN OVERLONG LINE: skipped rather than parsed as a truncation of
       * itself, and said so. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         for (int i = 0; i < 400; i++)
            fputc('9', f);
         fprintf(f, "\n1700000600,2,0,1700000600,1,5,0\n");
         fclose(f);
      }
      ck(insulin_load() < 0, "a line longer than any row makes it incomplete");
      ck(ins_count() == 2, "...and the doses either side survive");

      /* ...and a whole file still reads whole, so none of the above can be
       * satisfied by always answering -1. */
      f = fopen(insulin_path(), "w");
      if (f) {
         fprintf(f, "# insulin log\n");
         fprintf(f, "1700000000,1,0,1700000000,0,3,0\n");
         fprintf(f, "1700000600,2,0,1700000600,1,5,0\n");
         fclose(f);
      }
      ck(insulin_load() == 0, "a whole file reads whole");
      ck(ins_count() == 2, "...with every dose");

      /* ...AND ONE THE APP WROTE ITSELF: a rule that calls the app's own log
       * damaged would warn on every launch, which trains the user to ignore
       * the warning that matters. */
      unlink(insulin_path());
      ck(insulin_append(1700000000, INS_FAST, 3, 0) == 0, "a dose is logged");
      ck(insulin_append(1700000600, INS_SLOW, 5, 0) == 0, "...and another");
      ck(insulin_load() == 0, "the file the app just wrote reads whole");
      ck(ins_count() == 2, "...with both doses");

      /* open() on a DIRECTORY succeeds; read() then fails with EISDIR. A real
       * syscall failure, no mock. */
      /* IN THIS SUITE'S OWN TREE, in a subdirectory of its own. It used to be
       * "build/app", which is one path for every build mode -- and this suite
       * runs under ASan as well as plain, so two processes were creating and
       * rmdir'ing build/app/insulin.csv at once. A subdirectory rather than
       * the fixture directory itself, because the file staged here has to BE
       * a directory while the cases either side of it need a real file of the
       * same name. */
      char eis[160];
      test_path(eis, sizeof eis, "eisdir");
      (void)mkdir(test_dir(), 0755);
      (void)mkdir(eis, 0777);
      insulin_paths(eis);
      unlink(insulin_path());
      if (mkdir(insulin_path(), 0777) == 0 ||
          access(insulin_path(), F_OK) == 0) {
         ck(insulin_load() < 0,
            "a read that FAILS is reported, not treated as end of file");
         rmdir(insulin_path());
      } else {
         ck(0, "could not stage the unreadable-file case");
      }
      insulin_paths(test_dir());
      unlink(insulin_path());
   }

   printf("== append, reload: the file is the record ==\n");
   fresh();
   ck(ins_count() == 0, "a fresh install is an empty log");
   ck(insulin_append(t0, INS_SLOW, 12, -3600) == 0, "a dose appends");
   ck(insulin_append(t0 + 60, INS_FAST, 4, -3600) == 0, "...and another");
   ck(ins_count() == 2, "both are in the tail");
   insulin_load();
   ck(ins_count() == 2, "both load back");
   ck(ins_at(0).t == t0 && ins_at(0).type == INS_SLOW && ins_at(0).units == 12,
      "...values intact, oldest first");
   ck(ins_at(1).type == INS_FAST && ins_at(1).units == 4, "...newest last");

   printf("== pre-population: last units PER TYPE, by dose time ==\n");
   ck(insulin_last_units(INS_SLOW) == 12, "SLOW recalls its own last dose");
   ck(insulin_last_units(INS_FAST) == 4, "FAST recalls its own last dose");
   ck(insulin_append(t0 - 999, INS_FAST, 6, -3600) == 0,
      "a BACKDATED dose still appends");
   ck(ins_at(0).t == t0 - 999,
      "...and files into place: the tail is TIME-sorted, not entry-sorted");
   ck(insulin_last_units(INS_FAST) == 4,
      "...so 'last' means latest BY DOSE TIME, unmoved by the backdate");

   printf("== out-of-range input is refused, not clamped ==\n");
   int before = ins_count();
   ck(insulin_append(t0, 7, 5, 0) < 0, "an unknown type is refused");
   ck(insulin_append(t0, INS_SLOW, 0, 0) < 0, "zero units is refused");
   ck(insulin_append(t0, INS_SLOW, INS_UNITS_MAX + 1, 0) < 0,
      "over-max units is refused");
   ck(insulin_append(0, INS_SLOW, 5, 0) < 0, "a zero timestamp is refused");
   ck(ins_count() == before, "refusals changed nothing in the tail");

   printf("== rows this process did not write ==\n");
   {
      FILE *f = fopen(insulin_path(), "a");
      if (f) {
         fputs("garbage,line,here\n", f);
         fputs("1700000100,1,999,0\n", f);         /* implausible units */
         fputs("1700000200,9,5,0\n", f);           /* unknown type */
         fputs("99999999999999999999,1,5,0\n", f); /* absurd digit run */
         fputs("1700000300,0,7,0\n", f);           /* one legitimate row */
         fclose(f);
      }
   }
   insulin_load();
   ck(ins_count() == before + 1,
      "only the legitimate foreign row loads; corrupt ones are dropped");
   ck(ins_at(ins_count() - 1).units == 7 &&
          ins_at(ins_count() - 1).type == INS_SLOW,
      "...and it parsed correctly");

   printf("== edit / delete append an assertion for one matched dose ==\n");
   fresh();
   (void)insulin_append(t0, INS_SLOW, 10, 0);
   (void)insulin_append(t0 + 100, INS_FAST, 5, 0);
   (void)insulin_append(t0 + 200, INS_FAST, 5, 0);
   {
      struct ins_rec orig = {t0 + 100, INS_FAST, 5};
      ck(insulin_update(&orig, t0 + 150, INS_SLOW, 8, 0) == 0,
         "an edit rewrites the matched row");
      ck(ins_count() == 3, "...row count unchanged");
      ck(insulin_last_units(INS_SLOW) == 8, "...new values took effect");
      insulin_load();
      ck(ins_count() == 3 && insulin_last_units(INS_SLOW) == 8,
         "...and the edit is durable on disk");
      struct ins_rec gone = {t0 + 150, INS_SLOW, 8};
      ck(insulin_delete(&gone) == 0, "a delete removes the row");
      ck(ins_count() == 2, "...row count down one");
      ck(insulin_delete(&gone) < 0, "deleting a missing row refuses");
      insulin_load();
      ck(ins_count() == 2, "...and the delete is durable on disk");
   }

   printf(
       "== the log is APPEND-ONLY: an edit adds history, never removes ==\n");
   {
      /* The whole point of schema v2. An edit used to rewrite the file in
       * place, which made this the one log a bug could shorten, and threw
       * away what the dose used to be. Now the file only ever grows, and a
       * past day's rows never change -- which is also what lets the sync
       * protocol treat old buckets as frozen. */
      fresh();
      ck(insulin_append(t0, INS_SLOW, 6, 0) == 0, "a dose is logged");
      long after_add      = fsize(insulin_path());
      struct ins_rec orig = {t0, INS_SLOW, 6};
      ck(insulin_update(&orig, t0, INS_SLOW, 4, 0) == 0, "it is corrected");
      long after_edit = fsize(insulin_path());
      ck(after_edit > after_add,
         "...the file GREW: the correction is a new row");
      ck(ins_count() == 1, "...but there is still exactly one dose");
      ck(insulin_last_units(INS_SLOW) == 4, "...showing the new value");
      insulin_load();
      ck(ins_count() == 1 && insulin_last_units(INS_SLOW) == 4,
         "...and replay agrees after a reload");

      struct ins_rec now4 = {t0, INS_SLOW, 4};
      ck(insulin_delete(&now4) == 0, "it is then retracted");
      ck(fsize(insulin_path()) > after_edit, "...which also only appends");
      insulin_load();
      ck(ins_count() == 0, "...and replay leaves no dose");
   }

   printf("== an over-long row is skipped, and blocks nothing ==\n");
   {
      /* There is no rewrite buffer left to overflow. A corrupt or over-long
       * row is simply not an assertion: load skips it, and an edit to a
       * perfectly good dose is unaffected by its presence. */
      fresh();
      ck(insulin_append(t0, INS_SLOW, 10, 0) == 0, "a normal dose to edit");
      FILE *f = fopen(insulin_path(), "ab");
      ck(f != NULL, "the log opens for the hostile row");
      if (f) {
         char pad[257];
         memset(pad, '9', sizeof pad);
         fprintf(f, "1700001200,0,7,0");
         fwrite(pad, 1, 256 - 16, f);
         fputc('\n', f);
         fclose(f);
      }
      insulin_load();
      ck(ins_count() == 1, "the over-long row does not load as a dose");
      struct ins_rec orig = {t0, INS_SLOW, 10};
      ck(insulin_update(&orig, t0, INS_SLOW, 11, 0) == 0,
         "an edit past it succeeds");
      insulin_load();
      ck(insulin_last_units(INS_SLOW) == 11, "...and is durable");
   }

   printf("== the tail stays bounded; the newest rows win ==\n");
   fresh();
   for (int i = 0; i < NINS + 10; i++)
      (void)insulin_append(t0 + i, INS_SLOW, 1 + (i % 50), 0);
   ck(ins_count() == NINS, "the tail caps at NINS");
   ck(ins_at(ins_count() - 1).t == t0 + NINS + 9, "...keeping the newest");
   insulin_load();
   ck(ins_count() == NINS && ins_at(ins_count() - 1).t == t0 + NINS + 9,
      "...and a reload agrees");

   printf("== a BACKDATED dose never evicts a NEWER one ==\n");
   /* THE DEFECT, in the words of the person holding the phone: they import
    * their dose history from another app, or type in a dose they forgot to log
    * last week, and the doses from the last few days VANISH from the list --
    * one per backdated row -- while the month-old imported ones stay. The file
    * still holds every dose, so the next launch brings the recent ones back
    * for as long as the tail has room again: a display bug that heals itself,
    * which is the hardest kind to be believed about. On a dose log it is worse
    * than a nuisance, because "what did I take yesterday" is the question this
    * list exists to answer, and the form pre-populates from it.
    *
    * The cause was one line: overflow dropped element ZERO and left ins_sort
    * to tidy up, and element zero is whichever assertion was REPLAYED first --
    * the oldest dose only when the file happens to be chronological, which is
    * exactly what a backdated row is not. */
   {
      const long tb = 1700000000L;
      fresh();
      for (int i = 0; i < NINS; i++)
         (void)insulin_append(tb + i, INS_SLOW, 1 + (i % 20), 0);
      ck(ins_count() == NINS, "a full tail");
      long oldest_held = ins_at(0).t;
      long newest_held = ins_at(NINS - 1).t;

      ck(insulin_append(tb - 31536000L, INS_FAST, 7, 0) == 0,
         "a year-old dose is logged");
      ck(ins_count() == NINS, "...the tail is still exactly full");
      /* THE ISOLATING ASSERTION: with arrival-order eviction the oldest dose
       * HELD is the one dropped, and the backdated one takes its place -- so
       * the tail loses a dose NEWER than the one it gained. */
      ck(ins_at(0).t == oldest_held,
         "...and the oldest dose HELD is still held: a backdated arrival does "
         "not evict a dose newer than itself");
      int backdated_in_tail = 0;
      for (int i = 0; i < ins_count(); i++)
         if (ins_at(i).t == tb - 31536000L)
            backdated_in_tail = 1;
      ck(!backdated_in_tail, "...and the backdated dose is not sitting in the "
                             "tail having pushed a newer one out");
      ck(ins_at(NINS - 1).t == newest_held, "...the newest dose is untouched");
      ck(fsize(insulin_path()) > 0,
         "...and the log itself still holds every dose, tail or no tail");

      /* THE REPLAY PATH, which the append path cannot express: during a load
       * the tail is NOT yet sorted (ins_sort runs once at the end), so element
       * zero is simply the first assertion in the file. Put the NEWEST dose
       * first and arrival order and time order disagree completely.
       *
       * Columns: written,id,del,unix_time,type,units,tz_offset_s. */
      {
         FILE *f = fopen(insulin_path(), "w");
         ck(f != NULL, "a hand-built log opens");
         if (f) {
            fprintf(f, "%ld,1,0,%ld,0,9,0\n", tb, tb + 100000L); /* newest */
            for (int i = 1; i < NINS; i++)
               fprintf(f, "%ld,%d,0,%ld,0,%d,0\n", tb, i + 1, tb + i,
                       1 + (i % 20));
            fprintf(f, "%ld,%d,0,%ld,1,4,0\n", tb, NINS + 1, tb + 200000L);
            fclose(f);
         }
         ck(insulin_load() == 0, "it reads whole");
         ck(ins_count() == NINS, "...filling the tail");
         int newest_first_kept = 0;
         int true_oldest_kept  = 0;
         for (int i = 0; i < ins_count(); i++) {
            if (ins_at(i).t == tb + 100000L)
               newest_first_kept = 1;
            if (ins_at(i).t == tb + 1)
               true_oldest_kept = 1;
         }
         ck(newest_first_kept,
            "a dose that ARRIVED first but is the NEWEST of all is kept");
         ck(!true_oldest_kept,
            "...and the dose evicted is the oldest BY TIME, not by arrival");
      }
   }

   printf("== eviction does not disturb ASSERTION REPLAY ==\n");
   /* The file is a log of assertions replayed in FILE order, last one per id
    * wins, and a retraction can name a dose asserted thousands of rows
    * earlier. Changing WHICH dose overflow discards must not touch any of
    * that, and the failure mode if it did is the worst this app has: a dose
    * the user deleted coming back.
    *
    * Three cases, because there are three ways a later row can name an earlier
    * dose: retract one still in the window, retract one the window has already
    * dropped, and AMEND one the window has already dropped. */
   {
      const long tb = 1700000000L;

      /* (a) A RETRACTION OF A DOSE STILL IN THE TAIL still removes it. */
      fresh();
      {
         FILE *f = fopen(insulin_path(), "w");
         ck(f != NULL, "a log opens");
         if (f) {
            fprintf(f, "%ld,1,0,%ld,0,11,0\n", tb, tb + 500L);
            fprintf(f, "%ld,2,0,%ld,1,6,0\n", tb, tb + 600L);
            fprintf(f, "%ld,1,1,%ld,0,11,0\n", tb + 10,
                    tb + 500L); /* retract */
            fclose(f);
         }
         ck(insulin_load() == 0, "it reads whole");
         ck(ins_count() == 1, "the retracted dose is gone");
         ck(ins_at(0).units == 6, "...and the other dose is the one left");
      }

      /* (b) A RETRACTION OF A DOSE THE WINDOW HAS ALREADY DROPPED is a no-op
       * -- it must not remove some OTHER dose, and it must not put the named
       * one back. Id 1 is the oldest by time, so eviction reaches it first. */
      fresh();
      {
         FILE *f = fopen(insulin_path(), "w");
         ck(f != NULL, "a log opens");
         if (f) {
            fprintf(f, "%ld,1,0,%ld,0,11,0\n", tb, tb + 1L);
            for (int i = 1; i <= NINS; i++)
               fprintf(f, "%ld,%d,0,%ld,0,%d,0\n", tb, i + 1, tb + 1000L + i,
                       1 + (i % 20));
            fprintf(f, "%ld,1,1,%ld,0,11,0\n", tb + 10, tb + 1L); /* retract */
            fclose(f);
         }
         ck(insulin_load() == 0, "it reads whole");
         ck(ins_count() == NINS,
            "a retraction naming a dose outside the window removes nothing "
            "else");
         int evicted_present = 0;
         for (int i = 0; i < ins_count(); i++)
            if (ins_at(i).t == tb + 1L)
               evicted_present = 1;
         ck(!evicted_present, "...and does not resurrect the dose it names");
         ck(ins_at(NINS - 1).t == tb + 1000L + NINS,
            "...leaving the newest dose exactly where it was");
      }

      /* (c) AN AMENDMENT to a dose the window has dropped is a fresh push and
       * is judged on its own merits. Amended to a time OLDER than everything
       * held, it stays out; the doses in the window are untouched either way.
       * This is the case where getting the id bookkeeping wrong would show:
       * ins_apply advances g_ins_next from EVERY assertion before it ever
       * reaches the push, so a push the window refuses can never cause an id
       * to be minted twice. */
      fresh();
      {
         FILE *f = fopen(insulin_path(), "w");
         ck(f != NULL, "a log opens");
         if (f) {
            fprintf(f, "%ld,1,0,%ld,0,11,0\n", tb, tb + 1L);
            for (int i = 1; i <= NINS; i++)
               fprintf(f, "%ld,%d,0,%ld,0,%d,0\n", tb, i + 1, tb + 1000L + i,
                       1 + (i % 20));
            /* the same dose, corrected -- and still older than the window */
            fprintf(f, "%ld,1,0,%ld,0,12,0\n", tb + 10, tb + 2L);
            fclose(f);
         }
         ck(insulin_load() == 0, "it reads whole");
         ck(ins_count() == NINS, "the tail is still exactly full");
         int amended_in_tail = 0;
         for (int i = 0; i < ins_count(); i++)
            if (ins_at(i).t == tb + 2L)
               amended_in_tail = 1;
         ck(!amended_in_tail,
            "an amendment older than every dose held does not displace one");
         ck(ins_at(0).t == tb + 1000L + 1 &&
                ins_at(NINS - 1).t == tb + 1000L + NINS,
            "...and the window is exactly the newest NINS doses by time");
         /* AND THE ID SPACE IS INTACT: the next dose the user logs must get a
          * fresh id, not one the file has already used, or the next replay
          * would treat two different doses as one -- the later row would be
          * read as an AMENDMENT of the earlier, and a real dose would be
          * silently overwritten by an unrelated one. That is what an
          * implementation which advanced g_ins_next only for doses the window
          * KEPT would produce, which is why the previously-newest dose is
          * named here rather than only the new one. */
         long prev_newest = ins_at(ins_count() - 1).t;
         ck(insulin_append(tb + 9000L, INS_FAST, 3, 0) == 0,
            "a new dose is logged after all that");
         insulin_load();
         ck(ins_count() == NINS, "...the tail is still exactly full");
         ck(ins_at(ins_count() - 1).t == tb + 9000L,
            "...and it is the newest dose in the tail, once");
         int copies      = 0;
         int prev_intact = 0;
         for (int i = 0; i < ins_count(); i++) {
            if (ins_at(i).t == tb + 9000L)
               copies++;
            if (ins_at(i).t == prev_newest)
               prev_intact = 1;
         }
         ck(copies == 1, "...exactly once, so no id was minted twice");
         ck(prev_intact,
            "...and the dose that WAS the newest is still there, not "
            "overwritten by a re-used id");
      }
      fresh();
   }

   printf("== an edited timestamp is resolved in the TARGET date's zone ==\n");
   /* TODO 131. The keypad splits an instant into a civil date and recombines
    * it. Both halves used g_tz_off -- the offset TODAY -- so an edit that
    * moved a dose to the other side of a DST boundary persisted it an hour
    * wrong, and wrote today's offset into the row's tz column, which is the
    * one field that could have shown it. */
   {
      /* A dose at 09:30 on a November morning: PST, UTC-8. */
      long nov   = naive_at(2025, 11, 15, 9, 30);
      long t_nov = nov - STD;
      ck(pacific(0, t_nov) == STD, "the November dose really is on standard "
                                   "time (the fixture, not the code)");

      /* Move it to July 15th, keeping 09:30. July is DAYLIGHT time, so the
       * instant is an hour EARLIER than the same civil time in November. */
      struct civil_res r =
          civil_reaim(t_nov, CIVIL_EDIT_MONTHDAY, 7, 15, pacific, 0);
      long want = naive_at(2025, 7, 15, 9, 30) - DST;
      ck(r.t == want, "a dose moved to a July date lands on July's offset, "
                      "not November's");
      ck(r.t != naive_at(2025, 7, 15, 9, 30) - STD,
         "...which is an hour off what today's offset would have given");
      ck(r.off == DST, "...and the offset PERSISTED with it is July's");
      ck(r.fix == CIVIL_UNIQUE, "...an ordinary date names exactly one "
                                "instant");

      /* And back the other way: from a July dose to a November date. Both
       * directions, because a bug that adds the difference instead of
       * subtracting it passes one of them. */
      long jul   = naive_at(2025, 7, 4, 18, 5);
      long t_jul = jul - DST;
      struct civil_res b =
          civil_reaim(t_jul, CIVIL_EDIT_MONTHDAY, 12, 25, pacific, 0);
      ck(b.t == naive_at(2025, 12, 25, 18, 5) - STD,
         "a dose moved from July to December lands on December's offset");
      ck(b.off == STD, "...and carries December's offset");

      /* The YEAR field crosses the same boundary: the same civil date in a
       * different year can be a different offset, and the year is also what
       * decides how long February is. */
      long feb   = naive_at(2024, 2, 29, 7, 0);
      long t_feb = feb - STD;
      struct civil_res y =
          civil_reaim(t_feb, CIVIL_EDIT_YEAR, 2025, 0, pacific, 0);
      ck(y.t == naive_at(2025, 2, 28, 7, 0) - STD,
         "February 29th moved to a non-leap year clamps to the 28th");
      struct civil_res y2 =
          civil_reaim(t_feb, CIVIL_EDIT_YEAR, 2028, 0, pacific, 0);
      ck(y2.t == naive_at(2028, 2, 29, 7, 0) - STD,
         "...and keeps the 29th when the target year has one");
   }

   printf("== the civil date of an instant is read in ITS OWN offset ==\n");
   /* The other direction of the same defect, and the one that decides how
    * long February is when only MMDD is being typed. 00:30 on a July morning
    * is still July 15th; read in November's offset it is July 14th, and an
    * MMDD entry would then validate against the wrong month's length. */
   {
      long t  = naive_at(2025, 7, 15, 0, 30) - DST;
      long yy = 0, mm = 0, dd = 0;
      civil_at(t, pacific, 0, &yy, &mm, &dd);
      ck(yy == 2025 && mm == 7 && dd == 15,
         "00:30 on a summer morning is that morning's date");
      long t2 = naive_at(2025, 12, 31, 23, 30) - STD;
      civil_at(t2, pacific, 0, &yy, &mm, &dd);
      ck(yy == 2025 && mm == 12 && dd == 31,
         "...and New Year's Eve is still that year");
   }

   printf("== the REPEATED hour: two instants, and a stated choice ==\n");
   /* 2025-11-02 01:30 happened twice in US/Pacific. The rule (civil.h) is the
    * EARLIER instant -- the first time the clock read it -- and the other one
    * is reported rather than discarded. */
   {
      long base = naive_at(2025, 11, 2, 12, 0) - STD; /* that afternoon */
      struct civil_res r =
          civil_reaim(base, CIVIL_EDIT_TIME, 1, 30, pacific, 0);
      long nv    = naive_at(2025, 11, 2, 1, 30);
      long early = nv - DST;
      long late  = nv - STD;
      ck(late - early == 3600, "the two candidates really are an hour apart "
                               "(the fixture)");
      ck(r.fix == CIVIL_AMBIGUOUS, "01:30 on the fall-back day is ambiguous");
      ck(r.t == early, "...resolved to the EARLIER of the two instants");
      ck(r.t != late, "...and NOT the later one");
      ck(r.t_alt == late, "...with the rejected instant reported, not lost");
      ck(r.off == DST && r.off_alt == STD,
         "...each instant carrying the offset actually in force at it");

      /* STABLE. Re-editing the resolved instant to the same civil time must
       * not walk it an hour later each time -- which is what choosing the
       * LATER instant would do, since the later instant redisplays as the
       * same 01:30 and would resolve to a still later one. */
      struct civil_res again =
          civil_reaim(r.t, CIVIL_EDIT_TIME, 1, 30, pacific, 0);
      ck(again.t == r.t, "re-typing the same ambiguous time is a no-op");

      /* One minute either side of the repeated hour is not ambiguous. */
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 0, 59, pacific, 0).fix ==
             CIVIL_UNIQUE,
         "00:59 that day names one instant");
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 2, 0, pacific, 0).fix ==
             CIVIL_UNIQUE,
         "...and so does 02:00");
   }

   printf("== the SKIPPED hour: no instant, so the entry moves forward ==\n");
   /* 2025-03-09 02:30 never existed. The rule (civil.h) is to shift forward
    * by the gap, so it becomes 03:30 -- and the form redisplays 03:30, which
    * is how the user finds out. Accepting it as though it were valid puts the
    * dose at 01:30, an hour BEFORE what was typed. */
   {
      long base = naive_at(2025, 3, 9, 12, 0) - DST;
      struct civil_res r =
          civil_reaim(base, CIVIL_EDIT_TIME, 2, 30, pacific, 0);
      ck(r.fix == CIVIL_NONEXISTENT, "02:30 on the spring-forward day does "
                                     "not exist");
      ck(r.t == naive_at(2025, 3, 9, 2, 30) - STD,
         "...and is resolved through the offset in force BEFORE the gap");
      ck(r.t + r.off == naive_at(2025, 3, 9, 3, 30),
         "...which redisplays as 03:30: forward by the gap, never backward");
      ck(r.off == DST, "...and the offset stored is the one actually in "
                       "force at the instant stored, not the one the "
                       "arithmetic went through");
      /* The gap's two edges: 01:59 exists, 03:00 exists, and everything
       * between 02:00 and 02:59 does not. */
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 1, 59, pacific, 0).fix ==
             CIVIL_UNIQUE,
         "01:59 that morning exists");
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 3, 0, pacific, 0).fix ==
             CIVIL_UNIQUE,
         "...and so does 03:00");
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 2, 0, pacific, 0).fix ==
             CIVIL_NONEXISTENT,
         "...while 02:00 itself does not");
      ck(civil_reaim(base, CIVIL_EDIT_TIME, 2, 59, pacific, 0).fix ==
             CIVIL_NONEXISTENT,
         "...nor 02:59");
   }

   printf("== and every ordinary timestamp is untouched ==\n");
   /* The regression a disambiguator invites: two hours a year get fixed and
    * the other 8758 move. Every hour of an ordinary day, both sides of the
    * year, must resolve to exactly naive minus the offset. */
   {
      int bad = 0;
      for (int mo = 1; mo <= 12; mo++) {
         for (int hh = 0; hh < 24; hh++) {
            long nv            = naive_at(2025, mo, 20, hh, 15);
            struct civil_res r = civil_resolve(nv, pacific, 0);
            long off           = pacific(0, nv - DST) == pacific(0, nv - STD)
                                     ? pacific(0, nv - STD)
                                     : r.off;
            if (r.fix != CIVIL_UNIQUE || r.t != nv - off || r.t_alt != r.t)
               bad++;
         }
      }
      ck(bad == 0, "288 ordinary local times across the year each name "
                   "exactly one instant, unchanged");
      ck(zone_calls > 0, "...and the zone was actually consulted");
   }

   printf(all ? "ALL INSULIN TESTS PASSED\n" : "SOME TESTS FAILED\n");
   return all ? 0 : 1;
}
