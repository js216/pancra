// SPDX-License-Identifier: GPL-3.0
// input.c --- Touch (see input.h)
// Copyright 2026 Jakob Kastelic

#include "input.h"
#include "alarm.h"
#include "clock.h"
#include "exercise.h" /* the exercise tail the scrub must be able to pick */
#include "food.h"
#include "forms.h"
#include "gesturelogic.h" /* which finger this event belongs to */
#include "insrow.h"       /* INS_*: what a dose row can say */
#include "insulin.h"
#include "jbridge.h"
#include "menu.h" /* menu_action: a press dispatches one */
#include "model.h"
#include "nav.h"
#include "ndk.h"
#include "plot.h"
#include "plotdata.h"
#include "readingrec.h" /* struct reading: one stored sample */
#include "scan.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "store.h"
#include "style.h"
#include "ui.h"
#include "uiact.h"
#include "uimodel.h"
#include "weight.h"
#include <stdint.h>

/* ---- act-on-RELEASE ----
 * A press only ARMS the control under the finger; the action fires on the
 * RELEASE, and only if it lands back on that same control -- so sliding the
 * finger off first is a free cancel, and nothing else can fire until the
 * next press. While armed and on-target, draw_impl lightens the control's
 * whole hit rectangle (ui_press_overlay): one consistent pressed visual for
 * every screen, with no per-renderer work. Exempt BY DESIGN: plot scrubbing
 * (the drag itself is the interaction), the alarm +- steppers (step on
 * press + auto-repeat on hold is their whole point), and silencing a
 * sounding alarm (any press must silence IMMEDIATELY -- see on_input). */

static int g_arm_kind = ACT_NONE; /* armed action; ACT_NONE = none */
static int g_arm_code;            /* its MA_* code (ACT_MENU only) */
static int g_arm_arg;             /* its index / value */
static int g_arm_in;              /* finger currently on the armed control */
static int g_arm_x, g_arm_y;      /* where it last touched it (finds the box) */

/* THE DEVICE UNDER THE FINGER, LATCHED AT TOUCH-DOWN. -1 = this press is not
 * on a device row.
 *
 * A device row is a POSITION in the frame that was on screen when the finger
 * landed, and the action fires on the RELEASE. In between, the 1 Hz timer --
 * or an arriving reading -- redraws, and a mint or a forget on a binder
 * thread renumbers every row after it. The release then lands on the same row
 * NUMBER, which press_release accepts as the same control, and the tap opens,
 * or makes PRIMARY, whichever sensor moved into it. The screen said one
 * device; the app acted on another.
 *
 * So the row becomes a device ONCE, at the instant it was touched, and the
 * release checks the row still holds that device. If it does not, the control
 * that was pressed is gone and the press is dropped -- exactly as if the
 * finger had slid off it, which is what happened, in the only sense that
 * matters. Firing on the latched id instead would act on a device that is no
 * longer where the user is looking; firing on the new one acts on a device
 * they never chose. */
static int g_arm_dev = -1;

int input_row_value(int action, int row, int *val)
{
   if (!ma_names_device_row(action)) {
      *val = row; /* an ordinary index or value: it means what it says */
      return 1;
   }
   *val = g_arm_dev;
   return g_arm_dev > 0 && model_snap_id(row) == g_arm_dev;
}

/* press-and-hold plot scrub (the plot rect itself comes from the recorded
 * ACT_SCRUB hit box via plot_rect) */
/* The highlighted point is forms.c's (forms_set_scrub): the gesture writes
 * it, the frame reads it, and neither module has to name the other. */
static int g_scrubbing; /* a plot drag is in progress */
/* A drag across the WEIGHT plot is in progress. Gesture state, like the
 * glucose scrub above: the highlighted point itself belongs to the form. */
static int g_wt_scrubbing;

static int g_scrub_ins; /* what that drag scrubs: 1 insulin, 0 glucose.
                         * Latched at the DOWN and held for the whole
                         * gesture, so a finger wandering across the
                         * band boundary (or off the plot) keeps
                         * scrubbing what it started on. */

/* touch targets recorded by the last main-screen ui_render(); read by on_input
 * to map a tap to an action. Rebuilt on the main thread only. */
static struct hits g_hits;

struct hits *input_hits(void)
{
   return &g_hits;
}

