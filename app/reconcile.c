// SPDX-License-Identifier: GPL-3.0
// reconcile.c --- Sessions and the registry, kept in agreement (see
// reconcile.h) Copyright 2026 Jakob Kastelic

#include "reconcile.h"
#include "clock.h"
#include "devtag.h" /* a log may not carry an address; see there */
#include "dexdriver.h"
#include "linkinfo.h"
#include "log.h"
#include "meter.h"
#include "selection.h"
#include "senslogic.h"
#include "sensors.h"
#include "sesscache.h" /* sess_flush: the tick persists the session clock */
#include "status.h" /* set_status_refused: a sensor that could not be registered */
#include "thread.h"
#include "util.h"
#include <string.h>

/* Runs from the activity's 1 Hz timer AND, once that timer is gone, from the
 * service tick. Serialised by a try-lock because those are different threads:
 * it mints ids and appends to the provenance file, and two concurrent passes
 * could mint twice for one sensor. Skipping a tick is free. */
static struct flight g_reconcile_flight = FLIGHT_INIT;

/* (The link searches themselves are the DRIVER's: driver_link_of_identity
 * and driver_free_cgm_link_in. They are questions about links and sessions,
 * which is the driver's state -- keeping them here made the meter runtime
 * include this file to ask one, and this file is a workflow that calls back
 * into the meter.) */

/* Map a sensor slot to its transport link, BY ADDRESS.
 *
 * BY ADDRESS AND NOT BY ORDINAL, because a sensor's ordinal among the CGM
 * slots is not stable: sensor_forget() shifts g_slot while the remaining
 * sensors keep their live GATT connections, driver contexts and per-link key
 * files. Forgetting the first of two CGMs would re-point the survivor at an
 * emptied context, after which commit_pair() calls driver_forget() on the link
 * the survivor is ACTUALLY using and destroys its bond; calibration
 * went to a dead context while still logging "submitted"; and the survivor's
 * adverts stopped resolving to a live link. Resolving by the session address
 * -- the one identity a shift cannot move -- removes the whole class.
 *
 * An index one past the end is a legitimate query ("the link a NEW sensor would
 * take"), which is what free_cgm_link answers. */
/* THE LINK FOR ONE ROW OF A SNAPSHOT ALREADY TAKEN. The index is only ever
 * an index INTO `vp`, so it cannot go stale: it is read from the same instant
 * as the walk that ranks it. Callers outside this file name a DEVICE
 * (link_for_sensor) or ask for a new one (link_for_new_sensor). */
