// SPDX-License-Identifier: GPL-3.0
// dexdriver.h --- Dexcom protocol driver (API + transport hooks)
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver: the Dexcom pairing/reconnect state machine, with NO
 * Android/JNI dependency. The transport (BLE writes/subscribes) and UI/storage
 * are provided by the host via the drv_* hooks below -- implemented by dexble.c
 * on the phone and by a mock harness in the offline tests. */
#ifndef DEXDRIVER_H
#define DEXDRIVER_H
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
 * link_for_slot returns -1 and the caller says so instead of connecting on
 * another device's link. Ble.java's MAX_LINKS must match -- the Makefile's
 * crosscheck target fails the build if it does not. */
#define LINK_MAX 8

/* Dexcom GATT characteristic UUIDs. */
#define U_CTRL  "f8083534-849e-531c-c594-30f1f86a4ea5"
#define U_AUTH  "f8083535-849e-531c-c594-30f1f86a4ea5"
#define U_DATA  "f8083536-849e-531c-c594-30f1f86a4ea5"
#define U_ROUND "f8083538-849e-531c-c594-30f1f86a4ea5"

/* ---- provided BY the transport layer (dexble.c / test harness) ---- */
void drv_connect(int link, const char *mac);
void drv_subscribe(int link, const char *uuid, int indicate);
void drv_write(int link, const char *uuid, const uint8_t *data, int n,
               int no_resp);
void drv_status(const char *s);

/* ONE LIVE READING, AND WHETHER THE APP KEPT IT.
 *
 * Returns 1 when the reading entered the authoritative history -- past the
 * value and age gate in ingest.c, past per-link source attribution, and
 * inserted (not deduplicated away) by store_record -- and 0 when it did not.
 *
 * The return value is the whole reason this is not `void`. The driver used to
 * set `streamed` and persist the sensor's address the moment a 0x4e DECODED,
 * which is a statement about the wire, not about the app: a frame carrying a
 * 5,000 mg/dL value, or an age of 65535 seconds, or one arriving on a link no
 * registered slot claims yet, is refused downstream and never becomes a
 * reading. To the person holding the phone that connection produced NOTHING
 * -- the number on screen did not move and the plot gained no point -- while
 * the driver counted it as a success, cleared the failure streak that exists
 * to notice a sensor going bad, and wrote the address into the file that
 * decides which sensor future launches reconnect to. A sensor that can
 * decode but whose every value is rejected could therefore be adopted
 * permanently on the strength of nothing.
 *
 * The hook is declared warn_unused_result BECAUSE OF THAT HISTORY: a caller
 * that drops the answer is back to trusting the decode, and the compiler is
 * the only thing that reliably notices. */
#if defined(__GNUC__) || defined(__clang__)
#define DRV_MUST_USE __attribute__((warn_unused_result))
#else
#define DRV_MUST_USE
#endif
DRV_MUST_USE int drv_glucose(int link, int mg_dl, int trend,
                             int age_s); /* live reading; 1 = accepted */
/* Outcome of a calibration write we sent: 0 accepted, >0 rejected by the
 * sensor. Lets the shell clear or surface its durably-queued calibration.
 *
 * THE TOKEN OF THE WRITE THAT WAS ANSWERED TRAVELS WITH THE ANSWER, and this
 * is the whole of the fix for it. The driver used to carry one BOOLEAN --
 * "a calibration we sent is awaiting a reply" -- and the shell resolved
 * whatever calibration was queued at the moment the reply landed. Those are
 * not the same calibration whenever the user has changed their mind:
 *
 *   the user takes a fingerstick, types 100, confirms; the driver writes 0x34
 *   for 100. Before the sensor answers -- the reply is one connection interval
 *   away at best, and a whole reconnect away at worst -- they realise they
 *   misread the meter, cancel, and type 180. The sensor's answer to the 100
 *   arrives, ACCEPTED, and the shell marks the 180 accepted. The row says
 *   "LAST CAL 180 APPLIED". The sensor holds 100, and will go on reporting
 *   against 100 until the next calibration. The one number the user is
 *   entitled to believe -- what the sensor was actually told -- is wrong on
 *   the screen, and a CGM calibrated to a value nobody chose misreports
 *   glucose for as long as the session lasts.
 *
 * So a transmitted calibration is identified by the three things that make it
 * the one it is: the SENSOR it was for, the VALUE written, and the queue
 * GENERATION it came from. The generation is a counter the calibration module
 * bumps on every queue (it is NOT a clock -- see the deadline block in
 * calib.c; a clock would make the identity of a write depend on an NTP step),
 * so re-queueing the same value for the same sensor is a different write and
 * is told apart from the first. All three come back here, and the shell
 * resolves ONLY on an exact match with what is queued RIGHT NOW. Anything
 * else is discarded, loudly. */
void drv_cal_result(int link, int result, int sensor_id, int mg_dl,
                    unsigned gen);
