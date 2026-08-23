// SPDX-License-Identifier: GPL-3.0
// p256.h --- Portable NIST P-256 (API)
// Copyright 2026 Jakob Kastelic

/* NIST P-256 (secp256r1): the field, scalar and point operations this repo
 * needs. No external dependencies.
 *
 * THE OPERATION SEQUENCE DOES NOT DEPEND ON ANY SECRET.
 *
 * WHO THIS HAS TO BE TRUE FOR. srv/tls.c signs the server's LONG-TERM key with
 * ecdsa_p256_sign on every full TLS handshake, and any stranger on the
 * internet can force those by declining to present a session ticket; it also
 * does an ephemeral ECDHE per handshake with p256_mul. So the threat model is
 * a remote attacker who can ask for arbitrarily many signatures -- not the
 * Dexcom J-PAKE over a BLE link to a sensor on the user's own arm, which is
 * what this file was written for.
 *
 * WHAT IS GUARANTEED, AND WHAT IS NOT:
 *
 *   No branch in this file is steered by a secret, and no memory is read at a
 *   secret-derived offset -- the only computed indices are limb numbers and
 *   the constant reduction tables, so nothing here is exposed to a cache
 *   attack. The scalar multiplication is an always-add
 *   ladder over a branch-free point addition, so the work done for a scalar
 *   does not depend on its value: not on its leading zeros, not on its
 *   Hamming weight, not on its magnitude.
 *
 *   THAT IS A STATEMENT ABOUT BRANCHES AND OPERATION COUNTS, and it is under
 *   test rather than asserted: p256.c counts its own limb operations under
 *   -DP256_COUNT and test/srv/cttest.c requires the count to be identical
 *   across scalars that differ in exactly the ways a branching implementation
 *   answers differently for. Built against one, that test fails five
 *   assertions.
 *
 *   IT IS NOT A CLAIM OF CONSTANT TIME ON YOUR PROCESSOR. A count cannot see
 *   an operand-dependent instruction latency or a branch the compiler
 *   introduced on its own, and this is not a reviewed implementation. That is
 *   why the closing paragraph below still says what it says.
 *
 * WHAT IT COSTS, measured on a fast x86 host, 20 iterations each:
 *
 *     k*G                3.74 ms -> 9.06 ms   (2.4x)
 *     ecdsa_p256_sign   10.50 ms -> 15.24 ms  (1.45x)
 *     ecdsa_p256_verify 14.76 ms -> 24.96 ms  (1.69x)
 *
 *   On the Milk-V Duo the server runs on, measured there: 100 ms per
 *   signature, which is once per full TLS handshake and not once per request.
 *
 * WHAT IT COST, since the question always comes up: k*G 3.74 ms -> 9.06 ms,
 * ecdsa_p256_sign 10.50 ms -> 15.24 ms, ecdsa_p256_verify 14.76 ms -> 24.96 ms
 * on an x86 host; 100 ms per signature on the Milk-V Duo the server runs on.
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
 * device; behind a public TLS endpoint it is still not what a reviewed,
 * audited constant-time implementation would give you. The remaining
 * decision -- vendor one, or use a platform TLS backend -- is about what this
 * repo vendors rather than about an edit to this file. */
#ifndef DEX_P256_H
#define DEX_P256_H

#include "compiler.h" /* PANCRA_MUST_USE: the annotation, portably */
#include <stdint.h>

struct u256 { /* little-endian 64-bit limbs */
   uint64_t v[4];
};

struct jpoint { /* Jacobian; Z==0 => infinity */
   struct u256 X, Y, Z;
};

void p256_init(void); /* IDEMPOTENT: call from anywhere, as often as needed */


/* scalars mod n (curve order) */
void p256_sc_from_be(struct u256 *r,
                     const uint8_t be[32]); /* load + reduce mod n */
