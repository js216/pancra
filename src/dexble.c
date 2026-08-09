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
#include "dexdriver.h"
#include "dexlibc.h"
#include "otble.h"
#include "pancra.h"
#include <jni.h>
#include <jni_md.h>
#include <stdint.h>
#include <stdio.h>  /* snprintf: the bond table copies an address */
#include <string.h> /* strcmp: ...and looks one up by address */

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)

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
 * main.c's notification path used g_act->clazz, which is NULL once the
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
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   return 1;
}

/* ---- drv_* transport hooks ---- */
void drv_connect(const char *mac)
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
   jstring err = (*e)->CallStaticObjectMethod(e, g_ble, m_connect, g_ctx, m,
                                              (jint)driver_link());
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(
          e); /* don't leave it pending for the next JNI call */
   (*e)->DeleteLocalRef(e, m);
   if (err) {
      /* GetStringUTFChars can itself return NULL on OOM; s then flows into
       * LOGI("%s") and pancra_status -> NULL deref. */
      const char *s = (*e)->GetStringUTFChars(e, err, 0);
      if (s) {
         LOGI("connect: %s", s);
         pancra_status(s);
         (*e)->ReleaseStringUTFChars(e, err, s);
      }
      (*e)->DeleteLocalRef(e, err);
   }
}

/* The last bond state the receiver reported, per address.
 *
 * A tiny fixed table rather than a field on the sensor slot: the bond is a
 * property of the ADDRESS as the OS sees it, the broadcast arrives on a binder
 * thread with nothing but a MAC, and a device can be bonding before it has a
 * slot at all (the meter path connects to a bond it is still forming). Sized
 * for LINK_MAX devices plus slack; the oldest entry is recycled, which is
 * harmless because only the newest transition per device is ever interesting.
 */
#define BOND_SLOTS 12

static struct {
   char mac[20];
   int state;
} g_bond[BOND_SLOTS];

static int g_bond_n;

int dexble_bond_state(const char *mac)
{
   if (!mac || !mac[0])
      return 0;
   /* No lock: written on a binder thread, read on the main loop, and both are
    * a single int per entry whose only use is a status string. A torn read
    * would cost one frame of a stale label. Taking driver_lock here would put
    * a BLE callback behind the driver mutex for a cosmetic value. */
   for (int i = 0; i < g_bond_n; i++)
      if (strcmp(g_bond[i].mac, mac) == 0)
         return g_bond[i].state;
   return 0;
}

static void bond_state_set(const char *mac, int state)
{
   for (int i = 0; i < g_bond_n; i++) {
      if (strcmp(g_bond[i].mac, mac) == 0) {
         g_bond[i].state = state;
         return;
      }
   }
   int i = (g_bond_n < BOND_SLOTS) ? g_bond_n++ : (BOND_SLOTS - 1);
   (void)snprintf(g_bond[i].mac, sizeof g_bond[i].mac, "%s", mac);
   g_bond[i].state = state;
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
   LOGI("bond: %s state=%d", m, (int)state);
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
      LOGI("createBond %s: requested", mac);
      return 1;
   }
   const char *s = (*e)->GetStringUTFChars(e, err, 0);
   if (s) {
      LOGI("createBond %s: %s", mac, s);
      pancra_status(s);
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
void drv_subscribe(const char *uuid, int indicate)
{
   dexble_subscribe(driver_link(), uuid, indicate);
}

void drv_write(const char *uuid, const uint8_t *d, int n, int no_resp)
{
   dexble_write(driver_link(), uuid, d, n, no_resp);
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
   pancra_status(s);
}

static void ble_read_rssi(void)
{
   JNIEnv *e = any_env();
   if (!e || !m_readrssi)
      return;
   (*e)->CallStaticVoidMethod(e, g_ble, m_readrssi,
                              (jint)driver_link()); /* result -> onRssi */
   if ((*e)->ExceptionCheck(e)) /* never return to Java with one pending */
      (*e)->ExceptionClear(e);
}

/* One-shot read of the Device Information Service (0x180A) strings the Stelo
 * actually exposes: model 0x2A24, firmware 0x2A26, manufacturer 0x2A29. (It has
 * no 0x2A25 serial / 0x2A28 software characteristic.) Results arrive on onRead.
 */
/* Read the Device Information Service on an explicit link (the meter needs its
 * own model/firmware recorded, not the CGM's). */
void dexble_request_devinfo_link(int link)
{
   /* Under the lock, so swapping the driver's selected link cannot be observed
    * by another thread mid-operation. */
   driver_lock();
   int drv = driver_link();
   driver_select(link);
   dexble_request_devinfo();
   driver_select(drv);
   driver_unlock();
}

void dexble_request_devinfo(void)
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
      (*e)->CallStaticVoidMethod(e, g_ble, m_read, (jint)driver_link(),
                                 u); /* result -> onRead */
      /* Per iteration: a pending exception makes the NEXT NewStringUTF
       * illegal, so this cannot wait until the loop ends. */
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      (*e)->DeleteLocalRef(e, u);
   }
}

