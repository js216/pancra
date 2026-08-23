// SPDX-License-Identifier: GPL-3.0
// jbridge.c --- Every call into the Java adapters (see jbridge.h)
// Copyright 2026 Jakob Kastelic

#include "jbridge.h"
#include "log.h" /* LOGI: a described exception is the only account of it */
#include "ndk.h"
#include "util.h"
#include <jni.h>
#include <jni_md.h> /* jint / jlong: the fixed-width JNI scalars */
#include <stdint.h>

/* THREE CLASSES, BECAUSE THERE ARE THREE ADAPTERS.
 *
 * The GATT pipe, the export, the platform policies and the sync transport
 * are four adapters; behind one class name and one bind they are one
 * unreadable file. They are separate classes (see PancraExport.java,
 * PancraPlatform.java, PancraNet.java), and this side names the class each
 * method actually lives on rather than routing them all through a facade --
 * a delegating stub on Ble would rebuild the single facade one method deep.
 *
 * The natives are the one thing that CANNOT move. RegisterNatives binds them
 * to a class (app/dexble.c, app/pairing.c, app/scan.c), so the callbacks and
 * the three sync entry points stay declared on Ble -- which is why jb_class()
 * still exists and still means that class. */
static jclass g_ble;      /* global ref to com.jk.pancra.Ble */
static jclass g_platform; /* ...to com.jk.pancra.PancraPlatform */
static jclass g_export;   /* ...to com.jk.pancra.PancraExport */
static jmethodID m_scan, m_stop;
static jmethodID m_set_orient, m_perm_granted, m_req_perm,
    m_open_settings; /* settings-menu ops */
static jmethodID
    m_export; /* EXPORT DATA: share the CSVs via the system sheet */
static jmethodID m_batt_ok, m_req_batt, m_bucket,
    m_bg_restricted;             /* background-run ops */
static jmethodID m_show_glucose; /* push value+plot to the notification */
static jmethodID
    m_bonded_sensor; /* resolve a bonded sensor's MAC from the bond list */

jclass jb_class(void)
{
   return g_ble;
}

jclass jb_platform_class(void)
{
   return g_platform;
}

/* EVERY METHOD THIS BRIDGE NEEDS, as data.
 *
 * It was fourteen hand-written GetStaticMethodID calls whose results were
 * never looked at, followed by `return 1`. A signature that drifted -- a
 * parameter added to exportData, showGlucose losing its lock-screen flag --
 * left that one id NULL and the bind reporting success, so the feature was
 * silently dead (every call site checks its id and returns) and a
 * NoSuchMethodError sat PENDING, which makes the next unrelated JNI call fail
 * for a reason nothing in this file explains. As a table it is one loop with
 * one verdict, and adding a method cannot forget the check. */
struct jb_method {
   jmethodID *slot;
   const char *name;
   const char *sig;
};

/* ONE TABLE PER CLASS, and the class is part of the data. Binding a method
 * against the wrong class is a NoSuchMethodError at BIND time -- loud, once,
 * at launch -- which is exactly where a mistake in this file belongs. */
static const struct jb_method g_ble_methods[] = {
    /* (Context, gen): the generation the started scan owns, so an asynchronous
     * onScanFailed can name which scan died. */
    {&m_scan,          "scan",         "(Landroid/content/Context;I)Ljava/lang/String;"},
    {&m_stop,          "stop",         "()Z"                                           },
    {&m_bonded_sensor, "bondedSensor",
     "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;"                 },
};

static const struct jb_method g_platform_methods[] = {
    {&m_show_glucose,  "showGlucose",
     "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/"
     "String;Ljava/lang/String;[IIII)V"                                        },
    {&m_set_orient,    "setOrientation",        "(Landroid/content/Context;I)V"},
    {&m_perm_granted,  "permGranted",
     "(Landroid/content/Context;Ljava/lang/String;)Z"                          },
    {&m_req_perm,      "requestPerm",
     "(Landroid/content/Context;Ljava/lang/String;)V"                          },
    {&m_open_settings, "openAppSettings",       "(Landroid/content/Context;)V" },
    {&m_batt_ok,       "isBatteryUnrestricted", "(Landroid/content/Context;)Z" },
    {&m_req_batt,      "requestBatteryOpt",     "(Landroid/content/Context;)V" },
    {&m_bucket,        "standbyBucket",         "(Landroid/content/Context;)I" },
    {&m_bg_restricted, "isBgRestricted",        "(Landroid/content/Context;)Z" },
};

