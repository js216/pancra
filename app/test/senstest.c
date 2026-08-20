// SPDX-License-Identifier: GPL-3.0
// senstest.c --- Sensor/session reconciliation decisions (see senslogic.h)
// Copyright 2026 Jakob Kastelic
//
/* Every "yes" this module returns writes the APPEND-ONLY provenance file, so
 * a wrong answer here is permanent: a duration stored as a timestamp, a
 * truncated firmware, a second sensor's readings stamped with the first's id.
 * Each test below names the failure it prevents, and several of them are
 * failures that actually shipped. */

#include "senslogic.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* sensors.h's SENSOR_GRACE_S, restated. This module is pure and does not
 * include the registry header, and the expiry rule the case below models
 * lives in uimenu.c; 12 h is what that rule allows past the wear budget. */
#define SENSOR_GRACE_S_FIXTURE (12L * 3600)

static int fails;
/* HOW MANY CHECKS ACTUALLY RAN. ck is silent on success, which is the right
 * amount of noise -- but it means an empty run and a clean run look exactly
 * alike from outside, and "no output" has been read as "no failures" here
 * before. The count is printed at the end so a caller can tell a suite that
 * passed from one that died before its first assertion. */
static int checks;

static void ck(int cond, const char *what)
{
   checks++;
   if (!cond) {
      printf("FAIL: %s\n", what);
      fails++;
   }
}

/* A bonded CGM link carrying a real reading, unclaimed by any slot. */
static struct sens_obs fresh(void)
{
   struct sens_obs o = {0};
   o.is_cgm          = 1;
   o.has_mac         = 1;
   o.bonded          = 1;
   o.have_reading    = 1;
   o.session_seconds = 3600;
   return o;
}

