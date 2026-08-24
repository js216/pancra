// SPDX-License-Identifier: GPL-3.0
// setpriv.h --- the persistence engine the settings modules share
// Copyright 2026 Jakob Kastelic

/* PRIVATE TO THE SETTINGS MODULES: app/settings.c, app/alarmcfg.c,
 * app/devinfo.c, app/paircode.c, app/remotecfg.c and app/setfile.c. Nothing
 * else includes it -- `make settingscheck` refuses that -- because what it
 * declares is the WRITE HALF of every settings transaction with the
 * transaction taken away, plus the live state itself.
 *
 * WHY THERE IS AN ENGINE AT ALL. Five files are persisted here, and each one
 * has to be written the same way: rendered under the state lock, written with
 * it RELEASED (a spin lock held across an fsync is every reader of the
 * preferences waiting on flash), stamped with a generation so an older render
 * cannot land on top of a newer one, and rolled back only when nothing newer
 * has happened since. That is four rules and they are the same four for every
 * file; one copy of them is why a new settings file cannot get three of the
 * four right.
 *
 * WHY THE DOMAINS ARE SEPARATE FILES. settings.c held all five: the device's
 * model and firmware strings, the alarm thresholds, the display preferences,
 * the pairing code and the remote credentials -- 1541 lines behind one save
 * engine, so a reader after the alarm thresholds got the sync identity's
 * lifetime with them. They share the engine and the aggregate; they share
 * nothing else.
 */
#ifndef PANCRA_SETPRIV_H
#define PANCRA_SETPRIV_H

/* FORWARD-DECLARED, NOT INCLUDED. settings.h is the preferences module's own
 * public header, and this one is that module's private engine: including it
 * here makes settings.h depend on setpriv.h and setpriv.h on settings.h --
 * one node in the include graph, and test/inclusions.py refuses the cycle
 * (rightly: neither file could then be read on its own). Every .c that uses
 * `g_p` includes settings.h for the layout; an extern needs only the name. */
struct prefs;

#include "thread.h"

/* ---- WHAT A SAVE IS ---------------------------------------------------
 *
 * A render turns the current state into one of these under set_lk; the
 * caller releases the lock and hands it to set_write_job. `gen` is what makes
 * two racing saves land in order, and `written` is the file's own record of
 * what is on disk -- one per file, function-static in its renderer. */
struct save_job {
   const char *path;
   char buf[264]; /* the remote line is the longest at 256 */
   int len;
   unsigned gen;
   int ok;            /* the render itself succeeded */
   unsigned *written; /* this file's newest generation already on disk */
};

typedef void (*render_fn)(struct save_job *);

/* THE LIVE STATE, and the lock over it. Every domain module reads and writes
 * these; they are here rather than in settings.h because a writable aggregate
 * in a public header is a setting anybody can change without saving it (see
 * settingscheck). */
extern struct prefs g_p;
extern struct mutex set_lk;

/* Fill `j` for one file. Bumps the generation; caller holds set_lk. */
void set_job_stamp(struct save_job *j, const char *path, unsigned *written,
                   int len, int ok);

/* Write a rendered job. CALLER HOLDS NOTHING. 0 on success; a job older than
 * what is already on disk is skipped and reported as success, because the
 * newer bytes it would have overwritten are the ones that should be there. */
int set_write_job(const struct save_job *j);


/* Change one field and persist it, rolling the field back if the write fails
 * and nothing newer landed meanwhile. SETTINGS_OK / SETTINGS_UNSAVED. */
int set_int_field(int *field, int val, render_fn render);
int set_str_field(char *field, int cap, const char *val, render_fn render);

#ifdef APP_FAULTS
/* The preferences renderer hands its own on-disk generation counter to the
 * engine, so settings_fault_written_gen() can answer a test. Fault build
 * only; nothing that ships defines APP_FAULTS. */
void set_fault_note_written(unsigned *written);
#endif

/* The newest generation any render has taken, for a module doing its own
 * render/write/roll-back transaction: a rollback applies only when this still
 * equals the job's own generation, or it would revert an edit that succeeded.
 * Caller holds set_lk. */
unsigned set_gen_now(void);

/* The version marker at the head of a settings file: 0 when there is none
 * (every file already deployed), -1 when the marker is not one this build
 * knows. `*rest` is advanced past it. */
int set_file_version(char *b, char **rest, int newest);

/* THE FIVE RENDERERS, each defined by the module that owns its file. They are
 * declared together because set_save_now takes one of them. */
void set_render_settings(struct save_job *j);
void set_render_alarm(struct save_job *j);
void set_render_info(struct save_job *j);
void set_render_code(struct save_job *j);
void set_render_remote(struct save_job *j);

/* WHERE EACH FILE LIVES. settings_paths() fills these; a module reads the one
 * it owns. Arrays rather than accessors because a renderer needs the string
 * and nothing outside these files sees them. */
extern char g_info_path[256];
extern char g_alarm_path[256];
extern char g_settings_path[256];
extern char g_code_path[256];
extern char g_remote_path[256];

/* THE SETTINGS FILE'S OWN FORMAT VERSION, and the remote file's. Here rather
 * than in their modules because set_file_version is given the newest one the
 * build knows and the two readers must agree with their writers. */
/* 2: the pin slots went from six to nine and took the field order with them.
 * v1 put best_streak_s at field 25 with six pins before it; v2 has all nine
 * pins at 19..27 and the streak at 28, which is the order they are declared
 * in. A v1 file read with this layout would take its streak for a pin, so the
 * loader refuses one outright rather than misreading it -- see settings_load.
 * There was exactly one v1 file in the world and it was converted in place. */
#define SETTINGS_VERSION 2
#define REMOTE_VERSION   1

#endif
