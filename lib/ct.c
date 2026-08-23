/* SPDX-License-Identifier: GPL-3.0
 * ct.c --- constant-time comparison
 * Copyright 2026 Jakob Kastelic
 *
 * See ct.h. It lives in lib/ rather than beside one of its callers because
 * every one of them is crypto: the server's password and MAC checks, the
 * pairing confirmation, and the TLS binder and Finished. Declared in
 * srv/proto.h it is out of reach of srv/tls.c -- tls.h exists so the TLS
 * implementation depends on nothing above it -- leaving the TLS layer to
 * compare its two authenticators with memcmp, which is the one place in the
 * program where the timing is handed straight to a stranger.
 */
#include "ct.h"
#include <stddef.h> /* size_t: the length this compares over */
#include <stdint.h>

int ct_eq(const void *a, const void *b, size_t n)
{
   const uint8_t *x = a;
   const uint8_t *y = b;
   uint8_t d        = 0;
   for (size_t i = 0; i < n; i++)
      d |= (uint8_t)(x[i] ^ y[i]);
   return d == 0;
}

/* See ct.h. The volatile pointer is the whole mechanism: an implementation is
 * required to perform accesses through it, so the stores cannot be removed as
 * dead. Written a byte at a time deliberately -- calling memset through the
 * volatile pointer is not possible (memset takes a plain void *), and casting
 * the volatility away to call it is precisely the thing that lets the store be
 * elided again. */
void ct_wipe(void *p, size_t n)
{
   if (!p)
      return;
   volatile unsigned char *q = (volatile unsigned char *)p;
   while (n--)
      *q++ = 0;
}
