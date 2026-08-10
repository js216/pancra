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
#include "alarmlogic.h" /* AL_ENTRY_MAX: the alarm keypads' ceiling */
#include "font.h"
#include "insulin.h" /* struct ins_rec + INS_* for the INSULIN LOG table */
#include "ndk.h"
#include "plot.h"
#include "plotdata.h" /* PLOT_LONG_MAX: the long-span point ceiling */
#include "sensors.h"  /* sensor types, kinds, marker enum */
#include "settings.h" /* SET_NCOLORS: crosschecked below */
#include "util.h"     /* str_snapshot */
#include "weight.h"   /* wt_unit_name / wt_to_tenths: the weight rows */
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
/* 720h = 30D on the right. 6H went: it sat between 3H and 12H without
 * showing anything either of them didn't. */
static const int ui_tab_hours[UI_TABS] = {3, 12, 24, 72, 168, 720};

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

/* A CHECKBOX AT AN ARBITRARY SIZE, with a border `th` thick.
 *
 * The 5x7 icon_box glyph could not do this job. Drawn at the body scale it is
 * a box the size of one letter with a one-unit outline, and next to a SOLID
 * green one it disappeared: on the DEVICES list the unticked PRIMARY boxes
 * read as empty space, so the explainer's "TAP A BOX TO SWITCH" pointed at
 * nothing visible. Size is what makes a control look like a control, and a
 * glyph cannot be sized independently of the text around it.
 *
 * The tick is an INSET SOLID square rather than a check mark: it stays legible
 * at every size the two callers use, and it matches icon_boxfill, which is the
 * shape the PRIMARY column already spoke in. */
static void draw_checkbox(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                          int x, int y, int side, int th, int on, uint32_t c)
{
   if (th < 1)
      th = 1;
   if (side < 4 * th)
      return; /* nothing legible fits; fill_rect would only count clips */
   fill_rect(px, buf, x, y, side, th, c);
   fill_rect(px, buf, x, y + side - th, side, th, c);
   fill_rect(px, buf, x, y, th, side, c);
   fill_rect(px, buf, x + side - th, y, th, side, c);
   if (on)
      fill_rect(px, buf, x + (2 * th), y + (2 * th), side - (4 * th),
                side - (4 * th), c);
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
   if (h->n >= UI_MAX_HITS) {
      /* LOUD, not silent. A dropped box draws normally and is dead to
       * touch, which is indistinguishable from a control that simply does
       * not work -- exactly the failure the keypad-title fallthrough had.
       * uitest gates on this staying clear at every screen and geometry. */
      h->overflow = 1;
      return;
   }
   h->box[h->n].x    = x;
   h->box[h->n].y    = y;
   h->box[h->n].w    = w;
   h->box[h->n].h    = hgt;
   h->box[h->n].kind = kind;
   h->box[h->n].arg  = arg;
   /* glow (pressed-highlight) rect defaults to the hit rect itself */
   h->box[h->n].gx = x;
   h->box[h->n].gy = y;
   h->box[h->n].gw = w;
   h->box[h->n].gh = hgt;
   h->n++;
}

/* Narrow the LAST recorded hit box's glow rect to (x,y,w,h) -- for a control
 * whose hit zone is deliberately larger than its visible glyph, so arming it
 * lights the glyph alone, not every stranger caught in the zone. */
