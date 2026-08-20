/* SPDX-License-Identifier: GPL-3.0
 * util.h --- response buffers, hex, randomness, header and form parsing
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef UTIL_H
#define UTIL_H

#include "proto.h" /* struct req, and the protocol constants */

/* ---- util.c ---------------------------------------------------------- */
/* A growable response buffer. Digests and pages are built whole before they
 * are sent because http_respond needs a Content-Length, and a bucket cannot
 * exceed the PUT body cap that admitted it, so the sizes stay bounded. */
struct sb {
   char *p;
   size_t n, cap;
   int err; /* sticky: set on any allocation failure, checked once at the end */
};

/* printf-annotated: sb_add takes a format and a variable argument list, and
 * without telling the compiler so, -Wformat=2 cannot see these calls at all.
 * A page here passes fourteen arguments to one format string; dropping a
 * group of three silently made vsnprintf read past the end of the list and
 * take the server down with it. The attribute turns that into a build
 * error. */
int sb_add(struct sb *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int sb_raw(struct sb *s, const void *data, size_t n);
void sb_free(struct sb *s);

/* ---- THE CAPACITY THIS BUFFER WOULD NEED, AND WHY IT IS A FUNCTION ------
 *
 * WHAT THE ARITHMETIC USED TO DO. sb_room asked `if (s->n + need + 1 <=
 * s->cap) return 1;` and then grew with `while (cap < s->n + need + 1) cap *=
 * 2;`. Both expressions are size_t, both wrap, and neither was checked. Two
 * things followed, and both were measured against the code as it stood rather
 * than reasoned about:
 *
 *   * n + need + 1 WRAPS TO SOMETHING SMALL and the test passes. `need` of
 *     SIZE_MAX makes the sum zero, and zero is <= any capacity, so a builder
 *     with no allocation at all reported room and sb_raw ran
 *     memcpy(NULL, data, SIZE_MAX). More generally any need in
 *     [SIZE_MAX - n - cap, SIZE_MAX] approves an undersized buffer and the
 *     following memcpy writes past it. `err` was NOT set on that path, so the
 *     one thing every caller in this program checks said the page was fine.
 *     This is a heap overflow in the request path, and sb_* is what builds
 *     every HTML page this server serves -- a share list, a log page, an
 *     address on the settings page.
 *
 *   * THE DOUBLING LOOP NEVER ENDS. `cap` walks 4096, 8192, ... 2^63, and the
 *     next double is 0. Zero doubled is zero, so for any target above 2^63 the
 *     loop spins forever: a worker thread that never returns, holding the page
 *     mutex, with every other page for every other user behind it.
 *
 * WHY IT IS EXPOSED. The interesting boundaries are SIZE_MAX-adjacent, and
 * SIZE_MAX bytes cannot be allocated to drive sb_raw at them -- the same
 * problem lib/gcm.c has with aes128_gcm_limits, solved the same way. This is
 * a pure function of four numbers: the test calls it with SIZE_MAX, with
 * SIZE_MAX - 1, with 2^63 and with 2^63 + 1 and asks what it says, and drives
 * the real sb_raw only at the sizes a machine can actually hold.
 *
 * Returns 1 and writes to *out the capacity a buffer holding `n` bytes at
 * capacity `cap` must have to accept `need` more (plus the terminating NUL);
 * *out may equal `cap`, meaning no growth is needed. Returns 0, with *out set
 * to 0, when n + need + 1 is not a representable size_t -- there is no such
 * buffer, so there is no capacity to name. *out is written FIRST in every
 * case. */
#define SB_MIN_CAP 4096 /* the first allocation, unchanged: one page */

int sb_cap_for(size_t n, size_t cap, size_t need, size_t *out)
    __attribute__((warn_unused_result));

void rnd_bytes(uint8_t *out, size_t n);
void rnd_hex(char *out, size_t hexlen); /* hexlen chars + NUL */
void hex_of(const uint8_t *in, size_t n, char *out);
int hex_to(const char *in, size_t hexchars, uint8_t *out);
/* ct_eq moved to lib/ct.h: it is a crypto primitive, and srv/tls.c needs it
 * without being able to include this header. */
void sha256_hex(const void *in, size_t n, char *out64);

/* Percent-decode in place; returns the new length. */
size_t url_decode(char *s);
/* Escape `in` into `out` with &<>"' escaped; never overflows `cap`. */
void html_esc(char *out, size_t cap, const char *in);

/* ---- WHAT A FORM FIELD IS, AND THE FOUR WAYS IT IS NOT ONE --------------
 *
 * The decoder this replaces answered a yes/no question -- "was there a field
 * of this name?" -- and could not say no to anything else. Four behaviours
 * hid behind that one bit, and every one of them is reachable from an
 * unsigned, unauthenticated POST:
 *
 *   %00 DECODED TO AN EMBEDDED NUL. url_decode turned "%00" into a real NUL
 *      byte in the middle of the caller's buffer, and every caller then used
 *      the value as a C string. So `csrf=<goodtoken>%00junk` reached
 *      csrf_ok() as exactly <goodtoken> -- the token compare is strcmp -- and
 *      passed, while any length-aware reader (a proxy, a log, a WAF, a
 *      duplicate-detector) sees a different, longer value. The same trick
 *      applies to `email`, where the row that is looked up and the row that
 *      is written can be told apart by anything counting bytes.
 *
 *   IT CLIPPED TO THE CALLER'S BUFFER AND SAID NOTHING. `if (vlen >= cap)
 *      vlen = cap - 1;` -- a value one byte too long silently became a
 *      DIFFERENT value, and the caller was told it was the field the client
 *      sent. Nothing in the caller could tell "the user typed this" from
 *      "the user typed this and 900 bytes more". (Item 47 established by
 *      execution that today's truncation cannot produce a value that
 *      VALIDATES, because it truncates the still-ENCODED prefix and a cut
 *      "%"-escape then decodes to literal characters. That is a property of
 *      where the cut lands, not a rule anybody wrote; it held for `tz`, and
 *      it is not a defence for a field with a different validator. The point
 *      of FORM_TOO_LONG is that it is now a rule.)
 *
 *   IT KEPT INVALID ESCAPES AS TEXT. "%zz" decoded to the three characters
 *      "%zz", and "%4" at the end of a value to "%4". A percent that is not
 *      an escape is not something a client can have meant, and it is the
 *      other half of the disagreement above: a front end that normalises or
 *      rejects it does not see the value this server acted on.
 *
 *   IT HID DUPLICATES. It returned the FIRST match and stopped scanning, so
 *      "csrf=<good>&csrf=<bad>" had two answers and this server picked one
 *      without saying so. Whether a proxy, a log or a WAF picks the same one
 *      is not something either end can know -- and there is no reading of a
 *      repeated security field that is safe to guess at.
 *
 * So the answer is typed. FORM_ABSENT stays 0 so that "no field" remains the
 * falsy answer, but the FUNCTION was renamed on purpose: every one of the
 * thirteen call sites had to be reopened by the compiler, because a call
 * site testing the old name for truth would have silently kept compiling and
 * would have read MALFORMED, TOO_LONG and DUPLICATE as "present, carry on".
 *
 * `out` is always NUL-terminated, and is set to "" for every answer but
 * FORM_OK -- a caller that ignores the answer gets an empty value, not a
 * damaged one. */
enum form_field {
   FORM_ABSENT = 0, /* no pair of this name in the body */
   FORM_OK,         /* exactly one, decoded, fits, no NUL: usable */
   FORM_MALFORMED,  /* an invalid percent escape, or a decoded NUL */
   FORM_TOO_LONG,   /* the decoded value does not fit `cap`; NOT truncated */
   FORM_DUPLICATE,  /* the name appears more than once: no single answer */
};

enum form_field form_field(const char *body, size_t len, const char *name,
                           char *out, size_t cap)
    __attribute__((warn_unused_result));

/* THE WHOLE BODY, JUDGED BEFORE ANYBODY IS AUTHENTICATED.
 *
 * form_field answers about ONE name, which means a malformed or duplicated
 * field is only noticed if some handler happens to ask for it -- and the
 * handler that asks runs after the session lookup, after the route, and in
 * some cases after a row has been written. Item 120 asks for these refused
 * "before authentication or mutation", so the browser half runs this over the
 * entire body first (see web_route_locked): one pass, no names, purely a
 * question about whether the document is a well-formed
 * application/x-www-form-urlencoded one at all.
 *
 * There is no legitimate repeated name on this server -- csrf, action, email,
 * password, old, new, confirm, tz, token, who, and not a checkbox or a
 * multi-select among them -- so a repeat is refused rather than resolved. */
enum form_body {
   FORM_BODY_OK = 0,
   FORM_BODY_MALFORMED, /* a bad escape, or a NUL, anywhere in it */
   FORM_BODY_DUPLICATE, /* some name appears more than once */
};

enum form_body form_body_check(const char *body, size_t len)
    __attribute__((warn_unused_result));
/* One HTTP header value from the header block. 1 if found. */
int hdr_get(const char *hdr, const char *name, char *out, size_t cap);

/* ---- ONE CANONICAL EMAIL, ON EVERY SURFACE THAT TAKES ONE ---------------
 *
 * WHAT WENT WRONG WITHOUT IT. The login form refused an address longer than
 * 254 bytes before it touched storage. Nothing else did. The invitation form
 * -- the same two fields, the same throttle, the same account table, and the
 * ONLY path in this program that CREATES an account -- read the field into a
 * kilobyte of stack and went straight to the throttle lookup, the account
 * lookup, the failure record and, for an address with no account, user_create.
 * `sync adduser` took whatever was on its command line. So:
 *
 *   - a 900-byte address could be written into `login_fail` on every request,
 *     a row keyed on a string the login form can never produce again, so the
 *     throttle it belongs to will never match it and never count it down. It
 *     is simply storage, gained a row at a time, on a board whose disk is a
 *     memory card. (login_failed's sweep bounds it in TIME; nothing bounded
 *     what a single request could write.)
 *
 *   - an ACCOUNT could be created under an address longer than the login form
 *     will accept. Its owner can sign in through the invitation link that made
 *     it, once, and never again through /login: the page refuses the address
 *     before it looks anything up. There is no password reset on this server.
 *
 * AND THE OTHER HALF, WHICH IS NOT ABOUT LENGTH. `user.email` is
 * `COLLATE NOCASE` (srv/db.c), so the account lookup is case-insensitive.
 * `login_fail.email` is a TEXT PRIMARY KEY with no collation, so it is BINARY
 * and the throttle is case-SENSITIVE. One account therefore has as many
 * throttle rows as there are spellings of its address: five failures as
 * "bob@x", five as "Bob@x", five as "BOB@x" -- the counter never reaches
 * LOGIN_FAIL_MAX, and the PBKDF2 protection that stands between the login form
 * and an offline-speed password search never fires. Canonicalising to ASCII
 * lower case makes the throttle key agree with the collation the account
 * lookup already uses.
 *
 * email_canon writes the canonical form of `in` into `out` and returns 1, or
 * returns 0 with `out` set to "" when `in` is not an address this server will
 * act on. CALL IT FIRST, BEFORE THE THROTTLE LOOKUP, and use its output for
 * everything after -- the throttle, the account lookup, the failure record and
 * the creation must all be about the same string, or they are about different
 * accounts.
 *
 * `cap` must be at least EMAIL_BUF. See the definition for what is accepted:
 * the summary is 3..254 bytes after trimming the edges, exactly one '@' with
 * something either side, no byte at or below 0x20 and no 0x7f, and A-Z folded
 * to a-z (and nothing else folded, because NOCASE folds nothing else either).
 */
#define EMAIL_MAX 254 /* RFC 5321 forward path, brackets removed */
#define EMAIL_BUF (EMAIL_MAX + 1)

int email_canon(const char *in, char *out, size_t cap);

/* ---- THE ONE NAME THIS SERVER IS KNOWN BY FROM OUTSIDE -----------------
 *
 * THE REQUEST MUST NOT CHOOSE IT. The settings page built share links out of
 * the request's own `Host` header -- HTML-escaped, so not an injection, but
 * treated as authoritative. A share link carries a LIVE SINGLE-USE TOKEN, and
 * an authenticated request arriving with `Host: evil.example` through a
 * permissive proxy (or simply a second name pointed at this address) therefore
 * rendered `https://evil.example/invite/<token>` on the owner's own settings
 * page. The owner copies it, sends it to the person they meant to invite, and
 * the token is delivered to somebody else's domain. The comment beside that
 * code argued the server "has no other way to know what it is called from
 * outside" -- which is true, and is why it has to be TOLD rather than asked.
 *
 * So: one configured origin, from the environment (PANCRA_ORIGIN) with a
 * compiled default, validated to a host charset. Never a request value.
 * PANCRA_ORIGIN is the deployment's to set; srv/deploy/pancra.conf carries it
 * beside PANCRA_URL, which is the same fact written for the health check.
 *
 * Returns a host[:port] with no scheme and no trailing slash. Never NULL, never
 * empty: a misconfigured origin falls back to the compiled default rather than
 * producing a link with a hole in it.
 */
const char *public_origin(void);

/* 1 when `s` is a plausible host[:port] -- letters, digits, dot, hyphen, and at
 * most one colon before a numeric port. Exposed for the test: the whole value
 * of the function above is that a bad value cannot get in, and that is a claim
 * worth executing rather than reading. */
int origin_ok(const char *s);

#endif
