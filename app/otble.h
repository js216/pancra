// SPDX-License-Identifier: GPL-3.0
// otble.h --- LifeScan OneTouch BLE meter driver (API + transport hooks)
// Copyright 2026 Jakob Kastelic

/* Driver for the BLE OneTouch meters (Verio Flex / Reflect / Select Plus Flex
 * / Ultra Plus Flex). Like dexdriver.c this has NO Android/JNI dependency: the
 * transport supplies the ot_* hooks below, so the protocol can be exercised on
 * the host.
 *
 * A meter is not a CGM. It is only reachable while the user has it switched on
 * and advertising, and connecting keeps it awake past its own auto-power-off,
 * so the policy is: connect when it advertises, ask for the record counter,
 * and disconnect immediately if there is nothing new. Never poll -- a meter
 * that is off costs us nothing and must stay that way. */
#ifndef OTBLE_H
#define OTBLE_H
#include <stdint.h>

/* GATT: the meter does NOT use the standard Glucose Service 0x1808. */
#define OT_SVC    "af9df7a1-e595-11e3-96b4-0002a5d5c51b"
#define OT_WRITE  "af9df7a2-e595-11e3-96b4-0002a5d5c51b"
#define OT_NOTIFY "af9df7a3-e595-11e3-96b4-0002a5d5c51b"

/* ---- provided BY the transport ---- */
void ot_drv_write(const uint8_t *data, int n);
void ot_drv_subscribe(void);
void ot_drv_disconnect(void);
void ot_drv_status(const char *s);
/* One accepted reading. `naive` is the meter's own clock (seconds since
 * 2000-01-01 with no zone); the host converts it using the offset that was in
 * force at import and persists both. */
/* Returns 0 if the host REFUSED the record as implausible -- which for a
 * timestamp means the phone's own clock may be at fault, so the driver must
 * not persist its walk past it. Returns 1 when the record was accounted for,
 * including when it deduplicated against one already stored. */
int ot_drv_reading(long naive, int mg_dl);
void ot_drv_done(int new_records);

/* ---- driver API ---- */
void ot_init(int last_index); /* highest record index already stored */
int ot_last_index(void);
void ot_on_connected(void);
void ot_on_disconnected(void);
void ot_on_notify(const uint8_t *buf, int n);

/* Seconds between the meter's epoch (2000-01-01) and the Unix epoch. Exposed
 * so the host can convert: utc = naive + OT_EPOCH - tz_offset. */
#define OT_EPOCH 946684800L

/* Most records to pull in one session. Caps how long a single connection can
 * hold the meter awake, whatever its record counter says. */
#define OT_MAX_WALK 20

#endif
