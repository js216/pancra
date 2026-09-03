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

/* ---- WHAT IS ON DISK, AND WHAT IS ONLY IN MEMORY -------------
 *
 * Every write to a PERSISTED field bumps `g_mrt_gen`; a save that reached
 * the file records the generation it rendered in `g_mrt_saved`. The two
 * being equal is the only evidence that what the screen shows about a meter
 * -- LAST SEEN, and the signal at that instant -- will still be there after
 * a restart.
 *
 * They exist because the callers could not tell. meter_rt_rssi answers
 * whether the LIVE observation was accepted (was there a row for this
 * meter), meter_sync_save answers whether the FILE was written, and the
 * production callers took the first as though it were both -- they dropped
 * the save's answer on the floor entirely. A full data partition therefore
 * looked exactly like a healthy one until the next launch, when the meter
 * came back with no last-seen time at all and nothing to say why.
 *
 * A GENERATION RATHER THAN A FLAG, because the save renders the whole table
 * and the render is not instantaneous: a write that lands while a save is in
 * flight may or may not be in the bytes that save is about to commit, and a
 * flag cleared on success would clear that write's dirtiness too. Comparing
 * the generation the save actually RENDERED against the current one cannot
 * make that mistake -- the later write leaves the table dirty, and the retry
 * picks it up. Both are read and written under mrt_lk, beside the fields
 * they describe. */
static unsigned long g_mrt_gen;
static unsigned long g_mrt_saved;

/* A PERSISTED FIELD CHANGED. Caller holds mrt_lk. The non-persisted ones
 * (the phase text's siblings, the ambiguity count, the re-arm stamp) do not
 * call this: marking the table dirty for a field the file does not carry
 * would keep the retry writing the same bytes forever. */
static void rt_touch(void)
{
   g_mrt_gen++;
}

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
      rt_touch();
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
         rt_touch();
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
      rt_touch();
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
      r->sync_t = sync_t; /* persisted; the phase text is not */
      rt_touch();
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

/* rt_find(id, 1): the clock is read in the handshake, which is the first
 * exchange of a walk, so this can be the first thing said about a meter that
 * has just connected for the very first time. */
