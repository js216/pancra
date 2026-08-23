/* SPDX-License-Identifier: GPL-3.0
 * authsig.c --- the signed app request: its MAC, and its nonce window
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
#include "authsig.h"
#include "authint.h"
#include "ct.h"
#include "db.h"
#include "hmac.h"
#include "proto.h"
#include "sigstr.h"
#include "util.h"
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- signed app requests (the app half is app/sync.c) ----------------- */

/* WAS THIS NONCE NEW? THREE ANSWERS, for the reason authsig.h gives about
 * verify_signature: "the store could not answer" is not "the store says no". */
enum nonce_state { NONCE_FRESH = 0, NONCE_SPENT, NONCE_ERROR };

static enum nonce_state nonce_fresh(struct db *d, int64_t uid,
                                    const char *nonce, int64_t now)
{
   /* Prune first: the table is a sliding window, not a log.
    *
    * REPORTED, because this is the only bound on a table every signed request
    * writes to. Ignored, a prune that has been failing since the card filled
    * up looks exactly like one that has nothing to delete -- and the symptom
    * arrives weeks later as a database that will not open. */
   static _Atomic uint64_t prune_fails;
   sqlite3_stmt *del = db_prep(d, "DELETE FROM nonce WHERE seen_at < ?");
   if (del)
      sqlite3_bind_int64(del, 1, now - 2 * SIG_SKEW);
   (void)auth_maint_step(d, del, "nonce window prune", &prune_fails);
   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO nonce(user_id,nonce,seen_at) VALUES(?,?,?)");
   /* A PREPARE THAT FAILED IS NOT A REPLAY. Answer both with 0 and the caller
    * turns it into 401 -- a database that cannot be read telling a correctly
    * signing app that its nonce was spent. NONCE_ERROR travels so the caller
    * can say 503 instead. */
   if (!st)
      return NONCE_ERROR;
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_text(st, 2, nonce, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, now);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc == SQLITE_DONE)
      return NONCE_FRESH;
   /* THE EXACT TERMINAL RESULT, not "anything else is a replay". A repeat
    * collides on the primary key and that is SQLITE_CONSTRAINT; SQLITE_BUSY,
    * SQLITE_IOERR and SQLITE_CORRUPT are the store failing to answer, and
    * calling those a replay is the same mistake one level down. */
   if (rc == SQLITE_CONSTRAINT)
      return NONCE_SPENT;
   return NONCE_ERROR;
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

/* ---- THE APP KEY, AND THREE REASONS THERE MIGHT NOT BE ONE ---
 *
 * SIG_OK      `key` holds this app's 16-byte shared secret.
 * SIG_BAD     there is demonstrably no usable key: the query RAN to
 *             completion and returned no row, or the row it returned holds
 *             something that is not a 16-byte key. Both are bad credentials
 *             and both are 401.
 * SIG_ERROR   the store could not answer -- BUSY, an I/O error, a corrupt
 *             page, a failed allocation. 503, and RETRYABLE.
 *
 * WHY THE STORE'S FAILURE IS ITS OWN ANSWER. A lookup that sets a `have`
 * flag on SQLITE_ROW and leaves it clear for everything else delivers every
 * operational failure to the caller as SIG_BAD. A correctly paired phone,
 * signing correctly, against a database that is merely locked, is told its
 * credential is invalid --
 * and 401 is the one answer an app must not retry, so a transient fault
 * became a phone that has locked itself out of its own account until
 * somebody re-pairs it. The operator, meanwhile, saw authentication
 * failures rather than a database that could not be read. db_finished is
 * what tells "there is no such app" from "the question did not finish"; it
 * is the same distinction every other loop in this program ends with.
 *
 * FINALIZED ON EVERY PATH, including the ones that answer SIG_ERROR: a
 * statement leaked here is a statement leaked per request, on the path a
 * stranger can reach without holding anything at all. */
