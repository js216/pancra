// SPDX-License-Identifier: GPL-3.0
// uimenu.c --- Settings, and the modal screens reached from them (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "uimenu.h"
#include "font.h"
#include "exercise.h"
#include "insulin.h" /* struct ins_rec + INS_* for the INSULIN LOG table */
#include "ndk.h"
#include "plot.h"
#include "sensors.h"  /* sensor types, kinds, marker enum */
#include "settings.h" /* SET_NCOLORS: crosschecked below */
#include "style.h"
#include "syncstat.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include "weight.h" /* wt_unit_name / wt_to_tenths: the weight rows */
#include <stdint.h>
#include <stdio.h> /* snprintf */

void render_settings(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Bounded by height as well as width -- see ui_fit_scale. Worst case is
    * title (2) + five submenu rows each with a blank line after (10) + the
    * separator and the EXPORT DATA button (1 + 25/16). */
   int sc  = ui_fit_scale(fb->width, fb->height, 15);
   int tsc = 2 * sc;
   int lh  = 16 * sc; /* generous pitch: a blank line between rows */
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   /* title with a right-aligned X to close */
   draw_str(px, fb, x, y, tsc, "SETTINGS", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* generous close target: title + blank line + DISPLAY header */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 3 * lh, MA_CLOSE, 0);
   /* The same 3*lh title gap as every submenu this screen leads to. SETTINGS
    * was itself the odd one out at 2*lh, so the gap CHANGED as you stepped
    * from it into DISPLAY or DEVICES -- the inconsistency was most visible on
    * exactly the screen the others are entered from. */
   y += 3 * lh;

   /* Five submenu rows -- DISPLAY / DEVICES / ALARM / PERMISSIONS / REMOTE --
    * with a blank line between each, so the doors read as one calm block.
    * (The alarm settings live on their own submenu now, render_alarm.)
    *
    * NO ELLIPSIS on any of them. "DISPLAY ..." was the only row carrying one,
    * and it said nothing the row did not: every entry here is a door, so an
    * ellipsis on one of five marks nothing and just reads as ragged.
    *
    * DEVICES is a door, not a list: the registry used to be inlined here,
    * which tied the settings screen's height to how many sensors were paired
    * and pushed EXPORT DATA off the bottom. It now has its own screen -- the
    * same one the main screen's big number opens. Its value is the live device
    * count, so the common question ("is everything still connected?") is
    * answered without opening it. */
   menu_row(fb, h, y, sc, lh, "DISPLAY", "", 0xFFFFFFFF, MA_DISPLAY_OPEN, 0);
   y += 2 * lh;
   {
      int nlive = 0;
      int nconn = 0;
      for (int i = 0; i < m->dev.nsensors; i++)
         if (!m->dev.sensors[i].old) {
            nlive++;
            nconn += (m->dev.sensors[i].connected != 0);
         }
      /* Bounded by MAX_SLOTS in practice, but the compiler only sees an int:
       * clamp so the format cannot be truncated. BOTH ENDS -- only the upper
       * one was clamped, and gcc's range for the value was therefore
       * [INT_MIN, 99], which is 11 characters, not 2. It said so at -O1 (the
       * sanitizer build's optimisation level) and not at -O2, so the gate
       * that runs at -O2 never saw it. */
      if (nlive > 99)
         nlive = 99;
      if (nlive < 0)
         nlive = 0;
      if (nconn > 99)
         nconn = 99;
      if (nconn < 0)
         nconn = 0;
      char dv[24];
      (void)snprintf(dv, sizeof dv, "%d OF %d ON", nconn, nlive);
      menu_row(fb, h, y, sc, lh, "DEVICES", dv,
               (nlive > 0 && nconn == nlive) ? 0xFF33FF88 : 0xFFAAAAAA,
               MA_DEVICES_OPEN, 0);
      y += 2 * lh;
   }
   menu_row(fb, h, y, sc, lh, "ALARM", "", 0xFFFFFFFF, MA_ALARM_OPEN, 0);
   /* The row's "value" is the SAME icon language the two main-screen
    * threshold rows use -- speaker / phone / slashed circle / dot -- in the
    * SAME fixed, equally spaced cells (6*sc pitch, right-aligned), each
    * symbol always in its own place with an empty cell when that alert is
    * off.
    *
    * SIX cells, in two groups of three: the door leads to both sections, so
    * summarising only the ALARM half would say a nudge is silent when it is
    * not. Group order matches the menu -- ALARM's sound/vibration/disconnect,
    * a half-cell gap, then NUDGE's sound/vibration/new-datapoint. */
   {
      int iax = rx - (38 * sc);
      if (m->prefs.sound_on)
         draw_icon(px, fb, iax, y, sc, icon_speaker, 0xFF888888);
      if (m->prefs.vib_on)
         draw_icon(px, fb, iax + (6 * sc), y, sc, icon_vibrate, 0xFF888888);
      if (m->prefs.disc)
         draw_icon(px, fb, iax + (12 * sc), y, sc, icon_nolink, 0xFF888888);
      int nax = iax + (21 * sc); /* 3 cells + a half-cell group gap */
      if (m->prefs.nudge_sound)
         draw_icon(px, fb, nax, y, sc, icon_speaker, 0xFF888888);
      if (m->prefs.nudge_vib)
         draw_icon(px, fb, nax + (6 * sc), y, sc, icon_vibrate, 0xFF888888);
      if (m->prefs.newdata_mode)
         draw_icon(px, fb, nax + (12 * sc), y, sc, icon_dot, 0xFF888888);
   }
   y += 2 * lh;
   /* PERMISSIONS: one summary row -- green OK when everything a CGM needs
    * is granted, red CHECK otherwise -- opening the full submenu. */
   {
      int ok = m->sys.perm[0] && m->sys.perm[1] && m->sys.perm[2] &&
               m->sys.batt_ok && !m->sys.bg_restricted;
      menu_row(fb, h, y, sc, lh, "PERMISSIONS", ok ? "OK" : "CHECK",
               ok ? 0xFF33FF88 : 0xFF4466FF, MA_PERMS_OPEN, 0);
      y += 2 * lh;
   }
   /* REMOTE: the value is the push state -- and, when ON, the age of the
    * last push the server actually acknowledged (2xx) -- so whether
    * datapoints are leaving the phone AND arriving is visible without
    * opening the submenu. */
   char rmv[16] = "OFF";
   if (m->sync.remote_on) {
      if (m->sync.remote_last_ok > 0) {
         char rago[12];
         fmt_ago(m->now, m->sync.remote_last_ok, rago, sizeof rago);
         (void)snprintf(rmv, sizeof rmv, "ON %s", rago);
      } else {
         /* no acknowledged push yet this launch: a bare ON ("ON NEVER"
          * read as if the feature had never worked) */
         (void)snprintf(rmv, sizeof rmv, "ON");
      }
   }
   menu_row(fb, h, y, sc, lh, "REMOTE", rmv,
            m->sync.remote_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_REMOTE_OPEN, 0);

   /* EXPORT DATA closes the screen out: build the combined CSV and open the
    * system share sheet. The device registry used to sit here, inline; it
    * lives on its own screen now (render_devices, the DEVICES row above), so
    * this screen's height no longer grows with the sensor count.
    *
    * THREE gaps: the button is the one thing on this screen that is not a
    * door into a submenu, and it writes a file and opens a share sheet, so it
    * gets visibly more air than the rows above have between them.
    *
    * ...FALLING BACK TO TWO when the window cannot afford the third. The
    * alternative was raising this screen's row budget, and that drops
    * ui_fit_scale a whole step on a 1080-tall landscape window -- shrinking
    * every label on the screen to buy one blank line. Giving the line up on
    * exactly the geometries that cannot hold it costs nothing anywhere else:
    * every portrait phone keeps the full gap AND its font size. Measured
    * against the button's real height (25*sc, see menu_button). */
   {
      int air = 3 * lh;
      if (y + air + (25 * sc) > fb->height)
         air = 2 * lh;
      y += air;
   }
   menu_button(fb, h, x, y, fb->width - (2 * x), sc, "EXPORT DATA", 0xFFFFFFFF,
               MA_EXPORT, 0);
}