void drv_glucose(int mg, int trend, int age)
{
   pancra_glucose(mg, trend, age);
}

void drv_cal_result(int result)
{
   pancra_cal_result(result);
}

void drv_backfill(int mg, int trend, int age)
{
   pancra_backfill(mg, trend, age);
}

int drv_key_load(uint8_t key[16])
{
   char pth[264];
   int fd =
       open(link_path(pth, sizeof pth, g_keypath, driver_link()), O_RDONLY);
   if (fd < 0)
      return 0;
   int ok = (read(fd, key, 16) == 16);
   close(fd);
   return ok;
}

void drv_key_save(const uint8_t key[16])
{
   char pth[264];
   int fd = open(link_path(pth, sizeof pth, g_keypath, driver_link()),
                 O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd >= 0) {
      if (write(fd, key, 16) != 16) {
      }
      close(fd);
   }
}

void drv_key_clear(void)
{
   char pth[264];
   unlink(link_path(pth, sizeof pth, g_keypath, driver_link()));
} /* stale key: force a fresh pairing */

/* Persist the bonded sensor's MAC next to the key, so after a restart we
 * reconnect ONLY to that exact sensor (never grab another Dexcom in range). */
int drv_mac_load(char *mac, int n)
{
   char pth[264];
   int fd =
       open(link_path(pth, sizeof pth, g_macpath, driver_link()), O_RDONLY);
   if (fd < 0)
      return 0;
   long r = read(fd, mac, (unsigned)(n - 1));
   close(fd);
   if (r <= 0)
      return 0;
   mac[r] = 0;
   return 1;
}

void drv_mac_save(const char *mac)
{
   char pth[264];
   int fd = open(link_path(pth, sizeof pth, g_macpath, driver_link()),
                 O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd >= 0) {
      int len = 0;
      while (mac[len])
         len++;
      if (write(fd, mac, (unsigned)len) != len) {
      }
      close(fd);
   }
}

void drv_mac_clear(void)
{
   char pth[264];
   unlink(link_path(pth, sizeof pth, g_macpath, driver_link()));
}

/* ---- Ble.java callbacks (each stashes its env, then drives the state machine)
 * ---- */
/* Which protocol owns each link. The link id -- not the characteristic -- is
 * the routing key, so two sensors that share a GATT layout (Stelo and G7 do)
 * can be connected at once without their events being confused. */
/* WHICH LINKS CARRY A METER. A bitmask, not the LINK_METER constant.
 *
 * Every routing decision below is by LINK ID, because two sensors can share a
 * GATT layout and only the link tells their events apart. That was fine while
 * exactly one link was the meter's -- but a meter is only reachable for the
 * second or two it is switched on, so each registered meter needs its OWN
 * standing connection, and therefore its own link. The link a meter occupies
 * is now whatever the allocator gave it, so the transport has to be told
 * rather than assume.
 *
 * Set from main.c as links are bound. A link with no bit set runs the Dexcom
 * state machine, which is the safe default: an unset bit costs a meter a sync
 * it can retry, while a wrongly SET bit would feed a CGM's notifications to
 * the meter parser. */
static unsigned g_meter_links;

