// SPDX-License-Identifier: GPL-3.0
// sensors.c --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* See sensors.h for why provenance and preferences live in separate files.
 * Both are plain CSV parsed by hand: this build is freestanding, so there is no
 * sscanf, and the parsers here stop at the first field they cannot read (the
 * same forgiving style as settings.c, which is what lets the schema grow). */
#include "sensors.h"
#include "clock.h"
#include "csvcur.h"  /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h" /* errno / ENOENT: a missing file is not a failure */
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

int slot_count(void)
{
   reg_lock();
   int n = g_nslot;
   reg_unlock();
   return n;
}

int sensor_slot_at(int idx, struct sensor_slot *out)
{
   int have = 0;
   reg_lock();
   if (idx >= 0 && idx < g_nslot) {
      have = 1;
      if (out)
         *out = g_slot[idx];
   } else if (out) {
      struct sensor_slot z = {0};
      *out                 = z;
   }
   reg_unlock();
   return have;
}

struct sensor_slot slot_at(int i)
{
   struct sensor_slot z = {0};
   reg_lock();
   if (i >= 0 && i < g_nslot)
      z = g_slot[i];
   reg_unlock();
   return z;
}

int srec_count(void)
{
   reg_lock();
   int n = g_nsrec;
   reg_unlock();
   return n;
}

struct sensor_rec srec_at(int i)
{
   struct sensor_rec z = {0};
   reg_lock();
   if (i >= 0 && i < g_nsrec)
      z = g_srec[i];
   reg_unlock();
   return z;
}

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
 * It used to be a hand-rolled copy of driver_lock(), "duplicated rather than
 * shared because sensors.c must stay free of any BLE dependency to build in
 * the host UI harness" -- which was the right constraint and the wrong
 * conclusion: the lock was never a BLE dependency, only the file it happened
 * to live in was. thread.h has no BLE in it, so there is one implementation of
 * a recursive lock in this app now instead of three that had already begun to
 * differ in their memory ordering. */
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

long sensor_session_len(int type)
{
   if (type == SENSOR_STELO)
      return 15L * 86400; /* Stelo: 15 days */
   if (type == SENSOR_G7)
      return 10L * 86400; /* G7: 10 days (plus a 12 h grace period) */
   return 0;
}

long sensor_wear_seconds(int type, int wear_days, const char *model)
{
   /* The user's explicit override wins outright. */
   if (wear_days == 10 || wear_days == 15)
      return wear_days * 86400L;
   /* Dexcom sells the G7 in 10-day and 15-day versions and the sensor never
    * states its wear length in any field we parse -- only the DIS model
    * distinguishes them. Judging a 15-day G7 against the 10-day default
    * declared it ENDED five days early, while it was visibly still
    * delivering. Models learned in the field; extend as they appear. */
   if (model && !strcmp(model, "SW14758"))
      return 15L * 86400; /* G7 15 Day */
   return sensor_session_len(type);
}

const char *sensor_type_name(int type)
{
   if (type <= SENSOR_NONE || type >= SENSOR_NTYPES)
      return "--";
   return type_names[type];
}

/* ---- the CSV cursor ----
 *
 * These five readers used to live here, and insulin.c and weight.c each kept
 * their own copy of two of them. They are app/csvcur.h now; what stayed here
 * is the GRAMMAR -- how many fields a provenance row has, which of them may
 * be absent, and what makes one a rejection -- because that is the part that
 * must not be shared with a different file format. */

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
 * THIS USED TO EVICT, and that was the defect. It dropped "the oldest row NO
 * LIVE SLOT references", which sounds conservative and is not: a live slot is
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
 * slots_save takes the lock itself and this runs inside it; the lock is
 * recursive, which is what lets the undo be part of the same critical
 * section as the change it undoes. */
struct slot_undo {
   struct sensor_slot slot[MAX_SLOTS];
   int n;
};

static void slots_snapshot(struct slot_undo *u)
{
   for (int i = 0; i < MAX_SLOTS; i++)
      u->slot[i] = g_slot[i];
   u->n = g_nslot;
}

static int slots_commit(const struct slot_undo *u)
{
   if (slots_save() == 0)
      return SENSOR_OK;
   for (int i = 0; i < MAX_SLOTS; i++)
      g_slot[i] = u->slot[i];
   g_nslot = u->n;
   return SENSOR_UNSAVED;
}

