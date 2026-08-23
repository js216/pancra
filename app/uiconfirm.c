// SPDX-License-Identifier: GPL-3.0
// uiconfirm.c --- The screens that ask "are you sure" (see uipriv.h)
// Copyright 2026 Jakob Kastelic
//
/* SEVEN SCREENS, ONE JOB: standing between a tap and something the user did
 * not mean -- forgetting a sensor, reviving an expired one, committing to a
 * pairing, giving up on one that is armed, pulling the server's whole history
 * down, replacing a queued calibration, stopping an active rescale.
 *
 * They live together because their SHAPE is the contract, not their content.
 * Each states in plain words what is about to happen, puts a wide gap before
 * the committing button so it cannot be caught by a finger still travelling,
 * colours the way out and the way on differently, and treats the title-row X
 * as "no". A confirmation that looked like an ordinary menu row would defeat
 * the point of having one -- which is why they are checked side by side here
 * rather than each drifting inside the screen family it belongs to.
 */

#include "ndk.h"
#include "style.h" /* the colour roles: UI_TEXT, UI_MUTED, ... */
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* Shown when CALIBRATION is opened while one is still queued: REPLACE it with a
 * new value, or CANCEL (discard) it. X leaves the queue untouched. */
void render_calpend(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   /* X / title-bar tap leaves the pending calibration in place. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_CAL_BACK, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];
   draw_str(px, fb, x, y, tsc, "CAL PENDING", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, UI_TEXT, -1, 0);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(s->cal_pending, m->prefs.units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->prefs.units));
      menu_row(fb, h, y, sc, lh, "QUEUED", v, UI_BUSY, -1, 0);
      y += lh;
   }
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "REPLACE", UI_TEXT, MA_CAL_REPLACE, 0);
   y += 3 * lh; /* wide gap so DELETE (discard) is deliberate */
   /* "DELETE", not "CANCEL": CANCEL reads as "do nothing", but this button
    * DISCARDS the queued calibration. The X in the title bar is the no-op. */
   menu_button(fb, h, x, y, bw, sc, "DELETE", UI_ALERT, MA_CAL_CANCEL, 0);
}

/* Rescaling already active: CHANGE the value, or STOP. Mirrors render_calpend.
 */
void render_rescaleact(struct ANativeWindow_Buffer *fb, const struct screen *m,
                       struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_RESCALE_BACK,
              0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];
   draw_str(px, fb, x, y, tsc, "RESCALE ON", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* Same spacing as render_rescale, which it mirrors. */
   y += 3 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, UI_TEXT, -1, 0);
   y += 2 * lh;
   {
      char v[32]; /* "PENDING " + value(<=11) + ' ' + unit(<=6) + NUL */
      if (s->rescale_pending > 0) {
         /* Held, awaiting a reading to compute the factor from. */
         char gv[12];
         fmt_glu(s->rescale_pending, m->prefs.units, gv, sizeof gv);
         (void)snprintf(v, sizeof v, "PENDING %s %s", gv,
                        UI_LBL(m->prefs.units));
      } else {
         fmt_rescale_pct(s->rescale_pm, v, sizeof v);
      }
      menu_row(fb, h, y, sc, lh, "RESCALING", v, UI_BUSY, -1, 0);
      y += 2 * lh;
   }
   y += lh;
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CHANGE", UI_TEXT, MA_RESCALE_CHANGE,
                   0);
   y += 3 * lh; /* wide gap so TURN OFF is deliberate */
   /* "TURN OFF" (not STOP/CANCEL): STOP reads like ending the sensor SESSION,
    * and CANCEL like doing nothing -- this turns rescaling off. White, not red:
    * turning rescaling off is not destructive (no data is lost). */
   menu_button(fb, h, x, y, bw, sc, "TURN OFF", UI_TEXT, MA_RESCALE_STOP, 0);
}

/* ---- forget confirmation ----
 * Forgetting drops the slot only: the provenance row and every reading this
 * sensor produced stay exactly where they are. Saying so here is the point of
 * the screen -- otherwise "FORGET" reads like it deletes the data. */

void render_forget(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded (see ui_fit_scale). Left on
    * width-only scaling, this screen's controls were laid out past the
    * bottom in landscape -- and render_forget records no close target, so
    * it became a dead end with no way back. */
   int sc  = ui_fit_scale(fb->width, fb->height, 22);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int y   = (fb->height / 20) + (8 * sc);
   /* A way out, recorded BEFORE the range guard. This screen had no close
    * target of any kind, so with a stale selection it rendered blank and
    * swallowed every tap -- and even when it rendered, landscape put CANCEL
    * and FORGET off the buffer, leaving no way back. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_FORGET_NO, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   int rx = fb->width - (4 * sc);
   draw_str(px, fb, x, y, tsc, "DISCONNECT?", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT); /* close = cancel */
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, s->label, UI_TEXT);
   y += 2 * lh;
   static const char *const note[] = {
       "STOPS THIS DEVICE AND MOVES",
       "IT TO OLD DEVICES. READINGS",
       "AND HISTORY ARE KEPT.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], UI_MUTED);
      y += lh;
   }
   y += 2 * lh;

   /* Two consistent framed buttons, well separated so they cannot be confused:
    * CANCEL (safe, white) and DISCONNECT (RED). This IS the confirmation step
    * -- MA_FORGET_YES is what actually disconnects (the device becomes an OLD
    * DEVICE; nothing is deleted). */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_FORGET_NO, 0);
   y += 3 * lh; /* wide gap so DISCONNECT is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "DISCONNECT", UI_ALERT, MA_FORGET_YES, 0);
}

/* ---- reconnect-an-EXPIRED-device confirmation ----
 * Reconnecting a sensor pulled BEFORE it expired is direct; reconnecting one
 * that has already expired rarely makes sense, so it lands here first. ---- */
