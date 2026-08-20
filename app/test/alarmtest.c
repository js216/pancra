// SPDX-License-Identifier: GPL-3.0
// alarmtest.c --- Host tests for the alarm decision logic
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for the alarm state machine.
 *
 * Until this existed, `make check` could not fail on ANYTHING in main.c: it is
 * in no test binary, so clang-format and clang-tidy were the only gates on the
 * most safety-critical logic in the app. An adversarial review proved the hole
 * by making the glucose zone unconditionally 0 -- which makes a glucose alarm
 * impossible under every input -- with the whole gate still green.
 *
 * It was not hypothetical. The LOW-glucose alarm was in fact dead: the internal
 * "what should be sounding" level and Java's `kind` shared one enum in which
 * LOW == 0, and 0 was also the sentinel for "nothing", so a low reading was
 * indistinguishable from silence and the idempotence check returned early.
 * HIGH and STALE worked, which is exactly why nobody noticed. The first test
 * below is the one that would have caught it.
 *
 * Built and run by `make alarmtest`, which `make check` depends on.
 */
#include "alarmlogic.h"
#include <pthread.h> /* the prediction word is published across threads */
#include <sched.h>   /* sched_yield: let the reader run on a single core */
#include <stdatomic.h>
#include <stdio.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- THE PREDICTION WORD, DRIVEN BY TWO THREADS ---------------------------
 *
 * alarm.c publishes a link's predicted glucose and the monotonic second it
 * arrived as ONE atomic word, because the writer is a GATT binder thread
 * holding the DRIVER lock and the reader is the alarm evaluation holding the
 * ALARM lock -- two threads, two different locks, and no common one that may
 * be taken (alarm_lk is held across blocking MediaPlayer JNI, so taking it
 * under the driver lock is a freeze). alarm.c itself is in no test binary, so
 * what is executed here is the PRIMITIVE it rests on: pack, publish, load,
 * unpack, under real concurrency and under ThreadSanitizer.
 *
 * THE PAIR MUST BE MUTUALLY CHECKABLE, or this proves nothing. A run of
 * plausible-looking values passes against an unsynchronised publication
 * simply because every value is in range -- that is exactly what happened to
 * bondtabletest before it encoded identity into its values (200/200 green
 * over an unsynchronised table). So the writer publishes pairs satisfying a
 * relation the reader can check: mono == PRED_T0 + mgdl. A mixed pair -- one
 * epoch's value with another epoch's stamp -- violates it, and a torn word
 * violates it in both halves at once. */
#define PRED_T0 4000000L
#define PRED_LO 40
#define PRED_HI 900
/* How many publications the writer makes before it is allowed to stop, and how
 * many the reader must have OBSERVED by then. */
#define PRED_ITERS    200000
#define PRED_MIN_SEEN 1000
/* ...and the hard cap that keeps a starved reader a REPORTED failure instead
 * of a hung suite. A gate that can hang is a gate that gets disabled. */
#define PRED_MAX_ITERS 200000000L

static _Atomic unsigned long long g_pred_word;
/* atomic_int, NOT volatile: `volatile` orders nothing between threads and
 * ThreadSanitizer correctly reports a volatile stop flag as a race. */
static atomic_int g_pred_stop;
static atomic_int g_pred_seen;  /* pairs the reader actually observed */
static atomic_int g_pred_mixed; /* ...of which were incoherent */

static void *pred_writer(void *arg)
{
   (void)arg;
   /* THE WRITER WAITS FOR THE READER TO HAVE LOOKED.
    *
    * Without this the case passes on scheduling luck and proves nothing: 200k
    * relaxed stores take about a millisecond, so the writer can finish and
    * raise the stop flag before the reader thread is ever scheduled -- the
    * reader then observes ZERO pairs and "no mixed pair was seen" is true
    * because nothing was seen. Observed happening on the first run of this
    * suite at -O2. The `seen > 0` assertion below is what catches it; this
    * loop is what stops it being the common case. */
   for (long i = 0;; i++) {
      int mgdl = PRED_LO + (int)(i % (PRED_HI - PRED_LO + 1));
      atomic_store_explicit(&g_pred_word, pred_pack(mgdl, PRED_T0 + mgdl),
                            memory_order_relaxed);
      if (i < PRED_ITERS)
         continue;
      if (atomic_load_explicit(&g_pred_seen, memory_order_relaxed) >=
          PRED_MIN_SEEN)
         break;
      if (i >= PRED_MAX_ITERS)
         break;      /* the reader never ran; let the assertion say so */
      sched_yield(); /* one core: give the reader somewhere to run */
   }
   atomic_store_explicit(&g_pred_stop, 1, memory_order_release);
   return 0;
}

static void *pred_reader(void *arg)
{
   (void)arg;
   while (!atomic_load_explicit(&g_pred_stop, memory_order_acquire)) {
      /* ONE load, then decide from the pair it yielded -- the shape
       * any_pred_low uses. Re-reading the word for the second field would put
       * the mixed-pair hole straight back. */
      struct link_pred p =
          pred_unpack(atomic_load_explicit(&g_pred_word, memory_order_relaxed));
      if (!p.mgdl && !p.mono)
         continue; /* nothing published yet */
      if (p.mono != PRED_T0 + p.mgdl)
         atomic_fetch_add_explicit(&g_pred_mixed, 1, memory_order_relaxed);
      /* Incremented LAST and visibly, because the writer waits on it. */
      atomic_fetch_add_explicit(&g_pred_seen, 1, memory_order_relaxed);
   }
   return 0;
}

