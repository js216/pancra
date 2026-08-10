// SPDX-License-Identifier: GPL-3.0
// plot.c --- Glucose plot pixel rendering
// Copyright 2026 Jakob Kastelic

/* plot.c -- see plot.h. Pure pixel rendering; no dependencies beyond stdint. */
#include "plot.h"
#include <stdint.h>

/* Set one pixel, clipped to the framebuffer. */
static void put(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                uint32_t c)
{
   if ((unsigned)x < (unsigned)fbw && (unsigned)y < (unsigned)fbh)
      fb[(y * stride) + x] = c;
}

/* Plot rectangle a marker may paint into. A capped reading is centred on the
 * boundary gridline, so half its marker would otherwise land outside the frame
 * and paint over whatever is next to the plot. */
static int clip_x0, clip_y0, clip_x1, clip_y1;

static void putc_clipped(uint32_t *fb, int stride, int fbw, int fbh, int x,
                         int y, uint32_t c)
{
   if (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1)
      return;
   put(fb, stride, fbw, fbh, x, y, c);
}

/* Filled square dot, half-width r, centred on (cx,cy). */
static void dot(uint32_t *fb, int stride, int fbw, int fbh, int cx, int cy,
                int r, uint32_t c)
{
   for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++)
         putc_clipped(fb, stride, fbw, fbh, cx + dx, cy + dy, c);
}

/* One marker of the given shape, half-width r, centred on (cx,cy). Shapes are
 * kept simple and open-centred (except the dot) so overlapping sensors stay
 * readable where their traces cross. */
/* One straight segment, stepped along whichever axis is longer so the line has
 * no gaps. Used by the W glyph; this module draws its own shapes and has no
 * general line routine. */
static void wseg(uint32_t *fb, int stride, int fbw, int fbh, int x0, int y0,
                 int x1, int y1, uint32_t c)
{
   int dx  = x1 - x0;
   int dy  = y1 - y0;
   int adx = dx < 0 ? -dx : dx;
   int ady = dy < 0 ? -dy : dy;
   int n   = adx > ady ? adx : ady;
   if (n < 1)
      n = 1;
   for (int i = 0; i <= n; i++)
      putc_clipped(fb, stride, fbw, fbh, x0 + ((dx * i) / n),
                   y0 + ((dy * i) / n), c);
}

static void mark(uint32_t *fb, int stride, int fbw, int fbh, int cx, int cy,
                 int r, int shape, uint32_t c)
{
   /* Shape codes mirror sensors.h MARK_*: 0 dot, 1 cross, 2 square, 3 triangle,
    * 5 square-filled, 6 triangle-filled, 7 circle, 8 circle-filled. (4 = HIDE
    * never reaches here.) */
   switch (shape) {
      case PLOT_MARK_W: {
         /* A letter W: FOUR strokes, drawn as segments.
          *
          * The first version stepped two diagonals from the top corners to
          * the bottom centre, where they met -- which is a V, not a W. A W
          * needs two valleys either side of a centre peak, and the peak must
          * stop short of the top or the middle stroke closes into an X at
          * this size. Half height for the peak reads correctly down to r=2.
          *
          * seg() steps the DOMINANT axis so a stroke stays continuous; the
          * short axis alone would leave gaps in a glyph a few pixels tall. */
         int hw   = r;            /* half width */
         int vx   = r / 2;        /* where the two valleys sit */
         int peak = cy - (r / 2); /* the centre peak, half way up */
         wseg(fb, stride, fbw, fbh, cx - hw, cy - r, cx - vx, cy + r, c);
         wseg(fb, stride, fbw, fbh, cx - vx, cy + r, cx, peak, c);
         wseg(fb, stride, fbw, fbh, cx, peak, cx + vx, cy + r, c);
         wseg(fb, stride, fbw, fbh, cx + vx, cy + r, cx + hw, cy - r, c);
         return;
      }
      case 1: /* cross */
         for (int d = -r; d <= r; d++) {
            putc_clipped(fb, stride, fbw, fbh, cx + d, cy + d, c);
            putc_clipped(fb, stride, fbw, fbh, cx + d, cy - d, c);
         }
         return;
      case 2: /* open square */
         for (int d = -r; d <= r; d++) {
            putc_clipped(fb, stride, fbw, fbh, cx + d, cy - r, c);
            putc_clipped(fb, stride, fbw, fbh, cx + d, cy + r, c);
            putc_clipped(fb, stride, fbw, fbh, cx - r, cy + d, c);
            putc_clipped(fb, stride, fbw, fbh, cx + r, cy + d, c);
         }
         return;
      case 5: /* filled square */
         for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
               putc_clipped(fb, stride, fbw, fbh, cx + dx, cy + dy, c);
         return;
      case 3: /* open triangle */
         for (int dy = -r; dy <= r; dy++) {
            int half = (dy + r) / 2;
            putc_clipped(fb, stride, fbw, fbh, cx - half, cy + dy, c);
            putc_clipped(fb, stride, fbw, fbh, cx + half, cy + dy, c);
         }
         for (int dx = -r; dx <= r; dx++)
            putc_clipped(fb, stride, fbw, fbh, cx + dx, cy + r, c);
         return;
      case 6: /* filled triangle */
         for (int dy = -r; dy <= r; dy++) {
            int half = (dy + r) / 2;
            for (int dx = -half; dx <= half; dx++)
               putc_clipped(fb, stride, fbw, fbh, cx + dx, cy + dy, c);
         }
         return;
      case 7:   /* open circle */
      case 8: { /* filled circle */
         int r2    = r * r;
         int inner = (r - 1) * (r - 1);
         for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
               int d2 = (dx * dx) + (dy * dy);
               if (d2 <= r2 && (shape == 8 || d2 > inner))
                  putc_clipped(fb, stride, fbw, fbh, cx + dx, cy + dy, c);
            }
         return;
      }
      default: /* dot (0) */ dot(fb, stride, fbw, fbh, cx, cy, r, c); return;
   }
}

