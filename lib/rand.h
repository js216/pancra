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
 * An open("/dev/urandom") in rand.c, with open/read/close declared by hand
 * for the freestanding build because no libc there declares them, puts three
 * Unix syscalls and a device path inside a directory whose
 * whole claim is that it is self-contained reusable code -- so lib/ could not
 * be built for a platform without /dev/urandom, and the file said so in a
 * comment while doing it anyway.
 *
 * The platform contract is entropy.h -- one call, implemented by whichever
 * platform file is linked (lib/randunix.c today). Choosing a platform is
 * choosing which file to link rather than editing an #ifdef, and this header
 * declares only what rand.c itself implements. */

/* THE OVERRIDE IS NOT HERE ANY MORE.
 *
 * rand_set_source -- replace the entropy provider at run time -- was declared
 * in this header, which is the one every user of randomness includes. It
 * exists for TESTS ("what does this code do when the entropy source fails?"
 * is otherwise unaskable without a machine whose /dev/urandom is broken), and
 * it shipped as a process-global mutable capability that production code
 * consulted on every call: anything that could reach rand.h could replace the
 * source of every key, nonce and token this program generates, for the rest
 * of the process, from any thread, with nothing serialising it.
 *
 * It is lib/randtest.h now, and lib/rand.c compiles it in only when
 * RAND_TEST_SOURCE is defined -- which no production target defines. In a
 * shipped build the pointer, the setter and the branch that reads it do not
 * exist at all: rand_bytes calls the platform provider directly.
 */

#endif
