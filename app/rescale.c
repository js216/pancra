// SPDX-License-Identifier: GPL-3.0
// rescale.c --- the local correction: arithmetic on what the sensor reports
// Copyright 2026 Jakob Kastelic

/* THE OTHER OF THE TWO CORRECTIONS (see calib.h); the first is app/calibq.c
 * and what they share is app/calibint.h.
 *
 * A rescale is entirely LOCAL: a multiplicative factor in permille, computed
 * from a fingerstick against this sensor's own raw reading, applied to every
 * reading of that sensor whose timestamp is at or after the moment the factor
 * became effective. The sensor is never told, nothing is written to it, and
 * there is no verdict to wait for -- which is exactly why it is a different
 * mechanism from the queue and not a mode of it.
 */
#include "calib.h"
#include "calibint.h"
#include "clock.h"
#include "dexdriver.h"
#include "log.h"
#include "uimodel.h"
#include "util.h"
#include <stdio.h>

/* RESCALE: a persistent multiplicative correction (permille; 1000 = none) the
 * user sets from a fingerstick. Applied to THIS CGM's readings whose
 * timestamp is AT OR AFTER the moment rescaling was (re)activated -- so a
 * backfilled point with an OLDER timestamp is never rescaled even though it
 * arrives later. Clamped to +-25%. */
/* ---- THE RESCALE, AS ONE RECORD, for the same reasons ------ */
struct resc {
   int pm; /* active factor; 1000 = off */
   int id; /* sensor id it applies to */
   long t; /* activation instant; readings t>=this scale */

   /* PENDING target: a confirmed rescale that could not be computed yet because
    * no live reading was available. Held (PERSISTED, incl. its request time)
    * until the next reading for this sensor, then turned into a factor -- never
    * silently lost, and it survives an app restart. */
   struct {
      int mgdl;       /* target mg/dL awaiting a reading; 0 = none */
      int id;         /* sensor id it is for */
      long t;         /* realtime_s() when the user requested it */
      long expire_at; /* the DEADLINE, an interval: see the block below */
   } pend;

   /* Last attempt exceeded +-25% and was REJECTED (not clamped), or a pending
    * one EXPIRED. Shown in the RESCALE line until the user sets a valid one or
    * stops. In-memory only (a transient notice). */
   struct {
      int reject;
      int reject_id;
      int expired;
      int expired_id;
   } note;
};

static struct resc g_r = {.pm = 1000};
static char g_rescale_path[256]; /* persistence file */

/* Latest RAW (pre-rescale) reading per link -- the reference a future factor
 * is computed from, which is the only reason it is kept. */
static int g_link_raw[LINK_MAX];

/* THE STATE LOCK, over EVERYTHING this file holds.
 *
 * It started as the rescale half's alone: the queue was serialised by the
 * driver instead (the `_locked` functions below, reached through
 * driver_set_cal_ops with the driver's own lock held) while the rescale half
 * had nothing at all. Two locks over one persisted record is how a save came
 * to serialise a queue from one instant beside a verdict from another, so
 * cal_lk covers both now -- the queue, the last-resolved calibration and the
 * rescale factor.
 *
 * All of it is written by the user's actions on the MAIN thread
 * (calib_rescale_set / _stop, the queue and cancel), read and written by the
 * reading path on a BINDER thread (calib_on_reading, which activates a
 * pending target from the sample it was waiting for; calib_cal_result,
 * which records the sensor's verdict), expired by calib_tick on the main
 * looper AND the service heartbeat, and read by every frame the renderer
 * draws (calib_view).
 *
 * A factor is not one number: it is (pm, sensor id, effective-from). The
 * activation writes those three separately, so a reader between the first and
 * the second gets the new factor against the previous sensor's id -- which
 * is one
 * sensor's correction applied to another sensor's readings, in stored data.
 * The same shape covers the last-resolved calibration, four fields written by
 * a GATT callback and read as a set by the device row.
 *
 * A LEAF, and it must stay one: the order is driver_lk -> cal_lk, because the
 * `_locked` operations already arrive holding the driver's. Nothing here may
 * call a driver_* function while holding it -- calib_view asks the driver for
 * the queued value BEFORE taking it, for exactly that reason. */

