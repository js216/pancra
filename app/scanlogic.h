// SPDX-License-Identifier: GPL-3.0
// scanlogic.h --- Pure scan-lifecycle decision (host-testable)
// Copyright 2026 Jakob Kastelic

/* Should the BLE scan be (re)started right now?
 *
 * Split out of on_timer for the same reason as alarmlogic.c: nothing in the
 * Android shell
 * is reachable by any test, and this predicate has already been wrong once. It
 * governs whether an already-paired CGM can reconnect at all, because the
 * advert-driven reconnect runs off this scan -- so a condition too strict
 * leaves sensors silently offline, and one too loose re-enters JNI every tick
 * and trips Android's "app scanning too frequently" block, turning a
 * recoverable failure into a sticky one.
 *
 * Pure: no globals, no clock, no JNI. main.c passes the state in. */
#ifndef SCANLOGIC_H
#define SCANLOGIC_H
#include "clock.h" /* enum mono_get -- the TYPE only; nothing here reads a clock */

/* Minimum seconds between restart attempts. start_scan only clears the
 * "scanning" flag on SUCCESS, so a persistent failure (Bluetooth off, the scan
 * permission revoked, no LE scanner) leaves the condition true forever -- at
 * 1 Hz that is a JNI call and a rewritten status line every second, and enough
 * startScan calls to trip Android's 5-in-30-seconds block. */
#define SCAN_RETRY_S 30

/* The inputs, named so a caller cannot transpose two ints by accident. */
struct scan_state {
   int have_activity; /* an activity exists to scan on behalf of */
   int paused;        /* on_pause ran; the scan is down deliberately */
   int scanning;      /* we believe a scan is live */
   int pairing;       /* PAIR NEW SENSOR owns the radio */
   int meter_busy;    /* a meter sync is in flight */
   long now;
   long hold_until;   /* quiet-radio window after a bonding connect */
   long last_attempt; /* when a restart was last tried (0 = never) */
};

int scan_should_start(const struct scan_state *s);

/* Which scanned device to pair with, or -1 to show the list instead.
 *
 * The rule: pair automatically only when ONE candidate is unambiguously the
 * nearest -- at least SCAN_AMBIG_DB stronger than every other. Below that the
 * user must choose, because commit_pair drops the old bond and pairs the MAC
 * it is given, so guessing wrong is destructive and silent.
 *
 * Lifted out of main.c because nothing there is reachable by any test: an
 * adversarial review deleted the 20 dB rule outright -- auto-pairing whichever
 * sensor happened to be strongest, with no list ever shown -- and the entire
 * gate stayed green. `rssi` is in dBm, so larger (less negative) is nearer. */
#define SCAN_AMBIG_DB 20

int scan_pick_candidate(const int *rssi, int n);

/* ---- AN ASYNCHRONOUS SCAN FAILURE, AND WHOSE SCAN IT WAS ------------------
 *
 * WHAT THE USER SAW, and it is the worst kind of failure this app has:
 * nothing. Ble.scan() installs a ScanCallback and returns success as soon as
 * startScan() has been handed to the platform, but the platform refuses
 * ASYNCHRONOUSLY -- SCAN_FAILED_APPLICATION_REGISTRATION_FAILED while the
 * stack is coming back up, SCAN_FAILED_SCANNING_TOO_FREQUENTLY once five
 * starts have been spent in thirty seconds, SCAN_FAILED_INTERNAL_ERROR from a
 * stack on its way down. onScanFailed() only wrote a line to logcat. Native
 * had already latched "a scan is running"; every recovery path in the app is
 * gated on that flag being CLEAR, and it is cleared in exactly one place,
 * reachable only from a later SUCCESSFUL stop. So the app stopped scanning for
 * the rest of the process: no CGM reconnect after a dropout, no meter noticed
 * when it is switched on, no device ever becoming a pairing candidate -- and
 * no message anywhere, because as far as the app knew the scan was up. The
 * last reading just stops ageing forward. On a CGM reader that is the app
 * silently ceasing to find the device it exists to read, until the user
 * happens to restart the process.
 *
 * WHY A GENERATION, and not simply "a failure means stop scanning". The
 * failure is delivered on a Bluetooth binder thread, an unbounded time after
 * the call that provoked it, and by then the scan it belongs to may have been
 * stopped and replaced several times over: on_resume restarts the scan, so
 * does the DEVICES refresh (scan_restart), and pairing takes the radio and
 * gives it back. A failure that reset whatever scan happened to be live would
 * then tear down a HEALTHY one -- the same outage, reached by a different
 * route, and this time caused by the fix. So every start is handed the
 * generation it owns and a failure is acted on only when it names the
 * generation still believed live. This is the same rule, for the same reason,
 * as the per-link GATT generations in Ble.java.
 */
