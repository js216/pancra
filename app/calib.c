// SPDX-License-Identifier: GPL-3.0
// calib.c --- calibration queue and rescale factor
// Copyright 2026 Jakob Kastelic

/* THE COORDINATOR, AND ONLY THAT.
 *
 * The two corrections are app/calibq.c (a value the SENSOR must accept) and
 * app/rescale.c (local arithmetic on what it reports). See calib.h for what
 * each is for and app/calibint.h for why they are two modules with one
 * coordinator rather than one file of parallel globals.
 *
 * What is left here is what they genuinely share, and nothing else: the two
 * locks, the read/write of a one-line record, the monotonic window helper,
 * the startup that loads both, the tick that runs both halves' deadlines and
 * save-retries under one hold, and calib_view -- which fills ONE device row
 * from BOTH under ONE lock, because a row assembled from two instants shows
 * one sensor's correction beside another's verdict.
 */
#include "calib.h"
#include "calibint.h"
#include "clock.h"
#include "csvcur.h" /* the shared field reader: one grammar */
#include "dexdriver.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "shell.h"
#include "thread.h" /* the rescale state's own lock */
#include "uimodel.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* read_line tells a missing file from an unreadable one */
#endif

/* ---- THE DEADLINES, WHICH ARE INTERVALS AND NOT INSTANTS ---------------
 *
 * Every timestamp above is an INSTANT: when the user confirmed a value, when
 * a factor became effective, when a calibration resolved. Those are wall-clock
 * numbers because they are written to files, compared against reading
 * timestamps, and shown to a person. That is correct and unchanged.
 *
 * These four are the other kind. They answer "has enough time PASSED?", which
 * a difference of two realtime_s() stamps cannot: a wall-clock correction -- a
 * phone finding a network, an NTP step, a user fixing the date, a timezone
 * database update that moves UTC on a badly-set device -- moves that
 * difference by the size of the correction, instantly, and this module's whole
 * job is decided by it.
 *
 * WHAT THAT DID TO THE PERSON HOLDING THE PHONE. They take a fingerstick, type
 * 118, and confirm. Then:
 *
 *   FORWARD JUMP. `now - g_q.t` leaps past CALQ_WINDOW_S, and the very next
 *   tick declares the calibration FAILED -- seconds after they entered it, on
 *   a sensor that was streaming fine. The row says the sensor never accepted
 *   it. The sensor was never even asked. The same jump makes both throttles
 *   ("one 0x32 a minute", "one 0x34 a minute") fire on every single reading,
 *   so a sensor that has to be treated gently is written to five times a
 *   minute instead. And a pending rescale, waiting for the next reading to
 *   compute its factor, EXPIRES before that reading arrives.
 *
 *   BACKWARD JUMP. The mirror, and the quieter one. `now - g_q.t` goes
 *   NEGATIVE, so the give-up window never lapses and the queue retries for as
 *   long as the process lives -- long past the twenty minutes after which a
 *   fingerstick reference is no longer worth applying. The throttles go the
 *   same way: `now - sent < 60` stays true for an hour, so the ONE retry that
 *   would have got the value into the sensor is never made, and the
 *   calibration silently never happens. The user typed a number, watched it
 *   say PENDING, and nothing ever went out.
 *
 * So the process keeps its own deadlines on mono_s(), which counts elapsed
 * seconds and cannot be set. Each is an ABSOLUTE monotonic second, compared
 * with the same >/>= the wall-clock arithmetic used, so the boundaries are
 * exactly where they were; 0 means "no deadline outstanding". None of them is
 * persisted and none of them can be: a monotonic clock counts from an
 * arbitrary origin and restarts with the kernel, so it is meaningless in a
 * file. The realtime stamps above are what crosses a restart, and calq_load /
 * rescale_load turn a persisted age back into one of these (see
 * CLOCK_SKEW_TOL_S in calib.h for what they do with an age that is negative or
 * larger than the window).
 *
 * cal_lk's, like everything else in this file. */
