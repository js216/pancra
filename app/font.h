// SPDX-License-Identifier: GPL-3.0
// font.h --- 5x7 bitmap font (API)
// Copyright 2026 Jakob Kastelic

/* font.h -- the app's 5x7 bitmap font, as pure data + lookup.
 *
 * No Android/JNI types here: a glyph is 7 rows of a 5-wide bitmap (each row's
 * low 5 bits, MSB = leftmost pixel). The caller (draw_str in uidraw.c) owns the
 * framebuffer and blitting; this module only maps a character to its bitmap. */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* 7-byte 5x7 bitmap for character c, or NULL for a blank cell (space/unknown).
 */
const uint8_t *glyph_for(char c);

/* strlen for the ASCII strings we render (no libc dependency). */
int str_len(const char *s);

/* 5x7 status icons, same bitmap format as the glyphs, for states a letter
 * cannot say. Pure data like the font tables; the renderer's draw_icon blits
 * them. */
extern const uint8_t icon_speaker[7]; /* alarm output: sound enabled */
extern const uint8_t icon_vibrate[7]; /* alarm output: vibration enabled */
extern const uint8_t icon_pencil[7];  /* "edit this row" mark */
extern const uint8_t icon_dot[7];     /* NEW DATAPOINT beep indicator */
extern const uint8_t icon_nolink[7];  /* DISCONNECT (stale-data) alarm */
extern const uint8_t icon_box[7];     /* checkbox, empty */
extern const uint8_t icon_boxck[7];   /* checkbox, checked */
extern const uint8_t icon_boxfill[7]; /* checkbox, solid (the PRIMARY pick) */

#endif
