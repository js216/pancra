// SPDX-License-Identifier: GPL-3.0
// insrow.c --- one decoder for one row of the insulin assertion log
// Copyright 2026 Jakob Kastelic

/* See insrow.h for why this is shared. What follows is the grammar itself,
 * which is the phone's -- the stricter of the two readers that existed, and
 * the one whose rules were written down beside the code that enforced them. */
#include "insrow.h"

#include "csvcur.h"

/* How many separators the row has. The two dialects are told apart by the
 * FIELD COUNT and not by trying one and falling back: a four-field row and a
 * seven-field row are both valid, so "did the parse fail" is not the
 * question -- "which format is this" is. */
static int ins_ncommas(const char *p, const char *e)
{
   int n = 0;
   for (const char *q = p; q < e; q++)
      if (*q == ',')
         n++;
   return n;
}

int ins_row_decode(const char *p, const char *e, long legacy_id,
                   struct ins_row *out)
{
   if (!p || !e || !out || e < p)
      return 0;
   struct csv_cur c;
   csv_open(&c, p, e);
   out->del = 0;
   /* THE FIELD THIS FORMAT WILL NOT GUESS AT. Every other column is caught by
    * a range check below -- an empty `t` reads 0 and fails `t <= 0`, an empty
    * `type` is refused by the type check -- but `del` has no such luck: a missing
    * retraction flag reads as 0, which is the valid and much more common
    * answer, so a truncated row would silently resurrect a dose the user
    * deleted. It is asked about explicitly instead.
    *
    * AND IT IS 0 OR 1, NOT "NONZERO": any other value is not one this
    * format writes, and reading it as either answer would make it a live
    * dose to one reader and a retracted one to another, so the row is
    * refused. */
   /* EVERY NUMBER IS READ INTO A long AND RANGE-CHECKED before it is
    * narrowed to an int: a value past an int's range would otherwise wrap
    * into a valid one. */
   enum csv_field delok = CSV_FIELD_OK;
   enum csv_field uok   = CSV_FIELD_OK;
   enum csv_field idok  = CSV_FIELD_OK; /* 19 digits or more is damage */
   enum csv_field tok   = CSV_FIELD_OK;
   enum csv_field tyok  = CSV_FIELD_OK;
   long del             = 0;
   long type            = 0;
   long milli           = 0;
   if (ins_ncommas(p, e) >= 6) {
      /* EVERY FIELD ENDS AT ITS COMMA: "1.5" as a type would otherwise read
       * as 1, and its ".5" as the units. */
      (void)csv_num(&c, 0); /* written: ordering is FILE order, not this */
      if (!csv_sep(&c))
         return 0;
      out->id = csv_num(&c, &idok);
      if (!csv_sep(&c))
         return 0;
      del = csv_num(&c, &delok);
      if (!csv_sep(&c))
         return 0;
      out->t = csv_num(&c, &tok);
      if (!csv_sep(&c))
         return 0;
      type = csv_num(&c, &tyok);
      if (!csv_sep(&c))
         return 0;
      milli = csv_fixed(&c, 3, &uok);
      if (c.p < c.e && *c.p != ',')
         return 0; /* "20abc": the units field is not exactly a number */
      if (out->id == 0 || idok == CSV_FIELD_OVERFLOW ||
          delok != CSV_FIELD_OK || (del != 0 && del != 1))
         return 0;
      out->del = (int)del;
      if (out->del) {
         /* a retraction names a dose; it does not describe one */
         out->type  = 0;
         out->milli = 0;
         return 1;
      }
   } else {
      /* THE ORIGINAL FOUR-FIELD ROW: no id, so the caller's negative one by
       * file order. Positive ids are minted; the two spaces cannot collide
       * however the file was assembled. */
      out->id = legacy_id;
      out->t  = csv_num(&c, &tok);
      if (!csv_sep(&c))
         return 0;
      type = csv_num(&c, &tyok);
      if (!csv_sep(&c))
         return 0;
      milli = csv_fixed(&c, 3, &uok);
      if (c.p < c.e && *c.p != ',')
         return 0;
   }
   if (tok == CSV_FIELD_OVERFLOW || out->t <= 0 || out->t >= INS_T_MAX)
      return 0;
   if (tyok != CSV_FIELD_OK || (type != INS_SLOW && type != INS_FAST))
      return 0;
   if (uok == CSV_FIELD_OVERFLOW || milli < (long)INS_MILLI_MIN ||
       milli > (long)INS_MILLI_MAX)
      return 0;
   out->type  = (int)type;
   out->milli = (int)milli;
   return 1;
}

int ins_units_str(int milli, char *out, int cap)
{
   if (!out || cap < 2)
      return 0;
   int n   = 0;
   int neg = milli < 0;
   if (neg)
      milli = -milli;
   const int whole = milli / INS_MILLI;
   int frac        = milli % INS_MILLI;
   char b[16];
   int nb = 0;
   int w  = whole;
   do {
      b[nb++] = (char)('0' + (w % 10));
      w /= 10;
   } while (w && nb < (int)sizeof b);
   if (neg && n < cap - 1)
      out[n++] = '-';
   while (nb > 0 && n < cap - 1)
      out[n++] = b[--nb];
   if (frac) {
      /* NO TRAILING ZEROS: 500 is "0.5", not "0.500". The file says what a
       * person would write, and a reader that sees "0.500" learns nothing the
       * shorter form did not tell it. */
      char f[3];
      f[0]   = (char)('0' + (frac / 100));
      f[1]   = (char)('0' + ((frac / 10) % 10));
      f[2]   = (char)('0' + (frac % 10));
      int fl = 3;
      while (fl > 1 && f[fl - 1] == '0')
         fl--;
      if (n < cap - 1)
         out[n++] = '.';
      for (int i = 0; i < fl && n < cap - 1; i++)
         out[n++] = f[i];
   }
   out[n] = 0;
   return n;
}
