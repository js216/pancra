// SPDX-License-Identifier: GPL-3.0
// p256.c --- Portable NIST P-256 elliptic-curve arithmetic
// Copyright 2026 Jakob Kastelic

/* NIST P-256. See p256.h for the API and for what this is and is not safe
 * for -- in particular it is only PARTLY constant-time, and one of its callers
 * is now a public TLS server. Every branch in here that a secret can steer is
 * either gone, or carries a comment saying why it stays; if you add one, it
 * needs the same treatment. */
#include "p256.h"
#include "ct.h"   /* ct_cmov64, ct_nz64: the conditional reductions below */
#include "rand.h" /* p256_sc_rand: the only place this file asks for entropy */
#include <stddef.h>
#include <stdint.h>
#include <string.h> /* memset: zero affine outputs on the infinity path */

/* ---- THIS FILE NEEDS A 64x64 -> 128 MULTIPLY, AND C HAS NO SUCH TYPE ----
 *
 * The limb arithmetic below carries through a 128-bit accumulator.
 * `unsigned __int128` is a GCC and Clang EXTENSION -- `gcc -std=c11
 * -pedantic` says "ISO C does not support '__int128' types" -- and it does
 * not exist at all on a 32-bit target or under MSVC. Used unguarded -- or
 * guarded by an #error -- in a file whose header line says "Portable", it
 * makes a curve implementation that refuses to compile anywhere but on two
 * compilers' 64-bit targets.
 *
 * So the three things the accumulator was doing are named -- add with carry,
 * subtract with borrow, and multiply wide -- and there are TWO
 * implementations of each:
 *
 *   the __int128 one, used when the compiler has it, which is what every
 *   target this repo builds for actually uses; and
 *
 *   a portable one in plain C99, which splits each 64-bit operand into two
 *   32-bit halves. Slower (four multiplies and a handful of shifts where the
 *   other has one instruction) and correct on any conforming compiler.
 *
 * BOTH ARE BRANCH-FREE, which is not a nicety here: every one of them is
 * reached from the scalar multiplication, and a comparison compiled as a
 * branch is a timing signal about a secret. The portable carry uses the
 * standard unsigned trick -- a sum that wrapped is smaller than either
 * operand -- and nothing below tests a secret and jumps.
 *
 * THE FALLBACK IS EXERCISED, not merely present: `make -f test/Makefile
 * p256portable` builds the curve's own vectors and its constant-time
 * assertions with P256_NO_INT128 defined, so both cores are held to the same
 * answers and the same operation counts. A fallback nothing runs is a
 * fallback that does not work.
 *
 * P256_NO_INT128 forces the portable path on a compiler that has the
 * extension. That is what makes the two comparable at all: without it the
 * only way to run the fallback would be to find a machine that cannot run the
 * other one. */
#if defined(__SIZEOF_INT128__) && !defined(P256_NO_INT128)
#define P256_HAVE_INT128 1
__extension__ typedef unsigned __int128 u128;
#else
#define P256_HAVE_INT128 0
#endif

/* a + b + carry_in, as (sum, carry_out). Carry in and out are 0 or 1. */
static uint64_t addc64(uint64_t a, uint64_t b, uint64_t cin, uint64_t *sum)
{
#if P256_HAVE_INT128
   u128 t = (u128)a + b + cin;
   *sum   = (uint64_t)t;
   return (uint64_t)(t >> 64U);
#else
   /* A SUM THAT WRAPPED IS SMALLER THAN EITHER OPERAND, and that comparison
    * is the carry. Two of them, because adding the carry can wrap again --
    * a + b == UINT64_MAX with cin == 1. */
   uint64_t s  = a + b;
   uint64_t c1 = (uint64_t)(s < a);
   uint64_t t  = s + cin;
   uint64_t c2 = (uint64_t)(t < s);
   *sum        = t;
   return c1 | c2; /* both cannot be 1: c1 == 1 leaves s <= a, and then
                    * s + 1 can only wrap if s == UINT64_MAX, which c1 == 1
                    * already excludes */
#endif
}

/* a - b - borrow_in, as (difference, borrow_out). */
static uint64_t subb64(uint64_t a, uint64_t b, uint64_t bin, uint64_t *diff)
{
#if P256_HAVE_INT128
   u128 t = (u128)a - b - bin;
   *diff  = (uint64_t)t;
   return (uint64_t)((t >> 64U) & 1U);
#else
   uint64_t d  = a - b;
   uint64_t b1 = (uint64_t)(a < b);
   uint64_t e  = d - bin;
   uint64_t b2 = (uint64_t)(d < bin);
   *diff       = e;
   return b1 | b2;
#endif
}

/* a * b as (hi, lo). */
static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
#if P256_HAVE_INT128
   u128 t = (u128)a * b;
   *lo    = (uint64_t)t;
   *hi    = (uint64_t)(t >> 64U);
#else
   /* SCHOOLBOOK ON 32-BIT HALVES. Every partial product fits in 64 bits
    * (0xFFFFFFFF^2 < 2^64), and the middle column is accumulated in one
    * 64-bit word: (p00 >> 32) + lo32(p01) + lo32(p10) is at most
    * 0xFFFFFFFF + 2 * 0xFFFFFFFF, which does not overflow. */
   uint64_t a0 = a & 0xFFFFFFFFU, a1 = a >> 32U;
   uint64_t b0 = b & 0xFFFFFFFFU, b1 = b >> 32U;
   uint64_t p00 = a0 * b0, p01 = a0 * b1;
   uint64_t p10 = a1 * b0, p11 = a1 * b1;
   uint64_t mid = (p00 >> 32U) + (p01 & 0xFFFFFFFFU) + (p10 & 0xFFFFFFFFU);
   *lo          = (p00 & 0xFFFFFFFFU) | (mid << 32U);
   *hi          = p11 + (p01 >> 32U) + (p10 >> 32U) + (mid >> 32U);
#endif
}

/* ---- THE OPERATION COUNTER, and why a constant-time claim needs one ------
 *
 * A routine that says it is constant-time and has no way to be caught lying is
 * worth very little: the transformations below are the kind that a later edit
 * undoes by accident -- one early return put back for speed and the property
 * is gone with nothing failing. So the limb-level primitives count themselves
 * when P256_COUNT is defined, and test/srv/cttest.c drives the public entry
 * points with scalars chosen to differ in exactly the ways that show up here
 * (leading zeros, Hamming weight, small values) and asserts that the count is
 * IDENTICAL. That is not a timing measurement -- it cannot see a
 * data-dependent memory access or a variable-latency instruction -- but every
 * leak this file has had was a branch that did fewer limb operations, and
 * this sees those exactly.
 *
 * Compiled out entirely by default: no counter, no global, no store in the
 * hot loop. */
