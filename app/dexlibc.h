// SPDX-License-Identifier: GPL-3.0
// dexlibc.h --- Minimal libc declarations (host + freestanding)
// Copyright 2026 Jakob Kastelic

/* Minimal libc declarations, so the crypto/driver sources compile both on the
 * host (against glibc) and in the freestanding Android target build (no bionic
 * headers -- the phone's libc binds these at runtime; see stub_c.c). The ABI
 * matches glibc/bionic, so no system headers are needed. The C-standard
 * functions come from the freestanding shims (string.h/stdlib.h/stdio.h); the
 * POSIX file I/O below has no standard header and is declared here directly. */
#ifndef DEXLIBC_H
#define DEXLIBC_H

#include <stddef.h>
#include <stdint.h> /* freestanding: provided by the compiler */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* errno, and the one value we test it against.
 *
 * Here because WHY a call failed is sometimes load-bearing, not diagnostic.
 * app/sync.c enumerates a log's buckets, and any bucket missing from that
 * enumeration is pushed to the server as an empty body -- which the server
 * implements as a DELETE. So "this log has never been written" and "this log
 * cannot be read" must not look alike: the first is ordinary and the second
 * must abort the sync, and without errno an open() failure is just -1.
 *
 * The thread-local is reached through a function and the two libcs spell it
 * differently -- glibc __errno_location(), bionic __errno() -- so the hosted
 * build takes the real header and only the freestanding one declares bionic's
 * by hand, in the same spirit as the file I/O below. */
#if __STDC_HOSTED__
#include <errno.h>
#else
int *__errno(void);
#define errno  (*__errno())
#define ENOENT 2 /* Linux; this build targets nothing else */
#define EINTR  4 /* a write cut short by a signal is not a failed write */
#endif

/* POSIX file I/O, and the same split the errno block above makes -- for the
 * same reason, now applied to the whole list rather than to `rename` alone.
 *
 * WHAT THE SPLIT IS. On the phone there are no bionic headers, so these are
 * declared by hand and bound at runtime. On the HOST the real headers exist,
 * and declaring these a second time is a second declaration of a standard
 * function whose parameter names are not the libc's -- which is a finding the
 * linter reports (and reported, on the one test that included <unistd.h> and
 * these declarations at once). `rename` carried this note already; there was
 * never a reason the other seven were different, only that nothing had
 * tidied a unit that saw both.
 *
 * INCLUDED HERE, not left to the caller: every hosted unit that includes this
 * header calls some of these, and moving the include out would mean adding
 * <unistd.h> to two dozen files to say something this header already says. */
#if __STDC_HOSTED__
#include <fcntl.h>  /* open, and the O_* flags below */
#include <sched.h>  /* sched_yield */
#include <unistd.h> /* read/write/close/fsync/unlink/ftruncate/lseek */
#else
int open(const char *path, int flags, ...);
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
int close(int fd);
int fsync(int fd);
int unlink(const char *path);
int rename(const char *from, const char *to);
int ftruncate(int fd, long len);
int sched_yield(void);
long lseek(int fd, long off, int whence);
#endif
int gettid(void); /* kernel thread id (tell the main looper from BLE threads) */
#ifndef O_RDONLY
#define O_RDONLY 0U
#endif
#ifndef O_WRONLY
#define O_WRONLY 1U
#define O_CREAT  0100U
#define O_TRUNC  01000U
#endif
#ifndef O_APPEND
#define O_APPEND 02000U
#endif

#endif
