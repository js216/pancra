// SPDX-License-Identifier: GPL-3.0
// steps.h --- how many steps were taken, per five-minute window
// Copyright 2026 Jakob Kastelic

/* THE COUNTING IS THE PHONE'S, NOT OURS.
 *
 * Android exposes a hardware step counter (TYPE_STEP_COUNTER) that runs on
 * the sensor hub rather than the application processor: it keeps counting
 * while the phone sleeps and costs approximately nothing, because the CPU is
 * never woken to look at an accelerometer. There is no signal processing to
 * do here and none should be added -- a pedometer written in this app would
 * mean holding the AP awake to sample motion, which is the one thing a
 * glucose monitor's battery cannot spare.
 *
 * WHAT THAT COUNTER GIVES is a total since boot, monotonically rising. A
 * WINDOW's worth of steps is therefore a DIFFERENCE between two readings of
 * it, which is what steps_tick turns into rows.
 *
 * ONLY NON-EMPTY WINDOWS ARE WRITTEN. Sitting still is the common case and a
 * row saying "0" states nothing the absence of a row does not -- at one row
 * per five minutes a sedentary day would cost 288 of them to say nothing
 * happened. The plot draws a gap as zero, which is what it means.
 */
#ifndef PANCRA_STEPS_H
#define PANCRA_STEPS_H

/* FIVE MINUTES, matching a CGM's own cadence so the two logs line up on the
 * plot without either being resampled. */
#define STEP_BUCKET_S 300

/* The most one window may claim. A five-minute sprint is on the order of a
 * thousand steps; ten thousand is a counter that was reset, a clock that
 * moved, or a corrupt field, and none of those is a walk. */
#define STEP_MAX 10000

/* Epoch bound on a row's instant: the same value and the same rationale as
 * INS_T_MAX and WT_T_MAX. */
#define STEP_T_MAX 32503680000L

/* In-memory tail; the file keeps everything.
 *
 * 4096 windows. Only non-empty ones are stored, and a walked day fills on the
 * order of a hundred, so this holds months rather than the fourteen days
 * 4096 consecutive windows would come to. The longest span the plot offers is
 * 30 days, which is what the tail has to cover for the chart to be honest. */
#define NSTEPS 4096

struct step_rec {
   /* WHEN THE WINDOW ENDED, which is when the count was taken. Naming the end
    * rather than the start means a row is never about the future: the steps
    * it reports have all already happened at the instant it carries. */
   long t;
   int n; /* steps in it, 1..STEP_MAX */
};

/* THE TAIL IS PRIVATE and hands out copies -- weight.h explains why at
 * length. Order is part of the contract: oldest first, newest last. */
int steps_count(void);
/* The i-th, oldest first; out of range yields a zeroed record. */
struct step_rec steps_at(int i);
/* Copy up to `cap` of them, oldest first; returns how many were copied. */
int steps_copy(struct step_rec *out, int cap);

const char *steps_path(void);
/* Point it at the data directory; the filename lives here. 1 when the path
 * fitted, 0 when it did not -- and then it is not usable. */
int steps_paths(const char *dir);

/* Load the tail. A missing file is an empty log, not a failure. 0 read whole,
 * -1 what was loaded is INCOMPLETE -- see weight_load, which this follows. */
int steps_load(void);

/* Append one window durably and mirror it into the tail. 0 on success, -1
 * when the write failed (and then the tail is left untouched, so memory never
 * claims a row the file does not have). */
int steps_append(long t, int n, long tz);

/* ONE WINDOW'S WORTH OF SAMPLING, given the hardware counter's current total.
 *
 * Call it as often as convenient -- it is a no-op until a window has closed,
 * so the app's existing 1 Hz and service ticks can both drive it without
 * either needing a timer of its own. `cum` is the OS counter; a negative one
 * means the sensor has not answered yet and the call does nothing.
 *
 * A COUNTER THAT WENT BACKWARDS IS A REBOOT (the total resets to zero), so
 * the window it straddles is dropped rather than recorded as a huge negative
 * or a huge positive. One lost window per reboot is the honest price. */
void steps_tick(long now, long tz, long cum);

/* 1 while the tick has a counter to work from -- i.e. sampling has produced
 * at least one reading this run. The STEP COUNT screen says so, because a
 * toggle that is on while nothing is arriving looks identical to one that is
 * working until a day has passed with an empty plot. */
int steps_live(void);

/* Ask for ACTIVITY_RECOGNITION, which the counter needs from API 29. Called
 * when the user switches the feature ON and at no other time. Implemented in
 * main.c, beside the other JNI the shell owns. */
void steps_request_perm(void);

#endif
