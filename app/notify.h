// SPDX-License-Identifier: GPL-3.0
// notify.h --- The ongoing notification: the app's SECOND display
// Copyright 2026 Jakob Kastelic
//
/* THE ONLY GLUCOSE DISPLAY LEFT once the activity is destroyed.
 *
 * A back-press or task-swipe tears down the window while the foreground
 * service keeps taking readings, so from then on the lock screen and status
 * bar are all the user has. That is why this renderer takes its JNI context
 * from the transport rather than from the activity, why it is driven by BOTH
 * the activity's 1 Hz timer and the service's tick thread, and why it lives
 * in its own file: it is a second, independent renderer of the same data the
 * screen shows, not a decoration on the screen's path.
 *
 * DIRTY-DRIVEN. Refreshing on every tick would re-render the plot bitmap and
 * re-post the notification when nothing had changed -- pure battery and CPU
 * cost on a 24/7 app, for a display that only moves when a reading lands or
 * the data goes stale. Callers mark; the two consumers race for the mark and
 * exactly one renders it.
 */
#ifndef NOTIFY_H
#define NOTIFY_H

/* Something the notification shows has changed: a new reading, a units or
 * status-bar setting, a primary switch. Safe from ANY thread. */
void notify_mark(void);

/* Render if marked, and notice a transition into staleness. Called from the
 * activity's 1 Hz timer and from the service tick -- whichever gets the mark
 * renders, and the other does not double-render.
 *
 * A mark that could not be served is KEPT, not consumed: rendering needs the
 * transport's JNI env and the render slot, and either can be unavailable at
 * the instant the mark is taken. Dropping the request there left an old
 * glucose on the lock screen until some unrelated event marked again. */
void notify_tick(void);

/* Refresh the lock-screen notification. Safe on any thread and with no
 * activity alive -- the service heartbeat drives it, so the notification
 * keeps tracking readings after the activity is destroyed, which is when it
 * becomes the only glucose display the user has. */
void pancra_notify_refresh(void);

#endif
