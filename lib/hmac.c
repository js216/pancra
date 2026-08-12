/* SPDX-License-Identifier: GPL-3.0
 * hmac.c --- HMAC-SHA256 (RFC 2104)
 * Copyright 2026 Jakob Kastelic
 *
 * See hmac.h.
 *
 * STREAMED, so the message length is not bounded by a scratch buffer.
 *
 * There used to be two of these and both were wrong above their own scratch
 * size, in different directions: this one hashed the message first past 512
 * bytes -- a different function from HMAC, so a caller crossing the boundary
 * would silently start computing something else -- and the app carried a copy
 * that TRUNCATED the message at 256 kB instead, which is a MAC over a prefix.
 * Two implementations that disagree about what they compute are not two
 * implementations of HMAC.
 *
 * sha256 exposes an incremental context, so neither compromise was necessary:
 * feed the pad, then the message, then finish. The result is HMAC over any
 * length, it matches the RFC 4231 vectors, and there is now genuinely one
 * implementation for both halves of the repo.
 */
#include "hmac.h"
#include "sha256.h"
#include <stdint.h> /* uint8_t: keys, blocks and the digest */
#include <string.h>

void hmac_sha256(const uint8_t *key, size_t keyn, const uint8_t *msg,
                 size_t msgn, uint8_t out[32])
{
   uint8_t k[64] = {0};
   if (keyn > 64)
      sha256(key, keyn, k); /* RFC 2104: a long key is its own hash */
   else
      memcpy(k, key, keyn);

   uint8_t pad[64];
   struct sha256_ctx c;

   /* inner = H((k ^ ipad) || msg) */
   for (int i = 0; i < 64; i++)
      pad[i] = (uint8_t)(k[i] ^ 0x36U);
   sha256_init(&c);
   sha256_update(&c, pad, 64);
   sha256_update(&c, msg, msgn);
   sha256_final(&c, out);

   /* outer = H((k ^ opad) || inner) */
   for (int i = 0; i < 64; i++)
      pad[i] = (uint8_t)(k[i] ^ 0x5cU);
   sha256_init(&c);
   sha256_update(&c, pad, 64);
   sha256_update(&c, out, 32);
   sha256_final(&c, out);
}
