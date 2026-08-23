// SPDX-License-Identifier: GPL-3.0
// paircode.h --- the pairing code this phone claimed its sensor with
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN PER HEADER. A header declaring five unrelated files'
 * worth of state -- the device's model and firmware, the alarm thresholds,
 * the display preferences, the pairing code and the remote credentials --
 * makes every file that wants one of them
 * included all five. They share a save engine (private: app/setpriv.h) and
 * the preferences aggregate; they share nothing else.
 *
 * THE NAMES MOVED WITH THE DECLARATIONS. A setter that writes THIS file is
 * named for it: what was settings_set_* and lived beside the display
 * preferences is code_set here, so a reader can tell from the call which
 * file it persists to.
 */
#ifndef PANCRA_PAIRCODE_H
#define PANCRA_PAIRCODE_H

#include "loadresult.h"

/* The pairing code, stored and persisted in one call. */
int code_set(const char *digits);

enum load_result code_load(void);


#endif
