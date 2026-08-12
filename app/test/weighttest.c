// SPDX-License-Identifier: GPL-3.0
// weighttest.c --- Host tests for the body-weight log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for weight.c.
 *
 * The unit conversion is the part that needs pinning. The file stores grams
 * and the user types tenths of their chosen display unit, so every entry
 * makes a round trip through two conversions -- and if that trip is not
 * exact, the number shown back is not the number typed. This codebase has
 * already been bitten by precisely that once (the plot-max entry rendered a
 * value the user could not re-type to reproduce), so it is asserted here for
 * every tenth across the whole plausible range rather than at a few points.
 *
 * Built and run by `make weighttest`.
 */
#include "weight.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

int main(void)
{
   (void)snprintf(g_wt_path, sizeof g_wt_path, "build/app/test/wt-log.csv");
   unlink(g_wt_path);

   printf("== units: kilograms are exact ==\n");
   ck(wt_from_tenths(700, WT_KG) == 70000, "70.0 kg is 70000 g");
   ck(wt_to_tenths(70000, WT_KG) == 700, "...and back");
   ck(strcmp(wt_unit_name(WT_KG), "KG") == 0, "KG names itself");
   ck(strcmp(wt_unit_name(WT_LB), "LB") == 0, "LB names itself");

   printf("== units: pounds convert and ROUND TRIP ==\n");
   /* 154.0 lb is 69853 g. Truncating instead of rounding gave 69841, which
    * renders back as 153.9 -- a weight the user typed and cannot re-enter to
    * reproduce the same record. */
   ck(wt_from_tenths(1540, WT_LB) == 69853, "154.0 lb is 69853 g");
   ck(wt_to_tenths(69853, WT_LB) == 1540, "...and back to 154.0");

   printf("== EVERY typed weight survives the round trip ==\n");
   {
      int bad = 0;
      int n   = 0;
      for (int u = 0; u < 2; u++)
         for (int t = 1; t < 10000; t++) {
            long g = wt_from_tenths(t, u);
            if (g <= 0)
               continue; /* outside the stored range: refused, not converted */
            n++;
            if (wt_to_tenths(g, u) != t)
               bad++;
         }
      printf("  (checked %d values across both units)\n", n);
      ck(n > 5000, "the sweep actually covered the range");
      ck(bad == 0, "no typed weight renders back as a different number");
   }

   printf("== the stored range is enforced, not clamped ==\n");
   ck(wt_from_tenths(0, WT_KG) == 0, "zero is refused");
   ck(wt_from_tenths(-5, WT_KG) == 0, "negative is refused");
   ck(wt_from_tenths(10, WT_KG) == 0, "1.0 kg is below the floor, refused");
   ck(wt_from_tenths(9999, WT_KG) == 0, "999.9 kg is above the ceiling");
   /* REFUSE, never clamp: silently storing a different weight than the one
    * typed is worse than not storing it. */
   ck(wt_from_tenths(200, WT_KG) == 20000, "exactly the floor IS accepted");

   printf("== append, reload, order ==\n");
   ck(weight_append(1700000000L, 70000L, 0) == 0, "an entry appends");
   ck(g_nwt == 1 && g_wt[0].g == 70000, "...and lands in the tail");
   ck(weight_append(1699000000L, 69000L, 0) == 0, "a BACKDATED entry appends");
   ck(g_nwt == 2, "both are held");
   ck(g_wt[0].t < g_wt[1].t,
      "the tail is oldest-first even when entered out of order");
   weight_load();
   ck(g_nwt == 2 && g_wt[0].g == 69000 && g_wt[1].g == 70000,
      "a reload reproduces the same log");

   printf("== out-of-range rows are refused on the way IN ==\n");
   /* The file is loaded at every launch and never rewritten, so a corrupt row
    * admitted once is shown for good. */
   ck(weight_append(0, 70000L, 0) < 0, "a zero timestamp is refused");
   ck(weight_append(WT_T_MAX, 70000L, 0) < 0, "an absurd timestamp is refused");
   ck(weight_append(1700000001L, 5L, 0) < 0, "5 g is refused");
   ck(weight_append(1700000001L, 999999L, 0) < 0, "1 tonne is refused");
   ck(g_nwt == 2, "and none of them reached the tail");
   weight_load();
   ck(g_nwt == 2, "...nor the file");

   printf("== a corrupt file cannot wedge or resurrect a bad row ==\n");
   {
      FILE *f = fopen(g_wt_path, "a");
      if (f) {
         fputs("garbage,not,numbers\n", f);
         fputs("99999999999999999999999,99999999999999999999\n", f);
         fputs("1700000002,70500\n", f);
         fclose(f);
      }
      weight_load(); /* must terminate */
      ck(g_nwt == 3, "the two bad rows are dropped, the good one kept");
      ck(g_wt[2].g == 70500, "and it is the one that parsed");
   }

   printf("== update rewrites the RIGHT row, and only that row ==\n");
   {
      unlink(g_wt_path);
      g_nwt = 0;
      for (int i = 0; i < 5; i++)
         weight_append(1700000000L + i, 70000L + ((long)i * 100), 0);
      struct wt_rec orig = g_wt[2];
      ck(weight_update(&orig, orig.t, 71234L, 0) == 0, "an update succeeds");
      weight_load();
      ck(g_nwt == 5, "the row count is unchanged");
      ck(g_wt[2].g == 71234, "the target row carries the new value");
      ck(g_wt[0].g == 70000 && g_wt[4].g == 70400,
         "its neighbours are untouched");
      /* Matching is by CONTENT: an update whose original is not in the file
       * must change nothing rather than rewrite an arbitrary row. */
      struct wt_rec ghost = {1600000000L, 65000L};
      ck(weight_update(&ghost, 1600000000L, 66000L, 0) < 0,
         "an update for a row that is not there is refused");
      weight_load();
      ck(g_nwt == 5 && g_wt[2].g == 71234, "...and the file is unchanged");
      ck(weight_update(&orig, orig.t, 5L, 0) < 0,
         "an out-of-range update is refused");
   }

   printf("== delete removes exactly one row ==\n");
   {
      struct wt_rec d = g_wt[1];
      ck(weight_delete(&d) == 0, "a delete succeeds");
      weight_load();
      ck(g_nwt == 4, "one row fewer");
      for (int i = 0; i < g_nwt; i++)
         if (g_wt[i].t == d.t && g_wt[i].g == d.g)
            ck(0, "the deleted row is really gone");
      ck(weight_delete(&d) < 0, "deleting it again is refused");
      weight_load();
      ck(g_nwt == 4, "...and removes nothing else");
   }

   printf("== duplicates: the LAST match is the one touched ==\n");
   {
      unlink(g_wt_path);
      g_nwt = 0;
      /* Two rows can be genuinely identical -- two weigh-ins recorded at the
       * same second. The rewrite must touch exactly one of them, or a delete
       * silently removes both. */
      weight_append(1700000000L, 70000L, 0);
      weight_append(1700000000L, 70000L, 0);
      weight_append(1700000100L, 71000L, 0);
      ck(g_nwt == 3, "three rows, two of them identical");
      struct wt_rec dup = {1700000000L, 70000L};
      ck(weight_delete(&dup) == 0, "deleting a duplicated row succeeds");
      weight_load();
      ck(g_nwt == 2, "exactly ONE row went, not both");
      int left = 0;
      for (int i = 0; i < g_nwt; i++)
         if (g_wt[i].t == dup.t && g_wt[i].g == dup.g)
            left++;
      ck(left == 1, "...and the other copy survived");
   }

   printf("\n%s\n", all ? "ALL WEIGHT TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