/* DOWN on a control: arm it and repaint so the pressed shade shows. */
static void press_arm(int kind, int code, int arg, int x, int y)
{
   g_arm_kind = kind;
   g_arm_code = code;
   g_arm_arg  = arg;
   g_arm_in   = 1;
   g_arm_x    = x;
   g_arm_y    = y;
   shell_repaint();
}

/* MOVE: is the finger still on the armed control? Repaint only when the
 * answer changes, so a drag can't saturate the main thread with draws. */
static void press_track(int x, int y)
{
   if (g_arm_kind == ACT_NONE)
      return;
   struct action a = ui_hit(&g_hits, x, y);
   int in =
       (a.kind == g_arm_kind && a.code == g_arm_code && a.arg == g_arm_arg);
   if (in != g_arm_in) {
      g_arm_in = in;
      if (in) {
         g_arm_x = x;
         g_arm_y = y;
      }
      shell_repaint();
   }
}

/* ---- THE FINGER THIS GESTURE BELONGS TO --------------------------------
 *
 * -1 between gestures. Latched on ACTION_DOWN and released on UP/CANCEL, so
 * every event in between can ask "where is MY pointer" rather than trusting
 * slot 0. Main thread only, like the hit list.
 *
 * This holds the id; gesture_resolve (gesturelogic.h) decides what each event
 * means for it and hands back the id to hold next -- so the RULE is testable
 * without an Android event, and this file only has to obey the verdict. */
static int g_gesture_ptr = -1;

/* Give up on the current gesture without firing anything: the armed control is
 * disarmed and any scrub stops. Deliberately NOT press_release -- every caller
 * here has established that it can no longer tell what the user meant, and the
 * armed controls include ones that delete a logged dose.
 *
 * IT HAS TO REACH EVERYTHING THE GESTURE OWNS, not just the arming. An
 * abandoned gesture never reaches the UP handler that normally tidies up, so
 * whatever it leaves set stays set with nothing touching the screen:
 *
 *   - g_scrubbing left at 1 makes `cont` true for the FIRST MOVE of the NEXT
 *     gesture, so a drag that began nowhere near the plot scrubs it -- the arm
 *     was cancelled and the scrub inherited instead;
 *   - a glucose or weight cursor left set keeps a stale readout on screen, and
 *     the weight one borrows the tab row, so the span tabs stay hidden
 *     indefinitely (the same reason the UP handler below clears it).
 *
 * IDEMPOTENT, and that is not decoration either: once a gesture is abandoned
 * every remaining MOVE of the same physical drag comes back here, and an
 * unconditional shell_repaint() would then post a frame per touch sample. It
 * repaints only when it actually cleared something. */
static void gesture_abandon(void)
{
   g_gesture_ptr = -1;
   int dirty     = (g_arm_kind != ACT_NONE) || g_scrubbing || g_wt_scrubbing ||
                   forms_scrub() >= 0;
   press_cancel();
   g_scrubbing = 0;
   if (g_wt_scrubbing) {
      g_wt_scrubbing = 0;
      forms_set_wt_scrub(-1);
      shell_ui_dirty();
   }
   if (forms_scrub() >= 0)
      forms_set_scrub(-1);
   if (dirty)
      shell_repaint();
}

/* UP/CANCEL: disarm, and say whether the action should fire -- an UP that
 * lands back on the armed control. A miss repaints to clear the shade (a
 * fired action repaints through its own path). Read g_arm_kind/g_arm_code
 * BEFORE calling: this clears them. */
static int press_release(int up, int x, int y)
{
   if (g_arm_kind == ACT_NONE)
      return 0;
   struct action a = ui_hit(&g_hits, x, y);
   int fire =
       up && a.kind == g_arm_kind && a.code == g_arm_code && a.arg == g_arm_arg;
   g_arm_kind = ACT_NONE;
   g_arm_in   = 0;
   if (!fire)
      shell_repaint();
   return fire;
}

/* THE SAME CONFIGURATION THE RENDERER USED, out of the frame it recorded it
 * in (struct hits). plot_hit and plot_point_xy have to reproduce the render's
 * mapping exactly, and both inputs to it -- the vertical scale and the marker
 * radius -- are NOT process globals the renderer writes and this path reads
 * back afterwards. The radius is derived from the screen scale and the span,
 * so this side cannot recompute it; it is recorded, like the scrub rectangle
 * beside it. */
