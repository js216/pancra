// SPDX-License-Identifier: GPL-3.0
// dexport.h --- what the Dexcom driver needs FROM its host, and nothing else
// Copyright 2026 Jakob Kastelic

/* THE OTHER DIRECTION.
 *
 * app/dexdriver.h is what a CALLER asks of the driver: connect this link,
 * tell me about that session, arm a meter, queue a calibration. This header
 * is the reverse -- what the driver asks of whoever is hosting it: put these
 * bytes on the air, subscribe to that characteristic, keep this key, say this
 * on the screen.
 *
 * TWO DIRECTIONS, TWO HEADERS, and the split is not tidiness. Every module
 * that merely asks the driver something -- the menus, the model, the alarm,
 * the reconciler, a dozen files -- was being handed the transport's own
 * contract as well: the GATT characteristic UUIDs, the key and address files,
 * the callbacks only dexble.c and the test harnesses implement. A header that
 * declares both directions makes every reader decide which half applies to
 * them, and makes a change to the port look like a change to the API.
 *
 * WHO INCLUDES THIS: app/dexproto.c and app/dexlink.c (which call these),
 * app/dexble.c (which
 * implements them), and the test harnesses that stand in for the transport.
 * Nothing else -- and `make lockcheck` refuses it elsewhere.
 *
 * Include app/dexdriver.h first: a link number means what it means there. */
#ifndef PANCRA_DEXPORT_H
#define PANCRA_DEXPORT_H

#include "compiler.h" /* PANCRA_MUST_USE: the annotation, portably */

#ifndef DEXDRIVER_H
#error "app/dexport.h: include app/dexdriver.h first"
#endif

#include "dexdata.h" /* DEX_RECORD_LEN: the shape the ceiling divides by */
#include <stdint.h>

/* ---- WHAT ONE NOTIFICATION CAN CARRY ---------------------------------
 *
 * This number lives HERE, with the port, because it is a property
 * of the transport and of nothing else: app/dexble.c's jni_notify copies at
 * most this many bytes out of the Java byte[] into a stack buffer, and a
 * frame longer than this is truncated before the driver ever sees it. It used
 * to live in app/dexdata.h -- the reusable decoder -- as a record COUNT
 * (28 = 256/9), which put one app's JNI limit inside a decoder that has no
 * transport at all, and made the count the thing to reason about rather than
 * the byte ceiling it came from.
 *
 * A transport that can deliver more must raise this AND its own clamp
 * together; dexble.c has a _Static_assert tying the two, so they cannot drift.
 *
 * THE DERIVED RECORD CEILING is what a decoding caller sizes its array from
 * (see notify_stream in app/dexproto.c). Sizing it smaller than the transport
 * can deliver drops backfill points PERMANENTLY: a re-request returns the
 * same frame and truncates identically, so gap recovery quietly loses exactly
 * the points it exists to recover. */
#define DEX_NOTIFY_MAX     256
#define DEX_NOTIFY_RECORDS (DEX_NOTIFY_MAX / DEX_RECORD_LEN)

/* Dexcom GATT characteristic UUIDs. */
#define U_CTRL  "f8083534-849e-531c-c594-30f1f86a4ea5"
#define U_AUTH  "f8083535-849e-531c-c594-30f1f86a4ea5"
#define U_DATA  "f8083536-849e-531c-c594-30f1f86a4ea5"
#define U_ROUND "f8083538-849e-531c-c594-30f1f86a4ea5"

/* ---- provided BY the transport layer (dexble.c / test harness) ---- */
void drv_connect(int link, const char *mac);
void drv_subscribe(int link, const char *uuid, int indicate);
void drv_write(int link, const char *uuid, const uint8_t *data, int n,
               int no_resp);
void drv_status(const char *s);

