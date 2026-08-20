/* SPDX-License-Identifier: GPL-3.0
 * route.h --- which API route a path IS, and which it is not
 * Copyright 2026 Jakob Kastelic
 *
 * THE GRAMMAR, ON ITS OWN, so it can be tested against the wire vectors.
 *
 * This used to live inside sync.c's dispatcher as a sequence of strncmp
 * prefixes and one strtol, wrapped around a database, a socket and a lock --
 * which meant the only way to ask "is /v1/pair/1junk a route?" was to start a
 * server and send it. So nothing asked, and for a while the answer was yes:
 * strtol reports success on what it managed to read, so three spellings of
 * round 1 reached the one endpoint that is served WITHOUT a signature.
 *
 * A route is the WHOLE path. No prefix of a route is that route, and no
 * decoration of one is either -- leading or trailing: " 1", "+1", "01" and
 * "1junk" are none of them round 1. See lib/wirevec.h for the grammar this
 * implements and srv/test/wiretest.c for the vectors it is checked against.
 *
 * route_of TAKES A PATH AND NOTHING ELSE. The signature is a separate gate
 * that runs before any of these, and choosing the handler is the dispatcher's
 * job: it has the request, this function has a string.
 *
 * WHICH METHODS A ROUTE ANSWERS is nevertheless a fact about the ROUTE, so it
 * is declared here with the routes themselves -- see route_allow below. That
 * used to be three `strcmp(r->method, ...)` tests scattered through the
 * dispatcher's switch, each with its own hand-typed refusal string, plus a
 * fourth in another file entirely (srv/pair.c), so "which methods does
 * /v1/digest take?" had no single place to read the answer. route_allow does
 * not choose a handler and does not look at a request; it answers, per route,
 * the question the dispatcher then enforces.
 */
#ifndef PANCRA_ROUTE_H
#define PANCRA_ROUTE_H

#include "proto.h" /* LOGNAME_MAX: a route carries a log name */

enum route_kind {
   /* Not a route. The dispatcher answers 404, and the ONLY thing that makes
    * a path a route is being spelled exactly like one. */
   RT_NONE = 0,
   RT_PAIR,       /* /v1/pair/<1..4>: `round` is which */
   RT_DIGEST_ALL, /* /v1/digest */
   RT_DIGEST_LOG, /* /v1/digest/<log>: `log` is which */
   /* The per-log digest SHAPE with a log name that is empty or longer than
    * the wire allows. A 400, for the same reason as RT_BUCKET_BAD below. */
   RT_DIGEST_BAD,
   RT_BUCKET, /* /v1/bucket/<log>/<n>: `log` and `bucket` */
   /* The bucket SHAPE with an unusable log or number -- "/v1/bucket/g",
    * "/v1/bucket//5", "/v1/bucket/g/5x". A separate answer from RT_NONE
    * because the dispatcher owes it a 400 (the route exists, the request is
    * malformed) rather than a 404. */
   RT_BUCKET_BAD
};

/* THE HIGHEST BUCKET THIS SERVER CAN BE HOLDING.
 *
 * A bucket is a UTC day index -- `epoch / 86400` as app/sync.c computes it --
 * so the buckets that exist are five digits and will be for another fifty
 * thousand years. The number here is not that; it is the widest bucket a PUT
 * is allowed to CREATE, and srv/logs.c's h_bucket_put has enforced exactly
 * this cap since it was written.
 *
 * IT IS ENFORCED HERE TOO, and that is the point. The GET side never checked
 * it (h_bucket_get bounds `bucket < 0` and nothing above), so the two halves
 * of one route disagreed about what a bucket is: PUT /v1/bucket/g/4294967296
 * was a 400 and GET /v1/bucket/g/4294967296 was a 200 with an empty body --
 * an answer that reads as "that bucket is empty" about a bucket that cannot
 * exist. A number above this cap is not a bucket that happens to hold
 * nothing; it is outside the space a bucket number is drawn from, and the
 * shape-is-a-route rule makes it a 400 like every other malformed bucket
 * request.
 *
 * Not tightened to the five digits a real bucket needs: the wire contract
 * (lib/wirevec.h) declares no bucket ceiling, so narrowing one here would be
 * changing the protocol to suit a range check. This number is the one the
 * writer already refuses to exceed. */
#define ROUTE_BUCKET_MAX 0x7fffffffL

