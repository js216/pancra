// SPDX-License-Identifier: GPL-3.0
// calib.c --- calibration queue and rescale factor
// Copyright 2026 Jakob Kastelic

/* See calib.h for what the two corrections are and why they share a file.
 *
 * Everything durable about both lives here as file-static state, reached only
 * through the functions in the header. It used to be twenty-odd globals in
 * main.c, read and written from the reading path, the driver callback, the
 * keypad handlers and the renderer -- so "what can change this factor?" had
 * no answer shorter than the whole file.
 */
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "dexlibc.h"
#include "log.h"
#include "shell.h"
#include "thread.h" /* the rescale state's own lock */
#include "uimodel.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* read_line tells a missing file from an unreadable one */
#endif
#include <stdio.h>

/* DURABLE calibration queue: a CONFIRMED calibration that has not yet been
 * ACCEPTED by the sensor. It is persisted and retried on every stream until
 * the sensor answers -- so a calibration is NEVER lost to a reconnect gap or
 * an app restart, the way a one-shot write silently was. */
/* cal_lk's, all four, and persisted as ONE LINE together with the LAST CAL
 * record below -- which is why they share a lock rather than each being
 * written under whichever one its writer happened to hold. */
static int g_calq_mgdl; /* queued value, mg/dL; 0 = none queued */
static int g_calq_id;   /* sensor id it is for */
/* realtime_s() when the user confirmed it. AN INSTANT, and PERSISTED: it is
 * the only thing the next launch can subtract its own wall clock from to learn
 * how much of the window is left. It is NOT what the running process compares
 * against -- see the deadline block below. */
static long g_calq_t;
/* ---- WHICH CALIBRATION THIS IS, so a reply cannot be applied to another one
 *
 * g_calq_gen names the queue entry that is live RIGHT NOW. It travels with the
 * write (driver_calibrate) and comes back with the sensor's answer
 * (pancra_cal_result), which resolves the queue only when the sensor id, the
 * value AND this number all still match. Without it the driver had a single
 * boolean and the answer resolved whatever happened to be queued -- so a user
 * who replaced 100 with 180 before the sensor answered got 180 marked ACCEPTED
 * without one byte of 180 ever having been sent, and the sensor went on
 * reporting against 100 for the rest of the session.
 *
 * IT IS NOT A CLOCK, and it must never become one. The obvious alternative --
 * stamp each write with realtime_s() and match on the stamp -- would make the
 * IDENTITY of a calibration depend on the wall clock, so an NTP step between
 * the write and the reply would either lose a genuine answer or (with a
 * backward step and a re-queue) let two writes share a stamp. A counter cannot
 * be corrected. It is deliberately NOT named g_cal_*_at: that shape is
 * reserved for the deadlines below, and `make clockcheck` matches on it.
 *
 * g_calq_gen_next is the source of the numbers and only ever goes UP. The live
 * one is part of what a queue IS, so it is snapshotted and restored with the
 * rest of the queue (struct calq_undo) -- a rolled-back queue must keep its
 * identity, or a reply to the write that is genuinely still outstanding would
 * be discarded. The counter behind it is deliberately outside the undo, so a
 * rolled-back transaction never hands the same number to a later, different
 * queue.
 *
 * WRAPAROUND is not a concern worth code: it is unsigned (defined behaviour),
 * one entry is one deliberate user action, and reaching 2^32 of them would
 * take longer than the phone will exist. */
static unsigned g_calq_gen;      /* identity of the live queue; 0 = none */
static unsigned g_calq_gen_next; /* only ever incremented */
static char g_calq_path[256];    /* persistence file */
/* Last RESOLVED calibration, for the per-device LAST CAL row (persisted).
 *
 * cal_lk's, like the queue above it and for the same reason: the two share
 * one line in one file, so a save must see both at one instant. Written by
 * calq_resolve and by the load, read as a four-field set by the renderer --
 * under one lock either way, so a row cannot show one calibration's value
 * beside another's verdict.
 *
 * (It used to say the write side "needs no lock against itself" because
 * every writer was driver-serialised. That stopped being true when the queue
 * moved under cal_lk: the tick and the load write these too.) */
static int g_lastcal_mgdl;  /* value of the last resolved calibration */
static long g_lastcal_t;    /* realtime_s() it resolved; 0 = never */
static int g_lastcal_state; /* CAL_ST_* (ui.h) */
static int g_lastcal_id;    /* sensor id it was for */

/* RESCALE: a persistent multiplicative correction (permille; 1000 = none) the
 * user sets from a fingerstick. Applied to THIS CGM's readings whose
 * timestamp is AT OR AFTER the moment rescaling was (re)activated -- so a
 * backfilled point with an OLDER timestamp is never rescaled even though it
 * arrives later. Clamped to +-25%. */
static int g_rescale_pm = 1000; /* active factor; 1000 = off */
static int g_rescale_id;        /* sensor id it applies to */
static long g_rescale_t;        /* activation instant; readings t>=this scale */
/* PENDING target: a confirmed rescale that could not be computed yet because
 * no live reading was available. Held (PERSISTED, incl. its request time)
 * until the next reading for this sensor, then turned into a factor -- never
 * silently lost, and it survives an app restart. */
static int g_rescale_pend_mgdl; /* target mg/dL awaiting a reading; 0 = none */
static int g_rescale_pend_id;   /* sensor id it is for */
static long g_rescale_pend_t;   /* realtime_s() when the user requested it */
/* Last attempt exceeded +-25% and was REJECTED (not clamped), or a pending one
 * EXPIRED. Shown in the RESCALE line until the user sets a valid one or
 * stops. In-memory only (a transient notice). */
static int g_rescale_reject;
static int g_rescale_reject_id;
static int g_rescale_expired;
static int g_rescale_expired_id;
static char g_rescale_path[256]; /* persistence file */

/* ---- THE DEADLINES, WHICH ARE INTERVALS AND NOT INSTANTS ---------------
 *
 * Every timestamp above is an INSTANT: when the user confirmed a value, when
 * a factor became effective, when a calibration resolved. Those are wall-clock
 * numbers because they are written to files, compared against reading
 * timestamps, and shown to a person. That is correct and unchanged.
 *
 * These four are the other kind. They answer "has enough time PASSED?", and
 * every one of them used to be answered by subtracting a realtime_s() stamp
 * from a fresh realtime_s(). A wall-clock correction -- a phone finding a
 * network, an NTP step, a user fixing the date, a timezone database update
 * that moves UTC on a badly-set device -- moves that difference by the size of
 * the correction, instantly, and this module's whole job is decided by it.
 *
 * WHAT THAT DID TO THE PERSON HOLDING THE PHONE. They take a fingerstick, type
 * 118, and confirm. Then:
 *
 *   FORWARD JUMP. `now - g_calq_t` leaps past CALQ_WINDOW_S, and the very next
 *   tick declares the calibration FAILED -- seconds after they entered it, on
 *   a sensor that was streaming fine. The row says the sensor never accepted
 *   it. The sensor was never even asked. The same jump makes both throttles
 *   ("one 0x32 a minute", "one 0x34 a minute") fire on every single reading,
 *   so a sensor that has to be treated gently is written to five times a
 *   minute instead. And a pending rescale, waiting for the next reading to
 *   compute its factor, EXPIRES before that reading arrives.
 *
 *   BACKWARD JUMP. The mirror, and the quieter one. `now - g_calq_t` goes
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
/* Past this, the queued calibration has not been accepted in CALQ_WINDOW_S and
 * resolves FAILED. 0 = nothing queued. */
