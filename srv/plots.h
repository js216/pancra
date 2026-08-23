/* SPDX-License-Identifier: GPL-3.0
 * plots.h --- the plot images the web interface serves
 * Copyright 2026 Jakob Kastelic
 *
 * One module's interface, declared where it is implemented. Both of these
 * lived in the protocol header, which every file in the server includes for
 * the wire bounds -- so a change to how a plot is drawn was, to the include
 * graph, a change everything depended on.
 */
#ifndef PANCRA_PLOTS_H
#define PANCRA_PLOTS_H

#include "wireint.h" /* int64_t and PRIwire: the wire's scalars, exactly */

struct req;
struct db;

/* Serve one plot as a GIF: the window (win_start, win_end] over `hours`,
 * rendered by pancra's own plot.c so the web plot cannot drift from the
 * app's. */
void h_plot_gif(struct req *r, int64_t owner, int64_t win_start,
                int64_t win_end, int hours, int tz_min);
/* ---- WHAT THE ARCHIVE HOLDS, ASKED WITHOUT A LIFETIME CEILING --------
 *
 * Both of these were one function -- plot_days -- which copied
 * EVERY distinct day the account has ever had into a caller's array. The
 * arrays were `static int64_t days[4096]`, and 4096 days is eleven years:
 * after that the oldest months simply stopped appearing, and the function
 * reported success while doing it (`n >= cap` counted as a complete answer).
 * A record that silently forgets its own beginning is worse than one that
 * refuses to load.
 *
 * So neither answer is materialised. The month list is GROUPED IN SQL -- the
 * database already indexes the buckets, and there is no reason to walk eleven
 * years of days in C to discover twelve months -- and the day list is asked
 * for ONE MONTH at a time, which is the only thing any caller ever wanted. A
 * row at a time goes to the callback, so nothing is bounded by an array.
 *
 * BOTH RETURN 0 OR -1, and -1 means THE SCAN DID NOT FINISH. It is not "no
 * data": a caller that prints "no days with data yet" for a database that was
 * merely busy is telling the reader their record is empty. */

/* Every month (as year*100 + month, UTC) that holds a reading, newest first.
 * `fn` returns 0 to keep going. */
int plot_months(struct db *d, int64_t owner,
                int (*fn)(void *ctx, int year, int mon), void *ctx);

/* Every day (a UTC-day bucket number) in [day0, day1) that holds a reading,
 * newest first. `fn` returns 0 to keep going. */
int plot_days_in(struct db *d, int64_t owner, int64_t day0, int64_t day1,
                 int (*fn)(void *ctx, int64_t day), void *ctx);

#endif
