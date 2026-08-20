// SPDX-License-Identifier: GPL-3.0
// dexble.c --- BLE transport: JNI glue between Ble.java and the driver
// Copyright 2026 Jakob Kastelic

/* pancra BLE transport: the thin JNI layer between the Ble.java dumb pipe and
 * the transport-agnostic protocol driver (dexdriver.c). It implements the drv_*
 * hooks via Ble's static methods and forwards Ble's callbacks into the driver.
 *
 * All driver work happens synchronously inside a native callback (or the pair
 * kickoff). Each transport call resolves a JNIEnv for ITS OWN thread via
 * any_env(): GATT callbacks arrive on binder threads, and a JNIEnv is only
 * valid on the thread that produced it, so one may never be stashed in a
 * global and reused from another. Driver state is serialised by driver_lock().
 */
#include "alarm.h" /* pancra_alarm_check: a crossing rings on THIS thread */
#include "blejni.h"
#include "bletrans.h"
#include "bondtable.h"
#include "calib.h" /* pancra_cal_result: the sensor's answer to a write */
#include "dexdriver.h"
#include "dexlibc.h"
#include "jbridge.h" /* jb_checked: the one exception verdict this app has */
#include "meter.h"
#include "reading.h"
#include "remote.h" /* pancra_remote_ok: the push worker's acknowledgement */
#include "shell.h"  /* shell_service_tick: the heartbeat's one entry point */
#include "status.h"
#include "syncjni.h"
#include "util.h"
#include <jni.h>
#include <jni_md.h>
#include <stdint.h>
/* NOT thread.h, <stdio.h> or <string.h>. All three were here for the bond
 * table -- its leaf mutex, the snprintf that copies an address into a slot,
 * the strcmp that looks one up -- and the table is in bondtable.c now. An
 * include kept for a reason that has moved out is worse than a missing one:
 * it is a claim, in the file's own include list, that this translation unit
 * still does something it no longer does. */

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

static jclass g_ble;
static jobject g_ctx;
static jmethodID m_connect, m_subscribe, m_write, m_readrssi, m_read,
    m_startsvc, m_disconnect, m_createbond, m_bondwatch;
static char g_keypath[256];
static char g_macpath[256];

/* Per-link key/MAC files. Several CGMs can be bonded at once, and each holds a
 * different shared key -- a single stelo.key would have the second sensor
 * silently overwrite the first's, dropping its bond. Link 0 keeps the historic
 * unsuffixed names so an existing install is not orphaned by this change. */
static const char *link_path(char *out, int cap, const char *base, int link)
{
   int i = 0;
   while (base[i] && i < cap - 4) {
      out[i] = base[i];
      i++;
   }
   if (link != LINK_CGM) {
      out[i++] = '.';
      out[i++] = (char)('0' + (link % 10));
   }
   out[i] = 0;
   return out;
}

static JavaVM *g_vm;       /* for a JNIEnv on any thread */
static jclass g_alarm_cls; /* com.jk.pancra.Alarm */
static jmethodID m_alarm_trigger, m_alarm_silence, m_alarm_beep, m_alarm_chirp,
    m_alarm_nudge;

/* a JNIEnv valid on the calling thread (main-loop touches and binder callbacks
 * both drive the alarm), attaching if necessary */
static JNIEnv *any_env(void);

/* Public wrapper: main.c needs a thread-correct env for the timezone lookup,
 * which runs on a BLE binder thread during a meter import. */
JNIEnv *dexble_env(void)
{
   return any_env();
}

/* The app Context, as a global ref that outlives the activity.
 *
 * the notification path used g_act->clazz, which is NULL once the
 * activity is destroyed -- so the one glucose display left to the user froze.
 * This ref is created in dexble_register and never released. */
jobject dexble_ctx(void)
{
   return g_ctx;
}

static JNIEnv *any_env(void)
{
   JNIEnv *e = 0;
   if (!g_vm)
      return 0;
   if ((*g_vm)->GetEnv(g_vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK)
      return e;
   if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&e, 0) == 0)
      return e;
   return 0;
}

/* Returns 1 only if Java was actually reached.
 *
 * This was void, so failing to reach Alarm.trigger was invisible -- and
 * alarm_apply had already committed the level as "announced", so its
 * idempotence check suppressed every later attempt and the hypo stayed silent
 * for its whole duration. That is precisely the failure the staged try/catch
 * inside Alarm.java was written to eliminate, reintroduced one layer below
 * where the staging cannot reach it. Reporting failure lets the caller decline
 * to commit, so the level-based design self-corrects on the next tick. */
int dexble_alarm(int kind, int sound, int vibrate)
{ /* kind: 0 low, 1 high, 2 stale */
   JNIEnv *e = any_env();
   if (!e || !g_alarm_cls || !m_alarm_trigger) {
      LOGI("alarm: cannot fire (e=%p cls=%p m=%p)", (void *)e,
           (void *)g_alarm_cls, (void *)m_alarm_trigger);
      return 0;
   }
   LOGI("alarm: fire trigger kind=%d sound=%d vib=%d", kind, sound, vibrate);
   (*e)->CallStaticVoidMethod(e, g_alarm_cls, m_alarm_trigger, g_ctx,
                              (jint)kind, (jboolean)sound, (jboolean)vibrate);
   if ((*e)->ExceptionCheck(e)) {
      LOGI("alarm: java threw");
      (*e)->ExceptionClear(e);
      return 0;
   }
   return 1;
}

