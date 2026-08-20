// SPDX-License-Identifier: GPL-3.0
// remote.c --- When to talk to the sync server (see remote.h)
// Copyright 2026 Jakob Kastelic

#include "remote.h"
#include "clock.h"
#include "settings.h"
#include "shell.h"
#include "sync.h"
#include "syncjni.h"
#include "syncreport.h" /* sync_report: this module defines it */
#include "syncstat.h"
#include "thread.h" /* mutex: the sync WORKER writes the status */
#include <stdatomic.h>

/* WHEN TO SYNC AT ALL.
 *
 * Only when something changed. This used to fire every ten seconds forever:
 * with nothing to send it still cost a TLS handshake, a request to the
 * server, and a full local pass over every log to hash it -- several times a
 * minute, on a phone battery, against a single-core board. That is not a
 * sync, it is a slow denial of service against your own server.
 *
 * So: the sizes of the synced files are the trigger. A reading, a dose, an
 * edit or a deletion changes one of them; nothing else does. On top of that
 * there is a rare safety net, because the server could have been restored
 * from an older backup while the phone had nothing new to say, and a
 * BACKOFF, because a failing sync must keep retrying without becoming the
 * ten-second loop again. */
#define REMOTE_MIN_GAP  60L         /* never more often than this */
#define REMOTE_SAFETY   (6L * 3600) /* re-check even with nothing new */
#define REMOTE_FAIL_MIN 60L         /* first retry after a failure */
#define REMOTE_FAIL_MAX (30L * 60)  /* ...doubling up to here */
/* THE OTHER SCHEDULE: a failure only a REPAIR can end -- a name that does not
 * resolve, a refused certificate, a rejected key. A fixed five minutes, and
 * deliberately NOT the climbing backoff above.
 *
 * It used to go straight to REMOTE_FAIL_MAX, the slowest schedule there is,
 * on the reasoning that retrying "cannot help". Two things were wrong with
 * that. The repair is often at the OTHER end -- a certificate renewed on the
 * server is exactly this case, and srv/deploy/README.md tells the operator
 * the phone "does not back off on it, because backing off would not help",
 * which is the opposite of what the code did. And half an hour is long
 * enough that a botched rotation looks, from the phone, indistinguishable
 * from a phone that has given up.
 *
 * Five minutes is one handshake attempt per five minutes: a rounding error
 * against the radio's own traffic, and short enough that a repair anywhere
 * is picked up while somebody is still standing at the keyboard. A repair on
 * THIS end does not wait even that long -- see remote_retry_now. */
#define REMOTE_FIXED_RETRY (5L * 60)

static void remote_sync_locked(void);

/* THE SCHEDULE, AND THE LOCK THAT OWNS IT.
 *
 * These four are read and written from TWO kinds of thread: the decision to
 * start a sync runs on the activity's 1 Hz timer or the service tick, and the
 * RESULT comes back on Java's push worker (sync_report). They were plain
 * longs shared between them.
 *
 * The damage is not academic. sync_report sets g_rem_seen to -1 on failure so
 * the next attempt is not skipped for lack of new data, and sets g_rem_next
 * from the backoff; the tick thread reads both to decide whether to fire.
 * With no ordering between them the tick can see the new g_rem_next and the
 * OLD g_rem_seen -- or a long torn across the two halves it is written in on
 * a 32-bit build -- and the two outcomes are a sync that hammers a server
 * that is already failing, or one that never retries at all.
 *
 * One leaf mutex, taken for the whole decision and for the whole result.
 * Neither critical section calls anything that blocks or takes another lock,
 * so it cannot participate in a cycle. */
static struct mutex g_sched_lk = MUTEX_INIT;
static long g_rem_next;    /* earliest next attempt */
static long g_rem_seen;    /* the state stamp when we last synced */
static long g_rem_backoff; /* current failure backoff, seconds */
static long g_rem_safety;  /* when to look again with nothing new */

static struct flight g_sync_flight = FLIGHT_INIT; /* see pancra_remote_sync */

/* Java -> C: Ble.remotePush got a 2xx from the server (called on its push
 * worker thread, via dexble.c's registered onRemoteOk). */
void pancra_remote_ok(void)
{
   remote_note_ok(realtime_s());
}

/* SINGLE-FLIGHT, because this runs on TWO threads.
 *
 * The activity's 1 Hz on_timer and the service's tick thread both drive the
 * sync, and the Java-side sBusy flag does not serialise them: remoteBusy()
 * is polled at the top while sBusy is not set until remoteBatch/remoteRange
 * is actually entered, and the whole batch build -- three open/lseek/read
 * cycles into one SHARED static body[] -- sits in between. Two threads
 * inside that window fill the same buffer from different file offsets, so
 * the POST can carry a spliced line: a fabricated <epoch> <mg/dL> pair
 * stored as a real reading, after which the acknowledged offset advances
 * past data that was never correctly sent. Every other function reachable
 * from both callers already has such a guard (g_notify_busy,
 * g_reconcile_busy, alarm_lock, the atomic on last_kick); this one was the
 * exception.
 *
 * Skipping rather than waiting is right: the loser has nothing to
 * contribute, and the next tick is at most a minute away. */
