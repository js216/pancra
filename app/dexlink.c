// SPDX-License-Identifier: GPL-3.0
// dexlink.c --- which sensor a callback belongs to, and who gets told
// Copyright 2026 Jakob Kastelic

/* THE DRIVER'S REGISTRY AND ROUTING, and the lock both halves run under.
 *
 * This is the half of the Dexcom driver that knows about DEVICES rather than
 * about the protocol: the per-link contexts and the recursive lock that
 * guards them, each link's role (a meter's or a CGM's) and armed address, the
 * lock-free retry claims, the routing of every transport callback to either
 * the meter ops or the sensor state machine in dexproto.c, and the
 * calibration queue -- which runs the calibration module's own code inside
 * this lock rather than lending the lock out.
 *
 * WHAT THE SPLIT PRESERVES, because it would be a regression to lose any of
 * it (see test/app/test_driver.c, which pins all four):
 *
 *   - RECURSIVE-LOCK OWNERSHIP. driver_route_* take the lock and call into
 *     the protocol unit with it held; the protocol unit takes it again
 *     through driver_enter. That nesting is the contract, not an accident:
 *     drv_write() can complete synchronously and re-enter driver_on_written
 *     from inside a driver call.
 *   - ATOMIC PER-LINK TRANSITIONS. Deciding a link's role and acting on that
 *     decision happen under ONE hold (driver_route_connected,
 *     driver_meter_connect, driver_bind_mac). Split across two holds, a
 *     callback can land between them and be routed by a role that has since
 *     changed.
 *   - CALLBACK ORDERING. A meter's write-ack must never advance the Dexcom
 *     handshake, and a disconnect for a merely-idle registered meter must not
 *     reset the exchange another link owns.
 *   - NOTHING THAT REACHES JAVA HAPPENS INSIDE THE LOCK. What is left for the
 *     transport comes back as an enum driver_after; this lock is a spin lock
 *     the main looper also takes.
 *
 * The protocol side is dexproto.c; what they share is dexpriv.h, and the
 * public face of both is dexdriver.h.
 */
/* dexpriv.h FIRST, in a block of its own: it pulls dexdriver.h, and
 * dexport.h refuses to be read without it. A single sorted block would put
 * dexport.h ahead of it (o before p) and the build would stop there. */
#include "dexdriver.h" /* LINK_MAX, struct dex_session, the drv_* upcalls */
#include "dexpriv.h"
#include "scanlogic.h" /* struct live_stamp, live_*_due: the deadlines */
#include "sesscache.h" /* the session clock this registry restores from */

#include "clock.h"   /* mono_s: the deadline clock, never the wall one */
#include "dexport.h" /* drv_mac_save: the identity this registry persists */
#include "log.h"     /* LOGI/LOGW: the ONE declaration */
#include "otble.h" /* ot_init: the meter protocol's state is serialised here */
#include "senslogic.h" /* sens_project_clock: the session clock, host-testable */
#include "thread.h"    /* rmutex: the ONLY cross-thread primitives */
#include "util.h"      /* str_snapshot, realtime_s */

#include <stdatomic.h> /* the per-link retry claims are lock-free */
#include <stddef.h>
#include <string.h>

static struct dex_ctx g_dctx[LINK_MAX];

/* THERE IS NO CURRENT CONTEXT, and that is the point.
 *
 * Every helper is handed a validated `struct dex_ctx *`, and each context
 * knows its own link, so a helper's target is legible at its call site:
 * `send_authrequest(dc)` says which sensor it addresses. An invalid link
 * yields no context at all and the operation returns having touched nothing
 * -- it is never quietly rounded to the primary CGM, whose corruption is the
 * least recoverable of the lot.
 *
 * (It replaced a file-wide "currently selected context", which two callbacks
 * arriving for different sensors could each believe was theirs.)
 *
 * Recursive lock guarding every access to those contexts. Recursive because
 * drv_write() can complete synchronously and re-enter driver_on_written()
 * from inside a driver call.
 *
 * Every spin yields (thread.h, rule 4), and here that is not a nicety: most
 * guarded sections are short, but a few reach blocking JNI (drv_connect ->
 * connectGatt) or file I/O, and a binder thread busy-waiting through one of
 * those burns a full core and can starve the (small) binder pool it came
 * from. */
