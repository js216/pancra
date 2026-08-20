// SPDX-License-Identifier: GPL-3.0
// uidev.c --- Devices, and the screens reached from one (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "font.h"
#include "ndk.h"
#include "plot.h"
#include "sensors.h" /* sensor types, kinds, marker enum */
#include "style.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimenu.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* THE DEVICES LIST'S GEOMETRY, settled once and handed to the row.
 *
 * render_devices was 299 lines, and ninety of them were one row: the state
 * text, the age, the marker glyph and the primary checkbox, all reading nine
 * local variables that the loop around them had computed. The row is its own
 * function now, and this is the loop's half of what they shared. */
struct dev_geo {
   int sc, lh, gh;               /* text scale, line pitch, glyph height */
   int x, rx;                    /* margins */
   int cbs, cbx, cbh, colx, vrx; /* the primary column: box, target, values */
};

/* ONE DEVICE ROW: its marker, its name, what it is doing, and -- for a CGM
 * when there is more than one -- the primary checkbox. Returns the y the next
 * row starts at. */
static int device_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h, const struct ui_sensor *s, int i,
                      const struct dev_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   char val[28]; /* state[4] + ' ' + ago[12], with room to spare */
   char ago[12];
   char abbuf[8]; /* dev_state_abbrev's fallback scratch */
   /* For a meter the age is its last SYNC, never its last fingerstick -- so
    * "SYNCED 2 M" means synced 2 min ago, not a datapoint 2 min old. The
    * sync time is persisted, so it survives a restart; if a meter has
    * genuinely never synced it reads NEVER rather than mislabelling a
    * datapoint age. */
   long agot = (s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
   fmt_ago(m->now, agot, ago, sizeof ago);
   int warm_clk =
       s->session_seconds > 0 && s->session_seconds < SENSOR_WARMUP_S;
   int warm_est = s->session_seconds == 0 && s->paired > 0 &&
                  m->now - s->paired < SENSOR_WARMUP_S;
   if (s->kind == KIND_CGM && s->last == 0 && (warm_clk || warm_est)) {
      /* Warmup: time REMAINING, not "NEVER" -- the wait is by design and
       * the countdown says when data starts. With the sensor's own clock
       * it is exact to the second (matching the official reader); off the
       * pairing instant it is an estimate, and the '~' says so. */
      /* WARM, like every other state here, then the countdown in the
       * place the age occupies on the other rows -- so the column still
       * lines up while warming up. */
      if (warm_clk) {
         long r = SENSOR_WARMUP_S - s->session_seconds;
         (void)snprintf(val, sizeof val, "WARM %d:%02d", (int)(r / 60),
                        (int)(r % 60));
      } else {
         long r = (s->paired + SENSOR_WARMUP_S - m->now) / 60;
         (void)snprintf(val, sizeof val, "WARM ~%dM", (int)r);
      }
   } else {
      (void)snprintf(val, sizeof val, "%s %s",
                     dev_state_abbrev(s->status, abbuf, sizeof abbuf), ago);
   }
   /* Three leading spaces: the second cell holds this device's plot marker
    * (its shape, colour and size), so the list answers "which trace is
    * which" at a glance. No '>' for the primary any more -- the PRIMARY
    * column at the right says that now, and two marks for one fact on one
    * row is noise. */
   /* Holds the three-space prefix plus a full label (sizeof s->label, which
    * grew to 20 for the long OneTouch default names) plus the terminator.
    * Undersizing it truncated the MAC tail that tells two meters apart. */
   char name[4 + sizeof s->label];
   (void)snprintf(name, sizeof name, "   %s", s->label);
   menu_row_at(fb, h, y, g->sc, g->lh, g->vrx, name, val,
               s->connected ? 0xFF33FF88 : 0xFFAAAAAA, MA_SENSOR, i);
   /* The PRIMARY checkbox, for a CGM whose session is not over. Recorded
    * AFTER the row's own target and inside it: ui_hit_idx scans backwards,
    * so the box wins its own rectangle while the rest of the row still
    * opens the device. A meter and an expired sensor get no box at all --
    * neither can own the big number, and sensor_set_primary refuses both,
    * so offering the control would be a lie. */
   /* CENTRED ON THE ROW'S GLYPH, and drawn at row height rather than at
    * letter height: the unticked box used to be a 5x7 outline in a grey
    * that the eye simply skipped past, so the only box anyone could see was
    * the ticked one -- and a radio column where only the current choice is
    * visible offers no choice at all. */
   if (s->kind == KIND_CGM && !cgm_expired(s)) {
      draw_checkbox(px, fb, g->cbx, y - ((g->cbs - g->gh) / 2), g->cbs, g->sc,
                    s->primary, s->primary ? 0xFF33FF88 : 0xFFAAAAAA);
      /* The target is the box's own rectangle, not a fixed line height:
       * the box is taller than a line now, and a target that stopped short
       * of it would leave its bottom edge dead. It still ends well above
       * the next row's target (pitch 24*g->sc), so no row is stolen. */
      add_hit_ix(h, g->cbh, y - ((g->cbs - g->gh) / 2), fb->width - g->cbh,
                 g->cbs, MA_PRIM_PICK, i);
   }
   if (s->marker != MARK_HIDE) { /* hidden-from-plot draws no glyph */
      /* Centred in the reserved cell (text starts at 4*g->sc; the cell is the
       * second character, 6*g->sc wide). Radius follows the configured SIZE,
       * clamped to the cell so a large marker cannot strike the label. */
      int gr = (2 * g->sc * s->size) / MARK_SIZE_DEF;
      if (gr < g->sc)
         gr = g->sc;
      if (gr > 3 * g->sc)
         gr = 3 * g->sc;
      plot_marker_glyph((struct plot_fb){px, fb->stride, fb->width, fb->height},
                        (4 * g->sc) + (9 * g->sc), y + (3 * g->sc), gr,
                        s->marker, ui_sensor_color(s->color));
   }
   /* PITCH, not g->lh: the half-line of air that separates one device from the
    * next. ui_sensor_capacity divides by this same macro. */
   y += UI_DEV_PITCH(g->sc);
   return y;
}

/* THE PARAGRAPH ABOVE THE LIST, wrapped at runtime to the full text width.
 *
 * Its own function because it is a wrapper, not a layout: forty lines of
 * character arithmetic sitting in the middle of a screen renderer, sharing
 * nothing with it but `y`. The lines used to be hand-broken against a guessed
 * column budget, and the guess was a quarter too narrow on every geometry --
 * measuring beats guessing, and it self-corrects instead of being right once.
 */
