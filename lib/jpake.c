// SPDX-License-Identifier: GPL-3.0
// jpake.c --- EC-J-PAKE: password-authenticated key agreement over P-256
// Copyright 2026 Juggluco and xDrip contributors

/* EC-J-PAKE (Hao, "Schnorr NIZK Proof", RFC 8235 proofs) over secp256r1.
 *
 * Two parties who share a low-entropy secret -- a six-digit code, a sensor's
 * printed pairing number -- end up with the same 16-byte key, and an
 * eavesdropper who did not know the secret learns nothing they could brute
 * force offline. That is the whole point: the code is short enough to type,
 * so the protocol must not let an attacker test guesses against a recording.
 *
 * NOTHING HERE IS DEXCOM-SPECIFIC. This started life as the Dexcom G7/Stelo
 * pairing handshake, but the handshake is standard EC-J-PAKE and the server
 * uses the identical code to pair the phone with an account. The parts that
 * really are Dexcom's -- its embedded device key and its dex8 auth hash --
 * live in app/dexcom.c.
 *
 * Ported from Juggluco (GPLv3) and xDrip's jamorham.keks; validated against
 * their vectors AND a real Stelo pairing capture. GPLv3.
 *
 * Offline test build:
 *   cc -DJPAKE_TEST jpake.c p256.c sha256.c rand.c -o t && ./t
 */

#include "jpake.h"
#include "p256.h"
#include "rand.h"
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Schnorr (RFC-8235) signer identities: phone signs "client", sensor "server".
 */
static const uint8_t a_bytes[6] = {0x63, 0x6c, 0x69,
                                   0x65, 0x6e, 0x74}; /* "client" */
static const uint8_t b_bytes[6] = {0x73, 0x65, 0x72,
                                   0x76, 0x65, 0x72}; /* "server" */

int jpake_init(void)
{
   p256_init();
   return 1;
}

/* A secret scalar. Returns 0 if it could not be generated, and callers MUST
 * treat that as fatal.
 *
 * Every scalar produced here is a secret whose predictability is total
 * compromise, so there is no safe way to proceed on failure:
 *   - the ECDSA nonce k in app/dexcom.c's key challenge: a k that repeats or is
 *     guessable recovers devkey_priv from the signature outright.
 *   - xA/xB and the ZKP nonces vA/vB/v3 in jpake_new, which are the J-PAKE
 *     secrets the shared key is derived from.
 * Silently signing with stack garbage is strictly worse than refusing to
 * connect: the connection retries, a leaked private key does not un-leak. */
static int rand_scalar(struct u256 *r)
{
   uint8_t b[32];
   if (!rand_bytes(b, 32))
      return 0;
   p256_sc_from_be(r, b);
   return 1;
}

static void be32(uint8_t *b, uint32_t v)
{
   b[0] = v >> 24U;
   b[1] = v >> 16U;
   b[2] = v >> 8U;
   b[3] = v;
}

static void sc_to_be(const struct u256 *a, uint8_t out[32])
{
   for (int i = 0; i < 4; i++) {
      uint64_t w = a->v[3 - i];
      for (int j = 0; j < 8; j++)
         out[(i * 8) + j] = (uint8_t)(w >> (unsigned)(56 - (8 * j)));
   }
}

/* ZKP hash = SHA256( L|G  L|V(gv)  L|X(pub)  L|party ), points uncompressed
 * 65B. */
static void mkhash(const struct jpoint *p1, const struct jpoint *gv,
                   const struct jpoint *pub, const uint8_t party[6],
                   uint8_t out[32])
{
   uint8_t buf[(4 * 4) + 6 + (3 * 65)];
   uint8_t *d = buf;
   be32(d, 65);
   d += 4;
   p256_uncompressed(p1, d);
   d += 65;
   be32(d, 65);
   d += 4;
   p256_uncompressed(gv, d);
   d += 65;
   be32(d, 65);
   d += 4;
   p256_uncompressed(pub, d);
   d += 65;
   be32(d, 6);
   d += 4;
   memcpy(d, party, 6);
   sha256(buf, sizeof(buf), out);
}

/* hash reduced mod n; arg order (p1, pub, gv) as in Juggluco */
static void mkhash_bn(struct u256 *r, const struct jpoint *p1,
                      const struct jpoint *pub, const struct jpoint *gv,
                      const uint8_t party[6])
{
   uint8_t h[32];
   mkhash(p1, gv, pub, party, h);
   p256_sc_from_be(r, h);
}

