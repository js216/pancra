// SPDX-License-Identifier: GPL-3.0
// uilog.c --- The dose and weight logs, and their forms (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "font.h"
#include "insrow.h"  /* INS_*: what a dose row can say */
#include "insulin.h" /* struct ins_rec + INS_* for the INSULIN LOG table */
#include "ndk.h"
#include "style.h" /* the colour roles: UI_TEXT, UI_MUTED, ... */
#include "ui.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include "util.h"   /* str_snapshot */
#include "weight.h" /* wt_unit_name / wt_to_tenths: the weight rows */
#include <stdint.h>
#include <stdio.h> /* snprintf */

void render_insulin(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, m->ins.ins_edit ? "EDIT INSULIN" : "LOG INSULIN",
            UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* X discards -- nothing is written before an explicit CONFIRM. */
   /* 2*lh - 2*sc, not 2*lh: value_row's target starts at its y - 4*sc, so a
    * full close band reaches 4*sc into the TYPE row below it -- the same
    * overlap render_weight was already fixed for, and this form is the one
    * where the row underneath changes a logged dose. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc)),
              MA_INS_DISCARD, 0);
   y += 2 * lh;

   /* TYPE is editable in-form (the ADD menu's FAST/SLOW buttons only
    * pre-populate it); FAST shows in the log table's blue, at the same
    * large value size as the other editable fields. */
   y = value_row(fb, h, y, sc, "TYPE", m->ins.ins_type == 1 ? "FAST" : "SLOW",
                 m->ins.ins_type == 1 ? 0xFFFFAA66 : UI_TEXT, MA_INS_TYPE, 0);
   y += lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; the form splits it into YEAR,
    * MM-DD and HH:MM fields. (An older split assumed "MM-DD HH:MM" -- one
    * format behind fmt_date -- so DATE showed a truncated year and TIME
    * showed a slice of the date.) */
   char dt[20];
   fmt_date(m->ins.ins_t, m->tz_off, dt, sizeof dt);
   char yearp[8];
   char datep[8];
   char timep[8];
   str_snapshot(yearp, sizeof yearp, dt);
   if (str_len(yearp) > 4)
      yearp[4] = 0; /* "YYYY" */
   str_snapshot(datep, sizeof datep, (str_len(dt) > 5) ? dt + 5 : "");
   if (str_len(datep) > 5)
      datep[5] = 0; /* "MM-DD" */
   str_snapshot(timep, sizeof timep, (str_len(dt) > 11) ? dt + 11 : "");
   char val[20];
   (void)snprintf(val, sizeof val, "%d U", m->ins.ins_units);
   y = value_row(fb, h, y, sc, "UNITS", val, UI_TEXT, MA_INS_EDIT, 0);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, UI_TEXT, MA_INS_EDIT, 2);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, UI_TEXT, MA_INS_EDIT, 1);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, UI_TEXT, MA_INS_EDIT, 3);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide
    * rule, so reach-and-tap muscle memory can never commit by accident.
    * Editing adds DELETE (red) between the two. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_INS_DISCARD, 0);
   y += 2 * lh;
   if (m->ins.ins_edit) {
      y = menu_button(fb, h, x, y, bw, sc, "DELETE", UI_ALERT, MA_INS_DELETE,
                      0);
      y += 2 * lh;
   }
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_GO, MA_INS_CONFIRM, 0);
}

/* ---- delete-dose confirmation ----
 * The EDIT form's DELETE button lands here first; only the red DELETE on
 * this screen (MA_INSDEL_YES) actually removes the dose from the log. ---- */
