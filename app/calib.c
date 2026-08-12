// SPDX-License-Identifier: GPL-3.0
// calib.c --- calibration queue and rescale factor
// Copyright 2026 Jakob Kastelic

/* See calib.h for what the two corrections are and why they share a file.
 *
 * Everything durable about both lives here as file-static state, reached only
 * through the functions in the header. It used to be twenty-odd globals in
 * main.c, read and written from the reading path, the driver callback, the
 * keypad handlers and the renderer -- so "what can change this factor?" had
 * no answer shorter than the whole file.
 */
#include "calib.h"
#include "dexdriver.h"
#include "dexlibc.h"
#include "pancra.h" /* pancra_cal_result: the driver calls it here */
#include "stub_log.h"
#include "ui.h"
#include "util.h"
#include <stdio.h>

#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)

/* DURABLE calibration queue: a CONFIRMED calibration that has not yet been
 * ACCEPTED by the sensor. It is persisted and retried on every stream until
 * the sensor answers -- so a calibration is NEVER lost to a reconnect gap or
 * an app restart, the way a one-shot write silently was. */
static int g_calq_mgdl;  /* queued value, mg/dL; 0 = none queued */
static int g_calq_id;    /* sensor id it is for */
static long g_calq_t;    /* realtime_s() when the user confirmed it */
static long g_calq_sent; /* realtime_s() of the last write attempt; 0 = none */
static char g_calq_path[256]; /* persistence file */
/* Last RESOLVED calibration, for the per-device LAST CAL row (persisted). */
static int g_lastcal_mgdl;  /* value of the last resolved calibration */
static long g_lastcal_t;    /* realtime_s() it resolved; 0 = never */
static int g_lastcal_state; /* CAL_ST_* (ui.h) */
static int g_lastcal_id;    /* sensor id it was for */

/* RESCALE: a persistent multiplicative correction (permille; 1000 = none) the
 * user sets from a fingerstick. Applied to THIS CGM's readings whose
 * timestamp is AT OR AFTER the moment rescaling was (re)activated -- so a
 * backfilled point with an OLDER timestamp is never rescaled even though it
 * arrives later. Clamped to +-25%. */
static int g_rescale_pm = 1000; /* active factor; 1000 = off */
static int g_rescale_id;        /* sensor id it applies to */
static long g_rescale_t;        /* activation instant; readings t>=this scale */
/* PENDING target: a confirmed rescale that could not be computed yet because
 * no live reading was available. Held (PERSISTED, incl. its request time)
 * until the next reading for this sensor, then turned into a factor -- never
 * silently lost, and it survives an app restart. */
static int g_rescale_pend_mgdl; /* target mg/dL awaiting a reading; 0 = none */
static int g_rescale_pend_id;   /* sensor id it is for */
static long g_rescale_pend_t;   /* realtime_s() when the user requested it */
/* Last attempt exceeded +-25% and was REJECTED (not clamped), or a pending one
 * EXPIRED. Shown in the RESCALE line until the user sets a valid one or
 * stops. In-memory only (a transient notice). */
static int g_rescale_reject;
static int g_rescale_reject_id;
static int g_rescale_expired;
static int g_rescale_expired_id;
static char g_rescale_path[256]; /* persistence file */

/* Latest RAW (pre-rescale) reading per link -- the reference a future factor
 * is computed from, which is the only reason it is kept. */
static int g_link_raw[LINK_MAX];

/* ---- persistence ------------------------------------------------------ */

/* Both files are a single line of comma-separated integers. Parsed here
 * rather than with strtol per field because the whole line is one record and
 * a partial parse of it is not a usable answer. Missing trailing fields stay
 * 0, which is how an older file upgrades. */
static void parse_ints(const char *b, long *v, int n)
{
   int vi  = 0;
   int neg = 0;
   for (const char *p = b; *p && vi < n; p++) {
      if (*p >= '0' && *p <= '9') {
         v[vi] = (v[vi] * 10) + (*p - '0');
      } else if (*p == '-') {
         neg = 1;
      } else if (*p == ',' || *p == '\n') {
         if (neg)
            v[vi] = -v[vi];
         neg = 0;
         vi++;
         if (*p == '\n')
            break;
      }
   }
}

