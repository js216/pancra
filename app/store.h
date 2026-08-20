// SPDX-License-Identifier: GPL-3.0
// store.h --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_STORE_H
#define PANCRA_STORE_H

#include "ingest.h" /* STORE_GLU_MIN/MAX: what a stored reading may be */

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
 * Keep UI_PLOT_MAX in uirender.c EQUAL to this (the Makefile crosscheck
 * enforces it; uirender.c is decoupled from this header, so a smaller UI cap
 * would re-truncate the plot even with a large NHIST). */
#define NHIST 5040
/* Read BUFFER size for the startup replay -- not a limit on how much of the
 * log is read. store_load streams the WHOLE file through this in chunks,
 * because the log is in arrival order: after importing months of history the
 * newest readings are NOT at the end, and a tail-limited read came back with
 * an empty plot from an intact log. */
#define STORE_TAIL 262144

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

/* THE HISTORY IS PRIVATE. It was `extern struct reading g_hist[NHIST]` and a
 * count, which made every reader depend on the representation (an array, this
 * long, NEWEST FIRST, deduped) and let any of them write to it -- and a
 * reading written by hand is a reading in the plot and the alarms that is not
 * in the log.
 *
 * ORDER IS PART OF THE CONTRACT: newest first, because everything that reads
 * this wants the recent end and stops early.
 *
 * THE LOCK IS STILL THE CALLER'S for a multi-step walk (see hist_lock): the
 * readings arrive on a binder thread, so a walk that wants a consistent view
 * has to hold it across the whole walk, and no per-call lock can provide
 * that. hist_copy is the exception -- it takes the lock itself, because one
 * call IS the whole walk. */

/* How many readings the in-memory tail holds. Caller holds hist_lock for a
 * value it is about to walk with. */
int hist_count(void);
/* The i-th, NEWEST first; out of range yields a zeroed reading. */
struct reading hist_at(int i);
/* Copy up to `cap`, newest first, under the lock; returns how many. */
int hist_copy(struct reading *out, int cap);
/* Forget the in-memory tail (a reload rebuilds it from the file). Does not
 * touch the log. */
void hist_clear(void);

/* How many readings have been APPENDED this run (what the settings screen
 * reports as "stored"). Distinct from store_count(), which counts the rows in
 * the file. */
int store_appended(void);

/* THE CRASH HANDLER'S TWO POINTERS, and nothing else's. It runs on a signal
 * stack: it may not lock, allocate, or call into the modules it is
 * describing, so it holds these addresses from startup and reads them
 * directly. A torn read in a crash report is better than no context. */
const int *store_glu_ptr(void);
const int *hist_count_ptr(void);
/* The reading log's path, for the code that must NAME the file: the sync
 * client registers it, the long-span plot re-reads it, and the log line at
 * startup says where it is. Read-only -- it is set once, by store_paths. */
const char *store_path(void);
/* Point it at the data directory; the filename lives here. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int store_paths(const char *dir);

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
/* Glucose of the newest CGM sample from `src` in [not_before, t) -- strictly
 * older than `t`, and no older than `not_before` -- or -1 if that source has
 * none in the window.
 *
 * Per-source by design: the NEW DATAPOINT chirp pitches on the change since
 * this sensor's own previous reading, and with two CGMs worn at once a
 * cross-sensor difference is a calibration offset between two devices, not a
 * trend -- pitching on it would sound like a swing the wearer never had. The
 * window is what stops the same lie being told across a GAP: see
 * CHIRP_MAX_GAP_S. Call under hist_lock(). */
int hist_prev_glu(long t, int src, long not_before);
/* Append one row of schema v2:
 *   epoch,glucose,trend10,rssi,recv_lag,source_id,raw_time,tz_off,kind
 * `raw` is the sensor's own uncorrected time and `tz` the offset assumed when
 * converting it, so a bad conversion stays repairable decades later. */
/* rescale_pm is the multiplicative factor (permille; 1000 = none) applied to
 * this row's glucose, recorded in the trailing `rescale` column for provenance.
 * The `glu` passed is the ALREADY-rescaled value. */
/* 0 if the row reached the disk, -1 if it did NOT. CHECK IT: a reading that
 * was not written is still shown, alarmed on and counted until the next
 * restart, and then gone -- which is the one failure the log exists to
 * prevent. app/insulin.c's ins_write_row has always been checked this way. */
