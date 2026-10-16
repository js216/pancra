// SPDX-License-Identifier: GPL-3.0
// exrow.c --- one decoder for one row of the exercise log
// Copyright 2026 Jakob Kastelic

/* See exrow.h for why this is shared. */
#include "exrow.h"

#include "csvcur.h"

int ex_row_decode(const char *p, const char *e, struct ex_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   /* EVERY FIELD IS A WHOLE NUMBER OR (offset, duration) EMPTY -- the
    * offset may carry a '-' -- AND ENDS AT A SEPARATOR OR THE LINE'S END. An over-long digit run keeps only its
    * leading 18 digits (csvcur.h), so "0000000000000000020" would read as
    * level 2, and is refused; a field that stops at anything but ',' was not
    * written by this app ("t,2.5" is not level 2). The instant and the level
    * must be there. */
   enum csv_field w = CSV_FIELD_EMPTY;
   r->t             = csv_num(&c, &w);
   if (w != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   long lv = csv_num(&c, &w);
   if (w != CSV_FIELD_OK)
      return 0;
   /* THE THIRD AND FOURTH FIELDS MAY NOT BE THERE, and both cases are
    * ordinary rather than damage: a row may end after the level or after the
    * offset, and an absent duration reads as 0, which is exactly what "no
    * end was ever recorded" means. An empty field and a
    * written 0 are the same answer for these columns. */
   r->tz  = 0;
   r->dur = 0;
   if (csv_sep(&c)) {
      r->tz = csv_num(&c, &w);
      if (w == CSV_FIELD_OVERFLOW)
         return 0;
      if (csv_sep(&c)) {
         r->dur = csv_num(&c, &w);
         if (w == CSV_FIELD_OVERFLOW)
            return 0;
      }
   }
   if (!csv_at_end(&c))
      return 0;
   if (r->t <= 0 || r->t >= EX_T_MAX)
      return 0;
   if (lv < EX_MIN_LEVEL || lv > EX_MAX_LEVEL)
      return 0;
   /* A NEGATIVE OR ABSURD DURATION IS A CORRUPT ROW, not a long workout: it
    * would draw a line backwards across the plot or across every plot the app
    * has. Rejected here, where every other field's domain is checked. */
   if (r->dur < 0 || r->dur > EX_DUR_MAX)
      return 0;
   r->level = (int)lv;
   return 1;
}