static void add_glow(struct hits *h, int x, int y, int w, int hgt)
{
   if (h->n <= 0)
      return;
   h->box[h->n - 1].gx = x;
   h->box[h->n - 1].gy = y;
   h->box[h->n - 1].gw = w;
   h->box[h->n - 1].gh = hgt;
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

/* The 5x7 icon BITMAPS live in font.c with the glyph tables (all the pixel
 * art in one file); this blitter stays here with draw_cell and its clipping
 * counter. */

/* Draw one 5x7 icon at (ox,oy), scale sc -- the icon equivalent of one
 * draw_str glyph cell. */
static void draw_icon(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                      int ox, int oy, int sc, const uint8_t g[7], uint32_t c)
{
   for (int row = 0; row < 7; row++)
      for (int col = 0; col < 5; col++)
         if ((unsigned)g[row] & (0x10U >> (unsigned)col))
            draw_cell(px, buf, ox + (col * sc), oy + (row * sc), sc, c);
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
/* One constant, so the two tables and the two guards below cannot drift:
 * a colour index comes from a settings file the user can hand-edit, and a
 * table that outgrew its guard would read past its end. */
#define UI_NCOLORS 7
/* settings_load bounds the stored insulin colour against SET_NCOLORS,
 * which it cannot see this table to check. Keep them equal here. */
_Static_assert(UI_NCOLORS == SET_NCOLORS,
               "UI_NCOLORS and SET_NCOLORS disagree");
static const uint32_t ui_sensor_colors[UI_NCOLORS] = {
    0xFF88FF33 /* GREEN */,
    0xFFFFAA44 /* BLUE */,
    0xFF44CCFF /* AMBER */,
    0xFFAA66FF /* PINK */,
    0xFFEEFF66 /* CYAN */,
    0xFFFF88BB /* VIOLET */,
    0xFFFFFFFF /* WHITE -- the default primary-trace colour */};
static const char *const ui_color_names[UI_NCOLORS] = {
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
   if (color < 0 || color >= UI_NCOLORS)
      color = 0;
   return ui_sensor_colors[color];
}

const char *ui_color_name(int color)
{
   if (color < 0 || color >= UI_NCOLORS)
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

/* UI_DEV_ABOVE -- the rows the DEVICES screen spends outside its list -- lives
 * in ui.h, so test/uitest.c reads the same definition. See there for why. */

/* Layout scale for the DEVICES screen.
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

int ui_devices_scale(int w, int h)
{
   /* Must match ui_sensor_capacity's requirement EXACTLY. That function needs
    *   h - start - (UI_DEV_ABOVE + 1)*lh >= UI_MIN_SLOTS * UI_DEV_PITCH
    * with start = h/20 + 8*sc, lh = 16*sc and UI_DEV_PITCH = 24*sc. Measured
    * in the 16*sc lines ui_fit_scale counts, the list side is
    * UI_MIN_SLOTS*1.5 -> UI_DEV_MIN_ROWS (rounded up), i.e.
    *   h - h/20 >= sc * (8 + (UI_DEV_ABOVE + UI_DEV_MIN_ROWS + 1) * 16).
    * An earlier version omitted the +8 from `start`, leaving the two functions
    * 8*sc apart -- so on heights where the slack fell in that gap, capacity
    * still came out below UI_MIN_SLOTS and the renderer still took its
    * early return, hiding the device list and the ADD button. That band
    * included 1080x2280 (Galaxy S10 / Redmi Note 7 / Moto G7) and 1440x3200
    * (S20-S22 Ultra at QHD+). Derive it from the same expression so they
    * cannot drift. */
   /* UI_DEV_MIN_ROWS, not UI_MIN_SLOTS: device rows are spaced at
    * UI_DEV_PITCH (a line and a half), so the minimum list costs more lines
    * than it holds devices. Using the slot count here would reserve 3 lines
    * for a list that needs 5 and hand back a scale too large to fit it. */
   return ui_fit_scale(w, h, UI_DEV_ABOVE + UI_DEV_MIN_ROWS + 1);
}

int ui_sensor_capacity(int w, int h)
{
   int sc    = ui_devices_scale(w, h);
   int lh    = 16 * sc;
   int start = (h / 20) + (8 * sc);
   int avail = h - lh - start - (UI_DEV_ABOVE * lh);
   if (avail < 0)
      return 0;
   /* Divided by the PITCH the renderer actually advances by, gap included --
    * dividing by the bare line height would promise more rows than fit and
    * push the ADD button off the bottom. */
   int n = avail / UI_DEV_PITCH(sc);
   return n > UI_MAX_SLOTS ? UI_MAX_SLOTS : n;
}

/* A threshold that no reading can ever reach must SAY it is off, not show the
 * number that makes it so.
 *
 * 0 sits below every possible reading and AL_ENTRY_MAX above every one -- each
 * end is that threshold's deliberate OFF switch (alarmlogic.h). Rendering
 * those as "0" and "999" states the mechanism and hides the consequence, and
 * the consequence is the one this app must never let the user get wrong:
 * believing a reminder is armed when nothing can ever trigger it. That is
 * precisely the hazard the NUDGE exists to remove, and it would be perverse
 * for the row announcing it to be ambiguous. */
static int thresh_off(int mgdl, int ishigh)
{
   return ishigh ? (mgdl >= AL_ENTRY_MAX) : (mgdl <= 0);
}

static void fmt_thresh(int mgdl, int units, int ishigh, char *out, int n)
{
   if (thresh_off(mgdl, ishigh))
      (void)snprintf(out, n, "OFF");
   else
      fmt_glu(mgdl, units, out, n);
}

/* One threshold row on the main screen: "<NAME>  LOW <v>  HIGH <v>", the full
 * column width, returning the y below it.
 *
 * ALARM and NUDGE share this because their columns MUST agree: the two rows
 * are read as a pair (the nudge is the outer band, the alarm the inner one),
 * and values that do not line up cannot be compared at a glance. Both names
 * are five characters and both rows reserve the SAME fixed icon cell -- the
 * nudge simply leaves it empty -- so identical arithmetic here puts every
 * column in the same place on both rows. */
/* lx0 / rx1 are the row's EXACT left and right ink edges, not a column to be
 * distributed inside. They are the progress bar's leftmost pixel and the right
 * edge of the units label beside the big number, so these two rows share their
 * margins with the top of the screen instead of using the content column's own
 * wider one -- three different left margins in one vertical line was the thing
 * that made the screen look untidy even when every individual row was fine. */
static int thresh_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h, int lx0, int rx1, int y, int sc, int pad,
                      int isalarm)
{
   uint32_t *px      = fb->bits;
   const uint32_t gy = 0xFF888888;
   const uint32_t wt = 0xFFFFFFFF;
   int cwid          = 6 * sc;
   char lo[8];
   char hi[8];
   fmt_thresh(isalarm ? m->alarm_low : m->nudge_low, m->units, 0, lo,
              sizeof lo);
   fmt_thresh(isalarm ? m->alarm_high : m->nudge_high, m->units, 1, hi,
              sizeof hi);
   const char *tok[5] = {isalarm ? "ALARM" : "NUDGE", "LOW", lo, "HIGH", hi};
   uint32_t tcol[5]   = {gy, gy, wt, gy, wt};
   /* Four icon cells LEFT of the name, at a FIXED 6*sc pitch, and the SAME
    * four columns on both rows: speaker (sound), phone (vibration), then a
    * row-specific third, then dot (NEW DATAPOINT). Each symbol always appears
    * in the same place regardless of which others are enabled; an off state
    * just leaves its cell empty, so toggling never shifts anything, and
    * speaker sits above speaker so the two rows read as a table.
    *
    * Each row shows ITS OWN outputs. The nudge has its own sound and
    * vibration -- one alert says "act now" and the other says "have a look",
    * and muting either must not mute the other. The third cell is the
    * DISCONNECT alarm on the ALARM row and nothing on the NUDGE row; the dot
    * is on the NUDGE row because NEW DATAPOINT lives in the NUDGE section of
    * the menu these rows open, and icons that contradict the menu behind them
    * are worse than no icons. */
   int icon_w = 23 * sc;
   /* FIXED COLUMNS, SIZED FROM BOTH ROWS.
    *
    * The widths used to come from THIS row's own tokens, so the two rows
    * disagreed the moment their values differed in length: with ALARM 95/300
    * and NUDGE 100/250 the nudge row was one character wider, which shrank
    * its gap and shifted everything left -- measured 3 px on the icons and up
    * to 9 px on the labels. The two rows are read as a table, so every column
    * must start at the same x whatever the numbers happen to be. Take each
    * column's width from the WIDER of the two rows and use it for both. */
   char olo[8];
   char ohi[8];
   fmt_thresh(isalarm ? m->nudge_low : m->alarm_low, m->units, 0, olo,
              sizeof olo);
   fmt_thresh(isalarm ? m->nudge_high : m->alarm_high, m->units, 1, ohi,
              sizeof ohi);
   const char *oth[5] = {"XXXXX", "LOW", olo, "HIGH", ohi};
   int colw[5];
   int total = icon_w;
   for (int i = 0; i < 5; i++) {
      int a   = str_len(tok[i]);
      int b   = str_len(oth[i]);
      colw[i] = (a > b ? a : b) * cwid;
      total += colw[i];
   }
   /* SIX elements (the icon block plus five columns) means FIVE gaps between
    * them -- no leading or trailing gap, because the row's outer edges are
    * given, not derived. The last column's ink stops one unit short of its
    * cell (draw_str emits no trailing gap), so add that sc back before
    * dividing or the row lands a pixel inside rx1 instead of on it. */
   int g = ((rx1 - lx0) + sc - total) / 5;
   if (g < cwid)
      g = cwid;
   int ax = lx0;
   if (isalarm ? m->sound_on : m->nudge_sound)
      draw_icon(px, fb, ax, y, sc, icon_speaker, gy);
   if (isalarm ? m->vib_on : m->nudge_vib)
      draw_icon(px, fb, ax + (6 * sc), y, sc, icon_vibrate, gy);
   if (isalarm && m->disc)
      draw_icon(px, fb, ax + (12 * sc), y, sc, icon_nolink, gy);
   if (!isalarm && m->newdata_mode)
      draw_icon(px, fb, ax + (18 * sc), y, sc, icon_dot, gy);
   ax += icon_w + g;
   int al_y = y - (3 * sc);
   /* EXACTLY the row advance, so consecutive rows ABUT rather than overlap.
    * It used to be (3 + 7)*sc + pad while the advance is (7*sc) + pad, i.e.
    * 3*sc taller than its own row -- harmless while ALARM was the only such
    * row, and a mis-actuation the moment NUDGE appeared below it: ui_hit_idx
    * scans backwards, so the bottom 3*sc of "ALARM HIGH" (9 px at 1080x1920)
    * opened the NUDGE HIGH keypad instead. Measured at every geometry before
    * this line changed. */
   int al_h = (7 * sc) + pad;
   /* Three targets on the row: everything LEFT of "LOW" (the icons and the
    * ALARM label, from the screen's leftmost pixel) opens the ALARM
    * submenu; "LOW <value>" and "HIGH <value>" are each ONE target (label +
    * value + surrounding gap, full row height) opening that threshold's
    * keypad. The three are DISJOINT -- the pressed highlight lights the
    * armed control's whole rectangle, so they must not contain each other's
    * pixels. Both rows open the SAME submenu: it holds both sections. */
   int pair_x = 0;
   for (int i = 0; i < 5; i++) {
      if (i == 1 || i == 3)
         pair_x = ax; /* start of the LOW / HIGH pair */
      if (i == 1)
         add_hit(h, 0, al_y, ax - (g / 2), al_h, ACT_MENU, MA_ALARM_OPEN);
      /* Values RIGHT-aligned in their column so the digits line up under one
       * another; labels left-aligned. Advance by the COLUMN width, never by
       * this token's own width, or the columns drift apart again. */
      int tw = str_len(tok[i]) * cwid;
      int tx = (i == 2 || i == 4) ? ax + (colw[i] - tw) : ax;
      draw_str(px, fb, tx, y, sc, tok[i], tcol[i]);
      if (i == 2 || i == 4) {
         int code = (i == 2) ? MA_ALARM_LOW : MA_ALARM_HIGH;
         if (!isalarm)
            code = (i == 2) ? MA_NUDGE_LOW : MA_NUDGE_HIGH;
         /* CLAMPED TO THE ROW'S OWN RIGHT EDGE. The pair's box reaches half a
          * gap left and a full gap right so the label, the value and the air
          * around them are all pressable -- but rx1 is now an EXACT edge, not
          * a column with slack after it, so that trailing gap ran off the
          * buffer on the HIGH pair and the target was dropped as off-screen.
          * Half a gap of overhang is still comfortable and always fits. */
         int hx    = pair_x - (g / 2);
         int hw    = (ax + colw[i]) - pair_x + g;
         int hmaxw = rx1 - hx;
         if (hw > hmaxw)
            hw = hmaxw;
         /* A non-positive width would register a box no tap can ever fall
          * inside -- a control that draws normally and is dead to touch,
          * which is the failure add_hit's own overflow flag exists to make
          * loud. Unreachable at any real geometry; skip rather than record a
          * lie if one ever gets there. */
         if (hw > 0)
            add_hit(h, hx, al_y, hw, al_h, ACT_MENU, code);
      }
      ax += colw[i] + g;
   }
   return y + (7 * sc) + pad;
}

/* The CGM that owns the big number, or NULL. Four places open-coded this same
 * scan; the big number, its session line and the plot all have to agree on
 * which sensor they are describing, so there is one answer to the question. */
static const struct ui_sensor *primary_cgm(const struct screen *m)
{
   for (int k = 0; k < m->nsensors; k++)
      if (m->sensors[k].primary && m->sensors[k].kind == KIND_CGM)
         return &m->sensors[k];
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
   if (!m->has_cgm) {
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
   if (m->session_seconds <= 0) {
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
   long ss = m->session_seconds;
   if (m->sess_state == SENSOR_STATE_WARMUP ||
       (m->sess_state == 0 && ss > 0 && ss < SENSOR_WARMUP_S)) {
      /* The sensor's OWN state byte, or the clock heuristic before any 4e has
       * answered. The live clock is what makes this match the official
       * reader, to the second. */
      long r = SENSOR_WARMUP_S - ss;
      if (r < 0)
         r = 0;
      (void)snprintf(out, n, "WARMUP %ld:%02ld", r / 60, r % 60);
      return 0xFF00CCFF;
   }
   if (m->sess_state == SENSOR_STATE_ENDED) {
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
/* Vertical cost of the alarm banner, in units of sc: a (7+9) advance plus a
 * 5*sc-scaled glyph (7*5 tall). Every layout that can host the banner reserves
 * this before deciding its own heights -- the banner is the explicit statement
 * that the user is out of range, so "no room left" is never acceptable. */
#define UI_BANNER_H (51)

/* Defined below; both column layouts draw it, in different places. */
static int alarm_banner(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        int cx, int cw, int y, int sc);

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
   if (m->stale || m->glu < 0 || !m->has_cgm) {
      (void)snprintf(big, sizeof big, "---");
      bigcol = 0xFF888888;
   } else {
      fmt_glu(m->glu, m->units, big, sizeof big);
      bigcol = glu_color(m->glu);
   }
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
   if (m->has_cgm && m->predicted > 0 && m->predicted <= 400) {
      char pv[12];
      fmt_glu(m->predicted, m->units, pv, sizeof pv);
      (void)snprintf(pred, sizeof pred, ">%s", pv);
   } else {
      (void)snprintf(pred, sizeof pred, ">--");
   }
   /* Signal strength moved to each device's own menu (SIGNAL STRENGTH); the
    * main readout shows units / trend / prediction / age. */
   int uw     = str_len(UI_LBL(m->units));
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
   int nglyph = m->units ? 4 : 3;
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
   draw_str(px, fb, bx, y, bigsc, big, bigcol);
   /* The NUMBER ITSELF opens the DEVICES screen -- the registry of everything
    * feeding it, where the primary is chosen (per device) and a new one is
    * added. The glyphs only, not the band: the hamburger (settings) and the
    * tab row keep their own pixels, which is why the old whole-band settings
    * target was removed. */
   add_hit(h, bx, y, ink_w, 7 * bigsc, ACT_MENU, MA_DEVICES_OPEN);
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
   if (m->t > 0 && m->has_cgm) {
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
   draw_str(px, fb, colx, units_y, sc, UI_LBL(m->units), 0xFFCCCCCC);
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
   add_hit(h, hx0, y, (cx + cw) - hx0, (units_y + (7 * sc)) - y,
           ACT_OPEN_SETTINGS, 0);
   /* ...but the pressed highlight lights the hamburger GLYPH alone (plus a
    * little breathing room) -- the zone also contains the units label, and
    * a lit MG/DL would read as if the units were about to change. */
   add_glow(h, ham_x - (2 * sc), ham_y - (2 * sc), ham_w + (4 * sc),
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
         plot_marker_glyph(px, fb->stride, fb->width, fb->height, mx,
                           pl_y + (3 * sc), gr, ps->marker,
                           ui_sensor_color(ps->color));
      }
      /* The line names a device, so tapping it opens that device's menu --
       * the shortcut the old info table carried, kept on the content that
       * replaced it. */
      if (ps)
         add_hit(h, bx3, pl_y - (2 * sc), (cx + cw) - bx3, gh + (4 * sc),
                 ACT_MENU, MA_SENSOR + (int)(ps - m->sensors));
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
      g.x1 = colx + (((str_len(UI_LBL(m->units)) * 6) - 1) * sc);
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
   int scrub     = (m->scrub >= 0 && m->scrub < m->nhist);
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
      int ins = (m->hist[m->scrub].kind == KIND_INS);
      int wt  = (m->hist[m->scrub].kind == KIND_WT);
      fmt_hms(m->hist[m->scrub].t, m->tz_off, ts, sizeof ts);
      ts[5] = '\0';
      /* An insulin dose scrubs like glucose, shown as "2 U FAST" /
       * "10 U SLOW" (src carries the dose TYPE for insulin points). A weight
       * scrubs the same way -- identical code path, identical readout shape --
       * converted from the stored GRAMS into whichever display unit is set,
       * so switching KG/LB re-renders history instead of relabelling it. */
      if (ins) {
         (void)snprintf(gv, sizeof gv, "%d U", m->hist[m->scrub].glu);
      } else if (wt) {
         /* Clamped so the format is provably bounded: the store's own range
          * (WT_MIN_G..WT_MAX_G) tops out at 400 kg / 882 lb, i.e. four digits
          * of tenths, but the compiler cannot see that through wt_to_tenths
          * and treats the snprintf as possibly truncating. */
         int t10 = wt_to_tenths(m->hist[m->scrub].glu, m->wunits);
         if (t10 < 0)
            t10 = 0;
         if (t10 > 99999)
            t10 = 99999;
         (void)snprintf(gv, sizeof gv, "%d.%d", t10 / 10, t10 % 10);
      } else {
         fmt_glu(m->hist[m->scrub].glu, m->units, gv, sizeof gv);
      }
      /* On the multi-day spans a bare HH:MM is ambiguous across days, so
       * prefix the DATE as M/DD ("7/21"). A weekday name told you it was a
       * Thursday but not WHICH one, which is useless once the span passes a
       * week -- and at 30D it is useless immediately. */
      const char *unit = UI_LBL(m->units);
      if (ins)
         unit = (m->hist[m->scrub].src == INS_FAST) ? "FAST" : "SLOW";
      else if (wt)
         unit = wt_unit_name(m->wunits);
      if (m->plot_hours >= 720) {
         /* A MONTH-long span: a weekday name is ambiguous four times over,
          * so name the actual date. */
         char dt[20];
         /* 12, not 8: gcc cannot prove the month is two digits, and this
          * build treats a possibly-truncating snprintf as an error. */
         char md[12];
         fmt_date(m->hist[m->scrub].t, m->tz_off, dt, sizeof dt);
         /* fmt_date gives "YYYY-MM-DD HH:MM"; take the month without its
          * leading zero, and the day as written. */
         int mon = ((dt[5] - '0') * 10) + (dt[6] - '0');
         (void)snprintf(md, sizeof md, "%d/%c%c", mon, dt[8], dt[9]);
         (void)snprintf(line, sizeof line, "%s %s %s %s", gv, unit, md, ts);
      } else if (m->plot_hours >= 72) {
         /* Within a week, the weekday IS the clearer label -- "TUE 08:15"
          * places a reading the way you actually remember it, and there is
          * only one Tuesday to confuse it with. 1970-01-01 was a Thursday. */
         static const char *const wd[7] = {"SUN", "MON", "TUE", "WED",
                                           "THU", "FRI", "SAT"};
         long z = (m->hist[m->scrub].t + m->tz_off) / 86400;
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
    * threshold rows (39*sc) AND the alarm banner beneath them
    * (UI_BANNER_H) -- in landscape this column is the only one with room
    * under the plot, so the banner is drawn here and its space must be
    * reserved here. Taking the whole column left the banner nowhere to go,
    * which is the one thing this screen may never do. */
   {
      ph      = bottom - y - (39 * sc) - (UI_BANNER_H * sc);
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
    * full 7 days -- the same shrinking-7D bug NHIST's sizing fixes. ui.c is
    * intentionally decoupled from store.h, so the Makefile `crosscheck` target
    * greps both and fails the build if they ever drift apart. */
#define UI_PLOT_GLU 5040
   /* ...PLUS the insulin doses, which the shell appends AFTER the glucose
    * points in the SAME m->hist array (build_model sizes it NHIST + NINS).
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
      /* An insulin dose: drawn in the user's INSULIN MARKER styling
       * (marker/colour/size, both types alike). Its y IGNORES the units
       * value entirely -- units are not glucose -- and sits at 60, the
       * middle of the 50..70 band below the low line, where glucose
       * points rarely live and the frame never clips the glyph. The
       * scrub still reads the real units from m->hist. */
      if (m->hist[i].kind == KIND_INS) {
         int ty        = (m->hist[i].src == INS_FAST) ? INS_FAST : INS_SLOW;
         pts[i].glu    = 60;
         pts[i].marker = m->ins_marker[ty];
         pts[i].col    = ui_sensor_color(m->ins_color[ty]);
         pts[i].size   = m->ins_size[ty];
         if (m->ins_marker[ty] == MARK_HIDE)
            pts[i].hidden = 1;
         continue;
      }
      /* A logged WEIGHT: the same bottom line as the doses (glu pinned to 60,
       * well clear of real glucose), drawn as a small W. Its glu field carries
       * GRAMS, which is meaningless on this axis -- the scrub reads the real
       * value back out of m->hist, exactly as it does for a dose. A fixed
       * shape and colour, not a configurable marker: there is one weight
       * series, so there is nothing to tell apart. */
      if (m->hist[i].kind == KIND_WT) {
         pts[i].glu    = 60;
         pts[i].marker = PLOT_MARK_W;
         pts[i].col    = 0xFF88CCFF;
         pts[i].size   = MARK_SIZE_DEF;
         continue;
      }
      int matched = 0;
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
   /* The longer the span, the denser the points and the less a fat marker
    * says: at 30D thousands of readings share the width, so half the 7D
    * radius keeps the shape of the trace readable instead of smearing it
    * into a band. */
   int prad = 3 * sc / 2;
   if (m->plot_hours >= 720)
      prad = prad / 4;
   else if (m->plot_hours >= 168)
      prad = prad / 2;
   else if (m->plot_hours >= 72)
      prad = prad * 3 / 4;
   if (prad < 1)
      prad = 1;
   plot_render(px, fb->stride, fb->width, fb->height, plot_x, plot_y, plot_w,
               ph, pts, np, m->now, m->plot_hours, prad, white_color,
               scrub ? m->scrub : -1, UI_HILITE, m->tz_off);
   /* the whole plot rect scrubs; the shell resolves the datapoint via plot_hit
    */
   add_hit(h, plot_x, plot_y, plot_w, ph, ACT_SCRUB, 0);
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
   /* LANDSCAPE ONLY: this column has the space under it, so the banner goes
    * here. In portrait it belongs to the info block below the stats table,
    * which is where the eye already is. */
   if (landscape)
      y = alarm_banner(fb, m, cx, cw, y, sc);
   return y;
}

/* Right/bottom column: the rolling-stats table, the ADD '+' and the alarm
 * banner. The PRIMARY / STATE / SESSION / PRED table that used to head this
 * block is gone -- what it said now rides on the big number itself (the
 * prediction in the label column, the sensor and its countdown on one line
 * under the progress bar), which is where the eye already is. */
/* THE ALARM BANNER -- STALE / LOW / HIGH in big letters.
 *
 * Its own function because the two layouts put it in different columns: under
 * the stats table in portrait, and under the plot's threshold rows in
 * landscape, where the stats column has no room left beneath it. It is the
 * explicit on-screen statement that the user is out of range, so "there was
 * nowhere to draw it" is never an acceptable outcome -- both callers reserve
 * UI_BANNER_H for it before deciding their own heights. Returns the y below
 * whatever it drew. */
static int alarm_banner(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        int cx, int cw, int y, int sc)
{
   uint32_t *px    = fb->bits;
   const char *msg = 0;
   uint32_t c      = 0;
   if (m->disc_alarmed) {
      msg = "STALE";
      /* A banner-only colour, for the same visibility-check reason as LOW. */
      c = 0xFF00D0FF;
   } else if (m->now - m->t <= AL_FRESH_S) {
      /* INCLUSIVE, exactly like alarm_zone (alarmlogic.c): the alarm fires AT
       * the limit, so the banner must appear at the limit too. While this read
       * `<` and the alarm read `<=`, a reading of exactly LOW sounded the alarm
       * and posted "Glucose LOW" while this screen drew no banner and coloured
       * the big number in-range -- the app contradicting its own alarm, which
       * is a reason to dismiss a real hypo. One threshold, one comparison. */
      if (m->glu >= 0 && m->glu <= m->alarm_low) {
         msg = "LOW";
         /* Deliberately NOT glu_color's red (0xFF0000FF): sharing that value
          * made the offline visibility check vacuous, because the big number
          * is drawn in it too whenever glu < 50 -- so "the banner is visible"
          * passed while the banner was entirely off-screen. A banner-only
          * colour is what makes that assertion mean something. */
         c = 0xFF2020E0;
      } else if (m->glu >= m->alarm_high) { /* inclusive, as above */
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

   return y;
}

/* Defined with the menu renderers below; the main screen's shortcut row needs
 * the SAME framed button the ADD menu draws, so the two cannot drift into
 * looking like different controls for the same action. */
static int menu_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                       int y, int w, int sc, const char *label, uint32_t col,
                       int action);

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
   /* The banner is NOT this block's cost in landscape -- it is drawn under the
    * plot in the other column, which is the only one with room beneath it.
    * Charging for it here is what floored this block's own vsc a whole step
    * and rendered the stats table in a tiny font: the row count never changed,
    * the space it was given did. Any block that derives its scale from the
    * space it receives can shrink this way. */
   int landscape = fb->width > fb->height;
   int needv     = 7 + (4 * 16) + 24 + (landscape ? 0 : UI_BANNER_H);
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
   int ncols = 4 + (5 * 5) + str_len(UI_LBL(m->units));
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
                  ac[1], ac[2], ac[3], ac[4], UI_LBL(m->units));
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
      /* AIR BETWEEN THE STATS TABLE AND THIS ROW. The table is a block of
       * numbers and this row is a set of controls, and at 2*sc they touched.
       *
       * Taken only out of SLACK, never out of the alarm banner: the banner is
       * the explicit on-screen statement that the user is out of range, and it
       * must render. Its cost below this row is a (7+9)*sc advance plus a
       * 5*sc-scaled glyph, i.e. 7*5*sc tall -- 51*sc in total. Try a full
       * line, then half, then none, and take the first that still leaves the
       * banner its room. */
      int air = lh;
      if (y + (2 * sc) + air + ph + (51 * sc) > fb->height)
         air = lh / 2;
      if (y + (2 * sc) + air + ph + (51 * sc) > fb->height)
         air = 0;
      int pyy = y + (2 * sc) + air;
      draw_str(px, fb, pxx, pyy, psc, "+", 0xFFCCCCCC);
      /* PINNED BUTTONS share this row, to the LEFT of the '+', which keeps
       * its corner. They divide whatever the row has left after the plus, so
       * one button is wide and three are narrow -- the row's edges never move
       * as shortcuts are added or removed.
       *
       * The LABEL shrinks, never the font. A button that cannot hold its full
       * label takes the abbreviation from ui_sc_tab; if even that will not fit
       * the button is drawn empty rather than clipped, because a half-word on
       * a control that logs insulin is worse than no word. */
      int nsc = 0;
      for (int i = 0; i < SC_MAX; i++)
         if (m->shortcut[i] > 0)
            nsc++;
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
         int bwid   = (availw - ((nsc - 1) * sgap)) / nsc;
         if (bwid > 6 * sc) { /* narrower than this and nothing can be read */
            int bx = rowl;
            for (int i = 0; i < SC_MAX; i++) {
               int code = m->shortcut[i];
               if (code <= 0)
                  continue;
               int slot = ui_shortcut_slot(code);
               if (slot < 0)
                  continue; /* a code this build no longer offers */
               const char *lbl = ui_shortcut_label(slot, nsc > 1);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc))
                  lbl = ui_shortcut_label(slot, 1);
               if (((str_len(lbl) * 6) - 1) * sc > bwid - (4 * sc))
                  lbl = "";
               (void)menu_button(fb, h, bx, pyy - (2 * sc), bwid, sc, lbl,
                                 0xFFCCCCCC, code);
               bx += bwid + sgap;
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
      add_hit(h, hx0, pyy - (3 * sc), cx + cw - hx0, ph + (6 * sc), ACT_MENU,
              MA_ADD_OPEN);
      y = pyy + ph;
   }

   /* The banner lives in whichever column has room UNDER it: this block in
    * portrait, the plot column in landscape. Drawing it in both is not merely
    * redundant -- in landscape this column has nothing left below the '+' row,
    * so the copy here lands off the bottom of the buffer. */
   if (!landscape)
      (void)alarm_banner(fb, m, cx, cw, y, sc); /* nothing follows it here */

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

/* ---- settings menu (portrait table; rows carry menu_action codes) ---- */

static const char *ui_orient_lbl[] = {"PORTRAIT", "LANDSCAPE", "GRAVITY",
                                      "SYSTEM"};
/* Must match disc_min[] in main.c -- these are the labels for those values. */
static const char *ui_disc_lbl[] = {"OFF", "15 MIN", "30 MIN", "60 MIN"};
/* Indexed by ND_OFF / ND_BEEP / ND_CHIRP. */
static const char *ui_newdata_lbl[] = {"OFF", "BEEP", "CHIRP"};

static const char *ui_perm_lbl[] = {"BT SCAN", "BT CONNECT", "NOTIFY"};

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

/* One menu row: name left, value right-aligned at `rx`; records a full-width
 * tap target carrying the menu_action `code` (code < 0 = read-only, no
 * target). `rx` is explicit so a screen that reserves a right-hand column --
 * the DEVICES screen's PRIMARY checkboxes -- can pull the value text clear of
 * it instead of drawing the two on top of each other. */
static void menu_row_at(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                        int sc, int lh, int rx, const char *name,
                        const char *value, uint32_t valcol, int code)
{
   uint32_t *px = fb->bits;
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   int vw = str_len(value) * 6 * sc;
   draw_str(px, fb, rx - vw, y, sc, value, valcol);
   if (code >= 0)
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, code);
}

/* The ordinary row: value right-aligned on the screen's own right margin. */
static void menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, int lh, const char *name, const char *value,
                     uint32_t valcol, int code)
{
   menu_row_at(fb, h, y, sc, lh, fb->width - (4 * sc), name, value, valcol,
               code);
}

/* Draw a screen title at `tsc`, stepping the scale DOWN until it fits the
 * width, and return the scale used.
 *
 * Titles that interpolate a device type overflow: "PAIR NEW ONETOUCH VERIO"
 * is 23 characters, which at double scale needs 276*sc of width and had only
 * ~1000 px on a 1080-wide phone. draw_str clips silently, so the screen read
 * "PAIR NEW ONETOUCH VE" -- a truncated device name on the confirmation that
 * asks which device to pair. One notch smaller reads as the same text; a cut
 * word does not. */
static int draw_title_fit(uint32_t *px, const struct ANativeWindow_Buffer *fb,
                          int x, int y, int tsc, const char *s, uint32_t col,
                          int maxw)
{
   int t = tsc;
   while (t > 1 && str_len(s) * 6 * t > maxw)
      t--;
   draw_str(px, fb, x, y, t, s, col);
   return t;
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
   int bh       = 25 * sc; /* label glyph is 7*sc -> 9*sc padding each side
                            * (4*sc -> 6*sc -> 9*sc; +50% padding app-wide) */
   int lw  = str_len(label) * 6 * sc;
   int lhh = 7 * sc;
   draw_frame(px, fb, x, y, w, bh, 0xFF888888);
   draw_str(px, fb, x + ((w - lw) / 2), y + ((bh - lhh) / 2), sc, label, col);
   add_hit(h, x, y, w, bh, ACT_MENU, action);
   return y + bh;
}

static void render_settings(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 3 * lh, ACT_MENU, MA_CLOSE);
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
   menu_row(fb, h, y, sc, lh, "DISPLAY", "", 0xFFFFFFFF, MA_DISPLAY_OPEN);
   y += 2 * lh;
   {
      int nlive = 0;
      int nconn = 0;
      for (int i = 0; i < m->nsensors; i++)
         if (!m->sensors[i].old) {
            nlive++;
            nconn += (m->sensors[i].connected != 0);
         }
      /* Bounded by MAX_SLOTS in practice, but the compiler only sees an int:
       * clamp so the format cannot be truncated. */
      if (nlive > 99)
         nlive = 99;
      if (nconn > 99)
         nconn = 99;
      char dv[24];
      (void)snprintf(dv, sizeof dv, "%d OF %d ON", nconn, nlive);
      menu_row(fb, h, y, sc, lh, "DEVICES", dv,
               (nlive > 0 && nconn == nlive) ? 0xFF33FF88 : 0xFFAAAAAA,
               MA_DEVICES_OPEN);
      y += 2 * lh;
   }
   menu_row(fb, h, y, sc, lh, "ALARM", "", 0xFFFFFFFF, MA_ALARM_OPEN);
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
      if (m->sound_on)
         draw_icon(px, fb, iax, y, sc, icon_speaker, 0xFF888888);
      if (m->vib_on)
         draw_icon(px, fb, iax + (6 * sc), y, sc, icon_vibrate, 0xFF888888);
      if (m->disc)
         draw_icon(px, fb, iax + (12 * sc), y, sc, icon_nolink, 0xFF888888);
      int nax = iax + (21 * sc); /* 3 cells + a half-cell group gap */
      if (m->nudge_sound)
         draw_icon(px, fb, nax, y, sc, icon_speaker, 0xFF888888);
      if (m->nudge_vib)
         draw_icon(px, fb, nax + (6 * sc), y, sc, icon_vibrate, 0xFF888888);
      if (m->newdata_mode)
         draw_icon(px, fb, nax + (12 * sc), y, sc, icon_dot, 0xFF888888);
   }
   y += 2 * lh;
   /* PERMISSIONS: one summary row -- green OK when everything a CGM needs
    * is granted, red CHECK otherwise -- opening the full submenu. */
   {
      int ok = m->perm[0] && m->perm[1] && m->perm[2] && m->batt_ok &&
               !m->bg_restricted;
      menu_row(fb, h, y, sc, lh, "PERMISSIONS", ok ? "OK" : "CHECK",
               ok ? 0xFF33FF88 : 0xFF4466FF, MA_PERMS_OPEN);
      y += 2 * lh;
   }
   /* REMOTE: the value is the push state -- and, when ON, the age of the
    * last push the server actually acknowledged (2xx) -- so whether
    * datapoints are leaving the phone AND arriving is visible without
    * opening the submenu. */
   char rmv[16] = "OFF";
   if (m->remote_on) {
      if (m->remote_last_ok > 0) {
         char rago[12];
         fmt_ago(m->now, m->remote_last_ok, rago, sizeof rago);
         (void)snprintf(rmv, sizeof rmv, "ON %s", rago);
      } else {
         /* no acknowledged push yet this launch: a bare ON ("ON NEVER"
          * read as if the feature had never worked) */
         (void)snprintf(rmv, sizeof rmv, "ON");
      }
   }
   menu_row(fb, h, y, sc, lh, "REMOTE", rmv,
            m->remote_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_REMOTE_OPEN);

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
               MA_EXPORT);
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
static int cgm_expired(const struct ui_sensor *s)
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
static const char *dev_state_abbrev(const char *st, char *out, int n)
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

static void render_devices(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_DEVICES_BACK);
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
   {
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
   }

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
   for (int i = 0; i < m->nsensors; i++) {
      if (m->sensors[i].old)
         nold++;
      else if (nlive_i < UI_MAX_SLOTS)
         idxs[nlive_i++] = i;
   }
   int npages = (nlive_i + cap - 1) / cap;
   if (npages < 1)
      npages = 1;
   int page = m->dev_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int first = page * cap;
   for (int pi = first; pi < first + cap && pi < nlive_i; pi++) {
      const struct ui_sensor *s = &m->sensors[idxs[pi]];
      int i                     = idxs[pi];
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
      menu_row_at(fb, h, y, sc, lh, vrx, name, val,
                  s->connected ? 0xFF33FF88 : 0xFFAAAAAA, MA_SENSOR + i);
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
         draw_checkbox(px, fb, cbx, y - ((cbs - gh) / 2), cbs, sc, s->primary,
                       s->primary ? 0xFF33FF88 : 0xFFAAAAAA);
         /* The target is the box's own rectangle, not a fixed line height:
          * the box is taller than a line now, and a target that stopped short
          * of it would leave its bottom edge dead. It still ends well above
          * the next row's target (pitch 24*sc), so no row is stolen. */
         add_hit(h, cbh, y - ((cbs - gh) / 2), fb->width - cbh, cbs, ACT_MENU,
                 MA_PRIM_PICK + i);
      }
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
      /* PITCH, not lh: the half-line of air that separates one device from the
       * next. ui_sensor_capacity divides by this same macro. */
      y += UI_DEV_PITCH(sc);
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
   /* PAGE NAV, in place of the old "N MORE NOT SHOWN". Same shape as OLD
    * DEVICES: "<" and ">" with the page count between them, drawn only when
    * there IS more than one page so a short list stays quiet. */
   if (npages > 1) {
      if (page > 0) {
         draw_str(px, fb, x, y, sc, "<", 0xFFFFFFFF);
         add_hit(h, 0, y - (3 * sc), fb->width / 3, lh, ACT_MENU,
                 MA_DEVPAGE_PREV);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, y, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * sc), y, sc, ">", 0xFFFFFFFF);
         add_hit(h, (2 * fb->width) / 3, y - (3 * sc), fb->width / 3, lh,
                 ACT_MENU, MA_DEVPAGE_NEXT);
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
      menu_row(fb, h, y, sc, lh, od, ">", 0xFFAAAAAA, MA_OLDDEV_OPEN);
      y += 2 * lh;
   }
   if (nlive < UI_MAX_SLOTS) {
      /* A real framed button, like SYNC NOW / FORGET DEVICE, not a plain row.
       */
      y += lh; /* separate it from the device list above */
      menu_button(fb, h, x, y, fb->width - (2 * x), sc, "ADD NEW DEVICE",
                  0xFFFFFFFF, MA_ADDSENSOR);
   }
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
   /* 3*lh after the title and 2*lh between rows -- the DISPLAY menu's spacing,
    * which is the house style for a settings submenu. This screen was the only
    * one still packed at the bare line height, so six rows of GRANTED / DENIED
    * ran together into a block that had to be read word by word to find the
    * one that was wrong. It fits: at 22 rows the budget already covered a
    * longer list than this one, and DISPLAY carries more rows at exactly this
    * spacing. */
   y += 3 * lh;

   for (int i = 0; i < 3; i++) {
      int g = m->perm[i];
      menu_row(fb, h, y, sc, lh, ui_perm_lbl[i], g ? "GRANTED" : "DENIED",
               g ? 0xFF33FF88 : 0xFF4466FF, MA_PERM + i);
      y += 2 * lh;
   }
   menu_row(fb, h, y, sc, lh, "BATTERY",
            m->batt_ok ? "UNRESTRICTED" : "OPTIMIZED",
            m->batt_ok ? 0xFF33FF88 : 0xFF4466FF, MA_BATTERY);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "STANDBY", ui_bucket_label(m->standby_bucket),
            (m->standby_bucket > 0 && m->standby_bucket <= 20) ? 0xFF33FF88
                                                               : 0xFFAA8844,
            -1);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "BG EXEC",
            m->bg_restricted ? "RESTRICTED" : "ALLOWED",
            m->bg_restricted ? 0xFF4466FF : 0xFF33FF88, MA_BGEXEC);
}

