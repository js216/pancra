// SPDX-License-Identifier: GPL-3.0
// plot.h --- Glucose plot rendering (API)
// Copyright 2026 Jakob Kastelic

/* plot.h -- a crude fixed-scale glucose time plot.
 *
 * The module is deliberately self-contained: plot_render() only writes RGBA
 * pixels into a caller-supplied framebuffer rectangle and knows nothing about
 * the rest of the app -- no fonts, no Android types, and no state beyond the
 * clip rectangle plot_render sets for its own markers. The caller
 * owns layout, the colour palette (passed as a function), and interaction. */
#ifndef PLOT_H
#define PLOT_H
#include <stdint.h>

/* One reading: epoch-second timestamp and glucose in mg/dL, plus how to draw
 * it. `marker` is a shape index (0 dot, 1 cross, 2 open square, 3 triangle) and
 * `col` an ARGB override -- 0 means "ask the caller's color() callback", which
 * is what continuous readings use. Sparse meter readings set both so they stay
 * visually distinct from the CGM trace. This module stays dependency-free, so
 * the shape indices are plain ints, not an enum borrowed from elsewhere. */
struct plot_pt {
   long t;
   int glu;
   int marker;
   int hidden; /* 1 = do not draw (HIDE marker) */
   int size;   /* per-device marker size 1..N; 0 = default */
   uint32_t col;
};

/* A drawn letter W, for logged body weights on the doses line.
 *
 * NOT a MARK_* value: those are user-selectable device markers persisted in
 * slots.csv, and this must never appear in the marker picker or be storable as
 * a device's shape. It sits far above the MARK_ range so the two cannot
 * collide as shapes are appended there. plot.c stays font-free -- the letter
 * is four strokes, drawn like every other shape here, not a glyph lookup. */
#define PLOT_MARK_W 100

/* Vertical scale runs PLOT_GLU_MIN..(runtime max, default PLOT_GLU_MAX) mg/dL.
 */
#define PLOT_GLU_MIN 50
#define PLOT_GLU_MAX 300

/* Set the top of the vertical scale in mg/dL (clamped to 100..400). */
void plot_set_max(int mgdl);

/* Render the readings in `pts` (any order, newest-first is fine) whose
 * timestamps fall within the last `hours` before `now` into the framebuffer
 * rectangle (x,y)-(x+w,y+h). Draws a frame and 100/200 gridlines, then one
 * dot per in-window reading. `color(glu)` yields each dot's colour, so the
 * palette stays with the caller. `fb` is RGBA_8888 with `stride` pixels per
 * row and total size `fbw`x`fbh`; writes are clipped to those bounds. */
/* As above; the point at index `hi_idx` (if in-window) is drawn larger in
 * `hi_color` -- pass hi_idx < 0 for no highlight. */
/* `tz` is the local UTC offset in seconds, used ONLY to anchor the day
 * gridlines. Without it the 24 h step anchored on UTC midnight, so in UTC-7
 * every "day" line on the 3D/7D/30D plots sat at 17:00 local and the day
 * boundaries the grid exists to show were wrong by the offset. */
void plot_render(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                 int w, int h, const struct plot_pt *pts, int npts, long now,
                 int hours, int radius, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color, long tz);

/* Return the index into pts of the in-window point nearest to pixel (tx,ty),
 * using the same mapping as plot_render, or -1 if there are no in-window
 * points. Lets the caller resolve a touch to a datapoint. */
int plot_hit(int x, int y, int w, int h, const struct plot_pt *pts, int npts,
             long now, int hours, int tx, int ty);

/* Draw a single marker glyph at (cx,cy), half-width r, for menu previews. Shape
 * codes match sensors.h MARK_* (HIDE is not a drawable shape). */
void plot_marker_glyph(uint32_t *fb, int stride, int fbw, int fbh, int cx,
                       int cy, int r, int shape, uint32_t c);

/* Pixel centre of point `p` under the same mapping as plot_render. Returns 1
 * and fills *ox,*oy if p is in-window; returns 0 otherwise. Used by the
 * offline harness to assert the out-of-range capping rule. */
int plot_point_xy(int x, int y, int w, int h, struct plot_pt p, long now,
                  int hours, int *ox, int *oy);

#endif
