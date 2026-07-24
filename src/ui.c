// SPDX-License-Identifier: GPL-3.0
// ui.c --- On-screen rendering + touch input (the interactive UI layer)
// Copyright 2026 Jakob Kastelic

/* The whole UI as a pure function of an immutable `struct screen`: ui_render()
 * draws the current screen into a locked framebuffer and records its touch
 * targets into `struct hits`; ui_hit() maps a later tap to the action the shell
 * (main.c) should perform. No globals, no callbacks -- so every screen builds
 * and runs on the host against a malloc'd buffer (see test/uitest.c, which
 * renders each screen to a PPM and checks its hit-targets with no phone). */
#include "ui.h"
#include "font.h"
#include "ndk.h"
#include "plot.h"
#include "sensors.h" /* sensor types, kinds, marker enum */
#include "util.h"    /* str_snapshot */
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* Layout constants owned by the UI (not the shell). */
#define UI_COLS   33         /* character columns the layout targets */
#define UI_TABS   6          /* plot-span tabs */
#define UI_HILITE 0xFFAAAAAA /* scrub highlight dot (gray) */
/* Readings whose sensor is no longer in a slot: dim, so they stay legible as
 * history without competing with a live trace. Deliberately NOT 0xFF666666 --
 * that is plot.c's x-tick colour, and an orphan marker drawn in it reads as
 * part of the axis. */
#define UI_ORPHAN 0xFF8A8AA0
static const int ui_tab_hours[UI_TABS] = {3, 6, 12, 24, 72, 168};

/* plot.c takes the marker as a plain shape index (0 dot, 1 cross, 2 square,
 * 3 triangle) and deliberately does not borrow sensors.h's enum, so the two
 * numbering spaces agree only by convention. Reordering MARK_* would silently
 * draw the wrong glyph for every sensor -- cosmetic rather than dangerous, but
 * it is the same shape of latent mismatch that made the LOW alarm collide with
 * silence, so pin it where both headers are already in scope. */
_Static_assert(MARK_DOT == 0, "MARK_DOT must be plot.c shape 0");
_Static_assert(MARK_CROSS == 1, "MARK_CROSS must be plot.c shape 1");
_Static_assert(MARK_SQUARE == 2, "MARK_SQUARE must be plot.c shape 2");
_Static_assert(MARK_TRIANGLE == 3, "MARK_TRIANGLE must be plot.c shape 3");
_Static_assert(MARK_HIDE == 4, "plot.c draws shapes 0..3; HIDE follows them");

#define UI_LBL(units) ((units) ? "MMOL/L" : "MG/DL")

/* Glyph cells discarded by clipping since the last ui_clip_reset().
 *
 * Clipping is SILENT: draw_cell drops out-of-bounds pixels, so content laid out
 * past the edge simply never appears and leaves no trace -- no hit box out of
 * range, no colour missing if that colour is drawn elsewhere too. The offline
 * harness failed to notice an entirely invisible stats table and a truncated
 * medical disclaimer for several review rounds because of exactly this. One
 * counter makes it observable on every screen at once. Costs an increment on a
 * path that is already doing nothing. */
static long g_clipped;

void ui_clip_reset(void)
{
   g_clipped = 0;
}

long ui_clipped(void)
{
   return g_clipped;
}

/* Fill one sc*sc glyph cell at (bx,by), clipped to the buffer. */
static void draw_cell(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                      int bx, int by, int sc, uint32_t color)
{
   for (int dy = 0; dy < sc; dy++)
      for (int dx = 0; dx < sc; dx++) {
         int x = bx + dx;
         int y = by + dy;
         if (x >= 0 && x < buf->width && y >= 0 && y < buf->height)
            px[(y * buf->stride) + x] = color;
         else
            g_clipped++;
      }
}

void draw_str(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
              int oy, int sc, const char *s, uint32_t color)
{
   /* Hard scan bound: draw can run concurrently with a BLE-thread rewrite of a
    * text buffer (g_lines/g_status); a torn write can momentarily drop the NUL,
    * and without this cap the loop could scan off the end. No UI string is this
    * long. */
   for (int ci = 0; ci < 256 && s[ci]; ci++) {
      const uint8_t *g = glyph_for(s[ci]);
      if (!g)
         continue;
      for (int row = 0; row < 7; row++)
         for (int col = 0; col < 5; col++) {
            if (!((unsigned)g[row] & (0x10U >> (unsigned)col)))
               continue;
            int bx = ox + (((ci * 6) + col) * sc);
            int by = oy + (row * sc);
            draw_cell(px, buf, bx, by, sc, color);
         }
   }
}

void draw_frame(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                int y, int w, int h, uint32_t c)
{
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > buf->width ||
       y + h > buf->height) {
      /* Count it. A frame that does not fit vanishes ENTIRELY and silently --
       * key boxes, DEL/OK, the gate's CONTINUE button. draw_cell's counter
       * never saw this path, which is why a keypad whose key frames had all
       * disappeared still reported zero clipped cells. */
      g_clipped++;
      return;
   }
   for (int i = 0; i < w; i++) {
      px[(y * buf->stride) + x + i]           = c;
      px[((y + h - 1) * buf->stride) + x + i] = c;
   }
   for (int j = 0; j < h; j++) {
      px[((y + j) * buf->stride) + x]         = c;
      px[((y + j) * buf->stride) + x + w - 1] = c;
   }
}

/* Solid filled rectangle (draw_frame draws only the outline). Same clip-and-
 * count discipline so an off-screen fill is caught, not silently dropped. */
static void fill_rect(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                      int x, int y, int w, int h, uint32_t c)
{
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > buf->width ||
       y + h > buf->height) {
      g_clipped++;
      return;
   }
   for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
         px[((y + j) * buf->stride) + x + i] = c;
}

void fmt_glu(int mgdl, int units, char *out, int n)
{
   /* Clamp to a displayable range. Callers pass sensor values that are already
    * bounded, but the compiler cannot see that and the destination is a
    * caller-sized buffer -- so an out-of-range value would truncate silently.
    * Making the bound explicit is also the honest thing to show: a number
    * outside this range is not a glucose reading. */
   if (mgdl < -9999)
      mgdl = -9999;
   if (mgdl > 9999)
      mgdl = 9999;
   if (units) { /* mmol/L = mg/dL / 18, one decimal */
      int t = ((mgdl * 10) + 9) / 18;
      /* Format the sign once and the digits from the MAGNITUDE. C division
       * truncates toward zero, so a negative t made both halves negative and
       * "%d.%d" printed "-555.-4" -- the clamp above exists precisely so an
       * out-of-range value still renders as something honest, and this undid
       * it for every negative. No caller passes one today; the clamp is here
       * because the compiler cannot prove that, and this keeps the two
       * defences consistent. */
      int mag = t < 0 ? -t : t;
      (void)snprintf(out, n, "%s%d.%d", t < 0 ? "-" : "", mag / 10, mag % 10);
   } else {
      (void)snprintf(out, n, "%d", mgdl);
   }
}

/* ---- functional core: pure render + pure hit-test ---- */

static void clear_fb(struct ANativeWindow_Buffer *fb, uint32_t c)
{
   uint32_t *px = fb->bits;
   for (int32_t y = 0; y < fb->height; y++)
      for (int32_t x = 0; x < fb->width; x++)
         px[(y * fb->stride) + x] = c;
}

static void add_hit(struct hits *h, int x, int y, int w, int hgt, int kind,
                    int arg)
{
   if (h->n >= UI_MAX_HITS)
      return;
   h->box[h->n].x    = x;
   h->box[h->n].y    = y;
   h->box[h->n].w    = w;
   h->box[h->n].h    = hgt;
   h->box[h->n].kind = kind;
   h->box[h->n].arg  = arg;
   h->n++;
}

/* big-number colour by fixed medical range (0xAABBGGRR) */
static uint32_t glu_color(int g)
{
   if (g < 50)
      return 0xFF0000FF; /* red    */
   if (g < 70)
      return 0xFF0080FF; /* orange */
   if (g < 180)
      return 0xFF33FF88; /* green  */
   return 0xFFFFFFFF;    /* white  */
}

static uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFF; /* plot dots */
}

void fmt_trend(int tr, char *out, int n)
{
   if (tr == 127) {
      (void)snprintf(out, n, "--");
      return;
   }
   int a = tr < 0 ? -tr : tr;
   if (a > 9999) /* see fmt_glu: keep the formatted width bounded */
      a = 9999;
   (void)snprintf(out, n, "%c%d.%d", tr < 0 ? '-' : '+', a / 10, a % 10);
}

void fmt_hms(long epoch, long tz, char *out, int n)
{
   long t = (epoch + tz) % 86400;
   if (t < 0)
      t += 86400;
   (void)snprintf(out, n, "%02ld:%02ld:%02ld", t / 3600, (t % 3600) / 60,
                  t % 60);
}