static long g_cal_giveup_at;
/* Before this, no further 0x34 goes out -- one calibration write a minute, and
 * the sensor's reply normally resolves it on the first. 0 = send now.
 *
 * This replaces g_calq_sent, which held realtime_s() of the last attempt and
 * was never persisted (calq_load always zeroed it), so nothing is lost by it
 * becoming a deadline. The tick used to also clear that stamp after 60 s of
 * silence; a deadline lapses by itself, so that clause is gone rather than
 * duplicated. */
static long g_cal_resend_at;
/* Before this, no further 0x32 permission probe goes out. 0 = probe now.
 *
 * The throttle used to read struct dex_cal.asked, which the DRIVER stamps with
 * realtime_s(). Keeping the deadline here rather than there is what lets this
 * module put it on the clock the decision needs; the driver's `asked` is still
 * its own record of when it last sent the request. */
static long g_cal_probe_at;
/* Past this, a rescale target that is still waiting for a reading EXPIRES.
 * 0 = nothing pending. */
static long g_cal_pend_expire_at;

/* Latest RAW (pre-rescale) reading per link -- the reference a future factor
 * is computed from, which is the only reason it is kept. */
static int g_link_raw[LINK_MAX];

/* THE STATE LOCK, over EVERYTHING this file holds.
 *
 * It started as the rescale half's alone: the queue was serialised by the
 * driver instead (the `_locked` functions below, reached through
 * driver_set_cal_ops with the driver's own lock held) while the rescale half
 * had nothing at all. Two locks over one persisted record is how a save came
 * to serialise a queue from one instant beside a verdict from another, so
 * cal_lk covers both now -- the queue, the last-resolved calibration and the
 * rescale factor.
 *
 * All of it is written by the user's actions on the MAIN thread
 * (calib_rescale_set / _stop, the queue and cancel), read and written by the
 * reading path on a BINDER thread (calib_on_reading, which activates a
 * pending target from the sample it was waiting for; pancra_cal_result,
 * which records the sensor's verdict), expired by calib_tick on the main
 * looper AND the service heartbeat, and read by every frame the renderer
 * draws (calib_view).
 *
 * A factor is not one number: it is (pm, sensor id, effective-from). The
 * activation writes those three separately, so a reader between the first and
 * the second got the NEW factor with the OLD sensor's id -- which is one
 * sensor's correction applied to another sensor's readings, in stored data.
 * The same shape covers the last-resolved calibration, four fields written by
 * a GATT callback and read as a set by the device row.
 *
 * A LEAF, and it must stay one: the order is driver_lk -> cal_lk, because the
 * `_locked` operations already arrive holding the driver's. Nothing here may
 * call a driver_* function while holding it -- calib_view asks the driver for
 * the queued value BEFORE taking it, for exactly that reason. */
static struct mutex cal_lk = MUTEX_INIT;

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
 * The alternative, and what this replaces, was `(void)save()` -- a write
 * whose failure nothing recorded, after which the screen showed a factor the
 * next launch would not have.
 *
 * Both flags are cal_lk's, like everything else in this file. */
static int g_calq_unsaved; /* cal.q is behind the queue/LAST CAL in memory */
static int g_resc_unsaved; /* rescale.cfg is behind the factor in memory */

/* ---- persistence ------------------------------------------------------ */

/* Both files are a single line of comma-separated integers. Parsed here
 * rather than with strtol per field because the whole line is one record and
 * a partial parse of it is not a usable answer. Missing trailing fields stay
 * 0, which is how an older file upgrades. */
static void parse_ints(const char *b, long *v, int n)
{
   int vi  = 0;
   int neg = 0;
   for (const char *p = b; *p && vi < n; p++) {
      if (*p >= '0' && *p <= '9') {
         v[vi] = (v[vi] * 10) + (*p - '0');
      } else if (*p == '-') {
         neg = 1;
      } else if (*p == ',' || *p == '\n') {
         if (neg)
            v[vi] = -v[vi];
         neg = 0;
         vi++;
         if (*p == '\n')
            break;
      }
   }
}

/* Read one line. THREE ANSWERS, not two.
 *
 * READ_NONE  the file is not there. A first run, or a state that was never
 *            written: the defaults are correct and nothing is lost.
 * READ_OK    the line is in `b`.
 * READ_FAIL  the file EXISTS and could not be read whole.
 *
 * The third used to be folded into the first, so an unreadable calibration
 * queue was indistinguishable from never having queued one -- the app
 * silently forgot a calibration the user had confirmed, and a rescale factor
 * that was scaling every reading from a sensor silently reverted to 1.000.
 * Both look exactly like a fresh install, which is the one thing they are
 * not. */
#define READ_NONE 0
#define READ_OK   1
#define READ_FAIL (-1)

static int read_line(const char *path, char *b, int cap)
{
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? READ_NONE : READ_FAIL;
   long n = read(fd, b, (size_t)cap - 1);
   close(fd);
   if (n < 0)
      return READ_FAIL;
   if (n == 0)
      return READ_NONE; /* present but empty: the same as never written */
   b[n] = 0;
   return READ_OK;
}

/* 0 when the file now holds `b`, -1 when it does not.
 *
 * REPLACE_UNSYNCED counts as written: the rename is done, so the file HAS the
 * new line and only its survival of a power cut is unknown. Treating it as a
 * failure would roll the calibration state back to a value the disk no longer
 * holds -- and this module's whole contract is that memory and the file agree
 * or the difference is recorded (see g_calq_unsaved). The tick's retry
 * rewrites anyway, which is what eventually settles the directory entry. */
static int write_line(const char *path, const char *b, int n)
{
   return atomic_replace(path, b, clampn(n, n + 1)) == REPLACE_FAILED ? -1 : 0;
}

/* THE TWO STATES ARE TRANSACTIONS, and these are the halves that make them
 * so: keep a copy of everything the file holds, write, and put the copy back
 * when the write fails. See calib.h.
 *
 * Both files are replaced by rename (atomic_replace), so a failed write
 * leaves the previous file whole -- which is what makes "nothing changed"
 * true of memory AND disk rather than only of disk. A calibration the user
 * confirmed and the app forgot at the next launch is the failure this
 * prevents; the opposite -- a queue that survives on disk while memory says
 * it is gone -- is what discarding the result allowed. */
struct calq_undo {
   /* THE DEADLINES TRAVEL WITH THE QUEUE, and they are longs for the same
    * reason `sent` had to be: this used to hold the last write attempt as an
    * INT, so a rolled-back queue came back with an attempt stamped in 1970 --
    * and the retry gate read that stamp.
    *
    * They belong in the undo because they are part of what a queue IS. Restore
    * the value without restoring `giveup_at` and the rolled-back calibration
    * comes back with no window at all; without `resend_at` it is written to
    * the sensor again immediately, which is the hammering the throttle exists
    * to prevent. Nothing here reaches the file -- see the deadline block near
    * the top -- so the undo is the only thing that carries them. */
   long t, lt, giveup_at, resend_at, probe_at;
   int id, mgdl, lmgdl, lstate, lid;
   /* THE GENERATION TRAVELS WITH THE QUEUE for the same reason the deadlines
    * do: it is part of what a queue IS. A queue restored without its identity
    * would refuse the sensor's answer to the write that is genuinely still
    * outstanding for it, and the calibration would sit PENDING until the
    * window lapsed and then report FAILED -- on a value the sensor had already
    * accepted. (The COUNTER behind it, g_calq_gen_next, is not restored: it
    * must never hand the same number out twice.) */
   unsigned gen;
};

