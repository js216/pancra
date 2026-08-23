// SPDX-License-Identifier: GPL-3.0
// stdio.h --- Freestanding <stdio.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <stdio.h> shim. The JDK's jni.h includes <stdio.h> out
 * of tradition but uses nothing from it, and the real glibc header won't
 * compile in a freestanding Android-target build; -Isrc makes this the header
 * that satisfies <stdio.h>. va_list comes from clang's own <stdarg.h>. Only the
 * bounded snprintf we actually call is declared; the phone's real bionic binds
 * it at runtime (see stub_c.c). */
#ifndef PANCRA_STDIO_H
#define PANCRA_STDIO_H

#include "compiler.h" /* PANCRA_MUST_USE: the annotation, portably */

#include <stddef.h>

/* THE FORMAT ATTRIBUTES ARE THE POINT OF DECLARING THESE AT ALL.
 *
 * Without them the compiler cannot check a single format string in the app:
 * every %d against a long, every %s against an int, and -- the reason they
 * were added -- every %ld against an int64_t, which is invisible on LP64 (the
 * two are the same type) and wrong on any machine where they are not. The
 * real bionic header carries the same attributes, so this is a mirror of the
 * declaration rather than a decoration on it. `make -f test/Makefile
 * wirecheck` compiles the protocol units for three other data models, and
 * these attributes are what give that compile anything to say. The macros
 * themselves are lib/compiler.h, so a compiler without the GNU extension
 * still parses this header. */
int snprintf(char *s, size_t n, const char *fmt, ...) PANCRA_PRINTF(3, 4);

/* Only used by the host-side self-test harnesses (..._TEST builds); the app
 * itself calls nothing but snprintf. */
int printf(const char *fmt, ...) PANCRA_PRINTF(1, 2);
int sscanf(const char *s, const char *fmt, ...) PANCRA_SCANF(2, 3);

#define SEEK_SET 0
#define SEEK_END 2

#endif
