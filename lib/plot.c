// SPDX-License-Identifier: GPL-3.0
// plot.c --- Glucose plot pixel rendering
// Copyright 2026 Jakob Kastelic

/* plot.c -- see plot.h. Pure pixel rendering; no dependencies beyond stdint. */
#include "plot.h"

#include "colors.h" /* the palette: every colour the app draws */
#include <limits.h> /* INT_MAX: the checked multiply below is an INT one */
#include <stddef.h> /* size_t: every buffer offset is formed in one */
#include <stdint.h>

/* THE DIMENSION A COORDINATE CAN STILL BE ADDED TO.
 *
 * Every shape here is stepped from centre - r to centre + r, and r is bounded
 * below by the buffer's own size, so a centre inside the buffer plus a radius
 * the size of it must still be an int. Half of INT_MAX is that rule and
 * nothing else: it is not the overflow bound for w*h, which is a checked
 * multiply in plot_fb_check, and it is four orders of magnitude above any
 * screen or plot image this draws into. */
#define PLOT_DIM_MAX (INT_MAX / 2)

/* Set one pixel, clipped to the framebuffer.
 *
 * THE SIGN TEST COMES BEFORE THE CONVERSION, and that order is the point:
 * (size_t)(-1) is not a small negative, it is SIZE_MAX, so a bounds test made
 * on the converted value passes precisely the coordinates that have to be
 * refused. x and y are compared here as the signed ints they are; only then
 * is the offset formed in size_t. The public boundary has already established
 * stride >= 1, stride >= fbw and fbh * stride <= INT_MAX (plot_fb_check), so
 * y * stride + x is both in range and unable to overflow -- which the old
 * `(y * stride) + x` in int was not, for any stride a caller cared to pass. */
static void put(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                uint32_t c)
{
   if (x < 0 || y < 0 || x >= fbw || y >= fbh)
      return;
   fb[((size_t)y * (size_t)stride) + (size_t)x] = c;
}

/* ---- THE GEOMETRY RULES (see plot.h) ---------------------------------- */

enum plot_geom plot_fb_check(struct plot_fb fb)
{
   if (!fb.px)
      return PLOT_GEOM_PIXELS;
   /* The signs are tested while these are still ints. A negative width or
    * height widened first is an enormous positive, which is why the order
    * matters even though the comparisons below look the same either way. */
   if (fb.w < 0 || fb.h < 0)
      return PLOT_GEOM_SIZE;
   if (fb.w > PLOT_DIM_MAX || fb.h > PLOT_DIM_MAX)
      return PLOT_GEOM_SIZE;
   if (fb.stride < 1)
      return PLOT_GEOM_STRIDE;
   /* A STRIDE NARROWER THAN THE WIDTH is the quiet one. The rows then overlap
    * and the last of them runs past the end of the allocation, but the first
    * rows draw perfectly -- so it looks like a working plot until the bottom
    * of it lands on whatever follows the buffer. */
   if (fb.stride < fb.w)
      return PLOT_GEOM_STRIDE;
   /* A CHECKED MULTIPLY, not a ceiling picked to sit below the overflow.
    * The buffer must hold (h-1)*stride + w pixels, so h*stride is what has to
    * be representable; dividing INT_MAX by the stride asks exactly that
    * question without performing the multiply that would wrap. INT rather
    * than size_t because int is the domain this API speaks -- a buffer whose
    * last pixel cannot be counted in an int is not one these four ints can
    * describe -- and because it is the int multiply that wraps negative and
    * indexes backwards out of the allocation. */
   if (fb.h > INT_MAX / fb.stride)
      return PLOT_GEOM_OVERFLOW;
   return PLOT_GEOM_OK;
}

