// SPDX-License-Identifier: GPL-3.0
// setfile.c --- the versioned, generation-ordered file write
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN. Five unrelated files -- the device's model and
 * firmware, the alarm thresholds, the display preferences, the pairing code
 * and the remote credentials -- behind one save engine is 1541 lines of
 * module with no subject. They share the engine
 * (app/setpriv.h) and the preferences aggregate; they share nothing else, and
 * a reader after one of them had to read past the other four.
 */
#include "plot.h" /* PLOT_GLU_MAX: the plot ceiling's default */
#include "setpriv.h"
#include "settings.h" /* struct prefs: the aggregate the engine holds */
#include "thread.h"   /* the lock this module's state sits behind */
#include "util.h"
#include "weight.h" /* WT_LB: the weight unit's default */

struct prefs g_p = {
    .alarm_low   = 70,
    .alarm_high  = 300,
    .nudge_low   = 85,
    .nudge_high  = 250,
    .sound_on    = 1,
    .vib_on      = 1,
    .nudge_sound = 1,
    .nudge_vib   = 1,
    .screen_on   = 1, /* hold the screen on, as the app always has */
    .wunits      = WT_LB,
    .plot_max    = PLOT_GLU_MAX,
    .ins_marker  = {1, 1},
    .ins_color   = {6, 1},
    .ins_size    = {2, 2},
    .statbar_val = 1, /* the VALUE, not the app icon */
    .lockscr_val = 1, /* visible on the lock screen */
    .shortcut    = {SC_INS_SLOW, SC_WEIGHT, SC_NONE, SC_NONE, SC_NONE, SC_NONE},
    .code_str    = "9973", /* Stelo applicator default */
    .remote_server = "pancra.org",
    .remote_port   = 443,
};

/* ---- THE FILE WRITE HAPPENS OUTSIDE set_lk --------------------------
 *
 * set_lk is a yield-spinning mutex and it is what every reader of the
 * preferences takes -- the frame builder among them, once per frame. Holding
 * it across atomic_replace held it across a write, an fsync, a rename and a
 * directory fsync: milliseconds of flash latency that every unrelated reader
 * spins through. The calibration saves were moved out of this same shape.
 *
 * A save is TWO steps. Under the lock, a render_fn turns the current state
 * into a struct save_job -- bytes, path, and the generation it was rendered
 * at. The lock is then released BY THE FUNCTION THAT TOOK IT, and
 * write_job does the I/O with nothing held but set_file_lk.
 *
 * That split is deliberate: an earlier attempt had the saver itself release
 * and re-take set_lk, which works only while every caller already holds it --
 * and one did not, so the lock was left held and the next acquirer spun for
 * the life of the process. A function that releases a lock it did not take
 * cannot be made safe by inspection. Here, each function locks and unlocks
 * exactly once, and write_job takes nothing it does not release.
 *
 * TWO HAZARDS THE WINDOW OPENS, and what answers each:
 *
 *   ORDERING. Two setters can each render and then race to the file, so an
 *   older state could land after a newer one and be what the next launch
 *   reads. Each job carries its generation, and a write that is not newer
 *   than what is already on disk is SKIPPED.
 *
 *   ROLLBACK. A failed save undoes its own edit, and another edit may have
 *   landed while the file was being written. Undoing then would revert that
 *   one -- which succeeded -- on behalf of this one, which did not. So a
 *   rollback applies only when the generation shows nothing newer happened.
 *
 * A reader during the window sees the new value even if the save later fails
 * and reverts it: a preference that flickers for one flash write, against
 * every frame spinning for it. */
/* (struct save_job, render_fn and the five renderers are declared in
 * app/setpriv.h now: they are the contract between this engine and the five
 * domain modules that use it.) */

/* THE LOCK OVER EVERYTHING THIS FILE OWNS.
 *
 * Preferences read as a main-thread affair and are not one. info_set
 * is called from the SENSOR's device-information reply, on a BINDER thread,
 * while the renderer is reading the same three strings for the device screen
 * -- a 24-byte copy against a read, which really can be seen half-written.
 * The paired identity has the same shape from the sync worker.
 *
 * A LEAF. It is taken innermost and never held across another module's call:
 * alarm.c changes a threshold with the alarm lock held, so alarm_lk -> set_lk
 * exists, and nothing here may ever take a lock in the other direction. The
 * file writes below happen under it deliberately -- a save and the state it
 * writes are one transaction. */
struct mutex set_lk = MUTEX_INIT;

/* Serialises the WRITERS against each other. Taken with set_lk released and
 * held across the fsyncs and the rename; no reader ever takes it. */
