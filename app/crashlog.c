// SPDX-License-Identifier: GPL-3.0
// crashlog.c --- append a line of app context on a fatal signal
// Copyright 2026 Jakob Kastelic

/* See crashlog.h. EVERYTHING BELOW RUNS IN A SIGNAL HANDLER, so it is limited
 * to the pure helpers here plus open / write / close / signal / raise. No
 * snprintf (locale, and it may take a heap lock), no non-trivial libc
 * wrappers: a signal can arrive at any instant, including in the middle of a
 * malloc, and a handler that waits on a lock the interrupted code already
 * holds will simply never return.
 */
#include "crashlog.h"
#include "dexlibc.h"
#include <signal.h>

static char g_crash_path[256];
static struct crash_ctx g_ctx;

void crash_putn(char *b, int cap, int *pos, long v)
{
   char t[24];
   int i           = 0;
   unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
   do {
      t[i++] = (char)('0' + (int)(u % 10U));
      u /= 10U;
   } while (u && i < (int)sizeof t);
   if (v < 0 && *pos < cap)
      b[(*pos)++] = '-';
   while (i > 0 && *pos < cap)
      b[(*pos)++] = t[--i];
}

void crash_puts(char *b, int cap, int *pos, const volatile char *s, int max)
{
   for (int i = 0; s && s[i] && i < max && *pos < cap; i++)
      b[(*pos)++] = s[i];
}

int crash_line(char *b, int cap, int sig, const struct crash_ctx *ctx)
{
   int p = 0;
   crash_puts(b, cap, &p, "CRASH sig=", 200);
   crash_putn(b, cap, &p, sig);
   crash_puts(b, cap, &p, " where=", 200);
   crash_puts(b, cap, &p, (ctx && ctx->where) ? *ctx->where : 0, 40);
   crash_puts(b, cap, &p, " status=", 200);
   crash_puts(b, cap, &p, ctx ? ctx->status : 0, 24);
   crash_puts(b, cap, &p, " glu=", 200);
   crash_putn(b, cap, &p, (ctx && ctx->glu) ? *ctx->glu : -1);
   crash_puts(b, cap, &p, " menu=", 200);
   crash_putn(b, cap, &p, (ctx && ctx->menu) ? *ctx->menu : -1);
   crash_puts(b, cap, &p, " nhist=", 200);
   crash_putn(b, cap, &p, (ctx && ctx->nhist) ? *ctx->nhist : -1);
   crash_puts(b, cap, &p, "\n", 200);
   return p;
}

static void crash_handler(int sig)
{
   char b[200];
   int p  = crash_line(b, sizeof b, sig, &g_ctx);
   int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd >= 0) {
      /* Nothing useful to do about a failed write from inside a handler that
       * is about to re-raise; the tombstone is the backstop either way. */
      if (write(fd, b, p) != p) {
      }
      close(fd);
   }
   (void)signal(sig, SIG_DFL);
   (void)raise(sig);
}

void crash_install(const char *dir, const struct crash_ctx *ctx)
{
   if (ctx)
      g_ctx = *ctx;
   int i = 0;
   for (; dir[i] && i < 230; i++)
      g_crash_path[i] = dir[i];
   const char *f = "/crash.log";
   for (int j = 0; f[j]; j++)
      g_crash_path[i++] = f[j];
   g_crash_path[i] = 0;
   int sigs[]      = {4 /*ILL*/, 6 /*ABRT*/, 7 /*BUS*/, 8 /*FPE*/, 11 /*SEGV*/};
   for (unsigned k = 0; k < sizeof sigs / sizeof sigs[0]; k++)
      (void)signal(sigs[k], crash_handler);
}
