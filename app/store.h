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

/* `struct reading` -- the record this history is made of. Its own header so
 * that a caller wanting only the LAYOUT does not take a dependency on the
 * whole store; see readingrec.h for what that cost. */
#include "readingrec.h"
#include "sensors.h" /* enum sensor_kind: a reading is a CGM sample or a stick */

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

/* ---- ASK A QUESTION; DO NOT WALK THE TABLE ----------------
 *
 * hist_count() and hist_at() are a COUNT and an INDEXED READ: two calls about
 * a table another thread can change between them, and their coherence was
 * every caller's problem, solved by exporting the lock and asking each of
 * them to hold it. Four modules then open-coded the same three walks -- "the
 * newest reading from this sensor", "the oldest one", "this sensor's samples"
 * -- each with its own copy of the `kind != KIND_BGM` rule, and one of them
 * (model.c) deliberately holds no lock at all because the frame path already
 * has it. That is four chances to forget the lock and four copies of one
 * semantic rule.
 *
 * These are the questions the callers were really asking. Each takes the lock
 * ITSELF and answers from ONE instant, and none of them can be got wrong by
 * forgetting to hold something.
 *
 * CGM READINGS ONLY, in all three: a fingerstick (KIND_BGM) is a different
 * measurement with a different provenance, and every one of the open-coded
 * walks excluded it -- separately, in four places. */

/* The instant of the newest CGM reading from `src`, or 0 if it has none. */
long hist_newest_t(int src);
/* ...and of the oldest one held in the tail, or 0. NOT the oldest in the LOG:
 * this is the in-memory tail, which is a display window. */
long hist_oldest_t(int src);
/* Copy up to `cap` of `src`'s CGM readings, NEWEST FIRST; returns how many. */
int hist_copy_src(int src, struct reading *out, int cap);

/* HOW MANY THE TAIL HOLDS, as a number to SAY rather than to walk with: the
 * startup log line reports it, and nothing indexes anything with it. Takes
 * the lock itself, so it is not the count half of a count/index pair. */
int hist_in_memory(void);

/* How many readings the in-memory tail holds.
 *
 * ALREADY-LOCKED TRAVERSAL, and the only caller that may use it is one that
 * already holds the lock for another reason -- the frame builder, which runs
 * inside draw()'s hold (see the note in model.c's build_model). Everything
 * else asks one of the questions above; `make -f test/Makefile lockcheck`
 * refuses this pair anywhere else. */
int hist_count(void);
/* The i-th, NEWEST first; out of range yields a zeroed reading. Same rule as
 * hist_count. */
struct reading hist_at(int i);
/* Copy up to `cap`, newest first, under the lock; returns how many. */
int hist_copy(struct reading *out, int cap);

/* How many readings have been APPENDED this run (what the settings screen
 * reports as "stored"). Distinct from store_count(), which counts the rows in
 * the file. */
int store_appended(void);

/* THE CRASH HANDLER'S TWO POINTERS, and nothing else's. It runs on a signal
 * stack: it may not lock, allocate, or call into the modules it is
 * describing, so it holds these addresses from startup and reads them
 * directly.
 *
 * ATOMIC, so that reading them there is defined rather than merely likely to
 * work: they are written by the binder thread that ingests a reading and read
 * from a handler that can interrupt it mid-instruction. Both are dedicated
 * MIRRORS published at each mutation -- the live values stay plain ints, so
 * the ingest path pays nothing for a diagnostic. See crashlog.h. */
const _Atomic int *store_glu_ptr(void);
const _Atomic int *hist_count_ptr(void);
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
/* ---- AND IT IS A TYPE, NOT AN INT --------------------------
 *
 * Three outcomes, and the two that are NOT "duplicate" mean different things
 * to different callers: one wants "did this reach the record" (both), another
 * wants "is this the newest thing on the screen" (only HIST_NEW). As a bare
 * int, callers wrote `int isnew = r.inserted;` and then used it as a boolean
 * -- which reads as "new" and answers "not duplicate", so HIST_OLD (a
 * backfilled reading older than the display window) took every path meant for
 * a fresh sample: the big number, the alarm, the new-datapoint chirp.
 *
 * So the enum is named, the field carries the type, and the two questions
 * have their own predicates. `hist_kept` is "it reached the record";
 * `hist_is_tail` is "it is the newest sample on this screen". Neither is
 * spelled as a truth value, because the two are not the same truth. */
