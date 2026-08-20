// SPDX-License-Identifier: GPL-3.0
// scan.c --- The BLE scan (see scan.h)
// Copyright 2026 Jakob Kastelic

#include "scan.h"
#include "clock.h" /* mono_s: the failure back-off is a DEADLINE, not a date */
#include "dexdriver.h"
#include "dexlibc.h"
#include "jbridge.h"
#include "log.h"
#include "ndk.h"
#include "scanlogic.h"
#include "shell.h"
#include "status.h"
#include <jni.h>
#include <jni_md.h>
#include <stdatomic.h> /* the scan-failure callback is a BINDER thread */

/* The permissions a scan actually needs, by platform level. Asking for the
 * modern pair on an old device (or FINE_LOCATION on a new one) is refused
 * outright, and a refused request looks exactly like a denied one. */
static const char *perm_modern[] = {"android.permission.BLUETOOTH_SCAN",
                                    "android.permission.BLUETOOTH_CONNECT"};
static const char *perm_legacy[] = {"android.permission.ACCESS_FINE_LOCATION"};

/* Set when the permission request goes out, taken by the next resume: the
 * battery-exemption prompt is a SECOND system dialog, and stacking it on the
 * permission one puts them in a race the user loses. */
static int g_want_battery;

int scan_take_battery_wanted(void)
{
   int w          = g_want_battery;
   g_want_battery = 0;
   return w;
}

/* Is a scan running? Written only through the calls below, and believed only
 * when JAVA CONFIRMED the change -- see stop_scan and jni_scan_failed.
 *
 * ATOMIC because the third writer is not on the main thread: the platform
 * delivers a scan failure on a Bluetooth binder thread (the same kind of
 * thread that delivers adverts -- see jni_on_advert in pairing.c), and the
 * main looper reads this flag every tick to decide whether to heal the scan. */
static atomic_int g_scanning;

/* THE GENERATION OF THE SCAN JAVA IS CURRENTLY HOLDING, allocated here.
 *
 * Native allocates it rather than Java so there is ONE source of truth for
 * "which scan is live": the side that latches g_scanning is the side that
 * hands out the identity it is latching, so the two cannot drift apart when a
 * start fails synchronously. Ble.scan() records it beside the ScanCallback it
 * installs and quotes it back in the failure callback. Starts at 0, so the
 * first scan owns generation 1 and 0 always means "no scan of ours".
 *
 * Never wraps in practice: it counts successful STARTS, throttled to one per
 * SCAN_RETRY_S, so 2^31 of them is longer than any phone lives. */
static atomic_int g_scan_gen;

/* Hold starts off until this monotonic second, after a failure the platform
 * reported asynchronously. See scan_fail_retry_at: a failing scan fails again
 * on every attempt, and startScan is itself what trips Android's block. */
static _Atomic long g_scan_retry_after;

/* The ScanCallback error code of that failure, so a user who taps something
 * expecting the scan back gets told WHY it is still down instead of nothing
 * happening. */
static atomic_int g_scan_last_err;

/* Hold the scan down until this time, so a pairing/bonding connect gets a quiet
 * radio. Zero means "no hold". See the self-heal in on_timer. */
/* MONOTONIC. This is "keep the radio quiet for the next 20 seconds", an
 * interval, and a wall-clock correction used to either lift it instantly or
 * hold the scan down for an hour. */
static long g_scan_hold_until;

/* A stop_scan that Java could not confirm; the 1 Hz timer retries it. Without
 * this, g_scanning latches at 1 with no live scan behind it. Atomic for the
 * same reason as g_scanning: the failure callback clears it from a binder
 * thread. */
static atomic_int g_scan_stop_pending;