enum plot_geom plot_render_check(struct plot_fb fb, struct plot_rect r,
                                 struct plot_cfg cfg)
{
   enum plot_geom g = plot_fb_check(fb);
   if (g != PLOT_GEOM_OK)
      return g;
   /* SIGNS FIRST, WIDEN AFTER -- the same trap one level up: a rectangle at
    * x = -4 converted to size_t is 2^64 - 4, and adding a width of 8 wraps it
    * back to 4, straight through a "does it fit the buffer" test made on the
    * converted values. */
   if (r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0)
      return PLOT_GEOM_RECT;
   /* Four pixels each way is the smallest thing that can be drawn: two rows
    * and two columns of frame with something between them. plot_render used
    * to test this after deriving a margin from it. */
   if (r.w < 4 || r.h < 4)
      return PLOT_GEOM_RECT;
   /* CHECKED ADDITION, now that both terms are known non-negative. */
   {
      size_t rx = (size_t)r.x;
      size_t rw = (size_t)r.w;
      size_t ry = (size_t)r.y;
      size_t rh = (size_t)r.h;
      if (rw > SIZE_MAX - rx || rh > SIZE_MAX - ry)
         return PLOT_GEOM_RECT;
      if (rx + rw > (size_t)fb.w || ry + rh > (size_t)fb.h)
         return PLOT_GEOM_RECT;
   }
   /* Radius 0 is not refused: plot.h documents a zeroed cfg as "the default",
    * and cfg_radius turns it into 1 for the render AND for the hit test, so
    * the two mappings still agree. Negative is not a default, and a marker
    * wider than the buffer it is drawn in is not a marker. */
   if (cfg.radius < 0)
      return PLOT_GEOM_RADIUS;
   if (cfg.radius > fb.w || cfg.radius > fb.h)
      return PLOT_GEOM_RADIUS;
   return PLOT_GEOM_OK;
}

enum plot_geom plot_glyph_check(struct plot_fb fb, int cx, int cy, int r)
{
   enum plot_geom g = plot_fb_check(fb);
   if (g != PLOT_GEOM_OK)
      return g;
   if (r < 0)
      return PLOT_GEOM_RADIUS;
   if (r > fb.w || r > fb.h)
      return PLOT_GEOM_RADIUS;
   /* The centre may sit anywhere, including off the buffer -- a preview drawn
    * half past an edge is clipped, as it always was. What may NOT happen is
    * the step from cx - r to cx + r leaving the int, which is undefined
    * behaviour before a single pixel is ever offered to the clip test. */
   if (cx > INT_MAX - r || cx < INT_MIN + r || cy > INT_MAX - r ||
       cy < INT_MIN + r)
      return PLOT_GEOM_RECT;
   return PLOT_GEOM_OK;
}

/* Plot rectangle a marker may paint into. A capped reading is centred on the
 * boundary gridline, so half its marker would otherwise land outside the frame
 * and paint over whatever is next to the plot. */
/* THE CLIP RECTANGLE, passed rather than stored.
 *
 * It was four file-scope ints that plot_render set and every helper read, so
 * two renders at once clipped each other's markers -- and plot_marker_glyph,
 * which sets its own, left them behind for whatever drew next. */
struct clip {
   int x0, y0, x1, y1;
};

static void putc_clipped(struct clip cl, uint32_t *fb, int stride, int fbw,
                         int fbh, int x, int y, uint32_t c)
{
   if (x < cl.x0 || x > cl.x1 || y < cl.y0 || y > cl.y1)
      return;
   put(fb, stride, fbw, fbh, x, y, c);
}

/* Filled square dot, half-width r, centred on (cx,cy). */
static void dot(struct clip cl, uint32_t *fb, int stride, int fbw, int fbh,
                int cx, int cy, int r, uint32_t c)
{
   for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++)
         putc_clipped(cl, fb, stride, fbw, fbh, cx + dx, cy + dy, c);
}

/* One marker of the given shape, half-width r, centred on (cx,cy). Shapes are
 * kept simple and open-centred (except the dot) so overlapping sensors stay
 * readable where their traces cross. */
/* One straight segment, stepped along whichever axis is longer so the line has
 * no gaps. Used by the W glyph; this module draws its own shapes and has no
 * general line routine. */
static void wseg(struct clip cl, uint32_t *fb, int stride, int fbw, int fbh,
                 int x0, int y0, int x1, int y1, uint32_t c)
{
   int dx  = x1 - x0;
   int dy  = y1 - y0;
   int adx = dx < 0 ? -dx : dx;
   int ady = dy < 0 ? -dy : dy;
   int n   = adx > ady ? adx : ady;
   if (n < 1)
      n = 1;
   /* LONG for the two products: dx and i are each bounded by twice the
    * buffer, so their product is not bounded by an int at all -- and this is
    * arithmetic done before the clip test can throw the pixel away. */
   for (int i = 0; i <= n; i++)
      putc_clipped(cl, fb, stride, fbw, fbh, x0 + (int)(((long)dx * i) / n),
                   y0 + (int)(((long)dy * i) / n), c);
}

