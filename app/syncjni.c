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
#include "exercise.h"
#include "food.h"
#include "insulin.h"
#include "jbridge.h"  /* jb_checked: the one exception verdict this app has */
#include "log.h"      /* LOGI/LOGW: the ONE declaration */
#include "logsload.h" /* pancra_logs_reload: a restore rewrote the files */
#include "remotecfg.h"
#include "sensors.h"
#include "store.h"
#include "sync.h"
#include "syncreport.h" /* sync_report: how the sync went */
#include "syncstat.h"
#include "util.h"
#include "weight.h"
#include "wireint.h" /* PRIwire: the wire's scalars, printed exactly */
#include <jni.h>     /* JNIEnv, jstring, jbyteArray: this file IS the bridge */
#include <jni_md.h>  /* jint, jbyte: the JDK puts the scalar types here */
#include <stdint.h>  /* uint8_t: the pairing key crosses as raw bytes */
#include <stdio.h>

static JavaVM *g_vm;
static jclass g_cls;            /* Ble, global ref */
static jmethodID m_synchttp;    /* the transport */
static jmethodID m_synccode;    /* the status of the last one */
static jmethodID m_syncfail;    /* ...and why, when there was no status */
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

/* WHY THE RUN FAILED, remembered as the requests go by.
 *
 * sync.c reports a run as "worked" or "did not" and can report nothing else:
 * it never sees a socket, only this hook's return value. So each request that
 * fails records what it failed as, and the run reports the last one -- which
 * is the one that stopped it, because sync.c aborts on the first failure.
 *
 * Written and read on the sync worker only (Ble calls syncRun/syncPair from
 * its push worker and nowhere else), so no lock: the UI never touches it.
 */
static int g_why = SYNC_IDLE;

static void why_note(int status, int netfail)
{
   int by_status = sync_outcome_of_status(status);
   if (by_status != SYNC_IDLE) {
      g_why = by_status; /* the server ANSWERED; that beats a guess */
      return;
   }
   if (status < 100)
      g_why = sync_outcome_of_net(netfail);
}

/* What to report for a run that failed, when nothing more specific was
 * recorded -- a failure inside sync.c itself (a reply it could not parse, a
 * file it could not read) rather than on the wire. */
static int why_failed(void)
{
   return g_why == SYNC_IDLE ? SYNC_FAILED : g_why;
}

/* A PAIRING that failed. sync.c cannot tell a refused code from a server it
 * never reached -- both are "rc != 0" -- but this file saw the wire, so it
 * can: anything that failed before a usable answer is reported as itself, and
 * everything else means the server heard us and said no. */
static int pair_why(void)
{
   int w = why_failed();
   return sync_outcome_before_reply(w) ? w : SYNC_PAIR_REFUSED;
}

/* The transport hook sync.c calls. Returns the HTTP status, or -1 if the
 * request could not be made at all -- which sync.c treats as "nothing
 * happened", so the whole run is retried later rather than half-applied. */
