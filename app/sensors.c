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
#include "log.h"     /* a refused registry says WHY, where it can be seen */
#include "style.h"
#include "thread.h" /* rmutex: the ONLY cross-thread primitives */
#include "util.h"
#include <stdatomic.h> /* the style map is published with a release store */
#include <stdint.h>    /* uintptr_t: the deliberate cast in sensors_view_put */
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h>  /* snprintf, SEEK_SET / SEEK_END */
#include <stdlib.h> /* malloc/free: the published view, and the load buffers */
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
 * FROM thread.h, not a hand-rolled copy of the driver's. Keeping sensors.c
 * free of any BLE dependency is the right constraint and does not require one:
 * a recursive mutex is not a BLE dependency, only the file it happens to sit
 * beside is. thread.h has no BLE in it, so the app has ONE implementation
 * rather than several that drift apart in their memory ordering. */
/* AND IT IS THIS MODULE'S, and only this module's.
 *
 * EXPORTED, this lock is taken by hand around a count/index walk in every file
 * that reads the registry -- some of them not at all, and some across a call
 * that takes the DRIVER's lock, which inverts the documented driver -> registry
 * order. Callers get a reference to the published view (sensors_view_ref) or an
 * owned query instead; neither can be held wrongly, and neither can be
 * forgotten.
 *
 * Taken AFTER the driver's and BEFORE the history's -- the registry is the
 * middle of that order. */
static struct rmutex reg_lk = RMUTEX_INIT;

/* ---- AND A SECOND LOCK, FOR THE FLASH -------------------------------------
 *
 * reg_lk is what a PUBLISH runs under -- the frame itself only takes a
 * reference to what was published -- and what every binder callback takes to
 * mint or complete a row. Holding it across a write
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
 * takes it. app/thread.h carries the table. */
static struct mutex regfile_lk = MUTEX_INIT;

static void reg_lock(void)
{
   rmutex_lock(&reg_lk);
}

static void reg_unlock(void)
{
   rmutex_unlock(&reg_lk);
}

/* THE KIND OF A TYPE. Note what type 0 answers: KIND_CGM, because
 * SENSOR_STELO is the first of the enum. Callers that may be holding a slot
 * with NO provenance row -- which is every slot after a failed sensors.csv
 * read -- must test have_rec before asking, or a registered meter reads as a
 * CGM. Everything on the hot paths does (alarm_gather, sensor_reconcile,
 * meter_alloc_link, meter_sync_watchdog); the device row is display only. */
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

/* ---- THE PUBLISHED VIEW -------------------------------------------------
 *
 * ONE COPY PER CHANGE, not one per read.
 *
 * A reader needs a picture of the registry that cannot shift under it while it
 * calls the driver -- that is what stops the registry lock being held across
 * driver_* and inverting the documented order (app/thread.h). Copying the
 * whole table into the caller's frame bought exactly that, and cost 4 KB and
 * a walk on every read: several times a second on the advert path, three
 * times per reconcile tick, once per drawn frame.
 *
 * The picture does not change between mutations, so it is built once when the
 * registry changes and handed out by pointer until it changes again. The
 * table has ONE choke point -- every mutator runs between reg_write_begin and
 * reg_write_end -- so "when it changes" is a place, not a discipline.
 *
 * IMMUTABLE ONCE PUBLISHED. A reader holding a reference across a mutation
 * keeps reading the version it took, which is precisely the guarantee the
 * copy gave; the new version is published beside it and the old one is freed
 * when its last reader lets go.
 *
 * view_lk IS A LEAF AND THE ORDER IS reg_lk -> view_lk. The publisher already
 * holds reg_lk; a reader takes view_lk alone and never reaches for reg_lk
 * under it -- which is why ref() cannot build a view. It never has to: g_view
 * starts at a permanently-live empty node, so it is never null. */
/* THE NODE OWNS THE ROWS; the view POINTS at them.
 *
 * `v` is what readers get, and every pointer in it is const. The four aliases
 * below address the same bytes writably and belong to the publisher alone --
 * they are used to fill a node BEFORE it is visible to any reader, and never
 * afterwards. Keeping the writable names here rather than casting the const
 * away at the point of use is what makes "a published view never changes" a
 * property of the types rather than of everybody's care. */
struct view_node {
   struct sensor_view v;
   struct sensor_slot *w_slot;
   struct sensor_rec *w_rec;
   int *w_have;
   short *w_by_mac;
   int rc; /* references outstanding, guarded by view_lk */
};
static struct mutex view_lk = MUTEX_INIT;
/* Set when a publish could not allocate, cleared by the next one that can.
 * Written under reg_lk with the rest of the publish; read without it, which
 * is a plain int either way and one frame of lag at worst. */
static int g_view_stale;
/* Never freed and never counted down to zero: the view before the first
 * publish, and what a failed allocation falls back to. An empty registry is
 * the honest answer when there is nothing published -- the alternative is a
 * null every caller would have to test.
 *
 * Its row pointers are NULL and its three counts are 0, which is safe because
 * every reader's walk is bounded by one of those counts: an empty view has no
 * row to reach for. */
static struct view_node g_view_empty;
static struct view_node *g_view = &g_view_empty;

/* EVERY DEVICE'S PLOT STYLING, indexed by id and outliving its slot.
 *
 * Rebuilt beside the view, from the same choke points, because it answers the
 * same question at a different lifetime: the view is what is live NOW, and
 * this is how to draw anything the log has ever named. Three ints per id, and
 * the id space is the one the readings log cites (see SRC_LAST_MAX in
 * store.c) -- an id past the end simply has no styling, which draws as the
 * neutral trace rather than as somebody else's colour.
 *
 * SIZED FROM MAX_SLOTS, one past it because ids start at 1 -- but note what
 * that does NOT promise. An id is not an index into the slot table:
 * sensor_forget drops a slot while its provenance row stays, and the next
 * mint takes its id from the PROVENANCE table, so ids climb past the number
 * of slots in use. Past the end an id simply has no styling, which draws as
 * the neutral trace -- the guards below are what make that safe, not the
 * size.
 *
 * TWO MAPS AND AN INDEX, written under reg_lk and read without it. A rebuild
 * fills the map the readers are NOT using and then publishes it with one
 * release store; a reader takes the index once with an acquire load and reads
 * that map. So a frame drawn during ONE rebuild sees the whole previous map
 * or the whole new one, never a half-built one.
 *
 * TWO rebuilds while a reader sits between its index load and its read can
 * still overtake it -- the second one is refilling the map the reader chose.
 * The three style fields and `known` are written with no ordering between
 * them, so such a reader can see `known` set beside a half-updated entry and
 * draw a point in ANOTHER DEVICE'S colour, not merely an unstyled one. That
 * needs two registry mutations inside a handful of instructions and lasts one
 * frame; the alternative is registering readers, which is a great deal of
 * machinery for a wrong colour nobody would see.
 *
 * AND IT IS PER POINT, not per frame: each point asks separately, so a frame
 * spanning a publish can mix two maps rather than being uniformly one or the
 * other. That is a wider window than it sounds and still a smaller
 * consequence -- the styles differ only for devices whose colour just
 * changed, and the next frame is uniform.
 *
 * IN PLACE WOULD NEED A WINDOW. The rebuild has to forget ids that no longer
 * have a slot, so it starts by clearing -- and a clear applied to the live
 * map is a stretch of time in which every point on the plot reads as
 * unstyled. Rare and brief is exactly what makes such a flicker impossible to
 * diagnose later; a second map costs BSS that is never touched past the ids
 * in use. */
#define STYLE_MAX (MAX_SLOTS + 1)

struct style_ent {
   struct sensor_style st;
   /* KNOWN IS ITS OWN FIELD, not inferred from the values. A slot may
    * legitimately carry marker 0, colour 0 and size 0 (an unset size means
    * "use the default"), and reading that as "no such device" would draw a
    * real sensor's points as unattributed. */
   int known;
};
static struct style_ent g_style[2][STYLE_MAX];
/* Which of the two maps readers are on. Release on publish, acquire on read:
 * that pairing is what carries the map's contents to the reading thread. */
static _Atomic int g_style_live;

int sensor_style_of(int id, struct sensor_style *out)
{
   if (!out)
      return 0;
   *out = (struct sensor_style){0};
   if (id <= 0 || id >= STYLE_MAX)
      return 0;
   const struct style_ent *m =
       g_style[atomic_load_explicit(&g_style_live, memory_order_acquire)];
   if (!m[id].known)
      return 0;
   *out = m[id].st;
   return 1;
}

