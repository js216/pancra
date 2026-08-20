/* SPDX-License-Identifier: GPL-3.0
 * hkdf.h --- HKDF extract-and-expand over HMAC-SHA256 (RFC 5869)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef HKDF_H
#define HKDF_H
#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 is the only PRF here, so HashLen is 32 in every formula below
 * and the digest size is spelled with this name rather than a bare 32. */
#define HKDF_HASH_LEN 32

/* THE OUTPUT CEILING, straight out of RFC 5869 2.3, which lists it beside the
 * input it constrains:
 *
 *     L        length of output keying material in octets (<= 255*HashLen)
 *
 * It is not advice and it is not an implementation detail. Expand chains
 * T(1), T(2), ... T(N) with a ONE-BYTE counter appended to each HMAC input,
 * N = ceil(L/HashLen), so there is no 256th block for the construction to
 * name. Past 255 blocks the function is simply not HKDF any more.
 *
 * WHAT THE OLD CODE DID, for whoever ends up debugging a key that does not
 * match: the counter was a `uint8_t` that ran 1, 2, ... 255, 0, 1, 2 and kept
 * going, and nothing compared n against anything. An ask for 8161 bytes
 * returned 8161 bytes and reported nothing, with block 256 computed as
 * HMAC(PRK, T(255) || info || 0x00) -- a value no other HKDF on earth
 * produces from those inputs. A peer that had it right would have agreed
 * about the first 8160 bytes and disagreed about every byte after, and there
 * was no return value, no log line and no assertion anywhere to say so. */
#define HKDF_L_MAX (255 * HKDF_HASH_LEN) /* 8160 */

/* THE INFO CAPACITY, and why this construction has one when the RFC does not.
 *
 * RFC 5869 puts no limit on `info`. This implementation does, because it
 * assembles T(i-1) || info || i into one stack block and hands the whole
 * thing to the one-shot hmac_sha256(). Streaming `info` through an
 * incremental HMAC would remove the bound outright, but lib/hmac.c exposes
 * only the one-shot form, so the bound stays and is enforced instead of
 * assumed.
 *
 * The number is sized by the largest `info` anything in this repo builds: the
 * HkdfLabel of RFC 8446 7.1, whose two vectors are `opaque label<7..255>` and
 * `opaque context<0..255>` behind a uint16, so 2 + 1 + 255 + 1 + 255 = 514.
 * srv/tls.c static-asserts its own worst case against this constant, so the
 * two cannot drift apart silently.
 *
 * WHAT THE OLD CODE DID: the block was `uint8_t in[32 + 256 + 1]`, nothing
 * looked at infon, and the header simply asked callers to stay under 256
 * ("Every caller here passes an HkdfLabel, which cannot" -- while the
 * HkdfLabel encoder bounded neither its label nor its context). infon == 257
 * therefore wrote one byte past a stack array. */
#define HKDF_INFO_MAX 514

/* WHY A TYPED STATUS AND NOT A BOOL: a refusal here has three unrelated
 * causes and each one is a different caller bug. "It returned 0" sends the
 * next reader looking at the wrong one of the three. */
enum hkdf_status {
   HKDF_OK = 0,
   HKDF_ERR_ARG,  /* a NULL where bytes were promised */
   HKDF_ERR_INFO, /* infon > HKDF_INFO_MAX (see above) */
   HKDF_ERR_LEN   /* n == 0, or n > HKDF_L_MAX (RFC 5869 2.3) */
};

/* HKDF-Extract. salt may be NULL, which the RFC defines as HashLen zeros.
 *
 * NO STATUS, because there is nothing here to refuse: both inputs go straight
 * into HMAC-SHA256, which bounds neither, and the output is a fixed 32 bytes.
 *
 * ONE ROUGH EDGE LEFT DELIBERATELY: salt == NULL with saltn != 0 is read as
 * "no salt" and the caller's saltn is discarded -- a silent substitution of
 * exactly the kind the rest of this header exists to remove. Removing it
 * needs a return channel this function does not have and its callers in
 * srv/tls.c do not check, and no caller passes that pair. Flagged, not
 * fixed; see the matching note in hkdf.c. */
void hkdf_extract(const uint8_t *salt, size_t saltn, const uint8_t *ikm,
                  size_t ikmn, uint8_t out[HKDF_HASH_LEN]);

/* HKDF-Expand. HKDF_OK, or a status with `out` LEFT EXACTLY AS IT WAS.
 *
 * REFUSES, NEVER CLAMPS. A caller asking for more than the construction can
 * produce has a bug; handing it the first HKDF_L_MAX bytes instead would turn
 * that bug into a key that agrees with nothing and cannot be reproduced from
 * the arguments in the call. Every bound is checked before the first byte of
 * `out` is written, so a refused call cannot be told apart from one that was
 * never made.
 *
 * warn_unused_result is not decoration: a status nobody reads is worse than
 * the clamp it replaced, because the clamp at least produced bytes. */
enum hkdf_status hkdf_expand(const uint8_t prk[HKDF_HASH_LEN],
                             const uint8_t *info, size_t infon, uint8_t *out,
                             size_t n) __attribute__((warn_unused_result));

#endif