#ifdef P256_COUNT
unsigned long p256_op_count;
#define P256_TICK() (p256_op_count++)
#else
#define P256_TICK() ((void)0)
#endif

/* ---- 256-bit little-endian limb arithmetic ---- */
static int u256_iszero(const struct u256 *a)
{
   return (a->v[0] | a->v[1] | a->v[2] | a->v[3]) == 0;
}

/* r = c ? b : a, limb by limb and without a branch. c must be 0 or 1. */
static void u256_cmov(struct u256 *a, const struct u256 *b, uint64_t c)
{
   P256_TICK();
   for (int i = 0; i < 4; i++)
      a->v[i] = ct_cmov64(a->v[i], b->v[i], c);
}

/* 1 iff x < y, in constant time. The 128-bit subtraction borrows into bit 127
 * exactly when it went negative, and both operands are 64 bits wide so the
 * wrapped difference can never reach that bit by accident. */
static uint64_t u64_lt(uint64_t x, uint64_t y)
{
   uint64_t d = 0;
   return subb64(x, y, 0, &d); /* the borrow IS "it went negative" */
}

/* -1, 0 or 1, in time that does not depend on WHERE the two values differ.
 *
 * Walking the limbs from the top down and returning at the first difference
 * makes the comparison's cost a function of how many leading limbs matched.
 * That is the same shape of leak as memcmp on a MAC, except that the operands
 * here are secrets rather than guesses -- it is reached from every modular
 * reduction, so a scalar multiplication runs it thousands of times per
 * signature.
 *
 * Constant time instead: walk from the LEAST significant limb upwards and let
 * each differing limb overwrite the verdict, so the most significant
 * difference is the one left standing. Same answer, no early exit. */
static int u256_cmp(const struct u256 *a, const struct u256 *b)
{
   uint64_t lt = 0;
   uint64_t gt = 0;
   P256_TICK();
   for (int i = 0; i < 4; i++) {
      const uint64_t l  = u64_lt(a->v[i], b->v[i]);
      const uint64_t g  = u64_lt(b->v[i], a->v[i]);
      const uint64_t ne = ct_nz64(a->v[i] ^ b->v[i]);
      lt                = ct_cmov64(lt, l, ne);
      gt                = ct_cmov64(gt, g, ne);
   }
   return (int)gt - (int)lt;
}

/* 1 iff a == b, in time that does not depend on which limb differs. Separate
 * from u256_cmp because the callers below want a MASKABLE 0/1 rather than an
 * ordering, and because folding all four limbs into one accumulator is both
 * cheaper and harder to get subtly wrong than reading a sign. */
static uint64_t u256_eq1(const struct u256 *a, const struct u256 *b)
{
   uint64_t d = 0;
   P256_TICK();
   for (int i = 0; i < 4; i++)
      d |= a->v[i] ^ b->v[i];
   return 1U - ct_nz64(d);
}

static uint64_t u256_add(struct u256 *r, const struct u256 *a,
                         const struct u256 *b)
{ /* returns carry */
   uint64_t c = 0;
   P256_TICK();
   for (int i = 0; i < 4; i++)
      c = addc64(a->v[i], b->v[i], c, &r->v[i]);
   return c;
}

static uint64_t u256_sub(struct u256 *r, const struct u256 *a,
                         const struct u256 *b)
{ /* returns borrow */
   uint64_t br = 0;
   P256_TICK();
   for (int i = 0; i < 4; i++)
      br = subb64(a->v[i], b->v[i], br, &r->v[i]);
   return br;
}

static void u256_from_be(struct u256 *r, const uint8_t b[32])
{
   for (int i = 0; i < 4; i++) {
      const uint8_t *p = b + ((size_t)(3 - i) * 8);
      r->v[i]          = ((uint64_t)p[0] << 56U) | ((uint64_t)p[1] << 48U) |
                         ((uint64_t)p[2] << 40U) | ((uint64_t)p[3] << 32U) |
                         ((uint64_t)p[4] << 24U) | ((uint64_t)p[5] << 16U) |
                         ((uint64_t)p[6] << 8U) | (uint64_t)p[7];
   }
}

static void u256_to_be(const struct u256 *a, uint8_t b[32])
{
   for (int i = 0; i < 4; i++) {
      uint8_t *p = b + ((size_t)(3 - i) * 8);
      uint64_t x = a->v[i];
      for (int j = 0; j < 8; j++)
         p[j] = (uint8_t)(x >> (unsigned)(56 - (8 * j)));
   }
}

/* See p256.h: the one canonical encoder, exported because the layout it
 * encodes is this file's. u256_to_be is the internal spelling and stays
 * static -- callers get the scalar-shaped name, which is what they mean. */
void p256_sc_to_be(const struct u256 *a, uint8_t be[32])
{
   u256_to_be(a, be);
}

static int u256_bit(const struct u256 *a, int i)
{
   return (int)((a->v[(unsigned)i >> 6U] >> ((unsigned)i & 63U)) & 1U);
}

static uint64_t u256_shl1(struct u256 *a, int carry_in)
{ /* a = (a<<1)|carry_in; returns bit shifted out */
   uint64_t c = (unsigned)carry_in & 1U;
   for (int i = 0; i < 4; i++) {
      uint64_t nc = a->v[i] >> 63U;
      a->v[i]     = (a->v[i] << 1U) | c;
      c           = nc;
   }
   return c;
}

/* 256x256 -> 512 (as struct u256 lo, hi) */
static void u256_mul(const struct u256 *a, const struct u256 *b,
                     struct u256 *lo, struct u256 *hi)
{
   uint64_t r[8] = {0};
   P256_TICK();
   for (int i = 0; i < 4; i++) {
      uint64_t carry = 0;
      for (int j = 0; j < 4; j++) {
         /* t = a[i]*b[j] + r[i+j] + carry, as a 128-bit value in two words.
          * NEITHER ADDITION CAN OVERFLOW THE PAIR: the product is at most
          * (2^64-1)^2, and adding two more 64-bit words keeps it under
          * 2^128 -- (2^64-1)^2 + 2*(2^64-1) = 2^128 - 1 exactly. */
         /* ph/pl, not hi/lo: those are the OUTPUTS. */
         uint64_t ph = 0;
         uint64_t pl = 0;
         mul64(a->v[i], b->v[j], &ph, &pl);
         uint64_t c1 = addc64(pl, r[i + j], 0, &pl);
         uint64_t c2 = addc64(pl, carry, 0, &pl);
         r[i + j]    = pl;
         carry       = ph + c1 + c2;
      }
      r[i + 4] += carry;
   }
   for (int i = 0; i < 4; i++) {
      lo->v[i] = r[i];
      hi->v[i] = r[i + 4];
   }
}

