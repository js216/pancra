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

/* The current offset in seconds east of UTC. Read directly all over the app;
 * refreshed by tz_refresh(). Zero until the first refresh. */
extern long g_tz_off;

/* The offset in force at instant `at` (epoch seconds), or the current one
 * when `at` is 0. Returns g_tz_off unchanged on any JNI failure, so a
 * caller never gets a wild answer -- only a stale one. */
long tz_offset_at(JNIEnv *env, long at);

/* Refresh g_tz_off from the device. Call on resume: the offset is not a
 * constant, it moves at a DST boundary and goes stale while the activity is
 * destroyed. */
void tz_refresh(JNIEnv *env);

#endif
