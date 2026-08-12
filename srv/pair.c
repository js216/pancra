/* SPDX-License-Identifier: GPL-3.0
 * pair.c --- binding one app to one user, with a 6-digit code
 * Copyright 2026 Jakob Kastelic
 *
 * EC-J-PAKE over P-256: the same construction and the same 160-byte round
 * packets a Dexcom G7 pairs with, with a 6-digit code instead of 4. The code
 * never goes on the wire, and the protocol's whole point is that a wrong
 * guess yields nothing an attacker can test offline -- so the ONLY way to
 * search the code space is to keep asking this server, and this server allows
 * three tries.
 *
 * WHY THERE IS A FOURTH STEP. J-PAKE's zero-knowledge proofs validate
 * whatever password each side used: two parties with different codes complete
 * all three rounds happily and simply end up with different keys. Over BLE the
 * sensor's next challenge exposes that at once, but an HTTP pairing that ended
 * at round 3 would report success and then fail later as unexplained
 * signature errors. So each side proves it derived the same key with a MAC
 * over a fixed label.
 *
 * That confirmation cannot ride along IN round 3: the key depends on the
 * PEER's round 3, so when the server builds its round-3 reply the app has not
 * derived its key yet, and vice versa. Hence step 4, which carries only the
 * app's proof. The server's proof goes out with round 3, because by then the
 * server has everything it needs.
 *
 * ONE attempt is in flight at a time. Pairing is a rare, deliberate act by a
 * person reading digits off a screen, and the server serves one request at a
 * time anyway, so a table of concurrent sessions would be state without a
 * purpose.
 */
#include "pair.h"
#include "auth.h"
#include "ct.h"
#include "db.h"
#include "hmac.h"
#include "http.h"
#include "jpake.h"
#include "sync.h"
#include "util.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIRM_HEX 32 /* 16 bytes of MAC, hex */

static struct {
   char sess[33];
   long uid;
   struct jpake *p;
   double started;
   int round;       /* rounds completed so far */
   uint8_t key[16]; /* derived at round 3, confirmed at step 4 */
   int have_key;
} cur;

/* ONE pairing at a time, and it spans four requests. Those requests may now
 * land on different workers, so the whole round is taken under this lock --
 * which also preserves the old rule that a second person starting a pairing
 * displaces the first, rather than the two interleaving into a key neither
 * of them agreed to. */
static pthread_mutex_t pair_mu = PTHREAD_MUTEX_INITIALIZER;

void pair_lock(void)
{
   pthread_mutex_lock(&pair_mu);
}

void pair_unlock(void)
{
   pthread_mutex_unlock(&pair_mu);
}

static void pair_reset(void)
{
   if (cur.p)
      jpake_free(cur.p);
   memset(&cur, 0, sizeof cur);
}

static int pair_live(void)
{
   if (!cur.p)
      return 0;
   if (http_mono_s() - cur.started > PAIR_SESS_TTL) {
      pair_reset();
      return 0;
   }
   return 1;
}

/* Charge one attempt against the displayed code, and burn the code once the
 * budget is gone. This is the only thing standing between a 6-digit secret
 * and an exhaustive search.
 *
 * The try is charged when an attempt STARTS, not when one is seen to fail.
 * Neither side can derive the key until it has the other's round 3, so a
 * guesser learns their code was wrong at a point where they could simply stop
 * talking -- and a counter that only advanced on a reported failure would
 * never advance at all. Only a completed, confirmed pairing clears it. */
static void pair_charge(long uid)
{
   sqlite3_stmt *st =
       db_prep("UPDATE pairing SET tries=tries+1 WHERE user_id=?");
   if (st) {
      sqlite3_bind_int64(st, 1, uid);
      sqlite3_step(st);
      sqlite3_finalize(st);
   }
   st = db_prep("DELETE FROM pairing WHERE user_id=? AND tries>=?");
   if (st) {
      sqlite3_bind_int64(st, 1, uid);
      sqlite3_bind_int(st, 2, PAIR_TRIES);
      sqlite3_step(st);
      sqlite3_finalize(st);
   }
}

