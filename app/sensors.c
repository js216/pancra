// SPDX-License-Identifier: GPL-3.0
// sensors.c --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* See sensors.h for why provenance and preferences live in separate files.
 * Both are plain CSV parsed by hand: this build is freestanding, so there is no
 * sscanf, and the parsers here stop at the first field they cannot read (the
 * same forgiving style as settings.c, which is what lets the schema grow). */
#include "sensors.h"
#include "clock.h"
#include "csvcur.h"     /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"    /* errno / ENOENT: a missing file is not a failure */
#include "log.h"        /* a refused registry says WHY, where it can be seen */
#include "sensorsint.h" /* the indexed reads defined here, for its tests */
#include "style.h"
#include "thread.h" /* rmutex: the ONLY cross-thread primitives */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h>  /* snprintf, SEEK_SET / SEEK_END */
#include <string.h> /* strcmp */

/* THE ATTRIBUTION TABLE: every id sensors.csv has ever named, sorted by id.
 * Nothing evicts from it -- see MAX_SENSOR_RECS in sensors.h for why, and for
 * the memory that buys. Sorted so the lookup can bisect: sensor_in_warmup is
 * called once per row by stats.c's chunk loader, and a linear scan of a table
 * this size would put tens of millions of comparisons into every recompute of
 * the daily average and time-in-range. */
static struct sensor_rec g_srec[MAX_SENSOR_RECS];
static int g_nsrec;
static struct sensor_slot g_slot[MAX_SLOTS];
static int g_nslot;

/* The registry lock, defined below with the reason it is private. */
static void reg_lock(void);
static void reg_unlock(void);

static char g_sensors_path[256];
static char g_slots_path[256];

static const char *const type_names[SENSOR_NTYPES] = {"--", "STELO", "G7",
                                                      "ONETOUCH"};

/* ---- registry lock ----
 *
 * The registry is mutated from more than one thread: sensor_reconcile() and
 * every UI action run on the main thread, while ot_drv_done() and the DIS
 * callbacks re-mint on a binder thread. Two concurrent sensor_mint() calls
 * both scan for maxid before either appends, so both return the SAME id -- and
 * the whole design rests on an id naming exactly one physical device forever.
 * The overlapping append also interleaves two half-written provenance rows.
 *
 * Recursive, because the mutators call each other (claim -> primary -> save).
 *
 * NOT a hand-rolled copy of driver_lock(), "duplicated rather than shared
 * because sensors.c must stay free of any BLE dependency to build in the host
 * UI harness" -- the right constraint and the wrong conclusion: the lock is
 * not a BLE dependency, only the file it happens to live in is. thread.h has
 * no BLE in it, so this app has ONE implementation of a recursive lock rather
 * than three that drift apart in their memory ordering. */
/* AND IT IS THIS MODULE'S, and only this module's.
 *
 * It was public (as sensors_lock/sensors_unlock), and eleven files took it by
 * hand around a count/index walk -- several of them without it at all, and at
 * least one across a call that takes the DRIVER's lock, which inverts the
 * documented driver -> registry order. Callers get a snapshot
 * (sensors_view_get) or an owned query now; neither can be held wrongly, and
 * neither can be forgotten. Makefile `lockcheck` keeps it that way.
 *
 * Taken AFTER the driver's and BEFORE the history's -- the registry is the
 * middle of that order. */
static struct rmutex reg_lk = RMUTEX_INIT;

/* ---- AND A SECOND LOCK, FOR THE FLASH -------------------------------------
 *
 * reg_lk is what a FRAME takes (through sensors_view_get) and what every
 * binder callback takes to mint or complete a row. Holding it across a write
 * to flash means the renderer waits for an fsync -- and on this phone that is
 * not a theoretical millisecond: the registry is rewritten whole on every
 * slot change and appended durably on every mint, both from binder threads,
 * while the 1 Hz repaint and the history are behind the same lock by the
 * documented driver -> registry -> history order.
 *
 * So the state lock guards the STATE and this one guards the FILE:
 *
 *   1. take regfile_lk;
 *   2. take reg_lk, render the bytes (or reserve the id) from the table,
 *      release reg_lk;
 *   3. write, fsync, rename -- with NO state lock held;
 *   4. take reg_lk again to publish the result, and release.
 *
 * HELD ACROSS THE WHOLE SEQUENCE, not just the write, and that is what keeps
 * the invariants the registry rests on:
 *
 *   - AN ID IS UNIQUE FOREVER. Two mints must not both read the same maxid.
 *     They cannot: the reservation happens under regfile_lk, so the second
 *     mint does not begin until the first has published its row.
 *   - A SAVE CANNOT WRITE A STALE TABLE. A caller that rendered behind an
 *     in-flight writer would otherwise overwrite the newer state with the
 *     older one and report success -- the same defect meterstore.h describes
 *     for the meter's last-sync file, which is why it is solved the same way.
 *
 * ORDER: regfile_lk OUTSIDE reg_lk, never the reverse, and no reader ever
 * takes it. app/thread.h carries the table and test/app/lockorder.py checks
 * the code against it. */
static struct mutex regfile_lk = MUTEX_INIT;

static void reg_lock(void)
{
   rmutex_lock(&reg_lk);
}

static void reg_unlock(void)
{
   rmutex_unlock(&reg_lk);
}

int sensor_kind(int type)
{
   return type == SENSOR_ONETOUCH ? KIND_BGM : KIND_CGM;
}

struct sensor_wear sensor_wear_of(int type, int wear_days, const char *model)
{
   struct sensor_wear w = {0, 0, 0};
   /* The user's explicit override wins outright -- and this is the ONE place
    * that decides which values are a valid override, so the label below
    * cannot disagree with the number beside it. */
   if (wear_days == 10 || wear_days == 15) {
      w.seconds = wear_days * 86400L;
      w.pinned  = 1;
      return w;
   }
   /* Dexcom sells the G7 in 10-day and 15-day versions and the sensor never
    * states its wear length in any field we parse -- only the DIS model
    * distinguishes them. Judging a 15-day G7 against the 10-day default
    * declared it ENDED five days early, while it was visibly still
    * delivering. Models learned in the field; extend as they appear. */
   if (model && !strcmp(model, "SW14758")) {
      w.seconds = 15L * 86400; /* G7 15 Day */
      return w;
   }
   w.seconds = sensor_session_len(type);
   /* The G7 is the type sold in two lengths, so its default is an assumption
    * until the DIS arrives; every other type has one length and its default
    * IS the answer. */
   w.provisional = type == SENSOR_G7 && !(model && model[0]);
   return w;
}

long sensor_wear_seconds(int type, int wear_days, const char *model)
{
   return sensor_wear_of(type, wear_days, model).seconds;
}

const char *sensor_type_name(int type)
{
   if (type <= SENSOR_NONE || type >= SENSOR_NTYPES)
      return "--";
   return type_names[type];
}

