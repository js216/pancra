// SPDX-License-Identifier: GPL-3.0
// exercisetest.c --- Host tests for the exercise-intensity log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for exercise.c.
 *
 * TWO THINGS ARE UNDER TEST AND ONLY ONE OF THEM IS THE FILE.
 *
 * The log itself is the weight log with a narrower value, and it is checked
 * the same way: a row is validated on the way in, a damaged file is REPORTED
 * rather than silently shortened, and what did parse is kept.
 *
 * The settling rule is the new thing. The exercise button cycles 0-1-2-3, and
 * the values it passes through on the way to the one you want were never
 * statements about exercise -- so a value is written only once it has stood
 * unchanged for a minute. Everything about that rule is a decision made from
 * two numbers and a clock, which is exactly the kind of logic that becomes
 * untestable the moment it lives inside a renderer. It does not live there.
 *
 * Built and run by `make exercisetest`.
 */
#include "exercise.h"
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* Write a file verbatim, so a malformed log can be handed to the loader. */
static void put(const char *path, const char *body)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      printf("  [FAIL] cannot write %s\n", path);
      fails++;
      return;
   }
   fwrite(body, 1, strlen(body), f);
   fclose(f);
}

int main(void)
{
   char dir[256];
   char path[300];
   snprintf(dir, sizeof dir, "%s", test_dir());
   if (!exercise_paths(dir)) {
      printf("  [FAIL] exercise_paths did not fit\n");
      return 1;
   }
   snprintf(path, sizeof path, "%s", exercise_path());

   printf("== what the log accepts ==\n");
   {
      (void)unlink(path);
      ck(exercise_load() == 0, "a missing file is an empty log, not a failure");
      ck(ex_count() == 0, "...and it holds nothing");
      ck(exercise_append(1700000000L, 2, 0) == 0, "a level in range appends");
      ck(ex_count() == 1, "...and reaches the tail");
      ck(ex_newest().level == 2, "...with the level it was given");
      /* THE RESTING POSITION IS NOT A RECORD. 0 means "no exercise", which is
       * the absence of a row; a row saying 0 would mean "exercise of intensity
       * nothing", which is not a thing anybody logs. */
      ck(exercise_append(1700000100L, 0, 0) == -1, "a level of 0 is refused");
      ck(exercise_append(1700000100L, 4, 0) == -1,
         "a level above the domain is refused");
      ck(exercise_append(1700000100L, -1, 0) == -1,
         "a negative level is refused");
      ck(ex_count() == 1, "...and none of them reached the tail");
      ck(exercise_append(0, 2, 0) == -1, "an instant of 0 is refused");
      ck(exercise_append(EX_T_MAX, 2, 0) == -1,
         "an instant past the epoch bound is refused");
      ck(ex_count() == 1, "...nor did those");
   }

   printf("== what survives a reload ==\n");
   {
      (void)unlink(path);
      ck(exercise_append(1700000200L, 1, 0) == 0, "append one");
      ck(exercise_append(1700000100L, 3, 0) == 0, "...and an EARLIER one");
      ck(exercise_load() == 0, "the file reloads whole");
      ck(ex_count() == 2, "...with both rows");
      /* Oldest first is part of the contract, and a backdated entry has to
       * file into place rather than sit at the end until the next launch. */
      ck(ex_at(0).t == 1700000100L && ex_at(0).level == 3,
         "oldest first, whatever order they arrived in");
      ck(ex_at(1).t == 1700000200L && ex_at(1).level == 1,
         "...newest last");
   }

   printf("== a damaged file is REPORTED, and its good rows kept ==\n");
   {
      /* Each of these produces a SHORT history, and the whole point of the
       * -1 answer is that the caller can say so instead of presenting a
       * shortened log as complete. */
      put(path, "# unix_time,level,tz_offset_s\n1700000100,2,0\nrubbish\n"
                "1700000200,3,0\n");
      ck(exercise_load() == -1, "a row that does not parse is reported");
      ck(ex_count() == 2, "...and the rows that DID parse are kept");

      put(path, "# unix_time,level,tz_offset_s\n1700000100,2,0\n1700000200,3,0");
      ck(exercise_load() == -1, "a final line with no newline is reported");
      /* NOT PARSED: the bytes may make a valid pair and still be half a
       * record, because what says a row is finished is the newline the file
       * does not have. */
      ck(ex_count() == 1, "...and that line is NOT taken as a row");

      /* A WRITTEN ZERO is damage, not an empty log. It is the one value the
       * appender refuses, so a file holding it did not come from this app. */
      put(path, "# unix_time,level,tz_offset_s\n1700000100,0,0\n");
      ck(exercise_load() == -1, "a row holding the resting position is damage");
      ck(ex_count() == 0, "...and is not taken");

      /* An EMPTY level field reads as 0, and 0 is outside the domain, so it
       * is damage by the same rule that refuses a written 0 -- not by a
       * separate check on whether the field held digits. (One was tried; a
       * mutant deleting it survived, because the range check below had
       * already rejected everything it could catch.) */
      put(path, "# unix_time,level,tz_offset_s\n1700000100,,0\n");
      ck(exercise_load() == -1, "a row with an empty level field is damage");
      ck(ex_count() == 0, "...and is not taken");

      /* THE EPOCH BOUND HAS TO HOLD ON THE WAY IN, not only on the way out.
       * exercise_append refuses an implausible instant, but the loader is a
       * second door into the same tail: a file row is not something this app
       * necessarily wrote (a restore, a sync, a hand edit), and a corrupt
       * digit run that parses as the year 12000 would sort to the end and sit
       * there as the newest record for good. Asserted separately from the
       * append bound because a mutant deleting the parser's copy survived
       * while only the append was covered. */
      put(path, "# unix_time,level,tz_offset_s\n99999999999999,2,0\n");
      ck(exercise_load() == -1, "a row past the epoch bound is damage");
      ck(ex_count() == 0, "...and is not taken");

      put(path, "# unix_time,level,tz_offset_s\n1700000100,2,0\n");
      ck(exercise_load() == 0, "a clean file reports whole");
      ck(ex_count() == 1, "...and holds its row");
   }

   printf("== the tail keeps the NEWEST rows by time ==\n");
   {
      (void)unlink(path);
      /* RELOADED, not merely unlinked. Removing the file empties the log on
       * disk and leaves the in-memory tail exactly as it was -- which is
       * correct behaviour (the tail is a cache of what was loaded, not a view
       * of the file), and it means a section that only unlinks starts with
       * whatever the previous section left behind. This one counts evictions,
       * so a single stray row shifts every number in it by one. */
      ck(exercise_load() == 0, "an unlinked log reloads as empty");
      ck(ex_count() == 0, "...and the tail is reset with it");
      /* Fill past capacity with ascending instants, then offer a much older
       * one: it must evict nobody. An import of last year's rows displacing
       * this morning's is the failure this rule exists for, and it reads as a
       * display bug that fixes itself on restart. */
      for (int i = 0; i < NEX + 10; i++)
         (void)exercise_append(1700000000L + i, (i % 3) + 1, 0);
      ck(ex_count() == NEX, "the tail is capped");
      ck(ex_at(0).t == 1700000000L + 10,
         "...and holds the newest NEX, so the oldest fell off the front");
      long newest = ex_newest().t;
      ck(exercise_append(1600000000L, 2, 0) == 0, "an OLD row appends");
      ck(ex_newest().t == newest,
         "...and does not displace anything newer than itself");
      ck(ex_at(0).t == 1700000000L + 10, "...nor take a slot at the front");
   }

   printf("== the settling rule ==\n");
   {
      struct ex_pending p = {0, 0, 0};
      ck(ex_tick(&p, 1000) == EX_IDLE, "at rest, nothing is pending");
      ck(ex_remaining(&p, 1000) == 0, "...and there is nothing to count down");

      ex_press(&p, 1000);
      ck(p.level == 1, "a press advances to 1");
      ck(p.armed == 1, "...and arms it");
      ck(ex_tick(&p, 1000) == EX_HOLD, "immediately, it has not settled");
      ck(ex_remaining(&p, 1000) == EX_SETTLE_S, "...the full period remains");
      ck(ex_tick(&p, 1000 + EX_SETTLE_S - 1) == EX_HOLD,
         "one second short, it has still not settled");
      ck(ex_remaining(&p, 1000 + EX_SETTLE_S - 1) == 1, "...one second left");
      /* INCLUSIVE, so the value commits AT the mark rather than one tick past
       * it -- two places describing one threshold must not disagree about the
       * instant it is reached. */
      ck(ex_tick(&p, 1000 + EX_SETTLE_S) == EX_COMMIT,
         "at the mark exactly, it commits");
      ck(ex_remaining(&p, 1000 + EX_SETTLE_S) == 0, "...nothing left to wait");

      /* CYCLING PAST A VALUE MUST COST NOTHING. This is the whole reason the
       * delay exists: 0->3 goes through 1 and 2, and neither was ever a
       * statement about exercise. */
      struct ex_pending q = {0, 0, 0};
      ex_press(&q, 2000);       /* 1 */
      ex_press(&q, 2000 + 1);   /* 2 */
      ex_press(&q, 2000 + 2);   /* 3 */
      ck(q.level == 3, "three presses reach 3");
      ck(ex_tick(&q, 2000 + 2 + EX_SETTLE_S - 1) == EX_HOLD,
         "the clock restarted on the LAST press, not the first");
      ck(ex_tick(&q, 2000 + 2 + EX_SETTLE_S) == EX_COMMIT,
         "...and settles a full period after it");

      /* A FOURTH PRESS IS THE CANCEL. Back at rest means nothing pending and
       * nothing written -- cycling all the way round is how a user undoes a
       * press they did not mean. */
      ex_press(&q, 3000); /* 0 */
      ck(q.level == 0, "the fourth press returns to rest");
      ck(q.armed == 0, "...and disarms");
      ck(ex_tick(&q, 3000 + EX_SETTLE_S * 10) == EX_IDLE,
         "...so no amount of waiting commits anything");

      /* COMMITTING KEEPS THE VALUE ON THE BUTTON. Blanking it would read as
       * the press having been lost, at the exact moment it was recorded. */
      struct ex_pending r = {0, 0, 0};
      ex_press(&r, 4000);
      ck(ex_tick(&r, 4000 + EX_SETTLE_S) == EX_COMMIT, "it settles");
      ex_committed(&r);
      ck(r.level == 1, "the level stays on the button after committing");
      ck(r.armed == 0, "...but it is no longer pending");
      ck(ex_tick(&r, 4000 + EX_SETTLE_S * 3) == EX_IDLE,
         "...so it is not written a second time");

      /* A NEGATIVE ELAPSED INTERVAL cannot happen from a monotonic clock, but
       * a caller passing wall time by mistake would make one -- and the safe
       * answer is to delay a write, never to commit early. */
      struct ex_pending s = {0, 0, 0};
      ex_press(&s, 5000);
      ck(ex_tick(&s, 4000) == EX_HOLD,
         "a backward clock delays, it does not commit");
      ck(ex_remaining(&s, 4000) == EX_SETTLE_S,
         "...and the countdown does not run backwards");
   }

   if (fails == 0)
      printf("\nALL EXERCISE TESTS PASSED\n");
   else
      printf("\n%d EXERCISE TEST(S) FAILED\n", fails);
   return fails != 0;
}
