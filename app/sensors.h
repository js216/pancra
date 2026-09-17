// SPDX-License-Identifier: GPL-3.0
// sensors.h --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* Every datapoint must name its origin exactly, decades after the sensor
 * itself is landfill.
 *
 * "FOREVER" IS A PROMISE WITH A NUMBER BEHIND IT: the provenance table holds
 * MAX_SENSOR_RECS devices, and the block above that constant is the whole
 * argument for where the bound is, why it is not reachable before the phone is
 * landfill, and why the table REFUSES a new device rather than forgetting an
 * old one when it is. A contract that says "forever" and means a number is the
 * kind that gets read once and relied on for years, so it says the number.
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

/* USER-VISIBLE SENSOR SLOTS. The UI pages this list, so the number is not a
 * screen limit; it is how many devices the app will carry at once.
 *
 * BOUNDED, NOT UNLIMITED, because every table keyed by a slot is a fixed
 * array -- the registry itself, the frame's snapshot, the alarm's per-device
 * samples, the hit-test list -- and several of them are STACK arrays on the
 * binder advert path, which runs several times a second. Growing them
 * dynamically would put an allocation on that path to save memory measured in
 * hundreds of bytes.
 *
 * 8192 -- over three centuries at a sensor a fortnight. A slot is never
 * released: a retired device keeps its row, its label, its colour and its
 * per-device menu so its history stays readable, so this is a working life
 * and then some, not a screenful.
 *
 * NOTHING PER-SECOND GROWS WITH IT. A registry read takes a reference instead
 * of copying the table; an advertisement bisects the view's address index
 * instead of comparing every device; the hot walks stop at the live prefix;
 * and a frame carries a WINDOW -- the devices in service plus one page of
 * retired ones (UI_MAX_SLOTS) -- rather than a row per slot. Those four
 * properties are what make this number a capacity rather than a speed limit.
 *
 * NOTHING SIZED BY IT IS ON A STACK, which is the other half and the easier
 * one to lose. A binder thread gets about a megabyte, so a single whole-table
 * local -- a render buffer, an undo copy, a by-value view -- is fatal at this
 * size. The tables that must exist are file-scope, each owned by the lock
 * that already serialises the one operation using it; everything else is
 * sized by what it can actually hold: LINK_MAX for per-link work, METER_MAX
 * for per-meter work, SESSC_MAX for the session cache, UI_MAX_SLOTS for a
 * frame.
 *
 * WHAT IT COSTS IS MEMORY, paid once: the registry and its provenance rows in
 * BSS, and a published view of the same shape on the heap -- with a second
 * one alive only across a swap.
 *
 * Keep both properties if it is raised again. The second one is checkable:
 * compile the tree with the app's own flags and -Wframe-larger-than at two
 * different values of this number and compare the frame sizes. No frame may
 * grow WITH the capacity -- a constant-size step of a few bytes between two
 * values is the compiler encoding a larger immediate, while a difference that
 * scales is a table on a stack, which is the bug. */
#define MAX_SLOTS 8192