int has_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env       = a->env;
   const char **want = a->sdkVersion >= 31 ? perm_modern : perm_legacy;
   jsize n           = a->sdkVersion >= 31 ? 2 : 1;

   /* EVERY ONE OF THESE CALLS IS CHECKED, and none of them was.
    *
    * GetObjectClass and GetMethodID can fail and leave a pending exception,
    * after which CallIntMethod with a NULL jmethodID is undefined behaviour
    * -- and a checkSelfPermission that throws (which it does when the process
    * is being torn down) returned a value that was compared against 0 as
    * though it were an answer. Either way the exception stayed pending on
    * this thread and aborted the VM at the next unrelated JNI call.
    *
    * A permission this function cannot CONFIRM is reported as absent: the
    * caller shows "NO PERMISSION" and does not scan, which is the safe
    * direction -- scanning without the permission is what throws. */
   jclass act = (*env)->GetObjectClass(env, a->clazz);
   if (!jb_checked(env, "GetObjectClass") || !act)
      return 0;
   jmethodID check = (*env)->GetMethodID(env, act, "checkSelfPermission",
                                         "(Ljava/lang/String;)I");
   if (!jb_checked(env, "GetMethodID(checkSelfPermission)") || !check) {
      (*env)->DeleteLocalRef(env, act);
      return 0;
   }
   int ok = 1;
   for (jsize i = 0; i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      if (!s || !jb_checked(env, "NewStringUTF(perm)")) {
         ok = 0;
         break;
      }
      jint r = (*env)->CallIntMethod(env, a->clazz, check, s);
      if (!jb_checked(env, "checkSelfPermission") || r != 0)
         ok = 0;
      (*env)->DeleteLocalRef(env, s);
   }
   /* See start_scan: local refs made on the native looper thread are never
    * reclaimed by a frame pop. tz_offset_at was already fixed for this; these
    * were missed, and the 30 s scan self-heal calls this one repeatedly. */
   (*env)->DeleteLocalRef(env, act);
   return ok;
}

/* Ask for every runtime permission the app wants, in one dialog sequence: the
 * BLE pair needed to reach the sensor plus notifications so alarms can alert.
 * The battery-optimisation exemption isn't a runtime permission (it's a
 * settings intent) -- g_want_battery makes on_resume pop it right afterwards.
 * The result callback never reaches native code; grant state is re-checked on
 * resume. */
void request_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env = a->env;
   const char *want[4];
   jsize n = 0;
   if (a->sdkVersion >= 31) {
      want[n++] = "android.permission.BLUETOOTH_SCAN";
      want[n++] = "android.permission.BLUETOOTH_CONNECT";
   } else {
      want[n++] = "android.permission.ACCESS_FINE_LOCATION";
   }
   if (a->sdkVersion >= 33)
      want[n++] = "android.permission.POST_NOTIFICATIONS";

   /* CHECKED THROUGHOUT, for the same reasons as has_ble_permissions -- and
    * with one more: this builds an object ARRAY, and NewObjectArray or
    * SetObjectArrayElement failing leaves a NULL or partial array that
    * requestPermissions then receives. */
   jclass act = (*env)->GetObjectClass(env, a->clazz);
   if (!jb_checked(env, "GetObjectClass") || !act)
      return;
   jmethodID req    = (*env)->GetMethodID(env, act, "requestPermissions",
                                          "([Ljava/lang/String;I)V");
   int ok           = jb_checked(env, "GetMethodID(requestPermissions)") && req;
   jclass strcls    = ok ? (*env)->FindClass(env, "java/lang/String") : NULL;
   ok               = ok && jb_checked(env, "FindClass(String)") && strcls;
   jobjectArray arr = ok ? (*env)->NewObjectArray(env, n, strcls, NULL) : NULL;
   ok               = ok && jb_checked(env, "NewObjectArray") && arr;
   for (jsize i = 0; ok && i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      if (!s || !jb_checked(env, "NewStringUTF(perm)")) {
         ok = 0;
         break;
      }
      (*env)->SetObjectArrayElement(env, arr, i, s);
      ok = jb_checked(env, "SetObjectArrayElement");
      (*env)->DeleteLocalRef(env, s);
   }
   if (ok) {
      (*env)->CallVoidMethod(env, a->clazz, req, arr, (jint)1);
      ok = jb_checked(env, "requestPermissions");
   }
   if (arr)
      (*env)->DeleteLocalRef(env, arr);
   if (strcls)
      (*env)->DeleteLocalRef(env, strcls);
   (*env)->DeleteLocalRef(env, act);
   /* ONLY IF THE DIALOG WAS ACTUALLY REQUESTED. g_want_battery makes the next
    * resume pop the battery-exemption prompt on the assumption that a
    * permission dialog is on screen and about to close; setting it when no
    * dialog was ever shown pops that prompt out of nowhere. */
   if (ok)
      g_want_battery = 1;
}

