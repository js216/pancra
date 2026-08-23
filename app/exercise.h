// SPDX-License-Identifier: GPL-3.0
// exercise.h --- Exercise intensity log: an editable CSV + in-memory tail
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
 * the log -- rows the user would then have to go and delete one at a time. So
 * a value is committed only once it has gone
 * UNCHANGED for a settling period, and the pending state (including the
 * countdown the button draws) lives with the UI in exercise_pending. What
 * reaches this file has already survived that wait.
 *
 * The file is the lifetime record and the in-memory tail exists for the UI
 * and for sync.
 *
 * TWO TIERS, AND THIS IS THE ORDINARY ONE (see the block below): capture
 * APPENDS, and an edit or a delete atomically REPLACES the whole file. The
 * immutable, never-rewritten discipline is insulin.h's alone.
 *
 * (A header that calls the file "append-only, never rewritten" and then, a
 * hundred lines further down, documents its own edit and delete is saying two
 * things that cannot both be true.) */
#ifndef PANCRA_EXERCISE_H
#define PANCRA_EXERCISE_H

/* ---- WHICH KIND OF LOG THIS IS: REWRITTEN IN PLACE -------------------
 *
 * THE APP HAS TWO TIERS and this is the ordinary one. A row here records what
 * the USER BELIEVES -- what they ate, what they weighed, how hard they worked
 * -- and a correction REPLACES that belief; the superseded value is of no
 * interest to anybody afterwards. So an edit rewrites the row and a delete
 * removes it, through a temporary file and a rename, which makes the commit
 * atomic: there is no window in which the log is truncated.
 *
 * The other tier is insulin.h's, and it is the only one: a dose is not a
 * preference, so that log is never rewritten and keeps its own history. */

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
 * being wrong is one spurious row the user has to notice and delete, and the
 * cost of being slow is a control that feels broken every time it is used. */
#define EX_SETTLE_S 10

/* Epoch bound on an entry instant, same rationale and value as INS_T_MAX and
 * WT_T_MAX: wide enough for legitimate backdating, tight enough that a corrupt
 * digit run cannot parse as a plausible date. */
#define EX_T_MAX 32503680000L

/* In-memory tail only -- the file keeps everything. */
#define NEX 256

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

/* THE TAIL IS PRIVATE, and hands out copies -- weight.h explains why at
 * length. Order is part of the contract: oldest first, newest last. */
int ex_count(void);
/* The i-th, oldest first. Out of range yields a zeroed record. */
struct ex_rec ex_at(int i);
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

/* ---- CORRECTING AN ENTRY AFTER THE FACT ----------------------------
 *
 * The settling rule only ever appends, which is the ordinary way rows get
 * here. These two are the other act: the user looking at
 * the log and fixing a row -- a level cycled past and left too long, an entry
 * whose instant is wrong -- which the insulin, weight and food logs all
 * already allow. It goes through a temporary file and a rename, so a failure
 * leaves the original whole.
 *
 * `orig` NAMES THE ROW BY ITS CONTENT, not by an index: the caller holds a
 * snapshot of the tail that an append may already have outgrown. When two
 * rows are identical the LAST is the one affected, which is the one a person
 * reading a newest-first table means.
 *
 * 0 on success, -1 when nothing matched or the rewrite failed (and then the
 * file is unchanged). */
/* WHY AN EDIT WAS REFUSED, so the form can say which rule it broke rather
 * than "not changed". A refusal leaves the file untouched in every case. */
enum ex_update_result {
   EX_UPD_OK = 0,
   EX_UPD_RANGE,   /* a value outside the log's own domain */
   EX_UPD_FUTURE,  /* starts, or ends, after now */
   EX_UPD_OVERLAP, /* would share an instant with another session */
   EX_UPD_REOPEN,  /* would open a row that is not the running one */
   EX_UPD_FAILED   /* the rewrite itself did not land */
};

/* `now` is the WALL instant the edit is judged against: a session may not
 * start or end in the future, and the running row's span is measured to it.
 * Refused -- with the file unchanged -- when the result would start or end
 * after `now`, would open a row that is not the running one, or would share
 * an instant with another session. */
enum ex_update_result exercise_update(const struct ex_rec *orig, long t,
                                      int level, long dur, long tz, long now);
int exercise_delete(const struct ex_rec *orig);

/* ---- CLOSING THE SESSION THAT IS RUNNING ---------------------------
 *
 * Rewrites the newest OPEN row (dur == 0) to record how long it lasted. Called
 * when the user takes the button back to zero, which is now a deliberate act
 * rather than a stop on the way round the cycle -- see ex_press.
 *
 * 0 when a row was closed, -1 when there was none to close, the arithmetic
 * gave nothing sensible (a clock that went backwards, or a session longer than
 * EX_DUR_MAX), or the rewrite failed. In every -1 case the file is unchanged
 * and the row simply stays open, which the plot already knows how to read. */
