// SPDX-License-Identifier: GPL-3.0
// status.h --- Say one line to the user, and ask for a repaint
// Copyright 2026 Jakob Kastelic
//
/* THE SMALLEST THING A WORKFLOW NEEDS FROM THE SCREEN.
 *
 * Eight files call set_status() to explain a refusal ("SIZE NOT SAVED",
 * "METER BUSY, RETRY") and update_screen() to ask for a repaint. Both are
 * implemented in model.c, so all eight included model.h -- the header that
 * declares how a FRAME IS ASSEMBLED, which none of them do. menu.c doing that
 * is what made model.c and menu.c include each other (see menuview.h).
 *
 * These four are separated because they are a different contract: a workflow
 * that has just refused something needs to say so, and needs nothing else
 * about frames. They are still implemented in model.c, beside the buffer they
 * write.
 */
#ifndef STATUS_H
#define STATUS_H

/* Put one line on the status row, and repaint. Short, upper-case, and about
 * what just happened -- this is the only place a refusal becomes visible. */
void set_status(const char *s);

/* Rebuild the status text and repaint if anything visible changed -- at most
 * a few times a second, so radio chatter cannot saturate the main thread. Off
 * the main thread it only marks the frame dirty. */
void update_screen(void);

/* Forget the cached text lines: a new surface has nothing on it. */
void model_lines_reset(void);

/* The status line itself, for the crash handler, which may only hold a
 * pointer -- it runs on a signal stack and cannot allocate or lock. */
const char *model_status_buf(void);

#endif
