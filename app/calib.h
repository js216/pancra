// SPDX-License-Identifier: GPL-3.0
// calib.h --- the two corrections that start from a fingerstick
// Copyright 2026 Jakob Kastelic

/* CALIBRATION is sent TO the sensor: the user's fingerstick becomes a value
 * the sensor itself adopts. It is queued durably, retried until the sensor
 * answers, and its outcome always surfaces -- a confirmed calibration is
 * never lost to a reconnect gap or an app restart.
 *
 * RESCALE never leaves the app: a multiplicative factor applied to this
 * sensor's readings on the way in, so the stored, plotted and alarmed value
 * matches the fingerstick while the raw stays recoverable from the CSV.
 *
 * They are one module because they share a trigger (the user enters a true
 * value), a reference (the live raw reading), and a rule -- a stale reference
 * must EXPIRE VISIBLY rather than silently apply to a much later reading.
 * They are NOT one mechanism, and nothing here lets one become the other.
 *
 * The module owns every durable thing about both: the queue, the last
 * resolved result, the factor, the pending target, and the two files they
 * persist to. What it does NOT own is the value the user is presently typing
 * (that is keypad state) or which sensor is selected (that is the driver's).
 * Callers pass a sensor id; this file never consults the UI.
 */
#ifndef PANCRA_CALIB_H
#define PANCRA_CALIB_H

/* Give up (visibly, never silently) if the sensor has not accepted within
 * this long -- a fingerstick reference goes stale, so past this we tell the
 * user to re-enter rather than apply an old value or drop it without a
 * word. */
#define CALQ_WINDOW_S (20L * 60)
/* A pending rescale must be resolved within this much ELAPSED time of the
 * request, or the fingerstick reference is stale and the pending EXPIRES. */
#define RESCALE_PEND_WINDOW_S (15L * 60)

/* ---- THE TWO CLOCKS, AND WHICH NUMBER HERE COMES FROM WHICH -------------
 *
 * Both windows above are ELAPSED TIME, so inside a running process they are
 * measured with mono_s() and nothing a clock correction does can move them.
 * See the deadline block in calib.c.
 *
 * realtime_s() stamps survive here for exactly one purpose: RESTART
 * RECONCILIATION. A monotonic clock is meaningless across a reboot -- it
 * counts from an arbitrary origin and restarts with the kernel -- so the file
 * has to record WHEN, on the wall clock, the user confirmed the calibration or
 * asked for the rescale, and the next launch subtracts that from its own wall
 * clock to learn how much of the window is left. That subtraction is the one
 * place a clock correction can still reach, and this is the tolerance it is
 * allowed.
 *
 * A NEGATIVE AGE means the persisted stamp is in the FUTURE: the wall clock
 * has moved BACKWARD since the file was written, or the file came from a phone
 * whose clock disagrees. The elapsed time is then genuinely unknown -- it
 * could be a second or a week -- so there are only two honest answers, and
 * this constant chooses between them by size:
 *
 *   within the tolerance: routine noise (an NTP nudge, a leap-second smear, a
 *     phone settling its clock at boot). Treat the age as ZERO and grant the
 *     FULL window from now. Nothing the user did is thrown away for a
 *     two-second correction, and one full window is a bounded, visible amount
 *     of extra life -- the row shows PENDING throughout it.
 *
 *   beyond the tolerance: the clock has really moved, and this module's rule
 *     is that a reference it cannot vouch for EXPIRES VISIBLY rather than
 *     being applied. The queue resolves FAILED and the pending rescale reports
 *     EXPIRED, so the user is told to enter the value again rather than the app
 *     silently pushing an unknown-age fingerstick into the sensor.
 *
 * A FORWARD-SKEWED AGE -- a positive age larger than the window -- is already
 * the ordinary expiry: too much wall time has passed, so it FAILS or EXPIRES.
 * A clock jumped forward across a restart is indistinguishable from a phone
 * that was genuinely off for that long, and both mean the same thing to the
 * person holding it: enter it again. Two minutes rather than a few seconds
 * because a phone that has just found a network can correct by more than a
 * second, and rather than an hour because an hour of unaccounted age is
 * exactly what must not be silently applied. */
