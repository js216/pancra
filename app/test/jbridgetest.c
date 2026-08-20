// SPDX-License-Identifier: GPL-3.0
// jbridgetest.c --- the JNI bind either binds everything or binds nothing
// Copyright 2026 Jakob Kastelic

/* THE BOUNDARY NOTHING COULD REACH.
 *
 * THIS SUITE NOW COVERS FOUR JNI SEQUENCES, not one: the bind below, the app
 * classloader lookup (jb_app_class), the sync transport's argument
 * marshalling (jni_http, reached through the hook syncjni_wire installs) and
 * the BLE native registration (dexble_register). They are one subject, and it
 * is not "JNI": it is THE RULE THAT A JNI CALL MADE AFTER A FAILED ONE IS NOT
 * AN ERROR, IT IS AN ABORT. With CheckJNI on -- and it is on for a debuggable
 * build and for anyone running the app under a debugger -- calling any JNI
 * function with an exception pending, or handing one a null object, produces
 * `JNI DETECTED ERROR IN APPLICATION` and SIGABRT, with the tombstone naming
 * the innocent call rather than the failure before it. So the fake env below
 * MODELS CheckJNI: every entry point that ART would refuse counts itself, and
 * a suite that ends with a nonzero count is describing a phone that aborted.
 *
 * jb_bind looks up every method the app calls on Ble.java and used to look at
 * none of the results: fourteen GetStaticMethodID calls, then `return 1`. A
 * signature drifting on the Java side -- one parameter added, one flag
 * removed -- therefore produced an app that started normally with one dead
 * feature and a NoSuchMethodError left PENDING, which makes the NEXT
 * unrelated JNI call fail for a reason nothing in the C explains. The failure
 * appears only on a phone, only for that one call, and the log points
 * somewhere else.
 *
 * A JNIEnv is a pointer to a table of function pointers, so a test can BE one.
 * The fake below answers every lookup except the k-th, which is exactly the
 * shape of a real signature drift: everything present but one thing.
 *
 * What is pinned:
 *
 *   1. FAILING ANY ONE LOOKUP FAILS THE BIND. Run once per method, refusing a
 *      different one each time -- so a method added later without a check
 *      shows up here as a bind that succeeded when it should not have.
 *   2. THE PARTIAL STATE IS GIVEN BACK. The global ref is deleted, and
 *      jb_class() -- the thing every call site tests -- reports unbound. A
 *      bind that failed and left the class set would say "bound" for ever.
 *   3. THE EXCEPTION IS NOT LEFT PENDING. It is described (so logcat names
 *      the method) and cleared.
 *   4. A SECOND BIND IS A NO-OP. The activity is destroyed and recreated by a
 *      back-press without the process ending, so this runs again; a second
 *      global ref per relaunch is a leak with a hard ceiling behind it.
 *
 * Built and run by `make jbridgetest`.
 */
/* The stubs at the bottom stand in for every module syncjni.c and dexble.c
 * call. They are declared through the REAL headers rather than retyped here,
 * so a signature that moves is a compile error in this file rather than a
 * silent mismatch across the link. */
#include "jbridge.h"
#include "alarm.h"
#include "blejni.h" /* dexble_register / dexble_ctx: the BLE registration */
#include "bondtable.h"
#include "calib.h"
#include "dexdriver.h"
#include "insulin.h"
#include "meter.h"
#include "ndk.h" /* struct ANativeActivity: what the system calls take */
#include "reading.h"
#include "remote.h"
#include "sensors.h"
#include "settings.h" /* struct remote_config: where a sync request goes */
#include "shell.h"
#include "status.h"
#include "store.h"
#include "sync.h" /* sync_http_fn: the transport hook jni_http is installed as */
#include "syncjni.h"
#include "syncreport.h"
#include "syncstat.h"
#include "util.h"
#include "weight.h"
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* A COUNT, NOT A SILENCE. A suite whose success prints nothing cannot be told
 * from a suite that did not run -- and this one is built from four sections
 * that each set up a different fake VM, so "it exited 0" is genuinely
 * ambiguous. Every ck() is counted and the tally is printed at the end. */
static int all = 1;
static int n_checks;
static int n_failed;

static void ck(int cond, const char *what)
{
   n_checks++;
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond) {
      all = 0;
      n_failed++;
   }
}

/* ---- the fake VM ------------------------------------------------------ */

/* Which lookup to refuse (-1 = refuse none), and what the run observed. */
static int f_fail_at = -1;
static int f_lookups;      /* how many methods were asked for */
static int f_globals;      /* NewGlobalRef calls */
static int f_global_freed; /* DeleteGlobalRef calls */
static int f_described;    /* ExceptionDescribe calls */
static int f_cleared;      /* ExceptionClear calls */
static int f_pending;      /* an exception is pending right now */
/* The name of the method the run refused, so the failure can be reported in
 * the terms the reader cares about. */
static const char *f_refused;

/* ---- CheckJNI, MODELLED --------------------------------------------------
 *
 * This is the part of the fake VM that makes the suite worth running. On a
 * phone the consequence of a JNI call made after a failed one is not a return
 * value anybody can inspect -- ART's ScopedCheck refuses it before the call
 * happens and calls JniAbort, which logs `JNI DETECTED ERROR IN APPLICATION`
 * and raises SIGABRT. There is no error path to assert on, and no way to
 * provoke it on the host except by counting the calls ART would have refused.
 *
 * Two refusals matter here, and they are the two the fixed code exists to
 * avoid: entering with an exception pending, and being handed a null object
 * where the VM will dereference one. Every entry point below that ART checks
 * calls jni_enter(); the handful ART explicitly permits with an exception
 * pending -- ExceptionCheck/Describe/Clear, DeleteLocalRef, DeleteGlobalRef
 * -- deliberately do not. */