/* ---- ALARM submenu (opened from SETTINGS, or straight from the main
 * screen's alarm row left of "LOW") ---- */

/* A section caption: grey, no value, NO HIT BOX. It must not be tappable --
 * a target that looks like a row and does nothing teaches the user that rows
 * here sometimes do nothing, which is the last thing an alarm screen should
 * teach. menu_row with a negative code records none. */
static void menu_head(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                      int sc, int lh, const char *name)
{
   (void)h;
   (void)lh;
   uint32_t *px = fb->bits;
   /* DIMMER than a row name (0xCCCCCC) and underlined edge to edge. Colour
    * alone was not enough: drawn at the row colour with no value beside it,
    * a caption reads as a row whose value failed to render -- and on this
    * screen "a threshold with no value" is the single most alarming thing it
    * could accidentally say. The rule makes it structure, not content. */
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFF888888);
   draw_frame(px, fb, 4 * sc, y + (11 * sc), fb->width - (8 * sc), 1,
              0xFF444444);
}

/* One "LOW"/"HIGH" row: the value in display units, or OFF when the threshold
 * can never be reached (fmt_thresh). */
static void thresh_menu_row(struct ANativeWindow_Buffer *fb, struct hits *h,
                            int y, int sc, int lh, const char *name, int mgdl,
                            int units, int ishigh, int code)
{
   char v[8];
   char lv[16];
   fmt_thresh(mgdl, units, ishigh, v, sizeof v);
   if (thresh_off(mgdl, ishigh)) /* "OFF" carries no unit */
      (void)snprintf(lv, sizeof lv, "%s", v);
   else
      (void)snprintf(lv, sizeof lv, "%s %s", v, UI_LBL(units));
   menu_row(fb, h, y, sc, lh, name, lv, 0xFFFFFFFF, code);
}

