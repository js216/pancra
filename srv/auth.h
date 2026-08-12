/* SPDX-License-Identifier: GPL-3.0
 * auth.h --- who is asking: passwords, sessions, signed app requests
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef AUTH_H
#define AUTH_H

#include "sync.h" /* struct req, and the protocol constants */

/* ---- auth.c ---------------------------------------------------------- */
void pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
             uint8_t out[PW_HASH_LEN]);
long user_by_email(const char *email);
int user_create(const char *email, const char *pw, long *uid);
int user_set_password(long uid, const char *pw);
int user_check_password(long uid, const char *pw);
/* Sessions. `cookie` gets the full "selector:validator" to hand the browser. */
int session_new(long uid, char *cookie, size_t cap);
long session_user(const char *cookie); /* 0 if unknown or expired */
void session_drop(const char *cookie);
void session_drop_all(long uid);
/* The per-session CSRF token is derived from the selector, so it needs no
 * storage and cannot be forged without the cookie it is derived from. */
void csrf_token(const char *cookie, char *out, size_t cap);
int csrf_ok(const char *cookie, const char *sent);
/* Signed app requests; the signing string is built in auth.c and mirrored
 * in app/sync.c. Returns the user id or 0. */
long verify_signature(const struct req *r);
int login_throttled(const char *email);
void login_failed(const char *email);
void login_ok(const char *email);

#endif