static int link_in_view(const struct sensor_view *vp, int idx)
{
   /* READ THROUGH THE POINTER, because a published view never changes: the
    * caller holds a reference to it, and a registry mutation builds a NEW
    * view and swaps it in rather than editing this one. srec_push's
    * id-ordered shift and sensor_forget's shift-down both move the LIVE
    * tables, which this function does not touch -- so the records here cannot
    * move underneath the walk, and an index into `vp` stays the row it named.
    *
    * That is what makes it safe to run on a binder thread (jni_on_advert)
    * while the main thread mutates the registry, and it is load-bearing: the
    * answer feeds dexble_pair, where a torn read would connect one sensor's
    * address on another's link, using the wrong key file. */
   const char *ident = NULL;
   if (idx >= 0 && idx < vp->n && vp->have_rec[idx])
      ident = vp->rec[idx].identity;
   /* THE DRIVER'S STATE AS ONE INSTANT, taken through its own operation
    * rather than by holding its lock from here: every link lookup below reads
    * this copy, so they cannot disagree with each other, and this file no
    * longer reasons about somebody else's mutex. */
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   int link = ident ? driver_link_of_identity_in(sess, ident) : -1;
   if (link < 0) {
      /* Not yet bound (no session on any link -- the normal state right after
       * a restart). Rank this slot among the OTHER unbound slots so each one
       * claims a different free link. Ranking by slot order means a device
       * that IS bound never reaches here, so a forget cannot renumber a live
       * one.
       *
       * ONLY CGMs ARE RANKED, and the reason is what the ranking is counted
       * AGAINST: driver_free_cgm_link_in walks the links a CGM could take,
       * and it skips every link a meter holds (see there -- a meter's link
       * carries no driver session, so it would otherwise read as free and a
       * CGM allocated onto it would seize the meter's GATT client).
       *
       * A meter therefore costs that walk a link ALREADY. Counting it here as
       * well spends it twice: the rank climbs past the end of a list the
       * meter has itself shortened, and with three meters registered the walk
       * runs off four idle links and answers "none free". A meter still gets
       * a link of its own -- the meter runtime allocates it (meter.h), which
       * is a different pool and not this one. */
      /* THE LIVE PREFIX ONLY. The view puts live slots first, so the ranking
       * stops at n_live rather than walking every device ever registered in
       * order to skip them one at a time -- a retired device holds no link,
       * which is why it was skipped here and why it is not reached now. */
      int rank = 0;
      int lim  = idx < vp->n_live ? idx : vp->n_live;
      for (int i = 0; i < lim; i++) {
         if (!vp->have_rec[i])
            continue;
         if (sensor_kind(vp->rec[i].type) != KIND_CGM)
            continue; /* its link comes from the meter runtime's pool */
         if (driver_link_of_identity_in(sess, vp->rec[i].identity) < 0)
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
   const struct sensor_view *vp = sensors_view_ref();
   int link                     = -1;
   for (int i = 0; i < vp->n; i++)
      if (vp->slot[i].id == id) {
         link = link_in_view(vp, i);
         break;
      }
   sensors_view_put(vp);
   return link;
}

/* THE LINK A NEW SENSOR WOULD TAKE: the same answer for an index one past the
 * end (see link_in_view), with the count taken from the same snapshot. Named,
 * so no caller has to know that "one past the end" is the way to ask. */
int link_for_new_sensor(void)
{
   const struct sensor_view *vp = sensors_view_ref();
   /* ONE PAST THE WHOLE TABLE, which is what makes the identity lookup in
    * link_in_view come back empty -- that emptiness is the question being
    * asked, "the link a device with no address yet would take". n_live is NOT
    * one past the end: the retired devices sit at [n_live, n), so as soon as
    * one exists that index names a real device and the answer becomes the
    * link belonging to whichever device happens to sit there. The ranking is
    * unaffected either way -- it stops at n_live for any index at or past
    * it -- so this changes only whether an identity is found. */
   int link = link_in_view(vp, vp->n);
   sensors_view_put(vp);
   return link;
}

void sensor_reconcile(void)
{
   if (!flight_enter(&g_reconcile_flight))
      return;
   meter_sync_watchdog();

   /* Walk every CGM link so a newly bonded second sensor is registered too,
    * not just whichever one happened to connect first.
    *
    * ONE OBSERVATION PER LINK, which is what bounds this array: a row is
    * pushed only for a slot that resolved to a link, and no two slots resolve
    * to the same one -- a bound slot takes the link carrying its own address,
    * and the unbound are ranked onto distinct free links. */
   /* THE DRIVER AS ONE INSTANT, then the registry under its own lock. Held by
    * hand the two become ONE critical section here; a snapshot gives what the
    * walk actually needs -- every link's session as it was at one moment --
    * without this file taking the driver's lock. */
   struct dex_session sess[LINK_MAX];
   driver_snapshot(sess, -1, 0);
   long now = realtime_s();
   /* ot_drv_done mutates the registry from a binder thread (sensor_mint ->
    * srec_push), so this walk reads ONE snapshot. Held as a lock instead, it
    * also spanned link_for_sensor, which takes the DRIVER's lock: registry ->
    * driver, the inverse of the documented order. A snapshot cannot invert
    * anything, because it is over. */
   const struct sensor_view *rv = sensors_view_ref();
   for (int i = 0; i < rv->n_live; i++) {
      if (!rv->have_rec[i] || sensor_kind(rv->rec[i].type) != KIND_CGM)
         continue;
      int l = link_in_view(rv, i);
      if (l < 0)
         continue;
      struct dex_session ls = sess[l];
      /* THE SESSION IS RECORDED HERE, NOT WHILE DRAWING.
       *
       * A sessc_put in build_model -- once per CGM row per frame -- makes
       * the durable session clock a side effect of the REDRAW: a screen
       * nobody is looking at records nothing, and a busy one records several
       * times a second. That is the wrong dependency in
       * both directions. The one that cost something is the first -- with
       * the activity gone the service tick is the only thing running, it
       * draws nothing, and the cache it is flushing every minute therefore
       * never changed. The restore this cache exists for was doing nothing
       * on exactly the launches it was written for.
       *
       * This walk is the right place because it already has what the record
       * needs and takes no new lock to get it: one driver snapshot, every
       * CGM slot, and the same instant for all of them. It runs on the 1 Hz
       * timer AND on the service tick, so the record survives the activity;
       * and it runs at a fixed cadence, so nothing about the screen can
       * change what is stored.
       *
       * `now` is read once, above, for the same reason the snapshot is taken
       * once: two rows stamped from two instants are two different answers
       * to "when was this clock read". */
      if (ls.have_reading)
         sessc_put(rv->slot[i].id, &ls, now);
   }
   /* Released as soon as the walk is done: nothing below reads the registry
    * again on this pass. */
   sensors_view_put(rv);
   /* AND THEN THE FILE, at most once a minute (senslogic.h). AFTER the walk
    * above, so a session recorded on this tick can be written by this tick
    * rather than waiting for the next one; the rate limit is what keeps that
    * from becoming a write per tick. Losing up to a minute costs nothing --
    * the clock is projected forward from whatever instant was stored. */
   sess_flush(now);

   /* Only a CGM is registered from a dex_session. Without this guard, adding a
    * meter would leave sel_add_type() on ONETOUCH and the next CGM to bond
    * would be minted with the wrong type -- and a wrong type is permanent,
    * because the provenance row is never rewritten. */
   int cgm_type = (sensor_kind(sel_add_type()) == KIND_CGM) ? sel_add_type()
                                                            : SENSOR_STELO;
   /* The link a new pairing would use. Note this must not be left selected on
    * return: the caller's stall watchdog and build_model() both read the
    * driver afterwards, and an unused link reports an empty session. */
   /* Recover the meter's id FIRST and unconditionally -- after the CGM
    * early-return below, a meter-only user never recovers it after a restart
    * and their meter can never auto-sync again. */
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
   const struct sensor_view *mv = sensors_view_ref();
   /* THE LIVE PREFIX. A retired meter is one the user took out of service,
    * and binding it here would put it back into auto-sync behind their back;
    * reviving it is what asks for that. */
   for (int i = 0; i < mv->n_live && !have_meter; i++) {
      if (mv->have_rec[i] && mv->rec[i].type == SENSOR_ONETOUCH) {
         /* The ADDRESS goes with the id. Without it the "is this our meter"
          * guard was empty after a restart and accepted ANY OneTouch in
          * range -- importing a stranger's readings under our sensor id. */
         /* A REFUSAL NEEDS NO HANDLING: it means a sync is already running,
          * and that sync bound its own source when it claimed. The next tick
          * asks again. */
         (void)meter_bind(mv->slot[i].id, mv->rec[i].identity);
         break;
      }
   }
   sensors_view_put(mv);

   /* Find a CGM link carrying a live bonded session that NO slot claims yet --
    * that is the sensor which still needs registering.
    *
    * NOT link_for_new_sensor(), "the link a new pairing would use":
    * with address-based link resolution that is a guaranteed dead end.
    * that answer for an unregistered index is a FREE link, and a free
    * link is by definition one with no session, so s.mac[0] is always 0 and
    * this entire block stops executing -- nothing minted or slotted, every
    * reading falling back to source id 0 ("pre-registry legacy") in a log
    * that is never rewritten, and the advert-driven reconnect loop (which
    * iterates slots) left with an empty body. Scanning for the unclaimed
    * session asks the question directly. */
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
          sensor_id_by_mac(ls.mac) < 0) {
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
      /* AN ID, not a row: the question is only whether some slot already
       * claims this address. An index would name a row nothing below reads,
       * and `idx` further down is the row the claim RETURNS. */
      int held              = sensor_id_by_mac(s.mac);
      int idx               = -1;
      struct sens_obs so    = {0};
      struct sens_effect se = {0};
      so.is_cgm             = 1;
      so.has_mac            = 1;
      so.bonded             = s.bonded;
      so.have_reading       = s.have_reading;
      so.claimed            = (held > 0);
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
         char asn[24]    = {0};
         linkinfo_dis(s_link, amodel, sizeof amodel, afw, sizeof afw, asn,
                      sizeof asn);
         int id = sensor_mint(cgm_type, s.mac, asn, amodel, afw, activation);
         idx    = (id < 0) ? -1 : sensor_claim_slot(id, cgm_type, s.mac);
         /* THE ID, NOT THE ADDRESS. The registry id is this app's own name
          * for the device: it identifies no hardware, it means something to
          * anybody reading the logs beside the registry, and it is what every
          * other line about this sensor already carries. The per-run tag is
          * there so a line here can be matched with the transport's, which
          * only ever knows the address. See devtag.h. */
         char dt[DEVTAG_LEN];
         if (idx >= 0)
            LOGI("registered sensor id=%d type=%s dev %s fw=%s", id,
                 sensor_type_name(cgm_type), devtag(s.mac, dt), afw);
         else if (id >= 0)
            LOGI("sensor slots full (%d); dev %s not listed", MAX_SLOTS,
                 devtag(s.mac, dt));
         /* A REFUSAL ON THE AUTOMATIC PATH IS STILL A REFUSAL, and this is the
          * only place it can be said. A worn sensor that cannot be registered
          * streams readings nothing can attribute: they are deferred rather
          * than misfiled, so no record is damaged, but the user sees a device
          * that never appears in the list and is told nothing at all. The
          * manual path distinguishes the three causes the same way. */
         if (id < 0) {
            LOGW("sensor NOT registered: dev %s could not be minted",
                 devtag(s.mac, dt));
            /* THE THREE REASONS A MINT FAILS ARE THREE DIFFERENT MESSAGES, and
             * two of them are a named file: a mint reads both registry tables
             * and refuses while either is short. Neither is fixed by retrying,
             * so neither may be reported as a storage fault. */
            const char *why = "REGISTER FAILED: STORAGE?";
            if (!sensors_slots_whole())
               why = "SLOTS.CSV UNREADABLE: RESTART";
            else if (!sensors_provenance_loaded())
               why = "SENSORS.CSV UNREADABLE: RESTART";
            set_status_refused(why);
         } else if (idx < 0) {
            set_status_refused(sensors_writable()
                                   ? "LIST FULL: DISCONNECT ONE"
                                   : "REGISTRY UNREADABLE: RESTART");
         }
      }
      /* (NO AMBIENT PROVENANCE IS PUBLISHED HERE. A reading is stamped with
       * the sensor whose LINK it arrived on, resolved per reading, and a link
       * no slot claims yet defers rather than borrowing somebody's id. The
       * registration above is what makes that resolution possible, and it is
       * the whole of this block's job.) */
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
    * It does NOT mint a second id and rebind the slot to it: with MAC-only
    * identity such a mint returns the SAME id and the rebind is a no-op, so
    * the pass would be dead code and rows would stay bare forever. */
   /* Collect from ONE snapshot, ACT afterwards. sensor_complete does
    * synchronous file I/O (sensors.csv), and the driver's lock is a spin
    * lock every GATT binder callback waits on -- holding it across a file
    * write burns a core out of the small binder pool. The first pass above
    * already releases it before minting for exactly this reason. */
   struct {
      char model[24];
      char fw[24];
      char sn[24];
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
      char lsn[24]    = {0};
      linkinfo_dis(l, lmodel, sizeof lmodel, lfw, sizeof lfw, lsn, sizeof lsn);
      /* Slot and provenance from ONE snapshot: read separately, a mint on a
       * binder thread between them gives an index into a shifted array. */
      const struct sensor_view *cv = sensors_view_ref();
      /* THE CGM AT THIS ADDRESS. This walk is over CGM links, so the kind is
       * part of the question -- asked afterwards it would answer "none"
       * whenever another kind held the lower slot for that address. */
      int si               = sensors_view_find_mac_kind(cv, ls.mac, KIND_CGM);
      int cur_id           = (si >= 0) ? cv->slot[si].id : 0;
      struct sensor_rec cr = (si >= 0) ? cv->rec[si] : (struct sensor_rec){0};
      /* Both facts are copies now, so the view is done with -- and this runs
       * inside a per-link loop, where holding one would keep a version alive
       * for every link walked. */
      sensors_view_put(cv);
      int is_cgm = si >= 0 && sensor_kind(cr.type) == KIND_CGM;
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
      /* A ROW IS BARE IF ANY OF THE THREE IS MISSING AND CAN BE SUPPLIED --
       * the serial included. Testing only model and firmware leaves every
       * device registered before the serial was recorded permanently without
       * one: its row is already complete by that test, so the pass that would
       * write the serial never runs, and the SN line on its screen stays
       * blank for ever. The `lsn[0]` half is what stops the opposite
       * failure -- a sensor that reports no serial would otherwise make this
       * row bare on every tick, for a value nothing can supply. */
      so.row_bare =
          is_cgm && (!cr.model[0] || !cr.fw[0] || (!cr.serial[0] && lsn[0]));
      so.row_no_act      = is_cgm && !cr.activation;
      so.session_seconds = (long)ls.session_seconds;
      sens_link_eval(&so, realtime_s(), &se);
      if (!se.complete_mfw && !se.complete_act)
         continue;
      todo[ntodo].id = cur_id;
      str_snapshot(todo[ntodo].model, sizeof todo[ntodo].model,
                   se.complete_mfw ? lmodel : "");
      str_snapshot(todo[ntodo].fw, sizeof todo[ntodo].fw,
                   se.complete_mfw ? lfw : "");
      /* The serial rides with the model and firmware: one DIS read answers
       * all three. */
      str_snapshot(todo[ntodo].sn, sizeof todo[ntodo].sn,
                   se.complete_mfw ? lsn : "");
      todo[ntodo].act = se.activation;
      ntodo++;
   }
   for (int i = 0; i < ntodo; i++) {
      if (sensor_complete(todo[i].id, todo[i].sn, todo[i].model, todo[i].fw,
                          todo[i].act) == 1)
         LOGI("sensor provenance completed: id %d (%s / %s, act %ld)",
              todo[i].id, todo[i].model, todo[i].fw, todo[i].act);
   }

   /* NOTHING TO PUT BACK. A walk that moved an ambient selection would have
    * to end by "leaving the driver on a link that actually exists", because
    * the caller's stall watchdog and build_model() both read the session
    * straight after this and an unused link reports an empty one. This reads
    * each link by name, so it never moves a selection and there is no
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