/* ---- DEVICES: the whole registry on one screen -- the LIVE devices, a door
 * to the OLD (disconnected) ones, and ADD NEW DEVICE. Reached from the main
 * screen's big number and from the SETTINGS row; both record their origin, so
 * the X returns exactly where the user came from. Never scrolls -- see
 * ui_sensor_capacity(). ---- */

/* Is this CGM's session over for good? The sensor's own verdict outranks
 * arithmetic (same rule as the main screen); otherwise it is over once the
 * session has run past the wear budget AND the grace window on top of it -- a
 * sensor still inside grace keeps streaming, so it is still a legitimate owner
 * of the big number. A sensor with no budget or no session yet is not
 * expired: it has not had its chance. */
int cgm_expired(const struct ui_sensor *s)
{
   if (s->sess_state == SENSOR_STATE_ENDED)
      return 1;
   if (s->wear_len <= 0 || s->session_seconds <= 0)
      return 0;
   return s->session_seconds > s->wear_len + SENSOR_GRACE_S;
}

/* THE LIST'S STATE COLUMN, ABBREVIATED TO FOUR CHARACTERS.
 *
 * Every state a device can report is spelled here at one fixed width, so the
 * column is a column: the eye reads down it instead of re-measuring each row,
 * and the width it does NOT vary by is width the PRIM column can have. The
 * long forms ran from three characters ("OFF") to fifteen ("CONFIRM PAIRING"),
 * and the longest of them decided how far left everything else had to start.
 *
 * The full wording is not lost -- the per-device screen's STATE row still
 * prints s->status verbatim, and that is the screen you open when a state
 * needs explaining. This one is an overview.
 *
 * Mapped from the string rather than from a code because the string is what
 * the model carries: main.c composes CGM states itself and passes the meter's
 * through from the driver's own phase text. An unrecognised state falls back
 * to its first four characters, so a new one is truncated, never blank.
 *
 * PAIR covers both "the bond dialog is waiting" (CGM) and "this meter is not
 * bonded" (meter): in each case nothing proceeds until the user pairs it. */
const char *dev_state_abbrev(const char *st, char *out, int n)
{
   static const struct {
      const char *full, *ab;
   } tab[] = {
       /* CGM */
       {"CONFIRM PAIRING", "PAIR"},
       {"CONNECTED",       "CONN"},
       {"WAITING",         "WAIT"},
       {"WARMUP",          "WARM"},
       {"ENDED",           "OVER"},
       /* meter: the driver's phase text, then its resting states */
       {"HELLO",           "BUSY"},
       {"COUNT",           "BUSY"},
       {"SYNCING",         "BUSY"},
       {"SYNCED",          "SYNC"},
       {"NOTHING NEW",     "SYNC"},
       {"NOT PAIRED",      "PAIR"},
       {"REFUSED",         "DENY"},
       {"BAD DATA",        "JUNK"},
       {"OFF",             "OFF" },
   };

   for (int i = 0; i < (int)(sizeof tab / sizeof tab[0]); i++) {
      const char *a = st;
      const char *b = tab[i].full;
      while (*a && *a == *b) {
         a++;
         b++;
      }
      if (*a == 0 && *b == 0)
         return tab[i].ab;
   }
   int k = 0;
   while (k < 4 && k < n - 1 && st[k]) {
      out[k] = st[k];
      k++;
   }
   out[k] = 0;
   return out;
}

