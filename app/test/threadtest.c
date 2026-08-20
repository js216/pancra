// SPDX-License-Identifier: GPL-3.0
// threadtest.c --- the cross-thread primitives, under real threads
// Copyright 2026 Jakob Kastelic

/* WHY THIS EXISTS.
 *
 * app/thread.h is now the only place in the app where one thread hands
 * anything to another: the history lock, the driver lock, the registry lock,
 * every dirty flag and every single-flight latch are four small inline
 * functions in that header. Nothing exercised them. The code they replaced was
 * four hand-rolled copies of the same idea, and the reason it was worth
 * replacing is precisely that a subtle error in one of them -- a
 * compare-exchange that forgets to reset its expected value, an unlock that
 * decrements the wrong counter -- is invisible in review and reproduces once a
 * month on a phone.
 *
 * So: real pthreads, real contention, and assertions about the properties the
 * header PROMISES rather than about the instructions it emits.
 *
 *   - mutual exclusion: a counter incremented non-atomically under the lock by
 *     N threads ends at exactly N * iterations. This is the assertion that
 *     fails if the lock does not lock; nothing else here would.
 *   - recursion: rmutex re-entered by its holder does not deadlock, and stays
 *     held until the outermost unlock -- checked by another thread, which must
 *     NOT get in between them.
 *   - trylock: never blocks, and reports failure when it fails.
 *   - drain: returns 1 when the lock is free, 0 on timeout, and takes at least
 *     the timeout to say so -- a drain that returns 0 immediately would make
 *     every teardown wait a no-op.
 *   - flag: every raise taken by exactly ONE taker. This is the property the
 *     notification path depends on and the one a plain load-then-store breaks.
 *     Its first version passed against a deliberately broken take; the comment
 *     above `raiser` says why, and is the most useful thing in this file.
 *   - flight: exactly one runner at a time, and the losers LEAVE rather than
 *     queue.
 */
#include "thread.h"
#include "clock.h"
#include "util.h"    /* now_ms, for the drain's timing assertion */
#include <pthread.h> /* pthread_t, pthread_create: real threads, not a model */
#include <sched.h>   /* sched_yield: the spins here are the app's own */
#include <stdatomic.h> /* the fixtures race deliberately, so they are atomic */
#include <stdio.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- mutual exclusion ------------------------------------------------- */

#define NTHREAD 8
#define NITER   20000
/* Fewer, because each one yields: see mutator(). */
#define MUTITER 4000

static struct mutex m_count = MUTEX_INIT;
/* DELIBERATELY NOT ATOMIC. The whole claim under test is that the lock makes
 * a plain read-modify-write safe; making the counter atomic would make this
 * test pass with no lock at all. */
static long g_count;

static void *count_up(void *arg)
{
   (void)arg;
   for (int i = 0; i < NITER; i++) {
      mutex_lock(&m_count);
      g_count++;
      mutex_unlock(&m_count);
   }
   return 0;
}

/* ---- recursion, and what another thread may observe -------------------- */

static struct rmutex m_rec = RMUTEX_INIT;
/* Raised by the intruder if it ever holds m_rec while the owner believes it
 * still does. Atomic: the two threads genuinely race on it, which is the
 * point. */
static atomic_int g_intruded;
static atomic_int g_owner_inside;

static void *intruder(void *arg)
{
   (void)arg;
   rmutex_lock(&m_rec);
   if (atomic_load_explicit(&g_owner_inside, memory_order_acquire))
      atomic_store_explicit(&g_intruded, 1, memory_order_release);
   rmutex_unlock(&m_rec);
   return 0;
}

/* ---- flags: a raise is taken by exactly one taker ----------------------
 *
 * THE ASSERTION IS AN INEQUALITY, and it took a mutation to find out why.
 *
 * The obvious test -- raise once, let eight threads race to take it, require
 * exactly one success -- passes even when flag_take is written as a load
 * followed by a store, because the window between the two is a handful of
 * instructions and eight threads starting at slightly different times almost
 * never land inside it. A test that only fails when the scheduler cooperates
 * is not a test of anything.
 *
 * What separates the two implementations is a COUNT, and the counting only
 * works if every raise is its own contention point. A raiser that just spins
 * raising gives `takes <= raises` for free: a raise landing on an
 * already-raised flag is serviced by the same single take, so takes falls far
 * below raises and the inequality holds however broken the take is. (Tried;
 * the mutant passed.)
 *
 * So the raiser raises ONE at a time and waits for it to be consumed before
 * raising the next. Now every raise is a flag going 0 -> 1 with seven takers
 * already spinning on it, and the contract is an EQUALITY:
 *
 *     takes == raises
 *
 * An exchange gives that for every interleaving -- exactly one taker can turn
 * a 1 into a 0. A load-then-store breaks it in both directions: two takers
 * reading the same 1 both report success (one raise handed out twice), and a
 * taker that reads 0 and stores 0 anyway can wipe a raise that landed between
 * its two instructions (a raise nobody is told about). Measured against the
 * mutant, the second is the more common of the two.
 */
