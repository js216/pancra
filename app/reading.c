// SPDX-License-Identifier: GPL-3.0
// reading.c --- A decoded reading becomes a stored datapoint (see reading.h)
// Copyright 2026 Jakob Kastelic

#include "reading.h"
#include "alarm.h"
#include "alarmlogic.h"
#include "bletrans.h"
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "ingest.h"
#include "insulin.h"
#include "log.h"
#include "meter.h"
#include "notify.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "stats.h" /* stat_reload: a restore rewrote the readings log */
#include "status.h"
#include "store.h"
#include "thread.h" /* mutex: the per-link DIS strings have their own */
#include "tzoff.h"
#include "util.h"
#include "weight.h"
#include <stdatomic.h>
#include <string.h>

/* Per LINK, not per process: a single flag was latched by whichever CGM
 * reported first, so a second sensor never requested its once-per-launch
 * backward fill and its pre-launch history was never recovered, on every
 * launch. */
static int g_startup_bf[LINK_MAX];
/* Per-link throttle for the interior-gap backfill scan below: it re-requests
 * until the hole is filled, so it must not fire on every 5-minute reading. */
static long g_gap_bf_at[LINK_MAX];
static int g_conn_rssi; /* live connection RSSI (readRemoteRssi) */
/* THE TWO DEADLINES THAT USED TO LIVE HERE ARE IN THE DRIVER NOW.
 *
 * A wall-clock stamp for the RSSI freshness window, and a per-link array of
 * wall-clock stamps for the device-information re-read throttle. Both were
 * `realtime_s() - stamp > interval`, and a backward NTP correction makes that
 * difference NEGATIVE -- so for the whole hour it takes wall time to catch up
 * the DIS strings were never re-requested (a sensor minted with no model or
 * firmware stays that way) and every stale signal reading counted as "this
 * connection" and was stamped onto stored rows. See the liveness block in
 * scanlogic.h.
 *
 * They are driver_dis_claim / driver_rssi_note+driver_rssi_fresh now: per
 * link, monotonic, and -- for the DIS one -- a single-winner atomic claim
 * rather than a test followed by a store. */

/* Per-CGM-link DIS strings. The process-global model/firmware (settings.c) are
 * process-global and shared by every link, which is fine for the headline
 * display but WRONG for provenance -- see pancra_devinfo. Minting uses these.
 */
static char g_model_l[LINK_MAX][24], g_fw_l[LINK_MAX][24];

/* PER-LINK SIGNAL STRENGTH, retained as "last known" so it never expires
 * into "--" while readings lag. Kept HERE, where the measurement arrives
 * (pancra_rssi), and read by the frame -- it lived in model.c, so this file
 * had to include the frame builder to report a number it had just been
 * handed, and the frame builder was then part of every workflow cycle. */
static int g_link_rssi[LINK_MAX], g_link_rssi_ok[LINK_MAX];
static long g_link_rssi_t[LINK_MAX];

void reading_note_rssi(int link, int dbm, long when)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   g_link_rssi[link]    = dbm;
   g_link_rssi_ok[link] = 1;
   g_link_rssi_t[link]  = when;
}

int reading_link_rssi(int link, int *dbm, long *when)
{
   *dbm  = 0;
   *when = 0;
   if (link < 0 || link >= LINK_MAX || !g_link_rssi_ok[link])
      return 0;
   *dbm  = g_link_rssi[link];
   *when = g_link_rssi_t[link];
   return 1;
}
/* AND THEIR OWN LOCK. These strings were written and read under the REGISTRY
 * lock -- a lock belonging to another module, protecting a different thing,
 * taken here only because it happened to be public. That is how the registry
 * lock ended up held across driver calls elsewhere. They are written by a
 * binder thread byte-by-byte (devinfo_copy) and read by the mint path, so
 * they do need one; it is a leaf, taken by nothing else and holding nothing
 * else while taken. */
static struct mutex dis_lk = MUTEX_INIT;

/* Registry id of the sensor the Dexcom driver is currently bonded to, stamped
 * onto every reading it produces. 0 means "not yet identified", which is the
 * same id legacy pre-registry rows carry, so old and new data stay consistent.
 */
static int g_cur_src;

