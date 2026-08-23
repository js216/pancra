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
   /* HOW LONG THIS POINT LASTED, in seconds. 0 for everything that is an
    * instant -- a reading, a dose, a weight -- which is all of them but
    * exercise.
    *
    * A nonzero span draws a thin horizontal rule from the marker's right edge
    * to where the span ENDS on the same axis the marker was placed on, so the
    * plot shows a workout as something with a length rather than as a moment.
    * It is deliberately drawn only when the span is wider than about two
    * glyphs: shorter than that the rule is a smudge against the letter and
    * says less than the letter alone. */
   long span;
};

/* A drawn letter W, for logged body weights on the doses line.
 *
 * NOT a MARK_* value: those are user-selectable device markers persisted in
 * slots.csv, and this must never appear in the marker picker or be storable as
 * a device's shape. It sits far above the MARK_ range so the two cannot
 * collide as shapes are appended there. plot.c stays font-free -- the letter
 * is four strokes, drawn like every other shape here, not a glyph lookup. */
#define PLOT_MARK_W 100
/* A letter F, for a logged FOOD entry. Numbered beside the W and well clear
 * of the MARK_* shape codes, for the same reason that one is: these are
 * lettered glyphs the app asks for by name, not styling a user can choose. */
#define PLOT_MARK_F 101
/* A letter E, for a logged EXERCISE entry, numbered beside the other two and
 * for the same reason. Unlike them it can carry a LENGTH: see `span`. */
#define PLOT_MARK_E 102

/* Vertical scale runs PLOT_GLU_MIN..(runtime max, default PLOT_GLU_MAX) mg/dL.
 */
#define PLOT_GLU_MIN 50
#define PLOT_GLU_MAX 300

/* WHAT ONE PLOT IS, as one value the caller owns.
 *
 * The vertical scale and the marker radius were PROCESS GLOBALS: plot_set_max
 * stored the scale, and plot_render stored the horizontal margin it derived
 * from the radius for plot_hit to read back afterwards. Two plots with
 * different settings therefore could not exist at once -- the server renders
 * several windows and the app draws a 3 h trace beside a 30 d one -- and the
 * hit test answered against whatever the LAST render had left behind, which
 * on the phone is a touch resolving to the wrong datapoint on the first
 * frame after a scale change.
 *
 * It is passed by value, so a plot's configuration cannot outlive the call or
 * be changed by another one. `radius` is the marker's half-width and also
 * fixes the horizontal margin, which is why the hit test needs it too: the
 * two must reproduce exactly the same mapping. */
struct plot_cfg {
   int glu_max; /* top of the vertical scale, mg/dL; 0 = PLOT_GLU_MAX */
   int radius;  /* marker half-width in pixels */
};

/* Render the readings in `pts` (any order, newest-first is fine) whose
 * timestamps fall within the last `hours` before `now` into the framebuffer
 * rectangle (x,y)-(x+w,y+h). Draws a frame and 100/200 gridlines, then one
 * dot per in-window reading. `color(glu)` yields each dot's colour, so the
 * palette stays with the caller. `fb` is RGBA_8888 with `stride` pixels per
 * row and total size `fbw`x`fbh`; writes are clipped to those bounds, and the
 * geometry itself is checked first (plot_render_check) -- a framebuffer or
 * rectangle that fails draws NOTHING rather than a clipped approximation of
 * something impossible. */
/* As above; the point at index `hi_idx` (if in-window) is drawn larger in
 * `hi_color` -- pass hi_idx < 0 for no highlight. */
/* `tz` is the local UTC offset in seconds, used ONLY to anchor the day
 * gridlines. Without it the 24 h step anchored on UTC midnight, so in UTC-7
 * every "day" line on the 3D/7D/30D plots sat at 17:00 local and the day
 * boundaries the grid exists to show were wrong by the offset. */
/* THE FRAMEBUFFER AND THE RECTANGLE ARE EACH ONE THING.
 *
 * plot_render took seventeen arguments, of which the first four were one
 * framebuffer and the next four one rectangle -- eight positional ints in a
 * row, all interchangeable to the compiler. plot_hit repeated six of them.
 * Naming them costs two tiny structs and removes a whole class of transposed-
 * int bug at every call site. */
struct plot_fb {
   uint32_t *px; /* pixels */
   int stride;   /* pixels per row, which is NOT always the width */
   int w, h;     /* the buffer's own size, for clipping */
};

struct plot_rect {
   int x, y, w, h;
};

