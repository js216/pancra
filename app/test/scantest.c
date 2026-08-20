// SPDX-License-Identifier: GPL-3.0
// scantest.c --- Host tests for the scan-lifecycle decision
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for scan_should_start.
 *
 * This predicate decides whether an already-paired CGM can reconnect at all:
 * the advert-driven reconnect runs off the scan it restarts. It has been wrong
 * in both directions already -- once too loose (retrying every tick, which
 * re-enters JNI each second and trips Android's 5-startScan-in-30s block,
 * making a recoverable failure sticky), and the bugs it exists to heal were
 * all latches that left the scan down with nothing to notice.
 *
 * Built and run by `make scantest`.
 */
#include "scanlogic.h"
#include <stdio.h>
#include <string.h> /* strcmp: the failure messages are compared as text */

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* A state in which the scan SHOULD be restarted; each case perturbs one field
 * so a failure names exactly which condition is wrong. */
static struct scan_state ok_state(void)
{
   struct scan_state s = {.have_activity = 1,
                          .paused        = 0,
                          .scanning      = 0,
                          .pairing       = 0,
                          .meter_busy    = 0,
                          .now           = 100000,
                          .hold_until    = 0,
                          .last_attempt  = 0};
   return s;
}

/* A MINIATURE OF scan.c's SCAN STATE, so the ORDER of these decisions is
 * executed rather than argued.
 *
 * It composes exactly what start_scan and jni_scan_failed compose, in the same
 * order: refuse a start inside the back-off, else allocate a generation and
 * latch; match a failure against the latched generation, then stamp the
 * back-off and clear the latch. Nothing here is a second copy of the POLICY --
 * every judgement is scanlogic's, and this is only the state those judgements
 * act on. */
struct scan_model {
   int gen;          /* generations handed out so far */
   int scanning;     /* the latch that used to be permanent */
   long retry_after; /* the back-off a failure left */
   int last_err;     /* what the user was last told */
   long now;
};

/* start_scan: returns the generation that went out, or 0 if it was refused. */
static int model_start(struct scan_model *m)
{
   if (m->scanning)
      return 0;
   if (!scan_start_allowed(m->now, m->retry_after))
      return 0;
   m->scanning = 1;
   return ++m->gen;
}

/* jni_scan_failed: returns 1 if the failure was acted on. */
static int model_fail(struct scan_model *m, int gen, int err)
{
   struct scan_fail f = {
       .failed_gen = gen, .cur_gen = m->gen, .scanning = m->scanning};
   if (!scan_fail_applies(&f))
      return 0;
   m->last_err    = err;
   m->retry_after = scan_fail_retry_at(m->now);
   m->scanning    = 0;
   return 1;
}

