// SPDX-License-Identifier: GPL-3.0
// clock.c --- the two clocks (see clock.h)
// Copyright 2026 Jakob Kastelic
#include "clock.h"
#include "sysabi.h" /* the clock ids, declared once for the whole app */
#include <stdint.h>
#include <time.h>

/* ts is ZERO-INITIALISED and the return is checked, in both of these.
 *
 * clock_gettime on a valid clock id is a vDSO call that essentially cannot
 * fail, but "essentially cannot" was left resting on an UNINITIALISED struct:
 * on failure these returned whatever the stack held. realtime_s() stamps every
 * row of the append-only reading log and drives every staleness and alarm
 * comparison, so a garbage value there is not a wrong pixel, it is a fabricated
 * timestamp written permanently. Returning 0 instead is a failure the rest of
 * the code already rejects -- store_load drops rows with t <= 0. */
long long now_ms(void)
{
   struct timespec ts = {0, 0};
   if (clock_gettime(SYS_CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000);
}

/* MONOTONIC SECONDS, for every deadline that lives inside this process.
 *
 * The invariant this exists to make sayable: realtime_s() identifies an
 * INSTANT -- a reading's timestamp, a dose's, anything written to a file or
 * shown to a person -- and mono_s() measures an INTERVAL: watchdogs, retry
 * backoffs, cooldowns, the radio-quiet hold around a pairing.
 *
 * They were the same call, which means a wall-clock correction moved every
 * deadline in the app. A phone that syncs its clock forward an hour fires the
 * meter's 90-second sync watchdog instantly, tearing down an exchange that
 * was working; a correction backwards postpones it by an hour, so a genuinely
 * wedged link is never recovered. Neither is rare -- a phone that has been
 * off, or has just found a network, does exactly this. */
int64_t mono_s(void)
{
   int64_t t = 0;
   /* The lossy form, kept for the callers that only want a number: a failed
    * read reads as second zero. Every DEADLINE goes through mono_try()
    * instead, because for a deadline that conflation is the whole bug --
    * see clock.h. */
   if (mono_try(&t) != MONO_GET_OK)
      return 0;
   return t;
}

/* THE SAME READ, WITH THE FAILURE KEPT. See clock.h for why the outcome has
 * to travel separately from the value, and scanlogic.h for what the four BLE
 * deadlines do with MONO_GET_FAIL.
 *
 * *out is LEFT ALONE on failure rather than zeroed: a caller that ignores the
 * outcome despite warn_unused_result then keeps whatever it had, which for a
 * re-read of an already-stamped deadline is the previous instant -- stale,
 * but not a fabricated "second zero" that makes every age look enormous. */
enum mono_get mono_try(int64_t *out)
{
   struct timespec ts = {0, 0};
   if (clock_gettime(SYS_CLOCK_MONOTONIC, &ts) != 0)
      return MONO_GET_FAIL;
   *out = ts.tv_sec;
   return MONO_GET_OK;
}

int64_t realtime_s(void)
{
   struct timespec ts = {0, 0};
   if (clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0)
      return 0;
   return ts.tv_sec;
}
