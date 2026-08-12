// SPDX-License-Identifier: GPL-3.0
// jpake.h --- EC-J-PAKE over P-256 (API)
// Copyright 2026 Juggluco and xDrip contributors

/* Password-authenticated key agreement: two parties who share a short secret
 * end up with the same 16-byte key, and a recording of the exchange does not
 * let anyone test guesses at that secret offline.
 *
 * Nothing here is specific to any device or protocol. Both halves of this
 * project use it -- the app to pair with a sensor, the server to pair with
 * the app -- and it depends on nothing but lib/p256, lib/sha256 and lib/rand.
 */
#ifndef JPAKE_H
#define JPAKE_H

#include <stddef.h>
#include <stdint.h>

/* Initialise the curve context. Call once before anything else. Returns 1. */
int jpake_init(void);

/* Exchange 160-byte round packets with the peer, then derive the shared key.
 * is_client selects the signer identity: the two sides MUST disagree about
 * it, or the Schnorr proofs are computed over the same label and the protocol
 * loses the property that makes it worth using.
 *
 * The peer_* calls return 1 only if the received zero-knowledge proof
 * validates. A caller that ignores that has an unauthenticated key exchange:
 * the proofs are the entire defence against a man in the middle. */
struct jpake;
struct jpake *jpake_new(const uint8_t *pass, size_t passlen, int is_client);
int jpake_round1(struct jpake *p, uint8_t out[160]);
int jpake_round2(struct jpake *p, uint8_t out[160]);
int jpake_round3(struct jpake *p, uint8_t out[160]);
int jpake_peer_round1(struct jpake *p, const uint8_t in[160]);
int jpake_peer_round2(struct jpake *p, const uint8_t in[160]);
int jpake_peer_round3(struct jpake *p, const uint8_t in[160]);
int jpake_shared_key(struct jpake *p, uint8_t out16[16]);
void jpake_free(struct jpake *p);

#endif
