// SPDX-License-Identifier: GPL-3.0
// shellstate.h --- carrying the screen across an activity or process death
// Copyright 2026 Jakob Kastelic

/* TWO FUNCTIONS, AND THE SHELL WANTS NOTHING ELSE.
 *
 * Android hands a native activity a saved-state buffer at onCreate and asks
 * for one back through onSaveInstanceState. Everything about WHICH screens and
 * drafts may travel that way, how they are encoded, and what to do with a blob
 * that arrives corrupt or from another version lives in shellstate.c -- see its
 * header for the whole argument, including what was lost before any of it
 * existed.
 *
 * main.c installs these directly as the activity callback and calls the other
 * from onCreate. It does not know the format and must not: the shell's job is
 * the window and the looper. */
#ifndef PANCRA_SHELLSTATE_H
#define PANCRA_SHELLSTATE_H

#include "ndk.h" /* struct ANativeActivity: the callback's own signature */
#include <stddef.h>

/* THE FRAMEWORK IS ASKING FOR THE STATE. Installed as
 * activity->callbacks->onSaveInstanceState, so its signature is Android's:
 * the buffer is malloc'd here and freed by the framework, and *outsz is set to
 * 0 with a NULL return when there is nothing worth keeping. */
void *shellstate_save(struct ANativeActivity *a, size_t *outsz);

/* THE FRAMEWORK IS HANDING IT BACK. Call from onCreate AFTER the durable data
 * is loaded: two of the validity checks are against data that has to be on
 * disk and in memory first. A blob that is absent, corrupt or from an
 * incompatible version leaves the shell on the main screen. */
void shellstate_restore(const void *saved, size_t nb);

#endif
