// SPDX-License-Identifier: GPL-3.0
// reading.c --- A decoded reading becomes a stored datapoint (see reading.h)
// Copyright 2026 Jakob Kastelic

#include "reading.h"
#include "alarm.h"
#include "alarmlogic.h"
#include "bletrans.h" /* the beep and the chirp a new sample makes */
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "ingest.h"
#include "linkhealth.h" /* the repairs a reading triggers, not the record */
#include "linkinfo.h"   /* whose reading this is, and what the link reports */
#include "log.h"
#include "notify.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "stats.h"  /* TIR_LOW_MGDL/TIR_HIGH_MGDL: the clinical band */
#include "status.h" /* set_status: a reading that did NOT persist says so */
#include "store.h"
#include "tzoff.h"

/* Is this a usable glucose value?
 *
 * The 12-bit field carries 0..4095 verbatim, and a sentinel (0 during
 * warm-up or a sensor-error state) would otherwise become g_cur_glu -- the
 * headline number, with a trend arrow, feeding the alarm and firing a
 * spurious LOW. It would also be written to the permanent log and into the
 * stats. The meter path already bounds its values; this is the equivalent
 * for the sensor.
 *
 * The window itself is INGEST_GLU_MIN..INGEST_GLU_MAX in app/ingest.h, which
 * derives it from the sensor's own CAL_MIN_MGDL..CAL_MAX_MGDL range. It lives
 * there because it is reachable by a test there and was not here. */
static int glucose_plausible(int mg_dl)
{
   return mg_dl >= INGEST_GLU_MIN && mg_dl <= INGEST_GLU_MAX;
}