static int devices_explainer(struct ANativeWindow_Buffer *fb, int x, int rx,
                             int sc, int gh, int y)
{
   uint32_t *px = fb->bits;
   /* FIVE LINES AT MOST, which is what UI_DEV_ABOVE budgets (see ui.h).
    * The narrowest this ever wraps to is the width-bound case, sc =
    * w/(33*6), where cols comes out 31 -- keep it inside five lines there
    * or the ADD NEW DEVICE button pays for the sixth. */
   /* "THE BOX AT THE RIGHT OF A ROW", not "a box". Every row carries TWO
    * squares -- the coloured plot marker at the left and the checkbox at
    * the right -- so "a box" makes the reader pick between them, and the
    * marker is the one their eye lands on first. Naming the side settles
    * it in four words. */
   static const char pexp[] =
       "WITH TWO OR MORE CGMS, TAP THE BOX AT THE RIGHT OF A ROW TO MAKE "
       "IT THE BIG NUMBER. ALARMS WATCH THEM ALL ANYWAY. METERS AND ENDED "
       "SESSIONS HAVE NO BOX.";
   int cols = (rx - x) / (6 * sc);
   if (cols < 8)
      cols = 8; /* degenerate width: draw something rather than loop */
   int i = 0;
   while (pexp[i]) {
      /* Take up to `cols` characters, then back off to the last space so a
       * word is never split. A single word longer than the line is cut at
       * the boundary rather than dropped. */
      int take = 0;
      int last = 0;
      while (pexp[i + take] && take < cols) {
         if (pexp[i + take] == ' ')
            last = take;
         take++;
      }
      /* Back off ONLY when the cut would land mid-word. A line that ends
       * exactly where a word does is already whole, and backing off there
       * threw away a word that fitted -- "TO SWITCH." fell to the next
       * line at 38 columns while measuring 38 columns wide. */
      if (pexp[i + take] && pexp[i + take] != ' ' && last > 0)
         take = last;
      char ln[64];
      int n = take < (int)(sizeof ln) - 1 ? take : (int)(sizeof ln) - 1;
      for (int k = 0; k < n; k++)
         ln[k] = pexp[i + k];
      ln[n] = 0;
      draw_str(px, fb, x, y, sc, ln, 0xFF888888);
      y += gh + (2 * sc);
      i += take;
      while (pexp[i] == ' ')
         i++;
   }
   y += 2 * sc;
   return y;
}

void render_devices(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Bounded by height as well as width -- see ui_devices_scale, which is
    * derived from the SAME expression ui_sensor_capacity uses. */
   int sc  = ui_devices_scale(fb->width, fb->height);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int gh  = 7 * sc; /* a label glyph is 7 rows tall */
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DEVICES", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_DEVICES_BACK, 0);
   y += 3 * lh; /* the DISPLAY menu's title gap -- the house style */

   /* THE PRIMARY COLUMN'S GEOMETRY, settled before anything that has to keep
    * clear of it: the row values below stop short of it.
    *
    * NO HEADER. The column was headed "PRIMARY", then "P", then "PRIM", each
    * time trying to make one word carry a rule that takes a sentence -- and
    * the box is now big enough to read as a control on its own, which is what
    * a header over a single column of checkboxes was really for. The
    * paragraph above says what the boxes do; a word repeating a fragment of
    * it is noise, and it cost the rows a column of width. So the column is
    * exactly the box wide, and nothing else. */
   int cbs  = 15 * sc; /* the box: a real square */
   int colx = rx - cbs;
   int cbx  = colx;
   int cbh  = colx - (2 * sc); /* its tap target starts here */
   /* A FULL CHARACTER CELL of air between the row's value and the box, on top
    * of the 4*sc that only kept them from touching. The value ends in a unit
    * letter ("119 S") and the box is a bright square: at four units apart the
    * two read as one run, and the eye had to separate a countdown from a
    * control. 6*sc is the same cell the rest of this screen spaces by. */
   int vrx = colx - (4 * sc) - (6 * sc); /* right edge left to row values */

   /* WHAT THE BOXES ARE FOR. This paragraph is now the column's ONLY label,
    * so it has to answer all three questions on its own: when the choice
    * matters (only with more than one CGM), what it changes (which reading is
    * the big number), and what it does NOT change (the alarms, which watch
    * every sensor whatever is picked -- without that line, choosing feels
    * like a risk). The last sentence explains the rows with no box, which
    * reads as a bug until you know the rule. */
   /* WRAPPED AT RUNTIME to the FULL text width, margin to margin.
    *
    * Not to the box column: the paragraph sits entirely ABOVE the first row,
    * so nothing is ever drawn beside it and there is nothing to keep clear
    * of. Stopping it at the column cost five characters a line and bought
    * nothing -- it was a habit picked up from the row layout below, where the
    * boxes really are alongside.
    *
    * The lines were hand-broken against a guessed column budget, and the guess
    * was badly low: it assumed sc == w/(33*6) exactly, but that divide FLOORS,
    * so the real sc is smaller and about a quarter more characters fit. The
    * paragraph ran short of the edge on every screen and needed a line it did
    * not need. Measuring beats guessing, and it self-corrects on any geometry
    * instead of being right on one. */
   y = devices_explainer(fb, x, rx, sc, gh, y);

   /* The row the column header used to occupy, kept EMPTY. It is the air that
    * separates the paragraph from the first device, and the list read as one
    * more line of prose without it. Kept as a row rather than shrunk to a gap
    * so UI_DEV_ABOVE still counts exactly what the renderer spends.
    *
    * Below it: a checkbox per eligible CGM at the right edge, with rows
    * keeping their value text clear (vrx) so the two never overprint. Exactly
    * one box is ever solid -- it is a radio choice spelled as checkboxes,
    * because "which one owns the big number" reads better as a column than as
    * a value buried on one row. */
   y += lh;

   int cap = ui_sensor_capacity(fb->width, fb->height);
   if (cap < UI_MIN_SLOTS) {
      /* Too short a screen to show even the minimum honestly. Say so rather
       * than silently truncating, which would read as "these are all of them".
       */
      draw_str(px, fb, x, y, sc, "SCREEN TOO SHORT", 0xFF4466FF);
      y += lh;
      draw_str(px, fb, x, y, sc, "FOR DEVICE LIST", 0xFF4466FF);
      return;
   }
   /* PAGINATE the live list, the same way OLD DEVICES does.
    *
    * This used to draw the first `cap` devices and then a red "N MORE NOT
    * SHOWN" row -- honest about the truncation, but it left those devices
    * genuinely UNREACHABLE: there is no scrolling anywhere in this UI, so a
    * device past the cut had no row, no tap target and no way to be opened,
    * renamed, calibrated or forgotten. Naming the problem is not the same as
    * not having it. Collect the live indices first, then show one page. */
   int idxs[UI_MAX_SLOTS];
   int nlive_i = 0;
   int nold    = 0;
   for (int i = 0; i < m->dev.nsensors; i++) {
      if (m->dev.sensors[i].old)
         nold++;
      else if (nlive_i < UI_MAX_SLOTS)
         idxs[nlive_i++] = i;
   }
   int npages = (nlive_i + cap - 1) / cap;
   if (npages < 1)
      npages = 1;
   int page = m->dev.dev_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int first = page * cap;
   if (first < 0)
      first = 0; /* unreachable: cap >= UI_MIN_SLOTS above, so page >= 0 */
   for (int pi = first; pi < first + cap && pi < nlive_i; pi++) {
      struct dev_geo g = {sc, lh, gh, x, rx, cbs, cbx, cbh, colx, vrx};
      y = device_row(fb, m, h, &m->dev.sensors[idxs[pi]], idxs[pi], &g, y);
   }
   if (m->dev.pend_type > 0) {
      /* An ARMED pairing: registered intent, no sensor on the air yet. The
       * row is the visible promise that the code was accepted and the app is
       * watching -- and the tap is the way to change one's mind. */
      char pn[24];
      (void)snprintf(pn, sizeof pn, " %s", sensor_type_name(m->dev.pend_type));
      menu_row(fb, h, y, sc, lh, pn, "PENDING...", 0xFF00CCFF, MA_PEND_CANCEL,
               0);
      y += lh;
   }
   int nlive = m->dev.nsensors - nold;
   /* PAGE NAV, in place of the old "N MORE NOT SHOWN". Same shape as OLD
    * DEVICES: "<" and ">" with the page count between them, drawn only when
    * there IS more than one page so a short list stays quiet. */
   if (npages > 1) {
      if (page > 0) {
         draw_str(px, fb, x, y, sc, "<", 0xFFFFFFFF);
         add_hit_ix(h, 0, y - (3 * sc), fb->width / 3, lh, MA_DEVPAGE_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, y, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * sc), y, sc, ">", 0xFFFFFFFF);
         add_hit_ix(h, (2 * fb->width) / 3, y - (3 * sc), fb->width / 3, lh,
                    MA_DEVPAGE_NEXT, 0);
      }
      y += lh;
   }
   /* OLD DEVICES: DISCONNECTED devices. Each keeps its full slot, so the row
    * opens the SAME per-device menu (state EXPIRED). Only shown when there is
    * at least one, so it never adds noise on a fresh install. A blank line
    * either side: it is a door to a different list, not another device, and
    * without the air it read as one more row of the list above it. */
   if (nold > 0) {
      char od[32];
      (void)snprintf(od, sizeof od, "OLD DEVICES (%d)", nold);
      y += lh;
      menu_row(fb, h, y, sc, lh, od, ">", 0xFFAAAAAA, MA_OLDDEV_OPEN, 0);
      y += 2 * lh;
   }
   if (nlive < UI_MAX_SLOTS) {
      /* A real framed button, like SYNC NOW / FORGET DEVICE, not a plain row.
       */
      y += lh; /* separate it from the device list above */
      menu_button(fb, h, x, y, fb->width - (2 * x), sc, "ADD NEW DEVICE",
                  0xFFFFFFFF, MA_ADDSENSOR, 0);
   }
}

