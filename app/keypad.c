// SPDX-License-Identifier: GPL-3.0
// keypad.c --- the one description of every keypad mode (see keypad.h)
// Copyright 2026 Jakob Kastelic

#include "keypad.h"

/* ONE ROW PER MODE. The two tables this replaced -- titles in the renderer,
 * slot counts beside them -- were indexed by the same integer and maintained
 * separately, which is how the weight mode came to be missing from one of
 * them.
 *
 * The slot counts, and why each is what it is:
 *   4  a sensor pairing code
 *   3  a glucose value (plot maximum, calibration, rescale)
 *   15 a dotted quad, from when the server was one
 *   5  a TCP port, 1..65535
 *   2  insulin units, 1..99
 *   4  MMDD, HHMM, YYYY
 *   4  a threshold: holds "999" and the mmol/L form "55.5"
 *   5  a weight: "162.4" -- three digits, a dot and a tenth
 *   6  the SERVER's pairing code, which is six digits and burns after three
 *      attempts; a four-slot field would make pairing impossible rather than
 *      merely awkward
 */
static const struct kp_info g_modes[KP_NMODES] = {
    [KP_PAIR_CODE]  = {0,              4,  0, 0, 0, 0, 0},
    [KP_PLOT_MAX]   = {"PLOT MAX",     3,  0, 1, 0, 0, 0},
    [KP_CALIB]      = {"CALIBRATION",  3,  0, 1, 0, 0, 0},
    [KP_RESCALE]    = {"RESCALE",      3,  0, 1, 0, 0, 0},
    [KP_SERVER]     = {"SERVER",       15, 0, 0, 0, 0, 0},
    [KP_PORT]       = {"REMOTE PORT",  5,  0, 0, 0, 0, 0},
    [KP_INS_UNITS]  = {"UNITS",        2,  0, 0, 0, 0, 0},
    [KP_DATE]       = {"DATE (MMDD)",  4,  0, 0, 0, 0, 0},
    [KP_TIME]       = {"TIME (HHMM)",  4,  0, 0, 0, 0, 0},
    [KP_YEAR]       = {"YEAR",         4,  0, 0, 0, 0, 0},
    [KP_ALARM_LOW]  = {"ALARM LOW",    4,  1, 1, 0, 1, 0},
    [KP_ALARM_HIGH] = {"ALARM HIGH",   4,  1, 1, 0, 0, 0},
    [KP_NUDGE_LOW]  = {"NUDGE LOW",    4,  1, 1, 0, 1, 1},
    [KP_NUDGE_HIGH] = {"NUDGE HIGH",   4,  1, 1, 0, 0, 1},
    [KP_WEIGHT]     = {"WEIGHT",       5,  0, 1, 1, 0, 0},
    [KP_SYNC_CODE]  = {"PAIRING CODE", 6,  0, 0, 0, 0, 0},
    /* The weight form's calendar fields: the same entry as the insulin
     * form's, and their own modes so the commit knows whose instant it is
     * editing. */
    [KP_WT_DATE] = {"DATE (MMDD)",  4,  0, 0, 0, 0, 0},
    [KP_WT_TIME] = {"TIME (HHMM)",  4,  0, 0, 0, 0, 0},
    [KP_WT_YEAR] = {"YEAR",         4,  0, 0, 0, 0, 0},
    /* The LOG FOOD form. GRAMS is a whole number with a unit suffix and no
     * decimal point: portions are recorded to the gram, and a '.' key would
     * invite tenths the format does not store. Five digits covers
     * FOOD_MAX_G. */
    [KP_FOOD_G]    = {"GRAMS",        5,  0, 1, 0, 0, 0},
    [KP_FOOD_DATE] = {"DATE (MMDD)",  4,  0, 0, 0, 0, 0},
    [KP_FOOD_TIME] = {"TIME (HHMM)",  4,  0, 0, 0, 0, 0},
    [KP_FOOD_YEAR] = {"YEAR",         4,  0, 0, 0, 0, 0},
};

/* A mode this build does not define draws nothing and takes nothing. NOT the
 * pairing entry: the renderer's old fallback was "anything unknown is the
 * pairing keypad", so a weight tap opened the sensor-pairing flow with a
 * title that looked deliberate. */
static const struct kp_info g_unknown = {0, 0, 0, 0, 0, 0, 0};

