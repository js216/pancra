// SPDX-License-Identifier: GPL-3.0
// menu.h --- Every tap that is not typing: what each screen's controls do
// Copyright 2026 Jakob Kastelic
//
/* ONE TAP, ONE TRANSITION.
 *
 * A tap arrives from the shell as an MA_* action code and an index. This file
 * turns it into a change: a screen, a setting, a device operation, a
 * connection. It is split into one dispatcher PER SCREEN FAMILY, each
 * returning 1 when the action was its own, so adding a screen never means
 * editing one thousand-line branch.
 *
 * THE RULE THIS CODEBASE KEEPS RE-LEARNING, stated once here: an exit target
 * is RECORDED, never inferred. Every screen reachable from more than one
 * place returns to the entry BELOW it on the navigation path (nav.h), and
 * multi-screen flows take a nav_mark() when they begin. Six separate bugs
 * came from hardcoding "back goes to SETTINGS" in a screen that had grown a
 * second door.
 *
 * WHAT IS NOT HERE: typing (forms.h), pairing (pairing.h), and the alarm's
 * own actuation (alarm.h). Those are workflows with state of their own; these
 * are transitions.
 */
#ifndef MENU_H
#define MENU_H

#include "menuview.h" /* the read-only view the frame gets */

/* The DEVICE the menus act on and the TYPE the ADD flow offers are
 * selection.h's -- shared UI state that neither this module nor the workflows
 * it drives own. Reading them back through the menu is what put a ring
 * through menu, device, pairing and reconcile. */

/* Perform one action. `ix` is the row or value the control carried. */
void menu_action(int action, int ix);

/* The action the BACK KEY means on the current screen, or -1 for none.
 * `*ix` receives its index. Derived from the screen, not from a table that
 * has to be kept in step with one. */
int menu_back_code(int *ix);

/* Sample the system states the settings screen shows (permissions, battery
 * exemption, standby bucket). MAIN THREAD ONLY -- JNI through the activity's
 * env is only legal there, so a render (which can be requested from a BLE
 * binder thread) must never call it; it is sampled on menu-open and after an
 * action, and the renderer just copies the cache. */
void sys_refresh(void);

/* What sys_refresh sampled, and the EXPORT checkboxes, are PRIVATE to menu.c
 * and reach the screen as a read-only snapshot: see menuview.h. They used to
 * be seven writable globals that every file including this header could set,
 * with no chance to sample the system state or to persist a choice. */

#endif
