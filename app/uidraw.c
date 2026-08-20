// SPDX-License-Identifier: GPL-3.0
// uidraw.c --- The drawing primitives every screen shares (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "uidraw.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: the ceiling a threshold row shows */
#include "exercise.h" /* EX_MAX_LEVEL: the button draws its own level */
#include "font.h"
#include "ndk.h"
#include "sensors.h" /* sensor types, kinds, marker enum */
#include "style.h"
#include "uiact.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>
#include <stdio.h> /* snprintf */

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
            ui_clip_bump(1);
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
      ui_clip_bump(1);
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
void fill_rect(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
               int y, int w, int h, uint32_t c)
{
   if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > buf->width ||
       y + h > buf->height) {
      ui_clip_bump(1);
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
void draw_checkbox(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                   int y, int side, int th, int on, uint32_t c)
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

void clear_fb(struct ANativeWindow_Buffer *fb, uint32_t c)
{
   uint32_t *px = fb->bits;
   for (int32_t y = 0; y < fb->height; y++)
      for (int32_t x = 0; x < fb->width; x++)
         px[(y * fb->stride) + x] = c;
}

int add_hit(struct hits *h, int x, int y, int w, int hgt, int kind, int arg)
{
   if (h->n >= UI_MAX_HITS) {
      /* LOUD, not silent. A dropped box draws normally and is dead to
       * touch, which is indistinguishable from a control that simply does
       * not work -- exactly the failure the keypad-title fallthrough had.
       * uitest gates on this staying clear at every screen and geometry. */
      h->overflow = 1;
      return UI_HIT_DROPPED;
   }
   int slot          = h->n;
   h->box[slot].x    = x;
   h->box[slot].y    = y;
   h->box[slot].w    = w;
   h->box[slot].h    = hgt;
   h->box[slot].kind = kind;
   h->box[slot].code = 0; /* not a menu action; see add_hit_ix */
   h->box[slot].arg  = arg;
   /* glow (pressed-highlight) rect defaults to the hit rect itself */
   h->box[slot].gx = x;
   h->box[slot].gy = y;
   h->box[slot].gw = w;
   h->box[slot].gh = hgt;
   h->n++;
   return slot;
}

/* A MENU ACTION, with its index in its own field.
 *
 * Every ACT_MENU target goes through here rather than through add_hit, and
 * that is what retired the base+index integer namespace: "sensor 3" is
 * (MA_SENSOR, 3), not MA_SENSOR + 3, so no code can run into the next one
 * however many sensors, digits, colours or characters there turn out to be.
 * `ix` is 0 for the codes that name one control and nothing else. */
int add_hit_ix(struct hits *h, int x, int y, int w, int hgt, int code, int ix)
{
   int slot = add_hit(h, x, y, w, hgt, ACT_MENU, ix);
   /* THE SLOT THIS CALL FILLED, never box[n-1]. On a full table add_hit
    * leaves n where it was, so n-1 is the PREVIOUS control -- a real one,
    * drawn and tappable -- and stamping this code onto it silently made
    * BACK forget the device. The drop is already reported by `overflow`;
    * this keeps it from taking a working control down with it.
    *
    * `>= 0` and not a truth test: slot 0 is the first legitimate box, so
    * `if (slot)` would skip it and only UI_HIT_DROPPED is out of range. */
   if (slot >= 0)
      h->box[slot].code = code;
   return slot;
}

/* Narrow hit box `slot`'s glow rect to (x,y,w,h) -- for a control whose hit
 * zone is deliberately larger than its visible glyph, so arming it lights the
 * glyph alone, not every stranger caught in the zone.
 *
 * `slot` is what the add_hit that recorded the box returned, rather than the
 * last box on the list: those differ exactly when the append was dropped, and
 * then the last box belongs to somebody else, who would get a glow rect
 * pointing at pixels that are not theirs. UI_HIT_DROPPED (and any other
 * non-slot) is a no-op. */
void add_glow(struct hits *h, int slot, int x, int y, int w, int hgt)
{
   if (slot < 0 || slot >= h->n)
      return;
   h->box[slot].gx = x;
   h->box[slot].gy = y;
   h->box[slot].gw = w;
   h->box[slot].gh = hgt;
}

/* THE EXERCISE BUTTON, WHEREVER IT IS DRAWN.
 *
 * It appears in two places now -- the ADD menu's LOG section and, once pinned,
 * the main screen's shortcut row -- and it cannot be an ordinary menu_button
 * in either, because three things about it are not a label: the LEVEL showing
 * on it, the COLOUR that encodes that level, and the bar counting down the
 * settling period. Two copies of that would be two controls able to disagree
 * about one value, which is exactly the objection that kept it off the pin
 * list in the first place; one function drawing both is what makes pinning it
 * safe.
 *
 * THE COLOUR CARRIES THE VALUE, in deepening blue, so the level reads without
 * the number being parsed -- and the number is drawn too, because three shades
 * of one hue is not something to ask anybody to rank from memory. At rest it
 * is the same grey as any other unset control, which is what it is: 0 is not a
 * level, it is the absence of one.
 *
 * `remaining` is the seconds left of the settling period, 0 when nothing is
 * pending. The bar shrinks as it runs down and vanishing IS the receipt that
 * the record was written -- so it is drawn only while something is pending,
 * and never at rest.
 *
 * Returns the y below the button, like menu_button. */
int ui_exercise_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                       int y, int w, int sc, int level, int remaining,
                       int settle_s, const char *name, uint32_t rest_col)
{
   uint32_t *px = fb->bits;
   /* AT REST IT IS AN ORDINARY BUTTON, and `rest_col` is whatever its
    * neighbours are drawn in -- white in the ADD menu, the dimmer shade on
    * the main screen's shortcut row. It had its own grey, which made an
    * inactive control look subtly disabled next to identical buttons that
    * were not. Level 0 is not a state worth colouring: it is the absence of
    * one.
    *
    * Only 1..3 get a colour, deepening towards saturated blue, so the blue
    * means "exercise is being recorded" and nothing else does. */
   static const uint32_t exc[EX_MAX_LEVEL + 1] = {
       0, /* unused: level 0 takes rest_col */
       0xFFFF9955, /* 1 */
       0xFFFF6622, /* 2 */
       0xFFFF3300  /* 3: ABGR, so these deepen towards saturated blue */
   };
   int lv       = (level < 0 || level > EX_MAX_LEVEL) ? 0 : level;
   uint32_t col = lv > 0 ? exc[lv] : rest_col;
   char lbl[28];
   if (lv > 0)
      (void)snprintf(lbl, sizeof lbl, "%s  %d", name, lv);
   else
      (void)snprintf(lbl, sizeof lbl, "%s", name);
   int below = menu_button(fb, h, x, y, w, sc, lbl, col, MA_EXERCISE, 0);
   /* INSIDE the button's own rectangle, not below it: the row pitch is fixed
    * by the caller's layout, and a bar drawn under the button would either eat
    * the gap or land on the next control. */
   if (remaining > 0 && settle_s > 0) {
      int bh   = 2 * sc;
      int bwid = (w * remaining) / settle_s;
      if (bwid > w)
         bwid = w;
      if (bwid < 1)
         bwid = 1; /* a bar with a second left is still a bar */
      fill_rect(px, fb, x, below - bh - sc, bwid, bh, col);
   }
   return below;
}