static struct rmutex driver_lk = RMUTEX_INIT;

/* THE DRIVER'S LOCK, private to this file.
 *
 * It was public, then it was in a private header six modules included, and
 * both amounted to the same thing: other files reasoning about when the
 * driver's state is safe to touch. One redundant lock() left above a call
 * that took it internally is what once held it for the life of the process,
 * with every GATT callback spinning and the phone looking perfectly
 * connected while it never produced another reading.
 *
 * Everything that needed it now names an operation instead: the routing of a
 * callback (driver_route_*), a link's role and arming (driver_link_*), the
 * whole-driver snapshot (driver_snapshot), and the calibration queue
 * (driver_cal_*), which runs the calibration module's own code inside this
 * lock rather than lending it out.
 *
 * RECURSIVE: the transport can complete a write synchronously and re-enter
 * the driver from inside a driver call, and the operations above nest freely
 * in each other. */
static void driver_lock(void)
{
   rmutex_lock(&driver_lk);
}

static void driver_unlock(void)
{
   rmutex_unlock(&driver_lk);
}

/* IS THIS A LINK THIS DRIVER HAS?
 *
 * Asked BEFORE the lock, by every public entry point, so an operation going
 * nowhere never contends for it -- and so the refusal is visibly outside the
 * critical section. test/app/lockorder.py reads these functions looking for a
 * `return` between an acquire and its release, and it is right to: three such
 * returns once held the driver lock for the life of the process.
 *
 * ASKING IS THE POINT. Clamping an out-of-range link to LINK_CGM applies a
 * malformed callback -- and the link comes from Java, indexed by whatever the
 * framework handed back -- to the PRIMARY sensor's context, the one context
 * whose corruption cannot be undone without re-pairing. See the bad-link
 * section of test/app/test_driver.c. */
int dex_link_ok(int link)
{
   return link >= 0 && link < LINK_MAX;
}

/* Take the lock and name the context. `link` MUST already have passed
 * dex_link_ok(); there is no in-range fallback, because the only safe thing to
 * do with a link that does not exist is nothing at all. */
struct dex_ctx *driver_enter(int link)
{
   driver_lock(); /* recursive: nesting is fine and does happen */
   struct dex_ctx *dc = &g_dctx[link];
   dc->link           = link; /* see the field: idempotent, and no ordering
                               * dependency on driver_init having run */
   return dc;
}

void driver_leave(void)
{
   driver_unlock();
}

void driver_each_ctx(void (*fn)(struct dex_ctx *dc))
{
   if (!fn)
      return;
   driver_lock();
   for (int l = 0; l < LINK_MAX; l++) {
      g_dctx[l].link = l; /* the slot's own index, written once per pass and
                           * with no ordering dependency on driver_init */
      fn(&g_dctx[l]);
   }
   driver_unlock();
}

int driver_held(void)
{
   return rmutex_held_by_me(&driver_lk);
}

/* ZERO FIRST, THEN ANSWER. The clear happens before the only failure, so
 * there is no path out of here that leaves the caller's struct untouched --
 * see the contract in dexdriver.h for what a half-filled struct costs. */
int driver_session_of(int link, struct dex_session *out)
{
   if (!out)
      return 0;
   *out = (struct dex_session){0};
   if (!dex_link_ok(link))
      return 0;
   struct dex_ctx *dc = driver_enter(link);
   driver_get_session(dc, out);
   driver_leave();
   return 1;
}

int driver_cal_of(int link, struct dex_cal *out)
{
   if (!out)
      return 0;
   *out = (struct dex_cal){0};
   if (!dex_link_ok(link))
      return 0;
   struct dex_ctx *dc = driver_enter(link);
   driver_get_cal(dc, out);
   driver_leave();
   return 1;
}

