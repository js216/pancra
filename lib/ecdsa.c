/* SPDX-License-Identifier: GPL-3.0
 * ecdsa.c --- ECDSA over NIST P-256 (FIPS 186-4)
 * Copyright 2026 Jakob Kastelic
 *
 * Signing takes the nonce k as a parameter rather than generating it. That
 * is not a convenience: k must never repeat and must never be guessable --
 * either recovers the private key from a single pair of signatures -- so the
 * caller is made to name its entropy source, and a test can pin a published
 * vector by supplying the k that produced it.
 */
#include "ecdsa.h"
#include "p256.h"
#include <stddef.h>
#include <stdint.h>

static void sc_to_be(uint8_t out[32], const struct u256 *a)
{
   for (size_t i = 0; i < 4; i++)
      for (size_t j = 0; j < 8; j++)
         out[31 - ((i * 8) + j)] = (uint8_t)(a->v[i] >> (8U * j));
}

int ecdsa_p256_sign(const uint8_t d_be[32], const uint8_t hash[32],
                    const uint8_t k_be[32], uint8_t r_be[32], uint8_t s_be[32])
{
   struct u256 d;
   struct u256 z;
   struct u256 k;
   struct u256 r;
   struct u256 s;
   struct u256 t;
   p256_sc_from_be(&d, d_be);
   p256_sc_from_be(&z, hash);
   p256_sc_from_be(&k, k_be);
   if (p256_sc_is_zero(&k) || p256_sc_is_zero(&d))
      return 0;

   struct jpoint kg;
   p256_mul_g(&kg, &k);
   uint8_t rx[32];
   uint8_t ry[32];
   if (!p256_to_xy(&kg, rx, ry))
      return 0;
   p256_sc_from_be(&r, rx); /* r = x(kG) mod n */
   if (p256_sc_is_zero(&r))
      return 0;

   p256_sc_mul(&t, &r, &d); /* t = r*d */
   p256_sc_add(&t, &t, &z); /* t = z + r*d */
   p256_sc_inv(&s, &k);     /* s = k^-1 */
   p256_sc_mul(&s, &s, &t); /* s = k^-1 (z + r*d) */
   if (p256_sc_is_zero(&s))
      return 0;

   sc_to_be(r_be, &r);
   sc_to_be(s_be, &s);
   return 1;
}

int ecdsa_p256_verify(const uint8_t qx[32], const uint8_t qy[32],
                      const uint8_t hash[32], const uint8_t r_be[32],
                      const uint8_t s_be[32])
{
   struct u256 r;
   struct u256 s;
   struct u256 z;
   struct u256 w;
   struct u256 u1;
   struct u256 u2;
   p256_sc_from_be(&r, r_be);
   p256_sc_from_be(&s, s_be);
   p256_sc_from_be(&z, hash);
   if (p256_sc_is_zero(&r) || p256_sc_is_zero(&s))
      return 0;

   /* The public key must be validated, not merely decoded: p256_from_xy
    * checks the point is on the curve, which is what stops a forged key from
    * steering the arithmetic off it. */
   struct jpoint pub;
   if (!p256_from_xy(&pub, qx, qy))
      return 0;

   struct jpoint u1g;
   struct jpoint u2q;
   struct jpoint sum;
   p256_sc_inv(&w, &s);
   p256_sc_mul(&u1, &z, &w);
   p256_sc_mul(&u2, &r, &w);
   p256_mul_g(&u1g, &u1);
   p256_mul(&u2q, &u2, &pub);
   p256_padd(&sum, &u1g, &u2q);
   if (p256_is_inf(&sum))
      return 0;
   uint8_t sx[32];
   uint8_t sy[32];
   if (!p256_to_xy(&sum, sx, sy))
      return 0;
   struct u256 v;
   p256_sc_from_be(&v, sx);
   struct u256 diff;
   p256_sc_sub(&diff, &v, &r);
   return p256_sc_is_zero(&diff);
}
