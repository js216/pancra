// SPDX-License-Identifier: GPL-3.0
// log.h --- The one logcat declaration
// Copyright 2026 Jakob Kastelic
//
/* Every file that logs used to repeat this prototype and these two macros.
 * That is fine until one of them drifts (a different tag, a different
 * priority) and the logs stop being greppable as one stream. */
#ifndef PANCRA_LOG_H
#define PANCRA_LOG_H

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

#endif
