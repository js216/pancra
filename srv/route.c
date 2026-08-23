/* SPDX-License-Identifier: GPL-3.0
 * route.c --- the API route grammar, and nothing else
 * Copyright 2026 Jakob Kastelic
 */
#include "route.h"
#include "http.h" /* HTTP_M_*: the bits route_allow's mask is made of */
#include <string.h>

/* "<n>" as a COMPLETE decimal number, and the ONLY spelling of it.
 *
 * TRAILING decoration: strtol stops at the first character it cannot use and
 * reports success on what it did read, which is how "/v1/pair/1junk" became
 * round 1 and "/v1/bucket/g/5x" became bucket 5. The whole field must be
 * consumed, and the field must not be empty.
 *
 * LEADING decoration, which is the half that was missed: strtol also SKIPS
 * WHITESPACE and accepts a sign, so "/v1/pair/ 1", "/v1/pair/+1" and
 * "/v1/pair/%091" were three more spellings of round 1, and "/v1/pair/01" a
 * fourth. All of them arrive, because the target is percent-decoded before
 * it is routed. A route with four spellings is a route whose rate limits and
 * logs count something other than what they name -- so the field is digits
 * and nothing else.
 *
 * WIDTH, which is the half that was missed after that. Every rule above was
 * enforced by inspecting the string, and then the VALUE was still taken from
 * strtol -- which on a number too large for a long returns LONG_MAX, sets
 * errno to ERANGE, and leaves its end pointer on the terminating NUL. So the
 * string checks passed (it is all digits, it does not start with '0', the
 * whole field was consumed), errno was never read, and the caller was handed
 * LONG_MAX as though the path had said it. That is the fourth spelling
 * problem again and unbounded this time: "/v1/bucket/g/9999999999999999999",
 * "/v1/bucket/g/99999999999999999999" and every longer run of nines all named
 * bucket 9223372036854775807, and so did the honest path
 * "/v1/bucket/g/9223372036854775807". Infinitely many spellings of one
 * bucket, each of which the GET side answered 200 with an empty body.
 *
 * So strtol is gone. The value is accumulated here, under a digit cutoff that
 * makes overflow unreachable rather than detectable after the fact (see
 * ROUTE_NUM_DIGITS): no errno to forget to read, no locale, no end pointer,
 * and the OVERFLOW answer is a fact about the string's width rather than a
 * side effect the caller has to go and ask about. */
enum route_num route_number(const char *s, int64_t *out)
{
   if (!s || !*s)
      return ROUTE_NUM_BAD;
   int nd = 0;
   for (const char *p = s; *p; p++) {
      if (*p < '0' || *p > '9')
         return ROUTE_NUM_BAD;
      nd++;
   }
   /* ...and ONE spelling per number, so "01" is not a second name for round
    * 1 and "020000" not a second name for a bucket. "0" itself is a number;
    * a longer field starting with '0' is decoration.
    *
    * BEFORE the width test on purpose: "0000000000000000000001" is not a
    * twenty-two-digit number that overflowed, it is a decorated 1, and saying
    * OVERFLOW about it would describe the wrong fault. */
   if (s[0] == '0' && s[1])
      return ROUTE_NUM_BAD;
   if (nd > ROUTE_NUM_DIGITS)
      return ROUTE_NUM_OVERFLOW;
   /* Cannot overflow: nd <= 18 digits is at most 999999999999999999, and
    * proto.h refuses to compile anywhere `long` is narrower than 64 bits. */
   int64_t v = 0;
   for (const char *p = s; *p; p++)
      v = (v * 10) + (*p - '0');
   *out = v;
   return ROUTE_NUM_OK;
}

/* ---- WHICH METHODS EACH API ROUTE ANSWERS (see route.h) ------------------
 *
 * A switch rather than an array indexed by kind, so adding a route_kind
 * without deciding its methods does not compile: -Wswitch-enum flags the
 * missing case, and there is no default. A table with a hole in it would have
 * answered 0 -- which is a route nobody can reach, discovered in the field. */
unsigned route_allow(enum route_kind kind)
{
   switch (kind) {
      /* POST, and unsigned: pairing is where the signing key comes from, so it
       * is the one route reached without a signature -- which is exactly why
       * its method is as exact as its spelling. */
      case RT_PAIR: return HTTP_M_POST;
      case RT_DIGEST_ALL:
      case RT_DIGEST_LOG: return HTTP_M_GET;
      /* GET reads a bucket, PUT replaces it whole. The only route on this wire
       * with two methods, and the only one that writes. */
      case RT_BUCKET: return HTTP_M_GET | HTTP_M_PUT;
      /* Answered 400 and 404 respectively, on every method. */
      case RT_DIGEST_BAD:
      case RT_BUCKET_BAD:
      case RT_NONE: return 0;
   }
   return 0; /* unreachable: the switch is exhaustive over the enum */
}

