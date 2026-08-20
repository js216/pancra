// SPDX-License-Identifier: GPL-3.0
// reconcile.h --- Live sessions and the registry, kept in agreement
// Copyright 2026 Jakob Kastelic
//
/* WHICH DEVICE IS ON WHICH LINK, AND WHICH ROW DESCRIBES IT.
 *
 * Two pictures of the same devices drift apart on their own: the DRIVER knows
 * what is bonded on each BLE link right now, and the REGISTRY holds the
 * permanent, append-only row for each device the user has paired. Nothing
 * connects them except this pass, which runs once a second and answers three
 * questions:
 *
 *   1. WHICH LINK belongs to a slot -- resolved by ADDRESS, never by index.
 *      Index-based resolution is what let a second sensor's readings be
 *      stamped with the first's id.
 *   2. WHICH SENSOR still needs registering: a link carrying a bonded session
 *      with a reading that no slot claims yet.
 *   3. WHAT IS STILL MISSING from an already-registered row -- its model,
 *      firmware and activation instant, each of which only becomes knowable
 *      after the row was written.
 *
 * The rules for all three are pure and live in senslogic.h, where a test can
 * reach them; this file observes, locks and acts.
 *
 * SERIALISED BY A TRY-LOCK, because it runs from the activity's 1 Hz timer
 * AND, once that timer is gone, from the service tick. It mints ids and
 * appends to a file that is never rewritten, so two concurrent passes could
 * mint twice for one sensor. Skipping a tick is free.
 */
#ifndef RECONCILE_H
#define RECONCILE_H

/* The BLE link bound to this registry SLOT, or -1. Address-based. */
/* THE LINK A DEVICE IS ON, BY ID -- never by slot index.
 *
 * A slot index is a position and the table shifts under it, so an index that
 * travelled from one function to another could name a different device by the
 * time the radio was told about it: a retire would then close the link of the
 * sensor that had slid into the retired one's place. The index resolution
 * lives inside reconcile.c, against the same snapshot that ranks the links. */
int link_for_sensor(int id);
/* The link a NEW sensor would take, with the count from that same snapshot. */
int link_for_new_sensor(void);
/* (The link searches moved to dexdriver.h -- driver_link_of_identity and
 * driver_free_cgm_link. A question about which link holds which session is
 * the driver's, and asking it through this workflow header is what made the
 * meter runtime depend on the reconcile pass.) */

/* One pass. Safe from either thread; skips if one is already running. */
void sensor_reconcile(void);

/* Keep the sensor registry in step with the live session. Safe on any thread
 * and with no activity alive; the service heartbeat drives it, so a sensor
 * that bonds in the background is still registered. */
void pancra_reconcile_tick(void);

#endif
