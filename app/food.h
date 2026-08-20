// SPDX-License-Identifier: GPL-3.0
// food.h --- Food log: a named vocabulary plus append-only entries
// Copyright 2026 Jakob Kastelic

/* TWO FILES, BECAUSE THERE ARE TWO DIFFERENT LIFETIMES HERE.
 *
 * A food ENTRY is "at 12:40 I ate 90 grams of PORRIDGE" -- an event, in the
 * same shape as an insulin dose or a weigh-in, and it goes in an append-only
 * log that is never rewritten.
 *
 * A food TYPE is the word PORRIDGE. It is a vocabulary the user builds up over
 * time and picks from, and it outlives any individual entry: naming a food and
 * then abandoning the form must still leave the name there to pick next time,
 * or adding one becomes a thing you have to get right in one go. Deriving the
 * vocabulary from the entries instead -- scanning the log for distinct names --
 * would make a type exist only once it had been used, which is precisely
 * backwards for the screen that exists to choose one BEFORE logging.
 *
 * So: foodtypes.csv is the vocabulary (id, name), food.csv is the entries
 * (when, which type, how much). Entries reference a type by ID rather than
 * repeating its text, so the name is stored once and a future rename stays
 * possible without rewriting a log that is never rewritten.
 */
#ifndef PANCRA_FOOD_H
#define PANCRA_FOOD_H

/* A food name, in characters, not counting the terminator.
 *
 * Bounded because it is typed on the letter keypad and drawn in a menu row
 * beside a value column -- and because it is written into a CSV field, which
 * is the constraint that actually bites. See food_type_add: the separator and
 * the row terminator are the two characters a name may not contain, and a name
 * is REFUSED rather than quietly stripped of them. */
#define FOOD_NAME_MAX 20

/* How many distinct foods the vocabulary holds. Generous for a personal list
 * -- the point of the cap is that the picker is a menu somebody scrolls, not
 * a database. */
#define NFOODTYPE 64

/* In-memory tail of the entry log; the file keeps everything. */
#define NFOOD 256

/* Grams in one entry. The lower bound is 1 rather than 0 for the same reason
 * the exercise log has no zero: an entry of nothing is not an entry. The upper
 * bound is loose on purpose -- it exists to stop a corrupt digit run becoming
 * a plausible-looking meal, not to have an opinion about portion sizes. */
#define FOOD_MIN_G 1L
#define FOOD_MAX_G 20000L

/* Epoch bound on an entry instant: same rationale and value as INS_T_MAX,
 * WT_T_MAX and EX_T_MAX. */
#define FOOD_T_MAX 32503680000L

/* The id a type never has. Entries carrying it are damage. */
#define FOOD_TYPE_NONE 0

struct food_type {
   int id; /* >= 1, assigned in arrival order and never reused */
   char name[FOOD_NAME_MAX + 1];
};

struct food_rec {
   long t;    /* entry instant, epoch seconds */
   int type;  /* a food_type id */
   long g;    /* grams */
};

/* ---- the vocabulary ---- */

int food_type_count(void);
/* The i-th type, in the order they were added. Out of range yields a zeroed
 * record, so a caller that gets its bounds wrong draws nothing. */
struct food_type food_type_at(int i);
/* The name for an id, or "" when no type has it -- which is what an entry
 * referencing a type that is not there renders as, rather than nothing. */
const char *food_type_name(int id);
/* The index of the type with this id, or -1. For the picker, which highlights
 * the one the form currently holds. */
int food_type_index(int id);
/* Copy up to `cap` types, in the order they were added; returns how many.
 * For the frame, which needs a snapshot that cannot change while it is drawn
 * -- adding a food from the picker rewrites this table. */
int food_type_copy(struct food_type *out, int cap);

/* Add a name to the vocabulary and return its id, or -1.
 *
 * IDEMPOTENT ON THE NAME: adding one that is already there returns the
 * existing id rather than a second type with the same word. Two types spelled
 * identically would be indistinguishable in the picker and would split the
 * history of one food across two ids, which no later edit could join up.
 *
 * REFUSES rather than repairs: an empty name, one longer than FOOD_NAME_MAX,
 * or one containing a ',' or a newline. Silently stripping the separator would
 * store a name the user did not type and cannot retype; silently truncating
 * would make two long names collide. When the vocabulary is full it also
 * refuses -- the caller says so, rather than the newest food quietly replacing
 * the oldest. */
int food_type_add(const char *name);

/* ---- the entries ---- */

int food_count(void);
struct food_rec food_at(int i);
struct food_rec food_newest(void);
int food_copy(struct food_rec *out, int cap);

const char *food_path(void);
const char *food_types_path(void);
/* Point both files at the data directory. 1 when EVERY path fitted, 0 when one
 * did not -- and then none of them is usable, see data_path in util.h. */
int food_paths(const char *dir);

/* Load both files. 0 read whole (or nothing to read), -1 what was loaded is
 * INCOMPLETE -- weight_load carries the full argument for why those are
 * different answers and why a prefix is kept rather than discarded.
 *
 * THE VOCABULARY LOADS FIRST, because an entry naming a type is only checkable
 * once the types are known. An entry whose type is absent is damage: it is a
 * meal the app can no longer name, and saying so is better than drawing a
 * blank row. */
int food_load(void);

/* Append one entry durably and mirror it into the tail. 0 on success, -1 when
 * the write failed (the tail is then left untouched, so memory never claims an
 * entry the file does not have). Rejects an unknown type id and out-of-range
 * values. */
int food_append(long t, int type, long g, long tz);

#endif
