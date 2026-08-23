// SPDX-License-Identifier: GPL-3.0
// dexcom.c --- the parts of Dexcom's pairing that are Dexcom's alone
// Copyright 2026 Juggluco and xDrip contributors

/* Everything here is specific to Dexcom G7/Stelo transmitters, which is why
 * it is in app/ and not in lib/: the generic half of the pairing is standard
 * EC-J-PAKE and lives in lib/jpake.c, where the server uses it too.
 *
 * What is Dexcom's:
 *   - dex8, the per-connection authentication hash;
 *   - an embedded collector device key, published by Juggluco and xDrip;
 *   - the certificate key-challenge those two are used for.
 *
 * The signing itself is ordinary ECDSA over P-256 and comes from lib/ecdsa.c
 * rather than being written out again here.
 *
 * Ported from Juggluco (GPLv3) and xDrip's jamorham.keks. GPLv3.
 */

/* "p256.h" rather than "rand.h": the only entropy this file needs is one
 * ECDSA nonce, and a nonce is a curve scalar, so it comes from p256_sc_rand
 * and not from a raw byte source. */
#include "dexcom.h"
#include "aes.h"
#include "ecdsa.h"
#include "p256.h"
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* dex8: per-connection auth hash. AES-128-ECB(key, data||data)[:8]. */
int dexcom_dex8(const uint8_t key16[16], const uint8_t data8[8],
                uint8_t out8[8])
{
   uint8_t block[16];
   uint8_t enc[16];
   memcpy(block, data8, 8);
   memcpy(block + 8, data8, 8);
   aes128_encrypt(key16, block, enc);
   memcpy(out8, enc, 8);
   return 1;
}

/* Embedded collector device key for the certificate signature challenge. */
static const uint8_t devkey_priv[31] = {
    0x7c, 0xfb, 0xd5, 0x96, 0xf6, 0xe7, 0x44, 0x77, 0xb8, 0xc0, 0xe9,
    0xf6, 0xf7, 0xa1, 0x74, 0x27, 0x5e, 0x10, 0x1e, 0xf6, 0xbf, 0x7d,
    0x18, 0xca, 0xf0, 0x11, 0x81, 0xd1, 0x27, 0xb5, 0x79};
static const uint8_t devkey_pub[65] = {
    0x04, 0x51, 0x18, 0xC3, 0x5E, 0x9E, 0x41, 0xE7, 0xE0, 0x65, 0x4F,
    0xEE, 0x80, 0x1C, 0x52, 0xA9, 0xC5, 0xDF, 0xC5, 0x10, 0xEF, 0x09,
    0x59, 0x7D, 0x5C, 0xCA, 0x84, 0x61, 0xE4, 0xAF, 0x9C, 0x66, 0x67,
    0x14, 0x83, 0x4F, 0x2B, 0xC9, 0x03, 0xF1, 0x6F, 0xAB, 0xFC, 0x45,
    0x75, 0x5B, 0x01, 0x83, 0xF1, 0xA0, 0x97, 0x45, 0xCD, 0xFF, 0xCB,
    0x4E, 0x2F, 0x79, 0x9E, 0x50, 0xBE, 0xD9, 0xA6, 0xB5, 0x8C};

/* Certificate key-challenge: ECDSA-P256 sign SHA256(challenge[2:18]) with the
 * embedded device key; output raw r||s (64 bytes). challenge >= 18 bytes.
 *
 * The retry loop is required, not defensive: ECDSA fails for the rare nonce
 * that drives r or s to zero, and the only correct response is a FRESH k.
 * Reusing k across the retry would publish the private key. */
int dexcom_getchallenge(const uint8_t *challenge, size_t clen,
                        uint8_t out64[64])
{
   if (clen < 18)
      return 0;
   uint8_t hash[32];
   sha256(challenge + 2, 16, hash);
   uint8_t priv[32] = {0};
   memcpy(priv + 1, devkey_priv, 31);

   for (int t = 0; t < 32; t++) {
      uint8_t k[32];
      /* p256_sc_rand, not rand_bytes: an ECDSA nonce is a scalar in [1, n-1]
       * and this is the one generator that draws one (lib/p256.h). Zero was
       * never the exposure here -- ecdsa_p256_sign has always refused k == 0
       * and this loop would then have drawn a fresh k, which is the correct
       * response -- so what this fixes at this site is the 2^-32 bias the
       * reduction inside ecdsa_p256_sign introduced. It is included because a
       * generator used by three of four scalar sites is not one rule.
       *
       * Abort, do NOT retry: with a dead entropy source every attempt would
       * sign with the same stack garbage. p256_sc_rand propagates that
       * distinction -- it returns 0 both for a dead source and for a source so
       * broken that 64 straight draws were out of range, and neither is worth
       * a second attempt. Failing the key challenge is a case the driver
       * already handles. */
      if (!p256_sc_rand(k))
         return 0;
      if (ecdsa_p256_sign(priv, hash, k, out64, out64 + 32))
         return 1;
   }
   return 0;
}

/* THE VERIFYING SIDE, against the embedded public key. The sensor is what
 * runs this in production; here it closes the loop on the signer, so a
 * signature can be checked against a fixed challenge rather than only
 * against a sensor that is not on the desk. */
int dexcom_verify_challenge(const uint8_t *challenge, size_t clen,
                            const uint8_t sig64[64])
{
   if (clen < 18)
      return 0;
   uint8_t hash[32];
   sha256(challenge + 2, 16, hash);
   /* devkey_pub is 04||X||Y, the uncompressed SEC1 encoding, so the two
    * coordinates start one byte in. */
   return ecdsa_p256_verify(devkey_pub + 1, devkey_pub + 33, hash, sig64,
                            sig64 + 32);
}

#ifdef DEXCOM_TEST
#include <stdio.h>

int main(void)
{
   p256_init();
   int all = 1;
   {
      uint8_t k[16] = {0x6f, 0x83, 0x26, 0x74, 0x4b, 0xef, 0x03, 0xfa,
                       0xa5, 0x20, 0xad, 0x9c, 0x5c, 0xff, 0x67, 0x3f};
      uint8_t d[8]  = {0x2A, 0x40, 0x42, 0x90, 0xC4, 0xB6, 0x3B, 0x01};
      uint8_t g[8];
      static const uint8_t want[8] = {0x13, 0xab, 0x13, 0xf6,
                                      0x97, 0x5e, 0x30, 0x82};
      dexcom_dex8(k, d, g);
      int ok = memcmp(g, want, 8) == 0;
      printf("  [%s] dex8 matches vector\n", ok ? "PASS" : "FAIL");
      all &= ok;
   }
   {
      uint8_t chal[18] = {0x0c, 0x00, 0x0c, 0xee, 0x69, 0x1b, 0x76, 0x5a, 0x49,
                          0x7d, 0x22, 0x58, 0x23, 0xd1, 0x4f, 0x27, 0x8d, 0xd3};
      uint8_t sig[64];
      int ok = dexcom_getchallenge(chal, sizeof(chal), sig) &&
               dexcom_verify_challenge(chal, sizeof(chal), sig);
      printf("  [%s] cert key-challenge (ECDSA-P256) signs+verifies\n",
             ok ? "PASS" : "FAIL");
      all &= ok;
   }
   printf("\n%s\n", all ? "ALL DEXCOM TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
#endif
