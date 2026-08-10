// SPDX-License-Identifier: GPL-3.0
// stdlib.h --- Freestanding <stdlib.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <stdlib.h> shim (see string.h for the rationale). */
#ifndef PANCRA_STDLIB_H
#define PANCRA_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void *realloc(void *p, size_t n);
void *calloc(size_t nmemb, size_t size);
void free(void *p);

#endif
