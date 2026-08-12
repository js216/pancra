/* SPDX-License-Identifier: GPL-3.0
 * rand.h --- cryptographic random bytes from the operating system
 * Copyright 2026 Jakob Kastelic
 */
#ifndef RAND_H
#define RAND_H
#include <stddef.h>
#include <stdint.h>

/* 1 only if all n bytes were filled with real entropy; 0 otherwise, and on 0
 * the buffer's contents are UNDEFINED.
 *
 * The return value is not advisory. Every caller here passes an uninitialised
 * stack buffer, so ignoring a failure means signing or key-agreeing with
 * whatever was on the stack -- which for an ECDSA nonce hands over the private
 * key. A caller that cannot handle failure must abort, not continue. */
int rand_bytes(uint8_t *buf, size_t n);

#endif
