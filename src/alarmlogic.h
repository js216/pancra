// SPDX-License-Identifier: GPL-3.0
// alarmlogic.h --- Pure alarm decision logic (host-testable)
// Copyright 2026 Jakob Kastelic

/* The alarm DECISION, split out from the alarm ACTUATION in main.c.
 *
 * Why this file exists: `make check` cannot fail on anything in main.c -- it is
 * in no test binary -- and an adversarial review proved it by making
 * glucose_zone() return 0 unconditionally, which makes a glucose alarm
 * impossible under every input, with the whole gate still green. Across this
 * codebase's review history the alarm path is where regressions concentrated,
 * precisely because nothing behavioural guarded it.
 *
 * These functions are pure: no globals, no clock, no JNI, no locks. main.c
 * passes the state in and actuates on the result; test/alarmtest.c pins the
 * behaviour. Keeping them pure is the point -- anything that reaches for a
 * global here puts the logic back out of reach of the gate. */
#ifndef ALARMLOGIC_H
#define ALARMLOGIC_H

/* Internal alarm levels. These deliberately do NOT match Java's `kind` -- see
 * alarm_java_kind below, which is the only correct way to convert. AL_NONE
 * must stay 0 and every real level non-zero, because alarm_apply treats 0 as
 * "nothing should be sounding". */
#define AL_NONE  0
#define AL_LOW   1
#define AL_HIGH  2
#define AL_STALE 3

/* How old a reading may be and still count as current, in seconds.
 *
 * A CGM samples every 5 minutes, so 360 s (two cycles minus jitter) blanked
 * the screen after a SINGLE missed sample -- routine on a phone that has
 * wandered out of range for a minute. 660 s tolerates two missed samples and
 * still calls the data stale well before a third would have arrived.
 *
 * THE ONE PLACE THIS LIVES. Every display, notification and connected-dot
 * check reads it from here; when it was open-coded as 360 in five files, a
 * change like this one silently left some of them behind. */
#define AL_FRESH_S 660

/* ---- NEW DATAPOINT alert modes (g_newdata_mode, persisted) ----
 *   ND_OFF    silent
 *   ND_BEEP   one fixed tone per new primary-CGM sample
 *   ND_CHIRP  the same tone, pitch-bent by the change since that sensor's
 *             own previous sample
 * Values are PERSISTED (settings field 7, historically a 0/1 flag), so they
 * must not be renumbered. */
#define ND_OFF   0
#define ND_BEEP  1
#define ND_CHIRP 2

/* CHIRP pitch mapping. The chirp is the beep's duration and starting pitch,
 * then bends by one semitone per CHIRP_MGDL_PER_ST mg/dL of change, up for a
 * rise and down for a fall, clamped at CHIRP_MAX_ST semitones either way --
 * so the ear reads direction and rate without a glance, and a wild jump
 * cannot produce a wild noise. */
#define CHIRP_MGDL_PER_ST 2
#define CHIRP_MAX_ST      5
/* How old the sample being compared against may be. The pitch means "this
 * much change since the LAST reading", which only carries a rate if the last
 * reading is one cadence back. After a dropout -- a shower, the phone in
 * another room -- the first reading back would otherwise chirp the whole
 * accumulated drift at the cap, indistinguishable from a genuine 10 mg/dL
 * -in-5-minutes rocket. Past this the honest sound is the plain BEEP.
 * 450 s is one and a half CGM cycles: tolerant of jitter, short of a gap. */
#define CHIRP_MAX_GAP_S 450

