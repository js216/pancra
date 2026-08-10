// SPDX-License-Identifier: GPL-3.0
// stub_c.c --- Link-time stub for bionic libc.so
// Copyright 2026 Jakob Kastelic

/* Link-time stub for the device's bionic libc.so (see stub_android.c).
 * Add symbols here as the native code starts using them.
 */
#include "stub_c.h"
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *memset(void *d, int c, size_t n)
{
   (void)c;
   (void)n;
   return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
   (void)s;
   (void)n;
   return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
   (void)a;
   (void)b;
   (void)n;
   return 0;
}

int snprintf(char *s, size_t n, const char *f, ...)
{
   (void)s;
   (void)n;
   (void)f;
   return 0;
}

int strcmp(const char *a, const char *b)
{
   (void)a;
   (void)b;
   return 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
   (void)a;
   (void)b;
   (void)n;
   return 0;
}

int clock_gettime(int clk, struct timespec *ts)
{
   (void)clk;
   (void)ts;
   return 0;
}

int gettid(void)
{
   return 0;
}

void *malloc(size_t n)
{
   (void)n;
   return 0;
}

void *realloc(void *p, size_t n)
{
   (void)p;
   (void)n;
   return 0;
}

void *calloc(size_t nmemb, size_t size)
{
   (void)nmemb;
   (void)size;
   return 0;
}

void free(void *p)
{
   (void)p;
}

int open(const char *p, int f, ...)
{
   (void)p;
   (void)f;
   return -1;
}

long read(int fd, void *b, size_t n)
{
   (void)fd;
   (void)b;
   (void)n;
   return -1;
}

long write(int fd, const void *b, size_t n)
{
   (void)fd;
   (void)b;
   (void)n;
   return -1;
}

int close(int fd)
{
   (void)fd;
   return 0;
}

int rename(const char *from, const char *to)
{
   (void)from;
   (void)to;
   return 0;
}

int unlink(const char *p)
{
   (void)p;
   return 0;
}

int ftruncate(int fd, long len)
{
   (void)fd;
   (void)len;
   return 0;
}

int sched_yield(void)
{
   return 0;
}

void (*signal(int s, void (*h)(int)))(int)
{
   (void)s;
   return h;
}

int raise(int s)
{
   (void)s;
   return 0;
}

long lseek(int fd, long off, int w)
{
   (void)fd;
   (void)off;
   (void)w;
   return 0;
}

int timerfd_create(int c, int f)
{
   (void)c;
   (void)f;
   return -1;
}

int timerfd_settime(int fd, int f, const void *n, void *o)
{
   (void)fd;
   (void)f;
   (void)n;
   (void)o;
   return 0;
}
