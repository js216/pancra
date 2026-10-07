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
   /* NO `why` ON EITHER FIELD, and the level is worth spelling out because
    * the obvious reading says it needs one.
    *
    * csvcur.h's typed answer exists because an empty field reading 0 is
    * indistinguishable from a written 0 -- which matters wherever 0 is a
    * legitimate value (sensors.c's `activation`, where it means "session start
    * unknown"). Here it is not one: the domain is 1..3, so the range check
    * below rejects a 0 whatever produced it, and an over-long digit run keeps
    * only its leading digits, which cannot land in 1..3 either. Asking would
    * add a branch that no input can reach.
    *
    * Measured, not assumed: a mutant deleting a `why != CSV_FIELD_OK` guard
    * here survived the whole suite, because every input it would have caught
    * is already caught one line further down. The instant is the same story
    * against `t <= 0`. */
   r->t = csv_num(&c, 0);
   csv_sep(&c);
   long lv = csv_num(&c, 0);
   /* THE THIRD AND FOURTH FIELDS MAY NOT BE THERE, and both cases are
    * ordinary rather than damage. csv_sep answers 0 when the row simply ends,
    * and a row written before the duration column existed ends after the
    * offset -- so an absent field reads as 0, which is exactly what "no end
    * was ever recorded" means. Asking csv_num why it read 0 would not
    * distinguish an empty field from a written 0 here either, and both are
    * the same answer for this column. */
   csv_sep(&c);
   r->tz = csv_num(&c, 0);
   csv_sep(&c);
   r->dur = csv_num(&c, 0);
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
