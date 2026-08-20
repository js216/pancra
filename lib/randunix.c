/* SPDX-License-Identifier: GPL-3.0
 * randunix.c --- the Unix entropy provider: /dev/urandom, read whole
 * Copyright 2026 Jakob Kastelic
 *
 * THE ONLY FILE IN lib/ THAT KNOWS WHAT AN OPERATING SYSTEM IS.
 *
 * entropy.h holds the contract and lib/'s crypto holds none of this; a port
 * to a platform with a different entropy source replaces this file, without
 * touching a line of the algorithms.
 */
#include "entropy.h"
#include <stddef.h>
#include <stdint.h>

/* ONE copy, compiled by both halves. The app builds -ffreestanding, where no
 * libc declares open/read/close; the server is an ordinary hosted program and
 * takes them from the system. That is the entire difference, and
 * __STDC_HOSTED__ asks exactly that question, so no build flag is needed to
 * tell them apart.
 *
 * The freestanding branch declares the three syscalls ITSELF rather than
 * including app/dexlibc.h. lib/ is a self-contained collection of primitives,
 * and a file in it naming a header in app/ is the dependency pointing the
 * wrong way -- it compiled only because the app build happens to pass -Iapp,
 * and the server's own include path could not have resolved it. Three
 * prototypes are a smaller price than that edge. */
#if __STDC_HOSTED__
#include <fcntl.h>
#include <unistd.h>
#else
int open(const char *path, int flags, ...);
long read(int fd, void *buf, size_t n);
int close(int fd);
#ifndef O_RDONLY
#define O_RDONLY 0U
#endif
#endif

int entropy_fill(uint8_t *buf, size_t n)
{
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd < 0)
      return 0;
   size_t off = 0;
   while (off < n) {
      long r = read(fd, buf + off, n - off);
      if (r <= 0)
         break; /* a short read is a failure, not something to paper over */
      off += (size_t)r;
   }
   close(fd);
   return off == n;
}