void render_reconf(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_RECON_NO, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   draw_str(px, fb, x, y, tsc, "RECONNECT?", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, s->label, UI_TEXT);
   y += 2 * lh;
   static const char *const note[] = {
       "THIS SENSOR IS EXPIRED.",
       "RECONNECTING RARELY WORKS;",
       "IT WILL JUST WAIT FOREVER.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], UI_MUTED);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_RECON_NO, 0);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "RECONNECT", UI_GO, MA_RECON_YES, 0);
}

/* ---- pairing confirmation ----
 * Tapping a row in the device list must not commit the pairing on the spot:
 * commit_pair is consequential, registering the device and (for a CGM)
 * dropping the chosen link's old bond before the J-PAKE. One mis-tap in a
 * list ordered by live RSSI -- rows can reorder under the finger -- would do
 * all of that to the wrong device. So the pick only proposes; this screen's
 * explicit
 * YES is what commits, and NO returns to the list with nothing changed. */

void render_pairconf(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded, for the same landscape reason
    * as render_forget. */
   int sc  = ui_fit_scale(fb->width, fb->height, 22);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int y   = (fb->height / 20) + (8 * sc);
   /* A way out, recorded BEFORE anything can bail: a screen with no
    * dispatchable escape is a dead end that swallows every tap. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_PAIR_NO, 0);

   char title[24];
   (void)snprintf(title, sizeof title, "PAIR %s?",
                  m->dev.add_type ? m->dev.add_type : "SENSOR");
   int rx = fb->width - (4 * sc);
   (void)draw_title_fit(px, fb, x, y, tsc, title, UI_TEXT, rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT); /* close = NO */
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, m->dev.pair_name ? m->dev.pair_name : "",
            UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, m->dev.pair_mac ? m->dev.pair_mac : "", UI_MUTED);
   y += 2 * lh;

   /* Two consistent framed buttons, well separated so they cannot be
    * confused: NO (safe, white) first, YES (commits, green) below. */
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "NO", UI_TEXT, MA_PAIR_NO, 0);
   y += 3 * lh; /* wide gap so YES is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "YES", UI_GO, MA_PAIR_YES, 0);
}

/* ---- giving up on an ARMED pairing ----
 * An armed pairing is a row in the device list that looks exactly like a
 * device: a name on the left, a state on the right. A finger that lands on it
 * is reaching for the sensor's own screen, which is what every other row on
 * that list opens -- so the tap only asks, and this screen is where stopping
 * actually happens.
 *
 * What it costs is worth spelling out: the applicator code is spent by the
 * sensor, not by the app, so stopping and starting again is retyping, not a
 * lost sensor. ---- */
void render_pendcancel(struct ANativeWindow_Buffer *fb,
                       const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);
   /* A way out, recorded BEFORE anything can bail: a screen with no
    * dispatchable escape is a dead end that swallows every tap. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_PEND_KEEP, 0);
   /* The 1 Hz tick commits an armed pairing the moment its sensor advertises,
    * which can happen while this screen is open. With nothing left armed the
    * question is about nothing, so ask none of it -- the escape recorded
    * above is what carries the tap back to the list. */
   if (m->dev.pend_type <= 0)
      return;

   int rx = fb->width - (4 * sc);
   (void)draw_title_fit(px, fb, x, y, tsc, "STOP WAITING?", UI_TEXT,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT); /* close = keep */
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, sensor_disp_name(m->dev.pend_type), UI_TEXT);
   y += 2 * lh;
   static const char *const note[] = {
       "THE APP IS WAITING FOR THIS",
       "SENSOR TO COME ON THE AIR,",
       "AND PAIRS IT WHEN IT DOES.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], UI_MUTED);
      y += lh;
   }
   y += lh;
   draw_str(px, fb, x, y, sc, "STOPPING MEANS TYPING THE", UI_MUTED);
   y += lh;
   draw_str(px, fb, x, y, sc, "APPLICATOR CODE AGAIN.", UI_MUTED);
   y += 2 * lh;

   /* Safe choice first, wide gap, the committing one below -- the shape every
    * confirmation in this file keeps. White, not red: stopping loses no data
    * and no sensor, only the wait. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "KEEP WAITING", UI_TEXT, MA_PEND_KEEP,
                   0);
   y += 3 * lh; /* wide gap so STOP is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "STOP", UI_ALERT, MA_PEND_STOP, 0);
}

/* RESTORE: the one screen that pulls the record DOWN.
 *
 * Framed like every other destructive-ish confirmation -- safe choice first,
 * wide gap, the committing one below -- but the words matter more here than
 * usual, because "restore" sounds harmless and the user needs to know what it
 * will and will not do: it only ADDS days this phone does not have, and it
 * cannot remove or overwrite anything already here. */
void render_syncrestore(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        struct hits *h)
{
   (void)m; /* the question does not depend on the model */
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_SYNCREST_NO,
              0);

   int rx = fb->width - (4 * sc);
   (void)draw_title_fit(px, fb, x, y, tsc, "RESTORE?", UI_TEXT,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "PULL BACK EVERY DAY THE", UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, "SERVER HAS AND THIS PHONE", UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, "DOES NOT.", UI_TEXT);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "NOTHING HERE IS REMOVED", UI_MUTED);
   y += lh;
   draw_str(px, fb, x, y, sc, "OR OVERWRITTEN.", UI_MUTED);
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "NO", UI_TEXT, MA_SYNCREST_NO, 0);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "RESTORE", UI_GO, MA_SYNCREST_YES, 0);
}