/* EVERY ID sensors.csv HAS EVER NAMED, HELD AT ONCE -- and nothing is ever
 * evicted from this table. Its size is the whole of that promise, so the
 * number below is argued rather than picked.
 *
 * A SMALL CACHE OF RECENT ROWS CANNOT DO THIS JOB, however tempting the
 * reasoning that only sensors with points in the plot window need resolving.
 * readings.csv is append-only and EVERY row cites a source_id, so a citation
 * outlives the plot window by years -- the reason to keep the log at all is
 * that a reading taken in 2026 can still say which physical sensor produced it
 * in 2036. Evicting "the oldest record no LIVE SLOT references" drops exactly
 * the set of ids that only HISTORY cites: the row stays on disk and the app
 * simply stops being able to resolve it.
 *
 * THE RATE THAT SETS THE HORIZON. A CGM session mints one id (a new sensor
 * advertises a new address), so a Stelo (15 d) and a G7 (10 d) worn together
 * plus a meter is ~61 ids a year. A table of N rows therefore answers for
 * about N/61 years of history and no further, and the horizon moves forward
 * for as long as the app is used.
 *
 * WHAT AN UNRESOLVABLE ID COSTS, written down because the three readers fail
 * in three different ways and only one of them is loud:
 *
 *   - sensor_mint's identity scan reads these rows to recognise an address it
 *     has seen before. An address past the horizon is not recognised, so the
 *     device is minted a SECOND id -- one physical sensor with two
 *     identities, its history split in a log that is never rewritten.
 *     Reachable by anyone who forgets a device and re-pairs it, with one tap.
 *   - sensor_complete fills a bare row's model, firmware and session start
 *     when they arrive. A live sensor whose row is not held can never be
 *     completed, so it stays bare for ever.
 *   - sensor_in_warmup FAILS OPEN on an id it cannot resolve, and stats.c
 *     asks it for every historical row: an unresolvable sensor's uncalibrated
 *     first hour then COUNTS towards time-in-range and the daily average, at
 *     every launch. stats.c reaches back STAT_HOURS (~92 days) and g_hist
 *     ~9-17 days, so a horizon of years keeps this latent -- but at ~5x the
 *     typical mint rate (six concurrent CGMs, or sensors failing early and
 *     being replaced) a one-year horizon falls under 92 days and it starts
 *     moving the numbers on the front screen.
 *
 * The last one is the shape of the whole risk: nothing is wrong with the
 * READERS, and the log keeps every row. A bound on this table is the app
 * deciding in advance how far back it is willing to be able to answer -- and
 * an append-only log exists precisely so that limit does not have to exist.
 *
 * THE BOUND BEING ACCEPTED: MAX_SLOTS rows of 120 bytes, which at 8192 is
 * 960 kB of BSS for this table. Counting the slot rows and every table the app
 * indexes BY ID, the marginal cost measured between two values of MAX_SLOTS is
 * 374 bytes per slot -- so 2.92 MiB at 8192. Mostly demand-zero: the pages a
 * table of three devices never touches are never committed. The exception is
 * style_rebuild_locked, which clears the `known` flag across ONE of the two
 * id-indexed style banks on every publish and so dirties that bank's 128 kB. It
 * is demand-zero -- the pages a table of three
 * devices never touches are never committed -- and it buys centuries at either
 * mint rate, on ONE install of ONE phone whose readings.csv would by then hold
 * tens of millions of rows. "Unbounded" was not on offer: this path runs on a
 * phone, with no allocator behind it. A bound that cannot be reached before
 * the hardware is landfill is.
 *
 * PAST IT THE TABLE REFUSES, IT DOES NOT FORGET: the row is not taken,
 * sensors_load() reports the file as incompletely read (so the user is told),
 * and sensor_mint() fails -- a new sensor then shows as unregistered, which
 * is visible, rather than quietly taking a dropped id's place. That is the
 * same treatment sensors.c gives the 16-bit `src` ceiling: unreachable in
 * practice, but an invariant rather than an estimate.
 *
 * WHAT THIS CAPACITY OUTRUNS, said here rather than found later: the sync
 * layer carries a log in buckets of at most SYNC_BUF_MAX, and sensors.csv has
 * no timestamp in its leading field, so the whole file is one bucket (see
 * syncjni_register_logs). At the ~84 bytes per id it measures, syncing stops
 * carrying the provenance table somewhere past 3000 ids -- decades of wear, but
 * not the centuries the table itself holds. The registry keeps working; it is
 * the off-phone copy that stops growing.
 *
 * WHY NOT COMPACT THE FILE INSTEAD (the other route open here): sensors.csv
 * is append-only precisely so that no partial write can lose a row, and a
 * compaction is a rewrite. The file is already last-wins per id, so the only
 * thing a compaction could reclaim is superseded sensor_complete corrections
 * -- at most a few rows per sensor -- while putting every id that has ever
 * existed through one non-atomic rewrite. Memory is the cheaper thing to
 * spend.
 *
 * THE SAME BOUND AS THE SLOT TABLE, and it has to be. Every registered
 * device holds one row here and one slot there, and NEITHER is ever released
 * -- a disconnected device keeps its slot so its history stays readable (see
 * MAX_SLOTS). So the two count the same devices over the same lifetime, and a
 * provenance table smaller than the slot table is the real ceiling: srec_push
 * refuses first, sensor_mint refuses with it, and the slots past that point
 * can never be filled however large MAX_SLOTS is written. One number, so the
 * capacity the header claims is the capacity the app has. */
#define MAX_SENSOR_RECS MAX_SLOTS

/* `marker` and `size` below are plain ints carrying style.h's MARK_* values.
 * This header does not include it: nothing here names one in code, and the
 * shapes and the palette are a presentation concern that the registry only
 * stores. Whoever interprets those fields includes style.h itself. */

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
 * reference to the published view (sensors_view_ref below) rather than
 * calling them in a loop, so no caller has to hold a lock -- or know
 * which one. */

