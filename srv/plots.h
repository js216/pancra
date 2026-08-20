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

struct req;
struct db;

/* Serve one plot as a GIF: the window (win_start, win_end] over `hours`,
 * rendered by pancra's own plot.c so the web plot cannot drift from the
 * app's. */
void h_plot_gif(struct req *r, long owner, long win_start, long win_end,
                int hours, int tz_min);
/* Days (UTC-day bucket numbers) that hold readings, newest first. */
int plot_days(struct db *d, long owner, long *out, int cap);

#endif
