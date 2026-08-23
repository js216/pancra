/* SPDX-License-Identifier: GPL-3.0
 * wirehex.h --- the hex and hash the wire is written in, once
 * Copyright 2026 Jakob Kastelic
 *
 * The phone and the server each had their own lower-case hex
 * encoder, their own decoder, their own SHA-256-to-hex and their own
 * truncated protocol hash -- eight functions for four jobs, in a protocol
 * whose whole point is that both sides produce the same bytes.
 *
 * THE COPIES HAD ALREADY DIVERGED, in the way that matters most:
 *
 *   the server's decoder validates the WHOLE string before writing anything,
 *   so a call that fails leaves the caller's buffer untouched;
 *
 *   the phone's wrote each byte as it went and returned 0 at the first bad
 *   character, leaving a PREFIX of the output modified. A caller that treats
 *   its buffer as untouched after a refusal -- which is what a boolean
 *   "did it parse" invites -- then reads half a decoded value and half
 *   whatever was there before.
 *
 * FAILURE-ATOMIC IS THE CONTRACT HERE, for both, because it is the only one
 * that is safe to assume: a caller cannot un-write bytes it did not know were
 * written.
 *
 * EXACT WIDTHS: every one of these takes the number of characters or bytes it
 * is to work on. A NUL-terminated hex string that is SHORTER than the width
 * claimed is a refusal, not a short read -- the check is why lib/wirehex.c
 * looks at each character before it decodes any of them.
 *
 * A LEAF: lib/sha256.h and nothing else. The app half is freestanding, so
 * there is no libc here -- no strchr, no memcpy -- which is also why the
 * nibble lookup is arithmetic rather than a search.
 */
#ifndef PANCRA_WIREHEX_H
#define PANCRA_WIREHEX_H

#include <stddef.h>
#include <stdint.h>

/* `n` bytes as 2n LOWER-CASE hex characters plus a NUL. `out` must hold
 * 2n + 1. Lower case is the protocol's, and both halves compare tags with a
 * byte-for-byte string compare -- upper case on one side is a pairing that
 * never succeeds. */
void wire_hex(const uint8_t *in, size_t n, char *out);

/* `hexchars` characters into `hexchars / 2` bytes. 1 on success.
 *
 * ON FAILURE `out` IS NOT TOUCHED AT ALL -- not a prefix, not one byte. An
 * odd width, a character that is not hex, a string that ends early, or a NULL
 * argument are all refusals. */
int wire_unhex(const char *in, size_t hexchars, uint8_t *out);

/* SHA-256 of `n` bytes as 64 lower-case hex characters plus a NUL. */
void wire_sha256_hex(const void *in, size_t n, char *out65);

/* THE PROTOCOL'S HASH: the first 16 hex characters of the SHA-256, plus a
 * NUL. 64 bits, which is ample for comparing two copies of one honest data
 * set (lib/wirevec.h says so at length) and is what both halves have always
 * written into a digest line. */
void wire_hash16(const void *data, size_t len, char out17[17]);

#endif
