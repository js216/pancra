// SPDX-License-Identifier: GPL-3.0
// uitest.c --- Offline UI harness: render the pure core to PPM + check hit-test
// Copyright 2026 Jakob Kastelic

/* The UI is a pure function of a `struct screen`, so it runs with no phone:
 * fill a model, call ui_render into a plain framebuffer, dump a PPM (into
 * tmp/uitest/, never the source tree), and assert that ui_hit maps a tap to the
 * right action. As each screen is ported this harness grows a case per screen.
 * Built and run by `make uitest`. */
#include "ui.h"
#include "insulin.h" /* INS_FAST: the dose the full-history plot test logs */
#include "ndk.h"
#include "plot.h" /* the capping case asserts plot_render's own mapping */
#include "sensors.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 720
#define H 360
/* The sensor list needs a real phone's portrait height; allocate for the
 * tallest buffer any case renders so a tall render cannot overrun. */
#define TALL_H 1600
/* The reachability sweep renders real device geometries up to 1440x3200, which
 * is far larger than W x TALL_H -- g_px must cover the LARGEST buffer any case
 * uses, not the nominal one, or the sweep writes off the end of the
 * allocation. (It did: the first run segfaulted here.) */
#define MAX_W 1440
#define MAX_H 3200

static uint32_t *g_px;
static struct ANativeWindow_Buffer g_buf;

static void write_ppm_buf(const struct ANativeWindow_Buffer *b,
                          const char *path)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      perror(path);
      exit(1);
   }
   fprintf(f, "P6\n%d %d\n255\n", b->width, b->height);
   for (int i = 0; i < b->width * b->height; i++) {
      uint32_t c         = g_px[i];
      unsigned char p[3] = {(unsigned char)(c >> 16U), (unsigned char)(c >> 8U),
                            (unsigned char)c};
      fwrite(p, 1, 3, f);
   }
   fclose(f);
}

static void write_ppm(const char *path)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      perror(path);
      exit(1);
   }
   fprintf(f, "P6\n%d %d\n255\n", W, H);
   for (int i = 0; i < W * H; i++) {
      uint32_t c         = g_px[i];
      unsigned char b[3] = {(unsigned char)(c >> 16U), (unsigned char)(c >> 8U),
                            (unsigned char)c};
      fwrite(b, 1, 3, f);
   }
   fclose(f);
}

/* Count exact-colour pixels in a buffer. The multi-sensor plot styling is only
 * observable in the pixels -- there is no hit target for a marker -- so this is
 * what proves ui_sensor_color() and the non-primary/orphan branches ran. */
static long count_color(const struct ANativeWindow_Buffer *b, uint32_t want)
{
   long n = 0;
   for (int i = 0; i < b->width * b->height; i++)
      if (g_px[i] == want)
         n++;
   return n;
}

/* Same, but only at or below row y0. Whole-buffer counting is NOT a valid
 * visibility test when a colour is shared: 0xFFCCCCCC is drawn both by
 * render_info (the block under test) and by render_glucose's label column near
 * the top, so a whole-buffer count stayed comfortably positive while every row
 * render_info draws was off the bottom of the screen. Restricting to the lower
 * half is what makes the assertion actually about render_info. */
static long count_color_from(const struct ANativeWindow_Buffer *b,
                             uint32_t want, int x0, int y0)
{
   long n = 0;
   for (int y = y0; y < b->height; y++)
      for (int x = x0; x < b->width; x++)
         if (g_px[(y * b->stride) + x] == want)
            n++;
   return n;
}

/* Where render_info lives, which depends on orientation: it is the lower half
 * in portrait but the RIGHT-HAND COLUMN in landscape (render_main splits the
 * screen). Getting this wrong makes the assertion test the wrong pixels. */
static long count_info_block(const struct ANativeWindow_Buffer *b,
                             uint32_t want)
{
   int landscape = b->width > b->height;
   return landscape ? count_color_from(b, want, b->width / 2, 0)
                    : count_color_from(b, want, 0, b->height / 2);
}

static long lit_pixels(uint32_t bg)
{
   long n = 0;
   for (int i = 0; i < W * H; i++)
      if (g_px[i] != bg)
         n++;
   return n;
}

