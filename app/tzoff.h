// SPDX-License-Identifier: GPL-3.0
// tzoff.h --- the device's local UTC offset
// Copyright 2026 Jakob Kastelic

/* Every timestamp the app shows, and the tz_off column of every row it
 * writes, comes from here. It is JNI: Android is the only thing that knows
 * the device's zone, including the zone in force at some PAST instant, which
 * is what makes an imported meter record convert the same way twice.
 *
 * Split out of main.c because it is a complete subject with one dependency
 * (the JVM) and one output (the offset), and because it is the one part of
 * main.c whose correctness is visible on screen -- a wrong offset shows up as
 * every timestamp being wrong by an hour.
 */
#ifndef TZOFF_H
#define TZOFF_H

#include "ndk.h"

/* THE CURRENT OFFSET, seconds east of UTC. Zero until the first refresh.
 *
 * A FUNCTION, NOT AN EXPORTED long, because of WHO READS IT. tz_refresh()
 * runs on the main thread (the activity's resume, and the service tick's
 * five-minute re-check); the readers are the BLE binder thread stamping the
 * tz_off column of every reading it stores, the meter import and the frame
 * builder. A plain `long` written by one thread and read
 * by another is a data race in the language's terms -- and this one is not
 * academic: the value changes at a DST boundary, which is exactly when the
 * timestamps it decorates are worth getting right.
 *
 * RELAXED IS ENOUGH, and that is the whole reason this is one scalar rather
 * than a lock. Nothing is ordered against it: a reader wants the offset as a
 * number, not as a signal that something else happened, and a reading stamped
 * with the previous offset one instant either side of a DST change is
 * correct-or-one-refresh-stale, never torn. */
long tz_off_now(void);

/* The offset in force at instant `at` (epoch seconds), or the current one
 * when `at` is 0. Returns g_tz_off unchanged on any JNI failure, so a
 * caller never gets a wild answer -- only a stale one. */
long tz_offset_at(JNIEnv *env, long at);

/* Refresh g_tz_off from the device. Call on resume: the offset is not a
 * constant, it moves at a DST boundary and goes stale while the activity is
 * destroyed. */
void tz_refresh(JNIEnv *env);

#endif
