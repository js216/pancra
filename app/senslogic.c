// SPDX-License-Identifier: GPL-3.0
// senslogic.c --- Sensor/session reconciliation decisions (see senslogic.h)
// Copyright 2026 Jakob Kastelic

#include "senslogic.h"
#include <stdint.h>

uint32_t sens_project_clock(uint32_t last_clock, long recv_mono, long now_mono)
{
   if (!last_clock)
      return 0;
   long dt = now_mono - recv_mono;
   /* NONNEGATIVE FIRST. On a monotonic pair this cannot happen in a healthy
    * process, which is exactly why it must be handled: the only ways to reach
    * it are a stamp that was never written and a read that raced the write,
    * and unhandled both produce a projection that runs away rather than one
    * that stands still. */
   if (dt < 0)
      dt = 0;
   if (dt > SENS_PROJECT_MAX_S)
      dt = SENS_PROJECT_MAX_S;
   /* SATURATE. The sum is uint32 on the wire and in dex_session, and a clock
    * near the top of the range plus a day would wrap to a few seconds -- a
    * session that has run for an eternity reported as one that just began,
    * which is the same class of lie in the opposite direction. */
   uint32_t room = 0xffffffffU - last_clock;
   if ((unsigned long)dt > (unsigned long)room)
      return 0xffffffffU;
   return last_clock + (uint32_t)dt;
}

void sens_cache_touch(struct sens_cache *c)
{
   if (c)
      c->dirty = 1;
}

int sens_cache_due(const struct sens_cache *c, long now)
{
   if (!c || !c->dirty)
      return 0;
   return now - c->saved >= SENS_FLUSH_MIN_S;
}

void sens_cache_done(struct sens_cache *c, long now)
{
   if (!c)
      return;
   c->saved = now;
   c->dirty = 0;
}

void sens_link_eval(const struct sens_obs *o, long now, struct sens_effect *e)
{
   if (!e)
      return;
   e->mint         = 0;
   e->complete_mfw = 0;
   e->complete_act = 0;
   e->activation   = 0;
   if (!o || !o->is_cgm || !o->has_mac || !o->bonded)
      return;

   /* The epoch the session STARTED, not its elapsed length: the session clock
    * counts UP from activation, so the start instant is now minus elapsed.
    * Feeding the elapsed value straight in wrote a duration into a field
    * documented as a timestamp, in a file that is never rewritten. */
   long activation = now - o->session_seconds;

   if (!o->claimed) {
      /* HAVE_READING, not just bonded. Bonding happens at AuthStatus, several
       * round trips BEFORE the first glucose, while session_seconds still
       * reads 0 -- minting then wrote "session started now" for a sensor that
       * may have been worn for days, and activation is not part of the id
       * key, so it is never corrected. */
      if (!o->have_reading)
         return;
      e->mint       = 1;
      e->activation = activation;
      return;
   }

   /* Already registered: complete what has since become knowable. A CGM is
    * registered BARE the moment the user commits to pairing it, so its row
    * starts with no model, no firmware and no activation. */
   if (!o->registered)
      return;

   /* DIS strings only when BOTH have landed. They are separate serialized
    * GATT ops and the sensor commonly closes the cycle before all of them
    * arrive; writing "model present, firmware still empty" would append a
    * correction row per tick until the firmware showed up. */
   if (o->row_bare && o->have_dis)
      e->complete_mfw = 1;

   /* Activation only once a reading has anchored the session clock -- same
    * reason as the mint above, and this field is completed exactly once. */
   if (o->row_no_act && o->have_reading) {
      e->complete_act = 1;
      e->activation   = activation;
   }
}

int sens_primary_pick(const struct sens_slot_obs *slots, int n)
{
   int pick = -1;
   if (!slots)
      return -1;
   for (int i = 0; i < n; i++) {
      const struct sens_slot_obs *s = &slots[i];
      if (s->old || !s->is_cgm || !s->live || s->id <= 0)
         continue;
      if (pick < 0 || s->primary)
         pick = s->id;
      if (s->primary)
         break;
   }
   return pick;
}
