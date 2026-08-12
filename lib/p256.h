// SPDX-License-Identifier: GPL-3.0
// p256.h --- Portable NIST P-256 (API)
// Copyright 2026 Jakob Kastelic

/* NIST P-256 (secp256r1): the field, scalar and point operations this repo
 * needs. No external dependencies.
 *
 * NOT CONSTANT-TIME, AND THE CALLERS ARE NO LONGER ALL FRIENDLY.
 *
 * This said "a personal reader talking to its own sensor, not an adversarial
 * setting", which was true when the only caller was the Dexcom J-PAKE over a
 * BLE link to a sensor on the user's own arm. It is no longer the whole
 * picture: srv/tls.c signs the server's LONG-TERM key with ecdsa_p256_sign on
 * every full TLS handshake, and any stranger on the internet can force those
 * by declining to present a session ticket.
 *
 * What leaks, concretely:
 *   - p256_mul is double-and-add with `if (bit)`, so the work per scalar bit
 *     depends on the bit;
 *   - p256_padd branches on point equality and on infinity;
 *   - fast_reduce ends with a data-dependent conditional-subtract loop.
 *
 * The theoretical attack is lattice recovery of the long-term key from
 * partial nonce leakage across many signatures. It is not known to be
 * practical here -- the operation is ~170 ms, remote timing of it is very
 * noisy, and nothing has been measured -- but the honest statement is that
 * this code was written for a threat model it is no longer only used in, and
 * that making it constant-time means rewriting the ladder, the addition and
 * the reduction, not adding a flag.
 *
 * REQUIRES a 64x64 -> 128 multiply (GCC/Clang `unsigned __int128`); p256.c
 * refuses to compile without one, so 32-bit targets and MSVC are out.
 *
 * If you are copying this file: it is fine for a J-PAKE against your own
 * device, and it is not what you want behind a public TLS endpoint. */
#ifndef DEX_P256_H
#define DEX_P256_H
#include <stdint.h>

struct u256 { /* little-endian 64-bit limbs */
   uint64_t v[4];
};

struct jpoint { /* Jacobian; Z==0 => infinity */
   struct u256 X, Y, Z;
};

void p256_init(void); /* call once */

/* Checks the fast field reduction against the generic long division it
 * replaces; 0 if they agree. For the test suite -- call after p256_init. */
int p256_selftest(void);

/* scalars mod n (curve order) */
void p256_sc_from_be(struct u256 *r,
                     const uint8_t be[32]); /* load + reduce mod n */
void p256_sc_mul(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_add(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_sub(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_neg(struct u256 *r, const struct u256 *a);
void p256_sc_inv(struct u256 *r,
                 const struct u256 *a); /* modular inverse mod n */
int p256_sc_is_zero(const struct u256 *a);

/* points */
extern struct jpoint p256_g;
void p256_mul(struct jpoint *r, const struct u256 *k,
              const struct jpoint *P);                   /* k*P */
void p256_mul_g(struct jpoint *r, const struct u256 *k); /* k*G */
void p256_padd(struct jpoint *r, const struct jpoint *P,
               const struct jpoint *Q);
int p256_is_inf(const struct jpoint *P);
int p256_eq(const struct jpoint *A, const struct jpoint *B);
/* affine X||Y (32+32); returns 1 ok, 0 if infinity / not on curve */
int p256_to_xy(const struct jpoint *P, uint8_t x[32], uint8_t y[32]);
int p256_from_xy(struct jpoint *P, const uint8_t x[32], const uint8_t y[32]);
void p256_uncompressed(const struct jpoint *P, uint8_t out[65]); /* 04||X||Y */

#endif