/* The sensor id for the link a reading actually arrived on.
 *
 * A reading MUST be stamped with the sensor that produced it, not with a
 * global "current" id. The link is carried as an argument the whole way down
 * -- the driver has no ambient "current link" any more -- so while drv_glucose
 * runs, `link` IS the originating link, and that is the only trustworthy
 * attribution available at this depth. Stamping a single global instead meant
 * two CGMs shared one id, and per-source dedup (150 s window) then silently
 * DISCARDED whichever sensor's sample landed second: roughly half of one
 * sensor's data, never written to the log and never plotted.
 *
 * Returns -1 when the link maps to no registered slot, so the caller can
 * refuse to log rather than invent a provenance. */

int src_for_link(int link)
{
   /* Match on the session ADDRESS, and walk g_slot under the registry lock.
    *
    * The address is the only identity that cannot be shifted out from under a
    * live connection: sensor_forget() renumbers g_slot while the remaining
    * sensors keep streaming, so anything keyed on a slot's POSITION silently
    * re-points at a different sensor. With two CGMs that stamped one sensor's
    * id onto the other's readings -- in an append-only log that is never
    * rewritten, so the mistake is permanent. The lock matters for the same
    * reason: the main thread can be mid-shift while this runs on a BLE
    * thread. */
   /* THE SESSION IS READ AS ONE SNAPSHOT, which driver_session_of does under
    * the driver's own lock.
    *
    * This paragraph used to argue that the caller must hold driver_lock,
    * because the driver selected a link into two file-statics that every
    * function dereferenced, and a call from an unlocked thread could stomp
    * them out from under a binder thread mid-dispatch -- attributing a
    * reading to the other sensor, or writing one sensor's key over another's.
    * Those statics are gone: a link is an argument now, and no caller can
    * move another thread's context. What remains is the ordinary requirement
    * that the fields be read together, and driver_session_of does exactly
    * that. */
   struct dex_session s;
   driver_session_of(link, &s);
   if (!s.mac[0])
      return -1;
   int id = -1;
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n && id < 0; i++)
      if (v.have_rec[i] && sensor_kind(v.rec[i].type) == KIND_CGM &&
          !strcmp(v.rec[i].identity, s.mac))
         id = v.slot[i].id;
   return id;
}

/* Meter-sync watchdog. If the link drops mid-sync or the connect never
 * lands, meter_busy() latches and the meter never syncs again --
 * jni_on_advert gates every sync on !meter_busy(), and dexble_link_close is
 * never reached so the GATT client stays open too.
 *
 * Called from the service tick as well as the 1 Hz timer. It used to live
 * inside sensor_reconcile, whose only caller is on_timer -- the ACTIVITY's
 * looper, which on_destroy tears down. So a sync in flight when the user
 * back-pressed or swiped the task away left the meter wedged for the whole
 * background lifetime, which is exactly the window the service exists to
 * cover, and it self-healed only when the activity was reopened. */