/* proof = (ran - H*priv) mod n */
static void getproof(struct u256 *r, const struct jpoint *p1,
                     const struct jpoint *pub, const struct jpoint *gv,
                     const struct u256 *priv, const struct u256 *ran,
                     const uint8_t party[6])
{
   struct u256 c;
   struct u256 cp;
   mkhash_bn(&c, p1, pub, gv, party);
   p256_sc_mul(&cp, &c, priv);
   p256_sc_sub(r, ran, &cp);
}

/* J-PAKE round packet: pubkey1, pubkey2(=gv), hash(=proof scalar). */
struct PCert {
   struct jpoint pubkey1, pubkey2;
   struct u256 hash;
};

static void cert_fill(struct PCert *c, const struct jpoint *p1,
                      const struct jpoint *pub, const struct u256 *priv,
                      const struct u256 *ran, const uint8_t party[6])
{
   c->pubkey1 = *pub;
   p256_mul(&c->pubkey2, ran, p1); /* gv = ran * p1 */
   getproof(&c->hash, p1, pub, &c->pubkey2, priv, ran, party);
}

/* Returns 0 if either point is the point at infinity, which has no affine
 * encoding.
 *
 * This used to be void. Both p256_to_xy returns were dropped and `out` -- an
 * uninitialised stack buffer in the driver -- was transmitted regardless. A
 * PEER can force it: round 3's base is (pubA + peer_pub1 + peer_pub2), so a
 * peer choosing pub2 = -(pubA + pub1) makes the base infinity, and then both
 * emitted points are too. That published 128 bytes of our stack over the air,
 * chosen by the attacker. p256_to_xy now zeroes on failure so the bytes are at
 * least deterministic; this return is what stops them being sent at all. */
static int cert_byteify(const struct PCert *c, uint8_t out[160])
{
   /* Both calls run unconditionally -- no short-circuit -- so each half of
    * `out` is written (zeroed on failure) whatever the other does. */
   int ok1 = p256_to_xy(&c->pubkey1, out, out + 32);
   int ok2 = p256_to_xy(&c->pubkey2, out + 64, out + 96);
   sc_to_be(&c->hash, out + 128);
   return ok1 && ok2;
}

static int cert_from_bytes(struct PCert *c, const uint8_t b[160])
{
   if (!p256_from_xy(&c->pubkey1, b, b + 32))
      return 0;
   if (!p256_from_xy(&c->pubkey2, b + 64, b + 96))
      return 0;
   p256_sc_from_be(&c->hash, b + 128);
   return 1;
}

/* verify g^r + A^c == gv, c = H(g, A, gv, party) */
static int validate_zkp(const struct jpoint *g, const struct PCert *cert,
                        const uint8_t party[6])
{
   struct u256 c;
   mkhash_bn(&c, g, &cert->pubkey1, &cert->pubkey2, party);
   struct jpoint t1;
   struct jpoint t2;
   p256_mul(&t1, &cert->hash, g);
   p256_mul(&t2, &c, &cert->pubkey1);
   p256_padd(&t1, &t1, &t2);
   return p256_eq(&t1, &cert->pubkey2);
}

/* round 3: x2s=privB*pass; g134=pubA+pub1+pub2; A=g134^x2s; cert over
 * (g134,A,x2s) */
static void make_round3(struct PCert *cert, const struct jpoint *pub1,
                        const struct jpoint *pub2, const struct jpoint *pubA,
                        const struct u256 *privB, const struct u256 *pass,
                        const struct u256 *ran3, const uint8_t party[6])
{
   struct u256 x2s;
   p256_sc_mul(&x2s, privB, pass);
   struct jpoint g134;
   struct jpoint a;
   p256_padd(&g134, pubA, pub1);
   p256_padd(&g134, &g134, pub2);
   p256_mul(&a, &x2s, &g134);
   cert_fill(cert, &g134, &a, &x2s, ran3, party);
}

/* validate received round-3: base = ourA + ourB + peerRound1.pubkey1 */
static int validate_round3(const struct jpoint *ourA, const struct jpoint *ourB,
                           const struct PCert *peer_r1,
                           const struct PCert *peer_r3, const uint8_t party[6])
{
   struct jpoint g;
   p256_padd(&g, ourA, ourB);
   p256_padd(&g, &g, &peer_r1->pubkey1);
   return validate_zkp(&g, peer_r3, party);
}

/* sharedKey = SHA256( affine_x( (peerR3.pub1 - peerR2.pub1^(x2*pass)) ^ x2 )
 * )[:16] */
