// SPDX-License-Identifier: GPL-3.0
// streak.h --- which readings an in-range streak counts, and how long it is
// Copyright 2026 Jakob Kastelic

/* ONE STREAK, BOTH SIDES.
 *
 * The phone draws the in-range streak over its plot and the server prints it
 * under the big number. Two copies of the rules -- which sensor counts, which
 * readings of a new sensor are passed over, what band, what gap -- would give
 * two numbers for one run, so the rules live here and both sides call them.
 * Where the readings come from (the phone's history, the server's rows) is
 * each side's own business.
 *
 * Pure: no globals, no clock, no locks. */
#ifndef PANCRA_STREAK_H
#define PANCRA_STREAK_H

#include <stdbool.h>
#include <stddef.h> /* size_t */

/* The time-in-range band, in mg/dL: the ADA/ATTD international consensus
 * range, inclusive at both ends. The streak is measured against it, and so is
 * every TIR figure. */
#define TIR_LOW_MGDL  70
#define TIR_HIGH_MGDL 180

/* A hole longer than this, four missed five-minute samples, ends a streak at
 * its near side: nothing measured what happened inside it. */
#define STREAK_GAP_S (20L * 60)

/* A BRAND NEW SENSOR'S FIRST READINGS COUNT FOR NOTHING.
 *
 * They arrive about 8, 13 and 18 minutes into the session, inside the warm-up
 * hour, and the first is routinely far from the truth: 52 mg/dL one minute
 * and 121 the next on one sensor, 271 then 199 on another. They are stored and
 * shown like any other reading; alarms and streaks pass over them.
 *
 * COUNTED IN READINGS, BOUNDED IN TIME. The count is the rule, and the bound
 * is what confines it to a sensor that really is new: a sensor paired
 * mid-session also starts with an empty history, and its readings are good.
 * STREAK_SKIP_S is the last of the skipped samples plus two cycles of slack,
 * so a dropped sample cannot stretch the silence past it. */
#define STREAK_N_SKIP 3
#define STREAK_SKIP_S ((STREAK_N_SKIP + 2) * 300L)

/* Is a sensor's reading stamped `t` one of its first STREAK_N_SKIP? `start`
 * is the session's start in epoch seconds, 0 when unknown, and `n` the
 * reading's place among the sensor's readings, the oldest being 1.
 *
 * FAILS OPEN: an unknown start answers false, because a reading that cannot be
 * proven to be a new sensor's may be a real hypo. */
bool streak_first_points(long start, long t, int n);

/* WHETHER A READING CAME FROM THE PRIMARY CGM AS IT ARRIVED, the `lead`
 * column of readings.csv. Zero is "not recorded", which is what a row written
 * before the column existed says and what a zeroed reading says. */
enum read_lead { READ_LEAD_UNKNOWN = 0, READ_LEAD_NO = 1, READ_LEAD_YES = 2 };

/* A SENSOR'S SESSION START: epoch seconds, 0 when not known. */
struct sensor_start {
   int id;
   long start;
};

/* ONE CGM'S READINGS IN THE WHOLE LOG: the first and the last. STREAK_SPANS
 * is how many a streak is judged against, a lifetime of sensors. */
#define STREAK_SPANS 1024

struct src_span {
   int src;
   long first, last;
};

/* WAS `src` THE PRIMARY CGM WHEN ITS READING AT `t` ARRIVED? `recorded` is
 * the row's READ_LEAD_*, and when it says, that is the answer.
 *
 * A row from before the record existed is answered from the spans: the
 * primary is the sensor that started first among those running at `t`. A
 * worn sensor stays primary until the user moves the number to its
 * replacement, and a replacement starts after it. A source with no span
 * counts. */
bool read_lead(int recorded, int src, long t, const struct src_span *sp,
               int nsp);

/* ONE READING, as far as a streak cares: 16 bytes, so a lifetime of
 * readings can be held. */
struct streak_pt {
   long t;             /* epoch seconds */
   short glu;          /* mg/dL */
   unsigned short src; /* the registry id of the device */
   unsigned char cgm;  /* nonzero for a CGM reading; others never count */
   unsigned char lead; /* READ_LEAD_* */
};

/* The order a history is kept in, as a qsort comparator: newest first, then
 * by source and glucose, so a reading stored twice sorts next to itself. */
int streak_cmp(const void *a, const void *b);

/* Sort `r` newest first and drop a reading stored twice (the same time,
 * source and glucose); returns how many are left. */
int streak_sort(struct streak_pt *r, int n);

/* THE STREAK AS A RUNNING STATE, fed one reading at a time, oldest first.
 *
 * A streak needs no history: it is where the current run started, when its
 * last reading was, and the longest run so far -- plus, for a sensor still on
 * its first readings, how many it has given. So a whole log is read once
 * (streak_acc_build) and each new reading after that costs a few comparisons
 * (streak_acc_add). */
#define STREAK_FIRST_N 16

struct streak_acc {
   long seen_t;    /* the newest reading applied; 0 before the first */
   long run_start; /* the oldest reading of the current run; 0 when none */
   long run_t;     /* the newest reading of the current run */
   long cur;       /* the current run, in seconds; 0 when out of range */
   long best;      /* the longest run applied, in seconds */

   /* The sensors inside their first STREAK_SKIP_S: each one's start and how
    * many of its CGM readings have been applied since. */
   struct {
      int src;
      long start;
      int n;
   } first[STREAK_FIRST_N];
};

/* An accumulator with nothing applied. */
void streak_acc_init(struct streak_acc *a);

/* APPLY ONE READING, `x`, from a sensor whose session started at `start`
 * (epoch seconds, 0 when not known).
 *
 * What counts is a CGM reading from the primary as it arrived (read_lead,
 * against the spans in `sp`) that is not one of a new sensor's first
 * STREAK_N_SKIP (streak_first_points). Out of the band [TIR_LOW_MGDL,
 * TIR_HIGH_MGDL] ends the run, and so does a hole longer than STREAK_GAP_S,
 * which starts a new one at the reading past it.
 *
 * THE BAND IS INCLUSIVE: the statistics count 70 and 180 as in range, and
 * breaking at a reading TIR calls good would be two numbers on one screen
 * disagreeing about one sample. An alarm band is exclusive at its limits
 * because an alarm FIRES at the limit; a range CONTAINS it.
 *
 * Returns 0, applying nothing, for a reading this cannot apply in order: one
 * older than the newest applied, or one that does not record its lead when no
 * spans are given (`sp` NULL) to infer it from. The caller then rebuilds. */
int streak_acc_add(struct streak_acc *a, const struct streak_pt *x, long start,
                   const struct src_span *sp, int nsp);

/* THE WHOLE HISTORY, `r` newest first as streak_sort leaves it, applied from
 * the oldest: the starts in `s`, the spans in `sp`. */
void streak_acc_build(struct streak_acc *a, const struct streak_pt *r, int n,
                      const struct src_span *sp, int nsp,
                      const struct sensor_start *s, int ns);

/* A run of time in the coarsest unit that still says something: days and
 * hours from one day up ("12D 5H"), hours from one hour up ("5 H"), minutes
 * below that ("40 MIN"). From 100 days up the hours are dropped ("365D").
 * `cap` is at least 8, which holds the widest answer, "99D 23H". */
void streak_len(long s, char *out, size_t cap);

/* THE LINE THE STREAK IS SHOWN AS: "BEST STREAK 5 H" while the current run is
 * the record (or there is none), "STREAK 5 H (BEST 10D 11H)" otherwise. Both
 * are clamped to a year. `cap` is at least 40. */
void streak_line(long cur, long best, char *out, size_t cap);

#endif