static const struct jb_method g_export_methods[] = {
    {&m_export, "exportData", "(Landroid/content/Context;JZZZZ)V"},
};

#define JB_NBLE ((int)(sizeof g_ble_methods / sizeof g_ble_methods[0]))
#define JB_NPLATFORM                                                           \
   ((int)(sizeof g_platform_methods / sizeof g_platform_methods[0]))
#define JB_NEXPORT ((int)(sizeof g_export_methods / sizeof g_export_methods[0]))

/* THE APP'S OWN CLASSLOADER, one checked step at a time. See jbridge.h for
 * why FindClass cannot be used from a native-activity callback at all.
 *
 * SIX CALLS, SIX VERDICTS, ONE EXIT. Making all six and looking at the result
 * of ONE -- the last -- is two separate defects wearing the same shirt:
 *
 *   - A FAILURE WAS CARRIED FORWARD. getClassLoader returning null (it can:
 *     a class loaded by the bootstrap loader has none, and that is precisely
 *     what a system-instrumented or restored activity object can be) went
 *     straight into GetObjectClass(null). A missing method id left a
 *     NoSuchMethodError PENDING and the next call was made anyway. Neither is
 *     survivable: with CheckJNI on -- and it IS on for a debuggable build and
 *     for anyone running the app under a debugger -- a JNI call made with an
 *     exception pending, or handed a null object, is not an error code, it is
 *     `JNI DETECTED ERROR IN APPLICATION: JNI GetObjectClass called with
 *     pending exception` followed by SIGABRT. Somebody whose app aborted here
 *     saw it disappear midway through the launch animation, with a tombstone
 *     blaming a JNI call that was only the first innocent one after the real
 *     failure -- and with CheckJNI off, the same input instead dereferences a
 *     null jobject inside the VM and crashes somewhere even less related.
 *   - THE LOCALS WERE NEVER GIVEN BACK. Four of these steps return local
 *     references, and this does not run inside a Java method invocation --
 *     there is no frame to pop and reclaim them (see the header). They
 *     accumulated on every call, success included.
 *
 * The cleanup is a single labelled unwind rather than a delete beside each
 * early return, because the interesting property -- that NOTHING acquired
 * here outlives the call -- has to be checkable by reading one block. */
int jb_app_class(JNIEnv *env, jobject activity, const char *name, jclass *out)
{
   if (out)
      *out = NULL;
   if (!env || !activity || !name || !out)
      return 0;

   jclass act_cls    = NULL;
   jobject loader    = NULL;
   jclass loader_cls = NULL;
   jstring jname     = NULL;
   jclass cls        = NULL;
   jmethodID get_cl  = NULL;
   jmethodID load    = NULL;
   int ok            = 0;

   /* CHECKED BEFORE TESTED, every time. The order in each condition below is
    * jb_checked first and the returned value second, and it is not stylistic:
    * `!x || !jb_checked(...)` short-circuits when x is null, which is exactly
    * the case where the VM left an exception pending -- so the one arm that
    * most needs the clear is the one that would skip it. */
   act_cls = (*env)->GetObjectClass(env, activity);
   if (!jb_checked(env, "GetObjectClass(activity)") || !act_cls)
      goto done;

   get_cl = (*env)->GetMethodID(env, act_cls, "getClassLoader",
                                "()Ljava/lang/ClassLoader;");
   if (!jb_checked(env, "GetMethodID(getClassLoader)") || !get_cl)
      goto done;

   loader = (*env)->CallObjectMethod(env, activity, get_cl);
   if (!jb_checked(env, "getClassLoader") || !loader)
      goto done;

   loader_cls = (*env)->GetObjectClass(env, loader);
   if (!jb_checked(env, "GetObjectClass(loader)") || !loader_cls)
      goto done;

   load = (*env)->GetMethodID(env, loader_cls, "loadClass",
                              "(Ljava/lang/String;)Ljava/lang/Class;");
   if (!jb_checked(env, "GetMethodID(loadClass)") || !load)
      goto done;

   /* NewStringUTF throws OutOfMemoryError rather than merely returning NULL,
    * so this is a pending exception as well as a null -- and the call it fed
    * is the one that enters Java. */
   jname = (*env)->NewStringUTF(env, name);
   if (!jb_checked(env, "NewStringUTF(class name)") || !jname)
      goto done;

   /* THE ONE CALL THAT REALLY THROWS IN PRACTICE: loadClass raises
    * ClassNotFoundException when libpancra and classes.dex are out of step,
    * which is an ordinary consequence of an incremental install. */
   cls = (*env)->CallObjectMethod(env, loader, load, jname);
   if (!jb_checked(env, "loadClass") || !cls)
      goto done;
   ok = 1;

done:
   /* Newest first, though the VM does not care -- it reads as the inverse of
    * the acquisition order above, which is how a reader checks the list is
    * complete. */
   if (jname)
      (*env)->DeleteLocalRef(env, jname);
   if (loader_cls)
      (*env)->DeleteLocalRef(env, loader_cls);
   if (loader)
      (*env)->DeleteLocalRef(env, loader);
   if (act_cls)
      (*env)->DeleteLocalRef(env, act_cls);
   /* ONLY A VALID CLASS LEAVES -- and it does so by construction rather than
    * by a second test here. `cls` is assigned by the last step and `ok` is
    * set on the line after it, with nothing between them that can fail, so
    * every path that reaches this label with ok == 0 reaches it with cls
    * still NULL. A `if (!ok && cls) delete` guard was written here first and
    * then taken out again: no case in jbridgetest could make it run, and code
    * no test can reach is a claim nobody can check. If a step is ever added
    * BETWEEN the loadClass call and `ok = 1`, that stops being true and the
    * guard has to come back. */
   *out = cls;
   return ok;
}

