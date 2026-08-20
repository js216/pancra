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
enum { UI_HIT_DROPPED = -1 };

void add_glow(struct hits *h, int slot, int x, int y, int w, int hgt);

int add_hit(struct hits *h, int x, int y, int w, int hgt, int kind, int arg);

int add_hit_ix(struct hits *h, int x, int y, int w, int hgt, int code, int ix);

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
int ui_exercise_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                       int y, int w, int sc, int level, int remaining,
                       int settle_s, const char *name, uint32_t rest_col);

uint32_t glu_color(int g);
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

int menu_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x, int y,
                int w, int sc, const char *label, uint32_t col, int action,
                int ix);

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

uint32_t white_color(int g);
/* THE THRESHOLD ROW, in both of its forms: the tappable +/- row the main
 * screen and the alarm menu share, and the plain menu row that only displays
 * one. Together because a threshold reads the same everywhere -- "OFF" with
 * no unit when it is off, the value with its unit when it is not. */
int thresh_off(int mgdl, int ishigh);
int thresh_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h, int lx0, int rx1, int y, int sc, int pad,
               int isalarm);
const char *ui_bucket_label(int b);
int value_row(struct ANativeWindow_Buffer *fb, struct hits *h, int y, int sc,
              const char *name, const char *val, uint32_t vcol, int code,
              int ix);

#endif
