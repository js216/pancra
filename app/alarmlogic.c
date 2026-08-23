// SPDX-License-Identifier: GPL-3.0
// alarmlogic.c --- Pure alarm decision logic (host-testable)
// Copyright 2026 Jakob Kastelic

/* See alarmlogic.h. Pure by design: no globals, no clock, no locks, no JNI. */
#include "alarmlogic.h"

#include "readingrec.h" /* struct reading: the LAYOUT, not the store */
#include "sensors.h" /* KIND_CGM: a fingerstick neither ends nor extends a streak */

enum alarm_level alarm_zone(int glu, long glu_t, long now, int lo, int hi)
{
   /* data_fresh, not `now - glu_t <= AL_FRESH_S`: a clock rollback makes that
    * difference negative, which passed, so the zone never decayed and the
    * stale/DISCONNECT alarm underneath it could never be reached. `now` and
    * `glu_t` are both REALTIME here, and glu_t stays realtime: it is the
    * reading's identity and the time printed next to it on screen. */
   bool fresh = (bool)(glu >= 0 && data_fresh(now, glu_t, AL_FRESH_S));
   if (!fresh)
      return AL_NONE;
   /* AT the limit counts: the limit is the last acceptable value's neighbour,
    * so 70 with low=70 already rings -- waiting for 69 gives away 5 minutes
    * of a drop the user asked to be told about. Same at the top. */
   if (glu <= lo)
      return AL_LOW;
   if (glu >= hi)
      return AL_HIGH;
   return AL_NONE;
}

enum nudge_mode nudge_mode_of(int stored)
{
   /* NAMED, NOT RANGE-CHECKED. `stored >= ND_OFF && stored <= ND_CHIRP` is
    * the same answer today and stops being one the moment a fourth mode is
    * added out of order; and it reads as arithmetic on a type that has no
    * arithmetic. Anything unrecognised is ND_OFF: a settings file written by
    * a future version, or edited by hand, must not sound something nothing
    * here can describe. */
   if (stored == ND_BEEP)
      return ND_BEEP;
   if (stored == ND_CHIRP)
      return ND_CHIRP;
   return ND_OFF;
}

