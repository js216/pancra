// SPDX-License-Identifier: GPL-3.0
// dexdriver.c --- Dexcom dc->pairing/reconnect protocol state machine
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver -- transport-agnostic Dexcom dc->pairing/reconnect
 * state machine. See dexdriver.h. Event-driven: each transport callback
 * advances the state and issues the next operation via the drv_* hooks. Heavily
 * logged.
 *
 * Flow (validated against a real Stelo capture):
 *   connect -> subscribe auth+round -> [fresh: J-PAKE rounds] -> 02/03/04/05
 * auth
 *   -> certificate exchange (0b/0c/0d + ECDSA) to establish a streamable bond
 *   -> 06 1e -> subscribe ctrl+data -> 4e getdata -> stream EGV/backfill.
 * A bonded reconnect (auth==1) skips rounds and certs and streams directly.
 */
#include "dexdriver.h"
#include "clock.h"
#include "dexcerts.h"
#include "dexcom.h"
#include "dexdata.h"
#include "otble.h" /* ot_init: the meter protocol's state is serialised here */
/* dexlibc.h is gone from here: gettid/sched_yield were the only things this
 * file took from it, and they belong to the lock -- which now lives in
 * thread.h with every other lock in the app. */
#include "jpake.h"
#include "rand.h"
#include "senslogic.h" /* sens_project_clock: the session clock, host-testable */
#include "thread.h"    /* rmutex: the ONLY cross-thread primitives */
#include "util.h"      /* realtime_s */
#include <stdatomic.h> /* the per-link retry claims are lock-free */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)

/* Per-sensor driver state.
 *
 * Everything the pairing/auth/stream state machine touches lives here rather
 * than in file statics, so several sensors can be driven at once -- a Stelo and
 * a G7 share a GATT layout and would otherwise trample each other's phase,
 * keys and session. One context per transport link, and a caller names the
 * LINK: every public entry point validates it, takes the lock, and hands the
 * matching context to everything it calls. Nothing selects a context
 * ambiently any more -- see the note above driver_enter. */
struct dex_ctx {
   int phase;
   char g_mac[24];
   uint8_t g_code[8];
   int g_codelen;
   /* THIS CONNECTION PUT A READING IN THE HISTORY.
    *
    * Not "a frame decoded", which is what it used to mean. The two are
    * different whenever the app refuses what the wire delivered -- an
    * impossible value, a frame whose age backdates it, a link no registered
    * slot claims yet -- and this flag decides whether the connection cleared
    * the failure streak and whether the sensor's address was written down as
    * the one to come back to. See the drv_glucose call in notify_stream. */
   int streamed;
   int mac_saved; /* persisted this sensor's MAC this run (once) */
   struct jpake *pairing;
   uint8_t shared_key[16];
   int have_key;
   uint8_t token[8];
   int round_idx;
   uint8_t rxbuf[160];
   int rxlen;
   int round_done; /* this round's buffer already handled (see on_notify) */
   int tx_left, sub_idx;
   int did_rounds;
   /* A tokenHash on THIS connection was verified against our shared key.
    *
    * Rejecting a WRONG AuthChallenge is only half the check -- nothing
    * required a correct one to have arrived at all. The 0x03 and 0x05 handlers
    * are independent arms, so a peer could skip 0x03 entirely, send the three
    * bytes 05 01 01, and land in P_STREAM with its frames accepted as real
    * glucose. The app does bond (dexble.c requests Ble.createBond for the
    * pairing flow), but nothing in THIS protocol depends on it -- a sensor's
    * frames are accepted on the strength of the tokenHash and nothing else,
    * so that check is the only thing authenticating the peer. */
   int chal_ok;
   uint32_t last_clock; /* sensor session-time from the latest 4e */
   /* WHEN last_clock WAS RECEIVED, ON THE MONOTONIC CLOCK. The session time
    * is projected forward from here between responses, because the countdowns
    * built on it (warmup, session end) must tick per second while the sensor
    * answers only every ~5 minutes -- the official app's do.
    *
    * MONOTONIC. Its predecessor was a wall-clock stamp taken with
    * realtime_s(), and a wall-clock correction -- a phone coming back from
    * being off, or finding a network -- then moved the projection by however
    * far the correction went, in a subtraction whose negative result was cast
    * to uint32_t and added to an unsigned clock. See sens_project_clock in
    * senslogic.h for exactly what the two directions did to the screen.
    * `make clockcheck` names this field so it cannot quietly go back to the
    * wall clock. */
   long last_clock_m;
   /* WHEN THE LAST KEPT READING ARRIVED, ON BOTH CLOCKS. The field comment on
    * struct dex_session::last_rx in dexdriver.h is the whole story: .wall is
    * the instant that reading's row carries, .mono is what the silence
    * watchdog ages, and neither can do the other's job. `make clockcheck`
    * names last_rx.mono so it cannot quietly go back to the wall clock. */
   struct live_stamp last_rx;
   uint8_t last_state; /* raw session-state byte from the latest 4e --
                           logged and surfaced; during warmup the sensor
                           answers with no glucose but a running clock */
   uint16_t last_age;  /* age of that current reading, seconds */
   int last_glucose, last_trend, last_predicted, last_seq;
   int g_bonded; /* last AuthStatus was the fast (auth==1) path */
   int cert_idx, cert_size, cert_rx;
   int cert_sent; /* our cert chunks sent this exchange */
   /* Our signature for this key challenge has been sent.
    *
    * P_ROUNDS has round_done and P_CERT has cert_sent for exactly this;
    * P_KEYCHAL had no equivalent, so a repeated 0c frame re-signed and called
    * send_chunks again, resetting tx_left to 4 with 4 writes still in flight --
    * the acks then stop matching and 0d goes out over a partially delivered
    * signature. It also made the embedded collector key an unbounded
    * chosen-message signing oracle: one signature per connection is the
    * protocol; unlimited is not. */
   int keychal_signed;

   /* THE CALIBRATION WE ACTUALLY SENT, and which one it was.
    *
    * `pending` alone is what this used to be, and it answered only "a 0x34 we
    * sent is awaiting its reply". It is still needed for that: the reply
    * reuses the request opcode, and the handler re-reads the bounds (a 0x32
    * write) so the UI reflects the new state. Without the flag any 0x34 from
    * the peer triggered that write, and firmware echoing it -- or a post-auth
    * peer choosing to -- sustained a 0x32/0x34 ping-pong at connection-interval
    * rate, draining both batteries and flooding the log for as long as the link
    * stayed up.
    *
    * The other three fields are the identity of the write, recorded at the
    * moment it goes on the wire and handed back with the answer. A boolean
    * cannot say WHICH calibration was answered, so the shell resolved whatever
    * was queued when the reply landed -- see drv_cal_result in dexdriver.h for
    * what that did to a user who changed their mind between the write and the
    * reply. The driver does not interpret `sensor_id` or `gen`; they are the
    * caller's token, carried and returned unchanged. */
   struct {
      int pending;   /* a 0x34 we sent is awaiting its reply */
      int sensor_id; /* the caller's sensor id for that write */
      int mg_dl;     /* the value that actually went on the wire */
      unsigned gen;  /* the caller's queue generation for that write */
   } cal_tx;

   int fails;     /* consecutive connects that never streamed */
   int authfails; /* subset: failures after we reached auth/cert */
   struct dex_cal cal;
   /* WHICH LINK THIS IS, so a helper holding a context never has to be told
    * separately -- and cannot be told wrongly. It replaces the file-wide
    * g_cur_link, which was a second, mutable answer to the same question.
    *
    * WRITTEN BY driver_enter ON EVERY ENTRY, not once at startup. Once at
    * startup is enough only while driver_init is guaranteed to run before any
    * callback, and that guarantee lives in main.c -- a slot reached with the
    * field still 0 would address the RIGHT context over the WRONG wire, and
    * would read and erase link 0's key file. An idempotent store on a path
    * that has just validated the link costs nothing and needs no ordering
    * argument to be correct. */
   int link;
};

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
 * (The file-wide "currently selected context" this replaced, and the two ways
 * it went wrong, are in NOTES.md.)
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

