/* SPDX-License-Identifier: GPL-3.0
 * invite.c --- the invitation page and the login form
 * Copyright 2026 Jakob Kastelic
 */
#include "auth.h"
#include "db.h"
#include "http.h"
#include "page.h"
#include "util.h"
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
   char email[1024], pw[1024];
   form_get(r->body, r->body_len, "email", email, sizeof email);
   form_get(r->body, r->body_len, "password", pw, sizeof pw);
   /* An address longer than RFC 5321 allows is not a login attempt, and it is
    * the shape a stranger uses to write kilobytes into login_fail on every
    * request. Refuse it before it can be stored or counted. */
   if (strlen(email) > 254) {
      h_login_form(r, "bad");
      return;
   }
   if (login_throttled(email)) {
      page(r, 429, "Too Many Requests", "slow down",
           "<p class=err>Too many attempts. Try again in a few minutes.</p>");
      return;
   }
   long uid = user_by_email(email);
   if (!uid || !user_check_password(uid, pw)) {
      login_failed(email);
      h_login_form(r, "bad");
      return;
   }
   login_ok(email);
   char cookie[128], set[256];
   if (!session_new(uid, cookie, sizeof cookie)) {
      oops(r);
      return;
   }
   set_cookie_str(cookie, set, sizeof set);
   redirect(r, "/", set);
}

void h_invite(struct req *r, long me, const char *cookie, const char *token)
{
   sqlite3_stmt *st    = db_prep("SELECT owner_id,email FROM share_token"
                                 " WHERE token=? AND used_at IS NULL"
                                 " AND expires_at>?");
   long owner          = 0;
   char inv_email[256] = {0};
   int found           = 0;
   if (st) {
      sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 2, (long)time(NULL));
      if (sqlite3_step(st) == SQLITE_ROW) {
         found = 1;
         /* NULL owner: a plain signup link, minted from the command line. It
          * creates an account and grants no access to anybody's data. */
         if (sqlite3_column_type(st, 0) != SQLITE_NULL)
            owner = (long)sqlite3_column_int64(st, 0);
         const char *e = (const char *)sqlite3_column_text(st, 1);
         snprintf(inv_email, sizeof inv_email, "%s", e ? e : "");
      }
      sqlite3_finalize(st);
   }
   if (!found) {
      page(r, 404, "Not Found", "no such invitation",
           "<p>That link is not valid any more.</p>");
      return;
   }
   char oe[256] = {0}, oesc[300] = {0};
   if (owner) {
      email_of(owner, oe, sizeof oe);
      html_esc(oesc, sizeof oesc, oe);
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
   form_get(r->body, r->body_len, "action", action, sizeof action);
   long viewer         = me;
   char newcookie[128] = {0};
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
   } else if (!strcmp(action, "go")) {
      /* Sign in if the account exists, create it if it does not. */
      char email[1024], pw[1024];
      form_get(r->body, r->body_len, "email", email, sizeof email);
      form_get(r->body, r->body_len, "password", pw, sizeof pw);
      if (login_throttled(email)) {
         page(r, 429, "Too Many Requests", "Pancra Invite",
              "<p class=err>Too many attempts. Try again in a few "
              "minutes.</p>");
         return;
      }
      viewer = user_by_email(email);
      if (viewer) {
         if (!user_check_password(viewer, pw)) {
            login_failed(email);
            page(r, 401, "Unauthorized", "Pancra Invite",
                 "<p class=err>That email already has an account, and that is "
                 "not its password.</p>");
            return;
         }
         login_ok(email);
      } else if (!user_create(email, pw, &viewer)) {
         page(r, 400, "Bad Request", "Pancra Invite",
              "<p class=err>Need a real email address and a password.</p>");
         return;
      }
      if (!session_new(viewer, newcookie, sizeof newcookie)) {
         oops(r);
         return;
      }
   } else {
      page(r, 400, "Bad Request", "invitation", "<p>Unknown action.</p>");
      return;
   }
   if (owner && viewer == owner) {
      page(r, 400, "Bad Request", "invitation",
           "<p>That is your own link.</p>");
      return;
   }

   if (owner) {
      /* The follower cap is enforced HERE, at the moment it would be
       * exceeded: links can be minted freely, and what is limited is how many
       * people end up actually following. */
      int seen = 0;
      long nf  = db_one_long("SELECT count(*) FROM share WHERE owner_id=?",
                             owner, &seen);
      if (nf >= MAX_FOLLOWERS) {
         page(r, 409, "Conflict", "invitation",
              "<p>That person already has as many followers as allowed.</p>");
         return;
      }
      sqlite3_stmt *ins = db_prep("INSERT OR IGNORE INTO"
                                  " share(owner_id,viewer_id,created_at)"
                                  " VALUES(?,?,?)");
      if (!ins) {
         oops(r);
         return;
      }
      sqlite3_bind_int64(ins, 1, owner);
      sqlite3_bind_int64(ins, 2, viewer);
      sqlite3_bind_int64(ins, 3, (long)time(NULL));
      sqlite3_step(ins);
      sqlite3_finalize(ins);
   }

   sqlite3_stmt *up = db_prep("UPDATE share_token SET used_at=? WHERE token=?");
   if (up) {
      sqlite3_bind_int64(up, 1, (long)time(NULL));
      sqlite3_bind_text(up, 2, token, -1, SQLITE_STATIC);
      sqlite3_step(up);
      sqlite3_finalize(up);
   }
   char set[256];
   if (newcookie[0]) {
      set_cookie_str(newcookie, set, sizeof set);
      redirect(r, "/", set);
   } else {
      redirect(r, "/", NULL);
   }
}
