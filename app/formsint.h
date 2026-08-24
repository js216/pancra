// SPDX-License-Identifier: GPL-3.0
// formsint.h --- what the typed-entry controllers share, and nothing else
// Copyright 2026 Jakob Kastelic

/* ONE WORKFLOW PER FILE, AND ONE KEYPAD BETWEEN THEM.
 *
 * forms.h is the interface the app uses: open a form, act on a tap, read the
 * whole of it as one snapshot. Behind it, each workflow that can be TYPED --
 * weight, insulin, food, exercise -- owns its own draft, its own paging and
 * its own actions, in its own file:
 *
 *   app/formwt.c    LOG/EDIT WEIGHT, the weight log's paging and its plot tab
 *   app/formins.c   LOG/EDIT INSULIN and the dose table's paging
 *   app/formfood.c  LOG/EDIT FOOD, the type picker and both its pages
 *   app/formex.c    EDIT EXERCISE and the exercise log's paging
 *   app/forms.c     the KEYPAD itself, the commits that are grammar rather
 *                   than workflow, and the snapshot that assembles them
 *
 * WHY THE SPLIT IS HERE AND NOT ELSEWHERE. What made one file hard was not
 * its length: it was that four unrelated drafts, four sets of paging state
 * and one keypad were all in scope of each other, so nothing stopped a
 * weight action reading the food draft's leftovers -- which is the failure
 * that shape invites, and it is not a crash. A draft that only its own file
 * can see cannot be read by the wrong workflow at all.
 *
 * WHAT STAYS SHARED, and it is deliberately small: the keypad is ONE screen
 * with one entry buffer, so its state, its grammar (what a number means in
 * each mode, what a refusal says) and the commit that dispatches on that mode
 * belong to one file. Each controller exposes the narrowest possible sink for
 * it -- "the user typed this many tenths" -- and nothing more.
 *
 * INCLUDED BY the four controllers and by forms.c. Nothing else: a caller
 * outside them wants forms.h, which hands out a snapshot instead. Include
 * forms.h first -- the fillers below take its struct. */
#ifndef PANCRA_FORMSINT_H
#define PANCRA_FORMSINT_H

#ifndef FORMS_H
#error "app/formsint.h: include app/forms.h first"
#endif

/* ---- WHAT THE SHELL GIVES THE CONTROLLERS ---------------------------- */

/* (keypad_close() is forms.h's, and stays there: closing the keypad is
 * something the SHELL asks for too -- a menu action, a navigation -- not
 * only these four controllers.) */

/* THE ZONE OFFSET AT AN INSTANT, not the current one: a weigh-in backdated
 * across a DST boundary must carry the offset that was in force THEN, or the
 * row says something different from what the user entered. */
long form_zone(void *ctx, long t);

/* ---- WHAT EACH CONTROLLER GIVES THE SHELL ---------------------------- */

/* One tap. Returns 1 when the action belonged to this workflow, 0 when it
 * did not -- forms_action tries each in turn, so an action may be claimed by
 * exactly one of them. */
int form_wt_action(int action, int ix);
int form_ins_action(int action, int ix);
int form_food_action(int action, int ix);
int form_ex_action(int action, int ix);

/* THE INSTANT THIS WORKFLOW IS EDITING, or NULL when its form is not open.
 * The date and time keypads write through this pointer, which is why the
 * mapping from keypad mode to workflow (form_instant_of, in forms.c) has to
 * be total: a mode that belongs to no form must get NULL rather than
 * somebody else's timestamp. */
long *form_wt_instant(void);
long *form_ins_instant(void);
long *form_food_instant(void);
long *form_ex_instant(void);

/* THE DURATION THE EDIT EXERCISE FORM IS HOLDING, in SECONDS, or NULL when
 * that form is not open. The keypad collects minutes. */
long *form_ex_duration(void);

/* THE NUMBER THE KEYPAD ACCEPTED, in this workflow's own unit. The keypad
 * owns the grammar (how many digits, where the decimal point may go, what a
 * refusal says); each of these is the one line that follows a number the
 * keypad has already judged. */
void form_wt_set_tenths(int tenths);
/* THOUSANDTHS of a unit -- the keypad has already bounded it; this is the
 * assignment that follows. */
void form_ins_set_units(int milli);
void form_food_set_grams(int grams);

/* This workflow's part of the frame's snapshot. Each fills only its own
 * fields, so a field can be traced to exactly one writer. */
void form_wt_view(struct forms_view *out);
void form_ins_view(struct forms_view *out);
void form_food_view(struct forms_view *out);
void form_ex_view(struct forms_view *out);

#endif
