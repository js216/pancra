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

/* The append-only property is checked by SIZE: an edit that shrank or held
 * the file steady would mean the log had been rewritten. */
static long fsize(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   (void)fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fclose(f);
   return n;
}

static void fresh(void)
{
   (void)snprintf(g_ins_path, sizeof g_ins_path,
                  "build/app/test/rt-insulin.csv");
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

   printf("== edit / delete append an assertion for one matched dose ==\n");
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

   printf(
       "== the log is APPEND-ONLY: an edit adds history, never removes ==\n");
   {
      /* The whole point of schema v2. An edit used to rewrite the file in
       * place, which made this the one log a bug could shorten, and threw
       * away what the dose used to be. Now the file only ever grows, and a
       * past day's rows never change -- which is also what lets the sync
       * protocol treat old buckets as frozen. */
      fresh();
      ck(insulin_append(t0, INS_SLOW, 6, 0) == 0, "a dose is logged");
      long after_add      = fsize(g_ins_path);
      struct ins_rec orig = {t0, INS_SLOW, 6};
      ck(insulin_update(&orig, t0, INS_SLOW, 4, 0) == 0, "it is corrected");
      long after_edit = fsize(g_ins_path);
      ck(after_edit > after_add,
         "...the file GREW: the correction is a new row");
      ck(g_nins == 1, "...but there is still exactly one dose");
      ck(insulin_last_units(INS_SLOW) == 4, "...showing the new value");
      insulin_load();
      ck(g_nins == 1 && insulin_last_units(INS_SLOW) == 4,
         "...and replay agrees after a reload");

      struct ins_rec now4 = {t0, INS_SLOW, 4};
      ck(insulin_delete(&now4) == 0, "it is then retracted");
      ck(fsize(g_ins_path) > after_edit, "...which also only appends");
      insulin_load();
      ck(g_nins == 0, "...and replay leaves no dose");
   }

   printf("== an over-long row is skipped, and blocks nothing ==\n");
   {
      /* There is no rewrite buffer left to overflow. A corrupt or over-long
       * row is simply not an assertion: load skips it, and an edit to a
       * perfectly good dose is unaffected by its presence. */
      fresh();
      ck(insulin_append(t0, INS_SLOW, 10, 0) == 0, "a normal dose to edit");
      FILE *f = fopen(g_ins_path, "ab");
      ck(f != NULL, "the log opens for the hostile row");
      if (f) {
         char pad[257];
         memset(pad, '9', sizeof pad);
         fprintf(f, "1700001200,0,7,0");
         fwrite(pad, 1, 256 - 16, f);
         fputc('\n', f);
         fclose(f);
      }
      insulin_load();
      ck(g_nins == 1, "the over-long row does not load as a dose");
      struct ins_rec orig = {t0, INS_SLOW, 10};
      ck(insulin_update(&orig, t0, INS_SLOW, 11, 0) == 0,
         "an edit past it succeeds");
      insulin_load();
      ck(insulin_last_units(INS_SLOW) == 11, "...and is durable");
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
