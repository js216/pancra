// SPDX-License-Identifier: GPL-3.0
// model.c --- Live state becomes one immutable frame (see model.h)
// Copyright 2026 Jakob Kastelic

#include "model.h"
#include "alarm.h"
#include "alarmlogic.h"
#include "bletrans.h"
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "exercise.h"
#include "forms.h"
#include "insulin.h"
#include "menuview.h" /* the read-only menu snapshot the frame copies */
#include "meter.h"
#include "nav.h"
#include "pairing.h"
#include "plotdata.h"
#include "reading.h" /* the per-link RSSI the frame shows */
#include "reconcile.h"
#include "remote.h" /* the last sync outcome: the syncing module owns it */
#include "sensors.h"
#include "sesscache.h" /* the session clock a restart shows before the sensor answers */
#include "settings.h"
#include "shell.h"
#include "stats.h"
#include "store.h"
#include "style.h"
#include "sync.h"
#include "syncstat.h"
#include "tzoff.h"
#include "uifmt.h"
#include "uimodel.h"
#include "util.h"
#include "weight.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void snap_registry(void);
static void snap_drivers(void);

#define MAX_LINES 16 /* text lines on the pre-reading status screen */
#define MAX_COLS  33 /* character columns the UI lays out to */

static char g_status[MAX_COLS + 1] = "STARTING";

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
 * The renderer used to read the slot array directly and hold a POINTER into
 * the provenance cache across a run of field copies, with only the draw flag
 * held. (Both are behind copy queries now -- see sensors.h -- so the shape is
 * no longer expressible; this snapshot remains because the frame needs one
 * consistent instant, not merely one safe read at a time.) Both are mutated
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

static struct snap_slot g_snap_slot[MAX_SLOTS];
static int g_snap_nslot;

static struct dex_session g_snap_sess[LINK_MAX];

/* Driver state for the frame, captured before the draw flag is taken.
 *
 * build_model() used to call driver_lock() while draw() held g_draw_busy, and
 * the BLE side takes them the other way round (driver_lock -> hist_lock inside
 * driver_on_notify -> drv_glucose). Two spin locks acquired in opposite orders
 * is an unrecoverable hang, and it needed only a reading landing during a 1 Hz
 * repaint -- i.e. steady-state operation. Snapshotting here means the main
 * thread never holds one lock while waiting for the other. */
static struct dex_cal g_snap_cal;

static void snap_registry(void)
{
   /* ONE registry snapshot, taken by the registry itself. Assembled here out
    * of per-row calls it would be a walk whose rows can come from different
    * instants: a binder thread minting a sensor moves them underneath the
    * loop -- which is the whole reason the frame snapshots the registry at
    * all. */
   struct sensor_view v;
   sensors_view_get(&v);
   g_snap_nslot = v.n < MAX_SLOTS ? v.n : MAX_SLOTS;
   for (int i = 0; i < g_snap_nslot; i++) {
      const struct sensor_slot *sl = &v.slot[i];
      const struct sensor_rec *r   = v.have_rec[i] ? &v.rec[i] : 0;
      struct snap_slot *d          = &g_snap_slot[i];
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
   if (row < 0 || row >= g_snap_nslot)
      return -1;
   return g_snap_slot[row].id;
}

/* Draw-path variant: resolves from the per-frame snapshot instead of the live
 * driver, because the renderer must never take driver_lock while holding the
 * draw flag -- that inversion is what caused an unrecoverable hang. */
static int snap_shell_link_for_slot(int idx)
{
   if (idx < 0 || idx >= g_snap_nslot)
      return -1;
   const struct snap_slot *d = &g_snap_slot[idx];
   if (!d->have_rec)
      return -1;
   /* By ADDRESS for meters too: they hold their own links now, so there is
    * no single link to return for "the meter". */
   for (int l = 0; l < LINK_MAX; l++)
      if (g_snap_sess[l].mac[0] && strcmp(g_snap_sess[l].mac, d->mac) == 0)
         return l;
   return -1;
}

static void snap_drivers(void)
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
   driver_snapshot(g_snap_sess, cal_link, &g_snap_cal);
}