static int g_resc_unsaved; /* rescale.cfg is behind the factor in memory */

/* The rescale's undo, by the same argument: `pend.expire_at` travels with it
 * because a pending target restored without its deadline is a target that
 * never expires, and this module's rule is that a stale reference expires
 * VISIBLY. */
typedef struct resc resc_undo;

static void resc_snapshot(resc_undo *u)
{
   *u = g_r;
}

static void resc_restore(const resc_undo *u)
{
   g_r = *u;
}

/* CALLER HOLDS calfile_lk AND cal_lk, and gets both back; cal_lk is dropped
 * across the write. See calq_save. */
static int rescale_save(const resc_undo *undo)
{
   char b[96];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%d,%ld\n", g_r.id, g_r.pm, g_r.t,
                    g_r.pend.id, g_r.pend.mgdl, g_r.pend.t);
   cal_unlock();
   int wrote = cal_write_line(g_rescale_path, b, clampn(n, sizeof b));
   cal_lock();
   if (wrote == 0) {
      g_resc_unsaved = 0;
      return CALIB_OK;
   }
   if (undo) {
      resc_restore(undo);
      return CALIB_UNSAVED;
   }
   g_resc_unsaved = 1;
   return CALIB_UNSAVED;
}

/* ---- the rescale factor ----------------------------------------------- */

/* raw * factor, rounded. */
static int rescale_apply(int raw, int pm)
{
   return (int)((((long)raw * pm) + 500) / 1000);
}

/* The factor to apply to a reading (src, timestamp t), or 1000 (none). */
static int rescale_pm_for(int src, long t)
{
   if (g_r.pm != 1000 && g_r.id == src && t >= g_r.t)
      return g_r.pm;
   return 1000;
}

/* Turn a (target, raw) pair into an active factor for sensor `id`, effective
 * from `t`. A factor beyond +-25% is REJECTED (not clamped) -- the reading is
 * too far from the entered value to be a plausible correction -- and flagged
 * for the RESCALE line. Returns 1 if it activated, 0 if rejected or
 * uncomputable. */
static int rescale_activate(int id, int target_mgdl, int raw, long t)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   int pm = calib_rescale_preview(target_mgdl, raw);
   if (pm < RESCALE_MIN_PM || pm > RESCALE_MAX_PM) {
      g_r.note.reject    = 1;
      g_r.note.reject_id = id;
      LOGI("rescale REJECTED: %d mg/dL over raw %d -> %d permille exceeds "
           "+-25%%",
           target_mgdl, raw, pm);
      return 0;
   }
   g_r.pm          = pm;
   g_r.id          = id;
   g_r.t           = t;
   g_r.note.reject = 0; /* a valid one clears any prior rejection */
   LOGI("rescale active: %d mg/dL over raw %d -> %d permille (id %d)",
        target_mgdl, raw, pm, id);
   return 1;
}

/* Forget a pending target, whatever became of it -- including its deadline,
 * which would otherwise expire the NEXT target early (or, once the tick had
 * fired on it, raise an expiry notice for a target that is no longer there). */
static void rescale_pend_clear(void)
{
   g_r.pend.mgdl      = 0;
   g_r.pend.id        = 0;
   g_r.pend.t         = 0;
   g_r.pend.expire_at = 0;
}

int calib_rescale_preview(int target_mgdl, int raw)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   return (int)((((long)target_mgdl * 1000) + (raw / 2)) / raw);
}

int calib_rescale_pm(void)
{
   cal_lock();
   int pm = g_r.pm;
   cal_unlock();
   return pm;
}

int calib_raw_on_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   cal_lock();
   int raw = g_link_raw[link];
   cal_unlock();
   return raw;
}