void fmt_date(long epoch, long tz, char *out, int n)
{
   long t    = epoch + tz;
   long secs = t % 86400;
   long z    = t / 86400;
   if (secs < 0) {
      secs += 86400;
      z--;
   }
   /* days -> civil date (Howard Hinnant's algorithm, shifted to a 0000-03-01
    * era so leap years fall at the end of the cycle) */
   z += 719468;
   long era          = (z >= 0 ? z : z - 146096) / 146097;
   unsigned long doe = (unsigned long)(z - (era * 146097));
   unsigned long yoe =
       (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
   unsigned long doy  = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
   unsigned long mp   = ((5 * doy) + 2) / 153;
   unsigned long dday = doy - (((153 * mp) + 2) / 5) + 1;
   unsigned long mon  = mp < 10 ? mp + 3 : mp - 9;
   long year          = (long)yoe + (era * 400) + (mon <= 2 ? 1 : 0);
   (void)snprintf(out, n, "%04ld-%02lu-%02lu %02ld:%02ld", year, mon, dday,
                  secs / 3600, (secs % 3600) / 60);
}

void fmt_ago(long now, long then, char *out, int n)
{
   if (then <= 0) {
      (void)snprintf(out, n, "NEVER");
      return;
   }
   long d = now - then;
   if (d < 0)
      d = 0;
   if (d < 120)
      (void)snprintf(out, n, "%ld S", d);
   else if (d < 3600) /* without this band, 10 minutes reads as "0 H" */
      (void)snprintf(out, n, "%ld M", d / 60);
   else if (d < 86400)
      (void)snprintf(out, n, "%ld H", d / 3600);
   else
      (void)snprintf(out, n, "%ld D", d / 86400);
}

void fmt_dur(long seconds, char *out, int n)
{
   if (seconds <= 0) {
      (void)snprintf(out, n, "--");
      return;
   }
   long d  = seconds / 86400;
   long hr = (seconds % 86400) / 3600;
   long mi = (seconds % 3600) / 60;
   if (d > 0)
      (void)snprintf(out, n, "%ld D %ld H", d, hr);
   else if (hr > 0)
      (void)snprintf(out, n, "%ld H %ld M", hr, mi);
   else
      (void)snprintf(out, n, "%ld M", mi);
}

/* Distinguishable at a glance on a dark background, and distinct from the
 * glucose palette so a sensor's colour is never mistaken for a value. */
/* Framebuffer is RGBA_8888 and pixels are written as raw u32 on a little-endian
 * device, so the byte order is 0xAABBGGRR (low byte = red) -- the same encoding
 * glu_color uses (its "red" is 0xFF0000FF). These were previously written as
 * standard 0xAARRGGBB, which swaps red and blue: BLUE rendered orange, AMBER
 * rendered blue, etc. Encoded correctly (R and B swapped) they now match their
 * names. */
static const uint32_t ui_sensor_colors[7] = {
    0xFF88FF33 /* GREEN */,
    0xFFFFAA44 /* BLUE */,
    0xFF44CCFF /* AMBER */,
    0xFFAA66FF /* PINK */,
    0xFFEEFF66 /* CYAN */,
    0xFFFF88BB /* VIOLET */,
    0xFFFFFFFF /* WHITE -- the default primary-trace colour */};
static const char *const ui_color_names[7] = {
    "GREEN", "BLUE", "AMBER", "PINK", "CYAN", "VIOLET", "WHITE"};
/* Indexed by the (frozen) enum value. */
static const char *const ui_marker_names[MARK_N] = {
    "DOT",         "CROSS",    "SQUARE", "TRIANGLE", "HIDE",
    "SQUARE FILL", "TRI FILL", "CIRCLE", "CIRC FILL"};
/* Order the MARKER picker lists shapes in (grouped filled/empty, HIDE last).
 * DOT is omitted -- it renders identically to SQUARE FILL. */
#define UI_NMARKERS 8
static const int ui_marker_order[UI_NMARKERS] = {
    MARK_CIRCLE,   MARK_CIRCLE_F,   MARK_SQUARE, MARK_SQUARE_F,
    MARK_TRIANGLE, MARK_TRIANGLE_F, MARK_CROSS,  MARK_HIDE};

uint32_t ui_sensor_color(int color)
{
   if (color < 0 || color > 6)
      color = 0;
   return ui_sensor_colors[color];
}

const char *ui_color_name(int color)
{
   if (color < 0 || color > 6)
      color = 0;
   return ui_color_names[color];
}

const char *ui_marker_name(int marker)
{
   if (marker < 0 || marker >= MARK_N)
      marker = 0;
   return ui_marker_names[marker];
}

/* Full brand name for the ADD DEVICE picker and the per-device TYPE row. The
 * stored type name stays short (STELO/G7/ONETOUCH) so the 16-char device label
 * does not truncate. */
static const char *sensor_disp_name(int type)
{
   switch (type) {
      case SENSOR_STELO: return "DEXCOM STELO";
      case SENSOR_G7: return "DEXCOM G7";
      case SENSOR_ONETOUCH: return "ONETOUCH VERIO";
      default: return sensor_type_name(type);
   }
}

/* Rows consumed above the sensor entries: title (2), DISPLAY (6), ALARM (5),
 * PERMISSIONS (8), the SENSORS header (1), and the trailing ADD row (1). Keep
 * in step with render_settings. */
/* UI_SET_ABOVE now lives in ui.h -- see there for why. */

/* Layout scale for the settings screen.
 *
 * Deriving the scale from WIDTH alone was a lockout bug: the row pitch it
 * produces (16*sc) is spent on HEIGHT, so the screen only fitted when the
 * aspect ratio exceeded ~2.2. On 16:9 and 18:9 phones -- 1080x1920, 1440x2560,
 * 1440x3120 -- capacity came out 0, so the SENSORS list AND the ADD NEW SENSOR
 * row were never drawn and the sensor registry was completely unreachable: no
 * way to add, open, rename, calibrate or forget anything. Scrolling is not an
 * option here (a hard design rule), so the layout must genuinely fit instead.
 *
 * Bounding the scale by height too costs smaller text on a 16:9 screen and
 * keeps every control reachable, which is the right trade. */
/* Largest scale at which `rows` rows of pitch 16*sc, plus the standard top
 * margin (h/20 + 8*sc), still fit inside height h -- bounded by width as well.
 *
 * Every full-screen menu must go through this. Deriving a scale from width
 * alone, then spending the resulting row pitch on height, is what put content
 * below the bottom of the buffer on 16:9 and 18:9 phones; because scrolling is
 * ruled out by design, off-screen content is simply unreachable. */
int ui_fit_scale(int w, int h, int rows)
{
   int sc = w / (UI_COLS * 6);
   if (sc < 1)
      sc = 1;
   int vsc = (h - (h / 20)) / (8 + (rows * 16));
   if (vsc < 1)
      vsc = 1;
   return vsc < sc ? vsc : sc;
}

int ui_settings_scale(int w, int h)
{
   /* Must match ui_sensor_capacity's requirement EXACTLY. That function needs
    *   h - start - (UI_SET_ABOVE + 1)*lh >= UI_MIN_SLOTS*lh
    * with start = h/20 + 8*sc and lh = 16*sc, i.e.
    *   h - h/20 >= sc * (8 + (UI_SET_ABOVE + UI_MIN_SLOTS + 1) * 16).
    * An earlier version omitted the +8 from `start`, leaving the two functions
    * 8*sc apart -- so on heights where the slack fell in that gap, capacity
    * still came out below UI_MIN_SLOTS and render_settings still took its
    * early return, hiding the sensor list and the ADD row. That band included
    * 1080x2280 (Galaxy S10 / Redmi Note 7 / Moto G7) and 1440x3200 (S20-S22
    * Ultra at QHD+). Derive it from the same expression so they cannot drift.
    */
   return ui_fit_scale(w, h, UI_SET_ABOVE + UI_MIN_SLOTS + 1);
}

int ui_sensor_capacity(int w, int h)
{
   int sc    = ui_settings_scale(w, h);
   int lh    = 16 * sc;
   int start = (h / 20) + (8 * sc);
   int avail = h - lh - start - (UI_SET_ABOVE * lh);
   if (avail < 0)
      return 0;
   int n = avail / lh;
   return n > UI_MAX_SLOTS ? UI_MAX_SLOTS : n;
}

/* Left/top column: big number + label column, plot tabs, plot, alarm-config
 * row. Draws into [cx, cx+cw); returns the y just below the last row. Records
 * the big-number band (open settings), the plot rect (scrub), the tab cells,
 * and the two +/- alarm buttons as touch targets. */
static int render_glucose(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h, int cx,
                          int cw, int y, int sc, int bottom)
{
   uint32_t *px  = fb->bits;
   int landscape = bottom > 0;                   /* height-constrained column */
   int pad       = landscape ? 6 * sc : 18 * sc; /* padding around the number */
   int scrub     = (m->scrub >= 0 && m->scrub < m->nhist);

   char big[8];
   uint32_t bigcol = 0;
   /* glu < 0 is "no current reading" -- the primary has no data yet (the big
    * number never borrows another sensor's). With NO live CGM at all the big
    * number is meaningless, so it (and its age below) blank out entirely
    * rather than showing a stale value from a disconnected sensor. Same
    * placeholder as stale. */
   if (m->stale || m->glu < 0 || !m->has_cgm) {
      (void)snprintf(big, sizeof big, "---");
      bigcol = 0xFF888888;
   } else {
      fmt_glu(m->glu, m->units, big, sizeof big);
      bigcol = glu_color(m->glu);
   }
   int big_y0 = 0;
   y += landscape ? 4 * sc : 12 * sc;

   char tr[8];
   /* Sized for the widest formatted age. `a` is clamped below, but the
    * compiler cannot see that, and a genuinely huge value would truncate. */
   char agestr[24];
   if (m->stale || m->glu < 0 || !m->has_cgm)
      (void)snprintf(tr, sizeof tr, "---");
   else
      fmt_trend(m->trend, tr, sizeof tr);
   long a = m->now - m->t;
   if (a < 0)
      a = 0;
   if (m->t <= 0 ||
       !m->has_cgm) /* no live CGM (or no reading): blank the age */
      (void)snprintf(agestr, sizeof agestr, "--");
   else if (a < 600)
      (void)snprintf(agestr, sizeof agestr, "%ld S", a);
   else
      (void)snprintf(agestr, sizeof agestr, "%ld M", a / 60);
   /* Signal strength moved to each device's own menu (SIGNAL STRENGTH); the
    * main readout shows units / trend / age only. */
   int uw    = str_len(UI_LBL(m->units));
   int aw    = str_len(agestr);
   int col_w = (uw > aw ? uw : aw) * 6 * sc;
   int gap   = 6 * sc;
   int bigsc = sc * 10; /* shrink so number + label column fit cw */
   int fit   = (cw - (4 * sc) - gap - col_w) / (str_len(big) * 6);
   if (bigsc > fit)
      bigsc = fit;
   if (bigsc < 2 * sc)
      bigsc = 2 * sc;
   int big_w = str_len(big) * 6 * bigsc;
   int bx    = cx + ((cw - (big_w + gap + col_w)) / 2);
   if (bx < cx + (2 * sc))
      bx = cx + (2 * sc);
   draw_str(px, fb, bx, y, bigsc, big, bigcol);
   /* The NUMBER ITSELF is the choose-primary target: with more than one
    * active CGM the shell opens the CHOOSE PRIMARY screen (it knows the live
    * session count; the renderer does not), with zero or one it ignores the
    * tap -- so this never navigates away by accident, which is why the old
    * whole-band settings target was removed. The glyphs only, not the band:
    * the hamburger (settings) and the tab row keep their own pixels. */
   add_hit(h, bx, y, big_w, 7 * bigsc, ACT_PICK_PRIMARY, 0);
   int colx  = bx + big_w + gap;
   int gh    = 7 * sc;    /* a label glyph is 7 rows tall */
   int num_h = 7 * bigsc; /* the big number's exact glyph height */
   /* Three values, tightly stacked and BOTTOM-aligned so the age's bottom row
    * is exactly the big number's bottom row. */
   int vlh   = gh + (2 * sc);  /* tight line pitch */
   int age_y = y + num_h - gh; /* age bottom == number bottom */
   draw_str(px, fb, colx, age_y - (2 * vlh), sc, UI_LBL(m->units), 0xFFCCCCCC);
   draw_str(px, fb, colx, age_y - vlh, sc, tr, 0xFFCCCCCC);
   draw_str(px, fb, colx, age_y, sc, agestr, 0xFFCCCCCC);
   /* Settings hamburger: a modest 3-bar icon CENTERED (both axes) in the empty
    * space above the three values. Its hit box is the ONLY way to open settings
    * now (the whole-top-band target is gone), so pad it out well past the glyph
    * so the surrounding space is pressable too. */
   int ham_w  = 9 * sc;
   int ham_bh = 2 * sc; /* bar thickness */
   int ham_gp = 2 * sc; /* gap between bars */
   int ham_h  = (3 * ham_bh) + (2 * ham_gp);
   int sp_top = y;                       /* top of the empty space */
   int sp_bot = age_y - (2 * vlh) - gap; /* just above the first value */
   int ham_y  = sp_top + (((sp_bot - sp_top) - ham_h) / 2); /* v-centre */
   if (ham_y < sp_top)
      ham_y = sp_top;
   int ham_x = colx + ((col_w - ham_w) / 2); /* h-centre in the column */
   for (int b = 0; b < 3; b++)
      fill_rect(px, fb, ham_x, ham_y + (b * (ham_bh + ham_gp)), ham_w, ham_bh,
                0xFFCCCCCC);
   int ham_pad = 8 * sc;
   add_hit(h, ham_x - ham_pad, ham_y - ham_pad, ham_w + (2 * ham_pad),
           ham_h + (2 * ham_pad), ACT_OPEN_SETTINGS, 0);
   y += (8 * bigsc) + pad;

   /* plot-window tabs (or the scrub readout while dragging) */
   int colw = cw / UI_TABS;
   int rowh = 14 * sc;
   /* Tap target reaches up to the big number's lowest pixel and no higher, then
    * down through the tab row. The big-number (settings) band ends exactly
    * there, so the two never fight over the same pixels. */
   int tab_y = y - bigsc - pad;
   int tab_h = rowh + bigsc + pad;
   /* Settings now opens ONLY from the hamburger (added above), not from the
    * whole top band -- the band target that used to sit here is gone, so a tap
    * on the number or trend no longer navigates away by accident. */
   (void)big_y0;
   if (scrub) {
      char ts[16];
      char line[48];
      char gv[12];
      fmt_hms(m->hist[m->scrub].t, m->tz_off, ts, sizeof ts);
      ts[5] = '\0';
      fmt_glu(m->hist[m->scrub].glu, m->units, gv, sizeof gv);
      /* On the multi-day spans (3D / 7D) a bare HH:MM is ambiguous across days,
       * so prefix the weekday (SUN..SAT). 1970-01-01 was a Thursday. */
      if (m->plot_hours >= 72) {
         static const char *const wd[7] = {"SUN", "MON", "TUE", "WED",
                                           "THU", "FRI", "SAT"};
         long z = (m->hist[m->scrub].t + m->tz_off) / 86400;
         int wi = (int)(((z % 7) + 4 + 7) % 7); /* 0=Sun; 1970-01-01 = Thu */
         (void)snprintf(line, sizeof line, "%s %s  %s %s", gv, UI_LBL(m->units),
                        wd[wi], ts);
      } else {
         (void)snprintf(line, sizeof line, "%s %s  %s", gv, UI_LBL(m->units),
                        ts);
      }
      int lw = str_len(line) * 6 * 2 * sc;
      int lx = cx + ((cw - lw) / 2);
      if (lx < cx + (2 * sc))
         lx = cx + (2 * sc);
      draw_str(px, fb, lx, y, 2 * sc, line, 0xFFFFFFFF);
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
                  ui_tab_hours[i] == m->plot_hours ? 0xFFFFFFFF : 0xFF888888);
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
    * 5 info rows + gap + 4 stat rows + the banner's advance and 7*5 glyph) plus
    * one blank line (16) for the gap BELOW the alarm. render_info fits itself
    * into whatever is left, so reserving at least this much can only leave it
    * room to spare -- never clip. */
   /* Reserve, in sc units, everything drawn between the plot bottom and the
    * screen bottom AT FULL FONT:
    *   34 = what render_glucose itself adds after the plot (the ALARM LOW/HIGH
    *        config row: 9 gap + 7 row + 18 portrait pad),
    *  202 = render_info's own budget (needv: 5 info rows + gap + 4 stat rows +
    *        the banner's advance and glyph),
    *   16 = one blank line BELOW the alarm's large letters.
    * Reserving render_info's full budget keeps its font at sc (it only
    * downscales when squeezed), which is the point -- the plot grows into the
    * dead space, the text below it does NOT shrink. */
   int reserve = (34 + 202 + 16) * sc;
   int grow = fb->height - y - reserve; /* plot bottom = reserve from screen */
   int ph   = 0;
   if (landscape)
      ph = bottom - y - (26 * sc);
   else
      /* Never below the old fixed height (short screens keep exactly the
       * previous layout, so nothing that used to fit now clips); grow only
       * into genuine excess on taller screens. */
      ph = (grow > 12 * bigsc) ? grow : 12 * bigsc;
   if (ph < 20 * sc)
      ph = 20 * sc;
   int plot_x = cx + (2 * sc);
   int plot_y = y;
   int plot_w = cw - (4 * sc);
   /* Must EQUAL store.h's NHIST: the shell sends up to NHIST points, and this
    * static cap clamps how many the plot draws. If it were smaller, the plot
    * would truncate the oldest in-window points even when the shell holds a
    * full 7 days -- the same shrinking-7D bug NHIST's sizing fixes. ui.c is
    * intentionally decoupled from store.h, so the Makefile `crosscheck` target
    * greps both and fails the build if they ever drift apart. */
#define UI_PLOT_MAX 5040
   static struct plot_pt pts[UI_PLOT_MAX];
   int np = m->nhist < UI_PLOT_MAX ? m->nhist : UI_PLOT_MAX;
   for (int i = 0; i < np; i++) {
      pts[i].t   = m->hist[i].t;
      pts[i].glu = m->hist[i].glu;
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
      int matched   = 0;
      for (int k = 0; k < m->nsensors; k++) {
         /* Pre-registry legacy readings (src 0) match NO sensor and keep the
          * default value-based styling below. They used to be attributed to
          * the PRIMARY sensor at display time ("one CGM behind all of it"),
          * but the primary flag is mutable: the moment the user made a
          * freshly paired G7 primary, days of another sensor's legacy data
          * flipped to the G7's colour and marker on the plot -- a provenance
          * lie the append-only log exists to prevent. Unknown provenance is
          * rendered as the neutral main trace, never as a live device. */
         if (m->sensors[k].id == m->hist[i].src && m->hist[i].src != 0) {
            matched       = 1;
            pts[i].col    = ui_sensor_color(m->sensors[k].color);
            pts[i].marker = m->sensors[k].marker; /* shape applies to ALL,
                                                     including the primary */
            pts[i].size = m->sensors[k].size;
            /* HIDE: drop this device's point entirely. */
            if (m->sensors[k].marker == MARK_HIDE)
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
      if (!matched && m->nsensors > 0 && m->hist[i].src != 0) {
         pts[i].marker = MARK_CROSS;
         pts[i].col    = UI_ORPHAN;
      }
   }
   int prad = 3 * sc / 2;
   if (m->plot_hours >= 168)
      prad = prad / 2;
   else if (m->plot_hours >= 72)
      prad = prad * 3 / 4;
   plot_render(px, fb->stride, fb->width, fb->height, plot_x, plot_y, plot_w,
               ph, pts, np, m->now, m->plot_hours, prad, white_color,
               scrub ? m->scrub : -1, UI_HILITE);
   /* the whole plot rect scrubs; the shell resolves the datapoint via plot_hit
    */
   add_hit(h, plot_x, plot_y, plot_w, ph, ACT_SCRUB, 0);
   y += ph + (9 * sc);

   /* alarm config row: "ALARM  LOW - 110 +  HIGH - 300 +", full-column */
   const uint32_t gy = 0xFF888888;
   const uint32_t wt = 0xFFFFFFFF;
   int cwid          = 6 * sc;
   char lo[8];
   char hi[8];
   fmt_glu(m->alarm_low, m->units, lo, sizeof lo);
   fmt_glu(m->alarm_high, m->units, hi, sizeof hi);
   const char *tok[9] = {"ALARM", "LOW", "-", lo, "+", "HIGH", "-", hi, "+"};
   uint32_t tcol[9]   = {gy, gy, gy, wt, gy, gy, gy, wt, gy};
   int total          = 0;
   for (int i = 0; i < 9; i++)
      total += str_len(tok[i]) * cwid;
   int g = (cw - total) / 10;
   if (g < cwid)
      g = cwid;
   int ax   = cx + g;
   int al_y = y - (3 * sc);
   int al_h = (3 * sc) + (7 * sc) + pad;
   /* the four +/- buttons in draw order: LOW-, LOW+, HIGH-, HIGH+ */
   int alact[4] = {ACT_ALARM_LOW, ACT_ALARM_LOW, ACT_ALARM_HIGH,
                   ACT_ALARM_HIGH};
   int alarg[4] = {-1, +1, -1, +1};
   int btn      = 0;
   for (int i = 0; i < 9; i++) {
      draw_str(px, fb, ax, y, sc, tok[i], tcol[i]);
      int tw = str_len(tok[i]) * cwid;
      if (tok[i][0] == '-' || tok[i][0] == '+') {
         add_hit(h, ax - (g / 2), al_y, tw + g, al_h, alact[btn], alarg[btn]);
         btn++;
      }
      ax += tw + g;
   }
   y += (7 * sc) + pad;
   return y;
}

/* Right/bottom column: sensor+session panel, rolling-stats table and the
 * alarm banner. */
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
    * Vertical budget in units of sc: 4 info rows (4*16) + a 7 gap + 4 stats
    * rows (4*16) + the banner (7+9 advance, then a 7*5 glyph).
    * Horizontal: the stats rows format 34 fixed columns plus a units label of
    * up to 6, and a glyph is 6*sc wide -- so 40*6 = 240 units of sc, which is
    * wider than UI_COLS implies and was clipping "MG/DL" off the right edge. */
   /* Fit to the space left below the plot in BOTH axes -- the downscale is
    * kept, because it is what makes this block fit on smaller screens. The bug
    * was not the downscale, it was OVER-BUDGETING the width: `wide` reserved 53
    * chars for the worst-case unbonded STATE line, so on a normal phone the
    * whole block was shrunk a step below the rest of the UI to reserve room no
    * real row uses.
    *
    * The genuine fixed-width content is the stats table: 4+5*5 columns plus a
    * units label of up to 6 = 35. Budget THAT. The one variable-width row
    * (STATE) is instead truncated to the column, so it can never widen the
    * budget. On a normal-width phone this yields the same `sc` as everywhere
    * else; on a narrow or split-screen window it still steps down gracefully.
    */
   int needv  = (5 * 16) + 7 + (4 * 16) + (7 + 9) + (7 * 5);
   int availv = fb->height - y;
   int vsc    = availv > 0 ? availv / needv : 1;
   int hsc    = cw / (2 + (35 * 6)); /* 35 = the stats table's fixed width */
   if (vsc < sc)
      sc = vsc;
   if (hsc < sc)
      sc = hsc;
   if (sc < 1)
      sc = 1;
   int x              = cx + (2 * sc);
   int lh             = 16 * sc; /* row pitch: matches the settings leading */
   const uint32_t col = 0xFFCCCCCC;
   int info_y0        = y; /* top of the STATE..PRED block (a tap-shortcut) */

   /* Widest row is the unbonded STATE line (8 + UI_COLS status + advert count)
    * and the stats rows (4 + 5*6 + a 6-char unit, with each cell up to 15 now
    * that hc[] is wider). 96 covers both with room to spare. */
   char row[96];
   /* PRIMARY row (top of the block): the marker glyph + label of the CGM that
    * owns the big number, so at a glance you know WHICH sensor this block and
    * the number describe. Dashes when no CGM is primary. */
   {
      int pk = -1;
      for (int k = 0; k < m->nsensors; k++)
         if (m->sensors[k].primary && m->sensors[k].kind == KIND_CGM) {
            pk = k;
            break;
         }
      if (pk >= 0) {
         const struct ui_sensor *ps = &m->sensors[pk];
         draw_str(px, fb, x, y, sc, "PRIMARY", col);
         /* glyph in the sensor's colour, centred in its own cell, then the
          * label a FULL character-cell further right -- the same marker + gap
          * + name spacing the SETTINGS and PRIMARY CGM lists use. */
         int gx = x + (8 * 6 * sc);
         if (ps->marker != MARK_HIDE)
            plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                              gx + (3 * sc), y + (3 * sc), 2 * sc, ps->marker,
                              ui_sensor_color(ps->color));
         draw_str(px, fb, gx + (2 * 6 * sc), y, sc, ps->label, col);
      } else {
         draw_str(px, fb, x, y, sc, "PRIMARY --", col);
      }
      y += lh;
   }
   if (!m->has_cgm) {
      /* No CGM registered: this whole block describes a CGM, so it must NOT
       * borrow the global status line (a meter sync leaves it "SYNCED") or
       * a stale session. Blank STATE here; SESSION and PRED/SEQ blank below
       * on the same flag. STORED is a global reading count and stays. */
      (void)snprintf(row, sizeof row, "STATE   --");
   } else if (m->bonded) {
      (void)snprintf(row, sizeof row, "STATE   CONNECTED");
   } else {
      /* TRUNCATE the status so the ENTIRE row fits the column at the normal
       * font -- rather than shrinking the whole block to the widest possible
       * status. The overhead is measured EXACTLY ("STATE   " + "  " + the
       * actual advert-count digits + " ADV"), so the row can never overflow and
       * clip. */
      char advs[16];
      (void)snprintf(advs, sizeof advs, "%u", m->adv_total);
      /* Columns that fit from x (= cx + 2*sc) to the right edge -- NOT the full
       * cw, or the 2*sc left margin makes the row overrun and clip. */
      int fit      = (sc > 0) ? (cw - (4 * sc)) / (6 * sc) : UI_COLS;
      int overhead = 8 + 2 + str_len(advs) + 4; /* "STATE   " + "  N ADV" */
      int budget   = fit - overhead;
      if (budget < 0)
         budget = 0;
      if (budget > UI_COLS)
         budget = UI_COLS;
      char st[UI_COLS + 1];
      str_snapshot(st, sizeof st, m->status ? m->status : "");
      if (str_len(st) > budget)
         st[budget] = '\0';
      (void)snprintf(row, sizeof row, "STATE   %s  %s ADV", st, advs);
   }
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "STORED  %d readings", m->stored);
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   if (!m->has_cgm) {
      /* CGM-only row, blanked when no CGM exists (see STATE above). The PRED
       * row is blanked on the same flag further down. */
      draw_str(px, fb, x, y, sc, "SESSION --", col);
   } else if (m->have_reading) {
      long ss = m->session_seconds;
      /* The PRIMARY CGM's own wear budget (per-device: user override, DIS
       * model, or type default -- see sensor_wear_seconds), and whether it
       * has delivered yet (warmup detection). */
      long len   = 15L * 86400;
      long plast = -1;
      for (int k = 0; k < m->nsensors; k++)
         if (m->sensors[k].primary && m->sensors[k].kind == KIND_CGM) {
            if (m->sensors[k].wear_len > 0)
               len = m->sensors[k].wear_len;
            plast = m->sensors[k].last;
            break;
         }
      (void)plast;
      if (m->sess_state == SENSOR_STATE_WARMUP ||
          (m->sess_state == 0 && ss > 0 && ss < SENSOR_WARMUP_S)) {
         /* The primary is mid-warmup, per the SENSOR'S OWN state byte (or
          * the clock heuristic before any 4e answered). Count the warmup
          * down to the second (the live clock from driver_get_session is
          * what makes this match the official reader). Warmup readings are
          * recorded and shown as the big number meanwhile. */
         long r = SENSOR_WARMUP_S - ss;
         if (r < 0)
            r = 0;
         (void)snprintf(row, sizeof row, "SESSION WARMUP %ld:%02ld LEFT",
                        r / 60, r % 60);
         draw_str(px, fb, x, y, sc, row, 0xFF00CCFF);
      } else {
         long left = len - ss;
         (void)snprintf(row, sizeof row, "SESSION %ldD %ldH   ", ss / 86400,
                        (ss % 86400) / 3600);
         draw_str(px, fb, x, y, sc, row, col);
         /* The countdown is drawn separately so imminence can carry colour: the
          * last day counts in hours and minutes (a bare "0D" reads as already
          * over), in YELLOW, turning RED inside the final two hours. Past the
          * nominal end the sensor still runs for SENSOR_GRACE_S, so the row
          * says exactly that -- GRACE and what is left of it -- rather than the
          * old dead-end "LEFT 0D 0H"; after the grace too, ENDED. */
         char lrow[48];
         uint32_t lcol = col;
         long grace    = left + SENSOR_GRACE_S; /* time to the hard stop */
         if (m->sess_state == SENSOR_STATE_ENDED) {
            /* The sensor SAID the session is over -- its own state byte,
             * not arithmetic on a wear budget. The one allowed ENDED. */
            (void)snprintf(lrow, sizeof lrow, "ENDED");
            lcol = 0xFF4466FF;
         } else if (left <= 0) {
            /* Past the nominal end the sensor still runs for SENSOR_GRACE_S,
             * so count THAT down -- and past the grace too, keep counting into
             * the NEGATIVE rather than switching to a dead-end word: the sign
             * says "past the hard stop" while still showing by how much, which
             * stays honest even when the wear budget is set wrong for a sensor
             * that is visibly alive. Minutes-only inside the last hour, so
             * the boundary never renders as the nonsense "-0H 0M". */
            long ag        = (grace < 0) ? -grace : grace;
            const char *sg = (grace <= -60) ? "-" : "";
            if (ag >= 3600)
               (void)snprintf(lrow, sizeof lrow, "GRACE %s%ldH %ldM", sg,
                              ag / 3600, (ag % 3600) / 60);
            else
               (void)snprintf(lrow, sizeof lrow, "GRACE %s%ldM", sg, ag / 60);
            lcol = (grace < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
         } else if (left < 86400) {
            (void)snprintf(lrow, sizeof lrow, "LEFT %ldH %ldM", left / 3600,
                           (left % 3600) / 60);
            lcol = (left < 2L * 3600) ? 0xFF4466FF : 0xFF00CCFF;
         } else {
            (void)snprintf(lrow, sizeof lrow, "LEFT %ldD %ldH", left / 86400,
                           (left % 86400) / 3600);
         }
         draw_str(px, fb, x + (str_len(row) * 6 * sc), y, sc, lrow, lcol);
      }
   } else {
      /* No reading yet: if the primary is in its warmup window, SAY so with
       * the minutes left -- an unexplained "--" for the first half hour of a
       * new sensor reads as broken, and warmup is the one wait that is by
       * design. */
      long wpair = 0;
      for (int k = 0; k < m->nsensors; k++)
         if (m->sensors[k].primary && m->sensors[k].kind == KIND_CGM) {
            if (m->sensors[k].last == 0)
               wpair = m->sensors[k].paired;
            break;
         }
      if (wpair > 0 && m->now - wpair < SENSOR_WARMUP_S) {
         (void)snprintf(row, sizeof row, "SESSION WARMUP ~%ldM LEFT",
                        (wpair + SENSOR_WARMUP_S - m->now) / 60);
         draw_str(px, fb, x, y, sc, row, 0xFF00CCFF);
      } else {
         (void)snprintf(row, sizeof row, "SESSION --");
         draw_str(px, fb, x, y, sc, row, col);
      }
   }
   y += lh;
   /* PRED/SEQ blanks when there is no CGM (block is CGM-only) OR no reading
    * yet -- one condition, so the two identical blank bodies are one. */
   if (m->has_cgm && m->have_reading) {
      /* predicted is a 10-bit field; 0x3ff (1023) is the sensor's "no
       * prediction" sentinel and no real value exceeds Dexcom's 400 mg/dL cap
       * -- show "--" rather than the raw sentinel. SEQ is still valid. */
      if (m->predicted <= 0 || m->predicted > 400) {
         (void)snprintf(row, sizeof row, "PRED    --      SEQ %d", m->sequence);
      } else {
         char pv[12];
         fmt_glu(m->predicted, m->units, pv, sizeof pv);
         (void)snprintf(row, sizeof row, "PRED    %s %s   SEQ %d", pv,
                        UI_LBL(m->units), m->sequence);
      }
   } else {
      (void)snprintf(row, sizeof row, "PRED    --      SEQ --");
   }
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   /* The STATE..PRED block describes the PRIMARY CGM, so a tap anywhere in it
    * is a shortcut straight to that device's menu (via ACT_MENU -> MA_SENSOR).
    */
   for (int k = 0; k < m->nsensors; k++)
      if (m->sensors[k].primary && m->sensors[k].kind == KIND_CGM) {
         add_hit(h, cx, info_y0, cw, y - info_y0, ACT_MENU, MA_SENSOR + k);
         break;
      }
   /* A big '+' in the empty space right of the table: the ADD entry point
    * (new device / log insulin) reachable without a trip through SETTINGS.
    * Recorded AFTER the block shortcut above -- ui_hit takes the LAST box, so
    * the '+' wins inside its own square and the shortcut keeps the rest. */
   {
      int psc = 3 * sc;
      int pw  = 6 * psc;
      int pxx = cx + cw - pw - (2 * sc);
      int pyy = info_y0 + (((y - info_y0) - (7 * psc)) / 2);
      draw_str(px, fb, pxx, pyy, psc, "+", 0xFFCCCCCC);
      add_hit(h, pxx - (3 * sc), info_y0, pw + (5 * sc), y - info_y0, ACT_MENU,
              MA_ADD_OPEN);
   }

   /* rolling stats table: TIR / AVG / A1C across 1D/3D/7D/30D/90D */
   char tc[5][8];
   char ac[5][8];
   char hc[5][16]; /* "%d.%d" of an unbounded A1C estimate */
   for (int i = 0; i < 5; i++) {
      if (m->stat[i].have) {
         (void)snprintf(tc[i], sizeof tc[i], "%d", m->stat[i].tir);
         fmt_glu(m->stat[i].avg, m->units, ac[i], sizeof ac[i]);
         /* ADAG estimate: A1C% = (avg_mg/dL + 46.7) / 28.7, in tenths. */
         int te = ((100 * m->stat[i].avg) + 4670 + 143) / 287;
         (void)snprintf(hc[i], sizeof hc[i], "%d.%d", te / 10, te % 10);
      } else {
         (void)snprintf(tc[i], sizeof tc[i], "--");
         (void)snprintf(ac[i], sizeof ac[i], "--");
         (void)snprintf(hc[i], sizeof hc[i], "--");
      }
   }
   y += 7 * sc;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "", "1D", "3D",
                  "7D", "30D", "90D", "");
   draw_str(px, fb, x, y, sc, row, 0xFF888888);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "TIR", tc[0],
                  tc[1], tc[2], tc[3], tc[4], "%");
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "AVG", ac[0],
                  ac[1], ac[2], ac[3], ac[4], UI_LBL(m->units));
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;
   (void)snprintf(row, sizeof row, "%-4s%-5s%-5s%-5s%-5s%-5s%s", "A1C", hc[0],
                  hc[1], hc[2], hc[3], hc[4], "%");
   draw_str(px, fb, x, y, sc, row, col);
   y += lh;

   /* alarm banner: STALE, else LOW/HIGH if the reading is fresh */
   const char *msg = 0;
   uint32_t c      = 0;
   if (m->disc_alarmed) {
      msg = "STALE";
      /* A banner-only colour, for the same visibility-check reason as LOW. */
      c = 0xFF00D0FF;
   } else if (m->now - m->t <= 360) {
      if (m->glu < m->alarm_low) {
         msg = "LOW";
         /* Deliberately NOT glu_color's red (0xFF0000FF): sharing that value
          * made the offline visibility check vacuous, because the big number
          * is drawn in it too whenever glu < 50 -- so "the banner is visible"
          * passed while the banner was entirely off-screen. A banner-only
          * colour is what makes that assertion mean something. */
         c = 0xFF2020E0;
      } else if (m->glu > m->alarm_high) {
         msg = "HIGH";
         /* Banner-only, like LOW. Sharing glu_color's orange is what made the
          * LOW visibility assertion vacuous for five review rounds -- the
          * check passed on the big number while the banner was off-screen.
          * HIGH is safe today only by the coincidence that a high reading puts
          * the number in the white band; do not rely on that. */
         c = 0xFF20A0FF;
      }
   }
   if (msg) {
      int msc = 5 * sc;
      int w   = str_len(msg) * 6 * msc;
      int mx  = cx + ((cw - w) / 2);
      if (mx < cx + (2 * sc))
         mx = cx + (2 * sc);
      y += (7 * sc) + (9 * sc);
      draw_str(px, fb, mx, y, msc, msg, c);
   }

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
   (void)snprintf(line, sizeof line, "ADV %u  DX %d", (m->adv_total / 10) * 10,
                  m->ndev);
   draw_str(px, fb, 2 * sc, y, sc, line, 0xFFFFFFFF);
   y += 9 * sc;

   if (m->ndev > 0) {
      y += 3 * sc;
      draw_str(px, fb, 2 * sc, y, sc, "SENSORS", 0xFF888888);
      y += 10 * sc;
      for (int i = 0; i < m->ndev; i++) {
         char dl[48];
         (void)snprintf(dl, sizeof dl, "%-8s %4d %s", m->devs[i].name,
                        m->devs[i].rssi, m->devs[i].mac);
         draw_str(px, fb, 2 * sc, y, sc, dl, 0xFFCCCCCC);
         y += 9 * sc;
      }
   }
}