/* How many CGMs are registered. Above one, a reading whose link resolves to no
 * slot cannot be safely attributed to the global "current source". */
/* Fill one ui_sensor from its slot + provenance + (if it is the live one) the
 * driver session. */
/* THE FRAME'S ONE READING of the preferences and the identity. File-scope
 * because build_model fills the per-slot rows through fill_sensor, which has
 * to describe the SAME instant -- a copy per row is both repeated work and a
 * second instant, and one row could then show a pairing code the row above it
 * does not. Only the main thread builds a model. */
static struct prefs g_frame_prefs;
static struct sync_creds g_frame_creds;

static void fill_sensor(struct ui_sensor *u, int i, long now)
{
   /* From the pre-draw snapshot, never the live registry -- see snap_registry.
    */
   if (i < 0 || i >= g_snap_nslot)
      return;
   const struct snap_slot *sl = &g_snap_slot[i];
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
   /* This DEVICE's wear budget: override / model / type default. */
   u->wear_len = sensor_wear_seconds(u->type, sl->wear_days, sl->model);
   /* Mirrors sensor_wear_seconds' own test for "the user pinned this", and
    * must stay in step with it: anything that is not a valid pin resolves. */
   u->wear_auto  = (sl->wear_days != 10 && sl->wear_days != 15);
   u->paired     = sl->paired;
   u->activation = sl->activation;
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
      int sl_link = snap_shell_link_for_slot(i);
      struct dex_session s =
          (sl_link >= 0) ? g_snap_sess[sl_link] : (struct dex_session){0};
      /* LIVE WINS; the cache only fills the gap before this link's first 0x4e
       * of the process. Both directions in one place, so the per-device screen
       * and the top block cannot disagree about a sensor's session. */
      if (s.have_reading)
         sessc_put(sl->id, &s, now);
      else
         (void)sessc_restore(sl->id, now, &s);
      u->connected       = s.bonded && (now - u->last) < AL_FRESH_S;
      u->session_seconds = (long)s.session_seconds;
      u->sess_state      = s.state;
      u->predicted       = s.predicted;
      u->sequence        = s.sequence;
      /* THIS link's signal, not the last one measured on any link. */
      {
         int lk     = snap_shell_link_for_slot(i);
         int rdbm   = 0;
         long rwhen = 0;
         int haveit = reading_link_rssi(lk, &rdbm, &rwhen);
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
         str_snapshot(u->code, sizeof u->code, g_frame_prefs.code_str);
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
   settings_get(&g_frame_prefs);
   sync_creds_get(&g_frame_creds);
   struct prefs *psp      = &g_frame_prefs;
   struct sync_creds *csp = &g_frame_creds;
#define ps (*psp)
#define cs (*csp)

   /* glucose + doses + weights. All three ride in one array because plot_render
    * and plot_hit take points in any order, and the scrub index the UI hands
    * back has to index a single list. */
/* The candidate list is the pairing workflow's; this is only how many of its
 * rows the device screen can show at once. */
#define UI_DEVS_MAX 12
   static struct ui_point pts[UI_PTS_MAX];
   static struct ui_dev devs[UI_DEVS_MAX];
   long now = realtime_s();
   /* ONE copy of the menus' state for the whole frame (see menuview.h). The
    * selected slot used to be read eight separate times while a frame was
    * being built, so a tap arriving between two of them gave one frame two
    * different answers about which sensor it was showing. */
   struct menu_view mv;
   menu_view_get(&mv);
   /* ...and one of the FORMS, for the same reason (see forms.h): fifteen
    * separate reads as the frame was built could give one frame two different
    * answers about what the user was typing. */
   struct forms_view fv;
   forms_view_get(&fv);

   *m        = (struct screen){0};
   m->scr    = shell_gate() ? SCR_GATE : cur_screen();
   m->now    = now;
   m->tz_off = g_tz_off;

   /* Read directly, and consistently, because draw() holds the history lock
    * across this whole frame (see the note below on g_hist). store_now would
    * take that same non-recursive lock and deadlock; what it offers an
    * UNLOCKED caller -- one coherent triple -- the caller's lock already
    * gives this one. */
   /* THE CURRENT READING AS ONE TRIPLE, through the locked variant: draw()
    * holds the store lock across this whole frame and it is not recursive, so
    * the unlocked store_now would deadlock. Reading the three by hand is what
    * used to pair a new glucose with the previous timestamp. */
   struct reading_now cur   = store_now_locked(now);
   struct reading_rssi crss = store_rssi_locked();
   m->reading.glu           = cur.glu;
   m->reading.trend         = cur.trend;
   m->reading.t             = cur.t;
   m->reading.rssi          = crss.dbm;
   m->reading.rssi_ok       = crss.ok;
   m->reading.stale         = cur.stale;
   m->reading.disc_alarmed  = alarm_disc_latched();

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
         pts[nh++] = plong[i];
   } else {
      nh = hist_count() < NHIST ? hist_count() : NHIST;
      for (int i = 0; i < nh; i++) {
         pts[i].t    = hist_at(i).t;
         pts[i].glu  = hist_at(i).glu;
         pts[i].src  = hist_at(i).src;
         pts[i].kind = hist_at(i).kind;
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
      pts[nh].t         = ir.t;
      pts[nh].glu       = ir.units;
      pts[nh].src       = ir.type; /* the scrub shows "2U FAST" etc. */
      pts[nh].kind      = KIND_INS;
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
      pts[nh].t        = wr.t;
      pts[nh].glu      = (int)wr.g; /* grams; WT_MAX_G is 400000, fits an int */
      pts[nh].src      = 0;
      pts[nh].kind     = KIND_WT;
      nh++;
   }
   m->plot.hist       = pts;
   m->plot.nhist      = nh;
   m->plot.scrub      = fv.scrub;
   m->plot.plot_hours = model_plot_hours();
   m->plot.plot_max   = ps.plot_max;

   /* The PRIMARY CGM drives the top block -- resolved to ITS link, not
    * hardcoded LINK_CGM (link 0), which just belongs to whichever CGM claimed
    * it first. With the Stelo on link 0 and a G7 primary on another link,
    * the STATE/SESSION/PRED rows mixed the G7's 10-day budget with the
    * STELO's session clock: 15 days in vs 10 d + 12 h grace computed as past
    * even the grace period, so the SESSION row said ENDED while the primary
    * was nowhere near its end. A primary with no live session yet (freshly
    * committed pairing) reports an EMPTY session -- SESSION --, consistent
    * with the cleared big number, never another sensor's numbers. */
   struct dex_session s = g_snap_sess[LINK_CGM];
   for (int i = 0; i < g_snap_nslot; i++)
      if (g_snap_slot[i].primary) {
         int pl = snap_shell_link_for_slot(i);
         s      = (pl >= 0) ? g_snap_sess[pl] : (struct dex_session){0};
         /* Same restore as the per-device rows: without it PRED and the
          * session countdown under the big number blanked for a whole
          * five-minute cadence after every launch, while the reading and its
          * age beside them came straight back from readings.csv. */
         if (!s.have_reading)
            (void)sessc_restore(g_snap_slot[i].id, now, &s);
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
   for (int i = 0; i < g_snap_nslot; i++)
      if (g_snap_slot[i].have_rec && !g_snap_slot[i].old &&
          sensor_kind(g_snap_slot[i].type) == KIND_CGM) {
         m->reading.has_cgm = 1;
         break;
      }

   m->prefs.units       = ps.units;
   m->prefs.alarm_low   = ps.alarm_low;
   m->prefs.alarm_high  = ps.alarm_high;
   m->prefs.nudge_low   = ps.nudge_low;
   m->prefs.nudge_high  = ps.nudge_high;
   m->prefs.nudge_sound = ps.nudge_sound;
   m->prefs.nudge_vib   = ps.nudge_vib;

   /* settings + device info (globals persist; s.mac lives on our stack, so it
    * is copied into a static the borrowed pointer can safely outlast) */
   m->prefs.sound_on     = ps.sound_on;
   m->prefs.vib_on       = ps.vib_on;
   m->prefs.orient       = ps.orient;
   m->prefs.screen_on    = ps.screen_on;
   m->prefs.newdata_mode = ps.newdata_mode;
   m->sync.remote_on     = ps.remote_on;
   m->sync.remote_server = ps.remote_server; /* global: the borrow is stable */
   m->sync.sync_email    = cs.email;
   m->sync.sync_paired   = cs.uid > 0;
   m->entry.label_field  = fv.label_field;
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
    * local. The frame used to borrow a pointer to a buffer the sync worker
    * rewrites, and the renderer then compared it against a list of English
    * phrases; now it carries the code, and the label comes from the one place
    * that maps codes to words (syncstat.c).
    *
    * Reading the global twice -- once for the code, once for the label --
    * would be the same defect in a smaller window: two reads of a value
    * another thread is changing, which agree almost always. One read. */
   int outcome            = remote_outcome();
   m->sync.remote_outcome = outcome;
   m->sync.remote_status  = sync_outcome_label(outcome);
   m->sync.remote_port    = ps.remote_port;
   m->sync.remote_last_ok = remote_ok_time();
   m->prefs.disc          = ps.disc;
   m->dev.code            = ps.code_str;
   m->dev.model           = ps.model;
   m->dev.fw              = ps.fw;
   m->dev.mfr             = ps.mfr;
   static char macbuf[20];
   str_snapshot(macbuf, sizeof macbuf, s.mac);
   m->dev.mac = macbuf;
   for (int i = 0; i < NPERMS; i++)
      m->sys.perm[i] = mv.perm[i];
   m->sys.batt_ok        = mv.batt_ok;
   m->sys.standby_bucket = mv.standby_bucket;
   m->sys.bg_restricted  = mv.bg_restricted;

   /* keypad: mode + the digits typed so far (copied so the pointer is stable)
    */
   /* configured sensors, plus which one a detail screen is showing */
   static struct ui_sensor sens[MAX_SLOTS];
   /* Count from the SNAPSHOT, not the live slot_count(). Mixing the two means a
    * concurrent sensor_forget can shrink slot_count() between this loop
    * bound and the snapshot it indexes, so the last row renders whatever the
    * previous frame left in `sens` -- a sensor the user just forgot,
    * reappearing for a frame. */
   for (int i = 0; i < g_snap_nslot; i++)
      fill_sensor(&sens[i], i, now);
   m->dev.sensors  = sens;
   m->dev.nsensors = g_snap_nslot;
   /* WHICH ROW IS SELECTED, resolved from THIS frame's own snapshot: the
    * selection is a device (mv.sel_id), and the row it occupies is a fact
    * about the picture being drawn. Reading a stored index instead would
    * highlight, and then act on, whatever had moved into that position. */
   m->dev.sel = -1;
   for (int i = 0; i < g_snap_nslot && m->dev.sel < 0; i++)
      if (g_snap_slot[i].id == mv.sel_id)
         m->dev.sel = i;

   struct dex_cal c     = g_snap_cal;
   m->cal.cal_have      = c.have;
   m->cal.cal_permitted = c.permitted;
   m->cal.cal_status    = c.status;
   m->cal.cal_last_bg   = c.last_bg;
   m->cal.cal_result    = c.result;
   m->cal.cal_pending   = fv.cal_pending;

   /* RESCALE screens. On the confirmation, preview the CLAMPED factor computed
    * from the entered value over the selected sensor's live raw; on the active
    * screen, show the running factor. */
   m->cal.rescale_entry = fv.rescale_entry;
   if (cur_screen() == SCR_RESCALEACT) {
      m->cal.rescale_pm = calib_rescale_pm();
   } else if (cur_screen() == SCR_RESCALE && fv.rescale_entry > 0 &&
              mv.sel_id > 0) {
      /* Preview: UNCLAMPED (so a >25% value shows its real size, in red, and is
       * rejected on CONFIRM), or the sentinel 0 when there is no reading yet to
       * compute against (the screen then says "ON NEXT READING", not "0%"). */
      m->cal.rescale_pm = calib_rescale_preview(
          fv.rescale_entry, calib_raw_on_link(link_for_sensor(mv.sel_id)));
   } else {
      m->cal.rescale_pm = 1000;
   }

   m->entry.kp_mode = fv.kp_mode;
   (void)snprintf(m->entry.kp_err, sizeof m->entry.kp_err, "%s", fv.kp_err);
   /* Type being added, for the PAIR NEW <type> / SELECT <type> titles. The
    * OneTouch shows its full name; CGMs use their short type name. */
   m->dev.add_type = (mv.add_type == SENSOR_ONETOUCH)
                         ? "ONETOUCH VERIO"
                         : sensor_type_name(mv.add_type);
   m->dev.add_kind = sensor_kind(mv.add_type);
   /* the picked device SCR_PAIRCONF is asking about (main-thread globals,
    * like the pairing code above) */
   m->dev.pair_name = pairing_pend_name();
   m->dev.pair_mac  = pairing_pend_mac();
   /* LOG INSULIN form state */
   m->ins.ins_t     = fv.ins_t;
   m->ins.ins_type  = fv.ins_type;
   m->ins.ins_units = fv.ins_units;
   /* A COPY, into frame-owned storage: the tail is reloaded whenever a dose
    * is logged or edited, and a frame that borrowed it would be drawing an
    * array that had been rewritten underneath it. */
   static struct ins_rec inssnap[NINS];
   m->ins.ins_nlog    = ins_copy(inssnap, NINS);
   m->ins.ins_log     = inssnap;
   m->ins.inslog_page = fv.inslog_page;
   /* A COPY, into frame-owned storage: the tail is reloaded whenever a weight
    * is logged or edited, and a frame that borrowed it would be drawing an
    * array that had been rewritten underneath it. */
   static struct wt_rec wtsnap[NWT];
   m->wt.nwt       = wt_copy(wtsnap, NWT);
   m->wt.wt        = wtsnap;
   m->wt.wt_page   = fv.wtlog_page;
   m->wt.wt_t      = fv.wt_t;
   m->wt.wt_tenths = fv.wt_tenths;
   m->prefs.wunits = ps.wunits;
   m->wt.wt_edit   = (fv.wt_edit >= 0);
   m->wt.wt_tab    = fv.wt_tab;
   m->wt.wt_scrub  = fv.wt_scrub;
   m->wt.wt_orig_t = fv.wt_orig.t;
   m->wt.wt_orig_g = fv.wt_orig.g;
   /* A COPY, into frame-owned storage, for the reason the two above are:
    * adding a food from the picker grows this table, and that happens on the
    * same tap that leaves the picker -- so a frame borrowing it would draw an
    * array being rewritten underneath it. */
   static struct food_type ftsnap[NFOODTYPE];
   m->food.ntypes    = food_type_copy(ftsnap, NFOODTYPE);
   m->food.types     = ftsnap;
   m->food.type_page = fv.foodtype_page;
   m->food.food_t    = fv.food_t;
   m->food.food_type = fv.food_type;
   m->food.food_g    = fv.food_g;
   m->food.food_edit = (fv.food_edit >= 0);
   /* ONE call for both, so the number and the bar beside it describe the same
    * instant -- see exercise_button_get. */
   exercise_button_get(mono_s(), &m->food.ex_level, &m->food.ex_remaining);
   /* A COPY, into frame-owned storage, for the reason the insulin and weight
    * tails are copied: the tail is reloaded whenever an entry is logged, and
    * a frame borrowing it would draw an array being rewritten underneath. */
   static struct food_rec fdsnap[NFOOD];
   m->food.nlog     = food_copy(fdsnap, NFOOD);
   m->food.log      = fdsnap;
   m->food.log_page = fv.foodlog_page;
   m->ins.ins_edit = (fv.ins_edit >= 0);
   for (int k = 0; k < 2; k++) {
      m->ins.ins_marker[k] = ps.ins_marker[k];
      m->ins.ins_color[k]  = ps.ins_color[k];
      m->ins.ins_size[k]   = ps.ins_size[k];
   }
   m->ins.markpick_ins  = fv.markpick_ins;
   m->prefs.statbar_val = ps.statbar_val;
   m->prefs.lockscr_val = ps.lockscr_val;
   m->sys.exp_range     = mv.exp_range;
   m->sys.exp_glu       = mv.exp_glu;
   m->sys.exp_dev       = mv.exp_dev;
   m->sys.exp_ins       = mv.exp_ins;
   m->sys.exp_wt        = mv.exp_wt;
   m->dev.pend_type     = pairing_pending();
   m->dev.old_page      = mv.old_page;
   m->dev.dev_page      = mv.dev_page;
   for (int i = 0; i < SC_MAX; i++)
      m->prefs.shortcut[i] = ps.shortcut[i];

   /* Must hold the LONGEST entry any keypad accepts, not just a PIN. The
    * rename keypad caps at min(label-1, entry-1) = 11 characters, so an
    * 8-byte buffer echoed only the first 7: the field froze while typing
    * continued, DEL looked dead for four presses, and OK then saved a name the
    * user had never seen. Sized from the snapshot's own buffer so it cannot
    * drift again. */
   static char entrybuf[sizeof fv.entry];
   int el = fv.entrylen < (int)sizeof entrybuf - 1 ? fv.entrylen
                                                   : (int)sizeof entrybuf - 1;
   for (int i = 0; i < el; i++)
      entrybuf[i] = fv.entry[i];
   entrybuf[el]   = 0;
   m->entry.entry = entrybuf;

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

   m->dev.stored    = store_appended();
   m->status        = g_status;
   m->dev.adv_total = pairing_adverts_seen();

   /* A COPY: the binder thread keeps rewriting the candidate list under the
    * scan, so the renderer must never walk it directly. */
   struct pair_cand cand[UI_DEVS_MAX];
   int nd = pairing_candidates(cand, UI_DEVS_MAX);
   for (int i = 0; i < nd; i++) {
      str_snapshot(devs[i].name, sizeof devs[i].name, cand[i].name);
      str_snapshot(devs[i].mac, sizeof devs[i].mac, cand[i].mac);
      devs[i].rssi = cand[i].rssi;
   }
   m->dev.devs = devs;
   m->dev.ndev = nd;
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

   /* g_status is written by set_status on a BLE binder thread; snapshot it with
    * a bound so this read can never scan off the end during a racing write. */
   char st[MAX_COLS + 1];
   str_snapshot(st, sizeof st, g_status);
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
   (void)snprintf(g_status, sizeof g_status, "%s", s);
   update_screen();
}

/* BOTH SNAPSHOTS, in the order the lock discipline requires: driver first,
 * then registry, and both BEFORE the history lock the frame is built under.
 * Taking either one inside that lock would invert the documented
 * driver -> registry -> history order. */
void model_snapshot(void)
{
   snap_drivers();
   snap_registry();
}

/* Drop the cached text lines so the next update_screen rebuilds them all: a
 * new surface has nothing on it, and the throttle compares against what was
 * last DRAWN. */
void model_lines_reset(void)
{
   g_nlines = 0;
}

/* The status line, for the crash handler -- which reads its context through
 * pointers and cannot call anything to derive a value (see crashlog.h). */
const char *model_status_buf(void)
{
   return g_status;
}