static int jni_http(const char *method, const char *path, const char *hdr,
                    const char *body, int blen, char *out, int outcap)
{
   /* ONE snapshot of where this goes and as whom. Taken as two reads, a
    * request can be addressed to one server carrying another account's
    * signature -- see remote_config_get. */
   struct remote_config rc;
   remote_config_get(&rc);
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_synchttp || !m_synccode || !m_syncfail)
      return -1;
   if (!rc.server[0])
      return -1;

   /* Declared together and zeroed, because the unwind at the bottom is
    * reached from eight places and has to be able to tell an argument that
    * was never built from one that was. */
   jstring jsrv  = 0;
   jstring jm    = 0;
   jstring jp    = 0;
   jstring jh    = 0;
   jbyteArray jb = 0;
   jobject r     = 0;
   int code      = -1;
   int netfail   = SYNC_NET_OK;
   int n         = 0;
   int too_long  = 0;
   int ret       = -1;

   /* ONE ALLOCATION, ONE VERDICT, AND NOTHING AFTER A FAILED ONE.
    *
    * Made back to back with no check between them, with the results tested
    * -- three of the five -- only when it is time to call, three separate
    * things go wrong, and they get worse in order:
    *
    *   1. IT ENTERS JAVA WITH A NULL BODY. With `jb` untested, a
    *      NewByteArray that fails with blen > 0 leaves jb NULL and the call
    *      went ahead: Ble.syncHttp reads `body != null && body.length > 0`,
    *      finds null, sets no output stream, and posts the request WITH NO
    *      BODY AT ALL. The server then answers a well-formed empty push, and
    *      the phone reads that answer as the outcome of the push it thought
    *      it had made. The same hole swallowed `jh`: a header string that
    *      failed to allocate produced a request with no Authorization line
    *      rather than a request that was not made.
    *   2. IT READ A STATUS THAT BELONGED TO ANOTHER REQUEST. When jsrv, jm or
    *      jp was null the call was skipped -- and then syncCode() was asked
    *      anyway. sSyncCode is a field holding the status of the LAST
    *      syncHttp, so a request that never happened returned the previous
    *      one's 200, with an empty body, to a caller whose whole contract is
    *      that a negative value means "nothing happened". sync.c would treat
    *      an upload it never made as accepted and move on.
    *   3. IT MADE JNI CALLS WITH AN EXCEPTION PENDING. NewStringUTF and
    *      NewByteArray do not merely return NULL on failure; they leave an
    *      OutOfMemoryError pending, and every JNI function called after that
    *      -- the next NewStringUTF, DeleteLocalRef, the call itself -- is
    *      illegal. Under CheckJNI, which is on for a debuggable build and for
    *      anyone attached with a debugger, that is not a failure the app can
    *      report: it is `JNI DETECTED ERROR IN APPLICATION: JNI NewStringUTF
    *      called with pending exception java.lang.OutOfMemoryError` and an
    *      immediate SIGABRT. Somebody whose app aborted saw it die during a
    *      background sync -- a workflow they never started -- with the
    *      tombstone naming whichever innocent JNI call came next.
    *
    * So: allocate one at a time, refuse on the first failure, and leave
    * through the unwind below without touching Java. */
   jsrv = (*e)->NewStringUTF(e, rc.server);
   if (!jsrv)
      goto refused;
   jm = (*e)->NewStringUTF(e, method);
   if (!jm)
      goto refused;
   jp = (*e)->NewStringUTF(e, path);
   if (!jp)
      goto refused;
   /* NO HEADER and A HEADER WE COULD NOT BUILD are different things. Java
    * treats a null `hdr` as "set no extra request properties", which is
    * correct for the requests that carry none and catastrophic for the ones
    * that carry the signature -- so only the first may reach it. */
   if (hdr) {
      jh = (*e)->NewStringUTF(e, hdr);
      if (!jh)
         goto refused;
   }
   if (blen > 0) {
      jb = (*e)->NewByteArray(e, (jsize)blen);
      if (!jb)
         goto refused;
      /* SetByteArrayRegion throws ArrayIndexOutOfBoundsException rather than
       * returning anything, so the only way to learn it did not fill the
       * array is to ask -- and an unfilled array is a request whose body is
       * whatever the VM zeroed it to. */
      (*e)->SetByteArrayRegion(e, jb, 0, (jsize)blen, (const jbyte *)body);
      if (!jb_checked(e, "SetByteArrayRegion(body)"))
         goto refused;
   }

   /* Every argument exists and nothing is pending: NOW Java may be entered. */
   r = (*e)->CallStaticObjectMethod(e, g_cls, m_synchttp, jsrv, (jint)rc.port,
                                    jm, jp, jh, jb);
   /* A pending exception makes every later JNI call illegal, so clear it here
    * and report failure rather than take the process down on the next one. */
   if ((*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
      r = 0;
   }
   code = (int)(*e)->CallStaticIntMethod(e, g_cls, m_synccode);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   netfail = (int)(*e)->CallStaticIntMethod(e, g_cls, m_syncfail);
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   why_note(code, netfail);

   /* A REPLY THAT DOES NOT FIT IS A FAILED REQUEST, not a short one.
    *
    * Clamping to outcap-1 and handing back 200 leaves the caller unable to
    * tell: a truncated body is exactly outcap-1 bytes, which is also
    * what a legitimate body of that length looks like, and sync_fetch_bucket
    * checks only `len < cap`. So a bucket between the phone's own buffer
    * (SYNC_BUF_MAX) and the wire's ceiling (BODY_MAX) was accepted with rows
    * missing off the end -- and then compared, hashed and acted on. The
    * asymmetry rule in lib/wirevec.h is explicit that an implementation may
    * hold less than the wire allows and must then DECLINE, never truncate. */
   if (r) {
      jsize len = (*e)->GetArrayLength(e, (jbyteArray)r);
      if (outcap <= 0 || (int64_t)len > (int64_t)outcap - 1) {
         too_long = 1;
      } else {
         n = (int)len;
         if (n > 0)
            (*e)->GetByteArrayRegion(e, (jbyteArray)r, 0, (jsize)n,
                                     (jbyte *)out);
      }
   }
   if (outcap > 0)
      out[too_long ? 0 : n] = '\0';
   /* NEGATIVE, like a request that could not be made at all: the transport
    * contract in sync.h says a negative value means "no answer", and no
    * answer is exactly what an oversized one is worth here. */
   ret = too_long ? -1 : code;
   goto unwind;

refused:
   /* THE REQUEST WAS NEVER MADE, so nothing about it may be reported as if it
    * had been. syncCode() and syncFail() are deliberately NOT consulted here:
    * they answer for the previous request, and the caller cannot tell a stale
    * 200 from a fresh one. The outcome is recorded as a plain local failure
    * so the screen says something rather than nothing, and -1 tells sync.c to
    * retry the whole run later rather than treat any of it as applied. */
   (void)jb_checked(e, "sync argument marshalling");
   if (outcap > 0)
      out[0] = '\0';
   g_why = SYNC_FAILED;
   LOGI("sync request NOT SENT: its JNI arguments could not be built");
   ret = -1;

unwind:
   /* EVERY local ref this call took, on every path. There is no Java frame to
    * pop out here -- the sync worker is an attached native thread -- so these
    * accumulate against the reference ceiling for the life of the process,
    * and a sync is dozens of requests. */
   if (r)
      (*e)->DeleteLocalRef(e, r);
   if (jb)
      (*e)->DeleteLocalRef(e, jb);
   if (jh)
      (*e)->DeleteLocalRef(e, jh);
   if (jp)
      (*e)->DeleteLocalRef(e, jp);
   if (jm)
      (*e)->DeleteLocalRef(e, jm);
   if (jsrv)
      (*e)->DeleteLocalRef(e, jsrv);
   return ret;
}