/* Compose the main screen: two columns in landscape, stacked in portrait. */
static void render_main(struct ANativeWindow_Buffer *fb, const struct screen *m,
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
    * budget is roughly 340 units of sc (big number + trend rows + plot + alarm
    * row + the info/stats block), so on any window shorter than ~340*sc the
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
   if (m->glu >= 0 || m->nsensors > 0) {
      if (landscape) {
         int gw   = 2 * 6 * sc;
         int cwid = (fb->width - gw) / 2;
         render_glucose(fb, m, h, 0, cwid, y, sc, fb->height);
         render_info(fb, m, h, cwid + gw, fb->width - cwid - gw, y, sc);
      } else {
         y = render_glucose(fb, m, h, 0, fb->width, y, sc, 0);
         render_info(fb, m, h, 0, fb->width, y, sc);
      }
   } else {
      /* Records its own settings target -- see render_noreading. */
      render_noreading(fb, m, h, y, sc);
   }
}

/* ---- settings menu (portrait table; rows carry menu_action codes) ---- */

static const char *ui_orient_lbl[] = {"PORTRAIT", "LANDSCAPE", "GRAVITY",
                                      "SYSTEM"};
static const char *ui_disc_lbl[]   = {"OFF", "10 MIN", "30 MIN", "60 MIN"};
static const char *ui_perm_lbl[]   = {"BT SCAN", "BT CONNECT", "NOTIFY"};