/* Read one line. Returns 0 if there is nothing to read. */
static int read_line(const char *path, char *b, int cap)
{
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   long n = read(fd, b, (size_t)cap - 1);
   close(fd);
   if (n <= 0)
      return 0;
   b[n] = 0;
   return 1;
}

static void write_line(const char *path, const char *b, int n)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   if (write(fd, b, (size_t)clampn(n, n + 1)) < 0) { /* best effort: a lost
                    persist only costs a retry across a restart, never a wrong
                    write */
   }
   close(fd);
}

static void calq_save(void)
{
   /* queued (id,mgdl,t) then last-resolved (mgdl,t,state,id) on one line. */
   char b[112];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%ld,%d,%d\n", g_calq_id,
                    g_calq_mgdl, g_calq_t, g_lastcal_mgdl, g_lastcal_t,
                    g_lastcal_state, g_lastcal_id);
   write_line(g_calq_path, b, clampn(n, sizeof b));
}

static void calq_clear(void)
{
   g_calq_mgdl = 0;
   g_calq_id   = 0;
   g_calq_t    = 0;
   g_calq_sent = 0;
   calq_save();
}

static void rescale_save(void)
{
   char b[96];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%d,%ld\n", g_rescale_id,
                    g_rescale_pm, g_rescale_t, g_rescale_pend_id,
                    g_rescale_pend_mgdl, g_rescale_pend_t);
   write_line(g_rescale_path, b, clampn(n, sizeof b));
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
   if (g_rescale_pm != 1000 && g_rescale_id == src && t >= g_rescale_t)
      return g_rescale_pm;
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
      g_rescale_reject    = 1;
      g_rescale_reject_id = id;
      LOGI("rescale REJECTED: %d mg/dL over raw %d -> %d permille exceeds "
           "+-25%%",
           target_mgdl, raw, pm);
      return 0;
   }
   g_rescale_pm     = pm;
   g_rescale_id     = id;
   g_rescale_t      = t;
   g_rescale_reject = 0; /* a valid one clears any prior rejection */
   LOGI("rescale active: %d mg/dL over raw %d -> %d permille (id %d)",
        target_mgdl, raw, pm, id);
   return 1;
}

/* Forget a pending target, whatever became of it. */
static void rescale_pend_clear(void)
{
   g_rescale_pend_mgdl = 0;
   g_rescale_pend_id   = 0;
   g_rescale_pend_t    = 0;
}

int calib_rescale_preview(int target_mgdl, int raw)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   return (int)((((long)target_mgdl * 1000) + (raw / 2)) / raw);
}

int calib_rescale_pm(void)
{
   return g_rescale_pm;
}

int calib_raw_on_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   return g_link_raw[link];
}

int calib_rescale_engaged(int sensor_id)
{
   return (g_rescale_pm != 1000 && g_rescale_id == sensor_id) ||
          (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id);
}

void calib_rescale_set(int sensor_id, int raw, int target_mgdl)
{
   if (target_mgdl <= 0)
      return;
   /* A fresh attempt supersedes any prior rejection / expiry notice. */
   g_rescale_reject  = 0;
   g_rescale_expired = 0;
   if (raw > 0) {
      rescale_activate(sensor_id, target_mgdl, raw, realtime_s());
      rescale_pend_clear();
   } else {
      /* No live reading yet: HOLD the target rather than lose it. The next
       * reading for this sensor computes the factor. */
      g_rescale_pend_mgdl = target_mgdl;
      g_rescale_pend_id   = sensor_id;
      g_rescale_pend_t    = realtime_s(); /* for the freshness window */
      LOGI("rescale %d mg/dL queued: awaiting a reading to compute factor",
           target_mgdl);
   }
   rescale_save();
}

