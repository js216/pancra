// SPDX-License-Identifier: GPL-3.0
// calibq.c --- the calibration queue: a value the SENSOR has to accept
// Copyright 2026 Jakob Kastelic

/* ONE OF THE TWO CORRECTIONS (see calib.h); the other is app/rescale.c and
 * what they share is app/calibint.h.
 *
 * A calibration is a value the user confirms and the SENSOR must accept. It
 * is written to the sensor over GATT, and until the sensor answers it is not
 * finished -- so it is persisted and retried on every stream, and it carries
 * a verdict of its own (PENDING / APPLIED / REJECTED / FAILED / NOT
 * SUPPORTED) that the device row shows. Nothing about it is local arithmetic;
 * that is the rescale, next door.
 */
#include "calib.h"
#include "calibint.h"
#include "clock.h"
#include "dexdriver.h"
#include "log.h"
#include "shell.h"
#include "uimodel.h"
#include "util.h"
#include <stdio.h>

/* DURABLE calibration queue: a CONFIRMED calibration that has not yet been
 * ACCEPTED by the sensor. It is persisted and retried on every stream until
 * the sensor answers -- so a calibration is NEVER lost to a reconnect gap or
 * an app restart, the way a one-shot write silently was. */
/* cal_lk's, all four, and persisted as ONE LINE together with the LAST CAL
 * record below -- which is why they share a lock rather than each being
 * written under whichever one its writer happened to hold. */
/* ---- THE QUEUE, AS ONE RECORD -----------------------------
 *
 * These were eleven parallel file statics -- g_q.mgdl, g_q.id,
 * g_q.t, g_q.gen, three g_cal_*_at deadlines and four g_lastcal_* --
 * with a `struct calq_undo` beside them that listed the same eleven again,
 * and two functions that copied field by field in each direction. Nothing
 * connected the three lists: a twelfth piece of queue state added to the
 * globals and forgotten in the undo compiles perfectly and silently stops
 * rolling back, which is a calibration that survives a failed save in memory
 * while the file says it never happened.
 *
 * One record means the snapshot is `*u = g_q` and the restore is `g_q = *u`,
 * and a field added anywhere inside it is carried by both without anybody
 * remembering to say so. */
struct calq {
   int mgdl; /* queued value, mg/dL; 0 = none queued */
   int id;   /* sensor id it is for */
   /* realtime_s() when the user confirmed it. AN INSTANT, and PERSISTED: it is
    * the only thing the next launch can subtract its own wall clock from to
    * learn how much of the window is left. It is NOT what the running process
    * compares against -- see the deadline block below. */
   long t;
   /* ---- WHICH CALIBRATION THIS IS, so a reply cannot be applied to another
    *
    * g_q.gen names the queue entry that is live RIGHT NOW. It travels with the
    * write (driver_calibrate) and comes back with the sensor's answer
    * (calib_cal_result), which resolves the queue only when the sensor id, the
    * value AND this number all still match. Without it the driver had a single
    * boolean and the answer resolved whatever happened to be queued -- so a
    * user who replaced 100 with 180 before the sensor answered got 180 marked
    * ACCEPTED without one byte of 180 ever having been sent, and the sensor
    * went on reporting against 100 for the rest of the session.
    *
    * IT IS NOT A CLOCK, and it must never become one. The obvious alternative
    * -- stamp each write with realtime_s() and match on the stamp -- would make
    * the IDENTITY of a calibration depend on the wall clock, so an NTP step
    * between the write and the reply would either lose a genuine answer or
    * (with a backward step and a re-queue) let two writes share a stamp. A
    * counter cannot be corrected. It is deliberately NOT named g_cal_*_at: that
    * shape is reserved for the deadlines below, and `make clockcheck` matches
    * on it.
    *
    * g_calq_gen_next is the source of the numbers and only ever goes UP. The
    * live one is part of what a queue IS, so it is snapshotted and restored
    * with the rest of the queue (struct calq_undo) -- a rolled-back queue must
    * keep its identity, or a reply to the write that is genuinely still
    * outstanding would be discarded. The counter behind it is deliberately
    * outside the undo, so a rolled-back transaction never hands the same number
    * to a later, different queue.
    *
    * WRAPAROUND is not a concern worth code: it is unsigned (defined
    * behaviour), one entry is one deliberate user action, and reaching 2^32 of
    * them would take longer than the phone will exist. */
   unsigned gen; /* identity of the live queue; 0 = none */

