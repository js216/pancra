// SPDX-License-Identifier: GPL-3.0
// alarmlogic.h --- Pure alarm decision logic (host-testable)
// Copyright 2026 Jakob Kastelic

/* The alarm DECISION, split out from the alarm ACTUATION in alarm.c.
 *
 * Why this file exists: `make check` cannot fail on anything in the Android
 * shell -- it is
 * in no test binary -- and an adversarial review proved it by making
 * alarm_zone() return 0 unconditionally, which makes a glucose alarm
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
#include <stdbool.h> /* the yes/no observations are bool, not int */

/* FORWARD-DECLARED, NOT INCLUDED, and the difference is a build failure.
 *
 * store.h includes ingest.h which includes THIS header, so a translation unit
 * that reaches store.h first finds PANCRA_STORE_H already defined by the time
 * the cycle comes back round -- and `struct reading` is then still undefined
 * where the prototype below needs it. Including store.h here made exactly that
 * happen.
 *
 * A pointer to an incomplete type is all a prototype needs. alarmlogic.c
 * includes store.h itself for the definition, and this header keeps the
 * no-dependencies property its own comment claims. */
struct reading;

/* ---- ONE TYPE PER DOMAIN, AND THE CONVERSIONS NAMED ----------
 *
 * AS BARE INTS, the alarm level, the nudge band, the nudge sound mode, the
 * action to take, Java's notification kind and a dozen yes/no observations
 * are one type, so the compiler accepts any of them wherever any other is
 * expected -- and three of the lists overlap numerically. `alarm_want(stale,
 * zone)` compiles; so does passing a Java kind back in as a level (AJ_HIGH ==
 * AL_LOW), or a nudge band as an alarm level (NG_HIGH == AL_HIGH by luck,
 * NG_LOW == AL_LOW by luck, and neither is a promise). The two-numbering
 * incident recorded under alarm_java_kind below -- where LOW and "nothing
 * sounding" were the same value and the low-glucose alarm could not fire -- is
 * the same defect one layer down.
 *
 * So each domain is its own enum, with an explicit member for "unknown"
 * where one exists, and the raw-int boundaries are exactly two: what is
 * PERSISTED (settings.c reads a file) and what crosses JNI (Alarm.java gets
 * a kind). Both convert through a checked function here, and neither casts.
 *
 * The values are unchanged. ND_* is persisted and AJ_* is a wire protocol
 * shared with Java, so renumbering either is a data or a protocol break; the
 * enums state the numbers rather than inventing them. */

/* Internal alarm levels. These deliberately do NOT match Java's `kind` -- see
 * alarm_java_kind below, which is the only correct way to convert. AL_NONE
 * must stay 0 and every real level non-zero, because alarm_apply treats 0 as
 * "nothing should be sounding". */
enum alarm_level { AL_NONE = 0, AL_LOW = 1, AL_HIGH = 2, AL_STALE = 3 };

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

/* IS A STAMP RECENT ENOUGH TO ACT ON -- and did it come from the PAST?
 *
 * WHAT THIS EXISTS TO KILL. A freshness test spelled `now - stamp <= limit`
 * is true for every negative age as well as every small positive one. A
 * negative age is not exotic: the phone corrects its clock BACKWARDS routinely
 * -- an NTP step after a flat battery, a timezone database fix, a user setting
 * the date by hand -- and every reading already in the log then carries a
 * timestamp in the future. The alarm's view of the world after such a
 * correction was:
 *
 *   - the last reading, however old, is FRESH, and stays fresh forever. The
 *     big number stops ageing. A 58 mg/dL from three hours ago reads as the
 *     current value, and the low alarm rings on it, or worse is BELIEVED by
 *     someone deciding whether to eat;
 *   - alarm_zone therefore never decays, and a zone outranks a stale warning,
 *     so the DISCONNECT alarm can never fire. A sensor that has fallen off,
 *     run out, or lost its link is invisible for as long as the skew lasts;
 *   - alarm_stranded returns 0 (it declines while the data is "fresh"), so
 *     the one rule that keeps a ringing hypo alive through a dropout is
 *     switched off at exactly the moment the data cannot be trusted;
 *   - an imminent-hypo prediction is preserved indefinitely, and that alarm
 *     is the one the user cannot silence.
 *
 * So the rule is: an age must be NONNEGATIVE as well as within the limit. A
 * stamp from the future is a stamp whose age we do not know, and "I do not
 * know how old this is" must read as NOT CURRENT everywhere.
 *
 * WHAT THAT COSTS, stated honestly rather than hidden. Right after a backward
 * correction the newest reading is also future-dated, so the app reports no
 * current data until the next sample arrives -- at most one CGM cadence,
 * about five minutes, after which every stamp is on the corrected clock
 * again. Without the sanity test the window is not five minutes but
 * unbounded: it lasts until the wall clock catches back up, and throughout it
 * the app asserts a stale value as live. A bounded gap that says "no data"
 * beats an unbounded one that says the wrong number confidently.
 *
 * `now` and `stamp` must be read from the SAME clock. For a persisted fact --
 * a reading, a dose -- that clock is realtime_s(), because the stamp is the
 * record's identity and is shown to a person. For something that only exists
 * inside this process -- when a prediction arrived, when the process started
 * -- it is mono_s(), which no correction can move; see clock.h. */