static void render_alarm(struct ANativeWindow_Buffer *fb,
                         const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_ALARM_BACK);
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
   thresh_menu_row(fb, h, y, sc, lh, "LOW", m->alarm_low, m->units, 0,
                   MA_ALARM_LOW);
   y += 2 * lh;
   thresh_menu_row(fb, h, y, sc, lh, "HIGH", m->alarm_high, m->units, 1,
                   MA_ALARM_HIGH);
   y += 2 * lh;
   /* An enabled state reads GREEN, off stays white -- on/off is visible
    * from the colour alone, before reading a word. */
   menu_row(fb, h, y, sc, lh, "SOUND", m->sound_on ? "ON" : "OFF",
            m->sound_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_SOUND);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->vib_on ? "ON" : "OFF",
            m->vib_on ? 0xFF33FF88 : 0xFFFFFFFF, MA_VIB);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "DISCONNECT", ui_disc_lbl[(unsigned)m->disc & 3U],
            m->disc ? 0xFF33FF88 : 0xFFFFFFFF, MA_DISC);
   y += (5 * lh) / 2; /* two rows' worth, i.e. a blank line between sections */

   menu_head(fb, h, y, sc, lh, "NUDGE");
   y += (3 * lh) / 2;
   thresh_menu_row(fb, h, y, sc, lh, "LOW", m->nudge_low, m->units, 0,
                   MA_NUDGE_LOW);
   y += 2 * lh;
   thresh_menu_row(fb, h, y, sc, lh, "HIGH", m->nudge_high, m->units, 1,
                   MA_NUDGE_HIGH);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "SOUND", m->nudge_sound ? "ON" : "OFF",
            m->nudge_sound ? 0xFF33FF88 : 0xFFFFFFFF, MA_NUDGE_SOUND);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "VIBRATION", m->nudge_vib ? "ON" : "OFF",
            m->nudge_vib ? 0xFF33FF88 : 0xFFFFFFFF, MA_NUDGE_VIB);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "NEW DATAPOINT",
            ui_newdata_lbl[(unsigned)m->newdata_mode % 3U],
            m->newdata_mode ? 0xFF33FF88 : 0xFFFFFFFF, MA_NEWDATA);
}

/* ---- EXPORT DATA menu (opened from SETTINGS' EXPORT DATA button) ---- */

/* One checkbox row: name left, the checkbox ICON right (font.c: icon_box /
 * icon_boxck -- the same glyph language as every other symbol), green when
 * checked. The whole row toggles (generous target, like every menu_row). */
static void chk_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                    int sc, int lh, const char *name, int on, int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   draw_icon(px, fb, rx - (5 * sc), y, sc, on ? icon_boxck : icon_box,
             on ? 0xFF33FF88 : 0xFF888888);
   add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, code);
}

static void render_export(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_EXP_BACK);
   y += 3 * lh;

   static const char *const rng[3] = {"30 D", "1 Y", "ALL"};
   menu_row(fb, h, y, sc, lh, "RANGE",
            rng[(m->exp_range >= 0 && m->exp_range < 3) ? m->exp_range : 2],
            0xFFFFFFFF, MA_EXP_RANGE);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "GLUCOSE", m->exp_glu, MA_EXP_GLU);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "DEVICES", m->exp_dev, MA_EXP_DEV);
   y += 2 * lh;
   chk_row(fb, h, y, sc, lh, "INSULIN", m->exp_ins, MA_EXP_INS);
   y += lh;
   chk_row(fb, h, y, sc, lh, "WEIGHT", m->exp_wt, MA_EXP_WT);
   y += 3 * lh;

   /* The one acting control. With every section unticked there is nothing
    * to build, so the button greys out and records no target. */
   int any = m->exp_glu || m->exp_dev || m->exp_ins;
   int bw  = fb->width - (2 * x);
   if (any)
      menu_button(fb, h, x, y, bw, sc, "EXPORT", 0xFF33FF88, MA_EXP_GO);
   else
      draw_str(px, fb, x, y, sc, "NOTHING SELECTED", 0xFF888888);
}

/* ---- DISPLAY submenu (opened from SETTINGS) ---- */

static void render_display(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_DISPLAY_BACK);
   y += 3 * lh;

   menu_row(fb, h, y, sc, lh, "ORIENTATION",
            ui_orient_lbl[(unsigned)m->orient & 3U], 0xFFFFFFFF, MA_ORIENT);
   y += 2 * lh;
   /* "GLUCOSE UNITS", not "UNITS": with a weight unit directly below it, a
    * bare "UNITS" is the row that does not say what it governs. */
   menu_row(fb, h, y, sc, lh, "GLUCOSE UNITS", m->units ? "MMOL/L" : "MG/DL",
            0xFFFFFFFF, MA_UNITS);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "WEIGHT UNITS", wt_unit_name(m->wunits),
            0xFFFFFFFF, MA_WUNITS);
   y += 2 * lh;
   /* ALWAYS ON holds the screen awake while the app is open (the historical
    * behaviour); SYSTEM lets the normal display timeout apply. */
   menu_row(fb, h, y, sc, lh, "SCREEN", m->screen_on ? "ALWAYS ON" : "SYSTEM",
            0xFFFFFFFF, MA_SCREEN);
   y += 2 * lh;
   char pmv[20];
   char pmvv[8];
   fmt_glu(m->plot_max, m->units, pmvv, sizeof pmvv);
   (void)snprintf(pmv, sizeof pmv, "%s %s", pmvv, UI_LBL(m->units));
   menu_row(fb, h, y, sc, lh, "PLOT MAX", pmv, 0xFFFFFFFF, MA_PLOTMAX);
   y += 2 * lh;
   /* Insulin plot styling, one row PER TYPE -- each opens the full marker
    * picker (shape, colour, size) for that type. The value is the ACTUAL
    * glyph at its configured shape/colour/size, exactly like the
    * per-device MARKER row -- a preview, not a name. */
   static const char *const ins_lbl[2] = {"SLOW INSULIN MARKER",
                                          "FAST INSULIN MARKER"};
   for (int k = 0; k < 2; k++) {
      draw_str(px, fb, x, y, sc, ins_lbl[k], 0xFFCCCCCC);
      if (m->ins_marker[k] == MARK_HIDE) {
         int lw = str_len("OFF") * 6 * sc;
         draw_str(px, fb, rx - lw, y, sc, "OFF", 0xFFAAAAAA);
      } else {
         /* glyph reflects the configured SIZE too (plot scaling) */
         int gr = (2 * sc * m->ins_size[k]) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 5 * sc)
            gr = 5 * sc;
         plot_marker_glyph(px, fb->stride, fb->width, fb->height, rx - (6 * sc),
                           y + (3 * sc), gr, m->ins_marker[k],
                           ui_sensor_color(m->ins_color[k]));
      }
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, MA_INSMARK_OPEN + k);
      y += 2 * lh;
   }
   /* Status bar value vs plain app icon; lock-screen visibility; and a
    * way back for a swiped-away notification (it also reappears by
    * itself on the next reading). */
   menu_row(fb, h, y, sc, lh, "STATUS BAR", m->statbar_val ? "NUMBER" : "ICON",
            0xFFFFFFFF, MA_STATBAR);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "LOCK SCREEN", m->lockscr_val ? "SHOW" : "HIDE",
            0xFFFFFFFF, MA_LOCKSCR);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "NOTIFICATION", "REOPEN", 0xFFFFFFFF,
            MA_NOTIF_REOPEN);
}

/* ---- remote push (opened from SETTINGS) ---- */

