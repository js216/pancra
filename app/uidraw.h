// SPDX-License-Identifier: GPL-3.0
// uidraw.h --- turning a value into the characters that show it
// Copyright 2026 Jakob Kastelic
/*
 * DECLARED WHERE THEY ARE IMPLEMENTED (uidraw.c). These seven formatters
 * lived in uifmt.h, a header of presentation constants that had also
 * collected the helpers of three other renderers -- so a file that wanted one
 * number depended on all of them, and nothing in the module graph could tell
 * the pile from an interface.
 *
 * Pure functions of their arguments: no state, no screen, no allocation.
 * Every one writes into a caller's buffer of `n` bytes and always terminates.
 */
#ifndef PANCRA_UIDRAW_H
#define PANCRA_UIDRAW_H

#include "ndk.h"     /* struct ANativeWindow_Buffer: what these draw into */
#include "uiact.h"   /* struct hits: the touch targets add_hit fills */
#include "uimodel.h" /* struct screen: what the row helpers read */
#include <stdint.h>

/* ---- the two glyph routines ---- */
/* Draw a string at (ox,oy) scaled by `sc`, in ARGB colour. */
void draw_str(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
              int oy, int sc, const char *s, uint32_t color);
/* Draw a 1px rectangle outline (x,y,w,h) in ARGB colour; no-op if
 * off-buffer. */
void draw_frame(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                int y, int w, int h, uint32_t c);

/* ---- the formatters ---- */

/* Format glucose for display; units: 0 = mg/dL, 1 = mmol/L. Pure. */
void fmt_glu(int mgdl, int units, char *out, int n);
/* Format a trend (tenths of mg/dL per minute; 127 = unknown -> "--"). Pure. */
void fmt_trend(int tr, char *out, int n);
/* Format epoch seconds as HH:MM:SS local time (tz = offset seconds). Pure. */
void fmt_hms(long epoch, long tz, char *out, int n);
/* Format epoch seconds as a local date (tz = offset seconds). Pure. */
void fmt_date(long epoch, long tz, char *out, int n);
/* "N S" / "N M" / "N H" / "N D": how long ago `then` was, from `now`. The
 * unit changes as the gap grows, so a row never widens. Pure. */
void fmt_ago(long now, long then, char *out, int n);
/* A duration as "N D N H" / "N H N M" / "N M". Pure. */
void fmt_dur(long seconds, char *out, int n);

/* ---- the framebuffer and hit-list primitives ------------------------
 *
 * Also uidraw.c's, and also declared in uipriv.h until the module graph
 * pointed out that a header the whole ui family shares was speaking for eight
 * modules at once. What is left there is the family's dispatch table -- the
 * render_* screens -- which is one contract; these are one module's. */
/* WHAT add_hit ANSWERS: the slot it filled, or that it filled none.
 *
 * Not a zero-valued "OK" on purpose. Slot 0 is the first real box of every
 * frame, so an outcome enum in the shape of enum csv_field -- where
 * CSV_FIELD_OK == 0 -- would collide with a legitimate index and invert every
 * `if (slot)` written against it. The failure gets the out-of-range value and
 * the success stays an index, which is the only thing a decorator can use. */
/* A FRAME WITH NOWHERE TO PUT THE PIXELS.
 *
 * Every primitive here answers a NULL `bits` (or a NULL pixel pointer) by
 * doing everything EXCEPT the stores: the geometry is computed, the clip
 * counter is bumped exactly as it would have been, and hit boxes are recorded
 * by the callers as usual. What is skipped is the writing.
 *
 * Two callers want that. The offline harness sweeps every screen at fourteen
 * real device geometries to prove that nothing lands off-screen and no touch
 * target is dropped -- assertions about WHERE things are, which do not need a
 * single pixel, and which cost sixteen seconds of memory traffic when they
 * are drawn anyway. And on the phone, ANativeWindow_lock can hand back a
 * buffer it failed to map, which a primitive that does not check writes
 * through.
 *
 * THE CLIP COUNT is what the tests read to say a glyph
 * was cut off, so it is computed before the pixels are skipped, not after. */
