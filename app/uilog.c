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
                 m->ins.ins_type == 1 ? UI_MARK_FAST : UI_TEXT, MA_INS_TYPE, 0);
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
   char iu[16];
   (void)ins_units_str(m->ins.ins_milli, iu, sizeof iu);
   (void)snprintf(val, sizeof val, "%s U", iu);
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
   char iu[16];
   (void)ins_units_str(m->ins.ins_milli, iu, sizeof iu);
   (void)snprintf(line, sizeof line, "%s U %s", iu,
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
   /* THE SCREEN IS SPLIT SEVENTY / THIRTY IN THE TABLE'S FAVOUR, laid out
    * from the bottom: the plot takes its share and the table gets the rest,
    * and the system gesture bar is reserved because this screen reaches the
    * bottom edge.
    *
    * OF WHAT IS BELOW THE HEADER, not of the whole screen. Two fifths of the
    * screen height left the table with fewer rows than the plot had pixels,
    * on a screen whose reason for existing is the list of doses -- the plot
    * is the summary beside it. Measuring from `y` makes the ratio mean what
    * it says whatever the title and column header cost. */
   int sysbar   = fb->height / 24;
   int plot_h   = ((fb->height - y - sysbar) * 3) / 10;
   int tabs_h   = 2 * lh;
   int plot_top = fb->height - plot_h - sysbar;
   int tabs_y   = plot_top - tabs_h;
   int nav_y    = tabs_y - (2 * lh) - (6 * sc);

   int avail = nav_y - y;
   int per   = (avail > 0) ? avail / lh : 1;
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
      char iu[16];
      (void)ins_units_str(d->milli, iu, sizeof iu);
      /* RIGHT-ALIGNED IN THE COLUMN the header names, whatever its width:
       * "0.5" and "20" are different lengths, and a left-aligned dose column
       * makes a half-unit look like five. */
      (void)snprintf(row, sizeof row, "%s  %s %4s", when,
                     d->type == INS_FAST ? "FAST" : "SLOW", iu);
      /* FAST doses in a soft blue, so the two types separate at a glance
       * (0xAABBGGRR: R=0x66 G=0xAA B=0xFF). */
      draw_str(px, fb, x, y, sc, row,
               d->type == INS_FAST ? UI_MARK_FAST : UI_TEXT_DIM);
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

   pager_row(fb, h, x, rx, nav_y, sc, lh, page, npages, MA_INSLOG_PAGE);

   /* ONE DOT PER DOSE, coloured by type -- see the log plot's own note. */
   int tab = m->ins.inslog_tab;
   if (tab < 0 || tab >= UI_DAY_TABS)
      tab = 0;
   struct log_pt pts[UI_LOG_PTS];
   long from = 0;
   int npt   = ins_points(m, pts, UI_LOG_PTS, &from);

   /* Span tabs -- OR the scrub readout, the same swap the weight trend makes.
    */
   int colw  = (fb->width - (2 * x)) / UI_DAY_TABS;
   int trow  = 14 * sc;
   int laby  = plot_top - trow + ((trow - (7 * sc)) / 2);
   int scrub = m->log_scrub;
   if (scrub >= 0 && scrub < npt && scrub < m->ins.ins_nlog) {
      const struct ins_rec *d = &m->ins.ins_log[scrub];
      char when[24];
      char line[48];
      fmt_date(d->t, m->tz_off, when, sizeof when);
      /* THE WHOLE INSTANT, not just the date: a dot is one dose, and which
       * doses fell when is the pattern this plot exists to show. */
      char iu[16];
      (void)ins_units_str(d->milli, iu, sizeof iu);
      (void)snprintf(line, sizeof line, "%s  %s %s U", when,
                     d->type == INS_FAST ? "FAST" : "SLOW", iu);
      int tsc2 = 2 * sc;
      while (tsc2 > sc && str_len(line) * 6 * tsc2 > fb->width - (4 * sc))
         tsc2--;
      int lw = str_len(line) * 6 * tsc2;
      draw_str(px, fb, (fb->width - lw) / 2, plot_top - trow, tsc2, line,
               UI_TEXT);
   } else {
      for (int i = 0; i < UI_DAY_TABS; i++) {
         int lw   = str_len(ui_day_tab_lbl[i]) * 6 * sc;
         int tabx = x + (i * colw);
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, ui_day_tab_lbl[i],
                  i == tab ? UI_TEXT : UI_MUTED);
         add_hit_ix(h, ui_rect(tabx, tabs_y, colw, tabs_h), MA_INSTAB, i);
      }
   }

   int pw = fb->width - (2 * x);
   log_plot(px, fb, pts, npt, from, m->now, x, plot_top, pw, plot_h, sc,
            m->tz_off, scrub, "U", 3, ui_ins_col, UI_INS_SERIES);
   add_hit(h, ui_rect(x, plot_top, pw, plot_h), ACT_SCRUB, sc);
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
                    int sc, long from, int scrub)
{
   draw_frame(px, fb, px0, py0, pw, ph, UI_LOG_FRAME);
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
   const uint32_t grid = UI_LOG_GRID;
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
      draw_str(px, fb, gx - (lw / 2), py0 + ph - (9 * sc), sc, md, UI_DISCLAIM);
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
                      sc, sc, UI_MARK_FAST);
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
   if (scrub >= 0 && scrub < wt->nwt && wt->wt[scrub].t >= from) {
      const struct wt_rec *p = &wt->wt[scrub];
      int cx                 = wt_px(&w, p->t, px0, pw, pad);
      int cy                 = wt_py(&w, p->g, py0, ph, pad_t, pad_b);
      fill_rect(px, fb, cx, py0 + 1, 1, ph - 2, UI_LOG_CURSOR);
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
                  int scrub, struct hits *h)
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

   pager_row(fb, h, x, rx, nav_y, sc, lh, page, npages, MA_WTLOG_PAGE);

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
   if (scrub >= 0 && scrub < wt->nwt) {
      const struct wt_rec *p = &wt->wt[scrub];
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
           wt_from_of(wt, now), scrub);
   /* The whole plot scrubs; the shell resolves the point via ui_wt_hit. */
   /* arg carries sc: the shell needs the SAME scale the plot was drawn at to
    * map a finger x back to a point, and re-deriving it there would be a
    * second copy of the layout that can drift. */
   add_hit(h, ui_rect(x, plot_top, pw, plot_h), ACT_SCRUB, sc);
}

