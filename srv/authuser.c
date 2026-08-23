/* SPDX-License-Identifier: GPL-3.0
 * authuser.c --- the account: its address, its password, its removal
 * Copyright 2026 Jakob Kastelic
 *
 * ONE CREDENTIAL PER FILE. auth.c held five of them -- the account
 * and its password, the browser session and its CSRF derivative, the
 * invitation link, the signed app request, and the login throttle -- 1191
 * lines behind one public header, so a page that only wanted to know whether
 * a cookie was valid pulled in the password KDF, the nonce window and the
 * share-token cap with it. They are separate credentials with separate
 * tables, separate lifetimes and separate failure modes; the only things
 * they share are the constant-time compare and the maintenance-write
 * reporter, and those are in authint.h where nothing outside this module can
 * reach them.
 *
 * THE DECLARATIONS MOVED WITH THEM. auth.h is gone; each file above has a
 * header of its own (authuser.h, authsess.h, authshare.h, authsig.h,
 * authrate.h) holding exactly its own contracts, comments and all, so a
 * caller's include list says which credentials it deals in. Every one of
 * those contracts was paid for by an incident, so none of them is reworded
 * here.
 */
#include "authuser.h"
/* THE ONE EDGE BETWEEN TWO CREDENTIALS, AND IT IS DELIBERATE.
 * Changing a password REVOKES every session in the same transaction as the
 * write -- the browser path once forgot to, and reported success, so a stolen
 * cookie outlived the password it was issued against. Splitting the files
 * must not split that transaction, so this one names the other. It is not a
 * cycle: the session file knows nothing about accounts. */
#include "authsess.h"
#include "ct.h"
#include "db.h"
#include "pbkdf2.h"
#include "proto.h"
#include "pwcost.h" /* the cost THIS deployment measured */
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
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
 * A void return, over a KDF that substitutes rather than refuses -- a stored
 * iteration count of 0 computed as 1, a salt longer than the KDF's scratch
 * cut short -- can report neither. The visible symptom is a login that works
 * against a hash weaker than the row claims, or, once the substitution stops
 * matching (say after the salt length changes), an account whose correct
 * password stops being accepted with nothing anywhere to explain it. */
int pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
            uint8_t out[PW_HASH_LEN])
{
   return pbkdf2_sha256((const uint8_t *)pw, strlen(pw), salt, PW_SALT_LEN,
                        (unsigned)iters, out, PW_HASH_LEN) == PBKDF2_OK;
}

/* THE ACCOUNT'S ID, or 0 -- and `failed` (may be NULL) separates the two
 * zeroes: no such account, versus a lookup that did not run. As one answer,
 * a database that cannot be read tells the account holder their email or
 * password was wrong. */
int64_t user_by_email(struct db *d, const char *email, int *failed)
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
   int64_t id = 0;
   int rc     = sqlite3_step(st);
   if (rc == SQLITE_ROW)
      id = (int64_t)sqlite3_column_int64(st, 0);
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