/* (THE INDEXED READS ARE NOT HERE. A count-then-index pair is two locked
 * questions about a moving table, and no caller may ask them.)
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
 *     registry lock across link_for_sensor (which takes the driver's) inverts
 *     it. A snapshot cannot: the lock is gone before anything else is called.
 *
 * One copy, taken under one hold. `have_rec[i]` says whether slot i's id has
 * a provenance row; `rec[i]` is only meaningful when it does.
 *
 * MAX_SLOTS WIDE, which is a lifetime of devices rather than the handful worn
 * at once: a disconnected device keeps its slot, so the table only grows. See
 * MAX_SENSOR_RECS above -- the attribution table is the same bound, because
 * it counts the same devices over the same lifetime. */
struct sensor_view {
   int n;
   /* LIVE FIRST, then the retired ones: slots [0, n_live) are live and
    * [n_live, n) are disconnected.
    *
    * The order is the point. Every hot path -- the advert filter, the alarm's
    * gather, the reconcile walk, the link ranking -- wants only the live
    * devices, and the alternative is each of them walking the whole table
    * testing `.old` to skip most of it, several times a second. A device
    * retired years ago is still carried, since nothing is ever deleted, but
    * those loops stop at n_live. The two screens that show retired devices
    * read the tail. */
   int n_live;
   /* SLOT INDICES SORTED BY IDENTITY, for the lookups that ask "which device
    * has this address?" -- the advert filter (every advertisement) and the
    * reading path (every reading). Both walked the whole table and compared
    * addresses one at a time, which is the cost that grows with every sensor
    * ever owned rather than with the ones being worn.
    *
    * Built once per publish, searched by bisection. Only slots that have a
    * provenance row appear, because only those have an address; `n_mac` is
    * how many. */
   int n_mac;
   /* SHORT, and that is a bound on MAX_SLOTS as real as the stack rule above:
    * an index that does not fit wraps to a negative number and
    * sensors_view_find_mac then reads rec[] out of range -- one sensor's
    * address resolving to another's link and key file, the failure this whole
    * file is written against. Asserted rather than remembered. */
   /* POINTERS INTO THE SAME ALLOCATION, sized by the rows the registry HOLDS
    * rather than by the rows it could hold. As four arrays of MAX_SLOTS they
    * come to 1.4 MB, and a view is rebuilt on every rename, retire, pairing and
    * load -- so describing three devices would allocate and free a megabyte and
    * a half each time, with the empty sentinel below holding another for the
    * life of the process to say "nothing yet". The rows are contiguous and
    * indexed by position, so a reader walks them as it would an array.
    *
    * CONST, because a published view is immutable for as long as any
    * reference to it lives; the publisher writes through its own aliases in
    * struct view_node before the view is visible to anyone. */
   const short *by_mac;
   const struct sensor_slot *slot;
   const struct sensor_rec *rec;
   const int *have_rec;
};

_Static_assert(MAX_SLOTS <= 32767, "by_mac is short: MAX_SLOTS must fit one");

/* ---- THE SAME PICTURE, BY REFERENCE ------------------------------------
 *
 * The registry does not change between mutations, so it is built once when it
 * does and handed out by pointer until the next one. A reader gets a picture
 * that cannot shift while it calls the driver -- so the registry lock is
 * never held across driver_* -- for a lock acquire, whatever the table holds.
 * A copy would be the registry's whole capacity, which is a lifetime of
 * sensors and far past what any stack frame may carry.
 *
 * THE VIEW IS IMMUTABLE and lives until every reader has let go, so holding a
 * reference across a mutation keeps reading the version taken. Pair every ref
 * with exactly one put, on every path out.
 *
 * CONST, so the immutability is the compiler's rule and not a note: the view
 * is shared by every holder at once, and a write through it would corrupt the
 * picture the others are mid-read of. The one place that removes the
 * qualifier is sensors_view_put, which reaches the reference count beside the
 * view rather than the view; it says so there. */
/* ---- PLOT STYLING FOR EVERY DEVICE THE LOG NAMES ------------------------
 *
 * A reading carries the id of the sensor that produced it, and the plot draws
 * it in that sensor's colour and marker -- for as long as the reading is kept,
 * which is for ever. So styling has to be answerable for devices that are no
 * longer live, and eventually for devices no longer held in memory at all:
 * this is the ONE thing a retired device is still needed for on a hot path.
 *
 * THREE BYTES, INDEXED BY ID, so it stays answerable when the rest of a
 * device's record does not. Ids are dense and never reused, so the lookup is
 * an array index rather than a walk -- the plot asks once per point, and the
 * walk it replaces was over every live device per point.
 *
 * `out` is left at the defaults when the id names no device (a pre-registry
 * reading, or one past the table), which draws as the neutral main trace --
 * never as some other device's colour. */
