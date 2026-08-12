/* SPDX-License-Identifier: GPL-3.0
 * gcm.h --- AES-128-GCM authenticated encryption (NIST SP 800-38D)
 * Copyright 2026 Jakob Kastelic
 *
 * Counter mode for secrecy, GHASH for authenticity, over lib/aes.c's block
 * function. The 12-byte nonce is the only size worth supporting: it is what
 * TLS uses and the only one that needs no extra GHASH pass to build the
 * counter block.
 */
#ifndef GCM_H
#define GCM_H
#include <stddef.h>
#include <stdint.h>

/* ct may alias pt. tag is written separately, never appended. */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t *aad, size_t aadn, const uint8_t *pt,
                     size_t n, uint8_t *ct, uint8_t tag[16]);

/* 1 if the tag verifies, 0 if it does not -- and on 0 the plaintext is NOT
 * written, so a caller that ignores the result still cannot act on forged
 * bytes. The comparison is constant time. */
int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t n, const uint8_t tag[16], uint8_t *pt);

#endif
