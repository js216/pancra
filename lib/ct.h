/* SPDX-License-Identifier: GPL-3.0
 * ct.h --- constant-time comparison and branchless choice
 * Copyright 2026 Jakob Kastelic
 */
#ifndef CT_H
#define CT_H
#include <stddef.h>
#include <stdint.h> /* uint64_t: the width the branchless selects work in */

/* 1 iff the n bytes match, in time that does not depend on WHERE they differ.
 *
 * For anything an attacker can retry against: a MAC, a password hash, a
 * pairing confirmation, a TLS binder or Finished. memcmp returns as soon as
 * it finds a difference, so how long it took says how many leading bytes were
 * right -- which turns forging one 32-byte value into guessing 32 single
 * bytes. Use this everywhere a comparison decides whether to trust someone. */
int ct_eq(const void *a, const void *b, size_t n);

/* ---- WIPING A SECRET SO THE COMPILER CANNOT DELETE THE WIPE -------------
 *
 * `memset(p, 0, n)` immediately before `free(p)` -- or before a buffer goes
 * out of scope -- is a store to memory that provably nothing reads again. A
 * compiler is entitled to notice that and remove it entirely, and optimising
 * compilers do: the dead-store elimination that makes ordinary code fast is
 * the same pass that deletes the only line standing between a freed
 * allocation and the next thing to be handed that address.
 *
 * It is not a hypothetical. This is why C11 has memset_s and why the BSDs and
 * glibc ship explicit_bzero; every one of them exists because the plain memset
 * was being elided in exactly this position.
 *
 * ct_wipe is that primitive. It writes through a `volatile` pointer, which the
 * standard requires the implementation to actually perform -- so the stores
 * happen whether or not anything will read them.
 *
 * WHERE IT BELONGS, and where it does not. Use it for material whose value is
 * the secret: a passphrase, a private scalar, a derived key, a session state
 * about to be freed. Do NOT reach for it as a general "clear this" -- a clear
 * whose purpose is to reset state for reuse is ordinary and should read as
 * ordinary, and making every memset volatile would hide the few that are
 * load-bearing among a hundred that are not. jpake.c's r1/r2/r3 clear is the
 * example on the other side: those hold the PEER's public values, and its
 * comment says so. */
void ct_wipe(void *p, size_t n);

/* ---- branchless choice --------------------------------------------------
 *
 * ct_eq above is about a comparison whose RESULT the caller then acts on.
 * These are about the action itself.
 *
 * Modular arithmetic is built out of "if that overflowed the modulus, subtract
 * it", and the value that decides is routinely a secret: an ECDSA nonce, a
 * private scalar, a field element derived from one. Written as an `if`, the
 * subtraction either happens or it does not, and how long the routine took
 * says which -- once per limb, thousands of times per signature, which is
 * enough structure to reconstruct the secret's high bits. Written as an
 * unconditional subtraction followed by ct_cmov64, both candidates are always
 * computed and only the ANSWER differs, so neither the instruction trace nor
 * the memory access pattern depends on the secret any more.
 *
 * `c` must be exactly 0 or 1. These build a full-width mask by negating it, so
 * a 2 would select neither operand; normalise a wider condition with ct_nz64
 * first. Note that nothing here reads memory at a computed index, which is the
 * other half of being constant-time and the half a select cannot give you --
 * lib/aes.c's S-box is the example, and no amount of ct_cmov64 fixes it.
 *
 * They are `static inline` in the header deliberately, for two reasons. Every
 * caller sits inside a loop that runs thousands of times per handshake, so a
 * function call per limb would cost more than the branch it replaces. And
 * several link lines (see the Makefile: modeltest, interoptest) compile
 * lib/p256.c WITHOUT lib/ct.c, so defining these in ct.c would quietly make
 * ct.o a new link-time dependency of the curve code for binaries that have
 * never needed it. A header-only definition has no such reach. */

/* 0 -> 0, 1 -> all ones. Anything else is a caller bug. */
static inline uint64_t ct_mask64(uint64_t c)
{
   return (uint64_t)0 - c;
}

/* 1 iff x is nonzero. Either x or its negation has the top bit set unless x is
 * zero, which is the whole trick and the reason this needs no branch. */
static inline uint64_t ct_nz64(uint64_t x)
{
   return (x | ((uint64_t)0 - x)) >> 63U;
}

/* c ? newv : oldv, without telling anyone which. */
static inline uint64_t ct_cmov64(uint64_t oldv, uint64_t newv, uint64_t c)
{
   const uint64_t m = ct_mask64(c);
   return oldv ^ ((oldv ^ newv) & m);
}

#endif
