// SPDX-License-Identifier: GPL-3.0
// linkhealth.c --- link liveness and history repair (see linkhealth.h)
// Copyright 2026 Jakob Kastelic

#include "linkhealth.h"
#include "bletrans.h" /* dexble_reconnect: the one repair this module makes */
#include "clock.h"
#include "dexdriver.h" /* the session, the per-link claims, and the backfill */
#include "linkinfo.h"  /* src_for_link: whose history is being judged */
#include "log.h"
#include "readingrec.h" /* struct reading: one stored sample */
#include "scanlogic.h"  /* live_silence_due: the deadline, on both clocks */
#include "shell.h" /* shell_launch_mono: the interval before first receipt */
#include "store.h" /* this sensor's own samples, as one snapshot */

/* Per LINK, not per process: a single flag was latched by whichever CGM
 * reported first, so a second sensor never requested its once-per-launch
 * backward fill and its pre-launch history was never recovered, on every
 * launch. */
static int g_startup_bf[LINK_MAX];
/* Per-link throttle for the interior-gap backfill scan below: it re-requests
 * until the hole is filled, so it must not fire on every 5-minute reading. */
static long g_gap_bf_at[LINK_MAX];

/* Meter-sync watchdog. If the link drops mid-sync or the connect never
 * lands, meter_busy() latches and the meter never syncs again --
 * jni_on_advert gates every sync on !meter_busy(), and dexble_link_close is
 * never reached so the GATT client stays open too.
 *
 * Called from the service tick as well as the 1 Hz timer, and NOT from
 * inside sensor_reconcile, whose only caller is on_timer -- the ACTIVITY's
 * looper, which on_destroy tears down. Driven from there, a sync in flight
 * when the user back-presses or swipes the task away leaves the meter wedged
 * for the whole background lifetime -- exactly the window the service exists
 * to cover -- self-healing only when the activity is reopened. */

void pancra_link_watchdog(void)
{
   /* (last_kick[] is gone from here: the throttle is driver_kick_claim(),
    * which is per link, monotonic and a single-winner atomic exchange. It
    * moved because it is a fact about a LINK, and because on the wall clock a
    * backward correction made `now - prev` negative -- so the throttle
    * refused every kick for the whole hour it took wall time to catch up,
    * which is the one failure this watchdog exists to prevent.) */
   /* THE SAVE THAT DID NOT LAND, TRIED AGAIN. A recovered sensor
    * address whose file write failed leaves the driver dialling the old
    * target and the next launch with nothing; this is the only sweep that
    * runs on both the activity timer and the service tick, so it is where
    * the retry belongs. It throttles itself and does nothing at all when
    * nothing is owed. */
   driver_bind_retry();
   /* THE COOLDOWN'S ONLY DRIVER. A disconnect ARMS the next attempt rather
    * than making it -- see dex_retry_delay -- so without this call a link
    * that dropped never dials again on its own, and every cadence arrives
    * late through the 400-second silence branch below instead. */
   driver_retry_tick();
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
      /* l is in range by the loop bound; the answer is still read, because a
       * refusal means this link is not one the driver has and nothing about
       * it can be judged. */
      if (!driver_session_of(l, &s) || !s.paired || !s.mac[0])
         continue;
      /* This link's own newest sample, not the global one. */
      int src = src_for_link(l);
      /* ONE QUESTION, ANSWERED UNDER ITS OWN LOCK, rather than a
       * count/index walk with the store lock taken by hand around it and the
       * `kind != KIND_BGM` rule written out here for a third time. */
      long mine = hist_newest_t(src);
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
       * clock rather than parking it on a negative age that can never exceed
       * the threshold.
       *
       * So: the DEADLINE is monotonic in every branch that measures elapsed
       * time, and the one realtime term left is an IDENTITY -- "what does the
       * stored log say about this sensor" -- read once, before this process
       * has any interval of its own to offer. */
      struct live_stamp up = {.wall = 0, .mono = shell_launch_mono()};
      int quiet            = 0;
      long age             = 0;
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
       * At 700 s a link dropped with the screen dark stays silent for TWO
       * cycles -- observed as a 13-minute gap after an app restart -- when
       * one direct connect by saved MAC heals it. Backfill recovers the
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

/* THE TWO REPAIRS A READING TRIGGERS. See linkhealth.h. */
void linkhealth_after_reading(int link, int src, long t, int kept)
{
   int bflink = link;
   int did_bf = 0;
   /* Backward fill, at most once per launch: pull history back to the start
    * of the available window = min(24h, session age). Gating on session
    * duration means a young session stops re-requesting once we hold its
    * whole span, rather than forever chasing a 24h it can never reach. */
   if (bflink < 0 || bflink >= LINK_MAX)
      bflink = LINK_CGM;
   if (!g_startup_bf[bflink]) {
      g_startup_bf[bflink] = 1;
      struct dex_session s = {0};
      /* named, and locks itself; bflink was clamped into range above. A
       * refusal leaves s zeroed, which reads as "no reading yet" -- the same
       * conservative span this would choose anyway, so the answer is read
       * only to say that out loud. */
      if (!driver_session_of(bflink, &s))
         s = (struct dex_session){0};
      long target = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds < target)
         target = (long)s.session_seconds;
      /* Oldest sample FROM THIS SENSOR. g_hist[hist_count()-1] is the oldest
       * across all sources, so after store_load restored a week of merged
       * history this test was always false and the once-per-launch fill
       * never ran at all -- while g_startup_bf had already been latched. */
      long oldest = hist_oldest_t(src);
      if (!oldest)
         oldest = t; /* nothing from this sensor in the tail yet */
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
    * KEYING THE GAP OFF `prev` (the newest reading before this one), and
    * firing at most once per gap, strands interior holes: the moment a single
    * reading lands past a gap -- which the very next reconnect after an
    * outage delivers -- `prev` advances past it, every
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
   if (!did_bf && kept) {
      struct dex_session s = {0};
      if (!driver_session_of(bflink, &s)) /* in range; zeroed on refusal */
         s = (struct dex_session){0};
      long window = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds > 0 &&
          (long)s.session_seconds < window)
         window = (long)s.session_seconds;
      long now      = realtime_s();
      long floor_t  = now - window;
      long gap_from = 0; /* older edge of the OLDEST hole within the window */

      /* THIS SENSOR'S SAMPLES, TAKEN AS ONE SNAPSHOT. A walk that reads
       * hist_count() and hist_at() with the store lock taken by hand is a
       * count and an indexed read of a table a binder thread appends to.
       * What the search itself is (a hole between two consecutive samples)
       * belongs here; WHICH samples belongs to the store. */
      /* BOUNDED, and the bound is the window's own size: this loop stops at
       * the first sample older than floor_t, and floor_t is at most 24 h back
       * -- 288 samples at a CGM's 5-minute cadence. 512 covers that with room
       * for a stretch of denser backfilled points, and a truncation would
       * only mean the search looked at the newest 512 rather than further.
       * NOT static: this runs on a binder thread, and a shared buffer would
       * be two sensors' samples in one array. */
      enum { GAP_LOOK = 512 };
      struct reading mine[GAP_LOOK];
      int nm     = hist_copy_src(src, mine, GAP_LOOK);
      long newer = 0; /* previous (newer) sample's time, newest->oldest */
      for (int i = 0; i < nm; i++) {
         long ts = mine[i].t;
         if (ts < floor_t)
            break; /* newest-first, so we are past the window */
         if (newer && newer - ts > 450)
            gap_from = ts; /* hole between ts and newer; keep the oldest one */
         newer = ts;
      }
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
}
