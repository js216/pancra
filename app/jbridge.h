// SPDX-License-Identifier: GPL-3.0
// jbridge.h --- Every call this app makes into the Java adapters
// Copyright 2026 Jakob Kastelic
//
/* THE ONLY FILE THAT KNOWS THE JAVA SIDE EXISTS.
 *
 * A class global-ref, twelve cached jmethodIDs and a hand-written
 * CallStatic*Method at every use site, held in main.c, are why the rest of
 * main.c cannot be split up: any workflow that touches a system service
 * -- the notification, the settings screen, EXPORT DATA, the scan -- dragged
 * raw JNI state along with it, so it could only live where the ids lived.
 *
 * Everything here is a plain C function taking plain C types. The JNIEnv, the
 * local refs and the exception checks stop at this header.
 *
 * THREADING. Everything except jb_show_glucose runs on the MAIN thread and
 * takes the activity, whose env is only valid there. jb_show_glucose takes an
 * explicit env and context because it is also called from the foreground
 * SERVICE tick, when the activity may be gone -- and in that state the
 * notification is the only glucose display the user has.
 *
 * EXCEPTIONS ARE CLEARED, NEVER LEFT PENDING. A pending exception makes the
 * NEXT unrelated JNI call fail (and getBondedDevices really does throw, when
 * BLUETOOTH_CONNECT is revoked after pairing), so every call site here clears
 * before returning.
 *
 * LOCAL REFS ARE DELETED. These calls do not run inside a JNI method
 * invocation -- they are made from the native looper -- so there is no frame
 * pop to reclaim them: they accumulate against the ~512 ceiling for the life
 * of the process, and the VM aborts when it is exceeded. The scan-error path
 * is the one that proved it, because the self-heal calls it every 30 s while
 * scanning is failing.
 */
#ifndef JBRIDGE_H
#define JBRIDGE_H

#include <jni.h>
#include <stdint.h>

struct ANativeActivity;

/* RESOLVE AN APP CLASS THROUGH THE ACTIVITY'S OWN CLASSLOADER, by dotted name
 * ("com.jk.pancra.Ble"). Writes a LOCAL ref through `out` and returns 1; on
 * any failure `out` is set to NULL and 0 is returned.
 *
 * WHY NOT FindClass. Inside struct ANativeActivity callbacks FindClass
 * resolves against the FRAMEWORK's class loader, which cannot see the classes
 * in our own classes.dex -- so every app class has to come the long way
 * round: the activity object -> its class -> getClassLoader -> loadClass.
 *
 * WHY IT LIVES HERE rather than in main.c. It is six chained JNI calls, four
 * of which each hand back a LOCAL REFERENCE, and it feeds jb_bind: the class
 * jb_bind globalises is exactly the one this returns. Checked only at the
 * very last step, a null loader or a pending exception from step two is
 * carried into steps three through six --
 * and under CheckJNI (on for any debuggable build, and for anyone attached
 * with a debugger) making ANY JNI call with an exception pending is not an
 * error you can catch, it is `JNI DETECTED ERROR IN APPLICATION` and an
 * immediate abort. To the user that is the app vanishing during launch, with
 * a tombstone naming GetObjectClass -- a call that did nothing wrong -- as
 * the crashing frame. It also leaked the four intermediate locals on EVERY
 * call, success included; see the local-ref note above for why nothing
 * reclaims them out here.
 *
 * The returned class is the CALLER'S local ref to hold or drop; everything
 * this function acquired on the way to it is released before it returns. */
int jb_app_class(JNIEnv *env, jobject activity, const char *name, jclass *out);

/* Resolve the THREE adapter classes and every method id.
 *
 * `ble_local` is com.jk.pancra.Ble as returned by the app's own classloader --
 * handed in because main.c needs it anyway, to register natives on. The other
 * two (PancraPlatform, PancraExport) are resolved here by name through
 * `activity`, since nothing outside this file has a use for them. Global refs
 * are taken for all three.
 *
 * ALL OR NOTHING: a partial bridge is a feature that silently does nothing,
 * so a failure leaves every id and every class NULL and returns 0. Safe to
 * call again -- a relaunch re-enters onCreate in the same process, and
 * re-binding would leak the previous global refs. */
int jb_bind(JNIEnv *env, jobject activity, jclass ble_local);

/* The bound Ble class, for the callers that must register natives on it
 * themselves (dexble_register, and the advert callback). NULL until bound.
 *
 * NATIVES ARE WHY Ble IS STILL A CLASS OF ITS OWN: RegisterNatives binds them
 * to one class, so the GATT callbacks and the sync entry points are declared
 * there whatever else moves out. */
jclass jb_class(void);