static int link_ok(int link);
static void driver_get_session(const struct dex_ctx *dc,
                               struct dex_session *out);
static void driver_get_cal(const struct dex_ctx *dc, struct dex_cal *out);
static struct dex_ctx *driver_enter(int link);
static void driver_leave(void);

/* IS THIS A LINK THIS DRIVER HAS?
 *
 * Asked BEFORE the lock, by every public entry point, so an operation going
 * nowhere never contends for it -- and so the refusal is visibly outside the
 * critical section. app/test/lockorder.py reads these functions looking for a
 * `return` between an acquire and its release, and it is right to: three such
 * returns once held the driver lock for the life of the process.
 *
 * The predecessor asked nothing: it clamped an out-of-range link
 * to LINK_CGM, so a malformed callback -- and the link comes from Java,
 * indexed by whatever the framework handed back -- was applied to the PRIMARY
 * sensor's context, the one context whose corruption cannot be undone without
 * re-pairing. See the bad-link section of app/test/test_driver.c. */
static int link_ok(int link)
{
   return link >= 0 && link < LINK_MAX;
}

/* Take the lock and name the context. `link` MUST already have passed
 * link_ok(); there is no in-range fallback, because the only safe thing to do
 * with a link that does not exist is nothing at all. */
static struct dex_ctx *driver_enter(int link)
{
   driver_lock(); /* recursive: nesting is fine and does happen */
   struct dex_ctx *dc = &g_dctx[link];
   dc->link           = link; /* see the field: idempotent, and no ordering
                               * dependency on driver_init having run */
   return dc;
}

static void driver_leave(void)
{
   driver_unlock();
}

int driver_held(void)
{
   return rmutex_held_by_me(&driver_lk);
}

void driver_session_of(int link, struct dex_session *out)
{
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   driver_get_session(dc, out);
   driver_leave();
}

void driver_cal_of(int link, struct dex_cal *out)
{
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   driver_get_cal(dc, out);
   driver_leave();
}