static struct flag f_shared = FLAG_INIT;
static atomic_int g_takes;
static atomic_int g_raises;
static atomic_int g_raiser_done;

#define NRAISE 20000

static void *raiser(void *arg)
{
   (void)arg;
   for (int i = 0; i < NRAISE; i++) {
      flag_raise(&f_shared);
      atomic_fetch_add_explicit(&g_raises, 1, memory_order_relaxed);
      /* Wait for a taker to consume it, so the next raise is a fresh 0 -> 1
       * edge rather than a store onto a flag that is already up. */
      while (flag_peek(&f_shared))
         sched_yield();
   }
   atomic_store_explicit(&g_raiser_done, 1, memory_order_release);
   return 0;
}

static void *taker(void *arg)
{
   (void)arg;
   while (!atomic_load_explicit(&g_raiser_done, memory_order_acquire))
      if (flag_take(&f_shared))
         atomic_fetch_add_explicit(&g_takes, 1, memory_order_relaxed);
   /* Drain whatever the raiser left behind, so the final state is settled. */
   while (flag_take(&f_shared))
      atomic_fetch_add_explicit(&g_takes, 1, memory_order_relaxed);
   return 0;
}

/* ---- flight: one runner, and the rest leave --------------------------- */

static struct flight fl = FLIGHT_INIT;
static atomic_int g_entered, g_concurrent, g_max_concurrent;

/* THE OVERLAP IS ARRANGED, NOT HOPED FOR.
 *
 * The assertion below is that some callers were REFUSED, and refusal can only
 * happen while another thread is inside. The region is three atomics wide, so
 * whether any two of these threads are ever inside it at the same moment was
 * left to the scheduler -- and on a machine that is oversubscribed (`make -j8`
 * on four cores, which is exactly what the parallel gate does) each thread ran
 * its whole 20000-iteration loop inside one time slice, never overlapping.
 * Every attempt then succeeded and the suite failed on a true statement about
 * the code under test. It was a real failure of this fixture, not of
 * thread.h.
 *
 * So the FIRST thread in holds the region until a refusal has actually been
 * recorded, with a deadline so it cannot hang. This does not weaken the check:
 * a flight_enter that QUEUED instead of refusing would leave g_refused at zero,
 * the holder would leave on the deadline, and every queued caller would then
 * enter -- which is the failure this asserts on. */
static atomic_int g_refused;

static atomic_flag g_first_in = ATOMIC_FLAG_INIT;

#define FLIGHT_WAIT_MS 500

static void *flier(void *arg)
{
   (void)arg;
   for (int i = 0; i < NITER; i++) {
      if (!flight_enter(&fl)) {
         atomic_fetch_add_explicit(&g_refused, 1, memory_order_relaxed);
         continue;
      }
      atomic_fetch_add_explicit(&g_entered, 1, memory_order_relaxed);
      int now =
          atomic_fetch_add_explicit(&g_concurrent, 1, memory_order_acq_rel) + 1;
      /* A high-water mark. Any value above 1 means two runners were inside
       * the single-flight region at once, which is the one thing it exists to
       * prevent. */
      int seen = atomic_load_explicit(&g_max_concurrent, memory_order_relaxed);
      while (now > seen && !atomic_compare_exchange_weak_explicit(
                               &g_max_concurrent, &seen, now,
                               memory_order_relaxed, memory_order_relaxed))
         ;
      /* HELD OPEN, ONCE, until somebody has been turned away -- inside the
       * counted region, so the high-water mark above is measured over a window
       * the other seven threads can actually be seen in rather than over three
       * instructions. */
      if (!atomic_flag_test_and_set(&g_first_in)) {
         long long until = now_ms() + FLIGHT_WAIT_MS;
         while (!atomic_load_explicit(&g_refused, memory_order_relaxed) &&
                now_ms() < until)
            sched_yield();
      }
      atomic_fetch_sub_explicit(&g_concurrent, 1, memory_order_acq_rel);
      flight_leave(&fl);
   }
   return 0;
}

/* A holder that keeps a mutex for `g_hold_ms` milliseconds, so drain has
 * something real to wait on and to time out against. */
static struct mutex m_drain = MUTEX_INIT;
static int g_hold_ms;

static void *holder(void *arg)
{
   (void)arg;
   mutex_lock(&m_drain);
   long long until = now_ms() + g_hold_ms;
   while (now_ms() < until)
      sched_yield();
   mutex_unlock(&m_drain);
   return 0;
}