int meter_rt_clock(int id, long skew, long at)
{
   mutex_lock(&mrt_lk);
   struct meter_rt *r = rt_find(id, 1);
   if (r) {
      r->clock_skew = skew;
      r->clock_t    = at;
      r->clock_ok   = 1;
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
/* Drop records for meters the registry does not hold.
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
 * the order everything else uses, and test/app/lockorder.py would say so. So:
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
   if (keep != g_meter_nrt)
      rt_touch(); /* a row that is gone is a file that is wrong */
   g_meter_nrt = keep;
   mutex_unlock(&mrt_lk);
}

/* ---- THE SAVE IS SERIALISED THROUGH COMPLETION -------------------------
 *
 * THE RENDER AND THE WRITE ARE ONE CRITICAL SECTION, under msync_lk. A second
 * caller waits, and then renders the table AS IT IS WHEN ITS TURN COMES --
 * which necessarily contains its own mutation and everything that landed
 * while it waited. When this returns 0, the bytes on disk are at least as new
 * as the table was when the call began, and that is the contract a caller can
 * act on.
 *
 * WHY IT IS NOT A SINGLE FLIGHT. A caller that found a writer already running
 * and returned success would be relying on "a save in progress has just
 * written what I wanted written", and that is false: the running writer's
 * buffer was rendered from the table as it stood BEFORE the second caller's
 * mutation, so the newer value is in nobody's buffer, and nothing comes back
 * for it until some later save happens to run alone. For a meter that has
 * just been switched off again -- which is what a OneTouch does seconds after
 * a sync -- the next save is the next time the user picks the meter up, days
 * later. On the phone the race is two BLE binder threads milliseconds apart:
 * two meters woken by the same person, or one meter's advertisement landing
 * while the RSSI read from the other's connection is being persisted. The
 * user watches LAST SEEN update on the DEVICES row, closes the app, and the
 * next launch shows the older time -- or NEVER, for a meter whose first sync
 * it was -- with nothing on the screen to distrust, because the save reported
 * success.
 *
 * WHAT WAITING COSTS, plainly, because it is not nothing:
 *
 *   - A caller arriving during another save waits out one atomic_replace (two
 *     fsyncs and a rename).
 *   - N callers racing perform N writes.
 *
 * Both are affordable HERE and the reason is the caller list, not a general
 * principle: every caller of this function is a BLE binder callback (a scan
 * result via meter_note_advert, a connection's RSSI via pancra_meter_rssi),
 * the advert path is throttled to one per meter per minute, and there are two
 * meters. The MAIN thread never calls it, so rule 5's ANR shape is not on the
 * table. A dirty generation with the active writer re-snapshotting in a
 * bounded loop would keep the non-blocking return and weaken the contract to
 * "your value is on disk, or a writer that will include it is still running",
 * which is a contract no caller can act on and no test can assert at the
 * call.
 *
 * MRT_LK IS NOT HELD while waiting, and not held across the file. msync_lk is
 * therefore the same shape as calfile_lk and set_file_lk -- taken OUTSIDE the
 * state lock, held across the I/O, and never taken by a reader. See the rank
 * table in app/thread.h, which lists it, and test/app/lockorder.py, which
 * checks it. */
static struct mutex msync_lk = MUTEX_INIT;

/* THE BODY OF BOTH THE SAVE AND THE RETRY. `block` says what to do when
 * another caller already holds the file: the save waits (its contract is
 * "your value is on disk when I return"), the retry leaves at once and says
 * so -- it is speculative work on a tick, and a writer that is already
 * rendering will include the row it would have written anyway.
 *
 * Returns 0 written, -1 refused by the file system, 1 not attempted. */
static int sync_flush(int block)
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
   /* THE REFUSAL IS OUTSIDE THE LOCK, and it is a refusal to take one --
    * nothing is held here, which is what makes this an ordinary early
    * return rather than the shape lockorder.py rejects. */
   if (!block && !mutex_trylock(&msync_lk))
      return 1;
   if (block)
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
   /* THE GENERATION THESE BYTES DESCRIBE, read with the bytes and not after
    * them: a write landing between the render and this read would be
    * credited to a file that does not contain it. */
   unsigned long rendered = g_mrt_gen;
   for (int i = 0; i < g_meter_nrt; i++) {
      const struct meter_rt *r = &g_meter_rt[i];
      if (r->sync_t <= 0)
         continue;
      /* id, last-seen time, and the RSSI captured THEN (so SIGNAL STRENGTH is
       * the signal AT last-seen, and survives a restart -- held in memory
       * only, a meter reads "-- signal" despite a real LAST SEEN).
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
          * test/app/lockorder.py refuses a return taken while a lock is
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
   if (!rc) {
      /* THE FILE NOW HOLDS `rendered`. Recorded under mrt_lk, inside
       * msync_lk, and only ever forwards: two saves cannot commit out of
       * order (msync_lk serialises them through completion), but a retry
       * that rendered an older generation than a save still in flight must
       * not be able to walk the mark backwards. */
      mutex_lock(&mrt_lk);
      if (g_mrt_saved < rendered)
         g_mrt_saved = rendered;
      mutex_unlock(&mrt_lk);
   }
   mutex_unlock(&msync_lk);
   return rc;
}

int meter_sync_save(void)
{
   return sync_flush(1);
}

int meter_sync_dirty(void)
{
   mutex_lock(&mrt_lk);
   int dirty = g_mrt_gen != g_mrt_saved;
   mutex_unlock(&mrt_lk);
   return dirty;
}

/* THE RETRY CLOCK. A refused write is refused for a reason that does not go
 * away in a second (a full partition, a read-only remount), and this is
 * called from a 1 Hz tick, so retrying every tick would spend two fsyncs a
 * second achieving nothing. Stamped by the ATTEMPT rather than by the tick:
 * an interval that starts when the disk last said no is the one that
 * describes how long it has been given to recover. */
