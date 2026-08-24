// SPDX-License-Identifier: GPL-3.0
// model.c --- Live state becomes one immutable frame (see model.h)
// Copyright 2026 Jakob Kastelic

#include "model.h"
#include "alarm.h"
#include "alarmlogic.h"
#include "bondtable.h" /* the OS bond state a device row shows */
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "exercise.h"
#include "food.h"
#include "forms.h"
#include "insulin.h"
#include "linkinfo.h" /* the per-link RSSI the frame shows */
#include "menuview.h" /* the read-only menu snapshot the frame copies */
#include "meter.h"
#include "nav.h"
#include "pairing.h"
#include "plotdata.h"
#include "reconcile.h"
#include "remote.h" /* the last sync outcome: the syncing module owns it */
#include "remotecfg.h"
#include "sensors.h"
#include "sesscache.h" /* the session clock a restart shows before the sensor answers */
#include "settings.h"
#include "shell.h"
#include "stats.h"
#include "store.h"
#include "style.h"
#include "sync.h"
#include "syncstat.h"
#include "thread.h" /* the status line has two threads; see g_status */
#include "tzoff.h"
#include "uifmt.h"
#include "uimodel.h"
#include "util.h"
#include "weight.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct frame_ctx; /* defined below: everything one frame is made of */
static void snap_registry(struct frame_ctx *f);
static void snap_drivers(struct frame_ctx *f);

#define MAX_LINES 16 /* text lines on the pre-reading status screen */
#define MAX_COLS  33 /* character columns the UI lays out to */

/* THE STATUS LINE, AND THE TWO READERS THAT CANNOT SHARE ONE BUFFER.
 *
 * set_status runs on whichever thread had something to say -- the BLE binder
 * thread, mostly -- and the frame builder copies it out on the looper. Those
 * are two threads touching one char array with no synchronisation at all,
 * which is a data race in the language's own terms: not merely "might read
 * torn text", but undefined behaviour that a compiler is entitled to assume
 * cannot happen. In practice it is torn text -- half of one phase message and
 * half of the next, or a byte-loop copy that reaches the end without ever
 * seeing the NUL.
 *
 * SO THERE ARE TWO PUBLICATIONS, and they are different because their readers
 * are different:
 *
 *   g_status, under g_status_lk. Everything that runs in ordinary program
 *   context -- set_status, the frame builder, update_screen's own comparison
 *   -- goes through the lock, and gets a whole string every time. A leaf
 *   lock: it is taken while nothing else is held and nothing is called under
 *   it.
 *
 *   g_status_crash, written but never locked. The crash handler runs in a
 *   SIGNAL, where taking a mutex is undefined if the interrupted thread
 *   already holds it -- and the interrupted thread is very often the one that
 *   was setting the status. So the handler reads its own copy, published
 *   after the locked one with a release store, and the worst it can see is a
 *   message one update old. An old message is a fact; a deadlocked crash
 *   handler is no crash report at all. */
static struct mutex g_status_lk    = MUTEX_INIT;
static char g_status[MAX_COLS + 1] = "STARTING";
/* THE SIGNAL-SAFE COPY, one atomic byte at a time.
 *
 * Written NUL-FIRST, so a handler that interrupts the write below sees either
 * the previous message whole or this one whole -- never a mixture, because
 * every instant of the array is a valid C string.
 *
 * _Atomic char, not volatile char, and the difference is the same one
 * crashlog.h makes about the scalars: this array is written by whatever
 * thread set the status (a binder thread, the sync worker, the looper) and
 * read from a signal handler that can interrupt any of them. `volatile` tells
 * the compiler not to elide or reorder the stores; it does not make the
 * program's shared access defined, and a relaxed atomic byte costs the same
 * instruction while needing no argument about the target.
 *
 * NOT INITIALISED TO A LITERAL: C has no way to initialise an array of
 * atomics from a string, so it starts empty and the first set_status() puts
 * a message in it. A crash before the first set_status therefore
 * reports an empty status, which is true -- nothing had been said yet. */
static _Atomic char g_status_crash[MAX_COLS + 1];

/* A whole status line, or nothing. The one reader every in-context caller
 * uses; the crash handler has its own copy and must not come through here. */
static void status_copy(char *dst, int cap)
{
   mutex_lock(&g_status_lk);
   str_snapshot(dst, cap, g_status);
   mutex_unlock(&g_status_lk);
}

/* The plot span the user picked, in hours. The tab row that sets it, and its
 * hit boxes, live in the renderer (app/ui*.c). */
static int g_plot_hours = 3;

int model_plot_hours(void)
{
   return g_plot_hours;
}

void model_set_plot_hours(int hours)
{
   g_plot_hours = hours;
}

static char g_lines[MAX_LINES][MAX_COLS + 1];
static int g_nlines;

static int g_prog_shown; /* the eased sync fraction, per mille */

/* Per-frame REGISTRY snapshot, taken before the draw flag.
 *
 * The renderer does not read the slot array directly, nor hold a POINTER into
 * the provenance cache across a run of field copies with only the draw flag
 * held. (Both are behind copy queries -- see sensors.h -- so that shape is not
 * expressible; this snapshot exists because the frame needs one consistent
 * instant, not merely one safe read at a time.) Both are mutated
 * from a binder thread -- srec_push shifts g_srec to keep it id-ordered,
 * reachable via ot_drv_done -> sensor_mint. A shift landing mid-copy renders
 * a garbled or mixed sensor row, and resolving a slot to its link by identity
 * (driver_link_of_identity) against a shifting one can match the WRONG link,
 * putting one sensor's session age and CONNECTED state on another's row.
 *
 * It cannot be fixed by locking the registry in the renderer: that would nest
 * reg INSIDE hist and invert the documented driver -> reg -> hist order. So
 * snapshot first, exactly as snap_drivers() does for the driver state, and for
 * the same reason -- the main thread must never hold one of these locks while
 * waiting for another. */
struct snap_slot {
   long paired, activation;
   int id, marker, color, primary, size, wear_days, old, type, have_rec;
   char label[20];
   char mac[24], serial[24], model[24], fw[24];
};

/* The candidate list is the pairing workflow's; this is only how many of its
 * rows the device screen can show at once. */
#define UI_DEVS_MAX 12

/* ---- THE FRAME, AS ONE NAMED THING -------------------------------------
 *
 * Everything a frame is built OUT OF, and everything it is built INTO that
 * the renderer then borrows, lives here. Two properties are worth stating
 * because a dozen file-scope arrays and a pair of unscoped macros cannot
 * state either:
 *
 * ONE INSTANT. Every input is copied in at a known point -- the registry and
 * the driver in model_snapshot(), before the history lock; the settings, the
 * identity, the menus and the forms at the top of build_model() -- and
 * nothing below reads a live source again. A builder that took its own copy
 * would describe a different instant from the row above it.
 *
 * ONE LIFETIME. The renderer is handed POINTERS into this struct (the point
 * list, the device rows, the log tails, the borrowed strings), and they must
 * stay valid until the frame is drawn. That is why it is file-static rather
 * than a local: the storage outlives build_model by design, and the next
 * frame overwrites it. Only the main thread builds a model, so one is enough
 * -- and `struct frame_ctx *f` threaded through the builders is what says so
 * in each signature rather than leaving it to be inferred. */