   /* THE DEADLINES, WHICH ARE INTERVALS AND NOT INSTANTS -- see the block
    * further down for the whole argument. They are part of what a queue IS:
    * a value restored without its `giveup_at` comes back with no window at
    * all, and without `resend_at` it is written to the sensor again at once,
    * which is the hammering the throttle exists to prevent. */
   long giveup_at, resend_at, probe_at;

   /* Last RESOLVED calibration, for the per-device LAST CAL row (persisted).
    *
    * cal_lk's, like the queue above it and for the same reason: the two share
    * one line in one file, so a save must see both at one instant. Written by
    * calq_resolve and by the load, read as a four-field set by the renderer --
    * under one lock either way, so a row cannot show one calibration's value
    * beside another's verdict.
    *
    * (The write side needs the lock against ITSELF as well, not only against
    * readers: the tick and the load write these, and neither is
    * driver-serialised.) */
   struct {
      int mgdl;  /* value of the last resolved calibration */
      long t;    /* realtime_s() it resolved; 0 = never */
      int state; /* CAL_ST_* (ui.h) */
      int id;    /* sensor id it was for */
   } last;
};

static struct calq g_q;
/* OUTSIDE THE RECORD, on purpose: the source of the generation numbers only
 * ever goes up, so a rolled-back transaction must not hand the same number to
 * a later, different queue. */
static unsigned g_calq_gen_next;
static char g_calq_path[256]; /* persistence file */

static int g_calq_unsaved; /* cal.q is behind the queue/LAST CAL in memory */

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
/* THE UNDO IS THE RECORD ITSELF. Listing the same eleven fields a second
 * time, with two functions copying them across one by one, means a field
 * added to the queue and forgotten here compiles perfectly and silently
 * stops rolling back.
 *
 * WHAT IT CARRIES THAT THE FILE DOES NOT: the three deadlines and the
 * generation. Each is part of what a queue IS -- a value restored without
 * `giveup_at` comes back with no window at all, without `resend_at` it is
 * written to the sensor again immediately (the hammering the throttle exists
 * to prevent), and without `gen` it would refuse the sensor's answer to the
 * write that is genuinely still outstanding, sit PENDING until the window
 * lapsed, and report FAILED on a value the sensor had already accepted.
 *
 * (The COUNTER behind the generation, g_calq_gen_next, is deliberately NOT in
 * the record: a rolled-back transaction must never hand the same number to a
 * later, different queue.) */
typedef struct calq calq_undo;

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
 * reverse (see cal_q_attempt_locked, which does its driver work outside). */
static void calq_snapshot(calq_undo *u)
{
   *u = g_q;
}

static void calq_restore(const calq_undo *u)
{
   g_q = *u;
}

/* CALLER HOLDS calfile_lk AND cal_lk, and gets both back. cal_lk is DROPPED
 * across the write -- see the note above calfile_lk for why that is not
 * optional. */
static int calq_save(const calq_undo *undo)
{
   /* queued (id,mgdl,t) then last-resolved (mgdl,t,state,id) on one line,
    * rendered while the lock still makes them one instant. */
   char b[112];
   int n =
       snprintf(b, sizeof b, "%d,%d,%ld,%d,%ld,%d,%d\n", g_q.id, g_q.mgdl,
                g_q.t, g_q.last.mgdl, g_q.last.t, g_q.last.state, g_q.last.id);
   cal_unlock();
   int wrote = cal_write_line(g_calq_path, b, clampn(n, sizeof b));
   cal_lock();
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
   calq_undo undo;
   calq_snapshot(&undo);
   g_q.mgdl = 0;
   g_q.id   = 0;
   g_q.t    = 0;
   /* NO QUEUE, NO IDENTITY. A reply for the entry that has just gone must not
    * find a generation to match against -- and 0 is never handed out, so a
    * cleared queue matches nothing at all. */
   g_q.gen = 0;
   /* NO QUEUE, NO DEADLINES. Leaving g_q.resend_at standing would throttle
    * the next attempt of whatever the user queues, for up to a minute,
    * against a send that belonged to a calibration that is already over. */
   g_q.giveup_at = 0;
   g_q.resend_at = 0;
   g_q.probe_at  = 0;
   return calq_save(keep ? 0 : &undo);
}

/* ---- the calibration queue --------------------------------------------- */