static void mark(struct clip cl, uint32_t *fb, int stride, int fbw, int fbh,
                 int cx, int cy, int r, int shape, uint32_t c)
{
   /* Shape codes mirror sensors.h MARK_*: 0 dot, 1 cross, 2 square, 3 triangle,
    * 5 square-filled, 6 triangle-filled, 7 circle, 8 circle-filled. (4 = HIDE
    * never reaches here.) */
   switch (shape) {
      case PLOT_MARK_W: {
         /* A letter W: FOUR strokes, drawn as segments.
          *
          * Two diagonals stepped from the top corners to the bottom centre,
          * where they meet, draw a V, not a W. A W
          * needs two valleys either side of a centre peak, and the peak must
          * stop short of the top or the middle stroke closes into an X at
          * this size. Half height for the peak reads correctly down to r=2.
          *
          * seg() steps the DOMINANT axis so a stroke stays continuous; the
          * short axis alone would leave gaps in a glyph a few pixels tall. */
         int hw   = r;            /* half width */
         int vx   = r / 2;        /* where the two valleys sit */
         int peak = cy - (r / 2); /* the centre peak, half way up */
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy - r, cx - vx, cy + r, c);
         wseg(cl, fb, stride, fbw, fbh, cx - vx, cy + r, cx, peak, c);
         wseg(cl, fb, stride, fbw, fbh, cx, peak, cx + vx, cy + r, c);
         wseg(cl, fb, stride, fbw, fbh, cx + vx, cy + r, cx + hw, cy - r, c);
         return;
      }
      case PLOT_MARK_F: {
         /* A letter F: three strokes -- the upright, the top arm, and a
          * shorter middle arm. Drawn with the same seg() stepping as the W so
          * it stays continuous at small radii, and the middle arm is
          * deliberately shorter than the top one, which is what makes it read
          * as an F rather than an E at three or four pixels tall. */
         int hw  = r;
         int mid = cy;
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy - r, cx - hw, cy + r, c);
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy - r, cx + hw, cy - r, c);
         wseg(cl, fb, stride, fbw, fbh, cx - hw, mid, cx + (hw / 2), mid, c);
         return;
      }
      case PLOT_MARK_E: {
         /* A letter E: the F's three strokes with a fourth along the bottom,
          * and the middle arm SHORTER than the other two -- which is what
          * keeps the E and the F apart at three or four pixels tall, where
          * two full-width arms and one short one is the only difference a
          * reader has to go on. */
         int hw = r;
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy - r, cx - hw, cy + r, c);
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy - r, cx + hw, cy - r, c);
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy, cx + (hw / 2), cy, c);
         wseg(cl, fb, stride, fbw, fbh, cx - hw, cy + r, cx + hw, cy + r, c);
         return;
      }
      case 1: /* cross */
         for (int d = -r; d <= r; d++) {
            putc_clipped(cl, fb, stride, fbw, fbh, cx + d, cy + d, c);
            putc_clipped(cl, fb, stride, fbw, fbh, cx + d, cy - d, c);
         }
         return;
      case 2: /* open square */
         for (int d = -r; d <= r; d++) {
            putc_clipped(cl, fb, stride, fbw, fbh, cx + d, cy - r, c);
            putc_clipped(cl, fb, stride, fbw, fbh, cx + d, cy + r, c);
            putc_clipped(cl, fb, stride, fbw, fbh, cx - r, cy + d, c);
            putc_clipped(cl, fb, stride, fbw, fbh, cx + r, cy + d, c);
         }
         return;
      case 5: /* filled square */
         for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
               putc_clipped(cl, fb, stride, fbw, fbh, cx + dx, cy + dy, c);
         return;
      case 3: /* open triangle */
         for (int dy = -r; dy <= r; dy++) {
            int half = (dy + r) / 2;
            putc_clipped(cl, fb, stride, fbw, fbh, cx - half, cy + dy, c);
            putc_clipped(cl, fb, stride, fbw, fbh, cx + half, cy + dy, c);
         }
         for (int dx = -r; dx <= r; dx++)
            putc_clipped(cl, fb, stride, fbw, fbh, cx + dx, cy + r, c);
         return;
      case 6: /* filled triangle */
         for (int dy = -r; dy <= r; dy++) {
            int half = (dy + r) / 2;
            for (int dx = -half; dx <= half; dx++)
               putc_clipped(cl, fb, stride, fbw, fbh, cx + dx, cy + dy, c);
         }
         return;
      case 7:   /* open circle */
      case 8: { /* filled circle */
         /* SQUARED IN LONG. r is bounded by the buffer's smaller side, so r*r
          * is at most w*h -- which plot_fb_check keeps inside an int -- but
          * dx*dx + dy*dy is TWICE that at the corners of the box, and the
          * corner is where the loop starts. */
         long r2    = (long)r * r;
         long inner = (long)(r - 1) * (r - 1);
         for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
               long d2 = ((long)dx * dx) + ((long)dy * dy);
               if (d2 <= r2 && (shape == 8 || d2 > inner))
                  putc_clipped(cl, fb, stride, fbw, fbh, cx + dx, cy + dy, c);
            }
         return;
      }
      default: /* dot (0) */
         dot(cl, fb, stride, fbw, fbh, cx, cy, r, c);
         return;
   }
}

