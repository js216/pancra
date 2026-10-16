// SPDX-License-Identifier: GPL-3.0
// foodrow.c --- decoders for one row of the food log and of its vocabulary
// Copyright 2026 Jakob Kastelic

/* See foodrow.h for why this is shared. */
#include "foodrow.h"

#include "csvcur.h"

int food_macro_ok(int which, int milli)
{
   if (which < 0 || which >= FOOD_NMACRO || milli < 0)
      return 0;
   return milli <= (which == FOOD_KCAL ? FOOD_KCAL_MAX : FOOD_FRAC_MAX);
}

/* "0.454" -> 454: digits, then at most one '.' and three more. Anything else
 * -- a sign, a fourth decimal, a stray character, nothing at all -- is not a
 * value this format holds. 1 on success. */
static int milli_parse(const char *s, int *out)
{
   long v    = 0;
   int nd    = 0;
   int dot   = 0;
   int scale = 0;
   for (; *s; s++) {
      if (*s == '.' && !dot) {
         dot = 1;
      } else if (*s >= '0' && *s <= '9' && (!dot || scale < 3) && nd < 9) {
         v = (v * 10) + (*s - '0');
         nd++;
         if (dot)
            scale++;
      } else {
         return 0;
      }
   }
   if (nd == 0)
      return 0;
   while (scale++ < 3)
      v *= 10;
   if (v > 0x7fffffffL)
      return 0;
   *out = (int)v;
   return 1;
}

int food_goal_ok(int which, int v)
{
   if (which < 0 || which >= FOOD_NMACRO)
      return 0;
   if (which == FOOD_KCAL)
      return v >= 0 && v <= FOOD_GOAL_KCAL_MAX;
   return v >= 1 && v <= FOOD_GOAL_G_MAX;
}

int food_goals_decode(const char *p, const char *e, int out[FOOD_NMACRO])
{
   struct csv_cur c;
   csv_open(&c, p, e);
   int v[FOOD_NMACRO] = {0, 0, 0, 0};
   for (int k = 0; k < FOOD_NMACRO; k++) {
      if (k && !csv_sep(&c))
         return 0;
      enum csv_field why = CSV_FIELD_EMPTY;
      const long x       = csv_num(&c, &why);
      if (why != CSV_FIELD_OK || x < 0 || x > FOOD_GOAL_KCAL_MAX ||
          !food_goal_ok(k, (int)x))
         return 0;
      v[k] = (int)x;
   }
   if (!csv_at_end(&c))
      return 0;
   for (int k = 0; k < FOOD_NMACRO; k++)
      out[k] = v[k];
   return 1;
}

int food_name_ok(const char *n)
{
   if (!n || !n[0])
      return 0;
   if (n[0] == '#')
      return 0;
   int i = 0;
   for (; n[i]; i++) {
      if (n[i] == ',' || n[i] == '\n' || n[i] == '\r')
         return 0;
      if (i >= FOOD_NAME_MAX)
         return 0; /* longer than the field holds */
   }
   return 1;
}

/* "<t>,<type>,<grams>[,<tz>]". Every field is a whole number (the offset
 * may be negative) and ends at a separator or the line's end, as in exrow.c:
 * an over-long digit run is refused rather than read as its leading digits
 * (csvcur.h), and so is "t,12.5,...". The offset is not kept here, only
 * checked. */
int food_entry_decode(const char *p, const char *e, struct food_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   enum csv_field w = CSV_FIELD_EMPTY;
   r->t             = csv_num(&c, &w);
   if (w != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   long ty = csv_num(&c, &w);
   if (w != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   r->g = csv_num(&c, &w);
   if (w != CSV_FIELD_OK)
      return 0;
   if (csv_sep(&c)) {
      (void)csv_num(&c, &w);
      if (w != CSV_FIELD_OK)
         return 0;
   }
   if (!csv_at_end(&c))
      return 0;
   if (r->t <= 0 || r->t >= FOOD_T_MAX)
      return 0;
   if (r->g < FOOD_MIN_G || r->g > FOOD_MAX_G)
      return 0;
   if (ty <= FOOD_TYPE_NONE || ty > 0x7fffffffL)
      return 0;
   r->type = (int)ty;
   return 1;
}

int food_type_decode(const char *p, const char *e, int *id_out, char *name_out,
                     int macro_out[FOOD_NMACRO])
{
   struct csv_cur c;
   csv_open(&c, p, e);
   enum csv_field w = CSV_FIELD_EMPTY;
   long id          = csv_num(&c, &w);
   if (w != CSV_FIELD_OK || !csv_sep(&c))
      return 0; /* no separator: this is not a row, it is a fragment */
   /* ONE BYTE WIDER THAN A NAME, so a name longer than FOOD_NAME_MAX
    * reaches food_name_ok still too long, and is refused there. */
   char nm[FOOD_NAME_MAX + 2];
   csv_str(&c, nm, (int)sizeof nm);
   int macro[FOOD_NMACRO] = {0, 0, 0, 0};
   if (csv_sep(&c)) {
      for (int k = 0; k < FOOD_NMACRO; k++) {
         char f[16];
         if (k && !csv_sep(&c))
            return 0;
         csv_str(&c, f, (int)sizeof f);
         if (!milli_parse(f, &macro[k]) || !food_macro_ok(k, macro[k]))
            return 0;
      }
      if (c.p != c.e)
         return 0; /* a fifth value: not a row this app writes */
   }
   /* AN ID OF 0 IS NOT A TYPE (FOOD_TYPE_NONE), and a name that would not
    * survive a round trip through this format did not come from food_type_add
    * -- both mean the file has been edited or damaged. The largest int is
    * refused too: the loader mints the next id as the largest seen plus one,
    * and that must still be an int. */
   if (id <= FOOD_TYPE_NONE || id >= 0x7fffffffL)
      return 0;
   if (!food_name_ok(nm))
      return 0;
   *id_out = (int)id;
   for (int i = 0; i <= FOOD_NAME_MAX; i++) {
      name_out[i] = nm[i];
      if (!nm[i])
         break; /* csv_str terminates within nm; nothing past it is set */
   }
   for (int k = 0; k < FOOD_NMACRO; k++)
      macro_out[k] = macro[k];
   return 1;
}