/* --- Java -> C: the scan the platform never actually started --------------
 *
 * BINDER THREAD, an unbounded time after the start that provoked it, and it
 * carries the GENERATION of the scan it belongs to. Everything this decides is
 * in scanlogic.c (scan_fail_applies), which also holds the account of what the
 * missing version of this function cost the user. */
static void jni_scan_failed(JNIEnv *e, jclass c, jint gen, jint err)
{
   (void)e;
   (void)c;
   struct scan_fail f = {.failed_gen = (int)gen,
                         .cur_gen    = atomic_load(&g_scan_gen),
                         .scanning   = atomic_load(&g_scanning)};
   if (!scan_fail_applies(&f)) {
      /* Logged, because "a failure arrived and was ignored" and "no failure
       * arrived" look identical afterwards, and only one of them means the
       * generation matching is doing its job. */
      LOGI("scan failure %d for gen %d ignored (live gen %d, scanning %d)",
           (int)err, (int)gen, f.cur_gen, f.scanning);
      return;
   }
   /* THE BACK-OFF FIRST, THEN THE FLAG, and the order is load-bearing: the 1 Hz
    * heal becomes eligible the instant g_scanning reads 0, so clearing that
    * first would let the next tick call startScan with no back-off recorded --
    * which for failure 6 (SCANNING_TOO_FREQUENTLY) is precisely the call that
    * extends Android's block. */
   atomic_store(&g_scan_last_err, (int)err);
   atomic_store(&g_scan_retry_after, scan_fail_retry_at(mono_s()));
   /* A stop that Java could not confirm is no longer outstanding:
    * Ble.scanFailed releases its ScanCallback before calling in here, so there
    * is nothing left for the retry to cancel. Leaving it set would make the
    * tick call Ble.stop() forever against a handle Java has already dropped. */
   atomic_store(&g_scan_stop_pending, 0);
   atomic_store(&g_scanning, 0);
   LOGW("scan gen %d FAILED asynchronously (err %d); scan state reset, retry "
        "in %d s",
        (int)gen, (int)err, SCAN_RETRY_S);
   /* SAY SO ON SCREEN. The whole defect this repairs was silent, and the
    * status row is the only place a refusal becomes visible. Set from this
    * binder thread the same way the driver's own GATT callbacks set it
    * (drv_status in dexble.c): update_screen only marks the frame dirty off
    * the main thread. Deliberately NOT gated on the CGM being paired or
    * streaming the way start_scan's "SCANNING" is -- that gate exists so a
    * routine background state cannot mask the driver's own status, and a scan
    * that has stopped working is not a routine state: it is why the sensor
    * will not come back after the next dropout. */
   set_status(scan_fail_text((int)err));
}

/* ONE NATIVE, REGISTERED BY THE WORKFLOW THAT IMPLEMENTS IT, exactly as
 * pairing.c registers onAdvert: a scan's own failure belongs to the scan, not
 * to the GATT transport that happens to hold the Ble class. A JNIEnv and the
 * bound class are all it needs, and start_scan has both.
 *
 * BOUND BEFORE THE FIRST startScan, which is the ordering that matters: the
 * platform may refuse a scan before Ble.scan() has even returned, so the method
 * must be callable by the time the request goes out. Binding at the first start
 * guarantees that without the scan workflow needing to be initialised from
 * somewhere else in the shell -- and RegisterNatives may be called more than
 * once on a class, which is how pairing.c and dexble.c already share it.
 *
 * A BIND THAT FAILS IS NOT FATAL. RegisterNatives can only fail here if
 * classes.dex has no onScanFailed(II)V -- a libpancra/classes.dex skew -- and
 * in that state Ble.scanFailed's own catch swallows the UnsatisfiedLinkError.
 * Refusing to scan at all would turn a build-skew warning into a phone that
 * never reads a sensor, so this logs loudly and scans anyway: reverting to the
 * old latch is bad, and never scanning at all is worse. */
