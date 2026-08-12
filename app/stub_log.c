// SPDX-License-Identifier: GPL-3.0
// stub_log.c --- Link-time stub for liblog.so
// Copyright 2026 Jakob Kastelic

/* Link-time stub for the device's liblog.so (see stub_android.c). */
#include "stub_log.h"

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}
