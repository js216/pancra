// SPDX-License-Identifier: GPL-3.0
// ui.h --- Draw a frame, and light the control under a finger
// Copyright 2026 Jakob Kastelic
//
/* THE RENDER CONTRACT, and the umbrella over the three halves of the UI.
 *
 * This file was 819 lines and held four unrelated contracts at once: the glyph
 * and formatting primitives, the frame model, the action vocabulary, and the
 * handful of calls below that actually draw. Anything that wanted one of them
 * inherited all four -- meter.c included ui.h for fmt_glu and got every view
 * struct and the whole screen enumeration with it.
 *
 * They are now:
 *   uifmt.h    pixels, words, geometry -- pure, stateless
 *   uimodel.h  struct screen: the immutable frame the renderer reads
 *   uiact.h    struct hits / struct action: what a tap means
 *   uipriv.h   the layout rules shared BETWEEN the renderers (private)
 *
 * ui.h includes the first three, so every existing `#include "ui.h"` keeps
 * working and a caller that wants less can say so.
 */
#ifndef PANCRA_UI_H
#define PANCRA_UI_H

#include "ndk.h" /* struct ANativeWindow_Buffer: what ui_render draws into */
#include "uiact.h"
#include "uifmt.h"
#include "uimodel.h"

/* Render model `m` into framebuffer `fb`, recording touch targets into `h`. */
int ui_wt_hit(const struct screen *m, int plot_x, int plot_w, int sc, int x);

void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h);
/* Map a tap at (x,y) against the targets from the last render. Pure. */
struct action ui_hit(const struct hits *h, int x, int y);
/* As ui_hit, but returns the index of the winning box (-1 = none), so the
 * shell can shade the pressed control's own rectangle. Pure. */
int ui_hit_idx(const struct hits *h, int x, int y);
/* Pressed-but-not-yet-fired highlight: every drawn (non-black) pixel in the
 * rectangle brightens to the same hue at full intensity; the background
 * stays black. Drawn by the shell over the armed control after ui_render --
 * actions fire on RELEASE, and this is the one consistent "armed" visual. */
void ui_press_overlay(struct ANativeWindow_Buffer *fb, int x, int y, int w,
                      int h);
/* Whole-frame dim to 13/16 intensity, applied by the shell AFTER ui_render
 * (so the offline harness still sees exact colours). This is what makes the
 * pressed highlight visible on already-saturated foregrounds -- white text
 * and the green big number have no headroom at full intensity, so the
 * resting frame gives some up. */
void ui_dim(struct ANativeWindow_Buffer *fb);

long ui_wt_from(const struct screen *m);

#endif