void driver_snapshot(struct dex_session sess[LINK_MAX], int cal_link,
                     struct dex_cal *cal)
{
   /* ONE HOLD for all of it: the point of a snapshot is that every field in
    * it describes the same instant. */
   driver_lock();
   /* THE CONTEXTS DIRECTLY, under the hold this function already took: every
    * l is in range by construction, and driver_get_session clears its own
    * output, so an entry is always written. driver_session_of would take the
    * lock again per link (harmless -- it is recursive) to answer a question
    * this loop has already answered, and driver_enter inside a driver_lock()
    * is the exact shape lockcheck refuses, because ONE misplaced pairing of
    * it held the driver's lock for the life of the process. */
   if (sess)
      for (int l = 0; l < LINK_MAX; l++) {
         g_dctx[l].link = l; /* idempotent; see the field's comment */
         driver_get_session(&g_dctx[l], &sess[l]);
      }
   if (cal) {
      *cal = (struct dex_cal){0}; /* a caller who named no link gets this */
      if (cal_link >= 0 && dex_link_ok(cal_link)) {
         g_dctx[cal_link].link = cal_link;
         driver_get_cal(&g_dctx[cal_link], cal);
      }
   }
   driver_unlock();
}

/* ---- per-link role and arming (see dexdriver.h) ---- */

static int g_link_meter[LINK_MAX];
static char g_link_armed[LINK_MAX][24];

static void driver_link_set_meter_locked(int link, int on)
{
   if (link >= 0 && link < LINK_MAX)
      g_link_meter[link] = on ? 1 : 0;
}

void driver_link_set_meter(int link, int on)
{
   driver_lock();
   driver_link_set_meter_locked(link, on);
   driver_unlock();
}

/* For the routing above, which already holds the lock. */
static int driver_link_is_meter_locked(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   return g_link_meter[link];
}

int driver_link_is_meter(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   driver_lock();
   int is = g_link_meter[link];
   driver_unlock();
   return is;
}

void driver_link_arm(int link, const char *mac)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   driver_lock();
   str_snapshot(g_link_armed[link], sizeof g_link_armed[link], mac ? mac : "");
   driver_unlock();
}

int driver_link_armed(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   driver_lock();
   int armed = g_link_armed[link][0] != 0;
   driver_unlock();
   return armed;
}

void driver_link_armed_mac(int link, char *out, int cap)
{
   if (!out || cap <= 0)
      return;
   out[0] = 0;
   if (link < 0 || link >= LINK_MAX)
      return;
   driver_lock();
   str_snapshot(out, cap, g_link_armed[link]);
   driver_unlock();
}

int driver_link_of_mac(const char *mac)
{
   int found = -1;
   if (!mac || !*mac)
      return -1;
   driver_lock();
   for (int l = 0; l < LINK_MAX && found < 0; l++)
      if (g_link_armed[l][0] && strcmp(g_link_armed[l], mac) == 0)
         found = l;
   driver_unlock();
   return found;
}

/* ---- THE PER-LINK RETRY DEADLINES (see dexdriver.h) ---------------------
 *
 * Lock-free on purpose. These are read and written by the activity's 1 Hz
 * timer, the foreground service's 20 s tick, and -- for the DIS one -- a
 * binder thread already inside driver_on_notify with the driver lock held. A
 * mutex here would be a fourth thing to order against the driver lock for no
 * gain: the whole decision is one word.
 *
 * MONOTONIC, and that is the item this exists for. Stamped from realtime_s()
 * these throttles held a NEGATIVE age for the whole duration of a backward
 * NTP correction, so `age > interval` was false for an hour and the watchdog
 * was never allowed to kick -- the app sat on a stale reading and recovered
 * only when wall time caught up. */
static _Atomic long g_kick_at[LINK_MAX]; /* MONO_NEVER = never kicked */
static _Atomic long g_dis_at[LINK_MAX];  /* MONO_NEVER = never requested */
static _Atomic long g_rssi_at[LINK_MAX]; /* MONO_NEVER = never measured */

/* ONE ATOMIC EXCHANGE DECIDES THE WINNER.
 *
 * Whoever swaps in `now` reads the PREVIOUS stamp; only the caller that finds
 * that previous stamp genuinely stale acts, and the loser reads the winner's
 * fresh stamp and stands down. Testing first and then storing lets both
 * callers pass the test.
 *
 * ACQ_REL: the winner's decision to act is published to the loser (release),
 * and the loser's read of the winner's stamp is ordered against everything
 * the winner did before it (acquire). Nothing else is carried across, but a
 * read-modify-write that decides which thread acts is exactly where relaxed
 * stops being obviously safe, and this costs nothing on a once-a-second path.
 *
 * THE CLOCK IS READ BEFORE THE EXCHANGE, so a failed read cannot restamp the
 * deadline: an unreadable clock leaves the previous stamp exactly where it
 * was and refuses, which is the rule in scanlogic.h. */
