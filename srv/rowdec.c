/* SPDX-License-Identifier: GPL-3.0
 * rowdec.c --- the ONE decoder for a persisted reading row (see rowdec.h)
 * Copyright 2026 Jakob Kastelic
 */

#include "rowdec.h"
#include <limits.h> /* INT_MIN, INT_MAX: where the narrowing checks below stop */

/* One field, consumed WHOLE.
 *
 * `p` walks forward; on success it stops on the separator that ended the
 * field (or at the end of the line). Returns 1 when the field held at least
 * one digit and NOTHING ELSE -- which is the difference between this and
 * strtol, and the whole reason this file exists: strtol("12abc") is 12 and
 * strtol("") is 0, so a corrupt field and a real value were indistinguishable
 * at every call site.
 *
 * A field too LONG is refused rather than capped. Unbounded accumulation is
 * undefined behaviour and it happens during parsing, before any range check
 * can reject anything -- so the digits are counted. The app's loaders stop
 * accumulating and keep the prefix, which is right for a file the phone
 * wrote itself; here the bytes came off a network, and a 23-digit field is
 * not a number this format can carry. Refuse it.
 *
 * EIGHTEEN, the same number as app/csvcur.h's CSV_MAX_DIGITS, app/sync.c's
 * digest_num and srv/route.h's ROUTE_NUM_DIGITS: it is the widest decimal that
 * cannot overflow a 64-bit long, and one repository should have one answer to
 * "how wide may a decimal field be". LONG_MAX itself is nineteen digits and is
 * therefore refused -- deliberately, because the cutoff describes the FORMAT
 * and must not move when the compiler's long does. */
#define ROW_DIGITS_MAX 18

static int field(const char **p, const char *end, int64_t *out)
{
   const char *q = *p;
   int neg       = 0;
   if (q < end && *q == '-') {
      neg = 1;
      q++;
   }
   int64_t v = 0;
   int nd    = 0;
   while (q < end && *q >= '0' && *q <= '9') {
      if (nd < ROW_DIGITS_MAX)
         v = (v * 10) + (*q - '0');
      nd++;
      q++;
   }
   if (!nd)
      return 0; /* empty, or not a number at all */
   if (nd > ROW_DIGITS_MAX)
      return 0; /* longer than this format can carry: not a truncation */
   /* It must END here: at the separator, or at the end of the line. Anything
    * else means there was more in the field than a number. */
   if (q < end && *q != ',')
      return 0;
   *out = neg ? -v : v;
   *p   = q;
   return 1;
}

/* A PARSED long BECOMING A STORED int, or the row is not a row.
 *
 * Three of this format's fields are `int` in struct row_reading and `long` on
 * the way in, and none of them makes the trip by cast alone: `(int)v`.
 * A cast from a long that an int cannot hold is implementation-defined -- on
 * every compiler this ships on it is the low 32 bits, which is the worst
 * possible answer because it is a PLAUSIBLE one. What that looks like to
 * somebody reading the stored data:
 *
 *   - a row whose trend field said 4294967296 was stored, drawn and
 *     summarised as trend 0: a flat arrow. Not a gap in the plot, not a
 *     refused row -- an arrow pointing sideways, indistinguishable from a
 *     glucose that genuinely was not moving.
 *   - a row whose source_id said 4294967303 was attributed to device 7. If
 *     device 7 is the user's real G7 then a reading from nowhere is filed
 *     under it, appears in its history, and counts toward its statistics. The
 *     registry has no way to notice: it was handed the id 7.
 *   - a row whose tz_off said 4294967296 became offset 0, and offset 0 passed
 *     the timezone range check because THE CHECK RAN ON THE NARROWED VALUE
 *     (see the range block at the bottom of row_decode: `r.tz` was already an
 *     int by then). So a field that said four billion was accepted as UTC and
 *     every timestamp on the settings page was rendered in it. 4294967296 plus
 *     3600 was accepted as one hour east.
 *
 * The right shape was already in this file, one field over: the KIND is range
 * checked as a long and cast afterwards. This does the same for the rest.
 *
 * INT_MIN..INT_MAX and not a tighter bound: this function's only claim is
 * that the int holds the number that was written. What a trend, a source id
 * or an offset is ALLOWED to be is a separate question answered per field --
 * by the ranges at the bottom of row_decode for the offset, and by nothing at
 * all today for the trend and the source id. Folding a semantic bound in here
 * would put three different rules behind one name. */
static int narrow(int64_t v, int *out)
{
   if (v < INT_MIN || v > INT_MAX)
      return 0;
   *out = (int)v;
   return 1;
}

