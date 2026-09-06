// SPDX-License-Identifier: GPL-3.0
// stub_log.c --- Link-time stub for liblog.so
// Copyright 2026 Jakob Kastelic

/* Link-time stub for the device's liblog.so (see stub_android.c). */
/* EXPORTED: these are link-time stand-ins for the device's own
 * libraries, and a hidden symbol is not one the linker can stand in for. */
#pragma GCC visibility push(default)

/* The declaration comes through this header rather than from log.h direct,
 * which is the arrangement log.h itself documents: the stub has its own
 * named entry point. NOLINT: include-cleaner sees only the indirection. */
#include "stub_log.h" // NOLINT(misc-include-cleaner)

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

#pragma GCC visibility pop
