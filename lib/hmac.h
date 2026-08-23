/* SPDX-License-Identifier: GPL-3.0
 * hmac.h --- HMAC-SHA256 (RFC 2104)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef HMAC_H
#define HMAC_H
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 over a message of ANY length. Keys longer than the 64-byte
 * block are hashed first, as the RFC requires; the message is streamed, so
 * there is no scratch buffer and therefore no length at which this quietly
 * computes something else -- a MAC over a hashed-first or truncated message
 * is a different function from HMAC, and one that changes function at a
 * buffer size disagrees with every other implementation past that size. */
void hmac_sha256(const uint8_t *key, size_t keyn, const uint8_t *msg,
                 size_t msgn, uint8_t out[32]);

/* ---- ONE KEY, MANY MESSAGES -------------------------------------------
 *
 * HMAC's two padded key blocks depend on the KEY alone, and hashing each of
 * them is a full SHA-256 compression. A caller with one key and one message
 * pays that twice and cannot avoid it; PBKDF2 has one key and as many
 * messages as its iteration count, so paying it per message doubles the work
 * of the whole KDF.
 *
 * So the two states are computed once and copied per message. The result is
 * bit-for-bit hmac_sha256 -- it is the same construction with the prefix
 * hashed ahead of time. */
struct hmac_key {
   struct sha256_ctx inner; /* H((k ^ ipad) ...), the pad already absorbed */
   struct sha256_ctx outer; /* H((k ^ opad) ...), likewise */
};

/* Absorb `key` into both states. `k` may then be used for any number of
 * messages and holds no pointer to `key`. */
void hmac_key_init(struct hmac_key *k, const uint8_t *key, size_t keyn);

/* One message under a prepared key. `k` is not modified, so one key serves
 * concurrent callers. */
void hmac_key_mac(const struct hmac_key *k, const uint8_t *msg, size_t msgn,
                  uint8_t out[32]);

#endif
