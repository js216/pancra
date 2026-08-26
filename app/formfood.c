// SPDX-License-Identifier: GPL-3.0
// formfood.c --- LOG/EDIT FOOD: the draft, the type picker and the log
// Copyright 2026 Jakob Kastelic

/* ONE WORKFLOW, ONE FILE. See app/formsint.h for why the typed-entry code is
 * split this way and what each controller owes the keypad; forms.h is the
 * interface the rest of the app uses. Everything below is the food workflow and
 * nothing else -- its draft, its paging, its taps, and its part of the frame.
 */
#include "clock.h"
#include "exercise.h" /* the button this form's CONFIRM can end */
#include "food.h"
#include "forms.h"
#include "formsint.h"
#include "keypad.h"
#include "nav.h"
#include "status.h"
#include "uiact.h"
#include "uimodel.h"

/* The two pages this workflow has: the TYPE PICKER's and the FOOD LOG's. */
static int g_foodtype_page;
static int g_foodlog_page;

struct food_draft {
   long t;
   int type; /* a food_type id; FOOD_TYPE_NONE = nothing chosen yet */
   int g;
   /* DID A PERSON TYPE THAT NUMBER, or did this form suggest it?
    *
    * Picking a food seeds the portion with the one that food was last logged
    * with, which is what makes the common entry two taps rather than two taps
    * and a number. But a suggestion must never overwrite an answer: with only
    * `g` to look at, a form holding 60 cannot tell "the user typed 60" from
    * "we suggested 60 a moment ago", and changing the food would either
    * clobber a typed portion or keep the previous food's suggestion. So the
    * keypad sets this and the picker reads it. */
   int g_typed;
   int edit; /* index being edited, < 0 for a new entry */
   /* THE ROW AS IT WAS ON DISK, which is what food_update matches on. An
    * index into the tail would go stale the moment the log reloads -- and
    * rewriting by position is how an edit lands on somebody else's meal. */
   struct food_rec orig;
};
static struct food_draft g_food = {
    0, FOOD_TYPE_NONE, 0, 0, -1, {0, 0, 0}
};

/* FOOD actions, split out like form_wt_action so menu_action stays small.
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
 * the reason form_wt_action pushes SCR_WEIGHT under the keypad: nav_go RETURNS
 * to a screen already on the path rather than pushing a second copy, so picking
 * a food pops back to the form and the form's own exit goes to whatever opened
 * the flow -- the ADD menu or the main screen -- without either of them being
 * named here. Skipping the push instead leaves the form off the path, and then
 * the two exits chase each other and nothing reaches the main screen. That
 * failure has happened in this file before; the comment in form_wt_action is
 * the record of it. */
