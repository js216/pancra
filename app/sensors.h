// SPDX-License-Identifier: GPL-3.0
// sensors.h --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* Every datapoint must name its origin exactly, decades after the sensor
 * itself is landfill.
 *
 * "FOREVER" IS WHAT THIS SAID, and it is a promise with a number behind it
 *: the provenance table holds MAX_SENSOR_RECS devices, and the
 * block above that constant is the whole argument for where the bound is,
 * why it is not reachable before the phone is landfill, and why the table
 * REFUSES a new device rather than forgetting an old one when it is. A
 * contract that says "forever" and means "2048 devices" is the kind that gets
 * read once and relied on for years, so it says the number.
 *
 * WHAT A REFUSAL COSTS, PRECISELY: the new sensor shows as unregistered, and
 * a reading arriving on its link is DEFERRED rather than attributed (see
 * app/reading.c -- stamping it with an ambient id is how a retired sensor
 * comes to own somebody else's data). Nothing
 * already attributed is affected: every id the table holds still resolves,
 * and it resolves to its own row.
 *
 * The state splits into two very different kinds, so they live in two files:
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

/* EVERY ID sensors.csv HAS EVER NAMED, HELD AT ONCE -- and nothing is ever
 * evicted from this table. Its size is the whole of that promise, so the
 * number below is argued rather than picked.
 *
 * IT WAS 64, with the note "only sensors with points in the plot window need
 * resolving, so a bounded cache of recent provenance rows is enough". That
 * premise was false. readings.csv is append-only and EVERY row cites a
 * source_id, so a citation outlives the plot window by years -- the reason to
 * keep the log at all is that a reading taken in 2026 can still say which
 * physical sensor produced it in 2036. Dropping "the oldest record no LIVE
 * SLOT references" evicts exactly the set of ids that only HISTORY cites: the
 * row stays on disk and the app simply stops being able to resolve it.
 *
 * WHEN IT BIT, exactly. A CGM session mints one id (a new sensor advertises a
 * new address), so a Stelo (15 d) and a G7 (10 d) worn together plus a meter
 * is ~61 ids a year. Slots pin at most MAX_SLOTS rows, so the table kept the
 * newest (64 - slots) unslotted rows: about ONE YEAR of history, after which
 * every older id became permanently unresolvable, and the horizon moved
 * forward for as long as the app was used.
 *
 * WHAT THAT COST, honestly, because it is worth writing down which of these
 * were reachable and which were waiting:
 *
 *   - sensor_mint's identity scan reads these rows to recognise an address it
 *     has seen before. A device the user FORGOT and re-paired a year later
 *     was no longer recognised, so it was minted a SECOND id -- one physical
 *     sensor with two identities, its history split in a log that is never
 *     rewritten. Reachable by anyone, with one tap.
 *   - sensor_complete fills a bare row's model, firmware and session start
 *     when they arrive. A live sensor with no slot (slots fill at MAX_SLOTS)
 *     whose row was dropped could never be completed, so it would stay bare
 *     for ever. Reachable once the user owns ten devices.
 *   - sensor_in_warmup FAILS OPEN on an id it cannot resolve, and stats.c
 *     asks it for every historical row: an unresolvable sensor's uncalibrated
 *     first hour then COUNTS towards time-in-range and the daily average, at
 *     every launch. stats.c reaches back STAT_HOURS (~92 days) and g_hist
 *     ~9-17 days, both inside a one-year horizon -- so at a typical mint rate
 *     the exposure is latent. At ~5x that rate (six concurrent CGMs, or
 *     sensors
 *     failing early and being replaced) the horizon falls under 92 days and
 *     it starts moving the numbers on the front screen.
 *
 * The last one is the shape of the whole defect: nothing was wrong with the
 * READERS, and the log kept every row. The app had simply decided in advance
 * how far back it was willing to be able to answer -- and an append-only log
 * exists precisely so that limit does not have to exist.
 *
 * THE BOUND BEING ACCEPTED: 2048 rows x 120 bytes = 240 kB of BSS. This
 * library already reserves ~7.7 MB, of which one renderer scratch buffer is
 * 1.5 MB, so this is 3% of what is already there -- and it buys 20 years at
 * the pessimistic rate, 34 at the realistic one, on ONE install of ONE phone
 * whose readings.csv would by then hold some five million rows. "Unbounded"
 * was not on offer: this path runs on a phone, with no allocator behind it.
 * A bound that cannot be reached before the hardware is landfill is.
 *
 * PAST IT THE TABLE REFUSES, IT DOES NOT FORGET: the row is not taken,
 * sensors_load() reports the file as incompletely read (so the user is told),
 * and sensor_mint() fails -- a new sensor then shows as unregistered, which
 * is visible, rather than quietly taking a dropped id's place. That is the
 * same treatment sensors.c already gives the 0xFFFF id ceiling: unreachable
 * in practice, but an invariant rather than an estimate.
 *
 * WHY NOT COMPACT THE FILE INSTEAD (the other route open here): sensors.csv
 * is append-only precisely so that no partial write can lose a row, and a
 * compaction is a rewrite. The file is already last-wins per id, so the only
 * thing a compaction could reclaim is superseded sensor_complete corrections
 * -- at most a few rows per sensor -- while putting every id that has ever
 * existed through one non-atomic rewrite. Memory is the cheaper thing to
 * spend.
 *
 * THE BOUNDED THING IS THE UI SNAPSHOT, not the attribution: struct
 * sensor_view below is MAX_SLOTS wide because the user can only own ten
 * devices at once, and that is the only place a cap may cost anything. */
#define MAX_SENSOR_RECS 2048

/* The marker shapes and the palette size a slot's fields are bounded against.
 * A leaf both this and settings.h can name: see style.h for why neither owns
 * it. */
#include "style.h"

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
   KIND_INS,     /* insulin doses: plotted along the bottom edge, in the
                  * user-chosen INSULIN MARKER; excluded from stats and
                  * from the remote push (they are not glucose) */
   KIND_FOOD,    /* logged food: same bottom line as the doses and weights,
                  * drawn as a small F. Like them it never enters g_hist, so
                  * it cannot reach TIR, the average or the remote push. */
   KIND_WT,      /* logged body weights: same bottom line as the doses, drawn
                  * as a small W. Like KIND_INS these never enter g_hist, so
                  * they cannot reach TIR, the average or the remote push --
                  * they exist only in the plot model the UI is handed. */
   KIND_EX       /* logged exercise: the same bottom line, drawn as a small E.
                  * The ONE kind that is not an instant -- a session has a
                  * length, and the point carries it so the plot can draw a
                  * rule to where it ended. Its `glu` is the INTENSITY (1..3)
                  * and its `src` the length in seconds; neither is a glucose
                  * value, which is why these are pinned to the bottom line
                  * and read back out of the model by the scrub, exactly as
                  * the food and weight points are. */
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

/* THE REGISTRY IS PRIVATE. Export the two arrays and their counts and every
 * caller depends on the representation and can write to it -- and a slot
 * written by hand is a device the user sees whose change was never saved, or
 * a provenance row that contradicts the append-only file it came
 * from. An id names one physical device FOR EVER (readings cite it in a log
 * that is never rewritten), which is why this of all state is behind
 * accessors.
 *
 * COPIES, not pointers: a binder thread minting a sensor shifts the record
 * array, so a pointer held across that is a row that has moved.
 *
 * ONE CALL, ONE ANSWER. Each of these locks for itself; a WALK takes a
 * snapshot (sensors_view_get above) rather than calling them in a loop, so no
 * caller has to hold a lock -- or know which one. */


/* (THE INDEXED READS ARE NOT HERE. A count-then-index pair is two locked
 * questions about a moving table, and no production caller may ask them --
 * see app/sensorsint.h, which the registry's own tests include.)
 *
 * The two files, for the sync client that must NAME them. Read-only: they are
 * set once, by sensors_paths. */
const char *sensors_path(void);
const char *slots_path(void);
/* Point it at the data directory; the filename lives here. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int sensors_paths(const char *dir);

/* EVERY SLOT AND ITS PROVENANCE, AT ONE INSTANT.
 *
 * A PUBLIC lock, taken by hand around a count/index walk in a dozen files,
 * is wrong three ways, and the third is the reason this exists:
 *
 *   - a walk without it can see the array shift under it: srec_push memmoves
 *     the records when a binder thread mints a sensor, and sensor_forget
 *     shifts the slots;
 *   - "which lock, and for how long" becomes a question every caller has to
 *     answer correctly, at every call site, for as long as the code lives;
 *   - the documented order is driver -> registry, and a caller holding the
 *     registry lock across link_for_slot (which takes the driver's) inverts
 *     it. A snapshot cannot: the lock is gone before anything else is called.
 *
 * One copy, taken under one hold. `have_rec[i]` says whether slot i's id has
 * a provenance row; `rec[i]` is only meaningful when it does.
 *
 * THIS IS THE THING THAT IS BOUNDED. It is MAX_SLOTS wide because the user
 * can own at most ten devices at once, so nothing is lost by capping it --
 * whereas capping the ATTRIBUTION table behind sensor_rec_of costs the app
 * the ability to say where a historical reading came from. See
 * MAX_SENSOR_RECS above for why those two caps must not be the same idea. */
struct sensor_view {
   int n;
   struct sensor_slot slot[MAX_SLOTS];
   struct sensor_rec rec[MAX_SLOTS];
   int have_rec[MAX_SLOTS];
};

void sensors_view_get(struct sensor_view *out);

/* Does any slot hold this id -- i.e. is the device still one of the user's?
 * (A retired "old" slot counts: it keeps its place and its preferences.) */
int sensor_id_is_live(int id);

/* 0 when the registry was read whole (including a first run with no files),
 * -1 when a read failed partway. Whatever parsed is kept; the caller warns. */
int sensors_load(void); /* both files; safe on a fresh install */

/* Resolve a reading's source_id to its provenance. 0 only when NO ROW EXISTS
 * -- the id predates the registry (source 0), was never minted, or its row
 * was refused by the parser. A row that loaded once stays resolvable for the
 * life of the process: nothing ages out, because readings.csv keeps citing
 * these ids long after the sensor is gone. See MAX_SENSOR_RECS. */
/* THE PROVENANCE FOR AN ID, AS A COPY, never a pointer into the record cache
 * -- that is the one thing that must not leave this module: srec_push memmoves
 * that array when a binder thread mints a sensor, so a pointer held across any
 * call is a row that has MOVED, and the frame builder holds one across a run
 * of field copies. Returns 1 when the id is known; `out` may
 * be NULL to ask only whether it is. */
int sensor_rec_of(int id, struct sensor_rec *out);
/* The slot for an id, or 0 if the sensor has been forgotten. */
/* THE SLOT FOR AN ID, AS A COPY -- and its index, for the operations that
 * take one. A writable pointer here would let any caller change a device's
 * name, marker, colour or primary flag with none of the validation and none
 * of the persistence: the change shows on screen and is gone at the next
 * launch. Every mutation goes through a sensor_* operation.
 * Returns 1 when the id has a slot. */
int sensor_slot_of(int id, struct sensor_slot *out);
/* Derived, never stored: the kind follows from the type. */
int sensor_kind(int type);
const char *sensor_type_name(int type);

/* THE WEAR LENGTH AND WHERE IT CAME FROM, as one answer.
 *
 * `seconds` is what actually applies to ONE device: the user's per-slot
 * override when set, else the model-derived length (Dexcom sells both 10-day
 * and 15-day G7s and the sensor never states which it is -- the G7 15 Day is
 * only recognisable by its DIS model string), else the type default.
 *
 * `pinned` is whether that number came from the OVERRIDE, and it is part of
 * this answer rather than something a caller re-derives. The WEAR row shows
 * AUTO or a pinned value in a different colour, and the two behave
 * differently over time: a resolved length improves when a new model is
 * recognised, a pinned one never does. A caller that decides which it was by
 * re-testing `wear_days` against the values this function happens to accept
 * TODAY is a second copy of the rule -- and the day a third override becomes
 * valid, that copy labels a correct duration as automatic.
 *
 * Pure: no locks, no clock, no registry lookup. */
struct sensor_wear {
   long seconds;
   int pinned; /* 1 = the user's override, 0 = resolved from model or type */
   /* 1 = the TYPE's default standing in for a model that has not been read.
    * Dexcom sells the G7 in 10- and 15-day versions that are identical on the
    * air apart from the DIS model string, so between pairing and the first
    * DIS read there is no honest answer -- only a default. The row that shows
    * the budget marks it, because a countdown judged against a guess and one
    * judged against the sensor's own model look the same on screen. */
   int provisional;
};
struct sensor_wear sensor_wear_of(int type, int wear_days, const char *model);
/* The length alone, for callers that only do arithmetic with it. */
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

/* Does reading `t` from sensor `id` fall in that sensor's WARMUP hour?
 *
 * Warmup glucose is REAL but uncalibrated -- the sensor reports it with
 * state=0x02 and the official Dexcom UIs hide it outright. Pancra shows it (it
 * is the user's data and the WARMUP label says what it is) but must not COUNT
 * it: an uncalibrated first hour skews TIR and the average, and a fresh sensor
 * reading far from truth would otherwise drag the day's numbers with it.
 *
 * The anchor is sensor_rec.activation -- the session start, learned from the
 * session clock and persisted in sensors.csv -- NOT the pairing instant, which
 * is only an estimate, and NOT the live state byte, which no replayed row
 * carries. That choice is what lets the LIVE path and stat_load_chunk apply the
 * identical rule; anything the CSV cannot express would make TIR and the
 * average change across a restart of the same log, the drift stats.c records
 * having been fixed twice.
 *
 * FAILS OPEN, by design: an unknown id or an activation still 0 (session start
 * never learned) counts the reading. A reading that cannot be proven to be
 * warmup is ordinary data, and silently dropping it would be the worse error.
 * The `t >= activation` bound is what makes a stale activation harmless -- a
 * physical CGM is one session in practice (a new sensor advertises a new
 * address and mints a new id), but sensor_complete never overwrites a learned
 * value, so a reused device keeps its FIRST session's activation and every
 * later reading simply falls outside the window and counts.
 *
 * Takes the registry lock itself. The lock order in this codebase is
 * registry -> history, so callers on the live path MUST call this BEFORE
 * hist_lock(), alongside sensor_primary_id(), never inside it. */
int sensor_in_warmup(int id, long t);

/* ---- WHAT THE SENSOR SAID ABOUT ITS OWN WARM-UP ------------
 *
 * The rule above is an INFERENCE: it compares a reading's timestamp against a
 * session start learned from the session clock. The sensor also answers the
 * question directly -- every 0x4e response carries a state byte, and
 * SENSOR_STATE_WARMUP is the sensor saying so itself -- and that answer was
 * thrown away the moment the frame was decoded, so a replayed row could only
 * ever be judged by the inference.
 *
 * The two differ where it matters. An activation of 0 (a session whose start
 * was never learned -- a sensor paired mid-session, a registry row from
 * before the field existed) makes the inference count an uncalibrated first
 * hour into time-in-range and the average. That is a decision to include data
 * on the strength of not knowing.
 *
 * So the measurement is stored with the row, and this enum is what a row can
 * say. UNKNOWN is not a third kind of warm-up: it is the absence of a
 * measurement, and it is what every row written before this column existed,
 * and every reading that arrived before the sensor answered 0x4e, carries. */
enum warm_state {
   WARM_UNKNOWN = 0, /* nothing was measured; the inference is all there is */
   WARM_NO,          /* the sensor answered, and it was not warming up */
   WARM_YES          /* the sensor answered SENSOR_STATE_WARMUP */
};

/* What the statistics should do with a reading. */
enum warm_verdict {
   WARM_COUNT,        /* count it, and the answer is measured */
   WARM_COUNT_UNSURE, /* count it, but nothing measured said it was not warmup
                       */
   WARM_SKIP          /* uncalibrated: it is shown, and it is not counted */
};

/* THE ONE RULE, and both paths must call it: the live reading as it arrives,
 * and the same row when stat_reload_prepare replays it. Anything a row cannot
 * express would make TIR and the average change across a restart of the same
 * log -- the drift stats.c records having been fixed twice.
 *
 * A measured state is believed. Without one the inference decides, and a
 * reading it lets through is COUNTED BUT UNSURE, so the coverage figures can
 * say how much of a window rests on it rather than on a measurement.
 *
 * Takes the registry lock when it consults the inference, so the same
 * lock-order rule as sensor_in_warmup applies: call it BEFORE hist_lock. */
enum warm_verdict warm_decide(enum warm_state measured, int id, long t);

/* The state byte a 0x4e response carried, as a stored measurement. An
 * unrecognised byte is UNKNOWN: a firmware this build has never seen is not
 * evidence about warm-up in either direction, and 0 is "no response yet". */
enum warm_state warm_of_state(int state);

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

/* ---- WHAT A REGISTRY CHANGE ANSWERS ------------------------------------
 *
 * Every mutation below is a TRANSACTION: it changes the slot table, rewrites
 * slots.csv, and if that rewrite fails it puts the table back exactly as it
 * was. So there are only two outcomes, and the caller can act on either.
 *
 * NOT `void`. With the rewrite's result discarded, a full disk or a dying
 * card gives a device that is retired on screen and live again after the next
 * launch -- or, worse, a sensor CLAIMED, bonded and streaming whose slot was
 * never written, so the pairing has to be done again with the key file
 * already replaced. The screen says it worked. Nothing says otherwise.
 *
 * SENSOR_UNSAVED means NOTHING CHANGED: not in memory, not on disk. That is
 * what makes it safe to act on -- a caller that stops has lost nothing, and a
 * caller that retries is not retrying half an operation. It is possible
 * because slots.csv is replaced by rename: a failed write leaves the previous
 * file whole, so rolling the table back restores agreement rather than
 * inventing it. */
#define SENSOR_OK      0
#define SENSOR_UNSAVED (-1)

/* ---- ACTING ON ONE DEVICE: BY ID, NEVER BY INDEX ------------------------
 *
 * Every operation below names the device by its permanent id and resolves it
 * to a slot INSIDE the lock it changes the slot under. That is the whole
 * point: an index is a POSITION, and a mint or a forget on a binder thread
 * moves every position after it. A caller that read "the selected device is
 * slot 2" and then asked to make slot 2 primary -- or to DISCONNECT it --
 * could name a different sensor by the time the call landed, and the user
 * would watch the wrong device change colour, get renamed, or drop off the
 * plot. Capture the id once (sensor_slot_at / sensors_view_get) and pass
 * that; `make lockcheck` refuses a slot index handed straight to any of
 * these. An unknown id changes nothing and says so. */

/* Drop the slot (provenance is untouched, so old readings stay attributed). */
int sensor_forget(int id);
/* DISCONNECT: retire the slot to "old" rather than dropping it -- keeps its
 * marker/label/prefs and its place in the registry so the full per-device
 * menu and plot styling still work, but excludes it from every live path.
 * Reassigns the primary to the first live CGM left. */
int sensor_retire(int id);
/* RECONNECT: bring an old slot back to life (clears its `old` flag). */
int sensor_revive(int id);
/* How many LIVE (non-old) CGM slots exist -- the count that decides
 * multi-CGM behaviour (the primary picker, etc.). */
int sensor_live_cgm_count(void);
/* Make this device the primary; clears any other primary. No-op for a BGM. */
int sensor_set_primary(int id);
/* ---- PER-DEVICE PREFERENCES, as operations rather than fields ------------
 *
 * Each of these validates its argument, takes the registry lock, changes the
 * slot and PERSISTS the table -- and returns -1 if the save failed, so the
 * caller can say so rather than showing a change that will be gone after the
 * next launch.
 *
 * They exist so that no caller spells the transaction out by hand: lock,
 * assign, rewrite, unlock, and act on the result. Four copies of that
 * sequence in one dispatcher are four chances to forget a step -- and
 * forgetting the rewrite is invisible until the app restarts and the user's
 * marker, colour or wear budget is back to the stored value. */
int sensor_set_marker(int id, int marker);
int sensor_set_color(int id, int color);
int sensor_set_size(int id, int size);
int sensor_cycle_size(int id); /* 1..MARK_SIZE_MAX, wrapping */
/* AUTO -> 10 D -> 15 D -> AUTO. A wear budget the user set by hand outranks
 * the model/type rule; AUTO gives it back. */
int sensor_cycle_wear(int id);
/* Rename. An empty name falls back to "SENSOR <id>": an all-blank label makes
 * the device row unreadable, and the row is how a user tells two identical
 * sensors apart. */
int sensor_set_label(int id, const char *name, int len);

/* Index of the primary slot, or -1. */
int sensor_primary_slot(void);
/* The primary sensor's id, or -1, resolved under the registry lock. Use this
 * (not sensor_primary_slot) from any path that also takes hist_lock: resolve
 * BEFORE taking hist_lock, so the reg->hist order is preserved. */
int sensor_primary_id(void);

/* How long a session of this sensor type lasts, in seconds; 0 for a type
 * with no fixed wear length. */
long sensor_session_len(int type);

#ifdef APP_FAULTS
/* ---- A RENDER THAT CANNOT DESCRIBE THE TABLE ----------------
 *
 * When > 0, the registry's serializer pretends its buffer is this many bytes.
 * It exists because the real one cannot overflow at MAX_SLOTS = 10 with a
 * 19-byte label -- so the behaviour that matters (publish NOTHING, keep the
 * file, report the failure) is unreachable without it -- and an unreachable
 * path is one that stops working with nobody noticing. Never compiled into
 * the app. */
/* A SETTER, not the variable: app/sensors.h may not export a writable object
 * (make settingscheck), and that rule is right even for a hook -- a header
 * that hands out an int hands out the ability to write it from anywhere. 0
 * puts the real buffer size back. */
void sensors_fault_render_cap_set(int cap);

#endif

#endif
