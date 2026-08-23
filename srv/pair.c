/* SPDX-License-Identifier: GPL-3.0
 * pair.c --- binding one app to one user, with a 6-digit code
 * Copyright 2026 Jakob Kastelic
 *
 * EC-J-PAKE over P-256: the same construction and the same 160-byte round
 * packets a Dexcom G7 pairs with, with a 6-digit code rather than 4. The code
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
#include "authuser.h"
#include "ct.h"
#include "db.h"
#include "hmac.h"
#include "http.h"
#include "jpake.h"
#include "oops.h"
#include "proto.h"
#include "route.h" /* route_allow(RT_PAIR): the methods this route declares */
#include "util.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
   char sess[33];
   int64_t uid;
   struct jpake *p;
   double started;
   int round;       /* rounds completed so far */
   uint8_t key[16]; /* derived at round 3, confirmed at step 4 */
   int have_key;
} cur;

/* ONE pairing at a time, and it spans four requests. Those requests may now
 * land on different workers, so the whole round is taken under this lock --
 * without it two exchanges would interleave into a key neither party agreed
 * to.
 *
 * A SECOND STARTER NO LONGER DISPLACES THE FIRST. That is what the lock used
 * to preserve, and it was the wrong rule: the "second starter" needed nothing
 * but an email address. Round 1 refuses instead; see pair_in_flight. */
static pthread_mutex_t pair_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- THE SERIALISATION IS THIS MODULE'S, NOT ITS CALLERS' ----
 *
 * These were a lock/unlock PAIR exported from pair.h, and every caller had
 * to bracket its own call. That is a rule written in a header and
 * enforced nowhere, and it had already been broken once: the settings page's
 * Unpair handler called pair_unpair() without the lock, and pair_unpair
 * reaches pair_reset(), which jpake_free()s the in-flight exchange -- a
 * use-after-free on a live crypto context while another worker was mid-round.
 * Clicking Unpair on the website while a phone was pairing was the whole
 * reproduction.
 *
 * A shared object whose lock is the CALLER's responsibility has one failure
 * mode and it is that one. So the lock is private now: every entry point in
 * this file that touches `cur` takes it itself, and there is nothing in
 * pair.h to forget. */
static void pair_hold(void)
{
   pthread_mutex_lock(&pair_mu);
}

static void pair_release(void)
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

/* ---- AN EXCHANGE THAT STILL NEEDS THE SERVER -------------------------
 *
 * `pair_live` answers "is there an exchange" -- which is what rounds 2, 3 and
 * step 4 have to ask, because that is the state they act on. This answers a
 * narrower question: is there an exchange whose peer is still mid-
 * conversation, so that starting a new one would ABORT somebody?
 *
 * The difference is round 3. Once the server has answered round 3 it has
 * given the peer everything it has to give -- the last packet and its own
 * confirmation MAC -- and the only message left is the peer's step-4 proof,
 * which the peer sends immediately and with no human in the loop. An exchange
 * at round 1 or 2 is one the server is still being talked to about; an
 * exchange at round 3 is finished from the server's side and is being kept
 * only so step 4 has something to check.
 *
 * WHY THE LINE IS DRAWN THERE AND NOT AT "LIVE". A person who mistypes the
 * code runs the whole exchange: the app gets to round 3, finds the server's
 * confirmation does not match the key it derived, and stops WITHOUT sending
 * step 4 -- it will not save a key from a server it cannot verify. That
 * abandoned round-3 state then sits here for the rest of PAIR_SESS_TTL. If it
 * blocked a new round 1, the commonest pairing failure there is -- a wrong
 * digit -- would cost the person two minutes before they could try again,
 * with nothing on screen to explain the wait. Drawing the line at round 3
 * keeps the retry immediate.
 *
 * THE RACE THAT IS LEFT, said plainly rather than left to be found: between
 * the server's round-3 reply and the peer's step 4 there is one network
 * round-trip in which a new round 1 can still displace a pending
 * confirmation, and the honest phone then gets "no such pairing session" on
 * its step 4. It is not a new hole -- a stranger who wins the slot at round 1
 * blocks that phone just as completely -- and it is bounded by the same try
 * budget, because every exchange that gets established is charged. Closing it
 * would mean keeping a second slot for confirmations awaiting step 4, which
 * is state with one purpose: winning a race measured in milliseconds. */
