// SPDX-License-Identifier: GPL-3.0
// pairing.h --- Adverts in, a registered device out
// Copyright 2026 Jakob Kastelic
//
/* THE ONE WORKFLOW THAT TURNS A STRANGER INTO A DEVICE.
 *
 * Every advertisement the radio hears lands here (on a BINDER thread), is
 * kept as a CANDIDATE, and -- if the user has committed to a pairing -- is
 * matched, registered and connected. Two things make it its own file rather
 * than branches in the shell:
 *
 * 1. IT IS THE ONLY PLACE A NEW ROW IS MINTED FROM A STRANGER'S ADDRESS, and
 *    that row is append-only. A wrong type, or the wrong candidate picked out
 *    of a churning list, is permanent.
 * 2. IT SPANS THREADS AND SCREENS. The candidate list is written by the
 *    binder thread and read by the main looper, and the flow itself runs type
 *    picker -> keypad -> device list -> confirmation, any step of which can
 *    be abandoned.
 *
 * PENDING, NOT PARKED. When the code is in but no candidate is on the air,
 * the intent is ARMED and every menu closes; the tick commits the moment an
 * unambiguous candidate appears. The old flow parked the user in the device
 * list until the sensor deigned to advertise -- and suppressed every OTHER
 * sensor's reconnect the whole time, so waiting for a new sensor cost the
 * readings of the ones already worn.
 */
#ifndef PAIRING_H
#define PAIRING_H

#include <jni.h>

/* Register the advert callback on the Ble class: one advertisement heard
 * arrives on a BINDER thread. */
int pairing_register(JNIEnv *env, jclass ble);

/* Begin smart pairing: scan for candidates while the code is typed, WITHOUT
 * disturbing a sensor that is already streaming. */
void pair_scan_start(void);
/* Abandon the flow, wherever it got to. */
void pair_cancel(void);

/* Pick the one candidate the code should pair with: -1 when none is on the
 * air, -2 when the choice is ambiguous and the user must decide. */
int select_candidate(void);
/* How many candidates are fresh enough to still be worth showing. */
int fresh_candidates(void);

/* Register and connect this address as the type the user chose. */
void commit_pair(const char *mac);
/* Open the newly registered device's own screen -- even after a failed
 * connect, because that screen is where the failure is stated and retried. */
void open_new_device(int id);

/* The keypad's OK, when what was typed is a pairing code. The one entry that
 * ends at the RADIO rather than in storage. */
int kp_commit_pair(void);

/* The 1 Hz step: commit an armed pairing as soon as an unambiguous candidate
 * appears. MAIN THREAD, like every other route into commit_pair. */
void pairing_tick(void);

/* Stop the smart scan without cancelling anything else -- it suppresses every
 * other sensor's advert-driven reconnect, so it must not outlive the screen
 * that started it. */
void pairing_stop_smart(void);

/* Is a pairing armed but not yet committed, and is the smart scan running?
 * The device list shows both. */
int pairing_pending(void);
int pairing_smart(void);
void pairing_arm(int type); /* 0 = disarm */

/* The candidate list, for the device list screen and the pipe-health row.
 * Copied out under the list lock -- the scan keeps churning it. */
struct pair_cand {
   char name[9];
   char mac[18];
   int rssi;
   unsigned count;
   /* When it was last heard, on the MONOTONIC clock: this is only ever used
    * as an interval (is it still on the air?), and an interval measured on a
    * clock that can be corrected is not an interval. */
   long seen_t;
};

int pairing_candidates(struct pair_cand *out, int cap);
int pairing_candidate_count(void);
void pairing_forget_candidates(void);
/* Propose the candidate at this row for the confirmation screen. Returns 0
 * when the row is gone -- the list reorders by live signal under the finger. */
int pairing_pick(int idx);
unsigned pairing_adverts_seen(void);

/* The address a pick proposed, awaiting the confirmation screen's YES. */
const char *pairing_pend_mac(void);
const char *pairing_pend_name(void);
void pairing_propose(const char *mac, const char *name);

#endif