/* ---- THE FILE FORMATS CARRY THEIR VERSION -------------------------
 *
 * The settings line is 22 positional integers and the remote line is six
 * fields, and neither says which schema it is by HOW MANY of them are
 * present -- a loader stopping at the first field the file does not have and
 * leaving the rest at their defaults. That is a heuristic, not a format, and it
 * has three failure modes it cannot tell apart -- a file from an older build,
 * a file truncated by a power loss, and a file from a NEWER build whose extra
 * fields this one will silently discard and then overwrite on the next save.
 *
 * A marker at the head fixes all three. `v<N> ` before the fields; a file
 * with no marker is version 0, which is every file already deployed, and the
 * version-0 reader below is exactly the parser those files were written by.
 *
 * A version this build does not know is REFUSED WHOLE. Not partly applied,
 * not defaulted field by field -- refused, with the state left untouched, so
 * a downgrade cannot quietly discard settings the newer build stored. */
#define SETTINGS_VERSION 1
#define REMOTE_VERSION   1

/* The version at the head of `b`, and `*rest` advanced past it. 0 when there
 * is no marker, which is what a deployed file looks like. -1 when there is a
 * marker this build cannot use. */
int set_file_version(char *b, char **rest, int newest)
{
   *rest = b;
   if (b[0] != 'v' || b[1] < '0' || b[1] > '9')
      return 0; /* no marker: version 0, the deployed format */
   int ver = 0;
   char *q = b + 1;
   int nd  = 0;
   while (*q >= '0' && *q <= '9' && nd < 4) {
      ver = (ver * 10) + (*q - '0');
      q++;
      nd++;
   }
   if (*q != ' ')
      return -1; /* "v" followed by something that is not a version */
   while (*q == ' ')
      q++;
   *rest = q;
   return ver > newest ? -1 : ver;
}

static struct mutex set_file_lk = MUTEX_INIT;
static unsigned g_set_gen; /* bumped by each render; guarded by set_lk */

/* THE NEWEST GENERATION ANY RENDER HAS TAKEN. A domain module that performs
 * its own transaction -- render, write, and put the value back if the write
 * failed -- compares its job's generation against this to decide whether a
 * rollback would revert somebody ELSE's successful edit. Caller holds set_lk;
 * the counter is not exported, because reading it is the only thing anybody
 * outside this file may do with it. */
unsigned set_gen_now(void)
{
   return g_set_gen;
}

/* CALLER HOLDS set_lk. Stamps the job with this edit's generation. */
void set_job_stamp(struct save_job *j, const char *path, unsigned *written,
                   int len, int ok)
{
   j->path    = path;
   j->written = written;
   j->len     = len;
   j->ok      = ok;
   j->gen     = ++g_set_gen;
}

#ifdef APP_FAULTS
/* HELD OPEN ON DEMAND, in the fault build only: the gap between a render and
 * its write. See write_job, and settings.h for what a test does with it. */
void (*settings_fault_gap_here)(void);

/* See render_settings: the settings file's own on-disk generation. */
static unsigned *g_settings_written;

/* THE PREFERENCES FILE NAMES ITS OWN COUNTER. The renderer lives
 * in settings.c now and the counter is a function-static there -- one per
 * file, which is the point -- so it registers the pointer here rather than
 * this file reaching into it. */
void set_fault_note_written(unsigned *written)
{
   g_settings_written = written;
}

unsigned settings_fault_written_gen(void)
{
   return g_settings_written ? *g_settings_written : 0;
}
#endif

/* CALLER HOLDS NOTHING. 0 on success. */
int set_write_job(const struct save_job *j)
{
   if (!j->ok)
      return -1;
#ifdef APP_FAULTS
   /* THE WINDOW, HELD OPEN ON DEMAND.
    *
    * Between a render and its write, another thread may render and write a
    * NEWER state; the generation check below is what stops this one landing
    * on top of it. On real hardware that window is the length of a function
    * call.
    *
    * A YIELD IS NOT ENOUGH. A test that relies on one produces about two
    * out-of-order writes in four hundred, and only the LAST write being a
    * stale one is visible in the file -- so a build with the guard deleted
    * passes nearly every time. A property that a mutant survives is not
    * covered.
    *
    * So the test installs a hook here and BLOCKS one thread inside the
    * window while another completes a newer save, which makes the ordering
    * exact rather than lucky. With no hook installed this is a plain yield,
    * which the two-thread hammer case wants. Nothing that ships defines
    * APP_FAULTS; app/meterstore.c carries the identical device. */
   if (settings_fault_gap_here)
      settings_fault_gap_here();
   else
      sched_yield();
#endif
   mutex_lock(&set_file_lk);
   int bad = 0;
   /* Signed difference, so the comparison survives a wrap. */
   if ((int)(j->gen - *j->written) > 0) {
      enum replace_result rr = atomic_replace(j->path, j->buf, j->len);
      /* REPLACE_UNSYNCED IS NOT A FAILURE HERE. The rename happened, so the
       * file already holds these bytes; only the directory entry's
       * durability is in doubt. Reported as failure, the caller would put
       * the previous value back in memory while the disk held the new one --
       * and the next launch would read the file and "un-revert" it. Recorded
       * as written, because it is. */
      bad = (rr == REPLACE_FAILED);
      if (!bad)
         *j->written = j->gen;
   }
   mutex_unlock(&set_file_lk);
   return bad ? -1 : 0;
}

