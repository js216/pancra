// SPDX-License-Identifier: GPL-3.0
// uimain.c --- The main screen (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "alarmlogic.h" /* AL_ENTRY_MAX: the alarm keypads' ceiling */
#include "exercise.h" /* EX_SETTLE_S: the pinned button draws its countdown */
#include "font.h"
#include "insulin.h" /* struct ins_rec + INS_* for the INSULIN LOG table */
#include "ndk.h"
#include "plot.h"
#include "sensors.h"  /* sensor types, kinds, marker enum */
#include "settings.h" /* SET_NCOLORS: crosschecked below */
#include "style.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimenu.h"
#include "uimodel.h"
#include "uipriv.h"
#include "util.h"   /* str_snapshot */
#include "weight.h" /* wt_unit_name / wt_to_tenths: the weight rows */
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* One threshold row on the main screen: "<NAME>  LOW <v>  HIGH <v>", the full
 * column width, returning the y below it.
 *
 * ALARM and NUDGE share this because their columns MUST agree: the two rows
 * are read as a pair (the nudge is the outer band, the alarm the inner one),
 * and values that do not line up cannot be compared at a glance. Both names
 * are five characters and both rows reserve the SAME fixed icon cell -- the
 * nudge simply leaves it empty -- so identical arithmetic here puts every
 * column in the same place on both rows. */

/* The CGM that owns the big number, or NULL. Four places open-coded this same
 * scan; the big number, its session line and the plot all have to agree on
 * which sensor they are describing, so there is one answer to the question. */
static const struct ui_sensor *primary_cgm(const struct screen *m)
{
   for (int k = 0; k < m->dev.nsensors; k++)
      if (m->dev.sensors[k].primary && m->dev.sensors[k].kind == KIND_CGM)
         return &m->dev.sensors[k];
   return 0;
}

/* How much of the primary's session is left, as one string, returning the
 * colour it should carry: WARMUP m:ss / LEFT 3D 4H / GRACE 8H 35M / ENDED /
 * "--". Imminence is in the colour -- yellow inside the last day, red inside
 * the final two hours -- because the number alone does not shout.
 *
 * This rule used to be inlined in the main screen's SESSION row, one of four
 * rows in a table that has since collapsed into a single line under the
 * progress bar. Keeping it in a function is what let that move be a layout
 * change rather than a rewrite of the semantics. */
