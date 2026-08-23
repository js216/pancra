// SPDX-License-Identifier: GPL-3.0
// dexpriv.h --- the driver's own state, shared by its two units and nobody else
// Copyright 2026 Jakob Kastelic

/* WHY THIS HEADER EXISTS.
 *
 * The Dexcom driver is two jobs, and in one 1960-line file they read as one:
 * WHICH
 * SENSOR a callback belongs to (dexlink.c -- the link registry, the roles and
 * arming, the retry claims, the routing of every transport callback and the
 * calibration queue) and WHAT TO SAY TO IT (dexproto.c -- the pairing, auth,
 * certificate and stream state machine). They have different reasons to
 * change: the first follows the app's model of the devices a person owns, the
 * second follows a protocol that is fixed by somebody else's firmware.
 *
 * They share exactly what is below, and this header is the whole of it. It is
 * NOT a public header: app/dexdriver.h is what the rest of the app sees, and
 * nothing outside these two files may include this one -- the whole point of
 * the split is that the driver's state is not lent out. `make -f test/Makefile
 * inclusions` holds that line.
 *
 * THE LOCK IS NOT HERE. It lives in dexlink.c, private to it, and everything
 * this header declares is either taken under it (driver_enter) or explicitly
 * runs with it already held. That is deliberate: it was a public lock once,
 * then a lock in a private header six modules included, and both amounted to
 * other files reasoning about when the driver's state is safe to touch. One
 * redundant lock() above a call that took it internally is what once held it
 * for the life of the process. Callers name an OPERATION; only these two
 * files name the state.
 */
#ifndef DEXPRIV_H
#define DEXPRIV_H

#include "dexdriver.h" /* LINK_MAX, struct dex_session, struct dex_cal */
#include "scanlogic.h" /* struct live_stamp: the two clocks of a reading */
#include <stdint.h>

struct jpake; /* by pointer only: the pairing state is the protocol's */

/* ---- THE PHASES, NAMED ------------------------------------------------
 *
 * An anonymous `enum { P_IDLE, ... }` in the protocol unit beside an
 * `int phase` in the context is two things pretending to be one: the field
 * holds any int at all, and a helper taking a phase takes an int.
 * Naming the type is what lets a signature say `enum dex_phase` and mean it.
 *
 * THE ORDER IS THE PROTOCOL'S OWN, though nothing depends on it -- every test
 * is an equality against one phase. It is written in order because that is
 * how a connection proceeds:
 *
 *   IDLE -> SUB1 -> [ROUNDS ->] AUTH -> [CERT -> KEYCHAL ->] SUB2 -> STREAM
 *
 * with the bracketed pairs skipped on a bonded reconnect (auth == 1), and
 * FAIL reachable from all of them. FAIL is terminal for the connection: the
 * link is dropped, and the next connect starts again at IDLE. */
