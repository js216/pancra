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

/* TWO LEVELS, one word per entry, direct-mapped: an index (plus one, so a
 * zero word is empty) times TZ_OFF_SPAN, plus the offset biased by
 * TZ_OFF_BIAS to be non-negative. A word is written and read whole, so a
 * reader never sees one entry's key with another's offset. Real offsets lie
 * within +-14 h, well inside the span.
 *
 * THE DAY LEVEL answers for a whole UTC day when the offset at its first and
 * last second agree: no zone changes offset twice within one day, so the
 * offset is then the same throughout. A day whose two ends differ holds a
 * transition and is marked TZ_MIXED; its instants are answered by the
 * quarter-hour level, since every transition falls on a quarter hour. A log
 * screen asks about every record it holds, and the day level keeps that to
 * two questions to Android per day of history. */
#define TZ_SLOTS     256U
#define TZ_DAY_SLOTS 4096U
#define TZ_OFF_SPAN  1048576ULL
#define TZ_OFF_BIAS  524288L
#define TZ_MIXED     (TZ_OFF_SPAN - 1U)
static _Atomic unsigned long long g_tz_shown[TZ_SLOTS];
static _Atomic unsigned long long g_tz_day[TZ_DAY_SLOTS];

/* The value kept in `slot` for `key`, into *val: 1 when it is there. */
static int tz_word_get(_Atomic unsigned long long *slot, unsigned long long key,
                       unsigned long long *val)
{
   const unsigned long long w =
       atomic_load_explicit(slot, memory_order_relaxed);
   if (!w || w / TZ_OFF_SPAN != key)
      return 0;
   *val = w % TZ_OFF_SPAN;
   return 1;
}

static void tz_word_put(_Atomic unsigned long long *slot,
                        unsigned long long key, unsigned long long val)
{
   atomic_store_explicit(slot, (key * TZ_OFF_SPAN) + val, memory_order_relaxed);
}

static int tz_off_ok(long off)
{
   return off > -TZ_OFF_BIAS && off < TZ_OFF_BIAS;
}

long tz_shown_at(JNIEnv *env, long at)
{
   if (at <= 0)
      return tz_off_now();
   unsigned long long v           = 0;
   const unsigned long long day   = ((unsigned long long)at / 86400U) + 1U;
   _Atomic unsigned long long *ds = &g_tz_day[day % TZ_DAY_SLOTS];
   if (!tz_word_get(ds, day, &v)) {
      /* The day's first and last second; never 0, which asks for the current offset. */
      const long d0 = (long)((day - 1U) * 86400U);
      const long a  = tz_offset_at(env, d0 > 0 ? d0 : 1);
      const long b  = tz_offset_at(env, d0 + 86399);
      if (tz_off_ok(a) && tz_off_ok(b)) {
         v = (a == b) ? (unsigned long long)(a + TZ_OFF_BIAS) : TZ_MIXED;
         tz_word_put(ds, day, v);
      } else {
         v = TZ_MIXED;
      }
   }
   if (v != TZ_MIXED)
      return (long)v - TZ_OFF_BIAS;
   const unsigned long long key     = ((unsigned long long)at / 900U) + 1U;
   _Atomic unsigned long long *slot = &g_tz_shown[key % TZ_SLOTS];
   if (tz_word_get(slot, key, &v))
      return (long)v - TZ_OFF_BIAS;
   const long off = tz_offset_at(env, at);
   if (tz_off_ok(off)) {
      const long biased = off + TZ_OFF_BIAS; /* non-negative: tz_off_ok */
      tz_word_put(slot, key, (unsigned long long)biased);
   }
   return off;
}

void tz_refresh(JNIEnv *env)
{
   for (unsigned i = 0; i < TZ_SLOTS; i++)
      atomic_store_explicit(&g_tz_shown[i], 0U, memory_order_relaxed);
   for (unsigned i = 0; i < TZ_DAY_SLOTS; i++)
      atomic_store_explicit(&g_tz_day[i], 0U, memory_order_relaxed);
   tz_set(tz_offset_at(env, 0));
}