int form_food_action(int action, int ix)
{
   if (action == MA_FOOD_OPEN) {
      forms_food_open(realtime_s());
      g_foodtype_page = 0; /* always page one: see the food log's note */
      /* STRAIGHT TO THE FORM, CARRYING THE LAST MEAL.
       *
       * People eat the same things in the same amounts, so the entry that was
       * logged last is the best guess at the one being logged now -- and the
       * button says LOG FOOD, not CHOOSE FOOD. Opening the picker first made
       * every repeat meal a trip through a list to find the row the form
       * would have arrived at by itself. The type row on the form still opens
       * the picker, so choosing something else is one tap, and it is the tap
       * of somebody who actually wants to choose.
       *
       * forms_food_type_set seeds the PORTION from the last time that food
       * was eaten, so naming the type fills the grams too -- one call, and
       * the form opens complete.
       *
       * WITH NOTHING LOGGED YET there is no last meal to offer, so the picker
       * is pushed on top exactly as before. The form must be BELOW it on the
       * path or the picker has nothing to return to (see above). */
      const int nf              = food_count();
      const struct food_rec lst = (nf > 0) ? food_at(nf - 1)
                                           : (struct food_rec){0, 0, 0};
      nav_go(SCR_FOOD);
      if (lst.t > 0 && lst.type != FOOD_TYPE_NONE)
         forms_food_type_set(lst.type);
      else
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
      forms_kp_clear();
      nav_go(SCR_LABEL);
      forms_set_label_field(LABEL_FOOD);
      forms_kp_return_set(SCR_FOODTYPE);
   } else if (action == MA_FOODLOG_EDIT) {
      /* A row opens that entry in the form. The picker and the keypad reach
       * it exactly as they do for a new entry; what differs is that CONFIRM
       * rewrites the row rather than appending one. */
      if (ix >= 0 && ix < food_count()) {
         forms_food_edit(ix);
         nav_go(SCR_FOOD);
      }
   } else if (action == MA_FOOD_DELETE) {
      if (g_food.edit >= 0)
         nav_go(SCR_FOODDEL); /* confirm first; this tap deletes nothing */
   } else if (action == MA_FOODDEL_NO) {
      nav_go(SCR_FOOD);
   } else if (action == MA_FOODDEL_YES) {
      /* THE TARGET SURVIVES A FAILED DELETE. g_food.orig is the exact entry
       * food_delete matches on, and it is the only thing a second YES can
       * act upon -- clearing it here would leave the user on a populated
       * form whose next CONFIRM appends a duplicate rather than retrying.
       * That is items 136-138's rule, in a form written after them. */
      if (food_delete(&g_food.orig) != 0) {
         set_status("FOOD NOT DELETED");
         nav_go(SCR_FOOD);
      } else {
         /* THE WHOLE DRAFT GOES, not just its `edit` flag.
          *
          * Clearing `edit` alone leaves the type, the portion and the instant
          * of the deleted meal sitting in the draft, and a CONFIRM reaching
          * this form afterwards appends exactly the row that was deleted --
          * measured, from the table this now returns to. Closing a workflow
          * states the whole state it leaves behind, for the same reason
          * opening one does (see the head of this file). */
         forms_food_open(realtime_s());
         /* TWICE, and the second pop is what closes a real trap.
          *
          * SCR_FOODDEL is only reachable through the LOG -> form(edit) ->
          * confirm route, because DELETE is only drawn when the form is
          * editing. Popping once landed back on that form, which -- with
          * `edit` cleared -- is a NEW entry form still holding the deleted
          * meal's type, portion and instant. Its CONFIRM would then append
          * the very row the user had just deleted, and nothing on the screen
          * said so. Two pops go back to the table, which is where the deleted
          * row was. */
         nav_back();
         nav_back();
      }
   } else if (action == MA_FOODLOG_OPEN) {
      g_foodlog_page = 0; /* always page one: see the exercise log's note */
      nav_go(SCR_FOODLOG);
   } else if (action == MA_FOODLOG_BACK) {
      nav_back();
   } else if (action == MA_FOODLOG_PAGE) {
      g_foodlog_page = ix;
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
         g_foodtype_page = 0;
         nav_go(SCR_FOODTYPE);
      } else {
         enum keypad_mode mode = kp_food_field(ix - 1);
         if (mode != KP_NONE) {
            nav_go(SCR_KEYPAD);
            forms_kp_open(mode, SCR_FOOD);
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
      } else if (g_food.edit >= 0
                     ? food_update(&g_food.orig, g_food.t, g_food.type,
                                   g_food.g, form_zone(0, g_food.t)) != 0
                     : food_append(g_food.t, g_food.type, g_food.g,
                                   form_zone(0, g_food.t)) != 0) {
         /* PERSISTENCE FAILED, SO THE FORM STAYS. Navigating away here would
          * discard a draft whose write did not happen -- the failure items
          * 136-138 are about, in a form written after them. */
         set_status("FOOD NOT SAVED");
      } else {
         nav_back();
      }
   } else if (action == MA_EXERCISE) {
      /* ONE PRESS, NO SCREEN. Choosing a level does nothing here: what makes
       * it a record is ten seconds of not being pressed again, and that is
       * decided by the tick, not by this tap. ENDING a session is the
       * exception and happens at once -- the press writes the length into the
       * row that opened it, which is why the wall clock is passed too. */
      /* AND A FAILED END IS SAID OUT LOUD. The press that stops a session
       * rewrites the log; if that write fails the session is still running --
       * exercise.c puts the level back on the button precisely so the next
       * press retries it -- and a silent return would leave the user looking
       * at a lit button they have just pressed to turn off. */
      if (exercise_button_press(realtime_s(), mono_s()) == EX_PRESS_FAILED)
         set_status("EXERCISE END NOT SAVED");
   } else if (action == MA_FOOD_DISCARD) {
      /* Leave the entry form without logging. The draft is left alone
       * deliberately: nothing has been written, and the next MA_FOOD_OPEN
       * calls forms_food_open, which is the one place that decides what a
       * fresh form starts from. Clearing it here as well would be a second
       * opinion about that, in the function that knows least about it. */
      nav_back();
   } else if (action == MA_FOODPAGE) {
      g_foodtype_page = ix;
   } else {
      return 0;
   }
   return 1;
}