enum user_create_result user_create(struct db *d, const char *email,
                                    const char *pw, int64_t *uid)
{
   /* No length rule beyond "there is one". A minimum stops nobody who wants a
    * weak password -- "password1" clears the customary eight -- and it does
    * turn away a passphrase somebody chose deliberately. Empty is refused,
    * because that is not a short password, it is no password. */
   if (!pw || !*pw)
      return USER_CREATE_INVALID;
   /* THE ADDRESS MUST ALREADY BE CANONICAL. This is a backstop, not the check:
    * every caller runs email_canon first (see util.h) and passes the result.
    * It is here because an account is the one thing in this program that
    * CANNOT be undone by the person it hurts -- there is no password reset.
    * A rule of "contains an '@' somewhere" admits "@" itself, and admits a
    * 900-byte address that /login refuses to look up at all. A row like that
    * is an account whose owner can never sign in again, and nothing about it
    * is visible until they try.
    *
    * Comparing against the canonical form rather than re-deriving it is
    * deliberate: substituting a corrected address for the one the caller asked
    * for would create an account under an address the caller never named, and
    * the caller's throttle and lookup would still be about the other one. */
   char want[EMAIL_BUF];
   if (!email || !email_canon(email, want, sizeof want) ||
       strcmp(want, email) != 0)
      return USER_CREATE_INVALID;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   /* NO ACCOUNT AT ALL beats an account whose pw_hash column holds whatever
    * was on the stack: that row would be unloginnable and would look exactly
    * like a forgotten password. */
   /* A KDF THAT REFUSED IS NOT A BAD PASSWORD. Nothing about the
    * request was wrong; this server could not carry it out. */
   if (!pw_hash(pw, salt, pwcost_iters(), hash))
      return USER_CREATE_FAIL;
   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO user(email,pw_salt,pw_hash,pw_iters,pw_kdf,"
                  "                 created_at)"
                  " VALUES(?,?,?,?,?,?)");
   if (!st)
      return USER_CREATE_FAIL;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   sqlite3_bind_blob(st, 2, salt, sizeof salt, SQLITE_STATIC);
   sqlite3_bind_blob(st, 3, hash, sizeof hash, SQLITE_STATIC);
   sqlite3_bind_int(st, 4, pwcost_iters());
   /* THE ROW SAYS WHICH FUNCTION MADE ITS HASH. */
   sqlite3_bind_int(st, 5, PW_KDF_CURRENT);
   sqlite3_bind_int64(st, 6, (sqlite3_int64)time(NULL));
   /* THE CONSTRAINT IS WHAT SAYS "ALREADY EXISTS", and it is asked rather
    * than inferred. `email` is UNIQUE, so a duplicate comes back as
    * SQLITE_CONSTRAINT and nothing else does -- which is the difference
    * between telling somebody their address is taken and telling them that
    * because the disk was full. */
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc == SQLITE_DONE) {
      if (uid)
         *uid = db_last_id(d);
      return USER_CREATE_OK;
   }
   if (rc == SQLITE_CONSTRAINT)
      return USER_CREATE_EXISTS;
   return USER_CREATE_FAIL;
}

/* THE PASSWORD AND THE SESSIONS ARE ONE CHANGE, and this is the only way to
 * make it.
 *
 * A password change that leaves the existing cookies working is not a
 * password change: the usual reason to want one is that somebody else may be
 * signed in, and a session cookie is a bearer token that outlives the secret
 * it was issued against. Left to each caller as a second step, one of them
 * drops the sessions and another reports "Password changed." having revoked
 * nothing -- and the one that forgets is the path a worried user actually
 * reaches.
 *
 * So it is not a caller's step at all. Both happen inside ONE transaction:
 * half of this applied is worse than neither, because a new password with
 * the previous sessions still live is exactly the state the user believes
 * they have just left.
 *
 * Returns 1 only when both are committed. */