/* ---- permissions + background controls (opened from SETTINGS) ---- */

void render_perms(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "PERMISSIONS", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_PERMS_BACK, 0);
   /* 3*lh after the title and 2*lh between rows -- the DISPLAY menu's spacing,
    * which is the house style for a settings submenu. This screen was the only
    * one still packed at the bare line height, so six rows of GRANTED / DENIED
    * ran together into a block that had to be read word by word to find the
    * one that was wrong. It fits: at 22 rows the budget already covered a
    * longer list than this one, and DISPLAY carries more rows at exactly this
    * spacing. */
   y += 3 * lh;

   for (int i = 0; i < 3; i++) {
      int g = m->sys.perm[i];
      menu_row(fb, h, y, sc, lh, ui_perm_lbl[i], g ? "GRANTED" : "DENIED",
               g ? 0xFF33FF88 : 0xFF4466FF, MA_PERM, i);
      y += 2 * lh;
   }
   menu_row(fb, h, y, sc, lh, "BATTERY",
            m->sys.batt_ok ? "UNRESTRICTED" : "OPTIMIZED",
            m->sys.batt_ok ? 0xFF33FF88 : 0xFF4466FF, MA_BATTERY, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "STANDBY", ui_bucket_label(m->sys.standby_bucket),
            (m->sys.standby_bucket > 0 && m->sys.standby_bucket <= 20)
                ? 0xFF33FF88
                : 0xFFAA8844,
            -1, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "BG EXEC",
            m->sys.bg_restricted ? "RESTRICTED" : "ALLOWED",
            m->sys.bg_restricted ? 0xFF4466FF : 0xFF33FF88, MA_BGEXEC, 0);
}

/* ---- ALARM submenu (opened from SETTINGS, or straight from the main
 * screen's alarm row left of "LOW") ---- */

/* A section caption: grey, no value, NO HIT BOX. It must not be tappable --
 * a target that looks like a row and does nothing teaches the user that rows
 * here sometimes do nothing, which is the last thing an alarm screen should
 * teach. menu_row with a negative code records none. */

/* THE ONE SCREEN'S GEOMETRY, passed rather than recomputed.
 *
 * render_sensor was 399 lines: four sections -- identity, live status, device
 * info and the action buttons -- sharing eight local variables and one
 * running `y`. Each is now its own function, and this is what they share. A
 * struct rather than eight parameters, because the parameter list is what
 * makes a helper too tedious to split out in the first place. */
struct sensor_geo {
   int sc;  /* text scale */
   int lh;  /* line pitch */
   int x;   /* left margin */
   int rx;  /* right edge for right-aligned values */
   int tsc; /* title scale */
};

/* WHAT THIS DEVICE IS and how the user has styled it: the rows that are
 * settings rather than readings. */
