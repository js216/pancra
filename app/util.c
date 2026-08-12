// SPDX-License-Identifier: GPL-3.0
// util.c --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#include "util.h"
#include <time.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

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
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000);
}

long realtime_s(void)
{
   struct timespec ts = {0, 0};
   if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
      return 0;
   return ts.tv_sec;
}

/* snprintf returns the would-be length, which can exceed the buffer on
 * truncation; clamp so write() emits only bytes actually in the buffer. */
int clampn(int n, int cap)
{
   if (n < 0)
      return 0;
   return n >= cap ? cap - 1 : n;
}

void str_snapshot(char *dst, int cap, const char *src)
{
   /* cap <= 0 skipped the copy loop and then wrote dst[0] anyway -- a
    * one-byte overflow into a zero-length buffer. No caller passes 0 today
    * (every one passes a sizeof), but this is the project's single string
    * copy and it should not have a size at which it corrupts memory. */
   if (cap <= 0)
      return;
   int i = 0;
   for (; i < cap - 1 && src[i]; i++)
      dst[i] = src[i];
   dst[i] = 0;
}
