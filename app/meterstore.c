// SPDX-License-Identifier: GPL-3.0
// meterstore.c --- What the meter runtime remembers (see meterstore.h)
// Copyright 2026 Jakob Kastelic

#include "meterstore.h"
#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "loadresult.h" /* what a load actually found */
#include "log.h"
#include "sensors.h"
#include "thread.h" /* mrt_lk: the table's own lock */
#include "util.h"
#include <stdio.h>

/* PER-METER runtime state, keyed by registry id: when this meter was last
 * connected/synced and the RSSI then. In-memory (reset per launch). The global
 * g_meter_* above only ever hold the LAST meter, which with two meters made one
 * meter's sync throttle the other (a global 60 s gate) and show one meter's
 * signal/sync-time against both. */

static struct meter_rt g_meter_rt[MAX_SLOTS];
static int g_meter_nrt;
static char g_meter_path[256];
static char g_metersync_path[256]; /* per-meter last-sync time, persisted */

/* THE LOCK OVER THE TABLE. See meterstore.h for why the pointer does not
 * leave any more. A leaf: nothing below reaches another module. */
static struct mutex mrt_lk = MUTEX_INIT;

#ifdef APP_FAULTS
void (*meter_fault_gap_here)(void);
#endif

/* The record, or NULL. CALLER HOLDS mrt_lk -- this returns the pointer the
 * public interface deliberately does not. */
static struct meter_rt *rt_find(int id, int create)
{
   for (int i = 0; i < g_meter_nrt; i++)
      if (g_meter_rt[i].id == id)
         return &g_meter_rt[i];
   if (create &&
       g_meter_nrt < (int)(sizeof g_meter_rt / sizeof g_meter_rt[0])) {
      struct meter_rt *r = &g_meter_rt[g_meter_nrt++];
      *r                 = (struct meter_rt){0};
      r->id              = id;
      return r;
   }
   return 0;
}

/* A PLAUSIBLE SIGNAL, or none. The transport reports 0 or +127 when it has
 * no measurement, and storing that shows a signal the meter never sent -- and
 * persists it, so the next launch restores it as real. The advert path
 * filtered for this and the connect path did not; the rule belongs to the one
 * place that stores the field. */
static int rssi_plausible(int rssi)
{
   return rssi <= -1 && rssi >= -127;
}

int meter_rt_read(int id, struct meter_rt *out)
{
   mutex_lock(&mrt_lk);
   const struct meter_rt *r = rt_find(id, 0);
   if (r && out)
      *out = *r; /* A COPY. The phase text is 24 bytes a binder thread
                  * rewrites; a borrowed pointer to it can be read
                  * half-replaced, and the device row draws from it. */
   int have = r != 0;
   mutex_unlock(&mrt_lk);
   return have;
}

int meter_rt_advert(int id, long sync_t, long advert_mono, int rssi,
                    int rssi_ok, long rssi_t)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r) {
      r->sync_t      = sync_t;
      r->advert_mono = advert_mono;
      if (rssi_ok && rssi_plausible(rssi)) {
         r->rssi    = rssi;
         r->rssi_ok = 1;
         r->rssi_t  = rssi_t;
      }
   }
   mutex_unlock(&mrt_lk);
   return r != 0;
}

int meter_rt_advert_turn(int id, long sync_t, long advert_mono, int rssi,
                         int rssi_ok, long rssi_t, long window)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   int took           = 0;
   if (r) {
      /* THE TEST AND THE STAMP TOGETHER. 0 means "not since this launch",
       * which always takes the turn. */
      took = r->advert_mono == 0 || advert_mono - r->advert_mono > window;
      if (took) {
         r->sync_t      = sync_t;
         r->advert_mono = advert_mono;
         if (rssi_ok && rssi_plausible(rssi)) {
            r->rssi    = rssi;
            r->rssi_ok = 1;
            r->rssi_t  = rssi_t;
         }
      }
   }
   mutex_unlock(&mrt_lk);
   return took;
}

int meter_rt_rssi(int id, int rssi, long rssi_t, long sync_t)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r) {
      if (rssi_plausible(rssi)) {
         r->rssi    = rssi;
         r->rssi_ok = 1;
         r->rssi_t  = rssi_t;
      }
      r->sync_t = sync_t; /* the connection itself IS a sync */
   }
   mutex_unlock(&mrt_lk);
   return r != 0;
}

