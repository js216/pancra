// SPDX-License-Identifier: GPL-3.0
// pwcost.c --- measuring, persisting and obeying the password cost
// Copyright 2026 Jakob Kastelic

/* See pwcost.h for what a cost has to satisfy and why it is written down.
 * What follows is the measurement and the file. */
#include "pwcost.h"

#include "http.h"   /* http_mono_s / HTTP_WORKERS */
#include "posix.h"  /* SYS_PATH_MAX */
#include "proto.h"  /* PW_ITERS_*, PW_SALT_LEN, PW_HASH_LEN */
#include "pwhash.h" /* pw_hash: the thing being timed */
#include <errno.h>
#include <limits.h> /* INT_MAX: the policy fields are ints */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WRITTEN ONCE, AT STARTUP, READ BY EVERY WORKER AFTERWARDS. The ordering is
 * what makes it safe without a lock: pwcost_init runs on the one thread that
 * exists, before the pool is started, and nothing writes it again. */
static int g_iters = PW_ITERS_DEFAULT;

int pwcost_iters(void)
{
   return g_iters;
}

/* The policy file's path, or 0 when there is no directory to put it in. */
static int policy_path(const char *dir, char *out, size_t cap)
{
   if (!dir || !*dir)
      return 0;
   int n = snprintf(out, cap, "%s/%s", dir, PW_POLICY_NAME);
   return n > 0 && (size_t)n < cap;
}

/* Time ONE hash at `iters`, in milliseconds. -1 when the KDF refused, which
 * is not a slow machine and must not be read as one. */
static double time_hash(int iters)
{
   uint8_t salt[PW_SALT_LEN] = {0};
   uint8_t out[PW_HASH_LEN];
   double t0 = http_mono_s();
   if (!pw_hash("calibration-password", salt, iters, out))
      return -1;
   return (http_mono_s() - t0) * 1000.0;
}

/* THE SAME HASH, `n` OF THEM AT ONCE, and the answer is the SLOWEST -- that
 * is what a login waits for when the pool is busy. Threads rather than an
 * assumption about how the machine scales: on one core the sum is serialised
 * and every thread waits for all of it, which is exactly the fact worth
 * measuring. -1 when a thread could not be started, because a concurrency
 * measurement that quietly ran one hash would talk the calibration into a
 * cost the board cannot carry. */
struct hasher {
   double ms;
   int ok;
};

static void *hash_thread(void *arg)
{
   struct hasher *h = arg;
   h->ms            = time_hash(PW_ITERS_DEFAULT);
   h->ok            = h->ms > 0;
   return NULL;
}

static double time_hashes_busy(int n)
{
   if (n < 1)
      n = 1;
   if (n > HTTP_WORKERS)
      n = HTTP_WORKERS;
   pthread_t th[HTTP_WORKERS];
   struct hasher h[HTTP_WORKERS];
   int started = 0;
   for (int i = 0; i < n; i++) {
      h[i].ms = 0;
      h[i].ok = 0;
      if (pthread_create(&th[i], NULL, hash_thread, &h[i]) != 0)
         break;
      started++;
   }
   double worst = -1;
   for (int i = 0; i < started; i++) {
      pthread_join(th[i], NULL);
      if (!h[i].ok)
         worst = -1;
      else if (worst >= 0 && h[i].ms > worst)
         worst = h[i].ms;
      else if (worst == 0 || worst < 0)
         worst = h[i].ms;
   }
   if (started != n)
      return -1;
   return worst;
}

