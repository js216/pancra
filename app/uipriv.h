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

/* ---- THE DAILY-TOTAL PLOT, shared by the exercise and insulin logs ----
 *
 * Both answer the same shape of question -- how much of this did I do on each
 * of the last N days -- so both draw the same chart: one bar per local day,
 * scrubbable, under the same span tabs. Only the quantity differs, so the
 * caller buckets its own records and names the unit; nothing about exercise
 * or insulin is known here. */

/* Where the span tab `tab` starts. `oldest` is the log's first entry, which
 * is where the ALL tab reaches back to. */
long day_from_of(int tab, long now, long oldest);

/* The entries, drawn into [px0,px0+pw) x [py0,py0+ph). `hilite` is the index
 * the finger is on, or -1. `unit` labels the y bounds ("MIN", "U"). `col` has
 * `ncol` entries, one per series. */
/* `dp` is how many decimal places `v` is scaled by -- 0 for a plain count,
 * 3 when the points carry thousandths -- so the axis reads in the unit the
 * label names rather than in whatever integer the series happens to hold. */
void log_plot(uint32_t *px, const struct ANativeWindow_Buffer *fb,
              const struct log_pt *p, int n, long from, long now, int px0,
              int py0, int pw, int ph, int sc, long tz_off, int hilite,
              const char *unit, int dp, const uint32_t *col, int ncol);

/* Which entry a finger at (x,y) is nearest, or -1, through the same geometry
 * log_plot draws with, so a pick can never name an entry other than the one
 * under the finger.
 *
 * `lock` binds the search to one series, or -1 to search them all -- and it
 * also picks the metric. Unbound (a press) measures BOTH axes, because the
 * press is choosing which curve. Bound (a drag) measures x only, because the
 * finger is then sweeping along one trace to read it. */
int log_pick(const struct log_pt *p, int n, long from, long now, int px0,
             int py0, int pw, int ph, int sc, int x, int y, int lock);

/* THE SERIES EACH LOG PUTS ON THE PLOT, and the only definition of them: the
 * renderer draws what these return and the picker picks from it. `from`
 * receives the window's start. */
#define UI_INS_SERIES 2
extern const uint32_t ui_ins_col[UI_INS_SERIES];
int ins_points(const struct screen *m, struct log_pt *out, int cap,
               long *from);
int ex_points(const struct screen *m, struct log_pt *out, int cap, long *from);

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
/* `scrub` is the point the finger is on, or -1 -- the frame's one log-plot
 * scrub (see uimodel.h), passed in rather than reached for, so this stays a
 * function of the weight model, the display unit, the clock and the zone. */
void render_wtlog(struct ANativeWindow_Buffer *fb, const struct ui_wtview *wt,
                  const struct ui_prefs *prefs, long now, long tz_off,
                  int scrub, struct hits *h);


#endif
