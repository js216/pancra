// SPDX-License-Identifier: GPL-3.0
// sync.h --- Cloud sync client: digests, buckets, pairing (API)
// Copyright 2026 Jakob Kastelic

/* The phone is AUTHORITATIVE and the server is a replica of it. After a
 * successful run both sides hold exactly the same rows, and one small request
 * proves it.
 *
 * The record is a set of named LOGS. A log is a set of ROWS; a row is one
 * line of text and ITS BYTES ARE ITS IDENTITY -- nothing here reformats,
 * reorders or normalises a row, because the server hashes exactly the bytes
 * it is given and the two hashes have to agree. Rows are partitioned into
 * BUCKETS of one UTC day, taken from the row's leading timestamp field; a log
 * that is really one small file uses the single bucket 0.
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
#include "wireint.h" /* int64_t and PRIwire: the wire's scalars, exactly */
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
 * WHAT A CONTEXT PREVENTS: transport, identity, log registry and three
 * growing scratch buffers as independent process-wide globals, read field by
 * field by operations that take no context at all. A pairing that succeeds
 * mid-run hands a run one account's uid with another's key; a clear plus
 * three adds is a four-store rewrite an operation can walk through; and two
 * operations sharing a reallocating buffer is not a stale read but a freed
 * one. */
struct sync_ctx;

#define SYNC_KEY_LEN  16
#define SYNC_MAX_LOGS 12
/* BUCKETS THIS APP WILL ENUMERATE in one sync, remote and local: THE WIRE'S
 * OWN LIMIT, and not a smaller number of somebody's choosing.
 *
 * They were 4096 and 8192 -- about eleven and twenty-two years of daily
 * buckets -- and crossing either REFUSES the sync, because an incomplete
 * picture of what the server holds drives a loop that deletes. That refusal
 * is right and it stays; what was wrong was where the line sat. A log the
 * protocol still accepts (LOG_BUCKETS is 20000, and the server enforces it)
 * would have been refused BY THE PHONE, for ever, with the only remedy being
 * to delete history the user asked to keep. A limit that turns age into a
 * permanent failure has to be the protocol's, so that both ends agree about
 * what a full log is.
 *
 * THE MEMORY IS NOT PAID UP FRONT. At the wire limit these lists are about a
 * megabyte between them, which is not something to reserve in BSS on a phone
 * for an operation most users run a few times a day. They are allocated on
 * first use and kept (see sync_scratch in app/sync.c); an allocation that
 * fails refuses the sync exactly as a fixed ceiling would. */
#define SYNC_REMOTE_BUCKETS WV_LIMIT_LOG_BUCKETS
#define SYNC_LOCAL_BUCKETS  WV_LIMIT_LOG_BUCKETS
/* Longest bucket this will build or accept. One UTC day of two CGMs plus a
 * meter is a few tens of kB; the server's own body ceiling is 512 kB. */
#define SYNC_BUF_MAX                                                           \
   (256L * 1024) /* compared against int64_t buffer lengths                    \
                  */
/* THE MACHINE THIS APP ASSUMES: NONE, and that is the change.
 *
 * Every id, timestamp and bucket the app signs, writes or parses is an
 * int64_t, printed and parsed with PRIwire/SCNwire (lib/wireint.h), which is
 * the same 64 bits on every data model. A C `long` printed with %ld would
 * make each of them a property of the MACHINE, and none of these values
 * belongs to a machine: they are decimal text on a wire between two of them,
 * and the wire says nothing about either. An assertion that sizeof(long) >= 8
 * is an honest guard on a dishonest design.
 *
 * THE FILE AND THE WIRE ARE UNAFFECTED: decimal digits either way, so a phone
 * parses its own stored rows and the server's answers identically. What the
 * types buy is that a 32-bit host is not a silent 2038 bug. `make -f
 * test/Makefile
 * wirecheck` compiles these units for ILP32 to prove it, and that compile is
 * the only thing that can SEE a leftover %ld -- on LP64, %ld and PRIwire are
 * the same three characters.
 *
 * The shipped artifact is still arm64-v8a alone (apkcheck.sh refuses a
 * package with any other ABI). That is a packaging fact now, not a
 * correctness one. */

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
 * Exported because test/app/interoptest.c checks them against the paths the
 * route vectors name: the vectors are only worth anything if the shipping
 * code is what produces those bytes. Returns the length, or 0 if it would
 * not fit. */
int sync_path_bucket(char *out, size_t cap, const char *log, int64_t bucket);
int sync_path_digest(char *out, size_t cap, const char *log);