struct frame_ctx {
   /* Taken by model_snapshot(), BEFORE the history lock (see there). */
   struct snap_slot slot[MAX_SLOTS];
   int nslot;
   struct dex_session sess[LINK_MAX];
   struct dex_cal cal;

   /* Taken at the top of build_model(), under that lock. */
   long now;
   struct prefs prefs;
   struct sync_creds creds;
   struct menu_view mv;
   struct forms_view fv;

   /* Frame-owned storage the renderer BORROWS. Sized from the tails' own
    * bounds so a table that grows cannot silently start truncating here. */
   struct ui_point pts[UI_PTS_MAX];
   struct ui_dev devs[UI_DEVS_MAX];
   struct ui_sensor sens[MAX_SLOTS];
   struct reading hist[NHIST];
   struct ins_rec inslog[NINS];
   struct wt_rec wtlog[NWT];
   struct food_type ftypes[NFOODTYPE];
   struct food_rec foodlog[NFOOD];
   struct ex_rec exlog[NEX];
   char mac[24];
   char entry[64]; /* checked against forms_view::entry below */
};

static struct frame_ctx g_frame;

/* Driver state for the frame is captured before the draw flag is taken, into
 * the context above.
 *
 * build_model() must not call driver_lock() while draw() holds the history
 * lock: the BLE side takes them the other way round (driver_lock -> hist_lock
 * inside driver_on_notify -> drv_glucose). Two spin locks acquired in opposite
 * orders is an unrecoverable hang, and it needs only a reading landing during
 * a 1 Hz repaint -- i.e. steady-state operation. Snapshotting here means the
 * main thread never holds one lock while waiting for the other. */
static void snap_registry(struct frame_ctx *f)
{
   /* ONE registry snapshot, taken by the registry itself. Assembled here out
    * of per-row calls it would be a walk whose rows can come from different
    * instants: a binder thread minting a sensor moves them underneath the
    * loop -- which is the whole reason the frame snapshots the registry at
    * all. */
   struct sensor_view v;
   sensors_view_get(&v);
   f->nslot = v.n < MAX_SLOTS ? v.n : MAX_SLOTS;
   for (int i = 0; i < f->nslot; i++) {
      const struct sensor_slot *sl = &v.slot[i];
      const struct sensor_rec *r   = v.have_rec[i] ? &v.rec[i] : 0;
      struct snap_slot *d          = &f->slot[i];
      d->id                        = sl->id;
      d->marker                    = sl->marker;
      d->color                     = sl->color;
      d->primary                   = sl->primary;
      d->size                      = sl->size;
      d->wear_days                 = sl->wear_days;
      d->old                       = sl->old;
      str_snapshot(d->label, sizeof d->label, sl->label);
      d->have_rec   = (r != 0);
      d->type       = r ? r->type : 0;
      d->paired     = r ? r->paired : 0;
      d->activation = r ? r->activation : 0;
      str_snapshot(d->mac, sizeof d->mac, r ? r->identity : "");
      str_snapshot(d->serial, sizeof d->serial, r ? r->serial : "");
      str_snapshot(d->model, sizeof d->model, r ? r->model : "");
      str_snapshot(d->fw, sizeof d->fw, r ? r->fw : "");
   }
}

/* THE ID OF THE ROW THE FRAME DREW, or -1.
 *
 * A tap carries a ROW NUMBER, because that is what a finger can point at --
 * and by the time it arrives the registry may have shifted under it, so
 * asking the LIVE registry what is at that row answers with the new
 * occupant rather than the device the user saw. The frame's own snapshot is
 * the picture they touched, so the row is resolved against THAT, once, and
 * everything afterwards carries the id. */
int model_snap_id(int row)
{
   const struct frame_ctx *f = &g_frame;
   if (row < 0 || row >= f->nslot)
      return -1;
   return f->slot[row].id;
}

/* Draw-path variant: resolves from the per-frame snapshot rather than the live
 * driver, because the renderer must never take driver_lock while holding the
 * draw flag -- that inversion is what caused an unrecoverable hang. */
static int snap_shell_link_for_slot(const struct frame_ctx *f, int idx)
{
   if (idx < 0 || idx >= f->nslot)
      return -1;
   const struct snap_slot *d = &f->slot[idx];
   if (!d->have_rec)
      return -1;
   /* By ADDRESS for meters too: they hold their own links now, so there is
    * no single link to return for "the meter". */
   for (int l = 0; l < LINK_MAX; l++)
      if (f->sess[l].mac[0] && strcmp(f->sess[l].mac, d->mac) == 0)
         return l;
   return -1;
}

static void snap_drivers(struct frame_ctx *f)
{
   /* One lock for the whole snapshot, so the frame sees all the links as they
    * were at one instant. Each read names its own link, so the snapshot no
    * longer moves the ambient selection and no longer has to put it back --
    * which it did by assuming LINK_CGM rather than by remembering. */
   struct menu_view mv;
   menu_view_get(&mv);
   /* ONE OPERATION, so the frame sees every link as it was at one instant --
    * and so this file does not reason about the driver's lock at all. */
   int cal_link = link_for_sensor(mv.sel_id);
   driver_snapshot(f->sess, cal_link, &f->cal);
}

/* How many CGMs are registered. Above one, a reading whose link resolves to no
 * slot cannot be safely attributed to the global "current source". */
/* Fill one ui_sensor from its slot + provenance + (if it is the live one) the
 * driver session. */
/* ONE READING of the preferences and the identity, for the WHOLE frame: they
 * are in the context (above) because fill_sensor fills every per-slot row
 * from them and has to describe the SAME instant -- a copy per row is both
 * repeated work and a second instant, and one row could then show a pairing
 * code the row above it does not. */
