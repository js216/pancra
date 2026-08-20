// SPDX-License-Identifier: GPL-3.0
// p256.h --- Portable NIST P-256 (API)
// Copyright 2026 Jakob Kastelic

/* NIST P-256 (secp256r1): the field, scalar and point operations this repo
 * needs. No external dependencies.
 *
 * PARTLY CONSTANT-TIME. THE SCALAR'S HIGH BITS NO LONGER LEAK; ITS HAMMING
 * WEIGHT STILL DOES.
 *
 * This header once said "a personal reader talking to its own sensor, not an
 * adversarial setting", which was true when the only caller was the Dexcom
 * J-PAKE over a BLE link to a sensor on the user's own arm. It is not the whole
 * picture: srv/tls.c signs the server's LONG-TERM key with ecdsa_p256_sign on
 * every full TLS handshake, and any stranger on the internet can force those by
 * declining to present a session ticket. It also does an ephemeral ECDHE per
 * handshake with p256_mul.
 *
 * Nothing in this file indexes memory at a secret-derived offset -- the only
 * computed indices are limb numbers and the constant reduction tables -- so
 * unlike lib/aes.c it is not exposed to cache attacks, and everything below is
 * about instruction counts and branches, which is the kind of leak that
 * survives a network.
 *
 * FIXED, and how it was checked:
 *
 *   The three claims this header used to make were all true, and it was
 *   missing five more. u256_cmp returned at the first differing limb; mod512's
 *   512 per-bit reductions each branched, which is the whole of the scalar
 *   arithmetic and so of p256_sc_inv, the ECDSA k^-1; modadd and modsub
 *   branched; fast_reduce ended in two data-dependent loops; p256_sc_from_be,
 *   p256_sc_neg and p256_eq each had a secret-dependent early exit. All of
 *   those are now unconditional arithmetic plus a ct_cmov64 from lib/ct.h.
 *
 *   Worst of all, and unnamed here: jdouble returned early for the point at
 *   infinity. p256_mul starts its accumulator at infinity, so a scalar with z
 *   leading zero bits got z nearly-free ladder steps -- and the leading-zero
 *   count of an ECDSA nonce is exactly what the lattice attack on partial
 *   nonce leakage consumes. The general doubling formula already yields Z == 0
 *   from Z == 0, and infinity is represented by Z == 0 alone, so the branch
 *   was deleted rather than masked.
 *
 *   Measured, not asserted. Counting the limb operations inside one k*G for
 *   scalars of fixed Hamming weight 32 with the top set bit moved from bit 255
 *   down to bit 40: before, 49206 down to 14122 operations, a 3.5x readout of
 *   the leading-zero count; after, 121402 for every one of them, identically.
 *   p256_sc_inv likewise went from a per-input operation mix (its conditional
 *   subtraction fired 1 time for the input 1 and about 54000 times for a random
 *   scalar; 2.39 ms against 3.45 ms of wall clock) to the same 653226
 *   operations for every input. Equivalence was checked by diffing a 38000-line
 *   transcript of every internal and public routine over random and boundary
 *   inputs against the old code, plus p256_selftest and the CAVP vector in
 *   srv/test/cryptotest.c.
 *
 * WHAT STILL LEAKS:
 *
 *   - p256_mul is still double-and-add: the add is skipped when the bit is
 *     zero, so the cost is 256 doublings plus one addition per set bit. The
 *     residual signal is the scalar's HAMMING WEIGHT and nothing else -- 101376
 *     operations at weight 1, 183418 at weight 128, a straight line at 646
 *     operations per set bit. That is one aggregate number per signature with
 *     no positional information, which is a far weaker input to a lattice
 *     attack than the leading-zero count it replaces, but it is not zero.
 *     Closing it means an always-add ladder with a masked accumulator, and that
 *     needs a branch-free p256_padd (below) to be worth anything.
 *   - p256_padd still branches on infinity and on point equality. The infinity
 *     branches cannot simply be deleted the way jdouble's could -- the general
 *     formula returns infinity for "infinity + Q" instead of Q -- so this needs
 *     a select over all five outcomes, which roughly doubles its cost. Inside
 *     the ladder the equality cases are unreachable and exactly one addition
 *     per scalar meets an infinite accumulator, so it contributes no positional
 *     leak there; the exposure is p256_padd's other callers.
 *   - The constant-time reductions cost about 10% of a k*G and about 60% of a
 *     p256_sc_inv on a fast x86 host; a full ecdsa_p256_sign went from 7.5 ms
 *     to 10.2 ms there. The ~170 ms this header used to quote is the Milk-V Duo
 *     board, which was not re-measured -- expect the same proportions.
 *
 * modinv is deliberately not on either list: it is square-and-multiply over a
 * PUBLIC exponent (p-2 or n-2), so its operation sequence is fixed and only its
 * operands are secret. That is fine, and it is worth saying because it looks
 * exactly like the thing that is not.
 *
 * REQUIRES a 64x64 -> 128 multiply (GCC/Clang `unsigned __int128`); p256.c
 * refuses to compile without one, so 32-bit targets and MSVC are out.
 *
 * If you are copying this file: it is fine for a J-PAKE against your own
 * device; behind a public TLS endpoint it is better than it was and still not
 * what a reviewed constant-time implementation would give you. See TODO item
 * 56 for the outstanding decision. */
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

