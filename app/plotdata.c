// SPDX-License-Identifier: GPL-3.0
// plotdata.c --- long plot spans, read from the log (see plotdata.h)
// Copyright 2026 Jakob Kastelic
#include "plotdata.h"
#include "csvcur.h"  /* the shared CSV cursor; the grammar stays here */
#include "ingest.h"  /* STORE_GLU_MAX: the plot must be able to show it */
#include "sensors.h" /* KIND_CGM */
#include "uimodel.h"
#include <stddef.h>
/* The app is freestanding and declares its own syscalls (dexlibc.h); the
 * host test build has the real headers. Same code either way -- this file
 * exists to BE testable off the phone. */
#include <stdio.h> /* SEEK_SET / SEEK_END */
#ifdef PLOTDATA_HOST
#include <fcntl.h>
#include <unistd.h>
#else
#include "dexlibc.h"
#endif

#define PCOL_MAX PLOT_COLS

/* A plot is TWO dimensional: one column of pixels can show many readings at
 * different heights, so collapsing a column to one sample (or even to its
 * min and max) throws away nearly everything. Keep every reading that lands
 * on a DISTINCT CELL -- its x column and its own value -- and drop only what
 * would paint the same pixel twice. Memory is then bounded by the screen's
 * cell count, not by how much history exists or how many devices log it. */
/* Value rows. This MUST exceed STORE_GLU_MAX, because the filter below drops
 * any reading at or above it -- and a dropped reading is not clamped to the
 * top of the plot, it is simply absent from it.
 *
 * It read 512, with the comment "0..511 mg/dL covers the whole scale", and
 * that was false: store.h admits up to 750 (a 600 live reading times the +25%
 * rescale clamp). So a severe high was stored, alarmed on and counted in
 * time-in-range, then silently missing from the 30- and 90-day plots -- the
 * reading you would most want to see, absent from the view you would open to
 * see it. `make crosscheck` now fails the build if the two drift apart again.
 * 768 rather than 751 keeps the bitmap a whole number of bytes; it costs
 * 24 kB. */
#define PCELL_GLU 768
_Static_assert(PCELL_GLU > STORE_GLU_MAX,
               "a storable reading must be representable on the plot");
static unsigned char g_pcell[(PCOL_MAX * PCELL_GLU) / 8];
/* Per COLUMN, not just overall: an overall cap alone truncates by FILE
 * POSITION, and the log is in arrival order -- so a run of recent rows
 * filled the buffer and the older rows appended after them were never
 * reached, leaving the left of a 30-day plot empty while the right was
 * dense. A per-column budget is spatially fair: no stretch of time can
 * starve another, whatever order the rows arrive in. */
#define PCELL_PERCOL PLOT_PERCOL
static struct ui_point g_plong[PLOT_LONG_MAX];
static unsigned short g_pcoln[PCOL_MAX]; /* cells kept in each column */
static int g_nplong;
static long g_plong_end, g_plong_span, g_plong_size;

static long plot_log_size(const char *path)
{
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   long n = lseek(fd, 0, SEEK_END);
   close(fd);
   return n < 0 ? 0 : n;
}

/* THE WIDEST PROVENANCE ID THAT CAN EXIST, and it is not a guess: struct
 * reading stores `src` in sixteen bits (store.h), so sensor_mint refuses to
 * issue an id past 0xFFFF rather than let 65536 alias legacy id 0 and
 * reattribute a reading to a different physical device (sensors.c, "An id
 * must fit the 16-bit `src` field"). A wider number in this column therefore
 * did not come from this app, and cannot name anything on this phone.
 *
 * Spelled here rather than shared with sensors.c because that file's constant
 * is a MINTING rule and this one is a READING rule; nothing checks that the
 * two agree, which is worth knowing when either moves. */
#define PLOT_SRC_MAX 0xFFFF

