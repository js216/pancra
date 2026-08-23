/* SPDX-License-Identifier: GPL-3.0
 * page.c --- the page skeleton and the lookups every page needs
 * Copyright 2026 Jakob Kastelic
 */
#include "page.h"
#include "authsess.h"
#include "db.h"
#include "http.h"
#include "oops.h"
#include "pair.h"
#include "posix.h" /* the one boundary beyond ISO C -- see posix.h */
#include "rowdec.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* forward: the skeleton is defined below, page() calls it */
/* Every subpage opened the same way in the single-user version: a back link
 * ABOVE an h2 heading. (h2, not h1: at monospace an h1 dwarfs the page.) */
void sub_page(struct req *r, const char *title, const char *body_html)
{
   struct sb s = {0};
   sb_add(&s, "<a href=\"/%s\">&lt;- Main</a>\n<h2>%s</h2>\n%s", r->who, title,
          body_html);
   page(r, 200, "OK", title, s.p ? s.p : "");
   sb_free(&s);
}

/* A redirect is a RESPONSE, and goes out the same way every other one does.
 *
 * Hand-writing its own header block and pushing it straight down the socket
 * is what it must not do. Every caller of this -- login, logout, accepting an
 * invitation, saving a setting -- is reached from web_route_locked, i.e.
 * while the page mutex is HELD. So a client that asks for a redirect and then
 * stops reading blocks that mutex, and with it every page for every other
 * user, at the peer's pace. That is precisely the failure the buffered model
 * exists to prevent, and web_route's own comment describes it -- while such a
 * function would sit
 * outside the model and did it anyway.
 *
 * Now it fills in the response like page() does, and web_route flushes it
 * after unlocking. */
void redirect(struct req *r, const char *to, const char *set_cookie)
{
   int n =
       snprintf(r->resp_extra, sizeof r->resp_extra, "Location: %s\r\n%s%s%s",
                to, set_cookie ? "Set-Cookie: " : "",
                set_cookie ? set_cookie : "", set_cookie ? "\r\n" : "");
   if (n < 0 || n >= (int)sizeof r->resp_extra) {
      /* Cannot describe the redirect -- say so rather than send half of it. */
      r->resp_extra[0] = '\0';
      r->resp_code     = 500;
      r->resp_reason   = "Internal Server Error";
      r->resp_ctype    = "text/plain";
      return;
   }
   r->resp_code   = 303;
   r->resp_reason = "See Other";
   r->resp_ctype  = "text/plain";
   /* No body: resp stays NULL, and the flush sends headers alone. */
}

/* The single-user version's whole stylesheet, kept exactly: one monospace
 * face and nothing else. ("monospace,monospace" is not a typo -- the doubled
 * name dodges a browser quirk that shrinks bare `monospace` text.) The only
 * thing this build adds to a page is the name/email/logout row at the top,
 * which the single-user version had no need of. */
static const char CSS[] =
    "<style>body{font-family:monospace,monospace}</style>";

void page(struct req *r, int code, const char *reason, const char *title,
          const char *body_html)
{
   page_refresh(r, code, reason, title, body_html, 0);
}

/* `refresh` seconds, or 0 for none: the single-user main page refreshed
 * itself every two minutes so a phone left open stayed current. */
void page_refresh(struct req *r, int code, const char *reason,
                  const char *title, const char *body_html, int refresh)
{
   struct sb s = {0};
   sb_add(&s,
          "<!DOCTYPE html>\n"
          "<title>%s</title>\n"
          "<meta name=\"viewport\" content=\"width=device-width, "
          "initial-scale=1\">\n",
          title);
   if (refresh)
      sb_add(&s, "<meta http-equiv=\"refresh\" content=\"%d\">\n", refresh);
   sb_add(&s, "%s\n%s", CSS, body_html);
   if (s.err) {
      oops(r);
      sb_free(&s);
      return;
   }
   /* Hand the buffer to web_route rather than writing it here: see struct req.
    * s.p is already this request's own heap allocation, so the transfer costs
    * nothing but the assignment, and the ownership moves with it. */
   r->resp        = s.p;
   r->resp_n      = s.n;
   r->resp_code   = code;
   r->resp_reason = reason;
   r->resp_ctype  = "text/html";
}

