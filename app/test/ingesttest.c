// SPDX-License-Identifier: GPL-3.0
// ingesttest.c --- the gate every CGM reading passes through
// Copyright 2026 Jakob Kastelic

/* These two checks decide whether a frame off the wire becomes the user's
 * glucose: displayed, alarmed on, counted in time-in-range, and appended to a
 * log that is never rewritten.
 *
 * They existed inside main.c, where no test could reach them, and a review
 * proved the cost by mutating both with `make check` green:
 *
 *   - widening the value window to 0..100000, so a 5,000 mg/dL frame is
 *     displayed and fed to the alarm;
 *   - widening the age bound to 65535, which is the 18-hour backdating bug
 *     the comment above the check describes having fixed, reintroduced
 *     verbatim.
 *
 * So the cases below sit ON the boundaries, and the boundaries are asserted
 * from both sides. A test that only checks the middle of a range cannot fail
 * when the range moves.
 */
#include "ingest.h"
#include <stdio.h>

static int fails;

static void ck(int ok, const char *what)
{
   if (!ok) {
      printf("  FAIL: %s\n", what);
      fails = 1;
   }
}

static void verdict_is(int mg_dl, int age_s, int want, const char *what)
{
   struct ingest_out o = ingest_decide(mg_dl, age_s, 1000000L);
   if (o.verdict != want) {
      printf("  FAIL: %s (glu=%d age=%d -> verdict %d, wanted %d)\n", what,
             mg_dl, age_s, o.verdict, want);
      fails = 1;
   }
}

int main(void)
{
   /* ---- the VALUE window, from both sides ---- */
   verdict_is(INGEST_GLU_MIN, 0, INGEST_OK,
              "the lowest accepted value is accepted");
   verdict_is(INGEST_GLU_MIN - 1, 0, INGEST_IMPLAUSIBLE,
              "one below the floor is refused");
   verdict_is(INGEST_GLU_MAX, 0, INGEST_OK,
              "the highest accepted value is accepted");
   verdict_is(INGEST_GLU_MAX + 1, 0, INGEST_IMPLAUSIBLE,
              "one above the ceiling is refused");

   /* 0 is the warm-up / sensor-error sentinel and must never become a
    * reading: it would render as the headline number with a trend arrow and
    * fire a spurious LOW. */
   verdict_is(0, 0, INGEST_IMPLAUSIBLE, "the zero sentinel is refused");
   /* The 12-bit field carries up to 4095 verbatim. */
   verdict_is(4095, 0, INGEST_IMPLAUSIBLE,
              "a full-scale 12-bit value is refused");
   verdict_is(5000, 0, INGEST_IMPLAUSIBLE,
              "5000 mg/dL is refused (the widened-window mutant)");
   verdict_is(-1, 0, INGEST_IMPLAUSIBLE, "a negative value is refused");

   /* ---- the AGE window, from both sides ---- */
   verdict_is(113, 0, INGEST_OK, "a brand-new frame is accepted");
   verdict_is(113, INGEST_AGE_MAX, INGEST_OK,
              "a frame at the age limit is accepted");
   verdict_is(113, INGEST_AGE_MAX + 1, INGEST_STALE,
              "one second past the limit is refused");
   verdict_is(113, -1, INGEST_STALE, "a negative age is refused");

   /* THE REGRESSION. age arrives as a widened uint16, so 65535 is reachable
    * from the wire; it backdates the reading 18.2 hours into a log that is
    * never rewritten and is re-admitted on every restart. */
   verdict_is(113, 65535, INGEST_STALE,
              "age=65535 is refused (the 18-hour backdating bug)");

   /* ---- the timestamp ---- */
   {
      struct ingest_out o = ingest_decide(113, 300, 1000000L);
      ck(o.verdict == INGEST_OK, "a 5-minute-old frame is usable");
      ck(o.t == 1000000L - 300,
         "...and its instant is the age subtracted from now");
   }
   {
      /* A refused frame must not hand back a plausible-looking instant: a
       * caller that ignored the verdict would otherwise get `now`. */
      struct ingest_out o = ingest_decide(9999, 0, 1000000L);
      ck(o.t == 0, "a refused frame carries no timestamp");
   }

   /* The value check is applied BEFORE the age check, so an implausible value
    * reports as implausible whatever its age. The two messages go to
    * different log lines, and a reader diagnosing a silent sensor needs the
    * one that is actually true. */
   verdict_is(9999, 65535, INGEST_IMPLAUSIBLE,
              "an impossible value reports as implausible, not stale");

   if (!fails)
      printf("ingesttest: the reading gate holds at both boundaries\n");
   return fails;
}
