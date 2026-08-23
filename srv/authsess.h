/* SPDX-License-Identifier: GPL-3.0
 * authsess.h --- the browser session, and the CSRF token derived from it
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
#ifndef AUTHSESS_H
#define AUTHSESS_H

struct db; /* db.h: every storage call names its database */

#include <stddef.h> /* size_t: the cookie buffer the caller provides */
#include <stdint.h>

/* Sessions. `cookie` gets the full "selector:validator" to hand the browser.
 *
 * AND IT IS ALSO THE SWEEP. Creating a session prunes every EXPIRED session
 * row in the table -- anybody's, no cookie required -- and revokes this
 * account's oldest sessions down to MAX_SESSIONS, all in the same transaction
 * as the insert. 0 means none of it happened and no cookie was written.
 *
 * Before this, the only thing that ever removed a session row was the holder
 * of that exact cookie presenting it after it had expired, so a browser
 * somebody signed in from once and never opened again left a LIVE year-long
 * credential that nothing would ever reach. See the definition for why the
 * sweep lives here rather than on the request path, and proto.h for why the
 * cap is eight. */
int session_new(struct db *d, int64_t uid, char *cookie, size_t cap);

/* ---- READING A SESSION IS TWO OPERATIONS, AND ONLY ONE OF THEM IS A READ --
 *
 * There was one function, `session_user`, and it read like a query: hand it a
 * cookie, get back a user id. It also DELETED the row when the cookie had
 * expired and UPDATED expires_at and last_seen when it had not -- so every
 * page view was a potential write, and the routing layer had to know that in
 * order to gate methods BEFORE calling it (srv/web.c). A name that says
 * `_user` and writes to the database is a fact about the code that lives
 * nowhere except in the memory of whoever last read the definition; the
 * ordering constraint it imposes on the router is invisible at the call site,
 * and getting it wrong means a request the server is about to refuse has
 * already written a row.
 *
 * So there are two functions, and the split is the documentation:
 *
 *   session_verify   asks. Writes NOTHING, ever. Safe on any path, in any
 *                    order, including one that is about to answer 405.
 *   session_refresh  acts on what it found: retires an expired row and rolls
 *                    an active session's expiry forward. Called by the
 *                    request-policy boundary, once the method is allowed.
 *
 * srv/page.c's web_user is that boundary, and it is where the pair is
 * combined. */

/* WHAT A COOKIE TURNED OUT TO BE. Four answers, because the caller does
 * something different with each. */
enum session_check {
   /* No cookie, no row for that selector, or a verifier that does not match.
    * The reader is not signed in, and nothing is wrong. */
   SESSION_NONE = 0,
   /* It authenticates. `uid` and `last_seen` are set. */
   SESSION_OK,
   /* The row exists and has aged out. The cookie is refused, and the row is
    * a dead credential worth retiring -- see session_refresh. */
   SESSION_EXPIRED,
   /* The lookup did not RUN. Not "not signed in": the caller must answer 500
    * rather than show a login form to somebody who is signed in. */
   SESSION_UNAVAILABLE
};

/* PURE. Answers what `cookie` is, and touches nothing.
 *
 * `uid` and `last_seen` (either may be NULL) are written only for
 * SESSION_OK. */
enum session_check session_verify(struct db *d, const char *cookie,
                                  int64_t *uid, int64_t *last_seen);

/* THE WRITE HALF, made explicit at the call site.
 *
 * `what` and `last_seen` are session_verify's answers about this same cookie.
 * SESSION_EXPIRED retires the row; SESSION_OK rolls the expiry forward, at
 * most once a day. Anything else does nothing. Answers nothing, because
 * neither write changes whether the reader is signed in: the expiry test
 * already refused the first, and the second only extends a session that was
 * proven valid. */
void session_refresh(struct db *d, const char *cookie, enum session_check what,
                     int64_t last_seen);

/* ---- WHAT HAPPENED TO THE ONE SESSION ROW --------------------------------
 *
 * THREE ANSWERS, BECAUSE "LOGGED OUT" IS A PROMISE ABOUT THE SERVER.
 *
 * As `void` -- preparing a DELETE, stepping it, and throwing both results
 * away -- a prepare that returns NULL, a step that comes back BUSY, IOERR or
 * CORRUPT, and a delete that removed the row are one outcome: nothing. The
 * /logout route then clears the browser's cookie and redirects
 * to the login form, which is a completed logout as far as the person
 * clicking it can tell.
 *
 * What that looked like to the person affected: they finish on a shared or
 * borrowed machine, click "log out", see the login page, and walk away. The
 * session row is still there, `expires_at` is still a YEAR out, and anyone
 * who copied the cookie -- an extension, a colleague at the same terminal, a
 * backup of the browser profile -- is still signed in as them. Clearing the
 * cookie removes the copy in front of the user and none of the copies that
 * matter. Logging out is a revocation, and a revocation that did not happen
 * must be reported, not painted over with a redirect.
 *
 * The three are kept apart because they call for three different answers:
 *
 *   SESSION_DROP_GONE    the statement ran to SQLITE_DONE and removed a row.
 *                        The cookie is dead server-side; say so.
 *   SESSION_DROP_ABSENT  the statement ran to SQLITE_DONE and matched
 *                        nothing. There is no such session to revoke, so the
 *                        goal is met -- but it is NOT the same event, and a
 *                        caller that wants to notice (two tabs logging out at
 *                        once, a "sign out everywhere" that got there first)
 *                        can. Also what a cookie with no selector in it gets:
 *                        no statement ran, and none needed to.
 *   SESSION_DROP_FAILED  the statement did not complete. The row's fate is
 *                        UNKNOWN, which for a revocation means assume it
 *                        survived. Never report a logout on this.
 *
 * Collapsing ABSENT into GONE would be defensible; collapsing either into
 * FAILED, or FAILED into either, is the bug above. */
enum session_drop {
   SESSION_DROP_GONE,
   SESSION_DROP_ABSENT,
   SESSION_DROP_FAILED
};

enum session_drop session_drop(struct db *d, const char *cookie);
/* Drop EVERY session for this user -- a revocation. 1 when they are gone; 0
 * when the delete did not run, in which case old cookies still sign in and
 * the caller must not report the sessions ended. */
int session_drop_all(struct db *d, int64_t uid);

/* The per-session CSRF token is derived from the selector, so it needs no
 * storage and cannot be forged without the cookie it is derived from. */
void csrf_token(const char *cookie, char *out, size_t cap);
int csrf_ok(const char *cookie, const char *sent);

#endif
