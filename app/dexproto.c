// SPDX-License-Identifier: GPL-3.0
// dexproto.c --- Dexcom pairing/reconnect protocol state machine
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver -- transport-agnostic Dexcom dc->rnd.pairing/reconnect
 * state machine. See dexdriver.h. Event-driven: each transport callback
 * advances the state and issues the next operation via the drv_* hooks. Heavily
 * logged.
 *
 * THIS FILE IS THE PROTOCOL HALF ONLY. Which sensor a callback belongs to --
 * the link contexts and their lock, the roles, the arming, the retry claims,
 * the routing and the calibration queue -- is dexlink.c; the two share
 * dexpriv.h and nothing else, and the public face of both is dexdriver.h.
 * Every entry point here still begins the same way: validate the link OUTSIDE
 * the lock (dex_link_ok), then driver_enter() to take it and name the context.
 *
 * Flow (validated against a real Stelo capture):
 *   connect -> subscribe auth+round -> [fresh: J-PAKE rounds] -> 02/03/04/05
 * auth
 *   -> certificate exchange (0b/0c/0d + ECDSA) to establish a streamable bond
 *   -> 06 1e -> subscribe ctrl+data -> 4e getdata -> stream EGV/backfill.
 * A bonded reconnect (auth==1) skips rounds and certs and streams directly.
 */
/* dexpriv.h FIRST, in a block of its own: it pulls dexdriver.h, and
 * dexport.h refuses to be read without it. A single sorted block would put
 * dexport.h ahead of it (o before p) and the build would stop there. */
#include "dexdriver.h" /* LINK_MAX, struct dex_session, the drv_* upcalls */
#include "dexpriv.h"   /* struct dex_ctx, driver_enter: the state we share */

#include "clock.h"
#include "ct.h" /* ct_wipe: a clear the compiler may not delete */
#include "dexcerts.h"
#include "dexcom.h"
#include "dexdata.h"
#include "dexport.h" /* the host port: what the driver asks of us */
#include "log.h"     /* LOGI/LOGW: the ONE declaration */
/* gettid and sched_yield come from thread.h, with every other lock primitive
 * in the app, rather than from dexlibc.h. */
#include "jpake.h"
#include "rand.h"
#include "scanlogic.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The phase, spelled for a log line. The switch is exhaustive and has no
 * default, so a phase added to the enum is a compile error here rather than a
 * connection that logs "?" at the one moment somebody is reading the log. */