void calib_rescale_stop(void)
{
   LOGI("rescaling turned OFF by user (was %d permille, pend %d)", g_rescale_pm,
        g_rescale_pend_mgdl);
   g_rescale_pm = 1000;
   g_rescale_id = 0;
   g_rescale_t  = 0;
   rescale_pend_clear();
   g_rescale_reject  = 0; /* and clear any rejection / expiry notice */
   g_rescale_expired = 0;
   rescale_save();
}

/* ---- the reading path -------------------------------------------------- */

int calib_on_reading(int sensor_id, int link, long t, int raw_mgdl,
                     int *applied_pm, int *started)
{
   if (link >= 0 && link < LINK_MAX)
      g_link_raw[link] = raw_mgdl;
   if (started)
      *started = 0;

   /* A rescale target was waiting for a reading to compute its factor: this
    * is that reading -- BUT only if it is at most RESCALE_PEND_WINDOW_S newer
    * than the request. Past that the fingerstick reference is stale, so
    * EXPIRE it (visibly) rather than applying it to a much-later reading.
    * Otherwise activate from THIS raw, effective from THIS timestamp, so the
    * reference reading itself shows the entered value. */
   if (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id) {
      if (t - g_rescale_pend_t > RESCALE_PEND_WINDOW_S) {
         LOGI("pending rescale %d mg/dL EXPIRED (reading %ld s after request)",
              g_rescale_pend_mgdl, t - g_rescale_pend_t);
         g_rescale_expired    = 1;
         g_rescale_expired_id = sensor_id;
      } else {
         rescale_activate(sensor_id, g_rescale_pend_mgdl, raw_mgdl, t);
         if (started)
            *started = 1;
      }
      rescale_pend_clear();
      rescale_save();
   }

   int pm = rescale_pm_for(sensor_id, t);
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(raw_mgdl, pm) : raw_mgdl;
}

int calib_on_backfill(int sensor_id, long t, int mg_dl, int *applied_pm)
{
   int pm = rescale_pm_for(sensor_id, t);
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(mg_dl, pm) : mg_dl;
}

/* ---- the calibration queue --------------------------------------------- */

int calib_queued_for(int sensor_id)
{
   return (g_calq_mgdl > 0 && g_calq_id == sensor_id) ? g_calq_mgdl : 0;
}

void calib_queue(int sensor_id, int mgdl)
{
   if (mgdl <= 0)
      return;
   g_calq_mgdl = mgdl;
   g_calq_id   = sensor_id;
   g_calq_t    = realtime_s();
   g_calq_sent = 0;
   calq_save();
   LOGI("calibration QUEUED: %d mg/dL (id %d)", mgdl, sensor_id);
}

void calib_cancel(void)
{
   LOGI("queued calibration %d mg/dL cancelled by user", g_calq_mgdl);
   calq_clear();
}

/* Record how a queued calibration ended, and stop queueing it. */
static void calq_resolve(int state)
{
   g_lastcal_mgdl  = g_calq_mgdl;
   g_lastcal_t     = realtime_s();
   g_lastcal_state = state;
   g_lastcal_id    = g_calq_id;
   calq_clear();
}

