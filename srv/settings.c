/* SPDX-License-Identifier: GPL-3.0
 * settings.c --- the settings page
 * Copyright 2026 Jakob Kastelic
 */
#include "auth.h"
#include "db.h"
#include "http.h"
#include "oops.h"
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

/* ---- IS THIS ONE OF THE OFFSETS WE OFFER? -----------------------------
 *
 * The handler used to do `strtol(tz, NULL, 10)` and store the result: no end
 * pointer, no range test, no membership test, then narrowed to int and reported
 * as "Time zone saved." So every one of these was accepted and persisted:
 *
 *   "5abc"      -> 5      (strtol stops at the letter and does not say so)
 *   "  -60"     -> -60    (leading space)
 *   "+60"       -> 60
 *   "0060"      -> 60
 *   ""          -> handled separately (the "follow the data" choice)
 *   "99999"     -> 99999  minutes, an offset no place on earth has
 *   "9999999999" -> whatever that narrows to, which on this build is
 *                  implementation-defined and certainly not a time zone
 *
 * The form only ever POSTs the exact `%d` of a TZ_MIN entry, so anything else
 * arrived from something that was not this form -- and the value goes on to
 * shift every timestamp the owner sees. A stored 99999 is not a cosmetic
 * error: it silently relabels the hour of every reading in the history.
 *
 * So the rule is exact and canonical, not "parses as a number": an optional
 * '-', then digits with no leading zero, nothing before or after, and the
 * value must be one the table actually offers. Everything else is refused with
 * the database untouched -- a rejected offset must not be reported as saved,
 * because the owner would then trust a label that never changed. */
static int tz_canonical(const char *s, int *out)
{
   if (!s || !*s)
      return 0;
   const char *p = s;
   int neg       = 0;
   if (*p == '-') {
      neg = 1;
      p++;
   }
   if (*p < '0' || *p > '9')
      return 0; /* a sign with nothing after it, or no digits at all */
   /* NO LEADING ZEROS, so one offset has exactly one spelling. "0060" and "60"
    * naming the same zone is two keys for one value, and the form emits only
    * the second.
    *
    * AND NO NEGATIVE ZERO, which is the same rule and was the hole in it.
    * UTC *is* on the list -- 0 is a member of TZ_MIN and the form emits
    * `value="0"` for it -- so "0" has to be ACCEPTED, and the leading-zero test
    * alone let "-0" through with it: '-' consumed, a single '0' left, nothing
    * suspicious about either half, and -0 == 0 in integer arithmetic, so the
    * membership scan found UTC and the value was stored. That is a second
    * spelling of a listed offset, which is exactly what this function exists to
    * refuse; it is also the shape a probe uses to find out whether the check is
    * a canonical-form check or a numeric one. So: a bare "0" is the one and
    * only spelling of UTC, and a '0' that is either signed or followed by
    * anything is decoration. */
   if (*p == '0' && (p[1] || neg))
      return 0;
   long v = 0;
   int nd = 0;
   for (; *p; p++) {
      if (*p < '0' || *p > '9')
         return 0; /* trailing text: "5abc" is not 5 */
      /* FOUR DIGITS covers every offset the table holds (840 and -720 are three
       * apiece), so a longer run of digits cannot be one -- and stopping here
       * also keeps `v` from overflowing, which is undefined behaviour rather
       * than a large number.
       *
       * NOT REACHABLE THROUGH THE ONLY CALLER TODAY, and deliberately kept
       * anyway. The handler reads the field into `char tz[16]`, and
       * form_field REFUSES anything that would not fit rather than truncating
       * it (it used to truncate; see util.h), so at most fifteen digits ever
       * arrive here and fifteen cannot overflow a long -- the membership scan
       * would refuse them all on its own. That means no end-to-end case can
       * distinguish this line from its absence (srv/test/synctest.sh cannot
       * kill it, and says so). It stays because it is what makes the FUNCTION
       * safe rather than the buffer: widen tz[] to 24 bytes for any reason, and
       * without this line the accumulator overflows on input a stranger
       * chooses. */
      if (++nd > 4)
         return 0;
      v = (v * 10) + (*p - '0');
   }
   if (neg)
      v = -v;
   /* MEMBERSHIP, not range. A range test would admit values between the listed
    * offsets -- 61 minutes, say -- which no zone uses and the form never
    * offers. */
   for (int i = 0; i < (int)(sizeof TZ_MIN / sizeof TZ_MIN[0]); i++)
      if (TZ_MIN[i] == v) {
         *out = (int)v;
         return 1;
      }
   return 0;
}