int pair_code_new(long uid, char *out, size_t cap)
{
   /* Uniform over 000000..999999. Rejection sampling rather than a modulo of
    * a random byte string, which would make the low codes fractionally more
    * likely -- a small bias, but there is no reason to accept any. */
   uint32_t v;
   do {
      uint8_t b[4];
      rnd_bytes(b, 4);
      v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
          ((uint32_t)b[2] << 8) | b[3];
   } while (v >= 4294000000u);
   snprintf(out, cap, "%06u", (unsigned)(v % 1000000u));

   sqlite3_stmt *st = db_prep(
       "INSERT INTO pairing(user_id,code,expires_at,tries) VALUES(?,?,?,0)"
       " ON CONFLICT(user_id) DO UPDATE SET code=excluded.code,"
       "   expires_at=excluded.expires_at, tries=0");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_text(st, 2, out, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, (long)time(NULL) + PAIR_CODE_TTL);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

void pair_unpair(long uid)
{
   sqlite3_stmt *st = db_prep("DELETE FROM app WHERE user_id=?");
   if (st) {
      sqlite3_bind_int64(st, 1, uid);
      sqlite3_step(st);
      sqlite3_finalize(st);
   }
   if (cur.uid == uid)
      pair_reset();
}

int pair_is_paired(long uid)
{
   int found = 0;
   db_one_long("SELECT 1 FROM app WHERE user_id=?", uid, &found);
   return found;
}

/* The live code for a user, or 0 if there is none, it expired, or its try
 * budget is spent. */
static int pair_code_for(long uid, char *code, size_t cap)
{
   sqlite3_stmt *st =
       db_prep("SELECT code,expires_at,tries FROM pairing WHERE user_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int ok = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *c = (const char *)sqlite3_column_text(st, 0);
      long exp      = (long)sqlite3_column_int64(st, 1);
      int tries     = sqlite3_column_int(st, 2);
      if (c && exp > (long)time(NULL) && tries < PAIR_TRIES) {
         snprintf(code, cap, "%s", c);
         ok = 1;
      }
   }
   sqlite3_finalize(st);
   return ok;
}

static void confirm_mac(const uint8_t key[16], const char *label, char *out)
{
   uint8_t mac[32];
   hmac_sha256(key, 16, (const uint8_t *)label, strlen(label), mac);
   char hex[65];
   hex_of(mac, 32, hex);
   snprintf(out, CONFIRM_HEX + 1, "%.*s", CONFIRM_HEX, hex);
}

/* Split "<first line>\n<hex packet>[\n<confirm>]". */
static int split_body(const struct req *r, char *first, size_t fcap,
                      uint8_t pkt[PAIR_PKT], char *confirm, size_t ccap)
{
   const char *b  = r->body;
   size_t n       = r->body_len;
   const char *nl = memchr(b, '\n', n);
   if (!nl)
      return 0;
   size_t flen = (size_t)(nl - b);
   if (flen == 0 || flen >= fcap)
      return 0;
   memcpy(first, b, flen);
   first[flen]     = '\0';
   const char *hex = nl + 1;
   size_t left     = n - flen - 1;
   if (left < 2 * PAIR_PKT)
      return 0;
   if (!hex_to(hex, 2 * PAIR_PKT, pkt))
      return 0;
   if (confirm) {
      confirm[0]      = '\0';
      const char *nl2 = memchr(hex, '\n', left);
      if (nl2) {
         size_t clen = left - (size_t)(nl2 + 1 - hex);
         while (clen && (nl2[clen] == '\r' || nl2[clen] == '\n'))
            clen--;
         if (clen >= ccap)
            clen = ccap - 1;
         memcpy(confirm, nl2 + 1, clen);
         confirm[clen] = '\0';
         char *cr      = strpbrk(confirm, "\r\n");
         if (cr)
            *cr = '\0';
      }
   }
   return 1;
}

static void h_pair_confirm(struct req *r, const char *sent);

void h_pair(struct req *r, int round)
{
   if (strcmp(r->method, "POST")) {
      http_text(r->fd, 405, "Method Not Allowed", "POST only\n");
      return;
   }
   char first[128], confirm[CONFIRM_HEX + 1];
   uint8_t pkt[PAIR_PKT], out[PAIR_PKT];
   if (round == 4) {
      /* "<session>\n<confirmation>" -- no packet in this one. */
      char line[256];
      size_t n = r->body_len < sizeof line - 1 ? r->body_len : sizeof line - 1;
      memcpy(line, r->body, n);
      line[n]  = '\0';
      char *nl = strchr(line, '\n');
      if (!nl) {
         http_text(r->fd, 400, "Bad Request", "expected '<session>\\n<mac>'\n");
         return;
      }
      *nl        = '\0';
      char *sent = nl + 1;
      char *end  = strpbrk(sent, "\r\n");
      if (end)
         *end = '\0';
      /* ct_eq on the session id, like the confirmation below. It is a
       * 128-bit secret handed out at round 1 and presented on every later
       * round, so a compare that returns at the first differing byte tells a
       * guesser how much of a guess was right. The length is checked first so
       * ct_eq always sees two equal spans. */
      if (!pair_live() || !cur.have_key || strlen(line) != strlen(cur.sess) ||
          !ct_eq((const uint8_t *)line, (const uint8_t *)cur.sess,
                 strlen(cur.sess))) {
         http_text(r->fd, 409, "Conflict", "no such pairing session\n");
         return;
      }
      h_pair_confirm(r, sent);
      return;
   }
   if (!split_body(r, first, sizeof first, pkt, confirm, sizeof confirm)) {
      http_text(r->fd, 400, "Bad Request", "expected '<id>\\n<320 hex>'\n");
      return;
   }
   char hex[2 * PAIR_PKT + 1];

   if (round == 1) {
      long uid = user_by_email(first);
      /* A second attempt by the SAME user simply replaces the first: somebody
       * mistyping a code and starting over must not be locked out by their
       * own abandoned session. Another user's live attempt is not displaced. */
      if (pair_live() && cur.uid != uid) {
         http_text(r->fd, 409, "Conflict", "another pairing is in flight\n");
         return;
      }
      pair_reset();
      char code[PAIR_CODE_LEN + 1];
      /* One answer for "no such user", "no code showing" and "code expired".
       * Distinguishing them would turn this route into a way to test which
       * email addresses have accounts. */
      if (!uid || !pair_code_for(uid, code, sizeof code)) {
         http_text(r->fd, 403, "Forbidden", "no pairing code is active\n");
         return;
      }
      cur.p = jpake_new((const uint8_t *)code, strlen(code), 0 /* server */);
      if (!cur.p) {
         oops(r);
         return;
      }
      cur.uid     = uid;
      cur.started = http_mono_s();
      cur.round   = 0;
      rnd_hex(cur.sess, 32);
      /* Charged here, before anything else can go wrong: see pair_charge. */
      pair_charge(uid);
      if (!jpake_peer_round1(cur.p, pkt) || !jpake_round1(cur.p, out)) {
         pair_reset();
         http_text(r->fd, 400, "Bad Request", "round 1 rejected\n");
         return;
      }
      cur.round = 1;
      hex_of(out, PAIR_PKT, hex);
      struct sb sb = {0};
      sb_add(&sb, "%s\n%s\n", cur.sess, hex);
      http_respond(r->fd, 200, "OK", "text/plain", sb.p, sb.n);
      sb_free(&sb);
      return;
   }

   if (!pair_live() || strlen(first) != strlen(cur.sess) ||
       !ct_eq((const uint8_t *)first, (const uint8_t *)cur.sess,
              strlen(cur.sess)) ||
       cur.round != round - 1) {
      http_text(r->fd, 409, "Conflict", "no such pairing session\n");
      return;
   }

   if (round == 2) {
      if (!jpake_peer_round2(cur.p, pkt) || !jpake_round2(cur.p, out)) {
         pair_reset();
         http_text(r->fd, 400, "Bad Request", "round 2 rejected\n");
         return;
      }
      cur.round = 2;
      hex_of(out, PAIR_PKT, hex);
      struct sb sb = {0};
      sb_add(&sb, "%s\n", hex);
      http_respond(r->fd, 200, "OK", "text/plain", sb.p, sb.n);
      sb_free(&sb);
      return;
   }

   /* Round 3 finishes the exchange and derives the key. It CANNOT confirm it:
    * the key depends on the peer's round 3, so at the moment this reply is
    * built the app has not derived its own key yet. Confirmation is step 4. */
   if (!jpake_peer_round3(cur.p, pkt) || !jpake_round3(cur.p, out)) {
      pair_reset();
      http_text(r->fd, 400, "Bad Request", "round 3 rejected\n");
      return;
   }
   if (!jpake_shared_key(cur.p, cur.key)) {
      pair_reset();
      oops(r);
      return;
   }
   cur.have_key = 1;
   cur.round    = 3;
   char mine[CONFIRM_HEX + 1];
   confirm_mac(cur.key, "pancra-confirm-server", mine);
   hex_of(out, PAIR_PKT, hex);
   struct sb sb = {0};
   sb_add(&sb, "%s\n%s\n", hex, mine);
   http_respond(r->fd, 200, "OK", "text/plain", sb.p, sb.n);
   sb_free(&sb);
}

/* Step 4: the app proves it derived the SAME key, which is the only evidence
 * that it had the right code. Without this the pairing would "succeed" on a
 * wrong code and fail later as unexplained signature errors. */
static void h_pair_confirm(struct req *r, const char *sent)
{
   long uid = cur.uid;
   char want[CONFIRM_HEX + 1];
   confirm_mac(cur.key, "pancra-confirm-client", want);
   if (strlen(sent) != CONFIRM_HEX || !ct_eq(want, sent, CONFIRM_HEX)) {
      pair_reset();
      http_text(r->fd, 403, "Forbidden", "wrong pairing code\n");
      return;
   }
   sqlite3_stmt *st = db_prep(
       "INSERT INTO app(user_id,key,paired_at,last_seen) VALUES(?,?,?,?)"
       " ON CONFLICT(user_id) DO UPDATE SET key=excluded.key,"
       "   paired_at=excluded.paired_at, last_seen=excluded.last_seen");
   if (!st) {
      pair_reset();
      oops(r);
      return;
   }
   long now = (long)time(NULL);
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_blob(st, 2, cur.key, 16, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, now);
   sqlite3_bind_int64(st, 4, now);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok) {
      pair_reset();
      oops(r);
      return;
   }
   /* The code has done its job; leaving it live would be a second key to the
    * same door. */
   sqlite3_stmt *dp = db_prep("DELETE FROM pairing WHERE user_id=?");
   if (dp) {
      sqlite3_bind_int64(dp, 1, uid);
      sqlite3_step(dp);
      sqlite3_finalize(dp);
   }
   char msg[32];
   snprintf(msg, sizeof msg, "%ld\n", uid);
   http_text(r->fd, 200, "OK", msg);
   pair_reset();
}