static int f_abort_pending; /* JNI calls entered with an exception pending */
static int f_abort_null;    /* ...or handed a null object */

static void jni_enter(void)
{
   if (f_pending)
      f_abort_pending++;
}

static void jni_needs(const void *o)
{
   if (!o)
      f_abort_null++;
}

/* ---- FAILING ONE ALLOCATION AT A TIME -----------------------------------
 *
 * f_step_fail_at names WHICH resource-producing call fails, counted from zero
 * across everything that can run out of memory: GetObjectClass, GetMethodID,
 * CallObjectMethod, NewStringUTF, NewByteArray, NewGlobalRef. One index per
 * run, so each case isolates exactly one failure -- which is the only way to
 * tell "this sequence checks step 4" from "this sequence happens to be
 * refused by the check on step 6". */
static int f_step_fail_at = -1;
static int f_steps;

/* A NULL THAT IS NOT AN EXCEPTION, which is the other half of the same rule
 * and the half that needs no memory pressure at all to reach. getClassLoader
 * returns null, without throwing, for a class the bootstrap loader owns; a
 * lookup can answer nothing while the thread stays perfectly clean. A
 * sequence that only ever checked ExceptionCheck would carry that null
 * forward into a call the VM then dereferences. f_null_at names the step that
 * answers this way. */
static int f_null_at = -1;

/* Leaves the VM in the state a real failure leaves it: nothing handed back
 * and (for the OOM kind) an OutOfMemoryError pending. */
static int step_refused(void)
{
   int i = f_steps++;
   if (i == f_null_at)
      return 1; /* nothing handed back, and NOTHING pending */
   if (i != f_step_fail_at)
      return 0;
   f_pending = 1;
   return 1;
}

/* Local references, counted in and out. There is no Java frame to pop around
 * any of the sequences under test -- they run on the native looper and on the
 * sync worker -- so an unfreed local here is an unfreed local for the life of
 * the process, against a table of about 512 with a VM abort behind it. */
static int f_locals;
static int f_locals_freed;

static jobject new_local(void)
{
   f_locals++;
   /* Distinct and non-NULL. Nothing ever dereferences it. */
   return (jobject)(long)(0x1000 + f_locals);
}

static void fake_del_local(JNIEnv *env, jobject o)
{
   (void)env;
   jni_needs(o); /* DeleteLocalRef(NULL) is legal; a double delete is not */
   f_locals_freed++;
}

/* WHICH METHOD AN ID IS. The sync transport calls three different static
 * methods through one CallStaticIntMethod slot, and the answers have to
 * differ -- a syncCode() that returns whatever syncFail() returns cannot
 * express the defect this section is about. So the lookup remembers the name
 * behind each id and the call fakes answer by name. */
#define FAKE_IDS 64
static const char *f_id_name[FAKE_IDS];

static const char *id_name(jmethodID m)
{
   long i = (long)m;
   if (i > 0 && i < FAKE_IDS && f_id_name[i])
      return f_id_name[i];
   return "";
}

/* ---- the classloader lookup's calls ------------------------------------- */

static jclass fake_get_object_class(JNIEnv *env, jobject o)
{
   (void)env;
   jni_enter();
   jni_needs(o);
   if (step_refused())
      return NULL;
   return (jclass)new_local();
}

static jmethodID fake_get_method(JNIEnv *env, jclass c, const char *name,
                                 const char *sig)
{
   (void)env;
   (void)sig;
   jni_enter();
   jni_needs(c);
   if (step_refused()) {
      f_refused = name;
      return NULL;
   }
   return (jmethodID)(long)(0x2000 + f_steps);
}

/* HOW OFTEN JAVA WAS ACTUALLY ENTERED. Not "how many JNI calls were made":
 * the whole claim of items 108 and 110 is that a failed argument stops the
 * sequence BEFORE the method invocation, and only a count of invocations can
 * say whether it did. */
static int f_java_calls;

static jobject fake_call_object(JNIEnv *env, jobject o, jmethodID m, ...)
{
   (void)env;
   jni_enter();
   jni_needs(o);
   jni_needs((const void *)m);
   f_java_calls++;
   if (step_refused())
      return NULL;
   return new_local();
}

/* ---- the sync transport's calls ----------------------------------------- */

/* What Ble.syncHttp SAW. The old defect was not that the call failed; it was
 * that the call happened at all, with a null body or a null header standing
 * in for the bytes that could not be allocated -- and Java reads a null body
 * as "this request has no body" and posts it. */
static int f_http_calls;
static int f_http_null_body;
static int f_http_null_hdr;
static int f_sync_code =
    200; /* the status of the LAST request, as Java keeps it */
static int f_sync_fail;
static jbyte f_reply[64];
static int f_reply_len;

static jobject fake_call_static_object(JNIEnv *env, jclass c, jmethodID m, ...)
{
   (void)env;
   jni_enter();
   jni_needs(c);
   f_java_calls++;
   if (strcmp(id_name(m), "syncHttp") == 0) {
      va_list ap;
      va_start(ap, m);
      (void)va_arg(ap, jstring); /* server */
      (void)va_arg(ap, int);     /* port, promoted */
      (void)va_arg(ap, jstring); /* method */
      (void)va_arg(ap, jstring); /* path */
      jstring hdr    = va_arg(ap, jstring);
      jbyteArray bdy = va_arg(ap, jbyteArray);
      va_end(ap);
      if (!hdr)
         f_http_null_hdr++;
      if (!bdy)
         f_http_null_body++;
      f_http_calls++;
      if (step_refused())
         return NULL;
      return new_local(); /* the byte[] reply */
   }
   if (step_refused())
      return NULL;
   return new_local();
}

static jbyteArray fake_new_byte_array(JNIEnv *env, jsize n)
{
   (void)env;
   (void)n;
   jni_enter();
   if (step_refused())
      return NULL;
   return (jbyteArray)new_local();
}