/* reduce a 512-bit value (lo,hi) mod m, bitwise. Result < m.
 *
 * THE SUBTRACTION IS UNCONDITIONAL AND THE CHOICE IS A SELECT. This is the
 * whole of the scalar arithmetic mod n -- p256_sc_mul, and through it the
 * 512 squarings and multiplies inside p256_sc_inv -- so it is where an ECDSA
 * nonce spends most of its life. Written as `if (co || r >= m) subtract`, the
 * routine did 512 iterations whose individual costs spelled out the bits of
 * the intermediate values, which are k and k-derived. Computing r-m every time
 * and keeping it only when it was wanted costs one extra 4-limb subtraction
 * per iteration and removes the dependence entirely.
 *
 * The condition is spelled with the borrow rather than a comparison because
 * u256_sub already produces it: borrow==0 is exactly r >= m. */
static void mod512(const struct u256 *lo, const struct u256 *hi,
                   const struct u256 *m, struct u256 *out)
{
   struct u256 r = {
       {0, 0, 0, 0}
   };
   for (int i = 511; i >= 0; i--) {
      int bit = (i < 256) ? u256_bit(lo, i) : u256_bit(hi, i - 256);
      uint64_t co =
          u256_shl1(&r, bit); /* co==1 means value overflowed 256 bits */
      struct u256 t;
      const uint64_t br = u256_sub(&t, &r, m);
      u256_cmov(&r, &t, co | (br ^ 1U));
   }
   *out = r;
}

/* ---- modulus context (p for field, n for scalars) ---- */
static struct u256 field_p, order_n;
static struct u256 zero = {
    {0, 0, 0, 0}
};
static struct u256 one = {
    {1, 0, 0, 0}
};

static void modmul(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 lo;
   struct u256 hi;
   u256_mul(a, b, &lo, &hi);
   mod512(&lo, &hi, m, r);
}

/* Both of these reduce with a select rather than a branch, for the reason
 * given above mod512: the operands are secrets, and jdouble and p256_padd call
 * them a dozen times per ladder step. The corrected candidate is always
 * computed; only which one is kept changes. */
static void modadd(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 t;
   struct u256 u;
   const uint64_t c  = u256_add(&t, a, b);
   const uint64_t br = u256_sub(&u, &t, m); /* borrow==0 means t >= m */
   *r                = t;
   u256_cmov(r, &u, c | (br ^ 1U));
}

static void modsub(struct u256 *r, const struct u256 *a, const struct u256 *b,
                   const struct u256 *m)
{
   struct u256 t;
   struct u256 u;
   const uint64_t br = u256_sub(&t, a, b);
   u256_add(&u, &t, m);
   *r = t;
   u256_cmov(r, &u, br);
}

/* How to multiply two residues of whichever modulus is being inverted. The
 * multiply is a parameter because it is the whole cost of an inversion -- 384
 * of them -- and the field has a fast reduction that the curve order, which is
 * not a Solinas prime, cannot use. Passing the generic multiply for both was
 * costing about 26 ms per inversion on the board. */
typedef void (*mulfn)(struct u256 *r, const struct u256 *a,
                      const struct u256 *b);

/* a^(m-2) mod m: an inverse by Fermat, valid because both moduli are prime.
 *
 * The `if (u256_bit(&e, i))` below looks like the secret-dependent branch this
 * file has been purged of, and it is not: e is m-2 for a fixed modulus, so the
 * sequence of squarings and multiplies is the same for every input and only the
 * operands are secret. Left as a branch on purpose -- masking it would cost 256
 * multiplies to hide a public constant. */
static void modinv(struct u256 *r, const struct u256 *a, const struct u256 *m,
                   mulfn mul)
{
   struct u256 e;
   struct u256 two = {
       {2, 0, 0, 0}
   };
   u256_sub(&e, m, &two); /* e = m-2 */
   struct u256 res  = one;
   struct u256 base = *a;
   for (int i = 0; i < 256; i++) {
      if (u256_bit(&e, i)) {
         struct u256 t;
         mul(&t, &res, &base);
         res = t;
      }
      struct u256 t;
      mul(&t, &base, &base);
      base = t;
   }
   *r = res;
}

/* field (mod p) shortcuts */
static void fadd(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modadd(r, a, b, &field_p);
}

static void fsub(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modsub(r, a, b, &field_p);
}

/* Gather a 256-bit value out of eight 32-bit words of the product, most
 * significant word first; a negative index contributes a zero word. */
static void gather(struct u256 *r, const uint32_t *c, const signed char *idx)
{
   r->v[0] = r->v[1] = r->v[2] = r->v[3] = 0;
   for (unsigned i = 0; i < 8; i++) {
      unsigned j = 7 - i; /* which 32-bit word of the result idx[i] lands in */
      uint64_t w = idx[i] < 0 ? 0 : c[(unsigned)idx[i]];
      r->v[j / 2] |= w << ((j % 2) * 32);
   }
}

/* Reduce a 512-bit product modulo p = 2^256 - 2^224 + 2^192 + 2^96 - 1.
 *
 * mod512() would do it by long division: 512 rounds of shift, compare and
 * subtract. One scalar multiplication pushes several thousand field multiplies
 * through here, which is why a handshake cost a second on the board.
 *
 * p is a Solinas prime, so every power of two above 2^255 folds back onto a
 * few 32-bit word positions, and the reduction becomes nine 256-bit terms --
 * each just a shuffle of the product's words -- added and subtracted (FIPS
 * 186-4, D.2.3). Same answer, about a hundredth of the work. */