static int sensor_identity_rows(struct ANativeWindow_Buffer *fb,
                                const struct screen *m, struct hits *h,
                                const struct ui_sensor *s,
                                const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* Identity: type + name (+ PRIMARY for a CGM), no section title. */
   menu_row(fb, h, y, g->sc, g->lh, "TYPE", sensor_disp_name(s->type),
            0xFFFFFFFF, -1, 0);
   y += g->lh;
   menu_row(fb, h, y, g->sc, g->lh, "NAME", s->label, 0xFFFFFFFF, MA_LABEL, 0);
   y += g->lh;
   if (s->kind == KIND_CGM && !s->old) {
      /* PRIMARY only for a LIVE CGM -- a disconnected one cannot own the big
       * number. */
      menu_row(fb, h, y, g->sc, g->lh, "PRIMARY", s->primary ? "YES" : "NO",
               s->primary ? 0xFF33FF88 : 0xFFFFFFFF, MA_PRIMARY, 0);
      y += g->lh;
   }
   /* One MARKER row -- shows the ACTUAL glyph (in the device's colour), not a
    * name; shape + size + colour all live in its combined menu. */
   draw_str(px, fb, g->x, y, g->sc, "MARKER", 0xFFCCCCCC);
   if (s->marker == MARK_HIDE) {
      int lw = str_len("OFF") * 6 * g->sc;
      draw_str(px, fb, g->rx - lw, y, g->sc, "OFF", 0xFFAAAAAA);
   } else {
      /* Glyph reflects the configured SIZE too (same scaling as the plot). */
      int gr = (2 * g->sc * s->size) / MARK_SIZE_DEF;
      if (gr < g->sc)
         gr = g->sc;
      if (gr > 5 * g->sc)
         gr = 5 * g->sc;
      plot_marker_glyph((struct plot_fb){px, fb->stride, fb->width, fb->height},
                        g->rx - (6 * g->sc), y + (3 * g->sc), gr, s->marker,
                        ui_sensor_color(s->color));
   }
   add_hit_ix(h, 0, y - (3 * g->sc), fb->width, g->lh, MA_MARKER, 0);
   y += g->lh;
   return y;
}

/* WHAT IT IS DOING: session state, the clocks, and the last calibration.
 * Read-only -- every row here is the sensor's own account of itself. */
