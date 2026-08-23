// SPDX-License-Identifier: GPL-3.0
// meter.h --- The OneTouch meter runtime: links, syncs and the record index
// Copyright 2026 Jakob Kastelic
//
/* A METER IS NOT A SENSOR, and almost every rule differs.
 *
 * A CGM streams: it stays connected, pushes a reading every five minutes, and
 * its session is what the app is built around. A OneTouch meter is ASLEEP.
 * It wakes for a few seconds when the user takes a fingerstick, advertises,
 * accepts one connection, answers a request/response protocol, and powers
 * itself off. Everything here follows from that:
 *
 *   - EVERY meter gets its own link and its own standing connect, because the
 *     window in which it can be reached is seconds long and is not ours to
 *     choose;
 *   - the arm table is separate from the driver session, because a meter
 *     never runs the Dexcom handshake that stamps one (reading it back
 *     reported every meter as unarmed, and the tick issued a connect PER
 *     SECOND, forever);
 *   - a link is released only when the GATT disconnect actually ARRIVES, not
 *     when the close is requested -- and because that callback can be lost,
 *     meterlogic.c owns a bounded recovery for both timeouts.
 *
 * The RECORD INDEX is persisted per meter so a reconnect never re-reads what
 * we already hold: the protocol can only ask for "records after N", and
 * re-reading them all would duplicate every fingerstick in the log.
 *
 * THREADING. The link tables are written on the MAIN thread and read from
 * BINDER threads, so every write takes driver_lock -- the lock those readers
 * already hold. The arm entry is a 24-byte string, and a snapshot racing a
 * compare really can be read half-written, which mis-identifies the meter
 * that just connected.
 *
 * The SESSION -- which meter owns the one protocol exchange, on which link,
 * since when, and which links are waiting for a teardown -- is metersess.c's,
 * behind its own leaf lock. It was file-scope variables in meter.c written
 * from three threads, with the "is another meter mid-sync?" test and the
 * seizure of the exchange as two separate statements. Everything below that
 * reads it (meter_busy, meter_src, meter_ui_of) is a snapshot taken there.
 */
#include "loadresult.h" /* what a load actually found */

#ifndef METER_H
#define METER_H

#include "dexdriver.h" /* LINK_MAX */

/* --- the link table --- */

/* Does this link carry a meter? Several CGM-only passes walk every link and
 * must skip meters; this is the fact they check. It mirrors what the
 * transport was told, and is written in one place, so the two cannot drift. */
int meter_link_is(int link);
/* Say whether `link` carries a meter (1) or a CGM (0). */
void meter_link_set(int link, int on);
/* Un-arm: this meter no longer has a connection outstanding, so the tick may
 * arm it again. The link KEEPS its meter routing bit. */
void meter_unarm_link(int link);
/* Give the link back completely: un-armed AND no longer routed to otble.
 * Only correct once the GATT disconnect has actually arrived -- clearing the
 * routing bit when the close is merely REQUESTED sends the in-flight callback
 * into the CGM branch, which posts "CONNECTION ERROR" after every successful
 * meter sync. */
void meter_release_link(int link);

/* --- the sync --- */

int meter_busy(void);             /* a protocol exchange is running */
int meter_armed(const char *mac); /* this meter has a connect outstanding */
/* A free link CLAIMED for this meter's address, or -1. The search and the
 * claim are one step inside the driver: a link that reads free and is taken
 * by a binder thread before the caller uses it would be handed to two devices
 * at once. */
int meter_alloc_link(const char *mac);
void meter_sync_start(int mid, const char *mac);
/* Pair a newly registered meter: seed its index, arm a link, bond, connect.
 * Returns 1 when a connect is outstanding; every other path releases the
 * link, because an armed link with nothing behind it is what stops the tick
 * from ever retrying. */
int meter_pair(int id, const char *mac);
/* Bounded recovery for an exchange that never finished and for a link that
 * never got its disconnect callback. Called from the 1 Hz tick AND from the
 * BLE service thread. It is this module's function, so it is declared here;
 * the rules it applies are meterlogic.h's, and the heartbeat that drives it
 * asks the shell for one tick (shell.h) rather than naming it. */
