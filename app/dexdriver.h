// SPDX-License-Identifier: GPL-3.0
// dexdriver.h --- Dexcom protocol driver (API + transport hooks)
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver: the Dexcom pairing/reconnect state machine, with NO
 * Android/JNI dependency. The transport (BLE writes/subscribes) and UI/storage
 * are provided by the host via the drv_* hooks below -- implemented by dexble.c
 * on the phone and by a mock harness in the offline tests. */
#ifndef DEXDRIVER_H
#define DEXDRIVER_H

#include "compiler.h"  /* PANCRA_MUST_USE: the annotation, portably */
#include "scanlogic.h" /* struct live_stamp + the four liveness deadlines */
#include <stdint.h>

/* Concurrent GATT links. Each sensor gets its own link id, its own operation
 * queue in Ble.java, and its own driver context -- so a slow or stalled sensor
 * cannot hold up another one's advertising window, and two sensors sharing a
 * GATT layout (Stelo and G7 do) never trample each other's state. Defined here
 * rather than in the transport's port header so the protocol layer stays
 * free of JNI. */
#define LINK_CGM   0 /* first Dexcom sensor */
#define LINK_METER 1 /* OneTouch meter */
#define LINK_CGM2  2 /* a second Dexcom sensor, streaming concurrently */
/* 8, raised from 5 when every meter gained its own standing connect.
 *
 * A link is a GATT connection, an operation queue and a driver context, and
 * every registered device now needs one AT THE SAME TIME: CGMs stream
 * continuously, and a meter must hold a pending connect permanently because
 * it is reachable only for the second or two it is switched on. At 5 a user
 * with two sensors and three meters was already at the ceiling.
 *
 * Not raised to MAX_SLOTS (10): the practical limit is the Bluetooth
 * controller's simultaneous-connection count, which is around 7-8 on typical
 * phones, and asking for more than the controller can hold makes connects
 * fail rather than queue. 8 covers every realistic set-up; past that,
 * link_for_slot returns -1 and the caller says so rather than connecting on
 * another device's link. Ble.java's MAX_LINKS must match -- the Makefile's
 * crosscheck target fails the build if it does not. */
#define LINK_MAX 8

/* ---- driver API (called by the transport layer) ---- */
/* SELECTION IS NOT PART OF THIS INTERFACE.
 *
 * An ambient "select this link" call -- the transport calls it before
 * dispatching any callback, "so the rest of the API needs no link parameter"
 * -- is what ambient state always promises: fewer parameters, in exchange for
 * every reader having to know which call last ran. The cost is concrete:
 * callbacks that select and never restore, so a dozen unrelated places must
 * "select explicitly" first; restores that put back an assumed LINK_CGM
 * rather than what was actually selected; and a stall watchdog that kicks
 * whichever link a binder thread last touched.
 *
 * What this interface has instead:
 *   driver_enter/driver_leave  -- validate the link, take the lock, and hand
 *                                 back THAT link's context;
 *   driver_session_of/_cal_of  -- read ONE named link, moving nothing.
 *
 * There is no ambient selector left to reach for. It and the
 * file-wide pointer it moved are both gone: the context travels as an
 * argument, and a link this driver does not have yields no context and no
 * action at all. */
/* Serialise ALL driver state.
 *
 * GATT callbacks run inline on binder threads (connectGatt is called with no
 * Handler), so several links dispatch genuinely concurrently, while the main
 * thread also reads sessions at 1 Hz. A context is no longer chosen ambiently,
 * so a swap can no longer land mid-operation -- but the STATE inside one
 * context is still read and written by several threads, so the work on it
 * must be one atomic step: otherwise a concurrent callback lands between a
 * bounds check and its memcpy, or between deriving a key and saving it.
 * The lock is recursive because the transport can complete a write
 * synchronously and re-enter the driver from inside a driver call.
 *
 * This costs almost nothing in radio terms: the real per-link concurrency
 * lives in Ble.java's per-link operation queues, and the sections guarded here
 * are short and CPU-only. */
/* (The lock itself is NOT here. It is in dexlink.c, reached only through the
 * files that run inside GATT callbacks -- see that header. Everything else
 * uses the operations below, each of which takes the lock itself.) */

/* (driver_enter/driver_leave are gone from this interface. Every operation
 * below names the LINK it acts on and scopes the selection itself, so there
 * is no ambient context for a caller to set, forget to restore, or race
 * another thread over -- which is what "the driver was pointed at the wrong
 * sensor" meant every time it happened.) */