int store_append(long t, int glu, int trend, int rssi, int has_rssi, int src,
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

/* THE CURRENT READING, AS ONE FACT.
 *
 * g_cur_glu, g_cur_trend and g_cur_time are written as SEPARATE stores by
 * hist_refresh_current, under the history lock, from whichever thread a
 * reading arrived on. Every reader that took them one at a time could pair a
 * new glucose with the previous timestamp -- which evaluates as stale, so a
 * genuine low is not raised on that pass -- or the mirror, a stale
 * out-of-range value stamped with a fresh time, which chimes and then
 * silences itself.
 *
 * This is the only way to read them: one call, one lock, one consistent
 * triple. `stale` is computed inside, against the same `now` the caller is
 * reasoning about, so two readers cannot disagree about it either. */
struct reading_now {
   int glu; /* -1 when nothing has been seen */
   int trend;
   long t;    /* epoch seconds of that reading */
   int stale; /* no reading, or older than the freshness window */
};
struct reading_now store_now(long now);

/* THE SAME TRIPLE, for a caller that ALREADY HOLDS the store lock -- the
 * frame builder (draw() holds it across the whole frame) and the alarm
 * gatherer. The lock is not recursive, so store_now would self-deadlock
 * there; holding it is also what makes the three consistent, which is exactly
 * what store_now provides an unlocked caller. Naming the variant is what
 * stops the read being open-coded again. */
struct reading_now store_now_locked(long now);

/* THE LINK RSSI THAT GOES WITH THE CURRENT READING, as one value. Written by
 * the reading path when a sample or a connection reports one; read by the
 * frame. `ok` is 0 when nothing has reported one yet -- distinct from a
 * genuine 0 dBm, which no radio produces. */
struct reading_rssi {
   int dbm;
   int ok;
};
struct reading_rssi store_rssi(void);
/* ...for a caller already holding the store lock. */
struct reading_rssi store_rssi_locked(void);
/* Record one, from the reading path. */
void store_note_rssi(int dbm);

/* ===================== RECORDING ONE READING =========================
 *
 * A reading is FOUR things happening in a fixed order, and until this
 * interface existed every caller performed all four by hand:
 *
 *   1. take the history lock;
 *   2. hist_insert, and feed stat_add on ANY non-zero result (not just
 *      HIST_NEW -- gating on HIST_NEW made TIR and the average differ before
 *      and after a restart, because stat_load has no NHIST notion);
 *   3. hist_refresh_current with the primary resolved BEFORE the lock was
 *      taken (resolving it inside would invert the reg -> hist lock order);
 *   4. release the lock, then store_append OUTSIDE it -- and check the
 *      result, because a reading that did not persist is still on screen and
 *      still alarmed on, and is gone after a restart.
 *
 * Three call sites did that -- the CGM path, the backfill path and the meter
 * path -- each with its own copy of the ordering, and each an opportunity to
 * get one step wrong in a way nothing would report. Every rule above is a
 * comment somewhere explaining a bug that had already happened.
 *
 * So: a reading is a VALUE, and recording it is one operation that owns the
 * lock, the order and the failure.
 */
struct reading_event {
   long t;    /* when the reading was taken */
   int glu;   /* mg/dL, ALREADY rescaled */
   int trend; /* tenths of mg/dL per minute; 127 = unknown */
   int src;   /* provenance id of the device that made it */
   int kind;  /* KIND_CGM / KIND_BGM */
   int rssi;  /* link RSSI, meaningful only when has_rssi */
   int has_rssi;
   long raw;       /* the sensor's own uncorrected time */
   long tz;        /* the offset assumed when converting it */
   int rescale_pm; /* factor applied to glu, permille; 1000 = none */
   /* WARM-UP BACKFILL. Points replayed out of a sensor's own memory are real
    * readings and belong in the log and the history, but they were already
    * counted in the statistics when they were first seen -- feeding them
    * again double-counts a day of a person's time in range. */
   int warm;
   /* The primary source id, resolved by the caller UNDER THE REGISTRY LOCK
    * before calling. -1 when no CGM is registered. It is a parameter and not
    * a lookup for the lock-order reason in hist_refresh_current's comment. */
   int prime;
};

/* What recording it did. */
struct reading_result {
   int inserted;  /* HIST_DUP / HIST_NEW / HIST_OLD */
   int persisted; /* 1 = the row reached the disk. CHECK IT. */
   /* This source's previous glucose within CHIRP_MAX_GAP_S, captured before
    * the insert (afterwards the newest sample from src IS this one), or -1.
    * The NEW DATAPOINT chirp pitches on it. */
   int prev_glu;
};

/* Record one reading: history, statistics, current value and disk, in the one
 * order they are allowed to happen in. Takes and releases the history lock
 * itself; the disk append happens outside it.
 *
 * `gap` is the window hist_prev_glu searches back over for prev_glu. */
struct reading_result store_record(const struct reading_event *ev, long gap);

/* THE HISTORY LOCK, which lives here because the history does.
 *
 * It used to be a flag in the shell called g_draw_busy, taken by name at fifty
 * call sites in a different translation unit from the array it protects. A
 * lock that lives away from its data is a lock somebody forgets to take. */
void store_lock(void);
void store_unlock(void);
int store_trylock(void);
/* Wait up to `ms` for the lock to be FREE without taking it -- for teardown,
 * which must be bounded (see app/thread.h, rule 5). 1 if it went free. */
int store_drain(int ms);

/* The bound a STORED reading must satisfy, which is NOT the live sensor bound.
 *
 * The range itself is in ingest.h -- see there. It is not the STORE's fact:
 * every reader of the log needs it, and stats.c included this whole header
 * for those two numbers alone, which put a cycle between the history and the
 * statistics it feeds. */

/* Load the tail of the CSV into g_hist (most-recent NHIST rows) + g_cur_*. */
/* Load the whole log into the in-memory history. Returns 0 on success -- a
 * log that was never written is a success -- and -1 if the file exists but
 * could not be read whole, which means the history is SHORT and everything
 * derived from it (plot, statistics, the restored current reading) is
 * understated. Callers must say so rather than render a partial record as a
 * complete one.
 *
 * `prime` is the PRIMARY SENSOR'S ID, resolved by the caller BEFORE it takes
 * the history lock. This function used to call sensor_primary_id() itself, at
 * the end, with the history lock held -- registry inside history, the inverse
 * of the documented registry -> history order, and one binder thread away
 * from a freeze. The lock order is a property of the CALL SITE, so the id is
 * an argument. -1 means "no primary". */
int store_load(int prime);
/* Count the rows currently in the log (one pass). */
int store_count(void);

#endif
