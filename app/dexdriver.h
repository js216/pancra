// SPDX-License-Identifier: GPL-3.0
// dexdriver.h --- Dexcom protocol driver (API + transport hooks)
// Copyright 2026 Jakob Kastelic

/* pancra protocol driver: the Dexcom pairing/reconnect state machine, with NO
 * Android/JNI dependency. The transport (BLE writes/subscribes) and UI/storage
 * are provided by the host via the drv_* hooks below -- implemented by dexble.c
 * on the phone and by a mock harness in the offline tests. */
#ifndef DEXDRIVER_H
#define DEXDRIVER_H
#include <stdint.h>

/* Concurrent GATT links. Each sensor gets its own link id, its own operation
 * queue in Ble.java, and its own driver context -- so a slow or stalled sensor
 * cannot hold up another one's advertising window, and two sensors sharing a
 * GATT layout (Stelo and G7 do) never trample each other's state. Defined here
 * rather than in pancra.h so the protocol layer stays free of JNI. */
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
void drv_connect(const char *mac);
void drv_subscribe(const char *uuid, int indicate);
void drv_write(const char *uuid, const uint8_t *data, int n, int no_resp);
void drv_status(const char *s);
void drv_glucose(int mg_dl, int trend, int age_s); /* current reading */
/* Outcome of a calibration write we sent: 0 accepted, >0 rejected by the
 * sensor. Lets the shell clear or surface its durably-queued calibration. */
void drv_cal_result(int result);
void drv_backfill(int mg_dl, int trend,
                  int age_s);      /* recovered older reading */
int drv_key_load(uint8_t key[16]); /* returns 1 if a key was loaded */
void drv_key_save(const uint8_t key[16]);
void drv_key_clear(void); /* delete the stored key (force re-pair) */
int drv_mac_load(char *mac,
                 int n); /* bonded sensor's MAC; 1 if one was saved */
void drv_mac_save(const char *mac); /* remember the sensor we bonded to */
void drv_mac_clear(void);           /* forget it (re-pair) */

/* ---- driver API (called by the transport layer) ---- */
/* Point the driver at one link's state. The transport calls this before
 * dispatching any callback or action, exactly as it already scopes its JNIEnv,
 * so the rest of the API needs no link parameter. */
void driver_select(int link);
/* Serialise ALL driver state.
 *
 * GATT callbacks run inline on binder threads (connectGatt is called with no
 * Handler), so several links dispatch genuinely concurrently, while the main
 * thread also reads sessions at 1 Hz. The driver's per-link context is chosen
 * by an ambient selector, so selection and the work that follows it must be
 * one atomic step -- otherwise a context swap can land between a bounds check
 * and its memcpy, or between deriving a key and choosing the file to save it
 * in. The lock is recursive because the transport can complete a write
 * synchronously and re-enter the driver from inside a driver call.
 *
 * This costs almost nothing in radio terms: the real per-link concurrency
 * lives in Ble.java's per-link operation queues, and the sections guarded here
 * are short and CPU-only. */
void driver_lock(void);
void driver_unlock(void);
/* The link currently selected -- the transport uses it to address its GATT
 * operations and per-sensor key storage. */
int driver_link(void);

void driver_init(void); /* jpake_init + load saved key */
void driver_start(const char *mac,
                  const char *code); /* set target + code, connect */
void driver_forget(void);            /* drop key/bond, pair anew */
void driver_lock_mac(
    const char *mac);   /* set reconnect target, do not connect */
void driver_kick(void); /* force reconnect if stalled */
void driver_on_connected(void);
void driver_on_disconnected(int status);
void driver_on_written(const char *uuid, int status);
void driver_on_notify(const char *uuid, const uint8_t *buf, int n);
void driver_request_backfill(
    long span_seconds); /* recover a gap ending at now */

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
void driver_cal_bounds(void);
/* Submit a calibration in mg/dL. User-initiated only. Returns 1 if the write
 * was issued, 0 if refused (not streaming / not permitted / out of range) --
 * the shell keeps the value queued and retries on a 0 rather than dropping it.
 */
int driver_calibrate(int mg_dl);

/* Last known answer to 0x32, plus the last 0x34 result. */
struct dex_cal {
   long asked;    /* realtime_s() when bounds were last requested */
   int have;      /* a 0x32 reply has been parsed */
   int permitted; /* byte[14]: firmware allows calibration */
   int status;    /* byte[13]: 1 = factory, 2 = in progress, ... */
   int last_bg;   /* byte[7..8]: last accepted calibration value */
   long last_cal; /* byte[9..12]: sensor-clock time of that calibration */
   int result;    /* last 0x34 status byte; -1 = none yet */
};

void driver_get_cal(struct dex_cal *out);

/* Snapshot of what we know about the connected sensor and session. */
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
};

void driver_get_session(struct dex_session *out);

#endif