void render_insdel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE DOSE?", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT); /* close = cancel */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_INSDEL_NO, 0);
   y += 2 * lh;

   /* the dose about to be deleted, so a mis-tap from the log is caught */
   char dt[20];
   fmt_date(m->ins.ins_t, m->tz_off, dt, sizeof dt);
   char line[40];
   (void)snprintf(line, sizeof line, "%d U %s", m->ins.ins_units,
                  m->ins.ins_type == 1 ? "FAST" : "SLOW");
   draw_str(px, fb, x, y, sc, line, UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, dt, UI_TEXT);
   y += 2 * lh;
   static const char *const note[] = {
       "REMOVES THIS DOSE FROM THE",
       "INSULIN LOG. THIS CANNOT",
       "BE UNDONE.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], UI_MUTED);
      y += lh;
   }
   y += 2 * lh;

   /* CANCEL (safe, white) on top, DELETE (RED) well below -- the same
    * discipline as SCR_FORGET, so reach-and-tap muscle memory can never
    * delete by accident. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_INSDEL_NO, 0);
   y += 3 * lh; /* wide gap so DELETE is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "DELETE", UI_ALERT, MA_INSDEL_YES, 0);
}

/* ---- INSULIN LOG: the dose tail as a when/type/units table, newest
 * first, paginated like OLD DEVICES so any length stays usable. ---- */

void render_inslog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "INSULIN LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_INSLOG_BACK,
              0);
   y += 3 * lh;

   if (m->ins.ins_nlog <= 0) {
      draw_str(px, fb, x, y, sc, "No doses logged yet.", UI_MUTED);
      return;
   }
   draw_str(px, fb, x, y, sc, "TIME              TYPE  UNITS", UI_MUTED);
   y += lh;

   /* Rows that fit between the header and a reserved bottom nav line. */
   /* CAP THE ROWS BY THE HIT BUDGET, not just by height.
    *
    * Each row records a touch target, and add_hit silently DROPS everything
    * past UI_MAX_HITS -- so a window tall enough for more rows than the
    * budget allows drew them all and left the trailing ones, including the
    * next-page arrow, dead to touch. Reproduced at 480x1920 and 540x2340
    * with a full log. Reserve UI_LOG_FIXED for this screen's own controls
    * (title/close plus the pagination pair) and let the pages absorb the
    * rest: fewer rows per page is a visible, honest consequence; an
    * untappable control is not. */
   int avail = fb->height - y - (2 * lh);
   int per   = avail / lh;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (m->ins.ins_nlog + per - 1) / per;
   int page   = m->ins.inslog_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < m->ins.ins_nlog; r++) {
      /* the tail is oldest-first; the table shows newest first */
      int ti                  = m->ins.ins_nlog - 1 - r;
      const struct ins_rec *d = &m->ins.ins_log[ti];
      char when[20];
      char row[40];
      fmt_date(d->t, m->tz_off, when, sizeof when);
      (void)snprintf(row, sizeof row, "%s  %s %4d", when,
                     d->type == INS_FAST ? "FAST" : "SLOW", d->units);
      /* FAST doses in a soft blue, so the two types separate at a glance
       * (0xAABBGGRR: R=0x66 G=0xAA B=0xFF). */
      draw_str(px, fb, x, y, sc, row,
               d->type == INS_FAST ? 0xFFFFAA66 : UI_TEXT_DIM);
      /* The pencil is the affordance; the WHOLE row is the target (it
       * opens this dose in the EDIT INSULIN form). Centre the pencil in
       * the free column right of UNITS -- glued to the screen edge it
       * read as a tiny edge-of-screen button. */
      {
         int te = x + (29 * 6 * sc); /* right edge of the UNITS column */
         int ix = te + (((rx - te) - (5 * sc)) / 2);
         if (ix < te)
            ix = rx - (6 * sc); /* narrow screen: fall back to the edge */
         draw_icon(px, fb, ix, y, sc, icon_pencil, UI_MUTED);
      }
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_INSLOG_EDIT,
                 ti);
      y += lh;
   }

   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      /* Height `lh + 7*sc`, not `2*lh`: from `navy - 3*sc` a 2*lh box ends at
       * `height + 9*sc`, i.e. always 9*sc BELOW the buffer. The arrows drew
       * correctly and the top of each box was tappable, so it worked by
       * accident -- but an out-of-bounds target is exactly what the layout
       * gate forbids everywhere else, and the bottom strip of the finger
       * target simply did not exist. This ends flush with the bottom edge. */
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(0, navy - (3 * sc), fb->width / 3, lh + (7 * sc)),
                    MA_INSLOG_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               UI_MUTED);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), navy, tsc, ">", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(fb->width - (fb->width / 3), navy - (3 * sc),
                            fb->width / 3, lh + (7 * sc)),
                    MA_INSLOG_NEXT, 0);
      }
   }
}

