/* SPDX-License-Identifier: GPL-3.0
 * web.c --- the router, and nothing else
 * Copyright 2026 Jakob Kastelic
 *
 * Every page lives in its own file (see page.h); this one only decides which.
 * It was 1500 lines of skeleton, record page, settings, invitation and plots
 * in one unit -- four unrelated things sharing a file because they happened
 * to be reached the same way.
 */
#include "web.h"
#include "auth.h"
#include "http.h"
#include "page.h"
#include "plots.h"
#include "util.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The plot family, matched as a family: they all resolve a record and a time
 * zone before anything else, so they are dispatched together. */
static int is_plot_path(const char *p)
{
   return !strcmp(p, "/plots") || !strncmp(p, "/plots-", 7) ||
          !strncmp(p, "/plot-", 6) || !strncmp(p, "/day-", 5) ||
          !strncmp(p, "/data-", 6);
}

static void web_route_locked(struct req *r);

/* ---- WHICH METHODS EACH BROWSER ROUTE ANSWERS ----------------------------
 *
 * ONE TABLE, and it is the whole policy for this half. What it replaces was a
 * single `int get = !strcmp(r->method, "GET")` at the top of the router and
 * then, per route, either `get` or `!get` -- which meant "not GET" was the
 * condition for CHANGING THINGS. Every method nobody had thought of was on
 * that side:
 *
 *   PUT    /login              ran h_login_post: session minted, cookie set,
 *                              the login-failure counter written
 *   DELETE /logout             dropped the session -- a body carrying the CSRF
 *                              token is all a DELETE needs, and the token is
 *                              derived from the cookie the request already has
 *   PATCH  /settings/password  changed the password and signed every other
 *                              browser out
 *   PUT    /settings/delete    deleted the account, every table cascading
 *   HEAD   /invite/<token>     redeemed the invitation: account created,
 *                              session issued, share row inserted, token spent
 *
 * and the same shape produced a quieter bug in the other direction: GET
 * /logout hit `if (get || !csrf_guard(...)) return;` and returned having
 * written NOTHING, so a browser that followed a stale /logout link got an
 * empty response and a closed connection rather than an answer.
 *
 * So the rule is stated per route, once, positively -- these methods, nothing
 * else -- and enforced in one place before anything reads the session. It is
 * not "GET is special": /logout does not answer GET at all, and no route here
 * answers HEAD (http.h says why).
 *
 * ENFORCED BEFORE web_user(), deliberately. session_user() extends a session's
 * expiry as a side effect of reading it (auth.c's rolling expiry), so gating
 * after it would let a method that is about to be refused still write a row.
 * A refusal must leave the database exactly as it found it, and that is what
 * srv/test/synctest.sh asserts with a sentinel.
 *
 * A path that is in no rule below is NOT gated here: it has no methods to
 * advertise, and it is answered by the router's own 404 (or, when signed out,
 * by the login form) exactly as before. 405 means "this route exists and does
 * not do that"; it must not become a way to enumerate paths. */
enum web_match {
   WEB_EXACT,  /* the whole path, and only that */
   WEB_PREFIX, /* the path starts with this, e.g. "/settings/<action>" */
   WEB_PLOTS   /* the plot family: is_plot_path above is the rule */
};

struct web_rule {
   enum web_match how;
   const char *path; /* NULL for WEB_PLOTS, which matches by function */
   unsigned allow;   /* HTTP_M_* from http.h */
};

static const struct web_rule WEB_RULES[] = {
    /* GET renders the invitation, POST redeems it. The redemption creates an
     * account, issues a session and inserts a share row, so it is the most
     * state-changing route in the program that a signed-OUT stranger can
     * reach -- and it was reachable by HEAD. */
    {WEB_PREFIX, "/invite/",   HTTP_M_GET | HTTP_M_POST},
    /* GET renders the form, POST checks the password. */
    {WEB_EXACT,  "/login",     HTTP_M_GET | HTTP_M_POST},
    /* POST ONLY, and that predates this table: cookies are SameSite=Lax, so
     * they ride along on a cross-site top-level navigation, and any page could
     * have logged the user out by linking here. See nav() in page.c. */
    {WEB_EXACT,  "/logout",    HTTP_M_POST             },
    {WEB_EXACT,  "/",          HTTP_M_GET              },
    {WEB_EXACT,  "/units",     HTTP_M_GET              },
    {WEB_EXACT,  "/settings",  HTTP_M_GET              },
    /* Every state change the settings page offers: pair, unpair, tz, password,
     * delete, signout-all, share, revoke-link, revoke. One rule, because they
     * are one form target with an action in the path and they all mutate. */
    {WEB_PREFIX, "/settings/", HTTP_M_POST             },
    /* The plots, the day pages, the GIFs and the datapoint lists: reading. */
    {WEB_PLOTS,  NULL,         HTTP_M_GET              },
};

