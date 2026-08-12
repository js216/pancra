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

#include <stddef.h>

int snprintf(char *s, size_t n, const char *fmt, ...);

/* Only used by the host-side self-test harnesses (..._TEST builds); the app
 * itself calls nothing but snprintf. */
int printf(const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);

#define SEEK_SET 0
#define SEEK_END 2

#endif