int meter_rt_stat(int id, const char *stat, long sync_t)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r) {
      str_snapshot(r->stat, (int)sizeof r->stat, stat ? stat : "");
      r->sync_t = sync_t;
   }
   mutex_unlock(&mrt_lk);
   return r != 0;
}

/* THE TWO HALVES OF ONE FACT, written together under the one lock: a count
 * that has been incremented and an alternative instant that has not yet been
 * stored describes a record nobody can repair. Same reason meter_rt_advert
 * takes the signal and its stamp in one call. */
int meter_rt_ambiguous(int id, long alt)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r) {
      r->amb_n++;
      r->amb_alt = alt;
   }
   mutex_unlock(&mrt_lk);
   return r != 0;
}

/* NOT rt_find(id, 0). A meter that has no runtime record has no ambiguities
 * either, and creating one here would put an empty row in a fixed-size table
 * for a meter that may never connect. */
int meter_rt_amb_clear(int id)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 0);
   if (r) {
      r->amb_n   = 0;
      r->amb_alt = 0;
   }
   mutex_unlock(&mrt_lk);
   return r != 0;
}

int meter_rt_done(int id, long synced_mono)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r)
      r->synced_t = synced_mono;
   mutex_unlock(&mrt_lk);
   return r != 0;
}

/* Persist every meter's last-sync wall-clock so "LAST SYNC" and the
 * DEVICES-list age survive a restart (rt is otherwise in-memory only and reset
 * to 0 on every launch, which made a fresh install read "OFF / NEVER" for a
 * meter that had in fact synced). Rewrite-and-rename, like meter.idx, so a
 * crash never truncates it to nothing. */
/* Drop records for meters the registry no longer has.
 *
 * The table is MAX_SLOTS long and the loader creates a record for every row
 * in the file, so a forgotten meter's row is self-perpetuating: the save
 * writes it back, the next launch loads it again, and the seats it occupies
 * are never returned. Fill the table that way and every write for a LIVE
 * meter returns 0 -- meter_note_advert then silently drops both the record
 * and the save.
 *
 * IN THREE STEPS, because the registry's answer cannot be asked for under
 * mrt_lk: that would be meter-rt held while registry is taken, the reverse of
 * the order everything else uses, and app/test/lockorder.py would say so. So:
 * copy the ids out, ask, then compact. A record created in between is left
 * alone -- it was not in the list, so it is not one of the ones being
 * dropped. */
static void prune_dead(void)
{
   int ids[MAX_SLOTS];
   int n = 0;
   mutex_lock(&mrt_lk);
   for (int i = 0; i < g_meter_nrt && n < MAX_SLOTS; i++)
      ids[n++] = g_meter_rt[i].id;
   mutex_unlock(&mrt_lk);

   int dead[MAX_SLOTS];
   int nd    = 0;
   int alive = 0;
   for (int i = 0; i < n; i++) {
      if (sensor_id_is_live(ids[i]))
         alive++;
      else
         dead[nd++] = ids[i];
   }
   /* NOT WHILE THE REGISTRY SAYS NOTHING. "No live meter" is the answer both
    * when every meter really has been forgotten and when the registry has not
    * been read -- a first launch, or a slots.csv that failed to load. Acting
    * on the second would delete every meter's last-sync because of an
    * unrelated failure, so a table with nothing live in it is left alone: the
    * seats are only worth reclaiming when there is a live meter to reclaim
    * them for. */
   if (!nd || !alive)
      return;

   mutex_lock(&mrt_lk);
   int keep = 0;
   for (int i = 0; i < g_meter_nrt; i++) {
      int drop = 0;
      for (int j = 0; j < nd && !drop; j++)
         if (g_meter_rt[i].id == dead[j])
            drop = 1;
      if (!drop)
         g_meter_rt[keep++] = g_meter_rt[i];
   }
   g_meter_nrt = keep;
   mutex_unlock(&mrt_lk);
}