static uint32_t session_left(const struct screen *m, const struct ui_sensor *ps,
                             char *out, int n)
{
   const uint32_t plain = 0xFFCCCCCC;
   if (!m->reading.has_cgm) {
      (void)snprintf(out, n, "--");
      return plain;
   }
   /* GATED ON THE SESSION CLOCK, not on have_reading.
    *
    * have_reading means "the driver has decoded a 4e THIS PROCESS", which is a
    * fact about the app's uptime, not about the sensor's session. Gating on it
    * blanked the countdown for a whole five-minute cadence after every launch
    * even though the session was running and its clock was known (it is cached
    * across restarts now -- see sessc_restore). A session duration should
    * disappear only when there is genuinely no session to describe. */
   if (m->reading.session_seconds <= 0) {
      /* No clock yet: if the primary is inside its warmup window, SAY so
       * with the minutes left -- an unexplained "--" for the first half hour
       * of a new sensor reads as broken, and warmup is the one wait that is
       * by design. Off the pairing instant it is an estimate; the '~' says
       * so. */
      long wp = (ps && ps->last == 0) ? ps->paired : 0;
      if (wp > 0 && m->now - wp < SENSOR_WARMUP_S) {
         (void)snprintf(out, n, "WARMUP ~%ldM",
                        (wp + SENSOR_WARMUP_S - m->now) / 60);
         return 0xFF00CCFF;
      }
      (void)snprintf(out, n, "--");
      return plain;
   }
   long ss = m->reading.session_seconds;
   if (m->reading.sess_state == SENSOR_STATE_WARMUP ||
       (m->reading.sess_state == 0 && ss > 0 && ss < SENSOR_WARMUP_S)) {
      /* The sensor's OWN state byte, or the clock heuristic before any 4e has
       * answered. The live clock is what makes this match the official
       * reader, to the second. */
      long r = SENSOR_WARMUP_S - ss;
      if (r < 0)
         r = 0;
      (void)snprintf(out, n, "WARMUP %ld:%02ld", r / 60, r % 60);
      return 0xFF00CCFF;
   }
   if (m->reading.sess_state == SENSOR_STATE_ENDED) {
      /* The sensor SAID the session is over -- its own verdict, not
       * arithmetic on a wear budget. The one allowed ENDED. */
      (void)snprintf(out, n, "ENDED");
      return 0xFF4466FF;
   }
   /* The primary's own wear budget (per-device: user override, DIS model, or
    * type default -- see sensor_wear_seconds). */
   long len  = (ps && ps->wear_len > 0) ? ps->wear_len : 15L * 86400;
   long left = len - ss;
   if (left <= 0) {
      /* Past the nominal end the sensor still runs for SENSOR_GRACE_S, so
       * count THAT down -- and past the grace too, keep counting into the
       * NEGATIVE rather than switching to a dead-end word: the sign says
       * "past the hard stop" while still showing by how much, which stays
       * honest even when the wear budget is set wrong for a sensor that is
       * visibly alive. Minutes only inside the last hour, so the boundary
       * never renders as the nonsense "-0H 0M". */
      long grace     = left + SENSOR_GRACE_S;
      long ag        = (grace < 0) ? -grace : grace;
      const char *sg = (grace <= -60) ? "-" : "";
      if (ag >= 3600)
         (void)snprintf(out, n, "GRACE %s%ldH %ldM", sg, ag / 3600,
                        (ag % 3600) / 60);
      else
         (void)snprintf(out, n, "GRACE %s%ldM", sg, ag / 60);
      return (grace < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
   }
   if (left < 86400) {
      /* The last day counts in hours and minutes: a bare "0D" reads as
       * already over. */
      (void)snprintf(out, n, "LEFT %ldH %ldM", left / 3600, (left % 3600) / 60);
      return (left < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
   }
   (void)snprintf(out, n, "LEFT %ldD %ldH", left / 86400,
                  (left % 86400) / 3600);
   return plain;
}

/* Left/top column: big number + label column, plot tabs, plot, the ALARM and
 * NUDGE threshold rows. Draws into [cx, cx+cw); returns the y just below the
 * last row. Records the big-number band (open settings), the plot rect
 * (scrub), the tab cells, and both threshold rows' targets. */
/* Vertical cost of the out-of-range banner, in units of sc: one 7-tall glyph
 * at the normal scale plus 2 of air under it, in the band above the big
 * number.
 *
 * It was 51 -- a (7+9) advance plus a 5*sc-scaled glyph, 7*5 tall -- charged
 * to whichever block sat at the BOTTOM of the column, and the reason every
 * such block had to know about a banner it did not draw. The cost is now paid
 * once, above the number, out of padding that was already there: in portrait
 * the gap is 12 and this fits inside it, so the number does not move at all.
 * The blocks below no longer reserve anything, which is what leaves room for
 * the second shortcut row. */
#define UI_BANNER_H (9)

/* Defined below; the number block draws it, both orientations. */
static const char *banner_of(const struct screen *m, uint32_t *col);

/* THE TOP BLOCK AND THE PLOT BLOCK ARE SEPARATE RENDERERS.
 *
 * They used to be one function, which was fine while they were always stacked
 * in one column. In LANDSCAPE they are not: the big number keeps the left
 * column with the stats table under it, while the plot and the threshold rows
 * move to the right column where the plot can have real height. Two callers
 * placing two blocks independently is exactly what one function could not do.
 *
 * What the plot block needs from the number block travels in this struct
 * rather than being recomputed -- the tab row's tap band is defined by the
 * number's lowest pixel and by the rows beside it, so a second copy of that
 * arithmetic would be a silent mis-actuation waiting to happen (it has been
 * one before: a tap on the AGE used to switch the plot span). */
struct bignum_geo {
   int y; /* y below the block, before the tab-row gap */
   int bigsc3, bigsc, gh, pad;
   int agev_y, pl_y; /* the AGE row and the primary/session line */
   int x0, x1;       /* bar's left ink edge; units label's right ink edge */
};

static struct bignum_geo render_bignum(struct ANativeWindow_Buffer *fb,
                                       const struct screen *m, struct hits *h,
                                       int cx, int cw, int y, int sc,
                                       int bottom)
{
   uint32_t *px  = fb->bits;
   int landscape = bottom > 0;                   /* height-constrained column */
   int pad       = landscape ? 6 * sc : 18 * sc; /* padding around the number */

   char big[8];
   uint32_t bigcol = 0;
   /* glu < 0 is "no current reading" -- the primary has no data yet (the big
    * number never borrows another sensor's). With NO live CGM at all the big
    * number is meaningless, so it (and its age below) blank out entirely
    * rather than showing a stale value from a disconnected sensor. Same
    * placeholder as stale. */
   if (m->reading.stale || m->reading.glu < 0 || !m->reading.has_cgm) {
      (void)snprintf(big, sizeof big, "---");
      bigcol = 0xFF888888;
   } else {
      fmt_glu(m->reading.glu, m->prefs.units, big, sizeof big);
      bigcol = glu_color_band(m->reading.glu, m->prefs.alarm_low,
                              m->prefs.alarm_high, m->prefs.nudge_low,
                              m->prefs.nudge_high);
   }
   /* THE OUT-OF-RANGE BANNER, in the band above the number.
    *
    * The gap over the big number was empty, and in portrait it is 12*sc --
    * more than the 9*sc a normal-size row costs -- so the banner fits inside
    * padding that already existed and the number does not move. Landscape's
    * gap is only 4*sc, so there the band grows to UI_BANNER_H; that column
    * gains far more than it spends, because the plot no longer reserves the
    * old 51*sc banner under its threshold rows.
    *
    * DRAWN FROM `y` DOWNWARD, never from the number upward. `y` on entry is
    * already clear of the Android status bar (the caller starts the screen at
    * fb->height/20 + 2*sc for exactly that reason), so anything placed at or
    * below it is clear of the clock and the notification icons too. Deriving
    * the banner's position by subtracting from the number's y would put it an
    * unknown distance ABOVE that guarantee, which is the one direction on
    * this screen with no margin left. */
   int bangap = landscape ? 4 * sc : 12 * sc;
   if (bangap < UI_BANNER_H * sc)
      bangap = UI_BANNER_H * sc;
   /* WHERE the banner goes is fixed here; WHAT it is drawn at is not known
    * yet. It is left-aligned on the big number's own left ink edge, and that
    * edge depends on the layout below (the number is right-aligned inside a
    * pinned footprint, so a two-digit reading starts further right than a
    * three-digit one). So the row is reserved now -- the y is what everything
    * below is measured from -- and the text is drawn once bx is known. */
   int bany = y + (2 * sc);
   y += bangap;

   char tr[8];
   /* Sized for the widest formatted age. `a` is clamped below, but the
    * compiler cannot see that, and a genuinely huge value would truncate. */
   char agestr[24];
   if (m->reading.stale || m->reading.glu < 0 || !m->reading.has_cgm)
      (void)snprintf(tr, sizeof tr, "---");
   else
      fmt_trend(m->reading.trend, tr, sizeof tr);
   long a = m->now - m->reading.t;
   if (a < 0)
      a = 0;
   if (m->reading.t <= 0 ||
       !m->reading.has_cgm) /* no live CGM (or no reading): blank the age */
      (void)snprintf(agestr, sizeof agestr, "--");
   else if (a < 1000)
      /* Seconds for as long as three digits can hold them. A CGM reports
       * every five minutes, so the number people actually watch is "how long
       * since the last one" -- and rounding 599 seconds to "9 M" throws away
       * exactly the resolution that answers it. 1000 is the switch because
       * that is where the field would need a fourth digit. */
      (void)snprintf(agestr, sizeof agestr, "%ld S", a);
   else
      (void)snprintf(agestr, sizeof agestr, "%ld M", a / 60);
   /* PREDICTION, between the trend and the age: the sensor's own forecast for
    * the next reading, as ">123" -- the arrow glyph already in the font, so it
    * reads as "heading for" without a word. `predicted` is a 10-bit field
    * whose 0x3ff (1023) means "no prediction", and no real value exceeds
    * Dexcom's 400 mg/dL cap, so both bounds are the sentinel test. The row
    * holds its place with ">--" rather than vanishing, or the column below it
    * would jump every time a prediction came and went. */
   char pred[14];
   /* SHOWN IFF THE TREND IS -- both come out of the same 4e response, so a
    * prediction without a trend (or a trend without a prediction) means one of
    * the two sentinels fired, not that the app has just started. The
    * have_reading gate that used to be here made the row disappear for a
    * cadence after every launch while the trend beside it came straight back
    * from the log; the sentinel bounds below are the real test. */
   if (m->reading.has_cgm && m->reading.predicted > 0 &&
       m->reading.predicted <= 400) {
      char pv[12];
      fmt_glu(m->reading.predicted, m->prefs.units, pv, sizeof pv);
      (void)snprintf(pred, sizeof pred, ">%s", pv);
   } else {
      (void)snprintf(pred, sizeof pred, ">--");
   }
   /* Signal strength moved to each device's own menu (SIGNAL STRENGTH); the
    * main readout shows units / trend / prediction / age. */
   int uw     = str_len(UI_LBL(m->prefs.units));
   int aw     = str_len(agestr);
   int pw     = str_len(pred);
   int widest = uw > aw ? uw : aw;
   if (pw > widest)
      widest = pw;
   int col_w = widest * 6 * sc;
   int gap   = 6 * sc;
   /* THE FOOTPRINT IS SIZED FOR THE UNIT'S WIDEST POSSIBLE VALUE, not for
    * three glyphs always.
    *
    * mg/dL never exceeds three characters, so it keeps the three-glyph fit it
    * has always had -- unchanged, deliberately. mmol/L does: Dexcom's 400
    * mg/dL cap is 22.2, and anything from 10.0 up is FOUR characters. Pinned
    * at three, the number shrank the moment the reading crossed 10.0 and grew
    * back when it fell under -- the one element on the screen that must never
    * move, resizing itself several times a day.
    *
    * Everything on this band -- the scale, the left edge, the label column,
    * the hamburger and the age bar -- derives from this pinned scale rather
    * than from the current string, so nothing moves as the digit count
    * changes. The number is drawn right-aligned on the footprint's right ink
    * edge, so the units place stays put and a shorter value grows leftward. */
   int nglyph = m->prefs.units ? 4 : 3;
   int bigsc3 = sc * 10; /* the pinned N-glyph scale */
   int fit3   = (cw - (4 * sc) - gap - col_w) / (nglyph * 6);
   if (bigsc3 > fit3)
      bigsc3 = fit3;
   if (bigsc3 < 2 * sc)
      bigsc3 = 2 * sc;
   int foot_cells = nglyph * 6;     /* footprint incl. the trailing gap */
   int foot_ink   = foot_cells - 1; /* ...and without it */
   int len        = str_len(big);
   int bigsc      = bigsc3;
   /* Only ever reached if a value somehow exceeds the unit's own maximum
    * width; with the footprint now matched to the unit it should not happen,
    * but clipping the big number silently would be the worse failure. */
   if (len > nglyph) {
      int fit = (cw - (4 * sc) - gap - col_w) / (len * 6);
      if (bigsc > fit)
         bigsc = fit;
      if (bigsc < 2 * sc)
         bigsc = 2 * sc;
   }
   int foot_w = foot_cells * bigsc3; /* footprint incl. the trailing gap */
   int bx3    = cx + ((cw - (foot_w + gap + col_w)) / 2);
   if (bx3 < cx + (2 * sc))
      bx3 = cx + (2 * sc);
   int ink_w = ((len * 6) - 1) * bigsc;
   int bx    = bx3 + (foot_ink * bigsc3) - ink_w; /* right-aligned on it */
   if (bx < cx + (2 * sc))
      bx = cx + (2 * sc);
   /* THE BANNER, LEFT-ALIGNED ON THE NUMBER'S INK.
    *
    * `bx`, not the footprint's left edge bx3: the two differ for anything
    * shorter than the pinned digit count, and LOW readings are exactly the
    * two-digit case -- aligning to the footprint would leave the word sitting
    * left of the number precisely when it is showing. The cost is that the
    * banner moves with the digit count, which is visible only when a reading
    * crosses 100 while out of range.
    *
    * Drawn here rather than where the row was reserved, because bx is a
    * result of the layout immediately above. */
   {
      uint32_t bcol     = 0;
      const char *bmsg  = banner_of(m, &bcol);
      if (bmsg)
         draw_str(px, fb, bx, bany, sc, bmsg, bcol);
   }
   draw_str(px, fb, bx, y, bigsc, big, bigcol);
   /* The NUMBER ITSELF opens the DEVICES screen -- the registry of everything
    * feeding it, where the primary is chosen (per device) and a new one is
    * added. The glyphs only, not the band: the hamburger (settings) and the
    * tab row keep their own pixels, which is why the old whole-band settings
    * target was removed. */
   add_hit_ix(h, bx, y, ink_w, 7 * bigsc, MA_DEVICES_OPEN, 0);
   /* Age bar: a thin bar under the number, exactly the footprint's ink
    * width (three digits: 5+1+5+1+5 cells) whatever the current digit
    * count. The full-length TRACK is always drawn in dark gray, so the
    * bar visibly ENDS -- the fill is readable as a fraction of the whole.
    * The live part draws on top in the number's own (dynamically
    * recolored) colour, filling left-to-right over one CGM cadence plus
    * sync slack (305 s); once overdue it becomes a full-length DASHED
    * line -- a different pattern, not a fuller bar, so "late" can never
    * be misread as "fresh". Blank exactly when the age is blank. */
   /* Bar geometry lives OUTSIDE the draw condition: the AGE value in the
    * label column aligns itself to the bar's row whether or not the bar is
    * drawn this frame, so the column never jumps as data comes and goes. */
   int bar_w = foot_ink * bigsc3;
   int bar_h = 2 * sc;
   /* 8*sc of air under the number -- clamped so the bar always stays
    * inside this band (the space below the glyphs is bigsc3 + pad). */
   int bgap = 8 * sc;
   if (bgap > bigsc3 + pad - bar_h)
      bgap = bigsc3 + pad - bar_h;
   int bar_y = y + (7 * bigsc3) + bgap;
   if (m->reading.t > 0 && m->reading.has_cgm) {
      fill_rect(px, fb, bx3, bar_y, bar_w, bar_h, 0xFF444444);
      if (a >= 305) {
         int dash = 3 * sc; /* dash == gap */
         for (int dx = 0; dx < bar_w; dx += 2 * dash) {
            int seg = dash;
            if (dx + seg > bar_w)
               seg = bar_w - dx;
            fill_rect(px, fb, bx3 + dx, bar_y, seg, bar_h, bigcol);
         }
      } else {
         int fw = (int)(((long)bar_w * a) / 305);
         if (fw > 0)
            fill_rect(px, fb, bx3, bar_y, fw, bar_h, bigcol);
      }
   }
   int colx  = bx3 + foot_w + gap;
   int gh    = 7 * sc;     /* a label glyph is 7 rows tall */
   int num_h = 7 * bigsc3; /* the FOOTPRINT's glyph height, not the string's */
   int vlh   = gh + (2 * sc); /* tight line pitch */
   /* Column anchors: UNITS keeps its historical spot (two rows above the
    * number's bottom row); the AGE drops down to sit vertically centred on
    * the progress bar -- the value and the bar that visualises it read as
    * one row. TREND and PREDICTION divide the space between them into three,
    * so the column stays evenly spaced now that it holds four values rather
    * than three. */
   int units_y = y + num_h - gh - (2 * vlh);
   int agev_y  = bar_y + ((bar_h - gh) / 2);
   int vspan   = agev_y - units_y;
   int tr_y    = units_y + (vspan / 3);
   int pred_y  = units_y + ((2 * vspan) / 3);
   draw_str(px, fb, colx, units_y, sc, UI_LBL(m->prefs.units), 0xFFCCCCCC);
   draw_str(px, fb, colx, tr_y, sc, tr, 0xFFCCCCCC);
   draw_str(px, fb, colx, pred_y, sc, pred, 0xFFCCCCCC);
   draw_str(px, fb, colx, agev_y, sc, agestr, 0xFFCCCCCC);
   /* Settings hamburger: a modest 3-bar icon CENTERED (both axes) in the empty
    * space above the three values. Its hit box is the ONLY way to open settings
    * now (the whole-top-band target is gone), so pad it out well past the glyph
    * so the surrounding space is pressable too. */
   int ham_w  = 9 * sc;
   int ham_bh = 2 * sc; /* bar thickness */
   int ham_gp = 2 * sc; /* gap between bars */
   int ham_h  = (3 * ham_bh) + (2 * ham_gp);
   int sp_top = y;             /* top of the empty space */
   int sp_bot = units_y - gap; /* just above the first value */
   int ham_y  = sp_top + (((sp_bot - sp_top) - ham_h) / 2); /* v-centre */
   if (ham_y < sp_top)
      ham_y = sp_top;
   int ham_x = colx + ((col_w - ham_w) / 2); /* h-centre in the column */
   for (int b = 0; b < 3; b++)
      fill_rect(px, fb, ham_x, ham_y + (b * (ham_bh + ham_gp)), ham_w, ham_bh,
                0xFFCCCCCC);
   /* The settings hit zone is the WHOLE band right of the number -- from
    * the number's ink edge to the screen edge, from the band top down
    * through the entire units row -- so it cannot be missed. The number
    * keeps its own pixels (they open the DEVICES screen). */
   int hx0 = bx3 + (foot_ink * bigsc3) + sc;
   /* Bounded by THIS COLUMN, not by the whole screen. In landscape the number
    * owns the left column only, and a band running to fb->width reached across
    * into the plot column -- where the tab row, added later, took the pixels
    * back. The band still existed but its own centre no longer resolved to it,
    * which is the shape of a control that looks present and is not. Identical
    * in portrait, where the column IS the screen. */
   int hamslot = add_hit(h, hx0, y, (cx + cw) - hx0, (units_y + (7 * sc)) - y,
                         ACT_OPEN_SETTINGS, 0);
   /* ...but the pressed highlight lights the hamburger GLYPH alone (plus a
    * little breathing room) -- the zone also contains the units label, and
    * a lit MG/DL would read as if the units were about to change. Through the
    * slot the band actually got: if it was dropped there is no band to narrow,
    * and the last box on the list is a different control entirely. */
   add_glow(h, hamslot, ham_x - (2 * sc), ham_y - (2 * sc), ham_w + (4 * sc),
            ham_h + (4 * sc));

   /* ONE LINE under the progress bar: which sensor owns the big number, its
    * plot marker, and how much of its session is left --
    * "G7-91-D1  X  GRACE 8H 35M". This is the whole of what the old
    * four-row PRIMARY / STATE / SESSION / PRED table under the alarm rows
    * said that was worth saying, put on the number it describes instead of in
    * a table the eye had to travel to. Left-aligned on the bar, so the three
    * -- number, bar, line -- read as one block. */
   const struct ui_sensor *ps = primary_cgm(m);
   /* EQUAL AIR ABOVE AND BELOW THE BAR. The gap over the bar is bgap (the
    * number's glyph bottom to the bar's top); this one was a flat 5*sc, so the
    * bar sat visibly closer to the line under it than to the number over it
    * and the three stopped reading as one stack. Same constant, both sides. */
   int pl_y = bar_y + bar_h + bgap;
   {
      char sleft[32];
      uint32_t scol = session_left(m, ps, sleft, sizeof sleft);
      /* The countdown is RIGHT-ALIGNED ON THE BAR'S RIGHT EDGE, exactly.
       *
       * bx3 + bar_w is the same pixel the big number's rightmost ink lands on
       * (bx is derived from it), so number, bar and countdown share one right
       * margin and the block has a true edge on both sides instead of a ragged
       * one. draw_str lays out len cells of 6 units with no trailing gap after
       * the last, so the ink is (len*6 - 1)*sc wide -- subtracting the CELL
       * width instead would leave a one-unit sliver past the bar. */
      int slw = ((str_len(sleft) * 6) - 1) * sc;
      int sx  = bx3 + bar_w - slw;
      /* TRUNCATE the name to what is left after the countdown, never the
       * other way round: the countdown is the part that changes and the part
       * that matters, and draw_str clips silently, so an over-long label
       * would eat it with nothing on screen to say so. Budget = the columns
       * between the left edge and the countdown, less the marker cell and one
       * space either side of it. */
      char pname[24];
      str_snapshot(pname, sizeof pname, ps ? ps->label : "--");
      int nbud = ((sx - bx3) / (6 * sc)) - 3;
      if (nbud < 0)
         nbud = 0;
      if (str_len(pname) > nbud)
         pname[nbud] = '\0';
      draw_str(px, fb, bx3, pl_y, sc, pname, 0xFFCCCCCC);
      draw_str(px, fb, sx, pl_y, sc, sleft, scol);
      /* The marker sits at the exact MIDPOINT of the space between the two --
       * from the name's last ink column to the countdown's first. It used to
       * be pinned two cells after the name, which put it hard against the
       * label and left a ragged void before the countdown; centring makes the
       * gap read as deliberate whatever the name's length. */
      if (ps && ps->marker != MARK_HIDE) {
         int gr = (2 * sc * ps->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         int nw = str_len(pname) ? ((str_len(pname) * 6) - 1) * sc : 0;
         int mx = bx3 + nw + ((sx - (bx3 + nw)) / 2);
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height}, mx,
             pl_y + (3 * sc), gr, ps->marker, ui_sensor_color(ps->color));
      }
      /* The line names a device, so tapping it opens that device's menu --
       * the shortcut the old info table carried, kept on the content that
       * replaced it. */
      if (ps)
         add_hit_ix(h, bx3, pl_y - (2 * sc), (cx + cw) - bx3, gh + (4 * sc),
                    MA_SENSOR, (int)(ps - m->dev.sensors));
   }

   {
      struct bignum_geo g;
      /* g.y IS THE BOTTOM OF THIS BLOCK, not the top -- both callers place
       * something directly under it (the tab row in portrait, the stats table
       * in landscape), and returning the top silently drew that something over
       * the number.
       *
       * ONE BLANK LINE between the primary/session line and whatever follows.
       * At the bare 6*sc the two sat close enough to read as a single block,
       * but the line belongs to the NUMBER above it -- it names the sensor that
       * owns the big number and counts its session down. Air is what says so.
       *
       * Applied to the natural position AND to the floor, so the gap is there
       * on every geometry rather than only on the short windows where the
       * clamp binds. On a short window the number's nominal band is smaller
       * than the bar plus the line beneath it, and what follows simply gets the
       * remainder -- less room is a visible, honest consequence; overprinted
       * text is not. */
      int line_gap = gh + (6 * sc);
      int yb       = y + (8 * bigsc3) + pad + line_gap;
      if (yb < pl_y + gh + (6 * sc) + line_gap)
         yb = pl_y + gh + (6 * sc) + line_gap;
      g.y      = yb;
      g.bigsc3 = bigsc3;
      g.bigsc  = bigsc;
      g.gh     = gh;
      g.pad    = pad;
      g.agev_y = agev_y;
      g.pl_y   = pl_y;
      g.x0     = bx3;
      /* The units label's right ink edge -- the right margin the threshold
       * rows share with this block in portrait. */
      g.x1 = colx + (((str_len(UI_LBL(m->prefs.units)) * 6) - 1) * sc);
      return g;
   }
}

/* The plot block: the span tabs, the plot itself and the two threshold rows.
 * `g` carries what the number block above it decided (see bignum_geo); in
 * landscape the caller synthesises one for the right column, where there is no
 * number above and the rows align to the column's own edges instead. */
static int render_glucose(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h, int cx,
                          int cw, int sc, int bottom, struct bignum_geo g)
{
   uint32_t *px  = fb->bits;
   int landscape = bottom > 0;
   int scrub     = (m->plot.scrub >= 0 && m->plot.scrub < m->plot.nhist);
   int y         = g.y;
   int pad       = g.pad;
   int gh        = g.gh;
   int bigsc3    = g.bigsc3;
   int agev_y    = g.agev_y;
   int pl_y      = g.pl_y;

   /* plot-window tabs (or the scrub readout while dragging) */
   int colw = cw / UI_TABS;
   int rowh = 14 * sc;
   /* Tap target reaches up to the big number's lowest pixel and no higher, then
    * down through the tab row. The big-number (settings) band ends exactly
    * there, so the two never fight over the same pixels. */
   int tab_y = y - bigsc3 - pad;
   /* ...BUT NEVER OVER THE AGE ROW. The comment above is the original
    * intent, and it was true when the age sat beside the number; the bar and
    * its value were later moved DOWN to bar_y (= this same line plus bgap),
    * which put them inside this band. A tap on the age -- the natural "how
    * fresh is this?" gesture -- therefore dispatched ACT_PLOT_TAB and
    * silently switched the plot span to 30D. Keep the band's BOTTOM where it
    * was and start it below the age glyphs instead. */
   int age_bot = agev_y + gh + sc;
   /* ...and never over the PRIMARY line under the bar either, for exactly the
    * same reason: it is a tap target of its own now. */
   if (pl_y + gh + (4 * sc) > age_bot)
      age_bot = pl_y + gh + (4 * sc);
   int tab_bot = y + rowh;
   if (tab_y < age_bot)
      tab_y = age_bot;
   int tab_h = tab_bot - tab_y;
   if (tab_h < rowh)
      tab_h = rowh; /* never shrink below the tab glyphs themselves */
   /* Settings now opens ONLY from the hamburger (added above), not from the
    * whole top band -- the band target that used to sit here is gone, so a tap
    * on the number or trend no longer navigates away by accident. */
   if (scrub) {
      char ts[16];
      char line[48];
      char gv[12];
      int ins = (m->plot.hist[m->plot.scrub].kind == KIND_INS);
      int wt  = (m->plot.hist[m->plot.scrub].kind == KIND_WT);
      fmt_hms(m->plot.hist[m->plot.scrub].t, m->tz_off, ts, sizeof ts);
      ts[5] = '\0';
      /* An insulin dose scrubs like glucose, shown as "2 U FAST" /
       * "10 U SLOW" (src carries the dose TYPE for insulin points). A weight
       * scrubs the same way -- identical code path, identical readout shape --
       * converted from the stored GRAMS into whichever display unit is set,
       * so switching KG/LB re-renders history instead of relabelling it. */
      if (ins) {
         (void)snprintf(gv, sizeof gv, "%d U", m->plot.hist[m->plot.scrub].glu);
      } else if (wt) {
         /* Clamped so the format is provably bounded: the store's own range
          * (WT_MIN_G..WT_MAX_G) tops out at 400 kg / 882 lb, i.e. four digits
          * of tenths, but the compiler cannot see that through wt_to_tenths
          * and treats the snprintf as possibly truncating. */
         int t10 =
             wt_to_tenths(m->plot.hist[m->plot.scrub].glu, m->prefs.wunits);
         if (t10 < 0)
            t10 = 0;
         if (t10 > 99999)
            t10 = 99999;
         (void)snprintf(gv, sizeof gv, "%d.%d", t10 / 10, t10 % 10);
      } else {
         fmt_glu(m->plot.hist[m->plot.scrub].glu, m->prefs.units, gv,
                 sizeof gv);
      }
      /* On the multi-day spans a bare HH:MM is ambiguous across days, so
       * prefix the DATE as M/DD ("7/21"). A weekday name told you it was a
       * Thursday but not WHICH one, which is useless once the span passes a
       * week -- and at 30D it is useless immediately. */
      const char *unit = UI_LBL(m->prefs.units);
      if (ins)
         unit = (m->plot.hist[m->plot.scrub].src == INS_FAST) ? "FAST" : "SLOW";
      else if (wt)
         unit = wt_unit_name(m->prefs.wunits);
      if (m->plot.plot_hours >= 720) {
         /* A MONTH-long span: a weekday name is ambiguous four times over,
          * so name the actual date. */
         char dt[20];
         /* 12, not 8: gcc cannot prove the month is two digits, and this
          * build treats a possibly-truncating snprintf as an error. */
         char md[12];
         fmt_date(m->plot.hist[m->plot.scrub].t, m->tz_off, dt, sizeof dt);
         /* fmt_date gives "YYYY-MM-DD HH:MM"; take the month without its
          * leading zero, and the day as written. */
         int mon = ((dt[5] - '0') * 10) + (dt[6] - '0');
         (void)snprintf(md, sizeof md, "%d/%c%c", mon, dt[8], dt[9]);
         (void)snprintf(line, sizeof line, "%s %s %s %s", gv, unit, md, ts);
      } else if (m->plot.plot_hours >= 72) {
         /* Within a week, the weekday IS the clearer label -- "TUE 08:15"
          * places a reading the way you actually remember it, and there is
          * only one Tuesday to confuse it with. 1970-01-01 was a Thursday. */
         static const char *const wd[7] = {"SUN", "MON", "TUE", "WED",
                                           "THU", "FRI", "SAT"};
         long z = (m->plot.hist[m->plot.scrub].t + m->tz_off) / 86400;
         int wi = (int)(((z % 7) + 4 + 7) % 7); /* 0 = Sunday */
         (void)snprintf(line, sizeof line, "%s %s %s %s", gv, unit, wd[wi], ts);
      } else {
         (void)snprintf(line, sizeof line, "%s %s  %s", gv, unit, ts);
      }
      /* The unit is always shown -- it is what makes the number a
       * measurement -- so the DATED spans need a little more room than
       * double scale gives. Step the scale down only as far as the line
       * actually needs, rather than dropping to a fixed smaller size:
       * usually one notch, which reads as the same text, not a glitch. */
      int tsc2 = 2 * sc;
      while (tsc2 > sc && str_len(line) * 6 * tsc2 > cw - (4 * sc))
         tsc2--;
      int lw = str_len(line) * 6 * tsc2;
      int lx = cx + ((cw - lw) / 2);
      if (lx < cx + (2 * sc))
         lx = cx + (2 * sc);
      draw_str(px, fb, lx, y, tsc2, line, 0xFFFFFFFF);
   } else {
      int laby = y + ((rowh - (7 * sc)) / 2);
      for (int i = 0; i < UI_TABS; i++) {
         char lab[12];
         /* ui_tab_hours holds small constants, but the compiler cannot prove
          * it; clamp so the formatted width is provably bounded. */
         int th = ui_tab_hours[i];
         if (th < 0)
            th = 0;
         if (th > 99999)
            th = 99999;
         if (th < 48)
            (void)snprintf(lab, sizeof lab, "%dH", th);
         else
            (void)snprintf(lab, sizeof lab, "%dD", th / 24);
         int lw   = str_len(lab) * 6 * sc;
         int tabx = cx + (i * colw);
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, lab,
                  ui_tab_hours[i] == m->plot.plot_hours ? 0xFFFFFFFF
                                                        : 0xFF888888);
         /* arg carries the plot span in hours, so the shell needn't know the
          * tab list -- it just assigns it. */
         add_hit(h, tabx, tab_y, colw, tab_h, ACT_PLOT_TAB, ui_tab_hours[i]);
      }
   }
   y += rowh;

   /* Plot height: in landscape it fills the column; in PORTRAIT it grows to
    * consume the screen down to a RESERVED band that exactly holds the info
    * block plus the alarm banner with a blank line above and below the alarm's
    * large letters -- so the bottom is never a large dead gap, and the layout
    * does not jump when an alarm appears (the space is always held).
    *
    * The reserve is the info block's own vertical budget (render_info's needv:
    * 4 info rows + gap + 4 stat rows + the banner's advance and 7*5 glyph) plus
    * one blank line (16) for the gap BELOW the alarm. render_info fits itself
    * into whatever is left, so reserving at least this much can only leave it
    * room to spare -- never clip. */
   /* Reserve, in sc units, everything drawn between the plot bottom and the
    * screen bottom AT FULL FONT:
    *   59 = what render_glucose itself adds after the plot: a 9 gap, then the
    *        ALARM and NUDGE threshold rows at 7 row + 18 portrait pad EACH.
    *        This was 34 while there was one row; a second row that the
    *        reserve does not count is a second row the plot grows over, and
    *        the overlap lands on the thresholds -- the numbers whose whole
    *        purpose is to be readable at a glance.
    *  186 = render_info's own budget (needv: 4 info rows + gap + 4 stat rows +
    *        the banner's advance and glyph),
    *   16 = one blank line BELOW the alarm's large letters.
    * Reserving render_info's full budget keeps its font at sc (it only
    * downscales when squeezed), which is the point -- the plot grows into the
    * dead space, the text below it does NOT shrink. */
   int reserve = (59 + 186 + 16) * sc;
   int grow = fb->height - y - reserve; /* plot bottom = reserve from screen */
   int ph   = 0;
   if (landscape)
   /* 39, not 26: in landscape the plot is sized by SUBTRACTING what comes
    * after it, and what comes after it is now TWO threshold rows -- a 9 gap
    * then 7 glyph + 6 landscape pad EACH, i.e. 35, leaving the same 4 sc of
    * slack the old 26 left over one row. Left at 26 the NUDGE row and all
    * three of its tap targets landed below the buffer on every wide-short
    * geometry (measured at 1440x1300 and 1600x720), which is the whole
    * no-scrolling failure: drawn nowhere, tappable nowhere. */
   /* THREE QUARTERS of what is left, not all of it. The rest is the two
    * threshold rows (39*sc).
    *
    * It used to reserve the banner under them as well, this column being the
    * only one with room beneath the plot in landscape. The banner is above
    * the big number now, in the other column, so that reservation is gone and
    * the plot keeps the height -- but the three-quarter cap stays, because
    * what it protects is the threshold rows' own room on a short window, not
    * the banner's. */
   {
      ph      = bottom - y - (39 * sc);
      int cap = ((bottom - y) * 3) / 4;
      if (ph > cap)
         ph = cap;
   } else {
      /* Never below the old fixed height (short screens keep exactly the
       * previous layout, so nothing that used to fit now clips); grow only
       * into genuine excess on taller screens. */
      ph = (grow > 12 * g.bigsc) ? grow : 12 * g.bigsc;
   }
   if (ph < 20 * sc)
      ph = 20 * sc;
   int plot_x = cx + (2 * sc);
   int plot_y = y;
   int plot_w = cw - (4 * sc);
   /* Must EQUAL store.h's NHIST: the shell sends up to NHIST points, and this
    * static cap clamps how many the plot draws. If it were smaller, the plot
    * would truncate the oldest in-window points even when the shell holds a
    * full 7 days -- the same shrinking-7D bug NHIST's sizing fixes. the
    * renderer is intentionally decoupled from store.h, so the Makefile
    * `crosscheck` target greps both and fails the build if they ever drift
    * apart. */
#define UI_PLOT_GLU 5040
   /* ...PLUS the insulin doses, which the shell appends AFTER the glucose
    * points in the SAME m->plot.hist array (build_model sizes it NHIST + NINS).
    * Capping at the glucose figure alone silently dropped every dose whose
    * index landed past it: with the history full -- the steady state after
    * a fortnight -- that is ALL of them, and before that the NEWEST ones,
    * so a dose logged minutes ago was missing from the plot while older
    * ones still showed. NINS comes from insulin.h (already included by
    * ui.h); only the glucose half is a literal, so the Makefile's
    * crosscheck can keep it in step with store.h's NHIST. */
/* A LONG span returns up to PLOT_LONG_MAX points (plotdata.h), which is
 * far more than the live window holds -- size for the larger of the two or
 * the old half of a 30-day plot is silently cut off. */
#define UI_PLOT_MAX (PLOT_LONG_MAX + NINS + NWT)
   static struct plot_pt pts[UI_PLOT_MAX];
   int np = m->plot.nhist < UI_PLOT_MAX ? m->plot.nhist : UI_PLOT_MAX;
   for (int i = 0; i < np; i++) {
      pts[i].t   = m->plot.hist[i].t;
      pts[i].glu = m->plot.hist[i].glu;
      /* Every sensor's datapoints take the colour set in its menu, INCLUDING
       * the primary -- changing a sensor's colour must actually recolour its
       * trace (it silently did nothing before, because the primary was pinned
       * to col = 0). Only non-primary sources also take a custom marker shape,
       * so the main trace stays a clean dotted line while meter/secondary
       * points stay distinct -- the meter-vs-CGM divergence a calibration keys
       * on. */
      pts[i].marker = 0;
      pts[i].col    = 0;
      pts[i].hidden = 0;
      pts[i].size   = MARK_SIZE_DEF;
      /* An insulin dose: drawn in the user's INSULIN MARKER styling
       * (marker/colour/size, both types alike). Its y IGNORES the units
       * value entirely -- units are not glucose -- and sits at 60, the
       * middle of the 50..70 band below the low line, where glucose
       * points rarely live and the frame never clips the glyph. The
       * scrub still reads the real units from m->plot.hist. */
      if (m->plot.hist[i].kind == KIND_INS) {
         int ty     = (m->plot.hist[i].src == INS_FAST) ? INS_FAST : INS_SLOW;
         pts[i].glu = 60;
         pts[i].marker = m->ins.ins_marker[ty];
         pts[i].col    = ui_sensor_color(m->ins.ins_color[ty]);
         pts[i].size   = m->ins.ins_size[ty];
         if (m->ins.ins_marker[ty] == MARK_HIDE)
            pts[i].hidden = 1;
         continue;
      }
      /* A logged WEIGHT: the same bottom line as the doses (glu pinned to 60,
       * well clear of real glucose), drawn as a small W. Its glu field carries
       * GRAMS, which is meaningless on this axis -- the scrub reads the real
       * value back out of m->plot.hist, exactly as it does for a dose. A fixed
       * shape and colour, not a configurable marker: there is one weight
       * series, so there is nothing to tell apart. */
      if (m->plot.hist[i].kind == KIND_WT) {
         pts[i].glu    = 60;
         pts[i].marker = PLOT_MARK_W;
         pts[i].col    = 0xFF88CCFF;
         pts[i].size   = MARK_SIZE_DEF;
         continue;
      }
      int matched = 0;
      for (int k = 0; k < m->dev.nsensors; k++) {
         /* Pre-registry legacy readings (src 0) match NO sensor and keep the
          * default value-based styling below. They used to be attributed to
          * the PRIMARY sensor at display time ("one CGM behind all of it"),
          * but the primary flag is mutable: the moment the user made a
          * freshly paired G7 primary, days of another sensor's legacy data
          * flipped to the G7's colour and marker on the plot -- a provenance
          * lie the append-only log exists to prevent. Unknown provenance is
          * rendered as the neutral main trace, never as a live device. */
         if (m->dev.sensors[k].id == m->plot.hist[i].src &&
             m->plot.hist[i].src != 0) {
            matched       = 1;
            pts[i].col    = ui_sensor_color(m->dev.sensors[k].color);
            pts[i].marker = m->dev.sensors[k].marker; /* shape applies to ALL,
                                                     including the primary */
            pts[i].size = m->dev.sensors[k].size;
            /* HIDE: drop this device's point entirely. */
            if (m->dev.sensors[k].marker == MARK_HIDE)
               pts[i].hidden = 1;
            break;
         }
      }
      /* A DISCONNECTED (old) device keeps its slot, so it is matched by the
       * loop above and its historical trace stays in the device's own marker
       * and colour -- consistent with what the OLD DEVICES menu shows. Only a
       * source with NO slot at all (re-minted under a new id, e.g. a firmware
       * bump) is a true orphan: draw it muted and crossed so it reads as
       * "historical, not from a sensor you still have". src 0 is pre-registry
       * legacy data, which genuinely IS the primary trace, so it keeps the
       * default. */
      if (!matched && m->dev.nsensors > 0 && m->plot.hist[i].src != 0) {
         pts[i].marker = MARK_CROSS;
         pts[i].col    = UI_ORPHAN;
      }
   }
   /* The longer the span, the denser the points and the less a fat marker
    * says: at 30D thousands of readings share the width, so half the 7D
    * radius keeps the shape of the trace readable instead of smearing it
    * into a band. */
   int prad = 3 * sc / 2;
   if (m->plot.plot_hours >= 720)
      prad = prad / 4;
   else if (m->plot.plot_hours >= 168)
      prad = prad / 2;
   else if (m->plot.plot_hours >= 72)
      prad = prad * 3 / 4;
   if (prad < 1)
      prad = 1;
   /* ONE configuration, used to draw and then recorded for the touch path:
    * two compound literals are two things to keep in step, which is the drift
    * this whole change exists to remove. */
   struct plot_cfg pcfg = {m->plot.plot_max, prad};
   plot_render((struct plot_fb){px, fb->stride, fb->width, fb->height},
               (struct plot_rect){plot_x, plot_y, plot_w, ph}, pts, np, m->now,
               m->plot.plot_hours, pcfg, white_color,
               scrub ? m->plot.scrub : -1, UI_HILITE, m->tz_off);
   /* the whole plot rect scrubs; the shell resolves the datapoint via plot_hit
    */
   add_hit(h, plot_x, plot_y, plot_w, ph, ACT_SCRUB, 0);
   /* ...WITH THE CONFIGURATION IT WAS DRAWN WITH. plot_hit must reproduce
    * this mapping exactly, and the radius above is derived from the screen
    * scale and the span -- neither of which the touch path can see. It used
    * to read a process global this render had set, which answered for
    * whichever plot was drawn last. */
   h->plot = pcfg;
   y += ph + (9 * sc);

   /* Threshold rows: "ALARM  LOW 110  HIGH 300" and, below it, the NUDGE
    * pair. Both are the full column and both are laid out by thresh_row, so
    * their LOW/HIGH columns line up exactly (see there). */
   /* Left edge = the progress bar's leftmost pixel (bx3); right edge = the
    * right ink edge of the units label beside the big number. Both are locals
    * of THIS function -- the number, the bar and these rows are all laid out
    * here -- so the margins are shared by construction rather than by two
    * copies of the same arithmetic that could drift apart. */
   y = thresh_row(fb, m, h, g.x0, g.x1, y, sc, pad, 1);
   y = thresh_row(fb, m, h, g.x0, g.x1, y, sc, pad, 0);
   /* The banner used to be drawn here in landscape, this column being the
    * only one with room under it, and in the info block in portrait. It now
    * rides above the big number in both orientations, so neither column
    * places it and neither has to know it exists. */
   return y;
}

