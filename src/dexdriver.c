// SPDX-License-Identifier: GPL-3.0
// dexdriver.c --- Dexcom ctx->pairing/reconnect protocol state machine
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver -- transport-agnostic Dexcom ctx->pairing/reconnect
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
#include "dexauth.h"
#include "dexcerts.h"
#include "dexdata.h"
#include "dexlibc.h"
#include "util.h" /* realtime_s */
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
 * keys and session. One context per transport link; `D` points at whichever
 * link the current callback belongs to, and every public entry point selects
 * it before doing anything else. */
struct dex_ctx {
   int phase;
   char g_mac[24];
   uint8_t g_code[8];
   int g_codelen;
   int streamed;
   int mac_saved; /* persisted this sensor's MAC this run (once) */
   struct dex_pairing *pairing;
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
    * glucose. There is no link-layer fallback: the app never calls createBond,
    * so this tokenHash is the ONLY thing authenticating the peer. */
   int chal_ok;
   uint32_t last_clock; /* sensor session-time from the latest 4e */
   long last_clock_t;   /* wall clock when last_clock was read, so the
                           session time can be projected forward between
                           responses (the official app's warmup countdown
                           ticks per second; ours must too) */
   uint8_t last_state;  /* raw session-state byte from the latest 4e --
                           logged and surfaced; during warmup the sensor
                           answers with no glucose but a running clock */
   uint16_t last_age;   /* age of that current reading, seconds */
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
   /* A calibration we actually sent is awaiting its 0x34 reply.
    *
    * The reply reuses the request opcode, and the handler re-reads the bounds
    * (a 0x32 write) so the UI reflects the new state. Without this flag any
    * 0x34 from the peer triggered that write, and firmware echoing it -- or a
    * post-auth peer choosing to -- sustained a 0x32/0x34 ping-pong at
    * connection-interval rate, draining both batteries and flooding the log
    * for as long as the link stayed up. */
   int cal_pending;
   int fails;     /* consecutive connects that never streamed */
   int authfails; /* subset: failures after we reached auth/cert */
   struct dex_cal cal;
};

static struct dex_ctx g_dctx[LINK_MAX];
static struct dex_ctx *ctx = &g_dctx[LINK_CGM];

static int g_cur_link = LINK_CGM;

/* Recursive spin lock guarding every access to `ctx` and the contexts it
 * points at. Recursive because drv_write() can complete synchronously and
 * re-enter driver_on_written() from inside a driver call. */
static volatile int lock_owner; /* gettid() of the holder, 0 when free */
static int lock_depth;

void driver_lock(void)
{
   int me = gettid();
   if (__atomic_load_n(&lock_owner, __ATOMIC_SEQ_CST) == me) {
      lock_depth++;
      return;
   }
   /* Yield while spinning. Most guarded sections are short, but a few reach
    * blocking JNI (drv_connect -> connectGatt) or file I/O, and a binder thread
    * busy-waiting through one of those burns a full core and can starve the
    * (small) binder pool. */
   while (!__sync_bool_compare_and_swap(&lock_owner, 0, me))
      sched_yield();
   lock_depth = 1;
}

void driver_unlock(void)
{
   if (--lock_depth > 0)
      return;
   __atomic_store_n(&lock_owner, 0, __ATOMIC_SEQ_CST);
}

void driver_select(int link)
{
   if (link < 0 || link >= LINK_MAX)
      link = LINK_CGM;
   g_cur_link = link;
   ctx        = &g_dctx[link];
}

