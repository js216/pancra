// SPDX-License-Identifier: GPL-3.0
// aes.h --- Portable AES-128 single-block encrypt (API)
// Copyright 2026 Jakob Kastelic

/* Portable AES-128 single-block encrypt. */
#ifndef DEX_AES_H
#define DEX_AES_H
#include <stdint.h>
void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16]);
#endif