static int sensor_status_rows(struct ANativeWindow_Buffer *fb,
                              const struct screen *m, struct hits *h,
                              const struct ui_sensor *s,
                              const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* --- read-only --- */
   y += g->lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, g->x, y, g->sc, "STATUS", 0xFF888888);
   y += g->lh;
   /* A disconnected device reads EXPIRED (red); otherwise its live status. */
   uint32_t stcol = 0xFFAAAAAA;
   if (s->old)
      stcol = 0xFF4466FF;
   else if (s->connected)
      stcol = 0xFF33FF88;
   menu_row(fb, h, y, g->sc, g->lh, "STATE", s->old ? "EXPIRED" : s->status,
            stcol, -1, 0);
   y += g->lh;
   {
      char rs[16]; /* link RSSI (moved off the main screen). No age here -- LAST
                    * SYNC sits right beside it and carries the time. */
      if (s->rssi_ok && s->rssi_t > 0)
         (void)snprintf(rs, sizeof rs, "%d DB", s->rssi);
      else
         (void)snprintf(rs, sizeof rs, "--");
      menu_row(fb, h, y, g->sc, g->lh, "SIGNAL STRENGTH", rs, 0xFFFFFFFF, -1,
               0);
      y += g->lh;
   }
   {
      /* LAST SEEN: the most recent time we heard from this device -- a meter's
       * last connect/sync, a CGM's last reading. Directly under SIGNAL
       * STRENGTH, whose value is the signal captured at that same moment. */
      char when[20];
      char rel[12];
      char val[48]; /* "<date up to 19> (<rel up to 11> AGO)" + NUL */
      long seen = (s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
      if (seen > 0) {
         fmt_date(seen, m->tz_off, when, sizeof when);
         fmt_ago(m->now, seen, rel, sizeof rel);
         (void)snprintf(val, sizeof val, "%s (%s AGO)", when, rel);
      } else {
         (void)snprintf(val, sizeof val, "--");
      }
      menu_row(fb, h, y, g->sc, g->lh, "LAST SEEN", val, 0xFFFFFFFF, -1, 0);
      y += g->lh;
      /* A meter's fingerstick time is DISTINCT from its sync, so it keeps a
       * separate LAST DATA row; a CGM's LAST SEEN already IS its data time. */
      if (s->kind == KIND_BGM) {
         if (s->last > 0) {
            fmt_date(s->last, m->tz_off, when, sizeof when);
            fmt_ago(m->now, s->last, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "%s (%s AGO)", when, rel);
         } else {
            (void)snprintf(val, sizeof val, "--");
         }
         menu_row(fb, h, y, g->sc, g->lh, "LAST DATA", val, 0xFFFFFFFF, -1, 0);
         y += g->lh;
      }
   }
   if (s->kind == KIND_CGM) {
      /* Holds "<value> <unit>" for the PRED row: fmt_glu into a 12-byte buffer
       * (up to 11 chars, as the compiler sees it) + ' ' + a 6-char unit + NUL.
       */
      char b[24];
      /* Only show session timing once a real session is known. Before the first
       * reading session_seconds is 0, which otherwise renders as "started 0s
       * ago, ends in 15 days" -- misleading, so show "--" instead. */
      char when[20];
      char rel[12];
      char val[36];
      /* An OLD device has no live session clock, so its STARTED/ENDS/ELAPSED
       * come from the PERSISTED activation instant instead of `now - clock`.
       * A live one uses the running clock as before. */
      int have_session = 0;
      long began       = 0;
      if (s->old) {
         have_session = (s->activation > 0);
         began        = s->activation;
      } else {
         have_session = (s->session_seconds > 0);
         began        = m->now - s->session_seconds;
      }
      long len = s->wear_len; /* per-device: override / model / type */
      /* STARTED shows the absolute instant only. The relative age lives in the
       * ELAPSED row, so a parenthetical "(N AGO)" here was pure duplication. */
      if (have_session)
         fmt_date(began, m->tz_off, when, sizeof when);
      else
         (void)snprintf(when, sizeof when, "--");
      menu_row(fb, h, y, g->sc, g->lh, "STARTED", when, 0xFFFFFFFF, -1, 0);
      y += g->lh;
      if (len > 0) {
         /* ENDS shows the absolute instant only; REMAINING (below) carries the
          * relative countdown, mirroring STARTED/ELAPSED. */
         if (have_session)
            fmt_date(began + len, m->tz_off, when, sizeof when);
         else
            (void)snprintf(when, sizeof when, "--");
         menu_row(fb, h, y, g->sc, g->lh, "ENDS", when,
                  (have_session && began + len < m->now) ? 0xFF4466FF
                                                         : 0xFFFFFFFF,
                  -1, 0);
         y += g->lh;
      }
      /* ELAPSED: a live device's running clock; an old device's final run
       * (last reading minus its start), which is how long it actually lasted.
       */
      long elapsed = s->session_seconds;
      if (s->old)
         elapsed = (s->last > began) ? s->last - began : len;
      if (have_session)
         fmt_dur(elapsed, b, sizeof b);
      else
         (void)snprintf(b, sizeof b, "--");
      menu_row(fb, h, y, g->sc, g->lh, "ELAPSED", b, 0xFFFFFFFF, -1, 0);
      y += g->lh;
      /* REMAINING: relative time to session end, replacing the old ENDS
       * parenthetical. EXPIRED (red) once the session length is exceeded. */
      if (len > 0) {
         long ends     = began + len;
         uint32_t rcol = 0xFFFFFFFF;
         if (s->old) {
            /* A disconnected device is done -- no countdown, just EXPIRED. */
            (void)snprintf(val, sizeof val, "EXPIRED");
            rcol = 0xFF4466FF;
         } else if (!have_session) {
            (void)snprintf(val, sizeof val, "--");
         } else if (ends >= m->now) {
            /* Imminence carries colour here too: YELLOW inside the last day
             * (fmt_dur already switches to hours + minutes there), RED inside
             * the final two hours. */
            long left = ends - m->now;
            if (left < 86400)
               rcol = (left < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
            fmt_dur(left, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "%s", rel);
         } else if (s->sess_state == SENSOR_STATE_ENDED) {
            /* The sensor's own verdict, same rule as the main screen. */
            (void)snprintf(val, sizeof val, "ENDED");
            rcol = 0xFF4466FF;
         } else {
            /* Past the nominal end: count the grace down -- and past the
             * grace, KEEP counting into the negative (same rule as the main
             * screen's SESSION row): the sign says "past the hard stop"
             * while still showing by how much, which stays honest even when
             * the wear budget is set wrong for a sensor visibly alive. No
             * sign inside the first negative minute (never "-0 M"). */
            long gl = ends + SENSOR_GRACE_S - m->now;
            fmt_dur((gl < 0) ? -gl : gl, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "GRACE %s%s",
                           (gl <= -60) ? "-" : "", rel);
            rcol = (gl < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
         }
         menu_row(fb, h, y, g->sc, g->lh, "REMAINING", val, rcol, -1, 0);
         y += g->lh;
      }
      /* Same sentinel rule as the main screen's PRED row: `predicted` is a
       * 10-bit field whose 0x3ff (1023) means "no prediction", and no real
       * value exceeds Dexcom's 400 mg/dL cap. Without the upper bound this
       * row showed a bare 1023 while the main screen showed "--" for the same
       * sensor -- two answers to the same question, one of them a number the
       * user could act on. */
      if (s->predicted > 0 && s->predicted <= 400) {
         char pv[12];
         fmt_glu(s->predicted, m->prefs.units, pv, sizeof pv);
         (void)snprintf(b, sizeof b, "%s %s", pv, UI_LBL(m->prefs.units));
         menu_row(fb, h, y, g->sc, g->lh, "PRED", b, 0xFFFFFFFF, -1, 0);
         y += g->lh;
      }
      (void)snprintf(b, sizeof b, "%d", s->sequence);
      menu_row(fb, h, y, g->sc, g->lh, "SEQ", b, 0xFFFFFFFF, -1, 0);
      y += g->lh;
   }
   return y;
}

/* WHAT IT IS: the identifiers, which never change while it is worn. */
static int sensor_info_rows(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h,
                            const struct ui_sensor *s,
                            const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   y += g->lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, g->x, y, g->sc, "DEVICE INFO", 0xFF888888);
   y += g->lh;
   if (s->code[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "CODE", s->code, 0xFFFFFFFF, -1, 0);
      y += g->lh;
   }
   menu_row(fb, h, y, g->sc, g->lh, "MAC", s->mac[0] ? s->mac : "--",
            0xFFFFFFFF, -1, 0);
   y += g->lh;
   if (s->serial[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "SN", s->serial, 0xFFFFFFFF, -1, 0);
      y += g->lh;
   }
   if (s->model[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "SW", s->model, 0xFFFFFFFF, -1, 0);
      y += g->lh;
   }
   if (s->fw[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "FW", s->fw, 0xFFFFFFFF, -1, 0);
      y += g->lh;
   }
   if (s->kind == KIND_CGM) {
      /* WEAR belongs with the device facts: the nominal budget the
       * countdown judges against. Dexcom sells 10- and 15-day G7s that
       * are indistinguishable on the air, so when the auto-resolution
       * guesses wrong this row is the correction.
       *
       * AUTO IS NAMED, AND A PIN IS COLOURED. Both states used to print the
       * same bare "10 DAYS", so a device whose model says 15 and whose
       * override says 10 looked exactly like one correctly resolved to 10 --
       * the countdown was five days short with nothing on screen to explain
       * it. Green for a pin matches every other row here where green means
       * "the user changed this from the default". */
      char wd[24];
      int wdays = (int)(s->wear_len / 86400);
      if (s->wear_auto)
         (void)snprintf(wd, sizeof wd, "AUTO %d D", wdays);
      else
         (void)snprintf(wd, sizeof wd, "%d DAYS", wdays);
      menu_row(fb, h, y, g->sc, g->lh, "WEAR", wd,
               s->wear_auto ? 0xFFFFFFFF : 0xFF33FF88, MA_WEAR, 0);
      y += g->lh;
   }
   /* RESCALE: the active multiplicative correction as a signed percentage, or
    * (NONE). Tapping opens the value keypad, or -- if already active -- the
    * CHANGE / STOP screen. Sits just above LAST CAL. */
   if (s->kind == KIND_CGM) {
      char rv[32]; /* "PENDING " + value(<=11) + ' ' + unit(<=6) + NUL */
      uint32_t rcol = 0xFFFFFFFF;
      if (s->rescale_pending > 0) {
         char gv[12];
         fmt_glu(s->rescale_pending, m->prefs.units, gv, sizeof gv);
         (void)snprintf(rv, sizeof rv, "PENDING %s %s", gv,
                        UI_LBL(m->prefs.units));
         rcol = 0xFF44CCFF;
      } else if (s->rescale_rejected) {
         (void)snprintf(rv, sizeof rv, "REJECTED >25%%");
         rcol = 0xFF4466FF; /* red */
      } else if (s->rescale_expired) {
         (void)snprintf(rv, sizeof rv, "EXPIRED - RE-ENTER");
         rcol = 0xFF4466FF; /* red */
      } else if (s->rescale_pm != 1000) {
         int d = s->rescale_pm - 1000; /* tenths of a percent */
         int a = (d < 0) ? -d : d;
         (void)snprintf(rv, sizeof rv, "%c%d.%d%%", (d < 0) ? '-' : '+', a / 10,
                        a % 10);
         rcol = 0xFF44CCFF; /* amber: active */
      } else {
         (void)snprintf(rv, sizeof rv, "(NONE)");
      }
      /* AHEAD OF THE DISK. The factor is real -- it is scaling every reading
       * on this screen -- but the file has not taken it yet, and a restart
       * would show something else. Said in the row rather than left to be
       * discovered at the next launch; calib_tick keeps retrying, so this is
       * normally on screen for one tick.
       *
       * IT REPLACES THE VALUE RATHER THAN BEING APPENDED TO IT. The layout
       * targets UI_COLS (33) characters and this row already spends most of
       * them; " NOT SAVED" on the end of the longest value overflowed the
       * label -- and clipped to the row's own buffer it read "NOT SAVE",
       * which is worse than saying nothing. The value comes back the moment
       * the retry lands.
       *
       * ONLY WHEN THE ROW HAS SOMETHING TO LOSE: the flag is per-FILE, not
       * per-sensor, so a second CGM with no factor of its own would
       * otherwise announce that its "(NONE)" was unsaved. */
      if (s->rescale_unsaved &&
          (s->rescale_pending > 0 || s->rescale_rejected ||
           s->rescale_expired || s->rescale_pm != 1000)) {
         (void)snprintf(rv, sizeof rv, "NOT SAVED - RETRYING");
         rcol = 0xFF4466FF; /* red */
      }
      menu_row(fb, h, y, g->sc, g->lh, "RESCALE", rv, rcol, MA_RESCALE_OPEN, 0);
      y += g->lh;
   }
   /* LAST CAL: sits right above the CALIBRATION button so the outcome of the
    * last calibration is next to where you start a new one. Shows (NONE), a
    * PENDING queue entry, or the resolved outcome -- the accepted mg/dL value,
    * or FAIL. */
   if (s->kind == KIND_CGM) {
      char cv[40];
      char gv[12];
      uint32_t ccol = 0xFFFFFFFF;
      if (s->cal_pending > 0) {
         fmt_glu(s->cal_pending, m->prefs.units, gv, sizeof gv);
         (void)snprintf(cv, sizeof cv, "PENDING %s %s", gv,
                        UI_LBL(m->prefs.units));
         ccol = 0xFF44CCFF; /* amber: in progress */
      } else if (s->cal_t > 0) {
         char cd[20];
         fmt_date(s->cal_t, m->tz_off, cd, sizeof cd);
         if (s->cal_state == CAL_ST_APPLIED) {
            fmt_glu(s->cal_mgdl, m->prefs.units, gv, sizeof gv);
            (void)snprintf(cv, sizeof cv, "%s %s %s", cd, gv,
                           UI_LBL(m->prefs.units));
            ccol = 0xFF88FF33; /* green: accepted */
         } else {
            /* Distinct failure kinds so REJECTED (bad value) is not confused
             * with NOT SUPPORTED (sensor forbids calibration). */
            const char *w = "FAILED";
            if (s->cal_state == CAL_ST_REJECTED)
               w = "REJECTED";
            else if (s->cal_state == CAL_ST_NOTSUP)
               w = "NOT SUPPORTED";
            (void)snprintf(cv, sizeof cv, "%s %s", cd, w);
            ccol = 0xFF4466FF; /* red */
         }
      } else {
         (void)snprintf(cv, sizeof cv, "(NONE)");
      }
      /* See the RESCALE row above, for the replacement and for the guard: an
       * ACCEPTED or REJECTED verdict that could not be written stands (the
       * sensor cannot be asked twice), so the row says the next launch may
       * disagree -- but only on a row that is showing a verdict or a queue in
       * the first place. */
      if (s->cal_unsaved && (s->cal_pending > 0 || s->cal_t > 0)) {
         (void)snprintf(cv, sizeof cv, "NOT SAVED - RETRYING");
         ccol = 0xFF4466FF; /* red */
      }
      /* While a calibration is queued, the row itself is a shortcut into the
       * CAL PENDING menu (REPLACE / DELETE); otherwise it is display-only. */
      menu_row(fb, h, y, g->sc, g->lh, "LAST CAL", cv, ccol,
               s->cal_pending > 0 ? MA_CAL_OPEN : -1, 0);
      y += g->lh;
   }
   return y;
}

/* THE ACTIONS, kept together at the bottom and framed, with the destructive
 * one well clear of the others rather than one fat finger below. */
static int sensor_action_buttons(struct ANativeWindow_Buffer *fb,
                                 const struct screen *m, struct hits *h,
                                 const struct ui_sensor *s,
                                 const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* --- actions as framed buttons, kept together at the bottom --- */
   int bw = fb->width - (2 * g->x);
   y += 2 * g->lh;
   if (s->old) {
      /* A DISCONNECTED device: no live actions (calibrate/sync need a link).
       * RECONNECT revives it -- sensible for a sensor pulled BEFORE it expired
       * (still within its wear window). The handler shows a confirmation first
       * when the sensor is already expired, since reconnecting a dead sensor
       * rarely makes sense. */
      menu_button(fb, h, g->x, y, bw, g->sc, "RECONNECT", 0xFF00FF00,
                  MA_RECONNECT, 0);
      return y;
   }
   if (s->kind == KIND_CGM)
      y = menu_button(fb, h, g->x, y, bw, g->sc, "CALIBRATION", 0xFFFFFFFF,
                      MA_CAL_OPEN, 0);
   else
      y = menu_button(fb, h, g->x, y, bw, g->sc, "SYNC NOW", 0xFFFFFFFF,
                      MA_SYNC, 0);
   /* DISCONNECT is destructive: red, and well clear of the action above it
    * rather than one fat finger below. It only opens a confirmation. */
   y += 2 * g->lh;
   menu_button(fb, h, g->x, y, bw, g->sc, "DISCONNECT", 0xFF0000FF, MA_FORGET,
               0);
   return y;
}

void render_sensor(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 25 rows: 24 rows' worth of y advances at their maximum (every optional
    * attribute present, which is the normal state once DIS has answered) plus
    * the FORGET row itself. Width-only scaling pushed FORGET SENSOR below the
    * buffer on 16:9 phones, so the one destructive action was unreachable
    * exactly when every attribute row was populated. */
   /* Font size (sc) must match the other menus' NORMAL text, not shrink to fit
    * the many rows. Sizing for 32 rows at the usual 16*sc pitch floored sc to 2
    * (settings gets 3), which read as microscopic. Size instead for 28 rows and
    * use a tighter 14*sc pitch: 28*16 == 32*14, so the same content still fits,
    * but sc lands on the normal value. */
   /* 31, not 28. The worst case is every optional attribute present -- which
    * is the NORMAL state once DIS has answered and a pairing code is stored:
    * ENDS, REMAINING, PRED, SN and CODE all appear, 31 rows at the 14*sc
    * pitch. 28 was about two rows short before the shared button padding grew
    * and three after, so on 1080x1920, 1080x2400 and 2560x1440 the DISCONNECT
    * button -- the one destructive action -- was drawn entirely below the
    * buffer and could not be tapped. That is exactly the failure the note
    * above says this sizing exists to prevent; the number just never kept up
    * with the rows. */
   /* 31 IS A CEILING, NOT A BUDGET TO GROW. Raising it to 32 for one more row
    * dropped ui_fit_scale from 3 to 2 on a 720x1600 phone -- the exact cliff
    * render_remote's comment warns about -- and shrank this entire screen's
    * text to unreadable. Nothing here may claim another row: a new fact must
    * ride on a row that already exists (see the PAIRING state folded into
    * SESSION below). */
   int sc  = ui_fit_scale(fb->width, fb->height, 31);
   int tsc = 2 * sc;
   int lh  = 14 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   /* Record the way OUT before the range guard.
    *
    * Returning early left this screen with ZERO hit targets -- ui_render has
    * already cleared them, and on_input swallows every tap while a menu is
    * open. A stale selection (MA_SENSOR_BACK sets sel = -1, and several paths
    * then re-open a menu without setting it) therefore produced a blank screen
    * that ignored all input: force-stop required. */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_SENSOR_BACK, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   draw_str(px, fb, x, y, tsc, s->label, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* No second registration here: the one above the range guard covers
    * the identical rect with the identical action, so this was a leftover
    * from before that fix -- a duplicate target that burns a slot of the
    * UI_MAX_HITS budget and shows up in any reachability audit as a
    * shadowed control, masking a real one. */
   y += 2 * lh;

   struct sensor_geo geo = {sc, lh, x, rx, tsc};
   y                     = sensor_identity_rows(fb, m, h, s, &geo, y);
   y                     = sensor_status_rows(fb, m, h, s, &geo, y);
   y                     = sensor_info_rows(fb, m, h, s, &geo, y);
   sensor_action_buttons(fb, m, h, s, &geo, y);
}

/* ---- calibration confirmation ---- */

void render_cal(struct ANativeWindow_Buffer *fb, const struct screen *m,
                struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 25 rows: 24 rows' worth of y advances at their maximum (every optional
    * attribute present, which is the normal state once DIS has answered) plus
    * the FORGET row itself. Width-only scaling pushed FORGET SENSOR below the
    * buffer on 16:9 phones, so the one destructive action was unreachable
    * exactly when every attribute row was populated. */
   int sc  = ui_fit_scale(fb->width, fb->height, 25);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   /* Record the way OUT before the range guard.
    *
    * Returning early left this screen with ZERO hit targets -- ui_render has
    * already cleared them, and on_input swallows every tap while a menu is
    * open. A stale selection (MA_SENSOR_BACK sets sel = -1, and several paths
    * then re-open a menu without setting it) therefore produced a blank screen
    * that ignored all input: force-stop required. */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_CAL_BACK, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   /* Confirmation for the value just typed on the keypad. The write is the most
    * consequential in the app, so it happens ONLY on CONFIRM below. */
   draw_str(px, fb, x, y, tsc, "CONFIRM", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* No second registration here: the one above the range guard covers
    * the identical rect with the identical action, so this was a leftover
    * from before that fix -- a duplicate target that burns a slot of the
    * UI_MAX_HITS budget and shows up in any reachability audit as a
    * shadowed control, masking a real one. */
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1, 0);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->cal.cal_pending, m->prefs.units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->prefs.units));
      menu_row(fb, h, y, sc, lh, "CALIBRATE TO", v, 0xFF33FF88, -1, 0);
      y += lh;
   }
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_CAL_BACK, 0);
   y += 3 * lh; /* wide gap so CONFIRM is deliberate */
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88, MA_CAL_ENTER, 0);
}

