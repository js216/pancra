// SPDX-License-Identifier: GPL-3.0
// syncrow.h --- What a ROW is, and the byte helpers the protocol is built on
// Copyright 2026 Jakob Kastelic

/* THE BOTTOM OF THE SYNC CLIENT: the handful of pure functions that decide
 * what a row IS, what bucket it belongs to, how rows order against each other
 * and how the protocol's hash and hex are spelled. Nothing here reads a file,
 * makes a request or touches process state -- every one of them is a function
 * of its arguments alone, which is why they can be tested and reasoned about
 * without a server anywhere near them.
 *
 * WHY THEY MOVED OUT OF app/sync.c. Two reasons, and the second is the real
 * one. The file was one line under the 2000-line ceiling `make sizecheck`
 * enforces, so the next behavioural fix in it -- there were three queued --
 * could not be written with the explanation it needed. And the split falls
 * where the module genuinely divides: everything here is about BYTES, while
 * everything left in sync.c is about a file, a socket or a decision. A
 * boundary drawn at "what has no state" is one that stays put.
 *
 * These are the bytes BOTH IMPLEMENTATIONS have to agree on -- srv/logs.c
 * computes the same hash over the same canonical text -- so a change here is
 * a protocol change. lib/wirevec.h pins the answers. */
#ifndef PANCRA_SYNCROW_H
#define PANCRA_SYNCROW_H

#include "wireint.h" /* int64_t and PRIwire: the wire's scalars, exactly */
#include "wirevec.h" /* WV_LIMIT_ROW_MAX: a row is the WIRE's row */
#include <stddef.h>
#include <stdint.h>

/* THE LONGEST LINE THAT IS A ROW, and it is not this app's choice. Send a
 * longer one and the server is right to refuse it, so this must MATCH the
 * wire rather than merely stay under it -- unlike the buffer sizes in sync.h,
 * which are this phone's own capacities and are asserted to be no larger than
 * the protocol allows. See lib/wirevec.h for the rule and srv/proto.h for the
 * server's half.
 *
 * It lived in sync.h, which is why syncrow.c had to include sync.h back and
 * the two modules formed an include cycle `make inclusions` refused. The
 * number belongs to the definition of a row, and that is here. */
#define SYNC_ROW_MAX 512
_Static_assert(SYNC_ROW_MAX == WV_LIMIT_ROW_MAX,
               "a row this app will send is longer than the wire allows");

/* Row index into the log buffer: offset and length, so sorting moves 16 bytes
 * rather than a line. Unsigned because neither an offset into a buffer nor
 * the length of a line can be negative -- and while that is obvious to a
 * reader, a signed pair lets `buf + off` and `text + n` be provably out of
 * bounds to anything checking, which they then duly report. */
struct row {
   size_t off;
   size_t len;
};

/* strlen/strchr are not in the freestanding shim, and adding them there would
 * mean adding symbols to stub_c.c for two one-line loops. */
int64_t s_len(const char *p);
const char *s_chr(const char *p, char c);

/* `n` bytes as 2n lowercase hex characters plus a terminator. */
void hexify(const uint8_t *in, int n, char *out);
/* `hexchars` hex characters back into bytes. 0 if any of them is not hex --
 * which is a refusal, not a zero byte. */
int unhex(const char *in, int hexchars, uint8_t *out);
/* The protocol's hash: the first 16 hex characters of SHA-256, terminated. */
void hash16(const char *data, int64_t len, char out[17]);

/* Order two rows by their bytes, then by length: the total order the
 * canonical form of a bucket is sorted into. */
void row_sort(const char *base, struct row *r, int n);
/* Is this line a row at all? See the definition -- the answer is a protocol
 * question, not a tidiness one. */
int row_ok(const char *p, int len);
/* Which UTC day a row belongs to, from its leading decimal field. */
int64_t row_bucket(const char *p, size_t len, int bucketed);
/* Is bucket `b` in the list of `nwant` buckets? */
int want_has(const int64_t *want, int nwant, int64_t b);

#endif
