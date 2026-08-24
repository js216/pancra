// SPDX-License-Identifier: GPL-3.0
// forms.h --- Typed entry: the keypad and the two logging forms
// Copyright 2026 Jakob Kastelic
//
/* EVERYTHING THE USER TYPES, and everything that decides whether to keep it.
 *
 * The dose form, the weight form, the alphanumeric editor and the numeric
 * keypad are one workflow: a value is typed into a buffer, a commit function
 * validates it against the rules for that field, and it is either written or
 * REFUSED with a reason the screen shows. They were spread through main.c as
 * a dozen globals and eight commit branches inside a dispatcher.
 *
 * TWO RULES HOLD ACROSS ALL OF THEM, and both exist because they were broken:
 *
 * 1. A REFUSAL IS VISIBLE. Every commit either writes or sets kp_err; it must
 *    never drop a value silently, because the user has no other way to tell a
 *    stored value from a discarded one.
 * 2. AN EDIT MATCHES ON THE ROW, NOT ITS POSITION. Both logs are rewritten by
 *    comparing against a COPY of the original record: an index into the tail
 *    goes stale the moment the log reloads, and rewriting by position is how
 *    an edit lands on the wrong dose.
 *
 * The state below is EXPORTED because the renderer reads it (build_model
 * copies it into the screen model) -- but it is WRITTEN only in forms.c.
 */
#ifndef FORMS_H
#define FORMS_H

#include "insulin.h" /* struct ins_rec */
#include "keypad.h"  /* enum keypad_mode: what the keypad collects */
#include "ui.h"      /* enum ui_screen */
#include "weight.h"  /* struct wt_rec */

/* Which page each log is showing, and how the weight plot is being read. */

/* LOG INSULIN form: the instant is edited as a whole (the date and time
 * steppers both move it); units re-populate from the last dose of the
 * selected type when the form opens or the type toggles. */
/* Which dose the form EDITS (-1 = none, logging new), and the original row. */

/* LOG WEIGHT form: held in TENTHS OF THE DISPLAY UNIT, not grams -- it is
 * what the user typed and what the keypad round-trips, converted once on
 * CONFIRM. Grams here would re-render the field whenever the unit preference
 * changed mid-entry. */

/* The keypad: what has been typed, why the last attempt was refused, which
 * field is being typed into, and where it returns when it closes. */

/* THREE fields share the alphanumeric editor -- a sensor name, the sync
 * server and the account email -- and they commit to different places, so
 * which one opened it is recorded rather than guessed from the menu. */
enum { LABEL_SENSOR = 0, LABEL_SERVER = 1, LABEL_EMAIL = 2, LABEL_FOOD = 3 };

/* The rescale value awaiting CONFIRM. Keypad state, not calibration state:
 * calib.c is told the number only once the user commits to it. */

/* Which insulin type the MARKER PICKER is styling (INS_SLOW / INS_FAST), or
 * -1 when it is editing a sensor's styling instead. */

/* WHAT AN OK-COMMIT FAMILY DECIDED.
 *
 * Three answers, not two: a single chain has three exits and one of them is
 * a bare `return` out of menu_action -- which also skips the repaint at the
 * bottom of it. That is load-bearing behaviour for
 * a refused entry: the family has already cleared the entry and put a message
 * on the keypad, and the screen it wants is the one already on it.
 *
 * (Whether skipping the repaint is RIGHT is a separate question: the cleared
 * entry and the refusal message then wait for the 1 Hz tick to appear. It is
 * preserved here exactly as it was rather than quietly changed under a
 * refactor.) */
#define COMMIT_PASS 0 /* not this family's field */
#define COMMIT_DONE 1 /* committed; menu_action repaints as usual */
#define COMMIT_STAY 2 /* refused; the keypad stays up and is NOT repainted */

