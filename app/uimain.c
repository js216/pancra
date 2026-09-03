// SPDX-License-Identifier: GPL-3.0
// uimain.c --- The main screen (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "alarmlogic.h" /* AL_ENTRY_MAX: the alarm keypads' ceiling */
#include "exercise.h"   /* EX_SETTLE_S: the pinned button draws its countdown */
#include "font.h"
#include "food.h"
#include "insrow.h"  /* INS_SLOW / INS_FAST: which kind a dose row names */
#include "insulin.h" /* struct ins_rec: the doses the INSULIN LOG table draws */
#include "ndk.h"
#include "plot.h"
#include "sensors.h"  /* sensor types, kinds, marker enum */
#include "settings.h"
#include "stats.h" /* TIR_LOW_MGDL / TIR_HIGH_MGDL: the band plot.c shades */ /* SET_NCOLORS: crosschecked below */
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

/* HOW LONG THE AGE BAR UNDER THE BIG NUMBER TAKES TO FILL.
 *
 * One CGM cadence is 300 s and the reading has to travel -- sensor to phone
 * over BLE, then decoded and stored -- so the bar is the cadence plus the
 * slack that arrival takes. Full means "the next one is due about now";
 * PAST full it stops being a bar at all and becomes a dashed line, because a
 * bar that keeps growing reads as more data rather than late data.
 *
 * 345 s: a histogram of arrivals puts the great majority in the 3..45 s
 * window after the reading's own timestamp, so the bar fills right as the
 * late end of that window passes. At 305 it dashed while readings that were
 * merely at the far end of normal were still on their way, and a display
 * that cries late four times an hour is one nobody reads. */
#define AGE_BAR_S 345

/* One threshold row on the main screen: "<NAME>  LOW <v>  HIGH <v>", the full
 * column width, returning the y below it.
 *
 * ALARM and NUDGE share this because their columns MUST agree: the two rows
 * are read as a pair (the nudge is the outer band, the alarm the inner one),
 * and values that do not line up cannot be compared at a glance. Both names
 * are five characters and both rows reserve the SAME fixed icon cell -- the
 * nudge simply leaves it empty -- so identical arithmetic here puts every
 * column in the same place on both rows. */

/* A RUN OF TIME IN THE COARSEST UNIT THAT STILL SAYS SOMETHING.
 *
 * Days from two days up, hours from one hour up, minutes below that. One
 * function because the streak and the record it is measured against are
 * rendered side by side, and two copies of this arithmetic would eventually
 * disagree about where an hour becomes a day -- which would read as the two
 * numbers being on different scales.
 *
 * `cap` is at least 7: the widest answers are "59 MIN" and "365 D" plus their
 * terminator, and a truncated one would silently change the number rather than
 * the unit. The callers pass 8, which is what lets the compiler prove the line
 * they build out of two of these fits its own buffer. */
static void streak_len(long s, char *out, size_t cap)
{
   if (s < 0)
      s = 0;
   if (s >= 2L * 86400)
      (void)snprintf(out, cap, "%ld D", s / 86400);
   else if (s >= 3600)
      (void)snprintf(out, cap, "%ld H", s / 3600);
   else
      (void)snprintf(out, cap, "%ld MIN", s / 60);
}

/* HOW THE PINNED SHORTCUTS PACK: how many go on a row, and how many rows that
 * comes to.
 *
 * AT MOST TWO PER ROW UNTIL THERE ARE MORE THAN SIX, THEN THREE, AND NEVER
 * MORE THAN THREE ROWS. A two-wide button holds its full label ("SLOW
 * INSULIN"); a three-wide one starts falling back to the abbreviation, so the
 * tighter packing is spent only once the alternative is a fourth row there is
 * no height for. Nine is three rows of three, and SC_MAX is that same number.
 *
 * BALANCED, NOT FILLED: five pins are 2+2+1, never 2+2+1 arrived at by
 * filling -- the difference shows at seven, where filling gives 3+3+1 and a
 * lone button under two full rows reads as an afterthought. One percol for
 * every row, because the rows must share their columns: two rows of controls
 * that do not line up read as two unrelated groups.
 *
 * ONE DEFINITION, because three places need the answer -- the height budget
 * before the font is chosen, the drawing loop, and the row the '+' sits on --
 * and a layout that disagrees with its own budget overflows the screen. */
static int pin_rows(int n)
{
   const int cap = (n > 6) ? 3 : 2;
   int rows      = (n + cap - 1) / cap;
   if (rows > 3)
      rows = 3;
   return rows < 1 ? 1 : rows;
}

static int pin_percol(int n)
{
   const int rows = pin_rows(n);
   const int pc   = (n + rows - 1) / rows;
   return pc < 1 ? 1 : pc;
}

/* Is this table entry pinned to the main screen?
 *
 * Pins are stored BY IDENTITY in a list that is a set, so the question is
 * asked of the whole list rather than of one index -- and asking it slot-first
 * is what lets both the count and the drawing walk ui_sc_tab in the same
 * order. SC_NONE never names a table entry, so an empty pin slot cannot
 * match. */
static int pin_has(const struct ui_prefs *p, int id)
{
   for (int i = 0; i < SC_MAX; i++)
      if (p->shortcut[i] == id)
         return 1;
   return 0;
}

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
 * A FUNCTION RATHER THAN A LINE IN THE ROW THAT DRAWS IT: the rule is about
 * what the session means, and the row it appears in has moved twice. Keeping
 * the two apart is what makes a layout change a layout change. */