static void fast_reduce(struct u256 *out, const struct u256 *lo,
                        const struct u256 *hi)
{
   /* The nine terms, most significant 32-bit word first; -1 is a zero word. */
   static const signed char term[9][8] = {
       {7,  6,  5,  4,  3,  2,  1,  0 },
       {15, 14, 13, 12, 11, -1, -1, -1},
       {-1, 15, 14, 13, 12, -1, -1, -1},
       {15, 14, -1, -1, -1, 10, 9,  8 },
       {8,  13, 15, 14, 13, 11, 10, 9 },
       {10, 8,  -1, -1, -1, 13, 12, 11},
       {11, 9,  -1, -1, 15, 14, 13, 12},
       {12, -1, 10, 9,  8,  15, 14, 13},
       {13, -1, 11, 10, 9,  -1, 15, 14},
   };
   /* d = s1 + 2*s2 + 2*s3 + s4 + s5 - s6 - s7 - s8 - s9 */
   static const signed char weight[9] = {1, 2, 2, 1, 1, -1, -1, -1, -1};

   uint32_t c[16];
   for (size_t i = 0; i < 4; i++) {
      c[2 * i]           = (uint32_t)lo->v[i];
      c[(2 * i) + 1]     = (uint32_t)(lo->v[i] >> 32U);
      c[8 + (2 * i)]     = (uint32_t)hi->v[i];
      c[8 + (2 * i) + 1] = (uint32_t)(hi->v[i] >> 32U);
   }

   /* The running value is top*2^256 + r, with top a small signed count of
    * whole 2^256 overflows; the corrections below drive top back to zero.
    *
    * top is int64_t so that its sign can be extracted by a shift rather than a
    * comparison: `(uint64_t)top >> 63` is defined by the conversion to
    * unsigned, whereas `top < 0` leaves it to the compiler whether the answer
    * arrives as a flag or as a jump, and top is secret-derived. */
   struct u256 r = zero;
   int64_t top   = 0;
   for (int i = 0; i < 9; i++) {
      struct u256 s;
      gather(&s, c, term[i]);
      for (int k = 0; k < (weight[i] < 0 ? -weight[i] : weight[i]); k++) {
         struct u256 t;
         if (weight[i] > 0)
            top += (int64_t)u256_add(&t, &r, &s);
         else
            top -= (int64_t)u256_sub(&t, &r, &s);
         r = t;
      }
   }

   /* THE CORRECTION RUNS A FIXED NUMBER OF TIMES.
    *
    * As two `while` loops their trip counts are a direct readout of the
    * product's magnitude, and this is the hottest such site in the file --
    * every field multiply in every ladder step passes through here. Measured
    * over 200 signatures plus the 4000 boundary products in p256_selftest,
    * `top` lands in [-4, 4] and each loop would run at most 4 times: a cost
    * varying by up to eight 4-limb add/subtracts with the operands.
    *
    * The counts below are the analytic worst case, not the measured one, and
    * both loops are no-ops once their condition goes false, so overshooting is
    * free of consequence and only costs time:
    *
    *   ADDS: the four negatively weighted terms can borrow at most once each,
    *   so the value V = top*2^256 + r is greater than -4*2^256. p = 2^256 - d
    *   with d just over 2^224, so V + 5p > 2^256 - 5d > 0: five additions of p
    *   always bring V to zero or above, and V >= 0 is exactly top >= 0.
    *
    *   SUBTRACTS: the seven positively weighted additions can carry at most
    *   once each and the first cannot carry at all, so V <= 6*2^256 + (2^256-1)
    *   < 7*2^256 = 7p + 7d, and floor(V/p) <= 7. Each iteration whose
    *   condition holds removes exactly one p and leaves V >= 0, so seven are
    *   enough to reach V < p; eight is the margin.
    *
    * Both phases end with top == 0 and r < p, which is the unique canonical
    * residue -- the same answer the loops produced, which is what p256_selftest
    * checks against the long division in mod512. */
   for (int i = 0; i < 5; i++) {
      struct u256 t;
      const uint64_t carry = u256_add(&t, &r, &field_p);
      const uint64_t neg   = (uint64_t)top >> 63U;
      u256_cmov(&r, &t, neg);
      top += (int64_t)(carry & neg);
   }
   for (int i = 0; i < 8; i++) {
      struct u256 t;
      const uint64_t br = u256_sub(&t, &r, &field_p); /* borrow==0: r >= p */
      const uint64_t pos =
          ct_nz64((uint64_t)top) & (((uint64_t)top >> 63U) ^ 1U);
      const uint64_t take = pos | (br ^ 1U);
      u256_cmov(&r, &t, take);
      top -= (int64_t)(br & take);
   }
   *out = r;
}

static void fmul(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   struct u256 lo;
   struct u256 hi;
   u256_mul(a, b, &lo, &hi);
   fast_reduce(r, &lo, &hi);
}

static void finv(struct u256 *r, const struct u256 *a)
{
   modinv(r, a, &field_p, fmul);
}

/* ---- curve ---- */
static struct u256 curve_b;

/* THE GENERATOR IS PRIVATE, AND SO IS THE FACT THAT IT IS READY.
 *
 * It was an extern `struct jpoint` named for this curve -- a WRITABLE
 * global, filled at
 * runtime by p256_init and read directly by J-PAKE, so every file that
 * included p256.h could overwrite the base point of the curve or read it
 * before anything had put a curve in it. Neither failure announces itself:
 * an overwritten G still produces points, still passes the on-curve test if
 * the attacker picked a curve point, and the zero-knowledge proofs J-PAKE
 * builds over it still verify against the same wrong G at the other end of
 * the exchange. A G of all zeroes -- the pre-init state -- is the point at
 * infinity, and every scalar multiple of it is infinity too.
 *
 * So: static here, handed out only as a COPY, and only once init has run.
 * `curve_ready` is the guard, and it is what makes p256_init idempotent as
 * well -- it is called from four entry points (the driver, the TLS
 * credentials, jpake_init, and the tests) and none of them knows about the
 * others. */
static struct jpoint curve_g;
static int curve_ready;

int p256_gen(struct jpoint *out)
{
   if (!out)
      return 0;
   if (!curve_ready) {
      /* ZEROED, WHICH IS INFINITY. A caller that ignores the answer gets a
       * point every downstream check refuses (p256_to_xy, validate_zkp)
       * rather than one that looks usable. */
      struct jpoint z = {0};
      *out            = z;
      return 0;
   }
   *out = curve_g;
   return 1;
}