/* The bound PancraPlatform class, for the one caller outside this file that
 * needs it: dexble.c starts the foreground service, which is a platform
 * policy and not a BLE operation. NULL until bound. */
jclass jb_platform_class(void);

/* --- scan lifecycle --- */

/* Start the receive-only scan. Returns 1 on success; on failure `err` gets
 * Java's message (or "SCAN THREW") so the caller can show it.
 *
 * `gen` is the GENERATION this attempt owns, allocated by the caller (scan.c)
 * and quoted back by Ble's onScanFailed when the platform refuses the scan
 * asynchronously. It is a parameter rather than something Java counts for
 * itself so that the side which latches "a scan is running" is the side that
 * names the scan it latched -- a Java-side counter would drift from the native
 * one the first time a start failed synchronously, and a failure whose
 * generation does not match is DISCARDED, so drift reads as "that scan is
 * already gone" and restores the very latch this exists to prevent. */
int jb_scan(JNIEnv *env, jobject clazz, int gen, char *err, int cap);
/* Stop it. Returns 1 only if JAVA CONFIRMED the scan is down: clearing the
 * caller's "scanning" flag while Ble still holds a registered callback it can
 * no longer cancel lets the self-heal register a SECOND scan client. */
int jb_stop(JNIEnv *env);

/* --- system ops, from the settings screen (main thread) --- */
/*
 * EVERY ONE OF THESE ANSWERS WHETHER JAVA ACTUALLY DID IT.
 *
 * NOT `void`, and not the Java value returned directly. Both hide the same
 * failure: a JNI call that throws still RETURNS, with a pending exception and
 * a zeroed result. `jb_battery_ok` would report "not exempt" for a throw
 * exactly as for a real denial, `jb_standby_bucket` would return 0
 * ("active") for one, and a void one reports nothing at all -- while the
 * pending exception waits to abort the VM at the next unrelated JNI call on
 * that thread.
 *
 * The commands return 1 when Java completed the call. The queries return 1
 * when Java ANSWERED and write the answer through the out-parameter, which
 * is left untouched otherwise -- so a cache of these (menu.c's g_sys_*)
 * keeps its previous value rather than showing a fault as a fact.
 */
int jb_set_orientation(struct ANativeActivity *a, int mode);
/* Java builds the combined CSV and opens the system share sheet. Rows older
 * than `cutoff` (epoch, 0 = keep all) are left out. */
int jb_export_data(struct ANativeActivity *a, long cutoff, int glu, int dev,
                   int ins, int wt);
int jb_perm_granted(struct ANativeActivity *a, const char *perm, int *granted);
int jb_request_perm(struct ANativeActivity *a, const char *perm);
int jb_open_settings(struct ANativeActivity *a); /* the app details page */
int jb_battery_ok(struct ANativeActivity *a, int *ok); /* background exempt */
int jb_request_battery(struct ANativeActivity *a);
/* -1 = unknown, written only when Java answered. */
int jb_standby_bucket(struct ANativeActivity *a, int *bucket);

/* THE HARDWARE STEP COUNTER. `on` registers or tears down the listener; the
 * count is its newest total since boot, or -1 when nothing has arrived (no
 * such sensor, no permission, or no movement yet). A Context rather than an
 * activity: the sampler runs on the service tick too. */
void jb_steps_listen(JNIEnv *e, jobject ctx, int on);
long jb_steps_count(JNIEnv *e);
int jb_bg_restricted(struct ANativeActivity *a, int *restricted);

/* DID THE LAST JNI CALL ON `e` COMPLETE? 1 clean, 0 it threw -- and then the
 * exception has been described to the log and CLEARED, because leaving one
 * pending aborts the VM at the next JNI call on this thread.
 *
 * Exported because app/scan.c makes its own JNI calls (checkSelfPermission,
 * requestPermissions) against the activity class rather than Ble, and was
 * checking none of them: a revoked BLUETOOTH_CONNECT makes those throw. */
int jb_checked(JNIEnv *e, const char *what);

/* Resolve a bonded sensor's address by NAME PREFIX (e.g. "DX01"), so a
 * reconnect never has to guess from adverts, whose local name is usually
 * absent. Writes into `mac` and returns 1 when one was found. */
int jb_bonded_sensor(JNIEnv *env, jobject clazz, const char *prefix, char *mac,
                     int cap);

/* Push the live value and the plot bitmap into the ongoing notification.
 * `px` is w*h pixels in Bitmap ARGB_8888 order (0xAARRGGBB). */
void jb_show_glucose(JNIEnv *env, jobject ctx, const char *title,
                     const char *text, const char *val, const uint32_t *px,
                     int w, int h, int lockscr);

#endif