/* ---- THE PERSISTED QUEUE RECORD, WHICH IS ONE THING ---------------------
 *
 * The queued calibration and the LAST CAL record share one file and one line,
 * and they were under two different locks: the queue relied on the driver's
 * (every mutation is a `_locked` op the driver calls), while LAST CAL was
 * under cal_lk. calq_save read BOTH and wrote them as one line -- so a save
 * on the driver thread could serialise a queue from one instant beside a
 * last-resolved record from another, and calq_load could be halfway
 * through rewriting them.
 *
 * One lock covers both now: cal_lk, taken by the entry points below. The
 * helpers assume it is HELD -- which is also why none of them may call the
 * driver, since the documented order is driver_lk -> cal_lk and never the
 * reverse (see calq_attempt_locked, which does its driver work outside). */
static void calq_snapshot(struct calq_undo *u)
{
   u->id        = g_calq_id;
   u->mgdl      = g_calq_mgdl;
   u->t         = g_calq_t;
   u->gen       = g_calq_gen;
   u->giveup_at = g_cal_giveup_at;
   u->resend_at = g_cal_resend_at;
   u->probe_at  = g_cal_probe_at;
   u->lmgdl     = g_lastcal_mgdl;
   u->lt        = g_lastcal_t;
   u->lstate    = g_lastcal_state;
   u->lid       = g_lastcal_id;
}

static void calq_restore(const struct calq_undo *u)
{
   g_calq_id       = u->id;
   g_calq_mgdl     = u->mgdl;
   g_calq_t        = u->t;
   g_calq_gen      = u->gen;
   g_cal_giveup_at = u->giveup_at;
   g_cal_resend_at = u->resend_at;
   g_cal_probe_at  = u->probe_at;
   g_lastcal_mgdl  = u->lmgdl;
   g_lastcal_t     = u->lt;
   g_lastcal_state = u->lstate;
   g_lastcal_id    = u->lid;
}

/* CALLER HOLDS calfile_lk AND cal_lk, and gets both back. cal_lk is DROPPED
 * across the write -- see the note above calfile_lk for why that is not
 * optional. */
static int calq_save(const struct calq_undo *undo)
{
   /* queued (id,mgdl,t) then last-resolved (mgdl,t,state,id) on one line,
    * rendered while the lock still makes them one instant. */
   char b[112];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%ld,%d,%d\n", g_calq_id,
                    g_calq_mgdl, g_calq_t, g_lastcal_mgdl, g_lastcal_t,
                    g_lastcal_state, g_lastcal_id);
   mutex_unlock(&cal_lk);
   int wrote = write_line(g_calq_path, b, clampn(n, sizeof b));
   mutex_lock(&cal_lk);
   if (wrote == 0) {
      g_calq_unsaved = 0; /* the file now says what memory says */
      return CALIB_OK;
   }
   if (undo) {
      calq_restore(undo);
      return CALIB_UNSAVED; /* nothing happened: the file is still right */
   }
   /* NO UNDO MEANS THE TRANSITION STANDS. See the note above cal_lk. */
   g_calq_unsaved = 1;
   return CALIB_UNSAVED;
}

/* CALLER HOLDS cal_lk. `keep` = 1 for an automatic transition, which must NOT
 * be undone (see the note above cal_lk): the clear stands and the file is
 * marked behind. 0 for a user-driven cancel, which is undone. */
static int calq_clear(int keep)
{
   struct calq_undo undo;
   calq_snapshot(&undo);
   g_calq_mgdl = 0;
   g_calq_id   = 0;
   g_calq_t    = 0;
   /* NO QUEUE, NO IDENTITY. A reply for the entry that has just gone must not
    * find a generation to match against -- and 0 is never handed out, so a
    * cleared queue matches nothing at all. */
   g_calq_gen = 0;
   /* NO QUEUE, NO DEADLINES. Leaving g_cal_resend_at standing would throttle
    * the FIRST attempt of whatever the user queues next, for up to a minute,
    * against a send that belonged to a calibration that is already over. */
   g_cal_giveup_at = 0;
   g_cal_resend_at = 0;
   g_cal_probe_at  = 0;
   return calq_save(keep ? 0 : &undo);
}

struct resc_undo {
   /* `expire_at` for the same reason the queue's deadlines are in its undo: a
    * pending target restored without its deadline is a target that never
    * expires, and this module's rule is that a stale reference expires
    * VISIBLY. */
   long t, pt, expire_at;
   int id, pm, pid, pmgdl, reject, expired, expired_id;
};

static void resc_snapshot(struct resc_undo *u)
{
   u->id         = g_rescale_id;
   u->pm         = g_rescale_pm;
   u->t          = g_rescale_t;
   u->pid        = g_rescale_pend_id;
   u->pmgdl      = g_rescale_pend_mgdl;
   u->pt         = g_rescale_pend_t;
   u->expire_at  = g_cal_pend_expire_at;
   u->reject     = g_rescale_reject;
   u->expired    = g_rescale_expired;
   u->expired_id = g_rescale_expired_id;
}

static void resc_restore(const struct resc_undo *u)
{
   g_rescale_id         = u->id;
   g_rescale_pm         = u->pm;
   g_rescale_t          = u->t;
   g_rescale_pend_id    = u->pid;
   g_rescale_pend_mgdl  = u->pmgdl;
   g_rescale_pend_t     = u->pt;
   g_cal_pend_expire_at = u->expire_at;
   g_rescale_reject     = u->reject;
   g_rescale_expired    = u->expired;
   g_rescale_expired_id = u->expired_id;
}

/* ---- A PERSISTED WALL-CLOCK STAMP, TURNED INTO AN IN-PROCESS DEADLINE ---
 *
 * The ONE place either loader crosses from realtime to monotonic, and the one
 * place a clock correction can still reach this module. How much of `window`
 * is left for something stamped at `stamp`, or NO_WINDOW_LEFT when it cannot
 * be honoured at all. CLOCK_SKEW_TOL_S in calib.h has the full argument for
 * each branch; in short:
 *
 *   age > window        the ordinary lapse, and the forward-skew answer too:
 *                       a clock jumped forward across a restart cannot be told
 *                       from a phone that was genuinely off that long, and
 *                       both mean "enter it again".
 *   age within [0,window]  resume with what is left -- NOT with a fresh full
 *                       window, which would let a restart loop extend a stale
 *                       fingerstick indefinitely.
 *   -tolerance <= age < 0  a routine backward nudge. Treat as this instant and
 *                       grant the full window rather than discard the user's
 *                       work over a two-second correction.
 *   age < -tolerance    the clock really moved; the age is unknowable, so the
 *                       reference expires VISIBLY rather than being applied. */
#define NO_WINDOW_LEFT (-1L)

static long window_left(long stamp, long window)
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

/* CALLER HOLDS calfile_lk AND cal_lk, and gets both back; cal_lk is dropped
 * across the write. See calq_save. */
