/* SPDX-License-Identifier: GPL-3.0
 * authrate.h --- the login throttle, per address
 * Copyright 2026 Jakob Kastelic
 *
 * ONE CREDENTIAL PER HEADER. A header declaring five unrelated credentials
 * -- the account and its password, the browser session and its CSRF
 * derivative, the invitation link, the signed app request, and the login
 * throttle -- makes every page file inherit all
 * of them whether it touched one or not. They share no types, no tables and
 * no lifetimes; splitting them means a reader of any one of these files has
 * exactly one idea to hold, and a caller's includes say which credentials it
 * actually deals in.
 *
 * The declarations below are UNCHANGED, comments included: this is a move,
 * not a redesign, and the contracts they state were each paid for by an
 * incident.
 */
#ifndef AUTHRATE_H
#define AUTHRATE_H

struct db; /* db.h: every storage call names its database */

#include <stdint.h>

/* IS THIS ADDRESS THROTTLED? THREE ANSWERS, not two.
 *
 * LOGIN_OK        no, let it through
 * LOGIN_THROTTLED yes, refuse with 429
 * LOGIN_UNKNOWN   the counter could not be READ
 *
 * The third is the one that was missing. This returned "not throttled" when
 * db_prep failed and treated every non-row step the same way, so under BUSY,
 * an I/O error or a damaged page the PBKDF2 protection silently disappeared
 * -- exactly when a server is least able to afford an offline-speed password
 * search, and with nothing in the answer to say so. A counter that cannot be
 * read is not a counter that says zero; the caller refuses the request. */
#define LOGIN_OK        0
#define LOGIN_THROTTLED 1
#define LOGIN_UNKNOWN   (-1)

int login_throttled(struct db *d, const char *email);
/* Record a failed attempt / clear the record. Each returns 1 when the write
 * landed: an uncounted failure is an unthrottled guess, and an uncleared one
 * leaves a user who just succeeded closer to a lockout they cannot see. */
int login_failed(struct db *d, const char *email);
int login_ok(struct db *d, const char *email);

#endif