void driver_snapshot(struct dex_session sess[LINK_MAX], int cal_link,
                     struct dex_cal *cal)
{
   /* ONE HOLD for all of it: the point of a snapshot is that every field in
    * it describes the same instant. */
   driver_lock();
   if (sess)
      for (int l = 0; l < LINK_MAX; l++)
         driver_session_of(l, &sess[l]);
   if (cal) {
      *cal = (struct dex_cal){0};
      if (cal_link >= 0)
         driver_cal_of(cal_link, cal);
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
   if (!link_ok(link))
      return 0;
   return claim(&g_kick_at[link], live_kick_due);
}

int driver_dis_claim(int link)
{
   if (!link_ok(link))
      return 0;
   return claim(&g_dis_at[link], live_dis_due);
}

void driver_rssi_note(int link)
{
   if (!link_ok(link))
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
   if (!link_ok(link))
      return 0;
   struct live_now nw = {.wall = 0, .mono = 0, .ok = MONO_GET_FAIL};
   nw.ok              = mono_try(&nw.mono);
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
      driver_session_of(l, &ls);
      if (!ls.mac[0])
         freen++;
   }
   if (freen > reserve) {
      for (int l = LINK_CGM + 1; l < LINK_MAX && link < 0; l++) {
         if (g_link_armed[l][0])
            continue; /* another meter holds it */
         struct dex_session ls;
         driver_session_of(l, &ls);
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
    * Dexcom handshake by one step on a link that has no session. */
   if (!driver_link_is_meter_locked(link))
      driver_on_written(link, uuid, status);
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

void driver_bind_mac(int link, const char *mac)
{
   /* THE FILE AND THE TARGET TOGETHER. drv_mac_save writes what the driver
    * reads back at startup and driver_lock_mac is what it reconnects to now;
    * a callback that landed between them saw the new address with the old
    * target still set. */
   driver_lock();
   drv_mac_save(link, mac);
   driver_lock_mac(link, mac);
   driver_unlock();
}

static void loghex(const char *tag, const uint8_t *d, int n)
{
   /* Initialised: with n == 0 the loop below never runs, and %s would then
    * print an unterminated stack buffer. A zero-length notification is
    * reachable from any peer on the link (Ble substitutes an empty array for a
    * null value) and this logs before any length check. */
   char b[(3 * 40) + 8];
   b[0]    = 0;
   int l   = 0;
   int cap = n > 40 ? 40 : n;
   for (int i = 0; i < cap; i++)
      l += snprintf(b + l, sizeof(b) - (size_t)l, "%02x", d[i]);
   LOGI("%s [%d] %s%s", tag, n, b, n > cap ? ".." : "");
}

enum {
   P_IDLE,
   P_SUB1,
   P_ROUNDS,
   P_AUTH,
   P_CERT,
   P_KEYCHAL,
   P_SUB2,
   P_STREAM,
   P_FAIL
};

static const char *phase_name(int p)
{
   static const char *n[] = {"IDLE",    "SUB1", "ROUNDS", "AUTH", "CERT",
                             "KEYCHAL", "SUB2", "STREAM", "FAIL"};
   return (p >= 0 && p <= 8) ? n[p] : "?";
}

/* Once we actually stream from the connected sensor, remember its MAC so future
 * launches reconnect only to it. Runs once per process (cheap file write). */
static void remember_sensor(struct dex_ctx *dc)
{
   if (!dc->mac_saved && dc->g_mac[0]) {
      if (drv_mac_save(dc->link, dc->g_mac) == 0)
         dc->mac_saved = 1;
   }
}

/* THIS LINK JUST DELIVERED A READING THE APP KEPT. Both clocks, one instant.
 *
 * Called beside remember_sensor and gated on the same answer -- "a record
 * entered the history" -- because the two questions are the same question:
 * a link whose every frame is refused downstream is not delivering, and both
 * adopting it permanently and calling it alive are wrong for that reason.
 *
 * A monotonic read that FAILS leaves .mono at whatever it was, which for a
 * never-stamped context is MONO_NEVER: the watchdog then measures from the
 * process's launch instead, which is the same fallback it uses before the
 * first reading ever arrives. It is never stamped with a fabricated zero --
 * see clock.h for why that distinction had to be made a value. */
static void note_rx(struct dex_ctx *dc)
{
   dc->last_rx.wall = realtime_s(); /* the INSTANT: identity, persisted */
   long m           = 0;
   if (mono_try(&m) == MONO_GET_OK)
      dc->last_rx.mono = m; /* the DEADLINE: an interval, never persisted */
}

static void driver_get_session(const struct dex_ctx *dc,
                               struct dex_session *out)
{
   memset(out, 0, sizeof *out);
   int i = 0;
   for (; dc->g_mac[i] && i < 23; i++)
      out->mac[i] = dc->g_mac[i];
   out->mac[i]       = 0;
   out->bonded       = dc->g_bonded;
   out->paired       = dc->have_key;
   out->have_reading = (dc->last_clock != 0);
   /* LIVE session time: the sensor's clock at the last response, projected
    * forward by the ELAPSED time since. Responses arrive minutes apart, but
    * countdowns built on this (warmup, session end) must tick per second,
    * exactly as the official app's do.
    *
    * The arithmetic is in senslogic.c so a test can reach it. It used to be
    * an inline add of a uint32_t-cast difference of two realtime_s() stamps,
    * where nothing could reach it, and the wrap that produces on a wall-clock
    * rollback is not visible by reading it. */
   out->session_seconds =
       sens_project_clock(dc->last_clock, dc->last_clock_m, mono_s());
   out->last_rx   = dc->last_rx;
   out->state     = dc->last_state;
   out->glucose   = dc->last_glucose;
   out->trend     = dc->last_trend;
   out->age       = dc->last_age;
   out->predicted = dc->last_predicted;
   out->sequence  = dc->last_seq;
}

/* Send our certificate once the sensor's is fully received. Called from BOTH
 * the chunk (U_ROUND) and size-announce (0x0b) paths, since the two can arrive
 * in either order; the guard makes it fire exactly once per certificate. */
static void send_chunks(struct dex_ctx *dc, const uint8_t *buf, int len);

static void cert_maybe_complete(struct dex_ctx *dc)
{
   if (dc->cert_sent || dc->cert_size <= 0 || dc->cert_rx < dc->cert_size)
      return;
   dc->cert_sent = 1;
   LOGI("   sensor cert %d received (%d); sending ours", dc->cert_idx,
        dc->cert_rx);
   send_chunks(dc, dc->cert_idx == 0 ? dex_cert0 : dex_cert1,
               dc->cert_idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN);
}

static const struct {
   const char *uuid;
   int indicate;
} sub1[] = {
    {U_AUTH,  1},
    {U_ROUND, 0}
};

static const struct {
   const char *uuid;
   int indicate;
} sub2[] = {
    {U_CTRL, 1},
    {U_DATA, 0}
};

#define NSUB1 2
#define NSUB2 2

static uint32_t le32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U |
          (uint32_t)p[3] << 24U;
}

/* issue a buffer to the round-transport char in 20-byte chunks; dc->tx_left
 * counts acks */
static void send_chunks(struct dex_ctx *dc, const uint8_t *buf, int len)
{
   dc->tx_left = (len + 19) / 20;
   LOGI("== send %d bytes in %d chunks ==", len, dc->tx_left);
   for (int o = 0; o < len; o += 20) {
      int c = len - o > 20 ? 20 : len - o;
      drv_write(dc->link, U_ROUND, buf + o, c, 1);
   }
}

/* Fill `dst` with n genuinely random bytes. Returns 0 if it could not.
 *
 * FAIL CLOSED, and share ONE implementation, because both call sites got this
 * wrong in the same way: each swallowed an unopenable /dev/urandom and a short
 * read into an empty if-body, then used the buffer regardless. Neither failure
 * is survivable where these bytes go, and neither was visible.
 *
 * Callers must treat 0 as fatal for the connection. A refused connect retries
 * on the next advert; predictable or uninitialised bytes on the air do not
 * announce themselves. */
/* (rand_bytes lives in lib/rand.c. The copy that used to be here did a SINGLE
 * read and returned got == n, so a short read -- which /dev/urandom is
 * permitted to give -- read as failure while lib's retries, and it shadowed
 * lib's exported symbol with a different signature inside the same .so.) */

/* Fresh 8-byte challenge token for this connection.
 *
 * The token is the ONLY thing that makes the sensor's AuthChallenge tokenHash
 * fresh: with a constant token the expected hash is a constant per key, so a
 * tokenHash once observed on air replays forever and the verification in
 * driver_on_notify proves nothing. Before rand_bytes this left dc->token at
 * its previous value on failure -- on the first connection of a process, eight
 * zero bytes, because ctx is zero-initialised. */
static int gen_token(struct dex_ctx *dc)
{
   return rand_bytes(dc->token, 8);
}

static void send_authrequest(struct dex_ctx *dc)
{
   if (!gen_token(dc)) {
      LOGI("!! no entropy for the auth token -- refusing to authenticate");
      drv_status("NO ENTROPY");
      dc->phase = P_FAIL;
      return;
   }
   /* Fresh token => any earlier verification is void. Clear it HERE, with the
    * token it was computed against, so the flag can never outlive its input. */
   dc->chal_ok = 0;
   uint8_t m[10];
   m[0] = 0x02;
   memcpy(m + 1, dc->token, 8);
   m[9] = 0x02;
   LOGI("== AuthRequest (02 dc->token 02) ==");
   drv_write(dc->link, U_AUTH, m, 10, 0);
   dc->phase = P_AUTH;
}

static void request_round(struct dex_ctx *dc)
{
   LOGI("== request J-PAKE round %d (0a %02x) ==", dc->round_idx + 1,
        dc->round_idx);
   uint8_t m[2]   = {0x0a, (uint8_t)dc->round_idx};
   dc->rxlen      = 0;
   dc->round_done = 0;
   drv_write(dc->link, U_AUTH, m, 2, 0);
}

static void send_our_round(struct dex_ctx *dc)
{
   uint8_t pkt[160];
   int ok = 0;
   if (dc->round_idx == 0)
      ok = jpake_round1(dc->pairing, pkt);
   else if (dc->round_idx == 1)
      ok = jpake_round2(dc->pairing, pkt);
   else
      ok = jpake_round3(dc->pairing, pkt);
   if (!ok) {
      LOGI("!! round%d build failed", dc->round_idx + 1);
      dc->phase = P_FAIL;
      return;
   }
   LOGI("== send our round %d ==", dc->round_idx + 1);
   send_chunks(dc, pkt, 160);
}

/* announce a certificate (0b idx len) and expect the sensor's cert on the round
 * char */
static void start_cert(struct dex_ctx *dc, int idx)
{
   dc->cert_idx  = idx;
   dc->cert_rx   = 0;
   dc->cert_size = 0;
   dc->cert_sent = 0;
   int len       = idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN;
   uint8_t m[6]  = {0x0b,
                    (uint8_t)idx,
                    (uint8_t)len,
                    (uint8_t)((unsigned)len >> 8U),
                    (uint8_t)((unsigned)len >> 16U),
                    (uint8_t)((unsigned)len >> 24U)};
   LOGI("== certificate %d: announce (0b %02x len=%d) ==", idx, idx, len);
   drv_write(dc->link, U_AUTH, m, 6, 0);
   dc->phase = P_CERT;
}

static void start_keychallenge(struct dex_ctx *dc)
{
   LOGI("== key challenge (0c + random16) ==");
   uint8_t m[17];
   m[0] = 0x0c;
   /* m is an UNINITIALISED stack buffer, so failing to fill m[1..16] used to
    * transmit 16 bytes of whatever the stack last held -- leaking process
    * memory over the air and, worse, sending a key challenge that is not
    * random at all. The old code swallowed both /dev/urandom failures into an
    * empty if-body and wrote m regardless. */
   if (!rand_bytes(m + 1, 16)) {
      LOGI("!! no entropy for the key challenge -- refusing to bond");
      drv_status("NO ENTROPY");
      dc->phase = P_FAIL;
      return;
   }
   dc->cert_rx        = 0; /* accumulate the sensor's 64-byte blob */
   dc->keychal_signed = 0;
   drv_write(dc->link, U_AUTH, m, 17, 0);
   dc->phase = P_KEYCHAL;
}

static void goto_stream(struct dex_ctx *dc)
{
   LOGI("dc->phase -> SUB2 (enable ctrl+data CCCDs, then getdata)");
   dc->phase   = P_SUB2;
   dc->sub_idx = 0;
   drv_subscribe(dc->link, sub2[0].uuid, sub2[0].indicate);
}

void driver_init(void)
{
   jpake_init();
   /* Establish the documented "-1 = none yet" sentinel for every link.
    *
    * g_dctx is a zero-initialised static, and only driver_forget ever set
    * this -- so on a fresh process with a saved key, tapping READ BOUNDS set
    * cal.have without touching result, and the UI rendered "LAST RESULT 0x00"
    * in green: a calibration submitted and accepted, when none ever was. */
   for (int l = 0; l < LINK_MAX; l++)
      g_dctx[l].cal.result = -1;
   /* EVERY CGM link, not just the ambient one.
    *
    * The key and MAC files are per-link (stelo.key, stelo.key.2, ...), but this
    * used to load only whichever context happened to be selected -- always
    * LINK_CGM at startup. So links 2..N began every process with have_key = 0
    * and an empty MAC: a second CGM's advert hit the "no saved MAC" guard and
    * was never reconnected, and had it connected it would have demanded a fresh
    * J-PAKE pairing with an applicator code the user no longer has. A second
    * sensor therefore went silent at the first app restart, permanently. */
   driver_lock();
   /* EVERY link, including link 1.
    *
    * This used to skip LINK_METER, which is the last place in the tree still
    * using that constant as though it named a link a meter permanently owns
    * -- an idiom main.c:508 and dexble.c:548 both record replacing with a
    * per-link fact, because the link a meter occupies is now whatever the
    * allocator gave it. free_cgm_link (main.c:986) allocates by that dynamic
    * table, and at cold start nothing is armed and no link is flagged as a
    * meter, so link 1 is an ordinary free link and a CGM can be given it.
    * When that happened, this loop skipped it: the sensor began every process
    * with have_key = 0 and an empty MAC, its advert hit the "no saved MAC"
    * guard, and it was never reconnected -- verbatim the failure the comment
    * above says this loop was written to fix, still live for one link.
    *
    * There is nothing to protect here anyway. The key and MAC files are per
    * link, so a link that never held a CGM simply has no files and loads
    * nothing. The files are the fact; the constant was a guess. */
   for (int l = 0; l < LINK_MAX; l++) {
      struct dex_ctx *dc = &g_dctx[l];
      dc->link           = l; /* the slot's own index, written once */
      uint8_t k[16];
      if (drv_key_load(l, k)) {
         memcpy(dc->shared_key, k, 16);
         dc->have_key = 1;
         LOGI("link %d: loaded saved key", l);
      }
      /* Reconnect only to sensors we have bonded to: pre-load each link's MAC
       * so the app locks onto those exact devices and never touches another. */
      if (drv_mac_load(l, dc->g_mac, (int)sizeof dc->g_mac))
         LOGI("link %d: locked to the saved sensor", l);
   }
   driver_unlock();
}

/* Set the reconnect target MAC without initiating a connection. Used at startup
 * to re-lock onto the bonded sensor (resolved from the system bond list) when
 * files/stelo.mac is missing; the advert path then matches only this address.
 */
void driver_lock_mac(int link, const char *mac)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   int i              = 0;
   for (; mac[i] && i < 23; i++)
      dc->g_mac[i] = mac[i];
   dc->g_mac[i] = 0;
   driver_leave();
}

void driver_start(int link, const char *mac, const char *code)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   int i              = 0;
   for (; mac[i] && i < 23; i++)
      dc->g_mac[i] = mac[i];
   dc->g_mac[i]  = 0;
   dc->g_codelen = 0;
   for (int j = 0; code[j] && dc->g_codelen < 8; j++)
      dc->g_code[dc->g_codelen++] = (uint8_t)code[j];
   dc->phase = P_IDLE;
   drv_status(dc->have_key ? "WAITING" : "PAIRING");
   /* THE LINK, NOT THE ADDRESS. A sensor's BLE address is a hardware
    * identifier that follows the wearer for the life of the sensor, and it
    * was printed here beside the pairing code's length on a line that is
    * emitted every reconnect -- so a single logcat capture tied the person
    * holding the phone to the device on their arm. The link index is what
    * every other line in this file already keys on and is the only thing a
    * reader of the log actually needs to follow a session. */
   LOGI("driver_start link=%d code(len=%d) dc->have_key=%d", dc->link,
        dc->g_codelen, dc->have_key);
   drv_connect(dc->link, dc->g_mac);
   driver_leave();
}

