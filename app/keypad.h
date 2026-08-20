// SPDX-License-Identifier: GPL-3.0
// keypad.h --- what each keypad mode IS, in one place
// Copyright 2026 Jakob Kastelic

/* ONE KEYPAD, SIXTEEN JOBS, AND THE NUMBER 14 IN FOUR FILES.
 *
 * There is a single digit keypad. Which value it is collecting -- a pairing
 * code, a calibration, an alarm threshold, a dose, a weight -- was a bare int
 * passed around as 0..15, compared as `mode == 14` in the shell, `mode >= 10
 * && mode <= 13` in the renderer, and `6 + ix` at the two forms that open it.
 * Every fact about a mode was restated wherever it was needed:
 *
 *   - how many digits it takes (a table in the renderer),
 *   - what it is called (another table, beside it),
 *   - whether it shows a unit suffix (an expression at the draw site),
 *   - whether it offers a '.' key (a different expression, at a different
 *     draw site, and a THIRD one in the input dispatcher),
 *   - whether it is one of the four thresholds (a predicate in the renderer,
 *     and the same range spelled out by hand in the shell).
 *
 * They drifted, as copies do. The dot-key range said 10..11 in the input path
 * while the renderer drew it for 10..13, so the NUDGE keypads had a visible,
 * tappable, DEAD '.' -- and with it no way to enter a nudge threshold at all
 * in mmol/L. The weight mode was missing from the slots table and worked by
 * falling through to the pairing code's entry, which happened to want the
 * same number of digits.
 *
 * So: a NAMED mode, and one description of each. The renderer asks what to
 * draw, the shell asks what a key does, and neither restates the other's
 * rules. Adding a mode is one row.
 *
 * The values are NOT a file format -- they never reach disk -- but they do
 * cross the shell/renderer boundary inside `struct screen`, so they are
 * explicit rather than implied by order.
 */
#ifndef PANCRA_KEYPAD_H
#define PANCRA_KEYPAD_H

enum keypad_mode {
   /* No mode -- an index that names no field, and what an unknown mode reads
    * as. The forms refuse to open a keypad on it, so it is never STORED; the
    * renderer draws a red "BAD KP MODE" for it anyway, because a mode that
    * reaches the screen unrecognised must look broken rather than look like
    * some other feature. */
   KP_NONE      = -1,
   KP_PAIR_CODE = 0, /* the SENSOR's 4-digit code; title built from the type */
   KP_PLOT_MAX  = 1,
   KP_CALIB     = 2,
   KP_RESCALE   = 3,
   KP_SERVER    = 4, /* unused since the server became a name, not a quad */
   KP_PORT      = 5,
   /* The insulin form's four fields; the weight form reuses the last three,
    * which are a calendar instant and carry no insulin meaning. The forms
    * open one by ROW INDEX, and that translation is kp_ins_field() below --
    * not `KP_INS_UNITS + ix` at the call site, which is a second, silent
    * statement about this enum's order. */
   KP_INS_UNITS = 6,
   KP_DATE      = 7, /* MMDD */
   KP_TIME      = 8, /* HHMM */
   KP_YEAR      = 9,
   /* The four thresholds, in the order the ALARM screen lists them. Which
    * PAIR one belongs to and which END it is are facts about the mode, and
    * they are in the table below -- the shell used to derive them with
    * `>= KP_NUDGE_LOW` and `% 2`, which made this order a load-bearing
    * secret shared by two files. */
   KP_ALARM_LOW  = 10,
   KP_ALARM_HIGH = 11,
   KP_NUDGE_LOW  = 12,
   KP_NUDGE_HIGH = 13,
   KP_WEIGHT     = 14,
   KP_SYNC_CODE  = 15, /* the SERVER's 6-digit pairing code, not the sensor's */
   /* THE WEIGHT FORM'S OWN calendar fields. They collect exactly what
    * KP_DATE / KP_TIME / KP_YEAR collect and they are separate modes anyway,
    * because WHICH INSTANT is being edited is a fact about the field -- and
    * with the modes shared, the commit could not tell, so it read the RETURN
    * SCREEN instead, choosing between the weight form's instant and the
    * insulin form's by asking where the keypad would return to. That
    * is the numeric protocol this enum exists to remove, wearing a different
    * hat: a keypad re-aimed with forms_kp_return_set would edit the wrong
    * form's instant. Ask kp_edits_weight() instead. */
   KP_WT_DATE = 16,
   KP_WT_TIME = 17,
   KP_WT_YEAR = 18,
   /* The LOG FOOD form's own fields, and its own calendar modes for the same
    * reason the weight form has its own: which form an instant belongs to is
    * a property of the MODE. Sharing the insulin form's KP_DATE here would
    * make the answer depend on which screen the keypad happened to return
    * to, which is a different fact and one a user can change. */
   KP_FOOD_G    = 19,
   KP_FOOD_DATE = 20,
   KP_FOOD_TIME = 21,
   KP_FOOD_YEAR = 22,
   KP_NMODES    = 23
};