static void fill_sensor(struct frame_ctx *f, struct ui_sensor *u, int i,
                        long now)
{
   /* From the pre-draw snapshot, never the live registry -- see snap_registry.
    */
   if (i < 0 || i >= f->nslot)
      return;
   const struct snap_slot *sl = &f->slot[i];
   *u                         = (struct ui_sensor){0};
   u->id                      = sl->id;
   u->marker                  = sl->marker;
   u->color                   = sl->color;
   u->primary                 = sl->primary;
   u->old                     = sl->old;
   u->size =
       (sl->size >= 1 && sl->size <= MARK_SIZE_MAX) ? sl->size : MARK_SIZE_DEF;
   str_snapshot(u->label, sizeof u->label, sl->label);
   if (sl->have_rec) {
      u->type = sl->type;
      u->kind = sensor_kind(sl->type);
      str_snapshot(u->mac, sizeof u->mac, sl->mac);
      str_snapshot(u->serial, sizeof u->serial, sl->serial);
      str_snapshot(u->model, sizeof u->model, sl->model);
      str_snapshot(u->fw, sizeof u->fw, sl->fw);
   }
   /* This DEVICE's wear budget AND where it came from, as ONE answer from
    * the registry (see sensor_wear_of). Deciding "was this pinned?" here, by
    * re-testing wear_days against the values the resolver happens to accept,
    * is a second copy of that rule -- and the row would then label a correct
    * duration AUTO the day a third override becomes valid. */
   struct sensor_wear wear = sensor_wear_of(u->type, sl->wear_days, sl->model);
   u->wear_len             = wear.seconds;
   u->wear_auto            = !wear.pinned;
   u->wear_prov            = wear.provisional;
   u->paired               = sl->paired;
   u->activation           = sl->activation;
   /* The OS bond, keyed by the address the framework knows this device by --
    * for a G7 that is the BOND IDENTITY address, which is what the snapshot's
    * mac holds (a rotating RPA would never match a bond record). */
   u->bond = dexble_bond_state(sl->mac);
   /* newest reading from this source, for the "last seen" column */
   for (int k = 0; k < hist_count(); k++)
      if (hist_at(k).src == (unsigned short)sl->id) {
         u->last  = hist_at(k).t;
         u->glu   = hist_at(k).glu;
         u->trend = hist_at(k).trend;
         break;
      }
   if (u->kind == KIND_CGM) {
      /* Every CGM has its own link and its own driver context, so each row
       * reports that sensor's real session. Read from the pre-draw snapshot --
       * taking driver_lock() here would nest it inside the draw flag. */
      int sl_link = snap_shell_link_for_slot(f, i);
      struct dex_session s =
          (sl_link >= 0) ? f->sess[sl_link] : (struct dex_session){0};
      /* LIVE WINS; the cache only fills the gap before this link's first 0x4e
       * of the process. Both directions in one place, so the per-device screen
       * and the top block cannot disagree about a sensor's session. */
      /* READ-ONLY, BOTH WAYS. A sessc_put in the live branch would make
       * building a row MUTATE the cache -- its table, its generation, and
       * eventually the file -- so how often the screen redrew would decide
       * what survives the next launch, and a service
       * with no activity to draw for recorded nothing at all. The recording
       * is sensor_reconcile's now, on a cadence of its own; what is left
       * here is a lookup. */
      if (!s.have_reading)
         (void)sessc_restore(sl->id, now, &s);
      u->connected       = s.bonded && (now - u->last) < AL_FRESH_S;
      u->session_seconds = (long)s.session_seconds;
      u->sess_state      = s.state;
      u->predicted       = s.predicted;
      u->sequence        = s.sequence;
      /* THIS link's signal, not the last one measured on any link. */
      {
         int lk     = snap_shell_link_for_slot(f, i);
         int rdbm   = 0;
         long rwhen = 0;
         int haveit = linkinfo_rssi(lk, &rdbm, &rwhen);
         u->rssi    = rdbm;
         u->rssi_ok = haveit;
         u->rssi_t  = rwhen;
      }
      /* The pairing code is the GLOBAL last-entered one (settings.h),
       * not a per-device secret we persist -- so only show it on a LIVE CGM
       * (where it is at least the code in current use). An OLD device would
       * otherwise display a live sensor's code, which is why both G7s appeared
       * to share one. */
      if (!sl->old) {
         /* FROM THE FRAME'S OWN COPY (ps, taken once in build_model), not a
          * fresh one: this runs per slot inside the frame loop, so a copy
          * here is both repeated work and a second instant -- one row could
          * show a code the row above it does not. */
         str_snapshot(u->code, sizeof u->code, f->prefs.code_str);
      }
      /* WARMUP: a freshly paired sensor delivers nothing for about its first
       * hour BY DESIGN -- without saying so, that silence reads as broken
       * and the user hovers over a working sensor. Keyed off the pairing
       * instant, since the session clock is unknown until the first EGV. */
      /* The SENSOR'S OWN state byte is authoritative (warmup readings are
       * recorded and displayed, so "has no data" no longer implies warmup);
       * with no 4e seen yet, fall back to the session clock, then to the
       * pairing instant. */
      /* DISPLAY POLICY for a device's lifecycle, applied everywhere the
       * same: WARMUP while the sensor says so (readings recorded anyway),
       * CONNECTED/WAITING while live, ENDED once the sensor's own state
       * byte says the session is over. History always keeps the device's
       * colours; only FORGETTING a device orphans its points to grey. */
      /* AN UNANSWERED PAIRING DIALOG OUTRANKS EVERYTHING BELOW.
       *
       * It rides on the STATUS string rather than a row of its own: the
       * per-device screen is already at its row ceiling, and one more row
       * there drops ui_fit_scale over its cliff and shrinks the whole screen's
       * text (see the note at render_sensor). The state is genuinely a status
       * -- the device is registered and doing nothing, and this says why --
       * so the row that reports status is where it belongs. */
      if (u->bond == UI_BOND_BONDING)
         str_snapshot(u->status, sizeof u->status, "CONFIRM PAIRING");
      else if (u->sess_state == SENSOR_STATE_ENDED)
         str_snapshot(u->status, sizeof u->status, "ENDED");
      else if (u->sess_state == SENSOR_STATE_WARMUP ||
               (u->sess_state == 0 && u->last == 0 &&
                ((u->session_seconds > 0 &&
                  u->session_seconds < SENSOR_WARMUP_S) ||
                 (u->session_seconds == 0 && sl->paired > 0 &&
                  now - sl->paired < SENSOR_WARMUP_S))))
         str_snapshot(u->status, sizeof u->status, "WARMUP");
      else
         str_snapshot(u->status, sizeof u->status,
                      u->connected ? "CONNECTED" : "WAITING");
      /* Calibration and rescale for the LAST CAL / RESCALE rows, taken in one
       * call so the row cannot show a queued value beside the result that
       * just superseded it. */
      struct calib_view cv;
      calib_view(u->id, &cv);
      u->cal_pending      = cv.queued_mgdl;
      u->cal_mgdl         = cv.last_mgdl;
      u->cal_state        = cv.last_state;
      u->cal_t            = cv.last_t;
      u->rescale_pm       = cv.rescale_pm;
      u->rescale_pending  = cv.rescale_pending;
      u->rescale_rejected = cv.rescale_rejected;
      u->rescale_expired  = cv.rescale_expired;
      u->cal_unsaved      = cv.cal_unsaved;
      u->rescale_unsaved  = cv.rescale_unsaved;
   } else {
      /* PER-METER, so syncing one meter never rewrites another's row. Only the
       * meter that currently OWNS the sync (meter_src()) shows SYNCING and the
       * live RSSI; each meter's "last" and SYNCED/OFF come from ITS OWN reading
       * history (u->last, set above from g_hist), not the shared session
       * globals. u->last is persisted, so STATE is correct after a restart too.
       */
      struct meter_ui mu;
      meter_ui_of(u->id, &mu);
      u->connected = mu.syncing;
      /* LAST SYNC (when the app last connected THIS meter) is separate from
       * u->last, which is its last DATAPOINT (fingerstick). */
      u->meter_sync_t = mu.sync_t;
      if (mu.syncing)
         /* Live handshake step (HELLO/COUNT/READING/...) if the driver has
          * reported one this sync, else a plain SYNCING while connecting. */
         str_snapshot(u->status, sizeof u->status,
                      mu.stat[0] ? mu.stat : "SYNCING");
      else if (mu.stat[0])
         /* Terminal result of the last sync (SYNCED / NOTHING NEW / NOT PAIRED
          * / REFUSED / BAD DATA) -- more informative than a bare SYNCED. */
         str_snapshot(u->status, sizeof u->status, mu.stat);
      else if (u->meter_sync_t > 0 || u->last > 0)
         str_snapshot(u->status, sizeof u->status, "SYNCED");
      else
         str_snapshot(u->status, sizeof u->status, "OFF");
      /* This meter's OWN last RSSI (kept across the meter powering off
       * between syncs), not tied to a datapoint -- and out of the SAME copy
       * as everything above, so the row cannot show a last-sync time from
       * one instant beside a signal from another. */
      u->rssi_ok = mu.rssi_ok;
      u->rssi    = mu.rssi;
      u->rssi_t  = mu.rssi_t;
   }
}