static int rescale_save(const struct resc_undo *undo)
{
   char b[96];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%d,%ld\n", g_rescale_id,
                    g_rescale_pm, g_rescale_t, g_rescale_pend_id,
                    g_rescale_pend_mgdl, g_rescale_pend_t);
   mutex_unlock(&cal_lk);
   int wrote = write_line(g_rescale_path, b, clampn(n, sizeof b));
   mutex_lock(&cal_lk);
   if (wrote == 0) {
      g_resc_unsaved = 0;
      return CALIB_OK;
   }
   if (undo) {
      resc_restore(undo);
      return CALIB_UNSAVED;
   }
   g_resc_unsaved = 1;
   return CALIB_UNSAVED;
}

/* ---- the rescale factor ----------------------------------------------- */

/* raw * factor, rounded. */
static int rescale_apply(int raw, int pm)
{
   return (int)((((long)raw * pm) + 500) / 1000);
}

/* The factor to apply to a reading (src, timestamp t), or 1000 (none). */
static int rescale_pm_for(int src, long t)
{
   if (g_rescale_pm != 1000 && g_rescale_id == src && t >= g_rescale_t)
      return g_rescale_pm;
   return 1000;
}

/* Turn a (target, raw) pair into an active factor for sensor `id`, effective
 * from `t`. A factor beyond +-25% is REJECTED (not clamped) -- the reading is
 * too far from the entered value to be a plausible correction -- and flagged
 * for the RESCALE line. Returns 1 if it activated, 0 if rejected or
 * uncomputable. */
static int rescale_activate(int id, int target_mgdl, int raw, long t)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   int pm = calib_rescale_preview(target_mgdl, raw);
   if (pm < RESCALE_MIN_PM || pm > RESCALE_MAX_PM) {
      g_rescale_reject    = 1;
      g_rescale_reject_id = id;
      LOGI("rescale REJECTED: %d mg/dL over raw %d -> %d permille exceeds "
           "+-25%%",
           target_mgdl, raw, pm);
      return 0;
   }
   g_rescale_pm     = pm;
   g_rescale_id     = id;
   g_rescale_t      = t;
   g_rescale_reject = 0; /* a valid one clears any prior rejection */
   LOGI("rescale active: %d mg/dL over raw %d -> %d permille (id %d)",
        target_mgdl, raw, pm, id);
   return 1;
}

/* Forget a pending target, whatever became of it -- including its deadline,
 * which would otherwise expire the NEXT target early (or, once the tick had
 * fired on it, raise an expiry notice for a target that is no longer there). */
static void rescale_pend_clear(void)
{
   g_rescale_pend_mgdl  = 0;
   g_rescale_pend_id    = 0;
   g_rescale_pend_t     = 0;
   g_cal_pend_expire_at = 0;
}

int calib_rescale_preview(int target_mgdl, int raw)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   return (int)((((long)target_mgdl * 1000) + (raw / 2)) / raw);
}

int calib_rescale_pm(void)
{
   mutex_lock(&cal_lk);
   int pm = g_rescale_pm;
   mutex_unlock(&cal_lk);
   return pm;
}

int calib_raw_on_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   mutex_lock(&cal_lk);
   int raw = g_link_raw[link];
   mutex_unlock(&cal_lk);
   return raw;
}

int calib_rescale_engaged(int sensor_id)
{
   mutex_lock(&cal_lk);
   int on = (g_rescale_pm != 1000 && g_rescale_id == sensor_id) ||
            (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id);
   mutex_unlock(&cal_lk);
   return on;
}

int calib_rescale_set(int sensor_id, int raw, int target_mgdl)
{
   if (target_mgdl <= 0)
      return CALIB_UNSAVED;
   /* THE WHOLE TRANSITION under the locks, and it is UNDONE if the write
    * fails: this is the user's own change, so "nothing happened" is the
    * honest answer and the value can simply be entered again.
    *
    * A frame CAN see the factor during the write -- rescale_save releases
    * cal_lk across the fsyncs and the rename, because holding it there is
    * the ANR shape (see calfile_lk). So the rollback is not invisible; it is
    * a factor shown for the length of one disk write and then withdrawn,
    * which is the price of not spinning the looper. calfile_lk is held
    * throughout and is what keeps this the only WRITER during that window,
    * so the state the rollback restores is still the state it snapshotted. */
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   struct resc_undo undo;
   resc_snapshot(&undo);
   /* A fresh attempt supersedes any prior rejection / expiry notice. */
   g_rescale_reject  = 0;
   g_rescale_expired = 0;
   if (raw > 0) {
      rescale_activate(sensor_id, target_mgdl, raw, realtime_s());
      rescale_pend_clear();
   } else {
      /* No live reading yet: HOLD the target rather than lose it. The next
       * reading for this sensor computes the factor. */
      g_rescale_pend_mgdl = target_mgdl;
      g_rescale_pend_id   = sensor_id;
      /* TWO STAMPS FOR TWO JOBS. The realtime one is PERSISTED and exists so
       * the next launch can work out how much of the window a target set
       * before a restart has left. The monotonic one decides the expiry while
       * this process runs, and cannot be moved by a clock correction. */
      g_rescale_pend_t     = realtime_s();
      g_cal_pend_expire_at = mono_s() + RESCALE_PEND_WINDOW_S;
      LOGI("rescale %d mg/dL queued: awaiting a reading to compute factor",
           target_mgdl);
   }
   int rc = rescale_save(&undo);
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   return rc;
}

int calib_rescale_stop(void)
{
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   struct resc_undo undo;
   resc_snapshot(&undo);
   LOGI("rescaling turned OFF by user (was %d permille, pend %d)", g_rescale_pm,
        g_rescale_pend_mgdl);
   g_rescale_pm = 1000;
   g_rescale_id = 0;
   g_rescale_t  = 0;
   rescale_pend_clear();
   g_rescale_reject  = 0; /* and clear any rejection / expiry notice */
   g_rescale_expired = 0;
   int rc            = rescale_save(&undo);
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   return rc;
}

/* ---- the reading path -------------------------------------------------- */

