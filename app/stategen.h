// SPDX-License-Identifier: GPL-3.0
// stategen.h --- the generation stamp a backup pulls to know it got one moment
// Copyright 2026 Jakob Kastelic

/* A backup pulls the phone's files one at a time. Each arrives whole, but the
 * SET need not: a reading appended between the pull of readings.csv and the
 * pull of sensors.csv leaves an archive whose halves describe two different
 * moments, and a restore from it is a restore of a state the phone never had.
 *
 * The app cannot be asked to hold still (nothing outside it can take its
 * locks, and adb has no way to call into it), so it publishes a GENERATION
 * instead: one small file whose contents change whenever any synced record
 * does. Read before and after a pull, two readings that agree are the
 * evidence that the archive describes one moment.
 *
 * WHAT THE STAMP IS: syncjni_state_stamp(), the same number the sync
 * scheduler already uses to answer "is there anything new" -- every synced
 * file's size mixed with the process's mutation counter. It changes on every
 * append, every edit and every delete, and it is computed by the app from the
 * files it owns, under nothing (sizes are read with one lseek each and the
 * counter is atomic), so publishing it costs a stat per file on a tick.
 *
 * WHAT IT IS NOT: a lock. A phone written to WHILE the archive is pulled
 * produces two different stamps, and the answer to that is to pull again; it
 * does not make the phone hold still, because a backup tool that could stop a
 * medical device recording is a worse idea than a backup that occasionally
 * repeats.
 */
#ifndef PANCRA_STATEGEN_H
#define PANCRA_STATEGEN_H

/* Where the stamp is written, under the data directory. 1 if the path fitted
 * (data_path's rule -- see util.h); the module does nothing at all if not. */
int stategen_paths(const char *dir);

/* Publish the current stamp IF IT HAS CHANGED. Called from the ticks that
 * already run every second; a tick that finds nothing changed does one stat
 * per synced file and no I/O at all. */
void stategen_tick(void);

#endif