/* ---- THE QUEUE'S OWN HALF, called by the driver with its state held.
 *
 * Every one of these reads or writes state that a GATT callback also touches
 * (the reading path calls the attempt from a binder thread), so all of them
 * must be serialised with the driver. This file says WHAT to do and the
 * driver decides when it is safe -- see driver_set_cal_ops -- rather than
 * reaching for the driver's lock itself. */
int cal_q_queued_for_locked(int sensor_id)
{
   cal_lock();
   int q = (g_q.mgdl > 0 && g_q.id == sensor_id) ? g_q.mgdl : 0;
   cal_unlock();
   return q;
}

int cal_q_queue_locked(int sensor_id, int mgdl)
{
   if (mgdl <= 0)
      return CALIB_UNSAVED;
   cal_file_lock();
   cal_lock();
   calq_undo undo;
   calq_snapshot(&undo);
   g_q.mgdl = mgdl;
   g_q.id   = sensor_id;
   /* THE INSTANT (persisted, for the next launch's reconciliation) and THE
    * DEADLINE (this process's own, and the only thing the tick compares
    * against). Both throttles start clear so the very first attempt goes out
    * at once -- a calibration the user just confirmed must not wait a minute
    * for a send that belonged to a previous one. */
   g_q.t = realtime_s();
   /* A NEW ENTRY IS A NEW CALIBRATION, even when the value and the sensor are
    * the same as the one it replaces. Any reply still owed for the previous
    * entry now matches nothing and is discarded, which is exactly the point:
    * the sensor's answer to what the user typed BEFORE they changed their mind
    * must not be recorded against what they typed after. */
   g_q.gen       = ++g_calq_gen_next;
   g_q.giveup_at = mono_s() + CALQ_WINDOW_S;
   g_q.resend_at = 0;
   g_q.probe_at  = 0;
   int rc        = calq_save(&undo);
   cal_unlock();
   cal_file_unlock();
   if (rc == CALIB_OK)
      LOGI("calibration QUEUED: %d mg/dL (id %d)", mgdl, sensor_id);
   else
      LOGW("calibration %d mg/dL NOT queued: it did not reach the disk", mgdl);
   return rc;
}

int cal_q_cancel_locked(void)
{
   cal_file_lock();
   cal_lock();
   LOGI("queued calibration %d mg/dL cancelled by user", g_q.mgdl);
   int rc = calq_clear(0);
   cal_unlock();
   cal_file_unlock();
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
   g_q.last.mgdl  = g_q.mgdl;
   g_q.last.t     = realtime_s();
   g_q.last.state = state;
   g_q.last.id    = g_q.id;
   return calq_clear(1);
}