/* big-number colour by fixed medical range (0xAABBGGRR) */
uint32_t glu_color(int g)
{
   if (g < 50)
      return 0xFF0000FF; /* red    */
   if (g < 70)
      return 0xFF0080FF; /* orange */
   if (g < 180)
      return 0xFF33FF88; /* green  */
   return 0xFFFFFFFF;    /* white  */
}

/* THE SAME NUMBER, COLOURED BY THE USER'S OWN ALARM BAND AS WELL.
 *
 * glu_color above is the FIXED medical scale and it stays: 50 and 70 are
 * hypoglycaemia whatever anybody has configured, and a reading of 45 must not
 * read as normal because somebody set their low alarm to 40.
 *
 * What it cannot express is the band the user actually chose. Somebody with a
 * low alarm at 85 gets no warning colour at 80 -- the alarm sounds and the
 * banner appears while the number stays green, which is the app disagreeing
 * with itself about the same reading. So the configured band is applied ON TOP
 * of the fixed scale, and only ever in the direction of MORE alarm:
 *
 *   - at or below the low alarm, or at or above the high alarm, the number
 *     takes the alarm colour;
 *   - inside the nudge band it takes the nudge colour;
 *   - otherwise the fixed scale decides, unchanged.
 *
 * INCLUSIVE at both limits, exactly like alarm_zone in alarmlogic.c and like
 * banner_of on the main screen. The three describe one threshold and must not
 * disagree about the instant it is reached -- a reading of exactly 85 that
 * sounds the alarm, draws the banner, and leaves the number green was the
 * defect that made the banner's own comment necessary.
 *
 * NEVER LESS ALARMING THAN THE FIXED SCALE. A high alarm set to 400 must not
 * turn a reading of 60 from orange to white on the way past, so a fixed-scale
 * colour that is already a warning wins. That is what `worse` decides. */