struct route {
   enum route_kind kind;
   int round;                 /* RT_PAIR: 1..4 */
   char log[LOGNAME_MAX + 1]; /* RT_DIGEST_LOG, RT_BUCKET */
   long bucket;               /* RT_BUCKET: 0..ROUTE_BUCKET_MAX */
};

/* ---- THE ONE NUMBER GRAMMAR A PATH MAY CONTAIN ---------------------------
 *
 * THREE ANSWERS, NOT TWO, and OVERFLOW is not the same answer as BAD. A field
 * of eighteen ones and a field of twenty ones are both refused, but they are
 * refused for different reasons and a reader of this code has to be able to
 * tell which rule did it -- otherwise a test that thinks it is pinning the
 * digit-spelling rule ("+1 is not 1") is in fact watching the width cutoff do
 * the work, and loosening the spelling rule breaks nothing that is checked.
 * app/csvcur.h reached the same conclusion for the CSV cursor and spells it
 * the same way: OK, absent, too wide.
 *
 * route_of itself COLLAPSES them, because the wire has one answer for both: a
 * bucket path whose number is unusable is a 400 whatever made it unusable,
 * and a pairing path whose round is unusable is a 404. This distinction is
 * therefore not visible in `struct route` by design -- it is visible HERE, so
 * srv/test/rowtest.c can check each rule against the rule and not against
 * whichever later check happens to catch the same value. */
enum route_num {
   ROUTE_NUM_OK       = 0, /* digits, and the value is all of them */
   ROUTE_NUM_BAD      = 1, /* empty, or decorated: not a number at all */
   ROUTE_NUM_OVERFLOW = 2  /* digits, but wider than a decimal field carries */
};

/* Eighteen digits, the same cutoff as app/csvcur.h's CSV_MAX_DIGITS,
 * app/sync.c's digest_num and srv/rowdec.c's ROW_DIGITS_MAX -- one rule for
 * "how wide may a decimal be" across this repository, because four different
 * answers is how a bound gets loosened in one place and believed everywhere.
 *
 * WHY A DIGIT COUNT AND NOT LONG_MAX: eighteen digits is the widest decimal
 * that provably cannot overflow a 64-bit long, so the accumulation below is
 * safe by construction rather than safe if the platform's long is what we
 * assumed. It does mean LONG_MAX itself -- nineteen digits -- is reported
 * OVERFLOW even though a long holds it exactly. That is deliberate: the limit
 * is a property of the FORMAT, identical on every platform, and a route whose
 * acceptable numbers depend on the width of the compiler's long is a route
 * that answers differently on two machines running the same protocol. */
#define ROUTE_NUM_DIGITS 18

/* Read `s` as a COMPLETE canonical decimal: digits only, at least one, no
 * sign, no leading zero unless the number IS zero, nothing before or after.
 * `*out` is written only on ROUTE_NUM_OK and left alone otherwise.
 *
 * warn_unused_result, for the same reason lib/hkdf.h and lib/pbkdf2.h carry
 * it: a range check whose answer nobody reads is the original defect with a
 * function call in front of it. `*out` is UNTOUCHED on a refusal, so a caller
 * that ignores the status reads whatever it initialised the long to -- which
 * is zero at every call site here, and zero is a real bucket. The compiler
 * refuses that rather than trusting the next person to notice. */
enum route_num route_number(const char *s, long *out)
    __attribute__((warn_unused_result));

/* Classify `path` (already percent-decoded, no query). Never fails: an
 * unrecognised path is RT_NONE. `out` is fully initialised either way. */
void route_of(const char *path, struct route *out);

/* THE METHODS THIS ROUTE ANSWERS, as a mask of http.h's HTTP_M_* bits (include
 * that header to read them; this one deliberately does not, so the grammar
 * still depends on nothing but proto.h).
 *
 * FIXED BY THE APP, NOT CHOSEN HERE. app/sync.c signs POST for a pairing
 * round, GET for a digest, and GET or PUT for a bucket -- and the signature
 * covers the method, so these masks ARE the wire contract. lib/wirevec.h pins
 * them from outside both implementations (vector E: DELETE on a bucket and PUT
 * on a digest are both 405), and app/test/interoptest.c sends every one of
 * those vectors to a running server. Widening a mask here is changing the
 * protocol; it is not a way to make something pass.
 *
 * The two "BAD" kinds and RT_NONE allow nothing: they are answered 400 and 404
 * respectively, whatever the method, because a malformed or nonexistent route
 * has no methods to advertise. */
unsigned route_allow(enum route_kind kind);

#endif
