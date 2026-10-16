// SPDX-License-Identifier: GPL-3.0
// device.c --- What can be DONE to a registered device (see device.h)
// Copyright 2026 Jakob Kastelic

#include "device.h"
#include "bletrans.h"
#include "clock.h"
#include "dexdriver.h"
#include "linkinfo.h"
#include "log.h"
#include "meter.h"
#include "nav.h"
#include "notify.h"
#include "pairing.h"
#include "reconcile.h"
#include "selection.h"
#include "sensors.h"
#include "status.h" /* set_status: a change that was not saved must say so */
#include "store.h"
#include "ui.h"
#include <string.h> /* strcmp: whose sensor a code file names */

/* WHICH LINK the chosen sensor is on, for a calibration call, or -1 if this
 * slot may not be calibrated.
 *
 * It selects nothing and locks nothing: the driver takes a link as an
 * argument and scopes its own lock around the operation. With an ambient
 * selection to aim, this would have to return the PREVIOUS one for the caller
 * to restore, and hand it back with the driver lock HELD, because a
 * calibration write must be atomic with the selection that aims it. There is
 * no such selection, so there is nothing here to hold. */
int cal_link(void)
{
   /* BY ID from end to end. This decides whether a CALIBRATION -- the most
    * consequential write the app makes -- is permitted, and which radio link
    * it goes out on. A slot index between the two questions could answer
    * "yes, a CGM" about the device the user picked and then hand back the
    * link of whichever device had moved into its position. */
   int id = sel_device();
   struct sensor_rec r;
   if (!sensor_slot_of(id, 0) || !sensor_rec_of(id, &r))
      return -1;
   if (sensor_kind(r.type) != KIND_CGM)
      return -1;
   return link_for_sensor(id);
}

/* Is this old device already past its wear + grace? Reconnecting a
 * pre-expiry disconnect is sensible; a post-expiry one is confirmed first.
 * BY ID: the answer decides whether a CONFIRMATION screen is shown, and the
 * confirmation is a later tap on a device the index may no longer name. */
int old_sensor_expired(int id)
{
   struct sensor_slot sl;
   if (!sensor_slot_of(id, &sl))
      return 0;
   struct sensor_rec rv;
   const struct sensor_rec *r = sensor_rec_of(id, &rv) ? &rv : 0;
   long act                   = r ? r->activation : 0;
   long wear = sensor_wear_seconds(r ? r->type : SENSOR_STELO, sl.wear_days,
                                   r ? r->model : "");
   if (act <= 0 || wear <= 0)
      return 0; /* unknown -> treat as not-yet-expired (allow direct) */
   return realtime_s() > act + wear + SENSOR_GRACE_S;
}

/* Revive this device and leave the per-device screen. */
void do_reconnect(int id)
{
   if (sensor_slot_of(id, 0)) {
      /* SAY SO IF IT WAS NOT WRITTEN. A revival that never reached slots.csv
       * is a device the user watched come back and that is disconnected
       * again at the next launch. Nothing else here would notice. */
      if (sensor_revive(id) != SENSOR_OK)
         set_status_refused("RECONNECT NOT SAVED");
      int prime = sensor_primary_id();
      store_lock();
      hist_refresh_current(prime);
      store_unlock();
      notify_mark();
   }
   sel_set_device(-1);
   /* CLOSE WHAT THIS RAN FROM BY ASKING WHAT IS ON TOP, the same way
    * device_retire does, and for the same reason: an expired device routes
    * through a confirmation screen first, so the number of screens to pop
    * differs by route. Popping once from the confirmation leaves the user on
    * the detail screen with the selection just cleared, where render_sensor's
    * range guard draws the bare NO DEVICE screen. */
   if (cur_screen() == SCR_RECONF)
      nav_back();
   if (cur_screen() == SCR_SENSOR)
      nav_back();
}

/* RETIRE A DEVICE: the app's most consequential destructive action, and the
 * one with the most order-dependent steps -- which is why it is a named
 * function with its reasoning attached rather than a branch inside a menu
 * dispatcher, where each of the failures documented below reached the phone
 * before anyone noticed.
 *
 * RETIRE, DO NOT DELETE. The slot keeps its marker, label and preferences and
 * is flagged old, so it moves to OLD DEVICES with its per-device menu intact;
 * the provenance row stays, so historical readings keep resolving to the
 * sensor that actually made them. */
