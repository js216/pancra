// SPDX-License-Identifier: GPL-3.0
// uitest.c --- Offline UI harness: render the pure core to PPM + check hit-test
// Copyright 2026 Jakob Kastelic

/* The UI is a pure function of a `struct screen`, so it runs with no phone:
 * fill a model, call ui_render into a plain framebuffer, dump a PPM (into the
 * build tree the run was given, never the source tree -- see
 * app/test/testdir.h), and assert that ui_hit maps a tap to the right action.
 * As each screen is ported this harness grows a case per screen. Built and run
 * by `make uitest`. */
#include "exercise.h"
#include "ui.h"
#include "insulin.h" /* INS_FAST: the dose the full-history plot test logs */
#include "keypad.h"  /* KP_NMODES: every mode the keypad defines */
#include "ndk.h"
#include "plot.h" /* the capping case asserts plot_render's own mapping */
#include "sensors.h"
#include "style.h"   /* MARK_SIZE_DEF: the marker sizes a frame carries */
#include "testdir.h" /* test_path: the per-mode directory the screens land in */
#include "uiact.h"   /* MA_* / struct hits: the taps this feeds back */
#include "uidraw.h"  /* add_hit/add_hit_ix/add_glow: the append this gates */
#include "uifmt.h"   /* the scale arithmetic and the exported tables */
#include "uikeypad.h"
#include "uimodel.h" /* SCR_* and the frame it builds by hand */
#include "uipriv.h"  /* ui_clip_*: the clip counters this asserts on */
#include "weight.h"  /* struct wt_rec / NWT for the sweep fixture */
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

/* THE SCREEN'S NAME, NOT ITS PATH. Seventeen call sites used to spell out
 * "build/app/test/<name>.ppm", so the ASan build of this suite (it is in
 * APPASAN_TESTS) overwrote the plain build's screens while the plain build was
 * writing them. The directory is decided in ONE place -- here -- and comes from
 * test_dir(). */