/* ---- A SCALAR THAT CAME FROM OUTSIDE. CHECKED, NOT REDUCED. -----------
 *
 * p256_sc_from_be above REDUCES: it loads 32 big-endian bytes and subtracts n
 * once if the value reached it. That is the right answer for the two things
 * that are not scalars to begin with -- a HASH, which FIPS 186-4 defines as
 * `e mod n`, and an x-coordinate, which becomes a scalar only by being taken
 * mod n -- and for those it stays exactly where it is.
 *
 * It is the wrong answer for 32 bytes that arrived from outside CLAIMING to
 * be a scalar, because the reduction is many-to-one and every one of the
 * extra preimages is an ALIAS the API contract said could not exist:
 *
 *   - r + n and r are the same signature to a verifier that reduces. Measured
 *     against this tree before the check went in: a genuine (r, s) verified,
 *     and so did (r + n, s) and (r, s + n) -- three accepted encodings of one
 *     signature, two of which the signer never produced and never saw. The
 *     alias exists whenever the scalar is below 2^256 - n (just over 2^223),
 *     which is a 2^-32 slice of the interval rather than a curiosity someone
 *     has to be lucky to hit: an attacker picks the signature.
 *   - n and 0 both reduce to 0. A private key of n, a nonce of n, an r of n
 *     therefore became the one scalar every routine here refuses -- but they
 *     became it QUIETLY, one layer below the refusal, so what got reported
 *     was "zero key" for a caller that never supplied a zero key.
 *
 * The interval is [1, n-1], the same one p256_sc_rand draws from, so the
 * generator and the decoder now agree on what a scalar is.
 *
 * WATCH THE SENSE: P256_SCALAR_OK is 0, so `if (!p256_sc_from_be_checked(...))`
 * reads as success and is the opposite of what it looks like. Compare against
 * P256_SCALAR_OK by name, always. (enum csv_field made this mistake once.)
 *
 * `r` is set to ZERO on any refusal rather than left alone, for the same
 * reason p256_sc_rand zeroes its output: zero is the one scalar every
 * consumer in this repo independently refuses, so a caller that ignores the
 * status fails hard downstream instead of proceeding with an alias. */
enum p256_scalar {
   P256_SCALAR_OK = 0, /* in [1, n-1]: a scalar */
   P256_SCALAR_ZERO,   /* x == 0 */
   P256_SCALAR_RANGE   /* x >= n: an alias of x - n, not a scalar */
};

/* CONSTANT-TIME IN THE VALUE. The verdict is computed with the same borrow
 * and the same selects the reductions use, so nothing here branches on the
 * bytes -- which matters because d and k go through it. The caller's one
 * branch on the returned status leaks "this scalar was refused", and that is
 * all it leaks; ecdsa.c says why that disclosure is acceptable there. */
enum p256_scalar p256_sc_from_be_checked(struct u256 *r, const uint8_t be[32])
    __attribute__((warn_unused_result));

/* ---- A SECRET SCALAR. THE ONLY WAY TO GET ONE. ------------------------
 *
 * 32 big-endian bytes holding a uniformly distributed value in [1, n-1] --
 * the canonical interval for a P-256 private key, an ephemeral ECDHE scalar
 * and a Schnorr proof nonce alike. Returns 1 on success; on 0 it has written
 * 32 zero bytes and THE CALLER MUST NOT PROCEED.
 *
 * WHAT THIS REPLACES. Four call sites -- lib/jpake.c's five per-exchange
 * scalars, srv/tls.c's ECDHE scalar, srv/tls.c's CertificateVerify nonce and
 * app/dexcom.c's key-challenge nonce -- each did `rand_bytes(b, 32)` and then
 * reduced with p256_sc_from_be. That is biased (the low 2^-32 of the interval
 * comes out twice as often) and, far worse, it ADMITS ZERO: 0 and n both
 * reduce to 0, and a zero ECDHE scalar makes the shared secret a constant
 * while a zero proof nonce publishes the witness it was hiding. lib/p256.c
 * has the arithmetic and the numbers.
 *
 * ONE generator, so those properties are one thing to get right rather than
 * four. It is a P-256 fact, so it lives with the curve rather than in
 * lib/rand.c -- which deliberately knows no algorithm -- and it is why
 * lib/p256.c is the only file in lib/ that both does curve arithmetic and
 * asks for entropy.
 *
 * NOT REDUCED, on purpose. The 32 bytes are the drawn bytes, already in
 * range; feeding them to p256_sc_from_be (as the ECDHE and J-PAKE callers
 * still do, because that is how they get limbs) is the identity on a value
 * below n, not a second chance to introduce the bias this removed. */
int p256_sc_rand(uint8_t out[32]);

/* The retry cap, exposed so a test can pin it. Rejection needs a second draw
 * with probability 2^-32, so this is never reached by chance -- 64 straight
 * rejections is 2^-2048. It bounds a BROKEN source (stuck bits, a stub
 * returning a constant) into a reported failure instead of a hang. */
#define P256_SC_RAND_TRIES 64

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
