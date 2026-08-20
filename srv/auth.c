/* SPDX-License-Identifier: GPL-3.0
 * auth.c --- who is asking: passwords, cookies, and signed app requests
 * Copyright 2026 Jakob Kastelic
 *
 * Two entirely separate ways in, on purpose:
 *
 *   the browser   a password once, then a year-long split cookie
 *   the app       no password ever; a MAC over every request, keyed by the
 *                 secret J-PAKE derived at pairing
 *
 * They never mix. The app cannot log into the web interface and a browser
 * cannot push data, so a stolen cookie cannot corrupt the record and a stolen
 * phone cannot read someone else's.
 */
#include "auth.h"
#include "ct.h"
#include "db.h"
#include "hmac.h"
#include "pbkdf2.h"
#include "proto.h"
#include "sigstr.h"
#include "util.h"
#include <sqlite3.h>
#include <stdatomic.h> /* the maintenance counters are shared by the pool */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The default must itself be inside the range every stored count is checked
 * against, or new accounts would be unreadable the moment they were made. */
_Static_assert(PW_ITERS_DEFAULT >= PW_ITERS_MIN &&
                   PW_ITERS_DEFAULT <= PW_ITERS_MAX,
               "PW_ITERS_DEFAULT is outside the supported range");

/* The two fixed-size arguments are pinned against the KDF's own limits here,
 * so the only way pw_hash can be refused is an iteration count -- and
 * PW_ITERS_MIN > 0 means even that cannot happen for a count this program
 * wrote. Both are compile-time facts rather than comments, because the whole
 * point of the change below is that a substitution must not be possible in
 * silence. */
_Static_assert(PW_SALT_LEN <= PBKDF2_SALT_MAX,
               "the password salt must not need truncating");
_Static_assert(PW_HASH_LEN > 0, "a zero-length password hash is not a hash");
_Static_assert(PW_ITERS_MIN > 0, "an iteration count of zero is not a cost");

/* 1, or 0 with `out` UNTOUCHED.
 *
 * It used to return void, and the KDF underneath used to substitute rather
 * than refuse: a stored iteration count of 0 was computed as 1, and a salt
 * longer than the KDF's scratch was cut short. Neither could be reported, so
 * the visible symptom of either would have been a login that worked against a
 * hash weaker than the row claimed, or -- once the substitution stopped
 * matching, say after the salt length changed -- an account whose correct
 * password stopped being accepted with nothing anywhere to explain it. */
int pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
            uint8_t out[PW_HASH_LEN])
{
   return pbkdf2_sha256((const uint8_t *)pw, strlen(pw), salt, PW_SALT_LEN,
                        (unsigned)iters, out, PW_HASH_LEN) == PBKDF2_OK;
}

/* THE ACCOUNT'S ID, or 0 -- and `failed` (may be NULL) separates the two
 * zeroes: no such account, versus a lookup that did not run. They used to be
 * the same answer, so a database that could not be read told the account
 * holder their email or password was wrong. */
long user_by_email(struct db *d, const char *email, int *failed)
{
   if (failed)
      *failed = 0;
   sqlite3_stmt *st = db_prep(d, "SELECT id FROM user WHERE email=?");
   if (!st) {
      if (failed)
         *failed = 1;
      return 0;
   }
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   long id = 0;
   int rc  = sqlite3_step(st);
   if (rc == SQLITE_ROW)
      id = (long)sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   /* THE STEP, not just the prepare: a damaged page fails here, with the
    * statement perfectly well prepared, and "no row" is exactly what a wrong
    * email looks like. */
   if (rc != SQLITE_ROW && !db_finished(rc)) {
      if (failed)
         *failed = 1;
      return 0;
   }
   return id;
}

int user_create(struct db *d, const char *email, const char *pw, long *uid)
{
   /* No length rule beyond "there is one". A minimum stops nobody who wants a
    * weak password -- "password1" cleared the old eight -- and it does turn
    * away a passphrase somebody chose deliberately. Empty is still refused,
    * because that is not a short password, it is no password. */
   if (!pw || !*pw)
      return 0;
   /* THE ADDRESS MUST ALREADY BE CANONICAL. This is a backstop, not the check:
    * every caller runs email_canon first (see util.h) and passes the result.
    * It is here because an account is the one thing in this program that
    * CANNOT be undone by the person it hurts -- there is no password reset --
    * and the old rule was "contains an '@' somewhere", which admitted "@" and
    * admitted a 900-byte address that /login refuses to look up at all. A row
    * like that is an account whose owner can never sign in again, and nothing
    * about it is visible until they try.
    *
    * Comparing against the canonical form rather than re-deriving it is
    * deliberate: substituting a corrected address for the one the caller asked
    * for would create an account under an address the caller never named, and
    * the caller's throttle and lookup would still be about the other one. */
   char want[EMAIL_BUF];
   if (!email || !email_canon(email, want, sizeof want) ||
       strcmp(want, email) != 0)
      return 0;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   /* NO ACCOUNT AT ALL beats an account whose pw_hash column holds whatever
    * was on the stack: that row would be unloginnable and would look exactly
    * like a forgotten password. */
   if (!pw_hash(pw, salt, PW_ITERS_DEFAULT, hash))
      return 0;
   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO user(email,pw_salt,pw_hash,pw_iters,created_at)"
                  " VALUES(?,?,?,?,?)");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   sqlite3_bind_blob(st, 2, salt, sizeof salt, SQLITE_STATIC);
   sqlite3_bind_blob(st, 3, hash, sizeof hash, SQLITE_STATIC);
   sqlite3_bind_int(st, 4, PW_ITERS_DEFAULT);
   sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (ok && uid)
      *uid = db_last_id(d);
   return ok;
}