int calib_on_reading(int sensor_id, int link, long t, int raw_mgdl,
                     int *applied_pm, int *started)
{
   /* calfile_lk covers the whole call because an activation or an expiry may
    * write; the common case (nothing pending) takes an uncontended mutex and
    * writes nothing. */
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   if (link >= 0 && link < LINK_MAX)
      g_link_raw[link] = raw_mgdl;
   if (started)
      *started = 0;

   /* A rescale target was waiting for a reading to compute its factor: this
    * is that reading -- BUT only if the user has been waiting less than
    * RESCALE_PEND_WINDOW_S for it. Past that the fingerstick reference is
    * stale, so EXPIRE it (visibly) rather than applying it to a much-later
    * reading. Otherwise activate from THIS raw, effective from THIS timestamp,
    * so the reference reading itself shows the entered value.
    *
    * THE WAIT IS MEASURED MONOTONICALLY, and the difference is not cosmetic.
    * This used to be `t - g_rescale_pend_t`: the READING's timestamp minus the
    * request's wall clock. Those are two instants from two different sources
    * -- one the app stamped, one that arrives with the sample -- so their
    * difference is not an elapsed time at all. A wall-clock correction between
    * the request and the reading moves it by the whole correction: forward,
    * and the first reading to arrive instantly EXPIRES a target the user set
    * moments ago, so the number they entered is thrown away and the row tells
    * them the reference went stale; backward, and it goes negative, so no
    * reading ever expires it and a fingerstick from an hour ago is applied to
    * the sample that finally shows up.
    *
    * What the window is actually about is how long the user has been waiting
    * since they typed the number, and that is an interval. g_rescale_pend_t
    * stays -- it is persisted, and the next launch needs it -- but nothing
    * running compares against it. */
   if (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id) {
      if (g_cal_pend_expire_at > 0 && mono_s() > g_cal_pend_expire_at) {
         LOGI("pending rescale %d mg/dL EXPIRED (%ld s waiting for a reading)",
              g_rescale_pend_mgdl, RESCALE_PEND_WINDOW_S);
         g_rescale_expired    = 1;
         g_rescale_expired_id = sensor_id;
      } else {
         rescale_activate(sensor_id, g_rescale_pend_mgdl, raw_mgdl, t);
         if (started)
            *started = 1;
      }
      rescale_pend_clear();
      /* NO ROLLBACK HERE, and the reason is worth stating: this runs on the
       * READING path, where the factor has already been computed from a
       * sample that will not come again. Undoing it would leave a pending
       * target that the next reading re-activates from a different raw --
       * a different factor, silently. The file is stale until the next
       * write succeeds; the state is the one the reading justified. */
      (void)rescale_save(0);
   }

   int pm = rescale_pm_for(sensor_id, t);
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(raw_mgdl, pm) : raw_mgdl;
}

int calib_on_backfill(int sensor_id, long t, int mg_dl, int *applied_pm)
{
   mutex_lock(&cal_lk);
   int pm = rescale_pm_for(sensor_id, t);
   mutex_unlock(&cal_lk);
   if (applied_pm)
      *applied_pm = pm;
   return pm != 1000 ? rescale_apply(mg_dl, pm) : mg_dl;
}

/* ---- the calibration queue --------------------------------------------- */

/* ---- THE QUEUE'S OWN HALF, called by the driver with its state held.
 *
 * Every one of these reads or writes state that a GATT callback also touches
 * (the reading path calls the attempt from a binder thread), so all of them
 * must be serialised with the driver. This file used to do that by taking the
 * driver's lock itself; now it says WHAT to do and the driver decides when it
 * is safe -- see driver_set_cal_ops. */
static int calq_queued_for_locked(int sensor_id)
{
   mutex_lock(&cal_lk);
   int q = (g_calq_mgdl > 0 && g_calq_id == sensor_id) ? g_calq_mgdl : 0;
   mutex_unlock(&cal_lk);
   return q;
}

static int calq_queue_locked(int sensor_id, int mgdl)
{
   if (mgdl <= 0)
      return CALIB_UNSAVED;
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   struct calq_undo undo;
   calq_snapshot(&undo);
   g_calq_mgdl = mgdl;
   g_calq_id   = sensor_id;
   /* THE INSTANT (persisted, for the next launch's reconciliation) and THE
    * DEADLINE (this process's own, and the only thing the tick compares
    * against). Both throttles start clear so the very first attempt goes out
    * at once -- a calibration the user just confirmed must not wait a minute
    * for a send that belonged to a previous one. */
   g_calq_t = realtime_s();
   /* A NEW ENTRY IS A NEW CALIBRATION, even when the value and the sensor are
    * the same as the one it replaces. Any reply still owed for the previous
    * entry now matches nothing and is discarded, which is exactly the point:
    * the sensor's answer to what the user typed BEFORE they changed their mind
    * must not be recorded against what they typed after. */
   g_calq_gen      = ++g_calq_gen_next;
   g_cal_giveup_at = mono_s() + CALQ_WINDOW_S;
   g_cal_resend_at = 0;
   g_cal_probe_at  = 0;
   int rc          = calq_save(&undo);
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   if (rc == CALIB_OK)
      LOGI("calibration QUEUED: %d mg/dL (id %d)", mgdl, sensor_id);
   else
      LOGW("calibration %d mg/dL NOT queued: it did not reach the disk", mgdl);
   return rc;
}

static int calq_cancel_locked(void)
{
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   LOGI("queued calibration %d mg/dL cancelled by user", g_calq_mgdl);
   int rc = calq_clear(0);
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   return rc;
}

/* ---- what the rest of the app calls: one hop through the driver ---- */

int calib_queued_for(int sensor_id)
{
   return driver_cal_queued_for(sensor_id);
}

int calib_queue(int sensor_id, int mgdl)
{
   return driver_cal_queue(sensor_id, mgdl);
}

int calib_cancel(void)
{
   return driver_cal_cancel();
}

void calib_try(int link, int sensor_id)
{
   driver_cal_attempt(link, sensor_id);
}

void calib_tick(void)
{
   driver_cal_tick();
}

/* Record how a queued calibration ended, and stop queueing it. CALIB_OK when
 * the verdict reached the disk.
 *
 * CALLER HOLDS cal_lk. The record and the clear are one step: they are one
 * line in one file, and a reader between them sees a queue that is gone with
 * a verdict that has not arrived.
 *
 * IT IS NOT ROLLED BACK. The sensor has answered, and there is no way to ask
 * it again; putting the queue back would send the same value to a sensor that
 * has already accepted or rejected it. The verdict stands and the file is
 * marked behind -- the tick retries it, and the row says so meanwhile. */
static int calq_resolve(int state)
{
   g_lastcal_mgdl  = g_calq_mgdl;
   g_lastcal_t     = realtime_s();
   g_lastcal_state = state;
   g_lastcal_id    = g_calq_id;
   return calq_clear(1);
}

