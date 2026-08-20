/* SPDX-License-Identifier: GPL-3.0
 * rowtest.c --- the row decoder, against the rows a network can deliver
 * Copyright 2026 Jakob Kastelic
 *
 * A reading row arrives over the network and is stored as generic text. Four
 * readers used to parse it independently -- the headline, the recent table,
 * the plot, the timezone probe -- each walking commas by hand and handing
 * what it found to strtol, which stops where it likes and reports the prefix
 * it managed. "12abc" was 12, "0junk" was a CGM reading, an empty field was
 * zero, and a row that stopped early was a row whose missing fields were
 * zero. So one page refused a row the next one drew, and a corrupt field
 * became a number on a graph where nobody could tell.
 *
 * There is one decoder now (srv/rowdec.h), and this is its vector table. Each
 * case says what a real writer or a real corruption would produce, and
 * whether it is a row at all.
 *
 * THE VECTORS ARE SHARED WITH THE APP'S END where they can be: the phone
 * writes these rows (app/store.c) and reads them back (store_load), so the
 * malformed shapes below are the same ones app/test/storetest.c feeds its
 * loader. Keeping the two lists in the same shape is deliberate -- when one
 * side learns to refuse something, the list says whether the other side does.
 *
 * IT ALSO HOLDS THE NUMBER GRAMMAR OF A PATH (srv/route.h's route_number), for
 * the one thing that parse shares with this one and with nothing else in the
 * server: both turn attacker-influenced text into a number that is then stored
 * or routed on, and both therefore have to refuse a value they cannot carry
 * rather than deliver a different one. The two rules have to agree about how
 * wide a decimal may be, so they are checked side by side. srv/test/wiretest.c
 * still owns route CLASSIFICATION against the permanent wire vectors; what is
 * here is the arithmetic underneath it.
 *
 * Built and run by `make rowtest`.
 */
#include "route.h"
#include "rowdec.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* One vector: the line, whether it should decode, and why it is here. */
struct vec {
   const char *line;
   int ok;
   const char *why;
};

/* ---- ONE ROW WITH ONE FIELD REPLACED ---------------------------------------
 *
 * The boundary cases below are INT_MAX, INT_MAX+1, LONG_MAX and their
 * negatives, and none of those can be written as a string literal without
 * hard-coding a number the platform is entitled to disagree about. So the row
 * is assembled from the limits themselves: what is checked is "the widest int
 * this build has", not "2147483647", and a build where those differ is checked
 * against its own boundary rather than against a remembered one.
 *
 * FIELDS, in the format's order (see rowdec.h):
 *   1 epoch  2 glucose  3 trend10  4 rssi  5 recv_lag
 *   6 source_id  7 raw_time  8 tz_off  9 kind */
#define F_EPOCH 1
#define F_GLU   2
#define F_TREND 3
#define F_SRC   6
#define F_TZ    8

static void row_with(char *out, size_t cap, int idx, const char *text)
{
   static const char *base[9] = {"1700000000", "120",    "3", "-70", "2", "7",
                                 "1699999999", "-25200", "0"};
   size_t n                   = 0;
   for (int i = 0; i < 9; i++) {
      const char *f = (i + 1 == idx) ? text : base[i];
      n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? "," : "", f);
   }
}

/* The row is refused, AND the caller's struct is exactly as it was.
 *
 * Both halves in one case on purpose: "it returned 0" is only half of what
 * row_decode promises, and a decoder that half-filled `out` on the way to
 * refusing would satisfy the first half while leaving the previous row's
 * values for the caller to draw. The two failures print differently so the
 * output says which promise broke. */
static void ck_refused(int idx, const char *text, const char *why)
{
   char line[512];
   row_with(line, sizeof line, idx, text);
   struct row_reading rr;
   memset(&rr, 0xEE, sizeof rr);
   struct row_reading before = rr;
   if (row_decode(line, (int)strlen(line), &rr)) {
      printf("  [FAIL] %s\n         ACCEPTED: \"%s\"\n", why, line);
      all = 0;
   } else if (memcmp(&before, &rr, sizeof rr) != 0) {
      printf("  [FAIL] %s\n         refused but WROTE to the output\n", why);
      all = 0;
   } else {
      ck(1, why);
   }
}