/* Confirm a rescale: shows the target value and the clamped percentage, applied
 * only on CONFIRM. Mirrors render_cal. */
void render_rescale(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_RESCALE_BACK, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];
   draw_str(px, fb, x, y, tsc, "RESCALE", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* Title gap and row spacing follow the DISPLAY menu, like every other
    * settings screen: these three rows were packed at the bare line height
    * and read as one paragraph rather than three separate facts. */
   y += 3 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1, 0);
   y += 2 * lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->cal.rescale_entry, m->prefs.units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->prefs.units));
      menu_row(fb, h, y, sc, lh, "TARGET", v, 0xFF33FF88, -1, 0);
      y += 2 * lh;
   }
   {
      char v[20];
      uint32_t vc = 0xFF44CCFF;
      if (m->cal.rescale_pm == 0) {
         /* No reading yet to compute against -- do not show a bogus 0%. */
         (void)snprintf(v, sizeof v, "ON NEXT READING");
         vc = 0xFFAAAAAA;
      } else {
         fmt_rescale_pct(m->cal.rescale_pm, v, sizeof v);
         /* Beyond +-25% will be REJECTED on CONFIRM -- flag it red. */
         if (m->cal.rescale_pm < 750 || m->cal.rescale_pm > 1250)
            vc = 0xFF4466FF;
      }
      menu_row(fb, h, y, sc, lh, "RESCALE BY", v, vc, -1, 0);
      y += 2 * lh;
   }
   y += lh;
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_RESCALE_BACK,
                   0);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88, MA_RESCALE_ENTER, 0);
}