static void write_ppm_buf(const struct ANativeWindow_Buffer *b,
                          const char *name)
{
   char pbuf[192];
   const char *path = test_path(pbuf, sizeof pbuf, name);
   FILE *f          = fopen(path, "wb");
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

static void write_ppm(const char *name)
{
   char pbuf[192];
   const char *path = test_path(pbuf, sizeof pbuf, name);
   FILE *f          = fopen(path, "wb");
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
/* A plain boolean check, for the pure colour rules below: the ck() used
 * elsewhere in this file is about rendered buffers. */
static int fail;
static void ck_col(int cond, const char *what)
{
   if (!cond) {
      printf("  FAIL: %s\n", what);
      fail = 1;
   } else {
      printf("uitest: %s\n", what);
   }
}

static long count_color(const struct ANativeWindow_Buffer *b, uint32_t want)
{
   long n = 0;
   for (int i = 0; i < b->width * b->height; i++)
      if (g_px[i] == want)
         n++;
   return n;
}

/* The topmost row holding `want`, or -1 if it is nowhere on the buffer.
 *
 * A COUNT CANNOT ANSWER "WHERE", and for the out-of-range banner where it is
 * drawn is now the whole claim: it has to sit above the big number and below
 * the Android status bar, and a banner that satisfies neither still colours
 * exactly as many pixels as one that satisfies both. */
static int first_row_of_color(const struct ANativeWindow_Buffer *b,
                              uint32_t want)
{
   for (int y = 0; y < b->height; y++)
      for (int x = 0; x < b->width; x++)
         if (g_px[(y * b->stride) + x] == want)
            return y;
   return -1;
}

/* The BOTTOM row holding `want`, which is what "above" actually needs.
 *
 * Comparing first rows only says which element STARTS higher, and two elements
 * can start in that order while overlapping for most of their height. Measured:
 * with the banner's band left unclamped in landscape it began 2*sc below the
 * top of the content and the big number began at 4*sc, so the banner started
 * above the number -- and ran five units INTO it. A first-row test called that
 * correct. */
static int last_row_of_color(const struct ANativeWindow_Buffer *b,
                             uint32_t want)
{
   for (int y = b->height - 1; y >= 0; y--)
      for (int x = 0; x < b->width; x++)
         if (g_px[(y * b->stride) + x] == want)
            return y;
   return -1;
}

/* The longest horizontal run of ONE non-background colour anywhere on the
 * buffer.
 *
 * A rule is the only thing this app draws that is wide and uniform: glyph
 * strokes are a few pixels, a framed button's edge is one row but is broken by
 * the background inside it, and a filled bar belongs to the main screen. So
 * "no run wider than a fraction of the screen" is a statement about horizontal
 * rules that does not have to name the rule's colour -- which matters, because
 * the next rule somebody adds will not be 0xFF444444. */
static int longest_hrun(const struct ANativeWindow_Buffer *b, uint32_t bg)
{
   int best = 0;
   for (int y = 0; y < b->height; y++) {
      int run       = 0;
      uint32_t last = bg;
      for (int x = 0; x < b->width; x++) {
         uint32_t c = g_px[(y * b->stride) + x];
         if (c != bg && c == last) {
            run++;
         } else {
            run = (c != bg) ? 1 : 0;
         }
         last = c;
         if (run > best)
            best = run;
      }
   }
   return best;
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

/* As above, bounded on the right as well, so a landscape assertion can be
 * about ONE column rather than everything to its right. */
static long count_color_box(const struct ANativeWindow_Buffer *b, uint32_t want,
                            int x0, int y0, int x1)
{
   long n = 0;
   if (x1 > b->width)
      x1 = b->width;
   for (int y = y0; y < b->height; y++)
      for (int x = x0; x < x1; x++)
         if (g_px[(y * b->stride) + x] == want)
            n++;
   return n;
}

/* Where render_info lives, which depends on orientation: the lower half in
 * portrait, and the LOWER LEFT column in landscape -- the stats table sits
 * under the big number now, with the plot and threshold rows in the right
 * column. It used to be the whole right column, and this helper has to follow
 * that move or the assertion silently tests the wrong pixels (which is the
 * failure its own comment records). Bounded on the right so a stray CCCCCC in
 * the plot column cannot stand in for the block under test. */
static long count_info_block(const struct ANativeWindow_Buffer *b,
                             uint32_t want)
{
   int landscape = b->width > b->height;
   return landscape ? count_color_box(b, want, 0, b->height / 2, b->width / 2)
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

/* The trace colour the interleaved case renders with; the palette is the
 * caller's, and this one has no opinion. */
static uint32_t ip_white(int glu)
{
   (void)glu;
   return 0xFFFFFFFF;
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
       .scr                     = SCR_MAIN,
       .now                     = 1000,
       .reading.glu             = 148,
       .reading.trend           = 2,
       .reading.t               = 900,
       .reading.rssi            = -72,
       .reading.rssi_ok         = 1,
       .plot.hist               = hist,
       .plot.nhist              = 4,
       .plot.scrub              = -1,
       .plot.plot_hours         = 24,
       .plot.plot_max           = 300,
       .reading.bonded          = 1,
       .reading.have_reading    = 1,
       .reading.predicted       = 152,
       .reading.sequence        = 41,
       .reading.session_seconds = 3L * 86400,
       .dev.stored              = 812,
       .prefs.units             = 0,
       .prefs.alarm_low         = 100,
       .prefs.alarm_high        = 300,
       /* Set, not left zero: a zero pair renders as "OFF"/"OFF", the SHORTEST
        * the nudge row can be, so the sweep would measure the easy case of a
        * row whose whole reason for existing is to carry two more numbers.
        * Widest realistic values instead. */
       .prefs.nudge_low   = 130,
       .prefs.nudge_high  = 220,
       .prefs.nudge_sound = 1,
       .prefs.nudge_vib   = 1,
       .status            = "CONNECTED",
       .plot.stat         = {s, s, s, s, s},
   };

   int fail = 0;

   /* ---- A DROPPED TARGET MUST NOT REDECORATE THE ONE BEFORE IT ----
    *
    * add_hit_ix wrote box[n-1].code after calling add_hit, and add_hit leaves
    * n exactly where it was when the table is full -- so on overflow n-1 is
    * the LAST CONTROL THAT DID FIT: drawn, on screen, tappable. The dropped
    * control's code landed on it, and BACK started dispatching FORGET
    * DEVICE. Nothing about that is visible in `n` or in `overflow` (the old
    * code moved neither either), so every case below reads the SURVIVING BOX
    * back by index rather than counting rows. */
   {
      /* Distinct y bands so a tap resolves to one named box, and so the
       * table below is filled by real appends and not by memset. */
      struct hits h = {0};
      for (int i = 0; i < UI_MAX_HITS; i++) {
         int got = add_hit_ix(&h, 0, i * 20, 100, 20, MA_DEVICES_BACK, i);
         if (got != i) {
            printf("  FAIL: add_hit_ix slot %d reported %d\n", i, got);
            fail = 1;
         }
         /* The append that SUCCEEDS still has to decorate what it appended --
          * read back that index, not the count. */
         if (h.box[i].code != MA_DEVICES_BACK || h.box[i].arg != i) {
            printf("  FAIL: appended slot %d not decorated (code %d arg %d)\n",
                   i, h.box[i].code, h.box[i].arg);
            fail = 1;
         }
      }
      if (h.n != UI_MAX_HITS || h.overflow) {
         printf("  FAIL: %d clean appends gave n=%d overflow=%d\n", UI_MAX_HITS,
                h.n, h.overflow);
         fail = 1;
      }

      const int last    = UI_MAX_HITS - 1;
      const int wascode = h.box[last].code;
      const int wasarg  = h.box[last].arg;
      int drop          = add_hit_ix(&h, 0, 9000, 100, 20, MA_FORGET_YES, 777);
      if (drop != UI_HIT_DROPPED) {
         printf("  FAIL: append past the table reported slot %d\n", drop);
         fail = 1;
      }
      /* THE DEFECT ITSELF. */
      if (h.box[last].code != wascode || h.box[last].arg != wasarg) {
         printf("  FAIL: dropped target overwrote box %d: code %d->%d "
                "arg %d->%d\n",
                last, wascode, h.box[last].code, wasarg, h.box[last].arg);
         fail = 1;
      }
      /* ...and what the user would feel: a tap on the last surviving control
       * still dispatches that control. */
      struct action a = ui_hit(&h, 50, (last * 20) + 10);
      if (a.kind != ACT_MENU || a.code != wascode || a.arg != wasarg) {
         printf("  FAIL: tap on the last fitting control dispatches "
                "(%d,%d,%d)\n",
                a.kind, a.code, a.arg);
         fail = 1;
      }
      /* The drop stays LOUD -- the layout-did-not-fit gate below reads this
       * flag, so a fix that silenced it would trade one silent failure for
       * another. */
      if (!h.overflow || h.n != UI_MAX_HITS) {
         printf("  FAIL: drop left overflow=%d n=%d\n", h.overflow, h.n);
         fail = 1;
      }
      printf("uitest: a dropped hit box leaves box %d's action alone\n", last);
   }

   /* ---- THE SAME FOR THE GLOW RECT ----
    *
    * add_glow narrowed box[n-1] too, so the settings band's glyph-sized
    * highlight would have been stapled onto whatever control happened to be
    * last -- pressing it would light pixels belonging to nothing near it. */
   {
      struct hits h = {0};
      for (int i = 0; i < UI_MAX_HITS; i++)
         (void)add_hit(&h, 0, i * 20, 100, 20, ACT_MENU, i);
      const int last  = UI_MAX_HITS - 1;
      const int wasgx = h.box[last].gx;
      const int wasgy = h.box[last].gy;
      const int wasgw = h.box[last].gw;
      const int wasgh = h.box[last].gh;
      int slot        = add_hit(&h, 0, 9000, 100, 20, ACT_OPEN_SETTINGS, 0);
      if (slot != UI_HIT_DROPPED) {
         printf("  FAIL: add_hit past the table reported slot %d\n", slot);
         fail = 1;
      }
      add_glow(&h, slot, 5, 5, 7, 7);
      if (h.box[last].gx != wasgx || h.box[last].gy != wasgy ||
          h.box[last].gw != wasgw || h.box[last].gh != wasgh) {
         printf("  FAIL: dropped target's glow landed on box %d: "
                "%d,%d,%d,%d\n",
                last, h.box[last].gx, h.box[last].gy, h.box[last].gw,
                h.box[last].gh);
         fail = 1;
      }
      printf("uitest: a dropped glow leaves box %d's highlight alone\n", last);
   }

   /* ---- AND A GLOW THAT DOES FIT STILL NARROWS ITS OWN BOX ---- */
   {
      struct hits h = {0};
      (void)add_hit(&h, 0, 0, 100, 20, ACT_MENU, 0);
      int slot = add_hit(&h, 0, 40, 300, 60, ACT_OPEN_SETTINGS, 0);
      if (slot != 1) {
         printf("  FAIL: second append reported slot %d\n", slot);
         fail = 1;
      }
      add_glow(&h, slot, 11, 44, 13, 15);
      if (h.box[slot].gx != 11 || h.box[slot].gy != 44 ||
          h.box[slot].gw != 13 || h.box[slot].gh != 15) {
         printf("  FAIL: glow did not narrow its own box: %d,%d,%d,%d\n",
                h.box[slot].gx, h.box[slot].gy, h.box[slot].gw, h.box[slot].gh);
         fail = 1;
      }
      /* ...and only its own: box 0 keeps the default rect add_hit gave it. */
      if (h.box[0].gx != 0 || h.box[0].gy != 0 || h.box[0].gw != 100 ||
          h.box[0].gh != 20) {
         printf("  FAIL: glow reached back to box 0: %d,%d,%d,%d\n",
                h.box[0].gx, h.box[0].gy, h.box[0].gw, h.box[0].gh);
         fail = 1;
      }
      printf("uitest: a glow narrows the box its append returned\n");
   }

   /* --- portrait main screen --- */
   struct hits h;
   ui_render(&g_buf, &m, &h);
   write_ppm("main.ppm");
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
         /* The band is bounded by its COLUMN, so in landscape its centre must
          * still resolve to it -- a full-width band reached into the plot
          * column, where the later tab targets took the pixels back and left a
          * control that looked present and was not. */
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
      saw_low   = saw_low || (k == ACT_MENU && h.box[i].code == MA_ALARM_LOW);
      saw_high  = saw_high || (k == ACT_MENU && h.box[i].code == MA_ALARM_HIGH);
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
   struct screen scan = {.scr           = SCR_MAIN,
                         .reading.glu   = -1,
                         .status        = "SCANNING",
                         .dev.adv_total = 137,
                         .dev.devs      = devs,
                         .dev.ndev      = 2};
   ui_render(&g_buf, &scan, &h);
   write_ppm("scan.ppm");
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
   struct screen set   = m;
   set.scr             = SCR_SETTINGS;
   set.plot.plot_max   = 300;
   set.prefs.sound_on  = 1;
   set.prefs.screen_on = 1;
   set.sys.perm[0] = set.sys.perm[1] = set.sys.perm[2] = 1;
   set.sys.batt_ok                                     = 1;
   set.sys.standby_bucket                              = 10;
   set.dev.code                                        = "1234";
   set.dev.mac                                         = "F8:DA:11:22:33:44";
   set.dev.model                                       = "SW11163";
   set.dev.fw                                          = "1.6.5.15";
   ui_render(&g_buf, &set, &h);
   write_ppm("settings.ppm");
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
   struct screen kp = {
       .scr = SCR_KEYPAD, .entry.kp_mode = KP_PAIR_CODE, .entry.entry = "12"};
   ui_render(&g_buf, &kp, &h);
   write_ppm("keypad.ppm");
   int keys = 0;
   for (int i = 0; i < h.n; i++)
      keys += (h.box[i].kind == ACT_MENU);
   if (keys < 12) { /* 12 keys + close band */
      printf("  FAIL: keypad recorded %d ACT_MENU targets, want >=12\n", keys);
      fail = 1;
   }

   /* --- device list (a pick per scanned sensor) --- */
   struct screen dl = {.scr = SCR_DEVLIST, .dev.devs = devs, .dev.ndev = 2};
   ui_render(&g_buf, &dl, &h);
   write_ppm("devlist.ppm");
   int picks = 0;
   for (int i = 0; i < h.n; i++)
      picks += (h.box[i].kind == ACT_MENU && h.box[i].code == MA_DEV_PICK);
   if (picks != 2) {
      printf("  FAIL: device list recorded %d picks, want 2\n", picks);
      fail = 1;
   }

   /* --- device list on the DEVICES screen (space check + rows) --- */
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
   set.dev.sensors   = sens;
   set.dev.nsensors  = 3;
   set.dev.sel       = -1;
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
   /* The registry lives on its OWN screen now, opened from the main screen's
    * big number and from the SETTINGS row -- not inlined into settings. */
   struct screen devs_scr = set;
   devs_scr.scr           = SCR_DEVICES;
   ui_render(&tall, &devs_scr, &h);
   write_ppm_buf(&tall, "devices_tall.ppm");
   {
      int saw[3] = {0, 0, 0};
      for (int i = 0; i < h.n; i++)
         for (int k = 0; k < 3; k++)
            if (h.box[i].kind == ACT_MENU &&
                (h.box[i].code == MA_SENSOR && h.box[i].arg == k))
               saw[k] = 1;
      if (!(saw[0] && saw[1] && saw[2])) {
         printf("  FAIL: device rows not all tappable (%d %d %d)\n", saw[0],
                saw[1], saw[2]);
         fail = 1;
      }
      int saw_add = 0;
      for (int i = 0; i < h.n; i++)
         saw_add = saw_add ||
                   (h.box[i].kind == ACT_MENU && h.box[i].code == MA_ADDSENSOR);
      if (!saw_add) {
         printf("  FAIL: no ADD NEW SENSOR target\n");
         fail = 1;
      }
   }

   /* --- layout must fit, and essential controls must be REACHABLE ---
    *
    * Two failure modes, both shipped before: (a) the scale was derived from
    * WIDTH while the pitch it produced was spent on HEIGHT, so capacity fell
    * below the minimum on 16:9/18:9 phones and the renderer drew no device
    * rows and no ADD button at all; (b) ui_devices_scale and ui_sensor_capacity
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
          /* TALL AND NARROW. The list stopped at 720 wide, so the paginated
           * log screens -- whose row count scales with height while the hit
           * budget does not -- were never laid out anywhere the two could
           * disagree. These are freeform/split-screen windows an app with no
           * scrolling has to survive, and where the overflow reproduces. */
          {480,  1920},
          {540,  2340},
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
         int ssc  = ui_devices_scale(sw, sh);
         int scap = ui_sensor_capacity(sw, sh);
         int slh  = 16 * ssc;
         /* UI_DEV_ABOVE, not a literal: a stale copy of the overhead here is
          * exactly the drift the dense sweep below exists to catch. */
         int need = (sh / 20) + (8 * ssc) + ((UI_DEV_ABOVE + scap + 1) * slh);
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
            int ssc  = ui_devices_scale(sw, sh);
            int scap = ui_sensor_capacity(sw, sh);
            int need =
                (sh / 20) + (8 * ssc) + ((UI_DEV_ABOVE + scap + 1) * 16 * ssc);
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
      /* FULL LOGS, for the same reason the sensor fields below are filled:
       * the paginated log screens size their row count from the window and
       * their hit boxes from the rows, so with the empty tables this sweep
       * used to pass, SCR_INSLOG and SCR_WTLOG were laid out with nothing in
       * them and their worst case was never rendered at any geometry. */
      static struct ins_rec sweep_ins[NINS];
      static struct wt_rec sweep_wt[NWT];
      for (int z = 0; z < NINS; z++) {
         sweep_ins[z].t     = 1700000000L + ((long)z * 3600);
         sweep_ins[z].type  = z % 2;
         sweep_ins[z].units = 10 + (z % 90);
      }
      for (int z = 0; z < NWT; z++) {
         sweep_wt[z].t = 1700000000L + ((long)z * 86400);
         sweep_wt[z].g = 70000 + ((long)z * 10);
      }
      set.ins.ins_log     = sweep_ins;
      set.ins.ins_nlog    = NINS;
      set.wt.wt           = sweep_wt;
      set.wt.nwt          = NWT;
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
         /* EVERY screen, from the enum -- not a hand-kept list.
          *
          * The list this replaces held 26 of 33, and the seven it omitted
          * were the paginated log and entry screens (INSLOG, WTLOG,
          * INSULIN, WEIGHT, WTDEL, PERMS, NOREADING) -- which size their
          * rows from window height with no cap tied to UI_MAX_HITS, so on
          * tall-narrow geometries add_hit silently drops the trailing
          * targets INCLUDING the next-page arrow: drawn, and dead to touch.
          * The hits.overflow gate claims to cover "every screen and
          * geometry" and did not, because a list that must be updated by
          * hand is exactly how a new screen escapes the sweep. Deriving it
          * from SCR_N cannot drift. */
         static int scrs[SCR_N];
         for (int z = 0; z < SCR_N; z++)
            scrs[z] = z;
         nscr = SCR_N;
         for (int c = 0; c < SCR_N; c++) {
            struct screen rr = set;
            rr.scr           = scrs[c];
            /* SCR_CAL was only ever swept with cal_have == 0, which renders a
             * short early-return stub -- the full panel (bounds, last result,
             * ENTER VALUE) was never laid out at any swept geometry. */
            rr.cal.cal_have      = 1;
            rr.cal.cal_permitted = 1;
            rr.cal.cal_status    = 2;
            rr.cal.cal_last_bg   = 142;
            rr.cal.cal_result    = 0;
            rr.now               = now_ts;
            rr.reading.t         = now_ts - 100;
            /* A FULL sensor list, not one row.
             *
             * This swept nsensors = 1, so the DEVICES screen -- the only
             * screen whose height grows with content -- was never laid out at
             * its worst case at any of these geometries. A mutation adding
             * three rows to render_devices without bumping UI_DEV_ABOVE
             * passed the entire gate, while a full list clipped 8368 glyph
             * cells and put hit boxes 52 px below a 1080x1920 screen. With no
             * scrolling those rows and their tap targets are permanently
             * unreachable. The list is capped to what the geometry claims it
             * can show, so this asserts the screen honours its OWN promise. */
            /* ...and the list must be as long as the geometry CLAIMS it can
             * show, plus the optional rows. `min(cap, 3)` left every screen
             * with cap > 3 short of its own worst case, and none of the three
             * conditional rows was ever drawn, so UI_DEV_ABOVE could
             * under-count by three and the gate still passed while the
             * trailing button sat 130 px below the bottom edge. The worst
             * case is: the
             * full list, one device RETIRED (the "OLD DEVICES (n)" row plus
             * the blank line either side of it), more live devices than fit
             * (the page-nav row), and a pairing armed (the "PENDING..." row).
             */
            int cap_here = ui_sensor_capacity(sw, sh);
            static struct ui_sensor full[UI_MAX_SLOTS];
            int nfull = cap_here + 2; /* > one page: forces the page-nav row */
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
               /* An UNSETTLED bond draws render_sensor's PAIRING row, which is
                * one more optional row in that screen's budget. Left at 0 the
                * row never appeared, so the budget could be a row short and
                * the sweep would still pass -- exactly the gap the note above
                * describes for the other five optional rows. */
               full[q].bond = UI_BOND_BONDING;
               snprintf(full[q].serial, sizeof full[q].serial, "SN1234567");
               snprintf(full[q].code, sizeof full[q].code, "7381");
            }
            full[nfull - 1].old = 1; /* the OLD DEVICES row */
            rr.dev.sensors      = full;
            rr.dev.nsensors     = nfull;
            /* SIX shortcuts: SC_MAX, and the worst case for the main
             * screen's pinned block in both axes at once -- two rows instead
             * of one, and three buttons per row, where each button is
             * narrowest and the '+' has least space left beside it.
             *
             * It said THREE while SC_MAX was three, and the comment was right
             * then for the same reason it would be wrong now: a sweep that
             * pins fewer than the maximum never lays out the row that the
             * maximum requires. The second row is 28 units of budget that
             * simply would not be spent, so every geometry would pass while
             * the real worst case ran off the bottom of the screen. Pin
             * SC_MAX of them, always, and let the compiler complain here if
             * SC_MAX ever grows again. */
            /* SC_* IDENTITIES, NOT MA_* CODES -- which is what prefs holds.
             *
             * This fixture set MA_INS_FAST / MA_INS_SLOW / MA_WT_OPEN, and
             * every one of them made ui_shortcut_slot_by_id return -1: the
             * pins are stored by identity (settings.h, enum shortcut_id, 1..5)
             * precisely so the renderer's numbering can change, and an MA_*
             * code is >= 21 by construction so the two can never collide.
             * The layout loop skipped all three and drew NOTHING, so the row
             * this block claimed to be the worst case for was never laid out
             * at all -- for as long as the fixture has existed.
             *
             * All six ids ui_sc_tab offers, which is SC_MAX and forces the
             * full two-row path at three per row. It was five while FOOD was
             * not yet pinnable; the rows were 3 + 2 then, which exercised the
             * split but never a FULL second row. */
            rr.prefs.shortcut[0] = SC_INS_FAST;
            rr.prefs.shortcut[1] = SC_INS_SLOW;
            rr.prefs.shortcut[2] = SC_WEIGHT;
            rr.prefs.shortcut[3] = SC_INSLOG;
            rr.prefs.shortcut[4] = SC_WTLOG;
            rr.prefs.shortcut[5] = SC_FOOD;
            rr.dev.pend_type     = 1; /* the PENDING... row */
            rr.dev.sel           = 0;
            rr.dev.devs          = devs;
            rr.dev.ndev          = 2;
            rr.entry.entry       = "1234";
            ui_clip_reset();
            ui_render(&rb, &rr, &h);
            /* THE HIT-BOX BUDGET MUST NOT OVERFLOW.
             *
             * add_hit drops any target past UI_MAX_HITS, and a dropped one
             * draws perfectly while being dead to touch -- indistinguishable
             * from a control that just does not work, with nothing logged.
             * The margin is thinner than it looks: SCR_LABEL peaks at 41 of
             * 48, so seven more controls on that screen is the ceiling. This
             * is the gate that turns "silently untappable" into a build
             * failure. */
            if (h.overflow) {
               printf("  FAIL: scr %d at %dx%d: hit-box budget overflowed "
                      "(UI_MAX_HITS=%d)\n",
                      scrs[c], sw, sh, UI_MAX_HITS);
               fail = 1;
            }
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
         struct screen rm           = m;
         rm.now                     = now_ts;
         rm.reading.t               = now_ts - 100;
         rm.reading.session_seconds = 16L * 86400;
         rm.dev.sensors             = &rs;
         rm.dev.nsensors            = 1;
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
                                  h.box[k].code == MA_ADD_OPEN);
         if (!saw_add) {
            printf("  FAIL: %dx%d: main screen has no '+' ADD target\n", sw,
                   sh);
            fail = 1;
         }
         /* The NO-READING main screen, across geometries. It is what a fresh
          * install shows -- the state in which the app has to be usable enough
          * to pair a sensor -- and it was only ever rendered at one size. */
         struct screen nr = {.scr           = SCR_MAIN,
                             .reading.glu   = -1,
                             .now           = now_ts,
                             .status        = "SCANNING",
                             .dev.adv_total = 137,
                             .dev.devs      = devs,
                             .dev.ndev      = 2};
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
         struct screen ub  = m;
         ub.now            = now_ts;
         ub.reading.t      = now_ts - 100;
         ub.reading.bonded = 0;
         ub.status         = "METER: REGISTER FAILED";
         ub.dev.adv_total  = 1482137;
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
         mm.reading.t     = now_ts - 100;
         mm.prefs.units   = 1;
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
         lo.reading.t     = now_ts - 100;
         lo.reading.glu =
             45; /* below 50: the big number turns red, so a shared
                  * colour would make the banner check meaningless */
         lo.prefs.alarm_low         = 100;
         lo.reading.session_seconds = 3L * 86400;
         ui_clip_reset();
         ui_render(&rb, &lo, &h);
         if (ui_clipped() > 0) {
            printf("  FAIL: %dx%d low-main: %ld glyph cells clipped\n", sw, sh,
                   ui_clipped());
            fail = 1;
         }
         /* 0xFF2020E0 is the LOW banner's own colour -- see uimain.c. Using
          * glu_color's red here was vacuous: the big number uses it too. */
         /* HIGH and STALE get the same treatment as LOW: each banner has a
          * colour no other element draws, so "visible" means the banner. */
         struct screen hi    = lo;
         hi.reading.glu      = 350;
         hi.prefs.alarm_high = 300;
         hi.prefs.alarm_low  = 100;
         ui_render(&rb, &hi, &h);
         if (count_color(&rb, 0xFF20A0FF) <= 0) {
            printf("  FAIL: %dx%d: HIGH banner not visible on screen\n", sw,
                   sh);
            fail = 1;
         }
         struct screen st2        = lo;
         st2.reading.disc_alarmed = 1;
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
         /* WHERE THE BANNER IS, not merely that it is somewhere.
          *
          * It used to be drawn in big letters at the BOTTOM of the column and
          * is now one normal-size row in the band ABOVE the big number, which
          * is a claim about position that every check above is blind to -- a
          * count of the banner's colour is identical whether it sits above the
          * number, below it, or over the top of the system clock.
          *
          * Two bounds, and the upper one is the reason the item asked for
          * care: the Android status bar owns the top of the window (the clock
          * and the notification icons are drawn there by the system, over
          * whatever this app puts underneath), and ui_render clears it by
          * starting the screen at height/20. A banner placed by subtracting
          * from the number's position rather than adding to the top of the
          * content would drift up into that strip on some geometry and be
          * unreadable there, with nothing in this suite to say so.
          *
          * glu is 45 here, so the big number is glu_color's red (0xFF0000FF)
          * while the banner is 0xFF2020E0 -- a colour nothing else draws.
          * Sharing one colour is what made an earlier version of this
          * assertion vacuous for five review rounds. */
         {
            /* has_cgm ON for this one render, and only this one. Without a
             * live CGM the big number is the grey "---" placeholder, not
             * glu_color's red -- so the number this assertion is positioned
             * against would not be on the buffer at all, and the check would
             * fail for a reason that has nothing to do with where the banner
             * went. The banner itself does not depend on has_cgm; it is
             * gated on the reading's freshness, which `lo` already sets. */
            struct screen pos  = lo;
            pos.reading.has_cgm = 1;
            pos.reading.stale   = 0;
            ui_render(&rb, &pos, &h);
            int bany = first_row_of_color(&rb, 0xFF2020E0);
            int banb = last_row_of_color(&rb, 0xFF2020E0);
            int numy = first_row_of_color(&rb, 0xFF0000FF);
            if (bany < sh / 20) {
               printf("  FAIL: %dx%d: LOW banner at y=%d intrudes into the "
                      "system status bar (clears at y=%d)\n",
                      sw, sh, bany, sh / 20);
               fail = 1;
            }
            /* The banner's LAST row against the number's FIRST: clear of it,
             * not merely starting sooner. Comparing the two first rows passes
             * for a banner that starts above the number and then runs down
             * through it, which is exactly what happens when the band is not
             * widened for landscape's narrower padding. */
            if (numy < 0 || banb >= numy) {
               printf("  FAIL: %dx%d: LOW banner (y=%d..%d) is not clear of "
                      "the big number at y=%d\n",
                      sw, sh, bany, banb, numy);
               fail = 1;
            }
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

   /* --- every keypad mode must have its own title ---
    *
    * The title chain used to end in `else -> pairing keypad`, so a mode with
    * no title silently rendered PAIR NEW <sensor> -- a real screen for a
    * different feature. The WEIGHT keypad shipped exactly that way: tapping a
    * weight opened the sensor-pairing flow and nothing looked wrong. The
    * fallback is now a red "BAD KP MODE n", so this checks that no mode
    * reaches it -- i.e. that the title table has an entry for every mode the
    * slot table admits. */
   {
      int w   = 720;
      int hgt = 1600;
      int bad = 0;
      /* EVERY mode keypad.h defines, so a mode added there without a row in
       * the table shows up here as the red fallback rather than as a screen
       * that names some other feature. */
      struct ANativeWindow_Buffer kb = {
          .width = w, .height = hgt, .stride = w, .format = 1, .bits = g_px};
      for (int mode = 0; mode < KP_NMODES; mode++) {
         struct hits kh;
         struct screen km = set;
         km.scr           = SCR_KEYPAD;
         km.entry.kp_mode = mode;
         km.entry.entry   = "8";
         km.dev.add_type  = "STELO";
         for (long q = 0; q < (long)w * hgt; q++)
            g_px[q] = 0;
         ui_render(&kb, &km, &kh);
         /* the fallback's colour, which nothing else on this screen uses */
         long red = 0;
         for (long q = 0; q < (long)w * hgt; q++)
            if (g_px[q] == 0xFF4466FF)
               red++;
         if (red > 0) {
            printf("  FAIL: keypad mode %d has no title (BAD KP MODE)\n", mode);
            bad  = 1;
            fail = 1;
         }
      }
      if (!bad)
         printf("uitest: all %d keypad modes carry a title\n", KP_NMODES);
   }

   /* --- the keypad's metadata is the whole protocol ---
    *
    * The modes were bare integers with their behaviour spelled out at each
    * use site: `>= 10 && <= 13` in the renderer, `>= 12` and `% 2` in the
    * shell, `6 + ix` at the two forms. Every one of those is a silent
    * statement about the enum's ORDER, and one of them was already wrong (the
    * dot-key range, which left a dead '.' on both NUDGE keypads). The table
    * answers instead -- so this asserts the answers, which is what the call
    * sites now depend on. */
   {
      int bad = 0;
      /* Exactly four thresholds, and each knows its pair and its end. */
      int nthresh = 0;
      for (int m = 0; m < KP_NMODES; m++)
         if (kp_is_thresh(m))
            nthresh++;
      if (nthresh != 4)
         bad = 1;
      if (!kp_is_low(KP_ALARM_LOW) || kp_is_low(KP_ALARM_HIGH) ||
          !kp_is_low(KP_NUDGE_LOW) || kp_is_low(KP_NUDGE_HIGH))
         bad = 1;
      if (kp_is_nudge(KP_ALARM_LOW) || kp_is_nudge(KP_ALARM_HIGH) ||
          !kp_is_nudge(KP_NUDGE_LOW) || !kp_is_nudge(KP_NUDGE_HIGH))
         bad = 1;
      /* A '.' where the value has one: mmol/L thresholds, and a weight
       * always. NOT on a pairing code, a port or a dose. */
      if (kp_has_dot(KP_ALARM_LOW, 0) || !kp_has_dot(KP_ALARM_LOW, 1) ||
          !kp_has_dot(KP_NUDGE_HIGH, 1) || !kp_has_dot(KP_WEIGHT, 0) ||
          kp_has_dot(KP_PAIR_CODE, 1) || kp_has_dot(KP_PORT, 1) ||
          kp_has_dot(KP_INS_UNITS, 1))
         bad = 1;
      /* The forms' rows, which used to be KP_INS_UNITS + ix at two sites. */
      if (kp_ins_field(0) != KP_INS_UNITS || kp_ins_field(1) != KP_DATE ||
          kp_ins_field(2) != KP_TIME || kp_ins_field(3) != KP_YEAR ||
          kp_ins_field(4) != KP_NONE || kp_ins_field(-1) != KP_NONE)
         bad = 1;
      /* THE WEIGHT FORM'S OWN calendar modes, not the insulin form's: which
       * instant a field edits is a property of the mode, and while the two
       * shared modes the commit had to read the RETURN SCREEN to tell them
       * apart. */
      if (kp_weight_field(0) != KP_WEIGHT || kp_weight_field(1) != KP_WT_DATE ||
          kp_weight_field(2) != KP_WT_TIME ||
          kp_weight_field(3) != KP_WT_YEAR || kp_weight_field(9) != KP_NONE)
         bad = 1;
      /* The LOG FOOD form's own fields, for the same reason. */
      if (kp_food_field(0) != KP_FOOD_G || kp_food_field(1) != KP_FOOD_TIME ||
          kp_food_field(2) != KP_FOOD_DATE ||
          kp_food_field(3) != KP_FOOD_YEAR || kp_food_field(9) != KP_NONE)
         bad = 1;
      /* WHICH FORM EACH MODE BELONGS TO, as a three-way answer.
       *
       * This was a boolean, kp_edits_weight, and both call sites read it as
       * `edits_weight ? &weight : &insulin` -- correct while there were two
       * forms, and silently wrong the moment there was a third: every LOG
       * FOOD date would have moved the INSULIN form's instant, because "not
       * weight" defaulted to insulin. Asserted here in the direction that
       * catches it, i.e. that the food modes answer FOOD and not merely
       * "not weight". */
      if (kp_form_of(KP_WEIGHT) != KP_FORM_WEIGHT ||
          kp_form_of(KP_WT_DATE) != KP_FORM_WEIGHT ||
          kp_form_of(KP_WT_YEAR) != KP_FORM_WEIGHT ||
          kp_form_of(KP_DATE) != KP_FORM_INSULIN ||
          kp_form_of(KP_YEAR) != KP_FORM_INSULIN ||
          kp_form_of(KP_INS_UNITS) != KP_FORM_INSULIN ||
          kp_form_of(KP_FOOD_G) != KP_FORM_FOOD ||
          kp_form_of(KP_FOOD_DATE) != KP_FORM_FOOD ||
          kp_form_of(KP_FOOD_TIME) != KP_FORM_FOOD ||
          kp_form_of(KP_FOOD_YEAR) != KP_FORM_FOOD)
         bad = 1;
      /* A mode that is not a form field says so, rather than defaulting into
       * somebody's timestamp -- which is what made the old boolean's third
       * case invisible. */
      if (kp_form_of(KP_PAIR_CODE) != KP_FORM_NONE ||
          kp_form_of(KP_SYNC_CODE) != KP_FORM_NONE ||
          kp_form_of(KP_CALIB) != KP_FORM_NONE ||
          kp_form_of(KP_NONE) != KP_FORM_NONE)
         bad = 1;
      /* An unknown mode draws NOTHING -- not the pairing keypad, which is
       * what the renderer's old fallback made it. */
      if (kp_slots(KP_NMODES) != 0 || kp_info(KP_NONE)->title != 0 ||
          kp_slots(-99) != 0)
         bad = 1;
      /* ...and every real mode has room for what it collects. */
      for (int m = 0; m < KP_NMODES; m++)
         if (kp_slots(m) < 2)
            bad = 1;
      if (bad) {
         printf("  FAIL: the keypad table disagrees with what the call "
                "sites ask it\n");
         fail = 1;
      } else {
         printf("uitest: every keypad mode answers for its own behaviour\n");
      }
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
      wm.dev.sensors      = &ws;
      wm.dev.nsensors     = 1;
      wm.dev.sel          = 0;
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
         bad.dev.sel       = -1; /* stale selection */
         bad.dev.nsensors  = 0;
         bad.dev.sensors   = 0;
         bad.dev.ndev      = 0;
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
            dm.dev.sel       = 0;
            ui_render(&db, &dm, &dh);
            for (int i = 0; i < dh.n; i++) {
               if (dh.box[i].kind != ACT_MENU)
                  continue;
               if (dh.box[i].code == MA_FORGET_YES) {
                  printf("  FAIL: screen %d carries MA_FORGET_YES -- only "
                         "SCR_FORGET may, and tapping it destroys the bond\n",
                         allscr[si]);
                  fail = 1;
                  break;
               }
               if (dh.box[i].code == MA_INSDEL_YES) {
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
         fm.dev.sel                      = 0;
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
            if (fh.box[i].code == MA_FORGET_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].code == MA_FORGET_NO) {
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
         fm.scr           = SCR_INSDEL;
         fm.ins.ins_t     = now_ts;
         fm.ins.ins_type  = 1;
         fm.ins.ins_units = 12;
         ui_render(&fb2, &fm, &fh);
         nyes     = 0;
         nno      = 0;
         ymin_yes = 1073741824;
         ymax_no  = -1;
         for (int i = 0; i < fh.n; i++) {
            if (fh.box[i].kind != ACT_MENU)
               continue;
            if (fh.box[i].code == MA_INSDEL_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].code == MA_INSDEL_NO) {
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
         fm.scr           = SCR_PAIRCONF;
         fm.dev.pair_name = 0;
         fm.dev.pair_mac  = 0;
         ui_render(&fb2, &fm, &fh); /* null-safe: must not crash */
         fm.dev.pair_name = "DXCM77";
         fm.dev.pair_mac  = "C1:22:33:44:55:66";
         ui_render(&fb2, &fm, &fh);
         nyes     = 0;
         nno      = 0;
         ymin_yes = 1073741824;
         ymax_no  = -1;
         for (int i = 0; i < fh.n; i++) {
            if (fh.box[i].kind != ACT_MENU)
               continue;
            if (fh.box[i].code == MA_PAIR_YES) {
               nyes++;
               if (fh.box[i].y < ymin_yes)
                  ymin_yes = fh.box[i].y;
            }
            if (fh.box[i].code == MA_PAIR_NO) {
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
         fm.scr           = SCR_INSULIN;
         fm.ins.ins_t     = now_ts;
         fm.ins.ins_type  = 1;
         fm.ins.ins_units = 12;
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
               if (fh.box[i].code == MA_INS_CONFIRM) {
                  nconf++;
                  yconf = fh.box[i].y;
               }
               if (fh.box[i].code == MA_INS_DISCARD) {
                  ndisc++;
                  if (ydisc < 0 || fh.box[i].y > ydisc)
                     ydisc = fh.box[i].y; /* the button, not the title X */
               }
               int d = fh.box[i].arg;
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

         /* LOG WEIGHT mirrors the insulin form: cancel on TOP, the one
          * writing control (CONFIRM) exactly once on the BOTTOM, and in
          * EDIT mode (the worst case) DELETE between the two. */
         fm.scr          = SCR_WEIGHT;
         fm.wt.wt_edit   = 1;
         fm.wt.wt_t      = now_ts;
         fm.wt.wt_tenths = 1542;
         ui_render(&fb2, &fm, &fh);
         {
            int nconf = 0;
            int ydisc = -1;
            int ydel  = -1;
            int yconf = -1;
            for (int i = 0; i < fh.n; i++) {
               if (fh.box[i].kind != ACT_MENU)
                  continue;
               if (fh.box[i].code == MA_WT_CONFIRM) {
                  nconf++;
                  yconf = fh.box[i].y;
               }
               if (fh.box[i].code == MA_WT_DISCARD && fh.box[i].y > ydisc)
                  ydisc = fh.box[i].y; /* the button, not the title X */
               if (fh.box[i].code == MA_WT_DELETE)
                  ydel = fh.box[i].y;
            }
            if (nconf != 1) {
               printf("  FAIL: SCR_WEIGHT records %d MA_WT_CONFIRM targets, "
                      "want exactly 1\n",
                      nconf);
               fail = 1;
            }
            if (ydisc < 0) {
               printf("  FAIL: SCR_WEIGHT records no MA_WT_DISCARD\n");
               fail = 1;
            }
            if (ydel < 0) {
               printf("  FAIL: EDIT WEIGHT records no MA_WT_DELETE\n");
               fail = 1;
            }
            if (yconf >= 0 && (yconf <= ydisc || yconf <= ydel)) {
               printf("  FAIL: SCR_WEIGHT CONFIRM (y=%d) is not the BOTTOM "
                      "button (cancel y=%d, delete y=%d)\n",
                      yconf, ydisc, ydel);
               fail = 1;
            }
            if (ydel >= 0 && ydel <= ydisc) {
               printf("  FAIL: EDIT WEIGHT DELETE (y=%d) is not below "
                      "CANCEL (y=%d)\n",
                      ydel, ydisc);
               fail = 1;
            }
            printf("uitest: weight form commits once, cancel on top\n");
            fm.wt.wt_edit = 0; /* don't leak EDIT mode into later blocks */
         }

         /* THE PRIMARY COLUMN on the DEVICES screen: a checkbox for every
          * LIVE, UNEXPIRED CGM -- including one just paired with no session
          * and no datapoint yet, since promoting it is how the user pre-arms
          * the switch to a new sensor. A meter never gets one, however fresh
          * its data; neither does an old (disconnected) or an expired sensor,
          * because sensor_set_primary refuses all three and a control that
          * cannot work is worse than no control. The box's code must index
          * the sensor MODEL (MA_PRIM_PICK + slot), the same discipline as the
          * device pick. */
         {
            struct ui_sensor ps[5] = {sens[0], sens[1], sens[2], sens[0],
                                      sens[0]};
            ps[0].session_seconds  = 3L * 86400; /* live session */
            ps[1].session_seconds  = 0;          /* just paired: no session,
                                                    no data */
            ps[1].last            = 0;
            ps[2].session_seconds = 5; /* BGM: never eligible */
            ps[2].last            = now_ts - 60;
            /* An OLD (disconnected) CGM lives in OLD DEVICES and nowhere
             * else -- however live its session looks, it is not streaming
             * and must never be offered the big number. */
            ps[3].old = 1;
            /* An EXPIRED one: past its wear budget AND its grace window. */
            ps[4].wear_len        = 10L * 86400;
            ps[4].session_seconds = 11L * 86400;
            struct screen pm      = set;
            pm.scr                = SCR_DEVICES;
            pm.now                = now_ts;
            pm.dev.sensors        = ps;
            pm.dev.nsensors       = 5;
            ui_render(&fb2, &pm, &fh);
            int boxes   = 0;
            int saw_bgm = 0;
            int saw_old = 0;
            int saw_exp = 0;
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].code == MA_PRIM_PICK) {
                  boxes++;
                  if ((fh.box[i].code == MA_PRIM_PICK && fh.box[i].arg == 2))
                     saw_bgm = 1;
                  if ((fh.box[i].code == MA_PRIM_PICK && fh.box[i].arg == 3))
                     saw_old = 1;
                  if ((fh.box[i].code == MA_PRIM_PICK && fh.box[i].arg == 4))
                     saw_exp = 1;
               }
            if (boxes != 2 || saw_bgm || saw_old || saw_exp) {
               printf("  FAIL: DEVICES offers %d PRIMARY boxes (bgm=%d old=%d "
                      "expired=%d); want both LIVE CGMs (even the dataless "
                      "one) and never the meter, the old or the expired one\n",
                      boxes, saw_bgm, saw_old, saw_exp);
               fail = 1;
            }
            /* The checkbox must WIN its own rectangle: the row underneath it
             * carries MA_SENSOR + slot across the full width, so if the box
             * were recorded first (or omitted) a tap on it would open the
             * device menu instead of switching the primary. */
            int cbi = -1;
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   (fh.box[i].code == MA_PRIM_PICK && fh.box[i].arg == 0))
                  cbi = i;
            if (cbi >= 0) {
               struct action cb =
                   ui_hit(&fh, fh.box[cbi].x + (fh.box[cbi].w / 2),
                          fh.box[cbi].y + (fh.box[cbi].h / 2));
               if (cb.kind != ACT_MENU || cb.code != MA_PRIM_PICK + 0) {
                  printf("  FAIL: PRIMARY checkbox is shadowed by its row "
                         "(hit gave kind %d arg %d)\n",
                         cb.kind, cb.arg);
                  fail = 1;
               }
            }
            printf("uitest: PRIMARY column offers live unexpired CGMs only, "
                   "and its box outranks the row\n");
         }

         /* An ARMED (pending) pairing must be visible in DEVICES and
          * cancellable -- an invisible armed state that auto-pairs later
          * would be indistinguishable from the app acting on its own. */
         {
            struct screen sm = set;
            sm.scr           = SCR_DEVICES;
            sm.dev.pend_type = SENSOR_G7;
            ui_render(&fb2, &sm, &fh);
            int saw_cancel = 0;
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].code == MA_PEND_CANCEL)
                  saw_cancel = 1;
            if (!saw_cancel) {
               printf("  FAIL: DEVICES shows no cancellable PENDING row "
                      "while a pairing is armed\n");
               fail = 1;
            }
            sm.dev.pend_type = 0;
            ui_render(&fb2, &sm, &fh);
            for (int i = 0; i < fh.n; i++)
               if (fh.box[i].kind == ACT_MENU &&
                   fh.box[i].code == MA_PEND_CANCEL) {
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
            vm.dev.devs                    = dv;
            vm.dev.ndev                    = 3;
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
               if (vh.box[i].kind == ACT_MENU && vh.box[i].code == MA_DEV_PICK)
                  seen[vh.box[i].arg]++;
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
               if (vh.box[i].kind == ACT_MENU && vh.box[i].code == MA_DEV_PICK)
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
                  if (vh.box[rows[k]].arg != want_order[k]) {
                     printf("  FAIL: devlist row %d picks model %d, want %d "
                            "-- the pick is indexing the sorted position, so "
                            "the user taps one sensor and the app pairs "
                            "another\n",
                            k, vh.box[rows[k]].arg, want_order[k]);
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
         /* (code, index) pairs. This used to be the bare run
          * {107,108,109,...}, which is the base+index namespace written out
          * by hand: nothing in "107" said "digit seven", and the table went
          * on agreeing with the renderer only by both being edited together. */
         static const int want[12] = {
             MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_DIGIT,     MA_DIGIT,
             MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_BACKSPACE, MA_OK};
         static const int want_ix[12] = {7, 8, 9, 4, 5, 6, 1, 2, 3, 0, 0, 0};
         int idx[64];
         int n = 0;
         for (int i = 0; i < kh2.n && n < 64; i++)
            /* Keys only: the digits, DEL and OK. The close band
             * (MA_KP_CLOSE) is not part of the grid. */
            if (kh2.box[i].kind == ACT_MENU &&
                (kh2.box[i].code == MA_DIGIT ||
                 kh2.box[i].code == MA_BACKSPACE || kh2.box[i].code == MA_OK ||
                 kh2.box[i].code == MA_DOT))
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
               if (kh2.box[idx[k]].code != want[k] ||
                   kh2.box[idx[k]].arg != want_ix[k]) {
                  printf(
                      "  FAIL: keypad position %d carries (%d,%d), want "
                      "(%d,%d) -- a transposed key means a wrong calibration "
                      "value\n",
                      k, kh2.box[idx[k]].code, kh2.box[idx[k]].arg, want[k],
                      want_ix[k]);
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

      /* The remote-IP keypad (KP_SERVER) used to be swept here: a 5-row
       * numeric grid with a '.' key, because a server was an address. A
       * server is a NAME now, typed on the letter keypad, and kp_mode 4 is a
       * dead slot -- so that sweep tested a screen no one can reach. Its
       * successor is checked with the other letter-keypad fields below. */

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
             {SCR_SETTINGS, MA_CLOSE,        "SETTINGS"},
             {SCR_KEYPAD,   MA_KP_CLOSE,     "KEYPAD"  },
             {SCR_DEVLIST,  MA_DEV_CANCEL,   "DEVLIST" },
             {SCR_SENSOR,   MA_SENSOR_BACK,  "SENSOR"  },
             {SCR_CAL,      MA_CAL_BACK,     "CAL"     },
             {SCR_FORGET,   MA_FORGET_NO,    "FORGET"  },
             {SCR_SENSTYPE, MA_SENSOR_BACK,  "SENSTYPE"},
             {SCR_LABEL,    MA_KP_CLOSE,     "LABEL"   },
             {SCR_PAIRCONF, MA_PAIR_NO,      "PAIRCONF"},
             {SCR_ADDMENU,  MA_CLOSE,        "ADDMENU" },
             {SCR_INSULIN,  MA_INS_DISCARD,  "INSULIN" },
             {SCR_INSDEL,   MA_INSDEL_NO,    "INSDEL"  },
             {SCR_ALARM,    MA_ALARM_BACK,   "ALARM"   },
             {SCR_EXPORT,   MA_EXP_BACK,     "EXPORT"  },
             {SCR_DEVICES,  MA_DEVICES_BACK, "DEVICES" },
             {SCR_PERMS,    MA_PERMS_BACK,   "PERMS"   },
             {SCR_OLDDEV,   MA_OLDDEV_BACK,  "OLDDEV"  },
             {SCR_RECONF,   MA_RECON_NO,     "RECONF"  },
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
            em.dev.sel       = 0;
            ui_render(&eb, &em, &eh);
            int found = -1;
            for (int i = 0; i < eh.n; i++)
               if (eh.box[i].kind == ACT_MENU && eh.box[i].code == esc[e].esc &&
                   eh.box[i].w > 0 && eh.box[i].h > 0)
                  found = i;
            if (found < 0) {
               printf(
                   "  FAIL: %s records no working escape (want ACT_MENU code "
                   "%d) -- the screen cannot be left without a force stop\n",
                   esc[e].name, esc[e].esc);
               fail = 1;
               continue;
            }
            /* And a tap at its centre must actually dispatch to it. */
            int cx            = eh.box[found].x + (eh.box[found].w / 2);
            int cy            = eh.box[found].y + (eh.box[found].h / 2);
            struct action got = ui_hit(&eh, cx, cy);
            if (got.kind != ACT_MENU || got.code != esc[e].esc) {
               printf("  FAIL: %s escape at (%d,%d) dispatches kind=%d arg=%d, "
                      "want ACT_MENU %d -- something shadows the way out\n",
                      esc[e].name, cx, cy, got.kind, got.code, esc[e].esc);
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
      /* THE SCALE IS AN ARGUMENT NOW, not a process global something else
       * set: two plots with different maxima exist at once (the app's 3 h
       * and the server's day), and the hit test used to answer against
       * whichever was drawn last. */
      struct plot_cfg pc300 = {300, 2};
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
      plot_point_xy((struct plot_rect){px0, py0, pw, ph}, at_max, 0, 1, pc300,
                    &ax, &ay);
      plot_point_xy((struct plot_rect){px0, py0, pw, ph}, over, 0, 1, pc300,
                    &ox, &oy);
      plot_point_xy((struct plot_rect){px0, py0, pw, ph}, at_min, 0, 1, pc300,
                    &nx, &ny);
      plot_point_xy((struct plot_rect){px0, py0, pw, ph}, under, 0, 1, pc300,
                    &ux, &uy);
      printf("uitest: cap at_max y=%d over y=%d | at_min y=%d under y=%d\n", ay,
             oy, ny, uy);
      if (ay != oy || ny != uy || oy != py0 + 1) {
         printf(
             "  FAIL: out-of-range readings not capped onto the gridlines\n");
         fail = 1;
      }
      /* NOTHING TO RESTORE: the scale travelled with the two calls above
       * rather than being installed process-wide, which is the whole point
       * of the change. */
   }

   /* --- TWO PLOTS AT ONCE, WITH DIFFERENT SETTINGS ---
    *
    * The scale and the marker radius were process globals: plot_set_max
    * stored one, and plot_render stored the margin it derived from the other
    * for plot_hit to read back. So a hit test answered against whichever plot
    * was drawn LAST -- and the app has a 3 h trace and a 30 d one a tap
    * apart, while the server renders several windows at once.
    *
    * Interleaved on purpose: A, then B, then A again. If either input were
    * still shared, the second A would answer like B. */
   {
      const int pw        = 120;
      const int ph        = 80;
      struct plot_cfg a   = {300, 2};  /* scale 300, small marker */
      struct plot_cfg b   = {150, 12}; /* half the scale, a fat marker */
      struct plot_pt p1   = {.t = 0, .glu = 150};
      int ax1             = 0;
      int ay1             = 0;
      int bx              = 0;
      int by              = 0;
      int ax2             = 0;
      int ay2             = 0;
      struct plot_rect rc = {0, 0, pw, ph};
      plot_point_xy(rc, p1, 0, 1, a, &ax1, &ay1);
      plot_point_xy(rc, p1, 0, 1, b, &bx, &by);
      plot_point_xy(rc, p1, 0, 1, a, &ax2, &ay2);
      /* 150 is mid-scale at 300 and the TOP at 150, so the two rows must
       * differ -- and A's answer must not move because B was asked in
       * between. */
      if (ay1 == by) {
         printf("  FAIL: two scales map 150 mg/dL to the same row\n");
         fail = 1;
      }
      if (ax1 != ax2 || ay1 != ay2) {
         printf("  FAIL: a plot's mapping changed because ANOTHER plot was "
                "drawn (%d,%d then %d,%d)\n",
                ax1, ay1, ax2, ay2);
         fail = 1;
      }
      /* The x mapping too: the margin comes from the radius, and a fat
       * marker reserves more of the width. Same point, same window. */
      if (ax1 == bx) {
         printf("  FAIL: two radii reserve the same margin\n");
         fail = 1;
      }
      /* ...and the hit test uses the configuration it is GIVEN. Asked with
       * A, a tap at A's own newest column finds the newest point -- and the
       * SAME point sits in a different column under B, which is the only
       * thing the two configurations disagree about. Comparing the two
       * columns is what fails if the margin is shared again. */
      struct plot_pt pts2[2] = {
          {.t = 0,     .glu = 150},
          {.t = -1200, .glu = 150}
      };
      int hit_a = plot_hit(rc, pts2, 2, 0, 1, a, ax1, ph / 2, 0);
      int bx2   = 0;
      int by2   = 0;
      plot_point_xy(rc, pts2[0], 0, 1, b, &bx2, &by2);
      if (hit_a != 0) {
         printf("  FAIL: the hit test did not resolve A's newest point "
                "(%d)\n",
                hit_a);
         fail = 1;
      }
      if (bx2 == ax1) {
         printf("  FAIL: both configurations put the newest point in column "
                "%d, so this case proves nothing\n",
                ax1);
         fail = 1;
      }

      /* AND A RENDER IN BETWEEN, so the clip rectangle and the split scratch
       * are exercised rather than merely being call-local by inspection: A
       * drawn, B drawn, then A's mapping asked for again. */
      static uint32_t ipx[120 * 80];
      struct plot_fb ifb = {ipx, pw, pw, ph};
      plot_render(ifb, rc, pts2, 2, 0, 1, a, ip_white, -1, 0, 0);
      plot_render(ifb, rc, pts2, 2, 0, 1, b, ip_white, -1, 0, 0);
      int ax3 = 0;
      int ay3 = 0;
      plot_point_xy(rc, p1, 0, 1, a, &ax3, &ay3);
      if (ax3 != ax1 || ay3 != ay1) {
         printf("  FAIL: a RENDER with another configuration moved this "
                "plot's mapping (%d,%d -> %d,%d)\n",
                ax1, ay1, ax3, ay3);
         fail = 1;
      }
      /* ...AND THE SCALE REALLY COMES FROM THE MODEL. settingstest used to
       * assert that a loaded plot maximum was pushed into the renderer's
       * global; there is no global now, so the equivalent claim is that
       * ui_render passes the model's value through and records it with the
       * scrub target. Without this, a renderer that quietly used
       * PLOT_GLU_MAX everywhere would pass every other case. */
      {
         struct screen sm = set;
         struct hits sh;
         sm.scr           = SCR_MAIN;
         sm.plot.plot_max = 150; /* NOT the default */
         ui_render(&g_buf, &sm, &sh);
         if (sh.plot.glu_max != 150) {
            printf("  FAIL: the render did not carry the model's plot scale "
                   "(%d)\n",
                   sh.plot.glu_max);
            fail = 1;
         }
         if (sh.plot.radius < 1) {
            printf("  FAIL: ...nor the radius it drew with (%d)\n",
                   sh.plot.radius);
            fail = 1;
         }
      }

      if (!fail)
         printf("uitest: two plots with different scales and radii do not "
                "interfere\n");
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
      ms.reading.t     = now_ts - 100;
      ms.plot.hist     = mh;
      ms.plot.nhist    = 8;
      ms.dev.sensors   = sens;
      ms.dev.nsensors  = 3;
      ui_render(&tall, &ms, &h);
      write_ppm_buf(&tall, "main_multi.ppm");
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
      ms.dev.sensors  = 0;
      ms.dev.nsensors = 0;
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
      ms.plot.hist    = lh;
      ms.plot.nhist   = 1;
      ms.dev.sensors  = &lp;
      ms.dev.nsensors = 1;
      ui_render(&tall, &ms, &h);
      long with_pt  = count_color(&tall, ui_sensor_color(lp.color));
      ms.plot.nhist = 0;
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
       * m->hist array (sized NHIST + NINS), but the renderer capped its plot
       * loop at the GLUCOSE figure alone -- so once the history filled, every
       * dose's index sat past the cap and no dose was drawn at all; before that
       * the NEWEST were dropped first, which is how a dose logged minutes ago
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

         struct screen is   = m;
         is.now             = now_ts;
         is.reading.t       = now_ts - 100;
         is.plot.hist       = big;
         is.plot.nhist      = NG + 1;
         is.dev.sensors     = 0;
         is.dev.nsensors    = 0;
         is.plot.plot_hours = 24;
         /* a distinctive, drawable insulin styling */
         is.ins.ins_marker[INS_FAST] = MARK_SQUARE_F;
         is.ins.ins_color[INS_FAST]  = 3;
         is.ins.ins_size[INS_FAST]   = MARK_SIZE_DEF;
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
   det.dev.sel       = 0;
   ui_render(&tall, &det, &h);
   write_ppm_buf(&tall, "sensor.ppm");
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
         saw_primary = saw_primary || (h.box[i].code == MA_PRIMARY);
         saw_marker  = saw_marker || (h.box[i].code == MA_MARKER);
         saw_cal     = saw_cal || (h.box[i].code == MA_CAL_OPEN);
         saw_forget  = saw_forget || (h.box[i].code == MA_FORGET);
      }
      if (!(saw_primary && saw_marker && saw_cal && saw_forget)) {
         printf("  FAIL: sensor screen targets pri=%d mark=%d cal=%d "
                "forget=%d\n",
                saw_primary, saw_marker, saw_cal, saw_forget);
         fail = 1;
      }
   }
   /* a BGM must offer SYNC NOW and must NOT offer calibration or PRIMARY */
   det.dev.sel = 2;
   ui_render(&tall, &det, &h);
   {
      int saw_sync    = 0;
      int saw_cal     = 0;
      int saw_primary = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_sync    = saw_sync || (h.box[i].code == MA_SYNC);
         saw_cal     = saw_cal || (h.box[i].code == MA_CAL_OPEN);
         saw_primary = saw_primary || (h.box[i].code == MA_PRIMARY);
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
   fg.dev.sel       = 0;
   ui_render(&tall, &fg, &h);
   write_ppm_buf(&tall, "forget.ppm");
   {
      int saw_yes = 0;
      int saw_no  = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_yes = saw_yes || (h.box[i].code == MA_FORGET_YES);
         saw_no  = saw_no || (h.box[i].code == MA_FORGET_NO);
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
   struct screen ml = {.scr = SCR_DEVLIST, .dev.devs = meters, .dev.ndev = 1};
   ui_render(&tall, &ml, &h);
   write_ppm_buf(&tall, "meterlist.ppm");
   {
      int picks = 0;
      for (int i = 0; i < h.n; i++)
         picks += (h.box[i].kind == ACT_MENU && h.box[i].code == MA_DEV_PICK);
      if (picks != 1) {
         printf("  FAIL: meter picker recorded %d picks, want 1\n", picks);
         fail = 1;
      }
   }

   /* --- rename keypad: every character reachable, plus DEL and OK --- */
   struct screen lb = set;
   lb.scr           = SCR_LABEL;
   lb.dev.sel       = 0;
   lb.entry.entry   = "KITCH";
   ui_render(&tall, &lb, &h);
   write_ppm_buf(&tall, "label.ppm");
   {
      int chars   = 0;
      int saw_del = 0;
      int saw_ok  = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         if (h.box[i].code == MA_CHAR)
            chars++;
         saw_del = saw_del || (h.box[i].code == MA_BACKSPACE);
         saw_ok  = saw_ok || (h.box[i].code == MA_OK);
      }
      if (chars != ui_label_nchars() || !saw_del || !saw_ok) {
         printf("  FAIL: rename keypad chars=%d/%d del=%d ok=%d\n", chars,
                ui_label_nchars(), saw_del, saw_ok);
         fail = 1;
      }
   }

   /* THE SAME KEYPAD TYPES THE SERVER AND THE EMAIL. It is the only way to
    * enter either, so a field that renders short a key is an account nobody
    * can reach -- which is what the deleted IP-keypad sweep above used to
    * guard, back when a server was digits and a dot. Every field must offer
    * the full character set, a backspace and a confirm, at every shape. */
   {
      static const int fields[] = {0, 1, 2}; /* NAME, SERVER, EMAIL */
      static const int geo[][2] = {
          {720,  1440},
          {1080, 1920},
          {1920, 1080},
          {480,  720 }
      };
      for (unsigned f = 0; f < sizeof fields / sizeof fields[0]; f++)
         for (unsigned g = 0; g < sizeof geo / sizeof geo[0]; g++) {
            struct hits fh;
            struct screen fm                = set;
            fm.scr                          = SCR_LABEL;
            fm.dev.sel                      = 0;
            fm.entry.entry                  = "abc";
            fm.entry.label_field            = fields[f];
            struct ANativeWindow_Buffer fb2 = {.width  = geo[g][0],
                                               .height = geo[g][1],
                                               .stride = geo[g][0],
                                               .format = 1,
                                               .bits   = g_px};
            ui_render(&fb2, &fm, &fh);
            int nch  = 0;
            int del  = 0;
            int ok   = 0;
            int tiny = 0;
            for (int i = 0; i < fh.n; i++) {
               if (fh.box[i].kind != ACT_MENU)
                  continue;
               if (fh.box[i].code == MA_CHAR) {
                  nch++;
                  if (fh.box[i].w < 8 || fh.box[i].h < 8)
                     tiny++;
               }
               del = del || (fh.box[i].code == MA_BACKSPACE);
               ok  = ok || (fh.box[i].code == MA_OK);
            }
            if (nch != ui_label_nchars() || !del || !ok || tiny) {
               printf("  FAIL: letter keypad field %d at %dx%d: chars=%d/%d "
                      "del=%d ok=%d untappable=%d\n",
                      fields[f], geo[g][0], geo[g][1], nch, ui_label_nchars(),
                      del, ok, tiny);
               fail = 1;
            }
         }
      printf("uitest: letter keypad complete for NAME, SERVER and EMAIL\n");
   }

   /* --- calibration CONFIRMATION screen: the value keypad's OK lands here and
    * it always offers CONFIRM + CANCEL. Permission is no longer a UI gate: an
    * unsupported sensor is handled by the durable calibration queue (which
    * surfaces NOT SUPPORTED), so SCR_CAL just confirms the typed value. */
   struct screen cal = set;
   cal.scr           = SCR_CAL;
   cal.dev.sel       = 0;
   cal.cal.cal_pending =
       140; /* the value typed on the keypad, awaiting CONFIRM */
   ui_render(&tall, &cal, &h);
   write_ppm_buf(&tall, "cal.ppm");
   {
      int saw_enter  = 0;
      int saw_cancel = 0;
      for (int i = 0; i < h.n; i++) {
         if (h.box[i].kind != ACT_MENU)
            continue;
         saw_enter  = saw_enter || (h.box[i].code == MA_CAL_ENTER);
         saw_cancel = saw_cancel || (h.box[i].code == MA_CAL_BACK);
      }
      if (!saw_enter || !saw_cancel) {
         printf("  FAIL: cal confirm screen enter=%d cancel=%d\n", saw_enter,
                saw_cancel);
         fail = 1;
      }
   }

   /* --- sensor-type picker offers every type --- */
   struct screen st2 = {.scr = SCR_SENSTYPE, .dev.sel = -1};
   ui_render(&tall, &st2, &h);
   write_ppm_buf(&tall, "senstype.ppm");
   {
      int types = 0;
      for (int i = 0; i < h.n; i++)
         if (h.box[i].kind == ACT_MENU && h.box[i].code == MA_TYPE)
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
   write_ppm("gate.ppm");
   int saw_cont = 0;
   for (int i = 0; i < h.n; i++)
      saw_cont = saw_cont || (h.box[i].kind == ACT_GATE_CONTINUE);
   if (!saw_cont) {
      printf("  FAIL: gate recorded no ACT_GATE_CONTINUE target\n");
      fail = 1;
   }

   /* THE AGE READOUT MUST NOT BE A PLOT TAB.
    *
    * The span-tab band starts just under the big number, and the age bar and
    * its value were later moved down into that gap -- so a tap on "how fresh
    * is this?" dispatched ACT_PLOT_TAB and switched the plot to 30D.
    *
    * Found BY PIXEL, not by guessing at the layout: the age/units/trend
    * column is the only text drawn in 0xFFCCCCCC (the tab labels are white
    * or grey), so every pixel of that colour is a label glyph, and a tap on
    * one must never dispatch a tab. Same colour-presence idiom the
    * reachability sweep above uses, and it needs no knowledge of the
    * geometry it is checking. */
   {
      static const int ashapes[][2] = {
          {1080, 1920},
          {1440, 2560},
          {720,  1280},
          {1080, 2400},
          {480,  1920},
          {1920, 1080}
      };
      int nas    = (int)(sizeof ashapes / sizeof ashapes[0]);
      int agebad = 0;
      for (int i = 0; i < nas; i++) {
         int sw                         = ashapes[i][0];
         int sh                         = ashapes[i][1];
         struct ANativeWindow_Buffer ab = {.width  = sw,
                                           .height = sh,
                                           .stride = sw,
                                           .format = 1,
                                           .bits   = g_px};
         struct screen am               = set;
         am.scr                         = SCR_MAIN;
         am.reading.t                   = now_ts - 100;
         am.now                         = now_ts;
         am.reading.glu                 = 120;
         am.reading.has_cgm             = 1;
         struct hits ah;
         ui_render(&ab, &am, &ah);
         int hits_here = 0;
         for (int py = 0; py < sh && hits_here < 3; py++)
            for (int pxx = 0; pxx < sw && hits_here < 3; pxx++) {
               if (g_px[(py * sw) + pxx] != 0xFFCCCCCCU)
                  continue;
               struct action a = ui_hit(&ah, pxx, py);
               if (a.kind != ACT_PLOT_TAB)
                  continue;
               printf("  FAIL: %dx%d label pixel (%d,%d) dispatches "
                      "ACT_PLOT_TAB\n",
                      sw, sh, pxx, py);
               hits_here++;
               agebad++;
            }
      }
      if (agebad)
         fail = 1;
      printf("uitest: label column is not a plot tab on %d shapes\n", nas);
   }

   /* CO-LOCATED MARKERS STAY REACHABLE.
    *
    * A dose and a weight logged in the same sitting map to one pixel column,
    * and a plain nearest-by-x pick then hands every touch on the plot to the
    * same one: the other is drawn and cannot be scrubbed at all. plot_hit's
    * `split` shares the column's territory out between them. Assert on the
    * REACHABILITY (does some press select it), not on the offsets, so the
    * split's arithmetic can be retuned without rewriting the test. */
   {
      const int pw         = 700;
      const int ph         = 400;
      const long pnow      = 1000000;
      const int phours     = 24;
      struct plot_pt cp[3] = {
          {0, 0, 0, 0, 0, 0},
          {0, 0, 0, 0, 0, 0},
          {0, 0, 0, 0, 0, 0}
      };
      cp[0].t = pnow - (3600L * 5); /* dose */
      cp[1].t = pnow - (3600L * 5); /* weight, same instant */
      cp[2].t = pnow - (3600L * 12);
      for (int i = 0; i < 3; i++)
         cp[i].glu = 60;
      for (int sp = 0; sp < 2; sp++) {
         int won[3] = {0, 0, 0};
         for (int tx = 0; tx < pw; tx++) {
            int k = plot_hit((struct plot_rect){0, 0, pw, ph}, cp, 3, pnow,
                             phours, (struct plot_cfg){0, 2}, tx, ph / 2, sp);
            if (k >= 0)
               won[k]++;
         }
         printf("uitest: co-located pick split=%d reach %d/%d/%d px\n", sp,
                won[0], won[1], won[2]);
         /* Only the split pass is required to reach both; the plain pass is
          * the glucose trace's rule and is deliberately left alone. */
         if (sp && (won[0] < 10 || won[1] < 10 || won[2] < 10)) {
            printf("  FAIL: a co-located marker is unscrubbable\n");
            fail = 1;
         }
      }
   }

   /* ---- THE PINNED SHORTCUTS SPLIT INTO ROWS AT THE RIGHT COUNT ----
    *
    * Three or fewer on one row, four to six on two, at most three per row,
    * balanced. What makes this worth asserting rather than eyeballing is that
    * every WRONG split still renders: six buttons in one row simply become
    * narrow, six in three rows simply run down the screen, and both look
    * plausible in a screenshot. The row structure is only visible in the
    * targets' y coordinates, which is where this looks.
    *
    * It reads the HIT BOXES, not the pixels, for the same reason: a button
    * whose frame is drawn is not a button that can be pressed, and the split
    * has to be right in the layer that dispatches. */
   printf("== pinned shortcuts fill one row, then two ==\n");
   {
      static const int ids[6] = {SC_INS_FAST, SC_INS_SLOW, SC_WEIGHT,
                                 SC_INSLOG,   SC_WTLOG,    SC_FOOD};
      static const int codes[6] = {MA_INS_FAST,    MA_INS_SLOW,   MA_WT_OPEN,
                                   MA_INSLOG_OPEN, MA_WTLOG_OPEN, MA_FOOD_OPEN};
      for (int n = 1; n <= 6; n++) {
         struct screen sm = m;
         sm.scr           = SCR_MAIN;
         sm.now           = now_ts;
         sm.reading.t     = now_ts - 100;
         sm.reading.glu   = 120;
         sm.reading.has_cgm = 1;
         for (int i = 0; i < SC_MAX; i++)
            sm.prefs.shortcut[i] = (i < n) ? ids[i] : SC_NONE;
         struct hits hh;
         struct ANativeWindow_Buffer pb = {.width  = 1080,
                                           .height = 2340,
                                           .stride = 1080,
                                           .format = 1,
                                           .bits   = g_px};
         ui_render(&pb, &sm, &hh);
         /* Every pin's own target, found by the action it fires. */
         /* PER-ROW POPULATIONS, not just how many rows there are. A 3+1
          * split occupies two rows with at most three in each and passes
          * every count-only test, while looking exactly like the lone
          * afterthought button the balanced rule exists to avoid. */
         int rows[8];
         int pop[8];
         int nrow = 0;
         int seen = 0;
         for (int i = 0; i < hh.n; i++) {
            int isp = 0;
            for (int k = 0; k < n; k++)
               if (hh.box[i].code == codes[k])
                  isp = 1;
            if (!isp)
               continue;
            seen++;
            int found = -1;
            for (int r = 0; r < nrow; r++)
               if (rows[r] == hh.box[i].y)
                  found = r;
            if (found < 0 && nrow < 8) {
               rows[nrow] = hh.box[i].y;
               pop[nrow]  = 0;
               found      = nrow++;
            }
            if (found >= 0)
               pop[found]++;
         }
         /* THE '+' IS ON THE FIRST ROW, whatever the row count.
          *
          * It used to follow the LAST row, so pinning a fourth shortcut moved
          * the one control that opens the whole ADD menu -- and nothing here
          * could see it, because every other assertion is about the shortcut
          * targets themselves. Compared against the FIRST row's y, which is
          * the smallest of the row positions collected above. */
         int firsty = 1 << 30;
         for (int r = 0; r < nrow; r++)
            if (rows[r] < firsty)
               firsty = rows[r];
         int plus_y = -1;
         for (int i = 0; i < hh.n; i++)
            if (hh.box[i].code == MA_ADD_OPEN)
               plus_y = hh.box[i].y;
         if (nrow > 0 && plus_y >= 0) {
            /* The targets are recorded with different paddings -- the row
             * buttons at their own top edge, the '+' three units above its
             * glyph -- so they are compared with that slack rather than for
             * equality. What must not happen is the '+' landing a whole row
             * pitch below the first row, which is what the old code did. */
            int slack = 8 * (rows[0] > 0 ? 1 : 1) + 40;
            if (plus_y > firsty + slack) {
               printf("  FAIL: %d pins put the '+' at y=%d, a row below the "
                      "first shortcut row at y=%d\n",
                      n, plus_y, firsty);
               fail = 1;
            }
         }

         int want_rows = n > 3 ? 2 : 1;
         int want_per  = n > 3 ? ((n + 1) / 2) : n;
         printf("uitest: %d pinned -> %d row(s), %d target(s)\n", n, nrow,
                seen);
         if (seen != n) {
            printf("  FAIL: %d pins drew %d targets -- a pin that is stored "
                   "but not dispatchable is a dead button\n",
                   n, seen);
            fail = 1;
         }
         if (nrow != want_rows) {
            printf("  FAIL: %d pins occupy %d rows, want %d\n", n, nrow,
                   want_rows);
            fail = 1;
         }
         int lo = 99;
         int hi = 0;
         for (int r = 0; r < nrow; r++) {
            if (pop[r] < lo)
               lo = pop[r];
            if (pop[r] > hi)
               hi = pop[r];
         }
         if (nrow > 0 && (hi > 3 || hi - lo > 1 || hi != want_per)) {
            printf("  FAIL: %d pins split %d..%d per row, want %d per row "
                   "(max 3, balanced)\n",
                   n, lo, hi, want_per);
            fail = 1;
         }
         if (hh.overflow) {
            printf("  FAIL: %d pins overflowed the hit-box budget\n", n);
            fail = 1;
         }
      }
   }

   /* ---- A SLOT THIS BUILD CANNOT DRAW, AND A HOLE ----
    *
    * Both shapes are reachable: a pin is stored by IDENTITY so that the
    * renderer's numbering can change, which means a file can name an action
    * this build no longer offers; and the array is kept dense by settings.c,
    * but a hand-edited file need not be. Neither is exercised by a fixture of
    * consecutive valid pins, and both break the row split in a way that draws
    * perfectly:
    *
    *   - counting STORED rather than RENDERABLE pins shapes the rows around a
    *     button that never appears, so four stored pins with one unknown lay
    *     out as 3+1 with a hole where the missing one would have been;
    *   - taking the row from the LOOP INDEX rather than from the number of
    *     buttons actually placed splits where the gap is instead of after
    *     percol buttons, which turns three pins into two rows.
    *
    * Measured: with neither case present, both mutations pass the whole
    * suite. */
   /* ---- EXERCISE PINS LIKE ANYTHING ELSE, AND STILL SHOWS ITS LEVEL ----
    *
    * It is the one entry in the pin table that is not a plain button: its
    * level, colour and settling bar are the control, not decoration. The
    * objection that kept it unpinnable was that a second copy could disagree
    * with the first -- so what matters here is that the pinned one is a real,
    * dispatchable target drawn by the same function as the ADD menu's. */
   printf("== a pinned EXERCISE is a working control ==\n");
   {
      struct screen sm   = m;
      sm.scr             = SCR_MAIN;
      sm.now             = now_ts;
      sm.reading.t       = now_ts - 100;
      sm.reading.glu     = 120;
      sm.reading.has_cgm = 1;
      for (int i = 0; i < SC_MAX; i++)
         sm.prefs.shortcut[i] = SC_NONE;
      sm.prefs.shortcut[0] = SC_EXERCISE;
      sm.food.ex_level     = 2;
      sm.food.ex_remaining = EX_SETTLE_S / 2;
      struct hits hh;
      struct ANativeWindow_Buffer pb = {.width  = 1080,
                                        .height = 2340,
                                        .stride = 1080,
                                        .format = 1,
                                        .bits   = g_px};
      ui_render(&pb, &sm, &hh);
      int found = 0;
      for (int i = 0; i < hh.n; i++)
         if (hh.box[i].code == MA_EXERCISE)
            found++;
      ck_col(found == 1, "a pinned EXERCISE puts exactly one target on the "
                         "main screen");
      ck_col(!hh.overflow, "...without overflowing the hit budget");
      /* THE LEVEL IS ON IT. The button's own colour is what encodes the
       * level, and it is a colour nothing else on this screen draws, so its
       * presence is the check that the pinned copy is the real control and
       * not a plain label with the right action behind it. */
      ck_col(count_color(&pb, 0xFFFF6622) > 0,
             "...drawn in the colour of the level it is on");
   }

   printf("== an unknown pin and a hole do not shape the rows ==\n");
   {
      static const int codes[6] = {MA_INS_FAST,    MA_INS_SLOW,   MA_WT_OPEN,
                                   MA_INSLOG_OPEN, MA_WTLOG_OPEN, MA_FOOD_OPEN};
      struct {
         const char *what;
         int pin[SC_MAX];
         int want_seen, want_rows, want_per;
      } cases[2] = {
          /* 99 is no id this build defines (SC_ID_LAST is 5), so it is
           * exactly what an older pin looks like to a newer build. */
          {"an unknown pin between valid ones",
           {SC_INS_FAST, 99, SC_INS_SLOW, SC_WEIGHT, SC_INSLOG, SC_NONE},
           4, 2, 2},
          {"an empty slot between valid ones",
           {SC_INS_FAST, SC_NONE, SC_INS_SLOW, SC_WEIGHT, SC_NONE, SC_NONE},
           3, 1, 3},
      };
      for (int ci = 0; ci < 2; ci++) {
         struct screen sm   = m;
         sm.scr             = SCR_MAIN;
         sm.now             = now_ts;
         sm.reading.t       = now_ts - 100;
         sm.reading.glu     = 120;
         sm.reading.has_cgm = 1;
         for (int i = 0; i < SC_MAX; i++)
            sm.prefs.shortcut[i] = cases[ci].pin[i];
         struct hits hh;
         struct ANativeWindow_Buffer pb = {.width  = 1080,
                                           .height = 2340,
                                           .stride = 1080,
                                           .format = 1,
                                           .bits   = g_px};
         ui_render(&pb, &sm, &hh);
         int rows[8];
         int pop[8];
         int nrow = 0;
         int seen = 0;
         for (int i = 0; i < hh.n; i++) {
            int isp = 0;
            for (int k = 0; k < 6; k++)
               if (hh.box[i].code == codes[k])
                  isp = 1;
            if (!isp)
               continue;
            seen++;
            int found = -1;
            for (int r = 0; r < nrow; r++)
               if (rows[r] == hh.box[i].y)
                  found = r;
            if (found < 0 && nrow < 8) {
               rows[nrow] = hh.box[i].y;
               pop[nrow]  = 0;
               found      = nrow++;
            }
            if (found >= 0)
               pop[found]++;
         }
         int hi = 0;
         int lo = 99;
         for (int r = 0; r < nrow; r++) {
            if (pop[r] > hi)
               hi = pop[r];
            if (pop[r] < lo)
               lo = pop[r];
         }
         printf("uitest: %s -> %d target(s) in %d row(s)\n", cases[ci].what,
                seen, nrow);
         if (seen != cases[ci].want_seen || nrow != cases[ci].want_rows ||
             (nrow > 0 && (hi != cases[ci].want_per || hi - lo > 1))) {
            printf("  FAIL: %s: %d targets in %d rows (%d..%d per row); want "
                   "%d in %d rows, %d per row\n",
                   cases[ci].what, seen, nrow, lo, hi, cases[ci].want_seen,
                   cases[ci].want_rows, cases[ci].want_per);
            fail = 1;
         }
      }
   }

   /* ---- NO SCREEN SEPARATES ITS SECTIONS WITH A HORIZONTAL RULE ----
    *
    * ALARMS was the only screen in the app that drew one, under each of its
    * two section captions, while every other menu separates sections with a
    * blank line. One screen looking structurally unlike all the others is the
    * inconsistency; the rule is what went, not the blank lines elsewhere.
    *
    * Asserted by WIDTH rather than by colour, so it still holds for a rule
    * drawn in some other shade later: nothing this screen draws is a wide
    * uniform horizontal run except a rule. The threshold is a quarter of the
    * width -- comfortably above any glyph stroke and far below the edge-to-
    * edge rule that used to be here (width - 8*sc). */
   printf("== no menu draws a horizontal section rule ==\n");
   {
      /* ALARMS ALONE, and the reason is a limit of the measurement rather
       * than of the rule.
       *
       * SETTINGS and ADDMENU are built from framed buttons, and a framed
       * button spanning the column draws a full-width horizontal edge at its
       * top and another at its bottom -- measured at 1040px of 1080. That is
       * a control's border, not a separator between sections, and no cheap
       * scan of the pixels tells the two apart. ALARMS is built from plain
       * rows (menu_row / thresh_menu_row) and draws no frames at all, so on
       * that screen a wide uniform run can only be a rule, which is what
       * makes the assertion meaningful exactly here.
       *
       * Measured after the change: 50px, the widest glyph run on the screen.
       * Before it: 1040. */
      static const int rulescr[1] = {SCR_ALARM};
      for (int i = 0; i < 1; i++) {
         struct screen sm = m;
         sm.scr           = rulescr[i];
         sm.now           = now_ts;
         struct hits hh;
         struct ANativeWindow_Buffer pb = {.width  = 1080,
                                           .height = 2340,
                                           .stride = 1080,
                                           .format = 1,
                                           .bits   = g_px};
         ui_render(&pb, &sm, &hh);
         int run = longest_hrun(&pb, 0xFF000000);
         printf("uitest: scr %d longest horizontal run %d px of %d\n",
                rulescr[i], run, pb.width);
         if (run >= pb.width / 4) {
            printf("  FAIL: scr %d draws a %d px horizontal rule; sections "
                   "are separated by a blank line everywhere else\n",
                   rulescr[i], run);
            fail = 1;
         }
      }
   }

   /* ---- THE BIG NUMBER AGREES WITH THE ALARM AND THE BANNER ----
    *
    * Three things describe one threshold: alarm_zone decides whether to sound,
    * banner_of decides whether to draw LOW, and the number's colour. They were
    * two before -- the colour came from a FIXED medical scale that knows
    * nothing about what the user configured -- so somebody with a low alarm at
    * 85 got the alarm and the banner at 80 while the number stayed green. An
    * app contradicting itself about the same reading is a reason to dismiss a
    * real hypo, which is exactly what the banner's own comment records.
    *
    * INCLUSIVE at every limit, like the other two. */
   printf("== the big number is coloured by the configured band too ==\n");
   {
      const uint32_t green = 0xFF33FF88;
      const uint32_t white = 0xFFFFFFFF;
      const uint32_t alarm = 0xFF2020E0;
      const uint32_t nudge = 0xFF20A0FF;
      const uint32_t red   = 0xFF0000FF;
      const uint32_t amber = 0xFF0080FF;

      /* The fixed scale, untouched, when nothing is configured. */
      ck_col(glu_color_band(120, 0, 0, 0, 0) == green,
             "with no band set the fixed scale still decides");

      /* A LOW ALARM ABOVE THE FIXED GREEN FLOOR is the case that was broken:
       * 80 is green on the fixed scale and inside the user's alarm. */
      ck_col(glu_color_band(80, 85, 300, 100, 250) == alarm,
             "a reading inside the configured LOW alarm is not drawn green");
      ck_col(glu_color_band(85, 85, 300, 100, 250) == alarm,
             "...INCLUSIVE at the limit, as the alarm and the banner are");
      ck_col(glu_color_band(86, 85, 300, 100, 250) == nudge,
             "...and one above it falls to the nudge band");

      /* The high end, same rule. */
      ck_col(glu_color_band(300, 85, 300, 100, 250) == alarm,
             "a reading at the HIGH alarm takes the alarm colour");
      ck_col(glu_color_band(250, 85, 300, 100, 250) == nudge,
             "...and one at the nudge limit takes the nudge colour");

      /* NEVER LESS ALARMING THAN THE FIXED SCALE. A high alarm of 400 must not
       * repaint a hypo on the way past. */
      ck_col(glu_color_band(45, 40, 400, 60, 350) == red,
             "a hypo stays red even when the configured low is below it");
      ck_col(glu_color_band(65, 40, 400, 60, 350) == amber,
             "...and 65 stays amber rather than taking the nudge colour");

      /* In range on both scales. */
      ck_col(glu_color_band(120, 85, 300, 100, 250) == green,
             "a reading inside every band is still green");
      ck_col(glu_color_band(200, 85, 300, 100, 250) == white,
             "...and one above the fixed green ceiling is still white");
   }

   printf("uitest: %s\n", fail ? "FAIL" : "OK");
   return fail;
}