void calib_try_locked(int sensor_id)
{
   if (g_calq_mgdl <= 0 || g_calq_id != sensor_id)
      return;
   struct dex_cal c;
   driver_get_cal(&c);
   /* The sensor answered and does NOT permit calibration at all (a factory-
    * calibrated Stelo, say). Distinct from a value the sensor REJECTS: this
    * is "the device does not support calibration", so say NOT SUPPORTED.
    * Fail VISIBLY at once rather than leaving it PENDING until the window
    * lapses. This only drops the queued value -- it does NOT lock calibration
    * out: a later user-initiated calibration re-queues and re-probes
    * permission afresh. */
   if (c.have && !c.permitted) {
      LOGI("calibration not permitted by this sensor; queued %d mg/dL not sent",
           g_calq_mgdl);
      calq_resolve(CAL_ST_NOTSUP);
      calib_repaint();
      return;
   }
   /* Permission not yet known: PROBE it (0x32). driver_calibrate refuses
    * without a positive answer, and nothing else sends this probe during
    * streaming, so the calibration could otherwise never proceed. The write
    * itself goes on the next stream once the reply sets cal.permitted.
    *
    * BE GENTLE with a sensor that may not want calibrations: throttle the
    * probe to at most once a minute (cal.asked is when we last asked), so a
    * Stelo that never answers is nudged only a handful of times before the
    * window lapses and it FAILS -- never hammered. A sensor that answers "no"
    * is caught by the fast-fail above and never probed again. */
   if (!c.have) {
      if (c.asked == 0 || realtime_s() - c.asked >= 60) {
         driver_cal_bounds();
         LOGI("calibration queued: probing 0x32 permission before writing");
      }
      return;
   }
   /* Permitted: send the calibration, but only if we are not already awaiting
    * a reply from a recent send (calib_tick clears g_calq_sent after 60 s of
    * silence). One 0x34 per minute at most -- gentle, and the sensor's reply
    * normally resolves it on the first try. */
   if (g_calq_sent > 0 && realtime_s() - g_calq_sent < 60)
      return;
   if (driver_calibrate(g_calq_mgdl)) {
      g_calq_sent = realtime_s();
      LOGI("calibration %d mg/dL submitted from queue, awaiting sensor reply",
           g_calq_mgdl);
   }
}

/* Driver callback: the sensor answered a calibration we sent. */
void pancra_cal_result(int result)
{
   if (g_calq_mgdl <= 0)
      return; /* unsolicited / already resolved */
   if (result == 0) {
      LOGI("calibration %d mg/dL ACCEPTED by the sensor", g_calq_mgdl);
      calq_resolve(CAL_ST_APPLIED);
   } else {
      /* The sensor actively rejected the value -- resending it will not help,
       * so surface it (LAST CAL shows REJECTED) rather than looping or
       * dropping it silently. No beep: the official app is silent on a
       * rejection too. */
      LOGI("calibration %d mg/dL REJECTED by the sensor (result=0x%02x)",
           g_calq_mgdl, result);
      calq_resolve(CAL_ST_REJECTED);
   }
   calib_repaint();
}

/* ---- lifecycle --------------------------------------------------------- */

void calib_paths(const char *dir)
{
   (void)snprintf(g_calq_path, sizeof g_calq_path, "%s/cal.q", dir);
   (void)snprintf(g_rescale_path, sizeof g_rescale_path, "%s/rescale.cfg", dir);
}

/* Loading DEFINES the state, it does not merely add to it: what the file says
 * is the whole truth afterwards, including "nothing is queued". A load that
 * only ever filled things in would leave a caller's earlier queue standing
 * behind a file that had already resolved it. */
static void calq_load(void)
{
   g_calq_mgdl = g_calq_id = 0;
   g_calq_t = g_calq_sent = 0;
   g_lastcal_mgdl = g_lastcal_state = g_lastcal_id = 0;
   g_lastcal_t                                     = 0;
   char b[64];
   if (!read_line(g_calq_path, b, sizeof b))
      return;
   long v[7] = {0, 0, 0, 0, 0, 0, 0};
   parse_ints(b, v, 7);
   /* the last-resolved record (fields 4..7) survives regardless of the
    * queue. */
   g_lastcal_mgdl  = (int)v[3];
   g_lastcal_t     = v[4];
   g_lastcal_state = (int)v[5];
   g_lastcal_id    = (int)v[6];
   if (v[1] <= 0)
      return; /* no value queued */
   g_calq_id   = (int)v[0];
   g_calq_mgdl = (int)v[1];
   g_calq_t    = v[2];
   g_calq_sent = 0;
   /* A calibration confirmed before a restart: keep retrying if it is still
    * fresh, otherwise record the failure -- never drop it silently. */
   if (realtime_s() - g_calq_t > CALQ_WINDOW_S)
      calq_resolve(CAL_ST_FAILED);
}

