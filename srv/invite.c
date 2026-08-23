/* SPDX-License-Identifier: GPL-3.0
 * invite.c --- the invitation page and the login form
 * Copyright 2026 Jakob Kastelic
 */
#include "authrate.h"
#include "authsess.h"
#include "authuser.h"
#include "db.h"
#include "http.h"
#include "oops.h"
#include "page.h"
#include "util.h"
#include "web.h" /* web_method_not_allowed: this route serves two methods */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void h_login_form(struct req *r, const char *err)
{
   struct sb s = {0};
   sb_add(&s,
          "<h1>Pancra</h1>%s"
          "<form method=post action=\"/login\">"
          "<p><label>Email<br><input name=email type=email autofocus "
          "autocomplete=username></label></p>"
          "<p><label>Password<br><input name=password type=password "
          "autocomplete=current-password></label></p>"
          "<p><button>Sign in</button></p></form>"
          "<p><small>There is no sign-up here: an account is created by "
          "opening an invitation link. If you have one, open that link "
          "instead of this page.</small></p>",
          err ? "<p class=err>Wrong email or password. If you have not made "
                "an account yet, open your invitation link &mdash; this page "
                "cannot create one.</p>"
              : "");
   page(r, err ? 401 : 200, err ? "Unauthorized" : "OK", "sign in",
        s.p ? s.p : "");
   sb_free(&s);
}

void h_login_post(struct req *r)
{
   char raw[1024], pw[1024];
   /* BOTH FIELDS MUST BE FIELDS, AND THIS IS DECIDED BEFORE THE THROTTLE.
    *
    * Discarding the decoder's answer lets three shapes reach
    * login_throttled, user_by_email, user_check_password and login_failed as
    * though the client had sent them: a password longer than this buffer,
    * CLIPPED to 1023 bytes and then checked -- a different password
    * from the one typed, and a different one again if the buffer size ever
    * changes; an address carrying "%00", which is one string to the NOCASE
    * lookup here and another to anything counting bytes; and "email=a&email=b",
    * which had two answers and was silently given one.
    *
    * Refused here, the request touches neither the throttle table nor the
    * account table: nothing is read on the strength of it and, more to the
    * point, no login_fail row is written keyed on a string this form cannot
    * produce again. Answered as "bad" like any other unusable credential,
    * because which of the five it was is not something an unauthenticated
    * caller is owed. */
   enum form_field fe =
       form_field(r->body, r->body_len, "email", raw, sizeof raw);
   enum form_field fp =
       form_field(r->body, r->body_len, "password", pw, sizeof pw);
   if (fe != FORM_OK || fp != FORM_OK) {
      h_login_form(r, "bad");
      return;
   }
   /* ONE CANONICAL FORM, AND IT IS FIXED HERE, BEFORE ANYTHING IS LOOKED UP OR
    * WRITTEN. A bare `strlen(email) > 254`, with the raw field then used for
    * the throttle, the lookup and the failure record, gets the length half
    * right and cannot make the throttle key agree with the account table,
    * which compares under NOCASE. See util.h: an address spelled a dozen ways
    * is a dozen throttle
    * rows and one account, so the throttle never counts to its limit.
    *
    * `email` is what every line below uses. The raw field is not read again --
    * that is the point, and the reason the buffers have different names now. */
   char email[EMAIL_BUF];
   if (!email_canon(raw, email, sizeof email)) {
      h_login_form(r, "bad");
      return;
   }
   /* THREE ANSWERS. A counter that could not be READ is not a counter that
    * says zero: answering the second for the first is how the throttle --
    * the only thing between this form and an offline-speed password search
    * -- silently stops running under a busy or damaged database. Refuse. */
   int thr = login_throttled(r->db, email);
   if (thr == LOGIN_THROTTLED) {
      page(r, 429, "Too Many Requests", "slow down",
           "<p class=err>Too many attempts. Try again in a few minutes.</p>");
      return;
   }
   if (thr != LOGIN_OK) {
      /* 503, not 500: the counter is unreadable RIGHT NOW -- busy, locked,
       * a page that would not read -- and a client that cannot tell that
       * from a permanent fault has no reason to try again. */
      oops_busy(r);
      return;
   }
   int lookup_failed = 0;
   int64_t uid       = user_by_email(r->db, email, &lookup_failed);
   int pwok          = uid ? user_check_password(r->db, uid, pw) : 0;
   /* A STORAGE FAILURE IS NOT A WRONG PASSWORD. Telling the account holder it
    * was one sends them to reset a password that works -- and counts an
    * attempt against them, so enough of them lock the account out of a
    * database that is merely unreadable for a moment. */
   if (lookup_failed || pwok < 0) {
      oops(r);
      return;
   }
   if (!uid || !pwok) {
      /* AN UNCOUNTED FAILURE IS AN UNTHROTTLED GUESS. If the attempt could
       * not be recorded, the throttle that stands between this form and an
       * offline-speed password search is not running -- so refuse the
       * request rather than answer it for free. */
      if (!login_failed(r->db, email)) {
         oops(r);
         return;
      }
      h_login_form(r, "bad");
      return;
   }
   /* Not fatal if it fails: this sign-in is legitimate, and refusing it would
    * lock out the person who has just proved the password. The count ages out
    * of the window on its own. What must not go uncounted is a FAILURE, which
    * is the branch above. */
   (void)login_ok(r->db, email);
   char cookie[128], set[256];
   if (!session_new(r->db, uid, cookie, sizeof cookie)) {
      oops(r);
      return;
   }
   set_cookie_str(cookie, set, sizeof set);
   redirect(r, "/", set);
}