int exercise_end(long now);

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
   int level;  /* what the button shows now: 0..EX_MAX_LEVEL */
   long since; /* monotonic seconds when it last CHANGED */
   int armed;  /* 1 while a nonzero level is waiting to be committed */
};

/* What the caller should do on this tick. */
enum ex_verdict {
   EX_HOLD = 0, /* nothing yet: keep showing the countdown */
   EX_COMMIT,   /* the level has settled: write it */
   EX_IDLE      /* nothing pending -- the button is at rest */
};

/* Advance the value and restart the clock. */
void ex_press(struct ex_pending *p, long mono_now);

/* What this tick should do about it. */
enum ex_verdict ex_tick(const struct ex_pending *p, long mono_now);

/* Seconds of the settling period still to run; 0 when nothing is pending. */
int ex_remaining(const struct ex_pending *p, long mono_now);

/* The caller wrote the record: clear the arming, keep the level showing. */
void ex_committed(struct ex_pending *p);


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
/* WHAT A PRESS DID. Three outcomes, because collapsing two of them -- a
 * press that ended nothing and a press whose end COULD NOT BE WRITTEN, both
 * answering "no session was closed" -- leaves the only caller unable to tell
 * the ordinary case from the one the user has to be told about. */
enum ex_press_result {
   /* The button cycled, or started settling. Nothing was written and
    * nothing needed to be. */
   EX_PRESS_NONE = 0,
   /* A running session was closed and its length is in the file. */
   EX_PRESS_ENDED = 1,
   /* A running session's end could not be written. THE SESSION IS STILL
    * RUNNING -- the button stays lit at the level it had, and the next press
    * retries the close (see exercise.c). The caller must not report the
    * exercise as finished, because the record does not. */
   EX_PRESS_FAILED = 2
};

/* `now` is the WALL instant, `mono_now` the monotonic one -- two clocks doing
 * two jobs, as in exercise_button_tick. The wall one is needed because a press
 * that ends a session writes that session's length, and a length is measured
 * between two instants in the record. */
enum ex_press_result exercise_button_press(long now, long mono_now);

/* Commit if the value has settled. Returns 1 when a record was written, so a
 * caller can repaint; 0 otherwise. `now` is the wall instant to record and
 * `mono_now` the monotonic one to measure with -- they are different clocks
 * doing different jobs, and passing them separately is what stops one being
 * used for the other. */
int exercise_button_tick(long now, long mono_now, long tz);

/* What to draw: the level showing, and how many seconds of the settling
 * period are left (0 when nothing is pending). */
void exercise_button_get(long mono_now, int *level, int *remaining);

/* ---- IS THIS ROW THE ONE STILL BEING WRITTEN? ----------------------
 *
 * 1 when `row` is the session the button is running RIGHT NOW: it is the
 * newest row, it is still open, and the button is lit. That row's length is
 * not a value anybody can supply, because it is not over -- it is `now` minus
 * a start that is still moving, and exercise_end will write it the moment the
 * button goes back to zero.
 *
 * The edit form asks so it can REFUSE the duration field and say ACTIVE
 * instead: a length typed into a running session is overwritten by the end
 * press seconds later, so accepting one is promising an edit that will not
 * survive.
 *
 * AN OPEN ROW IS NOT ENOUGH ON ITS OWN. dur == 0 also means "never got an end
 * at all" -- the app was killed, or a level was left up overnight (see struct
 * ex_rec) -- and THOSE rows are exactly the ones a user needs to be able to
 * fill in by hand. The button being lit is what separates the two, so it is
 * part of the question. */
int exercise_row_running(const struct ex_rec *row);

/* THE RUNNING SESSION, or 0 when none: the newest row, while it is still
 * open. At most one exists by construction -- only the newest row can be the
 * running one, which is the rule exercise_end closes by. `out` may be null. */
int exercise_active(struct ex_rec *out);

/* MAKE THE BUTTON AGREE WITH THE LOG.
 *
 * The log is the durable fact and the button is a view of it: lit at the
 * running session's level, at rest when there is none. Called after anything
 * that can change which row is running -- an append, an edit, a delete, a
 * close, a restore, a launch -- so the two can never drift.
 *
 * A button that is ARMED is left alone. A settling value has not been written
 * yet, so there is nothing in the file for it to agree with, and overwriting
 * it would discard the choice the user is making at that moment. */
void exercise_button_sync(void);

#endif
