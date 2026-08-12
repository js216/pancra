/* SPDX-License-Identifier: GPL-3.0
 * pair.h --- the EC-J-PAKE pairing exchange
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef PAIR_H
#define PAIR_H

#include "sync.h" /* struct req, and the protocol constants */

/* ---- pair.c ---------------------------------------------------------- */
void h_pair(struct req *r, int round);
/* The pairing rounds share one in-memory exchange; the router holds this
 * across each round. */
void pair_lock(void);
void pair_unlock(void);
/* Mint (or replace) the displayed code for a user; `out` gets the digits. */
int pair_code_new(long uid, char *out, size_t cap);
void pair_unpair(long uid);
int pair_is_paired(long uid);

#endif
