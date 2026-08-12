// SPDX-License-Identifier: GPL-3.0
// plotdata.h --- long plot spans, read from the log rather than from RAM
// Copyright 2026 Jakob Kastelic

/* The live history buffer (store.h's g_hist) is a fixed number of POINTS, so
 * using it to draw a long span ties that span's depth to a point budget: the
 * 30D plot showed ten days once four sources were logging, and would shrink
 * again the day a user added a sensor.
 *
 * So anything past the live window is read straight from readings.csv and
 * downsampled to what the screen can actually distinguish -- see plotdata.c.
 * No Android or JNI types here, so the whole thing builds and is tested on
 * the host (test/plottest.c). */
#ifndef PLOTDATA_H
#define PLOTDATA_H

/* NOT #include "ui.h": ui.h needs PLOT_LONG_MAX from here to size its own
 * buffers, so including it back would be circular. The struct is only ever
 * handled by pointer here; plotdata.c includes ui.h for the definition. */
struct ui_point;

/* Spans up to this are drawn from the live RAM window instead. */
#define PLONG_MIN (24L * 3600)

/* The MOST points a long span can return. Every consumer must size its
 * arrays for this: copying the result into an NHIST-sized buffer truncates
 * it, and because the log is in arrival order the survivors are all recent,
 * so the old half of a 30-day plot vanishes -- the same truncation bug one
 * level up from where it was fixed. */
#define PLOT_COLS 768 /* x columns we ever draw into */
/* 64: a 56-minute column holds roughly two dozen readings with two CGMs and
 * a meter, so this is headroom rather than a guess -- and plottest asserts
 * that no distinguishable reading is dropped, which is exactly what fails if
 * it is ever too small. It was 24, and dropped points wherever two sensors
 * overlapped. */
#define PLOT_PERCOL   64 /* distinct values kept per column */
#define PLOT_LONG_MAX (PLOT_COLS * PLOT_PERCOL)

/* Points to draw for a span ending at `now`, or NULL when the caller should
 * use its own live buffer (span <= PLONG_MIN). `path` is the readings log.
 * The returned array is owned here and valid until the next call. */
const struct ui_point *plot_source_from(const char *path, long now, int hours,
                                        int *n);

/* Parse one readings.csv row into (t, glu, src, kind); 0 if it is not a
 * datapoint. Exposed for the test. */
int plot_store_row(const char *ln, long *t, int *glu, int *src, int *kind);

#endif
