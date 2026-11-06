// SPDX-License-Identifier: GPL-3.0
// uigluhist.c --- GLUCOSE HISTORY: one day, week, month or year
// Copyright 2026 Jakob Kastelic

/* FROM THE TOP: the window's first day as the title, YYYY-MM-DD, its year
 * and its MM-DD each a tap away from a keypad, and the X that closes the
 * screen; two rows of four buttons that step the date a day or a week, then a
 * month or a year, either way; the span row, laid out as the main screen's tab
 * row; the glucose plot of the window, drawn and scrubbed as the main screen's;
 * then how the window's CGM readings split below, inside and above 70-180; and
 * in what height is left, the window's ambulatory glucose profile -- every
 * reading folded onto one 24-hour day, as the 5th, 25th, 50th, 75th and 95th
 * percentile of each half hour.
 *
 * THE WIDTH BUDGET. Four equal step buttons, the two 12 sc buttons that go
 * to the first reading and to today, and five 2 sc gaps across the 190 sc
 * between the margins of the narrowest screen ui_fit_scale allows (33
 * characters) give each step button 39 sc, and the widest label, "-MONTH" or
 * "+MONTH", takes 35 sc of ink. */
#include "colors.h"
#include "font.h"
#include "gluhist.h" /* GH_*: the step buttons' order */
#include "plot.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"

#include "ndk.h"
#include <stdint.h>
#include <stdio.h>

static const char *const g_span_lbl[HSPAN_N] = {"DAY", "WEEK", "MONTH", "3 MON",
                                                "YEAR"};

/* One framed button, its label centred on its ink. */
static void gh_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                      int y, int w, int bh, int sc, const char *label,
                      uint32_t col, int code, int ix)
{
   uint32_t *px = fb->bits;
   const int lw = ((str_len(label) * 6) - 1) * sc;
   draw_frame(px, fb, x, y, w, bh, UI_MUTED);
   draw_str(px, fb, x + ((w - lw) / 2), y + ((bh - (7 * sc)) / 2), sc, label,
            col);
   add_hit_ix(h, ui_rect(x, y, w, bh), code, ix);
}

/* WHOLE PERCENTAGES THAT ADD UP TO 100: each share rounded down, and the
 * points left over handed to the largest remainders. Rounding each one alone
 * can print 33 + 33 + 33 for three equal thirds. */
static void shares(const int n[3], int pct[3])
{
   const long tot = (long)n[0] + n[1] + n[2];
   long rem[3];
   int left = 100;
   for (int k = 0; k < 3; k++) {
      pct[k] = tot > 0 ? (int)((n[k] * 100L) / tot) : 0;
      rem[k] = tot > 0 ? (n[k] * 100L) % tot : 0;
      left -= pct[k];
   }
   while (tot > 0 && left > 0) {
      int best = 0;
      for (int k = 1; k < 3; k++)
         if (rem[k] > rem[best])
            best = k;
      pct[best]++;
      rem[best] = -1;
      left--;
   }
}

/* THE BAR AND ITS FIGURES: one full-width bar, red, green and orange in
 * proportion, and under it the three shares in the same colours: below 70 at
 * the left, in range in the middle, above 180 at the right. Returns the
 * y below the figures. */