static int claim(_Atomic long *slot,
                 int (*due)(const struct live_stamp *, const struct live_now *))
{
   struct live_now nw = {.wall = 0, .mono = 0, .ok = MONO_GET_FAIL};
   nw.ok              = mono_try(&nw.mono);
   if (nw.ok != MONO_GET_OK)
      return 0;
   struct live_stamp prev = {
       .wall = 0,
       .mono = atomic_exchange_explicit(slot, nw.mono, memory_order_acq_rel)};
   return due(&prev, &nw);
}

int driver_kick_claim(int link)
{
   if (!dex_link_ok(link))
      return 0;
   return claim(&g_kick_at[link], live_kick_due);
}

int driver_dis_claim(int link)
{
   if (!dex_link_ok(link))
      return 0;
   return claim(&g_dis_at[link], live_dis_due);
}

void driver_rssi_note(int link)
{
   if (!dex_link_ok(link))
      return;
   long m = 0;
   /* A failed read leaves the previous stamp alone rather than writing a
    * fabricated 0: 0 is MONO_NEVER, and clearing a good stamp would make a
    * live connection's signal read as "not from this connection". */
   if (mono_try(&m) == MONO_GET_OK)
      atomic_store_explicit(&g_rssi_at[link], m, memory_order_release);
}

int driver_rssi_fresh(int link)
{
   if (!dex_link_ok(link))
      return 0;
   struct live_now nw     = {.wall = 0, .mono = 0, .ok = MONO_GET_FAIL};
   nw.ok                  = mono_try(&nw.mono);
   struct live_stamp meas = {
       .wall = 0,
       .mono = atomic_load_explicit(&g_rssi_at[link], memory_order_acquire)};
   return live_rssi_fresh(&meas, &nw);
}

int driver_link_claim(const char *mac, int reserve)
{
   int link = -1;
   if (!mac || !*mac)
      return -1;
   /* THE WHOLE SEARCH UNDER ONE HOLD: a link that reads free here and is
    * claimed by a binder thread before the caller uses it would be handed to
    * two devices at once. */
   driver_lock();
   /* FROM LINK_CGM + 1. Link 0 is reserved for a sensor: the key files drop
    * the suffix for link 0, and at cold start no CGM is bound yet, so the
    * first meter armed would take it and quietly change what those names
    * refer to. */
   int freen = 0;
   for (int l = LINK_CGM + 1; l < LINK_MAX; l++) {
      if (g_link_armed[l][0])
         continue;
      struct dex_session ls;
      /* the context directly, under the hold above -- see driver_snapshot */
      g_dctx[l].link = l;
      driver_get_session(&g_dctx[l], &ls);
      if (!ls.mac[0])
         freen++;
   }
   if (freen > reserve) {
      for (int l = LINK_CGM + 1; l < LINK_MAX && link < 0; l++) {
         if (g_link_armed[l][0])
            continue; /* another meter holds it */
         struct dex_session ls;
         g_dctx[l].link = l; /* in range; see the count above */
         driver_get_session(&g_dctx[l], &ls);
         if (!ls.mac[0]) /* no CGM bound here */
            link = l;
      }
      if (link >= 0)
         str_snapshot(g_link_armed[link], sizeof g_link_armed[link], mac);
   }
   driver_unlock();
   return link;
}

/* ---- routing (see dexdriver.h) ---- */

static const struct driver_meter_ops *g_meter_ops;

void driver_set_meter_ops(const struct driver_meter_ops *ops)
{
   g_meter_ops = ops;
}

/* Whether this link is a meter's AND the dispatch, decided and done under one
 * hold of the lock. What is left for the transport comes back as a
 * driver_after: each of those reaches Java, and this lock is a spin lock the
 * main looper also takes, so none of them may happen inside it. */