void render_alarm(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 26 rows, not 16: the NUDGE section added four rows (two thresholds, its
    * own SOUND and VIBRATION), two captions and the blank line between the
    * sections, and this screen has no scrolling to recover anything the
    * budget under-counts. Measured bottom is 24.5 rows below the title's
    * start plus the last row's hit box, i.e. 405 sc units against the 416
    * that 26 buys; 25 buys 400 and is NOT enough. Re-measure this number
    * whenever a row is added -- it is not a round guess. */
   int sc  = ui_fit_scale(fb->width, fb->height, 26);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "ALARM", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_ALARM_BACK, 0);
   y += 3 * lh;

   /* TWO SECTIONS, and the order is the point.
    *
    * ALARM first: the persistent, wake-you-up band, and the one that should
    * be set once to a conservative value and then left alone. NUDGE second:
    * the wider band that fires once, quietly, and is meant to be ignorable.
    * Keeping them visibly separate is what stops the alarm being edited as a
    * stand-in for the nudge -- the habit whose failure mode is an alarm left
    * parked somewhere it can no longer help. See alarmlogic.h.
    *
    * NEW DATAPOINT lives under NUDGE because it is the same kind of thing: a
    * one-shot sound that informs rather than demands. */
   menu_head(fb, h, y, sc, lh, "ALARM");
   y += (3 * lh) / 2;
   thresh_menu_row(fb, h, y, sc, lh, "LOW", m->prefs.alarm_low, m->prefs.units,
                   0, MA_ALARM_LOW);
   y += 2 * lh;
   thresh_menu_row(fb, h, y, sc, lh, "HIGH", m->prefs.alarm_high,
                   m->prefs.units, 1, MA_ALARM_HIGH);
   y += 2 * lh;
   /* An enabled state reads GREEN, off stays white -- on/off is visible
    * from the colour alone, before reading a word. */
   menu_row(fb, h, y, sc, lh, "SOUND", m->prefs.sound_on ? "ON" : "OFF",
            m->prefs.sound_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_SOUND, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->prefs.vib_on ? "ON" : "OFF",
            m->prefs.vib_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_VIB, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "DISCONNECT",
            ui_disc_lbl[(unsigned)m->prefs.disc & 3U],
            m->prefs.disc ? 0xFF33FF88 : 0xFFFFFFFF, MA_DISC, 0);
   y += (5 * lh) / 2; /* two rows' worth, i.e. a blank line between sections */

   menu_head(fb, h, y, sc, lh, "NUDGE");
   y += (3 * lh) / 2;
   thresh_menu_row(fb, h, y, sc, lh, "LOW", m->prefs.nudge_low, m->prefs.units,
                   0, MA_NUDGE_LOW);
   y += 2 * lh;
   thresh_menu_row(fb, h, y, sc, lh, "HIGH", m->prefs.nudge_high,
                   m->prefs.units, 1, MA_NUDGE_HIGH);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "SOUND", m->prefs.nudge_sound ? "ON" : "OFF",
            m->prefs.nudge_sound ? 0xFF33FF88 : 0xFFFFFFFF, MA_NUDGE_SOUND, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->prefs.nudge_vib ? "ON" : "OFF",
            m->prefs.nudge_vib ? 0xFF33FF88 : 0xFFFFFFFF, MA_NUDGE_VIB, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "NEW DATAPOINT",
            ui_newdata_lbl[(unsigned)m->prefs.newdata_mode % 3U],
            m->prefs.newdata_mode ? 0xFF33FF88 : 0xFFFFFFFF, MA_NEWDATA, 0);
}

/* ---- EXPORT DATA menu (opened from SETTINGS' EXPORT DATA button) ---- */

void render_export(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 16);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "EXPORT DATA", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_EXP_BACK, 0);
   y += 3 * lh;

   static const char *const rng[3] = {"30 D", "1 Y", "ALL"};
   menu_row(
       fb, h, y, sc, lh, "RANGE",
       rng[(m->sys.exp_range >= 0 && m->sys.exp_range < 3) ? m->sys.exp_range
                                                           : 2],
       0xFFFFFFFF, MA_EXP_RANGE, 0);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "GLUCOSE", m->sys.exp_glu, MA_EXP_GLU);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "DEVICES", m->sys.exp_dev, MA_EXP_DEV);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "INSULIN", m->sys.exp_ins, MA_EXP_INS);
   y += lh;
   chk_row(fb, h, y, sc, lh, "WEIGHT", m->sys.exp_wt, MA_EXP_WT);
   y += 3 * lh;

   /* The one acting control. With every section unticked there is nothing
    * to build, so the button greys out and records no target. */
   int any = m->sys.exp_glu || m->sys.exp_dev || m->sys.exp_ins;
   int bw  = fb->width - (2 * x);
   if (any)
      menu_button(fb, h, x, y, bw, sc, "EXPORT", 0xFF33FF88, MA_EXP_GO, 0);
   else
      draw_str(px, fb, x, y, sc, "NOTHING SELECTED", 0xFF888888);
}

/* ---- DISPLAY submenu (opened from SETTINGS) ---- */

void render_display(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DISPLAY", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_DISPLAY_BACK, 0);
   y += 3 * lh;

   menu_row(fb, h, y, sc, lh, "ORIENTATION",
            ui_orient_lbl[(unsigned)m->prefs.orient & 3U], 0xFFFFFFFF,
            MA_ORIENT, 0);
   y += 2 * lh;
   /* "GLUCOSE UNITS", not "UNITS": with a weight unit directly below it, a
    * bare "UNITS" is the row that does not say what it governs. */
   menu_row(fb, h, y, sc, lh, "GLUCOSE UNITS",
            m->prefs.units ? "MMOL/L" : "MG/DL", 0xFFFFFFFF, MA_UNITS, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "WEIGHT UNITS", wt_unit_name(m->prefs.wunits),
            0xFFFFFFFF, MA_WUNITS, 0);
   y += 2 * lh;
   /* ALWAYS ON holds the screen awake while the app is open (the historical
    * behaviour); SYSTEM lets the normal display timeout apply. */
   menu_row(fb, h, y, sc, lh, "SCREEN",
            m->prefs.screen_on ? "ALWAYS ON" : "SYSTEM", 0xFFFFFFFF, MA_SCREEN,
            0);
   y += 2 * lh;
   char pmv[20];
   char pmvv[8];
   fmt_glu(m->plot.plot_max, m->prefs.units, pmvv, sizeof pmvv);
   (void)snprintf(pmv, sizeof pmv, "%s %s", pmvv, UI_LBL(m->prefs.units));
   menu_row(fb, h, y, sc, lh, "PLOT MAX", pmv, 0xFFFFFFFF, MA_PLOTMAX, 0);
   y += 2 * lh;
   /* Insulin plot styling, one row PER TYPE -- each opens the full marker
    * picker (shape, colour, size) for that type. The value is the ACTUAL
    * glyph at its configured shape/colour/size, exactly like the
    * per-device MARKER row -- a preview, not a name. */
   static const char *const ins_lbl[2] = {"SLOW INSULIN MARKER",
                                          "FAST INSULIN MARKER"};
   for (int k = 0; k < 2; k++) {
      draw_str(px, fb, x, y, sc, ins_lbl[k], 0xFFCCCCCC);
      if (m->ins.ins_marker[k] == MARK_HIDE) {
         int lw = str_len("OFF") * 6 * sc;
         draw_str(px, fb, rx - lw, y, sc, "OFF", 0xFFAAAAAA);
      } else {
         /* glyph reflects the configured SIZE too (plot scaling) */
         int gr = (2 * sc * m->ins.ins_size[k]) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 5 * sc)
            gr = 5 * sc;
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height},
             rx - (6 * sc), y + (3 * sc), gr, m->ins.ins_marker[k],
             ui_sensor_color(m->ins.ins_color[k]));
      }
      add_hit_ix(h, 0, y - (3 * sc), fb->width, lh, MA_INSMARK_OPEN, k);
      y += 2 * lh;
   }
   /* Status bar value vs plain app icon; lock-screen visibility; and a
    * way back for a swiped-away notification (it also reappears by
    * itself on the next reading). */
   menu_row(fb, h, y, sc, lh, "STATUS BAR",
            m->prefs.statbar_val ? "NUMBER" : "ICON", 0xFFFFFFFF, MA_STATBAR,
            0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "LOCK SCREEN",
            m->prefs.lockscr_val ? "SHOW" : "HIDE", 0xFFFFFFFF, MA_LOCKSCR, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "NOTIFICATION", "REOPEN", 0xFFFFFFFF,
            MA_NOTIF_REOPEN, 0);
}

