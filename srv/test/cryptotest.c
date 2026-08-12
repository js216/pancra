// SPDX-License-Identifier: GPL-3.0
// tlscrypttest.c --- the TLS primitives against the published answers
// Copyright 2026 Jakob Kastelic

/* These are not our numbers. Every vector below is copied from the document
 * that defines the algorithm -- NIST's GCM validation set, RFC 5869 for HKDF,
 * RFC 8448 for the TLS 1.3 key schedule -- so a pass means our code agrees
 * with the specification rather than with itself.
 *
 * A cipher that is wrong in a way only a peer notices is the worst kind of
 * bug to chase, so none of this gets near a socket until this file passes.
 */
#include "ecdsa.h"
#include "gcm.h"
#include "hkdf.h"
#include "hmac.h"
#include "pbkdf2.h"
#include "tls.h"
#include "p256.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void hex(const char *s, uint8_t *out, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      unsigned v;
      (void)sscanf(s + 2 * i, "%2x", &v);
      out[i] = (uint8_t)v;
   }
}

static void ck(int ok, const char *what)
{
   if (!ok) {
      printf("  FAIL: %s\n", what);
      fails = 1;
   }
}

static void cmp(const uint8_t *got, const char *want_hex, size_t n,
                const char *what)
{
   uint8_t want[128];
   hex(want_hex, want, n);
   if (memcmp(got, want, n) != 0) {
      printf("  FAIL: %s\n    got  ", what);
      for (size_t i = 0; i < n; i++)
         printf("%02x", got[i]);
      printf("\n    want %s\n", want_hex);
      fails = 1;
   }
}

