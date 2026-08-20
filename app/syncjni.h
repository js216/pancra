// SPDX-License-Identifier: GPL-3.0
// syncjni.h --- the sync client's transport bridge (API)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_SYNCJNI_H
#define PANCRA_SYNCJNI_H

#include <jni.h>

/* Wire the transport to Ble.syncHttp and register which files sync. Called
 * once, from dexble_register, with the same Ble class. */
void syncjni_wire(JNIEnv *e, jclass ble);
/* Re-read the log paths (they are filled in after the data directory is
 * known, which is later than wiring). */
void syncjni_register_logs(void);

/* The two natives Ble declares. Both BLOCK and are only ever called from
 * Java's push worker. */
/* (sync_report -- the outcome of a sync -- is remote.h's: the module that
 * performs the sync says how it went, and this one merely calls it.) */

/* A number that changes when any synced file changes: the sum of their sizes.
 * The cheap answer to "is there anything to sync", asked before deciding to
 * ask the server the expensive version of the same question. */
long syncjni_state_stamp(void);

/* Ask Java's worker to run a sync. Returns at once, and says whether the
 * request was ACCEPTED: 1 if Java has it, 0 if it was dropped.
 *
 * IT CAN BE DROPPED, and silently was. There is no JNIEnv on a thread that
 * never attached, no class if registration has not run yet, no method if the
 * lookup failed -- and a Java call can throw. Each of those returned without
 * a word to the caller, and because the answer normally arrives later through
 * sync_report(), a dropped request produced no report at all: the scheduler
 * had already advanced its deadlines as though a sync were under way, so the
 * next attempt waited out the SIX-HOUR safety interval. The caller must not
 * commit a schedule to a request that was never made. */
int syncjni_sync_request(void);
void syncjni_pair_request(const char *email, const char *code);

jint syncjni_run(JNIEnv *e, jobject cls);
jint syncjni_pair(JNIEnv *e, jobject cls, jstring email, jstring code);

/* Run a restore on the sync worker. See sync_restore in app/sync.h. */
jint syncjni_restore(JNIEnv *e, jobject cls);

/* Ask the Java worker to run one. Returns immediately. */
void syncjni_restore_request(void);

#endif
