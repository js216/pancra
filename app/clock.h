// SPDX-License-Identifier: GPL-3.0
// clock.h --- the two clocks, and which one a number came from
// Copyright 2026 Jakob Kastelic
/*
 * THE PRIMITIVE EVERYTHING MEASURES WITH, in a module of its own.
 *
 * These lived in util.h, which is otherwise about files, logs and strings.
 * That was harmless until thread.h needed a millisecond clock for
 * mutex_drain's timeout and util.c needed a mutex to serialise appends: the
 * two headers then included each other, and the lowest primitive in the app
 * -- the one every other module rests on -- had a cycle in it.
 *
 * A clock depends on nothing. It is a leaf, and now it is one.
 *
 * THE DISTINCTION IS THE POINT. realtime_s() identifies an INSTANT -- a
 * reading's timestamp, a dose's, anything written to a file or shown to a
 * person -- and mono_s() measures an INTERVAL: watchdogs, retry backoffs,
 * cooldowns, the radio-quiet hold around a pairing.
 *
 * They are NOT one call. Made one, a wall-clock correction -- a phone coming
 * back from being off, or finding a network -- moves every deadline in the
 * app at once: forward an hour fires the meter's 90-second sync watchdog
 * immediately and tears down a working exchange; backward an hour postpones
 * it, so a wedged link is never recovered at all. `make clockcheck` keeps
 * deadlines off the wall clock.
 */
#ifndef PANCRA_CLOCK_H
#define PANCRA_CLOCK_H

#include "compiler.h" /* PANCRA_MUST_USE: the annotation, portably */

#include "wireint.h" /* int64_t: an instant is 64 bits on every machine */

/* EXACT WIDTHS, NOT `long`. An instant from realtime_s() is written into the
 * record files and travels to the server as decimal text, and a 32-bit `long`
 * stops being able to hold one in 2038 -- with no build error, since the code
 * compiles perfectly. The wire's scalars are int64_t (lib/wireint.h) for that
 * reason, and the clock they come from must be at least as wide or the
 * truncation simply happens one call earlier. On the shipped target
 * (arm64-v8a, LP64) int64_t IS long, so no caller changes. */
long long now_ms(void);   /* CLOCK_MONOTONIC milliseconds */
int64_t mono_s(void);     /* CLOCK_MONOTONIC seconds: for DEADLINES */
int64_t realtime_s(void); /* CLOCK_REALTIME seconds (epoch): for INSTANTS */

/* ---- WHEN THE CLOCK ITSELF DOES NOT ANSWER --------------------------------
 *
 * mono_s() returns 0 on a failed clock_gettime, and 0 is ALSO a legal
 * monotonic second -- the first second of uptime -- and, worse, the value the
 * app uses everywhere as "never stamped". Three different meanings in one
 * return value is fine for the callers that only ever want a number to print;
 * it is not fine for a DEADLINE, where "the clock did not answer" and "no
 * time has passed" lead to opposite actions.
 *
 * So a deadline reads the clock through mono_try(), which says which of the
 * two happened. MONO_GET_OK is deliberately NON-ZERO: enum csv_field made
 * OK == 0 and inverted the sense of an `if (!ok)` at a call site in
 * sensors.c that had to be found and fixed afterwards. Here `if (mono_try(&t)
 * != MONO_GET_OK)` and a bare `if (mono_try(&t))` both mean what they read
 * like.
 *
 * WHAT A DEADLINE DOES WITH MONO_GET_FAIL is defined once, in scanlogic.h
 * (live_due / live_*_fresh): it REFUSES -- nothing is due and nothing is
 * fresh -- and the stamp is left unarmed so the first successful read after
 * the clock comes back finds every deadline due at once. */
enum mono_get {
   MONO_GET_FAIL = 0, /* clock_gettime refused; *out is left untouched */
   MONO_GET_OK   = 1, /* *out holds CLOCK_MONOTONIC seconds */
};

/* Read CLOCK_MONOTONIC seconds, saying whether the read happened at all.
 *
 * warn_unused_result on purpose: the whole point of the outcome is that a
 * caller which drops it is back to treating a failed clock as second zero,
 * which is what the outcome exists to make unwritable. */
PANCRA_MUST_USE enum mono_get mono_try(int64_t *out);

#endif