/* One readings.csv row -> (t, glu, src, kind). Returns 0 if the line is not
 * a datapoint. Legacy short rows carry no source or kind; they read as the
 * unattributed CGM trace, exactly as they do everywhere else.
 *
 * THE CURSOR, NOT A FIFTH HAND-ROLLED DIGIT LOOP. Accumulating each field
 * after glucose with a bare `n = n * 10 + digit` and NO CAP AT ALL is wrong
 * twice over, and the second is the one that reaches the screen:
 *
 *   1. Signed overflow is undefined behaviour, and it happens while PARSING
 *      -- before any range check below can refuse the value. readings.csv
 *      is append-only and a torn write or a hand-edit can leave a long
 *      digit run in it, so it is reachable from a file rather than from a
 *      peer.
 *      At -O2 that is not "a wrong number": it is the whole translation unit
 *      losing its meaning, and this is the translation unit the 30- and
 *      90-day glucose plots are drawn from.
 *
 *   2. Even with the arithmetic made safe, A BOUND ON THE WRONG SIDE OF A
 *      NARROWING CAST IS NOT A BOUND. The kind column was already
 *      "normalised" here, with a comment saying so -- but the comparison
 *      `n == KIND_BGM` ran on an `int` the parse had already wrapped, and
 *      4294967297 wraps to exactly 1, which IS KIND_BGM. So the single input
 *      that column's normalisation existed to stop walked straight through
 *      it: the row drew as a fingerstick nobody took. The same digits in the
 *      source column wrapped to 1 and borrowed device slot 1's colour and
 *      name. Both are measured in test/plottest.c; both were live.
 *
 * So: read every column through csvcur.h's bounded reader, keep the value in
 * a `long` while it is checked, and cast only after. A field with more digits
 * than that reader holds is not a wrong value to be clamped -- it describes a
 * different row entirely -- so the COMPLETE row is refused, and refused
 * before any of the four out-params is written. A parser that rejects a field
 * after it has already published three others is the same defect one step
 * later: the caller sees 0, ignores the row, and the next caller to forget
 * that gets whatever the corrupt line left behind. */
int plot_store_row(const char *ln, long *t, int *glu, int *src, int *kind)
{
   /* The cursor reads inside ONE line and can never run past it, so it needs
    * that line's end. Callers hand this function either a NUL-terminated
    * buffer carved out of the read below, or a whole line from fgets with its
    * '\n' still attached -- stop at whichever arrives first. */
   const char *e = ln;
   while (*e && *e != '\n')
      e++;
   struct csv_cur c;
   csv_open(&c, ln, e);

   /* A leading digit is what makes a line a datapoint at all: the header
    * row, a comment and a blank line all fail here, exactly as before. Kept
    * as an explicit test rather than leaning on CSV_FIELD_EMPTY because
    * csv_num also accepts a leading '-', and a negative first column is not
    * a timestamp this file has ever written. */
   if (c.p >= c.e || *c.p < '0' || *c.p > '9')
      return 0;

   enum csv_field why = CSV_FIELD_OK;
   long tv            = csv_num(&c, &why);
   if (why != CSV_FIELD_OK)
      return 0;
   if (!csv_sep(&c))
      return 0;
   long gv = csv_num(&c, &why);
   if (why != CSV_FIELD_OK)
      return 0;

   /* Nothing is published yet; these are the row's answers, held back until
    * the whole row has been read and passed. */
   long srcv  = 0;
   long kindv = KIND_CGM;

   /* fields: t,glu,trend,rssi,lag,src,device_time,tz,kind,rescale */
   int f = 1;
   while (!csv_at_end(&c)) {
      if (!csv_sep(&c)) {
         /* Trailing junk inside a column -- "7x" -- is stepped over rather
          * than fatal, which is what this reader has always done: the column
          * keeps its leading digits. Worth naming, because it means a
          * contaminated field can still yield a perfectly legal value. */
         c.p++;
         continue;
      }
      f++;
      long n = csv_num(&c, &why);
      if (why == CSV_FIELD_OVERFLOW)
         return 0; /* more digits than any column of this file can mean */
      if (why == CSV_FIELD_EMPTY)
         continue; /* an absent column leaves its default standing */
      if (f == 5) {
         /* OUT OF THE ID DOMAIN IS UNATTRIBUTED, NOT A DROPPED READING.
          * Refusing the row here would delete a glucose value from the plot,
          * and this file's header explains at length that a dropped reading
          * is not clamped to an edge -- it is simply absent from the view you
          * opened to see it. Source is decoration; the reading is the datum.
          * So an id that cannot name any device on this phone reads as 0,
          * which the renderer already draws as the unattributed trace, the
          * same as every pre-registry row. Unattributed is honest;
          * misattributed -- which is what the wrapped 1 was -- is not. */
         srcv = (n >= 0 && n <= PLOT_SRC_MAX) ? n : 0;
      } else if (f == 8) {
         /* NORMALISE, do not store what the file says. This is the SECOND
          * reader of readings.csv -- hist_insert is the other, and it had the
          * identical gap -- and the return below bounds t and glu while
          * letting any digit run through as a kind. A fuzz of this function
          * accepted 9, 15, 149, 363 and 2312.
          *
          * The consequence is on screen: this kind is copied into the
          * ui_point handed to the long-span plot, and the renderer draws
          * kind == KIND_INS along the bottom edge -- so a corrupt 2 becomes
          * an insulin dose that never happened, and anything else is not
          * KIND_BGM so it draws as a CGM line point. The log is append-only,
          * so a row admitted once is redrawn at every launch.
          *
          * `n` IS STILL A LONG HERE, and that is the whole point: this
          * comparison is the bound, and narrowed first it would sit
          * downstream of the wrap that produced the value it checks. */
         kindv = (n == KIND_BGM) ? KIND_BGM : KIND_CGM;
      }
   }

   /* Both bounds are applied on the WIDE side, before anything narrows: gv
    * is compared as a long, so a value that would wrap into 1..1999 on the
    * way into an int is refused rather than admitted as a plausible
    * glucose. */
   if (tv <= 0 || gv <= 0 || gv >= 2000)
      return 0;
   *t    = tv;
   *glu  = (int)gv;
   *src  = (int)srcv;
   *kind = (int)kindv;
   return 1;
}