/* ================= THE LOG PLOT =================================
 *
 * ONE CHART FOR THE THREE LOGS, and it is the weight trend's: a frame, a
 * light grid, both y bounds named against the axis end each describes, date
 * ticks along the bottom, and the entries themselves as squares joined by a
 * line. Exercise and insulin draw it too, so the three log screens are one
 * instrument read three ways rather than three that have to be learned
 * separately.
 *
 * WHAT DIFFERS IS COLOUR, and only colour. Weight is one series; insulin has
 * two (slow and fast) and exercise three (its levels), and a dot's colour is
 * which one it belongs to. The joining line follows the same rule: it runs
 * within a series and never between them, because a line drawn from a 20-unit
 * slow dose to a 4-unit fast one would state a quantity that was never held.
 *
 * NOT BARS. Daily totals were the first shape of this, and they cannot carry
 * the colours: a day with a light session and a hard one has no single level,
 * and a day's insulin no single type. One dot per entry keeps every entry's
 * own category, which is the thing being asked for. */

const int ui_day_days[UI_DAY_TABS]            = {7, 14, 30, 90, 0};
const char *const ui_day_tab_lbl[UI_DAY_TABS] = {"1W", "2W", "1M", "3M",
                                                 "ALL"};

#define LOG_PAD 4

long day_from_of(int tab, long now, long oldest)
{
   if (tab < 0 || tab >= UI_DAY_TABS)
      tab = 0;
   /* 0 = everything, the sentinel ui_wt_days uses for its own ALL tab. It
    * reaches back to the log's first entry: there is nothing before it to
    * show, so starting at the epoch would be a chart that is mostly empty. */
   if (ui_day_days[tab] <= 0)
      return (oldest > 0 && oldest < now) ? oldest : now;
   return now - ((long)ui_day_days[tab] * 86400);
}

/* The window a set of points occupies: the time span actually covered and the
 * value range to scale against. */