/* ---- NUDGE: a one-time heads-up on a SECOND, wider pair of thresholds ----
 *
 * Why this exists, in the user's own words: a reading of 100 right after lunch
 * is nothing, and a reading of 100 falling fast with no idea it was coming is
 * a genuine problem. The only way to express that with one alarm was to keep
 * MOVING the alarm threshold -- down to 70 after eating, back up when
 * unaware -- and the failure mode of that habit is the dangerous one: the
 * threshold gets left parked low, and the user goes on believing a reminder is
 * armed that can no longer arrive.
 *
 * So the NUDGE is the threshold that gets to be permanent. It sits OUTSIDE the
 * alarm's (a higher low, a lower high), fires ONCE, quietly, and is meant to
 * be ignorable -- the sound says "look at the number", not "act now". The
 * alarm underneath it can then stay at the conservative value it should always
 * have had, and stops being something the user edits day to day.
 *
 * Consequences that follow from that, and are implemented below:
 *   - EDGE-TRIGGERED. It announces the CROSSING, not the state. Re-announcing
 *     every 5 minutes would make it exactly the thing the user must not learn
 *     to ignore, and something ignorable that repeats is worse than nothing.
 *   - SUPPRESSED UNDER THE ALARM. Once the alarm is sounding the nudge has
 *     nothing left to add, and a gentle blip layered on a ringing alarm reads
 *     as a malfunction.
 *   - IT DOES NOT RE-ARM ON A DROPOUT. Staleness must not clear the latch, or
 *     a flaky link turns one crossing into a nudge every few minutes. Only a
 *     genuine return to range re-arms it. */
#define NG_NONE 0
#define NG_LOW  1
#define NG_HIGH 2

/* Which nudge band the current reading is in: 0 in range, 1 low, 2 high, and
 * -1 for NO CURRENT READING -- distinct from 0, because the caller must HOLD
 * the previous latch through a dropout rather than clear it. Thresholds are
 * inclusive, exactly like alarm_zone. */
int nudge_zone(int glu, long glu_t, long now, int lo, int hi);

/* The latch to commit, given this tick's zone and the previous latch. Split
 * from nudge_fire so that a SUPPRESSED crossing still updates the latch: a
 * plunge straight past both thresholds must not leave the nudge armed to fire
 * on the way back up, when the user is already looking at a ringing alarm. */
int nudge_next(int nzone, int prev);

/* NG_NONE, NG_LOW or NG_HIGH: what to sound right now.
 *
 * `nzone` is this tick's nudge zone. `alarming` is non-zero iff the ALARM is
 * announcing anything -- and it must be the BROAD test, not just this tick's
 * alarm_zone. The alarm can be sounding for three reasons the zone does not
 * show: the imminent-hypo override (a prediction below PRED_LOW_MGDL forces
 * AL_LOW at any current reading), the DISCONNECT alarm, and the stranded
 * sustain. Passing the zone alone let a nudge blip through underneath the
 * unsilenceable predicted-hypo alarm -- the loudest possible moment to add a
 * sound that means "no action needed".
 *
 * The two zone-shaped arguments are named apart deliberately: they are the
 * same type and the same shape, and swapping them would silently invert the
 * suppression. */
int nudge_fire(int nzone, int alarming, int prev);

/* Semitones of bend for a delta in mg/dL, in TENTHS of a semitone (the app
 * has no floating point; Java turns tenths into a frequency ratio). A delta
 * of 0 -- or the first sample of a session, which has nothing to compare
 * against -- gives 0, i.e. exactly the BEEP tone. Pure, so it is tested. */
int chirp_semitone10(int delta_mgdl);

/* Glucose zone RIGHT NOW: 0 in range, 1 low, 2 high. Derived, never latched.
 *
 * `glu < 0` means "no reading". Staleness returns 0 rather than the last
 * zone: a latched zone outranks the stale warning in alarm_want(), so a sensor
 * dropping out while low would otherwise mask the DISCONNECT alarm forever. */
int alarm_zone(int glu, long glu_t, long now, int lo, int hi);
/* Combine two sensors' zone verdicts: LOW (1) anywhere outranks HIGH (2)
 * anywhere outranks in-range (0). Commutative and associative, so a caller
 * can fold it over any number of sensors. */
int alarm_zone_merge(int a, int b);

