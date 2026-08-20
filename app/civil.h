// SPDX-License-Identifier: GPL-3.0
// civil.h --- a local civil date/time, and the instant it names
// Copyright 2026 Jakob Kastelic
//
/* WHICH INSTANT DID THE USER MEAN, and which one did the meter record?
 *
 * Every timestamp this app stores is an instant (epoch seconds). Every
 * timestamp a PERSON types, and every timestamp a OneTouch meter reports, is
 * a local civil reading -- "2025-11-02 01:30" -- with no zone attached. The
 * two are not the same kind of thing, and the conversion between them is not
 * a subtraction.
 *
 * It was written as one. forms.c split an instant with `g_tz_off`, replaced
 * the date, and recombined with `g_tz_off` again -- TODAY's offset, applied
 * to a day that may be the other side of a DST boundary. Editing a dose to a
 * date in the other half of the year therefore persisted it an hour wrong,
 * and persisted today's offset in its tz column so nothing downstream could
 * tell.
 *
 * THE THREE ANSWERS. A civil time does not name exactly one instant. Given a
 * zone, it names:
 *
 *   ONE   the ordinary case, everywhere outside the two transition hours.
 *   TWO   the repeated hour of a fall-back. 2025-11-02 01:30 in US/Pacific
 *         happened twice, an hour apart: once at UTC-7 and once at UTC-8.
 *   NONE  the skipped hour of a spring-forward. 2025-03-09 02:30 in
 *         US/Pacific never existed; the clock went 01:59:59 -> 03:00:00.
 *
 * Code that pretends there is always exactly one answer does not fail
 * loudly. It stores the wrong hour, or -- for the meter, whose records carry
 * nothing but a naive clock reading -- collapses two distinct fingersticks
 * onto one instant, where the log's exact-timestamp dedup silently drops the
 * second one.
 *
 * THE RULES THIS MODULE COMMITS TO, stated once here because every caller
 * needs the same ones and a rule that lives in three places is three rules:
 *
 *   AMBIGUOUS -> THE EARLIER INSTANT. The first time the clock read that,
 *      i.e. the offset still in force before the fall-back. It is what
 *      java.time's ZonedDateTime.ofLocal and Python's fold=0 both choose, and
 *      it is the only choice that keeps a form's redisplay stable: the
 *      instant chosen renders back as the civil time that was typed, and
 *      re-editing it does not walk an hour later each time.
 *      civil_resolve reports the OTHER instant in `t_alt` so a caller with
 *      more evidence -- the meter has record order -- can overrule it, and so
 *      a caller with none can record that it guessed.
 *
 *   NONEXISTENT -> SHIFT FORWARD BY THE GAP. The offset in force BEFORE the
 *      transition is applied, which lands the entry one gap-length later on
 *      the far side: 02:30 becomes 03:30. Same rule java.time uses. It is
 *      preferred to refusing because the civil time can become nonexistent
 *      without anyone typing it -- change only the DATE of an 02:30 dose and
 *      the kept time-of-day may land in the gap -- and "NOT A TIME" against a
 *      date the user just typed is unexplainable. The shifted instant
 *      redisplays as 03:30 on the form immediately, so the correction is
 *      visible rather than silent, and `fix` says it happened.
 *
 * NO ZONE DATABASE HERE. The zone is a callback: "what was the offset at this
 * instant". On the phone that is Android's TimeZone.getOffset through
 * tz_offset_at; in a test it is a fake zone with a transition where the test
 * wants one. This module therefore depends on nothing, is pure, and can be
 * driven over transitions that will not occur for months.
 */
#ifndef PANCRA_CIVIL_H
#define PANCRA_CIVIL_H

/* THE ZONE, as one question: how many seconds east of UTC was local time at
 * the instant `t`? `ctx` is the caller's -- a JNIEnv on the phone, a table of
 * transitions in a test. Must be a pure function of `t`: civil_resolve calls
 * it several times for one answer and compares the results, so a callback
 * that reads a mutable "current offset" makes the comparison meaningless (it
 * is precisely the bug this module replaces). */
typedef long (*zone_off_fn)(void *ctx, long t);

/* How many instants the asked-for civil time named. */
enum civil_fix {
   CIVIL_UNIQUE = 0,  /* exactly one: the ordinary case */
   CIVIL_AMBIGUOUS,   /* two: the repeated hour of a fall-back */
   CIVIL_NONEXISTENT, /* none: the skipped hour of a spring-forward */
};

struct civil_res {
   long t;       /* the instant chosen, by the rules in the header comment */
   long off;     /* the offset ACTUALLY in force at `t` -- persist this, not the
                  * offset the arithmetic went through: for a NONEXISTENT time
                  * they differ, and the tz column has to describe the stored
                  * instant or it cannot be used to repair anything. */
   long t_alt;   /* AMBIGUOUS: the instant NOT chosen. Otherwise == t. */
   long off_alt; /* the offset in force at t_alt. Otherwise == off. */
   int fix;      /* enum civil_fix */
};

/* Days since 1970-01-01 for a proleptic Gregorian y-m-d, and back. Howard
 * Hinnant's algorithm, the same one uidraw.c's fmt_date and the server's
 * date parser use; here because a civil time has to become a day count
 * before it can become an instant. */
long civil_days(long y, long m, long d);
void civil_ymd(long z, long *y, long *m, long *d);

/* THE LOCAL CIVIL DATE at instant `t`, read in the offset in force AT `t`.
 * The direction that has no ambiguity in it -- an instant always names
 * exactly one civil time -- but still the one that must not be read in
 * today's offset: a dose stamped in the other half of the year names a
 * different DAY under the wrong offset, and on New Year's Eve a different
 * year, which decides how long February is. */
void civil_at(long t, zone_off_fn zone, void *ctx, long *y, long *m, long *d);

/* `naive` is a civil time expressed as seconds: civil_days(y,m,d) * 86400
 * plus the time of day. It is NOT an instant -- it is what a clock face read
 * -- and this is the function that turns it into one. */
struct civil_res civil_resolve(long naive, zone_off_fn zone, void *ctx);

/* WHICH HALF OF THE TIMESTAMP AN ENTRY REPLACED. The keypad edits one of
 * these three and keeps the rest, which is why the operation cannot be
 * written as "compute a new civil time from scratch": the half that was not
 * typed comes from the instant being edited, read in the offset in force AT
 * THAT INSTANT. */
enum civil_edit {
   CIVIL_EDIT_YEAR,     /* a = year; month, day and time of day kept */
   CIVIL_EDIT_MONTHDAY, /* a = month, b = day; year and time of day kept */
   CIVIL_EDIT_TIME,     /* a = hour, b = minute; the civil DATE kept */
};

/* Re-aim `t` at a different local civil date or time and resolve the result.
 *
 * The caller validates the numbers (a month of 13 or an hour of 25 is a
 * REFUSAL the user must see, and that message belongs to the form). The one
 * calendar rule applied here is February 29 in a year that has none, clamped
 * to the 28th: changing the year of a leap-day entry has to land on a real
 * date, and there is no other date to land on.
 *
 * CIVIL_EDIT_TIME drops the seconds, because HHMM is all the keypad can say
 * and keeping the old seconds would make an edited time differ from the
 * displayed one by up to 59 seconds. */
struct civil_res civil_reaim(long t, int what, int a, int b, zone_off_fn zone,
                             void *ctx);

#endif
