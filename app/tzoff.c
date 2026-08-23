// SPDX-License-Identifier: GPL-3.0
// tzoff.c --- the device's local UTC offset
// Copyright 2026 Jakob Kastelic

/* See tzoff.h. Android is the only thing that knows the device's zone, so
 * this is JNI and nothing else; every failure path leaves g_tz_off untouched
 * rather than inventing an offset, because a wrong offset silently moves
 * every timestamp the app has ever written. */
#include "tzoff.h"
#include <jni.h>       /* JNIEnv and friends, directly: this file is JNI */
#include <stdatomic.h> /* the offset crosses threads: see tzoff.h */

/* PRIVATE, and reached through tz_off_now()/tz_set() below. See tzoff.h for
 * why it is atomic and why relaxed is the right ordering. */
static _Atomic long g_tz_off;

long tz_off_now(void)
{
   return atomic_load_explicit(&g_tz_off, memory_order_relaxed);
}

static void tz_set(long off)
{
   atomic_store_explicit(&g_tz_off, off, memory_order_relaxed);
}

/* DID THAT CALL COMPLETE? 1 clean, 0 it threw -- and then the exception is
 * cleared, because leaving one pending makes the NEXT JNI call on this thread
 * undefined and in practice aborts the VM.
 *
 * A LOCAL COPY OF jb_checked ON PURPOSE, and it is three lines rather than a
 * dependency: this file is small enough to link on its own for the sake of
 * tz_offset_at, and reaching for jbridge.c would pull the whole Java bridge,
 * the Ble class handle and the logger in behind it. What is duplicated is
 * "check and clear", which is the JNI contract itself rather than a decision
 * this app made. */
static int tz_ok(JNIEnv *env)
{
   if (!(*env)->ExceptionCheck(env))
      return 1;
   (*env)->ExceptionClear(env);
   return 0;
}

long tz_offset_at(JNIEnv *env, long at)
{
   long off = tz_off_now();
   /* EVERY STEP CHECKED BEFORE THE NEXT ONE IS MADE.
    *
    * Looking up three methods, then calling one of them, and clearing at the
    * very end does not work here. Each lookup can fail -- a stripped or
    * shimmed runtime, a class the loader will not give -- and a failed one
    * answers NULL with an exception pending. CallStaticObjectMethod is then
    * called with a NULL jmethodID AND an exception outstanding, which is
    * doubly undefined and aborts the process; the trailing clear tidies up
    * after a crash that has already happened.
    *
    * Every failure returns the offset UNCHANGED, which is this file's rule:
    * an invented offset silently moves every timestamp the app has written,
    * so "we could not ask" must never become "the answer is UTC". */
   jclass tzc = (*env)->FindClass(env, "java/util/TimeZone");
   if (!tz_ok(env) || !tzc)
      return off;
   jclass sysc = (*env)->FindClass(env, "java/lang/System");
   if (!tz_ok(env) || !sysc) {
      (*env)->DeleteLocalRef(env, tzc);
      return off;
   }

   jmethodID get_def = (*env)->GetStaticMethodID(env, tzc, "getDefault",
                                                 "()Ljava/util/TimeZone;");
   jmethodID ctm =
       (*env)->GetStaticMethodID(env, sysc, "currentTimeMillis", "()J");
   jmethodID get_off = (*env)->GetMethodID(env, tzc, "getOffset", "(J)I");
   /* One verdict for the three lookups: they are asked back to back and any
    * failure among them means the same thing -- this runtime is not one this
    * function can ask. tz_ok is called once because each lookup clears the
    * previous exception state on the way past, so what matters is whether the
    * last one left anything and whether all three ids are real. */
   if (!tz_ok(env) || !get_def || !ctm || !get_off) {
      (*env)->DeleteLocalRef(env, sysc);
      (*env)->DeleteLocalRef(env, tzc);
      return off;
   }

   jobject tz = (*env)->CallStaticObjectMethod(env, tzc, get_def);
   if (!tz_ok(env) || !tz) { /* getDefault can also simply return null */
      if (tz)
         (*env)->DeleteLocalRef(env, tz);
      (*env)->DeleteLocalRef(env, sysc);
      (*env)->DeleteLocalRef(env, tzc);
      return off;
   }

   /* long long, not jlong: jlong is spelled in jni_md.h, which is not a
    * header to include by name, and the ABI is the same 64-bit integer. */
   long long when = (long long)at * 1000;
   if (!at) {
      when = (long long)(*env)->CallStaticLongMethod(env, sysc, ctm);
      if (!tz_ok(env))
         when = 0; /* and the getOffset below is then not worth making */
   }
   if (when) {
      int ms = (*env)->CallIntMethod(env, tz, get_off, when);
      if (tz_ok(env))
         off = ms / 1000;
   }

   /* Release the local refs. This runs on the per-fingerstick hot path,
    * twice per record, inside jni_notify's frame -- three refs a call
    * adds up even though the frame pop eventually reclaims them. */
   (*env)->DeleteLocalRef(env, tz);
   (*env)->DeleteLocalRef(env, sysc);
   (*env)->DeleteLocalRef(env, tzc);
   return off;
}

void tz_refresh(JNIEnv *env)
{
   tz_set(tz_offset_at(env, 0));
}