/* Draw one marker glyph anywhere (menu previews), clipped only to the buffer.
 * Standalone from plot_render, so it sets its own clip rectangle. */
void plot_marker_glyph(uint32_t *fb, int stride, int fbw, int fbh, int cx,
                       int cy, int r, int shape, uint32_t c)
{
   clip_x0 = 0;
   clip_y0 = 0;
   clip_x1 = fbw - 1;
   clip_y1 = fbh - 1;
   mark(fb, stride, fbw, fbh, cx, cy, r, shape, c);
}

/* Top of the vertical scale in mg/dL; runtime-adjustable (PLOT MAX setting). */
static int g_glu_max = PLOT_GLU_MAX;

void plot_set_max(int mgdl)
{
   if (mgdl >= 100 && mgdl <= 400)
      g_glu_max = mgdl;
}

/* Map a glucose value to a pixel row inside the frame (clamped to the scale).
 */
static int glu_to_y(int glu, int y, int h)
{
   /* Out-of-range readings are capped, not dropped: a value above the scale
    * lands exactly on the plot_max gridline (and below the scale, exactly on
    * the bottom one), so an excursion is still visible and still sits on a row
    * the axis labels explain. plot_hit and plot_point_xy share this mapping, so
    * a capped point stays scrubbable where it is drawn. */
   if (glu < PLOT_GLU_MIN)
      glu = PLOT_GLU_MIN;
   if (glu > g_glu_max)
      glu = g_glu_max;
   return y + h - 2 -
          (int)((long)(h - 3) * (glu - PLOT_GLU_MIN) /
                (g_glu_max - PLOT_GLU_MIN));
}

/* X pixel for a reading `dt` seconds before now (newest at the right edge). */
/* Horizontal margin (px) reserved at each end so a marker centred on the newest
 * or oldest datapoint is not half-clipped by the frame. Set from the marker
 * radius in plot_render; plot_hit reuses the last value. */
static int t_margin = 4;

static int t_to_x(long dt, int x, int w, long span)
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