void driver_on_connected(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   /* Free any pairing context from a previous attempt BEFORE re-walking the
    * subscribe sequence.
    *
    * This resets phase unconditionally, and a second on_connected without an
    * intervening disconnect overwrites dc->pairing further down -- orphaning
    * a block that jpake_free exists to memset. That leaks the J-PAKE
    * secrets (xA, xB, vA, vB, v3 and the passphrase-derived scalar) unwiped in
    * a process that runs for days. It is peer-reachable: Ble calls
    * discoverServices() on any onMtuChanged, including a peer-initiated MTU
    * exchange, and discovery completion calls back in here. */
   if (dc->pairing) {
      jpake_free(dc->pairing);
      dc->pairing = NULL;
   }
   LOGI("<< connected: services ready");
   drv_status("CONNECTED");
   dc->phase   = P_SUB1;
   dc->sub_idx = 0;
   LOGI("dc->phase -> SUB1 (enable auth+round CCCDs)");
   drv_subscribe(dc->link, sub1[0].uuid, sub1[0].indicate);
   driver_leave();
}

/* Continuous reconnect: once paired, reconnect for every sensor cycle (~5 min),
 * indefinitely and without ever giving up -- this app is meant to run 24/7 like
 * the official one. drv_connect uses autoConnect=true, a passive wait for the
 * sensor's next advertisement, so even a long run of failures is gentle: there
 * is no active scanning and no connect storm, just one armed reconnect at a
 * time. dc->fails is kept only for logging the current streak. */
#define MAX_FAILS 15 /* connect-but-no-stream cap; pause to stop hammering */

void driver_on_disconnected(int link, int status)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< disconnected status=%d in dc->phase=%s (dc->streamed=%d)", status,
        phase_name(dc->phase), dc->streamed);
   int did_stream = dc->streamed;
   dc->streamed   = 0;
   if (dc->pairing) {
      jpake_free(dc->pairing);
      dc->pairing = NULL;
   }
   int was   = dc->phase;
   dc->phase = P_IDLE;
   if (did_stream) {
      dc->fails     = 0;
      dc->authfails = 0;
   } else {
      dc->fails++;
      /* Authenticated (reached auth/cert/keychal) but never dc->streamed. If
       * this repeats with a stored key, the key is stale -- another app (e.g.
       * the official Stelo app) re-paired the sensor. Drop the key so the next
       * connect re-pairs from scratch via the J-PAKE rounds (dc->have_key=0
       * path).
       */
      /* NOT when the peer already PROVED it holds the key this connection.
       * dc->chal_ok is set once the AuthChallenge tokenHash verified, which
       * is cryptographic proof the key is good -- a drop after that is RF,
       * not a stale key. Counting it meant three range-edge drops during the
       * cert exchange deleted a working bond, and the pairing code is not
       * persisted, so recovery needs the applicator: discard it and the
       * sensor is unusable for the rest of its wear. A genuinely stale key
       * fails the tokenHash instead, which is handled at once where it is
       * proven. */
      if (dc->have_key && !dc->chal_ok &&
          (was == P_AUTH || was == P_CERT || was == P_KEYCHAL) &&
          ++dc->authfails >= 3) {
         LOGI("!! %d post-auth failures with a key -> discard key, re-pair",
              dc->authfails);
         dc->have_key  = 0;
         dc->authfails = 0;
         drv_key_clear(dc->link);
      }
   }
   /* Note: out-of-range does NOT count here -- autoConnect just waits for the
    * next advert without a failed connection. dc->fails climbs only when we
    * connect yet can't stream, so the cap catches a genuine loop, not a quiet
    * sensor.
    */
   if (dc->fails >= MAX_FAILS) {
      LOGI("!! %d straight failures -- pausing to avoid hammering (relaunch to "
           "retry)",
           dc->fails);
      drv_status("CONNECTION ERROR");
   } else if (dc->have_key || dc->g_codelen > 0) {
      drv_status(dc->have_key ? "WAITING" : "RE-PAIRING");
      LOGI("reconnect on link %d (fail streak %d, was=%s)", dc->link, dc->fails,
           phase_name(was));
      drv_connect(dc->link, dc->g_mac);
   } else {
      LOGI("no key/code -- not reconnecting (was=%s)", phase_name(was));
      drv_status("CONNECTION ERROR");
   }
   driver_leave();
}

/* Forget the paired sensor: drop the shared key/bond and reset all dc->pairing
 * state so the next connection pairs from scratch (J-PAKE) with a fresh code.
 * Used by "PAIR NEW SENSOR" -- the caller then re-arms scanning for the new
 * one.
 */
