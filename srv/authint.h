/* SPDX-License-Identifier: GPL-3.0
 * authint.h --- what the auth files share, and nothing else
 * Copyright 2026 Jakob Kastelic
 *
 * PRIVATE TO srv/auth*.c. Not included by anything else; everything outside
 * those files reaches this behaviour through the public entry points.
 *
 * Two things genuinely span the credential files, and they are here because
 * they are the only two:
 *
 *   THE MAINTENANCE REPORTER. Three writes in this module are housekeeping
 *   rather than authentication -- the rolling session expiry, the nonce
 *   window prune, the app last_seen stamp -- and all three are best-effort:
 *   the request they ride on succeeds whether or not they do. Best-effort
 *   plus silent is how a card that has stopped taking writes serves a
 *   perfectly healthy-looking site for a week, so they are counted and
 *   reported instead, and the counting has to work the same way in all three
 *   or the log is three different kinds of evidence.
 *
 * Nothing else is shared. The password KDF is the user file's alone, the
 * cookie split is the session file's, and the throttle's SQL is nobody
 * else's business.
 */
#ifndef AUTHINT_H
#define AUTHINT_H

#include <stdatomic.h> /* the counters are read and written by the pool */
#include <stdint.h>

struct db;           /* db.h: every storage call names its database */
struct sqlite3_stmt; /* sqlite3.h: the statement the caller prepared */

/* Say whether a maintenance write worked, counting a run of failures so the
 * log can distinguish one bad moment from a card that is gone. `fails` is
 * that write's own counter -- one per site, function-static at the call. */
void auth_maint_report(struct db *d, const char *what, int ok,
                       _Atomic uint64_t *fails);

/* Step and finalize one maintenance statement, reporting either outcome.
 * A NULL statement is a prepare that already failed and is counted as one. */
int auth_maint_step(struct db *d, struct sqlite3_stmt *st, const char *what,
                    _Atomic uint64_t *fails);

#endif
