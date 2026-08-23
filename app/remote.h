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
#include "syncstat.h" /* enum sync_outcome: the answer this reports */

#ifndef REMOTE_H
#define REMOTE_H

#include "compiler.h" /* PANCRA_MUST_USE: an answer no caller may drop */

/* Consider syncing NOW: does nothing unless something changed, the backoff
 * has expired and no attempt is already in flight. Called from the SERVICE
 * tick as well as the activity's timer -- the activity's looper dies with the
 * activity, and a phone with the app backgrounded must still deliver its
 * readings -- precisely the window in which points otherwise pile up
 * unsent. */
void pancra_remote_sync(void);

/* A push was ACKNOWLEDGED by the server (HTTP 2xx). Called on
 * Ble.remotePush's worker thread; just timestamps the last success. */
void pancra_remote_ok(void);

/* Try again as soon as the next tick allows, clearing any backoff. For the
 * moment the user fixes what a failure was about -- a server name, a
 * pairing -- so the correction is not hidden behind a schedule the settings
 * they just corrected earned. */
void remote_retry_now(void);

/* ---- DROPPING THE PAIRED IDENTITY --------------------------
 *
 * WHAT IT DOES, AND THE NAME SAYS IT. This DELETES THE PAIRED IDENTITY --
 * the account id and the signing key, out of the settings file -- zeroes the
 * key the sync worker signs with, and resets the retry schedule. Re-pairing
 * needs a fresh code from the server, so it is not an operation a caller
 * should be able to make by accident, and a name has to be consequential
 * enough that a reader of the call site knows what it costs.
 *
 * WHEN IT IS RIGHT: the configured SERVER changed. The identity belongs to
 * the server it was minted against and must not be offered to another one;
 * there is nothing to
 * carry over, because the next sync asks the new server what it holds.
 *
 * DURABILITY. The identity is not gone until the settings file says so. If
 * the file cannot be replaced, NOTHING is dropped -- the id and the key come
 * back whole, because an app that believes it is unpaired while the file
 * still names an account re-pairs into a SECOND account on the next code.
 * That is the IDENTITY_KEPT answer, and it is not a retry that will happen
 * later: the caller has already persisted a new server, so the phone is
 * configured for one server and paired to another until somebody acts.
 *
 * WHICH IS WHY THE ANSWER MAY NOT BE DROPPED. The caller has to tell the
 * user, because only they can re-pair. */
enum identity_drop {
   IDENTITY_DROPPED, /* gone from memory and from the file */
   IDENTITY_KEPT /* the file refused; nothing changed, and it is still ours */
};

PANCRA_MUST_USE enum identity_drop remote_drop_identity(void);

enum sync_outcome remote_outcome(void);
long remote_ok_time(void);

/* What the two above report, published by the push as it finishes. */
void remote_note_ok(long when);
void remote_note_outcome(int outcome);

#endif