int pancra_glucose(int link, int mg_dl, int trend, int age_s)
{
   struct prefs sp;
   settings_get(&sp);
   shell_where("pancra_glucose");
   /* THE GATE IS app/ingest.c, so a test can reach it.
    *
    * Both of these checks lived here, where nothing could: a review widened
    * the value window to 0..100000 and the age bound to 65535 and the whole
    * gate stayed green through both -- the second REINTRODUCING, verbatim,
    * the 18-hour backdating bug whose fix ingest.h now describes. That is the
    * argument that put alarmlogic.c in its own file. */
   struct ingest_out ing = ingest_decide(mg_dl, age_s, realtime_s());
   if (ing.verdict == INGEST_IMPLAUSIBLE) {
      LOGI("glucose %d mg/dL implausible, ignored", mg_dl);
      return 0;
   }
   if (ing.verdict != INGEST_OK) {
      LOGI("glucose age %d s implausible, ignored", age_s);
      return 0;
   }
   long t = ing.t;
   /* MONOTONIC, and per link. `t` above is the reading's own timestamp and
    * stays realtime -- it is written to readings.csv and compared against
    * every other device's rows. Whether the signal strength beside it comes
    * from THIS connection is an elapsed-time question, and on the wall clock
    * a correction answers it wrong in both directions. */
   int has = driver_rssi_fresh(link);
   /* Mutate the shared history / current-reading state under the same guard
    * the renderer holds (hist_history_lock), so a main-thread draw never
    * reads a half-shifted g_hist, torn stats, or a mismatched
    * g_cur_glu/g_cur_time. */
   /* Resolve the source BEFORE taking hist_lock: this reads the driver and
    * the registry, and the established lock order is driver -> reg -> hist.
    */
   int src = src_for_link(link);
   if (src < 0) {
      /* No slot claims this link's address yet -- registration happens on
       * the next 1 Hz reconcile, and requires have_reading, so the FIRST
       * reading of any new sensor always lands here.
       *
       * THE ONLY CASE WITH NO PROVENANCE TO WAIT FOR is an EMPTY registry: a
       * fresh install, where 0 is the pre-registry legacy id and names
       * nothing else. The threshold is >= 1, not > 1: with one sensor
       * already registered, a second sensor's first reading was written to
       * the append-only log carrying the FIRST sensor's id -- and if that
       * sensor had reported within 150 s, hist_insert deduped it away
       * entirely. Defer instead. */
      if (sensor_live_cgm_count() >= 1) {
         LOGI("glucose %d mg/dL from an unregistered link, deferred", mg_dl);
         return 0;
      }
      src = 0; /* pre-registry legacy: nothing else can be meant */
   }
   /* RESCALE. Record the RAW value (for computing a future factor), then
    * apply the active correction IF it belongs to this sensor and this
    * reading's timestamp is at/after activation. From here mg_dl is the
    * rescaled value, so history, stats, the alarm and the log all use it;
    * the factor rpm is written to the log's `rescale` column so the raw
    * stays recoverable. */
   /* `rescale_started` is set when a rescale takes effect ON this reading:
    * from here on mg_dl is rescaled while the history is not, so any change
    * computed against the history is the calibration step, not the wearer's
    * glucose moving. The CHIRP below is the only consumer. */
   int rescale_started = 0;
   int rpm             = 1000;
   mg_dl     = calib_on_reading(src, link, t, mg_dl, &rpm, &rescale_started);
   int prime = sensor_primary_id();
   /* WARMUP readings are shown but never COUNTED -- uncalibrated values skew
    * TIR and the average (see sensors.h). Resolved HERE, before hist_lock,
    * because the lock order is registry -> history.
    *
    * THE SENSOR IS ASKED, NOT ONLY THE REGISTRY. The 0x4e response
    * this reading came out of carries the sensor's own state byte, and it is
    * the only direct evidence there is: the activation the registry rule
    * compares against is learned from the session clock and is 0 for a sensor
    * paired mid-session, which makes that rule count an uncalibrated first
    * hour. The measurement travels with the row, so a replay of this log
    * reaches the same answer without inferring anything. */
   struct dex_session ws;
   enum warm_state measured =
       driver_session_of(link, &ws) ? warm_of_state(ws.state) : WARM_UNKNOWN;
   enum warm_verdict wv = warm_decide(measured, src, t);
   int warm             = wv == WARM_SKIP;
   /* ONE OPERATION, not six steps in the right order. store_record owns the
    * lock, the insert, the statistics, the current value and the disk -- see
    * store.h for what each of those orderings costs when a caller gets it
    * wrong, which is why they are not a caller's business. */
   struct reading_event ev  = {.t           = t,
                               .glu         = mg_dl,
                               .trend       = trend,
                               .src         = src,
                               .kind        = KIND_CGM,
                               .rssi        = linkinfo_conn_rssi(),
                               .has_rssi    = has,
                               .raw         = t,
                               .tz          = tz_off_now(),
                               .rescale_pm  = rpm,
                               .warm        = warm,
                               .wstate      = measured,
                               .warm_unsure = wv == WARM_COUNT_UNSURE,
                               .prime       = prime};
   struct reading_result rr = store_record(&ev, CHIRP_MAX_GAP_S);
   int prev_glu             = rr.prev_glu;
   /* WHAT HAPPENED TO IT, kept as its own type. Spelled `int isnew =
    * rr.inserted;` and used as a truth value, it reads as "new" and answers
    * "not a duplicate" -- so a BACKFILLED reading older than the display
    * window takes every path meant for a sample that just arrived. Each use
    * below asks the question it actually means:
    * hist_is_tail for "this is the newest thing on the screen", hist_kept
    * for "this reached the record". */
   enum hist_insert_result got = rr.inserted;
   /* NEWEST-SAMPLE WORK, and only for the newest: a backfilled reading older
    * than the display window reaches the record but is not what the screen is
    * showing, so it must not be treated as a sample that just arrived. */
   if (hist_is_tail(got)) {
      /* THE RECORD IN-RANGE RUN, raised here and nowhere else.
       *
       * HERE because this is the only moment a streak can grow, and because
       * it is outside the history lock (see the paragraph above) -- the
       * renderer recomputes the same streak every frame, but it does so while
       * draw() holds that lock, and persisting from there would put a file
       * write underneath it. settings_set_best_streak compares before it
       * writes, so an ordinary streak costs a comparison and no I/O.
       *
       * The band is the CLINICAL one and the gap rule is alarm_streak_s's,
       * which is what makes this number the same measurement the plot shows
       * rather than a second opinion about it. */
      static struct reading snap[NHIST];
      int hn = hist_copy(snap, NHIST);
      long run =
          alarm_streak_s(snap, hn, TIR_LOW_MGDL, TIR_HIGH_MGDL, 20L * 60L);
      if (run > 0 && run <= BEST_STREAK_MAX)
         (void)settings_set_best_streak((int)run);
   }
   if (hist_kept(got) && has) {
      store_note_rssi(linkinfo_conn_rssi()); /* kept like glu/trend */
   }
   /* Capture THIS CGM link's predicted value + time for the imminent-hypo
    * (predicted-low) alarm. The session is read for THIS LINK by name,
    * rather than from whichever context the callback last selected -- the
    * same thing only for as long as that stays true. Stamped
    * every reading (not just new ones) so the freshness gate in
    * any_pred_low() tracks the live prediction. */
   if (link >= 0 && link < LINK_MAX && !driver_link_is_meter(link)) {
      struct dex_session ps;
      /* in range, tested just above. A refusal would zero ps, and stamping a
       * prediction of zero as fresh is worse than not stamping at all -- the
       * predicted-low alarm reads exactly this. So the answer is read, and a
       * refusal simply leaves the previous stamp standing. */
      /* mono_s(), not realtime_s(): this stamp is only ever used to ask "did
       * this prediction arrive recently enough to still mean something",
       * which is an interval. It is never persisted and never displayed, so
       * nothing wants the wall clock -- and on the wall clock a backward
       * correction makes every recorded prediction permanently fresh, wedging
       * the one alarm the user cannot silence. See alarm_pred_low. */
      if (driver_session_of(link, &ps))
         alarm_note_pred(link, ps.predicted, mono_s());
   }
   /* A reading just PROVED this sensor is streaming -- the ideal moment to
    * flush any queued calibration for it. driver_lock is held (jni_notify)
    * and calib_try names the link and takes the (recursive) lock itself.
    */
   calib_try(link, src);
   /* Persist on HIST_OLD as well: the log is the lifetime record and NHIST
    * is only how much of it fits on screen. File I/O outside the lock -- it
    * touches no draw-shared state. */
   /* SAY SO IF IT DID NOT PERSIST. The reading is already in g_hist, so it is
    * on screen and the alarm has seen it -- and after a restart it will be
    * gone. Silence there means the user's record quietly loses samples with no
    * sign until they go looking, which is why the insulin path refuses out
    * loud too.
    *
    * No per-datapoint push any more: pancra_remote_sync() on the tick delivers
    * this point (and anything a previous outage lost) from the server's own
    * cursor. */
   if (hist_kept(got) && !rr.persisted)
      set_status("READING: WRITE FAILED");
   LOGI("glucose %d mg/dL trend %d age %d", mg_dl, trend, age_s);

   /* NEW DATAPOINT alert: a genuinely new sample from the PRIMARY CGM only,
    * so a secondary sensor or a backfilled/duplicate reading stays silent.
    *
    * CHIRP pitches on the change since THIS sensor's own previous sample.
    * Never against another CGM's: with two sensors worn at once their
    * difference is the calibration offset between two devices, and pitching
    * on it would announce swings the wearer never had. With no previous
    * sample of its own (a fresh session) the delta is 0, which is exactly
    * the BEEP tone -- the honest sound for "no change known yet". */
   /* THE NEWEST SAMPLE ON THE SCREEN, not merely one that was kept: a
    * backfilled point from last Tuesday must not beep. */
   if (hist_is_tail(got) && src == prime) {
      /* THE STORED INT BECOMES A MODE HERE, once, through the same
       * conversion the loader uses. */
      enum nudge_mode nd = nudge_mode_of(sp.newdata_mode);
      if (nd == ND_BEEP) {
         dexble_beep();
      } else if (nd == ND_CHIRP) {
         /* A rescale that ACTIVATED on this very reading makes the
          * comparison meaningless: mg_dl is already rescaled while every
          * sample already in g_hist is not, so the "change" would be the
          * calibration offset itself -- entering a 120 fingerstick against a
          * sensor reading 100 would chirp a full-cap rocket that never
          * happened. Treat it as having nothing to compare against, which
          * is the plain BEEP pitch. */
         int delta = (prev_glu >= 0 && !rescale_started) ? mg_dl - prev_glu : 0;
         dexble_chirp(chirp_semitone10(delta));
      }
   }

   /* Alarm evaluation is deliberately NOT done here.
    *
    * This runs on a BLE binder thread with driver_lock held (jni_notify
    * holds it across the whole notify dispatch), and raising an alarm calls
    * Alarm.trigger -> RingtoneManager + MediaPlayer.setDataSource/prepare/
    * start: media-server IPC, routinely hundreds of milliseconds. Holding a
    * SPIN lock across that makes the main looper burn a core in sched_yield
    * on its next snap_drivers() or watchdog tick.
    *
    * alarm_disc_reeval() on the 1 Hz main-thread timer already recomputes the
    * zone from a consistent current_reading() and calls alarm_apply, so the
    * alarm is raised within one second regardless -- immaterial against a
    * 5-minute sample interval. Leaving alarm_apply to a single thread also
    * removes the raise/silence ordering races entirely: every caller is now the
    * main thread. */
   /* Rendering is deferred to the main-thread 1 Hz timer (see on_main); just
    * mark the screen and notification dirty. */
   shell_ui_dirty();
   notify_mark();

   /* THE GATT METADATA AND THE HISTORY REPAIR ARE NOT INGESTION.
    *
    * Written out here, inline, after the reading is stored, they are forty
    * lines re-requesting the device-information strings and ninety more
    * scanning this sensor's history for a hole to ask the sensor to refill.
    * Neither decides whether THIS number is kept, which is what this function
    * is for -- and a file that both admits a permanent record
    * and dials the radio is one where a change to the second is reviewed as
    * if it were a change to the first.
    *
    * They keep their trigger, which is genuinely here: a reading is the proof
    * that this link is up and answering, so it is the only good moment to ask
    * it for anything. What moved is the deciding and the state each needs --
    * the per-link throttles, the window sizing, the hole search. */
   linkinfo_refresh_dis(link, sp.mfr[0] != 0);
   linkhealth_after_reading(link, src, t, hist_kept(got));

   /* THE ANSWER THE DRIVER WAITS FOR: a record entered the history.
    *
    * HIST_NEW or HIST_OLD -- both put a row in the log -- and NOT HIST_DUP,
    * which is store_record's way of saying this sample was already there and
    * nothing was added. Every `return 0` above is a refusal that leaves the
    * screen untouched; a duplicate does too. See reading.h. */
   return hist_kept(got);
}