/* WHAT A CONFIRMED WRITE LEFT ON THE DISK, and therefore whether the form
 * that asked for it may be torn down.
 *
 * CONFIRM must not clear the draft and navigate away whatever the write
 * answers. The draft is the only copy of what the user typed, and for an
 * EDIT it also carries `orig` -- the copy of the row being rewritten, which
 * is the match key weight_update and insulin_update need. Throwing both away
 * after a write that did NOT happen leaves a populated form that no longer
 * knows which row it is amending, so the next CONFIRM APPENDS a second copy
 * of the dose rather than rewriting the first. A failed delete is the way in:
 * it reports the failure and returns to exactly that form.
 *
 * DONE IS NOT ZERO, deliberately. Each of these is read to decide whether to
 * throw the user's state away, so the value a dropped comparison or a missed
 * initialisation yields must be the one that KEEPS it. (`enum csv_field` is
 * the other way round, and CSV_FIELD_OK == 0 silently inverted an `!ok` test
 * at a call site in sensors.c.) */
enum draft_fate {
   DRAFT_RETRY = 0, /* nothing reached the disk: keep it all, stay put */
   DRAFT_DONE  = 1  /* durable: clear the draft and navigate */
};

/* Close the keypad, returning where it was opened from. */
void keypad_close(void);

/* The two form dispatchers: each returns 1 when the action was one of its
 * own. */

/* The keypad's OK, per field. Each returns 1 when it handled the mode. */
int label_commit(void);
int kp_commit_correction(void);
int kp_commit_thresholds(void);
int kp_commit_number(void);
int kp_commit_datetime(void);

/* ---- OPENING AND DRIVING A FORM, as intents ---------------------------
 *
 * The state above is written through these, not by hand. Spelled out, a
 * keypad opener is three assignments in a row (mode, return screen, clear the
 * entry) repeated at fifteen call sites, and forgetting the third leaves the
 * previous entry in the field -- which on the PIN keypad is somebody else's
 * typing. One call cannot forget. */

/* Open the keypad in `mode`, returning to `ret` when it closes, with an EMPTY
 * entry. */
void forms_kp_open(enum keypad_mode mode, enum ui_screen ret);
/* Where the keypad returns to (some flows re-aim it after opening). */
void forms_kp_return_set(enum ui_screen ret);
enum ui_screen forms_kp_return(void);
/* Start the entry from nothing, or from `text` (the rename editor prefills
 * with the current name). Both clear the error line: opening is not a
 * refusal. */
void forms_kp_clear(void);
void forms_kp_seed(const char *text);
/* Type one character; refused when the field is full. */
void forms_kp_type(char c);
/* How many characters are in it, and a copy of them. */
int forms_kp_len(void);
void forms_kp_text(char *out, int cap);

/* ---- PUTTING A SAVED DRAFT BACK ---------------------------------------
 *
 * Android can kill this process while a dose or a weight is half typed and
 * restore the task afterwards, and the shell now snapshots the draft so the
 * digits are still there. These are how it comes back.
 *
 * BOTH FORCE `edit` TO -1, AND THAT IS THE POINT, not an omission. A draft
 * that was EDITING a row carries `orig`, a whole copy of that row, as the
 * rewrite's match key -- and the log it matches against is reloaded from disk
 * by the time this is called, possibly shorter (a truncated file), possibly
 * different (a sync pulled the server's record down). Restoring an edit would
 * put the user in front of a form that looks like it is amending an entry
 * while the entry it names may no longer exist; CONFIRM would then either
 * fail silently or, if a match were found, land on a row they are not
 * looking at. So the shell never SAVES an edit in progress (see main.c's
 * scr_saveable), and these two cannot express one even if it did.
 *
 * The values are otherwise taken as given: they were validated by the shell
 * against the units and the bounds that were loaded from disk before it got
 * here, which is a question this module cannot ask. */
/* ONE TAP, THROUGH ONE DOOR. Returns 1 when a typed-entry workflow claimed
 * the action -- weight, insulin, food or exercise, each in its own file (see
 * app/formsint.h). The caller does not choose which: the workflows are tried
 * in a fixed order and the first match wins, exactly as they did when they
 * were four `||`-ed calls in menu_action, but the ORDER now lives beside the
 * workflows it orders rather than in the menu. */
int forms_action(int action, int ix);


/* The odds and ends the menus set. */
void forms_set_label_field(int field);
void forms_set_markpick(int ins_type);
int forms_markpick(void);
/* The keypad's mode, for the two flows that re-aim an already-open one. */
void forms_kp_mode_set(enum keypad_mode mode);
enum keypad_mode forms_kp_mode(void);
/* One character back, and "this keystroke retires the last refusal". */
void forms_kp_del(void);
void forms_kp_err_clear(void);
/* Is this character already in the entry? (One decimal point per number.) */
int forms_kp_has(char c);
void forms_set_rescale_entry(int tenths);
int forms_rescale_entry(void);
void forms_set_cal_pending(int mgdl);
int forms_cal_pending(void);
/* The weight plot's scrub cursor: -1 = none. */