static void bind_scan_native(JNIEnv *env)
{
   static int bound;
   jclass ble = jb_class();
   if (bound || !env || !ble)
      return;
   /* char[] rather than literals so the char* JNINativeMethod fields need no
    * const cast (-Wcast-qual, -Wwrite-strings). */
   static char nm[]                       = "onScanFailed";
   static char sig[]                      = "(II)V";
   static const JNINativeMethod methods[] = {
       {nm, sig, (void *)jni_scan_failed},
   };
   if ((*env)->RegisterNatives(env, ble, methods, 1) != 0) {
      /* A failed registration leaves a pending NoSuchMethodError, and the next
       * JNI call made with one pending is illegal -- the very next thing this
       * function's caller does is call Ble.scan(). */
      if ((*env)->ExceptionCheck(env)) {
         (*env)->ExceptionDescribe(env);
         (*env)->ExceptionClear(env);
      }
      LOGW("onScanFailed could not be bound: an asynchronous scan failure will "
           "not reach native code (libpancra/classes.dex skew?)");
      return;
   }
   bound = 1;
}

void start_scan(struct ANativeActivity *a)
{
   if (!a || !a->env || atomic_load(&g_scanning) || !jb_class())
      return;
   /* THE FAILURE BACK-OFF, honoured HERE rather than in the 1 Hz heal.
    *
    * The heal's own throttle (scan_should_start) is stamped by main.c when it
    * DECIDES to heal, and its "never tried" sentinel deliberately does not
    * throttle the first attempt -- so a scan that fails asynchronously would
    * get one immediate un-throttled restart, and every explicit restart path
    * (on_resume, the DEVICES refresh) another. Refusing the start itself keeps
    * a persistently failing scan to one startScan per SCAN_RETRY_S from every
    * caller at once, which is what keeps Android's 5-in-30-seconds block from
    * turning a recoverable failure into a sticky one.
    *
    * The user is told again rather than left with a dead tap: the reason the
    * scan is down is more useful than silence. */
   if (!scan_start_allowed(mono_s(), atomic_load(&g_scan_retry_after))) {
      int err = atomic_load(&g_scan_last_err);
      LOGI("start_scan: still backing off after scan failure %d", err);
      set_status(scan_fail_text(err));
      return;
   }
   if (!has_ble_permissions(a)) {
      set_status("NO PERMISSION");
      return;
   }
   bind_scan_native(a->env);
   /* THE GENERATION, AND THE LATCH, BOTH BEFORE THE CALL.
    *
    * Java is handed the generation it must quote back, and g_scanning is set
    * before startScan is asked for -- not after it returns. The platform
    * delivers onScanFailed on a binder thread and is entitled to do so while
    * Ble.scan() is still returning; a failure that arrived in that window
    * would see scanning == 0, conclude "already reset", and return -- and the
    * main thread would then latch the flag for a scan that had already died.
    * That is the original bug in a one-microsecond window, which is the kind
    * that ships. */
   int gen = atomic_fetch_add(&g_scan_gen, 1) + 1;
   atomic_store(&g_scanning, 1);
   char err[64] = {0};
   if (!jb_scan(a->env, a->clazz, gen, err, sizeof err)) {
      /* Rolled back unconditionally: only the main thread starts scans, so
       * this generation is still the current one, and a binder-thread failure
       * that beat us to the reset stored the same 0. */
      atomic_store(&g_scanning, 0);
      LOGI("scan: %s", err);
      set_status(err);
      return;
   }
   LOGI("scanning (receive-only), gen %d", gen);
   /* only surface SCANNING before we're operational; once paired/streaming
    * the driver's own status (WAITING/CONNECTED/...) is the meaningful one
    * and the background scan must not mask it */
   struct dex_session s;
   /* SELECT explicitly, and take the lock in the SAME step. The ambient
    * selection is left wherever the last GATT callback put it (they select and
    * never restore), so reading without selecting reports whichever link a
    * binder thread happened to touch -- "the lock is held" is not the same as
    * "the right context is chosen". driver_enter is both. */
   driver_session_of(LINK_CGM, &s);
   if (!s.paired && !s.have_reading)
      set_status("SCANNING");
}

