// SPDX-License-Identifier: GPL-3.0
// reconcile.c --- Sessions and the registry, kept in agreement (see
// reconcile.h) Copyright 2026 Jakob Kastelic

#include "reconcile.h"
#include "clock.h"
#include "dexdriver.h"
#include "log.h"
#include "meter.h"
#include "reading.h"
#include "selection.h"
#include "senslogic.h"
#include "sensors.h"
#include "sesscache.h" /* sess_flush: the tick persists the session clock */
#include "thread.h"
#include "util.h"
#include <string.h>

/* Runs from the activity's 1 Hz timer AND, once that timer is gone, from the
 * service tick. Serialised by a try-lock because those are different threads:
 * it mints ids and appends to the provenance file, and two concurrent passes
 * could mint twice for one sensor. Skipping a tick is free. */
static struct flight g_reconcile_flight = FLIGHT_INIT;

/* (The link searches themselves are the DRIVER's: driver_link_of_identity
 * and driver_free_cgm_link. They are questions about links and sessions,
 * which is the driver's state -- keeping them here made the meter runtime
 * include this file to ask one, and this file is a workflow that calls back
 * into the meter.) */

/* Map a sensor slot to its transport link, BY ADDRESS.
 *
 * This used to derive the link from a sensor's ORDINAL among the CGM slots,
 * which is not stable: sensor_forget() shifts g_slot while the remaining
 * sensors keep their live GATT connections, driver contexts and per-link key
 * files. Forgetting the first of two CGMs therefore re-pointed the survivor at
 * an emptied context, after which commit_pair() would call driver_forget() on
 * the link the survivor was ACTUALLY using and destroy its bond; calibration
 * went to a dead context while still logging "submitted"; and the survivor's
 * adverts stopped resolving to a live link. Resolving by the session address
 * -- the one identity a shift cannot move -- removes the whole class.
 *
 * idx == slot_count() is a legitimate query ("the link a NEW sensor would
 * take"), which is what free_cgm_link answers. */
/* THE LINK FOR ONE ROW OF A SNAPSHOT ALREADY TAKEN. The index is only ever
 * an index INTO `v`, so it cannot go stale: it is read from the same instant
 * as the walk that ranks it. Callers outside this file name a DEVICE
 * (link_for_sensor) or ask for a new one (link_for_new_sensor). */
static int link_in_view(const struct sensor_view *vp, int idx)
{
   const struct sensor_view v = *vp;
   /* The identity is copied out rather than reached for through a pointer:
    * srec_push() shifts the records to keep them id-ordered, from a binder
    * thread via ot_drv_done, so a retained pointer can be overwritten mid-use.
    * This runs on a binder thread itself (jni_on_advert) while the main thread
    * may be inside sensor_forget's shift-down, and its result is fed to
    * dexble_pair -- a torn read here connects one sensor's address on
    * another's link, using the wrong key file. */
   char ident[24];
   ident[0] = 0;
   int have = 0;
   if (idx >= 0 && idx < v.n && v.have_rec[idx]) {
      str_snapshot(ident, sizeof ident, v.rec[idx].identity);
      have = 1;
   }
   /* THE DRIVER'S STATE AS ONE INSTANT, taken through its own operation
    * rather than by holding its lock from here: every link lookup below reads
    * this copy, so they cannot disagree with each other, and this file no
    * longer reasons about somebody else's mutex. */
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   int link = have ? driver_link_of_identity_in(sess, ident) : -1;
   if (link < 0) {
      /* Not yet bound (no session on any link -- the normal state right after
       * a restart). Rank this slot among the OTHER unbound slots so each one
       * claims a different free link.
       *
       * Every registered device is ranked, meters included: a meter needs a
       * link of its own to hold a standing connect, because it is reachable
       * only for the second or two it is switched on. Ranking by slot order
       * means a device that IS bound never reaches here, so a forget cannot
       * renumber a live one. */
      int rank = 0;
      for (int i = 0; i < idx && i < v.n; i++) {
         if (v.slot[i].old)
            continue; /* retired: holds no link */
         if (!v.have_rec[i])
            continue;
         if (driver_link_of_identity_in(sess, v.rec[i].identity) < 0)
            rank++;
      }
      link = driver_free_cgm_link_in(sess, rank);
   }
   return link;
}