void pancra_remote_sync(void)
{
   if (!flight_enter(&g_sync_flight))
      return;
   remote_sync_locked();
   flight_leave(&g_sync_flight);
}

static void remote_sync_locked(void)
{
   /* Ask JAVA to run a sync on its worker; the protocol itself is in sync.c.
    *
    * This used to BE the protocol -- cursors, outbox positions, per-set
    * batches, acknowledgement tags. All of that existed to make a
    * fire-and-forget push lossless. The replica protocol does not need any of
    * it: the phone and the server compare hashes and the phone pushes whole
    * buckets, so "what has the server already got" is answered by the server,
    * exactly, every time, instead of being tracked here and hoped for. */
   /* ONE reading of the three facts that decide whether to push at all: the
    * switch, the server and the identity are written by the settings screen
    * and the pairing worker, and this runs on the tick. Read one at a time
    * they can describe two different configurations -- a push aimed at the
    * old server with the new account's key. ONE acquisition; see
    * remote_config_get. */
   struct remote_config rcfg;
   remote_config_get(&rcfg);
   if (!rcfg.on || !rcfg.server[0] || !rcfg.uid)
      return;
   /* MONOTONIC: the floor, the backoff and the safety net are all intervals.
    * On the wall clock a correction forward skipped straight past the
    * six-hour safety sync, and one backward postponed every retry by however
    * far it moved. */
   long now = mono_s();
   long sz  = syncjni_state_stamp();
   /* DECIDE AND CLAIM IN ONE STEP. Reading the schedule, deciding, and then
    * writing it back as three separate acts let two threads both decide to
    * fire from the same state.
    *
    * The CLAIM is provisional: it stops a second thread firing from the same
    * state while this one is asking Java, and it is rolled back below if Java
    * never took the request. Claiming nothing until the answer came back
    * would reopen the double-fire; keeping the claim regardless is what this
    * item is about. */
   mutex_lock(&g_sched_lk);
   int fresh       = (sz != g_rem_seen);
   int go          = now >= g_rem_next && (fresh || now >= g_rem_safety);
   long was_seen   = g_rem_seen;
   long was_safety = g_rem_safety;
   if (go) {
      g_rem_seen   = sz;
      g_rem_next   = now + REMOTE_MIN_GAP;
      g_rem_safety = now + REMOTE_SAFETY;
   }
   mutex_unlock(&g_sched_lk);
   if (!go)
      return;
   /* OUTSIDE the lock: this reaches Java. */
   if (syncjni_sync_request())
      return;
   /* JAVA NEVER TOOK IT, so no sync is running and no sync_report() is
    * coming. The schedule must go back to what it was, or the next attempt
    * waits out REMOTE_SAFETY -- six hours in which the phone believes a sync
    * is in flight and nothing is. This is the ordinary prompt retry: the
    * floor (REMOTE_MIN_GAP) still applies, because a request Java keeps
    * refusing must not become a spin.
    *
    * Restored, not recomputed: another thread's sync_report may have landed
    * while this one was in the JNI call, and it knows more than we do. Only
    * the fields THIS call claimed are put back, and only if they are still
    * the ones it wrote. */
   mutex_lock(&g_sched_lk);
   if (g_rem_seen == sz)
      g_rem_seen = was_seen;
   if (g_rem_safety == now + REMOTE_SAFETY)
      g_rem_safety = was_safety;
   /* g_rem_next is NOT restored to what it was: `go` required
    * now >= g_rem_next, so the old value is already in the past and putting
    * it back would let the very next tick fire, one second later. The floor
    * is what a refused request deserves -- retry promptly, but a Java that
    * keeps refusing must not become a spin. It is left in place. */
   mutex_unlock(&g_sched_lk);
}

/* Called from the sync worker thread (see syncjni.h). Only touches values the
 * renderer reads whole, and sets the dirty flag last. */