/* Right/bottom column: the rolling-stats table, the ADD '+' and the alarm
 * banner. The PRIMARY / STATE / SESSION / PRED table that used to head this
 * block is gone -- what it said now rides on the big number itself (the
 * prediction in the label column, the sensor and its countdown on one line
 * under the progress bar), which is where the eye already is. */
/* THE OUT-OF-RANGE BANNER -- STALE / LOW / HIGH.
 *
 * ONLY THE WORDING AND THE COLOUR are decided here; the caller places it.
 *
 * It used to draw itself as well, in 5*sc letters, and it needed a function of
 * its own because the two layouts put it in different columns: under the stats
 * table in portrait, under the plot's threshold rows in landscape, the stats
 * column having nothing left beneath it there. Both callers therefore had to
 * reserve UI_BANNER_H before deciding their own heights, and the block that
 * forgot did not fail -- it drew the banner off the bottom of the buffer,
 * which is unreachable because nothing on this screen scrolls.
 *
 * It is now one normal-size row in the band ABOVE the big number: empty space
 * that was going spare, immediately beside the value it qualifies, and above
 * all a place whose height no other block has to bargain for. What it frees at
 * the bottom is what the second row of pinned shortcuts is drawn in.
 *
 * Returns the message, or NULL when there is nothing to say, writing the
 * colour through `col`. */
