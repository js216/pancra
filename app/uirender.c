// SPDX-License-Identifier: GPL-3.0
// uirender.c --- On-screen rendering: the top of the UI family
//
// NAMED FOR WHAT IT IS, not for the header it implements. As ui.c -- the one
// file this family was split out of -- it shared a node with ui.h in the
// module graph -- and ui.h is the vocabulary every one
// of the ui*.c files needs, including through uipriv.h, so the interface
// appeared to depend on one of its own implementations and the two formed a
// cycle. An interface with no dependencies is a leaf; this is what sits on
// top of it.
// Copyright 2026 Jakob Kastelic

/* THE UI AS A FUNCTION OF AN IMMUTABLE `struct screen`: ui_render() draws the
 * current screen into a locked framebuffer and records its touch targets into
 * `struct hits`; ui_hit() maps a later tap to the action the shell (main.c)
 * should perform. Nothing here reaches for a callback, and the frame is the
 * only description of what to draw -- which is what lets any screen be
 * rendered on the host against a malloc'd buffer, with no phone.
 *
 * NOT LITERALLY PURE, and the exceptions are worth knowing: render_devices asks
 * the registry directly whether it is writable and whether the view is stale
 * (three reads in one frame, so a mid-frame change shows on one row and not
 * another -- harmless, since each row is drawn once); render_olddev tells the
 * shell the page size it could really fit; and this file keeps the clipped-
 * output counter. None of them is read back by a drawing decision, so no frame
 * depends on a previous one. */
#include "colors.h"
#include "log.h" /* LOGW: content laid out past the screen edge */
#include "ndk.h"
#include "ui.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>

/* HOW MUCH OF THE LAST FRAME FELL OFF THE EDGE, in dropped glyph pixels plus
 * one per box or fill that did not fit at all.
 *
 * Clipping is SILENT: the leaf primitives drop out-of-bounds output, so a
 * table laid out past the bottom of the screen simply never appears and leaves
 * no trace -- no hit box out of range, no colour missing if that colour is
 * drawn elsewhere too. An entirely invisible stats table and a truncated
 * medical disclaimer are both things a layout can ship with and nobody sees.
 * One counter per frame, logged when it moves, makes it observable on every
 * screen at once.
 *
 * AND IT STAYS A GLOBAL, deliberately. The increment happens in draw_cell,
 * draw_frame and fill_rect -- the three leaf primitives, reached from every
 * renderer in the family -- so threading a count out of them means an
 * out-parameter on every drawing primitive and every caller of one, to move a
 * number no drawing decision ever reads.
 *
 * It is not logic, it is an instrument: written by the primitives, reset at
 * the top of each ui_render and reported at the bottom of it. Nothing
 * branches on it, so it cannot make one render depend on a previous one --
 * which is the property the purity argument protects. */
static long g_clipped;

void ui_clip_bump(long n)
{
   g_clipped += n;
}

