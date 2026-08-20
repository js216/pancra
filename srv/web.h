/* SPDX-License-Identifier: GPL-3.0
 * web.h --- the router: one request in, one response built
 * Copyright 2026 Jakob Kastelic
 */
#ifndef PANCRA_WEB_H
#define PANCRA_WEB_H

struct req;

/* Route one parsed request to the page that answers it, render under the
 * page lock, and send the bytes without it (see struct req's `resp`). */
void web_route(struct req *r);

/* 405 for the BROWSER half: an HTML page plus the `Allow` header RFC 9110
 * 9.5.5 requires, built from a mask of http.h's HTTP_M_* bits.
 *
 * Separate from http_method_not_allowed because of WHERE the bytes go. Every
 * response on this half is built into struct req and flushed by web_route
 * after the page lock is released (see the comment on `resp` in proto.h); a
 * refusal that wrote itself down the socket would do it while holding that
 * lock, which is the one thing this half must not do.
 *
 * Declared here rather than kept static in web.c because the handlers that a
 * route can reach with more than one method check the method again themselves
 * -- srv/invite.c serves a GET and REDEEMS on a POST, and "the router would
 * have stopped it" is precisely the assumption that made HEAD a signup. */
void web_method_not_allowed(struct req *r, unsigned allow);

#endif
