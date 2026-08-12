/* SPDX-License-Identifier: GPL-3.0
 * page.c --- the page skeleton and the lookups every page needs
 * Copyright 2026 Jakob Kastelic
 */
#include "page.h"
#include "auth.h"
#include "db.h"
#include "http.h"
#include "pair.h"
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

void oops(struct req *r)
{
   http_text(r->fd, 500, "Internal Server Error", "server error\n");
}

void redirect(struct req *r, const char *to, const char *set_cookie)
{
   char hdr[512];
   int n = snprintf(hdr, sizeof hdr,
                    "HTTP/1.1 303 See Other\r\n"
                    "Location: %s\r\n"
                    "%s%s%s"
                    "Content-Length: 0\r\n"
                    "Connection: %s\r\n\r\n",
                    to, set_cookie ? "Set-Cookie: " : "",
                    set_cookie ? set_cookie : "", set_cookie ? "\r\n" : "",
                    http_conn_value());
   if (n > 0)
      (void)!http_write(r->fd, hdr, (size_t)n);
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

long web_user(struct req *r, char *cookie, size_t cap)
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
   if (!cookie[0])
      return 0;
   return session_user(cookie);
}

void email_of(long uid, char *out, size_t cap)
{
   out[0]           = '\0';
   sqlite3_stmt *st = db_prep("SELECT email FROM user WHERE id=?");
   if (!st)
      return;
   sqlite3_bind_int64(st, 1, uid);
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *e = (const char *)sqlite3_column_text(st, 0);
      snprintf(out, cap, "%s", e ? e : "");
   }
   sqlite3_finalize(st);
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
int tz_resolve(long uid, int *have)
{
   int found = 0;
   if (have)
      *have = 1;
   long v = db_one_long("SELECT tz_offset FROM user WHERE id=? AND"
                        " tz_offset IS NOT NULL",
                        uid, &found);
   if (found)
      return (int)v;
   if (have)
      *have = 0;
   /* Whose readings to take the offset from: this user's own, or -- for a
    * FOLLOWER, who may never pair an app at all -- those of whoever shares
    * with them. A follower is looking at the owner's timeline anyway, so the
    * owner's offset is the right default rather than UTC.
    *
    * There is no better source. HTTP carries no time zone, a browser cannot
    * say without javascript, and IP geolocation would mean either a database
    * on a 56 MB board or sending every viewer's address to somebody else. */
   sqlite3_stmt *st =
       db_prep("SELECT line FROM logrow WHERE log='readings' AND user_id ="
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
      if (ln) {
         /* epoch,glu,trend,rssi,lag,src,raw,tz_off,kind[,rescale] */
         const char *p = ln;
         for (int f = 0; f < 7 && p; f++) {
            p = strchr(p, ',');
            if (p)
               p++;
         }
         if (p) {
            off = (int)(strtol(p, NULL, 10) / 60);
            if (have)
               *have = 1;
         }
      }
   }
   sqlite3_finalize(st);
   return off;
}

int tz_of(long uid)
{
   return tz_resolve(uid, NULL);
}

void stamp_local(long t, int tz_min, char *out, size_t cap)
{
   time_t local = (time_t)(t + (long)tz_min * 60);
   struct tm tm;
   gmtime_r(&local, &tm);
   snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* May `viewer` see `owner`'s record? Ownership, or a share row. Followers are
 * read-only by construction: no write path in this program consults share. */
int may_view(long viewer, long owner)
{
   if (viewer == owner)
      return 1;
   sqlite3_stmt *st =
       db_prep("SELECT 1 FROM share WHERE owner_id=? AND viewer_id=?");
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
   long glu;
   int ty;
};

void set_cookie_str(const char *cookie, char *out, size_t cap)
{
   /* Secure: this server is only reachable over TLS. HttpOnly: no script has
    * any reason to read it, and that is what keeps a cross-site scripting bug
    * from becoming a stolen year-long login. Lax: the login survives a normal
    * link from elsewhere, but does not ride along on a cross-site POST. */
   snprintf(out, cap,
            "sid=%s; Max-Age=%ld; Path=/; HttpOnly; Secure; SameSite=Lax",
            cookie, (long)SESS_TTL);
}

/* Every state-changing form carries the token; without this check a page on
 * another site could submit these forms with the visitor's cookie attached. */
int csrf_guard(struct req *r, const char *cookie)
{
   char sent[128];
   form_get(r->body, r->body_len, "csrf", sent, sizeof sent);
   if (csrf_ok(cookie, sent))
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
static void set_who(struct req *r, long owner, long me)
{
   if (owner > 0 && owner != me)
      (void)snprintf(r->who, sizeof r->who, "?who=%ld", owner);
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
long viewed_owner(struct req *r, long me, int *have_own)
{
   int own_dummy = 0;
   if (!have_own)
      have_own = &own_dummy;
   long owner = owner_of(r, me);
   *have_own  = 0;
   if (!owner || owner != me || strstr(r->target, "who=")) {
      set_who(r, owner, me);
      return owner;
   }

   int mine = 0;
   db_one_long("SELECT 1 FROM logrow WHERE user_id=? AND log='readings'"
               " LIMIT 1",
               me, &mine);
   *have_own = mine || pair_is_paired(me);
   if (*have_own) {
      set_who(r, owner, me);
      return owner;
   }

   int one = 0;
   long only =
       db_one_long("SELECT owner_id FROM share WHERE viewer_id=?"
                   " AND (SELECT count(*) FROM share WHERE viewer_id=?1)"
                   "     = 1",
                   me, &one);
   if (one && only > 0)
      owner = only;
   set_who(r, owner, me);
   return owner;
}

long owner_of(struct req *r, long me)
{
   long owner    = me;
   const char *q = strstr(r->target, "who=");
   if (q)
      owner = strtol(q + 4, NULL, 10);
   if (owner <= 0)
      owner = me;
   if (!may_view(me, owner)) {
      page(r, 403, "Forbidden", "Pancra",
           "<p>That record is not shared with you.</p>"
           "<p><a href=\"/\">Back</a></p>");
      return 0;
   }
   return owner;
}