/* ---- the CSV cursor ----
 *
 * These five readers are in app/csvcur.h, shared with insulin.c and weight.c
 * rather than copied into each. What stays HERE is the GRAMMAR -- how many
 * fields a provenance row has, which of them may be absent, and what makes
 * one a rejection -- because that is the part that must not be shared with a
 * different file format. */

/* WHERE THIS ID SITS, OR WHERE IT WOULD GO. The table is kept sorted by id,
 * so this is a bisection: `*found` says whether the index it returns already
 * holds that id or is merely the place to insert it. Callers run under
 * reg_lock. */
static int srec_bisect(int id, int *found)
{
   int lo = 0;
   int hi = g_nsrec;
   while (lo < hi) {
      int mid = lo + ((hi - lo) / 2);
      if (g_srec[mid].id < id)
         lo = mid + 1;
      else
         hi = mid;
   }
   *found = (lo < g_nsrec && g_srec[lo].id == id);
   return lo;
}

/* TAKE a provenance row into the attribution table. 1 when it is held, 0 when
 * the table is FULL -- and full means REFUSED, never evicted.
 *
 * IT MUST NOT EVICT. Dropping "the oldest row NO LIVE SLOT references"
 * sounds conservative and is not: a live slot is
 * a device the user owns TODAY, while readings.csv is append-only and every
 * row in it cites a source id for ever. So the rows this chose to drop were
 * precisely the ones only HISTORY still needed. The reading stayed on disk;
 * the app simply stopped being able to say which physical sensor produced it,
 * for anything older than roughly a year of ordinary use. See MAX_SENSOR_RECS
 * in sensors.h for the arithmetic and for which of the consequences were
 * reachable -- a forgotten device re-paired under a SECOND id is the one that
 * needed no unusual setup at all.
 *
 * (The eviction rule had already been narrowed once, from "oldest" to "oldest
 * unreferenced", because a meter mints once and never again and its row aged
 * out while the meter was still in daily use. That fixed the symptom for
 * devices the user still owns and left it in place for every device they no
 * longer do -- which is most of what the log cites.)
 *
 * REFUSING instead is the honest end of the same rule: the caller reports it
 * (srec_parse_line makes the load DAMAGED, sensor_mint declines to mint), so
 * a full table is something the user is told about rather than something that
 * silently unattributes their history. See MAX_SENSOR_RECS in sensors.h for
 * why the bound is where it is and why it cannot be reached in practice. */
static int srec_push(const struct sensor_rec *r)
{
   /* Last row wins per id: sensor_complete() appends a corrected row for an
    * id that already has one, and on load the correction must supersede the
    * original -- IN PLACE, so one id never occupies two rows and the table
    * stays one entry per id, which is what makes its bound a count of
    * DEVICES rather than a count of file lines. */
   int found = 0;
   int at    = srec_bisect(r->id, &found);
   if (found) {
      g_srec[at] = *r;
      return 1;
   }
   if (g_nsrec >= MAX_SENSOR_RECS)
      return 0;
   /* The shift that every "do not hold a pointer into g_srec" comment in this
    * codebase is about. Minted ids climb, and the file is read in append
    * order, so in practice `at` is the end and nothing moves; a correction
    * row for an id whose original was refused, or a hand-edited file, is what
    * makes the general case necessary. */
   for (int i = g_nsrec; i > at; i--)
      g_srec[i] = g_srec[i - 1];
   g_srec[at] = *r;
   g_nsrec++;
   return 1;
}

/* ---- lookups ---- */

static const struct sensor_rec *sensor_rec_by_id(int id)
{
   int found = 0;
   int at    = srec_bisect(id, &found);
   return found ? &g_srec[at] : 0;
}

/* See sensors.h for why activation is the anchor and why this fails open. */
int sensor_in_warmup(int id, long t)
{
   int warm = 0;
   reg_lock();
   const struct sensor_rec *r = sensor_rec_by_id(id);
   if (r && r->activation > 0 && t >= r->activation &&
       t < r->activation + SENSOR_WARMUP_S)
      warm = 1;
   reg_unlock();
   return warm;
}

enum warm_state warm_of_state(int state)
{
   if (state == SENSOR_STATE_WARMUP)
      return WARM_YES;
   if (state == SENSOR_STATE_OK || state == SENSOR_STATE_ENDED)
      return WARM_NO;
   return WARM_UNKNOWN;
}

enum warm_verdict warm_decide(enum warm_state measured, int id, long t)
{
   if (measured == WARM_YES)
      return WARM_SKIP;
   if (measured == WARM_NO)
      return WARM_COUNT;
   /* Nothing measured. The inference still SKIPS what it can prove -- a
    * reading inside a known session's first hour is uncalibrated whoever is
    * asking -- and what it lets through is counted with a mark on it. */
   if (sensor_in_warmup(id, t))
      return WARM_SKIP;
   return WARM_COUNT_UNSURE;
}

/* PRIVATE, and the only place a pointer into the slot array exists. Every
 * public answer below is a copy or an index. */
static struct sensor_slot *slot_ptr_by_id(int id)
{
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].id == id)
         return &g_slot[i];
   return 0;
}

void sensors_view_get(struct sensor_view *out)
{
   if (!out)
      return;
   reg_lock();
   out->n = g_nslot < MAX_SLOTS ? g_nslot : MAX_SLOTS;
   for (int i = 0; i < out->n; i++) {
      out->slot[i]               = g_slot[i];
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      out->have_rec[i]           = r != 0;
      if (r)
         out->rec[i] = *r;
      else
         out->rec[i] = (struct sensor_rec){0};
   }
   reg_unlock();
}

int sensor_id_is_live(int id)
{
   int live = 0;
   reg_lock();
   for (int i = 0; i < g_nslot && !live; i++)
      if (g_slot[i].id == id)
         live = 1;
   reg_unlock();
   return live;
}

int sensor_slot_of(int id, struct sensor_slot *out)
{
   reg_lock();
   const struct sensor_slot *s = slot_ptr_by_id(id);
   int have                    = s != 0;
   if (s && out)
      *out = *s;
   reg_unlock();
   return have;
}

int sensor_rec_of(int id, struct sensor_rec *out)
{
   reg_lock();
   const struct sensor_rec *r = sensor_rec_by_id(id);
   int have                   = r != 0;
   if (r && out)
      *out = *r;
   reg_unlock();
   return have;
}

int sensor_slot_by_mac(const char *identity)
{
   int at = -1;
   reg_lock();
   for (int i = 0; i < g_nslot && at < 0; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && !strcmp(r->identity, identity))
         at = i;
   }
   reg_unlock();
   return at;
}

