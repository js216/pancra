// SPDX-License-Identifier: GPL-3.0
// aes.c --- Portable constant-time AES-128 single-block encryption
// Copyright 2026 Jakob Kastelic

/* Portable AES-128. NO TABLE IS INDEXED BY A SECRET; see aes.h.
 *
 * As a 256-byte array the S-box is read at a state- or key-derived offset,
 * sixteen times a round and sixteen more per key schedule -- the whole of the
 * Bernstein / Osvik-Shamir-Tromer surface. Here it is COMPUTED, from its
 * definition, with bitwise operations on bit-sliced data.
 *
 * HOW, in one paragraph. The AES S-box is the multiplicative inverse in
 * GF(2^8) (with 0 mapped to itself) followed by a fixed affine map over GF(2).
 * The inverse is x^254, by Fermat, and exponentiation is squarings and
 * multiplications in the field -- all of which are AND and XOR once the data
 * is transposed so that bit b of every byte lives in plane b. Sixteen bytes go
 * in one 16-bit word per plane, so the whole SubBytes step is 11 field
 * operations on 8 words, and the cost per byte falls by sixteen.
 *
 * WHY THIS IS NOT "a hand-rolled cryptographic implementation" in the sense
 * aes.h warns about. Nothing here is a new cipher, a new circuit, or a clever
 * minimisation: it is the S-box's textbook definition evaluated directly, and
 * the surrounding rounds are the ordinary byte-oriented ShiftRows, MixColumns
 * and AddRoundKey. What stands in for a lookup is arithmetic whose
 * agreement with that lookup is checked EXHAUSTIVELY, for all 256 inputs, in
 * test/srv/cryptotest.c, alongside the FIPS-197 and GCM vectors the file
 * already had to pass. The property itself -- no memory read at a
 * secret-derived index -- is structural and visible: there is one array in
 * this file, the round-constant table, and its index is the round number. */
#include "aes.h"
#include <stddef.h>
#include <stdint.h>

/* ---- GF(2^8), BIT-SLICED --------------------------------------------------
 *
 * A value is eight uint16_t planes: bit i of plane b is bit b of byte i. Every
 * routine below therefore operates on sixteen bytes at once, in parallel, with
 * no data-dependent control flow and no memory access at a computed index. */

/* THE VALUE TYPE, passed and returned BY VALUE on purpose. Taking
 * `uint16_t a[8], uint16_t r[8]` pointers gives the compiler operands it must
 * assume can alias -- so every intermediate goes back to memory between
 * operations and the cipher runs three times slower than the operation count
 * says it should. A struct by value cannot alias anything and lives in
 * registers. */
struct gf8 {
   uint16_t p[8];
};

/* Fold a 15-term carry-less product back into 8 terms, modulo the AES field
 * polynomial x^8 + x^4 + x^3 + x + 1.
 *
 * From x^8 = x^4 + x^3 + x + 1 it follows that a term of degree k >= 8
 * contributes to degrees k-4, k-5, k-7 and k-8. Folding from the TOP down
 * matters: k-4 can itself still be >= 8, and going downwards means that term
 * has not been processed yet. */
static struct gf8 gf_reduce(uint16_t t[15])
{
   struct gf8 r;
   for (int k = 14; k >= 8; k--) {
      const uint16_t c = t[k];
      t[k - 4] ^= c;
      t[k - 5] ^= c;
      t[k - 7] ^= c;
      t[k - 8] ^= c;
   }
   for (int i = 0; i < 8; i++)
      r.p[i] = t[i];
   return r;
}

/* a * b. Schoolbook: the coefficient of x^(i+j) collects a_i AND b_j. */
static struct gf8 gf_mul(struct gf8 a, struct gf8 b)
{
   uint16_t t[15] = {0};
   for (int i = 0; i < 8; i++)
      for (int j = 0; j < 8; j++)
         t[i + j] ^= (uint16_t)(a.p[i] & b.p[j]);
   return gf_reduce(t);
}