static long g_retry_next;
#define METER_RETRY_S 30

enum sync_retry meter_sync_retry(long now)
{
   if (!meter_sync_dirty())
      return SYNC_CLEAN;
   if (now < g_retry_next)
      return SYNC_DEFERRED;
   g_retry_next = now + METER_RETRY_S;
   int rc       = sync_flush(0);
   if (rc == 1)
      return SYNC_DEFERRED; /* a real writer has it; theirs covers this */
   if (rc != 0)
      return SYNC_STILL_DIRTY;
   /* A SAVE THAT WORKED IS NOT NECESSARILY A CLEAN TABLE: a write that
    * landed after the render leaves it dirty on purpose, and the next tick
    * carries it. Ask, rather than assume. */
   return meter_sync_dirty() ? SYNC_DEFERRED : SYNC_CLEAN;
}

/* THE PER-METER LAST-SYNC FILE, READ WHOLE OR NOT AT ALL.
 *
 * THE THREE WAYS A LOADER OF THIS FILE GOES WRONG, each of which the rules
 * below exist to prevent:
 *
 *   - ONE read(2), taking whatever comes back as the file. A read interrupted
 *     by a signal returns -1/EINTR with nothing read, which reads as
 *     LOAD_ERROR for a perfectly good file; a read that fills the buffer
 *     looks like a whole file, so a longer one is silently truncated at 1023
 *     bytes -- and the truncation lands mid-row, whose surviving digits parse
 *     into a plausible id and a plausible instant.
 *   - TOLERATING ANYTHING. Fields accumulated by a hand-rolled digit loop
 *     that ignores every character it does not recognise make "9,,x,-" and
 *     "9" and "9,,,,,,,,7" all parse, each into some row. A missing field
 *     reads as zero, which for the RSSI columns means "a real reading of 0
 *     dBm" rather than "we do not know".
 *   - PUBLISHING AS IT PARSES. A row applied to the live runtime table the
 *     moment it is read leaves a file that goes wrong halfway with its first
 *     half applied and LOAD_OK reported. Half a restore is the shape that is
 *     hardest to notice: the meters that loaded look right, and the ones that
 *     did not look like meters never seen.
 *
 * So: read to EOF with EINTR handled, prove there is nothing past the buffer,
 * require exactly four fields in range on every row, stage the lot privately,
 * and apply nothing until all of it is known good. */

/* At most one row per slot: this file is written from the runtime table,
 * which is MAX_SLOTS wide. A file with more rows than that is not one this
 * app wrote. */
/* Epoch bound on a last-seen instant: the same value and the same rationale
 * as EX_T_MAX, INS_T_MAX and WT_T_MAX -- year 3000, wide enough for any real
 * clock and tight enough that a corrupt field is refused rather than shown as
 * a last-seen time centuries away. */
#define METER_SYNC_T_MAX 32503680000L

struct msync_row {
   long id, sync_t;
   int rssi, rssi_ok;
};

/* One row's four fields, or 0. `line`..`end` is the row without its
 * newline. */
static int msync_parse(const char *line, const char *end, struct msync_row *r)
{
   struct csv_cur c;
   enum csv_field why = CSV_FIELD_OK;
   csv_open(&c, line, end);
   r->id = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   r->sync_t = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   long rssi = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_sep(&c))
      return 0;
   long ok = csv_num(&c, &why);
   /* EXACTLY FOUR. A fifth field is not a row this format ever wrote, and
    * accepting it means accepting whatever a later version might put there
    * as though this version understood it. */
   if (why != CSV_FIELD_OK || !csv_at_end(&c))
      return 0;
   /* THE RANGES, and each one is a fact about the thing it describes:
    *   id       a sensor id, which sensors.h issues from 1 upward;
    *   sync_t   a wall-clock instant, in the past and not centuries hence;
    *   rssi     a BLE signal, which is dBm and negative in practice, held in
    *            a signed byte's worth of range;
    *   rssi_ok  a flag, so it is one of two values and nothing else. */
   if (r->id <= 0 || r->id > 0xFFFF)
      return 0;
   if (r->sync_t <= 0 || r->sync_t >= METER_SYNC_T_MAX)
      return 0;
   if (rssi < -200 || rssi > 20)
      return 0;
   if (ok != 0 && ok != 1)
      return 0;
   r->rssi    = (int)rssi;
   r->rssi_ok = (int)ok;
   return 1;
}