uint32_t glu_color_band(int g, int alarm_low, int alarm_high, int nudge_low,
                        int nudge_high)
{
   uint32_t fixed = glu_color(g);
   if (g < 0)
      return fixed;
   /* How alarming a colour is, so the two scales can be compared rather than
    * one blindly overwriting the other. */
   uint32_t pick = 0;
   if ((alarm_low > 0 && g <= alarm_low) || (alarm_high > 0 && g >= alarm_high))
      pick = 0xFF2020E0; /* the alarm's own red, as the banner uses */
   else if ((nudge_low > 0 && g <= nudge_low) ||
            (nudge_high > 0 && g >= nudge_high))
      pick = 0xFF20A0FF; /* the nudge band: the banner's amber */
   if (!pick)
      return fixed;
   /* The fixed scale already says hypo: keep it. Its red and orange are the
    * more urgent statement, and they are about a number, not a preference. */
   if (fixed == 0xFF0000FF || fixed == 0xFF0080FF)
      return fixed;
   return pick;
}

uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFF; /* plot dots */
}

/* The 5x7 icon BITMAPS live in font.c with the glyph tables (all the pixel
 * art in one file); this blitter stays here with draw_cell and its clipping
 * counter. */

/* Draw one 5x7 icon at (ox,oy), scale sc -- the icon equivalent of one
 * draw_str glyph cell. */
void draw_icon(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
               int oy, int sc, const uint8_t g[7], uint32_t c)
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

/* (ui_color_names and ui_marker_names went with ui_color_name and
 * ui_marker_name: the two accessors were exported and called by nothing, and
 * the tables existed only to feed them. The pickers draw the shapes and the
 * colours themselves, which is what a picker should do.) */

uint32_t ui_sensor_color(int color)
{
   if (color < 0 || color >= UI_NCOLORS)
      color = 0;
   return ui_sensor_colors[color];
}

/* Full brand name for the ADD DEVICE picker and the per-device TYPE row. The
 * stored type name stays short (STELO/G7/ONETOUCH) so the 16-char device label
 * does not truncate. */
const char *sensor_disp_name(int type)
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

const char *ui_bucket_label(int b)
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
void menu_row_at(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
                 int lh, int rx, const char *name, const char *value,
                 uint32_t valcol, int code, int ix)
{
   uint32_t *px = fb->bits;
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   int vw = str_len(value) * 6 * sc;
   draw_str(px, fb, rx - vw, y, sc, value, valcol);
   if (code >= 0)
      add_hit_ix(h, 0, y - (3 * sc), fb->width, lh, code, ix);
}