/* Plot ranges, 3H on the left to 720h = 30D on the right. */
const int ui_tab_hours[UI_TABS] = {3, 12, 24, 72, 168, 720};

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h)
{
   h->n        = 0;
   h->overflow = 0;
   g_clipped   = 0;
   /* TRUE black: zero photons on an OLED, rather than a near-black wash. */
   clear_fb(fb, UI_BLACK);
   switch (m->scr) {
      case SCR_SETTINGS: render_settings(fb, m, h); break;
      case SCR_KEYPAD: render_keypad(fb, m, h); break;
      case SCR_DEVLIST: render_devlist(fb, m, h); break;
      case SCR_GATE: render_gate(fb, h); break;
      case SCR_SENSOR: render_sensor(fb, m, h); break;
      case SCR_CAL: render_cal(fb, m, h); break;
      case SCR_CALPEND: render_calpend(fb, m, h); break;
      case SCR_RESCALE: render_rescale(fb, m, h); break;
      case SCR_RESCALEACT: render_rescaleact(fb, m, h); break;
      case SCR_SENSTYPE: render_senstype(fb, m, h); break;
      case SCR_METERHELP: render_meterhelp(fb, m, h); break;
      case SCR_FORGET: render_forget(fb, m, h); break;
      case SCR_RECONF: render_reconf(fb, m, h); break;
      case SCR_PENDCANCEL: render_pendcancel(fb, m, h); break;
      case SCR_PAIRCONF: render_pairconf(fb, m, h); break;
      case SCR_SYNCRESTORE: render_syncrestore(fb, m, h); break;
      case SCR_ADDMENU: render_addmenu(fb, m, h); break;
      case SCR_INSULIN: render_insulin(fb, m, h); break;
      case SCR_DEVICES: render_devices(fb, m, h); break;
      case SCR_PERMS: render_perms(fb, m, h); break;
      case SCR_REMOTE: render_remote(fb, m, h); break;
      case SCR_INSLOG: render_inslog(fb, m, h); break;
      case SCR_INSDEL: render_insdel(fb, m, h); break;
      case SCR_WEIGHT:
         render_weight(fb, &m->wt, &m->prefs, m->tz_off, h);
         break;
      case SCR_WTLOG:
         render_wtlog(fb, &m->wt, &m->prefs, m->now, m->tz_off, m->log_scrub,
                      h);
         break;
      case SCR_WTDEL: render_wtdel(fb, &m->wt, &m->prefs, m->tz_off, h); break;
      case SCR_ALARM: render_alarm(fb, m, h); break;
      case SCR_EXPORT: render_export(fb, m, h); break;
      case SCR_DISPLAY: render_display(fb, m, h); break;
      case SCR_OLDDEV: render_olddev(fb, m, h); break;
      case SCR_LABEL: render_label(fb, m, h); break;
      case SCR_FOODTYPE: render_foodtype(fb, m, h); break;
      case SCR_EXLOG: render_exlog(fb, m, h); break;
      case SCR_EXEDIT: render_exedit(fb, m, h); break;
      case SCR_EXDEL: render_exdel(fb, m, h); break;
      case SCR_FOODLOG: render_foodlog(fb, m, h); break;
      case SCR_FOODDEL: render_fooddel(fb, m, h); break;
      case SCR_FOOD: render_food(fb, m, h); break;
      /* ONE PICKER FOR BOTH, shape and colour: they are two columns of one
       * screen, so there is one screen and one renderer. */
      case SCR_MARKPICK: render_markpick(fb, m, h); break;
      case SCR_MAIN: render_main(fb, m, h); break;
      case SCR_N: break; /* not a screen; only bounds the enum */
   }
   /* THE REFUSAL, OVER WHATEVER IS THERE, and drawn last so nothing can
    * cover it.
    *
    * The status row is rendered on exactly one screen -- the pre-reading
    * main screen, which a user stops seeing the moment they own a device --
    * so it cannot carry a refusal: a refused rename, a refused disconnect, a
    * registry that will not load would all say their piece to nobody. A
    * banner needs no room reserved for it, which is what lets it appear on
    * screens whose line budgets are already spent.
    *
    * IT RECORDS NO TOUCH TARGET. It sits over the bottom line for a few
    * seconds, so a control under it is still exactly where it was and still
    * answers a tap. */
   uint32_t *px = fb->bits;
   if (m->refused[0] && px) {
      /* THE DENSEST LINE ANY SCREEN USES, which is what UI_BANNER_ROWS names:
       * the banner covers whatever is under it, so it takes the smallest line
       * the layout ever asks ui_fit_scale for and no more.
       *
       * The text is 33 columns of 6*sc plus a 2*sc margin, which is 2*sc wider
       * than the 33-column fit ui_fit_scale guarantees, so the scale comes
       * down once more where that overhangs. */
      int sc = ui_fit_scale(fb->width, fb->height, UI_BANNER_ROWS);
      while (sc > 1 && (2 * sc) + (UI_COLS * 6 * sc) > fb->width)
         sc--;
      const int lh = 16 * sc;
      int by       = fb->height - lh;
      if (by < 0)
         by = 0;
      for (long yy = by; yy < fb->height; yy++)
         for (long xx = 0; xx < fb->width; xx++)
            px[(yy * fb->stride) + xx] = UI_BLACK;
      draw_str(px, fb, 2 * sc, by + (3 * sc), sc, m->refused, UI_DANGER);
   }
   /* AND A FRAME THAT COULD NOT RECORD ALL ITS CONTROLS NAMES THE SCREEN.
    * add_hit says once per process THAT it happened; only here is it known
    * WHICH screen asked for more than the table holds, which is the half a
    * reader needs to find the layout that overspent. */
   {
      /* THE LAST FRAME'S ANSWER, RECORDED WHETHER OR NOT IT FIRED. Assigning it
       * only inside the branch means a clean frame between two bad ones leaves
       * the remembered value at the bad one, so the second occurrence says
       * nothing -- and a screen that overflows, is left, and is opened again is
       * exactly the sequence a reader needs to see twice. */
      static int ovf_scr = -1;
      int now_scr        = h->overflow ? (int)m->scr : -1;
      if (h->overflow && now_scr != ovf_scr)
         LOGW("ui: screen %d records more touch targets than the %d this build "
              "holds; controls on it are drawn and cannot be tapped",
              now_scr, UI_MAX_HITS);
      ovf_scr = now_scr;
   }
   /* WHAT THE FRAME LOST, said out loud once per change. Every frame at 1 Hz
    * would bury the log, and a screen that clips clips the same amount every
    * frame it is up. */
   {
      /* ONCE PER SCREEN, NOT PER COUNT. The count is in dropped glyph PIXELS,
       * so a clipping screen whose text changes width -- an age that ticks
       * over, a countdown crossing a digit -- produces a different number every
       * second, and reporting each one fills the log at the frame rate with
       * restatements of one fact. The screen is the fact; the number is
       * illustration.
       *
       * Recorded every frame, including the clean ones, so a screen that
       * clipped, was left, and is opened again reports twice. */
      static int last_scr = -1;
      int now_scr         = g_clipped > 0 ? (int)m->scr : -1;
      if (now_scr >= 0 && now_scr != last_scr)
         LOGW("ui: screen %d laid out %ld units past the %dx%d edge", now_scr,
              g_clipped, fb->width, fb->height);
      last_scr = now_scr;
   }
}