static inline bool data_fresh(long now, long stamp, long limit)
{
   long age = now - stamp;
   return (bool)(age >= 0 && age <= limit);
}

/* ---- NEW DATAPOINT alert modes (g_newdata_mode, persisted) ----
 *   ND_OFF    silent
 *   ND_BEEP   one fixed tone per new primary-CGM sample
 *   ND_CHIRP  the same tone, pitch-bent by the change since that sensor's
 *             own previous sample
 * Values are PERSISTED (settings field 7, historically a 0/1 flag), so they
 * must not be renumbered. */
enum nudge_mode { ND_OFF = 0, ND_BEEP = 1, ND_CHIRP = 2 };

/* THE ONE PLACE A STORED NUMBER BECOMES THIS TYPE. settings.c reads field 7
 * out of a text file that a previous version wrote (as a 0/1 flag) and that
 * a person can edit; anything not on the list is ND_OFF, which is the safe
 * reading -- a mode nobody recognises must not silently sound something.
 * Pure, so alarmtest executes every branch of it. */
enum nudge_mode nudge_mode_of(int stored);

/* THE CYCLE THE SETTINGS BUTTON PERFORMS: OFF -> BEEP -> CHIRP -> OFF. Here
 * rather than in settings.c, where it was `(mode + 1) % 3` -- arithmetic on a
 * domain that has none, and a wrong answer the day a fourth mode is added
 * anywhere but the end. */
enum nudge_mode nudge_mode_next(enum nudge_mode m);

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
 * armed that cannot arrive.
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
/* NG_UNKNOWN IS NOT NG_NONE, and the difference is the whole latch. "No
 * current reading" must HOLD the previous band through a dropout; "in range"
 * must clear it. As a bare int the unknown was -1 and every caller had to
 * remember that a negative meant hold -- `nzone <= 0` and `nzone < 0` both
 * appear below, one of them meaning something different from the other. */
enum nudge_band { NG_UNKNOWN = -1, NG_NONE = 0, NG_LOW = 1, NG_HIGH = 2 };

/* Which nudge band the current reading is in: 0 in range, 1 low, 2 high, and
 * -1 for NO CURRENT READING -- distinct from 0, because the caller must HOLD
 * the previous latch through a dropout rather than clear it. Thresholds are
 * inclusive, exactly like alarm_zone. */
enum nudge_band nudge_zone(int glu, long glu_t, long now, int lo, int hi);

/* The latch to commit, given this tick's zone and the previous latch. Split
 * from nudge_fire so that a SUPPRESSED crossing still updates the latch: a
 * plunge straight past both thresholds must not leave the nudge armed to fire
 * on the way back up, when the user is already looking at a ringing alarm. */
enum nudge_band nudge_next(enum nudge_band nzone, enum nudge_band prev);

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
enum nudge_band nudge_fire(enum nudge_band nzone, bool alarming,
                           enum nudge_band prev);

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
enum alarm_level alarm_zone(int glu, long glu_t, long now, int lo, int hi);

