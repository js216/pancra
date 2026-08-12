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
#include "sync.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
             uint8_t out[PW_HASH_LEN])
{
   pbkdf2_sha256((const uint8_t *)pw, strlen(pw), salt, PW_SALT_LEN,
                 (unsigned)iters, out, PW_HASH_LEN);
}

long user_by_email(const char *email)
{
   sqlite3_stmt *st = db_prep("SELECT id FROM user WHERE email=?");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   long id = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
      id = (long)sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return id;
}

int user_create(const char *email, const char *pw, long *uid)
{
   /* No length rule beyond "there is one". A minimum stops nobody who wants a
    * weak password -- "password1" cleared the old eight -- and it does turn
    * away a passphrase somebody chose deliberately. Empty is still refused,
    * because that is not a short password, it is no password. */
   if (!email || !*email || !strchr(email, '@') || !pw || !*pw)
      return 0;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   pw_hash(pw, salt, PW_ITERS_DEFAULT, hash);
   sqlite3_stmt *st =
       db_prep("INSERT INTO user(email,pw_salt,pw_hash,pw_iters,created_at)"
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
      *uid = db_last_id();
   return ok;
}

int user_set_password(long uid, const char *pw)
{
   if (!pw || !*pw)
      return 0;
   uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
   rnd_bytes(salt, sizeof salt);
   pw_hash(pw, salt, PW_ITERS_DEFAULT, hash);
   sqlite3_stmt *st =
       db_prep("UPDATE user SET pw_salt=?,pw_hash=?,pw_iters=? WHERE id=?");
   if (!st)
      return 0;
   sqlite3_bind_blob(st, 1, salt, sizeof salt, SQLITE_STATIC);
   sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
   sqlite3_bind_int(st, 3, PW_ITERS_DEFAULT);
   sqlite3_bind_int64(st, 4, uid);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

int user_check_password(long uid, const char *pw)
{
   sqlite3_stmt *st =
       db_prep("SELECT pw_salt,pw_hash,pw_iters FROM user WHERE id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int ok = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const uint8_t *salt = sqlite3_column_blob(st, 0);
      const uint8_t *want = sqlite3_column_blob(st, 1);
      int iters           = sqlite3_column_int(st, 2);
      if (salt && want && sqlite3_column_bytes(st, 0) == PW_SALT_LEN &&
          sqlite3_column_bytes(st, 1) == PW_HASH_LEN) {
         uint8_t got[PW_HASH_LEN];
         pw_hash(pw ? pw : "", salt, iters, got);
         ok = ct_eq(got, want, PW_HASH_LEN);
      }
   }
   sqlite3_finalize(st);
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

int session_new(long uid, char *cookie, size_t cap)
{
   char sel[SELECTOR_HEX + 1], val[VALIDATOR_HEX + 1], vhash[65];
   rnd_hex(sel, SELECTOR_HEX);
   rnd_hex(val, VALIDATOR_HEX);
   sha256_hex(val, strlen(val), vhash);
   sqlite3_stmt *st = db_prep(
       "INSERT INTO session(selector,verifier,user_id,expires_at,last_seen)"
       " VALUES(?,?,?,?,?)");
   if (!st)
      return 0;
   long now = (long)time(NULL);
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, vhash, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, uid);
   sqlite3_bind_int64(st, 4, now + SESS_TTL);
   sqlite3_bind_int64(st, 5, now);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (ok)
      snprintf(cookie, cap, "%s:%s", sel, val);
   return ok;
}

long session_user(const char *cookie)
{
   if (!cookie || !*cookie)
      return 0;
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie, sel, sizeof sel, &val);
   if (!sel[0])
      return 0;
   sqlite3_stmt *st =
       db_prep("SELECT verifier,user_id,expires_at,last_seen FROM session"
               " WHERE selector=?");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   long uid = 0, expires = 0, seen = 0;
   char stored[65] = {0};
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(st, 0);
      snprintf(stored, sizeof stored, "%s", v ? v : "");
      uid     = (long)sqlite3_column_int64(st, 1);
      expires = (long)sqlite3_column_int64(st, 2);
      seen    = (long)sqlite3_column_int64(st, 3);
   }
   sqlite3_finalize(st);
   if (!uid)
      return 0;
   long now = (long)time(NULL);
   if (expires < now) {
      session_drop(cookie);
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
      sqlite3_stmt *up = db_prep(
          "UPDATE session SET expires_at=?,last_seen=? WHERE selector=?");
      if (up) {
         sqlite3_bind_int64(up, 1, now + SESS_TTL);
         sqlite3_bind_int64(up, 2, now);
         sqlite3_bind_text(up, 3, sel, -1, SQLITE_STATIC);
         sqlite3_step(up);
         sqlite3_finalize(up);
      }
   }
   return uid;
}