struct log_win {
   long tmin, tmax;
   long lo, hi;
   int n;
};

static void log_window(const struct log_pt *p, int n, long from, long now,
                       struct log_win *w)
{
   w->n    = 0;
   w->tmin = from;
   w->tmax = now;
   w->lo   = 0;
   w->hi   = 0;
   for (int i = 0; i < n; i++) {
      /* BOTH ends. An entry after the window -- the forms accept any date, so
       * a mistyped year lands one decades ahead -- would otherwise be
       * excluded from the drawing and still counted in lo/hi, scaling the
       * axis to a value that is never plotted. */
      if (p[i].t < from || p[i].t > now)
         continue;
      if (!w->n) {
         w->tmin = p[i].t;
         w->lo   = p[i].v;
         w->hi   = p[i].v;
      }
      if (p[i].t < w->tmin)
         w->tmin = p[i].t;
      if (p[i].v < w->lo)
         w->lo = p[i].v;
      if (p[i].v > w->hi)
         w->hi = p[i].v;
      w->n++;
   }
   if (w->tmax <= w->tmin)
      w->tmax = w->tmin + 1; /* never divide by a zero-width window */
   /* lo AND hi ARE THE DATA'S OWN, even when they are equal. Widening the
    * range here to keep the division safe would put the fudge on the axis:
    * with doses held in thousandths, a lo+1 upper bound labels a flat run of
    * 20 U doses "20.001 U". log_py handles the flat case instead, where it
    * is a question about where to draw rather than about what the data is. */
}

/* THE RESERVED STRIPS, top and bottom, worked out ONCE.
 *
 * The y bounds are printed inside the plot, so the data band stops short of
 * both or a point lands on the text naming it. The picker needs the same
 * numbers the drawing used -- it maps a finger back to a point through the
 * identical geometry -- and two copies of this arithmetic is exactly how a
 * pick comes to name a point other than the one under the finger. */
static void log_pads(int ph, int sc, int *pad_t, int *pad_b)
{
   const int half = (7 * sc) / 2;
   int t          = (13 * sc) + half;
   int b          = (22 * sc) + half;
   if (t + b > (ph * 2) / 3) { /* a very short plot: share it out */
      t = ph / 6;
      b = ph / 4;
   }
   *pad_t = t;
   *pad_b = b;
}

static int log_px(const struct log_win *w, long t, int px0, int pw, int pad)
{
   return px0 + pad +
          (int)(((t - w->tmin) * (long)(pw - (2 * pad))) / (w->tmax - w->tmin));
}

/* Separate top and bottom insets: the y bounds are printed INSIDE the plot,
 * so the data band stops short of both or a point lands on the text naming
 * it. Insetting the band is the same thing as widening the range, and it is
 * exact -- no point can enter a reserved strip, whatever the data does. */
static int log_py(const struct log_win *w, long v, int py0, int ph, int pad_t,
                  int pad_b)
{
   const int band = ph - pad_t - pad_b;
   /* A SERIES THAT NEVER MOVES draws down the middle of the band. There is no
    * scale to place it on -- every point is the same value -- and pinning it
    * to the top or the bottom would suggest one. */
   if (w->hi <= w->lo)
      return py0 + pad_t + (band / 2);
   return py0 + pad_t + band -
          (int)(((v - w->lo) * (long)band) / (w->hi - w->lo));
}

