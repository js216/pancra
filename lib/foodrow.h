// SPDX-License-Identifier: GPL-3.0
// foodrow.h --- decoders for one row of the food log and of its vocabulary
// Copyright 2026 Jakob Kastelic

/* ONE FORMAT, ONE READER, BOTH SIDES -- the reason insrow.h gives, for the
 * food log. The phone writes food.csv and foodtypes.csv and the server
 * renders the rows it syncs, so both have to agree on which rows are rows.
 *
 * An entry is "t,type,grams,tz"; a type is "id,name,carbs,protein,fat,kcal".
 * Whether an entry's type exists, and whether two types share an id, are
 * questions about the whole vocabulary, so they are each side's own. */
#ifndef PANCRA_FOODROW_H
#define PANCRA_FOODROW_H

/* A food name, in characters, not counting the terminator.
 *
 * Bounded because it is typed on the letter keypad and drawn in a menu row
 * beside a value column -- and because it is written into a CSV field, which
 * is the constraint that actually bites. See food_type_add: the separator and
 * the row terminator are the two characters a name may not contain, and a name
 * is REFUSED rather than quietly stripped of them. */
#define FOOD_NAME_MAX 20

/* Grams in one entry.
 *
 * The lower bound is 1 rather than 0 for the same reason the exercise log has
 * no zero: an entry of nothing is not an entry.
 *
 * THE UPPER BOUND IS A PORTION SIZE, and it is meant to be. A kilogram in one
 * sitting is not a meal anybody logs; three digits covers every real portion
 * with room to spare, and what falls outside them is a slipped finger or a
 * corrupt digit run rather than lunch. Keeping the ceiling at the size of a
 * plausible meal is what lets the keypad offer three slots and every readout
 * that prints grams reserve three columns -- a looser bound costs width on
 * screens that have none to spare, to hold space no entry ever uses. */
#define FOOD_MIN_G 1L
#define FOOD_MAX_G 999L

/* Epoch bound on an entry instant: same rationale and value as INS_T_MAX,
 * WT_T_MAX and EX_T_MAX. */
#define FOOD_T_MAX 32503680000L

/* The id a type never has. Entries carrying it are damage. */
#define FOOD_TYPE_NONE 0

/* ---- WHAT A FOOD IS MADE OF --------------------------------------------
 *
 * PER GRAM OF THE FOOD, and a property of the FOOD, not of any one entry:
 * PORRIDGE is 0.12 carbohydrate whichever morning it was eaten, so changing
 * it changes it for every entry that names PORRIDGE, past ones included. That
 * is why the numbers live on the type, in foodtypes.csv, and an entry is still
 * only "when, which food, how many grams".
 *
 * THOUSANDTHS, like an insulin dose (insrow.h): the fractions are written as a
 * person writes them, "0.454", and a value that has to round-trip through a
 * text file exactly cannot be a binary float. Carbohydrate, protein and fat
 * are fractions of the food's weight, 0..1; energy is kcal per gram, 0..9.999
 * -- pure fat is about 9, the most a food can be. A new food is all zeros
 * until somebody says otherwise. */
enum { FOOD_CARBS, FOOD_PROTEIN, FOOD_FAT, FOOD_KCAL, FOOD_NMACRO };

#define FOOD_FRAC_MAX 1000 /* a fraction's thousandths: 1.000, all of it */
#define FOOD_KCAL_MAX 9999 /* kcal per gram, thousandths: 9.999 */

struct food_type {
   int id; /* >= 1, assigned in arrival order and never reused */
   char name[FOOD_NAME_MAX + 1];
   int macro[FOOD_NMACRO]; /* thousandths, see above */
};

/* 1 when `milli` is a value macro `which` can hold. */
int food_macro_ok(int which, int milli);

struct food_rec {
   long t;   /* entry instant, epoch seconds */
   int type; /* a food_type id */
   long g;   /* grams */
};

/* Is this a name that can be written to a CSV row and read back unchanged?
 *
 * ',' and '\n' are the field separator and the row terminator, so a name
 * holding either would not survive the round trip: the comma would split one
 * name into two fields and the newline would split one row into two. A name
 * holding a '#' at the start would be read back as the header comment.
 *
 * REFUSED, NOT REPAIRED. Stripping the character would store a name the user
 * did not type and cannot type again to match; escaping it would make this the
 * only field in the app's file formats that needs an unescaper. The keypad
 * offers no comma, so in practice this rejects only what arrives from a file
 * that something else wrote. */
int food_name_ok(const char *n);

/* Decode ONE entry row, from `p` to `e` (the end of the LINE). 1 when its
 * instant, grams and type id are in range, 0 otherwise. */
int food_entry_decode(const char *p, const char *e, struct food_rec *r);

/* Decode ONE vocabulary row. A row that stops after the name was written
 * before foods had values, and reads as all zeros; a row with some but not
 * all four is damage. 1 when it is a type this format allows, with the id,
 * the name (FOOD_NAME_MAX + 1 bytes) and the four values written out. */
int food_type_decode(const char *p, const char *e, int *id_out, char *name_out,
                     int macro_out[FOOD_NMACRO]);

#endif