/* ---- THE SAVE IS SERIALISED THROUGH COMPLETION -------------------------
 *
 * WHAT THIS USED TO BE, AND WHAT IT COST THE PERSON HOLDING THE PHONE.
 *
 * The text was rendered under mrt_lk and only THEN did the caller try to
 * enter a single flight; a caller that found a writer already running
 * returned 0 -- success -- on the argument that "a save that is already in
 * progress has just written what this caller wanted written".
 *
 * That argument is false, and the ORDER of the two steps is exactly why. The
 * running writer's buffer was rendered from the table as it stood BEFORE the
 * losing caller's mutation, so the loser's newer value was in nobody's
 * buffer. Nothing ever came back for it: the file kept the older row until
 * some later save happened to run alone. For a meter that has just been
 * switched off again -- which is what a OneTouch does seconds after a sync --
 * the next save is the next time the user picks the meter up, days later.
 *
 * On the phone this is two BLE binder threads milliseconds apart: two meters
 * woken by the same person, or one meter's advertisement landing while the
 * RSSI read from the other's connection is being persisted. The user watches
 * LAST SEEN update on the DEVICES row, closes the app, and the next launch
 * shows the older time -- or NEVER, for a meter whose first sync it was. It
 * is silent, and there is nothing on the screen to distrust: the save was
 * reported as having succeeded.
 *
 * SO THE RENDER AND THE WRITE ARE NOW ONE CRITICAL SECTION under msync_lk. A
 * second caller waits, and then renders the table AS IT IS WHEN ITS TURN
 * COMES -- which necessarily contains its own mutation and everything that
 * landed while it waited. When this returns 0, the bytes on disk are at least
 * as new as the table was when the call began.
 *
 * WHAT THAT GIVES UP, plainly, because it is not nothing:
 *
 *   - A caller arriving during another save now waits out one atomic_replace
 *     (two fsyncs and a rename) instead of returning at once.
 *   - N callers racing perform N writes where the single flight performed
 *     one.
 *
 * Both are affordable HERE and the reason is the caller list, not a general
 * principle: every caller of this function is a BLE binder callback (a scan
 * result via meter_note_advert, a connection's RSSI via pancra_meter_rssi),
 * the advert path is throttled to one per meter per minute, and there are two
 * meters. The MAIN thread never calls it, so rule 5's ANR shape is not on the
 * table. The alternative the TODO offers -- a dirty generation with the
 * active writer re-snapshotting in a bounded loop -- keeps the non-blocking
 * return but weakens the contract to "your value is on disk, or a writer that
 * will include it is still running", which is a contract no caller can act
 * on and no test can assert at the call.
 *
 * WHAT IT DOES NOT GIVE UP is the property the single flight was really
 * protecting: mrt_lk is NOT held while waiting, and not held across the file.
 * msync_lk is therefore the same shape as calfile_lk and set_file_lk -- taken
 * OUTSIDE the state lock, held across the I/O, and never taken by a reader.
 * See the rank table in app/thread.h, which lists it, and
 * app/test/lockorder.py, which checks it. */
static struct mutex msync_lk = MUTEX_INIT;

