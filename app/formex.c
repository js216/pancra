// SPDX-License-Identifier: GPL-3.0
// formex.c --- EDIT EXERCISE: the draft and the exercise log
// Copyright 2026 Jakob Kastelic

/* ONE WORKFLOW, ONE FILE. See app/formsint.h for why the typed-entry code is
 * split this way and what each controller owes the keypad; forms.h is the
 * interface the rest of the app uses. Everything below is the exercise workflow
 * and nothing else -- its draft, its paging, its taps, and its part of the
 * frame. */
#include "clock.h" /* realtime_s: the instant an edit is judged against */
#include "exercise.h"
#include "forms.h"
#include "formsint.h"
#include "keypad.h"
#include "nav.h"
#include "status.h"
#include "uiact.h"
#include "uifmt.h" /* UI_DAY_TABS: the plot spans */
#include "uimodel.h"
#include "util.h" /* str_snapshot */

/* Which page of the EXERCISE LOG table is showing. */
static int g_exlog_page;
/* And which span its plot covers: an index into ui_exday_days. Defaults to
 * the first, the 24 H view, where the step count is drawn five minutes at a
 * time against the bands of exercise it was taken during. */
static int g_exlog_tab;

/* EDIT EXERCISE. The one draft in this file with NO "new entry" state, because
 * there is no way to create an exercise record by typing -- the button and its
 * settling rule are the only writer (uiex.c says why at length). So `edit` is
 * either an index being corrected or -1 meaning "this form is not open", and
 * a CONFIRM with -1 rewrites nothing rather than appending something. */
struct ex_draft {
   long t;
   int level; /* EX_MIN_LEVEL..EX_MAX_LEVEL */
   /* HOW LONG IT LASTED, in SECONDS, and 0 means "not known" -- the same
    * column exercise.h describes, held in the same unit so nothing here has
    * to convert twice. The form types it in minutes. */
   long dur;
   int edit; /* index being edited, < 0 = not editing anything */
   /* WHY THE LAST CONFIRM WAS REFUSED, shown on the form itself. Empty when
    * there is nothing to say. It belongs to THIS draft rather than to the
    * global status line, for the reason the keypad's own error does: the
    * status line is drawn on the main screen, so a refusal announced there
    * while the form is open is a refusal nobody sees -- and one left standing
    * would describe an edit two screens ago. */
   char err[26];
   /* THE ROW AS IT WAS ON DISK, which is what exercise_update matches on --
    * the same reason the food draft keeps one. */
   struct ex_rec orig;
};
static struct ex_draft g_ex = {
    0, EX_MIN_LEVEL, 0, -1, {0}, {0, 0, 0, 0}
};

/* ---- THE EXERCISE LOG AND ITS CORRECTION FORM ----------------------------
 *
 * Modelled on form_food_action, minus everything to do with creating a record:
 * there is no MA_EX_OPEN because there is no way to log exercise by typing.
 * The button on the ADD menu is the only writer; this is only ever fixing
 * what it wrote. */