/* Caller holds reg_lk.
 *
 * CLEARED FIRST, so an id that no longer has a slot stops being styled. A
 * retired device keeps its slot and therefore keeps its styling, which is the
 * common case; what this drops is a device that was FORGOTTEN outright, and
 * the plot's rule for one of those is the orphan look -- muted and crossed --
 * rather than the colour of a sensor the user no longer has. Leaving the
 * entry behind makes that rule unreachable. */
static void style_rebuild_locked(void)
{
   int next = !atomic_load_explicit(&g_style_live, memory_order_relaxed);
   struct style_ent *m = g_style[next];
   for (int i = 0; i < STYLE_MAX; i++)
      m[i].known = 0;
   for (int i = 0; i < g_nslot; i++) {
      int id = g_slot[i].id;
      if (id <= 0 || id >= STYLE_MAX)
         continue;
      m[id].st.marker = g_slot[i].marker;
      m[id].st.color  = g_slot[i].color;
      m[id].st.size   = g_slot[i].size;
      m[id].known     = 1;
   }
   /* PUBLISHED LAST, with a release: every store above is visible to any
    * reader that takes this index. */
   atomic_store_explicit(&g_style_live, next, memory_order_release);
}

/* Order by_mac[0, n) so the identities it points at ascend. Caller holds
 * reg_lk; the array is the view being built, which no reader can see yet. */
static void by_mac_sift(struct view_node *nd, int root, int n)
{
   const struct sensor_rec *rec = nd->v.rec;
   short *bm                    = nd->w_by_mac;
   for (;;) {
      int big = root;
      int l   = (2 * root) + 1;
      int r   = l + 1;
      if (l < n && strcmp(rec[bm[l]].identity, rec[bm[big]].identity) > 0)
         big = l;
      if (r < n && strcmp(rec[bm[r]].identity, rec[bm[big]].identity) > 0)
         big = r;
      if (big == root)
         return;
      short t  = bm[root];
      bm[root] = bm[big];
      bm[big]  = t;
      root     = big;
   }
}

static void by_mac_sort(struct view_node *nd)
{
   short *bm = nd->w_by_mac;
   int n     = nd->v.n_mac;
   for (int i = (n / 2) - 1; i >= 0; i--)
      by_mac_sift(nd, i, n);
   for (int end = n - 1; end > 0; end--) {
      short t = bm[0];
      bm[0]   = bm[end];
      bm[end] = t;
      by_mac_sift(nd, 0, end);
   }
}

/* Build the current table into `out`. Caller holds reg_lk. */
/* ONE ALLOCATION FOR THE NODE AND ITS `n` ROWS, laid out back to back.
 *
 * Each array starts at the next offset its own type can be addressed at --
 * rounded up rather than assumed, because "the previous array happened to end
 * on the right boundary" is a fact about today's field list and not about the
 * layout rule. NULL if there is no room; the caller keeps the view it has.
 *
 * n == 0 is legal: every array then has length zero, the pointers address the
 * end of the header, and no reader reaches them because every walk is bounded
 * by a count that is also zero. */
static size_t view_round(size_t off, size_t a)
{
   size_t r = off % a;
   return r ? off + (a - r) : off;
}

static struct view_node *view_alloc(int n)
{
   size_t sz = (size_t)n;
   size_t o_slot =
       view_round(sizeof(struct view_node), _Alignof(struct sensor_slot));
   size_t o_rec = view_round(o_slot + (sz * sizeof(struct sensor_slot)),
                             _Alignof(struct sensor_rec));
   size_t o_have =
       view_round(o_rec + (sz * sizeof(struct sensor_rec)), _Alignof(int));
   size_t o_bm  = view_round(o_have + (sz * sizeof(int)), _Alignof(short));
   size_t total = o_bm + (sz * sizeof(short));

   struct view_node *nd = malloc(total);
   if (!nd)
      return 0;
   char *base     = (char *)nd;
   nd->w_slot     = (struct sensor_slot *)(base + o_slot);
   nd->w_rec      = (struct sensor_rec *)(base + o_rec);
   nd->w_have     = (int *)(base + o_have);
   nd->w_by_mac   = (short *)(base + o_bm);
   nd->v.slot     = nd->w_slot;
   nd->v.rec      = nd->w_rec;
   nd->v.have_rec = nd->w_have;
   nd->v.by_mac   = nd->w_by_mac;
   nd->v.n        = 0;
   nd->v.n_live   = 0;
   nd->v.n_mac    = 0;
   nd->rc         = 0;
   return nd;
}

/* Fill a node the publisher owns and nobody can see yet. Caller holds reg_lk.
 *
 * `cap` IS WHAT THE NODE WAS ALLOCATED FOR, and it is passed rather than
 * recomputed. Both numbers come from g_nslot under one hold of reg_lk, so
 * today they cannot differ -- but the rows written here are bounded by the
 * rows bought there, and saying so in the signature is what keeps that true
 * if either side is ever changed. */
static void view_fill_locked(struct view_node *nd, int cap)
{
   struct sensor_view *out = &nd->v;
   int lim                 = g_nslot < cap ? g_nslot : cap;
   int n                   = 0;
   /* TWO PASSES, live then retired, so the live ones occupy a prefix a caller
    * can stop at. Within each group the registry's own order is kept: the
    * device list is drawn in it, and shuffling rows under the user because a
    * sensor was retired is a worse cost than the walk this saves. */
   for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < lim && n < cap; i++) {
         if ((pass == 0) == (g_slot[i].old != 0))
            continue;
         nd->w_slot[n]              = g_slot[i];
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
         nd->w_have[n]              = r != 0;
         nd->w_rec[n]               = r ? *r : (struct sensor_rec){0};
         n++;
      }
      if (pass == 0)
         out->n_live = n;
   }
   out->n = n;
   /* THE ADDRESS INDEX: collect, then sort.
    *
    * HEAPSORT AND NOT INSERTION, and the reason is the capacity. Insertion is
    * O(n^2) comparisons on input in registry order -- which is chronological,
    * i.e. unrelated to address order -- and this runs under reg_lk on the
    * binder thread that mints a sensor. Measured on addresses sharing an OUI
    * prefix: 33 ms at 2048 devices and 510 ms at 8192 on a desktop core, so
    * one to two SECONDS on a phone, holding the registry lock. That is the
    * shape this app has been killed for before (thread.h). Heapsort is
    * O(n log n), in place, needs no allocation and no recursion. */
   out->n_mac = 0;
   for (int i = 0; i < n; i++) {
      if (!nd->w_have[i] || !nd->w_rec[i].identity[0])
         continue;
      nd->w_by_mac[out->n_mac++] = (short)i;
   }
   by_mac_sort(nd);
}

int sensors_view_find_mac_kind(const struct sensor_view *v, const char *mac,
                               int kind)
{
   if (!v || !mac || !mac[0])
      return -1;
   int lo = 0;
   int hi = v->n_mac - 1;
   while (lo <= hi) {
      int mid = lo + ((hi - lo) / 2);
      int idx = v->by_mac[mid];
      int c   = strcmp(v->rec[idx].identity, mac);
      if (c == 0) {
         /* THE LOWEST SLOT OF ANY RUN OF EQUAL ADDRESSES, which is the LIVE
          * one: the view puts live slots first, so a retired namesake always
          * sits at a higher index. An advert has to resolve to the device
          * still in service, and bisection lands anywhere in a run -- the
          * sort has no reason to keep the table's order among equals -- so
          * the run is walked out both ways rather than trusting where the
          * search stopped. One comparison in the ordinary case, where the
          * address is unique. */
         int at = -1;
         if (kind < 0 || sensor_kind(v->rec[idx].type) == kind)
            at = idx;
         for (int i = mid; i > 0; i--) {
            int p = v->by_mac[i - 1];
            if (strcmp(v->rec[p].identity, mac) != 0)
               break;
            if (kind >= 0 && sensor_kind(v->rec[p].type) != kind)
               continue;
            if (at < 0 || p < at)
               at = p;
         }
         for (int j = mid + 1; j < v->n_mac; j++) {
            int q = v->by_mac[j];
            if (strcmp(v->rec[q].identity, mac) != 0)
               break;
            if (kind >= 0 && sensor_kind(v->rec[q].type) != kind)
               continue;
            if (at < 0 || q < at)
               at = q;
         }
         return at;
      }
      if (c < 0)
         lo = mid + 1;
      else
         hi = mid - 1;
   }
   return -1;
}

/* Drop one reference. Caller holds view_lk. */
static void view_drop_locked(struct view_node *n)
{
   if (!n || n == &g_view_empty)
      return;
   if (--n->rc <= 0)
      free(n);
}

/* Publish the table as it stands. Caller holds reg_lk.
 *
 * A FAILED ALLOCATION KEEPS THE PREVIOUS VIEW. It is one mutation stale,
 * which is wrong in a way the next successful publish corrects -- and every
 * alternative (publishing nothing, publishing empty) is wrong in a way that
 * does not. */