/* ---- WEIGHT: the entry form and the log table ----
 *
 * Deliberately the insulin form's shape and helpers (value_row, the same
 * keypad modes for date/time/year), because they are the same act: a number
 * the user typed, filed against an instant they can correct. Nothing here is
 * written before an explicit CONFIRM. */

/* Render a stored weight into the DISPLAY unit, e.g. "154.2 LB". Grams are
 * what the file holds; this is the only place the preference is applied. */
static void fmt_weight(long g, int units, char *out, int n)
{
   int t = wt_to_tenths(g, units);
   (void)snprintf(out, n, "%d.%d %s", t / 10, t % 10, wt_unit_name(units));
}

/* NARROWED ON PURPOSE. Taking the whole `struct screen`, nothing in the
 * signature says that the weight form reads the weight model, the display
 * unit and the zone -- and nothing stops it quietly growing a dependency on
 * the sensor registry or the alarm thresholds. The parameters ARE the
 * contract. */
void render_weight(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                   const struct ui_prefs *prefs, long tz_off, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, wt->wt_edit ? "EDIT WEIGHT" : "LOG WEIGHT",
            UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* X discards -- nothing is written before an explicit CONFIRM. */
   /* 2*lh - 2*sc, not 2*lh: value_row's target starts at its y - 4*sc, so a
    * full 2*lh close band reached 4*sc into the WEIGHT row below it. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc)),
              MA_WT_DISCARD, 0);
   y += 2 * lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; split it the way the insulin form
    * does, into YEAR / MM-DD / HH:MM. */
   char dt[20];
   fmt_date(wt->wt_t, tz_off, dt, sizeof dt);
   char yearp[8];
   char datep[8];
   char timep[8];
   str_snapshot(yearp, sizeof yearp, dt);
   if (str_len(yearp) > 4)
      yearp[4] = 0;
   str_snapshot(datep, sizeof datep, (str_len(dt) > 5) ? dt + 5 : "");
   if (str_len(datep) > 5)
      datep[5] = 0;
   str_snapshot(timep, sizeof timep, (str_len(dt) > 11) ? dt + 11 : "");

   char val[20];
   (void)snprintf(val, sizeof val, "%d.%d %s", wt->wt_tenths / 10,
                  wt->wt_tenths % 10, wt_unit_name(prefs->wunits));
   y = value_row(fb, h, y, sc, "WEIGHT", val, UI_TEXT, MA_WT_EDIT, 0);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, UI_TEXT, MA_WT_EDIT, 2);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, UI_TEXT, MA_WT_EDIT, 1);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, UI_TEXT, MA_WT_EDIT, 3);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide
    * rule (see the insulin form). Editing adds DELETE (red) between the
    * two, mirroring EDIT INSULIN; it only opens a confirmation, it never
    * deletes on the tap itself. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_WT_DISCARD, 0);
   y += (3 * lh) / 2;
   if (wt->wt_edit) {
      y = menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_WT_DELETE,
                      0);
      y += (3 * lh) / 2;
   }
   (void)menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_OK, MA_WT_CONFIRM, 0);
}

/* Confirm deleting one weight entry. Mirrors the insulin one: the value being
 * destroyed is spelled out, CANCEL is first and DELETE is below it. */
