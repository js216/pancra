// SPDX-License-Identifier: GPL-3.0
// csvcur.h --- reading fields out of one line of a persisted CSV row
// Copyright 2026 Jakob Kastelic

/* THE CURSOR, NOT THE SCHEMA.
 *
 * Three files here persist append-only CSV that is loaded at every launch and
 * never rewritten: insulin doses, weights, and sensor provenance. Each of them
 * had its own copy of the same two readers -- a bounded signed-decimal reader
 * and a separator step -- and insulin.c and weight.c said so in a comment
 * ("same shape as sensors.c") while keeping a private copy anyway.
 *
 * THE COPIES HAD ALREADY DRIFTED, which is the whole argument. sensors.c grew
 * a version that reports whether the field held any digits at all, because an
 * empty field reading as 0 is indistinguishable from a genuine zero -- and for
 * that file a zero `activation` means "session start unknown", a real and
 * common value. insulin.c and weight.c never got that: their readers still
 * answer 0 for an absent field, a field of letters, and a written zero alike.
 * A malformed row therefore means something different in each of the three
 * formats, decided by which copy happened to be improved.
 *
 * So the cursor lives here, ONCE, and answers precisely:
 *
 *   CSV_FIELD_OK        digits were there and the value is all of them
 *   CSV_FIELD_EMPTY     no digits at all -- absent, or not a number
 *   CSV_FIELD_OVERFLOW  more digits than the cap holds; the value returned is
 *                       NOT the number that was written, it is its leading
 *                       digits, which describes a different row entirely
 *
 * WHAT IS NOT HERE is every rule about what the fields MEAN: how many columns
 * a row has, which are optional, what ranges are plausible, whether an unknown
 * value is a rejection or a row from a newer schema. Those differ per format
 * and belong beside the records they describe -- sharing them is how a
 * loosened bound on one file quietly loosens another.
 *
 * Header-only because these are a few lines each and the app links every
 * translation unit into one library; a .c file would buy nothing and cost an
 * entry in four build lists. */
/* IN lib/, NOT app/. The insulin log's decoder is shared with the
 * server -- one format, one reader -- and lib/ is the only place both builds
 * compile from. Nothing about this cursor was ever app-specific: it is a
 * bounded field reader over one line of text. */
#ifndef PANCRA_CSVCUR_H
#define PANCRA_CSVCUR_H

/* The digit cap. Accumulating without one is undefined behaviour on overflow,
 * and it happens during PARSING -- before any range check the caller would
 * have written to reject the row can run. Eighteen digits is the widest
 * decimal
 * that cannot overflow a 64-bit long. */
#define CSV_MAX_DIGITS 18

enum csv_field {
   CSV_FIELD_OK       = 0,
   CSV_FIELD_EMPTY    = 1,
   CSV_FIELD_OVERFLOW = 2
};

/* A position in one line. `e` is the end of the LINE, not the file: the caller
 * has already split on '\n', so no reader here can run past a row. */
struct csv_cur {
   const char *p;
   const char *e;
};

static inline void csv_open(struct csv_cur *c, const char *p, const char *e)
{
   c->p = p;
   c->e = e;
}

/* Read a signed decimal. `why` may be NULL when the caller's own range checks
 * are known to reject everything the answer would tell it -- but that is a
 * claim about the schema, so it is the caller's to make explicitly. */
static inline long csv_num(struct csv_cur *c, enum csv_field *why)
{
   long v        = 0;
   int nd        = 0;
   int neg       = 0;
   const char *q = c->p;
   if (q < c->e && *q == '-') {
      neg = 1;
      q++;
   }
   /* AFTER the sign, so the count below is digits alone. Starting the span at
    * the '-' instead counts the sign as though it were a digit, and that gets
    * BOTH answers below wrong in the same stroke. An eighteen-digit negative
    * -- a value this reader holds exactly, and the widest one it can -- spans
    * nineteen bytes and would be called OVERFLOW, so a number that was read
    * perfectly is reported as one that was not. And a field holding nothing
    * but a '-' spans one byte, so `q == digits` is false and a sign with no
    * number behind it is reported OK with the value 0 -- which is precisely
    * the "an absent field is indistinguishable from a written zero" confusion
    * this reader exists to end. */
   const char *digits = q;
   while (q < c->e && *q >= '0' && *q <= '9') {
      if (nd < CSV_MAX_DIGITS) {
         v = (v * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   if (why) {
      if (q == digits)
         *why = CSV_FIELD_EMPTY;
      else if ((q - digits) > CSV_MAX_DIGITS)
         *why = CSV_FIELD_OVERFLOW;
      else
         *why = CSV_FIELD_OK;
   }
   c->p = q;
   return neg ? -v : v;
}

/* Step over one field separator. 1 when there WAS one.
 *
 * A missing separator stepped over silently turns a row with fields run
 * together -- or truncated halfway -- into a SHORTER row holding whatever the
 * remaining text yields, which then parses and is accepted. Whether that
 * matters is the format's business; knowing it happened is not optional. */
static inline int csv_sep(struct csv_cur *c)
{
   if (c->p < c->e && *c->p == ',') {
      c->p++;
      return 1;
   }
   return 0;
}

/* Copy up to the next ',' (or the end of the line) into dst, always
 * NUL-terminated. Truncation is silent here on purpose: a text field that is
 * too long for its destination is the format's problem to size, and every
 * caller today sizes dst from the record it is filling. */
static inline void csv_str(struct csv_cur *c, char *dst, int n)
{
   const char *q = c->p;
   int k         = 0;
   while (q < c->e && *q != ',') {
      if (k < n - 1)
         dst[k++] = *q;
      q++;
   }
   dst[k] = 0;
   c->p   = q;
}

/* Everything the row had to offer has been read. */
static inline int csv_at_end(const struct csv_cur *c)
{
   return c->p >= c->e;
}

#endif