void dexble_set_meter_link(int link, int on)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   /* UNDER driver_lock, because every READER is.
    *
    * jni_connected / jni_disconnected / jni_notify / jni_written consult this
    * on a binder thread with driver_lock held, while the shell arms and
    * releases links from the main thread. Writing it unlocked was a plain
    * data race on the value that decides whether a packet goes to the Dexcom
    * state machine or the OneTouch parser -- the same misrouting the bitmask
    * was introduced to prevent. driver_lock is recursive, so callers that
    * already hold it (dexble_meter_connect) pay nothing. */
   driver_lock();
   if (on)
      g_meter_links |= 1U << (unsigned)link;
   else
      g_meter_links &= ~(1U << (unsigned)link);
   driver_unlock();
}

static int is_meter_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   return (g_meter_links & (1U << (unsigned)link)) != 0;
}

static void jni_connected(JNIEnv *e, jclass c, jint link)
{
   (void)c;
   (void)e;
   driver_lock();
   if (is_meter_link(link)) {
      /* Ask the shell first: it owns the per-meter index and the single
       * protocol state, and only it can say whether this link may have it. */
      if (!pancra_meter_connected(link)) {
         driver_unlock();
         dexble_link_close(link);
         return;
      }
      ot_on_connected();
      /* Sample the meter's link RSSI now -- it is only connected during a sync,
       * so this brief window is the one chance. Read THIS link explicitly
       * (ble_read_rssi() targets driver_link(), which is not the meter here);
       * the result returns via onRssi -> jni_rssi -> pancra_meter_rssi. */
      if (e && m_readrssi) {
         (*e)->CallStaticVoidMethod(e, g_ble, m_readrssi, link);
         if ((*e)->ExceptionCheck(e)) /* we return into Java from here */
            (*e)->ExceptionClear(e);
      }
   } else {
      driver_select(link);
      driver_on_connected();
      ble_read_rssi();
   }
   driver_unlock();
}

