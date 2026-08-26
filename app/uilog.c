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
   int tsc      = FONT_TITLE(sc);
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
   int tsc      = FONT_TITLE(sc);
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
   int tsc      = FONT_TITLE(sc);
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
      draw_str(px, fb, x, y, sc, "NO DOSES LOGGED YET.", UI_MUTED);
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
      /* THE DOSE IS THE VALUE; which insulin it was rides with the unit,
       * because "U FAST" is what the number is a quantity OF. */
      (void)snprintf(line, sizeof line, "U %s",
                     d->type == INS_FAST ? "FAST" : "SLOW");
      char ipad[16];
      (void)snprintf(ipad, sizeof ipad, "%5s", iu);
      log_scrub_row(px, fb, x, plot_top - trow, fb->width - (2 * x), sc, 2 * sc,
                    when, ipad, line);
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
   int tsc      = FONT_TITLE(sc);
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
   int tsc      = FONT_TITLE(sc);
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
   draw_str(px, fb, x, y, sc, "THIS CANNOT BE UNDONE.", UI_MUTED);
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
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "NO DATA IN RANGE",
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
      /* A CLOCK ON A SHORT SPAN, A DATE ON A LONG ONE.
       *
       * fmt_date lays out "YYYY-MM-DD HH:MM", so the two halves sit at fixed
       * offsets and both are five characters wide. Which one to show is
       * decided by how much time the plot covers: four ticks across three
       * hours all fall on one day and would print one date four times,
       * saying nothing about where along the axis they sit, while four
       * across a month all fall at a similar clock time and would repeat one
       * hour. Two days is the crossover -- past it a tick can no longer be
       * told from its neighbours by time of day alone. */
      const int byday = (w.tmax - w.tmin) > 2 * 86400;
      const int off   = byday ? 5 : 11;
      str_snapshot(md, sizeof md, (str_len(dt) > off) ? dt + off : "");
      if (str_len(md) > 5)
         md[5] = 0; /* "MM-DD" or "HH:MM" */
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
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "WEIGHT LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_WTLOG_BACK, 0);
   y += 3 * lh;

   if (wt->nwt <= 0) {
      draw_str(px, fb, x, y, sc, "NO WEIGHTS LOGGED YET.", UI_MUTED);
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
      /* THE INSTANT, THE WEIGHT, THE UNIT -- the app's one readout layout;
       * see log_scrub_row. White for the value and the point marked in
       * UI_HILITE grey, the same pair the glucose plot uses: green means
       * "on / enabled" everywhere else in this app, and a readout is
       * neither. */
      /* Right-aligned in a fixed field so the digits keep their columns as
        * the finger moves; see the main plot's readout, which explains why. */
      (void)snprintf(line, sizeof line, "%5s", wv);
      log_scrub_row(px, fb, x, plot_top - trow, fb->width - (2 * x), sc, 2 * sc,
                    when, line, wt_unit_name(prefs->wunits));
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

const int ui_exday_hours[UI_EXDAY_TABS] = {6, 24, 30 * 24, 90 * 24, 0};
const char *const ui_exday_tab_lbl[UI_EXDAY_TABS] = {"6H", "24H", "1M", "3M",
                                                     "ALL"};

const int ui_day_days[UI_DAY_TABS]            = {7, 14, 30, 90, 0};
const char *const ui_day_tab_lbl[UI_DAY_TABS] = {"1W", "2W", "1M", "3M",
                                                 "ALL"};

#define LOG_PAD 4

/* The EXERCISE LOG's version, over ui_exday_days -- and its ALL tab reaches
 * back to whichever of the TWO logs starts earlier, because the plot draws
 * both and a span that covered only one of them would cut the other off. */
long exday_from_of(int tab, long now, long oldest_ex, long oldest_step)
{
   if (tab < 0 || tab >= UI_EXDAY_TABS)
      tab = 0;
   if (ui_exday_hours[tab] > 0)
      return now - ((long)ui_exday_hours[tab] * 3600);
   long o = oldest_ex;
   if (o <= 0 || (oldest_step > 0 && oldest_step < o))
      o = oldest_step;
   return (o > 0 && o < now) ? o : now;
}

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
         w->lo = p[i].v;
         w->hi = p[i].v;
      }
      if (p[i].v < w->lo)
         w->lo = p[i].v;
      if (p[i].v > w->hi)
         w->hi = p[i].v;
      w->n++;
   }
   /* THE X AXIS IS THE SELECTED SPAN, not the data's own extent -- tmin and
    * tmax are the `from` and `now` set above and nothing below moves them.
    *
    * Collapsing them onto the first and last point made a tab a lie: "3M"
    * holding a week of entries drew that week across the full width, pixel
    * for pixel identical to the "1W" view, with only the date ticks to tell
    * them apart. A span the user picked has to be the span they get, and a
    * short history leaves the left of the plot empty -- which is the truth
    * about a short history. The weight trend already worked this way; this is
    * the same rule, stated in the other plot. */
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

