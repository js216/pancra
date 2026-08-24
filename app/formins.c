// SPDX-License-Identifier: GPL-3.0
// formins.c --- LOG/EDIT INSULIN: the draft and the dose table
// Copyright 2026 Jakob Kastelic

/* ONE WORKFLOW, ONE FILE. See app/formsint.h for why the typed-entry code is
 * split this way and what each controller owes the keypad; forms.h is the
 * interface the rest of the app uses. Everything below is the insulin workflow
 * and nothing else -- its draft, its paging, its taps, and its part of the
 * frame. */
#include "clock.h"
#include "forms.h"
#include "formsint.h"
#include "insrow.h" /* INS_*: what a dose row can say */
#include "insulin.h"
#include "keypad.h"
#include "log.h"
#include "nav.h"
#include "shell.h"
#include "status.h"
#include "uifmt.h" /* UI_DAY_TABS: the plot spans */
#include "uiact.h"
#include "uimodel.h"

/* Which page of the DOSE table is showing. See formwt.c on why paging lives
 * with the workflow whose screen it is. */
static int g_inslog_page;
/* And which span its units-per-day plot covers: an index into ui_day_days. */
static int g_inslog_tab;

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
   d->f.milli         = row.milli;
}

static void ins_draft_done(struct ins_draft *d)
{
   struct ins_rec none = {0, 0, 0};
   d->f.edit           = -1;
   d->orig             = none;
}

int form_ins_action(int action, int ix)
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
      g_inslog_page = 0;
      nav_go(SCR_INSLOG);
   } else if (action == MA_INSLOG_PAGE) {
      g_inslog_page = ix;
   } else if (action == MA_INSTAB) {
      if (ix >= 0 && ix < UI_DAY_TABS)
         g_inslog_tab = ix;
      /* Dropped with the span, for the reason the exercise log's tab gives:
       * the index counts days from the old window's left edge. */
      forms_set_log_scrub(-1);
   } else if (action == MA_INS_EDIT) {
      /* Tapping a form value opens the keypad for EXACT entry: units (2
       * digits), date (MMDD), time (HHMM) or year (YYYY). The keypad's OK
       * validates and writes back; X returns unchanged. */
      enum keypad_mode im = kp_ins_field(ix); /* KP_NONE opens nothing */
      if (im == KP_NONE)
         return 0;
      nav_go(SCR_KEYPAD);
      forms_kp_open(im, SCR_INSULIN);
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
                                g_ins.f.milli, itz);
         else
            rc = insulin_append(g_ins.f.t, g_ins.f.type, g_ins.f.milli, itz);
         enum draft_fate fate = rc == 0 ? DRAFT_DONE : DRAFT_RETRY;
         if (fate == DRAFT_DONE) {
            char uu[16];
            (void)ins_units_str(g_ins.f.milli, uu, sizeof uu);
            LOGI("insulin %s: %s U %s at %ld",
                 g_ins.f.edit >= 0 ? "edited" : "logged", uu,
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
         /* A FAILED DELETE IS THE ONE THAT DUPLICATES THE DOSE. Reporting
          * the failure, dropping back to the still-populated EDIT form and
          * clearing g_ins.f.edit on the way leaves the form no longer knowing
          * it is amending a row, so the next CONFIRM (the obvious thing to
          * try) APPENDS a second copy of a dose that was never deleted. Two
          * identical entries in the record of what was
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
      forms_set_markpick(ix); /* INS_SLOW / INS_FAST */
      nav_go(SCR_MARKPICK);
   } else if (action == MA_INSMARK_BACK) {
      forms_set_markpick(-1);
      nav_go(SCR_DISPLAY); /* the row lives on the DISPLAY menu */
   } else {
      return 0; /* not an insulin action */
   }
   return 1;
}

/* The same for a saved INSULIN draft, and for the same reason. */
/* ---- WHAT THE KEYPAD AND THE FRAME ASK OF THIS WORKFLOW -------------- */

long *form_ins_instant(void)
{
   return &g_ins.f.t;
}

/* WHOLE UNITS. The keypad has already bounded it (1..99); this is the
 * assignment that follows. */
void form_ins_set_units(int milli)
{
   g_ins.f.milli = milli;
}

void form_ins_view(struct forms_view *out)
{
   out->ins_t       = g_ins.f.t;
   out->ins_type    = g_ins.f.type;
   out->ins_milli   = g_ins.f.milli;
   out->ins_edit    = g_ins.f.edit;
   out->inslog_page = g_inslog_page;
   out->inslog_tab  = g_inslog_tab;
}