/* SetByteArrayRegion HAS NO RETURN VALUE. It reports failure by raising
 * ArrayIndexOutOfBoundsException, so the only way to learn the array was not
 * filled is to ask afterwards -- and an unfilled array is a request body of
 * whatever the VM zeroed it to, posted with the exception still pending. */
static int f_setregion_throws;

static void fake_set_byte_region(JNIEnv *env, jbyteArray a, jsize s, jsize n,
                                 const jbyte *b)
{
   (void)env;
   (void)s;
   (void)n;
   (void)b;
   jni_enter();
   jni_needs(a); /* on a NULL array this aborts; it does not throw */
   if (f_setregion_throws)
      f_pending = 1;
}

static jsize fake_array_len(JNIEnv *env, jarray a)
{
   (void)env;
   jni_enter();
   jni_needs(a);
   return (jsize)f_reply_len;
}

static void fake_get_byte_region(JNIEnv *env, jbyteArray a, jsize start,
                                 jsize n, jbyte *out)
{
   (void)env;
   jni_enter();
   jni_needs(a);
   for (jsize i = 0; i < n && start + i < (jsize)sizeof f_reply; i++)
      out[i] = f_reply[start + i];
}

static jstring fake_new_string(JNIEnv *env, const char *utf)
{
   (void)env;
   jni_enter();
   jni_needs(
       utf); /* NewStringUTF(NULL) is legal in JNI, but nothing here does it */
   if (step_refused())
      return NULL;
   return (jstring)new_local();
}

/* ---- the BLE registration's call ---------------------------------------- */

static int f_register_fail;
static int f_registered;

static jint fake_register(JNIEnv *env, jclass c, const JNINativeMethod *m,
                          jint n)
{
   (void)env;
   (void)m;
   jni_enter();
   jni_needs(c);
   if (f_register_fail) {
      /* RegisterNatives raises NoSuchMethodError when a native's signature no
       * longer matches the Java declaration, and returns nonzero. */
      f_pending = 1;
      return JNI_ERR;
   }
   f_registered = (int)n;
   return JNI_OK;
}

/* ---- the JavaVM, so sync_env() can find a thread env -------------------- */

static struct JNIInvokeInterface_ g_vm_fns;
static const struct JNIInvokeInterface_ *g_vm_iface = &g_vm_fns;
static JNIEnv *fake_env(void);

static jint fake_get_env(JavaVM *vm, void **out, jint ver)
{
   (void)vm;
   (void)ver;
   *out = fake_env();
   return JNI_OK;
}

static jint fake_get_java_vm(JNIEnv *env, JavaVM **out)
{
   (void)env;
   jni_enter();
   *out = (JavaVM *)&g_vm_iface;
   return JNI_OK;
}

static jobject fake_new_global(JNIEnv *env, jobject o)
{
   (void)env;
   jni_enter();
   jni_needs(o);
   if (step_refused())
      return NULL; /* the OOM that leaves a pending exception behind it */
   f_globals++;
   return o;
}

static void fake_del_global(JNIEnv *env, jobject o)
{
   (void)env;
   jni_needs(o); /* legal with an exception pending; not legal on NULL */
   f_global_freed++;
}

/* GLOBAL REFS STILL OUT. The number that must not grow across a relaunch. */
static int live_globals(void)
{
   return f_globals - f_global_freed;
}

static jmethodID fake_get_static(JNIEnv *env, jclass c, const char *name,
                                 const char *sig)
{
   (void)env;
   (void)sig;
   jni_enter();
   jni_needs(c);
   int i = f_lookups++;
   if (i == f_fail_at) {
      /* What the VM really does: no id, and a NoSuchMethodError pending. */
      f_refused = name;
      f_pending = 1;
      return NULL;
   }
   /* Any distinct non-NULL id; the app only ever tests it for NULL. Its name
    * is remembered so the call fakes can answer per method. */
   if (i + 1 < FAKE_IDS)
      f_id_name[i + 1] = name;
   return (jmethodID)(long)(i + 1);
}

static jboolean fake_exc_check(JNIEnv *env)
{
   (void)env;
   return f_pending ? JNI_TRUE : JNI_FALSE;
}

static void fake_exc_describe(JNIEnv *env)
{
   (void)env;
   f_described++;
}

static void fake_exc_clear(JNIEnv *env)
{
   (void)env;
   f_pending = 0;
   f_cleared++;
}

/* ---- the JAVA SIDE OF A SYSTEM CALL, which is allowed to throw ----
 *
 * Each records that it ran and then throws if asked to. That is the whole
 * shape this case exists for: a JNI call that throws still RETURNS, with a
 * zeroed result -- so an unchecked caller reads "false", or "bucket 0", as
 * though Java had answered. */
static int f_calls;
static int f_throw_next;

static void fake_call_void(JNIEnv *env, jclass c, jmethodID m, ...)
{
   (void)env;
   (void)m;
   jni_enter();
   jni_needs(c);
   f_java_calls++;
   f_calls++;
   if (f_throw_next)
      f_pending = 1;
}

static jboolean fake_call_bool(JNIEnv *env, jclass c, jmethodID m, ...)
{
   (void)env;
   (void)m;
   jni_enter();
   jni_needs(c);
   f_java_calls++;
   f_calls++;
   if (f_throw_next) {
      f_pending = 1;
      return JNI_FALSE; /* what a throw leaves behind */
   }
   return JNI_TRUE;
}

static jint fake_call_int(JNIEnv *env, jclass c, jmethodID m, ...)
{
   (void)env;
   jni_enter();
   jni_needs(c);
   f_java_calls++;
   f_calls++;
   if (f_throw_next) {
      f_pending = 1;
      return 0; /* 0 is Android's most reassuring bucket, not "unknown" */
   }
   /* THE STATUS OF THE LAST REQUEST, which is exactly what Ble keeps: a
    * static field, answered to whoever asks next, whether or not the asker
    * made a request of its own. */
   if (strcmp(id_name(m), "syncCode") == 0)
      return (jint)f_sync_code;
   if (strcmp(id_name(m), "syncFail") == 0)
      return (jint)f_sync_fail;
   return 30; /* RARE */
}

