// SPDX-License-Identifier: GPL-3.0
// forms.c --- Typed entry (see forms.h)
// Copyright 2026 Jakob Kastelic

#include "forms.h"
#include "alarm.h"
#include "alarmlogic.h" /* the entry bounds these commits enforce */
#include "blejni.h"     /* dexble_env: a JNIEnv for THIS thread */
#include "civil.h"
#include "clock.h"
#include "exercise.h"
#include "food.h"
#include "insulin.h"
#include "keypad.h" /* enum keypad_mode: these numbers have names */
#include "log.h"
#include "nav.h"
#include "notify.h"
#include "remote.h"
#include "selection.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "status.h"
#include "syncjni.h"
#include "tzoff.h"
#include "uiact.h"
#include "uimodel.h"
#include "util.h"
#include "weight.h"
#include <stdio.h>
#include <string.h>

/* LOG WEIGHT form state. The value is held in TENTHS of the DISPLAY unit, not
 * grams: it is what the user typed and what the keypad round-trips, and it is
 * converted once, on CONFIRM. Holding grams here instead would re-render the
 * field every time the unit preference changed mid-entry. */
/* ---- ONE STRUCT PER WORKFLOW -------------------------------------------
 *
 * These were eighteen adjacent globals, and adjacency was the only thing
 * grouping them. Nothing in a name said which editor a field belonged to, so
 * nothing stopped one workflow reading another's leftovers -- and the
 * sentinels that decide "new entry" versus "editing an existing one" lived
 * mainly in comments beside the declarations, which is to say nowhere the
 * compiler could see.
 *
 * The failure that shape invites is not a crash. It is opening LOG WEIGHT
 * straight after editing one, with `edit` still holding the previous row, and
 * having the new entry silently rewrite it. Each struct below is opened
 * through one helper that sets EVERY field, so a workflow starts from a state
 * it stated in full rather than from whatever the last one left behind. */

/* Which page each LOG is showing, and how the WEIGHT LOG's plot is being
 * read. Paging and scrubbing are typed-entry state in the same sense the
 * forms are: the user is navigating within a screen, not between screens. */
struct log_view {
   int ins_page;
   int wt_page;
   int wt_tab;
   int wt_scrub; /* -1 = not scrubbing */
   int foodtype_page; /* which page of the FOOD TYPE picker is showing */
   int foodlog_page;  /* ...and of the FOOD LOG table */
};
static struct log_view g_view = {0, 0, 0, -1, 0, 0};

/* LOG/EDIT WEIGHT.
 *
 * `f` IS THE MODEL'S OWN STRUCT, not a copy of its fields. weight.h has
 * defined `struct wt_form` and `wt_form_open` all along -- typed, tested by
 * weighttest, and holding the rule that a fresh form opens on the last logged
 * weight -- while this file kept its own three loose globals for the same
 * three values beside it. Two representations of one form is how the shell
 * and the model come to disagree about what is being edited.
 *
 * `orig` is the part the model has no opinion about: a COPY of the entry as
 * it was, which is the match key for the rewrite. An index into the tail
 * would go stale the moment the log reloads, and rewriting by position is how
 * an edit lands on the wrong row. */
struct wt_draft {
   struct wt_form f;
   struct wt_rec orig;
};
static struct wt_draft g_wt = {
    {0, 0, -1},
    {0, 0}
};

/* LOG/EDIT FOOD. The third of these, and the same shape for the same reasons:
 * the values being typed, plus the entry as it was on disk for an edit.
 *
 * WHICH FOOD IS AN ID, not a position in the vocabulary. The picker can ADD a
 * food -- that is the point of its NEW FOOD row -- which grows the table on
 * the very tap that returns here, so an index would name a different food
 * afterwards and nothing on the screen would say so. */
struct food_draft {
   long t;
   int type; /* a food_type id; FOOD_TYPE_NONE = nothing chosen yet */
   int g;
   int edit; /* index being edited, < 0 for a new entry */
};
static struct food_draft g_food = {0, FOOD_TYPE_NONE, 0, -1};

/* LOG/EDIT INSULIN. Same shape and same reason as the weight draft above:
 * insulin.h's `struct ins_form` is the model (the instant is edited as a
 * whole; units re-populate from the last dose of the selected type when the
 * form opens or the type toggles), and `orig` is the rewrite's match key. */
struct ins_draft {
   struct ins_form f; /* insulin.h's own, with ins_form_open/toggle_type */
   struct ins_rec orig;
};
static struct ins_draft g_ins = {
    {0, 0, 0, -1},
    {0, 0, 0}
};

/* WHICH FORM'S INSTANT THIS MODE EDITS, as a pointer or NULL.
 *
 * This was `kp_edits_weight(m) ? &g_wt.f.t : &g_ins.f.t` at both call sites --
 * a two-way choice that was right while there were two forms and silently
 * wrong the moment there was a third: every food date would have moved the
 * INSULIN form's instant, because "not weight" meant insulin by default.
 * keypad.h carries the full argument; what matters here is that the mapping is
 * now total, and a mode that is not a form field at all gets NULL rather than
 * somebody else's timestamp. */
static long *form_instant_of(enum keypad_mode m)
{
   switch (kp_form_of(m)) {
      case KP_FORM_WEIGHT: return &g_wt.f.t;
      case KP_FORM_INSULIN: return &g_ins.f.t;
      case KP_FORM_FOOD: return &g_food.t;
      case KP_FORM_NONE: break;
   }
   return 0;
}


/* ---- OPENING A DRAFT SETS ALL OF IT --------------------------------------
 *
 * EVERY FIELD, INCLUDING THE ONES THE PATH HAS NO USE FOR. Opening the weight
 * form for a new entry used to set `t`, `tenths` and `edit` and say nothing
 * about `orig`, which then still held the row of whatever was edited last.
 * That is safe only for exactly as long as every reader of `orig` remembers
 * to test `edit` first -- an invariant spread across a dozen branches, none of
 * which the compiler checks, guarding a rewrite that lands on a row the user
 * is not looking at.
 *
 * These take the whole draft, so a workflow starts from a state it stated in
 * full rather than from whatever the last one left behind. */

static void wt_draft_new(struct wt_draft *d, const struct prefs *sp)
{
   struct wt_rec none = {0, 0};
   d->orig            = none;
   /* wt_form_open is weight.h's, and holds the rule this file must not
    * restate: a fresh form opens on the LAST logged weight, because a
    * weigh-in moves by ounces and starting from zero would make every entry a
    * full retype. weighttest pins it. */
   wt_form_open(&d->f, wt_newest().g, sp->wunits, realtime_s());
}

static void wt_draft_edit(struct wt_draft *d, int i, const struct prefs *sp)
{
   struct wt_rec row = wt_at(i);
   d->orig           = row; /* the rewrite's match key, not an index */
   d->f.edit         = i;
   d->f.t            = row.t;
   d->f.tenths       = wt_to_tenths(row.g, sp->wunits);
}

/* No longer editing anything. The draft's values stay readable for the render
 * that follows the tap; what changes is that they no longer name a row. */
static void wt_draft_done(struct wt_draft *d)
{
   struct wt_rec none = {0, 0};
   d->f.edit          = -1;
   d->orig            = none;
}

static void ins_draft_new(struct ins_draft *d, int type)
{
   struct ins_rec none = {0, 0, 0};
   d->orig             = none;
   ins_form_open(&d->f, type, realtime_s());
}

static void ins_draft_edit(struct ins_draft *d, int i)
{
   struct ins_rec row = ins_at(i);
   d->orig            = row;
   d->f.edit          = i;
   d->f.t             = row.t;
   d->f.type          = row.type;
   d->f.units         = row.units;
}