/* 1 when the CALLING thread is holding the driver lock right now.
 *
 * FOR CHECKING BALANCE, and it exists because an unbalanced pair shipped. A
 * conversion to driver_enter left a redundant `driver_lock()` above it at one
 * site: the lock is recursive, so the depth went 2 and came back to 1, the
 * owner was never cleared, and the 1 Hz timer thread held the driver lock
 * FOREVER. Every GATT callback then spun on it. The Android side of the
 * connection still succeeded -- connect, MTU, service discovery are all Java
 * -- so the phone looked connected and simply never produced another reading
 * again. Three CGM cycles were missed before it was noticed.
 *
 * Nothing could see it: the counts balance textually, the tests do not run the
 * timer, and the UI stayed responsive because the holder was the thread that
 * wanted it. So the invariant is asserted instead -- a tick must not end
 * holding this. */
int driver_held(void);

void driver_init(void); /* jpake_init + load saved key */
void driver_start(int link, const char *mac,
                  const char *code); /* set target + code, connect */
void driver_forget(int link);        /* drop key/bond, pair anew */
void driver_lock_mac(
    int link, const char *mac); /* set reconnect target, do not connect */
void driver_kick(int link);     /* force reconnect if stalled */
void driver_on_connected(int link);
void driver_on_disconnected(int link, int status);
void driver_on_written(int link, const char *uuid, int status);

/* Dial any link whose post-disconnect cooldown has expired. The watchdog
 * sweep calls it; nothing else should. A disconnect no longer redials on the
 * spot -- see dex_retry_delay -- so this is what makes the next attempt
 * happen at all. */
void driver_retry_tick(void);
void driver_on_notify(int link, const char *uuid, const uint8_t *buf, int n);
void driver_request_backfill(
    int link, long span_seconds); /* recover a gap ending at now */

/* ---- calibration (opcodes 0x32 / 0x34) ----
 *
 * NEVER call driver_calibrate() automatically. Calibration is the only command
 * in this app that changes how a sensor reports, it cannot be undone for the
 * running session, and on a G7 it perturbs a device somebody may be relying on
 * medically. Both entry points are strictly user-initiated.
 *
 * Framing note: the G7/Stelo generation takes a 7-byte 0x34 with NO trailing
 * CRC and answers by reusing opcode 0x34 -- unlike the G5/G6 9-byte CRC form
 * that xDrip and CGMBLEKit send. That difference is why the one public report
 * of "Stelo ignores calibration" is inconclusive. */

/* Ask the sensor what it permits. Read-only: mutates no sensor state. */
void driver_cal_bounds(int link);
/* Submit a calibration in mg/dL. User-initiated only. Returns 1 if the write
 * was issued, 0 if refused (not streaming / not permitted / out of range) --
 * the shell keeps the value queued and retries on a 0 rather than dropping it.
 *
 * `sensor_id` and `gen` are the caller's OWN identity for this calibration and
 * mean nothing to the driver: it stores them beside the value it put on the
 * wire and hands all three back with the sensor's reply (see drv_cal_result),
 * so the caller can tell an answer to THIS write from an answer to a write it
 * has since replaced. Only ONE write per link is outstanding at a time, and a
 * second driver_calibrate REPLACES the record -- there is one 0x34 in flight
 * because the module's own throttle allows one a minute, and if a stale reply
 * to the first ever did arrive it would be discarded by the caller's match
 * rather than misapplied. */
int driver_calibrate(int link, int mg_dl, int sensor_id, unsigned gen);

/* Last known answer to 0x32, plus the last 0x34 result. */
struct dex_cal {
   long asked;    /* realtime_s() when bounds were last requested */
   int have;      /* a 0x32 reply has been parsed */
   int permitted; /* byte[14]: firmware allows calibration */
   int status;    /* byte[13]: 1 = factory, 2 = in progress, ... */
   int last_bg;   /* byte[7..8]: last accepted calibration value */
   long last_cal; /* byte[9..12]: sensor-clock time of that calibration */
   int result;    /* the last 0x34 outcome; -1 = none seen this process */
};

/* THE TWO READERS BELOW NAME THE LINK, and take the lock themselves. A
 * reader that instead served whichever context was last selected would be
 * answering a question no caller outside the driver can answer: "the link a
 * binder thread happened to touch" is not an identity. */