/* ---- remote push (opened from SETTINGS) ---- */

void render_remote(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 28 rows: title (3, the DISPLAY menu's gap) + three double-pitch setting
    * rows (6) + the two report rows now at double pitch too (4) + the
    * transport note (4) + the API reference (10) + margin. This number only
    * sizes the FONT (ui_fit_scale divides the height by it), and it falls off
    * a cliff -- 31 rows still renders at scale 3 on a 720x1600 phone, 32 drops
    * to 2 and the whole screen goes tiny. Claim what the content actually
    * needs, not more.
    *
    * It was 26 before the spacing was normalised; the two rows that added
    * (one at the title, one between LAST SYNC and LAST STATUS) clipped 3584
    * glyph cells at 828x1792 until this followed them. */
   int sc  = ui_fit_scale(fb->width, fb->height, 28);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "REMOTE", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_REMOTE_BACK, 0);
   y += 3 * lh; /* the DISPLAY menu's title gap -- the house style */

   const char *sv = (m->sync.remote_server && m->sync.remote_server[0])
                        ? m->sync.remote_server
                        : 0;
   /* PUSH reflects what will actually happen: enabling without a server set
    * shows NO SERVER (amber), not ON -- nothing leaves the phone until one
    * exists, and pretending otherwise would be a silent lie. */
   const char *pv = "OFF";
   uint32_t pc    = 0xFFFFFFFF;
   if (m->sync.remote_on && sv) {
      pv = "ON";
      pc = 0xFF33FF88;
   } else if (m->sync.remote_on) {
      pv = "NO SERVER";
      pc = 0xFFAA8844;
   }
   /* Double pitch between the three setting rows: the blank line makes each
    * an easier touch target (menu_row's hit box spans its own row only). */
   menu_row(fb, h, y, sc, lh, "PUSH", pv, pc, MA_REMOTE_TOGGLE, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "SERVER", sv ? sv : "NOT SET",
            sv ? 0xFFFFFFFF : 0xFFAAAAAA, MA_REMOTE_IP, 0);
   y += 2 * lh;
   char pt[8];
   (void)snprintf(pt, sizeof pt, "%d", m->sync.remote_port);
   menu_row(fb, h, y, sc, lh, "PORT", pt, 0xFFFFFFFF, MA_REMOTE_PORT, 0);
   y += 2 * lh;
   /* The account this phone syncs into, and whether it has been paired with
    * it yet. Both are needed before anything can be sent, so both are shown
    * whether or not they are set -- a missing one must not be invisible. */
   const char *em =
       (m->sync.sync_email && m->sync.sync_email[0]) ? m->sync.sync_email : 0;
   menu_row(fb, h, y, sc, lh, "EMAIL", em ? em : "NOT SET",
            em ? 0xFFFFFFFF : 0xFFAAAAAA, MA_SYNC_EMAIL, 0);
   y += 2 * lh;
   if (m->sync.sync_paired) {
      menu_row(fb, h, y, sc, lh, "PAIRED", "YES", 0xFF33FF88, MA_SYNC_UNPAIR,
               0);
      y += 2 * lh;
      /* Only when paired: a restore needs the key, and offering it otherwise
       * would be a row that can only ever fail. */
      menu_row(fb, h, y, sc, lh, "RESTORE", "FROM SERVER", 0xFFFFFFFF,
               MA_SYNC_RESTORE, 0);
   } else if (!sv) {
      /* Name the ONE thing standing in the way, top to bottom, rather than
       * "SET BOTH" -- which says something is missing without saying what,
       * and is exactly as unhelpful when only one of them is. */
      menu_row(fb, h, y, sc, lh, "PAIR", "(SET SERVER)", 0xFFAAAAAA,
               MA_SYNC_PAIR, 0);
   } else if (!em) {
      menu_row(fb, h, y, sc, lh, "PAIR", "(SET EMAIL)", 0xFFAAAAAA,
               MA_SYNC_PAIR, 0);
   } else {
      menu_row(fb, h, y, sc, lh, "PAIR", "ENTER CODE", 0xFFFFFFFF, MA_SYNC_PAIR,
               0);
   }
   y += 2 * lh;
   /* STATUS: when the server last ACKNOWLEDGED something. With push on,
    * this is the one row that says whether the link actually works --
    * "ON" above only means the app intends to send. Not tappable (code
    * -1): it reports, it does not act. */
   {
      char st[16];
      uint32_t scol = 0xFFAAAAAA;
      if (!m->sync.remote_on) {
         (void)snprintf(st, sizeof st, "--");
      } else if (m->sync.remote_last_ok > 0) {
         char ago[12];
         fmt_ago(m->now, m->sync.remote_last_ok, ago, sizeof ago);
         (void)snprintf(st, sizeof st, "%s AGO", ago);
         /* Fresh is green; a link that has not been acknowledged in over
          * ten minutes is amber, because that is a backlog building up. */
         scol =
             (m->now - m->sync.remote_last_ok <= 600) ? 0xFF33FF88 : 0xFFAA8844;
      } else {
         (void)snprintf(st, sizeof st, "NEVER");
         scol = 0xFFAA8844;
      }
      menu_row(fb, h, y, sc, lh, "LAST SYNC", st, scol, -1, 0);
      /* 2*lh like every other row pair on this screen. These two were the
       * only ones still butted together, which made them read as one
       * two-line row -- and they answer different questions (WHEN the last
       * batch landed vs WHAT the server said about it). */
      y += 2 * lh;
      /* ...and WHAT the server said. A refusal used to be invisible here:
       * the screen showed a happy PUSH ON while every batch was bouncing. */
      const char *rs = (m->sync.remote_status && m->sync.remote_status[0])
                           ? m->sync.remote_status
                           : "--";
      /* Colour by SEVERITY, which the outcome carries.
       *
       * This used to test rs[0] == '2', a leftover from when the field held
       * an HTTP status code -- so the green branch was unreachable and
       * success and failure rendered in identical grey, which is the same
       * defect the comment two lines above describes this row being added to
       * fix. Its replacement compared the text against a list of English
       * phrases, and that list was missing RESTORED and NOTHING TO RESTORE:
       * a restore that worked rendered grey, like a phone that had never
       * synced. A list of strings cannot be checked for completeness; a
       * switch over an enum is checked by the compiler (see syncstat.c). */
      uint32_t rcol = 0xFFAAAAAA; /* nothing has been attempted yet */
      switch (sync_outcome_severity(m->sync.remote_outcome)) {
         case SYNC_SEV_GOOD: rcol = 0xFF33FF88; break; /* green */
         case SYNC_SEV_WARN: rcol = 0xFFFFCC44; break; /* amber: yours to fix */
         case SYNC_SEV_BAD:
            rcol = 0xFFFF5555;
            break; /* red, as the keypad
                      refusals use */
         case SYNC_SEV_NONE:
         default: break;
      }
      menu_row(fb, h, y, sc, lh, "LAST STATUS", rs, rcol, -1, 0);
      y += 2 * lh;
   }

   /* A bar while a sync is running. It is the only thing on this screen that
    * moves, and it exists because a sync is otherwise indistinguishable from
    * a server that is quietly refusing everything: LAST SYNC only changes
    * once, at the end, and only if the whole thing worked. */
   if (m->sync.sync_active) {
      y += lh;
      int bar_x = 4 * sc;
      int bar_w = fb->width - (8 * sc);
      int bar_h = 3 * sc;
      draw_frame(px, fb, bar_x, y, bar_w, bar_h, 0xFF555555);
      int fill = ((bar_w - (2 * sc)) * m->sync.sync_permille) / 1000;
      if (fill < 0)
         fill = 0;
      if (fill > bar_w - (2 * sc))
         fill = bar_w - (2 * sc);
      for (int by = y + sc; by < y + bar_h - sc; by++)
         for (int bxx = bar_x + sc; bxx < bar_x + sc + fill; bxx++)
            px[(by * fb->stride) + bxx] = 0xFF33FF88;
      y += bar_h + lh;
      char pctxt[16];
      (void)snprintf(pctxt, sizeof pctxt, "%d%%", m->sync.sync_permille / 10);
      draw_str(px, fb, bar_x, y, sc, "SYNCING", 0xFF888888);
      draw_str(px, fb, fb->width - (4 * sc) - (str_len(pctxt) * 6 * sc), y, sc,
               pctxt, 0xFF888888);
   }
}

