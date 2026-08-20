/* SPDX-License-Identifier: GPL-3.0
 * oops.h --- the one answer for "the database said no"
 * Copyright 2026 Jakob Kastelic
 *
 * A 500 with nothing leaked, and a module of its own because EVERY layer
 * needs it -- the pairing endpoints, the log store, the pages. It lived in
 * page.c beside the page skeleton, so pair.c had to include the web
 * interface's header to answer an error, while page.c asks the pairing
 * module whether a user has a phone: a ring between the two.
 *
 * It is not a page. It is a response, and it says the same nothing however
 * the request arrived.
 */
#ifndef PANCRA_OOPS_H
#define PANCRA_OOPS_H

struct req;

void oops(struct req *r);

/* THE OTHER FAILURE: the request could not be answered RIGHT NOW.
 *
 * 503, not 500. A storage layer that is busy, locked or momentarily
 * unreadable is a transient condition -- the same request a minute later is
 * expected to work -- and a client that cannot tell it from a permanent
 * server fault has no reason to retry rather than give up. It is also what
 * the login path returns when the throttle counter cannot be READ: refusing
 * is the safe answer, and saying WHY it was refused costs nothing.
 *
 * Carries Retry-After, because a 503 without one is a 500 with a nicer
 * number. */
void oops_busy(struct req *r);

#endif
