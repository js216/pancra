// SPDX-License-Identifier: GPL-3.0
// gesturelogic.c --- Which finger an event belongs to (pure, host-testable)
// Copyright 2026 Jakob Kastelic

/* See gesturelogic.h. Pure by design: no globals, no clock, no event, no
 * locks, no JNI. */
#include "gesturelogic.h"
#include "ndk.h"

/* Where the latched pointer sits in THIS event, or -1 if it is not in it.
 *
 * By id, never by slot: the whole point of the file. An event can describe the
 * same finger at index 1 that the previous event described at index 0, and the
 * naive read of index 0 then follows a stranger. */
static int find_index(const struct gesture_in *in)
{
   if (in->latched < 0)
      return -1; /* no gesture: nothing to find */
   for (int i = 0; i < in->n; i++)
      if (in->ids[i] == in->latched)
         return i;
   return -1;
}

struct gesture_out gesture_resolve(const struct gesture_in *in)
{
   /* ABANDON is the DEFAULT, so a case reached by a route nobody thought of
    * fails safe -- it gives up the gesture rather than firing it from
    * whatever pointer happened to be nearest. */
   struct gesture_out out = {GEST_ABANDON, 0, -1};
   unsigned raw           = (unsigned)in->action;
   int masked             = (int)(raw & (unsigned)AMOTION_EVENT_ACTION_MASK);
   /* The index of the pointer that came or went, out of the action word's
    * second byte. Only POINTER_DOWN and POINTER_UP carry it; for every other
    * action those bits are zero. */
   int pidx = (int)((raw & (unsigned)AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                    (unsigned)AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

   /* A NEW GESTURE. ACTION_DOWN is by definition the FIRST finger, so the
    * action word's index bits are zero and slot 0 really is the new pointer --
    * but it is the pointer's ID that gets remembered, because slot 0 stops
    * meaning this finger the moment a second one arrives. */
   if (masked == AMOTION_EVENT_ACTION_DOWN) {
      if (in->n < 1)
         return out; /* an event describing no pointer: nothing to latch */
      out.verdict = GEST_FOLLOW;
      out.index   = 0;
      out.latch   = in->ids[0];
      return out;
   }

   /* A SECOND FINGER ARRIVING ENDS THE GESTURE. Under the one-finger policy
    * this is the whole of the POINTER_DOWN case: whichever finger it is, and
    * wherever it landed, the user is now touching the glass in two places and
    * what they meant by the first touch is no longer knowable. Ignoring it
    * instead -- which is what the code did before, by masking the action and
    * never looking at 5 or 6 -- is what let the second finger inherit the
    * first one's armed control. */
   if (masked == AMOTION_EVENT_ACTION_POINTER_DOWN)
      return out;

   /* A FINGER LIFTING WHILE OTHERS STAY DOWN. Only OUR finger matters:
    *
    *   - if the pointer named by the action word is the latched one, our
    *     finger has left while another remains, which under a one-finger
    *     policy is exactly as ambiguous as a second finger arriving. Abandon;
    *     do not treat it as the release, which would FIRE the armed control
    *     from a hand that still has a finger on the screen;
    *   - otherwise it is somebody else's finger going away, which says nothing
    *     about ours. Swallow the event and leave the gesture alone -- this is
    *     the one multi-touch event that must NOT cancel, or a stray second
    *     touch anywhere on the glass would kill every gesture it overlapped.
    *
    * Under the current policy a POINTER_DOWN would already have abandoned
    * before any POINTER_UP could arrive, so the first rule is a SECOND line of
    * defence. It is still asserted on its own: it is the only defence left the
    * day the policy softens to "ignore extra fingers", and a rule that only
    * holds because an earlier one fires first is a rule nobody can change. */
   if (masked == AMOTION_EVENT_ACTION_POINTER_UP) {
      if (pidx < 0 || pidx >= in->n)
         return out; /* the event does not describe the pointer it names */
      if (in->latched >= 0 && in->ids[pidx] == in->latched)
         return out; /* ours left; another remains */
      out.verdict = GEST_IGNORE;
      out.latch   = in->latched; /* untouched */
      return out;
   }

   /* EVERY OTHER EVENT -- MOVE, UP, CANCEL, and the hover/scroll codes this
    * app acts on nowhere -- is about the gesture already in progress, so it is
    * read at our pointer's CURRENT index, looked up afresh every time. */
   int idx = find_index(in);
   if (idx < 0) {
      /* Our pointer is not in this event at all. Nothing here describes the
       * gesture, and continuing would read another finger's position into the
       * release test. */
      if (masked != AMOTION_EVENT_ACTION_CANCEL)
         return out;
      /* Except for CANCEL, which ends the gesture whatever it carries -- the
       * system has taken the touch away. It is reported as FOLLOW, not
       * ABANDON, so the caller's own CANCEL arm still runs; a CANCEL carries
       * no useful position and every handler's CANCEL branch ignores the
       * coordinates. */
      out.verdict = GEST_FOLLOW;
      out.index   = 0;
      out.latch   = -1;
      return out;
   }
   out.verdict = GEST_FOLLOW;
   out.index   = idx;
   /* The gesture is over on the last finger's UP and on a CANCEL, and only
    * then: a MOVE keeps the same finger. */
   out.latch = (masked == AMOTION_EVENT_ACTION_UP ||
                masked == AMOTION_EVENT_ACTION_CANCEL)
                   ? -1
                   : in->latched;
   return out;
}