/* HOW LONG THE USER HAS BEEN CONTINUOUSLY IN RANGE, in seconds, ending at the
 * newest reading. 0 when the newest reading is itself out of range -- the
 * streak is lost the moment it breaks, which is the whole point of it.
 *
 * `r` is the history NEWEST FIRST, which is the order store.h hands out.
 *
 * THE BAND IS [lo, hi] INCLUSIVE, and the caller passes the CLINICAL
 * time-in-range range (TIR_LOW_MGDL..TIR_HIGH_MGDL, 70-180) rather than the
 * configured alarm band. A streak is a claim about glucose control, so it is
 * measured against the published consensus range that TIR itself uses -- and
 * because stats.c counts 70 and 180 as in range, so does this. Breaking at a
 * reading TIR calls good would be two numbers on one screen disagreeing about
 * one sample.
 *
 * Note this is the OPPOSITE convention from alarm_zone, which is exclusive at
 * its limits: an alarm FIRES at the limit, while a range CONTAINS it.
 *
 * A GAP BREAKS IT. Readings arrive every five minutes; a hole means the
 * sensor was off, the phone was away, or the session ended, and nobody knows
 * what happened in between. Counting straight through would claim credit for
 * time nobody measured, so a gap longer than `gap_max` seconds ends the
 * streak at the near side of the hole rather than spanning it.
 *
 * Pure, like everything else in this header: the caller passes the history
 * and the bounds, and alarmtest pins the behaviour. */
long alarm_streak_s(const struct reading *r, int n, int lo, int hi,
                    long gap_max);
/* Combine two sensors' zone verdicts: LOW (1) anywhere outranks HIGH (2)
 * anywhere outranks in-range (0). Commutative and associative, so a caller
 * can fold it over any number of sensors. */
enum alarm_level alarm_zone_merge(enum alarm_level a, enum alarm_level b);

/* Is the stale-data ("DISCONNECT") alarm justified?
 *
 * disc_s is the configured threshold in seconds; 0 disables it. A freshly
 * launched process gets a grace period equal to the threshold, since data may
 * legitimately be stale until the first sync.
 *
 * TWO CLOCKS, AND THEY ARE NOT INTERCHANGEABLE. The four time arguments come
 * in two pairs and each pair must be read from its own clock:
 *
 *   glu_t / now          REALTIME. glu_t is the reading's own timestamp --
 *                        its identity in the log and the time shown beside it
 *                        -- so it cannot be anything else, and `now` has to
 *                        match it to be subtractable from it.
 *   launch_mono/mono_now MONOTONIC. "How long has this process been up" is an
 *                        interval that exists only in this process, so a wall
 *                        clock correction must not move it. See clock.h, and
 *                        the comment on the grace test in alarmlogic.c for
 *                        what happened when it did.
 *
 * Passing all four rather than a precomputed "still in grace" flag keeps the
 * COMPOSITION here, where a test executes it. The shell has composed this
 * file's pieces by hand before, and the hand-written copy is the one that
 * ran while the tested copy sat unreferenced -- see alarm_actuate_step. */
bool alarm_stale(int glu, long glu_t, long now, long mono_now, long launch_mono,
                 long disc_s);

/* ---- THE IMMINENT-HYPO OVERRIDE'S OWN PREDICATE ----
 *
 * 55 mg/dL is ADA "Level 2" hypoglycaemia -- clinically significant, the
 * threshold at which guidance stops calling it a low and calls it urgent, and
 * the same number Dexcom's own readouts use for their Urgent Low alert. That
 * is why THIS alarm is the one that overrides the user's sound setting and
 * cannot be silenced away: it is the only value in the app chosen because
 * somebody else's evidence says a person may stop being able to act. */
#define PRED_LOW_MGDL 55

/* Does one CGM link's newest PREDICTION justify forcing a LOW right now?
 *
 * `pred_mono` is when that prediction ARRIVED, on the monotonic clock, and
 * `now_mono` is read from the same one. A prediction is not a record: it is
 * never written to a file and never shown, so the wall clock has no claim on
 * it, and stamping it with realtime meant a backward correction preserved a
 * prediction forever -- on the one alarm the user cannot silence.
 *
 * It lives here rather than in alarm.c because it is a pure decision on the
 * app's loudest path, and alarm.c is reachable by no test: the override was
 * once deleted outright (`if (pred_low)` -> `if (pred_low && 0)`) with the
 * whole gate green. The shell folds this over its links; the rule is here. */
