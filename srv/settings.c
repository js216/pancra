/* SPDX-License-Identifier: GPL-3.0
 * settings.c --- the settings page
 * Copyright 2026 Jakob Kastelic
 */
#include "auth.h"
#include "db.h"
#include "http.h"
#include "page.h"
#include "pair.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Every offset in real-world use, in minutes east of UTC. Offsets rather than
 * named zones because the board has no tz database to resolve a name with --
 * and the app already records the offset it was using with every reading, so
 * "follow the data" is the setting most people never have to change. */
static const int TZ_MIN[] = {-720, -660, -600, -570, -540, -480, -420, -360,
                             -300, -240, -210, -180, -150, -120, -60,  0,
                             60,   120,  180,  210,  240,  270,  300,  330,
                             345,  360,  390,  420,  480,  525,  540,  570,
                             600,  630,  660,  720,  765,  780,  840};

void h_settings(struct req *r, long me, const char *cookie, const char *note)
{
   char email[256], esc[300] = {0}, csrf[64];
   email_of(me, email, sizeof email);
   html_esc(esc, sizeof esc, email);
   csrf_token(cookie, csrf, sizeof csrf);

   struct sb s = {0};
   nav(&s, email, cookie);
   sb_add(&s, "<a href=\"/\">&lt;- Main</a>\n<h2>Settings</h2>\n");
   if (note)
      sb_add(&s, "<p><strong>%s</strong></p>", note);
   sb_add(&s, "<p>Signed in as %s</p>", esc);

   /* The paired app. */
   sb_add(&s, "<h2>App</h2>");
   if (pair_is_paired(me)) {
      sb_add(&s,
             "<p>An app is paired. To pair a different one, unpair first.</p>"
             "<form method=post action=\"/settings/unpair\">"
             "<input type=hidden name=csrf value=\"%s\">"
             "<button>Unpair</button></form>",
             csrf);
   } else {
      sqlite3_stmt *st =
          db_prep("SELECT code,expires_at FROM pairing WHERE user_id=?");
      char code[16] = {0};
      long exp      = 0;
      if (st) {
         sqlite3_bind_int64(st, 1, me);
         if (sqlite3_step(st) == SQLITE_ROW) {
            const char *c = (const char *)sqlite3_column_text(st, 0);
            snprintf(code, sizeof code, "%s", c ? c : "");
            exp = (long)sqlite3_column_int64(st, 1);
         }
         sqlite3_finalize(st);
      }
      if (code[0] && exp > (long)time(NULL))
         sb_add(&s,
                "<p class=code>%s</p><p>Type this into the app within "
                "%ld minutes.</p>",
                code, (exp - (long)time(NULL) + 59) / 60);
      sb_add(&s,
             "<form method=post action=\"/settings/pair\">"
             "<input type=hidden name=csrf value=\"%s\">"
             "<button>%s pairing code</button></form>",
             csrf, code[0] ? "New" : "Show");
   }

   /* Followers. */
   sb_add(&s, "<h2>Followers</h2>");
   sqlite3_stmt *fl = db_prep("SELECT u.id,u.email FROM share s"
                              " JOIN user u ON u.id=s.viewer_id"
                              " WHERE s.owner_id=? ORDER BY u.email");
   int nf           = 0;
   if (fl) {
      sqlite3_bind_int64(fl, 1, me);
      sb_add(&s, "<table>");
      while (sqlite3_step(fl) == SQLITE_ROW) {
         char fe[300] = {0};
         html_esc(fe, sizeof fe, (const char *)sqlite3_column_text(fl, 1));
         sb_add(&s,
                "<tr><td>%s</td><td>"
                "<form method=post action=\"/settings/revoke\">"
                "<input type=hidden name=csrf value=\"%s\">"
                "<input type=hidden name=who value=\"%lld\">"
                "<button>Revoke</button></form></td></tr>",
                fe, csrf, (long long)sqlite3_column_int64(fl, 0));
         nf++;
      }
      sb_add(&s, "</table>");
      sqlite3_finalize(fl);
   }
   if (!nf)
      sb_add(&s, "<p>Nobody follows you yet.</p>");
   if (nf < MAX_FOLLOWERS)
      sb_add(&s,
             "<form method=post action=\"/settings/share\">"
             "<input type=hidden name=csrf value=\"%s\">"
             "<p><label>Their email<br>"
             "<input name=email type=email></label></p>"
             "<button>Create share link</button></form>",
             csrf);
   else
      sb_add(&s, "<p>At the limit of %d followers.</p>", MAX_FOLLOWERS);

   /* Live share links, shown as the invited person will receive them -- built
    * from the name THIS request arrived on, because the server has no other
    * way to know what it is called from outside. A half-link the owner has to
    * assemble by hand is a link that gets sent wrong. */
   char host[200];
   if (!hdr_get(r->hdr, "Host", host, sizeof host) || !host[0])
      snprintf(host, sizeof host, "%s", "pancra.org");
   char hesc[240] = {0};
   html_esc(hesc, sizeof hesc, host);

   sqlite3_stmt *tk = db_prep("SELECT token,email,expires_at FROM share_token"
                              " WHERE owner_id=? AND used_at IS NULL"
                              " AND expires_at>? ORDER BY created_at DESC");
   if (tk) {
      sqlite3_bind_int64(tk, 1, me);
      sqlite3_bind_int64(tk, 2, (long)time(NULL));
      int any = 0;
      while (sqlite3_step(tk) == SQLITE_ROW) {
         if (!any)
            sb_add(&s, "<h3>Unused links</h3><table>");
         any             = 1;
         char te[300]    = {0};
         const char *em  = (const char *)sqlite3_column_text(tk, 1);
         const char *tok = (const char *)sqlite3_column_text(tk, 0);
         html_esc(te, sizeof te, em && *em ? em : "anyone");
         sb_add(&s,
                "<tr><td><code>https://%s/invite/%s</code><br>"
                "<small>for %s</small></td><td>"
                "<form method=post action=\"/settings/revoke-link\">"
                "<input type=hidden name=csrf value=\"%s\">"
                "<input type=hidden name=token value=\"%s\">"
                "<button>Revoke</button></form></td></tr>",
                hesc, tok, te, csrf, tok);
      }
      if (any)
         sb_add(&s, "</table>");
      sqlite3_finalize(tk);
   }

   /* Time zone and password. */
   /* The STORED setting, not the resolved one: the box showed the offset the
    * app happened to be reporting, so "follow the phone" looked like a
    * deliberate choice of UTC. */
   int tz_found = 0;
   long tz_set  = db_one_long(
       "SELECT tz_offset FROM user WHERE id=? AND tz_offset IS NOT NULL", me,
       &tz_found);
   int tz_known = 0;
   int tz_auto  = tz_resolve(me, &tz_known);
   sb_add(&s,
          "<h2>Time zone</h2>"
          "<form method=post action=\"/settings/tz\">"
          "<input type=hidden name=csrf value=\"%s\">"
          "<p><select name=tz>",
          csrf);
   if (tz_known || tz_found)
      sb_add(&s,
             "<option value=\"\"%s>Follow the data (currently %+d:%02d)"
             "</option>",
             tz_found ? "" : " selected", tz_auto / 60, abs(tz_auto % 60));
   else
      /* Nothing to follow yet. Saying "+0:00" here would report a fallback as
       * though it were a reading. */
      sb_add(&s,
             "<option value=\"\"%s>Follow the data (nothing to follow "
             "yet)</option>",
             tz_found ? "" : " selected");
   for (int i = 0; i < (int)(sizeof TZ_MIN / sizeof TZ_MIN[0]); i++) {
      int v = TZ_MIN[i];
      sb_add(&s, "<option value=\"%d\"%s>UTC%+03d:%02d</option>", v,
             (tz_found && v == (int)tz_set) ? " selected" : "", v / 60,
             abs(v % 60));
   }
   sb_add(&s, "</select></p><button>Save</button></form>");

   sb_add(&s,
          "<h2>Password</h2>"
          "<form method=post action=\"/settings/password\">"
          "<input type=hidden name=csrf value=\"%s\">"
          "<p><label>Current<br><input name=old type=password "
          "autocomplete=current-password></label></p>"
          "<p><label>New<br><input name=new "
          "type=password autocomplete=new-password></label></p>"
          "<button>Change password</button></form>"

          "<form method=post action=\"/settings/signout-all\">"
          "<input type=hidden name=csrf value=\"%s\">"
          "<button>Sign out everywhere</button></form>"

          /* Deleting is typed, not clicked. Everything goes -- the record,
           * the pairing, every share -- and there is no undo, so the
           * confirmation asks for something only the account holder can
           * supply rather than a button they can hit by accident. */
          "<h2>Delete account</h2>"
          "<form method=post action=\"/settings/delete\">"
          "<input type=hidden name=csrf value=\"%s\">"
          "<p>This removes your whole record, your paired app and every "
          "share. It cannot be undone.</p>"
          "<p><label>Type your email to confirm<br>"
          "<input name=confirm type=email></label></p>"
          "<button>Delete my account</button></form>",
          csrf, csrf, csrf);

   page(r, 200, "OK", "settings", s.p ? s.p : "");
   sb_free(&s);
}

