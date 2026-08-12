/* SPDX-License-Identifier: GPL-3.0
 * web.c --- the router, and nothing else
 * Copyright 2026 Jakob Kastelic
 *
 * Every page lives in its own file (see page.h); this one only decides which.
 * It was 1500 lines of skeleton, record page, settings, invitation and plots
 * in one unit -- four unrelated things sharing a file because they happened
 * to be reached the same way.
 */
#include "auth.h"
#include "http.h"
#include "page.h"
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
   if (r->resp) {
      http_respond(r->fd, r->resp_code, r->resp_reason, r->resp_ctype, r->resp,
                   r->resp_n);
      free(r->resp);
      r->resp   = NULL;
      r->resp_n = 0;
   }
}

static void web_route_locked(struct req *r)
{
   char cookie[128];
   long me = web_user(r, cookie, sizeof cookie);
   int get = !strcmp(r->method, "GET");

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
      if (get)
         h_login_form(r, NULL);
      else
         h_login_post(r);
      return;
   }
   if (!me) {
      if (get)
         h_login_form(r, NULL);
      else
         redirect(r, "/login", NULL);
      return;
   }
   if (!strcmp(r->path, "/logout")) {
      /* POST and a token, like every other state change. A GET /logout with
       * no token could be triggered by any page the user happened to visit. */
      if (get || !csrf_guard(r, cookie))
         return;
      session_drop(cookie);
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
   if (!strncmp(r->path, "/settings/", 10) && !get) {
      if (!csrf_guard(r, cookie))
         return;
      h_settings_post(r, me, cookie, r->path + 10);
      return;
   }
   page(r, 404, "Not Found", "not found",
        "<p>No such page. <a href=\"/\">Home</a></p>");
}