int64_t web_user(struct req *r, char *cookie, size_t cap, int *failed)
{
   char all[1024];
   cookie[0] = '\0';
   if (!hdr_get(r->hdr, "Cookie", all, sizeof all))
      return 0;
   for (char *p = all; p && *p;) {
      while (*p == ' ')
         p++;
      if (!strncmp(p, "sid=", 4)) {
         size_t k = 0;
         for (char *v = p + 4; *v && *v != ';' && k + 1 < cap; v++)
            cookie[k++] = *v;
         cookie[k] = '\0';
         break;
      }
      p = strchr(p, ';');
      if (p)
         p++;
   }
   if (failed)
      *failed = 0;
   if (!cookie[0])
      return 0;
   /* THE REQUEST-POLICY BOUNDARY, and the only place the two halves meet.
    *
    * session_verify answers and writes nothing; session_refresh does the
    * housekeeping that answer implies. They are separate because the router
    * gates methods BEFORE this is called (srv/web.c) -- a rule that only
    * works if it is visible, and it was not while one function called
    * `session_user` quietly wrote a row on every page view. Anything that
    * needs to know WHO without touching the database calls session_verify
    * directly and does not come through here. */
   int64_t uid             = 0;
   int64_t seen            = 0;
   enum session_check what = session_verify(r->db, cookie, &uid, &seen);
   session_refresh(r->db, cookie, what, seen);
   if (what == SESSION_UNAVAILABLE && failed)
      *failed = 1;
   return what == SESSION_OK ? uid : 0;
}

enum db_get email_of(struct db *d, int64_t uid, char *out, size_t cap)
{
   /* FOUR DIFFERENT THINGS, NOT ONE EMPTY STRING: a prepare that failed, a
    * step that failed, a user id that matches nothing, and a row whose email
    * column is NULL. Collapsed, every caller renders "" -- an empty
    * identity on a page that says whose data it is showing, which reads as
    * "this account has no address" rather than "this server could not
    * answer". On the sharing pages it is worse: the page names the account
    * you are about to grant access to.
    *
    * The three answers are the ones enum db_get already names, and using it
    * rather than inventing a second vocabulary means a caller that handles a
    * DB failure here handles it the same way it does everywhere else. */
   out[0]           = '\0';
   sqlite3_stmt *st = db_prep(d, "SELECT email FROM user WHERE id=?");
   if (!st)
      return DB_GET_FAIL;
   sqlite3_bind_int64(st, 1, uid);
   int rc        = sqlite3_step(st);
   enum db_get g = DB_GET_FAIL;
   if (rc == SQLITE_ROW) {
      const char *e = (const char *)sqlite3_column_text(st, 0);
      /* A NULL COLUMN IS NOT AN ADDRESS. The schema says NOT NULL, so this is
       * a database somebody has edited or one written by a version that did
       * not -- either way the honest answer is that there is no email here,
       * not that it is the empty string. */
      if (e) {
         snprintf(out, cap, "%s", e);
         g = DB_GET_VALUE;
      } else {
         g = DB_GET_NONE;
      }
   } else if (rc == SQLITE_DONE) {
      g = DB_GET_NONE; /* no such user */
   }
   sqlite3_finalize(st);
   return g;
}

/* Minutes east of UTC for rendering this user's timestamps.
 *
 * The board has no zoneinfo database, so a named zone could not be resolved
 * even if one were stored. It does not need one: the app already writes the
 * offset it was using into every reading, so with no explicit setting the
 * newest reading's own offset is both automatic and right -- including after
 * a trip, and without anybody configuring anything. */
