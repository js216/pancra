/* SPDX-License-Identifier: GPL-3.0
 * posix.c --- the ONE translation unit that asks for more than ISO C
 * Copyright 2026 Jakob Kastelic
 *
 * See posix.h for what is here and why. Every function is a thin wrapper: no
 * policy, no buffers, no state. That is deliberate -- a compatibility module
 * that also decides things is a module every reviewer has to read twice, and
 * the point of this one is that it can be read once and then trusted.
 */
/* timegm is BSD's, not POSIX's, so _XOPEN_SOURCE alone does not declare it.
 * Asked for HERE rather than in the build, so the one file that needs a
 * non-standard declaration is the one that says so. Before any include. */
#define _DEFAULT_SOURCE
#include "posix.h"
#include <strings.h>    /* strcasecmp, strncasecmp */
#include <sys/socket.h> /* send, MSG_* */

int sys_gmtime(time_t t, struct tm *out)
{
   if (!out)
      return 0;
   /* gmtime_r LEAVES `out` ALONE ON FAILURE, and callers rely on that: the
    * struct they passed is usually on the stack, and a failure that half-fills
    * it renders a date beside a real reading. */
   return gmtime_r(&t, out) != 0;
}

time_t sys_timegm(struct tm *tm)
{
   if (!tm)
      return (time_t)-1;
   return timegm(tm);
}

int sys_caseeq(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   return strcasecmp(a, b) == 0;
}

int sys_ncaseeq(const char *a, const char *b, size_t n)
{
   if (!a || !b)
      return 0;
   return strncasecmp(a, b, n) == 0;
}

long sys_send_quiet(int fd, const void *buf, size_t n)
{
   /* MSG_NOSIGNAL: a peer that hung up must fail THIS call with EPIPE rather
    * than kill the process. MSG_DONTWAIT: a full socket buffer must be an
    * EAGAIN the caller can time out on, not a block past its own deadline. */
   return (long)send(fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
}
