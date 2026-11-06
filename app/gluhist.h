// SPDX-License-Identifier: GPL-3.0
// gluhist.h --- GLUCOSE HISTORY: which day, week, month or year is shown
// Copyright 2026 Jakob Kastelic

/* The screen shows one window at a time: a span -- a day, seven days, one,
 * three or twelve calendar months -- starting on a chosen local date. The
 * window begins at that date's midnight and ends where the span says: MONTH
 * from the 15th of October runs to the 14th of November, and from the 31st
 * of January to the 28th or 29th of February, the length of the month it
 * starts in clamped like any month step. Any step moves the date, whatever
 * the span.
 *
 * Boundaries are LOCAL midnights, resolved to instants in the offset in force
 * at each one, so a day that changes offset is 23 or 25 hours long and the
 * window still runs midnight to midnight. */
#ifndef PANCRA_GLUHIST_H
#define PANCRA_GLUHIST_H

#include "uimodel.h" /* struct ui_gluhist, enum ui_hspan */

/* THE STEP BUTTONS, as the ix MA_GLUHIST_STEP carries, in the order the two
 * rows draw them left to right. */
enum gluhist_step {
   GH_WEEK_BACK,
   GH_DAY_BACK,
   GH_DAY_ON,
   GH_WEEK_ON,
   GH_YEAR_BACK,
   GH_MONTH_BACK,
   GH_MONTH_ON,
   GH_YEAR_ON,
   GH_TODAY, /* to the window that ends with today, the span kept */
   GH_FIRST, /* to the window that starts with the first reading's day */
   GH_STEP_N
};

/* MA_GLUHIST_OPEN / _BACK / _SPAN / _STEP / _DATE. Returns 1 when `action`
 * was one of these. Opening shows the window that ends with today, in the
 * span last chosen. */
int gluhist_action(int action, int ix);

/* The window now chosen: `start` and `end` are its first and one-past-last
 * instants, and `out` gets the span and the chosen date, the window's first
 * day (`out->sum` is left alone). */
void gluhist_window(long *start, long *end, struct ui_gluhist *out);

/* The same arithmetic for any span and date, `day` counted in days since
 * 1970-01-01, with the zone given as the offset east of UTC in force at an
 * instant. Exposed so it can be driven with a made-up zone. */
void gluhist_window_of(int span, long day, long (*tz)(long), long *start,
                       long *end, struct ui_gluhist *out);

/* `day` moved by one step, in days since 1970-01-01. A month or year step
 * keeps the day of the month, clamped to the length of the month it lands
 * in. */
long gluhist_step_day(long day, int step);

/* The parts of the shown date a tap can retype, each on its own keypad: the
 * year, and the month and day together as MMDD. */
enum gluhist_part { GH_PART_YEAR, GH_PART_MMDD, GH_PART_N };

/* SET ONE PART of the chosen date -- the window's first day -- to `v`: a
 * year from 1970 to 9999, keeping the month and day, or MMDD, a month from
 * 01 to 12 and a day from 01 to that month's length, keeping the year. A new
 * year that leaves the day past its month's end (29 February) moves it to the
 * last day. Returns 0, or -1 with `err` (`cap` bytes) saying the range when
 * `v` is out of it, and then changes nothing. */
int gluhist_set_part(int part, long v, char *err, int cap);

#endif