int calib_rescale_engaged(int sensor_id)
{
   cal_lock();
   int on = (g_r.pm != 1000 && g_r.id == sensor_id) ||
            (g_r.pend.mgdl > 0 && g_r.pend.id == sensor_id);
   cal_unlock();
   return on;
}

int calib_rescale_set(int sensor_id, int raw, int target_mgdl)
{
   if (target_mgdl <= 0)
      return CALIB_UNSAVED;
   /* THE WHOLE TRANSITION under the locks, and it is UNDONE if the write
    * fails: this is the user's own change, so "nothing happened" is the
    * honest answer and the value can simply be entered again.
    *
    * A frame CAN see the factor during the write -- rescale_save releases
    * cal_lk across the fsyncs and the rename, because holding it there is
    * the ANR shape (see calfile_lk). So the rollback is not invisible; it is
    * a factor shown for the length of one disk write and then withdrawn,
    * which is the price of not spinning the looper. calfile_lk is held
    * throughout and is what keeps this the only WRITER during that window,
    * so the state the rollback restores is still the state it snapshotted. */
   cal_file_lock();
   cal_lock();
   resc_undo undo;
   resc_snapshot(&undo);
   /* A fresh attempt supersedes any prior rejection / expiry notice. */
   g_r.note.reject  = 0;
   g_r.note.expired = 0;
   if (raw > 0) {
      rescale_activate(sensor_id, target_mgdl, raw, realtime_s());
      rescale_pend_clear();
   } else {
      /* No live reading yet: HOLD the target rather than lose it. The next
       * reading for this sensor computes the factor. */
      g_r.pend.mgdl = target_mgdl;
      g_r.pend.id   = sensor_id;
      /* TWO STAMPS FOR TWO JOBS. The realtime one is PERSISTED and exists so
       * the next launch can work out how much of the window a target set
       * before a restart has left. The monotonic one decides the expiry while
       * this process runs, and cannot be moved by a clock correction. */
      g_r.pend.t         = realtime_s();
      g_r.pend.expire_at = mono_s() + RESCALE_PEND_WINDOW_S;
      LOGI("rescale %d mg/dL queued: awaiting a reading to compute factor",
           target_mgdl);
   }
   int rc = rescale_save(&undo);
   cal_unlock();
   cal_file_unlock();
   return rc;
}

int calib_rescale_stop(void)
{
   cal_file_lock();
   cal_lock();
   resc_undo undo;
   resc_snapshot(&undo);
   LOGI("rescaling turned OFF by user (was %d permille, pend %d)", g_r.pm,
        g_r.pend.mgdl);
   g_r.pm = 1000;
   g_r.id = 0;
   g_r.t  = 0;
   rescale_pend_clear();
   g_r.note.reject  = 0; /* and clear any rejection / expiry notice */
   g_r.note.expired = 0;
   int rc           = rescale_save(&undo);
   cal_unlock();
   cal_file_unlock();
   return rc;
}

/* ---- the reading path -------------------------------------------------- */