void session_drop(const char *cookie)
{
   char sel[SELECTOR_HEX + 1];
   const char *val;
   split_cookie(cookie ? cookie : "", sel, sizeof sel, &val);
   if (!sel[0])
      return;
   sqlite3_stmt *st = db_prep("DELETE FROM session WHERE selector=?");
   if (!st)
      return;
   sqlite3_bind_text(st, 1, sel, -1, SQLITE_STATIC);
   sqlite3_step(st);
   sqlite3_finalize(st);
}

void session_drop_all(long uid)
{
   sqlite3_stmt *st = db_prep("DELETE FROM session WHERE user_id=?");
   if (!st)
      return;
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_step(st);
   sqlite3_finalize(st);
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

static int nonce_fresh(long uid, const char *nonce, long now)
{
   /* Prune first: the table is a sliding window, not a log. */
   sqlite3_stmt *del = db_prep("DELETE FROM nonce WHERE seen_at < ?");
   if (del) {
      sqlite3_bind_int64(del, 1, now - 2 * SIG_SKEW);
      sqlite3_step(del);
      sqlite3_finalize(del);
   }
   sqlite3_stmt *st =
       db_prep("INSERT INTO nonce(user_id,nonce,seen_at) VALUES(?,?,?)");
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
   sqlite3_stmt *st = db_prep("SELECT key FROM app WHERE user_id=?");
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
   int n = snprintf(signing, sizeof signing, "%s\n%s\n%ld\n%s\n%s", r->method,
                    r->target, ts, nonce, bodyhash);
   if (n <= 0 || (size_t)n >= sizeof signing)
      return 0;
   uint8_t want[32], got[32];
   hmac_sha256(key, sizeof key, (const uint8_t *)signing, (size_t)n, want);
   if (!hex_to(mac_hex, 64, got))
      return 0;
   if (!ct_eq(want, got, 32))
      return 0;
   /* Only now, with the MAC proven, is the nonce spent: an attacker must not
    * be able to burn nonces (or grow the table) with unsigned noise. */
   if (!nonce_fresh(uid, nonce, now))
      return 0;
   sqlite3_stmt *up = db_prep("UPDATE app SET last_seen=? WHERE user_id=?");
   if (up) {
      sqlite3_bind_int64(up, 1, now);
      sqlite3_bind_int64(up, 2, uid);
      sqlite3_step(up);
      sqlite3_finalize(up);
   }
   return uid;
}

/* ---- login throttle --------------------------------------------------
 *
 * PBKDF2 on one slow core is precisely the work an attacker would like to
 * make this server do: a few hundred guesses a minute would starve the app's
 * own pushes even if none of them ever succeeded. The counter is per email,
 * which is what a guessing attack varies least. */
int login_throttled(const char *email)
{
   sqlite3_stmt *st =
       db_prep("SELECT n,first_at FROM login_fail WHERE email=?");
   if (!st)
      return 0;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   int n      = 0;
   long first = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      n     = sqlite3_column_int(st, 0);
      first = (long)sqlite3_column_int64(st, 1);
   }
   sqlite3_finalize(st);
   if (n >= LOGIN_FAIL_MAX && (long)time(NULL) - first < LOGIN_FAIL_WIN)
      return 1;
   return 0;
}

void login_failed(const char *email)
{
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
   sqlite3_stmt *sw = db_prep("DELETE FROM login_fail WHERE first_at < ?");
   if (sw) {
      sqlite3_bind_int64(sw, 1, now - LOGIN_FAIL_WIN);
      sqlite3_step(sw);
      sqlite3_finalize(sw);
   }
   sqlite3_stmt *st =
       db_prep("INSERT INTO login_fail(email,n,first_at) VALUES(?,1,?)"
               " ON CONFLICT(email) DO UPDATE SET"
               "   n = CASE WHEN first_at < ? THEN 1 ELSE n+1 END,"
               "   first_at = CASE WHEN first_at < ? THEN ? ELSE first_at END");
   if (!st)
      return;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, now);
   sqlite3_bind_int64(st, 3, now - LOGIN_FAIL_WIN);
   sqlite3_bind_int64(st, 4, now - LOGIN_FAIL_WIN);
   sqlite3_bind_int64(st, 5, now);
   sqlite3_step(st);
   sqlite3_finalize(st);
}

void login_ok(const char *email)
{
   sqlite3_stmt *st = db_prep("DELETE FROM login_fail WHERE email=?");
   if (!st)
      return;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   sqlite3_step(st);
   sqlite3_finalize(st);
}
