/* SPDX-License-Identifier: GPL-3.0
 * authsess.c --- the browser session, and the CSRF token derived from it
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
#include "authsess.h"
#include "authint.h"
#include "ct.h"
#include "db.h"
#include "proto.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static int step_del1(struct db *d, const char *sql, int64_t a)
{
   sqlite3_stmt *st = db_prep(d, sql);
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, a);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

/* ---- A NEW SESSION IS ALSO WHEN THE STANDING ONES ARE DEALT WITH -------
 *
 * AS ONE INSERT, a year-long credential appears in the table and the only
 * thing that removes it is that same cookie being
 * presented after it expires (session_refresh, below, retires it on the way
 * to refusing it). So the rows cleaned up were exactly the ones still
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
 * WHY HERE AND NOT ON EVERY REQUEST. A session is verified on every single
 * page
 * view, and a sweep there would charge every reader for the housekeeping of
 * every other account -- the shape the nonce table has, except that the
 * nonce prune is a bounded delete on a small sliding window and this
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
int session_new(struct db *d, int64_t uid, char *cookie, size_t cap)
{
   char sel[SELECTOR_HEX + 1], val[VALIDATOR_HEX + 1], vhash[65];
   /* A TOKEN THAT COULD NOT BE MINTED IS NOT A SESSION. rnd_hex refuses an
    * odd or oversized request rather than returning a shorter string (see
    * util.h); a caller that ignored that would be handing out a credential
    * with less entropy than it believes. */
   if (!rnd_hex(sel, sizeof sel, SELECTOR_HEX) ||
       !rnd_hex(val, sizeof val, VALIDATOR_HEX))
      return 0;
   sha256_hex(val, strlen(val), vhash);
   int64_t now = (int64_t)time(NULL);

   /* JOIN a transaction rather than open a second one: the invitation POST
    * (invite.c) is already inside one, creating the account this session is
    * for, and sqlite has no nested BEGIN. Its rollback covers these
    * statements too, which is exactly right -- an account that did not get
    * created must not leave a pruned session table behind. */
   int outer = !db_in_transaction(d);
   if (outer && !db_durable_begin(d))
      return 0;

   /* 1. EVERY EXPIRED ROW, NOT JUST THIS USER'S. A session that has aged out
    * is already refused by session_verify, so keeping it protects nobody; it
    * is simply a dead credential taking space on a memory card. Sweeping
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
    * session_refresh bumps both at once, so for a session in use they say the
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

/* NOT ONE BYTE IS WRITTEN IN HERE, and that is the contract (authsess.h). The
 * writes a verification might be tempted to make are in session_refresh below,
 * where a caller has to name it. */
enum session_check session_verify(struct db *d, const char *cookie,
                                  int64_t *uid, int64_t *last_seen)
{
   if (uid)
      *uid = 0;
   if (last_seen)
      *last_seen = 0;
   if (!cookie || !*cookie)
      return SESSION_NONE;
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie, sel, sizeof sel, &val);
   if (!sel[0])
      return SESSION_NONE;
   sqlite3_stmt *st =
       db_prep(d, "SELECT verifier,user_id,expires_at,last_seen FROM session"
                  " WHERE selector=?");
   /* A LOOKUP THAT DID NOT RUN IS NOT "NOT SIGNED IN". As one answer of 0, a
    * failed read shows a signed-in reader the login form: they retype a
    * password that was never the problem, while the page that is really
    * unavailable says nothing about itself. */
   if (!st)
      return SESSION_UNAVAILABLE;
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   int64_t found = 0, expires = 0, seen = 0;
   char stored[65] = {0};
   int rc          = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(st, 0);
      snprintf(stored, sizeof stored, "%s", v ? v : "");
      found   = (int64_t)sqlite3_column_int64(st, 1);
      expires = (int64_t)sqlite3_column_int64(st, 2);
      seen    = (int64_t)sqlite3_column_int64(st, 3);
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && !db_finished(rc))
      return SESSION_UNAVAILABLE;
   if (!found)
      return SESSION_NONE;
   if (expires < (int64_t)time(NULL))
      return SESSION_EXPIRED;
   char vhash[65];
   sha256_hex(val, strlen(val), vhash);
   if (strlen(stored) != 64 || !ct_eq(stored, vhash, 64))
      return SESSION_NONE;
   if (uid)
      *uid = found;
   if (last_seen)
      *last_seen = seen;
   return SESSION_OK;
}

void session_refresh(struct db *d, const char *cookie, enum session_check what,
                     int64_t last_seen)
{
   if (!cookie || !*cookie)
      return;
   int64_t now = (int64_t)time(NULL);
   switch (what) {
      case SESSION_EXPIRED:
         /* THE ONE PLACE THE ANSWER IS DELIBERATELY DISCARDED, in writing.
          *
          * This is housekeeping, not a revocation the user was promised. The
          * cookie is refused by session_verify whether or not the row goes
          * away -- the refusal is the expiry test, not the delete -- so a
          * delete that did not run costs nothing but a second attempt on the
          * next request with the same cookie. Answering 500 here would turn a
          * full card into "you cannot even be told you are signed out".
          * db_finished has already printed the reason if there was one. */
         (void)session_drop(d, cookie);
         return;
      case SESSION_OK: break;
      /* Nothing was found and nothing is known: there is no row to retire and
       * none to extend. An UNAVAILABLE lookup in particular must not be
       * treated as an expiry -- the row may be perfectly good and merely
       * unreadable this second. */
      case SESSION_NONE:
      case SESSION_UNAVAILABLE: return;
   }
   /* Rolling expiry, at most once a day: an active user never gets logged
    * out, an abandoned session still ages out, and the write happens rarely
    * enough not to touch the SD card on every page view. */
   if (now - last_seen <= SESS_BUMP)
      return;
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie, sel, sizeof sel, &val);
   if (!sel[0])
      return;
   static _Atomic uint64_t bump_fails;
   sqlite3_stmt *up = db_prep(
       d, "UPDATE session SET expires_at=?,last_seen=? WHERE selector=?");
   if (up) {
      sqlite3_bind_int64(up, 1, now + SESS_TTL);
      sqlite3_bind_int64(up, 2, now);
      sqlite3_bind_text(up, 3, sel, -1, SQLITE_STATIC);
   }
   /* The session is STILL VALID either way -- session_verify proved it, and
    * this write only extends it. A failure here means the user may be logged
    * out at the original expiry; it is not a reason to log them out now. */
   (void)auth_maint_step(d, up, "session rolling expiry", &bump_fails);
}

/* SEE authsess.h for why this answers with three values rather than none. */
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
 * failed, those cookies go on signing people in for a year, and the person
 * who changed their password because they thought someone else had it
 * believes they have shut them out. */
int session_drop_all(struct db *d, int64_t uid)
{
   sqlite3_stmt *st = db_prep(d, "DELETE FROM session WHERE user_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
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
