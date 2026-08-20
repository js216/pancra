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

struct db; /* db.h: every storage call names its database */

#include "proto.h" /* struct req, and the protocol constants */

/* ---- pair.c ---------------------------------------------------------- */
void h_pair(struct req *r, int round);
/* The pairing rounds share one in-memory exchange; the router holds this
 * across each round. */
void pair_lock(void);
void pair_unlock(void);
/* Mint (or replace) the displayed code for a user; `out` gets the digits. */
int pair_code_new(struct db *d, long uid, char *out, size_t cap);
/* Revoke the paired app's key. 1 when it is GONE from the database; 0 when
 * the delete did not run, in which case the old key still signs requests and
 * the caller must not say the app is unpaired. */
int pair_unpair(struct db *d, long uid);
/* IS THIS ACCOUNT PAIRED? Three answers, because a database that cannot say
 * is not the same as an account with no app: 1 paired, 0 not paired, -1 the
 * question could not be answered. A caller that treats -1 as "not paired"
 * offers to pair a phone that is already paired, and one that treats it as
 * "paired" hides the pairing screen from someone who needs it. */
int pair_is_paired(struct db *d, long uid);

#endif
