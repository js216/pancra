// SPDX-License-Identifier: GPL-3.0
// scanlogic.c --- Pure scan-lifecycle decision (host-testable)
// Copyright 2026 Jakob Kastelic

/* See scanlogic.h. Pure by design: no globals, no clock, no locks, no JNI. */
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
