// SPDX-License-Identifier: GPL-3.0
// settings.c --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

/* Small config files, one concern each: device-info strings, alarm thresholds,
 * display/settings-menu prefs, and the pairing code. The UI (main.c) owns when
 * to save/load; this module owns the state and the on-disk format. */
#include "settings.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: alarm_load's bound = the keypad's */
#include "dexlibc.h"
#include "plot.h"
#include "style.h"
#include "thread.h" /* set_lk: the DIS strings arrive on a binder thread */
#include "util.h"
#include "weight.h" /* WT_KG / WT_LB: the weight display unit */
#include <stdio.h>  /* snprintf */

/* A config save that fails is a setting that silently reverts on the next
 * launch -- and one of the five below is the ALARM THRESHOLDS. Each used to
 * carry an empty `if (write(...) != n) { }`, which is the shape of a check
 * that was written and then never finished. Say it. */
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

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
static struct prefs g_p = {
    .alarm_low     = 70,
    .alarm_high    = 300,
    .nudge_low     = 85,
    .nudge_high    = 250,
    .sound_on      = 1,
    .vib_on        = 1,
    .nudge_sound   = 1,
    .nudge_vib     = 1,
    .screen_on     = 1, /* hold the screen on, as the app always has */
    .wunits        = WT_LB,
    .plot_max      = PLOT_GLU_MAX,
    .ins_marker    = {1, 1},
    .ins_color     = {6, 1},
    .ins_size      = {2, 2},
    .statbar_val   = 1, /* the VALUE, not the app icon */
    .lockscr_val   = 1, /* visible on the lock screen */
    .shortcut      = {SC_INS_SLOW, SC_WEIGHT, SC_NONE, SC_NONE,
                      SC_NONE, SC_NONE},
    .code_str      = "9973", /* Stelo applicator default */
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
 * A save is now TWO steps. Under the lock, a render_fn turns the current
 * state into a struct save_job -- bytes, path, and the generation it was
 * rendered at. The lock is then released BY THE FUNCTION THAT TOOK IT, and
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
struct save_job {
   const char *path;
   char buf[264]; /* the remote line is the longest at 256 */
   int len;
   unsigned gen;
   int ok;            /* the render itself succeeded */
   unsigned *written; /* this file's newest generation already on disk */
};

typedef void (*render_fn)(struct save_job *);

static void render_settings(struct save_job *j);
static void render_alarm(struct save_job *j);
static void render_info(struct save_job *j);
static void render_code(struct save_job *j);
static void render_remote(struct save_job *j);

/* THE LOCK OVER EVERYTHING THIS FILE OWNS.
 *
 * Preferences read as a main-thread affair and are not one. settings_set_dis
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
static struct mutex set_lk = MUTEX_INIT;

/* Serialises the WRITERS against each other. Taken with set_lk released and
 * held across the fsyncs and the rename; no reader ever takes it. */
/* ---- THE FILE FORMATS CARRY THEIR VERSION -------------------------
 *
 * The settings line is 22 positional integers and the remote line is six
 * fields, and both used to say which schema they were by HOW MANY of them
 * were present: a loader stopped at the first field the file did not have and
 * left the rest at their defaults. That is a heuristic, not a format, and it
 * has three failure modes it cannot tell apart -- a file from an older build,
 * a file truncated by a power loss, and a file from a NEWER build whose extra
 * fields this one will silently discard and then overwrite on the next save.
 *
 * A marker at the head fixes all three. `v<N> ` before the fields; a file
 * with no marker is version 0, which is every file already deployed, and the
 * version-0 reader below is exactly the parser those files were written by.
 *
 * A version this build does not know is REFUSED WHOLE. Not partly applied,
 * not defaulted field by field -- refused, with the state left as it was, so
 * a downgrade cannot quietly discard settings the newer build stored. */
#define SETTINGS_VERSION 1
#define REMOTE_VERSION   1

/* The version at the head of `b`, and `*rest` advanced past it. 0 when there
 * is no marker, which is what a deployed file looks like. -1 when there is a
 * marker this build cannot use. */
static int file_version(char *b, char **rest, int newest)
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

/* CALLER HOLDS set_lk. Stamps the job with this edit's generation. */
static void job_stamp(struct save_job *j, const char *path, unsigned *written,
                      int len, int ok)
{
   j->path    = path;
   j->written = written;
   j->len     = len;
   j->ok      = ok;
   j->gen     = ++g_set_gen;
}

/* CALLER HOLDS NOTHING. 0 on success. */
static int write_job(const struct save_job *j)
{
   if (!j->ok)
      return -1;
#ifdef APP_FAULTS
   /* WIDEN THE WINDOW, in the fault-injection build only.
    *
    * Between a render and its write, another thread may render and write a
    * NEWER state; the generation check below is what stops this one landing
    * on top of it. On real hardware that window is the length of a function
    * call and a test almost never lands inside it -- so with no help, a build
    * with the check deleted passed the concurrent case every time. A yield
    * here makes the interleaving ordinary rather than rare, which is the only
    * way the guard can be shown to do anything. */
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
       * the old value back in memory while the disk held the new one -- and
       * the next launch would read the file and "un-revert" it. Recorded as
       * written, because it was. */
      bad = (rr == REPLACE_FAILED);
      if (!bad)
         *j->written = j->gen;
   }
   mutex_unlock(&set_file_lk);
   return bad ? -1 : 0;
}