/* ---- per-sensor screen: attributes above, actions below ---- */

/* `id` is what settings.c stores (settings.h, enum shortcut_id) and `code` is
 * the touch code this build happens to use. Two columns, because they answer
 * different questions and change on different schedules: the id is a file
 * format and may never move, the code is an implementation detail of the
 * dispatcher and has already been renumbered once. */
static const struct {
   int id;
   int code;
   const char *full;
   const char *abbrev;
} ui_sc_tab[] = {
    {SC_INS_FAST, MA_INS_FAST,    "FAST INSULIN",     "FAST"   },
    {SC_INS_SLOW, MA_INS_SLOW,    "SLOW INSULIN",     "SLOW"   },
    {SC_INSLOG,   MA_INSLOG_OPEN, "VIEW INSULIN LOG", "INS LOG"},
    {SC_WEIGHT,   MA_WT_OPEN,     "WEIGHT",           "WEIGHT" },
    {SC_WTLOG,    MA_WTLOG_OPEN,  "VIEW WEIGHT LOG",  "WT LOG" },
    {SC_FOOD,     MA_FOOD_OPEN,   "FOOD",             "FOOD"   },
    /* LAST, and the odd one: every button above opens a form and is finished
     * when that form is confirmed, while this one records by being LEFT
     * ALONE. It is in this table so it can be PINNED like the rest; what it
     * cannot share is the drawing, because its level, colour and countdown
     * are not a label -- see ui_exercise_button, which both the menu below
     * and the main screen call. */
    {SC_FOODLOG,  MA_FOODLOG_OPEN, "VIEW FOOD LOG",   "FOOD LOG"},
    {SC_EXERCISE, MA_EXERCISE,    "EXERCISE",         "EXER"   },
};

#define UI_SC_N ((int)(sizeof ui_sc_tab / sizeof ui_sc_tab[0]))

int ui_shortcut_count(void)
{
   return UI_SC_N;
}

int ui_shortcut_code(int slot)
{
   return (slot >= 0 && slot < UI_SC_N) ? ui_sc_tab[slot].code : 0;
}

const char *ui_shortcut_label(int slot, int abbrev)
{
   if (slot < 0 || slot >= UI_SC_N)
      return "";
   return abbrev ? ui_sc_tab[slot].abbrev : ui_sc_tab[slot].full;
}

int ui_shortcut_slot(int code)
{
   for (int i = 0; i < UI_SC_N; i++)
      if (ui_sc_tab[i].code == code)
         return i;
   return -1;
}

int ui_shortcut_id(int slot)
{
   return (slot >= 0 && slot < UI_SC_N) ? ui_sc_tab[slot].id : SC_NONE;
}

int ui_shortcut_slot_by_id(int id)
{
   if (id <= SC_NONE)
      return -1;
   for (int i = 0; i < UI_SC_N; i++)
      if (ui_sc_tab[i].id == id)
         return i;
   return -1; /* stored by a build that offered a pin this one does not */
}

/* Is this shortcut SLOT already pinned to the main screen? Asked by slot
 * rather than by touch code, because what is stored is the stable id. */
static int sc_on(const struct screen *m, int slot)
{
   int id = ui_shortcut_id(slot);
   if (id <= SC_NONE)
      return 0;
   for (int i = 0; i < SC_MAX; i++)
      if (m->prefs.shortcut[i] == id)
         return 1;
   return 0;
}