/* The offset to render this user's timestamps in, and -- through `have` --
 * whether anything actually supplied it. The two are different answers: an
 * offset of 0 because the user is on UTC is not the same as 0 because no
 * reading has ever arrived, and the settings page must not present the second
 * as though it were the first. */
int tz_resolve(struct db *d, int64_t uid, int *have)
{
   if (have)
      *have = 1;
   int64_t v     = 0;
   enum db_get g = db_get_long(d,
                               "SELECT tz_offset FROM user WHERE id=? AND"
                               " tz_offset IS NOT NULL",
                               uid, &v);
   if (g == DB_GET_VALUE)
      return (int)v;
   if (have)
      *have = 0;
   /* A DATABASE THAT COULD NOT ANSWER IS NOT A USER WITH NO SETTING, and the
    * difference is visible: falling through to the reading-derived offset
    * below would render every timestamp on the page in whatever that found,
    * and the settings box would present it as the stored choice. `have` is
    * already 0, so the caller that cares (the settings page) shows the
    * automatic row rather than a fabricated one -- and callers that do not
    * pass `have` get 0, which is what they got before for an unreadable
    * database anyway. */
   if (g == DB_GET_FAIL)
      return 0;
   /* Whose readings to take the offset from: this user's own, or -- for a
    * FOLLOWER, who may never pair an app at all -- those of whoever shares
    * with them. A follower is looking at the owner's timeline anyway, so the
    * owner's offset is the right default rather than UTC.
    *
    * There is no better source. HTTP carries no time zone, a browser cannot
    * say without javascript, and IP geolocation would mean either a database
    * on a 56 MB board or sending every viewer's address to somebody else. */
   sqlite3_stmt *st =
       db_prep(d, "SELECT line FROM logrow WHERE log='readings' AND user_id ="
                  " CASE WHEN EXISTS(SELECT 1 FROM logrow WHERE user_id=?1"
                  "                  AND log='readings')"
                  "      THEN ?1"
                  "      ELSE COALESCE((SELECT owner_id FROM share"
                  "                     WHERE viewer_id=?1 ORDER BY created_at"
                  "                     LIMIT 1), ?1) END"
                  " ORDER BY bucket DESC, line DESC LIMIT 1");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   int off = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      struct row_reading rr;
      /* THE SAME DECODER as every other reader (rowdec.h). This walked to the
       * eighth field and handed it to strtol, so a row that stopped early --
       * or a field with anything but digits in it -- resolved to an offset of
       * ZERO and set *have: the settings page then presented UTC as the
       * user's own timezone, and every timestamp on every page was rendered
       * in it. Not knowing is a different answer from knowing zero. */
      if (ln && row_decode(ln, (int)strlen(ln), &rr)) {
         off = rr.tz / 60;
         if (have)
            *have = 1;
      }
   }
   sqlite3_finalize(st);
   return off;
}

int tz_of(struct db *d, int64_t uid)
{
   return tz_resolve(d, uid, NULL);
}

