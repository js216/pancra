// SPDX-License-Identifier: GPL-3.0
// gesturetest.c --- Host tests for "one gesture, one finger"
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for gesture_resolve: which finger an Android motion event
 * belongs to.
 *
 * WHY THIS BINARY EXISTS AT ALL. The bug it pins was live: input.c masked the
 * action word, dropped the pointer index, ignored POINTER_DOWN and POINTER_UP
 * outright, and read AMotionEvent_getX(ev, 0) for every event -- so a second
 * finger arriving, or the first one lifting while a second stayed down, moved a
 * different pointer into slot 0 and the gesture silently continued under it.
 * Since a press only ARMS a control and the action fires on the RELEASE, the
 * release was then compared against the armed box using SOMEBODY ELSE'S
 * coordinates. The armed controls delete a logged dose and forget a sensor.
 *
 * The host stubs for libandroid report exactly one finger, with id 0 (see
 * app/stub_android.c), so no test that drives on_input can express a second
 * pointer -- which is the only case that matters. That is why the decision
 * lives in gesturelogic.c and why this test talks to it directly: here a case
 * can hand it any number of fingers, in any slot order, with any action word.
 *
 * Two kinds of assertion below:
 *
 *   - the VERDICT cases, which pin one rule of gesture_resolve at a time;
 *   - the SCENARIO cases, which run a sequence of events through a miniature
 *     of input.c's caller (struct fake) that holds the latch and mirrors the
 *     act-on-release rule. Those are what let a case assert the thing that
 *     actually matters -- "the wrong control did not fire" -- rather than the
 *     verdict of one event in isolation.
 *
 * Built and run by `make gesturetest`.
 */
#include "gesturelogic.h"
#include <stdio.h>

/* ---- THE CONSTANTS, AGAINST THE REAL NDK ------------------------------
 *
 * ndk.h mirrors Android's <android/input.h> by hand, because the app is built
 * without the NDK headers. A wrong value here does not fail to compile and
 * does not warn: it silently disables the whole fix. If ACTION_MASK were too
 * wide, a POINTER_DOWN carrying a non-zero index would not compare equal to
 * POINTER_DOWN and would fall through to the mid-gesture path; if the SHIFT
 * were wrong, POINTER_UP would name the wrong finger and cancel the gesture at
 * the wrong times (or never).
 *
 * These are the values in AOSP's android/input.h -- the AMOTION_EVENT_ACTION_*
 * enum -- restated where a change to ndk.h has to walk past them:
 *
 *   DOWN 0, UP 1, MOVE 2, CANCEL 3, OUTSIDE 4, POINTER_DOWN 5, POINTER_UP 6,
 *   ACTION_MASK 0xff, ACTION_POINTER_INDEX_MASK 0xff00, ..._SHIFT 8
 *
 * THESE ARE THE ONLY THING PINNING THE ABSOLUTE VALUES, and that is not a
 * failure of the cases below -- it is unavoidable. A host test builds its
 * action words out of the SAME header the code decodes them with, so a value
 * that is internally consistent and wrong is invisible to every behavioural
 * assertion here: change POINTER_DOWN to 7 in ndk.h and the test still
 * constructs 7, still recognises 7, and still passes, while the phone sends 5
 * and the whole fix is off. That was measured, not assumed (a mutation run over
 * every constant: POINTER_DOWN, POINTER_UP and the index MASK all survived the
 * cases and are killed only here).
 *
 * What the cases below DO catch is every wrong RELATION between the values --
 * a mask that swallows the index bits, a shift that reads them from the wrong
 * place, an action code that collides with another -- because those break the
 * decoding no matter which header it came from. Both halves are needed; neither
 * covers the other. */
_Static_assert(AMOTION_EVENT_ACTION_DOWN == 0, "NDK: ACTION_DOWN is 0");
_Static_assert(AMOTION_EVENT_ACTION_UP == 1, "NDK: ACTION_UP is 1");
_Static_assert(AMOTION_EVENT_ACTION_MOVE == 2, "NDK: ACTION_MOVE is 2");
_Static_assert(AMOTION_EVENT_ACTION_CANCEL == 3, "NDK: ACTION_CANCEL is 3");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_DOWN == 5,
               "NDK: ACTION_POINTER_DOWN is 5");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_UP == 6,
               "NDK: ACTION_POINTER_UP is 6");