#define NWEB_RULES ((int)(sizeof WEB_RULES / sizeof WEB_RULES[0]))

/* The rule for `path`, or NULL when this half has no route by that name.
 *
 * FIRST MATCH WINS and the order is written to make that irrelevant: no two
 * rules here match one path. "/settings" is exact and "/settings/" is a
 * prefix, so neither can catch the other's paths -- the exact rule cannot
 * match "/settings/tz" and the ten-character prefix cannot match "/settings".
 */
static const struct web_rule *web_rule_for(const char *path)
{
   for (int i = 0; i < NWEB_RULES; i++) {
      const struct web_rule *w = &WEB_RULES[i];
      switch (w->how) {
         case WEB_EXACT:
            if (!strcmp(path, w->path))
               return w;
            break;
         case WEB_PREFIX:
            if (!strncmp(path, w->path, strlen(w->path)))
               return w;
            break;
         case WEB_PLOTS:
            if (is_plot_path(path))
               return w;
            break;
      }
   }
   return NULL;
}

void web_method_not_allowed(struct req *r, unsigned allow)
{
   char list[64], msg[256];
   http_allow_list(allow, list, sizeof list);
   /* THE ALLOW HEADER FIRST, because without it this is not a 405 (RFC 9110
    * 9.5.5 requires one) and a 405 without it is a refusal a client cannot act
    * on. If it will not fit, say 500 rather than send a header block that
    * contradicts itself -- the same rule redirect() follows above. */
   int n = snprintf(r->resp_extra, sizeof r->resp_extra, "Allow: %s\r\n", list);
   if (n < 0 || n >= (int)sizeof r->resp_extra) {
      r->resp_extra[0] = '\0';
      page(r, 500, "Internal Server Error", "temporarily unavailable",
           "<p>The server could not answer that request.</p>");
      return;
   }
   /* THE METHOD IS NOT ECHOED BACK. It is attacker-chosen text arriving in the
    * request line, and a page that reflected it -- even escaped -- would be
    * one careless edit away from putting a stranger's markup on this origin.
    * The reader learns nothing useful from being told the word they sent; what
    * they need is the list of methods that would have worked. */
   (void)snprintf(msg, sizeof msg,
                  "<p>That address does not answer this kind of request. "
                  "It allows: %s.</p>\n<p><a href=\"/\">Home</a></p>",
                  list);
   page(r, 405, "Method Not Allowed", "method not allowed", msg);
   /* page() answers 500 itself if the page could not be built, and a 500 must
    * not carry this route's Allow header: it is not a statement about methods
    * any more. */
   if (r->resp_code != 405)
      r->resp_extra[0] = '\0';
}

/* THE PAGES SHARE THEIR SCRATCH SPACE.
 *
 * The record table, the unit log and the plot renderer each build into large
 * file-scope buffers (home.c's `pre`, plots.c's framebuffer and GIF buffer,
 * plotpages.c's day list) -- deliberately, because a 56 MB board cannot put
 * a 720x300 framebuffer on a thread stack, and giving each of ten workers its
 * own copy would cost around eighteen megabytes.
 *
 * So the pages are drawn one at a time. That is no worse than before -- with
 * one core they were always going to be -- and it costs the pool nothing that
 * matters, because what the workers overlap is the WAITING: handshakes, slow
 * clients, silent ones. The /v1 sync API takes no part in this and stays
 * fully concurrent; it holds no shared buffers, and every worker now has its
 * own sqlite connection (srv/db.c), so a bucket PUT's transaction is its own
 * -- the earlier claim here, that "sqlite does its own locking", was true of
 * individual calls and false of transactions, which is where it mattered. */
static pthread_mutex_t page_mu = PTHREAD_MUTEX_INITIALIZER;

