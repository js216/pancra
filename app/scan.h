// SPDX-License-Identifier: GPL-3.0
// scan.h --- The BLE scan, and the permissions it needs
// Copyright 2026 Jakob Kastelic
//
/* RECEIVE-ONLY, and shared by everything. The scan is how a CGM is found
 * again after a dropout, how a meter is noticed the moment it is switched on,
 * and how a new device becomes a pairing candidate -- so it is not owned by
 * any one of those workflows.
 *
 * TWO RULES, both from failures that shipped:
 *
 * 1. THE SCAN IS DOWN ONLY WHEN JAVA SAYS SO. Clearing the flag while Ble
 *    still held a registered callback it could no longer cancel let the
 *    self-heal register a SECOND scan client -- duplicate adverts, and
 *    eventually Android's scan-throttle block. An unconfirmed stop is marked
 *    for RETRY instead.
 * 2. A PENDING STOP MUST NOT LATCH. Leaving "scanning" set with no scan
 *    behind it is a permanent dead state: the flag is cleared in exactly one
 *    place, reachable only from a later successful stop, and the self-heal is
 *    itself gated on it. The 1 Hz tick retries until Java confirms.
 * 3. NEITHER MUST A FAILURE THE PLATFORM REPORTS LATE. startScan succeeds and
 *    then fails on a binder thread, which used to be logged and nothing else
 *    -- the same permanent dead state as rule 2, reached without anybody
 *    calling stop. Ble.scanFailed hands the failure back with the GENERATION
 *    of the scan it belongs to (see scanlogic.h), so the live scan's failure
 *    resets the state and backs off, and a failure belonging to a scan that
 *    has since been replaced changes nothing.
 */
#ifndef SCAN_H
#define SCAN_H

struct ANativeActivity;

/* Start the receive-only scan. A no-op while one is already running. */
void start_scan(struct ANativeActivity *a);
/* Ask Java to stop it; marks a retry if Java could not confirm. */
void stop_scan(struct ANativeActivity *a);
/* Force a genuine restart. start_scan alone CANNOT do this: it early-returns
 * whenever a scan is running, which it always is while the UI is up. */
void scan_restart(struct ANativeActivity *a);

/* Is one running? Is a confirmed stop still outstanding? */
int scan_running(void);
int scan_stop_pending(void);

/* Hold the scan DOWN until `when`, so a pairing or bonding connect gets a
 * quiet radio. 0 clears the hold. */
void scan_hold_until(long when);
long scan_hold_time(void);

/* Did the permission request ask for the battery-exemption prompt to follow?
 * Taken once: the two are separate system dialogs and must not stack. */
int scan_take_battery_wanted(void);

/* The runtime permissions a scan needs, and the request for them. */
int has_ble_permissions(struct ANativeActivity *a);
void request_ble_permissions(struct ANativeActivity *a);

#endif
