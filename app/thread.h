// SPDX-License-Identifier: GPL-3.0
// thread.h --- the app's cross-thread primitives, and the rules for using them
// Copyright 2026 Jakob Kastelic

/* WHY THIS FILE EXISTS.
 *
 * This app has no threads of its own and five that reach into it anyway:
 *
 *   MAIN     the activity's looper. Every ANativeActivity callback, on_timer,
 *            on_input, and every draw. Dies with the activity.
 *   BINDER   the BLE stack's callback threads (adverts, GATT notifications,
 *            connect/disconnect). Several of them, from a small pool.
 *   SERVICE  the foreground service's tick HandlerThread. Outlives the
 *            activity, which is the whole point of it.
 *   WORKER   Ble.remotePush's one-shot thread, which reports a result back.
 *   SIGNAL   the crash handler, which is not a thread but has the same rule:
 *            it may read shared state and may call nothing that locks.
 *
 * Before this file, the shared state between them was `volatile int` plus
 * hand-rolled `__atomic_*(SEQ_CST)` at four separate lock sites, each written
 * slightly differently, and `volatile` was doing the work of documentation.
 * It is not a synchronization primitive: it orders nothing between threads and
 * prevents no reordering by the CPU. Where the code was in fact correct, it
 * was correct because someone had also written __atomic_ around it; where it
 * was not, nothing said so.
 *
 * So: one set of primitives, each with its memory ordering stated once, here,
 * rather than re-argued at every call site.
 *
 * THE RULES
 *
 *   1. A field shared across threads has ONE owner and ONE discipline, and
 *      both are written down where the field is declared. "Owner" is the
 *      thread allowed to write it, or the lock that must be held to.
 *   2. A scalar SIGNAL (a dirty flag, a busy latch, a thread id) is an atomic
 *      with an explicit ordering. Never `volatile`.
 *   3. COMPOUND state (an array and its count, a struct read as a whole) is
 *      behind a lock, or copied to a snapshot under one and read outside.
 *   4. Every spin YIELDS. A binder pool is small and a phone core is slow;
 *      a spin that does not yield can starve the thread it is waiting for.
 *   5. A wait during TEARDOWN is BOUNDED. The main thread may not be parked
 *      indefinitely on another thread's progress -- that is an ANR, and the
 *      user sees it as the app freezing while they close it.
 *
 *      TWO LOCKS ARE DELIBERATELY EXEMPT, and it is worth saying why rather
 *      than leaving the next reader to wonder whether they were missed.
 *      alarm_lk and driver_lk are both held across blocking JNI -- MediaPlayer
 *      start/stop, and connectGatt -- so a thread waiting on either is waiting
 *      on the Android framework. Neither may give up: the property alarm_lk
 *      enforces is that raising and silencing an alarm are mutually exclusive
 *      END TO END, JNI call included, and a waiter that timed out and
 *      proceeded would be exactly the interleaving that once left an alarm
 *      looping with nothing able to stop it. A bounded wait is right when the
 *      fallback is safe; here it is not, so the mitigation is that the spins
 *      yield and that a JNI call is the ONLY blocking thing under them.
 *
 *      The history lock is NOT on that list: it covers a model build and
 *      nothing else, never the compositor's dequeueBuffer. See draw() in
 *      main.c.
 *   6. THE LOCK ORDER, when more than one is held:
 *
 *          driver_lk  (dexlink.c)   ->  reg_lk  (sensors.c)  ->  g_hist_lk
 *          alarm_lk   (alarm.c)      is taken ALONE
 *
 *      ...and then THE LEAVES, every one of which is taken innermost and is
 *      never held across another module's call:
 *
 *          set_lk    (settings.c)     msess_lk  (metersess.c)
 *          cal_lk    (calib.c)        mrt_lk    (meterstore.c)
 *          bond_lk   (bondtable.c)    g_cfg_lk  (sync.c)
 *          sessc_lk  (sesscache.c)
 *          ins_lk    (insulin.c)      wt_lk     (weight.c)
 *          food_lk   (food.c)         extail_lk (exercise.c)
 *          ex_lk     (exercise.c)
 *          the dis / meter-dis / append locks
 *
 *      THE FOUR LOG LOCKS are the newest of them. Each guards
 *      one log's published state -- the tail, and for food the vocabulary
 *      and the picker order with it -- and each is taken by a reader on the
 *      MAIN thread and by a loader on the SYNC WORKER, which is the pairing
 *      that made them necessary: a restore reloads every log, and it does
 *      not run on the thread that draws them. None is held across file I/O:
 *      every writer appends or rewrites first and takes the lock only to
 *      bring memory into line, and every loader builds a separate state and
 *      publishes it with one assignment.
 *
 *      exercise.c has TWO leaves -- extail_lk over the log and ex_lk over
 *      the shortcut button's pending state -- and nothing in that file ever
 *      holds one while taking the other.
 *
 *      THIS LIST DRIFTED, which is the failure this table exists to prevent:
 *      bond_lk, g_cfg_lk and the two below were each added with their leaf
 *      claim written only beside their declaration, which is where such a
 *      claim naturally goes. A leaf documented only in its own file is a leaf
 *      nobody can check against the others.
 *      test/app/lockorder.py is the machine-checked copy; keep both.
 *
 *      Two more sit BETWEEN the two groups for the same reason calfile_lk
 *      does: set_file_lk (settings.c) is taken with set_lk RELEASED and held
 *      across the fsyncs and the rename, so no reader of the preferences ever
 *      waits for flash; and g_op_lk (sync.c) is held for a whole sync
 *      operation with g_cfg_lk taken inside it, never the reverse.
 *
 *      TWO MORE OF THE SAME SHAPE, added with the two lost-update fixes:
 *      sessfile_lk (sesscache.c) is taken with sessc_lk RELEASED and held
 *      across the session cache's rename, and msync_lk (meterstore.c) is
 *      taken OUTSIDE mrt_lk and held from the render right through the
 *      rename. The second is the odd one out and it is deliberate: the meter
 *      save has to SERIALISE ITS CALLERS THROUGH COMPLETION, or a caller that
 *      renders behind an in-flight writer is told its meter's last-sync time
 *      was persisted when no buffer anywhere held it. Its callers are all
 *      binder callbacks; MAIN never reaches it. Neither file lock is ever
 *      taken by a reader, and nothing takes a state lock while holding one.
 *
 *      THE REGISTRY HAS ONE TOO, and it is the odd one in this list because
 *      it sits ABOVE a ranked lock rather than beside a leaf: regfile_lk
 *      (sensors.c) is taken OUTSIDE reg_lk and held from the render -- or the
 *      id reservation -- through the write to the publish, with reg_lk
 *      RELEASED across the flash. That is what keeps a frame (which takes
 *      reg_lk through sensors_view_get) from waiting on an fsync while still
 *      making an id unique for ever: two mints cannot both read the same
 *      maxid, because the second does not begin until the first has
 *      published. Order: driver -> regfile_lk -> reg_lk -> history.
 *
 *      One lock sits BETWEEN the two groups below: calfile_lk (calib.c) is
 *      taken OUTSIDE cal_lk and is the only one held across file I/O. That is
 * the point of it -- cal_lk is what a frame takes, so the fsyncs and the rename
 * a save ends in must happen with cal_lk released and calfile_lk still held.
 *
 *      Written HERE rather than in a comment inside sensors.c, which is not
 *      where somebody adding a lock to main.c would look, and because leaves
 *      get added one at a time with their order stated only in the file that
 *      declares them.
 *      test/app/lockorder.py checks this table against the code. The alarm
 *      lock is outside this order on purpose: it is held across blocking JNI
 *      (MediaPlayer), so nesting anything inside it would put a JNI call
 *      under two locks.
 *
 * There are no condition variables here on purpose: the native build is
 * freestanding and declares its libc by hand (dexlibc.h), so pthread_cond_t
 * would mean transcribing bionic's private struct layout and trusting it to
 * stay put. The waits that would use one are all teardown waits, and rule 5
 * says those are bounded -- so a bounded yielding handoff is both sufficient
 * and the thing with no ABI to get wrong.
 */
