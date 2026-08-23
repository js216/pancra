/* SPDX-License-Identifier: GPL-3.0
 * randtest.h --- replacing the entropy source, in a TEST build only
 * Copyright 2026 Jakob Kastelic
 *
 * In lib/rand.h -- the header every user of randomness includes -- this is a
 * process-global mutable capability to replace the source of every key and
 * nonce, shipped in the production API, unsynchronised and consulted on every
 * single call.
 *
 * WHY IT EXISTS AT ALL: "what does this code do when the entropy source
 * fails?" cannot be asked on a machine whose /dev/urandom works. Without an
 * injectable source, the failure paths that guard an ECDSA nonce, a session
 * token and a pairing code are never once executed by any test -- and those
 * are precisely the paths where the wrong answer is silent.
 *
 * HOW IT IS KEPT OUT: lib/rand.c compiles the pointer, this setter and the
 * branch that reads it ONLY under -DRAND_TEST_SOURCE. Nothing that ships
 * defines it (`make -f test/Makefile randcheck` refuses it in the app and
 * server builds), so in a production binary rand_bytes calls the platform
 * provider directly and there is nothing to override.
 *
 * SCOPE IT. The setter returns the previous provider so a case can put the
 * real one back; a test that forgets leaves every later case running on a
 * source it did not choose.
 */
#ifndef PANCRA_RANDTEST_H
#define PANCRA_RANDTEST_H

#include <stddef.h>
#include <stdint.h>

#ifndef RAND_TEST_SOURCE
#error "lib/randtest.h is a test-only header: build with -DRAND_TEST_SOURCE"
#endif

typedef int (*rand_source_fn)(uint8_t *buf, size_t n);

/* Replace the provider; returns the previous one. NULL restores the
 * platform's. */
rand_source_fn rand_set_source(rand_source_fn src);

#endif