/* WHICH FILES SYNC. Everything that is a record of what happened, and nothing
 * that is a credential: code_path() and remote_path() hold the pairing code
 * and the derived key, and uploading those would put the secret that
 * authenticates us TO the server inside the server's own database.
 *
 * `bucketed` splits a log by UTC day on the row's leading timestamp. The two
 * small state files are single-bucket: they are rewritten wholesale, so
 * splitting them would buy nothing. */
void syncjni_register_logs(void)
{
   /* ONE LIST, PUBLISHED IN ONE STORE. A clear() and eight adds, each taking
    * the configuration lock on its own, let a sync that starts while they run
    * take a snapshot of three logs, upload three logs, and then agree with the
    * server that both sides matched. See sync_set_logs. */
   const struct sync_log_spec specs[] = {
       {"readings",  store_path(),      1},
       {"insulin",   insulin_path(),    1},
       {"weight",    weight_path(),     1},
       {"food",      food_path(),       1},
       {"exercise",  exercise_path(),   1},
       /* THE FOOD VOCABULARY IS NOT BUCKETED, and it is the one log here
        * whose rows do not begin with a timestamp -- they are "<id>,<name>".
        * Bucketing splits on the leading field read as a UTC day, so asking
        * for it here would file every food under whichever day its ID
        * happened to look like. It is also small and rewritten by append
        * only, so a single bucket costs nothing, exactly as for slots
        * below. */
       {"foodtypes", food_types_path(), 0},
       {"sensors",   sensors_path(),    1},
       {"slots",     slots_path(),      0},
   };
   /* A REFUSAL IS SAID OUT LOUD. It can only mean a path too long for the
    * registry's field or a list longer than SYNC_MAX_LOGS -- both of which
    * leave the PREVIOUS registry standing, so the app keeps syncing whatever
    * it was syncing rather than nothing. Silently, that is a phone that
    * uploads an out-of-date list of logs for the rest of the run. */
   if (sync_set_logs(specs, (int)(sizeof specs / sizeof specs[0])) != 0)
      LOGI("sync: the log registry was REFUSED -- the previous one stands");
}