enum driver_after driver_route_connected(int link)
{
   enum driver_after after = DRV_AFTER_NONE;
   driver_lock();
   if (driver_link_is_meter_locked(link)) {
      /* Ask the shell first: it owns the per-meter index and the single
       * protocol state, and only it can say whether this link may have it. */
      if (g_meter_ops && g_meter_ops->connected &&
          !g_meter_ops->connected(link)) {
         after = DRV_AFTER_CLOSE;
      } else {
         if (g_meter_ops && g_meter_ops->on_connected)
            g_meter_ops->on_connected();
         /* A meter is connected only during a sync, so this brief window is
          * the one chance to read its signal. */
         after = DRV_AFTER_RSSI_METER;
      }
   } else {
      driver_on_connected(link);
      after = DRV_AFTER_RSSI;
   }
   driver_unlock();
   return after;
}

void driver_route_disconnected(int link, int status)
{
   driver_lock();
   if (driver_link_is_meter_locked(link)) {
      /* ONLY the link that owns the exchange may reset the protocol state:
       * every registered meter holds a standing connect, so a disconnect can
       * arrive for one that is merely idle. */
      if (g_meter_ops && g_meter_ops->disconnected &&
          g_meter_ops->disconnected(link) && g_meter_ops->on_disconnected)
         g_meter_ops->on_disconnected();
   } else {
      driver_on_disconnected(link, status);
   }
   driver_unlock();
}

void driver_route_notify(int link, const char *uuid, const unsigned char *d,
                         int n)
{
   driver_lock();
   if (driver_link_is_meter_locked(link)) {
      if (g_meter_ops && g_meter_ops->on_notify)
         g_meter_ops->on_notify(d, n);
   } else {
      driver_on_notify(link, uuid, d, n);
   }
   driver_unlock();
}

void driver_route_written(int link, const char *uuid, int status)
{
   driver_lock();
   /* A METER's write-ack fed into the sensor state machine advances the
    * Dexcom handshake by one step on a link that has no session -- which is
    * why this is routed rather than shared. What it must NOT do is what it
    * did before: nothing at all. A meter's protocol has requests of its own
    * and a refused write is the one thing that tells it a request never went
    * out. */
   if (driver_link_is_meter_locked(link)) {
      if (g_meter_ops && g_meter_ops->on_written)
         g_meter_ops->on_written(uuid, status);
   } else {
      driver_on_written(link, uuid, status);
   }
   driver_unlock();
}

void driver_meter_connect(int link, const char *mac,
                          void (*connect)(int link, const char *mac))
{
   driver_lock();
   driver_link_set_meter_locked(link, 1);
   if (connect)
      connect(link, mac);
   driver_unlock();
}

/* ---- the calibration queue (see dexdriver.h) ---- */

static const struct driver_cal_ops *g_cal_ops;

void driver_set_cal_ops(const struct driver_cal_ops *ops)
{
   g_cal_ops = ops;
}

void driver_cal_attempt(int link, int sensor_id)
{
   driver_lock();
   if (g_cal_ops && g_cal_ops->attempt)
      g_cal_ops->attempt(link, sensor_id);
   driver_unlock();
}

int driver_cal_queue(int sensor_id, int mg_dl)
{
   driver_lock();
   int rc = (g_cal_ops && g_cal_ops->queue) ? g_cal_ops->queue(sensor_id, mg_dl)
                                            : -1;
   driver_unlock();
   return rc;
}

int driver_cal_cancel(void)
{
   driver_lock();
   int rc = (g_cal_ops && g_cal_ops->cancel) ? g_cal_ops->cancel() : -1;
   driver_unlock();
   return rc;
}

void driver_cal_tick(void)
{
   driver_lock();
   if (g_cal_ops && g_cal_ops->tick)
      g_cal_ops->tick();
   driver_unlock();
}

int driver_cal_queued_for(int sensor_id)
{
   int q = 0;
   driver_lock();
   if (g_cal_ops && g_cal_ops->queued_for)
      q = g_cal_ops->queued_for(sensor_id);
   driver_unlock();
   return q;
}

