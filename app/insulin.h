// SPDX-License-Identifier: GPL-3.0
// insulin.h --- Insulin dose log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_INSULIN_H
#define PANCRA_INSULIN_H

/* A dose is what the user TYPED, not something a sensor measured, so this log
 * is deliberately minimal: when, which kind, how much. The file is the
 * lifetime record and the in-memory tail exists for the UI -- pre-populating
 * the entry form with the last dose of a type, and any future plotting.
 *
 * THE FILE IS APPEND-ONLY, INCLUDING EDITS AND DELETES (schema v2). It used
 * to be rewritten in place by the edit form, which made it the one log that
 * could lose history to a bug, and left it unable to say what a dose used to
 * be. Now every row is an ASSERTION about a dose identified by an id:
 *
 *     written,id,del,unix_time,type,units,tz_offset_s
 *
 * Replay in file order, last assertion per id wins, del=1 removes it. So
 * "6 units at 08:12, corrected to 4 at 08:15" is two rows and both survive,
 * and a past day's rows never change -- which is also what lets the sync
 * protocol treat old buckets as frozen.
 *
 * Rows in the OLD four-field form (unix_time,type,units,tz_offset_s) still
 * load, and are given negative ids by file order so they can never collide
 * with a minted one. Nothing rewrites them. */

#define INS_SLOW 0 /* basal / long-acting */
#define INS_FAST 1 /* bolus / rapid-acting */

#define INS_UNITS_MIN 1
#define INS_UNITS_MAX 99

/* Epoch bound on a dose instant: any positive time up to year ~3000. Wide on
 * purpose (backdating and small future-dating are legitimate user entries);
 * it exists so a corrupt digit run can never parse as a plausible dose. */
#define INS_T_MAX 32503680000L

/* In-memory tail only -- the file keeps everything. 256 rows is over two
 * months of a heavy 4-dose day. */
#define NINS 256

struct ins_rec {
   long t;    /* dose instant, epoch seconds */
   int type;  /* INS_SLOW / INS_FAST */
   int units; /* whole units, INS_UNITS_MIN..INS_UNITS_MAX */
};

extern struct ins_rec g_ins[NINS]; /* oldest first; the newest is at the end */
extern int g_nins;
extern char g_ins_path[256];

/* Load the tail of the log (the last NINS plausible rows). Safe on a fresh
 * install: a missing file is an empty log. */
void insulin_load(void);

/* Append one dose durably and mirror it into the tail. Returns 0 on success,
 * -1 when the write failed (the tail is then left untouched, so memory never
 * claims a dose the file does not have). Rejects out-of-range input. */
int insulin_append(long t, int type, int units, long tz);

/* The units of the most recent dose of `type`, or 0 if none is known --
 * what the LOG INSULIN form pre-populates with. */
int insulin_last_units(int type);

/* Correct or retract the LAST dose matching `orig` (t/type/units) by
 * APPENDING an assertion against its id -- the file is never rewritten. 0 on
 * success, -1 when nothing matches or the write failed. Matching is by
 * CONTENT, so a stale tail index can never touch the wrong dose. */
int insulin_update(const struct ins_rec *orig, long t, int type, int units,
                   long tz);
int insulin_delete(const struct ins_rec *orig);

const char *insulin_type_name(int type); /* "SLOW" / "FAST" */

#endif