/* HAS ANYTHING CHANGED SINCE THE LAST SYNC?
 *
 * The sizes of every synced file, plus the count of committed record
 * mutations (util.h). The sizes alone were the whole answer once, and they
 * are wrong for exactly the changes a person makes deliberately: correcting a
 * dose from 12 to 13 units rewrites the row at the same length, so every
 * size is identical and the phone concluded it had nothing to send. The
 * correction then sat unsent until the six-hour safety sync.
 *
 * THIS LIST AND syncjni_register_logs MUST NAME THE SAME FILES. A log
 * registered for sync but missing here is worse than one missing from both:
 * it uploads correctly whenever something ELSE has changed, and never when
 * only it has -- so it appears to work, and loses exactly the records that
 * were logged on their own. When this list said "the five synced files" it
 * meant it, and adding food and exercise to the registry without adding them
 * here would have been that bug, twice.
 *
 * Cheap either way: an open/lseek pair per file and a counter, no reading. */
int64_t syncjni_state_stamp(void)
{
   const char *paths[] = {store_path(),   insulin_path(),    weight_path(),
                          food_path(),    food_types_path(), exercise_path(),
                          sensors_path(), slots_path()};
   int64_t total       = 0;
   for (int i = 0; i < (int)(sizeof paths / sizeof paths[0]); i++) {
      if (!paths[i] || !paths[i][0])
         continue;
      int fd = open(paths[i], O_RDONLY, 0);
      if (fd < 0)
         continue;
      int64_t n = lseek(fd, 0, SEEK_END);
      close(fd);
      if (n > 0)
         total += n;
   }
   /* Mixed in rather than added, so a mutation cannot be cancelled out by a
    * size moving the other way. */
   return (total * 1000003L) + record_generation();
}

void syncjni_wire(JNIEnv *e, jobject activity)
{
   if (!e || !activity)
      return;
   /* THE TRANSPORT'S OWN CLASS, which is PancraNet (PancraNet.java). The only
    * part of the transport that lives on Ble is the three natives, which
    * RegisterNatives binds to that class and which PancraNet calls as
    * ordinary static methods. Resolved through the app's own classloader for
    * the reason jbridge.h gives: FindClass in a native-activity callback
    * cannot see our classes.dex at all. */
   jclass net = NULL;
   if (!jb_app_class(e, activity, "com.jk.pancra.PancraNet", &net) || !net) {
      LOGI("sync transport NOT wired: PancraNet not found");
      return;
   }
   (*e)->GetJavaVM(e, &g_vm);
   /* CHECKED BEFORE ANYTHING ELSE IS ASKED FOR. NewGlobalRef fails only by
    * running out of memory, and when it does it leaves an OutOfMemoryError
    * PENDING -- so six unconditional GetStaticMethodID calls after it are six
    * illegal JNI calls, the first of which aborts under CheckJNI. Leaving
    * with g_cls NULL is the honest outcome: every
    * entry point in this file tests it, so the sync degrades to "no
    * transport" (jni_http returns -1, the run is reported failed and retried)
    * rather than taking the launch down. */
   jclass gcls = (*e)->NewGlobalRef(e, net);
   if (!gcls) {
      (void)jb_checked(e, "NewGlobalRef(PancraNet) for the sync transport");
      LOGI("sync transport NOT wired: no global reference for PancraNet");
      (*e)->DeleteLocalRef(e, net);
      return;
   }
   /* SWAPPED, NOT OVERWRITTEN. dexble_register calls this, and init_java calls
    * THAT again whenever the activity is recreated in the same process -- so a
    * plain assignment dropped the previous global ref beyond any reach, once
    * per relaunch, for the life of the process. */
   {
      jclass old = g_cls;
      g_cls      = gcls;
      if (old)
         (*e)->DeleteGlobalRef(e, old);
   }
   m_synchttp = (*e)->GetStaticMethodID(
       e, net, "syncHttp",
       "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;"
       "Ljava/lang/String;[B)[B");
   m_synccode    = (*e)->GetStaticMethodID(e, net, "syncCode", "()I");
   m_syncfail    = (*e)->GetStaticMethodID(e, net, "syncFail", "()I");
   m_syncsoon    = (*e)->GetStaticMethodID(e, net, "syncSoon", "()Z");
   m_restoresoon = (*e)->GetStaticMethodID(e, net, "syncRestoreSoon", "()V");
   m_pairsoon    = (*e)->GetStaticMethodID(
       e, net, "syncPairSoon", "(Ljava/lang/String;Ljava/lang/String;)V");
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   (*e)->DeleteLocalRef(e, net);
   sync_set_http(jni_http);
   syncjni_register_logs();
}

