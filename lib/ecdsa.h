/* SPDX-License-Identifier: GPL-3.0
 * ecdsa.h --- ECDSA over NIST P-256 (FIPS 186-4)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef ECDSA_H
#define ECDSA_H
#include <stdint.h>

/* Sign hash (32 bytes, already a digest) with private scalar d_be, using the
 * caller's per-signature nonce k_be. 1 on success, 0 if d, k or the derived
 * r or s is out of range -- in which case the caller must retry with a fresh
 * k, never with the same one.
 *
 * k MUST come from a cryptographic RNG and MUST NOT repeat across
 * signatures. Two signatures sharing a k reveal d outright.
 *
 * NOT CONSTANT-TIME: the scalar multiply underneath branches on the bits of
 * k (see the note at the top of p256.h). srv/tls.c calls this with the
 * server's long-term key on every full handshake, which a stranger can force,
 * so the exposure is stated there rather than left to be discovered. */
int ecdsa_p256_sign(const uint8_t d_be[32], const uint8_t hash[32],
                    const uint8_t k_be[32], uint8_t r_be[32], uint8_t s_be[32]);

/* 1 iff (r,s) is a valid signature by the public key (qx,qy) over hash. */
int ecdsa_p256_verify(const uint8_t qx[32], const uint8_t qy[32],
                      const uint8_t hash[32], const uint8_t r_be[32],
                      const uint8_t s_be[32]);

#endif
