// SPDX-License-Identifier: GPL-3.0
// log.h --- The one logcat declaration
// Copyright 2026 Jakob Kastelic
//
/* The prototype and these two macros live HERE, not repeated in every file
 * that logs: repeated, one of them drifts (a different tag, a different
 * priority) and the logs stop being greppable as one stream.
 *
 * AND "HERE" IS NOW TRUE. The header said this while SEVEN
 * production files -- dexble.c, dexlink.c, dexproto.c, otble.c, settings.c,
 * store.c and syncjni.c -- each carried their own copy of the prototype and
 * of whichever macros they used. Nothing forced them to agree: a file that
 * logged at priority 4 through a macro called LOGW, or under a tag of its
 * own, would compile and link perfectly and its lines would simply stop
 * appearing where everybody looks for them. They include this now, and
 * `make lockcheck` refuses a new copy (see test/app/lockcheck.sh).
 *
 * THE STUB IS AN IMPLEMENTATION OF THIS CONTRACT, not a second statement of
 * it: app/stub_log.h includes this header rather than repeating the
 * prototype, so the .so's ABI and the callers' expectation are one
 * declaration. */
#ifndef PANCRA_LOG_H
#define PANCRA_LOG_H

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

#endif
