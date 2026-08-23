// SPDX-License-Identifier: GPL-3.0
// loadresult.h --- what happened when a persisted file was read
// Copyright 2026 Jakob Kastelic

/* FOUR ANSWERS, BECAUSE THERE ARE FOUR SITUATIONS.
 *
 * A loader that returns void collapses three quite different things into
 * "the state is now whatever it is":
 *
 *   - the file is not there, which on a first run is CORRECT and means the
 *     compiled defaults apply;
 *   - the file is there and could not be READ (a permission change, a
 *     filesystem going read-only, an I/O error);
 *   - the file is there and could not be UNDERSTOOD (truncated by a power
 *     loss mid-write, or written by a version that is not this one).
 *
 * The second and third are storage failures. Reported as the first, the app
 * starts up looking exactly like a fresh install -- which is the one thing it
 * is not -- and silently applies defaults over settings the user chose, an
 * alarm threshold they set, or a paired identity they will now be asked to
 * establish again. Nothing on screen says the phone lost anything, because
 * nothing in the code ever knew.
 *
 * A loader that answers with one of these lets startup tell a clean first run
 * from degraded storage, and say so.
 *
 * ORDERED BY SEVERITY on purpose, so a caller can keep the worst of several
 * loads with a comparison rather than a chain of tests. */
#ifndef PANCRA_LOADRESULT_H
#define PANCRA_LOADRESULT_H

enum load_result {
   /* No file at all. A first run; the defaults are the right state and
    * nothing was lost. */
   LOAD_ABSENT = 0,
   /* Read and understood. */
   LOAD_OK = 1,
   /* The file exists and was read, but is not something this build can parse
    * whole -- truncated, or from a schema this version does not know. What
    * could be read has been applied; the rest is at its default. */
   LOAD_CORRUPT = 2,
   /* The file exists and could NOT be read. Nothing was applied, and the
    * state is whatever it was before -- which at startup is the defaults. */
   LOAD_ERROR = 3
};

/* The worse of two outcomes, for a caller that runs several loads and wants
 * one answer about the storage as a whole. */
static inline enum load_result load_worse(enum load_result a,
                                          enum load_result b)
{
   return a > b ? a : b;
}

/* For a status line or a log: never NULL. */
static inline const char *load_result_name(enum load_result r)
{
   switch (r) {
      case LOAD_ABSENT: return "absent";
      case LOAD_OK: return "ok";
      case LOAD_CORRUPT: return "CORRUPT";
      case LOAD_ERROR: return "UNREADABLE";
   }
   return "?";
}

#endif