void cal_q_attempt_locked(int link, int sensor_id)
{
   /* THE QUEUE IS READ AS A COPY, AND THE LOCK IS RELEASED BEFORE ANY DRIVER
    * CALL. The order is driver_lk -> cal_lk (this function already runs under
    * the driver's), so holding cal_lk across driver_cal_of, driver_cal_bounds
    * or driver_calibrate would be the one place in the app that takes them
    * the other way round -- see test/app/lockorder.py, which checks. */
   cal_lock();
   int q_mgdl       = g_q.mgdl;
   int q_id         = g_q.id;
   unsigned q_gen   = g_q.gen;
   long q_resend_at = g_q.resend_at;
   long q_probe_at  = g_q.probe_at;
   cal_unlock();
   if (q_mgdl <= 0 || q_id != sensor_id) {
      return;
   }
   struct dex_cal c;
   /* A LINK THE DRIVER DOES NOT HAVE IS NOT A SENSOR THAT SAID NOTHING.
    * driver_cal_of zeroes `c` either way, so continuing would read `have = 0`
    * and probe permission on a link that does not exist -- once per tick, for
    * as long as the queue stands. Say so and leave the queued value alone;
    * the value is keyed to a sensor id, and if that sensor comes back on a
    * real link the next tick sends it. */
   if (!driver_cal_of(link, &c)) {
      LOGI("calibration: link %d is not a link this driver has", link);
      return;
   }
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
      cal_file_lock();
      cal_lock();
      (void)calq_resolve(CAL_ST_NOTSUP); /* stands; the tick retries */
      cal_unlock();
      cal_file_unlock();
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
    * THE MINUTE IS ELAPSED TIME, not a difference of wall-clock stamps: that
    * lets a clock correction decide how gentle this is. Forward, and the
    * difference exceeds 60 on every reading, so a sensor already declining to
    * answer gets a probe every five minutes' worth of samples in a burst;
    * backward, and the
    * difference is negative for the length of the correction, so the probe is
    * never repeated and a calibration the user confirmed simply never
    * proceeds. The deadline is this module's own now (see the deadline block),
    * and the driver's cal.asked is left as its own record of when it last put
    * the request on the wire. */
   if (!c.have) {
      if (mono_s() >= q_probe_at) {
         driver_cal_bounds(link);
         cal_lock();
         /* ...against the queue we read, for the same reason the send below
          * checks: a probe throttle stamped over a queue the user has replaced
          * would silence the new one's first probe for a minute. The
          * generation is part of "the queue we read" -- re-queueing the SAME
          * value for the SAME sensor is a new entry and is entitled to its own
          * first probe. */
         if (g_q.mgdl == q_mgdl && g_q.id == q_id && g_q.gen == q_gen)
            g_q.probe_at = mono_s() + 60;
         cal_unlock();
         LOGI("calibration queued: probing 0x32 permission before writing");
      }
      return;
   }
   /* Permitted: send the calibration, but only if we are not already awaiting
    * a reply from a recent send. One 0x34 per minute at most -- gentle, and
    * the sensor's reply normally resolves it on the first try.
    *
    * ELAPSED TIME AGAIN, and this is the throttle whose backward-jump failure
    * is the quietest of the lot. A gate that subtracts a wall-clock stamp of
    * the last send from a fresh `realtime_s()` and refuses while the difference
    * is under sixty fails here: a backward correction makes that difference
    * negative, so it stays under sixty for the whole length of the correction
    * and the one retry that would have got the value into the sensor is never
    * made. The row goes on saying PENDING until the window
    * lapses and then says FAILED, and nothing was ever written. */
   if (mono_s() < q_resend_at) {
      return;
   }
   /* THE WRITE CARRIES ITS OWN IDENTITY. The queue was read as a copy above
    * and the lock released, so by the time this returns the user may already
    * have replaced it -- which is precisely the interleaving that ends with
    * the replacement marked accepted if nothing carries an identity. The token
    * handed over here is
    * the one the sensor's answer will come back with, and calib_cal_result
    * matches on all three fields. */
   if (driver_calibrate(link, q_mgdl, q_id, q_gen)) {
      cal_lock();
      /* ...and only if the queue is still the one we read: the write went
       * out for THAT value, and stamping the send against a queue the user
       * has replaced in the meantime would suppress the new one's first
       * attempt for a minute. */
      if (g_q.mgdl == q_mgdl && g_q.id == q_id && g_q.gen == q_gen)
         g_q.resend_at = mono_s() + 60;
      cal_unlock();
      LOGI("calibration %d mg/dL submitted from queue, awaiting sensor reply",
           q_mgdl);
   }
}

/* Driver callback: the sensor answered a calibration we sent. */
void calib_cal_result(int result, int sensor_id, int mg_dl, unsigned gen)
{
   /* THE WHOLE ANSWER UNDER ONE LOCK: the test for a live queue, the record
    * of how it ended, and the clear are one transition, and a reader between
    * any two of them sees a state that never existed. Called with the
    * driver's lock held, so this is driver_lk -> cal_lk as documented. */
   cal_file_lock();
   cal_lock();
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
    *                         exists for: resolving whatever is queued marks
    *                         the replacement APPLIED having never written it,
    *                         the row tells the user the sensor holds it, and
    *                         the sensor goes on reporting against the value
    *                         they discarded.
    *
    * DISCARDED LOUDLY, not silently. Every one of these is either a peer
    * saying something it should not or a real answer arriving after the
    * question changed, and both are worth a line in the log -- a silent drop
    * here is indistinguishable from a misresolution.
    *
    * The queued value is NOT lost by a discard: it was never sent, its resend
    * throttle was never stamped against it (cal_q_attempt_locked stamps only on
    * an exact match), and the next stream attempt writes it. */
   if (g_q.mgdl <= 0 || g_q.id != sensor_id || g_q.mgdl != mg_dl ||
       g_q.gen != gen) {
      LOGW("calibration reply DISCARDED: it answers %d mg/dL for sensor %d "
           "(gen %u), and the live queue is %d mg/dL for sensor %d (gen %u)",
           mg_dl, sensor_id, gen, g_q.mgdl, g_q.id, g_q.gen);
      cal_unlock();
      cal_file_unlock();
      return;
   }
   if (result == 0) {
      LOGI("calibration %d mg/dL ACCEPTED by the sensor", g_q.mgdl);
      (void)calq_resolve(CAL_ST_APPLIED); /* stands; the tick retries */
   } else {
      /* The sensor actively rejected the value -- resending it will not help,
       * so surface it (LAST CAL shows REJECTED) rather than looping or
       * dropping it silently. No beep: the official app is silent on a
       * rejection too. */
      LOGI("calibration %d mg/dL REJECTED by the sensor (result=0x%02x)",
           g_q.mgdl, result);
      (void)calq_resolve(CAL_ST_REJECTED); /* stands; the tick retries */
   }
   cal_unlock();
   cal_file_unlock();
   shell_ui_dirty();
}

