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
/* The reading that computes a pending rescale must be at most this much newer
 * than the request, or the reference is stale and the pending EXPIRES. */
#define RESCALE_PEND_WINDOW_S (15L * 60)
/* How far a user-entered fingerstick may move the displayed value: +-25%.
 *
 * Chosen against published CGM accuracy rather than picked: a Dexcom G7/Stelo
 * quotes a MARD around 8-9%, and readings beyond roughly three times that
 * from the sensor's own number are far likelier to be a mistyped entry, a
 * fingerstick taken during a fast change, or a failing sensor than a real
 * calibration offset. Refusing them is the conservative direction -- a
 * rejected calibration leaves the old value, an accepted wrong one rewrites
 * every reading the sensor produces.
 *
 * LOAD-BEARING: app/store.h derives STORE_GLU_MIN/MAX from this pair, so
 * widening it widens what the log will hold. */
#define RESCALE_MIN_PM 750  /* -25% */
#define RESCALE_MAX_PM 1250 /* +25% */

/* ---- lifecycle ---- */

/* Where the two states persist. Call once, before calib_load. */
void calib_paths(const char *dir);
/* Restore both from disk: resumes a queued calibration that is still fresh
 * (and records a FAILED one that is not), and the active rescale factor. */
void calib_load(void);
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
void calib_try_locked(int sensor_id);
/* Is a calibration queued for this sensor? (>0 = the queued value.) */
int calib_queued_for(int sensor_id);

/* ---- the user's actions ---- */

/* CONFIRM on the calibration keypad: queue `mgdl` durably for `sensor_id`.
 * The caller should then select the driver and call calib_try_locked for an
 * opportunistic first attempt. */
void calib_queue(int sensor_id, int mgdl);
/* Discard the queued calibration entirely. */
void calib_cancel(void);

/* CONFIRM on the rescale keypad. `raw` is the sensor's live raw reading, or 0
 * if there is none yet -- in which case the target is HELD (persisted) until
 * the next reading rather than lost. */
void calib_rescale_set(int sensor_id, int raw, int target_mgdl);
/* Turn rescaling off, discarding any pending target and any notice. */
void calib_rescale_stop(void);
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
};

void calib_view(int sensor_id, struct calib_view *out);

/* ---- what this module needs from its host ---- */

/* The calibration state changed in a way the user can see. Implemented by
 * main.c; declared here because it is this module's only upward call, and a
 * dependency worth stating rather than discovering. */
void calib_repaint(void);

#endif
