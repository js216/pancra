// SPDX-License-Identifier: GPL-3.0
// bondtable.h --- the last bond state reported per address
// Copyright 2026 Jakob Kastelic
#ifndef PANCRA_BONDTABLE_H
#define PANCRA_BONDTABLE_H

/* What the OS last said about this address's bond, or 0 if it has never said
 * anything about it. Safe from any thread: the value is COPIED out under the
 * table's own lock, so the caller never holds a pointer into it. */
int dexble_bond_state(const char *mac);

/* Record a transition. Called from the binder thread that receives the OS's
 * broadcast. */
void bond_state_set(const char *mac, int state);

#endif
