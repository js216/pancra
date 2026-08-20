// SPDX-License-Identifier: GPL-3.0
// remote.h --- When to talk to the sync server, and what to say about it
// Copyright 2026 Jakob Kastelic
//
/* THE SCHEDULE, NOT THE PROTOCOL. sync.c speaks to the server; this file
 * decides WHEN, and turns the answer into something the screen can show.
 *
 * A phone on mobile data must not push on every reading, and must not hammer
 * a server that is down. So an attempt happens only when something has
 * actually changed (a state stamp, not a timer), a failure doubles a backoff,
 * and a periodic safety look covers the case where the local stamp and the
 * server have drifted apart with nothing new to say.
 *
 * THREE THREADS TOUCH THIS. The attempt is started from the MAIN looper, the
 * HTTP work runs on a Java WORKER thread, and the result comes back on that
 * worker -- so the in-flight gate is a single-flight try-lock rather than a
 * flag, and the "server acknowledged" timestamp is atomic. Without the gate,
 * two overlapping pushes send the same rows twice.
 */
#ifndef REMOTE_H
#define REMOTE_H

/* Consider syncing NOW: does nothing unless something changed, the backoff
 * has expired and no attempt is already in flight. Called from the SERVICE
 * tick as well as the activity's timer -- the activity's looper dies with the
 * activity, and a phone with the app backgrounded must still deliver its
 * readings, which is precisely the window in which points used to pile up
 * unsent. */
void pancra_remote_sync(void);

/* A push was ACKNOWLEDGED by the server (HTTP 2xx). Called on
 * Ble.remotePush's worker thread; just timestamps the last success. */
void pancra_remote_ok(void);

/* The configured SERVER changed: drop the paired identity. There is no cursor
 * to forget any more -- the next sync asks the new server what it has -- but
 * the identity belongs to the old server and must not be offered to another
 * one. */
/* Try again as soon as the next tick allows, clearing any backoff. For the
 * moment the user fixes what a failure was about -- a server name, a
 * pairing -- so the fix is not hidden behind a schedule the old settings
 * earned. */
void remote_retry_now(void);

void remote_forget_cursor(void);

/* HOW THE LAST SYNC ENDED, and WHEN the server last acknowledged a push.
 *
 * Written by the sync worker thread, read by the main thread building a
 * frame. `remote_outcome` is an enum sync_outcome; syncstat.c turns it into
 * words with the lifetime of the process. */
void remote_note_outcome(int outcome);
int remote_outcome(void);
void remote_note_ok(long when);
long remote_ok_time(void);

#endif