static enum sig_verdict app_key_of(struct db *d, int64_t uid, uint8_t key[16])
{
   sqlite3_stmt *st = db_prep(d, "SELECT key FROM app WHERE user_id=?");
   /* THE STORE COULD NOT BE ASKED. This answered "invalid signature", so a
    * correctly paired phone was told its key was wrong. See authsig.h. */
   if (!st)
      return SIG_ERROR;
   sqlite3_bind_int64(st, 1, uid);
   int rc             = sqlite3_step(st);
   enum sig_verdict v = SIG_ERROR;
   if (rc == SQLITE_ROW) {
      /* A ROW WHOSE KEY IS NOT A KEY is a bad credential, not a broken
       * store: nothing but this program writes that column, so a wrong
       * length means the row was made by hand or damaged, and either way
       * there is no secret here to verify against. */
      if (sqlite3_column_bytes(st, 0) == 16 && sqlite3_column_blob(st, 0)) {
         memcpy(key, sqlite3_column_blob(st, 0), 16);
         v = SIG_OK;
      } else {
         v = SIG_BAD;
      }
   } else if (db_finished(rc)) {
      v = SIG_BAD; /* the query finished and this account has no app */
   }
   sqlite3_finalize(st);
   return v;
}

/* ---- THE CHECK, WHICH WRITES NOTHING ---------------------------------
 *
 * ONE FUNCTION THAT VERIFIED A MAC AND, IN THE SAME BREATH, INSERTed the
 * replay nonce and UPDATEd app.last_seen would be named "verify", would read
 * at its callers as a question, and would consume a credential and mutate
 * activity state every time it was asked. That matters beyond
 * tidiness: an operation that reads as observational is one somebody
 * eventually calls twice, or calls to decide whether to do something -- and
 * the second call would answer SIG_REPLAY against the nonce the first call
 * spent, which is a rejection produced entirely by the checking.
 *
 * So the two halves are separate and named for what they do. This half is
 * PURE: it parses the header, looks up the app key, recomputes the MAC and
 * says whether it matches. It may be called as many times as one likes, and
 * asking it changes nothing.
 *
 * On SIG_OK the caller is handed everything the accept step needs (in
 * particular the nonce and the `now` the timestamp was judged against, so the
 * two halves cannot disagree about the instant). */
enum sig_verdict sig_check(const struct req *r, struct sig_check *out)
{
   char auth[256];
   if (!hdr_get(r->hdr, "Authorization", auth, sizeof auth))
      return SIG_BAD;
   if (strncmp(auth, "Pancra ", 7))
      return SIG_BAD;
   char uid_s[32], ts_s[32], nonce[NONCE_MAX + 1], mac_hex[80];
   const char *p = auth + 7;
   /* "<uid>:<ts>:<nonce>:<mac>" -- four fields, no spaces. */
   if (sscanf(p, "%31[^:]:%31[^:]:%64[^:]:%79s", uid_s, ts_s, nonce, mac_hex) !=
       4)
      return SIG_BAD;
   int64_t uid = strtoll(uid_s, NULL, 10);
   int64_t ts  = strtoll(ts_s, NULL, 10);
   int64_t now = (int64_t)time(NULL);
   if (uid <= 0 || !nonce_charset_ok(nonce) || strlen(mac_hex) != 64)
      return SIG_BAD;
   if (ts < now - SIG_SKEW || ts > now + SIG_SKEW)
      return SIG_BAD;

   uint8_t key[16];
   enum sig_verdict kv = app_key_of(r->db, uid, key);
   if (kv != SIG_OK)
      return kv;