int calib_on_reading(int sensor_id, int link, long t, int raw_mgdl,
                     int *applied_pm, int *started)
{
   /* calfile_lk covers the whole call because an activation or an expiry may
    * write; the common case (nothing pending) takes an uncontended mutex and
    * writes nothing. */
   cal_file_lock();
   cal_lock();
   if (link >= 0 && link < LINK_MAX)
      g_link_raw[link] = raw_mgdl;
   if (started)
      *started = 0;

   /* A rescale target was waiting for a reading to compute its factor: this
    * is that reading -- BUT only if the user has been waiting less than
    * RESCALE_PEND_WINDOW_S for it. Past that the fingerstick reference is
    * stale, so EXPIRE it (visibly) rather than applying it to a much-later
    * reading. Otherwise activate from THIS raw, effective from THIS timestamp,
    * so the reference reading itself shows the entered value.
    *
    * THE WAIT IS MEASURED MONOTONICALLY, and the difference is not cosmetic.
    * NOT the READING's timestamp minus the request's wall clock: those are
    * two instants from two different sources -- one the app stamped, one that
    * arrives with the sample -- so their difference is not an elapsed time at
    * all. A wall-clock correction between the request and the reading moves
    * it by the whole correction: forward,
    * and the first reading to arrive instantly EXPIRES a target the user set
    * moments ago, so the number they entered is thrown away and the row tells
    * them the reference went stale; backward, and it goes negative, so no
    * reading ever expires it and a fingerstick from an hour ago is applied to
    * the sample that finally shows up.
    *
    * What the window is actually about is how long the user has been waiting
    * since they typed the number, and that is an interval. g_r.pend.t
    * stays -- it is persisted, and the next launch needs it -- but nothing
    * running compares against it. */
   if (g_r.pend.mgdl > 0 && g_r.pend.id == sensor_id) {
      if (g_r.pend.expire_at > 0 && mono_s() > g_r.pend.expire_at) {
         LOGI("pending rescale %d mg/dL EXPIRED (%ld s waiting for a reading)",
              g_r.pend.mgdl, RESCALE_PEND_WINDOW_S);
         g_r.note.expired    = 1;
         g_r.note.expired_id = sensor_id;
      } else {
         rescale_activate(sensor_id, g_r.pend.mgdl, raw_mgdl, t);
         if (started)
            *started = 1;
      }
      rescale_pend_clear();
      /* NO ROLLBACK HERE, and the reason is worth stating: this runs on the
       * READING path, where the factor has already been computed from a
       * sample that will not come again. Undoing it would leave a pending
       * target that the next reading re-activates from a different raw --
       * a different factor, silently. The file is stale until the next
       * write succeeds; the state is the one the reading justified. */
      (void)rescale_save(0);
   }

   int pm = rescale_pm_for(sensor_id, t);
   cal_unlock();
   cal_file_unlock();
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(raw_mgdl, pm) : raw_mgdl;
}

int calib_on_backfill(int sensor_id, long t, int mg_dl, int *applied_pm)
{
   cal_lock();
   int pm = rescale_pm_for(sensor_id, t);
   cal_unlock();
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(mg_dl, pm) : mg_dl;
}

/* Under the lock like every other writer of this state. calib_load has ONE
 * caller today (main.c, at startup), so the concurrent load is not a race
 * that can currently happen -- the lock is here because a loader that
 * installs a whole record must not be the one path that assumes it is alone,
 * and because a restore that re-reads while live is a change of one call
 * site. */