/* JNIEnv is `const struct JNINativeInterface_ *`, so a JNIEnv* is a pointer to
 * that pointer -- which is why the table is built here and its address is
 * what gets passed in. */
static struct JNINativeInterface_ g_fns;
static const struct JNINativeInterface_ *g_iface = &g_fns;

static JNIEnv *fake_env(void)
{
   return (JNIEnv *)&g_iface;
}

static void fake_reset(int fail_at)
{
   memset(&g_fns, 0, sizeof g_fns);
   g_fns.NewGlobalRef            = fake_new_global;
   g_fns.DeleteGlobalRef         = fake_del_global;
   g_fns.GetStaticMethodID       = fake_get_static;
   g_fns.ExceptionCheck          = fake_exc_check;
   g_fns.ExceptionDescribe       = fake_exc_describe;
   g_fns.ExceptionClear          = fake_exc_clear;
   g_fns.CallStaticVoidMethod    = fake_call_void;
   g_fns.CallStaticBooleanMethod = fake_call_bool;
   g_fns.CallStaticIntMethod     = fake_call_int;
   g_fns.DeleteLocalRef          = fake_del_local;
   g_fns.GetObjectClass          = fake_get_object_class;
   g_fns.GetMethodID             = fake_get_method;
   g_fns.CallObjectMethod        = fake_call_object;
   g_fns.CallStaticObjectMethod  = fake_call_static_object;
   g_fns.NewStringUTF            = fake_new_string;
   g_fns.NewByteArray            = fake_new_byte_array;
   g_fns.SetByteArrayRegion      = fake_set_byte_region;
   g_fns.GetByteArrayRegion      = fake_get_byte_region;
   g_fns.GetArrayLength          = fake_array_len;
   g_fns.RegisterNatives         = fake_register;
   g_fns.GetJavaVM               = fake_get_java_vm;
   memset(&g_vm_fns, 0, sizeof g_vm_fns);
   g_vm_fns.GetEnv = fake_get_env;
   f_calls = f_throw_next = 0;
   f_abort_pending = f_abort_null = 0;
   f_steps                        = 0;
   f_step_fail_at                 = -1;
   f_null_at                      = -1;
   f_setregion_throws             = 0;
   f_locals = f_locals_freed = 0;
   f_java_calls              = 0;
   f_http_calls = f_http_null_body = f_http_null_hdr = 0;
   f_register_fail = f_registered = 0;
   f_fail_at                      = fail_at;
   f_lookups = f_globals = f_global_freed = 0;
   f_described = f_cleared = f_pending = 0;
   f_refused                           = "";
}

/* ---- THE REST OF THE APP, STUBBED ---------------------------------------
 *
 * syncjni.c and dexble.c are linked in whole, because the sequences under
 * test are static functions inside them (jni_http is reached only through the
 * hook syncjni_wire installs) and there is no honest way to test a private
 * JNI sequence except by running the module that owns it. Everything those
 * two modules call that is NOT the JNI boundary is stubbed here: the driver,
 * the logs, the settings, the sync protocol. None of it is the subject, and
 * linking the real thing would drag in the whole app.
 *
 * Declared through the real headers (included at the top), so a signature
 * that changes breaks this file at compile time instead of at link time. */

/* THE HOOK ITSELF, which is the only handle anything has on jni_http:
 * syncjni_wire installs it here, and the test calls it exactly as sync.c
 * would. */
static sync_http_fn g_hook;

void sync_set_http(sync_http_fn fn)
{
   g_hook = fn;
}

/* A CONFIGURED SERVER, because jni_http refuses before any allocation when
 * there is none -- which would make every allocation case below pass for the
 * wrong reason. */
void remote_config_get(struct remote_config *out)
{
   memset(out, 0, sizeof *out);
   out->on   = 1;
   out->port = 443;
   out->uid  = 7;
   memcpy(out->server, "sync.example.org", sizeof "sync.example.org");
}

const char *store_path(void)
{
   return "";
}

const char *insulin_path(void)
{
   return "";
}

const char *weight_path(void)
{
   return "";
}

const char *sensors_path(void)
{
   return "";
}

const char *slots_path(void)
{
   return "";
}

void pancra_logs_reload(void)
{
}

int sync_add_log(const char *name, const char *path, int bucketed)
{
   (void)name;
   (void)path;
   (void)bucketed;
   return 0;
}

void sync_clear_logs(void)
{
}

int sync_key_save(long uid, const unsigned char key[16])
{
   (void)uid;
   (void)key;
   return 0;
}

int sync_outcome_before_reply(int outcome)
{
   (void)outcome;
   return 0;
}

int sync_outcome_of_net(int netfail)
{
   (void)netfail;
   return SYNC_FAILED;
}

int sync_outcome_of_status(int status)
{
   (void)status;
   return SYNC_IDLE;
}

int sync_pair(const char *email, const char *code, uint8_t out_key[16],
              long *out_uid)
{
   (void)email;
   (void)code;
   (void)out_key;
   (void)out_uid;
   return -1;
}

void sync_report(int outcome)
{
   (void)outcome;
}

int sync_restore(void)
{
   return -1;
}

int sync_run(void)
{
   return -1;
}

/* ---- what dexble.c calls, none of which is the JNI boundary ----
 *
 * atomic_replace and record_generation are NOT here: app/util.c is linked in
 * (jbridge.c needs str_snapshot) and owns both. Stubbing them would be a
 * second definition, and the linker says so. */
void bond_state_set(const char *mac, int state)
{
   (void)mac;
   (void)state;
}