   /* THE KEY IS LIVE FROM HERE, AND EVERY EXIT WIPES IT. One
    * variable and one return, because a `return SIG_BAD` in the middle of
    * this block is a copy of a device's shared secret left on a worker's
    * stack -- and this stack is reused by the next request on the same
    * thread. */
   enum sig_verdict v = SIG_BAD;
   char bodyhash[65];
   sha256_hex(r->body ? r->body : "", r->body_len, bodyhash);
   char signing[1024];
   int n = sig_signing_string(signing, sizeof signing, r->method, r->target, ts,
                              nonce, bodyhash);
   if (n > 0) {
      uint8_t want[32], got[32];
      hmac_sha256(key, sizeof key, (const uint8_t *)signing, (size_t)n, want);
      if (hex_to(mac_hex, 64, got) && ct_eq(want, got, 32))
         v = SIG_OK;
      ct_wipe(want, sizeof want);
   }
   ct_wipe(key, sizeof key);
   if (v != SIG_OK)
      return v;
   if (out) {
      out->uid = uid;
      out->now = now;
      /* The nonce is COPIED: `nonce` is a stack buffer of this call, and the
       * accept step runs after it returns. */
      size_t nl = strlen(nonce);
      if (nl >= sizeof out->nonce)
         return SIG_BAD; /* cannot happen: the sscanf width is smaller */
      memcpy(out->nonce, nonce, nl + 1);
   }
   return SIG_OK;
}

/* ---- ACCEPTING THE REQUEST, WHICH IS WHERE THE WRITES ARE -------------
 *
 * Called once per request, and only after sig_check answered SIG_OK. It does
 * the two things that make a verified request an ACCEPTED one:
 *
 *   1. spends the nonce -- which is what makes a replay of these exact bytes
 *      fail from here on, and is therefore the security-bearing write;
 *   2. stamps app.last_seen, which is bookkeeping and nothing else.
 *
 * ONLY REACHED WITH THE MAC ALREADY PROVEN, which is not a stylistic
 * preference: an attacker who could reach the nonce insert with unsigned
 * noise could burn nonces and grow that table at will. The ordering is the
 * whole reason the check comes first.
 *
 * THE TWO WRITES ARE NOT ONE TRANSACTION, on purpose. They have different
 * standing: if the nonce cannot be spent, this request must be refused
 * (SIG_REPLAY or SIG_ERROR) because its replay protection does not exist; if
 * the last_seen stamp fails, the request is still genuine and still served --
 * it is a timestamp on a dashboard. Wrapping them together would mean a
 * failed dashboard stamp rolled back the replay protection of a request the
 * server then answered, which is worse in exactly the way that matters. The
 * stamp's failures are counted and reported by maint_step instead. */
enum sig_verdict sig_accept(struct db *d, const struct sig_check *c)
{
   if (!d || !c)
      return SIG_ERROR;
   switch (nonce_fresh(d, c->uid, c->nonce, c->now)) {
      case NONCE_FRESH: break;
      case NONCE_SPENT: return SIG_REPLAY;
      case NONCE_ERROR: return SIG_ERROR;
   }
   static _Atomic uint64_t seen_fails;
   sqlite3_stmt *up = db_prep(d, "UPDATE app SET last_seen=? WHERE user_id=?");
   if (up) {
      sqlite3_bind_int64(up, 1, c->now);
      sqlite3_bind_int64(up, 2, c->uid);
   }
   /* The signature verified and the nonce was accepted: this request is
    * genuine whatever the stamp does. */
   (void)auth_maint_step(d, up, "app last_seen stamp", &seen_fails);
   return SIG_OK;
}

/* THE POLICY BOUNDARY, and the only thing routing calls: check, then accept.
 * Both halves exist separately so that the writes have a name and a place;
 * this composition exists so that no route can accidentally do only half of
 * it -- a verified request whose nonce was never spent is replayable. */
enum sig_verdict verify_signature(const struct req *r, int64_t *uid_out)
{
   struct sig_check c;
   enum sig_verdict v = sig_check(r, &c);
   if (v != SIG_OK)
      return v;
   v = sig_accept(r->db, &c);
   if (v != SIG_OK)
      return v;
   if (uid_out)
      *uid_out = c.uid;
   return SIG_OK;
}