/* a * a. Squaring is linear over GF(2) -- the cross terms cancel in pairs --
 * so it is a spread of the planes to even degrees and one reduction, which is
 * why the exponentiation below prefers squarings to multiplications. */
static struct gf8 gf_sqr(struct gf8 a)
{
   uint16_t t[15] = {0};
   for (int i = 0; i < 8; i++)
      t[(size_t)i * 2] = a.p[i];
   return gf_reduce(t);
}

/* a^-1, and 0 -> 0.
 *
 * a^254 = a^-1 for every nonzero a (the group has order 255), and 0^254 = 0,
 * which is exactly the convention AES wants -- so no special case, and
 * therefore no branch. The addition chain is 7 squarings and 4 multiplies:
 *
 *   x^2 -> x^3 -> x^6 -> x^12 -> x^15 -> x^240 -> x^252 -> x^254 */
static struct gf8 gf_inv(struct gf8 a)
{
   const struct gf8 x2   = gf_sqr(a);
   const struct gf8 x3   = gf_mul(x2, a);
   const struct gf8 x6   = gf_sqr(x3);
   const struct gf8 x12  = gf_sqr(x6);
   const struct gf8 x15  = gf_mul(x12, x3);
   struct gf8 x240       = gf_sqr(x15);
   x240                  = gf_sqr(x240);
   x240                  = gf_sqr(x240);
   x240                  = gf_sqr(x240);
   const struct gf8 x252 = gf_mul(x240, x12);
   return gf_mul(x252, x2);
}

/* The S-box's affine half: b_i = a_i ^ a_(i+4) ^ a_(i+5) ^ a_(i+6) ^ a_(i+7)
 * ^ c_i, indices mod 8, with c = 0x63. The one conditional here is on a bit of
 * that CONSTANT and on the loop index, neither of which is a secret. */
static struct gf8 gf_affine(struct gf8 a)
{
   struct gf8 o;
   for (int i = 0; i < 8; i++) {
      const unsigned u = (unsigned)i;
      uint16_t v = (uint16_t)((unsigned)a.p[i] ^ (unsigned)a.p[(u + 4U) & 7U] ^
                              (unsigned)a.p[(u + 5U) & 7U] ^
                              (unsigned)a.p[(u + 6U) & 7U] ^
                              (unsigned)a.p[(u + 7U) & 7U]);
      if (((0x63U >> u) & 1U) != 0U)
         v = (uint16_t)~v;
      o.p[i] = v;
   }
   return o;
}

/* ---- THE TRANSPOSE, which is the only cost this scheme adds ---------------
 *
 * Both directions are fixed shifts of a fixed number of bytes: the loop bounds
 * are `n` and 8, never a value. */
static struct gf8 gf_slice(const uint8_t *s, int n)
{
   struct gf8 r;
   for (int b = 0; b < 8; b++)
      r.p[b] = 0;
   for (int i = 0; i < n; i++)
      for (int b = 0; b < 8; b++)
         r.p[b] |=
             (uint16_t)((((unsigned)s[i] >> (unsigned)b) & 1U) << (unsigned)i);
   return r;
}

static void gf_unslice(struct gf8 a, uint8_t *s, int n)
{
   for (int i = 0; i < n; i++) {
      unsigned v = 0;
      for (int b = 0; b < 8; b++)
         v |= (((unsigned)a.p[b] >> (unsigned)i) & 1U) << (unsigned)b;
      s[i] = (uint8_t)v;
   }
}

/* SubBytes over `n` bytes in place, n <= 16. The whole S-box, for every byte
 * at once. */
static void sub_bytes(uint8_t *s, int n)
{
   gf_unslice(gf_affine(gf_inv(gf_slice(s, n))), s, n);
}

/* Multiply by x in GF(2^8): shift left, and fold in the reduction polynomial
 * if a one fell off the top.
 *
 * The fold is NOT `(x >> 7) * 0x1b`, a multiply by a secret 0 or 1. Every
 * core this repo targets has a fixed-latency integer multiplier, so that is
 * almost certainly not leaking -- but "almost certainly" is a claim about a
 * microarchitecture, and small cores with early-terminating multipliers exist
 * (this server also builds for a riscv64 board). A mask costs the same and
 * needs no such claim: 0 - (x >> 7) is 0x00 or 0xff, and AND with 0x1b picks
 * the polynomial or nothing. */