static int range_rows(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      int x, int w, int y, int sc)
{
   uint32_t *px                 = fb->bits;
   const struct ui_glusum *sum  = m->gh.sum;
   const int n[3]               = {sum ? sum->n_low : 0, sum ? sum->n_in : 0,
                                   sum ? sum->n_high : 0};
   const long tot               = (long)n[0] + n[1] + n[2];
   const int bh                 = 2 * sc;
   static const uint32_t col[3] = {UI_RANGE_LOW, UI_RANGE_IN, UI_RANGE_HIGH};
   if (tot == 0) {
      fill_rect(px, fb, x, y, w, bh, UI_RULE);
      y += bh + (4 * sc);
      const char *msg = "NO READINGS";
      const int mw    = ((str_len(msg) * 6) - 1) * sc;
      draw_str(px, fb, x + ((w - mw) / 2), y, sc, msg, UI_MUTED);
      return y + (7 * sc);
   }
   /* Each segment ends where the running count says, so the three always
    * meet exactly and fill the width. */
   long run = 0;
   int bx   = x;
   for (int k = 0; k < 3; k++) {
      run += n[k];
      const int ex = x + (int)((run * w) / tot);
      if (ex > bx)
         fill_rect(px, fb, bx, y, ex - bx, bh, col[k]);
      bx = ex;
   }
   y += bh + (4 * sc);
   int pct[3];
   shares(n, pct);
   /* The thresholds in the units the user reads glucose in. */
   char lo[12];
   char hi[12];
   fmt_glu(70, m->prefs.units, lo, sizeof lo);
   fmt_glu(180, m->prefs.units, hi, sizeof hi);
   char pre[2][8];
   (void)snprintf(pre[0], sizeof pre[0], "%d%% ", pct[0]);
   (void)snprintf(pre[1], sizeof pre[1], "%d%% ", pct[2]);
   char tir[16];
   (void)snprintf(tir, sizeof tir, "TIR %d%%", pct[1]);
   /* Below range at the left edge, above it at the right, in range between
    * them, each inset like the bar's own ends. The comparison is the sign,
    * one character cell wide, between the share and the threshold. */
   const int inset = 2 * sc;
   const int whi   = (((str_len(pre[1]) + 1 + str_len(hi)) * 6) - 1) * sc;
   const int wtir  = ((str_len(tir) * 6) - 1) * sc;
   const char *const post[2]    = {lo, hi};
   const uint8_t *const sign[2] = {icon_less, icon_more};
   const int sx[2]              = {x + inset, x + w - inset - whi};
   const uint32_t scol[2]       = {col[0], col[2]};
   for (int k = 0; k < 2; k++) {
      int cx = sx[k];
      draw_str(px, fb, cx, y, sc, pre[k], scol[k]);
      cx += str_len(pre[k]) * 6 * sc;
      draw_icon(px, fb, cx, y, sc, sign[k], scol[k]);
      cx += 6 * sc;
      draw_str(px, fb, cx, y, sc, post[k], scol[k]);
   }
   draw_str(px, fb, x + ((w - wtir) / 2), y, sc, tir, col[1]);
   return y + (7 * sc);
}

/* `v` held to lo..hi, so a value always fits the field printed for it. */
static long clamp_l(long v, long lo, long hi)
{
   if (v < lo)
      return lo;
   if (v > hi)
      return hi;
   return v;
}

/* One percentile at a fractional bin position, in 1/256 of a bin: the two
 * neighbouring half hours interpolated, wrapping at midnight. When only one
 * holds a reading, its value alone, but only across its own half hour, so a
 * stretch of readings ends where its last bin does; 0 elsewhere. */
static int agp_at(const struct ui_glusum *s, long f256, int k)
{
   long b0 = f256 / 256;
   long t  = f256 % 256;
   if (t < 0) {
      t += 256;
      b0--;
   }
   b0 %= UI_AGP_BINS;
   if (b0 < 0)
      b0 += UI_AGP_BINS;
   const long b1 = (b0 + 1) % UI_AGP_BINS;
   const int h0  = s->agp[b0].have;
   const int h1  = s->agp[b1].have;
   if (h0 && h1)
      return (int)(((s->agp[b0].p[k] * (256 - t)) + (s->agp[b1].p[k] * t)) /
                   256);
   if (h0 && t < 128)
      return s->agp[b0].p[k];
   if (h1 && t >= 128)
      return s->agp[b1].p[k];
   return 0;
}

/* THE AMBULATORY GLUCOSE PROFILE in `r`: midnight to midnight left to right,
 * on the main plot's glucose scale and with its 70-180 band, gridlines every
 * three hours. The 5-95 band is filled dark, 25-75 lighter, and the median
 * drawn as a line over both. */
