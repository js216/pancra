// SPDX-License-Identifier: GPL-3.0
// calibtest.c --- the calibration queue and the rescale factor
// Copyright 2026 Jakob Kastelic

/* This code decides whether the number on the screen is the number the sensor
 * meant, and it used to be twenty globals inside main.c where nothing could
 * reach it. Extracting it into calib.c is what makes this file possible, so
 * this file is the point of that extraction.
 *
 * What it pins down is the behaviour that is easy to lose in a refactor and
 * expensive to lose in the field: a correction is NEVER applied from a stale
 * reference, an implausible one is REJECTED rather than quietly clamped, a
 * backfilled point older than the correction keeps its raw value, and a
 * confirmed calibration is never dropped without saying so.
 *
 * The driver is stubbed here: what the sensor answers is exactly the variable
 * these paths turn on, so the test sets it directly rather than owning a
 * radio.
 */
#include "calib.h"
#include "dexdriver.h"
#include "pancra.h" /* pancra_cal_result: the sensor's answer */
#include "ui.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

static void ck(int cond, const char *what)
{
   if (!cond) {
      printf("  FAIL: %s\n", what);
      g_fail = 1;
   }
}

/* ---- the stubbed sensor ------------------------------------------------ */

static struct dex_cal g_cal;
static int g_sent;      /* last value handed to driver_calibrate */
static int g_nsent;     /* how many times it was called */
static int g_nprobe;    /* how many 0x32 probes went out */
static int g_refuse;    /* make driver_calibrate refuse (not streaming) */
static int g_repainted; /* the module asked for a repaint */

void driver_get_cal(struct dex_cal *out)
{
   *out = g_cal;
}

void driver_cal_bounds(void)
{
   g_nprobe++;
   g_cal.asked = realtime_s();
}

int driver_calibrate(int mg_dl)
{
   g_nsent++;
   if (g_refuse)
      return 0;
   g_sent = mg_dl;
   return 1;
}

void calib_repaint(void)
{
   g_repainted++;
}

/* ---- fixtures ---------------------------------------------------------- */

static char g_dir[64] = "build/app/test/calibdata";

/* Hand-write a persisted state, so a RESTART can be tested without one. */
static void put(const char *name, const char *text);

/* Back to a known state, through the public interface only: write the two
 * files as a fresh install would leave them and load them. This works because
 * calib_load DEFINES the state rather than adding to it -- which is itself
 * worth leaning on here, since every case below depends on it. */
static void reset(void)
{
   put("cal.q", "0,0,0,0,0,0,0\n");
   put("rescale.cfg", "0,1000,0,0,0,0\n");
   calib_load();
}

static void put(const char *name, const char *text)
{
   char p[128];
   (void)snprintf(p, sizeof p, "%s/%s", g_dir, name);
   FILE *f = fopen(p, "w");
   if (!f) {
      printf("  FAIL: cannot write %s\n", p);
      g_fail = 1;
      return;
   }
   fputs(text, f);
   fclose(f);
}

