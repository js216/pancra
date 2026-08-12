// SPDX-License-Identifier: GPL-3.0
// dexdata.h --- Dexcom glucose decoding (API)
// Copyright 2026 Juggluco and xDrip contributors

/* Dexcom G7 / Stelo glucose decoding -- public C API. */
#ifndef DEXDATA_H
#define DEXDATA_H

#include <stddef.h>
#include <stdint.h>

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

int dexdata_record(const uint8_t rec[9], struct dex_record *out);
/* Most 9-byte records one notification can carry: the JNI layer clamps a
 * notification to 256 bytes (dexble.c jni_notify), so 256/9 = 28. Callers
 * size their arrays from this rather than a literal, because decoding fewer
 * than arrive drops backfill points permanently -- a re-request returns the
 * same frame and truncates identically. */
#define DEX_MAX_RECORDS 28

int dexdata_records(const uint8_t *buf, size_t len, struct dex_record *out,
                    int max);
int dexdata_egv(const uint8_t *p, size_t len, struct dex_egv *out);

#endif