void p256_sc_mul(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_add(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_sub(struct u256 *r, const struct u256 *a, const struct u256 *b);
void p256_sc_neg(struct u256 *r, const struct u256 *a);
/* THE CANONICAL ENCODING, and the only one. 32 big-endian bytes, most
 * significant first, from the limb representation `struct u256` uses -- the
 * exact inverse of p256_sc_from_be's load.
 *
 * It lives here because the representation does. A caller that writes its own
 * loop is encoding a struct whose layout is this file's business, and two
 * such loops (ECDSA's signature and J-PAKE's proof scalar were each one) can
 * disagree about byte order in a way nothing catches: both sides of a
 * signature check are usually the same implementation, so a swapped encoding
 * verifies against itself and fails only against somebody else's.
 *
 * ENCODING IS NOT VALIDATION. This writes whatever the scalar holds,
 * including zero and values a caller should never have built; the range rule
 * for a scalar that came from OUTSIDE is p256_sc_from_be_checked below, and
 * keeping the two apart is deliberate -- an encoder that also judged would
 * have to answer, and every caller here has already decided. */
void p256_sc_to_be(const struct u256 *a, uint8_t be[32]);
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
 * status fails hard downstream rather than proceeding with an alias. */
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
PANCRA_MUST_USE enum p256_scalar p256_sc_from_be_checked(struct u256 *r,
                                                         const uint8_t be[32]);

/* ---- A SECRET SCALAR. THE ONLY WAY TO GET ONE. ------------------------
 *
 * 32 big-endian bytes holding a uniformly distributed value in [1, n-1] --
 * the canonical interval for a P-256 private key, an ephemeral ECDHE scalar
 * and a Schnorr proof nonce alike. Returns 1 on success; on 0 it has written
 * 32 zero bytes and THE CALLER MUST NOT PROCEED.
 *
 * WHY EVERY CALLER GOES THROUGH IT. The four sites that need such a scalar --
 * lib/jpake.c's five per-exchange scalars, srv/tls.c's ECDHE scalar,
 * srv/tls.c's CertificateVerify nonce and app/dexcom.c's key-challenge nonce
 * -- could each do `rand_bytes(b, 32)` and reduce with p256_sc_from_be. That
 * is biased (the low 2^-32 of the interval
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
 * returning a constant) into a reported failure rather than a hang. */
#define P256_SC_RAND_TRIES 64

/* points */

/* THE GENERATOR, AS A COPY. 1 when `*out` is the curve's base
 * point; 0 -- with `*out` set to the point at infinity -- when p256_init has
 * not run, which every downstream check then refuses.
 *
 * AS A WRITABLE GLOBAL, anything that includes this header can assign to the
 * base point of the curve, or read it before there is one in it, and neither
 * shows: J-PAKE's proofs verify perfectly well against a
 * substituted G at both ends, and a zeroed G makes every public key the
 * point at infinity. A copy cannot be assigned back, and asking cannot
 * happen too early without being told. */
int p256_gen(struct jpoint *out);

void p256_mul(struct jpoint *r, const struct u256 *k,
              const struct jpoint *P);                   /* k*P */
void p256_mul_g(struct jpoint *r, const struct u256 *k); /* k*G */
void p256_padd(struct jpoint *r, const struct jpoint *P,
               const struct jpoint *Q);
int p256_eq(const struct jpoint *A, const struct jpoint *B);
/* affine X||Y (32+32); returns 1 ok, 0 if infinity / not on curve */
int p256_to_xy(const struct jpoint *P, uint8_t x[32], uint8_t y[32]);
int p256_from_xy(struct jpoint *P, const uint8_t x[32], const uint8_t y[32]);
void p256_uncompressed(const struct jpoint *P, uint8_t out[65]); /* 04||X||Y */

/* ---- THE CONSTANT-TIME REGRESSION HOOK ----------------------------------
 *
 * Defined only in a build that passes -DP256_COUNT, which is test/srv/cttest.c
 * and nothing else. p256.c's limb primitives increment it; the test drives the
 * public entry points with operands that differ in exactly the ways that used
 * to change the work done here, and asserts the count does not move.
 *
 * A count is not a clock. It cannot see a data-dependent memory access or a
 * variable-latency instruction, and it is not evidence that this file is
 * constant-time on any particular processor. What it does catch is the whole
 * class of regression that has ever actually happened in this file: a branch
 * that skips work for some inputs. Read it as a lock on the property, not as
 * proof of it. */
#ifdef P256_COUNT
extern unsigned long p256_op_count;
#endif

#endif