void render_wtdel(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                  const struct ui_prefs *prefs, long tz_off, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 20);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE?", UI_DANGER);
   y += 3 * lh;
   char wv[16];
   char when[20];
   /* The ORIGINAL entry, not the form's current values. Editing the weight
    * and then tapping DELETE showed the edited number here while
    * weight_delete removed the row that was on disk -- a confirmation naming
    * a different record than the one it destroys is worse than none. */
   fmt_weight(wt->wt_orig_g, prefs->wunits, wv, sizeof wv);
   fmt_date(wt->wt_orig_t, tz_off, when, sizeof when);
   draw_str(px, fb, x, y, sc, wv, UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, when, UI_TEXT_DIM);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "This cannot be undone.", UI_MUTED);
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_WTDEL_NO, 0);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_WTDEL_YES, 0);
}

const int ui_wt_days[UI_WT_TABS] = {30, 90, 180, 365, 0}; /* 0 = everything */
static const char *const ui_wt_tab_lbl[UI_WT_TABS] = {"1M", "3M", "6M", "1Y",
                                                      "ALL"};

/* The window a span selects: the time and weight ranges the plot maps onto
 * its rectangle. ONE definition, used by the renderer AND the hit test --
 * computing it twice is how a scrub cursor ends up landing next to the point
 * the finger actually picked. */
struct wt_win {
   long tmin, tmax;
   long lo, hi;
   int n;
};

static void wt_window(const struct ui_wtview *wt, long now, long from,
                      struct wt_win *w)
{
   w->n = 0;
   for (int i = 0; i < wt->nwt; i++) {
      /* BOTH ends, matching the draw loop and the scrub picker. Only the
       * lower bound was applied here, so a mistyped year -- the entry form
       * accepts any date -- was excluded from the plot but still counted in
       * lo/hi, scaling the y axis to a weight that is never drawn and
       * flattening the real trace against the bottom of the frame. */
      if (wt->wt[i].t < from || wt->wt[i].t > now)
         continue;
      if (!w->n || wt->wt[i].g < w->lo)
         w->lo = wt->wt[i].g;
      if (!w->n || wt->wt[i].g > w->hi)
         w->hi = wt->wt[i].g;
      if (!w->n)
         w->tmin = wt->wt[i].t;
      w->n++;
   }
   if (!w->n)
      return;
   /* THE X AXIS IS THE SELECTED SPAN, not the data's own extent.
    *
    * Taking tmin/tmax from the points made "1Y" with three months of data
    * draw those three months stretched across the full width -- identical to
    * the "3M" view, with only the date ticks to tell them apart, so the tab
    * appeared to do nothing. The right edge is now, the left edge is the span
    * the tab names, and a short history simply leaves the left of the plot
    * empty, which is the truth. ALL has no fixed start, so it spans from the
    * first entry to now. */
   if (from > 0)
      w->tmin = from;
   w->tmax = now;
   /* A flat or single-point window has no range to scale to; pad it so the
    * trace lands mid-plot rather than dividing by zero. */
   if (w->hi - w->lo < 200) {
      long mid = (w->hi + w->lo) / 2;
      w->lo    = mid - 100;
      w->hi    = mid + 100;
   }
   if (w->tmax - w->tmin < 60)
      w->tmax = w->tmin + 60;
}

#define WT_PAD 6 /* inset in sc units, so points near the edge stay whole */

static int wt_px(const struct wt_win *w, long t, int px0, int pw, int pad)
{
   return px0 + pad +
          (int)(((t - w->tmin) * (long)(pw - (2 * pad))) / (w->tmax - w->tmin));
}

/* Separate TOP and BOTTOM insets, not one pad.
 *
 * The y bounds are printed inside the plot -- upper at the top-left, lower
 * above the date ticks at the bottom-left -- so the data band has to stop
 * short of both, or a point lands on the text that names it. Insetting the
 * band is the same thing as expanding the y range, and it is exact: no point
 * can enter a reserved strip, whatever the data does. */
static int wt_py(const struct wt_win *w, long g, int py0, int ph, int pad_t,
                 int pad_b)
{
   return py0 + pad_t +
          (int)(((w->hi - g) * (long)(ph - pad_t - pad_b)) / (w->hi - w->lo));
}

/* The start of the span the tabs select. Takes the weight model rather than
 * the frame, so the weight renderers can ask it without holding a screen. */