/* Where a series stands at `t`, interpolated between the two entries either
 * side of it. Used only for the instant a segment crosses the window's edge,
 * which is why a zero-width step answers with the later value rather than
 * dividing. */
static long log_cross(const struct log_pt *a, const struct log_pt *b, long t)
{
   const long dt = b->t - a->t;
   if (dt <= 0)
      return b->v;
   return a->v + (((b->v - a->v) * (t - a->t)) / dt);
}

/* One straight run between two plotted points, stepped along whichever axis is
 * longer so the line has no gaps. */
static void log_seg(uint32_t *px, const struct ANativeWindow_Buffer *fb, int x0,
                    int y0, int x1, int y1, int sc, uint32_t c)
{
   const int dx = x1 - x0;
   const int dy = y1 - y0;
   const int ax = dx < 0 ? -dx : dx;
   const int ay = dy < 0 ? -dy : dy;
   const int st = ax > ay ? ax : ay;
   for (int k = 1; k <= st && st > 0; k++)
      fill_rect(px, fb, x0 + ((dx * k) / st), y0 + ((dy * k) / st), sc, sc, c);
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

/* THE EXERCISE PLOT PICKS ON X ALONE, at the press as well as during the drag.
 *
 * Its two series do not share a vertical scale -- see exlog_plot -- so there is
 * no single y a finger can be near, and log_pick's opening both-axes measure
 * would be comparing a distance in minutes against one in steps and taking
 * whichever came out numerically smaller. There is nothing for it to decide in
 * any case: the day tabs put both series on the same instants, so an x names
 * one DAY and the readout gives both of that day's numbers, and the 24 H tab
 * has only the step buckets on it. */
static int ex_pick(const struct log_pt *p, int n, long from, long now, int px0,
                   int pw, int sc, int x)
{
   struct log_win w;
   log_window(p, n, from, now, &w);
   if (!w.n)
      return -1;
   const int pad = LOG_PAD * sc;
   int best      = -1;
   long bd       = 0;
   for (int i = 0; i < n; i++) {
      if (p[i].t < from || p[i].t > now)
         continue; /* the same window the renderer draws, or the pick misses */
      const long dx = log_px(&w, p[i].t, px0, pw, pad) - x;
      const long d  = dx * dx;
      if (best < 0 || d < bd) {
         bd   = d;
         best = i;
      }
   }
   return best;
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

/* THE PLOT'S FURNITURE: the divisions and the date ticks, which every log
 * plot draws identically and none of them owns.
 *
 * ONE DEFINITION, because the two callers differ only in what they put INSIDE
 * the frame -- one scale or two -- and a grid that drifted between them would
 * make the same span look like two different charts. */
static void log_chrome(uint32_t *px, const struct ANativeWindow_Buffer *fb,
                       const struct log_win *w, int px0, int py0, int pw,
                       int ph, int sc, long tz_off, int pad, int pad_t,
                       int pad_b, long step)
{
   const uint32_t grid = UI_LOG_GRID;
   for (int i = 1; i < 4; i++)
      fill_rect(px, fb, px0 + 1,
                py0 + pad_t + (((ph - pad_t - pad_b) * i) / 4), pw - 2, 1,
                grid);
   /* WHERE THE VERTICAL DIVISIONS FALL.
    *
    * `step` > 0 puts them on ROUND INSTANTS -- a whole number of hours in
    * local time -- rather than at even fractions of the window. On a span of
    * hours the difference is the whole point of the axis: fifths of six hours
    * land on 07:12 and 08:24, and a reader who wants to know what they were
    * doing at nine has to interpolate between two arbitrary numbers. The
    * window slides with `now`, so these lines drift leftward across the plot
    * while their labels stay put, which is the honest way round.
    *
    * `step` == 0 keeps the even fractions, which is right for the spans
    * measured in weeks: there is no round instant a month-long axis wants to
    * be cut at, and a fixed count keeps the plot's furniture stable. */
   const int nticks = 4; /* 3 interior + the right edge; see the label loop */
   const int half   = (5 * 6 * sc) / 2; /* a 5-glyph label's half-width */
   for (int i = 1; i <= nticks; i++) {
      long tt;
      int gx;
      if (step > 0) {
         /* The i-th round instant at or after the window's left edge. */
         long t0 = (((w->tmin + tz_off) / step) * step) - tz_off;
         if (t0 < w->tmin)
            t0 += step;
         tt = t0 + ((long)(i - 1) * step);
         if (tt > w->tmax)
            break;
         gx = log_px(w, tt, px0, pw, pad);
      } else {
         gx = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
         tt = w->tmin + (((w->tmax - w->tmin) * (long)i) / (long)(nticks + 1));
      }
      fill_rect(px, fb, gx, py0 + 1, 1, ph - 2, grid);
      /* A LABEL THAT WOULD RUN OFF ITS OWN PLOT IS NOT DRAWN. The rule still
       * is: the division is the thing being marked, and half a date under it
       * reads as a rendering fault rather than as a number. */
      if (gx - half < px0 || gx + half > px0 + pw)
         continue;
      char dt[20];
      char md[8];
      fmt_date(tt, tz_off, dt, sizeof dt);
      /* A CLOCK ON A SHORT SPAN, A DATE ON A LONG ONE.
       *
       * fmt_date lays out "YYYY-MM-DD HH:MM", so the two halves sit at fixed
       * offsets and both are five characters wide. Which one to show is
       * decided by how much time the plot covers: four ticks across three
       * hours all fall on one day and would print one date four times,
       * saying nothing about where along the axis they sit, while four
       * across a month all fall at a similar clock time and would repeat one
       * hour. Two days is the crossover -- past it a tick can no longer be
       * told from its neighbours by time of day alone. */
      const int byday = (w->tmax - w->tmin) > 2 * 86400;
      const int off   = byday ? 5 : 11;
      str_snapshot(md, sizeof md, (str_len(dt) > off) ? dt + off : "");
      if (str_len(md) > 5)
         md[5] = 0; /* "MM-DD" or "HH:MM" */
      draw_str(px, fb, gx - ((str_len(md) * 6 * sc) / 2), py0 + ph - (9 * sc),
               sc, md, UI_DISCLAIM);
   }
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
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "NO DATA IN RANGE",
               UI_MUTED);
      return;
   }
   const int pad = LOG_PAD * sc;
   int pad_t     = 0;
   int pad_b     = 0;
   log_pads(ph, sc, &pad_t, &pad_b);

   log_chrome(px, fb, &w, px0, py0, pw, ph, sc, tz_off, pad, pad_t, pad_b, 0);

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

   /* ONE PASS PER SERIES, so each trace joins only its own entries and the
    * line never crosses between two kinds.
    *
    * A SEGMENT THAT CROSSES THE WINDOW'S EDGE IS DRAWN AS FAR AS THE EDGE.
    *
    * Skipping every out-of-window point took the crossing segment with it, so
    * a trace whose previous entry fell just before the span began started in
    * mid-air a little way in from the frame -- the reader sees a gap and has
    * no way to tell "no doses that early" from "the line is off-screen".
    * There IS a line there: it is the run between two real entries, and part
    * of it lies inside the plot.
    *
    * The out-of-window end is not drawn where it belongs -- that is off the
    * plot, and a line stepped to it would spill across the frame -- so it is
    * replaced by where the segment CUTS the edge, interpolated between the
    * two entries. The visible part is then exactly the part that is inside,
    * and it meets the frame instead of stopping short of it. Only the line:
    * an entry outside the span still gets no marker, because there is no
    * instant on this axis to put one at. */
   for (int s = 0; s < ncol; s++) {
      const struct log_pt *pv = NULL; /* previous entry of THIS series */
      int prevx               = 0;
      int prevy               = 0;
      int have                = 0;
      for (int i = 0; i < n; i++) {
         if (p[i].series != s)
            continue;
         const struct log_pt *cu = &p[i];
         if (cu->t >= from && cu->t <= now) {
            const int cx = log_px(&w, cu->t, px0, pw, pad);
            const int cy = log_py(&w, cu->v, py0, ph, pad_t, pad_b);
            if (!have && pv && pv->t < from) {
               prevx = log_px(&w, from, px0, pw, pad);
               prevy = log_py(&w, log_cross(pv, cu, from), py0, ph, pad_t,
                              pad_b);
               have  = 1;
            }
            if (have)
               log_seg(px, fb, prevx, prevy, cx, cy, sc, col[s]);
            fill_rect(px, fb, cx - (2 * sc), cy - (2 * sc), 4 * sc, 4 * sc,
                      col[s]);
            prevx = cx;
            prevy = cy;
            have  = 1;
         } else if (have && cu->t > now) {
            /* Leaving to the right: only reachable from an entry dated in the
             * future, which the forms accept and log_window excludes. The
             * line still runs to the edge rather than stopping at the last
             * real reading. */
            log_seg(px, fb, prevx, prevy, log_px(&w, now, px0, pw, pad),
                    log_py(&w, log_cross(pv, cu, now), py0, ph, pad_t, pad_b),
                    sc, col[s]);
            have = 0;
         }
         pv = cu;
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

/* THE EXERCISE LOG'S TWO SERIES, on whichever grid the chosen tab uses.
 *
 * SERIES 0 IS EXERCISE, one point per day carrying every minute exercised
 * that day whatever the level. Not one point per session, and not one series
 * per level: the question the plot answers is how much was done each day, and
 * a day's exercise is a total -- a morning walk and an evening run are one
 * day's effort, not two competing readings. Splitting by level would also
 * leave a day with both no single colour to be. THE ZERO DAYS ARE DRAWN,
 * because a rest day is data; emitting only the days with something on them
 * would join two active days with a line straight across the gap.
 *
 * SERIES 1 IS THE STEP COUNT on the same grid, so the two can be read against
 * each other -- which is the whole reason they share a plot. The scales are
 * not shared; see exlog_plot.
 *
 * THE 24 H TAB IS A DIFFERENT GRID and a different shape. A day of steps is
 * worth seeing at the resolution it was recorded in, so there the buckets are
 * the log's own five minutes and exercise stops being a curve: a day holds
 * one or two sessions, which as a line is two spikes and a lot of floor.
 * `band` takes it instead -- the level in force during each bucket -- and
 * exlog_plot draws that as a strip.
 */
/* The start of the bucket `t` falls in, read in the offset the rest of the
 * screen renders its dates in. ONE offset for the whole plot, the same one the
 * table above it uses: a point and the rows it totals must land on the same
 * date, and reading each instant in its own historical offset would put a
 * session from the other side of a DST change on a different day from the row
 * that prints it. Five-minute buckets need no such care -- every zone this app
 * meets is a whole number of hours off -- but they go through the same floor
 * so one rule covers both. */
static long ex_floor(long t, long width, long tz_off)
{
   const long l = t + tz_off;
   long d       = l / width;
   if (l < 0 && (l % width) != 0)
      d--; /* floor, not the truncation toward zero C division gives */
   return (d * width) - tz_off;
}

/* Every minute of a record that falls inside the plot, the running session
 * measured up to `now`. Its `dur` is 0 until it is closed, so taking the
 * column at face value would leave today flat through an hour's walk and then
 * jump; a row that is open and is NOT running has no length at all and adds
 * nothing, which is the same distinction the MIN column draws. */
static long ex_secs_of(const struct screen *m, int i)
{
   const struct ex_rec *e = &m->food.exlog[i];
   if (i == m->food.exlog_act)
      return (m->now > e->t) ? m->now - e->t : 0;
   return e->dur;
}

/* Local midnight at or before `t`. Exported so the EXERCISE LOG's TODAY line
 * and the plot's day buckets floor the day identically -- two copies of this
 * is how a total comes to disagree with the bar beside it. */
long ex_day_floor(long t, long tz_off)
{
   return ex_floor(t, 86400, tz_off);
}

/* HOW WIDE A BUCKET IS on the sub-day tabs, and the ONE place that decides.
 *
 * Six hours of steps is worth seeing at the resolution the log records in.
 * A whole day at that resolution is 288 buckets sharing a few hundred pixels
 * -- a smear rather than data, and a readout that answers "how many steps in
 * these five minutes" when the question a day-long view asks is "when was I
 * moving". An hour is the unit that question comes in.
 *
 * The plot, the exercise band under it, the axis label and the scrub readout
 * must all agree about this number, which is why they ask for it rather than
 * each writing it down. */
long ex_bucket_for(int hours)
{
   return (hours <= 6) ? STEP_BUCKET_S : 3600;
}

/* The bucket's name for the axis: whole hours say so, anything shorter says
 * how many minutes it is. */
void ex_bucket_word(long width, char *out, int n)
{
   if (width >= 3600)
      (void)snprintf(out, (size_t)n, "HOUR");
   else
      (void)snprintf(out, (size_t)n, "%ld MIN", width / 60);
}

int ex_points(const struct screen *m, struct log_pt *out, int cap, long *from,
              unsigned char *band, int bandcap)
{
   int tab = m->food.exlog_tab;
   if (tab < 0 || tab >= UI_EXDAY_TABS)
      tab = 0;

   /* ---- A DAY OR LESS: bucketed steps, exercise as a band ---- */
   if (ui_exday_hours[tab] > 0 && ui_exday_hours[tab] <= 24) {
      const long width = ex_bucket_for(ui_exday_hours[tab]);
      const long start =
          ex_floor(m->now - ((long)ui_exday_hours[tab] * 3600), width,
                   m->tz_off);
      long n = ((ex_floor(m->now, width, m->tz_off) - start) / width) + 1;
      if (n < 1)
         n = 1;
      if (n > cap)
         n = cap;
      if (from)
         *from = start;
      for (long i = 0; i < n; i++) {
         out[i].t      = start + (i * width);
         out[i].v      = 0;
         out[i].series = 1;
      }
      int got = 0;
      for (int i = 0; i < m->food.nsteps; i++) {
         const long k =
             (ex_floor(m->food.steps[i].t, width, m->tz_off) - start) / width;
         if (k >= 0 && k < n) {
            out[k].v += m->food.steps[i].n;
            got = 1;
         }
      }
      /* THE OPEN WINDOW, WHILE IT IS STILL EMPTY, IS NOT A DATAPOINT: it has
       * not closed, so nothing has been written for it and nothing is known
       * about it. Earlier zeroes stay -- those windows did close, and their
       * emptiness was observed. */
      if (n > 0 && out[n - 1].v == 0)
         n--;
      /* THE BAND, one entry per bucket, carrying the HARDEST level in force
       * during it. A bucket straddling the end of one session and the start
       * of a harder one is coloured by the harder: the strip answers "was I
       * exercising, and how hard", and the gentler reading would understate
       * it. */
      if (band) {
         const long bn = (n < bandcap) ? n : bandcap;
         for (long i = 0; i < bn; i++)
            band[i] = 0;
         for (int i = 0; i < m->food.nexlog; i++) {
            const long secs = ex_secs_of(m, i);
            if (secs <= 0)
               continue;
            const struct ex_rec *e = &m->food.exlog[i];
            long k0                = (e->t - start) / width;
            long k1                = (e->t + secs - 1 - start) / width;
            if (e->t < start)
               k0 = 0;
            if (k1 >= bn)
               k1 = bn - 1;
            if (k0 < 0)
               k0 = 0;
            for (long k = k0; k <= k1; k++)
               if (band[k] < (unsigned char)e->level)
                  band[k] = (unsigned char)e->level;
         }
      }
      return got ? (int)n : 0;
   }

   /* ---- THE DAY TABS: both series, one point per day each ---- */
   /* HALF THE ARRAY EACH. Two series live in one array, so the day count is
    * bounded by half the capacity rather than all of it. */
   const int half = cap / 2;
   const long f   = exday_from_of(tab, m->now,
                                  (m->food.nexlog > 0) ? m->food.exlog[0].t : 0,
                                  (m->food.nsteps > 0) ? m->food.steps[0].t : 0);
   long d0        = ex_floor(f, 86400, m->tz_off);
   const long d1  = ex_floor(m->now, 86400, m->tz_off);
   long n         = ((d1 - d0) / 86400) + 1;
   if (n < 1)
      n = 1;
   if (n > half) {
      /* CLAMPED FROM THE OLD END, so the right-hand edge stays today.
       * Dropping the newest days instead would quietly turn a long span into
       * a chart of some earlier window with nothing on screen to say so. */
      d0 = d1 - ((long)(half - 1) * 86400);
      n  = half;
   }
   /* The window starts at the first day drawn, not at the raw span start: a
    * point sits at its day's MIDNIGHT, so a `from` part-way through that day
    * would put the leftmost point outside the window and drop it. */
   if (from)
      *from = d0;
   for (long i = 0; i < n; i++) {
      out[i].t          = d0 + (i * 86400);
      out[i].v          = 0;
      out[i].series     = 0;
      out[n + i].t      = out[i].t;
      out[n + i].v      = 0;
      out[n + i].series = 1;
   }
   for (int i = 0; i < m->food.nexlog; i++) {
      const long secs = ex_secs_of(m, i);
      if (secs <= 0)
         continue;
      const long k = (ex_floor(m->food.exlog[i].t, 86400, m->tz_off) - d0)
                     / 86400;
      if (k < 0 || k >= n)
         continue; /* outside the span, which also drops a mistyped year */
      out[k].v += secs / 60;
   }
   for (int i = 0; i < m->food.nsteps; i++) {
      const long k = (ex_floor(m->food.steps[i].t, 86400, m->tz_off) - d0)
                     / 86400;
      if (k >= 0 && k < n)
         out[n + k].v += m->food.steps[i].n;
   }
   return (int)(2 * n);
}

/* WHERE A VALUE SITS ON ITS OWN AXIS, from zero to that series' maximum.
 *
 * ZERO IS ALWAYS THE FLOOR, unlike log_py's data-relative scale. Both series
 * here are counts of something done -- minutes, steps -- so a day with none
 * is a real zero and belongs at the bottom; floating the axis on the smallest
 * value would draw the quietest day of the week as though it were nothing at
 * all, and the two series would each float by a different amount and stop
 * being comparable. */
static int ex_py(long v, long hi, int py0, int ph, int pad_t, int pad_b)
{
   const int h = ph - pad_t - pad_b;
   if (hi <= 0 || h <= 0)
      return py0 + pad_t + (h > 0 ? h : 0);
   long yy = (v * (long)h) / hi;
   if (yy < 0)
      yy = 0;
   if (yy > h)
      yy = h;
   return py0 + pad_t + h - (int)yy;
}

void exlog_plot(uint32_t *px, const struct ANativeWindow_Buffer *fb,
                const struct log_pt *p, int n, long from, long now, int px0,
                int py0, int pw, int ph, int sc, long tz_off, int hilite,
                const unsigned char *band, long bucket)
{
   struct log_win w;
   log_window(p, n, from, now, &w);
   draw_frame(px, fb, px0, py0, pw, ph, UI_LOG_FRAME);
   if (!w.n) {
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "NO DATA IN RANGE",
               UI_MUTED);
      return;
   }
   const int pad = LOG_PAD * sc;
   int pad_t     = 0;
   int pad_b     = 0;
   log_pads(ph, sc, &pad_t, &pad_b);
   /* ROUND HOURS WHEN THE SPAN IS SHORT ENOUGH FOR THEM TO MEAN SOMETHING,
    * which is exactly when the exercise band is drawn -- both are the sub-day
    * tabs. The step is the smallest of the usual divisions that still leaves
    * about four lines on the plot, so six hours is cut at every second hour
    * and a day at every sixth. */
   long step = 0;
   if (band) {
      static const long cand[] = {3600,     2 * 3600,  3 * 3600,
                                  6 * 3600, 12 * 3600, 86400};
      const long want          = (w.tmax - w.tmin) / 5;
      for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
         step = cand[i];
         if (cand[i] >= want)
            break;
      }
   }
   log_chrome(px, fb, &w, px0, py0, pw, ph, sc, tz_off, pad, pad_t, pad_b,
              step);

   /* EACH SERIES GETS ITS OWN CEILING. Minutes of exercise and thousands of
    * steps on one scale is not a comparison -- it is the step curve with a
    * flat line under it -- so the two are drawn against separate axes and the
    * axes are labelled in their series' own colour, which is what says which
    * number belongs to which trace. */
   long hi[2] = {0, 0};
   int cnt[2] = {0, 0};
   for (int i = 0; i < n; i++) {
      if (p[i].t < from || p[i].t > now)
         continue;
      const int sx = (p[i].series == 1) ? 1 : 0;
      cnt[sx]++;
      if (p[i].v > hi[sx])
         hi[sx] = p[i].v;
   }
   static const uint32_t excol[2] = {UI_EX_MOD, UI_MUTED};

   /* THE EXERCISE BAND, on the 24 H tab: a strip below the data showing when a
    * session was running and how hard, in the same blues the level wears
    * everywhere else. It sits in the bottom inset rather than over the plot
    * -- laid across the step trace it would read as a series of its own. */
   if (band) {
      const int by = py0 + ph - pad_b + (2 * sc);
      const int bh = 8 * sc;
      for (int i = 0; i < n; i++) {
         if (!band[i] || p[i].t < from || p[i].t > now)
            continue;
         const int x0 = log_px(&w, p[i].t, px0, pw, pad);
         const int x1 = log_px(&w, p[i].t + bucket, px0, pw, pad);
         int bw       = x1 - x0;
         if (bw < 1)
            bw = 1;
         fill_rect(px, fb, x0, by, bw, bh, ui_ex_color(band[i], UI_EX_LIGHT));
      }
   }

   /* THE AXIS BOUNDS, each against the side it belongs to. The right-hand
    * labels are pushed back by their own width so they end on the margin
    * rather than running past the frame. */
   {
      char lab[24];
      if (cnt[0] > 0) {
         /* Series 0 only ever appears on the day-bucketed tabs -- the short
          * spans draw exercise as a band instead -- so a point here is always
          * one day's total, which the dates along the axis already say. */
         (void)snprintf(lab, sizeof lab, "%ld MIN", hi[0]);
         draw_str(px, fb, px0 + (4 * sc), py0 + (3 * sc), sc, lab, excol[0]);
      }
      if (cnt[1] > 0) {
         /* THE BUCKET IS NAMED ONLY WHERE IT IS SURPRISING. On the spans
          * drawn five minutes at a time, a bare "700 STEPS" against a spike
          * reads as a running total or as a day's worth, so the window has to
          * be spelt out. A day-bucketed tab needs no such note: a point there
          * IS a day, which the date under it already says, and repeating it
          * on the axis is a word that tells the reader nothing they are not
          * looking straight at. */
         if (band) {
            char bw[12];
            ex_bucket_word(bucket, bw, sizeof bw);
            (void)snprintf(lab, sizeof lab, "%ld STEPS / %s", hi[1], bw);
         } else {
            (void)snprintf(lab, sizeof lab, "%ld STEPS", hi[1]);
         }
         const int lw = str_len(lab) * 6 * sc;
         draw_str(px, fb, px0 + pw - (4 * sc) - lw, py0 + (3 * sc), sc, lab,
                  excol[1]);
      }
   }

   /* ONE PASS PER SERIES, so a trace joins only its own entries.
    *
    * EVERY POINT IS MARKED, AT THE GLUCOSE PLOT'S OWN MARKER SIZE.
    *
    * A plot of a measurement has to show where the measurements ARE; a bare
    * line reads as something continuous rather than as a reading every five
    * minutes. And the size is not a fresh decision: this app draws one kind
    * of datapoint, and a step that was a different size from a glucose
    * reading would say the two are different kinds of thing.
    *
    * render_glucose derives 3*sc/2, and plot_render draws a point carrying its
    * own styling -- which every reading from a registered sensor does, i.e.
    * all of them in practice -- one radius larger, then scales by the
    * device's marker size over the default. So the mark a reader is actually
    * comparing these against is (3*sc/2 + 1) * size / 2, and that expression
    * is written out here rather than the number it currently comes to.
    *
    * AT THE DEFAULT SIZE, which is what a step count has: there is no device
    * behind these points whose marker a user could have restyled, so they
    * take MARK_SIZE_DEF. A sensor set to some other size will therefore draw
    * larger or smaller dots than these -- correctly, since that setting
    * exists to tell one sensor's trace from another's, and steps are neither.
    *
    * TWO SCALES, ONE FORMULA. `sc` here is this screen's and render_glucose's
    * is the main screen's; they agree on the test phone and need not on a
    * window short enough for the two fits to part company.
    *
    * Sizing them to the spacing instead was an invention: it made every tab's
    * dots a different size, which is the one thing a shared visual vocabulary
    * must not do. */
   const int mr = (((3 * sc) / 2) + 1) * MARK_SIZE_DEF / 2;
   for (int sx = 0; sx < 2; sx++) {
      int prevx      = 0;
      int prevy      = 0;
      int have       = 0;
      for (int i = 0; i < n; i++) {
         if (((p[i].series == 1) ? 1 : 0) != sx || p[i].t < from ||
             p[i].t > now)
            continue;
         const int cx = log_px(&w, p[i].t, px0, pw, pad);
         const int cy = ex_py(p[i].v, hi[sx], py0, ph, pad_t, pad_b);
         if (have) {
            const int dx = cx - prevx;
            const int dy = cy - prevy;
            const int ax = dx < 0 ? -dx : dx;
            const int ay = dy < 0 ? -dy : dy;
            const int st = ax > ay ? ax : ay;
            for (int k = 1; k <= st && st > 0; k++)
               fill_rect(px, fb, prevx + ((dx * k) / st),
                         prevy + ((dy * k) / st), sc, sc, excol[sx]);
         }
         fill_rect(px, fb, cx - mr, cy - mr, (2 * mr) + 1, (2 * mr) + 1,
                   excol[sx]);
         prevx = cx;
         prevy = cy;
         have  = 1;
      }
   }

   /* SCRUB CURSOR: the same full-height rule and greyed marker the other
    * plots use, each placed on its own series' axis.
    *
    * EVERY POINT AT THE PICKED INSTANT IS MARKED, not just the one the finger
    * resolved to. The readout answers with BOTH series for a day -- that is
    * the whole reason this plot carries two -- so highlighting only one of
    * them left the reader matching a number against an unmarked curve, and
    * the marked curve was whichever ex_pick happened to reach first. On the
    * sub-day tabs there is only the step series, so this marks the one point
    * there is. */
   if (hilite >= 0 && hilite < n && p[hilite].t >= from &&
       p[hilite].t <= now) {
      const int cx = log_px(&w, p[hilite].t, px0, pw, pad);
      fill_rect(px, fb, cx, py0 + 1, 1, ph - 2, UI_LOG_CURSOR);
      for (int i = 0; i < n; i++) {
         if (p[i].t != p[hilite].t || p[i].t < from || p[i].t > now)
            continue;
         const int sx = (p[i].series == 1) ? 1 : 0;
         const int cy = ex_py(p[i].v, hi[sx], py0, ph, pad_t, pad_b);
         fill_rect(px, fb, cx - (3 * sc), cy - (3 * sc), 6 * sc, 6 * sc,
                   UI_HILITE);
      }
   }
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
   if (m->scr == SCR_EXLOG) {
      n = ex_points(m, pts, UI_LOG_PTS, &from, NULL, 0);
      return ex_pick(pts, n, from, m->now, plot_x, plot_w, sc, x);
   }
   if (m->scr == SCR_INSLOG)
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
