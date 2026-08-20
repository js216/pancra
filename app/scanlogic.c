// SPDX-License-Identifier: GPL-3.0
// scanlogic.c --- Pure scan-lifecycle decision (host-testable)
// Copyright 2026 Jakob Kastelic

/* See scanlogic.h. Pure by design: no globals, no clock, no locks, no JNI.
 * (It names enum mono_get, which is a TYPE from clock.h; it calls nothing.) */
#include "scanlogic.h"

int scan_should_start(const struct scan_state *s)
{
   if (!s->have_activity || s->paused)
      return 0; /* nothing to scan for, or deliberately down */
   if (s->scanning)
      return 0; /* already up; start_scan is idempotent but this is the gate */
   if (s->pairing || s->meter_busy)
      return 0; /* someone else owns the radio */
   if (s->now < s->hold_until)
      return 0; /* quiet window for a bonding connect */
   /* last_attempt == 0 means "never tried", which must NOT be throttled --
    * otherwise the very first heal after a failed stop waits a full interval
    * for no reason. */
   if (s->last_attempt && s->now - s->last_attempt < SCAN_RETRY_S)
      return 0;
   return 1;
}

int scan_pick_candidate(const int *rssi, int n)
{
   if (n <= 0)
      return -1;
   if (n == 1)
      return 0; /* nothing to be ambiguous with */
   int best = 0;
   for (int i = 1; i < n; i++)
      if (rssi[i] > rssi[best])
         best = i;
   int second = -1;
   for (int i = 0; i < n; i++)
      if (i != best && (second < 0 || rssi[i] > rssi[second]))
         second = i;
   if (second < 0 || rssi[best] - rssi[second] >= SCAN_AMBIG_DB)
      return best;
   return -1; /* too close to call: make the user choose */
}

/* THREE REFUSALS, EACH FROM A DIFFERENT WAY OF GETTING THIS WRONG. */
int scan_fail_applies(const struct scan_fail *f)
{
   /* A generation was never handed out for this failure. Nothing can be
    * matched against it, and 0 is also what an unbound or mis-registered
    * native method would deliver -- which must not resolve to "the current
    * scan" merely because the current generation happens to be 0 too (it is,
    * before the first start). */
   if (f->failed_gen <= 0)
      return 0;
   /* SUPERSEDED. The scan this failure belongs to has already been replaced by
    * a newer one, which may be working perfectly; resetting on its behalf
    * would tear down a live scan and call it recovery. */
   if (f->failed_gen != f->cur_gen)
      return 0;
   /* ALREADY RESET. The platform delivers more than one onScanFailed for a
    * single registration (an error while starting, then another when the
    * scanner is torn down), and a stop can have won the race in between. A
    * second reset would restamp the back-off -- pushing the retry a further
    * interval out every time a duplicate arrived -- and clear a stop that is
    * still outstanding for a DIFFERENT reason. */
   if (!f->scanning)
      return 0;
   return 1;
}

long scan_fail_retry_at(long now)
{
   return now + SCAN_RETRY_S;
}

int scan_start_allowed(long now, long retry_after)
{
   return now >= retry_after;
}

const char *scan_fail_text(int err)
{
   /* android.bluetooth.le.ScanCallback's own numbering. */
   switch (err) {
      case 1: /* SCAN_FAILED_ALREADY_STARTED */ return "SCAN ALREADY UP";
      case 2: /* SCAN_FAILED_APPLICATION_REGISTRATION_FAILED */
         return "SCAN NOT REGISTERED";
      case 3: /* SCAN_FAILED_INTERNAL_ERROR */ return "BLUETOOTH ERROR";
      case 4: /* SCAN_FAILED_FEATURE_UNSUPPORTED */ return "SCAN UNSUPPORTED";
      case 5: /* SCAN_FAILED_OUT_OF_HARDWARE_RESOURCES */ return "RADIO BUSY";
      case 6: /* SCAN_FAILED_SCANNING_TOO_FREQUENTLY */ return "SCAN THROTTLED";
      default:
         /* A code this build has never heard of is still a real failure, and
          * the user must not be told the scan is fine because Android added a
          * seventh constant. */
         return "SCAN FAILED";
   }
}

/* ---- THE FOUR LIVENESS DEADLINES (see the block in scanlogic.h) -----------
 *
 * The shared core, and then four one-line predicates over it. The four are
 * NOT collapsed into one exported call: each is a deadline that can be
 * reverted to the wall clock on its own, and a suite has to be able to show
 * that it fails for each of them separately. One shared entry point makes all
 * four the same edit.
 *
 * `now->wall` is untouched here by construction. It travels in the struct
 * because the pairing is the invariant -- a record has an instant AND an age
 * origin -- and because the callers' log lines want it; the moment a
 * comparison in this file reaches for it, that is the bug back. */
static int elapsed_at_least(long stamp, const struct live_now *now,
                            long interval)
{
   if (now->ok != MONO_GET_OK)
      return 0; /* cannot measure -> refuse; see the header */
   if (stamp == MONO_NEVER)
      return 1; /* never stamped: a first attempt must not wait an interval */
   if (now->mono < stamp)
      /* CLOCK_MONOTONIC does not go backwards, so this is not a rollback --
       * it is a stamp from another boot (a value restored, or a struct never
       * cleared). An age that cannot be computed is not evidence of
       * liveness, and the deadline it guards exists to recover things. */
      return 1;
   return (now->mono - stamp) > interval;
}

static int within(long stamp, const struct live_now *now, long window)
{
   if (now->ok != MONO_GET_OK)
      return 0; /* cannot measure -> not fresh */
   if (stamp == MONO_NEVER)
      return 0; /* never measured -> nothing to be fresh */
   if (now->mono < stamp)
      return 0; /* see elapsed_at_least: an uncomputable age proves nothing */
   return (now->mono - stamp) < window;
}

int live_silence_due(const struct live_stamp *rx, const struct live_now *now)
{
   return elapsed_at_least(rx->mono, now, LIVE_SILENCE_S);
}

int live_kick_due(const struct live_stamp *kick, const struct live_now *now)
{
   return elapsed_at_least(kick->mono, now, LIVE_KICK_MIN_S);
}

int live_rssi_fresh(const struct live_stamp *meas, const struct live_now *now)
{
   return within(meas->mono, now, LIVE_RSSI_FRESH_S);
}

int live_dis_due(const struct live_stamp *req, const struct live_now *now)
{
   return elapsed_at_least(req->mono, now, LIVE_DIS_RETRY_S);
}