enum load_result meter_sync_load(void)
{
   /* ONE EXACT READ. This function's own loop -- EINTR-safe, to
    * the end, with a probe for a byte past the buffer -- is where
    * read_file_exact came FROM: it was the only loader in the app that had
    * all three right, and the others each had their own single unchecked
    * read. It is shared now, so there is one of it. */
   char b[1024];
   int used            = 0;
   enum load_result rr = read_file_exact(g_metersync_path, b, sizeof b, &used);
   if (rr != LOAD_OK)
      return rr;

   /* STAGED, and nothing is published until every row has parsed. */
   struct msync_row rows[MAX_SLOTS];
   int nrow = 0;
   int at   = 0;
   while (at < used) {
      int eol = at;
      while (eol < used && b[eol] != '\n')
         eol++;
      /* A LAST LINE WITH NO NEWLINE is a file cut while being written: its
       * bytes may parse perfectly and still be half a row. This file is
       * published by rename, so a torn one is damage rather than a race. */
      if (eol >= used)
         return LOAD_CORRUPT;
      if (eol > at) { /* a blank line is nothing to apply, and not damage */
         if (nrow >= (int)(sizeof rows / sizeof rows[0]))
            return LOAD_CORRUPT; /* more rows than the table can hold */
         if (!msync_parse(b + at, b + eol, &rows[nrow]))
            return LOAD_CORRUPT;
         nrow++;
      }
      at = eol + 1;
   }