/* ONE LIVE READING, AND WHETHER THE APP KEPT IT.
 *
 * Returns 1 when the reading entered the authoritative history -- past the
 * value and age gate in ingest.c, past per-link source attribution, and
 * inserted (not deduplicated away) by store_record -- and 0 when it did not.
 *
 * The return value is the whole reason this is not `void`. Setting `streamed`
 * and persisting the sensor's address the moment a 0x4e DECODES is a
 * statement about the wire, not about the app: a frame carrying a 5,000 mg/dL
 * value, or an age of 65535 seconds, or one arriving on a link no registered
 * slot claims yet, is refused downstream and never becomes a reading. To the
 * person holding the phone that connection produced NOTHING -- the number on
 * screen did not move and the plot gained no point -- so a driver counting it
 * as a success clears the failure streak that exists to notice a sensor going
 * bad and writes the address into the file that decides which sensor future
 * launches reconnect to. A sensor that can decode but whose every value is
 * rejected would be adopted permanently on the strength of nothing.
 *
 * The hook is declared warn_unused_result FOR THAT REASON: a caller that
 * drops the answer is back to trusting the decode, and the compiler is the
 * only thing that reliably notices. */
#if defined(__GNUC__) || defined(__clang__)
#define DRV_MUST_USE PANCRA_MUST_USE
#else
#define DRV_MUST_USE
#endif
DRV_MUST_USE int drv_glucose(int link, int mg_dl, int trend,
                             int age_s); /* live reading; 1 = accepted */
/* Outcome of a calibration write we sent: 0 accepted, >0 rejected by the
 * sensor. Lets the shell clear or surface its durably-queued calibration.
 *
 * THE TOKEN OF THE WRITE THAT WAS ANSWERED TRAVELS WITH THE ANSWER. One
 * BOOLEAN -- "a calibration we sent is awaiting a reply" -- would leave the
 * shell resolving whatever calibration was queued at the moment the reply
 * landed, and those are not the same calibration whenever the user has
 * changed their mind:
 *
 *   the user takes a fingerstick, types 100, confirms; the driver writes 0x34
 *   for 100. Before the sensor answers -- the reply is one connection interval
 *   away at best, and a whole reconnect away at worst -- they realise they
 *   misread the meter, cancel, and type 180. The sensor's answer to the 100
 *   arrives, ACCEPTED, and the shell marks the 180 accepted. The row says
 *   "LAST CAL 180 APPLIED". The sensor holds 100, and will go on reporting
 *   against 100 until the next calibration. The one number the user is
 *   entitled to believe -- what the sensor was actually told -- is wrong on
 *   the screen, and a CGM calibrated to a value nobody chose misreports
 *   glucose for as long as the session lasts.
 *
 * So a transmitted calibration is identified by the three things that make it
 * the one it is: the SENSOR it was for, the VALUE written, and the queue
 * GENERATION it came from. The generation is a counter the calibration module
 * bumps on every queue (it is NOT a clock -- see the deadline block in
 * calib.c; a clock would make the identity of a write depend on an NTP step),
 * so re-queueing the same value for the same sensor is a different write and
 * is told apart from the first. All three come back here, and the shell
 * resolves ONLY on an exact match with what is queued RIGHT NOW. Anything
 * else is discarded, loudly. */
void drv_cal_result(int link, int result, int sensor_id, int mg_dl,
                    unsigned gen);
/* One recovered older reading. 1 when it entered the authoritative history,
 * exactly as for drv_glucose above -- and for the same reason: a backfill
 * batch of 28 records every one of which is refused is not a batch that
 * proves the sensor is streaming. */
DRV_MUST_USE int drv_backfill(int link, int mg_dl, int trend,
                              int age_s);    /* 1 = accepted */
int drv_key_load(int link, uint8_t key[16]); /* 1 if a key was loaded */
int drv_key_save(int link, const uint8_t key[16]);
/* DELETE the stored key (force a re-pair). 0 when it is gone and the
 * directory entry is durable, -1 when it may still be there -- a file that
 * survives is a sensor the next launch silently reconnects to after the user
 * asked to forget it, so the caller must not claim otherwise.
 * A file that was never there is success. */
DRV_MUST_USE int drv_key_clear(int link);
int drv_mac_load(int link, char *mac,
                 int n); /* bonded sensor's MAC; 1 if one was saved */
int drv_mac_save(int link, const char *mac); /* the sensor we bonded to */
DRV_MUST_USE int drv_mac_clear(int link);    /* forget it: see drv_key_clear */

#endif
