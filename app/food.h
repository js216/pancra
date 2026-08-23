// SPDX-License-Identifier: GPL-3.0
// food.h --- Food log: a named vocabulary plus editable entries
// Copyright 2026 Jakob Kastelic

/* TWO FILES, BECAUSE THERE ARE TWO DIFFERENT LIFETIMES HERE.
 *
 * A food ENTRY is "at 12:40 I ate 90 grams of PORRIDGE" -- an event, in the
 * same shape as an insulin dose or a weigh-in.
 *
 * TWO TIERS, AND THIS IS THE ORDINARY ONE (see the block below): capture
 * APPENDS, and an edit or a delete atomically REPLACES the whole file. The
 * immutable, never-rewritten discipline is insulin.h's alone.
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
 * repeating its text, so the name is stored once and a future rename would
 * not have to touch a single entry row.
 */
#ifndef PANCRA_FOOD_H
#define PANCRA_FOOD_H

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
   long t;   /* entry instant, epoch seconds */
   int type; /* a food_type id */
   long g;   /* grams */
};

/* ---- the vocabulary ---- */

/* ---- THE ORDER THESE THREE AGREE ON --------------------------------------
 *
 * MOST-EATEN FIRST, not the order they were added. A vocabulary grows and
 * never shrinks, so after a few months the foods somebody actually eats are
 * scattered through a list ordered by when they first thought of them; sorting
 * by how often each has been logged puts breakfast at the top. Ties keep
 * insertion order, so a list of foods eaten once each does not reshuffle
 * between frames, and a food never eaten sits at the bottom.
 *
 * THE INDEX MEANS THE SAME THING TO BOTH, which matters more than the
 * sorting: food_type_copy fills the frame's snapshot and food_type_at resolves
 * the row a finger landed on. If they disagreed about position 3, a tap would
 * log a food nobody chose. They read one table, built in food.c.
 *
 * IT MOVES WHEN THE LOG DOES -- logging a meal can change the order -- so an
 * index is only good for as long as the state it was read from. That is
 * already the rule here for a different reason (the vocabulary can grow
 * mid-frame), and forms.c resolves a tap to an ID immediately for exactly
 * that reason. */

/* The type shown at display position `i`. Out of range yields a zeroed
 * record, so a caller that gets its bounds wrong draws nothing.
 *
 * A PURE READ, as is food_type_copy below. Rebuilding the display order
 * lazily when a flag says it is stale makes a "read" mutate three file
 * statics and makes the answer depend on which reads came before it. The
 * order is published by whatever CHANGES it. */
struct food_type food_type_at(int i);
/* The name for an id, or "" when no type has it -- which is what an entry
 * referencing a type that is not there renders as, rather than nothing.
 * BY ID: this is not a display position, so it does not move when the order
 * does. */
const char *food_type_name(int id);
/* Copy up to `cap` types, in display order; returns how many.
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

/* THE PORTION THIS FOOD WAS LAST LOGGED WITH, in grams, or 0 if it never has
 * been.
 *
 * What it is for: a person eats the same things in the same amounts, so a form
 * that starts at zero asks them to retype a number they have typed before,
 * every time. The entry form seeds itself from this the moment a food is
 * picked (forms.c).
 *
 * NEWEST FIRST, and the tail is the only place it looks: the log holds
 * everything but this is a convenience, not a fact anybody depends on, and
 * reading the whole file on a form open would put disk I/O on a tap. A food
 * last eaten more than NFOOD entries ago therefore answers 0 -- which is the
 * same answer as "never", and means the same thing to the caller: nothing to
 * suggest. 0 is not a portion (FOOD_MIN_G is 1), so it cannot be confused with
 * a real one. */
long food_last_grams(int type_id);
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

/* Rewrite the LAST file row matching `orig` (instant, type and grams) to the
 * new values, or delete it. Rewrite-and-rename, so a crash never truncates the
 * log; the tail is reloaded afterwards. 0 on success, -1 on failure or when no
 * row matches.
 *
 * These exist for the EDIT FOOD form only -- every other writer here APPENDS,
 * and nothing but these two replaces the file. Matching is by CONTENT, not
 * position, so a stale tail index can never touch the wrong entry: the table
 * renders from a snapshot that a concurrent append can have moved underneath
 * it. Exactly weight.c's contract, and the same reasoning. */
int food_update(const struct food_rec *orig, long t, int type, long g, long tz);
int food_delete(const struct food_rec *orig);

#endif