int sensor_hidden_ids(int *out, int max)
{
   /* The ids of slots whose marker is MARK_HIDE (the device is OFF the plot),
    * in one locked pass so a caller can flag hidden points without taking the
    * registry lock per point. There are at most MAX_SLOTS of them. */
   reg_lock();
   int n = 0;
   for (int i = 0; i < g_nslot && n < max; i++)
      if (g_slot[i].marker == MARK_HIDE)
         out[n++] = g_slot[i].id;
   reg_unlock();
   return n;
}

int sensor_primary_id(void)
{
   /* The primary's ID, resolved under the registry lock.
    *
    * hist_refresh_current() needs this while holding hist_lock. Reading g_slot
    * there directly was unsynchronized: sensor_forget's shift-down can
    * move the primary flag between the scan and the id load, yielding a
    * DIFFERENT sensor's id and binding the big number (and therefore the
    * alarm) to the wrong sensor. Taking reg_lock inside hist_lock would invert
    * the lock order instead, so callers resolve the id HERE first and pass it
    * in -- reg is a leaf, and reg-before-hist keeps the graph acyclic. */
   int id = -1;
   reg_lock();
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary) {
         id = g_slot[i].id;
         break;
      }
   reg_unlock();
   return id;
}

int sensor_primary_slot(void)
{
   int at = -1;
   reg_lock();
   for (int i = 0; i < g_nslot && at < 0; i++)
      if (g_slot[i].primary)
         at = i;
   reg_unlock();
   return at;
}

/* ---- per-device preferences (see sensors.h) ---- */

/* THE SLOT HOLDING AN ID, resolved INSIDE the lock the change is made under.
 *
 * This is the whole reason these operations take an id. An index is a
 * position, and a mint or a forget on a binder thread moves every position
 * after it: a caller that read "the selected device is slot 2" and then asked
 * to make slot 2 primary -- or to disconnect it -- could name a different
 * device by the time the call landed. An id names one physical device for
 * ever, so resolving it here, under the same lock, makes the lookup and the
 * change one step that nothing can slip between. -1 when no slot holds it. */
static int slot_of_id_locked(int id)
{
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].id == id)
         return i;
   return -1;
}

/* THE TRANSACTION, and it is the same three steps for every mutation: take a
 * copy of the table, change it, persist -- and on a failed persist put the
 * copy back, so memory and disk agree either way. See sensors.h.
 *
 * The rewrite takes the lock itself and this runs inside it; the lock is
 * recursive, which is what lets the undo be part of the same critical
 * section as the change it undoes. */
struct slot_undo {
   struct sensor_slot slot[MAX_SLOTS];
   int n;
};

#ifdef APP_FAULTS
static int g_fault_render_cap;

void sensors_fault_render_cap_set(int cap)
{
   g_fault_render_cap = cap;
}
#endif

static void slots_snapshot(struct slot_undo *u)
{
   for (int i = 0; i < MAX_SLOTS; i++)
      u->slot[i] = g_slot[i];
   u->n = g_nslot;
}

/* Defined with the rest of the file's persistence, below: the render is a
 * read of the table (caller holds reg_lk), the write is the flash half (no
 * state lock, regfile_lk held). */
static int slots_render_locked(char *out, size_t cap);
static int slots_write(const char *all, int used);

/* ---- THE THREE STEPS OF A MUTATION, so no caller has to spell them out --
 *
 * Every mutator below is: begin (both locks, file lock OUTSIDE), change the
 * table under the state lock, then either abort (nothing changed) or end
 * (render, release the state lock, write, undo on failure). The flash is
 * always outside reg_lk -- see regfile_lk -- and always inside regfile_lk, so
 * the render and the write cannot be interleaved by another mutator.
 *
 * reg_lk MUST BE HELD EXACTLY ONCE when reg_write_end runs, because it
 * releases it around the write. Every mutator holds it once: the helpers that
 * take it again (sensor_primary_slot, slot_of_id_locked's callers) release it
 * before the commit. */
static void reg_write_begin(void)
{
   mutex_lock(&regfile_lk);
   reg_lock();
}

/* Nothing was changed, so there is nothing to write. */
static void reg_write_abort(void)
{
   reg_unlock();
   mutex_unlock(&regfile_lk);
}

/* SENSOR_OK when the change is on disk; SENSOR_UNSAVED when the table has
 * been put back untouched. Releases both locks. */
static int reg_write_end(const struct slot_undo *u)
{
   char all[(MAX_SLOTS * 96) + 1];
   int used = slots_render_locked(all, sizeof all);
   reg_unlock();
   /* A RENDER THAT COULD NOT DESCRIBE THE TABLE PUBLISHES NOTHING (item
    * 288). The file on disk stays exactly as it was -- which still holds
    * every sensor -- and the change is undone below, so memory and the file
    * agree. Writing the prefix would agree with neither. */
   int ok = (used < 0) ? SENSOR_UNSAVED : slots_write(all, used);
   if (ok != 0) {
      /* THE UNDO RUNS UNDER THE STATE LOCK AGAIN, and it is still correct
       * because regfile_lk has been held throughout: no other mutation could
       * have landed between the render and here, so the copy `u` holds is
       * still the state this change was made against. */
      reg_lock();
      for (int i = 0; i < MAX_SLOTS; i++)
         g_slot[i] = u->slot[i];
      g_nslot = u->n;
      reg_unlock();
   }
   mutex_unlock(&regfile_lk);
   return ok == 0 ? SENSOR_OK : SENSOR_UNSAVED;
}

int sensor_set_marker(int id, int marker)
{
   struct slot_undo undo;
   if (marker < 0 || marker >= MARK_N)
      return -1;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].marker = marker;
   return reg_write_end(&undo);
}

int sensor_set_color(int id, int color)
{
   struct slot_undo undo;
   if (color < 0 || color >= SET_NCOLORS)
      return -1;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].color = color;
   return reg_write_end(&undo);
}

int sensor_set_size(int id, int size)
{
   struct slot_undo undo;
   if (size < 1 || size > MARK_SIZE_MAX)
      return -1;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].size = size;
   return reg_write_end(&undo);
}

int sensor_cycle_size(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   int nx           = g_slot[idx].size + 1;
   g_slot[idx].size = (nx > MARK_SIZE_MAX || nx < 1) ? 1 : nx;
   return reg_write_end(&undo);
}

int sensor_cycle_wear(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   int w = g_slot[idx].wear_days;
   /* AUTO -> 10 D -> 15 D -> AUTO. The middle step is what a user picks when
    * the app has guessed a 10-day budget for a 15-day sensor; AUTO hands the
    * decision back to the model/type rule. */
   if (w == 10)
      g_slot[idx].wear_days = 15;
   else if (w == 15)
      g_slot[idx].wear_days = 0;
   else
      g_slot[idx].wear_days = 10;
   return reg_write_end(&undo);
}