/* Draw one marker glyph anywhere (menu previews), clipped only to the buffer.
 * Standalone from plot_render, so it makes its own clip rectangle -- and does
 * not leave it behind for the next caller, which is what a file-scope one
 * did. */
void plot_marker_glyph(struct plot_fb b, int cx, int cy, int r, int shape,
                       uint32_t c)
{
   /* REFUSED, not clipped, and refused BEFORE the first pixel: the clip test
    * downstream bounds x and y, never the stride the offset is multiplied by.
    * `!= PLOT_GEOM_OK` because OK is zero (plot.h). */
   if (plot_glyph_check(b, cx, cy, r) != PLOT_GEOM_OK)
      return;
   uint32_t *fb   = b.px;
   int stride     = b.stride;
   int fbw        = b.w;
   int fbh        = b.h;
   struct clip cl = {0, 0, fbw - 1, fbh - 1};
   mark(cl, fb, stride, fbw, fbh, cx, cy, r, shape, c);
}

/* THE SCALE, FROM THE CALLER. `glu_max` 0 means "the default", so a caller
 * that has no preference passes {0} and gets PLOT_GLU_MAX; the clamp is the
 * a process-wide setter would apply, applied here where the value is used. */
static int cfg_max(struct plot_cfg cfg)
{
   int m = cfg.glu_max ? cfg.glu_max : PLOT_GLU_MAX;
   if (m < 100)
      m = 100;
   if (m > 400)
      m = 400;
   return m;
}

/* The radius a render will actually use. The clamp lives HERE, with the
 * margin that depends on it: plot_render clamped its own copy and the hit
 * test did not, so a cfg with radius 0 -- which is what a zeroed struct is --
 * drew at one x and picked at another. Two mappings that must agree cannot
 * each hold half the rule. */
static int cfg_radius(struct plot_cfg cfg)
{
   return cfg.radius < 1 ? 1 : cfg.radius;
}

/* The horizontal margin the radius implies. Derived, never stored: it was a
 * file-scope int that plot_render set and plot_hit read back afterwards, so
 * the hit test answered against the LAST render's radius -- a touch resolving
 * to the wrong datapoint on the first frame after any change. */
static int cfg_margin(struct plot_cfg cfg, int w)
{
   int m = ((cfg_radius(cfg) * 5) / 2) + 2;
   if ((2 * m) > (w - 4))
      m = (w - 4) / 2; /* never collapse the usable width */
   return m;
}

/* Map a glucose value to a pixel row inside the frame (clamped to the scale).
 */
static int glu_to_y(int glu, int y, int h, int glu_max)
{
   /* Out-of-range readings are capped, not dropped: a value above the scale
    * lands exactly on the plot_max gridline (and below the scale, exactly on
    * the bottom one), so an excursion is still visible and still sits on a row
    * the axis labels explain. plot_hit and plot_point_xy share this mapping, so
    * a capped point stays scrubbable where it is drawn. */
   if (glu < PLOT_GLU_MIN)
      glu = PLOT_GLU_MIN;
   if (glu > glu_max)
      glu = glu_max;
   return y + h - 2 -
          (int)((long)(h - 3) * (glu - PLOT_GLU_MIN) /
                (glu_max - PLOT_GLU_MIN));
}

