// SPDX-License-Identifier: GPL-3.0
// sysabifacts.h --- what app/sysabi.h claims about the kernel ABI
// Copyright 2026 Jakob Kastelic

/* The same arrangement as app/ndkabi.h, and for the same reason: these
 * assertions name neither declaration set, so they can be compiled twice --
 * against our freestanding declarations in every app build, and against the
 * OFFICIAL <time.h> and <sys/timerfd.h> of the pinned NDK by `make ndkcheck`
 * (test/app/sysofficial.c). A number that is true of one and not the other is
 * a compile error naming the member or the constant.
 *
 * The official side maps its own constants onto these names before including
 * this file, so an assertion here is a statement about BOTH -- not about a
 * copy of our value under a different spelling. */
#ifndef PANCRA_SYSABIFACTS_H
#define PANCRA_SYSABIFACTS_H

#ifndef SYSABI_DECLS
#error                                                                         \
    "include the declarations first: app/sysabi.h, or <time.h> + <sys/timerfd.h>"
#endif

#include <stddef.h>

/* THE CLOCK IDS ARE VALUES, and the wrong one is not a compile error anywhere
 * else. Asking for REALTIME where MONOTONIC was meant hands every deadline in
 * the process to a clock the user and the network can move. */
_Static_assert(SYS_CLOCK_REALTIME == 0, "CLOCK_REALTIME");
_Static_assert(SYS_CLOCK_MONOTONIC == 1, "CLOCK_MONOTONIC");
_Static_assert(SYS_TFD_NONBLOCK == 04000, "TFD_NONBLOCK");

/* struct timespec IS THE ARGUMENT to every one of these calls, and it is
 * declared by hand here (app/time.h). Sixteen bytes with an 8-byte nanosecond
 * field: a 32-bit tv_nsec would leave the kernel reading four bytes of
 * whatever follows as the top half of a nanosecond count. */
_Static_assert(sizeof(struct timespec) == 16, "timespec is 16 bytes on LP64");
_Static_assert(offsetof(struct timespec, tv_sec) == 0, "tv_sec");
_Static_assert(offsetof(struct timespec, tv_nsec) == 8, "tv_nsec");
_Static_assert(sizeof(((struct timespec *)0)->tv_sec) == 8, "time_t is 64-bit");
_Static_assert(sizeof(((struct timespec *)0)->tv_nsec) == 8, "tv_nsec is long");

/* THE TIMER'S TWO HALVES, in the order the kernel reads them. Swapping
 * it_interval and it_value arms a repeating timer that never fires the first
 * time, or a one-shot that repeats -- both of which look like a scheduling
 * bug in whatever the timer drives. */
_Static_assert(sizeof(struct itimerspec) == 32, "itimerspec is two timespecs");
_Static_assert(offsetof(struct itimerspec, it_interval) == 0, "it_interval");
_Static_assert(offsetof(struct itimerspec, it_value) == 16, "it_value");

/* THE PROTOTYPES. Each assignment is a type check the compiler performs
 * against whichever declaration set is in scope; an incompatible one is an
 * error naming the function. `static inline` so it costs nothing and warns
 * about nothing when never called -- it exists to be COMPILED. */
static inline void sysabi_prototypes(void)
{
   int (*getclock)(int, struct timespec *) = clock_gettime;
   int (*mktimer)(int, int)                = timerfd_create;
   int (*settimer)(int, int, const struct itimerspec *, struct itimerspec *) =
       timerfd_settime;
   (void)getclock;
   (void)mktimer;
   (void)settimer;
}

#endif