void plot_render(uint32_t *fb, int stride, int fbw, int fbh, int x, int y,
                 int w, int h, const struct plot_pt *pts, int npts, long now,
                 int hours, int radius, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color, long tz)
{
   const uint32_t frame = 0xFF555555; /* 50/max reference lines + sides   */
   const uint32_t band  = 0xFF262626; /* very slight dark-gray shade 70-180 */
   const uint32_t vgrid = 0xFF2E2E2E; /* faint vertical gridlines         */
   const uint32_t vtick = 0xFF666666; /* brighter x-tick at the bottom    */
   long span            = (long)hours * 3600;
   if (radius < 1)
      radius = 1;
   /* Reserve enough at each end for the LARGEST marker (radius scaled up to
    * MARK_SIZE_MAX/2, +1 for styled points) so the newest datapoint is not half
    * cut off at the right edge. */
   t_margin = ((radius * 5) / 2) + 2;
   if ((2 * t_margin) > (w - 4))
      t_margin = (w - 4) / 2; /* never collapse the usable width */
   if (span <= 0 || w < 4 || h < 4)
      return;

   int y50   = glu_to_y(50, y, h);
   int y_top = glu_to_y(g_glu_max, y, h);

   /* faint shade behind the 70-180 in-range band */
   int y_hi = glu_to_y(180, y, h);
   int y_lo = glu_to_y(70, y, h);
   for (int j = y_hi; j <= y_lo; j++)
      for (int i = 1; i < w - 1; i++)
         put(fb, stride, fbw, fbh, x + i, j, band);
   /* Thin light lines at the top (180) and bottom (70) edges of the range, so
    * the band stays legible in bright sunlight where the faint fill washes
    * out. */
   const uint32_t edge = 0xFFAAAAAA;
   for (int i = 1; i < w - 1; i++) {
      put(fb, stride, fbw, fbh, x + i, y_hi, edge);
      put(fb, stride, fbw, fbh, x + i, y_lo, edge);
   }

   /* vertical gridlines + bottom x-ticks: hourly for hour-scale windows
    * (3 lines for 3H ... 24 for 24H), daily once the span exceeds a day
    * (3 lines for 3D, 7 for 7D) so multi-day plots aren't a picket fence */
   long gstep = (span <= 24L * 3600) ? 3600 : 24L * 3600;
   /* Anchor lines to exact clock boundaries -- the last full hour (or day)
    * before now -- so every vertical line lands on a round time instead of an
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
      int gx = t_to_x(ts, x, w, span);
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
   clip_x0 = x + 1;
   clip_y0 = y + 1;
   clip_x1 = x + w - 2;
   clip_y1 = y + h - 2;

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
      int px = t_to_x(dt, x, w, span);
      int py = glu_to_y(pts[i].glu, y, h);
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
      int sz = pts[i].size > 0 ? pts[i].size : 2;
      r      = (r * sz) / 2;
      if (r < 1)
         r = 1;
      mark(fb, stride, fbw, fbh, px, py, r, pts[i].marker, c);
   }
   if (hx >= 0) {
      /* white vertical marker; only the dot itself is highlighted in colour */
      for (int j = y_top + 1; j < y50; j++)
         put(fb, stride, fbw, fbh, hx, j, 0xFFFFFFFF);
      dot(fb, stride, fbw, fbh, hx, hy, radius + 2, hi_color);
   }
}

/* Pixel centre of one point under plot_render's own mapping. The app resolves
 * touches with plot_hit and never needs this, but it is the seam the offline
 * harness uses to assert the out-of-range capping rule (a reading above the
 * scale must land EXACTLY on the plot_max gridline) without decoding pixels.
 * Keeping the assertion honest requires exposing the mapping, not a copy. */
int plot_point_xy(int x, int y, int w, int h, struct plot_pt p, long now,
                  int hours, int *ox, int *oy)
{
   long span = (long)hours * 3600;
   if (span <= 0)
      return 0;
   long dt = now - p.t;
   if (dt < 0)
      dt = 0;
   if (dt > span)
      return 0;
   *ox = t_to_x(dt, x, w, span);
   *oy = glu_to_y(p.glu, y, h);
   return 1;
}

/* SPLITTING A SHARED PIXEL COLUMN, for the hit test only.
 *
 * Two markers logged within the same few minutes land on the SAME column, and
 * a nearest-by-x pick can then only ever return one of them: the other is
 * drawn but permanently unscrubbable. A dose and a weight recorded in the same
 * sitting is the ordinary case of this, not a corner one.
 *
 * The fix is to give each of them its own place to be picked FROM. A group of
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

int plot_hit(int x, int y, int w, int h, const struct plot_pt *pts, int npts,
             long now, int hours, int tx, int ty, int split)
{
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
       * Static, because plot.c allocates nothing and this is far too big for
       * the stack; single-threaded like the rest of the touch path. If the
       * candidates overflow it the split is simply skipped -- the plain pass
       * below still sees every point, so the failure is "no split here", never
       * "a point that cannot be selected". */
      static int col[PLOT_SPLIT_MAX];
      static int idx[PLOT_SPLIT_MAX];
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
         col[nc] = t_to_x(dt, x, w, span);
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
      long ddx = t_to_x(dt, x, w, span) - tx;
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