void driver_forget(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->pairing) {
      jpake_free(dc->pairing);
      dc->pairing = NULL;
   }
   /* Wipe the key, do not merely stop trusting it: this context is a
    * long-lived global, so "forget" left the 16 bytes readable in memory. */
   memset(dc->shared_key, 0, sizeof dc->shared_key);
   dc->have_key   = 0;
   dc->g_bonded   = 0;
   dc->g_codelen  = 0;
   dc->did_rounds = 0;
   dc->fails      = 0;
   dc->authfails  = 0;
   dc->phase      = P_IDLE;
   dc->mac_saved  = 0;
   drv_key_clear(dc->link);
   drv_mac_clear(dc->link);
   dc->g_mac[0] = 0; /* unlock: allow dc->pairing a different sensor */
   /* Clear the SESSION as well. driver_get_session() otherwise still reported
    * the previous sensor's clock, and sensor_reconcile derives activation from
    * it -- so a sensor paired right after a forget was stamped with the old
    * one's start time, permanently, in a file that is never rewritten. */
   dc->last_clock     = 0;
   dc->last_age       = 0;
   dc->last_glucose   = 0;
   dc->last_trend     = 0;
   dc->last_predicted = 0;
   dc->last_seq       = 0;
   dc->streamed       = 0;
   dc->cal            = (struct dex_cal){.result = -1};
   LOGI("driver_forget: key/bond dropped, ready to pair a new sensor");
   driver_leave();
}

/* Watchdog reconnect: re-issue a connect from a stalled state. The transport's
 * connect() closes any lingering GATT client first, so this recovers a stranded
 * link (e.g. an orphaned client after a crash/force-stop) without a BT toggle.
 *
 * NO P_STREAM GUARD. There used to be an early return here on the grounds
 * that "actively streaming" means healthy and must not be disturbed -- but
 * the ONE path into this function is pancra_link_watchdog -> dexble_reconnect,
 * which fires only after a link has produced no reading for 420 s. By the
 * time we are called the caller has already PROVEN the link is not healthy,
 * so the guard could only ever suppress the recovery it was written to
 * protect. A link whose disconnect callback was lost sits at P_STREAM
 * forever: the watchdog no-opped every cycle and, with the screen off, the
 * advert path needs a scan that is down -- monitoring stopped silently until
 * the app was restarted. A healthy streaming link cannot reach here, because
 * its own samples are newer than the watchdog's threshold. */
void driver_kick(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->have_key || dc->g_codelen > 0) {
      dc->phase = P_IDLE;
      /* Clear the give-up counters so the watchdog can always revive a link
       * that hit MAX_FAILS (e.g. a sensor that connected but never
       * dc->streamed during warmup); otherwise dc->fails stays >= MAX_FAILS
       * and every cycle re-shows CONNECTION ERROR with no reconnect. */
      dc->fails     = 0;
      dc->authfails = 0;
      LOGI("driver_kick: forcing a fresh reconnect on link %d", dc->link);
      drv_connect(dc->link, dc->g_mac);
   }
   driver_leave();
}

/* Recover a gap: request records from (now - span) up to just before the
 * current reading. Endpoints are sensor session-time, mapped from the latest
 * 4e (dc->last_clock/dc->last_age). span is clamped to the sensor's ~24h
 * buffer.
 */
void driver_request_backfill(int link, long span)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->phase != P_STREAM || dc->last_clock == 0) {
      driver_leave();
      return;
   }
   long end = (long)dc->last_clock -
              (long)dc->last_age; /* current reading, session-time */
   if (end <= 1) {
      driver_leave();
      return;
   }
   if (span > 86400)
      span = 86400;
   long start = end - span;
   if (start < 0)
      start = 0;
   end -= 1; /* exclude the current reading */
   if (start >= end) {
      driver_leave();
      return;
   }
   uint32_t s   = (uint32_t)start;
   uint32_t e   = (uint32_t)end;
   uint8_t m[9] = {0x59,
                   (uint8_t)s,
                   (uint8_t)(s >> 8U),
                   (uint8_t)(s >> 16U),
                   (uint8_t)(s >> 24U),
                   (uint8_t)e,
                   (uint8_t)(e >> 8U),
                   (uint8_t)(e >> 16U),
                   (uint8_t)(e >> 24U)};
   LOGI("== backfill request 59 [%u..%u] (span %lds) ==", s, e, span);
   drv_write(dc->link, U_CTRL, m, 9, 0);
   driver_leave();
}

/* ---- calibration ---- */

static void driver_get_cal(const struct dex_ctx *dc, struct dex_cal *out)
{
   *out = dc->cal;
}

void driver_cal_bounds(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->phase != P_STREAM) {
      LOGI("== dc->cal bounds: not streaming, ignored ==");
      {
         driver_leave();
         return;
      }
   }
   uint8_t m[1]  = {0x32};
   dc->cal.asked = realtime_s();
   LOGI("== dc->cal bounds request 32 ==");
   drv_write(dc->link, U_CTRL, m, 1, 0);
   driver_leave();
}

/* Returns 1 if the calibration write was actually issued, 0 if refused. The
 * shell uses this to keep a confirmed calibration QUEUED and retry it later
 * rather than dropping it -- a refusal here is transient (not yet streaming, or
 * the 0x32 permission not yet parsed), so it must never lose the value. */
int driver_calibrate(int link, int mg_dl, int sensor_id, unsigned gen)
{
   /* THE LINK NAMES THE CONTEXT, and an unknown link names none: nothing was
    * written, which is exactly what 0 means to the caller -- the calibration
    * stays queued and is retried, rather than being reported as sent. */
   if (!link_ok(link))
      return 0; /* nothing was written, so nothing is reported as sent */
   struct dex_ctx *dc = driver_enter(link);
   if (dc->phase != P_STREAM || dc->last_clock == 0) {
      LOGI("== calibrate: not streaming, deferred ==");
      {
         driver_leave();
         return 0;
      }
   }
   /* Refuse a value the sensor would reject anyway, and refuse before we know
    * the firmware permits it -- so a stale UI can never push a blind write. */
   if (mg_dl < 40 || mg_dl > 400) {
      LOGI("== calibrate: %d out of range, refused ==", mg_dl);
      {
         driver_leave();
         return 0;
      }
   }
   /* Require a POSITIVE answer from 0x32, not merely the absence of a negative
    * one. `have && !permitted` let a calibration through whenever the bounds
    * probe had not answered yet -- which is exactly the blind write the
    * comment above says is refused, and the Stelo may well never answer 0x32
    * at all. The UI gates on cal_permitted for the same reason; this is the
    * driver-side backstop so no other caller can bypass it. */
   if (!dc->cal.have || !dc->cal.permitted) {
      LOGI("== calibrate: not permitted (have=%d permitted=%d) ==",
           dc->cal.have, dc->cal.permitted);
      {
         driver_leave();
         return 0;
      }
   }
   uint32_t when = dc->last_clock; /* sensor clock, seconds since activation */
   uint16_t g    = (uint16_t)mg_dl;
   /* G7 framing: opcode, glucose u16 LE, time u32 LE. No CRC. */
   uint8_t m[7] = {0x34,
                   (uint8_t)g,
                   (uint8_t)(g >> 8U),
                   (uint8_t)when,
                   (uint8_t)(when >> 8U),
                   (uint8_t)(when >> 16U),
                   (uint8_t)(when >> 24U)};
   loghex("== CALIBRATE 34", m, 7);
   dc->cal.result = -1; /* fresh: -1 awaiting, 0 accepted, >0 rejected */
   /* RECORDED BEFORE THE WRITE, and recorded WHOLE. The reply can arrive on
    * another thread the moment drv_write returns, and a record filled in
    * afterwards would be a window in which a genuine answer is matched against
    * the PREVIOUS write's token -- the same misresolution this record exists
    * to make impossible, moved into a race. (Both are under the driver lock
    * today, which is why this is an ordering that costs nothing rather than a
    * problem being solved twice.) */
   dc->cal_tx.pending   = 1;
   dc->cal_tx.sensor_id = sensor_id;
   dc->cal_tx.mg_dl     = mg_dl;
   dc->cal_tx.gen       = gen;
   drv_write(dc->link, U_CTRL, m, 7, 0);
   driver_leave();
   return 1;
}