static const char *banner_of(const struct screen *m, uint32_t *col)
{
   const char *msg = 0;
   uint32_t c      = 0;
   if (m->reading.disc_alarmed) {
      msg = "STALE";
      /* A banner-only colour, for the same visibility-check reason as LOW. */
      c = 0xFF00D0FF;
   } else if (m->now - m->reading.t <= AL_FRESH_S) {
      /* INCLUSIVE, exactly like alarm_zone (alarmlogic.c): the alarm fires AT
       * the limit, so the banner must appear at the limit too. While this read
       * `<` and the alarm read `<=`, a reading of exactly LOW sounded the alarm
       * and posted "Glucose LOW" while this screen drew no banner and coloured
       * the big number in-range -- the app contradicting its own alarm, which
       * is a reason to dismiss a real hypo. One threshold, one comparison. */
      if (m->reading.glu >= 0 && m->reading.glu <= m->prefs.alarm_low) {
         msg = "LOW";
         /* Deliberately NOT glu_color's red (0xFF0000FF): sharing that value
          * made the offline visibility check vacuous, because the big number
          * is drawn in it too whenever glu < 50 -- so "the banner is visible"
          * passed while the banner was entirely off-screen. A banner-only
          * colour is what makes that assertion mean something. */
         c = 0xFF2020E0;
      } else if (m->reading.glu >=
                 m->prefs.alarm_high) { /* inclusive, as above */
         msg = "HIGH";
         /* Banner-only, like LOW. Sharing glu_color's orange is what made the
          * LOW visibility assertion vacuous for five review rounds -- the
          * check passed on the big number while the banner was off-screen.
          * HIGH is safe today only by the coincidence that a high reading puts
          * the number in the white band; do not rely on that. */
         c = 0xFF20A0FF;
      }
   }
   *col = c;
   return msg;
}

