// SPDX-License-Identifier: GPL-3.0
// stats.h --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_STATS_H
#define PANCRA_STATS_H

/* O(1) per reading via hourly buckets; O(days*24) to read a rolling window. */
void stat_add(long t, int glu);
/* time-in-range (%) and average over the rolling last `days`; returns 0
 * (=>"--") until the data reaches back far enough to cover the whole window. */
int stat_window(int days, int *tir, int *avg);
/* The time-in-range band, in mg/dL: the ADA/ATTD international consensus
 * range. Named so the figure and its provenance travel together, and so a
 * test can sit on the boundary without repeating the literals. */
#define TIR_LOW_MGDL  70
#define TIR_HIGH_MGDL 180

/* seed the buckets from the tail of the readings CSV at `readings_path`. */
void stat_load(const char *readings_path);

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
 * phone freezes in one day, and app/test/lockorder.py refuses it.
 *
 * So the work is split where the locks say it must be:
 *
 *   stat_reload_prepare()   parses the log into a PRIVATE ring. Call with NO
 *                           history lock held. It reads the registry; it
 *                           touches none of the live buckets, so a frame
 *                           drawing beside it still renders the OLD numbers,
 *                           whole and consistent, rather than a half-built
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

/* Injectable-clock forms. The public calls above are these with
 * realtime_s(); tests drive these directly so the hour boundaries -- where the
 * aliasing bug lived -- are reachable deterministically. */
void stat_add_at(long t, int glu, long now);
int stat_window_at(int days, int *tir, int *avg, long now);

#endif