/* Bind one table against one class. Returns 1 with every slot filled, or 0
 * having filled none of them and said which method failed. */
static int bind_table(JNIEnv *env, jclass cls, const struct jb_method *tab,
                      int n)
{
   for (int i = 0; i < n; i++) {
      jmethodID id =
          (*env)->GetStaticMethodID(env, cls, tab[i].name, tab[i].sig);
      /* BOTH, because they are different failures with the same cure: a NULL
       * id is the lookup saying no, and a pending exception is the VM's
       * NoSuchMethodError -- which, left pending, makes the NEXT unrelated
       * JNI call fail rather than this one. */
      if (!id || (*env)->ExceptionCheck(env)) {
         /* DESCRIBE first: it names the method in logcat, and the whole
          * point of failing here is that somebody can see WHICH one. */
         if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
         }
         LOGI("jbridge: %s did not bind", tab[i].name);
         for (int k = 0; k < n; k++)
            *tab[k].slot = 0;
         return 0;
      }
      *tab[i].slot = id;
   }
   return 1;
}

/* Every slot in every table, cleared. Used on any failure, so a partial bind
 * can never be left behind. */
static void unbind_all(void)
{
   for (int i = 0; i < JB_NBLE; i++)
      *g_ble_methods[i].slot = 0;
   for (int i = 0; i < JB_NPLATFORM; i++)
      *g_platform_methods[i].slot = 0;
   for (int i = 0; i < JB_NEXPORT; i++)
      *g_export_methods[i].slot = 0;
}

/* ALL OR NOTHING, ACROSS ALL THREE CLASSES. A partial bind is the worst of
 * the three outcomes: the app runs, the screen draws, and whichever call lost
 * its id does nothing at all -- which for `stop` means a second scan client
 * stacks on the first, and for showGlucose means the notification the service
 * exists to draw is blank.
 *
 * `activity` is needed because two of the three classes are resolved HERE, by
 * name, through the app's own classloader: only Ble is handed in, since only
 * Ble has natives to register and main.c needs it for that. */