struct scan_fail {
   int failed_gen; /* the generation the failure carries (0 = none/unknown) */
   int cur_gen;    /* the generation the live registration was handed */
   int scanning;   /* native still believes THAT scan is up */
};

/* 1 when this failure is the live scan's own failure and native must reset. */
int scan_fail_applies(const struct scan_fail *f);

/* When the next start may be attempted after a failure at `now`.
 *
 * The SAME interval as the self-heal throttle, on purpose: a scan that fails
 * asynchronously does so on every attempt while the cause lasts (Bluetooth
 * mid-restart, the permission revoked, Android's own scan block), and the
 * cause of failure 6 is literally having called startScan too often. Backing
 * off here is what keeps a permanent failure to one startScan per interval
 * without the 1 Hz tick needing to learn a second throttle. */
long scan_fail_retry_at(long now);

/* May a start be attempted at `now`, given the back-off a failure left?
 *
 * Deliberately NOT sentinel-aware: `retry_after == 0` (no failure yet) is
 * already permitted by the comparison itself, and a separate `== 0` branch
 * would be an equivalent mutation -- a line no test could ever fail on. */
int scan_start_allowed(long now, long retry_after);

/* The status line for an android.bluetooth.le.ScanCallback error code.
 *
 * SHORT, and DIFFERENT PER CAUSE, because these are the only words the user
 * gets: "the radio is busy" and "Android is blocking us for scanning too
 * often" call for different patience, and one generic SCAN FAILED for all six
 * is what left people rebooting phones. The numbers are the framework's own
 * SCAN_FAILED_* constants, passed straight through from Java. */
const char *scan_fail_text(int err);

/* ---- THE FOUR BLE LIVENESS DEADLINES, AND WHICH CLOCK EACH END USES -------
 *
 * WHAT THE USER SAW. An NTP correction (or a manual clock change, or a phone
 * coming back from being off) moves CLOCK_REALTIME backwards by an hour.
 * Every age computed as `realtime_s() - stamp` goes NEGATIVE, so every
 * comparison of the form `age > interval` is false, and the four decisions
 * that keep a CGM connected all conclude that nothing has aged at all:
 *
 *   - the reconnect throttle refuses to let the watchdog kick, so a link that
 *     has genuinely gone quiet is never reconnected;
 *   - the DIS re-read throttle refuses, so model/firmware provenance is never
 *     completed for that sensor;
 *   - the RSSI freshness window says a measurement from before the step is
 *     from "this connection", so a stale signal number is stamped onto a
 *     stored reading as if it were live;
 *   - the CGM-silence measure itself flips between "nothing has aged" and
 *     "everything is ancient" depending on which of its two terms wins.
 *
 * For as long as it takes wall time to catch up -- minutes to hours on a
 * phone -- the app sits showing a stale glucose and never recovers. Nothing
 * on screen says so, because as far as the app is concerned the reading is
 * fresh. A forward jump is the mirror image and just as real: a reading that
 * arrived seconds ago looks ancient, and a healthy link is torn down.
 *
 * THE SPLIT. A sample's timestamp is an INSTANT: it goes in readings.csv, it
 * is compared against other devices' rows, it is shown to a person. It stays
 * on realtime_s() and MUST NOT become a monotonic value, which means nothing
 * after a reboot and nothing at all to another device. A deadline measures an
 * INTERVAL inside this process, and lives on mono_s(), which no correction
 * can move. See clock.h -- this is the same rule the meter watchdog and the
 * calibration back-offs already follow.
 *
 * SOME RECORDS THEREFORE CARRY BOTH, and that is not redundancy. struct
 * live_stamp is the pair, kept together deliberately: the wall member is the
 * instant the thing happened (the reading's own timestamp, the second an RSSI
 * was measured), the mono member is what an age is measured from. Delete
 * either one and the other cannot do its job -- the wall one cannot survive a
 * correction, and the mono one cannot be written down, sorted against another
 * phone's data, or shown to anybody.
 *
 * WHY HERE. Pure, so it is reachable by a test that can move the two clocks
 * independently, which is the only way the bug above is expressible at all:
 * with the real clocks linked in they can only be observed advancing
 * together, and that is precisely the case that never broke. The four
 * predicates are separate one-liners rather than one shared call ON PURPOSE
 * -- each is a deadline someone can revert to the wall clock on its own, and
 * a single shared predicate makes all four of those the same mutation, so a
 * suite could no longer show that it covers each of them. */