const struct kp_info *kp_info(enum keypad_mode mode)
{
   if (mode < 0 || mode >= KP_NMODES)
      return &g_unknown;
   return &g_modes[mode];
}

int kp_slots(enum keypad_mode mode)
{
   return kp_info(mode)->slots;
}

int kp_is_thresh(enum keypad_mode mode)
{
   return kp_info(mode)->thresh;
}

int kp_is_low(enum keypad_mode mode)
{
   return kp_info(mode)->low;
}

int kp_is_nudge(enum keypad_mode mode)
{
   return kp_info(mode)->nudge;
}

/* THE ROW INDEX -> FIELD tables. Written out rather than added to
 * KP_INS_UNITS: the arithmetic was correct and silent, and the next field
 * inserted into this enum would have moved both forms without touching
 * either. */
enum keypad_mode kp_ins_field(int ix)
{
   static const enum keypad_mode f[] = {KP_INS_UNITS, KP_DATE, KP_TIME,
                                        KP_YEAR};

   if (ix < 0 || ix >= (int)(sizeof f / sizeof f[0]))
      return KP_NONE;
   return f[ix];
}

enum kp_form kp_form_of(enum keypad_mode mode)
{
   /* Listed, not derived from the enum's order. keypad.h already records what
    * happened when two files computed a mode as `KP_INS_UNITS + ix`: the
    * order of this enum became load-bearing in places that never said so. */
   switch (mode) {
      case KP_WEIGHT:
      case KP_WT_DATE:
      case KP_WT_TIME:
      case KP_WT_YEAR: return KP_FORM_WEIGHT;
      case KP_INS_UNITS:
      case KP_DATE:
      case KP_TIME:
      case KP_YEAR: return KP_FORM_INSULIN;
      case KP_FOOD_G:
      case KP_FOOD_DATE:
      case KP_FOOD_TIME:
      case KP_FOOD_YEAR: return KP_FORM_FOOD;
      /* Every other mode is not a form field, and says so rather than
       * defaulting into somebody's timestamp. */
      case KP_NONE:
      case KP_PAIR_CODE:
      case KP_PLOT_MAX:
      case KP_CALIB:
      case KP_RESCALE:
      case KP_SERVER:
      case KP_PORT:
      case KP_ALARM_LOW:
      case KP_ALARM_HIGH:
      case KP_NUDGE_LOW:
      case KP_NUDGE_HIGH:
      case KP_SYNC_CODE:
      case KP_NMODES: return KP_FORM_NONE;
   }
   return KP_FORM_NONE;
}

/* Listed, like kp_form_of, and for the same reason: the alternative is
 * arithmetic on the enum's order, which keypad.h records going wrong. */
int kp_is_date(enum keypad_mode mode)
{
   return mode == KP_DATE || mode == KP_WT_DATE || mode == KP_FOOD_DATE;
}

int kp_is_time(enum keypad_mode mode)
{
   return mode == KP_TIME || mode == KP_WT_TIME || mode == KP_FOOD_TIME;
}

int kp_is_year(enum keypad_mode mode)
{
   return mode == KP_YEAR || mode == KP_WT_YEAR || mode == KP_FOOD_YEAR;
}

enum keypad_mode kp_food_field(int ix)
{
   /* ITS OWN calendar fields, like the weight form's: see keypad.h. */
   static const enum keypad_mode f[] = {KP_FOOD_G, KP_FOOD_TIME, KP_FOOD_DATE,
                                        KP_FOOD_YEAR};

   if (ix < 0 || ix >= (int)(sizeof f / sizeof f[0]))
      return KP_NONE;
   return f[ix];
}

enum keypad_mode kp_weight_field(int ix)
{
   /* ITS OWN calendar fields, not the insulin form's: see keypad.h. */
   static const enum keypad_mode f[] = {KP_WEIGHT, KP_WT_DATE, KP_WT_TIME,
                                        KP_WT_YEAR};

   if (ix < 0 || ix >= (int)(sizeof f / sizeof f[0]))
      return KP_NONE;
   return f[ix];
}

int kp_has_dot(enum keypad_mode mode, int mmol_units)
{
   const struct kp_info *k = kp_info(mode);
   /* A threshold takes a decimal point only where the unit has one: mg/dL is
    * whole numbers, mmol/L is "5.5". A weight always does. This was two
    * expressions in two files, and they disagreed. */
   return k->dot_always || (k->thresh && mmol_units);
}