int log_pick(const struct log_pt *p, int n, long from, long now, int px0,
             int py0, int pw, int ph, int sc, int x, int y, int lock)
{
   struct log_win w;
   log_window(p, n, from, now, &w);
   if (!w.n)
      return -1;
   const int pad = LOG_PAD * sc;
   int pad_t     = 0;
   int pad_b     = 0;
   log_pads(ph, sc, &pad_t, &pad_b);
   int best = -1;
   long bd  = 0;
   for (int i = 0; i < n; i++) {
      if (p[i].t < from || p[i].t > now)
         continue; /* the same window the renderer draws, or the pick misses */
      /* BOUND TO ONE SERIES once the gesture has chosen it. With two traces
       * crossing -- and a day's slow and fast doses are minutes apart on the
       * x axis -- an unbound drag hops between them wherever they pass, so
       * the readout changes type under a finger that never left the curve it
       * started on. -1 means nothing is bound yet: the press is free to land
       * on either. */
      if (lock >= 0 && p[i].series != lock)
         continue;
      /* BOTH AXES TO CHOOSE, ONE AXIS TO TRACK.
       *
       * The press has to decide WHICH curve the gesture is about, and on x
       * alone every dose in a day is the same distance away -- a finger
       * anywhere in a column would take whichever of that day's points came
       * last in the array rather than the one it was on. So the opening pick
       * measures both.
       *
       * Once a curve is bound, x alone: the finger is sweeping ALONG a trace
       * to read it, exactly as on the glucose and weight plots, and a
       * y term there would make the readout stick or skip wherever the hand
       * drifted off the line it is following. `lock` is what tells the two
       * apart -- it is -1 only on the press that begins a scrub. */
      const long dx = log_px(&w, p[i].t, px0, pw, pad) - x;
      long d        = dx * dx;
      if (lock < 0) {
         const long dy = log_py(&w, p[i].v, py0, ph, pad_t, pad_b) - y;
         d += dy * dy;
      }
      if (best < 0 || d < bd) {
         bd   = d;
         best = i;
      }
   }
   return best;
}

/* An axis value as text at the scale its series is held in. dp 0 prints the
 * number; dp 3 renders thousandths the way a dose is written, which is what
 * ins_units_str already does -- one renderer for both, so an axis and a
 * readout cannot disagree about the same quantity. */
static void log_val_str(long v, int dp, char *out, int cap)
{
   if (dp == 3)
      (void)ins_units_str((int)v, out, cap);
   else
      (void)snprintf(out, (size_t)cap, "%ld", v);
}

void log_plot(uint32_t *px, const struct ANativeWindow_Buffer *fb,
              const struct log_pt *p, int n, long from, long now, int px0,
              int py0, int pw, int ph, int sc, long tz_off, int hilite,
              const char *unit, int dp, const uint32_t *col, int ncol)
{
   draw_frame(px, fb, px0, py0, pw, ph, UI_LOG_FRAME);
   struct log_win w;
   log_window(p, n, from, now, &w);
   if (!w.n) {
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "no data in range",
               UI_MUTED);
      return;
   }
   const int pad = LOG_PAD * sc;
   int pad_t     = 0;
   int pad_b     = 0;
   log_pads(ph, sc, &pad_t, &pad_b);

   const uint32_t grid = UI_LOG_GRID;
   for (int i = 1; i < 4; i++)
      fill_rect(px, fb, px0 + 1,
                py0 + pad_t + (((ph - pad_t - pad_b) * i) / 4), pw - 2, 1,
                grid);
   const int nticks = 4; /* 3 interior + the right edge; see the label loop */
   for (int i = 1; i <= nticks; i++)
      fill_rect(px, fb, px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1)),
                py0 + 1, 1, ph - 2, grid);

   /* BOTH BOUNDS, each against the axis end it names -- not one "lo-hi" in a
    * corner, which states the range but not which end is which way up. */
   {
      char lab[24];
      char v[16];
      log_val_str(w.hi, dp, v, sizeof v);
      (void)snprintf(lab, sizeof lab, "%s %s", v, unit ? unit : "");
      draw_str(px, fb, px0 + (4 * sc), py0 + (3 * sc), sc, lab, UI_MUTED);
      log_val_str(w.lo, dp, v, sizeof v);
      (void)snprintf(lab, sizeof lab, "%s %s", v, unit ? unit : "");
      draw_str(px, fb, px0 + (4 * sc), py0 + ph - (19 * sc), sc, lab, UI_MUTED);
   }

   for (int i = 1; i <= nticks; i++) {
      const int gx = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
      const long tt =
          w.tmin + (((w.tmax - w.tmin) * (long)i) / (long)(nticks + 1));
      char dt[20];
      char md[8];
      fmt_date(tt, tz_off, dt, sizeof dt);
      str_snapshot(md, sizeof md, (str_len(dt) > 5) ? dt + 5 : "");
      if (str_len(md) > 5)
         md[5] = 0; /* "MM-DD" */
      draw_str(px, fb, gx - ((str_len(md) * 6 * sc) / 2), py0 + ph - (9 * sc),
               sc, md, UI_DISCLAIM);
   }

   /* ONE PASS PER SERIES, so each trace joins only its own entries and the
    * line never crosses between two kinds. */
   for (int s = 0; s < ncol; s++) {
      int prevx = 0;
      int prevy = 0;
      int have  = 0;
      for (int i = 0; i < n; i++) {
         if (p[i].series != s || p[i].t < from || p[i].t > now)
            continue;
         const int cx = log_px(&w, p[i].t, px0, pw, pad);
         const int cy = log_py(&w, p[i].v, py0, ph, pad_t, pad_b);
         if (have) {
            const int dx = cx - prevx;
            const int dy = cy - prevy;
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            const int st = ax > ay ? ax : ay;
            for (int k = 1; k <= st && st > 0; k++)
               fill_rect(px, fb, prevx + ((dx * k) / st),
                         prevy + ((dy * k) / st), sc, sc, col[s]);
         }
         fill_rect(px, fb, cx - (2 * sc), cy - (2 * sc), 4 * sc, 4 * sc,
                   col[s]);
         prevx = cx;
         prevy = cy;
         have  = 1;
      }
   }

   /* SCRUB CURSOR: a full-height rule through the picked entry and the entry
    * itself redrawn larger in UI_HILITE grey -- the same "coloured normally,
    * grey when picked" pair the weight trend and the glucose plot use, so all
    * three read the same way. */
   if (hilite >= 0 && hilite < n && p[hilite].t >= from &&
       p[hilite].t <= now) {
      const int cx = log_px(&w, p[hilite].t, px0, pw, pad);
      const int cy = log_py(&w, p[hilite].v, py0, ph, pad_t, pad_b);
      fill_rect(px, fb, cx, py0 + 1, 1, ph - 2, UI_LOG_CURSOR);
      fill_rect(px, fb, cx - (3 * sc), cy - (3 * sc), 6 * sc, 6 * sc,
                UI_HILITE);
   }
}