/* Defined with the menu renderers below; the main screen's shortcut row needs
 * the SAME framed button the ADD menu draws, so the two cannot drift into
 * looking like different controls for the same action. */

static void render_info(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        struct hits *h, int cx, int cw, int y, int sc)
{
   uint32_t *px = fb->bits;
   /* Fit to the space actually left below the plot, in BOTH axes.
    *
    * This block had no fitting logic at all: it inherited a width-derived
    * scale and spent it on height, so on every realistic phone window the
    * stats table (TIR / AVG / A1C) and the LOW/HIGH/STALE banner were drawn
    * entirely below the bottom of the buffer -- invisible, and unreachable
    * because there is no scrolling. The banner is the explicit on-screen
    * indication that the user is out of range, so it must render.
    *
    * Vertical budget in units of sc: a 7 gap, 4 stats rows (4*16), the '+'
    * band under them (24), then the banner (7+9 advance, then a 7*5 glyph).
    *
    * WIDTH is budgeted on the stats table alone -- 4+5*5 columns plus a units
    * label of up to 6 = 35 -- and nothing else here is wider. An earlier
    * version reserved 53 columns for a worst-case status line, which shrank
    * the whole block a step below the rest of the UI on a normal phone to
    * reserve room no real row used. */
   /* NO BANNER TERM, in either orientation.
    *
    * This used to read `+ (landscape ? 0 : UI_BANNER_H)`, and the asymmetry
    * was the point: in portrait this block drew the banner beneath its own
    * stats table, in landscape the plot column drew it instead. Charging for
    * it in landscape too is what once floored this block's vsc a whole step
    * and rendered the stats table in a tiny font -- the row count never
    * changed, the space it was given did. Any block that derives its scale
    * from the space it receives can shrink that way, which is why the term is
    * worth a paragraph even now that it is zero.
    *
    * The banner is above the big number now and costs this block nothing, in
    * either orientation, so the orientation test has gone with it. */
   /* The trailing 3 IS THE '+' ROW'S HIT BOX, not its glyph.
    *
    * The 24 above covers the band the '+' is DRAWN in. Its target is three
    * units taller than that at each edge, so a row whose glyph ends exactly at
    * the last pixel has a target reaching past it -- drawn on the screen,
    * partly off it, and reported by uitest's geometry sweep as a target
    * outside the buffer. It went unnoticed while this block also reserved
    * 51 units for a banner it drew underneath: that slack was absorbing an
    * overhang nobody had budgeted. Removing the banner is what exposed it, at
    * 3120x1440 and 1600x720. */
   /* AND THE SECOND SHORTCUT ROW, when there is one. 28 = the button (25) and
    * the 3 of air between the rows -- rowpitch, below, in the same units.
    *
    * Counted HERE, before the scale is chosen, because this block sizes its
    * own font from the space it is given: a row added after the fact does not
    * make the block taller, it makes the block overflow. Every element this
    * function draws has to be in this number before vsc is derived from it. */
   int npin = 0;
   for (int i = 0; i < SC_MAX; i++)
      if (m->prefs.shortcut[i] > 0)
         npin++;
   int needv  = 7 + (4 * 16) + 24 + 3 + (npin > 3 ? 28 : 0);
   int availv    = fb->height - y;
   int vsc       = availv > 0 ? availv / needv : 1;
   int hsc       = cw / (2 + (35 * 6)); /* 35 = the stats table's fixed width */
   if (vsc < sc)
      sc = vsc;
   if (hsc < sc)
      sc = hsc;
   if (sc < 1)
      sc = 1;
   /* No left-margin x any more: the stats table is CENTRED (tx below) and the
    * '+' is pinned to the right edge, so nothing on this block hangs off a
    * left margin. */
   int lh             = 16 * sc; /* row pitch: matches the settings leading */
   const uint32_t col = 0xFFCCCCCC;

   /* Widest row is a stats row: 4 + 5*6 + a 6-char unit, with each cell up to
    * 15 now that hc[] is wider. 96 covers it with room to spare. */
   char row[96];

   /* rolling stats table: TIR / AVG / A1C across 1D/3D/7D/30D/90D */
   char tc[5][8];
   char ac[5][8];
   char hc[5][16]; /* "%d.%d" of an unbounded A1C estimate */
   for (int i = 0; i < 5; i++) {
      if (m->plot.stat[i].have) {
         (void)snprintf(tc[i], sizeof tc[i], "%d", m->plot.stat[i].tir);
         fmt_glu(m->plot.stat[i].avg, m->prefs.units, ac[i], sizeof ac[i]);
         /* ADAG estimate: A1C% = (avg_mg/dL + 46.7) / 28.7, in tenths. */
         int te = ((100 * m->plot.stat[i].avg) + 4670 + 143) / 287;
         (void)snprintf(hc[i], sizeof hc[i], "%d.%d", te / 10, te % 10);
      } else {
         (void)snprintf(tc[i], sizeof tc[i], "--");
         (void)snprintf(ac[i], sizeof ac[i], "--");
         (void)snprintf(hc[i], sizeof hc[i], "--");
      }
   }
   /* CENTRE THE TABLE ON ITS OWN TRUE WIDTH.
    *
    * It was pinned to the left margin at a hardcoded 35 columns, so the empty
    * space fell entirely on the right and the block sat visibly off-centre.
    * Worse, 35 is only right for one units setting: the trailing label is the
    * widest cell in the table and it is "MG/DL" (5) or "MMOL/L" (6), so any
    * fixed number is wrong for one of them and the offset CHANGED when the
    * user switched units.
    *
    * Measure instead: the widest row is the AVG one (the label column plus the
    * five spans plus the unit), and draw_str lays out n cells of 6 units with
    * no trailing gap, so its ink is (n*6 - 1)*sc. Every row shares that left
    * edge, which is what keeps the columns aligned under one another. */
   int ncols = 4 + (5 * 5) + str_len(UI_LBL(m->prefs.units));
   int tinkw = ((ncols * 6) - 1) * sc;
   int tx    = cx + ((cw - tinkw) / 2);
   if (tx < cx)
      tx = cx;
   y += 7 * sc;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "", "1D", "3D",
                  "7D", "30D", "90D", "");
   draw_str(px, fb, tx, y, sc, row, 0xFF888888);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "TIR", tc[0],
                  tc[1], tc[2], tc[3], tc[4], "%");
   draw_str(px, fb, tx, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "AVG", ac[0],
                  ac[1], ac[2], ac[3], ac[4], UI_LBL(m->prefs.units));
   draw_str(px, fb, tx, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "A1C", hc[0],
                  hc[1], hc[2], hc[3], hc[4], "%");
   draw_str(px, fb, tx, y, sc, row, col);
   y += lh;

   /* A big '+' just under the stats table and hard right: the ADD entry point
    * (new device / log insulin / log weight) reachable without a trip through
    * SETTINGS. It used to sit beside the info table above; that table is gone,
    * so it follows the last thing still drawn here. Its hit zone is the whole
    * band right of the table's ink, so the surrounding space is pressable too
    * -- a bare glyph is a poor target one-handed. */
   {
      int psc = 3 * sc;
      int pw  = 6 * psc;
      int ph  = 7 * psc;
      int pxx = cx + cw - pw - (2 * sc);
      /* COUNTED AS RENDERABLE, not as stored.
       *
       * A slot can hold a pin this build no longer offers -- that is the whole
       * reason pins are stored by identity and ui_shortcut_slot_by_id can
       * answer -1 -- and the layout loop below skips those. Counting them here
       * would shape the rows around buttons that are never drawn: four stored
       * pins one of which is unknown would reserve two rows and fill them 2+1,
       * leaving a hole where the missing button would have been. */
      int nsc = 0;
      for (int i = 0; i < SC_MAX; i++)
         if (ui_shortcut_slot_by_id(m->prefs.shortcut[i]) >= 0)
            nsc++;
      /* AT MOST THREE PER ROW, AT MOST TWO ROWS.
       *
       * Three or fewer keep the single row this has always been. Four to six
       * split into two, balanced rather than filled -- four is 2+2, not 3+1 --
       * because a lone button under a full row reads as an afterthought, and
       * the eye pairs the columns of two equal rows without being told to.
       *
       * ceil(nsc/2) is that balance, and it is capped implicitly by SC_MAX: at
       * six it gives exactly three, which is the per-row ceiling. The two
       * numbers are the same fact, stated in settings.h where SC_MAX lives. */
      int nrows = nsc > 3 ? 2 : 1;
      int percol = nrows == 2 ? ((nsc + 1) / 2) : nsc;
      /* Pitch between the two rows: the button (25*sc, see menu_button) and
       * 3*sc of air. Named because the '+' is placed from it too. */
      int rowpitch = (25 * sc) + (3 * sc);
      /* AIR BETWEEN THE STATS TABLE AND THIS ROW. The table is a block of
       * numbers and this row is a set of controls, and at 2*sc they touched.
       *
       * Taken only out of SLACK: try a full line, then half, then none, and
       * take the first that still leaves the row itself on the screen.
       *
       * The 51*sc this used to hold back for the banner drawn beneath it is
       * gone -- the banner is above the big number now -- so on a short window
       * the air survives where it used to be spent, and the bottom of the
       * column is free for the second shortcut row.
       *
       * MEASURED AGAINST THE HIT BOX, which reaches 3*sc below the glyph. The
       * old test used the glyph alone and was wrong by exactly that much; the
       * banner's 51*sc was covering the difference, so it only became visible
       * once the banner moved. */
      int foot = ((nrows - 1) * rowpitch) + ph + (3 * sc);
      int air  = lh;
      if (y + (2 * sc) + air + foot > fb->height)
         air = lh / 2;
      if (y + (2 * sc) + air + foot > fb->height)
         air = 0;
      int pyy = y + (2 * sc) + air;
      /* THE '+' SITS ON THE FIRST ROW, always.
       *
       * It used to sit on the LAST one, so with two rows of shortcuts it
       * dropped to the second and its position depended on how many things
       * happened to be pinned -- the one control here that opens a whole menu
       * moved whenever an unrelated button was pinned or unpinned. Anchored
       * to the first row it is in the same place whatever the row count, and
       * the empty space falls BELOW it beside the second row, which reads as
       * the group of actions having grown rather than the '+' having
       * wandered. */
      int plusy = pyy;
      draw_str(px, fb, pxx, plusy, psc, "+", 0xFFCCCCCC);
      /* PINNED BUTTONS share the row(s) to the LEFT of the '+'. They divide
       * whatever the row has left after the plus, so one button is wide and
       * three are narrow -- the row's edges never move as shortcuts are added
       * or removed.
       *
       * BOTH ROWS TAKE THEIR WIDTH FROM THE FULLER ONE, so the columns line
       * up. Sizing each row to its own count would make a 3+2 split draw two
       * different button widths, and two rows of controls that do not share
       * columns read as two unrelated groups rather than one block.
       *
       * The LABEL shrinks, never the font. A button that cannot hold its full
       * label takes the abbreviation from ui_sc_tab; if even that will not fit
       * the button is drawn empty rather than clipped, because a half-word on
       * a control that logs insulin is worse than no word. */
      int hx0 = tx + tinkw; /* default '+' target start: past the table */
      if (nsc > 0) {
         /* ONE CHARACTER of air between the buttons, and the same again
          * before the '+'. At 2*sc they touched each other and crowded the
          * plus, which made a row of three read as one segmented control
          * rather than three separate actions -- and these actions log a
          * medication, so "which button am I about to press" has to be
          * obvious. 6*sc is exactly one glyph cell, the same unit the rest of
          * this screen spaces by. */
         int sgap = 6 * sc;
         int rowl = cx + (2 * sc); /* the row's left edge */
         /* DOUBLE the inter-button gap before the '+'. The plus is a
          * different kind of control -- it opens the whole ADD menu, where
          * the buttons beside it each fire one action directly -- so the
          * break between the group and it should read as larger than the
          * breaks inside the group. */
         int rowr   = pxx - (2 * sgap);
         int availw = rowr - rowl;
         int bwid   = (availw - ((percol - 1) * sgap)) / percol;
         if (bwid > 6 * sc) { /* narrower than this and nothing can be read */
            int bx   = rowl;
            int slotn = 0; /* how many pins have been PLACED, not scanned */
            for (int i = 0; i < SC_MAX; i++) {
               int slot = ui_shortcut_slot_by_id(m->prefs.shortcut[i]);
               if (slot < 0)
                  continue; /* a pin this build no longer offers */
               /* COUNTED ON PLACEMENT, never on the loop index. A pin this
                * build no longer offers is skipped above, so `i` and the
                * button's position part company the moment one appears -- and
                * the row would break where the gap was rather than after
                * percol buttons. */
               int r = slotn / percol;
               int c = slotn % percol;
               slotn++;
               bx = rowl + (c * (bwid + sgap));
               int code        = ui_shortcut_code(slot);
               const char *lbl = ui_shortcut_label(slot, nsc > 1);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc))
                  lbl = ui_shortcut_label(slot, 1);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc))
                  lbl = "";
               /* EXERCISE IS NOT A LABEL. Pinned here it must show the
                * level it is on, in the colour that encodes it, with the
                * settling bar -- otherwise the pinned copy and the one in the
                * ADD menu would disagree about the same value, which is the
                * objection that kept it off the pin list until now. One
                * function draws both. */
               if (code == MA_EXERCISE)
                  (void)ui_exercise_button(
                      fb, h, bx, pyy - (2 * sc) + (r * rowpitch), bwid, sc,
                      m->food.ex_level, m->food.ex_remaining, EX_SETTLE_S,
                      lbl, 0xFFCCCCCC);
               else
                  (void)menu_button(fb, h, bx,
                                    pyy - (2 * sc) + (r * rowpitch), bwid, sc,
                                    lbl, 0xFFCCCCCC, code, 0);
            }
            /* The '+' keeps only the space right of the last button. */
            hx0 = rowr;
         }
      }
      /* Start the target just past the table's actual right ink edge (it is
       * centred now, and its width follows the units label) -- never so far
       * right that the target shrinks below the glyph's own padded square. */
      if (hx0 > pxx - (3 * sc))
         hx0 = pxx - (3 * sc);
      add_hit_ix(h, hx0, plusy - (3 * sc), cx + cw - hx0, ph + (6 * sc),
                 MA_ADD_OPEN, 0);
      /* THE BLOCK ENDS BELOW THE LAST ROW, which is no longer where the '+'
       * is. With two rows the buttons extend a full rowpitch past it, and
       * `plusy + ph` would hand whatever follows a y that overlaps them. */
      y = pyy + ((nrows - 1) * rowpitch) + ph;
   }

   /* The banner used to be drawn here, this block being the one with room
    * under it in portrait. It is above the big number now, in both
    * orientations, and the space it has given up at the bottom of the column
    * is what the second row of pinned shortcuts is drawn in. */

   /* No SENSOR EXPIRED prompt any more: it read as an error banner and
    * confused more than it helped, and the main-screen '+' (ADD -> NEW
    * DEVICE) is now a permanent, calmer route to pairing a replacement. The
    * SESSION row's GRACE/ENDED countdown is what states expiry. */
}

