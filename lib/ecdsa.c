/* SPDX-License-Identifier: GPL-3.0
 * ecdsa.c --- ECDSA over NIST P-256 (FIPS 186-4)
 * Copyright 2026 Jakob Kastelic
 *
 * Signing takes the nonce k as a parameter rather than generating it. That
 * is not a convenience: k must never repeat and must never be guessable --
 * either recovers the private key from a single pair of signatures -- so the
 * caller is made to name its entropy source, and a test can pin a published
 * vector by supplying the k that produced it.
 *
 * CANONICAL SCALARS IN, REDUCED VALUES ONLY WHERE THEY BELONG. d, k, r and s
 * are decoded with p256_sc_from_be_checked, which requires [1, n-1] and
 * refuses everything else; the message hash and the x-coordinates of kG and
 * of u1G + u2Q are decoded with p256_sc_from_be, which reduces. The split is
 * not a style choice and inverting it breaks a different thing at each site:
 *
 *   A HASH IS NOT A SCALAR. FIPS 186-4 6.4 says e is the leftmost bits of the
 *   digest taken mod n, so reducing is the specification. Checking it instead
 *   would refuse roughly one digest in 2^32 -- signatures that fail for no
 *   reason a caller could act on, and CAVP vectors that stop reproducing.
 *
 *   AN X-COORDINATE IS NOT A SCALAR EITHER. r is DEFINED as x(kG) mod n and v
 *   as x(u1G + u2Q) mod n; x lives mod p, and p > n, so about one point in
 *   2^32 has an x above n. Checking there would refuse a correct signature at
 *   random, on both sides independently.
 *
 *   r AND s OFF THE WIRE ARE SCALARS, and reducing them was signature
 *   malleability. r and r + n reduce to the same value, so a verifier that
 *   reduced accepted three encodings of every signature whose r or s falls
 *   below 2^256 - n -- two of which the signer never produced. Measured on
 *   this code before the check: (r, s), (r + n, s) and (r, s + n) all
 *   verified. test/srv/cryptotest.c pins all three.
 *
 *   d AND k ARE SCALARS, and reducing them turned "you supplied n" into "you
 *   supplied zero", reported one layer below where it happened.
 *
 * On constant-time behaviour, since srv/tls.c signs with a long-term key on
 * every full handshake and a stranger can force those: see lib/p256.h for what
 * this rides on. Everything here does the same work for every input except the
 * refusals, and they are worth being precise about.
 *
 *   The d and k decode leaks only "the nonce or the key was not a canonical
 *   scalar" -- p256_sc_from_be_checked computes the verdict branchlessly, so
 *   not even WHICH of the two ways it was wrong shows in the timing. Both
 *   decodes run before the test, with no short-circuit between them, so the
 *   answer does not say which ARGUMENT was at fault either. It never happens
 *   with a working entropy source and its disclosure is irrelevant next to
 *   the fact that such a signature must not be produced at all.
 *
 *   The r == 0 and s == 0 refusals branch on parts of the SIGNATURE, which is
 *   about to be published.
 *
 *   p256_to_xy's infinity path is unreachable from here: k*G is infinite only
 *   for k == 0, which the decode already caught.
 */
#include "ecdsa.h"
#include "p256.h"
#include <stddef.h>
#include <stdint.h>

int ecdsa_p256_sign(const uint8_t d_be[32], const uint8_t hash[32],
                    const uint8_t k_be[32], uint8_t r_be[32], uint8_t s_be[32])
{
   struct u256 d;
   struct u256 z;
   struct u256 k;
   struct u256 r;
   struct u256 s;
   struct u256 t;
   /* BOTH decodes run, deliberately without a short-circuit between them, so
    * the work done is the same whichever argument is bad. Compared against
    * P256_SCALAR_OK by name: the enum's OK is 0, so a `!` here would read
    * backwards. */
   const enum p256_scalar ds = p256_sc_from_be_checked(&d, d_be);
   const enum p256_scalar ks = p256_sc_from_be_checked(&k, k_be);
   p256_sc_from_be(&z, hash); /* REDUCED: e = digest mod n, FIPS 186-4 6.4 */
   if (ds != P256_SCALAR_OK || ks != P256_SCALAR_OK)
      return 0;

   struct jpoint kg;
   p256_mul_g(&kg, &k);
   uint8_t rx[32];
   uint8_t ry[32];
   if (!p256_to_xy(&kg, rx, ry))
      return 0;
   /* REDUCED, and it must be: r is DEFINED as x(kG) mod n, and x lives mod p
    * which is larger than n. */
   p256_sc_from_be(&r, rx);
   if (p256_sc_is_zero(&r))
      return 0;

   p256_sc_mul(&t, &r, &d); /* t = r*d */
   p256_sc_add(&t, &t, &z); /* t = z + r*d */
   p256_sc_inv(&s, &k);     /* s = k^-1 */
   p256_sc_mul(&s, &s, &t); /* s = k^-1 (z + r*d) */
   if (p256_sc_is_zero(&s))
      return 0;

   /* p256_sc_to_be, not a loop here: the byte order of a signature is the
    * curve layer's business, and two encoders that drift agree with each
    * other and with nobody else (see p256.h). */
   p256_sc_to_be(&r, r_be);
   p256_sc_to_be(&s, s_be);
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
   struct u256 v;
   /* CHECKED, NOT REDUCED, and this is the malleability boundary: see the
    * head of this file and ecdsa.h for what accepting r + n costs. */
   const enum p256_scalar rs = p256_sc_from_be_checked(&r, r_be);
   const enum p256_scalar ss = p256_sc_from_be_checked(&s, s_be);
   if (rs != P256_SCALAR_OK || ss != P256_SCALAR_OK)
      return 0;

   /* THE PUBLIC KEY IS VALIDATED HERE, by the decode: p256_from_xy refuses a
    * pair that is not an affine point of this curve. A verifier that skipped
    * it would compute u2*Q on a point of some other curve entirely, where the
    * arithmetic is still defined and the answer means nothing. */
   struct jpoint q;
   if (!p256_from_xy(&q, qx, qy))
      return 0;

   p256_sc_from_be(&z, hash); /* REDUCED: e = digest mod n, FIPS 186-4 6.4 */
   p256_sc_inv(&w, &s);       /* w = s^-1 */
   p256_sc_mul(&u1, &z, &w);
   p256_sc_mul(&u2, &r, &w);

   struct jpoint a;
   struct jpoint b;
   struct jpoint sum;
   p256_mul_g(&a, &u1);
   p256_mul(&b, &u2, &q);
   p256_padd(&sum, &a, &b);

   uint8_t vx[32];
   uint8_t vy[32];
   /* The sum is the point at infinity for a signature that satisfies
    * u1*G = -u2*Q, which has no x-coordinate and so no v to compare. */
   if (!p256_to_xy(&sum, vx, vy))
      return 0;
   /* REDUCED, and on this side too: v is DEFINED as x(u1G + u2Q) mod n, and x
    * lives mod p. */
   p256_sc_from_be(&v, vx);
   p256_sc_sub(&v, &v, &r);
   return p256_sc_is_zero(&v);
}