static void calq_attempt_locked(int link, int sensor_id)
{
   /* THE QUEUE IS READ AS A COPY, AND THE LOCK IS RELEASED BEFORE ANY DRIVER
    * CALL. The order is driver_lk -> cal_lk (this function already runs under
    * the driver's), so holding cal_lk across driver_cal_of, driver_cal_bounds
    * or driver_calibrate would be the one place in the app that takes them
    * the other way round -- see app/test/lockorder.py, which checks. */
   mutex_lock(&cal_lk);
   int q_mgdl       = g_calq_mgdl;
   int q_id         = g_calq_id;
   unsigned q_gen   = g_calq_gen;
   long q_resend_at = g_cal_resend_at;
   long q_probe_at  = g_cal_probe_at;
   mutex_unlock(&cal_lk);
   if (q_mgdl <= 0 || q_id != sensor_id) {
      return;
   }
   struct dex_cal c;
   driver_cal_of(link, &c);
   /* The sensor answered and does NOT permit calibration at all (a factory-
    * calibrated Stelo, say). Distinct from a value the sensor REJECTS: this
    * is "the device does not support calibration", so say NOT SUPPORTED.
    * Fail VISIBLY at once rather than leaving it PENDING until the window
    * lapses. This only drops the queued value -- it does NOT lock calibration
    * out: a later user-initiated calibration re-queues and re-probes
    * permission afresh. */
   if (c.have && !c.permitted) {
      LOGI("calibration not permitted by this sensor; queued %d mg/dL not sent",
           q_mgdl);
      mutex_lock(&calfile_lk);
      mutex_lock(&cal_lk);
      (void)calq_resolve(CAL_ST_NOTSUP); /* stands; the tick retries */
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      shell_ui_dirty();
      return;
   }
   /* Permission not yet known: PROBE it (0x32). driver_calibrate refuses
    * without a positive answer, and nothing else sends this probe during
    * streaming, so the calibration could otherwise never proceed. The write
    * itself goes on the next stream once the reply sets cal.permitted.
    *
    * BE GENTLE with a sensor that may not want calibrations: throttle the
    * probe to at most once a minute, so a Stelo that never answers is nudged
    * only a handful of times before the window lapses and it FAILS -- never
    * hammered. A sensor that answers "no" is caught by the fast-fail above and
    * never probed again.
    *
    * THE MINUTE IS ELAPSED TIME. It used to be `realtime_s() - c.asked >= 60`
    * against the driver's own wall-clock stamp, which meant a clock correction
    * decided how gentle this was: forward, and the difference exceeds 60 on
    * every reading, so a sensor that is already declining to answer gets a
    * probe every five minutes' worth of samples in a burst; backward, and the
    * difference is negative for the length of the correction, so the probe is
    * never repeated and a calibration the user confirmed simply never
    * proceeds. The deadline is this module's own now (see the deadline block),
    * and the driver's cal.asked is left as its own record of when it last put
    * the request on the wire. */
   if (!c.have) {
      if (mono_s() >= q_probe_at) {
         driver_cal_bounds(link);
         mutex_lock(&cal_lk);
         /* ...against the queue we read, for the same reason the send below
          * checks: a probe throttle stamped over a queue the user has replaced
          * would silence the new one's first probe for a minute. The
          * generation is part of "the queue we read" -- re-queueing the SAME
          * value for the SAME sensor is a new entry and is entitled to its own
          * first probe. */
         if (g_calq_mgdl == q_mgdl && g_calq_id == q_id && g_calq_gen == q_gen)
            g_cal_probe_at = mono_s() + 60;
         mutex_unlock(&cal_lk);
         LOGI("calibration queued: probing 0x32 permission before writing");
      }
      return;
   }
   /* Permitted: send the calibration, but only if we are not already awaiting
    * a reply from a recent send. One 0x34 per minute at most -- gentle, and
    * the sensor's reply normally resolves it on the first try.
    *
    * ELAPSED TIME AGAIN, and this is the throttle whose backward-jump failure
    * is the quietest of the lot. The gate this replaces subtracted a
    * wall-clock stamp of the last send from a fresh `realtime_s()` and refused
    * while the difference was under sixty; a backward correction makes that
    * difference negative, so it stays under sixty for the whole length of the
    * correction and the one retry that would have got the value into the
    * sensor is never made. The row goes on saying PENDING until the window
    * lapses and then says FAILED, and nothing was ever written. */
   if (mono_s() < q_resend_at) {
      return;
   }
   /* THE WRITE CARRIES ITS OWN IDENTITY. The queue was read as a copy above
    * and the lock released, so by the time this returns the user may already
    * have replaced it -- which is precisely the interleaving that used to end
    * with the replacement being marked accepted. The token handed over here is
    * the one the sensor's answer will come back with, and pancra_cal_result
    * matches on all three fields. */
   if (driver_calibrate(link, q_mgdl, q_id, q_gen)) {
      mutex_lock(&cal_lk);
      /* ...and only if the queue is still the one we read: the write went
       * out for THAT value, and stamping the send against a queue the user
       * has replaced in the meantime would suppress the new one's first
       * attempt for a minute. */
      if (g_calq_mgdl == q_mgdl && g_calq_id == q_id && g_calq_gen == q_gen)
         g_cal_resend_at = mono_s() + 60;
      mutex_unlock(&cal_lk);
      LOGI("calibration %d mg/dL submitted from queue, awaiting sensor reply",
           q_mgdl);
   }
}

/* Driver callback: the sensor answered a calibration we sent. */
void pancra_cal_result(int result, int sensor_id, int mg_dl, unsigned gen)
{
   /* THE WHOLE ANSWER UNDER ONE LOCK: the test for a live queue, the record
    * of how it ended, and the clear are one transition, and a reader between
    * any two of them sees a state that never existed. Called with the
    * driver's lock held, so this is driver_lk -> cal_lk as documented. */
   mutex_lock(&calfile_lk);
   mutex_lock(&cal_lk);
   /* ---- ONLY AN EXACT LIVE-QUEUE MATCH RESOLVES ANYTHING ----------------
    *
    * All four conditions, and all four for the same reason: this answer
    * belongs to ONE write, and the only entry it may resolve is the one that
    * write came from.
    *
    *   nothing queued        an unsolicited reply, or one for an entry that
    *                         has already been resolved (accepted, rejected,
    *                         cancelled, or timed out). There is nothing left
    *                         to say about it.
    *   a different sensor    the user queued a calibration for another sensor
    *                         while this one's write was outstanding. Applying
    *                         it would record one sensor's verdict against
    *                         another's fingerstick.
    *   a different value     / a different generation
    *                         the user replaced the value -- with a new one, or
    *                         with the SAME one re-entered -- before the sensor
    *                         answered. THIS is the case the whole token
    *                         exists for: the old code resolved whatever was
    *                         queued, so the replacement was marked APPLIED
    *                         having never been written, the row told the user
    *                         the sensor holds it, and the sensor went on
    *                         reporting against the value they discarded.
    *
    * DISCARDED LOUDLY, not silently. Every one of these is either a peer
    * saying something it should not or a real answer arriving after the
    * question changed, and both are worth a line in the log -- a silent drop
    * here is indistinguishable from the misresolution it replaces.
    *
    * The queued value is NOT lost by a discard: it was never sent, its resend
    * throttle was never stamped against it (calq_attempt_locked stamps only on
    * an exact match), and the next stream attempt writes it. */
   if (g_calq_mgdl <= 0 || g_calq_id != sensor_id || g_calq_mgdl != mg_dl ||
       g_calq_gen != gen) {
      LOGW("calibration reply DISCARDED: it answers %d mg/dL for sensor %d "
           "(gen %u), and the live queue is %d mg/dL for sensor %d (gen %u)",
           mg_dl, sensor_id, gen, g_calq_mgdl, g_calq_id, g_calq_gen);
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      return;
   }
   if (result == 0) {
      LOGI("calibration %d mg/dL ACCEPTED by the sensor", g_calq_mgdl);
      (void)calq_resolve(CAL_ST_APPLIED); /* stands; the tick retries */
   } else {
      /* The sensor actively rejected the value -- resending it will not help,
       * so surface it (LAST CAL shows REJECTED) rather than looping or
       * dropping it silently. No beep: the official app is silent on a
       * rejection too. */
      LOGI("calibration %d mg/dL REJECTED by the sensor (result=0x%02x)",
           g_calq_mgdl, result);
      (void)calq_resolve(CAL_ST_REJECTED); /* stands; the tick retries */
   }
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   shell_ui_dirty();
}

/* ---- lifecycle --------------------------------------------------------- */

int calib_paths(const char *dir)
{
   /* THROUGH data_path, like every other persistence owner.
    *
    * This built its two paths with snprintf and discarded the result -- a
    * second, unchecked convention beside the shared one. snprintf truncates
    * rather than failing, so an over-long data directory produced a
    * well-formed path to somewhere else, and the calibration queue and the
    * rescale factor were read from and written to a location nothing else in
    * the app agreed on. */
   int ok = 1;
   ok &= data_path(g_calq_path, sizeof g_calq_path, dir, "/cal.q");
   ok &= data_path(g_rescale_path, sizeof g_rescale_path, dir, "/rescale.cfg");
   return ok;
}