/* Before any reading arrives: scan status lines + the scanned-sensor list. */
static void render_noreading(struct ANativeWindow_Buffer *fb,
                             const struct screen *m, struct hits *h, int y,
                             int sc)
{
   uint32_t *px = fb->bits;
   /* The status band opens settings, exactly as the big number does once a
    * reading exists. Without it this screen recorded NO touch targets at all,
    * and on_input has no fallback -- so on a fresh install (g_cur_glu is -1
    * until store_load finds a reading) settings, permissions and the whole
    * pairing flow were unreachable by touch. A sensor needing a 4-digit
    * applicator code could never be added. */
   add_hit(h, 0, y - (2 * sc), fb->width, 14 * sc, ACT_OPEN_SETTINGS, 0);
   /* "PANCRA  " + a UI_COLS-long status needs more than UI_COLS+1. The
    * status is snapshotted at UI_COLS, so budget the prefix on top. */
   char line[UI_COLS + 12];
   char st[UI_COLS + 1];
   str_snapshot(st, sizeof st, m->status ? m->status : "");
   (void)snprintf(line, sizeof line, "PANCRA  %s", st);
   draw_str(px, fb, 2 * sc, y, sc, line, 0xFFFFFFFF);
   y += 9 * sc;
   /* total rounded to 10s so ambient chatter doesn't churn the line */
   (void)snprintf(line, sizeof line, "ADV %u  DX %d",
                  (m->dev.adv_total / 10) * 10, m->dev.ndev);
   draw_str(px, fb, 2 * sc, y, sc, line, 0xFFFFFFFF);
   y += 9 * sc;

   if (m->dev.ndev > 0) {
      y += 3 * sc;
      draw_str(px, fb, 2 * sc, y, sc, "SENSORS", 0xFF888888);
      y += 10 * sc;
      for (int i = 0; i < m->dev.ndev; i++) {
         char dl[48];
         (void)snprintf(dl, sizeof dl, "%-8s %4d %s", m->dev.devs[i].name,
                        m->dev.devs[i].rssi, m->dev.devs[i].mac);
         draw_str(px, fb, 2 * sc, y, sc, dl, 0xFFCCCCCC);
         y += 9 * sc;
      }
   }
}

