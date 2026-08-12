// SPDX-License-Identifier: GPL-3.0
// ingest.h --- is this frame a usable live glucose reading?
// Copyright 2026 Jakob Kastelic

/* THE GATE EVERY CGM READING PASSES THROUGH, and the reason it is its own
 * file.
 *
 * These two checks -- is the value one a sensor could have produced, and is
 * the frame recent enough to be "now" -- are the only thing standing between
 * the wire and the record. A value that gets past them is displayed as the
 * user's glucose, fed to the alarm, counted in time-in-range, and appended to
 * a log that is never rewritten.
 *
 * They lived inside pancra_glucose in main.c, where nothing could reach them:
 * a review widened the value window to 0..100000 and the age bound to 65535
 * and `make check` stayed green through both -- the second of those
 * REINTRODUCING, verbatim, the 18-hour backdating bug whose fix is described
 * three lines above it. That is the same argument that put alarmlogic.c in
 * its own file, and it applies here for the same reason.
 *
 * Pure: no globals, no clock, no JNI, no locks. The caller passes `now`.
 */
#ifndef INGEST_H
#define INGEST_H

#include "alarmlogic.h" /* CAL_MIN_MGDL / CAL_MAX_MGDL: the sensor's own range */

/* What a CGM can physically report is CAL_MIN_MGDL..CAL_MAX_MGDL -- the range
 * a Dexcom sensor clamps to. The window here is deliberately WIDER, so a
 * genuine extreme is recorded rather than discarded, and derived from that
 * pair rather than written out again: the two are the same fact, and they
 * used to be two sets of literals three thousand lines apart. */
#define INGEST_GLU_MIN (CAL_MIN_MGDL / 2)     /* 20 */
#define INGEST_GLU_MAX (CAL_MAX_MGDL * 3 / 2) /* 600 */

/* How old a live 0x4e reading may be.
 *
 * `age` arrives as a full uint16 off the wire (dexdata.c), and only the
 * glucose used to be checked. A frame carrying age=65535 backdates the
 * reading 18.2 hours: into the history, into the time-in-range and average,
 * and into readings.csv -- a file that is never rewritten and whose only
 * load-time guard is t > 0, so it comes back on every restart.
 *
 * The sensor's cycle is ~5 minutes, so a live frame is seconds to a few
 * minutes old; 15 is generous. REJECT rather than clamp: clamping would stamp
 * a genuinely stale reading as current, and for a glucose display that is the
 * more dangerous of the two errors. */
#define INGEST_AGE_MAX 900

enum {
   INGEST_OK = 0,
   INGEST_IMPLAUSIBLE, /* the value is not one a sensor could report */
   INGEST_STALE        /* the frame is too old to be "now" */
};

struct ingest_out {
   int verdict; /* one of the above */
   long t;      /* the reading's instant; meaningful only when INGEST_OK */
};

/* Decide whether (mg_dl, age_s) is a usable live reading at `now`. */
struct ingest_out ingest_decide(int mg_dl, int age_s, long now);

#endif
