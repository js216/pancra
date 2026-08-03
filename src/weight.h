// SPDX-License-Identifier: GPL-3.0
// weight.h --- Body-weight log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* A weight is what the user TYPED, so this log is the same shape as the
 * insulin one: when, and how much. The file is the lifetime record
 * (append-only, never rewritten, the readings.csv discipline); the in-memory
 * tail exists for the table the UI draws. */
#ifndef PANCRA_WEIGHT_H
#define PANCRA_WEIGHT_H

/* STORED IN GRAMS, ALWAYS, whatever the display unit is.
 *
 * The display unit is a preference the user can flip at any time, so it must
 * not be part of the record: a log half in pounds and half in kilograms, with
 * nothing on the row to say which, is unrecoverable. One canonical unit on
 * disk means flipping the setting re-renders history instead of corrupting
 * it. Grams (not tenths of a kilo) so a pound entry survives the round trip
 * to storage and back without drifting -- see wt_from_tenths. */
#define WT_MIN_G 20000L  /* 20 kg / 44 lb */
#define WT_MAX_G 400000L /* 400 kg / 882 lb */

/* Display units, matching g_wunits (settings.h). */
#define WT_KG 0
#define WT_LB 1

/* Epoch bound on an entry instant, same rationale and value as INS_T_MAX:
 * wide enough for legitimate backdating, tight enough that a corrupt digit
 * run cannot parse as a plausible date. */
#define WT_T_MAX 32503680000L

/* In-memory tail only -- the file keeps everything. 256 daily weigh-ins is
 * eight months, and the table pages through the tail. */
#define NWT 256

struct wt_rec {
   long t; /* entry instant, epoch seconds */
   long g; /* grams */
};

extern struct wt_rec g_wt[NWT]; /* oldest first; the newest is at the end */
extern int g_nwt;
extern char g_wt_path[256];

/* Load the tail of the log (the last NWT plausible rows). Safe on a fresh
 * install: a missing file is an empty log. */
void weight_load(void);

/* Append one weight durably and mirror it into the tail. 0 on success, -1
 * when the write failed (the tail is then left untouched, so memory never
 * claims an entry the file does not have). Rejects out-of-range input. */
int weight_append(long t, long g, long tz);

/* Display conversions, PURE so the round trip is testable.
 *
 * Entry and display are both in TENTHS of the display unit ("154.2" is 1542),
 * matching how glucose is entered in mmol/L. Both directions round to nearest
 * rather than truncating, which is what makes the round trip stable: with
 * truncation, a weight typed as 154.0 lb stored as 69841 g rendered back as
 * 153.9 lb -- a number the user could not re-enter to get the same record,
 * which is precisely the trap the plot-max entry fell into once already. */
long wt_from_tenths(int tenths, int units); /* -> grams, 0 if out of range */
int wt_to_tenths(long g, int units);

const char *wt_unit_name(int units); /* "KG" / "LB" */

/* Rewrite the LAST file row matching `orig` (t and grams) to the new values,
 * or delete it. Rewrite-and-rename, so a crash never truncates the log; the
 * tail is reloaded afterwards. 0 on success, -1 on failure or when no row
 * matches. These exist for the EDIT WEIGHT form only -- everything else
 * treats the log as append-only. Matching is by CONTENT, not position, so a
 * stale tail index can never touch the wrong entry. */
int weight_update(const struct wt_rec *orig, long t, long g, long tz);
int weight_delete(const struct wt_rec *orig);

#endif