int sensor_set_marker(int id, int marker)
{
   struct slot_undo undo;
   if (marker < 0 || marker >= MARK_N)
      return -1;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return -1;
   }
   g_slot[idx].marker = marker;
   int rc             = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_set_color(int id, int color)
{
   struct slot_undo undo;
   if (color < 0 || color >= SET_NCOLORS)
      return -1;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return -1;
   }
   g_slot[idx].color = color;
   int rc            = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_set_size(int id, int size)
{
   struct slot_undo undo;
   if (size < 1 || size > MARK_SIZE_MAX)
      return -1;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return -1;
   }
   g_slot[idx].size = size;
   int rc           = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_cycle_size(int id)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return -1;
   }
   int nx           = g_slot[idx].size + 1;
   g_slot[idx].size = (nx > MARK_SIZE_MAX || nx < 1) ? 1 : nx;
   int rc           = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_cycle_wear(int id)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
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
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_set_label(int id, const char *name, int len)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
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
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_set_primary(int id)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx                    = slot_of_id_locked(id);
   const struct sensor_rec *r = idx >= 0 ? sensor_rec_by_id(id) : 0;
   /* A BGM must never own the big number: a hours-old fingerstick rendered as
    * the headline value (with a trend arrow) would actively mislead. An OLD
    * (disconnected) device cannot be primary either -- it is not streaming. */
   int rc = SENSOR_OK; /* nothing to do is not a failure */
   if (r && sensor_kind(r->type) == KIND_CGM && !g_slot[idx].old) {
      for (int i = 0; i < g_nslot; i++)
         g_slot[i].primary = (i == idx);
      rc = slots_commit(&undo);
   }
   reg_unlock();
   return rc;
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
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return SENSOR_UNSAVED; /* no such device: nothing was changed */
   }
   g_slot[idx].old     = 1;
   g_slot[idx].primary = 0;
   reassign_primary_locked();
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc;
}

int sensor_revive(int id)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
      return SENSOR_UNSAVED;
   }
   g_slot[idx].old = 0;
   /* If nothing else is primary and this is a CGM, it takes the big number. */
   const struct sensor_rec *r = sensor_rec_by_id(id);
   if (r && sensor_kind(r->type) == KIND_CGM && sensor_primary_slot() < 0)
      g_slot[idx].primary = 1;
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc;
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
    * The parser used to step over a missing separator and read an empty field
    * as 0, so a truncated or run-together row became a SHORTER row made of
    * whatever text remained -- accepted, pushed, and permanent. What it
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
   /* SPELLED OUT AGAINST CSV_FIELD_OK, not tested for truth. These were four
    * ints where non-zero meant good; they are now an enum whose OK member is
    * ZERO, so `!idok` would read as "this field was fine" and reject exactly
    * the rows it used to accept. */
   if (seps != 7 || idok != CSV_FIELD_OK || typeok != CSV_FIELD_OK ||
       actok != CSV_FIELD_OK || pairok != CSV_FIELD_OK)
      return -1;
   if (r.id <= 0 || r.activation < 0 || r.paired < 0)
      return -1;
   if (r.type <= SENSOR_NONE || r.type >= SENSOR_NTYPES)
      return -1; /* not a type this build can attribute a reading to */
   /* A row that will not FIT is a sensor whose readings can no longer be
    * attributed, which is the same loss to the user as a row that would not
    * PARSE -- so it is reported the same way, and the load comes back
    * incomplete rather than quietly short. */
   return srec_push(&r) ? 1 : -1;
}

/* Stream the WHOLE file, one line at a time.
 *
 * This used to read only the last 8 KB, which quietly defeated srec_push's
 * pinning: pinning protects rows a live slot references from eviction, but a
 * row outside the window was never loaded at all, so there was nothing to pin.
 * A provenance row is ~70 bytes, so past ~115 rows the meter's row -- minted
 * once, early -- simply vanished. sensor_reconcile then never recovered
 * g_meter_src and the meter SILENTLY STOPPED AUTO-SYNCING, permanently, which
 * is exactly the failure the pinning was added to prevent.
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
   static char buf[1025];
   long n = read(fd, buf, 1024);
   close(fd);
   if (n < 0)
      return -1;
   if (n == 0)
      return 0;
   buf[n]      = 0;
   int damaged = 0;
   /* A FILE THAT DOES NOT END IN A NEWLINE was cut while being written. The
    * slots file is rewritten whole (never appended to), so this means the
    * rewrite did not finish -- what follows may be a row, or half of one. */
   if (n > 0 && buf[n - 1] != '\n')
      damaged = 1;
   char *p = buf;
   while (*p && g_nslot < MAX_SLOTS) {
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
   /* Slots FIRST -- and it no longer MATTERS, which is the point worth
    * recording. srec_load() used to evict provenance rows and "protect" the
    * ones a live slot referenced, so the order was load-bearing: read second,
    * g_nslot was 0 throughout and nothing was ever protected. Nothing evicts
    * now, so no read of one file can change what the other keeps, and the
    * order below is merely the one that was already here. */
   reg_lock();
   int ok = slots_load() == 0;
   ok = (srec_load() == 0) && ok; /* both, always: each says its own piece */
   reg_unlock();
   return ok ? 0 : -1;
}

/* ---- old-device marker store ---- */