/* Everything the two sides need to know about one mode. */
struct kp_info {
   const char *title; /* the heading, or NULL for KP_PAIR_CODE, whose title
                       * names the sensor being paired */
   int slots;         /* digit cells: the renderer draws exactly this many and
                       * the input path accepts exactly this many */
   int thresh;        /* one of the four alarm/nudge thresholds */
   int unit;          /* the entry carries a unit suffix (mg/dL, mmol/L, kg) */
   int dot_always;    /* a '.' key regardless of the display units (weight) */
   /* WHICH THRESHOLD, for the four that are one. `low` is the lower end of
    * its pair, `nudge` says which pair. Meaningless (0) for every other
    * mode, which is why they are read through kp_is_low / kp_is_nudge and
    * only after kp_is_thresh. */
   int low;
   int nudge;
};

/* Never NULL: an unknown mode answers with a description that draws nothing
 * and accepts nothing, so a bad mode is visibly broken rather than silently
 * the pairing keypad -- which is what the renderer's old `else` branch made
 * it, so tapping a weight opened the sensor-pairing flow and nothing about
 * it looked wrong. */
const struct kp_info *kp_info(enum keypad_mode mode);

/* The three questions both sides ask, so neither spells out a range.
 *
 * kp_has_dot takes the display units because the thresholds offer a decimal
 * point only in mmol/L ("5.5"), while a weight always does ("162.4"). Two
 * files disagreed about exactly this. */
int kp_slots(enum keypad_mode mode);
int kp_is_thresh(enum keypad_mode mode);
int kp_has_dot(enum keypad_mode mode, int mmol_units);

/* WHICH THRESHOLD a mode is, for the one caller that has to know: the shell
 * turns the typed number into alarm_set_threshold(isnudge, islow, value).
 * Ask these rather than deriving them from the enum's order. */
int kp_is_low(enum keypad_mode mode);
int kp_is_nudge(enum keypad_mode mode);

/* THE FORMS' FIELDS BY ROW INDEX.
 *
 * The insulin form lists units, date, time, year; the weight form lists
 * weight, date, time, year. Tapping row `ix` opens that field's keypad --
 * which was `KP_INS_UNITS + ix` at both call sites, i.e. two files quietly
 * asserting the order of this enum. KP_NONE for an index the form does not
 * have. */
enum keypad_mode kp_ins_field(int ix);
enum keypad_mode kp_weight_field(int ix);

/* WHICH FORM'S INSTANT a calendar mode edits. A property of the MODE, so
 * re-aiming the keypad's return screen cannot move the wrong form's
 * timestamp.
 *
 * THIS USED TO BE A BOOLEAN, kp_edits_weight, and it was correct for exactly
 * as long as there were two forms. A third (LOG FOOD) makes "not the weight
 * form" mean two different things, and the callers were written as
 * `kp_edits_weight(m) ? &g_wt.f.t : &g_ins.f.t` -- so a food date would have
 * silently moved the INSULIN form's instant, with nothing on either screen to
 * say so. That is the same shape as the two-outcome database lookup NOTES.md
 * records: two answers for three situations, where the third quietly borrows
 * one of the others.
 *
 * KP_FORM_NONE is the honest answer for every mode that is not a form field
 * at all, rather than defaulting to somebody's timestamp. */
enum kp_form {
   KP_FORM_NONE = 0,
   KP_FORM_INSULIN,
   KP_FORM_WEIGHT,
   KP_FORM_FOOD
};
enum kp_form kp_form_of(enum keypad_mode mode);

/* WHAT KIND OF FIELD a mode is, across every form.
 *
 * The commits used to ask by NAME -- `mode == KP_DATE || mode == KP_WT_DATE`
 * -- which is a list that has to be extended in four places every time a form
 * is added, and which fails SILENTLY when it is not: the field simply stops
 * committing, or commits down a branch meant for something else. Adding the
 * LOG FOOD form did exactly that, and the date it typed went nowhere.
 *
 * A mode has a kind the way it has a form (kp_form_of); asking for it is how a
 * call site stays correct when a form is added. */
int kp_is_date(enum keypad_mode mode);
int kp_is_time(enum keypad_mode mode);
int kp_is_year(enum keypad_mode mode);

/* The LOG FOOD form's fields by row index: 0 grams, 1 time, 2 date, 3 year.
 * (Row 0 of the form is TYPE, which opens the picker rather than a keypad, so
 * the dispatcher maps it before reaching here.) */
enum keypad_mode kp_food_field(int ix);

#endif