/* THE BIG NUMBER AND THE SENSOR BEHIND IT: the current reading as one
 * triple, the primary CGM's session, and whether any live CGM exists at all.
 * Everything comes out of the frame's own snapshot, so the number, its age
 * and the session under it describe one instant */
static void build_reading(struct frame_ctx *f, struct screen *m)
{
   const long now = f->now;
   /* Read directly, and consistently, because draw() holds the history lock
    * across this whole frame (see the note below on g_hist). store_now would
    * take that same non-recursive lock and deadlock; what it offers an
    * UNLOCKED caller -- one coherent triple -- the caller's lock already
    * gives this one. */
   /* THE CURRENT READING AS ONE TRIPLE, through the locked variant: draw()
    * holds the store lock across this whole frame and it is not recursive, so
    * the unlocked store_now would deadlock. Reading the three by hand is what
    * pairs a new glucose with the previous timestamp. */
   struct reading_now cur   = store_now_locked(now);
   struct reading_rssi crss = store_rssi_locked();
   m->reading.glu           = cur.glu;
   m->reading.trend         = cur.trend;
   m->reading.t             = cur.t;
   m->reading.rssi          = crss.dbm;
   m->reading.rssi_ok       = crss.ok;
   m->reading.stale         = cur.stale;
   m->reading.disc_alarmed  = alarm_disc_latched();

   /* The PRIMARY CGM drives the top block -- resolved to ITS link, not
    * hardcoded LINK_CGM (link 0), which just belongs to whichever CGM claimed
    * it first. With the Stelo on link 0 and a G7 primary on another link,
    * the STATE/SESSION/PRED rows mixed the G7's 10-day budget with the
    * STELO's session clock: 15 days in vs 10 d + 12 h grace computed as past
    * even the grace period, so the SESSION row said ENDED while the primary
    * was nowhere near its end. A primary with no live session yet (freshly
    * committed pairing) reports an EMPTY session -- SESSION --, consistent
    * with the cleared big number, never another sensor's numbers. */
   struct dex_session s = f->sess[LINK_CGM];
   for (int i = 0; i < f->nslot; i++)
      if (f->slot[i].primary) {
         int pl = snap_shell_link_for_slot(f, i);
         s      = (pl >= 0) ? f->sess[pl] : (struct dex_session){0};
         /* Same restore as the per-device rows: without it PRED and the
          * session countdown under the big number blanked for a whole
          * five-minute cadence after every launch, while the reading and its
          * age beside them came straight back from readings.csv. */
         if (!s.have_reading)
            (void)sessc_restore(f->slot[i].id, now, &s);
         break;
      }
   m->reading.bonded          = s.bonded;
   m->reading.have_reading    = s.have_reading;
   m->reading.predicted       = s.predicted;
   m->reading.sequence        = s.sequence;
   m->reading.sess_state      = s.state;
   m->reading.session_seconds = (long)s.session_seconds;
   /* Whether ANY CGM is registered -- from the snapshot, so it is consistent
    * with the sensor rows below. The STATE/SESSION/PRED block is CGM-only;
    * with none, the renderer blanks it rather than echoing the meter's status.
    */
   /* A LIVE (non-old) CGM. Old/disconnected CGMs don't count -- with none
    * live the STATE/SESSION/PRED block and the big-number age blank out. */
   m->reading.has_cgm = 0;
   for (int i = 0; i < f->nslot; i++)
      if (f->slot[i].have_rec && !f->slot[i].old &&
          sensor_kind(f->slot[i].type) == KIND_CGM) {
         m->reading.has_cgm = 1;
         break;
      }

   /* THE ADDRESS THIS READING CAME FROM, copied into frame-owned storage: `s`
    * is on this stack and the renderer borrows the pointer until the frame is
    * drawn. build_devices publishes it. */
   str_snapshot(f->mac, sizeof f->mac, s.mac);
}

/* THE POINT LIST, which is glucose, doses, weights, food and exercise in ONE
 * array (plot_render and plot_hit take points in any order, and the scrub
 * index the UI hands back has to index a single list), plus the streak the
 * plot is captioned with */