enum dex_phase {
   P_IDLE = 0, /* nothing in flight; the context is at rest */
   P_SUB1,     /* subscribing to the auth and round characteristics */
   P_ROUNDS,   /* J-PAKE: three rounds, with an applicator code */
   P_AUTH,     /* AuthRequest / AuthChallenge / AuthStatus */
   P_CERT,     /* certificate exchange, both directions */
   P_KEYCHAL,  /* the key challenge we sign exactly once */
   P_SUB2,     /* subscribing to the control and data characteristics */
   P_STREAM,   /* the connection is producing readings */
   P_FAIL      /* terminal: this connection is over */
};


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
   enum dex_phase phase;
   char g_mac[24];
   uint8_t g_code[8];
   int g_codelen;
   int mac_saved;          /* persisted this sensor's MAC this run (once) */
   uint8_t shared_key[16]; /* OUTLIVES the exchange: it is written to a file
                            * and is what a bonded reconnect authenticates
                            * with, so it belongs to no phase */
   int have_key;

   /* WRITES STILL IN FLIGHT, and WHICH SUBSCRIPTION IS BEING CONFIRMED.
    *
    * Deliberately NOT in a phase group: SUB1 and SUB2 both walk a
    * subscription list with sub_idx, and ROUNDS, CERT and KEYCHAL each count
    * their chunk acknowledgements down with tx_left. A field three phases use
    * that groups under one of them is a lie about who owns it. */
   int tx_left, sub_idx;

   /* ---- P_ROUNDS: the J-PAKE exchange, and nothing else ----------------
    *
    * All of it is transient, and enter_rounds() clears the whole struct: a
    * second pairing attempt on one context cannot inherit a half-filled
    * buffer or a round index from the first. That is not hypothetical -- see
    * the reassembly note in notify_rounds for what a stuck `len` did. */
   struct {
      struct jpake *pairing;
      int idx;          /* which round (0..2) is in flight */
      uint8_t buf[160]; /* reassembly: a round arrives in 20-byte chunks */
      int len;
      int done; /* this round's buffer already handled (see on_notify) */
      int did;  /* rounds ran on this connection: a FRESH pairing rather than
                 * a bonded reconnect. OUTLIVES P_ROUNDS on purpose -- the
                 * decision it feeds is made later, in the auth handler. */
   } rnd;

   /* ---- P_AUTH: what we sent, and what came back ------------------------
    *
    * A tokenHash on THIS connection was verified against our shared key.
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
   uint8_t token[8]; /* the challenge we sent: the reply is checked against a
                      * hash of it, so it must outlive the write */
   int g_bonded;     /* last AuthStatus was the fast (auth==1) path */

   /* ---- P_STREAM: what the sensor last said, and when -------------------
    *
    * The one phase group that is NOT wholly transient. Everything here is
    * read out by driver_get_session (dexlink.c) for the screen and the
    * history, and it must survive the end of the connection that produced it:
    * a sensor that disconnects still has a last reading, and the age of that
    * reading is exactly what the screen counts up. So there is no enter_*
    * handler clearing this group -- unlike every other one -- and
    * driver_forget is what wipes it, because forgetting the sensor is the
    * only event that makes the last thing it said untrue. */
   struct {
      /* THIS CONNECTION PUT A READING IN THE HISTORY.
       *
       * Not "a frame decoded". The two are different whenever the app refuses
       * what the wire delivered -- an impossible value, a frame whose age
       * backdates it, a link no registered slot claims yet -- and this flag
       * decides whether the connection cleared the failure streak and whether
       * the sensor's address was written down as the one to come back to. See
       * the drv_glucose call in notify_stream. */
      int streamed;
      uint32_t clock; /* sensor session-time from the latest 4e */
      /* WHEN last_clock WAS RECEIVED, ON THE MONOTONIC CLOCK. The session time
       * is projected forward from here between responses, because the
       * countdowns built on it (warmup, session end) must tick per second while
       * the sensor answers only every ~5 minutes -- the official app's do.
       *
       * MONOTONIC. A wall-clock stamp taken with realtime_s() moves the
       * projection by however far a correction goes -- a phone coming back
       * from being off, or finding a network -- in a subtraction whose
       * negative result is cast
       * to uint32_t and added to an unsigned clock. See sens_project_clock in
       * senslogic.h for exactly what the two directions did to the screen.
       * `make clockcheck` names this field so it cannot quietly go back to the
       * wall clock. */
      long clock_m;
      /* WHEN THE LAST KEPT READING ARRIVED, ON BOTH CLOCKS. The field comment
       * on struct dex_session::last_rx in dexdriver.h is the whole story: .wall
       * is the instant that reading's row carries, .mono is what the silence
       * watchdog ages, and neither can do the other's job. `make clockcheck`
       * names last_rx.mono so it cannot quietly go back to the wall clock. */
      struct live_stamp last_rx;
      uint8_t state; /* raw session-state byte from the latest 4e --
                              logged and surfaced; during warmup the sensor
                              answers with no glucose but a running clock */
      uint16_t age;  /* age of that current reading, seconds */
      int glucose, trend, predicted, seq;
   } str;

   /* ---- P_CERT: one certificate exchange -------------------------------
    *
    * Every field is scoped to the exchange and cleared by enter_cert(). `rx`
    * against `size` is the completion test, and `sent` makes our own half
    * happen exactly once however many times the peer announces its size. */
   struct {
      int idx;  /* which certificate of the chain is being received */
      int size; /* the length the peer announced */
      int rx;   /* how much of it has arrived */
      int sent; /* our cert chunks sent this exchange */
   } cert;

   /* ---- P_KEYCHAL: one signature, once ---------------------------------
    *
    * Our signature for this key challenge has been sent.
    *
    * P_ROUNDS has round_done and P_CERT has cert_sent for exactly this;
    * P_KEYCHAL had no equivalent, so a repeated 0c frame re-signed and called
    * send_chunks again, resetting tx_left to 4 with 4 writes still in flight --
    * the acks then stop matching and 0d goes out over a partially delivered
    * signature. It also made the embedded collector key an unbounded
    * chosen-message signing oracle: one signature per connection is the
    * protocol; unlimited is not. */
   struct {
      int signed_once;
   } keychal;

   /* THE CALIBRATION WE ACTUALLY SENT, and which one it is.
    *
    * `pending` answers only "a 0x34 we sent is awaiting its reply", and it is
    * needed for that: the reply reuses the request opcode, and the handler
    * re-reads the bounds (a 0x32 write) so the UI reflects the new state.
    * Without the flag ANY 0x34 from the peer triggers that write, and firmware
    * echoing it -- or a post-auth peer choosing to -- sustains a 0x32/0x34
    * ping-pong at connection-interval rate, draining both batteries and
    * flooding the log for as long as the link stays up.
    *
    * The other three fields are the identity of the write, recorded at the
    * moment it goes on the wire and handed back with the answer. A boolean
    * cannot say WHICH calibration was answered, leaving the shell to resolve
    * whatever was queued when the reply landed -- see drv_cal_result in
    * dexdriver.h for what that does to a user who changes their mind between
    * the write and the reply. The driver does not interpret `sensor_id` or
    * `gen`; they are the
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
    * separately -- and cannot be told wrongly. A file-wide "current link"
    * beside this one would be a second, mutable answer to the same question.
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

/* IS THIS A LINK THIS DRIVER HAS? Asked BEFORE the lock by every public entry
 * point of both units.
 *
 * dex_ RATHER THAN driver_, here and for dex_phase_name: the public API of
 * the driver is driver_* (app/dexdriver.h), and these are the private
 * interface the two halves share. The prefix is the difference between "what
 * anyone may ask the driver" and "what the driver's own files call each
 * other" -- and one of these names, `link_ok`, was also a static in
 * app/metersess.c, which is exactly the confusion an unprefixed private name
 * invites. */
