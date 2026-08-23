// SPDX-License-Identifier: GPL-3.0
// logsload.h --- re-read every log from disk, after a restore
// Copyright 2026 Jakob Kastelic
//
/* ONE CALLER, ONE PURPOSE. sync_restore writes the log FILES
 * directly -- that is what a restore is -- and nothing in memory knows the
 * rows arrived. Without this the plot, the statistics and the history still
 * show what they showed before, which looks exactly like a restore that did
 * nothing.
 *
 * It is not in reading.c because it has nothing to do with a reading: it
 * reloads the registry, the history, the statistics, the insulin, the weight,
 * the food and its vocabulary, and the exercise -- seven modules, whose
 * headers reading.c was including for this one function. The ORDER it does
 * them in is the whole content of this module, and the reasons are in the
 * implementation beside each step.
 */
#ifndef LOGSLOAD_H
#define LOGSLOAD_H

/* Re-read every log from disk. Only the RESTORE path needs it. */
void pancra_logs_reload(void);

#endif