static void ins_draft_done(struct ins_draft *d)
{
   struct ins_rec none = {0, 0, 0};
   d->f.edit           = -1;
   d->orig             = none;
}

/* THE KEYPAD AND WHAT THE DIGITS ARE FOR.
 *
 * One editor serves every typed value in the app, so everything that says
 * which value is being typed belongs together: the buffer, the refusal
 * message, the mode, where to return on close, and the three pending values
 * that different modes commit to different places.
 *
 * 64: the widest thing typed here is an email address or a host name, both of
 * which are 63-byte fields. It was 24, which silently truncated an address at
 * 23 characters -- and the truncation only became visible when pairing failed
 * against an account that did not exist. */
struct kp_state {
   char entry[64];
   int len;
   /* Why the last entry was refused, shown under the field until the next
    * keystroke. Empty means nothing was refused. See ui.h's kp_err. */
   char err[40];
   enum keypad_mode mode;
   enum ui_screen ret; /* where the keypad returns on close */
   /* THREE fields share the alphanumeric editor -- a sensor name, the sync
    * server and the account email -- and they commit to different places, so
    * which one opened it has to be recorded rather than guessed from the
    * menu. */
   int label_field;
   /* The value presently being typed on the rescale keypad, awaiting CONFIRM.
    * Keypad state, not calibration state: calib.c is told the number only
    * once the user commits to it. */
   int rescale;
   /* The calibration value awaiting CONFIRM, mg/dL; 0 = none. Same rule. */
   int cal_pending;
   /* INS_SLOW/INS_FAST being styled; -1 = the picker edits a sensor's
    * styling. */
   int markpick_ins;
};
static struct kp_state g_kp = {{0}, 0, {0}, 0, 0, 0, 0, 0, -1};

/* Close the keypad/device-list back to wherever pairing was launched from:
 * the settings menu (g_kp.ret==SCR_SETTINGS) or the main screen
 * (SCR_MAIN, restoring the chosen orientation). */
void keypad_close(void)
{
   /* A refusal belongs to the field that refused. Leaving it set carried it to
    * the NEXT keypad the user opened, where it described nothing. */
   g_kp.err[0] = 0;
   nav_go(g_kp.ret);
   if (!g_kp.ret)
      shell_orient_apply();
}

/* THE ZONE THE FORMS EDIT IN. Every date/time entry below splits an instant
 * into a civil date and recombines it, and both halves of that need the
 * offset in force AT THE INSTANT BEING EDITED -- not g_tz_off, which is the
 * offset TODAY.
 *
 * That is TODO 131 exactly: the keypad split and recombined with g_tz_off, so
 * moving a dose to a date on the far side of a DST boundary persisted it an
 * hour wrong, and wrote today's offset into its tz column so nothing
 * downstream could tell it had happened. What each entry does about the
 * repeated and the skipped hour is stated once, in civil.h, and applied by
 * civil_reaim.
 *
 * dexble_env(), not g_act->env: it returns a JNIEnv valid on the CALLING
 * thread, and these run on the UI thread. g_tz_off is the fallback when there
 * is no VM at all -- a stale offset, but never a wild one, which is tzoff.h's
 * standing rule. */
static long form_zone(void *ctx, long t)
{
   (void)ctx;
   JNIEnv *env = dexble_env();
   return env ? tz_offset_at(env, t) : g_tz_off;
}

/* FOOD actions, split out like wt_action so menu_action stays small.
 * Returns 1 when `action` was one of ours.
 *
 * THE FLOW, AND WHY IT IS SHAPED LIKE THIS.
 *
 * Logging food is two decisions -- which food, then how much -- and the FOOD
 * button opens the PICKER, not the form, because the food is the one that can
 * fail: the vocabulary may not have it yet. So the picker is the first
 * question, and the form is what the picker returns to.
 *
 * The form is pushed onto the path UNDER the picker, unrendered, for exactly
 * the reason wt_action pushes SCR_WEIGHT under the keypad: nav_go RETURNS to a
 * screen already on the path rather than pushing a second copy, so picking a
 * food pops back to the form and the form's own exit goes to whatever opened
 * the flow -- the ADD menu or the main screen -- without either of them being
 * named here. Skipping the push instead leaves the form off the path, and then
 * the two exits chase each other and nothing reaches the main screen. That
 * failure has happened in this file before; the comment in wt_action is the
 * record of it. */
int food_action(int action, int ix)
{
   if (action == MA_FOOD_OPEN) {
      forms_food_open(realtime_s());
      /* Both, in this order. See above: the form has to be BELOW the picker
       * on the path or there is nothing for the picker to return to. */
      nav_go(SCR_FOOD);
      nav_go(SCR_FOODTYPE);
   } else if (action == MA_FOODTYPE_PICK) {
      /* THE ROW GAVE AN INDEX; WHAT IS STORED IS AN ID. The vocabulary can
       * grow between the frame that drew the row and the tap that lands on it
       * -- NEW FOOD does exactly that -- so an index kept past this moment
       * would name a different food. Resolve it here, immediately, and store
       * the id. An index that no longer exists resolves to nothing and the
       * form is left alone rather than being pointed at a neighbour. */
      struct food_type ft = food_type_at(ix);
      if (ft.id != FOOD_TYPE_NONE) {
         forms_food_type_set(ft.id);
         nav_back(); /* to the form: whatever opened the picker */
      }
   } else if (action == MA_FOODTYPE_NEW) {
      /* The letter keypad, seeded empty. A new food is a word nobody has
       * typed here before, so there is nothing useful to pre-fill. */
      g_kp.len = 0;
      g_kp.entry[0] = 0;
      nav_go(SCR_LABEL);
      forms_set_label_field(LABEL_FOOD);
      forms_kp_return_set(SCR_FOODTYPE);
   } else if (action == MA_FOODLOG_OPEN) {
      nav_go(SCR_FOODLOG);
   } else if (action == MA_FOODLOG_BACK) {
      nav_back();
   } else if (action == MA_FOODLOG_PREV) {
      if (g_view.foodlog_page > 0)
         g_view.foodlog_page--;
   } else if (action == MA_FOODLOG_NEXT) {
      /* The renderer clamps an over-large page to the last one, so walking
       * past the end shows the end rather than an empty table -- how many
       * rows fit is a property of the window, which nothing here can see. */
      g_view.foodlog_page++;
   } else if (action == MA_FOODTYPE_BACK) {
      nav_back();
   } else if (action == MA_FOOD_EDIT) {
      /* ROW 0 IS THE TYPE, and it is the one field that is not a number:
       * it reopens the PICKER rather than a keypad, so there is one way to
       * choose a food rather than two that could disagree. Every other row
       * maps through kp_food_field, which owns the row-to-mode table -- the
       * arithmetic version of that (`KP_FOOD_G + ix`) is what keypad.h
       * records going wrong for the insulin form. */
      if (ix == 0) {
         nav_go(SCR_FOODTYPE);
      } else {
         enum keypad_mode mode = kp_food_field(ix - 1);
         if (mode != KP_NONE) {
            nav_go(SCR_KEYPAD);
            g_kp.mode = mode;
            g_kp.ret  = SCR_FOOD;
            g_kp.len  = 0;
         }
      }
   } else if (action == MA_FOOD_CONFIRM) {
      /* BOTH REFUSALS ARE VISIBLE, and neither leaves the form.
       *
       * A portion with no food is not an entry, and a food with no portion is
       * not one either -- food_append refuses both, but by then the user is
       * off the screen that could fix it. So the form checks what it can say
       * something useful about and stays put; food_append remains the
       * authority on the bounds, and its refusal is reported the same way. */
      if (g_food.type == FOOD_TYPE_NONE) {
         set_status("CHOOSE A FOOD FIRST");
      } else if (g_food.g < FOOD_MIN_G) {
         set_status("ENTER HOW MANY GRAMS");
      } else if (food_append(g_food.t, g_food.type, g_food.g,
                             form_zone(0, g_food.t)) != 0) {
         /* PERSISTENCE FAILED, SO THE FORM STAYS. Navigating away here would
          * discard a draft whose write did not happen -- the failure items
          * 136-138 are about, in a form written after them. */
         set_status("FOOD NOT SAVED");
      } else {
         nav_back();
      }
   } else if (action == MA_EXERCISE) {
      /* ONE PRESS, NO SCREEN. The button cycles and nothing else happens
       * here: what makes the value a record is a minute of not being pressed
       * again, and that is decided by the tick, not by this tap. */
      exercise_button_press(mono_s());
   } else if (action == MA_FOOD_DISCARD) {
      /* Leave the entry form without logging. The draft is left alone
       * deliberately: nothing has been written, and the next MA_FOOD_OPEN
       * calls forms_food_open, which is the one place that decides what a
       * fresh form starts from. Clearing it here as well would be a second
       * opinion about that, in the function that knows least about it. */
      nav_back();
   } else if (action == MA_FOODPAGE_PREV) {
      if (g_view.foodtype_page > 0)
         g_view.foodtype_page--;
   } else if (action == MA_FOODPAGE_NEXT) {
      /* THE UPPER BOUND IS THE RENDERER'S, not this function's: how many
       * types fit on a page depends on the window, which nothing here can
       * see. The renderer clamps the page it is given, so an over-large value
       * shows the last page rather than an empty one -- and the next tap on
       * '<' walks back from where the user actually is. */
      g_view.foodtype_page++;
   } else {
      return 0;
   }
   return 1;
}

