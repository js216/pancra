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

#include "sync.h"
#include "util.h" /* struct sb: nav and the page builders take one */

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

/* Who is asking, and what they may see. */
void email_of(long uid, char *out, size_t cap);
int may_view(long viewer, long owner);
/* The record being viewed: `who=` when it is shared, else the viewer's own.
 * Returns 0 and answers the request itself when it is not shared. */
long owner_of(struct req *r, long me);
/* The same question EVERY record page must ask, including the default a lone
 * follower relies on -- and it fills r->who so the page's links keep pointing
 * at the record it answered with. `have_own` (may be NULL) reports whether
 * the viewer has a record of their own at all. */
long viewed_owner(struct req *r, long me, int *have_own);

/* Minutes east of UTC to render this user's timestamps in, and a local
 * "YYYY-MM-DD HH:MM" stamp. `have` (may be NULL) says whether anything
 * actually supplied the offset -- 0 because the user is on UTC is not the
 * same answer as 0 because nothing has ever synced. */
int tz_resolve(long uid, int *have);
int tz_of(long uid);
void stamp_local(long t, int tz_min, char *out, size_t cap);

/* The cookie a browser is given, and the check every state-changing form
 * carries. */
void set_cookie_str(const char *cookie, char *out, size_t cap);
int csrf_guard(struct req *r, const char *cookie);

/* The pages themselves. */
void h_home(struct req *r, long me, const char *cookie);
void h_units(struct req *r, long owner);
void h_settings(struct req *r, long me, const char *cookie, const char *note);
/* `what` is the path after "/settings/"; CSRF is the caller's job. */
void h_settings_post(struct req *r, long me, const char *cookie,
                     const char *what);
void h_invite(struct req *r, long me, const char *cookie, const char *token);
void h_login_form(struct req *r, const char *err);
void h_login_post(struct req *r);
void h_plot_route(struct req *r, long me);
void h_plots(struct req *r, long owner);
void h_plots_month(struct req *r, long owner, int year, int mon);
void h_data(struct req *r, long owner, long t0, long t1, const char *title);

#endif