const char *dex_phase_name(enum dex_phase p)
{
   switch (p) {
   case P_IDLE:
      return "IDLE";
   case P_SUB1:
      return "SUB1";
   case P_ROUNDS:
      return "ROUNDS";
   case P_AUTH:
      return "AUTH";
   case P_CERT:
      return "CERT";
   case P_KEYCHAL:
      return "KEYCHAL";
   case P_SUB2:
      return "SUB2";
   case P_STREAM:
      return "STREAM";
   case P_FAIL:
      return "FAIL";
   }
   return "?";
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
 * A monotonic read that FAILS leaves .mono untouched, which for a
 * never-stamped context is MONO_NEVER: the watchdog then measures from the
 * process's launch instead, which is the same fallback it uses before the
 * first reading ever arrives. It is never stamped with a fabricated zero --
 * see clock.h for why that distinction had to be made a value. */
static void note_rx(struct dex_ctx *dc)
{
   dc->str.last_rx.wall = realtime_s(); /* the INSTANT: identity, persisted */
   long m               = 0;
   if (mono_try(&m) == MONO_GET_OK)
      dc->str.last_rx.mono = m; /* the DEADLINE: an interval, never persisted */
}

/* Send our certificate once the sensor's is fully received. Called from BOTH
 * the chunk (U_ROUND) and size-announce (0x0b) paths, since the two can arrive
 * in either order; the guard makes it fire exactly once per certificate. */
static void send_chunks(struct dex_ctx *dc, const uint8_t *buf, int len);

static void cert_maybe_complete(struct dex_ctx *dc)
{
   if (dc->cert.sent || dc->cert.size <= 0 || dc->cert.rx < dc->cert.size)
      return;
   dc->cert.sent = 1;
   LOGI("   sensor cert %d received (%d); sending ours", dc->cert.idx,
        dc->cert.rx);
   send_chunks(dc, dc->cert.idx == 0 ? dex_cert0 : dex_cert1,
               dc->cert.idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN);
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
/* (rand_bytes lives in lib/rand.c, and there is deliberately no copy here: a
 * local one doing a SINGLE read and returning got == n treats a short read --
 * which /dev/urandom is permitted to give -- as failure where lib's retries,
 * and it shadows lib's exported symbol with a different signature inside the
 * same .so.) */

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

static void request_round(struct dex_ctx *dc);

/* ---- ENTERING A PHASE IS A FUNCTION, AND IT OWNS ITS OWN STATE --------
 *
 * Each of these sets the phase and initialises EVERY field of that
 * phase's group -- the group being a struct in dexpriv.h rather than a dozen
 * flat fields whose live meaning depended on which branch of a long if-chain
 * you were reading. The clearing is wholesale (memset of the group, not field
 * by field) for one reason: a field added to a group later is then cleared by
 * construction, and the failure that motivates this is precisely the field
 * somebody forgot to reset. `rxlen stuck below 160` -- see notify_rounds --
 * was that, and it made every subsequent round of that connection silently
 * un-handleable.
 *
 * They return int where entering can FAIL (no entropy, no allocation): the
 * caller must not carry on into a phase whose state was never established.
 * The two that cannot fail return void, so a caller cannot invent a check
 * that has nothing to test. */

/* P_ROUNDS: a FRESH pairing, with the applicator code. 0 on failure, with the
 * phase already set to P_FAIL. */
static int enter_rounds(struct dex_ctx *dc)
{
   memset(&dc->rnd, 0, sizeof dc->rnd);
   dc->rnd.did     = 1; /* this connection paired: outlives the phase */
   dc->rnd.pairing = jpake_new(dc->g_code, dc->g_codelen, 1);
   if (!dc->rnd.pairing) { /* the round handlers would dereference it
                            * unguarded */
      LOGI("!! pairing alloc failed");
      dc->phase = P_FAIL;
      return 0;
   }
   dc->phase = P_ROUNDS;
   request_round(dc);
   return 1;
}

static void request_round(struct dex_ctx *dc)
{
   LOGI("== request J-PAKE round %d (0a %02x) ==", dc->rnd.idx + 1,
        dc->rnd.idx);
   uint8_t m[2] = {0x0a, (uint8_t)dc->rnd.idx};
   dc->rnd.len  = 0;
   dc->rnd.done = 0;
   drv_write(dc->link, U_AUTH, m, 2, 0);
}

static void send_our_round(struct dex_ctx *dc)
{
   uint8_t pkt[160];
   int ok = 0;
   if (dc->rnd.idx == 0)
      ok = jpake_round1(dc->rnd.pairing, pkt);
   else if (dc->rnd.idx == 1)
      ok = jpake_round2(dc->rnd.pairing, pkt);
   else
      ok = jpake_round3(dc->rnd.pairing, pkt);
   if (!ok) {
      LOGI("!! round%d build failed", dc->rnd.idx + 1);
      dc->phase = P_FAIL;
      return;
   }
   LOGI("== send our round %d ==", dc->rnd.idx + 1);
   send_chunks(dc, pkt, 160);
}

/* announce a certificate (0b idx len) and expect the sensor's cert on the round
 * char */
static void enter_cert(struct dex_ctx *dc, int idx)
{
   memset(&dc->cert, 0, sizeof dc->cert);
   dc->cert.idx = idx;
   int len      = idx == 0 ? DEX_CERT0_LEN : DEX_CERT1_LEN;
   uint8_t m[6] = {0x0b,
                   (uint8_t)idx,
                   (uint8_t)len,
                   (uint8_t)((unsigned)len >> 8U),
                   (uint8_t)((unsigned)len >> 16U),
                   (uint8_t)((unsigned)len >> 24U)};
   LOGI("== certificate %d: announce (0b %02x len=%d) ==", idx, idx, len);
   drv_write(dc->link, U_AUTH, m, 6, 0);
   dc->phase = P_CERT;
}

/* P_KEYCHAL: one challenge, one signature. 0 on failure (no entropy), with
 * the phase already set to P_FAIL. */
static int enter_keychal(struct dex_ctx *dc)
{
   LOGI("== key challenge (0c + random16) ==");
   uint8_t m[17];
   m[0] = 0x0c;
   /* m is an UNINITIALISED stack buffer, so a failure to fill m[1..16] must
    * not be swallowed: transmitting anyway sends 16 bytes of whatever the
    * stack last held -- leaking process memory over the air and, worse,
    * sending a key challenge that is not random at all. */
   if (!rand_bytes(m + 1, 16)) {
      LOGI("!! no entropy for the key challenge -- refusing to bond");
      drv_status("NO ENTROPY");
      dc->phase = P_FAIL;
      return 0;
   }
   memset(&dc->keychal, 0, sizeof dc->keychal);
   /* THE CERT GROUP'S rx IS THE ACCUMULATOR HERE TOO: the sensor's 64-byte
    * signature blob arrives on the same characteristic, in the same chunked
    * way, and is counted the same way. It is reset here rather than left over
    * from the certificate that has just completed. */
   dc->cert.rx = 0;
   drv_write(dc->link, U_AUTH, m, 17, 0);
   dc->phase = P_KEYCHAL;
   return 1;
}

/* P_SUB2, which is how P_STREAM is reached: the control and data CCCDs are
 * enabled one at a time and the last acknowledgement makes the phase STREAM
 * (see driver_on_written). The stream group is NOT cleared here -- it holds
 * the last reading, which must survive a reconnect; driver_forget is what
 * ends its life. */
static void enter_sub2(struct dex_ctx *dc)
{
   LOGI("dc->phase -> SUB2 (enable ctrl+data CCCDs, then getdata)");
   dc->phase   = P_SUB2;
   dc->sub_idx = 0;
   drv_subscribe(dc->link, sub2[0].uuid, sub2[0].indicate);
}

/* ONE LINK'S STARTUP STATE, run for every link by driver_each_ctx under a
 * single hold of the driver's lock (dexlink.c owns that lock; this unit never
 * names it). */
static void init_ctx(struct dex_ctx *dc)
{
   /* Establish the documented "-1 = none yet" sentinel.
    *
    * The contexts are zero-initialised statics, and only driver_forget ever
    * set this -- so on a fresh process with a saved key, tapping READ BOUNDS
    * set cal.have without touching result, and the UI rendered "LAST RESULT
    * 0x00" in green: a calibration submitted and accepted, when none ever
    * was. */
   dc->cal.result = -1;
   uint8_t k[16];
   if (drv_key_load(dc->link, k)) {
      memcpy(dc->shared_key, k, 16);
      dc->have_key = 1;
      LOGI("link %d: loaded saved key", dc->link);
   }
   /* Reconnect only to sensors we have bonded to: pre-load each link's MAC so
    * the app locks onto those exact devices and never touches another. */
   if (drv_mac_load(dc->link, dc->g_mac, (int)sizeof dc->g_mac))
      LOGI("link %d: locked to the saved sensor", dc->link);
}

void driver_init(void)
{
   jpake_init();
   /* EVERY CGM link, not just the ambient one, and INCLUDING LINK 1.
    *
    * The key and MAC files are per-link (stelo.key, stelo.key.2, ...), so
    * loading only whichever context happens to be selected -- always LINK_CGM
    * at startup -- leaves links 2..N beginning every process with have_key = 0
    * and an empty MAC. A second CGM's advert then hits the "no saved MAC"
    * guard and is never reconnected; had it connected it would demand a fresh
    * J-PAKE pairing with an applicator code the user no longer has. A second
    * sensor goes silent at the first app restart, permanently.
    *
    * SKIPPING LINK_METER would be treating that constant as though it named a
    * link a meter permanently owns -- an idiom main.c and dexble.c both
    * record replacing with a per-link fact, because the link a meter occupies
    * is whatever the allocator gave it. driver_free_cgm_link allocates by that
    * dynamic table, and at cold start nothing is armed and no link is flagged
    * as a meter, so link 1 is an ordinary free link and a CGM can be given it.
    * Skip it and that sensor begins every process unpaired -- verbatim the
    * failure above, live for one link. There is nothing to protect anyway:
    * a link that never held a CGM has no files and loads nothing. The files
    * are the fact; the constant was a guess. */
   driver_each_ctx(init_ctx);
}

/* Set the reconnect target MAC without initiating a connection. Used at startup
 * to re-lock onto the bonded sensor (resolved from the system bond list) when
 * files/stelo.mac is missing; the advert path then matches only this address.
 */
void driver_lock_mac(int link, const char *mac)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!dex_link_ok(link))
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
   if (!dex_link_ok(link))
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
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   /* Free any pairing context from a previous attempt BEFORE re-walking the
    * subscribe sequence.
    *
    * This resets phase unconditionally, and a second on_connected without an
    * intervening disconnect overwrites dc->rnd.pairing further down --
    * orphaning a block that jpake_free exists to memset. That leaks the J-PAKE
    * secrets (xA, xB, vA, vB, v3 and the passphrase-derived scalar) unwiped in
    * a process that runs for days. It is peer-reachable: Ble calls
    * discoverServices() on any onMtuChanged, including a peer-initiated MTU
    * exchange, and discovery completion calls back in here. */
   if (dc->rnd.pairing) {
      jpake_free(dc->rnd.pairing);
      dc->rnd.pairing = NULL;
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
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< disconnected status=%d in dc->phase=%s (dc->str.streamed=%d)",
        status, dex_phase_name(dc->phase), dc->str.streamed);
   int did_stream   = dc->str.streamed;
   dc->str.streamed = 0;
   if (dc->rnd.pairing) {
      jpake_free(dc->rnd.pairing);
      dc->rnd.pairing = NULL;
   }
   /* THE PHASE, AS A PHASE. Held in an int and handed back to
    * dex_phase_name, it is a value the compiler cannot vouch for; the field
    * is an enum and so is this. */
   enum dex_phase was = dc->phase;
   dc->phase          = P_IDLE;
   if (did_stream) {
      dc->fails     = 0;
      dc->authfails = 0;
   } else {
      dc->fails++;
      /* Authenticated (reached auth/cert/keychal) but never dc->str.streamed.
       * If this repeats with a stored key, the key is stale -- another app
       * (e.g. the official Stelo app) re-paired the sensor. Drop the key so the
       * next connect re-pairs from scratch via the J-PAKE rounds
       * (dc->have_key=0 path).
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
         /* A KEY FILE THAT SURVIVES IS A KEY THE NEXT LAUNCH LOADS BACK
          *. Memory has already dropped it, so this connection
          * re-pairs either way; saying so is what stops the failure being
          * invisible when the same sensor reconnects tomorrow with the same
          * dead key. */
         if (drv_key_clear(dc->link) != 0)
            LOGI("!! the stale key is still on disk -- it will be loaded "
                 "again at the next launch");
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
           dex_phase_name(was));
      drv_connect(dc->link, dc->g_mac);
   } else {
      LOGI("no key/code -- not reconnecting (was=%s)", dex_phase_name(was));
      drv_status("CONNECTION ERROR");
   }
   driver_leave();
}

