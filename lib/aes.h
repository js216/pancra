// SPDX-License-Identifier: GPL-3.0
// aes.h --- Portable AES-128 single-block encrypt (API)
// Copyright 2026 Jakob Kastelic

/* Portable AES-128 single-block encrypt.
 *
 * CONSTANT-TIME BY CONSTRUCTION: NO TABLE IS INDEXED BY A SECRET.
 *
 * THE TEXTBOOK IMPLEMENTATION CANNOT SAY THAT SENTENCE. A 256-byte S-box
 * read at a state-derived offset, sixteen times a round, ten rounds a block,
 * plus forty more lookups for the key schedule, takes every one of those
 * indices from the key. The values do
 * not leak, but which cache lines were touched did, and that is the whole of
 * the Bernstein / Osvik-Shamir-Tromer family: an observer sharing a cache
 * recovers the key from the access pattern, with no fault, no oracle and no
 * protocol flaw.
 *
 * The S-box is now COMPUTED. It is defined as the multiplicative inverse in
 * GF(2^8) followed by a fixed affine map, and aes.c evaluates exactly that:
 * the inverse as x^254 (Fermat, which also sends 0 to 0 without a special
 * case), over bit-sliced data, so the arithmetic is AND and XOR on eight
 * words. Sixteen bytes are transposed into eight 16-bit planes and done at
 * once. The rounds around it -- ShiftRows, MixColumns, AddRoundKey -- are
 * unchanged from the version that passed every vector in this repo.
 *
 * WHAT IS LEFT TO BE SURE OF. The property is structural rather than argued:
 * there is one array in aes.c, the ten round constants, and its index is the
 * round number. Nothing else is read at a computed offset, and there is no
 * branch on a data value anywhere in the cipher. That is a claim a reader can
 * check by looking, which is the strongest form available without a reviewed
 * implementation.
 *
 * WHAT IS CHECKED, and how far it goes. test/srv/cryptotest.c evaluates all
 * 256 S-box inputs against the published table -- not a sample -- and then the
 * FIPS-197 C.1 vector, both entry points, and the NIST GCM vectors on top. In
 * migration the whole cipher was diffed against the table-driven version over
 * 200000 random (key, block) pairs, byte for byte, with no mismatch. A
 * computed S-box that is wrong for a single input is a cipher that is wrong
 * for a fraction of blocks, and the exhaustive test is there because that is
 * exactly the failure a known-answer vector can miss.
 *
 * WHAT IT COSTS, measured on a fast x86 host at 200000 blocks:
 *
 *     table-driven, key schedule per block      1.43 us/block
 *     computed, key schedule per block         23.9  us/block
 *     computed, key schedule held (struct)     14.8  us/block
 *
 * So about 10x, which is what a table buys and is the honest price of not
 * having one: ~1 MB/s here.
 *
 * AND ON THE BOARD THE SERVER ACTUALLY RUNS ON, measured rather than scaled --
 * this binary cross-compiled for riscv64 and run on the Milk-V Duo: 124 us per
 * block with the key held, which is 126 KB/s. For what this server moves -- a
 * few kilobytes of CSV per sync, a plot page of tens of kilobytes -- that is
 * a couple of hundred milliseconds at worst, next to the 100 ms the same board
 * spends on the handshake signature. If it ever does become the bound, the
 * answer is not a return to tables: widen the plane word and run four or
 * eight blocks of the GCM counter stream through the rounds together, which
 * amortises the transpose and the S-box over four times the data.
 *
 * HOLD THE KEY SCHEDULE. aes128_encrypt expands the key and encrypts one
 * block, which was the only entry point and meant TLS record traffic re-derived
 * the schedule for every sixteen bytes. With a computed S-box that is a third
 * of the work, so lib/gcm.c now keeps a struct aes128 for the whole message.
 * A caller with one block to encrypt (app/dexcom.c) can still use the one-shot
 * form.
 *
 * WHO CALLS THIS:
 *
 *   srv/tls.c, via lib/gcm.c, for TLS_AES_128_GCM_SHA256 record protection.
 *   The keys are per-connection traffic secrets, the attacker chooses much of
 *   the plaintext, and the number of blocks is unbounded -- everything a
 *   cache attack wants except the shared cache, which is why no table is
 *   indexed by a secret here.
 *
 *   app/dexcom.c dexcom_dex8, for the Dexcom per-connection auth hash over
 *   BLE. One block per connection; the threat model was never the issue here.
 *
 * WHAT WOULD STILL BE BETTER. A reviewed, audited implementation, or the
 * ARMv8 AES instructions on the phone (the riscv64 C906 the server runs on has
 * none, so that route cannot cover the side that matters). Both remain worth
 * doing and neither is a precondition for this file being safe to run beside
 * other tenants. */
#ifndef DEX_AES_H
#define DEX_AES_H
#include <stdint.h>

/* AN EXPANDED KEY. Hold one and encrypt many blocks with it: the schedule
 * costs 40 S-box evaluations, and through the one-shot call below alone they
 * would be recomputed for every 16 bytes. */
struct aes128 {
   uint8_t rk[176];
};

void aes128_init(struct aes128 *ctx, const uint8_t key[16]);
void aes128_encrypt_ctx(const struct aes128 *ctx, const uint8_t in[16],
                        uint8_t out[16]);

/* Key schedule and one block, for callers with one block to encrypt. */
void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16]);


#endif