static uint32_t session_left(const struct screen *m, const struct ui_sensor *ps,
                             char *out, int n)
{
   const uint32_t plain = UI_TEXT_DIM;
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
   long ss = m->reading.session_seconds;
   /* NO LIVE CLOCK: FALL BACK TO WHEN THE SESSION WAS RECORDED AS STARTING.
    *
    * The live clock is the sensor's own, exact to the second, and it is
    * preferred whenever there is one -- but there is none until this link's
    * first 0x4e of the process, and a reconnect takes it away again. The
    * instant the session started does not have to be re-learned across
    * either: it is minted once and kept in the provenance row, which is what
    * the per-device screen's STARTED and ELAPSED are built from.
    *
    * Without this the headline row read "--" for a sensor delivering
    * readings on screen: the warmup estimate below it is disqualified the
    * moment a sensor HAS produced a reading (ps->last != 0), so a sensor
    * past its first sample but before this process's first 0x4e fell
    * between the two and described its session as nothing at all. */
   if (ss <= 0 && ps && ps->activation > 0 && m->now > ps->activation)
      ss = m->now - ps->activation;
   if (ss <= 0) {
      /* Nothing recorded yet either: if the primary is inside its warmup
       * window, SAY so with the minutes left -- an unexplained "--" for the
       * first half hour of a new sensor reads as broken, and warmup is the
       * one wait that is by design. Off the pairing instant it is an
       * estimate; the '~' says so. */
      long wp = (ps && ps->last == 0) ? ps->paired : 0;
      if (wp > 0 && m->now - wp < SENSOR_WARMUP_S) {
         (void)snprintf(out, n, "WARMUP ~%ldM",
                        (wp + SENSOR_WARMUP_S - m->now) / 60);
         return UI_WARN;
      }
      (void)snprintf(out, n, "--");
      return plain;
   }
   if (m->reading.sess_state == SENSOR_STATE_WARMUP ||
       (m->reading.sess_state == 0 && ss > 0 && ss < SENSOR_WARMUP_S)) {
      /* The sensor's OWN state byte, or the clock heuristic before any 4e has
       * answered. The live clock is what makes this match the official
       * reader, to the second. */
      long r = SENSOR_WARMUP_S - ss;
      if (r < 0)
         r = 0;
      (void)snprintf(out, n, "WARMUP %ld:%02ld", r / 60, r % 60);
      return UI_WARN;
   }
   if (m->reading.sess_state == SENSOR_STATE_ENDED) {
      /* The sensor SAID the session is over -- its own verdict, not
       * arithmetic on a wear budget. The one allowed ENDED. */
      (void)snprintf(out, n, "ENDED");
      return UI_DANGER;
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
      return (grace < 2L * 3600) ? UI_DANGER : UI_WARN;
   }
   if (left < 86400) {
      /* The last day counts in hours and minutes: a bare "0D" reads as
       * already over. */
      (void)snprintf(out, n, "LEFT %ldH %ldM", left / 3600, (left % 3600) / 60);
      return (left < 2L * 3600) ? UI_DANGER : UI_WARN;
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
 * PAID ONCE, ABOVE THE NUMBER, out of padding that is already there: in
 * portrait the gap is 12 and this fits inside it, so the number does not
 * move. No block below reserves anything for it, which is what leaves room
 * for the second shortcut row -- and it is why a block at the BOTTOM of the
 * column does not have to know about a banner it does not draw. */
#define UI_BANNER_H (9)

/* Defined below; the number block draws it, both orientations. */
static const char *banner_of(const struct screen *m, uint32_t *col);

/* THE TOP BLOCK AND THE PLOT BLOCK ARE SEPARATE RENDERERS.
 *
 * IN LANDSCAPE THEY ARE NOT STACKED: the big number keeps the left column
 * with the stats table under it, while the plot and the threshold rows move to
 * the right column where the plot can have real height. Two callers placing
 * two blocks independently is what one function cannot do.
 *
 * What the plot block needs from the number block travels in this struct
 * rather than being recomputed. The tab row's tap band is defined by the
 * number's lowest pixel and by the rows beside it, so a second copy of that
 * arithmetic is a silent mis-actuation waiting to happen -- a tap landing on
 * one control and firing another. */
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
   if (ui_unpaired_count(m) > 0) {
      /* LOUDER THAN A DASH, BECAUSE IT IS NOT THE SAME THING.
       *
       * "---" means no reading, which happens for a dozen ordinary reasons --
       * out of range, warmup, a sleeping phone -- and none of them is
       * actionable. A sensor the OS has no bond with is different in kind:
       * nothing will ever arrive again, and the fix is one tap the user has
       * to be told to make. Red, and the same tap target that already opens
       * DEVICES, where the unpaired list says which sensor and offers the
       * RECONNECT that raises the pairing dialog. */
      (void)snprintf(big, sizeof big, "ERR");
      bigcol = UI_DANGER;
   } else if (m->reading.stale || m->reading.glu < 0 || !m->reading.has_cgm) {
      (void)snprintf(big, sizeof big, "---");
      bigcol = UI_MUTED;
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
    * gains far more than it spends, because the plot reserves nothing under
    * its threshold rows.
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
    * the two sentinels fired, not that the app has just started.
    *
    * NOT GATED ON have_reading: the trend beside it comes straight back from
    * the log at launch, so gating this on a live reading makes the row vanish
    * for a cadence after every start while its neighbour is already there.
    * The sentinel bounds below are the real test. */
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
      uint32_t bcol    = 0;
      const char *bmsg = banner_of(m, &bcol);
      if (bmsg)
         draw_str(px, fb, bx, bany, sc, bmsg, bcol);
   }
   draw_str(px, fb, bx, y, bigsc, big, bigcol);
   /* The NUMBER ITSELF opens the DEVICES screen -- the registry of everything
    * feeding it, where the primary is chosen (per device) and a new one is
    * added. The glyphs only, not the band: the hamburger (settings) and the
    * tab row keep their own pixels, and the band between them belongs to
    * neither. */
   add_hit_ix(h, ui_rect(bx, y, ink_w, 7 * bigsc), MA_DEVICES_OPEN, 0);
   /* Age bar: a thin bar under the number, exactly the footprint's ink
    * width (three digits: 5+1+5+1+5 cells) whatever the current digit
    * count. The full-length TRACK is always drawn in dark gray, so the
    * bar visibly ENDS -- the fill is readable as a fraction of the whole.
    * The live part draws on top in the number's own (dynamically
    * recolored) colour, filling left-to-right over one CGM cadence plus
    * sync slack (AGE_BAR_S); once overdue it becomes a full-length DASHED
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
      fill_rect(px, fb, bx3, bar_y, bar_w, bar_h, UI_BAR_AGE);
      if (a >= AGE_BAR_S) {
         int dash = 3 * sc; /* dash == gap */
         for (int dx = 0; dx < bar_w; dx += 2 * dash) {
            int seg = dash;
            if (dx + seg > bar_w)
               seg = bar_w - dx;
            fill_rect(px, fb, bx3 + dx, bar_y, seg, bar_h, bigcol);
         }
      } else {
         int fw = (int)(((long)bar_w * a) / AGE_BAR_S);
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
   draw_str(px, fb, colx, units_y, sc, UI_LBL(m->prefs.units), UI_TEXT_DIM);
   draw_str(px, fb, colx, tr_y, sc, tr, UI_TEXT_DIM);
   draw_str(px, fb, colx, pred_y, sc, pred, UI_TEXT_DIM);
   draw_str(px, fb, colx, agev_y, sc, agestr, UI_TEXT_DIM);
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
                UI_TEXT_DIM);
   /* The settings hit zone is the WHOLE band right of the number -- from
    * the number's ink edge to the screen edge, from the band top down
    * through the entire units row -- so it cannot be missed. The number
    * keeps its own pixels (they open the DEVICES screen). */
   int hx0 = bx3 + (foot_ink * bigsc3) + sc;
   /* Bounded by THIS COLUMN, not by the whole screen. In landscape the number
    * owns the left column only; a band running to fb->width would reach into
    * the plot column, where the tab row owns those pixels, and its centre
    * would resolve to a control the eye cannot hit -- the shape of a thing
    * that looks present and is not. Identical in portrait, where the column IS
    * the screen. */
   int hamslot =
       add_hit(h, ui_rect(hx0, y, (cx + cw) - hx0, (units_y + (7 * sc)) - y),
               ACT_OPEN_SETTINGS, 0);
   /* ...but the pressed highlight lights the hamburger GLYPH alone (plus a
    * little breathing room) -- the zone also contains the units label, and
    * a lit MG/DL would read as if the units were about to change. Through the
    * slot the band actually got: if it was dropped there is no band to narrow,
    * and the last box on the list is a different control entirely. */
   add_glow(h, hamslot,
            ui_rect(ham_x - (2 * sc), ham_y - (2 * sc), ham_w + (4 * sc),
                    ham_h + (4 * sc)));

   /* ONE LINE under the progress bar: which sensor owns the big number, its
    * plot marker, and how much of its session is left --
    * "G7-91-D1  X  GRACE 8H 35M". Everything worth saying about the primary
    * sensor -- which one it is, its state, its session and its prediction --
    * sits ON the number it describes, rather than in a table the eye has to
    * travel to. Left-aligned on the bar, so the three -- number, bar, line --
    * read as one block. */
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
       * margin, which is what gives the block a true edge on both sides.
       * draw_str lays out len cells of 6 units with no trailing gap after the
       * last, so the ink is (len*6 - 1)*sc wide; subtracting the CELL width
       * would leave a one-unit sliver past the bar. */
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
      draw_str(px, fb, bx3, pl_y, sc, pname, UI_TEXT_DIM);
      draw_str(px, fb, sx, pl_y, sc, sleft, scol);
      /* The marker sits at the exact MIDPOINT of the space between the two --
       * from the name's last ink column to the countdown's first. Centring
       * rather than a fixed offset from the name, so the gap reads as
       * deliberate whatever the name's length: a pinned marker sits hard
       * against a long name and leaves a ragged void before the countdown. */
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
      /* The line names a device, so tapping it opens that device's menu. */
      if (ps)
         add_hit_ix(
             h, ui_rect(bx3, pl_y - (2 * sc), (cx + cw) - bx3, gh + (4 * sc)),
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
   /* SETTINGS OPENS FROM THE HAMBURGER ONLY (added above), never from the
    * whole top band: a band-wide target means a tap on the number or the
    * trend navigates away by accident. */
   if (scrub) {
      char ts[16];
      /* THE VALUE ALONE, in whatever form its kind is measured: the unit and
       * the instant are separate fields with separate zones on the row, so
       * this holds only the number. */
      char gv[12];
      /* HOW WIDE THE VALUE FIELD IS, and it is per KIND rather than one width
       * for all of them.
       *
       * The field is centred and the number right-aligned inside it, so every
       * column of padding beyond what the value can actually need pushes the
       * digits that far right of centre. A glucose reading in mg/dL is three
       * digits at most; padding it out to hold a weight's five would sit it a
       * full glyph off centre for the whole of its life on screen. So each
       * kind declares the widest IT can be -- mmol/L needs a tenth, mg/dL
       * does not -- and the digits straddle the middle in every case. */
      int gvw = m->prefs.units ? 4 : 3;
      int ins = (m->plot.hist[m->plot.scrub].kind == KIND_INS);
      int wt  = (m->plot.hist[m->plot.scrub].kind == KIND_WT);
      int fd  = (m->plot.hist[m->plot.scrub].kind == KIND_FOOD);
      int exr = (m->plot.hist[m->plot.scrub].kind == KIND_EX);
      fmt_hms(m->plot.hist[m->plot.scrub].t, m->tz_off, ts, sizeof ts);
      ts[5] = '\0';
      /* An insulin dose scrubs like glucose, shown as "2 U FAST" /
       * "10 U SLOW" (src carries the dose TYPE for insulin points). A weight
       * scrubs the same way -- identical code path, identical readout shape --
       * converted from the stored GRAMS into whichever display unit is set,
       * so switching KG/LB re-renders history rather than relabelling it. */
      if (ins) {
         /* `glu` carries THOUSANDTHS for an insulin point (model.c), so the
          * dose is rendered rather than printed: "0.5 U", not "500 U". */
         char iu[16];
         (void)ins_units_str(m->plot.hist[m->plot.scrub].glu, iu, sizeof iu);
         (void)snprintf(gv, sizeof gv, "%s", iu);
         gvw = 5; /* "12.5", and thousandths can render "1.125" */
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
         gvw = 5; /* "150.5" */
      } else if (fd) {
         /* Grams, as stored. FOOD_MAX_G is 999, so three digits and the unit
          * fit `gv` with room to spare -- and fit the value column below
          * without widening it. */
         (void)snprintf(gv, sizeof gv, "%d", m->plot.hist[m->plot.scrub].glu);
         gvw = 3; /* FOOD_MAX_G is 999 */
      } else if (exr) {
         /* HOW LONG IT LASTED, which is the thing about a session that a
          * glance at the plot cannot give you: the rule shows roughly, this
          * says exactly. Minutes, because seconds are noise at this scale and
          * hours would round a 40-minute walk to nothing. A session with no
          * recorded end -- the app was killed, or it is still running -- has
          * no length to show, so the intensity carries the line alone. */
         /* NO "EX " PREFIX: the unit column beside this one already carries
          * LIGHT / MOD / HARD, so the letters would be saying a second time
          * what the next field says once -- and they are two of the six
          * columns the value field gets, which every other reading on the
          * plot then has to be padded out to. */
         long mins = m->plot.hist[m->plot.scrub].src / 60;
         if (mins <= 0)
            (void)snprintf(gv, sizeof gv, "--");
         else if (mins > 9999)
            (void)snprintf(gv, sizeof gv, "9999");
         else
            (void)snprintf(gv, sizeof gv, "%ld", mins);
         gvw = 4; /* minutes, clamped at 9999 above */
      } else {
         fmt_glu(m->plot.hist[m->plot.scrub].glu, m->prefs.units, gv,
                 sizeof gv);
      }
      /* On the multi-day spans a bare HH:MM is ambiguous across days, so
       * prefix the DATE as M/DD ("7/21"). A weekday name told you it was a
       * Thursday but not WHICH one, which is useless once the span passes a
       * week -- and at 30D it is useless immediately. */
      const char *unit = UI_LBL(m->prefs.units);
      if (ins) {
         unit = (m->plot.hist[m->plot.scrub].src == INS_FAST) ? "U FAST"
                                                              : "U SLOW";
      } else if (wt) {
         unit = wt_unit_name(m->prefs.wunits);
      } else if (exr) {
         /* The user's own words for the three levels, the same ones the
          * button shows -- a scrub that said "2" would be asking the reader
          * to remember a scale nothing on screen defines. */
         static const char *const exl[EX_MAX_LEVEL + 1] = {
             "MIN", "MIN LIGHT", "MIN MOD", "MIN HARD"};
         int lv = m->plot.hist[m->plot.scrub].glu;
         unit   = (lv >= EX_MIN_LEVEL && lv <= EX_MAX_LEVEL) ? exl[lv]
                                                             : exl[0];
      } else if (fd) {
         /* THE FOOD'S NAME, TRUNCATED TO WHAT THE LINE HOLDS.
          *
          * A name is up to FOOD_NAME_MAX (20) characters and this readout
          * also carries a value, a date and a time inside a 48-byte line --
          * so a long name would push the time off the end, and the time is
          * the part that says WHICH entry is being scrubbed. Cut to SIX, the
          * width of the unit column this readout keeps -- see the layout
          * below -- so a name can never widen the line and shift the fields
          * after it.
          *
          * A copy, not a borrowed pointer: `unit` is used further down and
          * food_type_name's answer is only valid while the vocabulary is
          * unchanged. An id the vocabulary does not hold answers "", which
          * renders as a blank name rather than a number -- the honest look of
          * an entry nothing can name. */
         static char fname[9];
         const char *nm = food_type_name(m->plot.hist[m->plot.scrub].src);
         int fi         = 0;
         for (; nm[fi] && fi < (int)sizeof fname - 1; fi++)
            fname[fi] = nm[fi];
         fname[fi] = 0;
         /* "G" AND THE NAME TOGETHER: the number is a quantity of grams OF
          * something, and both halves of that belong on the unit's side of
          * the reading. */
         static char fu[16];
         (void)snprintf(fu, sizeof fu, "G %s", fname);
         unit = fu;
      }
      /* WHEN IT HAPPENED, on the left. A bare HH:MM is ambiguous across days,
       * so the multi-day spans put the date in front of it: M/DD ("7/21") at
       * a month or more, where a weekday name tells you it was a Thursday but
       * not WHICH one, and the weekday itself within a week, because "TUE
       * 08:15" places a reading the way you actually remember it and there is
       * only one Tuesday to confuse it with. */
      char whenbuf[24];
      const char *whenp = ts;
      if (m->plot.plot_hours >= 720) {
         char dt[20];
         /* 12, not 8: gcc cannot prove the month is two digits, and this
          * build treats a possibly-truncating snprintf as an error. */
         char md[12];
         fmt_date(m->plot.hist[m->plot.scrub].t, m->tz_off, dt, sizeof dt);
         /* fmt_date gives "YYYY-MM-DD HH:MM"; take the month without its
          * leading zero, and the day as written. */
         int mon = ((dt[5] - '0') * 10) + (dt[6] - '0');
         (void)snprintf(md, sizeof md, "%d/%c%c", mon, dt[8], dt[9]);
         (void)snprintf(whenbuf, sizeof whenbuf, "%s %s", md, ts);
         whenp = whenbuf;
      } else if (m->plot.plot_hours >= 72) {
         /* 1970-01-01 was a Thursday. */
         static const char *const wd[7] = {"SUN", "MON", "TUE", "WED",
                                           "THU", "FRI", "SAT"};
         long z = (m->plot.hist[m->plot.scrub].t + m->tz_off) / 86400;
         int wi = (int)(((z % 7) + 4 + 7) % 7); /* 0 = Sunday */
         (void)snprintf(whenbuf, sizeof whenbuf, "%s %s", wd[wi], ts);
         whenp = whenbuf;
      }
      /* THE APP'S ONE READOUT LAYOUT -- when, value, unit, each anchored so
       * that sweeping the trace moves the number and nothing else. See
       * log_scrub_row, which every plot in the app draws through.
       *
       * FULL DOUBLE SCALE, unconditionally. The old single centred string had
       * to be stepped down a size on the dated spans to fit the date it had
       * gained; with the fields anchored to the plot's two edges instead of
       * packed end to end, the width they need is the width they occupy and
       * nothing has to give. */
      /* THE NUMBER IS RIGHT-ALIGNED IN A FIXED FIELD, and it is the field
       * that is centred.
       *
       * Centring the digits themselves would move them every time the reading
       * changed width: sweeping past 100 down to 90 slides the number half a
       * glyph left, which under a finger reads as the row twitching rather
       * than as the value falling. Padded to a constant width, the field's
       * centre never moves and the units digit stays in its column, so what
       * changes on screen is the digits and nothing else.
       *
       * The width is `gvw`, which each kind sets to what it can actually
       * need; see where it is declared. */
      char vpad[16];
      (void)snprintf(vpad, sizeof vpad, "%*s", gvw, gv);
      log_scrub_row(px, fb, cx + (2 * sc), y, cw - (4 * sc), sc, 2 * sc, whenp,
                    vpad, unit);
   } else {
      int laby = y + ((rowh - (7 * sc)) / 2);
      for (int i = 0; i < UI_TABS; i++) {
         char lab[12];
         int th = ui_tab_hours[i];
         ui_span_label(th, lab, sizeof lab);
         int lw   = str_len(lab) * 6 * sc;
         int tabx = cx + (i * colw);
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, lab,
                  ui_tab_hours[i] == m->plot.plot_hours ? UI_TEXT : UI_MUTED);
         /* arg carries the plot span in hours, so the shell needn't know the
          * tab list -- it just assigns it. */
         add_hit(h, ui_rect(tabx, tab_y, colw, tab_h), ACT_PLOT_TAB,
                 ui_tab_hours[i]);
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
    *   34 = what render_glucose itself adds after the plot: a 9 gap, then the
    *        threshold row at 7 row + 18 portrait pad. A row the reserve
    *        leaves out is a row the plot grows over, and the overlap would
    *        land on the thresholds -- the numbers whose whole purpose is to
    *        be readable at a glance.
    *  211 = render_info's own budget (needv: 4 info rows + gap + 4 stat rows +
    *        the banner's advance and glyph) plus the THIRD shortcut row.
    *
    *   16 = one blank line BELOW the alarm's large letters.
    *
    * 34 + 211 IS THE 59 + 186 IT REPLACES, and deliberately so: collapsing
    * the ALARM and NUDGE rows into one freed 25 of height, and that height
    * belongs to the shortcut grid rather than to the plot. Leaving the total
    * alone is what hands it over -- the plot ends where it always did and the
    * block below it is 25 taller.
    *
    * Reserving render_info's full budget keeps its font at sc (it only
    * downscales when squeezed), which is the point -- the plot grows into the
    * dead space, the text below it does NOT shrink. */
   int reserve = (34 + 211 + 16) * sc;
   int grow = fb->height - y - reserve; /* plot bottom = reserve from screen */
   int ph   = 0;
   if (landscape)
   /* 26, BECAUSE ONE THRESHOLD ROW COMES AFTER THE PLOT. In landscape the
    * plot is sized by subtracting what follows it: a 9 gap, then 7 glyph +
    * 6 landscape pad, i.e. 22, plus 4 sc of slack.
    *
    * A NUMBER TOO SMALL HERE DOES NOT CROP THE PLOT -- it puts the threshold
    * row and all five of its tap targets below the buffer, on every
    * wide-short geometry. This app does not scroll, so that is drawn nowhere
    * and tappable nowhere.
    *
    * The shortcut grid is in the OTHER column in landscape, so the row freed
    * by collapsing ALARM and NUDGE has nothing to be handed to here and goes
    * to the plot. */
   /* THREE QUARTERS of what is left, not all of it. The rest is the
    * threshold row (26*sc).
    *
    * The banner needs no reservation here: it is above the big number, in the
    * other column. The three-quarter cap stays anyway, because what it
    * protects is the threshold rows' own room on a short window, not
    * the banner's. */
   {
      ph      = bottom - y - (26 * sc);
      int cap = ((bottom - y) * 3) / 4;
      if (ph > cap)
         ph = cap;
   } else {
      /* A FLOOR OF 12*bigsc, and growth only into genuine excess above it. A
       * plot that shrinks on a short screen takes the space from itself
       * rather than from the rows below, which is the wrong way round: those
       * rows have tap targets and this does not. */
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
/* A LONG span returns up to PLOT_LONG_MAX points (plotdata.h), which is far
 * more than the live window holds. Sized for the LARGER of the two: too
 * small and the older half of a 30-day plot is silently cut off. */
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
      /* EVERY POINT IS AN INSTANT UNLESS IT SAYS OTHERWISE. Set here, before
       * the kinds below, because `pts` is a static that outlives the frame:
       * a span left over from the last frame's exercise entry would draw a
       * rule from an unrelated point. */
      pts[i].span = 0;
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
      /* A logged FOOD entry: the same bottom line, drawn as a small F. Fixed
       * shape and colour like the weight's W -- there is one food series on
       * this axis, however many food TYPES exist, so there is nothing to tell
       * apart by styling; the scrub names the food. */
      if (m->plot.hist[i].kind == KIND_FOOD) {
         pts[i].glu    = 60;
         pts[i].marker = PLOT_MARK_F;
         pts[i].col    = UI_MARK_FOOD;
         pts[i].size   = MARK_SIZE_DEF;
         continue;
      }
      if (m->plot.hist[i].kind == KIND_WT) {
         pts[i].glu    = 60;
         pts[i].marker = PLOT_MARK_W;
         pts[i].col    = UI_MARK_WT;
         pts[i].size   = MARK_SIZE_DEF;
         continue;
      }
      /* A logged EXERCISE session: the same bottom line as the doses, drawn
       * as a small E in the button's own blue so the plot and the control
       * agree about what exercise looks like. `src` carries the LENGTH in
       * seconds (model.c), which becomes the rule drawn beside the letter --
       * the one point on this plot that is a stretch of time rather than an
       * instant. */
      if (m->plot.hist[i].kind == KIND_EX) {
         pts[i].glu    = 60;
         pts[i].marker = PLOT_MARK_E;
         pts[i].col    = ui_ex_color(m->plot.hist[i].glu, UI_EX_LIGHT);
         pts[i].size   = MARK_SIZE_DEF;
         pts[i].span   = m->plot.hist[i].src;
         continue;
      }
      int matched = 0;
      for (int k = 0; k < m->dev.nsensors; k++) {
         /* Pre-registry legacy readings (src 0) match NO sensor and keep the
          * default value-based styling below.
          *
          * NOT ATTRIBUTED TO THE PRIMARY, however tempting: the primary flag
          * is mutable, so the moment a freshly paired G7 is made primary,
          * days of another sensor's legacy data would flip to the G7's colour
          * and marker on the plot -- a provenance
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
       * and colour -- consistent with what the DEVICES menu draws for a
       * retired slot. Only a
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
    * radius keeps the shape of the trace readable rather than smearing it
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
   /* ONE configuration, drawn with and then recorded for the touch path.
    * Two compound literals would be two things to keep in step, and the touch
    * path cannot see the scale and span the renderer derived. */
   struct plot_cfg pcfg = {m->plot.plot_max, prad};
   plot_render((struct plot_fb){px, fb->stride, fb->width, fb->height},
               (struct plot_rect){plot_x, plot_y, plot_w, ph}, pts, np, m->now,
               m->plot.plot_hours, pcfg, white_color,
               scrub ? m->plot.scrub : -1, UI_HILITE, m->tz_off);

   /* THE RANGE'S OWN EDGES, NAMED.
    *
    * plot.c draws a faint band between 70 and 180 and a thin line along each
    * edge, and until now nothing said what those two lines were. They are the
    * numbers the whole screen is about -- the alarm thresholds are set
    * against them and the TIR figure counts against them -- so a reader who
    * does not already know them is looking at two anonymous rules.
    *
    * IN THE READING'S OWN UNITS, through fmt_glu, so the pair says 3.9 and
    * 10.0 to somebody whose app is set to mmol/L. The band itself is fixed in
    * mg/dL (stats.h owns the one definition); only its label converts.
    *
    * THE HEIGHT COMES FROM plot_point_xy, which is the renderer's OWN mapping
    * exported for exactly this -- recomputing glu_to_y here would be a second
    * copy of the axis, and a label a pixel off the line it names is worse
    * than no label.
    *
    * SMALL AND DIM, sitting just above its line at the left margin: the trace
    * is what the plot is for, and these are a legend for it. Same grey as the
    * date ticks along the bottom, which are the plot's other annotation.
    *
    * FONT_NOTE, the size the app keeps for marks inside a plot -- see the
    * ladder in font.h. It lands within a pixel of the W and F glyphs beside
    * it, which is what a legend for this plot should read as. */
   {
      static const int edge[2] = {TIR_HIGH_MGDL, TIR_LOW_MGDL};
      const int lsc            = FONT_NOTE(sc) < 1 ? 1 : FONT_NOTE(sc);
      for (int i = 0; i < 2; i++) {
         char lab[12];
         int ex = 0;
         int ey = 0;
         if (!plot_point_xy((struct plot_rect){plot_x, plot_y, plot_w, ph},
                            (struct plot_pt){m->now, edge[i], 0, 0, 0, 0, 0},
                            m->now, m->plot.plot_hours, pcfg, &ex, &ey))
            continue;
         fmt_glu(edge[i], m->prefs.units, lab, sizeof lab);
         draw_str(px, fb, plot_x + (2 * lsc), ey - (8 * lsc), lsc, lab,
                  UI_DISCLAIM);
      }
   }
   /* THE IN-RANGE STREAK, upper right, inside the plot.
    *
    * Drawn only while the streak is running -- 0 means the newest reading is
    * out of range, and a "STREAK 0 MIN" would be a consolation prize for the
    * one state it is not meant to celebrate. Grey and normal-sized: it is a
    * fact about the trace, not a control and not a headline.
    *
    * RIGHT-ALIGNED, so the number grows leftward and the label never moves as
    * the units change from minutes to hours to days.
    *
    * AFTER plot_render, so it sits on top of the trace rather than under it,
    * and inside the plot rect rather than above it, where the tab row lives
    * and the scrub readout replaces it. */
   if (m->plot.streak_s > 0) {
      /* 40, and clamped below. A long is up to 19 digits and the compiler
       * cannot see that a streak is bounded by the history's own span, so at
       * 24 it treats the format as possibly truncating -- which this build
       * makes an error rather than letting the label be cut in half. The
       * clamp is the honest half of it: a streak of more than a year is not
       * a number this screen should be trying to render. */
      char st[40];
      long ss = m->plot.streak_s;
      if (ss > 365L * 86400)
         ss = 365L * 86400;
      long bb = m->plot.best_streak_s;
      if (bb > 365L * 86400)
         bb = 365L * 86400;
      /* IS THIS THE BEST ONE YET? Then it says so and nothing else: a record
       * being broken is the whole news, and a bar showing 100% of a target
       * already passed would be reporting the smaller fact. `>=` because
       * equalling the record is reaching it, and because the record is raised
       * on the reading path -- a streak that has just passed it reads its own
       * value back here one reading later. */
      const int is_best = (bb <= 0) || (ss >= bb);
      /* 8 IS THE PROVABLE BOUND, and the compiler is the reason it is not 16:
       * streak_len's widest answer is "59 MIN" or "365 D", but with a 16-byte
       * destination gcc must assume 15 characters twice and cannot show the
       * combined line fits -- which this build treats as an error rather than
       * letting a number be cut in half. */
      char cur[8];
      char bst[8];
      streak_len(ss, cur, sizeof cur);
      streak_len(bb, bst, sizeof bst);
      if (is_best)
         (void)snprintf(st, sizeof st, "BEST STREAK %s", cur);
      else
         (void)snprintf(st, sizeof st, "STREAK %s (BEST %s)", cur, bst);
      /* INSET BY HALF A CHARACTER HEIGHT, down and left.
       *
       * A glyph is 7*sc tall at this scale, so the inset is (7*sc)/2 on both
       * axes. Hard against the plot's corner the text read as part of the
       * frame rather than as something written inside it; half a character is
       * enough air to separate them and small enough that the label still
       * belongs to the plot rather than floating above it. */
      int inset = (7 * sc) / 2;
      int sw    = ((str_len(st) * 6) - 1) * sc;
      int sx    = plot_x + plot_w - sw - (2 * sc) - inset;
      if (sx < plot_x)
         sx = plot_x;
      int sy = plot_y + (2 * sc) + inset;
      draw_str(px, fb, sx, sy, sc, st, UI_MUTED);
      /* HOW FAR ALONG THE RUN IS, as a rule under the whole line.
       *
       * Only while the record still stands: once it is beaten the text says
       * so and a bar has nothing left to measure. Two rows of pixels, the
       * width of the text: a dark track with the earned part lit over it. It
       * is a progress bar for something nobody is racing, so it reads as an
       * underline that happens to be partly lit rather than as a control.
       *
       * THE LIT SHADE IS ITS OWN, one step off the label's grey and drawn
       * nowhere else in the app. Not for looks -- at these two rows nobody
       * could tell them apart -- but so a test can say "no bar was drawn"
       * and mean it. With the lit part in the label's own colour, a bar
       * drawn at FULL width covers its dark track completely and is then
       * indistinguishable, in pixels, from no bar at all: a mutant that drew
       * the bar after the record was beaten survived exactly that way.
       *
       * The fraction is computed in LONG and clamped to the bar: ss and bb are
       * both bounded to a year above, so ss * sw cannot overflow, and the
       * clamp covers the frame between passing the record and the reading
       * that raises it. */
      if (!is_best && bb > 0) {
         long fill = (ss * (long)sw) / bb;
         if (fill < 0)
            fill = 0;
         if (fill > sw)
            fill = sw;
         int bh = sc < 2 ? 1 : 2;
         /* ONE BAR HEIGHT OF AIR BELOW THE TEXT, on top of the half-glyph
          * gap: at two rows of pixels the rule sat close enough to the
          * letters' baseline to read as an underline of the words rather
          * than a measure of the run. */
         int by = sy + (7 * sc) + (sc < 2 ? 1 : sc / 2) + bh;
         fill_rect(px, fb, sx, by, sw, bh, UI_BAR_STREAK);
         if (fill > 0)
            fill_rect(px, fb, sx, by, (int)fill, bh, UI_BAR_FILL);
      }
   }
   /* the whole plot rect scrubs; the shell resolves the datapoint via plot_hit
    */
   add_hit(h, ui_rect(plot_x, plot_y, plot_w, ph), ACT_SCRUB, 0);
   /* ...WITH THE CONFIGURATION IT WAS DRAWN WITH. plot_hit must reproduce
    * this mapping exactly, and the radius above is derived from the screen
    * scale and the span -- neither of which the touch path can see. It used
    * to read a process global this render had set, which answered for
    * whichever plot was drawn last. */
   h->plot = pcfg;
   y += ph + (9 * sc);

   /* The threshold row: all four values on one line, low to high, each
    * marked by how far out of band it is (see thresh_row).
    *
    * ALIGNED TO THE PLOT, inset one character each side. The row and the plot
    * are the two full-width blocks in this column and they sit one above the
    * other, so ink that starts and ends anywhere else reads as a misalignment
    * however tidy each block is on its own -- which is what the old edges
    * (the progress bar's left, the units label's right) did. The character of
    * inset keeps the outermost arrow and digit off the plot's frame rather
    * than flush against it. */
   /* Left edge = the progress bar's leftmost pixel (bx3); right edge = the
    * right ink edge of the units label beside the big number. Both are locals
    * of THIS function -- the number, the bar and these rows are all laid out
    * here -- so the margins are shared by construction rather than by two
    * copies of the same arithmetic that could drift apart. */
   y = thresh_row(fb, m, h, plot_x + (6 * sc), plot_x + plot_w - (6 * sc), y,
                  sc, pad);
   /* NO BANNER HERE. It rides above the big number in both orientations, so
    * neither column places it and neither has to know it exists. */
   return y;
}

/* Right/bottom column: the rolling-stats table, the ADD '+' and the alarm
 * banner. What a PRIMARY / STATE / SESSION / PRED table would say rides on the
 * big number itself instead -- the prediction in the label column, the sensor
 * and its countdown on one line under the progress bar -- because that is
 * where the eye already is. */
/* THE OUT-OF-RANGE BANNER -- STALE / LOW / HIGH.
 *
 * ONLY THE WORDING AND THE COLOUR are decided here; the caller places it.
 *
 * A DECIDER, NOT A DRAWER, and that split is what keeps the height budget in
 * one place. When this drew itself, both column layouts had to reserve
 * UI_BANNER_H before deciding their own heights, and the block that forgot did
 * not fail -- it drew the banner off the bottom of the buffer,
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
   /* STALE IS THE ONLY BANNER LEFT.
    *
    * LOW and HIGH used to sit here too. The threshold row below the plot now
    * reverses the breached number into its band's colour, which says the same
    * thing and says it against the limit that was crossed -- the banner could
    * only name a direction. Two announcements of one fact, one of them less
    * specific, is the kind of duplication that gets one of them changed and
    * not the other.
    *
    * STALE stays because nothing else on this screen says that readings have
    * stopped arriving, and the row cannot: it is about thresholds, and a
    * reading that is not coming is not breaching one. */
   if (m->reading.disc_alarmed) {
      msg = "STALE";
      /* Banner-only, deliberately not a colour the big number can take: it is
       * what makes an offline "is the banner visible" check mean something
       * rather than passing on the number underneath it. */
      c = UI_BANNER;
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
   /* NO BANNER TERM, in either orientation -- the banner is not in this
    * column any more, so this block must not charge itself for it.
    *
    * Charging for a row that is not drawn floors this block's vsc a whole
    * step and renders the stats table in a tiny font: the row count never
    * changes, the space it is given does. Any block that derives its scale
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
   /* AND EVERY SHORTCUT ROW PAST THE FIRST. 28 = the button (25) and the 3 of
    * air between rows -- rowpitch, below, in the same units.
    *
    * Counted HERE, before the scale is chosen, because this block sizes its
    * own font from the space it is given: a row added after the fact does not
    * make the block taller, it makes the block overflow. Every element this
    * function draws has to be in this number before vsc is derived from it. */
   int npin = 0;
   for (int i = 0; i < SC_MAX; i++)
      if (m->prefs.shortcut[i] > 0)
         npin++;
   int needv  = 7 + (4 * 16) + 24 + 3 + ((pin_rows(npin) - 1) * 28);
   int availv = fb->height - y;
   int vsc    = availv > 0 ? availv / needv : 1;
   int hsc    = cw / (2 + (35 * 6)); /* 35 = the stats table's fixed width */
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
   const uint32_t col = UI_TEXT_DIM;

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
   draw_str(px, fb, tx, y, sc, row, UI_MUTED);
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
    * SETTINGS. It follows the last thing still drawn in this column. Its hit
    * zone is the whole band right of the table's ink, so the surrounding
    * space is pressable too -- a bare glyph is a poor target one-handed. */
   {
      int psc = FONT_HUGE(sc);
      int pw  = 6 * psc;
      int ph  = 7 * psc;
      int pxx = cx + cw - pw - (2 * sc);
      /* COUNTED AS RENDERABLE, not as stored.
       *
       * A slot can hold a pin this build does not offer -- that is the whole
       * reason pins are stored by identity and ui_shortcut_slot_by_id can
       * answer -1 -- and the layout loop below skips those. Counting them here
       * would shape the rows around buttons that are never drawn: four stored
       * pins one of which is unknown would reserve two rows and fill them 2+1,
       * leaving a hole where the missing button would have been. */
      /* COUNTED BY WALKING THE TABLE, which is the same walk the drawing
       * loop below makes. Counting the pin list instead would disagree with
       * it if a stored list ever held one id twice -- the budget would
       * reserve a button the loop draws once. */
      int nsc = 0;
      for (int slot = 0; slot < ui_shortcut_count(); slot++)
         if (pin_has(&m->prefs, ui_shortcut_id(slot)))
            nsc++;
      /* The packing rule and its one definition are pin_rows / pin_percol
       * above -- the same answer the height budget was derived from. */
      int nrows  = pin_rows(nsc);
      int percol = pin_percol(nsc);
      /* Pitch between rows: the button (25*sc, see menu_button) and 3*sc of
       * air. Named because the '+' is placed from it too. */
      int rowpitch = (25 * sc) + (3 * sc);
      /* AIR BETWEEN THE STATS TABLE AND THIS ROW. The table is a block of
       * numbers and this row is a set of controls, and at 2*sc they touched.
       *
       * Taken only out of SLACK: try a full line, then half, then none, and
       * take the first that still leaves the row itself on the screen.
       *
       * NOTHING IS HELD BACK FOR THE BANNER, which is above the big number
       * and not beneath this column: on a short window the air survives here
       * and the bottom of the column is free for the second shortcut row.
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
       * ANCHORED TO THE FIRST ROW, not the last: on the last one its position
       * depends on how many things happen to be pinned, so the one control
       * here that opens a whole menu moves whenever an unrelated button is
       * pinned or unpinned. On the first it is in the same place whatever the
       * row count, and
       * the empty space falls BELOW it beside the second row, which reads as
       * the group of actions having grown rather than the '+' having
       * wandered. */
      int plusy = pyy;
      draw_str(px, fb, pxx, plusy, psc, "+", UI_TEXT_DIM);
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
            int slotn = 0;    /* how many pins have been PLACED, not scanned */
            /* IN THE ADD MENU'S ORDER, NOT THE ORDER THEY WERE PINNED.
             *
             * The pin list is chronological -- it is a set, kept dense, in
             * whatever sequence the boxes happened to be ticked -- so walking
             * it laid the buttons out differently on two phones holding the
             * same four pins, and MOVED them when one was unpinned and
             * re-pinned. A control that logs a medication must be in the same
             * place every time it is looked for, and the place a user learns
             * it from is the menu they pinned it in. So iterate the TABLE and
             * draw the entries that are pinned, which makes the order a
             * property of the build rather than of the tapping history.
             *
             * ui_shortcut_menu_nth is that order, and it lives beside the ADD
             * menu's own drawing so the two cannot drift apart. */
            for (int k = 0; k < ui_shortcut_count(); k++) {
               const int slot = ui_shortcut_menu_nth(k);
               if (slot < 0 || !pin_has(&m->prefs, ui_shortcut_id(slot)))
                  continue;
               /* COUNTED ON PLACEMENT, never on the loop index: the table
                * holds entries that are not pinned, so `slot` and the
                * button's position part company at the first one skipped --
                * and the row would break where the gap was rather than after
                * percol buttons. */
               int r = slotn / percol;
               int c = slotn % percol;
               slotn++;
               int bx          = rowl + (c * (bwid + sgap));
               int code        = ui_shortcut_code(slot);
               /* THE NAME FOR THIS ROW'S WIDTH, then shorter forms if even
                * that will not fit -- a narrow screen can defeat any of them.
                * The font never shrinks: a half-word on a button that logs a
                * medication is worse than no word. */
               /* THE BULLET'S WIDTH IS RESERVED BEFORE THE NAME IS PICKED
                * -- 5*sc of dot and a 6*sc cell of gap. Choosing the label
                * against the bare button and then drawing a mark beside it
                * would spend width the fit had already promised to the
                * words. */
               const int due = (code == MA_WT_OPEN && ui_weight_due(m))
                               || (code == MA_INS_SLOW && ui_slow_ins_due(m));
               const int mark = due ? 11 * sc : 0;
               const char *lbl = ui_shortcut_label(slot, percol);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc) - mark)
                  lbl = ui_shortcut_label(slot, 3);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc) - mark)
                  lbl = "";
               /* EXERCISE IS NOT A LABEL. Pinned here it must show the
                * level it is on, in the colour that encodes it, with the
                * settling bar -- otherwise the pinned copy and the one in the
                * ADD menu would disagree about the same value. One function
                * draws both. */
               if (code == MA_EXERCISE)
                  (void)ui_exercise_button(
                      fb, h, bx, pyy - (2 * sc) + (r * rowpitch), bwid, sc,
                      m->food.ex_level, m->food.ex_remaining, EX_SETTLE_S, lbl,
                      UI_TEXT_DIM);
               /* THE SAME BULLET THE ADD MENU DRAWS, off the same test: the
                * pinned copy of a button and the one in the menu must never
                * disagree about what they are saying. */
               else if (mark)
                  (void)menu_button_mark(
                      fb, h, bx, pyy - (2 * sc) + (r * rowpitch), bwid, sc, lbl,
                      UI_TEXT_DIM, UI_MARK_WT, code, 0);
               else
                  (void)menu_button(fb, h, bx, pyy - (2 * sc) + (r * rowpitch),
                                    bwid, sc, lbl, UI_TEXT_DIM, code, 0);
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
      add_hit_ix(h,
                 ui_rect(hx0, plusy - (3 * sc), cx + cw - hx0, ph + (6 * sc)),
                 MA_ADD_OPEN, 0);
      /* THE BLOCK ENDS BELOW THE LAST ROW, which is not where the '+' is:
       * with two rows the buttons extend a full rowpitch past it. Nothing
       * below reads `y` -- this is the last thing the main screen draws -- so
       * the running cursor is not advanced here: a number that looks
       * load-bearing and is not is worse than no number. */
   }

   /* THE ROUTE TO A REPLACEMENT SENSOR is the main screen's '+' (ADD -> NEW
    * DEVICE): permanent, and calm. An expiry prompt on this screen would read
    * as an error banner and confuse more than it helps. The
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
   add_hit(h, ui_rect(0, y - (2 * sc), fb->width, 14 * sc), ACT_OPEN_SETTINGS,
           0);
   /* "PANCRA  " + a UI_COLS-long status needs more than UI_COLS+1. The
    * status is snapshotted at UI_COLS, so budget the prefix on top. */
   char line[UI_COLS + 12];
   char st[UI_COLS + 1];
   /* No null test: the frame CARRIES the text now rather than pointing at it
    * (uimodel.h), so there is nothing that could be absent. */
   str_snapshot(st, sizeof st, m->status);
   (void)snprintf(line, sizeof line, "PANCRA  %s", st);
   draw_str(px, fb, 2 * sc, y, sc, line, UI_TEXT);
   y += 9 * sc;
   /* total rounded to 10s so ambient chatter doesn't churn the line */
   (void)snprintf(line, sizeof line, "ADV %u  DX %d",
                  (m->dev.adv_total / 10) * 10, m->dev.ndev);
   draw_str(px, fb, 2 * sc, y, sc, line, UI_TEXT);
   y += 9 * sc;

   if (m->dev.ndev > 0) {
      y += 3 * sc;
      draw_str(px, fb, 2 * sc, y, sc, "SENSORS", UI_MUTED);
      y += 10 * sc;
      for (int i = 0; i < m->dev.ndev; i++) {
         char dl[48];
         (void)snprintf(dl, sizeof dl, "%-8s %4d %s", m->dev.devs[i].name,
                        m->dev.devs[i].rssi, m->dev.devs[i].mac);
         draw_str(px, fb, 2 * sc, y, sc, dl, UI_TEXT_DIM);
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
          * LANDSCAPE EXISTS TO GIVE THE PLOT ROOM, so the plot gets a column
          * of its own. Sharing the left column with the number leaves it a
          * sliver of height while the right column holds a stats table and a
          * lot of empty space. */
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