static void render_remote(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_REMOTE_BACK);
   y += 3 * lh; /* the DISPLAY menu's title gap -- the house style */

   const char *sv =
       (m->remote_server && m->remote_server[0]) ? m->remote_server : 0;
   /* PUSH reflects what will actually happen: enabling without a server set
    * shows NO SERVER (amber), not ON -- nothing leaves the phone until one
    * exists, and pretending otherwise would be a silent lie. */
   const char *pv = "OFF";
   uint32_t pc    = 0xFFFFFFFF;
   if (m->remote_on && sv) {
      pv = "ON";
      pc = 0xFF33FF88;
   } else if (m->remote_on) {
      pv = "NO SERVER";
      pc = 0xFFAA8844;
   }
   /* Double pitch between the three setting rows: the blank line makes each
    * an easier touch target (menu_row's hit box spans its own row only). */
   menu_row(fb, h, y, sc, lh, "PUSH", pv, pc, MA_REMOTE_TOGGLE);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "SERVER", sv ? sv : "NOT SET",
            sv ? 0xFFFFFFFF : 0xFFAAAAAA, MA_REMOTE_IP);
   y += 2 * lh;
   char pt[8];
   (void)snprintf(pt, sizeof pt, "%d", m->remote_port);
   menu_row(fb, h, y, sc, lh, "PORT", pt, 0xFFFFFFFF, MA_REMOTE_PORT);
   y += 2 * lh;
   /* The account this phone syncs into, and whether it has been paired with
    * it yet. Both are needed before anything can be sent, so both are shown
    * whether or not they are set -- a missing one must not be invisible. */
   const char *em =
       (m->sync_email && m->sync_email[0]) ? m->sync_email : 0;
   menu_row(fb, h, y, sc, lh, "EMAIL", em ? em : "NOT SET",
            em ? 0xFFFFFFFF : 0xFFAAAAAA, MA_SYNC_EMAIL);
   y += 2 * lh;
   if (m->sync_paired) {
      menu_row(fb, h, y, sc, lh, "PAIRED", "YES", 0xFF33FF88, MA_SYNC_UNPAIR);
   } else if (!sv) {
      /* Name the ONE thing standing in the way, top to bottom, rather than
       * "SET BOTH" -- which says something is missing without saying what,
       * and is exactly as unhelpful when only one of them is. */
      menu_row(fb, h, y, sc, lh, "PAIR", "(SET SERVER)", 0xFFAAAAAA,
               MA_SYNC_PAIR);
   } else if (!em) {
      menu_row(fb, h, y, sc, lh, "PAIR", "(SET EMAIL)", 0xFFAAAAAA,
               MA_SYNC_PAIR);
   } else {
      menu_row(fb, h, y, sc, lh, "PAIR", "ENTER CODE", 0xFFFFFFFF,
               MA_SYNC_PAIR);
   }
   y += 2 * lh;
   /* STATUS: when the server last ACKNOWLEDGED something. With push on,
    * this is the one row that says whether the link actually works --
    * "ON" above only means the app intends to send. Not tappable (code
    * -1): it reports, it does not act. */
   {
      char st[16];
      uint32_t scol = 0xFFAAAAAA;
      if (!m->remote_on) {
         (void)snprintf(st, sizeof st, "--");
      } else if (m->remote_last_ok > 0) {
         char ago[12];
         fmt_ago(m->now, m->remote_last_ok, ago, sizeof ago);
         (void)snprintf(st, sizeof st, "%s AGO", ago);
         /* Fresh is green; a link that has not been acknowledged in over
          * ten minutes is amber, because that is a backlog building up. */
         scol = (m->now - m->remote_last_ok <= 600) ? 0xFF33FF88 : 0xFFAA8844;
      } else {
         (void)snprintf(st, sizeof st, "NEVER");
         scol = 0xFFAA8844;
      }
      menu_row(fb, h, y, sc, lh, "LAST SYNC", st, scol, -1);
      /* 2*lh like every other row pair on this screen. These two were the
       * only ones still butted together, which made them read as one
       * two-line row -- and they answer different questions (WHEN the last
       * batch landed vs WHAT the server said about it). */
      y += 2 * lh;
      /* ...and WHAT the server said. A refusal used to be invisible here:
       * the screen showed a happy PUSH ON while every batch was bouncing. */
      const char *rs =
          (m->remote_status && m->remote_status[0]) ? m->remote_status : "--";
      menu_row(fb, h, y, sc, lh, "LAST STATUS", rs,
               (rs[0] == '2') ? 0xFF33FF88 : 0xFFAAAAAA, -1);
      y += 2 * lh;
   }

   /* A bar while a sync is running. It is the only thing on this screen that
    * moves, and it exists because a sync is otherwise indistinguishable from
    * a server that is quietly refusing everything: LAST SYNC only changes
    * once, at the end, and only if the whole thing worked. */
   if (m->sync_active) {
      y += lh;
      int bar_x = 4 * sc;
      int bar_w = fb->width - (8 * sc);
      int bar_h = 3 * sc;
      draw_frame(px, fb, bar_x, y, bar_w, bar_h, 0xFF555555);
      int fill = ((bar_w - (2 * sc)) * m->sync_permille) / 1000;
      if (fill < 0)
         fill = 0;
      if (fill > bar_w - (2 * sc))
         fill = bar_w - (2 * sc);
      for (int by = y + sc; by < y + bar_h - sc; by++)
         for (int bxx = bar_x + sc; bxx < bar_x + sc + fill; bxx++)
            px[(by * fb->stride) + bxx] = 0xFF33FF88;
      y += bar_h + lh;
      char pctxt[16];
      (void)snprintf(pctxt, sizeof pctxt, "%d%%", m->sync_permille / 10);
      draw_str(px, fb, bar_x, y, sc, "SYNCING", 0xFF888888);
      draw_str(px, fb, fb->width - (4 * sc) - (str_len(pctxt) * 6 * sc), y, sc, pctxt,
               0xFF888888);
   }
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_SENSOR_BACK);
   if (m->sel < 0 || m->sel >= m->nsensors)
      return;
   const struct ui_sensor *s = &m->sensors[m->sel];

   draw_str(px, fb, x, y, tsc, s->label, 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* No second registration here: the one above the range guard covers
    * the identical rect with the identical action, so this was a leftover
    * from before that fix -- a duplicate target that burns a slot of the
    * UI_MAX_HITS budget and shows up in any reachability audit as a
    * shadowed control, masking a real one. */
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
      /* Same sentinel rule as the main screen's PRED row: `predicted` is a
       * 10-bit field whose 0x3ff (1023) means "no prediction", and no real
       * value exceeds Dexcom's 400 mg/dL cap. Without the upper bound this
       * row showed a bare 1023 while the main screen showed "--" for the same
       * sensor -- two answers to the same question, one of them a number the
       * user could act on. */
      if (s->predicted > 0 && s->predicted <= 400) {
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
      menu_row(fb, h, y, sc, lh, "WEAR", wd,
               s->wear_auto ? 0xFFFFFFFF : 0xFF33FF88, MA_WEAR);
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
   /* No second registration here: the one above the range guard covers
    * the identical rect with the identical action, so this was a leftover
    * from before that fix -- a duplicate target that burns a slot of the
    * UI_MAX_HITS budget and shows up in any reachability audit as a
    * shadowed control, masking a real one. */
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
   /* Title gap and row spacing follow the DISPLAY menu, like every other
    * settings screen: these three rows were packed at the bare line height
    * and read as one paragraph rather than three separate facts. */
   y += 3 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += 2 * lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->rescale_entry, m->units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->units));
      menu_row(fb, h, y, sc, lh, "TARGET", v, 0xFF33FF88, -1);
      y += 2 * lh;
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
      y += 2 * lh;
   }
   y += lh;
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
   /* Same spacing as render_rescale, which it mirrors. */
   y += 3 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, 0xFFFFFFFF, -1);
   y += 2 * lh;
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
      y += 2 * lh;
   }
   y += lh;
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
   int rx = fb->width - (4 * sc);
   (void)draw_title_fit(px, fb, x, y, tsc, title, 0xFFFFFFFF,
                        rx - x - (7 * tsc));
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
static const struct {
   int code;
   const char *full;
   const char *abbrev;
} ui_sc_tab[] = {
    {MA_INS_FAST,    "FAST INSULIN",     "FAST"   },
    {MA_INS_SLOW,    "SLOW INSULIN",     "SLOW"   },
    {MA_INSLOG_OPEN, "VIEW INSULIN LOG", "INS LOG"},
    {MA_WT_OPEN,     "WEIGHT",           "WEIGHT" },
    {MA_WTLOG_OPEN,  "VIEW WEIGHT LOG",  "WT LOG" },
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

/* Is this action already on the main screen? */
static int sc_on(const struct screen *m, int code)
{
   for (int i = 0; i < SC_MAX; i++)
      if (m->shortcut[i] == code)
         return 1;
   return 0;
}

static void render_addmenu(struct ANativeWindow_Buffer *fb,
                           const struct screen *m, struct hits *h)
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_CLOSE);
   y += 3 * lh;

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
   int gap = lh;
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
      int on   = sc_on(m, code);
      int by   = y;
      y = menu_button(fb, h, x, y, lbw, sc, ui_shortcut_label(i, 0), 0xFFFFFFFF,
                      code);
      /* The box shares the button's top and bottom edge (they are the same
       * height), and its TARGET is the whole column cell -- this one sits
       * right beside a button that logs a medication, so the two must not be
       * easy to confuse. Recorded AFTER the button and inside its row:
       * ui_hit_idx scans backwards, so the box wins its own rectangle while
       * the rest of the row still logs. */
      draw_checkbox(px, fb, cbx, by, cbs, sc, on, on ? 0xFF33FF88 : 0xFFAAAAAA);
      add_hit(h, x + lbw, by, bw - lbw, y - by, ACT_MENU, MA_SCTOGGLE + i);
      y += gap;
   }
   y += (2 * lh) - gap;

   draw_str(px, fb, x, y, sc, "DEVICES", 0xFF888888);
   y += lh;
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      y = menu_button(fb, h, x, y, bw, sc, sensor_disp_name(t), 0xFFFFFFFF,
                      MA_TYPE + t);
      y += gap;
   }
}

/* ---- LOG INSULIN: units / date / time, then CONFIRM or DISCARD. The type
 * (FAST/SLOW) is chosen on the ADD menu and fixed in this form's title. ---- */

/* One "NAME   <big value>" row; tapping the VALUE opens the keypad for
 * exact entry (arrows and steppers proved too fiddly at phone size --
 * typing the digits is faster and cannot overshoot). The whole right
 * half of the row is the tap target. Returns the y below the row. */
static int value_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, const char *name, const char *val, uint32_t vcol,
                     int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   int vsc      = 2 * sc;
   int vw       = str_len(val) * 6 * vsc;
   draw_str(px, fb, 4 * sc, y + (((7 * vsc) - (7 * sc)) / 2), sc, name,
            0xFFCCCCCC);
   draw_str(px, fb, rx - vw, y, vsc, val, vcol);
   add_hit(h, fb->width / 2, y - (4 * sc), fb->width / 2, (7 * vsc) + (8 * sc),
           ACT_MENU, code);
   return y + (7 * vsc) + (8 * sc);
}

/* One stepper block with PER-DIGIT vertical arrows:
 *
 *    NAME     ^  ^     ^  ^
 *             0  7  -  2  6
 *             v  v     v  v
 *
 * The value sits right-aligned at double scale; EVERY digit carries its
 * own up arrow above and down arrow below (punctuation gets none), each
 * with a tap target the digit's full cell wide and half the block tall --
 * comfortably past fingertip size. Codes run base + 2*digit + dir in
 * form order, so the shell maps each digit to its place quantum. Returns
 * the y below the block. */
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

   draw_str(px, fb, x, y, tsc, m->ins_edit ? "EDIT INSULIN" : "LOG INSULIN",
            0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* X discards -- nothing is written before an explicit CONFIRM. */
   /* 2*lh - 2*sc, not 2*lh: value_row's target starts at its y - 4*sc, so a
    * full close band reaches 4*sc into the TYPE row below it -- the same
    * overlap render_weight was already fixed for, and this form is the one
    * where the row underneath changes a logged dose. */
   add_hit(h, 0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc), ACT_MENU,
           MA_INS_DISCARD);
   y += 2 * lh;

   /* TYPE is editable in-form (the ADD menu's FAST/SLOW buttons only
    * pre-populate it); FAST shows in the log table's blue, at the same
    * large value size as the other editable fields. */
   y = value_row(fb, h, y, sc, "TYPE", m->ins_type == 1 ? "FAST" : "SLOW",
                 m->ins_type == 1 ? 0xFFFFAA66 : 0xFFFFFFFF, MA_INS_TYPE);
   y += lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; the form splits it into YEAR,
    * MM-DD and HH:MM fields. (An older split assumed "MM-DD HH:MM" -- one
    * format behind fmt_date -- so DATE showed a truncated year and TIME
    * showed a slice of the date.) */
   char dt[20];
   fmt_date(m->ins_t, m->tz_off, dt, sizeof dt);
   char yearp[8];
   char datep[8];
   char timep[8];
   str_snapshot(yearp, sizeof yearp, dt);
   if (str_len(yearp) > 4)
      yearp[4] = 0; /* "YYYY" */
   str_snapshot(datep, sizeof datep, (str_len(dt) > 5) ? dt + 5 : "");
   if (str_len(datep) > 5)
      datep[5] = 0; /* "MM-DD" */
   str_snapshot(timep, sizeof timep, (str_len(dt) > 11) ? dt + 11 : "");
   char val[20];
   (void)snprintf(val, sizeof val, "%d U", m->ins_units);
   y = value_row(fb, h, y, sc, "UNITS", val, 0xFFFFFFFF, MA_INS_EDIT);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, 0xFFFFFFFF, MA_INS_EDIT + 2);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, 0xFFFFFFFF, MA_INS_EDIT + 1);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, 0xFFFFFFFF, MA_INS_EDIT + 3);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide
    * rule, so reach-and-tap muscle memory can never commit by accident.
    * Editing adds DELETE (red) between the two. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_INS_DISCARD);
   y += 2 * lh;
   if (m->ins_edit) {
      y = menu_button(fb, h, x, y, bw, sc, "DELETE", 0xFF0000FF, MA_INS_DELETE);
      y += 2 * lh;
   }
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF00FF00, MA_INS_CONFIRM);
}

/* ---- delete-dose confirmation ----
 * The EDIT form's DELETE button lands here first; only the red DELETE on
 * this screen (MA_INSDEL_YES) actually removes the dose from the log. ---- */
static void render_insdel(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE DOSE?", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X",
            0xFFFFFFFF); /* close = cancel */
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_INSDEL_NO);
   y += 2 * lh;

   /* the dose about to be deleted, so a mis-tap from the log is caught */
   char dt[20];
   fmt_date(m->ins_t, m->tz_off, dt, sizeof dt);
   char line[40];
   (void)snprintf(line, sizeof line, "%d U %s", m->ins_units,
                  m->ins_type == 1 ? "FAST" : "SLOW");
   draw_str(px, fb, x, y, sc, line, 0xFFFFFFFF);
   y += lh;
   draw_str(px, fb, x, y, sc, dt, 0xFFFFFFFF);
   y += 2 * lh;
   static const char *const note[] = {
       "REMOVES THIS DOSE FROM THE",
       "INSULIN LOG. THIS CANNOT",
       "BE UNDONE.",
   };
   for (int i = 0; i < (int)(sizeof note / sizeof note[0]); i++) {
      draw_str(px, fb, x, y, sc, note[i], 0xFF888888);
      y += lh;
   }
   y += 2 * lh;

   /* CANCEL (safe, white) on top, DELETE (RED) well below -- the same
    * discipline as SCR_FORGET, so reach-and-tap muscle memory can never
    * delete by accident. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_INSDEL_NO);
   y += 3 * lh; /* wide gap so DELETE is not tapped by accident */
   menu_button(fb, h, x, y, bw, sc, "DELETE", 0xFF0000FF, MA_INSDEL_YES);
}