bool alarm_pred_low(int pred_mgdl, long pred_mono, long now_mono);

/* ---- ONE PREDICTION, PUBLISHED AS ONE VALUE -----------------------------
 *
 * THE RACE THIS REMOVES. The prediction and the time it arrived were two
 * plain arrays in alarm.c, written as two separate stores by a GATT binder
 * thread (pancra_glucose, with the DRIVER lock held) and read as two separate
 * loads by the alarm evaluation under the ALARM lock. Two problems, and the
 * second is the dangerous one:
 *
 *   1. It is a C data race -- unsynchronised access to a non-atomic object
 *      from two threads -- which is undefined behaviour, not merely a
 *      stale read. ThreadSanitizer reports it as one.
 *   2. A reader landing between the two stores gets a MIXED PAIR: this
 *      sample's predicted value stamped with the PREVIOUS sample's arrival
 *      time, or the previous value with this arrival time. The freshness
 *      rule asks whether an age is sane; this makes sure the value
 *      and the age belong to each other in the first place. Neither is any
 *      use without the other -- a 48 mg/dL prediction carrying a stamp from
 *      five minutes ago is refused as expired when it is current, and a
 *      recovered value carrying a fresh stamp forces the unsilenceable LOW
 *      over a prediction that has nothing to say.
 *
 * WHY NOT SIMPLY TAKE alarm_lk IN THE WRITER. Because the writer holds the
 * DRIVER lock, and alarm_lk is held across blocking MediaPlayer JNI --
 * hundreds of milliseconds. driver -> alarm would put a binder thread's
 * driver lock behind a media call while the MAIN LOOPER spins on the driver
 * lock, which is the freeze this app has already had (see any_pred_low, and
 * app/thread.h rule 6: alarm_lk is taken ALONE). The pair is small enough to
 * publish as ONE machine word, so it needs no lock and adds no order edge.
 *
 * THE ENCODING: glucose in the top 16 bits, arrival seconds in the low 48.
 * 16 bits holds any prediction a CGM can express with three orders of
 * magnitude to spare (the sensor scale stops at 400), and 48 bits of seconds
 * is 8.9 million years of uptime. Both halves are clamped rather than
 * truncated, because a wrapped stamp would read as an arrival time that never
 * happened -- on the one alarm the user cannot silence.
 *
 * These two are PURE, so alarmtest executes them: a round trip that loses a
 * field, or shifts by one bit, is the whole failure and is otherwise
 * invisible in a file no test can reach. */
#define PRED_MGDL_MAX 65535

struct link_pred {
   int mgdl;  /* predicted glucose; 0 means nothing has been recorded */
   long mono; /* when it arrived, MONOTONIC seconds; 0 means never */
};

unsigned long long pred_pack(int mgdl, long mono);
struct link_pred pred_unpack(unsigned long long word);

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
 * Treating that as a stale-data alarm keeps a sound going rather than killing
 * one. It cannot mask anything (it IS the stale alarm), it cannot fire while
 * data is fresh, and it cannot fire when the last known reading was in range,
 * so it adds no new alarms the user has not asked for -- it only refuses to
 * cancel one that is already justified. */
bool alarm_stranded(int glu, long glu_t, long now, int lo, int hi);


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
enum alarm_level alarm_want_sustained(enum alarm_level zone, bool stale,
                                      bool stranded,
                                      enum alarm_level prev_want);

/* Does `want` correspond to something the user will actually perceive?
 *
 * With sound and vibration both off nothing is audible or tactile, and the
 * caller uses this to decide whether a tap should be consumed as a silence
 * gesture. Latching "sounding" when nothing sounds swallows the user's next
 * tap with no on-screen explanation. */
bool alarm_audible(enum alarm_level want, bool sound_on, bool vib_on);

