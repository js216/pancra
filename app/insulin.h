// SPDX-License-Identifier: GPL-3.0
// insulin.h --- Insulin dose log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

#include "insrow.h" /* the log's own domain and decoder */

#ifndef PANCRA_INSULIN_H
#define PANCRA_INSULIN_H

/* ---- WHICH KIND OF LOG THIS IS: AN ASSERTION LOG ---------------------
 *
 * THE APP HAS TWO TIERS and this is the strict one. A row here records
 * something that happened to a BODY and cannot be taken back, so the file
 * keeps what was asserted and when it was corrected rather than only what is
 * currently believed: "14 units at 08:00" and "14 units at 08:00, corrected
 * an hour later to 12" are different facts about a drug that is already in
 * someone, and the second is the one a person or a doctor needs.
 *
 * So it is never rewritten. An edit appends a new assertion carrying the same
 * id; a delete appends one with del=1; readers replay. No bug in a rewrite
 * can lose a dose, because there is no rewrite.
 *
 * Every other hand-edited log -- weight, food, exercise -- is the other tier
 * and is rewritten in place. */

/* A dose is what the user TYPED, not something a sensor measured, so this log
 * is deliberately minimal: when, which kind, how much. The file is the
 * lifetime record and the in-memory tail exists for the UI -- pre-populating
 * the entry form with the last dose of a type, and any future plotting.
 *
 * THE FILE IS APPEND-ONLY, INCLUDING EDITS AND DELETES (schema v2).
 * Rewritten in place by the edit form, it would be the one log that can lose
 * history to a bug, and it could not say what a dose said yesterday. Every
 * row is an ASSERTION about a dose identified by an id:
 *
 *     written,id,del,unix_time,type,units,tz_offset_s
 *
 * Replay in file order, last assertion per id wins, del=1 removes it. So
 * "6 units at 08:12, corrected to 4 at 08:15" is two rows and both survive,
 * and a past day's rows never change -- which is also what lets the sync
 * protocol treat old buckets as frozen.
 *
 * Rows in the original four-field form (unix_time,type,units,tz_offset_s)
 * load too, and are given negative ids by file order so they can never
 * collide with a minted one. Nothing rewrites them. */

/* (INS_SLOW / INS_FAST / INS_UNITS_* / INS_T_MAX are lib/insrow.h's: they
 * are the LOG's domain, and the server replays the same log. Declared here
 * with the server repeating them as literals, the two are kept in step by a
 * comment saying they must be -- which is a rule, not a
 * mechanism.) */

/* Epoch bound on a dose instant: any positive time up to year ~3000. Wide on
 * purpose (backdating and small future-dating are legitimate user entries);
 * it exists so a corrupt digit run can never parse as a plausible dose. */

/* In-memory tail only -- the file keeps everything. 256 rows is over two
 * months of a heavy 4-dose day. */
#define NINS 256

struct ins_rec {
   long t;    /* dose instant, epoch seconds */
   int type;  /* INS_SLOW / INS_FAST */
   /* THOUSANDTHS OF A UNIT, INS_MILLI_MIN..INS_MILLI_MAX -- 500 is half a
    * unit. Named `milli` and not `units` on purpose: the two are not the same
    * number, and a field called `units` holding 500 for a half-unit dose is
    * the kind of quiet unit confusion that puts a dose out by a thousand.
    * ins_units_str renders it for a screen. */
   int milli;
};

/* THE TAIL IS PRIVATE, for the same reasons as the weight log's (weight.h):
 * exported, every reader depends on the representation, the ordering
 * invariant and the lifetime -- and can write to it. These hand out COPIES.
 *
 * ORDER IS PART OF THE CONTRACT: oldest first, newest last, which is what the
 * dose table and the plot both draw. */
int ins_count(void);
/* The i-th, oldest first; out of range yields a zeroed record. */
struct ins_rec ins_at(int i);
/* Copy up to `cap`, oldest first; returns how many were copied. */
int ins_copy(struct ins_rec *out, int cap);
const char *insulin_path(void);
/* Point it at the data directory; the filename lives here. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int insulin_paths(const char *dir);

/* Load the tail of the log (the last NINS plausible rows). Safe on a fresh
 * install: a missing file is an empty log. */
/* 0 read whole (or nothing to read), -1 a read failed partway: whatever
 * parsed is kept, and the caller says the record is incomplete. */
int insulin_load(void);

/* Append one dose durably and mirror it into the tail. Returns 0 on success,
 * -1 when the write failed (the tail is then left untouched, so memory never
 * claims a dose the file does not have). Rejects out-of-range input. */
int insulin_append(long t, int type, int units, long tz);

/* The units of the most recent dose of `type`, or 0 if none is known --
 * what the LOG INSULIN form pre-populates with. */
int insulin_last_units(int type);

/* Correct or retract the LAST dose matching `orig` (t/type/units) by
 * APPENDING an assertion against its id -- the file is never rewritten. 0 on
 * success, -1 when nothing matches or the write failed. Matching is by
 * CONTENT, so a stale tail index can never touch the wrong dose. */
int insulin_update(const struct ins_rec *orig, long t, int type, int units,
                   long tz);
int insulin_delete(const struct ins_rec *orig);

const char *insulin_type_name(int type); /* "SLOW" / "FAST" */

/* ================= THE DOSE-ENTRY WORKFLOW ==========================
 *
 * The LOG INSULIN form is four values and three rules about them, and all
 * seven lived as globals plus branches in the shell's action dispatcher --
 * where nothing could reach them. The rules are small and entirely about
 * doses, which is what makes them worth having here instead:
 *
 *   - a NEW form is pre-populated with this type's last amount, because each
 *     type has its own habitual dose and retyping it every time is the whole
 *     friction of logging;
 *   - changing the type on a NEW form re-populates from the new type, but on
 *     an EDIT it must NOT -- the amount being edited is the one on record;
 *   - the time starts at the whole minute, so two doses logged in the same
 *     minute are the same instant and dedup as one.
 *
 * `edit` is the index of the entry being edited, or < 0 for a new dose. */
struct ins_form {
   long t;   /* the dose's instant */
   int type; /* INS_SLOW / INS_FAST */
   int milli; /* thousandths of a unit, like struct ins_rec's */
   int edit;
};

/* Open a fresh form. `type` < 0 keeps the form's current type (what the
 * legacy "LOG INSULIN" entry point does); otherwise it is preset, which is
 * what the ADD menu's FAST and SLOW buttons do. `now` is the clock. */
void ins_form_open(struct ins_form *f, int type, long now);

/* Toggle SLOW <-> FAST. Re-populates the amount from the new type's history,
 * but only when this is a new dose. */
void ins_form_toggle_type(struct ins_form *f);

#endif
