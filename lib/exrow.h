// SPDX-License-Identifier: GPL-3.0
// exrow.h --- one decoder for one row of the exercise log
// Copyright 2026 Jakob Kastelic

/* ONE FORMAT, ONE READER, BOTH SIDES -- the reason insrow.h gives, for the
 * exercise log. The phone writes exercise.csv and the server renders the rows
 * it syncs, so both have to agree on which rows are rows and what they say.
 *
 * A row is "t,level,tz,dur": the instant, the intensity, the offset it was
 * written with, and how long it lasted. The last two may be absent. */
#ifndef PANCRA_EXROW_H
#define PANCRA_EXROW_H

/* The intensity domain, and it is deliberately tiny.
 *
 * 1 / 2 / 3 are "light", "moderate", "hard" in the user's own judgement; the
 * app does not define them further and does not try to. 0 is the button's
 * resting position and is NOT a value: it means no exercise is being recorded,
 * which is the absence of a row rather than a row saying zero. Storing it
 * would make "no exercise" and "exercise of intensity nothing" the same
 * record, and only one of them is a thing a person means. */
#define EX_MIN_LEVEL 1
#define EX_MAX_LEVEL 3

/* Epoch bound on an entry instant, same rationale and value as INS_T_MAX and
 * WT_T_MAX: wide enough for legitimate backdating, tight enough that a corrupt
 * digit run cannot parse as a plausible date. */
#define EX_T_MAX 32503680000L

struct ex_rec {
   long t;    /* entry instant, epoch seconds */
   int level; /* EX_MIN_LEVEL..EX_MAX_LEVEL */
   /* HOW LONG IT LASTED, in seconds, and 0 means "not known".
    *
    * A row is written when the level SETTLES, which is the start; nothing at
    * that moment knows when the exercise will end. So the row is written open
    * and closed later, by exercise_end, when the user takes the button back
    * to zero -- the rewrite this log's tier already allows for corrections.
    *
    * 0 therefore means one of two things and the plot treats them
    * differently: the session is still running (the button is still at a
    * nonzero level, and the plot measures it against the clock), or it never
    * got an end at all -- the app was killed, or the level was left up
    * overnight and cycled from a cold start. The second draws no line rather
    * than a made-up one.
    *
    * Rows written before this column existed have no field for it and read as
    * 0, which lands them in the second case: correct, and the reason the
    * column was appended rather than inserted. */
   long dur;
   /* THE OFFSET THE ROW WAS WRITTEN WITH, seconds east of UTC. Parsed but not
    * part of the match key, and carried for one reason: closing a session
    * rewrites its row, and a rewrite that substituted the CURRENT offset would
    * silently relabel an entry made on the other side of a DST change. */
   long tz;
};

/* THE WIDEST DURATION A ROW MAY CLAIM. Seven days -- long enough that no real
 * session is refused, short enough that a corrupt digit run cannot draw a line
 * across every plot the app has. Beyond it the row is rejected, like an
 * out-of-range level. */
#define EX_DUR_MAX 604800L

/* Decode ONE row, from `p` to `e` (the end of the LINE, not of the file).
 * 1 when it is a record this format allows, 0 when it is not: an instant,
 * level or duration out of range. A header or blank line is the caller's to
 * skip before asking. */
int ex_row_decode(const char *p, const char *e, struct ex_rec *r);

#endif