/* One short beep (NEW DATAPOINT alert). Best-effort: a missed beep is harmless,
 * unlike the glucose alarm. */
void dexble_beep(void)
{
   JNIEnv *e = any_env();
   if (!e || !g_alarm_cls || !m_alarm_beep) {
      LOGI("beep: not wired (e=%p cls=%p m=%p)", (void *)e, (void *)g_alarm_cls,
           (void *)m_alarm_beep);
      return;
   }
   LOGI("beep: fire");
   (*e)->CallStaticVoidMethod(e, g_alarm_cls, m_alarm_beep, g_ctx);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

/* One pitch-bent chirp (NEW DATAPOINT alert in CHIRP mode). `st10` is tenths
 * of a semitone, signed; 0 sounds exactly like the beep. Best-effort, same as
 * dexble_beep. */
void dexble_chirp(int st10)
{
   JNIEnv *e = any_env();
   if (!e || !g_alarm_cls || !m_alarm_chirp) {
      LOGI("chirp: not wired (e=%p cls=%p m=%p)", (void *)e,
           (void *)g_alarm_cls, (void *)m_alarm_chirp);
      return;
   }
   LOGI("chirp: fire st10=%d", st10);
   (*e)->CallStaticVoidMethod(e, g_alarm_cls, m_alarm_chirp, g_ctx, (jint)st10);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

/* One NUDGE (a threshold crossing on the wider, one-time band). `kind` is
 * 0 low, 1 high. Best-effort, same as dexble_beep: the nudge is by definition
 * the alert the user is allowed to miss, and it must never be able to throw
 * into the caller, which is the alarm evaluator. */
void dexble_nudge(int kind, int sound, int vibrate)
{
   JNIEnv *e = any_env();
   if (!e || !g_alarm_cls || !m_alarm_nudge) {
      LOGI("nudge: not wired (e=%p cls=%p m=%p)", (void *)e,
           (void *)g_alarm_cls, (void *)m_alarm_nudge);
      return;
   }
   LOGI("nudge: fire kind=%d sound=%d vib=%d", kind, sound, vibrate);
   (*e)->CallStaticVoidMethod(e, g_alarm_cls, m_alarm_nudge, g_ctx, (jint)kind,
                              sound ? JNI_TRUE : JNI_FALSE,
                              vibrate ? JNI_TRUE : JNI_FALSE);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

/* Returns 1 only if Java was reached. Symmetric with dexble_alarm, and for the
 * same reason: the callers commit the "silent" state BEFORE calling, so a
 * silent no-op here leaves a looping USAGE_ALARM MediaPlayer running with
 * g_alarm_want == AL_NONE (no later apply issues another silence) and
 * g_alarm_sounding == 0 (every further tap falls through to the UI). That is
 * an alarm that plays until the process dies -- the worst outcome in this
 * file's own risk model. */
int dexble_alarm_silence(void)
{
   JNIEnv *e = any_env();
   if (!e || !g_alarm_cls || !m_alarm_silence)
      return 0;
   (*e)->CallStaticVoidMethod(e, g_alarm_cls, m_alarm_silence, g_ctx);
   if ((*e)->ExceptionCheck(e)) {
      /* Java threw, so the player is still running. Reporting success here
       * contradicted the contract stated above and produced exactly the
       * outcome it names: the caller has already committed the silent state,
       * so g_alarm_want is AL_NONE (no later apply issues another silence)
       * and g_alarm_sounding is 0 (every further tap falls through to the
       * UI), and the tone plays until the process dies. Symmetric with
       * dexble_alarm, which has always got this right. */
      LOGW("alarm: java threw on silence; alarm may still be sounding");
      (*e)->ExceptionClear(e);
      return 0;
   }
   return 1;
}

/* ---- drv_* transport hooks ---- */
void drv_connect(int link, const char *mac)
{
   JNIEnv *e = any_env();
   if (!e)
      return;
   /* NULL means OOM with an exception PENDING; the next JNI call on this thread
    * would then be illegal (VM abort under CheckJNI). subscribe/write guard the
    * same NewStringUTF pattern; connect -- reached on every pair/reconnect --
    * did not. */
   jstring m = (*e)->NewStringUTF(e, mac);
   if (!m) {
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      return;
   }
   jstring err =
       (*e)->CallStaticObjectMethod(e, g_ble, m_connect, g_ctx, m, (jint)link);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(
          e); /* don't leave it pending for the next JNI call */
   (*e)->DeleteLocalRef(e, m);
   if (err) {
      /* GetStringUTFChars can itself return NULL on OOM; s then flows into
       * LOGI("%s") and set_status -> NULL deref. */
      const char *s = (*e)->GetStringUTFChars(e, err, 0);
      if (s) {
         LOGI("connect: %s", s);
         set_status(s);
         (*e)->ReleaseStringUTFChars(e, err, s);
      }
      (*e)->DeleteLocalRef(e, err);
   }
}

/* The bond table lives in bondtable.c.
 *
 * It was here, in the middle of the JNI bridge, and that is why nothing
 * tested it: this file is one translation unit holding the whole bridge, so
 * linking it into a host suite means supplying a JavaVM. The table itself
 * needs none of that -- it is a fixed array, a lock, and two functions over
 * strings -- so it moved to a file that a host test CAN link, and
 * bondtabletest now runs a reader and a writer at it under ThreadSanitizer.
 * The race this lock exists for was, until then, argued rather than shown. */

/* A BLUETOOTH ADDRESS, AS MUCH OF IT AS A LOG LINE NEEDS.
 *
 * logcat is readable by anyone holding the phone with adb enabled, and it is
 * exactly what a bug report collects and mails off. A Bluetooth address is a
 * stable per-device identifier, so a full one in a log line is a durable
 * record of WHICH sensor this person wears, sitting in a file that leaves the
 * device for reasons that have nothing to do with pairing. The last two
 * octets are enough to tell two sensors apart while a human reads a log,
 * which is all any of these lines is for: "AA:BB:CC:DD:EE:FF" -> "EE:FF".
 *
 * Returns a pointer INTO the caller's string, so there is nothing to free and
 * nothing to size; a MAC too short to trim is returned whole, because a
 * malformed address is a bug worth seeing in full. */
static const char *mac_tail(const char *mac)
{
   int n = 0;
   while (mac[n])
      n++;
   return n > 5 ? mac + n - 5 : mac;
}

static void jni_bond_state(JNIEnv *e, jobject cls, jstring mac, jint state)
{
   (void)cls;
   if (!mac)
      return;
   const char *m = (*e)->GetStringUTFChars(e, mac, 0);
   if (!m)
      return;
   bond_state_set(m, (int)state);
   LOGI("bond: ..%s state=%d", mac_tail(m), (int)state);
   (*e)->ReleaseStringUTFChars(e, mac, m);
}

int dexble_create_bond(const char *mac)
{
   JNIEnv *e = any_env();
   if (!e || !m_createbond || !mac || !mac[0])
      return 0;
   jstring m = (*e)->NewStringUTF(e, mac);
   if (!m) {
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      return 0;
   }
   jstring err = (*e)->CallStaticObjectMethod(e, g_ble, m_createbond, g_ctx, m);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   (*e)->DeleteLocalRef(e, m);
   if (!err) {
      LOGI("createBond ..%s: requested", mac_tail(mac));
      return 1;
   }
   const char *s = (*e)->GetStringUTFChars(e, err, 0);
   if (s) {
      LOGI("createBond ..%s: %s", mac_tail(mac), s);
      set_status(s);
      (*e)->ReleaseStringUTFChars(e, err, s);
   }
   (*e)->DeleteLocalRef(e, err);
   return 0;
}

void dexble_subscribe(int link, const char *uuid, int indicate)
{
   JNIEnv *e = any_env();
   if (!e)
      return;
   /* NULL means OOM with an exception PENDING, and any further JNI call on a
    * thread with a pending exception is illegal -- so the failure would not
    * stay contained to this one operation. dexble_write already guards this
    * exact pattern; subscribe and the devinfo reads did not. */
   jstring u = (*e)->NewStringUTF(e, uuid);
   if (!u) {
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      return;
   }
   (*e)->CallStaticVoidMethod(e, g_ble, m_subscribe, (jint)link, u,
                              (jboolean)indicate);
   /* Clear before returning, for the reason dexble_write spells out: this is
    * reached from jni_connected, which goes on to make further JNI calls with
    * the exception still pending (illegal -- CheckJNI aborts) and then returns
    * into onServicesDiscovered, which has no catch of its own. That is an
    * uncaught throw on a Bluetooth binder thread, i.e. the death of the
    * process holding the CGM link and the alarm. */
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   (*e)->DeleteLocalRef(e, u);
}

void dexble_write(int link, const char *uuid, const uint8_t *d, int n,
                  int no_resp)
{
   JNIEnv *e = any_env();
   if (!e)
      return;
   jstring u    = (*e)->NewStringUTF(e, uuid);
   jbyteArray a = (*e)->NewByteArray(e, n);
   /* Both allocations can fail under memory pressure, and SetByteArrayRegion
    * on a NULL array ABORTS the VM rather than throwing. notify_update guards
    * the identical pattern; this path did not. */
   if (!u || !a) {
      (*e)->ExceptionClear(e);
      if (u)
         (*e)->DeleteLocalRef(e, u);
      if (a)
         (*e)->DeleteLocalRef(e, a);
      return;
   }
   (*e)->SetByteArrayRegion(e, a, 0, n, (const jbyte *)d);
   (*e)->CallStaticVoidMethod(e, g_ble, m_write, (jint)link, u, a,
                              (jboolean)no_resp);
   /* Leaving an exception pending makes the NEXT JNI call on this thread
    * illegal; Ble.write's queue/link path is not wrapped in a catch-all. */
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   (*e)->DeleteLocalRef(e, u);
   (*e)->DeleteLocalRef(e, a);
}

/* The Dexcom driver owns LINK_CGM; these keep its existing hook names. */
void drv_subscribe(int link, const char *uuid, int indicate)
{
   dexble_subscribe(link, uuid, indicate);
}

void drv_write(int link, const char *uuid, const uint8_t *d, int n, int no_resp)
{
   dexble_write(link, uuid, d, n, no_resp);
}

/* Drop the link now rather than waiting for the peer to time out. The meter
 * driver needs this: holding a meter connected keeps it awake past its own
 * auto-power-off and burns its coin cell for nothing. */
void dexble_link_close(int link)
{
   JNIEnv *e = any_env();
   if (!e || !m_disconnect)
      return;
   (*e)->CallStaticVoidMethod(e, g_ble, m_disconnect, (jint)link);
   /* Clear rather than leave pending: this runs on a binder thread that will
    * make further JNI calls, and a pending exception makes all of them
    * illegal. */
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

void drv_status(const char *s)
{
   set_status(s);
}

static void ble_read_rssi(int link)
{
   JNIEnv *e = any_env();
   if (!e || !m_readrssi)
      return;
   (*e)->CallStaticVoidMethod(e, g_ble, m_readrssi,
                              (jint)link); /* result -> onRssi */
   if ((*e)->ExceptionCheck(e)) /* never return to Java with one pending */
      (*e)->ExceptionClear(e);
}

/* One-shot read of the Device Information Service (0x180A) strings the Stelo
 * actually exposes: model 0x2A24, firmware 0x2A26, manufacturer 0x2A29. (It has
 * no 0x2A25 serial / 0x2A28 software characteristic.) Results arrive on onRead.
 */
static void dexble_devinfo_on(int link);

/* Read the Device Information Service on an EXPLICIT LINK. There was a
 * no-argument wrapper beside this that read LINK_CGM, left over from when
 * there was one link -- an ambient version of a link-addressed operation,
 * which is the exact shape the driver boundary was rebuilt to remove: the
 * meter's model and firmware are not the CGM's, and a caller that forgot to
 * say which link recorded one device's strings against another's row, in an
 * append-only file. Nothing called it. */
void dexble_request_devinfo_link(int link)
{
   dexble_devinfo_on(link);
}

static void dexble_devinfo_on(int link)
{
   JNIEnv *e = any_env();
   if (!e || !m_read)
      return;
   static const char *uuids[3] = {
       "00002a24-0000-1000-8000-00805f9b34fb", /* model number   */
       "00002a26-0000-1000-8000-00805f9b34fb", /* firmware rev.  */
       "00002a29-0000-1000-8000-00805f9b34fb", /* manufacturer   */
   };
   for (int i = 0; i < 3; i++) {
      jstring u = (*e)->NewStringUTF(e, uuids[i]);
      if (!u) {
         if ((*e)->ExceptionCheck(e))
            (*e)->ExceptionClear(e);
         return;
      }
      (*e)->CallStaticVoidMethod(e, g_ble, m_read, (jint)link,
                                 u); /* result -> onRead */
      /* Per iteration: a pending exception makes the NEXT NewStringUTF
       * illegal, so this cannot wait until the loop ends. */
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      (*e)->DeleteLocalRef(e, u);
   }
}

/* THE ANSWER IS PASSED STRAIGHT BACK, not summarised here. Whether a reading
 * was kept is a fact only the history knows, and the driver is the only
 * caller that needs it -- see drv_glucose in dexdriver.h for what it does
 * with it. */
int drv_glucose(int link, int mg, int trend, int age)
{
   return pancra_glucose(link, mg, trend, age);
}

void drv_cal_result(int link, int result, int sensor_id, int mg_dl,
                    unsigned gen)
{
   (void)link; /* the queue is keyed by SENSOR id, not by link */
   /* The token is passed straight through: the driver kept it only so this
    * answer could name the write it belongs to, and the queue -- which is the
    * only thing that knows what is queued now -- decides whether it matches. */
   pancra_cal_result(result, sensor_id, mg_dl, gen);
}

int drv_backfill(int link, int mg, int trend, int age)
{
   return pancra_backfill(link, mg, trend, age);
}

int drv_key_load(int link, uint8_t key[16])
{
   char pth[264];
   int fd = open(link_path(pth, sizeof pth, g_keypath, link), O_RDONLY);
   if (fd < 0)
      return 0;
   int ok = (read(fd, key, 16) == 16);
   close(fd);
   return ok;
}

int drv_key_save(int link, const uint8_t key[16])
{
   char pth[264];
   link_path(pth, sizeof pth, g_keypath, link);
   if (atomic_replace(pth, key, 16) == REPLACE_FAILED) {
      /* NO PATH. It is the app's private data directory, which is a
       * filesystem layout detail the reader cannot act on and logcat has no
       * business publishing; the link number already names the file. */
      LOGW("link %d: session key NOT SAVED", link);
      return -1;
   }
   return 0;
}

void drv_key_clear(int link)
{
   char pth[264];
   unlink(link_path(pth, sizeof pth, g_keypath, link));
} /* stale key: force a fresh pairing */

/* Persist the bonded sensor's MAC next to the key, so after a restart we
 * reconnect ONLY to that exact sensor (never grab another Dexcom in range). */
int drv_mac_load(int link, char *mac, int n)
{
   char pth[264];
   int fd = open(link_path(pth, sizeof pth, g_macpath, link), O_RDONLY);
   if (fd < 0)
      return 0;
   long r = read(fd, mac, (unsigned)(n - 1));
   close(fd);
   if (r <= 0)
      return 0;
   mac[r] = 0;
   return 1;
}

int drv_mac_save(int link, const char *mac)
{
   char pth[264];
   link_path(pth, sizeof pth, g_macpath, link);
   int len = 0;
   while (mac[len])
      len++;
   if (atomic_replace(pth, mac, len) == REPLACE_FAILED) {
      LOGW("link %d: sensor address NOT SAVED", link); /* no path: see above */
      return -1;
   }
   return 0;
}

void drv_mac_clear(int link)
{
   char pth[264];
   unlink(link_path(pth, sizeof pth, g_macpath, link));
}

/* ---- Ble.java callbacks (each stashes its env, then drives the state machine)
 * ---- */
/* Which protocol owns each link. The link id -- not the characteristic -- is
 * the routing key, so two sensors that share a GATT layout (Stelo and G7 do)
 * can be connected at once without their events being confused. */
/* (WHICH LINK CARRIES A METER used to be a bitmask here AND a table in
 * meter.c -- the same fact in two places, each written under the driver's
 * lock from another module. It is the driver's now: see
 * driver_link_set_meter, and driver_route_* for the callbacks that read it.)
 */
static void jni_connected(JNIEnv *e, jclass c, jint link)
{
   (void)c;
   /* THE ROUTING IS THE DRIVER'S. This used to read the meter bit here and
    * branch, holding the driver's lock across both halves -- the decision and
    * the dispatch have to be one critical section, and doing that from the
    * transport meant reaching for another module's lock. One call now; what
    * is left for this side is whatever reaches Java, which must happen with
    * that lock released. */
   enum driver_after after = driver_route_connected(link);
   if (after == DRV_AFTER_CLOSE) {
      dexble_link_close(link);
      return;
   }
   if (after == DRV_AFTER_RSSI_METER) {
      /* Read THIS link explicitly: ble_read_rssi targets the CGM link, which
       * is not the meter here. The result returns via onRssi -> jni_rssi ->
       * pancra_meter_rssi. */
      if (e && m_readrssi) {
         (*e)->CallStaticVoidMethod(e, g_ble, m_readrssi, link);
         if ((*e)->ExceptionCheck(e)) /* we return into Java from here */
            (*e)->ExceptionClear(e);
      }
   } else if (after == DRV_AFTER_RSSI) {
      ble_read_rssi(link);
   }
}

static void jni_disconnected(JNIEnv *e, jclass c, jint link, jint s)
{
   (void)c;
   (void)e;
   driver_route_disconnected(link, s);
}

static void jni_written(JNIEnv *e, jclass c, jint link, jstring ju, jint s)
{
   (void)c;
   (void)e;
   const char *u = (*e)->GetStringUTFChars(e, ju, 0);
   if (!u) { /* OOM, exception pending -- u would NULL-deref in strcmp */
      (*e)->ExceptionClear(e);
      return;
   }
   /* The meter protocol is request/response and drives itself off
    * notifications, so a write ack needs no action there -- but the TEST and
    * the dispatch must still be one step: a stale read feeds a METER's ack
    * into the Dexcom state machine on a link with no session, or drops a real
    * CGM ack, and the J-PAKE handshake is driven by exactly those acks. */
   driver_route_written(link, u, s);
   (*e)->ReleaseStringUTFChars(e, ju, u);
}

static void jni_notify(JNIEnv *e, jclass c, jint link, jstring ju,
                       jbyteArray jd)
{
   (void)c;
   (void)e;
   const char *u = (*e)->GetStringUTFChars(e, ju, 0);
   /* NULL means OOM with an exception pending. u goes straight into
    * driver_on_notify, whose first act is strcmp(uuid, ...) -- a NULL deref on
    * a BLE thread. jni_on_advert already guards exactly this. */
   if (!u) {
      (*e)->ExceptionClear(e);
      return;
   }
   jsize n = jd ? (*e)->GetArrayLength(e, jd) : 0;
   uint8_t buf[256];
   if (n > 256)
      n = 256;
   if (n > 0)
      (*e)->GetByteArrayRegion(e, jd, 0, n, (jbyte *)buf);
   driver_route_notify(link, u, buf, n);
   /* Evaluate the alarm here -- on this BLE thread, but only AFTER the driver
    * lock is released.
    *
    * It cannot go inside the dispatch: Alarm.trigger blocks for hundreds of
    * milliseconds and driver_lock is a no-timeout spin lock the main looper
    * also takes. It must not be left to the UI timer either: that timer lives
    * on the ACTIVITY's looper and is destroyed on back-press or task-swipe,
    * while the foreground service keeps this transport running for days -- so
    * a hypo would be decoded and logged with no sound, no vibration, and no
    * way to silence one already ringing. Alarms must not depend on a visible
    * activity. */
   pancra_alarm_check();
   (*e)->ReleaseStringUTFChars(e, ju, u);
}

/* Service heartbeat. The stale-data alarm is triggered by the ABSENCE of
 * readings, so something must evaluate it on a timer -- and that timer cannot
 * live on the activity's looper, which is destroyed on back-press while the
 * foreground service keeps running. */
static void jni_tick(JNIEnv *e, jclass c)
{
   (void)e;
   (void)c;
   /* ONE CALL. This named six functions from six modules, in an order that
    * mattered, inside the BLE transport -- the layer with the least business
    * knowing which workflows the app has. WHAT the heartbeat drives is the
    * shell's list (shell.h); that this thread is where it happens is the
    * transport's fact, and that is all this function knows. */
   shell_service_tick();
}

/* Ble.remotePush's worker thread: the server acknowledged a push (2xx). */
static void jni_remote_ok(JNIEnv *e, jclass c)
{
   (void)e;
   (void)c;
   pancra_remote_ok();
}

static void jni_rssi(JNIEnv *e, jclass c, jint link, jint rssi)
{
   (void)e;
   (void)c;
   /* Only the TEST needs to be atomic here, and the query is: the dispatch
    * below calls back into main.c, which takes locks of its own, and there is
    * no reason to widen a critical section over that. */
   if (!driver_link_is_meter(link))
      pancra_rssi(link, rssi);
   else
      pancra_meter_rssi(rssi); /* meter's last-sync signal strength */
}

static void jni_read(JNIEnv *e, jclass c, jint link, jstring ju, jbyteArray jd)
{
   (void)c;
   (void)e;
   const char *u = (*e)->GetStringUTFChars(e, ju, 0);
   if (!u) {
      (*e)->ExceptionClear(e);
      return;
   }
   jsize n = jd ? (*e)->GetArrayLength(e, jd) : 0;
   char buf[64];
   if (n > 63)
      n = 63;
   if (n > 0)
      (*e)->GetByteArrayRegion(e, jd, 0, n, (jbyte *)buf);
   buf[n < 0 ? 0 : n] = 0;
   pancra_devinfo(link, u, buf);
   (*e)->ReleaseStringUTFChars(e, ju, u);
}

/* ---- public API (called from the app side) ---- */
void dexble_init(const char *data_dir)
{
   int i = 0;
   for (; data_dir[i] && i < 200; i++)
      g_keypath[i] = data_dir[i];
   const char *f = "/stelo.key";
   for (int j = 0; f[j]; j++)
      g_keypath[i++] = f[j];
   g_keypath[i] = 0;
   int k        = 0;
   for (; data_dir[k] && k < 200; k++)
      g_macpath[k] = data_dir[k];
   const char *g = "/stelo.mac";
   for (int j = 0; g[j]; j++)
      g_macpath[k++] = g[j];
   g_macpath[k] = 0;
   driver_init();
}

int dexble_register(JNIEnv *e, jclass ble, jobject ctx)
{
   if (!e || !ble || !ctx)
      return 0;
   /* JNINativeMethod.name/signature are char* (a JNI API wart); hold the text
    * in mutable arrays so no const is cast away (-Wcast-qual +
    * -Wwrite-strings). */
   static char n0[] = "onConnected";
   static char s0[] = "(I)V";
   static char n1[] = "onDisconnected";
   static char s1[] = "(II)V";
   static char n2[] = "onNotify";
   static char s2[] = "(ILjava/lang/String;[B)V";
   static char n3[] = "onWritten";
   static char s3[] = "(ILjava/lang/String;I)V";
   static char n4[] = "onRssi";
   static char s4[] = "(II)V";
   static char n5[] = "onRead";
   static char s5[] = "(ILjava/lang/String;[B)V";
   static char n6[] = "onTick";
   static char s6[] = "()V";
   static char n7[] = "onRemoteOk";
   static char s7[] = "()V";
   static char n8[] = "onBondState";
   static char s8[] = "(Ljava/lang/String;I)V";
   /* The sync client's entry points. They BLOCK for several round trips --
    * syncRestore for one per missing bucket, so potentially hundreds -- so
    * Java only ever calls them on its push worker, never here. */
   static char n9[]                 = "syncRun";
   static char s9[]                 = "()I";
   static char n10[]                = "syncPair";
   static char s10[]                = "(Ljava/lang/String;Ljava/lang/String;)I";
   static char n11[]                = "syncRestore";
   static char s11[]                = "()I";
   static const JNINativeMethod m[] = {
       {n0,  s0,  (void *)jni_connected   },
       {n1,  s1,  (void *)jni_disconnected},
       {n2,  s2,  (void *)jni_notify      },
       {n3,  s3,  (void *)jni_written     },
       {n4,  s4,  (void *)jni_rssi        },
       {n5,  s5,  (void *)jni_read        },
       {n6,  s6,  (void *)jni_tick        },
       {n7,  s7,  (void *)jni_remote_ok   },
       {n8,  s8,  (void *)jni_bond_state  },
       {n9,  s9,  (void *)syncjni_run     },
       {n10, s10, (void *)syncjni_pair    },
       {n11, s11, (void *)syncjni_restore },
   };
   /* BUILT IN LOCALS, PUBLISHED AS A PAIR, AND ONLY ONCE THE NATIVES ARE ON
    * THE CLASS.
    *
    * The two global refs used to be assigned straight into g_ble and g_ctx,
    * unchecked, before anything else was attempted. Three consequences, and
    * the process never recovers from any of them, because a global reference
    * lives as long as the process does:
    *
    *   - A FAILED NewGlobalRef LEFT AN EXCEPTION PENDING. It fails by running
    *     out of memory, and then the next line called NewGlobalRef again with
    *     an OutOfMemoryError pending -- illegal, and under CheckJNI (on for a
    *     debuggable build and for anyone attached with a debugger) an
    *     immediate `JNI DETECTED ERROR IN APPLICATION` abort. Somebody whose
    *     app aborted saw it die on the launch that first hit memory pressure,
    *     with the tombstone naming NewGlobalRef and no hint that the previous
    *     one was the failure.
    *   - A FAILED RegisterNatives KEPT THE REFS ANYWAY. `return 0` left g_ble
    *     and g_ctx set, so the app that had just reported BLE REG FAILED --
    *     with no native methods bound at all -- nevertheless held, and
    *     published through dexble_ctx(), a Context that every later caller
    *     would take as proof the transport was up.
    *   - A RETRY OVERWROTE THEM. init_java runs again when the activity is
    *     recreated in the same process (a back-press does exactly that), and
    *     the plain assignment dropped the previous pair on the floor: two
    *     permanently unreachable global refs per relaunch, against a table
    *     with a hard ceiling and an abort behind it.
    *
    * So: construct into locals, refuse on the first failure with BOTH deleted,
    * and publish the pair in one step at the point where Java can begin
    * calling back into C -- which is the instant RegisterNatives succeeds, and
    * not before. */
   jclass gble  = (*e)->NewGlobalRef(e, ble);
   jobject gctx = 0;
   if (gble) /* not if it failed: the second call would be the illegal one */
      gctx = (*e)->NewGlobalRef(e, ctx);
   if (!gble || !gctx)
      goto refuse;

   /* The COUNT, not a literal that has to be remembered: registering 8 of 9
    * leaves onBondState unbound, and the first bond transition then takes the
    * process down with an UnsatisfiedLinkError from a binder thread. */
   if ((*e)->RegisterNatives(e, ble, m, (jint)(sizeof m / sizeof m[0])) != 0)
      goto refuse;

   /* PUBLISHED. From here a binder thread may call jni_notify and read g_ctx,
    * so the pair has to be live before the first callback -- which is why
    * this is the moment, and why the id lookups below cannot be waited for.
    * The previous pair (a relaunch) is released after the swap, never before:
    * a reader between the two stores must see a valid ref, not a freed one. */
   {
      jclass old_ble  = g_ble;
      jobject old_ctx = g_ctx;
      g_ble           = gble;
      g_ctx           = gctx;
      if (old_ble)
         (*e)->DeleteGlobalRef(e, old_ble);
      if (old_ctx)
         (*e)->DeleteGlobalRef(e, old_ctx);
   }
   m_connect = (*e)->GetStaticMethodID(
       e, ble, "connect",
       "(Landroid/content/Context;Ljava/lang/String;I)Ljava/lang/String;");
   m_subscribe =
       (*e)->GetStaticMethodID(e, ble, "subscribe", "(ILjava/lang/String;Z)V");
   m_write =
       (*e)->GetStaticMethodID(e, ble, "write", "(ILjava/lang/String;[BZ)V");
   m_readrssi   = (*e)->GetStaticMethodID(e, ble, "readRssi", "(I)V");
   m_disconnect = (*e)->GetStaticMethodID(e, ble, "disconnect", "(I)V");
   /* A missed method id leaves a pending NoSuchMethodError; making any further
    * JNI call with one pending is illegal and aborts under CheckJNI. Clear it
    * here so a lookup failure degrades to a null id instead of taking the
    * process down on the next call. */
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   m_read = (*e)->GetStaticMethodID(e, ble, "read", "(ILjava/lang/String;)V");
   m_startsvc   = (*e)->GetStaticMethodID(e, ble, "startService",
                                          "(Landroid/content/Context;)V");
   m_createbond = (*e)->GetStaticMethodID(
       e, ble, "createBond",
       "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;");
   m_bondwatch = (*e)->GetStaticMethodID(e, ble, "bondWatch",
                                         "(Landroid/content/Context;)V");
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   /* Start watching bond state right away, not at the first createBond: the
    * sensor can begin pairing on its own (a reconnect after the bond was
    * cleared does exactly that), and those are the transitions the user most
    * needs told about. */
   if (m_bondwatch)
      (*e)->CallStaticVoidMethod(e, g_ble, m_bondwatch, g_ctx);
   if (m_startsvc)
      (*e)->CallStaticVoidMethod(e, g_ble, m_startsvc,
                                 g_ctx); /* keep alive in bg */
   (*e)->GetJavaVM(e, &g_vm);
   /* The sync transport rides the same class and the same VM. */
   syncjni_wire(e, ble);
   /* Every id we will later call, not just most of them: a missing
    * m_disconnect would leave dexble_link_close a silent no-op, holding a
    * meter awake past its own power-off -- the one thing otble.h says must
    * never happen. */
   return m_connect && m_subscribe && m_write && m_readrssi && m_read &&
          m_disconnect && m_startsvc && m_createbond && m_bondwatch;

refuse:
   /* NOTHING WAS PUBLISHED, so nothing is left behind. jb_checked describes
    * the OutOfMemoryError (or whatever RegisterNatives raised -- it throws
    * NoSuchMethodError when a native's signature no longer matches Ble.java,
    * which is the ordinary consequence of libpancra and classes.dex being out
    * of step) and clears it, because the caller goes on to draw a status row,
    * and a pending exception would abort at whichever JNI call came first.
    *
    * DeleteGlobalRef is one of the few JNI calls that is legal with an
    * exception pending, so the order here -- clear, then release -- is a
    * courtesy to the log rather than a requirement. Both are released whether
    * one or both were taken. */
   (void)jb_checked(e, "dexble_register");
   if (gctx)
      (*e)->DeleteGlobalRef(e, gctx);
   if (gble)
      (*e)->DeleteGlobalRef(e, gble);
   LOGI("BLE natives NOT registered; no global refs published");
   return 0;
}

/* Wire the Alarm class. FindClass here would resolve via the framework loader,
 * which can't see app classes, so main.c loads it via the activity's
 * classloader (find_app_class) and hands it in. */
void dexble_set_alarm(JNIEnv *e, jclass alarm_cls)
{
   if (!alarm_cls) {
      LOGI("alarm class not wired (null)");
      return;
   }
   g_alarm_cls     = (*e)->NewGlobalRef(e, alarm_cls);
   m_alarm_trigger = (*e)->GetStaticMethodID(e, alarm_cls, "trigger",
                                             "(Landroid/content/Context;IZZ)V");
   m_alarm_silence = (*e)->GetStaticMethodID(e, alarm_cls, "silence",
                                             "(Landroid/content/Context;)V");
   m_alarm_beep    = (*e)->GetStaticMethodID(e, alarm_cls, "beep",
                                             "(Landroid/content/Context;)V");
   m_alarm_nudge   = (*e)->GetStaticMethodID(e, alarm_cls, "nudge",
                                             "(Landroid/content/Context;IZZ)V");
   m_alarm_chirp   = (*e)->GetStaticMethodID(e, alarm_cls, "chirp",
                                             "(Landroid/content/Context;I)V");
   /* A missed method id leaves a pending NoSuchMethodError, and ANY further
    * JNI call with one pending is illegal -- a VM abort under CheckJNI. The
    * caller goes on to load settings and build strings, so the throw would
    * surface far from here. dexble_register states this rule and obeys it;
    * this function never did, and it now performs five lookups, any of which
    * can miss if libpancra and classes.dex are ever out of step. Each id is
    * separately NULL-checked at every use site, so clearing is safe. */
   if ((*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
      LOGI("alarm class: a method id is missing (dex/so mismatch?)");
   }
   LOGI("alarm class wired (trigger=%p silence=%p chirp=%p nudge=%p)",
        (void *)m_alarm_trigger, (void *)m_alarm_silence, (void *)m_alarm_chirp,
        (void *)m_alarm_nudge);
}

void dexble_pair(int link, const char *mac, const char *code)
{
   driver_start(link, mac, code);
}

/* Open the meter's link. It is a plain connect: the meter driver takes over
 * from onConnected and tears the link down itself when it is finished. */
int dexble_meter_connect(int link, const char *mac)
{
   /* Select the meter's link only for the duration of the connect, under the
    * lock, so a concurrent CGM reconnect cannot observe the swap and route a
    * sensor into the meter's protocol driver. */
   if (link < 0 || link >= LINK_MAX)
      return 0;
   /* Report whether the request actually reached Java. drv_connect is
    * best-effort and returns nothing, so a missing JNIEnv (Bluetooth off, or
    * before the transport is wired) used to look exactly like success -- and
    * the caller then recorded the meter as armed forever. */
   if (!any_env())
      return 0;
   /* ONE step for both: routing the link to the meter protocol and issuing
    * the connect must not be observable half-done by a concurrent CGM
    * reconnect -- a connect on a link whose routing has not landed yet
    * delivers the meter's first notification to the Dexcom state machine. */
   driver_meter_connect(link, mac, drv_connect);
   return 1;
}

void dexble_reconnect(int link)
{ /* stall watchdog: force a fresh connect on a SPECIFIC link */
   /* NAMED, not ambient. driver_kick used to act on whatever context was
    * selected, and the GATT callbacks selected without restoring -- so with a
    * second sensor or a meter sync in flight this kicked whichever link a
    * binder thread last touched, spuriously reconnecting a healthy link while
    * leaving the stalled one stranded until the next throttle window. */
   driver_kick(link);
}