int ui_hit_idx(const struct hits *h, int x, int y)
{
   /* last box wins, matching draw order (later-drawn overlays are on top) */
   for (int i = h->n - 1; i >= 0; i--) {
      int bx = h->box[i].x;
      int by = h->box[i].y;
      if (x >= bx && x < bx + h->box[i].w && y >= by && y < by + h->box[i].h)
         return i;
   }
   return -1;
}

struct action ui_hit(const struct hits *h, int x, int y)
{
   int i = ui_hit_idx(h, x, y);
   if (i < 0)
      return (struct action){ACT_NONE, 0, 0};
   return (struct action){h->box[i].kind, h->box[i].code, h->box[i].arg};
}

void ui_dim(struct ANativeWindow_Buffer *fb)
{
   uint32_t *px = fb->bits;
   for (int j = 0; j < fb->height; j++)
      for (int i = 0; i < fb->width; i++) {
         uint32_t c = px[(j * fb->stride) + i];
         /* Two channels per multiply: red+blue share one 13/16 scaling,
          * green the other. Each 8-bit field times 13 stays under 16 bits,
          * so the fields cannot bleed into each other before the mask. */
         uint32_t rb = ((c & 0x00FF00FFU) * 13U >> 4U) & 0x00FF00FFU;
         uint32_t g  = ((c & 0x0000FF00U) * 13U >> 4U) & 0x0000FF00U;
         px[(j * fb->stride) + i] = (c & 0xFF000000U) | rb | g;
      }
}

void ui_press_overlay(struct ANativeWindow_Buffer *fb, int x, int y, int w,
                      int h)
{
   uint32_t *px = fb->bits;
   int x0       = x < 0 ? 0 : x;
   int y0       = y < 0 ? 0 : y;
   int x1       = (x + w > fb->width) ? fb->width : x + w;
   int y1       = (y + h > fb->height) ? fb->height : y + h;
   for (int j = y0; j < y1; j++)
      for (int i = x0; i < x1; i++) {
         uint32_t c = px[(j * fb->stride) + i];
         uint32_t r = (c >> 16U) & 0xFFU;
         uint32_t g = (c >> 8U) & 0xFFU;
         uint32_t b = c & 0xFFU;
         uint32_t m = r > g ? r : g;
         if (b > m)
            m = b;
         /* FOREGROUND only -- the text/graphics drawn IN the control, by
          * brightness, NOT merely "not zero": that test once scaled the
          * (then not-quite-black) background to full white. Content starts
          * at the 0x555555 frame gray; anything dimmer (the black clear,
          * faint fills) is background and must not change. */
         if (m < 0x40U)
            continue;
         /* Scale so the brightest channel saturates: the same hue at full
          * intensity. Gray text goes white; green/red buttons stay green/
          * red (their 0x555555 frames go white, so every control shows). */
         r = (r * 0xFFU) / m;
         g = (g * 0xFFU) / m;
         b = (b * 0xFFU) / m;
         px[(j * fb->stride) + i] =
             (c & 0xFF000000U) | (r << 16U) | (g << 8U) | b;
      }
}