/* X pixel for a reading `dt` seconds before now (newest at the right edge). */
static int t_to_x(long dt, int x, int w, long span, int t_margin)
{
   if (dt < 0)
      dt = 0;
   /* Pad the RIGHT edge only (t_margin), so the newest point isn't
    * half-clipped; the LEFT edge runs flush to the frame so an extra (older)
    * datapoint can show there. Hence a single t_margin in usable, not two. */
   int usable = w - 3 - t_margin;
   if (usable < 1)
      usable = 1;
   return x + w - 2 - t_margin - (int)((long)usable * dt / span);
}

void plot_render(struct plot_fb b, struct plot_rect rc,
                 const struct plot_pt *pts, int npts, long now, int hours,
                 struct plot_cfg cfg, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color, long tz)
{
   /* THE GEOMETRY, ONCE, BEFORE ANYTHING IS DRAWN. Every write below goes
    * through put(), which knows only x and y; the stride it multiplies y by
    * and the buffer it indexes are the caller's word, and this is where that
    * word is checked. `!= PLOT_GEOM_OK` because OK is zero (plot.h). */
   if (plot_render_check(b, rc, cfg) != PLOT_GEOM_OK)
      return;
   int radius           = cfg_radius(cfg);
   int glu_max          = cfg_max(cfg);
   uint32_t *fb         = b.px;
   int stride           = b.stride;
   int fbw              = b.w;
   int fbh              = b.h;
   int x                = rc.x;
   int y                = rc.y;
   int w                = rc.w;
   int h                = rc.h;
   const uint32_t frame = UI_PLOT_FRAME; /* 50/max reference lines + sides   */
   const uint32_t band  = UI_PLOT_BAND; /* very slight dark-gray shade 70-180 */
   const uint32_t vgrid = UI_PLOT_VGRID; /* faint vertical gridlines         */
   const uint32_t vtick = UI_PLOT_VTICK; /* brighter x-tick at the bottom    */
   long span            = (long)hours * 3600;
   /* Reserve enough at each end for the LARGEST marker (radius scaled up to
    * MARK_SIZE_MAX/2, +1 for styled points) so the newest datapoint is not half
    * cut off at the right edge. */
   struct plot_cfg use = {glu_max, radius};
   int t_margin        = cfg_margin(use, w);
   /* The rectangle is the boundary's business now (plot_render_check); a span
    * of zero or less is not geometry, it is an empty window. */
   if (span <= 0)
      return;
   /* No marker may be wider than the buffer it is drawn in: `size` below is a
    * per-device number read from slots.csv, and it multiplies this. */
   int rmax = b.w < b.h ? b.w : b.h;
   /* Clipped to the frame's inside, and the rectangle travels with the call
    * rather than living in the file. */
   struct clip cl = {x + 1, y + 1, x + w - 2, y + h - 2};

   int y50   = glu_to_y(50, y, h, glu_max);
   int y_top = glu_to_y(glu_max, y, h, glu_max);

   /* faint shade behind the 70-180 in-range band */
   int y_hi = glu_to_y(180, y, h, glu_max);
   int y_lo = glu_to_y(70, y, h, glu_max);
   for (int j = y_hi; j <= y_lo; j++)
      for (int i = 1; i < w - 1; i++)
         put(fb, stride, fbw, fbh, x + i, j, band);
   /* Thin light lines at the top (180) and bottom (70) edges of the range, so
    * the band stays legible in bright sunlight where the faint fill washes
    * out. */
   const uint32_t edge = UI_PLOT_EDGE;
   for (int i = 1; i < w - 1; i++) {
      put(fb, stride, fbw, fbh, x + i, y_hi, edge);
      put(fb, stride, fbw, fbh, x + i, y_lo, edge);
   }