/* THE LINK THIS DEVICE IS ON, or would take. BY ID: everything outside this
 * file names the sensor, and the index it lives at is resolved here, from the
 * same snapshot the ranking walk reads. An index that crossed a function
 * boundary was the whole defect -- a retire could close the radio link of a
 * device that had merely slid into the retired one's position. */
int link_for_sensor(int id)
{
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n; i++)
      if (v.slot[i].id == id)
         return link_in_view(&v, i);
   return -1;
}

/* THE LINK A NEW SENSOR WOULD TAKE: the same answer for an index one past the
 * end (see link_in_view), with the count taken from the same snapshot. Named,
 * so no caller has to know that "one past the end" is the way to ask. */
int link_for_new_sensor(void)
{
   struct sensor_view v;
   sensors_view_get(&v);
   return link_in_view(&v, v.n);
}

void sensor_reconcile(void)
{
   if (!flight_enter(&g_reconcile_flight))
      return;
   meter_sync_watchdog();
   /* Flush the session cache at most once a minute. sessc_put marks it dirty
    * from the draw path, which runs far more often than the 5-minute cadence
    * that actually changes anything; rate-limiting here keeps a redraw storm
    * from becoming a write storm on a file whose only job is to survive the
    * next launch. Losing up to a minute of it costs nothing -- the clock is
    * projected forward from whatever instant was stored. */
   sess_flush(realtime_s());

   /* Walk every CGM link so a newly bonded second sensor is registered too,
    * not just whichever one happened to connect first. */
   struct sens_slot_obs sobs[MAX_SLOTS];
   int nsobs = 0;
   /* THE DRIVER AS ONE INSTANT, then the registry under its own lock. Held by
    * hand, the two used to be one critical section here; a snapshot gives the
    * same thing the walk actually needed -- every link's session as it was at
    * one moment -- without this file taking the driver's lock. */
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   /* ot_drv_done mutates the registry from a binder thread (sensor_mint ->
    * srec_push), so this walk reads ONE snapshot -- it is the walk that
    * decides g_cur_src, the fallback provenance stamped into the permanent
    * log. Held as a lock instead, it also spanned link_for_slot, which takes
    * the DRIVER's lock: registry -> driver, the inverse of the documented
    * order. A snapshot cannot invert anything, because it is over. */
   struct sensor_view rv;
   sensors_view_get(&rv);
   for (int i = 0; i < rv.n; i++) {
      if (rv.slot[i].old) /* disconnected: no live session to reconcile */
         continue;
      if (!rv.have_rec[i] || sensor_kind(rv.rec[i].type) != KIND_CGM)
         continue;
      int l = link_in_view(&rv, i);
      if (l < 0)
         continue;
      struct dex_session ls = sess[l];
      /* OBSERVE here, choose in sens_primary_pick -- including the "prefer the
       * primary and stop at it" rule, which is pinned by senstest. */
      sobs[nsobs].id     = rv.slot[i].id;
      sobs[nsobs].old    = 0;
      sobs[nsobs].is_cgm = 1;
      sobs[nsobs].live =
          ls.bonded && ls.mac[0] && !strcmp(ls.mac, rv.rec[i].identity);
      sobs[nsobs].primary = rv.slot[i].primary;
      nsobs++;
   }
   /* USE the result. This loop previously assigned a local that was never
    * read, so the whole "prefer the primary" fix above was a dead store and
    * g_cur_src kept whatever the registration block below left it -- which,
    * once any CGM had a slot, was never anything at all (see there). */
   int primary_src = sens_primary_pick(sobs, nsobs);
   if (primary_src > 0)
      reading_set_src(primary_src);

   /* Only a CGM is registered from a dex_session. Without this guard, adding a
    * meter would leave sel_add_type() on ONETOUCH and the next CGM to bond
    * would be minted with the wrong type -- and a wrong type is permanent,
    * because the provenance row is never rewritten. */
   int cgm_type = (sensor_kind(sel_add_type()) == KIND_CGM) ? sel_add_type()
                                                            : SENSOR_STELO;
   /* The link a new pairing would use. Note this must not be left selected on
    * return: the caller's stall watchdog and build_model() both read the
    * driver afterwards, and an unused link reports an empty session. */
   /* Recover the meter's id FIRST and unconditionally. It used to sit after
    * the CGM early-return below, so a meter-only user never recovered it after
    * a restart and their meter could never auto-sync again. */
   /* Only seed this when it is not already pointing at a registered meter.
    *
    * The advert handler now selects the meter per advert (any registered one,
    * not just the first), so re-latching the first slot on every 1 Hz tick
    * would clobber that selection -- including mid-sync, which would attribute
    * one meter's fingersticks to another. This is a fallback for the case
    * where nothing has selected a meter yet, e.g. right after a restart. */
   int have_meter = 0;
   if (meter_src() > 0) {
      struct sensor_rec cur;
      have_meter =
          sensor_rec_of(meter_src(), &cur) && cur.type == SENSOR_ONETOUCH;
   }
   struct sensor_view mv;
   sensors_view_get(&mv);
   for (int i = 0; i < mv.n && !have_meter; i++) {
      if (mv.have_rec[i] && mv.rec[i].type == SENSOR_ONETOUCH) {
         /* The ADDRESS goes with the id. Without it the "is this our meter"
          * guard was empty after a restart and accepted ANY OneTouch in
          * range -- importing a stranger's readings under our sensor id. */
         meter_bind(mv.slot[i].id, mv.rec[i].identity);
         break;
      }
   }

   /* Find a CGM link carrying a live bonded session that NO slot claims yet --
    * that is the sensor which still needs registering.
    *
    * This used to probe link_for_slot(slot_count()), i.e. "the link a new
    * pairing would use". Once link resolution became address-based that became
    * a guaranteed dead end: link_for_slot for an unregistered index returns a
    * FREE link, and a free link is by definition one with no session, so
    * s.mac[0] was always 0 and this entire block stopped executing. Nothing
    * was ever minted or slotted, so every reading fell back to source id 0
    * ("pre-registry legacy") in a log that is never rewritten, and the
    * advert-driven reconnect loop -- which iterates slots -- had an empty body.
    * Scanning for the unclaimed session asks the question directly. */
   struct dex_session s;
   memset(&s, 0, sizeof s);
   int s_link = -1;
   /* One instant of the driver, then the registry under its own lock. */
   struct dex_session usess[LINK_MAX];
   driver_snapshot(usess, -1, 0);
   for (int l = 0; l < LINK_MAX && !s.mac[0]; l++) {
      if (meter_link_is(l))
         continue; /* CGMs only */
      struct dex_session ls = usess[l];
      /* have_reading, not just bonded. g_bonded is set at AuthStatus, several
       * round trips BEFORE the first glucose, so session_seconds is still 0
       * then -- and activation is derived from it, so minting that early wrote
       * "session started now" for a sensor that may have been worn for days.
       * activation is not part of the id-reuse key, so it is never corrected.
       */
      if (ls.mac[0] && ls.bonded && ls.have_reading &&
          sensor_slot_by_mac(ls.mac) < 0) {
         s      = ls;
         s_link = l;
      }
   }
   if (s.mac[0] && s.bonded && s_link >= 0) {
      /* Match on ADDRESS FIRST, whatever type is currently selected in the
       * ADD SENSOR menu. sel_add_type() persists after the menu closes, so
       * keying only on (type, mac) let merely *browsing* the type picker
       * re-mint an already-registered sensor under the wrong type --
       * unrecoverable, since provenance rows are never rewritten. */
      int idx               = sensor_slot_by_mac(s.mac);
      struct sens_obs so    = {0};
      struct sens_effect se = {0};
      so.is_cgm             = 1;
      so.has_mac            = 1;
      so.bonded             = s.bonded;
      so.have_reading       = s.have_reading;
      so.claimed            = (idx >= 0);
      so.session_seconds    = (long)s.session_seconds;
      sens_link_eval(&so, realtime_s(), &se);
      if (se.mint) {
         /* se.activation is an EPOCH, not the elapsed length -- see
          * senslogic.c. Feeding the elapsed value straight in wrote a
          * duration into a field documented as a timestamp, in a file that is
          * never rewritten. */
         long activation = se.activation;
         /* This LINK's own DIS strings, never the process-global ones -- those
          * are shared across links and persist to disk, so a second sensor
          * would inherit the first's model and firmware permanently.
          *
          * Copied under the registry lock, for the reason spelled out in the
          * second pass below: pancra_devinfo fills these byte-by-byte from a
          * binder thread, and a torn read here is minted into an append-only
          * row that is never rewritten. */
         char amodel[24] = {0};
         char afw[24]    = {0};
         reading_dis(s_link, amodel, sizeof amodel, afw, sizeof afw);
         int id = sensor_mint(cgm_type, s.mac, "", amodel, afw, activation);
         idx    = (id < 0) ? -1 : sensor_claim_slot(id, cgm_type, s.mac);
         if (idx >= 0)
            LOGI("registered sensor id=%d type=%s mac=%s fw=%s", id,
                 sensor_type_name(cgm_type), s.mac, afw);
         else if (id >= 0)
            LOGI("sensor slots full (%d); %s not listed", MAX_SLOTS, s.mac);
      }
      /* THE ID ON THIS LINK, not the position it landed in: this is the
       * fallback provenance stamped onto every reading until the next
       * reconcile, in a log that is never rewritten. Resolved from the
       * ADDRESS, which is the one identity a registry shift cannot move --
       * and which is right whether the device was just minted here or was
       * already registered. */
      struct sensor_view sv;
      sensors_view_get(&sv);
      for (int i = 0; i < sv.n; i++)
         if (sv.have_rec[i] && !strcmp(sv.rec[i].identity, s.mac)) {
            reading_set_src(sv.slot[i].id);
            break;
         }
   }

   /* SECOND PASS: complete provenance for an ALREADY-registered CGM whose
    * learned attributes have since arrived.
    *
    * A CGM is registered BARE the moment the user commits to pairing it (see
    * commit_pair), so its permanent row starts with no model, no firmware and
    * an unknown activation. The DIS strings land a few seconds after the
    * first connection and the activation instant is only knowable once a
    * reading has anchored the session clock -- this pass writes each the
    * moment it becomes true, via sensor_complete, which fills ONLY missing
    * fields and appends the corrected row (the file is never rewritten).
    *
    * This used to mint a second id and rebind the slot to it. Since identity
    * became MAC-only that mint always returned the SAME id and the rebind was
    * a no-op -- the pass was dead code and rows stayed bare forever. */
   /* Collect from ONE snapshot, ACT afterwards. sensor_complete does
    * synchronous file I/O (sensors.csv), and the driver's lock is a spin
    * lock every GATT binder callback waits on -- holding it across a file
    * write burns a core out of the small binder pool. The first pass above
    * already releases it before minting for exactly this reason. */
   struct {
      char model[24];
      char fw[24];
      long act;
      int id;
   } todo[LINK_MAX];

   int ntodo = 0;
   struct dex_session csess[LINK_MAX];
   driver_snapshot(csess, -1, 0);
   for (int l = 0; l < LINK_MAX; l++) {
      if (meter_link_is(l))
         continue; /* CGMs only */
      struct dex_session ls = csess[l];
      if (!ls.mac[0] || !ls.bonded)
         continue;
      /* COPY the DIS strings -- reading_dis takes the lock they are written
       * under, on a binder thread, while this runs on the main thread.
       * Testing and snapshotting them unlocked (as this once did) made that
       * writer's lock inert: the emptiness test passes as soon as the writer
       * lands byte 0, so a firmware of "1.6.0.11" can be read as "1", minted
       * as a DIFFERENT id, and the slot rebound to it. The next tick sees a
       * non-empty fw, so stale_row is false and the truncated value is never
       * corrected -- in an append-only file. */
      char lmodel[24] = {0};
      char lfw[24]    = {0};
      reading_dis(l, lmodel, sizeof lmodel, lfw, sizeof lfw);
      /* Slot and provenance from ONE snapshot: read separately, a mint on a
       * binder thread between them gives an index into a shifted array. */
      struct sensor_view cv;
      sensors_view_get(&cv);
      int si = -1;
      for (int i = 0; i < cv.n && si < 0; i++)
         if (cv.have_rec[i] && !strcmp(cv.rec[i].identity, ls.mac))
            si = i;
      int cur_id           = (si >= 0) ? cv.slot[si].id : 0;
      struct sensor_rec cr = (si >= 0) ? cv.rec[si] : (struct sensor_rec){0};
      int is_cgm           = si >= 0 && sensor_kind(cr.type) == KIND_CGM;
      /* DIS strings only when BOTH have landed -- the same rule the meter
       * path already enforces. They are separate serialized GATT ops and the
       * sensor commonly closes the cycle before all of them land; writing
       * "model present, fw still empty" would append a correction row per
       * tick until fw arrived. sensor_complete cannot fork an id any more,
       * but the file should not carry churn either. */
      struct sens_obs so    = {0};
      struct sens_effect se = {0};
      so.is_cgm             = 1;
      so.has_mac            = 1;
      so.bonded             = 1;
      so.have_reading       = ls.have_reading;
      so.claimed            = 1;
      so.registered         = is_cgm;
      so.have_dis           = lmodel[0] && lfw[0];
      so.row_bare           = is_cgm && (!cr.model[0] || !cr.fw[0]);
      so.row_no_act         = is_cgm && !cr.activation;
      so.session_seconds    = (long)ls.session_seconds;
      sens_link_eval(&so, realtime_s(), &se);
      if (!se.complete_mfw && !se.complete_act)
         continue;
      todo[ntodo].id = cur_id;
      str_snapshot(todo[ntodo].model, sizeof todo[ntodo].model,
                   se.complete_mfw ? lmodel : "");
      str_snapshot(todo[ntodo].fw, sizeof todo[ntodo].fw,
                   se.complete_mfw ? lfw : "");
      todo[ntodo].act = se.activation;
      ntodo++;
   }
   for (int i = 0; i < ntodo; i++) {
      if (sensor_complete(todo[i].id, "", todo[i].model, todo[i].fw,
                          todo[i].act) == 1)
         LOGI("sensor provenance completed: id %d (%s / %s, act %ld)",
              todo[i].id, todo[i].model, todo[i].fw, todo[i].act);
   }

   /* NOTHING TO PUT BACK ANY MORE. This used to end with
    * a call selecting LINK_CGM and the note "leave the driver on a link that
    * actually exists: the caller's stall watchdog and build_model() both read
    * the session straight after this, and an unused link reports an empty
    * one". That was a workaround for the walk above moving the ambient
    * selection and leaving it on whatever link it stopped at. It reads each
    * link by name now, so it never moves the selection and there is no
    * "leave it somewhere sensible" to get right. */
   flight_leave(&g_reconcile_flight);
}

/* The service tick's route to the registry.
 *
 * sensor_reconcile ran only from on_timer -- the ACTIVITY's looper, which
 * on_destroy tears down. Its work includes minting a newly bonded CGM and
 * completing a sensor's provenance once its DIS strings arrive, both of which
 * write the append-only file. A sensor that bonds while the activity is gone
 * was therefore never registered, and its readings were stamped with the
 * fallback source id in a log that is never rewritten. meter_sync_watchdog was
 * lifted out for exactly this reason; the rest of the function was left
 * behind. */
void pancra_reconcile_tick(void)
{
   sensor_reconcile();
}