void h_invite(struct req *r, int64_t me, const char *cookie, const char *token)
{
   /* ---- "NO SUCH INVITATION" IS AN ANSWER, NOT A SHRUG -----
    *
    * This collapsed three different things into `found = 0`: a prepare that
    * failed, a step that answered BUSY or IOERR, and a genuine absence. The
    * first two are the STORE not answering, and the page then told the
    * visitor their link was "not valid any more" -- a statement about their
    * invitation, made on the strength of a database that could not be read.
    * An invitation is single-use and expires: a person told it is dead does
    * not try again, they ask for another one, and the one they were holding
    * may have been perfectly good.
    *
    * ABSENCE IS SQLITE_DONE AND NOTHING ELSE. Anything else is a 503 with a
    * Retry-After, which is the honest answer and the one that invites the
    * retry that will work. */
   sqlite3_stmt *st = db_prep(r->db, "SELECT owner_id,email FROM share_token"
                                     " WHERE token=? AND used_at IS NULL"
                                     " AND expires_at>?");
   if (!st) {
      oops_busy(r);
      return;
   }
   int64_t owner       = 0;
   char inv_email[256] = {0};
   int found           = 0;
   sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, (int64_t)time(NULL));
   int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      found = 1;
      /* NULL owner: a plain signup link, minted from the command line. It
       * creates an account and grants no access to anybody's data. */
      if (sqlite3_column_type(st, 0) != SQLITE_NULL)
         owner = (int64_t)sqlite3_column_int64(st, 0);
      const char *e = (const char *)sqlite3_column_text(st, 1);
      snprintf(inv_email, sizeof inv_email, "%s", e ? e : "");
   }
   int fin = sqlite3_finalize(st);
   /* THE FINALIZE'S STATUS COUNTS TOO: sqlite reports a deferred error here
    * (a statement that failed after stepping), and a row read out of one is a
    * row nobody should act on. */
   if (rc != SQLITE_ROW && !db_finished(rc)) {
      oops_busy(r);
      return;
   }
   if (fin != SQLITE_OK) {
      oops_busy(r);
      return;
   }
   if (!found) {
      /* A freshly-created account is already signed in when the signup POST
       * redirects home. If the person then opens the same invitation URL in
       * the app, the token is necessarily spent, but the session cookie is
       * still useful: send them to the public site so that URL opens their
       * account rather than ending at a dead invitation page. Keep the 404 for
       * signed-out visitors, since for them there is nowhere safe to send a
       * token that may be spent, expired, revoked, or simply invented. */
      if (me) {
         redirect(r, "https://pancra.org", NULL);
         return;
      }
      page(r, 404, "Not Found", "no such invitation",
           "<p>That link is not valid any more.</p>");
      return;
   }
   char oe[256] = {0}, oesc[300] = {0};
   if (owner) {
      /* THIS PAGE ASKS SOMEBODY TO ACCEPT ACCESS TO A NAMED PERSON'S DATA,
       * and the name is the only thing telling them whose. A database that
       * could not produce it must not render the invitation with the name
       * missing -- that is the one field a reader would use to decide. */
      if (email_of(r->db, owner, oe, sizeof oe) == DB_GET_FAIL) {
         oops_busy(r);
         return;
      }
      html_esc(oesc, sizeof oesc, oe);
   }

   /* GET RENDERS, POST REDEEMS -- and a single `if (GET) { ...render...;
    * return; }` falling straight into the redemption below is not that: HEAD,
    * PUT, DELETE and PATCH would all redeem.
    * That is the worst instance of the defect in the program: redemption
    * creates an ACCOUNT, issues a session cookie, inserts a share row against
    * somebody else's record and spends the token, it is reachable while signed
    * OUT, and the only secret involved is the token in the URL that the
    * invitee's browser -- and anything in front of it -- already has.
    *
    * web.c's WEB_RULES now refuses everything but GET and POST before this
    * handler runs. Checked again anyway, on the POST side rather than the GET
    * side, so a method that somehow arrives here renders nothing and redeems
    * nothing: the guarantee this function needs is "the method is POST", and
    * that is what it now asks. */
   if (strcmp(r->method, "GET") && strcmp(r->method, "POST")) {
      web_method_not_allowed(r, HTTP_M_GET | HTTP_M_POST);
      return;
   }

   if (!strcmp(r->method, "GET")) {
      char esc[300] = {0};
      html_esc(esc, sizeof esc, inv_email);
      struct sb s = {0};
      /* One heading for the whole invitation, because the page offers two
       * different things: signing in with an account you have, or making one.
       * Titling it after the second made the first half look like the wrong
       * page. */
      if (owner)
         sb_add(&s,
                "<h1>Pancra Invite</h1>"
                "<p>%s wants to share their data with you.</p>",
                oesc);
      else
         sb_add(&s, "<h1>Pancra Invite</h1>");
      if (me && !owner) {
         sb_add(&s, "<p>You are already signed in. "
                    "<a href=\"/\">Go to your data</a>.</p>");
         page(r, 200, "OK", "Pancra Invite", s.p ? s.p : "");
         sb_free(&s);
         return;
      }
      if (me) {
         char csrf[64];
         csrf_token(cookie, csrf, sizeof csrf);
         sb_add(&s,
                "<form method=post action=\"/invite/%s\">"
                "<input type=hidden name=csrf value=\"%s\">"
                "<input type=hidden name=action value=accept>"
                "<button>Accept</button></form>",
                token, csrf);
      } else {
         /* ONE form. Whether this address already has an account is something
          * the server can look up; asking the person to tell us first was
          * making them classify themselves before they could get in.
          *
          * Note what this costs: with no separate "new account" form there is
          * no confirm field, and there is no password reset on this server, so
          * a typo when creating an account is an account only a `sync passwd`
          * can recover. The session lasts a year, so the practical escape is
          * to change it from settings while still signed in. */
         char esc2[300] = {0};
         html_esc(esc2, sizeof esc2, inv_email);
         sb_add(&s,
                "<form method=post action=\"/invite/%s\">"
                "<input type=hidden name=action value=go>",
                token);
         /* Prefilled with the address the link was minted for, but EDITABLE:
          * the owner typed that address from memory, and an invitee whose
          * real one differs was otherwise stuck with no way forward. Locking
          * it bought nothing -- signing into an existing account still needs
          * that account's password. */
         sb_add(&s,
                "<p><label>Email<br><input name=email type=email value=\"%s\" "
                "required autocomplete=username></label></p>",
                esc2);
         sb_add(&s,
                "<p><label>Password<br>"
                "<input name=password type=password required "
                "autocomplete=current-password></label></p>"
                "<button>Continue</button></form>"
                "<p><small>If you already have an account, this signs you in. "
                "If you do not, it makes one.</small></p>");
      }
      page(r, 200, "OK", "Pancra Invite", s.p ? s.p : "");
      sb_free(&s);
      return;
   }

   /* POST: accept as the signed-in user, or after signing in, or as a new
    * account. A share link IS the invitation -- this is the only way an
    * account comes into existence. */
   char action[32];
   /* An action that is not one field of well-formed text is not an action.
    * FORM_ABSENT leaves "" and falls through to the "Unknown action" refusal
    * at the bottom, which is the same answer -- named here rather than left
    * implicit, and refused BEFORE db_durable_begin either branch would run. */
   switch (form_field(r->body, r->body_len, "action", action, sizeof action)) {
      case FORM_OK:
      case FORM_ABSENT: break;
      case FORM_MALFORMED:
      case FORM_TOO_LONG:
      case FORM_DUPLICATE:
         page(r, 400, "Bad Request", "invitation", "<p>Unknown action.</p>");
         return;
   }
   int64_t viewer      = me;
   char newcookie[128] = {0};
   int tx              = 0;
   if (!strcmp(action, "accept")) {
      /* csrf_guard renders its own refusal; the other two branches did not,
       * so they returned having written NOTHING and the browser got a blank
       * page. Reachable by signing out in a second tab and then pressing
       * Accept -- an ordinary thing to do, answered with an empty window. */
      if (!me) {
         page(r, 403, "Forbidden", "Pancra Invite",
              "<p>You are not signed in any more. "
              "<a href=\"/login\">Sign in</a> and open the link again.</p>");
         return;
      }
      if (!owner) {
         page(r, 404, "Not Found", "Pancra Invite",
              "<p>That invitation is no longer valid. Ask for a new one.</p>"
              "<p><a href=\"/login\">Sign in</a></p>");
         return;
      }
      if (!csrf_guard(r, cookie))
         return;
      if (!db_durable_begin(r->db)) {
         oops(r);
         return;
      }
      tx = 1;
   } else if (!strcmp(action, "go")) {
      /* Sign in if the account exists, create it if it does not. */
      char raw[1024], pw[1024];
      /* THE SAME TYPED REFUSAL AS /login, AND FOR A SHARPER REASON: this is
       * the only path in the program that CREATES an account. A clipped
       * password would be stored as the clipped one and could never be typed
       * again, and a "%00" address would create a row the login form can
       * never match. Both are refused before the throttle, before the account
       * lookup and before db_durable_begin. */
      enum form_field ie =
          form_field(r->body, r->body_len, "email", raw, sizeof raw);
      enum form_field ip =
          form_field(r->body, r->body_len, "password", pw, sizeof pw);
      if (ie != FORM_OK || ip != FORM_OK) {
         page(r, 400, "Bad Request", "Pancra Invite",
              "<p class=err>Need a real email address and a password.</p>");
         return;
      }
      /* THE SAME BOUND AND THE SAME CANONICAL FORM AS /login, AND BEFORE THE
       * THROTTLE LOOKUP -- which is exactly what this path did not have.
       *
       * It took the field as it arrived, up to a kilobyte of it, and handed it
       * straight to login_throttled, user_by_email, login_failed and, for an
       * address with no account, user_create. This is the ONLY path in the
       * program that creates an account, so it is the one where the missing
       * bound cost the most: an invitee could be given an account under a
       * 900-byte address that /login refuses to look up at all, and there is
       * no password reset here to recover it with. The same request also wrote
       * a login_fail row keyed on that address -- a row no later attempt can
       * ever match, because no later attempt can get an address that long past
       * /login.
       *
       * Refused BEFORE the throttle, so nothing is read and nothing is written
       * on the strength of a string this server will not act on. */
      char email[EMAIL_BUF];
      if (!email_canon(raw, email, sizeof email)) {
         page(r, 400, "Bad Request", "Pancra Invite",
              "<p class=err>Need a real email address and a password.</p>");
         return;
      }
      /* Same three answers as the login form above, and the same reason. */
      int ithr = login_throttled(r->db, email);
      if (ithr != LOGIN_OK && ithr != LOGIN_THROTTLED) {
         oops_busy(r); /* transient, as above */
         return;
      }
      if (ithr == LOGIN_THROTTLED) {
         page(r, 429, "Too Many Requests", "Pancra Invite",
              "<p class=err>Too many attempts. Try again in a few "
              "minutes.</p>");
         return;
      }
      int vfailed = 0;
      viewer      = user_by_email(r->db, email, &vfailed);
      if (vfailed) {
         oops(r);
         return;
      }
      if (viewer) {
         int vpw = user_check_password(r->db, viewer, pw);
         if (vpw < 0) {
            oops(r);
            return;
         }
         if (!vpw) {
            /* Same rule as the login form: an attempt that could not be
             * counted is an unthrottled guess against an existing account. */
            if (!login_failed(r->db, email)) {
               oops(r);
               return;
            }
            page(r, 401, "Unauthorized", "Pancra Invite",
                 "<p class=err>That email already has an account, and that is "
                 "not its password.</p>");
            return;
         }
         /* Not fatal if it fails: the sign-in is legitimate and refusing it
          * would lock out the very person who proved the password. The count
          * ages out of the window on its own; what must not happen is a
          * FAILURE going uncounted, which is the branch above. */
         (void)login_ok(r->db, email);
      }
      if (!db_durable_begin(r->db)) {
         oops(r);
         return;
      }
      tx = 1;
      if (!viewer) {
         /* A SERVER FAILURE IS NOT A BAD ADDRESS. This answered
          * 400 "Need a real email address and a password" for a database that
          * could not be written -- so somebody accepting an invitation was
          * told to correct an address that was perfectly good, and would go
          * on correcting it. */
         enum user_create_result made = user_create(r->db, email, pw, &viewer);
         if (made == USER_CREATE_FAIL) {
            db_durable_rollback(r->db);
            oops(r);
            return;
         }
         if (made != USER_CREATE_OK) {
            db_durable_rollback(r->db);
            page(r, 400, "Bad Request", "Pancra Invite",
                 made == USER_CREATE_EXISTS
                     ? "<p class=err>That address already has an account. "
                       "Sign in instead.</p>"
                     : "<p class=err>Need a real email address and a "
                       "password.</p>");
            return;
         }
      }
      if (!session_new(r->db, viewer, newcookie, sizeof newcookie)) {
         db_durable_rollback(r->db);
         oops(r);
         return;
      }
   } else {
      page(r, 400, "Bad Request", "invitation", "<p>Unknown action.</p>");
      return;
   }
   if (owner && viewer == owner) {
      if (tx)
         db_durable_rollback(r->db);
      page(r, 400, "Bad Request", "invitation",
           "<p>That is your own link.</p>");
      return;
   }

   if (owner) {
      /* The follower cap is enforced HERE, at the moment it would be
       * exceeded: links can be minted freely, and what is limited is how many
       * people end up actually following. */
      /* count(*) always yields a row, so DB_GET_NONE here would itself be a
       * fault; either non-VALUE outcome means the cap could not be checked,
       * and a cap that cannot be checked is not a cap. */
      int64_t nf = 0;
      if (db_get_long(r->db, "SELECT count(*) FROM share WHERE owner_id=?",
                      owner, &nf) != DB_GET_VALUE) {
         db_durable_rollback(r->db);
         oops(r);
         return;
      }
      if (nf >= MAX_FOLLOWERS) {
         db_durable_rollback(r->db);
         page(r, 409, "Conflict", "invitation",
              "<p>That person already has as many followers as allowed.</p>");
         return;
      }
      sqlite3_stmt *ins = db_prep(r->db, "INSERT OR IGNORE INTO"
                                         " share(owner_id,viewer_id,created_at)"
                                         " VALUES(?,?,?)");
      if (!ins) {
         db_durable_rollback(r->db);
         oops(r);
         return;
      }
      sqlite3_bind_int64(ins, 1, owner);
      sqlite3_bind_int64(ins, 2, viewer);
      sqlite3_bind_int64(ins, 3, (int64_t)time(NULL));
      int irc = sqlite3_step(ins);
      sqlite3_finalize(ins);
      if (irc != SQLITE_DONE) {
         db_durable_rollback(r->db);
         oops(r);
         return;
      }
   }

   sqlite3_stmt *up =
       db_prep(r->db, "UPDATE share_token SET used_at=? WHERE token=?"
                      " AND used_at IS NULL AND expires_at>?");
   if (!up) {
      db_durable_rollback(r->db);
      oops(r);
      return;
   }
   int64_t now = (int64_t)time(NULL);
   sqlite3_bind_int64(up, 1, now);
   sqlite3_bind_text(up, 2, token, -1, SQLITE_STATIC);
   sqlite3_bind_int64(up, 3, now);
   int urc = sqlite3_step(up);
   sqlite3_finalize(up);
   /* THE SAME DISTINCTION AS THE LOOKUP, and it matters more here
    * because this is the spend. A step that did not FINISH says nothing about
    * the token: telling the visitor their invitation is "not valid any more"
    * on the strength of a busy database retires a link that is still good,
    * and it is single-use, so they cannot check. DONE with no row changed is
    * the real conflict -- somebody redeemed it first, or it expired between
    * the page and the button. */
   if (!db_finished(urc)) {
      db_durable_rollback(r->db);
      oops_busy(r);
      return;
   }
   if (db_changes(r->db) != 1) {
      db_durable_rollback(r->db);
      page(r, 409, "Conflict", "invitation",
           "<p>That link is not valid any more.</p>");
      return;
   }
   if (!db_durable_commit(r->db)) {
      db_durable_rollback(r->db);
      oops(r);
      return;
   }
   char set[256];
   if (newcookie[0]) {
      set_cookie_str(newcookie, set, sizeof set);
      redirect(r, "/", set);
   } else {
      redirect(r, "/", NULL);
   }
}