enum hist_insert_result { HIST_DUP = 0, HIST_NEW = 1, HIST_OLD = 2 };

/* DID IT REACH THE RECORD? True for HIST_NEW and HIST_OLD alike: the log and
 * the statistics take both, and gating them on HIST_NEW is what once made the
 * live numbers and the post-restart numbers disagree about one file. */
static inline int hist_kept(enum hist_insert_result r)
{
   return r != HIST_DUP;
}

/* IS IT THE NEWEST SAMPLE THE SCREEN HOLDS? Only HIST_NEW. Everything that
 * describes "now" -- the big number, the trend, the alarm evaluation, the
 * new-datapoint sound -- asks THIS, because a backfilled reading from last
 * Tuesday is a fact worth keeping and not a thing that just happened. */
static inline int hist_is_tail(enum hist_insert_result r)
{
   return r == HIST_NEW;
}

/* Insert a reading (out-of-order safe for backfill); see the enum above.
 *
 * Dedup is per-SOURCE: two sensors sampling seconds apart are distinct facts,
 * and a global time window would let one silently overwrite the other. A BGM
 * fingerstick never dedups against a CGM sample either -- a meter reading in
 * the same minute is precisely the divergence worth seeing. */
enum hist_insert_result hist_insert(long t, int glu, int trend, int src,
                                    int kind);
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
                 long raw, long tz, int kind, int rescale_pm,
                 enum warm_state warm);
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
   /* THE ENUM. A reading's kind decides whether it dedups
    * against a CGM sample, whether it ends a streak, and how it is drawn --
    * three rules that all read this field, and none of which wants an
    * arbitrary integer. */
   enum sensor_kind kind;
   int rssi; /* link RSSI, meaningful only when has_rssi */
   int has_rssi;
   long raw;       /* the sensor's own uncorrected time */
   long tz;        /* the offset assumed when converting it */
   int rescale_pm; /* factor applied to glu, permille; 1000 = none */
   /* WARM-UP BACKFILL. Points replayed out of a sensor's own memory are real
    * readings and belong in the log and the history, but they were already
    * counted in the statistics when they were first seen -- feeding them
    * again double-counts a day of a person's time in range. */
   int warm;
   /* WHAT THE SENSOR SAID ABOUT ITS OWN WARM-UP, stored with the
    * row so a replay is not left inferring it from an activation that may
    * never have been learned. `warm` above is the DECISION -- whether this
    * reading feeds the statistics -- and it is made from this by warm_decide;
    * this is the EVIDENCE, and it is what goes in the log. */
   enum warm_state wstate;
   /* Whether that decision rests on the inference rather than on a
    * measurement, so the coverage figures can say how much of a window does
    * (see struct stat_cov). */
   int warm_unsure;
   /* The primary source id, resolved by the caller UNDER THE REGISTRY LOCK
    * before calling. -1 when no CGM is registered. It is a parameter and not
    * a lookup for the lock-order reason in hist_refresh_current's comment. */
   int prime;
};

/* What recording it did. */
struct reading_result {
   /* WHAT HAPPENED TO IT. Ask hist_kept() for "did it reach the
    * record" and hist_is_tail() for "is it the newest on screen"; the two
    * are different questions and were the same `int` until they were not. */
   enum hist_insert_result inserted;
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
 * NOT a flag in the shell taken by name at fifty call sites in a different
 * translation unit from the array it protects: a lock that lives away from
 * its data is a lock somebody forgets to take. */
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
 * the history lock. Calling sensor_primary_id() here, at the end, with the
 * history lock held would be registry inside history -- the inverse of the
 * documented registry -> history order, and one binder thread away from a
 * freeze. The lock order is a property of the CALL SITE, so the id is
 * an argument. -1 means "no primary". */
int store_load(int prime);
/* Count the rows currently in the log (one pass). */
int store_count(void);

#endif