static void jni_disconnected(JNIEnv *e, jclass c, jint link, jint s)
{
   (void)c;
   (void)e;
   driver_lock();
   if (is_meter_link(link)) {
      /* ONLY the link that owns the exchange may reset otble's state.
       *
       * Every registered meter holds a standing connect, so a disconnect can
       * arrive for a meter that is merely idle -- or for one the shell just
       * refused because another was mid-sync. Routing that into
       * ot_on_disconnected wiped the protocol state out from under the meter
       * that WAS syncing: phase to idle mid-walk, and its remaining records
       * lost or filed under the wrong id. The shell owns that decision. */
      if (pancra_meter_disconnected(link))
         ot_on_disconnected();
   } else {
      driver_select(link);
      driver_on_disconnected(s);
   }
   driver_unlock();
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
   /* The meter driver is request/response and drives itself off notifications,
    * so a write ack needs no action there.
    *
    * THE TEST ITSELF MUST BE UNDER THE LOCK. g_meter_links is written from
    * the MAIN thread (link_set_meter -> dexble_set_meter_link) and read from
    * BINDER threads, and the writer takes driver_lock on the stated premise
    * that "the readers already hold it" -- which jni_connected,
    * jni_disconnected and jni_notify do, and this did not. A stale read here
    * is not cosmetic: it feeds a METER's write-ack into driver_on_written on
    * a link with no Dexcom session, or drops a real CGM ack, and the J-PAKE
    * handshake is a state machine driven by exactly those acks. Recursive,
    * so hoisting it costs nothing. */
   driver_lock();
   if (!is_meter_link(link)) {
      driver_select(link);
      driver_on_written(u, s);
   }
   driver_unlock();
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
   driver_lock();
   if (is_meter_link(link)) {
      ot_on_notify(buf, n);
   } else {
      driver_select(link);
      driver_on_notify(u, buf, n);
   }
   driver_unlock();
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
   pancra_alarm_check();
   /* Keep the lock-screen notification tracking readings too: it is the only
    * glucose display left once the activity is gone. */
   pancra_notify_refresh();
   /* Also repair stranded links. on_timer does this too, but on_timer lives on
    * the ACTIVITY's looper and dies with it, while this service tick is
    * designed to outlive the activity by days -- which is exactly the window
    * in which a stranded link would otherwise never be reconnected. */
   pancra_link_watchdog();
   /* And push: the sync is driven from here for the SAME reason -- the
    * activity's timer dies with the activity, so with the app backgrounded
    * nothing left the phone until it was reopened. */
   pancra_remote_sync();
   meter_sync_watchdog();
   pancra_reconcile_tick();
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
   /* Same unlocked-read as jni_written had -- see there. Only the TEST needs
    * the lock, so snapshot the bit and dispatch outside it: pancra_*_rssi
    * call back into main.c and may take sensors_lock, and there is no reason
    * to widen the critical section to cover them. */
   driver_lock();
   int meter = is_meter_link(link);
   driver_unlock();
   if (!meter)
      pancra_rssi(rssi);
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

/* ---- public API (called from main.c) ---- */
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
   g_ble = (*e)->NewGlobalRef(e, ble);
   g_ctx = (*e)->NewGlobalRef(e, ctx);
   /* JNINativeMethod.name/signature are char* (a JNI API wart); hold the text
    * in mutable arrays so no const is cast away (-Wcast-qual +
    * -Wwrite-strings). */
   static char n0[]                 = "onConnected";
   static char s0[]                 = "(I)V";
   static char n1[]                 = "onDisconnected";
   static char s1[]                 = "(II)V";
   static char n2[]                 = "onNotify";
   static char s2[]                 = "(ILjava/lang/String;[B)V";
   static char n3[]                 = "onWritten";
   static char s3[]                 = "(ILjava/lang/String;I)V";
   static char n4[]                 = "onRssi";
   static char s4[]                 = "(II)V";
   static char n5[]                 = "onRead";
   static char s5[]                 = "(ILjava/lang/String;[B)V";
   static char n6[]                 = "onTick";
   static char s6[]                 = "()V";
   static char n7[]                 = "onRemoteOk";
   static char s7[]                 = "()V";
   static char n8[]                 = "onBondState";
   static char s8[]                 = "(Ljava/lang/String;I)V";
   static const JNINativeMethod m[] = {
       {n0, s0, (void *)jni_connected   },
       {n1, s1, (void *)jni_disconnected},
       {n2, s2, (void *)jni_notify      },
       {n3, s3, (void *)jni_written     },
       {n4, s4, (void *)jni_rssi        },
       {n5, s5, (void *)jni_read        },
       {n6, s6, (void *)jni_tick        },
       {n7, s7, (void *)jni_remote_ok   },
       {n8, s8, (void *)jni_bond_state  },
   };
   /* The COUNT, not a literal that has to be remembered: registering 8 of 9
    * leaves onBondState unbound, and the first bond transition then takes the
    * process down with an UnsatisfiedLinkError from a binder thread. */
   if ((*e)->RegisterNatives(e, ble, m, (jint)(sizeof m / sizeof m[0])) != 0)
      return 0;
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
   /* Every id we will later call, not just most of them: a missing
    * m_disconnect would leave dexble_link_close a silent no-op, holding a
    * meter awake past its own power-off -- the one thing otble.h says must
    * never happen. */
   return m_connect && m_subscribe && m_write && m_readrssi && m_read &&
          m_disconnect && m_startsvc && m_createbond && m_bondwatch;
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
   driver_lock();
   driver_select(link);
   driver_start(mac, code);
   driver_unlock();
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
   driver_lock();
   dexble_set_meter_link(link, 1); /* route this link's events to otble */
   int drv = driver_link();
   driver_select(link);
   drv_connect(mac);
   driver_select(drv);
   driver_unlock();
   return 1;
}

void dexble_reconnect(int link)
{ /* stall watchdog: force a fresh connect on a SPECIFIC link */
   driver_lock();
   /* Select explicitly. driver_kick() acts on the ambient context, and the GATT
    * callbacks select without restoring -- so with a second sensor or a meter
    * sync in flight this kicked whichever link a binder thread last touched,
    * spuriously reconnecting a healthy link while leaving the stalled one
    * stranded until the next throttle window. */
   int prev = driver_link();
   driver_select(link);
   driver_kick();
   driver_select(prev);
   driver_unlock();
}
