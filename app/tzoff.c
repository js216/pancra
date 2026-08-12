// SPDX-License-Identifier: GPL-3.0
// tzoff.c --- the device's local UTC offset
// Copyright 2026 Jakob Kastelic

/* See tzoff.h. Android is the only thing that knows the device's zone, so
 * this is JNI and nothing else; every failure path leaves g_tz_off as it was
 * rather than inventing an offset, because a wrong offset silently moves
 * every timestamp the app has ever written. */
#include "tzoff.h"
#include <jni.h> /* JNIEnv and friends, directly: this file is JNI */

long g_tz_off; /* seconds east of UTC; 0 until the first refresh */

long tz_offset_at(JNIEnv *env, long at)
{
   long off    = g_tz_off;
   jclass tzc  = (*env)->FindClass(env, "java/util/TimeZone");
   jclass sysc = (*env)->FindClass(env, "java/lang/System");
   if (tzc && sysc) {
      jmethodID get_def = (*env)->GetStaticMethodID(env, tzc, "getDefault",
                                                    "()Ljava/util/TimeZone;");
      jmethodID ctm =
          (*env)->GetStaticMethodID(env, sysc, "currentTimeMillis", "()J");
      jmethodID get_off = (*env)->GetMethodID(env, tzc, "getOffset", "(J)I");
      jobject tz        = (*env)->CallStaticObjectMethod(env, tzc, get_def);
      /* long long, not jlong: jlong is spelled in jni_md.h, which is not a
       * header to include by name, and the ABI is the same 64-bit integer. */
      long long when =
          at ? (long long)at * 1000
             : (long long)(*env)->CallStaticLongMethod(env, sysc, ctm);
      if (tz) /* getDefault can return null; don't call getOffset on it */
         off = (*env)->CallIntMethod(env, tz, get_off, when) / 1000;
      /* Release the local refs. This runs on the per-fingerstick hot path,
       * twice per record, inside jni_notify's frame -- three refs a call
       * adds up even though the frame pop eventually reclaims them. */
      if (tz)
         (*env)->DeleteLocalRef(env, tz);
   }
   if (tzc)
      (*env)->DeleteLocalRef(env, tzc);
   if (sysc)
      (*env)->DeleteLocalRef(env, sysc);
   if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
   return off;
}

void tz_refresh(JNIEnv *env)
{
   g_tz_off = tz_offset_at(env, 0);
}