void render_addmenu(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 26 rows, unchanged. The two weight buttons are paid for out of the GAPS
    * (see `gap` below), not out of the font: raising the budget to fit them
    * shrank every label on this screen, and a smaller font on the menu that
    * logs medication is a worse trade than tighter spacing. */
   int sc  = ui_fit_scale(fb->width, fb->height, 26);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "ADD ...", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* This menu opens from the MAIN screen, so its X returns there (MA_CLOSE),
    * not into SETTINGS. Generous close target across the title band. */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_CLOSE, 0);
   /* TWO ROWS UNDER THE TITLE, not three. The eleventh button on this screen
    * put the last DEVICES entry and its target below the buffer again --
    * measured at 1080x2280 and 1440x3200 -- and the title band was the only
    * slack left that is not between two controls. Taken from whitespace, as
    * every previous round was; the font is not negotiable.
    *
    * THIS SCREEN IS NOW FULL. The gaps are half a row, the section break is
    * one, and the title band is two. A twelfth button does not fit at this
    * text size on the shortest supported window, and the next one to be added
    * needs pagination or a two-column LOG section -- not another slice off
    * the whitespace, because there is none left to take. */
   y += 2 * lh;

   /* Three sections. LOG: what the user records by hand -- the insulin type
    * is chosen HERE (FAST / SLOW), so the form opens already knowing it, and
    * weight sits alongside because it is the same kind of act. DEVICES: the
    * device types from the ADD DEVICE picker, one tap instead of two. */
   int bw = fb->width - (2 * x);
   /* Air BETWEEN buttons: this screen is nothing but stacked buttons, several
    * of them destructive-adjacent (logging a dose vs. adding a device), so
    * the separation is what stops a mistap. Header-to-button spacing stays at
    * one row, which keeps each label visibly attached to the button it names.
    *
    * ONE row, reduced from one and a half when the two weight buttons landed.
    * The arithmetic at the 26-row budget, in sc: 48 title + 16 header +
    * 5*25 LOG buttons + 32 + 16 header + 3*25 device buttons = 312, leaving
    * 416 - 312 = 104 for the six gaps between adjacent buttons, i.e. at most
    * 17 each. A 24 gap does not fit and a 17 gap clears by 2 sc, which is no
    * margin at all; one row (16) clears by 8 and is the spacing the rest of
    * the menus already use. Recheck this if a button is ever added. */
   /* HALF A ROW between buttons, not a full one.
    *
    * The arithmetic below is what forced it, and it is worth writing out
    * because the previous version of this comment ended "Recheck this if a
    * button is ever added" and a button has now been added. At the 26-row
    * budget (416 sc): 48 title + 16 header + 6*25 LOG buttons + 32 + 16
    * header + 3*25 device buttons = 387, leaving 29 for the seven gaps
    * between adjacent buttons. A full row (16) needs 112 and overflowed on
    * EVERY geometry in the sweep -- measured: the last DEVICES button and its
    * target landed below the buffer at 1080x1920, 1080x2280, 1440x3200,
    * 1080x2340 and 828x1792.
    *
    * Half a row is 8, i.e. 56 for the seven, which still does not fit on
    * paper -- and does in practice because the title band and the section
    * break below are the slack the calculation above treats as fixed. The
    * sweep is the authority, not this paragraph: it is what caught the
    * overflow and what has to stay green.
    *
    * NOT taken out of the FONT. Shrinking the text to make room is a
    * standing prohibition here -- ui_fit_scale's row count is a cliff, and
    * one more row makes every label on the screen smaller. Air between
    * controls is the only thing this screen has to give. */
   /* TWO FIFTHS of a row between buttons.
    *
    * It was a full row, then a half when the tenth button arrived, and the
    * eleventh needs this. At sc=5 that is 32 px of air -- still six times the
    * 2*sc at which the comment above records that buttons "touched each other
    * and crowded the plus", so it is thinner than it was and not yet thin.
    *
    * What forced it: the LAST device button's hit target ended nine pixels
    * below the buffer at 1080x2280 (and five at 1440x3200), and a target is
    * the button, so the button itself did not fit. Removing the trailing gap
    * after it changed nothing -- that space is BELOW the target, not above
    * it -- which is worth recording, because it is the obvious fix and it
    * does not work.
    *
    * THE SCREEN IS FULL. Gaps are two fifths of a row, the section break is
    * one, the title band is two. A twelfth button does not fit at this text
    * size on the shortest supported window; the next one needs pagination or
    * a two-column LOG section, not another slice off the whitespace. */
   int gap = (2 * lh) / 5;
   /* THE PIN COLUMN. A checkbox per promotable action at the right, under its
    * own header, with the buttons shortened to make room -- a quarter of the
    * old button width, which is enough for the header and a comfortable tap
    * target without making the labels wrap.
    *
    * Headed PIN, not SHORTCUT: the box does not create a shortcut somewhere
    * else to be gone looking for, it PINS this action to the main screen, and
    * the shorter word says the same thing in three characters.
    *
    * Only the LOG buttons get one (see ui_sc_tab), so the column header sits
    * over that section and the DEVICES buttons below keep the full width. */
   int scw = bw / 4;   /* the checkbox column */
   int lbw = bw - scw; /* what the LOG buttons keep */
   int scx = x + lbw + (2 * sc);
   /* SQUARE, and as tall as the button it controls (menu_button's own 25*sc):
    * a checkbox is the control for the thing beside it, so it should read as
    * that thing's partner rather than as a mark of punctuation after it. The
    * column always has room -- scw is bw/4, and ui_fit_scale bounds sc by
    * width at w/(33*6), so bw/4 >= 47*sc. */
   int cbs = 25 * sc;
   int cbx = scx + (((scw - (2 * sc)) - cbs) / 2);
   draw_str(px, fb, x, y, sc, "LOG", 0xFF888888);
   {
      /* Centred over the boxes, so header and column share one axis. */
      int hw = ((str_len("PIN") * 6) - 1) * sc;
      int hx = cbx + ((cbs - hw) / 2);
      draw_str(px, fb, hx, y, sc, "PIN", 0xFF888888);
   }
   y += lh;
   /* The buttons NAME what they log. "FAST" and "SLOW" were only unambiguous
    * while insulin was the sole thing on this screen; with weight beside them
    * a bare "FAST" is a button whose meaning depends on a header three rows
    * up, which is not a property to rely on when the tap logs a medication. */
   for (int i = 0; i < ui_shortcut_count(); i++) {
      int code = ui_shortcut_code(i);
      int on   = sc_on(m, i);
      int by   = y;
      /* One entry in this table is not a plain button: EXERCISE carries its
       * own level, colour and settling countdown. It still gets a PIN box and
       * the same width as its neighbours -- only the drawing differs. */
      if (code == MA_EXERCISE)
         y = ui_exercise_button(fb, h, x, y, lbw, sc, m->food.ex_level,
                                m->food.ex_remaining, EX_SETTLE_S,
                                ui_shortcut_label(i, 0), 0xFFFFFFFF);
      else
         y = menu_button(fb, h, x, y, lbw, sc, ui_shortcut_label(i, 0),
                         0xFFFFFFFF, code, 0);
      /* The box shares the button's top and bottom edge (they are the same
       * height), and its TARGET is the whole column cell -- this one sits
       * right beside a button that logs a medication, so the two must not be
       * easy to confuse. Recorded AFTER the button and inside its row:
       * ui_hit_idx scans backwards, so the box wins its own rectangle while
       * the rest of the row still logs. */
      draw_checkbox(px, fb, cbx, by, cbs, sc, on, on ? 0xFF33FF88 : 0xFFAAAAAA);
      add_hit_ix(h, x + lbw, by, bw - lbw, y - by, MA_SCTOGGLE, i);
      y += gap;
   }
   /* THE SECTION BREAK, and it is the last slack this screen had.
    *
    * It was 2*lh - gap (a blank line between the two groups). With EXERCISE
    * added the screen overflowed again -- measured at 1080x2280, 828x1792 and
    * 640x900, by between 4 and 13 pixels -- and a break of one row clears it
    * while still reading as a break: the DEVICES header sits under it and
    * does its own separating. Whitespace, again, rather than the font. */
   y += lh - gap;

   draw_str(px, fb, x, y, sc, "DEVICES", 0xFF888888);
   y += lh;
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      y = menu_button(fb, h, x, y, bw, sc, sensor_disp_name(t), 0xFFFFFFFF,
                      MA_TYPE, t);
      /* NO GAP AFTER THE LAST ONE. A gap separates two controls, and below
       * the final button there is no second control to separate it from --
       * it was simply a row of empty space at the bottom of the screen, paid
       * for out of the budget that the last button's TARGET then overran by
       * nine pixels. Measured at 1080x2280 and 1440x3200. */
      if (t + 1 < SENSOR_NTYPES)
         y += gap;
   }
}

