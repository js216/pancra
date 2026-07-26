// SPDX-License-Identifier: GPL-3.0
// insulin.h --- Insulin dose log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_INSULIN_H
#define PANCRA_INSULIN_H

/* A dose is what the user TYPED, not something a sensor measured, so this log
 * is deliberately minimal: when, which kind, how much. The file is the
 * lifetime record (append-only, never rewritten, same discipline as
 * readings.csv); the in-memory tail exists for the UI -- pre-populating the
 * entry form with the last dose of a type, and any future plotting. */

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

/* Rewrite the LAST file row matching `orig` (t/type/units) to the new
 * values, or delete it. Rewrite-and-rename (a crash never truncates the
 * log), then the tail is reloaded. 0 on success, -1 on failure or when no
 * row matches. These exist for the EDIT INSULIN form only -- everything
 * else treats the log as append-only. */
int insulin_update(const struct ins_rec *orig, long t, int type, int units,
                   long tz);
int insulin_delete(const struct ins_rec *orig);

const char *insulin_type_name(int type); /* "SLOW" / "FAST" */

#endif