jint syncjni_run(JNIEnv *e, jobject cls)
{
   struct remote_config rc;
   remote_config_get(&rc);
   (void)e;
   (void)cls;
   syncjni_register_logs(); /* the paths are set after wiring, on first launch
                             */
   if (!rc.uid) {
      sync_report(SYNC_NOT_PAIRED);
      return 0;
   }
   g_why  = SYNC_IDLE;
   int ok = sync_run() == 0;
   /* One line per sync, not per request: a sync is dozens of requests, and a
    * log entry each turned the app's log into its own traffic. */
   LOGI("sync %s (uid %" PRIwire ", %s:%d)", ok ? "ok" : "FAILED", rc.uid,
        rc.server, rc.port);
   sync_report(ok ? SYNC_OK : why_failed());
   return ok ? 1 : 0;
}

/* RESTORE, on the same worker as a sync and never on the UI thread: it is a
 * GET per missing bucket, so a phone recovering a year of history makes
 * hundreds of round trips.
 *
 * Logs are reloaded here rather than by the caller, because sync_restore
 * writes FILES -- without this the rows are on disk and the plot, statistics
 * and history still show what they showed before, which looks exactly like a
 * restore that did nothing.
 *
 * EVERY pancra_logs_reload() below runs BEFORE the sync_report() beside it,
 * and that order is the contract, not an accident of layout. sync_report is
 * how the restore announces itself to the screen the user is watching; the
 * moment it fires they look at the history, the plot and the TIR/average
 * beside them. Reporting first would put a "RESTORED" line on a screen whose
 * numbers had not been rebuilt yet -- which is the same disagreement the
 * rebuild exists to remove, moved from "until you restart" to "for however
 * long the rebuild takes". Anything added here that publishes state must go
 * on the reload side of that line. */
jint syncjni_restore(JNIEnv *e, jobject cls)
{
   struct remote_config rc;
   remote_config_get(&rc);
   (void)e;
   (void)cls;
   syncjni_register_logs();
   if (!rc.uid) {
      sync_report(SYNC_NOT_PAIRED);
      return 0;
   }
   g_why = SYNC_IDLE;
   int n = sync_restore();
   LOGI("restore %s (uid %" PRIwire ", %d bucket(s))",
        n == SYNC_RESTORE_UNSYNCED ? "UNSYNCED"
        : n >= 0                   ? "ok"
                                   : "FAILED",
        rc.uid, n);
   /* CHANGED BUT UNPROVEN, and handled BEFORE the `n < 0` test -- it is
    * negative so no caller counts it as rows, and reading it as a failure
    * would report nothing restored over a log that has every row in it.
    *
    * The logs are reloaded, because the rows really are in the files and the
    * user should see them; what is uncertain is only whether a power loss
    * before the writeback completes takes them away again. */
   if (n == SYNC_RESTORE_UNSYNCED) {
      pancra_logs_reload();
      sync_report(SYNC_RESTORED_UNSYNCED);
      return 1;
   }
   if (n < 0) {
      sync_report(why_failed());
      return 0;
   }
   if (n > 0)
      pancra_logs_reload(); /* the files changed under the in-memory state */
   sync_report(n > 0 ? SYNC_RESTORED : SYNC_NOTHING_NEW);
   return 1;
}