/* Loading DEFINES the state, it does not merely add to it: what the file says
 * is the whole truth afterwards, including "nothing is queued". A load that
 * only ever filled things in would leave a caller's earlier queue standing
 * behind a file that had already resolved it. */
static int calq_load(void)
{
   /* THE FILE IS ONE RECORD, AND IT IS INSTALLED AS ONE.
    *
    * THE READ IS UNDER calfile_lk, not outside every lock. It is I/O, so it
    * must not be under cal_lk -- but a read taken outside BOTH could be
    * overtaken by a save and then installed on top of it, putting memory back
    * to a state older than the file it had just written, with nothing
    * inconsistent about either half. calfile_lk is exactly the lock for
    * "this process is deciding what the file says"; no frame ever takes it.
    *
    * Everything the file describes -- the queue and the last-resolved
    * verdict, which share one line -- is then installed under cal_lk in one
    * go. In three short sections a render could see a cleared queue beside
    * the OLD verdict, or the new verdict beside a queue from before the
    * restore. */
   mutex_lock(&calfile_lk);
   char b[64];
   int rd    = read_line(g_calq_path, b, sizeof b);
   long v[7] = {0, 0, 0, 0, 0, 0, 0};
   if (rd == READ_OK)
      parse_ints(b, v, 7);

   /* A FILE THAT COULD NOT BE READ DEFINES NOTHING, so it must not be the
    * reason the state is wiped. This used to clear all eight fields first
    * and only then look at `rd`, so an unreadable cal.q destroyed a live
    * queue and a live verdict -- exactly the "an unreadable file is not a
    * fresh install" case this loader's three-way answer exists for, applied
    * backwards. READ_NONE (no file at all) does define the state: there is
    * nothing queued, which is the truth on a first run. */
   mutex_lock(&cal_lk);
   if (rd == READ_FAIL) {
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      return CALIB_UNSAVED;
   }
   /* THE FILE IS NOW WHAT MEMORY SAYS, by definition: a load DEFINES the
    * state. Anything the previous state had failed to write is gone, not
    * pending -- retrying it would write back a record this file has just
    * replaced. */
   g_calq_unsaved = 0;
   g_calq_mgdl = g_calq_id = 0;
   g_calq_t                = 0;
   g_calq_gen              = 0; /* a load DEFINES the state; see below */
   /* The deadlines belong to whatever was queued a moment ago, and a load
    * defines the state: whatever the file says replaces it entirely. */
   g_cal_giveup_at = g_cal_resend_at = g_cal_probe_at = 0;
   g_lastcal_mgdl = g_lastcal_state = g_lastcal_id = 0;
   g_lastcal_t                                     = 0;
   if (rd != READ_OK) {
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      return CALIB_OK; /* READ_NONE: a first run, and that IS the state */
   }
   /* the last-resolved record (fields 4..7) survives regardless of the
    * queue. */
   g_lastcal_mgdl  = (int)v[3];
   g_lastcal_t     = v[4];
   g_lastcal_state = (int)v[5];
   g_lastcal_id    = (int)v[6];
   if (v[1] > 0) {
      g_calq_id   = (int)v[0];
      g_calq_mgdl = (int)v[1];
      g_calq_t    = v[2];
      /* A FRESH IDENTITY, AND DELIBERATELY NOT PERSISTED. The generation says
       * which in-process queue entry a write in flight belongs to, and no
       * write can be in flight across a load: at startup the driver has no
       * context at all, and a reload during a run replaces the queue, which is
       * exactly the case where an outstanding reply must NOT be applied to
       * what came off the disk. Writing the number to cal.q would also make a
       * restored entry collide with one the running process had already handed
       * out. So the entry is real, and it is a NEW one. */
      g_calq_gen = ++g_calq_gen_next;
      /* RESTART RECONCILIATION, which is the whole reason g_calq_t is a
       * wall-clock number and is written to the file. A monotonic deadline
       * cannot cross a reboot -- it counts from an arbitrary origin and starts
       * again with the kernel -- so this is the one subtraction that has to be
       * done on the wall clock, once, here.
       *
       * A calibration confirmed before a restart keeps retrying if it is still
       * fresh, and records the failure if it is not -- never a silent drop.
       * What is left of the window becomes THIS process's deadline, and
       * nothing after this point consults g_calq_t again, so a clock
       * correction a minute from now cannot expire or postpone it.
       *
       * REMAINING, NOT A FRESH WINDOW: a phone that relaunches the app every
       * few minutes would otherwise keep a twenty-minute-old fingerstick
       * queued for ever. window_left also decides what a NEGATIVE age means --
       * see it and CLOCK_SKEW_TOL_S in calib.h. */
      long left = window_left(g_calq_t, CALQ_WINDOW_S);
      if (left == NO_WINDOW_LEFT) {
         (void)calq_resolve(CAL_ST_FAILED); /* stands; the tick retries */
      } else {
         g_cal_giveup_at = mono_s() + left;
         g_cal_resend_at = 0; /* a restart may attempt at once */
         g_cal_probe_at  = 0;
      }
   }
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   return CALIB_OK;
}

/* Under the lock like every other writer of this state. calib_load has ONE
 * caller today (main.c, at startup), so the concurrent load is not a race
 * that can currently happen -- the lock is here because a loader that
 * installs a whole record must not be the one path that assumes it is alone,
 * and because a restore that re-reads while live is a change of one call
 * site. */
static int rescale_load(void)
{
   mutex_lock(&calfile_lk); /* see calq_load: the read and the install are
                             * one decision about what the file says */
   char b[96];
   int rd = read_line(g_rescale_path, b, sizeof b);
   mutex_lock(&cal_lk);
   /* See calq_load: a file that could not be READ does not get to wipe a
    * factor that is scaling every reading from a sensor. */
   if (rd == READ_FAIL) {
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      return CALIB_UNSAVED;
   }
   g_resc_unsaved = 0; /* see calq_load: a load defines the state */
   g_rescale_pm   = 1000;
   g_rescale_id   = 0;
   g_rescale_t    = 0;
   rescale_pend_clear();
   /* The notices describe attempts made in this session; a load starts with
    * none, except one this load itself raises just below. */
   g_rescale_reject  = 0;
   g_rescale_expired = 0;
   if (rd != READ_OK) {
      mutex_unlock(&cal_lk);
      mutex_unlock(&calfile_lk);
      return CALIB_OK; /* READ_NONE: a first run */
   }
   long v[6] = {0, 0, 0, 0, 0, 0};
   parse_ints(b, v, 6);
   if (v[1] >= RESCALE_MIN_PM && v[1] <= RESCALE_MAX_PM && v[1] != 1000) {
      g_rescale_id = (int)v[0];
      g_rescale_pm = (int)v[1];
      g_rescale_t  = v[2];
   }
   if (v[4] > 0) { /* a target was awaiting a reading when we last ran */
      long pend_t = v[5];
      /* Restart must NOT lose a pending rescale -- BUT only honour it if its
       * request is still within the freshness window; a target set before a
       * long downtime has a stale fingerstick reference and must not silently
       * apply to a much-later reading.
       *
       * The queue's reconciliation, with the same reasoning: see calq_load.
       * The persisted stamp is wall-clock because nothing else survives a
       * reboot; what is LEFT of the window becomes this process's monotonic
       * deadline, and nothing running compares against pend_t again. */
      long left = pend_t > 0 ? window_left(pend_t, RESCALE_PEND_WINDOW_S)
                             : NO_WINDOW_LEFT;
      if (left != NO_WINDOW_LEFT) {
         g_rescale_pend_id    = (int)v[3];
         g_rescale_pend_mgdl  = (int)v[4];
         g_rescale_pend_t     = pend_t;
         g_cal_pend_expire_at = mono_s() + left;
      } else {
         g_rescale_expired    = 1; /* surface it, never a silent drop */
         g_rescale_expired_id = (int)v[3];
      }
   }
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   return CALIB_OK;
}