/* Is the stale-data ("DISCONNECT") alarm justified?
 *
 * disc_s is the configured threshold in seconds; 0 disables it. A freshly
 * launched process gets a grace period equal to the threshold, since data may
 * legitimately be stale until the first sync. */
int alarm_stale(int glu, long glu_t, long now, long launch_t, long disc_s);

/* Has the data gone stale while the last reading we DID have was out of range?
 *
 * This exists because the two rules above compose into an actively dangerous
 * result in the default configuration. alarm_zone un-latches at AL_FRESH_S
 * (11 min) so that a stale zone cannot mask the DISCONNECT alarm -- sound
 * reasoning, but it assumes the DISCONNECT alarm is there to take over. It is
 * OFF by default (g_disc == 0 => alarm_stale is unconditionally 0), so with a
 * sensor dropping out on a hypo the zone decayed to nothing, alarm_want
 * returned AL_NONE, and alarm_apply called dexble_alarm_silence() -- ACTIVELY
 * STOPPING a ringing hypo alarm after two missed CGM cycles, while the user
 * was still low and nothing knew otherwise.
 *
 * The hand-off only works while every DISCONNECT threshold is longer than
 * AL_FRESH_S: a zone that is still fresh outranks AL_STALE, so a shorter
 * threshold would fire into a zone that has not decayed yet and the label
 * would lag its own setting. disc_min[] (main.c) starts at 15 min for that
 * reason -- raising AL_FRESH_S means checking that table.
 *
 * Treating that as a stale-data alarm keeps a sound going instead of killing
 * one. It cannot mask anything (it IS the stale alarm), it cannot fire while
 * data is fresh, and it cannot fire when the last known reading was in range,
 * so it adds no new alarms the user has not asked for -- it only refuses to
 * cancel one that is already justified. */
int alarm_stranded(int glu, long glu_t, long now, int lo, int hi);

/* What should be sounding: a glucose excursion outranks a stale-data warning,
 * because it is the more urgent fact and the one the user must act on. */
int alarm_want(int zone, int stale);

/* What should sound, given that an alarm may ALREADY be sounding.
 *
 * alarm_stranded on its own was too strong in two directions, both found by
 * review after it shipped:
 *
 *   - It ORIGINATED alarms. store_load restores g_cur_glu/g_cur_time from the
 *     log before the first tick, so opening the app hours after a low reading
 *     fired "Sensor disconnected" about a second after launch, for a condition
 *     the user had switched off. Requiring prev_want != AL_NONE means it can
 *     only ever SUSTAIN an alarm, never mint one -- which is what its own
 *     header claimed all along.
 *
 *   - It RELABELLED. A hypo the user had already silenced flipped LOW -> STALE
 *     the moment the data aged past AL_FRESH_S, and a changed level defeats
 *     the idempotence check, so the phone started blaring again under a
 *     misleading name with nothing new having happened. Returning prev_want
 *     unchanged keeps the acknowledgement intact.
 *
 * So: sustain exactly what was already sounding, or nothing. */
int alarm_want_sustained(int zone, int stale, int stranded, int prev_want);

/* Does `want` correspond to something the user will actually perceive?
 *
 * With sound and vibration both off nothing is audible or tactile, and the
 * caller uses this to decide whether a tap should be consumed as a silence
 * gesture. Latching "sounding" when nothing sounds swallows the user's next
 * tap with no on-screen explanation. */
int alarm_audible(int want, int sound_on, int vib_on);

/* Translate an internal AL_* level to the `kind` Alarm.java expects
 * (0 = low, 1 = high, 2 = stale). Returns -1 for AL_NONE, which must never be
 * actuated.
 *
 * THE TWO NUMBERINGS MUST NOT BE THE SAME ONE. Java's kind puts LOW at 0, but
 * the internal level needs 0 to mean "nothing should be sounding" -- and when
 * a single enum served both, `want = ALARM_LOW` produced 0, indistinguishable
 * from silence. alarm_apply's own idempotence check then returned early and
 * the LOW GLUCOSE ALARM COULD NEVER FIRE, while HIGH and STALE worked
 * normally, which is exactly the shape that hides from casual testing. Keeping
 * the two spaces separate and converting explicitly here is what stops that
 * recurring. */