int sensor_set_label(int id, const char *name, int len)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   int k = 0;
   if (name)
      for (; k < len && k < (int)sizeof g_slot[0].label - 1; k++)
         g_slot[idx].label[k] = name[k];
   g_slot[idx].label[k] = 0;
   /* An all-blank name makes the device row unreadable, and that row is how a
    * user tells two identical sensors apart. */
   if (k == 0)
      (void)snprintf(g_slot[idx].label, sizeof g_slot[0].label, "SENSOR %d",
                     g_slot[idx].id);
   return reg_write_end(&undo);
}

int sensor_set_primary(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx                    = slot_of_id_locked(id);
   const struct sensor_rec *r = idx >= 0 ? sensor_rec_by_id(id) : 0;
   /* A BGM must never own the big number: a hours-old fingerstick rendered as
    * the headline value (with a trend arrow) would actively mislead. An OLD
    * (disconnected) device cannot be primary either -- it is not streaming. */
   if (!r || sensor_kind(r->type) != KIND_CGM || g_slot[idx].old) {
      reg_write_abort();
      return SENSOR_OK; /* nothing to do is not a failure */
   }
   for (int i = 0; i < g_nslot; i++)
      g_slot[i].primary = (i == idx);
   return reg_write_end(&undo);
}

int sensor_live_cgm_count(void)
{
   int n = 0;
   reg_lock();
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].old)
         continue;
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && sensor_kind(r->type) == KIND_CGM)
         n++;
   }
   reg_unlock();
   return n;
}

/* Hand the primary to the first LIVE CGM, if the current one is gone/old. */
static void reassign_primary_locked(void)
{
   int have = 0;
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary && !g_slot[i].old) {
         have = 1;
         break;
      }
   if (have)
      return;
   for (int i = 0; i < g_nslot; i++)
      g_slot[i].primary = 0;
   for (int i = 0; i < g_nslot; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (!g_slot[i].old && r && sensor_kind(r->type) == KIND_CGM) {
         g_slot[i].primary = 1;
         break;
      }
   }
}

int sensor_retire(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return SENSOR_UNSAVED; /* no such device: nothing was changed */
   }
   g_slot[idx].old     = 1;
   g_slot[idx].primary = 0;
   reassign_primary_locked();
   return reg_write_end(&undo);
}

int sensor_revive(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return SENSOR_UNSAVED;
   }
   g_slot[idx].old = 0;
   /* If nothing else is primary and this is a CGM, it takes the big number. */
   const struct sensor_rec *r = sensor_rec_by_id(id);
   if (r && sensor_kind(r->type) == KIND_CGM && sensor_primary_slot() < 0)
      g_slot[idx].primary = 1;
   return reg_write_end(&undo);
}

/* ---- load / save ---- */

/* Column header for sensors.csv, so an exported registry is self-describing.
 * A leading-'#' line parses to id 0 and is dropped by srec_parse_line. */
static const char g_sensors_hdr[] =
    "# id,type,mac,serial,model,fw,activation_time,paired_time\n";

/* Provenance rows are appended in id order, so reading only the tail still
 * yields the highest id -- which is all minting needs. */
/* 1 = a row was taken, 0 = there was nothing to take (a header or a blank
 * line), -1 = A ROW WAS REJECTED.
 *
 * The third answer is what was missing. PROVENANCE is what this file holds --
 * which physical sensor each reading in the permanent log came from -- and a
 * row skipped in silence means readings whose source can never be resolved
 * again, shown as though nothing were wrong. */
static int srec_parse_line(char *p, char *e)
{
   if (p >= e)
      return 0;
   if (*p == '#')
      return 0;
   /* EIGHT FIELDS, SEVEN COMMAS, A TYPE THIS BUILD KNOWS -- AND NOTHING
    * STUCK TO THE END. (A NINTH field, comma-separated, is a newer schema's
    * and is accepted unread; see the rule after the last field below.)
    *
    * A parser that steps over a missing separator and reads an empty field
    * as 0 turns a truncated or run-together row into a SHORTER row made of
    * whatever text remains -- accepted, pushed, and permanent. What it
    * describes is which physical sensor produced every reading in a log that
    * is never rewritten; a row whose type is garbage resolves to KIND_CGM
    * (sensor_kind's default), which is what decides whether a value can own
    * the big number, feed the alarm, and be calibrated against.
    *
    * So the grammar is exact: every field present, every separator present,
    * both numeric ids actually numeric, and the type one of this build's. An
    * unknown type is a REJECTION, not a CGM -- a future version's sensor read
    * by an older build is precisely the case that must not be guessed at. */
   struct sensor_rec r = {0};
   struct csv_cur c;
   csv_open(&c, p, e);
   enum csv_field idok   = CSV_FIELD_EMPTY;
   enum csv_field typeok = CSV_FIELD_EMPTY;
   enum csv_field actok  = CSV_FIELD_EMPTY;
   enum csv_field pairok = CSV_FIELD_EMPTY;
   int seps              = 0;
   r.id                  = (int)csv_num(&c, &idok);
   seps += csv_sep(&c);
   r.type = (int)csv_num(&c, &typeok);
   seps += csv_sep(&c);
   csv_str(&c, r.identity, (int)sizeof r.identity);
   seps += csv_sep(&c);
   csv_str(&c, r.serial, (int)sizeof r.serial);
   seps += csv_sep(&c);
   csv_str(&c, r.model, (int)sizeof r.model);
   seps += csv_sep(&c);
   csv_str(&c, r.fw, (int)sizeof r.fw);
   seps += csv_sep(&c);
   r.activation = csv_num(&c, &actok);
   seps += csv_sep(&c);
   r.paired = csv_num(&c, &pairok);
   /* AND THE FIELD ENDS WHERE THE ROW OR THE NEXT SEPARATOR DOES.
    *
    * The grammar was exact through the last field and then simply stopped
    * looking, so "...,200junk" parsed as a valid row with the junk ignored --
    * accepted, pushed, and PERMANENT. A number with letters stuck to it is
    * not the number; the row it came from is one this reader cannot account
    * for, and accounting for rows is the whole purpose of this file.
    *
    * A NINTH FIELD IS NOT JUNK. The format grows by appending columns, and an
    * older build must keep reading rows a newer one writes -- rejecting a
    * longer row would turn the next schema addition into permanent data loss
    * on every phone not yet updated. The two are told apart by the byte that
    * follows the last number this build knows: a ',' begins a field that is
    * not ours to judge, and anything else is text stuck to a number.
    *
    * The caller has already split on '\n', so `e` is the end of the row; a
    * trailing '\r' from a file that crossed platforms is the line ending, not
    * content. */
   if (c.p < e && *c.p == '\r')
      c.p++;
   if (!csv_at_end(&c) && *c.p != ',')
      return -1;
   /* SPELLED OUT AGAINST CSV_FIELD_OK, not tested for truth. As four ints
    * non-zero would mean good; this is an enum whose OK member is ZERO, so
    * `!idok` reads as "this field was fine" and rejects exactly the rows it
    * should accept. */
   if (seps != 7 || idok != CSV_FIELD_OK || typeok != CSV_FIELD_OK ||
       actok != CSV_FIELD_OK || pairok != CSV_FIELD_OK)
      return -1;
   if (r.id <= 0 || r.activation < 0 || r.paired < 0)
      return -1;
   if (r.type <= SENSOR_NONE || r.type >= SENSOR_NTYPES)
      return -1; /* not a type this build can attribute a reading to */
   /* A row that will not FIT is a sensor whose readings cannot be
    * attributed, which is the same loss to the user as a row that would not
    * PARSE -- so it is reported the same way, and the load comes back
    * incomplete rather than quietly short. */
   return srec_push(&r) ? 1 : -1;
}