enum { UI_HIT_DROPPED = -1 };

/* ---- A RECTANGLE IS ONE VALUE ----------------------------------------
 *
 * These four numbers travelled as four positional arguments through every
 * touch target in the app -- `add_hit(h, x, y, w, hgt, kind, arg)`, seven
 * arguments, four of which are one thing. Two of them are coordinates and two
 * are extents, they are all `int`, and every call site writes them in the
 * same order out of habit rather than because anything checks: a transposed
 * pair compiles, draws nothing wrong (the drawing is separate code), and
 * leaves a control whose tappable area is somewhere the control is not. The
 * only symptom is a button that does not respond, or one that responds when
 * the user meant its neighbour.
 *
 * As one value it can be BUILT ONCE and used twice, which is the other half
 * of this: the glow rect a pressed control lights is the hit rect in all but
 * a handful of cases, and those cases now narrow a copy of a named rectangle
 * rather than repeating four expressions.
 *
 * A CONSTRUCTOR RATHER THAN A COMPOUND LITERAL AT EVERY CALL SITE, because a
 * literal is four positional numbers again -- this at least has a name, one
 * place to read the order from, and one place a bound could ever go. */
struct ui_rect {
   int x, y, w, h;
};

static inline struct ui_rect ui_rect(int x, int y, int w, int h)
{
   struct ui_rect r;
   r.x = x;
   r.y = y;
   r.w = w;
   r.h = h;
   return r;
}

void add_glow(struct hits *h, int slot, struct ui_rect r);

int add_hit(struct hits *h, struct ui_rect r, int kind, int arg);

int add_hit_ix(struct hits *h, struct ui_rect r, int code, int ix);

void chk_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
             int lh, const char *name, int on, int code);

void clear_fb(struct ANativeWindow_Buffer *fb, uint32_t c);

void draw_checkbox(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                   int y, int side, int th, int on, uint32_t c);

/* ---- the rest of uidraw.c's surface ---------------------------------- */
void fmt_thresh(int mgdl, int units, int ishigh, char *out, int n);

/* A rescale factor (per mille) as the percentage the user set. */
void fmt_rescale_pct(int pm, char *out, int n);

int draw_title_fit(uint32_t *px, const struct ANativeWindow_Buffer *fb, int x,
                   int y, int tsc, const char *s, uint32_t col, int maxw);

/* The EXERCISE control, drawn identically in the ADD menu and on the main
 * screen: level, colour and the settling countdown. See uidraw.c. */
/* `rest_col` is what the button is drawn in at level 0 -- pass whatever the
 * neighbouring buttons use, so an inactive EXERCISE is indistinguishable from
 * any other control. Only levels 1..3 take the blue. */
/* The colour a level is drawn in, everywhere it is drawn. Levels outside
 * EX_MIN_LEVEL..EX_MAX_LEVEL -- including the resting 0 -- take `rest_col`,
 * the caller's ordinary text colour. See the definition. */
uint32_t ui_text_on(uint32_t bg);
uint32_t ui_ex_color(int level, uint32_t rest_col);


int ui_exercise_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                       int y, int w, int sc, int level, int remaining,
                       int settle_s, const char *name, uint32_t rest_col);

/* The same, with the user's configured alarm and nudge bands applied on top --
 * only ever in the direction of MORE alarm, and inclusive at every limit so
 * the number, the banner and the alarm agree. See uidraw.c. */
uint32_t glu_color_band(int g, int alarm_low, int alarm_high, int nudge_low,
                        int nudge_high);

void draw_icon(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
               int oy, int sc, const uint8_t g[7], uint32_t c);

void fill_rect(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
               int y, int w, int h, uint32_t c);