int main(void)
{
   const long now = 1000000;
   const int lo   = 70;
   const int hi   = 180;

   printf("== every real alarm level is distinguishable from silence ==\n");
   /* THE regression test: if any level collides with AL_NONE, that alarm can
    * never fire, because alarm_apply treats "want == 0" as nothing to sound. */
   ck(alarm_want(1, 0) != AL_NONE, "LOW zone yields a non-silent level");
   ck(alarm_want(2, 0) != AL_NONE, "HIGH zone yields a non-silent level");
   ck(alarm_want(0, 1) != AL_NONE, "stale yields a non-silent level");
   ck(alarm_want(0, 0) == AL_NONE, "in range + fresh yields silence");
   ck(alarm_want(1, 0) != alarm_want(2, 0) &&
          alarm_want(2, 0) != alarm_want(0, 1) &&
          alarm_want(1, 0) != alarm_want(0, 1),
      "the three levels are mutually distinct");

   printf("== levels map to the kind Alarm.java expects ==\n");
   ck(alarm_java_kind(alarm_want(1, 0)) == 0, "LOW  -> java kind 0");
   ck(alarm_java_kind(alarm_want(2, 0)) == 1, "HIGH -> java kind 1");
   ck(alarm_java_kind(alarm_want(0, 1)) == 2, "STALE-> java kind 2");
   ck(alarm_java_kind(AL_NONE) < 0, "silence never actuates a kind");

   printf("== zone: thresholds are inclusive at the boundary ==\n");
   ck(alarm_zone(69, now, now, lo, hi) == 1, "69 with low=70 is LOW");
   ck(alarm_zone(70, now, now, lo, hi) == 1, "70 AT low=70 is already LOW");
   ck(alarm_zone(71, now, now, lo, hi) == 0, "71 with low=70 is in range");
   ck(alarm_zone(179, now, now, lo, hi) == 0, "179 with high=180 is in range");
   ck(alarm_zone(180, now, now, lo, hi) == 2,
      "180 AT high=180 is already HIGH");
   ck(alarm_zone(181, now, now, lo, hi) == 2, "181 with high=180 is HIGH");

   printf("== CHIRP pitch: direction, rate, and a hard cap ==\n");
   /* The audible contract: no change sounds exactly like BEEP, a rise bends
    * up and a fall bends down, and no excursion however violent can bend
    * further than CHIRP_MAX_ST -- the alert has to stay a recognisable tone. */
   ck(chirp_semitone10(0) == 0, "no change is the plain BEEP pitch");
   ck(chirp_semitone10(2) == 10, "+2 mg/dL is one semitone up");
   ck(chirp_semitone10(-2) == -10, "-2 mg/dL is one semitone down");
   ck(chirp_semitone10(1) == 5, "an odd delta keeps its half semitone");
   ck(chirp_semitone10(-1) == -5, "...in both directions");
   ck(chirp_semitone10(10) == CHIRP_MAX_ST * 10, "+10 mg/dL reaches the cap");
   ck(chirp_semitone10(-10) == -CHIRP_MAX_ST * 10, "-10 reaches it too");
   ck(chirp_semitone10(400) == CHIRP_MAX_ST * 10, "a wild rise stays capped");
   ck(chirp_semitone10(-400) == -CHIRP_MAX_ST * 10, "a wild fall likewise");
   ck(chirp_semitone10(11) == chirp_semitone10(10),
      "past the cap the pitch stops moving");
   ck(chirp_semitone10(-3) < 0 && chirp_semitone10(3) > 0,
      "sign follows the direction of travel");

   printf("== zone DECAYS with staleness (it must never latch) ==\n");
   /* A sensor dropping out while low used to leave the zone latched at LOW.
    * Because a glucose zone outranks the stale warning, the DISCONNECT alarm
    * could then never sound -- a lost alarm through a side door. */
   ck(alarm_zone(50, now - AL_FRESH_S, now, lo, hi) == 1,
      "a just-fresh low reading still reads LOW");
   ck(alarm_zone(50, now - AL_FRESH_S - 1, now, lo, hi) == 0,
      "one second past fresh, the zone decays to 0");
   ck(alarm_zone(-1, now, now, lo, hi) == 0, "no reading is not a zone");

   printf("== stale: threshold, launch grace, and disabled ==\n");
   /* `mono` is the monotonic clock, which is a DIFFERENT number space from
    * `now`: seconds since boot, not since 1970. It is deliberately nothing
    * like `now` here, so a call that fed the wall clock into the grace pair
    * (or the other way round) could not accidentally still pass. */
   const long mono   = 5000;
   const long launch = mono - 10000; /* long since out of the launch grace */
   ck(alarm_stale(100, now - 10000, now, mono, launch, 0) == 0,
      "disc disabled never fires");
   ck(alarm_stale(100, now - 100, now, mono, launch, 600) == 0,
      "a recent reading is not stale");
   ck(alarm_stale(100, now - 601, now, mono, launch, 600) == 1,
      "past the threshold is stale");
   ck(alarm_stale(100, now - 601, now, mono, mono - 10, 600) == 0,
      "inside the launch grace, not stale");
   ck(alarm_stale(-1, now, now, mono, launch, 600) == 1,
      "no reading at all is stale");

   printf("== a glucose excursion outranks a stale warning ==\n");
   ck(alarm_want(1, 1) == alarm_want(1, 0), "LOW wins over stale");
   ck(alarm_want(2, 1) == alarm_want(2, 0), "HIGH wins over stale");

   printf("== 'sounding' means actually perceptible ==\n");
   /* Its only other job is to make the next tap silence the alarm and do
    * nothing else; latching it when nothing sounds swallows that tap. */
   ck(alarm_audible(alarm_want(1, 0), 1, 0) == 1, "sound on -> audible");
   ck(alarm_audible(alarm_want(1, 0), 0, 1) == 1, "vibrate on -> audible");
   ck(alarm_audible(alarm_want(1, 0), 0, 0) == 0, "both off -> NOT audible");
   ck(alarm_audible(AL_NONE, 1, 1) == 0, "silence is never audible");

   printf("== a hypo alarm is not SILENCED by the sensor dropping out ==\n");
   {
      /* The regression that mattered: with DISCONNECT off (the default), the
       * zone decayed at AL_FRESH_S and alarm_want returned AL_NONE, so
       * alarm_apply
       * actively called silence() on a ringing hypo. */
      const long gt = now - AL_FRESH_S - 60; /* one minute past fresh */
      int zone      = alarm_zone(45, gt, now, lo, hi);
      int stale_off = alarm_stale(45, gt, now, 5000, -95000, 0); /* disc OFF */
      ck(zone == 0, "the zone still decays (it must not mask a stale alarm)");
      ck(stale_off == 0, "the configured stale alarm is off");
      int stranded = alarm_stranded(45, gt, now, lo, hi);
      ck(stranded == 1, "but a stale LOW is stranded");
      ck(alarm_want(zone, stale_off || stranded) != AL_NONE,
         "so something still sounds instead of being silenced");

      /* It must not invent alarms. */
      ck(alarm_stranded(100, gt, now, lo, hi) == 0,
         "a stale IN-RANGE reading is not stranded");
      ck(alarm_stranded(45, now, now, lo, hi) == 0,
         "a FRESH low is not stranded (the zone rules own it)");
      ck(alarm_stranded(-1, gt, now, lo, hi) == 0,
         "never having had a reading is not stranded");
      ck(alarm_stranded(250, gt, now, lo, hi) == 1,
         "a stale HIGH is stranded too");
   }

   printf("== a stranded alarm SUSTAINS, it never originates or relabels ==\n");
   {
      const long gt = now - AL_FRESH_S - 60; /* one minute past fresh */
      int zone      = alarm_zone(45, gt, now, lo, hi);
      int stranded  = alarm_stranded(45, gt, now, lo, hi);
      ck(zone == 0 && stranded == 1, "a stale low is stranded but out of zone");

      /* ORIGINATION: at process start nothing is sounding. store_load restores
       * the last reading from the log, so without this a cold start fired
       * "Sensor disconnected" about a second in, for a condition the user had
       * switched off. */
      ck(alarm_want_sustained(zone, 0, stranded, AL_NONE) == AL_NONE,
         "nothing sounding => a stranded reading raises NOTHING");

      /* SUSTAIN: a ringing hypo must not be silenced by its own data ageing. */
      ck(alarm_want_sustained(zone, 0, stranded, AL_LOW) == AL_LOW,
         "a sounding LOW is sustained, not silenced");

      /* NO RELABEL: switching LOW->STALE defeats the idempotence check and
       * re-fires an alarm the user already acknowledged, under a misleading
       * name, with nothing new having happened. */
      ck(alarm_want_sustained(zone, 0, stranded, AL_LOW) != AL_STALE,
         "...and is NOT relabelled to STALE");
      ck(alarm_want_sustained(zone, 0, stranded, AL_HIGH) == AL_HIGH,
         "a sounding HIGH is likewise sustained as HIGH");

      /* WITH THE DISCONNECT ALARM ENABLED (stale = 1). Every assertion above
       * passes stale = 0, so the mutant `stranded && !stale && ...` survived
       * the whole suite -- and that mutant IS the shipped bug this rule was
       * written for: a dismissed hypo coming back relabelled "Sensor
       * disconnected" because alarm_want(0,1) is AL_STALE, not AL_NONE. */
      ck(alarm_want_sustained(zone, 1, stranded, AL_LOW) == AL_LOW,
         "with DISCONNECT on, a sustained LOW still wins over STALE");
      ck(alarm_want_sustained(zone, 1, stranded, AL_HIGH) == AL_HIGH,
         "...and a sustained HIGH likewise");
      ck(alarm_want_sustained(zone, 1, 0, AL_NONE) == AL_STALE,
         "...while a genuine stale alarm with nothing sustained is STALE");
      /* Switching DISCONNECT off while a STALE alarm sounds must silence it,
       * not sustain it: only a glucose level is ever sustained. */
      /* A DISMISSED level must stop sustaining, or the user's configured
       * DISCONNECT alarm is silently disabled forever: the sustain outranks
       * `stale`, and `stranded` never clears because the reading cannot age
       * back into freshness. main.c passes `stranded && !acked` for exactly
       * this; here is the rule that makes it correct. */
      ck(alarm_want_sustained(zone, 1, 0, AL_LOW) == AL_STALE,
         "once dismissed (stranded suppressed), STALE can fire again");
      ck(alarm_want_sustained(zone, 0, 0, AL_LOW) == AL_NONE,
         "...and with no stale reason either, it falls silent");

      ck(alarm_want_sustained(0, 0, 1, AL_STALE) == AL_NONE,
         "a sounding STALE is not sustained once the stale reason clears");

      /* It must not override a genuine current zone. */
      int fz = alarm_zone(250, now, now, lo, hi);
      ck(alarm_want_sustained(fz, 0, 0, AL_LOW) == AL_HIGH,
         "a fresh HIGH still wins over a previously sounding LOW");
      /* And a configured stale alarm still works on its own. */
      ck(alarm_want_sustained(0, 1, 0, AL_NONE) == AL_STALE,
         "the user's own DISCONNECT alarm still originates normally");
      /* In-range fresh data clears everything. */
      ck(alarm_want_sustained(0, 0, 0, AL_LOW) == AL_NONE,
         "returning to range silences a sustained alarm");
   }

   /* ---- A CLOCK ROLLBACK ----
    *
    * The phone corrects its wall clock BACKWARDS as a matter of routine: an
    * NTP step after the battery went flat, a timezone-database fix, a date
    * typed in by hand. Every reading already in the log is then stamped in
    * the FUTURE, and every freshness test in this file used to be
    * `now - stamp <= limit`, which a negative age satisfies. So after a
    * correction the app believed a three-hour-old reading was the current
    * one, indefinitely -- and "fresh" is what decides whether a low is
    * believed and sounded.
    *
    * Every assertion below is written against the ROLLED-BACK clock: `rb` is
    * `now` after the correction, and `gt` is a reading taken before it, which
    * is therefore in `rb`'s future.
    */
   printf("== a clock ROLLBACK must not make stale data read as fresh ==\n");
   {
      const long gt = now;        /* stamped before the correction */
      const long rb = now - 3600; /* ...which then moved an hour back */

      /* THE BOUNDARY, both sides of it. Age 0 is fresh (a reading that has
       * just landed) and age -1 is not. These two are what pin `age >= 0`:
       * an assertion at -3600 alone is satisfied by a mutant that merely
       * shifts the window, and one at 0 alone by deleting the test outright.
       */
      ck(alarm_zone(45, now, now, lo, hi) == 1, "a zero-age reading is fresh");
      ck(alarm_zone(45, now, now - 1, lo, hi) == 0,
         "ONE SECOND into the future is already not fresh");
      ck(alarm_zone(45, gt, rb, lo, hi) == 0,
         "...and an hour into the future is not a live LOW zone either");

      /* The nudge's own freshness, which has the same shape and the same
       * -1 = "no idea" answer a dropout gets. */
      ck(nudge_zone(120, now, now, 100, 200) == NG_NONE,
         "a zero-age reading is a known nudge zone");
      ck(nudge_zone(120, now, now - 1, 100, 200) == -1,
         "a future-stamped reading is UNKNOWN, not in-range");

      /* STRANDED MUST STILL FIRE. This is the rule that keeps a ringing hypo
       * alive when the sensor drops out; it declines while the data is
       * "fresh", so the old predicate switched it off at exactly the moment
       * the data stopped being ageable -- and alarm_want then returned
       * AL_NONE, which actively SILENCES a hypo alarm. */
      ck(alarm_stranded(45, gt, rb, lo, hi) == 1,
         "a future-stamped LOW is stranded, so the alarm is not silenced");
      ck(alarm_stranded(45, now, now - 1, lo, hi) == 1,
         "...one second is enough: an unageable stamp is not fresh data");
      ck(alarm_stranded(45, now, now, lo, hi) == 0,
         "...while a genuinely current LOW is still the zone's business");
      ck(alarm_stranded(100, gt, rb, lo, hi) == 0,
         "and a future-stamped IN-RANGE reading strands nothing");

      /* THE STALE TRANSITION MUST STILL FIRE. `now - glu_t > disc_s` was
       * false for every negative age, so the user's DISCONNECT alarm could
       * never announce a sensor that had stopped reporting. */
      ck(alarm_stale(100, gt, rb, mono, launch, 600) == 1,
         "a future-stamped reading is STALE: its age is unknowable");
      ck(alarm_stale(100, now, now - 1, mono, launch, 600) == 1,
         "...one second into the future is already unknowable");
      ck(alarm_stale(100, now, now, mono, launch, 600) == 0,
         "...while a zero-age reading is not stale");

      /* THE WHOLE COMPOSITION, which is what the user actually hears. */
      int zone     = alarm_zone(45, gt, rb, lo, hi);
      int stale    = alarm_stale(45, gt, rb, mono, launch, 600);
      int stranded = alarm_stranded(45, gt, rb, lo, hi);
      ck(alarm_want_sustained(zone, stale, stranded, AL_LOW) == AL_LOW,
         "a hypo already sounding survives the correction, unrelabelled");
      ck(alarm_want_sustained(zone, stale, stranded, AL_NONE) == AL_STALE,
         "with nothing sounding, the correction announces STALE, not silence");
   }

   /* ---- THE LAUNCH GRACE IS MONOTONIC ----
    *
    * The grace exists because data may legitimately be stale until the first
    * sync, and while it holds alarm_stale returns 0 UNCONDITIONALLY -- which
    * is to say the grace SUPPRESSES the user's DISCONNECT alarm. Measured on
    * the wall clock, a phone that finds a network shortly after boot (which
    * is when this app starts) could step its clock and disable that alarm for
    * the length of the skew, or end the grace early and announce a
    * disconnected sensor over data that was merely waiting.
    *
    * The pairs below differ ONLY in the realtime `now`. The monotonic pair is
    * identical across each pair, so the answer must be too.
    */
   printf("== the launch grace is not moved by a wall-clock jump ==\n");
   {
      const long m0 = 900000;    /* seconds since boot; nothing like `now` */
      const long gt = now - 601; /* data older than the 600 s threshold */
      const long jump_back = -86400;
      const long jump_fwd  = 86400;

      /* INSIDE the grace: 10 monotonic seconds up, threshold 600. */
      ck(alarm_stale(100, gt, now, m0 + 10, m0, 600) == 0,
         "10 s after launch the grace holds");
      ck(alarm_stale(100, gt, now + jump_back, m0 + 10, m0, 600) == 0,
         "...and a wall clock jumping a day BACK does not end it");
      ck(alarm_stale(100, gt, now + jump_fwd, m0 + 10, m0, 600) == 0,
         "...nor does one jumping a day FORWARD");

      /* PAST the grace, at the exact boundary: 600 monotonic seconds up is
       * no longer `< 600`, so the alarm becomes reachable. Pinning the
       * boundary is what stops a mutant that merely widens or narrows it. */
      ck(alarm_stale(100, gt, now, m0 + 599, m0, 600) == 0,
         "one second short of the threshold, the grace still holds");
      ck(alarm_stale(100, gt, now, m0 + 600, m0, 600) == 1,
         "AT the threshold the grace is over and the alarm can fire");
      ck(alarm_stale(100, gt, now + jump_back, m0 + 600, m0, 600) == 1,
         "...and a wall-clock rollback cannot put us back inside it");
      ck(alarm_stale(100, gt, now + jump_fwd, m0 + 600, m0, 600) == 1,
         "...nor can a jump forward, which never had a grace to give");
   }

   /* ---- THE IMMINENT-HYPO PREDICTION'S FRESHNESS ----
    *
    * This is the alarm that sounds through a muted phone and re-triggers
    * itself after every silence, so a prediction preserved past its useful
    * life is an alarm that cannot be switched off and is no longer about
    * anything. Both stamps are MONOTONIC -- the arrival time of an
    * in-process fact that is never written down and never displayed.
    */
   printf("== a hypo prediction expires, and cannot be preserved ==\n");
   {
      const long m = 900000; /* monotonic: seconds since boot */
      ck(alarm_pred_low(50, m, m) == 1,
         "a fresh prediction below 55 forces LOW");
      ck(alarm_pred_low(54, m, m) == 1, "one below the threshold counts");
      ck(alarm_pred_low(PRED_LOW_MGDL, m, m) == 0,
         "AT 55 it does not: the override is for BELOW Level 2");
      ck(alarm_pred_low(80, m, m) == 0, "a comfortable prediction is silent");
      ck(alarm_pred_low(50, m, m + AL_FRESH_S) == 1,
         "at the freshness edge it still holds");
      ck(alarm_pred_low(50, m, m + AL_FRESH_S + 1) == 0,
         "one second past it, the prediction has expired");
      ck(alarm_pred_low(50, m, m - 1) == 0,
         "a stamp from the future is refused, exactly like a reading's");
      ck(alarm_pred_low(0, m, m) == 0,
         "no prediction recorded is not an alarm");
      ck(alarm_pred_low(50, 0, m) == 0,
         "a link that has never reported one is not an alarm either");
   }

   printf("== the announce/dismiss sequences, end to end ==\n");
   {
      /* These eight steps were verified only by hand-tracing main.c last time,
       * which is exactly the weak evidence this file exists to replace. Each
       * step feeds the previous step's committed state back in, so the
       * sequence is what is being asserted, not eight isolated calls. */
      struct alarm_out o;
      int want  = AL_NONE;
      int acked = 0;

      /* 1. a low arrives, sound on -> announce it */
      alarm_decide(AL_LOW, want, 1, 0, &o);
      ck(o.act == AL_ACT_TRIGGER && o.want == AL_LOW && o.sounding == 1,
         "1. a new LOW is announced and arms the silence gesture");
      want  = o.want;
      acked = o.acked;
      ck(acked == 0, "   ...and starts un-acknowledged");

      /* 2. the user silences it: main.c sets acked, level unchanged */
      acked = 1;

      /* 3. the next evaluation must NOT re-chime */
      alarm_decide(AL_LOW, want, 1, 0, &o);
      ck(o.act == AL_ACT_NONE, "3. re-asserting the same level does nothing");

      /* 4. toggling an audible setting must NOT resurrect a dismissed alarm */
      ck(alarm_reactuate_allowed(acked) == 0,
         "4. a DISMISSED level is not re-announced by a settings change");

      /* 5. a genuine change of level is a new alarm the user has not seen */
      alarm_decide(AL_HIGH, want, 1, 0, &o);
      ck(o.act == AL_ACT_TRIGGER && o.acked == 0,
         "5. a CHANGED level announces and clears the acknowledgement");
      want = o.want;

      /* 6. returning to range silences */
      alarm_decide(AL_NONE, want, 1, 0, &o);
      ck(o.act == AL_ACT_SILENCE && o.want == AL_NONE,
         "6. returning to range silences");
      want = o.want;

      /* 7. and a later low can sound again */
      alarm_decide(AL_LOW, want, 1, 0, &o);
      ck(o.act == AL_ACT_TRIGGER, "7. a later LOW sounds again after recovery");

      /* 8. THE case that must not be confused with 4: an alarm that was never
       * audible at all (both settings off) has NOT been dismissed, so enabling
       * sound must still be able to announce it. */
      alarm_decide(AL_LOW, AL_NONE, 0, 0, &o);
      ck(o.act == AL_ACT_TRIGGER && o.sounding == 0,
         "8. with sound+vibrate off the level commits but nothing sounds");
      ck(o.acked == 0, "   ...and it is NOT treated as acknowledged");
      ck(alarm_reactuate_allowed(o.acked) == 1,
         "   ...so enabling sound may still announce it");
   }

   printf("== threshold stepping clamps and never crosses ==\n");
   {
      int a = 100;
      int b = 200;
      alarm_step(0, &a, &b);
      ck(a == 95 && b == 200, "low minus steps low only");
      alarm_step(1, &a, &b);
      ck(a == 100 && b == 200, "low plus steps low only");
      alarm_step(3, &a, &b);
      ck(a == 100 && b == 205, "high plus steps high only");
      alarm_step(2, &a, &b);
      ck(a == 100 && b == 200, "high minus steps high only");

      a = AL_MIN;
      b = 200;
      alarm_step(0, &a, &b);
      ck(a == AL_MIN, "low cannot go below AL_MIN");
      a = 100;
      b = AL_MAX;
      alarm_step(3, &a, &b);
      ck(b == AL_MAX, "high cannot go above AL_MAX");

      /* A low pushed up past high must not produce low > high: that state
       * latches BOTH alarms permanently (everything is simultaneously below
       * low and above high). */
      /* Pin WHICH threshold survives, not merely that the pair is ordered.
       * Both the correct rule and its mirror keep low <= high, so an assertion
       * on ordering alone is satisfied by either -- verified by mutation: the
       * swapped version passed until these two lines existed. The rule is that
       * the threshold the user did NOT touch is preserved, and their own push
       * is what gets capped. The mirror would silently drag the high alarm up
       * every time the user nudged the low one. */
      a = 200;
      b = 200;
      alarm_step(1, &a, &b); /* low-plus into high */
      ck(b == 200, "pushing low into high leaves HIGH untouched");
      ck(a == 200, "...and caps the low at it");
      a = 200;
      b = 200;
      alarm_step(2, &a, &b); /* high-minus into low */
      ck(a == 200, "pulling high into low leaves LOW untouched");
      ck(b == 200, "...and caps the high at it");

      /* Sweep every reachable state: no sequence of taps may invert the pair
       * or escape the range. */
      a = 100;
      b = 200;
      for (int n = 0; n < 400; n++) {
         alarm_step(n % 4, &a, &b);
         if (a > b || a < AL_MIN || a > AL_MAX || b < AL_MIN || b > AL_MAX) {
            ck(0, "invariant broken during tap sweep");
            break;
         }
      }
      ck(a <= b && a >= AL_MIN && b <= AL_MAX, "400 taps preserve invariants");
   }

   printf("== calibration entry: refuse, never clamp ==\n");
   {
      /* The most consequential write in the app. Clamping instead of refusing
       * would correct the sensor toward a number the user never chose, and in
       * mmol/L the entry is TENTHS so a scaling error is silent and large. */
      ck(cal_entry_mgdl("120", 3, 0) == 120, "mg/dL passes through");
      ck(cal_entry_mgdl("78", 2, 1) == 140, "mmol tenths: 7.8 -> 140 mg/dL");
      ck(cal_entry_mgdl("55", 2, 1) == 99, "mmol tenths: 5.5 -> 99 mg/dL");
      /* Boundaries, exactly. */
      ck(cal_entry_mgdl("40", 2, 0) == 40, "the low bound is accepted");
      ck(cal_entry_mgdl("39", 2, 0) == -1, "one below it is REFUSED");
      ck(cal_entry_mgdl("400", 3, 0) == 400, "the high bound is accepted");
      ck(cal_entry_mgdl("401", 3, 0) == -1, "one above it is REFUSED");
      /* Refusal must not be a clamp: an out-of-range entry yields no value. */
      ck(cal_entry_mgdl("999", 3, 0) < 0, "far out of range is refused");
      ck(cal_entry_mgdl("1", 1, 0) < 0, "far below is refused");
      /* mmol/L makes 2.2 land at 39 mg/dL -- just under the floor, and easy to
       * type by accident. It must refuse rather than submit 40. */
      ck(cal_entry_mgdl("22", 2, 1) == -1, "2.2 mmol/L (39 mg/dL) is refused");
      ck(cal_entry_mgdl("", 0, 0) == -1, "an empty entry is refused");
      ck(cal_entry_mgdl("9999999999", 10, 0) == -1,
         "an absurd entry is refused without overflowing");
   }

   printf("== zone merge: the alarm watches every sensor, worst wins ==\n");
   {
      /* The shell folds this over every CGM's own verdict; the rule that a
       * LOW anywhere outranks a HIGH anywhere is what makes two sensors
       * disagreeing in opposite directions ring the one that kills. */
      ck(alarm_zone_merge(0, 0) == 0, "in-range everywhere stays quiet");
      ck(alarm_zone_merge(0, 1) == 1, "a LOW on either sensor rings LOW");
      ck(alarm_zone_merge(1, 0) == 1, "...in either order");
      ck(alarm_zone_merge(0, 2) == 2, "a HIGH on either sensor rings HIGH");
      ck(alarm_zone_merge(2, 0) == 2, "...in either order");
      ck(alarm_zone_merge(1, 2) == 1, "LOW outranks HIGH");
      ck(alarm_zone_merge(2, 1) == 1, "...in either order");
   }

   printf("== nudge: the one-time heads-up on the wider band ==\n");
   {
      const long t = 1000;
      /* Thresholds chosen the way the feature is meant to be used: the nudge
       * OUTSIDE the alarm. Alarm 70/300, nudge 100/200. */
      ck(nudge_zone(120, t, t, 100, 200) == NG_NONE, "in the middle: nothing");
      ck(nudge_zone(100, t, t, 100, 200) == NG_LOW,
         "AT the low threshold counts, exactly as the alarm does");
      ck(nudge_zone(200, t, t, 100, 200) == NG_HIGH, "...and at the high one");
      ck(nudge_zone(99, t, t, 100, 200) == NG_LOW, "below is low");
      ck(nudge_zone(201, t, t, 100, 200) == NG_HIGH, "above is high");
      ck(nudge_zone(-1, t, t, 100, 200) == -1, "no reading is UNKNOWN, not 0");
      ck(nudge_zone(120, t, t + AL_FRESH_S, 100, 200) == NG_NONE,
         "a reading right at the freshness edge still counts");
      ck(nudge_zone(120, t, t + AL_FRESH_S + 1, 100, 200) == -1,
         "one second past it is UNKNOWN, not in-range");
      /* THE DROPOUT RULE, and why it is -1 rather than 0. If staleness read as
       * "in range" the latch would clear, and the next reading back would
       * re-announce a crossing the user already heard -- once per dropout, all
       * day, on a flaky link. */
      ck(nudge_next(-1, NG_LOW) == NG_LOW, "a dropout HOLDS the latch");
      ck(nudge_next(NG_NONE, NG_LOW) == NG_NONE,
         "a genuine return to range clears it");
      ck(nudge_next(NG_HIGH, NG_LOW) == NG_HIGH, "a new band replaces it");

      /* Edge-triggered: the crossing sounds, the state does not. */
      ck(nudge_fire(NG_LOW, 0, NG_NONE) == NG_LOW, "crossing in fires");
      ck(nudge_fire(NG_LOW, 0, NG_LOW) == NG_NONE,
         "staying below does NOT fire again -- the whole point of a nudge is "
         "that it can be ignored, and an ignorable sound that repeats is worse "
         "than none");
      ck(nudge_fire(NG_NONE, 0, NG_LOW) == NG_NONE,
         "returning to range is silent");
      ck(nudge_fire(-1, 0, NG_NONE) == NG_NONE,
         "an unknown reading never fires");
      ck(nudge_fire(NG_HIGH, 0, NG_LOW) == NG_HIGH,
         "crossing straight from low to high fires the new direction");

      /* Suppression under the alarm. The second argument is "the alarm is
       * announcing SOMETHING", not this tick's zone -- see nudge_fire. The
       * caller folds in the imminent-hypo override and the committed level,
       * which is how a nudge is kept off the back of an unsilenceable
       * predicted hypo, a DISCONNECT alarm, or a stranded sustain, none of
       * which appear in the excursion zone at all. */
      ck(nudge_fire(NG_LOW, 1, NG_NONE) == NG_NONE,
         "a ringing LOW alarm suppresses the nudge");
      ck(nudge_fire(NG_HIGH, 2, NG_NONE) == NG_NONE, "...and a HIGH one");
      ck(nudge_fire(NG_LOW, 2, NG_NONE) == NG_NONE,
         "ANY alarm suppresses it, not just the matching direction");
      ck(nudge_fire(NG_LOW, AL_STALE, NG_NONE) == NG_NONE,
         "a non-zone alarm (stale / forced hypo) suppresses it too");
      ck(nudge_fire(NG_LOW, 0, NG_NONE) == NG_LOW,
         "and with nothing sounding it still fires");

      /* The sequence that made nudge_next a separate function: a plunge past
       * BOTH thresholds between two samples. The nudge is suppressed, but the
       * latch must still commit -- otherwise, when glucose climbs back out of
       * the alarm band and is merely nudge-low, prev is still NONE and the
       * nudge fires at the user who is already staring at the alarm. */
      int prev  = NG_NONE;
      int azone = 0;
      int nz    = nudge_zone(120, t, t, 100, 200); /* fine */
      ck(nudge_fire(nz, azone, prev) == NG_NONE, "in range: silent");
      prev = nudge_next(nz, prev);

      nz    = nudge_zone(65, t, t, 100, 200); /* plunged past both */
      azone = alarm_zone(65, t, t, 70, 300);
      ck(azone == 1 && nz == NG_LOW, "one sample, both bands crossed");
      ck(nudge_fire(nz, azone, prev) == NG_NONE,
         "the alarm has it; stay quiet");
      prev = nudge_next(nz, prev);
      ck(prev == NG_LOW, "but the latch MUST record the crossing");

      nz    = nudge_zone(85, t, t, 100, 200); /* recovering: out of the alarm */
      azone = alarm_zone(85, t, t, 70, 300);
      ck(azone == 0 && nz == NG_LOW, "back above the alarm, still nudge-low");
      ck(nudge_fire(nz, azone, prev) == NG_NONE,
         "and it does NOT fire on the way back up");
      prev = nudge_next(nz, prev);

      nz   = nudge_zone(140, t, t, 100, 200); /* fully recovered */
      prev = nudge_next(nz, prev);
      ck(prev == NG_NONE, "a real recovery re-arms it");
      nz = nudge_zone(95, t, t, 100, 200);
      ck(nudge_fire(nz, 0, prev) == NG_LOW,
         "so the NEXT crossing sounds again");
   }

   /* ---- THE IMMINENT-HYPO OVERRIDE ----
    *
    * The only code path in this app that can wake a sleeping person, and it
    * was unreachable by any test while it sat inside main.c: a review deleted
    * it (`if (pred_low)` -> `if (pred_low && 0)`) and the entire gate stayed
    * green. Three things have to hold, and each is a separate way to lose it.
    */
   printf("== the imminent-hypo override ==\n");
   {
      struct alarm_plan pl;

      /* 1. It outranks whatever the data alone justifies -- including a
       *    perfectly in-range reading, which is the point: the PREDICTION is
       *    what is urgent. */
      alarm_plan_next(0 /* zone: in range */, 0, 0, 0, 1, AL_NONE, 0, 1, &pl);
      ck(pl.want == AL_LOW,
         "a predicted hypo forces LOW from an in-range zone");

      /* 2. It sounds even when the user has switched the alarm sound OFF.
       *    Every other alert in the app respects that setting. */
      alarm_plan_next(0 /* zone: in range */, 0, 0, 0, 1, AL_NONE, 0, 0, &pl);
      ck(pl.sound_on == 1, "...and sounds with SOUND switched off");

      /* 3. It cannot be silenced away. Silencing clears `sounding`; the plan
       *    must then clear the committed level so the next evaluation sees a
       *    fresh NONE->LOW edge and triggers again. Without this the alarm is
       *    dismissed permanently while the prediction still holds. */
      alarm_plan_next(0 /* zone: in range */, 0, 0, 0, 1, AL_LOW, 0, 1, &pl);
      ck(pl.prev_want == AL_NONE,
         "...and re-arms after a silence, so it cannot be dismissed");
      /* While it IS sounding, the level stays committed -- re-triggering a
       * playing alarm every second would restart the tone forever. */
      alarm_plan_next(0 /* zone: in range */, 0, 0, 0, 1, AL_LOW, 1, 1, &pl);
      ck(pl.prev_want == AL_LOW, "...but does not re-trigger while sounding");

      /* Without a prediction, none of this applies: the plan is exactly what
       * the sustained level says, and the user's sound setting is honoured. */
      alarm_plan_next(0 /* zone: in range */, 0, 0, 0, 0, AL_NONE, 0, 0, &pl);
      ck(pl.want == AL_NONE && pl.sound_on == 0 && pl.prev_want == AL_NONE,
         "with no prediction the override changes nothing");

      /* A DISMISSED stranded level must stop sustaining -- the dismissal bug
       * that silently disabled the DISCONNECT alarm forever. */
      alarm_plan_next(0 /* zone: in range */, 0, 1, 1, 0, AL_LOW, 0, 1, &pl);
      ck(pl.want != AL_LOW,
         "an acked stranded level stops sustaining the old want");
   }

   /* ---- THE WHOLE ACTUATION TRANSITION ----
    *
    * Every piece below was already tested on its own. The COMPOSITION was
    * not, and that is where the gap was: main.c wrote out its own version of
    * plan_next + decide by hand, so the tested pieces could have been deleted
    * without failing anything the phone runs. alarm_actuate_step is that
    * composition, and this is the test that makes deleting it fail here. */
   printf("== the alarm actuation transition ==\n");
   {
      struct alarm_state st = {AL_NONE, 0, 0};
      struct alarm_obs obs  = {0};
      struct alarm_state nx;
      struct alarm_effect eff;

      /* Nothing wrong, nothing to say. */
      obs.sound_on = 1;
      obs.vib_on   = 1;
      alarm_actuate_step(&st, &obs, &nx, &eff);
      ck(eff.act == AL_ACT_NONE, "in range and silent: no actuation");

      /* A low is announced, with the user's outputs. */
      obs.zone = AL_LOW;
      alarm_actuate_step(&st, &obs, &nx, &eff);
      ck(eff.act == AL_ACT_TRIGGER, "a low triggers");
      ck(eff.kind == alarm_java_kind(AL_LOW), "...as Java's LOW kind");
      ck(nx.want == AL_LOW, "...and commits the level");
      ck(eff.sound == 1 && eff.vib == 1, "...with the configured outputs");

      /* Same level again is not re-chimed. */
      st = nx;
      alarm_actuate_step(&st, &obs, &nx, &eff);
      ck(eff.act == AL_ACT_NONE, "the same level again does not re-chime");

      /* THE IMMINENT-HYPO OVERRIDE, end to end through the transition: it
       * sounds even with the alarm muted, and it re-triggers after a silence
       * so it cannot be dismissed while the prediction holds. */
      struct alarm_state q = {AL_NONE, 0, 0};
      struct alarm_obs po  = {0};
      po.pred_low          = 1;
      po.sound_on          = 0; /* the user has muted the alarm */
      po.vib_on            = 0;
      alarm_actuate_step(&q, &po, &nx, &eff);
      ck(eff.act == AL_ACT_TRIGGER && eff.kind == alarm_java_kind(AL_LOW),
         "a predicted hypo triggers a LOW");
      ck(eff.sound == 1, "...audibly, even with the alarm sound switched off");

      /* Silenced by the user (sounding 0, want still LOW) -- and the very next
       * evaluation must announce it again. */
      struct alarm_state silenced = {AL_LOW, 1, 0};
      alarm_actuate_step(&silenced, &po, &nx, &eff);
      ck(eff.act == AL_ACT_TRIGGER,
         "...and re-triggers after a silence: it cannot be dismissed");

      /* A dismissed STRANDED level stops sustaining, so a later disconnect
       * alarm is reachable again. */
      struct alarm_state acked = {AL_LOW, 1, 0};
      struct alarm_obs so      = {0};
      so.stranded              = 1;
      so.sound_on              = 1;
      alarm_actuate_step(&acked, &so, &nx, &eff);
      ck(nx.want != AL_LOW || eff.act != AL_ACT_NONE,
         "an acked stranded level does not pin the old want forever");
   }

   /* ---- A PREDICTION AND ITS ARRIVAL TIME ARE ONE FACT ----
    *
    * They were two plain arrays in alarm.c, stored separately by a binder
    * thread and loaded separately under the alarm lock: a C data race, and a
    * reader landing between the two stores got this sample's value carrying
    * the previous sample's stamp. Item 70 asks whether an age is sane; this
    * asks whether the value and the age belong to each other at all, and
    * neither question is any use without the other.
    */
   printf("== a prediction and its arrival time survive a round trip ==\n");
   {
      /* Exact values, so a shift off by one bit or a dropped field cannot
       * pass. The two fields are asserted SEPARATELY: a single combined
       * assertion is satisfied by a mutant that swaps them. */
      struct link_pred p = pred_unpack(pred_pack(48, 1234567));
      ck(p.mgdl == 48, "the predicted value comes back exactly");
      ck(p.mono == 1234567, "...and so does the second it arrived");

      /* The zero pair is "nothing recorded", and must round-trip as that --
       * alarm_pred_low refuses it, so a rebuilt nonzero would mint an alarm
       * for a link that has never reported. */
      struct link_pred z = pred_unpack(pred_pack(0, 0));
      ck(z.mgdl == 0 && z.mono == 0, "the empty pair stays empty");

      /* NEITHER FIELD MAY BLEED INTO THE OTHER. Each of these puts a maximal
       * value in one half and a distinctive one in the other; if the widths
       * or the shift are wrong, the small field is what changes. */
      struct link_pred a = pred_unpack(pred_pack(PRED_MGDL_MAX, 7));
      ck(a.mgdl == PRED_MGDL_MAX && a.mono == 7,
         "a maximal glucose does not disturb the stamp");
      struct link_pred b = pred_unpack(pred_pack(7, 0xffffffffffffL));
      ck(b.mgdl == 7 && b.mono == 0xffffffffffffL,
         "...and a maximal stamp does not disturb the glucose");
      struct link_pred c = pred_unpack(pred_pack(55, 0x1ffffffffL));
      ck(c.mgdl == 55 && c.mono == 0x1ffffffffL,
         "a stamp past 32 bits keeps every bit it had");

      /* CLAMPED, NOT TRUNCATED. A masked-off high bit does not give a
       * slightly wrong number, it gives a plausible one -- a stamp that
       * wraps reads as an arrival that never happened, on the alarm the user
       * cannot silence. */
      struct link_pred big = pred_unpack(pred_pack(PRED_MGDL_MAX + 1, 5));
      ck(big.mgdl == PRED_MGDL_MAX, "an over-large glucose clamps");
      ck(big.mono == 5, "...without corrupting the stamp beside it");
      /* THE STAMP'S OWN CEILING. A stamp past 48 bits masked rather than
       * clamped comes back as a SMALL number -- one ring of the counter
       * earlier -- which reads as an arrival that never happened and, worse,
       * as an ancient one: the prediction would silently expire. Clamping
       * yields the largest representable second instead, which ages
       * correctly. Verified by mutation: dropping this clamp survived the
       * whole suite until this case existed. */
      struct link_pred hi = pred_unpack(pred_pack(50, 0x1000000000000L));
      ck(hi.mono == 0xffffffffffffL, "an over-large stamp clamps, not wraps");
      ck(hi.mgdl == 50, "...without corrupting the glucose beside it");

      struct link_pred neg = pred_unpack(pred_pack(-1, -1));
      ck(neg.mgdl == 0 && neg.mono == 0,
         "negatives become 'nothing recorded', which is refused outright");
      ck(alarm_pred_low(neg.mgdl, neg.mono, 900000) == 0,
         "...and refused it is: a clamped pair cannot mint an alarm");
   }

   printf("== ...and are published across threads as ONE value ==\n");
   {
      pthread_t w;
      pthread_t r;
      /* The reader first, so it is spinning before anything is published. */
      if (pthread_create(&r, 0, pred_reader, 0) != 0 ||
          pthread_create(&w, 0, pred_writer, 0) != 0) {
         ck(0, "the concurrency case could not start its threads");
      } else {
         pthread_join(w, 0);
         pthread_join(r, 0);
         int seen  = atomic_load(&g_pred_seen);
         int mixed = atomic_load(&g_pred_mixed);
         /* The reader must actually have LOOKED. Without this the case
          * passes on zero observations, which is the "5abc is rejected"
          * shape: an assertion satisfied because the input never reached
          * the rule. */
         ck(seen > 0, "the reader observed published pairs at all");
         ck(mixed == 0,
            "no reader ever saw a value from one epoch with a stamp from "
            "another");
      }
   }

   printf("\n%s\n", all ? "ALL ALARM TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