int main(void)
{
   /* ---- AES-128-GCM, NIST gcmEncryptExtIV128 ---- */
   {
      /* Test case 3 of the original GCM specification: 16-byte key, 12-byte
       * IV, 64 bytes of plaintext, no AAD. */
      uint8_t key[16], iv[12], pt[64], ct[64], tag[16];
      hex("feffe9928665731c6d6a8f9467308308", key, 16);
      hex("cafebabefacedbaddecaf888", iv, 12);
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39"
          "1aafd255",
          pt, 64);
      aes128_gcm_seal(key, iv, NULL, 0, pt, 64, ct, tag);
      cmp(ct,
          "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
          "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
          "473f5985",
          64, "GCM ciphertext (NIST case 3)");
      cmp(tag, "4d5c2af327cd64a62cf35abd2ba6fab4", 16, "GCM tag (case 3)");

      /* And it must come back. */
      uint8_t back[64];
      ck(aes128_gcm_open(key, iv, NULL, 0, ct, 64, tag, back),
         "GCM open accepts a good tag");
      ck(memcmp(back, pt, 64) == 0, "GCM round trip returns the plaintext");

      /* A single flipped bit in the tag must be refused. */
      uint8_t bad[16];
      memcpy(bad, tag, 16);
      bad[0] ^= 1;
      ck(!aes128_gcm_open(key, iv, NULL, 0, ct, 64, bad, back),
         "GCM open REFUSES a forged tag");
   }

   /* ---- GCM with AAD, NIST case 4 (the shape TLS actually uses) ---- */
   {
      uint8_t key[16], iv[12], pt[64], aad[20], ct[64], tag[16];
      hex("feffe9928665731c6d6a8f9467308308", key, 16);
      hex("cafebabefacedbaddecaf888", iv, 12);
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
          pt, 60);
      hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
      aes128_gcm_seal(key, iv, aad, 20, pt, 60, ct, tag);
      cmp(tag, "5bc94fbc3221a5db94fae95ae7121a47", 16,
          "GCM tag covers the additional data");
   }

   /* ---- HKDF-SHA256, RFC 5869 test case 1 ---- */
   {
      uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
      hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, 22);
      hex("000102030405060708090a0b0c", salt, 13);
      hex("f0f1f2f3f4f5f6f7f8f9", info, 10);
      hkdf_extract(salt, 13, ikm, 22, prk);
      cmp(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
          32, "HKDF-Extract (RFC 5869 case 1)");
      hkdf_expand(prk, info, 10, okm, 42);
      cmp(okm,
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865",
          42, "HKDF-Expand (RFC 5869 case 1)");
   }

   /* ---- HMAC-SHA256, RFC 4231 test case 2 (key shorter than a block) ---- */
   {
      uint8_t out[32];
      hmac_sha256((const uint8_t *)"Jefe", 4,
                  (const uint8_t *)"what do ya want for nothing?", 28, out);
      cmp(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          32, "HMAC-SHA256 (RFC 4231 case 2)");
   }

   /* ---- the TLS 1.3 key schedule, RFC 8448 ----
    *
    * The first step of the schedule: with no PSK, Early Secret is
    * Extract(0, 0). If this disagrees, every key after it is wrong. */
   {
      uint8_t zeros[32] = {0}, early[32];
      hkdf_extract(NULL, 0, zeros, 32, early);
      cmp(early,
          "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a",
          32, "Early Secret with no PSK (RFC 8448)");
   }

   /* HkdfLabel framing: the label really does get the "tls13 " prefix, and
    * the length goes in first. Checked against RFC 8448's derived secret. */
   {
      uint8_t early[32], derived[32], zeros[32] = {0};
      hkdf_extract(NULL, 0, zeros, 32, early);
      derive_secret(early, "derived", (const uint8_t *)"", 0, derived);
      cmp(derived,
          "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba",
          32, "Derive-Secret(early, \"derived\", \"\") (RFC 8448)");
   }

   /* ---- ECDSA P-256, NIST CAVP vector (P-256, SHA-256) ----
    *
    * Signing needs a per-signature secret k, so a signature is only
    * reproducible if k is fixed. This vector fixes it, which makes the whole
    * (r, s) pair a known answer rather than something only we can check. */
   {
      p256_init();
      /* The field reduction is a fast path specific to P-256's prime; make it
       * agree with the textbook long division before trusting any curve
       * result that rides on it. */
      ck(p256_selftest() == 0, "P-256 fast reduction matches long division");
      uint8_t d[32], k[32], h[32], r[32], s[32], qx[32], qy[32];
      hex("c477f9f65c22cce20657faa5b2d1d8122336f851a508a1ed04e479c34985bf96", d, 32);
      hex("7a1a7e52797fc8caaa435d2a4dace39158504bf204fbe19f14dbb427faee50ae", k, 32);
      hex("a41a41a12a799548211c410c65d8133afde34d28bdd542e4b680cf2899c8a8c4", h, 32);
      hex("b7e08afdfe94bad3f1dc8c734798ba1c62b3a0ad1e9ea2a38201cd0889bc7a19", qx, 32);
      hex("3603f747959dbf7a4bb226e41928729063adc7ae43529e61b563bbc606cc5e09", qy, 32);
      ck(ecdsa_p256_sign(d, h, k, r, s), "ECDSA signing succeeds");
      cmp(r, "2b42f576d07f4165ff65d1f3b1500f81e44c316f1f0b3ef57325b69aca46104f",
          32, "ECDSA r (NIST P-256/SHA-256 vector)");
      cmp(s, "dc42c2122d6392cd3e3a993a89502a8198c1886fe69d262c4b329bdb6b63faf1",
          32, "ECDSA s (NIST P-256/SHA-256 vector)");
      ck(ecdsa_p256_verify(qx, qy, h, r, s),
         "...and our own verify accepts it");
      uint8_t bad[32];
      memcpy(bad, s, 32);
      bad[31] ^= 1;
      ck(!ecdsa_p256_verify(qx, qy, h, r, bad),
         "...and REFUSES a tampered signature");
   }

   /* ---- streaming SHA-256 must equal the one-shot, fed any which way ---- */
   {
      const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
      uint8_t one[32], many[32];
      sha256((const uint8_t *)msg, strlen(msg), one);
      cmp(one, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          32, "SHA-256 one-shot (FIPS 180-2 vector)");
      struct sha256_ctx c;
      sha256_init(&c);
      for (const char *q = msg; *q; q++) /* a byte at a time is the worst case */
         sha256_update(&c, (const uint8_t *)q, 1);
      sha256_final(&c, many);
      ck(memcmp(one, many, 32) == 0,
         "streaming SHA-256 agrees with the one-shot");

      /* and across a block boundary, where the padding logic actually bites */
      uint8_t big[200], a1[32], a2[32];
      for (int i = 0; i < 200; i++)
         big[i] = (uint8_t)i;
      sha256(big, 200, a1);
      sha256_init(&c);
      sha256_update(&c, big, 61);
      sha256_update(&c, big + 61, 139);
      sha256_final(&c, a2);
      ck(memcmp(a1, a2, 32) == 0, "...including across block boundaries");
   }

   /* ---- PBKDF2-HMAC-SHA256, RFC 7914 vector ---- */
   {
      uint8_t out[40];
      pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                    1, out, 40);
      cmp(out,
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
          40, "PBKDF2-HMAC-SHA256, 1 iteration (RFC 7914)");
   }

   if (fails) {
      printf("tlscrypttest: FAIL\n");
      return 1;
   }
   printf("tlscrypttest: GCM, HMAC, HKDF and the TLS 1.3 schedule agree with "
          "the specs\n");
   return 0;
}