/* WEIGHT actions, split out like ins_action so menu_action stays small.
 * Returns 1 when `action` was one of ours. */
int wt_action(int action, int ix)
{
   struct prefs sp;
   settings_get(&sp);
   if (action == MA_WT_OPEN) {
      /* Pre-populate: now, and the LAST logged weight -- a weigh-in moves by
       * ounces, so the previous value is nearly always one or two keypresses
       * from the new one, and starting from zero would make every entry a
       * full retype. */
      wt_draft_new(&g_wt, &sp);
      /* STRAIGHT TO THE KEYPAD, not to the form. Logging a weight is one
       * number, and every door into this action -- the ADD menu button, the
       * pinned main-screen button -- already says which number. The form in
       * between existed only to be tapped once, on the row this opens.
       *
       * BUT THE FORM STILL GOES ON THE PATH, because that is what makes the
       * way out work. OK and X both land on it (g_kp.ret), and nav_go
       * RETURNS to a screen already on the path instead of pushing a second
       * copy -- so the keypad pops and the form's own CANCEL goes back to
       * whatever opened the flow. Skipping the push instead put a screen
       * BELOW the keypad that was not on the path, and the two exits chased
       * each other: LOG WEIGHT returned to WEIGHT, WEIGHT returned to LOG
       * WEIGHT, and nothing reached the main screen. The user has to be able
       * to leave. It is not rendered on the way in -- the keypad opens on
       * top of it in the same tap. */
      nav_go(SCR_WEIGHT);
      nav_go(SCR_KEYPAD);
      g_kp.mode = KP_WEIGHT;
      g_kp.ret  = SCR_WEIGHT;
      g_kp.len  = 0;
   } else if (action == MA_WTLOG_EDIT) {
      /* A row in the table opens that entry in the EDIT WEIGHT form. Keep a
       * COPY as the rewrite's match key -- see g_wt.orig. */
      int i = ix;
      if (i >= 0 && i < wt_count()) {
         wt_draft_edit(&g_wt, i, &sp);
         nav_go(SCR_WEIGHT);
      }
   } else if (action == MA_WTTAB) {
      g_view.wt_tab   = ix;
      g_view.wt_scrub = -1; /* the picked point may not be in the new span */
   } else if (action == MA_WT_DELETE) {
      if (g_wt.f.edit >= 0)
         nav_go(SCR_WTDEL); /* confirm first; this tap deletes nothing */
   } else if (action == MA_WTDEL_NO) {
      nav_go(SCR_WEIGHT);
   } else if (action == MA_WTDEL_YES) {
      /* THE TARGET SURVIVES A FAILED DELETE. g_wt.orig is the exact entry
       * weight_delete matches on, and it is the only thing a second YES can
       * aim at; this reported the failure and then cleared it anyway, so the
       * retry had no row to delete and the user was back on the log with
       * nothing to say which weigh-in they had meant. Stay on the
       * confirmation instead -- the retry is then one press, on the screen
       * already in front of them. */
      enum draft_fate fate = DRAFT_RETRY;
      if (g_wt.f.edit < 0) {
         /* Not editing anything, so there is no target to keep and no retry
          * to offer. Leave as before. */
         fate = DRAFT_DONE;
      } else if (weight_delete(&g_wt.orig) == 0) {
         LOGI("weight entry deleted: %ld g at %ld", g_wt.orig.g, g_wt.orig.t);
         set_status("WEIGHT DELETED");
         fate = DRAFT_DONE;
      } else {
         set_status("WEIGHT: DELETE FAILED");
      }
      if (fate == DRAFT_DONE) {
         wt_draft_done(&g_wt);
         nav_go(SCR_WTLOG);
      }
      shell_ui_dirty();
   } else if (action == MA_WTLOG_OPEN) {
      g_view.wt_page = 0;
      nav_go(SCR_WTLOG);
   } else if (action == MA_WTLOG_PREV) {
      if (g_view.wt_page > 0)
         g_view.wt_page--;
   } else if (action == MA_WTLOG_NEXT) {
      g_view.wt_page++; /* the renderer clamps to the last page */
   } else if (action == MA_WT_EDIT) {
      /* Tapping a form value opens the keypad for EXACT entry. Which field
       * row `ix` is belongs to the keypad's own table (kp_weight_field): the
       * weight has its own mode, and date/time/year are shared with the
       * insulin form because they are a calendar instant and carry no
       * insulin meaning -- the keypad returns here, so nothing crosses
       * over. */
      /* AN INDEX THAT NAMES NO FIELD OPENS NOTHING. kp_weight_field answers
       * KP_NONE for one, and storing that would put the keypad on screen
       * with the renderer's red "BAD KP MODE" -- visible, which is right,
       * but it is a tap that should simply do nothing. */
      enum keypad_mode wm = kp_weight_field(ix);
      if (wm == KP_NONE)
         return 0;
      nav_go(SCR_KEYPAD);
      g_kp.mode = wm;
      g_kp.ret  = SCR_WEIGHT;
      g_kp.len  = 0;
   } else if (action == MA_WT_CONFIRM) {
      /* The one write, on the explicit CONFIRM only (the calibration rule).
       */
      if (cur_screen() == SCR_WEIGHT) {
         long g = wt_from_tenths(g_wt.f.tenths, sp.wunits);
         int rc = -1;
         int ed = (g_wt.f.edit >= 0);
         /* THE OFFSET AT THE INSTANT BEING WRITTEN, not the offset now. A
          * weigh-in backdated across a DST boundary used to carry today's
          * offset in its tz column, which is the one field that could have
          * revealed the hour error the recombination above used to make. */
         long wtz = form_zone(0, g_wt.f.t);
         if (g > 0)
            rc = ed ? weight_update(&g_wt.orig, g_wt.f.t, g, wtz)
                    : weight_append(g_wt.f.t, g, wtz);
         if (rc == 0) {
            LOGI("weight %s: %ld g at %ld", ed ? "edited" : "logged", g,
                 g_wt.f.t);
            set_status(ed ? "WEIGHT EDITED" : "WEIGHT LOGGED");
            /* An EDIT returns to the log it was opened from; a NEW entry is
             * a completed task and lands on the main screen (the insulin
             * rule).
             */
            nav_go(ed ? SCR_WTLOG : SCR_MAIN);
            wt_draft_done(&g_wt);
         } else {
            /* Refuse VISIBLY. A weight the user believes recorded but is not
             * is a silent hole in the only copy of that number. */
            set_status("WEIGHT: WRITE FAILED");
         }
         shell_ui_dirty();
      }
   } else if (action == MA_WT_DISCARD) {
      /* Back where it came from: the log for an edit, otherwise the screen
       * the form was opened from (the record-the-origin rule -- which this
       * line only claimed to follow while it named a fixed menu). */
      if (g_wt.f.edit >= 0)
         nav_go(SCR_WTLOG);
      else
         nav_back();
      wt_draft_done(&g_wt);
   } else if (action == MA_WUNITS) {
      /* Display only: the file is grams, so this re-renders history rather
       * than converting it. */
      if (settings_set_wunits((sp.wunits == WT_LB) ? WT_KG : WT_LB) !=
          SETTINGS_OK)
         set_status("UNITS NOT SAVED");
   } else {
      return 0; /* not ours */
   }
   return 1;
}