void driver_init(void)
{
}

void driver_kick(int link)
{
   (void)link;
}

int driver_link_is_meter(int link)
{
   (void)link;
   return 0;
}

void driver_meter_connect(int link, const char *mac,
                          void (*connect)(int link, const char *mac))
{
   (void)link;
   (void)mac;
   (void)connect;
}

enum driver_after driver_route_connected(int link)
{
   (void)link;
   return DRV_AFTER_NONE;
}

void driver_route_disconnected(int link, int status)
{
   (void)link;
   (void)status;
}

void driver_route_notify(int link, const char *uuid, const unsigned char *d,
                         int n)
{
   (void)link;
   (void)uuid;
   (void)d;
   (void)n;
}

void driver_route_written(int link, const char *uuid, int status)
{
   (void)link;
   (void)uuid;
   (void)status;
}

void driver_start(int link, const char *mac, const char *code)
{
   (void)link;
   (void)mac;
   (void)code;
}

void pancra_alarm_check(void)
{
}

int pancra_backfill(int link, int mg_dl, int trend, int age_s)
{
   (void)link;
   (void)mg_dl;
   (void)trend;
   (void)age_s;
   return 0;
}

void pancra_cal_result(int result, int sensor_id, int mg_dl, unsigned gen)
{
   (void)result;
   (void)sensor_id;
   (void)mg_dl;
   (void)gen;
}

void pancra_devinfo(int link, const char *uuid, const char *val)
{
   (void)link;
   (void)uuid;
   (void)val;
}

int pancra_glucose(int link, int mg_dl, int trend, int age_s)
{
   (void)link;
   (void)mg_dl;
   (void)trend;
   (void)age_s;
   return 0;
}

void pancra_meter_rssi(int rssi)
{
   (void)rssi;
}

void pancra_remote_ok(void)
{
}

void pancra_rssi(int link, int rssi)
{
   (void)link;
   (void)rssi;
}

void set_status(const char *s)
{
   (void)s;
}

void shell_service_tick(void)
{
}

/* Any non-NULL local ref stands in for the class. */
static jclass fake_class(void)
{
   return (jclass)&g_fns;
}

