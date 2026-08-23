/* SPDX-License-Identifier: GPL-3.0
 * pwhash.h --- the password KDF, as a function of its arguments
 * Copyright 2026 Jakob Kastelic
 *
 * ONE FUNCTION, AND IT KNOWS NOTHING ABOUT ACCOUNTS. It was declared in
 * srv/authuser.h, which is the module that stores users -- so anything that
 * merely wants to HASH a password had to include the account store, and
 * srv/pwcost.c, which times the KDF to choose its cost, formed a cycle with
 * it.
 *
 * The split is where the module genuinely divides: this is arithmetic over a
 * password, a salt and a cost, with no database, no clock and no state.
 * Everything about WHOSE password it is stays in authuser.h.
 */
#ifndef PANCRA_PWHASH_H
#define PANCRA_PWHASH_H

#include "proto.h" /* PW_*: the KDF parameters this server writes */
#include <stdint.h>

/* 1 = hashed, 0 = REFUSED with `out` untouched. The only refusable argument
 * is `iters`: zero is not a cost parameter and is not quietly computed as
 * one. See the definition in srv/authuser.c. */
int pw_hash(const char *pw, const uint8_t salt[PW_SALT_LEN], int iters,
            uint8_t out[PW_HASH_LEN]);

#endif
