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

/* The whole UI as a pure function of an immutable `struct screen`: ui_render()
 * draws the current screen into a locked framebuffer and records its touch
 * targets into `struct hits`; ui_hit() maps a later tap to the action the shell
 * (main.c) should perform. No globals, no callbacks -- so every screen builds
 * and runs on the host against a malloc'd buffer (see test/uitest.c, which
 * renders each screen to a PPM and checks its hit-targets with no phone). */
#include "ndk.h"
#include "ui.h"
#include "uiact.h"
#include "uidraw.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>

/* Glyph cells discarded by clipping since the last ui_clip_reset().
 *
 * Clipping is SILENT: draw_cell drops out-of-bounds pixels, so content laid
 * out past the edge simply never appears and leaves no trace -- no hit box out
 * of range, no colour missing if that colour is drawn elsewhere too. The
 * offline harness failed to notice an entirely invisible stats table and a
 * truncated medical disclaimer for several review rounds because of exactly
 * this. One counter makes it observable on every screen at once.
 *
 * AND IT STAYS A GLOBAL, deliberately. A review measured that this counter
 * makes 46 renderers "effectful" and proposed returning it from the render
 * entry points instead. That trade is a bad one: the increment happens in
 * draw_cell, draw_frame and fill_rect -- the three leaf primitives, reached
 * from all 46 -- so threading a count out of them means an out-parameter on
 * every drawing primitive and every caller of one, to move a number no
 * drawing decision ever reads.
 *
 * It is not logic, it is an instrument: written by the primitives, read only
 * by test/app/uitest.c, reset per screen. Nothing branches on it, so it cannot
 * make one render depend on a previous one -- which is the property the purity
 * argument is actually there to protect. */
static long g_clipped;

void ui_clip_bump(long n)
{
   g_clipped += n;
}

/* 720h = 30D on the right. 6H went: it sat between 3H and 12H without showing
 * anything either of them didn't. */
const int ui_tab_hours[UI_TABS] = {3, 12, 24, 72, 168, 720};

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h)
{
   h->n        = 0;
   h->overflow = 0;
   /* TRUE black: zero photons on an OLED, rather than a near-black wash. */
   clear_fb(fb, 0xFF000000);
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
         render_wtlog(fb, &m->wt, &m->prefs, m->now, m->tz_off, h);
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
      case SCR_MARKPICK:
      case SCR_COLORPICK: render_markpick(fb, m, h); break; /* combined menu */
      case SCR_MAIN: render_main(fb, m, h); break;
      case SCR_N: break; /* not a screen; only bounds the enum */
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
