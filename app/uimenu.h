// SPDX-License-Identifier: GPL-3.0
// uimenu.h --- the main screen's shortcut buttons
// Copyright 2026 Jakob Kastelic
/*
 * DECLARED WHERE IT IS IMPLEMENTED. These lived in uifmt.h, a header of
 * presentation constants that had collected the helpers of four different
 * renderers -- so every file that wanted one number depended on all of
 * them, and the graph could not tell the pile from an interface.
 */
#ifndef PANCRA_UIMENU_H
#define PANCRA_UIMENU_H

#include "uimodel.h" /* struct ui_sensor: what these two ask about */

int ui_shortcut_code(int slot);
/* MA_* action, or 0 if slot is out of range */
/* The name to draw, chosen by how many buttons share the row: 1 gives the
 * full phrase, 2 the half form, 3 or more the shortest. */
const char *ui_shortcut_label(int slot, int percol);
/* The STABLE id a slot is stored as, and the inverse. The id belongs to the
 * domain (settings.h, enum shortcut_id); the code above belongs to this
 * renderer. Keeping them apart is what lets the touch codes be renumbered
 * without repointing somebody's pinned buttons. */
int ui_shortcut_id(int slot);
int ui_shortcut_slot_by_id(int id);
int ui_shortcut_sect(int slot);
/* The slot drawn k-th in the ADD menu, top to bottom; -1 past the last. The
 * main screen orders its pinned buttons by this so that a shortcut sits where
 * the menu taught the user to expect it, whatever order the pins were
 * ticked in. */
int ui_shortcut_menu_nth(int k);
/* How many slots there are to ask about. */
int ui_shortcut_count(void);


/* ---- two questions the menus answer about a device -------------------
 *
 * uimenu.c's, and declared here rather than in the family's shared private
 * header for the reason given in uidraw.h. */
const char *dev_state_abbrev(const char *st, char *out, int n);

int cgm_expired(const struct ui_sensor *s);

#endif
