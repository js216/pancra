/* SPDX-License-Identifier: GPL-3.0
 * ct.c --- constant-time comparison
 * Copyright 2026 Jakob Kastelic
 *
 * See ct.h. It lives in lib/ rather than beside one of its callers because
 * every one of them is crypto: the server's password and MAC checks, the
 * pairing confirmation, and the TLS binder and Finished. It used to be
 * declared in srv/sync.h, which srv/tls.c cannot include -- tls.h exists so
 * the TLS implementation depends on nothing above it -- so the TLS layer
 * compared its two authenticators with memcmp instead, which is the one place
 * in the program where the timing is handed straight to a stranger.
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
