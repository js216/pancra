/* SPDX-License-Identifier: GPL-3.0
 * pbkdf2.h --- PBKDF2-HMAC-SHA256 (RFC 8018)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef PBKDF2_H
#define PBKDF2_H
#include <stddef.h>
#include <stdint.h>

/* iters is the caller's cost parameter and must be stored beside the hash,
 * so it can be raised later without invalidating existing passwords. */
void pbkdf2_sha256(const uint8_t *pw, size_t pwn, const uint8_t *salt,
                   size_t saltn, unsigned iters, uint8_t *out, size_t n);

#endif