int driver_link(void)
{
   return g_cur_link;
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
static void remember_sensor(void)
{
   if (!ctx->mac_saved && ctx->g_mac[0]) {
      drv_mac_save(ctx->g_mac);
      ctx->mac_saved = 1;
   }
}

void driver_get_session(struct dex_session *out)
{
   memset(out, 0, sizeof *out);
   int i = 0;
   for (; ctx->g_mac[i] && i < 23; i++)
      out->mac[i] = ctx->g_mac[i];
   out->mac[i]       = 0;
   out->bonded       = ctx->g_bonded;
   out->paired       = ctx->have_key;
   out->have_reading = (ctx->last_clock != 0);
   /* LIVE session time: the sensor's clock at the last response, projected
    * forward by the wall time since. Responses arrive minutes apart, but
    * countdowns built on this (warmup, session end) must tick per second,
    * exactly as the official app's do. */
   out->session_seconds =
       ctx->last_clock
           ? ctx->last_clock + (uint32_t)(realtime_s() - ctx->last_clock_t)
           : 0;
   out->state     = ctx->last_state;
   out->glucose   = ctx->last_glucose;
   out->trend     = ctx->last_trend;
   out->age       = ctx->last_age;
   out->predicted = ctx->last_predicted;
   out->sequence  = ctx->last_seq;
}

/* Send our certificate once the sensor's is fully received. Called from BOTH
 * the chunk (U_ROUND) and size-announce (0x0b) paths, since the two can arrive
 * in either order; the guard makes it fire exactly once per certificate. */
static void send_chunks(const uint8_t *buf, int len);

static void cert_maybe_complete(void)
{
   if (ctx->cert_sent || ctx->cert_size <= 0 || ctx->cert_rx < ctx->cert_size)
      return;
   ctx->cert_sent = 1;
   LOGI("   sensor cert %d received (%d); sending ours", ctx->cert_idx,
        ctx->cert_rx);
   send_chunks(ctx->cert_idx == 0 ? dex_cert0 : dex_cert1,
               ctx->cert_idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN);
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

/* issue a buffer to the round-transport char in 20-byte chunks; ctx->tx_left
 * counts acks */
static void send_chunks(const uint8_t *buf, int len)
{
   ctx->tx_left = (len + 19) / 20;
   LOGI("== send %d bytes in %d chunks ==", len, ctx->tx_left);
   for (int o = 0; o < len; o += 20) {
      int c = len - o > 20 ? 20 : len - o;
      drv_write(U_ROUND, buf + o, c, 1);
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
static int rand_bytes(uint8_t *dst, int n)
{
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0)
      return 0;
   long got = read(fd, dst, (unsigned)n);
   close(fd);
   return got == n;
}

/* Fresh 8-byte challenge token for this connection.
 *
 * The token is the ONLY thing that makes the sensor's AuthChallenge tokenHash
 * fresh: with a constant token the expected hash is a constant per key, so a
 * tokenHash once observed on air replays forever and the verification in
 * driver_on_notify proves nothing. Before rand_bytes this left ctx->token at
 * its previous value on failure -- on the first connection of a process, eight
 * zero bytes, because ctx is zero-initialised. */
static int gen_token(void)
{
   return rand_bytes(ctx->token, 8);
}

static void send_authrequest(void)
{
   if (!gen_token()) {
      LOGI("!! no entropy for the auth token -- refusing to authenticate");
      drv_status("NO ENTROPY");
      ctx->phase = P_FAIL;
      return;
   }
   /* Fresh token => any earlier verification is void. Clear it HERE, with the
    * token it was computed against, so the flag can never outlive its input. */
   ctx->chal_ok = 0;
   uint8_t m[10];
   m[0] = 0x02;
   memcpy(m + 1, ctx->token, 8);
   m[9] = 0x02;
   LOGI("== AuthRequest (02 ctx->token 02) ==");
   drv_write(U_AUTH, m, 10, 0);
   ctx->phase = P_AUTH;
}

static void request_round(void)
{
   LOGI("== request J-PAKE round %d (0a %02x) ==", ctx->round_idx + 1,
        ctx->round_idx);
   uint8_t m[2]    = {0x0a, (uint8_t)ctx->round_idx};
   ctx->rxlen      = 0;
   ctx->round_done = 0;
   drv_write(U_AUTH, m, 2, 0);
}

static void send_our_round(void)
{
   uint8_t pkt[160];
   int ok = 0;
   if (ctx->round_idx == 0)
      ok = dexpair_round1(ctx->pairing, pkt);
   else if (ctx->round_idx == 1)
      ok = dexpair_round2(ctx->pairing, pkt);
   else
      ok = dexpair_round3(ctx->pairing, pkt);
   if (!ok) {
      LOGI("!! round%d build failed", ctx->round_idx + 1);
      ctx->phase = P_FAIL;
      return;
   }
   LOGI("== send our round %d ==", ctx->round_idx + 1);
   send_chunks(pkt, 160);
}

/* announce a certificate (0b idx len) and expect the sensor's cert on the round
 * char */
static void start_cert(int idx)
{
   ctx->cert_idx  = idx;
   ctx->cert_rx   = 0;
   ctx->cert_size = 0;
   ctx->cert_sent = 0;
   int len        = idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN;
   uint8_t m[6]   = {0x0b,
                     (uint8_t)idx,
                     (uint8_t)len,
                     (uint8_t)((unsigned)len >> 8U),
                     (uint8_t)((unsigned)len >> 16U),
                     (uint8_t)((unsigned)len >> 24U)};
   LOGI("== certificate %d: announce (0b %02x len=%d) ==", idx, idx, len);
   drv_write(U_AUTH, m, 6, 0);
   ctx->phase = P_CERT;
}

static void start_keychallenge(void)
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
      ctx->phase = P_FAIL;
      return;
   }
   ctx->cert_rx        = 0; /* accumulate the sensor's 64-byte blob */
   ctx->keychal_signed = 0;
   drv_write(U_AUTH, m, 17, 0);
   ctx->phase = P_KEYCHAL;
}

static void goto_stream(void)
{
   LOGI("ctx->phase -> SUB2 (enable ctrl+data CCCDs, then getdata)");
   ctx->phase   = P_SUB2;
   ctx->sub_idx = 0;
   drv_subscribe(sub2[0].uuid, sub2[0].indicate);
}

void driver_init(void)
{
   dexauth_init();
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
   int prev = driver_link();
   for (int l = 0; l < LINK_MAX; l++) {
      if (l == LINK_METER)
         continue;
      driver_select(l);
      uint8_t k[16];
      if (drv_key_load(k)) {
         memcpy(ctx->shared_key, k, 16);
         ctx->have_key = 1;
         LOGI("link %d: loaded saved key", l);
      }
      /* Reconnect only to sensors we have bonded to: pre-load each link's MAC
       * so the app locks onto those exact devices and never touches another. */
      if (drv_mac_load(ctx->g_mac, (int)sizeof ctx->g_mac))
         LOGI("link %d: locked to saved sensor %s", l, ctx->g_mac);
   }
   driver_select(prev);
   driver_unlock();
}

/* Set the reconnect target MAC without initiating a connection. Used at startup
 * to re-lock onto the bonded sensor (resolved from the system bond list) when
 * files/stelo.mac is missing; the advert path then matches only this address.
 */
void driver_lock_mac(const char *mac)
{
   int i = 0;
   for (; mac[i] && i < 23; i++)
      ctx->g_mac[i] = mac[i];
   ctx->g_mac[i] = 0;
}

void driver_start(const char *mac, const char *code)
{
   int i = 0;
   for (; mac[i] && i < 23; i++)
      ctx->g_mac[i] = mac[i];
   ctx->g_mac[i]  = 0;
   ctx->g_codelen = 0;
   for (int j = 0; code[j] && ctx->g_codelen < 8; j++)
      ctx->g_code[ctx->g_codelen++] = (uint8_t)code[j];
   ctx->phase = P_IDLE;
   drv_status(ctx->have_key ? "WAITING" : "PAIRING");
   LOGI("driver_start mac=%s code(len=%d) ctx->have_key=%d", ctx->g_mac,
        ctx->g_codelen, ctx->have_key);
   drv_connect(ctx->g_mac);
}

void driver_on_connected(void)
{
   /* Free any pairing context from a previous attempt BEFORE re-walking the
    * subscribe sequence.
    *
    * This resets phase unconditionally, and a second on_connected without an
    * intervening disconnect overwrites ctx->pairing further down -- orphaning
    * a block that dexpair_free exists to memset. That leaks the J-PAKE
    * secrets (xA, xB, vA, vB, v3 and the passphrase-derived scalar) unwiped in
    * a process that runs for days. It is peer-reachable: Ble calls
    * discoverServices() on any onMtuChanged, including a peer-initiated MTU
    * exchange, and discovery completion calls back in here. */
   if (ctx->pairing) {
      dexpair_free(ctx->pairing);
      ctx->pairing = NULL;
   }
   LOGI("<< connected: services ready");
   drv_status("CONNECTED");
   ctx->phase   = P_SUB1;
   ctx->sub_idx = 0;
   LOGI("ctx->phase -> SUB1 (enable auth+round CCCDs)");
   drv_subscribe(sub1[0].uuid, sub1[0].indicate);
}

/* Continuous reconnect: once paired, reconnect for every sensor cycle (~5 min),
 * indefinitely and without ever giving up -- this app is meant to run 24/7 like
 * the official one. drv_connect uses autoConnect=true, a passive wait for the
 * sensor's next advertisement, so even a long run of failures is gentle: there
 * is no active scanning and no connect storm, just one armed reconnect at a
 * time. ctx->fails is kept only for logging the current streak. */
#define MAX_FAILS 15 /* connect-but-no-stream cap; pause to stop hammering */

void driver_on_disconnected(int status)
{
   LOGI("<< disconnected status=%d in ctx->phase=%s (ctx->streamed=%d)", status,
        phase_name(ctx->phase), ctx->streamed);
   int did_stream = ctx->streamed;
   ctx->streamed  = 0;
   if (ctx->pairing) {
      dexpair_free(ctx->pairing);
      ctx->pairing = NULL;
   }
   int was    = ctx->phase;
   ctx->phase = P_IDLE;
   if (did_stream) {
      ctx->fails     = 0;
      ctx->authfails = 0;
   } else {
      ctx->fails++;
      /* Authenticated (reached auth/cert/keychal) but never ctx->streamed. If
       * this repeats with a stored key, the key is stale -- another app (e.g.
       * the official Stelo app) re-paired the sensor. Drop the key so the next
       * connect re-pairs from scratch via the J-PAKE rounds (ctx->have_key=0
       * path).
       */
      if (ctx->have_key &&
          (was == P_AUTH || was == P_CERT || was == P_KEYCHAL) &&
          ++ctx->authfails >= 3) {
         LOGI("!! %d post-auth failures with a key -> discard key, re-pair",
              ctx->authfails);
         ctx->have_key  = 0;
         ctx->authfails = 0;
         drv_key_clear();
      }
   }
   /* Note: out-of-range does NOT count here -- autoConnect just waits for the
    * next advert without a failed connection. ctx->fails climbs only when we
    * connect yet can't stream, so the cap catches a genuine loop, not a quiet
    * sensor.
    */
   if (ctx->fails >= MAX_FAILS) {
      LOGI("!! %d straight failures -- pausing to avoid hammering (relaunch to "
           "retry)",
           ctx->fails);
      drv_status("CONNECTION ERROR");
   } else if (ctx->have_key || ctx->g_codelen > 0) {
      drv_status(ctx->have_key ? "WAITING" : "RE-PAIRING");
      LOGI("reconnect to %s (fail streak %d, was=%s)", ctx->g_mac, ctx->fails,
           phase_name(was));
      drv_connect(ctx->g_mac);
   } else {
      LOGI("no key/code -- not reconnecting (was=%s)", phase_name(was));
      drv_status("CONNECTION ERROR");
   }
}

/* Forget the paired sensor: drop the shared key/bond and reset all ctx->pairing
 * state so the next connection pairs from scratch (J-PAKE) with a fresh code.
 * Used by "PAIR NEW SENSOR" -- the caller then re-arms scanning for the new
 * one.
 */
void driver_forget(void)
{
   if (ctx->pairing) {
      dexpair_free(ctx->pairing);
      ctx->pairing = NULL;
   }
   /* Wipe the key, do not merely stop trusting it: this context is a
    * long-lived global, so "forget" left the 16 bytes readable in memory. */
   memset(ctx->shared_key, 0, sizeof ctx->shared_key);
   ctx->have_key   = 0;
   ctx->g_bonded   = 0;
   ctx->g_codelen  = 0;
   ctx->did_rounds = 0;
   ctx->fails      = 0;
   ctx->authfails  = 0;
   ctx->phase      = P_IDLE;
   ctx->mac_saved  = 0;
   drv_key_clear();
   drv_mac_clear();
   ctx->g_mac[0] = 0; /* unlock: allow ctx->pairing a different sensor */
   /* Clear the SESSION as well. driver_get_session() otherwise still reported
    * the previous sensor's clock, and sensor_reconcile derives activation from
    * it -- so a sensor paired right after a forget was stamped with the old
    * one's start time, permanently, in a file that is never rewritten. */
   ctx->last_clock     = 0;
   ctx->last_age       = 0;
   ctx->last_glucose   = 0;
   ctx->last_trend     = 0;
   ctx->last_predicted = 0;
   ctx->last_seq       = 0;
   ctx->streamed       = 0;
   ctx->cal            = (struct dex_cal){.result = -1};
   LOGI("driver_forget: key/bond dropped, ready to pair a new sensor");
}

/* Watchdog reconnect: re-issue a connect from a stalled state. The transport's
 * connect() closes any lingering GATT client first, so this recovers a stranded
 * link (e.g. an orphaned client after a crash/force-stop) without a BT toggle.
 * A no-op while actively streaming, so it never disturbs a healthy cycle. */
void driver_kick(void)
{
   if (ctx->phase == P_STREAM)
      return;
   if (ctx->have_key || ctx->g_codelen > 0) {
      ctx->phase = P_IDLE;
      /* Clear the give-up counters so the watchdog can always revive a link
       * that hit MAX_FAILS (e.g. a sensor that connected but never
       * ctx->streamed during warmup); otherwise ctx->fails stays >= MAX_FAILS
       * and every cycle re-shows CONNECTION ERROR with no reconnect. */
      ctx->fails     = 0;
      ctx->authfails = 0;
      LOGI("driver_kick: forcing a fresh reconnect to %s", ctx->g_mac);
      drv_connect(ctx->g_mac);
   }
}

/* Recover a gap: request records from (now - span) up to just before the
 * current reading. Endpoints are sensor session-time, mapped from the latest
 * 4e (ctx->last_clock/ctx->last_age). span is clamped to the sensor's ~24h
 * buffer.
 */
void driver_request_backfill(long span)
{
   if (ctx->phase != P_STREAM || ctx->last_clock == 0)
      return;
   long end = (long)ctx->last_clock -
              (long)ctx->last_age; /* current reading, session-time */
   if (end <= 1)
      return;
   if (span > 86400)
      span = 86400;
   long start = end - span;
   if (start < 0)
      start = 0;
   end -= 1; /* exclude the current reading */
   if (start >= end)
      return;
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
   drv_write(U_CTRL, m, 9, 0);
}

/* ---- calibration ---- */

void driver_get_cal(struct dex_cal *out)
{
   *out = ctx->cal;
}

void driver_cal_bounds(void)
{
   if (ctx->phase != P_STREAM) {
      LOGI("== ctx->cal bounds: not streaming, ignored ==");
      return;
   }
   uint8_t m[1]   = {0x32};
   ctx->cal.asked = realtime_s();
   LOGI("== ctx->cal bounds request 32 ==");
   drv_write(U_CTRL, m, 1, 0);
}

/* Returns 1 if the calibration write was actually issued, 0 if refused. The
 * shell uses this to keep a confirmed calibration QUEUED and retry it later
 * rather than dropping it -- a refusal here is transient (not yet streaming, or
 * the 0x32 permission not yet parsed), so it must never lose the value. */
int driver_calibrate(int mg_dl)
{
   if (ctx->phase != P_STREAM || ctx->last_clock == 0) {
      LOGI("== calibrate: not streaming, deferred ==");
      return 0;
   }
   /* Refuse a value the sensor would reject anyway, and refuse before we know
    * the firmware permits it -- so a stale UI can never push a blind write. */
   if (mg_dl < 40 || mg_dl > 400) {
      LOGI("== calibrate: %d out of range, refused ==", mg_dl);
      return 0;
   }
   /* Require a POSITIVE answer from 0x32, not merely the absence of a negative
    * one. `have && !permitted` let a calibration through whenever the bounds
    * probe had not answered yet -- which is exactly the blind write the
    * comment above says is refused, and the Stelo may well never answer 0x32
    * at all. The UI gates on cal_permitted for the same reason; this is the
    * driver-side backstop so no other caller can bypass it. */
   if (!ctx->cal.have || !ctx->cal.permitted) {
      LOGI("== calibrate: not permitted (have=%d permitted=%d) ==",
           ctx->cal.have, ctx->cal.permitted);
      return 0;
   }
   uint32_t when = ctx->last_clock; /* sensor clock, seconds since activation */
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
   ctx->cal.result  = -1; /* fresh: -1 awaiting, 0 accepted, >0 rejected */
   ctx->cal_pending = 1;
   drv_write(U_CTRL, m, 7, 0);
   return 1;
}

void driver_on_written(const char *uuid, int status)
{
   LOGI("<< onWritten %.8s status=%d ctx->phase=%s", uuid, status,
        phase_name(ctx->phase));
   if (ctx->phase == P_SUB1) {
      if (strcmp(uuid, sub1[ctx->sub_idx].uuid) != 0) {
         LOGI("   (ignore stray ack)");
         return;
      }
      if (++ctx->sub_idx < NSUB1) {
         drv_subscribe(sub1[ctx->sub_idx].uuid, sub1[ctx->sub_idx].indicate);
      } else if (ctx->have_key) {
         ctx->did_rounds = 0;
         send_authrequest();
      } else {
         ctx->did_rounds = 1;
         ctx->pairing    = dexpair_new(ctx->g_code, ctx->g_codelen, 1);
         if (!ctx->pairing) { /* allocation failed; the round handlers would
                                 dereference it unguarded */
            LOGI("!! pairing alloc failed");
            ctx->phase = P_FAIL;
            return;
         }
         ctx->round_idx = 0;
         ctx->phase     = P_ROUNDS;
         request_round();
      }
   } else if (ctx->phase == P_ROUNDS) {
      if (!strcmp(uuid, U_ROUND) && ctx->tx_left > 0) {
         ctx->tx_left--;
         if (ctx->tx_left == 0) {
            if (++ctx->round_idx < 3) {
               request_round();
            } else if (!dexpair_shared_key(ctx->pairing, ctx->shared_key)) {
               LOGI("!! sharedkey fail");
               ctx->phase = P_FAIL;
            } else {
               ctx->have_key = 1;
               loghex("SHAREDKEY(derived)", ctx->shared_key, 16);
               drv_key_save(ctx->shared_key);
               send_authrequest();
            }
         }
      }
   } else if (ctx->phase == P_CERT) {
      /* our cert chunks being acked; when all sent, next cert or key challenge
       */
      if (!strcmp(uuid, U_ROUND) && ctx->tx_left > 0) {
         ctx->tx_left--;
         if (ctx->tx_left == 0) {
            if (ctx->cert_idx == 0)
               start_cert(1);
            else
               start_keychallenge();
         }
      }
   } else if (ctx->phase == P_KEYCHAL) {
      /* our signature chunks acked; when all sent, write 0d 00 02 */
      if (!strcmp(uuid, U_ROUND) && ctx->tx_left > 0) {
         ctx->tx_left--;
         if (ctx->tx_left == 0) {
            uint8_t m[3] = {0x0d, 0x00, 0x02};
            LOGI("   -> challenge out (0d 00 02)");
            drv_write(U_AUTH, m, 3, 0);
         }
      }
   } else if (ctx->phase == P_SUB2) {
      if (strcmp(uuid, sub2[ctx->sub_idx].uuid) != 0) {
         LOGI("   (ignore stray ack)");
         return;
      }
      if (++ctx->sub_idx < NSUB2) {
         drv_subscribe(sub2[ctx->sub_idx].uuid, sub2[ctx->sub_idx].indicate);
      } else {
         LOGI("== get data (write 4e) ==");
         uint8_t c = 0x4e;
         drv_write(U_CTRL, &c, 1, 0);
         ctx->phase = P_STREAM;
      }
   }
}

void driver_on_notify(const char *uuid, const uint8_t *buf, int n)
{
   LOGI("<< onNotify %.8s ctx->phase=%s", uuid, phase_name(ctx->phase));
   loghex("   ", buf, n);
   if (ctx->phase == P_ROUNDS && !strcmp(uuid, U_ROUND)) {
      /* Copy what fits rather than dropping an oversized notify whole. The MTU
       * is 185, so a 160-byte round can arrive in chunks that straddle the
       * boundary; dropping one left rxlen stuck below 160, the completion test
       * never fired, and pairing hung with no timeout -- the stall watchdog is
       * gated on `paired`, which is 0 during first-time pairing. */
      int room = 160 - ctx->rxlen;
      if (room > 0) {
         int take = n < room ? n : room;
         memcpy(ctx->rxbuf + ctx->rxlen, buf, (size_t)take);
         ctx->rxlen += take;
      }
      /* Handle each completed round EXACTLY once. rxlen is only reset by
       * request_round(), which does not run until our last chunk is acked, so
       * it stays pinned at 160 in between. Any further U_ROUND notify in that
       * window (a peer retransmit, or a stray frame from a hostile peer on the
       * link) re-ran the ZKP on the stale buffer and called send_chunks again,
       * resetting tx_left to 8 while 8 writes were still in flight. The acks
       * then no longer matched the outstanding writes: tx_left hit 0 early,
       * the remaining acks were dropped, round_idx never advanced, and pairing
       * stalled forever -- with no timeout, because the stall watchdog is gated
       * on `paired`, which is 0 during first-time pairing. P_CERT already has
       * this guard as cert_sent; this is the missing equivalent. */
      if (ctx->rxlen >= 160 && !ctx->round_done) {
         ctx->round_done = 1;
         int ok          = 0;
         if (ctx->round_idx == 0)
            ok = dexpair_peer_round1(ctx->pairing, ctx->rxbuf);
         else if (ctx->round_idx == 1)
            ok = dexpair_peer_round2(ctx->pairing, ctx->rxbuf);
         else
            ok = dexpair_peer_round3(ctx->pairing, ctx->rxbuf);
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
                 ctx->round_idx + 1);
            drv_status("PAIR FAILED");
            ctx->phase = P_FAIL;
            return;
         }
         LOGI("   peer round%d ZKP VALID", ctx->round_idx + 1);
         send_our_round();
      }
   } else if (ctx->phase == P_AUTH && !strcmp(uuid, U_AUTH)) {
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
         dexauth_dex8(ctx->shared_key, ctx->token, expect);
         if (memcmp(expect, buf + 1, 8) != 0) {
            LOGI(
                "!! AuthChallenge tokenHash MISMATCH -- peer does not hold the "
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
            if (ctx->have_key) {
               LOGI("!! stale key proven by tokenHash mismatch -- discarding "
                    "it now so the sensor re-pairs on the next connect");
               ctx->have_key  = 0;
               ctx->authfails = 0;
               drv_key_clear();
            }
            ctx->phase = P_FAIL;
            return;
         }
         LOGI("   AuthChallenge (03) verified -> ChallengeReply (04)");
         ctx->chal_ok = 1;
         uint8_t reply[9];
         reply[0] = 0x04;
         dexauth_dex8(ctx->shared_key, buf + 9, reply + 1);
         drv_write(U_AUTH, reply, 9, 0);
      } else if (n >= 3 && buf[0] == 0x05) {
         int auth      = buf[1];
         int bond      = buf[2];
         ctx->g_bonded = (auth == 1);
         LOGI("   AuthStatus (05) auth=%02x bond=%02x", auth, bond);
         if (auth == 0) {
            LOGI("!! auth failed");
            drv_status("AUTH FAILED");
            ctx->phase = P_FAIL;
         } else if (!ctx->chal_ok) {
            /* THE PEER NEVER PROVED IT HOLDS THE KEY. A genuine sensor always
             * sends AuthChallenge (03) before AuthStatus (05) -- that is the
             * captured order in both the pairing and the bonded-reconnect
             * flow. Accepting 05 on its own let a peer on the locked MAC skip
             * the proof entirely and reach P_STREAM, after which its 4e frames
             * became the headline number, the alarm input, and permanent rows
             * in readings.csv. */
            LOGI("!! AuthStatus without a verified AuthChallenge -- refusing");
            drv_status("AUTH FAILED");
            ctx->phase = P_FAIL;
         } else if (ctx->did_rounds || auth != 1) {
            /* establish/refresh the bond via the certificate exchange */
            drv_status(ctx->did_rounds ? "PAIRED" : "BONDING");
            start_cert(0);
         } else {
            LOGI("   bonded reconnect -> stream");
            drv_status("AUTHENTICATED");
            goto_stream();
         }
      }
   } else if (ctx->phase == P_CERT) {
      if (!strcmp(uuid, U_AUTH) && n >= 7 && buf[0] == 0x0b) {
         /* size announce; don't reset ctx->cert_rx -- the sensor streams some
          * cert chunks before this arrives (start_cert already zeroed it). le32
          * is unsigned; clamp so a garbage/huge size can't become a negative
          * int that defeats the completion test below. */
         uint32_t sz    = le32(buf + 3);
         ctx->cert_size = sz > 0x7fffffffU ? 0 : (int)sz;
         LOGI("   sensor cert %d size=%d (rx so far %d)", ctx->cert_idx,
              ctx->cert_size, ctx->cert_rx);
         /* If every chunk already arrived before this announce, the U_ROUND
          * branch will get no further notify -- complete it here too, or P_CERT
          * stalls until the link drops. */
         cert_maybe_complete();
      } else if (!strcmp(uuid, U_ROUND)) {
         ctx->cert_rx += n;
         cert_maybe_complete();
      }
   } else if (ctx->phase == P_KEYCHAL) {
      if (!strcmp(uuid, U_AUTH) && n >= 18 && buf[0] == 0x0c &&
          !ctx->keychal_signed) {
         ctx->keychal_signed = 1;
         LOGI("   sensor key-challenge; signing (ECDSA)");
         uint8_t sig[64];
         if (dexauth_getchallenge(buf, (size_t)n, sig)) {
            send_chunks(sig, 64);
         } else {
            LOGI("!! sign failed");
            ctx->phase = P_FAIL;
         }
      } else if (!strcmp(uuid, U_AUTH) && n >= 3 && buf[0] == 0x0d) {
         LOGI("   challenge accepted (0d); -> time-extended (06 1e)");
         uint8_t m[2] = {0x06, 0x1e};
         drv_write(U_AUTH, m, 2, 0);
         goto_stream();
      }
      /* sensor's 64-byte challenge blob on the round char is
       * accumulated/ignored */
   } else if (ctx->phase == P_STREAM) {
      if (!strcmp(uuid, U_CTRL) && n >= 19 && buf[0] == 0x4e) {
         struct dex_egv ev;
         if (dexdata_egv(buf, (size_t)n, &ev)) {
            ctx->last_clock     = ev.clock;
            ctx->last_clock_t   = realtime_s();
            ctx->last_state     = ev.state;
            ctx->last_age       = ev.age;
            ctx->last_glucose   = ev.glucose;
            ctx->last_trend     = (int)ev.trend;
            ctx->last_predicted = ev.predicted;
            ctx->last_seq       = ev.sequence;
            /* state is LOGGED on purpose: the warmup-phase value has never
             * been captured from a live sensor here, and it is the byte
             * that would let the UI say WARMUP from the sensor's own mouth
             * rather than by inference. */
            LOGI("   EGV glucose=%d age=%d trend=%d clock=%u state=%d",
                 ev.glucose, ev.age, ev.trend, ev.clock, ev.state);
            /* state 0x02 = WARMUP (captured live 2026-07-23): a fresh
             * session answers with state=2, a running clock, AND a glucose
             * value. The official Dexcom UIs hide warmup glucose; here it is
             * DELIBERATELY kept -- the standing rule is that every datapoint
             * the sensor produces is stored, recorded and displayed. The
             * state byte still drives the WARMUP label, so the user can see
             * the value is a warmup one. */
            drv_glucose(ev.glucose, ev.trend, ev.age);
            ctx->streamed = 1;
            remember_sensor();
         }
      } else if (!strcmp(uuid, U_CTRL) && n >= 15 && buf[0] == 0x32) {
         /* calibration bounds; byte offsets confirmed against a live capture */
         ctx->cal.have    = 1;
         ctx->cal.last_bg = (int)((unsigned)buf[7] | ((unsigned)buf[8] << 8U));
         ctx->cal.last_cal =
             (long)((uint32_t)buf[9] | ((uint32_t)buf[10] << 8U) |
                    ((uint32_t)buf[11] << 16U) | ((uint32_t)buf[12] << 24U));
         ctx->cal.status    = buf[13];
         ctx->cal.permitted = buf[14] != 0;
         loghex("   CAL BOUNDS 32", buf, n);
         LOGI("   permitted=%d status=%d lastBG=%d", ctx->cal.permitted,
              ctx->cal.status, ctx->cal.last_bg);
      } else if (!strcmp(uuid, U_CTRL) && n >= 2 && buf[0] == 0x34) {
         /* Reply reuses the request opcode on this generation. Log the raw
          * bytes whatever the status: this response code is the thing no
          * public capture of a Stelo has recorded. */
         ctx->cal.result = buf[1];
         loghex("   CALIBRATE REPLY 34", buf, n);
         LOGI("   calibration result=0x%02x (%s)", ctx->cal.result,
              ctx->cal.result == 0 ? "accepted" : "not accepted");
         /* Re-read the bounds ONLY for a calibration we sent. An unsolicited
          * 0x34 is either an echo or a hostile peer; answering it with a 0x32
          * write is what makes the exchange self-sustaining. */
         if (ctx->cal_pending) {
            ctx->cal_pending = 0;
            driver_cal_bounds(); /* so the UI shows the new state */
            /* Tell the shell the OUTCOME so it can clear (accepted) or surface
             * (rejected) the durably-queued calibration -- never a silent drop.
             */
            drv_cal_result(ctx->cal.result);
         } else {
            LOGI("   unsolicited 34 -- not re-reading bounds");
         }
      } else if (!strcmp(uuid, U_DATA)) {
         struct dex_record r[8];
         int k = dexdata_records(buf, (size_t)n, r, 8);
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
            if (!ctx->last_clock) {
               LOGI("     skipped: no session clock yet");
               continue;
            }
            long age = (long)ctx->last_clock - (long)r[i].timestamp;
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
            drv_backfill(r[i].glucose, 127,
                         (int)age); /* 127 = trend unavailable */
         }
         if (k > 0) {
            ctx->streamed = 1;
            remember_sensor();
         }
      }
   }
}