   /* vertical gridlines + bottom x-ticks: hourly for hour-scale windows
    * (3 lines for 3H ... 24 for 24H), daily once the span exceeds a day
    * (3 lines for 3D, 7 for 7D) so multi-day plots aren't a picket fence */
   long gstep = (span <= 24L * 3600) ? 3600 : 24L * 3600;
   /* Anchor lines to exact clock boundaries -- the last full hour (or day)
    * before now -- so every vertical line lands on a round time rather than an
    * arbitrary offset back from now. (Whole-hour time zones; sub-hour zones
    * shift slightly as this layer carries no tz.) */
   /* In LOCAL time: the boundary the user reads off the axis is local
    * midnight (or the local hour), not UTC's. Shifting by tz before the
    * modulo is the whole fix -- with gstep = 3600 and a whole-hour zone it
    * changes nothing, which is why only the daily lines were visibly wrong. */
   long first = (now + tz) % gstep;
   if (first <= 0)
      first = gstep; /* exactly on a boundary: skip the right edge */
   for (long ts = first; ts <= span; ts += gstep) {
      int gx = t_to_x(ts, x, w, span, t_margin);
      for (int j = y_top + 1; j < y50; j++)
         put(fb, stride, fbw, fbh, gx, j, vgrid);
      for (int j = y50 - (2 * radius); j <= y50; j++)
         put(fb, stride, fbw, fbh, gx, j, vtick);
   }

   /* gray reference lines at the 50 and max bounds, plus vertical sides */
   for (int i = 0; i < w; i++) {
      put(fb, stride, fbw, fbh, x + i, y50, frame);
      put(fb, stride, fbw, fbh, x + i, y_top, frame);
   }
   for (int j = y_top; j <= y50; j++) {
      put(fb, stride, fbw, fbh, x, j, frame);
      put(fb, stride, fbw, fbh, x + w - 1, j, frame);
   }

   /* Markers may paint only inside the frame; a capped reading sits on the
    * boundary gridline and would otherwise spill past it. */

   /* one dot per in-window reading; the highlighted one drawn last, on top */
   int hx = -1;
   int hy = -1;
   for (int i = 0; i < npts; i++) {
      long dt = now - pts[i].t;
      if (dt < 0)
         dt = 0;
      if (dt > span)
         continue;
      if (pts[i].hidden) /* HIDE marker: this device is not drawn */
         continue;
      int px = t_to_x(dt, x, w, span, t_margin);
      int py = glu_to_y(pts[i].glu, y, h, glu_max);
      if (i == hi_idx) {
         hx = px;
         hy = py;
         continue;
      }
      /* An explicit colour means the point carries its own styling (a meter
       * reading, or a second sensor); otherwise fall back to the value-based
       * palette the caller supplied. Styled points are drawn a little larger
       * so a sparse fingerstick is visible against a dense CGM trace. */
      uint32_t c = pts[i].col ? pts[i].col : color(pts[i].glu);
      int r      = pts[i].col ? radius + 1 : radius;
      /* Per-device SIZE multiplies the span-scaled base radius (default size 2
       * == the base), so markers stay proportional across 3H..7D spans. */
      /* IN LONG, AND CLAMPED. `size` is data -- a file this app wrote, and
       * one a torn write or a hand-edit can leave any int in -- so the
       * multiply is done where it cannot wrap and the result is then held to
       * a radius the buffer can actually contain. */
      long sz = pts[i].size > 0 ? pts[i].size : 2;
      long rr = ((long)r * sz) / 2;
      if (rr < 1)
         rr = 1;
      if (rr > rmax)
         rr = rmax;
      /* THE RULE THAT GIVES A POINT A LENGTH, drawn UNDER the glyph so the
       * letter stays readable where the two meet.
       *
       * The span ends earlier in `dt` terms than the point begins -- dt counts
       * backwards from now -- so the end is to the RIGHT, and a span reaching
       * into the future (a session still running, measured against a clock
       * that has moved on) clamps to dt = 0, the right edge, rather than
       * wrapping round.
       *
       * TOO SHORT AND IT IS NOT DRAWN. Under about two glyph widths the rule
       * is a smudge on the letter's shoulder that says less than the letter
       * alone; the threshold is in PIXELS, not seconds, so a ten-minute
       * session shows a rule on a 3 h plot and correctly shows none on a
       * 30 d one, where it would be a single pixel claiming to be a duration.
       */
      if (pts[i].span > 0) {
         long dt_end = dt - pts[i].span;
         if (dt_end < 0)
            dt_end = 0;
         int ex     = t_to_x(dt_end, x, w, span, t_margin);
         int from   = px + (int)rr + 1;
         int min_px = (int)(4 * rr);
         if (ex - px > min_px)
            for (int gx = from; gx <= ex; gx++)
               putc_clipped(cl, fb, stride, fbw, fbh, gx, py, c);
      }
      mark(cl, fb, stride, fbw, fbh, px, py, (int)rr, pts[i].marker, c);
   }
   if (hx >= 0) {
      /* white vertical marker; only the dot itself is highlighted in colour */
      for (int j = y_top + 1; j < y50; j++)
         put(fb, stride, fbw, fbh, hx, j, UI_PLOT_SCRUB);
      /* Same clamp as the ordinary markers: radius is bounded by the buffer,
       * so radius + 2 need not be. */
      long hr = (long)radius + 2;
      if (hr > rmax)
         hr = rmax;
      dot(cl, fb, stride, fbw, fbh, hx, hy, (int)hr, hi_color);
   }
}

