// SPDX-License-Identifier: GPL-3.0
// string.h --- Freestanding <string.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <string.h> shim. The Android-target build has no bionic
 * headers, and the host glibc <string.h> won't compile freestanding; -Isrc
 * makes this the header that satisfies <string.h> for the functions we use.
 * The phone's real bionic binds the definitions at runtime (see stub_c.c). */
#ifndef PANCRA_STRING_H
#define PANCRA_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

#endif
