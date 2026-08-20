// SPDX-License-Identifier: GPL-3.0
// device.h --- What can be DONE to a registered device
// Copyright 2026 Jakob Kastelic
//
/* THE OPERATIONS, SEPARATED FROM THE TAPS THAT REQUEST THEM.
 *
 * Each of these touches several subsystems at once -- the registry, the BLE
 * link, the driver, the meter runtime, the reading history -- in an order
 * that matters. They lived as branches inside a menu dispatcher, where the
 * order was invisible and every one of the failures documented in device.c
 * shipped: a forgotten sensor still streaming on a link that now belonged to
 * another slot, a retired meter burning one of the eight links for the life
 * of the process, a link handed on with the previous sensor's firmware still
 * cached and minted into a row that is never rewritten.
 *
 * The menus now dispatch; this is what they dispatch TO.
 */
#ifndef DEVICE_H
#define DEVICE_H

/* Retire a device: release its link, retire its slot, and re-bind the big
 * number to whoever owns it now. The slot and the provenance row are KEPT --
 * the device moves to OLD DEVICES, and its old readings still resolve. */
void device_retire(int id);

/* Revive a retired device. */
void do_reconnect(int id);

/* Has this retired device been gone long enough that reviving it needs a
 * confirmation? */
int old_sensor_expired(int id);

/* WHICH LINK the sensor whose screen is open is on, for a calibration.
 * Returns -1 when there is no usable one -- calibrating a DIFFERENT sensor
 * from the one named on screen is not an acceptable failure, so the caller
 * must check rather than fall back to a default link. */
int cal_link(void);

#endif