int user_set_password(struct db *d, int64_t uid, const char *pw)
{
   if (!pw || !*pw)
      return 0;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   /* BEFORE the transaction opens: a refusal here leaves the account
    * untouched, which is what this function promises. */
   if (!pw_hash(pw, salt, pwcost_iters(), hash))
      return 0;

   /* IMMEDIATE, so the write lock is taken now rather than at the first
    * write: a deferred transaction that cannot upgrade fails at COMMIT, and
    * by then the caller has already been told which parts worked. */
   if (!db_durable_begin(d))
      return 0;

   sqlite3_stmt *st =
       db_prep(d, "UPDATE user SET pw_salt=?,pw_hash=?,pw_iters=?,pw_kdf=?"
                  " WHERE id=?");
   if (!st) {
      db_durable_rollback(d);
      return 0;
   }
   sqlite3_bind_blob(st, 1, salt, sizeof salt, SQLITE_STATIC);
   sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
   sqlite3_bind_int(st, 3, pwcost_iters());
   sqlite3_bind_int(st, 4, PW_KDF_CURRENT);
   sqlite3_bind_int64(st, 5, uid);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok) {
      db_durable_rollback(d);
      return 0;
   }

   if (!session_drop_all(d, uid)) {
      db_durable_rollback(d); /* the stored password stays valid, and so
                               * do its sessions: consistent, and
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
 * ANSWERED. Fold the third case into the second and a database that cannot
 * be read tells the account holder their password was wrong -- and records a
 * failed attempt against them, which is how a storage fault turns into a
 * lockout. */
/* See authuser.h. ONE STATEMENT, IN A TRANSACTION: the cascade is the schema's,
 * and wrapping it means a delete either happened or did not -- there is no
 * state in between for a caller to discover. */
enum user_delete user_delete_account(struct db *d, int64_t uid)
{
   if (!d || uid <= 0)
      return USER_DELETE_FAIL;
   if (!db_durable_begin(d))
      return USER_DELETE_FAIL;
   sqlite3_stmt *st = db_prep(d, "DELETE FROM user WHERE id=?");
   if (!st) {
      db_durable_rollback(d);
      return USER_DELETE_FAIL;
   }
   sqlite3_bind_int64(st, 1, uid);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      db_durable_rollback(d);
      return USER_DELETE_FAIL;
   }
   /* NO ROW CHANGED IS NOT A FAILURE. The account was not there; the
    * transaction is closed rather than rolled back, because nothing in it
    * needs undoing and a rollback would report an error to a caller who
    * asked a perfectly good question. */
   int gone = db_changes(d) > 0;
   if (!db_durable_commit(d)) {
      db_durable_rollback(d);
      return USER_DELETE_FAIL;
   }
   return gone ? USER_DELETED : USER_NO_SUCH;
}

/* ---- THE COST PARAMETER MOVES; ESTABLISHED ACCOUNTS DID NOT --
 *
 * The cost is a judgement about what a CPU can do, and the whole point of
 * raising it is that a smaller number has stopped being enough. Applied by
 * user_create and user_set_password and by nothing else, it reaches exactly
 * the accounts that do not need it -- the new ones -- and never the ones that
 * have been there since the first release. This server
 * has no password-expiry policy and no reset-by-email, so "they will change
 * it eventually" is not a mechanism; an account made under the first cost
 * stays under it for as long as it exists.
 *
 * A successful login is the one moment the plaintext is in hand and the row
 * is known to match it, which makes it the only place a re-derivation can
 * happen without asking the user for anything. So: after a check that
 * SUCCEEDED, and only when the stored cost is below current policy, derive a
 * fresh salt and hash at the current cost and publish them.
 *
 * COMPARE-AND-SWAP, NOT AN UNCONDITIONAL UPDATE. Between the check and this
 * write the user may have changed their password in another tab -- and
 * user_set_password also SIGNS EVERY SESSION OUT, so overwriting it here
 * would restore a credential that was deliberately revoked, silently, on the
 * strength of a login that raced it. The UPDATE therefore names the row it
 * expects to find (`WHERE pw_hash=? AND pw_iters=?`); a password change that
 * landed first simply means no row matches and the upgrade does nothing.
 *
 * NON-FATAL, ALWAYS. This is housekeeping riding on somebody's login. A
 * failed prepare, a locked database, a step that does not finish -- none of
 * them says anything about whether the password was right, and the answer to
 * that question has already been decided. The row keeps its old cost and the
 * next successful login tries again.
 *
 * SECRETS WIPED. The plaintext is the caller's, but both derived hashes and
 * the new salt are this function's, and this is a long-lived worker thread
 * whose stack is reused by the next request. */
