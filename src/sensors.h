// SPDX-License-Identifier: GPL-3.0
// sensors.h --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* Every datapoint must name its origin exactly, forever -- decades after the
 * sensor itself is landfill. That splits into two very different kinds of
 * state, so they live in two different files:
 *
 *   sensors.csv  IMMUTABLE provenance, append-only, never rewritten. One row
 *                per minted id: what the device was, which firmware, which
 *                session. A reading's source_id resolves through this table
 *                and the answer is the same in ten years as it is today.
 *   slots.csv    MUTABLE presentation state, rewritten freely: the user's
 *                label, plot marker, colour, and which sensor owns the big
 *                number. Losing this file costs preferences, never data.
 *
 * The id is a monotonically increasing integer and is NEVER reused, not even
 * after a sensor is forgotten -- readings.csv references it permanently, so
 * recycling an id would silently reattribute old data to a different physical
 * device. Forgetting a sensor drops its slot; the provenance row stays.
 */
#ifndef PANCRA_SENSORS_H
#define PANCRA_SENSORS_H

#define MAX_SLOTS 10 /* user-visible sensors; the UI shrinks this to fit */
/* In-memory tail of sensors.csv. Only sensors with points in the plot window
 * need resolving, so a bounded cache of recent provenance rows is enough. */
#define MAX_SENSOR_RECS 64

/* What protocol a sensor speaks. The type -- not the kind -- decides which
 * driver runs, because Stelo and G7 share a GATT layout but differ in policy.
 */
enum sensor_type {
   SENSOR_NONE = 0,
   SENSOR_STELO,    /* Dexcom Stelo, advertises DX01 */
   SENSOR_G7,       /* Dexcom G7, advertises DXCM, rotating RPA */
   SENSOR_ONETOUCH, /* LifeScan OneTouch BLE meter */
   SENSOR_NTYPES
};

/* How the data behaves, which decides plotting and whether it can be primary.
 */
enum sensor_kind {
   KIND_CGM = 0, /* continuous, 5-min, trend arrow, drawn as a line */
   KIND_BGM,     /* sparse fingersticks, drawn as discrete markers */
   KIND_INS      /* insulin doses: plotted along the bottom edge, in the
                  * user-chosen INSULIN MARKER; excluded from stats and
                  * from the remote push (they are not glucose) */
};

/* MARK_HIDE is not a plot.c shape: it means "do not draw this device's points"
 * and is handled in ui.c by skipping them. Keep it last before MARK_N so the
 * drawable shapes stay 0..MARK_TRIANGLE. */
/* Marker shapes. Values 0..MARK_HIDE are FROZEN (persisted in slots.csv); new
 * variants are appended so old files keep their meaning. Every shape has a
 * filled and an empty version except CROSS. MARK_HIDE means "do not draw". */
enum {
   MARK_DOT = 0,    /* small filled dot */
   MARK_CROSS,      /* X (no fill variant) */
   MARK_SQUARE,     /* empty square */
   MARK_TRIANGLE,   /* empty triangle */
   MARK_HIDE,       /* not drawn */
   MARK_SQUARE_F,   /* filled square */
   MARK_TRIANGLE_F, /* filled triangle */
   MARK_CIRCLE,     /* empty circle */
   MARK_CIRCLE_F,   /* filled circle */
   MARK_N
};

/* Immutable provenance for one minted id. */
struct sensor_rec {
   long activation; /* session start, epoch seconds (0 if unknown).
                     * Recorded, but NOT part of the id reuse key. */
   long paired;     /* when this id was minted */
   int id;
   int type;
   char identity[24]; /* MAC for Stelo/meter; BOND identity addr for G7 */
   char serial[24];   /* empty when the device does not expose one */
   char model[24];    /* DIS model string, e.g. SW11163 */
   char fw[24];
};

/* Mutable per-sensor preferences, keyed by id. */
struct sensor_slot {
   int id;
   int marker;    /* MARK_* */
   int color;     /* index into ui_sensor_colors[] */
   int primary;   /* owns the big number; CGM only, at most one */
   int size;      /* marker size 1..MARK_SIZE_MAX; 0 = unset -> default */
   int wear_days; /* nominal wear override: 10 or 15; 0 = resolve by
                     model/type (sensor_wear_seconds) */
   int old;       /* 1 = DISCONNECTED (an "old device"): the slot and all its
                     preferences are KEPT so the full per-device menu and the
                     plot styling still work, but it is excluded from every
                     LIVE path (reconnect, primary, counts). Reviving it (a
                     re-add) clears this. */
   char label[20];
};

#define MARK_SIZE_DEF 2
#define MARK_SIZE_MAX 5

extern struct sensor_rec g_srec[MAX_SENSOR_RECS];
extern int g_nsrec;
extern struct sensor_slot g_slot[MAX_SLOTS];
extern int g_nslot;
extern char g_sensors_path[256], g_slots_path[256];

/* Registry lock, for callers holding it across a multi-step read of g_slot.
 * Recursive. Lock order is driver -> reg -> hist; reg is a leaf. */
void sensors_lock(void);
void sensors_unlock(void);

void sensors_load(void); /* both files; safe on a fresh install */
void slots_save(void);   /* rewrite slots.csv from g_slot */

/* Resolve a reading's source_id to its provenance, or 0 if it has aged out of
 * the cache (or predates the registry entirely). */