static void agp_plot(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct plot_rect r, int sc)
{
   uint32_t *px  = fb->bits;
   const int top = plot_cfg_max((struct plot_cfg){m->plot.plot_max, 1, 0});
   const int bot = PLOT_GLU_MIN;
   if (r.w < 4 || r.h < 4 || top <= bot)
      return;
   const int y0 = r.y;
   const int y1 = r.y + r.h - 1;
#define AGP_Y(g)                                                               \
   ((g) >= top   ? y0                                                          \
    : (g) <= bot ? y1                                                          \
                 : y1 - (int)(((long)((g) - bot) * (y1 - y0)) / (top - bot)))
   const int yhi = AGP_Y(180);
   const int ylo = AGP_Y(70);
   fill_rect(px, fb, r.x + 1, yhi, r.w - 2, ylo - yhi + 1, UI_PLOT_BAND);
   for (int hr = 3; hr < 24; hr += 3) {
      const int gx = r.x + 1 + (int)(((long)hr * (r.w - 2)) / 24);
      fill_rect(px, fb, gx, y0 + 1, 1, y1 - y0 - 1, UI_PLOT_VGRID);
   }
   const struct ui_glusum *s = m->gh.sum;
   const int inner_w         = r.w - 2;
   int prev_med              = -1;
   for (int i = 0; s && i < inner_w; i++) {
      /* the column's centre, as a bin position less half a bin: a bin's
       * value belongs to its middle */
      const long f256 =
          (((((long)i * 2) + 1) * UI_AGP_BINS * 256) / (2L * inner_w)) - 128;
      const int p5 = agp_at(s, f256, 0);
      if (p5 <= 0) {
         prev_med = -1;
         continue;
      }
      const int gx  = r.x + 1 + i;
      const int a95 = AGP_Y(agp_at(s, f256, 4));
      const int a5  = AGP_Y(p5);
      const int a75 = AGP_Y(agp_at(s, f256, 3));
      const int a25 = AGP_Y(agp_at(s, f256, 1));
      const int med = AGP_Y(agp_at(s, f256, 2));
      fill_rect(px, fb, gx, a95, 1, a5 - a95 + 1, UI_AGP_OUTER);
      fill_rect(px, fb, gx, a75, 1, a25 - a75 + 1, UI_AGP_INNER);
      /* joined to the last column's, so a steep stretch is still a line */
      int m0 = med;
      int m1 = med;
      if (prev_med >= 0) {
         m0 = prev_med < med ? prev_med : med;
         m1 = prev_med < med ? med : prev_med;
      }
      const int lw = sc < 2 ? 1 : (sc / 2) + 1;
      int my       = m0 - (lw / 2);
      if (my < y0)
         my = y0;
      int mh = (m1 - m0) + lw;
      if (my + mh > y1 + 1)
         mh = y1 + 1 - my;
      fill_rect(px, fb, gx, my, 1, mh, UI_AGP_MEDIAN);
      prev_med = med;
   }
   /* The edges of the range over the fill, as the main plot draws them. */
   fill_rect(px, fb, r.x + 1, yhi, r.w - 2, 1, UI_PLOT_EDGE);
   fill_rect(px, fb, r.x + 1, ylo, r.w - 2, 1, UI_PLOT_EDGE);
   draw_frame(px, fb, r.x, y0, r.w, r.h, UI_PLOT_FRAME);
   const int lsc = FONT_NOTE(sc) < 1 ? 1 : FONT_NOTE(sc);
   char lab[12];
   fmt_glu(180, m->prefs.units, lab, sizeof lab);
   draw_str(px, fb, r.x + (2 * lsc), yhi - (8 * lsc), lsc, lab, UI_DISCLAIM);
   fmt_glu(70, m->prefs.units, lab, sizeof lab);
   draw_str(px, fb, r.x + (2 * lsc), ylo - (8 * lsc), lsc, lab, UI_DISCLAIM);
   for (int hr = 6; hr < 24; hr += 6) {
      const int gx = r.x + 1 + (int)(((long)hr * (r.w - 2)) / 24);
      (void)snprintf(lab, sizeof lab, "%02d:00", hr);
      draw_str(px, fb, gx + (2 * lsc), y1 - (8 * lsc), lsc, lab, UI_DISCLAIM);
   }
   /* The axis's two ends, midnight to midnight, inside the frame at the
    * left and right edges. */
   draw_str(px, fb, r.x + (2 * lsc), y1 - (8 * lsc), lsc, "00:00", UI_DISCLAIM);
   draw_str(px, fb, r.x + r.w - (2 * lsc) - (((5 * 6) - 1) * lsc),
            y1 - (8 * lsc), lsc, "24:00", UI_DISCLAIM);
   /* THE LEGEND, in the main plot's streak style -- the normal font, grey,
    * half a character in from the corner -- with a swatch of each band and of
    * the median line before its name, the three spread evenly across the
    * top: the first at the left, the last at the right, equal gaps between.
    * Drawn last, so it reads over the bands. */
   {
      const int inset                  = (7 * sc) / 2;
      const int ly                     = y0 + (2 * sc) + inset;
      const int sq                     = 5 * sc;
      static const char *const name[3] = {"5-95%", "25-75%", "MEDIAN"};
      int iw[3];
      int tot = 0;
      for (int k = 0; k < 3; k++) {
         iw[k] = sq + (3 * sc) + (((str_len(name[k]) * 6) - 1) * sc);
         tot += iw[k];
      }
      const int l0  = r.x + (2 * sc) + inset;
      const int l1  = r.x + r.w - (2 * sc) - inset;
      const int gap = (l1 - l0 - tot) / 2;
      int lx        = l0;
      for (int k = 0; k < 3; k++) {
         if (k < 2)
            fill_rect(px, fb, lx, ly + sc, sq, sq,
                      k == 0 ? UI_AGP_OUTER : UI_AGP_INNER);
         else
            fill_rect(px, fb, lx, ly + (3 * sc), sq, sc < 2 ? 1 : (sc / 2) + 1,
                      UI_AGP_MEDIAN);
         draw_str(px, fb, lx + sq + (3 * sc), ly, sc, name[k], UI_MUTED);
         lx += iw[k] + gap;
      }
   }
#undef AGP_Y
}