static long wt_from_of(const struct ui_wtview *wt, long now)
{
   int tab = wt->wt_tab;
   if (tab < 0 || tab >= UI_WT_TABS)
      tab = 0;
   return ui_wt_days[tab] > 0 ? now - ((long)ui_wt_days[tab] * 86400L) : 0;
}

int ui_wt_hit(const struct screen *m, int plot_x, int plot_w, int sc, int x)
{
   struct wt_win w;
   wt_window(&m->wt, m->now, ui_wt_from(m), &w);
   if (!w.n)
      return -1;
   int pad  = WT_PAD * sc;
   int best = -1;
   long bd  = 0;
   long lo  = ui_wt_from(m);
   for (int i = 0; i < m->wt.nwt; i++) {
      if (m->wt.wt[i].t < lo || m->wt.wt[i].t > w.tmax)
         continue; /* same window the renderer uses, or the pick misses */
      int cx = wt_px(&w, m->wt.wt[i].t, plot_x, plot_w, pad);
      long d = cx - x;
      if (d < 0)
         d = -d;
      if (best < 0 || d < bd) {
         bd   = d;
         best = i;
      }
   }
   return best;
}

/* The weight trend, drawn into [px0,px0+pw) x [py0,py0+ph).
 *
 * Its own plot, not plot.c's: that one is a glucose instrument -- fixed
 * mg/dL scale, in-range band, per-sensor markers. A weight trend needs none
 * of it and needs the one thing it cannot do, an AUTOSCALED y axis. A body
 * weight moves a few percent over a year, so a fixed axis would draw every
 * point as one flat line. Both axes are labelled with their real extremes,
 * because an autoscaled plot that does not state its range is the one that
 * misleads. */