int calib_load(void)
{
   /* BOTH, always -- one unreadable file must not stop the other from being
    * restored. The caller is told if either was lost. */
   int q = calq_load();
   int r = rescale_load();
   return (q == CALIB_OK && r == CALIB_OK) ? CALIB_OK : CALIB_UNSAVED;
}

static void calq_tick_locked(void)
{
   /* MONOTONIC, because everything this function decides is "has enough time
    * passed?" -- and this is the function a wall-clock jump used to break most
    * visibly. It read realtime_s() once and compared it against three
    * persisted stamps: a correction forward of more than CALQ_WINDOW_S made
    * the very next tick declare a calibration the user had just confirmed
    * FAILED, and a correction backward made every one of the three comparisons
    * go negative, so nothing ever expired and nothing was ever retried. See
    * the deadline block near the top of this file. */
   long now = mono_s();
   /* calfile_lk over the whole tick: the give-up resolve, the expiry and the
    * two retries all write, and this is the thread that most often does.
    * cal_lk is taken and dropped inside, and every save drops it again for
    * the syscalls -- so the MAIN thread's per-frame calib_view never waits on
    * a disk. */
   mutex_lock(&calfile_lk);
   /* The queue's own half, under the lock that owns it now. */
   mutex_lock(&cal_lk);
   int gave_up = 0;
   if (g_calq_mgdl > 0) {
      /* (The "no reply in a minute: allow another attempt" clause that stood
       * here is gone, not moved. It cleared the last-send stamp so the attempt
       * path would stop refusing; a deadline lapses by itself, so the attempt
       * path's own `mono_s() < g_cal_resend_at` is the whole rule now. Two
       * places deciding one thing is how they came to disagree under a clock
       * jump in the first place.) */
      if (g_cal_giveup_at > 0 && now > g_cal_giveup_at) {
         LOGI("calibration %d mg/dL never accepted within %ld s; giving up "
              "VISIBLY",
              g_calq_mgdl, CALQ_WINDOW_S);
         /* No beep -- LAST CAL shows FAILED; the official app is silent
          * too. */
         /* The result is not checked HERE: calq_resolve records a failed
          * write in g_calq_unsaved and the retry below picks it up. The
          * verdict itself stands either way -- the window has passed. */
         (void)calq_resolve(CAL_ST_FAILED);
         gave_up = 1;
      }
   }
   mutex_unlock(&cal_lk);
   if (gave_up)
      shell_ui_dirty();
   /* A pending rescale target expires on the clock too, not only when a
    * reading finally arrives -- otherwise a sensor that goes quiet leaves the
    * target looking live indefinitely. The rescale half is cal_lk's, even on
    * a path that already holds the driver's: this tick runs on the main
    * looper or the service heartbeat, and the reading path on a binder
    * thread. */
   mutex_lock(&cal_lk);
   int expired = 0;
   if (g_rescale_pend_mgdl > 0 && g_cal_pend_expire_at > 0 &&
       now > g_cal_pend_expire_at) {
      LOGI("pending rescale %d mg/dL EXPIRED with no reading",
           g_rescale_pend_mgdl);
      g_rescale_expired    = 1;
      g_rescale_expired_id = g_rescale_pend_id;
      rescale_pend_clear();
      (void)rescale_save(0); /* an expiry that cannot be written re-expires */
      expired = 1;
   }
   /* THE RETRY. A transition that stood without reaching the disk is tried
    * again here, every tick, until it lands -- which is what makes "the state
    * is the one the sample justified" survive a restart rather than merely
    * outlive the screen. Cheap when there is nothing to do (two ints), and
    * the writes are the same replace-by-rename either save does. */
   if (g_resc_unsaved) {
      if (rescale_save(0) == CALIB_OK)
         LOGI("rescale state reached the disk on a retry");
   }
   mutex_unlock(&cal_lk);

   mutex_lock(&cal_lk);
   if (g_calq_unsaved) {
      if (calq_save(0) == CALIB_OK)
         LOGI("calibration state reached the disk on a retry");
   }
   mutex_unlock(&cal_lk);
   mutex_unlock(&calfile_lk);
   if (expired)
      shell_ui_dirty();
}

void calib_view(int sensor_id, struct calib_view *out)
{
   /* ONE LOCK, ONE ROW. The queued value and the last-resolved record are
    * ALTERNATIVES, not a pair: while one is in flight the row shows it, and
    * only once it resolves does LAST CAL take the row back. Read under two
    * different locks -- which is what "ask the driver, then take cal_lk"
    * was -- a resolution landing in between shows a live "PENDING 120"
    * beside the older "APPLIED 96" it has just replaced, or neither.
    *
    * The queue is cal_lk's now (it was the driver's), so both come out of
    * one critical section and the driver is not involved at all. */
   mutex_lock(&cal_lk);
   out->queued_mgdl =
       (g_calq_mgdl > 0 && g_calq_id == sensor_id) ? g_calq_mgdl : 0;
   int resolved =
       (!out->queued_mgdl && g_lastcal_t > 0 && g_lastcal_id == sensor_id);
   out->last_mgdl  = resolved ? g_lastcal_mgdl : 0;
   out->last_state = resolved ? g_lastcal_state : 0;
   out->last_t     = resolved ? g_lastcal_t : 0;

   out->rescale_pm = (g_rescale_pm != 1000 && g_rescale_id == sensor_id)
                         ? g_rescale_pm
                         : 1000;
   out->rescale_pending =
       (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == sensor_id)
           ? g_rescale_pend_mgdl
           : 0;
   out->rescale_rejected =
       (g_rescale_reject && g_rescale_reject_id == sensor_id);
   out->rescale_expired =
       (g_rescale_expired && g_rescale_expired_id == sensor_id);
   /* NOT PER-SENSOR, on purpose: a file that is behind is behind for every
    * row it describes, and there is exactly one of each file. */
   out->cal_unsaved     = g_calq_unsaved;
   out->rescale_unsaved = g_resc_unsaved;
   mutex_unlock(&cal_lk);
}

/* THE CALIBRATION MODULE'S HALF of the driver's serialisation, registered
 * once at startup: the driver takes its own lock and calls straight back in.
 * See dexdriver.h. */
static const struct driver_cal_ops g_cal_ops = {
    .attempt    = calq_attempt_locked,
    .queue      = calq_queue_locked,
    .cancel     = calq_cancel_locked,
    .tick       = calq_tick_locked,
    .queued_for = calq_queued_for_locked,
};

void calib_register_ops(void)
{
   driver_set_cal_ops(&g_cal_ops);
}
