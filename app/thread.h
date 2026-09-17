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
 * ONE SET OF PRIMITIVES, each with its memory ordering stated once, here,
 * rather than re-argued at every call site. `volatile` appears nowhere in that
 * set: it is not a synchronization primitive -- it orders nothing between
 * threads and prevents no reordering by the CPU -- so a `volatile int` shared
 * across threads is correct only where somebody also wrote __atomic_ around it,
 * and nothing marks the places where they did not.
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
 *      alarm_lk and driver_lk are both held across work that blocks: blocking
 *      JNI in both cases -- MediaPlayer start/stop, and connectGatt -- and, for
 *      driver_lk, FILE I/O as well, because a notification callback runs
 *      ot_drv_done -> meter_index_save under it, which is two fsyncs, a rename
 *      and a directory fsync. Neither lock may give up: the property alarm_lk
 *      enforces is that raising and silencing an alarm are mutually exclusive
 *      END TO END, JNI call included, and a waiter that timed out and proceeded
 *      would be exactly the interleaving that once left an alarm looping with
 *      nothing able to stop it.
 *
 *      SO THE MAIN THREAD DOES NOT WAIT FOR EITHER. The spins yield, which
 *      bounds nothing on its own -- 10 to 100 ms of flash is 10 to 100 ms of
 *      frozen UI, dozens of times a second while a finger scrubs a plot. What
 *      keeps the frame out of that is driver_try_enter (dexdriver.h): the
 *      builder takes driver_lk only if it is free and keeps the previous
 *      instant's driver picture when it is not, the same trade model_frame
 *      already makes with the history lock.
 *
 *      The history lock is NOT on that list: it never spans the compositor's
 *      dequeueBuffer, which is what draw() in main.c is arranged to avoid.
 *
 *      IT DOES SPAN FILE I/O, though, and that is worth knowing before
 *      anything else is added under it: build_plot calls plot_source_from,
 *      which opens and reads readings.csv for any span past the live window,
 *      and re-reads the whole log whenever the file has grown. So a binder
 *      thread appending a reading can wait on flash, not merely on a memcpy.
 *      Nothing else under this lock may add to that.
 *   6. THE LOCK ORDER, when more than one is held:
 *
 *          driver_lk  (dexlink.c)   ->  reg_lk  (sensors.c)  ->  g_hist_lk
 *          alarm_lk   (alarm.c)     ->  set_lk  (setfile.c)
 *
 *      alarm_lk TAKES NOTHING BUT set_lk, and nothing may ever be nested
 *      inside it besides that: it is held across blocking JNI (MediaPlayer),
 *      so a second lock under it puts a JNI call under two. The one edge that
 *      exists is alarm_set_threshold reading the partner threshold under the
 *      alarm lock (alarm.c), which setfile.c documents from its own side.
 *      NOTHING may take alarm_lk while holding set_lk -- that is the cycle.
 *
 *      ...and then THE LEAVES, every one of which is taken innermost and is
 *      never held across another module's call:
 *
 *          set_lk    (setfile.c)      msess_lk  (metersess.c)
 *          cal_lk    (calib.c)        mrt_lk    (meterstore.c)
 *          bond_lk   (bondtable.c)    g_cfg_lk  (sync.c)
 *          bondmac_lk (dexble.c)      g_devlist_lk (pairing.c)
 *          rssi_lk   (linkinfo.c)     g_status_lk (model.c)
 *          g_sched_lk (remote.c)      g_rstat_lk (remote.c)
 *          tag_lk    (devtag.c)       steptail_lk (steps.c)
 *          sessc_lk  (sesscache.c)
 *          ins_lk    (insulin.c)      wt_lk     (weight.c)
 *          food_lk   (food.c)         extail_lk (exercise.c)
 *          ex_lk     (exercise.c)
 *          dis_lk (linkinfo.c)        mdis_lk (meter.c)
 *          append_lk (util.c)
 *
 *      THE LOG LOCKS are the newest of them -- ins_lk, wt_lk, food_lk, ex_lk
 *      and extail_lk, with steptail_lk beside them. Each guards
 *      one log's published state -- the tail, and for food the vocabulary
 *      and the picker order with it -- and each is taken by a reader on the
 *      MAIN thread and by a loader on the SYNC WORKER, which is the pairing
 *      that made them necessary: a restore reloads every log, and it does
 *      not run on the thread that draws them. None is held across file I/O:
 *      every writer appends or rewrites first and takes the lock only to
 *      bring memory into line, and every loader builds a separate state and
 *      publishes it with one assignment.
 *
 *      AND EACH LOADER HAS A STAGING LOCK ABOVE ITS TAIL LOCK -- wt_stage_lk,
 *      ins_stage_lk, food_stage_lk, ex_stage_lk, step_stage_lk. The buffer a
 *      loader parses into is a static (a tail is thousands of rows and this
 *      runs on a service thread), so the two threads named above can be inside
 *      one loader at once: both reset it and refill it, and the tail that gets
 *      published is then a splice of two parses. The publish was already atomic
 *      -- one assignment under the tail lock -- but the FILL was not. Each
 *      staging lock is held across parse AND publish, so the nesting is always
 *      stage -> tail and never the reverse; no caller holds a tail lock across
 *      a load.
 *
 *      exercise.c has TWO leaves -- extail_lk over the log and ex_lk over
 *      the shortcut button's pending state -- and nothing in that file ever
 *      holds one while taking the other.
 *
 *      view_lk (sensors.c) is a TRUE LEAF, and the only lock in this file
 *      that may be taken under any other. It guards the published registry
 *      view and its reference count, and it is held for a pointer copy and an
 *      increment -- no file I/O, no callback, no second lock. The rankings
 *      that exist are therefore all one-way into it:
 *
 *        reg_lk -> view_lk        the publisher swaps the pointer
 *        driver_lk -> view_lk     the transport's connect callback runs under
 *                                 driver_lk and resolves the connecting
 *                                 address against the view (meter.c,
 *                                 meter_hook_connected)
 *
 *      NOT g_hist_lk -> view_lk, and the frame is arranged so it stays that
 *      way: snap_registry is the only place the builder refs the view, and
 *      model_frame runs the whole snapshot BEFORE it takes the history lock.
 *      A ref moved inside build_model would add that edge -- harmless while
 *      view_lk stays a leaf, and one more thing to keep true.
 *
 *      A reader takes view_lk ALONE and reaches for nothing under it, which
 *      is the whole point -- a reader that had to take reg_lk under it would
 *      be the inversion the published view exists to make impossible, and it
 *      is what lets the two callbacks above ref a view without knowing
 *      anything about the registry's own rank. That is also why
 *      sensors_view_ref cannot build a view when none is published: it never
 *      has to, because the pointer starts at a permanently-live empty one.
 *
 *      EVERY LEAF IS NAMED HERE AS WELL AS BESIDE ITS DECLARATION, which is
 *      the natural place to write such a claim and the one place nobody can
 *      check it from: a leaf documented only in its own file cannot be compared
 *      against the others. A lock missing from this list is a lock whose rank
 *      nothing states. Add one here when you add one anywhere.
 *
 *      TAKEN UNDER THE HISTORY LOCK: a frame holds it across the whole of
 *      build_model, and every leaf the builder touches is therefore nested
 *      inside it -- bond_lk, rssi_lk, cal_lk, mrt_lk, msess_lk, sessc_lk,
 *      ins_lk, wt_lk, food_lk, ex_lk, extail_lk, steptail_lk, g_rstat_lk,
 *      g_status_lk, set_lk and g_devlist_lk. None of them takes another lock,
 *      so none can close a cycle; they are named here because "leaf" says
 *      what a lock does NOT do and this says where it is actually held.
 *
 *      THE WAY TO CHECK THIS LIST is to read what build_model calls, not to
 *      trust it -- the indirect arrivals are the ones a reader misses:
 *      sessc_lk comes through fill_sensor's sessc_restore and build_reading;
 *      wt_lk through build_forms' wt_copy; g_status_lk through build_status.
 *
 *      And once per device row: g_devlist_lk
 *      (pairing.c). A frame holds g_hist_lk across build_model, and
 *      fill_sensor asks pairing_adv_name for each row. It is still a leaf --
 *      every region under it does string and clock work and calls nothing --
 *      so the edge g_hist_lk -> g_devlist_lk closes no cycle; it is written
 *      down because a leaf whose one nesting is recorded only in its own
 *      file is the drift this table exists to stop.
 *
 *      HELD ACROSS FILE I/O, like the ones between the groups above:
 *      append_lk (util.c) serialises appends to a log and is held across the
 *      write itself. It takes nothing, so it ranks with the leaves; the note
 *      is here because "leaf" and "never held across anything slow" are two
 *      different claims and only the first one is true of it.
 *
 *      NOT A LEAF, whatever is convenient to call it:
 *      idx_lk (meterstore.c) is held across sensor_id_is_live -- which takes
 *      reg_lk -- and across the index file's own write. Its rank is
 *      driver -> idx_lk -> reg_lk, one-way because the registry never
 *      reaches back for it.
 *
 *      Two more sit BETWEEN the two groups for the same reason calfile_lk
 *      does: set_file_lk (setfile.c) is taken with set_lk RELEASED and held
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
 *      RELEASED across the flash. That is what keeps a publish -- and so
 *      the next frame to take a reference -- from waiting on an fsync while
 * still making an id unique for ever: two mints cannot both read the same
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
 *      declares them. The alarm lock sits outside the driver/registry/history
 *      order on purpose: it is held across blocking JNI (MediaPlayer), which
 *      is why the one lock nested under it is a leaf that touches no file
 *      handle and no other module.
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

/* TAKE IT ONLY IF IT IS FREE, or if we already hold it. 1 when the caller now
 * holds it (and owes an unlock), 0 when somebody else does.
 *
 * FOR A CALLER WITH SOMETHING BETTER TO DO THAN WAIT. rmutex_lock spins with a
 * yield, so a thread that takes it behind a holder doing file I/O waits for the
 * whole of that I/O -- which is the right trade for a writer and the wrong one
 * for a frame that is about to be built again anyway. */
static inline int rmutex_trylock(struct rmutex *m)
{
   int me = gettid();
   if (atomic_load_explicit(&m->owner, memory_order_relaxed) == me) {
      m->depth++;
      return 1;
   }
   int expect = 0;
   if (!atomic_compare_exchange_strong_explicit(
           &m->owner, &expect, me, memory_order_acquire, memory_order_relaxed))
      return 0;
   m->depth = 1;
   return 1;
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