static void build_plot(struct frame_ctx *f, struct screen *m)
{
   const long now = f->now;
   /* g_hist is read WITHOUT an explicit hist_lock() here, and that is correct,
    * not an oversight: draw() holds g_hist_lk across draw_impl -> build_model,
    * so any BLE thread entering hist_insert waits until this frame is done.
    * Adding hist_lock() here would SELF-DEADLOCK -- unlike driver_lock and
    * reg_lock, this one is not recursive (struct mutex, not struct rmutex).
    * An adversarial review flagged the missing lock; acting on it would have
    * wedged the app on the first repaint. */
   int nlong = 0;
   const struct ui_point *plong =
       plot_source_from(store_path(), now, model_plot_hours(), &nlong);
   int nh = 0;
   if (plong) {
      /* A long span: one column per pixel, straight from the log, so its
       * depth does not depend on how many points happen to fit in RAM. */
      for (int i = 0; i < nlong && nh < PLOT_LONG_MAX; i++)
         f->pts[nh++] = plong[i];
   } else {
      nh = hist_count() < NHIST ? hist_count() : NHIST;
      for (int i = 0; i < nh; i++) {
         f->pts[i].t    = hist_at(i).t;
         f->pts[i].glu  = hist_at(i).glu;
         f->pts[i].src  = hist_at(i).src;
         f->pts[i].kind = hist_at(i).kind;
      }
   }
   /* Insulin doses ride along as KIND_INS points (glu = units; the renderer
    * pins them to the plot's bottom edge and scrubs them as "N UNITS").
    * plot_render and plot_hit take points in any order, so appending
    * after the newest-first glucose is fine. They are NEVER in g_hist,
    * so they cannot leak into stats or the remote push. */
   int nins = ins_count();
   for (int i = 0; i < nins && nh < UI_PTS_MAX; i++) {
      struct ins_rec ir = ins_at(i);
      f->pts[nh].t      = ir.t;
      /* THOUSANDTHS, carried through the plot point unchanged: the scrub
       * readout renders it with ins_units_str, so a half-unit dose reads
       * "0.5 U" there rather than being flattened on the way in. */
      f->pts[nh].glu    = ir.milli;
      f->pts[nh].src    = ir.type; /* the scrub shows "2U FAST" etc. */
      f->pts[nh].kind   = KIND_INS;
      nh++;
   }
   /* Logged WEIGHTS ride along the same way, on the same bottom line, as a
    * small W. Carried in GRAMS -- the log's canonical unit -- so the scrub
    * renders them in whichever display unit is set at the time, exactly as
    * the weight table does. Like the doses these are never in g_hist, so they
    * cannot leak into TIR, the average or the remote push. */
   int nwt = wt_count();
   for (int i = 0; i < nwt && nh < UI_PTS_MAX; i++) {
      struct wt_rec wr = wt_at(i);
      f->pts[nh].t     = wr.t;
      f->pts[nh].glu   = (int)wr.g; /* grams; WT_MAX_G is 400000, fits an int */
      f->pts[nh].src   = 0;
      f->pts[nh].kind  = KIND_WT;
      nh++;
   }
   /* Logged FOOD rides along exactly as the doses and weights do, on the same
    * bottom line, as a small F. Its `glu` carries GRAMS -- meaningless on a
    * glucose axis, which is why the point is pinned to 60 by the renderer and
    * the scrub reads the real value back out of hist. `src` carries the TYPE
    * ID so the scrub can name the food; an id the vocabulary no longer holds
    * renders as an empty name rather than a number. */
   int nfd = food_count();
   for (int i = 0; i < nfd && nh < UI_PTS_MAX; i++) {
      struct food_rec fr = food_at(i);
      f->pts[nh].t       = fr.t;
      f->pts[nh].glu     = (int)fr.g; /* grams; FOOD_MAX_G fits an int */
      f->pts[nh].src     = fr.type;
      f->pts[nh].kind    = KIND_FOOD;
      nh++;
   }
   /* Logged EXERCISE rides the same bottom line as a small E, and is the one
    * entry here that is not an instant: `glu` carries the INTENSITY and `src`
    * the LENGTH IN SECONDS, which the renderer turns into a rule drawn from
    * the letter to where the session ended.
    *
    * A RUNNING SESSION IS MEASURED AGAINST THE CLOCK. The row that opened it
    * has no length yet -- one is written when the user ends it -- so a length
    * for the newest row is derived here, and only when the button is actually
    * still up. Without that test a session the app was killed during would
    * grow a rule for ever, which is a claim about exercise that never
    * happened rather than about one still happening. */
   int nex     = ex_count();
   int ex_live = 0;
   {
      int lvl = 0;
      int rem = 0;
      exercise_button_get(mono_s(), &lvl, &rem);
      ex_live = lvl != 0;
   }
   for (int i = 0; i < nex && nh < UI_PTS_MAX; i++) {
      struct ex_rec er = ex_at(i);
      long dur         = er.dur;
      if (dur == 0 && i == nex - 1 && ex_live) {
         dur = now - er.t;
         if (dur < 0 || dur > EX_DUR_MAX)
            dur = 0;
      }
      f->pts[nh].t    = er.t;
      f->pts[nh].glu  = er.level;
      f->pts[nh].src  = (int)dur;
      f->pts[nh].kind = KIND_EX;
      nh++;
   }
   m->plot.hist       = f->pts;
   m->plot.nhist      = nh;
   m->plot.scrub      = f->fv.scrub;
   m->plot.plot_hours = model_plot_hours();
   m->plot.plot_max   = f->prefs.plot_max;
   /* THE IN-RANGE STREAK. Computed from the reading history, not from the
    * point list above -- that one carries doses, weights and food mixed in,
    * and a 90-gram meal is not a glucose value. The band is the CLINICAL one
    * (70-180, TIR's own), not the configured alarm band, so the streak and
    * the TIR figure below it are measured against the same range and cannot
    * disagree about a sample; a gap longer than four missed samples ends it
    * rather than counting through time nobody measured. */
   {
      /* hist_at, NOT hist_copy -- and this is the self-deadlock the comment
       * above spells out. hist_copy takes g_hist_lk itself, draw() already
       * holds it across build_model, and the lock is not recursive: the first
       * repaint wedged the app on the splash screen with the process alive
       * and nothing drawn. Reading through hist_at is what every other walk
       * in this function does, for exactly this reason. */
      int hn = hist_count();
      if (hn > NHIST)
         hn = NHIST;
      for (int i = 0; i < hn; i++)
         f->hist[i] = hist_at(i);
      m->plot.streak_s =
          alarm_streak_s(f->hist, hn, TIR_LOW_MGDL, TIR_HIGH_MGDL, 20L * 60);
      /* THE RECORD, as stored. Read here and raised nowhere near here: see
       * the note on the field, and reading.c for where it grows. A streak
       * that has just passed the record shows a record BEHIND it for one
       * frame until the next reading lands, which is the honest lag of a
       * number that is only allowed to be written from one place. */
      m->plot.best_streak_s = f->prefs.best_streak_s;
   }
}

/* THE SETTINGS THE RENDERER READS, out of the frame's one copy of them */
static void build_prefs(struct frame_ctx *f, struct screen *m)
{
   m->prefs.units       = f->prefs.units;
   m->prefs.alarm_low   = f->prefs.alarm_low;
   m->prefs.alarm_high  = f->prefs.alarm_high;
   m->prefs.nudge_low   = f->prefs.nudge_low;
   m->prefs.nudge_high  = f->prefs.nudge_high;
   m->prefs.nudge_sound = f->prefs.nudge_sound;
   m->prefs.nudge_vib   = f->prefs.nudge_vib;

   /* settings + device info (globals persist; s.mac lives on our stack, so it
    * is copied into a static the borrowed pointer can safely outlast) */
   m->prefs.sound_on     = f->prefs.sound_on;
   m->prefs.vib_on       = f->prefs.vib_on;
   m->prefs.orient       = f->prefs.orient;
   m->prefs.screen_on    = f->prefs.screen_on;
   m->prefs.newdata_mode = f->prefs.newdata_mode;
   m->sync.remote_on     = f->prefs.remote_on;
   m->sync.remote_server =
       f->prefs.remote_server; /* global: the borrow is stable */
   m->sync.sync_email   = f->creds.email;
   m->sync.sync_paired  = f->creds.uid > 0;
   m->entry.label_field = f->fv.label_field;
}

/* WHAT THE SYNC IS DOING, and what the server last said about it */
static void build_sync(struct frame_ctx *f, struct screen *m)
{
   /* SMOOTHED HERE, not in the renderer. The sync reports whole buckets, so
    * the raw fraction steps; easing it toward the target gives a bar that
    * moves continuously without ever claiming more progress than was made
    * (it only ever approaches the true value, never passes it). Keeping the
    * easing on this side leaves render_remote a pure function of the struct,
    * which is what uitest depends on. */
   {
      int pdone  = 0;
      int ptotal = 0;
      int act    = sync_progress(&pdone, &ptotal);
      int target = (ptotal > 0) ? (int)(((long)pdone * 1000) / ptotal) : 0;
      if (!act) {
         g_prog_shown = 0;
      } else if (g_prog_shown < target) {
         int step = (target - g_prog_shown) / 4;
         g_prog_shown += step > 0 ? step : 1;
         if (g_prog_shown > target)
            g_prog_shown = target;
      }
      m->sync.sync_active   = act;
      m->sync.sync_permille = g_prog_shown;
      /* Keep frames coming while it runs, or the bar would freeze between
       * whatever else happens to repaint the screen. */
      if (act)
         shell_ui_dirty();
   }
   /* A CODE, copied ONCE into a local, with the label derived from that same
    * local. A frame that borrows a pointer to a buffer the sync worker
    * rewrites leaves the renderer comparing it against a list of English
    * phrases; this carries the code, and the label comes from the one place
    * that maps codes to words (syncstat.c).
    *
    * Reading the global twice -- once for the code, once for the label --
    * would be the same defect in a smaller window: two reads of a value
    * another thread is changing, which agree almost always. One read. */
   enum sync_outcome outcome = remote_outcome();
   m->sync.remote_outcome    = outcome;
   m->sync.remote_status     = sync_outcome_label(outcome);
   m->sync.remote_port       = f->prefs.remote_port;
   m->sync.remote_last_ok    = remote_ok_time();
   m->prefs.disc             = f->prefs.disc;
   m->dev.code               = f->prefs.code_str;
   m->dev.model              = f->prefs.model;
   m->dev.fw                 = f->prefs.fw;
   m->dev.mfr                = f->prefs.mfr;
   m->dev.mac =
       f->mac; /* filled by build_reading, from the primary's session */
   for (int i = 0; i < NPERMS; i++)
      m->sys.perm[i] = f->mv.perm[i];
   m->sys.batt_ok        = f->mv.batt_ok;
   m->sys.standby_bucket = f->mv.standby_bucket;
   m->sys.bg_restricted  = f->mv.bg_restricted;
}

