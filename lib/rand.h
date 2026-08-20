/* SPDX-License-Identifier: GPL-3.0
 * rand.h --- cryptographic random bytes, and where they come from
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

/* ---- WHERE THE BYTES COME FROM ---------------------------------------
 *
 * THE CRYPTO IN lib/ DOES NOT KNOW. It calls rand_bytes() and nothing else;
 * the operating system appears nowhere in it.
 *
 * rand.c used to open("/dev/urandom") itself, and to declare open/read/close
 * by hand for the freestanding build because no libc there declares them.
 * That put three Unix syscalls and a device path inside a directory whose
 * whole claim is that it is self-contained reusable code -- so lib/ could not
 * be built for a platform without /dev/urandom, and the file said so in a
 * comment while doing it anyway.
 *
 * The platform contract is entropy.h -- one call, implemented by whichever
 * platform file is linked (lib/randunix.c today). Choosing a platform is
 * choosing which file to link rather than editing an #ifdef, and this header
 * declares only what rand.c itself implements. */

/* Override the provider at run time. Returns the previous one, so a caller
 * can restore it.
 *
 * This exists for TESTS, which is the other half of why the boundary is
 * worth having: "what does this code do when the entropy source fails?" was
 * previously unaskable without a machine whose /dev/urandom was broken, so
 * the failure paths that guard an ECDSA nonce were never once executed.
 * Passing NULL restores the platform provider. */
typedef int (*rand_source_fn)(uint8_t *buf, size_t n);
rand_source_fn rand_set_source(rand_source_fn src);

#endif
