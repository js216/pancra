/* SPDX-License-Identifier: GPL-3.0
 * compiler.h --- the annotations, said once, in a way any C11 compiler can read
 * Copyright 2026 Jakob Kastelic
 *
 * WHY THIS EXISTS.
 *
 * Several of these headers are meant to be reusable on their own -- lib/p256.h
 * and lib/gcm.h are self-contained implementations of published primitives,
 * and lib/wirevec.h describes a protocol two independent programs speak. They
 * carried GNU syntax in their PUBLIC declarations:
 *
 *     int p256_verify(...) __attribute__((warn_unused_result));
 *
 * which is not C at all. A conforming C11 compiler that does not implement
 * the GNU extension cannot PARSE that line -- not "loses the warning", cannot
 * compile the header -- so an API that is otherwise portable was refused by
 * anything but GCC and Clang. The annotation is a hint; the parse error is
 * fatal. That is the wrong way round.
 *
 * So the annotations are named here, defined once for each dialect, and used
 * everywhere by name:
 *
 *   PANCRA_MUST_USE       the result must be read. On these APIs that is not
 *                         style: p256_verify returning "this signature is
 *                         invalid" into a dropped value is a signature check
 *                         that always passes, and gcm_decrypt's is the
 *                         authentication tag.
 *   PANCRA_PRINTF(f, a)   this function takes a printf format at argument f,
 *                         with its arguments starting at a.
 *   PANCRA_SCANF(f, a)    the same, for scanf.
 *
 * THREE DIALECTS, IN ORDER OF PREFERENCE. C23 spells the first one in the
 * standard ([[nodiscard]]), so where the compiler is new enough that is what
 * is used -- it needs no extension at all. GCC and Clang get the attribute
 * they have understood for twenty years. Anything else gets NOTHING, which
 * costs a warning and keeps the header compiling: exactly the trade the old
 * spelling got backwards.
 *
 * The format attributes have no standard spelling at all, so they are the
 * GNU one or nothing.
 */
#ifndef PANCRA_COMPILER_H
#define PANCRA_COMPILER_H

/* C23's [[nodiscard]] is an attribute in the STANDARD grammar, so it needs no
 * compiler extension. __STDC_VERSION__ is 202311L for C23; the guard is > the
 * C17 value so that any later standard keeps it too. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
#define PANCRA_MUST_USE [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define PANCRA_MUST_USE __attribute__((warn_unused_result))
#else
#define PANCRA_MUST_USE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PANCRA_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define PANCRA_SCANF(f, a)  __attribute__((format(scanf, f, a)))
#else
#define PANCRA_PRINTF(f, a)
#define PANCRA_SCANF(f, a)
#endif

/* PLACEMENT DIFFERS BETWEEN THE TWO SPELLINGS, and that is the one thing a
 * caller of these macros has to know. A C23 attribute goes BEFORE the
 * declaration; a GNU one may go before or after. So every use here writes it
 * FIRST -- `PANCRA_MUST_USE int f(void);` -- which both dialects accept. A
 * trailing `... f(void) PANCRA_MUST_USE;` would be a syntax error under C23,
 * which is the failure this header exists to prevent. */

#endif