int slots_save(void)
{
   /* Build the WHOLE file, then rename it into place.
    *
    * This used to truncate the live file and then write one line per slot,
    * so anything that stopped the process mid-loop -- an OOM kill, a crash,
    * the battery -- left a registry holding some of the sensors and not the
    * rest. These are not "preferences": a lost slot is a G7 the user has to
    * pair again, key and all. The temp-then-rename here is the same shape
    * insulin.c already uses for its own rewrite; rename is atomic, so the
    * file on disk is always either the old registry or the new one. */
   reg_lock();
   char all[(MAX_SLOTS * 96) + 1];
   int used = 0;
   for (int i = 0; i < g_nslot && used < (int)sizeof all; i++) {
      int n =
          snprintf(all + used, sizeof all - (size_t)used,
                   "%d,%s,%d,%d,%d,%d,%d,%d\n", g_slot[i].id, g_slot[i].label,
                   g_slot[i].marker, g_slot[i].color, g_slot[i].primary,
                   g_slot[i].size, g_slot[i].wear_days, g_slot[i].old);
      if (n <= 0 || n >= (int)sizeof all - used)
         break; /* cannot describe the rest: keep the file we already have */
      used += n;
   }
   int ok = atomic_replace(g_slots_path, all, used) == REPLACE_FAILED ? -1 : 0;
   reg_unlock();
   if (ok == 0) {
      record_mutated(); /* slots.csv is synced too: see util.h */
      /* THIS IS THE DELETION WORKFLOW, and `used == 0` is what a deliberate
       * "remove the last device" looks like on disk: the whole registry is
       * rewritten every time, so an empty registry is a ZERO-BYTE FILE.
       *
       * The sync client cannot tell that file apart from a phone that lost
       * its storage, and it must not guess -- guessing wrong deletes the
       * server's copy of the record. So it refuses every empty log unless the
       * code that emptied it left evidence, and this is the code that emptied
       * it. Without this line the user removes their last sensor, the removal
       * never reaches the server, and sync_run stops at this log for ever,
       * taking the readings, doses and weights down with it.
       *
       * The answer is deliberately not folded into `ok`. REPLACE_UNSYNCED
       * means the tombstone IS on disk and readable -- only a power cut in
       * the next moments could lose it -- and the registry itself was already
       * written successfully, so reporting a failed save would be a lie about
       * the thing the caller actually asked for. A tombstone lost that way
       * costs one refused sync and is re-minted by the next slots_save. */
      if (used == 0)
         (void)log_note_cleared(g_slots_path);
      else
         (void)log_clear_forget(g_slots_path);
   }
   return ok;
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
   /* Scan, append and cache are ONE atomic step. Split apart, two threads
    * both read the same maxid and hand out the same id to two different
    * physical sensors -- which readings.csv then cites forever, with no way
    * to tell the two apart after the fact. */
   reg_lock();
   /* A physical device is identified by (type, identity/MAC) ALONE. serial,
    * model, fw and activation are LEARNED ATTRIBUTES, not identity.
    *
    * The reuse key used to also include (serial, model, fw), so the instant
    * a device's DIS was read -- which happens a few seconds AFTER its first
    * reading -- the bare mint (empty model/fw) and the with-model mint got
    * DIFFERENT ids. Everything logged under the bare id was then orphaned:
    * cited by an id no slot pointed at, drawn on the plot as gray crosses,
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
      reg_unlock();
      return id;
   }
   /* No slot yet, but a provenance row for this (type, MAC) already exists
    * (e.g. from an earlier launch, or from a device the user forgot and has
    * now re-paired): reuse its id rather than minting another.
    * activation/model/fw differences do NOT fork the id anymore.
    *
    * THIS SCAN IS NOW COMPLETE, which it was not: the table used to evict, so
    * a device whose row had been dropped was re-minted under a SECOND id and
    * everything it had logged before was orphaned from everything it logged
    * after -- one physical sensor, two identities, in a log nothing rewrites.
    * Nothing evicts now, so a row that has ever been read is still here to be
    * matched. */
   for (int i = 0; i < g_nsrec; i++) {
      const struct sensor_rec *r = &g_srec[i];
      if (r->type == type && !strcmp(r->identity, identity)) {
         int id = r->id;
         reg_unlock();
         return id;
      }
   }

   /* NO ROOM IS A REFUSAL, NOT AN EVICTION. The table is one entry per id and
    * holds every id this registry has ever named (MAX_SENSOR_RECS in
    * sensors.h argues the size, and why this is unreachable before the phone
    * is landfill). Full, there are only two things to do: forget an older
    * device's provenance -- which is the defect this table was rebuilt to end
    * -- or decline. Declining stops NEW data instead of unattributing OLD
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
      reg_unlock();
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
    * sensor shows as unregistered, which is visible, instead of quietly
    * borrowing another device's identity. Unreachable in practice (a few
    * mints per sensor per year), but it is an invariant, not an estimate. */
   if (maxid + 1 > 0xFFFF) {
      reg_unlock();
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

   int arc = srec_append_row(&r);
   if (arc != LOG_OK) {
      reg_unlock();
      /* PROVENANCE MUST BE DURABLE: refuse the id if it did not reach the
       * disk, or readings would cite a row nobody has. The write's own answer
       * travels, so LOG_DAMAGED (a partial row still in the file) is
       * distinguishable from a clean refusal. */
      return arc;
   }

   /* Cannot fail: the fullness check above ran under this same hold, and this
    * id is new, so it needs a row of its own and there is one. */
   srec_push(&r);
   reg_unlock();
   return r.id;
}

int sensor_complete(int id, const char *serial, const char *model,
                    const char *fw, long activation)
{
   reg_lock();
   /* Locate the row by INDEX and work on a copy: srec_push shifts the array
    * to keep it id-ordered, and the durable append must precede the in-memory
    * update so a failed write leaves the row still-incomplete and the caller
    * retries. */
   int found = 0;
   int idx   = srec_bisect(id, &found);
   if (!found) {
      reg_unlock();
      /* NO SUCH ROW -- which now means exactly that, and no longer "the row
       * exists but the cache dropped it". Nothing ages out, so a completion
       * for a device this registry knows always finds its row; only an id
       * that was never minted (or whose row the parser refused) lands here.
       * That distinction matters: a completion silently discarded because the
       * row had been evicted left a permanently bare provenance row -- no
       * model, no firmware, no session start -- for a sensor still in use. */
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
      reg_unlock();
      return 0;
   }
   if (srec_append_row(&r) < 0) {
      reg_unlock();
      return -1;
   }
   g_srec[idx] = r;
   reg_unlock();
   return 1;
}

/* Repoint the slot that shows `old_id` at `new_id`, keeping everything the
 * user chose about it -- label, marker, colour, primary, pin.
 *
 * A slot is the USER'S row: their name for a sensor and how it draws. A
 * sensor id is the registry's identity for one physical device. When a
 * device is replaced by one the registry considers distinct, the row should
 * follow the new device rather than the user having to re-decorate it.
 *
 * Returns 1 if a slot was repointed, 0 if no slot held old_id.
 *
 * NOTE: no production caller today -- the meter replacement path mints and
 * claims instead. It is kept because it is a correct, tested operation on
 * the registry (app/test/registrytest.c pins the marker/colour survival and
 * the unknown-id refusal), and because deleting a tested capability on the
 * strength of "nothing calls it this week" is how a repair path disappears.
 */
int sensor_rebind_slot(int old_id, int new_id)
{
   if (old_id <= 0 || new_id <= 0)
      return 0;
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   struct sensor_slot *sl = slot_ptr_by_id(old_id);
   if (!sl) {
      reg_unlock();
      return 0;
   }
   sl->id = new_id;
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc == SENSOR_OK; /* a rebind that was not written did not happen */
}

int sensor_claim_slot(int id, int type, const char *identity)
{
   if (id <= 0)
      return -1;
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   struct sensor_slot *have = slot_ptr_by_id(id);
   if (have) {
      int idx = (int)(have - g_slot);
      /* Re-adding a device that was DISCONNECTED revives its existing slot
       * -- keeping the marker/label/colour the user chose -- instead of
       * leaving it stranded as an old device with a duplicate live one. */
      if (have->old) {
         have->old = 0;
         if (sensor_kind(type) == KIND_CGM && sensor_primary_slot() < 0)
            have->primary = 1;
         if (slots_commit(&undo) != SENSOR_OK)
            idx = -1; /* the revival was not written: see below */
      }
      reg_unlock();
      return idx;
   }
   if (g_nslot >= MAX_SLOTS) {
      reg_unlock();
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
    * let commit_pair go on to erase a key file and bond a sensor whose slot
    * would be gone at the next launch -- the pairing then had to be done
    * again, with the old key already destroyed. -1 is the same answer as
    * "slots full", which every caller already handles. */
   int rc  = slots_commit(&undo);
   int idx = rc == SENSOR_OK ? g_nslot - 1 : -1;
   reg_unlock();
   return idx;
}

int sensor_forget(int id)
{
   struct slot_undo undo;
   reg_lock();
   slots_snapshot(&undo);
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_unlock();
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
   int rc = slots_commit(&undo);
   reg_unlock();
   return rc;
}

/* The registry's two files: the append-only provenance rows, and the slot
 * table that says which of them are worn right now. */
int sensors_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_sensors_path, sizeof g_sensors_path, dir, "/sensors.csv");
   ok &= data_path(g_slots_path, sizeof g_slots_path, dir, "/slots.csv");
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