/* ---- the menu row vocabulary, also uidraw.c's ------------------------ */
const char *sensor_disp_name(int type);

/* THE SCRUB READOUT EVERY PLOT DRAWS: `when` left-aligned at x, `unit` right
 * -aligned at x+w, `val` centred between them. Each field is anchored to
 * something fixed, so reading along a trace moves the value alone. */
void log_scrub_row(uint32_t *px, const struct ANativeWindow_Buffer *fb, int x,
                   int y, int w, int lo, int hi, const char *when,
                   const char *val, const char *unit);

/* "STEPS", or "STEP " for exactly one -- the same width either way, because
 * the readouts that print it keep fixed columns. */
const char *ui_steps_word(long n);

int menu_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x, int y,
                int w, int sc, const char *label, uint32_t col, int action,
                int ix);

/* menu_button, with a BULLET drawn before the label in `mcol`: the button is
 * asking to be pressed. The mark and the label are centred as one unit, so
 * the two cannot collide however narrow the button is. */
int menu_button_mark(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                     int y, int w, int sc, const char *label, uint32_t col,
                     uint32_t mcol, int action, int ix);

/* 1 when a weighing is DUE: none on record, or the most recent one is a day
 * old or more. The WEIGHT buttons carry the bullet when it is. */
/* How many registered, live CGMs the OS has no bond with -- see the
 * definition. Nonzero is an error condition the user must act on. */
int ui_unpaired_count(const struct screen *m);

int ui_weight_due(const struct screen *m);

/* 1 when the once-daily SLOW dose is due: none on record, or the most recent
 * one is a day old or more. FAST is deliberately not asked about -- see the
 * definition. */
int ui_slow_ins_due(const struct screen *m);

void menu_head(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
               int lh, const char *name);

void menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
              int lh, const char *name, const char *value, uint32_t valcol,
              int code, int ix);

void menu_row_at(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
                 int lh, int rx, const char *name, const char *value,
                 uint32_t valcol, int code, int ix);

void thresh_menu_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y,
                     int sc, int lh, const char *name, int mgdl, int units,
                     int ishigh, int code);

/* THE PAGER: |< < n/m > >|, drawn across [x, rx] at `y`.
 *
 * `code` is the ONE action all four buttons carry; the destination page rides
 * in the hit's index, so the handler is `page = ix` and nothing about
 * stepping, ends or bounds lives in it. A button with nowhere to go is drawn
 * greyed and records no target.
 *
 * Returns nothing: the row's height is the caller's `lh`, which it had to
 * reserve before calling. */
void pager_row(struct ANativeWindow_Buffer *fb, struct hits *h, int x, int rx,
               int y, int sc, int lh, int page, int npages, int code);

/* THE LARGEST TEXT SCALE at which `s` fits in `maxw`, never below `min`.
 *
 * INK, NOT CELLS. A string of n glyphs occupies n*6-1 columns, not n*6:
 * draw_str emits no trailing gap after the last one. Measuring in whole cells
 * over-counts by a cell and shrinks text a size earlier than it needs to --
 * which is how a scrub readout came to use a smaller font for a three-digit
 * value than for a two-digit one, with room to spare in both. */
int fit_scale(const char *s, int maxw, int min, int max);

uint32_t white_color(int g);
/* The big number's colour for `g`, by the fixed medical range. */
uint32_t glu_color(int g);
/* The main screen's threshold row: all four values on ONE line, low to high,
 * marked with arrows. lx0/rx1 are its exact ink edges. */
int thresh_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h, int lx0, int rx1, int y, int sc, int pad);
/* Is this threshold at the end of its range, where it switches the alarm
 * off? `ishigh` picks which end. */
int thresh_off(int mgdl, int ishigh);
const char *ui_bucket_label(int b);
int value_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
              const char *name, const char *val, uint32_t vcol, int code,
              int ix);

#endif