/* an older reading recovered via backfill: store it, place it in history,
 * but don't disturb the current value unless it turns out to be the newest
 */
int pancra_backfill(int link, int mg_dl, int trend, int age_s)
{
   if (!glucose_plausible(mg_dl)) {
      LOGI("backfill %d mg/dL implausible, ignored", mg_dl);
      return 0;
   }
   /* AND BOUND THE AGE, which this path never did.
    *
    * The live path rejects a frame older than INGEST_AGE_MAX; backfill is
    * historical by definition, so that bound is the wrong one -- but "any
    * uint16 the wire happens to carry" is not a bound at all. A backfill
    * request covers at most 24 hours (see the gap sizing that issues it), so
    * a point claiming to be older than that did not come from the request we
    * made. Negative is malformed either way. */
   if (age_s < 0 || age_s > 24 * 3600) {
      LOGI("backfill age %d s outside the requested window, ignored", age_s);
      return 0;
   }
   long t = realtime_s() - age_s;
   /* Same per-link attribution as the live path (see src_for_link): a
    * backfill arrives on the link of the sensor that buffered it. */
   int src = src_for_link(link);
   if (src < 0) {
      if (sensor_live_cgm_count() >= 1) {
         LOGI("backfill %d mg/dL from an unregistered link, deferred", mg_dl);
         return 0;
      }
      src = 0; /* an EMPTY registry only: see the live path */
   }
   /* RESCALE, timestamp-gated: a backfilled point is only rescaled if ITS
    * OWN timestamp is at/after activation. An older buffered point (t <
    * activation) predates the correction and keeps its raw value -- this is
    * the backfill care the feature calls for. calib_on_backfill records no
    * raw: a factor is computed from the LIVE reading, never a historical
    * one. */
   int rpm   = 1000;
   mg_dl     = calib_on_backfill(src, t, mg_dl, &rpm);
   int prime = sensor_primary_id();
   /* Backfill reaches back across the warmup hour on a fresh sensor, so it
    * needs the same gate as the live path -- and before hist_lock, per the
    * registry -> history lock order (sensors.h).
    *
    * THE STATE BYTE IS NOT EVIDENCE ABOUT A BACKFILLED POINT. It
    * describes the sensor NOW; this reading came out of the sensor's memory
    * and may be twenty hours old, so a session that is streaming today says
    * nothing about whether that point was taken during its warm-up hour. The
    * row records no measurement, and the inference decides -- which is the
    * case the coverage figures mark as unsure. */
   enum warm_verdict wv = warm_decide(WARM_UNKNOWN, src, t);
   int warm             = wv == WARM_SKIP;
   /* Backfill: no RSSI. A failure here loses a point the sensor already held,
    * so it is worth the same visible refusal. Delivered by pancra_remote_sync()
    * on the tick. */
   struct reading_event bev    = {.t           = t,
                                  .glu         = mg_dl,
                                  .trend       = trend,
                                  .src         = src,
                                  .kind        = KIND_CGM,
                                  .raw         = t,
                                  .tz          = tz_off_now(),
                                  .rescale_pm  = rpm,
                                  .warm        = warm,
                                  .wstate      = WARM_UNKNOWN,
                                  .warm_unsure = wv == WARM_COUNT_UNSURE,
                                  .prime       = prime};
   struct reading_result brr   = store_record(&bev, CHIRP_MAX_GAP_S);
   enum hist_insert_result got = brr.inserted;
   if (hist_kept(got) && !brr.persisted)
      set_status("BACKFILL: WRITE FAILED");
   LOGI("backfill reading %d mg/dL age %d -> t=%ld", mg_dl, age_s, t);
   /* A gap recovered by backfill can be the newest reading (a missed live
    * cycle); re-evaluate the alarm and refresh the notification rather than
    * waiting for the next live reading. Rendering is on the 1 Hz timer. */
   /* Alarm left to the 1 Hz main-thread path -- see pancra_glucose. */
   shell_ui_dirty();
   notify_mark();
   /* See reading.h and the same return at the end of pancra_glucose: a
    * backfill record the history already held has recovered nothing, and a
    * batch of nothing-but-duplicates must not count as a streaming sensor. */
   return hist_kept(got);
}