/* App-Standby bucket -> short label. */
static const char *ui_bucket_label(int b)
{
   if (b <= 0)
      return "?";
   if (b <= 5)
      return "EXEMPT";
   if (b <= 10)
      return "ACTIVE";
   if (b <= 20)
      return "WORKING";
   if (b <= 30)
      return "FREQUENT";
   if (b <= 40)
      return "RARE";
   return "RESTRICTED";
}

/* One menu row: name left, value right; records a full-width tap target
 * carrying the menu_action `code` (code < 0 = read-only, no target). */
static void menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, int lh, const char *name, const char *value,
                     uint32_t valcol, int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   int vw = str_len(value) * 6 * sc;
   draw_str(px, fb, rx - vw, y, sc, value, valcol);
   if (code >= 0)
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, code);
}

/* A framed, centred, tappable BUTTON with generous vertical padding -- the one
 * consistent button style used across the menus (NOT the keypads). Frame is
 * grey; the label carries the colour (e.g. red for a destructive action).
 * Returns the y just past the button. */
static int menu_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                       int y, int w, int sc, const char *label, uint32_t col,
                       int action)
{
   uint32_t *px = fb->bits;
   int bh       = 15 * sc; /* label glyph is 7*sc -> ~4*sc padding each side */
   int lw       = str_len(label) * 6 * sc;
   int lhh      = 7 * sc;
   draw_frame(px, fb, x, y, w, bh, 0xFF888888);
   draw_str(px, fb, x + ((w - lw) / 2), y + ((bh - lhh) / 2), sc, label, col);
   add_hit(h, x, y, w, bh, ACT_MENU, action);
   return y + bh;
}

