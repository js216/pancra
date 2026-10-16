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
/* dexble_pair, only while the link has not been forgotten since the session
 * read that gave `gen` (driver_start_if). */
void dexble_pair_if(int link, const char *mac, const char *code, unsigned gen);
/* The code the sensor at `mac` was paired with on `link` (see drv_code_save):
 * saved when a pairing is committed, and the one a re-pairing of that sensor
 * must use. */
int dexble_code_save(int link, const char *mac, const char *code);
int dexble_code_load(int link, const char *mac, char *code,
                     int n); /* 1 if a usable code is on file for this
                              * address (0 once the sensor refused it) */
/* The code a code file holds for the sensor at `mac`, on any link, from
 * memory, for the CODE row and the advert path's candidate test: 1 when a
 * file names that sensor (its code is empty when the sensor refused it). */
int dexble_code_for(const char *mac, char *code, int n);
/* 1 when `link` has a code file naming an address, from memory: the CODE
 * row asks it every frame. */
int dexble_code_file(int link);
/* The address a code was saved for on `link`, into `mac` (18 bytes or more):
 * 1 when one was. */
int dexble_code_owner(int link, char *mac, int n);
/* Ask the OS to bond with `mac` NOW -- the meter pairing flow does, so the
 * system pairing dialog appears as a consequence of the user's tap rather
 * than minutes later when the stack happens to hit an encrypted
 * characteristic. A CGM raises its own request after the J-PAKE. See the
 * "OS-level bonding" comment block in Ble.java for why auto-accepting the
 * dialog is not available to us.
 * Returns 1 if the request went out (or the device was already bonded). */
int dexble_create_bond(const char *mac);
/* For the latest OS bond state seen for an address, include bondtable.h --
 * dexble_bond_state is declared there, beside the setter that feeds it. */
void dexble_reconnect(int link); /* stall watchdog: force a fresh connect */
/* Read one link's Device Information Service (model, firmware, manufacturer).
 * LINK-ADDRESSED, and only that: the ambient no-argument version that assumed
 * LINK_CGM is gone -- a meter's strings are not a sensor's, and provenance is
 * append-only. */
void dexble_request_devinfo_link(int link);
/* Drop one link. The meter driver uses this the moment it has what it needs,
 * so the meter can power itself down rather than being held awake. */
void dexble_link_close(int link);
/* Link-addressed GATT operations (the drv_* hooks wrap these for LINK_CGM). */
void dexble_subscribe(int link, const char *uuid, int indicate);
void dexble_write(int link, const char *uuid, const uint8_t *d, int n,
                  int no_resp);
/* Connect to a OneTouch meter on `link`, and mark that link as carrying a
 * meter so the transport routes its events to the otble driver. */
int dexble_meter_connect(int link, const char *mac);

/* THE NOISES. They are transport operations because the Java side owns the
 * player and the vibrator, not because they have anything to do with BLE. */
int dexble_alarm(int kind, int sound, int vibrate); /* 0 low, 1 high, 2 stale */
void dexble_beep(void); /* one short NEW DATAPOINT beep */
/* One NEW DATAPOINT chirp, pitch-bent by `st10` tenths of a semitone
 * (chirp_semitone10); 0 is the beep's own pitch. */
void dexble_chirp(int st10);
/* One NEW DATAPOINT message in MORSE mode. `elems` is the element string
 * morse_encode produced: '.' a dit, '-' a dah, ' ' a character gap. */
void dexble_morse(const char *elems);
/* One NUDGE: a single two-note motif plus a short buzz, descending for a low
 * crossing and rising for a high one. `kind` is 0 low, 1 high; `sound` and
 * `vibrate` are the nudge's own outputs, independent of the alarm's. */
void dexble_nudge(int kind, int sound, int vibrate);
int dexble_alarm_silence(void);

#endif
