/* SPDX-License-Identifier: GPL-3.0
 * auth.h --- who is asking: passwords, sessions, signed app requests
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef AUTH_H
#define AUTH_H

struct db; /* db.h: every storage call names its database */

#include "proto.h" /* struct req, and the protocol constants */

/* ---- auth.c ---------------------------------------------------------- */
/* 1 = hashed, 0 = REFUSED with `out` untouched. The only refusable argument
 * is `iters`: zero is not a cost parameter and is no longer quietly computed
 * as one. See the definition. */
int pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
            uint8_t out[PW_HASH_LEN]);
/* The account's id, or 0. `failed` (may be NULL) is set when the lookup did
 * not run, which is not the same as "no such account". */
long user_by_email(struct db *d, const char *email, int *failed);
int user_create(struct db *d, const char *email, const char *pw, long *uid);
/* Set the password AND sign every session out, in one transaction. 1 only
 * when both are committed; 0 leaves the account exactly as it was.
 *
 * Revocation is not a separate step a caller can forget -- the browser path
 * forgot it and reported success, so a stolen cookie outlived the password it
 * was issued against. See the comment on the definition. */
int user_set_password(struct db *d, long uid, const char *pw);
/* 1 = matches, 0 = does not, -1 = could not be checked (a storage failure,
 * which must not be reported to the user as a wrong password). */
int user_check_password(struct db *d, long uid, const char *pw);
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
int session_new(struct db *d, long uid, char *cookie, size_t cap);
/* The signed-in user, or 0. `failed` (may be NULL) distinguishes the two
 * zeroes: "no such session, or expired" from "the lookup did not run", which
 * the caller must answer 500 to rather than show a login form. */
long session_user(struct db *d, const char *cookie, int *failed);

/* ---- WHAT HAPPENED TO THE ONE SESSION ROW --------------------------------
 *
 * THREE ANSWERS, BECAUSE "LOGGED OUT" IS A PROMISE ABOUT THE SERVER.
 *
 * This was `void`. It prepared a DELETE, stepped it, and threw both results
 * away -- so a prepare that returned NULL, a step that came back BUSY,
 * IOERR or CORRUPT, and a delete that removed the row were one outcome:
 * nothing. The /logout route then cleared the browser's cookie and redirected
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
int session_drop_all(struct db *d, long uid);

/* ---- MINTING AN INVITATION LINK: THREE ANSWERS, NOT TWO ---------------
 *
 * A share token is a credential, so it is minted here with the others, and
 * minting one also PRUNES the table and enforces the owner's quota -- in one
 * transaction, because a cap enforced against rows a half-applied prune left
 * behind is not a cap.
 *
 * "Could not create the link" and "you already have as many as you may" are
 * different sentences to the person pressing the button, and only one of them
 * has anything they can do about it:
 *
 *   SHARE_MINT_OK      the row is committed and `token` holds the hex.
 *   SHARE_MINT_FULL    the owner already holds MAX_LIVE_TOKENS unused links.
 *                      Nothing was minted, nothing was revoked, and the fix
 *                      is the Revoke button on the same page. Collapsing this
 *                      into FAILED would send somebody to look at the server
 *                      for a limit that is working exactly as intended.
 *   SHARE_MINT_FAILED  storage. `token` is untouched and no row exists.
 *
 * `owner` 0 means a plain signup link with no follow attached and no quota to
 * count against (see the definition). `email` may be NULL or "" for a link
 * addressed to nobody in particular. */
enum share_mint { SHARE_MINT_OK, SHARE_MINT_FULL, SHARE_MINT_FAILED };

enum share_mint share_token_mint(struct db *d, long owner, const char *email,
                                 char *token, size_t cap);

/* The per-session CSRF token is derived from the selector, so it needs no
 * storage and cannot be forged without the cookie it is derived from. */
void csrf_token(const char *cookie, char *out, size_t cap);
int csrf_ok(const char *cookie, const char *sent);
/* Signed app requests; the signing string is built in auth.c and mirrored
 * in app/sync.c. Returns the user id or 0. */
long verify_signature(const struct req *r);
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