int ins_action(int action, int ix)
{
   if (action == MA_INS_OPEN || action == MA_INS_FAST ||
       action == MA_INS_SLOW) {
      /* The ADD menu picks the type up front (FAST / SLOW buttons); the
       * legacy MA_INS_OPEN keeps the last-used type. Pre-populate: now
       * (whole minute) and the type's last entered amount (1 U when none
       * is known). */
      /* The form's own rules live in insulin.c, where a test can reach them:
       * which amount a fresh form offers, and why the instant is a whole
       * minute. */
      int itype = -1; /* -1 = "keep whatever the form had" */
      if (action == MA_INS_FAST)
         itype = INS_FAST;
      else if (action == MA_INS_SLOW)
         itype = INS_SLOW;
      ins_draft_new(&g_ins, itype);
      /* The screen the tap came from: the ADD menu, or the main screen when
       * this action is one of its shortcut buttons. */
      nav_go(SCR_INSULIN);
   } else if (action == MA_INS_TYPE) {
      ins_form_toggle_type(&g_ins.f);
   } else if (action == MA_INSLOG_OPEN) {
      g_view.ins_page = 0;
      nav_go(SCR_INSLOG);
   } else if (action == MA_INSLOG_PREV) {
      if (g_view.ins_page > 0)
         g_view.ins_page--;
   } else if (action == MA_INSLOG_NEXT) {
      g_view.ins_page++; /* render clamps to the last page */
   } else if (action == MA_INS_EDIT) {
      /* Tapping a form value opens the keypad for EXACT entry: units (2
       * digits), date (MMDD), time (HHMM) or year (YYYY). The keypad's OK
       * validates and writes back; X returns unchanged. */
      enum keypad_mode im = kp_ins_field(ix); /* KP_NONE opens nothing */
      if (im == KP_NONE)
         return 0;
      nav_go(SCR_KEYPAD);
      g_kp.mode = im;
      g_kp.ret  = SCR_INSULIN;
      g_kp.len  = 0;
   } else if (action == MA_INS_CONFIRM) {
      /* The one write, on the explicit CONFIRM only (the calibration rule).
       * Editing rewrites the matched original row; logging appends. */
      if (cur_screen() == SCR_INSULIN) {
         int rc = -1;
         /* The dose's OWN offset, resolved at the dose's instant. See the
          * weight CONFIRM above and TODO 131: a dose moved to a date in the
          * other half of the year was persisted with today's offset. */
         long itz = form_zone(0, g_ins.f.t);
         if (g_ins.f.edit >= 0)
            rc = insulin_update(&g_ins.orig, g_ins.f.t, g_ins.f.type,
                                g_ins.f.units, itz);
         else
            rc = insulin_append(g_ins.f.t, g_ins.f.type, g_ins.f.units, itz);
         enum draft_fate fate = rc == 0 ? DRAFT_DONE : DRAFT_RETRY;
         if (fate == DRAFT_DONE) {
            LOGI("insulin %s: %d U %s at %ld",
                 g_ins.f.edit >= 0 ? "edited" : "logged", g_ins.f.units,
                 insulin_type_name(g_ins.f.type), g_ins.f.t);
            set_status(g_ins.f.edit >= 0 ? "INSULIN EDITED" : "INSULIN LOGGED");
         } else {
            /* Refuse VISIBLY -- a dose the user believes recorded but is not
             * would corrupt every judgement made on top of the log. */
            set_status("INSULIN: WRITE FAILED");
         }
         /* ...AND STAY ON THE FORM WHEN IT FAILED. The draft is the only
          * copy of what was typed and g_ins.f.edit is the only thing saying
          * WHICH dose is being rewritten; tearing both down after a write
          * that did not happen made the retry a full retype at best, and at
          * worst -- see MA_INSDEL_YES -- a second copy of the dose. */
         if (fate == DRAFT_DONE) {
            /* CONFIRM on a NEW dose lands on the MAIN screen -- logging a
             * dose is a completed task, not a detour to return from, and the
             * status banner + plot marker there ARE the confirmation. An
             * EDIT still returns to the log it was opened from -- which the
             * path knows. */
            if (g_ins.f.edit >= 0)
               nav_back();
            else
               nav_home();
            ins_draft_done(&g_ins);
         }
         shell_ui_dirty();
      }
   } else if (action == MA_INS_DELETE) {
      /* Confirm first; this action deletes nothing (the SCR_FORGET rule --
       * DELETE sat right between CANCEL and CONFIRM, one mis-tap from
       * silently losing a logged dose). */
      if (cur_screen() == SCR_INSULIN && g_ins.f.edit >= 0)
         nav_go(SCR_INSDEL);
   } else if (action == MA_INSDEL_YES) {
      /* The one deleting control, on the confirmation screen only. */
      if (cur_screen() == SCR_INSDEL && g_ins.f.edit >= 0) {
         /* A FAILED DELETE IS THE ONE THAT DUPLICATES THE DOSE. This used to
          * report the failure, drop back to the still-populated EDIT form
          * and clear g_ins.f.edit on the way -- so the form no longer knew
          * it was amending a row, and the next CONFIRM (the obvious thing to
          * try) APPENDED a second copy of a dose that had never been
          * deleted. Two identical entries in the record of what was
          * injected is worse than either the failed delete or the failed
          * edit. So the confirmation stays up, holding g_ins.orig, and YES
          * retries the same delete. */
         enum draft_fate fate = DRAFT_RETRY;
         if (insulin_delete(&g_ins.orig) == 0) {
            set_status("INSULIN DELETED");
            fate = DRAFT_DONE;
         } else {
            set_status("INSULIN: DELETE FAILED");
         }
         if (fate == DRAFT_DONE) {
            nav_back();
            ins_draft_done(&g_ins);
         }
         shell_ui_dirty();
      }
   } else if (action == MA_INSDEL_NO) {
      if (cur_screen() == SCR_INSDEL)
         nav_go(SCR_INSULIN); /* back to the EDIT form, state intact */
   } else if (action == MA_INSLOG_EDIT) {
      /* Open this dose in the EDIT form, pre-filled; remember the ORIGINAL
       * row so the eventual rewrite matches content, not a tail index that
       * may have shifted meanwhile. */
      int i = ix;
      if (cur_screen() == SCR_INSLOG && i >= 0 && i < ins_count()) {
         ins_draft_edit(&g_ins, i);
         nav_go(SCR_INSULIN);
      }
   } else if (action == MA_INS_DISCARD) {
      if (cur_screen() == SCR_INSULIN) {
         nav_back();
         ins_draft_done(&g_ins);
      }
   } else if (action == MA_INSMARK_OPEN) {
      g_kp.markpick_ins = ix; /* INS_SLOW / INS_FAST */
      nav_go(SCR_MARKPICK);
   } else if (action == MA_INSMARK_BACK) {
      g_kp.markpick_ins = -1;
      nav_go(SCR_DISPLAY); /* the row lives on the DISPLAY menu */
   } else {
      return 0; /* not an insulin action */
   }
   return 1;
}

