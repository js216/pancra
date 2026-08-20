// SPDX-License-Identifier: GPL-3.0
// stub_c.c --- Link-time symbol stub for Android's libc.so
// Copyright 2026 Jakob Kastelic

/* These bodies never run: the APK records libc.so as a dependency and
 * Android's dynamic linker resolves every symbol to bionic. Keep only the
 * names referenced by the freestanding native build. */
#define STUB(name)                                                             \
   void name(void)                                                             \
   {                                                                           \
   }

STUB(__errno)
STUB(clock_gettime)
STUB(close)
STUB(fsync)
STUB(ftruncate)
STUB(gettid)
STUB(lseek)
STUB(open)
STUB(raise)
STUB(read)
STUB(rename)
STUB(sched_yield)
STUB(signal)
STUB(timerfd_create)
STUB(timerfd_settime)
STUB(unlink)
STUB(write)

typedef __SIZE_TYPE__ size_t;

void *calloc(size_t n, size_t size)
{
   (void)n;
   (void)size;
   return 0;
}

void free(void *p)
{
   (void)p;
}

void *malloc(size_t n)
{
   (void)n;
   return 0;
}

int memcmp(const void *a, const void *b, size_t n)
{
   return a == b ? 0 : (int)n;
}

void *memcpy(void *dst, const void *src, size_t n)
{
   (void)src;
   (void)n;
   return dst;
}

/* memmove is here for the same reason memcpy is, and for one more: a caller
 * whose ranges may overlap must not use memcpy, whose contract forbids it and
 * which a compiler may optimise on that promise. The app build links
 * -nostdlib against this stub only to satisfy --no-undefined; bionic supplies
 * the working implementation at run time. A symbol missing here is a link
 * error rather than a silent fallback, which is why it has to be added
 * deliberately when a new libc call appears. */
void *memmove(void *dst, const void *src, size_t n)
{
   (void)src;
   (void)n;
   return dst;
}

void *memset(void *dst, int c, size_t n)
{
   (void)c;
   (void)n;
   return dst;
}

void *realloc(void *p, size_t n)
{
   (void)n;
   return p;
}

int snprintf(char *dst, size_t n, const char *fmt, ...)
{
   (void)dst;
   (void)n;
   (void)fmt;
   return 0;
}

int strcmp(const char *a, const char *b)
{
   return a == b ? 0 : 1;
}

int strncmp(const char *a, const char *b, size_t n)
{
   return a == b ? 0 : (int)n;
}