#define CLOCK_SKEW_TOL_S (2L * 60)
/* How far a user-entered fingerstick may move the displayed value: +-25%.
 *
 * Chosen against published CGM accuracy rather than picked: a Dexcom G7/Stelo
 * quotes a MARD around 8-9%, and readings beyond roughly three times that
 * from the sensor's own number are far likelier to be a mistyped entry, a
 * fingerstick taken during a fast change, or a failing sensor than a real
 * calibration offset. Refusing them is the conservative direction -- a
 * rejected calibration leaves the stored value, an accepted wrong one
 * rewrites
 * every reading the sensor produces.
 *
 * LOAD-BEARING: app/store.h derives STORE_GLU_MIN/MAX from this pair, so
 * widening it widens what the log will hold. */
#define RESCALE_MIN_PM 750  /* -25% */
#define RESCALE_MAX_PM 1250 /* +25% */

/* ---- WHAT A CALIBRATION OR RESCALE CHANGE ANSWERS ----------------------
 *
 * Every transition below is a TRANSACTION: it changes the state, rewrites the
 * one-line file, and if that rewrite fails it puts the state back untouched.
 * Two outcomes, and the caller can act on either.
 *
 * They are NOT `void` with atomic_replace's result dropped: that leaves a
 * calibration the user confirmed -- the most consequential write this app
 * makes -- queued in memory, reported as queued on screen, and gone at the
 * next launch; and a rescale STOP that cannot be written leaves the factor
 * off on screen and on again after a restart, silently scaling every reading
 * from that sensor.
 *
 * CALIB_UNSAVED means NOTHING CHANGED, in memory or on disk. Both files are
 * replaced by rename, so a failed write leaves the previous file whole and
 * rolling memory back restores agreement rather than inventing it. */
#define CALIB_OK      0
#define CALIB_UNSAVED (-1)

/* ---- lifecycle ---- */

/* Where the two states persist. Call once, before calib_load. */
/* Hand the driver this module's half of the calibration queue, so the driver
 * serialises it with its own state (see dexdriver.h). Once, at startup. */
void calib_register_ops(void);

/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int calib_paths(const char *dir);
/* Restore both from disk: resumes a queued calibration that is still fresh
 * (and records a FAILED one that is not), and the active rescale factor. */
/* Restore both states. CALIB_OK when each file was read whole or was simply
 * absent (a first run); CALIB_UNSAVED when one EXISTS and could not be read,
 * which means a calibration or a live rescale factor has been lost and the
 * caller must say so rather than start as though there had never been one. */
int calib_load(void);
/* 1 Hz housekeeping: let a stuck attempt retry, and expire what has gone
 * stale -- both the calibration queue and a pending rescale target. */
void calib_tick(void);

/* ---- the reading path ---- */

/* A LIVE reading arrived from `sensor_id` on `link`, timestamped `t`, before
 * any correction. Records the raw value (a future factor is computed from
 * it), resolves a rescale target that was waiting for a reading, and returns
 * the value everything downstream should use.
 *
 * *applied_pm receives the factor used (1000 = none), for the log's rescale
 * column. *started, if non-NULL, is set when a rescale took effect ON this
 * reading -- the caller needs that to tell a calibration step apart from the
 * wearer's glucose actually moving. */
int calib_on_reading(int sensor_id, int link, long t, int raw_mgdl,
                     int *applied_pm, int *started);
/* A BACKFILLED point: the factor only, gated on the point's OWN timestamp, so
 * one that predates activation keeps its raw value. Records no raw -- a
 * factor is computed from live readings, never historical ones. */
int calib_on_backfill(int sensor_id, long t, int mg_dl, int *applied_pm);

/* The driver holds its lock and is selected to this sensor's link: the ideal
 * moment to push the queued calibration, since a reading just proved the
 * sensor is streaming. A refusal is not a loss -- the value stays queued. */
/* Try to flush the queued calibration for this sensor on this LINK. Takes the
 * driver lock itself -- it was calib_try_locked, a contract expressed only in
 * a name. */
void calib_try(int link, int sensor_id);
/* Is a calibration queued for this sensor? (>0 = the queued value.) */
int calib_queued_for(int sensor_id);

/* ---- the user's actions ---- */

/* CONFIRM on the calibration keypad: queue `mgdl` durably for `sensor_id`.
 * The caller should then call calib_try for an
 * opportunistic first attempt. */
