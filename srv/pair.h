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
/* ONE PAIRING ROUND, SERIALISED BY THIS MODULE.
 *
 * The four rounds share one in-memory exchange, and they land on different
 * workers. Left to the CALLER -- a lock/unlock pair exported from this
 * header, with every caller bracketing its own call -- it is a rule a header
 * states and nothing enforces, and it breaks: the settings page's Unpair path
 * calls pair_unpair() without the
 * lock, and pair_unpair resets the exchange, so clicking Unpair while a phone
 * was pairing freed a crypto context another worker was using.
 *
 * Every entry point below now takes the lock itself. There is nothing to
 * bracket and nothing to forget. */
void h_pair(struct req *r, int round);
/* Mint (or replace) the displayed code for a user; `out` gets the digits. */
int pair_code_new(struct db *d, int64_t uid, char *out, size_t cap);
/* Revoke the paired app's key. 1 when it is GONE from the database; 0 when
 * the delete did not run, in which case that key still signs requests and
 * the caller must not say the app is unpaired. */
int pair_unpair(struct db *d, int64_t uid);
/* IS THIS ACCOUNT PAIRED? Three answers, because a database that cannot say
 * is not the same as an account with no app: 1 paired, 0 not paired, -1 the
 * question could not be answered. A caller that treats -1 as "not paired"
 * offers to pair a phone that is already paired, and one that treats it as
 * "paired" hides the pairing screen from someone who needs it. */
int pair_is_paired(struct db *d, int64_t uid);

#endif