#ifndef PANCRA_THREAD_H
#define PANCRA_THREAD_H

#include "clock.h"   /* now_ms: mutex_drain is the app's only bounded wait */
#include "dexlibc.h" /* sched_yield, gettid */
#include <stdatomic.h>

/* ---- flags: one scalar, one meaning, any number of threads ---------------
 *
 * The producer RELEASES (everything it wrote before raising the flag is
 * visible to whoever sees the flag) and the consumer ACQUIRES. `take` is an
 * exchange rather than a load-then-store because two consumers exist for some
 * of these -- the activity's timer and the service tick -- and a reading must
 * not be marked dirty and cleared by one of them without ever being rendered.
 *
 * Called `flag` and not `signal` on purpose: `signal` is a libc function, and
 * a struct sharing its name reads as a POSIX signal in a file whose header
 * comment is already about the crash handler.
 */
struct flag {
   atomic_int raised;
};

#define FLAG_INIT {0}

static inline void flag_raise(struct flag *s)
{
   atomic_store_explicit(&s->raised, 1, memory_order_release);
}

/* 1 if it was raised, and clears it. Exactly one caller gets the 1. */
static inline int flag_take(struct flag *s)
{
   return atomic_exchange_explicit(&s->raised, 0, memory_order_acquire);
}

