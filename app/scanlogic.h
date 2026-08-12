// SPDX-License-Identifier: GPL-3.0
// scanlogic.h --- Pure scan-lifecycle decision (host-testable)
// Copyright 2026 Jakob Kastelic

/* Should the BLE scan be (re)started right now?
 *
 * Split out of on_timer for the same reason as alarmlogic.c: nothing in main.c
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

#endif