/* One thread's worth of committed records.
 *
 * The yield is what makes this a TEST rather than a micro-benchmark: without
 * it a compiler is free to turn twenty thousand increments of a plain global
 * into one addition, which no two threads can then interleave inside -- the
 * defect would be real in the app (where every commit is separated by a file
 * write) and invisible here. Yielding between commits reproduces that
 * separation. */
static void *mutator(void *p)
{
   (void)p;
   for (int i = 0; i < MUTITER; i++) {
      record_mutated();
      sched_yield();
   }
   return NULL;
}

static int spawn_join(void *(*fn)(void *), int n)
{
   /* <pthread.h> IS included above. glibc defines pthread_t in
    * <bits/pthreadtypes.h> with no IWYU pragma pointing back at its public
    * header, so include-cleaner asks for a private glibc header by name --
    * including which would be the actual defect. Silenced on this one line
    * rather than for the file. */
   /* NOLINTNEXTLINE(misc-include-cleaner) */
   pthread_t t[NTHREAD] = {0};
   if (n > NTHREAD)
      n = NTHREAD;
   for (int i = 0; i < n; i++)
      if (pthread_create(&t[i], 0, fn, 0) != 0)
         return 0;
   for (int i = 0; i < n; i++)
      pthread_join(t[i], 0);
   return 1;
}

