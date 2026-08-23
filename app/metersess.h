// SPDX-License-Identifier: GPL-3.0
// metersess.h --- The meter's protocol SESSION: one owner, one lock
// Copyright 2026 Jakob Kastelic
//
/* WHOSE EXCHANGE IS RUNNING, AND ON WHICH LINK.
 *
 * There is exactly ONE OneTouch protocol state in the process (otble.c holds
 * it), while every registered meter carries its own standing connect -- so at
 * any moment at most one meter may own the exchange, and every other meter's
 * connect has to be refused until it does not. That fact -- busy, the link it
 * is running on, the registry id behind it, when it started, and which links
 * are waiting for a teardown -- was five file-scope variables in meter.c
 * written from three threads with no lock at all:
 *
 *   BINDER   the connect/disconnect/notify callbacks (meter_hook_connected,
 *            meter_hook_disconnected, ot_drv_*), which both TEST busy and
 *            SET it;
 *   MAIN     the 1 Hz tick's watchdog, which reads busy and the start stamp to
 *            decide whether to tear an exchange down, and the pairing path,
 *            which claims the session outright;
 *   SERVICE  the same watchdog again, from the foreground service's thread.
 *
 * The watchdog's single-flight guard serialises watchdog callers against each
 * OTHER and against nothing else, which is the part that read as safe and was
 * not: testing a bare "busy" flag against a bare "which link" in a binder
 * callback is a
 * test and a set with a window between them, and two meters waking together
 * really do connect milliseconds apart. Losing that race seeds otble's shared
 * state twice -- phase reset mid-walk, last_index replaced -- and the second
 * meter's fingersticks are written under the first meter's id, into an
 * append-only file that is never rewritten.
 *
 * So the session is a MODULE with a lock rather than variables with a
 * convention. Every operation that has to be atomic -- claim, release,
 * end-of-exchange -- is one call here, and callers get a SNAPSHOT
 * (msess_get) rather than fields they read one at a time.
 *
 * THE LOCK IS A LEAF, and structurally so: nothing in this file calls the
 * driver, the transport, the registry or the clock, so its lock cannot be
 * held across any of them. That matters because the binder callers arrive
 * ALREADY HOLDING driver_lk -- the order is driver_lk -> msess_lk -- while
 * the main-thread callers take this one on their way to a driver call. A
 * design where meter.c held this lock and then called dexble_* would close
 * exactly the cycle the app has already frozen on twice; here it cannot,
 * because the lock is not reachable from outside this file.
 *
 * TIME IS PASSED IN. Every stamp here is an INTERVAL -- how long has this
 * exchange run, how long has this link waited -- so callers pass mono_s().
 * The module has no clock of its own precisely so that a wall-clock stamp
 * cannot be smuggled in by including the wrong header; see clockcheck.
 *
 * Pure otherwise: no globals but its own, no JNI, no files. test/app/
 * metersesstest.c runs the callback and watchdog paths against it on real
 * threads.
 */
#ifndef METERSESS_H
#define METERSESS_H

/* Links this module can speak about. Matches LINK_MAX, kept as its own name
 * so the session needs no BLE header (see meterlogic.h, same rule). */
#define MSESS_LINKS_MAX 8

/* Address buffer, matching the meter runtime's own. */
#define MSESS_MAC_MAX 24

/* One coherent reading of the whole session. Fields read one at a time from
 * three threads is how a log line reported link -1 as mid-sync. */
struct msess {
   int busy;                /* a protocol exchange is running */
   int link;                /* the link it is running on, or -1 */
   int src;                 /* registry id of the meter that owns it, or 0 */
   long start;              /* when it began, on the MONOTONIC clock */
   char mac[MSESS_MAC_MAX]; /* that meter's address */
};

/* Everything at one instant. */
void msess_get(struct msess *out);

/* CLAIM the exchange for `link` -- the whole test-and-set in one step.
 *
 * 1 when this link now owns the session (it already did, or nothing did).
 * 0 when another link is mid-exchange and this connect must be refused;
 * nothing is written in that case.
 *
 * `now` is mono_s(): the start stamp the watchdog measures the exchange
 * against. */
int msess_claim(int link, int src, const char *mac, long now);

/* Take the session unconditionally, for the PAIRING path: the user is
 * standing there having just registered this meter, and the link was
 * allocated for it a line ago. */
void msess_begin(int link, int src, long now);

/* The exchange this link owned is over, or the link died.
 *
 * 1 when `link` really was the owner (the caller's "was this the active
 * sync?"), 0 when it was an idle standing connect. Clears the session either
 * way it was the owner; leaves it alone when it was not, so a stale
 * disconnect for an old link cannot cancel the sync now running on another.
 *
 * `idle_now` is stamped into the link's teardown-wait slot when non-zero, so
 * "the exchange ended and this link is now waiting for its disconnect" is one
 * atomic step rather than two. Pass 0 to leave the slot alone. */
int msess_end(int link, long idle_now);

/* Drop the session with no owner named: the watchdog decided the exchange has
 * overrun. Returns the link it was running on, or -1 -- the caller needs it to
 * close the GATT link, and reading it separately afterwards would read the
 * cleared value. */
int msess_drop(void);

/* Which meter the app is bound to, and its address. `src` is 0 when none. */
int msess_src(void);
int msess_busy(void);
/* Bind to a meter without claiming an exchange (the reconcile path, which
 * decides WHICH registered meter the runtime speaks for). */
void msess_bind(int src, const char *mac);

/* THE TEARDOWN-WAIT TABLE. `when` is mono_s() at the moment the close was
 * asked for, or 0 for "not waiting". The watchdog copies the whole table --
 * one call, one instant -- and hands it to meter_tick_eval. */
void msess_idle_set(int link, long when);
void msess_idle_copy(long *out, int n);


#endif