/* THE FOUR OF THEM LIVE IN THE TWO RECORDS ABOVE -- g_q.giveup_at,
 * g_q.resend_at, g_q.probe_at and g_r.pend.expire_at -- because each is part
 * of what its mechanism IS, and a rollback that restored the value without
 * the deadline would produce a calibration with no window or one that is
 * rewritten to the sensor immediately. What each of them means:
 *
 *   g_q.giveup_at        past this, a queued calibration that the sensor has
 *                        not accepted within CALQ_WINDOW_S resolves FAILED.
 *                        0 = nothing queued.
 *   g_q.resend_at        before this, no further 0x34 goes out -- one write a
 *                        minute, and the sensor's reply normally resolves it
 *                        on the first. 0 = send now. A DEADLINE, not a stamp
 *                        of the last attempt: a deadline lapses by itself,
 *                        where a stamp needs somebody to clear it after 60 s
 *                        of silence -- a second rule, in the tick, that has
 *                        to agree with this one.
 *   g_q.probe_at         before this, no further 0x32 permission probe goes
 *                        out. 0 = probe now. HERE RATHER THAN struct
 *                        dex_cal.asked, which the DRIVER stamps with
 *                        realtime_s(): keeping the deadline in this module is
 *                        what lets it be on the clock the decision needs.
 *   g_r.pend.expire_at   past this, a rescale target still waiting for a
 *                        reading EXPIRES. 0 = nothing pending. */

static struct mutex cal_lk = MUTEX_INIT;

/* THE LOCK IS REACHED BY NAME, not by the mutex. The two modules
 * take it and this file owns it, which is the same rule the driver's lock
 * follows: a caller names an OPERATION, and only the owner names the state.
 * (Both are recursive-free leaf mutexes; nothing here nests them.) */
void cal_lock(void)
{
   mutex_lock(&cal_lk);
}

void cal_unlock(void)
{
   mutex_unlock(&cal_lk);
}

/* ---- THE WRITE IS NOT UNDER cal_lk -------------------------------------
 *
 * Both saves end in atomic_replace: two fsyncs and a rename. cal_lk is a
 * spinning mutex and calib_view is called from the model build on the MAIN
 * thread for EVERY FRAME -- so a save on a binder or service thread that held
 * cal_lk across those syscalls would put the looper into a busy-wait for the
 * duration of a disk flush. That is the ANR shape, and it is the same one
 * meter_sync_save had.
 *
 * So a save renders the line under cal_lk -- which is what makes the record
 * ONE snapshot, the point of this file's locking -- then RELEASES it and does
 * the I/O holding calfile_lk instead. calfile_lk keeps the writers single
 * file (two concurrent replaces of one path is a lost update however atomic
 * each is), and no reader ever takes it, so no reader ever waits for a disk.
 *
 * THE ORDER IS calfile_lk -> cal_lk, always. Every mutating entry point takes
 * calfile_lk first, so nothing else can mutate during the window in which a
 * save has let cal_lk go; readers take cal_lk alone and may see the new state
 * a moment before the file does, which is exactly what `unsaved` reports. */
static struct mutex calfile_lk = MUTEX_INIT;

void cal_file_lock(void)
{
   mutex_lock(&calfile_lk);
}

void cal_file_unlock(void)
{
   mutex_unlock(&calfile_lk);
}

/* ---- WHEN THE SCREEN IS AHEAD OF THE DISK -----------------------------
 *
 * Most changes here are the user's, and a user-driven change that cannot be
 * written is UNDONE: nothing happened, the screen says so, and the file and
 * the memory still agree. That is the honest answer when the input can simply
 * be given again.
 *
 * The AUTOMATIC transitions cannot do that, and it is not a shortcut that
 * they do not. A rescale activation is computed from ONE SAMPLE, which will
 * not come again; an expiry is a deadline that has already passed; an
 * ACCEPTED or REJECTED verdict is the sensor's answer, and there is no way to
 * ask it twice. Rolling any of them back would leave a pending target that
 * the next reading re-activates from a different raw -- a different factor,
 * silently -- or a queue that goes on being sent to a sensor that has already
 * answered it.
 *
 * So those transitions STAND, and the fact that the file is behind is state
 * in its own right: recorded here, RETRIED on every calib_tick, and shown
 * (calib_view.cal_unsaved / .rescale_unsaved) so the row does not read as
 * durable when it is not. The retry runs from calib_tick, which BOTH the
 * activity's timer and the service heartbeat drive -- the service one
 * matters, because a reading that arrives with no activity alive is the
 * likeliest way to get here.
 * The alternative is `(void)save()` -- a write whose failure nothing records,
 * after which the screen shows a factor the next launch will not have.
 *
 * Both flags are cal_lk's, like everything else in this file. */