/* This LINK's calibration state, read as one snapshot. 1 if `*out` holds it.
 *
 * The alternative is "select it, read, select back": three steps with a
 * file-wide variable in the middle, on a shell that walks all LINK_MAX of
 * them every frame.
 *
 * TOTAL, AND FALLIBLE OUT LOUD. These were void, and on a link
 * this driver does not have they returned having touched NOTHING -- leaving
 * the caller's struct exactly as its stack found it, while the caller went on
 * to read it as a snapshot. Uninitialised is the worst possible answer here:
 * a garbage `have_reading` and a garbage glucose are indistinguishable from a
 * sensor that just reported, and this is the path the screen and the alarm
 * read. So: `*out` is ZEROED before anything can fail, which makes the
 * refusal safe by construction, and the answer is RETURNED so a caller can
 * tell "no such link" from "a link with nothing in it" -- the two are
 * different, and only one of them is a bug in the caller.
 *
 * warn_unused_result, because a caller that ignores this is back where it
 * started: reading a struct without knowing whether anything filled it. */
PANCRA_MUST_USE int driver_cal_of(int link, struct dex_cal *out);

/* Snapshot of what we know about a connected sensor and its session. */
struct dex_session {
   char mac[24];
   int bonded;               /* authenticated on the fast saved-key path */
   int paired;               /* we hold a shared key */
   int have_reading;         /* a 4e EGV has been decoded this run */
   uint32_t session_seconds; /* LIVE sensor session time: the clock from the
                                last 4e projected forward by wall time, so
                                countdowns tick per second between responses */
   /* THE SENSOR'S OWN STATE BYTE, and it stays a byte on purpose.
    * It comes off the wire: SENSOR_STATE_* names the three values this app
    * acts on, and a sensor is free to send others -- a firmware revision
    * this build has never seen is not a value to invent an enumerator for,
    * and an enum here would claim the domain is closed when it is not. 0 is
    * "no 0x4e decoded yet", which is why every reader tests for a NAMED
    * state rather than for non-zero. */
   int state;
   int glucose, trend, age, predicted, sequence;
   /* WHEN THE LAST ACCEPTED READING ARRIVED, ON BOTH CLOCKS, AND BOTH ARE
    * LOAD-BEARING. See the liveness block in scanlogic.h.
    *
    *   .wall is the INSTANT of receipt -- the same civil second that went into
    *   readings.csv on that reading's row, so a log line or a screen can say
    *   when it was without inventing a number that means nothing off this
    *   phone.
    *   .mono is what the silence watchdog AGES. It is the only one of the two
    *   a wall-clock correction cannot move, and the watchdog is the ONLY
    *   reconnect mechanism while the screen is off (the advert path needs a
    *   scan, whose lifecycle follows on_resume/on_pause).
    *
    * Deleting either as "the same fact twice" breaks the other's job: a
    * monotonic value cannot be written down or compared against another
    * device's data, and a wall value cannot survive an NTP step. Both stay.
    *
    * SET ONLY WHEN THE READING WAS KEPT, not when the frame decoded -- the
    * same rule as `streamed` and remember_sensor above it. A sensor whose
    * every value is refused downstream is not a sensor that is delivering,
    * and the watchdog exists to reconnect exactly that. */
   struct live_stamp last_rx;
};

/* THE WHOLE DRIVER, AS ONE INSTANT. Every link's session, plus the
 * calibration state of one named link (-1 for none), taken under a single
 * hold of the lock.
 *
 * A frame that read them one at a time -- which is what the caller did, with
 * a hand-taken lock around the loop -- can show one sensor's session age on
 * another's row when a binder thread rebinds a link between two of the
 * reads. */
void driver_snapshot(struct dex_session sess[LINK_MAX], int cal_link,
                     struct dex_cal *cal);

/* ---- PER-LINK ROLE AND ARMING, which the driver owns because its own
 * callbacks read them.
 *
 * Split across two arrays in meter.c and a bitmask in dexble.c they are the
 * same fact in two places, each guarded by reaching for the DRIVER's lock
 * from another module. They live here, with the lock, and every caller gets
 * an operation instead. */

/* Is this link carrying a meter rather than a sensor? Set when a meter is
 * armed on it, cleared when the link is given back. */
void driver_link_set_meter(int link, int on);
int driver_link_is_meter(int link);

/* ARM a link for a meter's address ("" to un-arm), and ask about it. Arming
 * is what says "a connect is outstanding for this meter", which is how a
 * second attempt is kept from cancelling the first during the one second the
 * meter is awake. */
void driver_link_arm(int link, const char *mac);
int driver_link_armed(int link);
void driver_link_armed_mac(int link, char *out, int cap);
/* Which link is armed for this address, or -1. */
int driver_link_of_mac(const char *mac);

