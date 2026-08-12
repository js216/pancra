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

   printf("\n%s\n", all ? "ALL SCAN TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