/* ---- persistence ------------------------------------------------------ */

/* ---- ONE EXACT DECODER FOR BOTH FILES ----------------------
 *
 * Both are a single line of comma-separated integers, and the whole line is
 * one record: a partial parse of it is not a usable answer, which is why this
 * is here rather than a strtol per field at two call sites.
 *
 * WHAT A LENIENT SCANNER ACCEPTS. Walking the bytes accumulating digits,
 * treating any '-' anywhere as a sign, ignoring everything else and letting
 * missing trailing fields stay 0, "9,,x,-" parses, "9" parses, "1-2" is -12,
 * and a run of digits longer than a long silently wraps. Every one of
 * those produced a RECORD -- a queued calibration, or a rescale factor -- out
 * of bytes this program did not write. What a wrapped or invented factor does
 * is rescale every reading on the screen by it.
 *
 * WHAT IT ACCEPTS NOW: exactly `n` fields, each a whole decimal integer that
 * fits, separated by single commas, and nothing after the last one but an
 * optional newline. Anything else is refused, and `v` is left untouched so a
 * caller cannot half-apply a line.
 *
 * The field reader is the shared one (app/csvcur.h): its three answers --
 * present, empty, overflowed -- are three cases a hand-rolled reader
 * conflates.
 *
 * 1 when the whole line decoded, 0 when it did not. */
int cal_parse_ints(const char *b, long *v, int n)
{
   const char *e = b;
   while (*e && *e != '\n')
      e++;
   struct csv_cur c;
   csv_open(&c, b, e);
   long got[CAL_FIELDS_MAX];
   if (n < 1 || n > CAL_FIELDS_MAX)
      return 0;
   for (int i = 0; i < n; i++) {
      enum csv_field why = CSV_FIELD_OK;
      got[i]             = csv_num(&c, &why);
      if (why != CSV_FIELD_OK)
         return 0;
      if (i < n - 1 && !csv_sep(&c))
         return 0;
   }
   if (c.p != e)
      return 0; /* a field too many, or rubbish after the last one */
   for (int i = 0; i < n; i++)
      v[i] = got[i];
   return 1;
}

int cal_read_line(const char *path, char *b, int cap)
{
   /* ONE EXACT READ. With a single unchecked read whose return is taken as
    * the file's length, a short read publishes a prefix of the record, EINTR
    * truncates it, and a file longer than `cap` decodes as a valid head.
    * read_file_exact answers those; the three READ_* answers this
    * module's callers expect are a translation of its four.
    *
    * A FILE LONGER THAN THE BUFFER IS A FAILURE, not an absence: it is not
    * something this build wrote, and treating it as "never written" would
    * silently reset a calibration queue or a rescale factor that is on the
    * disk. */
   int n               = 0;
   enum load_result rr = read_file_exact(path, b, cap, &n);
   switch (rr) {
      case LOAD_OK: return READ_OK;
      case LOAD_ABSENT: return READ_NONE;
      case LOAD_CORRUPT:
         /* Empty is "created and not written", which for these two files is
          * the same as never written -- the loaders' own comment. Anything
          * else CORRUPT means too long. */
         return n == 0 ? READ_NONE : READ_FAIL;
      case LOAD_ERROR:
      default: return READ_FAIL;
   }
}

/* 0 when the file now holds `b`, -1 when it does not.
 *
 * REPLACE_UNSYNCED counts as written: the rename is done, so the file HAS the
 * new line and only its survival of a power cut is unknown. Treating it as a
 * failure would roll the calibration state back to a value the disk no longer
 * holds -- and this module's whole contract is that memory and the file agree
 * or the difference is recorded (see g_calq_unsaved). The tick's retry
 * rewrites anyway, which is what eventually settles the directory entry. */
int cal_write_line(const char *path, const char *b, int n)
{
   return atomic_replace(path, b, clampn(n, n + 1)) == REPLACE_FAILED ? -1 : 0;
}