/* Pixel centre of one point under plot_render's own mapping. The app resolves
 * touches with plot_hit and never needs this, but it is the seam the offline
 * harness uses to assert the out-of-range capping rule (a reading above the
 * scale must land EXACTLY on the plot_max gridline) without decoding pixels.
 * Keeping the assertion honest requires exposing the mapping, not a copy. */
int plot_point_xy(struct plot_rect rc, struct plot_pt p, long now, int hours,
                  struct plot_cfg cfg, int *ox, int *oy)
{
   int x        = rc.x;
   int y        = rc.y;
   int w        = rc.w;
   int h        = rc.h;
   int t_margin = cfg_margin(cfg, w);
   long span    = (long)hours * 3600;
   if (span <= 0)
      return 0;
   long dt = now - p.t;
   if (dt < 0)
      dt = 0;
   if (dt > span)
      return 0;
   *ox = t_to_x(dt, x, w, span, t_margin);
   *oy = glu_to_y(p.glu, y, h, cfg_max(cfg));
   return 1;
}

/* SPLITTING A SHARED PIXEL COLUMN, for the hit test only.
 *
 * Two markers logged within the same few minutes land on the SAME column, and
 * a nearest-by-x pick can then only ever return one of them: the other is
 * drawn but permanently unscrubbable. A dose and a weight recorded in the same
 * sitting is the ordinary case of this, not a corner one.
 *
 * So each of them gets its own place to be picked FROM. A group of
 * markers on one column already owns, between them, the run of plot from
 * halfway to the marker on their left to halfway to the one on their right;
 * this shares that run out equally, spreading the group symmetrically about
 * its true column. The boundary between two co-located markers then falls on
 * the column they are drawn at -- press to the left of them for the first,
 * to the right for the second -- and each gets a fingertip of travel rather
 * than the single pixel that separates them in time.
 *
 * SYMMETRIC about the column, using the SMALLER of the two free sides, so the
 * group can never reach past the midpoint into a neighbour's half however
 * lopsided its surroundings are. When that free side is nothing -- another
 * marker sits one pixel away -- nothing is spread and the group keeps the
 * behaviour it had. Neighbours are never pushed along to make room: shifting
 * a marker the finger is nowhere near would drift a dense stretch of them
 * away from where they are drawn.
 *
 * The spread is invisible. Every marker in a group is drawn at the same
 * column, so whichever one a press resolves to, the highlight lands where the
 * finger already is.
 *
 * Returns the offset to add to point k's column; 0 when it keeps its own.
 * `lo`/`hi` bound the plot, standing in for a neighbour at either edge.
 * O(nc) per point, so O(nc^2) for a pick -- integer compares over a list
 * bounded by PLOT_SPLIT_MAX, and only the sparse doses line asks for it. */