/* ---- WHAT EACH LOG PUTS ON THE PLOT ----------------------------
 *
 * One builder per log, and each is the ONLY definition of its series -- the
 * renderer draws what it returns and the scrub picker picks from what it
 * returns, so a finger can never resolve against a different set of entries
 * than the one on screen. `from` comes back too, because the window is part
 * of the answer. */

const uint32_t ui_ins_col[UI_INS_SERIES] = {
    UI_TEXT_DIM,  /* SLOW: the dim its rows are printed in */
    UI_MARK_FAST, /* FAST: the same soft blue theirs carry */
};

int ins_points(const struct screen *m, struct log_pt *out, int cap, long *from)
{
   const long f = day_from_of(m->ins.inslog_tab, m->now,
                              (m->ins.ins_nlog > 0) ? m->ins.ins_log[0].t : 0);
   if (from)
      *from = f;
   int n = 0;
   for (int i = 0; i < m->ins.ins_nlog && n < cap; i++) {
      const struct ins_rec *d = &m->ins.ins_log[i];
      out[n].t                = d->t;
      /* THOUSANDTHS on the axis, so a 0.5 U dose sits half a unit up rather
       * than being rounded to nothing. The bound labels render through
       * ins_units_str, so the axis still reads in units. */
      out[n].v                = d->milli;
      out[n].series           = (d->type == INS_FAST) ? 1 : 0;
      n++;
   }
   return n;
}

/* Local midnight at or before `t`, read in the offset the rest of the screen
 * renders its dates in. ONE offset for the whole plot, the same one the table
 * above it uses: a point and the rows it totals must land on the same date,
 * and reading each instant in its own historical offset would put a session
 * from the other side of a DST change on a different day from the row that
 * prints it. */
static long day_of(long t, long tz_off)
{
   const long l = t + tz_off;
   long d       = l / 86400;
   if (l < 0 && (l % 86400) != 0)
      d--; /* floor, not the truncation toward zero C division gives */
   return (d * 86400) - tz_off;
}

