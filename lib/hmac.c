/* SPDX-License-Identifier: GPL-3.0
 * hmac.c --- HMAC-SHA256 (RFC 2104)
 * Copyright 2026 Jakob Kastelic
 *
 * See hmac.h.
 *
 * STREAMED, so the message length is not bounded by a scratch buffer, and
 * built on the prepared-key form below so that there is one description of
 * the construction rather than two that can drift.
 */
#include "hmac.h"

#include "sha256.h"
#include <stdint.h> /* uint8_t: keys, blocks and the digest */
#include <string.h>

void hmac_key_init(struct hmac_key *k, const uint8_t *key, size_t keyn)
{
   uint8_t kb[64] = {0};
   if (keyn > 64)
      sha256(key, keyn, kb); /* RFC 2104: a long key is its own hash */
   else if (keyn)
      memcpy(kb, key, keyn);

   uint8_t pad[64];
   for (int i = 0; i < 64; i++)
      pad[i] = (uint8_t)(kb[i] ^ 0x36U);
   sha256_init(&k->inner);
   sha256_update(&k->inner, pad, 64);

   for (int i = 0; i < 64; i++)
      pad[i] = (uint8_t)(kb[i] ^ 0x5cU);
   sha256_init(&k->outer);
   sha256_update(&k->outer, pad, 64);
}

void hmac_key_mac(const struct hmac_key *k, const uint8_t *msg, size_t msgn,
                  uint8_t out[32])
{
   struct sha256_ctx c = k->inner; /* inner = H((k ^ ipad) || msg) */
   sha256_update(&c, msg, msgn);
   sha256_final(&c, out);

   c = k->outer; /* outer = H((k ^ opad) || inner) */
   sha256_update(&c, out, 32);
   sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t keyn, const uint8_t *msg,
                 size_t msgn, uint8_t out[32])
{
   struct hmac_key k;
   hmac_key_init(&k, key, keyn);
   hmac_key_mac(&k, msg, msgn, out);
}