static void view_publish_locked(void)
{
   /* THE ALLOCATION FIRST, THEN THE STYLING. Both describe the registry as it
    * now stands, and a plot drawn from a stale style map miscolours the very
    * points the change was about -- but publishing the styling for a view
    * that then fails to allocate leaves the two describing different
    * registries, which is the one state this pairing exists to prevent. */
   int rows                = g_nslot < MAX_SLOTS ? g_nslot : MAX_SLOTS;
   struct view_node *fresh = view_alloc(rows);
   if (!fresh) {
      /* THE TABLE CHANGED AND NO READER CAN SEE IT. The mutation still
       * reaches the flash, so this is not lost data -- but every reader goes
       * on answering from the previous view until some later mutation
       * publishes successfully, and the advert path not recognising a sensor
       * that just claimed its slot is exactly the kind of silence that costs
       * hours to diagnose. Say it where it can be found. */
      LOGW("registry: could not publish a view of %d devices; readers keep "
           "the previous one until the next change",
           rows);
      g_view_stale = 1;
      return;
   }
   style_rebuild_locked();
   view_fill_locked(fresh, rows);
   fresh->rc = 1; /* the publisher's own reference */
   mutex_lock(&view_lk);
   g_view_stale          = 0;
   struct view_node *old = g_view;
   g_view                = fresh;
   view_drop_locked(old);
   mutex_unlock(&view_lk);
}

const struct sensor_view *sensors_view_ref(void)
{
   mutex_lock(&view_lk);
   struct view_node *n = g_view;
   if (n != &g_view_empty)
      n->rc++;
   mutex_unlock(&view_lk);
   return &n->v;
}

void sensors_view_put(const struct sensor_view *v)
{
   if (!v)
      return;
   mutex_lock(&view_lk);
   /* The node is the view: sensor_view is its first member, so this is the
    * inverse of what ref() handed out and not a search.
    *
    * THE CONST THAT IS DROPPED IS THE CONTAINER'S, NOT THE PICTURE'S. A
    * published view is handed out const because no reader may ever change
    * one -- that immutability is what lets a reference be held across another
    * thread's registry mutation. The reference COUNT sits beside the view in
    * the node, is not part of the picture, and is what releasing a reference
    * has to reach. The cast goes through uintptr_t so that removal is
    * deliberate and visible rather than something -Wcast-qual would have to
    * be turned off for. */
   struct view_node *n = (struct view_node *)(uintptr_t)v;
   view_drop_locked(n);
   mutex_unlock(&view_lk);
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

int sensor_id_by_mac(const char *identity)
{
   /* THROUGH THE VIEW'S SORTED ADDRESS INDEX, so this costs a bisect and not a
    * walk. The slot-table version below resolves every row's provenance by
    * bisect to compare one address, which is n log n over a table bounded by
    * every device ever registered -- under reg_lk, which is an untimed spin
    * every advert callback takes, from a caller that runs at 1 Hz. The view is
    * refcounted and needs no state lock at all.
    *
    * ONE PUBLISH BEHIND is the trade, and it is the right one here: the
    * question is "does a slot already claim this address", asked once a second
    * about a device that has been on the air for minutes. sensor_mint asks the
    * same question about the id it is on the point of issuing, and must not be
    * answered from a snapshot -- it keeps the walk below. */
   const struct sensor_view *v = sensors_view_ref();
   int at                      = sensors_view_find_mac(v, identity);
   int id                      = at >= 0 ? v->slot[at].id : -1;
   sensors_view_put(v);
   return id;
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
    * in -- the registry ranks ABOVE the history (thread.h), and taking them
    * in that order keeps the graph acyclic. */
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
/* THE SCRATCH FOR ONE TRANSACTION, and there is one because there can only
 * ever be one transaction: regfile_lk is taken by reg_write_begin and held
 * until reg_write_end or reg_write_abort releases it, so the undo copy and
 * the rendered bytes below have exactly one writer for their whole lifetime.
 *
 * They are file-scope rather than automatic because they are sized by
 * MAX_SLOTS, which is a capacity measured in centuries (sensors.h): a whole
 * table on the stack would put a megabyte in a binder frame at that size and
 * kill the thread that touches the registry. Static, they are paid once. */
struct slot_undo {
   struct sensor_slot slot[MAX_SLOTS];
   int n;
};

/* DID THE SLOT TABLE EVER LOAD? Set by sensors_load, and read by the one
 * place that replaces the file.
 *
 * A load that fails leaves g_nslot at 0 -- an empty registry, published like
 * any other. Everything downstream then behaves as though the user owns no
 * devices, and the FIRST mutation after that (a pairing, a tap, or the
 * startup walk that re-slots orphaned readings) renders that empty table and
 * atomically replaces slots.csv with it. Every label, colour, marker, wear
 * override, primary flag and retired flag is gone, and the file that held
 * them has been overwritten by the process that could not read it.
 *
 * So a registry that did not load is READ-ONLY until one does. Refusing the
 * write costs the user a change they must make again after a restart;
 * allowing it costs them every device they have ever owned. */
static int g_slots_loaded = 1; /* until a load says otherwise */
/* AND WHETHER IT PARSED WHOLE, which is a stricter answer than g_slots_loaded
 * and is asked by a different caller.
 *
 * g_slots_loaded means "the rows are not GONE" -- it is what decides whether
 * the table may be written back over the file, so it stays set for a file that
 * read but lost individual rows: those rows are still on disk, and refusing to
 * write would strand every later change. A MINT needs the other answer. It
 * takes the next id from the highest one the two tables hold, so a table
 * missing the row that carried the highest id walks maxid backwards and
 * reissues an id readings.csv already cites -- two devices' histories merged in
 * a log nothing rewrites, and the next rewrite erases the dropped slot for
 * good. Mirrors what g_srec_loaded already covers for the provenance file. */
static int g_slots_whole = 1;
/* AND THE SAME FOR PROVENANCE, for a different consequence. sensor_mint takes
 * the next id from the highest one this table holds, so a table that did not
 * load whole hands out an id readings.csv already attributes to another
 * physical sensor -- two devices' histories merged in a file nothing
 * rewrites. A mint is refused until the file reads. */
static int g_srec_loaded = 1;
/* AND WHETHER IT COVERS EVERY SLOT, which is a different answer with a
 * different consequence and so a different flag. The two files are separate
 * append-only logs restored separately, so slots.csv can name an id sensors.csv
 * does not -- both files whole, both parses clean. Those devices show with no
 * type and no session until the row arrives, which is worth SAYING; it is not
 * worth refusing a mint over, because the state persists across every relaunch
 * and no control the user has removes a slot. What keeps the ids safe is
 * sensor_mint reading maxid from both tables. */
static int g_srec_covers = 1;

static struct slot_undo g_undo;
/* THE WIDEST ROW THE FORMAT CAN PRINT, per slot. "%d,%s,%d,%d,%d,%d,%d,%d\n"
 * is an id, a label of at most 19 characters, six more ints, seven commas and
 * a newline: 104 at the widest an int prints. Sized from the values expected
 * instead, a full table renders short, and slots_render_locked's answer to
 * that is to refuse the whole write -- correct, but a registry that cannot be
 * saved at capacity is not a capacity. */
#define SLOT_ROW 112

static char g_render[(MAX_SLOTS * SLOT_ROW) + 1];

#ifdef APP_FAULTS
static int g_fault_render_cap;

void sensors_fault_render_cap_set(int cap)
{
   g_fault_render_cap = cap;
}
#endif

/* THE ROWS THAT EXIST, not the capacity. Rows at and past g_nslot hold
 * nothing a restore has to put back -- the count is what says which rows are
 * real -- so copying to the end of the table would move hundreds of kilobytes
 * of unused storage on every rename. */
static void slots_snapshot(void)
{
   for (int i = 0; i < g_nslot; i++)
      g_undo.slot[i] = g_slot[i];
   g_undo.n = g_nslot;
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
static int reg_write_end(void)
{
   if (!g_slots_loaded) {
      /* PUT THE TABLE BACK AND WRITE NOTHING. The change is refused whole,
       * so memory and the untouched file still agree. */
      for (int i = 0; i < g_undo.n; i++)
         g_slot[i] = g_undo.slot[i];
      g_nslot = g_undo.n;
      view_publish_locked();
      reg_unlock();
      mutex_unlock(&regfile_lk);
      LOGW("registry: slots.csv did not load, so it will not be rewritten; "
           "restart once it can be read before changing devices");
      return SENSOR_UNSAVED;
   }
   int used = slots_render_locked(g_render, sizeof g_render);
   /* PUBLISHED BEFORE THE WRITE, not after it. The mutator changed the table
    * before calling here, and the write that follows releases reg_lk across
    * an fsync -- so publishing afterwards would leave every reader looking at
    * the state before the change for as long as the flash took. A copy taken
    * from the live table saw the change the instant it was made, and readers
    * must not lose that to a storage delay: an advert arriving in that window
    * would not recognise a sensor that had just claimed its slot. */
   view_publish_locked();
   reg_unlock();
   /* A RENDER THAT COULD NOT DESCRIBE THE TABLE PUBLISHES NOTHING. The file on
    * disk stays exactly as it was -- which still holds
    * every sensor -- and the change is undone below, so memory and the file
    * agree. Writing the prefix would agree with neither. */
   int ok = (used < 0) ? SENSOR_UNSAVED : slots_write(g_render, used);
   if (ok != 0) {
      /* THE UNDO RUNS UNDER THE STATE LOCK AGAIN, and it is still correct
       * because regfile_lk has been held throughout: no other mutation could
       * have landed between the render and here, so the copy in g_undo is
       * still the state this mutation was applied to. */
      reg_lock();
      for (int i = 0; i < g_undo.n; i++)
         g_slot[i] = g_undo.slot[i];
      g_nslot = g_undo.n;
      /* AND AGAIN, because the table just moved back: the view published
       * before the write describes a change that did not survive it. */
      view_publish_locked();
      reg_unlock();
   }
   mutex_unlock(&regfile_lk);
   return ok == 0 ? SENSOR_OK : SENSOR_UNSAVED;
}

int sensor_set_marker(int id, int marker)
{
   if (marker < 0 || marker >= MARK_N)
      return -1;
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].marker = marker;
   return reg_write_end();
}

int sensor_set_color(int id, int color)
{
   if (color < 0 || color >= SET_NCOLORS)
      return -1;
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].color = color;
   return reg_write_end();
}

int sensor_set_size(int id, int size)
{
   if (size < 1 || size > MARK_SIZE_MAX)
      return -1;
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   g_slot[idx].size = size;
   return reg_write_end();
}

int sensor_cycle_wear(int id)
{
   reg_write_begin();
   slots_snapshot();
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
   return reg_write_end();
}

int sensor_set_label(int id, const char *name, int len)
{
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return -1;
   }
   /* A COMMA OR A NEWLINE WOULD BE A SECOND FIELD, or a second row: this file
    * has no quoting, so a label carrying either is one the loader reads back
    * as a different device. The keypad cannot produce them (ui_label_chars),
    * which is exactly why the guard belongs HERE -- at the only door into the
    * table -- rather than resting on what today's one caller happens to
    * allow. Dropped rather than rejected: the rest of the name is still the
    * name the user typed. */
   int k = 0;
   if (name)
      for (int i = 0; i < len && k < (int)sizeof g_slot[0].label - 1; i++) {
         if (name[i] == ',' || name[i] == '\n' || name[i] == '\r' ||
             name[i] == '\0')
            continue; /* a NUL truncates the row and revives a retired device */
         g_slot[idx].label[k++] = name[i];
      }
   g_slot[idx].label[k] = 0;
   /* An all-blank name makes the device row unreadable, and that row is how a
    * user tells two identical sensors apart. */
   if (k == 0)
      (void)snprintf(g_slot[idx].label, sizeof g_slot[0].label, "SENSOR %d",
                     g_slot[idx].id);
   return reg_write_end();
}