void meter_sync_watchdog(void);
/* Point the runtime at the data directory; both filenames live here. */
/* Hand the driver this module's half of the callback routing. Once, at
 * startup, before any link is armed. */
void meter_register_ops(void);

/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int meter_paths(const char *dir);
/* Load the persisted per-meter sync times and record indices. */
/* THE PER-METER STATE FILES, read at startup: the last-sync times and the
 * signal captured with them. Answers what the storage actually gave back --
 * LOAD_ABSENT on a first run, LOAD_CORRUPT for a file this program did not
 * write, LOAD_ERROR for one it could not read -- so startup can report it
 * with every other loader's answer rather than presenting a lost file as an
 * ordinary launch. */
enum load_result meter_state_load(void);

/* Registry id of the meter the app is bound to, and its address. */
int meter_src(void);
/* Bind to a meter: the id whose fingersticks are being imported, and the
 * address the "is this OUR meter" guard compares against. Without the address
 * that guard accepted ANY OneTouch in range after a restart -- importing a
 * stranger's readings under our sensor id. */
void meter_bind(int id, const char *mac);

/* PER-METER runtime, keyed by registry id: in-memory, reset each launch,
 * except the last-sync time which is persisted. It exists because the
 * process-global "last meter" state made one meter's sync throttle the other
 * and showed one meter's signal against both. */

/* (This meter's own last signal is NOT its own question: it is in struct
 * meter_ui. Asked for separately from the sync time, that is two reads of a
 * record a binder thread writes, and the row can show one instant's time
 * beside another instant's signal.) */

/* (The advert throttle's own stamp was readable as meter_advert_mono. It is
 * not any more: reading it, deciding, and recording were three steps a
 * second scan callback could interleave. meter_note_advert does all three,
 * and its answer is whether this advert took the turn.) */
/* An advertisement is proof the meter is switched on and in range: record it
 * as a real "last seen", with the signal that advert carried.
 *
 * THROTTLED, and the throttle is part of the same step: 1 when this advert
 * took the turn (none in the last `window` seconds), 0 when it was too soon.
 * A caller that asks for the last advert time, decides, and then records
 * lets two scan callbacks for one meter -- which is what a meter waking up
 * produces -- both pass. */
int meter_note_advert(int id, int rssi, long now, long window);

/* What the device row shows for this meter. */
struct meter_ui {
   long sync_t; /* when the app last CONNECTED it (not its last reading) */
   /* ...and its signal, from the SAME copy: take a second one through a
    * separate query and a binder write in between shows a time and a signal
    * from two different instants. */
   int rssi, rssi_ok;
   long rssi_t;
   char stat[24]; /* the driver's last phase text, or "" -- A COPY: this was
                   * a pointer into the runtime table, which a binder thread
                   * rewrites while the row that borrowed it is drawn */
   int syncing;   /* an exchange is running with THIS meter right now */
};

void meter_ui_of(int id, struct meter_ui *out);

/* Mark a link as waiting for its teardown, so the watchdog can recover one
 * whose disconnect callback never arrives. */
void meter_link_idle(int link, long when);

/* The DIS strings learned from the meter's own GATT service. */
/* The meter's model and firmware, as the meter's own business: it sanitises
 * what a GATT characteristic hands it (printable, no commas, bounded) rather
 * than lending its buffers out for another file to write into. */
enum { METER_DIS_MODEL = 0, METER_DIS_FW = 1 };

void meter_set_dis(int which, const char *val);

/* THE METER'S SIGNAL STRENGTH, as reported by the transport. Declared here
 * rather than in a shared app<->transport header for the same reason as the
 * CGM's hooks in reading.h: the module that IMPLEMENTS a callback is the one
 * that should describe it. Arrives on a BINDER thread. */
void pancra_meter_rssi(int rssi);

#endif