void driver_meter_seed_index(int index)
{
   driver_lock();
   ot_init(index);
   driver_unlock();
}

/* THE RECOVERED ADDRESS THAT IS NOT ON DISK YET, per link; empty means there
 * is nothing owed. Read and written under driver_lock, beside the target it
 * is trying to become. See driver_bind_mac for why the two are not the same
 * thing and why this exists.
 *
 * `g_bind_next` throttles the retry: the sweep that drives it runs at 1 Hz
 * and a failing atomic_replace costs an open, a write and an unlink each
 * time. A full disk does not empty in a second, and hammering it once a
 * second buys nothing that once every BIND_RETRY_S does not. It is stamped
 * by the FAILURE, not by the sweep, so an empty sweep costs one comparison
 * and a sweep after a fresh failure waits the full interval. */
static char g_bind_pending[LINK_MAX][24];
static long g_bind_next;
#define BIND_RETRY_S 30

/* Copy an address into a fixed 24-byte slot, always NUL-terminated. */
static void bind_copy(char *dst, const char *mac)
{
   int i = 0;
   for (; mac[i] && i < 23; i++)
      dst[i] = mac[i];
   dst[i] = 0;
}

/* Save then publish; the caller holds the lock and has read the monotonic
 * clock. Answers whether the address is now BOTH on disk and the reconnect
 * target. A failure starts the retry deadline HERE -- at the moment the disk
 * actually refused, first attempt or hundredth -- because a throttle armed
 * anywhere else lets the very next sweep through and the sweep runs at 1 Hz. */
static enum bind_mac bind_locked(int link, const char *mac, long now)
{
   enum bind_mac r = BIND_NOT_SAVED;
   if (drv_mac_save(link, mac) == 0) {
      driver_lock_mac(link, mac);
      g_bind_pending[link][0] = 0;
      r                       = BIND_PUBLISHED;
   } else {
      bind_copy(g_bind_pending[link], mac);
      g_bind_next = now + BIND_RETRY_S;
   }
   return r;
}

enum bind_mac driver_bind_mac(int link, const char *mac)
{
   /* DURABLE FIRST, PUBLISHED SECOND -- AND NOT AT ALL IF THE WRITE FAILED.
    *
    * "The file and the target together", one lock around both with the
    * file's answer dropped on the floor, is the wrong pairing: what the
    * driver reconnects to NOW and what it reads back at the NEXT launch are
    * only one fact while both actually happened. With the save failing
    * silently the process runs happily on the recovered address and the next
    * launch has no idea it existed -- and the recovery that found it (a walk
    * of the system bond list, at startup, once) does not run again, so the
    * address is lost rather than retried.
    *
    * So publishing is conditional on the save, the failure is returned, the
    * PRIOR target is left standing, and the address is remembered
    * for driver_bind_retry. A caller that ignores the answer is a caller
    * that has silently un-recovered a sensor, which is why this is
    * PANCRA_MUST_USE.
    *
    * The lock spans both steps: a GATT callback landing between the save and
    * the publish would see the new address against the old target, which is
    * the same inconsistency by a shorter route. */
   if (!dex_link_ok(link) || !mac || !mac[0])
      return BIND_NOT_SAVED;
   long now = mono_s();
   driver_lock();
   enum bind_mac r = bind_locked(link, mac, now);
   driver_unlock();
   return r;
}

void driver_bind_retry(void)
{
   long now = mono_s();
   driver_lock();
   /* THE CLOCK IS READ OUTSIDE, THE DEADLINE COMPARED INSIDE, so two
    * threads' sweeps cannot both pass the throttle on one interval. A
    * monotonic read that fails returns 0, which is <= any deadline: the
    * retry simply waits, and the next successful read moves it. */
   if (now >= g_bind_next) {
      for (int l = 0; l < LINK_MAX; l++) {
         if (!g_bind_pending[l][0])
            continue;
         char mac[24];
         bind_copy(mac, g_bind_pending[l]);
         if (bind_locked(l, mac, now) == BIND_PUBLISHED)
            LOGI("link %d: recovered sensor address saved on retry", l);
      }
   }
   driver_unlock();
}

void driver_get_cal(const struct dex_ctx *dc, struct dex_cal *out)
{
   *out = dc->cal;
}