void route_of(const char *path, struct route *out)
{
   /* A NULL DESTINATION IS NOT SOMETHING TO CLEAR. This memset ran before
    * anything was checked, so a caller that passed no output got a write
    * through a null pointer -- in the function every request goes through
    * FIRST, before authentication, on a path a stranger chose. Every caller
    * today passes a stack struct; the guard costs one branch and removes the
    * question. A null PATH is already handled below: it is RT_NONE, which is
    * a real answer. */
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   out->kind = RT_NONE;
   if (!path)
      return;

   if (!strncmp(path, "/v1/pair/", 9)) {
      int64_t round = 0;
      /* ONE answer for all three refusals: a pairing path whose round is not
       * one of the four is not a route at all (404), whether the field was
       * decorated, too wide, or a perfectly good 5. Unlike the bucket route
       * below there is no "the shape exists but the request is malformed"
       * middle ground here -- /v1/pair is the one endpoint reached WITHOUT a
       * signature, so anything that is not exactly a round is nothing.
       *
       * The bound comes BEFORE the narrowing cast, and that ordering is the
       * whole of why this cast is safe: `round` is known to be 1..4 by the
       * time it becomes an int. Moving the cast up -- which is what
       * srv/rowdec.c had done to its offset field -- would make the bound
       * guard the narrowed value rather than the parsed one, and 4294967297
       * would be round 1. */
      if (route_number(path + 9, &round) != ROUTE_NUM_OK || round < 1 ||
          round > 4)
         return; /* RT_NONE: there are four rounds and no others */
      out->kind  = RT_PAIR;
      out->round = (int)round;
      return;
   }

   if (!strcmp(path, "/v1/digest")) {
      out->kind = RT_DIGEST_ALL;
      return;
   }

   if (!strncmp(path, "/v1/digest/", 11)) {
      /* A log name is bounded by the wire, and a name that does not fit is
       * not a shorter name: the digest of "glucose" must never be served for
       * a request naming something longer that happens to start the same
       * way. */
      /* A LOG NAME THAT DOES NOT FIT IS NOT A SHORTER NAME: the digest of
       * "glucose" must never be served for a request naming something longer
       * that starts the same way. RT_DIGEST_BAD rather than RT_NONE, so it
       * answers 400 like the bucket route with the same fault -- and like
       * this route did before the grammar was lifted out of the
       * dispatcher. */
      out->kind       = RT_DIGEST_BAD;
      const char *log = path + 11;
      size_t n        = strlen(log);
      if (n == 0 || n > LOGNAME_MAX)
         return;
      memcpy(out->log, log, n);
      out->log[n] = '\0';
      out->kind   = RT_DIGEST_LOG;
      return;
   }

   if (!strncmp(path, "/v1/bucket/", 11)) {
      /* The SHAPE is a route even when the contents are not usable: the
       * dispatcher owes a malformed bucket request a 400, not a 404. */
      out->kind         = RT_BUCKET_BAD;
      const char *p     = path + 11;
      const char *slash = strchr(p, '/');
      if (!slash)
         return;
      size_t n = (size_t)(slash - p);
      if (n == 0 || n > LOGNAME_MAX)
         return;
      int64_t bucket = 0;
      /* TWO REFUSALS, ONE STATUS. Both of these leave RT_BUCKET_BAD standing
       * and the dispatcher answers 400 either way, but they are not the same
       * fault and the code says so: the first is "that is not a number this
       * format carries", the second is "that is a number, and it is not a
       * bucket". Collapsing them into one test would mean the width cutoff
       * and the bucket ceiling could never be told apart, and a test aimed at
       * one of them would silently be satisfied by the other. */
      if (route_number(slash + 1, &bucket) != ROUTE_NUM_OK)
         return;
      if (bucket > ROUTE_BUCKET_MAX)
         return;
      /* No lower bound needed: route_number refuses a sign, so `bucket` is
       * non-negative by construction. srv/logs.c still checks `bucket < 0`
       * because h_bucket_get is a function with a long parameter and not a
       * promise about this parser. */
      memcpy(out->log, p, n);
      out->log[n] = '\0';
      out->bucket = bucket;
      out->kind   = RT_BUCKET;
      return;
   }
}