struct sensor_style {
   int marker;
   int color;
   int size;
};

/* THE SLOT INDEX FOR THIS ADDRESS, or -1. Bisects the view's own index, so it
 * costs a handful of comparisons whatever the device count. The caller applies
 * its own filter (live, CGM, meter) to what comes back -- this answers only
 * "which slot", which is the part that had to walk. */
int sensors_view_find_mac(const struct sensor_view *v, const char *mac);

/* THE SAME SEARCH, RESTRICTED TO ONE KIND (style.h's KIND_*), or -1 for any.
 *
 * The filter belongs INSIDE the search. Applied to whatever the plain form
 * returned, it answers "no such device" whenever the lowest slot holding that
 * address is of another kind -- so a CGM sharing an address with a meter row
 * would stop resolving, and the reading path would attribute its data to
 * nobody. Asked here, the run of equal addresses is walked and the lowest
 * slot OF THAT KIND comes back. */
int sensors_view_find_mac_kind(const struct sensor_view *v, const char *mac,
                               int kind);

/* 1 when `id` names a device with styling of its own. */
int sensor_style_of(int id, struct sensor_style *out);

const struct sensor_view *sensors_view_ref(void);
void sensors_view_put(const struct sensor_view *v);

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
 * clock, and both Dexcom UIs counting warmup down as 3600 - clock. The anchor
 * is therefore the LIVE session clock, which the sensor reports on every 4e. */
#define SENSOR_WARMUP_S 3600L

/* IS THIS SENSOR WARMING UP RIGHT NOW, as the sensor itself reports it?
 *
 * Distinct from sensor_in_warmup below, which asks whether one stored READING
 * fell inside a sensor's warm-up hour. This one is about the sensor's present
 * state, and is what the device screens label.
 *
 * Two ways it says so and no third: the state byte, or a session clock that
 * has not yet run out the hour. Both come off the 0x4e response, so both are
 * measurements. A `state` of 0 with a `session_seconds` of 0 is neither -- it
 * is a sensor nothing has arrived from, which is not a warm-up but a sensor
 * the app has yet to hear from, and it must not be dressed as one.
 *
 * ONE PREDICATE, because the status string and the list's countdown are two
 * renderings of a single fact: a second copy of this test is a second answer
 * to it, and the two screens then describe the same sensor differently. */
static inline int sensor_warming_now(int state, long session_seconds)
{
   /* A CLOCK INSIDE THE HOUR DOES NOT OUTRANK A STATE BYTE THAT DISAGREES.
    * SENSOR_STATE_OK is the sensor saying it is past warm-up, and a session
    * whose clock restarted -- another collector beginning a session -- would
    * otherwise read as warming up for an hour while good glucose arrived. The
    * clock decides only where the sensor has not said: state 0 before any
    * response, and the early post-start states that carry no glucose yet. */
   if (state == SENSOR_STATE_WARMUP)
      return 1;
   if (state == SENSOR_STATE_OK || state == SENSOR_STATE_ENDED)
      return 0;
   return session_seconds > 0 && session_seconds < SENSOR_WARMUP_S;
}

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

/* Find a slot by address alone, regardless of type, and INCLUDING a retired
 * one -- which is what re-pairing a disconnected device needs: it keeps the
 * id it always had. Use this to recognise
 * an already-registered device: keying on type as well lets a stale UI
 * selection re-register one physical sensor under a second, wrong type. */
/* THE ID OF THE SLOT THAT CLAIMS THIS ADDRESS, or -1. Answered from the
 * published view, so it is a bisect of the sorted address index and takes no
 * state lock -- which is what a caller on the 1 Hz path needs. The answer can
 * be one publish old; ask sensor_slot_by_mac when it must not be. */
int sensor_id_by_mac(const char *identity);

/* THE SLOT INDEX FOR THIS ADDRESS, or -1, read from the live table. A walk
 * bounded by every device ever registered, so it belongs on the paths that run
 * a few times a year (minting), not on the ones that run every second. */
int sensor_slot_by_mac(const char *identity);

/* Fill `out` (capacity `max`) with the ids of slots whose marker is MARK_HIDE,
 * returning the count. One locked pass, so the plot's scrub path can flag
 * hidden points without a per-point registry lock. */
int sensor_hidden_ids(int *out, int max);

/* Mint an id for a newly paired sensor and append its provenance row. A
 * physical device is identified by its address alone -- serial, model, fw and
 * activation are learned attributes, not identity -- so a repeat mint for a
 * known address returns the existing id, and an id maps to one physical
 * device for life. Returns the id; on failure a NEGATIVE value, which is -1
 * when nothing was written and the log module's own code when the provenance
 * append reached the file and failed there. Every caller tests < 0. */
