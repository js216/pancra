// SPDX-License-Identifier: GPL-3.0
// pancra.h --- main.c <-> dexble.c interface declarations
// Copyright 2026 Jakob Kastelic

/* Interface between main.c (UI + JNI registration) and dexble.c (BLE
 * transport). Declaring both directions here gives every translation unit a
 * prototype, so the definitions are checked against it (-Wmissing-prototypes).
 */
#ifndef PANCRA_H
#define PANCRA_H

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

/* The NDK entry point (defined in main.c). Declared here so it has a visible
 * prototype and is understood to be externally linked (the Android runtime
 * resolves it by name), not a candidate for internal linkage. */
struct ANativeActivity;
void ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved,
                              size_t saved_size);

/* UI callbacks -- implemented in main.c, called from the BLE/driver layer. */
void pancra_status(const char *s);
void pancra_glucose(int mg_dl, int trend, int age_s);
void pancra_backfill(int mg_dl, int trend, int age_s);
void pancra_rssi(int rssi);
void pancra_meter_rssi(int rssi);
/* Result of a calibration write the sensor answered: 0 accepted, >0 rejected.
 * Clears or surfaces the durably-queued calibration. */
void pancra_cal_result(int result);
void pancra_devinfo(int link, const char *uuid, const char *val);

/* BLE transport -- implemented in dexble.c, called from main.c. */
void dexble_init(const char *data_dir);
int dexble_register(JNIEnv *env, jclass ble, jobject ctx);
void dexble_set_alarm(JNIEnv *env, jclass alarm_cls);
/* Pair/connect a Dexcom sensor on `link`. */
void dexble_pair(int link, const char *mac, const char *code);
/* Evaluate + actuate the alarm. Any thread; must NOT hold driver_lock. */
void pancra_alarm_check(void);
/* Re-connect any CGM link that has gone quiet. Safe on any thread, and safe
 * with no activity alive -- the service heartbeat drives it so a stranded link
 * is repaired even after the activity is destroyed. */
void pancra_link_watchdog(void);
/* One step of the REMOTE sync (cursor read, or the next batch). Called from
 * the SERVICE tick as well as the activity's timer: the activity's looper
 * dies with the activity, and a phone with the app backgrounded must still
 * deliver its readings -- that is precisely the window in which points used
 * to pile up unsent. */
void pancra_remote_sync(void);
/* Time out a wedged meter sync. Safe on any thread and with no activity
 * alive; the service heartbeat drives it so a sync interrupted by the
 * activity's teardown cannot latch g_meter_busy forever. */
void meter_sync_watchdog(void);
/* Keep the sensor registry in step with the live session. Safe on any thread
 * and with no activity alive; the service heartbeat drives it so a sensor that
 * bonds in the background is still registered. */
void pancra_reconcile_tick(void);
JNIEnv *dexble_env(void); /* a JNIEnv valid on the CALLING thread */
jobject dexble_ctx(void); /* app Context global ref; outlives the activity */
/* Refresh the lock-screen notification. Safe on any thread and with no
 * activity alive -- the service heartbeat drives it so the notification keeps
 * tracking readings after the activity is destroyed. */
void pancra_notify_refresh(void);
/* A REMOTE push was acknowledged by the server (HTTP 2xx). Called on
 * Ble.remotePush's worker thread; just timestamps the last success. */
void pancra_remote_ok(void);
void dexble_reconnect(int link); /* stall watchdog: force a fresh connect */
void dexble_request_devinfo(void);
void dexble_request_devinfo_link(int link);
/* Drop one link. The meter driver uses this the moment it has what it needs,
 * so the meter can power itself down instead of being held awake. */
void dexble_link_close(int link);
/* Link-addressed GATT operations (the drv_* hooks wrap these for LINK_CGM). */
void dexble_subscribe(int link, const char *uuid, int indicate);
void dexble_write(int link, const char *uuid, const uint8_t *d, int n,
                  int no_resp);
/* Connect to a OneTouch meter on LINK_METER (independent of the CGM link). */
void dexble_meter_connect(const char *mac);
int dexble_alarm(int kind, int sound, int vibrate); /* 0 low, 1 high, 2 stale */
void dexble_beep(void); /* one short NEW DATAPOINT beep */
/* One NEW DATAPOINT chirp, pitch-bent by `st10` tenths of a semitone
 * (chirp_semitone10); 0 is the beep's own pitch. */
void dexble_chirp(int st10);
int dexble_alarm_silence(void);

#endif
