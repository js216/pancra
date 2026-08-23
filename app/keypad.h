// SPDX-License-Identifier: GPL-3.0
// keypad.h --- what each keypad mode IS, in one place
// Copyright 2026 Jakob Kastelic

/* ONE KEYPAD, SIXTEEN JOBS, AND THE NUMBER 14 IN FOUR FILES.
 *
 * There is a single digit keypad. Which value it is collecting -- a pairing
 * code, a calibration, an alarm threshold, a dose, a weight -- was a bare int
 * passed around as bare integers, compared against literals in the shell and
 * the renderer, and opened as `<base> + ix` at the two forms that use one.
 * Every fact about a mode was restated wherever it was needed:
 *
 *   - how many digits it takes (a table in the renderer),
 *   - what it is called (another table, beside it),
 *   - which unit suffix it shows, if any (see enum kp_unit),
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
   KP_PORT      = 4,
   /* The insulin form's four fields; the weight form reuses the last three,
    * which are a calendar instant and carry no insulin meaning. The forms
    * open one by ROW INDEX, and that translation is kp_ins_field() below --
    * not `KP_INS_UNITS + ix` at the call site, which is a second, silent
    * statement about this enum's order. */
   KP_INS_UNITS = 5,
   KP_DATE      = 6, /* MMDD */
   KP_TIME      = 7, /* HHMM */
   KP_YEAR      = 8,
   /* The four thresholds, in the order the ALARM screen lists them. Which
    * PAIR one belongs to and which END it is are facts about the mode, and
    * they are in the table below. Derived by the shell with `>= KP_NUDGE_LOW`
    * and `% 2`, this order would be a load-bearing secret shared by two
    * files. */
   KP_ALARM_LOW  = 9,
   KP_ALARM_HIGH = 10,
   KP_NUDGE_LOW  = 11,
   KP_NUDGE_HIGH = 12,
   KP_WEIGHT     = 13,
   KP_SYNC_CODE  = 14, /* the SERVER's 6-digit pairing code, not the sensor's */
   /* THE WEIGHT FORM'S OWN calendar fields. They collect exactly what
    * KP_DATE / KP_TIME / KP_YEAR collect and they are separate modes anyway,
    * because WHICH INSTANT is being edited is a fact about the field -- and
    * with the modes shared, the commit could not tell, so it read the RETURN
    * SCREEN instead, choosing between the weight form's instant and the
    * insulin form's by asking where the keypad would return to. That
    * is the numeric protocol this enum exists to remove, wearing a different
    * hat: a keypad re-aimed with forms_kp_return_set would edit the wrong
    * form's instant. Ask the retired weight-or-not boolean() instead. */
   KP_WT_DATE = 15,
   KP_WT_TIME = 16,
   KP_WT_YEAR = 17,
   /* The LOG FOOD form's own fields, and its own calendar modes for the same
    * reason the weight form has its own: which form an instant belongs to is
    * a property of the MODE. Sharing the insulin form's KP_DATE here would
    * make the answer depend on which screen the keypad happened to return
    * to, which is a different fact and one a user can change. */
   KP_FOOD_G    = 18,
   KP_FOOD_DATE = 19,
   KP_FOOD_TIME = 20,
   KP_FOOD_YEAR = 21,
   /* The EDIT EXERCISE form's own fields. The LEVEL is not among them: it
    * cycles 1-2-3 on the form itself, because those are the only three values
    * there are and the ADD button already says them that way. The DURATION is
    * a number, and one the button cannot know -- a row is written when the
    * level settles, which is the start -- so it is typed. */
   KP_EX_DATE = 22,
   KP_EX_TIME = 23,
   KP_EX_YEAR = 24,
   KP_EX_DUR  = 25,
   KP_NMODES  = 26
};

/* WHICH UNIT THE ENTRY CARRIES -- a name, not a yes/no.
 *
 * As `int unit` -- a flag meaning "print the glucose unit here" -- the
 * renderer needs one hardcoded exception for the WEIGHT keypad, on the
 * premise that weight is "the one entry that is not a glucose value". The
 * food form makes that premise false without saying so: GRAMS inherits the
 * flag and labels a portion of beans MG/DL. A boolean cannot be wrong about
 * which unit it means, so the compiler has nothing to
 * check.
 *
 * Naming the unit puts the answer in the same table as the title and the slot
 * count, and -Wswitch-enum then makes the draw site fail to build until a new
 * unit is handled. NONE is a real answer -- a date, a port, a pairing code --
 * and not a default. */
enum kp_unit {
   KP_UNIT_NONE = 0, /* bare digits: dates, times, codes, ports, insulin */
   KP_UNIT_GLU,      /* mg/dL or mmol/L, whichever the user displays in */
   KP_UNIT_WT,       /* kg or lb, per the weight preference */
   KP_UNIT_G         /* grams -- a food portion, and not a preference */
};

/* Everything the two sides need to know about one mode. */
struct kp_info {
   const char *title; /* the heading, or NULL for KP_PAIR_CODE, whose title
                       * names the sensor being paired */
   int slots;         /* digit cells: the renderer draws exactly this many and
                       * the input path accepts exactly this many */
   int thresh;        /* one of the four alarm/nudge thresholds */
   enum kp_unit unit; /* which unit suffix the entry carries, if any */
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
 * A WEIGHT-OR-NOT BOOLEAN is correct for exactly as long as there are two
 * forms. A third (LOG FOOD) makes "not the weight form" mean two different
 * things, and a caller that asks "is this the weight form" and picks
 * &g_wt.f.t or &g_ins.f.t from the answer silently moves the INSULIN form's
 * instant for a food date, with nothing on
 * either screen to say so. That is the same shape as the two-outcome
 * two-answer database lookup this codebase has been bitten by before: two
 * answers for three situations, where the third quietly borrows one of the
 * others.
 *
 * KP_FORM_NONE is the honest answer for every mode that is not a form field
 * at all, rather than defaulting to somebody's timestamp. */
enum kp_form {
   KP_FORM_NONE = 0,
   KP_FORM_INSULIN,
   KP_FORM_WEIGHT,
   KP_FORM_FOOD,
   KP_FORM_EXERCISE
};
enum kp_form kp_form_of(enum keypad_mode mode);

/* WHAT KIND OF FIELD a mode is, across every form.
 *
 * A COMMIT THAT ASKS BY NAME -- `mode == KP_DATE || mode == KP_WT_DATE` --
 * is a list that has to be extended in four places every time a form is
 * added, and which fails SILENTLY when it is not: the field simply stops
 * committing, or commits down a branch meant for something else. That is how
 * a form ships with a date that goes nowhere.
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
/* The EDIT EXERCISE form's rows, minus the LEVEL row the caller handles
 * itself: 0 time, 1 date, 2 year. KP_NONE for anything else. */
enum keypad_mode kp_ex_field(int ix);

#endif
