// SPDX-License-Identifier: GPL-3.0
// sha256.h --- Portable SHA-256 (FIPS 180-4)
// Copyright 2026 Jakob Kastelic

#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>

/* One-shot, for data held whole. */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Streaming, for data never held whole (a sync bucket fed line by line). */
struct sha256_ctx {
   uint32_t h[8];
   uint8_t block[64];
   size_t used;
   uint64_t total;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const uint8_t *p, size_t n);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);

#endif