long cal_window_left(long stamp, long window)
{
   long age = realtime_s() - stamp;
   if (age < 0) {
      if (age < -CLOCK_SKEW_TOL_S)
         return NO_WINDOW_LEFT;
      age = 0;
   }
   if (age > window)
      return NO_WINDOW_LEFT;
   return window - age;
}

void calib_tick(void)
{
   driver_cal_tick();
}

/* ---- lifecycle --------------------------------------------------------- */

int calib_paths(const char *dir)
{
   /* EACH MODULE NAMES ITS OWN FILE. This built both paths here, which meant
    * the coordinator knew that a queue lives in cal.q and a factor in
    * rescale.cfg -- two facts that belong to the modules that read and write
    * them. What is shared is the DIRECTORY and the rule that both are
    * attempted: one unreadable name must not stop the other from being set.
    *
    * (Through data_path, like every other persistence owner. This used
    * snprintf and discarded the result, and snprintf truncates rather than
    * failing -- so an over-long data directory produced a well-formed path to
    * somewhere else, and both files were read from and written to a location
    * nothing else in the app agreed on.) */
   int q = cal_q_paths(dir);
   int r = cal_r_paths(dir);
   return q && r;
}

int calib_load(void)
{
   /* BOTH, always -- one unreadable file must not stop the other from being
    * restored. The caller is told if either was lost. */
   int q = cal_q_load();
   int r = cal_r_load();
   return (q == CALIB_OK && r == CALIB_OK) ? CALIB_OK : CALIB_UNSAVED;
}

/* THE TICK, WHICH IS BOTH MODULES' DEADLINES AND BOTH MODULES' RETRIES.
 *
 * MONOTONIC, because everything either half decides is "has enough time
 * passed?" -- and this is where a wall-clock jump does the most visible
 * damage. Comparing one realtime_s() against persisted stamps means a
 * correction forward of more than CALQ_WINDOW_S makes the very next tick
 * declare a calibration the user just confirmed FAILED, and a correction
 * backward makes every comparison go negative, so nothing ever expires and
 * nothing is ever retried. See the deadline block near the top of this file.
 *
 * calfile_lk OVER THE WHOLE TICK: the give-up resolve, the expiry and the two
 * save-retries all write, and this is the thread that most often does. cal_lk
 * is taken and dropped INSIDE each half, and every save drops it again for
 * the syscalls -- so the MAIN thread's per-frame calib_view never waits on a
 * disk.
 *
 * REGISTERED WITH THE DRIVER (see the ops below), so it arrives with the
 * driver's lock held: the order driver_lk -> cal_lk is what that means, and
 * nothing under cal_lk may call back into the driver. */
static void cal_tick_locked(void)
{
   long now = mono_s();
   cal_file_lock();
   int redraw = cal_q_tick(now);
   /* BOTH TICKS RUN. The rescale half is not short-circuited by the queue
    * half having asked for a repaint already. */
   if (cal_r_tick(now))
      redraw = 1;
   cal_file_unlock();
   if (redraw)
      shell_ui_dirty();
}

void calib_view(int sensor_id, struct calib_view *out)
{
   /* ONE LOCK, ONE ROW, and that is the whole reason this function is in the
    * coordinator rather than in either module. The queued value, the
    * last-resolved verdict, the active factor, a pending target and the two
    * notices are ONE row on the screen: read under two locks -- or under one
    * lock twice -- a resolution landing in between shows a live "PENDING 120"
    * beside the older "APPLIED 96" it has just replaced, or neither, or one
    * sensor's correction beside another's verdict.
    *
    * Each module fills its own half, with the lock already held. */
   cal_lock();
   cal_q_view(sensor_id, out);
   cal_r_view(sensor_id, out);
   cal_unlock();
}

/* THE CALIBRATION MODULE'S HALF of the driver's serialisation, registered
 * once at startup: the driver takes its own lock and calls straight back in.
 * See dexdriver.h. */
static const struct driver_cal_ops g_cal_ops = {
    .attempt    = cal_q_attempt_locked,
    .queue      = cal_q_queue_locked,
    .cancel     = cal_q_cancel_locked,
    .tick       = cal_tick_locked,
    .queued_for = cal_q_queued_for_locked,
};

void calib_register_ops(void)
{
   driver_set_cal_ops(&g_cal_ops);
}
