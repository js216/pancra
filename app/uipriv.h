// SPDX-License-Identifier: GPL-3.0
// uipriv.h --- What the UI's own files share with each other
// Copyright 2026 Jakob Kastelic
//
/* NOT AN INTERFACE. ui.h is the interface: a screen model in, pixels and hit
 * boxes out. This header is the seam INSIDE that box, where the renderers
 * were split off the primitives once one file had grown past five thousand
 * lines:
 *
 *   uidraw.c  the primitives -- glyphs, frames, rows, buttons, hit boxes --
 *             and the formatters every screen shares;
 *   uimain.c  the main screen (the big number, the plot, the info column);
 *   uidev.c   devices and per-device screens;
 *   uimenu.c  settings and the modal screens reached from them;
 *   uilog.c   the dose and weight logs and their forms;
 *   the renderer      the entry points, and the switch that dispatches one
 * screen.
 *
 * The split changes nothing about the design that matters: every renderer is
 * still a pure function of an immutable `struct screen`, still takes its
 * framebuffer and hit list as parameters, and still runs on the host against
 * a malloc'd buffer (test/uitest.c renders each screen to a PPM). Nothing
 * below is state.
 */
#ifndef UIPRIV_H
#define UIPRIV_H

#include "ndk.h"
#include "ui.h"
#include <stdint.h>

/* Layout constants owned by the UI (not the shell). */
#define UI_COLS   33         /* character columns the layout targets */
#define UI_TABS   6          /* plot-span tabs */
#define UI_HILITE 0xFFAAAAAA /* scrub highlight dot (gray) */
/* Readings whose sensor is no longer in a slot: dim, so they stay legible as
 * history without competing with a live trace. Deliberately NOT 0xFF666666 --
 * that is plot.c's x-tick colour, and an orphan marker drawn in it reads as
 * part of the axis. */
#define UI_ORPHAN 0xFF8A8AA0

/* Sensor trace colours the picker offers; crosschecked against SET_NCOLORS
 * where the palette is defined. */
#define UI_NCOLORS 7

#define UI_LBL(units) ((units) ? "MMOL/L" : "MG/DL")

/* 720h = 30D on the right. 6H went: it sat between 3H and 12H without showing
 * anything either of them didn't. */
extern const int ui_tab_hours[UI_TABS];

/* Glyph cells discarded by clipping, bumped by the leaf primitives. See
 * ui_clip_reset in the renderer for why this is an instrument rather than
 * logic. */
void ui_clip_bump(long n);

/* Setting labels, indexed by the stored value. ui_disc_lbl must match
 * disc_min[] in alarm.c -- these are the labels for those values. */
extern const char *const ui_orient_lbl[];
extern const char *const ui_disc_lbl[];
extern const char *const ui_newdata_lbl[]; /* ND_OFF / ND_BEEP / ND_CHIRP */
extern const char *const ui_perm_lbl[];
void render_addmenu(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_alarm(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h);
void render_cal(struct ANativeWindow_Buffer *fb, const struct screen *m,
                struct hits *h);
void render_calpend(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_devices(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_devlist(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_display(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_export(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_forget(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_gate(struct ANativeWindow_Buffer *fb, struct hits *h);
void render_insdel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_inslog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_insulin(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_keypad(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_label(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h);
void render_main(struct ANativeWindow_Buffer *fb, const struct screen *m,
                 struct hits *h);
void render_markpick(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h);
void render_meterhelp(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h);
/* The LOG FOOD entry form: type, grams, and the instant. */
void render_food(struct ANativeWindow_Buffer *fb, const struct screen *m,
                 struct hits *h);

/* Confirm deleting one food entry. */
/* THE EXERCISE LOG and its correction form -- uiex.c, which explains why the
 * log has an edit screen and no logging screen. */
void render_exlog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h);
void render_exedit(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_exdel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h);

void render_fooddel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);

/* The FOOD LOG: entries newest first, paginated. */
void render_foodlog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);

/* The FOOD TYPE picker: the vocabulary, plus the row that adds to it. */
void render_foodtype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h);

void render_olddev(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_pairconf(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h);
/* Confirm giving up on an armed pairing: KEEP WAITING / STOP. */
void render_pendcancel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                       struct hits *h);
void render_perms(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h);
void render_reconf(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_remote(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_rescale(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h);
void render_rescaleact(struct ANativeWindow_Buffer *fb, const struct screen *m,
                       struct hits *h);
void render_sensor(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h);
void render_senstype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h);
void render_settings(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h);
void render_syncrestore(struct ANativeWindow_Buffer *fb, const struct screen *m,
                        struct hits *h);
void render_weight(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                   const struct ui_prefs *prefs, long tz_off, struct hits *h);
void render_wtdel(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                  const struct ui_prefs *prefs, long tz_off, struct hits *h);
void render_wtlog(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                  const struct ui_prefs *prefs, long now, long tz_off,
                  struct hits *h);


#endif
