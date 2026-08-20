/* SPDX-License-Identifier: GPL-3.0
 * logs.h --- the replica: digests and whole-bucket replacement
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef LOGS_H
#define LOGS_H

#include "proto.h" /* struct req, and the protocol constants */

/* ---- logs.c ---------------------------------------------------------- */
int log_name_ok(const char *s);
void h_digest(struct req *r);
void h_digest_log(struct req *r, const char *log);
void h_bucket_get(struct req *r, const char *log, long bucket);
void h_bucket_put(struct req *r, const char *log, long bucket);

#endif