/* ---- ROUTING ONE CALLBACK, decision and dispatch together ------------
 *
 * Every GATT callback belongs to exactly one of two state machines: the
 * Dexcom protocol, or the OneTouch one. A transport that makes that decision
 * itself -- read the routing bit, then branch -- has to hold the DRIVER's
 * lock across both halves, because the shell arms and releases links from the
 * main thread and a decision split from its dispatch sends a meter's
 * write-ack into the sensor state machine.
 *
 * The decision and the dispatch are one operation here, inside the lock that
 * owns the bit. The meter's half is registered once at startup, so the driver
 * routes to it without knowing what a OneTouch is. */
struct driver_meter_ops {
   /* 1 = this link may have the single meter protocol state; 0 = close it. */
   int (*connected)(int link);
   /* 1 = this link owned the exchange (reset the protocol); 0 = ignore. */
   int (*disconnected)(int link);
   void (*on_connected)(void);
   void (*on_disconnected)(void);
   void (*on_notify)(const unsigned char *data, int n);
   /* A WRITE OR A SUBSCRIBE COMPLETED, and `status` is the transport's own
    * (0 = the bytes went out; anything else = they did not). DISCARDED for a
    * meter link -- driver_route_written calling nothing -- a refused write
    * leaves the meter protocol waiting for an
    * answer to a request that was never sent, with the link held open and the
    * meter held awake. */
   void (*on_written)(const char *uuid, int status);
};

/* What the TRANSPORT must do once the routing above has returned and the
 * driver's lock is back. Each of these reaches Java, and this lock is a spin
 * lock the main looper also takes, so none of them may happen inside it. */
enum driver_after {
   DRV_AFTER_NONE = 0,
   DRV_AFTER_CLOSE,      /* the shell refused this link the meter state */
   DRV_AFTER_RSSI,       /* sample the link's signal (a sensor connected) */
   DRV_AFTER_RSSI_METER, /* ...the same, for a meter's brief window */
};

void driver_set_meter_ops(const struct driver_meter_ops *ops);

/* The transport calls these; the driver decides whose event it is. */
enum driver_after driver_route_connected(int link);
void driver_route_disconnected(int link, int status);
void driver_route_notify(int link, const char *uuid, const unsigned char *d,
                         int n);
void driver_route_written(int link, const char *uuid, int status);

/* Route `link` to the meter protocol AND issue its connect, as one step. The
 * transport passes its own connect function: a connect on a link whose
 * routing has not landed yet delivers the meter's first notification to the
 * Dexcom state machine. */
void driver_meter_connect(int link, const char *mac,
                          void (*connect)(int link, const char *mac));

/* ---- THE CALIBRATION QUEUE, run under the driver's own lock ----------
 *
 * The queue (what the user typed, whether it has been sent, how it ended) is
 * read and written from BOTH the reading path -- a binder thread already
 * inside a driver callback -- and the menus on the main thread. It is
 * therefore serialised with the driver's own state -- which the calibration
 * module must not do by taking the driver's lock itself.
 *
 * The same shape as the meter routing above: the calibration module supplies
 * WHAT to do, the driver decides WHEN it is safe to do it. Nothing outside
 * the driver touches a lock. */
struct driver_cal_ops {
   void (*attempt)(int link, int sensor_id); /* send it if the sensor allows */
   /* These two PERSIST, so they answer whether the change reached the disk:
    * a calibration the user confirmed and the app forgot at the next launch
    * is the failure that answer exists to prevent. 0 = committed. */
   int (*queue)(int sensor_id, int mg_dl);
   int (*cancel)(void);
   void (*tick)(void);
   int (*queued_for)(int sensor_id);
};

void driver_set_cal_ops(const struct driver_cal_ops *ops);

void driver_cal_attempt(int link, int sensor_id);
int driver_cal_queue(int sensor_id, int mg_dl);
int driver_cal_cancel(void);
void driver_cal_tick(void);
int driver_cal_queued_for(int sensor_id);

/* Seed the meter protocol's record index for the link being armed. The
 * protocol's own state is touched by the driver's callbacks, so setting it
 * from the main thread is the driver's step, not the caller's: without it a
 * newly paired meter keeps whatever index the last synced one left, and its
 * oldest records are skipped -- then persisted as skipped. */
void driver_meter_seed_index(int index);

/* Claim a FREE link for `mac`, leaving `reserve` links for CGMs that have not
 * claimed one yet. Returns the link, or -1. The search and the claim are ONE
 * critical section: a link that reads free and is claimed by a binder thread
 * before the caller uses it would be handed to two devices at once. */
int driver_link_claim(const char *mac, int reserve);

/* WAS THE RECOVERED ADDRESS MADE PERMANENT? The two states are not "worked"
 * and "did not": BIND_NOT_SAVED means the reconnect target is UNCHANGED --
 * whatever it was before is still what this link will dial -- and the address
 * is held for driver_bind_retry. Nothing is half-applied either way. */