void sync_report(int outcome)
{
   int ok = sync_outcome_severity(outcome) == SYNC_SEV_GOOD;
   /* Two clocks, deliberately: the UI shows WHEN the server last accepted
    * something (an instant), while the retry schedule measures elapsed time
    * (an interval). */
   if (ok)
      remote_note_ok(realtime_s());
   long now = mono_s();
   /* THE WHOLE RESULT UNDER ONE LOCK, so a tick cannot observe the new
    * deadline with the old freshness stamp (see g_sched_lk). */
   mutex_lock(&g_sched_lk);
   if (ok) {
      g_rem_backoff = 0;
      /* A PAIRING that just succeeded is the answer to whatever the last
       * failure was, and this phone has sent that server nothing yet. Sync
       * now rather than at the next change: the first thing a user does
       * after pairing is look for their data on the other end. */
      if (outcome == SYNC_PAIRED) {
         g_rem_next = 0;
         g_rem_seen = -1;
      }
   } else if (!sync_outcome_retries(outcome)) {
      /* A FAILURE THAT ONLY A REPAIR CAN END: a name that does not resolve, a
       * refused certificate, a rejected key. It does not climb -- there is
       * nothing to climb away from, since the attempts are not the problem --
       * and it does not go to the slowest schedule either, because the repair
       * is often at the other end and the phone has to notice it. A steady
       * five minutes. See REMOTE_FIXED_RETRY. */
      g_rem_backoff = REMOTE_FIXED_RETRY;
      g_rem_next    = now + REMOTE_FIXED_RETRY;
      g_rem_seen    = -1;
   } else {
      /* Retry, but slower each time: a server that is down must not be asked
       * once a minute for ever. */
      g_rem_backoff = g_rem_backoff ? g_rem_backoff * 2 : REMOTE_FAIL_MIN;
      if (g_rem_backoff > REMOTE_FAIL_MAX)
         g_rem_backoff = REMOTE_FAIL_MAX;
      g_rem_next = now + g_rem_backoff;
      /* A failed sync has NOT caught up, so the next attempt must not be
       * skipped for lack of new data. */
      g_rem_seen = -1;
   }
   mutex_unlock(&g_sched_lk);
   remote_note_outcome(outcome);
   shell_ui_dirty();
}

/* THE USER CHANGED SOMETHING THE FAILURE WAS ABOUT. Clear the schedule so
 * the fix is tried at once instead of waiting out a backoff that was earned
 * by the old settings: a corrected server name that takes half an hour to be
 * tried looks exactly like a name that is still wrong. */
void remote_retry_now(void)
{
   mutex_lock(&g_sched_lk);
   g_rem_backoff = 0;
   g_rem_next    = 0;
   g_rem_seen    = -1; /* nothing has been accepted by THIS server */
   mutex_unlock(&g_sched_lk);
}

/* The configured server changed. There is no cursor to forget any more --
 * the next sync asks the NEW server what it has and finds out exactly -- but
 * the paired identity belongs to the old one and must not be offered to
 * another server. */
void remote_forget_cursor(void)
{
   struct sync_creds sc;
   sync_creds_get(&sc);
   /* BOTH OR NEITHER. The stored identity is rolled back whole when the
    * write fails, so dropping the RUNTIME key regardless is what made it
    * half gone: the phone stops signing requests while the file still names
    * the account, and the next launch loads it back. */
   if (settings_forget_identity() != SETTINGS_OK)
      return;
   sync_set_key(0, sc.key);
   remote_retry_now();
}

/* Wall-clock of the last REMOTE push the server ACKNOWLEDGED (HTTP 2xx),
 * reported back by Ble.remotePush's WORKER thread via onRemoteOk, read by
 * MAIN for the settings row. Runtime state, not a setting: it starts at 0
 * ("NEVER") on every launch.
 *
 * RELAXED, and deliberately: the value is a timestamp rendered as an age, it
 * orders nothing else, and the worst case is a settings row one frame stale.
 * Atomic rather than plain because a torn 64-bit read would not be one frame
 * stale, it would be a number from no point in time at all. */
static _Atomic long g_remote_last_ok;

/* HOW THE LAST SYNC ENDED -- a CODE, not a sentence.
 *
 * Written by the sync WORKER thread (sync_report -> model_note_remote) and
 * read by the main thread building a frame. It was a text buffer that the
 * frame borrowed a POINTER to, which makes an "immutable frame" a window onto
 * memory another thread is in the middle of rewriting: str_snapshot writes
 * the bytes and then the terminator, so a renderer reading across that lands
 * on a mixture of two replies, or on a string whose NUL has not arrived.
 *
 * An int copied under the lock cannot tear, and the words come from
 * syncstat.c, where they are string literals with the lifetime of the
 * process. The screen colours by the code's severity rather than by
 * recognising the words, which is what let two outcomes render as "nothing
 * has happened" for as long as they existed. */
static struct mutex g_rstat_lk = MUTEX_INIT;
static int g_remote_outcome; /* enum sync_outcome, written by the worker */

void remote_note_outcome(int outcome)
{
   mutex_lock(&g_rstat_lk);
   g_remote_outcome = outcome;
   mutex_unlock(&g_rstat_lk);
}

int remote_outcome(void)
{
   mutex_lock(&g_rstat_lk);
   int o = g_remote_outcome;
   mutex_unlock(&g_rstat_lk);
   return o;
}

void remote_note_ok(long when)
{
   atomic_store_explicit(&g_remote_last_ok, when, memory_order_relaxed);
}

long remote_ok_time(void)
{
   return atomic_load_explicit(&g_remote_last_ok, memory_order_relaxed);
}
