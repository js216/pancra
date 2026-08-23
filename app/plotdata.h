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

/* THE PLOT GEOMETRY MOVED TO uimodel.h, and the include below is why.
 *
 * PLOT_LONG_MAX sizes the frame's point array, which is uimodel.h's; the
 * struct those points are is uimodel.h's too. Declaring the constants here
 * and the struct there makes the two headers include each other -- a cycle
 * to be removed rather than worked around. One vocabulary, one owner: the
 * model that the plot is drawn from. */
#include "uimodel.h" /* struct ui_point, PLOT_LONG_MAX, PLONG_MIN */

/* Points to draw for a span ending at `now`, or NULL when the caller should
 * use its own live buffer (span <= PLONG_MIN). `path` is the readings log.
 * The returned array is owned here and valid until the next call. */
const struct ui_point *plot_source_from(const char *path, long now, int hours,
                                        int *n);

/* Parse one readings.csv row into (t, glu, src, kind); 0 if it is not a
 * datapoint. Exposed for the test. */
int plot_store_row(const char *ln, long *t, int *glu, int *src, int *kind);

#endif