/* ---- INSULIN LOG: the dose tail as a when/type/units table, newest
 * first, paginated like OLD DEVICES so any length stays usable. ---- */

static void render_inslog(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "INSULIN LOG", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_INSLOG_BACK);
   y += 3 * lh;

   if (m->ins_nlog <= 0) {
      draw_str(px, fb, x, y, sc, "No doses logged yet.", 0xFF888888);
      return;
   }
   draw_str(px, fb, x, y, sc, "TIME              TYPE  UNITS", 0xFF888888);
   y += lh;

   /* Rows that fit between the header and a reserved bottom nav line. */
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
   int avail = fb->height - y - (2 * lh);
   int per   = avail / lh;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (m->ins_nlog + per - 1) / per;
   int page   = m->inslog_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < m->ins_nlog; r++) {
      /* the tail is oldest-first; the table shows newest first */
      int ti                  = m->ins_nlog - 1 - r;
      const struct ins_rec *d = &m->ins_log[ti];
      char when[20];
      char row[40];
      fmt_date(d->t, m->tz_off, when, sizeof when);
      (void)snprintf(row, sizeof row, "%s  %s %4d", when,
                     d->type == INS_FAST ? "FAST" : "SLOW", d->units);
      /* FAST doses in a soft blue, so the two types separate at a glance
       * (0xAABBGGRR: R=0x66 G=0xAA B=0xFF). */
      draw_str(px, fb, x, y, sc, row,
               d->type == INS_FAST ? 0xFFFFAA66 : 0xFFCCCCCC);
      /* The pencil is the affordance; the WHOLE row is the target (it
       * opens this dose in the EDIT INSULIN form). Centre the pencil in
       * the free column right of UNITS -- glued to the screen edge it
       * read as a tiny edge-of-screen button. */
      {
         int te = x + (29 * 6 * sc); /* right edge of the UNITS column */
         int ix = te + (((rx - te) - (5 * sc)) / 2);
         if (ix < te)
            ix = rx - (6 * sc); /* narrow screen: fall back to the edge */
         draw_icon(px, fb, ix, y, sc, icon_pencil, 0xFF888888);
      }
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, MA_INSLOG_EDIT + ti);
      y += lh;
   }

   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      /* Height `lh + 7*sc`, not `2*lh`: from `navy - 3*sc` a 2*lh box ends at
       * `height + 9*sc`, i.e. always 9*sc BELOW the buffer. The arrows drew
       * correctly and the top of each box was tappable, so it worked by
       * accident -- but an out-of-bounds target is exactly what the layout
       * gate forbids everywhere else, and the bottom strip of the finger
       * target simply did not exist. This ends flush with the bottom edge. */
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", 0xFFFFFFFF);
         add_hit(h, 0, navy - (3 * sc), fb->width / 3, lh + (7 * sc), ACT_MENU,
                 MA_INSLOG_PREV);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit(h, fb->width - (fb->width / 3), navy - (3 * sc), fb->width / 3,
                 lh + (7 * sc), ACT_MENU, MA_INSLOG_NEXT);
      }
   }
}

/* ---- WEIGHT: the entry form and the log table ----
 *
 * Deliberately the insulin form's shape and helpers (value_row, the same
 * keypad modes for date/time/year), because they are the same act: a number
 * the user typed, filed against an instant they can correct. Nothing here is
 * written before an explicit CONFIRM. */

/* Render a stored weight into the DISPLAY unit, e.g. "154.2 LB". Grams are
 * what the file holds; this is the only place the preference is applied. */
static void fmt_weight(long g, int units, char *out, int n)
{
   int t = wt_to_tenths(g, units);
   (void)snprintf(out, n, "%d.%d %s", t / 10, t % 10, wt_unit_name(units));
}

static void render_weight(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, m->wt_edit ? "EDIT WEIGHT" : "LOG WEIGHT",
            0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* X discards -- nothing is written before an explicit CONFIRM. */
   /* 2*lh - 2*sc, not 2*lh: value_row's target starts at its y - 4*sc, so a
    * full 2*lh close band reached 4*sc into the WEIGHT row below it. */
   add_hit(h, 0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc), ACT_MENU,
           MA_WT_DISCARD);
   y += 2 * lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; split it the way the insulin form
    * does, into YEAR / MM-DD / HH:MM. */
   char dt[20];
   fmt_date(m->wt_t, m->tz_off, dt, sizeof dt);
   char yearp[8];
   char datep[8];
   char timep[8];
   str_snapshot(yearp, sizeof yearp, dt);
   if (str_len(yearp) > 4)
      yearp[4] = 0;
   str_snapshot(datep, sizeof datep, (str_len(dt) > 5) ? dt + 5 : "");
   if (str_len(datep) > 5)
      datep[5] = 0;
   str_snapshot(timep, sizeof timep, (str_len(dt) > 11) ? dt + 11 : "");

   char val[20];
   (void)snprintf(val, sizeof val, "%d.%d %s", m->wt_tenths / 10,
                  m->wt_tenths % 10, wt_unit_name(m->wunits));
   y = value_row(fb, h, y, sc, "WEIGHT", val, 0xFFFFFFFF, MA_WT_EDIT);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, 0xFFFFFFFF, MA_WT_EDIT + 2);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, 0xFFFFFFFF, MA_WT_EDIT + 1);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, 0xFFFFFFFF, MA_WT_EDIT + 3);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide
    * rule (see the insulin form). Editing adds DELETE (red) between the
    * two, mirroring EDIT INSULIN; it only opens a confirmation, it never
    * deletes on the tap itself. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_WT_DISCARD);
   y += (3 * lh) / 2;
   if (m->wt_edit) {
      y = menu_button(fb, h, x, y, bw, sc, "DELETE", 0xFF4466FF, MA_WT_DELETE);
      y += (3 * lh) / 2;
   }
   (void)menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88, MA_WT_CONFIRM);
}

/* Confirm deleting one weight entry. Mirrors the insulin one: the value being
 * destroyed is spelled out, CANCEL is first and DELETE is below it. */
static void render_wtdel(struct ANativeWindow_Buffer *fb,
                         const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 20);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE?", 0xFF4466FF);
   y += 3 * lh;
   char wv[16];
   char when[20];
   /* The ORIGINAL entry, not the form's current values. Editing the weight
    * and then tapping DELETE showed the edited number here while
    * weight_delete removed the row that was on disk -- a confirmation naming
    * a different record than the one it destroys is worse than none. */
   fmt_weight(m->wt_orig_g, m->wunits, wv, sizeof wv);
   fmt_date(m->wt_orig_t, m->tz_off, when, sizeof when);
   draw_str(px, fb, x, y, sc, wv, 0xFFFFFFFF);
   y += lh;
   draw_str(px, fb, x, y, sc, when, 0xFFCCCCCC);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "This cannot be undone.", 0xFF888888);
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_WTDEL_NO);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "DELETE", 0xFF4466FF, MA_WTDEL_YES);
}

const int ui_wt_days[UI_WT_TABS] = {30, 90, 180, 365, 0}; /* 0 = everything */
static const char *const ui_wt_tab_lbl[UI_WT_TABS] = {"1M", "3M", "6M", "1Y",
                                                      "ALL"};

/* The window a span selects: the time and weight ranges the plot maps onto
 * its rectangle. ONE definition, used by the renderer AND the hit test --
 * computing it twice is how a scrub cursor ends up landing next to the point
 * the finger actually picked. */
struct wt_win {
   long tmin, tmax;
   long lo, hi;
   int n;
};

static void wt_window(const struct screen *m, long from, struct wt_win *w)
{
   w->n = 0;
   for (int i = 0; i < m->nwt; i++) {
      /* BOTH ends, matching the draw loop and the scrub picker. Only the
       * lower bound was applied here, so a mistyped year -- the entry form
       * accepts any date -- was excluded from the plot but still counted in
       * lo/hi, scaling the y axis to a weight that is never drawn and
       * flattening the real trace against the bottom of the frame. */
      if (m->wt[i].t < from || m->wt[i].t > m->now)
         continue;
      if (!w->n || m->wt[i].g < w->lo)
         w->lo = m->wt[i].g;
      if (!w->n || m->wt[i].g > w->hi)
         w->hi = m->wt[i].g;
      if (!w->n)
         w->tmin = m->wt[i].t;
      w->n++;
   }
   if (!w->n)
      return;
   /* THE X AXIS IS THE SELECTED SPAN, not the data's own extent.
    *
    * Taking tmin/tmax from the points made "1Y" with three months of data
    * draw those three months stretched across the full width -- identical to
    * the "3M" view, with only the date ticks to tell them apart, so the tab
    * appeared to do nothing. The right edge is now, the left edge is the span
    * the tab names, and a short history simply leaves the left of the plot
    * empty, which is the truth. ALL has no fixed start, so it spans from the
    * first entry to now. */
   if (from > 0)
      w->tmin = from;
   w->tmax = m->now;
   /* A flat or single-point window has no range to scale to; pad it so the
    * trace lands mid-plot instead of dividing by zero. */
   if (w->hi - w->lo < 200) {
      long mid = (w->hi + w->lo) / 2;
      w->lo    = mid - 100;
      w->hi    = mid + 100;
   }
   if (w->tmax - w->tmin < 60)
      w->tmax = w->tmin + 60;
}

#define WT_PAD 6 /* inset in sc units, so points near the edge stay whole */

static int wt_px(const struct wt_win *w, long t, int px0, int pw, int pad)
{
   return px0 + pad +
          (int)(((t - w->tmin) * (long)(pw - (2 * pad))) / (w->tmax - w->tmin));
}

/* Separate TOP and BOTTOM insets, not one pad.
 *
 * The y bounds are printed inside the plot -- upper at the top-left, lower
 * above the date ticks at the bottom-left -- so the data band has to stop
 * short of both, or a point lands on the text that names it. Insetting the
 * band is the same thing as expanding the y range, and it is exact: no point
 * can enter a reserved strip, whatever the data does. */
static int wt_py(const struct wt_win *w, long g, int py0, int ph, int pad_t,
                 int pad_b)
{
   return py0 + pad_t +
          (int)(((w->hi - g) * (long)(ph - pad_t - pad_b)) / (w->hi - w->lo));
}

long ui_wt_from(const struct screen *m)
{
   int tab = m->wt_tab;
   if (tab < 0 || tab >= UI_WT_TABS)
      tab = 0;
   return ui_wt_days[tab] > 0 ? m->now - ((long)ui_wt_days[tab] * 86400L) : 0;
}

int ui_wt_hit(const struct screen *m, int plot_x, int plot_w, int sc, int x)
{
   struct wt_win w;
   wt_window(m, ui_wt_from(m), &w);
   if (!w.n)
      return -1;
   int pad  = WT_PAD * sc;
   int best = -1;
   long bd  = 0;
   long lo  = ui_wt_from(m);
   for (int i = 0; i < m->nwt; i++) {
      if (m->wt[i].t < lo || m->wt[i].t > w.tmax)
         continue; /* same window the renderer uses, or the pick misses */
      int cx = wt_px(&w, m->wt[i].t, plot_x, plot_w, pad);
      long d = cx - x;
      if (d < 0)
         d = -d;
      if (best < 0 || d < bd) {
         bd   = d;
         best = i;
      }
   }
   return best;
}

/* The weight trend, drawn into [px0,px0+pw) x [py0,py0+ph).
 *
 * Its own plot, not plot.c's: that one is a glucose instrument -- fixed
 * mg/dL scale, in-range band, per-sensor markers. A weight trend needs none
 * of it and needs the one thing it cannot do, an AUTOSCALED y axis. A body
 * weight moves a few percent over a year, so a fixed axis would draw every
 * point as one flat line. Both axes are labelled with their real extremes,
 * because an autoscaled plot that does not state its range is the one that
 * misleads. */