/* Translate an internal AL_* level to the `kind` Alarm.java expects
 * (0 = low, 1 = high, 2 = stale). Returns -1 for AL_NONE, which must never be
 * actuated.
 *
 * THE TWO NUMBERINGS MUST NOT BE THE SAME ONE. Java's kind puts LOW at 0, but
 * the internal level needs 0 to mean "nothing should be sounding" -- and when
 * a single enum served both, a request to sound LOW produced 0 --
 * indistinguishable from silence. alarm_apply's own idempotence check then
 * returned early and the LOW GLUCOSE ALARM COULD NEVER FIRE, while HIGH and
 * STALE worked normally, which is exactly the shape that hides from casual
 * testing. Keeping the two spaces separate and converting explicitly here is
 * what stops that recurring.
 *
 * ---- THE KIND IS A PROTOCOL, SO IT HAS NAMES ON BOTH SIDES -------------
 *
 * These three numbers cross a language boundary: C computes one and
 * hands it to Alarm.trigger, which decided what to SAY with a chain of
 * `kind == 2 ? ... : kind == 1 ? ... : "Glucose LOW"`. Two things were wrong
 * with that beyond its being unreadable. Nothing named the numbers on either
 * side, so the two lists agreed only by inspection and a renumbering here
 * would have relabelled a safety notification silently. And the chain was not
 * exhaustive: ANY unrecognised kind fell through to LOW, so a future fourth
 * alarm -- or a corrupted argument -- announces a hypoglycaemic emergency.
 *
 * The names are AJ_* here and Alarm.KIND_* there, the values are stated once
 * on each side, and `make -f test/Makefile javacheck` compares them literally.
 * An edit to either list that the other does not match fails the build. */
enum java_kind {
   AJ_NONE  = -1, /* not a level Java can sound: alarm_apply stays quiet */
   AJ_LOW   = 0,
   AJ_HIGH  = 1,
   AJ_STALE = 2
};

/* THE OTHER RAW-INT BOUNDARY. The JNI call takes an int, so this is where
 * the type stops; every caller converts here and nothing casts. */
enum java_kind alarm_java_kind(enum alarm_level want);

/* What alarm_apply should DO, given the level it computed and what is already
 * committed. Pure, so the sequences can be tested; main.c holds the state and
 * performs the actuation.
 *
 * `act` is one of: */
enum alarm_act {
   AL_ACT_NONE    = 0, /* nothing changed -- do not re-chime */
   AL_ACT_TRIGGER = 1, /* announce o->want (convert with alarm_java_kind) */
   AL_ACT_SILENCE = 2  /* stop whatever is sounding */
};

/* `sounding` and `acked` are TRISTATE here and nowhere else: AL_KEEP means
 * the caller keeps what it has. They were -1 in an int field, which is the
 * kind of thing a reader has to be told; named, the decision below can still
 * say "unchanged" without a bool having to mean three things. */
#define AL_KEEP (-1)

struct alarm_out {
   enum alarm_act act;
   enum alarm_level want; /* level to commit */
   int sounding;          /* arms the tap-to-silence gesture, or AL_KEEP */
   int acked;             /* acknowledgement state to commit, or AL_KEEP */
};

/* prev_acked records that the user DISMISSED prev_want. It is deliberately not
 * an input to the decision below -- a dismissal suppresses re-ANNOUNCING the
 * same level (see alarm_reactuate_allowed), it does not suppress a genuine
 * change of level, which is a new alarm the user has not seen. */
void alarm_decide(enum alarm_level want, enum alarm_level prev_want,
                  bool sound_on, bool vib_on, struct alarm_out *o);

/* May a re-actuation request (an audible-setting change) re-announce the
 * currently committed level?
 *
 * Only if the user has not already dismissed it. Recorded implicitly as
 * "prev_want still equals this level", acknowledgement is destroyed by a
 * re-actuation by design -- so toggling SOUND or VIBRATION restarts an alarm
 * the user has silenced, from a settings screen reachable only because it had
 * gone quiet. The distinction that matters: a level that was
 * DISMISSED must stay quiet, while one that was never audible at all (both
 * settings off) must still be able to sound once one is enabled. */
bool alarm_reactuate_allowed(bool acked);

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
 * it lives here because the shell it actuates through is reachable by no
 * test. */
/* 40..400 mg/dL is the range a Dexcom sensor itself reports and clamps to, so
 * a calibration outside it describes a reading the sensor could not have
 * produced. THE SAME PAIR bounds what the app will believe off the wire --
 * glucose_plausible in reading.c widens it to 20..600 so a genuine extreme is
 * recorded rather than dropped, and says so by deriving from these rather
 * than by repeating two more literals three thousand lines away. */
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
 * permanently. The shell that actuates it cannot be tested; this can. */