void driver_on_written(int link, const char *uuid, int status)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   /* EVERY EXIT GOES THROUGH driver_leave -- see the `out` label below.
    *
    * Three paths here used to `return` outright, and the driver's lock is
    * RECURSIVE and has no timeout: the owner stayed set to a binder thread
    * that had already gone home, its depth never fell to zero, and every
    * other thread -- the main looper first -- span in driver_lock for ever.
    * The phone froze mid-reconnect and Android killed the app as not
    * responding. The commonest of the three was a stray write-ack during
    * subscription, which is ordinary BLE traffic. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< onWritten %.8s status=%d dc->phase=%s", uuid, status,
        phase_name(dc->phase));
   if (dc->phase == P_SUB1) {
      if (strcmp(uuid, sub1[dc->sub_idx].uuid) != 0) {
         LOGI("   (ignore stray ack)");
         goto out;
      }
      if (++dc->sub_idx < NSUB1) {
         drv_subscribe(dc->link, sub1[dc->sub_idx].uuid,
                       sub1[dc->sub_idx].indicate);
      } else if (dc->have_key) {
         dc->did_rounds = 0;
         send_authrequest(dc);
      } else {
         dc->did_rounds = 1;
         dc->pairing    = jpake_new(dc->g_code, dc->g_codelen, 1);
         if (!dc->pairing) { /* allocation failed; the round handlers would
                                 dereference it unguarded */
            LOGI("!! pairing alloc failed");
            dc->phase = P_FAIL;
            goto out;
         }
         dc->round_idx = 0;
         dc->phase     = P_ROUNDS;
         request_round(dc);
      }
   } else if (dc->phase == P_ROUNDS) {
      if (!strcmp(uuid, U_ROUND) && dc->tx_left > 0) {
         dc->tx_left--;
         if (dc->tx_left == 0) {
            if (++dc->round_idx < 3) {
               request_round(dc);
            } else if (!jpake_shared_key(dc->pairing, dc->shared_key)) {
               LOGI("!! sharedkey fail");
               dc->phase = P_FAIL;
            } else {
               /* THE OUTCOME, NEVER THE KEY. This used to be
                * loghex("SHAREDKEY(derived)", dc->shared_key, 16) -- the
                * sixteen bytes the whole J-PAKE exists to agree on, printed
                * as hex into logcat. logcat is readable by `adb logcat` from
                * any machine the phone is plugged into, and a bug report
                * carries it off the device; anyone holding those bytes can
                * impersonate this phone to that sensor for the rest of its
                * wear without ever knowing the pairing code. The line was
                * there because the derivation was being brought up against a
                * live Stelo and the two sides' keys had to be compared by
                * eye. That comparison is done, and the test suite compares
                * them now (drivertest asserts drv_key_save gets the sensor's
                * key), so nothing is lost by saying only that it happened. */
               LOGI("   shared key derived (16 bytes, not logged)");
               if (drv_key_save(dc->link, dc->shared_key) != 0) {
                  LOGI("!! shared key persistence failed");
                  dc->phase = P_FAIL;
               } else {
                  dc->have_key = 1;
                  send_authrequest(dc);
               }
            }
         }
      }
   } else if (dc->phase == P_CERT) {
      /* our cert chunks being acked; when all sent, next cert or key challenge
       */
      if (!strcmp(uuid, U_ROUND) && dc->tx_left > 0) {
         dc->tx_left--;
         if (dc->tx_left == 0) {
            if (dc->cert_idx == 0)
               start_cert(dc, 1);
            else
               start_keychallenge(dc);
         }
      }
   } else if (dc->phase == P_KEYCHAL) {
      /* our signature chunks acked; when all sent, write 0d 00 02 */
      if (!strcmp(uuid, U_ROUND) && dc->tx_left > 0) {
         dc->tx_left--;
         if (dc->tx_left == 0) {
            uint8_t m[3] = {0x0d, 0x00, 0x02};
            LOGI("   -> challenge out (0d 00 02)");
            drv_write(dc->link, U_AUTH, m, 3, 0);
         }
      }
   } else if (dc->phase == P_SUB2) {
      if (strcmp(uuid, sub2[dc->sub_idx].uuid) != 0) {
         LOGI("   (ignore stray ack)");
         goto out;
      }
      if (++dc->sub_idx < NSUB2) {
         drv_subscribe(dc->link, sub2[dc->sub_idx].uuid,
                       sub2[dc->sub_idx].indicate);
      } else {
         LOGI("== get data (write 4e) ==");
         uint8_t c = 0x4e;
         drv_write(dc->link, U_CTRL, &c, 1, 0);
         dc->phase = P_STREAM;
      }
   }
out:
   driver_leave();
}

/* PAIRING, ROUND BY ROUND. The peer's J-PAKE round arrives here, possibly in
 * MTU-sized chunks that straddle the 160-byte boundary. */
static void notify_rounds(struct dex_ctx *dc, const uint8_t *buf, int n)
{
   /* Copy what fits rather than dropping an oversized notify whole. The MTU
    * is 185, so a 160-byte round can arrive in chunks that straddle the
    * boundary; dropping one left rxlen stuck below 160, the completion test
    * never fired, and pairing hung with no timeout -- the stall watchdog is
    * gated on `paired`, which is 0 during first-time pairing. */
   int room = 160 - dc->rxlen;
   if (room > 0) {
      int take = n < room ? n : room;
      memcpy(dc->rxbuf + dc->rxlen, buf, (size_t)take);
      dc->rxlen += take;
   }
   /* Handle each completed round EXACTLY once. rxlen is only reset by
    * request_round(dc), which does not run until our last chunk is acked, so
    * it stays pinned at 160 in between. Any further U_ROUND notify in that
    * window (a peer retransmit, or a stray frame from a hostile peer on the
    * link) re-ran the ZKP on the stale buffer and called send_chunks again,
    * resetting tx_left to 8 while 8 writes were still in flight. The acks
    * then no longer matched the outstanding writes: tx_left hit 0 early,
    * the remaining acks were dropped, round_idx never advanced, and pairing
    * stalled forever -- with no timeout, because the stall watchdog is gated
    * on `paired`, which is 0 during first-time pairing. P_CERT already has
    * this guard as cert_sent; this is the missing equivalent. */
   if (dc->rxlen >= 160 && !dc->round_done) {
      dc->round_done = 1;
      int ok         = 0;
      if (dc->round_idx == 0)
         ok = jpake_peer_round1(dc->pairing, dc->rxbuf);
      else if (dc->round_idx == 1)
         ok = jpake_peer_round2(dc->pairing, dc->rxbuf);
      else
         ok = jpake_peer_round3(dc->pairing, dc->rxbuf);
      /* ENFORCE the peer's Schnorr ZKP (RFC-8235: g^r + X^H == V). A genuine
       * sensor's proofs always verify -- they attest knowledge of the round
       * ephemerals, independent of the pairing code -- so a failure means a
       * malformed or hostile peer whose round data must NOT be folded into
       * the shared key. This used to log "INVALID(continuing)" and derive
       * the key anyway; the later dex8 tokenHash gate would reject the wrong
       * key, but the spec check belongs here and refusing early avoids
       * emitting our own round to a peer that already failed. */
      if (!ok) {
         LOGI("!! peer round%d ZKP INVALID -- refusing to pair",
              dc->round_idx + 1);
         drv_status("PAIR FAILED");
         dc->phase = P_FAIL;
         return;
      }
      LOGI("   peer round%d ZKP VALID", dc->round_idx + 1);
      send_our_round(dc);
   }
}

/* THE AUTHENTICATION EXCHANGE: the sensor's challenge, our answer, and the
 * status byte that says whether the saved key was accepted. */
