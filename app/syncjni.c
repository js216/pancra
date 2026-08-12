// SPDX-License-Identifier: GPL-3.0
// syncjni.c --- the sync client's transport: native <-> Ble.syncHttp
// Copyright 2026 Jakob Kastelic

/* sync.c decides every byte on the wire; this file only carries them. The
 * carrying happens in Java because the platform's TLS is free there and a C
 * TLS stack would cost about a megabyte of library in a 143 kB app -- so the
 * protocol stays in C, where it can be read and tested, and only the socket
 * is borrowed.
 *
 * THREADS. syncRun and syncPair block for several round trips, so Java only
 * ever calls them on its push worker; every JNI call below therefore happens
 * on that worker, and any_env attaches it. Nothing here touches the UI or the
 * driver lock.
 */
#include "syncjni.h"
#include "dexlibc.h" /* open/lseek: the state stamp */
#include "insulin.h"
#include "pancra.h" /* pancra_logs_reload: restore writes files */
#include "sensors.h"
#include "settings.h"
#include "store.h"
#include "sync.h"
#include "weight.h"
#include <jni.h>    /* JNIEnv, jstring, jbyteArray: this file IS the bridge */
#include <jni_md.h> /* jint, jbyte: the JDK puts the scalar types here */
#include <stdint.h> /* uint8_t: the pairing key crosses as raw bytes */
#include <stdio.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...);

#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)

static JavaVM *g_vm;
static jclass g_cls;            /* Ble, global ref */
static jmethodID m_synchttp;    /* the transport */
static jmethodID m_synccode;    /* the status of the last one */
static jmethodID m_syncsoon;    /* ask Java to run a sync on its worker */
static jmethodID m_restoresoon; /* ...and to run a restore there */
static jmethodID m_pairsoon;    /* ask Java to run a pairing on its worker */

static JNIEnv *sync_env(void)
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

/* The transport hook sync.c calls. Returns the HTTP status, or -1 if the
 * request could not be made at all -- which sync.c treats as "nothing
 * happened", so the whole run is retried later rather than half-applied. */
static int jni_http(const char *method, const char *path, const char *hdr,
                    const char *body, int blen, char *out, int outcap)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_synchttp || !m_synccode)
      return -1;
   if (!g_remote_server[0])
      return -1;

   jstring jsrv  = (*e)->NewStringUTF(e, g_remote_server);
   jstring jm    = (*e)->NewStringUTF(e, method);
   jstring jp    = (*e)->NewStringUTF(e, path);
   jstring jh    = hdr ? (*e)->NewStringUTF(e, hdr) : 0;
   jbyteArray jb = 0;
   if (blen > 0) {
      jb = (*e)->NewByteArray(e, (jsize)blen);
      if (jb)
         (*e)->SetByteArrayRegion(e, jb, 0, (jsize)blen, (const jbyte *)body);
   }
   jobject r = 0;
   if (jsrv && jm && jp)
      r = (*e)->CallStaticObjectMethod(e, g_cls, m_synchttp, jsrv,
                                       (jint)g_remote_port, jm, jp, jh, jb);
   /* A pending exception makes every later JNI call illegal, so clear it here
    * and report failure rather than take the process down on the next one. */
   if ((*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
      r = 0;
   }
   int code = (int)(*e)->CallStaticIntMethod(e, g_cls, m_synccode);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);

   int n = 0;
   if (r) {
      jsize len = (*e)->GetArrayLength(e, (jbyteArray)r);
      n         = len < outcap - 1 ? (int)len : outcap - 1;
      if (n > 0)
         (*e)->GetByteArrayRegion(e, (jbyteArray)r, 0, (jsize)n, (jbyte *)out);
      (*e)->DeleteLocalRef(e, r);
   }
   out[n < 0 ? 0 : n] = '\0';

   if (jsrv)
      (*e)->DeleteLocalRef(e, jsrv);
   if (jm)
      (*e)->DeleteLocalRef(e, jm);
   if (jp)
      (*e)->DeleteLocalRef(e, jp);
   if (jh)
      (*e)->DeleteLocalRef(e, jh);
   if (jb)
      (*e)->DeleteLocalRef(e, jb);
   return code;
}

/* WHICH FILES SYNC. Everything that is a record of what happened, and nothing
 * that is a credential: g_code_path and g_remote_path hold the pairing code
 * and the derived key, and uploading those would put the secret that
 * authenticates us TO the server inside the server's own database.
 *
 * `bucketed` splits a log by UTC day on the row's leading timestamp. The two
 * small state files are single-bucket: they are rewritten wholesale, so
 * splitting them would buy nothing. */
void syncjni_register_logs(void)
{
   sync_clear_logs();
   (void)sync_add_log("readings", g_store_path, 1);
   (void)sync_add_log("insulin", g_ins_path, 1);
   (void)sync_add_log("weight", g_wt_path, 1);
   (void)sync_add_log("sensors", g_sensors_path, 1);
   (void)sync_add_log("slots", g_slots_path, 0);
}

/* Sizes of every synced file, added up. A reading, a dose, a correction or a
 * deletion all change one of them, and nothing else does. Cheap: five
 * open/lseek pairs, no reading. */
