/* SPDX-License-Identifier: GPL-3.0
 * wireint.h --- the exact-width integers this protocol is written in
 * Copyright 2026 Jakob Kastelic
 *
 * WHY THE WIRE'S NUMBERS ARE NOT `long`.
 *
 * As a C `long` on both sides, printed and parsed with %ld, every id,
 * instant, bucket number and row count in the sync protocol needs
 * `sizeof(long) >= 8` asserted on both halves to be safe. That assertion is
 * honest about the risk and wrong about the remedy: it declares a HOST
 * PROPERTY to be part of an INTERCHANGE FORMAT. The wire is decimal text -- it
 * says nothing about the machine at either end -- so the only thing the
 * assertion bought was a refusal to build somewhere the code would have been
 * wrong, at the price of never being able to build there at all.
 *
 * These are the widths the format actually needs, named exactly:
 *
 *   int64_t   an id, an instant, a bucket number, a row count, the length of
 *             a bucket's canonical text.
 *
 * and they are the same 64 bits on every data model, so the same source is
 * correct on LP64 (aarch64-linux-android, the server targets), on LLP64, and
 * on ILP32 -- where `long` is 32 bits and a timestamp would have stopped
 * being representable in 2038 while the tests still passed. `make -f
 * test/Makefile wirecheck` COMPILES the protocol units for ILP32 for exactly
 * that reason: it is the only way to see a %ld that should have been a
 * PRIwire, since on LP64 the two are the same type and no warning is
 * possible.
 *
 * PRIwire / SCNwire ARE HERE RATHER THAN FROM <inttypes.h> because the app
 * half is FREESTANDING: it mirrors the bionic ABI by hand (app/ndk.h) and
 * cannot include a hosted header. Both sides use these so the two
 * implementations spell the wire the same way -- which is the whole reason
 * lib/wirevec.h exists.
 *
 * DECODING IS UNCHANGED, and that is a compatibility requirement, not an
 * accident: the text a phone in the field signed and stored is decimal
 * digits, so it parses identically whether the variable it lands in is a
 * `long` or an int64_t. Nothing in a file or on the wire moved.
 */
#ifndef WIREINT_H
#define WIREINT_H

#include <stdint.h>

/* The length modifier for an int64_t, worked out the same way <inttypes.h>
 * does it, but without the header. __SIZEOF_LONG__ is a compiler predefine on
 * both clang and gcc; the #else is the ILP32 and LLP64 case. */
#if defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 8
#define PRIwire "ld"
#define SCNwire "ld"
#else
#define PRIwire "lld"
#define SCNwire "lld"
#endif

/* The claim above, checked. NOT sizeof(long): a host property is not what
 * this file is asserting. */
_Static_assert(sizeof(int64_t) == 8, "int64_t is 64 bits by definition");

#endif