int main(void)
{
   printf("jbridgetest: the JNI bind is all or nothing\n");

   /* ---- 1..3: refuse each method in turn ---- */
   /* The loop ends when the refusal index runs past the last method, which is
    * also how the number of methods is learned -- no second copy of the list
    * here to drift from the one in jbridge.c. */
   int n           = 0;
   int bad_release = 0;
   int bad_pending = 0;
   int bad_class   = 0;
   int bad_desc    = 0;
   for (int k = 0; k < 64; k++) {
      fake_reset(k);
      if (jb_bind(fake_env(), fake_class())) {
         n = k; /* every lookup succeeded: k is past the last method */
         break;
      }
      if (jb_class() != NULL)
         bad_class = 1;
      if (f_global_freed != 1)
         bad_release = 1;
      if (f_pending)
         bad_pending = 1;
      if (!f_described || !f_cleared)
         bad_desc = 1;
   }
   /* n is the first index at which nothing was refused, so every bind before
    * it -- one per method -- returned failure. */
   ck(n > 0, "the bind succeeds once every method is present");
   ck(n >= 13, "...and it really does look up the whole bridge");
   ck(!bad_class, "a refused method leaves the bridge reporting UNBOUND");
   ck(!bad_release, "...and gives its global class reference back");
   ck(!bad_pending, "...and leaves no exception pending for the next call");
   ck(!bad_desc, "...having described it, so logcat names the method");

   /* ---- 4: the bind that succeeded is the one that stuck ---- */
   ck(jb_class() != NULL, "a complete class binds");
   int before = f_globals;
   ck(jb_bind(fake_env(), fake_class()) == 1, "binding again reports success");
   ck(f_globals == before, "...without taking a second global reference");

   /* ---- and the arguments a real onCreate can get wrong ---- */
   ck(jb_bind(NULL, fake_class()) == 0, "no env is not a bind");
   ck(jb_bind(fake_env(), NULL) == 0, "no class is not a bind");

   /* ---- A JAVA CALL THAT THROWS IS A FAILURE, NOT AN ANSWER ---- */
   {
      /* A bound bridge, and a fake activity whose env is the fake one. */
      fake_reset(64);
      ck(jb_bind(fake_env(), fake_class()) == 1, "the bridge binds for the "
                                                 "system-call cases");
      struct ANativeActivity act;
      memset(&act, 0, sizeof act);
      act.env   = fake_env();
      act.clazz = (jobject)&g_fns;

      /* THE HELPER ITSELF. */
      ck(jb_checked(fake_env(), "clean") == 1, "a clean env is not a failure");
      ck(jb_checked(NULL, "noenv") == 0, "no env is a failure");
      f_pending   = 1;
      f_described = f_cleared = 0;
      ck(jb_checked(fake_env(), "thrown") == 0, "a pending exception is one");
      ck(f_described == 1, "...described, so logcat names it");
      ck(f_cleared == 1, "...and cleared, so the VM survives the next call");
      ck(f_pending == 0, "...leaving nothing pending");

      /* A COMMAND: reports whether Java completed it. */
      f_throw_next = 0;
      ck(jb_set_orientation(&act, 1) == 1, "an orientation change that lands "
                                           "reports success");
      f_throw_next = 1;
      ck(jb_set_orientation(&act, 1) == 0, "...and one that throws does not");
      ck(f_pending == 0, "...with the exception cleared either way");

      /* A BOOLEAN QUERY: the cached value must survive a throw. */
      int batt     = 1; /* a previous good reading */
      f_throw_next = 1;
      ck(jb_battery_ok(&act, &batt) == 0, "a throwing query answers nothing");
      ck(batt == 1, "...and leaves the caller's last good value alone");
      f_throw_next = 0;
      ck(jb_battery_ok(&act, &batt) == 1, "an answered query answers");
      ck(batt == 1, "...with what Java said");

      /* AN INT QUERY: 0 from a throw is a real bucket value (ACTIVE), which
       * is exactly why it must not be written. */
      int bucket   = -1;
      f_throw_next = 1;
      ck(jb_standby_bucket(&act, &bucket) == 0, "a throwing bucket query "
                                                "answers nothing");
      ck(bucket == -1, "...and does not write 0 over UNKNOWN");
      f_throw_next = 0;
      ck(jb_standby_bucket(&act, &bucket) == 1, "an answered one answers");
      ck(bucket == 30, "...with the bucket Java reported");
   }

   /* ---- ITEM 110: THE APP CLASSLOADER LOOKUP ------------------------------
    *
    * Six JNI calls in a chain, four of which hand back a local reference, and
    * exactly one of which the old code checked. What the cases below pin is
    * not "it fails when it should": it is that a refused step is the LAST
    * step. The version this replaces would run all seven regardless -- so
    * f_steps, the count of resource-producing calls actually attempted, is
    * the assertion that fails the moment a check is taken out again. */
   {
      printf("  -- jb_app_class: the activity's own loader, checked step by "
             "step\n");
      jobject activity = (jobject)&g_fns; /* any non-NULL object will do */

      fake_reset(64);
      jclass cls = (jclass)(long)0xdead; /* must be overwritten either way */
      ck(jb_app_class(fake_env(), activity, "com.jk.pancra.Ble", &cls) == 1,
         "a loader that answers resolves the class");
      ck(cls != NULL, "...and hands it back");
      ck(f_locals - f_locals_freed == 1,
         "...keeping ONLY it: every intermediate local ref is released");
      ck(f_abort_pending == 0 && f_abort_null == 0,
         "...with no JNI call CheckJNI would have refused");

      /* ONE REFUSED STEP PER RUN. The loop ends at the first index that
       * refuses nothing, which is also how the number of steps is learned --
       * no second copy of the sequence here to drift from jbridge.c. */
      int steps   = 0;
      int bad_out = 0, bad_leak = 0, bad_pending = 0, bad_abort = 0;
      int bad_continued = 0;
      for (int k = 0; k < 16; k++) {
         fake_reset(64);
         f_step_fail_at = k;
         jclass c       = (jclass)(long)0xdead;
         if (jb_app_class(fake_env(), activity, "com.jk.pancra.Ble", &c)) {
            steps = k;
            break;
         }
         if (c != NULL)
            bad_out = 1;
         if (f_locals != f_locals_freed)
            bad_leak = 1;
         if (f_pending)
            bad_pending = 1;
         if (f_abort_pending || f_abort_null)
            bad_abort = 1;
         /* THE ONE THAT MATTERS. k+1 calls attempted means the k-th was the
          * last: nothing was tried after the failure. */
         if (f_steps != k + 1)
            bad_continued = 1;
      }
      ck(steps == 7, "the lookup really is seven separately checked steps");
      ck(!bad_continued,
         "a refused step is the LAST step: nothing is attempted after it");
      ck(!bad_abort, "...so no JNI call is made with an exception pending or "
                     "a null object, which is what CheckJNI aborts on");
      ck(!bad_out, "...and no half-resolved class is handed back");
      ck(!bad_leak, "...and every local ref taken on the way is released");
      ck(!bad_pending, "...and nothing is left pending for the caller");

      /* A STEP THAT ANSWERS NOTHING WITHOUT THROWING, which is the failure
       * here that needs no memory pressure and no signature drift:
       * getClassLoader() returns null, cleanly, for a class the bootstrap
       * loader owns. A sequence that checked only ExceptionCheck would carry
       * that null into GetObjectClass, which the VM dereferences. */
      int bad_null_ret = 0, bad_null_out = 0, bad_null_cont = 0;
      int bad_null_leak = 0, bad_null_abort = 0;
      for (int k = 0; k < 7; k++) {
         fake_reset(64);
         f_null_at = k;
         jclass c  = (jclass)(long)0xdead;
         if (jb_app_class(fake_env(), activity, "com.jk.pancra.Ble", &c))
            bad_null_ret = 1;
         if (c != NULL)
            bad_null_out = 1;
         if (f_steps != k + 1)
            bad_null_cont = 1;
         if (f_locals != f_locals_freed)
            bad_null_leak = 1;
         if (f_abort_null || f_abort_pending)
            bad_null_abort = 1;
      }
      ck(!bad_null_ret,
         "a step that answers null WITHOUT throwing is still a failure");
      ck(!bad_null_cont, "...and is still the last step attempted");
      ck(!bad_null_abort, "...so no null object is handed to the next call, "
                          "which is the other thing CheckJNI aborts on");
      ck(!bad_null_out, "...and no class is handed back");
      ck(!bad_null_leak, "...and every local ref taken is released");

      /* The arguments a real onCreate can get wrong. */
      fake_reset(64);
      cls = (jclass)(long)0xdead;
      ck(jb_app_class(NULL, activity, "x", &cls) == 0, "no env is no lookup");
      ck(cls == NULL, "...and the out-param is cleared even so");
      cls = (jclass)(long)0xdead;
      ck(jb_app_class(fake_env(), NULL, "x", &cls) == 0,
         "no activity is no lookup");
      ck(cls == NULL, "...and the out-param is cleared even so");
      ck(jb_app_class(fake_env(), activity, NULL, &cls) == 0,
         "no class name is no lookup");
      ck(jb_app_class(fake_env(), activity, "x", NULL) == 0,
         "nowhere to put the answer is no lookup");
      ck(f_steps == 0, "...none of which touches the VM at all");
   }

   /* ---- ITEM 109: THE BLE NATIVE REGISTRATION -----------------------------
    *
    * Two global references, and a global reference is for the life of the
    * PROCESS: one leaked here is never recovered, and the table it lives in
    * has a ceiling with a VM abort behind it. The old code assigned both
    * straight into the file-scope globals before checking either, and kept
    * them when RegisterNatives failed.
    *
    * The order of these cases is deliberate: the first one runs before any
    * registration has ever succeeded, so dexble_ctx() starting NULL is a
    * fact, not an assumption. */
   {
      printf("  -- dexble_register: the global pair is published only by a "
             "registration that completed\n");
      jclass ble  = fake_class();
      jobject ctx = (jobject)&g_iface; /* a distinct non-NULL object */

      ck(dexble_ctx() == NULL,
         "nothing is published before the first registration");

      /* THE CLASS REF ITSELF FAILS. The next line used to call NewGlobalRef
       * again with the OutOfMemoryError pending -- the abort. */
      fake_reset(64);
      f_step_fail_at = 0;
      ck(dexble_register(fake_env(), ble, ctx) == 0,
         "a class reference that cannot be taken is not a registration");
      ck(dexble_ctx() == NULL, "...and publishes no Context");
      ck(live_globals() == 0, "...and leaves no global reference out");
      ck(f_abort_pending == 0,
         "...and does not take the second reference with the OOM pending");
      ck(f_registered == 0, "...and binds no natives");
      ck(f_pending == 0, "...and leaves nothing pending for the caller");

      /* THE CONTEXT REF FAILS, one step later. This is the leak case: the
       * class ref HAS been taken by now, and the old code kept it in g_ble
       * for ever. */
      fake_reset(64);
      f_step_fail_at = 1;
      ck(dexble_register(fake_env(), ble, ctx) == 0,
         "a Context reference that cannot be taken is not a registration");
      ck(dexble_ctx() == NULL, "...and publishes nothing");
      ck(live_globals() == 0,
         "...and gives back the class reference taken a moment earlier");
      ck(f_registered == 0, "...and binds no natives");

      /* REGISTRATION ITSELF FAILS. Both refs exist and both must go back --
       * and bondWatch/startService must not be called on a class whose
       * natives are not bound. */
      fake_reset(64);
      f_register_fail = 1;
      ck(dexble_register(fake_env(), ble, ctx) == 0,
         "a RegisterNatives that fails is not a registration");
      ck(dexble_ctx() == NULL, "...and publishes no Context");
      ck(live_globals() == 0, "...and returns BOTH global references");
      ck(f_java_calls == 0,
         "...and never calls bondWatch or startService on an unbound class");
      ck(f_pending == 0, "...having cleared what RegisterNatives raised");

      /* THE RETRY, which is how this is reached in the first place: init_java
       * runs again when the activity is recreated in the same process. */
      fake_reset(64);
      ck(dexble_register(fake_env(), ble, ctx) == 1,
         "a complete registration succeeds after a failed one");
      ck(dexble_ctx() != NULL, "...and NOW publishes the Context");
      ck(f_registered > 0, "...having bound the natives");
      ck(f_abort_pending == 0 && f_abort_null == 0,
         "...with nothing CheckJNI would have refused");

      /* THE RELAUNCH. Captured BEFORE the second call, because the number
       * that must not grow is the one the call could grow. */
      int live_before = live_globals();
      ck(dexble_register(fake_env(), ble, ctx) == 1,
         "registering again -- an activity relaunch -- still succeeds");
      ck(live_globals() == live_before,
         "...without leaking the previous global pair");

      /* The arguments a real init_java can get wrong. */
      ck(dexble_register(NULL, ble, ctx) == 0, "no env is no registration");
      ck(dexble_register(fake_env(), NULL, ctx) == 0,
         "no class is no registration");
      ck(dexble_register(fake_env(), ble, NULL) == 0,
         "no Context is no registration");
   }

   /* ---- ITEM 108: THE SYNC TRANSPORT'S ARGUMENTS --------------------------
    *
    * jni_http is static inside syncjni.c and reachable only as the hook
    * syncjni_wire hands to sync.c -- which is exactly how the test gets at
    * it, so nothing had to be exported to make this testable. */
   {
      printf("  -- jni_http: the sync transport enters Java only with "
             "arguments it actually built\n");
      char out[64];

      fake_reset(64);
      syncjni_wire(fake_env(), fake_class());
      ck(g_hook != NULL, "wiring the transport installs the request hook");
      ck(f_abort_pending == 0 && f_abort_null == 0,
         "...with nothing CheckJNI would have refused");

      /* RE-WIRING RELEASES THE OLD CLASS REF. dexble_register calls this, and
       * init_java calls that again on every activity relaunch. */
      fake_reset(64);
      syncjni_wire(fake_env(), fake_class());
      ck(live_globals() == 0,
         "re-wiring the transport releases the previous class reference");

      /* THE HAPPY PATH, so the failure cases below are known to be failing
       * for their own reason and not because nothing works. */
      fake_reset(64);
      f_sync_code = 200;
      f_reply_len = 3;
      memcpy(f_reply, "ok\n", 3);
      memset(out, 'x', sizeof out);
      ck(g_hook("POST", "/v1/push", "Authorization: x\r\n", "body", 4, out,
                (int)sizeof out) == 200,
         "a request whose arguments all built reports Java's status");
      ck(f_http_calls == 1, "...having entered Java exactly once");
      ck(f_http_null_body == 0 && f_http_null_hdr == 0,
         "...with a real body and a real header");
      ck(f_locals == f_locals_freed, "...and every local ref given back");
      ck(strcmp(out, "ok\n") == 0, "...and the reply copied out");
      ck(f_abort_pending == 0 && f_abort_null == 0,
         "...with nothing CheckJNI would have refused");

      /* EACH ALLOCATION, FAILED ON ITS OWN: the server name, the method, the
       * path, the header, the body array. Five runs, one failure each, so
       * every case isolates one allocation rather than one of them standing
       * in for the rest. */
      int bad_entered = 0, bad_rc = 0, bad_leak = 0, bad_abort = 0;
      int bad_pending = 0, bad_out = 0, bad_continued = 0, bad_stale = 0;
      for (int k = 0; k < 5; k++) {
         fake_reset(64);
         /* A PREVIOUS REQUEST THAT REALLY DID SUCCEED, still sitting in
          * Ble's sSyncCode field where syncCode() will hand it to whoever
          * asks next. */
         f_sync_code    = 200;
         f_reply_len    = 3;
         f_step_fail_at = k;
         memset(out, 'x', sizeof out);
         int r = g_hook("POST", "/v1/push", "Authorization: x\r\n", "body", 4,
                        out, (int)sizeof out);
         if (f_http_calls)
            bad_entered = 1;
         if (r != -1)
            bad_rc = 1;
         if (r == 200)
            bad_stale = 1;
         if (f_locals != f_locals_freed)
            bad_leak = 1;
         if (f_abort_pending || f_abort_null)
            bad_abort = 1;
         if (f_pending)
            bad_pending = 1;
         if (out[0] != '\0')
            bad_out = 1;
         if (f_steps != k + 1)
            bad_continued = 1;
      }
      ck(!bad_entered,
         "an allocation that failed stops the request BEFORE Java is entered");
      ck(!bad_continued,
         "...and nothing further is allocated after the failed one");
      ck(!bad_rc, "...and the caller is told the request could not be made");
      ck(!bad_stale, "...never the PREVIOUS request's 200, which sync.c would "
                     "read as an upload it never made being accepted");
      ck(!bad_leak, "...with every local ref already taken given back");
      ck(!bad_abort, "...and no JNI call CheckJNI would have refused");
      ck(!bad_pending, "...and nothing left pending for the next call");
      ck(!bad_out, "...and no stale reply left in the caller's buffer");

      /* THE SAME ARGUMENTS, WITHOUT A BODY. With a body present, a header
       * that failed to allocate is caught a moment later by the body fill's
       * own check -- so that loop cannot tell the header's check from its
       * neighbour's. A request that has no body puts the header last, and
       * then only the header's own check stands between a failed allocation
       * and a request posted with no Authorization line at all. */
      int bad2_entered = 0, bad2_hdr = 0, bad2_rc = 0, bad2_cont = 0;
      for (int k = 0; k < 4; k++) {
         fake_reset(64);
         f_sync_code    = 200;
         f_step_fail_at = k;
         memset(out, 'x', sizeof out);
         int r = g_hook("GET", "/v1/pull", "Authorization: x\r\n", NULL, 0, out,
                        (int)sizeof out);
         if (f_http_calls)
            bad2_entered = 1;
         if (f_http_null_hdr)
            bad2_hdr = 1;
         if (r != -1)
            bad2_rc = 1;
         if (f_steps != k + 1)
            bad2_cont = 1;
      }
      ck(!bad2_entered, "a bodyless request also stops before Java when an "
                        "argument could not be built");
      ck(!bad2_hdr, "...so a header that failed is never passed to Java as "
                    "'this request has no header'");
      ck(!bad2_rc, "...and the request is reported as not made");
      ck(!bad2_cont, "...with nothing allocated after the failed one");

      /* THE FILL THAT THREW. NewByteArray answered, so every null test in
       * the sequence passes -- and the array is still not the body. */
      fake_reset(64);
      f_sync_code        = 200;
      f_setregion_throws = 1;
      memset(out, 'x', sizeof out);
      ck(g_hook("POST", "/v1/push", "Authorization: x\r\n", "body", 4, out,
                (int)sizeof out) == -1,
         "a body the array could not be filled with is not a request");
      ck(f_http_calls == 0,
         "...so Java is not entered with an unfilled array and a pending "
         "exception");
      ck(f_locals == f_locals_freed, "...and every local ref is given back");
      ck(f_pending == 0, "...and the exception is cleared");
      ck(out[0] == '\0', "...leaving no stale reply behind");

      /* NO HEADER AND NO BODY IS A LEGITIMATE REQUEST, and Java expects null
       * for both. Without this case the rule above reads as "never pass
       * null", which would be wrong -- and a refusal here would be invisible
       * in a suite that only failed allocations. */
      fake_reset(64);
      f_sync_code = 204;
      f_reply_len = 0;
      memset(out, 'x', sizeof out);
      ck(g_hook("GET", "/v1/x", NULL, NULL, 0, out, (int)sizeof out) == 204,
         "a request that legitimately has no header and no body is still made");
      ck(f_http_calls == 1, "...entering Java once");
      ck(f_http_null_hdr == 1 && f_http_null_body == 1,
         "...with the nulls Java reads as 'there are none'");
      ck(out[0] == '\0', "...and an empty reply terminated");
      ck(f_locals == f_locals_freed, "...and no local ref left over");

      /* A TRANSPORT THAT COULD NOT BE WIRED refuses before the method lookups
       * rather than making six of them with an OutOfMemoryError pending. */
      fake_reset(64);
      f_step_fail_at = 0;
      syncjni_wire(fake_env(), fake_class());
      ck(f_steps == 1,
         "a class reference that fails stops the wiring at that step");
      ck(f_abort_pending == 0,
         "...so no method lookup is made with the OOM pending");
      ck(f_pending == 0, "...and the exception is cleared");
      ck(live_globals() == 0, "...and no global reference is left out");
   }

   printf("jbridgetest: %d checks, %d failed\n", n_checks, n_failed);
   printf("%s\n", all ? "jbridgetest: a partial bridge is never reported "
                        "bound, and no JNI sequence enters Java on a "
                        "failed argument"
                      : "jbridgetest: FAILED");
   return all ? 0 : 1;
}
