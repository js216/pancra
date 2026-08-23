// SPDX-License-Identifier: GPL-3.0
// devinfo.h --- the device-information strings the sensor reports
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
 * preferences is info_set here, so a reader can tell from the call which
 * file it persists to.
 */
#ifndef PANCRA_DEVINFO_H
#define PANCRA_DEVINFO_H

#include "loadresult.h"

/* The device-information strings the sensor reports, stored and persisted in
 * one call. Learned from the device, not chosen by the user. */
enum { SET_DIS_MODEL = 0, SET_DIS_FW = 1, SET_DIS_MFR = 2 };

int info_set(int which, const char *val);


/* Read the file back into the live state at startup. */
enum load_result info_load(void);

#endif