int jb_bind(JNIEnv *env, jobject activity, jclass ble_local)
{
   if (!env || !activity || !ble_local)
      return 0;
   /* Once only. A back-press destroys the activity without ending the
    * process, so a relaunch re-enters onCreate here -- re-binding would leak
    * the previous global ref every time. */
   if (g_ble)
      return 1;

   jclass plat_local = NULL;
   jclass exp_local  = NULL;
   jclass cls        = NULL;
   jclass gplat      = NULL;
   jclass gexp       = NULL;
   int ok            = 0;

   if (!jb_app_class(env, activity, "com.jk.pancra.PancraPlatform",
                     &plat_local)) {
      LOGI("jbridge: PancraPlatform NOT found");
      goto out;
   }
   if (!jb_app_class(env, activity, "com.jk.pancra.PancraExport", &exp_local)) {
      LOGI("jbridge: PancraExport NOT found");
      goto out;
   }

   /* THE GLOBAL REFS FIRST AND TOGETHER. NewGlobalRef fails by running out of
    * memory and leaves an OutOfMemoryError pending, so the next call must not
    * be made unconditionally -- under CheckJNI that is an abort, not an error
    * code. */
   cls = (*env)->NewGlobalRef(env, ble_local);
   if (cls)
      gplat = (*env)->NewGlobalRef(env, plat_local);
   if (gplat)
      gexp = (*env)->NewGlobalRef(env, exp_local);
   if (!cls || !gplat || !gexp) {
      if ((*env)->ExceptionCheck(env))
         (*env)->ExceptionClear(env);
      goto out;
   }

   if (!bind_table(env, cls, g_ble_methods, JB_NBLE) ||
       !bind_table(env, gplat, g_platform_methods, JB_NPLATFORM) ||
       !bind_table(env, gexp, g_export_methods, JB_NEXPORT)) {
      /* Give the partial state back rather than leaving half a bridge
       * behind: g_ble is what every call site tests, so leaving it set
       * would say "bound" for ever. */
      unbind_all();
      goto out;
   }

   /* PUBLISHED TOGETHER, at the end: every call site tests its class pointer,
    * and a class published before its table is bound reads as ready. */
   g_ble      = cls;
   g_platform = gplat;
   g_export   = gexp;
   cls        = NULL;
   gplat      = NULL;
   gexp       = NULL;
   ok         = 1;

out:
   if (cls)
      (*env)->DeleteGlobalRef(env, cls);
   if (gplat)
      (*env)->DeleteGlobalRef(env, gplat);
   if (gexp)
      (*env)->DeleteGlobalRef(env, gexp);
   if (plat_local)
      (*env)->DeleteLocalRef(env, plat_local);
   if (exp_local)
      (*env)->DeleteLocalRef(env, exp_local);
   return ok;
}

/* --- scan lifecycle --- */

int jb_scan(JNIEnv *env, jobject clazz, int gen, char *err, int cap)
{
   if (!env || !g_ble || !m_scan)
      return 0;
   jstring jerr =
       (*env)->CallStaticObjectMethod(env, g_ble, m_scan, clazz, (jint)gen);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      str_snapshot(err, cap, "SCAN THREW");
      return 0;
   }
   if (!jerr)
      return 1; /* no message = it started */
   /* NULL-check the chars too: OOM leaves an exception pending. */
   const char *e = (*env)->GetStringUTFChars(env, jerr, NULL);
   if (e) {
      str_snapshot(err, cap, e);
      (*env)->ReleaseStringUTFChars(env, jerr, e);
   } else {
      /* jb_checked rather than a bare ExceptionCheck/Clear pair: it is the
       * one place that describes the throw to the log, and a failure here is
       * exactly the kind that is otherwise invisible. */
      (void)jb_checked(env, "GetStringUTFChars(scan error)");
      str_snapshot(err, cap, "SCAN FAILED");
   }
   (*env)->DeleteLocalRef(env, jerr);
   return 0;
}

int jb_stop(JNIEnv *env)
{
   if (!env || !g_ble || !m_stop)
      return 0;
   jboolean stopped = (*env)->CallStaticBooleanMethod(env, g_ble, m_stop);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      return 0;
   }
   return stopped ? 1 : 0;
}

/* --- system ops --- */

/* All of these need the activity AND its env: the caller passes g_act, which
 * on_destroy clears, and some run from the 1 Hz timer -- so a teardown racing
 * one of them would dereference a null activity. */
