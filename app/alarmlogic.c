// SPDX-License-Identifier: GPL-3.0
// alarmlogic.c --- Pure alarm decision logic (host-testable)
// Copyright 2026 Jakob Kastelic

/* See alarmlogic.h. Pure by design: no globals, no clock, no locks, no JNI. */
#include "alarmlogic.h"

int alarm_zone(int glu, long glu_t, long now, int lo, int hi)
{
   /* data_fresh, not `now - glu_t <= AL_FRESH_S`: a clock rollback makes that
    * difference negative, which passed, so the zone never decayed and the
    * stale/DISCONNECT alarm underneath it could never be reached. `now` and
    * `glu_t` are both REALTIME here, and glu_t stays realtime: it is the
    * reading's identity and the time printed next to it on screen. */
   int fresh = (glu >= 0 && data_fresh(now, glu_t, AL_FRESH_S));
   if (!fresh)
      return 0;
   /* AT the limit counts: the limit is the last acceptable value's neighbour,
    * so 70 with low=70 already rings -- waiting for 69 gives away 5 minutes
    * of a drop the user asked to be told about. Same at the top. */
   if (glu <= lo)
      return 1;
   if (glu >= hi)
      return 2;
   return 0;
}

int chirp_semitone10(int delta_mgdl)
{
   /* Tenths, so the 2 mg/dL-per-semitone rule survives integer division at
    * odd deltas: 1 mg/dL is half a semitone (5 tenths), not 0. */
   int st10 = (delta_mgdl * 10) / CHIRP_MGDL_PER_ST;
   int cap  = CHIRP_MAX_ST * 10;
   if (st10 > cap)
      st10 = cap;
   if (st10 < -cap)
      st10 = -cap;
   return st10;
}

int nudge_zone(int glu, long glu_t, long now, int lo, int hi)
{
   /* -1, not 0. See alarmlogic.h: a dropout must HOLD the latch, and a caller
    * that cannot tell "in range" from "no idea" would clear it instead --
    * re-arming the nudge to fire again every time a flaky link comes back. */
   /* A FUTURE stamp is UNKNOWN too, and -1 is the right answer for it for the
    * same reason a dropout gets -1: we cannot age the sample, so we must not
    * vote "in range" and clear the latch. */
   if (glu < 0 || !data_fresh(now, glu_t, AL_FRESH_S))
      return -1;
   if (glu <= lo) /* inclusive, matching alarm_zone */
      return NG_LOW;
   if (glu >= hi)
      return NG_HIGH;
   return NG_NONE;
}

int nudge_next(int nzone, int prev)
{
   return (nzone < 0) ? prev : nzone;
}

int nudge_fire(int nzone, int alarming, int prev)
{
   if (nzone <= 0 || nzone == prev)
      return NG_NONE; /* in range, unknown, or already announced */
   if (alarming)
      return NG_NONE; /* the alarm has it; the nudge adds nothing */
   return nzone;
}

int alarm_stale(int glu, long glu_t, long now, long mono_now, long launch_mono,
                long disc_s)
{
   if (disc_s <= 0)
      return 0;
   /* THE LAUNCH GRACE IS MEASURED ON THE MONOTONIC CLOCK, and nothing else in
    * this function may look at those two arguments.
    *
    * "How long has this process been running" is an INTERVAL, and it was
    * being computed from two wall-clock readings. A phone that finds a
    * network shortly after boot -- the common case, because that is exactly
    * when the app starts -- steps its clock, and the grace stepped with it:
    * forward past the threshold ended the grace early and fired "Sensor
    * disconnected" over data that was merely waiting for the first sync;
    * backward extended it, and while the grace holds this function returns 0
    * unconditionally, so the user's DISCONNECT alarm was disabled outright
    * for as long as the skew lasted. A backward step of an hour with a 15
    * minute threshold disables it for an hour, silently, on the alarm whose
    * entire job is to say the sensor has stopped reporting.
    *
    * mono_s() cannot be stepped, so this measures the thing it names. */
   if (mono_now - launch_mono < disc_s)
      return 0;
   /* The DATA age, on the realtime clock the reading was stamped with. A
    * future stamp is not "recent": it is unageable, which is precisely the
    * condition this alarm announces, so it must read as stale rather than
    * suppress the transition. */
   return (glu < 0 || !data_fresh(now, glu_t, disc_s));
}

