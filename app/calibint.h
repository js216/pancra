// SPDX-License-Identifier: GPL-3.0
// calibint.h --- what the two correction modules share, and nobody else does
// Copyright 2026 Jakob Kastelic

/* app/calib.c was two independently persisted mechanisms in one
 * file: the CALIBRATION QUEUE (a value confirmed by the user, written to the
 * sensor, retried until the sensor answers, with a verdict of its own) and
 * the RESCALE (a local multiplicative correction computed from a fingerstick,
 * with an active factor, a pending target and two transient notices). They
 * had nothing in common but a file, a lock and a tick, and they were told
 * apart only by the prefixes on twenty-odd file statics -- so a change to one
 * was a change in the middle of the other's state.
 *
 * They are app/calibq.c and app/rescale.c now, each owning its own record,
 * its own file and its own rules. app/calib.c is what is genuinely shared and
 * nothing else:
 *
 *   THE LOCK      one lock over both, because calib_view renders a device row
 *                 from BOTH at one instant, and a row that mixes two instants
 *                 shows one sensor's correction beside another's verdict.
 *   THE TRIGGER   a reading drives both (the rescale activates a pending
 *                 target from it; a queued calibration is attempted on the
 *                 stream it proves is alive).
 *   THE TICK      both have deadlines and both retry a save that did not land.
 *   THE STARTUP   one directory, two files, loaded together.
 *   THE VIEW      one struct calib_view, filled by both halves.
 *
 * EVERY NAME HERE IS cal_. Not because the prefix is pretty: this header
 * speaks for three translation units, and `make -f test/Makefile inclusions`
 * refuses a header that collects several modules' declarations UNLESS they
 * are one named contract -- which is exactly what this is, and the single
 * prefix is how that is checked mechanically rather than asserted.
 *
 * NOT PUBLIC. app/calib.h is what the rest of the app sees. Nothing outside
 * these three files may include this one.
 */
#include "compiler.h" /* PANCRA_MUST_USE: an answer no caller may drop */

#ifndef PANCRA_CALIBINT_H
#define PANCRA_CALIBINT_H

/* NOT #include "calib.h". The public header is what the rest of the app sees,
 * and every one of these three files includes it directly; including it from
 * here as well would make the private header and the public one one node in
 * the include graph, which is a cycle (`make -f test/Makefile inclusions`
 * says so). A pointer needs no definition. */
struct calib_view;

/* ---- THE LOCK, WHICH IS THE COORDINATOR'S ---------------------------
 *
 * cal_lk covers BOTH records. Its rank is a LEAF: the order is driver_lk ->
 * cal_lk, because the `_locked` operations already arrive holding the
 * driver's, and nothing under this lock may call a driver_* function.
 *
 * calfile_lk is the other one, and the order is calfile_lk -> cal_lk, always.
 * A save renders its line under cal_lk, RELEASES it, and does the I/O holding
 * calfile_lk alone -- so no reader ever waits on a disk. See the long note in
 * calib.c for what that prevents (a per-frame calib_view spinning through an
 * fsync is the ANR shape). */
void cal_lock(void);
void cal_unlock(void);
void cal_file_lock(void);
void cal_file_unlock(void);

/* ---- THE TWO FILES, READ AND WRITTEN THE SAME WAY ------------------- */

/* Both files are a single line of comma-separated integers, parsed as a whole
 * record rather than field by field: a partial parse is not a usable answer.
 * Missing trailing fields stay 0, which is how an older file upgrades. */
/* THE MOST FIELDS EITHER FILE HAS, and the bound this decoder stages into.
 * cal.q is seven and rescale.cfg is six; the constant is here so a caller
 * cannot ask for more than the decoder can hold. */
#define CAL_FIELDS_MAX 8

/* Decode a whole line of exactly `n` integers. 1 when every field was there
 * and fitted, 0 otherwise -- and then `v` is UNTOUCHED, so a refused line
 * cannot be half-applied. */