int jb_checked(JNIEnv *e, const char *what)
{
   if (!e)
      return 0;
   if (!(*e)->ExceptionCheck(e))
      return 1;
   /* DESCRIBE THEN CLEAR. Describe first because the clear discards the
    * throwable, and a Java stack trace in logcat is the only account of what
    * went wrong that anyone will ever get here. Clear because a pending
    * exception is not a return value: it stays on the thread and aborts the
    * VM at the next JNI call, which may be in an unrelated workflow that did
    * nothing wrong. */
   (*e)->ExceptionDescribe(e);
   (*e)->ExceptionClear(e);
   LOGI("JNI %s THREW; treating as failure", what ? what : "call");
   return 0;
}

/* g_ble stands for ALL THREE classes here: jb_bind publishes them together
 * or not at all, so one test is the whole question ("is the bridge up"). */
static JNIEnv *act_env(struct ANativeActivity *a, jmethodID m)
{
   if (!a || !a->env || !g_ble || !m)
      return 0;
   return a->env;
}

int jb_set_orientation(struct ANativeActivity *a, int mode)
{
   JNIEnv *e = act_env(a, m_set_orient);
   if (!e)
      return 0;
   (*e)->CallStaticVoidMethod(e, g_platform, m_set_orient, a->clazz,
                              (jint)mode);
   return jb_checked(e, "setOrientation");
}

int jb_export_data(struct ANativeActivity *a, long cutoff, int glu, int dev,
                   int ins, int wt)
{
   JNIEnv *e = act_env(a, m_export);
   if (!e)
      return 0;
   (*e)->CallStaticVoidMethod(e, g_export, m_export, a->clazz, (jlong)cutoff,
                              (jboolean)(glu != 0), (jboolean)(dev != 0),
                              (jboolean)(ins != 0), (jboolean)(wt != 0));
   return jb_checked(e, "exportData");
}

int jb_perm_granted(struct ANativeActivity *a, const char *perm, int *granted)
{
   JNIEnv *e = act_env(a, m_perm_granted);
   if (!e)
      return 0;
   jstring p = (*e)->NewStringUTF(e, perm);
   if (!p) {
      /* NewStringUTF throws OutOfMemoryError rather than merely returning
       * NULL, so this is a pending exception too. */
      (void)jb_checked(e, "NewStringUTF(perm)");
      return 0;
   }
   jboolean r = (*e)->CallStaticBooleanMethod(e, g_platform, m_perm_granted,
                                              a->clazz, p);
   int ok     = jb_checked(e, "permGranted");
   (*e)->DeleteLocalRef(e, p);
   if (!ok)
      return 0; /* r is zero-because-it-threw, not zero-because-denied */
   if (granted)
      *granted = r ? 1 : 0;
   return 1;
}

int jb_request_perm(struct ANativeActivity *a, const char *perm)
{
   JNIEnv *e = act_env(a, m_req_perm);
   if (!e)
      return 0;
   jstring p = (*e)->NewStringUTF(e, perm);
   if (!p) {
      (void)jb_checked(e, "NewStringUTF(perm)");
      return 0;
   }
   (*e)->CallStaticVoidMethod(e, g_platform, m_req_perm, a->clazz, p);
   int ok = jb_checked(e, "requestPerm");
   (*e)->DeleteLocalRef(e, p);
   return ok;
}

int jb_open_settings(struct ANativeActivity *a)
{
   JNIEnv *e = act_env(a, m_open_settings);
   if (!e)
      return 0;
   (*e)->CallStaticVoidMethod(e, g_platform, m_open_settings, a->clazz);
   return jb_checked(e, "openSettings");
}

/* A boolean QUERY: 1 when Java answered, and only then is *out written. */
static int call_bool(struct ANativeActivity *a, jmethodID m, const char *what,
                     int *out)
{
   JNIEnv *e = act_env(a, m);
   if (!e)
      return 0;
   /* Both callers are platform-policy questions; the class is named here
    * rather than assumed, so a future third caller on another adapter is a
    * compile error and not a silent lookup on the wrong class. */
   jboolean r = (*e)->CallStaticBooleanMethod(e, g_platform, m, a->clazz);
   if (!jb_checked(e, what))
      return 0;
   if (out)
      *out = r ? 1 : 0;
   return 1;
}

int jb_battery_ok(struct ANativeActivity *a, int *ok)
{
   return call_bool(a, m_batt_ok, "batteryOk", ok);
}

int jb_bg_restricted(struct ANativeActivity *a, int *restricted)
{
   return call_bool(a, m_bg_restricted, "bgRestricted", restricted);
}