/* ---- ADD menu: the main-screen '+' lands here ---- */

/* THE PINNABLE-ACTION TABLE, in ADD-menu order.
 *
 * These are the actions the ADD menu's PIN column offers, and the ones the
 * main screen draws as buttons when pinned.
 *
 * The device types are deliberately absent. A pin is for something done
 * often -- logging a dose, logging a weight, glancing at a log -- and adding a
 * device is done a handful of times in a sensor's life, from a menu that also
 * asks for a pairing code. Promoting it would spend one of three scarce slots
 * on the rarest action here.
 *
 * `abbrev` must fit a third of the '+' row, so keep them to seven characters.
 * They are NOT generated by truncating the full label: "VIEW INSULIN LOG" cut
 * to seven is "VIEW IN", which names nothing. */

void render_olddev(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "OLD DEVICES", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_OLDDEV_BACK, 0);
   y += 3 * lh;

   /* Collect the old slots' indices (into m->dev.sensors) in list order. */
   int idxs[UI_MAX_SLOTS];
   int nold = 0;
   for (int i = 0; i < m->dev.nsensors && nold < UI_MAX_SLOTS; i++)
      if (m->dev.sensors[i].old)
         idxs[nold++] = i;
   if (nold <= 0) {
      draw_str(px, fb, x, y, sc, "None yet. Disconnected", 0xFF888888);
      y += lh;
      draw_str(px, fb, x, y, sc, "devices appear here.", 0xFF888888);
      return;
   }

   /* Rows that fit between the title and a reserved bottom nav line. Each row
    * takes 2*lh (a blank line between). At least one, always. */
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
   int avail = fb->height - y - (2 * lh); /* reserve the nav line */
   int per   = avail / (2 * lh);
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (nold + per - 1) / per;
   int page   = m->dev.old_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int start = page * per;

   for (int r = start; r < start + per && r < nold; r++) {
      const struct ui_sensor *s = &m->dev.sensors[idxs[r]];
      char val[20];
      long agot = (s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
      if (agot > 0)
         fmt_date(agot, m->tz_off, val, sizeof val);
      else
         (void)snprintf(val, sizeof val, "--");
      char name[3 + sizeof s->label];
      (void)snprintf(name, sizeof name, "  %s", s->label);
      menu_row(fb, h, y, sc, lh, name, val, 0xFFAAAAAA, MA_SENSOR, idxs[r]);
      if (s->marker != MARK_HIDE) {
         int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height},
             (4 * sc) + (3 * sc), y + (3 * sc), gr, s->marker,
             ui_sensor_color(s->color));
      }
      y += 2 * lh; /* blank line between rows */
   }

   /* Bottom navigation, only when there is more than one page. "<" and ">"
    * are generous tap targets on the left and right; the page count is
    * centred between them. */
   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", 0xFFFFFFFF);
         add_hit_ix(h, 0, navy - (3 * sc), fb->width / 3, lh + (7 * sc),
                    MA_OLDPAGE_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (1 * 6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit_ix(h, (2 * fb->width) / 3, navy - (3 * sc), fb->width / 3,
                    lh + (7 * sc), MA_OLDPAGE_NEXT, 0);
      }
   }
}