/* THE WHOLE TRANSACTION for a save with no field to roll back. */
static int save_now(render_fn render)
{
   struct save_job j;
   mutex_lock(&set_lk);
   render(&j);
   mutex_unlock(&set_lk);
   return write_job(&j);
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
static long g_sync_uid; /* 0 until an app is paired */
static unsigned char g_sync_key[16];
static char g_sync_email[64];
static struct sync_creds g_creds;

/* Refresh the view. Called after every write to the three above -- they are
 * the storage, this is what everyone else sees. */
static void creds_publish(void)
{
   g_creds.uid = g_sync_uid;
   for (int i = 0; i < (int)sizeof g_creds.key; i++)
      g_creds.key[i] = g_sync_key[i];
   str_snapshot(g_creds.email, sizeof g_creds.email, g_sync_email);
}

/* The identity as a COPY, for the same reason as settings_get: the sync
 * worker rewrites it, and uid-without-key is the state that fails every
 * request with nothing on screen to say why. */
void sync_creds_get(struct sync_creds *out)
{
   if (!out)
      return;
   mutex_lock(&set_lk);
   *out = g_creds;
   mutex_unlock(&set_lk);
}

/* THE ENDPOINT AND THE IDENTITY TOGETHER, under ONE acquisition. See the
 * header: taken as two, a request can be aimed at the old server and signed
 * as the new account. */
void remote_config_get(struct remote_config *out)
{
   if (!out)
      return;
   mutex_lock(&set_lk);
   out->on   = g_p.remote_on;
   out->port = g_p.remote_port;
   str_snapshot(out->server, (int)sizeof out->server, g_p.remote_server);
   out->uid = g_creds.uid;
   for (int i = 0; i < (int)sizeof out->key; i++)
      out->key[i] = g_creds.key[i];
   str_snapshot(out->email, (int)sizeof out->email, g_creds.email);
   mutex_unlock(&set_lk);
}

/* 443, because the client always speaks https now: the platform's TLS is
 * free and the session cookie the server sets is Secure. The old default of
 * 80 belonged to a plain-HTTP intake API that no longer exists. */

static char g_info_path[256];
static char g_alarm_path[256];
static char g_settings_path[256];
static char g_code_path[256];
static char g_remote_path[256];

/* The two that persist a whole family, declared here because the setters
 * above them name which file their transaction writes. */

/* THE SAVERS assume set_lk is HELD: they are the second half of every
 * transaction below, and a save that took the lock itself would either
 * deadlock the setter or write a line composed from two different states.
 * */
static void render_info(struct save_job *j)
{
   static unsigned written;
   int n = snprintf(j->buf, sizeof j->buf, "%s\n%s\n%s\n", g_p.model, g_p.fw,
                    g_p.mfr);
   job_stamp(j, g_info_path, &written, n, n > 0 && n < 96);
}

int info_save(void)
{
   int rc = save_now(render_info);
   if (rc)
      LOGW("settings: sensor info not saved");
   return rc;
}

enum load_result info_load(void)
{
   int fd = open(g_info_path, O_RDONLY, 0);
   if (fd < 0) {
      /* ABSENT is a first run and is not a failure; anything else means the
       * file is there and could not be opened, which is. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      LOGW("settings: cannot read %s (%d)", g_info_path, errno);
      return LOAD_ERROR;
   }
   char b[96];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* THE FILE EXISTS AND HAS NOTHING USABLE IN IT. A zero-length file is what
    * a power loss between create and write leaves behind; a negative read is
    * an I/O error. Neither is a first run. */
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT;
   b[n]         = 0;
   char *p      = b;
   char *dst[3] = {g_p.model, g_p.fw, g_p.mfr};
   for (int i = 0; i < 3 && p; i++) {
      char *nl = p;
      while (*nl && *nl != '\n')
         nl++;
      int len = (int)(nl - p);
      if (len > 22)
         len = 22;
      for (int j = 0; j < len; j++)
         dst[i][j] = p[j];
      dst[i][len] = 0;
      p           = *nl ? nl + 1 : 0;
   }
   return LOAD_OK;
}

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
static int set_int(int *field, int val, render_fn render)
{
   struct save_job j;
   mutex_lock(&set_lk);
   int old = *field;
   *field  = val;
   render(&j);
   mutex_unlock(&set_lk); /* released by the function that took it */
   int bad = write_job(&j) != 0;
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
static int set_str(char *field, int cap, const char *val, render_fn render)
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
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) /* see set_int */
         str_snapshot(field, cap, old);
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

int settings_set_units(int mmol)
{
   return set_int(&g_p.units, mmol ? 1 : 0, render_settings);
}

int settings_set_wunits(int wu)
{
   return set_int(&g_p.wunits, wu, render_settings);
}

int settings_set_sound(int on)
{
   return set_int(&g_p.sound_on, on ? 1 : 0, render_settings);
}

int settings_set_remote_on(int on)
{
   return set_int(&g_p.remote_on, on ? 1 : 0, render_remote);
}

/* THE CYCLES. How many states each of these has is a fact about the SETTING,
 * not about the row that taps it; it used to be spelled out as wrap arithmetic
 * in menu_action, three different ways. */
int settings_cycle_orient(void)
{
   return set_int(&g_p.orient, (g_p.orient + 1) % 4, render_settings);
}

int settings_cycle_disc(void)
{
   return set_int(&g_p.disc, (int)(((unsigned)g_p.disc + 1U) & 3U),
                  render_settings);
}

int settings_cycle_newdata(void)
{
   /* OFF -> BEEP -> CHIRP */
   return set_int(&g_p.newdata_mode, (g_p.newdata_mode + 1) % 3,
                  render_settings);
}

int settings_set_vib(int on)
{
   return set_int(&g_p.vib_on, on ? 1 : 0, render_settings);
}

int settings_set_screen_on(int on)
{
   return set_int(&g_p.screen_on, on ? 1 : 0, render_settings);
}

int settings_set_statbar(int on)
{
   return set_int(&g_p.statbar_val, on ? 1 : 0, render_settings);
}

int settings_set_lockscr(int on)
{
   return set_int(&g_p.lockscr_val, on ? 1 : 0, render_settings);
}

int settings_set_nudge_sound(int on)
{
   return set_int(&g_p.nudge_sound, on ? 1 : 0, render_settings);
}

int settings_set_nudge_vib(int on)
{
   return set_int(&g_p.nudge_vib, on ? 1 : 0, render_settings);
}

int settings_set_plot_max(int mgdl)
{
   /* The RANGE lives here, with settings_load's own clamp, rather than at the
    * keypad that happens to type it. */
   if (mgdl < 100)
      mgdl = 100;
   if (mgdl > 400)
      mgdl = 400;
   /* NOTHING DERIVED TO APPLY. The renderer used to keep the scale in a
    * process global that this had to push into (plot_set_max), which is why
    * two plots could not have different scales and why the touch path
    * answered against whichever was drawn last. The scale is passed to each
    * render and each hit test now, read from this setting at the call. */
   return set_int(&g_p.plot_max, mgdl, render_settings);
}

int settings_set_remote_port(int port)
{
   if (port < 1 || port > 65535)
      return SETTINGS_UNSAVED;
   return set_int(&g_p.remote_port, port, render_remote);
}

/* THE FOUR THRESHOLDS, one at a time. The CALLER decides whether the pair is
 * still ordered -- that check has to share a critical section with the read of
 * the partner value, and the alarm lock that provides it lives in alarm.c --
 * and the caller also calls alarm_save() once the pair is settled, which is
 * why these four store without persisting. */
int settings_store_thresholds(int alarm_low, int alarm_high, int nudge_low,
                              int nudge_high)
{
   /* ALL FOUR AT ONCE, AND PERSISTED HERE. The four one-field setters this
    * replaces were public, stored without saving, and left the ordering
    * check and the alarm_save() to whoever called them -- three separate
    * obligations, in a header, on the pair of numbers that decides whether a
    * hypo alarm can fire. A caller that met two of the three left the phone
    * with a live threshold that the next launch would not have.
    *
    * The ordering is still the ALARM's to decide (it needs its own lock to
    * read the partner and choose atomically); what is no longer possible is
    * storing one half, or storing both and forgetting the file. */
   struct save_job j;
   mutex_lock(&set_lk);
   int old_al = g_p.alarm_low, old_ah = g_p.alarm_high;
   int old_nl = g_p.nudge_low, old_nh = g_p.nudge_high;
   g_p.alarm_low  = alarm_low;
   g_p.alarm_high = alarm_high;
   g_p.nudge_low  = nudge_low;
   g_p.nudge_high = nudge_high;
   render_alarm(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) { /* see set_int */
         g_p.alarm_low  = old_al;
         g_p.alarm_high = old_ah;
         g_p.nudge_low  = old_nl;
         g_p.nudge_high = old_nh;
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* One insulin type's plot styling: up to THREE fields, so all three come back
 * if the file cannot be replaced. Two of them restored and one left is a
 * marker the user did not choose, in a colour they did not choose either. */
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
   render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) { /* see set_int */
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
   render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) /* see set_int */
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
   render_settings(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) /* see set_int */
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
/* THE DEVICE-INFORMATION STRINGS the sensor reports (model, firmware,
 * manufacturer). Not a preference the user sets -- learned from the device --
 * but persisted here with them, and written only through this call so the
 * save cannot be forgotten. `which` is one of SET_DIS_*.
 *
 * The copy is bounded and stops at the first control character: these arrive
 * as raw GATT bytes and a stray one would corrupt the single-line file the
 * loader parses positionally. Written on a BINDER thread, which is why the
 * whole family is under set_lk -- the device screen reads these three as a
 * set. */
int settings_set_dis(int which, const char *val)
{
   char *dst = 0;
   if (which == SET_DIS_MODEL)
      dst = g_p.model;
   else if (which == SET_DIS_FW)
      dst = g_p.fw;
   else if (which == SET_DIS_MFR)
      dst = g_p.mfr;
   if (!dst || !val)
      return SETTINGS_UNSAVED;
   char clean[24];
   int i = 0;
   for (; val[i] && i < 23 && val[i] >= 0x20; i++)
      clean[i] = val[i];
   clean[i] = 0;
   return set_str(dst, 24, clean, render_info);
}

int settings_set_code(const char *digits)
{
   return set_str(g_p.code_str, (int)sizeof g_p.code_str, digits, render_code);
}

int settings_set_server(const char *host)
{
   return set_str(g_p.remote_server, (int)sizeof g_p.remote_server, host,
                  render_remote);
}

/* The account address lives in the CREDENTIAL, not in prefs, so it needs its
 * own transaction: the published copy has to be refreshed on the way in AND
 * on the way back out. */
int settings_set_email(const char *addr)
{
   mutex_lock(&set_lk);
   char old[sizeof g_sync_email];
   str_snapshot(old, (int)sizeof old, g_sync_email);
   str_snapshot(g_sync_email, sizeof g_sync_email, addr ? addr : "");
   creds_publish();
   struct save_job j;
   render_remote(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) { /* see set_int */
         str_snapshot(g_sync_email, sizeof g_sync_email, old);
         creds_publish();
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* FORGET THE PAIRED IDENTITY, keeping the server: re-pairing is the normal
 * way to move this phone to a different account, and it needs a fresh code
 * from the server anyway. The key is zeroed here so no caller has to remember
 * that half a forgotten identity is worse than none -- and if the file cannot
 * be replaced the identity comes BACK WHOLE, because an app that believes it
 * is unpaired while the file still says otherwise re-pairs into a second
 * account. */
int settings_forget_identity(void)
{
   mutex_lock(&set_lk);
   long old_uid = g_sync_uid;
   unsigned char old_key[16];
   for (int i = 0; i < (int)sizeof old_key; i++)
      old_key[i] = g_sync_key[i];
   g_sync_uid = 0;
   for (int i = 0; i < (int)sizeof g_sync_key; i++)
      g_sync_key[i] = 0;
   creds_publish();
   struct save_job j;
   render_remote(&j);
   mutex_unlock(&set_lk);
   int bad = write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (g_set_gen == j.gen) { /* see set_int */
         g_sync_uid = old_uid;
         for (int i = 0; i < (int)sizeof g_sync_key; i++)
            g_sync_key[i] = old_key[i];
         creds_publish();
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* THE TWO PUBLIC SAVES. Everything above persists what it changes; these are
 * for the one caller that changes a PAIR under its own lock (alarm.c) and for
 * the load path. They take set_lk; the _locked forms above assume it. */
int settings_save(void)
{
   int rc = save_now(render_settings);
   if (rc)
      LOGW("settings: settings not saved");
   return rc;
}

int alarm_save(void)
{
   int rc = save_now(render_alarm);
   if (rc)
      LOGW("settings: ALARM THRESHOLDS not saved");
   return rc;
}

static void render_alarm(struct save_job *j)
{
   static unsigned written;
   int n = snprintf(j->buf, sizeof j->buf, "%d %d %d %d\n", g_p.alarm_low,
                    g_p.alarm_high, g_p.nudge_low, g_p.nudge_high);
   job_stamp(j, g_alarm_path, &written, n, n > 0 && n < 48);
}

/* One unsigned decimal field at *q, skipping leading spaces and advancing past
 * the digits. Returns 1 iff at least one digit was consumed.
 *
 * DIGIT-CAPPED. Unbounded accumulation is undefined behaviour, and it happens
 * during parsing -- before any range check can reject anything. A wrapped
 * value can land back inside a plausible range and silently install thresholds
 * the user never chose, on the numbers that decide whether a hypo alarm can
 * fire at all. store.c, stats.c and sensors.c all received this hardening.
 *
 * The advance is OUTSIDE the cap, deliberately: putting it inside is what
 * turned the same fix in sensors.c into an infinite loop.
 *
 * Extracted when the nudge pair was appended: four hand-inlined copies of this
 * loop is four places for the cap to be dropped from. */
static int parse_field(char **q, int *out)
{
   char *p = *q;
   while (*p == ' ')
      p++;
   int v  = 0;
   int nd = 0;
   while (*p >= '0' && *p <= '9') {
      if (nd < 9) {
         v = (v * 10) + (*p - '0');
         nd++;
      }
      p++;
   }
   *q   = p;
   *out = v;
   return nd > 0;
}

enum load_result alarm_load(void)
{
   int fd = open(g_alarm_path, O_RDONLY, 0);
   if (fd < 0) {
      /* ABSENT is a first run and is not a failure; anything else means the
       * file is there and could not be opened, which is. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      LOGW("settings: cannot read %s (%d)", g_alarm_path, errno);
      return LOAD_ERROR;
   }
   /* 256, matching what remote_save writes. It was 48, sized for
    * "1 1.2.3.4 8080" -- and the line has since grown a host NAME, a user id,
    * a 32-character key and an email address. The read simply truncated, so
    * the key came back malformed and the account came back empty, and the app
    * looked like it had forgotten a pairing it had actually stored. Both ends
    * of this file are now the same size for that reason. */
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* THE FILE EXISTS AND HAS NOTHING USABLE IN IT. A zero-length file is what
    * a power loss between create and write leaves behind; a negative read is
    * an I/O error. Neither is a first run. */
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT;
   b[n]       = 0;
   int lo     = 0;
   int hi     = 0;
   int nlo    = 0;
   int nhi    = 0;
   char *q    = b;
   int got_lo = parse_field(&q, &lo);
   int got_hi = parse_field(&q, &hi);
   /* Fields 3 and 4 are NEWER than files already on disk: an alarm file
    * written before the nudge existed has two fields, and must keep loading
    * its alarm pair rather than being rejected wholesale. Absent => the nudge
    * keeps its OFF defaults, which is the safe direction (no sound the user
    * did not ask for). */
   int got_nlo = parse_field(&q, &nlo);
   int got_nhi = parse_field(&q, &nhi);
   /* Range-check, do not merely test for non-zero. A corrupt or hand-edited
    * file with lo=99999 silently DISABLES the low alarm (nothing is ever below
    * it) and lo>hi leaves both alarms permanently latched -- the two ways this
    * file can fail dangerously. Bounds match the keypad's own limits, so a
    * value that could not be typed cannot be loaded either: both thresholds
    * 0..AL_ENTRY_MAX (each end is that alarm's deliberate OFF switch -- see
    * alarmlogic.h). */
   /* got_lo/got_hi: with 0 now LEGAL, a file that parses to no digits at all
    * must be rejected explicitly -- otherwise any garbage reads as the valid
    * pair 0/0 and silently installs both alarms OFF, thresholds the user
    * never chose. */
   /* lo <= hi, not lo < hi: a threshold entry refuses a crossing, but the
    * old steppers could set the two EQUAL, and equal pairs exist in saved
    * files. Rejecting one silently reverted the user's thresholds to the
    * compiled defaults on the next launch -- values they never chose. The
    * predicate must accept everything the writer can emit. */
   if (got_lo && got_hi && lo <= AL_ENTRY_MAX && hi <= AL_ENTRY_MAX &&
       lo <= hi) {
      g_p.alarm_low  = lo;
      g_p.alarm_high = hi;
   }
   /* The nudge pair is committed SEPARATELY and by the same rules, never
    * cross-checked against the alarm pair. A nudge inside the alarm band is
    * pointless but harmless (the alarm suppresses it), while refusing to load
    * it would silently revert a threshold the user chose -- and this file's
    * whole reason for existing is that a threshold the user believes is armed
    * must actually be armed. */
   if (got_nlo && got_nhi && nlo <= AL_ENTRY_MAX && nhi <= AL_ENTRY_MAX &&
       nlo <= nhi) {
      g_p.nudge_low  = nlo;
      g_p.nudge_high = nhi;
   }
   return LOAD_OK;
}

static void render_settings(struct save_job *j)
{
   static unsigned written;
   /* 192, MATCHING settings_load's READER -- the two numbers move together or
    * not at all. At 96 they disagreed the moment the field count grew, and a
    * truncated line does not fail loudly: clampn writes the prefix, the loader
    * parses what it finds and stops, and the tail fields silently revert to
    * their defaults on every launch.
    *
    * It was 128, which held 23 fields comfortably. Six pins instead of three
    * makes 26, and the parser accepts up to 9 digits per field, so the
    * headroom that made 128 obviously safe is no longer obvious. Raised on
    * both sides at once rather than measured against today's typical values,
    * because the failure this guards is silent and only appears on the launch
    * AFTER the one that wrote the long line. */
   int n = snprintf(
       j->buf, sizeof j->buf,
       "v%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
       "%d %d %d %d\n",
       SETTINGS_VERSION, g_p.sound_on, g_p.vib_on, g_p.orient, g_p.units,
       g_p.disc, g_p.plot_max, g_p.screen_on, g_p.newdata_mode,
       g_p.ins_marker[0], g_p.ins_color[0], g_p.ins_size[0], g_p.ins_marker[1],
       g_p.ins_color[1], g_p.ins_size[1], g_p.statbar_val, g_p.lockscr_val,
       g_p.nudge_sound, g_p.nudge_vib, g_p.wunits, g_p.shortcut[0],
       g_p.shortcut[1], g_p.shortcut[2], g_p.shortcut[3], g_p.shortcut[4],
       g_p.shortcut[5]);
   job_stamp(j, g_settings_path, &written, n, n > 0 && n < 192);
}

enum load_result settings_load(void)
{
   int fd = open(g_settings_path, O_RDONLY, 0);
   if (fd < 0) {
      /* ABSENT is a first run and is not a failure; anything else means the
       * file is there and could not be opened, which is. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      LOGW("settings: cannot read %s (%d)", g_settings_path, errno);
      return LOAD_ERROR;
   }
   /* 192, and settings_render's length guard is the same number. See there. */
   char b[192];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* THE FILE EXISTS AND HAS NOTHING USABLE IN IT. A zero-length file is what
    * a power loss between create and write leaves behind; a negative read is
    * an I/O error. Neither is a first run. */
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT;
   b[n] = 0;
   /* THE VERSION FIRST, and nothing applied until it is one we know.
    *
    * A file from a NEWER build is refused whole rather than read as far as
    * this build understands: parsed partially, its extra fields would be
    * dropped and the very next save would write them away for good. */
   char *vq    = b;
   int filever = file_version(b, &vq, SETTINGS_VERSION);
   if (filever < 0) {
      LOGW("settings: %s is a NEWER format than this build knows; "
           "leaving it alone",
           g_settings_path);
      return LOAD_CORRUPT;
   }
   int v[25] = {
       g_p.sound_on,      g_p.vib_on,       g_p.orient,      g_p.units,
       g_p.disc,          g_p.plot_max,     g_p.screen_on,   g_p.newdata_mode,
       g_p.ins_marker[0], g_p.ins_color[0], g_p.ins_size[0], g_p.ins_marker[1],
       g_p.ins_color[1],  g_p.ins_size[1],  g_p.statbar_val, g_p.lockscr_val,
       g_p.nudge_sound,   g_p.nudge_vib,    g_p.wunits,      g_p.shortcut[0],
       g_p.shortcut[1],   g_p.shortcut[2],  g_p.shortcut[3], g_p.shortcut[4],
       g_p.shortcut[5]};
   /* VERSION 0 AND VERSION 1 SHARE THIS READER, and that is the migration:
    * v1 added the marker and changed nothing else, so a v0 file is read
    * field-for-field as it always was and is rewritten as v1 at the next
    * save. When a future version changes a FIELD, this is where the ordered
    * step for it goes -- keyed on `filever`, applied in order, with the v0
    * reader kept for the files already on phones. */
   char *q = vq;
   for (int i = 0; i < 25; i++) {
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
   g_p.newdata_mode = (v[7] >= ND_OFF && v[7] <= ND_CHIRP) ? v[7] : ND_OFF;
   /* Fields 9-14 are newer than some files on disk: out-of-range (or absent,
    * leaving the default) falls back to the defaults.
    *
    * THE BOUNDS COME FROM sensors.h NOW. They used to be literals, with a
    * comment saying "9 == MARK_N, 7 colours, 4 == MARK_SIZE_MAX; settings.c
    * stays decoupled from sensors.h, crosschecked by eye" -- and the eye is
    * what failed: MARK_SIZE_MAX is 5, not 4. The size picker offers 1..5 and
    * menu_action saves whatever it is handed, so choosing the LARGEST insulin
    * marker worked, persisted to disk, and was then silently reset to 2 by
    * this line on the next launch. A setting that quietly forgets itself
    * across a restart is worse than one that refuses the value outright.
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

static void render_code(struct save_job *j)
{
   static unsigned written;
   int n = 0;
   while (g_p.code_str[n] && n < (int)sizeof j->buf)
      n++;
   memcpy(j->buf, g_p.code_str, (size_t)n);
   job_stamp(j, g_code_path, &written, n, 1);
}

int code_save(void)
{
   int rc = save_now(render_code);
   if (rc)
      LOGW("settings: pairing code not saved");
   return rc;
}

/* A hostname or an IPv4 address: labels of letters, digits and hyphens
 * separated by single dots, no empty label, none starting or ending with a
 * hyphen. It used to accept a dotted quad ONLY, which was right when the
 * server was a box on the LAN and wrong the moment it got a name.
 *
 * Deliberately permissive about the whole name -- "duo", "pancra.org" and
 * "192.168.0.243" are all things the user legitimately types -- because this
 * is a field they typed, not a security boundary; what it must not do is
 * accept something that cannot be a host at all and then silently point every
 * future sync at nothing. */
int remote_server_valid(const char *s)
{
   if (!s || !*s)
      return 0;
   int n     = 0;
   int label = 0;
   for (const char *p = s; *p; p++) {
      if (++n > 63)
         return 0;
      if (*p == '.') {
         if (label == 0 || p[-1] == '-')
            return 0; /* empty label, or one ending in a hyphen */
         label = 0;
         continue;
      }
      int alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9');
      if (!alnum && *p != '-')
         return 0;
      if (label == 0 && *p == '-')
         return 0; /* a label may not start with a hyphen */
      if (++label > 63)
         return 0;
   }
   return label > 0 && s[n - 1] != '-';
}

static void render_remote(struct save_job *j)
{
   static unsigned written;
   /* "on server port uid keyhex". The two trailing fields were added with
    * pairing; a file without them loads as "not paired", so an upgrade keeps
    * the server and simply asks to pair again. */
   char kh[33];
   static const char hx[] = "0123456789abcdef";
   for (size_t i = 0; i < 16; i++) {
      kh[2 * i]       = hx[g_sync_key[i] >> 4U];
      kh[(2 * i) + 1] = hx[g_sync_key[i] & 15U];
   }
   kh[32] = 0;
   int n  = snprintf(j->buf, sizeof j->buf, "v%d %d %s %d %ld %s %s\n",
                     REMOTE_VERSION, g_p.remote_on ? 1 : 0,
                     g_p.remote_server[0] ? g_p.remote_server : "-",
                     g_p.remote_port, g_sync_uid, g_sync_uid > 0 ? kh : "-",
                     g_sync_email[0] ? g_sync_email : "-");
   job_stamp(j, g_remote_path, &written, n, n > 0 && n < 256);
}

int remote_save(void)
{
   int rc = save_now(render_remote);
   if (rc)
      LOGW("settings: remote config not saved");
   return rc;
}

enum load_result remote_load(void)
{
   int fd = open(g_remote_path, O_RDONLY, 0);
   if (fd < 0) {
      /* ABSENT is a first run and is not a failure; anything else means the
       * file is there and could not be opened, which is. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      LOGW("settings: cannot read %s (%d)", g_remote_path, errno);
      return LOAD_ERROR;
   }
   /* 256, matching what remote_save writes. It was 48, sized for
    * "1 1.2.3.4 8080" -- and the line has since grown a host NAME, a user id,
    * a 32-character key and an email address. The read simply truncated, so
    * the key came back malformed and the account came back empty, and the app
    * looked like it had forgotten a pairing it had actually stored. Both ends
    * of this file are now the same size for that reason. */
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* THE FILE EXISTS AND HAS NOTHING USABLE IN IT. A zero-length file is what
    * a power loss between create and write leaves behind; a negative read is
    * an I/O error. Neither is a first run. */
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT;
   b[n] = 0;
   /* The version first; a newer format is refused whole. See file_version. */
   char *q     = b;
   int filever = file_version(b, &q, REMOTE_VERSION);
   if (filever < 0) {
      LOGW("settings: %s is a NEWER format than this build knows; "
           "leaving it alone",
           g_remote_path);
      return LOAD_CORRUPT;
   }
   /* v0 and v1 differ only by the marker: the fields below are read the same
    * way for both, and a v0 file becomes v1 at the next save. A future
    * version that changes a FIELD adds its ordered step here. */
   if (*q != '0' && *q != '1')
      return LOAD_CORRUPT; /* garbage: keep the prior values, like every sibling
                              loader */
   int on = *q++ - '0';
   while (*q == ' ')
      q++;
   char host[sizeof g_p.remote_server];
   int k = 0;
   while (*q && *q != ' ' && *q != '\n') {
      if (k >= (int)sizeof host - 1)
         return LOAD_CORRUPT; /* an address that long cannot be valid */
      host[k++] = *q++;
   }
   host[k] = 0;
   while (*q == ' ')
      q++;
   int port = 0;
   int nd   = 0; /* see alarm_load: cap the digits, advance outside the cap */
   while (*q >= '0' && *q <= '9') {
      if (nd < 9) {
         port = (port * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   /* "-" is the saver's own empty-address marker; anything else must be a
    * well-formed host name, and the port must be a real TCP port. Commit all
    * three together or nothing: a half-applied file (say, a valid port with a
    * corrupt address) could silently re-point the push at the wrong host. */
   int host_ok = (host[0] == '-' && host[1] == 0) || remote_server_valid(host);
   if (!host_ok || port < 1 || port > 65535)
      return LOAD_CORRUPT;
   /* The paired identity and the account, read as SEQUENTIAL TOKENS.
    *
    * They were read by OFFSET -- the email was taken from 32 bytes past the
    * uid, the length of a key in hex. That is right only when a key is
    * actually there: unpaired, the saver writes "-" for the key, so the
    * offset landed inside the next field and the email loaded as "-", which
    * the check below then treated as unset. The address the user had typed
    * vanished on the next launch, with nothing to say why. Never compute a
    * position in a whitespace-separated line; walk it. */
   while (*q == ' ')
      q++;
   long uid = 0;
   int ud   = 0;
   while (*q >= '0' && *q <= '9') {
      if (ud < 18) {
         uid = (uid * 10) + (*q - '0');
         ud++;
      }
      q++;
   }
   while (*q == ' ')
      q++;
   /* the key token, however long it turns out to be */
   char keytok[80];
   int kk = 0;
   while (*q && *q != ' ' && *q != '\n' && kk < (int)sizeof keytok - 1)
      keytok[kk++] = *q++;
   keytok[kk] = 0;
   while (*q == ' ')
      q++;
   char em[sizeof g_sync_email];
   int ek = 0;
   while (*q && *q != ' ' && *q != '\n' && ek < (int)sizeof em - 1)
      em[ek++] = *q++;
   em[ek] = 0;

   unsigned char key[16];
   int keyok = 0;
   if (uid > 0 && kk == 32) {
      keyok = 1;
      for (int i = 0; i < 16 && keyok; i++) {
         int v = 0;
         for (int h = 0; h < 2; h++) {
            char c = keytok[(2 * i) + h];
            int d  = -1;
            if (c >= '0' && c <= '9')
               d = c - '0';
            else if (c >= 'a' && c <= 'f')
               d = (c - 'a') + 10;
            else if (c >= 'A' && c <= 'F')
               d = (c - 'A') + 10;
            if (d < 0) {
               keyok = 0;
               break;
            }
            v = (v * 16) + d;
         }
         key[i] = (unsigned char)v;
      }
   }
   g_p.remote_on   = on;
   g_p.remote_port = port;
   if (ek && !(em[0] == '-' && em[1] == 0)) {
      for (int i = 0;; i++) {
         g_sync_email[i] = em[i];
         if (!em[i])
            break;
      }
   } else {
      g_sync_email[0] = 0;
   }
   if (uid > 0 && keyok) {
      g_sync_uid = uid;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = key[i];
   } else {
      g_sync_uid = 0;
   }
   creds_publish();
   if (host[0] == '-')
      g_p.remote_server[0] = 0;
   else
      for (int i = 0;; i++) {
         g_p.remote_server[i] = host[i];
         if (!host[i])
            break;
      }
   return LOAD_OK;
}

int sync_key_save(long uid, const unsigned char key[16])
{
   /* UNDER set_lk, like every other mutation of this state -- and it was not.
    *
    * This writes the paired identity and then saves it, on the pairing
    * worker's thread, while the main thread reads the same fields through
    * sync_creds_get(). Every other setter in this file takes the lock; this
    * one never did, so the copy a request signs with could be assembled from
    * a uid written here and a key from before it. */
   mutex_lock(&set_lk);
   long old_uid = g_sync_uid;
   unsigned char old_key[16];
   for (int i = 0; i < 16; i++)
      old_key[i] = g_sync_key[i];
   g_sync_uid = uid;
   for (int i = 0; i < 16; i++)
      g_sync_key[i] = key[i];
   creds_publish();
   struct save_job j;
   render_remote(&j);
   mutex_unlock(&set_lk);
   if (write_job(&j) == 0)
      return 0;
   /* PUT BACK EVERY FIELD. A half-rolled-back identity -- the uid restored
    * while the key stays -- fails every request with nothing on screen to
    * say why. */
   mutex_lock(&set_lk);
   if (g_set_gen == j.gen) { /* see set_int */
      g_sync_uid = old_uid;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = old_key[i];
      creds_publish();
   }
   mutex_unlock(&set_lk);
   return -1;
}

enum load_result code_load(void)
{
   int fd = open(g_code_path, O_RDONLY, 0);
   if (fd < 0) {
      /* ABSENT is a first run and is not a failure; anything else means the
       * file is there and could not be opened, which is. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      LOGW("settings: cannot read %s (%d)", g_code_path, errno);
      return LOAD_ERROR;
   }
   char b[16];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* THE FILE EXISTS AND HAS NOTHING USABLE IN IT. A zero-length file is what
    * a power loss between create and write leaves behind; a negative read is
    * an I/O error. Neither is a first run. */
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT;
   int k = 0;
   for (int i = 0; i < n && k < (int)sizeof g_p.code_str - 1; i++)
      if (b[i] >= '0' && b[i] <= '9')
         g_p.code_str[k++] = b[i];
   /* Only commit when at least one digit was parsed. A non-empty file with no
    * digits (a partial write, or a hand-edit) would otherwise wipe a working
    * code to "" -- every sibling loader preserves its prior value on garbage.
    */
   if (k > 0)
      g_p.code_str[k] = 0;
   return LOAD_OK;
}

/* THE FIVE FILES THIS MODULE OWNS. The shell hands over the data directory
 * and nothing else: a filename belongs with the code that reads and writes
 * it, so renaming one is a local change rather than an edit to the activity's
 * startup. */
int settings_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_info_path, sizeof g_info_path, dir, "/stelo.info");
   ok &= data_path(g_alarm_path, sizeof g_alarm_path, dir, "/alarm.cfg");
   ok &=
       data_path(g_settings_path, sizeof g_settings_path, dir, "/settings.cfg");
   ok &= data_path(g_code_path, sizeof g_code_path, dir, "/paircode.txt");
   ok &= data_path(g_remote_path, sizeof g_remote_path, dir, "/remote.cfg");
   return ok;
}

/* THE TWO CREDENTIAL PATHS, for the sync client that must know which files
 * NOT to upload: they hold the pairing code and the derived key, and sending
 * them would put the secret that authenticates us TO the server inside the
 * server's own database. The other three are nobody else's business. */
const char *code_path(void)
{
   return g_code_path;
}

const char *remote_path(void)
{
   return g_remote_path;
}

/* The alarm and settings files by name, for the tests that simulate a
 * CORRUPTED one -- the loaders' behaviour on garbage is half of what settings
 * persistence has to get right, and it cannot be checked without writing the
 * bytes. Read-only, like every path here: they are set once, by
 * settings_paths. */
const char *alarm_path(void)
{
   return g_alarm_path;
}

const char *settings_path(void)
{
   return g_settings_path;
}
