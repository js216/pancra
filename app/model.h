// SPDX-License-Identifier: GPL-3.0
// model.h --- Live state becomes ONE immutable frame
// Copyright 2026 Jakob Kastelic
//
/* THE ONLY PLACE THE RENDERER'S INPUT IS ASSEMBLED.
 *
 * the renderer draws a `struct screen` and nothing else -- no globals, no
 * callbacks. This file is what fills one: it reads the registry, the driver
 * sessions, the reading history, the settings and the forms, and copies out a
 * frame that cannot change while it is being drawn.
 *
 * THE SNAPSHOT IS THE POINT, and it is a locking argument, not tidiness. The
 * registry and the driver sessions are mutated from BINDER threads (a sensor
 * minting shifts the whole record array), and the documented lock order is
 * driver -> registry -> history. A renderer that read them live would either
 * render a half-shifted row -- one sensor's session age on another's line --
 * or have to take the registry lock INSIDE the history lock, inverting that
 * order. So each is snapshotted first, under its own lock, and the frame is
 * built from the copies.
 *
 * THE SESSION CACHE lives here for the same reason: it is what lets a frame
 * show a sensor's clock and state immediately after a restart, before the
 * sensor has answered. It is written from the draw path, so it is rate
 * limited against the last SAVE (see senslogic.h).
 */
#ifndef MODEL_H
#define MODEL_H

#include "status.h" /* set_status / update_screen: still implemented here */
#include "ui.h"     /* struct screen */

/* Snapshot the registry and the driver sessions. Call BEFORE taking the
 * history lock -- see the header comment. */
void model_snapshot(void);

/* The id of the device drawn at row `row` of the last frame, or -1. A tap
 * names a row; this is what turns it into a device, against the picture the
 * user actually touched rather than the registry as it now stands. */
int model_snap_id(int row);

/* The plot span the user picked, in hours: frame state, so it lives with the
 * frame. */
int model_plot_hours(void);

void model_set_plot_hours(int hours);

/* Fill `m` with everything one frame needs. Main thread. */
void build_model(struct screen *m);

/* (The status row and the repaint request are in status.h, included above:
 * they are what a WORKFLOW needs from the screen, and a workflow has no
 * business with how a frame is assembled.) */

/* (The last-known session cache is sesscache.h's: a cache the workflow tick
 * flushes and this file merely reads is not part of assembling a frame, and
 * having it here is what made reconcile.c include model.h.) */

/* (Per-link signal strength is reading.h's: it is kept where the measurement
 * arrives, and this file reads it.) */

/* (The remote sync's last reply and last acknowledgement are remote.h's: the
 * module that performs the sync owns what happened, and the frame reads it.
 * They were here, so remote.c had to include the frame builder.) */

#endif