int sensor_set_primary(int id)
{
   reg_write_begin();
   slots_snapshot();
   int idx                    = slot_of_id_locked(id);
   const struct sensor_rec *r = idx >= 0 ? sensor_rec_by_id(id) : 0;
   /* A BGM must never own the big number: a hours-old fingerstick rendered as
    * the headline value (with a trend arrow) would actively mislead. An OLD
    * (disconnected) device cannot be primary either -- it is not streaming. */
   if (!r || sensor_kind(r->type) != KIND_CGM || g_slot[idx].old) {
      reg_write_abort();
      /* A REFUSAL, NOT A SUCCESS. Reported as SENSOR_OK the caller says
       * nothing, the checkbox does not fill, and the user is left tapping a
       * control that answers neither way. The three reasons are all real --
       * a meter, a retired device, or a slot whose provenance did not load --
       * and each of them is something to be told about rather than a no-op.
       */
      return SENSOR_UNSAVED;
   }
   for (int i = 0; i < g_nslot; i++)
      g_slot[i].primary = (i == idx);
   return reg_write_end();
}

int sensor_slot_count(void)
{
   /* EVERY SLOT, live or retired, of any kind. The one question this answers is
    * "is the registry empty" -- which is what decides whether provenance id 0
    * can only mean pre-registry data. A count of live CGMs answers a different
    * question and gets this one wrong: with a meter registered and no CGM it
    * says zero, so a newly adopted sensor's first reading is stamped 0 in a log
    * that is never rewritten. */
   reg_lock();
   int n = g_nslot;
   reg_unlock();
   return n;
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
   /* A LIVE HOLDER IS THE ONLY HOLDER THAT COUNTS. Callers hand a device to
    * this function by retiring it, so "is there still a primary" has to mean
    * a live one. */
   int have = 0;
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary && !g_slot[i].old) {
         have = 1;
         break;
      }
   if (have)
      return;

   /* A RETIRED SLOT DOES NOT KEEP THE FLAG once this function is reached at
    * all -- which is after the live-holder test above, so it runs exactly
    * when there is no live primary. The loader and sensor_set_primary both
    * reject a retired holder, so leaving one behind writes a state the app
    * itself will not accept and the next load silently drops. */
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].old)
         g_slot[i].primary = 0;

   /* WHETHER THE OPERATION MAY HAPPEN AT ALL is the CALLER's question.
    * Choosing a successor needs every slot's TYPE, which lives in the
    * provenance table, so with sensors.csv unread none can be chosen and the
    * registry would be persisted owing the big number to nobody --
    * irrecoverable, because repairing the file afterwards does not bring the
    * flag back. sensor_retire and sensor_forget refuse the whole operation in
    * that state rather than let it be written; this is the second line, for
    * any path that does not. */
   if (!g_srec_loaded) {
      LOGW("registry: no primary can be chosen -- sensors.csv did not load, "
           "so the device types are unknown");
      return;
   }
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
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return SENSOR_UNSAVED; /* no such device: nothing was changed */
   }
   /* NOT WHILE NO SUCCESSOR CAN BE CHOSEN. Handing this device's primary
    * flag on needs every other slot's TYPE, which lives in the provenance
    * table -- so with sensors.csv unread the operation would persist a
    * registry that owns the big number to nobody, and repairing the file
    * afterwards does not bring it back. Refused whole instead, and the
    * refusal is on screen. */
   if (g_slot[idx].primary && !g_srec_loaded) {
      reg_write_abort();
      return SENSOR_UNSAVED;
   }
   g_slot[idx].old = 1;
   /* THE FLAG IS CLEARED BY THE REASSIGNMENT, NOT BEFORE IT. Cleared here,
    * reassign_primary_locked's "there is still a primary" test is already
    * false when it runs, so its refusal to act without provenance leaves the
    * registry with NO primary at all -- the outcome that refusal exists to
    * prevent. It knows a retired slot cannot hold the flag; let it say so. */
   reassign_primary_locked();
   return reg_write_end();
}