_Static_assert(AMOTION_EVENT_ACTION_MASK == 0xff, "NDK: ACTION_MASK is 0xff");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_INDEX_MASK == 0xff00,
               "NDK: POINTER_INDEX_MASK is 0xff00");
_Static_assert(AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT == 8,
               "NDK: POINTER_INDEX_SHIFT is 8");
/* The index field must sit ENTIRELY above the action field, or the two decode
 * into each other -- the relation, not just the two numbers. */
_Static_assert((AMOTION_EVENT_ACTION_POINTER_INDEX_MASK >>
                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT) > 0 &&
                   ((unsigned)AMOTION_EVENT_ACTION_MASK &
                    (unsigned)AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) == 0,
               "NDK: the pointer index and the action code must not overlap");
/* And the six codes this file switches on must be SIX DIFFERENT numbers. A
 * typo'd copy of the list can leave every individual value looking plausible
 * while two of them collide, and a collision does not read as a wrong
 * constant -- it reads as a whole action being handled by the wrong arm. */
_Static_assert(
    AMOTION_EVENT_ACTION_DOWN != AMOTION_EVENT_ACTION_UP &&
        AMOTION_EVENT_ACTION_DOWN != AMOTION_EVENT_ACTION_MOVE &&
        AMOTION_EVENT_ACTION_DOWN != AMOTION_EVENT_ACTION_CANCEL &&
        AMOTION_EVENT_ACTION_DOWN != AMOTION_EVENT_ACTION_POINTER_DOWN &&
        AMOTION_EVENT_ACTION_DOWN != AMOTION_EVENT_ACTION_POINTER_UP &&
        AMOTION_EVENT_ACTION_UP != AMOTION_EVENT_ACTION_MOVE &&
        AMOTION_EVENT_ACTION_UP != AMOTION_EVENT_ACTION_CANCEL &&
        AMOTION_EVENT_ACTION_UP != AMOTION_EVENT_ACTION_POINTER_DOWN &&
        AMOTION_EVENT_ACTION_UP != AMOTION_EVENT_ACTION_POINTER_UP &&
        AMOTION_EVENT_ACTION_MOVE != AMOTION_EVENT_ACTION_CANCEL &&
        AMOTION_EVENT_ACTION_MOVE != AMOTION_EVENT_ACTION_POINTER_DOWN &&
        AMOTION_EVENT_ACTION_MOVE != AMOTION_EVENT_ACTION_POINTER_UP &&
        AMOTION_EVENT_ACTION_CANCEL != AMOTION_EVENT_ACTION_POINTER_DOWN &&
        AMOTION_EVENT_ACTION_CANCEL != AMOTION_EVENT_ACTION_POINTER_UP &&
        AMOTION_EVENT_ACTION_POINTER_DOWN != AMOTION_EVENT_ACTION_POINTER_UP,
    "NDK: the six action codes must all differ");

/* The action word as the OS builds it: the code, plus the index of the pointer
 * that came or went. Spelled out here so a case reads like the event it
 * stands for. */
#define PTR_DOWN(i)                                                            \
   (AMOTION_EVENT_ACTION_POINTER_DOWN |                                        \
    ((i) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT))
#define PTR_UP(i)                                                              \
   (AMOTION_EVENT_ACTION_POINTER_UP |                                          \
    ((i) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT))

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- the miniature caller -------------------------------------------- */

/* The controls a finger can be over. CTL_NONE is 0 so a partly-initialised
 * event describes "over nothing" rather than over the first real control. */
enum ctl {
   CTL_NONE = 0,
   CTL_DELETE, /* the one whose mis-fire deletes a logged dose */
   CTL_LOG,
   CTL_PLOT, /* pressing this one begins a scrub, not an arm */
};

/* One event: the action word, the pointer ids IN INDEX ORDER, and what each of
 * those pointers is sitting on. Per-index controls are the point: the reported
 * bug is precisely that the UP was read at the WRONG index, so a case has to
 * be able to put a different control under each finger. */
struct evt {
   int32_t action;
   int n;
   int32_t ids[GEST_MAX_PTRS];
   int ctl[GEST_MAX_PTRS];
};

/* input.c's caller, reduced to the part that can fire something: it holds the
 * latch across events and mirrors act-on-release -- a press arms the control
 * under the finger, a move off it is a free cancel, the release fires only if
 * it lands back on the armed control, and an abandonment disarms without
 * firing. */