void stamp_local(int64_t t, int tz_min, char *out, size_t cap)
{
   time_t local = (time_t)(t + (int64_t)tz_min * 60);
   struct tm tm;
   /* A TIMESTAMP THAT COULD NOT BE COMPUTED IS NOT A TIMESTAMP. gmtime_r
    * fails on a time_t it cannot represent as a date, and leaves `tm`
    * untouched -- so the fields printed below were whatever the stack held,
    * rendered as a date beside a real glucose reading. Say it is unknown. */
   if (!sys_gmtime(local, &tm)) {
      snprintf(out, cap, "(unknown time)");
      return;
   }
   snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* May `viewer` see `owner`'s record? Ownership, or a share row. Followers are
 * read-only by construction: no write path in this program consults share. */
int may_view(struct db *d, int64_t viewer, int64_t owner)
{
   if (viewer == owner)
      return 1;
   sqlite3_stmt *st =
       db_prep(d, "SELECT 1 FROM share WHERE owner_id=? AND viewer_id=?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   sqlite3_bind_int64(st, 2, viewer);
   int ok = sqlite3_step(st) == SQLITE_ROW;
   sqlite3_finalize(st);
   return ok;
}

void nav(struct sb *s, const char *email, const char *cookie)
{
   char esc[256] = {0};
   char csrf[64];
   html_esc(esc, sizeof esc, email);
   csrf_token(cookie, csrf, sizeof csrf);
   /* The name is in the tab title; repeating it on the page only pushed the
    * reading down. The row is the account and the way out of it.
    *
    * The token is here because /logout is a state change and the route now
    * refuses one without it: cookies are SameSite=Lax, which still sends them
    * on a cross-site top-level navigation, so any page could log the user out
    * by pointing at this URL. Harmless next to the other routes, but it is
    * the same guard for the same reason and it costs one hidden field. */
   sb_add(s,
          "<div><a href=\"/settings\">%s</a> &nbsp; "
          "<form method=post action=\"/logout\" style=\"display:inline\">"
          "<input type=hidden name=csrf value=\"%s\">"
          "<button>Log out</button></form></div>\n",
          esc, csrf);
}

/* The data page: the newest reading, then the recent ones. Rows are parsed
 * here rather than stored parsed -- see the note at the top of db.c. */
/* The main page, in the single-user version's exact shape: the reading's
 * timestamp, the big number (which links to itself, so tapping it reloads),
 * then one <pre> row per hour of the last 24 hours with each reading in its
 * five-minute slot -- "." for a slot with nothing, [brackets] for a meter
 * fingerstick, a bold date header at each midnight. Rows newest first.
 *
 * Rows are parsed here rather than stored parsed (see db.c). */
#define HSLOTS 12
#define HMAX   400

struct hpt {
   int slot;
   int64_t glu;
   int ty;
};

void set_cookie_str(const char *cookie, char *out, size_t cap)
{
   /* Secure: this server is only reachable over TLS. HttpOnly: no script has
    * any reason to read it, and that is what keeps a cross-site scripting bug
    * from becoming a stolen year-long login. Lax: the login survives a normal
    * link from elsewhere, but does not ride along on a cross-site POST. */
   snprintf(out, cap,
            "sid=%s; Max-Age=%" PRIwire
            "; Path=/; HttpOnly; Secure; SameSite=Lax",
            cookie, (int64_t)SESS_TTL);
}

/* Every state-changing form carries the token; without this check a page on
 * another site could submit these forms with the visitor's cookie attached. */
int csrf_guard(struct req *r, const char *cookie)
{
   char sent[128];
   /* THE TOKEN MUST BE THE ONLY ONE, AND IT MUST BE A TOKEN.
    *
    * Read the field, throw the decoder's answer away and go straight into
    * csrf_ok on whatever was written, and two shapes get past:
    *
    *   "csrf=<valid>%00anything"  decodes to a NUL-terminated <valid>, and
    *      csrf_ok is a string compare, so it PASSES -- while a proxy, a log
    *      or a WAF counting bytes sees a different value entirely.
    *   "csrf=<valid>&csrf=<junk>" is answered with the first, silently. Any
    *      other reader of the same body is free to prefer the last.
    *
    * Neither is a token this browser was given, so neither is a form this
    * user submitted. Every not-OK answer is refused the same way and with the
    * same page: which of the five it was tells an attacker how their guess
    * was WRONG, and the honest answer to all of them is that the form did not
    * come from here. They are still named one by one so that -Wswitch-enum
    * makes a sixth answer, added later, a compile error here rather than a
    * silent "carry on". */
   int usable = 0;
   switch (form_field(r->body, r->body_len, "csrf", sent, sizeof sent)) {
      case FORM_OK: usable = 1; break;
      case FORM_ABSENT:
      case FORM_MALFORMED:
      case FORM_TOO_LONG:
      case FORM_DUPLICATE: usable = 0; break;
   }
   if (usable && csrf_ok(cookie, sent))
      return 1;
   page(r, 403, "Forbidden", "expired",
        "<p>That form expired. <a href=\"/settings\">Try again</a>.</p>");
   return 0;
}

/* The record a page is being asked for: `who=` when it is shared with the
 * viewer, otherwise their own. Answers the request itself and returns 0 when
 * it is not shared, so every page refuses the same way. */
/* A viewer looking at their OWN record needs no marker; one looking at a
 * shared record needs every link to say so, or the next click silently lands
 * them back on their own. */
static void set_who(struct req *r, int64_t owner, int64_t me)
{
   if (owner > 0 && owner != me)
      (void)snprintf(r->who, sizeof r->who, "?who=%" PRIwire "", owner);
   else
      r->who[0] = '\0';
}

/* Whose record this viewer is looking at, and whether they have one of their
 * own (*have_own, may be NULL).
 *
 * A follower with no app of their own has nothing to show on their own page,
 * and had to know to type "?who=2" to see the only record they can actually
 * look at. If they have no readings, no paired app and follow exactly one
 * person, that record IS their page. An explicit "who=" always wins: it is
 * the viewer saying which record they want. */
int64_t viewed_owner(struct req *r, int64_t me, int *have_own)
{
   int own_dummy = 0;
   if (!have_own)
      have_own = &own_dummy;
   int64_t owner = owner_of(r, me);
   *have_own     = 0;
   if (!owner || owner != me || strstr(r->target, "who=")) {
      set_who(r, owner, me);
      return owner;
   }

   /* "DOES THIS VIEWER HAVE A RECORD OF THEIR OWN?" -- and a database that
    * cannot say must not answer NO. Answering no sends the caller down the
    * follower path below, which can hand back somebody ELSE'S account as the
    * owner: the wrong person's readings, rendered as the viewer's own,
    * because a query errored. Treated as "yes, their own", the worst case is
    * an empty page about the right account. */
   enum db_get gm = db_get_long(r->db,
                                "SELECT 1 FROM logrow WHERE user_id=? AND"
                                " log='readings' LIMIT 1",
                                me, NULL);
   int mine       = (gm != DB_GET_NONE); /* VALUE or FAIL */
   /* `!= 0` INCLUDES THE FAILURE ANSWER, and that is the documented
    * conservative choice here: treated as "yes, their own", the
    * worst case is an empty page about the RIGHT account, while treating it
    * as "no" would re-point the view at somebody else's readings because a
    * query errored. The other three callers do not get to make that trade --
    * see settings.c and home.c, where the same -1 blocked recovery or
    * asserted a state nobody established. */
   *have_own = mine || pair_is_paired(r->db, me) != 0;
   if (*have_own) {
      set_who(r, owner, me);
      return owner;
   }

   int64_t only = 0;
   /* ONLY an actual row re-points the view. A failure here leaves `owner` as
    * the viewer, which is the conservative answer: showing somebody their own
    * empty record is recoverable, showing them a stranger's is not. */
   if (db_get_long(r->db,
                   "SELECT owner_id FROM share WHERE viewer_id=?"
                   " AND (SELECT count(*) FROM share WHERE viewer_id=?1)"
                   "     = 1",
                   me, &only) == DB_GET_VALUE &&
       only > 0)
      owner = only;
   set_who(r, owner, me);
   return owner;
}

int64_t owner_of(struct req *r, int64_t me)
{
   int64_t owner = me;
   const char *q = strstr(r->target, "who=");
   if (q)
      owner = strtoll(q + 4, NULL, 10);
   if (owner <= 0)
      owner = me;
   if (!may_view(r->db, me, owner)) {
      page(r, 403, "Forbidden", "Pancra",
           "<p>That record is not shared with you.</p>"
           "<p><a href=\"/\">Back</a></p>");
      return 0;
   }
   return owner;
}
