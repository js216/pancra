// SPDX-License-Identifier: GPL-3.0
// posix.h --- the ONE place this program leaves ISO C
// Copyright 2026 Jakob Kastelic

/* WHY THIS EXISTS.
 *
 * Everything else in this repository compiles as `-std=c11`: the app, the
 * shared crypto and plotting primitives in lib/, and every server file but
 * one. That is a property worth keeping mechanically rather than by
 * intention, because the alternative -- a global `-std=gnu11` -- extends GNU
 * semantics over 35k lines to satisfy a handful of calls in the server, and
 * nothing then says which lines they were. A file that quietly starts using
 * a GNU builtin, a GNU-only libc function, or GNU `inline` rules compiles
 * silently and cannot be moved to another toolchain later without finding
 * out the hard way.
 *
 * So: srv/posix.c is compiled with `-std=gnu11` and is the only translation
 * unit that is. Everything the server needs from beyond ISO C comes through
 * the declarations below, and the boundary is checked by `make stdcheck`.
 *
 * WHAT IS ACTUALLY BEYOND ISO C, and why each one is here:
 *
 *   timegm(3)        -- NOT in POSIX at all (a GNU/BSD extension). The
 *                       archive pages name a calendar DAY, and turning
 *                       Y/M/D into a UTC instant without it means either
 *                       mktime (which applies the machine's local zone --
 *                       wrong, and different on every board) or hand-rolled
 *                       civil arithmetic.
 *   gmtime_r(3)      -- POSIX, not ISO C. gmtime(3) IS ISO C and returns a
 *                       pointer into a static buffer: on a threaded server
 *                       that is a data race between two workers rendering
 *                       two pages.
 *   strcasecmp(3),   -- POSIX (strings.h), not ISO C. HTTP field names are
 *   strncasecmp(3)      case-insensitive by RFC 9110 5.1, and a hand-rolled
 *                       tolower loop is locale-dependent in a way this is
 *                       not supposed to be.
 *   MSG_NOSIGNAL     -- Linux/POSIX-2008 send(2) flags. Without NOSIGNAL a
 *   MSG_DONTWAIT        peer that hung up kills the PROCESS with SIGPIPE;
 *                       without DONTWAIT a full socket buffer blocks a
 *                       worker past its own deadline.
 *   PATH_MAX         -- POSIX, and not guaranteed to be defined at all. The
 *                       server compares canonical paths, so it needs ONE
 *                       number both sides of the comparison agree on.
 *
 * WHAT DOES NOT BELONG HERE: anything the C standard already provides, and
 * anything only one caller wants. This is a boundary, not a utility drawer;
 * srv/util.h is where server helpers go. */
#ifndef PANCRA_POSIX_H
#define PANCRA_POSIX_H

#include <stddef.h> /* size_t */
#include <time.h>   /* time_t, struct tm: ISO C types, ISO C header */

/* THE LENGTH OF THE LONGEST PATH THIS SERVER WILL HANDLE.
 *
 * Not `PATH_MAX` at each call site: limits.h may not define it (POSIX allows
 * a filesystem with no fixed limit to omit it), and a server whose buffers
 * change size with the build host is a server whose canonical-path
 * comparisons mean something different on the board. 4096 is Linux's own
 * value; a path longer than this is refused, not truncated. */
#define SYS_PATH_MAX 4096

/* UTC broken-down time, thread-safely. 1 on success; 0 when the time_t
 * cannot be represented as a date, with `out` UNTOUCHED -- a caller that
 * ignores the answer would otherwise print whatever its stack held. */
int sys_gmtime(time_t t, struct tm *out);

/* The inverse: a UTC calendar date to an instant, or (time_t)-1. The local
 * time zone is not consulted, which is the whole reason this is not
 * mktime(3). */
time_t sys_timegm(struct tm *tm);

/* ASCII case-insensitive equality, for HTTP field names. Nonzero when equal
 * -- the sense of the C library's comparators is inverted deliberately, so
 * `if (sys_caseeq(a, b))` reads as what it tests. */
int sys_caseeq(const char *a, const char *b);
int sys_ncaseeq(const char *a, const char *b, size_t n);

/* send(2) that cannot raise SIGPIPE and cannot block. Returns what send does:
 * the count, or -1 with errno set (EAGAIN when the socket is full, EPIPE when
 * the peer is gone). */
long sys_send_quiet(int fd, const void *buf, size_t n);

#endif
