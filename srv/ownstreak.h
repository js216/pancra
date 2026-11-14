/* SPDX-License-Identifier: GPL-3.0
 * ownstreak.h --- an account's in-range streak, from its synced rows
 * Copyright 2026 Jakob Kastelic
 *
 * The phone's streak line, computed here from the same rows by the same rules
 * (lib/streak.h): the readings log for the samples and the lead column, the
 * sensors log for each sensor's session start.
 */
#ifndef OWNSTREAK_H
#define OWNSTREAK_H

#include "db.h"

#include <stdint.h>

/* The run ending at the newest counted reading, in *cur, and the longest run
 * in the whole log, in *best -- both in seconds. 1 when the logs were read
 * whole, 0 when they could not be, and then neither is set.
 *
 * A WHOLE-LOG READ, so the answer is cached per account and reused until a
 * bucket write commits (logs_gen). */
int owner_streak(struct db *d, int64_t owner, long *cur, long *best);

#endif
