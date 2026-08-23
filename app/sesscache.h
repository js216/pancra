// SPDX-License-Identifier: GPL-3.0
// sesscache.h --- the last-known session clock, per sensor, across restarts
// Copyright 2026 Jakob Kastelic
/*
 * A CACHE, NOT A FRAME. This lived in model.c, and it is the reason
 * reconcile.c included model.h: the reconcile tick flushes the cache, and to
 * say so it had to name the module that ASSEMBLES THE SCREEN. The frame
 * builder then depended on the workflow tier and the workflow tier on it, in
 * a ring through four more files.
 *
 * What it is: the session clock, the state byte and the prediction all live
 * in the DRIVER, which is per-process state built from 0x4e responses. The
 * last reading, its trend and its age survive a restart because they are
 * replayed from readings.csv -- so after every launch the main screen showed
 * a glucose value and its age while PRED and the session countdown sat blank
 * for up to a full five-minute cadence, waiting for the sensor to answer. Two
 * of the four numbers on one line disappearing, repeatedly, for no reason the
 * user can see.
 *
 * The clock is stored WITH the wall time it was read at and projected forward
 * on load, exactly as driver_get_session projects it between responses:
 * storing the raw number would restore a countdown frozen at whenever the app
 * last ran, which is worse than blank because it looks live.
 *
 * The live driver ALWAYS wins -- this is consulted only while have_reading is
 * still 0 for that link, i.e. before its first response of this process.
 *
 * EVERY FUNCTION HERE IS SAFE TO CALL FROM ANY THREAD, and it has to be: the
 * restore is on the RENDER path (build_model, on MAIN, several times a
 * second), while the put and the flush run from sensor_reconcile on both the
 * activity's timer and the foreground service's tick -- the one that outlives
 * the activity. (A put on the render path makes the recording depend on the
 * repaint rate.) As plain globals shared by those paths, the table, its count
 * and the save-rate state let a flush render a row half from before a 0x4e
 * response and half from after it, and clear the dirty
 * flag for a change that had landed while the file was being written. See the
 * lock block at the head of sesscache.c.
 *
 * What that does NOT buy: the restore and the put are separately atomic, not
 * atomic together. A caller that puts and then restores may see its own value
 * or a newer one. Nothing does both: build_model only ever restores, and the
 * reconcile tick only ever puts.
 */
#ifndef PANCRA_SESSCACHE_H
#define PANCRA_SESSCACHE_H
#include "loadresult.h" /* what a load actually found */

struct dex_session;

/* Where it persists. Call once, before sess_load. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int sess_paths(const char *dir);
enum load_result sess_load(void);
/* Write the cache if it is dirty and the rate limit allows. On the tick. */
void sess_flush(long now);

/* Record a LIVE session for `id`.
 *
 * CALLED FROM THE RECONCILE TICK, NOT FROM A RENDER. It is cheap
 * -- it only marks the file dirty when the clock actually moved -- but cheap
 * was never the question: called while drawing, it made what survives a
 * restart depend on how often the screen was repainted, and recorded nothing
 * at all once the activity was gone and only the service was ticking. */
void sessc_put(int id, const struct dex_session *s, long now);
/* Fill `out` from the cache, projecting the clock forward to `now`. 1 when a
 * usable cached session was restored. */
int sessc_restore(int id, long now, struct dex_session *out);

#ifdef APP_FAULTS
/* HELD OPEN ON DEMAND, between the two halves of a row inside the render.
 * That window is a handful of instructions wide, and it is what sessc_lk
 * exists for -- so a build without the lock wrote a perfectly coherent file
 * on every run and the torn-row assertion passed against it. A pointer rather
 * than a bare yield so the suite can install it only around the section that
 * needs it; app_fault_gap_here in util.h is the same device. Test builds
 * only; nothing that ships defines APP_FAULTS. */
extern void (*sess_fault_gap_here)(void);
#endif

#endif