int main(void)
{
   printf("== the healing case ==\n");
   {
      struct scan_state s = ok_state();
      ck(scan_should_start(&s) == 1,
         "UI up, scan down, nothing else owning the radio -> restart");
      /* The previous assertion here re-checked s.last_attempt == 0, which
       * scan_should_start cannot change (it takes a const pointer) and
       * ok_state() had just set. It could not fail under any mutation. What is
       * actually worth pinning is that the decision does not MUTATE its input,
       * since main.c stamps last_attempt itself afterwards. */
      struct scan_state before = s;
      (void)scan_should_start(&s);
      ck(s.last_attempt == before.last_attempt && s.scanning == before.scanning,
         "...and the decision leaves its input untouched");
   }

   printf("== every reason NOT to start ==\n");
   {
      struct scan_state s = ok_state();
      s.have_activity     = 0;
      ck(scan_should_start(&s) == 0, "no activity: nothing to scan for");
   }
   {
      struct scan_state s = ok_state();
      s.paused            = 1;
      ck(scan_should_start(&s) == 0,
         "paused: the scan is down deliberately, do not fight on_pause");
   }
   {
      struct scan_state s = ok_state();
      s.scanning          = 1;
      ck(scan_should_start(&s) == 0,
         "already scanning: never stack a second scan client");
   }
   {
      struct scan_state s = ok_state();
      s.pairing           = 1;
      ck(scan_should_start(&s) == 0, "pairing owns the radio");
   }
   {
      struct scan_state s = ok_state();
      s.meter_busy        = 1;
      ck(scan_should_start(&s) == 0, "a meter sync owns the radio");
   }

   printf("== the quiet window after a bonding connect ==\n");
   {
      struct scan_state s = ok_state();
      s.hold_until        = s.now + 10;
      ck(scan_should_start(&s) == 0, "inside the hold, stay off the air");
      s.now = s.hold_until;
      ck(scan_should_start(&s) == 1, "at the hold's expiry, restart");
   }

   printf("== the retry throttle ==\n");
   {
      /* Too loose was a real bug: start_scan only clears `scanning` on
       * SUCCESS, so a persistent failure keeps this condition true forever. At
       * 1 Hz that is a JNI call and a rewritten status line every second, and
       * enough startScan calls to trip Android's own block. */
      struct scan_state s = ok_state();
      s.last_attempt      = s.now - 1;
      ck(scan_should_start(&s) == 0, "one second after a try: throttled");
      s.last_attempt = s.now - (SCAN_RETRY_S - 1);
      ck(scan_should_start(&s) == 0,
         "just inside the interval: still throttled");
      s.last_attempt = s.now - SCAN_RETRY_S;
      ck(scan_should_start(&s) == 1, "at the interval: retry");
      s.last_attempt = s.now - (SCAN_RETRY_S * 10L);
      ck(scan_should_start(&s) == 1, "long after: retry");
   }

   printf("== a never-tried heal is not throttled, even at a tiny epoch ==\n");
   {
      /* `last_attempt == 0` means "never tried". Testing that with a realistic
       * clock proves nothing -- now minus zero is always past the interval, so
       * dropping the sentinel check is an EQUIVALENT mutation there (verified).
       * The sentinel only bites when the clock itself is near zero, which is
       * precisely the unset-clock case realtime_s() is hardened for: a device
       * with a dead RTC and no network reports ~1970, and realtime_s() returns
       * 0 outright if clock_gettime fails. Without the sentinel the very first
       * heal after a failed stop would be suppressed on such a device. */
      struct scan_state s = ok_state();
      s.now               = 5; /* clock unset */
      s.last_attempt      = 0;
      ck(scan_should_start(&s) == 1,
         "never tried + tiny epoch -> heal immediately, not after 30 s");
      s.last_attempt = 1;
      ck(scan_should_start(&s) == 0,
         "...but a real prior attempt at a tiny epoch still throttles");
   }

   printf("== the throttle never outranks a hard reason ==\n");
   {
      struct scan_state s = ok_state();
      s.last_attempt      = s.now - (SCAN_RETRY_S * 10L);
      s.paused            = 1;
      ck(scan_should_start(&s) == 0,
         "an elapsed throttle does not override pause");
      s.paused   = 0;
      s.scanning = 1;
      ck(scan_should_start(&s) == 0, "...nor override an already-live scan");
   }

   printf("== candidate selection: guess wrong and a live bond dies ==\n");
   {
      /* commit_pair drops the old bond and pairs the MAC it is handed, so an
       * ambiguous auto-pick is destructive and silent. Deleting this rule
       * passed the entire gate while it lived in main.c. */
      ck(scan_pick_candidate(0, 0) == -1, "no devices: nothing to pick");
      {
         int one[1] = {-80};
         ck(scan_pick_candidate(one, 1) == 0,
            "a single device is unambiguous whatever its signal");
      }
      {
         int clear[3] = {-90, -40, -85}; /* index 1 leads by 45 dB */
         ck(scan_pick_candidate(clear, 3) == 1,
            "a clear leader is picked, by MODEL index not position");
      }
      {
         int tie[2] = {-60, -61}; /* 1 dB apart */
         ck(scan_pick_candidate(tie, 2) == -1,
            "two similar signals are refused -- the user must choose");
      }
      {
         int edge[2] = {-40, -40 - SCAN_AMBIG_DB}; /* exactly the threshold */
         ck(scan_pick_candidate(edge, 2) == 0,
            "exactly SCAN_AMBIG_DB apart is decisive");
         int inside[2] = {-40, -40 - (SCAN_AMBIG_DB - 1)};
         ck(scan_pick_candidate(inside, 2) == -1,
            "one dB inside the threshold is not");
      }
      {
         /* The runner-up must be the true second, not merely the next in
          * array order -- otherwise a distant third can mask a near tie. */
         int masked[3] = {-41, -40, -95};
         ck(scan_pick_candidate(masked, 3) == -1,
            "a near tie is refused even with a distant third present");
      }
   }

   printf("== a late scan failure: whose scan was it? ==\n");
   {
      /* THE FAILURE THIS PINS. startScan() reports success and the platform
       * refuses afterwards, on a binder thread. Native had already latched
       * "scanning", every recovery path is gated on that flag being clear, and
       * it was cleared in exactly one place -- a later successful stop. So the
       * app stopped scanning for the life of the process: no reconnect after a
       * dropout, no meter noticed when switched on, nothing on screen. */
      struct scan_fail live = {.failed_gen = 4, .cur_gen = 4, .scanning = 1};
      ck(scan_fail_applies(&live) == 1,
         "the live scan's own failure must reset the state");

      /* SUPERSEDED. Chosen so ONLY the generation differs: something is
       * installed and native believes it is scanning, exactly as in the live
       * case, so this case can only be refused by the generation comparison. */
      struct scan_fail old = {.failed_gen = 4, .cur_gen = 5, .scanning = 1};
      ck(scan_fail_applies(&old) == 0,
         "a failure for a REPLACED scan must not cancel the newer one");

      /* NO GENERATION AT ALL. cur_gen is 0 too -- the state before the first
       * start -- so the generations MATCH and `scanning` is set: this case is
       * refused only by the failed_gen <= 0 rule, and drops through to
       * "current" the moment that rule is deleted. */
      struct scan_fail none = {.failed_gen = 0, .cur_gen = 0, .scanning = 1};
      ck(scan_fail_applies(&none) == 0,
         "a failure carrying no generation is not the current scan's");
      struct scan_fail neg = {.failed_gen = -1, .cur_gen = -1, .scanning = 1};
      ck(scan_fail_applies(&neg) == 0, "...nor is a negative one");

      /* ALREADY RESET. Generations match, so only the `scanning` rule can
       * refuse it. The platform really does deliver more than one failure per
       * registration, and a second reset would restamp the back-off (pushing
       * the retry a further interval out for every duplicate) and clear a stop
       * that is outstanding for an unrelated reason. */
      struct scan_fail again = {.failed_gen = 4, .cur_gen = 4, .scanning = 0};
      ck(scan_fail_applies(&again) == 0,
         "a second failure for the same scan changes nothing");
   }

   printf("== the failure back-off is the retry throttle, not a bypass ==\n");
   {
      long now = 100000;
      ck(scan_fail_retry_at(now) == now + SCAN_RETRY_S,
         "a failure holds the next start off by the self-heal interval");
      ck(scan_start_allowed(now, scan_fail_retry_at(now)) == 0,
         "so a start attempted immediately after a failure is refused");
      ck(scan_start_allowed(now + SCAN_RETRY_S - 1, scan_fail_retry_at(now)) ==
             0,
         "one second short of the interval is still refused");
      ck(scan_start_allowed(now + SCAN_RETRY_S, scan_fail_retry_at(now)) == 1,
         "at the interval the retry is allowed");
      ck(scan_start_allowed(0, 0) == 1,
         "with no failure recorded, a start is never held off");
   }

   printf("== the sequences that actually bit ==\n");
   {
      /* THE MODEL IS DRIVEN, NOT NARRATED.
       *
       * An earlier version of this block inlined each transition beside its
       * assertion, which made one check ("the scheduled retry did not move")
       * unfailable: the test itself never performed the restamp it claimed to
       * rule out, so deleting the rule that prevents it changed nothing here.
       * model_start/model_fail apply the state change whenever scanlogic
       * permits it, so every assertion below is about state the RULES
       * produced. */
      struct scan_model m = {.now = 1000};

      /* 1. THE HEALING SEQUENCE. */
      int g1 = model_start(&m);
      ck(g1 == 1 && m.scanning == 1, "a first start goes out and is latched");
      ck(model_fail(&m, g1, 6) == 1, /* SCANNING_TOO_FREQUENTLY */
         "its own failure is recognised and acted on");
      ck(m.scanning == 0, "the latch is CLEARED -- this is the whole defect");
      ck(m.retry_after == 1000 + SCAN_RETRY_S,
         "...and the retry is SCHEDULED, not immediate");
      long scheduled = m.retry_after;

      /* 2. A DUPLICATE FAILURE MUST NOT PUSH THE RETRY FURTHER OUT. The stack
       * delivers more than one per registration; without the already-reset
       * rule each would restamp the back-off, and while failures kept arriving
       * the retry would never come due -- the same permanent outage, by
       * arithmetic instead of by a latch. */
      m.now += 5;
      ck(model_fail(&m, g1, 6) == 0, "the duplicate is refused");
      ck(m.retry_after == scheduled,
         "...so the scheduled retry did not move (no double-retry)");

      /* 3. THE THROTTLE HOLDS UNTIL IT IS DUE. A start attempted inside the
       * back-off must not go out: for failure 6 that call is literally what
       * extends Android's block. */
      ck(model_start(&m) == 0, "five seconds later the retry is refused");
      ck(m.gen == g1, "...and no generation was spent on it");
      m.now  = scheduled;
      int g2 = model_start(&m);
      ck(g2 == g1 + 1 && m.scanning == 1,
         "at the deadline a NEW generation goes out");

      /* 4. A LATE FAILURE FOR THE FIRST SCAN MUST NOT UNDO THE SECOND. This is
       * the reason for generations at all: the reset is a real action -- it
       * tells the app its scan is dead and holds starts off for an interval --
       * so applying it for a scan that no longer exists is the original outage
       * reintroduced by the fix for the original outage. */
      ck(model_fail(&m, g1, 3) == 0,
         "the first scan's failure is refused after the second started");
      ck(m.scanning == 1, "...the working scan is still believed live");
      ck(m.last_err == 6 && m.retry_after == scheduled,
         "...and it left no back-off or message behind either");

      /* 5. THE SECOND SCAN'S OWN FAILURE IS STILL ACTED ON, so the staleness
       * rule cannot be satisfied by ignoring everything. */
      ck(model_fail(&m, g2, 3) == 1, "the live generation still gets through");
      ck(m.scanning == 0 && m.last_err == 3 &&
             m.retry_after == m.now + SCAN_RETRY_S,
         "...resetting the latch and rescheduling from NOW");
   }

   printf("== what the user is told ==\n");
   {
      /* One generic message for all six codes is what left people rebooting
       * phones: "the radio is busy" and "Android is blocking us for scanning
       * too often" ask for different patience. */
      const char *seen[7];
      int distinct = 1, present = 1, named = 1;
      for (int e = 1; e <= 6; e++) {
         seen[e] = scan_fail_text(e);
         if (!seen[e] || !seen[e][0])
            present = 0;
         /* 25 COLUMNS, not 33: update_screen renders the row as
          * "PANCRA  %.*s" with MAX_COLS - 8, so anything longer is truncated
          * mid-word and the cause -- which is the last word of most of these --
          * is the part that falls off the screen. */
         int n = 0;
         while (seen[e][n])
            n++;
         if (n > 33 - 8)
            present = 0;
         /* A KNOWN CAUSE MUST BE NAMED. Distinctness alone does not say this:
          * one of the six collapsing into the unknown-code fallback leaves the
          * remaining five distinct, so that check stays green over exactly the
          * regression it looks like it covers (demonstrated by mutation). */
         if (strcmp(seen[e], scan_fail_text(0)) == 0)
            named = 0;
         for (int p = 1; p < e; p++)
            if (seen[p] == seen[e] || strcmp(seen[p], seen[e]) == 0)
               distinct = 0;
      }
      ck(present == 1, "every ScanCallback code has a short message");
      ck(distinct == 1, "and no two codes say the same thing");
      ck(named == 1, "and none of them falls back to the generic line");
      ck(strcmp(scan_fail_text(6), "SCAN THROTTLED") == 0,
         "SCANNING_TOO_FREQUENTLY (6) names Android's own block");
      ck(strcmp(scan_fail_text(0), "SCAN FAILED") == 0,
         "a code this build never heard of is still a failure");
      ck(strcmp(scan_fail_text(7), "SCAN FAILED") == 0,
         "...including a seventh constant Android has yet to add");
   }

   printf("\n%s\n", all ? "ALL SCAN TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