void device_retire(int id)
{
   /* BY ID, INCLUDING THE RADIO. The id logged, the provenance that decides
    * whether a link is released, the link itself and the retirement all have
    * to name the SAME device -- this is the action that takes a sensor off
    * the plot and hands its link back, and it runs one confirmation tap after
    * the screen that named the device. Resolving a slot index here could
    * close the radio link of a sensor that had merely slid into position. */
   struct sensor_rec frv;
   if (!sensor_slot_of(id, 0))
      return;
   {
      /* THE DURABLE CHANGE FIRST, AND THE TEARDOWN ONLY IF IT LANDED.
       *
       * sensor_retire rolls the registry back when slots.csv cannot be
       * replaced, and the slot then stays LIVE. Torn down before that, it
       * would be a listed device with no session key, no link and no DIS,
       * under "DISCONNECT NOT SAVED", and nothing re-establishes those short
       * of a restart.
       *
       * So the registry commits first. If it will not, nothing has been
       * taken apart and the device goes on working exactly as it did.
       *
       * Retiring before the teardown is safe in the other direction too: the
       * 1 Hz tick skips retired slots, so it cannot re-arm the meter in the
       * window between the two, and a stray GATT callback for a link whose
       * slot is retired is what meter_hook_disconnected and the CGM
       * disconnect path already handle. */
      LOGI("forgetting device id %d; history retained", id);
      /* WHICH LINK IS THIS DEVICE'S, asked while its slot is still live: a
       * sensor with no address on any link is placed by rank among the live
       * ones, and asked after the retire it ranks as a different device and
       * names that device's link -- whose credentials the teardown below
       * would then erase. Only the lookup comes first; the registry commits
       * before anything is torn down. */
      const struct sensor_rec *fr = sensor_rec_of(id, &frv) ? &frv : 0;
      /* A METER'S LINK IS FOUND IN THE ARM TABLE, NOT BY ADDRESS.
       * link_for_sensor resolves through the driver session, which a meter
       * never has (see meter_link_of_mac) -- so asked about one it returns
       * whatever free CGM link the ranking lands on, and the teardown below
       * would erase THAT sensor's key file while the meter's own link stayed
       * armed for the life of the process. */
      int flink = -1;
      if (fr && sensor_kind(fr->type) == KIND_BGM)
         flink = meter_link_of_mac(fr->identity);
      else if (fr)
         flink = link_for_sensor(id);
      /* A LINK WITHOUT AN ADDRESS IS HANDED OUT BY RANK, so for a sensor not
       * yet on the air it can be the link another sensor is pairing on. Its
       * code file names the sensor it was saved for; when that is another
       * sensor, the link is not this device's to tear down. */
      if (fr && sensor_kind(fr->type) == KIND_CGM && flink >= 0) {
         struct dex_session fs;
         char owner[24];
         if (driver_session_of(flink, &fs) && !fs.mac[0] &&
             dexble_code_owner(flink, owner, (int)sizeof owner) &&
             strcmp(owner, fr->identity) != 0)
            flink = -1;
      }

      /* RETIRE, do not delete: the slot is kept (marker, label, prefs) and
       * flagged old, so it moves to OLD DEVICES with its full per-device menu
       * intact. sensor_retire reassigns the primary to the first live CGM
       * left. BY ID: the snapshot above is where the index stops being
       * trusted. */
      if (sensor_retire(id) != SENSOR_OK) {
         set_status_refused("DISCONNECT NOT SAVED");
         /* ...AND GO BACK, so the message is somewhere it can be READ. The
          * confirm screen draws no status line (only the main screen does),
          * so returning from here left the failure completely invisible: the
          * user taps DISCONNECT and nothing happens at all. Nothing has been
          * torn down, so the device is untouched. */
         sel_set_device(-1);
         nav_back();
         return;
      }
      /* OFF THE CANDIDATE LIST, link or no link: an advert heard just
       * before the disconnect would otherwise let a pairing already armed
       * pick it up and revive the slot. DECLINED as well: a sensor that
       * never streamed is listed again by its next advert, and only the
       * declined set keeps the tick from committing to it on its own. An
       * explicit YES still pairs it, and a newly typed code clears the
       * declined set. */
      if (fr) {
         pairing_forget_candidate(fr->identity);
         pairing_decline(fr->identity);
      }
      /* AND THE WAIT ENDS WITH ITS OWN DEVICE: left armed, the next advert
       * of a sensor that never streamed makes it a candidate again, and the
       * wait would commit to it and bring the slot back. */
      if (pairing_pending_device() == id)
         pairing_arm(0);
      /* RELEASE THE BLE LINK. Left connected, a forgotten sensor would keep
       * streaming on a link another slot can be given -- and commit_pair
       * would later call driver_forget() on that same link, destroying the
       * SURVIVING sensor's bond. */
      /* BOTH bounds: flink indexes driver contexts and g_model_l below,
       * and the DIS block a few lines down already checks both. */
      if (fr && flink >= 0 && flink < LINK_MAX) {
         driver_forget(flink);
         dexble_link_close(flink);
         /* Clear this link's cached DIS strings. Nothing else does, and
          * the re-read gate is "already non-empty -> never ask again" --
          * so the NEXT sensor to claim this link (there are only 4, and
          * replacing a sensor every 15 days forces reuse) would be minted
          * carrying the FORGOTTEN sensor's model and firmware, in a
          * provenance row that is never rewritten and whose fields are
          * part of the id-reuse key. */
         linkinfo_forget_dis(flink);  /* takes the DIS lock itself */
         linkinfo_forget_rssi(flink); /* and its signal sample */
         /* GIVE THE LINK BACK. For a METER, the armed table holds its
          * MAC and the routing bit is set, which excludes the link from
          * BOTH pools -- meter_alloc_link's claim (driver_link_claim)
          * skips any link with g_link_armed set, driver_free_cgm_link_in
          * skips any link driver_link_armed or driver_link_is_meter
          * reports. The disconnect callback's meter_release_link clears them, and
          * dexble_link_close on a connect that is merely PENDING (a meter
          * is switched off between fingersticks, so that is the normal
          * state) never produces that callback. Without this, every meter
          * retired would hold one of LINK_MAX links for the life of the
          * process.
          *
          * UN-ARM, DO NOT RELEASE. meter_release_link here clears the
          * routing bit -- and a disconnect callback still in flight (the
          * meter really was connected when forgotten) then lands in the CGM
          * branch and posts a spurious CONNECTION ERROR. The split exists
          * for
          * exactly this: un-arm frees the meter pool now, the routing bit
          * stays until the callback lands (meter_hook_disconnected
          * releases fully), and if no callback is coming -- the normal
          * case, a PENDING connect on a switched-off meter -- the
          * teardown stamp below lets the stranded-link recovery in
          * meter_sync_watchdog give the link back after
          * METER_TEARDOWN_MAX. A 3-minute bounded hold rather than a
          * permanent leak, and no misrouting either way.
          *
          * The tick cannot re-arm meanwhile: it skips retired slots, and
          * this slot was retired before the teardown began.
          *
          * For a CGM all of this is a no-op: the armed entry is empty,
          * the meter bit is 0, and the stamp is gated on that bit. */
         meter_unarm_link(flink);
         int fmeter = meter_link_is(flink);
         /* MONOTONIC: this is a DEADLINE, and the stranded-link recovery
          * measures it against mono_s(). */
         if (fmeter)
            meter_link_idle(flink, mono_s());
      }
      /* Re-bind the big number to the new owner immediately, clearing it
       * if no eligible live sample exists -- the disconnected sensor's
       * value must not stay latched on screen. */
      int prime = sensor_primary_id();
      store_lock();
      hist_refresh_current(prime);
      store_unlock();
      notify_mark();
      sel_set_device(-1);
      /* CLOSE BOTH SCREENS THIS RAN FROM, and close them because of what
       * they are about rather than by counting steps.
       *
       * The confirmation sits on top of the retired device's OWN screen, and
       * both read the selection that was just cleared. The detail screen with
       * nothing selected has nothing to show (render_sensor's "NO DEVICE"),
       * so it goes too.
       *
       * Landing place, once both are off the path: WHERE THE SENSOR SCREEN
       * WAS OPENED FROM, which the path holds and no caller has to name. The
       * detail screen is reachable from the main-screen STATE/SESSION table
       * as well as from the settings DEVICES list, and disconnecting from the
       * former must land back on the main screen, not somewhere the user
       * never was.
       *
       * TESTED, NOT COUNTED. Two bare nav_back() calls would be right for
       * this one route and wrong for any other way in; asking what is on top
       * is right for every route, including ones added later. */
      if (cur_screen() == SCR_FORGET)
         nav_back();
      if (cur_screen() == SCR_SENSOR)
         nav_back();
   }
}