/* The row decodes, and `rr` is what it decoded to. */
static int row_at(struct row_reading *rr, int idx, const char *text,
                  const char *why)
{
   char line[512];
   row_with(line, sizeof line, idx, text);
   memset(rr, 0xEE, sizeof *rr);
   int got = row_decode(line, (int)strlen(line), rr);
   ck(got, why);
   return got;
}

/* epoch,glucose,trend10,rssi,recv_lag,source_id,raw_time,tz_off,kind[,rescale]
 */
static const struct vec V[] = {
    /* ---- what the writer really emits ---- */
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0",              1,
     "a CGM row exactly as the phone writes it"                                                                         },
    {"1700000000,120,3,,2,7,1700000000,-25200,0",                 1,
     "...with no RSSI: the one field the writer leaves empty"                                                           },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,1",              1,
     "a fingerstick row: a row, and it says so"                                                                         },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0,1.040",        1,
     "...and a trailing rescale factor, which a newer writer appends"                                                   },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0,1.040,7",      1,
     "...and a field this build has never heard of: the format may grow"                                                },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0\n",            1,
     "a trailing newline belongs to the storage, not to the row"                                                        },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0\r\n",          1,
     "...and so does a carriage return"                                                                                 },
    {"1700000000,120,-3,-70,2,7,1700000000,-25200,0",             1,
     "a falling trend is negative, and negative is a number"                                                            },

    /* ---- the prefix problem: strtol's answer is not the field's ---- */
    {"1700000000,120abc,3,-70,2,7,1700000000,-25200,0",           0,
     "a glucose with letters after it is not a glucose"                                                                 },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0junk",          0,
     "a kind of '0junk' is not KIND_CGM"                                                                                },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,0 ",             0,
     "...nor is '0 ': a trailing space is not a delimiter"                                                              },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,",               0,
     "...but an EMPTY kind field is a corruption, not an absence"                                                       },
    {" 1700000000,120,3,-70,2,7,1700000000,-25200,0",             0,
     "a leading space is not a digit"                                                                                   },
    {"1700000000,1 20,3,-70,2,7,1700000000,-25200,0",             0,
     "a space INSIDE a number splits it into two things"                                                                },
    {"1700000000,0x78,3,-70,2,7,1700000000,-25200,0",             0,
     "hexadecimal is not the format"                                                                                    },
    {"1700000000,+120,3,-70,2,7,1700000000,-25200,0",             0,
     "...and neither is an explicit plus"                                                                               },

    /* ---- rows that stop ---- */
    {"1700000000,120,3,-70,2,7,1700000000,-25200",                1,
     "a row that ends before the kind is still a reading..."                                                            },
    {"1700000000,120,3,-70,2",                                    0, "...or stops halfway"                              },
    {"1700000000,120",                                            0, "...or holds only a time and a number"             },
    {"1700000000",                                                0, "...or only a time"                                },
    {"",                                                          0, "an empty line is not a row"                       },
    {",,,,,,,,",                                                  0, "...and neither is a row of nothing but separators"},

    /* ---- the empty-field rule is RSSI's alone ---- */
    {"1700000000,,3,-70,2,7,1700000000,-25200,0",                 0,
     "an empty GLUCOSE is not a reading of zero"                                                                        },
    {",120,3,-70,2,7,1700000000,-25200,0",                        0, "an empty TIME is not the epoch"                   },
    {"1700000000,120,,-70,2,7,1700000000,-25200,0",               0,
     "an empty TREND is not a flat arrow"                                                                               },
    {"1700000000,120,3,-70,2,7,1700000000,,0",                    0,
     "an empty OFFSET is not UTC -- not knowing is a different answer"                                                  },

    /* ---- separators ---- */
    {"1700000000;120;3;-70;2;7;1700000000;-25200;0",              0,
     "semicolons are not this format"                                                                                   },
    {"1700000000 120 3 -70 2 7 1700000000 -25200 0",              0,
     "...and neither are spaces"                                                                                        },
    {"1700000000,,120,3,-70,2,7,1700000000,-25200,0",             0,
     "a doubled separator shifts every field after it"                                                                  },
    {"#device_time,glucose,trend",                                0, "the file's HEADER line is not a row"              },

    /* ---- values that parse but are not facts ---- */
    {"0,120,3,-70,2,7,1700000000,-25200,0",                       0, "a timestamp of zero"                              },
    {"-5,120,3,-70,2,7,1700000000,-25200,0",                      0, "...or before the epoch"                           },
    {"1700000000,14,3,-70,2,7,1700000000,-25200,0",               0,
     "a glucose below the band the phone itself stores"                                                                 },
    {"1700000000,751,3,-70,2,7,1700000000,-25200,0",              0, "...or above it"                                   },
    {"1700000000,15,3,-70,2,7,1700000000,-25200,0",               1,
     "the bottom of the band is IN it"                                                                                  },
    {"1700000000,750,3,-70,2,7,1700000000,-25200,0",              1, "...and so is the top"                             },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,4",              0,
     "a kind this build does not define is not a kind"                                                                  },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,-1",             0,
     "...nor is a negative one"                                                                                         },
    {"1700000000,120,3,-70,2,7,1700000000,3,0",                   1,
     "a zone three seconds east is odd but is a zone"                                                                   },
    {"1700000000,120,3,-70,2,7,1700000000,86400,0",               0,
     "a whole day east of UTC is not a timezone"                                                                        },
    {"1700000000,120,3,-70,2,7,1700000000,-86400,0",              0, "...nor a day west"                                },

    /* ---- the wrap that a cap prevents ---- */
    {"99999999999999999999999,120,3,-70,2,7,1700000000,-25200,0", 0,
     "a number too long to hold is refused, not wrapped into range"                                                     },

    /* ---- THE NARROWING THAT USED TO BE SILENT ------------------------------
     *
     * Three fields are `int` in struct row_reading and `long` on the way in,
     * and each used to make the trip by bare cast. 4294967296 is 2^32, so its
     * low thirty-two bits are ZERO: on every compiler this ships on the cast
     * produced a value that is not merely wrong but PLAUSIBLE, and two of the
     * three then passed every check downstream of it.
     *
     * These are the cases that isolate the narrowing check itself, because the
     * value they narrow to is INSIDE every other bound. A field of 2147483648
     * is also refused, but a reader cannot tell from that alone whether the
     * width check or something later did it -- see the boundary section in
     * main() for the pairs that separate them. */
    {"1700000000,120,4294967296,-70,2,7,1700000000,-25200,0",     0,
     "a trend of 2^32 is not a FLAT ARROW, which is what its low 32 bits say"                                           },
    {"1700000000,120,3,-70,2,4294967303,1700000000,-25200,0",     0,
     "a source id of 2^32+7 is not DEVICE 7: a reading may not change hands"                                            },
    {"1700000000,120,3,-70,2,7,1700000000,4294967296,0",          0,
     "an offset of 2^32 is not UTC -- the old range check ran on the CAST"                                              },
    {"1700000000,120,3,-70,2,7,1700000000,4294970896,0",          0,
     "...nor is 2^32 plus an hour one hour east of it"                                                                  },
    {"1700000000,120,3,-70,2,7,1700000000,-4294967296,0",         0,
     "...and neither is minus 2^32"                                                                                     },

    /* THE KIND WAS ALREADY BOUNDED BEFORE ITS CAST -- and nothing said so.
     *
     * A mutation study moved that bound to the far side of the cast, the way
     * the offset's had been, and every other case in this file went on
     * passing. What it buys is the worst of the four: the kind decides which
     * LOG a row is, so 2^32 arrives as kind 0 and a corrupt row becomes a CGM
     * glucose, while 2^32+2 arrives as kind 2 and becomes an INSULIN DOSE --
     * a dose nobody administered, on the page a person reads before deciding
     * whether to inject. */
    {"1700000000,120,3,-70,2,7,1700000000,-25200,4294967296",     0,
     "a kind of 2^32 is not KIND_CGM, which its low 32 bits spell"                                                      },
    {"1700000000,120,3,-70,2,7,1700000000,-25200,4294967298",     0,
     "...and a kind of 2^32+2 is CERTAINLY not an insulin dose"                                                         },

    /* ---- the offset band, from the inside ---- */
    /* The pair above needs these: without a case that ACCEPTS an offset one
     * second inside the band, a decoder that refused every offset would pass
     * every refusal vector here. */
    {"1700000000,120,3,-70,2,7,1700000000,86399,0",               1,
     "a second short of a whole day east is still a zone"                                                               },
    {"1700000000,120,3,-70,2,7,1700000000,-86399,0",              1,
     "...and so is a second short of one west"                                                                          },

    /* ---- a sign with no number behind it ---- */
    {"1700000000,120,-,-70,2,7,1700000000,-25200,0",              0,
     "a lone '-' in the trend is not a trend of zero"                                                                   },
    {"1700000000,-,3,-70,2,7,1700000000,-25200,0",                0,
     "...and not a glucose of zero either"                                                                              },

    /* ---- leading zeros: ACCEPTED HERE, and refused in a path ---------------
     *
     * srv/route.h's route_number refuses "01" because a route with two
     * spellings is a route whose logs count something other than what they
     * name. This decoder does not, and the difference is deliberate: a row is
     * identified by its BYTES (the sync digests hash the line, not the parsed
     * fields), so "0120" and "120" are already two different rows to both ends
     * of the protocol and agree with each other about it. Refusing the
     * spelling here would make the server stop reading rows it is already
     * storing, which is the one thing rowdec.h promises not to do. Pinned as
     * a vector so the difference is a decision on the record rather than an
     * oversight somebody later "fixes" in one direction. */
    {"1700000000,0120,3,-70,2,7,1700000000,-25200,0",             1,
     "a leading zero is not a second spelling of a ROW, only of a route"                                                },
};