static int split_off(const int *col, int nc, int k, int lo, int hi)
{
   int c    = col[k];
   int rank = 0; /* how many of the group precede this one */
   int tot  = 0; /* how many share the column, this one included */
   int cl   = lo;
   int cr   = hi;
   for (int j = 0; j < nc; j++) {
      if (col[j] == c) {
         if (j < k)
            rank++;
         tot++;
      } else if (col[j] < c) {
         if (col[j] > cl)
            cl = col[j];
      } else if (col[j] < cr) {
         cr = col[j];
      }
   }
   if (tot < 2)
      return 0;
   /* Half the distance to the nearer neighbour: the group's own half of the
    * space on the side where it has least. */
   int s  = (c - cl) / 2;
   int sr = (cr - c) / 2;
   if (sr < s)
      s = sr;
   if (s < 1)
      return 0; /* hemmed in: the next pixel is already someone else's */
   /* Cell centres across [c-s, c+s]: -s + (2*rank + 1) * (2s) / (2 * tot). */
   return ((((2 * rank) + 1) * s) / tot) - s;
}

int plot_hit(struct plot_rect rc, const struct plot_pt *pts, int npts, long now,
             int hours, struct plot_cfg cfg, int tx, int ty, int split)
{
   int x = rc.x;
   int y = rc.y;
   int w = rc.w;
   int h = rc.h;
   /* THE SAME MAPPING THE RENDER USED, because the caller passes the same
    * configuration. This read back the margin the last plot_render had left
    * in a file-scope int, so a hit test against a plot drawn with a different
    * radius -- the 3 h trace and the 30 d one are one tap apart -- resolved
    * to the wrong datapoint. */
   int t_margin = cfg_margin(cfg, w);
   /* Select purely by time (horizontal position) so dragging steps smoothly
    * through consecutive points; the finger's vertical position is ignored. */
   (void)y;
   (void)h;
   (void)ty;
   long span = (long)hours * 3600;
   if (span <= 0)
      return -1;
   int best   = -1;
   long bestd = 0;
   if (split) {
      /* Columns computed ONCE. split_off compares them against each other, so
       * recomputing t_to_x in there would put a divide inside an O(n^2) loop.
       *
       * ON THE STACK, one set per call: a plain static is scratch that two
       * picks would deal into each other, and this module is shared with a
       * server that renders concurrently. 4 KB for the duration of one pick
       * -- the earlier comment called that "far too big for the stack", which
       * is true of a signal handler and not of the touch path. plot.c still
       * allocates nothing. If the candidates overflow it the split is simply
       * skipped: the plain pass below still sees every point, so the failure
       * is "no split here", never "a point that cannot be selected". */
      int col[PLOT_SPLIT_MAX];
      int idx[PLOT_SPLIT_MAX];
      int nc  = 0;
      int ovf = 0;
      for (int i = 0; i < npts; i++) {
         if (pts[i].hidden)
            continue;
         long dt = now - pts[i].t;
         if (dt < 0)
            dt = 0;
         if (dt > span)
            continue;
         if (nc >= PLOT_SPLIT_MAX) {
            ovf = 1;
            break;
         }
         col[nc] = t_to_x(dt, x, w, span, t_margin);
         idx[nc] = i;
         nc++;
      }
      if (!ovf && nc > 0) {
         for (int k = 0; k < nc; k++) {
            long ddx = col[k] + split_off(col, nc, k, x, x + w) - tx;
            if (ddx < 0)
               ddx = -ddx;
            /* Same tie rule as the plain pass below, over the same order. */
            if (best < 0 || ddx <= bestd) {
               best  = idx[k];
               bestd = ddx;
            }
         }
         return best;
      }
   }
   for (int i = 0; i < npts; i++) {
      if (pts[i].hidden) /* a HIDDEN device is off the plot: not selectable */
         continue;
      long dt = now - pts[i].t;
      if (dt < 0)
         dt = 0;
      if (dt > span)
         continue;
      long ddx = t_to_x(dt, x, w, span, t_margin) - tx;
      if (ddx < 0)
         ddx = -ddx;
      /* `<=`, not `<`, so an EQUAL-distance tie resolves to the later-iterated
       * point. pts is newest-first, so that is the OLDER of the two. On a
       * multi-day span many 5-minute samples share one pixel column (a 7D plot
       * is ~15 min per pixel), and with a strict `<` the far-left drag could
       * never reach the true oldest sample -- the scrub stuck one or two
       * samples in from the edge and appeared to "evict" the oldest point as
       * newer data arrived. Preferring the older sample on a tie makes a drag
       * to the left edge land on the actual leftmost point that is drawn. */
      if (best < 0 || ddx <= bestd) {
         best  = i;
         bestd = ddx;
      }
   }
   return best;
}