/* Stream the WHOLE file, one line at a time.
 *
 * Reading only the last 8 KB quietly defeats srec_push's pinning: pinning
 * protects rows a live slot references from eviction, but a row outside the
 * window is never loaded at all, so there is nothing to pin. A provenance row
 * is ~70 bytes, so past ~115 rows the meter's row -- minted once, early --
 * simply vanishes. sensor_reconcile then never recovers g_meter_src and the
 * meter SILENTLY STOPS AUTO-SYNCING, permanently -- exactly the failure the
 * pinning exists to prevent.
 *
 * Streaming is affordable: the file grows about one row per sensor session
 * (~1.7 KB/year), so even decades of use is a single sub-100 KB scan at
 * startup, and the table holds one entry per ID rather than one per line --
 * every sensor_complete correction lands back on the row it corrects. */
/* 0 when read whole (including "no file yet"), -1 when a read failed partway.
 * What a short read loses here is PROVENANCE: which physical sensor each
 * historical reading came from, and the model/firmware/activation the app
 * mints new rows against. */
static int srec_load(void)
{
   g_nsrec = 0;
   int fd  = open(g_sensors_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char buf[1024];
   char line[256];
   int llen = 0;
   int over = 0; /* this line exceeded the buffer: skip it rather than truncate,
                  * since a truncated row parses as a DIFFERENT sensor */
   long n      = 0;
   int damaged = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            /* Longer than any row can be, or a row that does not parse:
             * either way a sensor's provenance is missing and the caller has
             * to be told. */
            if (over || srec_parse_line(line, line + llen) < 0)
               damaged = 1;
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0) {
      /* NO TRAILING NEWLINE: cut while being appended to, and NOT parsed. A
       * truncated provenance row still parses -- the fields are positional
       * and a missing one reads as zero -- so it would mint a sensor with a
       * real id and half an identity, which every later reading is then
       * attributed to. */
      damaged = 1;
   }
   close(fd);
   return (n < 0 || damaged) ? -1 : 0;
}

/* 0 when read whole, -1 when the read failed. A short slot table is a sensor
 * the user has to pair again, key and all. */
static int slots_load(void)
{
   g_nslot = 0;
   int fd  = open(g_slots_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   /* ONE BYTE MORE THAN CAN BE KEPT, so a file that does not fit says so.
    *
    * This read 1024 into a 1025-byte buffer and NUL-terminated whatever came
    * back. A file of EXACTLY 1024 bytes ending in a newline then looked
    * perfect -- it ended in a newline, so the damage check below passed -- and
    * every slot after byte 1024 was silently dropped. What that costs is a
    * paired sensor disappearing from the registry with no error anywhere:
    * the user is told to pair it again, key and all, and nothing says why.
    *
    * Asking for the whole buffer turns "it did not fit" into a fact this
    * function can see. A short slot table is a REPORTED failure here (-1),
    * which is the same answer a read error gets, because publishing part of a
    * registry is worse than publishing none of it. */
   static char buf[1025];
   long n = read(fd, buf, sizeof buf);
   close(fd);
   if (n < 0)
      return -1;
   if (n == 0)
      return 0;
   if (n >= (long)sizeof buf) {
      LOGW("slots: %s is larger than the %d bytes this build can load; "
           "REFUSING it rather than publishing a registry missing its tail",
           g_slots_path, (int)sizeof buf - 1);
      return -1;
   }
   buf[n]      = 0;
   int damaged = 0;
   /* A FILE THAT DOES NOT END IN A NEWLINE was cut while being written. The
    * slots file is rewritten whole (never appended to), so this means the
    * rewrite did not finish -- what follows may be a row, or half of one. */
   if (n > 0 && buf[n - 1] != '\n')
      damaged = 1;
   char *p = buf;
   while (*p) {
      /* MORE ROWS THAN THERE ARE SLOTS is the same failure by another route:
       * stopping quietly at MAX_SLOTS publishes a registry that is missing
       * sensors the file describes. */
      if (g_nslot >= MAX_SLOTS) {
         LOGW("slots: %s holds more than %d sensors; REFUSING it rather than "
              "publishing a registry missing the rest",
              g_slots_path, MAX_SLOTS);
         g_nslot = 0;
         return -1;
      }
      char *e = p;
      while (*e && *e != '\n')
         e++;
      struct sensor_slot s = {0};
      struct csv_cur c;
      csv_open(&c, p, e);
      /* NO `why` ON ANY OF THESE, deliberately: this file GROWS COLUMNS, and
       * every field from the sixth on is absent in a file written by an older
       * build. An empty field reading as 0 is exactly the migration -- 0 is
       * spelled as the default of each one -- so "was there a number?" is a
       * question this format has already answered with "not necessarily". The
       * row is accepted or rejected on `s.id` alone, below. */
      s.id = (int)csv_num(&c, 0);
      csv_sep(&c);
      csv_str(&c, s.label, (int)sizeof s.label);
      csv_sep(&c);
      s.marker = (int)csv_num(&c, 0);
      csv_sep(&c);
      s.color = (int)csv_num(&c, 0);
      csv_sep(&c);
      s.primary = (int)csv_num(&c, 0) ? 1 : 0;
      csv_sep(&c);
      s.size = (int)csv_num(&c, 0); /* 6th; absent in pre-size files -> 0 */
      csv_sep(&c);
      /* 7th field; absent in older files -> 0 = resolve by model/type. */
      s.wear_days = (int)csv_num(&c, 0);
      csv_sep(&c);
      /* 8th field; absent in older files -> 0 = live (not an old device). */
      s.old = (int)csv_num(&c, 0) ? 1 : 0;
      if (s.id > 0) {
         if (s.marker < 0 || s.marker >= MARK_N)
            s.marker = MARK_SQUARE_F;
         if (s.marker == MARK_DOT) /* DOT dropped -> its identical twin */
            s.marker = MARK_SQUARE_F;
         if (s.color < 0 || s.color > 6)
            s.color = 0;
         if (s.size < 1 || s.size > MARK_SIZE_MAX)
            s.size = MARK_SIZE_DEF; /* default / migrate old files */
         if (s.wear_days != 10 && s.wear_days != 15)
            s.wear_days = 0; /* anything else means "not overridden" */
         if (s.old)
            s.primary = 0; /* an old device can never be the primary */
         g_slot[g_nslot++] = s;
      } else if (c.p != p) {
         /* A ROW WITH NO ID is not a device: skipped, and reported. Every
          * per-device preference -- its name, its colour, whether it is the
          * primary -- lives here, so a row lost in silence is a device that
          * quietly reverts to defaults. */
         damaged = 1;
      }
      p = (*e == '\n') ? e + 1 : e;
   }
   /* MORE SLOTS IN THE FILE THAN THE APP HOLDS: the tail was not read, which
    * is exactly the "silently short" case. */
   if (*p)
      damaged = 1;
   /* At most one primary can survive a hand-edited file. */
   int seen = 0;
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].primary && seen)
         g_slot[i].primary = 0;
      if (g_slot[i].primary)
         seen = 1;
   }
   return damaged ? -1 : 0;
}