/* THE GLUCOSE PLOT'S SCRUB CURSOR, the same way and for the same reason.
 *
 * It sat in input.c as gesture state, and the frame builder reached up into
 * the input layer to read it -- the only thing it wanted from there, and
 * enough to make the model and the input handler include each other. A
 * highlighted point is transient screen state, exactly like the weight
 * cursor beside it: the gesture WRITES it, the frame READS it, and neither
 * has to know about the other. */
void forms_set_scrub(int idx);
int forms_scrub(void);

/* ---- WHAT A FRAME MAY READ OF THE FORMS ------------------------------
 *
 * A read-only snapshot, for the same two reasons menuview.h gives: a frame
 * reading fifteen separate globals as it goes can give one frame two
 * different answers when a tap arrives mid-frame, and every one of those
 * globals is writable by anything that includes this header.
 *
 * Main thread only, like the forms themselves. */
struct forms_view {
   /* the keypad. ENUM-TYPED, like the field it is copied FROM (forms.c's
    * g_kp.mode) and the one it is copied INTO (ui_entry.kp_mode): an `int`
    * here made this snapshot the one hop on that path where the value
    * stopped being a keypad mode and became a number, which is exactly the
    * convention keypad.h exists to remove. A snapshot is a copy, and a copy
    * that widens its own type is a place an arbitrary integer can enter. */
   enum keypad_mode kp_mode;
   int entrylen;
   char entry[64];
   char kp_err[40];
   /* the insulin form and its log */
   long ins_t;
   int ins_type, ins_milli, ins_edit;
   int inslog_page;
   int markpick_ins;
   /* the weight form, its log and its plot */
   long wt_t;
   int wt_tenths, wt_edit;
   struct wt_rec wt_orig;
   int wtlog_page, wt_tab;
   /* the food form and the picker's page */
   long food_t;
   int food_type; /* the chosen type ID, not an index: see ui_foodview */
   int food_g, food_edit;
   struct food_rec food_orig;
   /* The EDIT EXERCISE draft: the instant and level it holds, whether it is
    * open at all (`ex_edit` < 0 = not), and the row it is correcting. */
   long ex_t;
   long ex_dur; /* seconds, 0 = not known */
   int ex_level, ex_edit;
   /* 1 when the row being edited is the session the button is running now
    * (exercise_row_running): its length is not settled, so the form shows
    * ACTIVE there and refuses the field. */
   int ex_running;
   const char *ex_err; /* why the last CONFIRM was refused; "" = nothing */
   struct ex_rec ex_orig;
   int foodtype_page, foodlog_page, exlog_page;
   int exlog_tab, inslog_tab; /* the two daily plots' spans */
   int log_scrub;             /* the bar/point under the finger, or -1 */
   int scrub; /* the glucose plot cursor, -1 = none */
   /* the odds and ends */
   int label_field;
   int rescale_entry;
   int cal_pending;
};

/* Open the food form on a NEW entry at instant `t`, discarding whatever the
 * draft held. This is what both opening the workflow and closing it call: a
 * form is only ever entered with its whole state stated. */
void forms_food_open(long t);

/* Open an existing entry for editing; keeps a copy as the rewrite's key. */
void forms_food_edit(int i);

/* Open an exercise entry for correction. `i` indexes the tail, oldest first;
 * out of range opens nothing. There is deliberately no opener for a NEW
 * entry: an exercise record is created by the button and its settling rule,
 * never by a form -- see uiex.c. */
void forms_ex_edit(int i);

/* Set the type the form holds; the picker's only side effect on the form. */
void forms_food_type_set(int type_id);

/* One consistent copy of all of it. */
/* The bar or point the finger is on in whichever log plot is open; -1 clears
 * it. Set by the shell's scrub gesture, read back through the frame. */
void forms_set_log_scrub(int idx);

void forms_view_get(struct forms_view *out);

#endif
