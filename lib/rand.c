/* SPDX-License-Identifier: GPL-3.0
 * rand.c --- the entropy boundary: ask the provider, check the answer
 * Copyright 2026 Jakob Kastelic
 *
 * See rand.h. There is nothing clever here, and that is the point: this used
 * to exist four times over -- in the pairing, in the BLE driver, in the
 * server's utilities and in the TLS layer -- and the copies disagreed about
 * whether a short read was worth retrying and whether a failure was fatal.
 * A short read from /dev/urandom is rare enough that a copy which ignores it
 * looks correct for years.
 *
 * WHAT IS NOT HERE ANY MORE: the operating system. This file used to open
 * /dev/urandom and hand-declare open/read/close for the freestanding build.
 * Those live in the platform provider now (lib/randunix.c), and this file --
 * the one every piece of crypto in lib/ depends on -- names no syscall, no
 * device and no header outside the C standard's freestanding subset.
 */
#include "rand.h"
#include "entropy.h"
#include <stddef.h>
#include <stdint.h>

/* NULL means "the platform's own", resolved at the call rather than stored
 * here: a pointer initialised to entropy_fill would be a second place that
 * decides the default, and the two could disagree after a test forgot to
 * restore. */
static rand_source_fn g_src;

rand_source_fn rand_set_source(rand_source_fn src)
{
   rand_source_fn prev = g_src;
   g_src               = src;
   return prev;
}

int rand_bytes(uint8_t *buf, size_t n)
{
   if (!buf)
      return 0;
   if (n == 0)
      return 1; /* nothing asked for is nothing to fail at */
   /* THE PROVIDER'S ANSWER IS NOT TAKEN ON TRUST. A source that reports
    * success is required to have filled every byte; this wrapper is where
    * that contract is stated, so each provider does not restate it. */
   int ok = g_src ? g_src(buf, n) : entropy_fill(buf, n);
   return ok ? 1 : 0;
}