void stop_scan(struct ANativeActivity *a)
{
   /* Guard the activity AND its env. Callers pass g_act, which on_destroy
    * clears, and the pending-stop retry runs from the 1 Hz timer -- so a
    * teardown racing that retry would dereference a null activity here. */
   if (!a || !a->env || !atomic_load(&g_scanning))
      return;
   int stopped = jb_stop(a->env);

   /* Only believe the scan is down if Java confirmed it. Clearing g_scanning
    * unconditionally, while Ble still held a registered callback it could no
    * longer cancel, let the on_timer self-heal register a SECOND scan client
    * -- duplicate adverts, and eventually Android's scan-throttle block.
    * Leaving g_scanning set means the self-heal will not stack another one,
    * and the next stop_scan retries the cancel. */
   if (!stopped) {
      /* Mark it for RETRY. Leaving g_scanning set is right -- it stops the
       * self-heal stacking a second scan client on top of one Ble still
       * holds
       * -- but on its own it was a permanent latch: g_scanning is cleared in
       * exactly one place, reachable only from a LATER successful stop_scan,
       * and the self-heal that exists so the user need not background the
       * app is itself gated on !g_scanning. So the one mechanism that could
       * recover was structurally excluded, and nothing anywhere notices that
       * adverts have stopped arriving. The 1 Hz timer retries until Java
       * confirms. */
      atomic_store(&g_scan_stop_pending, 1);
      LOGI("stop_scan: Ble could not confirm the scan stopped; will retry");
      return;
   }
   atomic_store(&g_scan_stop_pending, 0);
   atomic_store(&g_scanning, 0);
   /* don't surface "PAUSED": stopping the background scan is an internal
    * detail (it happens on pause and on every orientation flip); the
    * driver's own connection status stays the meaningful thing to show */
}

/* Tear the scan down and bring it back up.
 *
 * WHY THIS EXISTS, and it is the bug that made SYNC NOW useless: start_scan
 * is idempotent on g_scanning, so calling it while a scan is already
 * registered does NOTHING. That is right for the self-heal -- stacking a
 * second scan client is how the app used to hit Android's scan-throttle
 * block -- but it meant the app had no way at all to REFRESH a scan that was
 * still registered yet no longer delivering.
 *
 * And Android degrades scans behind our back with no callback: send the
 * activity to the background and the stack quietly demotes SCAN_MODE_LOW_
 * LATENCY towards opportunistic, so results only arrive when some other app
 * happens to scan. Measured on 2026-08-03: ~20 advertisements a minute from
 * 3 devices while degraded, versus ~14000 a minute from 140 devices once
 * genuinely restarted -- a 700x difference, with g_scanning reading 1 and
 * the app believing all was well the entire time. A OneTouch meter that
 * advertises in short bursts is invisible in that state, which is exactly
 * how a meter switched on and sitting next to the phone went 26 minutes
 * without syncing while the user pressed SYNC NOW.
 *
 * on_pause/on_resume was the ONLY path that produced a real restart, so the
 * user's workaround was to leave the app and come back. An explicit request
 * must not require that.
 *
 * If Java cannot confirm the stop, stop_scan leaves g_scanning set on
 * purpose and start_scan will no-op -- deliberately, so we never stack a
 * second client; the 1 Hz retry finishes the job. */
void scan_restart(struct ANativeActivity *a)
{
   if (!a || !a->env)
      return;
   stop_scan(a);
   start_scan(a);
}

int scan_running(void)
{
   return atomic_load(&g_scanning);
}

int scan_stop_pending(void)
{
   return atomic_load(&g_scan_stop_pending);
}

void scan_hold_until(long when)
{
   g_scan_hold_until = when;
}

long scan_hold_time(void)
{
   return g_scan_hold_until;
}