enum bind_mac { BIND_PUBLISHED, BIND_NOT_SAVED };

/* Bind this link to `mac` DURABLY: write the address the driver reads back on
 * the next launch, and only then set it as the reconnect target. Done
 * separately -- with a lock the caller took by hand -- a GATT callback
 * landing between the two sees the new address against the old target.
 *
 * The ORDER is the contract and so is the answer. Publishing an address the
 * file system refused is a sensor that works until the process dies and is
 * unknown afterwards, while the one path that discovers it (the bond-list
 * walk at startup) runs once per launch -- so a dropped failure here does not
 * degrade, it loses the sensor. Ignore the result and that is what happens,
 * hence PANCRA_MUST_USE. */
PANCRA_MUST_USE enum bind_mac driver_bind_mac(int link, const char *mac);

/* Try again for every link that owes a save, at most once per 30 s. Cheap
 * and a no-op when nothing is pending, so the link watchdog can just call it.
 * A retry that succeeds publishes the address exactly as an immediate save
 * would. */
void driver_bind_retry(void);


/* This LINK's session. Same reasoning as driver_cal_of, and the same
 * contract: `*out` is zeroed first, 1 means it was filled from the link. */
PANCRA_MUST_USE int driver_session_of(int link, struct dex_session *out);

/* ---- THE PER-LINK RETRY DEADLINES, CLAIMED RATHER THAN READ --------------
 *
 * Two throttles, rather than file statics in reading.c stamped from
 * realtime_s(). They are here for two reasons, and the second is the one that
 * matters:
 *
 * 1. They are facts about a LINK, and the driver owns links. A throttle
 *    beside the reading path has to be indexed by link anyway, and a single
 *    global one lets one sensor's DIS request block every other sensor's for
 *    a minute.
 *
 * 2. CLAIM, not test-then-set. pancra_link_watchdog runs on BOTH the
 *    activity's 1 Hz timer and the foreground service's 20 s tick. With a
 *    plain read-modify-write both could see the interval elapsed and both
 *    call dexble_reconnect on the same link: the second Ble.connect bumps the
 *    link generation, so the first closes the client it had just created,
 *    tearing down the very reconnect the watchdog exists to start and leaving
 *    the link down for another cycle. A single atomic exchange makes the
 *    claim single-winner, and putting the exchange behind a named operation
 *    is what stops the next caller reintroducing the split.
 *
 * Each returns 1 to EXACTLY ONE caller per interval, and 0 to everyone else
 * -- including when the monotonic clock cannot be read at all, which is the
 * refusal defined in scanlogic.h. No lock is taken: these are lock-free
 * atomics, so they may be called from a binder thread already inside the
 * driver, from the main looper, and from the service tick without any
 * ordering obligation at all. */

/* 1 = you won the reconnect throttle for this link; go and reconnect it. */
int driver_kick_claim(int link);
/* 1 = you may re-request this link's device-information strings now. */
int driver_dis_claim(int link);

/* ---- THE LIVE CONNECTION'S SIGNAL STRENGTH, AND WHETHER IT IS THIS ONE ----
 *
 * The freshness window that decides whether a measured RSSI belongs to the
 * connection a reading arrived on. It was `realtime_s() - stamp < 120` in
 * reading.c, which a backward wall step turns into "always fresh" -- so a
 * signal number measured before the step was stamped onto stored readings as
 * if it were live -- and a forward jump turns into "never fresh", which drops
 * the signal column out of the log for as long as the jump lasts.
 *
 * PER LINK, like everything else the driver holds: one process-global stamp
 * would let one sensor's connect refresh the other's window. */
void driver_rssi_note(int link); /* an RSSI was just measured on this link */
int driver_rssi_fresh(int link); /* ...recently enough to be this connection */

/* ---- WHICH LINK HOLDS WHAT ------------------------------------------
 *
 * Answered from a driver SNAPSHOT, so a caller that asks twice gets two
 * answers about the same instant; the plain forms take their own. These were
 * in reconcile.h, which made every module that needed to find a link -- the
 * meter runtime among them -- depend on the reconcile workflow, which calls
 * back into those same modules. A link is the driver's. */

/* The link bound to this device ADDRESS, or -1. */
int driver_link_of_identity_in(const struct dex_session *sess,
                               const char *identity);
int driver_link_of_identity(const char *identity);
/* A free CGM link at this rank among the free ones, or -1. */
int driver_free_cgm_link_in(const struct dex_session *sess, int rank);

#endif