/* ---- LOG INSULIN: units / date / time, then CONFIRM or DISCARD. The type
 * (FAST/SLOW) is chosen on the ADD menu and fixed in this form's title. ---- */

/* One "NAME   <big value>" row; tapping the VALUE opens the keypad for
 * exact entry (arrows and steppers proved too fiddly at phone size --
 * typing the digits is faster and cannot overshoot). The whole right
 * half of the row is the tap target. Returns the y below the row. */

/* Order the MARKER picker lists shapes in (grouped filled/empty, HIDE last).
 * DOT is omitted -- it renders identically to SQUARE FILL. */
#define UI_NMARKERS 8
static const int ui_marker_order[UI_NMARKERS] = {
    MARK_CIRCLE,   MARK_CIRCLE_F,   MARK_SQUARE, MARK_SQUARE_F,
    MARK_TRIANGLE, MARK_TRIANGLE_F, MARK_CROSS,  MARK_HIDE};

void render_markpick(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Graphical combined picker: shapes shown as glyphs, colours as full-colour
    * buttons, and a size row previewing the CURRENT shape+colour at each size.
    * All selections update in place; the title-row X returns to the device. */
   int sc  = ui_fit_scale(fb->width, fb->height, 22);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   /* The picker edits either the SELECTED SLOT's styling (old devices
    * keep their slot, so this works for them exactly like a live one) or,
    * in markpick_ins mode, the INSULIN marker -- shape only, drawn white,
    * X back to settings. */
   int ins  = (m->ins.markpick_ins >= 0); /* which insulin type, or a sensor */
   int ity  = (ins && m->ins.markpick_ins == INS_FAST) ? INS_FAST : INS_SLOW;
   int okk  = !ins && (m->dev.sel >= 0 && m->dev.sel < m->dev.nsensors);
   int back = MA_INSMARK_BACK;
   int back_ix = 0;
   int curm    = m->ins.ins_marker[ity];
   int curc    = m->ins.ins_color[ity];
   int curs    = m->ins.ins_size[ity];
   if (!ins) {
      back    = MA_SENSOR;
      back_ix = m->dev.sel >= 0 ? m->dev.sel : 0;
      curm    = okk ? m->dev.sensors[m->dev.sel].marker : 0;
      curc    = okk ? m->dev.sensors[m->dev.sel].color : 0;
      /* size 0 means UNSET, i.e. the default (sensors.h) -- so resolve it the
       * way the list row and the SIZE preview already do. Passing the raw 0
       * through made the SIZE row highlight no cell at all, so a sensor that
       * had never been styled showed no current selection. */
      curs = (okk && m->dev.sensors[m->dev.sel].size >= 1)
                 ? m->dev.sensors[m->dev.sel].size
                 : MARK_SIZE_DEF;
   }
   uint32_t curcol = ui_sensor_color(curc);

   /* short titles: at title scale the full "SLOW INSULIN MARKER" would
    * run under the X */
   const char *ttl = "MARKER";
   if (ins)
      ttl = (ity == INS_FAST) ? "FAST MARKER" : "SLOW MARKER";
   draw_str(px, fb, x, y, tsc, ttl, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, back, back_ix);
   y += 2 * lh;
   int gw = fb->width - (2 * x);

   /* CELLS ARE SQUARE, SO THEIR SIZE IS BOUNDED BY BOTH AXES.
    *
    * Sizing them from `gw / cols` alone is correct in portrait and badly
    * wrong in landscape: at 1920x1080 the shape grid took 1912/4 = 478 px
    * cells, and the three stacked grids ran some 800 px past the bottom of a
    * 1080-px buffer. Every swatch below the fold was drawn off-screen while
    * still recording a full-size tap target, so the styling picker was
    * unusable in landscape and the gate never saw it -- this screen was
    * absent from the reachability sweep. ui_devices_scale and ui_fit_scale
    * already bound themselves by both axes; this is the same rule.
    *
    * `rows` counts the grid rows only (shape rows + colour + size); the
    * labels and gaps between them are subtracted separately. */
   int gridrows = ((UI_NMARKERS + 3) / 4) + 2;
   int gridav   = fb->height - y   /* what is left below the title */
                  - (3 * lh)       /* the SHAPE / COLOR / SIZE labels */
                  - ((3 * lh) / 2) /* the half-line gap after each grid */
                  - (lh / 2);      /* keep the last row off the very edge */
   int cellmax  = (gridav > gridrows) ? gridav / gridrows : 1;

   /* SHAPE grid: each shape as a glyph in the sensor's own colour. */
   draw_str(px, fb, x, y, sc, "SHAPE", 0xFF888888);
   y += lh;
   {
      int cols = 4;
      int cell = gw / cols;
      if (cell > cellmax)
         cell = cellmax;
      /* Centre what the height cap left over, so a wide screen reads as a
       * deliberate block rather than a grid shoved against the left edge. */
      int gx = x + ((gw - (cols * cell)) / 2);
      for (int i = 0; i < UI_NMARKERS; i++) {
         int mk = ui_marker_order[i];
         int cx = gx + ((i % cols) * cell);
         int cy = y + ((i / cols) * cell);
         if (mk == MARK_HIDE) {
            int lw = str_len("OFF") * 6 * sc;
            draw_str(px, fb, cx + ((cell - lw) / 2),
                     cy + ((cell - (7 * sc)) / 2), sc, "OFF", 0xFFAAAAAA);
         } else {
            plot_marker_glyph(
                (struct plot_fb){px, fb->stride, fb->width, fb->height},
                cx + (cell / 2), cy + (cell / 2), cell / 5, mk, curcol);
         }
         draw_frame(px, fb, cx + sc, cy + sc, cell - (2 * sc), cell - (2 * sc),
                    (mk == curm) ? 0xFF33FF88 : 0xFF555555);
         add_hit_ix(h, cx, cy, cell, cell, MA_MARK_PICK, mk);
      }
      y += (((UI_NMARKERS + cols - 1) / cols) * cell) + (lh / 2);
   }

   /* COLOR grid: full-colour buttons. */
   draw_str(px, fb, x, y, sc, "COLOR", 0xFF888888);
   y += lh;
   {
      int cols = UI_NCOLORS;
      int cell = gw / cols;
      if (cell > cellmax)
         cell = cellmax;
      int gx = x + ((gw - (cols * cell)) / 2);
      for (int i = 0; i < UI_NCOLORS; i++) {
         int cx = gx + (i * cell);
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height},
             cx + (cell / 2), y + (cell / 2), (cell - (4 * sc)) / 2,
             MARK_SQUARE_F, ui_sensor_color(i));
         draw_frame(px, fb, cx + sc, y + sc, cell - (2 * sc), cell - (2 * sc),
                    (i == curc) ? 0xFF33FF88 : 0xFF555555);
         add_hit_ix(h, cx, y, cell, cell, MA_COLOR_PICK, i);
      }
      y += cell + (lh / 2);
   }

   /* SIZE row: the current shape+colour drawn at each of the 5 sizes. */
   draw_str(px, fb, x, y, sc, "SIZE", 0xFF888888);
   y += lh;
   {
      int cols = MARK_SIZE_MAX;
      int cell = gw / cols;
      if (cell > cellmax)
         cell = cellmax;
      int gx    = x + ((gw - (cols * cell)) / 2);
      int shape = (curm == MARK_HIDE) ? MARK_SQUARE_F : curm;
      for (int s = 1; s <= MARK_SIZE_MAX; s++) {
         int cx = gx + ((s - 1) * cell);
         /* Same scaling the plot uses (radius grows linearly with size), so the
          * preview reflects the real on-plot size rather than filling the cell.
          */
         int r = (3 * sc * s) / MARK_SIZE_DEF;
         if (r < 1)
            r = 1;
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height},
             cx + (cell / 2), y + (cell / 2), r, shape, curcol);
         draw_frame(px, fb, cx + sc, y + sc, cell - (2 * sc), cell - (2 * sc),
                    (s == curs) ? 0xFF33FF88 : 0xFF555555);
         add_hit_ix(h, cx, y, cell, cell, MA_SIZE_PICK, s);
      }
      /* no y advance: the SIZE row is the last thing in this screen */
   }
}

