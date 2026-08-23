// SPDX-License-Identifier: GPL-3.0
// readingrec.h --- what ONE reading is, and nothing else
// Copyright 2026 Jakob Kastelic

/* THE RECORD, SPLIT OUT FROM THE STORE THAT HOLDS THEM.
 *
 * This lived in store.h, beside the history it fills, which is where it
 * belongs conceptually and is the wrong place for it structurally: store.h
 * also declares the history's API, so anything that merely wanted to know
 * what a reading LOOKS LIKE had to depend on the whole store -- and store
 * depends on ingest, which depends on alarmlogic. The moment alarmlogic
 * needed the layout (to walk a run of readings and measure a streak) that
 * became a cycle: alarmlogic -> store -> ingest -> alarmlogic, three modules
 * none of which can be read or tested without the other two.
 *
 * A forward declaration is not enough for a caller that indexes an ARRAY of
 * them, which is what the streak walk does. So the type gets a header of its
 * own -- no functions, no dependencies, nothing to include it FROM -- and
 * both sides include that. store.h still presents it as part of its
 * interface; nothing that included store.h needs to change.
 */
#ifndef PANCRA_READINGREC_H
#define PANCRA_READINGREC_H

/* One reading in the display history.
 *
 * glu/trend are narrowed to 16 bits so `src` and `kind` fit in what was
 * padding: the struct stays 16 bytes, so g_hist costs exactly what it always
 * did (2100 x 16 B) despite carrying full attribution. Glucose fits in mg/dL
 * and trend10 in tenths-per-minute with room to spare. */
struct reading {
   short glu, trend;
   /* Sensor id (see sensors.h); 0 = pre-registry legacy. 16 bits, NOT 8: ids
    * are minted for every session and firmware change and never reused, so an
    * 8-bit field wraps after 255 -- and a wrapped id aliases a real one, which
    * would silently reattribute readings to the wrong physical device. */
   unsigned short src;
   unsigned char kind; /* KIND_CGM / KIND_BGM -- decides how it is plotted */
   long t;             /* canonical UTC epoch seconds */
};

#endif