static void wt_plot(uint32_t *px, const struct ANativeWindow_Buffer *fb,
                    const struct screen *m, int px0, int py0, int pw, int ph,
                    int sc, long from)
{
   draw_frame(px, fb, px0, py0, pw, ph, 0xFF444444);
   struct wt_win w;
   wt_window(m, from, &w);
   if (!w.n) {
      draw_str(px, fb, px0 + (4 * sc), py0 + (ph / 2), sc, "no data in range",
               0xFF888888);
      return;
   }
   int pad = WT_PAD * sc;
   /* Reserved strips: the upper bound's line at the top, and the lower
    * bound's line PLUS the date-tick line at the bottom. */
   /* + half a text height of breathing room, so the extreme datapoint clears
    * the bound that names it instead of just touching it. */
   int half  = (7 * sc) / 2;
   int pad_t = (13 * sc) + half;
   int pad_b = (22 * sc) + half;
   if (pad_t + pad_b > (ph * 2) / 3) { /* a very short plot: share it out */
      pad_t = ph / 6;
      pad_b = ph / 4;
   }

   /* LIGHT GRID. Dark enough to sit behind the trace rather than compete with
    * it: the trace is the data, the grid is only a ruler. Four horizontal
    * divisions, and vertical lines on the same columns the date labels use so
    * a label always names a line rather than floating between two. */
   const uint32_t grid = 0xFF2A2A2A;
   for (int i = 1; i < 4; i++) {
      int gy = py0 + pad_t + (((ph - pad_t - pad_b) * i) / 4);
      fill_rect(px, fb, px0 + 1, gy, pw - 2, 1, grid);
   }
   int nticks = 4; /* 3 interior + the right edge; see the label loop */
   for (int i = 1; i <= nticks; i++) {
      int gx = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
      fill_rect(px, fb, gx, py0 + 1, 1, ph - 2, grid);
   }

   /* THE Y BOUNDS, upper at the top-left and lower at the bottom-left, each
    * against the axis end it names. They used to be one "lo-hi" string in the
    * top-right corner, which states the range but not which end is which way
    * up. Always drawn: the reserved strips above keep the trace off them, so
    * unlike the old corner label there is nothing to suppress while
    * scrubbing. */
   {
      char slab[16];
      fmt_weight(w.hi, m->wunits, slab, sizeof slab);
      draw_str(px, fb, px0 + (4 * sc), py0 + (3 * sc), sc, slab, 0xFF888888);
      fmt_weight(w.lo, m->wunits, slab, sizeof slab);
      draw_str(px, fb, px0 + (4 * sc), py0 + ph - (19 * sc), sc, slab,
               0xFF888888);
   }

   /* DATE TICKS on the vertical grid lines. Without them the x axis is
    * unlabelled and "1Y" could be any year. MM-DD only -- the span already
    * says how far back this is, and a full date at this size would collide
    * with its neighbour. */
   for (int i = 1; i <= nticks; i++) {
      int gx  = px0 + pad + (((pw - (2 * pad)) * i) / (nticks + 1));
      long tt = w.tmin + (((w.tmax - w.tmin) * i) / (nticks + 1));
      char dt[20];
      char md[8];
      fmt_date(tt, m->tz_off, dt, sizeof dt);
      str_snapshot(md, sizeof md, (str_len(dt) > 5) ? dt + 5 : "");
      if (str_len(md) > 5)
         md[5] = 0; /* "MM-DD" */
      int lw = str_len(md) * 6 * sc;
      draw_str(px, fb, gx - (lw / 2), py0 + ph - (9 * sc), sc, md, 0xFF777777);
   }

   int prevx = 0;
   int prevy = 0;
   int have  = 0;
   for (int i = 0; i < m->nwt; i++) {
      /* Both ends. A point AFTER the window -- the entry form allows any date,
       * so a mistyped year lands one decades ahead -- mapped past the right
       * edge and drew outside the frame, over the tabs above it. */
      if (m->wt[i].t < from || m->wt[i].t > w.tmax)
         continue;
      int cx = wt_px(&w, m->wt[i].t, px0, pw, pad);
      int cy = wt_py(&w, m->wt[i].g, py0, ph, pad_t, pad_b);
      /* Join consecutive points: a weight trend is read as a line, and dots
       * alone at one a day over a year are unreadable. 0xAABBGGRR, so this is
       * the soft blue the insulin log already uses. */
      if (have) {
         int dx = cx - prevx;
         int dy = cy - prevy;
         int ax = dx < 0 ? -dx : dx;
         int ay = dy < 0 ? -dy : dy;
         int st = ax > ay ? ax : ay;
         for (int k = 1; k <= st && st > 0; k++)
            fill_rect(px, fb, prevx + ((dx * k) / st), prevy + ((dy * k) / st),
                      sc, sc, 0xFFFFAA66);
      }
      /* White, like the glucose trace. The SCRUBBED point is redrawn below in
       * UI_HILITE grey -- the same "white normally, grey when picked" pair
       * plot_render uses, so both plots read the same way. */
      fill_rect(px, fb, cx - (2 * sc), cy - (2 * sc), 4 * sc, 4 * sc,
                0xFFFFFFFF);
      prevx = cx;
      prevy = cy;
      have  = 1;
   }

   /* SCRUB CURSOR: a full-height rule through the picked point and its value
    * spelled out, so the number under the finger is readable rather than
    * estimated off the axis. */
   if (m->wt_scrub >= 0 && m->wt_scrub < m->nwt &&
       m->wt[m->wt_scrub].t >= from) {
      const struct wt_rec *p = &m->wt[m->wt_scrub];
      int cx                 = wt_px(&w, p->t, px0, pw, pad);
      int cy                 = wt_py(&w, p->g, py0, ph, pad_t, pad_b);
      fill_rect(px, fb, cx, py0 + 1, 1, ph - 2, 0xFF666666);
      fill_rect(px, fb, cx - (3 * sc), cy - (3 * sc), 6 * sc, 6 * sc,
                UI_HILITE);
   }
}

static void render_wtlog(struct ANativeWindow_Buffer *fb,
                         const struct screen *m, struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "WEIGHT LOG", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, MA_WTLOG_BACK);
   y += 3 * lh;

   if (m->nwt <= 0) {
      draw_str(px, fb, x, y, sc, "No weights logged yet.", 0xFF888888);
      return;
   }

   /* THE SCREEN IS SPLIT: table above, trend below. The plot gets the bottom
    * ~40% and everything else is laid out against what is left, so the table
    * cannot grow into the plot on a tall screen nor the plot squeeze the
    * table to nothing on a short one. */
   /* RESERVE THE SYSTEM GESTURE BAR. The activity draws edge to edge, so the
    * phone's own navigation pill sits ON TOP of the bottom of this surface --
    * the plot's bottom edge and its date labels were rendering underneath it.
    * Everything else in the app happens to stop short of the bottom; this
    * screen is the first to reach it, so it is the first to collide. */
   int sysbar   = fb->height / 24;
   int plot_h   = (fb->height * 2) / 5;
   int tabs_h   = 2 * lh; /* the span tabs */
   int plot_top = fb->height - plot_h - sysbar;
   int tabs_y   = plot_top - tabs_h;
   /* 6*sc, not 2: the nav box runs from nav_y - 3*sc for lh + 7*sc, i.e. to
    * nav_y + lh + 4*sc, so a 2*sc gap left its bottom 2*sc inside the tab row
    * -- and ui_hit_idx scans backwards, so the TAB won and the bottom sliver
    * of the "next page" arrow silently changed the plot's span instead. */
   /* The blank line goes BELOW the pagination, not above it: the page counter
    * belongs to the TABLE it pages, and a gap between them grouped it with
    * the plot instead -- the one thing it has nothing to do with. */
   int nav_y = tabs_y - (2 * lh) - (6 * sc);
   /* A blank line between the table and the pagination row, so the two halves
    * of this screen read as two things rather than one crowded column. */

   draw_str(px, fb, x, y, sc, "TIME              WEIGHT", 0xFF888888);
   y += lh;

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
   int avail = nav_y - y; /* rows run right down to the pagination */
   int per   = (avail > 0) ? avail / lh : 1;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (m->nwt + per - 1) / per;
   int page   = m->wt_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < m->nwt; r++) {
      int ti                 = m->nwt - 1 - r; /* tail is oldest-first */
      const struct wt_rec *w = &m->wt[ti];
      char when[20];
      char wv[16];
      char row[48];
      fmt_date(w->t, m->tz_off, when, sizeof when);
      fmt_weight(w->g, m->wunits, wv, sizeof wv);
      (void)snprintf(row, sizeof row, "%s  %s", when, wv);
      draw_str(px, fb, x, y, sc, row, 0xFFCCCCCC);
      /* The pencil is the affordance; the WHOLE row is the target, opening
       * this entry in the EDIT WEIGHT form (the insulin log's pattern). */
      {
         int te = x + (24 * 6 * sc);
         int ix = te + (((rx - te) - (5 * sc)) / 2);
         if (ix < te)
            ix = rx - (6 * sc);
         draw_icon(px, fb, ix, y, sc, icon_pencil, 0xFF888888);
      }
      add_hit(h, 0, y - (3 * sc), fb->width, lh, ACT_MENU, MA_WTLOG_EDIT + ti);
      y += lh;
   }

   if (npages > 1) {
      if (page > 0) {
         draw_str(px, fb, x, nav_y, tsc, "<", 0xFFFFFFFF);
         add_hit(h, 0, nav_y - (3 * sc), fb->width / 3, lh + (7 * sc), ACT_MENU,
                 MA_WTLOG_PREV);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, nav_y, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), nav_y, tsc, ">", 0xFFFFFFFF);
         add_hit(h, fb->width - (fb->width / 3), nav_y - (3 * sc),
                 fb->width / 3, lh + (7 * sc), ACT_MENU, MA_WTLOG_NEXT);
      }
   }

   /* Span tabs -- OR the scrub readout, exactly as the glucose plot does it:
    * while a finger is down the tab row becomes the value under it, and the
    * tabs come back the moment it lifts. The readout needs a fixed, roomy
    * home, and drawing it inside the plot put it over the trace it describes.
    */
   int tab = m->wt_tab;
   if (tab < 0 || tab >= UI_WT_TABS)
      tab = 0;
   int colw = (fb->width - (2 * x)) / UI_WT_TABS;
   int trow = 14 * sc; /* render_glucose's tab row height */
   int laby = plot_top - trow + ((trow - (7 * sc)) / 2);
   if (m->wt_scrub >= 0 && m->wt_scrub < m->nwt) {
      const struct wt_rec *p = &m->wt[m->wt_scrub];
      char wv[16];
      char when[24];
      char line[48];
      fmt_weight(p->g, m->wunits, wv, sizeof wv);
      fmt_date(p->t, m->tz_off, when, sizeof when);
      (void)snprintf(line, sizeof line, "%s   %s", when, wv);
      int tsc2 = 2 * sc;
      while (tsc2 > sc && str_len(line) * 6 * tsc2 > fb->width - (4 * sc))
         tsc2--;
      int lw = str_len(line) * 6 * tsc2;
      /* White, and the point marked in UI_HILITE grey -- the same pair the
       * glucose plot uses. Green means "on / enabled" everywhere else in this
       * app; a readout is neither. Top-aligned in the tab row, as there. */
      draw_str(px, fb, (fb->width - lw) / 2, plot_top - trow, tsc2, line,
               0xFFFFFFFF);
      /* No tab targets while scrubbing: the row is not showing tabs, and a
       * target that does not match what is drawn is how a drag ends up
       * changing the span it was only trying to read. */
   } else {
      for (int i = 0; i < UI_WT_TABS; i++) {
         int lw   = str_len(ui_wt_tab_lbl[i]) * 6 * sc;
         int tabx = x + (i * colw);
         /* Same construction as render_glucose: the labels sit in a 14*sc row
          * ending at the plot's top edge, vertically centred in it, so the
          * gap between a tab and the plot is identical on both screens. The
          * TARGET still spans the whole band above, which is empty. */
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc, ui_wt_tab_lbl[i],
                  i == tab ? 0xFFFFFFFF : 0xFF888888);
         add_hit(h, tabx, tabs_y, colw, tabs_h, ACT_MENU, MA_WTTAB + i);
      }
   }

   int pw = fb->width - (2 * x);
   wt_plot(px, fb, m, x, plot_top, pw, plot_h, sc, ui_wt_from(m));
   /* The whole plot scrubs; the shell resolves the point via ui_wt_hit. */
   /* arg carries sc: the shell needs the SAME scale the plot was drawn at to
    * map a finger x back to a point, and re-deriving it there would be a
    * second copy of the layout that can drift. */
   add_hit(h, x, plot_top, pw, plot_h, ACT_SCRUB, sc);
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
         add_hit(h, 0, navy - (3 * sc), fb->width / 3, lh + (7 * sc), ACT_MENU,
                 MA_OLDPAGE_PREV);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (1 * 6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit(h, (2 * fb->width) / 3, navy - (3 * sc), fb->width / 3,
                 lh + (7 * sc), ACT_MENU, MA_OLDPAGE_NEXT);
      }
   }
}