/* THE LINK A SENSOR IS ON, FROM THE FRAME'S OWN SNAPSHOT.
 *
 * link_for_sensor() (reconcile.c) answers the same question by taking a fresh
 * registry view and then the DRIVER LOCK -- and build_model runs with the
 * HISTORY lock held, so calling it from here nests the two in the order the
 * BLE side takes them in reverse (driver -> history inside driver_on_notify).
 * Two spin locks taken in opposite orders is an unrecoverable hang, needing
 * only a reading to land during a repaint. The comment at the top of this
 * file has said so since the day the snapshot was introduced; the path
 * survived anyway, through build_devices -> link_for_sensor, because the
 * frame's history lock is a TRYLOCK and the lock-order checker did not count
 * it as held.
 *
 * The frame already has both halves at one instant -- the slots from
 * snap_registry and the sessions from snap_drivers -- so the answer is here,
 * out of what has already been taken, and nothing under build_model reaches
 * for a lock at all. */
static int frame_link_of(const struct frame_ctx *f, int id)
{
   if (id <= 0)
      return -1;
   for (int i = 0; i < f->nslot; i++)
      if (f->slot[i].id == id)
         return f->slot[i].have_rec
                    ? driver_link_of_identity_in(f->sess, f->slot[i].mac)
                    : -1;
   return -1;
}

/* THE DEVICE ROWS AND THE CALIBRATION STATE: one ui_sensor per snapshot slot,
 * which row a detail screen is showing, and the rescale preview */
static void build_devices(struct frame_ctx *f, struct screen *m)
{
   const long now = f->now;
   /* keypad: mode + the digits typed so far (copied so the pointer is stable)
    */
   /* configured sensors, plus which one a detail screen is showing */
   /* Count from the SNAPSHOT, not the live slot_count(). Mixing the two means a
    * concurrent sensor_forget can shrink slot_count() between this loop
    * bound and the snapshot it indexes, so the last row renders whatever the
    * previous frame left in `sens` -- a sensor the user just forgot,
    * reappearing for a frame. */
   for (int i = 0; i < f->nslot; i++)
      fill_sensor(f, &f->sens[i], i, now);
   m->dev.sensors  = f->sens;
   m->dev.nsensors = f->nslot;
   /* WHICH ROW IS SELECTED, resolved from THIS frame's own snapshot: the
    * selection is a device (f->mv.sel_id), and the row it occupies is a fact
    * about the picture being drawn. Reading a stored index instead would
    * highlight, and then act on, whatever had moved into that position. */
   m->dev.sel = -1;
   for (int i = 0; i < f->nslot && m->dev.sel < 0; i++)
      if (f->slot[i].id == f->mv.sel_id)
         m->dev.sel = i;

   struct dex_cal c     = f->cal;
   m->cal.cal_have      = c.have;
   m->cal.cal_permitted = c.permitted;
   m->cal.cal_status    = c.status;
   m->cal.cal_last_bg   = c.last_bg;
   m->cal.cal_result    = c.result;
   m->cal.cal_pending   = f->fv.cal_pending;

   /* RESCALE screens. On the confirmation, preview the CLAMPED factor computed
    * from the entered value over the selected sensor's live raw; on the active
    * screen, show the running factor. */
   m->cal.rescale_entry = f->fv.rescale_entry;
   if (cur_screen() == SCR_RESCALEACT) {
      m->cal.rescale_pm = calib_rescale_pm();
   } else if (cur_screen() == SCR_RESCALE && f->fv.rescale_entry > 0 &&
              f->mv.sel_id > 0) {
      /* Preview: UNCLAMPED (so a >25% value shows its real size, in red, and is
       * rejected on CONFIRM), or the sentinel 0 when there is no reading yet to
       * compute against (the screen then says "ON NEXT READING", not "0%"). */
      m->cal.rescale_pm = calib_rescale_preview(
          f->fv.rescale_entry,
          calib_raw_on_link(frame_link_of(f, f->mv.sel_id)));
   } else {
      m->cal.rescale_pm = 1000;
   }
}

/* WHAT THE USER IS TYPING, and the log tails the forms draw beside it. Every
 * tail is COPIED into frame-owned storage: they are reloaded whenever an
 * entry is logged or edited, and a frame that borrowed one would be drawing
 * an array rewritten underneath it */