/* WHY A GEOMETRY WAS REFUSED, and why refusing is a boundary job.
 *
 * Every pixel this module writes lands at `y * stride + x`. The clip test
 * bounded x and y against the buffer's own w/h and NOTHING bounded stride, so
 * the product was a signed int multiply over three numbers a caller could
 * choose freely -- and each of the three ways of choosing them is an
 * out-of-bounds WRITE, not a wrong picture:
 *
 *   - a NEGATIVE stride walks the rows backwards off the front of the
 *     allocation, and every row after the first is outside it;
 *   - a stride SMALLER than the width overlaps the rows, so the last one runs
 *     past the end -- the quiet one, because the first rows draw perfectly;
 *   - dimensions whose PRODUCT overflows int wrap to a small or negative
 *     offset that then passes any check made on the wrapped value.
 *
 * So the geometry is checked ONCE, at the public boundary, before a single
 * pixel is written, and the offsets are then formed in size_t. The rules are
 * exposed as predicates because the interesting corners cannot all be driven:
 * a buffer claiming 100000 x 100000 is 40 GB nobody can allocate, so that
 * rule is pinned by asking the predicate rather than by rendering (the same
 * split gif_dims_ok makes for four-gigapixel GIFs).
 *
 * PLOT_GEOM_OK IS ZERO, matching enum db_get and enum csv_field -- so it is
 * `!= PLOT_GEOM_OK` that means refused, and a bare `!check(...)` reads
 * exactly backwards. Spelled out at every call site here for that reason. */
enum plot_geom {
   PLOT_GEOM_OK = 0,
   PLOT_GEOM_PIXELS,   /* no pixels at all */
   PLOT_GEOM_STRIDE,   /* stride negative, zero, or narrower than the width */
   PLOT_GEOM_SIZE,     /* negative or unrepresentable buffer dimensions */
   PLOT_GEOM_OVERFLOW, /* width x height does not fit the API's own int */
   PLOT_GEOM_RECT,     /* the rectangle is impossible or off the buffer */
   PLOT_GEOM_RADIUS    /* a negative radius, or one larger than the buffer */
};

/* The buffer alone: what must hold before any pixel may be written into it.
 * plot_render and plot_marker_glyph both start here. */
enum plot_geom plot_fb_check(struct plot_fb fb);

/* The buffer, the rectangle to draw in, and the marker radius that will be
 * scaled up inside it -- everything plot_render needs before it draws. A
 * rectangle is refused rather than clipped: a plot drawn half off its buffer
 * is a layout fault, and silently painting the half that fits is how it stays
 * unnoticed. */
enum plot_geom plot_render_check(struct plot_fb fb, struct plot_rect r,
                                 struct plot_cfg cfg);

/* One free-standing glyph: the buffer, plus a radius and a centre whose
 * corners (cx +/- r, cy +/- r) must stay inside an int. The centre itself may
 * be anywhere -- a preview half off the edge is clipped, as it always was --
 * so only the arithmetic is bounded here, not the position. */
enum plot_geom plot_glyph_check(struct plot_fb fb, int cx, int cy, int r);

void plot_render(struct plot_fb fb, struct plot_rect r,
                 const struct plot_pt *pts, int npts, long now, int hours,
                 struct plot_cfg cfg, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color, long tz);

/* Candidates the column split (see `split` below) will consider. Sized for the
 * whole insulin + weight logs at once (NINS + NWT), which is every marker the
 * doses line can ever hold; past it the split is skipped, never the points. */
#define PLOT_SPLIT_MAX 512

/* Return the index into pts of the in-window point nearest to pixel (tx,ty),
 * using the same mapping as plot_render, or -1 if there are no in-window
 * points. Lets the caller resolve a touch to a datapoint.
 *
 * `split` deals markers that share one pixel column out to adjacent free
 * columns FOR THE PICK ONLY -- nothing moves on screen -- so two logged at the
 * same minute are both reachable rather than one shadowing the other. Pass it
 * for a SPARSE series (the doses line: insulin and weights), never for the
 * glucose trace, where hundreds of samples legitimately share a column on a
 * multi-day span and the pick must stay a plain nearest-in-time. */
int plot_hit(struct plot_rect r, const struct plot_pt *pts, int npts, long now,
             int hours, struct plot_cfg cfg, int tx, int ty, int split);

/* Draw a single marker glyph at (cx,cy), half-width r, for menu previews. Shape
 * codes match sensors.h MARK_* (HIDE is not a drawable shape). */
void plot_marker_glyph(struct plot_fb fb, int cx, int cy, int r, int shape,
                       uint32_t c);

/* Pixel centre of point `p` under the same mapping as plot_render. Returns 1
 * and fills *ox,*oy if p is in-window; returns 0 otherwise. Used by the
 * offline harness to assert the out-of-range capping rule. */
int plot_point_xy(struct plot_rect r, struct plot_pt p, long now, int hours,
                  struct plot_cfg cfg, int *ox, int *oy);

#endif