/* ONE COHERENT COPY -- the only way anything outside this file reads these.
 *
 * A pointer into the live aggregate can answer differently on two reads, and
 * a `const char *` taken from it can be rewritten under the borrower: that is
 * what the device screen did with model and firmware. */
void settings_get(struct prefs *out)
{
   if (!out)
      return;
   mutex_lock(&set_lk);
   *out = g_p;
   mutex_unlock(&set_lk);
}

/* THE CREDENTIAL, private to this file.
 *
 * The uid and key are what authenticate this phone TO the server: anything
 * that can write them can silently repoint the account, and anything that can
 * write half of them (a uid without its key, or the reverse) leaves an
 * identity that fails every request with no way to tell why. They are
 * exported as a READ-ONLY view and changed only through the two operations
 * that also persist them. */
/* ---- setters: change a setting AND persist it, in one call ------------
 *
 * The module's interface is a set of mutable globals plus a rule -- "write
 * the global, then call the matching *_save()" -- that nothing enforces. Every
 * caller in the tree honours it today; the risk is the next one, because a
 * forgotten save is invisible until the app restarts and silently reverts a
 * choice the user made.
 *
 * These are not a full encapsulation: reads stay direct, because the renderer
 * touches these values on every frame and routing that through accessors
 * would buy nothing. They cover the WRITES, which are few and are where the
 * mistake lives. */
/* ONE INTEGER PREFERENCE, stored and persisted as ONE transaction.
 *
 * Thirteen setters were thirteen copies of read-old / write-new / save /
 * put-it-back-if-the-save-failed, and every copy returned void -- so the
 * rollback was invisible to the caller and the screen went on showing the
 * value it had just failed to store. The recipe lives once; each setter names
 * its field and its file. */
int set_int_field(int *field, int val, render_fn render)
{
   struct save_job j;
   mutex_lock(&set_lk);
   int old = *field;
   *field  = val;
   render(&j);
   mutex_unlock(&set_lk); /* released by the function that took it */
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      /* ONLY IF NOTHING NEWER LANDED. Another setter that ran while this
       * one was writing bumped the generation; undoing then would revert
       * THAT edit, which succeeded, on behalf of this one, which did not. */
      if (g_set_gen == j.gen)
         *field = old;
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* The same for a string field. `cap` is the destination's size; the longest
 * of them is the 64-byte server name, which is what bounds the undo copy. */
int set_str_field(char *field, int cap, const char *val, render_fn render)
{
   char old[64];
   if (cap <= 0 || cap > (int)sizeof old)
      return SETTINGS_UNSAVED;
   struct save_job j;
   mutex_lock(&set_lk);
   str_snapshot(old, (int)sizeof old, field);
   str_snapshot(field, cap, val ? val : "");
   render(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) /* see set_int */
         str_snapshot(field, cap, old);
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

char g_info_path[256];
char g_alarm_path[256];
char g_settings_path[256];
char g_code_path[256];
char g_remote_path[256];

/* The two that persist a whole family, declared here because the setters
 * above them name which file their transaction writes. */

/* THE SAVERS assume set_lk is HELD: they are the second half of every
 * transaction below, and a save that took the lock itself would either
 * deadlock the setter or write a line composed from two different states.
 * */
int settings_paths(const char *dir)
{
   int ok = 1;
   if (!(data_path(g_info_path, sizeof g_info_path, dir, "/stelo.info")))
      ok = 0;
   if (!(data_path(g_alarm_path, sizeof g_alarm_path, dir, "/alarm.cfg")))
      ok = 0;
   if (!(data_path(g_settings_path, sizeof g_settings_path, dir,
                   "/settings.cfg")))
      ok = 0;
   if (!(data_path(g_code_path, sizeof g_code_path, dir, "/paircode.txt")))
      ok = 0;
   if (!(data_path(g_remote_path, sizeof g_remote_path, dir, "/remote.cfg")))
      ok = 0;
   return ok;
}

/* THE TWO CREDENTIAL PATHS, for the sync client that must know which files
 * NOT to upload: they hold the pairing code and the derived key, and sending
 * them would put the secret that authenticates us TO the server inside the
 * server's own database. The other three are nobody else's business. */

/* The alarm and settings files by name, for the tests that simulate a
 * CORRUPTED one -- the loaders' behaviour on garbage is half of what settings
 * persistence has to get right, and it cannot be checked without writing the
 * bytes. Read-only, like every path here: they are set once, by
 * settings_paths. */