const struct sensor_rec *sensor_rec_by_id(int id);
/* The slot for an id, or 0 if the sensor has been forgotten. */
struct sensor_slot *sensor_slot_by_id(int id);
/* Derived, never stored: the kind follows from the type. */
int sensor_kind(int type);
const char *sensor_type_name(int type);
/* Nominal wear time in seconds, so the UI can show when a session ends.
 * 0 for a meter, which has no session at all. */
long sensor_session_len(int type);
/* The wear length that actually applies to ONE device: the user's per-slot
 * override when set, else the model-derived length (Dexcom sells both 10-day
 * and 15-day G7s and the sensor never states which it is -- the G7 15 Day is
 * only recognisable by its DIS model string), else the type default. Pure. */
long sensor_wear_seconds(int type, int wear_days, const char *model);
/* The post-session grace period (Stelo and G7 both give 12 hours past the
 * nominal end before the sensor hard-stops). The UI counts this down as
 * GRACE once the nominal session is over. */
#define SENSOR_GRACE_S (12L * 3600)

/* Session-state byte from the sensor's 0x4e response (dex_session.state /
 * ui_sensor.sess_state). Values measured from a live HCI capture
 * (2026-07-23), not documentation; 0 means no response seen yet. */
#define SENSOR_STATE_WARMUP 0x02
#define SENSOR_STATE_OK     0x06
#define SENSOR_STATE_ENDED  0x18

/* Warmup: ONE HOUR from session start (sensor clock 0). Measured, not
 * assumed: an HCI capture (2026-07-23) of the official app alongside the
 * reader shows a fresh session answering 4e with state=0x02 and a running
 * clock, and both Dexcom UIs counting warmup down as 3600 - clock. The
 * preferred anchor is therefore the LIVE session clock; the pairing instant
 * is only the estimate used until a first 4e response arrives. */
#define SENSOR_WARMUP_S 3600L

/* How recently a CGM must have delivered to still count as an ACTIVE session
 * when it holds no live bond (right after an app restart no sensor does --
 * sessions re-establish one connect cycle at a time, but the reading history
 * is already loaded from disk). Generous on purpose: a sensor mid-wear with a
 * connectivity gap is still a session the user may want to switch to. */
#define SENSOR_ACTIVE_S (24L * 3600)

/* Find a live slot by address alone, regardless of type. Use this to recognise
 * an already-registered device: keying on type as well lets a stale UI
 * selection re-register one physical sensor under a second, wrong type. */
int sensor_slot_by_mac(const char *identity);

/* Fill `out` (capacity `max`) with the ids of slots whose marker is MARK_HIDE,
 * returning the count. One locked pass, so the plot's scrub path can flag
 * hidden points without a per-point registry lock. */
int sensor_hidden_ids(int *out, int max);

/* Mint an id for a newly paired sensor and append its provenance row. A
 * physical device is identified by its address alone -- serial, model, fw and
 * activation are learned attributes, not identity -- so a repeat mint for a
 * known address returns the existing id, and an id maps to one physical
 * device for life. Returns the id, or -1 on failure. */
int sensor_mint(int type, const char *identity, const char *serial,
                const char *model, const char *fw, long activation);

/* Complete a row's learned attributes once they arrive (DIS strings a few
 * seconds after the first reading; activation once the session clock is
 * known). Fills only fields the row is missing -- an empty string or 0
 * activation -- never overwrites a learned value, so it is idempotent and a
 * late caller cannot clobber an earlier truth. Durable the same way minting
 * is: the corrected row is APPENDED to sensors.csv (the file is never
 * rewritten) and loading is last-row-wins per id. Returns 1 if something was
 * completed, 0 if there was nothing to do, -1 if the append failed (the
 * in-memory row is then left unchanged so the completion retries later). */
int sensor_complete(int id, const char *serial, const char *model,
                    const char *fw, long activation);

/* Give a freshly minted sensor a slot (label defaults to type + MAC tail).
 * Returns the slot index, or -1 when all MAX_SLOTS are taken. */
int sensor_claim_slot(int id, int type, const char *identity);
/* Repoint the slot holding `old_id` at `new_id` and persist, atomically under
 * the registry lock. Returns 1 if a slot was found and updated, 0 otherwise.
 * Use this instead of assigning through a sensor_slot_by_id() pointer: that
 * pointer is an index into an array sensor_forget_slot() shifts. */
int sensor_rebind_slot(int old_id, int new_id);
/* Drop the slot (provenance is untouched, so old readings stay attributed). */
void sensor_forget_slot(int idx);
/* DISCONNECT: retire the slot to "old" instead of dropping it -- keeps its
 * marker/label/prefs and its place in the registry so the full per-device
 * menu and plot styling still work, but excludes it from every live path.
 * Reassigns the primary to the first live CGM left. */
void sensor_retire_slot(int idx);
/* RECONNECT: bring an old slot back to life (clears its `old` flag). */
void sensor_revive_slot(int idx);
/* How many LIVE (non-old) CGM slots exist -- the count that decides
 * multi-CGM behaviour (the primary picker, etc.). */
int sensor_live_cgm_count(void);
/* Make `idx` the primary; clears any other primary. No-op for a BGM. */
void sensor_set_primary(int idx);
/* Index of the primary slot, or -1. Unlocked -- for main-thread UI use. */
int sensor_primary_slot(void);
/* The primary sensor's id, or -1, resolved under the registry lock. Use this
 * (not sensor_primary_slot) from any path that also takes hist_lock: resolve
 * BEFORE taking hist_lock, so the reg->hist order is preserved. */
int sensor_primary_id(void);

#endif