int meter_sync_save(void)
{
   char all[(MAX_SLOTS * 64) + 1];
   int used = 0;
   /* A forgotten meter's row is not written back, and does not go on holding
    * a seat in the table.
    *
    * OUTSIDE msync_lk, deliberately: prune_dead asks the REGISTRY which ids
    * are still live, and the registry's lock ranks above both of this file's
    * (thread.h). Taking it under msync_lk would be the one backwards edge in
    * this module. */
   prune_dead();
   int rc = 0;
   mutex_lock(&msync_lk);
   /* THE TEXT IS BUILT UNDER THE TABLE'S LOCK; THE FILE IS WRITTEN WITH IT
    * RELEASED.
    *
    * Both halves matter. Building it under the lock is what makes the line
    * coherent -- a binder thread updating a meter's RSSI mid-loop would
    * otherwise be persisted as one meter's time beside another's signal.
    *
    * But atomic_replace does two fsyncs and a rename, and mrt_lk is a
    * SPINNING mutex with no timeout (thread.h): holding it across flash I/O
    * parks every other caller in a yield-spin for the duration -- including
    * the main thread, which reaches meter_rt_read from build_model while it
    * holds the history lock. That is the ANR shape this app has already been
    * killed for twice. The sibling module states the rule plainly (see
    * metersess.h): a leaf lock is not held across anything slow. */
   mutex_lock(&mrt_lk);
   for (int i = 0; i < g_meter_nrt; i++) {
      const struct meter_rt *r = &g_meter_rt[i];
      if (r->sync_t <= 0)
         continue;
      /* id, last-seen time, and the RSSI captured THEN (so SIGNAL STRENGTH is
       * the signal AT last-seen, and survives a restart -- it used to be
       * in-memory only, so a meter read "-- signal" despite a real LAST SEEN).
       *
       * Read out field by field, then formatted, rather than as four
       * subexpressions of one snprintf. Identical under the lock; what it
       * buys is a place to put the yield below. */
      int rid    = r->id;
      long rsync = r->sync_t;
#ifdef APP_FAULTS
      /* WIDEN THE WINDOW, in the fault-injection build only.
       *
       * The time and the signal are read within a few instructions of each
       * other, so a render that does not hold mrt_lk is wrong for exactly
       * that long. With no help, a build with the lock deleted wrote a
       * coherent file on every run of the concurrency test below --
       * ThreadSanitizer saw the race and no assertion could. Yielding BETWEEN
       * the two halves of a row is what makes the interleaving ordinary; a
       * yield between whole rows changed nothing, because the tear was never
       * between rows.
       *
       * A HOOK RATHER THAN A BARE YIELD, and the reason is measured: this
       * loop also runs under the suite's OTHER concurrency section, where a
       * save thread renders continuously while two writers hammer the table.
       * Yielding under mrt_lk there starved those writers and turned a 15 s
       * suite into one that had not finished in five minutes. So the test
       * installs the gap around the section that needs it and takes it away
       * again -- app_fault_gap_here in util.h is the same device. Nothing
       * that ships defines APP_FAULTS. */
      if (meter_fault_gap_here)
         meter_fault_gap_here();
#endif
      int rrssi = r->rssi;
      int rok   = r->rssi_ok;
      int bn = snprintf(all + used, sizeof all - (size_t)used, "%d,%ld,%d,%d\n",
                        rid, rsync, rrssi, rok);
      if (bn <= 0 || bn >= (int)sizeof all - used) {
         /* ONE EXIT, and it is not a `return`: leaving from here would walk
          * away holding both locks, and both are yield-spins with no timeout.
          * app/test/lockorder.py refuses a return taken while a lock is
          * held for exactly this reason. */
         rc = -1;
         break;
      }
      used += bn;
   }
   mutex_unlock(&mrt_lk);
#ifdef APP_FAULTS
   /* AND WIDEN THE OTHER WINDOW: between the render and the write.
    *
    * This is the one the whole fix is about. Rendering here and serialising
    * only afterwards means a caller can render, be overtaken by a NEWER
    * render that reaches the file first, and then put its own stale bytes on
    * top -- which is the losing caller's mutation disappearing. On real
    * hardware that window is a few instructions and a test almost never lands
    * inside it; settings.c's write_job carries the identical yield for the
    * identical reason.
    *
    * In this build the yield sits INSIDE msync_lk, where it costs a delayed
    * write and nothing else. In a build that takes the lock after the render
    * it sits outside, and the overtaking becomes ordinary rather than rare --
    * which is exactly the difference the test has to be able to see. Nothing
    * that ships defines APP_FAULTS. */
   if (meter_fault_gap_here)
      meter_fault_gap_here();
#endif
   /* REPLACE_UNSYNCED counts as saved: the rename happened, so the file
    * holds these bytes and only their survival of a power cut is unknown.
    * Reported as failure the caller would keep re-writing a file that is
    * already correct. */
   if (!rc && atomic_replace(g_metersync_path, all, used) == REPLACE_FAILED)
      rc = -1;
   mutex_unlock(&msync_lk);
   return rc;
}