int sensor_revive(int id)
{
   reg_write_begin();
   slots_snapshot();
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
   return reg_write_end();
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
   /* THE LONG IS TESTED BEFORE IT IS NARROWED. csv_num accepts up to
    * CSV_MAX_DIGITS, so a row citing 4294967297 narrows to 1 -- and srec_push
    * is last-wins per id, so that row would silently OVERWRITE device 1's
    * address, type and serial, which is the one thing this table exists to
    * keep. Bounded to the same 1..MAX_SLOTS the mint and slots_parse use, so
    * a value outside it is a row this build did not write, not a device. */
   long rawid = csv_num(&c, &idok);
   r.id       = (rawid > 0 && rawid <= MAX_SLOTS) ? (int)rawid : 0;
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
 * A TAIL-LIMITED READ LOSES THE OLDEST ROWS, and the oldest rows are the ones
 * that matter most here: a device is minted ONCE, early, and every reading it
 * ever produces cites that row for the life of the log. A provenance row is
 * ~70 bytes, so a last-8-KB read stops resolving ids somewhere past a hundred
 * devices -- the meter's row among them, since a meter is minted once and never
 * again. Its id then resolves to nothing, sensor_reconcile cannot bind it, and
 * the meter silently stops auto-syncing for good.
 *
 * Streaming is affordable: the file grows about one row per sensor session
 * (~1.7 KB/year), so even decades of use is a single sub-100 KB scan at
 * startup, and the table holds one entry per ID rather than one per line --
 * every sensor_complete correction lands back on the row it corrects. */
/* 0 when read whole (including "no file yet"), -1 when a read failed partway.
 * What a short read loses here is PROVENANCE: which physical sensor each
 * historical reading came from, and the model/firmware/activation the app
 * mints new rows against. */
/* THE FILE INTO MEMORY, WITH NO STATE LOCK HELD. The parse below fills the
 * provenance table and therefore needs reg_lk; the READ does not, and reg_lk
 * is an untimed yield-spin that the frame builder and every advert callback
 * take. Reading under it makes them wait on flash for as long as the file
 * takes -- and at this capacity that file is megabytes. regfile_lk, held by
 * the caller throughout, is what keeps a writer out of the gap.
 *
 * *out is malloc'd and the caller frees it; NULL with 0 length means the file
 * is absent, which is a first run and not a failure. Returns -1 only when the
 * file exists and could not be read whole. */
static int srec_read(char **out, long *outn)
{
   *out   = NULL;
   *outn  = 0;
   int fd = open(g_sensors_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   /* A CEILING, like every other loader here. Doubling with none is a file
    * that can ask for whatever it likes; this one is bounded by what the
    * registry can describe -- one provenance row per id, and ids stop at
    * MAX_SENSOR_RECS -- with room to spare for a file carrying superseded
    * correction rows. Past it the file is refused rather than read, which is
    * the same answer slots_read gives to an oversized table. */
   /* 1 kB PER ID, not 512 bytes. A row is at most ~127 bytes, and an id can
    * carry its mint plus up to four sensor_complete corrections, so ~635
    * bytes is the real worst case -- 512 would refuse the app's OWN file
    * somewhere past six thousand devices, which is inside the capacity this
    * build advertises. A ceiling that the writer can cross is not a ceiling,
    * it is a second failure mode. */
   const long srec_cap_max = (long)MAX_SENSOR_RECS * 1024;
   long cap                = 0;
   long used               = 0;
   char *b                 = NULL;
   for (;;) {
      if (used == cap) {
         /* THE CEILING IS A SIZE THIS LOADER CAN HOLD, not one short of it.
          * Doubling straight past it refuses a file of EXACTLY srec_cap_max
          * bytes -- the buffer fills, the read has not yet answered zero, and
          * the next growth step is over the limit -- so the one file size the
          * ceiling names is the one it rejects. Clamped to a byte more than the
          * ceiling, that file reads and the EOF probe still has somewhere to
          * land; the refusal then means what it says. */
         long want = cap ? cap * 2 : 8192;
         if (want > srec_cap_max + 1)
            want = srec_cap_max + 1;
         if (want == cap) {
            LOGW("sensors: %s is larger than the %ld bytes this build can "
                 "load; REFUSING it rather than minting from a prefix",
                 g_sensors_path, srec_cap_max);
            free(b);
            close(fd);
            return -1;
         }
         char *nb = realloc(b, (size_t)want);
         if (!nb) {
            free(b);
            close(fd);
            return -1;
         }
         b   = nb;
         cap = want;
      }
      long r = read(fd, b + used, (size_t)(cap - used));
      if (r < 0) {
         if (errno == EINTR)
            continue; /* a signal is not end of file: see srec_parse */
         free(b);
         close(fd);
         return -1;
      }
      if (r == 0)
         break;
      used += r;
   }
   close(fd);
   *out  = b;
   *outn = used;
   return 0;
}

/* CALLER HOLDS reg_lk. */
static int srec_parse(const char *src, long srcn)
{
   g_nsrec = 0;
   char line[256];
   int llen = 0;
   int over = 0; /* this line exceeded the buffer: skip it rather than truncate,
                  * since a truncated row parses as a DIFFERENT sensor */
   int damaged = 0;
   /* A SIGNAL IS NOT END OF FILE, which is why the read above retries EINTR
    * and this parse sees the whole file or nothing. A prefix of this table is
    * the LOW ids -- the file is appended in id order -- so maxid would walk
    * backwards and the next mint would reissue an id readings.csv already
    * attributes to another physical sensor, merging two devices' histories in
    * a file nothing rewrites. */
   for (long i = 0; i < srcn; i++) {
      if (src[i] == '\n') {
         /* Longer than any row can be, or a row that does not parse:
          * either way a sensor's provenance is missing and the caller has
          * to be told. */
         if (over || srec_parse_line(line, line + llen) < 0)
            damaged = 1;
         llen = 0;
         over = 0;
      } else if (src[i] == 0) {
         /* A NUL IS DAMAGE, NOT A FIELD. The walk is bounded by the count, so
          * a NUL does not end the file here -- but the fields are copied out
          * as C strings, so one inside a serial or an address truncates that
          * string and the row then names a DIFFERENT sensor than the file
          * records. Report it and drop the row it is in. */
         damaged = 1;
         over    = 1;
      } else if (llen < (int)sizeof line - 1) {
         line[llen++] = src[i];
      } else {
         over = 1;
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
   return damaged ? -1 : 0;
}

/* 0 when read whole, -1 when the read failed. A short slot table is a sensor
 * the user has to pair again, key and all. */
/* One bit per possible id, for the duplicate check below. Ids run 1..MAX_SLOTS
 * -- bounded at the mint and again by the loader -- so this covers every id
 * that can exist, and the check needs no case for one that cannot. */
static unsigned char g_id_seen[(MAX_SLOTS / 8) + 1];

/* THE FILE INTO g_render, WITH NO STATE LOCK HELD -- the same split as
 * srec_read, and for the same reason: the read is flash and reg_lk is a spin
 * every frame and every advert takes. g_render is guarded by regfile_lk,
 * which the caller holds. Returns the byte count, or -1. */
static long slots_read(void)
{
   /* READ-ONLY IS ABOUT LOST ROWS, NOT ABOUT A CLEAN PARSE.
    *
    * The rewrite this guards against is the one that replaces the file with
    * an EMPTY table -- so what matters is whether any row survived, not
    * whether every row did. A file missing its last newline, or carrying one
    * unparseable line, still loads every other device: rewriting it from what
    * loaded is the repair, and refusing would leave the registry read-only
    * for the life of the install with nothing the user could do about it
    * from inside the app.
    *
    * So this is cleared only where the rows are GONE -- the file could not be
    * opened, could not be read, or did not fit. */
   g_slots_loaded = 1;
   int fd         = open(g_slots_path, O_RDONLY, 0);
   if (fd < 0) {
      if (errno == ENOENT)
         return 0; /* first run: writing creates it */
      g_slots_loaded = 0;
      return -1;
   }
   /* THE BUFFER THE WRITER RENDERS INTO, read back through the same array.
    *
    * THE READER MUST NOT BE THE SMALLER OF THE TWO. A load buffer sized by
    * its own constant is a second number that has to be kept in step with the
    * render's, and when it falls behind the app writes a registry it cannot
    * read again: the next launch refuses the file, publishes an empty table,
    * and the first mutation after that rewrites the file from that empty
    * table -- every label, colour, primary flag and retired flag gone, with
    * no error the user can act on. Sharing the array makes the two sides the
    * same size by construction, so the failure cannot be reintroduced.
    *
    * SAFE TO SHARE because regfile_lk serialises the only two users: a load
    * holds it (sensors_load) and so does every mutation, from
    * reg_write_begin through the render and the write in reg_write_end.
    *
    * NOT read_file_exact: that reader stages through a fixed local capped at
    * MAX_EXACT_READ, and this file grows with the registry's capacity.
    *
    * A short table is a REPORTED failure (-1), the same answer a read error
    * gets, because publishing part of a registry is worse than publishing
    * none of it. */
   char *buf      = g_render;
   const long cap = (long)sizeof g_render;
   long n         = 0;
   for (;;) {
      /* READ TO EOF, not once: read() may return less than asked for on a
       * file this size, and a single call would silently treat the first
       * chunk as the whole registry. */
      long r = read(fd, buf + n, (size_t)(cap - n));
      if (r < 0) {
         if (errno == EINTR)
            continue;
         close(fd);
         g_slots_loaded = 0;
         return -1;
      }
      if (r == 0)
         break; /* EOF: the file ended where the buffer still had room */
      n += r;
      if (n >= cap) {
         close(fd);
         g_slots_loaded = 0;
         LOGW("slots: %s is larger than the %ld bytes this build can load; "
              "REFUSING it rather than publishing a registry missing its tail",
              g_slots_path, cap - 1);
         return -1;
      }
   }
   close(fd);
   return n;
}

/* CALLER HOLDS reg_lk. `n` is what slots_read returned. */
static int slots_parse(long n)
{
   char *buf = g_render;
   g_nslot   = 0;
   memset(g_id_seen, 0, sizeof g_id_seen);
   if (n == 0)
      return 0;
   buf[n]      = 0;
   int damaged = 0;
   /* A FILE THAT DOES NOT END IN A NEWLINE was cut while being written. The
    * slots file is rewritten whole (never appended to), so this means the
    * rewrite did not finish -- what follows may be a row, or half of one. */
   if (n > 0 && buf[n - 1] != '\n')
      damaged = 1;
   /* AND A NUL INSIDE IT IS DAMAGE TOO, not the end of the file.
    *
    * The read counted bytes; walking the result as a C string would stop at
    * the first NUL and silently drop every row after it -- and because the
    * LAST byte can still be a newline, that truncation would pass the check
    * above, report success, and be made permanent by the next rewrite. So
    * the walk below is bounded by the count, and a NUL is reported rather
    * than obeyed. Every row is still parsed: csv_open takes explicit bounds,
    * so a row containing one is rejected on its own without ending the
    * file. */
   for (long i = 0; i < n; i++) {
      if (buf[i] == 0) {
         damaged = 1;
         break;
      }
   }
   char *p = buf;
   /* THE TRUNCATED LAST ROW IS NOT PARSED. A file that does not end in a
    * newline was cut mid-rewrite, so its final row is a prefix -- and the
    * fields are positional, so a prefix parses: the id and part of the label
    * survive while marker, colour, size, wear and `old` all read as 0. That
    * silently reverts the device's appearance and, because `old` is one of
    * them, brings a device the user DISCONNECTED back live, where it takes a
    * link and re-enters the primary pick. The row is already counted as
    * damage above; stopping the walk before it is what keeps it out of the
    * table. */
   const char *end = buf + n;
   if (n > 0 && buf[n - 1] != '\n') {
      const char *last = end;
      while (last > buf && last[-1] != '\n')
         last--;
      end = last; /* the last complete row, and nothing after it */
   }
   while (p < end) {
      char *e = p;
      while (e < end && *e != '\n')
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
      /* AND IT MUST BE AN ID THIS BUILD COULD HAVE MINTED. csv_num narrows,
       * so a hand-edited or merged row carrying 99999999999 arrives as some
       * other positive number, loads without a complaint, and can take the
       * primary flag -- while the id-indexed tables (g_style, g_src_last)
       * could never hold it, so it would draw with the ORPHAN look and show
       * no last-seen. Ids stop at MAX_SLOTS where they are issued, and a row
       * naming one past that is damage, not a device. */
      long rawid = csv_num(&c, 0);
      s.id       = (rawid > 0 && rawid <= MAX_SLOTS) ? (int)rawid : 0;
      if (rawid > 0 && s.id == 0)
         damaged = 1;
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
      /* 8th field; absent in older files -> 0 = live (not an old device).
       *
       * AND THIS IS THE ONE FIELD WHOSE DEFAULT IS A STATE CHANGE, so it is the
       * one field that asks whether it was really read. Everything above
       * defaults to an appearance; `old` defaults to LIVE, and a device wrongly
       * read as live takes a radio link and re-enters the primary pick -- and
       * the next mutation writes that back, so the user's disconnect is undone
       * permanently.
       *
       * ABSENT AND UNREADABLE ARE DIFFERENT, and the cursor tells them apart: a
       * file written before this column existed has nothing left on the line,
       * while a row that HAS an eighth field which is not a number is damage.
       * Damage reads as RETIRED, because that is the reversible direction --
       * a device wrongly retired sits in OLD DEVICES with a RECONNECT button on
       * it, and one wrongly revived silently competes for the big number. */
      int old_present     = c.p < e;
      enum csv_field oldw = CSV_FIELD_EMPTY;
      s.old               = (int)csv_num(&c, &oldw) ? 1 : 0;
      if (old_present && oldw != CSV_FIELD_OK) {
         s.old   = 1;
         damaged = 1;
      }
      if (s.id > 0) {
         if (s.marker < 0 || s.marker >= MARK_N)
            s.marker = MARK_SQUARE_F;
         if (s.marker == MARK_DOT) /* DOT dropped -> its identical twin */
            s.marker = MARK_SQUARE_F;
         /* THE PALETTE'S OWN SIZE, not a literal: written as 6 this silently
          * rewrites a legitimate colour to 0 on every launch the moment the
          * palette grows, and the user's choice is gone from the file after
          * the next mutation. */
         if (s.color < 0 || s.color >= SET_NCOLORS)
            s.color = 0;
         if (s.size < 1 || s.size > MARK_SIZE_MAX)
            s.size = MARK_SIZE_DEF; /* default / migrate old files */
         if (s.wear_days != 10 && s.wear_days != 15)
            s.wear_days = 0; /* anything else means "not overridden" */
         if (s.old)
            s.primary = 0; /* an old device can never be the primary */
         /* ONE ROW PER ID, FIRST WINS -- and which one is first is not an
          * arbitrary choice, it is where the restore merge puts them.
          *
          * This file is rewritten whole, so it never holds a duplicate this
          * app wrote. A restore makes one: sync_restore stages THIS PHONE's
          * copy byte for byte and then APPENDS the rows the server sent that
          * the phone does not already hold (syncrestore.c). So for an id the
          * user has changed since the last push, the phone's current row
          * comes first and the server's superseded row is appended after it.
          *
          * Taking the last would therefore undo the change: a rename or a
          * colour would revert, and -- because `old` and `primary` are on
          * this row too -- a device the user DISCONNECTED would come back
          * live, reconnect, take a link and re-enter the primary pick. The
          * first row is the phone's, and the phone is the authority for its
          * own preferences.
          *
          * SEEN BY ID, not by scanning the rows built so far: at this
          * capacity that scan is quadratic on the startup path, under both
          * registry locks. An id is 1..MAX_SLOTS, so one bit each is a
          * kilobyte. */
         unsigned uid  = (unsigned)s.id;
         unsigned byte = uid >> 3U; /* uid is 1..MAX_SLOTS, checked above */
         unsigned bit  = 1U << (uid & 7U);
         /* EVERY ID IS DEDUPED, with no exception to reason about: the row
          * above rejects anything outside 1..MAX_SLOTS, so the bitmap indexes
          * every id that can reach here. Two slots sharing an id is how
          * slot_of_id_locked comes to name one of them while every mutation
          * silently applies to that one. */
         int dup = (g_id_seen[byte] & bit) != 0U;
         if (dup) {
            LOGW("slots: %s names id %d twice; keeping this phone's row",
                 g_slots_path, s.id);
         } else {
            /* MORE DISTINCT DEVICES THAN THERE ARE SLOTS is the same
             * failure as a short read: stopping quietly at MAX_SLOTS
             * publishes a registry missing sensors the file describes.
             *
             * COUNTED AFTER THE DUPLICATE CHECK, so a restore-merged file
             * that holds a full table PLUS a superseded copy of one row is
             * deduped and kept, not refused for rows it does not really
             * have. */
            if (g_nslot >= MAX_SLOTS) {
               LOGW("slots: %s holds more than %d sensors; REFUSING it "
                    "rather than publishing a registry missing the rest",
                    g_slots_path, MAX_SLOTS);
               g_nslot        = 0;
               g_slots_loaded = 0;
               return -1;
            }
            g_id_seen[byte]   = (unsigned char)(g_id_seen[byte] | bit);
            g_slot[g_nslot++] = s;
         }
      } else if (c.p != p) {
         /* A ROW WITH NO ID is not a device: skipped, and reported. Every
          * per-device preference -- its name, its colour, whether it is the
          * primary -- lives here, so a row lost in silence is a device that
          * quietly reverts to defaults. */
         damaged = 1;
      }
      p = (e < end && *e == '\n') ? e + 1 : e;
   }
   /* At most one primary can survive a hand-edited file. */
   int seen = 0;
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].primary && seen)
         g_slot[i].primary = 0;
      if (g_slot[i].primary)
         seen = 1;
   }
   /* BYTES IN, NO DEVICE OUT -- that is rows lost, whatever the reason.
    *
    * A file whose every row was rejected leaves an EMPTY table, which is the
    * one state the read-only guard exists for: publish it and the next
    * mutation rewrites the file from nothing. The four hard failures above
    * clear the flag where they stand; this catches the parse that read the
    * whole file and found no device in it. */
   if (n > 0 && g_nslot == 0) {
      g_slots_loaded = 0;
      LOGW("slots: %s held %ld bytes and no readable device; the registry is "
           "read-only until it loads",
           g_slots_path, n);
      /* AND IT IS A FAILURE, not merely a note. A file of blank lines sets no
       * `damaged` -- a blank line is not a bad row -- so reporting on that
       * alone would return SUCCESS from a load that produced no device and
       * quietly turned the registry read-only. The caller has to be able to
       * tell the user something happened. */
      return -1;
   }
   return damaged ? -1 : 0;
}

int sensors_load(void)
{
   /* Slots FIRST -- and it does not MATTER, which is the point worth
    * recording. A provenance read that evicted rows and "protected"
    * the ones a live slot referenced would make the order load-bearing: read
    * second, g_nslot is 0 throughout and nothing is protected. Nothing
    * evicts, so no read of one file can change what the other keeps, and the
    * order below is merely conventional.
    *
    * BOTH LOCKS, file first, for the reason every writer takes both: a save
    * or a mint landing between the two reads below would write one file from
    * a table the other read has not filled yet.
    *
    * reg_lk IS HELD ACROSS BOTH PARSES BUT ACROSS NEITHER READ, which is
    * worth stating because this is not only a startup path:
    * pancra_logs_reload calls it on the sync worker after a restore. reg_lk
    * is an untimed yield-spin the frame builder and every advert callback
    * take, so whatever it spans is what they wait on -- and at this capacity
    * the two files are megabytes of flash. What remains inside the hold is
    * the parse of bytes already in memory. */
   mutex_lock(&regfile_lk);
   /* BOTH FILES ARE READ FIRST, WITH NO STATE LOCK, and then both are parsed
    * under ONE hold of it. That ordering buys two things at once: no flash
    * read happens while the frame builder and the advert callbacks are
    * spinning on reg_lk, and the two tables still become visible together, so
    * no reader ever sees slots filled and provenance empty. regfile_lk is
    * held across all of it, so no writer can reach either file in between. */
   long sn     = slots_read();
   char *srbuf = NULL;
   long srn    = 0;
   int srok    = srec_read(&srbuf, &srn) == 0;
   reg_lock();
   /* BOTH PARSES RUN WHETHER OR NOT THEIR READ SUCCEEDED, because the parse
    * is also the RESET. Skipped, it leaves that table holding the previous
    * load's rows while the other one is refilled -- so a reload after a
    * restore would publish two tables from two different instants, and a view
    * built from them describes a registry that never existed. A read that
    * failed parses zero bytes, which empties its table; g_slots_loaded is
    * already clear by then, so the empty slot table cannot be written back
    * over the file it came from. */
   int sok = slots_parse(sn > 0 ? sn : 0) == 0 && sn >= 0;
   /* The parse's own verdict, kept for the mint. g_slots_loaded is the
    * rewrite flag and is deliberately more forgiving; see both declarations. */
   g_slots_whole = sok;
   /* THE GATE COVERS THE PARSE, NOT ONLY THE READ. A file that reads whole
    * and then loses rows to the parse -- a type this build does not know, a
    * row past the line buffer, a corrupted field -- leaves a table missing
    * exactly the rows it rejected. If one of them carried the highest id,
    * maxid walks backwards and the next mint reissues an id readings.csv
    * already cites. Refusing to mint until the file parses whole is the same
    * answer slots_parse gives for its own table, and the two gates must not
    * disagree about what "loaded" means. */
   int pparse    = srec_parse(srbuf, srn) == 0;
   g_srec_loaded = srok && pparse;
   g_srec_covers = 1; /* the walk below is what can lower it */
   int pok       = srok && pparse;
   int ok        = sok && pok;
   /* THE PRIMARY IS VALIDATED ONLY NOW, because only now are both halves in
    * memory. slots_parse enforces "at most one" and "not retired" from the
    * slot row alone -- it cannot enforce "a CGM", because the TYPE lives in
    * the provenance file this function has only just read. A merged
    * slots.csv can therefore carry the flag on a meter, and the big number,
    * its trend arrow and the notification would bind to a fingerstick.
    *
    * AND A REGISTRY THAT ARRIVES WITH NOBODY OWNING IT gets an owner here.
    * reassign_primary_locked is otherwise reached only from retire and
    * forget, so a file whose flag sat on a retired row -- cleared by the
    * parse -- would stay ownerless for the life of the install, with every
    * later mutation persisting that. */
   /* WHETHER THE PROVENANCE TABLE COVERS EVERY SLOT is a SEPARATE answer from
    * whether its file read, and it is REPORTED rather than used as a gate.
    *
    * The two files are separate append-only logs, restored separately by the
    * sync layer, so slots.csv can name ids sensors.csv does not -- with both
    * files whole and both parses clean. An absent sensors.csv is the extreme of
    * the same state: srec_read reports success for a file that is not there.
    *
    * IT MUST NOT SHUT THE MINT GATE, and that is the whole point of separating
    * the two. The state is on disk and self-consistent, so it survives every
    * relaunch -- and nothing the user can tap repairs it: DISCONNECT retires a
    * slot and KEEPS it, so the uncovered id is still there, and no other
    * control removes one. Gating the mint on it therefore ends pairing on that
    * install for ever, behind a message telling the user to restart, which
    * cannot help. What actually protects the ids is sensor_mint deriving maxid
    * from BOTH tables, which it does unconditionally.
    *
    * WHAT IT DOES CHANGE is the revalidation below, per slot rather than
    * wholesale: a flag holder whose type is unknown cannot be judged, so its
    * flag is left exactly as the file had it. */
   for (int i = 0; i < g_nslot; i++) {
      if (sensor_rec_by_id(g_slot[i].id))
         continue;
      LOGW("sensors.csv holds no row for slot id %d: that device shows with no "
           "type or session until the row arrives",
           g_slot[i].id);
      g_srec_covers = 0;
      /* THE LOAD IS REPORTED AS INCOMPLETE, which is what puts it on screen --
       * `g_srec_loaded` is NOT touched: it says whether the provenance file
       * read and parsed, and it did. */
      ok = 0;
   }
   if (g_srec_loaded) {
      for (int i = 0; i < g_nslot; i++) {
         if (!g_slot[i].primary)
            continue;
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
         /* AN UNKNOWN TYPE IS NOT A WRONG ONE. With no row for this id the
          * question "is it a live CGM" has no answer, and answering it "no"
          * clears a flag the file holds -- persisted by the next mutation,
          * taking the big number, the trend arrow and the alarm owner with it,
          * with no way back once the row arrives. Left alone, the flag is still
          * there when the row does. */
         if (!r)
            continue;
         if (sensor_kind(r->type) != KIND_CGM) {
            LOGW("slots: id %d holds the primary flag and is not a live CGM; "
                 "choosing again",
                 g_slot[i].id);
            g_slot[i].primary = 0;
         }
      }
      reassign_primary_locked();
   }
   /* A load is a mutation like any other, and the biggest one: without this
    * every reader would hold the empty startup view until the first pairing.
    * Published under the same lock that filled the tables. */
   view_publish_locked();
   reg_unlock();
   mutex_unlock(&regfile_lk);
   free(srbuf);
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
 * The accounting is util.h's textout, which is sticky: the first row that does
 * not fit poisons the builder and nothing after it is written. This function's
 * job is to pass that verdict on rather than to hand back a prefix. */
static int slots_render_locked(char *out, size_t cap)
{
#ifdef APP_FAULTS
   /* A BUFFER TOO SMALL FOR THE TABLE, on demand. SLOT_ROW is sized from what
    * the format can print, so the real buffer does not overflow -- and that
    * is exactly why this hook exists: the path that refuses a short render is
    * unreachable in a shipping build, and the file it would otherwise shorten
    * is the one that says which sensors this phone is paired to. Nothing that
    * ships defines APP_FAULTS. */
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
   /* NOT FROM A TABLE THAT DID NOT LOAD -- AND THE SCAN BELOW READS TWO.
    *
    * The next id is the highest either table holds, so minting while EITHER is
    * short reissues an id readings.csv already cites. Reading only sensors.csv
    * here is the trap: an unreadable slots.csv leaves g_nslot at 0, so the slot
    * half of that scan contributes nothing and the mint is back to one table --
    * in exactly the state where the other table is the one that knows. The row
    * is appended to a file nothing rewrites; the claim that follows is refused
    * while the slot table is unread, so nothing is visible until slots.csv
    * reads again, and then srec_push's last-wins puts a different physical
    * device's address and serial on that id for ever.
    *
    * A refused mint shows the sensor as unregistered, which is visible and
    * reversible; a reissued id merges two devices' histories permanently. */
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
   /* THE GATE IS READ UNDER THE LOCK THE SCAN RUNS UNDER. Both flags are
    * written by sensors_load, which a restore runs on the SYNC WORKER, so
    * reading them before reg_write_begin lets a reload land in between: the
    * gate sees two loaded tables and the scan below reads one of them
    * half-replaced. */
   if (!g_srec_loaded || !g_slots_whole) {
      LOGW("registry: %s did not load whole, so no new id may be issued; "
           "restart once it can be read",
           !g_slots_whole ? "slots.csv" : "sensors.csv");
      reg_write_abort();
      return -1;
   }
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
   /* BY ADDRESS ALONE, and deliberately unlike the provenance scan below,
    * which keys on (type, identity). One physical device keeps one id even if
    * its type were ever recorded differently; sensors_view_find_mac_kind
    * exists for the READ side, where a caller asking "the CGM at this
    * address" must not be answered with a meter. */
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
    * re-paired): reuse its id rather than minting another. The key is (type,
    * identity) and nothing else -- a difference in activation, model or
    * firmware is the SAME device described better, not a second one, and
    * keying on any of them forks one sensor's history across two ids.
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
    * AND IT IS WHAT KEEPS THE ID SCAN BELOW HONEST. maxid is read off the two
    * tables, so it is the highest id ever issued only while they hold every
    * row that loaded. Nothing evicts, and a table that cannot take a row mints
    * nothing at all -- so the scan is never consulted in the one state where it
    * could be short, and an id readings.csv already cites can never be handed
    * out a second time. */
   if (g_nsrec >= MAX_SENSOR_RECS) {
      reg_write_abort();
      return -1;
   }

   /* THE HIGHEST ID EITHER TABLE HAS EVER HELD, because there are TWO records
    * of a spent id and only one of them is this file. A slot row outlives
    * nothing -- nothing is evicted -- but the two files are separate logs, so
    * slots.csv can name an id sensors.csv does not. Reading only the
    * provenance table there reissues an id a slot already holds, and
    * sensor_claim_slot then binds this physical sensor to that device's slot,
    * label, colour and whole readings.csv history, with the append-only
    * provenance row superseding the original's. The load gate refuses that
    * state outright; scanning both tables means the mint does not depend on
    * its having done so. */
   int maxid = 0;
   for (int i = 0; i < g_nsrec; i++)
      if (g_srec[i].id > maxid)
         maxid = g_srec[i].id;
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].id > maxid)
         maxid = g_slot[i].id;

   /* An id must fit the 16-bit `src` field of struct reading (see store.h).
    * Past 65535 the narrowing cast wraps and id 65536 aliases legacy id 0,
    * so readings would be silently reattributed to a different physical
    * device -- the one failure this whole design exists to make impossible.
    * Refusing to mint stops new data rather than corrupting the record: the
    * sensor shows as unregistered, which is visible, rather than quietly
    * borrowing another device's identity. Unreachable in practice (a few
    * mints per sensor per year), but it is an invariant, not an estimate. */
   /* ONE ID SPACE, AND EVERY TABLE COVERS ALL OF IT. The ceiling is the
    * registry's own capacity, which is the tighter of the two bounds an id
    * has to satisfy (the other is the 16-bit `src` field): g_style and
    * g_src_last are indexed
    * BY ID and sized MAX_SLOTS + 1, so an id past that gets no styling and no
    * last-seen -- and the plot draws an unstyled source with the ORPHAN look,
    * which asserts "this sensor no longer exists" about a device being worn.
    * Bounding the mint here and the loader below by the same number makes
    * every id an index those tables hold. */
   if (maxid + 1 > MAX_SLOTS) {
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
   /* THE RECORD TABLE IS PART OF THE PUBLISHED VIEW TOO. reg_write_end is
    * the choke point for the SLOT table and this path does not pass through
    * it, so the publish belongs here: a minted row invisible to readers is a
    * sensor the advert path does not recognise as its own. */
   view_publish_locked();
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
   if (again) {
      g_srec[at] = r;
      /* Same reason as the mint: this path never reaches reg_write_end, and
       * the attributes it completes -- model, firmware, serial, activation --
       * are exactly what the per-device screen and the wear budget read. An
       * unpublished completion leaves both showing the guess for ever. */
      view_publish_locked();
   }
   reg_write_abort(); /* releases both; nothing further to write */
   return again ? 1 : 0;
}

