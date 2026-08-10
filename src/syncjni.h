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
/* Report the outcome of a sync or a pairing to the UI: the LAST SYNC and LAST
 * STATUS rows, and a repaint. Defined in main.c, which owns that state.
 * `ok` stamps the success time; `what` is a short phrase for the status row.
 *
 * Without this the rows are dead text: they used to be fed by the old push
 * path, and when that went, nothing replaced it -- so a working sync and a
 * server that refused every request looked exactly alike. */
void sync_report(int ok, const char *what);

/* A number that changes when any synced file changes: the sum of their sizes.
 * The cheap answer to "is there anything to sync", asked before deciding to
 * ask the server the expensive version of the same question. */
long syncjni_state_stamp(void);

/* Ask Java's worker to run a sync / a pairing. Return at once. */
void syncjni_sync_request(void);
void syncjni_pair_request(const char *email, const char *code);

jint syncjni_run(JNIEnv *e, jobject cls);
jint syncjni_pair(JNIEnv *e, jobject cls, jstring email, jstring code);

#endif