/* The ordinary row: value right-aligned on the screen's own right margin. */
void menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
              int lh, const char *name, const char *value, uint32_t valcol,
              int code, int ix)
{
   menu_row_at(fb, h, y, sc, lh, fb->width - (4 * sc), name, value, valcol,
               code, ix);
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
int draw_title_fit(uint32_t *px, const struct ANativeWindow_Buffer *fb, int x,
                   int y, int tsc, const char *s, uint32_t col, int maxw)
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
int menu_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x, int y,
                int w, int sc, const char *label, uint32_t col, int action,
                int ix)
{
   uint32_t *px = fb->bits;
   int bh       = 25 * sc; /* label glyph is 7*sc -> 9*sc padding each side
                            * (4*sc -> 6*sc -> 9*sc; +50% padding app-wide) */
   int lw  = str_len(label) * 6 * sc;
   int lhh = 7 * sc;
   draw_frame(px, fb, x, y, w, bh, 0xFF888888);
   draw_str(px, fb, x + ((w - lw) / 2), y + ((bh - lhh) / 2), sc, label, col);
   add_hit_ix(h, x, y, w, bh, action, ix);
   return y + bh;
}

void menu_head(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
               int lh, const char *name)
{
   (void)h;
   (void)lh;
   uint32_t *px = fb->bits;
   /* DIMMER THAN A ROW NAME (0xCCCCCC), and that is now the whole of it.
    *
    * There used to be a rule under the caption, edge to edge, and the reason
    * was real: drawn at the row colour with no value beside it, a caption
    * reads as a row whose value failed to render -- and on THIS screen "a
    * threshold with no value" is the most alarming thing the UI could say by
    * accident. The rule made the caption structure rather than content.
    *
    * It was also the only horizontal rule in the entire app. Every other menu
    * separates its sections with a blank line and nothing else, so the one
    * screen that drew a line read as a different kind of screen -- and the
    * fix for an inconsistency between one screen and all the others is to
    * change the one. The dim colour carries the job alone; what actually
    * distinguishes a caption from a broken row is that a row always has a
    * value column and a caption never does, which no rule was needed to say.
    *
    * The blank line above each caption (the callers' 5*lh/2 step) is doing
    * the separating now, exactly as it does everywhere else. */
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFF888888);
}

int thresh_off(int mgdl, int ishigh)
{
   return ishigh ? (mgdl >= AL_ENTRY_MAX) : (mgdl <= 0);
}

void fmt_thresh(int mgdl, int units, int ishigh, char *out, int n)
{
   if (thresh_off(mgdl, ishigh))
      (void)snprintf(out, n, "OFF");
   else
      fmt_glu(mgdl, units, out, n);
}

/* lx0 / rx1 are the row's EXACT left and right ink edges, not a column to be
 * distributed inside. They are the progress bar's leftmost pixel and the right
 * edge of the units label beside the big number, so these two rows share their
 * margins with the top of the screen instead of using the content column's own
 * wider one -- three different left margins in one vertical line was the thing
 * that made the screen look untidy even when every individual row was fine. */