int jb_request_battery(struct ANativeActivity *a)
{
   JNIEnv *e = act_env(a, m_req_batt);
   if (!e)
      return 0;
   (*e)->CallStaticVoidMethod(e, g_platform, m_req_batt, a->clazz);
   return jb_checked(e, "requestBattery");
}

int jb_standby_bucket(struct ANativeActivity *a, int *bucket)
{
   JNIEnv *e = act_env(a, m_bucket);
   if (!e)
      return 0;
   jint r = (*e)->CallStaticIntMethod(e, g_platform, m_bucket, a->clazz);
   if (!jb_checked(e, "standbyBucket"))
      return 0; /* 0 from a throw is "ACTIVE", the most reassuring answer
                 * there is, and it would have been shown as a fact */
   if (bucket)
      *bucket = (int)r;
   return 1;
}

int jb_bonded_sensor(JNIEnv *env, jobject clazz, const char *prefix, char *mac,
                     int cap)
{
   if (!env || !g_ble || !m_bonded_sensor || !mac || cap <= 0)
      return 0;
   mac[0] = 0;
   /* CHECKED, NOT JUST NULL-TESTED. NewStringUTF does not merely return NULL
    * when the heap is exhausted -- it leaves an OutOfMemoryError PENDING on
    * this thread, and the next JNI call anybody makes with one pending is
    * undefined behaviour that in practice aborts the VM. Returning 0 here
    * without clearing therefore turns a recoverable allocation failure into a
    * crash in whatever unrelated code makes the next call. */
   jstring jpfx = (*env)->NewStringUTF(env, prefix);
   if (!jb_checked(env, "NewStringUTF(bonded prefix)") || !jpfx)
      return 0;
   jstring jm =
       (*env)->CallStaticObjectMethod(env, g_ble, m_bonded_sensor, clazz, jpfx);
   /* getBondedDevices() throws SecurityException if BLUETOOTH_CONNECT was
    * revoked after pairing; clear it so the caller's NEXT JNI call is not
    * made with an exception pending. */
   if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
   (*env)->DeleteLocalRef(env, jpfx);
   if (!jm)
      return 0;
   /* SAME FOR THE CHARS. GetStringUTFChars answers NULL on OOM with the
    * exception pending; the DeleteLocalRef below is one of the few calls the
    * JNI spec allows with one outstanding, but the caller's next call is not,
    * and this function's contract is that it returns to ordinary C code. */
   const char *bm = (*env)->GetStringUTFChars(env, jm, NULL);
   if (!jb_checked(env, "GetStringUTFChars(bonded mac)"))
      bm = NULL;
   if (bm) {
      str_snapshot(mac, cap, bm);
      (*env)->ReleaseStringUTFChars(env, jm, bm);
   }
   (*env)->DeleteLocalRef(env, jm);
   return mac[0] ? 1 : 0;
}

void jb_show_glucose(JNIEnv *e, jobject ctx, const char *title,
                     const char *text, const char *val, const uint32_t *px,
                     int w, int h, int lockscr)
{
   if (!e || !ctx || !g_platform || !m_show_glucose || !px || w <= 0 || h <= 0)
      return;
   jstring jt    = (*e)->NewStringUTF(e, title);
   jstring js    = (*e)->NewStringUTF(e, text);
   jstring jv    = (*e)->NewStringUTF(e, val);
   jintArray arr = (*e)->NewIntArray(e, w * h);
   /* On OOM any of these is NULL with an exception pending, and
    * SetIntArrayRegion on a NULL array ABORTS THE VM -- so bail and clean up
    * rather than pass one through. */
   if (jt && js && jv && arr) {
      (*e)->SetIntArrayRegion(e, arr, 0, w * h, (const jint *)px);
      (*e)->CallStaticVoidMethod(e, g_platform, m_show_glucose, ctx, jt, js, jv,
                                 arr, (jint)w, (jint)h,
                                 (jint)(lockscr ? 1 : 0));
   }
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   if (jt)
      (*e)->DeleteLocalRef(e, jt);
   if (js)
      (*e)->DeleteLocalRef(e, js);
   if (jv)
      (*e)->DeleteLocalRef(e, jv);
   if (arr)
      (*e)->DeleteLocalRef(e, arr);
}
