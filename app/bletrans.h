// SPDX-License-Identifier: GPL-3.0
// bletrans.h --- what the app asks OF the BLE transport, and nothing else
// Copyright 2026 Jakob Kastelic

/* ONE DIRECTION ONLY: the app -> transport calls.
 *
 * Every operation here is implemented in dexble.c and called from the app
 * side (pairing, meter, reading, alarm, device...). What the transport calls
 * back INTO the app is NOT here -- those declarations live with the modules
 * that implement them (reading.h, meter.h, alarm.h, calib.h, remote.h), so a
 * caller of dexble_write() no longer gets, and no longer has to read, the
 * prototype of every hook the radio can fire.
 *
 * THE JNI-TYPED OPERATIONS ARE NOT HERE EITHER (blejni.h). They are a
 * different boundary -- the transport's registration with the Java side --
 * and a file that wants to close a link should not have to compile <jni.h>
 * to say so. (Several still see it another way round, through ui.h or
 * tzoff.h; that is those headers' business, not this one's.)
 *
 * NO .c SHARES THIS STEM, on purpose. A `dexble.h` beside `dexble.c` would
 * merge into ONE module node with the transport in the inclusion graph -- and
 * dexble.c includes alarm.h, meter.h and reading.h, every one of which
 * includes this header. That is a cycle: transport -> alarm -> transport. A
 * declaration-only header that includes nothing is a leaf instead, so both
 * sides may depend on it and neither depends on the other. srv/proto.h is a
 * leaf for the same reason, and says so. */
#ifndef PANCRA_BLETRANS_H
#define PANCRA_BLETRANS_H

#include <stdint.h>

void dexble_init(const char *data_dir);

/* Pair/connect a Dexcom sensor on `link`. */
void dexble_pair(int link, const char *mac, const char *code);
/* Ask the OS to bond with `mac` NOW, so the system pairing dialog appears as a
 * consequence of the user's tap rather than minutes later when the stack
 * happens to hit an encrypted characteristic. See the comment block above
 * Ble.createBond for why auto-accepting the dialog is not available to us.
 * Returns 1 if the request went out (or the device was already bonded). */
int dexble_create_bond(const char *mac);
/* Latest OS bond state seen for `mac`, using the framework's own constants:
 * 0 = never heard, 10 = NONE, 11 = BONDING, 12 = BONDED. Fed by Ble's
 * ACTION_BOND_STATE_CHANGED receiver. */
int dexble_bond_state(const char *mac);
void dexble_reconnect(int link); /* stall watchdog: force a fresh connect */
/* Read one link's Device Information Service (model, firmware, manufacturer).
 * LINK-ADDRESSED, and only that: the ambient no-argument version that assumed
 * LINK_CGM is gone -- a meter's strings are not a sensor's, and provenance is
 * append-only. */
void dexble_request_devinfo_link(int link);
/* Drop one link. The meter driver uses this the moment it has what it needs,
 * so the meter can power itself down instead of being held awake. */
void dexble_link_close(int link);
/* Link-addressed GATT operations (the drv_* hooks wrap these for LINK_CGM). */
void dexble_subscribe(int link, const char *uuid, int indicate);
void dexble_write(int link, const char *uuid, const uint8_t *d, int n,
                  int no_resp);
/* Connect to a OneTouch meter on `link`, and mark that link as carrying a
 * meter so the transport routes its events to the otble driver. */
int dexble_meter_connect(int link, const char *mac);
/* Tell the transport whether `link` carries a meter (1) or a CGM (0). */
void dexble_set_meter_link(int link, int on);

/* THE NOISES. They are transport operations because the Java side owns the
 * player and the vibrator, not because they have anything to do with BLE. */
int dexble_alarm(int kind, int sound, int vibrate); /* 0 low, 1 high, 2 stale */
void dexble_beep(void); /* one short NEW DATAPOINT beep */
/* One NEW DATAPOINT chirp, pitch-bent by `st10` tenths of a semitone
 * (chirp_semitone10); 0 is the beep's own pitch. */
void dexble_chirp(int st10);
/* One NUDGE: a single two-note motif plus a short buzz, descending for a low
 * crossing and rising for a high one. `kind` is 0 low, 1 high; `sound` and
 * `vibrate` are the nudge's own outputs, independent of the alarm's. */
void dexble_nudge(int kind, int sound, int vibrate);
int dexble_alarm_silence(void);

#endif