int main(void)
{
   printf("== a mutex actually excludes ==\n");
   ck(spawn_join(count_up, NTHREAD), "eight threads started");
   /* If the lock does not lock, this number is SMALLER than the total -- lost
    * updates -- and by an amount that varies per run. It is never larger. */
   ck(g_count == ((long)NTHREAD * NITER),
      "every increment under the lock survived");
   if (g_count != ((long)NTHREAD * NITER))
      printf("       (%ld of %ld -- %ld updates were lost)\n", g_count,
             (long)NTHREAD * NITER, ((long)NTHREAD * NITER) - g_count);

   printf("== trylock reports failure rather than waiting ==\n");
   {
      struct mutex m = MUTEX_INIT;
      ck(mutex_trylock(&m), "a free mutex is taken");
      ck(!mutex_trylock(&m), "...and a held one is refused, not waited for");
      mutex_unlock(&m);
      ck(mutex_trylock(&m), "...and is takeable again once released");
      mutex_unlock(&m);
   }

   printf("== a recursive lock re-enters itself and nobody else gets in ==\n");
   {
      pthread_t t = 0;
      rmutex_lock(&m_rec);
      rmutex_lock(&m_rec); /* would deadlock on a non-recursive lock */
      rmutex_lock(&m_rec);
      ck(rmutex_held_by_me(&m_rec), "the holder knows it holds it");
      atomic_store_explicit(&g_owner_inside, 1, memory_order_release);
      ck(pthread_create(&t, 0, intruder, 0) == 0, "an intruder thread starts");
      /* Two of the three unlocks: still held, so the intruder must still be
       * waiting. A depth counter that decremented to zero early would let it
       * in right here. */
      rmutex_unlock(&m_rec);
      rmutex_unlock(&m_rec);
      long long until = now_ms() + 50;
      while (now_ms() < until)
         sched_yield();
      ck(!atomic_load_explicit(&g_intruded, memory_order_acquire),
         "...and cannot enter while the outer hold is still open");
      atomic_store_explicit(&g_owner_inside, 0, memory_order_release);
      rmutex_unlock(&m_rec); /* the outermost: now it is free */
      pthread_join(t, 0);
      ck(!atomic_load_explicit(&g_intruded, memory_order_acquire),
         "...and got in only after it was fully released");
      ck(!rmutex_held_by_me(&m_rec), "the lock is free afterwards");
   }

   printf("== a bounded drain waits, and then stops waiting ==\n");
   {
      ck(mutex_drain(&m_drain, 10) == 1, "a free lock drains at once");

      /* Held for longer than the bound: drain must give up and SAY so. */
      g_hold_ms   = 400;
      pthread_t t = 0;
      ck(pthread_create(&t, 0, holder, 0) == 0, "a holder thread starts");
      /* Wait until it really has the lock, or this measures nothing. */
      long long until = now_ms() + 1000;
      while (mutex_trylock(&m_drain) && (mutex_unlock(&m_drain), 1) &&
             now_ms() < until)
         sched_yield();
      long long t0 = now_ms();
      int got      = mutex_drain(&m_drain, 50);
      long long dt = now_ms() - t0;
      ck(got == 0, "a lock held past the bound times out");
      /* AND IT TOOK THE TIME. A drain that returns 0 immediately satisfies the
       * line above while making every teardown wait a no-op -- exactly the
       * shape of test that passes while the thing it names does nothing. */
      ck(dt >= 45, "...after actually waiting for the bound");
      if (dt < 45)
         printf("       (gave up after only %lld ms)\n", dt);
      pthread_join(t, 0);
      ck(mutex_drain(&m_drain, 100) == 1,
         "...and reports success once the holder is done");
   }

   printf("== a raised flag is taken exactly once ==\n");
   {
      flag_raise(&f_shared);
      ck(flag_peek(&f_shared), "the flag reads as raised before anyone takes");
      ck(flag_take(&f_shared), "...and the first taker gets it");
      ck(!flag_take(&f_shared), "...and the second does not");

      /* The real one: sustained contention, and an inequality no
       * load-then-store implementation can satisfy. See the comment on
       * `raiser` for why the obvious version of this test passes when the
       * primitive is broken. */
      pthread_t r = 0;
      atomic_store_explicit(&g_raises, 0, memory_order_relaxed);
      atomic_store_explicit(&g_takes, 0, memory_order_relaxed);
      ck(pthread_create(&r, 0, raiser, 0) == 0, "a raiser thread starts");
      ck(spawn_join(taker, NTHREAD - 1), "seven takers started");
      pthread_join(r, 0);
      int raises = atomic_load_explicit(&g_raises, memory_order_relaxed);
      int takes  = atomic_load_explicit(&g_takes, memory_order_relaxed);
      ck(raises == NRAISE, "the raiser raised every one of them");
      ck(takes == raises, "each raise was taken by exactly one taker");
      /* It can miss in EITHER direction, and both are the same defect. Too
       * many takes is one raise reported to two takers; too few is a take that
       * cleared the flag without reporting it -- a reading marked dirty and
       * silently dropped, which on the notification path means a glucose value
       * the lock screen never shows. */
      if (takes != raises)
         printf("       (%d takes from %d raises: %d %s)\n", takes, raises,
                takes > raises ? takes - raises : raises - takes,
                takes > raises ? "handed out more than once"
                               : "raises were swallowed");
      ck(!flag_peek(&f_shared), "...and the flag is clear afterwards");
      flag_raise(&f_shared);
      flag_clear(&f_shared);
      ck(!flag_take(&f_shared), "a cleared flag is not takeable");
   }

   printf("== single flight admits one runner at a time ==\n");
   {
      ck(spawn_join(flier, NTHREAD), "eight fliers started");
      ck(atomic_load_explicit(&g_max_concurrent, memory_order_relaxed) == 1,
         "never two inside the single-flight region at once");
      if (atomic_load_explicit(&g_max_concurrent, memory_order_relaxed) != 1)
         printf("       (%d were inside together)\n",
                atomic_load_explicit(&g_max_concurrent, memory_order_relaxed));
      /* Losers LEAVE. If flight_enter queued instead of refusing, every one of
       * the 8 * NITER attempts would have entered. The overlap this needs is
       * arranged by flier() rather than left to the scheduler -- see the
       * comment there, and note that a queueing flight_enter still fails this
       * assertion. */
      ck(atomic_load_explicit(&g_entered, memory_order_relaxed) <
             ((long)NTHREAD * NITER),
         "callers that could not enter left instead of queueing");
      if (!atomic_load_explicit(&g_refused, memory_order_relaxed))
         printf("       (nobody was turned away in %d ms: the fixture never "
                "achieved overlap)\n",
                FLIGHT_WAIT_MS);
      ck(!flight_busy(&fl), "the latch is clear when nothing is running");
      ck(flight_enter(&fl), "...and a fresh caller can enter");
      ck(flight_busy(&fl), "...which reads as busy");
      ck(!flight_enter(&fl), "...and a second caller is refused");
      flight_leave(&fl);
   }

   printf("== every committed mutation is counted, from every thread ==\n");
   {
      /* THE SYNC TRIGGER. record_mutated() is called by whichever thread just
       * committed a record -- a reading on a BINDER thread, a dose or a weight
       * on the MAIN thread -- and the sync worker reads the total to decide
       * whether anything has changed since it last looked.
       *
       * As a plain `long` this was `load, add, store`: two threads inside that
       * window commit two records and announce one. The lost announcement is
       * not noticed by the file sizes either, because the case the counter
       * exists for is exactly the edit whose sizes do not move -- so the
       * correction sits on the phone until the six-hour safety sync.
       *
       * Eight threads, twenty thousand each: the total is exact or the
       * counter is not a counter. */
      long before = record_generation();
      ck(spawn_join(mutator, NTHREAD), "eight committing threads started");
      long after = record_generation();
      ck(after - before == (long)NTHREAD * MUTITER,
         "every mutation from every thread is counted, exactly once");
      if (after - before != (long)NTHREAD * MUTITER)
         printf("       (%ld counted of %ld announced: %ld lost)\n",
                after - before, (long)NTHREAD * MUTITER,
                ((long)NTHREAD * MUTITER) - (after - before));
   }

   printf("\n%s\n",
          all ? "ALL THREAD TESTS PASSED" : "SOME THREAD TESTS FAILED");
   return all ? 0 : 1;
}
