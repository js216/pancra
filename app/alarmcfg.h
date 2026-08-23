// SPDX-License-Identifier: GPL-3.0
// alarmcfg.h --- the alarm and nudge thresholds, and their file
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
 * preferences is alarm_set_thresholds here, so a reader can tell from the call
 * which file it persists to.
 */
#ifndef PANCRA_ALARMCFG_H
#define PANCRA_ALARMCFG_H

#include "loadresult.h"

int alarm_set_thresholds(int alarm_low, int alarm_high, int nudge_low,
                         int nudge_high);


enum load_result alarm_load(void);


#endif