static void wt_plot(uint32_t *px, const struct ANativeWindow_Buffer *fb,
                    const struct ui_wtview *wt, const struct ui_prefs *prefs,
                    long now, long tz_off, int px0, int py0, int pw, int ph,
                    int sc, long from)
{
   draw_frame(px, fb, px0, py0, pw, ph, 0xFF444444);
   struct wt_win w;
   wt_window(wt, now, from, &w);
   if (!w.n) {
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "no data in range",
               UI_MUTED);
      return;
   }
   int pad = WT_PAD * sc;
   /* Reserved strips: the upper bound's line at the top, and the lower
    * bound's line PLUS the date-tick line at the bottom. */
   /* + half a text height of breathing room, so the extreme datapoint clears
    * the bound that names it rather than just touching it. */
   int half  = (7 * sc) / 2;
   int pad_t = (13 * sc) + half;
   int pad_b = (22 * sc) + half;
   if (pad_t + pad_b > (ph * 2) / 3) { /* a very short plot: share it out */
      pad_t = ph / 6;
      pad_b = ph / 4;
   }

   /* LIGHT GRID. Dark enough to sit behind the trace rather than compete with
    * it: the trace is the data, the grid is only a ruler. Four horizontal
    * divisions, and vertical lines on the same columns the date labels use so
    * a label always names a line rather than floating between two. */
   const uint32_t grid = 0xFF2A2A2A;
   for (int i = 1; i < 4; i++) {
      int gy = py0 + pad_t + (((ph - pad_t - pad_b) * i) / 4);
      fill_rect(px, fb, px0 + 1, gy, pw - 2, 1, grid);
   }
   int nticks = 4; /* 3 interior + the right edge; see the label loop */
   for (int i = 1; i <= nticks; i++) {
      int gx = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
      fill_rect(px, fb, gx, py0 + 1, 1, ph - 2, grid);
   }

   /* THE Y BOUNDS, upper at the top-left and lower at the bottom-left, each
    * against the axis end it names -- not one "lo-hi" string in the top-right
    * corner, which states the range but not which end is which way up. Always
    * drawn: the reserved strips above keep the trace off them, so unlike a
    * corner label there is nothing to suppress while scrubbing. */
   {
      char slab[16];
      fmt_weight(w.hi, prefs->wunits, slab, sizeof slab);
      draw_str(px, fb, px0 + (4 * sc), py0 + (3 * sc), sc, slab, UI_MUTED);
      fmt_weight(w.lo, prefs->wunits, slab, sizeof slab);
      draw_str(px, fb, px0 + (4 * sc), py0 + ph - (19 * sc), sc, slab,
               UI_MUTED);
   }

   /* DATE TICKS on the vertical grid lines. Without them the x axis is
    * unlabelled and "1Y" could be any year. MM-DD only -- the span already
    * says how far back this is, and a full date at this size would collide
    * with its neighbour. */
   for (int i = 1; i <= nticks; i++) {
      int gx  = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
      long tt = w.tmin + (((w.tmax - w.tmin) * i) / (nticks + 1));
      char dt[20];
      char md[8];
      fmt_date(tt, tz_off, dt, sizeof dt);
      str_snapshot(md, sizeof md, (str_len(dt) > 5) ? dt + 5 : "");
      if (str_len(md) > 5)
         md[5] = 0; /* "MM-DD" */
      int lw = str_len(md) * 6 * sc;
      draw_str(px, fb, gx - (lw / 2), py0 + ph - (9 * sc), sc, md, 0xFF777777);
   }

   int prevx = 0;
   int prevy = 0;
   int have  = 0;
   for (int i = 0; i < wt->nwt; i++) {
      /* Both ends. A point AFTER the window -- the entry form allows any date,
       * so a mistyped year lands one decades ahead -- mapped past the right
       * edge and drew outside the frame, over the tabs above it. */
      if (wt->wt[i].t < from || wt->wt[i].t > w.tmax)
         continue;
      int cx = wt_px(&w, wt->wt[i].t, px0, pw, pad);
      int cy = wt_py(&w, wt->wt[i].g, py0, ph, pad_t, pad_b);
      /* Join consecutive points: a weight trend is read as a line, and dots
       * alone at one a day over a year are unreadable. 0xAABBGGRR, so this is
       * the soft blue the insulin log already uses. */
      if (have) {
         int dx = cx - prevx;
         int dy = cy - prevy;
         int ax = dx < 0 ? -dx : dx;
         int ay = dy < 0 ? -dy : dy;
         int st = ax > ay ? ax : ay;
         for (int k = 1; k <= st && st > 0; k++)
            fill_rect(px, fb, prevx + ((dx * k) / st), prevy + ((dy * k) / st),
                      sc, sc, 0xFFFFAA66);
      }
      /* White, like the glucose trace. The SCRUBBED point is redrawn below in
       * UI_HILITE grey -- the same "white normally, grey when picked" pair
       * plot_render uses, so both plots read the same way. */
      fill_rect(px, fb, cx - (2 * sc), cy - (2 * sc), 4 * sc, 4 * sc, UI_TEXT);
      prevx = cx;
      prevy = cy;
      have  = 1;
   }

   /* SCRUB CURSOR: a full-height rule through the picked point and its value
    * spelled out, so the number under the finger is readable rather than
    * estimated off the axis. */
   if (wt->wt_scrub >= 0 && wt->wt_scrub < wt->nwt &&
       wt->wt[wt->wt_scrub].t >= from) {
      const struct wt_rec *p = &wt->wt[wt->wt_scrub];
      int cx                 = wt_px(&w, p->t, px0, pw, pad);
      int cy                 = wt_py(&w, p->g, py0, ph, pad_t, pad_b);
      fill_rect(px, fb, cx, py0 + 1, 1, ph - 2, 0xFF666666);
      fill_rect(px, fb, cx - (3 * sc), cy - (3 * sc), 6 * sc, 6 * sc,
                UI_HILITE);
   }
}

/* NARROWED, and narrowing it answered a question. The table was assumed to
 * need the sensor registry (to draw each entry with the marker of the device
 * that recorded it); making the dependency explicit showed the compiler
 * disagreeing -- it reads no registry at all. A weight table depends on the
 * weight model, the display unit, the clock and the zone, and now says so. */
