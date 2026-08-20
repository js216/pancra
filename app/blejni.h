// SPDX-License-Identifier: GPL-3.0
// blejni.h --- the app's registration with the Java side
// Copyright 2026 Jakob Kastelic

/* THE TRANSPORT'S REGISTRATION WITH THE JAVA SIDE, kept off bletrans.h.
 *
 * Four calls, and they are a different boundary from the radio operations in
 * bletrans.h: this is how the native side attaches itself to Ble.java (its
 * native methods, its alarm class) and how any thread gets a JNIEnv it is
 * allowed to use. They lived beside the GATT operations, so a file that
 * wanted to close a link had to take <jni.h> and every type it declares with
 * it.
 *
 * IT IS NOT THE ONLY HEADER THAT INCLUDES <jni.h>, and claiming so would be a
 * comment that reads like a rule nothing enforces. ndk.h, jbridge.h,
 * pairing.h and syncjni.h each name JNI types in their own contracts; several
 * files therefore still see <jni.h> transitively through ui.h or tzoff.h.
 * What this header is, is the place the four TRANSPORT-side JNI calls live,
 * so bletrans.h does not have to be a JNI header to describe a GATT write.
 *
 * A JNIEnv IS PER-THREAD. dexble_env() returns one valid on the CALLING
 * thread -- never a stored env from another. That is not a style preference:
 * the reading paths run on binder threads, the service heartbeat on its own,
 * and using the activity's env off the main thread aborts the VM. */
#ifndef PANCRA_BLEJNI_H
#define PANCRA_BLEJNI_H

#include <jni.h>

/* Bind the native methods of the Ble class, using `ctx` (the app Context) as
 * the global reference every later call works from. */
int dexble_register(JNIEnv *env, jclass ble, jobject ctx);
void dexble_set_alarm(JNIEnv *env, jclass alarm_cls);
jobject dexble_ctx(void); /* app Context global ref; outlives the activity */
JNIEnv *dexble_env(void); /* a JNIEnv valid on the CALLING thread */

#endif