/* THE WHOLE FORM STATE, IN ONE COPY.
 *
 * As a stub -- `(void)out;` -- every frame reads an UNINITIALISED stack
 * struct: kp_mode is whatever was on the stack, and the keypad renders "BAD
 * KP MODE 875648851" over a field of garbage characters. The red title is
 * what catches that, which is exactly why the unknown-mode branch
 * refuses to draw a plausible screen (see uikeypad.c).
 *
 * Fill EVERY member. A field left out here is the same bug in slower motion:
 * it reads as zero on one frame and as the caller's leftover stack on the
 * next, and zero is a real mode, a real screen and a real insulin type. */
/* Open an existing entry for editing. Keeps a COPY as the rewrite's match key
 * -- see struct food_draft. */
void forms_food_open(long t)
{
   g_food = (struct food_draft){
       t, FOOD_TYPE_NONE, 0, 0, -1, {0, 0, 0}
   };
}

void forms_food_edit(int i)
{
   struct food_rec row = food_at(i);
   if (row.t <= 0)
      return; /* out of range: food_at zeroes, and a zeroed row is not one */
   g_food.orig = row;
   g_food.edit = i;
   g_food.t    = row.t;
   g_food.type = row.type;
   g_food.g    = (int)row.g;
   /* THE ROW'S OWN PORTION IS AN ANSWER, not a suggestion: somebody typed it
    * when they logged the meal. Changing the food on an edit therefore leaves
    * it alone, exactly as it does after the user types one. */
   g_food.g_typed = 1;
}

void forms_food_type_set(int type_id)
{
   g_food.type = type_id;
   /* SEEDED FROM THE LAST TIME THIS FOOD WAS EATEN. A person eats the same
    * things in the same amounts, and a form that starts at zero asks them to
    * retype a number they have typed before, every time.
    *
    * ONLY OVER A SUGGESTION, never over an answer. `g_typed` is what tells
    * them apart -- see the field. So: picking a food fills the portion in;
    * picking a DIFFERENT food before typing replaces it with that food's own
    * usual amount; and once a number has been typed (or the form was opened on
    * an existing row) the pick changes the food and leaves the portion alone.
    *
    * A food never logged before answers 0, which is what the form already
    * showed and is not a portion FOOD_MIN_G would accept -- so nothing is
    * suggested and the user types one, as before. */
   if (!g_food.g_typed)
      g_food.g = (int)food_last_grams(type_id);
}

/* ---- WHAT THE KEYPAD AND THE FRAME ASK OF THIS WORKFLOW -------------- */

long *form_food_instant(void)
{
   return &g_food.t;
}

/* GRAMS, and the flag that says a PERSON typed them. The picker seeds the
 * portion with what this food was last logged with, and a suggestion must
 * never overwrite an answer -- see g_food.g_typed. */
void form_food_set_grams(int grams)
{
   g_food.g       = grams;
   g_food.g_typed = 1;
}

void form_food_view(struct forms_view *out)
{
   out->food_t        = g_food.t;
   out->food_type     = g_food.type;
   out->food_g        = g_food.g;
   out->food_edit     = g_food.edit;
   out->food_orig     = g_food.orig;
   out->foodtype_page = g_foodtype_page;
   out->foodlog_page  = g_foodlog_page;
}
