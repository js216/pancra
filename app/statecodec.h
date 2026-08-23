// SPDX-License-Identifier: GPL-3.0
// statecodec.h --- the saved-state blob, as a contract rather than a file
// Copyright 2026 Jakob Kastelic
//
/* app/shellstate.h is what the shell sees: two functions, one
 * called when Android asks for a blob and one when it hands one back. What is
 * actually worth testing is neither of those -- it is the CODEC between them,
 * and it was static inside shellstate.c, so the only way a test could reach it
 * was to `#include "shellstate.c"`.
 *
 * WHAT THAT COST, and it is why this header exists. A source-included module
 * is not the module that ships: it is compiled with the test's flags, into a
 * translation unit that also holds a hundred stubs, and it is excluded from
 * clang-tidy -- so the file with the most delicate parsing in the app was the
 * one file no linter ever looked at. Both units are compiled and linted
 * normally now, and the test reaches in through this door instead.
 *
 * WHY THE CODEC AND NOT THE ENTRY POINTS. state_encode/state_decode are pure:
 * text in, struct out, no framework, no window, no Java. That is a contract a
 * test can state exhaustively -- every version byte, every truncation, every
 * out-of-range field -- while shellstate_save/restore can only be driven by
 * pretending to be Android.
 *
 * NOT PUBLIC. app/shellstate.h is the interface the app uses. This header is
 * for app/shellstate.c and for the one suite that tests it; `make -f
 * test/Makefile settingscheck` names the exceptions so they stay written down
 * rather than assumed.
 */
#ifndef PANCRA_STATECODEC_H
#define PANCRA_STATECODEC_H

#include "keypad.h" /* enum keypad_mode: which draft was on show */
#include "loadresult.h"
#include "nav.h" /* NAV_MAX: the route is the shell's, not this codec's */
#include "uimodel.h"
#include <stddef.h>

/* The wire version, and the ceiling on a blob this build will even look at.
 * Android hands back whatever it stored, including something an older or
 * newer build wrote. */
#define STATE_VERSION 1
#define STATE_MAX     256

/* EVERYTHING THE SHELL WOULD LIKE BACK. The route the user was on, the
 * keypad's mode and the screen it closes to, and the two drafts that are a
 * typed number rather than a stored fact. */
struct saved_state {
   enum ui_screen path[NAV_MAX];
   int n;
   enum keypad_mode kp_mode;
   enum ui_screen kp_ret;
   long wt_t;
   int wt_tenths;
   long ins_t;
   int ins_type, ins_units;
   char entry[64];
};

/* Encode the CURRENT shell state into `out`. The byte count, or 0 when there
 * is nothing worth saving -- which is the ordinary case, and the reason the
 * framework is not handed a blob on every pause. */
int state_encode(char *out, int cap);

/* Decode a blob from the framework. Fills `*st` only on LOAD_OK; ABSENT is a
 * normal cold start and CORRUPT is a blob that exists and cannot be trusted.
 */
enum load_result state_decode(const void *blob, size_t nb,
                              struct saved_state *st);

#endif