static void rescale_load(void)
{
   g_rescale_pm = 1000;
   g_rescale_id = 0;
   g_rescale_t  = 0;
   rescale_pend_clear();
   /* The notices describe attempts made in this session; a load starts with
    * none, except one this load itself raises just below. */
   g_rescale_reject  = 0;
   g_rescale_expired = 0;
   char b[96];
   if (!read_line(g_rescale_path, b, sizeof b))
      return;
   long v[6] = {0, 0, 0, 0, 0, 0};
   parse_ints(b, v, 6);
   if (v[1] >= RESCALE_MIN_PM && v[1] <= RESCALE_MAX_PM && v[1] != 1000) {
      g_rescale_id = (int)v[0];
      g_rescale_pm = (int)v[1];
      g_rescale_t  = v[2];
   }
   if (v[4] > 0) { /* a target was awaiting a reading when we last ran */
      long pend_t = v[5];
      /* Restart must NOT lose a pending rescale -- BUT only honour it if its
       * request is still within the freshness window; a target set before a
       * long downtime has a stale fingerstick reference and must not silently
       * apply to a much-later reading. */
      if (pend_t > 0 && realtime_s() - pend_t <= RESCALE_PEND_WINDOW_S) {
         g_rescale_pend_id   = (int)v[3];
         g_rescale_pend_mgdl = (int)v[4];
         g_rescale_pend_t    = pend_t;
      } else {
         g_rescale_expired    = 1; /* surface it, never a silent drop */
         g_rescale_expired_id = (int)v[3];
      }
   }
}

void calib_load(void)
{
   calq_load();
   rescale_load();
}

void calib_tick(void)
{
   long now = realtime_s();
   if (g_calq_mgdl > 0) {
      if (g_calq_sent > 0 && now - g_calq_sent > 60)
         g_calq_sent = 0; /* no reply in a minute: allow another attempt */
      if (now - g_calq_t > CALQ_WINDOW_S) {
         LOGI("calibration %d mg/dL never accepted within %ld s; giving up "
              "VISIBLY",
              g_calq_mgdl, CALQ_WINDOW_S);
         /* No beep -- LAST CAL shows FAILED; the official app is silent
          * too. */
         calq_resolve(CAL_ST_FAILED);
         calib_repaint();
      }
   }
   /* A pending rescale target expires on the clock too, not only when a
    * reading finally arrives -- otherwise a sensor that goes quiet leaves the
    * target looking live indefinitely. */
   if (g_rescale_pend_mgdl > 0 &&
       now - g_rescale_pend_t > RESCALE_PEND_WINDOW_S) {
      LOGI("pending rescale %d mg/dL EXPIRED with no reading",
           g_rescale_pend_mgdl);
      g_rescale_expired    = 1;
      g_rescale_expired_id = g_rescale_pend_id;
      rescale_pend_clear();
      rescale_save();
      calib_repaint();
   }
}

void calib_view(int sensor_id, struct calib_view *out)
{
   out->queued_mgdl = calib_queued_for(sensor_id);
   /* The queued value and the last resolved one are ALTERNATIVES, not a pair:
    * while one is in flight the row shows it, and only once it resolves does
    * the LAST CAL record take the row back. Reporting both would put a live
    * "PENDING 120" beside the older "APPLIED 96" it is about to replace. */
   int resolved =
       (!out->queued_mgdl && g_lastcal_t > 0 && g_lastcal_id == sensor_id);
   out->last_mgdl  = resolved ? g_lastcal_mgdl : 0;
   out->last_state = resolved ? g_lastcal_state : 0;
   out->last_t     = resolved ? g_lastcal_t : 0;

   out->rescale_pm = (g_rescale_pm != 1000 && g_rescale_id == sensor_id)
                         ? g_rescale_pm
                         : 1000;
   out->rescale_pending =
       (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id)
           ? g_rescale_pend_mgdl
           : 0;
   out->rescale_rejected =
       (g_rescale_reject && g_rescale_reject_id == sensor_id);
   out->rescale_expired =
       (g_rescale_expired && g_rescale_expired_id == sensor_id);
}