/* THE PASSWORD AND THE SESSIONS ARE ONE CHANGE, and this is the only way to
 * make it.
 *
 * A password change that leaves the old cookies working is not a password
 * change: the usual reason to want one is that somebody else may be signed
 * in, and a session cookie is a bearer token that outlives the secret it was
 * issued against. The CLI knew this and dropped the sessions itself; the
 * BROWSER path called this function and then reported "Password changed."
 * having revoked nothing -- so the one path a worried user actually reaches
 * was the one that did not shut anybody out.
 *
 * Making revocation the caller's second step is what allowed that, so it is
 * not a step any more. Both happen inside ONE transaction: half of this
 * applied is worse than neither, because a new password with the old sessions
 * still live is exactly the state the user believes they have just left.
 *
 * Returns 1 only when both are committed. */
int user_set_password(struct db *d, long uid, const char *pw)
{
   if (!pw || !*pw)
      return 0;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   /* BEFORE the transaction opens: a refusal here leaves the account exactly
    * as it was, which is what this function promises. */
   if (!pw_hash(pw, salt, PW_ITERS_DEFAULT, hash))
      return 0;

   /* IMMEDIATE, so the write lock is taken now rather than at the first
    * write: a deferred transaction that cannot upgrade fails at COMMIT, and
    * by then the caller has already been told which parts worked. */
   if (!db_durable_begin(d))
      return 0;

   sqlite3_stmt *st =
       db_prep(d, "UPDATE user SET pw_salt=?,pw_hash=?,pw_iters=? WHERE id=?");
   if (!st) {
      db_durable_rollback(d);
      return 0;
   }
   sqlite3_bind_blob(st, 1, salt, sizeof salt, SQLITE_STATIC);
   sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
   sqlite3_bind_int(st, 3, PW_ITERS_DEFAULT);
   sqlite3_bind_int64(st, 4, uid);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok) {
      db_durable_rollback(d);
      return 0;
   }

   if (!session_drop_all(d, uid)) {
      db_durable_rollback(d); /* the old password stays valid, and so
                               * do the old sessions: consistent, and
                               * the caller is told nothing happened */
      return 0;
   }
   if (!db_durable_commit(d)) {
      db_durable_rollback(d);
      return 0;
   }
   return 1;
}

/* 1 = the password matches, 0 = it does not, -1 = THE QUESTION WAS NOT
 * ANSWERED. The third case used to be the second, so a database that could
 * not be read told the account holder their password was wrong -- and
 * recorded a failed attempt against them, which is how a storage fault turns
 * into a lockout. */
int user_check_password(struct db *d, long uid, const char *pw)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT pw_salt,pw_hash,pw_iters FROM user WHERE id=?");
   if (!st)
      return -1;
   sqlite3_bind_int64(st, 1, uid);
   int ok      = 0;
   int refused = 0; /* the KDF would not run: NOT "wrong password" */
   int rc      = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      const uint8_t *salt = sqlite3_column_blob(st, 0);
      const uint8_t *want = sqlite3_column_blob(st, 1);
      int iters           = sqlite3_column_int(st, 2);
      /* A COUNT OUTSIDE THE SUPPORTED RANGE IS NOT A PASSWORD TO CHECK.
       *
       * It cannot have been written by this program, so the row is damaged:
       * hashing against it would either burn a worker for billions of rounds
       * (a negative value casts to nearly 2^32) or, at zero, compare against
       * something that is not a hash. Answered as a storage failure -- -1,
       * the same as an unreadable row -- which the caller already turns into
       * a 500 rather than "wrong password", and which does NOT charge the
       * account a failed attempt. */
      if (iters < PW_ITERS_MIN || iters > PW_ITERS_MAX) {
         sqlite3_finalize(st);
         return -1;
      }
      if (salt && want && sqlite3_column_bytes(st, 0) == PW_SALT_LEN &&
          sqlite3_column_bytes(st, 1) == PW_HASH_LEN) {
         uint8_t got[PW_HASH_LEN];
         /* A REFUSED HASH IS NOT A MISMATCH. `got` would be unwritten stack,
          * and ct_eq over it is a comparison against nothing -- it would
          * almost certainly answer "wrong password", charge the account a
          * failed attempt, and never say why. Answered as the storage failure
          * it is, the same as the range check above. */
         if (!pw_hash(pw ? pw : "", salt, iters, got))
            refused = 1;
         else
            ok = ct_eq(got, want, PW_HASH_LEN);
      }
   }
   sqlite3_finalize(st);
   if (refused)
      return -1;
   if (rc != SQLITE_ROW && !db_finished(rc))
      return -1;
   return ok;
}