static int pair_in_flight(void)
{
   return pair_live() && cur.round < 3;
}

/* Charge one attempt against the displayed code, and burn the code once the
 * budget is gone. This is the only thing standing between a 6-digit secret
 * and an exhaustive search.
 *
 * The try is charged when an exchange is ESTABLISHED, not when one is seen to
 * fail. Neither side can derive the key until it has the other's round 3, so a
 * guesser learns their code was wrong at a point where they could simply stop
 * talking -- and a counter that only advanced on a reported failure would
 * never advance at all. Only a completed, confirmed pairing clears it.
 *
 * "Established" is a precise point and the round-1 handler states it in full:
 * after the peer's packet has passed its zero-knowledge proof, and never
 * before. Charged "on arrival" instead, the owner pays for a stranger's 320
 * zeros -- see the rule written out in h_pair. */
/* 1 when the try was CHARGED and the budget enforced; 0 when the database
 * would not do it.
 *
 * This is the only thing standing between a 6-digit secret and an exhaustive
 * search, and it is not `void`: with both statements' results dropped, a
 * database that cannot prepare or step simply does not count the attempt --
 * and the exchange goes ahead anyway. A guesser who can keep the database
 * busy, or who finds it wedged, has an unlimited budget against a
 * million-value code. The caller refuses the round instead. */
/* THE INCREMENT AND THE BURN ARE ONE TRANSACTION.
 *
 * They were two statements with two results, and the failure between them is
 * the one that matters: the count lands, the DELETE that burns a spent code
 * does not, and the code stays live with `tries` climbing past PAIR_TRIES --
 * the budget silently gone, on the one secret a six-digit search would
 * otherwise take a million guesses to find. Checking both results (which this
 * did) refuses the ROUND, but the increment it already committed stays
 * behind; the next attempt then starts from a count nothing will ever act on.
 *
 * Inside BEGIN IMMEDIATE both land or neither does. IMMEDIATE, not deferred:
 * the write lock is taken up front, so two rounds for one user cannot both
 * read `tries` and both write it. */
static int pair_charge(struct db *d, int64_t uid)
{
   if (!db_exec(d, "BEGIN IMMEDIATE;"))
      return 0;
   sqlite3_stmt *st =
       db_prep(d, "UPDATE pairing SET tries=tries+1 WHERE user_id=?");
   int ok = st != NULL;
   if (ok) {
      sqlite3_bind_int64(st, 1, uid);
      /* db_finished, not `== SQLITE_DONE`: BUSY, IOERR and CORRUPT are all
       * "it did not run", and the rule this file follows everywhere else is
       * that they are SAID so rather than folded into a bare failure. The
       * caller answers 500 either way; without this the log does not explain
       * it. */
      int rc = sqlite3_step(st);
      ok     = db_finished(rc);
      if (!ok)
         fprintf(stderr, "sync: pairing charge did not run: %s\n",
                 sqlite3_errstr(rc));
      sqlite3_finalize(st);
   }
   if (ok) {
      /* The budget is enforced in the same breath: a charge that lands while
       * the burn does not lets tries climb past PAIR_TRIES with the code
       * still live. */
      st = db_prep(d, "DELETE FROM pairing WHERE user_id=? AND tries>=?");
      ok = st != NULL;
      if (ok) {
         sqlite3_bind_int64(st, 1, uid);
         sqlite3_bind_int(st, 2, PAIR_TRIES);
         int rc = sqlite3_step(st);
         ok     = db_finished(rc);
         if (!ok)
            fprintf(stderr, "sync: pairing burn did not run: %s\n",
                    sqlite3_errstr(rc));
         sqlite3_finalize(st);
      }
   }
   if (!ok || !db_exec(d, "COMMIT;")) {
      /* Nothing was charged and nothing was burned. The caller refuses the
       * round, which is what keeps the budget honest either way. */
      (void)db_exec(d, "ROLLBACK;");
      return 0;
   }
   return 1;
}

