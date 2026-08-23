// SPDX-License-Identifier: GPL-3.0
// sysabi.h --- the kernel clock and timer ABI, declared once and checked
// Copyright 2026 Jakob Kastelic

/* THE CLOCK AND TIMER BOUNDARY, in one place.
 *
 * This build is freestanding: there is no bionic header on the include path,
 * so every kernel call it makes has to be declared by hand, and a hand-written
 * declaration of somebody else's ABI is a claim no compiler checks. These were
 * spread across three files -- CLOCK_REALTIME and CLOCK_MONOTONIC defined
 * privately in app/clock.c, CLOCK_MONOTONIC defined AGAIN in app/main.c beside
 * its own copy of `struct itimerspec` and the timerfd prototypes, and
 * TFD_NONBLOCK written as the bare octal 04000 at the one call site. Four
 * declarations of one interface, none of which could disagree loudly: a
 * constant that drifts asks the kernel for a different clock, and a struct
 * whose members are the wrong width arms a timer at a time nobody chose.
 *
 * What a mistake here costs is worth saying, because none of it looks like a
 * timer bug: CLOCK_MONOTONIC and CLOCK_REALTIME swapped makes every deadline
 * in the process jump when the phone syncs its clock -- the repaint stalls,
 * the stale-data alarm re-arms itself, and a session's elapsed time steps by
 * hours. The screen still draws. Nothing logs anything.
 *
 * So: one declaration, named wrapper operations for the two things the app
 * actually does with a timer, and the numbers asserted against the OFFICIAL
 * headers of the pinned NDK by `make ndkcheck` (app/sysabifacts.h). */
#ifndef PANCRA_SYSABI_H
#define PANCRA_SYSABI_H

#include <time.h> /* struct timespec: our shim freestanding, the system's on the host */

/* THE CLOCK IDS. Two, and the difference between them is the difference
 * between "what time is it" and "how long has it been" -- see clock.h, which
 * is where the app's rule about which to use for what lives. */
#define SYS_CLOCK_REALTIME  0
#define SYS_CLOCK_MONOTONIC 1

/* timerfd_create: do not block when the timer has not fired. The looper polls
 * the descriptor, so a blocking read here would park the MAIN thread. */
#define SYS_TFD_NONBLOCK 04000

#if !__STDC_HOSTED__
/* THE FREESTANDING SIDE: no bionic headers exist, so these are the
 * declarations, and libc binds them at runtime (see app/stub_c.c).
 *
 * (An editor configured with the normal Android sysroot sees BOTH this and
 * the system <time.h> and reports a redefinition of itimerspec; that is a
 * tooling mismatch, not a build defect -- the compiler here has no system
 * header.) */
struct itimerspec {
   struct timespec it_interval, it_value;
};

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *nv,
                    struct itimerspec *ov);
#else
#include <sys/timerfd.h> /* the host has the real thing; use it */
#endif

/* ---- the named operations, which is all the app should need ----
 *
 * Written as verbs rather than as a struct to fill in at each call site: the
 * one place that armed a timer by hand set four fields in the right order and
 * the other set two, and neither said what the number meant. */

/* A monotonic, non-blocking timer descriptor, or -1. The caller owns the fd
 * and closes it. */
int sys_timer_open(void);

/* Fire after `first_ms`, then every `repeat_ms` (0 = one shot). Returns 1 on
 * success. Both are milliseconds, which is what every caller in this app
 * actually has; the nanosecond arithmetic the kernel wants happens once,
 * here, rather than at each call site. */
int sys_timer_arm_ms(int fd, long first_ms, long repeat_ms);

#define SYSABI_DECLS 1
#include "sysabifacts.h" /* the numbers above, as assertions */

#endif