void render_wtlog(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                  const struct ui_prefs *prefs, long now, long tz_off,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "WEIGHT LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_WTLOG_BACK, 0);
   y += 3 * lh;

   if (wt->nwt <= 0) {
      draw_str(px, fb, x, y, sc, "No weights logged yet.", UI_MUTED);
      return;
   }

   /* THE SCREEN IS SPLIT: table above, trend below. The plot gets the bottom
    * ~40% and everything else is laid out against what is left, so the table
    * cannot grow into the plot on a tall screen nor the plot squeeze the
    * table to nothing on a short one. */
   /* RESERVE THE SYSTEM GESTURE BAR. The activity draws edge to edge, so the
    * phone's own navigation pill sits ON TOP of the bottom of this surface --
    * the plot's bottom edge and its date labels were rendering underneath it.
    * Everything else in the app happens to stop short of the bottom; this
    * screen is the first to reach it, so it is the first to collide. */
   int sysbar   = fb->height / 24;
   int plot_h   = (fb->height * 2) / 5;
   int tabs_h   = 2 * lh; /* the span tabs */
   int plot_top = fb->height - plot_h - sysbar;
   int tabs_y   = plot_top - tabs_h;
   /* 6*sc, not 2: the nav box runs from nav_y - 3*sc for lh + 7*sc, i.e. to
    * nav_y + lh + 4*sc, so a 2*sc gap left its bottom 2*sc inside the tab row
    * -- and ui_hit_idx scans backwards, so the TAB won and the bottom sliver
    * of the "next page" arrow silently changed the plot's span instead. */
   /* The blank line goes BELOW the pagination, not above it: the page counter
    * belongs to the TABLE it pages, and a gap between them grouped it with
    * the plot instead -- the one thing it has nothing to do with. */
   int nav_y = tabs_y - (2 * lh) - (6 * sc);
   /* A blank line between the table and the pagination row, so the two halves
    * of this screen read as two things rather than one crowded column. */

   draw_str(px, fb, x, y, sc, "TIME              WEIGHT", UI_MUTED);
   y += lh;

   /* CAP THE ROWS BY THE HIT BUDGET, not just by height.
    *
    * Each row records a touch target, and add_hit silently DROPS everything
    * past UI_MAX_HITS -- so a window tall enough for more rows than the
    * budget allows drew them all and left the trailing ones, including the
    * next-page arrow, dead to touch. Reproduced at 480x1920 and 540x2340
    * with a full log. Reserve UI_LOG_FIXED for this screen's own controls
    * (title/close plus the pagination pair) and let the pages absorb the
    * rest: fewer rows per page is a visible, honest consequence; an
    * untappable control is not. */
   int avail = nav_y - y; /* rows run right down to the pagination */
   int per   = (avail > 0) ? avail / lh : 1;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (wt->nwt + per - 1) / per;
   int page   = wt->wt_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < wt->nwt; r++) {
      int ti                 = wt->nwt - 1 - r; /* tail is oldest-first */
      const struct wt_rec *w = &wt->wt[ti];
      char when[20];
      char wv[16];
      char row[48];
      fmt_date(w->t, tz_off, when, sizeof when);
      fmt_weight(w->g, prefs->wunits, wv, sizeof wv);
      (void)snprintf(row, sizeof row, "%s  %s", when, wv);
      draw_str(px, fb, x, y, sc, row, UI_TEXT_DIM);
      /* The pencil is the affordance; the WHOLE row is the target, opening
       * this entry in the EDIT WEIGHT form (the insulin log's pattern). */
      {
         int te = x + (24 * 6 * sc);
         int ix = te + (((rx - te) - (5 * sc)) / 2);
         if (ix < te)
            ix = rx - (6 * sc);
         draw_icon(px, fb, ix, y, sc, icon_pencil, UI_MUTED);
      }
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_WTLOG_EDIT, ti);
      y += lh;
   }

   if (npages > 1) {
      if (page > 0) {
         draw_str(px, fb, x, nav_y, tsc, "<", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(0, nav_y - (3 * sc), fb->width / 3, lh + (7 * sc)),
                    MA_WTLOG_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, nav_y, sc, pg,
               UI_MUTED);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), nav_y, tsc, ">", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(fb->width - (fb->width / 3), nav_y - (3 * sc),
                            fb->width / 3, lh + (7 * sc)),
                    MA_WTLOG_NEXT, 0);
      }
   }

   /* Span tabs -- OR the scrub readout, exactly as the glucose plot does it:
    * while a finger is down the tab row becomes the value under it, and the
    * tabs come back the moment it lifts. The readout needs a fixed, roomy
    * home, and drawing it inside the plot put it over the trace it describes.
    */
   int tab = wt->wt_tab;
   if (tab < 0 || tab >= UI_WT_TABS)
      tab = 0;
   int colw = (fb->width - (2 * x)) / UI_WT_TABS;
   int trow = 14 * sc; /* render_glucose's tab row height */
   int laby = plot_top - trow + ((trow - (7 * sc)) / 2);
   if (wt->wt_scrub >= 0 && wt->wt_scrub < wt->nwt) {
      const struct wt_rec *p = &wt->wt[wt->wt_scrub];
      char wv[16];
      char when[24];
      char line[48];
      fmt_weight(p->g, prefs->wunits, wv, sizeof wv);
      fmt_date(p->t, tz_off, when, sizeof when);
      (void)snprintf(line, sizeof line, "%s   %s", when, wv);
      int tsc2 = 2 * sc;
      while (tsc2 > sc && str_len(line) * 6 * tsc2 > fb->width - (4 * sc))
         tsc2--;
      int lw = str_len(line) * 6 * tsc2;
      /* White, and the point marked in UI_HILITE grey -- the same pair the
       * glucose plot uses. Green means "on / enabled" everywhere else in this
       * app; a readout is neither. Top-aligned in the tab row, as there. */
      draw_str(px, fb, (fb->width - lw) / 2, plot_top - trow, tsc2, line,
               UI_TEXT);
      /* No tab targets while scrubbing: the row is not showing tabs, and a
       * target that does not match what is drawn is how a drag ends up
       * changing the span it was only trying to read. */
   } else {
      for (int i = 0; i < UI_WT_TABS; i++) {
         int lw   = str_len(ui_wt_tab_lbl[i]) * 6 * sc;
         int tabx = x + (i * colw);
         /* Same construction as render_glucose: the labels sit in a 14*sc row
          * ending at the plot's top edge, vertically centred in it, so the
          * gap between a tab and the plot is identical on both screens. The
          * TARGET still spans the whole band above, which is empty. */
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, ui_wt_tab_lbl[i],
                  i == tab ? UI_TEXT : UI_MUTED);
         add_hit_ix(h, ui_rect(tabx, tabs_y, colw, tabs_h), MA_WTTAB, i);
      }
   }

   int pw = fb->width - (2 * x);
   wt_plot(px, fb, wt, prefs, now, tz_off, x, plot_top, pw, plot_h, sc,
           wt_from_of(wt, now));
   /* The whole plot scrubs; the shell resolves the point via ui_wt_hit. */
   /* arg carries sc: the shell needs the SAME scale the plot was drawn at to
    * map a finger x back to a point, and re-deriving it there would be a
    * second copy of the layout that can drift. */
   add_hit(h, ui_rect(x, plot_top, pw, plot_h), ACT_SCRUB, sc);
}

/* ---- OLD DEVICES: DISCONNECTED devices. Each keeps its whole slot, so a row
 * opens the SAME per-device menu as a live one (MA_SENSOR + slot index). The
 * list PAGINATES: if there are more than fit, a "< PAGE i/n >" row at the
 * bottom navigates, so any number of old devices is usable. ---- */

long ui_wt_from(const struct screen *m)
{
   return wt_from_of(&m->wt, m->now);
}