int pair_code_new(struct db *d, int64_t uid, char *out, size_t cap)
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
       d, "INSERT INTO pairing(user_id,code,expires_at,tries) VALUES(?,?,?,0)"
          " ON CONFLICT(user_id) DO UPDATE SET code=excluded.code,"
          "   expires_at=excluded.expires_at, tries=0");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_text(st, 2, out, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, (int64_t)time(NULL) + PAIR_CODE_TTL);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok;
}

/* 1 when the app key is GONE from the database.
 *
 * "Unpair" is a revocation: the phone's key stops being accepted. Reported
 * as done while the DELETE failed, the settings page says the app is
 * unpaired and the stored key goes on signing requests -- which is the whole
 * of what unpairing is for. The in-memory exchange is still reset either
 * way; that part cannot fail, and leaving a half-finished pairing running
 * after the user asked to stop would be worse. */
int pair_unpair(struct db *d, int64_t uid)
{
   /* THE LOCK IS TAKEN HERE, not by the caller: a handler that forgets it
    * leaves this function's pair_reset() jpake_free()ing an exchange another
    * worker is in the middle of. The delete itself needs no lock; what does
    * is the reset below,
    * which is why it cannot be left to whoever calls this next. */
   pair_hold();
   sqlite3_stmt *st = db_prep(d, "DELETE FROM app WHERE user_id=?");
   int ok           = 0;
   if (st) {
      sqlite3_bind_int64(st, 1, uid);
      ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
   }
   if (cur.uid == uid)
      pair_reset();
   pair_release();
   return ok;
}

int pair_is_paired(struct db *d, int64_t uid)
{
   switch (db_get_long(d, "SELECT 1 FROM app WHERE user_id=?", uid, NULL)) {
      case DB_GET_VALUE: return 1;
      case DB_GET_NONE: return 0;
      case DB_GET_FAIL: break;
   }
   return -1;
}

/* The live code for a user, or 0 if there is none, it expired, or its try
 * budget is spent. */
static int pair_code_for(struct db *d, int64_t uid, char *code, size_t cap)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT code,expires_at,tries FROM pairing WHERE user_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int ok = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *c = (const char *)sqlite3_column_text(st, 0);
      int64_t exp   = (int64_t)sqlite3_column_int64(st, 1);
      int tries     = sqlite3_column_int(st, 2);
      if (c && exp > (int64_t)time(NULL) && tries < PAIR_TRIES) {
         snprintf(code, cap, "%s", c);
         ok = 1;
      }
   }
   sqlite3_finalize(st);
   return ok;
}

/* THE CONSTRUCTION IS lib/pairtag.h's. This wrapper is what is
 * left of confirm_mac: the key length this protocol uses, and a refusal that
 * leaves `out` empty -- which no tag equals, so a failure to build one cannot
 * become an accidental match. */