int main(void)
{
   g_px  = malloc((size_t)MAX_W * MAX_H * 4);
   g_buf = (struct ANativeWindow_Buffer){
       .width = W, .height = H, .stride = W, .format = 1, .bits = g_px};

   /* newest-first history spanning a few hours so the plot has points */
   struct ui_point hist[4] = {
       {.t = 900, .glu = 148},
       {.t = 800, .glu = 143},
       {.t = 400, .glu = 155},
       {.t = 100, .glu = 132}
   };
   struct ui_stat s = {.have = 1, .tir = 82, .avg = 149};
   struct screen m  = {
       .scr             = SCR_MAIN,
       .now             = 1000,
       .glu             = 148,
       .trend           = 2,
       .t               = 900,
       .rssi            = -72,
       .rssi_ok         = 1,
       .hist            = hist,
       .nhist           = 4,
       .scrub           = -1,
       .plot_hours      = 24,
       .plot_max        = 300,
       .bonded          = 1,
       .have_reading    = 1,
       .predicted       = 152,
       .sequence        = 41,
       .session_seconds = 3L * 86400,
       .stored          = 812,
       .units           = 0,
       .alarm_low       = 100,
       .alarm_high      = 300,
       /* Set, not left zero: a zero pair renders as "OFF"/"OFF", the SHORTEST
        * the nudge row can be, so the sweep would measure the easy case of a
        * row whose whole reason for existing is to carry two more numbers.
        * Widest realistic values instead. */
       .nudge_low   = 130,
       .nudge_high  = 220,
       .nudge_sound = 1,
       .nudge_vib   = 1,
       .status      = "CONNECTED",
       .stat        = {s, s, s, s, s},
   };

   int fail = 0;

   /* --- portrait main screen --- */
   struct hits h;
   ui_render(&g_buf, &m, &h);
   write_ppm("tmp/uitest/main.ppm");
   long lit = lit_pixels(0xFF181818);
   printf("uitest: main.ppm %dx%d, %ld lit pixels, %d hit targets\n", W, H, lit,
          h.n);
   if (lit < 500) {
      printf("  FAIL: main screen rendered almost nothing\n");
      fail = 1;
   }
   /* Settings now opens ONLY from the hamburger (top-right), not the whole top
    * band -- so a tap on the number no longer navigates away by accident. The
    * hamburger's ACT_OPEN_SETTINGS target must exist and be tappable. */
   {
      int saw_settings = 0;
      for (int i = 0; i < h.n; i++)
         if (h.box[i].kind == ACT_OPEN_SETTINGS) {
            saw_settings = 1;
            /* and tapping inside its box resolves to it */
            if (ui_hit(&h, h.box[i].x + (h.box[i].w / 2),
                       h.box[i].y + (h.box[i].h / 2))
                    .kind != ACT_OPEN_SETTINGS)
               saw_settings = 0;
         }
      if (!saw_settings) {
         printf("  FAIL: no tappable hamburger (ACT_OPEN_SETTINGS) on main\n");
         fail = 1;
      }
   }
   /* the tab row, the plot, and the two alarm-threshold targets (LOW/HIGH
    * value -> keypad; the old +- steppers are gone) must each be reachable */
   int saw_tab   = 0;
   int saw_scrub = 0;
   int saw_low   = 0;
   int saw_high  = 0;
   for (int i = 0; i < h.n; i++) {
      int k     = h.box[i].kind;
      saw_tab   = saw_tab || (k == ACT_PLOT_TAB);
      saw_scrub = saw_scrub || (k == ACT_SCRUB);
      saw_low   = saw_low || (k == ACT_MENU && h.box[i].arg == MA_ALARM_LOW);
      saw_high  = saw_high || (k == ACT_MENU && h.box[i].arg == MA_ALARM_HIGH);
   }
   if (!(saw_tab && saw_scrub && saw_low && saw_high)) {
      printf("  FAIL: missing targets tab=%d scrub=%d low=%d high=%d\n",
             saw_tab, saw_scrub, saw_low, saw_high);
      fail = 1;
   }
   /* a tap in dead space (far right edge, top) must do nothing */
   if (ui_hit(&h, W - 2, 1).kind != ACT_NONE) {
      printf("  FAIL: empty-area tap should be ACT_NONE\n");
      fail = 1;
   }

   /* --- no-reading screen (scan status + sensor list) --- */
   struct ui_dev devs[2] = {
       {"DX01AB", "F8:DA:11:22:33:44", -61},
       {"DX01CD", "C1:22:33:44:55:66", -80}
   };
   struct screen scan = {.scr       = SCR_MAIN,
                         .glu       = -1,
                         .status    = "SCANNING",
                         .adv_total = 137,
                         .devs      = devs,
                         .ndev      = 2};
   ui_render(&g_buf, &scan, &h);
   write_ppm("tmp/uitest/scan.ppm");
   if (lit_pixels(0xFF181818) < 200) {
      printf("  FAIL: no-reading screen rendered almost nothing\n");
      fail = 1;
   }
   /* It must be possible to REACH SETTINGS with no reading yet. This screen
    * recorded no hit targets at all, and on_input has no fallback -- so on a
    * fresh install the pairing flow was unreachable by touch, which is the one
    * thing a fresh install has to do. */
   {
      int saw_set = 0;
      for (int i = 0; i < h.n; i++)
         saw_set = saw_set || (h.box[i].kind == ACT_OPEN_SETTINGS);
      if (!saw_set) {
         printf("  FAIL: no-reading screen has no way to open settings\n");
         fail = 1;
      }
   }

   /* --- settings screen (rows carry menu_action codes via ACT_MENU) --- */
   struct screen set = m;
   set.scr           = SCR_SETTINGS;
   set.plot_max      = 300;
   set.sound_on      = 1;
   set.screen_on     = 1;
   set.perm[0] = set.perm[1] = set.perm[2] = 1;
   set.batt_ok                             = 1;
   set.standby_bucket                      = 10;
   set.code                                = "1234";
   set.mac                                 = "F8:DA:11:22:33:44";
   set.model                               = "SW11163";
   set.fw                                  = "1.6.5.15";
   ui_render(&g_buf, &set, &h);
   write_ppm("tmp/uitest/settings.ppm");
   if (lit_pixels(0xFF181818) < 500) {
      printf("  FAIL: settings screen rendered almost nothing\n");
      fail = 1;
   }
   /* the title band is the close target (menu_action 99) */
   struct action ca = ui_hit(&h, W / 2, H / 6);
   if (ca.kind != ACT_MENU) {
      printf("  FAIL: settings title tap should be ACT_MENU (got %d)\n",
             ca.kind);
      fail = 1;
   }
   /* at least one actionable row must be recorded */
   int saw_menu = 0;
   for (int i = 0; i < h.n; i++)
      saw_menu = saw_menu || (h.box[i].kind == ACT_MENU);
   if (!saw_menu) {
      printf("  FAIL: settings recorded no ACT_MENU targets\n");
      fail = 1;
   }

   /* --- pairing keypad (digits carry menu_action codes) --- */
   struct screen kp = {.scr = SCR_KEYPAD, .kp_mode = 0, .entry = "12"};
   ui_render(&g_buf, &kp, &h);
   write_ppm("tmp/uitest/keypad.ppm");
   int keys = 0;
   for (int i = 0; i < h.n; i++)
      keys += (h.box[i].kind == ACT_MENU);
   if (keys < 12) { /* 12 keys + close band */
      printf("  FAIL: keypad recorded %d ACT_MENU targets, want >=12\n", keys);
      fail = 1;
   }

   /* --- device list (a pick per scanned sensor) --- */
   struct screen dl = {.scr = SCR_DEVLIST, .devs = devs, .ndev = 2};
   ui_render(&g_buf, &dl, &h);
   write_ppm("tmp/uitest/devlist.ppm");
   int picks = 0;
   for (int i = 0; i < h.n; i++)
      picks += (h.box[i].kind == ACT_MENU && h.box[i].arg >= 200);
   if (picks != 2) {
      printf("  FAIL: device list recorded %d picks, want 2\n", picks);
      fail = 1;
   }

   /* --- sensor list in settings (space check + rows) --- */
   struct ui_sensor sens[3] = {
       {.id              = 1,
        .type            = SENSOR_STELO,
        .kind            = KIND_CGM,
        .primary         = 1,
        .color           = 0,
        .marker          = 0,
        .last            = 900,
        .session_seconds = 3L * 86400,
        .connected       = 1,
        .label           = "STELO",
        .status          = "CONNECTED",
        .mac             = "F8:DA:11:22:33:44",
        .model           = "SW11163",
        .fw              = "1.6.5.15"},
       {.id     = 2,
        .type   = SENSOR_G7,
        .kind   = KIND_CGM,
        .color  = 1,
        .marker = 1,
        .last   = 400,
        .label  = "G7",
        .status = "IDLE",
        .mac    = "C1:22:33:44:55:66"},
       {.id     = 3,
        .type   = SENSOR_ONETOUCH,
        .kind   = KIND_BGM,
        .color  = 2,
        .marker = 2,
        .last   = 100,
        .label  = "KITCHEN",
        .status = "OFF",
        .mac    = "F7:F0:20:2D:77:28"},
   };
   /* A realistic epoch: session start/end rows derive from now, so the toy
    * clock used by the plot cases would render them as pre-1970 nonsense. */
   const long now_ts = 1784358853L; /* 2026-07-18 */
   set.now           = now_ts;
   sens[0].last      = now_ts - 100;
   sens[1].last      = now_ts - 4000;
   sens[2].last      = now_ts - 200000;
   set.sensors       = sens;
   set.nsensors      = 3;
   set.sel           = -1;
   /* A phone-shaped portrait buffer: the 720x360 test surface is far too short
    * for the sensor list, and asserting on it would test nothing real. */
   struct ANativeWindow_Buffer tall = {
       .width = W, .height = TALL_H, .stride = W, .format = 1, .bits = g_px};
   int cap = ui_sensor_capacity(W, TALL_H);
   printf("uitest: sensor capacity at %dx%d = %d (min %d, max %d)\n", W, TALL_H,
          cap, UI_MIN_SLOTS, UI_MAX_SLOTS);
   if (cap < UI_MIN_SLOTS) {
      printf("  FAIL: portrait phone must fit at least %d sensors\n",
             UI_MIN_SLOTS);
      fail = 1;
   }
   ui_render(&tall, &set, &h);
   write_ppm_buf(&tall, "tmp/uitest/settings_tall.ppm");
   {
      int saw[3] = {0, 0, 0};
      for (int i = 0; i < h.n; i++)
         for (int k = 0; k < 3; k++)
            if (h.box[i].kind == ACT_MENU && h.box[i].arg == MA_SENSOR + k)
               saw[k] = 1;
      if (!(saw[0] && saw[1] && saw[2])) {
         printf("  FAIL: sensor rows not all tappable (%d %d %d)\n", saw[0],
                saw[1], saw[2]);
         fail = 1;
      }
      int saw_add = 0;
      for (int i = 0; i < h.n; i++)
         saw_add = saw_add ||
                   (h.box[i].kind == ACT_MENU && h.box[i].arg == MA_ADDSENSOR);
      if (!saw_add) {
         printf("  FAIL: no ADD NEW SENSOR target\n");
         fail = 1;
      }
   }

   /* --- layout must fit, and essential controls must be REACHABLE ---
    *
    * Two failure modes, both shipped before: (a) the scale was derived from
    * WIDTH while the pitch it produced was spent on HEIGHT, so capacity fell
    * below the minimum on 16:9/18:9 phones and render_settings drew no sensor
    * rows and no ADD row at all; (b) ui_settings_scale and ui_sensor_capacity
    * disagreed by 8*sc, so the lockout still fired on 1080x2280 (Galaxy S10)
    * and 1440x3200. Scrolling is ruled out by design, so off-screen content is
    * simply unreachable. Sweep geometries AND assert the controls exist. */
   {
      static const int shapes[][2] = {
          {1080, 1920},
          {1440, 2560},
          {1080, 2160},
          {1080, 2280},
          {1440, 3120},
          {1440, 3200},
          {1080, 2340},
          {828,  1792},
          {1080, 2400},
          {720,  1600},
          {720,  1280},
          {480,  800 },
          /* LANDSCAPE. The sweep had none, which is why three screens kept
           * width-only scaling: in landscape that puts their controls below
           * the buffer, and SCR_FORGET records no close target, so it became
           * a dead end with no way out. */
          {1920, 1080},
          {2340, 1080},
          {2400, 1080},
          {2560, 1440},
          {3120, 1440},
          {1600, 720 },
          /* Split-screen / freeform windows. The manifest allows resizing, so
           * these heights are reachable, and they are where the info block was
           * still being laid out entirely off-screen. */
          {1080, 1100},
          {1080, 1400},
          {1080, 1500},
          {1440, 1300},
          /* SMALL windows. The list jumped from {480,800} straight to 1080-wide
           * split-screen shapes, skipping the whole 715..1078 band at narrow
           * widths -- which is exactly where the out-of-range banner was being
           * laid out past the bottom of the buffer. */
          {480,  720 },
          {480,  760 },
          {540,  730 },
          {600,  740 },
          {640,  900 },
          {720,  748 },
          {720,  1000},
          {768,  1000},
          {828,  1075},
          {900,  1078},
      };
      int nshape = (int)(sizeof shapes / sizeof shapes[0]);
      int nscr   = 0; /* set inside the loop from the array itself */
      for (int i = 0; i < nshape; i++) {
         int sw   = shapes[i][0];
         int sh   = shapes[i][1];
         int ssc  = ui_settings_scale(sw, sh);
         int scap = ui_sensor_capacity(sw, sh);
         int slh  = 16 * ssc;
         /* UI_SET_ABOVE, not a literal: a stale copy of the overhead here is
          * exactly the drift the dense sweep below exists to catch. */
         int need = (sh / 20) + (8 * ssc) + ((UI_SET_ABOVE + scap + 1) * slh);
         if (scap < UI_MIN_SLOTS || need > sh) {
            printf("  FAIL: %dx%d capacity %d (min %d), needs %d of %d px\n",
                   sw, sh, scap, UI_MIN_SLOTS, need, sh);
            fail = 1;
         }
      }
      /* Dense sweep: the two functions must agree at EVERY plausible size, not
       * just at the dozen shapes above -- that is how the 8*sc gap survived. */
      int swept                 = 0;
      int swbad                 = 0;
      static const int widths[] = {480, 540, 600,  640,  720,  768,
                                   828, 900, 1080, 1200, 1242, 1440};
      for (unsigned wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
         int sw = widths[wi];
         for (int sh = (sw * 12) / 10; sh <= sw * 3; sh++) {
            int ssc  = ui_settings_scale(sw, sh);
            int scap = ui_sensor_capacity(sw, sh);
            int need =
                (sh / 20) + (8 * ssc) + ((UI_SET_ABOVE + scap + 1) * 16 * ssc);
            swept++;
            if (scap < UI_MIN_SLOTS || need > sh) {
               if (!swbad)
                  printf("  FAIL: sweep %dx%d cap=%d need=%d\n", sw, sh, scap,
                         need);
               swbad++;
            }
         }
      }
      if (swbad)
         fail = 1;
      printf("uitest: layout swept %d geometries, %d bad\n", swept, swbad);

      /* Reachability + VISIBILITY.
       *
       * A hit-box bounds check alone is not enough and gave false confidence:
       * draw_cell clips, so text drawn past the bottom simply never appears --
       * it has no hit box to be out of bounds. The whole stats table and the
       * LOW/HIGH/STALE banner were invisible on every realistic phone window
       * while this sweep reported OK. Colour presence is the honest detector:
       * if the content did not land on screen, its pixels do not exist. */
      struct ui_sensor rs = sens[0];
      rs.session_seconds  = 16L * 86400; /* expired -> PAIR NEW SENSOR prompt */
      /* Populate EVERY optional row, or render_sensor's worst case -- the one
       * ui_fit_scale(.., 25) is sized for -- is never actually exercised. */
      (void)snprintf(rs.serial, sizeof rs.serial, "SN123456");
      (void)snprintf(rs.code, sizeof rs.code, "1234");
      rs.predicted = 152;
      rs.sequence  = 41;
      for (int i = 0; i < nshape; i++) {
         int sw                         = shapes[i][0];
         int sh                         = shapes[i][1];
         struct ANativeWindow_Buffer rb = {.width  = sw,
                                           .height = sh,
                                           .stride = sw,
                                           .format = 1,
                                           .bits   = g_px};
         /* Sweep EVERY screen, not just three -- the excluded ones were
          * exactly where the un-converted width-only scaling survived. */
         /* Every screen built from framed BUTTONS belongs here, not just the
          * ones that existed when the sweep was written. The button height is
          * shared by all of them (menu_button), so a change to its padding
          * moves every one of these at once -- and the screens that were
          * missing were exactly the ones nothing measured: ADDMENU is nothing
          * BUT stacked buttons, and ALARM grew a row. A screen absent from
          * this list is a screen where a control can sit below the bottom
          * edge, draw nothing, and still record a hit box the gate accepts. */
         static const int scrs[] = {
             SCR_MAIN,       SCR_SETTINGS, SCR_SENSOR,   SCR_CAL,
             SCR_FORGET,     SCR_LABEL,    SCR_KEYPAD,   SCR_DEVLIST,
             SCR_GATE,       SCR_SENSTYPE, SCR_REMOTE,   SCR_ADDMENU,
             SCR_ALARM,      SCR_EXPORT,   SCR_INSDEL,   SCR_RECONF,
             SCR_METERHELP,  SCR_PAIRCONF, SCR_CALPEND,  SCR_RESCALE,
             SCR_RESCALEACT, SCR_DISPLAY,  SCR_PRIMPICK, SCR_OLDDEV,
             SCR_MARKPICK,   SCR_COLORPICK};
         nscr = (int)(sizeof scrs / sizeof scrs[0]);
         for (int c = 0; c < (int)(sizeof scrs / sizeof scrs[0]); c++) {
            struct screen rr = set;
            rr.scr           = scrs[c];
            /* SCR_CAL was only ever swept with cal_have == 0, which renders a
             * short early-return stub -- the full panel (bounds, last result,
             * ENTER VALUE) was never laid out at any swept geometry. */
            rr.cal_have      = 1;
            rr.cal_permitted = 1;
            rr.cal_status    = 2;
            rr.cal_last_bg   = 142;
            rr.cal_result    = 0;
            rr.now           = now_ts;
            rr.t             = now_ts - 100;
            /* A FULL sensor list, not one row.
             *
             * This swept nsensors = 1, so the settings screen -- the only
             * screen whose height grows with content -- was never laid out at
             * its worst case at any of these geometries. A mutation adding
             * three rows to render_settings without bumping UI_SET_ABOVE
             * passed the entire gate, while a full list clipped 8368 glyph
             * cells and put hit boxes 52 px below a 1080x1920 screen. With no
             * scrolling those rows and their tap targets are permanently
             * unreachable. The list is capped to what the geometry claims it
             * can show, so this asserts the screen honours its OWN promise. */
            /* ...and the list must be as long as the geometry CLAIMS it can
             * show, plus the optional rows. `min(cap, 3)` left every screen
             * with cap > 3 short of its own worst case, and none of the three
             * conditional rows was ever drawn, so UI_SET_ABOVE could
             * under-count by three and the gate still passed while EXPORT
             * DATA sat 130 px below the bottom edge. The worst case is: the
             * full list, one device RETIRED (the "OLD DEVICES (n)" row), more
             * live devices than fit (the "N MORE NOT SHOWN" row), and a
             * pairing armed (the "PENDING..." row). */
            int cap_here = ui_sensor_capacity(sw, sh);
            static struct ui_sensor full[UI_MAX_SLOTS];
            int nfull = cap_here + 2; /* > what fits: forces "MORE NOT SHOWN" */
            if (nfull > UI_MAX_SLOTS)
               nfull = UI_MAX_SLOTS;
            for (int q = 0; q < nfull; q++) {
               full[q]     = sens[q % 3];
               full[q].id  = q + 1;
               full[q].old = 0;
               /* EVERY optional attribute present -- which is the ordinary
                * state of a CGM once DIS has answered and a pairing code is
                * stored. Without these, render_sensor was swept with a stub
                * that skips the ENDS / REMAINING / PRED / SN / CODE rows, so
                * its row budget could be three rows short and the gate still
                * passed while DISCONNECT -- the one destructive action --
                * was laid out entirely below the buffer. */
               full[q].wear_len        = 10L * 86400;
               full[q].session_seconds = 7L * 86400;
               full[q].predicted       = 150;
               full[q].sequence        = 2029;
               full[q].connected       = 1;
               full[q].glu             = 140;
               full[q].cal_t           = now_ts - 3600;
               full[q].cal_mgdl        = 120;
               full[q].cal_state       = CAL_ST_APPLIED;
               snprintf(full[q].serial, sizeof full[q].serial, "SN1234567");
               snprintf(full[q].code, sizeof full[q].code, "7381");
            }
            full[nfull - 1].old = 1; /* the OLD DEVICES row */
            rr.sensors          = full;
            rr.nsensors         = nfull;
            rr.pend_type        = 1; /* the PENDING... row */
            rr.sel              = 0;
            rr.devs             = devs;
            rr.ndev             = 2;
            rr.entry            = "1234";
            ui_clip_reset();
            ui_render(&rb, &rr, &h);
            /* Nothing may be laid out past an edge. This is the check that
             * actually catches invisible content -- hit-box bounds and colour
             * presence both miss it (a shared colour drawn elsewhere keeps the
             * count positive, and content without a hit box has no box to be
             * out of range). */
            if (ui_clipped() > 0) {
               printf("  FAIL: scr %d at %dx%d: %ld glyph cells clipped\n",
                      scrs[c], sw, sh, ui_clipped());
               fail = 1;
            }
            for (int k = 0; k < h.n; k++) {
               /* BOTH axes: the stats rows were clipped horizontally too. */
               /* A DEGENERATE box (w or h <= 0) can never satisfy ui_hit's
                * `y < by + h`, so the control draws normally and simply does
                * not respond. It also defeats the bounds test below, because a
                * negative h makes y+h SMALLER. That is how a keypad with 0 of
                * 12 tappable keys passed this sweep. Check it first. */
               /* A SUB-FINGERTIP box is the same defect one step milder:
                * w<=0/h<=0 catches only a fully dead target, so shrinking a
                * control to 1 px left it drawing normally and untappable, and
                * the gate stayed green. This check lived on SCR_KEYPAD alone;
                * everywhere else a 1-px control passed -- including the
                * first-run gate's CONTINUE, which would make the app unusable
                * from install, and the delete confirmation's CANCEL, whose
                * death leaves the destructive control under the finger. */
               if (h.box[k].w <= 0 || h.box[k].h <= 0 || h.box[k].w < 8 ||
                   h.box[k].h < 8) {
                  printf("  FAIL: scr %d at %dx%d: target kind=%d arg=%d is "
                         "unhittable (w=%d h=%d) -- draws but cannot be "
                         "tapped\n",
                         scrs[c], sw, sh, h.box[k].kind, h.box[k].arg,
                         h.box[k].w, h.box[k].h);
                  fail = 1;
               } else if (h.box[k].y < 0 || h.box[k].y + h.box[k].h > sh ||
                          h.box[k].x < 0 || h.box[k].x + h.box[k].w > sw) {
                  printf("  FAIL: scr %d at %dx%d: target kind=%d arg=%d "
                         "x=%d..%d y=%d..%d outside buffer\n",
                         scrs[c], sw, sh, h.box[k].kind, h.box[k].arg,
                         h.box[k].x, h.box[k].x + h.box[k].w, h.box[k].y,
                         h.box[k].y + h.box[k].h);
                  fail = 1;
               }
            }
         }
         /* Essential controls must EXIST (not merely be in bounds). */
         struct screen rm   = m;
         rm.now             = now_ts;
         rm.t               = now_ts - 100;
         rm.session_seconds = 16L * 86400;
         rm.sensors         = &rs;
         rm.nsensors        = 1;
         ui_clip_reset();
         ui_render(&rb, &rm, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d expired-main: %ld glyph cells clipped\n", sw,
                   sh, ui_clipped());
            fail = 1;
         }
         /* The SENSOR EXPIRED prompt is gone (it read as an error banner); the
          * '+' ADD entry point is now the in-app route to a replacement, so it
          * must exist on this screen -- at every geometry. */
         int saw_add = 0;
         for (int k = 0; k < h.n; k++)
            saw_add = saw_add || (h.box[k].kind == ACT_MENU &&
                                  h.box[k].arg == MA_ADD_OPEN);
         if (!saw_add) {
            printf("  FAIL: %dx%d: main screen has no '+' ADD target\n", sw,
                   sh);
            fail = 1;
         }
         /* The NO-READING main screen, across geometries. It is what a fresh
          * install shows -- the state in which the app has to be usable enough
          * to pair a sensor -- and it was only ever rendered at one size. */
         struct screen nr = {.scr       = SCR_MAIN,
                             .glu       = -1,
                             .now       = now_ts,
                             .status    = "SCANNING",
                             .adv_total = 137,
                             .devs      = devs,
                             .ndev      = 2};
         ui_clip_reset();
         ui_render(&rb, &nr, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d no-reading: %ld glyph cells clipped\n", sw,
                   sh, ui_clipped());
            fail = 1;
         }
         {
            int ok_set = 0;
            for (int k = 0; k < h.n; k++) {
               if (h.box[k].w <= 0 || h.box[k].h <= 0) {
                  printf("  FAIL: %dx%d no-reading: degenerate target\n", sw,
                         sh);
                  fail = 1;
               }
               ok_set = ok_set || (h.box[k].kind == ACT_OPEN_SETTINGS);
            }
            if (!ok_set) {
               printf("  FAIL: %dx%d no-reading: cannot reach settings\n", sw,
                      sh);
               fail = 1;
            }
         }

         /* UNBONDED main screen: render_info's STATE row then carries the
          * status string and the advert count, which is far wider than the AVG
          * row the budget was sized for. The harness set .bonded = 1 and never
          * cleared it, so this row was never laid out anywhere. */
         struct screen ub = m;
         ub.now           = now_ts;
         ub.t             = now_ts - 100;
         ub.bonded        = 0;
         ub.status        = "METER: REGISTER FAILED";
         ub.adv_total     = 1482137;
         ui_clip_reset();
         ui_render(&rb, &ub, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d unbonded main: %ld glyph cells clipped\n", sw,
                   sh, ui_clipped());
            fail = 1;
         }

         /* Repeat the whole main-screen check in mmol/L. The sweep only ever
          * used mg/dL, so the wider units label ("MMOL/L", 6 chars vs 5) was
          * never exercised -- and it is exactly what overflowed the row. */
         struct screen mm = m;
         mm.now           = now_ts;
         mm.t             = now_ts - 100;
         mm.units         = 1;
         ui_clip_reset();
         ui_render(&rb, &mm, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d mmol main: %ld glyph cells clipped\n", sw, sh,
                   ui_clipped());
            fail = 1;
         }
         /* The LOW banner and the stats table must actually be VISIBLE. */
         struct screen lo = m;
         lo.now           = now_ts;
         lo.t             = now_ts - 100;
         lo.glu = 45; /* below 50: the big number turns red, so a shared
                       * colour would make the banner check meaningless */
         lo.alarm_low       = 100;
         lo.session_seconds = 3L * 86400;
         ui_clip_reset();
         ui_render(&rb, &lo, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d low-main: %ld glyph cells clipped\n", sw, sh,
                   ui_clipped());
            fail = 1;
         }
         /* 0xFF2020E0 is the LOW banner's own colour -- see ui.c. Using
          * glu_color's red here was vacuous: the big number uses it too. */
         /* HIGH and STALE get the same treatment as LOW: each banner has a
          * colour no other element draws, so "visible" means the banner. */
         struct screen hi = lo;
         hi.glu           = 350;
         hi.alarm_high    = 300;
         hi.alarm_low     = 100;
         ui_render(&rb, &hi, &h);
         if (count_color(&rb, 0xFF20A0FF) <= 0) {
            printf("  FAIL: %dx%d: HIGH banner not visible on screen\n", sw,
                   sh);
            fail = 1;
         }
         struct screen st2 = lo;
         st2.disc_alarmed  = 1;
         ui_render(&rb, &st2, &h);
         if (count_color(&rb, 0xFF00D0FF) <= 0) {
            printf("  FAIL: %dx%d: STALE banner not visible on screen\n", sw,
                   sh);
            fail = 1;
         }
         ui_render(&rb, &lo, &h);
         if (count_color(&rb, 0xFF2020E0) <= 0) {
            printf("  FAIL: %dx%d: LOW banner not visible on screen\n", sw, sh);
            fail = 1;
         }
         if (count_info_block(&rb, 0xFFCCCCCC) <= 0) {
            printf("  FAIL: %dx%d: stats/info block not visible on screen\n",
                   sw, sh);
            fail = 1;
         }
         /* The gate's medical disclaimer must be visible in full. */
         struct screen gt = set;
         gt.scr           = SCR_GATE;
         ui_render(&rb, &gt, &h);
         /* 0xFF777777 is the DISCLAIMER's own colour. The previous check used
          * 0xFFCCCCCC -- the intro body copy, drawn at the top -- so it never
          * looked at the disclaimer at all, which really was being clipped. */
         if (count_color(&rb, 0xFF777777) <= 0) {
            printf("  FAIL: %dx%d: gate disclaimer not visible\n", sw, sh);
            fail = 1;
         }
      }
      /* Count derived, never hardcoded: a stale literal is exactly how a screen
       * gets dropped from the sweep without anyone noticing. */
      printf("uitest: reachability+visibility on %d shapes x %d screens\n",
             nshape, nscr);
   }

   /* --- an AUTO wear budget must not look like a pinned one ---
    *
    * Both states printed the same bare "10 DAYS", so a G7 whose model says
    * 15 days and whose stored pin says 10 was pixel-identical to one
    * correctly resolved to 10 -- the countdown ran five days short and the
    * screen offered nothing to explain why. Compared as PIXELS, not as a
    * string, so the check survives any rewording of the row. */
   {
      int w                          = 720;
      int hgt                        = 1600;
      uint32_t *shot                 = malloc((size_t)w * hgt * 4);
      struct ANativeWindow_Buffer wb = {
          .width = w, .height = hgt, .stride = w, .format = 1, .bits = g_px};
      struct hits wh;
      struct ui_sensor ws = sens[0];
      ws.kind             = KIND_CGM;
      ws.wear_len         = 10L * 86400; /* IDENTICAL budget in both renders */
      struct screen wm    = set;
      wm.scr              = SCR_SENSOR;
      wm.sensors          = &ws;
      wm.nsensors         = 1;
      wm.sel              = 0;
      long npx            = (long)w * hgt;
      ws.wear_auto        = 1;
      for (long q = 0; q < npx; q++)
         g_px[q] = 0;
      ui_render(&wb, &wm, &wh);
      for (long q = 0; q < npx; q++)
         shot[q] = g_px[q];
      ws.wear_auto = 0;
      for (long q = 0; q < npx; q++)
         g_px[q] = 0;
      ui_render(&wb, &wm, &wh);
      long diff = 0;
      for (long q = 0; q < npx; q++)
         if (shot[q] != g_px[q])
            diff++;
      if (diff == 0) {
         printf("  FAIL: a pinned wear budget renders identically to AUTO\n");
         fail = 1;
      } else {
         printf("uitest: AUTO vs pinned wear budget differ in %ld pixels\n",
                diff);
      }
      free(shot);
   }

   /* --- the ALARM and NUDGE rows must not share a single pixel ---
    *
    * ui_hit_idx scans BACKWARDS ("last box wins"), which is what makes the
    * app's deliberate overlaps safe: a specific control drawn later beats the
    * generous title-bar close target that contains it. These two rows are the
    * case where that rule bites instead, because neither contains the other --
    * they are siblings, drawn one after the other, carrying DIFFERENT actions
    * in the same screen columns. Any vertical overlap means the bottom edge of
    * "ALARM HIGH" silently opens the NUDGE HIGH keypad.
    *
    * It happened: the row's hit box was 3*sc taller than the row's own
    * advance, harmless while ALARM was the only such row and a 9 px
    * mis-actuation band (at 1080x1920) the moment NUDGE appeared under it.
    * Checked at every swept geometry, not one, because the band scales with
    * sc and vanishes at sc == 1. */
   {
      static const int thr[] = {MA_ALARM_LOW, MA_ALARM_HIGH, MA_NUDGE_LOW,
                                MA_NUDGE_HIGH};
      /* Its own list, wider than the reachability sweep's: the mis-actuation
       * band scales with sc, so both the smallest sc (where it vanishes) and
       * the largest (where it is widest) have to be covered, plus landscape,
       * where the row padding differs. */
      static const int tg[][2] = {
          {320,  480 },
          {480,  760 },
          {540,  960 },
          {720,  1280},
          {1080, 1920},
          {1080, 2400},
          {1440, 2560},
          {1440, 3200},
          {800,  480 },
          {1280, 720 },
          {1600, 720 },
          {1920, 1080},
          {2560, 1440},
          {1200, 1200},
      };
      int shapes = 0;
      int bad    = 0;
      for (unsigned s = 0; s < sizeof tg / sizeof tg[0]; s++) {
         int sw                         = tg[s][0];
         int sh                         = tg[s][1];
         struct ANativeWindow_Buffer tb = {.width  = sw,
                                           .height = sh,
                                           .stride = sw,
                                           .format = 1,
                                           .bits   = g_px};
         struct hits th;
         struct screen tm = set;
         tm.scr           = SCR_MAIN;
         ui_render(&tb, &tm, &th);
         shapes++;
         for (int i = 0; i < th.n; i++)
            for (int j = i + 1; j < th.n; j++) {
               int ai = -1;
               int aj = -1;
               for (int k = 0; k < 4; k++) {
                  if (th.box[i].arg == thr[k])
                     ai = k;
                  if (th.box[j].arg == thr[k])
                     aj = k;
               }
               if (ai < 0 || aj < 0 || th.box[i].w <= 0 || th.box[j].w <= 0)
                  continue;
               int ax2 = th.box[i].x + th.box[i].w;
               int ay2 = th.box[i].y + th.box[i].h;
               int bx2 = th.box[j].x + th.box[j].w;
               int by2 = th.box[j].y + th.box[j].h;
               if ((th.box[i].x < bx2) && (th.box[j].x < ax2) &&
                   (th.box[i].y < by2) && (th.box[j].y < ay2)) {
                  printf("  FAIL: %dx%d: threshold targets %d and %d overlap "
                         "(y %d..%d vs %d..%d)\n",
                         sw, sh, th.box[i].arg, th.box[j].arg, th.box[i].y, ay2,
                         th.box[j].y, by2);
                  bad  = 1;
                  fail = 1;
               }
            }
      }
      if (!bad)
         printf("uitest: ALARM/NUDGE threshold targets disjoint on %d shapes\n",
                shapes);
   }

   /* --- no screen may be a dead end ---
    *
    * A modal that records zero hit targets is unrecoverable: ui_render clears
    * them and on_input swallows every tap while a menu is open. Three screens
    * returned early on a stale `sel` and did exactly that -- blank screen,
    * force-stop required. Every modal must offer a way out regardless of state.
    */
   {
      static const int modal[] = {
          SCR_SENSOR,  SCR_CAL,      SCR_FORGET,   SCR_LABEL, SCR_KEYPAD,
          SCR_DEVLIST, SCR_SENSTYPE, SCR_SETTINGS, SCR_GATE,  SCR_REMOTE};
      for (int i = 0; i < (int)(sizeof modal / sizeof modal[0]); i++) {
         struct screen bad = set;
         bad.scr           = modal[i];
         bad.sel           = -1; /* stale selection */
         bad.nsensors      = 0;
         bad.sensors       = 0;
         bad.ndev          = 0;
         ui_render(&tall, &bad, &h);
         int usable = 0;
         for (int k = 0; k < h.n; k++)
            if (h.box[k].w > 0 && h.box[k].h > 0)
               usable = 1;
         if (!usable) {
            printf("  FAIL: screen %d with sel=-1 has no usable hit target "
                   "(dead end)\n",
                   modal[i]);
            fail = 1;
         }
      }
      /* ui_hit must return the target's ARG, not just its kind.
       *
       * The whole menu_action protocol rides on arg: every settings row, keypad
       * digit, sensor row, device pick and FORGET YES/NO is distinguished only
       * by it. Returning a constant collapses all of them onto MA_ORIENT --
       * "every tap rotates the screen" -- and the suite passed, because ui_hit
       * was called three times in this file and every other assertion read
       * h.box[] directly, bypassing the dispatch entirely. */
      {
         struct hits hh;
         struct screen sm               = set;
         sm.scr                         = SCR_SETTINGS;
         int bad                        = 0;
         int checked                    = 0;
         struct ANativeWindow_Buffer hb = {.width  = W,
                                           .height = TALL_H,
                                           .stride = W,
                                           .format = 1,
                                           .bits   = g_px};
         ui_render(&hb, &sm, &hh);
         for (int i = 0; i < hh.n; i++) {
            if (hh.box[i].kind != ACT_MENU || hh.box[i].w <= 0 ||
                hh.box[i].h <= 0)
               continue;
            int cx            = hh.box[i].x + (hh.box[i].w / 2);
            int cy            = hh.box[i].y + (hh.box[i].h / 2);
            struct action got = ui_hit(&hh, cx, cy);
            checked++;
            /* The last box covering the point wins, so compare against that. */
            int want_arg  = -1;
            int want_kind = -1;
            for (int k = hh.n - 1; k >= 0; k--)
               if (cx >= hh.box[k].x && cx < hh.box[k].x + hh.box[k].w &&
                   cy >= hh.box[k].y && cy < hh.box[k].y + hh.box[k].h) {
                  want_kind = hh.box[k].kind;
                  want_arg  = hh.box[k].arg;
                  break;
               }
            if (got.kind != want_kind || got.arg != want_arg) {
               printf("  FAIL: ui_hit at (%d,%d) gave kind=%d arg=%d, "
                      "want kind=%d arg=%d\n",
                      cx, cy, got.kind, got.arg, want_kind, want_arg);
               bad = 1;
            }
         }
         if (checked < 5) {
            printf("  FAIL: only %d ACT_MENU targets probed; the sweep is not "
                   "exercising the dispatch\n",
                   checked);
            bad = 1;
         }
         if (bad)
            fail = 1;
         printf("uitest: ui_hit dispatch verified on %d settings targets\n",
                checked);
      }

      /* A DESTRUCTIVE code must not appear on ANY screen but its own.
       *
       * The per-screen count below closes the SCR_FORGET case, but the class is
       * wider: a read-only row on some other screen carrying MA_FORGET_YES
       * fires driver_forget() on the sensor's link the moment it is tapped. Two
       * such mutants (the sensor-detail MAC row, the settings STANDBY row)
       * survived a screen-local check. Scan every screen instead. */
      {
         static const int allscr[] = {
             SCR_MAIN,    SCR_SETTINGS, SCR_SENSOR, SCR_CAL,  SCR_KEYPAD,
             SCR_DEVLIST, SCR_SENSTYPE, SCR_LABEL,  SCR_GATE, SCR_REMOTE};
         struct ANativeWindow_Buffer db = {.width  = W,
                                           .height = TALL_H,
                                           .stride = W,
                                           .format = 1,
                                           .bits   = g_px};
         for (unsigned si = 0; si < sizeof allscr / sizeof allscr[0]; si++) {
            struct hits dh;
            struct screen dm = set;
            dm.scr           = allscr[si];
            dm.sel           = 0;
            ui_render(&db, &dm, &dh);
            for (int i = 0; i < dh.n; i++) {
               if (dh.box[i].kind != ACT_MENU)
                  continue;
               if (dh.box[i].arg == MA_FORGET_YES) {
                  printf("  FAIL: screen %d carries MA_FORGET_YES -- only "
                         "SCR_FORGET may, and tapping it destroys the bond\n",
                         allscr[si]);
                  fail = 1;
                  break;
               }
               if (dh.box[i].arg == MA_INSDEL_YES) {
                  printf("  FAIL: screen %d carries MA_INSDEL_YES -- only "
                         "SCR_INSDEL may, and tapping it deletes the dose\n",
                         allscr[si]);
                  fail = 1;
                  break;
               }
            }
         }
         printf("uitest: destructive code confined to its own screen\n");
      }

      /* A DESTRUCTIVE code must appear exactly once, on the control that means
       * it. Asserting only that MA_FORGET_NO is PRESENT is satisfied by the
       * pre-guard escape hatch at the top of render_forget, so miswiring the
       * CANCEL row to MA_FORGET_YES passed the whole suite -- tapping CANCEL on
       * the delete confirmation would forget the sensor: driver_forget() on its
       * link, the bond destroyed, its DIS cache cleared. */
      {
         struct hits fh;
         struct screen fm                = set;
         fm.scr                          = SCR_FORGET;
         fm.sel                          = 0;
         struct ANativeWindow_Buffer fb2 = {.width  = W,
                                            .height = TALL_H,
                                            .stride = W,
                                            .format = 1,
                                            .bits   = g_px};
         ui_render(&fb2, &fm, &fh);
         int nyes     = 0;
         int nno      = 0;
         int ymin_yes = 1073741824; /* 1<<30: a y past any real coordinate */
         int ymax_no  = -1;
         for (int i = 0; i < fh.n; i++) {
            if (fh.box[i].kind != ACT_MENU)
               continue;
            if (fh.box[i].arg == MA_FORGET_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].arg == MA_FORGET_NO) {
               nno++;
               if (fh.box[i].y > ymax_no)
                  ymax_no = fh.box[i].y;
            }
         }
         if (nyes != 1) {
            printf("  FAIL: SCR_FORGET records %d MA_FORGET_YES targets, want "
                   "exactly 1 -- a second one means a non-destructive control "
                   "carries the destructive code\n",
                   nyes);
            fail = 1;
         }
         if (nno < 1) {
            printf("  FAIL: SCR_FORGET records no MA_FORGET_NO target\n");
            fail = 1;
         }
         /* CANCEL is drawn above FORGET, so the lowest NO must sit above YES.
          */
         if (nyes == 1 && ymax_no > ymin_yes) {
            printf(
                "  FAIL: SCR_FORGET has a CANCEL target BELOW the destructive "
                "one (no=%d yes=%d)\n",
                ymax_no, ymin_yes);
            fail = 1;
         }
         printf("uitest: destructive code appears once, below cancel\n");

         /* The delete-dose confirmation follows SCR_FORGET's discipline:
          * the deleting code (MA_INSDEL_YES reaches insulin_delete) appears
          * exactly once, with the safe CANCEL above it. */
         fm.scr       = SCR_INSDEL;
         fm.ins_t     = now_ts;
         fm.ins_type  = 1;
         fm.ins_units = 12;
         ui_render(&fb2, &fm, &fh);
         nyes     = 0;
         nno      = 0;
         ymin_yes = 1073741824;
         ymax_no  = -1;
         for (int i = 0; i < fh.n; i++) {
            if (fh.box[i].kind != ACT_MENU)
               continue;
            if (fh.box[i].arg == MA_INSDEL_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].arg == MA_INSDEL_NO) {
               nno++;
               if (fh.box[i].y > ymax_no)
                  ymax_no = fh.box[i].y;
            }
         }
         if (nyes != 1) {
            printf("  FAIL: SCR_INSDEL records %d MA_INSDEL_YES targets, want "
                   "exactly 1\n",
                   nyes);
            fail = 1;
         }
         if (nno < 1) {
            printf("  FAIL: SCR_INSDEL records no MA_INSDEL_NO target\n");
            fail = 1;
         }
         if (nyes == 1 && ymax_no > ymin_yes) {
            printf("  FAIL: SCR_INSDEL has a CANCEL target BELOW the deleting "
                   "one (no=%d yes=%d)\n",
                   ymax_no, ymin_yes);
            fail = 1;
         }
         printf("uitest: insdel deletes once, below cancel\n");

         /* The pairing confirmation follows the same discipline: the code
          * that commits (MA_PAIR_YES reaches commit_pair, which registers the
          * device and drops the chosen link's old bond) appears exactly once,
          * with the safe NO above it -- and the screen must render even
          * before any device was proposed (null pair_name/pair_mac). */
         fm.scr       = SCR_PAIRCONF;
         fm.pair_name = 0;
         fm.pair_mac  = 0;
         ui_render(&fb2, &fm, &fh); /* null-safe: must not crash */
         fm.pair_name = "DXCM77";
         fm.pair_mac  = "C1:22:33:44:55:66";
         ui_render(&fb2, &fm, &fh);
         nyes     = 0;
         nno      = 0;
         ymin_yes = 1073741824;
         ymax_no  = -1;
         for (int i = 0; i < fh.n; i++) {
            if (fh.box[i].kind != ACT_MENU)
               continue;
            if (fh.box[i].arg == MA_PAIR_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].arg == MA_PAIR_NO) {
               nno++;
               if (fh.box[i].y > ymax_no)
                  ymax_no = fh.box[i].y;
            }
         }
         if (nyes != 1) {
            printf("  FAIL: SCR_PAIRCONF records %d MA_PAIR_YES targets, want "
                   "exactly 1\n",
                   nyes);
            fail = 1;
         }
         if (nno < 1) {
            printf("  FAIL: SCR_PAIRCONF records no MA_PAIR_NO target\n");
            fail = 1;
         }
         if (nyes == 1 && ymax_no > ymin_yes) {
            printf("  FAIL: SCR_PAIRCONF has a NO target BELOW the committing "
                   "one (no=%d yes=%d)\n",
                   ymax_no, ymin_yes);
            fail = 1;
         }
         printf("uitest: pairconf commits once, below NO\n");

         /* LOG INSULIN: the one writing control (CONFIRM) appears exactly
          * once, and every field the form promises is actually adjustable --
          * units, date and time arrow steppers. (The TYPE toggle is gone:
          * FAST/SLOW is chosen on the ADD menu and fixed in the title.)
          * Each field's UP target must sit ABOVE its DOWN target -- that is
          * the whole promise of the vertical arrow steppers. */
         fm.scr       = SCR_INSULIN;
         fm.ins_t     = now_ts;
         fm.ins_type  = 1;
         fm.ins_units = 12;
         ui_render(&fb2, &fm, &fh);
         {
            /* The three fields (units/date/time) each carry a keypad-entry
             * target, and the one writing control (CONFIRM) appears
             * exactly once -- BELOW discard, the app-wide cancel-on-top
             * rule. */
            int nconf   = 0;
            int ndisc   = 0;
            int seen[3] = {0};
            int yconf   = -1;
            int ydisc   = -1;
            for (int i = 0; i < fh.n; i++) {
               if (fh.box[i].kind != ACT_MENU)
                  continue;
               if (fh.box[i].arg == MA_INS_CONFIRM) {
                  nconf++;
                  yconf = fh.box[i].y;
               }
               if (fh.box[i].arg == MA_INS_DISCARD) {
                  ndisc++;
                  if (ydisc < 0 || fh.box[i].y > ydisc)
                     ydisc = fh.box[i].y; /* the button, not the title X */
               }
               int d = fh.box[i].arg - MA_INS_EDIT;
               if (d >= 0 && d < 3)
                  seen[d] = 1;
            }
            if (nconf != 1) {
               printf("  FAIL: SCR_INSULIN records %d MA_INS_CONFIRM targets, "
                      "want exactly 1\n",
                      nconf);
               fail = 1;
            }
            if (ndisc < 1) {
               printf("  FAIL: SCR_INSULIN records no MA_INS_DISCARD\n");
               fail = 1;
            }
            if (yconf >= 0 && ydisc >= 0 && yconf <= ydisc) {
               printf("  FAIL: SCR_INSULIN CONFIRM (y=%d) is not BELOW "
                      "DISCARD (y=%d)\n",
                      yconf, ydisc);
               fail = 1;
            }
            for (int k = 0; k < 3; k++)
               if (!seen[k]) {
                  printf("  FAIL: SCR_INSULIN field %d has no keypad-entry "
                         "target -- that field cannot be edited\n",
                         k);
                  fail = 1;
               }
            printf("uitest: insulin form carries every control, one CONFIRM\n");
         }

         /* CHOOSE PRIMARY: EVERY registered CGM is offered -- including one
          * just paired with no session and no datapoint yet (promoting it is
          * how the user pre-arms the switch to a new sensor). A meter never
          * appears, however fresh its data. The row's code must index the
          * sensor MODEL (MA_PRIM_PICK + slot), the same discipline as the
          * device pick. */
         {
            struct ui_sensor ps[4] = {sens[0], sens[1], sens[2], sens[0]};
            ps[0].session_seconds  = 3L * 86400; /* live session */
            ps[1].session_seconds  = 0;          /* just paired: no session,
                                                    no data */
            ps[1].last            = 0;
            ps[2].session_seconds = 5; /* BGM: never eligible */
            ps[2].last            = now_ts - 60;
            /* An OLD (disconnected) CGM lives in OLD DEVICES and nowhere
             * else -- however live its session looks, it is not streaming
             * and must never be offered the big number. */
            ps[3].old        = 1;
            struct screen pm = set;
            pm.scr           = SCR_PRIMPICK;
            pm.now           = now_ts;
            pm.sensors       = ps;
            pm.nsensors      = 4;
            ui_render(&fb2, &pm, &fh);
            int rows    = 0;
            int saw_bgm = 0;
            int saw_old = 0;
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].arg >= MA_PRIM_PICK &&
                   fh.box[i].arg < MA_PRIM_PICK + UI_MAX_SLOTS) {
                  rows++;
                  if (fh.box[i].arg == MA_PRIM_PICK + 2)
                     saw_bgm = 1;
                  if (fh.box[i].arg == MA_PRIM_PICK + 3)
                     saw_old = 1;
               }
            if (rows != 2 || saw_bgm || saw_old) {
               printf("  FAIL: PRIMPICK offers %d rows (bgm=%d old=%d); want "
                      "both LIVE CGMs (even the dataless one) and never the "
                      "meter or an old device\n",
                      rows, saw_bgm, saw_old);
               fail = 1;
            }
            printf("uitest: choose-primary lists live CGMs only -- never a "
                   "meter, never an old device\n");
         }

         /* An ARMED (pending) pairing must be visible in DEVICES and
          * cancellable -- an invisible armed state that auto-pairs later
          * would be indistinguishable from the app acting on its own. */
         {
            struct screen sm = set;
            sm.scr           = SCR_SETTINGS;
            sm.pend_type     = SENSOR_G7;
            ui_render(&fb2, &sm, &fh);
            int saw_cancel = 0;
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].arg == MA_PEND_CANCEL)
                  saw_cancel = 1;
            if (!saw_cancel) {
               printf("  FAIL: SETTINGS shows no cancellable PENDING row "
                      "while a pairing is armed\n");
               fail = 1;
            }
            sm.pend_type = 0;
            ui_render(&fb2, &sm, &fh);
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].arg == MA_PEND_CANCEL) {
                  printf("  FAIL: PENDING row rendered with nothing armed\n");
                  fail = 1;
               }
            printf("uitest: armed pairing is visible and cancellable\n");
         }

         /* THE DEVICE PICK MUST INDEX THE MODEL, NOT THE SORTED ROW.
          *
          * render_devlist sorts by RSSI while main.c does
          * commit_pair(g_devs[arg - MA_DEV_PICK].mac) against the UNSORTED
          * array. The existing fixture is {-61,-80} -- already descending, so
          * the sorted position and the model index coincide and a mutant using
          * either passed. With a permuting order the user taps the nearest
          * sensor and the app pairs a different one, running driver_forget() on
          * a live link first. */
         {
            struct ui_dev dv[3];
            for (int i = 0; i < 3; i++)
               dv[i] = devs[0];
            (void)snprintf(dv[0].mac, sizeof dv[0].mac, "AA:00:00:00:00:00");
            (void)snprintf(dv[1].mac, sizeof dv[1].mac, "BB:11:11:11:11:11");
            (void)snprintf(dv[2].mac, sizeof dv[2].mac, "CC:22:22:22:22:22");
            dv[0].rssi =
                -90; /* weakest first, so sorted order != array order */
            dv[1].rssi = -55; /* strongest */
            dv[2].rssi = -70;
            struct hits vh;
            struct screen vm               = set;
            vm.scr                         = SCR_DEVLIST;
            vm.devs                        = dv;
            vm.ndev                        = 3;
            struct ANativeWindow_Buffer vb = {.width  = W,
                                              .height = TALL_H,
                                              .stride = W,
                                              .format = 1,
                                              .bits   = g_px};
            ui_render(&vb, &vm, &vh);
            /* Every device must be pickable exactly once, by its MODEL index.
             */
            int seen[3] = {0, 0, 0};
            for (int i = 0; i < vh.n; i++)
               if (vh.box[i].kind == ACT_MENU && vh.box[i].arg >= MA_DEV_PICK &&
                   vh.box[i].arg < MA_DEV_PICK + 3)
                  seen[vh.box[i].arg - MA_DEV_PICK]++;
            for (int k = 0; k < 3; k++)
               if (seen[k] != 1) {
                  printf("  FAIL: device %d pickable %d times -- the pick "
                         "index does "
                         "not map 1:1 to the model array\n",
                         k, seen[k]);
                  fail = 1;
               }
            /* Uniqueness is not enough: indexing by sorted POSITION also
             * yields each code exactly once, just attached to the wrong
             * device. Rows are drawn strongest-RSSI first, so walking them top
             * to bottom must give the MODEL indices in RSSI order -- here
             * 1 (-55), 2 (-70), 0 (-90). */
            int rows[3];
            int nr = 0;
            for (int i = 0; i < vh.n && nr < 3; i++)
               if (vh.box[i].kind == ACT_MENU && vh.box[i].arg >= MA_DEV_PICK &&
                   vh.box[i].arg < MA_DEV_PICK + 3)
                  rows[nr++] = i;
            for (int a = 0; a < nr; a++)
               for (int b = a + 1; b < nr; b++)
                  if (vh.box[rows[b]].y < vh.box[rows[a]].y) {
                     int t   = rows[a];
                     rows[a] = rows[b];
                     rows[b] = t;
                  }
            static const int want_order[3] = {1, 2, 0};
            if (nr == 3)
               for (int k = 0; k < 3; k++)
                  if (vh.box[rows[k]].arg - MA_DEV_PICK != want_order[k]) {
                     printf("  FAIL: devlist row %d picks model %d, want %d "
                            "-- the pick is indexing the sorted position, so "
                            "the user taps one sensor and the app pairs "
                            "another\n",
                            k, vh.box[rows[k]].arg - MA_DEV_PICK,
                            want_order[k]);
                     fail = 1;
                     break;
                  }
            printf(
                "uitest: device picks index the model, not the sorted row\n");
         }
      }

      /* THE KEY AT EACH GRID POSITION MUST CARRY ITS OWN CODE.
       *
       * Asserting only that the twelve digit codes are all PRESENT is satisfied
       * by any permutation of them, so transposing two keys passed the whole
       * suite. That is a wrong pairing code, and -- via the same keypad -- a
       * wrong CALIBRATION value written to the sensor, which main.c calls the
       * single most consequential write in the app. Positions are checked in
       * reading order: the layout is 7 8 9 / 4 5 6 / 1 2 3 / 0 < OK. */
      {
         struct hits kh2;
         struct screen km2               = set;
         km2.scr                         = SCR_KEYPAD;
         struct ANativeWindow_Buffer kb2 = {.width  = W,
                                            .height = TALL_H,
                                            .stride = W,
                                            .format = 1,
                                            .bits   = g_px};
         ui_render(&kb2, &km2, &kh2);
         static const int want[12] = {107, 108, 109, 104, 105, 106,
                                      101, 102, 103, 100, 110, MA_OK};
         int idx[64];
         int n = 0;
         for (int i = 0; i < kh2.n && n < 64; i++)
            /* Keys only: MA_DIGIT(100..109), MA_BACKSPACE(110) and MA_OK(111).
             * MA_KP_CLOSE(113) is the close band, not part of the grid. */
            if (kh2.box[i].kind == ACT_MENU && kh2.box[i].arg >= 100 &&
                kh2.box[i].arg <= MA_OK)
               idx[n++] = i;
         /* Sort the collected targets into reading order (row, then column). */
         for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++) {
               int ia   = idx[a];
               int ib   = idx[b];
               int rowa = kh2.box[ia].y;
               int rowb = kh2.box[ib].y;
               if (rowb < rowa - 4 ||
                   (rowb < rowa + 4 && kh2.box[ib].x < kh2.box[ia].x)) {
                  int t  = idx[a];
                  idx[a] = idx[b];
                  idx[b] = t;
               }
            }
         if (n != 12) {
            printf("  FAIL: keypad recorded %d key targets, want 12\n", n);
            fail = 1;
         } else {
            for (int k = 0; k < 12; k++)
               if (kh2.box[idx[k]].arg != want[k]) {
                  printf(
                      "  FAIL: keypad position %d carries arg %d, want %d -- "
                      "a transposed key means a wrong calibration value\n",
                      k, kh2.box[idx[k]].arg, want[k]);
                  fail = 1;
                  break;
               }
         }
         printf("uitest: keypad key positions carry their own codes\n");
      }

      /* A target smaller than a fingertip draws normally and cannot be hit. The
       * degenerate check only catches w<=0/h<=0, so a 1-px-tall keypad key
       * passed. */
      {
         struct hits kh;
         struct screen km               = set;
         km.scr                         = SCR_KEYPAD;
         struct ANativeWindow_Buffer kb = {.width  = W,
                                           .height = TALL_H,
                                           .stride = W,
                                           .format = 1,
                                           .bits   = g_px};
         ui_render(&kb, &km, &kh);
         int tiny = 0;
         for (int i = 0; i < kh.n; i++)
            if (kh.box[i].kind == ACT_MENU &&
                (kh.box[i].w < 8 || kh.box[i].h < 8))
               tiny++;
         if (tiny) {
            printf("  FAIL: keypad has %d target(s) under 8 px -- draws but "
                   "cannot realistically be tapped\n",
                   tiny);
            fail = 1;
         }
         printf("uitest: no sub-fingertip keypad targets\n");
      }

      /* THE REMOTE-IP KEYPAD (kp_mode 4) IS ITS OWN GEOMETRY PATH: a 5-row
       * grid with a '.' key and a full-width OK. The 4-row grid once laid out
       * NEGATIVE-height keys in landscape (drawn but untappable), so the new
       * row count must be swept too, and positions checked in reading order --
       * 7 8 9 / 4 5 6 / 1 2 3 / . 0 < / OK -- since a transposed dot types a
       * malformed address the OK then refuses, with no hint why. */
      {
         static const int ipwant[13] = {107, 108, 109,    104, 105, 106,  101,
                                        102, 103, MA_DOT, 100, 110, MA_OK};
         static const int ipgeo[][2] = {
             {720,  1440},
             {1080, 1920},
             {1920, 1080},
             {480,  720 }
         };
         for (unsigned g = 0; g < sizeof ipgeo / sizeof ipgeo[0]; g++) {
            struct hits kh;
            struct screen km               = set;
            km.scr                         = SCR_KEYPAD;
            km.kp_mode                     = 4;
            struct ANativeWindow_Buffer kb = {.width  = ipgeo[g][0],
                                              .height = ipgeo[g][1],
                                              .stride = ipgeo[g][0],
                                              .format = 1,
                                              .bits   = g_px};
            ui_render(&kb, &km, &kh);
            int idx[64];
            int n = 0;
            for (int i = 0; i < kh.n && n < 64; i++)
               if (kh.box[i].kind == ACT_MENU &&
                   ((kh.box[i].arg >= 100 && kh.box[i].arg <= MA_OK) ||
                    kh.box[i].arg == MA_DOT))
                  idx[n++] = i;
            for (int a = 0; a < n; a++)
               for (int b = a + 1; b < n; b++) {
                  int ia   = idx[a];
                  int ib   = idx[b];
                  int rowa = kh.box[ia].y;
                  int rowb = kh.box[ib].y;
                  if (rowb < rowa - 4 ||
                      (rowb < rowa + 4 && kh.box[ib].x < kh.box[ia].x)) {
                     int t  = idx[a];
                     idx[a] = idx[b];
                     idx[b] = t;
                  }
               }
            if (n != 13) {
               printf("  FAIL: IP keypad at %dx%d recorded %d key targets, "
                      "want 13\n",
                      ipgeo[g][0], ipgeo[g][1], n);
               fail = 1;
               continue;
            }
            for (int k = 0; k < 13; k++)
               if (kh.box[idx[k]].arg != ipwant[k] || kh.box[idx[k]].w < 8 ||
                   kh.box[idx[k]].h < 8) {
                  printf("  FAIL: IP keypad at %dx%d position %d carries arg "
                         "%d (%dx%d px), want %d at fingertip size\n",
                         ipgeo[g][0], ipgeo[g][1], k, kh.box[idx[k]].arg,
                         kh.box[idx[k]].w, kh.box[idx[k]].h, ipwant[k]);
                  fail = 1;
                  break;
               }
         }
         printf("uitest: IP keypad grid verified (5 rows, dot key, wide OK)\n");
      }

      /* EVERY MODAL MUST CARRY ITS OWN ESCAPE CODE, and ui_hit must return it.
       *
       * The existing dead-end sweep only checks that SOME box has w>0 and h>0
       * -- it is kind- and arg-agnostic. So mis-wiring an escape control (X
       * becomes ORIENTATION, cancel becomes ACT_NONE, back becomes PRIMARY)
       * left the screen inescapable while the gate stayed green: on_input
       * swallows every non-ACT_MENU tap while g_menu is set, and Android's back
       * button destroys the activity rather than closing a menu, so those are
       * force-stop-only states. Deleting an escape target was already caught;
       * MIS-WIRING one was not. Probing through ui_hit rather than reading
       * h.box[] also catches a later box that shadows the control. */
      {
         static const struct {
            int scr;
            int esc;
            const char *name;
         } esc[] = {
             {SCR_SETTINGS, MA_CLOSE,       "SETTINGS"},
             {SCR_KEYPAD,   MA_KP_CLOSE,    "KEYPAD"  },
             {SCR_DEVLIST,  MA_DEV_CANCEL,  "DEVLIST" },
             {SCR_SENSOR,   MA_SENSOR_BACK, "SENSOR"  },
             {SCR_CAL,      MA_CAL_BACK,    "CAL"     },
             {SCR_FORGET,   MA_FORGET_NO,   "FORGET"  },
             {SCR_SENSTYPE, MA_SENSOR_BACK, "SENSTYPE"},
             {SCR_LABEL,    MA_KP_CLOSE,    "LABEL"   },
             {SCR_PAIRCONF, MA_PAIR_NO,     "PAIRCONF"},
             {SCR_ADDMENU,  MA_CLOSE,       "ADDMENU" },
             {SCR_INSULIN,  MA_INS_DISCARD, "INSULIN" },
             {SCR_INSDEL,   MA_INSDEL_NO,   "INSDEL"  },
             {SCR_ALARM,    MA_ALARM_BACK,  "ALARM"   },
             {SCR_EXPORT,   MA_EXP_BACK,    "EXPORT"  },
             {SCR_PRIMPICK, MA_CLOSE,       "PRIMPICK"},
             {SCR_PERMS,    MA_PERMS_BACK,  "PERMS"   },
             {SCR_OLDDEV,   MA_OLDDEV_BACK, "OLDDEV"  },
             {SCR_RECONF,   MA_RECON_NO,    "RECONF"  },
         };
         struct ANativeWindow_Buffer eb = {.width  = W,
                                           .height = TALL_H,
                                           .stride = W,
                                           .format = 1,
                                           .bits   = g_px};
         for (unsigned e = 0; e < sizeof esc / sizeof esc[0]; e++) {
            struct hits eh;
            struct screen em = set;
            em.scr           = esc[e].scr;
            em.sel           = 0;
            ui_render(&eb, &em, &eh);
            int found = -1;
            for (int i = 0; i < eh.n; i++)
               if (eh.box[i].kind == ACT_MENU && eh.box[i].arg == esc[e].esc &&
                   eh.box[i].w > 0 && eh.box[i].h > 0)
                  found = i;
            if (found < 0) {
               printf("  FAIL: %s records no working escape (want ACT_MENU arg "
                      "%d) -- the screen cannot be left without a force stop\n",
                      esc[e].name, esc[e].esc);
               fail = 1;
               continue;
            }
            /* And a tap at its centre must actually dispatch to it. */
            int cx            = eh.box[found].x + (eh.box[found].w / 2);
            int cy            = eh.box[found].y + (eh.box[found].h / 2);
            struct action got = ui_hit(&eh, cx, cy);
            if (got.kind != ACT_MENU || got.arg != esc[e].esc) {
               printf("  FAIL: %s escape at (%d,%d) dispatches kind=%d arg=%d, "
                      "want ACT_MENU %d -- something shadows the way out\n",
                      esc[e].name, cx, cy, got.kind, got.arg, esc[e].esc);
               fail = 1;
            }
         }
         printf("uitest: every modal carries a dispatchable escape\n");
      }

      printf("uitest: all modal screens escapable with a stale selection\n");
   }

   /* --- out-of-range capping ---
    *
    * A reading above the scale must land EXACTLY on the plot_max gridline (and
    * one below it exactly on the bottom line), never off the plot or on some
    * arbitrary nearby row: an excursion has to stay visible AND sit on a row
    * the axis labels actually explain. Asserted through plot_point_xy so this
    * checks plot_render's own mapping rather than a re-derivation of it. */
   {
      const int px0 = 0;
      const int py0 = 0;
      const int pw  = 100;
      const int ph  = 60;
      plot_set_max(300);
      struct plot_pt at_max = {.t = 0, .glu = 300};
      struct plot_pt over   = {.t = 0, .glu = 900};
      struct plot_pt at_min = {.t = 0, .glu = 50};
      struct plot_pt under  = {.t = 0, .glu = 10};
      int ax                = 0;
      int ay                = 0;
      int ox                = 0;
      int oy                = 0;
      int nx                = 0;
      int ny                = 0;
      int ux                = 0;
      int uy                = 0;
      plot_point_xy(px0, py0, pw, ph, at_max, 0, 1, &ax, &ay);
      plot_point_xy(px0, py0, pw, ph, over, 0, 1, &ox, &oy);
      plot_point_xy(px0, py0, pw, ph, at_min, 0, 1, &nx, &ny);
      plot_point_xy(px0, py0, pw, ph, under, 0, 1, &ux, &uy);
      printf("uitest: cap at_max y=%d over y=%d | at_min y=%d under y=%d\n", ay,
             oy, ny, uy);
      if (ay != oy || ny != uy || oy != py0 + 1) {
         printf(
             "  FAIL: out-of-range readings not capped onto the gridlines\n");
         fail = 1;
      }
      plot_set_max(m.plot_max); /* restore: later cases render real screens */
   }

   /* --- multi-sensor plot styling on the main screen ---
    *
    * Without this the harness only ever rendered SCR_MAIN with nsensors = 0,
    * so the whole per-source styling path -- ui_sensor_color(), the
    * non-primary branch, the orphan branch, and every mark() shape but the dot
    * -- was dead code as far as the offline tests were concerned. */
   {
      struct ui_point mh[8] = {
          {.t   = now_ts - 100,
           .glu = 148,
           .src = 1                                   }, /* primary CGM: value palette */
          {.t   = now_ts - 300,
           .glu = 143,
           .src = 2                                   }, /* 2nd CGM: own colour+square */
          {.t   = now_ts - 600,
           .glu = 205,
           .src = 3                                   }, /* meter: own colour+triangle */
          {.t   = now_ts - 900,
           .glu = 132,
           .src = 99                                  }, /* forgotten sensor: orphan */
          {.t = now_ts - 1200,   .glu = 155, .src = 1 },
          {.t = now_ts - 1500,   .glu = 121, .src = 2 },
          {.t = now_ts - 1800,   .glu = 178, .src = 99},
          {.t   = now_ts - 2100,
           .glu = 160,
           .src = 0                                   }, /* legacy: also value palette */
      };
      struct screen ms = m;
      ms.now           = now_ts;
      ms.t             = now_ts - 100;
      ms.hist          = mh;
      ms.nhist         = 8;
      ms.sensors       = sens;
      ms.nsensors      = 3;
      ui_render(&tall, &ms, &h);
      write_ppm_buf(&tall, "tmp/uitest/main_multi.ppm");
      long c1   = count_color(&tall, ui_sensor_color(1)); /* sens[1] colour */
      long c2   = count_color(&tall, ui_sensor_color(2)); /* sens[2] colour */
      long orph = count_color(&tall, 0xFF8A8AA0);         /* UI_ORPHAN */
      printf(
          "uitest: multi-sensor plot px sensor1=%ld sensor2=%ld orphan=%ld\n",
          c1, c2, orph);
      if (c1 <= 0 || c2 <= 0) {
         printf("  FAIL: non-primary sensors not drawn in their own colour\n");
         fail = 1;
      }
      /* The orphan branch is what stops a forgotten sensor's readings being
       * drawn as the live primary trace -- assert it actually fires. */
      if (orph <= 0) {
         printf("  FAIL: orphaned source not drawn in the muted colour\n");
         fail = 1;
      }
      /* And with no registry at all, nothing may be styled as an orphan:
       * pre-registry logs are legitimately the primary trace. */
      ms.sensors  = 0;
      ms.nsensors = 0;
      ui_render(&tall, &ms, &h);
      if (count_color(&tall, 0xFF8A8AA0) > 0) {
         printf("  FAIL: orphan styling applied with an empty registry\n");
         fail = 1;
      }

      /* Legacy (src 0) points must NOT follow the primary flag. Attributing
       * them to the current primary at display time meant that promoting a
       * freshly paired sensor to primary re-labelled days of another sensor's
       * pre-registry data with the new sensor's colour. Render the same
       * legacy-only history with and without a distinctively coloured primary:
       * the primary's colour count must not change. */
      struct ui_sensor lp   = sens[1]; /* colour 1, distinct */
      lp.primary            = 1;
      struct ui_point lh[1] = {
          {.t = now_ts - 2100, .glu = 160, .src = 0}
      };
      ms.hist     = lh;
      ms.nhist    = 1;
      ms.sensors  = &lp;
      ms.nsensors = 1;
      ui_render(&tall, &ms, &h);
      long with_pt = count_color(&tall, ui_sensor_color(lp.color));
      ms.nhist     = 0;
      ui_render(&tall, &ms, &h);
      long without_pt = count_color(&tall, ui_sensor_color(lp.color));
      printf("uitest: legacy-vs-primary px with=%ld without=%ld\n", with_pt,
             without_pt);
      if (with_pt != without_pt) {
         printf("  FAIL: legacy (src 0) point styled as the current primary\n");
         fail = 1;
      }

      /* AN INSULIN DOSE MUST STILL DRAW WITH THE GLUCOSE HISTORY FULL.
       *
       * The shell appends the doses AFTER the glucose points in the SAME
       * m->hist array (sized NHIST + NINS), but ui.c capped its plot loop at
       * the GLUCOSE figure alone -- so once the history filled, every dose's
       * index sat past the cap and no dose was drawn at all; before that the
       * NEWEST were dropped first, which is how a dose logged minutes ago
       * went missing while older ones still showed. Nothing caught it: every
       * other plot test uses a handful of points, and a full history is the
       * steady state on a real phone, not an edge case.
       *
       * Render a FULL glucose history plus one dose, and require the
       * insulin marker's colour to appear. */
      {
         enum { NG = 5040 };
         static struct ui_point big[NG + 1];
         for (int i = 0; i < NG; i++) {
            long back   = (long)i * 300; /* one CGM cadence per step */
            big[i].t    = now_ts - back;
            big[i].glu  = 120;
            big[i].src  = 0;
            big[i].kind = KIND_CGM;
         }
         /* the dose: newest-first ordering puts it last, like the shell */
         big[NG].t    = now_ts - 600;
         big[NG].glu  = 8; /* units; the renderer pins the y itself */
         big[NG].src  = INS_FAST;
         big[NG].kind = KIND_INS;

         struct screen is = m;
         is.now           = now_ts;
         is.t             = now_ts - 100;
         is.hist          = big;
         is.nhist         = NG + 1;
         is.sensors       = 0;
         is.nsensors      = 0;
         is.plot_hours    = 24;
         /* a distinctive, drawable insulin styling */
         is.ins_marker[INS_FAST] = MARK_SQUARE_F;
         is.ins_color[INS_FAST]  = 3;
         is.ins_size[INS_FAST]   = MARK_SIZE_DEF;
         ui_render(&tall, &is, &h);
         long ins_px = count_color(&tall, ui_sensor_color(3));
         printf("uitest: insulin px with a FULL history = %ld\n", ins_px);
         if (ins_px <= 0) {
            printf("  FAIL: an insulin dose vanishes once the glucose "
                   "history fills -- the plot cap must cover the doses the "
                   "shell appends after the readings\n");
            fail = 1;
         }
      }
   }

   /* --- per-sensor screen: attributes settable, actions present --- */
   struct screen det = set;
   det.scr           = SCR_SENSOR;
   det.sel           = 0;
   ui_render(&tall, &det, &h);
   write_ppm_buf(&tall, "tmp/uitest/sensor.ppm");
   {
      /* Marker shape, COLOUR and SIZE were combined into ONE picker opened from
       * the MARKER row, so the per-sensor screen no longer has a separate COLOR
       * target -- MA_MARKER is the single affordance for all three. */
      int saw_primary = 0;
      int saw_marker  = 0;
      int saw_cal     = 0;
      int saw_forget  = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_primary = saw_primary || (h.box[i].arg == MA_PRIMARY);
         saw_marker  = saw_marker || (h.box[i].arg == MA_MARKER);
         saw_cal     = saw_cal || (h.box[i].arg == MA_CAL_OPEN);
         saw_forget  = saw_forget || (h.box[i].arg == MA_FORGET);
      }
      if (!(saw_primary && saw_marker && saw_cal && saw_forget)) {
         printf("  FAIL: sensor screen targets pri=%d mark=%d cal=%d "
                "forget=%d\n",
                saw_primary, saw_marker, saw_cal, saw_forget);
         fail = 1;
      }
   }
   /* a BGM must offer SYNC NOW and must NOT offer calibration or PRIMARY */
   det.sel = 2;
   ui_render(&tall, &det, &h);
   {
      int saw_sync    = 0;
      int saw_cal     = 0;
      int saw_primary = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_sync    = saw_sync || (h.box[i].arg == MA_SYNC);
         saw_cal     = saw_cal || (h.box[i].arg == MA_CAL_OPEN);
         saw_primary = saw_primary || (h.box[i].arg == MA_PRIMARY);
      }
      if (!saw_sync || saw_cal || saw_primary) {
         printf("  FAIL: meter screen sync=%d cal=%d primary=%d "
                "(want 1 0 0)\n",
                saw_sync, saw_cal, saw_primary);
         fail = 1;
      }
   }

   /* --- forget confirmation: reachable, and offers both ways out --- */
   struct screen fg = set;
   fg.scr           = SCR_FORGET;
   fg.sel           = 0;
   ui_render(&tall, &fg, &h);
   write_ppm_buf(&tall, "tmp/uitest/forget.ppm");
   {
      int saw_yes = 0;
      int saw_no  = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_yes = saw_yes || (h.box[i].arg == MA_FORGET_YES);
         saw_no  = saw_no || (h.box[i].arg == MA_FORGET_NO);
      }
      if (!saw_yes || !saw_no) {
         printf("  FAIL: forget confirm yes=%d no=%d\n", saw_yes, saw_no);
         fail = 1;
      }
   }

   /* --- meter discovery: the picker must list a OneTouch, not just Dexcoms ---
    */
   /* ui_dev.name is 12 bytes and the shell truncates into it (main.c uses
    * str_snapshot / snprintf), so this literal must fit WITH its terminator.
    * The real advert "OneTouch C0HD" is 13 chars and, written out in full
    * here, left the array unterminated -- the renderer then read past it. */
   struct ui_dev meters[1] = {
       {"OneTouch C0", "F7:F0:20:2D:77:28", -58}
   };
   struct screen ml = {.scr = SCR_DEVLIST, .devs = meters, .ndev = 1};
   ui_render(&tall, &ml, &h);
   write_ppm_buf(&tall, "tmp/uitest/meterlist.ppm");
   {
      int picks = 0;
      for (int i = 0; i < h.n; i++)
         picks += (h.box[i].kind == ACT_MENU && h.box[i].arg >= MA_DEV_PICK);
      if (picks != 1) {
         printf("  FAIL: meter picker recorded %d picks, want 1\n", picks);
         fail = 1;
      }
   }

   /* --- rename keypad: every character reachable, plus DEL and OK --- */
   struct screen lb = set;
   lb.scr           = SCR_LABEL;
   lb.sel           = 0;
   lb.entry         = "KITCH";
   ui_render(&tall, &lb, &h);
   write_ppm_buf(&tall, "tmp/uitest/label.ppm");
   {
      int chars   = 0;
      int saw_del = 0;
      int saw_ok  = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         if (h.box[i].arg >= MA_CHAR &&
             h.box[i].arg < MA_CHAR + ui_label_nchars())
            chars++;
         saw_del = saw_del || (h.box[i].arg == MA_BACKSPACE);
         saw_ok  = saw_ok || (h.box[i].arg == 111);
      }
      if (chars != ui_label_nchars() || !saw_del || !saw_ok) {
         printf("  FAIL: rename keypad chars=%d/%d del=%d ok=%d\n", chars,
                ui_label_nchars(), saw_del, saw_ok);
         fail = 1;
      }
   }

   /* --- calibration CONFIRMATION screen: the value keypad's OK lands here and
    * it always offers CONFIRM + CANCEL. Permission is no longer a UI gate: an
    * unsupported sensor is handled by the durable calibration queue (which
    * surfaces NOT SUPPORTED), so SCR_CAL just confirms the typed value. */
   struct screen cal = set;
   cal.scr           = SCR_CAL;
   cal.sel           = 0;
   cal.cal_pending = 140; /* the value typed on the keypad, awaiting CONFIRM */
   ui_render(&tall, &cal, &h);
   write_ppm_buf(&tall, "tmp/uitest/cal.ppm");
   {
      int saw_enter  = 0;
      int saw_cancel = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_enter  = saw_enter || (h.box[i].arg == MA_CAL_ENTER);
         saw_cancel = saw_cancel || (h.box[i].arg == MA_CAL_BACK);
      }
      if (!saw_enter || !saw_cancel) {
         printf("  FAIL: cal confirm screen enter=%d cancel=%d\n", saw_enter,
                saw_cancel);
         fail = 1;
      }
   }

   /* --- sensor-type picker offers every type --- */
   struct screen st2 = {.scr = SCR_SENSTYPE, .sel = -1};
   ui_render(&tall, &st2, &h);
   write_ppm_buf(&tall, "tmp/uitest/senstype.ppm");
   {
      int types = 0;
      for (int i = 0; i < h.n; i++)
         if (h.box[i].kind == ACT_MENU && h.box[i].arg >= MA_TYPE &&
             h.box[i].arg < MA_TYPE + SENSOR_NTYPES)
            types++;
      if (types != SENSOR_NTYPES - 1) {
         printf("  FAIL: type picker offered %d types, want %d\n", types,
                SENSOR_NTYPES - 1);
         fail = 1;
      }
   }

   /* --- first-run gate screen (CONTINUE button) --- */
   struct screen gate = {.scr = SCR_GATE};
   ui_render(&g_buf, &gate, &h);
   write_ppm("tmp/uitest/gate.ppm");
   int saw_cont = 0;
   for (int i = 0; i < h.n; i++)
      saw_cont = saw_cont || (h.box[i].kind == ACT_GATE_CONTINUE);
   if (!saw_cont) {
      printf("  FAIL: gate recorded no ACT_GATE_CONTINUE target\n");
      fail = 1;
   }

   printf("uitest: %s\n", fail ? "FAIL" : "OK");
   return fail;
}