/* THE KEY-CONFIRMATION TAG: the first 32 hex characters of
 * HMAC-SHA256(shared key, label), lower case. `out` must hold 33 bytes.
 *
 * The two labels are "pancra-confirm-server" and "pancra-confirm-client", and
 * which one each side sends is the whole content of pairing rounds 3 and 4 --
 * it is what proves the key both sides derived is the SAME key. Truncate
 * differently, upper-case the hex, or swap the labels, and pairing still
 * succeeds against a partner with the same bug and fails against every
 * correct one.
 *
 * THE CONSTRUCTION AND THE LABELS ARE lib/pairtag.h's NOW: this
 * side and the server's had a copy each, four places for one twelve-line
 * rule. What is still independent -- deliberately -- is the VECTORS:
 * lib/wirevec.h pins the tags for one fixed key, and test/app/interoptest.c
 * holds the app's production path to them while test/srv/wiretest.c holds the
 * server's. */

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
                        const char *path, int64_t ts, const char *nonce,
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
void sync_set_key(int64_t uid, const uint8_t key[SYNC_KEY_LEN]);

/* ONE FILE THE CALLER WANTS SYNCED. `bucketed` splits it by UTC day on the
 * row's leading timestamp; 0 puts the whole file in bucket 0. */
struct sync_log_spec {
   const char *name;
   const char *path;
   int bucketed;
};

/* PUBLISH THE WHOLE REGISTRY, IN ONE STORE.
 *
 * Returns 0 when `n` entries were published, -1 when NONE were: too many for
 * SYNC_MAX_LOGS, or a name or path that does not fit its field. There is no
 * partial outcome, and that is the point of the shape.
 *
 * WHY THERE IS NO add() AND NO clear(), which is the whole shape of this
 * call. A clear followed by eight adds, each taking the configuration lock
 * on its own, lets a sync starting anywhere inside the sequence snapshot a
 * PREFIX of the registry, upload that, and report success. A sync that sends
 * five of eight logs and says it is done is worse than one that fails: the
 * digests agree, both sides believe they match, and nothing ever revisits it.
 * The sequence was a clear followed by eight adds, each taking the
 * configuration lock on its own.
 *
 * A caller cannot express that any more. The list is built and validated
 * off-lock, and one store swaps `{logs, nlog}` together, so an operation's
 * snapshot is always a registry SOMEBODY REGISTERED, never a prefix of
 * one. */
int sync_set_logs(const struct sync_log_spec *specs, int n);

/* Pair with a 6-digit code (EC-J-PAKE over P-256, four steps -- see sync.c).
 * On success the key is stored via sync_set_key and copied to `out_key`.
 * Returns 0 on success, -1 on a wrong code or any protocol failure. */
int sync_pair(const char *email, const char *code,
              uint8_t out_key[SYNC_KEY_LEN], int64_t *out_uid);

/* ---- HOW FAR THE RUNNING SYNC HAS GOT -----------------------
 *
 * ONE STATE, NOT THREE VALUES. Three atomics read one at a time promise "at
 * worst a stale pair, never a torn one" about ONE pair, and there is more
 * than one: a run can END and another BEGIN between two of those loads, so a
 * reader pairs one run's `done` with the next run's `total` and draws 900 of
 * 12. Everything is published in
 * one 64-bit word now (see sync.c for the layout and why there is no lock).
 *
 * THE CONTRACT: what comes back existed -- one run's flag, its total and a
 * `done` that belonged to it -- and may be a few buckets old. Fields from two
 * different runs cannot appear together. `gen` changes when a new run starts,
 * so two reads carrying the same `gen` describe the same run; it wraps, and
 * is for telling runs apart rather than counting them.
 *
 * `total` and `done` are clamped at 2^24-1 rather than wrapped: a wrapped
 * denominator is a plausible-looking wrong number. */
struct sync_prog {
   int active;   /* 1 while a run is in flight */
   int done;     /* buckets examined so far */
   int total;    /* buckets this run set out to examine */
   unsigned gen; /* which run these belong to */
};


/* The same state for a caller that only wants the two numbers. Kept because
 * every existing caller reads exactly this; it is one read of the word, so
 * the pair it returns is coherent for the same reason. */
struct sync_prog sync_progress_get(void);

/* Bracket one run: begin names how many buckets it set out to examine, end
 * marks it finished without disturbing the counts. One writer only -- the
 * sync worker -- which is why neither needs a compare-exchange. */
void sync_progress_begin(int total);
void sync_progress_end(void);

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
 * THREE ANSWERS, BECAUSE THERE ARE THREE SITUATIONS. Answering 1 for a row
 * and 0 for everything else makes "everything else" the end of the reply AND
 * every syntax failure, reported identically. All three callers loop until 0
 * and then treat what they have as the server's complete list, so a reply
 * truncated by a dropped connection is accepted as the whole truth, and a
 * restore over it reports
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
enum dline digest_line(const char **p, char *name, int ncap, int64_t *count,
                       char hash[17]);

/* The canonical text of one bucket of `log_idx` -- its rows, sorted,
 * de-duplicated, newline-terminated -- which is the text whose hash the two
 * sides of a sync compare. NUL-terminated; returns the length written, or -1
 * if the bucket does not exist or does not fit `cap`.
 *
 * Declared here for the same reason digest_line above is: the agreement it
 * encodes is with a REMOTE implementation, and the only way to hold it to
 * that is to render a known log and look at the bytes. */
int64_t sync_bucket_text(int log_idx, int64_t bucket, char *out, int64_t cap);

#endif
