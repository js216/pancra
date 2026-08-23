// SPDX-License-Identifier: GPL-3.0
// formwt.c --- LOG/EDIT WEIGHT: the draft, the log and its plot
// Copyright 2026 Jakob Kastelic

/* ONE WORKFLOW, ONE FILE. See app/formsint.h for why the typed-entry code is
 * split this way and what each controller owes the keypad; forms.h is the
 * interface the rest of the app uses. Everything below is the weight workflow
 * and nothing else -- its draft, its paging, its taps, and its part of the
 * frame. */
#include "clock.h"
#include "forms.h"
#include "formsint.h"
#include "keypad.h"
#include "log.h"
#include "nav.h"
#include "settings.h"
#include "shell.h"
#include "status.h"
#include "uiact.h"
#include "uimodel.h"
#include "weight.h"

/* THE WEIGHT LOG'S OWN VIEW STATE. Paging and scrubbing are typed-entry
 * state in the same sense the draft is: the user is navigating WITHIN a
 * screen, not between screens -- and each of these belongs to exactly one
 * screen, which is why they live with that screen's controller rather than
 * in a struct four workflows can reach. */
static int g_wtlog_page;    /* which page of the WEIGHT LOG table is showing */
static int g_wt_tab;        /* which span of the weight plot */
static int g_wt_scrub = -1; /* -1 = not scrubbing */

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

/* WEIGHT actions, split out like form_ins_action so menu_action stays small.
 * Returns 1 when `action` was one of ours. */
int form_wt_action(int action, int ix)
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
       * way out work. OK and X both land on the keypad's return screen,
       * and nav_go
       * RETURNS to a screen already on the path rather than pushing a second
       * copy -- so the keypad pops and the form's own CANCEL goes back to
       * whatever opened the flow. Skipping the push instead put a screen
       * BELOW the keypad that was not on the path, and the two exits chased
       * each other: LOG WEIGHT returned to WEIGHT, WEIGHT returned to LOG
       * WEIGHT, and nothing reached the main screen. The user has to be able
       * to leave. It is not rendered on the way in -- the keypad opens on
       * top of it in the same tap. */
      nav_go(SCR_WEIGHT);
      nav_go(SCR_KEYPAD);
      forms_kp_open(KP_WEIGHT, SCR_WEIGHT);
   } else if (action == MA_WTLOG_EDIT) {
      /* A row in the table opens that entry in the EDIT WEIGHT form. Keep a
       * COPY as the rewrite's match key -- see g_wt.orig. */
      int i = ix;
      if (i >= 0 && i < wt_count()) {
         wt_draft_edit(&g_wt, i, &sp);
         nav_go(SCR_WEIGHT);
      }
   } else if (action == MA_WTTAB) {
      g_wt_tab   = ix;
      g_wt_scrub = -1; /* the picked point may not be in the new span */
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
      g_wtlog_page = 0;
      nav_go(SCR_WTLOG);
   } else if (action == MA_WTLOG_PREV) {
      if (g_wtlog_page > 0)
         g_wtlog_page--;
   } else if (action == MA_WTLOG_NEXT) {
      g_wtlog_page++; /* the renderer clamps to the last page */
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
      forms_kp_open(wm, SCR_WEIGHT);
   } else if (action == MA_WT_CONFIRM) {
      /* The one write, on the explicit CONFIRM only (the calibration rule).
       */
      if (cur_screen() == SCR_WEIGHT) {
         long g = wt_from_tenths(g_wt.f.tenths, sp.wunits);
         int rc = -1;
         int ed = (g_wt.f.edit >= 0);
         /* THE OFFSET AT THE INSTANT BEING WRITTEN, not the offset now.
          * Stamped with today's offset, a weigh-in backdated across a DST
          * boundary carries the wrong tz column -- the one field that can
          * reveal an hour error in the recombination above. */
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

void forms_set_wt_scrub(int idx)
{
   g_wt_scrub = idx;
}

/* ---- WHAT THE KEYPAD AND THE FRAME ASK OF THIS WORKFLOW ---------------
 *
 * Three narrow entry points, and they are the whole of what the rest of the
 * typed-entry code may know about a weight (see app/formsint.h). */

long *form_wt_instant(void)
{
   return &g_wt.f.t;
}

/* TENTHS OF THE DISPLAY UNIT, which is what the field holds and what the
 * keypad round-trips: the conversion to grams happens once, on CONFIRM. The
 * keypad has already decided this is a number of the right shape. */
void form_wt_set_tenths(int tenths)
{
   g_wt.f.tenths = tenths;
}

void form_wt_view(struct forms_view *out)
{
   out->wt_t       = g_wt.f.t;
   out->wt_tenths  = g_wt.f.tenths;
   out->wt_edit    = g_wt.f.edit;
   out->wt_orig    = g_wt.orig;
   out->wtlog_page = g_wtlog_page;
   out->wt_tab     = g_wt_tab;
   out->wt_scrub   = g_wt_scrub;
}