enum nudge_mode nudge_mode_next(enum nudge_mode m)
{
   if (m == ND_OFF)
      return ND_BEEP;
   if (m == ND_BEEP)
      return ND_CHIRP;
   return ND_OFF;
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

enum nudge_band nudge_zone(int glu, long glu_t, long now, int lo, int hi)
{
   /* NG_UNKNOWN, not NG_NONE. See alarmlogic.h: a dropout must HOLD the
    * latch, and a caller that cannot tell "in range" from "no idea" would
    * clear it instead -- re-arming the nudge to fire again every time a
    * flaky link comes back.
    *
    * A FUTURE stamp is UNKNOWN too, for the same reason a dropout is: we
    * cannot age the sample, so we must not vote "in range" and clear the
    * latch. */
   if (glu < 0 || !data_fresh(now, glu_t, AL_FRESH_S))
      return NG_UNKNOWN;
   if (glu <= lo) /* inclusive, matching alarm_zone */
      return NG_LOW;
   if (glu >= hi)
      return NG_HIGH;
   return NG_NONE;
}

enum nudge_band nudge_next(enum nudge_band nzone, enum nudge_band prev)
{
   return (nzone == NG_UNKNOWN) ? prev : nzone;
}

enum nudge_band nudge_fire(enum nudge_band nzone, bool alarming,
                           enum nudge_band prev)
{
   if (nzone == NG_UNKNOWN || nzone == NG_NONE || nzone == prev)
      return NG_NONE; /* unknown, in range, or already announced */
   if (alarming)
      return NG_NONE; /* the alarm has it; the nudge adds nothing */
   return nzone;
}

bool alarm_stale(int glu, long glu_t, long now, long mono_now, long launch_mono,
                 long disc_s)
{
   if (disc_s <= 0)
      return false;
   /* THE LAUNCH GRACE IS MEASURED ON THE MONOTONIC CLOCK, and nothing else in
    * this function may look at those two arguments.
    *
    * "How long has this process been running" is an INTERVAL, and two
    * wall-clock readings cannot measure one. A phone that finds a
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
      return false;
   /* The DATA age, on the realtime clock the reading was stamped with. A
    * future stamp is not "recent": it is unageable, which is precisely the
    * condition this alarm announces, so it must read as stale rather than
    * suppress the transition. */
   return (bool)(glu < 0 || !data_fresh(now, glu_t, disc_s));
}

bool alarm_pred_low(int pred_mgdl, long pred_mono, long now_mono)
{
   /* 0 mg/dL is "no prediction recorded"; a stamp of 0 is "this link has
    * never reported one". Both are the pre-first-reading state of the arrays
    * this reads in alarm.c, and neither may resolve to an alarm. */
   if (pred_mgdl <= 0 || pred_mono <= 0)
      return false;
   if (pred_mgdl >= PRED_LOW_MGDL)
      return false;
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
   return ((unsigned long long)(unsigned int)mgdl << 48U) |
          ((unsigned long long)mono & PRED_MONO_MASK);
}

struct link_pred pred_unpack(unsigned long long word)
{
   struct link_pred p;
   p.mgdl = (int)(word >> 48U);
   p.mono = (long)(word & PRED_MONO_MASK);
   return p;
}

enum alarm_level alarm_zone_merge(enum alarm_level a, enum alarm_level b)
{
   /* Combine verdicts from DIFFERENT sensors: the alarm watches every CGM
    * the user wears, and the worst excursion anywhere wins. A LOW outranks a
    * HIGH -- hypoglycemia is the one that kills quickly -- so two sensors
    * disagreeing in opposite directions ring LOW. */
   if (a == AL_LOW || b == AL_LOW)
      return AL_LOW;
   if (a == AL_HIGH || b == AL_HIGH)
      return AL_HIGH;
   return AL_NONE;
}

bool alarm_stranded(int glu, long glu_t, long now, int lo, int hi)
{
   if (glu < 0)
      return false; /* never had a reading */
   /* Only a stamp we can actually AGE hands this back to the zone rules. A
    * future-dated reading must NOT take this early return: the one rule that
    * keeps a ringing hypo alive through a dropout would switch itself off at
    * the exact moment the data became untrustworthy, and alarm_want would
    * return AL_NONE and SILENCE it. Refusing the future stamp means the
    * sustain applies; it can still only sustain, never originate. */
   if (data_fresh(now, glu_t, AL_FRESH_S))
      return false; /* still fresh: the zone rules apply */
   /* inclusive, matching alarm_zone */
   return (bool)(glu <= lo || glu >= hi);
}

enum alarm_level alarm_want_sustained(enum alarm_level zone, bool stale,
                                      bool stranded,
                                      enum alarm_level prev_want)
{
   if (zone != AL_NONE)
      return zone; /* fresh data, out of range: the zone rules decide */
   if (stale)
      return AL_STALE;
   /* SUSTAIN ONLY, AND UNDER THE LEVEL IT IS ALREADY SOUNDING. Both halves
    * are the point: with nothing committed there is nothing to sustain, and
    * returning anything other than prev_want would relabel an alarm the user
    * has already heard. See alarmlogic.h. */
   if (stranded && prev_want != AL_NONE)
      return prev_want;
   return AL_NONE;
}

bool alarm_audible(enum alarm_level want, bool sound_on, bool vib_on)
{
   return (bool)(want != AL_NONE && (sound_on || vib_on));
}

enum java_kind alarm_java_kind(enum alarm_level want)
{
   if (want == AL_LOW)
      return AJ_LOW;
   if (want == AL_HIGH)
      return AJ_HIGH;
   if (want == AL_STALE)
      return AJ_STALE;
   return AJ_NONE;
}

void alarm_decide(enum alarm_level want, enum alarm_level prev_want,
                  bool sound_on, bool vib_on, struct alarm_out *o)
{
   /* THE IDEMPOTENCE CHECK, and it is what keeps one alarm one alarm: this
    * runs on every tick and on every arriving reading, and a level that has
    * not changed has already been announced. Nothing is committed either --
    * the caller keeps the state it has. */
   if (want == prev_want) {
      o->act      = AL_ACT_NONE;
      o->want     = prev_want;
      o->sounding = AL_KEEP;
      o->acked    = AL_KEEP;
      return;
   }
   if (want == AL_NONE) {
      o->act      = AL_ACT_SILENCE;
      o->want     = AL_NONE;
      o->sounding = 0;
      /* The acknowledgement goes with the alarm it acknowledged: the next
       * level to arrive is one the user has not seen. */
      o->acked = 0;
      return;
   }
   o->act  = AL_ACT_TRIGGER;
   o->want = want;
   /* ARMED ONLY IF THERE IS SOMETHING TO SILENCE. With both outputs off the
    * announcement is silent, and latching `sounding` over it would swallow
    * the user's next tap as a silence gesture. */
   o->sounding = alarm_audible(want, sound_on, vib_on);
   o->acked    = 0;
}

bool alarm_reactuate_allowed(bool acked)
{
   return (bool)!acked;
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

void alarm_plan_next(enum alarm_level zone, bool stale, bool stranded,
                     bool acked, bool pred_low, enum alarm_level prev_want,
                     bool sounding, bool sound_on, struct alarm_plan *out)
{
   out->want      = alarm_want_sustained(zone, stale, stranded, prev_want);
   out->sound_on  = sound_on;
   out->prev_want = prev_want;
   if (!pred_low)
      return;

   /* THE IMMINENT-HYPO OVERRIDE. It outranks the zone rules, and it outranks
    * the user's sound setting: this is the one alarm in the app that is
    * allowed to wake somebody who has switched sound off. */
   out->want     = AL_LOW;
   out->sound_on = true;
   /* AND IT RE-ARMS ITSELF. Whenever the forced level is not actually being
    * heard -- dismissed by a tap (`acked`), or never audible in the first
    * place -- the level alarm_decide compares against is cleared, so it sees
    * a fresh NONE->LOW edge and announces again. That re-trigger is the whole
    * of "cannot be silenced while the prediction holds". */
   if (!sounding || acked)
      out->prev_want = AL_NONE;
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
   eff->kind  = AJ_NONE;
   eff->sound = pl.sound_on;
   eff->vib   = obs->vib_on;
   if (out.act == AL_ACT_NONE)
      return; /* nothing changed: do not re-chime, and commit nothing */
   next->want     = out.want;
   next->acked    = out.acked != 0;
   next->sounding = out.sounding != 0;
   if (out.act == AL_ACT_TRIGGER)
      eff->kind = alarm_java_kind(out.want);
}

/* See alarmlogic.h. Newest first, so the walk starts at the present and stops
 * at the first thing that breaks the run. */
long alarm_streak_s(const struct reading *r, int n, int lo, int hi,
                    long gap_max)
{
   if (!r || n <= 0)
      return 0;
   /* THE BAND IS INCLUSIVE: [lo, hi] is IN range, and only a reading strictly
    * outside it breaks the run.
    *
    * That is the opposite convention from alarm_zone, and deliberately so.
    * This measures against the clinical time-in-range band, and stats.c
    * counts a reading with `glu >= TIR_LOW_MGDL && glu <= TIR_HIGH_MGDL` --
    * so 70 and 180 are in range there. A streak that broke at exactly 70
    * while TIR counted the same reading as good would be two numbers on one
    * screen disagreeing about one sample. The alarm band is exclusive at its
    * limits because an alarm FIRES at the limit; a range is inclusive at its
    * limits because the limit is inside it. */
   int i = 0;
   /* CGM ONLY. A fingerstick is a spot check from a different device with its
    * own calibration, and people test precisely when they suspect a low --
    * letting one end a streak would punish the user for checking, and letting
    * one EXTEND a streak would credit a reading the CGM never made. Skipped
    * entirely, exactly as the statistics skip them. */
   while (i < n && r[i].kind != KIND_CGM)
      i++;
   if (i >= n)
      return 0;
   /* NO EARLY RETURN for "out of range now". The loop below breaks on its
    * first iteration in that case, leaving oldest == newest, which is already
    * 0 -- measured: a mutant deleting the guard passed every case here. A
    * branch no input can distinguish is one more thing to keep true. */
   long newest = r[i].t;
   long oldest = newest;
   long prev   = newest;
   for (; i < n; i++) {
      if (r[i].kind != KIND_CGM)
         continue;
      if (r[i].glu < lo || r[i].glu > hi)
         break; /* the run ends here */
      /* A HOLE ENDS IT AT THE NEAR SIDE. Readings come every five minutes;
       * a longer gap means the sensor was off, the phone was away or the
       * session ended, and nothing measured what happened inside it.
       * Counting through would claim credit for time nobody observed. */
      if (prev - r[i].t > gap_max)
         break;
      oldest = r[i].t;
      prev   = r[i].t;
   }
   return newest - oldest;
}
