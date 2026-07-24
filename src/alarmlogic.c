// SPDX-License-Identifier: GPL-3.0
// alarmlogic.c --- Pure alarm decision logic (host-testable)
// Copyright 2026 Jakob Kastelic

/* See alarmlogic.h. Pure by design: no globals, no clock, no locks, no JNI. */
#include "alarmlogic.h"

int alarm_zone(int glu, long glu_t, long now, int lo, int hi)
{
   int fresh = (glu >= 0 && now - glu_t <= AL_FRESH_S);
   if (!fresh)
      return 0;
   if (glu < lo)
      return 1;
   if (glu > hi)
      return 2;
   return 0;
}

int alarm_stale(int glu, long glu_t, long now, long launch_t, long disc_s)
{
   if (disc_s <= 0)
      return 0;
   if (now - launch_t < disc_s) /* launch grace */
      return 0;
   return (glu < 0 || now - glu_t > disc_s);
}

int alarm_zone_merge(int a, int b)
{
   /* Combine verdicts from DIFFERENT sensors: the alarm watches every CGM
    * the user wears, and the worst excursion anywhere wins. A LOW outranks a
    * HIGH -- hypoglycemia is the one that kills quickly -- so two sensors
    * disagreeing in opposite directions ring LOW. */
   if (a == 1 || b == 1)
      return 1;
   if (a == 2 || b == 2)
      return 2;
   return 0;
}

int alarm_stranded(int glu, long glu_t, long now, int lo, int hi)
{
   if (glu < 0)
      return 0; /* never had a reading */
   if (now - glu_t <= AL_FRESH_S)
      return 0; /* still fresh: the zone rules apply */
   return glu < lo || glu > hi;
}

int alarm_want(int zone, int stale)
{
   if (zone == 2)
      return AL_HIGH;
   if (zone)
      return AL_LOW;
   if (stale)
      return AL_STALE;
   return AL_NONE;
}

int alarm_want_sustained(int zone, int stale, int stranded, int prev_want)
{
   /* A stranded GLUCOSE alarm outranks anything the fresh rules produce,
    * because the underlying fact has not changed -- only our sight of it.
    *
    * Gating this on `want == AL_NONE` was wrong whenever the user had the
    * DISCONNECT alarm enabled: alarm_stale then returns 1, so want is AL_STALE
    * rather than AL_NONE, the sustain branch was skipped, and a hypo the user
    * had already dismissed came back as a NEW alarm labelled "Sensor
    * disconnected". That discards the acknowledgement and downgrades the
    * severity of the one alarm that must never be downgraded -- alarm_want's
    * own ranking says a glucose excursion outranks a stale warning, and this
    * is where that ranking was being dropped.
    *
    * Requiring prev_want to be a glucose level is also what stops this
    * ORIGINATING an alarm: with nothing sounding there is nothing to sustain.
    */
   if (stranded && (prev_want == AL_LOW || prev_want == AL_HIGH))
      return prev_want;
   return alarm_want(zone, stale);
}

int alarm_audible(int want, int sound_on, int vib_on)
{
   return want != AL_NONE && (sound_on || vib_on);
}

int alarm_java_kind(int want)
{
   if (want == AL_LOW)
      return 0;
   if (want == AL_HIGH)
      return 1;
   if (want == AL_STALE)
      return 2;
   return -1;
}

void alarm_step(int which, int *lo, int *hi)
{
   int *v = (which < 2) ? lo : hi;
   *v += ((unsigned)which & 1U) ? AL_STEP : -AL_STEP; /* even minus, odd plus */
   if (*v < AL_MIN)
      *v = AL_MIN;
   if (*v > AL_MAX)
      *v = AL_MAX;
   if (*lo > *hi) { /* keep low <= high by moving the one NOT just touched */
      if (which < 2)
         *lo = *hi;
      else
         *hi = *lo;
   }
}

void alarm_decide(int want, int prev_want, int sound_on, int vib_on,
                  struct alarm_out *o)
{
   o->want     = prev_want;
   o->sounding = -1; /* caller keeps its current value */
   o->acked    = -1;
   if (want == prev_want) {
      o->act = AL_ACT_NONE; /* idempotent on the level */
      return;
   }
   o->want = want;
   /* A CHANGED level is a new alarm; any previous dismissal does not carry. */
   o->acked    = 0;
   o->sounding = alarm_audible(want, sound_on, vib_on);
   o->act      = (want == AL_NONE) ? AL_ACT_SILENCE : AL_ACT_TRIGGER;
}

int alarm_reactuate_allowed(int acked)
{
   return !acked;
}

int cal_entry_mgdl(const char *digits, int n, int units)
{
   if (n <= 0)
      return -1;
   int v = 0;
   for (int i = 0; i < n; i++) {
      if (digits[i] < '0' || digits[i] > '9')
         return -1;
      if (v > 100000) /* cannot become valid; stop before it overflows */
         return -1;
      v = (v * 10) + (digits[i] - '0');
   }
   int mgdl = units ? (v * 18) / 10 : v;
   if (mgdl < CAL_MIN_MGDL || mgdl > CAL_MAX_MGDL)
      return -1;
   return mgdl;
}