int calib_queue(int sensor_id, int mgdl);
/* Discard the queued calibration entirely. */
int calib_cancel(void);

/* CONFIRM on the rescale keypad. `raw` is the sensor's live raw reading, or 0
 * if there is none yet -- in which case the target is HELD (persisted) until
 * the next reading rather than lost. */
int calib_rescale_set(int sensor_id, int raw, int target_mgdl);
/* Turn rescaling off, discarding any pending target and any notice. */
int calib_rescale_stop(void);
/* Is a factor active OR a target pending for this sensor? */
int calib_rescale_engaged(int sensor_id);
/* The factor `target` over `raw` WOULD produce, UNCLAMPED so the confirmation
 * screen can show a >25% value at its real size (and reject it), or 0 when
 * there is no reading to compute against. */
int calib_rescale_preview(int target_mgdl, int raw);
/* The running factor, for the active screen. 1000 = off. */
int calib_rescale_pm(void);
/* The raw reading last seen on `link`, or 0. */
int calib_raw_on_link(int link);

/* ---- what the UI shows ---- */

/* Everything the per-sensor rows display, answered for one sensor at once so
 * a caller cannot pick up half of a state that changed underneath it. */
struct calib_view {
   int queued_mgdl; /* >0: confirmed, awaiting the sensor */
   int last_mgdl;   /* last RESOLVED calibration, 0 = never */
   int last_state;  /* CAL_ST_* (ui.h) */
   long last_t;
   int rescale_pm;      /* active factor, 1000 = none */
   int rescale_pending; /* target awaiting a reading, 0 = none */
   int rescale_rejected;
   int rescale_expired;
   /* THE DISPLAYED STATE IS NEWER THAN THE FILE, one flag per file. Only an
    * AUTOMATIC transition can set either -- an activation computed from a
    * sample that will not come again, an expiry whose deadline has passed, or
    * the sensor's own ACCEPTED/REJECTED answer. Those stand rather than roll
    * back (there is nothing to ask again), so the ROW has to say that a
    * restart may not agree with it. calib_tick retries the write until it
    * lands, and then these clear.
    *
    * Two, not one: they describe two files, and a row that said NOT SAVED
    * because the OTHER file was behind would be telling the user something
    * untrue about the value in front of them. */
   int cal_unsaved;     /* the queue / LAST CAL record */
   int rescale_unsaved; /* the rescale factor */
};

void calib_view(int sensor_id, struct calib_view *out);

/* ---- what this module needs from its host ---- */

/* (This module's only upward call is shell_ui_dirty(), which shell.h already
 * declares, so there is nothing for this header to declare. It replaced a
 * second name for the same thing, which is one too many for a repaint.) */

/* THE SENSOR'S ANSWER to a calibration write: 0 accepted, >0 rejected.
 * Clears or surfaces the durably-queued calibration. Called from the driver,
 * which is why it is named pancra_*; it belongs to the calibration queue,
 * which is why it is declared here.
 *
 * WHICH CALIBRATION WAS ANSWERED, not merely how it ended. `sensor_id`,
 * `mg_dl` and `gen` are the token the driver was handed when it put THAT write
 * on the wire (see driver_calibrate), and the queue resolves only when all
 * three match what is queued right now. Anything else is discarded with a
 * warning in the log.
 *
 * WHY THAT IS NOT PEDANTRY. Take the result alone, over a driver carrying
 * only a boolean "something we sent is awaiting a reply", and a reply resolves
 * whatever is queued at the instant it lands -- a different calibration
 * whenever the user replaced one before the sensor answered. The new value is
 * recorded APPLIED (or REJECTED) having never been sent, and the LAST CAL row
 * then tells the user the sensor holds a number it was never given. A CGM
 * calibrated to a value nobody chose misreports glucose until the next
 * calibration, and the screen says everything is fine.
 *
 * A DISCARDED REPLY IS NOT A LOST CALIBRATION. The value the user actually has
 * queued stays queued, its send throttle was never stamped against it (see
 * calq_attempt_locked), and the next stream attempt writes it -- so the outcome
 * of the interleaving above is now "180 is still PENDING and goes out", which
 * is what the user asked for. */
void calib_cal_result(int result, int sensor_id, int mg_dl, unsigned gen);

#endif