enum pwcost_load pwcost_init(const char *dir)
{
   char path[SYS_PATH_MAX];
   if (!policy_path(dir, path, sizeof path))
      return PWCOST_ABSENT;
   FILE *f = fopen(path, "r");
   if (!f) {
      if (errno != ENOENT)
         fprintf(stderr,
                 "sync: %s exists and cannot be read; password hashes will "
                 "use the compiled cost of %d\n",
                 path, PW_ITERS_DEFAULT);
      return errno == ENOENT ? PWCOST_ABSENT : PWCOST_BAD;
   }
   /* THE WHOLE FILE IS ONE LINE, and it is parsed strictly: a policy that
    * cannot be read completely is not half a policy.
    *
    * READ, THEN CONVERTED, rather than scanned: a %d that overflows reports
    * nothing at all, and the number it leaves behind is the one this server
    * would then hash every password with. strtol says so.
    */
   char line[128];
   char *rd = fgets(line, sizeof line, f);
   fclose(f);
   int version = 0;
   int iters   = 0;
   int ms      = 0;
   int got     = 0;
   if (rd) {
      static const char tag[] = "pancra-pwcost";
      const char *p           = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (strncmp(p, tag, sizeof tag - 1) == 0) {
         p += sizeof tag - 1;
         long field[3] = {0, 0, 0};
         for (got = 0; got < 3; got++) {
            char *end  = NULL;
            errno      = 0;
            field[got] = strtol(p, &end, 10);
            if (end == p || errno == ERANGE || field[got] < 0 ||
                field[got] > INT_MAX)
               break;
            p = end;
         }
         if (got == 3) {
            version = (int)field[0];
            iters   = (int)field[1];
            ms      = (int)field[2];
         }
      }
   }
   if (got != 3) {
      fprintf(stderr,
              "sync: %s is not a cost policy this build can read; using the "
              "compiled cost of %d\n",
              path, PW_ITERS_DEFAULT);
      return PWCOST_BAD;
   }
   if (version != PW_POLICY_VERSION) {
      fprintf(stderr,
              "sync: %s is version %d and this build speaks %d; using the "
              "compiled cost of %d. Re-run `sync bench <datadir>`.\n",
              path, version, PW_POLICY_VERSION, PW_ITERS_DEFAULT);
      return PWCOST_BAD;
   }
   /* THE RANGE IS CHECKED AGAINST THE SAME BOUNDS A STORED ROW IS. A policy
    * naming a count outside them would write credentials this server then
    * refuses to verify -- an account created and immediately unusable. */
   if (iters < PW_ITERS_MIN || iters > PW_ITERS_MAX) {
      fprintf(stderr,
              "sync: %s asks for %d iterations, which is outside %d..%d; "
              "using the compiled cost of %d\n",
              path, iters, PW_ITERS_MIN, PW_ITERS_MAX, PW_ITERS_DEFAULT);
      return PWCOST_BAD;
   }
   /* NEVER BELOW THE COMPILED FLOOR. The default is a judgement about what
    * makes a stolen database expensive, and a policy measured on a slow board
    * must not be allowed to talk this server into a cheaper one -- the
    * attacker's machine is not the board. */
   if (iters < PW_ITERS_DEFAULT) {
      fprintf(stderr,
              "sync: %s asks for %d iterations, below the compiled floor of "
              "%d; the floor is used\n",
              path, iters, PW_ITERS_DEFAULT);
      g_iters = PW_ITERS_DEFAULT;
      return PWCOST_OK;
   }
   g_iters = iters;
   printf("sync: password cost %d iterations (measured %d ms, budget %d ms)\n",
          iters, ms, PW_TARGET_MS);
   return PWCOST_OK;
}