static struct plot_cfg input_plot_cfg(void)
{
   return g_hits.plot;
}

/* Plot rectangle recorded by the last render (the ACT_SCRUB target), so a
 * drag can resolve to a datapoint even after the finger leaves the plot. */
static int plot_rect(int *x, int *y, int *w, int *h)
{
   for (int i = 0; i < g_hits.n; i++)
      if (g_hits.box[i].kind == ACT_SCRUB) {
         *x = g_hits.box[i].x;
         *y = g_hits.box[i].y;
         *w = g_hits.box[i].w;
         *h = g_hits.box[i].h;
         return 1;
      }
   return 0;
}

/* THE CHOSEN ORIENTATION AS IT IS *NOW*, applied to the main screen.
 *
 * on_input drains EVERY queued event in one pass, and a menu action can
 * change the orientation (MA_ORIENT). Read from a snapshot taken when the
 * drain began, an orientation tap and the tap that lands back on the main
 * screen -- coalesced into one drain, which is exactly what a fast double tap
 * produces -- applied the PRE-cycle value, and the screen came back the way
 * it already was. The setting is asked for after the action has run. */
static void apply_main_orientation(void)
{
   struct prefs now;
   settings_get(&now);
   jb_set_orientation(shell_activity(), now.orient);
}

int on_input(int fd, int events, void *data)
{
   shell_where("on_input");
   (void)fd;
   (void)events;
   struct AInputQueue *q  = data;
   struct AInputEvent *ev = NULL;
   while (AInputQueue_getEvent(q, &ev) >= 0) {
      if (AInputQueue_preDispatchEvent(q, ev))
         continue; /* IME took it; finished elsewhere */
      /* ONE COPY PER EVENT, not per drain: the events in a single drain can
       * be seconds apart and one of them can change these settings. */
      struct prefs sp;
      settings_get(&sp);
      int handled = 0;
      if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
         int32_t raw = AMotionEvent_getAction(ev);
         int action =
             (int)((unsigned)raw & (unsigned)AMOTION_EVENT_ACTION_MASK);

         /* ---- ONE GESTURE, ONE FINGER --------------------------------
          *
          * This read AMotionEvent_getX(ev, 0) for every event and ignored
          * POINTER_DOWN and POINTER_UP entirely. Index 0 is not a finger, it is
          * a SLOT, and Android repacks the slots as pointers come and go -- so
          * a second finger touching down, or the first lifting while a second
          * stays, moves a DIFFERENT pointer into slot 0 and the gesture
          * silently continues under it.
          *
          * What that costs here is not a cosmetic glitch. A press only ARMS the
          * control under it and the action fires on the RELEASE, so:
          *
          *   - arm DELETE with one finger, put a second finger down elsewhere,
          *     lift the first: the UP is reported at the second finger's
          *     coordinates, and press_release compares them against the armed
          *     box. The wrong control fires, or the right one fires from a
          *     touch the user made somewhere else entirely;
          *   - a weight-log scrub started by one finger follows whichever
          *     pointer lands in slot 0, so a second touch drags the selection
          *     to a reading the user never pointed at.
          *
          * So the ID is latched on DOWN and every later event is resolved
          * through it. THE POLICY IS STRICTLY ONE FINGER: a second pointer
          * touching down CANCELS the gesture rather than being ignored. For a
          * UI whose controls delete doses and silence alarms, "the user did
          * something ambiguous" must not resolve to "fire the armed control" --
          * and a cancel costs them one deliberate re-tap.
          *
          * The RULE itself is gesturelogic.c -- a decision over four integers,
          * so a host test can hand it a second finger, which these stubs
          * cannot. This end does only what needs the event: read the pointer
          * ids out of it, and act on the verdict. */
         int32_t ids[GEST_MAX_PTRS];
         struct gesture_in gin = {raw, g_gesture_ptr, ids, 0};
         unsigned long nptr    = AMotionEvent_getPointerCount(ev);
         for (unsigned long i = 0; i < nptr && gin.n < GEST_MAX_PTRS; i++)
            ids[gin.n++] = AMotionEvent_getPointerId(ev, i);
         struct gesture_out gout = gesture_resolve(&gin);
         g_gesture_ptr           = gout.latch;
         if (gout.verdict == GEST_ABANDON) {
            gesture_abandon();
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         if (gout.verdict == GEST_IGNORE) {
            /* Another finger's POINTER_UP: nothing in it is about this
             * gesture, and nothing below may run on its coordinates. Claimed
             * rather than passed on -- the touch belongs to this window. */
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* WHERE OUR FINGER IS NOW -- at the index the resolver found it, never
          * slot 0. The bounds test is for the one case that reports a
          * placeholder index: a CANCEL whose pointer list no longer holds our
          * finger. Its coordinates are read by no handler's CANCEL arm, but the
          * read still has to stay inside the event. */
         int tx = 0;
         int ty = 0;
         if (gout.index < gin.n) {
            tx = (int)AMotionEvent_getX(ev, (unsigned long)gout.index);
            ty = (int)AMotionEvent_getY(ev, (unsigned long)gout.index);
         }
         /* A sounding alarm is silenced by ANY press anywhere in the app --
          * main screen, settings, keypad, gate -- and that press does
          * nothing else. Checked before every modal handler so it always
          * wins. */
         /* The test, the silence and the acknowledgement are ONE critical
          * section, inside alarm_silence_tap -- see alarm.h. Doing it here,
          * around an unsynchronized read, could swallow a tap meant for the
          * UI or kill an alarm that had just legitimately started. */
         /* (Actions fire on the RELEASE, so no mid-gesture "touch swallow"
          * is needed: a menu cannot change under a still-held finger, and the
          * back key -- the one way it can -- cancels any armed press
          * explicitly.) */
         int was_sounding = 0;
         if (action == AMOTION_EVENT_ACTION_DOWN)
            was_sounding = alarm_silence_tap();
         if (was_sounding) {
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* the first-run rationale screen is modal: CONTINUE arms on the
          * press and fires the permission request on the release (the
          * app-wide act-on-release rule); anything else is ignored */
         if (shell_gate()) {
            if (action == AMOTION_EVENT_ACTION_DOWN) {
               struct action a = ui_hit(&g_hits, tx, ty);
               if (a.kind == ACT_GATE_CONTINUE)
                  press_arm(a.kind, a.code, a.arg, tx, ty);
               else
                  press_cancel();
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
               press_track(tx, ty);
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
               if (press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty)) {
                  shell_gate_done();
                  if (shell_activity())
                     request_ble_permissions(shell_activity());
                  shell_repaint();
               }
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* All modal menus (settings / keypad / device list) are pure now: a
          * press ARMS the recorded ACT_MENU target under it, and the release
          * -- back on the same target -- dispatches its menu_action code.
          * Sliding off first cancels without firing anything. */
         if (cur_screen()) {
            /* The WEIGHT LOG plot scrubs. It lives on a MENU screen, where
             * every other target is press-arm/release, so it is handled here
             * rather than in the main screen's gesture code: a press picks
             * the nearest point and a drag keeps picking, using the plot
             * rect the last render recorded. */
            if (cur_screen() == SCR_WTLOG &&
                (action == AMOTION_EVENT_ACTION_DOWN ||
                 (action == AMOTION_EVENT_ACTION_MOVE && g_wt_scrubbing))) {
               for (int i = 0; i < g_hits.n; i++) {
                  if (g_hits.box[i].kind != ACT_SCRUB)
                     continue;
                  if (action == AMOTION_EVENT_ACTION_DOWN &&
                      (tx < g_hits.box[i].x ||
                       tx >= g_hits.box[i].x + g_hits.box[i].w ||
                       ty < g_hits.box[i].y ||
                       ty >= g_hits.box[i].y + g_hits.box[i].h))
                     break; /* the press began outside the plot */
                  /* THE WHOLE SEQUENCE, not build_model alone. This ran on
                   * the main thread with no snapshot and no history lock,
                   * walking the very array a BLE reading inserts into. A
                   * frame the lock could not be taken for is skipped: this
                   * finger will produce another MOVE within milliseconds. */
                  struct screen sm;
                  if (!model_frame(&sm))
                     break;
                  int pick = ui_wt_hit(&sm, g_hits.box[i].x, g_hits.box[i].w,
                                       g_hits.box[i].arg, tx);
                  if (pick >= 0) {
                     forms_set_wt_scrub(pick);
                     g_wt_scrubbing = 1;
                     press_cancel(); /* scrubbing is not a button press */
                     shell_ui_dirty();
                     shell_repaint();
                  }
                  break;
               }
               if (g_wt_scrubbing) {
                  AInputQueue_finishEvent(q, ev, 1);
                  continue;
               }
            }
            if (action == AMOTION_EVENT_ACTION_UP ||
                action == AMOTION_EVENT_ACTION_CANCEL) {
               /* Scrubbing lasts only while a finger is down, as on the
                * glucose plot: the readout borrows the tab row, so leaving
                * it up would hide the span tabs indefinitely and leave a
                * stale value on screen with nothing touching it. */
               if (g_wt_scrubbing) {
                  g_wt_scrubbing = 0;
                  forms_set_wt_scrub(-1);
                  shell_ui_dirty();
                  shell_repaint();
               }
            }
            if (action == AMOTION_EVENT_ACTION_DOWN) {
               struct action a = ui_hit(&g_hits, tx, ty);
               if (a.kind == ACT_MENU) {
                  press_arm(a.kind, a.code, a.arg, tx, ty);
                  input_arm_row(a.code, a.arg); /* the DEVICE, if it is one */
               } else {
                  press_cancel();
               }
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
               press_track(tx, ty);
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
               int acode = g_arm_code; /* press_release clears these */
               int aarg  = g_arm_arg;
               int aval  = aarg;
               if (press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty) &&
                   input_row_value(acode, aarg, &aval)) {
                  menu_action(acode, aval);
                  /* GENERAL rule (so no menu needs special-casing): the
                   * moment a menu action lands back on the MAIN screen,
                   * restore its chosen orientation -- menus render portrait,
                   * main follows the chosen orientation. Without this, closing
                   * a menu left the main screen stuck in the portrait the
                   * menu-open had forced. */
                  if (cur_screen() == SCR_MAIN)
                     apply_main_orientation();
               }
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* main screen: resolve the tap against the targets recorded by the
          * last ui_render(), then run the shell-side gesture/timer state. */
         struct action act = ui_hit(&g_hits, tx, ty);
         /* A drag begins on a press inside the plot; once begun, keep
          * scrubbing for every MOVE -- even when the finger leaves the plot
          * rectangle -- using its X position (plot_hit picks by time/X
          * only). */
         int begin =
             (action == AMOTION_EVENT_ACTION_DOWN && act.kind == ACT_SCRUB);
         int cont = (action == AMOTION_EVENT_ACTION_MOVE && g_scrubbing);
         int rx   = 0;
         int ry   = 0;
         int rw   = 0;
         int rh   = 0;
         if ((begin || cont) && plot_rect(&rx, &ry, &rw, &rh)) {
            g_scrubbing = 1;
            press_cancel(); /* scrubbing is exempt; no arm may linger */
            /* Average the current sample with the batched historical ones so
             * the pick tracks the centre of the contact, not a jittery edge.
             */
            unsigned long hs = AMotionEvent_getHistorySize(ev);
            long ax          = tx;
            long ay          = ty;
            long n           = 1;
            for (unsigned long h = 0; h < hs; h++) {
               ax += (long)AMotionEvent_getHistoricalX(ev, 0, h);
               ay += (long)AMotionEvent_getHistoricalY(ev, 0, h);
               n++;
            }
            int fx = (int)(ax / n);
            int fy = (int)(ay / n);
            static struct plot_pt pts[UI_PTS_MAX];
            /* SIZED FOR THE SAME LOOP AS pts, not for the live window.
             * Left at NHIST while pts holds a long span, scrubbing 7D or
             * 30D writes up to PLOT_LONG_MAX ints into a
             * 5040-int array -- 176 kB straight through the statics that
             * follow it, which is what silently emptied the stats ring
             * minutes after every restart. */
            static int psrc[UI_PTS_MAX];
            /* Under hist_lock: hist_insert memmoves g_hist from a BLE binder
             * thread, so an unlocked copy here reads a half-shifted array
             * and the scrub lands on a datapoint that was never there. Every
             * other reader takes the lock; this one was missed. */
            int nlong2                    = 0;
            const struct ui_point *plong2 = plot_source_from(
                store_path(), realtime_s(), model_plot_hours(), &nlong2);
            int np = 0;
            if (plong2) {
               /* Hit-test what was DRAWN: on a long span that is the
                * bucketed set, not the RAM window, or the scrub would
                * select points the plot never showed. */
               for (int i = 0; i < nlong2 && np < PLOT_LONG_MAX; i++) {
                  pts[np].t      = plong2[i].t;
                  pts[np].glu    = plong2[i].glu;
                  pts[np].marker = 0;
                  pts[np].col    = 0;
                  pts[np].hidden = 0;
                  psrc[np]       = plong2[i].src;
                  np++;
               }
            } else {
               /* ONE SNAPSHOT, rather than a count and NHIST indexed reads
                * with the store lock taken by hand around them. */
               static struct reading snap[NHIST];
               np = hist_copy(snap, NHIST);
               for (int i = 0; i < np; i++) {
                  pts[i].t      = snap[i].t;
                  pts[i].glu    = snap[i].glu;
                  pts[i].marker = 0;
                  pts[i].col    = 0;
                  pts[i].hidden = 0;
                  psrc[i]       = snap[i].src;
               }
            }
            /* A HIDDEN device (marker OFF) is off the plot, so its points
             * must not be scrub-selectable either. plot_hit skips
             * pts[].hidden. Pull the (few) hidden-marker device ids in ONE
             * locked call, then flag matching points -- no per-point
             * registry lock. */
            int hid[MAX_SLOTS];
            int nhid = sensor_hidden_ids(hid, MAX_SLOTS);
            for (int i = 0; nhid > 0 && i < np; i++)
               for (int j = 0; j < nhid; j++)
                  if (psrc[i] == hid[j]) {
                     pts[i].hidden = 1;
                     break;
                  }
            /* Insulin doses ride along, in the SAME order the model
             * appends them, so the returned index maps onto m->plot.hist. */
            int np_glu = np;
            int nins   = ins_count();
            for (int i = 0; i < nins && np < UI_PTS_MAX; i++) {
               struct ins_rec ir = ins_at(i);
               pts[np].t         = ir.t;
               pts[np].glu       = 60; /* the renderer's fixed insulin y */
               pts[np].marker    = 0;
               pts[np].col       = 0;
               pts[np].size      = 0;
               pts[np].hidden =
                   (sp.ins_marker[ir.type == INS_FAST ? INS_FAST : INS_SLOW] ==
                    MARK_HIDE);
               np++;
            }
            /* WEIGHTS, in the SAME order build_model appends them (glucose,
             * doses, weights) so the index plot_hit returns maps onto
             * m->plot.hist. Missing here, they were drawn on the plot and could
             * not be scrubbed: this array -- not the model's -- is what the
             * hit test walks. They share the doses' y, so they fall inside
             * the same bottom band and the aiming rule below picks them up
             * without any extra case. */
            int nwt = wt_count();
            for (int i = 0; i < nwt && np < UI_PTS_MAX; i++) {
               struct wt_rec wr = wt_at(i);
               pts[np].t        = wr.t;
               pts[np].glu      = 60; /* the renderer's fixed bottom-line y */
               pts[np].marker   = 0;
               pts[np].col      = 0;
               pts[np].size     = 0;
               pts[np].hidden   = 0; /* weights have no hide toggle */
               np++;
            }
            /* FOOD, last, because build_model appends it last -- glucose,
             * doses, weights, food. THE ORDER IS THE CONTRACT: plot_hit
             * returns an INDEX into this array and the scrub reads that index
             * out of m->plot.hist, so the two lists must be built in the same
             * order or a tap on a food entry reads out somebody else's
             * record. Nothing checks that at compile time; it is why both
             * loops name the order they follow.
             *
             * Food shares the doses' fixed y, so it lands in the same bottom
             * band and the aiming rule below picks it up with no extra case
             * -- exactly as the weights above do. Without this the F markers
             * drew perfectly and could not be selected, which is the bug the
             * weights had and the comment above records. */
            int nfd = food_count();
            for (int i = 0; i < nfd && np < UI_PTS_MAX; i++) {
               struct food_rec fr = food_at(i);
               pts[np].t          = fr.t;
               pts[np].glu        = 60; /* the renderer's fixed bottom line */
               pts[np].marker     = 0;
               pts[np].col        = 0;
               pts[np].size       = 0;
               pts[np].hidden     = 0; /* food has no hide toggle */
               np++;
            }
            /* EXERCISE, after food, because build_model appends it last --
             * glucose, doses, weights, food, exercise. The order IS the
             * contract, for the reason spelled out above: plot_hit returns an
             * index into THIS array and the scrub reads that index out of
             * m->plot.hist, so a kind appended here in a different place
             * would read out somebody else's record.
             *
             * `span` is left at zero deliberately. It changes how a point is
             * DRAWN and not where it is, and the hit test picks by position;
             * a span here would be a second copy of a number this array has
             * no other use for. */
            int nexr = ex_count();
            for (int i = 0; i < nexr && np < UI_PTS_MAX; i++) {
               struct ex_rec er = ex_at(i);
               pts[np].t        = er.t;
               pts[np].glu      = 60; /* the renderer's fixed bottom line */
               pts[np].marker   = 0;
               pts[np].col      = 0;
               pts[np].size     = 0;
               pts[np].hidden   = 0; /* exercise has no hide toggle */
               pts[np].span     = 0;
               np++;
            }
            /* plot_hit picks by TIME alone, which would leave a dose
             * between two 5-minute CGM points a sliver of reachability.
             * Aim by finger HEIGHT instead: in the insulin band (twice the
             * below-70 strip, so the row is reachable without covering it
             * with the fingertip) only insulin is selectable, above it
             * only glucose -- sliding along the plot bottom walks the
             * doses. Decided ONCE, at the DOWN: the rest of the gesture
             * keeps scrubbing whichever series it began on, wherever the
             * finger wanders. With no dose in the visible window the whole
             * plot aims at glucose -- no dead band over nothing. */
            if (begin) {
               int oy70 = ry + rh;
               {
                  int oxx            = 0;
                  struct plot_pt ref = {0};
                  ref.t              = realtime_s();
                  ref.glu            = 70;
                  if (!plot_point_xy((struct plot_rect){rx, ry, rw, rh}, ref,
                                     realtime_s(), model_plot_hours(),
                                     input_plot_cfg(), &oxx, &oy70))
                     oy70 = ry + rh; /* degenerate window: all glucose */
               }
               /* A dose only claims the touch if one is actually NEAR the
                * finger. Asking merely "is there a dose in this window"
                * meant that on a 30-day span a single marker at one edge
                * captured a press at the far edge, and the scrub then
                * jumped to a dose an inch away rather than reading the
                * glucose under the fingertip. About half an inch of x
                * either side -- an eighth of the plot -- is roughly a
                * fingertip's own width. */
               int near_x   = rw / 8;
               int have_ins = 0;
               for (int i = np_glu; i < np && !have_ins; i++) {
                  int oxx = 0;
                  int oyy = 0;
                  if (pts[i].hidden ||
                      !plot_point_xy((struct plot_rect){rx, ry, rw, rh}, pts[i],
                                     realtime_s(), model_plot_hours(),
                                     input_plot_cfg(), &oxx, &oyy))
                     continue;
                  int dx = oxx - fx;
                  if (dx < 0)
                     dx = -dx;
                  if (dx <= near_x)
                     have_ins = 1;
               }
               g_scrub_ins = have_ins && (fy >= (2 * oy70) - (ry + rh));
            }
            int aim_ins = g_scrub_ins;
            for (int i = 0; i < np_glu; i++)
               if (aim_ins)
                  pts[i].hidden = 1;
            for (int i = np_glu; i < np; i++)
               if (!aim_ins)
                  pts[i].hidden = 1;
            /* SPLIT the doses line's shared columns, never the glucose
             * trace's: a dose and a weight logged in the same sitting sit on
             * one pixel, and without this only one of the two could ever be
             * scrubbed. On the glucose series a shared column is the normal
             * state of a multi-day span, and the plain nearest-in-time pick is
             * what keeps a drag to the left edge landing on the oldest sample.
             */
            int idx = plot_hit((struct plot_rect){rx, ry, rw, rh}, pts, np,
                               realtime_s(), model_plot_hours(),
                               input_plot_cfg(), fx, fy, aim_ins);
            if (idx != forms_scrub()) {
               forms_set_scrub(idx);
               shell_repaint();
            }
            handled = 1;
         } else if (action == AMOTION_EVENT_ACTION_DOWN) {
            if (act.kind == ACT_OPEN_SETTINGS || act.kind == ACT_MENU ||
                act.kind == ACT_PLOT_TAB) {
               /* Every real main-screen action arms here and fires on the
                * release back on the same control (press_release below). */
               press_arm(act.kind, act.code, act.arg, tx, ty);
               if (act.kind == ACT_MENU)
                  input_arm_row(act.code, act.arg);
               handled = 1;
            } else {
               press_cancel(); /* a stale arm must not survive a dead tap */
            }
         } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            if (g_arm_kind != ACT_NONE) {
               press_track(tx, ty);
               handled = 1;
            }
         } else if (action == AMOTION_EVENT_ACTION_UP ||
                    action == AMOTION_EVENT_ACTION_CANCEL) {
            g_scrubbing = 0;
            int akind   = g_arm_kind; /* press_release clears these */
            int acode   = g_arm_code;
            int aarg    = g_arm_arg;
            int fire = press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty);
            if (forms_scrub() >= 0) { /* release clears the highlight */
               forms_set_scrub(-1);
               shell_repaint();
               handled = 1;
            } else if (fire) {
               if (akind == ACT_OPEN_SETTINGS) {
                  sys_refresh(); /* snapshot system state before draw (main
                                    thread)*/
                  nav_go(SCR_SETTINGS);
                  jb_set_orientation(shell_activity(), 0);
                  shell_repaint();
               } else if (akind == ACT_MENU &&
                          input_row_value(acode, aarg, &aarg)) {
                  /* Main-screen shortcut into a menu action: the info/stats
                   * block taps straight to the primary CGM's device menu, and
                   * the big number itself opens the DEVICES screen. */
                  sys_refresh();
                  jb_set_orientation(shell_activity(), 0);
                  menu_action(acode, aarg);
                  shell_repaint();
               } else if (akind == ACT_PLOT_TAB) { /* tab (arg = hours) */
                  model_set_plot_hours(aarg);
                  shell_repaint();
               }
               handled = 1;
            }
         }
      } else if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY &&
                 AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
         /* The system back gesture/button, acting exactly like a tap on the
          * current screen's title-row X. ALWAYS claimed (handled = 1), even
          * when it does nothing: unhandled it falls through to
          * NativeActivity, which finishes the activity -- so on the main
          * screen and the gate "no effect" still has to eat the event. Act
          * on UP, the OS's own back-on-release semantics; the DOWN is
          * claimed silently. */
         if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP && !shell_gate()) {
            /* Same contract as a touch: a sounding alarm is silenced by ANY
             * press, and that press does nothing else (see the DOWN handler
             * above for why the test and the silence share the lock). */
            int was_sounding = alarm_silence_tap();
            int bix          = 0;
            int code         = was_sounding ? -1 : menu_back_code(&bix);
            if (code >= 0) {
               press_cancel(); /* the screen changes under any held finger */
               menu_action(code, bix); /* redraws via its trailing draw() */
               if (cur_screen() == SCR_MAIN)
                  apply_main_orientation(); /* see the menu rule above */
            }
         }
         handled = 1;
      }
      AInputQueue_finishEvent(q, ev, handled);
   }

   return 1; /* keep the callback registered */
}

