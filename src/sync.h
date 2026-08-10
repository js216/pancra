// SPDX-License-Identifier: GPL-3.0
// sync.h --- Cloud sync client: digests, buckets, pairing (API)
// Copyright 2026 Jakob Kastelic

/* The phone is AUTHORITATIVE and the server is a replica of it. After a
 * successful run both sides hold exactly the same rows, and one small request
 * proves it.
 *
 * The record is a set of named LOGS. A log is a set of ROWS; a row is one
 * line of text and ITS BYTES ARE ITS IDENTITY -- nothing here reformats,
 * reorders or normalises a row, because the server hashes exactly what it was
 * given and the two hashes have to agree. Rows are partitioned into BUCKETS
 * of one UTC day, taken from the row's leading timestamp field; a log that is
 * really one small file uses the single bucket 0.
 *
 * Sync compares two levels of hash and pushes only what differs:
 *
 *     GET /v1/digest                  -> "<log> <rows> <hash>" per log
 *     GET /v1/digest/<log>            -> "<bucket> <rows> <hash>" per bucket
 *     PUT /v1/bucket/<log>/<bucket>   -> body REPLACES that bucket
 *
 * so a corrected dose, a deleted one and a reading backfilled a week late all
 * reach the server the same way: "this bucket now contains exactly these
 * lines". There is no notion of editing or deleting a row, and first pairing
 * is not a special case -- the server's digest is simply empty.
 *
 * WHAT MUST NEVER BE SYNCED: the pairing code and the derived key (see
 * g_code_path / g_remote_path in settings.h). Uploading the credential that
 * authenticates us TO the server would make a leak of the server's database
 * an impersonation kit.
 */
#ifndef PANCRA_SYNC_H
#define PANCRA_SYNC_H

#include <stddef.h>
#include <stdint.h>

#define SYNC_KEY_LEN  16
#define SYNC_MAX_LOGS 12
/* Longest bucket this will build or accept. One UTC day of two CGMs plus a
 * meter is a few tens of kB; the server's own body ceiling is 512 kB. */
#define SYNC_BUF_MAX  (256 * 1024)
#define SYNC_ROW_MAX  512

/* The transport, supplied by the caller: Java's HttpsURLConnection on the
 * phone, plain sockets in the host test. `hdr` is extra request headers
 * (already CRLF-terminated) or NULL. Returns the HTTP status code, or a
 * negative value if the request could not be made at all; the response body
 * is copied into `out`. Kept as a hook because TLS on Android is free from
 * the platform and would cost a megabyte of library in C. */
typedef int (*sync_http_fn)(const char *method, const char *path,
                            const char *hdr, const char *body, int blen,
                            char *out, int outcap);

void sync_set_http(sync_http_fn fn);

/* The paired identity. `uid` 0 means "not paired" and every call fails. */
void sync_set_key(long uid, const uint8_t key[SYNC_KEY_LEN]);
long sync_uid(void);

/* Register a file to sync. `bucketed` splits it by UTC day on the row's
 * leading timestamp; 0 puts the whole file in bucket 0. Returns 0 on success.
 */
int sync_add_log(const char *name, const char *path, int bucketed);
void sync_clear_logs(void);

/* Pair with a 6-digit code (EC-J-PAKE over P-256, four steps -- see sync.c).
 * On success the key is stored via sync_set_key and copied to `out_key`.
 * Returns 0 on success, -1 on a wrong code or any protocol failure. */
int sync_pair(const char *email, const char *code, uint8_t out_key[SYNC_KEY_LEN],
              long *out_uid);

/* How far the running sync has got: `done` of `total` buckets examined.
 * Returns 1 while a sync is in flight, 0 otherwise. Written from the sync
 * worker and read by the UI thread; the values are plain ints, so a reader
 * can at worst catch a stale pair, never a torn one. */
int sync_progress(int *done, int *total);

/* Run one sync. Returns 0 when both sides provably match afterwards, -1
 * otherwise. Safe to call repeatedly; a run with nothing to do costs one
 * request. */
int sync_run(void);

/* Canonical text of one bucket, for tests and for the push path: the log's
 * rows for `bucket`, sorted bytewise, each newline-terminated. Returns the
 * length written, or -1 if it does not fit. */
long sync_bucket_text(int log_idx, long bucket, char *out, long cap);

#endif
