// SPDX-License-Identifier: GPL-3.0
// insrow.h --- one decoder for one row of the insulin assertion log
// Copyright 2026 Jakob Kastelic

/* ONE FORMAT, ONE READER, BOTH SIDES.
 *
 * The insulin log is append-only ASSERTIONS: each row names a dose by id, and
 * a later row for the same id corrects or retracts it. The phone writes it and
 * the server renders it, so both replay it. Decoded separately, by hand, on
 * each side, the two drift -- which is what this file is for:
 *
 *   the phone   required every numeric field to be exactly a number, and a
 *               retraction flag to be exactly 0 or 1;
 *   the server  used strtoll, which takes a numeric PREFIX -- so "12abc" was
 *               12 and "abc" was 0 -- and treated every nonzero `del` as a
 *               retraction.
 *
 * So a row the phone refuses to show could be rendered by the server as a dose
 * the person had taken, and one the phone shows as retracted could be shown by
 * the server as live. On a medical record read by the person it belongs to,
 * either direction is the same failure: the two halves disagreeing about what
 * was injected.
 *
 * WHAT IS HERE: the grammar, the ranges, the two dialects, and the
 * last-wins-per-id rule that replays them. WHAT IS NOT: where the rows come
 * from (a file on the phone, a table on the server) and what is done with the
 * result (a bounded tail, a rendered page). Those are the two sides' own
 * business and are why this is a leaf rather than a module either of them
 * owns.
 */
#ifndef PANCRA_INSROW_H
#define PANCRA_INSROW_H

/* ---- THE DOMAIN, stated once ------------------------------------------
 *
 * These were in app/insulin.h with the server repeating them as literals
 * beside a comment saying they are protocol and must be changed together.
 * They are here now, so there is nothing to keep in step. */
#define INS_SLOW 0 /* basal / long-acting */
#define INS_FAST 1 /* bolus / rapid-acting */

/* A DOSE IS A DECIMAL NUMBER OF UNITS, held as THOUSANDTHS.
 *
 * Half units are how insulin is actually dosed -- 0.5, 1.5, 16.5 -- so a
 * whole-number field cannot carry the record: it either drops the small
 * boluses or states every half-unit basal a half low. The file writes what a
 * person writes ("20", "0.5", "16.5") and this is the integer that text
 * scales to, because a quantity that must round-trip through a text file
 * exactly cannot live in a binary float (0.1 is not representable in one).
 *
 * MILLI-, not tenths: 0.7 is in the record already, and a scale that only
 * just covers today's doses is one that has to be widened the first time a
 * pen measures finer. Three places costs nothing here.
 *
 * OLD ROWS NEED NO MIGRATION. A legacy "20" is a decimal that happens to have
 * no fraction, so it reads as 20000 through the same parser -- the column's
 * meaning did not change, only the set of values it can express. */
#define INS_MILLI 1000
#define INS_UNITS_MAX 99
/* 0 is not a dose; anything above zero is one, down to the last place the
 * format carries. Three digits of units is not a dose anybody takes. */
#define INS_MILLI_MIN 1
#define INS_MILLI_MAX (INS_UNITS_MAX * INS_MILLI)

/* The year 3000, in epoch seconds: a stamp past this is not a time anybody
 * injected at, it is a corrupted field. */
#define INS_T_MAX 32503680000L

/* ONE ASSERTION ABOUT ONE DOSE.
 *
 * `id` names the dose. A row in the ORIGINAL four-field dialect carries no
 * id, so the replay gives it a negative one by file order -- the two spaces
 * cannot collide, however the file was assembled.
 *
 * `del` retracts the dose named by `id`; when it is set, `t`, `type` and
 * `units` describe the dose being retracted and are not required to be
 * anything in particular (the phone writes them so the row still reads on its
 * own; the log is a history, not a diff). */
struct ins_row {
   long id;
   int del;
   long t;
   int type;
   int milli; /* thousandths of a unit: 500 is half a unit */
};

/* Render `milli` the way a person writes it -- "20", "0.5", "16.5" -- with no
 * trailing zeros and no decimal point when there is no fraction. Returns the
 * length written. Both halves of the app and the server format a dose through
 * this, so a value cannot read one way on a phone and another on a page. */
int ins_units_str(int milli, char *out, int cap);

/* Decode ONE row, from `p` to `e` (the end of the LINE, not of the file).
 *
 * `legacy_id` is the id to give a four-field row -- callers pass a negative,
 * decreasing number.
 *
 * 1 when the row is an assertion this format allows, 0 when it is not: a
 * missing field, a field that is not exactly a number, a retraction flag that
 * is neither 0 nor 1, an id of zero, or a time / type / dose out of range. A
 * refused row is data neither half may render.
 */
int ins_row_decode(const char *p, const char *e, long legacy_id,
                   struct ins_row *out);

#endif