static uint8_t xtime(uint8_t x)
{
   const unsigned hi = (unsigned)x >> 7U; /* 0 or 1 */
   return (uint8_t)(((unsigned)x << 1U) ^ ((0U - hi) & 0x1bU));
}

/* Expand a 16-byte key into 176 bytes (11 round keys).
 *
 * ONCE PER KEY, NOT ONCE PER BLOCK. Run inside aes128_encrypt, every 16 bytes
 * of TLS record traffic re-derives the whole schedule -- 40 S-box evaluations
 * per block on top of the 160 the rounds need. With a computed S-box that
 * overhead is not a rounding error, which is why struct aes128 exists and why
 * lib/gcm.c keeps one. */
void aes128_init(struct aes128 *ctx, const uint8_t key[16])
{
   static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                    0x20, 0x40, 0x80, 0x1b, 0x36};
   uint8_t *rk                   = ctx->rk;
   for (int i = 0; i < 16; i++)
      rk[i] = key[i];
   for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
         const uint8_t tmp = t[0];
         t[0]              = t[1];
         t[1]              = t[2];
         t[2]              = t[3];
         t[3]              = tmp; /* RotWord */
         sub_bytes(t, 4);         /* SubWord */
         t[0] ^= rcon[(i / 16) - 1];
      }
      for (int j = 0; j < 4; j++)
         rk[i + j] = rk[i - 16 + j] ^ t[j];
   }
}

void aes128_encrypt_ctx(const struct aes128 *ctx, const uint8_t in[16],
                        uint8_t out[16])
{
   const uint8_t *rk = ctx->rk;
   uint8_t s[16];
   for (int i = 0; i < 16; i++)
      s[i] = in[i] ^ rk[i];
   for (int round = 1; round <= 10; round++) {
      sub_bytes(s, 16);
      /* ShiftRows (column-major state: byte index = col*4 + row) */
      uint8_t t[16];
      for (int r = 0; r < 4; r++)
         for (int c = 0; c < 4; c++)
            t[(c * 4) + r] = s[(((c + r) % 4) * 4) + r];
      for (int i = 0; i < 16; i++)
         s[i] = t[i];
      /* MixColumns (skip on final round) */
      if (round != 10) {
         for (int c = 0; c < 4; c++) {
            uint8_t *col     = s + ((size_t)c * 4);
            const uint8_t a0 = col[0];
            const uint8_t a1 = col[1];
            const uint8_t a2 = col[2];
            const uint8_t a3 = col[3];
            col[0]           = (uint8_t)((unsigned)xtime(a0) ^
                                         ((unsigned)xtime(a1) ^ (unsigned)a1) ^
                                         (unsigned)a2 ^ (unsigned)a3);
            col[1] =
                (uint8_t)((unsigned)a0 ^ (unsigned)xtime(a1) ^
                          ((unsigned)xtime(a2) ^ (unsigned)a2) ^ (unsigned)a3);
            col[2] =
                (uint8_t)((unsigned)a0 ^ (unsigned)a1 ^ (unsigned)xtime(a2) ^
                          ((unsigned)xtime(a3) ^ (unsigned)a3));
            col[3] =
                (uint8_t)(((unsigned)xtime(a0) ^ (unsigned)a0) ^ (unsigned)a1 ^
                          (unsigned)a2 ^ (unsigned)xtime(a3));
         }
      }
      /* AddRoundKey */
      for (int i = 0; i < 16; i++)
         s[i] ^= rk[(round * 16) + i];
   }
   for (int i = 0; i < 16; i++)
      out[i] = s[i];
}

void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16])
{
   struct aes128 ctx;
   aes128_init(&ctx, key);
   aes128_encrypt_ctx(&ctx, in, out);
}