/* ---- MARKER / COLOR pickers: a full list of options, each with a live glyph,
 * so the user sees every choice before selecting. The title row returns to the
 * device's own menu (MA_SENSOR + its slot). ---- */
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
   /* The picker edits either the SELECTED SLOT's styling (old devices
    * keep their slot, so this works for them exactly like a live one) or,
    * in markpick_ins mode, the INSULIN marker -- shape only, drawn white,
    * X back to settings. */
   int ins  = (m->markpick_ins >= 0); /* which insulin type, or a sensor */
   int ity  = (ins && m->markpick_ins == INS_FAST) ? INS_FAST : INS_SLOW;
   int okk  = !ins && (m->sel >= 0 && m->sel < m->nsensors);
   int back = MA_INSMARK_BACK;
   int curm = m->ins_marker[ity];
   int curc = m->ins_color[ity];
   int curs = m->ins_size[ity];
   if (!ins) {
      back = MA_SENSOR + (m->sel >= 0 ? m->sel : 0);
      curm = okk ? m->sensors[m->sel].marker : 0;
      curc = okk ? m->sensors[m->sel].color : 0;
      /* size 0 means UNSET, i.e. the default (sensors.h) -- so resolve it the
       * way the list row and the SIZE preview already do. Passing the raw 0
       * through made the SIZE row highlight no cell at all, so a sensor that
       * had never been styled showed no current selection. */
      curs = (okk && m->sensors[m->sel].size >= 1) ? m->sensors[m->sel].size
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
   add_hit(h, 0, y - (3 * sc), fb->width, 2 * lh, ACT_MENU, back);
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
      if (cell > cellmax)
         cell = cellmax;
      int gx = x + ((gw - (cols * cell)) / 2);
      for (int i = 0; i < UI_NCOLORS; i++) {
         int cx = gx + (i * cell);
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

/* The dot is here for host names (SERVER): without it "pancra.org" cannot
 * be typed at all, and the at-sign for the account email. Sensor labels
 * simply never use either. */
const char ui_label_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.@";
#define UI_LABEL_COLS 6

/* code = 4 digits; plot max / cal / rescale = 3; IP = a full dotted quad's
 * 15; port = 5; the four thresholds (10-13) = 4, which holds "999" and the
 * mmol/L form "55.5". Index is struct screen's kp_mode. */
int ui_kp_slots(int mode)
{
   /* mode 14 (WEIGHT) takes 4: "1542" is 154.2 lb, and 4 digits also holds a
    * three-digit kilogram weight to a tenth. It was missing from this table,
    * which happened to yield 4 anyway -- the out-of-range fallback is mode 0,
    * the pairing code, which is also 4. Right answer, wrong reason, and it
    * would have silently become 4-when-it-should-be-something-else the moment
    * either changed. */
   /* mode 14 (WEIGHT) takes 5: "162.4" -- three digits, a dot and a tenth. */
   /* Mode 15 is the SYNC pairing code: SIX digits, not four. The server shows
    * six and burns the code after three attempts, so a field that could only
    * hold four would make pairing impossible rather than merely awkward. */
   static const int slots_for[UI_KP_MODES] = {4, 3, 3, 3, 15, 5, 2, 4,
                                              4, 4, 4, 4, 4,  4, 5, 6};
   return slots_for[(mode >= 0 && mode < UI_KP_MODES) ? mode : 0];
}

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

   draw_str(px, fb, x, y, tsc,
            m->label_field == 1   ? "SERVER"
            : m->label_field == 2 ? "EMAIL"
                                  : "NAME",
            0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;

   /* What has been typed so far, with a caret so an empty field is still
    * obviously an entry field.
    *
    * WRAPPED, not shrunk. This used to be one line in a 16-byte buffer, which
    * was fine for a sensor name and useless for an email address: anything
    * past 15 characters simply was not shown. Shrinking the text to fit would
    * make a long address unreadable exactly when the user most needs to check
    * it letter by letter, so the text keeps its size and takes as many lines
    * as it needs. */
   char shown[80];
   const char *en = m->entry ? m->entry : "";
   int k          = 0;
   while (en[k] && k < (int)sizeof shown - 2) {
      shown[k] = en[k];
      k++;
   }
   shown[k]     = '_';
   shown[k + 1] = 0;
   int dsc      = 2 * sc;
   int percol   = fb->width / (6 * dsc);  /* characters that fit on one line */
   if (percol < 8)
      percol = 8;
   int shownlen = str_len(shown);
   for (int off = 0; off < shownlen; off += percol) {
      char line[80];
      int len = shownlen - off;
      if (len > percol)
         len = percol;
      for (int i = 0; i < len; i++)
         line[i] = shown[off + i];
      line[len] = 0;
      int dw    = len * 6 * dsc;
      draw_str(px, fb, (fb->width - dw) / 2, y, dsc, line, 0xFF33FF88);
      y += 7 * dsc;
   }
   y += 8 * sc;
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

   /* Each type is a standard menu_button -- the SAME control every other
    * screen uses, at the same height, so buttons look and feel identical
    * across the app (this screen used to grow bespoke, much taller
    * buttons). The OneTouch shows its full name (the stored type name
    * stays "ONETOUCH" so the 16-char device label does not truncate). */
   int bw = fb->width - (2 * x);
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      y = menu_button(fb, h, x, y, bw, sc, sensor_disp_name(t), 0xFFFFFFFF,
                      MA_TYPE + t);
      y += lh;
   }
}

/* One keypad key: framed cell, centred label (in `col`), full-cell ACT_MENU
 * target. code < 0 = a DISABLED key: drawn but recording no target, so it
 * cannot fire (the grayed-out OK of an over-limit entry). */
static void pad_key(struct ANativeWindow_Buffer *fb, struct hits *h, int cx,
                    int cy, int cw, int ch, int ksc, const char *lab, int code,
                    uint32_t col)
{
   uint32_t *px = fb->bits;
   draw_frame(px, fb, cx, cy, cw, ch, 0xFF555555);
   int lw  = str_len(lab) * 6 * ksc;
   int lhh = 7 * ksc;
   draw_str(px, fb, cx + ((cw - lw) / 2), cy + ((ch - lhh) / 2), ksc, lab, col);
   if (code >= 0)
      add_hit(h, cx, cy, cw, ch, ACT_MENU, code);
}

/* The four threshold-entry keypad modes -- ALARM LOW/HIGH and NUDGE LOW/HIGH.
 * They share every behaviour that distinguishes a threshold entry from the
 * other keypads: the unit suffix, the MAX line, the dot key in mmol/L, and the
 * OK ceiling. One predicate, so adding the nudge pair could not leave one of
 * those four sites behind -- which would have meant, for instance, a NUDGE
 * screen in mmol/L with no '.' key and therefore no way to type 5.5. */
static int kp_thresh(int mode)
{
   return mode >= 10 && mode <= 13;
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
   /* TABLE-DRIVEN, AND THE FALLBACK IS AN ERROR, NOT A SCREEN.
    *
    * This was an if/else chain ending in `else -> pairing keypad`, so "no
    * title for this mode" rendered a REAL, PLAUSIBLE screen: a mode added
    * without a title silently became PAIR NEW <sensor>. The WEIGHT keypad did
    * exactly that -- tapping a weight opened the sensor-pairing flow and
    * nothing about it looked wrong. A default branch must never name a
    * different feature. Pairing is mode 0 and ONLY mode 0; anything unknown
    * says so, in red, where it cannot be mistaken for working. */
   static const char *const kp_titles[UI_KP_MODES] = {
       0,             /* 0: pairing -- title built from add_type below */
       "PLOT MAX",    /* 1 */
       "CALIBRATION", /* 2 */
       "RESCALE",     /* 3 */
       "SERVER",      /* 4: unused since SERVER became a name */
       "REMOTE PORT", /* 5 */
       "UNITS",       /* 6 */
       "DATE (MMDD)", /* 7 */
       "TIME (HHMM)", /* 8 */
       "YEAR",        /* 9 */
       "ALARM LOW",   /* 10 */
       "ALARM HIGH",  /* 11 */
       "NUDGE LOW",   /* 12 */
       "NUDGE HIGH",  /* 13 */
       "WEIGHT",      /* 14 */
       "PAIRING CODE",/* 15 */
   };
   uint32_t title_col = 0xFFFFFFFF;
   if (m->kp_mode == 0) {
      (void)snprintf(pair_title, sizeof pair_title, "PAIR NEW %s",
                     m->add_type ? m->add_type : "SENSOR");
   } else if (m->kp_mode > 0 && m->kp_mode < UI_KP_MODES &&
              kp_titles[m->kp_mode]) {
      kp_title = kp_titles[m->kp_mode];
   } else {
      (void)snprintf(pair_title, sizeof pair_title, "BAD KP MODE %d",
                     m->kp_mode);
      title_col = 0xFF4466FF; /* red: a bug, not a feature */
   }
   /* Leave room for the X, which is right-aligned at 6*tsc. */
   (void)draw_title_fit(px, fb, x, y, tsc, kp_title, title_col,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   y += 2 * lh;
   if (kp_thresh(m->kp_mode)) {
      /* the accepted ceiling, in the entry's own units, then a blank row;
       * the key grid below sizes itself into whatever height remains */
      char mv[8];
      char mx[24];
      fmt_glu(AL_ENTRY_MAX, m->units, mv, sizeof mv);
      (void)snprintf(mx, sizeof mx, "MAX: %s %s", mv, UI_LBL(m->units));
      draw_str(px, fb, x, y, sc, mx, 0xFF888888);
      y += 2 * lh;
   }

   /* Entry field: one underscore per slot, replaced by digits as typed, so the
    * width never shifts. Plot-max shows the unit after the value; pair shows
    * the 4 code digits; the remote IP gets a full dotted quad's 15 slots and
    * the remote port a TCP port's 5. dsc is sized for the widest label so the
    * field -- and the keypad below -- is identical across modes. */
   int nslots = ui_kp_slots(m->kp_mode);
   /* glucose entries carry the unit label: plot max / cal / rescale and the
    * two alarm thresholds */
   int has_unit   = (m->kp_mode >= 1 && m->kp_mode <= 3) ||
                    kp_thresh(m->kp_mode) || m->kp_mode == 14;
   const char *en = m->entry ? m->entry : "";
   char shown[24];
   int k = 0;
   /* Both bounds are belt-and-braces: the widest mode is 15 slots plus
    * " MMOL/L". snprintf returns what it WOULD have written, so an unclamped
    * k would index past `shown` the moment a longer unit or a wider keypad
    * appeared -- a silent stack overrun for a one-character change. */
   for (int i = 0; i < nslots && k < (int)sizeof shown - 1; i++)
      shown[k++] = *en ? *en++ : '_'; /* typed digits, then '_' for the rest */
   if (has_unit) {
      /* The WEIGHT keypad is the one entry that is not a glucose value, so
       * it must not be labelled with the glucose unit. */
      const char *ul =
          (m->kp_mode == 14) ? wt_unit_name(m->wunits) : UI_LBL(m->units);
      int w = snprintf(shown + k, sizeof shown - k, " %s", ul);
      if (w > 0)
         k += (w < (int)sizeof shown - k) ? w : (int)sizeof shown - k - 1;
   }
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
    * -- and the two alarm-threshold modes in mmol/L, where the value carries
    * a decimal ("5.5") -- need a 13th key ('.'), so they swap to a 5-row
    * layout: the dot takes 0's old cell, 0 and DEL shift right, and OK
    * becomes a full-width bottom row (which also makes the confirm harder to
    * fat-finger from DEL). */
   int dotkey = (kp_thresh(m->kp_mode) && m->units) ||
                m->kp_mode == 14;
   int iprows = dotkey ? 5 : 4;
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
   static const char *keys[12]   = {"7", "8", "9", "4",   "5", "6",
                                    "1", "2", "3", "0", "DEL", "OK"};
   static const int acts[12]     = {107, 108, 109, 104, 105, 106,
                                    101, 102, 103, 100, 110, MA_OK};
   static const char *ipkeys[12] = {"7", "8", "9", "4", "5",   "6",
                                    "1", "2", "3", ".", "0", "DEL"};
   static const int ipacts[12]   = {107, 108, 109, 104,    105, 106,
                                    101, 102, 103, MA_DOT, 100, 110};
   const char **kk               = dotkey ? ipkeys : keys;
   const int *aa                 = dotkey ? ipacts : acts;
   /* A value too LARGE for this mode's field DISABLES OK -- drawn gray,
    * recording no target, so an over-limit entry cannot even be submitted;
    * deleting digits re-enables it. Only modes with a numeric ceiling
    * participate (glucose entries 400, the alarm thresholds AL_ENTRY_MAX,
    * the TCP port 65535); a code/IP/date entry never disables here. */
   long okmax = 0; /* this mode's ceiling in comparable units; 0 = none */
   long okval = 0;
   {
      long ipart = 0;
      int frac   = -1;
      int sdot   = 0;
      /* NOT `en` -- the entry-field loop above consumed that pointer */
      for (const char *p = m->entry ? m->entry : ""; *p; p++) {
         if (*p == '.') {
            sdot = 1;
         } else if (*p >= '0' && *p <= '9') {
            if (!sdot)
               ipart = (ipart * 10) + (*p - '0');
            else if (frac < 0)
               frac = *p - '0';
         }
      }
      if (m->kp_mode >= 1 && m->kp_mode <= 3) {
         /* display units, mmol as TENTHS (no dot key in these modes) */
         okval = m->units ? (ipart * 18) / 10 : ipart;
         okmax = 400;
      } else if (kp_thresh(m->kp_mode)) {
         long tenths = (ipart * 10) + (frac > 0 ? frac : 0);
         okval       = m->units ? (tenths * 18) / 10 : ipart;
         okmax       = AL_ENTRY_MAX;
      } else if (m->kp_mode == 5) {
         okval = ipart;
         okmax = 65535;
      }
   }
   int ok_dead = okmax > 0 && okval > okmax;
   for (int r = 0; r < 4; r++)
      for (int col = 0; col < 3; col++) {
         int idx      = (r * 3) + col;
         int code     = aa[idx];
         uint32_t kcl = 0xFFFFFFFF;
         if (code == MA_OK && ok_dead) {
            code = -1;
            kcl  = 0xFF555555;
         }
         pad_key(fb, h, gm + (col * cw), y + (r * ch), cw - (2 * sc),
                 ch - (2 * sc), ksc, kk[idx], code, kcl);
      }
   if (dotkey)
      pad_key(fb, h, gm, y + (4 * ch), (3 * cw) - (2 * sc), ch - (2 * sc), ksc,
              "OK", ok_dead ? -1 : MA_OK, ok_dead ? 0xFF555555 : 0xFFFFFFFF);
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
   (void)draw_title_fit(px, fb, x, y, tsc, sel_title, 0xFFFFFFFF,
                        rx - x - (7 * tsc));
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
   /* devs may be null with a stale ndev; every other list here is guarded the
    * same way. The renderer is a pure function of the model and must not
    * trust two fields to agree. */
   int n = (m->devs && m->ndev > 0) ? m->ndev : 0;
   if (n > 16)
      n = 16;
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

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h)
{
   h->n        = 0;
   h->overflow = 0;
   /* TRUE black, zero photons on an OLED -- not the old 0xFF181818 wash. */
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
      case SCR_PAIRCONF: render_pairconf(fb, m, h); break;
      case SCR_ADDMENU: render_addmenu(fb, m, h); break;
      case SCR_INSULIN: render_insulin(fb, m, h); break;
      case SCR_DEVICES: render_devices(fb, m, h); break;
      case SCR_PERMS: render_perms(fb, m, h); break;
      case SCR_REMOTE: render_remote(fb, m, h); break;
      case SCR_INSLOG: render_inslog(fb, m, h); break;
      case SCR_INSDEL: render_insdel(fb, m, h); break;
      case SCR_WEIGHT: render_weight(fb, m, h); break;
      case SCR_WTLOG: render_wtlog(fb, m, h); break;
      case SCR_WTDEL: render_wtdel(fb, m, h); break;
      case SCR_ALARM: render_alarm(fb, m, h); break;
      case SCR_EXPORT: render_export(fb, m, h); break;
      case SCR_DISPLAY: render_display(fb, m, h); break;
      case SCR_OLDDEV: render_olddev(fb, m, h); break;
      case SCR_LABEL: render_label(fb, m, h); break;
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
      return (struct action){ACT_NONE, 0};
   return (struct action){h->box[i].kind, h->box[i].arg};
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
