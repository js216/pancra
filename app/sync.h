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

#include "syncrow.h" /* SYNC_ROW_MAX and what a row IS: the bottom of this */
#include "wirevec.h" /* WV_LIMIT_*: the wire's own numbers, pinned */
#include <stddef.h>
#include <stdint.h>

/* ONE SYNC OPERATION'S OWN STATE, opaque on purpose.
 *
 * sync_run, sync_pair and sync_restore each take one of these: a snapshot of
 * the configuration that cannot change while they run, and ownership of the
 * reallocating workspace they parse logs in. It is declared and never defined
 * here because nothing outside app/sync.c has any business holding one --
 * naming it is how the header says the operations are serialized without
 * offering a way to opt out.
 *
 * What this replaced: transport, identity, log registry and three growing
 * scratch buffers as independent process-wide globals, read field by field by
 * operations that took no context at all. A pairing that succeeded mid-run
 * could hand a run the new uid with the old key; sync_clear_logs plus three
 * sync_add_log calls is a four-store rewrite an operation could walk through;
 * and two operations sharing a reallocating buffer is not a stale read but a
 * freed one. */
struct sync_ctx;

#define SYNC_KEY_LEN  16
#define SYNC_MAX_LOGS 12
/* BUCKETS THIS APP WILL ENUMERATE in one sync, remote and local. Capacities,
 * like the two above: the wire allows LOG_BUCKETS (20000) per log, and these
 * are what one phone will hold in memory while comparing the two sides. Both
 * are ~11 and ~22 years of daily buckets.
 *
 * Named and asserted here rather than left as `sizeof rb / sizeof rb[0]` at
 * five call sites, which is how the two ends came to disagree about what a
 * full log even is with nobody having decided it. Crossing either REFUSES the
 * sync -- an incomplete picture of what the server holds drives a loop that
 * deletes. */
#define SYNC_REMOTE_BUCKETS 4096
#define SYNC_LOCAL_BUCKETS  8192
/* Longest bucket this will build or accept. One UTC day of two CGMs plus a
 * meter is a few tens of kB; the server's own body ceiling is 512 kB. */
#define SYNC_BUF_MAX                                                           \
   (256L * 1024) /* long: it is compared against file offsets */
/* THE MACHINE THIS APP ASSUMES, asserted where the protocol is described.
 *
 * Every id, timestamp and bucket the app signs, writes or parses is a C
 * `long` (%ld on both sides of the wire), and the server asserts the same
 * thing in srv/proto.h. On a 32-bit `long` the code still compiles, the tests
 * still pass, and timestamps stop being representable in 2038 -- so the
 * boundary is declared rather than discovered. The shipped artifact is
 * arm64-v8a alone (LP64); apkcheck.sh refuses a package with any other ABI. */
_Static_assert(sizeof(long) >= 8,
               "LP64 only: an id, an instant and a bucket are all C longs "
               "here, and a 32-bit one is the 2038 problem with no build "
               "error to warn of it");

/* WHAT THE WIRE ALLOWS, AND WHAT THIS PHONE HOLDS, are two different claims
 * and only the first is shared. A row is the wire's row: send a longer one
 * and the server is right to refuse it, so this must MATCH.
 *
 * The buffer and the log count are CAPACITIES -- this app's own -- and they
 * are deliberately far below the wire's ceiling, because it is a phone. They
 * are asserted to be no LARGER than the protocol allows, never equal to it:
 * an implementation may hold less than the wire permits (and must then
 * decline its own request rather than truncate one), but may never send more.
 * See lib/wirevec.h for the rule and srv/proto.h for the server's half. */
_Static_assert(SYNC_BUF_MAX <= WV_LIMIT_BODY_MAX,
               "this app would build a body the server must refuse");
_Static_assert(SYNC_MAX_LOGS <= WV_LIMIT_USER_LOGS,
               "this app would sync more logs than an account may hold");
_Static_assert(SYNC_REMOTE_BUCKETS <= WV_LIMIT_LOG_BUCKETS &&
                   SYNC_LOCAL_BUCKETS <= WV_LIMIT_LOG_BUCKETS,
               "this app would enumerate more buckets than a log may have -- "
               "a capacity ABOVE the wire's limit is not a bigger phone, it "
               "is a buffer nothing on the wire can fill");
