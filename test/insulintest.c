// SPDX-License-Identifier: GPL-3.0
// insulintest.c --- Host tests for the insulin dose log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for insulin.c. A dose is a user-entered fact in an
 * append-only file the app reloads at every launch, so the properties that
 * matter are: what was confirmed is durably on disk, what loads back is
 * exactly what was written, a corrupt row can never load as a plausible dose,
 * the tail is TIME-SORTED however doses were entered or edited, and the
 * form's pre-population (last units per type) follows dose time.
 *
 * Built and run by `make insulintest`, which `make check` depends on. */
#include "insulin.h"
#include <stdio.h>
#include <string.h> /* memset: the over-long-row regression builds one */
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

static void fresh(void)
{
   (void)snprintf(g_ins_path, sizeof g_ins_path, "build/test/rt-insulin.csv");
   unlink(g_ins_path);
   insulin_load();
}

int main(void)
{
   const long t0 = 1700000000;

   printf("== append, reload: the file is the record ==\n");
   fresh();
   ck(g_nins == 0, "a fresh install is an empty log");
   ck(insulin_append(t0, INS_SLOW, 12, -3600) == 0, "a dose appends");
   ck(insulin_append(t0 + 60, INS_FAST, 4, -3600) == 0, "...and another");
   ck(g_nins == 2, "both are in the tail");
   insulin_load();
   ck(g_nins == 2, "both load back");
   ck(g_ins[0].t == t0 && g_ins[0].type == INS_SLOW && g_ins[0].units == 12,
      "...values intact, oldest first");
   ck(g_ins[1].type == INS_FAST && g_ins[1].units == 4, "...newest last");

   printf("== pre-population: last units PER TYPE, by dose time ==\n");
   ck(insulin_last_units(INS_SLOW) == 12, "SLOW recalls its own last dose");
   ck(insulin_last_units(INS_FAST) == 4, "FAST recalls its own last dose");
   ck(insulin_append(t0 - 999, INS_FAST, 6, -3600) == 0,
      "a BACKDATED dose still appends");
   ck(g_ins[0].t == t0 - 999,
      "...and files into place: the tail is TIME-sorted, not entry-sorted");
   ck(insulin_last_units(INS_FAST) == 4,
      "...so 'last' means latest BY DOSE TIME, unmoved by the backdate");

   printf("== out-of-range input is refused, not clamped ==\n");
   int before = g_nins;
   ck(insulin_append(t0, 7, 5, 0) < 0, "an unknown type is refused");
   ck(insulin_append(t0, INS_SLOW, 0, 0) < 0, "zero units is refused");
   ck(insulin_append(t0, INS_SLOW, INS_UNITS_MAX + 1, 0) < 0,
      "over-max units is refused");
   ck(insulin_append(0, INS_SLOW, 5, 0) < 0, "a zero timestamp is refused");
   ck(g_nins == before, "refusals changed nothing in the tail");

   printf("== rows this process did not write ==\n");
   {
      FILE *f = fopen(g_ins_path, "a");
      if (f) {
         fputs("garbage,line,here\n", f);
         fputs("1700000100,1,999,0\n", f);         /* implausible units */
         fputs("1700000200,9,5,0\n", f);           /* unknown type */
         fputs("99999999999999999999,1,5,0\n", f); /* absurd digit run */
         fputs("1700000300,0,7,0\n", f);           /* one legitimate row */
         fclose(f);
      }
   }
   insulin_load();
   ck(g_nins == before + 1,
      "only the legitimate foreign row loads; corrupt ones are dropped");
   ck(g_ins[g_nins - 1].units == 7 && g_ins[g_nins - 1].type == INS_SLOW,
      "...and it parsed correctly");

   printf("== edit / delete rewrite exactly one content-matched row ==\n");
   fresh();
   (void)insulin_append(t0, INS_SLOW, 10, 0);
   (void)insulin_append(t0 + 100, INS_FAST, 5, 0);
   (void)insulin_append(t0 + 200, INS_FAST, 5, 0);
   {
      struct ins_rec orig = {t0 + 100, INS_FAST, 5};
      ck(insulin_update(&orig, t0 + 150, INS_SLOW, 8, 0) == 0,
         "an edit rewrites the matched row");
      ck(g_nins == 3, "...row count unchanged");
      ck(insulin_last_units(INS_SLOW) == 8, "...new values took effect");
      insulin_load();
      ck(g_nins == 3 && insulin_last_units(INS_SLOW) == 8,
         "...and the edit is durable on disk");
      struct ins_rec gone = {t0 + 150, INS_SLOW, 8};
      ck(insulin_delete(&gone) == 0, "a delete removes the row");
      ck(g_nins == 2, "...row count down one");
      ck(insulin_delete(&gone) < 0, "deleting a missing row refuses");
      insulin_load();
      ck(g_nins == 2, "...and the delete is durable on disk");
   }

   printf("== an over-long row cannot overflow the rewrite buffer ==\n");
   {
      /* ins_rewrite's pass-2 line buffer is 256 bytes and appends its newline
       * UNCONDITIONALLY, so a row of exactly 256 characters used to write one
       * byte past the array (ASan: stack-buffer-overflow at insulin.c:254) and
       * hand write() a 257-byte length. The row is unparseable either way; the
       * contract is that the rewrite REFUSES rather than corrupting the stack.
       * Run this under ASan to see the regression, not just the return code. */
      fresh();
      ck(insulin_append(t0, INS_SLOW, 10, 0) == 0, "a normal dose to edit");
      FILE *f = fopen(g_ins_path, "ab");
      ck(f != NULL, "the log opens for the hostile row");
      if (f) {
         char pad[257];
         memset(pad, '9', sizeof pad);
         /* 16 characters of plausible row, padded to exactly 256 */
         fprintf(f, "1700001200,0,7,0");
         fwrite(pad, 1, 256 - 16, f);
         fputc('\n', f);
         fclose(f);
      }
      struct ins_rec orig = {t0, INS_SLOW, 10};
      ck(insulin_update(&orig, t0, INS_SLOW, 11, 0) < 0,
         "an edit past a 256-char row refuses instead of overflowing");
      insulin_load();
      ck(insulin_last_units(INS_SLOW) == 10,
         "...and the refused edit changed nothing");
   }

   printf("== the tail stays bounded; the newest rows win ==\n");
   fresh();
   for (int i = 0; i < NINS + 10; i++)
      (void)insulin_append(t0 + i, INS_SLOW, 1 + (i % 50), 0);
   ck(g_nins == NINS, "the tail caps at NINS");
   ck(g_ins[g_nins - 1].t == t0 + NINS + 9, "...keeping the newest");
   insulin_load();
   ck(g_nins == NINS && g_ins[g_nins - 1].t == t0 + NINS + 9,
      "...and a reload agrees");

   printf(all ? "ALL INSULIN TESTS PASSED\n" : "SOME TESTS FAILED\n");
   return all ? 0 : 1;
}
