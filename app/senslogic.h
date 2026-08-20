// SPDX-License-Identifier: GPL-3.0
// senslogic.h --- Sensor/session reconciliation decisions (host-testable)
// Copyright 2026 Jakob Kastelic
//
/* WHAT RECONCILIATION DECIDES, split out from what it DOES.
 *
 * sensor_reconcile() walks every BLE link once a second and answers four
 * questions about each one. Answering them wrongly is uniquely expensive here,
 * because every "yes" writes the APPEND-ONLY provenance file: a row minted
 * with the wrong type, a truncated firmware, or an activation that is really
 * an elapsed duration is never rewritten and never corrected. Several of those
 * mistakes have actually shipped (each is spelled out at its rule below).
 *
 * The answers used to be `if` conditions threaded between the driver's lock
 * and the registry's, interleaved with GATT reads and file I/O, so no test
 * could
 * reach them without a live sensor. They are pure decisions and they belong
 * here, in the shape the other workflows already use (alarmlogic, scanlogic,
 * meterlogic): main.c observes, this module decides, main.c acts.
 *
 * Pure: no globals, no clock, no JNI, no locks. test/senstest.c pins it.
 */
#ifndef SENSLOGIC_H
#define SENSLOGIC_H

#include <stdint.h>

/* HOW FAR A SESSION CLOCK MAY BE PROJECTED WITHOUT A FRESH RESPONSE.
 *
 * The sensor answers every ~5 minutes and the countdowns built on its clock
 * have to tick per second in between, so the clock is projected forward from
 * the last answer. Past a day of silence that projection is not a countdown,
 * it is fiction: the link has been down for a wear's worth of cycles and the
 * number would still be advancing on screen. Matching SENSOR_ACTIVE_S, which
 * is the same judgement made about the cached copy in sesscache.c -- stated
 * here rather than included from sensors.h, because this module is pure and
 * sensors.h drags in the registry. */
#define SENS_PROJECT_MAX_S (24L * 3600)

/* PROJECT A SENSOR SESSION CLOCK FORWARD, from the receipt of the response
 * that carried it to now. BOTH STAMPS ARE MONOTONIC (mono_s), and that is the
 * whole point of this function existing.
 *
 * WHAT IT REPLACED, and what that did to the person holding the phone: the
 * driver added, to the sensor's last reported clock, the difference between
 * realtime_s() now and a realtime_s() stamp taken when that clock arrived --
 * cast to uint32_t.
 *
 * Two wall-clock stamps. A phone that comes back from being off, or finds a
 * network and corrects itself, moves realtime_s() by minutes or hours in
 * either direction while the sensor's session goes on exactly as before. A
 * BACKWARD correction of an hour makes the difference -3600; the cast turns
 * that into 4294963696 and the unsigned add wraps, so the projected clock
 * jumps BACKWARD by an hour -- or, when the session is younger than the
 * correction, wraps to ~4.29 billion seconds.
 *
 * Both outcomes are visible and wrong. On the device screen the warmup
 * countdown is `SENSOR_WARMUP_S - session_seconds`: a sensor twenty minutes
 * into its hour reads as finished the instant the clock is corrected back,
 * so the app announces data that will not arrive for forty more minutes.
 * cgm_expired() asks `session_seconds > wear_len + grace`: the wrapped value
 * clears any budget, so the same sensor is simultaneously declared EXPIRED --
 * and in the other direction, a session repeatedly pushed an hour back is
 * never judged finished at all, so a sensor that stopped reporting days ago
 * still owns the big number.
 *
 * The delta is clamped at both ends: never negative (a monotonic clock cannot
 * go back, so a negative difference means the stamp was never set or was read
 * torn, and freezing the clock is the only honest answer), and never more
 * than SENS_PROJECT_MAX_S. The sum saturates rather than wrapping, because a
 * uint32 session clock plus a bounded delta is the one arithmetic this whole
 * function exists to keep off the screen.
 *
 * last_clock == 0 means no response has ever been decoded; a session that has
 * not started projects to nothing, not to the elapsed time since boot. */
uint32_t sens_project_clock(uint32_t last_clock, long recv_mono, long now_mono);

/* Seconds between session-cache writes. sessc_put marks the cache dirty from
 * the DRAW path, which runs far more often than the 5-minute cadence that
 * actually changes anything; without this a redraw storm becomes a write
 * storm. Losing up to a minute costs nothing -- the stored clock is projected
 * forward from whatever instant it holds. */
#define SENS_FLUSH_MIN_S 60

/* The session cache's whole state: has it changed, and when was it written. */
struct sens_cache {
   int dirty;
   long saved;
};

/* A change landed. */
void sens_cache_touch(struct sens_cache *c);
/* Should it be written now? */
int sens_cache_due(const struct sens_cache *c, long now);
/* A write succeeded at `now`. Only call this on success -- a failed write must
 * stay dirty, or the cache is silently dropped. */
void sens_cache_done(struct sens_cache *c, long now);

/* ONE LINK, in decision terms. main.c fills this from the driver session, the
 * link's own DIS strings and the registry row that claims its address. */
struct sens_obs {
   int is_cgm;       /* the link carries a CGM, not a meter */
   int has_mac;      /* a peer address is known */
   int bonded;       /* authenticated */
   int have_reading; /* an EGV has been decoded, so the session clock is real */
   int claimed;      /* some slot already claims this address */
   int registered;   /* ...and its provenance row was found */
   int have_dis;     /* BOTH model and firmware have landed on THIS link */
   int row_bare;     /* the claiming row is missing model or firmware */
   int row_no_act;   /* the claiming row has no activation instant */
   long session_seconds; /* the live session clock */
};

/* What to do about that link. */
struct sens_effect {
   int mint;         /* register this sensor: it is bonded and unclaimed */
   int complete_mfw; /* write model+firmware into an already-registered row */
   int complete_act; /* write the activation instant into it */
   /* The epoch the session STARTED. Valid when mint or complete_act is set. */
   long activation;
};

/* Decide for one link. */
void sens_link_eval(const struct sens_obs *o, long now, struct sens_effect *e);

/* ONE SLOT, for the provenance-stamp decision. */
struct sens_slot_obs {
   int id;      /* the slot's sensor id (>0) */
   int old;     /* retired: no live session to reconcile */
   int is_cgm;  /* the slot's registered type is a CGM */
   int live;    /* its link carries a bonded session for THIS slot's address */
   int primary; /* the user marked it primary */
};

/* Which sensor id should stamp readings that carry no source of their own.
 *
 * Returns the id, or -1 for "leave it alone" -- never 0, which means
 * "pre-registry legacy" in a log that is never rewritten.
 *
 * PREFER THE PRIMARY, AND STOP AT IT. Without that this walked to the end and
 * left the stamp on whichever bonded CGM sat highest in the slot table, so a
 * second sensor's readings were stamped with the first's id -- and per-source
 * dedup then silently discarded samples that collided within 150 s. */
int sens_primary_pick(const struct sens_slot_obs *slots, int n);

#endif