void render_gluhist(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px  = fb->bits;
   const int sc  = ui_fit_scale(fb->width, fb->height, 20);
   const int tsc = FONT_TITLE(sc);
   const int lh  = 16 * sc;
   const int x   = 4 * sc;
   const int rx  = fb->width - (4 * sc);
   const int bh  = 25 * sc;
   const int gap = 2 * sc;
   int y         = (fb->height / 20) + (8 * sc);

   /* THE X CLOSES THE SCREEN, and only the X: the rest of the title line is
    * the date, whose parts are targets of their own. */
   const int xx = rx - (6 * tsc);
   draw_str(px, fb, xx, y, tsc, "X", UI_TEXT);
   add_hit_ix(h,
              ui_rect(xx - (3 * sc), y - (3 * sc), fb->width - xx + (3 * sc),
                      (2 * lh) - (2 * sc)),
              MA_GLUHIST_BACK, 0);

   /* THE TITLE IS THE DATE, YYYY-MM-DD: the window's first day, whatever the
    * span. A tap on the year opens a keypad for the year, one on MM-DD a
    * keypad for both as MMDD. */
   char ys[8];
   char md[8];
   (void)snprintf(ys, sizeof ys, "%04ld", clamp_l(m->gh.y, 0, 9999));
   (void)snprintf(md, sizeof md, "%02ld-%02ld", clamp_l(m->gh.mo, 1, 12),
                  clamp_l(m->gh.d, 1, 31));

   /* The pieces in reading order, each with the part a tap on it retypes,
    * or -1 for the separator. */
   const struct {
      const char *s;
      int part;
   } seg[3] = {
       {ys,  GH_PART_YEAR},
       {"-", -1          },
       {md,  GH_PART_MMDD}
   };

   const int ns = 3;
   int dx       = x;
   for (int k = 0; k < ns; k++) {
      const int w = str_len(seg[k].s) * 6 * tsc;
      draw_str(px, fb, dx, y, tsc, seg[k].s, UI_TEXT);
      /* The hit covers the piece and, toward a neighbouring separator or
       * weekday, half a character of it, so a separator is shared between
       * the parts either side; two parts that touch meet at their common
       * edge. The first reaches the screen's edge. */
      if (seg[k].part >= 0) {
         const int open_l = k > 0 && seg[k - 1].part < 0;
         const int open_r = k + 1 < ns && seg[k + 1].part < 0;
         const int hx     = k == 0 ? 0 : dx - (open_l ? 3 * tsc : 0);
         const int hr     = dx + w + (open_r ? 3 * tsc : 0);
         add_hit_ix(h, ui_rect(hx, y - (3 * sc), hr - hx, (2 * lh) - (2 * sc)),
                    MA_GLUHIST_DATE, seg[k].part);
      }
      dx += w;
   }
   y += 2 * lh;

   /* The step columns leave room either side for the buttons that go to
    * the first reading and to today: the pager's |< and >| icons at the
    * title size, 1 sc of padding either side. */
   const int ew = (5 * tsc) + (2 * sc);
   const int sx = x + ew + gap;
   const int sw = (rx - x - (2 * ew) - (5 * gap)) / 4;

   /* THE STEPS, two rows of four: days and weeks, then months and years,
    * each backward from the larger unit to the smaller and forward from the
    * smaller to the larger, so each pair mirrors about the middle. See the
    * width budget above. */
   {
      static const char *const lab[2][4] = {
          {"-WEEK", "-DAY",   "+DAY",   "+WEEK"},
          {"-YEAR", "-MONTH", "+MONTH", "+YEAR"},
      };
      static const int ix[2][4] = {
          {GH_WEEK_BACK, GH_DAY_BACK,   GH_DAY_ON,   GH_WEEK_ON},
          {GH_YEAR_BACK, GH_MONTH_BACK, GH_MONTH_ON, GH_YEAR_ON},
      };
      for (int row = 0; row < 2; row++) {
         for (int k = 0; k < 4; k++)
            gh_button(fb, h, sx + (k * (sw + gap)), y, sw, bh, sc, lab[row][k],
                      UI_TEXT, MA_GLUHIST_STEP, ix[row][k]);
         y += bh + (6 * sc);
      }
      /* THE ENDS: one button the height of both rows each side, |< to the
       * window that starts with the first reading's day, >| to the one that
       * ends with today, both keeping the span. */
      const int ey = y - (2 * (bh + (6 * sc)));
      const int eh = (2 * bh) + (6 * sc);

      const struct {
         int ex;
         const uint8_t *ic;
         int step;
      } end[2] = {
          {x,                     icon_pg_first, GH_FIRST},
          {sx + (4 * (sw + gap)), icon_pg_last,  GH_TODAY}
      };

      for (int k = 0; k < 2; k++) {
         draw_frame(px, fb, end[k].ex, ey, ew, eh, UI_MUTED);
         draw_icon(px, fb, end[k].ex + ((ew - (5 * tsc)) / 2),
                   ey + ((eh - (7 * tsc)) / 2), tsc, end[k].ic, UI_TEXT);
         add_hit_ix(h, ui_rect(end[k].ex, ey, ew, eh), MA_GLUHIST_STEP,
                    end[k].step);
      }
   }
   y += 4 * sc;

   /* THE SPAN ROW, the main screen's tab row: the five spans in the normal
    * font right above the plot, the chosen one lit. While a finger scrubs
    * the plot the row shows the readout of the point under it, as the main
    * screen's does. */
   const int plot_x = 2 * sc;
   const int plot_w = fb->width - (4 * sc);
   const int rowh   = 14 * sc;
   const int scrub  = m->plot.scrub >= 0 && m->plot.scrub < m->plot.nhist;
   if (scrub) {
      ui_glu_scrub_row(fb, m, plot_x + (2 * sc), y, plot_w - (4 * sc), sc);
   } else {
      const int colw = plot_w / HSPAN_N;
      const int laby = y + ((rowh - (7 * sc)) / 2);
      for (int k = 0; k < HSPAN_N; k++) {
         const int lw = str_len(g_span_lbl[k]) * 6 * sc;
         const int tx = plot_x + (k * colw);
         draw_str(px, fb, tx + ((colw - lw) / 2), laby, sc, g_span_lbl[k],
                  k == m->gh.span ? UI_TEXT : UI_MUTED);
         add_hit_ix(h, ui_rect(tx, y, colw, rowh), MA_GLUHIST_SPAN, k);
      }
   }
   y += rowh;

   /* WHAT IS LEFT, shared between the plot and the profile, after the bar
    * and its figures (2 + 4 + 7 sc) and the gaps around them. */
   const int bottom = fb->height - (fb->height / 24);
   const int below  = (6 * sc) + (2 * sc) + (4 * sc) + (7 * sc) + (8 * sc);
   int avail        = bottom - y - below;
   if (avail < 40 * sc)
      avail = 40 * sc;
   const int ph = (avail * 55) / 100;
   const int ah = avail - ph;

   /* The plot scrubs as the main screen's: the shell resolves a press in
    * this rectangle to a point with the configuration it was drawn with. */
   h->plot = ui_glu_plot(fb, m, sc, (struct plot_rect){plot_x, y, plot_w, ph},
                         scrub ? m->plot.scrub : -1);
   add_hit(h, ui_rect(plot_x, y, plot_w, ph), ACT_SCRUB, 0);
   y += ph + (6 * sc);
   y = range_rows(fb, m, plot_x, plot_w, y, sc);
   y += 8 * sc;
   if (y + ah <= fb->height)
      agp_plot(fb, m, (struct plot_rect){plot_x, y, plot_w, ah}, sc);
}