/* ---- MAINTENANCE WRITES ------------------------------------------------
 *
 * NOT AUTHENTICATION, AND NOT OPTIONAL EITHER.
 *
 * Three writes here are not part of deciding whether a request is genuine:
 * the session's rolling expiry, the nonce table's sliding-window prune, and
 * the app's last_seen stamp. All three were written as `if (st) { step;
 * finalize; }` -- the result of the step discarded, the prepare failure
 * ignored -- and each one failing is invisible and consequential:
 *
 *   - the rolling expiry is what keeps an ACTIVE user logged in. If it never
 *     lands, the session ages out on its original expiry while the user is
 *     using it, and they are logged out mid-session for no reason anyone can
 *     see afterwards;
 *   - the nonce prune is the only thing bounding a table that every signed
 *     request inserts into. If it never lands, the table grows without limit
 *     on a board whose storage is a memory card;
 *   - last_seen is how anyone tells a phone that stopped syncing from one
 *     that never tried.
 *
 * THE REQUEST IS STILL GOOD. Authentication is fail-closed and stays that
 * way, but none of these decide it: rejecting a request whose MAC verified
 * because a telemetry write failed would turn a full disk into a total
 * outage. So the answer is unchanged and the failure is REPORTED.
 *
 * AND RETRIED, which costs nothing here because each of the three is
 * attempted from state that the failed write would have changed: the rolling
 * expiry fires while `now - last_seen > SESS_BUMP`, and a failed update
 * leaves last_seen where it was, so the next request tries again rather than
 * waiting another day; the prune and the stamp run on every relevant request
 * already. What the counters below add is the ability to say that it has been
 * failing for a while, rather than printing the same line for ever with no
 * indication of whether it is one bad moment or a card that is gone.
 *
 * THE COUNTERS ARE ATOMIC BECAUSE THE POOL IS REAL. Requests are served by
 * HTTP_WORKERS threads, so every one of these counters is read and written by
 * ten threads at once; a plain `unsigned long` there is a data race, which is
 * undefined behaviour and not merely a number that comes out slightly wrong.
 * db.c counts its injected faults the same way and for the same reason.
 *
 * The reset is an EXCHANGE rather than a load-then-store, so exactly one
 * thread sees the old count and prints the recovery line: two workers
 * recovering at the same moment would otherwise both read the same tally and
 * report the run of failures twice.
 */
static void maint_report(struct db *d, const char *what, int ok,
                         _Atomic unsigned long *fails)
{
   if (ok) {
      unsigned long had = atomic_exchange(fails, 0UL);
      if (had)
         fprintf(stderr,
                 "sync: auth maintenance: %s succeeded again after "
                 "%lu failure(s)\n",
                 what, had);
      return;
   }
   unsigned long n = atomic_fetch_add(fails, 1UL) + 1;
   fprintf(stderr, "sync: auth maintenance: %s FAILED (%lu in a row): %s\n",
           what, n, db_errmsg(d));
}

/* Step and finalize one maintenance statement, reporting either outcome.
 * A NULL statement is a prepare that already failed and is counted as one. */
static int maint_step(struct db *d, struct sqlite3_stmt *st, const char *what,
                      _Atomic unsigned long *fails)
{
   if (!st) {
      maint_report(d, what, 0, fails);
      return 0;
   }
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   maint_report(d, what, ok, fails);
   return ok;
}

/* ---- sessions --------------------------------------------------------
 *
 * The cookie is "<selector>:<validator>". Only the selector is stored in the
 * clear (it is the index); of the validator only a SHA-256 is kept. So the
 * database holds nothing that can be presented as a login, and the lookup is
 * still a single indexed hit rather than a scan with a timing side channel.
 */
static void split_cookie(const char *cookie, char *sel, size_t selcap,
                         const char **val)
{
   const char *colon = strchr(cookie, ':');
   size_t n          = colon ? (size_t)(colon - cookie) : 0;
   if (!colon || n == 0 || n >= selcap) {
      sel[0] = '\0';
      *val   = "";
      return;
   }
   memcpy(sel, cookie, n);
   sel[n] = '\0';
   *val   = colon + 1;
}

/* Prepare, bind one integer, step to DONE, finalize. 1 when the statement RAN
 * -- not when it matched anything, which for a prune is not a fact worth
 * separating: a sweep that deleted nothing did its job. */
static int step_del1(struct db *d, const char *sql, long a)
{
   sqlite3_stmt *st = db_prep(d, sql);
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, a);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