/* Pressed-but-not-yet-fired: shade the armed control. Re-found in the
 * JUST-rebuilt hit boxes via the finger's last on-target point, and only
 * shaded while it still resolves to the same action -- a redraw that moved or
 * removed the control drops the shade rather than lighting a stranger. */
void input_press_overlay(struct ANativeWindow_Buffer *buf)
{
   if (g_arm_kind == ACT_NONE || !g_arm_in)
      return;
   int bi = ui_hit_idx(&g_hits, g_arm_x, g_arm_y);
   if (bi < 0 || g_hits.box[bi].kind != g_arm_kind ||
       g_hits.box[bi].code != g_arm_code || g_hits.box[bi].arg != g_arm_arg)
      return;
   /* the GLOW rect, not the hit rect: usually the same, narrower for controls
    * whose hit zone out-sizes their glyph (see add_glow) */
   ui_press_overlay(buf, g_hits.box[bi].gx, g_hits.box[bi].gy,
                    g_hits.box[bi].gw, g_hits.box[bi].gh);
}

void input_arm_row(int action, int row)
{
   g_arm_dev = ma_names_device_row(action) ? model_snap_id(row) : -1;
}

/* Drop any armed press without firing (a stale arm must never shade or fire
 * on a later screen -- called when a DOWN lands on nothing, and by the back
 * key, which can change the screen under a held finger). */
void press_cancel(void)
{
   g_arm_kind = ACT_NONE;
   g_arm_code = 0;
   g_arm_in   = 0;
   g_arm_dev  = -1;
}