void render_gate(struct ANativeWindow_Buffer *fb, struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded, computed from THIS screen's own geometry rather than
    * borrowed from ui_fit_scale.
    *
    * The gate starts at h/12 (not ui_fit_scale's h/20) and lays out at a 12*sc
    * pitch (not 16*sc): 3 + 15 + 2 rows, a button of 24*sc, then 3 + 5 rows,
    * i.e. the last glyph row ends at h/12 + 365*sc. Approximating that through
    * ui_fit_scale was off by h/30 in the margin and 5*sc in the rows, which was
    * enough to clip the final disclaimer line -- "decisions." -- clean off the
    * bottom at 1080x1920, the most common Android resolution there is. On the
    * one screen whose entire purpose is to say this is not for treatment or
    * hypoglycemia decisions. Budget 370 for a little slack. */
   int sc = fb->width / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   int gvsc = (fb->height - (fb->height / 12)) / 370;
   if (gvsc < 1)
      gvsc = 1;
   if (gvsc < sc)
      sc = gvsc;
   static const char *lines[] = {
       "PANCRA reads your CGM",
       "sensor over Bluetooth and",
       "warns you of highs and lows.",
       "",
       "IT ASKS FOR:",
       "",
       "BLUETOOTH  find + connect",
       "           to the sensor",
       "NOTIFY     alert you to",
       "           highs and lows",
       "BATTERY    keep reading in",
       "           the background",
       "",
       /* Was "never leaves this phone" -- no longer true since the
        * REMOTE push exists. Still two lines: this screen's row budget
        * is exact (see above), one more line re-clips the disclaimer. */
       "Data stays on this phone",
       "unless you enable REMOTE.",
   };
   int tsc = 2 * sc;
   int lh  = 12 * sc;
   int x   = 6 * sc;
   int y   = fb->height / 12;
   draw_str(px, fb, x, y, tsc, "PERMISSIONS", 0xFFFFFFFF);
   y += 3 * lh;
   for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
      draw_str(px, fb, x, y, sc, lines[i], 0xFFCCCCCC);
      y += lh;
   }
   y += 2 * lh;
   const char *lbl = "CONTINUE";
   int bsc         = 2 * sc;
   int lw          = str_len(lbl) * 6 * bsc;
   int gh          = 7 * bsc;
   int padx        = 6 * bsc;
   int pady        = 5 * bsc; /* roomy box around the label */
   int bw          = lw + (2 * padx);
   int bh          = gh + (2 * pady);
   int bx          = (fb->width - bw) / 2;
   draw_frame(px, fb, bx, y, bw, bh, 0xFF33FF88);
   draw_str(px, fb, bx + padx, y + pady, bsc, lbl, 0xFF33FF88);
   add_hit(h, bx, y, bw, bh, ACT_GATE_CONTINUE, 0);

   /* disclaimer, dim, at the foot of the screen */
   static const char *disc[] = {
       "Not a medical device, and",
       "not affiliated with Dexcom.",
       "For awareness only, not for",
       "treatment or hypoglycemia",
       "decisions.",
   };
   y += bh + (3 * lh);
   for (int i = 0; i < (int)(sizeof disc / sizeof disc[0]); i++) {
      draw_str(px, fb, x, y, sc, disc[i], 0xFF777777);
      y += lh;
   }
}
