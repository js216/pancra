// SPDX-License-Identifier: GPL-3.0
// dexdata.h --- Dexcom glucose decoding (API)
// Copyright 2026 Juggluco and xDrip contributors

/* Dexcom G7 / Stelo glucose decoding -- public C API. */
#ifndef DEXDATA_H
#define DEXDATA_H

#include <stddef.h>
#include <stdint.h>

/* THE RECORD'S SHAPE, WHICH IS ALL THIS DECODER KNOWS ABOUT SIZE.
 *
 * A record COUNT lived here too, defined as 28 -- 256/9 --
 * where 256 was the byte ceiling app/dexble.c's jni_notify puts on ONE
 * Android notification. That made a reusable decoder's API carry a number
 * belonging to one app's JNI transport: a second transport with a different
 * MTU would silently keep decoding 28, and anyone reading this header would
 * have no way to know why 28. The decoder now knows a record is 9 bytes and
 * nothing else; HOW MANY of them can arrive at once is the transport's fact,
 * declared where that limit is enforced (DEX_NOTIFY_MAX in app/dexport.h),
 * and the CAPACITY is the caller's, passed in. */
#define DEX_RECORD_LEN 9

/* One backfill/EGV record (9 bytes on the wire). */
struct dex_record {
   uint32_t timestamp; /* seconds since session start */
   uint16_t glucose;   /* mg/dL */
   int display_only;   /* high nibble of the glucose field */
   uint8_t status[3];  /* status / trend, not fully decoded */
};

/* Current-EGV control response (opcode 0x4e). */
struct dex_egv {
   uint8_t status_raw;
   uint32_t clock; /* seconds since session start */
   uint16_t sequence;
   uint16_t age;     /* seconds since this reading was taken */
   uint16_t glucose; /* mg/dL */
   int display_only;
   uint8_t state;      /* calibration/session state */
   int8_t trend;       /* trend*10 mg/dL/min; 127 = unavailable */
   uint16_t predicted; /* predicted glucose, 10-bit */
};


/* Decode one 9-byte backfill/EGV record. */
int dexdata_record(const uint8_t rec[9], struct dex_record *out);

/* Decode up to `max` records out of `buf`. THE CAPACITY IS THE CALLER'S: it
 * knows what its transport can deliver in one frame, and sizing an array
 * smaller than that drops backfill points PERMANENTLY -- a re-request returns
 * the same frame and truncates identically. See the array in notify_stream
 * (app/dexproto.c), which is sized from the transport's own ceiling. */
int dexdata_records(const uint8_t *buf, size_t len, struct dex_record *out,
                    int max);
int dexdata_egv(const uint8_t *p, size_t len, struct dex_egv *out);

#endif