/* Step over the separator between two fields. 0 when there is none, which
 * means the row STOPPED -- a shorter row is not a row with zeroes in it. */
static int sep(const char **p, const char *end)
{
   if (*p >= end || **p != ',')
      return 0;
   (*p)++;
   return 1;
}

int row_decode(const char *line, int len, struct row_reading *out)
{
   if (!line || !out || len <= 0)
      return 0;
   const char *p   = line;
   const char *end = line + len;
   /* A trailing newline is part of the storage, not of the row. */
   while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
      end--;

   struct row_reading r = {0};
   int64_t v;

   if (!field(&p, end, &v) || !sep(&p, end))
      return 0;
   r.t = v;
   if (!field(&p, end, &v) || !sep(&p, end))
      return 0;
   r.glu = v;
   if (!field(&p, end, &v) || !sep(&p, end))
      return 0;
   if (!narrow(v, &r.trend))
      return 0;
   /* RSSI, the one field that may be EMPTY: the writer emits nothing there
    * when the sample carried no signal strength (app/store.c). Empty is
    * therefore a value here and nowhere else. */
   if (p < end && *p != ',' && !field(&p, end, &v))
      return 0;
   if (!sep(&p, end))
      return 0;
   if (!field(&p, end, &v) || !sep(&p, end)) /* recv_lag: diagnostics only */
      return 0;
   if (!field(&p, end, &v) || !sep(&p, end))
      return 0;
   if (!narrow(v, &r.src))
      return 0;
   if (!field(&p, end, &v) || !sep(&p, end)) /* raw device time */
      return 0;
   if (!field(&p, end, &v))
      return 0;
   /* NARROWED HERE, RANGE-CHECKED BELOW, and both are needed. The range check
    * at the bottom (+/- a day) is far tighter than an int, so it looks like it
    * subsumes this one -- it does not: run AFTER the cast it judges the
    * truncated value. This refuses the offset that an int cannot hold; the
    * range below refuses the offset that a clock cannot
    * have. Deleting either one puts a wrong wall clock on the page. */
   if (!narrow(v, &r.tz))
      return 0;
   /* THE KIND, WHICH MAY SIMPLY NOT BE THERE. A row that ends here was
    * written before the column existed; it is still a reading, and it is
    * reported as saying nothing rather than as saying CGM. A row that HAS the
    * field must state a kind this build defines -- an empty or malformed one
    * is a corruption, not an absence. */
   if (!sep(&p, end)) {
      if (p != end)
         return 0; /* something after the offset that is not a separator */
      r.kind = ROW_KIND_NONE;
   } else {
      if (!field(&p, end, &v))
         return 0;
      /* Checked HERE, against what the field actually said, and not with the
       * ranges below: ROW_KIND_NONE is -1, so a row whose kind field really
       * says "-1" would otherwise pass as an absent kind -- a corrupt row
       * wearing the one value that means "this row is old". */
      if (v < 0 || v > ROW_KIND_MAX)
         return 0;
      /* A bare cast, and correct: the bound one line up is 0..3, so the value
       * is inside an int on any conforming platform. narrow() would be dead
       * code here -- a range check that can never fire is a check nobody can
       * test, which is worse than the comment saying why there is none. */
      r.kind = (int)v;
   }
   /* Anything after the kind is provenance this build may not know about --
    * today the rescale factor. It must be SEPARATED (the field ended at a
    * comma or at the end of the line, which `field` already required), and
    * beyond that it is not this reader's business: refusing it would mean an
    * older build stops reading rows a newer one writes. */

   /* ---- the ranges. A field that parsed is not yet a fact. ---- */
   /* `t` and `glu` are longs the whole way through, so nothing narrows them
    * and there is no truncation to catch. Their only upper bound is the digit
    * cutoff: an eighteen-digit timestamp is accepted here as a positive
    * number, and srv/page.c's stamp_local prints "(unknown time)" for it
    * because gmtime_r refuses a time_t it cannot turn into a date. That is a
    * survivable answer rather than a correct one, and giving `t` a real
    * ceiling is a semantic rule this item did not carry -- see the report. */
   if (r.t <= 0)
      return 0;
   if (r.glu < ROW_GLU_MIN || r.glu > ROW_GLU_MAX)
      return 0;
   /* (the kind was checked where it was read: see above) */
   /* The offset is minutes-east expressed in seconds; anything past a day is
    * not a timezone, and it is used to render a wall clock. */
   if (r.tz <= -86400 || r.tz >= 86400)
      return 0;
   *out = r;
   return 1;
}
