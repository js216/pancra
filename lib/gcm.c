/* SPDX-License-Identifier: GPL-3.0
 * gcm.c --- AES-128-GCM authenticated encryption
 * Copyright 2026 Jakob Kastelic
 *
 * See gcm.h. Built on lib/aes.c's block function; nothing here knows about
 * TLS, which is why it is a primitive in lib/ and not part of the server.
 */
#include "gcm.h"
#include "aes.h"
#include <string.h>

/* ---- GHASH ------------------------------------------------------------
 *
 * Multiplication in GF(2^128), which is what turns AES-CTR into an
 * authenticated cipher. The bit order is the awkward part: the standard
 * numbers bits from the left, so bit 0 is the top bit of byte 0, and the
 * reduction polynomial appears as 0xe1 in the first byte.
 */
static void gf_mul(uint8_t x[16], const uint8_t y[16])
{
   uint8_t z[16] = {0};
   uint8_t v[16];
   memcpy(v, y, 16);
   for (int i = 0; i < 128; i++) {
      if (x[i / 8] & (0x80u >> (i % 8)))
         for (int j = 0; j < 16; j++)
            z[j] ^= v[j];
      /* v = v >> 1, and if a 1 fell off the end, reduce */
      int lsb = v[15] & 1;
      for (int j = 15; j > 0; j--)
         v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
      v[0] >>= 1;
      if (lsb)
         v[0] ^= 0xe1;
   }
   memcpy(x, z, 16);
}

/* GHASH over `n` bytes, continuing the accumulator `y`. Whole blocks only;
 * the caller pads. */
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data,
                  size_t n)
{
   while (n >= 16) {
      for (int i = 0; i < 16; i++)
         y[i] ^= data[i];
      gf_mul(y, h);
      data += 16;
      n -= 16;
   }
   if (n) {
      uint8_t last[16] = {0};
      memcpy(last, data, n);
      for (int i = 0; i < 16; i++)
         y[i] ^= last[i];
      gf_mul(y, h);
   }
}

static void be64(uint8_t out[8], uint64_t v)
{
   for (int i = 7; i >= 0; i--) {
      out[i] = (uint8_t)(v & 0xff);
      v >>= 8;
   }
}

/* CTR keystream starting at counter `ctr`, XORed over `n` bytes. */
static void gctr(const uint8_t key[16], const uint8_t iv[12], uint32_t ctr,
                 const uint8_t *in, uint8_t *out, size_t n)
{
   uint8_t blk[16], ks[16];
   memcpy(blk, iv, 12);
   while (n) {
      blk[12] = (uint8_t)(ctr >> 24);
      blk[13] = (uint8_t)(ctr >> 16);
      blk[14] = (uint8_t)(ctr >> 8);
      blk[15] = (uint8_t)ctr;
      aes128_encrypt(key, blk, ks);
      size_t take = n < 16 ? n : 16;
      for (size_t i = 0; i < take; i++)
         out[i] = in[i] ^ ks[i];
      in += take;
      out += take;
      n -= take;
      ctr++;
   }
}

/* The authentication tag over (aad, ct), for a given key and nonce. */
static void gcm_tag(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t ctn, uint8_t tag[16])
{
   uint8_t h[16] = {0}, zero[16] = {0}, y[16] = {0}, len[16], s0[16];
   aes128_encrypt(key, zero, h);
   ghash(y, h, aad, aadn);
   ghash(y, h, ct, ctn);
   be64(len, (uint64_t)aadn * 8);
   be64(len + 8, (uint64_t)ctn * 8);
   ghash(y, h, len, 16);
   gctr(key, iv, 1, y, tag, 16); /* counter 1 masks the tag */
   (void)s0;
}

void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, size_t aadn, const uint8_t *pt,
                     size_t n, uint8_t *ct, uint8_t tag[16])
{
   gctr(key, iv, 2, pt, ct, n); /* data starts at counter 2 */
   gcm_tag(key, iv, aad, aadn, ct, n, tag);
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t n, const uint8_t tag[16], uint8_t *pt)
{
   uint8_t want[16];
   gcm_tag(key, iv, aad, aadn, ct, n, want);
   /* Constant time: a comparison that returns early tells an attacker how
    * many bytes of a forged tag were right. */
   uint8_t diff = 0;
   for (int i = 0; i < 16; i++)
      diff |= (uint8_t)(want[i] ^ tag[i]);
   if (diff)
      return 0;
   gctr(key, iv, 2, ct, pt, n);
   return 1;
}