long syncjni_state_stamp(void)
{
   static const char *const paths[] = {g_store_path, g_ins_path, g_wt_path,
                                       g_sensors_path, g_slots_path};
   long total                       = 0;
   for (int i = 0; i < (int)(sizeof paths / sizeof paths[0]); i++) {
      if (!paths[i] || !paths[i][0])
         continue;
      int fd = open(paths[i], O_RDONLY, 0);
      if (fd < 0)
         continue;
      long n = lseek(fd, 0, SEEK_END);
      close(fd);
      if (n > 0)
         total += n;
   }
   return total;
}

void syncjni_wire(JNIEnv *e, jclass ble)
{
   (*e)->GetJavaVM(e, &g_vm);
   g_cls      = (*e)->NewGlobalRef(e, ble);
   m_synchttp = (*e)->GetStaticMethodID(
       e, ble, "syncHttp",
       "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;"
       "Ljava/lang/String;[B)[B");
   m_synccode    = (*e)->GetStaticMethodID(e, ble, "syncCode", "()I");
   m_syncsoon    = (*e)->GetStaticMethodID(e, ble, "syncSoon", "()V");
   m_restoresoon = (*e)->GetStaticMethodID(e, ble, "syncRestoreSoon", "()V");
   m_pairsoon    = (*e)->GetStaticMethodID(
       e, ble, "syncPairSoon", "(Ljava/lang/String;Ljava/lang/String;)V");
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   sync_set_http(jni_http);
   syncjni_register_logs();
}

jint syncjni_run(JNIEnv *e, jobject cls)
{
   (void)e;
   (void)cls;
   syncjni_register_logs(); /* the paths are set after wiring, on first launch
                             */
   if (!g_sync_uid) {
      sync_report(0, "NOT PAIRED");
      return 0;
   }
   int ok = sync_run() == 0;
   /* One line per sync, not per request: a sync is dozens of requests, and a
    * log entry each turned the app's log into its own traffic. */
   LOGI("sync %s (uid %ld, %s:%d)", ok ? "ok" : "FAILED", g_sync_uid,
        g_remote_server, g_remote_port);
   sync_report(ok, ok ? "SYNCED" : "SYNC FAILED");
   return ok ? 1 : 0;
}

/* RESTORE, on the same worker as a sync and never on the UI thread: it is a
 * GET per missing bucket, so a phone recovering a year of history makes
 * hundreds of round trips.
 *
 * Logs are reloaded here rather than by the caller, because sync_restore
 * writes FILES -- without this the rows are on disk and the plot, statistics
 * and history still show what they showed before, which looks exactly like a
 * restore that did nothing. */
jint syncjni_restore(JNIEnv *e, jobject cls)
{
   (void)e;
   (void)cls;
   syncjni_register_logs();
   if (!g_sync_uid) {
      sync_report(0, "NOT PAIRED");
      return 0;
   }
   int n = sync_restore();
   LOGI("restore %s (uid %ld, %d bucket(s))", n >= 0 ? "ok" : "FAILED",
        g_sync_uid, n);
   if (n < 0) {
      sync_report(0, "RESTORE FAILED");
      return 0;
   }
   if (n > 0)
      pancra_logs_reload(); /* the files changed under the in-memory state */
   sync_report(1, n > 0 ? "RESTORED" : "NOTHING TO RESTORE");
   return 1;
}

jint syncjni_pair(JNIEnv *e, jobject cls, jstring email, jstring code)
{
   (void)cls;
   const char *em = email ? (*e)->GetStringUTFChars(e, email, 0) : 0;
   const char *cd = code ? (*e)->GetStringUTFChars(e, code, 0) : 0;
   uint8_t key[SYNC_KEY_LEN];
   long uid = 0;
   int rc   = -1;
   if (em && cd)
      rc = sync_pair(em, cd, key, &uid);
   if (rc == 0) {
      sync_key_save(uid, key);
      sync_report(0, "PAIRED");
   } else {
      /* The server refuses a wrong code, an unknown account and a spent code
       * the same way, on purpose -- so this cannot say which, only that it
       * did not work. Silence was worse: the keypad simply closed. */
      sync_report(0, "PAIRING FAILED");
   }
   if (em)
      (*e)->ReleaseStringUTFChars(e, email, em);
   if (cd)
      (*e)->ReleaseStringUTFChars(e, code, cd);
   return rc == 0 ? 1 : 0;
}

/* Both of these hand the work to Java's push worker and return at once: they
 * are called from the tick and from the UI, and neither may block. */
void syncjni_sync_request(void)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_syncsoon) {
      LOGI("sync request DROPPED: env=%p cls=%p m=%p", (void *)e, (void *)g_cls,
           (void *)m_syncsoon);
      return;
   }
   (*e)->CallStaticVoidMethod(e, g_cls, m_syncsoon);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

/* Ask Java to run a restore on the sync worker. */
void syncjni_restore_request(void)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_restoresoon) {
      LOGI("restore request DROPPED: env=%p cls=%p m=%p", (void *)e,
           (void *)g_cls, (void *)m_restoresoon);
      return;
   }
   (*e)->CallStaticVoidMethod(e, g_cls, m_restoresoon);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
}

void syncjni_pair_request(const char *email, const char *code)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_pairsoon)
      return;
   jstring je = (*e)->NewStringUTF(e, email);
   jstring jc = (*e)->NewStringUTF(e, code);
   if (je && jc)
      (*e)->CallStaticVoidMethod(e, g_cls, m_pairsoon, je, jc);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   if (je)
      (*e)->DeleteLocalRef(e, je);
   if (jc)
      (*e)->DeleteLocalRef(e, jc);
}
