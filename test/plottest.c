// SPDX-License-Identifier: GPL-3.0
// plottest.c --- the long-span plot data: nothing visible may be dropped
// Copyright 2026 Jakob Kastelic

/* The 30D plot is drawn from the LOG, downsampled, because tying its depth
 * to a point budget meant it silently shrank as sources were added -- the
 * bug that had a 30-day plot showing ten days.
 *
 * Downsampling is where such a plot lies quietly, so this pins the property
 * that matters: every reading that would occupy its OWN pixel survives. Two
 * readings that would paint the same pixel are indistinguishable on screen,
 * so keeping one is not a loss; anything more IS.
 */
#include "plotdata.h"
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "ui.h"      /* struct ui_point, in full */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail;

static void ck(int cond, const char *what)
{
   if (cond) {
      printf("  ok   %s\n", what);
      return;
   }
   printf("  FAIL %s\n", what);
   fail = 1;
}

/* Write a log in ARRIVAL order: recent rows first, then imported history
 * appended after them -- the shape the phone's file actually has. */
static void write_log(const char *path, long now, int recent, int imported)
{
   FILE *f = fopen(path, "w");
   if (!f)
      exit(2);
   fprintf(f, "# unix_time,glucose_mgdl,trend,rssi,recv_lag_s,sensor_id,"
              "device_time,tz_offset_s,kind,rescale\n");
   for (int i = 0; i < recent; i++) {
      long t = now - ((long)(recent - i) * 300);
      fprintf(f, "%ld,%d,3,-70,4,7,%ld,-25200,0,\n", t, 100 + (i % 80), t);
   }
   for (int i = 0; i < imported; i++) {
      long t = now - (95L * 86400) + ((long)i * 270);
      fprintf(f, "%ld,%d,127,,0,9,%ld,-25200,0,\n", t, 90 + (i % 90), t);
   }
   fclose(f);
}

int main(void)
{
   const char *path = "build/test/plot-readings.csv";
   long now         = 1785272930;
   write_log(path, now, 5000, 30000);

   printf("== a short span defers to the live buffer ==\n");
   int n                    = -1;
   const struct ui_point *p = plot_source_from(path, now, 3, &n);
   ck(p == NULL, "3H returns NULL so the caller uses g_hist");

   printf("== a long span is served from the log ==\n");
   p = plot_source_from(path, now, 24 * 30, &n);
   ck(p != NULL && n > 0, "30D returns points");
   if (!p) {
      printf("plottest: FAIL (no points to check)\n");
      return 1;
   }

   printf("== and it reaches back the whole span ==\n");
   long oldest = now;
   long newest = 0;
   for (int i = 0; i < n; i++) {
      if (p[i].t < oldest)
         oldest = p[i].t;
      if (p[i].t > newest)
         newest = p[i].t;
   }
   double covered = (double)(newest - oldest) / 86400.0;
   printf("       covers %.1f of 30 days, %d points\n", covered, n);
   ck(covered > 29.0, "the 30D window is filled, not just its right-hand end");

   printf("== the LEFT of the window is populated, not just the right ==\n");
   {
      /* The log is in ARRIVAL order with the imported history LAST, so a
       * reader that stops when its buffer fills never reaches the old rows
       * and leaves the left of the plot empty -- which is exactly what a
       * 30-day plot did. Count points in each half of the window. */
      long span = 30L * 86400;
      long mid  = now - (span / 2);
      int left  = 0;
      int right = 0;
      for (int i = 0; i < n; i++) {
         if (p[i].t < mid)
            left++;
         else
            right++;
      }
      printf("       %d points in the older half, %d in the newer\n", left,
             right);
      ck(left > 1000, "the older half of the span is drawn");
   }

   printf("== nothing VISIBLE is dropped ==\n");
   /* Re-read the log and check that every row inside the window shares a
    * (column, value) cell with some point that survived. That is exactly
    * "no reading the screen could have distinguished is missing". */
   {
      static unsigned char seen[(768 * 512) / 8];
      long span = 30L * 86400;
      long from = now - span;
      for (int i = 0; i < n; i++) {
         int col = (int)(((p[i].t - from) * 767) / span);
         unsigned long cell =
             ((unsigned long)col * 512) + (unsigned long)p[i].glu;
         seen[cell >> 3U] |= (unsigned char)(1U << (cell & 7U));
      }
      FILE *f = fopen(path, "r");
      if (!f)
         return 2;
      char ln[256];
      long missing = 0;
      long inwin   = 0;
      while (fgets(ln, sizeof ln, f)) {
         long t   = 0;
         int glu  = 0;
         int src  = 0;
         int kind = 0;
         if (!plot_store_row(ln, &t, &glu, &src, &kind))
            continue;
         if (t <= from || t > now || glu >= 512)
            continue;
         inwin++;
         int col            = (int)(((t - from) * 767) / span);
         unsigned long cell = ((unsigned long)col * 512) + (unsigned long)glu;
         if (!(seen[cell >> 3U] & (unsigned char)(1U << (cell & 7U))))
            missing++;
      }
      fclose(f);
      printf("       %ld rows in window, %ld with no pixel drawn\n", inwin,
             missing);
      ck(missing == 0, "every distinguishable reading is represented");
   }

   printf("== memory is bounded by the SCREEN, not the history ==\n");
   {
      /* Ten times the readings must not produce ten times the points. */
      write_log(path, now, 5000, 300000);
      int n2 = 0;
      (void)plot_source_from(path, now + 1, 24 * 30, &n2);
      long span2               = 30L * 86400;
      long mid2                = now - (span2 / 2);
      int left2                = 0;
      const struct ui_point *q = plot_source_from(path, now + 1, 24 * 30, &n2);
      for (int i = 0; i < n2; i++)
         if (q[i].t < mid2)
            left2++;
      printf("       10x the log -> %d points (was %d), %d in the older half\n",
             n2, n, left2);
      ck(left2 > 1000, "...and a log that overflows the buffer still draws "
                       "the older half");
      ck(n2 <= 768 * 40, "the point count stays inside the fixed buffer");
   }

   /* KIND IS NORMALISED, not taken from the file.
    *
    * plot_store_row is the SECOND reader of readings.csv (hist_insert is the
    * other, which had the same gap). Its return guard bounds t and glu and
    * used to let any digit run through as a kind: a fuzz of it accepted 9,
    * 15, 149, 363 and 2312. That kind is copied into the ui_point handed to
    * the long-span plot, and ui.c draws kind == KIND_INS along the bottom
    * edge -- so a corrupt 2 renders as an insulin dose that never happened,
    * and anything else is not KIND_BGM so it draws as a CGM line vertex.
    * readings.csv is append-only, so one bad row is redrawn at every launch. */
   {
      long t   = 0;
      int glu  = 0;
      int src  = 0;
      int kind = 0;
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "a KIND_INS row parses with kind normalised to KIND_CGM");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2312,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "an out-of-range kind becomes KIND_CGM");
      /* A real fingerstick must survive: forcing everything to CGM would
       * silently redraw every meter reading as a line vertex. */
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,1,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_BGM,
         "KIND_BGM is preserved");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,0,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "KIND_CGM is preserved");
   }

   printf(fail ? "plottest: FAIL\n" : "ALL PLOT TESTS PASSED\n");
   return fail;
}
