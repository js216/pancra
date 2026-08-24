// SPDX-License-Identifier: GPL-3.0
// uidraw.c --- The drawing primitives every screen shares (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "uidraw.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: the ceiling a threshold row shows */
#include "civil.h"      /* civil_ymd: ONE days-to-civil conversion */
#include "exercise.h"   /* EX_MAX_LEVEL: the button draws its own level */
#include "font.h"
#include "ndk.h"
#include "sensors.h"
#include "stats.h" /* TIR_LOW_MGDL / TIR_HIGH_MGDL: one definition of the range */ /* sensor types, kinds, marker enum */
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

/* Fill one sc*sc glyph cell at (bx,by), clipped to the buffer.
 *
 * CLIPPED ONCE, NOT PER PIXEL. This tested every pixel's x and y against the
 * buffer inside the inner loop -- four comparisons and a branch to decide a
 * single 32-bit store -- which is the whole of the app's text drawing and,
 * measured, the whole of its frame time: the offline harness spent 18 seconds
 * of its 18.4 rendering, and the phone pays the same shape once a second
 * forever on a core far slower than this one.
 *
 * The visible sub-rectangle is computed here, the count of dropped pixels is
 * arithmetic rather than a branch per pixel, and what remains is a store loop
 * a compiler can vectorise. Same pixels, same clip count, ~10x the speed. */