static void build_forms(struct frame_ctx *f, struct screen *m)
{
   m->entry.kp_mode = f->fv.kp_mode;
   str_snapshot(m->entry.kp_err, sizeof m->entry.kp_err, f->fv.kp_err);
   /* Type being added, for the PAIR NEW <type> / SELECT <type> titles. The
    * OneTouch shows its full name; CGMs use their short type name. */
   m->dev.add_type = (f->mv.add_type == SENSOR_ONETOUCH)
                         ? "ONETOUCH VERIO"
                         : sensor_type_name(f->mv.add_type);
   m->dev.add_kind = sensor_kind(f->mv.add_type);
   /* the picked device SCR_PAIRCONF is asking about (main-thread globals,
    * like the pairing code above) */
   m->dev.pair_name = pairing_pend_name();
   m->dev.pair_mac  = pairing_pend_mac();
   /* LOG INSULIN form state */
   m->ins.ins_t     = f->fv.ins_t;
   m->ins.ins_type  = f->fv.ins_type;
   m->ins.ins_milli = f->fv.ins_milli;
   /* A COPY, into frame-owned storage: the tail is reloaded whenever a dose
    * is logged or edited, and a frame that borrowed it would be drawing an
    * array rewritten underneath it. */
   m->ins.ins_nlog    = ins_copy(f->inslog, NINS);
   m->ins.ins_log     = f->inslog;
   m->ins.inslog_page = f->fv.inslog_page;
   m->ins.inslog_tab  = f->fv.inslog_tab;
   /* A COPY, into frame-owned storage: the tail is reloaded whenever a weight
    * is logged or edited, and a frame that borrowed it would be drawing an
    * array rewritten underneath it. */
   m->wt.nwt       = wt_copy(f->wtlog, NWT);
   m->wt.wt        = f->wtlog;
   m->wt.wt_page   = f->fv.wtlog_page;
   m->wt.wt_t      = f->fv.wt_t;
   m->wt.wt_tenths = f->fv.wt_tenths;
   m->prefs.wunits = f->prefs.wunits;
   m->wt.wt_edit   = (f->fv.wt_edit >= 0);
   m->wt.wt_tab    = f->fv.wt_tab;
   m->wt.wt_orig_t = f->fv.wt_orig.t;
   m->wt.wt_orig_g = f->fv.wt_orig.g;
   /* A COPY, into frame-owned storage, for the reason the two above are:
    * adding a food from the picker grows this table, and that happens on the
    * same tap that leaves the picker -- so a frame borrowing it would draw an
    * array being rewritten underneath it. */
   m->food.ntypes         = food_type_copy(f->ftypes, NFOODTYPE);
   m->food.types          = f->ftypes;
   m->food.type_page      = f->fv.foodtype_page;
   m->food.food_t         = f->fv.food_t;
   m->food.food_type      = f->fv.food_type;
   m->food.food_g         = f->fv.food_g;
   m->food.food_edit      = (f->fv.food_edit >= 0);
   m->food.food_orig_t    = f->fv.food_orig.t;
   m->food.food_orig_g    = f->fv.food_orig.g;
   m->food.food_orig_type = f->fv.food_orig.type;
   /* ONE call for both, so the number and the bar beside it describe the same
    * instant -- see exercise_button_get. */
   exercise_button_get(mono_s(), &m->food.ex_level, &m->food.ex_remaining);
   /* A COPY, into frame-owned storage, for the reason the insulin and weight
    * tails are copied: the tail is reloaded whenever an entry is logged, and
    * a frame borrowing it would draw an array being rewritten underneath. */
   m->food.nlog     = food_copy(f->foodlog, NFOOD);
   m->food.log      = f->foodlog;
   m->food.log_page = f->fv.foodlog_page;
   /* The exercise tail, copied for the same reason: exercise_button_tick can
    * commit a record from the SERVICE thread between frames, which reloads
    * it. */
   m->food.nexlog        = ex_copy(f->exlog, NEX);
   m->food.exlog         = f->exlog;
   m->food.exlog_page    = f->fv.exlog_page;
   m->food.exlog_tab     = f->fv.exlog_tab;
   /* THE RUNNING ROW, named by its position in the copy above. ex_copy keeps
    * the tail's order (oldest first), so the running session -- which is
    * always the newest row -- is the last one copied; the instant is compared
    * so a tail that was truncated or has moved cannot mislabel a neighbour. */
   m->food.exlog_act = -1;
   {
      struct ex_rec act;
      if (exercise_active(&act) && m->food.nexlog > 0 &&
          m->food.exlog[m->food.nexlog - 1].t == act.t)
         m->food.exlog_act = m->food.nexlog - 1;
   }
   m->food.ex_t          = f->fv.ex_t;
   m->food.ex_form_level = f->fv.ex_level;
   m->food.ex_form_dur   = f->fv.ex_dur;
   m->food.ex_edit       = (f->fv.ex_edit >= 0);
   m->food.ex_running    = f->fv.ex_running;
   str_snapshot(m->food.ex_err, sizeof m->food.ex_err, f->fv.ex_err);
   m->food.ex_orig_t     = f->fv.ex_orig.t;
   m->food.ex_orig_level = f->fv.ex_orig.level;
   m->ins.ins_edit = (f->fv.ins_edit >= 0);
   for (int k = 0; k < 2; k++) {
      m->ins.ins_marker[k] = f->prefs.ins_marker[k];
      m->ins.ins_color[k]  = f->prefs.ins_color[k];
      m->ins.ins_size[k]   = f->prefs.ins_size[k];
   }
   m->ins.markpick_ins  = f->fv.markpick_ins;
   m->prefs.statbar_val = f->prefs.statbar_val;
   m->prefs.lockscr_val = f->prefs.lockscr_val;
   m->sys.exp_range     = f->mv.exp_range;
   m->sys.exp_glu       = f->mv.exp_glu;
   m->sys.exp_dev       = f->mv.exp_dev;
   m->sys.exp_ins       = f->mv.exp_ins;
   m->sys.exp_wt        = f->mv.exp_wt;
   m->sys.exp_failed    = f->mv.exp_failed;
   m->dev.pend_type     = pairing_pending();
   m->dev.old_page      = f->mv.old_page;
   m->dev.dev_page      = f->mv.dev_page;
   for (int i = 0; i < SC_MAX; i++)
      m->prefs.shortcut[i] = f->prefs.shortcut[i];

   /* Must hold the LONGEST entry any keypad accepts, not just a PIN. The
    * rename keypad caps at min(label-1, entry-1) = 11 characters, so an
    * 8-byte buffer echoed only the first 7: the field froze while typing
    * continued, DEL looked dead for four presses, and OK then saved a name the
    * user had never seen. Sized from the snapshot's own buffer so it cannot
    * drift again. */
   int el = f->fv.entrylen < (int)sizeof f->entry - 1
                ? f->fv.entrylen
                : (int)sizeof f->entry - 1;
   for (int i = 0; i < el; i++)
      f->entry[i] = f->fv.entry[i];
   f->entry[el]   = 0;
   m->entry.entry = f->entry;
}

/* THE FIVE WINDOWS under the plot. It takes the frame like every other
 * builder even though it reads nothing out of it: stat_window answers from
 * the statistics module's own buckets, and a signature that says so by being
 * different is a signature somebody has to think about. */
static void build_stats(struct frame_ctx *f, struct screen *m)
{
   (void)f;
   static const int win[5] = {1, 3, 7, 30, 90};
   for (int i = 0; i < 5; i++) {
      int tir = 0;
      int avg = 0;
      if (stat_window(win[i], &tir, &avg)) {
         m->plot.stat[i].have = 1;
         m->plot.stat[i].tir  = tir;
         m->plot.stat[i].avg  = avg;
      }
   }
}

/* WHAT THE SHELL ITSELF HAS TO SAY: the append counter, the status line and
 * the pairing candidate list */
static void build_status(struct frame_ctx *f, struct screen *m)
{
   m->dev.stored = store_appended();
   /* A COPY, UNDER THE LOCK. g_status is rewritten by set_status on a binder
    * thread, and a pointer kept across a render is a string that can change
    * while it is being drawn -- but the copy itself was the race: a bounded
    * byte loop against a concurrent snprintf reads whatever mixture the two
    * happen to produce. The lock is a leaf and nothing is called under it. */
   status_copy(m->status, (int)sizeof m->status);
   m->dev.adv_total = pairing_adverts_seen();

   /* A COPY: the binder thread keeps rewriting the candidate list under the
    * scan, so the renderer must never walk it directly. */
   struct pair_cand cand[UI_DEVS_MAX];
   int nd = pairing_candidates(cand, UI_DEVS_MAX);
   for (int i = 0; i < nd; i++) {
      str_snapshot(f->devs[i].name, sizeof f->devs[i].name, cand[i].name);
      str_snapshot(f->devs[i].mac, sizeof f->devs[i].mac, cand[i].mac);
      f->devs[i].rssi = cand[i].rssi;
   }
   m->dev.devs = f->devs;
   m->dev.ndev = nd;
}