static void confirm_mac(const uint8_t key[16], const char *label, char *out)
{
   (void)pair_tag(key, 16, label, out, CONFIRM_HEX + 1);
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

static void h_pair_locked(struct req *r, int round);

void h_pair(struct req *r, int round)
{
   /* POST ONLY, and route_api has already refused everything else before it
    * took the pairing lock (see srv/sync.c). Checked again here rather than
    * trusted: this is the one handler that runs with no authentication at all,
    * and a route_allow mask is a declaration a future caller could forget to
    * enforce. The set comes from route_allow so the two cannot disagree, and
    * the refusal carries the `Allow` header a 405 owes (RFC 9110 9.5.5),
    * rather than the word "POST" in the BODY, which no client parses. */
   if (!(http_method_bit(r->method) & route_allow(RT_PAIR))) {
      http_method_not_allowed(r->c, route_allow(RT_PAIR));
      return;
   }
   /* ONE EXCHANGE AT A TIME, AND THIS FUNCTION IS WHERE THAT IS ENFORCED
    *. The lock is private to this function, so there is nothing for a route
    * to forget. Taken AFTER the
    * method check, deliberately: a stranger sending GET /v1/pair/1 in a loop
    * must not serialise every real pairing round behind requests that are
    * going to be refused. */
   pair_hold();
   h_pair_locked(r, round);
   pair_release();
}

static void h_pair_locked(struct req *r, int round)
{
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
         http_text(r->c, 400, "Bad Request", "expected '<session>\\n<mac>'\n");
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
         http_text(r->c, 409, "Conflict", "no such pairing session\n");
         return;
      }
      h_pair_confirm(r, sent);
      return;
   }
   if (!split_body(r, first, sizeof first, pkt, confirm, sizeof confirm)) {
      http_text(r->c, 400, "Bad Request", "expected '<id>\\n<320 hex>'\n");
      return;
   }
   char hex[2 * PAIR_PKT + 1];

   if (round == 1) {
      /* ---- A NAME IS NOT A CREDENTIAL -----------------------------------
       *
       * The only thing round 1 carries about WHO is asking is an email
       * address, and an email address is not a secret: it is on the account
       * holder's business card. So nothing this request says about itself may
       * be allowed to interfere with an exchange that is already running.
       *
       * WHAT A NARROWER TEST COSTS. Under `pair_live() && cur.uid != uid`
       * only an exchange belonging to ANOTHER user is protected; a live
       * exchange belonging to the named user is destroyed by the
       * unconditional `pair_reset()` on the next line, before one byte of the
       * request has been checked against anything. Charge the try a few lines
       * further down, still before the peer's packet is validated, and the
       * two together hand a stranger who knows nothing but the address
       * this:
       *
       *   POST /v1/pair/1   "jk@example.com\n" + 320 zeros
       *
       * which aborts whatever the owner's phone is in the middle of AND
       * spends one of the three tries the six-digit code is protected by.
       * Three of those -- three requests, no secrets, no crypto -- burn the
       * code. The owner sees their phone fail to pair, mints a new code, and
       * has it burned again. There is no number of retries that wins that,
       * because the attacker's cost per code is three HTTP requests.
       *
       * THE RULE: an exchange the peer is still mid-conversation with is
       * refused a replacement, whoever asks, until it expires. See
       * pair_in_flight for where "still mid-conversation" ends and for the
       * one race that remains.
       *
       * WHY CONFLICT-UNTIL-EXPIRY RATHER THAN A CANCEL CAPABILITY. The item
       * this fixes offered both: refuse until expiry, or require the caller
       * to prove it holds something unguessable from the existing exchange
       * (its 128-bit session id) before cancelling it. A capability is the
       * more flexible design and it is the wrong one HERE, for a reason that
       * has nothing to do with elegance: the capability would have to travel
       * in the round-1 body, and the round-1 body's shape is fixed by
       * app/sync.c and by every copy of the app already installed. A server
       * that required a capability the shipped app does not send would refuse
       * every retry after the first, which is a worse failure than the one
       * being fixed. Refusing costs no protocol change at all.
       *
       * THE COST OF CHOOSING IT, stated because somebody will pay it: a phone
       * that dies in the middle of an exchange -- killed, out of signal,
       * screen off at the wrong moment -- leaves state here that nothing can
       * clear early, and its owner must wait out PAIR_SESS_TTL (120s) before
       * a retry is accepted. They get "another pairing is in flight" and no
       * way to hurry it. Two minutes is the ceiling on that, which is why the
       * TTL being short matters; if it were ten minutes this trade would not
       * be worth making. The ordinary wrong-digit retry does NOT pay it, for
       * the reason given on pair_in_flight.
       *
       * The refusal says the same thing whoever it is talking to, so it is
       * not an oracle for "is this account pairing right now" pointed at
       * anybody else's account either. */
      if (pair_in_flight()) {
         http_text(r->c, 409, "Conflict", "another pairing is in flight\n");
         return;
      }
      /* Nothing live is being destroyed here now -- pair_in_flight has just
       * said so -- but expired or already-concluded state has to go before a
       * new exchange is built on top of it. */
      pair_reset();
      /* THE SAME ADDRESS RULE AS EVERY OTHER SURFACE, and this one had its
       * OWN, quietly: split_body refuses a first line that does not fit
       * `first`, so the bound here was 127 bytes rather than 254 -- a third
       * number, in a third file, for the same question. It writes nothing and
       * counts nothing, so no row was ever at stake; what was at stake is that
       * "which account is this?" must have one answer everywhere. Canonicalised
       * so a phone that was paired against "JK@X" reaches the same account as
       * the browser that signed in as "jk@x", and refused (as "no pairing code
       * is active", below, which is the one answer this route gives to every
       * unknown address) when it is not an address this server acts on. */
      char cemail[EMAIL_BUF];
      /* THE CLIENT'S ANSWER DOES NOT CHANGE, and that is deliberate: one
       * reply for "no such user", "no code showing" and "code expired" is what
       * stops this route being an address oracle. But the SERVER should still
       * know the difference, because "the database did not answer" is an
       * operational fault and it looked exactly like an unknown address in
       * every log this route produced.
       *
       * So the failure output is read and logged, and the reply below is left
       * alone. Discarding it was the only way the two could not be told
       * apart. */
      int pfailed = 0;
      int64_t uid = email_canon(first, cemail, sizeof cemail)
                        ? user_by_email(r->db, cemail, &pfailed)
                        : 0;
      if (pfailed)
         fprintf(stderr, "sync: pairing lookup did not run; answering as "
                         "though the address were unknown\n");
      char code[PAIR_CODE_LEN + 1];
      /* One answer for "no such user", "no code showing" and "code expired".
       * Distinguishing them would turn this route into a way to test which
       * email addresses have accounts. */
      if (!uid || !pair_code_for(r->db, uid, code, sizeof code)) {
         http_text(r->c, 403, "Forbidden", "no pairing code is active\n");
         return;
      }
      cur.p = jpake_new((const uint8_t *)code, strlen(code), 0 /* server */);
      if (!cur.p) {
         oops(r);
         return;
      }
      /* ---- WHEN A ROUND ONE COSTS THE OWNER A GUESS ---------------------
       *
       * STATED, not left to fall out of the order of the code: A TRY IS
       * CHARGED EXACTLY ONCE PER EXCHANGE THAT GETS ESTABLISHED, and an
       * exchange is established only when the body parsed, the account was
       * found, a live code was found, and the peer's round-1 packet passed
       * its zero-knowledge proof. Every refusal before that point -- a
       * malformed body, unparsable hex, an unknown address, no live code, a
       * ZKP the packet fails, and the conflict above -- costs NOTHING.
       *
       * WHY A MALFORMED ROUND ONE MUST BE FREE. In EC-J-PAKE the password
       * does not enter until round 2: round 1 is two ephemeral public keys
       * and their Schnorr proofs, and it is identical whatever the code is.
       * A round-1 packet therefore cannot be a guess at the code, and a
       * malformed one is not even a packet. Charging it did not slow an
       * attacker down by one guess -- it let a stranger spend the OWNER's
       * budget with 320 zeros, which is the burn described above.
       *
       * WHY THE CHARGE STILL HAPPENS HERE AND NOT LATER. Neither side can
       * derive the key before it holds the other's round 3, so a guesser
       * learns their code was wrong at a point where they can simply stop
       * talking; a counter that only advanced on a reported failure would
       * never advance at all. Charging at the moment an exchange comes into
       * existence is the last point that is still both unavoidable and
       * unforgeable. An exchange that is abandoned after this line has cost a
       * try, and that is correct: the slot was taken.
       *
       * FAIL CLOSED. An attempt that could not be counted must not be made,
       * or a six-digit code faces unlimited guesses -- so the round is
       * refused when the charge does not land, and the exchange goes with
       * it. */
      if (!jpake_peer_round1(cur.p, pkt) || !jpake_round1(cur.p, out)) {
         pair_reset();
         http_text(r->c, 400, "Bad Request", "round 1 rejected\n");
         return;
      }
      cur.uid     = uid;
      cur.started = http_mono_s();
      cur.round   = 0;
      if (!rnd_hex(cur.sess, sizeof cur.sess, 32)) {
         pair_reset();
         oops(r);
         return;
      }
      if (!pair_charge(r->db, uid)) {
         pair_reset();
         oops(r);
         return;
      }
      cur.round = 1;
      hex_of(out, PAIR_PKT, hex);
      struct sb sb = {0};
      sb_add(&sb, "%s\n%s\n", cur.sess, hex);
      http_respond(r->c, 200, "OK", "text/plain", sb.p, sb.n);
      sb_free(&sb);
      return;
   }

   if (!pair_live() || strlen(first) != strlen(cur.sess) ||
       !ct_eq((const uint8_t *)first, (const uint8_t *)cur.sess,
              strlen(cur.sess)) ||
       cur.round != round - 1) {
      http_text(r->c, 409, "Conflict", "no such pairing session\n");
      return;
   }

   if (round == 2) {
      if (!jpake_peer_round2(cur.p, pkt) || !jpake_round2(cur.p, out)) {
         pair_reset();
         http_text(r->c, 400, "Bad Request", "round 2 rejected\n");
         return;
      }
      cur.round = 2;
      hex_of(out, PAIR_PKT, hex);
      struct sb sb = {0};
      sb_add(&sb, "%s\n", hex);
      http_respond(r->c, 200, "OK", "text/plain", sb.p, sb.n);
      sb_free(&sb);
      return;
   }

   /* Round 3 finishes the exchange and derives the key. It CANNOT confirm it:
    * the key depends on the peer's round 3, so at the moment this reply is
    * built the app has not derived its own key yet. Confirmation is step 4. */
   if (!jpake_peer_round3(cur.p, pkt) || !jpake_round3(cur.p, out)) {
      pair_reset();
      http_text(r->c, 400, "Bad Request", "round 3 rejected\n");
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
   confirm_mac(cur.key, CONFIRM_LABEL_SERVER, mine);
   hex_of(out, PAIR_PKT, hex);
   struct sb sb = {0};
   sb_add(&sb, "%s\n%s\n", hex, mine);
   http_respond(r->c, 200, "OK", "text/plain", sb.p, sb.n);
   sb_free(&sb);
}