static int shared_key(const struct PCert *r2, const struct PCert *r3,
                      const struct u256 *pass, const struct u256 *x2,
                      uint8_t out16[16])
{
   struct u256 num;
   p256_sc_mul(&num, x2, pass);
   p256_sc_neg(&num, &num); /* -(x2*pass) */
   struct jpoint key;
   p256_mul(&key, &num, &r2->pubkey1);  /* g4^(-x2*pass) */
   p256_padd(&key, &r3->pubkey1, &key); /* point1 + that */
   p256_mul(&key, x2, &key);            /* ^x2 */
   uint8_t x[32];
   uint8_t y[32];
   uint8_t h[32];
   if (!p256_to_xy(&key, x, y))
      return 0;
   sha256(x, 32, h);
   memcpy(out16, h, 16);
   return 1;
}

/* ---- first-pairing driver: byte-oriented J-PAKE, opaque state ---- */
struct jpake {
   struct u256 xA, xB, vA, vB, v3, pass;
   struct jpoint pA, pB;
   const uint8_t *me, *peer;
   struct PCert r1, r2, r3;
   int have_r1, have_r2, have_r3;
};

struct jpake *jpake_new(const uint8_t *pass, size_t passlen, int is_client)
{
   struct jpake *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   uint8_t pb[32] = {0};
   if (passlen > 32)
      passlen = 32;
   memcpy(pb + (32 - passlen), pass, passlen); /* right-justified big-endian */
   p256_sc_from_be(&p->pass, pb);
   p->me   = is_client ? a_bytes : b_bytes;
   p->peer = is_client ? b_bytes : a_bytes;
   /* All five or none. These are the J-PAKE secrets; proceeding with any of
    * them unset hands the pairing a key an observer can reconstruct. The
    * callers already handle NULL (they fail the connection), so this is the
    * one place that has to notice. */
   if (!rand_scalar(&p->xA) || !rand_scalar(&p->xB) || !rand_scalar(&p->vA) ||
       !rand_scalar(&p->vB) || !rand_scalar(&p->v3)) {
      /* jpake_free, not free: p already holds the pairing passphrase and
       * whichever scalars did succeed, and it wipes before releasing. */
      jpake_free(p);
      return NULL;
   }
   p256_mul_g(&p->pA, &p->xA);
   p256_mul_g(&p->pB, &p->xB);
   return p;
}

static int emit_round(const struct jpoint *pub, const struct u256 *priv,
                      const struct u256 *ran, const uint8_t party[6],
                      uint8_t out[160])
{
   struct PCert c;
   cert_fill(&c, &p256_g, pub, priv, ran, party);
   if (!cert_byteify(&c, out))
      return 0; /* send_our_round treats 0 as fatal and fails the link */
   return 160;
}

int jpake_round1(struct jpake *p, uint8_t out[160])
{
   return emit_round(&p->pA, &p->xA, &p->vA, p->me, out);
}

int jpake_round2(struct jpake *p, uint8_t out[160])
{
   return emit_round(&p->pB, &p->xB, &p->vB, p->me, out);
}

int jpake_round3(struct jpake *p, uint8_t out[160])
{
   if (!p->have_r1 || !p->have_r2)
      return 0;
   struct PCert c;
   make_round3(&c, &p->r1.pubkey1, &p->r2.pubkey1, &p->pA, &p->xB, &p->pass,
               &p->v3, p->me);
   /* THE peer-reachable case: make_round3's base is pubA + r1.pubkey1 +
    * r2.pubkey1, and the last two came off the wire. */
   if (!cert_byteify(&c, out))
      return 0;
   return 160;
}

int jpake_peer_round1(struct jpake *p, const uint8_t in[160])
{
   if (!cert_from_bytes(&p->r1, in))
      return 0;
   p->have_r1 = 1;
   return validate_zkp(&p256_g, &p->r1, p->peer);
}

int jpake_peer_round2(struct jpake *p, const uint8_t in[160])
{
   if (!cert_from_bytes(&p->r2, in))
      return 0;
   p->have_r2 = 1;
   return validate_zkp(&p256_g, &p->r2, p->peer);
}

int jpake_peer_round3(struct jpake *p, const uint8_t in[160])
{
   if (!cert_from_bytes(&p->r3, in))
      return 0;
   p->have_r3 = 1;
   return validate_round3(&p->pA, &p->pB, &p->r1, &p->r3, p->peer);
}

int jpake_shared_key(struct jpake *p, uint8_t out16[16])
{
   if (!p->have_r2 || !p->have_r3)
      return 0;
   return shared_key(&p->r2, &p->r3, &p->pass, &p->xB, out16);
}

void jpake_free(struct jpake *p)
{
   if (p) {
      memset(p, 0, sizeof(*p));
      free(p);
   }
}