int alarm_pred_low(int pred_mgdl, long pred_mono, long now_mono)
{
   /* 0 mg/dL is "no prediction recorded"; a stamp of 0 is "this link has
    * never reported one". Both are the pre-first-reading state of the arrays
    * this reads in alarm.c, and neither may resolve to an alarm. */
   if (pred_mgdl <= 0 || pred_mono <= 0)
      return 0;
   if (pred_mgdl >= PRED_LOW_MGDL)
      return 0;
   /* MONOTONIC on both sides. The prediction is an in-process fact -- it is
    * never persisted and never displayed -- so the honest stamp for it is
    * when it ARRIVED, not what the wall clock said at the time. With realtime
    * stamps a backward correction made every recorded prediction
    * indefinitely fresh, and this is the alarm that overrides the user's
    * sound setting and re-triggers itself after every silence: a stale
    * prediction preserved this way is an alarm that cannot be switched off
    * and is no longer about anything. The nonnegative half of data_fresh is
    * belt and braces here (a monotonic clock does not go backwards) and is
    * kept so the rule reads the same everywhere it appears. */
   return data_fresh(now_mono, pred_mono, AL_FRESH_S);
}

/* Seconds occupy the low 48 bits; the prediction the top 16. See alarmlogic.h
 * for why the pair travels as one word at all. */
#define PRED_MONO_MASK 0xffffffffffffULL
#define PRED_MONO_MAX  ((long)PRED_MONO_MASK)

unsigned long long pred_pack(int mgdl, long mono)
{
   /* CLAMP, DO NOT TRUNCATE, at both ends of both fields.
    *
    * A masked-off high bit does not produce a slightly wrong number, it
    * produces a plausible one: a stamp that wraps reads as an arrival time
    * that never happened, and this feeds the alarm that overrides a muted
    * phone. Clamping keeps a nonsense input nonsense -- 0 means "nothing
    * recorded", which alarm_pred_low refuses outright. */
   if (mgdl < 0)
      mgdl = 0;
   if (mgdl > PRED_MGDL_MAX)
      mgdl = PRED_MGDL_MAX;
   if (mono < 0)
      mono = 0;
   if (mono > PRED_MONO_MAX)
      mono = PRED_MONO_MAX;
   return ((unsigned long long)(unsigned int)mgdl << 48) |
          ((unsigned long long)mono & PRED_MONO_MASK);
}

struct link_pred pred_unpack(unsigned long long word)
{
   struct link_pred p;
   p.mgdl = (int)(word >> 48);
   p.mono = (long)(word & PRED_MONO_MASK);
   return p;
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
   /* Only a stamp we can actually AGE hands this back to the zone rules. A
    * future-dated reading used to take this early return -- so the one rule
    * that keeps a ringing hypo alive through a dropout switched itself off at
    * the exact moment the data became untrustworthy, and alarm_want then
    * returned AL_NONE and SILENCED it. Refusing the future stamp means the
    * sustain applies; it can still only sustain, never originate. */
   if (data_fresh(now, glu_t, AL_FRESH_S))
      return 0;                   /* still fresh: the zone rules apply */
   return glu <= lo || glu >= hi; /* inclusive, matching alarm_zone */
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

void alarm_plan_next(int zone, int stale, int stranded, int acked, int pred_low,
                     int prev_want, int sounding, int sound_on,
                     struct alarm_plan *out)
{
   /* A DISMISSED level must stop sustaining: the sustain exists to stop a
    * RINGING alarm being silenced by its own data ageing out, and once the
    * user has dismissed it nothing is ringing. */
   out->want = alarm_want_sustained(zone, stale, stranded && !acked, prev_want);
   out->sound_on  = sound_on;
   out->prev_want = prev_want;
   if (pred_low) {
      out->want     = AL_LOW;
      out->sound_on = 1; /* heard regardless of the SOUND setting */
      if (!sounding)
         out->prev_want = AL_NONE; /* force a fresh edge: see the header */
   }
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

void alarm_actuate_step(const struct alarm_state *st,
                        const struct alarm_obs *obs, struct alarm_state *next,
                        struct alarm_effect *eff)
{
   struct alarm_plan pl;
   alarm_plan_next(obs->zone, obs->stale, obs->stranded, st->acked,
                   obs->pred_low, st->want, st->sounding, obs->sound_on, &pl);
   struct alarm_out out;
   alarm_decide(pl.want, pl.prev_want, pl.sound_on, obs->vib_on, &out);

   *next      = *st;
   eff->act   = out.act;
   eff->kind  = -1;
   eff->sound = pl.sound_on;
   eff->vib   = obs->vib_on;
   if (out.act == AL_ACT_NONE)
      return; /* nothing changed: do not re-chime, and commit nothing */
   next->want     = out.want;
   next->acked    = out.acked;
   next->sounding = out.sounding;
   if (out.act == AL_ACT_TRIGGER)
      eff->kind = alarm_java_kind(out.want);
}
