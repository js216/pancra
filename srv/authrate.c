/* SPDX-License-Identifier: GPL-3.0
 * authrate.c --- the login throttle, per address
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
#include "authrate.h"
#include "db.h"
#include "proto.h"
#include "util.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

/* ---- login throttle --------------------------------------------------
 *
 * PBKDF2 on one slow core is precisely the work an attacker would like to
 * make this server do: a few hundred guesses a minute would starve the app's
 * own pushes even if none of them ever succeeded. The counter is per email,
 * which is what a guessing attack varies least. */
/* LOGIN_OK / LOGIN_THROTTLED / LOGIN_UNKNOWN -- see authrate.h for why the
 * third exists. In short: "the counter could not be read" is not "the counter
 * says zero", and answering the second for the first turns a database hiccup
 * into an unthrottled password search. */
int login_throttled(struct db *d, const char *email)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT n,first_at FROM login_fail WHERE email=?");
   if (!st)
      return LOGIN_UNKNOWN;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   int n         = 0;
   int64_t first = 0;
   int rc        = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      n     = sqlite3_column_int(st, 0);
      first = (int64_t)sqlite3_column_int64(st, 1);
   }
   sqlite3_finalize(st);
   /* SQLITE_DONE is a real answer: this address has no row, so it is not
    * throttled. Anything else -- BUSY, IOERR, CORRUPT -- means the scan did
    * not finish, and a scan that did not finish found nothing for the same
    * reason it would have found a count of five. */
   if (rc != SQLITE_ROW && !db_finished(rc))
      return LOGIN_UNKNOWN;
   if (n >= LOGIN_FAIL_MAX && (int64_t)time(NULL) - first < LOGIN_FAIL_WIN)
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
   int64_t now = (int64_t)time(NULL);
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
