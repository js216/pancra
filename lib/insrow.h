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

/* A dose is whole units. 1..99 is what the keypad accepts and what the log
 * carries; 0 is not a dose and three digits is not a dose anybody takes. */
#define INS_UNITS_MIN 1
#define INS_UNITS_MAX 99

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
   int units;
};

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
