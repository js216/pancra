/* SPDX-License-Identifier: GPL-3.0
 * authshare.c --- the invitation link: minted, capped, and swept
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
#include "authshare.h"
#include "db.h"
#include "proto.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <time.h>

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
 * WHY IT IS NOT A BARE INSERT ON POST /settings/share. Each press of "Create
 * share link" adds a row with a fourteen-day life, and nothing else ever
 * removes one. Not expiry -- an expired token is filtered out of every query
 * by `expires_at>?` and then left in the table for ever. Not redemption --
 * h_invite_post sets `used_at` and the row stays. Only an explicit Revoke
 * click, or the account being deleted, takes anything out.
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
enum share_mint share_token_mint(struct db *d, int64_t owner, const char *email,
                                 char *token, size_t cap)
{
   char tok[TOKEN_HEX + 1];
   if (!rnd_hex(tok, sizeof tok, TOKEN_HEX))
      return SHARE_MINT_FAILED;
   int64_t now = (int64_t)time(NULL);

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
      int64_t n = 0;
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
