// SPDX-License-Identifier: GPL-3.0
// plotdata.c --- long plot spans, read from the log (see plotdata.h)
// Copyright 2026 Jakob Kastelic
#include "plotdata.h"
#include "sensors.h" /* KIND_CGM */
#include "store.h"   /* STORE_GLU_MAX: the plot must be able to show it */
#include "ui.h"      /* struct ui_point, in full */
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

/* One readings.csv row -> (t, glu, src, kind). Returns 0 if the line is not
 * a datapoint. Legacy short rows carry no source or kind; they read as the
 * unattributed CGM trace, exactly as they do everywhere else. */
int plot_store_row(const char *ln, long *t, int *glu, int *src, int *kind)
{
   const char *p = ln;
   long v        = 0;
   if (*p < '0' || *p > '9')
      return 0;
   /* CAP THE DIGITS, the rdfield/stats/sensors/insulin/weight idiom.
    * Signed overflow is UB and happens DURING parsing, before any range
    * check downstream can reject the value -- and this parser reads an
    * append-only file that a torn write can corrupt. Saturate so the
    * bounds below discard the row instead. */
   int nt = 0;
   while (*p >= '0' && *p <= '9') {
      if (nt < 18) {
         v = (v * 10) + (*p - '0');
         nt++;
      }
      p++;
   }
   if (nt >= 18)
      v = 0; /* unparseable: fails the *t > 0 bound below */
   *t = v;
   if (*p != ',')
      return 0;
   p++;
   int g = 0;
   int d = 0;
   while (*p >= '0' && *p <= '9') {
      if (d < 9)
         g = (g * 10) + (*p - '0');
      else
         g = 16777216; /* saturate: fails the *glu < 2000 bound below */
      p++;
      d++;
   }
   if (!d)
      return 0;
   *glu  = g;
   *src  = 0;
   *kind = KIND_CGM;
   /* fields: t,glu,trend,rssi,lag,src,device_time,tz,kind,rescale */
   int f = 1;
   while (*p && *p != '\n') {
      if (*p != ',') {
         p++;
         continue;
      }
      p++;
      f++;
      int neg = 0;
      if (*p == '-') {
         neg = 1;
         p++;
      }
      int n = 0;
      int k = 0;
      while (*p >= '0' && *p <= '9') {
         n = (n * 10) + (*p++ - '0');
         k++;
      }
      if (!k)
         continue;
      if (f == 5)
         *src = neg ? 0 : n;
      else if (f == 8)
         /* NORMALISE, do not store what the file says. This is the SECOND
          * reader of readings.csv -- hist_insert is the other, and it had the
          * identical gap -- and the return below bounds t and glu while
          * letting any digit run through as a kind. A fuzz of this function
          * accepted 9, 15, 149, 363 and 2312.
          *
          * The consequence is on screen: this kind is copied into the
          * ui_point handed to the long-span plot, and ui.c draws
          * kind == KIND_INS along the bottom edge -- so a corrupt 2 becomes
          * an insulin dose that never happened, and anything else is not
          * KIND_BGM so it draws as a CGM line point. The log is append-only,
          * so a row admitted once is redrawn at every launch. */
         *kind = (!neg && n == KIND_BGM) ? KIND_BGM : KIND_CGM;
   }
   return *t > 0 && *glu > 0 && *glu < 2000;
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
