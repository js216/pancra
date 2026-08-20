/* SPDX-License-Identifier: GPL-3.0
 * entropy.h --- what lib/ requires of a platform, and nothing else
 * Copyright 2026 Jakob Kastelic
 *
 * THE ONE THING THE CRYPTO CANNOT COMPUTE FOR ITSELF.
 *
 * Everything else in lib/ is arithmetic: given the same input it produces the
 * same output on any machine, and needs no operating system to do it. Random
 * bytes are the exception, and this header is that exception written down --
 * a single call, declared here and implemented by whichever platform file is
 * linked (lib/randunix.c reads /dev/urandom).
 *
 * WHY IT IS NOT IN rand.h. rand.c opened /dev/urandom itself and hand-declared
 * open/read/close for the freestanding build, which put three Unix syscalls
 * and a device path inside a directory whose whole claim is that it is
 * self-contained reusable code -- the file said as much in a comment while
 * doing it anyway. Separating the two means the crypto depends on the
 * CONTRACT (this file) and never on the implementation, and porting to a
 * platform without /dev/urandom is choosing which file to link.
 *
 * There is deliberately no default and no fallback: a program that links no
 * provider fails to LINK, which is the right moment to discover it. An
 * entropy source that silently degrades is the one failure this codebase can
 * least afford -- see rand.h on what an unfilled buffer does to an ECDSA
 * nonce.
 */
#ifndef ENTROPY_H
#define ENTROPY_H
#include <stddef.h>
#include <stdint.h>

/* Fill `buf` with `n` bytes from the platform's entropy source.
 *
 * 1 only if EVERY byte was filled; 0 otherwise, and then the buffer's
 * contents are undefined. A provider must never report success having filled
 * part of the buffer -- that is what a short read from /dev/urandom looks
 * like, and it is rare enough that an implementation ignoring it appears
 * correct for years. */
int entropy_fill(uint8_t *buf, size_t n);

#endif