int sensors_load(void)
{
   /* Slots FIRST -- and it does not MATTER, which is the point worth
    * recording. An srec_load() that evicted provenance rows and "protected"
    * the ones a live slot referenced would make the order load-bearing: read
    * second, g_nslot is 0 throughout and nothing is protected. Nothing
    * evicts, so no read of one file can change what the other keeps, and the
    * order below is merely conventional.
    *
    * BOTH LOCKS, file first, for the reason every writer takes both: a save
    * or a mint landing between the two reads below would write one file from
    * a table the other read has not filled yet. Reading holds reg_lk because
    * the parse fills the tables directly -- there is no intermediate copy to
    * take -- and a load is a startup or a restore, not a frame. */
   mutex_lock(&regfile_lk);
   reg_lock();
   int ok = slots_load() == 0;
   ok = (srec_load() == 0) && ok; /* both, always: each says its own piece */
   reg_unlock();
   mutex_unlock(&regfile_lk);
   return ok ? 0 : -1;
}

/* ---- old-device marker store ---- */

/* THE WHOLE TABLE AS BYTES, rendered under reg_lk by a caller that holds it.
 * Returns how many bytes `out` holds. A slot that cannot be described stops
 * the render: the file already on disk is better than a truncated one. */
/* THE WHOLE TABLE OR NOTHING. Answers the number of bytes when
 * every slot was described, and -1 when one of them could not be.
 *
 * WHY A PREFIX IS NOT AN ANSWER. A loop that `break`s on a row that will not
 * fit and returns what it has hands both callers something they atomically
 * replace the registry with, and report success for. A registry of eight
 * sensors silently becomes a registry of six, permanently: the file is the
 * record, the missing rows are gone, and what the user sees is two sensors
 * they have to
 * pair again, key and all. It is the same buffer for every row, so the case
 * is not exotic -- a long label is all it takes.
 *
 * The accounting is util.h's textout now, which is sticky: the first row that
 * does not fit poisons the builder and nothing after it is written. This
 * function's job is to pass that verdict on rather than to hand back a
 * prefix. */
static int slots_render_locked(char *out, size_t cap)
{
#ifdef APP_FAULTS
   /* A BUFFER TOO SMALL FOR THE TABLE, on demand. With MAX_SLOTS at ten and
    * a label of nineteen bytes the real buffer cannot overflow -- which is
    * exactly why the old `break` survived review: the case is unreachable
    * TODAY, and the file it would silently shorten is the one that says which
    * sensors this phone is paired to. One more slot, or a longer label, and
    * it is reachable. Nothing that ships defines APP_FAULTS. */
   if (g_fault_render_cap > 0 && (size_t)g_fault_render_cap < cap)
      cap = (size_t)g_fault_render_cap;
#endif
   struct textout to;
   tout_init(&to, out, (int)cap);
   for (int i = 0; i < g_nslot; i++) {
      int room = 0;
      char *at = tout_room(&to, &room);
      if (!at) {
         /* NO ROOM LEFT AND ROWS STILL TO WRITE is the same failure as a row
          * that does not fit: what would be published is short. */
         return -1;
      }
      tout_took(&to,
                snprintf(at, (size_t)room, "%d,%s,%d,%d,%d,%d,%d,%d\n",
                         g_slot[i].id, g_slot[i].label, g_slot[i].marker,
                         g_slot[i].color, g_slot[i].primary, g_slot[i].size,
                         g_slot[i].wear_days, g_slot[i].old));
   }
   return tout_ok(&to) ? to.len : -1;
}

/* THE FLASH HALF, with NO state lock held (regfile_lk is, see there). 0 when
 * the registry is on disk. */
static int slots_write(const char *all, int used)
{
   int ok = atomic_replace(g_slots_path, all, used) == REPLACE_FAILED ? -1 : 0;
   if (ok != 0)
      return ok;
   record_mutated(); /* slots.csv is synced too: see util.h */
   /* THIS IS THE DELETION WORKFLOW, and `used == 0` is what a deliberate
    * "remove the last device" looks like on disk: the whole registry is
    * rewritten every time, so an empty registry is a ZERO-BYTE FILE.
    *
    * The sync client cannot tell that file apart from a phone that lost its
    * storage, and it must not guess -- guessing wrong deletes the server's
    * copy of the record. So it refuses every empty log unless the code that
    * emptied it left evidence, and this is the code that emptied it. Without
    * this the user removes their last sensor, the removal never reaches the
    * server, and sync_run stops at this log for ever, taking the readings,
    * doses and weights down with it.
    *
    * The answer is deliberately not folded into `ok`. REPLACE_UNSYNCED means
    * the tombstone IS on disk and readable -- only a power cut in the next
    * moments could lose it -- and the registry itself was already written
    * successfully, so reporting a failed save would be a lie about the thing
    * the caller actually asked for. A tombstone lost that way costs one
    * refused sync and is re-minted by the next rewrite. */
   if (used == 0) {
      (void)log_note_cleared(g_slots_path);
   } else if (log_clear_forget(g_slots_path) != 0) {
      /* THE SAVE ITSELF SUCCEEDED, and that is what this function answers
       * for -- so this is not a failed save. What it is is a tombstone that
       * could not be removed beside a file that now has rows: evidence that
       * says this log was emptied on purpose when it was not, ready to be
       * believed by the next sync that finds the file short. It cannot be
       * fixed from here (the removal is what just failed), so it is reported
       * where somebody can see it and the save still stands. */
      LOGW("slots: a stale clear-tombstone could not be removed; a later "
           "sync may treat a short slots file as a deliberate clear");
   }
   return 0;
}

/* ---- minting ---- */

/* Append one provenance row durably. 0 on success, -1 on failure -- and on a
 * short write the partial line is rolled back: left in place it would merge
 * with the next append into one unparseable row, hiding an id from the
 * parser, after which maxid goes backwards and the NEXT mint reissues a live
 * id. */