/* ---- A NEW SESSION IS ALSO WHEN THE OLD ONES ARE DEALT WITH ----------
 *
 * WHAT THIS USED TO BE: one INSERT. A year-long credential appeared in the
 * table and the only thing that ever removed it was that same cookie being
 * presented after it expired (session_user, below, deletes on the way to
 * refusing it). So the rows that were cleaned up were exactly the ones still
 * being used, and the rows nobody ever came back for -- which are the ones
 * that matter -- stayed for a year each.
 *
 * What that looked like to the person affected: they sign in once on a
 * borrowed laptop, or a hotel machine, or a browser they were trying out, and
 * never open it again. There is no page anywhere that lists their sessions,
 * no expiry they will ever reach by using it, and no logout was clicked. The
 * row sits there for twelve months and it is not a stale record, it is a LIVE
 * LOGIN: anyone who can read that browser profile is signed in as them. Item
 * 54 made /logout fail closed rather than report a revocation that did not
 * happen; this is the other half of the same problem -- the sessions nobody
 * ever logs out of at all.
 *
 * So creating a session now also does two things, in the SAME transaction as
 * the insert, so that a crash cannot leave a cookie in the browser with no row
 * behind it or a cap enforced against a session that was never created:
 *
 *   1. EXPIRY, GLOBALLY. Every session row past its expires_at, whoever it
 *      belongs to, is deleted -- no cookie required. This is the half the old
 *      code structurally could not do: it only ever looked at the row it had
 *      just been handed the key to.
 *
 *   2. THE PER-USER CAP. After this insert the account holds at most
 *      MAX_SESSIONS live cookies; anything over that is revoked oldest-first
 *      by `last_seen`. See proto.h for why eight and why last_seen.
 *
 * WHY HERE AND NOT ON EVERY REQUEST. session_user runs on every single page
 * view, and a sweep there would charge every reader for the housekeeping of
 * every other account -- the shape item 35 flagged on the nonce table, except
 * that the nonce prune is a bounded delete on a small sliding window and this
 * one is not. Session CREATION is the right place: it is the event that adds
 * a row, it happens a handful of times per account per year, and it already
 * costs a quarter of a second of PBKDF2 -- against which a delete on an
 * indexed column is not measurable. The v2 index session(user_id, last_seen)
 * (db.c) is what keeps step 2 a seek rather than a scan.
 *
 * IT IS FAIL-CLOSED, and that is a deliberate choice rather than an
 * oversight. If the prune or the cap cannot be applied, no session is
 * created and the login answers 500. The alternative -- issue the cookie
 * anyway and report the housekeeping failure, which is what the nonce prune
 * and the rolling expiry do -- is right for a write that only ever ADDS
 * safety, and wrong here: step 2 REVOKES credentials, and a login that
 * quietly declined to revoke the excess is a login that handed out a ninth
 * live cookie while the account's own limit says eight. Refusing one login
 * on a database that is not writable costs the user a retry; the other way
 * costs them a credential they were told did not exist. */
int session_new(struct db *d, long uid, char *cookie, size_t cap)
{
   char sel[SELECTOR_HEX + 1], val[VALIDATOR_HEX + 1], vhash[65];
   rnd_hex(sel, SELECTOR_HEX);
   rnd_hex(val, VALIDATOR_HEX);
   sha256_hex(val, strlen(val), vhash);
   long now = (long)time(NULL);

   /* JOIN a transaction rather than open a second one: the invitation POST
    * (invite.c) is already inside one, creating the account this session is
    * for, and sqlite has no nested BEGIN. Its rollback covers these
    * statements too, which is exactly right -- an account that did not get
    * created must not leave a pruned session table behind. */
   int outer = !db_in_transaction(d);
   if (outer && !db_durable_begin(d))
      return 0;

   /* 1. EVERY EXPIRED ROW, NOT JUST THIS USER'S. A session that has aged out
    * is already refused by session_user, so keeping it protects nobody; it is
    * simply a dead credential taking space on a memory card. Sweeping
    * globally rather than per-user is what makes an ABANDONED account's rows
    * go away at all -- nobody is ever going to log in as them to trigger a
    * per-user sweep. */
   if (!step_del1(d, "DELETE FROM session WHERE expires_at<?", now))
      goto fail;

   /* 2. THE CAP, applied BEFORE the insert and therefore to MAX_SESSIONS-1:
    * the row about to be added is the newest of the set, so the account is
    * left holding exactly MAX_SESSIONS once this function returns.
    *
    * `LIMIT -1 OFFSET n` is sqlite's "everything after the first n rows", so
    * the subquery names precisely the excess. The tiebreak on rowid is not
    * decoration: rows created in the same second share a last_seen, and
    * without it which credential gets revoked would be sqlite's choice rather
    * than this rule's. rowid ascends with insertion, so DESC on both keys
    * means "newest kept, oldest cut" under every ordering of the ties.
    *
    * ORDER BY last_seen and NOT expires_at, although the two move together:
    * session_user bumps both at once, so for a session in use they say the
    * same thing -- but last_seen is the one that MEANS "when was this
    * credential last used", and if the rolling-expiry rule ever changes it is
    * the one that stays honest. A NULL last_seen sorts last under DESC and is
    * therefore cut first, which is also right: a row that has never recorded
    * a use is the emptiest thing in the table. */
   sqlite3_stmt *cp =
       db_prep(d, "DELETE FROM session WHERE user_id=?1 AND selector IN ("
                  "  SELECT selector FROM session WHERE user_id=?1"
                  "  ORDER BY last_seen DESC, rowid DESC LIMIT -1 OFFSET ?2)");
   if (!cp)
      goto fail;
   sqlite3_bind_int64(cp, 1, uid);
   sqlite3_bind_int64(cp, 2, MAX_SESSIONS - 1);
   int cok = sqlite3_step(cp) == SQLITE_DONE;
   sqlite3_finalize(cp);
   if (!cok)
      goto fail;

   sqlite3_stmt *st = db_prep(
       d, "INSERT INTO session(selector,verifier,user_id,expires_at,last_seen)"
          " VALUES(?,?,?,?,?)");
   if (!st)
      goto fail;
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, vhash, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, uid);
   sqlite3_bind_int64(st, 4, now + SESS_TTL);
   sqlite3_bind_int64(st, 5, now);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok)
      goto fail;

   /* THE COOKIE IS WRITTEN ONLY AFTER THE COMMIT. Handing it back and then
    * failing to commit would give the browser a credential for a row that
    * does not exist -- the user is "signed in" until their first click. */
   if (outer && !db_durable_commit(d))
      return 0;
   snprintf(cookie, cap, "%s:%s", sel, val);
   return 1;