/* ONE POINT PER DAY, carrying every minute exercised that day whatever the
 * level.
 *
 * NOT one point per session, and not one series per level. The question this
 * plot answers is how much was done each day, and a day's exercise is a total
 * -- a morning walk and an evening run are one day's effort, not two competing
 * readings. Splitting by level would also give a day with both no single
 * colour to be.
 *
 * THE ZERO DAYS ARE DRAWN, because a rest day is data. Emitting only the days
 * with something on them would join two active days with a line straight
 * across the gap, drawing effort that did not happen. */
int ex_points(const struct screen *m, struct log_pt *out, int cap, long *from)
{
   const long f = day_from_of(m->food.exlog_tab, m->now,
                              (m->food.nexlog > 0) ? m->food.exlog[0].t : 0);
   long d0      = day_of(f, m->tz_off);
   const long d1 = day_of(m->now, m->tz_off);
   long n        = ((d1 - d0) / 86400) + 1;
   if (n < 1)
      n = 1;
   if (n > cap) {
      /* CLAMPED FROM THE OLD END, so the right-hand edge stays today.
       * Dropping the newest days instead would quietly turn a long span into
       * a chart of some earlier window with nothing on screen to say so. */
      d0 = d1 - ((long)(cap - 1) * 86400);
      n  = cap;
   }
   /* The window starts at the first day drawn, not at the raw span start: a
    * point sits at its day's MIDNIGHT, so a `from` part-way through that day
    * would put the leftmost point outside the window and drop it. */
   if (from)
      *from = d0;
   for (long i = 0; i < n; i++) {
      out[i].t      = d0 + (i * 86400);
      out[i].v      = 0;
      out[i].series = 0;
   }
   for (int i = 0; i < m->food.nexlog; i++) {
      const struct ex_rec *e = &m->food.exlog[i];
      /* THE RUNNING SESSION COUNTS WHAT IT HAS DONE SO FAR. Its dur is 0
       * until it is closed, so taking the column at face value would leave
       * today flat through an hour's walk and then jump. A row that is open
       * and is NOT running has no length at all and adds nothing -- the same
       * distinction the MIN column draws. */
      long secs = e->dur;
      if (i == m->food.exlog_act)
         secs = (m->now > e->t) ? m->now - e->t : 0;
      if (secs <= 0)
         continue;
      const long k = (day_of(e->t, m->tz_off) - d0) / 86400;
      if (k < 0 || k >= n)
         continue; /* outside the span, which also drops a mistyped year */
      out[k].v += secs / 60;
   }
   return (int)n;
}

int ui_log_hit(const struct screen *m, int plot_x, int plot_y, int plot_w,
               int plot_h, int sc, int x, int y, int *lock)
{
   /* An if-chain, not a switch: -Wswitch-enum makes a switch over a screen
    * name all forty of them, and thirty-seven "not this one" cases would bury
    * the three that answer. */
   if (m->scr == SCR_WTLOG)
      return ui_wt_hit(m, plot_x, plot_w, sc, x);
   struct log_pt pts[UI_LOG_PTS];
   long from = 0;
   int n     = 0;
   if (m->scr == SCR_EXLOG)
      n = ex_points(m, pts, UI_LOG_PTS, &from);
   else if (m->scr == SCR_INSLOG)
      n = ins_points(m, pts, UI_LOG_PTS, &from);
   else
      return -1;
   const int held = lock ? *lock : -1;
   int at = log_pick(pts, n, from, m->now, plot_x, plot_y, plot_w, plot_h, sc,
                     x, y, held);
   /* THE SERIES IS BOUND BY THE FIRST PICK OF THE GESTURE and reported back,
    * so the caller can hand it to every move that follows. Only when nothing
    * was bound yet: a drag must not be able to re-bind itself onto a curve it
    * has wandered near. */
   if (lock && held < 0 && at >= 0)
      *lock = pts[at].series;
   return at;
}

/* ---- OLD DEVICES: DISCONNECTED devices. Each keeps its whole slot, so a row
 * opens the SAME per-device menu as a live one (MA_SENSOR + slot index). The
 * list PAGINATES: if there are more than fit, a "< PAGE i/n >" row at the
 * bottom navigates, so any number of old devices is usable. ---- */

long ui_wt_from(const struct screen *m)
{
   return wt_from_of(&m->wt, m->now);
}