   /* PUBLISHED, all of it, only now. The same operation an advertisement
    * performs, because it says the same thing: this meter was last seen THEN,
    * with THAT signal. There is no advert interval to restore, since an
    * interval that spans a restart means nothing. */
   for (int i = 0; i < nrow; i++)
      (void)meter_rt_advert((int)rows[i].id, rows[i].sync_t, 0, rows[i].rssi,
                            rows[i].rssi_ok, rows[i].sync_t);
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
static int index_all_locked(int *ids, int *vals, int cap, enum load_result *how,
                            int *overflowed);

int meter_index_save(int id, int idx)
{
   if (id <= 0)
      return -1;
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   mutex_lock(&idx_lk);
   /* A REWRITE MUST SEE THE WHOLE MAP. This function reads every
    * pair, changes one, and writes them all back -- so a read that could not
    * see every pair would publish a file with the unseen ones deleted. That
    * is exactly the loss this file exists to prevent, in the one operation
    * that can cause it. */
   int over = 0;
   int n    = index_all_locked(ids, vals, MAX_SLOTS, NULL, &over);
   if (over) {
      mutex_unlock(&idx_lk);
      LOGW("meter.idx: holds more meters than this build can rewrite; the "
           "index for %d was NOT saved rather than dropping the others",
           id);
      return -1;
   }
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
       * Evict a row NO LIVE SLOT REFERENCES. Dropping ids[0] on the grounds
       * that it "belongs to a superseded id that nothing reads" is an
       * assumption nothing checks. Rows sit in the order they were first
       * written, so the oldest row is the FIRST meter ever registered, and if
       * that meter is still in use its index is the one thrown away: its next
       * sync sees no stored index, re-walks, and
       * re-appends fingersticks that are weeks old and therefore outside the
       * dedup window -- double-counted in the stats, permanently. */
      /* WHICH ROWS ARE LIVE IS THE REGISTRY'S QUESTION, and it is asked
       * here, inside idx_lk, only because the answer is used immediately. It
       * is safe in this order -- idx_lk is a leaf that the registry never
       * takes -- but it is the one call out of this file under a lock, so it
       * is worth naming. See test/app/lockorder.py, which checks the pair. */
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

   /* Write a fresh file and rename it over the live one, rather than
    * truncating in place. O_TRUNC destroys the stored indices BEFORE the new
    * ones are written, so a crash in that window leaves the file empty and the
    * next sync re-imports -- those fingersticks are typically weeks old, i.e.
    * outside the dedup window, so they are appended a second time and
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
static int index_all_locked(int *ids, int *vals, int cap, enum load_result *how,
                            int *overflowed)
{
   /* ONE EXACT READ: short reads, EINTR and a file longer than
    * this buffer are read_file_exact's problem, not this walk's.
    *
    * AND THE ANSWER IS TYPED. Untyped, "no index to give you" covers a
    * missing file, an unreadable one and a good one this meter is not in;
    * the first two want different actions from the third -- see
    * meter_index_load in meterstore.h -- so what happened travels. */
   char b[256];
   int n                     = 0;
   enum load_result how_read = read_file_exact(g_meter_path, b, sizeof b, &n);
   if (how)
      *how = how_read;
   if (overflowed)
      *overflowed = 0;
   if (how_read != LOAD_OK)
      return 0;
   int cnt = 0;
   char *p = b;
   while (*p) {
      if (cnt >= cap) {
         /* MORE PAIRS THAN THE CALLER CAN HOLD. Silently keeping the first
          * `cap` is what makes a rewrite drop the rest, so this says so and
          * the rewrite refuses. */
         if (overflowed)
            *overflowed = 1;
         if (how)
            *how = LOAD_CORRUPT;
         break;
      }
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
      } else if (how) {
         /* A ROW THAT IS NOT THIS FORMAT. The rows that DID parse are still
          * answered from -- losing every meter's progress over one spliced
          * line would re-walk them all, which is the harm this file exists
          * to prevent -- but the file is not whole, and the caller is told */
         *how = LOAD_CORRUPT;
      }
      while (*p && *p != '\n')
         p++;
      if (*p == '\n')
         p++;
   }
   return cnt;
}

/* See meterstore.h for the four answers and why one int could not carry them.
 * `*out` is the index or -1 for "nothing stored for this meter" -- -1, not 0,
 * because index 0 is a real record and the sentinel must sit below every
 * valid index or the meter's first record is skipped. */
enum load_result meter_index_load(int id, int *out)
{
   int dummy = -1;
   if (!out)
      out = &dummy;
   *out = -1;
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   enum load_result how = LOAD_OK;
   mutex_lock(&idx_lk);
   int n = index_all_locked(ids, vals, MAX_SLOTS, &how, NULL);
   mutex_unlock(&idx_lk);
   if (how == LOAD_ERROR)
      return LOAD_ERROR; /* the file is there and did not answer */
   for (int i = 0; i < n; i++)
      if (ids[i] == id) {
         *out = vals[i];
         return how == LOAD_CORRUPT ? LOAD_CORRUPT : LOAD_OK;
      }
   if (n > 0)
      return how; /* a file in this format; this meter is not in it yet */
   if (how == LOAD_ABSENT)
      return LOAD_ABSENT; /* no file at all: a first run */