/* Compose the main screen: two columns in landscape, stacked in portrait. */
void render_main(struct ANativeWindow_Buffer *fb, const struct screen *m,
                 struct hits *h)
{
   int landscape = fb->width > fb->height;
   int colw      = landscape ? fb->width / 2 : fb->width;
   int sc        = colw / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   /* Bound by HEIGHT as well, exactly as the menus do.
    *
    * The main screen was the last width-only-scaled layout. Its total vertical
    * budget is the `budget` computed below -- 466 units of sc in portrait,
    * 210 in landscape, derived there -- so on any window shorter than that
    * bottom of it -- the TIR/AVG/A1C table and the LOW/HIGH/STALE banner --
    * was laid out past the edge and silently dropped by draw_cell. That is
    * reachable on 16:9 phones and on every split-screen window, and the banner
    * is the explicit on-screen indication that the user is out of range. */
   /* Vertical budget, in units of sc. In PORTRAIT the glucose block (~278) and
    * the info block (~186) are STACKED, so both must fit the height: ~466. In
    * LANDSCAPE they are SIDE BY SIDE, independent columns -- the plot flexes,
    * so the binding column is the info block (~186) plus margin. Using 466
    * there halved the scale, leaving everything tiny with the right half mostly
    * empty.
    */
   int budget = landscape ? 210 : 466;
   int mvsc   = (fb->height - (fb->height / 20)) / budget;
   if (mvsc < 1)
      mvsc = 1;
   if (mvsc < sc)
      sc = mvsc;
   int y = (fb->height / 20) + (2 * sc); /* clear the system status bar */

