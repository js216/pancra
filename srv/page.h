/* SPDX-License-Identifier: GPL-3.0
 * page.h --- what every page in the web interface shares
 * Copyright 2026 Jakob Kastelic
 *
 * The web interface is four pages that happen to sit behind one router: the
 * record, the settings, the invitation and the plots. They shared nothing but
 * a page skeleton and half a dozen lookups, so they are four files, and this
 * header is exactly the surface between them -- small on purpose. If it grows
 * to twenty entries the split has stopped being a split.
 */
#ifndef PANCRA_PAGE_H
#define PANCRA_PAGE_H

struct db; /* db.h: every storage call names its database */

#include "db.h"    /* enum db_get: email_of answers with one */
#include "proto.h" /* struct req: what a handler is given */
#include "util.h"  /* struct sb: nav and the page builders take one */

/* The page skeleton the single-user version used: doctype, title, viewport,
 * the one-line stylesheet, then the body. `refresh` seconds, or 0 for none --
 * only the main page refreshes itself. */
void page_refresh(struct req *r, int code, const char *reason,
                  const char *title, const char *body_html, int refresh);
/* A subpage: a "<- Main" link above an h2 heading, as every subpage had. */
void sub_page(struct req *r, const char *title, const char *body_html);
/* The account row at the top: the one thing the single-user version had no
 * need of. */
void nav(struct sb *s, const char *email, const char *cookie);

/* THE PAGE SKELETON'S OWN ENTRY POINTS, declared where page.c implements
 * them. They were in the protocol header, which made every module in the
 * server -- the database, the log store, the pairing -- include the surface
 * of the web interface. */
int64_t web_user(struct req *r, char *cookie, size_t cap, int *failed);
void redirect(struct req *r, const char *to, const char *set_cookie);
/* (oops -- the one answer for "the database said no" -- is oops.h: it is a
 * response, not a page, and every layer needs it.) */
void page(struct req *r, int code, const char *reason, const char *title,
          const char *body_html);

/* Who is asking, and what they may see. */
/* THIS USER'S ADDRESS, and which of the three things happened.
 *
 * DB_GET_VALUE  `out` holds it.
 * DB_GET_NONE   no such user, or the column is NULL: `out` is empty and that
 *               is the truth about the database rather than a failure.
 * DB_GET_FAIL   the database could not answer. `out` is empty and MUST NOT be
 *               rendered as an identity -- a page that names whose data it is
 *               showing must not show a blank where the name goes.
 *
 * Returning void gives all three the same empty string. */
enum db_get email_of(struct db *d, int64_t uid, char *out, size_t cap);
int may_view(struct db *d, int64_t viewer, int64_t owner);
/* The record being viewed: `who=` when it is shared, else the viewer's own.
 * Returns 0 and answers the request itself when it is not shared. */
int64_t owner_of(struct req *r, int64_t me);
/* The same question EVERY record page must ask, including the default a lone
 * follower relies on -- and it fills r->who so the page's links keep pointing
 * at the record it answered with. `have_own` (may be NULL) reports whether
 * the viewer has a record of their own at all. */
int64_t viewed_owner(struct req *r, int64_t me, int *have_own);

/* Minutes east of UTC to render this user's timestamps in, and a local
 * "YYYY-MM-DD HH:MM" stamp. `have` (may be NULL) says whether anything
 * actually supplied the offset -- 0 because the user is on UTC is not the
 * same answer as 0 because nothing has ever synced. */
int tz_resolve(struct db *d, int64_t uid, int *have);
/* THE SAME ANSWER FOR A CALLER THAT IS RENDERING TIMESTAMPS AND NOTHING ELSE.
 *
 * WHY DISCARDING `have` IS ALL RIGHT HERE, said out loud because "the caller
 * throws the found/error flag away" is exactly the shape of defect this
 * function looks like. A page rendering a list of readings has one honest
 * fallback and it is UTC: the alternative is refusing to draw a page of
 * glucose values because their labels might be an hour out, which is a worse
 * answer for the person reading them than a label that says UTC. The SETTINGS
 * page is the one place where the difference is the subject rather than the
 * decoration, and that one calls tz_resolve and shows "automatic" rather than
 * a fabricated stored choice.
 *
 * So this is not "the flag is ignored", it is "0 is the right rendering
 * default and the page that must not use it does not call this". */
int tz_of(struct db *d, int64_t uid);
void stamp_local(int64_t t, int tz_min, char *out, size_t cap);

/* The cookie a browser is given, and the check every state-changing form
 * carries. */
void set_cookie_str(const char *cookie, char *out, size_t cap);
int csrf_guard(struct req *r, const char *cookie);

/* The pages themselves. */
void h_home(struct req *r, int64_t me, const char *cookie);
void h_units(struct req *r, int64_t owner);
void h_settings(struct req *r, int64_t me, const char *cookie,
                const char *note);
/* `what` is the path after "/settings/"; CSRF is the caller's job. */
void h_settings_post(struct req *r, int64_t me, const char *cookie,
                     const char *what);
void h_invite(struct req *r, int64_t me, const char *cookie, const char *token);
void h_login_form(struct req *r, const char *err);
void h_login_post(struct req *r);
void h_plot_route(struct req *r, int64_t me);
void h_plots(struct req *r, int64_t owner);
void h_plots_month(struct req *r, int64_t owner, int year, int mon);
void h_data(struct req *r, int64_t owner, int64_t t0, int64_t t1,
            const char *title);

#endif