static void render_settings(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Bounded by height as well as width -- see ui_settings_scale. */
   int sc  = ui_settings_scale(fb->width, fb->height);
   int tsc = 2 * sc;
   int lh  = 16 * sc; /* generous pitch: a blank line between rows */
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   /* title with a right-aligned X to close */
   draw_str(px, fb, x, y, tsc, "SETTINGS", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* generous close target: title + blank line + DISPLAY header */
   add_hit(h, 0, y - (3 * sc), fb->width, 3 * lh, ACT_MENU, MA_CLOSE);
   y += 2 * lh;

   draw_str(px, fb, x, y, sc, "DISPLAY", 0xFF888888);
   y += lh;
   menu_row(fb, h, y, sc, lh, "ORIENTATION",
            ui_orient_lbl[(unsigned)m->orient & 3U], 0xFFFFFFFF, MA_ORIENT);
   y += lh;
   menu_row(fb, h, y, sc, lh, "UNITS", m->units ? "MMOL/L" : "MG/DL",
            0xFFFFFFFF, MA_UNITS);
   y += lh;
   /* ALWAYS ON holds the screen awake while the app is open (the historical
    * behaviour); SYSTEM lets the normal display timeout apply. */
   menu_row(fb, h, y, sc, lh, "SCREEN", m->screen_on ? "ALWAYS ON" : "SYSTEM",
            0xFFFFFFFF, MA_SCREEN);
   y += lh;
   char pmv[20];
   char pmvv[8];
   fmt_glu(m->plot_max, m->units, pmvv, sizeof pmvv);
   (void)snprintf(pmv, sizeof pmv, "%s %s", pmvv, UI_LBL(m->units));
   menu_row(fb, h, y, sc, lh, "PLOT MAX", pmv, 0xFFFFFFFF, MA_PLOTMAX);
   y += lh;
   /* REMOTE: one summary row opening the submenu, like PERMISSIONS below. The
    * value is the push state, so whether datapoints are leaving the phone is
    * visible without opening the submenu. */
   menu_row(fb, h, y, sc, lh, "REMOTE", m->remote_on ? "ON" : "OFF",
            m->remote_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_REMOTE_OPEN);
   y += lh;
   /* PERMISSIONS lives at the END of DISPLAY, as one summary row -- green OK
    * when everything a CGM needs is granted, red CHECK otherwise -- opening
    * the full submenu. The eight raw permission rows moved off this
    * unscrollable screen so the DEVICES list gets the space. */
   {
      int ok = m->perm[0] && m->perm[1] && m->perm[2] && m->batt_ok &&
               !m->bg_restricted;
      menu_row(fb, h, y, sc, lh, "PERMISSIONS", ok ? "OK" : "CHECK",
               ok ? 0xFF33FF88 : 0xFF4466FF, MA_PERMS_OPEN);
      y += 2 * lh;
   }

   draw_str(px, fb, x, y, sc, "ALARM", 0xFF888888);
   y += lh;
   menu_row(fb, h, y, sc, lh, "SOUND", m->sound_on ? "ON" : "OFF", 0xFFFFFFFF,
            MA_SOUND);
   y += lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->vib_on ? "ON" : "OFF", 0xFFFFFFFF,
            MA_VIB);
   y += lh;
   menu_row(fb, h, y, sc, lh, "DISCONNECT", ui_disc_lbl[(unsigned)m->disc & 3U],
            0xFFFFFFFF, MA_DISC);
   y += lh;
   menu_row(fb, h, y, sc, lh, "NEW DATAPOINT", m->newdata_beep ? "BEEP" : "OFF",
            0xFFFFFFFF, MA_NEWDATA);
   y += 2 * lh;

   /* SENSORS: one row per configured sensor, then the add action. The old
    * single-sensor block moved into the per-sensor screen, which is what frees
    * the space this list needs. Never scrolls -- see ui_sensor_capacity(). */
   draw_str(px, fb, x, y, sc, "DEVICES", 0xFF888888);
   y += lh;
   int cap = ui_sensor_capacity(fb->width, fb->height);
   if (cap < UI_MIN_SLOTS) {
      /* Too short a screen to show even the minimum honestly. Say so rather
       * than silently truncating, which would read as "these are all of them".
       */
      draw_str(px, fb, x, y, sc, "SCREEN TOO SHORT", 0xFF4466FF);
      y += lh;
      draw_str(px, fb, x, y, sc, "FOR SENSOR LIST", 0xFF4466FF);
      return;
   }
   int shown = 0;
   int nold  = 0;
   for (int i = 0; i < m->nsensors; i++)
      if (m->sensors[i].old)
         nold++;
   for (int i = 0; i < m->nsensors && shown < cap; i++) {
      const struct ui_sensor *s = &m->sensors[i];
      if (s->old) /* disconnected: lives under OLD DEVICES, not here */
         continue;
      shown++;
      char val[28]; /* status[12] + ' ' + ago[12], with room to spare */
      char ago[12];
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
         if (warm_clk) {
            long r = SENSOR_WARMUP_S - s->session_seconds;
            (void)snprintf(val, sizeof val, "WARMUP %d:%02d", (int)(r / 60),
                           (int)(r % 60));
         } else {
            long r = (s->paired + SENSOR_WARMUP_S - m->now) / 60;
            (void)snprintf(val, sizeof val, "WARMUP ~%dM", (int)r);
         }
      } else {
         (void)snprintf(val, sizeof val, "%s %s", s->status, ago);
      }
      /* '>' marks the primary -- the sensor that owns the big number. It goes
       * in the label rather than left of the row, where it overlapped. The
       * blank cell after it holds this device's plot marker (its shape, colour
       * and size), so the list answers "which trace is which" at a glance. */
      /* Holds the primary marker (1), the reserved glyph cell (2) plus a full
       * label (sizeof s->label, which grew to 20 for the long OneTouch default
       * names) plus the terminator. Undersizing it truncated the MAC tail that
       * tells two meters apart. */
      char name[3 + sizeof s->label];
      (void)snprintf(name, sizeof name, "%s  %s", s->primary ? ">" : " ",
                     s->label);
      menu_row(fb, h, y, sc, lh, name, val,
               s->connected ? 0xFF33FF88 : 0xFFAAAAAA, MA_SENSOR + i);
      if (s->marker != MARK_HIDE) { /* hidden-from-plot draws no glyph */
         /* Centred in the reserved cell (text starts at 4*sc; the cell is the
          * second character, 6*sc wide). Radius follows the configured SIZE,
          * clamped to the cell so a large marker cannot strike the label. */
         int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                           (4 * sc) + (9 * sc), y + (3 * sc), gr, s->marker,
                           ui_sensor_color(s->color));
      }
      y += lh;
   }
   if (m->pend_type > 0) {
      /* An ARMED pairing: registered intent, no sensor on the air yet. The
       * row is the visible promise that the code was accepted and the app is
       * watching -- and the tap is the way to change one's mind. */
      char pn[24];
      (void)snprintf(pn, sizeof pn, " %s", sensor_type_name(m->pend_type));
      menu_row(fb, h, y, sc, lh, pn, "PENDING...", 0xFF00CCFF, MA_PEND_CANCEL);
      y += lh;
   }
   int nlive = m->nsensors - nold;
   if (shown < nlive) { /* never claim to have listed them all */
      char more[32];
      int nmore = nlive - shown;
      if (nmore > 99)
         nmore = 99; /* bounded by MAX_SLOTS in practice */
      (void)snprintf(more, sizeof more, "%d MORE NOT SHOWN", nmore);
      draw_str(px, fb, x, y, sc, more, 0xFF4466FF);
      y += lh;
   }
   /* OLD DEVICES: DISCONNECTED devices. Each keeps its full slot, so the row
    * opens the SAME per-device menu (state EXPIRED). Only shown when there is
    * at least one, so it never adds noise on a fresh install. */
   if (nold > 0) {
      char od[32];
      (void)snprintf(od, sizeof od, "OLD DEVICES (%d)", nold);
      menu_row(fb, h, y, sc, lh, od, ">", 0xFFAAAAAA, MA_OLDDEV_OPEN);
      y += lh;
   }
   int bw = fb->width - (2 * x);
   if (nlive < UI_MAX_SLOTS) {
      /* A real framed button, like SYNC NOW / FORGET DEVICE, not a plain row.
       */
      y += lh; /* separate it from the device list above */
      y = menu_button(fb, h, x, y, bw, sc, "ADD NEW DEVICE", 0xFFFFFFFF,
                      MA_ADDSENSOR);
   }
   /* EXPORT DATA: build the combined CSV and open the system share sheet. */
   y += lh;
   menu_button(fb, h, x, y, bw, sc, "EXPORT DATA", 0xFFFFFFFF, MA_EXPORT);
}

/* ---- permissions + background controls (opened from SETTINGS) ---- */

static void render_perms(struct ANativeWindow_Buffer *fb,
                         const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_PERMS_BACK);
   y += 2 * lh;

   for (int i = 0; i < 3; i++) {
      int g = m->perm[i];
      menu_row(fb, h, y, sc, lh, ui_perm_lbl[i], g ? "GRANTED" : "DENIED",
               g ? 0xFF33FF88 : 0xFF4466FF, MA_PERM + i);
      y += lh;
   }
   menu_row(fb, h, y, sc, lh, "BATTERY",
            m->batt_ok ? "UNRESTRICTED" : "OPTIMIZED",
            m->batt_ok ? 0xFF33FF88 : 0xFF4466FF, MA_BATTERY);
   y += lh;
   menu_row(fb, h, y, sc, lh, "STANDBY", ui_bucket_label(m->standby_bucket),
            (m->standby_bucket > 0 && m->standby_bucket <= 20) ? 0xFF33FF88
                                                               : 0xFFAA8844,
            -1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "BG EXEC",
            m->bg_restricted ? "RESTRICTED" : "ALLOWED",
            m->bg_restricted ? 0xFF4466FF : 0xFF33FF88, MA_BGEXEC);
}

/* ---- remote push (opened from SETTINGS) ---- */

static void render_remote(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 14);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "REMOTE", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_REMOTE_BACK);
   y += 2 * lh;

   const char *ip = (m->remote_ip && m->remote_ip[0]) ? m->remote_ip : 0;
   /* PUSH reflects what will actually happen: enabling without an address set
    * shows WAITING (amber), not ON -- nothing leaves the phone until the IP
    * exists, and pretending otherwise would be a silent lie. */
   const char *pv = "OFF";
   uint32_t pc    = 0xFFFFFFFF;
   if (m->remote_on && ip) {
      pv = "ON";
      pc = 0xFF33FF88;
   } else if (m->remote_on) {
      pv = "NO ADDRESS";
      pc = 0xFFAA8844;
   }
   menu_row(fb, h, y, sc, lh, "PUSH", pv, pc, MA_REMOTE_TOGGLE);
   y += lh;
   menu_row(fb, h, y, sc, lh, "IP ADDRESS", ip ? ip : "NOT SET",
            ip ? 0xFFFFFFFF : 0xFFAAAAAA, MA_REMOTE_IP);
   y += lh;
   char pt[8];
   (void)snprintf(pt, sizeof pt, "%d", m->remote_port);
   menu_row(fb, h, y, sc, lh, "PORT", pt, 0xFFFFFFFF, MA_REMOTE_PORT);
   y += 2 * lh;

   draw_str(px, fb, x, y, sc, "Each new datapoint is sent", 0xFF888888);
   y += lh;
   draw_str(px, fb, x, y, sc, "to this server as plain,", 0xFF888888);
   y += lh;
   draw_str(px, fb, x, y, sc, "unencrypted HTTP.", 0xFF888888);
}

/* ---- per-sensor screen: attributes above, actions below ---- */

static void render_sensor(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
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
   int sc  = ui_fit_scale(fb->width, fb->height, 28);
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_SENSOR_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];

   draw_str(px, fb, x, y, tsc, s->label, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_SENSOR_BACK);
   y += 2 * lh;

   /* Identity: type + name (+ PRIMARY for a CGM), no section title. */
   menu_row(fb, h, y, sc, lh, "TYPE", sensor_disp_name(s->type), 0xFFFFFFFF,
            -1);
   y += lh;
   menu_row(fb, h, y, sc, lh, "NAME", s->label, 0xFFFFFFFF, MA_LABEL);
   y += lh;
   if (s->kind == KIND_CGM && !s->old) {
      /* PRIMARY only for a LIVE CGM -- a disconnected one cannot own the big
       * number. */
      menu_row(fb, h, y, sc, lh, "PRIMARY", s->primary ? "YES" : "NO",
               s->primary ? 0xFF33FF88 : 0xFFFFFFFF, MA_PRIMARY);
      y += lh;
   }
   if (s->kind == KIND_CGM) {
      /* WEAR: the nominal budget the countdown judges against. Dexcom sells
       * 10- and 15-day G7s that are indistinguishable on the air, so when the
       * auto-resolution guesses wrong this row is the correction. */
      char wd[24];
      (void)snprintf(wd, sizeof wd, "%d DAYS", (int)(s->wear_len / 86400));
      menu_row(fb, h, y, sc, lh, "WEAR", wd, 0xFFFFFFFF, MA_WEAR);
      y += lh;
   }
   /* One MARKER row -- shows the ACTUAL glyph (in the device's colour), not a
    * name; shape + size + colour all live in its combined menu. */
   draw_str(px, fb, x, y, sc, "MARKER", 0xFFCCCCCC);
   if (s->marker == MARK_HIDE) {
      int lw = str_len("OFF") * 6 * sc;
      draw_str(px, fb, rx - lw, y, sc, "OFF", 0xFFAAAAAA);
   } else {
      /* Glyph reflects the configured SIZE too (same scaling as the plot). */
      int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
      if (gr < sc)
         gr = sc;
      if (gr > 5 * sc)
         gr = 5 * sc;
      plot_marker_glyph(px, fb->stride, fb->width, fb->height, rx - (6 * sc),
                        y + (3 * sc), gr, s->marker, ui_sensor_color(s->color));
   }
   add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, MA_MARKER);
   y += lh;

   /* --- read-only --- */
   y += lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, x, y, sc, "STATUS", 0xFF888888);
   y += lh;
   /* A disconnected device reads EXPIRED (red); otherwise its live status. */
   uint32_t stcol = 0xFFAAAAAA;
   if (s->old)
      stcol = 0xFF4466FF;
   else if (s->connected)
      stcol = 0xFF33FF88;
   menu_row(fb, h, y, sc, lh, "STATE", s->old ? "EXPIRED" : s->status, stcol,
            -1);
   y += lh;
   {
      char rs[16]; /* link RSSI (moved off the main screen). No age here -- LAST
                    * SYNC sits right beside it and carries the time. */
      if (s->rssi_ok && s->rssi_t > 0)
         (void)snprintf(rs, sizeof rs, "%d DB", s->rssi);
      else
         (void)snprintf(rs, sizeof rs, "--");
      menu_row(fb, h, y, sc, lh, "SIGNAL STRENGTH", rs, 0xFFFFFFFF, -1);
      y += lh;
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
      menu_row(fb, h, y, sc, lh, "LAST SEEN", val, 0xFFFFFFFF, -1);
      y += lh;
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
         menu_row(fb, h, y, sc, lh, "LAST DATA", val, 0xFFFFFFFF, -1);
         y += lh;
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
      menu_row(fb, h, y, sc, lh, "STARTED", when, 0xFFFFFFFF, -1);
      y += lh;
      if (len > 0) {
         /* ENDS shows the absolute instant only; REMAINING (below) carries the
          * relative countdown, mirroring STARTED/ELAPSED. */
         if (have_session)
            fmt_date(began + len, m->tz_off, when, sizeof when);
         else
            (void)snprintf(when, sizeof when, "--");
         menu_row(fb, h, y, sc, lh, "ENDS", when,
                  (have_session && began + len < m->now) ? 0xFF4466FF
                                                         : 0xFFFFFFFF,
                  -1);
         y += lh;
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
      menu_row(fb, h, y, sc, lh, "ELAPSED", b, 0xFFFFFFFF, -1);
      y += lh;
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
         menu_row(fb, h, y, sc, lh, "REMAINING", val, rcol, -1);
         y += lh;
      }
      if (s->predicted > 0) {
         char pv[12];
         fmt_glu(s->predicted, m->units, pv, sizeof pv);
         (void)snprintf(b, sizeof b, "%s %s", pv, UI_LBL(m->units));
         menu_row(fb, h, y, sc, lh, "PRED", b, 0xFFFFFFFF, -1);
         y += lh;
      }
      (void)snprintf(b, sizeof b, "%d", s->sequence);
      menu_row(fb, h, y, sc, lh, "SEQ", b, 0xFFFFFFFF, -1);
      y += lh;
   }
   y += lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, x, y, sc, "DEVICE INFO", 0xFF888888);
   y += lh;
   if (s->code[0]) {
      menu_row(fb, h, y, sc, lh, "CODE", s->code, 0xFFFFFFFF, -1);
      y += lh;
   }
   menu_row(fb, h, y, sc, lh, "MAC", s->mac[0] ? s->mac : "--", 0xFFFFFFFF, -1);
   y += lh;
   if (s->serial[0]) {
      menu_row(fb, h, y, sc, lh, "SN", s->serial, 0xFFFFFFFF, -1);
      y += lh;
   }
   if (s->model[0]) {
      menu_row(fb, h, y, sc, lh, "SW", s->model, 0xFFFFFFFF, -1);
      y += lh;
   }
   if (s->fw[0]) {
      menu_row(fb, h, y, sc, lh, "FW", s->fw, 0xFFFFFFFF, -1);
      y += lh;
   }
   /* RESCALE: the active multiplicative correction as a signed percentage, or
    * (NONE). Tapping opens the value keypad, or -- if already active -- the
    * CHANGE / STOP screen. Sits just above LAST CAL. */
   if (s->kind == KIND_CGM) {
      char rv[32]; /* "PENDING " + value(<=11) + ' ' + unit(<=6) + NUL */
      uint32_t rcol = 0xFFFFFFFF;
      if (s->rescale_pending > 0) {
         char gv[12];
         fmt_glu(s->rescale_pending, m->units, gv, sizeof gv);
         (void)snprintf(rv, sizeof rv, "PENDING %s %s", gv, UI_LBL(m->units));
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
      menu_row(fb, h, y, sc, lh, "RESCALE", rv, rcol, MA_RESCALE_OPEN);
      y += lh;
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
         fmt_glu(s->cal_pending, m->units, gv, sizeof gv);
         (void)snprintf(cv, sizeof cv, "PENDING %s %s", gv, UI_LBL(m->units));
         ccol = 0xFF44CCFF; /* amber: in progress */
      } else if (s->cal_t > 0) {
         char cd[20];
         fmt_date(s->cal_t, m->tz_off, cd, sizeof cd);
         if (s->cal_state == CAL_ST_APPLIED) {
            fmt_glu(s->cal_mgdl, m->units, gv, sizeof gv);
            (void)snprintf(cv, sizeof cv, "%s %s %s", cd, gv, UI_LBL(m->units));
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
      /* While a calibration is queued, the row itself is a shortcut into the
       * CAL PENDING menu (REPLACE / DELETE); otherwise it is display-only. */
      menu_row(fb, h, y, sc, lh, "LAST CAL", cv, ccol,
               s->cal_pending > 0 ? MA_CAL_OPEN : -1);
      y += lh;
   }

   /* --- actions as framed buttons, kept together at the bottom --- */
   int bw = fb->width - (2 * x);
   y += 2 * lh;
   if (s->old) {
      /* A DISCONNECTED device: no live actions (calibrate/sync need a link).
       * RECONNECT revives it -- sensible for a sensor pulled BEFORE it expired
       * (still within its wear window). The handler shows a confirmation first
       * when the sensor is already expired, since reconnecting a dead sensor
       * rarely makes sense. */
      menu_button(fb, h, x, y, bw, sc, "RECONNECT", 0xFF00FF00, MA_RECONNECT);
      return;
   }
   if (s->kind == KIND_CGM)
      y = menu_button(fb, h, x, y, bw, sc, "CALIBRATION", 0xFFFFFFFF,
                      MA_CAL_OPEN);
   else
      y = menu_button(fb, h, x, y, bw, sc, "SYNC NOW", 0xFFFFFFFF, MA_SYNC);
   /* DISCONNECT is destructive: red, and well clear of the action above it
    * rather than one fat finger below. It only opens a confirmation. */
   y += 2 * lh;
   menu_button(fb, h, x, y, bw, sc, "DISCONNECT", 0xFF0000FF, MA_FORGET);
}

/* ---- calibration confirmation ---- */

static void render_cal(struct ANativeWindow_Buffer *fb, const struct screen *m,
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CAL_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];

   /* Confirmation for the value just typed on the keypad. The write is the most
    * consequential in the app, so it happens ONLY on CONFIRM below. */
   draw_str(px, fb, x, y, tsc, "CONFIRM", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CAL_BACK);
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->cal_pending, m->units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->units));
      menu_row(fb, h, y, sc, lh, "CALIBRATE TO", v, 0xFF33FF88, -1);
      y += lh;
   }
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_CAL_BACK);
   y += 3 * lh; /* wide gap so CONFIRM is deliberate */
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88, MA_CAL_ENTER);
}