static void pw_upgrade(struct db *d, int64_t uid, const char *pw,
                       const uint8_t oldhash[PW_HASH_LEN], int olditers,
                       int oldkdf)
{
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   if (pw_hash(pw ? pw : "", salt, pwcost_iters(), hash)) {
      sqlite3_stmt *st =
          db_prep(d, "UPDATE user SET pw_salt=?,pw_hash=?,pw_iters=?,pw_kdf=?"
                     " WHERE id=? AND pw_hash=? AND pw_iters=? AND pw_kdf=?");
      if (st) {
         sqlite3_bind_blob(st, 1, salt, sizeof salt, SQLITE_STATIC);
         sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
         sqlite3_bind_int(st, 3, pwcost_iters());
         sqlite3_bind_int(st, 4, PW_KDF_CURRENT);
         sqlite3_bind_int64(st, 5, uid);
         sqlite3_bind_blob(st, 6, oldhash, PW_HASH_LEN, SQLITE_STATIC);
         sqlite3_bind_int(st, 7, olditers);
         /* THE VERSION IS PART OF THE COMPARE-AND-SWAP, not only of the
          * write: a password change that landed first may have moved the row
          * to another KDF, and this upgrade must not overwrite it. */
         sqlite3_bind_int(st, 8, oldkdf);
         /* The answer is deliberately not returned: see above. */
         (void)sqlite3_step(st);
         sqlite3_finalize(st);
      }
   }
   ct_wipe(salt, sizeof salt);
   ct_wipe(hash, sizeof hash);
}

int user_check_password(struct db *d, int64_t uid, const char *pw)
{
   sqlite3_stmt *st = db_prep(
       d, "SELECT pw_salt,pw_hash,pw_iters,pw_kdf FROM user WHERE id=?");
   if (!st)
      return -1;
   sqlite3_bind_int64(st, 1, uid);
   int ok      = 0;
   int refused = 0; /* the KDF would not run: NOT "wrong password" */
   /* WHAT THE ROW HELD, copied out for the upgrade below: the statement is
    * finalized before anything else runs, so the blob pointers do not
    * outlive it. `upgrade` is set only when the stored cost is behind
    * current policy AND the password matched. */
   uint8_t oldhash[PW_HASH_LEN];
   int olditers = 0;
   int oldkdf   = 0;
   int upgrade  = 0;
   int rc       = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      const uint8_t *salt = sqlite3_column_blob(st, 0);
      const uint8_t *want = sqlite3_column_blob(st, 1);
      int iters           = sqlite3_column_int(st, 2);
      int kdf             = sqlite3_column_int(st, 3);
      /* ---- VERIFIED BY THE FUNCTION THAT MADE IT ------------
       *
       * A KDF version this build does not know is not a wrong password: it
       * is a row written by a NEWER server (a rolling upgrade, a rollback),
       * or a row somebody edited. Answering "wrong password" would send the
       * account holder to reset a password that is fine and charge them a
       * failed attempt for it; -1 is the storage-failure answer the caller
       * already turns into a 500. */
      if (kdf != PW_KDF_PBKDF2_SHA256) {
         fprintf(stderr,
                 "sync: user %lld's password was hashed by KDF version %d, "
                 "which this build does not implement.\n"
                 "  It is not being treated as a wrong password. Run a build "
                 "that knows that version.\n",
                 (long long)uid, kdf);
         sqlite3_finalize(st);
         return -1;
      }
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
         /* AND AN OLD ONE IS RE-DERIVED UNDER TODAY'S POLICY. `kdf` is
          * already known to be the current version here -- the branch above
          * refuses anything else -- so what is left to be behind is the cost
          * parameter; when a second KDF version exists this is where the
          * "or a version older than PW_KDF_CURRENT" arm goes, and the CAS
          * below already names the column. */
         /* AGAINST TODAY'S POLICY, not against the compiled default:
          * the cost this deployment measured is what a credential written now
          * would carry, so it is what "behind" means. */
         if (ok && iters < pwcost_iters()) {
            memcpy(oldhash, want, PW_HASH_LEN);
            olditers = iters;
            oldkdf   = kdf;
            upgrade  = 1;
         }
         ct_wipe(got, sizeof got);
      }
   }
   sqlite3_finalize(st);
   if (refused)
      return -1;
   if (rc != SQLITE_ROW && !db_finished(rc))
      return -1;
   /* AFTER THE STATEMENT IS FINALIZED, and after the answer is settled: the
    * upgrade must not hold a read statement open across a write of its own,
    * and it cannot change what this function returns. */
   if (upgrade) {
      pw_upgrade(d, uid, pw, oldhash, olditers, oldkdf);
      ct_wipe(oldhash, sizeof oldhash);
   }
   return ok;
}