jint syncjni_pair(JNIEnv *e, jobject cls, jstring email, jstring code)
{
   (void)cls;
   const char *em = email ? (*e)->GetStringUTFChars(e, email, 0) : 0;
   const char *cd = code ? (*e)->GetStringUTFChars(e, code, 0) : 0;
   uint8_t key[SYNC_KEY_LEN];
   int64_t uid = 0;
   int rc      = -1;
   g_why       = SYNC_IDLE;
   if (em && cd)
      rc = sync_pair(em, cd, key, &uid);
   if (rc == 0) {
      if (remote_key_save(uid, key) == 0) {
         sync_report(SYNC_PAIRED);
      } else {
         sync_report(SYNC_KEY_NOT_SAVED);
         rc = -1;
      }
   } else {
      /* The server refuses a wrong code, an unknown account and a spent code
       * the same way, on purpose -- so this cannot say WHICH of those, only
       * that it did not work. It can still say whether the server was
       * reached at all, which is the part the user can act on. Silence was
       * worse: the keypad simply closed. */
      sync_report(pair_why());
   }
   if (em)
      (*e)->ReleaseStringUTFChars(e, email, em);
   if (cd)
      (*e)->ReleaseStringUTFChars(e, code, cd);
   return rc == 0 ? 1 : 0;
}

/* Both of these hand the work to Java's push worker and return at once: they
 * are called from the tick and from the UI, and neither may block. */
int syncjni_sync_request(void)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_syncsoon) {
      LOGI("sync request DROPPED: env=%p cls=%p m=%p", (void *)e, (void *)g_cls,
           (void *)m_syncsoon);
      return 0;
   }
   jboolean took = (*e)->CallStaticBooleanMethod(e, g_cls, m_syncsoon);
   /* A THROW IS A DROP. The exception is cleared (leaving one pending would
    * abort the VM at the next JNI call on this thread), but clearing it does
    * not mean the worker was started -- and the caller has to know the
    * difference, because nothing else will ever tell it. */
   if ((*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
      LOGI("sync request DROPPED: Ble.syncSoon threw");
      return 0;
   }
   /* ...AND SO IS A COALESCED REFUSAL. syncSoon drops a request made while a
    * sync is already running, which is the COMMON case: the tick fires every
    * minute and a slow sync easily outlives one. That path throws nothing and
    * returns normally, so it was indistinguishable from success. */
   if (!took) {
      LOGI("sync request DROPPED: a sync is already running");
      return 0;
   }
   return 1;
}

/* Ask Java to run a restore on the sync worker.
 *
 * A DROP IS REPORTED, unlike the sync's. Both of these are USER-INITIATED --
 * a tap on RESTORE, a pairing code typed into the keypad -- so there is a
 * person watching a screen for the answer, and the answer arrives only
 * through sync_report(). A silent drop leaves the restore row unchanged
 * forever and closes the pairing keypad with nothing said, which is exactly
 * the failure syncjni_pair() was written to remove at the other end of the
 * same flow. The sync's own drop needs no report: nobody is waiting on it,
 * and remote.c retries it at the next tick. */
void syncjni_restore_request(void)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_restoresoon) {
      LOGI("restore request DROPPED: env=%p cls=%p m=%p", (void *)e,
           (void *)g_cls, (void *)m_restoresoon);
      sync_report(SYNC_FAILED);
      return;
   }
   (*e)->CallStaticVoidMethod(e, g_cls, m_restoresoon);
   if ((*e)->ExceptionCheck(e)) {
      (*e)->ExceptionClear(e);
      LOGI("restore request DROPPED: Ble.syncRestoreSoon threw");
      sync_report(SYNC_FAILED);
   }
}

void syncjni_pair_request(const char *email, const char *code)
{
   JNIEnv *e = sync_env();
   if (!e || !g_cls || !m_pairsoon) {
      LOGI("pair request DROPPED: env=%p cls=%p m=%p", (void *)e, (void *)g_cls,
           (void *)m_pairsoon);
      sync_report(SYNC_FAILED);
      return;
   }
   jstring je = (*e)->NewStringUTF(e, email);
   jstring jc = (*e)->NewStringUTF(e, code);
   if (je && jc)
      (*e)->CallStaticVoidMethod(e, g_cls, m_pairsoon, je, jc);
   /* A NULL string is an OutOfMemoryError pending, so this covers it too. */
   if (!je || !jc || (*e)->ExceptionCheck(e)) {
      if ((*e)->ExceptionCheck(e))
         (*e)->ExceptionClear(e);
      LOGI("pair request DROPPED: the request never reached the worker");
      sync_report(SYNC_FAILED);
   }
   if (je)
      (*e)->DeleteLocalRef(e, je);
   if (jc)
      (*e)->DeleteLocalRef(e, jc);
}
