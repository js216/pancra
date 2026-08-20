// SPDX-License-Identifier: GPL-3.0
// input.h --- Touch: a finger on the glass becomes one action
// Copyright 2026 Jakob Kastelic
//
/* THE APP ACTS ON THE RELEASE, NOT THE PRESS.
 *
 * A press only ARMS the control under the finger; the action fires when the
 * finger lifts, and only if it lifts back on that same control -- so sliding
 * off first is a free cancel, and nothing else can fire until the next press.
 * That rule is not decoration. The controls here log insulin, forget a
 * sensor, and write a calibration; a mis-press on any of them is not
 * recoverable by tapping again.
 *
 * EXEMPT BY DESIGN, all three for a reason:
 *   - plot scrubbing: the drag IS the interaction;
 *   - the alarm +- steppers: step on press plus auto-repeat on hold is their
 *     whole point;
 *   - silencing a sounding alarm: any press must silence IMMEDIATELY, and
 *     that press then does nothing else.
 *
 * The hit list is rebuilt by every render and read here, both on the MAIN
 * thread. An armed control is re-found in the just-rebuilt list by the
 * finger's last on-target point, so a redraw that moved or removed it drops
 * the arming rather than firing a stranger.
 */
#ifndef INPUT_H
#define INPUT_H

#include "ui.h" /* struct hits */

struct ANativeWindow_Buffer;

/* The looper callback for the input queue: drains it so the ANR watchdog
 * stays fed. `data` is the AInputQueue. */
int on_input(int fd, int events, void *data);

/* The touch-target list the renderer fills and this file reads. */
struct hits *input_hits(void);

/* THE IDENTITY HALF OF A PRESS, and the rule a test can reach.
 *
 * input_arm_row latches the device under the finger at touch-DOWN (nothing,
 * for an action whose ix is a value). input_row_value answers at touch-UP:
 * 1 to fire, with *val the value to dispatch -- the DEVICE ID for a device
 * row -- and 0 when the row no longer holds the device that was pressed, in
 * which case nothing fires. See input.c for why. */
void input_arm_row(int action, int row);
int input_row_value(int action, int row, int *val);

/* Shade the armed control, if it is still under the finger and still resolves
 * to the same action in the just-rebuilt hit list. */
void input_press_overlay(struct ANativeWindow_Buffer *buf);

/* Cancel any arming: the screen is about to change under the finger. */
void press_cancel(void);

#endif