/* Loading DEFINES the state, it does not merely add to it: what the file says
 * is the whole truth afterwards, including "nothing is queued". A load that
 * only ever filled things in would leave a caller's earlier queue standing
 * behind a file that had already resolved it. */
int cal_q_load(void)
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
    * the previous verdict, or the new verdict beside a queue from before the
    * restore. */
   cal_file_lock();
   char b[64];
   int rd    = cal_read_line(g_calq_path, b, sizeof b);
   long v[7] = {0, 0, 0, 0, 0, 0, 0};
   /* A LINE THAT DOES NOT DECODE IS NOT A RECORD, and it is not a
    * first run either: it is a file this program did not write. Decoded HERE,
    * before the clear below, so a refusal leaves the live queue and the live
    * verdict exactly as they were -- READ_FAIL is the answer that already
    * means that, and this is the same failure by a different route. */
   if (rd == READ_OK && !cal_parse_ints(b, v, 7)) {
      LOGW("calibration: cal.q does not decode; the queue in memory is "
           "left as it is");
      rd = READ_FAIL;
   }

   /* A FILE THAT COULD NOT BE READ DEFINES NOTHING, so it must not be the
    * reason the state is wiped. Clearing all eight fields before looking at
    * `rd` lets an unreadable cal.q destroy a live queue and a live verdict --
    * exactly the "an unreadable file is not a fresh install" case this
    * loader's three-way answer exists for, applied backwards. READ_NONE (no
    * file at all) does define the state: there is
    * nothing queued, which is the truth on a first run. */
   cal_lock();
   if (rd == READ_FAIL) {
      cal_unlock();
      cal_file_unlock();
      return CALIB_UNSAVED;
   }
   /* THE FILE IS NOW WHAT MEMORY SAYS, by definition: a load DEFINES the
    * state. Anything the previous state had failed to write is gone, not
    * pending -- retrying it would write back a record this file has just
    * replaced. */
   g_calq_unsaved = 0;
   g_q.mgdl = g_q.id = 0;
   g_q.t             = 0;
   g_q.gen           = 0; /* a load DEFINES the state; see below */
   /* The deadlines belong to whatever was queued a moment ago, and a load
    * defines the state: whatever the file says replaces it entirely. */
   g_q.giveup_at = g_q.resend_at = g_q.probe_at = 0;
   g_q.last.mgdl = g_q.last.state = g_q.last.id = 0;
   g_q.last.t                                   = 0;
   if (rd != READ_OK) {
      cal_unlock();
      cal_file_unlock();
      return CALIB_OK; /* READ_NONE: a first run, and that IS the state */
   }
   /* the last-resolved record (fields 4..7) survives regardless of the
    * queue. */
   g_q.last.mgdl  = (int)v[3];
   g_q.last.t     = v[4];
   g_q.last.state = (int)v[5];
   g_q.last.id    = (int)v[6];
   if (v[1] > 0) {
      g_q.id   = (int)v[0];
      g_q.mgdl = (int)v[1];
      g_q.t    = v[2];
      /* A FRESH IDENTITY, AND DELIBERATELY NOT PERSISTED. The generation says
       * which in-process queue entry a write in flight belongs to, and no
       * write can be in flight across a load: at startup the driver has no
       * context at all, and a reload during a run replaces the queue, which is
       * exactly the case where an outstanding reply must NOT be applied to
       * what came off the disk. Writing the number to cal.q would also make a
       * restored entry collide with one the running process had already handed
       * out. So the entry is real, and it is a NEW one. */
      g_q.gen = ++g_calq_gen_next;
      /* RESTART RECONCILIATION, which is the whole reason g_q.t is a
       * wall-clock number and is written to the file. A monotonic deadline
       * cannot cross a reboot -- it counts from an arbitrary origin and starts
       * again with the kernel -- so this is the one subtraction that has to be
       * done on the wall clock, once, here.
       *
       * A calibration confirmed before a restart keeps retrying if it is still
       * fresh, and records the failure if it is not -- never a silent drop.
       * What is left of the window becomes THIS process's deadline, and
       * nothing after this point consults g_q.t again, so a clock
       * correction a minute from now cannot expire or postpone it.
       *
       * REMAINING, NOT A FRESH WINDOW: a phone that relaunches the app every
       * few minutes would otherwise keep a twenty-minute-old fingerstick
       * queued for ever. window_left also decides what a NEGATIVE age means --
       * see it and CLOCK_SKEW_TOL_S in calib.h. */
      long left = cal_window_left(g_q.t, CALQ_WINDOW_S);
      if (left == NO_WINDOW_LEFT) {
         (void)calq_resolve(CAL_ST_FAILED); /* stands; the tick retries */
      } else {
         g_q.giveup_at = mono_s() + left;
         g_q.resend_at = 0; /* a restart may attempt at once */
         g_q.probe_at  = 0;
      }
   }
   cal_unlock();
   cal_file_unlock();
   return CALIB_OK;
}