static void notify_auth(struct dex_ctx *dc, const uint8_t *buf, int n)
{
   if (n >= 17 && buf[0] == 0x03) {
      /* VERIFY THE SENSOR FIRST. The frame is
       *   03 <tokenHash8> <challenge8>
       * where tokenHash is the peer's proof that it holds the shared key,
       * computed over the token WE generated for this connection. Answering
       * without checking it made authentication one-way: any peer
       * presenting the locked MAC could reply 03 + 16 arbitrary bytes, then
       * 05 01 01, and reach P_STREAM -- after which its 0x4e frames became
       * real glucose, feeding the big number, the ALARM, and the permanent
       * log. A spoofed sensor could mask a hypo.
       *
       * Replying unconditionally was also a chosen-plaintext oracle: the
       * peer picks buf[9..16] and gets AES-ECB(key, c||c)[:8] back.
       *
       * This check existed in test/dexsession.c, a file no build target
       * ever compiled -- so it protected nothing. That file is now deleted
       * and the check lives here, exercised by test_driver.c's spoofed-peer
       * and skipped-AuthChallenge cases. */
      uint8_t expect[8];
      dexcom_dex8(dc->shared_key, dc->token, expect);
      if (memcmp(expect, buf + 1, 8) != 0) {
         LOGI("!! AuthChallenge tokenHash MISMATCH -- peer does not hold the "
              "shared key; refusing");
         drv_status("AUTH FAILED");
         /* Discard the saved key IMMEDIATELY, not after three failures.
          *
          * A tokenHash mismatch is CRYPTOGRAPHIC PROOF the peer does not
          * hold our key -- a retry with the same key cannot ever succeed,
          * so there is nothing to gain by keeping it for two more
          * connect->refuse->drop cycles. It means exactly one thing:
          * something else re-paired this sensor (e.g. the user paired it
          * to the official Dexcom app), so our stored key is now stale.
          * Drop it here and the next connection re-pairs via J-PAKE with
          * the pairing code (have_key == 0 path). The old authfails >= 3
          * gate was for AMBIGUOUS failures (a drop mid-cert); a mismatch
          * is not ambiguous.
          *
          * Run it HERE, because setting P_FAIL below takes us out of
          * P_AUTH and driver_on_disconnected's `was == P_AUTH` test would
          * then never fire. */
         if (dc->have_key) {
            LOGI("!! stale key proven by tokenHash mismatch -- discarding "
                 "it now so the sensor re-pairs on the next connect");
            dc->have_key  = 0;
            dc->authfails = 0;
            drv_key_clear(dc->link);
         }
         dc->phase = P_FAIL;
         return;
      }
      LOGI("   AuthChallenge (03) verified -> ChallengeReply (04)");
      dc->chal_ok = 1;
      uint8_t reply[9];
      reply[0] = 0x04;
      dexcom_dex8(dc->shared_key, buf + 9, reply + 1);
      drv_write(dc->link, U_AUTH, reply, 9, 0);
   } else if (n >= 3 && buf[0] == 0x05) {
      int auth     = buf[1];
      int bond     = buf[2];
      dc->g_bonded = (auth == 1);
      LOGI("   AuthStatus (05) auth=%02x bond=%02x", auth, bond);
      if (auth == 0) {
         LOGI("!! auth failed");
         drv_status("AUTH FAILED");
         dc->phase = P_FAIL;
      } else if (!dc->chal_ok) {
         /* THE PEER NEVER PROVED IT HOLDS THE KEY. A genuine sensor always
          * sends AuthChallenge (03) before AuthStatus (05) -- that is the
          * captured order in both the pairing and the bonded-reconnect
          * flow. Accepting 05 on its own let a peer on the locked MAC skip
          * the proof entirely and reach P_STREAM, after which its 4e frames
          * became the headline number, the alarm input, and permanent rows
          * in readings.csv. */
         LOGI("!! AuthStatus without a verified AuthChallenge -- refusing");
         drv_status("AUTH FAILED");
         dc->phase = P_FAIL;
      } else if (dc->did_rounds || auth != 1) {
         /* establish/refresh the bond via the certificate exchange */
         drv_status(dc->did_rounds ? "PAIRED" : "BONDING");
         start_cert(dc, 0);
      } else {
         LOGI("   bonded reconnect -> stream");
         drv_status("AUTHENTICATED");
         goto_stream(dc);
      }
   }
}

/* THE CERTIFICATE, which arrives in fragments and is only complete when its
 * declared length has been reached (see cert_maybe_complete). */
static void notify_cert(struct dex_ctx *dc, const char *uuid,
                        const uint8_t *buf, int n)
{
   if (!strcmp(uuid, U_AUTH) && n >= 7 && buf[0] == 0x0b) {
      /* size announce; don't reset dc->cert_rx -- the sensor streams some
       * cert chunks before this arrives (start_cert already zeroed it). le32
       * is unsigned; clamp so a garbage/huge size can't become a negative
       * int that defeats the completion test below. */
      uint32_t sz   = le32(buf + 3);
      dc->cert_size = sz > 0x7fffffffU ? 0 : (int)sz;
      LOGI("   sensor cert %d size=%d (rx so far %d)", dc->cert_idx,
           dc->cert_size, dc->cert_rx);
      /* If every chunk already arrived before this announce, the U_ROUND
       * branch will get no further notify -- complete it here too, or P_CERT
       * stalls until the link drops. */
      cert_maybe_complete(dc);
   } else if (!strcmp(uuid, U_ROUND)) {
      dc->cert_rx += n;
      cert_maybe_complete(dc);
   }
}

/* THE KEY CHALLENGE that follows a certificate exchange. */
static void notify_keychal(struct dex_ctx *dc, const char *uuid,
                           const uint8_t *buf, int n)
{
   if (!strcmp(uuid, U_AUTH) && n >= 18 && buf[0] == 0x0c &&
       !dc->keychal_signed) {
      dc->keychal_signed = 1;
      LOGI("   sensor key-challenge; signing (ECDSA)");
      uint8_t sig[64];
      if (dexcom_getchallenge(buf, (size_t)n, sig)) {
         send_chunks(dc, sig, 64);
      } else {
         LOGI("!! sign failed");
         dc->phase = P_FAIL;
      }
   } else if (!strcmp(uuid, U_AUTH) && n >= 3 && buf[0] == 0x0d) {
      LOGI("   challenge accepted (0d); -> time-extended (06 1e)");
      uint8_t m[2] = {0x06, 0x1e};
      drv_write(dc->link, U_AUTH, m, 2, 0);
      goto_stream(dc);
   }
   /* sensor's 64-byte challenge blob on the round char is
    * accumulated/ignored */
}

/* STREAMING: the phase the sensor spends its life in -- glucose, backfill
 * and calibration responses. */