/* An unstamped / unarmed monotonic deadline.
 *
 * 0, because every one of these lives in a zero-initialised static or a
 * memset context and "never" has to be the value it starts at. It does
 * collide with a genuine stamp taken in the FIRST SECOND OF UPTIME, whose
 * only consequence is one extra retry during that second; the alternative
 * (a separate armed flag beside every stamp) buys nothing for it. */
#define MONO_NEVER 0L

/* The intervals, in one place, because three of the four used to be bare
 * numbers at their call sites in reading.c. */
#define LIVE_SILENCE_S    420 /* CGM quiet this long -> force a reconnect */
#define LIVE_KICK_MIN_S   300 /* ...and no more often than this per link */
#define LIVE_RSSI_FRESH_S 120 /* an RSSI this recent is "this connection" */
#define LIVE_DIS_RETRY_S  60  /* device-information re-read throttle */

/* ONE EVENT, ON BOTH CLOCKS. See the block above for why both are kept. */
struct live_stamp {
   long wall; /* realtime_s() when it happened: the INSTANT. 0 = never */
   long mono; /* mono_s() when it happened: the DEADLINE. MONO_NEVER = never */
};

/* BOTH CLOCKS READ AT ONE INSTANT, plus whether the monotonic one answered.
 *
 * Built by the caller (this module reads no clock); `ok` comes straight from
 * mono_try(). `wall` is here because every caller already needs it for the
 * log line and the stored row -- and because keeping the pair together is
 * what stops the next reader ageing a monotonic stamp against a wall now. */
struct live_now {
   long wall;
   long mono;
   enum mono_get ok;
};

/* CLOCK UNAVAILABLE: REFUSE, AND LEAVE THE STAMP UNARMED.
 *
 * clock_gettime on CLOCK_MONOTONIC is a vDSO call that essentially cannot
 * fail, but "essentially cannot" is exactly the reasoning that left an
 * uninitialised timespec in clock.c for a year, so it is defined rather than
 * assumed:
 *
 *   - a comparison that cannot read `now` reports NOT DUE and NOT FRESH. The
 *     app refuses to act on a deadline it cannot measure. This is the same
 *     answer the TLS ticket clock gives when the monotonic clock is
 *     unavailable: refuse the work rather than guess at the interval.
 *   - a stamp that could not be taken is left at MONO_NEVER rather than
 *     written as 0-meaning-now, so the FIRST successful read after the clock
 *     comes back finds the deadline unarmed and therefore immediately due.
 *     Recovery is not delayed by a further full interval.
 *
 * REFUSE, rather than "fire everything": a permanently dead monotonic clock
 * would otherwise mean an unthrottled reconnect on every 1 Hz tick, which is
 * how Android's five-startScan-in-thirty-seconds block gets tripped -- a
 * recoverable failure turned into a sticky one. The cost of refusing is that
 * a dead monotonic clock stops the self-heal; the cost of firing is that it
 * jams the radio. Only one of those two is reachable without the whole
 * process already being broken (thread.h's bounded waits read the same
 * clock).
 *
 * NOT-DUE and NOT-FRESH are opposite words for the same refusal, which is why
 * live_rssi_fresh is not simply !live_due: an unstamped deadline is DUE (a
 * first attempt must not wait an interval) while an unmeasured signal is NOT
 * FRESH (there is nothing to be fresh). */

/* Has this link been quiet long enough to force a reconnect? */
int live_silence_due(const struct live_stamp *rx, const struct live_now *now);
/* May this link be kicked again yet? (the per-link reconnect throttle) */
int live_kick_due(const struct live_stamp *kick, const struct live_now *now);
/* Is this signal measurement recent enough to belong to this connection? */
int live_rssi_fresh(const struct live_stamp *meas, const struct live_now *now);
/* May the device-information strings be re-requested again yet? */
int live_dis_due(const struct live_stamp *req, const struct live_now *now);

#endif
