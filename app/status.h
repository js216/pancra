// SPDX-License-Identifier: GPL-3.0
// status.h --- Say one line to the user, and ask for a repaint
// Copyright 2026 Jakob Kastelic
//
/* THE SMALLEST THING A WORKFLOW NEEDS FROM THE SCREEN.
 *
 * Workflows all over the app say one line about what just happened ("SIZE NOT
 * SAVED", "METER BUSY, RETRY") and ask for a repaint. Both are implemented in
 * model.c, so each of those files would otherwise include model.h -- the
 * header that declares how a FRAME IS ASSEMBLED, which none of them do, and
 * which is what makes model.c and menu.c include each other (see menuview.h).
 *
 * The declarations below are separated because they are a different contract:
 * a workflow that has just refused something needs to say so, and needs
 * nothing else about frames. They are still implemented in model.c, beside the
 * buffer they write.
 */
#ifndef STATUS_H
#define STATUS_H

/* Put one line on the status row, and repaint. Short, upper-case, and about
 * what just happened -- this is the only place a refusal becomes visible. */
void set_status(const char *s);

/* THE SAME LINE, MARKED AS A REFUSAL -- and the difference is where it is
 * seen. The plain status row is drawn on ONE screen: the pre-reading main
 * screen, which a user with a registered device never sees again. Every
 * "NOT SAVED" this app says therefore reached nobody. A refusal is put on
 * the screen the user is actually looking at, over the bottom line, until
 * the next frame that has nothing to complain about.
 *
 * For refusals only. Progress chatter ("PAIRING", "SYNCING") must stay on
 * set_status, or the banner is on screen permanently and says nothing. */
void set_status_refused(const char *s);

/* Rebuild the status text and repaint if anything visible changed -- at most
 * a few times a second, so radio chatter cannot saturate the main thread. Off
 * the main thread it only marks the frame dirty. */
void update_screen(void);

/* Forget the cached text lines: a new surface has nothing on it. */
void model_lines_reset(void);

/* The status line itself, for the crash handler, which may only hold a
 * pointer -- it runs on a signal stack and cannot allocate or lock. */
/* ATOMIC BYTES: the handler that reads this can interrupt the thread writing
 * it (see crashlog.h and set_status in model.c). */
const _Atomic char *model_status_buf(void);

#endif