PANCRA_MUST_USE int cal_parse_ints(const char *b, long *v, int n);
/* Read one line. THREE ANSWERS, not two.
 *
 * READ_NONE  the file is not there. A first run, or a state that was never
 *            written: the defaults are correct and nothing is lost.
 * READ_OK    the line is in `b`.
 * READ_FAIL  the file EXISTS and could not be read whole.
 *
 * THE THIRD IS NOT THE FIRST. Folded together, an unreadable calibration
 * queue is indistinguishable from never having queued one -- the app silently
 * forgets a calibration the user confirmed, and a rescale factor that was
 * scaling every reading from a sensor silently reverts to 1.000. Both look
 * exactly like a fresh install, which is the one thing they are
 * not. */
#define READ_NONE 0
#define READ_OK   1
#define READ_FAIL (-1)
int cal_read_line(const char *path, char *b, int cap);
/* Replace-by-rename. CALIB_OK, or a CALIB_* failure. */
int cal_write_line(const char *path, const char *b, int n);

/* ---- A PERSISTED WALL-CLOCK STAMP, TURNED INTO AN IN-PROCESS DEADLINE ---
 *
 * The ONE place either loader crosses from realtime to monotonic, and the one
 * place a clock correction can still reach this module. How much of `window`
 * is left for something stamped at `stamp`, or NO_WINDOW_LEFT when it cannot
 * be honoured at all. CLOCK_SKEW_TOL_S in calib.h has the full argument for
 * each branch; in short:
 *
 *   age > window        the ordinary lapse, and the forward-skew answer too:
 *                       a clock jumped forward across a restart cannot be told
 *                       from a phone that was genuinely off that long, and
 *                       both mean "enter it again".
 *   age within [0,window]  resume with what is left -- NOT with a fresh full
 *                       window, which would let a restart loop extend a stale
 *                       fingerstick indefinitely.
 *   -tolerance <= age < 0  a routine backward nudge. Treat as this instant and
 *                       grant the full window rather than discard the user's
 *                       work over a two-second correction.
 *   age < -tolerance    the clock really moved; the age is unknowable, so the
 *                       reference expires VISIBLY rather than being applied. */
#define NO_WINDOW_LEFT (-1L)
long cal_window_left(long stamp, long window);

/* ---- WHAT THE COORDINATOR ASKS OF EACH MODULE ----------------------- */

/* Startup. Each takes its own path under the data directory. 1 on success. */
int cal_q_paths(const char *dir);
int cal_r_paths(const char *dir);
/* Load the persisted record. CALIB_OK or a CALIB_* failure. */
int cal_q_load(void);
int cal_r_load(void);
/* The deadline half of a tick, and the save-retry half. CALLER HOLDS
 * cal_file_lock; each takes and drops cal_lock itself, because a save inside
 * drops it again for the syscalls. Both answer 1 if the screen must be
 * redrawn. */
int cal_q_tick(long now_mono);
int cal_r_tick(long now_mono);
/* Fill this module's half of a device row. CALLER HOLDS cal_lock, so that
 * both halves describe ONE instant. */
void cal_q_view(int sensor_id, struct calib_view *out);
void cal_r_view(int sensor_id, struct calib_view *out);

/* ---- THE DRIVER'S SERIALISATION, WHICH IS THE QUEUE'S ALONE -----------
 *
 * These four are registered with the driver (driver_set_cal_ops) and are
 * called with the DRIVER'S lock already held, so the documented order
 * driver_lk -> cal_lk holds by construction and nothing they reach may call
 * back into the driver. The registration itself is the coordinator's, because
 * the tick it registers runs BOTH halves. */
void cal_q_attempt_locked(int link, int sensor_id);
int cal_q_queue_locked(int sensor_id, int mgdl);
int cal_q_cancel_locked(void);
int cal_q_queued_for_locked(int sensor_id);

#endif