/* The state-changing half of the page. Each action answers by re-rendering
 * the settings page with a note saying what happened, so there is one place
 * that draws it and one thing a form can do: post here and come back.
 *
 * CSRF is checked by the router before we are called -- it is the same check
 * for every action, and one that is easy to forget per-action. */
void h_settings_post(struct req *r, long me, const char *cookie,
                     const char *what)
{
   if (!strcmp(what, "pair")) {
      char code[16];
      if (pair_is_paired(me))
         h_settings(r, me, cookie, "Unpair the current app first.");
      else if (pair_code_new(me, code, sizeof code))
         h_settings(r, me, cookie, "Type the code into the app.");
      else
         oops(r);
      return;
   }
   if (!strcmp(what, "unpair")) {
      /* UNDER THE PAIRING LOCK. pair.c states the invariant -- one exchange at
       * a time, spanning four requests that may land on different workers, so
       * the whole round is taken under this lock -- and route_api honours it.
       * This handler did not, and pair_unpair reads cur.uid and then calls
       * pair_reset(), which jpake_free()s the in-flight object: a use-after-
       * free on a crypto context while another worker is mid-round. Clicking
       * Unpair on the website while a phone was pairing was enough. */
      pair_lock();
      pair_unpair(me);
      pair_unlock();
      h_settings(r, me, cookie, "The app is unpaired.");
      return;
   }
   if (!strcmp(what, "tz")) {
      char tz[16];
      form_get(r->body, r->body_len, "tz", tz, sizeof tz);
      sqlite3_stmt *st = db_prep("UPDATE user SET tz_offset=? WHERE id=?");
      int ok           = 0; /* did the statement actually run? */
      if (st) {
         if (tz[0])
            sqlite3_bind_int(st, 1, (int)strtol(tz, NULL, 10));
         else
            sqlite3_bind_null(st, 1);
         sqlite3_bind_int64(st, 2, me);
         ok = sqlite3_step(st) == SQLITE_DONE;
         sqlite3_finalize(st);
      }
      h_settings(r, me, cookie,
                 ok ? "Time zone saved." : "Could not save the time zone.");
      return;
   }
   if (!strcmp(what, "password")) {
      char old[1024], new[1024];
      form_get(r->body, r->body_len, "old", old, sizeof old);
      form_get(r->body, r->body_len, "new", new, sizeof new);
      if (!user_check_password(me, old))
         h_settings(r, me, cookie, "That is not the current password.");
      else if (!user_set_password(me, new))
         h_settings(r, me, cookie, "A password cannot be empty.");
      else
         h_settings(r, me, cookie, "Password changed.");
      return;
   }
   if (!strcmp(what, "delete")) {
      char typed[256], mine[256];
      form_get(r->body, r->body_len, "confirm", typed, sizeof typed);
      email_of(me, mine, sizeof mine);
      /* BOTH must be non-empty, not merely equal.
       *
       * email_of writes "" and returns on any failure -- no row, a prepare
       * that failed, a NULL column -- and form_get leaves "" for a field that
       * was not sent. So "" == "" compared equal and deleted the account,
       * every table cascading with it, on a request that proved nothing. The
       * typed-confirmation is the strongest guard in the product; it must not
       * be satisfiable by two absences. */
      if (!mine[0] || !typed[0] || strcasecmp(typed, mine)) {
         h_settings(r, me, cookie,
                    "That is not your email; nothing was deleted.");
         return;
      }
      /* One statement: every other table references user(id) with ON
       * DELETE CASCADE, so the rows, the pairing, the sessions and the
       * shares in BOTH directions go with it. */
      sqlite3_stmt *st = db_prep("DELETE FROM user WHERE id=?");
      if (!st) {
         oops(r);
         return;
      }
      sqlite3_bind_int64(st, 1, me);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
      if (!ok) {
         oops(r);
         return;
      }
      redirect(r, "/login",
               "sid=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax");
      return;
   }
   if (!strcmp(what, "signout-all")) {
      session_drop_all(me);
      redirect(r, "/login", NULL);
      return;
   }
   if (!strcmp(what, "share")) {
      char email[256], token[TOKEN_HEX + 1];
      form_get(r->body, r->body_len, "email", email, sizeof email);
      rnd_hex(token, TOKEN_HEX);
      sqlite3_stmt *st =
          db_prep("INSERT INTO share_token(token,owner_id,email,created_at,"
                  " expires_at) VALUES(?,?,?,?,?)");
      if (!st) {
         oops(r);
         return;
      }
      long now = (long)time(NULL);
      sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 2, me);
      sqlite3_bind_text(st, 3, email, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 4, now);
      sqlite3_bind_int64(st, 5, now + TOKEN_TTL);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
      if (!ok) {
         h_settings(r, me, cookie, "Could not create the share link.");
         return;
      }
      h_settings(r, me, cookie, "Share link created; copy it below.");
      return;
   }
   if (!strcmp(what, "revoke-link")) {
      char tok[TOKEN_HEX + 1];
      form_get(r->body, r->body_len, "token", tok, sizeof tok);
      sqlite3_stmt *st =
          db_prep("DELETE FROM share_token WHERE token=? AND owner_id=?");
      int ok = 0; /* did the statement actually run? */
      if (st) {
         sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
         sqlite3_bind_int64(st, 2, me);
         /* db_changes, not SQLITE_DONE: a DELETE that matched nothing is also
          * DONE, so a mistyped or already-revoked token reported success. */
         ok = sqlite3_step(st) == SQLITE_DONE && db_changes() > 0;
         sqlite3_finalize(st);
      }
      h_settings(r, me, cookie,
                 ok ? "Link revoked."
                    : "That link was not found; nothing "
                      "was revoked.");
      return;
   }
   if (!strcmp(what, "revoke")) {
      char who[32];
      form_get(r->body, r->body_len, "who", who, sizeof who);
      sqlite3_stmt *st =
          db_prep("DELETE FROM share WHERE owner_id=? AND viewer_id=?");
      int ok = 0; /* did the statement actually run? */
      if (st) {
         sqlite3_bind_int64(st, 1, me);
         sqlite3_bind_int64(st, 2, strtol(who, NULL, 10));
         /* And it must have matched a row. SQLITE_DONE alone is true of a
          * DELETE that found nothing, so this reported a revocation that had
          * not happened -- see the note below. */
         ok = sqlite3_step(st) == SQLITE_DONE && db_changes() > 0;
         sqlite3_finalize(st);
      }
      /* "Access revoked." was printed whether or not the DELETE ran. Telling
       * someone their medical record is no longer shared, when it still is,
       * is the worst answer this page can give. */
      h_settings(r, me, cookie,
                 ok ? "Access revoked."
                    : "That follower was not found; nothing was revoked.");
      return;
   }
   /* An action nobody defined is a bad URL, not a settings page: this used
    * to fall through to the router's 404 and still means the same thing. */
   page(r, 404, "Not Found", "not found",
        "<p>No such page. <a href=\"/\">Home</a></p>");
}