void pancra_link_watchdog(void)
{
   /* (last_kick[] is gone from here: the throttle is driver_kick_claim(),
    * which is per link, monotonic and a single-winner atomic exchange. It
    * moved because it is a fact about a LINK, and because on the wall clock a
    * backward correction made `now - prev` negative -- so the throttle
    * refused every kick for the whole hour it took wall time to catch up,
    * which is the one failure this watchdog exists to prevent.) */
   long now = realtime_s();
   /* BOTH CLOCKS, READ ONCE, so every link in this sweep is judged against
    * the same instant. `ok` says whether the monotonic one answered at all;
    * see scanlogic.h for what each deadline does when it did not. */
   struct live_now nw = {.wall = now, .mono = 0, .ok = MONO_GET_FAIL};
   nw.ok              = mono_try(&nw.mono);
   for (int l = 0; l < LINK_MAX; l++) {
      if (driver_link_is_meter(l))
         continue; /* CGMs only */
      struct dex_session s;
      driver_session_of(l, &s);
      if (!s.paired || !s.mac[0])
         continue;
      /* This link's own newest sample, not the global one. */
      int src   = src_for_link(l);
      long mine = 0;
      store_lock();
      for (int k = 0; k < hist_count() && !mine; k++)
         if (src >= 0 && hist_at(k).src == (unsigned short)src &&
             hist_at(k).kind != KIND_BGM)
            mine = hist_at(k).t;
      store_unlock();
      /* TWO CLOCKS, AND WHICH ONE ANSWERS DEPENDS ON WHAT IS BEING MEASURED.
       *
       * `s.last_rx.mono` is when this link last delivered a reading the app
       * KEPT, on the monotonic clock. Once anything has arrived in this
       * process that is the whole answer, and it is the only form of it a
       * wall-clock correction cannot move -- which matters more here than
       * anywhere else in the app, because this watchdog is the ONLY reconnect
       * mechanism while the screen is off (the advert path needs a scan,
       * whose lifecycle follows on_resume/on_pause).
       *
       * BEFORE THE FIRST RECEIPT there is no interval to measure, and the
       * only evidence available is `mine` -- this link's newest STORED
       * sample, whose timestamp is a persisted realtime instant and stays
       * one. It is what makes a relaunch after a long kill heal in seconds
       * rather than in LIVE_SILENCE_S: the log already says this sensor has
       * been quiet for half an hour. `now >= mine` guards the rollback -- a
       * sample stamped in the future cannot be aged, and an unageable sample
       * is not evidence that the link is alive -- and falling through to the
       * since-launch interval keeps the watchdog running on the monotonic
       * clock instead of parking it on a negative age that can never exceed
       * the threshold.
       *
       * So: the DEADLINE is monotonic in every branch that measures elapsed
       * time, and the one realtime term left is an IDENTITY -- "what does the
       * stored log say about this sensor" -- read once, before this process
       * has any interval of its own to offer. */
      struct live_stamp up = {.wall = 0, .mono = shell_launch_mono()};
      int quiet;
      long age;
      if (s.last_rx.mono != MONO_NEVER) {
         quiet = live_silence_due(&s.last_rx, &nw);
         age   = (nw.ok == MONO_GET_OK) ? nw.mono - s.last_rx.mono : 0;
      } else if (mine && now >= mine) {
         age   = now - mine;
         quiet = age > LIVE_SILENCE_S;
      } else {
         quiet = live_silence_due(&up, &nw);
         age   = (nw.ok == MONO_GET_OK) ? nw.mono - up.mono : 0;
      }
      /* LIVE_SILENCE_S is one missed 5-min cycle plus two minutes of slack.
       * At the old 700 s a link dropped with the screen dark stayed silent
       * for TWO cycles -- observed as a 13-minute gap after an app restart --
       * when one direct connect by saved MAC heals it. Backfill recovers the
       * data either way; this recovers the latency.
       *
       * driver_kick_claim is the throttle, and it is a CLAIM: this runs on
       * both the activity's 1 Hz timer and the service's 20 s tick, and
       * exactly one of them may act per interval. See dexdriver.h for what
       * two simultaneous kicks do to a link. */
      if (quiet && driver_kick_claim(l)) {
         LOGI("watchdog: link %d %ld s since its last reading -> "
              "reconnect",
              l, age);
         dexble_reconnect(l);
      }
   }
}

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

/* Re-read every log from disk.
 *
 * Only sync_restore needs this: it APPENDS rows to the log files behind the
 * app's back, and nothing in memory knows. Without a reload the rows are on
 * disk and the plot, the statistics and the history still show what they
 * showed before -- which looks exactly like a restore that did nothing.
 *
 * Same lock discipline as startup: sensors first (readings resolve their
 * source through the registry), then the history under hist_lock, because
 * store_load rewrites g_hist wholesale and a main-thread draw must not see it
 * half-shifted.
 *
 * THE STATISTICS ARE PART OF "EVERY LOG", and for a long time they were not.
 * The paragraph above has always CLAIMED the statistics come back, and the
 * sentence was false: stat_load ran once, at startup, and nothing here
 * touched the buckets. So a restore filled the history list and redrew the
 * plot while the TIR and AVERAGE printed against that same plot stayed at
 * whatever the fresh install had computed for itself -- two figures on one
 * screen describing the same readings and disagreeing, with no way for the
 * user to tell which pair to believe, until they restarted the app.
 *
 * ...AND THE HISTORY THEY DESCRIBE HAS TO BE RIGHT FIRST. store_load used to
 * INSERT into the live table rather than replace it, so after a restore the
 * history was the union of what was there before and what came back: a
 * reading the user had deleted was still on the plot, and where the restored
 * file corrected a value the dedup kept the old one. Rebuilding the
 * statistics from that table would have produced figures matching neither the
 * file nor the plot -- a second wrong number, computed carefully. store_load
 * now stages a whole table and swaps it (see store.c), and the rebuild below
 * is published AFTER that swap, from the same file, under the same lock. */