/* Shown when CALIBRATION is opened while one is still queued: REPLACE it with a
 * new value, or CANCEL (discard) it. X leaves the queue untouched. */
static void render_calpend(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   /* X / title-bar tap leaves the pending calibration in place. */
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CAL_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];
   draw_str(px, fb, x, y, tsc, "CAL PENDING", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(s->cal_pending, m->units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->units));
      menu_row(fb, h, y, sc, lh, "QUEUED", v, 0xFF44CCFF, -1);
      y += lh;
   }
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "REPLACE", 0xFFFFFFFF, MA_CAL_REPLACE);
   y += 3 * lh; /* wide gap so DELETE (discard) is deliberate */
   /* "DELETE", not "CANCEL": CANCEL reads as "do nothing", but this button
    * DISCARDS the queued calibration. The X in the title bar is the no-op. */
   menu_button(fb, h, x, y, bw, sc, "DELETE", 0xFF0000FF, MA_CAL_CANCEL);
}

/* Format a rescale factor (permille) as a signed percentage, e.g. 1040 ->
 * +4.0%. */
static void fmt_rescale_pct(int pm, char *out, int n)
{
   int d = pm - 1000; /* tenths of a percent */
   int a = (d < 0) ? -d : d;
   (void)snprintf(out, n, "%c%d.%d%%", (d < 0) ? '-' : '+', a / 10, a % 10);
}

/* Confirm a rescale: shows the target value and the clamped percentage, applied
 * only on CONFIRM. Mirrors render_cal. */
static void render_rescale(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_RESCALE_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];
   draw_str(px, fb, x, y, tsc, "RESCALE", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->rescale_entry, m->units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->units));
      menu_row(fb, h, y, sc, lh, "TARGET", v, 0xFF33FF88, -1);
      y += lh;
   }
   {
      char v[20];
      uint32_t vc = 0xFF44CCFF;
      if (m->rescale_pm == 0) {
         /* No reading yet to compute against -- do not show a bogus 0%. */
         (void)snprintf(v, sizeof v, "ON NEXT READING");
         vc = 0xFFAAAAAA;
      } else {
         fmt_rescale_pct(m->rescale_pm, v, sizeof v);
         /* Beyond +-25% will be REJECTED on CONFIRM -- flag it red. */
         if (m->rescale_pm < 750 || m->rescale_pm > 1250)
            vc = 0xFF4466FF;
      }
      menu_row(fb, h, y, sc, lh, "RESCALE BY", v, vc, -1);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_RESCALE_BACK);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88, MA_RESCALE_ENTER);
}

/* Rescaling already active: CHANGE the value, or STOP. Mirrors render_calpend.
 */
static void render_rescaleact(struct ANativeWindow_Buffer *fb,
                              const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_RESCALE_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];
   draw_str(px, fb, x, y, tsc, "RESCALE ON", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += lh;
   {
      char v[32]; /* "PENDING " + value(<=11) + ' ' + unit(<=6) + NUL */
      if (s->rescale_pending > 0) {
         /* Held, awaiting a reading to compute the factor from. */
         char gv[12];
         fmt_glu(s->rescale_pending, m->units, gv, sizeof gv);
         (void)snprintf(v, sizeof v, "PENDING %s %s", gv, UI_LBL(m->units));
      } else {
         fmt_rescale_pct(s->rescale_pm, v, sizeof v);
      }
      menu_row(fb, h, y, sc, lh, "RESCALING", v, 0xFF44CCFF, -1);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CHANGE", 0xFFFFFFFF,
                        MA_RESCALE_CHANGE);
   y += 3 * lh; /* wide gap so TURN OFF is deliberate */
   /* "TURN OFF" (not STOP/CANCEL): STOP reads like ending the sensor SESSION,
    * and CANCEL like doing nothing -- this turns rescaling off. White, not red:
    * turning rescaling off is not destructive (no data is lost). */
   menu_button(fb, h, x, y, bw, sc, "TURN OFF", 0xFFFFFFFF, MA_RESCALE_STOP);
}

/* ---- forget confirmation ----
 * Forgetting drops the slot only: the provenance row and every reading this
 * sensor produced stay exactly where they are. Saying so here is the point of
 * the screen -- otherwise "FORGET" reads like it deletes the data. */

static void render_forget(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_FORGET_NO);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];

   int rx = fb->width - (4 * sc);
   draw_str(px, fb, x, y, tsc, "DISCONNECT?", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X",
            0xFFFFFFFF); /* close = cancel */
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, s->label, 0xFFFFFFFF);
   y += 2 * lh;
   static const char *const note[] = {
       "STOPS THIS DEVICE AND MOVES",
       "IT TO OLD DEVICES. READINGS",
       "AND HISTORY ARE KEPT.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], 0xFF888888);
      y += lh;
   }
   y += 2 * lh;

   /* Two consistent framed buttons, well separated so they cannot be confused:
    * CANCEL (safe, white) and DISCONNECT (RED). This IS the confirmation step
    * -- MA_FORGET_YES is what actually disconnects (the device becomes an OLD
    * DEVICE; nothing is deleted). */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_FORGET_NO);
   y += 3 * lh; /* wide gap so DISCONNECT is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "DISCONNECT", 0xFF0000FF, MA_FORGET_YES);
}

/* ---- reconnect-an-EXPIRED-device confirmation ----
 * Reconnecting a sensor pulled BEFORE it expired is direct; reconnecting one
 * that has already expired rarely makes sense, so it lands here first. ---- */
static void render_reconf(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_RECON_NO);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];

   draw_str(px, fb, x, y, tsc, "RECONNECT?", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, s->label, 0xFFFFFFFF);
   y += 2 * lh;
   static const char *const note[] = {
       "THIS SENSOR IS EXPIRED.",
       "RECONNECTING RARELY WORKS;",
       "IT WILL JUST WAIT FOREVER.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], 0xFF888888);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_RECON_NO);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "RECONNECT", 0xFF00FF00, MA_RECON_YES);
}

/* ---- pairing confirmation ----
 * Tapping a row in the device list used to commit the pairing on the spot,
 * and commit_pair is consequential: it registers the device and (for a CGM)
 * drops the chosen link's old bond before the J-PAKE. One mis-tap in a list
 * ordered by live RSSI -- rows can reorder under the finger -- did all of
 * that to the wrong device. So the pick only proposes; this screen's explicit
 * YES is what commits, and NO returns to the list with nothing changed. */

static void render_pairconf(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_PAIR_NO);

   char title[24];
   (void)snprintf(title, sizeof title, "PAIR %s?",
                  m->add_type ? m->add_type : "SENSOR");
   draw_str(px, fb, x, y, tsc, title, 0xFFFFFFFF);
   int rx = fb->width - (4 * sc);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF); /* close = NO */
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, m->pair_name ? m->pair_name : "", 0xFFFFFFFF);
   y += lh;
   draw_str(px, fb, x, y, sc, m->pair_mac ? m->pair_mac : "", 0xFF888888);
   y += 2 * lh;

   /* Two consistent framed buttons, well separated so they cannot be
    * confused: NO (safe, white) first, YES (commits, green) below. */
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "NO", 0xFFFFFFFF, MA_PAIR_NO);
   y += 3 * lh; /* wide gap so YES is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "YES", 0xFF00FF00, MA_PAIR_YES);
}

/* ---- CHOOSE PRIMARY CGM: big-number tap with more than one active CGM ----
 * The big number belongs to exactly one sensor by contract, so when several
 * CGMs hold live sessions the tap that used to open SETTINGS instead asks
 * which one should own it. Each row shows the sensor's own newest value and
 * age, so the choice is informed; '>' marks the current owner. */

static void render_primpick(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "PRIMARY CGM", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CLOSE);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "Owns the big number + alarm:", 0xFF888888);
   y += 2 * lh;

   int shown = 0;
   for (int i = 0; i < m->nsensors && i < UI_MAX_SLOTS; i++) {
      const struct ui_sensor *s = &m->sensors[i];
      /* EVERY registered CGM is offered -- including one just paired that
       * has no session and no datapoint yet: making it primary is precisely
       * how the user pre-arms the display for the sensor they are switching
       * to. Only a meter never qualifies. Rows without data say so. */
      if (s->kind != KIND_CGM)
         continue;
      shown++;
      char val[28];
      if (s->sess_state == SENSOR_STATE_ENDED) {
         /* Lifecycle policy: the sensor's own verdict labels the row; its
          * history keeps its colours. Still selectable -- browsing a dead
          * sensor's trace as the headline is legitimate. */
         (void)snprintf(val, sizeof val, "ENDED");
      } else if (s->last > 0) {
         char gv[12];
         char ago[12];
         fmt_glu(s->glu, m->units, gv, sizeof gv);
         fmt_ago(m->now, s->last, ago, sizeof ago);
         (void)snprintf(val, sizeof val, "%s  %s", gv, ago);
      } else if ((s->sess_state == SENSOR_STATE_WARMUP || s->sess_state == 0) &&
                 s->session_seconds > 0 &&
                 s->session_seconds < SENSOR_WARMUP_S) {
         long r = SENSOR_WARMUP_S - s->session_seconds;
         (void)snprintf(val, sizeof val, "WARMUP %d:%02d", (int)(r / 60),
                        (int)(r % 60));
      } else if (s->paired > 0 && m->now - s->paired < SENSOR_WARMUP_S) {
         long wl = (s->paired + SENSOR_WARMUP_S - m->now) / 60;
         (void)snprintf(val, sizeof val, "WARMUP ~%dM", (int)wl);
      } else {
         (void)snprintf(val, sizeof val, "NO DATA");
      }
      /* Same layout as the SETTINGS device list: '>' for the current owner,
       * then a reserved cell showing this device's plot marker (shape,
       * colour, size), so the choice maps to the trace at a glance. */
      char name[3 + sizeof s->label];
      (void)snprintf(name, sizeof name, "%s  %s", s->primary ? ">" : " ",
                     s->label);
      menu_row(fb, h, y, sc, lh, name, val,
               s->primary ? 0xFF33FF88 : 0xFFFFFFFF, MA_PRIM_PICK + i);
      if (s->marker != MARK_HIDE) {
         int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                           (4 * sc) + (9 * sc), y + (3 * sc), gr, s->marker,
                           ui_sensor_color(s->color));
      }
      /* A BLANK LINE between rows: these are consequential taps made
       * one-handed at a glance, so each target gets breathing room. */
      y += 2 * lh;
   }
   if (m->pend_type > 0) {
      /* The ARMED pairing is choosable too: picking it means "the incoming
       * sensor takes the big number the moment it lands". */
      char pn[24];
      (void)snprintf(pn, sizeof pn, "%s  %s PENDING",
                     m->pend_primary ? ">" : " ",
                     sensor_type_name(m->pend_type));
      menu_row(fb, h, y, sc, lh, pn,
               m->pend_primary ? "PRIMARY ON PAIR" : "WAITING", 0xFF00CCFF,
               MA_PRIM_PEND);
      shown++;
   }
   if (shown == 0)
      /* Opening this with no CGM at all is legitimate (the big number is
       * always tappable); say so rather than showing an empty list. */
      draw_str(px, fb, x, y, sc, "No CGM. Add one from +.", 0xFF888888);
}