static void plong_build(const char *path, long end, long span)
{
   for (size_t i = 0; i < sizeof g_pcell; i++)
      g_pcell[i] = 0;
   for (int i = 0; i < PCOL_MAX; i++)
      g_pcoln[i] = 0;
   long from = end - span;
   g_nplong  = 0;
   int cap   = (int)(sizeof g_plong / sizeof g_plong[0]);

   int fd = open(path, O_RDONLY, 0);
   if (fd >= 0) {
      char rd[2048];
      char line[256];
      int llen = 0;
      int over = 0;
      long got = 0;
      while ((got = read(fd, rd, sizeof rd)) > 0) {
         for (long i = 0; i < got; i++) {
            if (rd[i] != '\n') {
               if (llen < (int)sizeof line - 1)
                  line[llen++] = rd[i];
               else
                  over = 1;
               continue;
            }
            line[llen] = '\0';
            int ok     = !over;
            llen       = 0;
            over       = 0;
            if (!ok)
               continue;
            long t   = 0;
            int glu  = 0;
            int src  = 0;
            int kind = 0;
            if (!plot_store_row(line, &t, &glu, &src, &kind))
               continue;
            if (t <= from || t > end || glu >= PCELL_GLU)
               continue;
            int col = (int)(((t - from) * (PCOL_MAX - 1)) / span);
            if (col < 0 || col >= PCOL_MAX)
               continue;
            if (g_pcoln[col] >= PCELL_PERCOL || g_nplong >= cap)
               continue; /* this column has had its share */
            unsigned long cell =
                ((unsigned long)col * PCELL_GLU) + (unsigned long)glu;
            if (g_pcell[cell >> 3U] & (unsigned char)(1U << (cell & 7U)))
               continue; /* this pixel is already painted */
            g_pcell[cell >> 3U] |= (unsigned char)(1U << (cell & 7U));
            g_pcoln[col]++;
            g_plong[g_nplong].t    = t;
            g_plong[g_nplong].glu  = glu;
            g_plong[g_nplong].src  = src;
            g_plong[g_nplong].kind = kind;
            g_nplong++;
         }
      }
      close(fd);
   }
   g_plong_end  = end;
   g_plong_span = span;
   g_plong_size = plot_log_size(path);
}

/* The glucose points to draw for `span`, and how many. Short spans come from
 * the live RAM window; long ones from the bucketed log above. */
const struct ui_point *plot_source_from(const char *path, long now, int hours,
                                        int *n)
{
   long span = (long)hours * 3600;
   if (span <= PLONG_MIN) {
      *n = 0;
      return NULL; /* caller uses g_hist */
   }
   long sz = plot_log_size(path);
   if (g_plong_span != span || g_plong_size != sz || now - g_plong_end >= 60)
      plong_build(path, now, span);
   *n = g_nplong;
   return g_plong;
}