/* One recovered older reading. 1 when it entered the authoritative history,
 * exactly as for drv_glucose above -- and for the same reason: a backfill
 * batch of 28 records every one of which is refused is not a batch that
 * proves the sensor is streaming. */
DRV_MUST_USE int drv_backfill(int link, int mg_dl, int trend,
                              int age_s);    /* 1 = accepted */
int drv_key_load(int link, uint8_t key[16]); /* 1 if a key was loaded */
int drv_key_save(int link, const uint8_t key[16]);
void drv_key_clear(int link); /* delete the stored key (force re-pair) */
int drv_mac_load(int link, char *mac,
                 int n); /* bonded sensor's MAC; 1 if one was saved */
int drv_mac_save(int link, const char *mac); /* the sensor we bonded to */
void drv_mac_clear(int link);                /* forget it (re-pair) */

/* ---- driver API (called by the transport layer) ---- */
/* SELECTION IS NOT PART OF THIS INTERFACE ANY MORE.
 *
 * An ambient "select this link" call used to be here, and the comment above it
 * said the transport calls it before dispatching any callback "so the rest of
 * the API needs no link parameter". That is what ambient state always
 * promises: fewer parameters, in exchange for every reader having to know
 * which call last ran. The cost was real and is documented all over this
 * tree -- callbacks that selected and never restored, so a dozen unrelated
 * places had to "select explicitly" first; restores that put back an assumed
 * LINK_CGM rather than what was actually selected; and a stall watchdog that
 * kicked whichever link a binder thread last touched.
 *
 * What replaced it:
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
/* (The lock itself is NOT here. It is in dexdriver.c, reached only through the
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

/* (driver_get_cal and driver_get_session are gone from this interface. They
 * read whichever context was last SELECTED, which is a question no caller
 * outside the driver can answer -- "the link a binder thread happened to
 * touch" is not an identity. The two below take the link by name and take the
 * lock themselves.) */

/* This LINK's calibration state, read as one snapshot.
 *
 * Reading another link's used to be "select it, read, select back": three
 * steps with a file-wide variable in the middle, on a shell that walks all
 * LINK_MAX of them every frame. */
void driver_cal_of(int link, struct dex_cal *out);

/* Snapshot of what we know about a connected sensor and its session. */
struct dex_session {
   char mac[24];
   int bonded;               /* authenticated on the fast saved-key path */
   int paired;               /* we hold a shared key */
   int have_reading;         /* a 4e EGV has been decoded this run */
   uint32_t session_seconds; /* LIVE sensor session time: the clock from the
                                last 4e projected forward by wall time, so
                                countdowns tick per second between responses */
   int state; /* raw session-state byte from the last 4e (0 = none seen) */
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
 * These used to be two arrays in meter.c and a bitmask in dexble.c -- the
 * same fact in two places -- each guarded by reaching for the DRIVER's lock
 * from another module. They live here now, with the lock, and every caller
 * gets an operation instead. */

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
 * Dexcom protocol, or the OneTouch one. The transport used to make that
 * decision itself -- read the routing bit, then branch -- and had to hold the
 * DRIVER's lock across both halves, because the shell arms and releases links
 * from the main thread and a decision split from its dispatch sends a meter's
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
 * therefore serialised with the driver's own state, and the calibration
 * module used to achieve that by taking the driver's lock itself.
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
 * newly paired meter kept whatever index the previously synced one left, and
 * its oldest records were skipped -- then persisted as skipped. */
void driver_meter_seed_index(int index);

/* Claim a FREE link for `mac`, leaving `reserve` links for CGMs that have not
 * claimed one yet. Returns the link, or -1. The search and the claim are ONE
 * critical section: a link that reads free and is claimed by a binder thread
 * before the caller uses it would be handed to two devices at once. */
int driver_link_claim(const char *mac, int reserve);

/* Bind this link to `mac` DURABLY: write the address the driver reads back on
 * the next launch, and set it as the reconnect target, as one step. Done
 * separately -- with a lock the caller took by hand -- a GATT callback
 * landing between the two saw the new address with the old target still set.
 */
void driver_bind_mac(int link, const char *mac);

/* This LINK's session. Same reasoning as driver_cal_of. */
void driver_session_of(int link, struct dex_session *out);

/* ---- THE PER-LINK RETRY DEADLINES, CLAIMED RATHER THAN READ --------------
 *
 * Two throttles that used to be file statics in reading.c, stamped from
 * realtime_s(). They are here for two reasons, and the second is the one that
 * matters:
 *
 * 1. They are facts about a LINK, and the driver owns links. A throttle that
 *    lived beside the reading path had to be indexed by link anyway, and one
 *    of them was a single global before that -- so one sensor's DIS request
 *    blocked every other sensor's for a minute.
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
 * PER LINK, like everything else the driver holds: the process-global stamp
 * it replaces meant one sensor's connect refreshed the other's window. */
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
int driver_free_cgm_link(int rank);

#endif
