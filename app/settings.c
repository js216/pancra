// SPDX-License-Identifier: GPL-3.0
// settings.c --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

/* THE DISPLAY AND MENU PREFERENCES, and nothing else.
 *
 * Five persisted files in one module -- the device's model and firmware, the
 * alarm thresholds, these preferences, the pairing code and the remote
 * credentials -- is 1541 lines, five unrelated lifetimes and one save engine.
 * Each of the other four is its own module (alarmcfg.c,
 * devinfo.c, paircode.c, remotecfg.c) and the engine they share is
 * setfile.c behind app/setpriv.h.
 *
 * What is left here is the preferences file itself: what the screen draws
 * with, what the menus toggle, and the shortcut row. The UI (main.c) owns
 * when to save and load; this owns the format. */
#include "settings.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: alarm_load's bound = the keypad's */
#include "loadresult.h"
#include "log.h" /* LOGI/LOGW: the ONE declaration */
#include "plot.h"
#include "setpriv.h" /* the shared engine and the live aggregate */
#include "style.h"
#include "thread.h" /* set_lk: the DIS strings arrive on a binder thread */
#include "util.h"
#include "weight.h" /* WT_KG / WT_LB: the weight display unit */
#include <stdio.h>  /* snprintf */

/* A config save that fails is a setting that silently reverts on the next
 * launch -- and one of the five below is the ALARM THRESHOLDS. An empty
 * `if (write(...) != n) { }` is the shape of a check that was written and
 * never finished. Say it. */

/* THE TWO BANDS, NESTED, AND THE NUDGE IS THE OUTER ONE. Chosen by the user,
 * not derived: alarm 70/300 is the conservative "act now" band that should be
 * set once and left alone, and nudge 85/250 sits outside it as the early
 * "have a look" that fires first and can be ignored.
 *
 * That nesting is the whole feature. Editing the ALARM threshold day to day --
 * down after a meal, back up when unaware -- is the habit the nudge exists to
 * retire, because its failure mode is the dangerous one: the alarm gets left
 * parked somewhere it can no longer help while the user goes on believing a
 * reminder is armed. Anything that changes these numbers must preserve
 * nudge_low >= alarm_low and nudge_high <= alarm_high, or the nudge is inside
 * the alarm and can never fire first (nudge_fire suppresses it under a
 * sounding alarm).
 *
 * Defaults only. A fresh install has no alarm.cfg; every existing file
 * overrides all four on load. */

/* Both ON by default: with the nudge band armed out of the box, an alert the
 * user has to go and switch on in a second place would just be a way for it
 * to be silently missing. */

/* Weight display unit. Pounds by default because that is what the migrated
 * log was kept in; the file itself is always grams, so this only chooses how
 * the numbers are rendered (weight.h). */

/* Insulin plot styling, PER TYPE (index INS_SLOW / INS_FAST): marker
 * shape, ui palette colour, marker size. Defaults: crosses, SLOW white,
 * FAST blue (matching the log table's blue FAST rows). */

/* PINNED BY DEFAULT on a fresh install: SLOW insulin and WEIGHT.
 *
 * Those are the two things logged on a schedule rather than in reaction to
 * something -- once a day, at roughly the same hour -- so they are the two the
 * main screen can save a trip through the ADD menu for on the very first day,
 * before the user has found the PIN column to ask. FAST is deliberately not
 * among them: a correction dose is logged when it happens, not on a routine,
 * and the third slot is left free so the first pin the user chooses has
 * somewhere to go.
 *
 * These are MA_* action codes (see settings.h on why positions are not
 * stored); ui.h is included for exactly these two names. A settings file
 * written before the shortcut fields existed still loads with none pinned --
 * that is an upgrade, not a first install, and silently adding buttons to a
 * main screen someone already knows is not a default, it is a surprise. */

/* Defaulted, not blank: a fresh install that has to be told the server before
 * it can do anything is a fresh install that mostly does not get told. Anyone
 * running their own instance edits one row. */

/* THE PREFERENCES, and the ONLY copy of them.
 *
 * Every field leaves this file only as a COPY (settings_get): a
 * preference that anything could write is a preference that can be changed
 * without being SAVED, and the difference is invisible until the app
 * restarts and reverts what the user chose. Writes go through the
 * settings_set_* operations below, each of which stores and persists in one
 * call.
 *
 * These are DEFAULTS ONLY. A fresh install has no settings file; every
 * existing file overrides what it carries on load. */
