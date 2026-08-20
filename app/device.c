// SPDX-License-Identifier: GPL-3.0
// device.c --- What can be DONE to a registered device (see device.h)
// Copyright 2026 Jakob Kastelic

#include "device.h"
#include "bletrans.h"
#include "clock.h"
#include "dexdriver.h"
#include "log.h"
#include "meter.h"
#include "nav.h"
#include "notify.h"
#include "reading.h"
#include "reconcile.h"
#include "selection.h"
#include "sensors.h"
#include "status.h" /* set_status: a change that was not saved must say so */
#include "store.h"

/* WHICH LINK the chosen sensor is on, for a calibration call, or -1 if this
 * slot may not be calibrated.
 *
 * It selects nothing and locks nothing: the driver takes a link as an
 * argument and scopes its own lock around the operation. This used to return
 * the PREVIOUS selection for the caller to restore, and to hand back with the
 * driver lock HELD, because a calibration write had to be atomic with the
 * ambient selection that aimed it. There is no ambient selection to aim, so
 * there is nothing here to hold. */
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
         set_status("RECONNECT NOT SAVED");
      int prime = sensor_primary_id();
      store_lock();
      hist_refresh_current(prime);
      store_unlock();
      notify_mark();
   }
   sel_set_device(-1);
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
       * This ran the other way round: forget the key, close the link, clear
       * the device-information strings, un-arm the radio -- and THEN call
       * sensor_retire, which rolls the registry back if slots.csv cannot be
       * replaced. The rollback restored a slot that says LIVE while its
       * transport state was already destroyed: no session key, no link, no
       * DIS, and a status line reading "DISCONNECT NOT SAVED" over a device
       * that the user can see is still listed. Nothing re-establishes any of
       * it short of a restart.
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
      /* RETIRE, do not delete: the slot is kept (marker, label, prefs) and
       * flagged old, so it moves to OLD DEVICES with its full per-device menu
       * intact. sensor_retire reassigns the primary to the first live CGM
       * left. BY ID: the snapshot above is where the index stops being
       * trusted. */
      if (sensor_retire(id) != SENSOR_OK) {
         set_status("DISCONNECT NOT SAVED");
         /* ...AND GO BACK, so the message is somewhere it can be READ. The
          * confirm screen draws no status line (only the main screen does),
          * so returning from here left the failure completely invisible: the
          * user taps DISCONNECT and nothing happens at all. Nothing has been
          * torn down, so the device is exactly as it was. */
         sel_set_device(-1);
         nav_back();
         return;
      }
      /* Release the BLE link BEFORE the slot array shifts. Leaving it
       * connected meant a forgotten sensor kept streaming on a link that
       * now belonged to a different slot ordinal -- and commit_pair would
       * later call driver_forget() on that same link, destroying the
       * SURVIVING sensor's bond. */
      const struct sensor_rec *fr = sensor_rec_of(id, &frv) ? &frv : 0;
      int flink                   = link_for_sensor(id);
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
         reading_forget_dis(flink); /* takes the DIS lock itself */
         /* GIVE THE LINK BACK. Nothing else here did, and for a METER
          * that leaked the link permanently: the armed table kept the
          * forgotten meter's MAC and the routing bit stayed set, which
          * excludes the link from BOTH pools -- meter_alloc_link skips
          * any link with g_link_armed set, free_cgm_link skips
          * g_link_armed || g_link_meter. Nothing ever cleared them,
          * because the only thing that does is meter_release_link on the
          * disconnect callback, and dexble_link_close on a connect that
          * is merely PENDING (a meter is switched off between
          * fingersticks, so that is the normal state) never produces one.
          * Every meter retired therefore burned one of LINK_MAX links for
          * the life of the process.
          *
          * UN-ARM, DO NOT RELEASE. An earlier version called
          * meter_release_link here, which clears the routing bit -- and a
          * disconnect callback still in flight (the meter really was
          * connected when forgotten) then landed in the CGM branch and
          * posted a spurious CONNECTION ERROR. The split exists for
          * exactly this: un-arm frees the meter pool now, the routing bit
          * stays until the callback lands (meter_hook_disconnected
          * releases fully), and if no callback is coming -- the normal
          * case, a PENDING connect on a switched-off meter -- the
          * teardown stamp below lets the stranded-link recovery in
          * meter_sync_watchdog give the link back after
          * METER_TEARDOWN_MAX. A 3-minute bounded hold instead of a
          * permanent leak, and no misrouting either way.
          *
          * The tick cannot re-arm meanwhile: it skips retired slots, and
          * this slot is retired two lines down.
          *
          * For a CGM all of this is a no-op: the armed entry is empty,
          * the meter bit is 0, and the stamp is gated on that bit. */
         meter_unarm_link(flink);
         int fmeter = meter_link_is(flink);
         /* MONOTONIC. This is a DEADLINE -- the stranded-link recovery
          * measures it against mono_s() -- and it was stamped from the wall
          * clock, which is roughly 1.7e9 seconds ahead. `now - stamped` was
          * therefore hugely negative on every tick, the 3-minute bound never
          * expired, and the link the comment above promises to give back
          * after METER_TEARDOWN_MAX was leaked for the life of the process.
          * See clockcheck, which now knows this setter. */
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
      /* Return to WHERE THE SENSOR SCREEN WAS OPENED FROM, not a
       * hardcoded SETTINGS: the detail screen is reachable from the
       * main-screen STATE/SESSION table (g_sensor_from == SCR_MAIN) as
       * well as from the settings DEVICES list, and disconnecting from
       * the former must land back on the main screen, not somewhere the
       * user never was. */
      nav_back();
   }
}
