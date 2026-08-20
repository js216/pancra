// SPDX-License-Identifier: GPL-3.0
// exercise.h --- Exercise intensity log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* WHAT IS RECORDED IS THAT AN INTENSITY WAS SETTLED ON, AND WHEN.
 *
 * The exercise control is not a form. It is one button on the ADD menu that
 * cycles 0 - 1 - 2 - 3 and back, and 0 means "no exercise" and is never
 * written. So the record is a timestamp and a number from 1 to 3, which is the
 * same shape as the weight log with a much narrower value.
 *
 * THE DELAY IS THE WHOLE DESIGN, and it belongs to the caller rather than to
 * this file. Cycling from 0 to 3 passes through 1 and 2, and neither of those
 * was ever a statement about exercise -- they are what the button looks like
 * on the way past. Writing on every press would put two rows nobody meant into
 * a log that is never rewritten. So a value is committed only once it has gone
 * UNCHANGED for a settling period, and the pending state (including the
 * countdown the button draws) lives with the UI in exercise_pending. What
 * reaches this file has already survived that wait.
 *
 * The file is the lifetime record -- append-only, never rewritten, the
 * readings.csv discipline -- and the in-memory tail exists for the UI and for
 * sync. */
#ifndef PANCRA_EXERCISE_H
#define PANCRA_EXERCISE_H

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

/* How long a value must stay put before it is written, in seconds.
 *
 * Long enough to cycle past the values you did not want -- a press takes well
 * under a second, and getting from 0 to 3 takes three of them -- and short
 * enough that the record's timestamp is still honestly "about when the
 * exercise was", and that the button stops looking unresponsive.
 *
 * It was 60. Ten is the same rule with the wait made bearable: the cost of
 * being wrong is one spurious row in a log that is never rewritten, and the
 * cost of being slow is a control that feels broken every time it is used. */
#define EX_SETTLE_S 10

/* Epoch bound on an entry instant, same rationale and value as INS_T_MAX and
 * WT_T_MAX: wide enough for legitimate backdating, tight enough that a corrupt
 * digit run cannot parse as a plausible date. */
#define EX_T_MAX 32503680000L

/* In-memory tail only -- the file keeps everything. */
#define NEX 256

struct ex_rec {
   long t;   /* entry instant, epoch seconds */
   int level; /* EX_MIN_LEVEL..EX_MAX_LEVEL */
};

/* THE TAIL IS PRIVATE, and hands out copies -- weight.h explains why at
 * length. Order is part of the contract: oldest first, newest last. */
int ex_count(void);
/* The i-th, oldest first. Out of range yields a zeroed record. */
struct ex_rec ex_at(int i);
/* The newest, or a zeroed record when there is none. */
struct ex_rec ex_newest(void);
/* Copy up to `cap` of them, oldest first; returns how many were copied. */
int ex_copy(struct ex_rec *out, int cap);

const char *exercise_path(void);
/* Point it at the data directory; the filename lives here. 1 when the path
 * fitted, 0 when it did not -- and then it is not usable. */
int exercise_paths(const char *dir);

/* Load the tail of the log. Safe on a fresh install: a missing file is an
 * empty log. 0 read whole (or nothing to read), -1 what was loaded is
 * INCOMPLETE -- see weight_load, which this follows exactly. */
int exercise_load(void);

/* Append one intensity durably and mirror it into the tail. 0 on success, -1
 * when the write failed (the tail is then left untouched, so memory never
 * claims an entry the file does not have). Rejects a level outside
 * EX_MIN_LEVEL..EX_MAX_LEVEL, which is what keeps a resting 0 out of the
 * file even if a caller loses track of the settling rule. */
int exercise_append(long t, int level, long tz);

/* ================== THE SETTLING RULE, AS A PURE FUNCTION ==============
 *
 * Kept here, and kept pure, for the same reason the insulin and weight form
 * rules were pulled out of the shell's dispatcher: the interesting behaviour
 * is a small decision made on every tick, and a decision that lives inside a
 * renderer is one no test can reach.
 *
 * The state is what the button is showing and since when. A press advances the
 * value and RESTARTS the clock -- that is what makes cycling past a value
 * free. A tick with the value unchanged for EX_SETTLE_S says "commit this".
 *
 * `since` IS MONOTONIC, not wall time. What is being measured is an elapsed
 * interval on the phone in the user's hand, and a clock correction during it
 * must not either commit early or park the pending value for an hour. The
 * instant that goes in the RECORD is wall time, taken at commit -- that one is
 * a civil instant and belongs in the log as such. */
struct ex_pending {
   int level;   /* what the button shows now: 0..EX_MAX_LEVEL */
   long since;  /* monotonic seconds when it last CHANGED */
   int armed;   /* 1 while a nonzero level is waiting to be committed */
};

/* What the caller should do on this tick. */
enum ex_verdict {
   EX_HOLD = 0,  /* nothing yet: keep showing the countdown */
   EX_COMMIT,    /* the level has settled: write it */
   EX_IDLE       /* nothing pending -- the button is at rest */
};

/* Advance the button by one press. Cycles 0->1->2->3->0. */
void ex_press(struct ex_pending *p, long mono_now);

/* ---- THE LIVE BUTTON, WHICH IS NOT A FORM --------------------------
 *
 * The three functions below wrap ONE process-wide pending value, because the
 * exercise button is unlike every other control that records something: the
 * commit does not happen on a tap. It happens a minute later, whether or not
 * anybody is looking at the screen the button is on -- or at any screen.
 *
 * That makes it state two threads touch. The press arrives on the UI thread;
 * the minute expires on whichever tick notices, and the service tick runs on
 * the service's own thread precisely so that it keeps happening when the
 * activity is gone. So the value lives here, behind a leaf lock, rather than
 * beside the drafts in forms.c, which is single-threaded by contract.
 *
 * exercise_button_press advances it. exercise_button_tick commits it when it
 * has settled -- call it from every tick; it is a no-op the rest of the time.
 * exercise_button_get hands the renderer a coherent copy of what to draw. */
void exercise_button_press(long mono_now);

/* Commit if the value has settled. Returns 1 when a record was written, so a
 * caller can repaint; 0 otherwise. `now` is the wall instant to record and
 * `mono_now` the monotonic one to measure with -- they are different clocks
 * doing different jobs, and passing them separately is what stops one being
 * used for the other. */
int exercise_button_tick(long now, long mono_now, long tz);

/* What to draw: the level showing, and how many seconds of the settling
 * period are left (0 when nothing is pending). */
void exercise_button_get(long mono_now, int *level, int *remaining);

/* Ask what this tick means. Never writes anything itself; the caller commits,
 * because the caller is the one that can report a failed write. */
enum ex_verdict ex_tick(const struct ex_pending *p, long mono_now);

/* Mark the pending value as written, so it is not written again. */
void ex_committed(struct ex_pending *p);

/* How much of the settling period is LEFT, in the range 0..EX_SETTLE_S, for
 * the shrinking progress bar the button draws. 0 when nothing is pending. */
int ex_remaining(const struct ex_pending *p, long mono_now);

#endif
