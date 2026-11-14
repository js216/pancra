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
void h_bucket_get(struct req *r, const char *log, int64_t bucket);
void h_bucket_put(struct req *r, const char *log, int64_t bucket);

/* A COUNT OF COMMITTED BUCKET WRITES, any account, any log, since the process
 * started. Unchanged means no row has changed, so anything derived from the
 * rows and tagged with this number is still current. */
unsigned long logs_gen(void);

/* THE SAME, COUNTING ONLY WRITES TO AN OLD BUCKET: one more than
 * LOGS_RECENT_DAYS days before the day of the write. A bucket is a day of
 * rows, and a phone rewrites today's and yesterday's as readings arrive, so
 * this moves only for a backfill or an import -- and anything derived from
 * the old buckets alone stays current for as long as it does not. */
#define LOGS_RECENT_DAYS 2
unsigned long logs_old_gen(void);

#endif
