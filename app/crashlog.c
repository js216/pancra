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
#include <stdatomic.h> /* the checkpoint is read from the handler; see the .h */

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

int crash_line(char *b, int cap, int sig, const struct crash_ctx *ctx)
{
   int p = 0;
   crash_puts(b, cap, &p, "CRASH sig=", 200);
   crash_putn(b, cap, &p, sig);
   crash_puts(b, cap, &p, " where=", 200);
   /* One relaxed load, which is what makes this safe to do from inside a
    * signal handler: indivisible, no lock, no call. See crashlog.h. */
   crash_puts(b, cap, &p,
              (ctx && ctx->where)
                  ? atomic_load_explicit(ctx->where, memory_order_relaxed)
                  : 0,
              40);
   crash_puts(b, cap, &p, " status=", 200);
   crash_puts_atomic(b, cap, &p, ctx ? ctx->status : 0, 24);
   /* RELAXED ATOMIC LOADS, all three, for the reason the checkpoint above is
    * one: each of these is written by another thread, and this handler can
    * interrupt that thread between any two instructions. See crashlog.h. */
   crash_puts(b, cap, &p, " glu=", 200);
   crash_putn(b, cap, &p,
              (ctx && ctx->glu)
                  ? atomic_load_explicit(ctx->glu, memory_order_relaxed)
                  : -1);
   crash_puts(b, cap, &p, " menu=", 200);
   crash_putn(b, cap, &p,
              (ctx && ctx->menu)
                  ? atomic_load_explicit(ctx->menu, memory_order_relaxed)
                  : -1);
   crash_puts(b, cap, &p, " nhist=", 200);
   crash_putn(b, cap, &p,
              (ctx && ctx->nhist)
                  ? atomic_load_explicit(ctx->nhist, memory_order_relaxed)
                  : -1);
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

/* THE SIGNALS THIS HANDLER CLAIMS, in the order the bitmap reports them. */
static const int crash_sigs[] = {4 /*ILL*/, 6 /*ABRT*/, 7 /*BUS*/, 8 /*FPE*/,
                                 11 /*SEGV*/};

struct crash_install_result crash_install(const char *dir,
                                          const struct crash_ctx *ctx)
{
   struct crash_install_result r = {0, 0};
   /* ---- THE CONTEXT AND THE PATH BEFORE ANY HANDLER EXISTS ---
    *
    * A signal can arrive between two registrations, and the handler it lands
    * in reads g_ctx and g_crash_path. Publishing them first means the very
    * first signal this process can take already has somewhere to write and
    * something to say; the other order leaves a window in which the handler
    * is installed and has neither. */
   if (ctx)
      g_ctx = *ctx;
   int i = 0;
   for (; dir[i] && i < 230; i++)
      g_crash_path[i] = dir[i];
   const char *f = "/crash.log";
   for (int j = 0; f[j]; j++)
      g_crash_path[i++] = f[j];
   g_crash_path[i] = 0;

   /* ---- AND WHETHER IT WAS ACTUALLY INSTALLED --------------------------
    *
    * `(void)signal(...)` five times and a void return means startup
    * announces crash reporting whether or not any of it is in place. What
    * that costs is exactly the case the file exists for: the app dies, there
    * is no crash.log, and the only record is that the process is gone --
    * with nothing anywhere to say the handler was never installed.
    *
    * signal() answers SIG_ERR when it refuses. Each answer is kept, as a
    * BITMAP rather than a count, because "four of five" is not a fact
    * anybody can act on and "SIGSEGV is not covered" is.
    *
    * PARTIAL INSTALLATION IS KEPT, NOT ROLLED BACK. A handler that IS
    * installed writes a report for its own signal, and removing it because a
    * different one could not be installed trades a partial diagnostic for
    * none at all. The caller is told which are missing and decides what to
    * say. */
   for (unsigned k = 0; k < sizeof crash_sigs / sizeof crash_sigs[0]; k++) {
      if (signal(crash_sigs[k], crash_handler) == SIG_ERR)
         r.failed |= 1U << k;
      else
         r.installed |= 1U << k;
   }
   return r;
}

int crash_sig_of(unsigned bit)
{
   return (bit < sizeof crash_sigs / sizeof crash_sigs[0]) ? crash_sigs[bit]
                                                           : 0;
}

void crash_puts(char *b, int cap, int *pos, const volatile char *s, int max)
{
   for (int i = 0; s && s[i] && i < max && *pos < cap; i++)
      b[(*pos)++] = s[i];
}

void crash_puts_atomic(char *b, int cap, int *pos, const _Atomic char *s,
                       int max)
{
   for (int i = 0; s && i < max && *pos < cap; i++) {
      char c = atomic_load_explicit(&s[i], memory_order_relaxed);
      if (!c)
         break;
      b[(*pos)++] = c;
   }
}