/* ---- MARKER / COLOR pickers: a full list of options, each with a live glyph,
 * so the user sees every choice before selecting. The title row returns to the
 * device's own menu (MA_SENSOR + its slot). ---- */

void render_devlist(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded (see ui_fit_scale). Left on
    * width-only scaling, this screen's controls were laid out past the
    * bottom in landscape -- and render_forget records no close target, so
    * it became a dead end with no way back. */
   int sc  = ui_fit_scale(fb->width, fb->height, 26);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   char sel_title[24];
   if (m->dev.add_kind == KIND_BGM)
      (void)snprintf(sel_title, sizeof sel_title, "SELECT METER");
   else
      (void)snprintf(sel_title, sizeof sel_title, "SELECT %s",
                     m->dev.add_type ? m->dev.add_type : "SENSOR");
   (void)draw_title_fit(px, fb, x, y, tsc, sel_title, 0xFFFFFFFF,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 8 * tsc, MA_DEV_CANCEL, 0);
   y += 2 * lh;

   if (m->dev.ndev <= 0) {
      draw_str(px, fb, x, y, sc, "Searching for sensors...", 0xFF888888);
      return;
   }
   draw_str(px, fb, x, y, sc, "Nearest first -- tap yours:", 0xFF888888);
   y += 2 * lh;

   /* selection sort by RSSI, strongest first (the model owns index -> device)
    */
   int order[16];
   /* devs may be null with a stale ndev; every other list here is guarded the
    * same way. The renderer is a pure function of the model and must not
    * trust two fields to agree. */
   int n = (m->dev.devs && m->dev.ndev > 0) ? m->dev.ndev : 0;
   if (n > 16)
      n = 16;
   for (int i = 0; i < n; i++)
      order[i] = i;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         if (m->dev.devs[order[j]].rssi > m->dev.devs[order[i]].rssi) {
            int t    = order[i];
            order[i] = order[j];
            order[j] = t;
         }
   for (int kk = 0; kk < n; kk++) {
      int i = order[kk];
      char rs[12];
      (void)snprintf(rs, sizeof rs, "%d dBm", m->dev.devs[i].rssi);
      menu_row(fb, h, y, sc, lh, m->dev.devs[i].name, rs, 0xFFFFFFFF,
               MA_DEV_PICK, i);
      y += lh;
   }
}

/* First-run permission rationale + a CONTINUE button (records
 * ACT_GATE_CONTINUE). All static copy; the model is unused beyond the
 * framebuffer size. */

/* ---- OneTouch: how to wake the meter, then a Scan button ---- */
void render_meterhelp(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h)
{
   uint32_t *px = fb->bits;
   (void)m;
   int sc  = ui_fit_scale(fb->width, fb->height, 20);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   draw_str(px, fb, x, y, tsc, "ADD ONETOUCH", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* X returns to the ADD DEVICE type picker. */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_ADDSENSOR, 0);
   y += 3 * lh;

   static const char *const steps[] = {
       "1. TURN THE METER ON.",
       "",
       "2. PRESS ITS UP ARROW AND",
       "   OK AT THE SAME TIME SO",
       "   THE BLUETOOTH SYMBOL",
       "   SHOWS.",
       "",
       "3. THEN TAP SCAN BELOW.",
   };
   for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
      draw_str(px, fb, x, y, sc, steps[i], 0xFFCCCCCC);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   menu_button(fb, h, x, y, bw, sc, "SCAN", 0xFF33FF88, MA_METERSCAN, 0);
}

/* ---- sensor-type picker (first step of adding a sensor) ---- */

void render_senstype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded (see ui_fit_scale). Left on
    * width-only scaling, this screen's controls were laid out past the
    * bottom in landscape -- and render_forget records no close target, so
    * it became a dead end with no way back. */
   int sc  = ui_fit_scale(fb->width, fb->height, 20);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   (void)m;

   draw_str(px, fb, x, y, tsc, "ADD DEVICE", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_SENSOR_BACK, 0);
   y += 4 * lh; /* generous gap below the title */

   /* Each type is a standard menu_button -- the SAME control every other
    * screen uses, at the same height, so buttons look and feel identical
    * across the app (this screen used to grow bespoke, much taller
    * buttons). The OneTouch shows its full name (the stored type name
    * stays "ONETOUCH" so the 16-char device label does not truncate). */
   int bw = fb->width - (2 * x);
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      y = menu_button(fb, h, x, y, bw, sc, sensor_disp_name(t), 0xFFFFFFFF,
                      MA_TYPE, t);
      y += lh;
   }
}
