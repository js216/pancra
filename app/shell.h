// SPDX-License-Identifier: GPL-3.0
// shell.h --- What the Android shell still owns, for the workflows that ask
// Copyright 2026 Jakob Kastelic
//
/* main.c is the SHELL: the activity callbacks, the looper, the input queue,
 * the window, and the BLE link table those are wired to. The workflows that
 * were split out of it (alarm.c, notify.c, meter runtime, the menus) do not
 * get to reach into that state -- they ask, through the handful of questions
 * below.
 *
 * Keep this header SMALL. Every entry is a place where a workflow still
 * depends on the shell, so a growing list means the split is going the wrong
 * way. It is not a dumping ground for globals that were awkward to move.
 */
#ifndef SHELL_H
#define SHELL_H

struct ANativeActivity;

/* When this PROCESS started, ON THE MONOTONIC CLOCK. Set once, never re-armed
 * -- the service outlives the activity, so a grace period keyed to it must not
 * restart when the user opens the app (doing so silenced the very alarm they
 * opened it to investigate, for a whole threshold).
 *
 * MONOTONIC, not wall clock, and the rename is the point: everything that
 * asks this question is measuring an INTERVAL ("how long have we been up",
 * "how long has this link been quiet"), never naming an instant. It was
 * realtime_s(), and a phone that finds a network shortly after boot -- which
 * is exactly when this app starts -- steps its clock underneath every one of
 * those intervals at once. Forward past the DISCONNECT threshold ends the
 * launch grace early and announces a disconnected sensor over data that was
 * merely waiting for its first sync; backward extends the grace, and the
 * grace SUPPRESSES the alarm, so the user's DISCONNECT alarm is switched off
 * for as long as the skew lasts. See clock.h, and `make clockcheck`. */
long shell_launch_mono(void);

/* Something the SCREEN shows has changed: rebuild the text lines on the next
 * main-thread pass. Safe from any thread (the BLE callbacks use it). */
void shell_ui_dirty(void);

/* Re-apply the user's chosen ORIENTATION. The main screen honours it; every
 * menu is forced portrait, so returning to the main screen has to put it
 * back -- and only the shell holds the activity that can. */
void shell_orient_apply(void);

/* The live activity, or NULL once it is destroyed -- which it can be while
 * the foreground service keeps running. Anything that reaches Java through it
 * must handle the NULL: a teardown racing a timer retry would otherwise
 * dereference it. */
struct ANativeActivity *shell_activity(void);

/* THE SERVICE HEARTBEAT'S ONE ENTRY POINT.
 *
 * The foreground service ticks the app once a second and outlives the
 * activity by days, so everything that must keep happening with no UI --
 * evaluating the alarm, refreshing the lock-screen notification, repairing
 * stranded links, pushing to the server, re-arming a meter, reconciling the
 * registry -- happens here.
 *
 * It is ONE call because the transport should not know that list. dexble.c's
 * tick used to name six functions from six modules, so the BLE transport --
 * the layer furthest from any of them -- was the place that knew which
 * workflows exist and in what order they run. Adding a seventh meant editing
 * the transport. The shell owns the list, as it already does for the
 * activity's own timer; the transport asks for a tick. */
void shell_service_tick(void);

/* Repaint NOW if there is a window to paint on. The workflows use this after
 * a change the user is waiting to see; the 1 Hz tick would otherwise show it
 * up to a second later. */
void shell_repaint(void);

/* Re-apply the KEEP SCREEN ON preference to the window. */
void shell_apply_screen_on(void);

/* Is this the MAIN (render) thread? JNI through the activity, and every
 * rebuild of the shared text lines, is legal only there. */
int shell_on_main(void);

/* The first-run permission RATIONALE screen: modal, and shown before any
 * permission dialog so the user knows what they are being asked for. */
int shell_gate(void);
void shell_gate_done(void);

/* Crash breadcrumb: the last checkpoint this process passed. The handler
 * prints it, so it must be a pointer to a STRING LITERAL -- never a buffer
 * that can be freed or rewritten. */
void shell_where(const char *label);

#endif