int form_ex_action(int action, int ix)
{
   if (action == MA_EXLOG_OPEN) {
      /* ALWAYS PAGE ONE. Which page was last looked at is not something a
       * user carries between visits -- they come back for the newest entries,
       * which is where page one is -- and a log reopening three pages deep
       * reads as the app having lost the recent ones. */
      g_exlog_page = 0;
      nav_go(SCR_EXLOG);
   } else if (action == MA_EXLOG_BACK) {
      nav_back();
   } else if (action == MA_EXLOG_PAGE) {
      /* THE PAGE COMES FROM THE HIT. pager_row works out where each of its
       * four buttons goes -- it is what knows the page count -- so there is
       * no stepping or clamping to do here, and no way for the stored page
       * to run past the end the way an unbounded ++ used to. */
      g_exlog_page = ix;
   } else if (action == MA_STEPS_TOGGLE) {
      struct prefs sp;
      settings_get(&sp);
      if (settings_set_steps_on(!sp.steps_on) != SETTINGS_OK)
         set_status("NOT SAVED");
      /* ASK ONLY WHEN SWITCHING ON, and only then: the counter needs
       * ACTIVITY_RECOGNITION from API 29, and a glucose app demanding activity
       * data at launch has no visible reason to. Requesting a permission
       * already held is a no-op, so no state is kept about whether we asked.
       * The switch itself is honest either way -- the row says WAITING while
       * the permission is refused, which is what is actually happening. */
      else if (!sp.steps_on)
         steps_request_perm();
   } else if (action == MA_EXTAB) {
      if (ix >= 0 && ix < UI_EXDAY_TABS)
         g_exlog_tab = ix;
      /* THE PICKED BAR IS DROPPED WITH THE SPAN. Its index counts days from
       * the left edge of the old window, so keeping it would move the readout
       * to a different date the moment the span changed. */
      forms_set_log_scrub(-1);
   } else if (action == MA_EXLOG_EDIT) {
      if (ix >= 0 && ix < ex_count()) {
         forms_ex_edit(ix);
         nav_go(SCR_EXEDIT);
      }
   } else if (action == MA_EX_EDIT) {
      /* ROW 0 IS THE LEVEL, and it CYCLES rather than opening a keypad: the
       * domain is three values, which is a thing to tap through, not a number
       * to type -- and a keypad would collect 0, 4 and 97, every one of which
       * exercise_update refuses. The wrap is 3 -> 1, never through 0: 0 is
       * the button's resting position, not a level, and this form is editing
       * a record that exists. */
      if (ix == 0) {
         g_ex.level =
             (g_ex.level >= EX_MAX_LEVEL) ? EX_MIN_LEVEL : g_ex.level + 1;
      } else {
         enum keypad_mode mode = kp_ex_field(ix - 1);
         /* THE DURATION OF A RUNNING SESSION IS NOT A FIELD. It is `now`
          * minus a start that is still moving, and the end press writes it --
          * so a number typed here is overwritten seconds later, and offering
          * the keypad promises an edit that cannot survive. Say why rather
          * than opening a pad whose CONFIRM would be undone. */
         if (mode == KP_EX_DUR && exercise_row_running(&g_ex.orig)) {
            set_status("EXERCISE STILL ACTIVE");
            mode = KP_NONE;
         }
         if (mode != KP_NONE) {
            nav_go(SCR_KEYPAD);
            forms_kp_open(mode, SCR_EXEDIT);
         }
      }
   } else if (action == MA_EX_DELETE) {
      if (g_ex.edit >= 0)
         nav_go(SCR_EXDEL); /* confirm first; this tap deletes nothing */
   } else if (action == MA_EXDEL_NO) {
      nav_go(SCR_EXEDIT);
   } else if (action == MA_EXDEL_YES) {
      /* THE TARGET SURVIVES A FAILED DELETE, for the reason form_food_action
       * spells out: g_ex.orig is the only thing a second YES can act on. */
      if (exercise_delete(&g_ex.orig) != 0) {
         set_status("EXERCISE NOT DELETED");
         nav_go(SCR_EXEDIT);
      } else {
         /* The whole draft, for the reason form_food_action's delete spells
          * out: a cleared flag beside a loaded row is a CONFIRM waiting to act
          * on something that is gone. */
         struct ex_rec none = {0, 0, 0, 0};
         g_ex.orig          = none;
         g_ex.t             = 0;
         g_ex.level         = EX_MIN_LEVEL;
         g_ex.dur           = 0;
         g_ex.edit          = -1;
         /* TWICE: the confirmation AND the form it was opened from. Both are
          * about a row that no longer exists, so popping one leaves the user
          * looking at the fields of a deleted entry -- and its CONFIRM would
          * then fail against a key that matches nothing. Two pops, not a
          * nav_go at SCR_EXLOG: the target is still derived from the path
          * rather than named here (nav.h), which is the rule that keeps
          * getting broken in this file. */
         nav_back();
         nav_back();
      }
   } else if (action == MA_EX_CONFIRM) {
      /* A CONFIRM WITH NOTHING OPEN WRITES NOTHING. There is no append path
       * here to fall back on, so an edit index of -1 is simply not a state
       * this button can act in. */
      if (g_ex.edit < 0) {
         str_snapshot(g_ex.err, sizeof g_ex.err, "NOTHING TO CHANGE");
      } else {
         /* NAMED, NOT JUST REFUSED. "Not changed" over a form whose values
          * look fine is a dead end; which rule was broken is what tells the
          * user what to move. */
         const char *why = 0;
         switch (exercise_update(&g_ex.orig, g_ex.t, g_ex.level, g_ex.dur,
                                 form_zone(0, g_ex.t), realtime_s())) {
            case EX_UPD_OK: break;
            case EX_UPD_FUTURE: why = "NOT IN THE FUTURE"; break;
            case EX_UPD_OVERLAP: why = "OVERLAPS ANOTHER"; break;
            case EX_UPD_REOPEN: why = "ONLY THE LIVE ONE"; break;
            case EX_UPD_RANGE:
            case EX_UPD_FAILED:
            default: why = "NOT CHANGED"; break;
         }
         if (why) {
            /* THE DRAFT AND THE SCREEN BOTH SURVIVE, so a retry does not mean
             * re-entering everything -- items 136-138's rule. */
            str_snapshot(g_ex.err, sizeof g_ex.err, why);
         } else {
            g_ex.edit   = -1;
            g_ex.err[0] = 0;
            nav_back();
         }
      }
   } else if (action == MA_EX_DISCARD) {
      g_ex.edit = -1;
      nav_back();
   } else {
      return 0;
   }
   return 1;
}

void forms_ex_edit(int i)
{
   struct ex_rec row = ex_at(i);
   if (row.t <= 0)
      return; /* out of range: ex_at zeroes, and a zeroed row is not one */
   g_ex.orig   = row;
   g_ex.edit   = i;
   g_ex.err[0] = 0; /* a refusal belongs to the edit that caused it */
   g_ex.t     = row.t;
   g_ex.level = row.level;
   g_ex.dur   = row.dur;
}

/* ---- WHAT THE FRAME ASKS OF THIS WORKFLOW ----------------------------
 *
 * The keypad edits this form's INSTANT and its DURATION. It cannot create a
 * record and it cannot set the level: the button and its settling rule are the
 * only writer of a row (uiex.c says why at length), and the level is three
 * values to tap through rather than a number to type. */

long *form_ex_instant(void)
{
   return &g_ex.t;
}

/* SECONDS, which is the unit the log stores; the keypad collects minutes and
 * kp_commit_number does the one multiplication. */
long *form_ex_duration(void)
{
   return &g_ex.dur;
}

void form_ex_view(struct forms_view *out)
{
   out->ex_t       = g_ex.t;
   out->ex_level   = g_ex.level;
   out->ex_dur     = g_ex.dur;
   out->ex_edit    = g_ex.edit;
   out->ex_running = exercise_row_running(&g_ex.orig);
   out->ex_err     = g_ex.err;
   out->ex_orig    = g_ex.orig;
   out->exlog_page = g_exlog_page;
   out->exlog_tab   = g_exlog_tab;
}