int settings_set_units(int mmol)
{
   return set_int_field(&g_p.units, mmol ? 1 : 0, set_render_settings);
}

int settings_set_wunits(int wu)
{
   return set_int_field(&g_p.wunits, wu, set_render_settings);
}

int settings_set_sound(int on)
{
   return set_int_field(&g_p.sound_on, on ? 1 : 0, set_render_settings);
}

/* THE CYCLES. How many states each of these has is a fact about the SETTING,
 * not about the row that taps it, and is not wrap arithmetic spelled out in
 * menu_action three different ways. */
int settings_cycle_orient(void)
{
   return set_int_field(&g_p.orient, (g_p.orient + 1) % 4, set_render_settings);
}

int settings_cycle_disc(void)
{
   return set_int_field(&g_p.disc, (int)(((unsigned)g_p.disc + 1U) & 3U),
                        set_render_settings);
}

int settings_cycle_newdata(void)
{
   /* OFF -> BEEP -> CHIRP -> OFF, as a named step over the mode's own type
    *. The stored field is an int because that is what the file
    * holds and what set_int rolls back on a failed write; it becomes a mode
    * on the way in and an int again on the way out, and the wrap is not
    * modular arithmetic on a number that only looks like one. */
   return set_int_field(&g_p.newdata_mode,
                        (int)nudge_mode_next(nudge_mode_of(g_p.newdata_mode)),
                        set_render_settings);
}

int settings_set_vib(int on)
{
   return set_int_field(&g_p.vib_on, on ? 1 : 0, set_render_settings);
}

int settings_set_screen_on(int on)
{
   return set_int_field(&g_p.screen_on, on ? 1 : 0, set_render_settings);
}

int settings_set_statbar(int on)
{
   return set_int_field(&g_p.statbar_val, on ? 1 : 0, set_render_settings);
}

int settings_set_lockscr(int on)
{
   return set_int_field(&g_p.lockscr_val, on ? 1 : 0, set_render_settings);
}

int settings_set_nudge_sound(int on)
{
   return set_int_field(&g_p.nudge_sound, on ? 1 : 0, set_render_settings);
}

int settings_set_nudge_vib(int on)
{
   return set_int_field(&g_p.nudge_vib, on ? 1 : 0, set_render_settings);
}

int settings_set_best_streak(int seconds)
{
   if (seconds <= 0 || seconds > BEST_STREAK_MAX)
      return SETTINGS_OK; /* nothing worth recording; not an error */
   /* READ, COMPARE, THEN WRITE. Without the comparison this would rewrite the
    * settings file on every reading of every streak, which is a file write
    * every five minutes for a number that changes once. The read is a
    * snapshot copy like every other reader here. */
   struct prefs p;
   settings_get(&p);
   if (seconds <= p.best_streak_s)
      return SETTINGS_OK;
   return set_int_field(&g_p.best_streak_s, seconds, set_render_settings);
}

int settings_set_plot_max(int mgdl)
{
   /* The RANGE lives here, with settings_load's own clamp, rather than at the
    * keypad that happens to type it. */
   if (mgdl < 100)
      mgdl = 100;
   if (mgdl > 400)
      mgdl = 400;
   /* NOTHING DERIVED TO APPLY. A renderer keeping the scale in a process
    * global that this has to push into (plot_set_max) is why two plots could
    * not have different scales and why the touch path answered against
    * whichever was drawn last. The scale is passed to each render and each
    * hit test, read from this setting at the call. */
   return set_int_field(&g_p.plot_max, mgdl, set_render_settings);
}

/* THE FOUR THRESHOLDS, one at a time. The CALLER decides whether the pair is
 * still ordered -- that check has to share a critical section with the read of
 * the partner value, and the alarm lock that provides it lives in alarm.c --
 * and the caller also calls alarm_save() once the pair is settled, which is
 * why these four store without persisting. */