int sensor_claim_slot(int id, int type, const char *identity)
{
   /* THE SAME BOUND THE PARSER ENFORCES. A row this refuses to create is one
    * slots_parse would reject as damage on the next launch -- and the startup
    * orphan walk takes its id from the provenance table, so without this the
    * two disagree and every launch writes a row the next one throws away. */
   if (id <= 0 || id > MAX_SLOTS)
      return -1;
   reg_write_begin();
   slots_snapshot();
   struct sensor_slot *have = slot_ptr_by_id(id);
   if (have) {
      int idx = (int)(have - g_slot);
      /* Re-adding a device that was DISCONNECTED revives its existing slot
       * -- keeping the marker/label/colour the user chose -- rather than
       * leaving it stranded as an old device with a duplicate live one. */
      if (have->old) {
         have->old = 0;
         /* THE SLOT'S OWN TYPE, not the caller's argument. sensor_mint
          * matches an existing slot by ADDRESS alone (deliberately -- one
          * physical device keeps one id), so a CGM add flow at a registered
          * meter's address arrives here with type = a CGM and would make the
          * METER primary, which sensor_set_primary then refuses to create.
          * sensor_revive and sensor_set_primary both ask the provenance row;
          * so does this. */
         const struct sensor_rec *hr = sensor_rec_by_id(have->id);
         if (hr && sensor_kind(hr->type) == KIND_CGM &&
             sensor_primary_slot() < 0)
            have->primary = 1;
         if (reg_write_end() != SENSOR_OK)
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
   s.color =
       (g_nslot == 0) ? (SET_NCOLORS - 1) : ((g_nslot - 1) % (SET_NCOLORS - 1));
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
   /* First CGM paired becomes primary, so the big number always has an owner
    * -- but ONLY when the choice can be checked. Without provenance the other
    * slots' types are unknown, so "nothing else holds it" cannot be trusted,
    * and the two operations that would take the flag back (retire, forget)
    * refuse in that state, which would make this claim permanent. */
   if (g_srec_loaded && sensor_kind(type) == KIND_CGM &&
       sensor_primary_slot() < 0)
      s.primary = 1;
   g_slot[g_nslot++] = s;
   /* A CLAIM THAT WAS NOT WRITTEN IS NOT A CLAIM. Returning the index anyway
    * lets commit_pair erase a key file and bond a sensor whose slot will be
    * gone at the next launch -- the pairing then has to be done again, with
    * the key it destroyed. -1 is the same answer as
    * "slots full", which every caller already handles. */
   int at  = g_nslot - 1; /* read before the undo can move it back */
   int rc  = reg_write_end();
   int idx = rc == SENSOR_OK ? at : -1;
   return idx;
}

int sensors_writable(void)
{
   /* READ WITHOUT THE LOCK, like g_view_stale beside it: a plain int written
    * under regfile_lk by whichever thread loaded (startup, or the sync worker
    * through pancra_logs_reload) and read by the frame and by commit_pair. A
    * reader racing the store sees the old answer for one frame; both answers
    * are safe, because the WRITE path tests it again under both locks. */
   return g_slots_loaded;
}

int sensors_view_stale(void)
{
   return g_view_stale;
}

int sensors_provenance_loaded(void)
{
   return g_srec_loaded;
}

int sensors_provenance_covers(void)
{
   return g_srec_covers;
}

int sensors_slots_whole(void)
{
   return g_slots_whole;
}

int sensor_forget(int id)
{
   reg_write_begin();
   slots_snapshot();
   int idx = slot_of_id_locked(id);
   if (idx < 0) {
      reg_write_abort();
      return SENSOR_UNSAVED;
   }
   /* NOTE: the DISCONNECT flow uses sensor_retire (which keeps the slot and
    * its appearance); this hard-delete is for a true removal, and for the
    * rollback in main.c when a claim could not be completed. */
   if (g_slot[idx].primary && !g_srec_loaded) {
      reg_write_abort(); /* same reason as sensor_retire: no successor */
      return SENSOR_UNSAVED;
   }
   int was_primary = g_slot[idx].primary;
   for (int i = idx + 1; i < g_nslot; i++)
      g_slot[i - 1] = g_slot[i];
   g_nslot--;
   /* Never leave the big number ownerless -- through the ONE function that
    * knows the rule. Written out again here, a copy that forgets the `old` test
    * hands the flag to a RETIRED slot, which slots_parse clears at the next
    * launch and sensor_set_primary refuses to create. Two answers to "who is
    * primary now" is one too many. */
   if (was_primary)
      reassign_primary_locked();
   return reg_write_end();
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

int sensors_view_find_mac(const struct sensor_view *v, const char *mac)
{
   return sensors_view_find_mac_kind(v, mac, -1);
}
