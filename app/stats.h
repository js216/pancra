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

/* Ring capacity in hours. Exposed so the tests can sit exactly on the boundary
 * where an over-old reading would alias onto a live bucket. */
#define STAT_HOURS 2200

/* Injectable-clock forms. The public calls above are these with
 * realtime_s(); tests drive these directly so the hour boundaries -- where the
 * aliasing bug lived -- are reachable deterministically. */
void stat_add_at(long t, int glu, long now);
int stat_window_at(int days, int *tir, int *avg, long now);

#endif