fail:
   /* Only the transaction we opened is ours to abandon. When a caller owns
    * it, returning 0 is the whole of our contract and the caller rolls back
    * -- invite.c does exactly that. */
   if (outer)
      db_durable_rollback(d);
   return 0;
}

long session_user(struct db *d, const char *cookie, int *failed)
{
   if (failed)
      *failed = 0;
   if (!cookie || !*cookie)
      return 0;
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie, sel, sizeof sel, &val);
   if (!sel[0])
      return 0;
   sqlite3_stmt *st =
       db_prep(d, "SELECT verifier,user_id,expires_at,last_seen FROM session"
                  " WHERE selector=?");
   /* A LOOKUP THAT DID NOT RUN IS NOT "NOT SIGNED IN". Both used to be 0, so
    * a failed read showed a signed-in reader the login form: they retype a
    * password that was never the problem, while the page that was really
    * unavailable says nothing about itself. */
   if (!st) {
      if (failed)
         *failed = 1;
      return 0;
   }
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   long uid = 0, expires = 0, seen = 0;
   char stored[65] = {0};
   int rc          = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(st, 0);
      snprintf(stored, sizeof stored, "%s", v ? v : "");
      uid     = (long)sqlite3_column_int64(st, 1);
      expires = (long)sqlite3_column_int64(st, 2);
      seen    = (long)sqlite3_column_int64(st, 3);
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && !db_finished(rc)) {
      if (failed)
         *failed = 1;
      return 0;
   }
   if (!uid)
      return 0;
   long now = (long)time(NULL);
   if (expires < now) {
      /* THE ONE PLACE THE ANSWER IS DELIBERATELY DISCARDED, in writing.
       *
       * This is housekeeping, not a revocation the user was promised. The
       * cookie is refused by the line above whether or not the row goes away
       * -- the refusal is the expiry test, not the delete -- so a delete that
       * did not run costs nothing but a second attempt on the next request
       * with the same cookie. Answering 500 here would turn a full card into
       * "you cannot even be told you are signed out". db_finished has already
       * printed the reason if there was one. */
      (void)session_drop(d, cookie);
      return 0;
   }
   char vhash[65];
   sha256_hex(val, strlen(val), vhash);
   if (strlen(stored) != 64 || !ct_eq(stored, vhash, 64))
      return 0;
   /* Rolling expiry, at most once a day: an active user never gets logged
    * out, an abandoned session still ages out, and the write happens rarely
    * enough not to touch the SD card on every page view. */
   if (now - seen > SESS_BUMP) {
      static _Atomic unsigned long bump_fails;
      sqlite3_stmt *up = db_prep(
          d, "UPDATE session SET expires_at=?,last_seen=? WHERE selector=?");
      if (up) {
         sqlite3_bind_int64(up, 1, now + SESS_TTL);
         sqlite3_bind_int64(up, 2, now);
         sqlite3_bind_text(up, 3, sel, -1, SQLITE_STATIC);
      }
      /* The session is STILL VALID either way -- it was proven so above, and
       * this write only extends it. A failure here means the user may be
       * logged out at the original expiry; it is not a reason to log them
       * out now. */
      (void)maint_step(d, up, "session rolling expiry", &bump_fails);
   }
   return uid;
}

/* SEE auth.h for why this answers with three values rather than none. */
enum session_drop session_drop(struct db *d, const char *cookie)
{
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie ? cookie : "", sel, sizeof sel, &val);
   /* Nothing named a session, so there is nothing to survive the request.
    * ABSENT rather than FAILED: no statement was attempted and none was
    * needed. (/logout cannot reach this -- the router has already resolved a
    * user from this cookie, which needs a selector -- but a helper's answer
    * should not depend on which caller happens to use it.) */
   if (!sel[0])
      return SESSION_DROP_ABSENT;
   sqlite3_stmt *st = db_prep(d, "DELETE FROM session WHERE selector=?");
   if (!st) {
      fprintf(stderr, "sync: logout cannot prepare the session delete: %s\n",
              db_errmsg(d));
      return SESSION_DROP_FAILED;
   }
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   int rc = sqlite3_step(st);
   /* THE ROW COUNT IS READ BEFORE THE FINALIZE and used only on DONE.
    *
    * sqlite3_changes reports the most recent statement on this connection, so
    * after a step that FAILED it is the previous statement's number -- a
    * count belonging to some other question entirely. It is meaningful here
    * only once the DELETE is known to have finished, which is what the test
    * order below enforces.
    *
    * And db_changes, not SQLITE_DONE alone: a DELETE that matched no row is
    * also DONE, so "the statement ran" and "a session was removed" are
    * different claims (see db.c). This is the distinction the settings page's
    * revoke buttons already make; the logout route made neither. */
   int rows = db_changes(d);
   sqlite3_finalize(st);
   /* db_finished, not `rc == SQLITE_DONE`: BUSY, IOERR and CORRUPT all mean
    * the delete did not run, and it prints which one. The caller answers the
    * same way for all of them, but an operator deciding whether to wait, free
    * the card or restore needs the reason in the log. */
   if (!db_finished(rc))
      return SESSION_DROP_FAILED;
   return rows > 0 ? SESSION_DROP_GONE : SESSION_DROP_ABSENT;
}