static inline int flag_peek(struct flag *s)
{
   return atomic_load_explicit(&s->raised, memory_order_acquire);
}

static inline void flag_clear(struct flag *s)
{
   atomic_store_explicit(&s->raised, 0, memory_order_release);
}

/* ---- single flight: at most one runner, and a second caller LEAVES --------
 *
 * The existing reconcile/sync/notify paths already had this shape and had it
 * right; it is named here so the next one is not re-invented. The difference
 * from a lock is the whole point: a caller that cannot enter returns at once
 * rather than queueing behind work that is already being done for it.
 */
struct flight {
   atomic_int running;
};

#define FLIGHT_INIT {0}

static inline int flight_enter(struct flight *f)
{
   return !atomic_exchange_explicit(&f->running, 1, memory_order_acquire);
}

static inline void flight_leave(struct flight *f)
{
   atomic_store_explicit(&f->running, 0, memory_order_release);
}

static inline int flight_busy(struct flight *f)
{
   return atomic_load_explicit(&f->running, memory_order_acquire);
}

/* ---- mutex: a leaf lock over compound state ------------------------------
 *
 * Taken alone. Never nested inside another lock, and never held across a call
 * that takes one -- with one documented exception per lock, stated where that
 * lock is declared.
 */
struct mutex {
   atomic_int held;
};

#define MUTEX_INIT {0}

static inline void mutex_lock(struct mutex *m)
{
   /* ACQUIRE on the successful exchange: every write the previous holder made
    * before its releasing unlock is visible to us from here on. */
   while (atomic_exchange_explicit(&m->held, 1, memory_order_acquire))
      sched_yield();
}

static inline int mutex_trylock(struct mutex *m)
{
   return !atomic_exchange_explicit(&m->held, 1, memory_order_acquire);
}

static inline void mutex_unlock(struct mutex *m)
{
   atomic_store_explicit(&m->held, 0, memory_order_release);
}

/* BOUNDED HANDOFF. Waits for the lock to be FREE without taking it, for at
 * most `ms` milliseconds; returns 1 if it went free and 0 on timeout.
 *
 * This is rule 5's tool, and it is the only wait in the app that is allowed to
 * give up. Teardown callbacks run on the main thread, and a wait there that
 * cannot end is an ANR: the app freezes as the user closes it, and the reason
 * is a binder thread halfway through appending a reading.
 */
static inline int mutex_drain(struct mutex *m, int ms)
{
   long long deadline = now_ms() + ms;
   while (atomic_load_explicit(&m->held, memory_order_acquire)) {
      if (now_ms() >= deadline)
         return 0;
      sched_yield();
   }
   return 1;
}

/* ---- rmutex: the same, but re-entrant by the thread that holds it --------
 *
 * Needed where a guarded call can complete synchronously and re-enter the
 * guarded region from inside itself (the BLE driver's write path does exactly
 * this). `depth` is written only by the holder, so it needs no atomicity of
 * its own -- the owner field is what publishes it.
 */
struct rmutex {
   atomic_int owner; /* gettid() of the holder, 0 when free */
   int depth;        /* touched only by the owner */
};

#define RMUTEX_INIT {0, 0}

static inline void rmutex_lock(struct rmutex *m)
{
   int me = gettid();
   /* RELAXED is enough for the re-entry test: reading our OWN id can only
    * happen if we are already the holder, in which case the state we are
    * about to read is state we wrote ourselves and needs no barrier. Any
    * other value (0, or another thread's id) falls through to the exchange
    * below, which is where the ordering is established. */
   if (atomic_load_explicit(&m->owner, memory_order_relaxed) == me) {
      m->depth++;
      return;
   }
   int expect = 0;
   while (!atomic_compare_exchange_weak_explicit(
       &m->owner, &expect, me, memory_order_acquire, memory_order_relaxed)) {
      /* A failed compare-exchange stores what it SAW into `expect`, so it has
       * to be reset or the next attempt asks to replace the current holder's
       * id -- which would hand us a lock somebody else is holding. */
      expect = 0;
      sched_yield();
   }
   m->depth = 1;
}

static inline void rmutex_unlock(struct rmutex *m)
{
   if (--m->depth > 0)
      return;
   atomic_store_explicit(&m->owner, 0, memory_order_release);
}

/* 1 when the CALLING thread holds it. For assertions and for the few paths
 * that must behave differently depending on whether they were re-entered. */
static inline int rmutex_held_by_me(struct rmutex *m)
{
   return atomic_load_explicit(&m->owner, memory_order_relaxed) == gettid();
}

#endif