_Static_assert(SYNC_KEY_LEN == 16, "the pairing key is 128 bits on this wire");

/* ONE SPELLING OF EACH ROUTE.
 *
 * The bucket path was built with snprintf at four call sites and the digest
 * path at two -- six copies of a format string that is part of the protocol,
 * which is exactly the sort of thing that agrees with itself while drifting
 * from the wire. These build the path or fail; they never truncate one, and a
 * truncated path is a DIFFERENT bucket, not a broken request.
 *
 * Exported because app/test/interoptest.c checks them against the paths the
 * route vectors name: the vectors are only worth anything if the shipping
 * code is what produces those bytes. Returns the length, or 0 if it would
 * not fit. */
int sync_path_bucket(char *out, size_t cap, const char *log, long bucket);
int sync_path_digest(char *out, size_t cap, const char *log);

/* ONE ARBITRARY SIGNED REQUEST, through the app's OWN signing.
 *
 * Exported for app/test/interoptest.c, which drives the route and boundary
 * vectors (lib/wirevec.h) against the real server. A test that built its own
 * Authorization header would be a third implementation of the signing rule,
 * and would agree with itself no matter what the app does; this sends what
 * the phone sends. Returns the HTTP status, or a negative value if the
 * request could not be made (which includes having no key). */
int sync_request(const char *method, const char *path, const char *body,
                 int blen, char *out, int outcap);

/* THE KEY-CONFIRMATION TAG: the first 32 hex characters of
 * HMAC-SHA256(shared key, label), lower case. `out` must hold 33 bytes.
 *
 * The two labels are "pancra-confirm-server" and "pancra-confirm-client", and
 * which one each side sends is the whole content of pairing rounds 3 and 4 --
 * it is what proves the key both sides derived is the SAME key. Truncate
 * differently, upper-case the hex, or swap the labels, and pairing still
 * succeeds against a partner with the same bug and fails against every
 * correct one. lib/wirevec.h pins the tags for one fixed key, and
 * app/test/interoptest.c holds this function to them. */
void sync_confirm_tag(const uint8_t key[SYNC_KEY_LEN], const char *label,
                      char out[33]);

/* THE TWO LABELS, named rather than typed out where they are used, for the
 * same reason as the server's (srv/proto.h): they are protocol constants and
 * a typo in one succeeds against a partner with the same typo. */
#define SYNC_CONFIRM_LABEL_SERVER "pancra-confirm-server"
#define SYNC_CONFIRM_LABEL_CLIENT "pancra-confirm-client"

/* THE EXACT BYTES THIS APP TAKES A REQUEST MAC OVER:
 *
 *    METHOD LF PATH LF TS LF NONCE LF BODYHASH,   no trailing newline.
 *
 * Exported because it was built inside signed_req, where the only way to ask
 * what the app signs was to sign something and watch the wire -- so the
 * vectors in lib/wirevec.h were being checked against a second snprintf in a
 * test rather than against this. A trailing newline could be added to BOTH
 * implementations at once with every gate green, which is the one failure two
 * independent implementations exist to prevent.
 *
 * Returns the length written, or 0 if it would not fit -- and then nothing is
 * signed, rather than a truncated string being. */
int sync_signing_string(char *out, size_t cap, const char *method,
                        const char *path, long ts, const char *nonce,
                        const char *bodyhash);

/* THE NONCE a signed request carries, as 32 hex characters (128 bits) from
 * the OS entropy source. `cap` must be at least 33.
 *
 * 1 on success; 0 when the entropy source could not be read, and then `out`
 * is the empty string and the caller MUST NOT SIGN -- rand_bytes leaves the
 * buffer undefined on failure, and an undefined nonce is stack contents,
 * which can repeat. See sync.c for the uniqueness invariant and for the two
 * ordinary situations (a same-second restart, a clock correction) that the
 * old clock-and-counter nonce could not survive.
 *
 * Exported for the test, which is the only way to assert the invariant. */
int sync_nonce(char *out, int cap);

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

/* Register a file to sync. `bucketed` splits it by UTC day on the row's
 * leading timestamp; 0 puts the whole file in bucket 0. Returns 0 on success.
 */
int sync_add_log(const char *name, const char *path, int bucketed);
void sync_clear_logs(void);