int main(void)
{
   printf("rowtest: one decoder, and a row decodes whole or not at all\n");
   int bad = 0;
   for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
      struct row_reading rr;
      memset(&rr, 0xEE, sizeof rr);
      int got = row_decode(V[i].line, (int)strlen(V[i].line), &rr);
      if (got != V[i].ok) {
         printf("  [FAIL] %s\n         line: \"%s\" -> %s\n", V[i].why,
                V[i].line, got ? "accepted" : "refused");
         bad++;
      }
   }
   ck(bad == 0, "every vector decodes exactly as the format says");

   /* THE FIELDS THEMSELVES, once, so "it decoded" is not the whole claim: a
    * decoder that accepted everything and filled nothing would pass the table
    * above for the rows that are supposed to decode. */
   struct row_reading rr;
   static const char one[] = "1700000000,120,3,-70,2,7,1699999999,-25200,1";
   ck(row_decode(one, (int)strlen(one), &rr), "a complete row decodes");
   ck(rr.t == 1700000000, "...the time is the first field");
   ck(rr.glu == 120, "...the glucose is the second");
   ck(rr.trend == 3, "...the trend is the third");
   ck(rr.src == 7, "...the source id is the sixth");
   ck(rr.tz == -25200, "...the offset is the eighth, in SECONDS");
   ck(rr.kind == ROW_KIND_BGM, "...and the kind is the ninth");

   /* THE UNTYPED ROW, reported as saying nothing rather than as saying CGM.
    * Guessing here is what let an untyped row become the headline. */
   static const char untyped[] = "1700000000,120,3,-70,2,7,1699999999,-25200";
   ck(row_decode(untyped, (int)strlen(untyped), &rr),
      "a row from before the kind column still decodes");
   ck(rr.kind == ROW_KIND_NONE, "...and reports that it does not say");
   ck(rr.glu == 120 && rr.t == 1700000000,
      "...with everything it DOES say intact");

   /* A REFUSED ROW LEAVES THE OUTPUT ALONE. Callers reuse one struct across a
    * scan of thousands of rows; a decoder that half-filled it on the way to
    * refusing would leave the previous row's values behind, and the caller
    * would draw them again. */
   struct row_reading keep = rr;
   ck(!row_decode("1700000001,999", 14, &rr), "a short row is refused");
   ck(memcmp(&keep, &rr, sizeof rr) == 0,
      "...and it did not touch what the caller already had");

   /* Bounds: the length is the length, whatever follows it in memory. */
   ck(!row_decode(one, 10, &rr),
      "a line cut short by its LENGTH is short, whatever is after it");
   ck(!row_decode(NULL, 10, &rr), "no line is not a row");
   ck(!row_decode(one, 0, &rr), "...and neither is a line of no length");

   /* ---- THE EXACT EDGE OF EVERY FIELD THAT NARROWS ------------------------
    *
    * A range check is only as good as the two cases either side of its bound,
    * and the ACCEPTING one is the half that is usually missing: a decoder that
    * refused every large number would pass every refusal above and every
    * refusal below. So each bound is pinned from both sides, and the accepting
    * side checks the VALUE -- INT_MAX has to arrive as INT_MAX, not as
    * something that merely got through.
    *
    * trend and source_id have NO semantic bound today: whatever fits an int is
    * a trend and whatever fits an int is a registry id. That is what makes
    * them the fields that isolate the width check, and it is also why the
    * cases below are the only thing standing between a stored source id and
    * the low thirty-two bits of something else. */
   char num[64];
   printf("-- the trend, at the edge of the int it is stored in --\n");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX);
   if (row_at(&rr, F_TREND, num, "a trend of exactly INT_MAX is a trend"))
      ck(rr.trend == INT_MAX, "...and it arrives as INT_MAX, not as anything "
                              "else that fits");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX + 1);
   ck_refused(F_TREND, num, "...and one past INT_MAX is not a trend at all");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN);
   if (row_at(&rr, F_TREND, num, "a trend of exactly INT_MIN is a trend"))
      ck(rr.trend == INT_MIN, "...and it arrives as INT_MIN");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN - 1);
   ck_refused(F_TREND, num, "...and one below INT_MIN is not");

   printf("-- the source id, which decides WHOSE reading this is --\n");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX);
   if (row_at(&rr, F_SRC, num, "a source id of exactly INT_MAX is an id"))
      ck(rr.src == INT_MAX, "...and it arrives as INT_MAX");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX + 1);
   ck_refused(F_SRC, num, "...and one past INT_MAX names no device");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN);
   if (row_at(&rr, F_SRC, num, "a source id of exactly INT_MIN is an id"))
      ck(rr.src == INT_MIN, "...and it arrives as INT_MIN");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN - 1);
   ck_refused(F_SRC, num, "...and one below INT_MIN names none either");

   /* THE OFFSET'S int BOUNDARY IS NOT ISOLATING, and saying so is the point.
    *
    * An offset of INT_MAX fits an int perfectly and is still refused, by the
    * +/- one day band and not by the width check -- so this case would go on
    * passing with the width check deleted. It is here because the item asks
    * for the boundary on every narrowed field, not because it pins anything
    * the band does not already pin. The cases that DO isolate the offset's
    * width check are the 2^32 family in the vector table above, whose narrowed
    * value lands inside the band. */
   printf("-- the offset: at the int edge, and refused by the BAND --\n");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX);
   ck_refused(F_TZ, num,
              "an offset of INT_MAX is not a timezone (the band "
              "refuses it, not the width)");
   snprintf(num, sizeof num, "%ld", (long)INT_MAX + 1);
   ck_refused(F_TZ, num, "...nor is one past INT_MAX");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN);
   ck_refused(F_TZ, num, "...nor INT_MIN");
   snprintf(num, sizeof num, "%ld", (long)INT_MIN - 1);
   ck_refused(F_TZ, num, "...nor one below it");

   /* ---- HOW WIDE A DECIMAL FIELD MAY BE ----------------------------------
    *
    * Eighteen digits, because that is the widest decimal that cannot overflow
    * a 64-bit long during accumulation -- and the accumulation happens BEFORE
    * any range check could reject the row, so a cap is the only thing that can
    * prevent the undefined behaviour rather than notice it afterwards.
    *
    * PINNED ON THE TIMESTAMP, which is the only field wide enough to show it:
    * `t` is a long all the way through and its only bound is `t > 0`, so an
    * eighteen-digit value there is genuinely accepted and a nineteen-digit one
    * is refused BY THE CAP. The same experiment on the trend would prove
    * nothing -- a nineteen-digit trend is refused whatever the cap is,
    * because it does not fit an int. */
   printf("-- eighteen digits, and the nineteenth --\n");
   if (row_at(&rr, F_EPOCH, "100000000000000000",
              "an eighteen-digit timestamp is a number this format carries"))
      ck(rr.t == 100000000000000000L, "...and it arrives whole");
   snprintf(num, sizeof num, "%ld", LONG_MAX);
   ck_refused(F_EPOCH, num,
              "LONG_MAX is nineteen digits, so it is refused even though a "
              "long holds it");
   snprintf(num, sizeof num, "%ld", LONG_MIN);
   ck_refused(F_EPOCH, num,
              "...and LONG_MIN likewise (the sign is not a "
              "digit, the nineteen digits are)");
   ck_refused(F_EPOCH, "99999999999999999999999",
              "and a twenty-three digit run is not a very large timestamp");
   /* The same cap on a field that is not the timestamp, so the cap is not
    * something only `field`'s first caller enforces. */
   ck_refused(F_GLU, "1000000000000000000",
              "nineteen digits is nineteen digits in the glucose field too");

   /* ---- THE NUMBER IN A PATH (srv/route.h) -------------------------------
    *
    * THREE ANSWERS, and the whole reason route_number is declared rather than
    * static: route_of collapses OVERFLOW and BAD into one status because the
    * wire has one status for both, so a test that only ever called route_of
    * could not tell which rule refused a path -- and a rule nothing can
    * distinguish is a rule that can be deleted without any gate noticing. */
   printf("-- the one number a path may contain --\n");
   {
      long v;
      /* Numbers, and the values they are. */
      v = -1;
      ck(route_number("0", &v) == ROUTE_NUM_OK && v == 0,
         "\"0\" is the number zero -- the bucket every unbucketed log uses");
      v = -1;
      ck(route_number("20000", &v) == ROUTE_NUM_OK && v == 20000,
         "\"20000\" is the bucket the wire vectors use");
      v = -1;
      ck(route_number("999999999999999999", &v) == ROUTE_NUM_OK &&
             v == 999999999999999999L,
         "eighteen digits is a number a path may carry");

      /* TOO WIDE, which is not the same answer as malformed. */
      v = -1;
      ck(route_number("1000000000000000000", &v) == ROUTE_NUM_OVERFLOW &&
             v == -1,
         "nineteen digits is OVERFLOW, and nothing was written");
      v = -1;
      ck(route_number("9223372036854775807", &v) == ROUTE_NUM_OVERFLOW,
         "...LONG_MAX itself included: the cutoff is the format's, not the "
         "compiler's");
      v = -1;
      ck(route_number("9223372036854775808", &v) == ROUTE_NUM_OVERFLOW &&
             v == -1,
         "...and LONG_MAX plus one, which strtol used to report as LONG_MAX "
         "with ERANGE nobody read");
      v = -1;
      ck(route_number("99999999999999999999999", &v) == ROUTE_NUM_OVERFLOW,
         "...and a twenty-three digit run");

      /* NOT A NUMBER AT ALL. Each of these is inside the width cutoff, so the
       * cutoff cannot be what refuses them -- that is what makes them pin the
       * spelling rules rather than accidentally re-testing the width. */
      v = -1;
      ck(route_number("", &v) == ROUTE_NUM_BAD && v == -1,
         "an empty field is not a number, and wrote nothing");
      v = -1;
      ck(route_number("+1", &v) == ROUTE_NUM_BAD,
         "\"+1\" is not 1: a sign is not a digit");
      v = -1;
      ck(route_number("-1", &v) == ROUTE_NUM_BAD,
         "...and neither is a minus, so a bucket cannot be negative");
      v = -1;
      ck(route_number("-", &v) == ROUTE_NUM_BAD,
         "...and a sign with nothing behind it is nothing");
      v = -1;
      ck(route_number(" 1", &v) == ROUTE_NUM_BAD,
         "a leading space is not skipped here, whatever strtol does");
      v = -1;
      ck(route_number("1 ", &v) == ROUTE_NUM_BAD, "...nor a trailing one");
      v = -1;
      ck(route_number("1x", &v) == ROUTE_NUM_BAD,
         "...and trailing text is not a number followed by nothing");
      v = -1;
      ck(route_number("01", &v) == ROUTE_NUM_BAD,
         "\"01\" is a decorated 1, not a second name for it");

      /* THE ORDER OF THE TWO REFUSALS, which is a decision and not an
       * accident: a field of leading zeros longer than the cutoff is a
       * DECORATED number, and calling it OVERFLOW would name the wrong
       * fault -- there is no large number in it to overflow. */
      v = -1;
      ck(route_number("0000000000000000000001", &v) == ROUTE_NUM_BAD,
         "twenty-two characters of decorated 1 is BAD, not OVERFLOW");
   }

   /* ---- AND WHAT route_of DOES WITH IT ----------------------------------- */
   printf("-- a bucket path, at the ceiling and past it --\n");
   {
      struct route rt;
      char path[128];
      route_of("/v1/bucket/glucose/0", &rt);
      ck(rt.kind == RT_BUCKET && rt.bucket == 0,
         "bucket 0 is a bucket: every log the app does not bucket lives there");

      /* THE CEILING, SPELLED OUT AS A NUMBER AND NOT AS ITS OWN NAME.
       *
       * Every other case here builds its path from ROUTE_BUCKET_MAX, which
       * means moving the ceiling moves the test with it and the whole section
       * goes on passing -- a mutation study found exactly that. The number
       * comes from OUTSIDE this header: it is the cap srv/logs.c's
       * h_bucket_put has always refused to exceed, and the reason route.h
       * enforces it is so a GET and a PUT of the same path agree. If that cap
       * ever changes, these three lines are what says so out loud. */
      ck(ROUTE_BUCKET_MAX == 0x7fffffffL,
         "the ceiling is the number srv/logs.c's h_bucket_put enforces");
      route_of("/v1/bucket/glucose/2147483647", &rt);
      ck(rt.kind == RT_BUCKET && rt.bucket == 2147483647L,
         "...so 2147483647 is a bucket, written out in full");
      route_of("/v1/bucket/glucose/2147483648", &rt);
      ck(rt.kind == RT_BUCKET_BAD, "...and 2147483648 is not");

      snprintf(path, sizeof path, "/v1/bucket/glucose/%ld", ROUTE_BUCKET_MAX);
      route_of(path, &rt);
      ck(rt.kind == RT_BUCKET && rt.bucket == ROUTE_BUCKET_MAX &&
             !strcmp(rt.log, "glucose"),
         "the highest bucket a PUT can create is still a route");

      /* THE TWO REFUSALS, SIDE BY SIDE. Same status out of route_of, and the
       * assertion under each says which rule got there -- so neither can be
       * deleted while the other keeps the gate green. */
      snprintf(path, sizeof path, "/v1/bucket/glucose/%ld",
               ROUTE_BUCKET_MAX + 1);
      route_of(path, &rt);
      ck(rt.kind == RT_BUCKET_BAD,
         "one past the ceiling is a malformed bucket request (400)");
      ck(rt.bucket == 0 && rt.log[0] == '\0',
         "...and route_of wrote neither the bucket nor the log name");
      {
         long v = -1;
         char digits[32];
         snprintf(digits, sizeof digits, "%ld", ROUTE_BUCKET_MAX + 1);
         ck(route_number(digits, &v) == ROUTE_NUM_OK &&
                v == ROUTE_BUCKET_MAX + 1,
            "...and it got there by the CEILING: the number itself parsed "
            "fine");
      }
      route_of("/v1/bucket/glucose/99999999999999999999", &rt);
      ck(rt.kind == RT_BUCKET_BAD,
         "a twenty-digit bucket is also a malformed bucket request");
      ck(rt.bucket == 0 && rt.log[0] == '\0', "...and wrote nothing either");
      {
         long v = -1;
         ck(route_number("99999999999999999999", &v) == ROUTE_NUM_OVERFLOW,
            "...but it got there by the WIDTH CUTOFF, which is a different "
            "rule");
      }
      /* THE ALIASES THAT USED TO EXIST. Every one of these was bucket
       * 9223372036854775807 -- strtol's saturated answer, delivered with
       * ERANGE set and never read -- and the GET side answered each of them
       * 200 with an empty body. */
      route_of("/v1/bucket/glucose/9223372036854775807", &rt);
      ck(rt.kind == RT_BUCKET_BAD, "LONG_MAX is not a bucket");
      route_of("/v1/bucket/glucose/9223372036854775808", &rt);
      ck(rt.kind == RT_BUCKET_BAD, "...nor is LONG_MAX plus one");
      route_of("/v1/bucket/glucose/99999999999999999999999999", &rt);
      ck(rt.kind == RT_BUCKET_BAD,
         "...nor twenty-six nines, which used to be the SAME bucket as both");

      /* The pairing round, where an unusable number is a 404 rather than a
       * 400: it was already refused for being greater than 4, but only
       * because strtol happened to saturate upwards. */
      route_of("/v1/pair/9223372036854775808", &rt);
      ck(rt.kind == RT_NONE && rt.round == 0,
         "a pairing round too wide to hold is not a round");
      route_of("/v1/pair/1", &rt);
      ck(rt.kind == RT_PAIR && rt.round == 1, "...and round 1 still is one");

      /* WHICH SIDE OF THE CAST THE BOUND IS ON, which is the whole of why
       * route_of's `(int)round` is safe. 4294967297 is 2^32 + 1, so its low
       * thirty-two bits are 1: a version of this function that narrowed first
       * and bounded the int afterwards -- which is precisely what
       * srv/rowdec.c had done to its offset -- would call this round 1 and
       * dispatch it to the ONE endpoint served without a signature. Nothing
       * else in this file catches that reordering; a mutation study found the
       * claim in route.c's comment standing entirely unchecked. */
      route_of("/v1/pair/4294967297", &rt);
      ck(rt.kind == RT_NONE && rt.round == 0,
         "2^32+1 is not round 1: the bound is on the PARSED value, not the "
         "cast");
      route_of("/v1/pair/4294967298", &rt);
      ck(rt.kind == RT_NONE && rt.round == 0, "...and 2^32+2 is not round 2");
   }

   printf("%s\n", all ? "rowtest: the same row means the same thing to every "
                        "reader"
                      : "rowtest: FAILED");
   return all ? 0 : 1;
}
