// SPDX-License-Identifier: GPL-3.0
// nav.h --- The navigation stack: where the user is, and how they got there
// Copyright 2026 Jakob Kastelic
//
/*
 * WHAT IT REPLACES. Every screen that could be reached from more than one
 * place carried its own "where did I come from" global -- g_sensor_from,
 * g_devices_from, g_pair_from, g_alarm_from, g_ins_from, g_inslog_from,
 * g_wt_from, g_wtlog_from -- captured by hand at each entry and read back by
 * hand at each exit. Eight variables, eight capture rules, and a back key that
 * is wrong the moment one of them is missed.
 *
 * They were also each guarded by a condition of the form "capture the origin
 * ONLY on a genuine external entry", because a screen re-entered from one of
 * its OWN sub-screens would otherwise record the sub-screen as its origin and
 * closing it would go round in a circle. That condition is a hand-maintained
 * list of which screens count as "outside" -- and it is exactly the thing that
 * gets forgotten when a new route to a screen appears.
 *
 * A PATH MAKES BOTH PROBLEMS GO AWAY. g_nav is the route the user actually
 * took, root first, so:
 *
 *   - the origin of a screen is not recorded anywhere: it is simply the entry
 *     below it, which cannot be stale or forgotten;
 *   - going to a screen ALREADY ON THE PATH is a RETURN, and nav_go pops back
 *     to it rather than pushing a second copy. That is the "external entry"
 *     condition, derived instead of maintained.
 *
 * The current screen is the top of the path; there is no separate variable for
 * it, because two representations of one fact is how this started.
 */
#ifndef NAV_H
#define NAV_H

#include "ui.h" /* enum ui_screen */

/* Deeper than any real route. On overflow the TOP is replaced rather than the
 * root dropped: losing the root would strand the user with no way back to the
 * main screen, which is the one failure worse than a wrong back target. */
#define NAV_MAX 12

/* The screen showing now. SCR_MAIN means no modal is open. */
enum ui_screen cur_screen(void);

/* Go to `to`: open it on top of the current screen, or -- if it is already on
 * the path -- RETURN to it, discarding everything above. That second case is
 * what replaces the hand-maintained "only capture the origin on a genuine
 * external entry" condition at every call site. */
void nav_go(enum ui_screen to);

/* Close the current screen, returning to whatever opened it. */
void nav_back(void);

/* Abandon the whole path (a flow that ends at the main screen). */
void nav_home(void);

/* 1 when `s` is on the path -- the user came THROUGH it to get here. */
int nav_has(enum ui_screen s);

/* A BOOKMARK, for flows that span several screens: take a mark when the flow
 * starts, return to it when it ends, and the landing place is right for every
 * route in -- including ones that skip a step. */
int nav_mark(void);
void nav_return_to(int mark);

/* ---- THE WHOLE PATH, FOR THE ONE THING THAT HAS TO PERSIST IT -------
 *
 * Android can destroy this activity -- and the process with it -- and put the
 * user back in front of it later believing nothing happened. Everything else
 * in this module deliberately keeps the path private, because two
 * representations of one fact is how the eight `g_*_from` globals it replaced
 * went wrong. These two exist for exactly one caller (the shell's
 * onSaveInstanceState / onCreate pair in main.c), which needs to hand the path
 * to the framework and get it back.
 *
 * `nav_path` copies at most `cap` entries, root first, and returns how many.
 * `nav_set_path` REPLACES the path; it refuses a length outside 1..NAV_MAX
 * rather than clamping, because a path with no root is a user with no way
 * back to the main screen. It does NOT check that the screens are ones worth
 * restoring -- that is a question about what data exists, which this module
 * knows nothing about, and it is answered in main.c after the durable state
 * has loaded. */
int nav_path(enum ui_screen *out, int cap);
void nav_set_path(const enum ui_screen *p, int n);

/* The current screen as a plain int at a FIXED address, for the crash handler
 * -- which reads its context through pointers and cannot call anything to
 * derive a value (see crashlog.h). A MIRROR, written only by this module, so
 * it cannot drift from the path. */
extern int g_screen_now;

#endif