enum load_result meter_sync_load(void)
{
   int fd = open(g_metersync_path, O_RDONLY, 0);
   if (fd < 0) {
      /* A first run has no file, and that is not a failure. Anything else is
       * a file that is there and cannot be opened. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      return LOAD_ERROR;
   }
   char b[1024];
   int n = (int)read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT; /* created and not written: a torn save */
   b[n]    = 0;
   char *p = b;
   while (*p) {
      long v[4] = {0, 0, 0, 0}; /* id, sync_t, rssi (signed), rssi_ok */
      int vi    = 0;
      int neg   = 0;
      int any   = 0;
      int nd    = 0; /* digits in THIS field */
      while (*p && *p != '\n') {
         if (*p >= '0' && *p <= '9') {
            /* CAPPED, like meter_index_all's parser 120 lines below and like
             * every other loader in the app. Unbounded accumulation on a long
             * is undefined behaviour and it happens during PARSING, before
             * any range check can reject the row -- and the value it lands on
             * is a last-seen time centuries away, which reads as "LAST SEEN"
             * forever. The sibling parser was hardened and this one, in the
             * same file, was not. */
            if (nd < 15) {
               v[vi] = (v[vi] * 10) + (*p - '0');
               nd++;
            }
            any = 1;
         } else if (*p == '-') {
            neg = 1;
         } else if (*p == ',' && vi < 3) {
            if (neg)
               v[vi] = -v[vi];
            neg = 0;
            vi++;
            nd = 0;
         }
         p++;
      }
      if (neg)
         v[vi] = -v[vi];
      if (*p == '\n')
         p++;
      if (any && v[0] > 0) {
         /* The same operation an advertisement performs, because it says the
          * same thing: this meter was last seen THEN, with THAT signal. v[1]
          * is a wall-clock instant; there is no advert interval to restore,
          * since an interval that spans a restart means nothing. */
         (void)meter_rt_advert((int)v[0], v[1], 0, (int)v[2], (int)v[3], v[1]);
      }
   }
   return LOAD_OK;
}

/* Per-meter record index, keyed by SENSOR ID.
 *
 * A single shared index cannot serve two meters: their counters are
 * unrelated, so each sync looked like the other meter's counter had gone
 * backwards
 * ("memory cleared"), reset to -1, re-imported, and saved its own value --
 * leaving the pair oscillating forever. One meter never reached its own new
 * records again, so every fingerstick it took was silently lost, while the
 * other re-imported records it already had (re-appending them to the log,
 * since they are outside the in-memory dedup window) and held each meter
 * awake for a full walk on every advert.
 *
 * Stored as "id,index" lines, rewritten whole -- it is at most MAX_SLOTS
 * rows.
 */
/* THE INDEX FILE'S OWN LOCK, distinct from the runtime table's.
 *
 * meter_index_save is a read-modify-write of meter.idx: read every row, put
 * this meter's index in, write the whole file back. Two of those at once lose
 * one meter's walk position -- and the record index is the one fact in this
 * module whose loss costs real fingersticks, because the protocol can only be
 * asked for "records after N".
 *
 * It was serialised by ACCIDENT: the only writer runs inside the driver's
 * lock (ot_drv_done), while meter_index_load is called from the main thread
 * during pairing with no such guarantee. An invariant that lives in another
 * module's call graph is one an edit somewhere else can remove.
 *
 * Separate from mrt_lk because the two protect different things and this one
 * is held across file I/O; nothing takes both. */
static struct mutex idx_lk = MUTEX_INIT;

/* The stored pairs, with idx_lk already held: defined below, beside the file
 * format it reads. */
static int index_all_locked(int *ids, int *vals, int cap);

int meter_index_save(int id, int idx)
{
   if (id <= 0)
      return -1;
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   mutex_lock(&idx_lk);
   int n  = index_all_locked(ids, vals, MAX_SLOTS);
   int at = -1;
   for (int i = 0; i < n && at < 0; i++)
      if (ids[i] == id)
         at = i;
   if (at < 0 && n < MAX_SLOTS) {
      at      = n++;
      ids[at] = id;
   }
   if (at < 0) {
      /* Full. Rows are never pruned -- every id a meter has ever carried
       * keeps one -- so silently skipping the write meant this meter's index
       * was NEVER persisted again, and every later advert re-walked
       * OT_MAX_WALK records, re-appending weeks-old fingersticks to the
       * lifetime log.
       *
       * Evict a row NO LIVE SLOT REFERENCES. This used to drop ids[0] with a
       * comment claiming it "belongs to a superseded id that nothing reads"
       * -- an assumption the code did not check. Rows sit in the order they
       * were first written, so the oldest row is the FIRST meter ever
       * registered, and if that meter is still in use its index was the one
       * thrown away: its next sync sees no stored index, re-walks, and
       * re-appends fingersticks that are weeks old and therefore outside the
       * dedup window -- double-counted in the stats, permanently. */
      /* WHICH ROWS ARE LIVE IS THE REGISTRY'S QUESTION, and it is asked
       * here, inside idx_lk, only because the answer is used immediately. It
       * is safe in this order -- idx_lk is a leaf that the registry never
       * takes -- but it is the one call out of this file under a lock, so it
       * is worth naming. See app/test/lockorder.py, which checks the pair. */
      int victim = -1;
      for (int i = 0; i < n && victim < 0; i++)
         if (!sensor_id_is_live(ids[i]))
            victim = i;
      /* Every row live: only possible if the table already names every slot,
       * in which case `id` matched above and we never got here. Fall back to
       * the oldest so the write still happens rather than being skipped. */
      if (victim < 0)
         victim = 0;
      for (int i = victim + 1; i < n; i++) {
         ids[i - 1]  = ids[i];
         vals[i - 1] = vals[i];
      }
      at      = n - 1;
      ids[at] = id;
   }
   vals[at] = idx;

   /* Write a fresh file and rename over the old one, rather than truncating
    * in place. O_TRUNC destroys the stored indices BEFORE the new ones are
    * written, so a crash in that window left the file empty and the next
    * sync re-imported -- those fingersticks are typically weeks old, i.e.
    * outside the dedup window, so they were appended a second time and
    * double-counted in the stats. rename() is atomic: old values or new,
    * never nothing. */
   char all[(MAX_SLOTS * 32) + 1];
   int used = 0;
   for (int i = 0; i < n; i++) {
      int bn = snprintf(all + used, sizeof all - (size_t)used, "%d,%d\n",
                        ids[i], vals[i]);
      if (bn <= 0 || bn >= (int)sizeof all - used) {
         mutex_unlock(&idx_lk);
         return -1;
      }
      used += bn;
   }
   int rc = atomic_replace(g_meter_path, all, used) == REPLACE_FAILED;
   mutex_unlock(&idx_lk);
   return rc ? -1 : 0;
}

/* The stored (id, index) pairs. CALLER HOLDS idx_lk: the save reads them,
 * decides, and writes the file back as one operation. */
static int index_all_locked(int *ids, int *vals, int cap)
{
   int fd = open(g_meter_path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return 0;
   b[n]    = 0;
   int cnt = 0;
   char *p = b;
   while (*p && cnt < cap) {
      int id  = 0;
      int v   = 0;
      int gi  = 0;
      int gv  = 0;
      int neg = 0;
      /* Digit-capped: unbounded accumulation is UB and happens during
       * parsing, before the `id > 0` test can reject anything. This file is
       * ours, but surviving a corrupt or hand-edited row is a parser's job
       * -- and every sibling parser (store.c, stats.c, sensors.c,
       * settings.c) received this hardening. The advance stays OUTSIDE the
       * cap, which is what a sibling fix got wrong and turned into a
       * launch-time hang. */
      int nid = 0;
      while (*p >= '0' && *p <= '9') {
         if (nid < 9) {
            id = (id * 10) + (*p - '0');
            nid++;
         }
         p++;
         gi = 1;
      }
      if (*p == ',')
         p++;
      if (*p == '-') {
         neg = 1;
         p++;
      }
      int nv = 0;
      while (*p >= '0' && *p <= '9') {
         if (nv < 9) {
            v = (v * 10) + (*p - '0');
            nv++;
         }
         p++;
         gv = 1;
      }
      if (gi && gv && id > 0) {
         ids[cnt]  = id;
         vals[cnt] = neg ? -v : v;
         cnt++;
      }
      while (*p && *p != '\n')
         p++;
      if (*p == '\n')
         p++;
   }
   return cnt;
}

/* ...and the same question from outside, which takes the lock for itself. */
int meter_index_all(int *ids, int *vals, int cap)
{
   mutex_lock(&idx_lk);
   int n = index_all_locked(ids, vals, cap);
   mutex_unlock(&idx_lk);
   return n;
}

/* This meter's stored index, or -1 for "nothing stored yet".
 * -1, not 0: index 0 is a real record, so the sentinel must sit below every
 * valid index or the meter's first record is skipped. */
int meter_index_load(int id)
{
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   int n = meter_index_all(ids, vals, MAX_SLOTS);
   for (int i = 0; i < n; i++)
      if (ids[i] == id)
         return vals[i];
   if (n > 0)
      return -1; /* new-format file, but this meter is not in it yet */

   /* MIGRATION: an install from before this file was keyed by sensor id
    * holds a bare integer. Parsing it as "id,index" yields nothing, so the
    * index would look unset and the meter would re-import its recent window
    * -- records typically weeks old, i.e. outside the in-memory dedup
    * window, so they would be appended to the lifetime log a second time.
    * Adopt the old value for whichever meter asks first; that is exactly
    * right, because the old format could only ever describe one meter. The
    * next save rewrites the file in the new format. */
   /* AND IT IS PARSED STRICTLY, because of what an index MEANS.
    *
    * This ran `v = v * 10 + digit` into a signed int with no cutoff, at
    * STARTUP, over a file a user can edit and a torn write can leave
    * half-formed. Two consequences, and the second is the one that costs
    * fingersticks:
    *
    *   - signed overflow is undefined behaviour, so a long digit run here was
    *     UB during launch, before any check could look at the value;
    *
    *   - and the value that came out was not obviously wrong. `4294967297`
    *     wrapped to exactly 1, which is a PERFECTLY LEGAL record index --
    *     nothing downstream refuses it -- so the app published "records 0 and
    *     1 are already imported" for a meter it had never read, and those two
    *     fingersticks were never fetched. `4294967296` wrapped to 0 and lost
    *     the first one the same way. Measured on the shipped parser: 1, 0,
    *     -2147483648 and 1661992959 for four inputs that are all just digits.
    *
    * So: the WHOLE file must be one canonical unsigned decimal, and the value
    * must lie in the domain a OneTouch record index actually has.
    *
    * "Canonical" earns its keep. The old loop stopped at the first non-digit
    * and kept what it had, so `123abc` adopted 123 and `0012345` adopted
    * 12345 -- a file that is not this format at all was read as though it
    * were, and its leading digits became a walk position. A migration path is
    * exactly where that must not happen: it runs once, silently, and what it
    * publishes is then written back in the new format and believed for ever.
    * A file we cannot read with certainty is better treated as absent (-1,
    * "walk from the beginning"), which costs one re-import of a bounded
    * window, than as a number we guessed. */
   int fd = open(g_meter_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char b[32];
   int rn = (int)read(fd, b, sizeof b - 1);
   close(fd);
   /* A short read is the whole file; a FULL one means there may be more of it
    * past the buffer, and a prefix of a file is not "full canonical input" --
    * the old code would have adopted the first 31 bytes' worth of digits. */
   if (rn <= 0 || rn >= (int)sizeof b - 1)
      return -1;
   int end = rn;
   if (end > 0 && b[end - 1] == '\n')
      end--;
   if (end > 0 && b[end - 1] == '\r')
      end--;
   /* csvcur.h's reader, the same one sensors.c, insulin.c, weight.c and
    * plotdata.c use, rather than a sixth private digit loop: it caps the
    * accumulation at CSV_MAX_DIGITS into a LONG, so the arithmetic below can
    * no longer be undefined, and it distinguishes "no digits" from "too many"
    * -- both of which are a refusal here, but for different reasons worth
    * being able to tell apart. */
   struct csv_cur c;
   enum csv_field why;
   csv_open(&c, b, b + end);
   long v = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_at_end(&c))
      return -1; /* empty, trailing rubbish, or more digits than fit */
   if (b[0] == '0' && end > 1)
      return -1; /* leading zeros: not something this format ever wrote */
   /* THE REAL DOMAIN, and it is not int's. A record index goes out to the
    * meter in sixteen bits, so otble.c refuses a record counter above 0xFFFF
    * outright ("a counter above 0xFFFF can never be satisfied"): an index
    * wider than that names no record any OneTouch meter can be asked for. The
    * comparison is on `v` while it is still a long -- a bound applied after
    * the narrowing cast would be no bound at all, which is the entire defect
    * above. Nothing checks that this agrees with otble.c's literal. */
   if (v < 0 || v > 0xFFFF)
      return -1;
   LOGI("meter.idx: adopting legacy index %d for id %d", (int)v, id);
   return (int)v;
}

void meter_store_paths(const char *index_path, const char *sync_path)
{
   str_snapshot(g_meter_path, sizeof g_meter_path, index_path);
   str_snapshot(g_metersync_path, sizeof g_metersync_path, sync_path);
}
