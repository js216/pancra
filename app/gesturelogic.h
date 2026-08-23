// SPDX-License-Identifier: GPL-3.0
// gesturelogic.h --- Which finger an event belongs to (pure, host-testable)
// Copyright 2026 Jakob Kastelic

/* ONE GESTURE, ONE FINGER -- and the decision that enforces it, with no event
 * object anywhere in it.
 *
 * Android reports a touch by SLOT, not by finger. AMotionEvent_getX(ev, 0) is
 * the position of whatever pointer happens to sit at index zero in that
 * event, and the indices are repacked as fingers come and go: a second finger
 * arriving, or the first one lifting while a second stays down, moves a
 * DIFFERENT pointer into slot 0. Read index zero every time and the gesture
 * silently changes fingers halfway through.
 *
 * In this app that is not a cosmetic glitch. A press only ARMS the control
 * under it and the action fires on the RELEASE (see input.h), so a gesture
 * that changes fingers fires the armed control from a touch the user made
 * somewhere else -- and the armed controls delete a logged dose, forget a
 * sensor and write a calibration. "The user did something ambiguous" must
 * never resolve to "fire the armed control".
 *
 * So the pointer ID is latched at the DOWN and every later event is resolved
 * through it, and that resolution is HERE rather than in input.c: it is a
 * decision over four small integers, it is where this class of bug lives, and
 * nothing in the Android shell is reachable by any test. The one thing the
 * host stubs cannot express is a second finger (see stub_android.c), which is
 * precisely the case that matters -- so the decision is lifted out to where a
 * test can hand it as many fingers as it likes, in any slot order. See
 * test/app/gesturetest.c.
 *
 * Pure: no globals, no clock, no event, no JNI. The caller reads the pointer
 * ids out of the event and passes them in; it holds the latch and updates it
 * from the answer.
 *
 * THE POLICY IS STRICTLY ONE FINGER. A second pointer touching down CANCELS
 * the gesture rather than being ignored, because a cancel costs the user one
 * deliberate re-tap and a mis-resolved index costs them a deleted dose. */
#ifndef GESTURELOGIC_H
#define GESTURELOGIC_H

#include "ndk.h" /* the action word decoded here is Android's own */

/* The most pointers one event is described by. Android's input pipeline caps a
 * motion event well below this, and a hand cannot produce more; a pointer past
 * the cap is simply not handed to this decision, which reports the gesture's
 * finger ABSENT and abandons. That is the safe direction: abandoning costs a
 * re-tap, resolving to the wrong index fires the wrong control. */
#define GEST_MAX_PTRS 16

/* What the caller must do with this event. */
enum gesture_verdict {
   /* Our finger is in this event at .index: handle the event, reading its
    * coordinates from THAT index and no other. */
   GEST_FOLLOW,
   /* Give the gesture up without firing anything: either a second finger has
    * made the user's intent ambiguous, or our own finger is no longer
    * described by the event and there is nothing left to read. Deliberately
    * not "release": every path that reaches this has established that it can
    * no longer tell what the user meant. */
   GEST_ABANDON,
   /* Somebody else's finger, in an event that says nothing about ours: swallow
    * it and leave the gesture untouched. */
   GEST_IGNORE,
};

/* The inputs, named so a caller cannot transpose two ints by accident. */
struct gesture_in {
   /* The RAW action word from AMotionEvent_getAction: the masked action code
    * in the low byte AND, for the POINTER_DOWN/POINTER_UP pair, the INDEX of
    * the pointer that came or went in the second byte. Raw, not pre-masked,
    * because that index is half of what this has to decide with -- masking it
    * off and dropping it is what this file exists to prevent. */
   int32_t action;
   /* The pointer id this gesture owns; -1 between gestures. */
   int latched;
   /* The event's pointer ids, in INDEX order: ids[i] is the id of the pointer
    * at index i, which is what makes a repacked slot visible here. */
   const int32_t *ids;
   int n; /* how many of them (0 .. GEST_MAX_PTRS) */
};

struct gesture_out {
   enum gesture_verdict verdict;
   /* Where our finger is in THIS event. Meaningful for GEST_FOLLOW only, and
    * a CANCEL that no longer carries our pointer at all reports 0 as a
    * placeholder -- a CANCEL's coordinates are read by no handler, and the
    * caller must still keep the read in range (see input.c). */
   int index;
   /* The id the caller must hold AFTER this event: unchanged mid-gesture, the
    * newly latched finger on a DOWN, and -1 once the gesture is over by any
    * route -- UP, CANCEL or abandonment. */
   int latch;
};

struct gesture_out gesture_resolve(const struct gesture_in *in);

#endif
