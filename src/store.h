// SPDX-License-Identifier: GPL-3.0
// store.h --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_STORE_H
#define PANCRA_STORE_H

/* Master reading history. This is the DISPLAY buffer the plot draws from, and
 * its size is what bounds how far back the longest plot span (7D = 168 h) can
 * actually be filled. It MUST be a count large enough that a full 7 days of
 * readings never overflow it -- otherwise the oldest in-window points get
 * evicted as new ones arrive and the "7D" plot silently shrinks below a week
 * (the very bug this sizing fixes). 2100 was "7 days at exactly one 5-min CGM
 * sample" with zero headroom, so any extra density -- a second sensor, meter
 * fingersticks, reconnect backfill re-reads -- pushed real 7-day data off the
 * left edge. 5040 = 7 days at one reading every 2 minutes, a ceiling that
 * covers two concurrent 5-min CGMs plus a meter plus backfill with margin.
 * Keep UI_PLOT_MAX in ui.c EQUAL to this (the Makefile crosscheck enforces it;
 * ui.c is decoupled from this header, so a smaller UI cap would re-truncate the
 * plot even with a large NHIST). */
#define NHIST 5040
/* Bytes of readings.csv read back at startup. Sized so NHIST rows of schema v2
 * (~50 B each with the rescale column and two sensors logging) still fit. */
#define STORE_TAIL 524288

/* One reading in the display history.
 *
 * glu/trend are narrowed to 16 bits so `src` and `kind` fit in what was
 * padding: the struct stays 16 bytes, so g_hist costs exactly what it always
 * did (2100 x 16 B) despite carrying full attribution. Glucose fits in mg/dL
 * and trend10 in tenths-per-minute with room to spare. */
struct reading {
   short glu, trend;
   /* Sensor id (see sensors.h); 0 = pre-registry legacy. 16 bits, NOT 8: ids
    * are minted for every session and firmware change and never reused, so an
    * 8-bit field wraps after 255 -- and a wrapped id aliases a real one, which
    * would silently reattribute readings to the wrong physical device. */
   unsigned short src;
   unsigned char kind; /* KIND_CGM / KIND_BGM -- decides how it is plotted */
   long t;             /* canonical UTC epoch seconds */
};

/* Reading data model, owned by store.c and read by the UI. History is kept
 * newest-first and deduped; g_cur_* is the latest reading snapshot. */
extern struct reading g_hist[NHIST];
extern int g_nhist;
extern int g_cur_glu, g_cur_trend;
extern long g_cur_time; /* epoch seconds of the latest reading */
extern int g_cur_rssi, g_cur_rssi_ok;
extern int g_stored;           /* total rows in the log */
extern char g_store_path[256]; /* path to the readings CSV */

/* hist_insert results. HIST_OLD exists because NHIST is a DISPLAY cap, not a
 * retention policy: a reading older than the ~7 days kept on screen is still a
 * fact the user wants kept for life, so it must reach the log even though it
 * has no place in g_hist. Treating that case as "not new" silently discarded
 * every backfilled point older than the window -- a meter's first sync can
 * carry records weeks old.
 *
 * Callers feed BOTH the log and the in-memory stats on any non-zero result.
 * They once fed the stats only on HIST_NEW, which made the live numbers and
 * the post-restart numbers disagree about the same file: stat_load has no
 * NHIST notion, so on the next launch it counted exactly the rows the live
 * path had skipped. The stats ring spans ~91 days against g_hist's ~7, so a
 * reading off the end of the DISPLAY window is still inside the STATISTICS
 * window and belongs in them. */
enum { HIST_DUP = 0, HIST_NEW = 1, HIST_OLD = 2 };

/* Insert a reading (out-of-order safe for backfill); see the enum above.
 *
 * Dedup is per-SOURCE: two sensors sampling seconds apart are distinct facts,
 * and a global time window would let one silently overwrite the other. A BGM
 * fingerstick never dedups against a CGM sample either -- a meter reading in
 * the same minute is precisely the divergence worth seeing. */
int hist_insert(long t, int glu, int trend, int src, int kind);
/* Append one row of schema v2:
 *   epoch,glucose,trend10,rssi,recv_lag,source_id,raw_time,tz_off,kind
 * `raw` is the sensor's own uncorrected time and `tz` the offset assumed when
 * converting it, so a bad conversion stays repairable decades later. */
/* rescale_pm is the multiplicative factor (permille; 1000 = none) applied to
 * this row's glucose, recorded in the trailing `rescale` column for provenance.
 * The `glu` passed is the ALREADY-rescaled value. */
void store_append(long t, int glu, int trend, int rssi, int has_rssi, int src,
                  long raw, long tz, int kind, int rescale_pm);
/* Recompute g_cur_* from the newest CGM sample in g_hist. With a primary
 * configured (`prime` >= 0), ONLY the primary's samples qualify: a primary
 * with no data yet clears the current reading (glu -1 = none) rather than
 * borrowing another sensor's -- the primary IS the big-number contract. Only
 * with no primary at all (prime -1: no CGM registered, e.g. a pre-registry
 * install) does the newest sample of any CGM source fill in. A BGM
 * fingerstick is never eligible. Call with the history lock held after any
 * insert, and after any primary change.
 *
 * `prime` is passed in rather than looked up because the caller must resolve it
 * under the registry lock BEFORE taking hist_lock: looking it up here would be
 * an unsynchronized read of a concurrently-shifted array, and locking it here
 * would invert the reg->hist order. */
void hist_refresh_current(int prime);
/* Load the tail of the CSV into g_hist (most-recent NHIST rows) + g_cur_*. */
void store_load(void);
/* Count the rows currently in the log (one pass). */
int store_count(void);

#endif