void web_route(struct req *r)
{
   pthread_mutex_lock(&page_mu);
   web_route_locked(r);
   pthread_mutex_unlock(&page_mu);
   /* THE WRITE HAPPENS HERE, OUTSIDE THE LOCK.
    *
    * Rendering is microseconds and shares the scratch buffers, so it is
    * serialised above. Sending goes at the PEER's pace and shares nothing, so
    * it must not be. With the write inside the lock, one client that asked for
    * a page and stopped reading held every other page for every other user --
    * and, before full_write learned its deadline, held them forever. */
   /* `resp_code`, not `resp`: a redirect is a response with no body, and
    * gating the flush on the body meant it had to write itself -- under the
    * lock. See redirect() in page.c. */
   if (r->resp_code) {
      http_respond_hdr(r->c, r->resp_code, r->resp_reason, r->resp_ctype,
                       r->resp_extra[0] ? r->resp_extra : NULL, r->resp,
                       r->resp_n);
      free(r->resp);
      r->resp      = NULL;
      r->resp_n    = 0;
      r->resp_code = 0;
   }
}

static void web_route_locked(struct req *r)
{
   /* THE METHOD GATE, BEFORE ANYTHING ELSE TOUCHES THE REQUEST -- before the
    * session is read, so a refusal cannot extend a session's expiry, and
    * before any handler, so it cannot mutate anything at all. See WEB_RULES. */
   const struct web_rule *rule = web_rule_for(r->path);
   unsigned m                  = http_method_bit(r->method);
   if (rule && !(m & rule->allow)) {
      web_method_not_allowed(r, rule->allow);
      return;
   }

   /* ---- THE BODY IS JUDGED BEFORE ANYBODY IS AUTHENTICATED ----
    *
    * Item 120 asks for a malformed, NUL-bearing or duplicated form field to
    * be refused "before authentication or mutation", and a per-field check
    * cannot do that: form_field answers about ONE name, so a bad `csrf` is
    * only noticed by the handler that asks for it -- and the handler runs
    * after web_user() below has looked the session up and refreshed it, after
    * the route has been chosen, and in some branches after a row has been
    * written. csrf_guard in particular is called from inside handlers, which
    * is exactly the "later" that the item rules out.
    *
    * So the shape of the whole document is settled here, on the line before
    * authentication, with no names involved: are all its escapes real
    * escapes, does it decode without a NUL, and does any name appear twice.
    * A body failing any of those is not a form this server's own pages
    * produced, and there is no reading of it that two parsers would have to
    * agree on -- which is the same argument as the request line above.
    *
    * IT COSTS THE GET ROUTES NOTHING: form_body_check returns immediately for
    * an empty body, and only POST carries one here. */
   switch (form_body_check(r->body, r->body_len)) {
      case FORM_BODY_OK: break;
      case FORM_BODY_MALFORMED:
         page(r, 400, "Bad Request", "bad form",
              "<p>That form did not arrive in one piece. "
              "<a href=\"/\">Home</a></p>");
         return;
      case FORM_BODY_DUPLICATE:
         /* NOT resolved to the first or the last. No form on this server has
          * a repeated field, so a body that has one was not built by a page
          * of ours, and picking one of the two answers is precisely the
          * silent choice this refuses to make. */
         page(r, 400, "Bad Request", "bad form",
              "<p>That form sent the same field twice. "
              "<a href=\"/\">Home</a></p>");
         return;
   }

   char cookie[128];
   int sess_failed = 0;
   long me         = web_user(r, cookie, sizeof cookie, &sess_failed);
   /* GET and POST are now the only two that reach a route below, and each
    * route is named by the bit it holds rather than by "not the other one":
    * `!get` was the condition that let every unconsidered method mutate. */
   int get  = m == HTTP_M_GET;
   int post = m == HTTP_M_POST;
   /* THE SESSION COULD NOT BE READ, which is not the same as there being no
    * session: showing the login form would tell a signed-in reader that their
    * password stopped working. */
   if (sess_failed) {
      page(r, 500, "Internal Server Error", "temporarily unavailable",
           "<p>The server could not read your session. Nothing is lost; try "
           "again in a moment.</p>");
      return;
   }

   if (!strncmp(r->path, "/invite/", 8)) {
      char tok[TOKEN_HEX + 1];
      const char *p = r->path + 8;
      size_t n      = strlen(p);
      /* "/new" was a second page when signing in and signing up were separate
       * forms. One form serves both now, but links to it are in people's
       * inboxes, so the suffix still resolves to the same invitation. */
      if (n == TOKEN_HEX + 4 && !strcmp(p + TOKEN_HEX, "/new"))
         n = TOKEN_HEX;
      if (n == TOKEN_HEX) {
         memcpy(tok, p, TOKEN_HEX);
         tok[TOKEN_HEX] = '\0';
         h_invite(r, me, cookie, tok);
      } else {
         page(r, 404, "Not Found", "no such invitation",
              "<p>That link is not valid.</p>");
      }
      return;
   }
   if (!strcmp(r->path, "/login")) {
      /* GET or POST, per WEB_RULES; `post` rather than `!get`, so a method the
       * gate somehow let through does nothing instead of signing someone in. */
      if (post)
         h_login_post(r);
      else
         h_login_form(r, NULL);
      return;
   }
   if (!me) {
      /* NOT A ROUTE-SPECIFIC BRANCH: this also catches paths that are in no
       * rule at all, which is why it still tests `get` rather than a method
       * this half declared. Nothing here mutates either way. */
      if (get)
         h_login_form(r, NULL);
      else
         redirect(r, "/login", NULL);
      return;
   }
   if (!strcmp(r->path, "/logout")) {
      /* POST and a token, like every other state change. A GET /logout with
       * no token could be triggered by any page the user happened to visit.
       *
       * The gate above has already refused everything but POST -- and it
       * ANSWERS, which this branch did not: `if (get || !csrf_guard(...))
       * return;` sent a GET /logout away with no response written at all, so a
       * stale link produced an empty reply rather than a 405. Re-tested here
       * because the alternative is a bare `return` that is correct only as
       * long as the table says what this comment says. */
      if (!post) {
         web_method_not_allowed(r, HTTP_M_POST);
         return;
      }
      if (!csrf_guard(r, cookie))
         return;
      /* THE REDIRECT IS THE CLAIM, so it may only be sent once the server
       * side really is gone.
       *
       * This was `session_drop(r->db, cookie);` -- a void call -- followed
       * unconditionally by the cookie-clearing redirect. session_drop threw
       * away both its prepare and its step results (see auth.h), so a logout
       * over a busy, full or damaged database looked exactly like a logout
       * that worked: the browser's cookie was deleted, the login form
       * appeared, and the session row sat there with a year left on it. Any
       * copy of that cookie -- taken on a shared machine, by an extension, out
       * of a browser-profile backup -- went on signing in as the user who had
       * just been shown a completed logout.
       *
       * So the answer follows the storage. On FAILED nothing about the browser
       * is touched either: clearing the cookie would leave the user unable to
       * retry the logout while the session it names is still valid, which is
       * the worst of both. They are still signed in, and the page says so. */
      switch (session_drop(r->db, cookie)) {
         case SESSION_DROP_GONE:
         /* ABSENT counts as logged out, and must: two tabs posting /logout,
          * or a "sign out everywhere" that arrived first, leave no row for the
          * second delete to find. The user's goal -- this cookie names no
          * session on the server -- is met, and answering 500 to somebody who
          * is genuinely signed out would leave their cookie in place to prove
          * the error wrong. */
         case SESSION_DROP_ABSENT: break;
         case SESSION_DROP_FAILED:
            page(r, 500, "Internal Server Error", "still signed in",
                 "<p>The server could not end this session, so you are still "
                 "signed in. Nothing was changed.</p>"
                 "<p>Try again in a moment. <a href=\"/\">Main</a></p>");
            return;
      }
      redirect(r, "/login",
               "sid=; Max-Age=0; Path=/; HttpOnly; Secure; SameSite=Lax");
      return;
   }
   if (!strcmp(r->path, "/") && get) {
      h_home(r, me, cookie);
      return;
   }
   if (get && is_plot_path(r->path)) {
      h_plot_route(r, me);
      return;
   }
   if (!strcmp(r->path, "/units") && get) {
      long owner = viewed_owner(r, me, NULL);
      if (owner)
         h_units(r, owner);
      return;
   }
   if (!strcmp(r->path, "/settings") && get) {
      h_settings(r, me, cookie, NULL);
      return;
   }
   if (!strncmp(r->path, "/settings/", 10)) {
      /* POST, and `post` rather than the old `!get`: every action behind this
       * prefix mutates -- unpair, password, delete, signout-all, revoke,
       * revoke-link -- and `!get` made all of them reachable by PUT, PATCH,
       * DELETE and HEAD with a CSRF token the request's own cookie supplies. */
      if (!post) {
         web_method_not_allowed(r, HTTP_M_POST);
         return;
      }
      if (!csrf_guard(r, cookie))
         return;
      h_settings_post(r, me, cookie, r->path + 10);
      return;
   }
   page(r, 404, "Not Found", "not found",
        "<p>No such page. <a href=\"/\">Home</a></p>");
}