/* Render a mg/dL bound in the units the screen is showing.
 *
 * The threshold and calibration keypads ACCEPT mmol/L when the display unit is
 * set, so a refusal quoting mg/dL sends the user to check a number they never
 * typed -- the comment on the calibration branch already notes that mmol/L
 * users are the ones who hit that bound. the renderer does this correctly for
 * the MAX: line it draws one row above; this is the same conversion for the
 * refusal text. */
static void fmt_bound(char *out, int cap, int mgdl)
{
   struct prefs sp;
   settings_get(&sp);
   if (sp.units) {
      int t = (mgdl * 10) / 18;
      (void)snprintf(out, (size_t)cap, "%d.%d", t / 10, t % 10);
   } else {
      (void)snprintf(out, (size_t)cap, "%d", mgdl);
   }
}

/* WHAT A TYPED TEXT FIELD MEANT. The label screen is one editor serving three
 * different fields -- the account email, the sync server, and a device's own
 * name -- so the validation belongs to the FIELD, not to the editor. Each is
 * refused with its own message, because "invalid" on an email that failed an
 * address check and a server that failed a hostname check are different
 * problems with different fixes. */
int label_commit(void)
{
   if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_EMAIL) {
      /* The account email. Lower-cased for the same reason as the server,
       * and required to look like an address at all -- a typo here fails
       * pairing with a message about the code, which is the wrong thing to
       * go looking at. */
      char em[sizeof(((struct sync_creds *)0)->email)];
      int n  = g_kp.len < (int)sizeof em - 1 ? g_kp.len : (int)sizeof em - 1;
      int at = 0;
      int dot_after_at = 0;
      for (int i = 0; i < n; i++) {
         char c = g_kp.entry[i];
         em[i]  = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
         if (em[i] == '@')
            at++;
         else if (em[i] == '.' && at)
            dot_after_at = 1;
      }
      em[n] = 0;
      if (n < 5 || at != 1 || !dot_after_at || em[0] == '@' ||
          em[n - 1] == '@' || em[n - 1] == '.') {
         LOGI("account '%s' does not look like an address, not saved", em);
         (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT AN EMAIL ADDRESS");
         g_kp.len = 0;
         shell_ui_dirty();
         return COMMIT_STAY;
      }
      /* A SETTER THAT DID NOT COMMIT DOES NOT START A WORKFLOW. The address
       * on screen is the one that was already stored, so retrying the sync
       * against it -- and telling the user it was saved by moving on -- would
       * both be false. Stay on the editor with the failure showing. */
      if (settings_set_email(em) != SETTINGS_OK) {
         set_status("EMAIL NOT SAVED");
         return COMMIT_STAY;
      }
      /* THE ACCOUNT WAS WHAT THE LAST FAILURE WAS ABOUT, often enough: an
       * address typed wrong is refused by the server, and the correction
       * must be tried NOW rather than after the schedule the wrong one
       * earned. The server and port paths get this through
       * remote_forget_cursor; the address does not change the server, so it
       * asks directly. */
      remote_retry_now();
      g_kp.len = 0;
      nav_go(SCR_REMOTE);
   } else if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_SERVER) {
      /* SERVER. Lower-cased because the editor can only type upper case and
       * a host name reads wrong shouted. Malformed input refuses VISIBLY --
       * the entry is cleared and the editor stays open -- because silently
       * storing a bad server would point every future sync at nothing. */
      char host[sizeof(((struct prefs *)0)->remote_server)];
      int n = g_kp.len < (int)sizeof host - 1 ? g_kp.len : (int)sizeof host - 1;
      for (int i = 0; i < n; i++) {
         char c  = g_kp.entry[i];
         host[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
      }
      host[n] = 0;
      if (!remote_server_valid(host)) {
         LOGI("server '%s' malformed, not saved", host);
         (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A SERVER NAME");
         g_kp.len = 0;
         shell_ui_dirty();
         return COMMIT_STAY;
      }
      /* NOT SAVED, NOT DONE: forgetting the identity below is irreversible
       * -- it drops the paired key -- and doing it for a server change that
       * did not persist would unpair the phone from the server it is still
       * configured for. */
      if (settings_set_server(host) != SETTINGS_OK) {
         set_status("SERVER NOT SAVED");
         return COMMIT_STAY;
      }
      /* A DIFFERENT server holds a different record: whatever we knew about
       * what it already had is meaningless now. */
      remote_forget_cursor();
      g_kp.len = 0;
      nav_go(SCR_REMOTE);
   } else if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_FOOD) {
      /* A NEW FOOD: added to the vocabulary and CHOSEN in one step.
       *
       * Naming a food is already the act of picking it -- nobody types
       * PORRIDGE in order to then go and find PORRIDGE in a list -- so the
       * commit sets the form's type and returns to the form rather than to
       * the picker it was opened from. nav_go RETURNS to SCR_FOOD because it
       * is already on the path (food_action pushed it under the picker), so
       * the keypad and the picker are both discarded in one step and the
       * form's own exit still goes wherever the flow began.
       *
       * food_type_add OWNS the rules -- what a name may contain, what happens
       * to one that is already there, what happens when the vocabulary is
       * full -- and it answers with an id or -1. Restating any of that here
       * would be a second copy of the format's constraints, in a file that
       * has no business knowing them. A refusal keeps the keypad open with
       * the text still in it, because the alternative is silently discarding
       * something the user typed. */
      g_kp.entry[g_kp.len < (int)sizeof g_kp.entry ? g_kp.len : 0] = 0;
      int id = food_type_add(g_kp.entry);
      if (id <= FOOD_TYPE_NONE) {
         set_status("FOOD NAME NOT SAVED");
         return COMMIT_DONE; /* stay put: the text is still on the keypad */
      }
      forms_food_type_set(id);
      g_kp.len = 0;
      nav_go(SCR_FOOD);
   } else if (cur_screen() == SCR_LABEL) {
      /* The registry validates, renames under its own lock and persists --
       * including the blank-name fallback, which is a rule about what a
       * device row must be readable as, not about typing. */
      if (sensor_set_label(sel_device(), g_kp.entry, g_kp.len) != 0)
         set_status("NAME NOT SAVED");
      g_kp.len = 0;
      nav_go(SCR_SENSOR);
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a correction to what the sensor reports.
 * Calibration (a fingerstick the sensor should agree with) and rescale (a true
 * value the app should scale towards) are the two, and they are one family
 * because both are entered in DISPLAY units and both must be converted before
 * anything downstream sees them. */
int kp_commit_correction(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (g_kp.mode == KP_CALIB) { /* entry is in display units */
      if (g_kp.len > 0) {
         /* Conversion and bound live in alarmlogic.c so `make check` can
          * fail on them; this branch only actuates. */
         /* mmol/L is entered as tenths (e.g. "78" = 7.8), so scale back
          * to mg/dL the same way the plot-max entry does. */
         int mgdl = cal_entry_mgdl(g_kp.entry, g_kp.len, sp.units);
         /* Out of range: refuse VISIBLY. Do NOT clamp -- silently
          * altering a calibration value the user typed is worse than not
          * accepting it. Previously the driver refused with only a log
          * line while the keypad closed and SCR_CAL still showed the
          * PREVIOUS result, so a rejected entry looked exactly like a
          * successful one. Staying on the keypad with the entry cleared
          * is the feedback: nothing was submitted, retype it. Easy to hit
          * in mmol/L (2.2 -> 39 mg/dL). */
         if (mgdl < 0) {
            LOGI("calibration %d mg/dL out of range 40..400, not "
                 "submitted",
                 mgdl);
            char lo2[8];
            char hi2[8];
            fmt_bound(lo2, sizeof lo2, CAL_MIN_MGDL);
            fmt_bound(hi2, sizeof hi2, CAL_MAX_MGDL);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "RANGE IS %s..%s", lo2,
                           hi2);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: the cleared entry IS the
                                 * feedback
                                 */
         }
         /* The single most consequential write in the app, so it happens
          * only here: a digit typed by the user, then an explicit OK. */
         /* Do NOT write yet -- stash the value and show a confirmation.
          * The actual (consequential) calibration write happens only on
          * the explicit CONFIRM (MA_CAL_ENTER). */
         g_kp.len         = 0;
         g_kp.cal_pending = mgdl;
         keypad_close();
         nav_go(SCR_CAL);
      }
   } else if (g_kp.mode == KP_RESCALE) { /* a true glucose value (display
                                   units), like calibration */
      if (g_kp.len > 0) {
         int mgdl = cal_entry_mgdl(g_kp.entry, g_kp.len, sp.units);
         if (mgdl < 0) {
            LOGI("rescale %d mg/dL out of range, not submitted", mgdl);
            char lo3[8];
            char hi3[8];
            fmt_bound(lo3, sizeof lo3, CAL_MIN_MGDL);
            fmt_bound(hi3, sizeof hi3, CAL_MAX_MGDL);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "RANGE IS %s..%s", lo3,
                           hi3);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: cleared entry is the
                                 * feedback
                                 */
         }
         g_kp.len     = 0;
         g_kp.rescale = mgdl; /* factor computed on CONFIRM */
         keypad_close();
         nav_go(SCR_RESCALE);
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: an alarm threshold. Four modes -- low, high, and
 * the two nudge levels -- and they are one family because of the ordering rule
 * between them: a low above a high is not a threshold, it is an alarm that can
 * never stop, so each commit has to re-validate the SET rather than its own
 * field. */
int kp_commit_thresholds(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (kp_is_thresh(g_kp.mode)) {
      /* ALARM LOW / HIGH and NUDGE LOW / HIGH: entry in
       * DISPLAY units. mg/dL is a plain integer; mmol/L is LITERAL mmol
       * with an optional '.' and one decimal digit ("5.5") -- its keypad
       * shows a dot key (keypad.h).
       * All four accept 0..AL_ENTRY_MAX: 0 parks LOW below any possible
       * reading and a past-the-scale HIGH above any, each threshold's
       * deliberate OFF switch. Refuse VISIBLY (stay on the keypad, entry
       * cleared) a malformed entry, an out-of-range value, or one that
       * would invert ITS OWN pair -- a silent clamp would move a
       * threshold the user never typed. Equal is allowed.
       *
       * The two pairs are checked against THEMSELVES only, never against
       * each other. A nudge inside the alarm band is pointless but
       * harmless (nudge_fire suppresses it under the alarm), and refusing
       * the entry would block the legitimate order of operations -- move
       * the nudge first, then the alarm -- for no safety gain. */
      if (g_kp.len > 0) {
         /* WHICH pair, and WHICH end -- asked of the mode rather than
          * derived from its number. `>= KP_NUDGE_LOW` and `% 2` were correct
          * and silent, and they made the enum's ORDER a fact two files
          * depended on without saying so. */
         int isnudge = kp_is_nudge(g_kp.mode);
         int islow   = kp_is_low(g_kp.mode);
         int ip      = 0;
         int fd      = 0;
         int dot     = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
         int bad     = 0;
         for (int i = 0; i < g_kp.len; i++) {
            char ch = g_kp.entry[i];
            if (ch == '.') {
               if (dot || !sp.units)
                  bad = 1; /* one dot, and only in mmol/L mode */
               else
                  dot = 1;
            } else if (dot == 0) {
               ip = (ip * 10) + (ch - '0');
            } else if (dot == 1) {
               fd  = ch - '0';
               dot = 2;
            } else {
               bad = 1; /* a second decimal digit: not representable */
            }
         }
         int mgdl = sp.units ? (((ip * 10) + fd) * 18) / 10 : ip;
         /* The RULES are alarm_set_threshold's (see there): it reads the
          * partner threshold and stores this one in ONE critical section, so
          * the pair it approved is the pair it writes. This branch's job is
          * only to say WHY when it refuses. */
         int why = bad ? -1 : alarm_set_threshold(isnudge, islow, mgdl);
         /* STORED BUT NOT WRITTEN is not a refusal: the value is live now,
          * and it is the OLD one again after the next launch. Say so and
          * close, rather than clearing the entry as though it had been
          * rejected -- retyping it would not help. */
         if (why == TH_NOT_SAVED) {
            set_status("THRESHOLD NOT SAVED");
            g_kp.len = 0;
            keypad_close();
            return COMMIT_DONE;
         }
         if (why != TH_OK) {
            LOGI("%s %s %d mg/dL refused (0..%d, low<=high)",
                 isnudge ? "nudge" : "alarm", islow ? "low" : "high", mgdl,
                 AL_ENTRY_MAX);
            /* THREE different refusals used to share one sentence.
             *
             * Typing 1500 was answered with "HIGH MUST BE >= LOW", which is
             * not why it was refused and sends the user to change the other
             * number. Say which rule was broken, and say it in the units the
             * keypad is accepting. */
            char bnd[8];
            fmt_bound(bnd, sizeof bnd, AL_ENTRY_MAX);
            if (bad)
               (void)snprintf(g_kp.err, sizeof g_kp.err,
                              sp.units ? "USE ONE DECIMAL, LIKE 5.5"
                                       : "DIGITS ONLY");
            else if (why == TH_TOO_BIG)
               (void)snprintf(g_kp.err, sizeof g_kp.err, "MAX IS %s", bnd);
            else if (why == TH_HIGH_ZERO)
               (void)snprintf(g_kp.err, sizeof g_kp.err, "HIGH CANNOT BE ZERO");
            else
               (void)snprintf(g_kp.err, sizeof g_kp.err,
                              islow ? "LOW MUST BE <= HIGH"
                                    : "HIGH MUST BE >= LOW");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* cleared entry is the feedback; retype it */
         }
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a plain quantity with a range. Plot maximum, the
 * server's pairing code, a port, a weight, a dose, a year. Nothing here
 * changes what the app DOES -- each one parses, bounds-checks, stores, and
 * returns to wherever the keypad was opened from. */
int kp_commit_number(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (g_kp.mode == KP_PLOT_MAX) { /* entry is in the display unit */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* TENTHS of mmol/L, matching how the row is DISPLAYED: the
          * renderer draws plot max through fmt_glu, which prints one decimal in
          * mmol mode (300 mg/dL shows as "16.7"). Treating the entry as
          * whole mmol made the shown value impossible to re-enter --
          * typing 167 gave 3006 mg/dL (silently clamped to 400) and
          * typing 16 gave 288, not 300. The calibration entry below
          * already scales this way; this is the one that disagreed with
          * its own display. */
         /* The clamp and the renderer's scale belong to the setting, not to
          * the keypad that types it -- settings_load applies the same two. */
         if (settings_set_plot_max(sp.units ? (v * 18) / 10 : v) != SETTINGS_OK)
            set_status("PLOT SCALE NOT SAVED");
         keypad_close();
         /* the notification plot shares this vertical scale; without a
          * refresh it keeps the old one until the next datapoint */
         notify_mark();
         notify_tick();
      }
   } else if (g_kp.mode == KP_SYNC_CODE) { /* the server's 6 digits */
      if (g_kp.len == 6) {
         char code[8];
         for (int i = 0; i < 6; i++)
            code[i] = g_kp.entry[i];
         code[6] = 0;
         /* Handed to Java's worker: pairing is four round trips and must
          * not run on the UI thread. The result arrives as a changed
          * PAIRED row, because the only thing the user can do about a
          * failure is ask the server for a fresh code. */
         /* THE ACCOUNT AND THE ENDPOINT AS ONE VALUE. A pairing aimed at a
          * server the user has just changed, carrying the account they had
          * before it, fails with nothing on screen to explain which half was
          * stale. */
         struct remote_config rc;
         remote_config_get(&rc);
         syncjni_pair_request(rc.email, code);
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_PORT) { /* 1..65535 */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0'); /* max 5 digits: no wrap */
         if (v < 1 || v > 65535) {
            LOGI("remote port %d out of range, not saved", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "PORT MUST BE 1..65535");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: cleared entry is the
                                 * feedback
                                 */
         }
         /* range checked just above, so a refusal here is the file */
         /* Same rule as the server name: the identity is dropped below, and
          * a port that did not persist must not cost the pairing. */
         if (settings_set_remote_port(v) != SETTINGS_OK) {
            set_status("PORT NOT SAVED");
            g_kp.len = 0;
            return COMMIT_STAY;
         }
         remote_forget_cursor(); /* possibly a different server */
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_FOOD_G) { /* whole grams, no decimal point */
      if (g_kp.len > 0) {
         long v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* REFUSED VISIBLY, NEVER CLAMPED. Silently altering a number the
          * user typed is the trap the calibration entry above spells out: the
          * value stored is then one they did not enter and cannot reproduce.
          * The bounds are food.h's, not restated here -- what this branch
          * knows is that a portion outside them is not a portion.
          *
          * The keypad has no '.' key in this mode (kp_info), so there is no
          * fractional case to reject: grams are whole, which is the
          * resolution the format stores. */
         if (v < FOOD_MIN_G || v > FOOD_MAX_G) {
            (void)snprintf(g_kp.err, sizeof g_kp.err, "GRAMS OUT OF RANGE");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         g_food.g = (int)v;
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_WEIGHT) { /* "162" or "162.4" */
      if (g_kp.len > 0) {
         /* THE DIGITS ARE THE WHOLE NUMBER, with an optional '.' and one
          * decimal -- exactly the alarm-threshold entry's shape.
          *
          * They used to be TENTHS, so "162" meant 16.2 lb: below the
          * minimum, refused, entry cleared, and the only way to enter 162
          * was to type "1620". Nobody would. An entry form has to accept
          * the number as it is spoken and as the row displays it. */
         int ip  = 0;
         int fd  = 0;
         int dot = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
         int bad = 0;
         for (int i = 0; i < g_kp.len; i++) {
            char ch = g_kp.entry[i];
            if (ch == '.') {
               if (dot)
                  bad = 1; /* one dot only */
               else
                  dot = 1;
            } else if (dot == 0) {
               ip = (ip * 10) + (ch - '0');
            } else if (dot == 1) {
               fd  = ch - '0';
               dot = 2;
            } else {
               bad = 1; /* a second decimal digit: not representable */
            }
         }
         int tenths = (ip * 10) + fd;
         /* Validate by CONVERTING: wt_from_tenths returns 0 outside the
          * stored range, so an impossible weight is refused VISIBLY
          * rather than silently clamped into the log. */
         if (bad || ip > 999 || wt_from_tenths(tenths, sp.wunits) <= 0) {
            LOGI("weight %d.%d %s refused (out of range)", tenths / 10,
                 tenths % 10, wt_unit_name(sp.wunits));
            (void)snprintf(g_kp.err, sizeof g_kp.err, "WEIGHT OUT OF RANGE");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay: the cleared entry is the feedback */
         }
         g_wt.f.tenths = tenths;
         g_kp.len      = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_INS_UNITS) { /* 1..99 */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         if (v < INS_UNITS_MIN || v > INS_UNITS_MAX) {
            LOGI("insulin %d units out of range, not saved", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "UNITS MUST BE %d..%d",
                           INS_UNITS_MIN, INS_UNITS_MAX);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay: cleared entry is the refusal */
         }
         g_ins.f.units = v;
         g_kp.len      = 0;
         keypad_close();
      }
   } else if (kp_is_year(g_kp.mode)) {
      if (g_kp.len == 4) {
         int v = 0;
         for (int i = 0; i < 4; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* WHICH INSTANT, from the MODE. This read the return screen --
          * `g_kp.ret == SCR_WEIGHT` -- which is a second, unstated
          * protocol: the keypad's return can be re-aimed (menu.c does it for
          * PLOT MAX), and a typed year would then have moved the other
          * form's timestamp. */
         long *tp = form_instant_of(g_kp.mode);
         if (!tp)
            return COMMIT_PASS; /* not a form field: nothing to move */
         /* a dose belongs to a human timescale; refuse typo years */
         if (v < 2000 || v > 2199) {
            LOGI("year %d refused", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err,
                           "YEAR MUST BE 2000..2199");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         /* Month, day and time of day are kept; Feb 29 out of a non-leap year
          * is clamped to the 28th (civil.h). A year change is the edit most
          * likely to cross a transition -- the same civil date half a year
          * away is a different offset -- so the recombination is the target
          * year's, never today's. */
         *tp      = civil_reaim(*tp, CIVIL_EDIT_YEAR, v, 0, form_zone, 0).t;
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a date or a time. Two modes and one family,
 * because they are the two halves of a single entry -- MMDD, then HHMM -- and
 * validating the second depends on the first having been accepted. */
int kp_commit_datetime(void)
{
   if (kp_is_date(g_kp.mode) || kp_is_time(g_kp.mode)) {
      if (g_kp.len == 4) {
         int a = ((g_kp.entry[0] - '0') * 10) + (g_kp.entry[1] - '0');
         int b = ((g_kp.entry[2] - '0') * 10) + (g_kp.entry[3] - '0');
         /* whichever form's field this MODE is -- see the year entry above */
         long *tp = form_instant_of(g_kp.mode);
         if (!tp)
            return COMMIT_PASS; /* not a form field: nothing to move */
         if (kp_is_date(g_kp.mode)) { /* MMDD, within the current year */
            /* THE YEAR THE INSTANT IS IN, read in the offset in force at that
             * instant, because February's length depends on it. Reading it in
             * today's offset can name the wrong DAY (and, on New Year's Eve,
             * the wrong year), which is the same defect one step earlier. */
            long yy = 0;
            long mm = 0;
            long dd = 0;
            civil_at(*tp, form_zone, 0, &yy, &mm, &dd);
            static const int mdl[12] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
            int leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
            int md   = (a >= 1 && a <= 12) ? mdl[a - 1] + (a == 2 && leap) : 0;
            if (a < 1 || a > 12 || b < 1 || b > md) {
               LOGI("date %02d%02d refused", a, b);
               (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A DATE (MMDD)");
               g_kp.len = 0;
               shell_ui_dirty();
               return COMMIT_STAY; /* invalid date: stay, entry cleared */
            }
            *tp = civil_reaim(*tp, CIVIL_EDIT_MONTHDAY, a, b, form_zone, 0).t;
         } else { /* HHMM: keep the civil date, set the time of day */
            if (a > 23 || b > 59) {
               LOGI("time %02d%02d refused", a, b);
               (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A TIME (HHMM)");
               g_kp.len = 0;
               shell_ui_dirty();
               return COMMIT_STAY; /* invalid time: stay, entry cleared */
            }
            /* A time typed into the skipped hour is moved forward rather than
             * refused, and the form redisplays what was stored -- 0230
             * becomes 0330 on screen. See civil.h for why that beats a
             * refusal the user cannot act on. */
            *tp = civil_reaim(*tp, CIVIL_EDIT_TIME, a, b, form_zone, 0).t;
         }
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

void forms_kp_open(enum keypad_mode mode, enum ui_screen ret)
{
   g_kp.mode = mode;
   g_kp.ret  = ret;
   forms_kp_clear();
}

void forms_kp_return_set(enum ui_screen ret)
{
   g_kp.ret = ret;
}

enum ui_screen forms_kp_return(void)
{
   return g_kp.ret;
}

void forms_kp_clear(void)
{
   g_kp.len    = 0;
   g_kp.err[0] = 0; /* opening, or starting again, is not a refusal */
}

void forms_kp_seed(const char *text)
{
   forms_kp_clear();
   for (int i = 0; text && text[i] && g_kp.len < (int)sizeof g_kp.entry - 1;
        i++)
      g_kp.entry[g_kp.len++] = text[i];
}

void forms_kp_type(char c)
{
   if (g_kp.len < (int)sizeof g_kp.entry - 1)
      g_kp.entry[g_kp.len++] = c;
}

int forms_kp_len(void)
{
   return g_kp.len;
}

void forms_kp_text(char *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return;
   for (; n < g_kp.len && n < cap - 1; n++)
      out[n] = g_kp.entry[n];
   out[n] = 0;
}

void forms_set_label_field(int field)
{
   g_kp.label_field = field;
}

void forms_set_markpick(int ins_type)
{
   g_kp.markpick_ins = ins_type;
}

enum keypad_mode forms_kp_mode(void)
{
   return g_kp.mode;
}

void forms_kp_del(void)
{
   g_kp.err[0] = 0; /* a correction retires the last refusal */
   if (g_kp.len > 0)
      g_kp.len--;
}

void forms_kp_err_clear(void)
{
   g_kp.err[0] = 0;
}

int forms_kp_has(char c)
{
   for (int i = 0; i < g_kp.len; i++)
      if (g_kp.entry[i] == c)
         return 1;
   return 0;
}

int forms_markpick(void)
{
   return g_kp.markpick_ins;
}

/* Put a saved WEIGHT draft back, as a NEW entry. See forms.h: `edit` is
 * forced to -1 and `orig` cleared, so a restored draft can never rewrite a
 * row -- the log behind it was reloaded from disk while the process was
 * gone, and the row this draft named may not be there any more. */
void forms_wt_restore(long t, int tenths)
{
   struct wt_rec none = {0, 0};
   g_wt.orig          = none;
   g_wt.f.edit        = -1;
   g_wt.f.t           = t;
   g_wt.f.tenths      = tenths;
}

/* The same for a saved INSULIN draft, and for the same reason. */
void forms_ins_restore(long t, int type, int units)
{
   struct ins_rec none = {0, 0, 0};
   g_ins.orig          = none;
   g_ins.f.edit        = -1;
   g_ins.f.t           = t;
   g_ins.f.type        = type;
   g_ins.f.units       = units;
}

void forms_kp_mode_set(enum keypad_mode mode)
{
   g_kp.mode = mode;
}

void forms_set_rescale_entry(int tenths)
{
   g_kp.rescale = tenths;
}

int forms_rescale_entry(void)
{
   return g_kp.rescale;
}

void forms_set_cal_pending(int mgdl)
{
   g_kp.cal_pending = mgdl;
}

int forms_cal_pending(void)
{
   return g_kp.cal_pending;
}

void forms_set_wt_scrub(int idx)
{
   g_view.wt_scrub = idx;
}

static int g_scrub = -1;

void forms_set_scrub(int idx)
{
   g_scrub = idx;
}

int forms_scrub(void)
{
   return g_scrub;
}

/* THE WHOLE FORM STATE, IN ONE COPY.
 *
 * This was a stub -- `(void)out;` -- so every frame read an UNINITIALISED
 * stack struct: kp_mode was whatever was on the stack, and the keypad
 * rendered "BAD KP MODE 875648851" over a field of garbage characters. The
 * red title is what caught it, which is exactly why the unknown-mode branch
 * refuses to draw a plausible screen (see uikeypad.c).
 *
 * Fill EVERY member. A field left out here is the same bug in slower motion:
 * it reads as zero on one frame and as the caller's leftover stack on the
 * next, and zero is a real mode, a real screen and a real insulin type. */
/* Open a fresh LOG FOOD form.
 *
 * NO TYPE IS CHOSEN, deliberately, and it is why the FOOD button opens the
 * PICKER rather than this form: an entry needs a food before it needs a
 * portion, and a form that opened on the last food used would log the wrong
 * one for anybody who forgot to look. The picker is the first question.
 *
 * The instant defaults to now, like the other two forms, because the common
 * case is logging something as it happens. */
void forms_food_open(long now)
{
   g_food.t    = now;
   g_food.type = FOOD_TYPE_NONE;
   g_food.g    = 0;
   g_food.edit = -1;
}

void forms_food_type_set(int type_id)
{
   g_food.type = type_id;
}

void forms_foodtype_page_set(int page)
{
   g_view.foodtype_page = page < 0 ? 0 : page;
}

int forms_foodtype_page(void)
{
   return g_view.foodtype_page;
}

void forms_view_get(struct forms_view *out)
{
   if (!out)
      return;
   /* the keypad */
   out->kp_mode  = g_kp.mode;
   out->entrylen = g_kp.len;
   str_snapshot(out->entry, sizeof out->entry, g_kp.entry);
   str_snapshot(out->kp_err, sizeof out->kp_err, g_kp.err);
   /* the insulin form and its log */
   out->ins_t        = g_ins.f.t;
   out->ins_type     = g_ins.f.type;
   out->ins_units    = g_ins.f.units;
   out->ins_edit     = g_ins.f.edit;
   out->inslog_page  = g_view.ins_page;
   out->markpick_ins = g_kp.markpick_ins;
   /* the weight form, its log and its plot */
   out->wt_t       = g_wt.f.t;
   out->wt_tenths  = g_wt.f.tenths;
   out->wt_edit    = g_wt.f.edit;
   out->wt_orig    = g_wt.orig;
   out->wtlog_page = g_view.wt_page;
   out->wt_tab     = g_view.wt_tab;
   out->wt_scrub   = g_view.wt_scrub;
   /* the food form and the picker's page */
   out->food_t        = g_food.t;
   out->food_type     = g_food.type;
   out->food_g        = g_food.g;
   out->food_edit     = g_food.edit;
   out->foodtype_page = g_view.foodtype_page;
   out->foodlog_page  = g_view.foodlog_page;
   out->scrub      = g_scrub;
   /* the odds and ends */
   out->label_field   = g_kp.label_field;
   out->rescale_entry = g_kp.rescale;
   out->cal_pending   = g_kp.cal_pending;
}
