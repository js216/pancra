/* SPDX-License-Identifier: GPL-3.0
 * hmac.h --- HMAC-SHA256 (RFC 2104)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef HMAC_H
#define HMAC_H
#include <stddef.h>
#include <stdint.h>

/* HMAC-SHA256 over a message of ANY length. Keys longer than the 64-byte
 * block are hashed first, as the RFC requires; the message is streamed, so
 * there is no scratch buffer and therefore no length at which this quietly
 * computes something else.
 *
 * It did have one: past 512 bytes it hashed the message first, and the app
 * carried a second copy that truncated at 256 kB instead. Both were MACs over
 * something other than the message they were handed, and they disagreed with
 * each other. There is now one implementation, linked into both halves. */
void hmac_sha256(const uint8_t *key, size_t keyn, const uint8_t *msg,
                 size_t msgn, uint8_t out[32]);

#endif
