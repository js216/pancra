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

#include "sync.h" /* struct req, and the protocol constants */

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
/* One field of an application/x-www-form-urlencoded body. 1 if found. */
int form_get(const char *body, size_t len, const char *name, char *out,
             size_t cap);
/* One HTTP header value from the header block. 1 if found. */
int hdr_get(const char *hdr, const char *name, char *out, size_t cap);

#endif
