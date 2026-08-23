// SPDX-License-Identifier: GPL-3.0
// alarm.h --- Alarm actuation: the decision reaching the user
// Copyright 2026 Jakob Kastelic
//
/* THE LOUD PART. alarmlogic.c decides WHAT should be sounding from a set of
 * numbers; this file owns the state that decision runs on, the lock that
 * makes raise-and-silence mutually exclusive, and the calls into Java that
 * actually make noise.
 *
 * It is separated from the shell because of who calls it: three kinds of
 * thread, not one. The main looper (the 1 Hz timer, a threshold tap, the
 * tap-to-silence), a GATT binder thread (pancra_alarm_check, after it has
 * released driver_lock), and the service's tick thread. Everything crossing
 * this header is safe from all three; everything behind it takes alarm_lock.
 *
 * Two properties are load-bearing and must survive any change here:
 *
 * 1. LEVEL-based, not edge-triggered. Every entry point recomputes what
 *    should be sounding and reconciles, so a missed transition self-corrects
 *    on the next tick. An edge-triggered version loses a disconnect alarm
 *    permanently when a reading silences it a microsecond after it is raised:
 *    the latch stays at 1, so the !alarmed -> alarmed edge never happens
 *    again.
 * 2. Raise and silence are mutually exclusive END TO END, JNI call included.
 *    Splitting the decision from the call lets two threads decide in one
 *    order and call Java in the other; Alarm.trigger/silence being
 *    `synchronized` orders them against each other but cannot repair a wrong
 *    order. The result was an alarm that looped forever with nothing able to
 *    stop it.
 */
#ifndef ALARM_H
#define ALARM_H

/* Silence a sounding alarm and report whether that CONSUMED the gesture: any
 * press silences, and that press then does nothing else. Returns 0 when
 * nothing was sounding, and also when Java could not be reached -- in which
 * case the tone may still be playing and the next press must try again. */
int alarm_silence_tap(void);

/* The newest PREDICTED value on a CGM link, recorded as each reading is
 * decoded. The imminent-hypo override reads it, gated on freshness -- the
 * prediction must be current, not a value frozen at disconnect. */
void alarm_note_pred(int link, int pred_mgdl, long now);

/* Is the disconnect (stale-data) alarm currently latched? For the screen. */
int alarm_disc_latched(void);

/* Re-evaluate everything and reconcile what is sounding. */
void alarm_reeval(void);

/* Re-apply the CURRENT decision after the sound/vibration settings changed:
 * an alarm can be latched but inaudible, and turning sound back on must make
 * it audible again without waiting for a new reading. */
void alarm_reactuate(void);

/* SET ONE THRESHOLD, WHOLE.
 *
 * The four thresholds are two ordered PAIRS -- a low above its high is not a
 * threshold, it is an alarm that can never stop -- so validating one means
 * reading the other, and the read and the write have to be the same critical
 * section. They were not: the keypad commit took alarm_lock to read the pair,
 * released it, validated, then took it again to write. Between those two the
 * other end could move, and the pair the check approved is not the pair that
 * gets stored.
 *
 * This is also where the range and the ordering rule now live, rather than
 * inside the branch of a dispatcher that happens to be typing digits. The
 * caller gets a reason back and renders it; the rules are not the renderer's.
 */
enum thresh_result {
   TH_OK = 0,
   TH_TOO_BIG,   /* above AL_ENTRY_MAX */
   TH_HIGH_ZERO, /* a HIGH of 0 alarms on every reading, forever */
   TH_ORDER,     /* low > high, which latches both ends */
   /* The value was accepted and stored, and the FILE could not be replaced.
    * Distinct from a refusal: the threshold is live now, and it will be the
    * old one again after the next launch -- which the user has to be told,
    * because these are the two numbers that decide whether a hypo alarm can
    * fire at all. */
   TH_NOT_SAVED
};

/* Move one threshold. Returns TH_OK, or the reason the caller renders. */
int alarm_set_threshold(int isnudge, int islow, int mgdl);

/* The 1 Hz sweep: notice data going stale, and reconcile. */
void alarm_disc_reeval(void);

/* EVALUATE AND ACTUATE THE ALARM. Any thread; must NOT hold driver_lock --
 * this reaches Java's MediaPlayer, which blocks for hundreds of milliseconds,
 * and driver_lock is a spin lock the main looper also takes.
 *
 * Named pancra_* because the BLE transport calls it directly after a
 * notification dispatch (see dexble.c): a reading that crosses a threshold
 * must ring on the thread that decoded it, not on a UI timer that may be
 * gone. Declared HERE, with the alarm, rather than in the transport's port
 * header, because it is the alarm's operation and this is where a reader
 * looks for it. */
void pancra_alarm_check(void);

#endif