int alarm_java_kind(int want);

/* What alarm_apply should DO, given the level it computed and what is already
 * committed. Pure, so the sequences can be tested; main.c holds the state and
 * performs the actuation.
 *
 * `act` is one of: */
#define AL_ACT_NONE    0 /* nothing changed -- do not re-chime */
#define AL_ACT_TRIGGER 1 /* announce o->want (convert with alarm_java_kind) */
#define AL_ACT_SILENCE 2 /* stop whatever is sounding */

struct alarm_out {
   int act;
   int want;     /* level to commit */
   int sounding; /* arms the tap-to-silence gesture; see alarm_audible */
   int acked;    /* acknowledgement state to commit */
};

/* prev_acked records that the user DISMISSED prev_want. It is deliberately not
 * an input to the decision below -- a dismissal suppresses re-ANNOUNCING the
 * same level (see alarm_reactuate_allowed), it does not suppress a genuine
 * change of level, which is a new alarm the user has not seen. */
void alarm_decide(int want, int prev_want, int sound_on, int vib_on,
                  struct alarm_out *o);

/* May a re-actuation request (an audible-setting change) re-announce the
 * currently committed level?
 *
 * Only if the user has not already dismissed it. Acknowledgement used to be
 * recorded implicitly as "prev_want still equals this level", which a
 * re-actuation destroys by design -- so toggling SOUND or VIBRATION restarted
 * an alarm the user had silenced, from a settings screen reachable only
 * because it had gone quiet. The distinction that matters: a level that was
 * DISMISSED must stay quiet, while one that was never audible at all (both
 * settings off) must still be able to sound once one is enabled. */
int alarm_reactuate_allowed(int acked);

/* Convert a keypad entry into mg/dL for a CALIBRATION, or return -1 to refuse.
 *
 * `digits`/`n` are the typed characters; `units` is 0 for mg/dL, 1 for mmol/L,
 * in which case the entry is TENTHS ("78" means 7.8) -- matching how the value
 * is displayed, because a number the user cannot re-type from the screen is a
 * trap.
 *
 * REFUSE, never clamp. Silently altering a calibration the user typed is worse
 * than not accepting it: the sensor would be corrected toward a number nobody
 * chose. main.c calls this the single most consequential write in the app, and
 * it lives here because nothing in main.c is reachable by any test. */
#define CAL_MIN_MGDL 40
#define CAL_MAX_MGDL 400

int cal_entry_mgdl(const char *digits, int n, int units);

/* Apply one +/- step to a threshold pair and re-establish low <= high.
 *
 * `which` is 0 low-minus, 1 low-plus, 2 high-minus, 3 high-plus (the UI's own
 * button order). Both thresholds are clamped to [AL_MIN, AL_MAX], and a
 * crossing is resolved by moving the OTHER threshold to meet the one the user
 * just moved -- so the pair can end up equal, which alarm_load must therefore
 * accept as a legitimate saved state.
 *
 * Extracted for the same reason as the rest of this file: the clamp is a
 * guard-threshold, and getting a comparison one off here silently disables an
 * alarm (a low of 400 means nothing is ever below it) or latches both
 * permanently. Nothing in main.c can be tested; this can. */
#define AL_MIN  40
#define AL_MAX  400
#define AL_STEP 5
/* Keypad-entry bound for BOTH alarm thresholds, and alarm_load's range check
 * (the two must agree, so it lives here next to AL_*): 0..999 mg/dL. Either
 * end is a deliberate OFF switch -- LOW 0 sits below any possible reading,
 * and a HIGH past the sensor's 400 scale above any -- so both are legitimate
 * user choices, unlike the stepper-era [AL_MIN, AL_MAX] clamp. */
#define AL_ENTRY_MAX 999

void alarm_step(int which, int *lo, int *hi);

#endif
