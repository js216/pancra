// SPDX-License-Identifier: GPL-3.0
// crashtest.c --- the crash logger's formatters, which run in a signal handler
// Copyright 2026 Jakob Kastelic

/* These two functions exist because a signal handler may not call snprintf,
 * and they were unreachable by any test while they sat inside main.c -- which
 * is the whole reason crashlog.c was split out.
 *
 * What matters about them is not that they format nicely. It is that they
 * NEVER WRITE PAST cap, whatever they are handed: they run after the program
 * has already crashed, and a crash logger that corrupts the stack while
 * recording a corrupted stack tells you nothing and costs you the tombstone.
 * So the cases below are mostly hostile ones.
 */
#include "crashlog.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int ok, const char *what)
{
   if (!ok) {
      printf("  FAIL: %s\n", what);
      fails = 1;
   }
}

static void eq(const char *got, int n, const char *want, const char *what)
{
   if ((int)strlen(want) != n || memcmp(got, want, (size_t)n) != 0) {
      printf("  FAIL: %s\n    got  '%.*s'\n    want '%s'\n", what, n, got,
             want);
      fails = 1;
   }
}

int main(void)
{
   /* ---- ordinary numbers ---- */
   {
      char b[32];
      int p = 0;
      crash_putn(b, sizeof b, &p, 0);
      eq(b, p, "0", "zero");
   }
   {
      char b[32];
      int p = 0;
      crash_putn(b, sizeof b, &p, 12345);
      eq(b, p, "12345", "a positive number");
   }
   {
      char b[32];
      int p = 0;
      crash_putn(b, sizeof b, &p, -42);
      eq(b, p, "-42", "a negative number keeps its sign");
   }

   /* LONG_MIN has no positive counterpart, so negating it overflows. The
    * unsigned cast in crash_putn is what makes this printable at all; a naive
    * -v would be undefined precisely when a crash logger is running. */
   {
      char b[40];
      int p    = 0;
      long min = (-9223372036854775807L - 1L);
      crash_putn(b, sizeof b, &p, min);
      eq(b, p, "-9223372036854775808", "LONG_MIN does not overflow");
   }

   /* ---- the bound is the point ---- */
   {
      char b[4] = {0};
      int p     = 0;
      crash_putn(b, 4, &p, 1234567890);
      ck(p <= 4, "crash_putn never writes past cap");
   }
   {
      char b[8];
      int p = 3; /* already partly full, as it is in real use */
      crash_putn(b, 8, &p, 999999);
      ck(p <= 8, "crash_putn respects a non-zero starting position");
   }
   {
      char b[6];
      int p = 0;
      crash_puts(b, 6, &p, "abcdefghij", 100);
      ck(p <= 6, "crash_puts never writes past cap");
   }
   {
      char b[32];
      int p = 0;
      crash_puts(b, sizeof b, &p, "abcdefghij", 3);
      eq(b, p, "abc", "crash_puts honours its own max");
   }
   {
      char b[32];
      int p = 0;
      crash_puts(b, sizeof b, &p, NULL, 10);
      ck(p == 0, "a NULL string writes nothing rather than crashing");
   }

   /* ---- the shape the handler actually builds ---- */
   {
      char b[200];
      int p = 0;
      crash_puts(b, sizeof b, &p, "CRASH sig=", 200);
      crash_putn(b, sizeof b, &p, 11);
      crash_puts(b, sizeof b, &p, " glu=", 200);
      crash_putn(b, sizeof b, &p, 113);
      eq(b, p, "CRASH sig=11 glu=113", "the assembled line");
   }

   /* ---- the line FOLLOWS the live values ----
    *
    * This is the case that matters and the one nothing covered. The context
    * holds pointers so the handler reports what is true at the instant of the
    * crash. The checkpoint label is a pointer VARIABLE, so the context has to
    * hold its ADDRESS; holding its value instead compiled fine, passed every
    * formatter test above, and made every crash report in the shipped build
    * say "boot" forever. So: move each value AFTER building the context, and
    * require the line to have moved with it. */
   {
      /* _Atomic, matching the field it is bound to: the checkpoint is written
       * by every thread in the app and read from a signal handler, so the
       * handler's load has to be indivisible (see crashlog.h). The test holds
       * the same type the app does, or it would be exercising a different
       * function signature from the one that ships. */
      const char *_Atomic where = "boot";
      const char *status        = "READING";
      int glu                   = 113;
      int menu                  = 4;
      int nhist                 = 900;
      struct crash_ctx ctx;
      ctx.where  = &where;
      ctx.status = status;
      ctx.glu    = &glu;
      ctx.menu   = &menu;
      ctx.nhist  = &nhist;

      char b[200];
      int n = crash_line(b, sizeof b, 11, &ctx);
      eq(b, n,
         "CRASH sig=11 where=boot status=READING glu=113 menu=4 "
         "nhist=900\n",
         "the line at boot");

      where = "on_timer";
      glu   = 54;
      menu  = 0;
      n     = crash_line(b, sizeof b, 6, &ctx);
      eq(b, n,
         "CRASH sig=6 where=on_timer status=READING glu=54 menu=0 "
         "nhist=900\n",
         "a moved checkpoint and glucose are both reported");
   }

   /* A context whose fields are all absent must still produce a line rather
    * than dereferencing null in a handler. */
   {
      struct crash_ctx ctx;
      ctx.where  = 0;
      ctx.status = 0;
      ctx.glu    = 0;
      ctx.menu   = 0;
      ctx.nhist  = 0;
      char b[200];
      int n = crash_line(b, sizeof b, 4, &ctx);
      eq(b, n, "CRASH sig=4 where= status= glu=-1 menu=-1 nhist=-1\n",
         "an empty context still writes a line");
   }

   if (!fails)
      printf("crashtest: the crash logger's formatters stay inside the "
             "buffer, and the line follows the live values\n");
   return fails;
}
