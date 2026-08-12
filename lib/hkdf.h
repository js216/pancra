/* SPDX-License-Identifier: GPL-3.0
 * hkdf.h --- HKDF extract-and-expand over HMAC-SHA256 (RFC 5869)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef HKDF_H
#define HKDF_H
#include <stddef.h>
#include <stdint.h>

/* salt may be NULL, which the RFC defines as a string of HashLen zeros. */
void hkdf_extract(const uint8_t *salt, size_t saltn, const uint8_t *ikm,
                  size_t ikmn, uint8_t out[32]);

/* info is bounded by the caller: the internal block is 32+256+1 bytes, so
 * infon must not exceed 256. Every caller here passes an HkdfLabel, which
 * cannot. */
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t infon,
                 uint8_t *out, size_t n);

#endif