int pwcost_calibrate(const char *dir, int workers)
{
   char path[SYS_PATH_MAX];
   if (!policy_path(dir, path, sizeof path)) {
      fprintf(stderr, "sync: no data directory to write the cost policy in\n");
      return 1;
   }
   /* MEASURE AT THE COMPILED DEFAULT, THEN SCALE. PBKDF2's cost is linear in
    * the iteration count -- that is the whole of what the parameter does --
    * so timed runs give the rate, and the answer is a division rather than a
    * search that would take a second per step.
    *
    * TIMED TWICE, and the SECOND one counts: the first pass pays for cold
    * caches and a page fault per buffer, which on this hardware is tens of
    * milliseconds and would talk the calibration into a cost well under what
    * the machine can carry. */
   double warm = time_hash(PW_ITERS_DEFAULT);
   double ms   = time_hash(PW_ITERS_DEFAULT);
   if (warm < 0 || ms <= 0) {
      fprintf(stderr, "sync: the password KDF refused its own defaults\n");
      return 1;
   }
   double per = ms / (double)PW_ITERS_DEFAULT; /* ms per iteration, idle */

   /* AND AGAIN WITH THE POOL HASHING, which is the measurement that decides
    * on the hardware this runs on. `workers` threads hash at once and the
    * SLOWEST is what a login waits for; on one core that is `workers` times
    * the idle figure, and no amount of arithmetic about fractions of a pool
    * would have found that out. */
   double busy_ms = time_hashes_busy(workers);
   if (busy_ms <= 0) {
      fprintf(stderr, "sync: the concurrent measurement did not run; no "
                      "policy was written\n");
      return 1;
   }
   double per_busy = busy_ms / (double)PW_ITERS_DEFAULT;

   /* TWO CEILINGS, AND THE LOWER WINS. */
   double by_latency = (double)PW_TARGET_MS / per;
   double by_pool    = (double)PW_BUSY_MS / per_busy;
   double want       = by_latency < by_pool ? by_latency : by_pool;

   long iters = (long)want;
   /* Rounded DOWN to a hundred: the third significant figure of a
    * measurement taken once on a loaded machine is noise, and a policy that
    * reads 12,700 rather than 12,743 is one an operator can compare with the
    * next one. */
   iters -= iters % 100;
   if (iters < PW_ITERS_DEFAULT)
      iters = PW_ITERS_DEFAULT; /* the floor: see pwcost_init */
   if (iters > PW_ITERS_MAX)
      iters = PW_ITERS_MAX;

   double chosen_ms = per * (double)iters;
   printf("PBKDF2-HMAC-SHA256 at %d iterations on this machine:\n",
          PW_ITERS_DEFAULT);
   printf("  idle:   %.0f ms\n", ms);
   printf("  busy:   %.0f ms with %d hashing at once (the pool)\n", busy_ms,
          workers);
   printf("  budget: %d ms idle, %d ms busy\n", PW_TARGET_MS, PW_BUSY_MS);
   printf("  chosen: %ld iterations (~%.0f ms idle, ~%.0f ms busy)\n", iters,
          chosen_ms, per_busy * (double)iters);

   /* WRITTEN WHOLE OR NOT AT ALL. A half-written policy is one the next
    * startup refuses, which would leave the server on the compiled default
    * with an alarming line about a damaged file -- correct, and avoidable. */
   char tmp[SYS_PATH_MAX];
   int n = snprintf(tmp, sizeof tmp, "%s.new", path);
   if (n <= 0 || (size_t)n >= sizeof tmp) {
      fprintf(stderr, "sync: the path to %s is too long\n", path);
      return 1;
   }
   FILE *f = fopen(tmp, "w");
   if (!f) {
      fprintf(stderr, "sync: cannot write %s\n", tmp);
      return 1;
   }
   int wrote = fprintf(f, "pancra-pwcost %d %ld %.0f\n", PW_POLICY_VERSION,
                       iters, chosen_ms);
   /* fflush BEFORE the rename, and the close is checked: a policy still in a
    * stdio buffer is a policy that is not there. The close happens whatever
    * the write did -- a stream left open on the failing path is a descriptor
    * this process never gets back. */
   int kept = (wrote > 0) && fflush(f) == 0;
   if (fclose(f) != 0)
      kept = 0;
   if (!kept) {
      fprintf(stderr, "sync: the cost policy was not written to %s\n", tmp);
      (void)remove(tmp);
      return 1;
   }
   if (rename(tmp, path) != 0) {
      fprintf(stderr, "sync: cannot publish %s\n", path);
      (void)remove(tmp);
      return 1;
   }
   printf("  written: %s\n", path);
   printf("  RESTART the server for it to take effect.\n");
   return 0;
}
