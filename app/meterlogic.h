// SPDX-License-Identifier: GPL-3.0
// meterlogic.h --- The meter runtime's timing decisions (host-testable)
// Copyright 2026 Jakob Kastelic
//
/* THE METER RUNTIME'S TWO TIMEOUTS, split out from the actuation in meter.c.
 *
 * Same reason as alarmlogic.h: these are decisions, they are the only thing
 * standing between a wedged exchange and a meter that never syncs again, and
 * while they were `if` statements inside meter_sync_watchdog_locked -- a
 * function that closes GATT links and takes two locks -- no test could reach
 * them.
 *
 * Both are recovery rules, and both exist because a BLE callback can simply
 * not arrive:
 *
 *   1. A SYNC THAT OVERRAN. The meter answered, the exchange started, and
 *      then it stopped -- the meter was switched off mid-walk, or moved out
 *      of range. Nothing else covers this: the link is busy, so the stranded
 *      check below skips it, and pancra_link_watchdog skips meter links
 *      entirely.
 *
 *   2. A LINK STRANDED WAITING FOR A TEARDOWN. The close was asked for and
 *      the link left ARMED deliberately (un-arming there is what let the tick
 *      reconnect a still-awake meter and re-run the exchange once a second),
 *      so the release depends entirely on the disconnect callback arriving.
 *      One that never lands would strand the link for the life of the
 *      process, and that meter would never sync again.
 *
 * The bounds are deliberately generous. A real teardown is either immediate
 * (an app-initiated close: 0.3 s in an HCI capture) or the meter powering
 * itself off (a supervision timeout ~35 s later), so three minutes only ever
 * fires on a callback that is genuinely lost, and re-arming costs one
 * connect.
 *
 * Pure: no globals, no clock, no JNI, no locks. main.c passes the state in
 * and acts on the result; test/metertest.c pins the behaviour.
 */
#ifndef METERLOGIC_H
#define METERLOGIC_H

#include "civil.h" /* zone_off_fn: the meter's clock has no zone on it */

/* Seconds a protocol exchange may run before it is treated as wedged. */
#define METER_SYNC_MAX_S 90
/* Seconds a link may sit waiting for a disconnect callback before it is
 * released anyway. */
#define METER_TEARDOWN_MAX_S 180

/* Links this decision can speak about. Matches LINK_MAX; kept as its own name
 * so this module needs no BLE header. */
#define METER_LINKS_MAX 8

/* What the runtime should do on this tick. */
struct meter_tick {
   /* The running sync has overrun: close and release its link. */
   int drop_sync;
   /* One flag per link: it has been waiting for a teardown too long and
    * should be released regardless of the callback. */
   int release[METER_LINKS_MAX];
   int nrelease; /* how many `release` flags are set, so a caller can skip */
};

/* Evaluate both rules.
 *
 * `busy`/`start` describe the running exchange (start is when it began);
 * `idle_since[l]` is when link l asked for its teardown, or 0 if it is not
 * waiting for one. `nlinks` bounds both. `now` is the clock.
 *
 * The stranded check is skipped entirely while a sync is running -- a busy
 * runtime owns its links, and releasing one under it would tear down the
 * exchange this same tick may be about to time out properly. */
void meter_tick_eval(int busy, long start, const long *idle_since, int nlinks,
                     long now, struct meter_tick *out);

/* ---- WHICH INSTANT A FINGERSTICK WAS TAKEN AT -------------------------
 *
 * A OneTouch record carries a naive local clock reading and nothing else: no
 * offset, no zone, no UTC. Converting it is civil.h's job, and for all but
 * two hours a year civil.h has one answer. In the repeated hour of a
 * fall-back it has two, an hour apart, and the meter's record says nothing
 * about which.
 *
 * WHAT WENT WRONG WITH GUESSING. meter.c resolved each record independently
 * by a fixed-point iteration, which always settled on the SAME one of the two
 * -- so a fingerstick at 01:30 PDT and another at 01:30 PST, a real hour
 * apart, both became the same instant. The reading log dedups a BGM by exact
 * timestamp, so the second fingerstick was not stored an hour wrong, it was
 * not stored at all. And 01:45 PDT followed by 01:15 PST came out in the
 * wrong ORDER, which puts a fingerstick before the meal it followed.
 *
 * WHAT DECIDES IT. The records themselves. The meter walks its memory by
 * ascending index and an index is assigned in the order fingersticks were
 * taken, so the sequence of instants is known to be increasing even where the
 * sequence of clock readings is not. A clock reading that fails to advance is
 * therefore the fall-back itself, observed: from that record on, the second
 * pass of the repeated hour is in force. Import time is the other
 * constraint -- a reading cannot have been taken after it was imported, so a
 * candidate instant in the future is not a candidate.
 *
 * WHEN NOTHING DECIDES IT. A run of records whose clock readings rise
 * steadily through the repeated hour is consistent with the fall-back having
 * happened at ANY point in the run, or before it, or after it. There is no
 * evidence, and inventing some is how the original defect was written. Such a
 * record is stamped with civil.h's documented choice (the earlier instant)
 * AND FLAGGED: `ambiguous` is 1 and `t_alt` is the instant that was not
 * chosen, so what was stored is a stated guess rather than a silent one.
 *
 * Pure, like the two timeouts above: no clock, no JNI, no globals. The zone
 * is a callback and the state is the caller's. */

/* One record's answer. */
struct meter_stamp {
   long t;        /* the instant chosen */
   long off;      /* the offset in force at `t` -- what goes in the tz column */
   long t_alt;    /* the instant NOT chosen; == t unless `ambiguous` */
   int ambiguous; /* 1 = two valid instants and nothing in evidence decided */
   int shifted;   /* 1 = the civil time never existed and was moved forward */
};

/* Carried across ONE walk, in record order. Zero it before the first record
 * (meter_seq_reset), and nowhere else: it is the record order, and record
 * order is the evidence. */
struct meter_seq {
   long prev_t; /* instant chosen for the previous record; 0 = none yet */
   int have_prev;
   /* The fall-back has been OBSERVED in this run of ambiguous records, so
    * every later one in the same run belongs to the second pass. Cleared by
    * any record that resolves unambiguously, which is what leaving the
    * repeated hour looks like from here -- a stale flag would otherwise push
    * a later, unrelated ambiguous record an hour late. */
   int fell_back;
};

void meter_seq_reset(struct meter_seq *sq);

/* Stamp one record. `naive` is the meter's clock reading as civil seconds
 * (see civil.h); `import_t` is the wall clock now, or 0 when it is not known
 * -- 0 disables only the future-candidate constraint, never the ordering
 * one, so a caller without a trustworthy clock still gets a monotonic
 * sequence. */
struct meter_stamp meter_stamp_step(struct meter_seq *sq, long naive,
                                    long import_t, zone_off_fn zone, void *ctx);

#endif