/* ---- ADD menu: the main-screen '+' lands here ---- */

static void render_addmenu(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
{
   (void)m;
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "ADD ...", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* This menu opens from the MAIN screen, so its X returns there (MA_CLOSE),
    * not into SETTINGS. Generous close target across the title band. */
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CLOSE);
   y += 3 * lh;

   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "NEW DEVICE", 0xFFFFFFFF, MA_ADDSENSOR);
   y += 2 * lh;
   menu_button(fb, h, x, y, bw, sc, "INSULIN", 0xFFFFFFFF, MA_INS_OPEN);
}

/* ---- LOG INSULIN: type / units / date / time, then CONFIRM or DISCARD ---- */

/* One "NAME   - value +" row: the value sits right-aligned with a minus and a
 * plus stepper either side, each a full row-height target two cells wide. */
static void stepper_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                        int sc, int lh, const char *name, const char *val,
                        int minus_code, int plus_code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   /* layout from the right edge: "- <val> +", one blank cell between parts */
   int vw = str_len(val) * 6 * sc;
   int xp = rx - (6 * sc);      /* '+' cell */
   int xv = xp - (6 * sc) - vw; /* value */
   int xm = xv - (2 * 6 * sc);  /* '-' cell */
   draw_str(px, fb, xm, y, sc, "-", 0xFFCCCCCC);
   draw_str(px, fb, xv, y, sc, val, 0xFFFFFFFF);
   draw_str(px, fb, xp, y, sc, "+", 0xFFCCCCCC);
   /* Steppers are the whole half-row each side of the value's centre, so they
    * are unmissable on a phone; the value itself is not a target. */
   int mid = xv + (vw / 2);
   add_hit(h, xm - (6 * sc), y - (3 * sc), mid - (xm - (6 * sc)), lh, ACT_MENU,
           minus_code);
   add_hit(h, mid, y - (3 * sc), (rx - mid) + (4 * sc), lh, ACT_MENU,
           plus_code);
}

static void render_insulin(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "LOG INSULIN", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* X discards -- nothing is written before an explicit CONFIRM. */
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_INS_DISCARD);
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "TYPE", m->ins_type == 1 ? "FAST" : "SLOW",
            0xFFFFFFFF, MA_INS_TYPE);
   y += lh;
   char val[20];
   (void)snprintf(val, sizeof val, "%d U", m->ins_units);
   stepper_row(fb, h, y, sc, lh, "UNITS", val, MA_INS_UMINUS, MA_INS_UPLUS);
   y += lh;
   /* fmt_date renders "MM-DD HH:MM"; the form shows the two halves on their
    * own adjustable lines, both pre-populated by the shell with now. */
   char dt[20];
   fmt_date(m->ins_t, m->tz_off, dt, sizeof dt);
   char datep[8];
   char timep[8];
   str_snapshot(datep, sizeof datep, dt);
   if (str_len(datep) > 5)
      datep[5] = 0; /* "MM-DD" */
   str_snapshot(timep, sizeof timep, (str_len(dt) > 6) ? dt + 6 : "");
   stepper_row(fb, h, y, sc, lh, "DATE", datep, MA_INS_DMINUS, MA_INS_DPLUS);
   y += lh;
   stepper_row(fb, h, y, sc, lh, "TIME", timep, MA_INS_TMINUS, MA_INS_TPLUS);
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF00FF00, MA_INS_CONFIRM);
   y += 2 * lh;
   menu_button(fb, h, x, y, bw, sc, "DISCARD", 0xFFFFFFFF, MA_INS_DISCARD);
}

/* ---- OLD DEVICES: DISCONNECTED devices. Each keeps its whole slot, so a row
 * opens the SAME per-device menu as a live one (MA_SENSOR + slot index). The
 * list PAGINATES: if there are more than fit, a "< PAGE i/n >" row at the
 * bottom navigates, so any number of old devices is usable. ---- */

static void render_olddev(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_OLDDEV_BACK);
   y += 3 * lh;

   /* Collect the old slots' indices (into m->sensors) in list order. */
   int idxs[UI_MAX_SLOTS];
   int nold = 0;
   for (int i = 0; i < m->nsensors && nold < UI_MAX_SLOTS; i++)
      if (m->sensors[i].old)
         idxs[nold++] = i;
   if (nold <= 0) {
      draw_str(px, fb, x, y, sc, "None yet. Disconnected", 0xFF888888);
      y += lh;
      draw_str(px, fb, x, y, sc, "devices appear here.", 0xFF888888);
      return;
   }

   /* Rows that fit between the title and a reserved bottom nav line. Each row
    * takes 2*lh (a blank line between). At least one, always. */
   int avail = fb->height - y - (2 * lh); /* reserve the nav line */
   int per   = avail / (2 * lh);
   if (per < 1)
      per = 1;
   int npages = (nold + per - 1) / per;
   int page   = m->old_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int start = page * per;

   for (int r = start; r < start + per && r < nold; r++) {
      const struct ui_sensor *s = &m->sensors[idxs[r]];
      char val[20];
      long agot = (s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
      if (agot > 0)
         fmt_date(agot, m->tz_off, val, sizeof val);
      else
         (void)snprintf(val, sizeof val, "--");
      char name[3 + sizeof s->label];
      (void)snprintf(name, sizeof name, "  %s", s->label);
      menu_row(fb, h, y, sc, lh, name, val, 0xFFAAAAAA, MA_SENSOR + idxs[r]);
      if (s->marker != MARK_HIDE) {
         int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         plot_marker_glyph(px, fb->stride, fb->width, fb->height,
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
         add_hit(h, 0, navy - (3 * sc), fb->width / 3, 2 * lh, ACT_MENU,
                 MA_OLDPAGE_PREV);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (1 * 6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit(h, (2 * fb->width) / 3, navy - (3 * sc), fb->width / 3, 2 * lh,
                 ACT_MENU, MA_OLDPAGE_NEXT);
      }
   }
}

/* ---- MARKER / COLOR pickers: a full list of options, each with a live glyph,
 * so the user sees every choice before selecting. The title row returns to the
 * device's own menu (MA_SENSOR + its slot). ---- */
#define UI_NCOLORS 7 /* ui_sensor_colors[] entries */

static void render_markpick(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
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
   /* The picker always edits the selected slot -- old devices keep their slot,
    * so this works for them exactly like a live one (the title X returns to
    * that device's per-sensor menu). */
   int okk         = (m->sel >= 0 && m->sel < m->nsensors);
   int back        = MA_SENSOR + (m->sel >= 0 ? m->sel : 0);
   int curm        = okk ? m->sensors[m->sel].marker : MARK_DOT;
   int curc        = okk ? m->sensors[m->sel].color : 0;
   int curs        = okk ? m->sensors[m->sel].size : MARK_SIZE_DEF;
   uint32_t curcol = ui_sensor_color(curc);

   draw_str(px, fb, x, y, tsc, "MARKER", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, back);
   y += 2 * lh;
   int gw = fb->width - (2 * x);

   /* SHAPE grid: each shape as a glyph in the sensor's own colour. */
   draw_str(px, fb, x, y, sc, "SHAPE", 0xFF888888);
   y += lh;
   {
      int cols = 4;
      int cell = gw / cols;
      for (int i = 0; i < UI_NMARKERS; i++) {
         int mk = ui_marker_order[i];
         int cx = x + ((i % cols) * cell);
         int cy = y + ((i / cols) * cell);
         if (mk == MARK_HIDE) {
            int lw = str_len("OFF") * 6 * sc;
            draw_str(px, fb, cx + ((cell - lw) / 2),
                     cy + ((cell - (7 * sc)) / 2), sc, "OFF", 0xFFAAAAAA);
         } else {
            plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                              cx + (cell / 2), cy + (cell / 2), cell / 5, mk,
                              curcol);
         }
         draw_frame(px, fb, cx + sc, cy + sc, cell - (2 * sc), cell - (2 * sc),
                    (mk == curm) ? 0xFF33FF88 : 0xFF555555);
         add_hit(h, cx, cy, cell, cell, ACT_MENU, MA_MARK_PICK + mk);
      }
      y += (((UI_NMARKERS + cols - 1) / cols) * cell) + (lh / 2);
   }

   /* COLOR grid: full-colour buttons. */
   draw_str(px, fb, x, y, sc, "COLOR", 0xFF888888);
   y += lh;
   {
      int cols = UI_NCOLORS;
      int cell = gw / cols;
      for (int i = 0; i < UI_NCOLORS; i++) {
         int cx = x + (i * cell);
         plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                           cx + (cell / 2), y + (cell / 2),
                           (cell - (4 * sc)) / 2, MARK_SQUARE_F,
                           ui_sensor_color(i));
         draw_frame(px, fb, cx + sc, y + sc, cell - (2 * sc), cell - (2 * sc),
                    (i == curc) ? 0xFF33FF88 : 0xFF555555);
         add_hit(h, cx, y, cell, cell, ACT_MENU, MA_COLOR_PICK + i);
      }
      y += cell + (lh / 2);
   }

   /* SIZE row: the current shape+colour drawn at each of the 5 sizes. */
   draw_str(px, fb, x, y, sc, "SIZE", 0xFF888888);
   y += lh;
   {
      int cols  = MARK_SIZE_MAX;
      int cell  = gw / cols;
      int shape = (curm == MARK_HIDE) ? MARK_SQUARE_F : curm;
      for (int s = 1; s <= MARK_SIZE_MAX; s++) {
         int cx = x + ((s - 1) * cell);
         /* Same scaling the plot uses (radius grows linearly with size), so the
          * preview reflects the real on-plot size rather than filling the cell.
          */
         int r = (3 * sc * s) / MARK_SIZE_DEF;
         if (r < 1)
            r = 1;
         plot_marker_glyph(px, fb->stride, fb->width, fb->height,
                           cx + (cell / 2), y + (cell / 2), r, shape, curcol);
         draw_frame(px, fb, cx + sc, y + sc, cell - (2 * sc), cell - (2 * sc),
                    (s == curs) ? 0xFF33FF88 : 0xFF555555);
         add_hit(h, cx, y, cell, cell, ACT_MENU, MA_SIZE_PICK + s);
      }
      /* no y advance: the SIZE row is the last thing in this screen */
   }
}

/* ---- rename ----
 * Labels matter most when several identical devices are paired (two meters
 * look the same in a list), so this is a plain letter grid rather than a
 * digits-only keypad. 6 columns keeps every key a comfortable target. */

const char ui_label_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -";
#define UI_LABEL_COLS 6

int ui_label_nchars(void)
{
   return (int)(sizeof ui_label_chars) - 1;
}

