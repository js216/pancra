/* SPDX-License-Identifier: GPL-3.0
 * ct.h --- constant-time comparison
 * Copyright 2026 Jakob Kastelic
 */
#ifndef CT_H
#define CT_H
#include <stddef.h>

/* 1 iff the n bytes match, in time that does not depend on WHERE they differ.
 *
 * For anything an attacker can retry against: a MAC, a password hash, a
 * pairing confirmation, a TLS binder or Finished. memcmp returns as soon as
 * it finds a difference, so how long it took says how many leading bytes were
 * right -- which turns forging one 32-byte value into guessing 32 single
 * bytes. Use this everywhere a comparison decides whether to trust someone. */
int ct_eq(const void *a, const void *b, size_t n);

#endif