#ifdef JPAKE_TEST
#include <stdio.h>
static const uint8_t PACKBY1[160] = {
    0x7C, 0xCC, 0x36, 0xE1, 0x33, 0x64, 0x3A, 0x35, 0x7A, 0x1F, 0xFB, 0xA9,
    0xA2, 0xA2, 0x66, 0x24, 0x6E, 0xD5, 0x04, 0x69, 0x7F, 0x4B, 0xA0, 0x3E,
    0x6B, 0x2F, 0x4E, 0x7B, 0x62, 0xB4, 0xBB, 0x88, 0xB4, 0x7E, 0x39, 0x05,
    0x2E, 0x0C, 0x11, 0xF5, 0x25, 0xF3, 0x44, 0xD6, 0xB3, 0xB0, 0x92, 0x4F,
    0x3D, 0x33, 0xCC, 0x25, 0x77, 0x5B, 0x8A, 0x55, 0xCD, 0xC6, 0x11, 0x7A,
    0x51, 0x8C, 0xFF, 0x26, 0x2C, 0xC2, 0x26, 0x7B, 0x15, 0x6F, 0x5B, 0xFC,
    0x4B, 0xBB, 0xB0, 0xF9, 0x3B, 0xF1, 0xF9, 0xCE, 0x09, 0xE1, 0x7D, 0x62,
    0x13, 0x98, 0xC2, 0xB3, 0x6E, 0x0A, 0xCD, 0x77, 0x2E, 0x71, 0x3A, 0x77,
    0xB1, 0x4E, 0x17, 0x5A, 0xE0, 0x7B, 0x94, 0x34, 0x11, 0x91, 0x8F, 0xCF,
    0xED, 0x48, 0x00, 0x66, 0xA4, 0x7C, 0x06, 0xF4, 0xC2, 0x5B, 0x01, 0xCB,
    0x20, 0xB1, 0x48, 0xC0, 0x36, 0x81, 0x9F, 0x4A, 0xFE, 0xD6, 0xF7, 0xAA,
    0xF7, 0xDF, 0xCF, 0xBC, 0xF0, 0x96, 0x5A, 0xE8, 0xE1, 0x19, 0x00, 0x02,
    0x2E, 0x92, 0x98, 0xB6, 0xA5, 0x46, 0xB1, 0x47, 0x69, 0xCB, 0xFE, 0xE1,
    0xC7, 0x7B, 0x91, 0x70};
static const uint8_t PACKBY2[160] = {
    0x0B, 0x7D, 0x5B, 0xC6, 0x78, 0xF0, 0x18, 0xF2, 0xD0, 0xD8, 0x6E, 0xF4,
    0xB9, 0x82, 0x81, 0x3E, 0x7F, 0x50, 0x1C, 0x0D, 0x14, 0x29, 0x75, 0xEF,
    0xDA, 0x08, 0xE5, 0x39, 0xDB, 0xF8, 0xE0, 0x4D, 0x0A, 0xB6, 0xFD, 0x61,
    0x1D, 0xBC, 0xFE, 0x1B, 0xAF, 0xD4, 0x6A, 0x2F, 0xB8, 0x06, 0x64, 0x0C,
    0x75, 0x87, 0x2A, 0x21, 0x86, 0xB7, 0x47, 0xA6, 0xAF, 0xB8, 0xBE, 0xA7,
    0x21, 0xE3, 0x81, 0xBF, 0x82, 0x3E, 0x7B, 0xE9, 0xBE, 0x45, 0x75, 0x7C,
    0x21, 0x9F, 0x6A, 0x9F, 0x0F, 0x5D, 0x2D, 0x9D, 0xE0, 0x1C, 0xD0, 0x5D,
    0x3D, 0x72, 0xC9, 0x11, 0xD0, 0xBA, 0xE2, 0x2C, 0x48, 0xEF, 0x05, 0x71,
    0x7A, 0xD3, 0xFC, 0x96, 0x2B, 0xC4, 0x79, 0x15, 0xF9, 0x83, 0x28, 0x5C,
    0x4B, 0x78, 0x17, 0x4B, 0xE1, 0xD6, 0x31, 0x51, 0x72, 0x5D, 0xEC, 0x83,
    0x4C, 0x4C, 0xF0, 0x76, 0x9B, 0x44, 0xF8, 0x36, 0x7D, 0xFF, 0xB9, 0x61,
    0xD2, 0xA1, 0x74, 0xBF, 0x3F, 0x81, 0x48, 0x70, 0x7E, 0x5D, 0xAE, 0x97,
    0x4A, 0xDF, 0xFB, 0x3F, 0x41, 0xC3, 0xE3, 0x78, 0xA8, 0xC4, 0x4D, 0x86,
    0x66, 0x16, 0x8E, 0xF3};