/* Forget the paired sensor: drop the shared key/bond and reset all
 * dc->rnd.pairing state so the next connection pairs from scratch (J-PAKE) with
 * a fresh code. Used by "PAIR NEW SENSOR" -- the caller then re-arms scanning
 * for the new one.
 */
void driver_forget(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->rnd.pairing) {
      jpake_free(dc->rnd.pairing);
      dc->rnd.pairing = NULL;
   }
   /* Wipe the key, do not merely stop trusting it: this context is a
    * long-lived global, so "forget" left the 16 bytes readable in memory.
    *
    * ct_wipe, NOT memset, and the difference is the whole point of the line.
    * A compiler is entitled to delete a memset whose result is never read --
    * that is the dead-store elimination every optimiser performs, and it is
    * why explicit_bzero exists at all. This clear IS never read: the fields
    * below say the key is gone. So the one write that must survive -O2 is
    * exactly the one the optimiser is most confident it can drop. See ct.h.
    *
    * THE PAIRING CODE AND THE TOKEN GO WITH IT. The code is the six digits
    * the user read off the sensor and typed, and the token is what proves
    * this connection was authenticated; both sat in a long-lived global with
    * only a length counter set to zero. Clearing the length made them
    * invisible to this file's own readers and to nothing else. */
   ct_wipe(dc->shared_key, sizeof dc->shared_key);
   ct_wipe(dc->g_code, sizeof dc->g_code);
   ct_wipe(dc->token, sizeof dc->token);
   dc->have_key  = 0;
   dc->g_bonded  = 0;
   dc->g_codelen = 0;
   dc->rnd.did   = 0;
   dc->fails     = 0;
   dc->authfails = 0;
   dc->phase     = P_IDLE;
   dc->mac_saved = 0;
   /* THE USER ASKED TO FORGET THIS SENSOR, so a credential that survives is
    * the one failure that matters here: memory says unpaired, the screen says
    * unpaired, and the next launch loads the key back and reconnects to the
    * sensor they told the app to forget. Both are attempted --
    * the address surviving without the key is not better -- and either
    * failure is said out loud. */
   int kept = 0;
   if (drv_key_clear(dc->link) != 0)
      kept = 1;
   if (drv_mac_clear(dc->link) != 0)
      kept = 1;
   dc->g_mac[0] = 0; /* unlock: allow dc->rnd.pairing a different sensor */
   /* Clear the SESSION as well. driver_get_session() otherwise still reported
    * the previous sensor's clock, and sensor_reconcile derives activation from
    * it -- so a sensor paired right after a forget was stamped with the old
    * one's start time, permanently, in a file that is never rewritten. */
   dc->str.clock     = 0;
   dc->str.age       = 0;
   dc->str.glucose   = 0;
   dc->str.trend     = 0;
   dc->str.predicted = 0;
   dc->str.seq       = 0;
   dc->str.streamed  = 0;
   dc->cal           = (struct dex_cal){.result = -1};
   if (kept)
      LOGW("driver_forget: a credential could NOT be removed from disk; the "
           "next launch may reconnect to this sensor");
   else
      LOGI("driver_forget: key/bond dropped, ready to pair a new sensor");
   driver_leave();
}

