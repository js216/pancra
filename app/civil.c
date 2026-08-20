// SPDX-License-Identifier: GPL-3.0
// civil.c --- a local civil date/time, and the instant it names (see civil.h)
// Copyright 2026 Jakob Kastelic

#include "civil.h"

#define DAY_S 86400L

long civil_days(long y, long m, long d)
{
   y -= m <= 2;
   long era          = (y >= 0 ? y : y - 399) / 400;
   unsigned long yoe = (unsigned long)(y - (era * 400));
   unsigned long doy =
       (((153UL * (unsigned long)(m > 2 ? m - 3 : m + 9)) + 2) / 5) +
       (unsigned long)d - 1;
   unsigned long doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
   return (era * 146097) + (long)doe - 719468;
}

void civil_ymd(long z, long *y, long *m, long *d)
{
   z += 719468;
   long era          = (z >= 0 ? z : z - 146096) / 146097;
   unsigned long doe = (unsigned long)(z - (era * 146097));
   unsigned long yoe =
       (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
   long yy           = (long)yoe + (era * 400);
   unsigned long doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
   unsigned long mp  = ((5 * doy) + 2) / 153;
   *d                = (long)(doy - (((153 * mp) + 2) / 5) + 1);
   *m                = (long)(mp < 10 ? mp + 3 : mp - 9);
   *y                = yy + (*m <= 2);
}

/* SOLVE, DO NOT SUBTRACT.
 *
 * An offset o is the right one for this civil time only if the instant it
 * produces is itself in that offset: naive - o must satisfy
 * zone(naive - o) == o. That is the whole test, and it is what makes the
 * three answers in civil.h fall out rather than having to be special-cased:
 * count the candidate offsets that survive it.
 *
 * WHERE THE CANDIDATES COME FROM. Any offset that can apply within a day
 * either side, plus the offset at `naive` read as though it were an instant.
 * The last one is not a rounding error, it is the cheap seed: it is within
 * fourteen hours of the truth by construction, so on the ordinary path it is
 * already the answer and the one-day probes merely agree. Those probes
 * are what bracket a transition -- a zone changes offset at most once a day
 * in practice, so the offsets in force a day before and a day after are
 * exactly the "before" and "after" of the transition being straddled.
 *
 * A candidate is only ever ACCEPTED after the round trip above, so an extra
 * probe can never invent a solution; at worst it costs a callback. */
struct civil_res civil_resolve(long naive, zone_off_fn zone, void *ctx)
{
   struct civil_res r = {naive, 0, naive, 0, CIVIL_UNIQUE};
   if (!zone)
      return r;
   long before = zone(ctx, naive - DAY_S);
   long probe[3];
   probe[0] = before;
   probe[1] = zone(ctx, naive);
   probe[2] = zone(ctx, naive + DAY_S);

   long sol[3];
   int n = 0;
   for (int i = 0; i < 3; i++) {
      long t   = naive - probe[i];
      int seen = 0;
      for (int k = 0; k < n; k++)
         if (sol[k] == t)
            seen = 1;
      if (seen)
         continue;
      if (zone(ctx, t) == probe[i])
         sol[n++] = t;
   }

   if (n == 0) {
      /* THE SKIPPED HOUR. No offset survives the round trip, so there is no
       * instant to return -- only a decision. Applying the offset in force
       * BEFORE the transition moves the entry forward by exactly the gap
       * (02:30 -> 03:30); applying the one after would move it backward, to
       * a time earlier than the one that was typed. See civil.h. */
      r.fix = CIVIL_NONEXISTENT;
      r.t   = naive - before;
   } else {
      long lo = sol[0];
      long hi = sol[0];
      for (int i = 1; i < n; i++) {
         if (sol[i] < lo)
            lo = sol[i];
         if (sol[i] > hi)
            hi = sol[i];
      }
      /* THE EARLIER INSTANT when there are two. `t_alt` carries the other one
       * out rather than discarding it: the meter walk has record order and
       * overrules this, and a form that has nothing to overrule it with still
       * needs to be able to say that it guessed. */
      r.fix   = n > 1 ? CIVIL_AMBIGUOUS : CIVIL_UNIQUE;
      r.t     = lo;
      r.t_alt = hi;
   }
   /* The offsets are read back FROM the chosen instants, never assumed to be
    * the offsets the arithmetic went through. For a nonexistent civil time
    * those differ by the gap, and it is this one that describes the stamp
    * actually stored. */
   r.off     = zone(ctx, r.t);
   r.off_alt = zone(ctx, r.t_alt);
   return r;
}

/* THE OFFSET AT THE INSTANT BEING SPLIT, not the offset today. Splitting with
 * the wrong one puts the half the user did not retype -- the time of day, or
 * the year February's length is decided by -- on the wrong civil day to begin
 * with, so an edit is already an hour out before the recombination gets its
 * chance. */
static long split_local(long t, zone_off_fn zone, void *ctx, long *y, long *m,
                        long *d)
{
   long local = t + (zone ? zone(ctx, t) : 0);
   long secs  = local % DAY_S;
   long z     = local / DAY_S;
   if (secs < 0) {
      secs += DAY_S; /* C division truncates toward zero; days floor */
      z--;
   }
   civil_ymd(z, y, m, d);
   return secs;
}

void civil_at(long t, zone_off_fn zone, void *ctx, long *y, long *m, long *d)
{
   long yy = 0;
   long mm = 0;
   long dd = 0;
   (void)split_local(t, zone, ctx, &yy, &mm, &dd);
   if (y)
      *y = yy;
   if (m)
      *m = mm;
   if (d)
      *d = dd;
}

struct civil_res civil_reaim(long t, int what, int a, int b, zone_off_fn zone,
                             void *ctx)
{
   long y    = 0;
   long m    = 0;
   long d    = 0;
   long secs = split_local(t, zone, ctx, &y, &m, &d);

   if (what == CIVIL_EDIT_YEAR) {
      y        = a;
      int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
      if (m == 2 && d == 29 && !leap)
         d = 28; /* the one date a year change can delete; see civil.h */
   } else if (what == CIVIL_EDIT_MONTHDAY) {
      m = a;
      d = b;
   } else {
      secs = (a * 3600L) + (b * 60L); /* HHMM is all the keypad can say */
   }
   return civil_resolve((civil_days(y, m, d) * DAY_S) + secs, zone, ctx);
}
