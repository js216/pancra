// SPDX-License-Identifier: GPL-3.0
// syncreport.h --- how a sync went, reported back
// Copyright 2026 Jakob Kastelic
#ifndef PANCRA_SYNCREPORT_H
#define PANCRA_SYNCREPORT_H

/*
 * ONE CALLBACK, IN A HEADER OF ITS OWN, and that is the whole point.
 *
 * remote.c asks the Java worker to run a sync (syncjni.h); the worker reports
 * the outcome back into remote.c. Declaring the report in syncjni.h made the
 * JNI bridge speak for a module it does not implement; declaring it in
 * remote.h made syncjni.c include remote.h while remote.c includes
 * syncjni.h -- a cycle either way.
 *
 * The direction is not the problem: a callback is a legitimate upward call.
 * What matters is that neither module names the other's header to make it.
 */
/* Report the outcome of a sync, a pair or a restore: the LAST SYNC and LAST
 * STATUS rows, the retry schedule, and a repaint. Defined in remote.c, which
 * owns the schedule. `outcome` is an enum sync_outcome (syncstat.h) -- a code,
 * not a sentence, so the screen can colour it and the scheduler can tell a
 * failure worth retrying from one only the user can fix.
 *
 * Without this the rows are dead text: they used to be fed by the old push
 * path, and when that went, nothing replaced it -- so a working sync and a
 * server that refused every request looked exactly alike. */
void sync_report(int outcome);

#endif