int settings_set_ins_style(int type, int marker, int color, int size)
{
   if (type < 0 || type > 1)
      return SETTINGS_UNSAVED;
   mutex_lock(&set_lk);
   int old_m = g_p.ins_marker[type];
   int old_c = g_p.ins_color[type];
   int old_s = g_p.ins_size[type];
   if (marker >= 0)
      g_p.ins_marker[type] = marker;
   if (color >= 0)
      g_p.ins_color[type] = color;
   if (size >= 1)
      g_p.ins_size[type] = size;
   struct save_job j;
   set_render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) { /* see set_int */
         g_p.ins_marker[type] = old_m;
         g_p.ins_color[type]  = old_c;
         g_p.ins_size[type]   = old_s;
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* THE PINNED SHORTCUTS, as the two operations the UI actually performs.
 *
 * The list is DENSE -- the main screen's button row walks it until the first
 * empty slot -- so removing one has to close the gap behind it. That
 * compaction was written out at the call site, in a menu dispatcher, next to
 * the touch-code arithmetic; a pin list with a hole in it silently loses
 * every button after the hole.
 *
 * The WHOLE LIST is the undo unit: a compaction that is half rolled back
 * duplicates one pin and drops another. */
int settings_pin_add(int id)
{
   mutex_lock(&set_lk);
   int n = 0;
   for (int i = 0; i < SC_MAX; i++)
      if (g_p.shortcut[i] > SC_NONE)
         n++;
   if (n >= SC_MAX) {
      mutex_unlock(&set_lk);
      return SETTINGS_FULL; /* REFUSED, not evicted: the user picked these */
   }
   int old[SC_MAX];
   for (int i = 0; i < SC_MAX; i++)
      old[i] = g_p.shortcut[i];
   g_p.shortcut[n] = id;
   struct save_job j;
   set_render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) /* see set_int */
         for (int i = 0; i < SC_MAX; i++)
            g_p.shortcut[i] = old[i];
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

int settings_pin_remove(int id)
{
   mutex_lock(&set_lk);
   int at = -1;
   for (int i = 0; i < SC_MAX; i++)
      if (g_p.shortcut[i] == id)
         at = i;
   if (at < 0) {
      mutex_unlock(&set_lk);
      return SETTINGS_OK; /* not pinned: nothing to store, nothing to lose */
   }
   int old[SC_MAX];
   for (int i = 0; i < SC_MAX; i++)
      old[i] = g_p.shortcut[i];
   for (int i = at; i < SC_MAX - 1; i++)
      g_p.shortcut[i] = g_p.shortcut[i + 1];
   g_p.shortcut[SC_MAX - 1] = SC_NONE;
   struct save_job j;
   set_render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) /* see set_int */
         for (int i = 0; i < SC_MAX; i++)
            g_p.shortcut[i] = old[i];
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

int settings_pinned(int id)
{
   mutex_lock(&set_lk);
   int yes = 0;
   for (int i = 0; i < SC_MAX; i++)
      if (g_p.shortcut[i] == id)
         yes = 1;
   mutex_unlock(&set_lk);
   return yes;
}

/* The sync SERVER and ACCOUNT, copied in bounded and persisted. Both were
 * written character by character at the call site. */
/* THE PAIRING CODE the user typed, stored and persisted in one call. The code
 * is what the J-PAKE exchange proves knowledge of, so a code that is changed
 * but not saved pairs once and then fails silently after the next launch. */
void set_render_settings(struct save_job *j)
{
   static unsigned written;
#ifdef APP_FAULTS
   /* THE GENERATION THE SETTINGS FILE IS KNOWN TO HOLD, reachable by a test
    * and by nothing that ships. Recorded here rather than
    * exported, because `written` is a function-static on purpose: one
    * counter per FILE, and nothing outside its renderer may write it. */
   set_fault_note_written(&written);
#endif
   /* 192, MATCHING settings_load's READER -- the two numbers move together or
    * not at all. At 96 they disagreed the moment the field count grew, and a
    * truncated line does not fail loudly: clampn writes the prefix, the loader
    * parses what it finds and stops, and the tail fields silently revert to
    * their defaults on every launch.
    *
    * It was 128, which held 23 fields comfortably. Six pins rather than three
    * makes 26, and the parser accepts up to 9 digits per field, so the
    * headroom that made 128 obviously safe is no longer obvious. Raised on
    * both sides at once rather than measured against today's typical values,
    * because the failure this guards is silent and only appears on the launch
    * AFTER the one that wrote the long line. */
   int n = snprintf(
       j->buf, sizeof j->buf,
       "v%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
       "%d %d %d %d %d\n",
       SETTINGS_VERSION, g_p.sound_on, g_p.vib_on, g_p.orient, g_p.units,
       g_p.disc, g_p.plot_max, g_p.screen_on, g_p.newdata_mode,
       g_p.ins_marker[0], g_p.ins_color[0], g_p.ins_size[0], g_p.ins_marker[1],
       g_p.ins_color[1], g_p.ins_size[1], g_p.statbar_val, g_p.lockscr_val,
       g_p.nudge_sound, g_p.nudge_vib, g_p.wunits, g_p.shortcut[0],
       g_p.shortcut[1], g_p.shortcut[2], g_p.shortcut[3], g_p.shortcut[4],
       g_p.shortcut[5], g_p.best_streak_s);
   set_job_stamp(j, g_settings_path, &written, n, n > 0 && n < 192);
}

enum load_result settings_load(void)
{
   /* 192, and settings_render's length guard is the same number. See there. */
   /* ONE EXACT READ: the short-read loop, EINTR, and the
    * probe that tells a full buffer from a file longer than this build
    * can hold, all in read_file_exact rather than one unchecked read whose
    * return is taken as the file's length. */
   char b[192];
   int n               = 0;
   enum load_result rr = read_file_exact(g_settings_path, b, sizeof b, &n);
   if (rr != LOAD_OK)
      return rr;
   /* THE VERSION FIRST, and nothing applied until it is one we know.
    *
    * A file from a NEWER build is refused whole rather than read as far as
    * this build understands: parsed partially, its extra fields would be
    * dropped and the very next save would write them away for good. */
   char *vq    = b;
   int filever = set_file_version(b, &vq, SETTINGS_VERSION);
   if (filever < 0) {
      LOGW("settings: %s is a NEWER format than this build knows; "
           "leaving it alone",
           g_settings_path);
      return LOAD_CORRUPT;
   }
   int v[26] = {
       g_p.sound_on,      g_p.vib_on,       g_p.orient,      g_p.units,
       g_p.disc,          g_p.plot_max,     g_p.screen_on,   g_p.newdata_mode,
       g_p.ins_marker[0], g_p.ins_color[0], g_p.ins_size[0], g_p.ins_marker[1],
       g_p.ins_color[1],  g_p.ins_size[1],  g_p.statbar_val, g_p.lockscr_val,
       g_p.nudge_sound,   g_p.nudge_vib,    g_p.wunits,      g_p.shortcut[0],
       g_p.shortcut[1],   g_p.shortcut[2],  g_p.shortcut[3], g_p.shortcut[4],
       g_p.shortcut[5],   g_p.best_streak_s};
   /* VERSION 0 AND VERSION 1 SHARE THIS READER, and that is the migration:
    * v1 added the marker and changed nothing else, so a v0 file is read
    * field-for-field as it always was and is rewritten as v1 at the next
    * save. When a future version changes a FIELD, this is where the ordered
    * step for it goes -- keyed on `filever`, applied in order, with the v0
    * reader kept for the files already on phones. */
   char *q = vq;
   for (int i = 0; i < 26; i++) {
      while (*q == ' ')
         q++;
      if (*q < '0' || *q > '9')
         break;
      int x  = 0;
      int nd = 0; /* see alarm_load: cap the digits, advance outside the cap */
      while (*q >= '0' && *q <= '9') {
         if (nd < 9) {
            x = (x * 10) + (*q - '0');
            nd++;
         }
         q++;
      }
      v[i] = x;
   }
   g_p.sound_on  = v[0];
   g_p.vib_on    = v[1];
   g_p.orient    = (int)((unsigned)v[2] & 3U);
   g_p.units     = v[3] ? 1 : 0;
   g_p.disc      = (v[4] >= 0 && v[4] < 4) ? v[4] : 0;
   g_p.plot_max  = (v[5] >= 100 && v[5] <= 400) ? v[5] : PLOT_GLU_MAX;
   g_p.screen_on = v[6] ? 1 : 0;
   /* Was a 0/1 flag; CHIRP added a third value. Old files hold 0 or 1 and
    * still mean exactly what they meant, and anything else falls back to
    * silent rather than to a noise the user never chose. */
   /* THE STORED NUMBER BECOMES A MODE THROUGH ITS OWN CONVERSION.
    * A range test reads as arithmetic on a domain that has none, and it goes
    * on passing an unknown middle value the day a fourth mode is added out of
    * order. nudge_mode_of names each accepted value and answers ND_OFF for
    * everything else. */
   g_p.newdata_mode = nudge_mode_of(v[7]);
   /* Fields 9-14 are newer than some files on disk: out-of-range (or absent,
    * leaving the default) falls back to the defaults.
    *
    * THE BOUNDS COME FROM sensors.h, not from literals kept "decoupled from
    * sensors.h, crosschecked by eye". The eye is what fails: written out as
    * "9 == MARK_N, 7 colours, 4 == MARK_SIZE_MAX", one of them is wrong --
    * MARK_SIZE_MAX is 5. The size picker offers 1..5 and menu_action saves
    * whatever it is handed, so choosing the LARGEST insulin marker works,
    * persists to disk, and is then silently reset to 2 by this line on the
    * next launch. A setting that quietly forgets itself across a restart is
    * worse than one that refuses the value outright.
    *
    * The colour count still has to be a literal: UI_NCOLORS is private to
    * the renderer, so the static assert below is what keeps it honest instead.
    */
   for (int k = 0; k < 2; k++) {
      int base          = 8 + (k * 3);
      int defc          = k ? 1 : 6; /* SLOW white, FAST blue */
      g_p.ins_marker[k] = (v[base] >= 0 && v[base] < MARK_N) ? v[base] : 1;
      g_p.ins_color[k] =
          (v[base + 1] >= 0 && v[base + 1] < SET_NCOLORS) ? v[base + 1] : defc;
      g_p.ins_size[k] =
          (v[base + 2] >= 1 && v[base + 2] <= MARK_SIZE_MAX) ? v[base + 2] : 2;
   }
   g_p.statbar_val = v[14] ? 1 : 0;
   g_p.lockscr_val = v[15] ? 1 : 0;
   /* Fields 17-18, newer than files already on disk. The loop above stops at
    * the first field the file does not have, so an older file leaves these at
    * their (ON) defaults -- see the header comment on the format. */
   g_p.nudge_sound = v[16] ? 1 : 0;
   g_p.nudge_vib   = v[17] ? 1 : 0;
   g_p.wunits      = v[18] ? WT_LB : WT_KG;
   /* Fields 19-21: the main-screen shortcuts, appended after them. An older
    * file stops the loop before these and leaves the (empty) defaults, which
    * is the whole point of the positional format. Compacted so a hole in the
    * middle -- which nothing writes, but a hand-edited file could -- cannot
    * leave a live slot stranded behind an empty one. */
   {
      int n2 = 0;
      for (int i = 0; i < SC_MAX; i++) {
         /* MIGRATED ON THE WAY IN, so everything above this line only ever
          * sees the schema in settings.h. A file written by an older build
          * holds MA_* codes here. */
         int id = shortcut_migrate(v[19 + i]);
         if (id > SC_NONE)
            g_p.shortcut[n2++] = id;
      }
      while (n2 < SC_MAX)
         g_p.shortcut[n2++] = SC_NONE;
   }
   /* Field 26: the best in-range run, and the newest field in the file. An
    * older file stops the loop before it and leaves the default of 0, which
    * reads as "no record yet" -- the next streak of any length becomes the
    * best, which is the truth about what this install has seen. Bounded like
    * everything else here: a negative or absurd value is a corrupt file, not
    * a person who has been in range since the Bronze Age. */
   g_p.best_streak_s = (v[25] >= 0 && v[25] <= BEST_STREAK_MAX) ? v[25] : 0;
   return LOAD_OK;
}

/* See settings.h. The left column is what OLD files hold -- the MA_* codes as
 * they stood when the pins were first added -- and the right is the schema
 * those files are being migrated onto.
 *
 * The numbers are written out rather than referred to by name ON PURPOSE.
 * Naming them would tie this table to whatever ui.h calls those codes TODAY,
 * and the point of the migration is that ui.h's numbering is free to change.
 * A file format is a set of numbers, and these are they. */
int shortcut_migrate(int stored)
{
   static const struct {
      int legacy;
      int id;
   } was[] = {
       {21,  SC_INS_FAST}, /* MA_INS_FAST    */
       {23,  SC_INS_SLOW}, /* MA_INS_SLOW    */
       {25,  SC_INSLOG  }, /* MA_INSLOG_OPEN */
       {281, SC_WEIGHT  }, /* MA_WT_OPEN     */
       {282, SC_WTLOG   }, /* MA_WTLOG_OPEN  */
   };

   if (stored <= SC_NONE)
      return SC_NONE;
   if (stored <= SC_ID_LAST)
      return stored; /* already the new schema */
   for (unsigned i = 0; i < sizeof was / sizeof was[0]; i++)
      if (was[i].legacy == stored)
         return was[i].id;
   /* A pin this build no longer offers. Dropped rather than kept as a number
    * nothing can render -- a button with no label is worse than no button. */
   return SC_NONE;
}
