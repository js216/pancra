// SPDX-License-Identifier: GPL-3.0
// sha256.c --- Portable SHA-256, one-shot and streaming
// Copyright 2026 Jakob Kastelic

/* Portable SHA-256 (FIPS 180-4). No dependencies beyond stdint. */
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t ror(uint32_t x, int n)
{
   return (x >> (unsigned)n) | (x << (unsigned)(32 - n));
}

static const uint32_t kconst[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/* One copy of the compression function, two ways of feeding it: the one-shot
 * below and the streaming wrapper at the end of this file. Internal, because
 * it does no padding and is useless on its own. */
static void sha256_compress(uint32_t h[8], const uint8_t block[64])
{
   uint32_t w[64];
   for (int t = 0; t < 16; t++)
      w[t] = ((uint32_t)block[(size_t)t * 4] << 24U) |
             ((uint32_t)block[(t * 4) + 1] << 16U) |
             ((uint32_t)block[(t * 4) + 2] << 8U) |
             (uint32_t)block[(t * 4) + 3];
   for (int t = 16; t < 64; t++) {
      uint32_t s0 = ror(w[t - 15], 7) ^ ror(w[t - 15], 18) ^ (w[t - 15] >> 3U);
      uint32_t s1 = ror(w[t - 2], 17) ^ ror(w[t - 2], 19) ^ (w[t - 2] >> 10U);
      w[t]        = w[t - 16] + s0 + w[t - 7] + s1;
   }
   uint32_t a  = h[0];
   uint32_t b  = h[1];
   uint32_t c  = h[2];
   uint32_t d  = h[3];
   uint32_t e  = h[4];
   uint32_t f  = h[5];
   uint32_t g  = h[6];
   uint32_t hh = h[7];
   for (int t = 0; t < 64; t++) {
      uint32_t s1  = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      uint32_t ch  = (e & f) ^ (~e & g);
      uint32_t t1  = hh + s1 + ch + kconst[t] + w[t];
      uint32_t s0  = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2  = s0 + maj;
      hh           = g;
      g            = f;
      f            = e;
      e            = d + t1;
      d            = c;
      c            = b;
      b            = a;
      a            = t1 + t2;
   }
   h[0] += a;
   h[1] += b;
   h[2] += c;
   h[3] += d;
   h[4] += e;
   h[5] += f;
   h[6] += g;
   h[7] += hh;
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
   uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
   uint8_t block[64];
   size_t i = 0;
   /* full blocks */
   for (; i + 64 <= len; i += 64)
      sha256_compress(h, data + i);
   /* final block(s) with padding */
   size_t rem = len - i;
   for (size_t n = 0; n < rem; n++)
      block[n] = data[i + n];
   block[rem] = 0x80;
   for (size_t n = rem + 1; n < 64; n++)
      block[n] = 0;
   if (rem >= 56) {
      sha256_compress(h, block);
      for (int n = 0; n < 64; n++)
         block[n] = 0;
   }
   uint64_t bitlen = (uint64_t)len * 8;
   for (int n = 0; n < 8; n++)
      block[56 + n] = (uint8_t)(bitlen >> (unsigned)(56 - (8 * n)));
   sha256_compress(h, block);
   for (int t = 0; t < 8; t++) {
      out[(size_t)t * 4] = h[t] >> 24U;
      out[(t * 4) + 1]   = h[t] >> 16U;
      out[(t * 4) + 2]   = h[t] >> 8U;
      out[(t * 4) + 3]   = h[t];
   }
}

/* ---- streaming SHA-256 -------------------------------------------------
 *
 * sha256() above hashes a buffer in one call, which is all the pairing ever
 * needed. The server hashes things it never holds whole -- a bucket of
 * readings is fed a line at a time -- so the same compression function is
 * driven again here with the state kept between calls. One implementation of
 * the mathematics, two ways in.
 */
void sha256_init(struct sha256_ctx *c)
{
   static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19};
   memcpy(c->h, iv, sizeof iv);
   c->used  = 0;
   c->total = 0;
}

void sha256_update(struct sha256_ctx *c, const uint8_t *p, size_t n)
{
   c->total += n;
   while (n) {
      size_t take = 64 - c->used;
      if (take > n)
         take = n;
      memcpy(c->block + c->used, p, take);
      c->used += take;
      p += take;
      n -= take;
      if (c->used == 64) {
         sha256_compress(c->h, c->block);
         c->used = 0;
      }
   }
}

void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
   uint64_t bits = c->total * 8;
   /* The padding is written straight into the block rather than fed back
    * through sha256_update: a MAC over a 32-byte message spends most of its
    * padding on zeros, and a call per zero byte is most of the cost of the
    * short hashes PBKDF2 is made of.
    *
    * `used` is below 64 here -- update compresses the moment it fills -- so
    * the 0x80 always fits, and only the length field can push past the
    * block, which is the case the first compression below is for. */
   c->block[c->used++] = 0x80;
   if (c->used > 56) {
      memset(c->block + c->used, 0, 64 - c->used);
      sha256_compress(c->h, c->block);
      c->used = 0;
   }
   memset(c->block + c->used, 0, 56 - c->used);
   for (unsigned i = 0; i < 8; i++)
      c->block[56 + i] = (uint8_t)(bits >> (56U - (8U * i)));
   sha256_compress(c->h, c->block);
   for (size_t i = 0; i < 8; i++) {
      out[4 * i]       = (uint8_t)(c->h[i] >> 24U);
      out[(4 * i) + 1] = (uint8_t)(c->h[i] >> 16U);
      out[(4 * i) + 2] = (uint8_t)(c->h[i] >> 8U);
      out[(4 * i) + 3] = (uint8_t)c->h[i];
   }
}
