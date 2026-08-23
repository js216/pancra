/* SPDX-License-Identifier: GPL-3.0
 * authmaint.c --- the housekeeping writes, counted and reported
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
#include "authint.h"
#include "db.h"
#include <sqlite3.h>
#include <stdio.h>

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
 * leaves last_seen unchanged, so the next request tries again rather than
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
 * thread sees the accumulated count and prints the recovery line: two
 * workers recovering at the same moment would otherwise both read the same
 * tally and
 * report the run of failures twice.
 */
void auth_maint_report(struct db *d, const char *what, int ok,
                       _Atomic uint64_t *fails)
{
   if (ok) {
      uint64_t had = atomic_exchange(fails, 0UL);
      if (had)
         fprintf(stderr,
                 "sync: auth maintenance: %s succeeded again after "
                 "%lu failure(s)\n",
                 what, had);
      return;
   }
   uint64_t n = atomic_fetch_add(fails, 1UL) + 1;
   fprintf(stderr, "sync: auth maintenance: %s FAILED (%lu in a row): %s\n",
           what, n, db_errmsg(d));
}

/* Step and finalize one maintenance statement, reporting either outcome.
 * A NULL statement is a prepare that already failed and is counted as one. */
int auth_maint_step(struct db *d, struct sqlite3_stmt *st, const char *what,
                    _Atomic uint64_t *fails)
{
   if (!st) {
      auth_maint_report(d, what, 0, fails);
      return 0;
   }
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   auth_maint_report(d, what, ok, fails);
   return ok;
}
