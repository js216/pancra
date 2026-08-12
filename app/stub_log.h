// SPDX-License-Identifier: GPL-3.0
// stub_log.h --- liblog.so stub declarations
// Copyright 2026 Jakob Kastelic

/* Declaration for the symbol defined in stub_log.c. This is the ABI the stub
 * liblog.so exports; the phone's real liblog binds it at runtime. */
#ifndef STUB_LOG_H
#define STUB_LOG_H

int __android_log_print(int prio, const char *tag, const char *fmt, ...);

#endif