struct fake {
   int latch;
   int armed;
   int scrubbing;
   int fired;  /* the control that last fired */
   int nfired; /* how many times anything fired AT ALL */
   int nabandon;
   int nignore;
   int at; /* the control under our finger at the last FOLLOW */
   struct gesture_out last;
};

static struct fake fake_new(void)
{
   struct fake f = {.latch = -1, .armed = CTL_NONE, .fired = CTL_NONE};
   return f;
}

static void step(struct fake *f, const struct evt *e)
{
   struct gesture_in in = {e->action, f->latch, e->ids, e->n};
   struct gesture_out o = gesture_resolve(&in);
   f->last              = o;
   f->latch             = o.latch;
   if (o.verdict == GEST_ABANDON) {
      f->nabandon++;
      f->armed     = CTL_NONE;
      f->scrubbing = 0;
      return;
   }
   if (o.verdict == GEST_IGNORE) {
      f->nignore++;
      return; /* the gesture is untouched, latch included */
   }
   int c   = (o.index >= 0 && o.index < e->n) ? e->ctl[o.index] : CTL_NONE;
   f->at   = c;
   int msk = (int)((unsigned)e->action & (unsigned)AMOTION_EVENT_ACTION_MASK);
   if (msk == AMOTION_EVENT_ACTION_DOWN) {
      f->armed     = c;
      f->scrubbing = (c == CTL_PLOT);
   } else if (msk == AMOTION_EVENT_ACTION_MOVE) {
      if (f->armed != CTL_NONE && f->armed != c)
         f->armed = CTL_NONE; /* slid off the armed control: a free cancel */
   } else if (msk == AMOTION_EVENT_ACTION_UP) {
      if (f->armed != CTL_NONE && f->armed == c) {
         f->fired = c;
         f->nfired++;
      }
      f->armed     = CTL_NONE;
      f->scrubbing = 0;
   } else if (msk == AMOTION_EVENT_ACTION_CANCEL) {
      f->armed     = CTL_NONE;
      f->scrubbing = 0;
   }
}

