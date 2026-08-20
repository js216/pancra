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
/* THE PINNABLE-ACTION TABLE: which ADD-menu actions the PIN column may pin to
 * the main screen, in ADD-menu order. Exported because the shell has to turn a
 * tapped
 * checkbox (MA_SCTOGGLE + slot) into the MA_* code it persists, and deriving
 * that mapping twice is how the stored code and the drawn row drift apart.
 * `abbrev` gives the short label the main screen falls back to when two or
 * three buttons share the row -- the font never shrinks, the words do. */
int ui_shortcut_count(void);
/* MA_* action, or 0 if slot is out of range */
const char *ui_shortcut_label(int slot, int abbrev);
/* The slot holding `code`, or -1. The inverse of ui_shortcut_code, for
 * rendering what the persisted codes point at. */
int ui_shortcut_slot(int code);
/* The STABLE id a slot is stored as, and the inverse. The id belongs to the
 * domain (settings.h, enum shortcut_id); the code above belongs to this
 * renderer. Keeping them apart is what lets the touch codes be renumbered
 * without repointing somebody's pinned buttons. */
int ui_shortcut_id(int slot);
int ui_shortcut_slot_by_id(int id);

/* ---- two questions the menus answer about a device -------------------
 *
 * uimenu.c's, and declared here rather than in the family's shared private
 * header for the reason given in uidraw.h. */
const char *dev_state_abbrev(const char *st, char *out, int n);

int cgm_expired(const struct ui_sensor *s);

#endif
