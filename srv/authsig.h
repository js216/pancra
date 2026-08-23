/* SPDX-License-Identifier: GPL-3.0
 * authsig.h --- the signed app request, and the nonce that stops a replay
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
#ifndef AUTHSIG_H
#define AUTHSIG_H

struct db; /* db.h: every storage call names its database */

#include "proto.h" /* struct req, NONCE_MAX: the request being judged */

/* ---- SIGNED APP REQUESTS: FOUR ANSWERS, NOT TWO ----------------------
 *
 * The signing string is built in authsig.c and mirrored in app/sync.c.
 *
 * This returned "the user id, or 0", and 0 meant three unrelated things: the
 * signature did not verify, the app-key lookup could not be PREPARED or
 * STEPPED, and the nonce could not be INSERTED. Routing turned all of them
 * into 401.
 *
 * The two database faults are the problem. A correctly paired phone, signing
 * correctly, is told its key is invalid -- and 401 is the one answer it must
 * not retry, because retrying a rejected credential is how an app locks
 * itself out. Meanwhile the operator sees authentication failures rather than
 * a database that cannot be read. The condition is transient and the honest
 * answer is "ask again shortly", which is a 503.
 *
 * The same distinction LOGIN_UNKNOWN makes below, for the same reason: "the
 * store could not answer" is not "the store says no".
 *
 * SIG_REPLAY is separated from SIG_BAD because it is a different event to
 * look at in a log -- a signature that verified and a nonce already spent is
 * a retry or an attack, not a misconfigured key -- and both are still 401 on
 * the wire, which is deliberate: the wire must not distinguish them. */
enum sig_verdict {
   SIG_OK = 0, /* *uid is the caller */
   SIG_BAD,    /* not signed, or the MAC does not verify: 401 */
   SIG_REPLAY, /* verified, but the nonce was already spent: 401 */
   SIG_ERROR   /* the store could not answer: 503, and RETRYABLE */
};

/* WHAT THE CHECK LEARNED, and everything the accept step needs from it. The
 * `now` travels so that the two halves judge one request against ONE instant:
 * re-reading the clock in the second half would let a request pass the skew
 * window and then be recorded under a different second. */
struct sig_check {
   int64_t uid;
   int64_t now;
   char nonce[NONCE_MAX + 1];
};

/* THE CHECK: pure. Parses the Authorization header, looks up the app key and
 * recomputes the MAC. Writes nothing, spends nothing, and may be called any
 * number of times. `*out` is filled only on SIG_OK. */
enum sig_verdict sig_check(const struct req *r, struct sig_check *out);

/* THE ACCEPTANCE: this is where the writes are. Spends the replay nonce and
 * stamps app.last_seen. Call ONLY with a `c` that sig_check answered SIG_OK
 * for -- reaching the nonce insert without a proven MAC lets anybody burn
 * nonces with unsigned noise. */
enum sig_verdict sig_accept(struct db *d, const struct sig_check *c);

/* Check then accept, which is what a route wants: a verified request whose
 * nonce was never spent is a replayable one, so the two are not offered to
 * routing separately. `*uid` is set only on SIG_OK; it is left alone
 * otherwise. */
enum sig_verdict verify_signature(const struct req *r, int64_t *uid);

#endif