int main(void)
{
   printf("== projecting the sensor's session clock between responses ==\n");
   {
      /* THE ARITHMETIC THAT DECIDES WARMUP AND SESSION END.
       *
       * The sensor answers every ~5 minutes; the warmup countdown and the
       * expiry test both read a clock projected forward from that answer, and
       * for most of this app's life the projection was
       *
       *     last_clock + (uint32_t)(realtime_s() - <realtime stamp>)
       *
       * with BOTH stamps off the wall clock. A phone that comes back from
       * being off, or finds a network and corrects itself, moves that
       * difference by the whole correction. These cases are written as the
       * elapsed-time pairs the driver now passes, and the isolating one is
       * the NEGATIVE delta: it is the one the old arithmetic got wrong, and
       * it is the only one that can tell a bounded projection from an
       * unbounded one. */
      ck(sens_project_clock(1200, 5000, 5000) == 1200,
         "no time elapsed, no movement");
      ck(sens_project_clock(1200, 5000, 5060) == 1260,
         "a minute elapsed advances the session clock by a minute");

      /* THE ONE HOUR BACKWARDS. Under the old code this difference was -3600,
       * the cast made it 4294963696, and the unsigned add wrapped: a sensor
       * 1200 s into its warmup hour projected to 4294964896.
       *
       * Both consequences are asserted as the SCREEN would compute them,
       * because "the number is wrong" is not what the user experiences.
       * uidev.c calls it warmup while session_seconds < SENSOR_WARMUP_S
       * (3600), so the wrapped value ends warmup 40 minutes early and the app
       * announces data that will not arrive; uimenu.c's cgm_expired() asks
       * session_seconds > wear_len + grace, so the SAME value simultaneously
       * declares a sensor 20 minutes old to be finished for good. */
      {
         uint32_t p = sens_project_clock(1200, 5000, 5000 - 3600);
         ck(p == 1200, "a clock that went BACKWARD does not move the "
                       "projection");
         ck(p < 3600U, "...so a sensor 20 min into warmup is still warming "
                       "up");
         ck(!(p > (10L * 86400) + SENSOR_GRACE_S_FIXTURE),
            "...and is not simultaneously declared expired");
      }

      /* A day of silence is the cap, not a licence to keep counting: past
       * that the link has been down for a wear's worth of cycles and the
       * countdown would be fiction that still ticks. */
      ck(sens_project_clock(1200, 0, SENS_PROJECT_MAX_S) ==
             1200 + (uint32_t)SENS_PROJECT_MAX_S,
         "a full day is still projected");
      ck(sens_project_clock(1200, 0, SENS_PROJECT_MAX_S + 1) ==
             1200 + (uint32_t)SENS_PROJECT_MAX_S,
         "...and a day and a second is not");

      /* SATURATE, DO NOT WRAP. A clock near the top of the uint32 range plus
       * a bounded delta would otherwise come back as a few seconds -- a
       * session that has run for an eternity reported as one that just
       * began, which is the same lie in the other direction. */
      ck(sens_project_clock(0xfffffff0U, 0, 100) == 0xffffffffU,
         "a clock near the top of the range saturates rather than wrapping");

      /* No response has ever been decoded: the session has not started, so
       * it projects to nothing rather than to the time since boot. */
      ck(sens_project_clock(0, 0, 999999) == 0,
         "a session that never started projects to nothing");
   }

   printf("== the session cache is written on a SAVE cadence ==\n");
   {
      /* The rate limit is against the last SAVE, not the last change. The
       * clock projects forward every second, so sessc_put marks a change
       * every second and a "quiet for 60 s" rule is never satisfied while a
       * sensor is connected -- session.cache was never written at all, and
       * the restore it exists for silently did nothing on every launch. */
      struct sens_cache c = {0};
      ck(!sens_cache_due(&c, 1000), "a clean cache is not written");
      sens_cache_touch(&c);
      ck(sens_cache_due(&c, 1000), "a dirty cache with no prior save is due");
      sens_cache_done(&c, 1000);
      ck(!sens_cache_due(&c, 1000), "...and is clean once written");
      /* CLEAN, not merely rate-limited: a successful write must not leave the
       * cache due again the moment the interval lapses, or the file is
       * rewritten forever at 1/min with nothing new in it. */
      ck(!sens_cache_due(&c, 1000 + SENS_FLUSH_MIN_S),
         "...and stays clean once the interval lapses");

      sens_cache_touch(&c);
      ck(!sens_cache_due(&c, 1000 + SENS_FLUSH_MIN_S - 1),
         "a change inside the interval waits");
      ck(sens_cache_due(&c, 1000 + SENS_FLUSH_MIN_S),
         "...and is written once the interval is up");

      /* A change every second must NOT keep pushing the deadline out. */
      for (long t = 1001; t < 1000 + SENS_FLUSH_MIN_S; t++)
         sens_cache_touch(&c);
      ck(sens_cache_due(&c, 1000 + SENS_FLUSH_MIN_S),
         "constant changes do not postpone the write");

      /* A FAILED save must stay dirty, or the cache is silently dropped:
       * main.c only calls sens_cache_done when sess_save() returned 0. */
      sens_cache_done(&c, 2000);
      sens_cache_touch(&c);
      ck(sens_cache_due(&c, 2000 + SENS_FLUSH_MIN_S), "still dirty after a "
                                                      "failed save");
   }

   printf("== minting a newly bonded sensor ==\n");
   {
      struct sens_obs o;
      struct sens_effect e;

      o = fresh();
      sens_link_eval(&o, 100000, &e);
      ck(e.mint, "an unclaimed bonded CGM with a reading is registered");

      /* ACTIVATION IS AN EPOCH. The session clock counts UP from it, so the
       * start instant is now minus elapsed. Storing the elapsed value wrote a
       * duration into a field documented as a timestamp -- in a file that is
       * never rewritten, so it is never corrected. */
      ck(e.activation == 100000 - 3600,
         "activation is when the session started, not how long it has run");

      /* HAVE_READING, not just bonded. Bonding happens at AuthStatus, several
       * round trips before the first glucose, while session_seconds still
       * reads 0 -- minting then wrote "started now" for a sensor that may
       * have been worn for days. */
      o                 = fresh();
      o.have_reading    = 0;
      o.session_seconds = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.mint, "a bonded sensor with no reading yet is NOT minted");

      o        = fresh();
      o.bonded = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.mint, "an unbonded link is not minted");

      o         = fresh();
      o.has_mac = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.mint, "a link with no address is not minted");

      o        = fresh();
      o.is_cgm = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.mint, "a meter link is never minted as a sensor");

      o         = fresh();
      o.claimed = 1;
      sens_link_eval(&o, 100000, &e);
      ck(!e.mint, "an address a slot already claims is not minted twice");
   }

   printf("== completing an already-registered sensor's provenance ==\n");
   {
      /* A CGM is registered BARE when the user commits to pairing it. The DIS
       * strings land seconds later and the activation is only knowable once a
       * reading anchors the clock; this pass writes each the moment it
       * becomes true. */
      struct sens_obs o;
      struct sens_effect e;

      o            = fresh();
      o.claimed    = 1;
      o.registered = 1;
      o.row_bare   = 1;
      o.row_no_act = 1;
      o.have_dis   = 1;
      sens_link_eval(&o, 100000, &e);
      ck(e.complete_mfw, "a bare row gets its model and firmware");
      ck(e.complete_act, "...and its activation");
      ck(e.activation == 100000 - 3600, "...as an epoch, again");
      ck(!e.mint, "a completed row is never re-minted");

      /* BOTH DIS strings or neither. They are separate serialized GATT ops
       * and the sensor commonly closes the cycle before all of them land;
       * "model present, firmware empty" would append a correction row per
       * tick until the firmware showed up. */
      o.have_dis = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.complete_mfw, "half-arrived DIS strings are not written");
      ck(e.complete_act, "...but the activation still is");

      /* Same reading rule as the mint: no reading, no activation. */
      o                 = fresh();
      o.claimed         = 1;
      o.registered      = 1;
      o.row_no_act      = 1;
      o.have_reading    = 0;
      o.session_seconds = 0;
      sens_link_eval(&o, 100000, &e);
      ck(!e.complete_act, "activation waits for a reading to anchor the clock");

      /* Nothing missing, nothing written -- the file is append-only. */
      o            = fresh();
      o.claimed    = 1;
      o.registered = 1;
      o.have_dis   = 1;
      sens_link_eval(&o, 100000, &e);
      ck(!e.complete_mfw && !e.complete_act,
         "a complete row is left alone (the file only ever grows)");

      /* Claimed but with no provenance row found: nothing to complete. */
      o            = fresh();
      o.claimed    = 1;
      o.registered = 0;
      o.row_bare   = 1;
      o.have_dis   = 1;
      sens_link_eval(&o, 100000, &e);
      ck(!e.complete_mfw, "no row, nothing to complete");
   }

   printf("== which sensor stamps a reading that carries no source ==\n");
   {
      /* PREFER THE PRIMARY AND STOP AT IT. Without that this walked to the
       * end and kept whichever bonded CGM sat highest in the slot table, so a
       * second sensor's readings were stamped with the first's id -- and
       * per-source dedup then silently discarded samples that collided
       * within 150 s. */
      struct sens_slot_obs s[4] = {0};
      ck(sens_primary_pick(s, 0) == -1, "no slots: leave the stamp alone");

      s[0].id = 11, s[0].is_cgm = 1, s[0].live = 1;
      s[1].id = 22, s[1].is_cgm = 1, s[1].live = 1;
      ck(sens_primary_pick(s, 2) == 11, "with no primary marked, the first "
                                        "live CGM stamps");

      /* A third live CGM AFTER the primary: the walk must stop at the
       * primary, not run to the end of the table. */
      s[2].id = 33, s[2].is_cgm = 1, s[2].live = 1;
      ck(sens_primary_pick(s, 3) == 11, "the FIRST live CGM stamps, not the "
                                        "last one in the table");

      s[1].primary = 1;
      ck(sens_primary_pick(s, 3) == 22, "the primary wins over a later live "
                                        "CGM too");
      ck(sens_primary_pick(s, 2) == 22, "the primary wins even when it is not "
                                        "first");

      /* ...and a primary that is NOT connected must not win: the stamp names
       * the sensor a reading came from. */
      s[1].live = 0;
      ck(sens_primary_pick(s, 2) == 11, "an offline primary does not stamp");

      /* Two primaries is a corrupt registry, but the stamp still has to be
       * DETERMINISTIC: the same reading must not land under a different
       * sensor id from one tick to the next, in a log that is never
       * rewritten. First primary wins. */
      s[1].live    = 1;
      s[2].primary = 1;
      ck(sens_primary_pick(s, 3) == 22,
         "with two primaries marked, the first one wins every time");

      /* Retired and non-CGM slots have no live session to reconcile. */
      s[1].live = 1;
      s[1].old  = 1;
      ck(sens_primary_pick(s, 2) == 11, "a retired slot does not stamp");
      s[1].old    = 0;
      s[1].is_cgm = 0;
      ck(sens_primary_pick(s, 2) == 11, "a meter does not stamp a CGM "
                                        "reading");

      /* NEVER 0. Zero means "pre-registry legacy" in a log that is never
       * rewritten, so an empty answer has to be -1. */
      memset(s, 0, sizeof s);
      s[0].is_cgm = 1, s[0].live = 1, s[0].id = 0;
      ck(sens_primary_pick(s, 1) == -1, "id 0 is not a source, it is the "
                                        "legacy marker");
      ck(sens_primary_pick(0, 3) == -1, "no observation, no stamp");
   }

   printf("\n%d checks, %d failed\n", checks, fails);
   printf("%s\n", fails ? "SENS TESTS FAILED" : "ALL SENS TESTS PASSED");
   return fails ? 1 : 0;
}