void build_model(struct screen *m)
{
   /* ONE READING of the preferences and the identity for the WHOLE frame.
    *
    * The settings and the identity are read as COPIES, and
    * several fields below are handed on to the renderer as borrowed strings
    * (model, firmware, the server name, the account address) that outlive
    * the statement that took them. Those strings are written from a BINDER
    * thread -- the sensor's device-information reply -- and from the sync
    * worker, so a frame could draw one sensor's model beside another's
    * firmware, or a paired flag beside the previous account's address.
    *
    * The copies are file-static rather than automatic because the pointers
    * below have to stay valid until the frame is drawn. Only the main thread
    * builds a model, so one copy is enough. */
   struct frame_ctx *f = &g_frame;
   settings_get(&f->prefs);
   remote_creds_get(&f->creds);
   f->now   = realtime_s();
   long now = f->now;
   /* ONE copy of the menus' state for the whole frame (see menuview.h).
    * Reading the selected slot eight separate times while a frame is built
    * lets a tap arriving between two of them give one frame two different
    * answers about which sensor it is showing. */
   menu_view_get(&f->mv);
   /* ...and one of the FORMS, for the same reason (see forms.h): fifteen
    * separate reads as the frame was built could give one frame two different
    * answers about what the user was typing. */
   forms_view_get(&f->fv);

   *m        = (struct screen){0};
   m->scr       = shell_gate() ? SCR_GATE : cur_screen();
   m->log_scrub = f->fv.log_scrub;
   m->now    = now;
   m->tz_off = tz_off_now();

   build_reading(f, m);
   build_plot(f, m);
   build_prefs(f, m);
   build_sync(f, m);
   build_devices(f, m);
   build_forms(f, m);
   build_stats(f, m);
   build_status(f, m);
}

/* Rebuild the text lines; redraw only if something visible changed, and at
 * most ~5 times/second so radio chatter can't saturate the main thread. */
void update_screen(void)
{
   /* Off the main thread (a BLE-thread status/advert update), don't rebuild the
    * shared text lines or draw -- just mark dirty; on_timer rebuilds+paints. */
   if (!shell_on_main()) {
      shell_ui_dirty();
      return;
   }
   char next[MAX_LINES][MAX_COLS + 1];
   int n = 0;

   /* g_status is written by set_status on a BLE binder thread; take it under
    * the lock, for the reason build_model's copy does. */
   char st[MAX_COLS + 1];
   status_copy(st, (int)sizeof st);
   /* The status gets the room the label leaves, said explicitly: this line is
    * a fixed-width cell, so truncation is intended -- but "%s" into a buffer
    * the label has already eaten 8 columns of is truncation by accident, and
    * the host harness (-O2, which is what makes gcc emit
    * -Wformat-truncation) says so. */
   (void)snprintf(next[n++], sizeof next[0], "PANCRA  %.*s", MAX_COLS - 8, st);
   /* total rounded to 10s so ambient chatter doesn't redraw every advert.
    * g_ndevs is written by the binder-thread advert handler under devlist_lock,
    * so snapshot it under the same lock -- honouring the invariant every other
    * g_ndevs reader holds. (g_total is a benign rounded pipe-health counter
    * whose writer takes no lock; reading it unlocked is fine.) */
   (void)snprintf(next[n++], sizeof next[0], "ADV %u  DX %d",
                  pairing_adverts_seen() / 10 * 10, pairing_candidate_count());

   int changed = (n != g_nlines);
   for (int i = 0; !changed && i < n; i++)
      changed = strcmp(next[i], g_lines[i]) != 0;
   if (!changed)
      return;

   g_nlines = n;
   /* memcpy, not snprintf("%s"): the two arrays are the SAME fixed row size,
    * so this is a whole-row copy with no truncation question to answer. The
    * formatted version made the compiler reason about how far a NUL might be
    * from the start of a row, and under some inlining it could not prove the
    * row was terminated within its own bounds -- a -Wformat-truncation error
    * that appears or not depending on optimisation, which is a poor reason
    * for a build to fail and a poor reason to trust one that does not. */
   for (int i = 0; i < n; i++)
      memcpy(g_lines[i], next[i], sizeof g_lines[0]);

   static long long last_draw_ms;
   long long now = now_ms();
   if (now - last_draw_ms < 200)
      return; /* next change will repaint */
   last_draw_ms = now;
   shell_repaint();
}

/* --- scan lifecycle (all on main thread) --- */

void set_status(const char *s)
{
   mutex_lock(&g_status_lk);
   (void)snprintf(g_status, sizeof g_status, "%s", s);
   /* PUBLISHED TO THE HANDLER AFTER IT IS WHOLE. The string above is complete
    * before the first byte of this copy is written, so a signal landing
    * mid-copy leaves the handler reading a prefix of the NEW message followed
    * by the tail of the previous one -- which is why the NUL is written first
    * and
    * the bytes after it. A handler that arrives between the two sees the
    * short-but-valid string, never an unterminated one. */
   atomic_store_explicit(&g_status_crash[0], 0, memory_order_relaxed);
   for (size_t i = 0; i + 1 < sizeof g_status_crash && g_status[i]; i++) {
      /* THE TERMINATOR FIRST, THEN THE BYTE. Every instant between these two
       * stores is a complete string: the handler either stops before this
       * character or reads it followed by the NUL. */
      atomic_store_explicit(&g_status_crash[i + 1], 0, memory_order_relaxed);
      atomic_store_explicit(&g_status_crash[i], g_status[i],
                            memory_order_relaxed);
   }
   mutex_unlock(&g_status_lk);
   update_screen();
}

/* BOTH SNAPSHOTS, in the order the lock discipline requires: driver first,
 * then registry, and both BEFORE the history lock the frame is built under.
 * Taking either one inside that lock would invert the documented
 * driver -> registry -> history order. */
void model_snapshot(void)
{
   snap_drivers(&g_frame);
   snap_registry(&g_frame);
}

int model_frame(struct screen *m)
{
   if (!m)
      return 0;
   /* SNAPSHOT BEFORE THE HISTORY LOCK, never inside it: snap_drivers takes
    * the driver lock, and the rank is driver -> registry -> history (see
    * thread.h). Taking them the other way round is the deadlock the two
    * freezes of 2026-08-15 were.
    *
    * TRYLOCK, ALWAYS. A frame is disposable to both callers: a repaint is
    * redrawn by the 1 Hz timer, and a scrub is redriven by the next MOVE
    * event a finger produces (dozens a second). Waiting would put the main
    * thread behind a binder thread with a reading to deliver, for a frame
    * that is about to be built again anyway.
    *
    * ONE EXIT, and not as a style preference: app/test/lockorder.py refuses a
    * return taken while a lock is held, because a lock let go of by
    * returning is never released. */
   model_snapshot();
   int got = store_trylock();
   if (got) {
      build_model(m);
      store_unlock();
   }
   return got;
}

/* Drop the cached text lines so the next update_screen rebuilds them all: a
 * new surface has nothing on it, and the throttle compares against what was
 * last DRAWN. */
void model_lines_reset(void)
{
   g_nlines = 0;
}

/* The status line, for the crash handler -- which reads its context through
 * pointers and cannot call anything to derive a value (see crashlog.h).
 *
 * THE SIGNAL-SAFE COPY, NOT THE LIVE BUFFER. Handing back g_status itself
 * leaves the handler reading, byte by byte and with no lock, the same array a
 * binder thread might be inside snprintf on -- so the one string that has to
 * survive a crash is the one most likely to be torn when it matters. It is
 * also the wrong thing to lock: the thread the signal interrupted may hold
 * g_status_lk already, and a handler that blocks on it produces no report at
 * all.
 *
 * What the handler gets instead is a copy that is only ever written whole,
 * NUL first (see set_status), so every instant is a valid C string, and whose
 * bytes are ATOMIC so that reading them from a handler is defined rather than
 * merely likely to work. It can be one update stale. That is the trade, and
 * it is the right way round. */
const _Atomic char *model_status_buf(void)
{
   return g_status_crash;
}