int dex_link_ok(int link);

/* The phase as a short upper-case word, for log lines. */
const char *dex_phase_name(enum dex_phase p);

/* Take the driver's lock and name the context. `link` MUST already have
 * passed dex_link_ok(); there is no in-range fallback. Pair with
 * driver_leave(). The lock is RECURSIVE, so these nest -- and they do: routing
 * enters, and what it routes to enters again. */
struct dex_ctx *driver_enter(int link);
void driver_leave(void);

/* Run `fn` over EVERY link's context under ONE hold of the lock.
 *
 * This is how the protocol unit reaches all the contexts at once --
 * driver_init loads every link's saved key and address -- without the lock
 * leaving dexlink.c. ONE hold, not one per link: a pass that re-takes the
 * lock between links describes a different instant at each of them, and the
 * whole reason driver_init walks every link (rather than the selected one) is
 * that a second sensor otherwise begins each process unpaired. */
void driver_each_ctx(void (*fn)(struct dex_ctx *dc));

/* Read a context out into the public shapes. Both run with the lock HELD --
 * driver_session_of and driver_snapshot are their locked callers. */
void driver_get_session(const struct dex_ctx *dc, struct dex_session *out);
void driver_get_cal(const struct dex_ctx *dc, struct dex_cal *out);

#endif