static void render_label(struct ANativeWindow_Buffer *fb,
                         const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded. Left on width-only scaling, the row pitch this produces is
    * spent on height: the key grid's cell height collapses and pad_key then
    * subtracts from it with no floor, yielding NEGATIVE-height hit boxes. The
    * keys still DRAW, so the screen looks normal -- but ui_hit needs
    * `y < by + h`, which a negative h makes unsatisfiable, so none of them
    * respond. In landscape that meant no pairing code, no calibration value and
    * no plot max could be entered at all. */
   int sc  = ui_fit_scale(fb->width, fb->height, 30);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   int ty  = y;

   draw_str(px, fb, x, y, tsc, "NAME", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;

   /* what has been typed so far, with a caret so an empty name is still
    * obviously an entry field */
   char shown[16];
   const char *en = m->entry ? m->entry : "";
   int k          = 0;
   while (en[k] && k < (int)sizeof shown - 2) {
      shown[k] = en[k];
      k++;
   }
   shown[k]     = '_';
   shown[k + 1] = 0;
   int dsc      = 2 * sc;
   int dw       = str_len(shown) * 6 * dsc;
   draw_str(px, fb, (fb->width - dw) / 2, y, dsc, shown, 0xFF33FF88);
   y += (7 * dsc) + (8 * sc);
   add_hit(h, 0, ty - (3 * sc), fb->width, y - (ty - (3 * sc)), ACT_MENU,
           MA_KP_CLOSE);

   int n      = ui_label_nchars();
   int rows   = (n + UI_LABEL_COLS - 1) / UI_LABEL_COLS;
   int gm     = fb->width / 24;
   int gw     = fb->width - (2 * gm);
   int cw     = gw / UI_LABEL_COLS;
   int bottom = fb->height - (fb->height / 20);
   int ch     = (bottom - y) / (rows + 1); /* +1 row for DEL / OK */
   int ksc    = (ch - (4 * sc)) / 7;
   if (ksc < sc)
      ksc = sc;
   if (ksc > 3 * sc)
      ksc = 3 * sc;
   for (int i = 0; i < n; i++) {
      int cx      = gm + ((i % UI_LABEL_COLS) * cw);
      int cy      = y + ((i / UI_LABEL_COLS) * ch);
      char lbl[2] = {ui_label_chars[i], 0};
      draw_str(px, fb, cx + ((cw - (6 * ksc)) / 2), cy + ((ch - (7 * ksc)) / 2),
               ksc, lbl, 0xFFFFFFFF);
      add_hit(h, cx, cy, cw, ch, ACT_MENU, MA_CHAR + i);
   }
   /* DEL and OK share the last row */
   int cy = y + (rows * ch);
   int hw = gw / 2;
   draw_frame(px, fb, gm + (2 * sc), cy + (2 * sc), hw - (4 * sc),
              ch - (4 * sc), 0xFF555555);
   draw_str(px, fb, gm + ((hw - (3 * 6 * ksc)) / 2),
            cy + ((ch - (7 * ksc)) / 2), ksc, "DEL", 0xFFFFFFFF);
   add_hit(h, gm, cy, hw, ch, ACT_MENU, MA_BACKSPACE);
   draw_frame(px, fb, gm + hw + (2 * sc), cy + (2 * sc), hw - (4 * sc),
              ch - (4 * sc), 0xFF555555);
   draw_str(px, fb, gm + hw + ((hw - (2 * 6 * ksc)) / 2),
            cy + ((ch - (7 * ksc)) / 2), ksc, "OK", 0xFF33FF88);
   add_hit(h, gm + hw, cy, hw, ch, ACT_MENU, MA_OK);
}

/* ---- OneTouch: how to wake the meter, then a Scan button ---- */

static void render_meterhelp(struct ANativeWindow_Buffer *fb,
                             const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_ADDSENSOR);
   y += 3 * lh;

   static const char *const steps[] = {
       "1. TURN THE METER ON.",      "",
       "2. PRESS AND HOLD ITS",      "   UP ARROW UNTIL THE",
       "   BLUETOOTH SYMBOL SHOWS.", "",
       "3. THEN TAP SCAN BELOW.",
   };
   for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
      draw_str(px, fb, x, y, sc, steps[i], 0xFFCCCCCC);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   menu_button(fb, h, x, y, bw, sc, "SCAN", 0xFF33FF88, MA_METERSCAN);
}

/* ---- sensor-type picker (first step of adding a sensor) ---- */

static void render_senstype(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_SENSOR_BACK);
   y += 4 * lh; /* generous gap below the title */

   /* Each type is a large, framed, generously spaced tappable BUTTON -- label
    * only, no "TYPE" header and no CGM/METER tag. The OneTouch shows its full
    * name (the stored type name stays "ONETOUCH" so the 16-char device label
    * does not truncate). Buttons fill the width and are sized to the space that
    * is actually left, so they stay large without running off the bottom. */
   int ntypes = SENSOR_NTYPES - SENSOR_STELO;
   int btn_w  = fb->width - (2 * x);
   int bottom = fb->height - (fb->height / 20);
   int avail  = bottom - y; /* space from here to the bottom */
   /* Fill the space: a gap above, below, and between each button, so the
    * buttons are large and evenly spread rather than clustered at the top. */
   int gap   = avail / ((2 * ntypes) + 2);
   int btn_h = (avail - ((ntypes + 1) * gap)) / ntypes;
   if (btn_h > 40 * sc)
      btn_h = 40 * sc; /* a sane ceiling on very tall screens */
   if (btn_h < 16 * sc)
      btn_h = 16 * sc; /* tall enough to read clearly as a button */
   y += gap;           /* top gap */
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      const char *nm = sensor_disp_name(t);
      draw_frame(px, fb, x, y, btn_w, btn_h, 0xFF888888);
      int lw  = str_len(nm) * 6 * sc;
      int lhh = 7 * sc;
      draw_str(px, fb, x + ((btn_w - lw) / 2), y + ((btn_h - lhh) / 2), sc, nm,
               0xFFFFFFFF);
      add_hit(h, x, y, btn_w, btn_h, ACT_MENU, MA_TYPE + t);
      y += btn_h + gap;
   }
}

/* One keypad key: framed cell, centred label, full-cell ACT_MENU target. */
static void pad_key(struct ANativeWindow_Buffer *fb, struct hits *h, int cx,
                    int cy, int cw, int ch, int ksc, const char *lab, int code)
{
   uint32_t *px = fb->bits;
   draw_frame(px, fb, cx, cy, cw, ch, 0xFF555555);
   int lw  = str_len(lab) * 6 * ksc;
   int lhh = 7 * ksc;
   draw_str(px, fb, cx + ((cw - lw) / 2), cy + ((ch - lhh) / 2), ksc, lab,
            0xFFFFFFFF);
   add_hit(h, cx, cy, cw, ch, ACT_MENU, code);
}

/* Pairing / plot-max keypad: a title, a fixed-width entry field, and a 3x4
 * digit grid. Keys and the close band carry menu_action codes (100-113). */
static void render_keypad(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded. Left on width-only scaling, the row pitch this produces is
    * spent on height: the key grid's cell height collapses and pad_key then
    * subtracts from it with no floor, yielding NEGATIVE-height hit boxes. The
    * keys still DRAW, so the screen looks normal -- but ui_hit needs
    * `y < by + h`, which a negative h makes unsatisfiable, so none of them
    * respond. In landscape that meant no pairing code, no calibration value and
    * no plot max could be entered at all. */
   int sc  = ui_fit_scale(fb->width, fb->height, 30);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   int ty = y;
   char pair_title[24];
   const char *kp_title = pair_title;
   if (m->kp_mode == 1)
      kp_title = "PLOT MAX";
   else if (m->kp_mode == 2)
      kp_title = "CALIBRATION";
   else if (m->kp_mode == 3)
      kp_title = "RESCALE";
   else if (m->kp_mode == 4)
      kp_title = "REMOTE IP";
   else if (m->kp_mode == 5)
      kp_title = "REMOTE PORT";
   else /* pairing: name the CGM being added, e.g. "PAIR NEW STELO" */
      (void)snprintf(pair_title, sizeof pair_title, "PAIR NEW %s",
                     m->add_type ? m->add_type : "SENSOR");
   draw_str(px, fb, x, y, tsc, kp_title, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;

   /* Entry field: one underscore per slot, replaced by digits as typed, so the
    * width never shifts. Plot-max shows the unit after the value; pair shows
    * the 4 code digits; the remote IP gets a full dotted quad's 15 slots and
    * the remote port a TCP port's 5. dsc is sized for the widest label so the
    * field -- and the keypad below -- is identical across modes. */
   static const int slots_for[6] = {4, 3, 3, 3, 15, 5};
   int nslots = slots_for[(m->kp_mode >= 0 && m->kp_mode < 6) ? m->kp_mode : 0];
   int has_unit   = m->kp_mode >= 1 && m->kp_mode <= 3; /* glucose entries */
   const char *en = m->entry ? m->entry : "";
   char shown[24];
   int k = 0;
   for (int i = 0; i < nslots; i++) /* typed digits, then '_' for empty slots */
      shown[k++] = *en ? *en++ : '_';
   if (has_unit)
      k += snprintf(shown + k, sizeof shown - k, " %s", UI_LBL(m->units));
   shown[k] = 0;
   /* Size the field from what is actually shown ("___ MMOL/L" = 10 cells, the
    * 15-slot IP is wider still), so no mode overflows the margins. */
   int fcells = str_len(shown);
   if (fcells < 10)
      fcells = 10;
   int dsc = (fb->width - (8 * sc)) / (fcells * 6);
   if (dsc > 4 * sc)
      dsc = 4 * sc;
   if (dsc < sc)
      dsc = sc;
   int dw = str_len(shown) * 6 * dsc;
   draw_str(px, fb, (fb->width - dw) / 2, y, dsc, shown, 0xFF33FF88);
   y += (7 * dsc) + (12 * sc);

   /* Generous close target: the whole area above the keypad closes it. */
   add_hit(h, 0, ty - (3 * sc), fb->width, y - (ty - (3 * sc)), ACT_MENU, 113);

   /* 3x4 grid: digits, then 0 / DEL / OK. The title's X cancels. The IP mode
    * needs a 13th key ('.'), so it swaps to a 5-row layout: the dot takes 0's
    * old cell, 0 and DEL shift right, and OK becomes a full-width bottom row
    * (which also makes the confirm harder to fat-finger from DEL). */
   int iprows = (m->kp_mode == 4) ? 5 : 4;
   int gm     = fb->width / 12;
   int gw     = fb->width - (2 * gm);
   int cw     = gw / 3;
   int bottom = fb->height - (fb->height / 20);
   int ch     = (bottom - y) / iprows;
   int wfit   = (cw - (4 * sc)) / (3 * 6); /* widest label "DEL" fits width */
   int hfit   = (ch - (4 * sc)) / 7;
   int ksc    = wfit < hfit ? wfit : hfit;
   if (ksc < sc)
      ksc = sc;
   static const char *keys[12]   = {"7", "8", "9", "4", "5", "6",
                                    "1", "2", "3", "0", "<", "OK"};
   static const int acts[12]     = {107, 108, 109, 104, 105, 106,
                                    101, 102, 103, 100, 110, MA_OK};
   static const char *ipkeys[12] = {"7", "8", "9", "4", "5", "6",
                                    "1", "2", "3", ".", "0", "<"};
   static const int ipacts[12]   = {107, 108, 109, 104,    105, 106,
                                    101, 102, 103, MA_DOT, 100, 110};
   const char **kk               = (m->kp_mode == 4) ? ipkeys : keys;
   const int *aa                 = (m->kp_mode == 4) ? ipacts : acts;
   for (int r = 0; r < 4; r++)
      for (int col = 0; col < 3; col++) {
         int idx = (r * 3) + col;
         pad_key(fb, h, gm + (col * cw), y + (r * ch), cw - (2 * sc),
                 ch - (2 * sc), ksc, kk[idx], aa[idx]);
      }
   if (m->kp_mode == 4)
      pad_key(fb, h, gm, y + (4 * ch), (3 * cw) - (2 * sc), ch - (2 * sc), ksc,
              "OK", MA_OK);
}

/* Pairing candidate picker: scanned sensors strongest-first; a tap pairs one
 * (ACT_MENU 200+index), the X cancels (199). */
static void render_devlist(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
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
   if (m->add_kind == KIND_BGM)
      (void)snprintf(sel_title, sizeof sel_title, "SELECT METER");
   else
      (void)snprintf(sel_title, sizeof sel_title, "SELECT %s",
                     m->add_type ? m->add_type : "SENSOR");
   draw_str(px, fb, x, y, tsc, sel_title, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 8 * tsc, ACT_MENU, 199);
   y += 2 * lh;

   if (m->ndev <= 0) {
      draw_str(px, fb, x, y, sc, "Searching for sensors...", 0xFF888888);
      return;
   }
   draw_str(px, fb, x, y, sc, "Nearest first -- tap yours:", 0xFF888888);
   y += 2 * lh;

   /* selection sort by RSSI, strongest first (the model owns index -> device)
    */
   int order[16];
   int n = m->ndev < 16 ? m->ndev : 16;
   for (int i = 0; i < n; i++)
      order[i] = i;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         if (m->devs[order[j]].rssi > m->devs[order[i]].rssi) {
            int t    = order[i];
            order[i] = order[j];
            order[j] = t;
         }
   for (int kk = 0; kk < n; kk++) {
      int i = order[kk];
      char rs[12];
      (void)snprintf(rs, sizeof rs, "%d dBm", m->devs[i].rssi);
      menu_row(fb, h, y, sc, lh, m->devs[i].name, rs, 0xFFFFFFFF, 200 + i);
      y += lh;
   }
}

/* First-run permission rationale + a CONTINUE button (records
 * ACT_GATE_CONTINUE). All static copy; the model is unused beyond the
 * framebuffer size. */
static void render_gate(struct ANativeWindow_Buffer *fb, struct hits *h)
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
       "Your glucose data never",
       "leaves this phone.",
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

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h)
{
   h->n = 0;
   clear_fb(fb, 0xFF181818);
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
      case SCR_PAIRCONF: render_pairconf(fb, m, h); break;
      case SCR_ADDMENU: render_addmenu(fb, m, h); break;
      case SCR_INSULIN: render_insulin(fb, m, h); break;
      case SCR_PRIMPICK: render_primpick(fb, m, h); break;
      case SCR_PERMS: render_perms(fb, m, h); break;
      case SCR_REMOTE: render_remote(fb, m, h); break;
      case SCR_OLDDEV: render_olddev(fb, m, h); break;
      case SCR_LABEL: render_label(fb, m, h); break;
      case SCR_MARKPICK:
      case SCR_COLORPICK: render_markpick(fb, m, h); break; /* combined menu */
      case SCR_MAIN: render_main(fb, m, h); break;
      case SCR_N: break; /* not a screen; only bounds the enum */
   }
}

struct action ui_hit(const struct hits *h, int x, int y)
{
   /* last box wins, matching draw order (later-drawn overlays are on top) */
   for (int i = h->n - 1; i >= 0; i--) {
      int bx = h->box[i].x;
      int by = h->box[i].y;
      if (x >= bx && x < bx + h->box[i].w && y >= by && y < by + h->box[i].h)
         return (struct action){h->box[i].kind, h->box[i].arg};
   }
   return (struct action){ACT_NONE, 0};
}