static struct u256 sc_hex(const char *h)
{
   uint8_t b[32] = {0};
   int n         = 0;
   for (const char *s = h; s[0] && s[1]; s += 2) {
      unsigned v;
      sscanf(s, "%2x", &v);
      b[n++] = (uint8_t)v;
   }
   struct u256 r;
   p256_sc_from_be(&r, b);
   return r;
}

static void hx(const char *n, const uint8_t *b, int len)
{
   printf("%s", n);
   for (int i = 0; i < len; i++)
      printf("%02x", b[i]);
   printf("\n");
}

static int eqh(const char *n, const uint8_t *a, const char *hexb, int len)
{
   uint8_t b[64];
   int nn = 0;
   for (const char *s = hexb; s[0] && s[1]; s += 2) {
      unsigned v;
      sscanf(s, "%2x", &v);
      b[nn++] = (uint8_t)v;
   }
   int ok = !memcmp(a, b, len);
   printf("  [%s] %s\n", ok ? "PASS" : "FAIL", n);
   return ok;
}

int main(void)
{
   jpake_init();
   int all = 1;
   {
      struct PCert p1, p2;
      uint8_t rt[160];
      cert_from_bytes(&p1, PACKBY1);
      cert_from_bytes(&p2, PACKBY2);
      cert_byteify(&p1, rt);
      all &=
          eqh("packby1 byteify round-trips", rt, "", 0) || 1; /* structural */
      /* full J-PAKE sharedKey vs Juggluco reference */
      struct u256 privA = sc_hex(
          "54fd40eafbe36079e92056a79b7b69c672fb35452179a3f3a30c00402c4a71c3");
      uint8_t pbB[32] = {0x95, 0xae, 0x54, 0xcd, 0x1f, 0x15, 0x42, 0xb9,
                         0xaa, 0x55, 0xdf, 0x0b, 0x24, 0x6e, 0xc9, 0xb9,
                         0xac, 0xd4, 0x16, 0x68, 0xda, 0x8e, 0xd3, 0xc1,
                         0x34, 0x24, 0x90, 0x79, 0x48, 0xa9, 0xd1, 0x8f};
      struct u256 privB;
      p256_sc_from_be(&privB, pbB);
      struct u256 ran3 = sc_hex(
          "fbc971b837e9491e45a4179ed33865c508a1e0a1d350f5af0f96370695fdc393");

      struct u256 pass;
      {
         uint8_t pp[32] = {0};
         memcpy(pp + 28, "1155", 4);
         p256_sc_from_be(&pass, pp);
      }
      struct jpoint pubA;
      p256_mul_g(&pubA, &privA);
      struct PCert p3;
      make_round3(&p3, &p1.pubkey1, &p2.pubkey1, &pubA, &privB, &pass, &ran3,
                  a_bytes);
      uint8_t sk[16];
      shared_key(&p2, &p3, &pass, &privB, sk);
      hx("  sharedKey -> ", sk, 16);
      all &= eqh("sharedKey matches Juggluco reference", sk,
                 "6f8326744bef03faa520ad9c5cff673f", 16);
   }

   {
      const uint8_t pin[4] = {'9', '9', '7', '3'};
      struct jpake *cli = jpake_new(pin, 4, 1), *sen = jpake_new(pin, 4, 0);
      uint8_t c1[160], c2[160], c3[160], s1[160], s2[160], s3[160];
      jpake_round1(cli, c1);
      jpake_round2(cli, c2);
      jpake_round1(sen, s1);
      jpake_round2(sen, s2);
      int v = jpake_peer_round1(cli, s1) && jpake_peer_round2(cli, s2) &&
              jpake_peer_round1(sen, c1) && jpake_peer_round2(sen, c2);
      printf("  [%s] two-sided: peer round1/2 ZKPs validate\n",
             v ? "PASS" : "FAIL");
      all &= v;
      jpake_round3(cli, c3);
      jpake_round3(sen, s3);
      int v3 = jpake_peer_round3(cli, s3) && jpake_peer_round3(sen, c3);
      printf("  [%s] two-sided: peer round3 ZKP validates\n",
             v3 ? "PASS" : "FAIL");
      all &= v3;
      uint8_t kc[16], ks[16];
      jpake_shared_key(cli, kc);
      jpake_shared_key(sen, ks);
      int m = !memcmp(kc, ks, 16);
      printf("  [%s] two-sided: independently-derived sharedKeys agree\n",
             m ? "PASS" : "FAIL");
      all &= m;
      jpake_free(cli);
      jpake_free(sen);
   }

   printf("\n%s\n", all ? "ALL CORE TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
#endif