static int srec_append_row(const struct sensor_rec *r)
{
   char b[192];
   int n = snprintf(b, sizeof b, "%d,%d,%s,%s,%s,%s,%ld,%ld\n", r->id, r->type,
                    r->identity, r->serial, r->model, r->fw, r->activation,
                    r->paired);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION, header included and the first row atomic with it: see
    * log_append. This file is the one where a lost or spliced row is worst --
    * an id hidden from the parser makes maxid go backwards, and the NEXT mint
    * reissues a LIVE id, merging two physical sensors' histories for ever. */
   int rc = log_append(g_sensors_path, g_sensors_hdr,
                       (int)sizeof g_sensors_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc;     /* LOG_DAMAGED travels: the file may hold a partial row */
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

int sensor_mint(int type, const char *identity, const char *serial,
                const char *model, const char *fw, long activation)
{
   if (!identity || !identity[0])
      return -1;
   const char *se = serial ? serial : "";
   const char *mo = model ? model : "";
   const char *fv = fw ? fw : "";
   /* Scan, append and cache are ONE atomic step -- but the step that must be
    * atomic is against other MINTS, not against readers. Two threads that
    * both read the same maxid hand out the same id to two different physical
    * sensors, which readings.csv then cites forever with no way to tell them
    * apart. regfile_lk is what makes that impossible: it is held from the
    * scan below through the durable append to the publish, so no second mint
    * can begin in between -- while reg_lk, which a FRAME takes, is released
    * across the flash. */
   reg_write_begin();
   /* A physical device is identified by (type, identity/MAC) ALONE. serial,
    * model, fw and activation are LEARNED ATTRIBUTES, not identity.
    *
    * A reuse key that also included (serial, model, fw) would split at the
    * instant a device's DIS is read -- a few seconds AFTER its first reading
    * -- because the bare mint (empty model/fw) and the with-model mint get
    * DIFFERENT ids. Everything logged under the bare id is then orphaned:
    * cited by an id no slot points at, drawn on the plot as gray crosses,
    * its history split off from the device the user still holds. Keying on
    * MAC alone makes an id map to one PHYSICAL device for life. A CGM
    * session always brings a new MAC, so sessions still separate cleanly; a
    * meter keeps one MAC, so all its fingersticks group under one id,
    * forever. */
   int slotidx = sensor_slot_by_mac(identity);
   if (slotidx >= 0) {
      /* Already tracked by a live slot: ALL of this device's readings belong
       * to that slot's id, no matter what model/fw we now report. This is
       * the pin that makes the split above impossible. */
      int id = g_slot[slotidx].id;
      reg_write_abort();
      return id;
   }
   /* No slot yet, but a provenance row for this (type, MAC) already exists
    * (e.g. from an earlier launch, or from a device the user forgot and has
    * now re-paired): reuse its id rather than minting another.
    * activation/model/fw differences do NOT fork the id anymore.
    *
    * THIS SCAN IS COMPLETE, and has to be: with an evicting table, a device
    * whose row was dropped is re-minted under a SECOND id and everything it
    * logged before is orphaned from everything it logs after -- one physical
    * sensor, two identities, in a log nothing rewrites. Nothing evicts, so a
    * row that has ever been read is still here to be matched. */
   for (int i = 0; i < g_nsrec; i++) {
      const struct sensor_rec *r = &g_srec[i];
      if (r->type == type && !strcmp(r->identity, identity)) {
         int id = r->id;
         reg_write_abort();
         return id;
      }
   }

   /* NO ROOM IS A REFUSAL, NOT AN EVICTION. The table is one entry per id and
    * holds every id this registry has ever named (MAX_SENSOR_RECS in
    * sensors.h argues the size, and why this is unreachable before the phone
    * is landfill). Full, there are only two things to do: forget an older
    * device's provenance -- the loss this table exists to prevent -- or
    * decline. Declining stops NEW data rather than unattributing OLD
    * data. The sensor then shows as unregistered and its readings are logged
    * as source 0, exactly like pre-registry data: UNATTRIBUTED, which is
    * honest, rather than MISATTRIBUTED, which is not. Checked BEFORE the
    * append, so the file never grows a row this process cannot resolve.
    *
    * AND IT IS WHAT KEEPS THE ID SCAN BELOW HONEST. maxid is read off the
    * table, so it is the highest id ever issued only while the table holds
    * every row that loaded. Nothing evicts, and a table that cannot take a
    * row mints nothing at all -- so the scan is never consulted in the one
    * state where it could be short, and an id readings.csv already cites can
    * never be handed out a second time. */
   if (g_nsrec >= MAX_SENSOR_RECS) {
      reg_write_abort();
      return -1;
   }

   int maxid = 0;
   for (int i = 0; i < g_nsrec; i++)
      if (g_srec[i].id > maxid)
         maxid = g_srec[i].id;

   /* An id must fit the 16-bit `src` field of struct reading (see store.h).
    * Past 65535 the narrowing cast wraps and id 65536 aliases legacy id 0,
    * so readings would be silently reattributed to a different physical
    * device -- the one failure this whole design exists to make impossible.
    * Refusing to mint stops new data rather than corrupting the record: the
    * sensor shows as unregistered, which is visible, rather than quietly
    * borrowing another device's identity. Unreachable in practice (a few
    * mints per sensor per year), but it is an invariant, not an estimate. */
   if (maxid + 1 > 0xFFFF) {
      reg_write_abort();
      return -1;
   }

   struct sensor_rec r = {0};
   r.id                = maxid + 1;
   r.type              = type;
   r.activation        = activation;
   r.paired            = realtime_s();
   str_snapshot(r.identity, sizeof r.identity, identity);
   str_snapshot(r.serial, sizeof r.serial, se);
   str_snapshot(r.model, sizeof r.model, mo);
   str_snapshot(r.fw, sizeof r.fw, fv);

   /* THE STATE LOCK GOES DOWN FOR THE FLASH, the file lock stays up. `r` is a
    * local, so the append below reads no registry state, and a frame drawing
    * the device list does not wait for an fsync. */
   reg_unlock();
   int arc = srec_append_row(&r);
   if (arc != LOG_OK) {
      mutex_unlock(&regfile_lk);
      /* PROVENANCE MUST BE DURABLE: refuse the id if it did not reach the
       * disk, or readings would cite a row nobody has. The write's own answer
       * travels, so LOG_DAMAGED (a partial row still in the file) is
       * distinguishable from a clean refusal. Nothing was published, and the
       * id is not consumed: the next mint scans the same maxid. */
      return arc;
   }

   /* PUBLISHED UNDER THE STATE LOCK, AFTER THE DISK. Cannot fail: the
    * fullness check ran under regfile_lk, which has been held since, so no
    * other mint has taken the room this row needs. */
   reg_lock();
   srec_push(&r);
   reg_unlock();
   mutex_unlock(&regfile_lk);
   return r.id;
}

int sensor_complete(int id, const char *serial, const char *model,
                    const char *fw, long activation)
{
   /* BOTH LOCKS, file first. The append below is durable, so it runs with
    * reg_lk released -- and regfile_lk held across the read, the write and
    * the publish is what stops a second completion of the same row from
    * interleaving with this one. */
   reg_write_begin();
   /* Locate the row by INDEX and work on a copy: srec_push shifts the array
    * to keep it id-ordered, and the durable append must precede the in-memory
    * update so a failed write leaves the row still-incomplete and the caller
    * retries. */
   int found = 0;
   int idx   = srec_bisect(id, &found);
   if (!found) {
      reg_write_abort();
      /* NO SUCH ROW -- which means exactly that, and never "the row exists
       * but the cache dropped it". Nothing ages out, so a completion for a
       * device this registry knows always finds its row; only an id that was
       * never minted (or whose row the parser refused) lands here. That
       * distinction matters: a completion silently discarded because the row
       * was evicted leaves a permanently bare provenance row -- no model, no
       * firmware, no session start -- for a sensor still in use. */
      return 0;
   }
   struct sensor_rec r = g_srec[idx];
   int changed         = 0;
   if (!r.serial[0] && serial && serial[0]) {
      str_snapshot(r.serial, sizeof r.serial, serial);
      changed = 1;
   }
   if (!r.model[0] && model && model[0]) {
      str_snapshot(r.model, sizeof r.model, model);
      changed = 1;
   }
   if (!r.fw[0] && fw && fw[0]) {
      str_snapshot(r.fw, sizeof r.fw, fw);
      changed = 1;
   }
   if (!r.activation && activation) {
      r.activation = activation;
      changed      = 1;
   }
   if (!changed) {
      reg_write_abort();
      return 0;
   }
   /* THE DISK FIRST, WITH NO STATE LOCK. `r` is a local copy, so nothing
    * below reads the table until the row is durable -- and a failed write
    * leaves the row still-incomplete, which is what makes the caller's retry
    * correct. */
   reg_unlock();
   int arc = srec_append_row(&r);
   reg_lock();
   if (arc < 0) {
      reg_write_abort();
      return -1;
   }
   /* THE INDEX IS RESOLVED AGAIN. srec_push shifts the array to keep it
    * id-ordered, and reg_lk was down across the append -- so the position
    * this row had before is not necessarily the position it has now. The ID
    * is what does not move. */
   int again = 0;
   int at    = srec_bisect(id, &again);
   if (again)
      g_srec[at] = r;
   reg_write_abort(); /* releases both; nothing further to write */
   return again ? 1 : 0;
}

int sensor_claim_slot(int id, int type, const char *identity)
{
   if (id <= 0)
      return -1;
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   struct sensor_slot *have = slot_ptr_by_id(id);
   if (have) {
      int idx = (int)(have - g_slot);
      /* Re-adding a device that was DISCONNECTED revives its existing slot
       * -- keeping the marker/label/colour the user chose -- rather than
       * leaving it stranded as an old device with a duplicate live one. */
      if (have->old) {
         have->old = 0;
         if (sensor_kind(type) == KIND_CGM && sensor_primary_slot() < 0)
            have->primary = 1;
         if (reg_write_end(&undo) != SENSOR_OK)
            idx = -1; /* the revival was not written: see below */
         return idx;  /* reg_write_end released both locks */
      }
      reg_write_abort();
      return idx;
   }
   if (g_nslot >= MAX_SLOTS) {
      reg_write_abort();
      return -1;
   }
   struct sensor_slot s = {0};
   s.id                 = id;
   s.marker             = MARK_SQUARE_F; /* DOT dropped; identical to this */
   s.size               = MARK_SIZE_DEF;
   /* The primary (first) device defaults to WHITE (index 6) -- the classic
    * main-trace colour; additional devices get a distinct colour each so
    * they are told apart at a glance. */
   s.color = (g_nslot == 0) ? 6 : ((g_nslot - 1) % 6);
   /* Default label is type + the last two MAC octets, so a freshly paired
    * sensor is never nameless and two meters are told apart on sight. */
   int n = 0;
   while (identity && identity[n])
      n++;
   const char *tail = (n >= 5) ? identity + n - 5 : "";
   (void)snprintf(s.label, sizeof s.label, "%s%s%s", sensor_type_name(type),
                  tail[0] ? "-" : "", tail[0] ? tail : "");
   for (int i = 0; s.label[i]; i++)
      if (s.label[i] == ':')
         s.label[i] = '-';
   /* First CGM paired becomes primary, so the big number always has an
    * owner.
    */
   if (sensor_kind(type) == KIND_CGM && sensor_primary_slot() < 0)
      s.primary = 1;
   g_slot[g_nslot++] = s;
   /* A CLAIM THAT WAS NOT WRITTEN IS NOT A CLAIM. Returning the index anyway
    * lets commit_pair erase a key file and bond a sensor whose slot will be
    * gone at the next launch -- the pairing then has to be done again, with
    * the key it destroyed. -1 is the same answer as
    * "slots full", which every caller already handles. */
   int at  = g_nslot - 1; /* read before the undo can move it back */
   int rc  = reg_write_end(&undo);
   int idx = rc == SENSOR_OK ? at : -1;
   return idx;
}

int sensor_forget(int id)
{
   struct slot_undo undo;
   reg_write_begin();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return SENSOR_UNSAVED;
   }
   /* NOTE: the DISCONNECT flow uses sensor_retire (which keeps the slot
    * and its appearance); this hard-delete remains only for a true removal
    * and for the test suite. */
   int was_primary = g_slot[idx].primary;
   for (int i = idx + 1; i < g_nslot; i++)
      g_slot[i - 1] = g_slot[i];
   g_nslot--;
   /* Never leave the big number ownerless: hand it to the first CGM left. */
   if (was_primary)
      for (int i = 0; i < g_nslot; i++) {
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
         if (r && sensor_kind(r->type) == KIND_CGM) {
            g_slot[i].primary = 1;
            break;
         }
      }
   return reg_write_end(&undo);
}

/* The registry's two files: the append-only provenance rows, and the slot
 * table that says which of them are worn right now. */
int sensors_paths(const char *dir)
{
   int ok = 1;
   if (!(data_path(g_sensors_path, sizeof g_sensors_path, dir, "/sensors.csv")))
      ok = 0;
   if (!(data_path(g_slots_path, sizeof g_slots_path, dir, "/slots.csv")))
      ok = 0;
   return ok;
}

const char *sensors_path(void)
{
   return g_sensors_path;
}

const char *slots_path(void)
{
   return g_slots_path;
}

long sensor_session_len(int type)
{
   if (type == SENSOR_STELO)
      return 15L * 86400; /* Stelo: 15 days */
   if (type == SENSOR_G7)
      return 10L * 86400; /* G7: 10 days (plus a 12 h grace period) */
   return 0;
}