void h_settings(struct req *r, long me, const char *cookie, const char *note)
{
   char email[256], esc[300] = {0}, csrf[64];
   email_of(r->db, me, email, sizeof email);
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
   if (pair_is_paired(r->db, me)) {
      sb_add(&s,
             "<p>An app is paired. To pair a different one, unpair first.</p>"
             "<form method=post action=\"/settings/unpair\">"
             "<input type=hidden name=csrf value=\"%s\">"
             "<button>Unpair</button></form>",
             csrf);
   } else {
      sqlite3_stmt *st =
          db_prep(r->db, "SELECT code,expires_at FROM pairing WHERE user_id=?");
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
   sqlite3_stmt *fl = db_prep(r->db, "SELECT u.id,u.email FROM share s"
                                     " JOIN user u ON u.id=s.viewer_id"
                                     " WHERE s.owner_id=? ORDER BY u.email");
   int nf           = 0;
   if (fl) {
      sqlite3_bind_int64(fl, 1, me);
      sb_add(&s, "<table>");
      int frc;
      while ((frc = sqlite3_step(fl)) == SQLITE_ROW) {
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
      /* A follower list that stopped early is a list of people who still have
       * access and are no longer shown -- exactly the row a revoke page must
       * not hide. */
      if (!db_finished(frc))
         sb_add(&s, "<p>(this list could not be read in full)</p>");
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

   /* Live share links, shown as the invited person will receive them.
    *
    * FROM THE CONFIGURED ORIGIN, NEVER THE REQUEST'S. This used to read the
    * request's own `Host` header, on the argument that the server "has no other
    * way to know what it is called from outside" -- which is true, and is
    * exactly why it has to be told rather than asked. The value was escaped for
    * HTML, so it was never an injection; it was worse than that. These links
    * carry a LIVE SINGLE-USE TOKEN, so an authenticated request arriving with
    * `Host: evil.example` -- through a permissive proxy, or a second name
    * pointed at this address -- rendered `https://evil.example/invite/<token>`
    * on the owner's own settings page. The owner copies what the page shows and
    * sends it to the person they meant to invite; the token arrives at somebody
    * else's domain, and it is still good.
    *
    * public_origin() is configured (PANCRA_ORIGIN, see srv/util.h) and
    * validated, so no request can steer a token into a host this deployment
    * was not set up with. Still escaped, because a configured value is trusted
    * to be ours and not trusted to be free of HTML. */
   char hesc[240] = {0};
   html_esc(hesc, sizeof hesc, public_origin());

   sqlite3_stmt *tk =
       db_prep(r->db, "SELECT token,email,expires_at FROM share_token"
                      " WHERE owner_id=? AND used_at IS NULL"
                      " AND expires_at>? ORDER BY created_at DESC");
   if (tk) {
      sqlite3_bind_int64(tk, 1, me);
      sqlite3_bind_int64(tk, 2, (long)time(NULL));
      int any = 0;
      int trc;
      while ((trc = sqlite3_step(tk)) == SQLITE_ROW) {
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
      if (!db_finished(trc))
         sb_add(&s, "<p>(the invitation list could not be read in full)</p>");
      sqlite3_finalize(tk);
   }

   /* Time zone and password. */
   /* The STORED setting, not the resolved one: the box showed the offset the
    * app happened to be reporting, so "follow the phone" looked like a
    * deliberate choice of UTC. */
   long tz_set = 0;
   /* THE SETTINGS PAGE IS THE ONE PAGE THAT MUST NOT GUESS. A database that
    * cannot report the stored offset is not a user who has not chosen one:
    * shown as "not set" the page presents "follow the phone" as a deliberate
    * choice, and the next save writes that guess back as if the user had made
    * it. Refuse the page instead. */
   enum db_get tzg = db_get_long(
       r->db, "SELECT tz_offset FROM user WHERE id=? AND tz_offset IS NOT NULL",
       me, &tz_set);
   if (tzg == DB_GET_FAIL) {
      sb_free(&s);
      oops_busy(r);
      return;
   }
   int tz_found = (tzg == DB_GET_VALUE);
   int tz_known = 0;
   int tz_auto  = tz_resolve(r->db, me, &tz_known);
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
 * for every action, and one that is easy to forget per-action.
 *
 * SO IS THE METHOD. Every action here mutates, so "/settings/" is declared
 * POST-only in web.c's WEB_RULES and refused with a 405 before this function
 * runs. It matters that the declaration is there and not here: the router used
 * to dispatch on `!get`, which made every one of the nine actions below --
 * including `password`, `delete` and `revoke` -- reachable by PUT, PATCH,
 * DELETE and HEAD. There is no per-action method check to forget, because there
 * is no per-action method: this handler is only ever reached by POST. */
void h_settings_post(struct req *r, long me, const char *cookie,
                     const char *what)
{
   if (!strcmp(what, "pair")) {
      char code[16];
      if (pair_is_paired(r->db, me))
         h_settings(r, me, cookie, "Unpair the current app first.");
      else if (pair_code_new(r->db, me, code, sizeof code))
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
      int unpaired = pair_unpair(r->db, me);
      pair_unlock();
      /* SAY WHAT HAPPENED. "The app is unpaired" after a failed delete is a
       * revocation the user believes in and the server did not perform: the
       * phone's key goes on signing requests. */
      if (!unpaired) {
         oops(r);
         return;
      }
      h_settings(r, me, cookie, "The app is unpaired.");
      return;
   }
   if (!strcmp(what, "tz")) {
      char tz[16];
      /* THE FIELD MUST BE THERE. form_field says whether it found one, and
       * that answer is the difference between the two ways of arriving with an
       * empty string: `tz=` in the body, which is the <select>'s own "follow
       * the data" option and a deliberate choice, and NO `tz` at all, which is
       * a body this form never produces (a select always submits its selected
       * option). Ignoring the return conflated them, so a POST carrying nothing
       * but a valid CSRF token CLEARED the owner's stored offset and answered
       * "Time zone saved." -- a change nobody asked for, reported as one they
       * did. A missing field is now refused like any other value that is not on
       * the list, with the setting left standing.
       *
       * `tz` is left initialised whatever the answer: form_field writes ""
       * for everything but FORM_OK. */
      /* FIVE ANSWERS NOW, AND ONLY ONE OF THEM IS A SETTING.
       *
       * `present` used to be the decoder's single bit. The distinction it drew
       * -- "tz=" (the <select>'s deliberate "follow the data" choice) versus
       * no tz field at all (a body this form never produces) -- is still
       * exactly the FORM_OK/FORM_ABSENT distinction, and is preserved.
       *
       * What it could not draw is the other three. tz is a `char[16]`, so
       * item 47 measured the truncation case here: a value longer than 15
       * bytes was CLIPPED, and it happened not to be able to clip into
       * something tz_canonical accepts, because the clip lands in the
       * still-encoded text and a cut "%"-escape decodes to literal
       * characters. That was a fact about where the cut fell, not a rule.
       * FORM_TOO_LONG makes it a rule, and it now refuses instead of
       * validating the wrong string. */
      enum form_field ftz =
          form_field(r->body, r->body_len, "tz", tz, sizeof tz);
      int present = ftz == FORM_OK;
      switch (ftz) {
         case FORM_OK:
         case FORM_ABSENT: break;
         case FORM_MALFORMED:
         case FORM_TOO_LONG:
         case FORM_DUPLICATE:
            /* Same sentence as a value that is not on the list, and for the
             * same reason: the setting was NOT changed, and that is the whole
             * of what the person at the browser needs to know. */
            present = 0;
            break;
      }
      /* VALIDATED BEFORE THE DATABASE IS OPENED, so a refusal cannot leave a
       * half-applied change and cannot be reported as a save. The empty value
       * is the form's explicit "follow the data" choice and is the one
       * non-numeric answer that means something. */
      int tzv = 0;
      if (!present || (tz[0] && !tz_canonical(tz, &tzv))) {
         h_settings(r, me, cookie,
                    "That is not one of the offsets on the list; "
                    "the time zone was not changed.");
         return;
      }
      sqlite3_stmt *st =
          db_prep(r->db, "UPDATE user SET tz_offset=? WHERE id=?");
      int ok = 0; /* did the statement actually run? */
      if (st) {
         if (tz[0])
            sqlite3_bind_int(st, 1, tzv);
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
      /* BEFORE THE CURRENT PASSWORD IS EVEN CHECKED. A clipped `new` would be
       * STORED clipped -- the owner would then be locked out by the password
       * they had just chosen -- and user_set_password signs every other
       * session out in the same transaction, so the damage is not recoverable
       * by staying signed in elsewhere. "new=a&new=b" had two answers and
       * this end picked one. Refused before either half runs. */
      enum form_field fo =
          form_field(r->body, r->body_len, "old", old, sizeof old);
      enum form_field fn =
          form_field(r->body, r->body_len, "new", new, sizeof new);
      if (fo != FORM_OK || fn != FORM_OK) {
         h_settings(r, me, cookie,
                    "The password was not changed: that form did not arrive "
                    "in one piece. Try again.");
         return;
      }
      int pwok = user_check_password(r->db, me, old);
      if (pwok < 0)
         oops(r); /* could not be CHECKED: not the same as wrong */
      else if (!pwok)
         h_settings(r, me, cookie, "That is not the current password.");
      else if (!user_set_password(r->db, me, new))
         /* The message no longer claims which half failed, because there are
          * no halves: user_set_password changes the password and signs every
          * session out in one transaction, and 0 means neither happened. */
         h_settings(r, me, cookie,
                    "The password was not changed. It cannot be empty, and "
                    "nothing is changed unless every other session can also "
                    "be signed out.");
      else
         h_settings(r, me, cookie,
                    "Password changed, and every other browser signed out.");
      return;
   }
   if (!strcmp(what, "delete")) {
      char typed[256], mine[256];
      /* THE STRONGEST GUARD IN THE PRODUCT, so it gets the strictest read of
       * its field. strcasecmp is a C-string compare, which is exactly what
       * "confirm=<address>%00x" was built to exploit: it would have matched
       * the stored address and CASCADED EVERY TABLE, on a request whose
       * confirmation field was not the address at all. */
      if (form_field(r->body, r->body_len, "confirm", typed, sizeof typed) !=
          FORM_OK) {
         h_settings(r, me, cookie,
                    "Type the account's email address to confirm; nothing "
                    "was deleted.");
         return;
      }
      email_of(r->db, me, mine, sizeof mine);
      /* BOTH must be non-empty, not merely equal.
       *
       * email_of writes "" and returns on any failure -- no row, a prepare
       * that failed, a NULL column -- and form_field leaves "" for a field
       * that was not sent. So "" == "" compared equal and deleted the account,
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
      sqlite3_stmt *st = db_prep(r->db, "DELETE FROM user WHERE id=?");
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
      /* "Sign out everywhere" that did not is the one answer this button
       * must never give: it is what someone clicks when they believe another
       * person is signed in as them. */
      if (!session_drop_all(r->db, me)) {
         oops(r);
         return;
      }
      redirect(r, "/login", NULL);
      return;
   }
   if (!strcmp(what, "share")) {
      char email[256], token[TOKEN_HEX + 1] = {0};
      /* Refused before share_token_mint, which is a transaction: it prunes
       * dead rows and enforces the owner's quota, and neither should run for
       * an address this server will not send to. */
      if (form_field(r->body, r->body_len, "email", email, sizeof email) !=
          FORM_OK) {
         h_settings(r, me, cookie,
                    "That is not an email address; no link was created.");
         return;
      }
      /* THE INSERT USED TO BE RIGHT HERE, on its own, and nothing anywhere
       * removed the row it made. share_token_mint prunes the dead rows and
       * enforces the owner's quota in the same transaction; see auth.c for
       * why the quota REFUSES rather than replacing the oldest link. */
      switch (share_token_mint(r->db, me, email, token, sizeof token)) {
         case SHARE_MINT_OK:
            h_settings(r, me, cookie, "Share link created; copy it below.");
            return;
         case SHARE_MINT_FULL: {
            /* NAMED AS A LIMIT AND NOT AS A FAILURE, with the way out in the
             * same sentence. The list of unused links, and a Revoke button
             * against each of them, is rendered directly below this message --
             * so "revoke one below" points at something the reader can already
             * see rather than at a page they have to go and find. */
            char msg[128];
            (void)snprintf(msg, sizeof msg,
                           "You already have %d unused invitation links, which "
                           "is as many as one account may have at once. Revoke "
                           "one below to make room.",
                           MAX_LIVE_TOKENS);
            h_settings(r, me, cookie, msg);
            return;
         }
         /* NAMED, not left to `default`: -Wswitch-enum is on, so a fourth
          * answer added to `enum share_mint` later must be given a sentence
          * here rather than silently inheriting "could not create". */
         case SHARE_MINT_FAILED:
            h_settings(r, me, cookie, "Could not create the share link.");
            return;
      }
      /* Unreachable, and still written: a value outside the enum can only
       * come from a corrupted return, and falling off the end of this handler
       * would answer the POST with nothing at all. */
      h_settings(r, me, cookie, "Could not create the share link.");
      return;
   }
   if (!strcmp(what, "revoke-link")) {
      char tok[TOKEN_HEX + 1];
      /* BEFORE THE DELETE. sqlite3_bind_text with -1 measures the value with
       * strlen, so an embedded NUL made the row that is deleted differ from
       * the token that was sent -- and the buffer is exactly TOKEN_HEX + 1,
       * so anything longer used to be clipped to a token-shaped prefix. */
      if (form_field(r->body, r->body_len, "token", tok, sizeof tok) !=
          FORM_OK) {
         h_settings(r, me, cookie,
                    "That link was not found; nothing was revoked.");
         return;
      }
      sqlite3_stmt *st = db_prep(
          r->db, "DELETE FROM share_token WHERE token=? AND owner_id=?");
      int ok = 0; /* did the statement actually run? */
      if (st) {
         sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
         sqlite3_bind_int64(st, 2, me);
         /* db_changes, not SQLITE_DONE: a DELETE that matched nothing is also
          * DONE, so a mistyped or already-revoked token reported success. */
         ok = sqlite3_step(st) == SQLITE_DONE && db_changes(r->db) > 0;
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
      /* BEFORE THE DELETE, as above. This one is read with strtol, which
       * stops at the first byte that is not a digit -- so a malformed value
       * was never going to be reported, only silently reinterpreted. */
      if (form_field(r->body, r->body_len, "who", who, sizeof who) != FORM_OK) {
         h_settings(r, me, cookie,
                    "That follower was not found; nothing was revoked.");
         return;
      }
      sqlite3_stmt *st =
          db_prep(r->db, "DELETE FROM share WHERE owner_id=? AND viewer_id=?");
      int ok = 0; /* did the statement actually run? */
      if (st) {
         sqlite3_bind_int64(st, 1, me);
         sqlite3_bind_int64(st, 2, strtol(who, NULL, 10));
         /* And it must have matched a row. SQLITE_DONE alone is true of a
          * DELETE that found nothing, so this reported a revocation that had
          * not happened -- see the note below. */
         ok = sqlite3_step(st) == SQLITE_DONE && db_changes(r->db) > 0;
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
