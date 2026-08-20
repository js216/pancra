/* SPDX-License-Identifier: GPL-3.0
 * pbkdf2.h --- PBKDF2-HMAC-SHA256 (RFC 8018)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef PBKDF2_H
#define PBKDF2_H
#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 is the PRF, so hLen is 32 in RFC 8018's formulas. */
#define PBKDF2_HASH_LEN 32

/* THE SALT CAPACITY. RFC 8018 does not bound the salt; this implementation
 * does, because it builds `salt || INT(i)` into one stack block and hands it
 * to the one-shot hmac_sha256(). 252 + 4 keeps that block at the 256 bytes it
 * has always been. What changes is the answer to a longer salt.
 *
 * WHAT THE OLD CODE DID: `size_t k = saltn > sizeof si - 4 ? sizeof si - 4 :
 * saltn;` -- it TRUNCATED, and the API returned void, so nobody could be
 * told. Two accounts salted with 300-byte values that shared their first 252
 * bytes hashed identically; a salt lengthened from 252 to 260 kept deriving
 * the old key while the stored salt said otherwise. Both are unreproducible
 * from the arguments at the call site, which is the whole problem with a KDF
 * that edits its inputs. */
#define PBKDF2_SALT_MAX 252

/* THE BLOCK CEILING. RFC 8018 5.2 step 1:
 *
 *     If dkLen > (2^32 - 1) * hLen, output "derived key too long" and stop.
 *
 * because the block index goes into the PRF as a 32-bit INT(i): there is no
 * 2^32-th block to ask for. The old `uint32_t block` would have wrapped back
 * to 0 and started repeating the derived material. Unreachable in this
 * program -- the ask would be 128 GiB of output -- but it is the RFC's limit
 * rather than this platform's, so it is written as the RFC's limit. */
#define PBKDF2_BLOCK_MAX 0xFFFFFFFFu

/* Typed, because "it refused" is not actionable: an oversize salt is a
 * storage bug, a zero iteration count is a configuration bug, and a zero
 * dkLen is a caller bug. */
enum pbkdf2_status {
   PBKDF2_OK = 0,
   PBKDF2_ERR_ARG,   /* a NULL where bytes were promised */
   PBKDF2_ERR_SALT,  /* saltn > PBKDF2_SALT_MAX */
   PBKDF2_ERR_ITERS, /* iters == 0; RFC 8018 5.2 makes c a positive integer */
   PBKDF2_ERR_DKLEN  /* n == 0, or above the block ceiling above */
};

/* PBKDF2_OK, or a status with `out` LEFT EXACTLY AS IT WAS.
 *
 * `iters` is the caller's cost parameter and must be stored beside the hash,
 * so it can be raised later without invalidating existing passwords -- which
 * is also why zero is REFUSED rather than rounded up. The old code ran
 * `for (i = 1; i < iters; i++)`, so a stored count of 0 silently became a
 * count of 1: a password hash with no work factor at all, indistinguishable
 * on the wire and in the database from one deliberately created with c = 1,
 * and still verifying successfully for the account holder. A cost parameter
 * that can quietly become the weakest legal value is not a cost parameter.
 *
 * warn_unused_result because a KDF that reports a refusal nobody reads still
 * leaves the caller holding whatever was in the buffer. */
enum pbkdf2_status pbkdf2_sha256(const uint8_t *pw, size_t pwn,
                                 const uint8_t *salt, size_t saltn,
                                 unsigned iters, uint8_t *out, size_t n)
    __attribute__((warn_unused_result));

#endif