#define AL_MIN  40
#define AL_MAX  400
#define AL_STEP 5
/* Keypad-entry bound for BOTH alarm thresholds, and alarm_load's range check
 * (the two must agree, so it lives here next to AL_*): 0..999 mg/dL. Either
 * end is a deliberate OFF switch -- LOW 0 sits below any possible reading,
 * and a HIGH past the sensor's 400 scale above any -- so both are legitimate
 * user choices, unlike the stepper-era [AL_MIN, AL_MAX] clamp. */
#define AL_ENTRY_MAX 999


/* THE LEVEL TO AIM FOR, INCLUDING THE IMMINENT-HYPO OVERRIDE.
 *
 * alarm_want_sustained answers "what level does the data justify". This adds
 * the one rule that outranks it: a CGM predicting below PRED_LOW_MGDL forces
 * a LOW that sounds even when the user has switched sound off, and that
 * cannot be silenced while the prediction holds.
 *
 * It lives here, and not in the actuator, because it is the only code path in
 * the app that can wake a sleeping person -- and because a review deleted it
 * (`if (pred_low)` -> `if (pred_low && 0)`) with the entire gate green. It is
 * a pure decision; the actuation around it is not.
 *
 * `prev_want` in is the level currently committed. `prev_want` out is what
 * alarm_decide should compare against: normally unchanged, but cleared to
 * AL_NONE when the override holds and nothing is sounding, so the next
 * evaluation sees a fresh NONE->LOW edge and re-triggers. THAT re-trigger is
 * what makes the alarm unsilenceable. */
struct alarm_plan {
   enum alarm_level want;      /* the level to aim for */
   bool sound_on;              /* possibly forced on by the override */
   enum alarm_level prev_want; /* what alarm_decide compares against */
};

void alarm_plan_next(enum alarm_level zone, bool stale, bool stranded,
                     bool acked, bool pred_low, enum alarm_level prev_want,
                     bool sounding, bool sound_on, struct alarm_plan *out);

/* ================= THE ALARM ACTUATION WORKFLOW =====================
 *
 * The three pieces above -- sustain, override, decide -- were composed BY HAND
 * inside alarm.c's actuator, which is how the composition escaped the gate: a
 * pure copy of the override lived here and was tested, while the copy that
 * actually ran was a second one written out inline. Deleting the tested copy
 * would have failed alarmtest and changed nothing on the phone.
 *
 * So the composition is here too, as one transition. The shell holds the
 * state, hands it in with what it observed, and does what the effect says.
 *
 * State, not globals: `want` is the level last committed to Java, `acked`
 * whether the user has dismissed it, `sounding` whether anything is actually
 * audible or tactile (which is what arms the tap-to-silence gesture). */
struct alarm_state {
   enum alarm_level want;
   bool acked;
   bool sounding;
};

/* What the shell is asked to DO. Nothing here touches Java; the shell does. */
struct alarm_effect {
   enum alarm_act act;
   enum java_kind kind; /* AJ_NONE when there is nothing to say */
   bool sound, vib;     /* the outputs that trigger should use */
};

/* What the shell OBSERVED this tick. Every input the decision needs, so the
 * transition can be pure -- no clock, no globals, no locks. */
struct alarm_obs {
   enum alarm_level zone; /* AL_NONE in range, AL_LOW, AL_HIGH */
   bool stale;            /* the current reading has aged out */
   bool stranded;         /* the sensor is gone, not merely quiet */
   bool pred_low;         /* a CGM predicts an imminent hypo */
   bool sound_on, vib_on; /* the user's alarm output settings */
};

/* state + observation -> next state + one effect. Pure.
 *
 * The caller commits `next` ONLY if the effect it performs succeeds; a failed
 * JNI call must leave the state unchanged, so the next tick tries again.
 * That
 * retry-by-level is the whole error-recovery story for the alarm, which is
 * why the transition never assumes its effect happened. */
void alarm_actuate_step(const struct alarm_state *st,
                        const struct alarm_obs *obs, struct alarm_state *next,
                        struct alarm_effect *eff);

#endif