void driver_get_session(const struct dex_ctx *dc, struct dex_session *out)
{
   memset(out, 0, sizeof *out);
   int i = 0;
   for (; dc->g_mac[i] && i < 23; i++)
      out->mac[i] = dc->g_mac[i];
   out->mac[i]       = 0;
   out->bonded       = dc->g_bonded;
   out->paired       = dc->have_key;
   out->have_reading = (dc->str.clock != 0);
   /* LIVE session time: the sensor's clock at the last response, projected
    * forward by the ELAPSED time since. Responses arrive minutes apart, but
    * countdowns built on this (warmup, session end) must tick per second,
    * exactly as the official app's do.
    *
    * The arithmetic is in senslogic.c so a test can reach it. Inline, as an
    * add of a uint32_t-cast difference of two realtime_s() stamps, nothing
    * can -- and the wrap that produces on a wall-clock rollback is not
    * visible by reading it. */
   out->session_seconds =
       sens_project_clock(dc->str.clock, dc->str.clock_m, mono_s());
   out->last_rx   = dc->str.last_rx;
   out->state     = dc->str.state;
   out->glucose   = dc->str.glucose;
   out->trend     = dc->str.trend;
   out->age       = dc->str.age;
   out->predicted = dc->str.predicted;
   out->sequence  = dc->str.seq;
}

/* ---- calibration ---- */

/* The link bound to `identity` IN A SNAPSHOT, or -1.
 *
 * Taking a snapshot first and answering from it -- rather than holding the
 * driver's lock across a walk from out here -- is what lets this file stop
 * reaching for somebody else's mutex. Two lookups against the same snapshot
 * also cannot disagree with each other, which is what the hand-held lock was
 * really buying. */
int driver_link_of_identity_in(const struct dex_session *sess,
                               const char *identity)
{
   if (!sess || !identity || !identity[0])
      return -1;
   int found = -1;
   /* EVERY link, including a meter's. drv_connect stamps the address into the
    * link's session whichever kind of device it is, so one lookup binds both
    * -- and reserving a link for "the meter" is exactly what stopped a second
    * and third meter from ever holding a connection of their own.
    *
    * No save/restore any more: driver_session_of names the link it reads, so
    * this walk leaves the ambient selection exactly as it found it. */
   for (int l = 0; l < LINK_MAX && found < 0; l++)
      if (sess[l].mac[0] && strcmp(sess[l].mac, identity) == 0)
         found = l;
   return found;
}

/* The same question, for a caller that has no snapshot of its own. */
int driver_link_of_identity(const char *identity)
{
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   return driver_link_of_identity_in(sess, identity);
}

/* The (rank+1)'th CGM link with no session bound to it, or -1 if there are
 * fewer than that many free. Answered from a snapshot, so two questions
 * asked of the same one cannot disagree.
 *
 * The rank matters: returning simply "the lowest free link" gave every unbound
 * sensor the SAME answer, and after a restart no link has a session yet -- so
 * two registered CGMs would both be routed to LINK_CGM and fight over it, the
 * second clobbering the first. Ranking restores the distinctness the old
 * ordinal scheme had, without reintroducing its instability: a sensor that IS
 * bound never reaches here, so a forget cannot renumber a live sensor. */
int driver_free_cgm_link_in(const struct dex_session *sess, int rank)
{
   int found = -1;
   int seen  = 0;
   if (!sess)
      return -1;
   for (int l = 0; l < LINK_MAX && found < 0; l++) {
      /* SKIP A LINK A METER HOLDS. drv_connect does not stamp the driver
       * session -- only the Dexcom handshake does -- so a meter's link reads
       * as having no session and looked FREE here. A CGM allocated onto it
       * would have taken over the meter's GATT client and, with the link
       * still routed to otble, fed its sensor notifications to the OneTouch
       * parser. The armed table is the only record that the link is taken --
       * and both of these are this module's own tables, asked directly rather
       * than through the meter runtime, which would be a cycle. */
      if (driver_link_armed(l) || driver_link_is_meter(l))
         continue;
      if (!sess[l].mac[0] && seen++ == rank)
         found = l;
   }
   return found;
}
