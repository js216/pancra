// SPDX-License-Identifier: GPL-3.0
// uikeypad.h --- what the keypad screens can hold
// Copyright 2026 Jakob Kastelic
/*
 * DECLARED WHERE IT IS IMPLEMENTED. These lived in uifmt.h, a header of
 * presentation constants that had collected the helpers of four different
 * renderers -- so every file that wanted one number depended on all of
 * them, and the graph could not tell the pile from an interface.
 */
#ifndef PANCRA_UIKEYPAD_H
#define PANCRA_UIKEYPAD_H

int ui_label_nchars(void);

/* (The per-mode slot-count wrapper is gone. It was a function wrapping
 * kp_slots(enum keypad_mode) -- a published interface with exactly ONE
 * caller, inside uikeypad.c itself, whose only effect was to launder a
 * keypad mode through an int on the last hop to the renderer. Its comment
 * said the shell asked it through this header; the shell called kp_slots
 * directly. Ask keypad.h.) */

#endif