/* ---- THE QUEUE'S HALF OF A TICK ---------------------------------------
 *
 * CALLER HOLDS cal_file_lock and passes the MONOTONIC now (see cal_tick_locked
 * in calib.c for why the clock is the caller's and why it is monotonic).
 * Returns 1 if the screen must be redrawn.
 *
 * cal_lock is taken and dropped here rather than around the whole tick,
 * because a save inside drops it again for its syscalls and the main thread's
 * per-frame calib_view must never wait on a disk. */
int cal_q_tick(long now_mono)
{
   cal_lock();
   int gave_up = 0;
   if (g_q.mgdl > 0 && g_q.giveup_at > 0 && now_mono > g_q.giveup_at) {
      /* (The "no reply in a minute: allow another attempt" clause that stood
       * here is gone, not moved. It cleared the last-send stamp so the
       * attempt path would stop refusing; a deadline lapses by itself, so the
       * attempt path's own `mono_s() < g_q.resend_at` is the whole rule now.
       * Two places deciding one thing is how they came to disagree under a
       * clock jump in the first place.) */
      LOGI("calibration %d mg/dL never accepted within %ld s; giving up "
           "VISIBLY",
           g_q.mgdl, CALQ_WINDOW_S);
      /* No beep -- LAST CAL shows FAILED; the official app is silent too. */
      /* The result is not checked HERE: calq_resolve records a failed write
       * in g_calq_unsaved and the retry below picks it up. The verdict itself
       * stands either way -- the window has passed. */
      (void)calq_resolve(CAL_ST_FAILED);
      gave_up = 1;
   }
   cal_unlock();

   /* THE RETRY. A transition that stood without reaching the disk is tried
    * again here, every tick, until it lands -- which is what makes "the state
    * is the one the sensor answered" survive a restart rather than merely
    * outlive the screen. Cheap when there is nothing to do. */
   cal_lock();
   if (g_calq_unsaved && calq_save(0) == CALIB_OK)
      LOGI("calibration state reached the disk on a retry");
   cal_unlock();
   return gave_up;
}

/* ---- THE QUEUE'S HALF OF A DEVICE ROW ---------------------------------
 *
 * CALLER HOLDS cal_lock: this half and the rescale's must describe ONE
 * instant, which is the whole reason calib_view is in the coordinator. */
void cal_q_view(int sensor_id, struct calib_view *out)
{
   /* THE QUEUED VALUE AND THE LAST-RESOLVED RECORD ARE ALTERNATIVES, not a
    * pair: while one is in flight the row shows it, and only once it resolves
    * does LAST CAL take the row back. */
   out->queued_mgdl = (g_q.mgdl > 0 && g_q.id == sensor_id) ? g_q.mgdl : 0;
   int resolved =
       (!out->queued_mgdl && g_q.last.t > 0 && g_q.last.id == sensor_id);
   out->last_mgdl  = resolved ? g_q.last.mgdl : 0;
   out->last_state = resolved ? g_q.last.state : 0;
   out->last_t     = resolved ? g_q.last.t : 0;
   /* NOT PER-SENSOR, on purpose: a file that is behind is behind for every
    * row it describes, and there is exactly one of it. */
   out->cal_unsaved = g_calq_unsaved;
}

int cal_q_paths(const char *dir)
{
   return data_path(g_calq_path, sizeof g_calq_path, dir, "/cal.q") != 0;
}