   /* The full screen renders whenever there is anything to show: a current
    * reading, OR any registered device (the plot may hold another sensor's
    * trace while the primary has no data yet -- the big number then shows the
    * "---" placeholder, not the scan screen). Only a genuinely fresh install
    * (no reading, no devices) gets the no-reading scan screen. */
   if (m->reading.glu >= 0 || m->dev.nsensors > 0) {
      if (landscape) {
         /* LEFT: the big number, then the stats table directly under it.
          * RIGHT: the plot and the threshold rows, with the whole column's
          * height to spend.
          *
          * The plot used to share the left column with the number, which left
          * it a sliver of height, while the right column held only the stats
          * table and a lot of empty space. Landscape exists to give the plot
          * room; this is the arrangement that actually does. */
         int gw   = 2 * 6 * sc;
         int cwid = (fb->width - gw) / 2;
         int rx0  = cwid + gw;
         int rw   = fb->width - cwid - gw;
         struct bignum_geo g =
             render_bignum(fb, m, h, 0, cwid, y, sc, fb->height);
         render_info(fb, m, h, 0, cwid, g.y, sc);
         /* A synthetic geometry for the right column: no number above it, so
          * the tab band starts at the column top and the threshold rows take
          * the COLUMN's margins rather than a bar that is not in this column.
          * Zero bigsc3/pad is what collapses the tab band's "reach up to the
          * number's lowest pixel" rule to "start where the column starts". */
         struct bignum_geo rg;
         rg.y      = y;
         rg.bigsc3 = 0;
         rg.bigsc  = 0;
         rg.gh     = 7 * sc;
         rg.pad    = 0;
         rg.agev_y = y;
         rg.pl_y   = y;
         rg.x0     = rx0 + (2 * sc);
         rg.x1     = rx0 + rw - (2 * sc);
         render_glucose(fb, m, h, rx0, rw, sc, fb->height, rg);
      } else {
         struct bignum_geo g = render_bignum(fb, m, h, 0, fb->width, y, sc, 0);
         y                   = render_glucose(fb, m, h, 0, fb->width, sc, 0, g);
         render_info(fb, m, h, 0, fb->width, y, sc);
      }
   } else {
      /* Records its own settings target -- see render_noreading. */
      render_noreading(fb, m, h, y, sc);
   }
}