int main(void)
{
   long now = realtime_s();
   calib_paths(g_dir);

   /* ---- the factor itself ---- */
   ck(calib_rescale_preview(120, 100) == 1200, "120 over raw 100 is +20%");
   ck(calib_rescale_preview(100, 100) == 1000, "a matching value is no change");
   ck(calib_rescale_preview(120, 0) == 0,
      "no reading to divide by yields the 'not yet' sentinel, not a factor");

   /* ---- an implausible correction is REJECTED, not clamped ---- */
   reset();
   calib_rescale_set(7, 100, 200); /* +100%: far outside +-25% */
   ck(calib_rescale_pm() == 1000, "a >25%% correction does NOT become active");
   struct calib_view v;
   calib_view(7, &v);
   ck(v.rescale_rejected, "...and the rejection is surfaced, not swallowed");
   ck(v.rescale_pm == 1000, "...leaving no factor behind");

   /* Clamping instead of rejecting would silently apply 125% of a reading the
    * user just said was 100% wrong -- the case this asserts against. */
   int adj = calib_on_reading(7, 0, now, 100, NULL, NULL);
   ck(adj == 100, "a rejected correction leaves readings untouched");

   /* ---- THE BAND ITSELF, at its edges ---- */
   /* Tested AT the boundary, not merely well inside it: "+100% is rejected
    * and +10% is accepted" holds for any band between them, so it pins down
    * nothing about where the limit actually is. */
   reset();
   calib_rescale_set(7, 100, 125); /* exactly +25%: the last allowed */
   ck(calib_rescale_pm() == RESCALE_MAX_PM, "the +25% edge is allowed");
   reset();
   calib_rescale_set(7, 100, 126); /* one step past it */
   ck(calib_rescale_pm() == 1000, "...and one step past it is refused");
   reset();
   calib_rescale_set(7, 100, 75); /* exactly -25% */
   ck(calib_rescale_pm() == RESCALE_MIN_PM, "the -25% edge is allowed");
   reset();
   calib_rescale_set(7, 100, 74);
   ck(calib_rescale_pm() == 1000, "...and one step below it is refused");

   /* ---- a plausible one applies, to the right sensor only ---- */
   reset();
   calib_rescale_set(7, 100, 110); /* +10% */
   ck(calib_rescale_pm() == 1100, "110 over raw 100 activates as +10%");
   int pm = 0;
   adj    = calib_on_reading(7, 0, now + 10, 100, &pm, NULL);
   ck(adj == 110 && pm == 1100, "the sensor it was set for is rescaled");
   adj = calib_on_reading(8, 1, now + 10, 100, &pm, NULL);
   ck(adj == 100 && pm == 1000, "a DIFFERENT sensor is not");

   /* ---- the timestamp gate: history predating the correction is raw ---- */
   adj = calib_on_backfill(7, now - 3600, 100, &pm);
   ck(adj == 100 && pm == 1000,
      "a backfilled point OLDER than the activation keeps its raw value");
   adj = calib_on_backfill(7, now + 3600, 100, &pm);
   ck(adj == 110 && pm == 1100, "...and a newer one is rescaled");

   /* ---- persistence: a factor survives a restart ---- */
   calib_rescale_stop();
   ck(calib_rescale_pm() == 1000, "STOP turns it off");
   calib_rescale_set(7, 100, 110);
   calib_load(); /* what a restart does */
   ck(calib_rescale_pm() == 1100, "an active factor survives a restart");

   /* ---- a target with no reading yet is HELD, not lost ---- */
   reset();
   calib_rescale_set(7, 0, 130); /* raw 0: nothing to compute against */
   ck(calib_rescale_pm() == 1000, "with no reading there is no factor yet");
   calib_view(7, &v);
   ck(v.rescale_pending == 130, "...the target is held, visibly");
   ck(calib_rescale_engaged(7), "...and the sensor counts as engaged");
   /* 130 over 100 is +30%, outside the band, so the held path must REJECT it
    * exactly as the direct one does -- a target does not become acceptable by
    * having waited. */
   adj = calib_on_reading(7, 0, now, 100, &pm, NULL);
   ck(calib_rescale_pm() == 1000, "a held target obeys the same +-25% bound");
   ck(adj == 100 && pm == 1000, "...so the reading it resolved on is raw");
   calib_view(7, &v);
   ck(v.rescale_pending == 0,
      "...and the target is consumed, not left hanging");
   ck(v.rescale_rejected, "...with the rejection surfaced");

   reset();
   calib_rescale_set(7, 0, 110);
   int started = 0;
   adj         = calib_on_reading(7, 0, now, 100, &pm, &started);
   ck(calib_rescale_pm() == 1100, "a plausible held target activates");
   ck(started, "...and says so, so the chirp is not a phantom swing");
   ck(adj == 110, "...effective on the very reading that resolved it");

   /* ---- AND IT BELONGS TO ONE SENSOR ----
    *
    * Every case in this file used sensor id 7, so `g_rescale_id == src` could
    * be relaxed to `>=` and nothing failed: one sensor's correction would
    * silently rescale another's readings, which with two sensors on the arm
    * is a wrong number on the screen and in the log. The factor above is
    * live for id 7; id 8 must be untouched by it. */
   {
      int pm8  = 0;
      int adj8 = calib_on_reading(8, 1, now, 100, &pm8, NULL);
      ck(pm8 == 1000, "another sensor's reading is not rescaled");
      ck(adj8 == 100, "...and comes through at its raw value");
      int pm7 = 0;
      ck(calib_on_reading(7, 0, now, 100, &pm7, NULL) == 110 && pm7 == 1100,
         "...while the sensor it belongs to still is");
   }

   /* Rounding is half-up at the boundary, and it is applied to the value the
    * user reads: 1005 pm of 100 is 100.5, which must not truncate to 100. */
   {
      reset();
      calib_rescale_set(7, 200, 201); /* 1005 pm */
      int pmr = 0;
      calib_on_reading(7, 0, now, 200, &pmr, NULL);
      ck(pmr == 1005, "a 0.5% correction is representable");
      int r = calib_on_reading(7, 0, now + 300, 100, &pmr, NULL);
      ck(r == 101, "...and rounds half-up rather than truncating");
   }

   /* ---- a STALE reference expires instead of applying ---- */
   reset();
   calib_rescale_set(7, 0, 110);
   started = 0;
   adj     = calib_on_reading(7, 0, now + RESCALE_PEND_WINDOW_S + 1, 100, &pm,
                              &started);
   ck(calib_rescale_pm() == 1000,
      "a reading past the window does NOT apply the stale target");
   ck(!started && adj == 100, "...and the reading itself is untouched");
   calib_view(7, &v);
   ck(v.rescale_expired, "...the expiry is surfaced, never a silent drop");

   /* A pending target written before a long downtime must not spring to life
    * on restart either. Fields: id,pm,t,pend_id,pend_mgdl,pend_t */
   reset();
   char line[128];
   (void)snprintf(line, sizeof line, "0,1000,0,7,110,%ld\n",
                  now - RESCALE_PEND_WINDOW_S - 60);
   put("rescale.cfg", line);
   calib_load();
   ck(calib_rescale_pm() == 1000, "a stale pending target does not survive");
   calib_view(7, &v);
   ck(v.rescale_pending == 0 && v.rescale_expired,
      "...it is reported EXPIRED rather than resumed");

   /* ---- the calibration queue ---- */
   reset();
   calib_load();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.result = -1;
   calib_queue(7, 120);
   ck(calib_queued_for(7) == 120, "a confirmed calibration is queued");
   ck(calib_queued_for(8) == 0, "...for that sensor only");
   calib_view(7, &v);
   ck(v.queued_mgdl == 120, "...and shown as pending");

   /* Permission unknown: PROBE, do not write. */
   g_nsent = g_nprobe = 0;
   calib_try_locked(7);
   ck(g_nsent == 0, "nothing is written before the sensor grants permission");
   ck(g_nprobe == 1, "...a permission probe goes out instead");
   calib_try_locked(7);
   ck(g_nprobe == 1, "...and is throttled, never hammered");

   /* Permitted: write it. */
   g_cal.have = g_cal.permitted = 1;
   calib_try_locked(7);
   ck(g_nsent == 1 && g_sent == 120, "once permitted, the value is written");
   calib_try_locked(7);
   ck(g_nsent == 1, "...and not resent while a reply is still awaited");
   ck(calib_queued_for(7) == 120,
      "...it stays queued until the sensor answers");

   /* The sensor accepts. */
   g_repainted = 0;
   pancra_cal_result(0);
   ck(calib_queued_for(7) == 0, "an accepted calibration leaves the queue");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 120,
      "...and is recorded as APPLIED");
   ck(v.queued_mgdl == 0, "...with nothing still pending");
   ck(g_repainted > 0, "...and the screen is told to update");

   /* The sensor rejects: surfaced, not retried forever. */
   reset();
   calib_queue(7, 300);
   g_nsent = 0;
   calib_try_locked(7);
   pancra_cal_result(3);
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_REJECTED, "a rejected value is recorded as such");
   ck(calib_queued_for(7) == 0, "...and dropped rather than resent");

   /* A sensor that permits nothing fails FAST and VISIBLY. */
   reset();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.have      = 1;
   g_cal.permitted = 0;
   calib_queue(7, 120);
   g_nsent = 0;
   calib_try_locked(7);
   ck(g_nsent == 0, "a factory-calibrated sensor is never written to");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_NOTSUP, "...and says NOT SUPPORTED at once");
   ck(calib_queued_for(7) == 0, "...without waiting out the whole window");

   /* ---- a queue that outlived its window ---- */
   /* Fields: id,mgdl,t,last_mgdl,last_t,last_state,last_id */
   reset();
   (void)snprintf(line, sizeof line, "7,120,%ld,0,0,0,0\n",
                  now - CALQ_WINDOW_S - 60);
   put("cal.q", line);
   calib_load();
   ck(calib_queued_for(7) == 0, "a calibration older than the window is over");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_FAILED,
      "...and reported FAILED, never silently forgotten");

   /* One still inside its window resumes and keeps trying. */
   reset();
   (void)snprintf(line, sizeof line, "7,120,%ld,0,0,0,0\n", now - 60);
   put("cal.q", line);
   calib_load();
   ck(calib_queued_for(7) == 120,
      "a fresh calibration survives a restart and stays queued");

   /* ---- the row shows one thing at a time ---- */
   reset();
   (void)snprintf(line, sizeof line, "0,0,0,96,%ld,%d,7\n", now - 300,
                  CAL_ST_APPLIED);
   put("cal.q", line);
   calib_load();
   calib_view(7, &v);
   ck(v.last_mgdl == 96 && v.last_state == CAL_ST_APPLIED,
      "with nothing queued the row shows the last result");
   calib_queue(7, 130);
   calib_view(7, &v);
   ck(v.queued_mgdl == 130, "once a new one is queued the row shows that");
   ck(v.last_mgdl == 0, "...and NOT the superseded result beside it");

   reset();
   if (g_fail) {
      printf("calibtest: FAIL\n");
      return 1;
   }
   printf("calibtest: calibration queue and rescale factor OK\n");
   return 0;
}
