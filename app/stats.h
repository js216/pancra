// SPDX-License-Identifier: GPL-3.0
// stats.h --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_STATS_H
#define PANCRA_STATS_H
#include <stdbool.h>

#include "loadresult.h" /* what a load actually found */

/* O(1) per reading via hourly buckets; O(days*24) to read a rolling window. */
/* Record one reading in the statistics. `unsure` marks a reading counted on
 * the strength of an inference rather than a measured warm-up state -- see
 * warm_decide in sensors.h. */
/* The time-in-range band, in mg/dL: the published one, not the user's alarm
 * thresholds. See stat_in_range. */
#define STAT_TIR_LO 70
#define STAT_TIR_HI 180

/* Is this reading inside that band? */
bool stat_in_range(int glu);

/* Did the load fall short of the whole history? Every window answers blank
 * while this is true. */
bool stat_degraded(void);

void stat_add(long t, int glu, int unsure);
/* The same, with the caller naming `now` -- for a test that owns the clock. */
void stat_add_at(long t, int glu, int unsure, long now);

/* ---- HOW MUCH OF THE WINDOW WAS ACTUALLY MEASURED ----------
 *
 * "THE OLDEST READING IS OLD ENOUGH" is a statement about ONE timestamp. Two
 * readings ninety days apart satisfy it, and the screen then prints a 90-DAY
 * TIME IN RANGE and A1C computed from two samples -- indistinguishable, in
 * every pixel, from the same figures
 * over three months of continuous wear. A percentage is only as good as the
 * data under it, and nothing said how much there was.
 *
 * COVERAGE IS COUNTED, in the unit the buckets already use: an hour with any
 * reading in it is a covered hour. A CGM samples every five minutes, so a
 * worn sensor covers essentially every hour; a phone left off overnight
 * costs eight; a sensor change costs two.
 *
 * THE BAR IS HALF THE WINDOW, and deliberately no higher. The clinical
 * consensus for a meaningful TIR is 70% of 14 days, but this figure sits on
 * a phone screen beside the live number rather than in a clinic letter, and
 * blanking it for somebody whose phone was off for a night would be a worse
 * answer than showing it. Half is far above "two readings" and far below
 * "ordinary life".
 *
 * THE LONGEST GAP IS REPORTED BUT NOT GATED ON: a fortnight away from the
 * phone leaves a real 90-day figure that is still worth showing, and the
 * caller can say so if it wants to. */
struct stat_cov {
   int hours;     /* hours in the window that hold at least one reading */
   int of_hours;  /* the window's length in hours */
   int gap_hours; /* the longest run of empty hours inside it */
   /* HOW MUCH OF THIS RESTS ON AN INFERENCE. Readings counted
    * whose warm-up state nothing measured -- rows written before the sensor
    * answered, or before the column existed -- and which the activation rule
    * then let through. They are ordinary glucose in every other respect; what
    * is uncertain is only whether they were uncalibrated warm-up, which is
    * the one thing that would put them in these figures wrongly. */
   int unsure;
   int counted; /* readings in the window, so `unsure` has a denominator */
};

/* The same question without the metadata. */
int stat_window(int days, int *tir, int *avg);
/* The time-in-range band, in mg/dL: the ADA/ATTD international consensus
 * range. Named so the figure and its provenance travel together, and so a
 * test can sit on the boundary without repeating the literals. */
#define TIR_LOW_MGDL  70
#define TIR_HIGH_MGDL 180


/* SEED THE BUCKETS FROM THE READINGS LOG, and say what happened.
 *
 * LOAD_OK      the whole log was read; the windows below are a summary of it.
 * LOAD_ABSENT  there is no log yet -- a first run, and a complete answer.
 * LOAD_ERROR   the log exists and could not be read whole. What was read is
 *              kept (it is all this run knows) but stat_window answers
 *              NOTHING while it stands, because a percentage over a PREFIX of
 *              a record is indistinguishable from the same percentage over
 *              all of it -- and the screen has no way to say which it is
 *              looking at. The caller reports the degradation; the figures
 *              stay blank rather than plausible. */
enum load_result stat_load(const char *readings_path);


/* ============ THROW THE BUCKETS AWAY AND REBUILD THEM FROM THE LOG =========
 *
 * What the person holding the phone saw without this. They reinstall, tap
 * RESTORE, and the record comes back: the history list fills, the plot draws
 * a month of it -- and the TIR and AVERAGE figures printed beside that plot
 * are still whatever they were before the restore, which on a fresh install
 * is "--" or the handful of readings the new install had made for itself.
 * Two numbers on one screen disagreeing about the same data, with the wrong
 * pair looking every bit as authoritative as the right one, and no way to
 * tell which is which. They stayed wrong until the app was restarted, because
 * stat_load ran once at startup and pancra_logs_reload -- which reloads the
 * sensors, the readings, the insulin and the weight -- had no idea the
 * statistics existed.
 *
 * IDEMPOTENT, and that is the whole design: the buckets are replaced, not
 * added to, so the result depends only on the file. stat_load alone is
 * ADDITIVE -- it is a seed, run once into a ring that BSS had already zeroed
 * -- so calling it again over a live ring would count the restored rows on
 * top of whatever was already there. Replacing also means a log that cannot
 * be read leaves NO numbers behind rather than the previous ones: silence is
 * the honest answer when the record is unreadable, and a stale figure is not.
 *
 * IT IS TWO CALLS, AND THE SPLIT IS A LOCK-ORDER FACT, not a convenience.
 *
 * The replay resolves every row's sensor through the REGISTRY (the warm-up
 * hour is per-sensor and anchored on its activation), so it takes the
 * registry lock. The documented order is driver -> registry -> history
 * (app/thread.h, rule 6), and the caller that wants this -- pancra_logs_reload
 * -- is holding the HISTORY lock across store_load. Parsing under that lock
 * would take the registry INSIDE the history, the exact inversion behind two
 * phone freezes in one day, and test/app/lockorder.py refuses it.
 *
 * So the work is split where the locks say it must be:
 *
 *   stat_reload_prepare()   parses the log into a PRIVATE ring. Call with NO
 *                           history lock held. It reads the registry; it
 *                           touches none of the live buckets, so a frame
 *                           drawing beside it still renders the published
 *                           numbers, whole and consistent, rather than a
 *                           half-built
 *                           ring reading TIR 0 over a record that is entirely
 *                           in range. Returns 0 if the private ring could not
 *                           be allocated, in which case there is nothing to
 *                           publish and the existing numbers stand.
 *
 *   stat_reload_publish()   swaps it in. Call with the STORE LOCK HELD -- the
 *                           same lock stat_add is called under on the reading
 *                           path and stat_window is read under during the
 *                           model build. It is a copy and two stores, no I/O
 *                           and no other lock, so it can live inside the hold
 *                           store_load already takes: the restored history and
 *                           the restored statistics then become visible in the
 *                           same instant, which is the whole point. A publish
 *                           with nothing prepared does nothing.
 */
int stat_reload_prepare(const char *readings_path);
void stat_reload_publish(void);

/* Ring capacity in hours. Exposed so the tests can sit exactly on the boundary
 * where an over-old reading would alias onto a live bucket. */
#define STAT_HOURS 2200

/* ...and the same with the coverage metadata, for a test that owns the clock
 * and for anything that wants to say WHY a window is blank. */
int stat_window_cov_at(int days, int *tir, int *avg, struct stat_cov *cov,
                       long now);

#endif