/* Watchdog reconnect: re-issue a connect from a stalled state. The transport's
 * connect() closes any lingering GATT client first, so this recovers a stranded
 * link (e.g. an orphaned client after a crash/force-stop) without a BT toggle.
 *
 * NO P_STREAM GUARD, tempting though one is on the grounds that "actively
 * streaming" means healthy and must not be disturbed. The ONE path into this
 * function is pancra_link_watchdog -> dexble_reconnect, which fires only
 * after a link has produced no reading for 420 s: by the time we are called
 * the caller has already PROVEN the link is not healthy, so such a guard can
 * only suppress the recovery it appears to protect. A link whose disconnect
 * callback was lost sits at P_STREAM forever -- the watchdog no-ops every
 * cycle and, with the screen off, the advert path needs a scan that is down,
 * so monitoring stops silently until the app is restarted. A healthy
 * streaming link cannot reach here, because its own samples are newer than
 * the watchdog's threshold. */
void driver_kick(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->have_key || dc->g_codelen > 0) {
      dc->phase = P_IDLE;
      /* Clear the give-up counters so the watchdog can always revive a link
       * that hit MAX_FAILS (e.g. a sensor that connected but never
       * dc->str.streamed during warmup); otherwise dc->fails stays >= MAX_FAILS
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
 * 4e (dc->str.clock/dc->str.age). span is clamped to the sensor's ~24h
 * buffer.
 */
void driver_request_backfill(int link, long span)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   if (dc->phase != P_STREAM || dc->str.clock == 0) {
      driver_leave();
      return;
   }
   long end = (long)dc->str.clock -
              (long)dc->str.age; /* current reading, session-time */
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

void driver_cal_bounds(int link)
{
   /* SELECT AND LOCK AS ONE SCOPE: which context an operation runs on is
    * this file's own business, and the caller names the LINK. */
   if (!dex_link_ok(link))
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
   if (!dex_link_ok(link))
      return 0; /* nothing was written, so nothing is reported as sent */
   struct dex_ctx *dc = driver_enter(link);
   if (dc->phase != P_STREAM || dc->str.clock == 0) {
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
   uint32_t when = dc->str.clock; /* sensor clock, seconds since activation */
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
    * A bare `return` from inside the entered scope leaks the lock, which is
    * RECURSIVE and has no timeout: the owner stays set to a binder thread
    * that has already gone home, its depth never falls to zero, and every
    * other thread -- the main looper first -- spins in driver_lock for ever.
    * The phone freezes mid-reconnect and Android kills the app as not
    * responding. The likeliest way in is a stray write-ack during
    * subscription, which is ordinary BLE traffic. */
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< onWritten %.8s status=%d dc->phase=%s", uuid, status,
        dex_phase_name(dc->phase));
   if (dc->phase == P_SUB1) {
      if (strcmp(uuid, sub1[dc->sub_idx].uuid) != 0) {
         LOGI("   (ignore stray ack)");
         goto out;
      }
      if (++dc->sub_idx < NSUB1) {
         drv_subscribe(dc->link, sub1[dc->sub_idx].uuid,
                       sub1[dc->sub_idx].indicate);
      } else if (dc->have_key) {
         dc->rnd.did = 0;
         send_authrequest(dc);
      } else if (!enter_rounds(dc)) {
         goto out;
      }
   } else if (dc->phase == P_ROUNDS) {
      if (!strcmp(uuid, U_ROUND) && dc->tx_left > 0) {
         dc->tx_left--;
         if (dc->tx_left == 0) {
            if (++dc->rnd.idx < 3) {
               request_round(dc);
            } else if (!jpake_shared_key(dc->rnd.pairing, dc->shared_key)) {
               LOGI("!! sharedkey fail");
               dc->phase = P_FAIL;
            } else {
               /* THE OUTCOME, NEVER THE KEY. Logging the derived bytes --
                * loghex("SHAREDKEY(derived)", dc->shared_key, 16) -- puts the
                * sixteen bytes the whole J-PAKE exists to agree on into
                * logcat, which is readable by `adb logcat` from any machine
                * the phone is plugged into and which a bug report carries off
                * the device; anyone holding those bytes can impersonate this
                * phone to that sensor for the rest of its wear without ever
                * knowing the pairing code. Comparing the two sides' keys by
                * eye during bring-up is what tempts one into it; the test
                * suite does that comparison instead (drivertest asserts
                * drv_key_save gets the sensor's key), so nothing is lost by
                * saying only that it happened. */
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
            if (dc->cert.idx == 0)
               enter_cert(dc, 1);
            else
               (void)enter_keychal(
                   dc); /* on failure the phase is P_FAIL and
                         * this handler has nothing left to do */
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
   int room = 160 - dc->rnd.len;
   if (room > 0) {
      int take = n < room ? n : room;
      memcpy(dc->rnd.buf + dc->rnd.len, buf, (size_t)take);
      dc->rnd.len += take;
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
   if (dc->rnd.len >= 160 && !dc->rnd.done) {
      dc->rnd.done = 1;
      int ok       = 0;
      if (dc->rnd.idx == 0)
         ok = jpake_peer_round1(dc->rnd.pairing, dc->rnd.buf);
      else if (dc->rnd.idx == 1)
         ok = jpake_peer_round2(dc->rnd.pairing, dc->rnd.buf);
      else
         ok = jpake_peer_round3(dc->rnd.pairing, dc->rnd.buf);
      /* ENFORCE the peer's Schnorr ZKP (RFC-8235: g^r + X^H == V). A genuine
       * sensor's proofs always verify -- they attest knowledge of the round
       * ephemerals, independent of the pairing code -- so a failure means a
       * malformed or hostile peer whose round data must NOT be folded into
       * the shared key. Logging "INVALID(continuing)" and deriving anyway
       * leaves the later dex8 tokenHash gate to reject the wrong key; the
       * spec check belongs here, and refusing early avoids emitting our own
       * round to a peer that already failed. */
      if (!ok) {
         LOGI("!! peer round%d ZKP INVALID -- refusing to pair",
              dc->rnd.idx + 1);
         drv_status("PAIR FAILED");
         dc->phase = P_FAIL;
         return;
      }
      LOGI("   peer round%d ZKP VALID", dc->rnd.idx + 1);
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
          * the pairing code (have_key == 0 path). The authfails >= 3 gate
          * is for AMBIGUOUS failures (a drop mid-cert); a mismatch is not
          * ambiguous.
          *
          * Run it HERE, because setting P_FAIL below takes us out of
          * P_AUTH and driver_on_disconnected's `was == P_AUTH` test would
          * then never fire. */
         if (dc->have_key) {
            LOGI("!! stale key proven by tokenHash mismatch -- discarding "
                 "it now so the sensor re-pairs on the next connect");
            dc->have_key  = 0;
            dc->authfails = 0;
            if (drv_key_clear(dc->link) != 0)
               LOGI("!! the stale key is still on disk -- it will be loaded "
                    "again at the next launch");
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
      } else if (dc->rnd.did || auth != 1) {
         /* establish/refresh the bond via the certificate exchange */
         drv_status(dc->rnd.did ? "PAIRED" : "BONDING");
         enter_cert(dc, 0);
      } else {
         LOGI("   bonded reconnect -> stream");
         drv_status("AUTHENTICATED");
         enter_sub2(dc);
      }
   }
}

/* THE CERTIFICATE, which arrives in fragments and is only complete when its
 * declared length has been reached (see cert_maybe_complete). */
static void notify_cert(struct dex_ctx *dc, const char *uuid,
                        const uint8_t *buf, int n)
{
   if (!strcmp(uuid, U_AUTH) && n >= 7 && buf[0] == 0x0b) {
      /* size announce; don't reset dc->cert.rx -- the sensor streams some
       * cert chunks before this arrives (enter_cert already zeroed it). le32
       * is unsigned; clamp so a garbage/huge size can't become a negative
       * int that defeats the completion test below. */
      uint32_t sz   = le32(buf + 3);
      dc->cert.size = sz > 0x7fffffffU ? 0 : (int)sz;
      LOGI("   sensor cert %d size=%d (rx so far %d)", dc->cert.idx,
           dc->cert.size, dc->cert.rx);
      /* If every chunk already arrived before this announce, the U_ROUND
       * branch will get no further notify -- complete it here too, or P_CERT
       * stalls until the link drops. */
      cert_maybe_complete(dc);
   } else if (!strcmp(uuid, U_ROUND)) {
      dc->cert.rx += n;
      cert_maybe_complete(dc);
   }
}

/* THE KEY CHALLENGE that follows a certificate exchange. */
static void notify_keychal(struct dex_ctx *dc, const char *uuid,
                           const uint8_t *buf, int n)
{
   if (!strcmp(uuid, U_AUTH) && n >= 18 && buf[0] == 0x0c &&
       !dc->keychal.signed_once) {
      dc->keychal.signed_once = 1;
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
      enter_sub2(dc);
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
         dc->str.clock = ev.clock;
         /* mono_s(), NOT realtime_s(): this stamp is only ever used as the
          * base of an INTERVAL (see sens_project_clock), never as an instant
          * that is persisted or shown. */
         dc->str.clock_m   = mono_s();
         dc->str.state     = ev.state;
         dc->str.age       = ev.age;
         dc->str.glucose   = ev.glucose;
         dc->str.trend     = (int)ev.trend;
         /* ABSENCE BECOMES ZERO HERE, once, rather than at each reader.
          *
          * The prediction is a 10-bit field whose 0x3ff (1023) means "no
          * prediction", and no real reading exceeds Dexcom's 400 mg/dL
          * scale -- a G7 sends 0x3ff on every response for the whole warmup
          * hour. Carried on raw, that sentinel is an ordinary-looking number
          * in every structure it reaches: the two PRED rows each have to
          * know the bound to hide it, the predicted-low alarm has to be
          * checked against it by hand, and the session cache STORES it and
          * then refuses to load the file back (SESS_PRED_MAX is 1000), which
          * costs every OTHER sensor its recorded session clock too.
          *
          * Zero is what "none" already means everywhere downstream, so the
          * conversion belongs at the one place the wire value enters. */
         dc->str.predicted =
             (ev.predicted > 0 && ev.predicted <= 400) ? ev.predicted : 0;
         dc->str.seq       = ev.sequence;
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
          * These two lines are conditional on the reading being KEPT, not on
          * dexdata_egv() succeeding. Decoding only says the 19 bytes had the
          * right shape; the app still refuses the value if it is outside
          * what a sensor can report, refuses the frame if its age backdates
          * it, and drops it entirely when no registered slot claims this
          * link yet and another CGM is already live (a second sensor's first
          * readings take that path by construction). On any of those the
          * user's screen does not move and the plot gains no point -- so
          * clearing the failure streak that exists to notice a sensor going
          * bad, or writing this address into the file that decides which
          * sensor every future launch reconnects to, would adopt a sensor
          * permanently on the strength of a reading nobody kept. */
         if (drv_glucose(dc->link, ev.glucose, ev.trend, ev.age)) {
            dc->str.streamed = 1;
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
       * DEX_NOTIFY_RECORDS is the port's byte ceiling divided by the record
       * length (dexport.h), so this array holds whatever one notification can
       * possibly contain; decoding only 8 of them silently dropped the rest,
       * and because a re-request returns the same frame the loss was
       * permanent -- gap recovery quietly losing exactly the points it exists
       * to recover. */
      struct dex_record r[DEX_NOTIFY_RECORDS];
      int k = dexdata_records(buf, (size_t)n, r, DEX_NOTIFY_RECORDS);
      /* HOW MANY OF THEM THE APP KEPT, which is not the same as how many
       * arrived: the gate below drops records with no session clock and
       * records whose age is impossible, and ingestion drops more. `k > 0`
       * is NOT the test: by it, a batch of 28 records every one of which was
       * refused counts as a streaming connection. */
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
          * only set by a 0x4e, and enter_sub2 requests one first, so the
          * benign ordering is normal -- but a post-auth peer controls the
          * order, and pancra_backfill has no age bound of its own to catch
          * it (unlike pancra_glucose). */
         if (!dc->str.clock) {
            LOGI("     skipped: no session clock yet");
            continue;
         }
         long age = (long)dc->str.clock - (long)r[i].timestamp;
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
      /* AT LEAST ONE RECORD IN THE HISTORY. `k > 0` -- a nonempty batch,
       * whatever became of it -- would clear the failure streak of a sensor
       * whose every backfilled value was refused, and persist its address as
       * the one to reconnect to. */
      if (accepted > 0) {
         dc->str.streamed = 1;
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
   if (!dex_link_ok(link))
      return;
   struct dex_ctx *dc = driver_enter(link);
   LOGI("<< onNotify %.8s dc->phase=%s", uuid, dex_phase_name(dc->phase));
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