/* Step 4: the app proves it derived the SAME key, which is the only evidence
 * that it had the right code. Without this the pairing would "succeed" on a
 * wrong code and fail later as unexplained signature errors. */
static void h_pair_confirm(struct req *r, const char *sent)
{
   int64_t uid = cur.uid;
   char want[CONFIRM_HEX + 1];
   confirm_mac(cur.key, CONFIRM_LABEL_CLIENT, want);
   if (strlen(sent) != CONFIRM_HEX || !ct_eq(want, sent, CONFIRM_HEX)) {
      pair_reset();
      http_text(r->c, 403, "Forbidden", "wrong pairing code\n");
      return;
   }
   if (!db_durable_begin(r->db)) {
      pair_reset();
      oops(r);
      return;
   }
   sqlite3_stmt *st = db_prep(
       r->db, "INSERT INTO app(user_id,key,paired_at,last_seen) VALUES(?,?,?,?)"
              " ON CONFLICT(user_id) DO UPDATE SET key=excluded.key,"
              "   paired_at=excluded.paired_at, last_seen=excluded.last_seen");
   if (!st) {
      db_durable_rollback(r->db);
      pair_reset();
      oops(r);
      return;
   }
   int64_t now = (int64_t)time(NULL);
   sqlite3_bind_int64(st, 1, uid);
   sqlite3_bind_blob(st, 2, cur.key, 16, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, now);
   sqlite3_bind_int64(st, 4, now);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   if (!ok) {
      db_durable_rollback(r->db);
      pair_reset();
      oops(r);
      return;
   }
   /* The code has done its job; leaving it live would be a second key to the
    * same door. */
   sqlite3_stmt *dp = db_prep(r->db, "DELETE FROM pairing WHERE user_id=?");
   if (!dp) {
      db_durable_rollback(r->db);
      pair_reset();
      oops(r);
      return;
   }
   sqlite3_bind_int64(dp, 1, uid);
   ok = sqlite3_step(dp) == SQLITE_DONE;
   sqlite3_finalize(dp);
   /* The third charged attempt may already have burned the code before a
    * successful confirmation arrives, so deleting zero rows is legitimate;
    * what matters is that key install and "no code remains" commit together. */
   if (!ok || !db_durable_commit(r->db)) {
      db_durable_rollback(r->db);
      pair_reset();
      oops(r);
      return;
   }
   char msg[32];
   snprintf(msg, sizeof msg, "%" PRIwire "\n", uid);
   http_text(r->c, 200, "OK", msg);
   pair_reset();
}