   /* MIGRATION: an install from before this file was keyed by sensor id
    * holds a bare integer. Parsing it as "id,index" yields nothing, so the
    * index would look unset and the meter would re-import its recent window
    * -- records typically weeks old, i.e. outside the in-memory dedup
    * window, so they would be appended to the lifetime log a second time.
    * Adopt the bare value for whichever meter asks first; that is exactly
    * right, because a file in that shape can only ever describe one meter.
    * The next save rewrites it with an id per row. */
   /* AND IT IS PARSED STRICTLY, because of what an index MEANS.
    *
    * `v = v * 10 + digit` into a signed int with no cutoff runs at
    * STARTUP, over a file a user can edit and a torn write can leave
    * half-formed. Two consequences, and the second is the one that costs
    * fingersticks:
    *
    *   - signed overflow is undefined behaviour, so a long digit run there is
    *     UB during launch, before any check can look at the value;
    *
    *   - and the value that comes out is not obviously wrong. `4294967297`
    *     wraps to exactly 1, which is a PERFECTLY LEGAL record index --
    *     nothing downstream refuses it -- so the app would publish "records 0
    *     and 1 are already imported" for a meter it had never read, and
    *     those two fingersticks would never be fetched. `4294967296` wraps to
    *     0 and loses the first one the same way. Measured on a lenient
    *     parser: 1, 0, -2147483648 and 1661992959 for four inputs that are
    *     all just digits.
    *
    * So: the WHOLE file must be one canonical unsigned decimal, and the value
    * must lie in the domain a OneTouch record index actually has.
    *
    * "Canonical" earns its keep. A loop that stops at the first non-digit
    * and keeps what it has adopts 123 from `123abc` and 12345 from
    * `0012345` -- a file that is not this format at all read as though it
    * were, its leading digits becoming a walk position. A migration path is
    * exactly where that must not happen: it runs once, silently, and what it
    * publishes is then written back in the new format and believed for ever.
    * A file we cannot read with certainty is better treated as absent (-1,
    * "walk from the beginning"), which costs one re-import of a bounded
    * window, than as a number we guessed. */
   /* ONE EXACT READ. The rule -- a FULL buffer means there may be more of
    * the file past it, and a prefix is not canonical input -- is
    * read_file_exact's EOF probe, which answers LOAD_CORRUPT for exactly
    * that case. */
   char b[32];
   int rn              = 0;
   enum load_result lr = read_file_exact(g_meter_path, b, sizeof b, &rn);
   if (lr != LOAD_OK)
      return lr;
   int end = rn;
   if (end > 0 && b[end - 1] == '\n')
      end--;
   if (end > 0 && b[end - 1] == '\r')
      end--;
   /* csvcur.h's reader, the same one sensors.c, insulin.c, weight.c and
    * plotdata.c use, rather than a sixth private digit loop: it caps the
    * accumulation at CSV_MAX_DIGITS into a LONG, which keeps the arithmetic
    * below defined, and it distinguishes "no digits" from "too many"
    * -- both of which are a refusal here, but for different reasons worth
    * being able to tell apart. */
   struct csv_cur c;
   enum csv_field why = CSV_FIELD_OK;
   csv_open(&c, b, b + end);
   long v = csv_num(&c, &why);
   if (why != CSV_FIELD_OK || !csv_at_end(&c))
      return LOAD_CORRUPT; /* empty, trailing rubbish, or too many digits */
   if (b[0] == '0' && end > 1)
      return LOAD_CORRUPT; /* leading zeros: not this format */
   /* THE REAL DOMAIN, and it is not int's. A record index goes out to the
    * meter in sixteen bits, so otble.c refuses a record counter above 0xFFFF
    * outright ("a counter above 0xFFFF can never be satisfied"): an index
    * wider than that names no record any OneTouch meter can be asked for. The
    * comparison is on `v` while it is still a long -- a bound applied after
    * the narrowing cast would be no bound at all, which is the entire defect
    * above. Nothing checks that this agrees with otble.c's literal. */
   if (v < 0 || v > 0xFFFF)
      return LOAD_CORRUPT;
   /* THE LEGACY FORMAT, POSITIVELY IDENTIFIED: the whole file is
    * one canonical unsigned decimal in the domain a record index has. Only
    * then is it adopted -- a file that is nearly that, or something else
    * entirely, is CORRUPT rather than a number to walk from. */
   LOGI("meter.idx: adopting legacy index %d for id %d", (int)v, id);
   *out = (int)v;
   return LOAD_OK;
}

void meter_store_paths(const char *index_path, const char *sync_path)
{
   str_snapshot(g_meter_path, sizeof g_meter_path, index_path);
   str_snapshot(g_metersync_path, sizeof g_metersync_path, sync_path);
}