int cal_r_load(void)
{
   cal_file_lock(); /* see calq_load: the read and the install are
                     * one decision about what the file says */
   char b[96];
   int rd = cal_read_line(g_rescale_path, b, sizeof b);
   /* ---- DECODED BEFORE ANYTHING IS TOUCHED ------------------
    *
    * The decode happens HERE, before the lock and before the clear below,
    * because a line that does not decode must leave the live state exactly
    * as it was -- and the clear is what a load does to state it is about to
    * replace. Decoding after it would mean a file this program did not write
    * still wiped a factor that is scaling every reading on the screen. */
   long v[6] = {0, 0, 0, 0, 0, 0};
   if (rd == READ_OK && !cal_parse_ints(b, v, 6)) {
      LOGW("calibration: rescale.cfg does not decode; the factor in memory "
           "is left as it is");
      cal_file_unlock();
      return CALIB_UNSAVED;
   }
   cal_lock();
   /* See calq_load: a file that could not be READ does not get to wipe a
    * factor that is scaling every reading from a sensor. */
   if (rd == READ_FAIL) {
      cal_unlock();
      cal_file_unlock();
      return CALIB_UNSAVED;
   }
   g_resc_unsaved = 0; /* see calq_load: a load defines the state */
   g_r.pm         = 1000;
   g_r.id         = 0;
   g_r.t          = 0;
   rescale_pend_clear();
   /* The notices describe attempts made in this session; a load starts with
    * none, except one this load itself raises just below. */
   g_r.note.reject  = 0;
   g_r.note.expired = 0;
   if (rd != READ_OK) {
      cal_unlock();
      cal_file_unlock();
      return CALIB_OK; /* READ_NONE: a first run */
   }
   if (v[1] >= RESCALE_MIN_PM && v[1] <= RESCALE_MAX_PM && v[1] != 1000) {
      g_r.id = (int)v[0];
      g_r.pm = (int)v[1];
      g_r.t  = v[2];
   }
   if (v[4] > 0) { /* a target was awaiting a reading when we last ran */
      long pend_t = v[5];
      /* Restart must NOT lose a pending rescale -- BUT only honour it if its
       * request is still within the freshness window; a target set before a
       * long downtime has a stale fingerstick reference and must not silently
       * apply to a much-later reading.
       *
       * The queue's reconciliation, with the same reasoning: see calq_load.
       * The persisted stamp is wall-clock because nothing else survives a
       * reboot; what is LEFT of the window becomes this process's monotonic
       * deadline, and nothing running compares against pend_t again. */
      long left = pend_t > 0 ? cal_window_left(pend_t, RESCALE_PEND_WINDOW_S)
                             : NO_WINDOW_LEFT;
      if (left != NO_WINDOW_LEFT) {
         g_r.pend.id        = (int)v[3];
         g_r.pend.mgdl      = (int)v[4];
         g_r.pend.t         = pend_t;
         g_r.pend.expire_at = mono_s() + left;
      } else {
         g_r.note.expired    = 1; /* surface it, never a silent drop */
         g_r.note.expired_id = (int)v[3];
      }
   }
   cal_unlock();
   cal_file_unlock();
   return CALIB_OK;
}

/* ---- THE RESCALE'S HALF OF A TICK --------------------------------------
 *
 * CALLER HOLDS cal_file_lock and passes the MONOTONIC now. Returns 1 if the
 * screen must be redrawn. See cal_q_tick for the locking, which is the same.
 *
 * A pending target expires on the CLOCK, not only when a reading finally
 * arrives -- otherwise a sensor that goes quiet leaves the target looking
 * live indefinitely. */
int cal_r_tick(long now_mono)
{
   cal_lock();
   int expired = 0;
   if (g_r.pend.mgdl > 0 && g_r.pend.expire_at > 0 &&
       now_mono > g_r.pend.expire_at) {
      LOGI("pending rescale %d mg/dL EXPIRED with no reading", g_r.pend.mgdl);
      g_r.note.expired    = 1;
      g_r.note.expired_id = g_r.pend.id;
      rescale_pend_clear();
      (void)rescale_save(0); /* an expiry that cannot be written re-expires */
      expired = 1;
   }
   /* THE RETRY, for the same reason as the queue's: a transition that stood
    * without reaching the disk is tried again every tick until it lands. */
   if (g_resc_unsaved && rescale_save(0) == CALIB_OK)
      LOGI("rescale state reached the disk on a retry");
   cal_unlock();
   return expired;
}

/* CALLER HOLDS cal_lock -- see cal_q_view. */
void cal_r_view(int sensor_id, struct calib_view *out)
{
   out->rescale_pm = (g_r.pm != 1000 && g_r.id == sensor_id) ? g_r.pm : 1000;
   out->rescale_pending =
       (g_r.pend.mgdl > 0 && g_r.pend.id == sensor_id) ? g_r.pend.mgdl : 0;
   out->rescale_rejected = (g_r.note.reject && g_r.note.reject_id == sensor_id);
   out->rescale_expired =
       (g_r.note.expired && g_r.note.expired_id == sensor_id);
   out->rescale_unsaved = g_resc_unsaved;
}

int cal_r_paths(const char *dir)
{
   return data_path(g_rescale_path, sizeof g_rescale_path, dir,
                    "/rescale.cfg") != 0;
}
