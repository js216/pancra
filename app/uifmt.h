// SPDX-License-Identifier: GPL-3.0
// uifmt.h --- Pixels, words and geometry: the UI's primitives
// Copyright 2026 Jakob Kastelic
//
/* THE PART OF THE UI THAT HAS NO STATE AND NO SCREEN.
 *
 * Two glyph routines, seven formatters, the scale arithmetic every full-screen
 * menu sizes itself through, and the small exported tables whose second copy
 * would silently drift (the weight spans, the rename keypad's characters, the
 * pinnable actions). Everything here is a pure function of its arguments.
 *
 * Split out of ui.h, which had grown to 819 lines holding four unrelated
 * contracts -- these primitives, the frame model, the action vocabulary and
 * the render entry points -- and split again since: the FORMATTERS are
 * uidraw.h's, the keypad's capacities uikeypad.h's and the shortcut table
 * uimenu.h's, each declared where it is implemented. What is left here is
 * what the name says: constants and pure geometry.
 */
#ifndef PANCRA_UIFMT_H
#define PANCRA_UIFMT_H

#include "sensors.h" /* MAX_SLOTS: UI_MAX_SLOTS mirrors it */
#include <stdint.h>

/* OS bond states, mirroring android.bluetooth.BluetoothDevice's own constants
 * because Ble passes them through unchanged. UI_BOND_UNKNOWN (0) is not one of
 * the framework's: it means the bond receiver has never reported this address,
 * which is the normal state for a device registered before the app started. It
 * must render as "nothing to say", NOT as "not paired" -- an existing, working,
 * long-bonded sensor reads as 0 until its next transition. */
#define UI_BOND_UNKNOWN 0
#define UI_BOND_NONE    10
#define UI_BOND_BONDING 11
#define UI_BOND_BONDED  12

/* ---- sensor presentation (shared by the list, the detail screen, the plot) */
#define UI_MAX_SLOTS MAX_SLOTS
/* Refuse to render a sensor list shorter than this rather than silently
 * truncating it, so a cramped screen is a visible error, not a quiet lie. */
#define UI_MIN_SLOTS 3

/* Rows the DEVICES screen consumes ABOVE (and below) the device entries:
 * title (2), the five-line primary-box explainer plus its trailing air (~3),
 * the blank row under it (1), the armed-pairing "PENDING..." row (1), the
 * page-nav row (1), the blank line above OLD DEVICES (1), the "OLD DEVICES (n)"
 * row (1), the blank line below it (1), the separator before the button (1) and
 * the ADD NEW DEVICE button itself (25*sc, i.e. ~1.6 rows -> 2). Keep in step
 * with render_devices.
 *
 * The explainer is five glyph lines at gh + 2*sc each, which is under three
 * 16*sc rows -- counted as 3, rounding UP, because rounding down here is
 * indistinguishable from forgetting a row.
 *
 * The three middle rows are each OPTIONAL, and every one of them must still be
 * counted: a user with a full list, a retired device and a pairing in flight
 * draws all three at once. The count must be the WORST case the renderer can
 * produce, not the common one -- being pessimistic costs a little font size on
 * a crowded screen, while being optimistic costs the ADD button entirely, with
 * no scrolling to recover it. (It covers render_devices alone: the device
 * list has its own screen, so render_settings' four submenu rows and EXPORT
 * DATA are no part of it.)
 *
 * Exported deliberately. Private to the renderer, with test/uitest.c carrying
 * its own literal for the same quantity, the two drift apart silently -- and
 * adding rows to render_devices without bumping
 * this is exactly the mistake that leaves device rows and their tap targets
 * below the bottom of the screen, permanently unreachable because there is no
 * scrolling. One definition, both users. */
#define UI_DEV_ABOVE 14

/* AIR BETWEEN DEVICE ROWS. The list packed rows at the bare 16*sc line height,
 * which ran the devices together -- each row already carries a label, a plot
 * marker, a status and a countdown, so with no gap the eye cannot tell where
 * one device ends and the next begins.
 *
 * Half a line, not a full one: the gap is spent on HEIGHT and the screen does
 * not scroll, so every pixel here comes straight out of ui_sensor_capacity. A
 * full blank line reads no better and costs a third of the list.
 *
 * The PITCH is a macro rather than a literal in render_devices because
 * ui_sensor_capacity must divide by the SAME number the renderer advances by.
 * When those two disagree the list overflows the bottom of the screen and the
 * rows below it -- OLD DEVICES and the ADD NEW DEVICE button -- become
 * unreachable, with no scrolling to recover them. One definition, both users;
 * this is the same drift UI_DEV_ABOVE is exported to prevent. */
#define UI_DEV_GAP(sc)   (8 * (sc))
#define UI_DEV_PITCH(sc) ((16 * (sc)) + UI_DEV_GAP(sc))
/* UI_MIN_SLOTS device rows expressed in whole 16*sc lines, rounded UP, so
 * ui_devices_scale can keep reserving space in the units ui_fit_scale counts.
 * ceil(n * 3/2): at UI_MIN_SLOTS 3 the minimum list is 4.5 lines -> 5. Rounding
 * DOWN here would hand back a scale at which the minimum list does not fit,
 * which is precisely the early-return the scale exists to avoid. */
#define UI_DEV_MIN_ROWS ((((UI_MIN_SLOTS) * 3) + 1) / 2)

/* How many device rows fit in the DEVICES screen at this geometry, given
 * everything above and below the list. The whole UI never scrolls, so this is
 * what bounds the list. Pure, and exposed so the shell can log a warning. */
/* Layout scale for the DEVICES screen: bounded by BOTH width and height, so
 * the device list and ADD button stay on screen on 16:9 phones too. */
int ui_devices_scale(int w, int h);
/* Largest scale at which `rows` rows fit in height h (also bounded by width).
 * Every full-screen menu must size itself through this. */
int ui_fit_scale(int w, int h, int rows);

int ui_sensor_capacity(int w, int h);

/* Spans the WEIGHT LOG plot offers, in DAYS; 0 is "everything". Exported so
 * the shell can map a tab tap back to a span without duplicating the list. */
#define UI_WT_TABS 5
extern const int ui_wt_days[UI_WT_TABS];

/* The characters the rename keypad offers, in grid order. Exposed so the shell
 * can map an MA_CHAR code back to the character that was tapped. */
extern const char ui_label_chars[];

/* Digit slots a keypad mode accepts. The renderer draws exactly this many
 * cells and the input path accepts exactly this many digits, so the two
 * cannot drift into disagreeing about how long an entry may be. */
/* (Keypad modes moved to keypad.h, where each one is described ONCE -- title,
 * digit slots, unit suffix, decimal point, threshold -- rather than being a
 * bare integer with a table in the renderer and a range in the shell. The two
 * tables this bound have become one, and the ranges have become names.) */

uint32_t ui_sensor_color(int color);
#endif