static void notify_stream(struct dex_ctx *dc, const char *uuid,
                          const uint8_t *buf, int n)
{
   if (!strcmp(uuid, U_CTRL) && n >= 19 && buf[0] == 0x4e) {
      struct dex_egv ev;
      if (dexdata_egv(buf, (size_t)n, &ev)) {
         dc->last_clock = ev.clock;
         /* mono_s(), NOT realtime_s(): this stamp is only ever used as the
          * base of an INTERVAL (see sens_project_clock), never as an instant
          * that is persisted or shown. */
         dc->last_clock_m   = mono_s();
         dc->last_state     = ev.state;
         dc->last_age       = ev.age;
         dc->last_glucose   = ev.glucose;
         dc->last_trend     = (int)ev.trend;
         dc->last_predicted = ev.predicted;
         dc->last_seq       = ev.sequence;
         /* state is LOGGED on purpose: the warmup-phase value has never
          * been captured from a live sensor here, and it is the byte
          * that would let the UI say WARMUP from the sensor's own mouth
          * rather than by inference. */
         LOGI("   EGV glucose=%d age=%d trend=%d clock=%u state=%d", ev.glucose,
              ev.age, ev.trend, ev.clock, ev.state);
         /* state 0x02 = WARMUP (captured live 2026-07-23): a fresh
          * session answers with state=2, a running clock, AND a glucose
          * value. The official Dexcom UIs hide warmup glucose; here it is
          * DELIBERATELY kept -- the standing rule is that every datapoint
          * the sensor produces is stored, recorded and displayed. The
          * state byte still drives the WARMUP label, so the user can see
          * the value is a warmup one. */
         /* STREAMING IS A STATEMENT ABOUT THE HISTORY, NOT ABOUT THE DECODE.
          *
          * These two lines used to run unconditionally the moment
          * dexdata_egv() succeeded. Decoding only says the 19 bytes had the
          * right shape; the app still refuses the value if it is outside
          * what a sensor can report, refuses the frame if its age backdates
          * it, and drops it entirely when no registered slot claims this
          * link yet and another CGM is already live (a second sensor's first
          * readings take that path by construction). On any of those the
          * user's screen does not move and the plot gains no point -- and
          * the driver was nonetheless clearing the failure streak that
          * exists to notice a sensor going bad, and writing this address
          * into the file that decides which sensor every future launch
          * reconnects to. Adopting a sensor permanently on the strength of a
          * reading nobody kept is the defect; the accepted flag is the
          * fix. */
         if (drv_glucose(dc->link, ev.glucose, ev.trend, ev.age)) {
            dc->streamed = 1;
            note_rx(dc);
            remember_sensor(dc);
         }
      }
   } else if (!strcmp(uuid, U_CTRL) && n >= 15 && buf[0] == 0x32) {
      /* calibration bounds; byte offsets confirmed against a live capture */
      dc->cal.have    = 1;
      dc->cal.last_bg = (int)((unsigned)buf[7] | ((unsigned)buf[8] << 8U));
      dc->cal.last_cal =
          (long)((uint32_t)buf[9] | ((uint32_t)buf[10] << 8U) |
                 ((uint32_t)buf[11] << 16U) | ((uint32_t)buf[12] << 24U));
      dc->cal.status    = buf[13];
      dc->cal.permitted = buf[14] != 0;
      loghex("   CAL BOUNDS 32", buf, n);
      LOGI("   permitted=%d status=%d lastBG=%d", dc->cal.permitted,
           dc->cal.status, dc->cal.last_bg);
   } else if (!strcmp(uuid, U_CTRL) && n >= 2 && buf[0] == 0x34) {
      /* Reply reuses the request opcode on this generation. Log the raw
       * bytes whatever the status: this response code is the thing no
       * public capture of a Stelo has recorded. */
      dc->cal.result = buf[1];
      loghex("   CALIBRATE REPLY 34", buf, n);
      LOGI("   calibration result=0x%02x (%s)", dc->cal.result,
           dc->cal.result == 0 ? "accepted" : "not accepted");
      /* Re-read the bounds ONLY for a calibration we sent. An unsolicited
       * 0x34 is either an echo or a hostile peer; answering it with a 0x32
       * write is what makes the exchange self-sustaining. */
      if (dc->cal_tx.pending) {
         /* THE TOKEN IS TAKEN BEFORE THE RECORD IS CLEARED, because
          * driver_cal_bounds below re-enters the driver and the answer must
          * name the write that was answered, not whatever the context holds
          * afterwards. */
         int done_id        = dc->cal_tx.sensor_id;
         int done_mg        = dc->cal_tx.mg_dl;
         unsigned done_g    = dc->cal_tx.gen;
         dc->cal_tx.pending = 0;
         driver_cal_bounds(dc->link); /* so the UI shows the new state */
         /* Tell the shell the OUTCOME so it can clear (accepted) or surface
          * (rejected) the durably-queued calibration -- never a silent drop.
          * WITH THE TOKEN of the write this answers, so the shell resolves the
          * calibration that was actually sent and nothing else. */
         drv_cal_result(dc->link, dc->cal.result, done_id, done_mg, done_g);
      } else {
         LOGI("   unsolicited 34 -- not re-reading bounds");
      }
   } else if (!strcmp(uuid, U_DATA)) {
      /* Size for what the TRANSPORT can actually deliver, not a guess.
       * jni_notify clamps a notification to 256 bytes and records are 9
       * bytes, so up to 28 can arrive; decoding only 8 silently dropped
       * the rest, and because a re-request returns the same frame the
       * loss was permanent -- gap recovery quietly losing exactly the
       * points it exists to recover. */
      struct dex_record r[DEX_MAX_RECORDS];
      int k = dexdata_records(buf, (size_t)n, r, DEX_MAX_RECORDS);
      /* HOW MANY OF THEM THE APP KEPT, which is not the same as how many
       * arrived: the gate below drops records with no session clock and
       * records whose age is impossible, and ingestion drops more. `k > 0`
       * used to be the whole test, so a batch of 28 records every one of
       * which was refused counted as a streaming connection. */
      int accepted = 0;
      LOGI("   %d backfill record(s)", k);
      for (int i = 0; i < k; i++) {
         LOGI("     rec ts=%u glu=%d", r[i].timestamp, r[i].glucose);
         /* age = how long ago this record was taken, from the sensor clock
          */
         /* NO SENSOR CLOCK => NO USABLE AGE, so drop the record rather
          * than fall back to 0. Age 0 means "taken just now", so every
          * historical record in the batch would be written to the
          * append-only log dated at the moment it ARRIVED -- a burst of
          * fabricated readings at the right edge of the plot. last_clock is
          * only set by a 0x4e, and goto_stream requests one first, so the
          * benign ordering is normal -- but a post-auth peer controls the
          * order, and pancra_backfill has no age bound of its own to catch
          * it (unlike pancra_glucose). */
         if (!dc->last_clock) {
            LOGI("     skipped: no session clock yet");
            continue;
         }
         long age = (long)dc->last_clock - (long)r[i].timestamp;
         /* Bound from BOTH ends. A record whose timestamp is 0 (or garbage)
          * against a mature session clock yields an age of up to the whole
          * session, which the host turns into a reading dated days in the
          * past -- written to a log that is never rewritten. Clamping only
          * at zero caught the negative case and let that one through.
          * A backfill record older than the sensor's own life is not a
          * record, so drop it rather than invent a timestamp for it. */
         if (age < 0 || age > 16L * 86400) {
            LOGI("     skipped: implausible age %ld s", age);
            continue;
         }
         accepted += drv_backfill(dc->link, r[i].glucose, 127,
                                  (int)age); /* 127 = trend unavailable */
      }
      LOGI("   %d of %d backfill record(s) accepted", accepted, k);
      /* AT LEAST ONE RECORD IN THE HISTORY. This was `k > 0` -- nonempty
       * batch, whatever became of it -- so a sensor whose every backfilled
       * value was refused had its failure streak cleared and its address
       * persisted as the one to reconnect to. */
      if (accepted > 0) {
         dc->streamed = 1;
         note_rx(dc);
         remember_sensor(dc);
      }
   }
}

/* ONE NOTIFY, DISPATCHED BY PHASE.
 *
 * The sensor's link carries five different conversations depending on where
 * the handshake has got to, and a frame that belongs to one phase must never
 * be decoded by another -- the opcodes overlap. This function was 290 lines
 * with all five inline, which is exactly how a frame ends up handled twice or
 * in the wrong state. Each phase is now a function that can be read on its
 * own; this is only the routing. */
void driver_on_notify(int link, const char *uuid, const uint8_t *buf, int n)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< onNotify %.8s dc->phase=%s", uuid, phase_name(dc->phase));
   loghex("   ", buf, n);
   if (dc->phase == P_ROUNDS && !strcmp(uuid, U_ROUND))
      notify_rounds(dc, buf, n);
   else if (dc->phase == P_AUTH && !strcmp(uuid, U_AUTH))
      notify_auth(dc, buf, n);
   else if (dc->phase == P_CERT)
      notify_cert(dc, uuid, buf, n);
   else if (dc->phase == P_KEYCHAL)
      notify_keychal(dc, uuid, buf, n);
   else if (dc->phase == P_STREAM)
      notify_stream(dc, uuid, buf, n);
   driver_leave();
}

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

/* The same question, for a caller with no snapshot of its own. */
int driver_free_cgm_link(int rank)
{
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   return driver_free_cgm_link_in(sess, rank);
}