/* Pair with a 6-digit code (EC-J-PAKE over P-256, four steps -- see sync.c).
 * On success the key is stored via sync_set_key and copied to `out_key`.
 * Returns 0 on success, -1 on a wrong code or any protocol failure. */
int sync_pair(const char *email, const char *code,
              uint8_t out_key[SYNC_KEY_LEN], long *out_uid);

/* How far the running sync has got: `done` of `total` buckets examined.
 * Returns 1 while a sync is in flight, 0 otherwise. Written from the sync
 * worker and read by the UI thread; the values are plain ints, so a reader
 * can at worst catch a stale pair, never a torn one. */
int sync_progress(int *done, int *total);

/* Run one sync. Returns 0 when both sides provably match afterwards, -1
 * otherwise. Safe to call repeatedly; a run with nothing to do costs one
 * request. */
int sync_run(void);

/* PULL every bucket the server holds and this phone does not, appending them
 * to the local logs. Returns how many buckets were restored, or -1.
 *
 * The opposite direction from sync_run, and deliberately manual: the phone is
 * authoritative, so an automatic pull would undo real deletions. This exists
 * for the case that authority cannot cover -- a phone whose record is gone
 * (reinstalled, cleared, replaced) facing a server that still has it. It only
 * ever ADDS; nothing local is removed or rewritten.
 *
 * Writes FILES. The caller must reload the logs afterwards for the restored
 * rows to appear in the history, statistics and plot. */
/* CHANGED, BUT DURABILITY UNKNOWN -- distinct from both a count and -1.
 *
 * The restored rows are in the log file and the next load will read them, but
 * a durability step AFTER the rename failed (the file's fsync, the directory's,
 * or close reporting a write the card never took). A power loss now could lose
 * rows the user has been told are back.
 *
 * Neither other answer is honest about that. A count says "your record is
 * restored", which may stop being true at the next reboot; -1 says nothing was
 * restored, which invites the user to run it again over rows that ARE there.
 * Negative so no caller counting rows mistakes it for one, and distinct from
 * -1 so "it failed" and "it worked but may not survive" can be told apart. */
#define SYNC_RESTORE_UNSYNCED (-2)

int sync_restore(void);

/* ---- ONE LINE OF A DIGEST REPLY, exposed for tests --------------------
 *
 * THREE ANSWERS, BECAUSE THERE ARE THREE SITUATIONS. This used to be static in
 * sync.c and to answer 1 for a row and 0 for everything else -- where
 * "everything else" was the end of the reply AND every syntax failure,
 * reported identically. All three callers loop until 0 and then treat what
 * they have as the server's complete list, so a reply truncated by a dropped
 * connection was accepted as the whole truth, and a restore over it reported
 * SUCCESS with a count: the user asked for their record and was told it came
 * back, short.
 *
 * Declared here because no real server can be asked for a deliberately
 * malformed reply, so the only way to hold this to its contract is to call it
 * -- and a parser whose failure mode is "looks finished" is exactly the kind
 * that must be tested rather than argued about. Same reason sync_bucket_text
 * is declared below. */
enum dline {
   DLINE_END = 0, /* the reply ended, cleanly, between lines */
   DLINE_ROW = 1, /* a whole, valid row */
   DLINE_BAD = -1 /* malformed or truncated: the reply is not usable */
};

/* Parse "<name> <count> <hash>\n" at *p, advancing *p past it on DLINE_ROW.
 * Requires: a non-empty name that fits `ncap`, an all-digits count that fits a
 * long, a hash of EXACTLY 16 bytes, and a terminating newline. */
enum dline digest_line(const char **p, char *name, int ncap, long *count,
                       char hash[17]);

/* Canonical text of one bucket, for tests and for the push path: the log's
 * rows for `bucket`, sorted bytewise, each newline-terminated. Returns the
 * length written, or -1 if it does not fit.
 *
 * ALSO NUL-TERMINATED, and one byte of `cap` is reserved for that -- the
 * length is what the push uses, but the buffer is read as a C string too. */
long sync_bucket_text(int log_idx, long bucket, char *out, long cap);

/* Fetch the server's canonical text for one registered bucket through the
 * authenticated production request path. Returns its byte length, or -1. */
long sync_fetch_bucket(int log_idx, long bucket, char *out, long cap);

#endif