/* 1 when EVERY session for this user is gone.
 *
 * This is a revocation -- it is what a password change and `sync logout` use
 * to make stolen cookies stop working. Reported as done while the DELETE
 * failed, the old cookies go on signing people in for a year, and the person
 * who changed their password because they thought someone else had it
 * believes they have shut them out. */
int session_drop_all(struct db *d, long uid)
{
   sqlite3_stmt *st = db_prep(d, "DELETE FROM session WHERE user_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

/* ---- INVITATION LINKS: THE OTHER TABLE NOBODY EVER SWEPT --------------
 *
 * A share token is a credential too -- whoever holds the link either signs in
 * or gets an account made on the spot -- so its rules live here with the
 * other credential mints rather than in the settings page that happens to be
 * the only caller today. That is not tidiness: the rule below is a rule about
 * the TABLE, and a rule that lives inside one handler is a rule the next mint
 * site will not get. `sync invite` on the command line is that second mint
 * site (sync.c), and it can adopt this in one line.
 *
 * WHAT THIS REPLACES: a bare INSERT on POST /settings/share. Every press of
 * "Create share link" added a row with a fourteen-day life, and nothing ever
 * removed one. Not expiry -- an expired token is filtered out of every query
 * by `expires_at>?` and then left in the table for ever. Not redemption --
 * h_invite_post sets `used_at` and the row stays. Only an explicit Revoke
 * click, or the account being deleted, took anything out.
 *
 * What that looked like to the person affected: the settings page renders one
 * row per LIVE link, so a user who kept minting links -- because the first one
 * did not arrive, because they mistyped the address, because they were not
 * sure it had worked -- watched their own settings page grow a longer and
 * longer table of near-identical invitations, none of which they could tell
 * apart. Underneath it, the spent and expired rows they could no longer see
 * accumulated on a memory card with about nineteen megabytes free, at whatever
 * rate a signed-in session cared to POST.
 *
 * REFUSING, NOT REPLACING, at the cap. The item permits either and they are
 * different promises. A share token is a credential that has ALREADY BEEN
 * HANDED OUT: by the time an eleventh one is minted, the first ten are in
 * other people's inboxes. Replacing the oldest would silently kill an
 * invitation somebody was sent last week, with no notice to either end -- the
 * owner sees "Share link created" and cannot tell that they have just broken
 * the link they mailed their doctor, and the doctor finds out by clicking it.
 * This page already refuses in exactly this shape when the follower cap is
 * reached ("At the limit of 10 followers"), the Revoke button that makes room
 * is on the same screen, and a refusal is REVERSIBLE where a replacement is
 * not: nothing can bring back a token that has been overwritten. Silently
 * revoking access nobody asked to have revoked is the mirror image of the
 * mistake this file's revoke buttons are so careful about -- reporting a
 * revocation that did not happen -- and it is no better for being the
 * friendlier-looking direction.
 *
 * ONE TRANSACTION, and the order in it is load-bearing. The prune runs FIRST,
 * so by the time the count is taken there is no such thing as an expired or
 * spent row: `WHERE owner_id=?` alone is exactly the live set, which is also
 * exactly what the settings page lists. Counting before the prune would refuse
 * an owner on the strength of ten links that all died a fortnight ago. */
enum share_mint share_token_mint(struct db *d, long owner, const char *email,
                                 char *token, size_t cap)
{
   char tok[TOKEN_HEX + 1];
   rnd_hex(tok, TOKEN_HEX);
   long now = (long)time(NULL);

   if (!db_durable_begin(d))
      return SHARE_MINT_FAILED;

   /* EVERY OWNER'S DEAD ROWS, not just this one's. An account that has stopped
    * being used is never going to mint again, so a per-owner sweep would leave
    * its rows for ever -- and they are the rows most likely to be there. */
   sqlite3_stmt *del =
       db_prep(d, "DELETE FROM share_token"
                  " WHERE expires_at<=? OR used_at IS NOT NULL");
   if (!del) {
      db_durable_rollback(d);
      return SHARE_MINT_FAILED;
   }
   sqlite3_bind_int64(del, 1, now);
   int dok = sqlite3_step(del) == SQLITE_DONE;
   sqlite3_finalize(del);
   if (!dok) {
      db_durable_rollback(d);
      return SHARE_MINT_FAILED;
   }

   /* The cap is PER OWNER, so an ownerless signup link -- what `sync invite`
    * prints, minted by hand by whoever runs the box -- has nobody to count
    * against and is not capped. It is also not reachable from a browser. */
   if (owner) {
      long n = 0;
      /* count(*) always yields a row, so DB_GET_NONE here would itself be a
       * fault; either non-VALUE outcome means the cap could not be read, and
       * a cap that cannot be read is not a cap. (The same reasoning, in the
       * same words, as the follower cap in invite.c.) */
      if (db_get_long(d, "SELECT count(*) FROM share_token WHERE owner_id=?",
                      owner, &n) != DB_GET_VALUE) {
         db_durable_rollback(d);
         return SHARE_MINT_FAILED;
      }
      if (n >= MAX_LIVE_TOKENS) {
         /* COMMITTED, THOUGH NOTHING IS BEING MINTED. The prune is
          * maintenance, not part of the mint's promise, and throwing it away
          * because the mint was refused would mean the one owner who most
          * needs the sweep -- the one sitting at the cap, who cannot mint and
          * therefore cannot trigger a successful one -- is the one who never
          * gets it. db_durable_commit rolls back by itself if it cannot
          * commit, and the answer is unchanged either way: the count was
          * read, and it said full. */
         (void)db_durable_commit(d);
         return SHARE_MINT_FULL;
      }
   }

   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO share_token(token,owner_id,email,created_at,"
                  " expires_at) VALUES(?,?,?,?,?)");
   if (!st) {
      db_durable_rollback(d);
      return SHARE_MINT_FAILED;
   }
   sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
   /* NULL, not 0: `owner_id` NULL is what distinguishes a plain signup link
    * from one that also grants a follow, and 0 is a user id that would match
    * nothing and cascade from nothing. */
   if (owner)
      sqlite3_bind_int64(st, 2, owner);
   else
      sqlite3_bind_null(st, 2);
   if (email && *email)
      sqlite3_bind_text(st, 3, email, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(st, 3);
   sqlite3_bind_int64(st, 4, now);
   sqlite3_bind_int64(st, 5, now + TOKEN_TTL);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok) {
      db_durable_rollback(d);
      return SHARE_MINT_FAILED;
   }
   /* As with the session cookie: the token is handed back only once the row
    * that backs it is committed. A link printed for a transaction that then
    * failed to commit is a link that leads to "no such invitation". */
   if (!db_durable_commit(d))
      return SHARE_MINT_FAILED;
   snprintf(token, cap, "%s", tok);
   return SHARE_MINT_OK;
}

/* Derived from the cookie itself, so it needs no storage and survives a
 * restart. An attacker who cannot read the cookie (HttpOnly, and another
 * origin's script cannot see it) cannot compute this. */
void csrf_token(const char *cookie, char *out, size_t cap)
{
   char buf[256], hex[65];
   snprintf(buf, sizeof buf, "csrf|%s", cookie ? cookie : "");
   sha256_hex(buf, strlen(buf), hex);
   snprintf(out, cap, "%.32s", hex);
}

int csrf_ok(const char *cookie, const char *sent)
{
   char want[64];
   csrf_token(cookie, want, sizeof want);
   return sent && strlen(sent) == strlen(want) &&
          ct_eq(want, sent, strlen(want));
}

/* ---- signed app requests (the app half is app/sync.c) ----------------- */

static int nonce_fresh(struct db *d, long uid, const char *nonce, long now)
{
   /* Prune first: the table is a sliding window, not a log.
    *
    * REPORTED, because this is the only bound on a table every signed request
    * writes to. Ignored, a prune that has been failing since the card filled
    * up looks exactly like one that has nothing to delete -- and the symptom
    * arrives weeks later as a database that will not open. */
   static _Atomic unsigned long prune_fails;
   sqlite3_stmt *del = db_prep(d, "DELETE FROM nonce WHERE seen_at < ?");
   if (del)
      sqlite3_bind_int64(del, 1, now - 2 * SIG_SKEW);
   (void)maint_step(d, del, "nonce window prune", &prune_fails);
   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO nonce(user_id,nonce,seen_at) VALUES(?,?,?)");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_text(st, 2, nonce, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, now);
   int ok = sqlite3_step(st) == SQLITE_DONE; /* a repeat collides on the PK */
   sqlite3_finalize(st);
   return ok;
}

static int nonce_charset_ok(const char *n)
{
   size_t len = strlen(n);
   if (len < NONCE_MIN || len > NONCE_MAX)
      return 0;
   for (size_t i = 0; i < len; i++) {
      char c = n[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

long verify_signature(const struct req *r)
{
   char auth[256];
   if (!hdr_get(r->hdr, "Authorization", auth, sizeof auth))
      return 0;
   if (strncmp(auth, "Pancra ", 7))
      return 0;
   char uid_s[32], ts_s[32], nonce[NONCE_MAX + 1], mac_hex[80];
   const char *p = auth + 7;
   /* "<uid>:<ts>:<nonce>:<mac>" -- four fields, no spaces. */
   if (sscanf(p, "%31[^:]:%31[^:]:%64[^:]:%79s", uid_s, ts_s, nonce, mac_hex) !=
       4)
      return 0;
   long uid = strtol(uid_s, NULL, 10);
   long ts  = strtol(ts_s, NULL, 10);
   long now = (long)time(NULL);
   if (uid <= 0 || !nonce_charset_ok(nonce) || strlen(mac_hex) != 64)
      return 0;
   if (ts < now - SIG_SKEW || ts > now + SIG_SKEW)
      return 0;

   uint8_t key[16];
   sqlite3_stmt *st = db_prep(r->db, "SELECT key FROM app WHERE user_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int have = 0;
   if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 16) {
      memcpy(key, sqlite3_column_blob(st, 0), 16);
      have = 1;
   }
   sqlite3_finalize(st);
   if (!have)
      return 0;

   char bodyhash[65];
   sha256_hex(r->body ? r->body : "", r->body_len, bodyhash);
   char signing[1024];
   int n = sig_signing_string(signing, sizeof signing, r->method, r->target, ts,
                              nonce, bodyhash);
   if (n <= 0)
      return 0;
   uint8_t want[32], got[32];
   hmac_sha256(key, sizeof key, (const uint8_t *)signing, (size_t)n, want);
   if (!hex_to(mac_hex, 64, got))
      return 0;
   if (!ct_eq(want, got, 32))
      return 0;
   /* Only now, with the MAC proven, is the nonce spent: an attacker must not
    * be able to burn nonces (or grow the table) with unsigned noise. */
   if (!nonce_fresh(r->db, uid, nonce, now))
      return 0;
   static _Atomic unsigned long seen_fails;
   sqlite3_stmt *up =
       db_prep(r->db, "UPDATE app SET last_seen=? WHERE user_id=?");
   if (up) {
      sqlite3_bind_int64(up, 1, now);
      sqlite3_bind_int64(up, 2, uid);
   }
   /* The signature verified and the nonce was accepted: this request is
    * genuine whatever the stamp does. */
   (void)maint_step(r->db, up, "app last_seen stamp", &seen_fails);
   return uid;
}

/* ---- login throttle --------------------------------------------------
 *
 * PBKDF2 on one slow core is precisely the work an attacker would like to
 * make this server do: a few hundred guesses a minute would starve the app's
 * own pushes even if none of them ever succeeded. The counter is per email,
 * which is what a guessing attack varies least. */
/* LOGIN_OK / LOGIN_THROTTLED / LOGIN_UNKNOWN -- see auth.h for why the third
 * exists. In short: "the counter could not be read" is not "the counter says
 * zero", and answering the second for the first turns a database hiccup into
 * an unthrottled password search. */
int login_throttled(struct db *d, const char *email)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT n,first_at FROM login_fail WHERE email=?");
   if (!st)
      return LOGIN_UNKNOWN;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   int n      = 0;
   long first = 0;
   int rc     = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      n     = sqlite3_column_int(st, 0);
      first = (long)sqlite3_column_int64(st, 1);
   }
   sqlite3_finalize(st);
   /* SQLITE_DONE is a real answer: this address has no row, so it is not
    * throttled. Anything else -- BUSY, IOERR, CORRUPT -- means the scan did
    * not finish, and a scan that did not finish found nothing for the same
    * reason it would have found a count of five. */
   if (rc != SQLITE_ROW && !db_finished(rc))
      return LOGIN_UNKNOWN;
   if (n >= LOGIN_FAIL_MAX && (long)time(NULL) - first < LOGIN_FAIL_WIN)
      return LOGIN_THROTTLED;
   return LOGIN_OK;
}

int login_failed(struct db *d, const char *email)
{
   /* THE ADDRESS MUST ALREADY BE CANONICAL, as in user_create above, and for
    * the sharper reason: this is the one write in the program a stranger can
    * cause without holding anything at all. A row keyed on an address no
    * surface will accept again is a row the throttle can never match and no
    * successful login can ever clear -- it is pure storage, and it is the
    * shape a kilobyte-long field was being written in.
    *
    * A refusal here is reported as a FAILED WRITE, and both callers already
    * treat that as fatal to the request ("an uncounted failure is an
    * unthrottled guess"). That is the right answer: this cannot be reached
    * with a canonical address, so reaching it at all means the caller skipped
    * email_canon, and serving that request would be serving an unthrottled
    * one. */
   char want[EMAIL_BUF];
   if (!email || !email_canon(email, want, sizeof want) ||
       strcmp(want, email) != 0)
      return 0;
   long now = (long)time(NULL);
   /* SWEEP FIRST, on the same sliding-window principle nonce_fresh uses.
    *
    * This table is the only one a stranger can grow at will. The row is
    * inserted for an email that does not exist -- user_by_email fails and the
    * password check is never reached, so the request costs nothing -- and the
    * throttle above counts PER EMAIL, so varying the address defeats it
    * entirely. Nothing ever removed these rows except a successful login for
    * that exact address, which by construction never comes. With emails
    * accepted up to a kilobyte and keep-alive allowing hundreds of POSTs per
    * handshake, that is megabytes a day onto an SD card, forever.
    *
    * A row outside the window can no longer throttle anything (login_throttled
    * requires first_at within LOGIN_FAIL_WIN), so keeping it serves no
    * purpose. The nonce table -- reachable only AFTER a MAC verifies, i.e. the
    * one an outsider cannot touch -- has been pruned this way all along. */
   sqlite3_stmt *sw = db_prep(d, "DELETE FROM login_fail WHERE first_at < ?");
   if (sw) {
      sqlite3_bind_int64(sw, 1, now - LOGIN_FAIL_WIN);
      sqlite3_step(sw);
      sqlite3_finalize(sw);
   }
   sqlite3_stmt *st = db_prep(
       d, "INSERT INTO login_fail(email,n,first_at) VALUES(?,1,?)"
          " ON CONFLICT(email) DO UPDATE SET"
          "   n = CASE WHEN first_at < ? THEN 1 ELSE n+1 END,"
          "   first_at = CASE WHEN first_at < ? THEN ? ELSE first_at END");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, now);
   sqlite3_bind_int64(st, 3, now - LOGIN_FAIL_WIN);
   sqlite3_bind_int64(st, 4, now - LOGIN_FAIL_WIN);
   sqlite3_bind_int64(st, 5, now);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

/* 1 when the account's failure record is CLEARED. Dropped, a database that
 * would not delete left the count standing, so a user who has just typed the
 * right password stays one failure closer to a lockout they cannot see. */
int login_ok(struct db *d, const char *email)
{
   sqlite3_stmt *st = db_prep(d, "DELETE FROM login_fail WHERE email=?");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}