int thresh_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h, int lx0, int rx1, int y, int sc, int pad,
               int isalarm)
{
   uint32_t *px      = fb->bits;
   const uint32_t gy = 0xFF888888;
   const uint32_t wt = 0xFFFFFFFF;
   int cwid          = 6 * sc;
   char lo[8];
   char hi[8];
   fmt_thresh(isalarm ? m->prefs.alarm_low : m->prefs.nudge_low, m->prefs.units,
              0, lo, sizeof lo);
   fmt_thresh(isalarm ? m->prefs.alarm_high : m->prefs.nudge_high,
              m->prefs.units, 1, hi, sizeof hi);
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
   fmt_thresh(isalarm ? m->prefs.nudge_low : m->prefs.alarm_low, m->prefs.units,
              0, olo, sizeof olo);
   fmt_thresh(isalarm ? m->prefs.nudge_high : m->prefs.alarm_high,
              m->prefs.units, 1, ohi, sizeof ohi);
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
   if (isalarm ? m->prefs.sound_on : m->prefs.nudge_sound)
      draw_icon(px, fb, ax, y, sc, icon_speaker, gy);
   if (isalarm ? m->prefs.vib_on : m->prefs.nudge_vib)
      draw_icon(px, fb, ax + (6 * sc), y, sc, icon_vibrate, gy);
   if (isalarm && m->prefs.disc)
      draw_icon(px, fb, ax + (12 * sc), y, sc, icon_nolink, gy);
   if (!isalarm && m->prefs.newdata_mode)
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
         add_hit_ix(h, 0, al_y, ax - (g / 2), al_h, MA_ALARM_OPEN, 0);
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
            add_hit_ix(h, hx, al_y, hw, al_h, code, 0);
      }
      ax += colw[i] + g;
   }
   return y + (7 * sc) + pad;
}

/* One "LOW"/"HIGH" row: the value in display units, or OFF when the threshold
 * can never be reached (fmt_thresh). */
void thresh_menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, int lh, const char *name, int mgdl, int units,
                     int ishigh, int code)
{
   char v[8];
   char lv[16];
   fmt_thresh(mgdl, units, ishigh, v, sizeof v);
   if (thresh_off(mgdl, ishigh)) /* "OFF" carries no unit */
      (void)snprintf(lv, sizeof lv, "%s", v);
   else
      (void)snprintf(lv, sizeof lv, "%s %s", v, UI_LBL(units));
   menu_row(fb, h, y, sc, lh, name, lv, 0xFFFFFFFF, code, 0);
}

/* One checkbox row: name left, the checkbox ICON right (font.c: icon_box /
 * icon_boxck -- the same glyph language as every other symbol), green when
 * checked. The whole row toggles (generous target, like every menu_row). */
void chk_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
             int lh, const char *name, int on, int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, 0xFFCCCCCC);
   draw_icon(px, fb, rx - (5 * sc), y, sc, on ? icon_boxck : icon_box,
             on ? 0xFF33FF88 : 0xFF888888);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, lh, code, 0);
}

int value_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
              const char *name, const char *val, uint32_t vcol, int code,
              int ix)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   int vsc      = 2 * sc;
   int vw       = str_len(val) * 6 * vsc;
   draw_str(px, fb, 4 * sc, y + (((7 * vsc) - (7 * sc)) / 2), sc, name,
            0xFFCCCCCC);
   draw_str(px, fb, rx - vw, y, vsc, val, vcol);
   add_hit_ix(h, fb->width / 2, y - (4 * sc), fb->width / 2,
              (7 * vsc) + (8 * sc), code, ix);
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

/* ---- settings menu (portrait table; rows carry menu_action codes) ---- */

const char *const ui_orient_lbl[] = {"PORTRAIT", "LANDSCAPE", "GRAVITY",
                                     "SYSTEM"};
/* Must match disc_min[] in alarm.c -- these are the labels for those values. */
const char *const ui_disc_lbl[] = {"OFF", "15 MIN", "30 MIN", "60 MIN"};
/* Indexed by ND_OFF / ND_BEEP / ND_CHIRP. */
const char *const ui_newdata_lbl[] = {"OFF", "BEEP", "CHIRP"};

const char *const ui_perm_lbl[] = {"BT SCAN", "BT CONNECT", "NOTIFY"};

/* App-Standby bucket -> short label. */

/* A rescale factor (per mille) as the signed percentage the user typed. */
void fmt_rescale_pct(int pm, char *out, int n)
{
   int d = pm - 1000; /* tenths of a percent */
   int a = (d < 0) ? -d : d;
   (void)snprintf(out, n, "%c%d.%d%%", (d < 0) ? '-' : '+', a / 10, a % 10);
}