int main(void)
{
   printf("gesturetest: one gesture, one finger\n");

   printf("\n== 1. one finger, down / move / up ==\n");
   {
      struct fake f   = fake_new();
      struct evt down = {.action = AMOTION_EVENT_ACTION_DOWN,
                         .n      = 1,
                         .ids    = {4},
                         .ctl    = {CTL_DELETE}};
      step(&f, &down);
      ck(f.last.verdict == GEST_FOLLOW && f.last.index == 0,
         "the DOWN is followed at index 0");
      ck(f.latch == 4, "...and its pointer id is latched, not its slot");
      ck(f.armed == CTL_DELETE, "...arming the control under it");
      struct evt move = {.action = AMOTION_EVENT_ACTION_MOVE,
                         .n      = 1,
                         .ids    = {4},
                         .ctl    = {CTL_DELETE}};
      step(&f, &move);
      ck(f.last.verdict == GEST_FOLLOW && f.latch == 4,
         "a MOVE by the same finger keeps the gesture and the latch");
      ck(f.armed == CTL_DELETE,
         "...and the arming, since it has not moved off");
      struct evt up = {.action = AMOTION_EVENT_ACTION_UP,
                       .n      = 1,
                       .ids    = {4},
                       .ctl    = {CTL_DELETE}};
      step(&f, &up);
      ck(f.nfired == 1 && f.fired == CTL_DELETE,
         "the UP back on the armed control FIRES it -- the ordinary tap still "
         "works");
      ck(f.latch == -1, "...and the latch is released for the next gesture");
      ck(f.nabandon == 0, "nothing about a plain one-finger tap is abandoned");
   }

   printf("\n== 2. a second finger down mid-gesture ==\n");
   {
      struct fake f   = fake_new();
      struct evt down = {.action = AMOTION_EVENT_ACTION_DOWN,
                         .n      = 1,
                         .ids    = {4},
                         .ctl    = {CTL_DELETE}};
      step(&f, &down);
      /* the second finger lands at index 1, on a different control */
      struct evt second = {
          .action = PTR_DOWN(1),
          .n      = 2,
          .ids    = {4,          7      },
          .ctl    = {CTL_DELETE, CTL_LOG}
      };
      step(&f, &second);
      ck(f.last.verdict == GEST_ABANDON,
         "a POINTER_DOWN abandons: two fingers, no knowable intent");
      ck(f.armed == CTL_NONE && f.latch == -1,
         "...disarming, and dropping the latch");
      /* Now the FIRST finger lifts and the SECOND one -- which is sitting on
       * DELETE -- remains. This is the reported bug in full: the naive code
       * read the release at slot 0 and fired whatever was under it. */
      struct evt firstup = {
          .action = PTR_UP(0),
          .n      = 2,
          .ids    = {4,          7         },
          .ctl    = {CTL_DELETE, CTL_DELETE}
      };
      step(&f, &firstup);
      struct evt lastup = {.action = AMOTION_EVENT_ACTION_UP,
                           .n      = 1,
                           .ids    = {7},
                           .ctl    = {CTL_DELETE}};
      step(&f, &lastup);
      ck(f.nfired == 0,
         "and NOTHING fires afterwards, even with a finger resting on the "
         "armed control");
   }

   printf("\n== 3. our finger lifts while another stays down ==\n");
   {
      /* Reached DIRECTLY, not through a POINTER_DOWN: under the current
       * one-finger policy the POINTER_DOWN would already have abandoned, so
       * this rule is the SECOND line of defence -- and the only one left the
       * day the policy softens to "ignore extra fingers". A rule that holds
       * merely because an earlier one fires first is a rule nobody can
       * change. */
      struct fake f    = fake_new();
      f.latch          = 4;
      f.armed          = CTL_DELETE;
      struct evt ourup = {
          .action = PTR_UP(0),
          .n      = 2,
          .ids    = {4,          7      },
          .ctl    = {CTL_DELETE, CTL_LOG}
      };
      step(&f, &ourup);
      ck(f.last.verdict == GEST_ABANDON,
         "the POINTER_UP of the LATCHED finger abandons, it does not release");
      ck(f.nfired == 0 && f.armed == CTL_NONE,
         "...so the armed control does not fire from a hand still on the "
         "glass");
      ck(f.latch == -1, "...and the gesture is over");
   }

   printf("\n== 4. another finger lifts while ours stays down ==\n");
   {
      struct fake f = fake_new();
      f.latch       = 4;
      f.armed       = CTL_DELETE;
      /* id 7, at index 1, goes away; ours (id 4) is still down */
      struct evt theirup = {
          .action = PTR_UP(1),
          .n      = 2,
          .ids    = {4,          7      },
          .ctl    = {CTL_DELETE, CTL_LOG}
      };
      step(&f, &theirup);
      ck(f.last.verdict == GEST_IGNORE,
         "somebody else's finger lifting is IGNORED, not fatal");
      ck(f.latch == 4 && f.armed == CTL_DELETE,
         "...the latch and the arming both survive it");
      ck(f.nabandon == 0, "...nothing was abandoned");
      /* and the gesture can still complete normally */
      struct evt up = {.action = AMOTION_EVENT_ACTION_UP,
                       .n      = 1,
                       .ids    = {4},
                       .ctl    = {CTL_DELETE}};
      step(&f, &up);
      ck(f.nfired == 1 && f.fired == CTL_DELETE,
         "...so our own release still fires: a stray touch must not kill every "
         "gesture it overlaps");
   }

   printf("\n== 5. the slots are repacked under us ==\n");
   {
      struct fake f = fake_new();
      f.latch       = 4;
      f.armed       = CTL_DELETE;
      /* Our finger is now at index 1 and a stranger sits at index 0, over a
       * DIFFERENT control. Reading slot 0 here reports the finger as having
       * slid off DELETE onto LOG. */
      struct evt move = {
          .action = AMOTION_EVENT_ACTION_MOVE,
          .n      = 2,
          .ids    = {9,       4         },
          .ctl    = {CTL_LOG, CTL_DELETE}
      };
      step(&f, &move);
      ck(f.last.verdict == GEST_FOLLOW && f.last.index == 1,
         "the MOVE follows our pointer to index 1");
      ck(f.at == CTL_DELETE, "...so it is read over OUR control, not slot 0's");
      ck(f.armed == CTL_DELETE, "...and the arming survives the repack");
      /* the stranger leaves, packing us back down to index 0 */
      struct evt theirup = {
          .action = PTR_UP(0),
          .n      = 2,
          .ids    = {9,       4         },
          .ctl    = {CTL_LOG, CTL_DELETE}
      };
      step(&f, &theirup);
      ck(f.last.verdict == GEST_IGNORE && f.latch == 4,
         "...the stranger's departure is ignored");
      struct evt up = {.action = AMOTION_EVENT_ACTION_UP,
                       .n      = 1,
                       .ids    = {4},
                       .ctl    = {CTL_DELETE}};
      step(&f, &up);
      ck(f.last.index == 0 && f.nfired == 1 && f.fired == CTL_DELETE,
         "...and the release, back at index 0, fires the right control");
   }

   printf("\n== 6. our finger is not in the event at all ==\n");
   {
      struct fake f   = fake_new();
      f.latch         = 4;
      f.armed         = CTL_DELETE;
      struct evt move = {
          .action = AMOTION_EVENT_ACTION_MOVE,
          .n      = 2,
          .ids    = {7,          9      },
          .ctl    = {CTL_DELETE, CTL_LOG}
      };
      step(&f, &move);
      ck(f.last.verdict == GEST_ABANDON,
         "a MOVE that does not describe our pointer abandons");
      ck(f.armed == CTL_NONE && f.latch == -1, "...disarming and unlatching");
      struct fake g = fake_new();
      g.latch       = 4;
      g.armed       = CTL_DELETE;
      struct evt up = {.action = AMOTION_EVENT_ACTION_UP,
                       .n      = 1,
                       .ids    = {7},
                       .ctl    = {CTL_DELETE}};
      step(&g, &up);
      ck(g.last.verdict == GEST_ABANDON && g.nfired == 0,
         "an UP from a pointer that is not ours fires nothing");
      /* An empty pointer list is the degenerate form of the same thing, and
       * the one that used to read AMotionEvent_getX(ev, 0) off the end. */
      struct fake h    = fake_new();
      struct evt empty = {.action = AMOTION_EVENT_ACTION_DOWN, .n = 0};
      step(&h, &empty);
      ck(h.last.verdict == GEST_ABANDON && h.latch == -1,
         "a DOWN describing no pointer latches nothing");
   }

   printf("\n== 7. CANCEL ==\n");
   {
      struct fake f     = fake_new();
      f.latch           = 4;
      f.armed           = CTL_DELETE;
      f.scrubbing       = 1;
      struct evt cancel = {.action = AMOTION_EVENT_ACTION_CANCEL,
                           .n      = 1,
                           .ids    = {4},
                           .ctl    = {CTL_DELETE}};
      step(&f, &cancel);
      ck(f.last.verdict == GEST_FOLLOW,
         "a CANCEL is FOLLOWED, so the caller's own CANCEL arm still runs");
      ck(f.latch == -1 && f.armed == CTL_NONE && f.scrubbing == 0,
         "...and it ends the gesture: no latch, no arming, no scrub");
      ck(f.nfired == 0, "...without firing -- a CANCEL is not a release");
      /* The system can take the touch away and hand back an event that no
       * longer lists our pointer. That must still END the gesture rather than
       * be abandoned by the absent-pointer rule above, because the caller's
       * CANCEL arm is what clears the scrub. */
      struct fake g   = fake_new();
      g.latch         = 4;
      g.armed         = CTL_DELETE;
      struct evt gone = {.action = AMOTION_EVENT_ACTION_CANCEL,
                         .n      = 1,
                         .ids    = {7},
                         .ctl    = {CTL_LOG}};
      step(&g, &gone);
      ck(g.last.verdict == GEST_FOLLOW && g.latch == -1 && g.nfired == 0,
         "a CANCEL whose pointer list has lost our finger still ends the "
         "gesture");
      ck(g.last.index == 0,
         "...at a placeholder index the caller must bounds-check");
   }

   printf("\n== the action word is decoded, not guessed ==\n");
   {
      /* The index bits must not disturb the action code. If ACTION_MASK were
       * too wide, this POINTER_DOWN would not be recognised as one and would
       * fall through to the mid-gesture path -- the exact shape of the
       * original bug. */
      struct fake f     = fake_new();
      f.latch           = 4;
      f.armed           = CTL_DELETE;
      struct evt second = {
          .action = PTR_DOWN(1),
          .n      = 2,
          .ids    = {4,          7      },
          .ctl    = {CTL_DELETE, CTL_LOG}
      };
      step(&f, &second);
      ck(f.last.verdict == GEST_ABANDON,
         "a POINTER_DOWN carrying index 1 is still a POINTER_DOWN");
      /* And the index bits must be read from the right place: here the
       * LIFTING pointer is at index 2 of three, and it is ours. Decoding the
       * index wrongly points at somebody else's finger and the gesture
       * survives a lift it should not have. */
      struct fake g    = fake_new();
      g.latch          = 11;
      g.armed          = CTL_DELETE;
      struct evt ourup = {
          .action = PTR_UP(2),
          .n      = 3,
          .ids    = {5,       7,       11        },
          .ctl    = {CTL_LOG, CTL_LOG, CTL_DELETE}
      };
      step(&g, &ourup);
      ck(g.last.verdict == GEST_ABANDON,
         "the lifting pointer is found at index 2, so ours is seen to leave");
      struct fake h      = fake_new();
      h.latch            = 5;
      h.armed            = CTL_DELETE;
      struct evt notours = {
          .action = PTR_UP(2),
          .n      = 3,
          .ids    = {5,          7,       11     },
          .ctl    = {CTL_DELETE, CTL_LOG, CTL_LOG}
      };
      step(&h, &notours);
      ck(h.last.verdict == GEST_IGNORE && h.latch == 5,
         "...and with a different finger lifting from index 2, ours stays");
      /* A POINTER_UP naming an index the event does not describe is not
       * something the OS sends; it must not index off the end either. */
      struct fake k    = fake_new();
      k.latch          = 4;
      struct evt bogus = {
          .action = PTR_UP(3), .n = 1, .ids = {4}, .ctl = {CTL_DELETE}};
      step(&k, &bogus);
      ck(k.last.verdict == GEST_ABANDON,
         "a POINTER_UP naming an index outside the event abandons");
   }

   printf("\n== nothing fires without a DOWN ==\n");
   {
      /* Between gestures the latch is -1, and no id equals it: a MOVE or an UP
       * arriving out of nowhere -- the tail of a gesture already abandoned --
       * must not resolve to a finger. */
      struct fake f   = fake_new();
      struct evt move = {.action = AMOTION_EVENT_ACTION_MOVE,
                         .n      = 1,
                         .ids    = {0},
                         .ctl    = {CTL_DELETE}};
      step(&f, &move);
      ck(f.last.verdict == GEST_ABANDON && f.latch == -1,
         "a MOVE with no gesture latched resolves to nothing");
      struct evt up = {.action = AMOTION_EVENT_ACTION_UP,
                       .n      = 1,
                       .ids    = {0},
                       .ctl    = {CTL_DELETE}};
      step(&f, &up);
      ck(f.last.verdict == GEST_ABANDON && f.nfired == 0,
         "...and neither does the UP that follows it");
      struct evt theirup = {
          .action = PTR_UP(0), .n = 1, .ids = {0}, .ctl = {CTL_DELETE}};
      step(&f, &theirup);
      ck(f.last.verdict == GEST_IGNORE,
         "...a POINTER_UP with no gesture latched is somebody else's finger");
      /* id 0 is a real id, and -1 is the "no gesture" sentinel: the two must
       * never be confused, which is why the latch is compared explicitly. */
      struct fake g    = fake_new();
      struct evt down0 = {.action = AMOTION_EVENT_ACTION_DOWN,
                          .n      = 1,
                          .ids    = {0},
                          .ctl    = {CTL_DELETE}};
      step(&g, &down0);
      ck(g.latch == 0 && g.last.verdict == GEST_FOLLOW,
         "pointer id 0 is a real finger, latched like any other");
   }

   printf("\n== the decision does not touch its input ==\n");
   {
      int32_t ids[2]           = {4, 7};
      struct gesture_in in     = {PTR_UP(1), 4, ids, 2};
      struct gesture_in before = in;
      (void)gesture_resolve(&in);
      ck(in.action == before.action && in.latched == before.latched &&
             in.n == before.n && ids[0] == 4 && ids[1] == 7,
         "the caller's latch and pointer list come back unchanged");
   }

   printf("\n%s\n", all ? "ALL GESTURE TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