int sensor_mint(int type, const char *identity, const char *serial,
                const char *model, const char *fw, long activation);

/* Complete a row's learned attributes once they arrive (DIS strings a few
 * seconds after the first reading; activation once the session clock is
 * known). Fills only fields the row is missing -- an empty string or 0
 * activation -- never overwrites a learned value, so it is idempotent and a
 * late caller cannot clobber an earlier truth. Durable the same way minting
 * is: the corrected row is APPENDED to sensors.csv (the file is never
 * rewritten) and loading it is last-row-wins per id -- which is sensors.csv's
 * rule and NOT slots.csv's, where the first row wins because that is the one
 * this phone wrote (see slots_read). Returns 1 if something was
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
 * plot. Capture the id once (sensors_view_ref) and pass
 * that; a slot index must never be handed straight to any of these. An
 * unknown id changes nothing and says so. */

/* Drop the slot (provenance is untouched, so old readings stay attributed). */
int sensor_forget(int id);

/* CAN THE REGISTRY BE CHANGED AT ALL? 0 when slots.csv did not load and the
 * table must therefore not be written back over it (see slots_read). Every
 * mutation is refused while this is 0, and a caller that reports "full" for
 * that refusal tells the user to forget one of the devices it is also not
 * showing them. Ask this to say the true thing instead. */
int sensors_writable(void);

/* 1 when the published view is OLDER than the table behind it -- a publish
 * whose allocation failed. The change reached the flash, so nothing is lost,
 * but every reader (the advert filter, the alarm, the frame) answers from the
 * previous picture until some later change publishes. The DEVICES screen says
 * so, because a registry that is right on disk and wrong on screen is
 * otherwise indistinguishable from one that ignored the tap. */
int sensors_view_stale(void);

/* 0 when sensors.csv did not load or did not parse whole. No new device can
 * be registered while this is 0 (sensor_mint refuses), because the next id
 * comes from the highest one that table holds and a short table reissues an
 * id readings.csv already cites. Ask this to say WHY a pairing was refused:
 * without it the refusal reads as a disk failure, which is the one thing it
 * is not. */
int sensors_provenance_loaded(void);

/* 1 when sensors.csv holds a row for every slot slots.csv names.
 *
 * A DIFFERENT QUESTION FROM sensors_provenance_loaded, which asks whether the
 * file read and parsed. Both can be true while this is false: the two files are
 * separate append-only logs, restored separately, so one can arrive ahead of
 * the other. The devices without a row show with no type, no serial and no
 * session, and every hot path skips them -- so this is reported to the user and
 * gates nothing. It cannot gate anything: no control removes a slot, so a
 * refusal based on it would stand for the life of the install. */
int sensors_provenance_covers(void);

/* 1 when slots.csv parsed WHOLE -- every row it holds became a slot.
 *
 * Stricter than sensors_writable(), which asks only whether the rows are still
 * on disk and so stays true for a file that lost individual rows to the parse.
 * A mint needs this one: a dropped row can be the one carrying the highest id,
 * and the next id is derived from that. */
int sensors_slots_whole(void);
/* DISCONNECT: retire the slot to "old" rather than dropping it -- keeps its
 * marker/label/prefs and its place in the registry so the full per-device
 * menu and plot styling still work, but excludes it from every live path.
 * Reassigns the primary to the first live CGM left. */
int sensor_retire(int id);
/* RECONNECT: bring an old slot back to life (clears its `old` flag). */
int sensor_revive(int id);
/* How many LIVE (non-old) CGM slots exist -- the count that decides
 * multi-CGM behaviour (the primary picker, etc.). */
/* Slots the registry holds, live and retired, of every kind. Zero means the
 * registry is genuinely empty -- which is the only state in which provenance id
 * 0 can be read as "pre-registry data" rather than as a device this phone
 * knows. Ask sensor_live_cgm_count for the narrower question. */
int sensor_slot_count(void);

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
 * It exists because SLOT_ROW is sized from what the format can print, so the
 * real buffer does not overflow -- which leaves the behaviour that matters
 * (publish NOTHING, keep the file, report the failure) unreachable without
 * this, and an unreachable path is one that stops working with nobody
 * noticing. Never compiled into the app. */
/* A SETTER, not the variable: this header may not export a writable object,
 * and that rule is right even for a hook -- a header that hands out an int
 * hands out the ability to write it from anywhere. 0 puts the real buffer
 * size back. */
void sensors_fault_render_cap_set(int cap);

#endif

#endif