void p256_init(void)
{
   if (curve_ready)
      return;
   static const uint8_t p_be[32] = {
       0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0,    0,    0,
       0,    0,    0,    0,    0,    0,    0,    0,    0,    0xff, 0xff,
       0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
   static const uint8_t n_be[32] = {
       0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
       0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
       0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51};
   static const uint8_t b_be[32] = {
       0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7, 0xb3, 0xeb, 0xbd,
       0x55, 0x76, 0x98, 0x86, 0xbc, 0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53,
       0xb0, 0xf6, 0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b};
   static const uint8_t gx[32] = {
       0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6,
       0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb,
       0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96};
   static const uint8_t gy[32] = {
       0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb,
       0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31,
       0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};
   u256_from_be(&field_p, p_be);
   u256_from_be(&order_n, n_be);
   u256_from_be(&curve_b, b_be);
   u256_from_be(&curve_g.X, gx);
   u256_from_be(&curve_g.Y, gy);
   curve_g.Z = one;
   /* LAST, AFTER EVERY CONSTANT IS IN PLACE. p256_gen and p256_mul_g both
    * read this flag, and a reader that saw it set while the generator was
    * still half-written would get a point that is not on the curve. */
   curve_ready = 1;
}

/* Jacobian doubling, a = -3.
 *
 * THERE IS NO EARLY RETURN FOR INFINITY, AND THAT IS THE POINT.
 *
 * Beginning `if (Z == 0 || Y == 0) return infinity;` costs almost nothing
 * while the general case costs nine field multiplies. p256_mul starts its
 * accumulator at infinity, so a scalar with z leading zero bits takes the
 * cheap path for its first z ladder steps -- and the number of leading zeros
 * of an ECDSA nonce is precisely the quantity the lattice attack on partial
 * nonce leakage needs. It is the loudest leak this file can have: measured
 * on the branching version, 49206 limb operations falling to 14122 as the
 * top set bit moves from bit 255 to bit 40 -- a 3.5x readout of the
 * leading-zero count.
 *
 * Having no branch is exact rather than approximate, because the general
 * formula computes the right answer for both cases such a branch catches:
 *
 *   Z == 0 (infinity): Z' = 2YZ = 0, so the result is again a Z == 0 point.
 *   Infinity is represented by Z == 0 alone -- p256_is_inf, p256_padd,
 *   p256_to_xy and p256_eq all test only that -- so the X and Y that come out
 *   are meaningless but harmless, exactly as a returned (1, 1, 0) would be.
 *
 *   Y == 0 with Z != 0 would be a point of order two. P-256's group order is
 *   an odd prime, so no such point exists on the curve and p256_from_xy will
 *   not admit one; if it somehow arrived, Z' = 2YZ = 0 is still the
 *   mathematically correct answer.
 *
 * What remains is that the FIRST addition in the ladder still meets an
 * infinite accumulator and takes p256_padd's cheap path -- but exactly one
 * addition does, whatever the scalar, so it carries no positional
 * information. */
static void jdouble(struct jpoint *r, const struct jpoint *p)
{
   struct u256 a;
   struct u256 bv;
   struct u256 c;
   struct u256 d;
   struct u256 t;
   struct u256 t2;
   struct u256 z2;
   struct u256 z4;
   fmul(&z2, &p->Z, &p->Z);
   fmul(&z4, &z2, &z2);
   fmul(&a, &p->X, &p->X); /* X^2 */
   struct u256 three_x2;
   fadd(&three_x2, &a, &a);
   fadd(&three_x2, &three_x2, &a); /* 3X^2 */
   struct u256 three_z4;
   fadd(&three_z4, &z4, &z4);
   fadd(&three_z4, &three_z4, &z4); /* 3Z^4 */
   struct u256 m;
   fsub(&m, &three_x2, &three_z4); /* M = 3X^2 - 3Z^4  (a=-3) */
   fmul(&t, &p->Y, &p->Y);         /* Y^2 */
   fmul(&bv, &p->X, &t);
   fadd(&bv, &bv, &bv);
   fadd(&bv, &bv, &bv); /* S = 4*X*Y^2 */
   fmul(&c, &t, &t);
   fadd(&c, &c, &c);
   fadd(&c, &c, &c);
   fadd(&c, &c, &c); /* 8*Y^4 */
   fmul(&d, &m, &m);
   struct u256 two_b;
   fadd(&two_b, &bv, &bv);
   fsub(&d, &d, &two_b); /* X' = M^2 - 2S */
   fsub(&t2, &bv, &d);
   fmul(&t2, &m, &t2);
   fsub(&t2, &t2, &c); /* Y' = M(S-X') - 8Y^4 */
   struct u256 zr;
   fmul(&zr, &p->Y, &p->Z);
   fadd(&zr, &zr, &zr); /* Z' = 2YZ */
   r->X = d;
   r->Y = t2;
   r->Z = zr;
}

/* r = c ? b : a, all three coordinates, no branch. c must be 0 or 1. */
static void jpoint_cmov(struct jpoint *a, const struct jpoint *b, uint64_t c)
{
   u256_cmov(&a->X, &b->X, c);
   u256_cmov(&a->Y, &b->Y, c);
   u256_cmov(&a->Z, &b->Z, c);
}

/* Jacobian add, BRANCH-FREE over all five outcomes.
 *
 * The general formula alone is not enough and never was: it gives
 * Z3 = Z1*Z2*h, which is zero when either input is infinity -- so it answers
 * infinity for "infinity + Q" where the answer is Q -- and it divides by
 * nothing but produces 0/0 in the shape of h == 0 when the two points are
 * equal or opposite. So all five answers are COMPUTED and then selected:
 *
 *   P infinite         -> Q
 *   Q infinite         -> P
 *   u1 == u2, s1 == s2 -> 2P   (the same point; the general formula
 * degenerates) u1 == u2, s1 != s2 -> infinity  (opposite points) otherwise ->
 * the general formula
 *
 * The selects run in that order of increasing priority, so an infinite operand
 * wins over everything -- which is what makes "infinity + infinity" come out
 * as infinity rather than as the doubling case it also matches.
 *
 * THE COST is a doubling on top of every addition: about 27 field multiplies
 * where the branching version did 16, and it is paid on every addition
 * including the ones inside p256_mul. That is the price of the property, and
 * p256.h quotes what it comes to end to end. What it buys is that the
 * exception cases are not observable. An attacker cannot steer them inside the
 * ladder (the accumulator is an even multiple of P), but jpake.c and
 * ecdsa_p256_verify add points an attacker chooses, and there the shape of the
 * operands would be readable in the timing. */
void p256_padd(struct jpoint *r, const struct jpoint *P_,
               const struct jpoint *Q)
{
   struct u256 z1z1;
   struct u256 z2z2;
   struct u256 u1;
   struct u256 u2;
   struct u256 s1;
   struct u256 s2;
   struct u256 t;
   fmul(&z1z1, &P_->Z, &P_->Z);
   fmul(&z2z2, &Q->Z, &Q->Z);
   fmul(&u1, &P_->X, &z2z2);
   fmul(&u2, &Q->X, &z1z1);
   fmul(&t, &Q->Z, &z2z2);
   fmul(&s1, &P_->Y, &t);
   fmul(&t, &P_->Z, &z1z1);
   fmul(&s2, &Q->Y, &t);
   struct u256 h;
   struct u256 rr;
   struct u256 h2;
   struct u256 h3;
   struct u256 u1h2;
   struct u256 x3;
   struct u256 y3;
   struct u256 z3;
   fsub(&h, &u2, &u1);
   fsub(&rr, &s2, &s1);
   fmul(&h2, &h, &h);
   fmul(&h3, &h2, &h);
   fmul(&u1h2, &u1, &h2);
   fmul(&x3, &rr, &rr);
   fsub(&x3, &x3, &h3);
   struct u256 two_u1h2;
   fadd(&two_u1h2, &u1h2, &u1h2);
   fsub(&x3, &x3, &two_u1h2);
   fsub(&y3, &u1h2, &x3);
   fmul(&y3, &rr, &y3);
   struct u256 s1h3;
   fmul(&s1h3, &s1, &h3);
   fsub(&y3, &y3, &s1h3);
   fmul(&z3, &P_->Z, &Q->Z);
   fmul(&z3, &z3, &h);

   /* THE FIVE OUTCOMES, computed and then chosen. `dbl` is unconditional: it
    * is the one answer the general formula cannot produce, and computing it
    * only when it is needed is exactly the branch this routine exists without.
    */
   struct jpoint dbl;
   jdouble(&dbl, P_);
   struct jpoint inf;
   inf.X = one;
   inf.Y = one;
   inf.Z = zero;

   const uint64_t p_inf =
       1U - ct_nz64(P_->Z.v[0] | P_->Z.v[1] | P_->Z.v[2] | P_->Z.v[3]);
   const uint64_t q_inf =
       1U - ct_nz64(Q->Z.v[0] | Q->Z.v[1] | Q->Z.v[2] | Q->Z.v[3]);
   const uint64_t same_u = u256_eq1(&u1, &u2);
   const uint64_t same_s = u256_eq1(&s1, &s2);

   struct jpoint out;
   out.X = x3;
   out.Y = y3;
   out.Z = z3;
   jpoint_cmov(&out, &inf, same_u & (1U - same_s)); /* P + (-P) */
   jpoint_cmov(&out, &dbl, same_u & same_s);        /* P + P */
   jpoint_cmov(&out, P_, q_inf);
   jpoint_cmov(&out, Q, p_inf);
   *r = out;
}

/* k*P by double-AND-ADD, most significant bit first: 256 doublings and 256
 * additions, whatever k is.
 *
 * EVERY ITERATION DOES THE SAME WORK. The addition happens on a zero bit too
 * and its result is thrown away by a masked select rather than by an `if`, so
 * the cost of a scalar multiplication no longer depends on the scalar in any
 * way this file can express: not on its leading zeros (jdouble's infinity
 * shortcut went first), and now not on its Hamming weight either. The count
 * that test/srv/cttest.c asserts is one number for every scalar it tries.
 *
 * That last step is only sound because p256_padd is itself branch-free. An
 * always-add over a branching p256_padd moves the leak rather than closing
 * it: the discarded additions still take the cheap infinity path for the
 * leading zeros and the expensive one elsewhere.
 *
 * WHAT IT COSTS is one addition per bit rather than one per SET bit, each
 * carrying a doubling of its own -- about 2.4x the field multiplies of a
 * conditional-add ladder. p256.h has the end-to-end numbers.
 * The alternative to paying it is a windowed or Montgomery ladder, which is
 * less work per bit but needs a constant-time table read to stay honest, and
 * that is the one primitive lib/ct.h deliberately does not provide. */
void p256_mul(struct jpoint *r, const struct u256 *k, const struct jpoint *P_)
{
   struct jpoint acc;
   acc.X = one;
   acc.Y = one;
   acc.Z = zero; /* infinity */
   for (int i = 255; i >= 0; i--) {
      struct jpoint d;
      jdouble(&d, &acc);
      acc = d;
      struct jpoint a;
      p256_padd(&a, &acc, P_);
      jpoint_cmov(&acc, &a, (uint64_t)u256_bit(k, i));
   }
   *r = acc;
}

void p256_mul_g(struct jpoint *r, const struct u256 *k)
{
   /* THE GENERATOR THROUGH ITS OWN ACCESSOR, so this cannot outrun the
    * initialisation either: before p256_init the copy is infinity, k times
    * infinity is infinity, and p256_to_xy refuses it. That is the same
    * answer this always gave for an uninitialised curve -- what is new is
    * that it is now a stated one rather than a consequence of a global
    * happening to be zero. */
   struct jpoint g;
   (void)p256_gen(&g);
   p256_mul(r, k, &g);
}

int p256_to_xy(const struct jpoint *P_, uint8_t x[32], uint8_t y[32])
{
   if (u256_iszero(&P_->Z)) {
      /* ZERO the outputs on failure. The point at infinity has no affine
       * coordinates, and every caller passes an uninitialised stack buffer --
       * so leaving them untouched published whatever the stack last held.
       * jpake's cert_byteify ignored this return entirely and transmitted
       * the result, and a peer CAN force the failure: round 3 multiplies by
       * (pubA + peer_pub1 + peer_pub2), and a peer that picks pub2 =
       * -(pubA + pub1) makes that the point at infinity. Zeroing makes the
       * failure deterministic; callers that must not proceed check the
       * return. */
      memset(x, 0, 32);
      memset(y, 0, 32);
      return 0;
   }
   struct u256 zi;
   struct u256 zi2;
   struct u256 zi3;
   struct u256 ax;
   struct u256 ay;
   finv(&zi, &P_->Z);
   fmul(&zi2, &zi, &zi);
   fmul(&zi3, &zi2, &zi);
   fmul(&ax, &P_->X, &zi2);
   fmul(&ay, &P_->Y, &zi3);
   u256_to_be(&ax, x);
   u256_to_be(&ay, y);
   return 1;
}

int p256_eq(const struct jpoint *A, const struct jpoint *B)
{
   int ia = u256_iszero(&A->Z);
   int ib = u256_iszero(&B->Z);
   if (ia || ib)
      return ia && ib;
   uint8_t xa[32];
   uint8_t ya[32];
   uint8_t xb[32];
   uint8_t yb[32];
   p256_to_xy(A, xa, ya);
   p256_to_xy(B, xb, yb);
   /* Accumulate the difference rather than returning at the first one, so the
    * time taken does not say how many leading bytes agreed. Spelled out here
    * rather than calling ct_eq because several link lines compile this file
    * without lib/ct.c; see the note in ct.h. */
   unsigned d = 0;
   for (int i = 0; i < 32; i++)
      d |= ((unsigned)xa[i] ^ (unsigned)xb[i]) |
           ((unsigned)ya[i] ^ (unsigned)yb[i]);
   return d == 0;
}

int p256_from_xy(struct jpoint *P_, const uint8_t x[32], const uint8_t y[32])
{
   u256_from_be(&P_->X, x);
   u256_from_be(&P_->Y, y);
   P_->Z = one;
   if (u256_cmp(&P_->X, &field_p) >= 0 || u256_cmp(&P_->Y, &field_p) >= 0)
      return 0;
   /* on-curve: y^2 == x^3 - 3x + b */
   struct u256 y2;
   struct u256 x3;
   struct u256 t;
   struct u256 rhs;
   fmul(&y2, &P_->Y, &P_->Y);
   fmul(&x3, &P_->X, &P_->X);
   fmul(&x3, &x3, &P_->X);
   fadd(&t, &P_->X, &P_->X);
   fadd(&t, &t, &P_->X); /* 3x */
   fsub(&rhs, &x3, &t);
   fadd(&rhs, &rhs, &curve_b);
   return u256_cmp(&y2, &rhs) == 0;
}

void p256_uncompressed(const struct jpoint *P_, uint8_t out[65])
{
   out[0] = 0x04;
   p256_to_xy(P_, out + 1, out + 33);
}

/* scalars mod n */
/* One conditional subtraction is enough: the input is below 2^256 and
 * 2^256 - n is under 2^225, so t - n is always already below n. The
 * subtraction happens unconditionally and the select decides -- this is the
 * first thing ecdsa_p256_sign does to the private key and to the nonce, so a
 * branch here reports whether each of them happened to exceed the group
 * order. */
void p256_sc_from_be(struct u256 *r, const uint8_t be[32])
{
   struct u256 t;
   struct u256 u;
   u256_from_be(&t, be);
   const uint64_t br = u256_sub(&u, &t, &order_n); /* borrow==0: t >= n */
   *r                = t;
   u256_cmov(r, &u, br ^ 1U);
}

void p256_sc_mul(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modmul(r, a, b, &order_n);
}

void p256_sc_add(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modadd(r, a, b, &order_n);
}

void p256_sc_sub(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modsub(r, a, b, &order_n);
}

/* The curve order has no Solinas form, so its multiply is the generic one.
 * This runs once per signature, not once per point, so it stays as it is. */
static void nmul(struct u256 *r, const struct u256 *a, const struct u256 *b)
{
   modmul(r, a, b, &order_n);
}

void p256_sc_inv(struct u256 *r, const struct u256 *a)
{
   modinv(r, a, &order_n, nmul);
}

/* n - a, except that zero negates to zero rather than to n. The zero case is
 * selected rather than returned early: `a` is a secret scalar everywhere this
 * is called from, and while a zero scalar is not a value an honest caller
 * produces, "did the argument happen to be zero" is not a question the timing
 * should answer. */
void p256_sc_neg(struct u256 *r, const struct u256 *a)
{
   struct u256 t;
   u256_sub(&t, &order_n, a);
   *r = t;
   u256_cmov(r, a, (uint64_t)u256_iszero(a));
}

int p256_sc_is_zero(const struct u256 *a)
{
   return u256_iszero(a);
}

/* See p256.h for WHY this exists beside p256_sc_from_be rather than replacing
 * it. What belongs here is how it stays constant-time while answering three
 * different things.
 *
 * The two facts are computed with exactly the machinery the reductions above
 * use: u256_sub's borrow (1 iff t < n, read the same way p256_sc_from_be and
 * p256_sc_rand read it) and u256_iszero, both of which touch every limb
 * unconditionally. The STATUS is then selected with ct_cmov64 rather than
 * branched to -- not because the difference between "zero" and "out of range"
 * is worth hiding on its own, but because a `if (t >= n) return ...` here
 * would be a data-dependent branch in the first thing ecdsa_p256_sign does to
 * the private key, which is the precise shape of leak the rest of this file
 * spent an item removing. The refusal itself is visible to the caller and
 * that is deliberate; WHICH refusal is not visible in the timing.
 *
 * The two conditions are mutually exclusive -- 0 < n, so a zero value is
 * never also out of range -- so the order of the two selects does not
 * matter. */
enum p256_scalar p256_sc_from_be_checked(struct u256 *r, const uint8_t be[32])
{
   struct u256 t;
   struct u256 diff;
   u256_from_be(&t, be);
   const uint64_t lt  = u256_sub(&diff, &t, &order_n); /* borrow==1: t < n */
   const uint64_t z   = (uint64_t)u256_iszero(&t);
   const uint64_t bad = ct_nz64((lt ^ 1U) | z);

   *r = t;
   u256_cmov(r, &zero, bad); /* zeroed on refusal; see the header */

   uint64_t st = (uint64_t)P256_SCALAR_OK;
   st          = ct_cmov64(st, (uint64_t)P256_SCALAR_ZERO, z);
   st          = ct_cmov64(st, (uint64_t)P256_SCALAR_RANGE, lt ^ 1U);
   return (enum p256_scalar)st;
}

/* ---- THE ONE SECRET-SCALAR GENERATOR ----------------------------------
 *
 * See p256.h for the contract and for the two defects a per-call-site
 * generator has. What belongs here is why this one is shaped as it is.
 *
 * WHY REJECTION AND NOT A REDUCTION. The obvious two lines -- 32 bytes from
 * rand_bytes, then p256_sc_from_be to fold them into [0, n-1] -- fold with
 * one conditional subtraction, so the 2^256 - n values
 * at the top of the range land on the 2^256 - n residues at the BOTTOM, and
 * those residues come out twice as often as every other. 2^256 - n is
 * 0xffffffff00000000000000004319055258e8617b0c46353d039cdaaf, just over
 * 2^223, so the doubled band is a 2^-32 fraction of the interval: a scalar
 * falls in it with probability 2^-31 rather than 2^-32. Nobody attacks P-256
 * with that. It is still measurable, it is still a deviation from the uniform
 * scalar every proof about ECDSA and J-PAKE assumes, and rejection costs
 * nothing (below), so there is no reason to keep it.
 *
 * ZERO IS THE HALF THAT MATTERS. p256_sc_from_be maps both 0 and n to 0 and
 * hands it back as a perfectly ordinary scalar. What that means for somebody
 * whose key it is:
 *
 *   - a zero TLS ECDHE scalar (srv/tls.c) makes our key share the point at
 *     infinity and the ECDHE shared point the identity, so the handshake
 *     secret is a constant that anybody watching can compute;
 *   - a zero J-PAKE proof nonce (lib/jpake.c's vA/vB/v3) collapses the
 *     Schnorr proof `ran - H*priv` to `-H*priv`, which is the WITNESS times a
 *     value the verifier computed itself -- publishing the proof publishes
 *     the private scalar;
 *   - a zero ECDSA nonce (lib/ecdsa.c's k) is refused there and always was,
 *     which is why that site is the odd one out.
 *
 * In this tree every one of those is caught downstream today, by an accident
 * worth naming rather than relying on: an infinite point has no affine
 * encoding, so p256_to_xy refuses it, and jpake's cert_byteify and tls.c's
 * shared-secret step both fail before anything is transmitted. That is
 * defence in depth belonging to two unrelated functions, and it is not what
 * anybody reading `p256_sc_from_be(&d, priv)` would have concluded. The rule
 * lives here now.
 *
 * HOW MANY DRAWS. A 32-byte draw is out of range with probability
 * (2^256 - n)/2^256 = 2.33e-10, i.e. 2^-32 exactly to four figures, and zero
 * with probability 2^-256. Expected iterations 1.000000000233 -- for every
 * practical purpose this is a single draw, and no caller needs to budget for
 * more.
 *
 * WHY THE CAP EXISTS ANYWAY, and where the number comes from. With a working
 * source, P256_SC_RAND_TRIES consecutive rejections has probability
 * (2^-32)^64 = 2^-2048; there is no entropy source and no operating lifetime
 * for which that is a case to design for. The cap is not for bad luck, it is
 * for a source that is BROKEN -- stuck at 0xff..ff, returning a constant, an
 * emulator's stub -- and against that the difference between a bounded
 * failure and `while (1)` is the difference between a handshake that fails
 * and a process that stops answering. A caller can act on the first one.
 *
 * WHAT LEAKS. The loop count depends on the values that were THROWN AWAY, and
 * a rejected draw is independent of the one returned, so the timing says
 * nothing about the scalar handed back. The two tests are therefore ordinary
 * branches rather than ct_cmov64 selects; that is a deliberate exception to
 * the rule at the top of this file, not an oversight. The comparison itself
 * borrows u256_sub's borrow the way p256_sc_from_be does, so there is no new
 * secret-dependent compare here either.
 *
 * BEFORE p256_init() this refuses. order_n is zero until then, `t - 0` never
 * borrows, so every draw looks out of range and the cap returns 0. A
 * generator that quietly produced scalars against a zero group order would be
 * worse than one that fails, so that is left as it falls out. */
int p256_sc_rand(uint8_t out[32])
{
   /* ZEROED FIRST, so that every failure return below leaves the same value
    * behind. rand.h says a failed draw leaves the buffer UNDEFINED, and the
    * whole point of a refusal here is that it must not turn into a
    * caller keying with stack contents. Zero is the deliberate choice: it is
    * the one scalar that every consumer in this repo independently refuses
    * (ecdsa_p256_sign's is_zero check, and p256_to_xy on the infinite point
    * everywhere else), so a caller that ignores the return code fails hard
    * downstream rather than proceeding with something that looks like a key. */
   memset(out, 0, 32);
   for (int i = 0; i < P256_SC_RAND_TRIES; i++) {
      uint8_t b[32];
      struct u256 t;
      struct u256 diff;
      /* NOT a rejection: the source itself failed. Redrawing from a source
       * that just said no is how a caller ends up spinning, and rand.h's
       * contract is that this is fatal. Propagate immediately. */
      if (!rand_bytes(b, sizeof b))
         return 0;
      u256_from_be(&t, b);
      /* borrow == 1 means t < n, read exactly as in p256_sc_from_be. */
      if (u256_sub(&diff, &t, &order_n) == 0 || u256_iszero(&t)) {
         /* REJECTED, AND WIPED ON THE WAY OUT. t >= n is the biased tail and
          * t == 0 is the invalid scalar; both are draws from the same source
          * as the accepted one, and a rejected secret left on the stack is
          * exactly as readable as an accepted one. The next iteration
          * overwrites these, but the LAST iteration does not. */
         ct_wipe(b, sizeof b);
         ct_wipe(&t, sizeof t);
         ct_wipe(&diff, sizeof diff);
         continue;
      }
      /* `b` is already the canonical big-endian encoding of a value in
       * [1, n-1] -- that is what the two tests just established -- so there
       * is nothing to reduce and nothing to re-serialise. */
      memcpy(out, b, 32);
      /* AND THE COPIES ON THE STACK GO. `b` and `t` hold the private scalar
       * this function exists to produce -- an ECDSA nonce or a J-PAKE secret
       * -- and a stack frame is not private memory: it is reused by whatever
       * runs next, and it is in the core dump, the crash report and the
       * hibernation image. The caller wipes its own copy when it is done;
       * these are the ones nobody else can reach to clear.
       *
       * ct_wipe rather than memset because nothing reads them afterwards,
       * which is precisely the condition under which an optimiser deletes a
       * memset. Every loop exit wipes -- the `continue` paths above draw into
       * the same buffers next time round and the final one falls out of scope
       * with the rejected draw still in it. */
      ct_wipe(b, sizeof b);
      ct_wipe(&t, sizeof t);
      ct_wipe(&diff, sizeof diff);
      return 1;
   }
   return 0;
}