void pancra_logs_reload(void)
{
   sensors_load();
   /* The primary BEFORE the history lock: registry -> history is the order,
    * and store_load needs the id to re-bind the big number. */
   int prime = sensor_primary_id();
   /* THE STATISTICS ARE PARSED HERE, BEFORE THE HISTORY LOCK, AND PUBLISHED
    * INSIDE IT. Both halves of that are lock order, not taste.
    *
    * The parse resolves every row's sensor through the REGISTRY (the warm-up
    * hour is per-sensor, anchored on its activation), and the order is
    * driver -> registry -> history. Parsing under the history lock would take
    * the registry inside it -- the inversion behind two phone freezes in one
    * day -- and app/test/lockorder.py refuses it. Publishing is a copy and
    * two stores, no I/O and no other lock, so it belongs inside the very hold
    * store_load already takes: the restored history and the restored
    * statistics then become visible in the same instant, and no frame can
    * ever draw the new plot beside the old average.
    *
    * AFTER sensors_load(), which is not optional: the replay excludes a
    * sensor's warm-up hour, and the activation it measures that against comes
    * out of the registry this call has just re-read. Preparing first would
    * count uncalibrated readings the live path never counted, so the restored
    * numbers would differ from the ones the phone had produced from the very
    * same rows. */
   int prepared = stat_reload_prepare(store_path());
   store_lock();
   int rc = store_load(prime);
   stat_reload_publish();
   store_unlock();
   if (!prepared)
      LOGW("restore: the statistics could not be rebuilt; TIR and the "
           "average still describe the record from before the restore");
   if (rc < 0)
      LOGW("restore: the readings log could not be re-read whole");
   insulin_load();
   weight_load();
   shell_ui_dirty();
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
    * every other device's rows. Whether the signal strength beside it was
    * measured on THIS connection is an elapsed-time question, and on the wall
    * clock a correction answered it wrong in both directions. */
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
       * reading of any new sensor always lands here. g_cur_src belongs to
       * some ALREADY REGISTERED sensor, so borrowing it is safe only when
       * none exists (a fresh install, where g_cur_src is 0 = pre-registry
       * legacy and therefore unambiguous). The threshold is >= 1, not > 1:
       * with one sensor already registered, a second sensor's first reading
       * was written to the append-only log carrying the FIRST sensor's id --
       * and if that sensor had reported within 150 s, hist_insert deduped it
       * away entirely. Drop instead: the next sample arrives in ~5 minutes,
       * by which time the sensor is registered. A wrong attribution is
       * permanent; a missed sample is not. */
      if (sensor_live_cgm_count() >= 1) {
         LOGI("glucose %d mg/dL from an unregistered link, deferred", mg_dl);
         return 0;
      }
      src = g_cur_src;
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
    * because the lock order is registry -> history. */
   int warm = sensor_in_warmup(src, t);
   /* ONE OPERATION, not six steps in the right order. store_record owns the
    * lock, the insert, the statistics, the current value and the disk -- see
    * store.h for what each of those orderings costs when a caller gets it
    * wrong, which is why they are no longer a caller's business. */
   struct reading_event ev  = {.t          = t,
                               .glu        = mg_dl,
                               .trend      = trend,
                               .src        = src,
                               .kind       = KIND_CGM,
                               .rssi       = g_conn_rssi,
                               .has_rssi   = has,
                               .raw        = t,
                               .tz         = g_tz_off,
                               .rescale_pm = rpm,
                               .warm       = warm,
                               .prime      = prime};
   struct reading_result rr = store_record(&ev, CHIRP_MAX_GAP_S);
   int prev_glu             = rr.prev_glu;
   int isnew                = rr.inserted;
   if (isnew && has) {
      store_note_rssi(g_conn_rssi); /* kept like glu/trend */
   }
   /* Capture THIS CGM link's predicted value + time for the imminent-hypo
    * (predicted-low) alarm. The session is read for THIS LINK by name --
    * it used to be read from whichever context the callback had selected,
    * which is the same thing only for as long as that stays true. Stamped
    * every reading (not just new ones) so the freshness gate in
    * any_pred_low() tracks the live prediction. */
   if (link >= 0 && link < LINK_MAX && !driver_link_is_meter(link)) {
      struct dex_session ps;
      driver_session_of(link, &ps);
      /* mono_s(), not realtime_s(): this stamp is only ever used to ask "did
       * this prediction arrive recently enough to still mean something",
       * which is an interval. It is never persisted and never displayed, so
       * nothing wants the wall clock -- and on the wall clock a backward
       * correction made every recorded prediction permanently fresh, wedging
       * the one alarm the user cannot silence. See alarm_pred_low. */
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
   if (isnew && !rr.persisted)
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
   if (isnew && src == prime) {
      if (sp.newdata_mode == ND_BEEP) {
         dexble_beep();
      } else if (sp.newdata_mode == ND_CHIRP) {
         /* A rescale that ACTIVATED on this very reading makes the
          * comparison meaningless: mg_dl is already rescaled while every
          * sample already in g_hist is not, so the "change" would be the
          * calibration offset itself -- entering a 120 fingerstick against a
          * sensor reading 100 would chirp a full-cap rocket that never
          * happened. Treat it as having no predecessor, which is the plain
          * BEEP pitch. */
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

   /* Read the sensor's serial / firmware / software strings. Deferred to
    * here (after the first reading) so it runs post-auth, when the reads
    * succeed. The sensor closes the cycle within a few seconds, often before
    * all three reads land, so we retry each reconnect until we have them all
    * -- throttled to at most once a minute, and stopping entirely once
    * complete. */
   /* Gate on THIS LINK's strings, not the process-global ones. Keying on the
    * globals meant that once any sensor had filled them -- and they persist
    * to disk -- DIS was never re-read for any sensor again, so every later
    * sensor was minted carrying the first one's model and firmware. */
   /* THE THROTTLE IS A DEADLINE, so it is claimed from the driver on the
    * monotonic clock (driver_dis_claim). Claimed LAST, after the "do we still
    * need these strings" test: a claim consumes the interval whether or not
    * the caller goes on to use it, so testing it first would burn the window
    * on a link that already has everything. */
   int dlink = link;
   if (dlink >= 0 && dlink < LINK_MAX &&
       (!g_model_l[dlink][0] || !g_fw_l[dlink][0] || !sp.mfr[0]) &&
       driver_dis_claim(dlink)) {
      dexble_request_devinfo_link(dlink);
   }

   int did_bf = 0;
   /* Backward fill, at most once per launch: pull history back to the start
    * of the available window = min(24h, session age). Gating on session
    * duration means a young session stops re-requesting once we hold its
    * whole span, instead of forever chasing a 24h it can never reach. */
   int bflink = link;
   if (bflink < 0 || bflink >= LINK_MAX)
      bflink = LINK_CGM;
   if (!g_startup_bf[bflink]) {
      g_startup_bf[bflink] = 1;
      struct dex_session s;
      driver_session_of(bflink, &s); /* named, and locks itself */
      long target = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds < target)
         target = (long)s.session_seconds;
      /* Oldest sample FROM THIS SENSOR. g_hist[hist_count()-1] is the oldest
       * across all sources, so after store_load restored a week of merged
       * history this test was always false and the once-per-launch fill
       * never ran at all -- while g_startup_bf had already been latched. */
      long oldest = t;
      store_lock();
      for (int i = hist_count() - 1; i >= 0; i--)
         if (hist_at(i).src == (unsigned short)src &&
             hist_at(i).kind != KIND_BGM) {
            oldest = hist_at(i).t;
            break;
         }
      store_unlock();
      if (target > 600 && realtime_s() - oldest < target - 300) {
         LOGI("backward fill: %ld s backfill (have %ld s of %ld s window)",
              target, realtime_s() - oldest, target);
         driver_request_backfill(bflink, target); /* takes the lock itself */
         did_bf = 1;
      }
   }
   /* Ongoing: recover ANY interior hole in this sensor's recent buffer
    * window, retried until it is filled.
    *
    * The old rule keyed the gap off `prev` (the newest reading before this
    * one) and fired at most once per gap. That stranded interior holes: the
    * moment a single reading lands past a gap -- which the very next
    * reconnect after an outage delivers -- `prev` advances past it, every
    * later span is a normal 5-minute step, and the missing block is never
    * requested again. The once-per-launch backward fill above does not save
    * it either: it is gated off whenever we already span the window. A
    * ~15-minute reinstall gap was lost permanently this way even though the
    * sensor still held the records.
    *
    * Instead: scan this sensor's samples across the sensor's buffer window
    * (min 24h, session age) for the OLDEST >450 s hole, and request backfill
    * covering from there to now. Delivered records dedupe on insert, so a
    * wide re-request is harmless; the request is throttled per link and,
    * because it is driven by the presence of a hole rather than a one-shot
    * event, it simply repeats each cycle -- shrinking the span as records
    * arrive -- until no hole remains. This survives the short (~4 s)
    * per-cycle connect window, which a single one-shot request does not. */
   if (!did_bf && isnew) {
      struct dex_session s;
      driver_session_of(bflink, &s);
      long window = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds > 0 &&
          (long)s.session_seconds < window)
         window = (long)s.session_seconds;
      long now      = realtime_s();
      long floor_t  = now - window;
      long gap_from = 0; /* older edge of the OLDEST hole within the window */
      store_lock();
      long newer =
          0; /* previous (newer) sample's time, walking newest->oldest */
      for (int i = 0; i < hist_count(); i++) {
         if (hist_at(i).src != (unsigned short)src ||
             hist_at(i).kind == KIND_BGM)
            continue;
         long ts = hist_at(i).t;
         if (ts < floor_t)
            break; /* g_hist is newest-first, so we are past the window */
         if (newer && newer - ts > 450)
            gap_from = ts; /* hole between ts and newer; keep the oldest one */
         newer = ts;
      }
      store_unlock();
      /* Throttle so a persistent hole (records genuinely gone from the
       * sensor) is retried at a sane cadence, not on every reading. */
      if (gap_from > 0 && now - g_gap_bf_at[bflink] > 120) {
         g_gap_bf_at[bflink] = now;
         long span = (now - gap_from) + 300; /* cover the hole plus a margin */
         LOGI("interior gap in window -> backfill span %ld s (retry until "
              "filled)",
              span);
         driver_request_backfill(bflink, span);
      }
   }
   /* THE ANSWER THE DRIVER WAITS FOR: a record entered the history.
    *
    * HIST_NEW or HIST_OLD -- both put a row in the log -- and NOT HIST_DUP,
    * which is store_record's way of saying this sample was already there and
    * nothing was added. Every `return 0` above is a refusal that leaves the
    * screen exactly as it was; a duplicate does too. See reading.h. */
   return isnew != HIST_DUP;
}

/* live connection signal strength from readRemoteRssi (no sensor-battery
 * cost)
 */
void pancra_rssi(int link, int rssi)
{
   /* PER LINK. jni_rssi knows which link the measurement came from, but this
    * used to drop it into globals that fill_sensor then stamped onto EVERY
    * CGM slot -- so with a Stelo and a G7 both worn, each device screen could
    * show the other's signal, which is the one number on that row the user
    * cannot sanity-check. The globals stay for the main screen's single
    * readout; the per-link copy is what the per-device row reads. */
   int lk = link;
   if (lk >= 0 && lk < LINK_MAX) {
      /* TWO STAMPS FOR ONE MEASUREMENT, and both are needed. The realtime one
       * goes to reading_note_rssi because the per-device row DISPLAYS when
       * the signal was last seen; the monotonic one is the freshness DEADLINE
       * that decides whether this measurement belongs to the connection a
       * reading arrives on. A wall-clock correction moves the first (it is a
       * civil instant, and that is what it is for) and cannot move the
       * second. */
      reading_note_rssi(lk, rssi, realtime_s());
      driver_rssi_note(lk);
   }
   g_conn_rssi = rssi;
   /* Latch the CGM's last signal strength the MOMENT it is measured on
    * connect, exactly like pancra_meter_rssi does for a meter -- not gated
    * behind a fresh datapoint. Otherwise the Stelo's SIGNAL row drops to
    * "--" whenever readings lag, while a meter (which latches on connect)
    * keeps showing its last value. This is a retained "last known" display,
    * so it never expires.
    */
   store_note_rssi(rssi);
   LOGI("rssi %d dbm", rssi);
   shell_repaint();
}

/* Copy a DIS string into a 24-byte field, NEUTERING the CSV delimiters.
 *
 * These strings come off the sensor's GATT server and go straight into
 * sensors.csv as bare %s fields -- no quoting, no escaping. A model or
 * firmware value containing a COMMA shifts every following field on parse
 * (activation and paired times land in the wrong columns); one containing a
 * NEWLINE splits the row in two, and sensor_mint documents exactly where
 * that leads: an unparseable row hides an id from the loader, maxid goes
 * backwards, and the next mint REISSUES A LIVE ID -- the one failure the
 * whole provenance design exists to make impossible, and it is permanent
 * because the file is never rewritten.
 *
 * The value is attacker-controlled by anything that can present the locked
 * MAC, and merely quirky vendor firmware could do it by accident. Substitute
 * rather than truncate: an empty firmware field is itself meaningful (it
 * marks the row stale and drives the re-mint pass), so dropping characters
 * could turn a hostile string into a silent re-mint loop. */
static void devinfo_copy(char *dst, const char *src)
{
   int k = 0;
   for (; src[k] && k < 22; k++) {
      unsigned char c = (unsigned char)src[k];
      dst[k]          = (c < 0x20 || c > 0x7e || c == ',') ? '_' : (char)c;
   }
   dst[k] = 0;
}

/* device-info string (serial / firmware / software) read from DIS 0x180A */
void pancra_devinfo(int link, const char *uuid, const char *val)
{
   if (!val || !val[0] || !uuid)
      return;
   /* uuid is the full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb";
    * the 16-bit assigned number sits at offset 4. Guard the length before
    * indexing uuid+4 so a short/empty string can't read out of bounds. */
   int ulen = 0;
   while (uuid[ulen] && ulen < 8)
      ulen++;
   if (ulen < 8)
      return;
   /* A meter's identity must not land in the CGM's globals: each sensor's
    * model/firmware is part of its permanent provenance, and mixing them
    * would attribute readings to hardware that never produced them. */
   /* READ THE ROUTING BIT UNDER THE LOCK. link_set_meter's comment claims
    * this function is one of the binder-thread readers that "already hold
    * it" -- the premise the writer's own locking rests on -- and it did not.
    * The write lands on the main thread in meter_sync_start/commit_pair
    * immediately before the connect, and this read arrives on a binder
    * thread just after it, so a stale value is exactly the ordering the
    * lock's barrier exists to prevent. Snapshot and release: the rest of the
    * function takes the DIS lock, and there is no reason to hold both. */
   /* driver_link_is_meter takes the driver lock itself: the table it
    * reads is written under that lock, and having every caller remember to is
    * how one function ended up reading it twice and getting two answers. */
   int is_meter = driver_link_is_meter(link);
   /* WHICH FIELD this UUID carries, decided once. -1 = none of ours.
    *
    * Neither branch writes anywhere itself: the meter and the settings module
    * each own their strings and sanitise what arrives, because this value
    * comes off a GATT characteristic on a binder thread and ends up in files
    * that are never rewritten. */
   int mdis = -1; /* the meter's field, when this is a meter link */
   int pref = -1; /* or the process-global preference field, when it is not */
   if (is_meter) {
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         mdis = METER_DIS_MODEL;
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         mdis = METER_DIS_FW;
      else
         return;
   } else if (strncmp(uuid + 4, "2a24", 4) == 0) {
      pref = SET_DIS_MODEL;
   } else if (strncmp(uuid + 4, "2a26", 4) == 0) {
      pref = SET_DIS_FW;
   } else if (strncmp(uuid + 4, "2a29", 4) == 0) {
      pref = SET_DIS_MFR;
   }
   /* Keep a PER-LINK copy as well. sp.model/sp.fw are
    * process-global and shared by every CGM link, and the devinfo re-read is
    * skipped once they are non-empty (and they persist to disk), so a second
    * sensor was minted with the FIRST sensor's model and firmware -- written
    * into an append-only provenance file that is never rewritten, and used as
    * part of the id-reuse key. Pair a G7 after a Stelo and its permanent record
    * claimed Stelo hardware. Minting reads the per-link copy. */
   /* THE PER-LINK COPY IS THE MINT INPUT, so it is what needs the lock.
    *
    * An earlier version locked only the process-global
    * sp.model/sp.fw below, which are display-only -- the arrays
    * sensor_mint actually reads were left written byte-by-byte with no lock at
    * all, so the fix was inert. A torn read (terminator not yet written, so
    * "1.4" over "1.2.3" reads as "1.4.3") matches no stored row, mints a NEW id
    * and rebinds the slot, permanently splitting one physical sensor into two
    * identities in an append-only file. Readers hold the same lock: reading_dis
    * takes it, and it is the only way out of this file. */
   /* REUSE THE SNAPSHOT -- do not re-read g_link_meter here.
    *
    * This read was under the REGISTRY's lock, which is the wrong lock for
    * that variable (link_set_meter writes it under driver_lock), and taking
    * driver_lock inside the registry's would invert the documented
    * driver -> reg order. But the deeper problem is that it was a SECOND,
    * independent read of a bit already decided above: link_set_meter landing
    * between the two makes this function pick the meter branch for `dst` and
    * the CGM branch for the per-link copy. Since the per-link copy is the
    * mint input, that writes a METER's model into the array sensor_mint
    * reads, in an append-only provenance file that is never rewritten. One
    * snapshot, one decision. */
   mutex_lock(&dis_lk);
   if (link >= 0 && link < LINK_MAX && !is_meter) {
      char *ld = 0;
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         ld = g_model_l[link];
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         ld = g_fw_l[link];
      if (ld)
         devinfo_copy(ld, val);
   }
   mutex_unlock(&dis_lk);
   if (mdis < 0 && pref < 0)
      return;
   if (mdis >= 0)
      meter_set_dis(mdis, val); /* the meter sanitises its own */
   else
      /* Stores AND persists. A failure here is not worth a banner -- these
       * are device-information strings the sensor will report again on the
       * next connection -- but it must not be reported as recorded. */
      if (settings_set_dis(pref, val) != SETTINGS_OK)
         LOGW("devinfo %s NOT saved", uuid);
   LOGI("devinfo %s = %s", uuid, val);
   shell_repaint();
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
      src = g_cur_src; /* single CGM: the global is unambiguous */
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
    * registry -> history lock order (sensors.h). */
   int warm = sensor_in_warmup(src, t);
   /* Backfill: no RSSI. A failure here loses a point the sensor already held,
    * so it is worth the same visible refusal. Delivered by pancra_remote_sync()
    * on the tick. */
   struct reading_event bev  = {.t          = t,
                                .glu        = mg_dl,
                                .trend      = trend,
                                .src        = src,
                                .kind       = KIND_CGM,
                                .raw        = t,
                                .tz         = g_tz_off,
                                .rescale_pm = rpm,
                                .warm       = warm,
                                .prime      = prime};
   struct reading_result brr = store_record(&bev, CHIRP_MAX_GAP_S);
   int isnew                 = brr.inserted;
   if (isnew && !brr.persisted)
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
   return isnew != HIST_DUP;
}

/* --- what the rest of the app asks about attribution (see reading.h) --- */

int reading_src(void)
{
   return g_cur_src;
}

void reading_set_src(int id)
{
   g_cur_src = id;
}

/* COPIED UNDER dis_lk -- the same lock pancra_devinfo writes them under, on a
 * binder thread. Testing and snapshotting them unlocked made that writer's
 * lock inert: the emptiness test passes as soon as the writer lands byte 0,
 * so a firmware of "1.6.0.11" could be read as "1" and minted that way, into
 * a file that is never rewritten.
 *
 * There is no "_locked" variant to call instead: this takes its own lock, and
 * the caller that used to hold the registry lock around a run of these was
 * exactly the caller that held it across driver calls too. */
void reading_dis(int link, char *model, int mcap, char *fw, int fcap)
{
   mutex_lock(&dis_lk);
   if (model && mcap > 0)
      model[0] = 0;
   if (fw && fcap > 0)
      fw[0] = 0;
   if (link < 0 || link >= LINK_MAX)
      goto out;
   if (model)
      str_snapshot(model, mcap, g_model_l[link]);
   if (fw)
      str_snapshot(fw, fcap, g_fw_l[link]);
out:
   mutex_unlock(&dis_lk);
}

void reading_forget_dis(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   mutex_lock(&dis_lk);
   g_model_l[link][0] = 0;
   g_fw_l[link][0]    = 0;
   mutex_unlock(&dis_lk);
}