static void draw_cell(uint32_t *px, const struct ANativeWindow_Buffer *buf,
                      int bx, int by, int sc, uint32_t color)
{
   if (sc <= 0)
      return;
   int x0 = bx < 0 ? 0 : bx;
   int y0 = by < 0 ? 0 : by;
   int x1 = bx + sc;
   int y1 = by + sc;
   if (x1 > buf->width)
      x1 = buf->width;
   if (y1 > buf->height)
      y1 = buf->height;
   /* WHAT FELL OUTSIDE, counted exactly as the per-pixel test counted it: the
    * cell's area less the part that landed. ui_clip_bump is what the tests
    * read to say a glyph was cut off, so this number is load-bearing. */
   int vw   = x1 - x0;
   int vh   = y1 - y0;
   long vis = (vw > 0 && vh > 0) ? (long)vw * vh : 0;
   long all = (long)sc * sc;
   if (vis < all)
      ui_clip_bump((int)(all - vis));
   if (vis == 0 || !px)
      return;
   for (int y = y0; y < y1; y++) {
      uint32_t *row = px + ((long)y * buf->stride);
      for (int x = x0; x < x1; x++)
         row[x] = color;
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
   if (!px)
      return; /* laid out, not drawn: see the note on `bits` in uidraw.h */
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
   if (!px)
      return; /* laid out, not drawn: see the note on `bits` in uidraw.h */
   /* One row pointer per row, then a straight store loop: the address
    * arithmetic is not redone per pixel (see draw_cell for what that
    * costs). */
   for (int j = 0; j < h; j++) {
      uint32_t *row = px + ((long)(y + j) * buf->stride) + x;
      for (int i = 0; i < w; i++)
         row[i] = c;
   }
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
   if (!px)
      return; /* laid out, not drawn: see the note on `bits` in uidraw.h */
   for (int32_t y = 0; y < fb->height; y++) {
      uint32_t *row = px + ((long)y * fb->stride);
      for (int32_t x = 0; x < fb->width; x++)
         row[x] = c;
   }
}

int add_hit(struct hits *h, struct ui_rect r, int kind, int arg)
{
   const int x   = r.x;
   const int y   = r.y;
   const int w   = r.w;
   const int hgt = r.h;
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
int add_hit_ix(struct hits *h, struct ui_rect r, int code, int ix)
{
   int slot = add_hit(h, r, ACT_MENU, ix);
   /* THE SLOT THIS CALL FILLED, never box[n-1]. On a full table add_hit
    * leaves n unchanged, so n-1 is the PREVIOUS control -- a real one, drawn
    * and tappable -- and stamping this code onto it silently makes BACK
    * forget the device. The drop is already reported by `overflow`;
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
void add_glow(struct hits *h, int slot, struct ui_rect r)
{
   if (slot < 0 || slot >= h->n)
      return;
   h->box[slot].gx = r.x;
   h->box[slot].gy = r.y;
   h->box[slot].gw = r.w;
   h->box[slot].gh = r.h;
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
/* ONE TABLE FOR THE THREE PLACES A LEVEL IS SHOWN.
 *
 * The button on the ADD menu, the row in the log, and the LEVEL field of the
 * edit form all colour the same three values, and a level that is "moderate
 * orange" on one screen and something else on another is three controls
 * disagreeing about one number. It lived inside the button, which is where it
 * was needed first and not where it belongs.
 *
 * Only 1..3 get a colour, deepening towards saturated blue, so the blue means
 * "exercise is being recorded" and nothing else does. Level 0 is not a state
 * worth colouring -- it is the ABSENCE of one -- so it takes `rest_col`,
 * whatever the caller's ordinary text is: white in the ADD menu, the dimmer
 * shade on the main screen's shortcut row. It once had a grey of its own,
 * which made an inactive control look subtly disabled beside identical
 * buttons that were not. */
/* The label colour that stays readable on `bg`, which is ABGR8888.
 *
 * Rec. 709 luminance, in integer arithmetic: the eye weights green far above
 * red and red above blue, so a plain average of the three calls a saturated
 * blue as bright as a mid grey and puts black text on it. The split is at
 * half of full scale -- black above it, white below. */
uint32_t ui_text_on(uint32_t bg)
{
   unsigned r = bg & 0xFFU;
   unsigned g = (bg >> 8U) & 0xFFU;
   unsigned b = (bg >> 16U) & 0xFFU;
   unsigned y = ((2126U * r) + (7152U * g) + (722U * b)) / 10000U;

   return (y >= 128U) ? UI_BLACK : UI_TEXT;
}

uint32_t ui_ex_color(int level, uint32_t rest_col)
{
   static const uint32_t exc[EX_MAX_LEVEL + 1] = {
       0,          /* unused: level 0 takes rest_col */
       UI_EX_LIGHT, /* 1 */
       UI_EX_MOD,   /* 2 */
       UI_EX_HARD  /* 3: ABGR, so these deepen towards saturated blue */
   };
   if (level < EX_MIN_LEVEL || level > EX_MAX_LEVEL)
      return rest_col;
   return exc[level];
}

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
   int lv = (level < 0 || level > EX_MAX_LEVEL) ? 0 : level;
   char lbl[28];
   if (lv > 0)
      (void)snprintf(lbl, sizeof lbl, "%s  %d", name, lv);
   else
      (void)snprintf(lbl, sizeof lbl, "%s", name);

   /* ---- THE LEVEL IS THE BACKGROUND, NOT THE LETTERS -------------------
    *
    * Tinting the LABEL said the same thing in the least visible way there
    * is: a few dozen coloured pixels inside an otherwise identical button,
    * on a screen of otherwise identical buttons. A filled button is legible
    * from across a room and at a glance, which is what a control you press
    * mid-walk needs to be.
    *
    * THE TEXT COLOUR IS THEN NOT A CHOICE, it is a consequence: the three
    * shades run from a light blue to a saturated one, and a single label
    * colour cannot be readable on both ends. So it is computed from the
    * background's luminance -- see ui_text_on -- which also means the next
    * person to adjust a shade does not have to remember to adjust the text.
    *
    * AT REST NOTHING IS FILLED. Level 0 is the absence of a state, not a
    * state, and a filled button at rest would make an idle control the
    * loudest thing on the menu. */
   uint32_t bg  = ui_ex_color(lv, 0);
   uint32_t col = rest_col;
   if (lv > 0) {
      fill_rect(px, fb, x, y, w, 25 * sc, bg);
      col = ui_text_on(bg);
   }
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
      /* IN THE LABEL'S COLOUR, which is now the CONTRASTING one. The bar used
       * to be drawn in the level's colour -- which is the background now, so
       * it would be a bar of exactly the shade it sits on: invisible, on the
       * one control whose whole point is a countdown you can watch. */
      fill_rect(px, fb, x, below - bh - sc, bwid, bh, col);
   }
   return below;
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
   /* BY DIRECTION AS WELL AS BY BAND, and in the same order of precedence the
    * threshold row lights its groups in -- the number and the row are saying
    * one thing about one reading, so they take their colour from one list. */
   uint32_t pick = 0;
   if (alarm_low > 0 && g <= alarm_low)
      pick = UI_BAND_ALARM_LO;
   else if (alarm_high > 0 && g >= alarm_high)
      pick = UI_BAND_ALARM_HI;
   else if (nudge_low > 0 && g <= nudge_low)
      pick = UI_BAND_NUDGE_LO;
   else if (nudge_high > 0 && g >= nudge_high)
      pick = UI_BAND_NUDGE_HI;
   if (!pick)
      return fixed;
   /* The fixed scale already says hypo: keep it. Its red and orange are the
    * more urgent statement, and they are about a number, not a preference. */
   if (fixed == UI_ALERT || fixed == UI_GLU_SOFT)
      return fixed;
   return pick;
}

uint32_t white_color(int g)
{
   (void)g;
   return UI_TEXT; /* plot dots */
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
   /* THE SHARED CONVERSION, NOT A SECOND COPY OF IT.
    *
    * Howard Hinnant's days-to-civil algorithm written out here would be the
    * same twelve lines civil.c already holds, with its own era shift, its own
    * negative-era correction and its own leap rule. Two copies of an
    * algorithm nobody re-derives when they touch it is a drift waiting for
    * one of them to be fixed: the dates a person reads off the screen and
    * the dates the log is keyed by would then disagree, and only for the
    * inputs that are awkward to reach on purpose (an epoch before 1970, a
    * century year, the 400-year boundary). civil.c's copy is the one the
    * tests execute.
    *
    * What stays here is the SECONDS-OF-DAY half, because that is this
    * function's own business: `secs` has already been floored into [0,86400)
    * above, together with the day count that made it. */
   long year = 0;
   long mon  = 0;
   long dday = 0;
   civil_ymd(z, &year, &mon, &dday);
   (void)snprintf(out, n, "%04ld-%02ld-%02ld %02ld:%02ld", year, mon, dday,
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
 * glu_color uses (its "red" is 0xFF0000FF). Written as standard 0xAARRGGBB
 * they swap red and blue: BLUE renders orange, AMBER renders blue, and so on.
 * Encoded here with R and B swapped, they match their names. */
/* One constant, so the two tables and the two guards below cannot drift:
 * a colour index comes from a settings file the user can hand-edit, and a
 * table that outgrew its guard would read past its end. */
/* settings_load bounds the stored insulin colour against SET_NCOLORS,
 * which it cannot see this table to check. Keep them equal here. */
_Static_assert(UI_NCOLORS == SET_NCOLORS,
               "UI_NCOLORS and SET_NCOLORS disagree");
static const uint32_t ui_sensor_colors[UI_NCOLORS] = {
    UI_SENS_GREEN /* GREEN */,
    UI_SENS_BLUE /* BLUE */,
    UI_BUSY /* AMBER */,
    UI_SENS_PINK /* PINK */,
    UI_SENS_CYAN /* CYAN */,
    UI_SENS_VIOLET /* VIOLET */,
    UI_TEXT /* WHITE -- the default primary-trace colour */};

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
    * Omit the +8 from `start` and the two functions sit 8*sc apart -- so on
    * heights where the slack falls in that gap, capacity comes out below
    * UI_MIN_SLOTS and the renderer takes its early return, hiding the device
    * list and the ADD button. That band includes 1080x2280 (Galaxy S10 /
    * Redmi Note 7 / Moto G7) and 1440x3200
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
 * it rather than drawing the two on top of each other. */
void menu_row_at(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
                 int lh, int rx, const char *name, const char *value,
                 uint32_t valcol, int code, int ix)
{
   uint32_t *px = fb->bits;
   draw_str(px, fb, 4 * sc, y, sc, name, UI_TEXT_DIM);
   int vw = str_len(value) * 6 * sc;
   draw_str(px, fb, rx - vw, y, sc, value, valcol);
   if (code >= 0)
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), code, ix);
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
   draw_frame(px, fb, x, y, w, bh, UI_MUTED);
   draw_str(px, fb, x + ((w - lw) / 2), y + ((bh - lhh) / 2), sc, label, col);
   add_hit_ix(h, ui_rect(x, y, w, bh), action, ix);
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
    * NO RULE UNDER THE CAPTION, tempting though one is: drawn at the row
    * colour with no value beside it, a caption reads as a row whose value
    * failed to render -- and on THIS screen "a threshold with no value" is
    * the most alarming thing the UI could say by accident.
    *
    * But a rule here would be the only horizontal rule in the app. Every
    * other menu separates its sections with a blank line and nothing else, so
    * the one screen that drew a line would read as a different kind of
    * screen. The dim colour carries the job alone; what actually
    * distinguishes a caption from a broken row is that a row always has a
    * value column and a caption never does, which no rule is needed to say.
    *
    * The blank line above each caption (the callers' 5*lh/2 step) does the
    * separating, exactly as it does everywhere else. */
   draw_str(px, fb, 4 * sc, y, sc, name, UI_MUTED);
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
 * edge of the units label beside the big number, so this row shares its
 * margins with the top of the screen rather than using the content column's
 * own wider one -- three different left margins in one vertical line was the
 * thing that made the screen look untidy even when every individual row was
 * fine. */

/* ONE ROW FOR ALL FOUR THRESHOLDS, and the arrows are what makes it one.
 *
 * It was two: an ALARM row and a NUDGE row, each spelling "LOW" and "HIGH".
 * Four numbers that describe ONE axis were split across two lines, each line
 * spending half its width on words, and reading "is 110 the nudge or the
 * alarm" meant looking at which row it sat on. Ordered low to high with the
 * count of arrows saying how far out the band is -- two down, one down, one
 * up, two up -- the four read as the scale they actually are, and the row
 * they used to need is what the second and third rows of shortcut buttons are
 * drawn in.
 *
 * THE SYMBOLS ARE THE ALARM'S ONLY. Sound, vibration and the disconnect
 * alarm are alarm outputs; the dot is NEW DATAPOINT. The nudge has its own
 * sound and vibration settings and shows neither, because with one row there
 * is nothing to tell them apart by -- two speakers side by side would be a
 * puzzle, not a legend. The alarm is the one whose outputs are worth knowing
 * at a glance. */
/* Black or white, whichever stands out on `bg`. Rec. 709 luminance, because
 * the two backgrounds this picks between are a dark red and a light amber and
 * eyeballing it gets one of them wrong: white on the amber is the pairing
 * that looks fine at desk brightness and vanishes in sunlight. */
static uint32_t on_color(uint32_t bg)
{
   /* 0xAABBGGRR, so the channels come out in this order. */
   const unsigned r = bg & 0xFFU;
   const unsigned g = (bg >> 8) & 0xFFU;
   const unsigned b = (bg >> 16) & 0xFFU;
   const unsigned l = ((2126U * r) + (7152U * g) + (722U * b)) / 10000U;
   return l > 140U ? UI_BLACK : UI_TEXT;
}

/* WHICH THRESHOLD THE READING IS CURRENTLY BREACHING, as an index into the
 * four groups, or -1.
 *
 * ONE AT A TIME, AND THE ALARM WINS. A reading below the low alarm is below
 * the low nudge as well -- the bands nest -- so lighting both would say the
 * nudge is what is happening when the alarm is. Same precedence, and the same
 * two colours, as glu_color_band uses on the big number: the row and the
 * number must not disagree about which band the reading is in.
 *
 * ONLY WHILE THE READING IS CURRENT. A threshold lit from a value hours old
 * states a condition that is not known to hold; the whole point of the
 * highlight is that it is present NOW. */
static int thresh_hot(const struct screen *m)
{
   if (!m->reading.has_cgm || m->reading.glu < 0)
      return -1;
   if (m->now - m->reading.t > AL_FRESH_S)
      return -1;
   const int g = m->reading.glu;
   /* INCLUSIVE, exactly like alarm_zone and the banner: the alarm fires AT
    * the limit, so the row must light at the limit too. */
   if (m->prefs.alarm_low > 0 && g <= m->prefs.alarm_low)
      return 0;
   if (m->prefs.alarm_high > 0 && g >= m->prefs.alarm_high)
      return 3;
   if (m->prefs.nudge_low > 0 && g <= m->prefs.nudge_low)
      return 1;
   if (m->prefs.nudge_high > 0 && g >= m->prefs.nudge_high)
      return 2;
   return -1;
}

int thresh_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h, int lx0, int rx1, int y, int sc, int pad)
{
   uint32_t *px      = fb->bits;
   const uint32_t gy = UI_MUTED;
   const uint32_t wt = UI_TEXT;
   const int cwid    = 6 * sc;

   /* LOW TO HIGH, left to right -- the order they sit in on the axis. */
   char v[4][8];
   fmt_thresh(m->prefs.alarm_low, m->prefs.units, 0, v[0], sizeof v[0]);
   fmt_thresh(m->prefs.nudge_low, m->prefs.units, 0, v[1], sizeof v[1]);
   fmt_thresh(m->prefs.nudge_high, m->prefs.units, 1, v[2], sizeof v[2]);
   fmt_thresh(m->prefs.alarm_high, m->prefs.units, 1, v[3], sizeof v[3]);
   static const int narrow[4] = {2, 1, 1, 2};
   static const int updn[4]   = {0, 0, 1, 1}; /* 0 = down, 1 = up */
   static const int code[4]   = {MA_ALARM_LOW, MA_NUDGE_LOW, MA_NUDGE_HIGH,
                                 MA_ALARM_HIGH};
   /* The same four the big number takes its colour from -- see uidraw.h. */
   static const uint32_t band[4] = {UI_BAND_ALARM_LO, UI_BAND_NUDGE_LO,
                                    UI_BAND_NUDGE_HI, UI_BAND_ALARM_HI};
   const int hot                 = thresh_hot(m);

   /* THE WORD IS THE ANNUNCIATOR. It was four icon cells -- speaker, phone,
    * slashed circle, dot -- and they answered a question nobody was asking:
    * which OUTPUTS are armed, spelled in symbols that have to be learned,
    * next to four numbers that need none. What is worth knowing at a glance
    * is whether this row will make a noise at all, and that is one word.
    *
    * ALARM when the alarm itself will sound. NUDGE when it will not but the
    * nudge still will -- the row can still speak, just not for the band it is
    * named after. SILENT when neither will, which is the state worth being
    * able to see from across a room. Vibration, the disconnect alarm and the
    * new-datapoint beep have their own rows in the ALARM submenu, which this
    * word opens. */
   const char *name = m->prefs.sound_on      ? "ALARM"
                      : m->prefs.nudge_sound ? "NUDGE"
                                             : "SILENT";

   /* THE NUMBER FIELD IS SIZED BY THE UNIT, NOT BY THE VALUES IN IT.
    *
    * Three digits in mg/dL ("400", and "OFF" is three too); four in mmol/L,
    * where the same ceiling reads "22.2". Taking the width from the current
    * values instead would move every column on the row the day a threshold
    * gained or lost a digit -- and the numbers are right-aligned in the
    * field, so with the width fixed the ones, tens and hundreds each keep one
    * column for good, whatever is typed. */
   const int vw    = (m->prefs.units ? 4 : 3) * cwid;
   /* THE WORD'S OWN WIDTH, not the widest of the three. Padding ALARM out to
    * SILENT's six characters would leave a blank column after it and make the
    * first gap read wider than the other three, which is the one thing this
    * row's spacing is for. The groups shift by a character when the word
    * changes; the word only changes when a sound setting is toggled, which is
    * a deliberate act and not something that happens while reading. */
   const int namew = str_len(name) * cwid;

   /* EACH GROUP IS AS WIDE AS ITS OWN CONTENTS, so the SPACE BETWEEN groups
    * is what is equal rather than the groups themselves.
    *
    * Reserving two arrow slots on every group made the four equal and the
    * gaps unequal: a single-arrow marker left its outer slot blank, so the
    * eye saw twice the space before a nudge as before an alarm and the row
    * read as two pairs rather than one scale. The arrows still never move
    * when a value changes -- nothing here depends on the digits -- they are
    * simply not padded out to a width they do not use. */
   int cellw[4];
   for (int i = 0; i < 4; i++)
      cellw[i] = (narrow[i] * cwid) + cwid + vw; /* arrows, a space, number */

   /* FIVE elements -- the word and four groups -- so four gaps, all equal,
    * and no leading or trailing gap because the row's outer edges are given
    * rather than derived. draw_str emits no trailing gap, so the last group's
    * ink stops one unit short of it; add that sc back before dividing or the
    * row lands a pixel inside rx1 rather than on it. */
   int total = namew;
   for (int i = 0; i < 4; i++)
      total += cellw[i];
   int g             = ((rx1 - lx0) + sc - total) / 4;
   /* THE FLOOR IS A COLLISION GUARD, NOT A LOOK. The row's outer edges are
    * GIVEN -- it aligns with the plot above it -- so the gap is whatever is
    * left over, and a floor of a whole character was clamping it UP on a
    * 720-wide screen and pushing the last number six pixels past the right
    * edge it was supposed to land on. 2*sc is close enough to touching to be
    * worth refusing and far enough below the natural spacing never to bite
    * except where nothing would have fitted anyway. */
   if (g < 2 * sc)
      g = 2 * sc;

   int ax = lx0;

   /* EXACTLY the row advance, so this row and whatever follows it ABUT
    * rather than overlap: ui_hit_idx scans backwards, so an over-tall target
    * here would swallow the top of the control below it. */
   const int al_y = y - (3 * sc);
   const int al_h = (7 * sc) + pad;

   /* Everything LEFT of the first value -- the icons and the name, from the
    * screen's leftmost pixel -- opens the ALARM submenu, which holds both
    * sections. The four value cells are each their own target opening that
    * threshold's keypad. All five are DISJOINT: the pressed highlight lights
    * the armed control's whole rectangle, so one must not contain another's
    * pixels. */
   draw_str(px, fb, ax, y, sc, name, gy);
   add_hit_ix(h, ui_rect(0, al_y, ax + namew + (g / 2), al_h), MA_ALARM_OPEN,
              0);
   ax += namew + g;

   for (int i = 0; i < 4; i++) {
      /* Arrows at the group's left edge, the number RIGHT-aligned at its
       * right: the ones digit lands on the group's last column whatever the
       * value, so a two-digit threshold leaves the hundreds column blank
       * rather than sliding the whole number over. */
      /* THE BREACHED THRESHOLD IS REVERSED: its band colour behind, and
       * whatever reads on that in front. A colour swap alone would put a red
       * number among four grey ones and leave the eye to notice it; filling
       * the group makes the row itself say which band the reading is in, the
       * same statement the big number above is already making. */
      uint32_t acol = gy;
      uint32_t ncol = wt;
      if (i == hot) {
         /* One character of margin each side and 2*sc above and below the
          * glyph, which is what the row's own advance (7*sc + pad) has to
          * spare -- pad is 18*sc in portrait and 6*sc in landscape. */
         fill_rect(px, fb, ax - (cwid / 2), y - (2 * sc), cellw[i] + cwid,
                   (7 * sc) + (4 * sc), band[i]);
         acol = on_color(band[i]);
         ncol = acol;
      }
      const uint8_t *ic = updn[i] ? icon_arrow_up : icon_arrow_dn;
      for (int k = 0; k < narrow[i]; k++)
         draw_icon(px, fb, ax + (k * cwid), y, sc, ic, acol);
      const int tw = str_len(v[i]) * cwid;
      draw_str(px, fb, ax + cellw[i] - tw, y, sc, v[i], ncol);
      /* Half a gap either side, clamped to the row's own right edge: rx1 is
       * an exact edge, not a column with slack after it, so an overhanging
       * target on the last cell would run off the buffer and be dropped. */
      int hx          = ax - (g / 2);
      int hw          = cellw[i] + g;
      const int hmaxw = rx1 - hx;
      if (hw > hmaxw)
         hw = hmaxw;
      if (hw > 0)
         add_hit_ix(h, ui_rect(hx, al_y, hw, al_h), code[i], 0);
      ax += cellw[i] + g;
   }
   return y + (7 * sc) + pad;
}

/* ALL FOUR BUTTONS, ALWAYS, AND GREYED WHEN THEY HAVE NOWHERE TO GO.
 *
 * They used to appear and disappear: at page one there was no "<" at all, so
 * the row's controls moved under the finger as you paged, and "am I at the
 * start" had to be inferred from a missing glyph. A control that is present
 * but dim says the same thing without moving anything, and the four cells
 * give the counter a fixed place to sit between them.
 *
 * FIRST AND LAST EARN THEIR PLACE on the logs, where the page count runs to
 * twenty-five: stepping back to the newest entries one page at a time is not
 * a thing anybody should have to do, and the newest entries are the ones
 * these tables are read for.
 *
 * THE DESTINATION IS IN THE HIT, not worked out later. The renderer is what
 * knows how many pages there are -- it derives the count from the window it
 * just laid out -- so it is the only place that can say where "last" is.
 * Handlers that incremented a page and left the renderer to clamp it let the
 * stored number run past the end, so a NEXT pressed ten times at the bottom
 * needed ten PRESSES of PREV to come back. */
void pager_row(struct ANativeWindow_Buffer *fb, struct hits *h, int x, int rx,
               int y, int sc, int lh, int page, int npages, int code)
{
   uint32_t *px  = fb->bits;
   const int tsc = 2 * sc;
   /* Two character cells per button, so the single-glyph ones line up with
    * the doubles and every target is the same size. */
   const int bw  = 12 * tsc;
   const int hy  = y - (3 * sc);
   const int hh  = lh + (7 * sc);
   struct {
      const uint8_t *ic;
      int cx; /* cell's left edge */
      int to; /* the page it goes to */
   } b[4] = {
       {icon_pg_first, x, 0},
       {icon_pg_prev, x + bw, page - 1},
       {icon_pg_next, rx - (2 * bw), page + 1},
       {icon_pg_last, rx - bw, npages - 1},
   };
   for (int i = 0; i < 4; i++) {
      /* A destination outside the range, or the page already showing, is a
       * button with nothing to do -- which is exactly when it greys. */
      const int live = b[i].to >= 0 && b[i].to < npages && b[i].to != page;
      /* UI_TEXT_DIM, NOT UI_TEXT, and that is what makes the press visible.
       * ui_press_overlay brightens a control by scaling its brightest channel
       * up to full -- so anything already drawn at 0xFF cannot get brighter
       * and a press on it does nothing at all. Every other tappable label on
       * these screens is dim for the same reason; these were white, which
       * looked emphatic and was inert. */
      /* AND THE DEAD ONE IS UI_RULE, not UI_MUTED. Against a dim-white live
       * arrow, 0x88 still reads as an arrow you could press -- the pair
       * differed by one step where the difference has to carry the whole
       * message. UI_RULE is the tone this app already spends on structure
       * rather than content, which is what a button with nowhere to go has
       * become. */
      draw_icon(px, fb, b[i].cx + ((bw - (5 * tsc)) / 2), y, tsc, b[i].ic,
                live ? UI_TEXT_DIM : UI_RULE);
      if (live)
         add_hit_ix(h, ui_rect(b[i].cx, hy, bw, hh), code, b[i].to);
   }
   char pg[24];
   (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
   draw_str(px, fb, (x + rx - (str_len(pg) * 6 * sc)) / 2, y, sc, pg, UI_MUTED);
}

/* One "LOW"/"HIGH" row in the ALARM submenu: the value in display units, or
 * OFF when the threshold can never be reached (fmt_thresh). */
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
   menu_row(fb, h, y, sc, lh, name, lv, UI_TEXT, code, 0);
}

/* One checkbox row: name left, the checkbox ICON right (font.c: icon_box /
 * icon_boxck -- the same glyph language as every other symbol), green when
 * checked. The whole row toggles (generous target, like every menu_row). */
void chk_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
             int lh, const char *name, int on, int code)
{
   uint32_t *px = fb->bits;
   int rx       = fb->width - (4 * sc);
   draw_str(px, fb, 4 * sc, y, sc, name, UI_TEXT_DIM);
   draw_icon(px, fb, rx - (5 * sc), y, sc, on ? icon_boxck : icon_box,
             on ? UI_OK : UI_MUTED);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), code, 0);
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
            UI_TEXT_DIM);
   draw_str(px, fb, rx - vw, y, vsc, val, vcol);
   add_hit_ix(h,
              ui_rect(fb->width / 2, y - (4 * sc), fb->width / 2,
                      (7 * vsc) + (8 * sc)),
              code, ix);
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

/* big-number colour by fixed medical range (0xAABBGGRR) */
uint32_t glu_color(int g)
{
   if (g < 50)
      return UI_GLU_LOW;  /* under 50 */
   if (g < 70)
      return UI_GLU_SOFT; /* 50..70   */
   if (g < 180)
      return UI_GLU_MID;  /* 70..180  */
   return UI_GLU_HIGH; /* over 180 */
}

int thresh_off(int mgdl, int ishigh)
{
   return ishigh ? (mgdl >= AL_ENTRY_MAX) : (mgdl <= 0);
}
