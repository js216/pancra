// SPDX-License-Identifier: GPL-3.0
// meterlogic.c --- The meter runtime's timing decisions (see meterlogic.h)
// Copyright 2026 Jakob Kastelic

#include "meterlogic.h"
#include "civil.h"

void meter_tick_eval(int busy, long start, const long *idle_since, int nlinks,
                     long now, struct meter_tick *out)
{
   if (!out)
      return;
   out->drop_sync = 0;
   out->nrelease  = 0;
   for (int l = 0; l < METER_LINKS_MAX; l++)
      out->release[l] = 0;
   if (nlinks > METER_LINKS_MAX)
      nlinks = METER_LINKS_MAX;

   /* STRICTLY GREATER. A sync that has run for exactly the budget has not
    * overrun it, and a boundary that fires one tick early would abandon an
    * exchange that was about to finish. */
   if (busy && now - start > METER_SYNC_MAX_S)
      out->drop_sync = 1;

   /* Not while a sync is running: a busy runtime owns its links, and
    * releasing one under it would tear down the very exchange the rule above
    * may be about to end properly. */
   if (busy || !idle_since)
      return;
   for (int l = 0; l < nlinks; l++) {
      if (!idle_since[l]) /* 0 = not waiting for a teardown */
         continue;
      if (now - idle_since[l] < METER_TEARDOWN_MAX_S)
         continue;
      out->release[l] = 1;
      out->nrelease++;
   }
}

void meter_seq_reset(struct meter_seq *sq)
{
   if (!sq)
      return;
   sq->prev_t    = 0;
   sq->have_prev = 0;
   sq->fell_back = 0;
}

struct meter_stamp meter_stamp_step(struct meter_seq *sq, long naive,
                                    long import_t, zone_off_fn zone, void *ctx)
{
   struct civil_res r   = civil_resolve(naive, zone, ctx);
   struct meter_stamp s = {r.t, r.off, r.t, 0, r.fix == CIVIL_NONEXISTENT};

   if (r.fix != CIVIL_AMBIGUOUS) {
      /* THE ORDINARY PATH, and it must stay byte-for-byte this: one
       * civil time, one instant, no state consulted. A disambiguator that
       * moves a record outside the repeated hour has broken every timestamp
       * in the log to fix two hours a year. The run state is CLEARED here
       * and only here -- see meterlogic.h. */
      if (sq)
         sq->fell_back = 0;
   } else {
      /* Two instants an hour apart. r.t is the earlier (first pass of the
       * repeated hour), r.t_alt the later (second pass). */
      long early    = r.t;
      long late     = r.t_alt;
      long eoff     = r.off;
      long loff     = r.off_alt;
      int decided   = 0;
      int pick_late = 0;

      if (sq && sq->fell_back) {
         /* The clock has already been seen to go back inside this run: every
          * later record in it is on the far side of the transition. */
         pick_late = 1;
         decided   = 1;
      } else if (sq && sq->have_prev && early <= sq->prev_t) {
         /* THE FALL-BACK, OBSERVED. Records arrive in the order they were
          * taken, so an instant that does not advance is impossible -- the
          * earlier candidate is ruled out by the record before it, not
          * guessed away. Equality matters as much as inequality: two
          * fingersticks with the SAME clock reading is the collision that
          * made the second one vanish into the log's dedup. */
         pick_late     = 1;
         decided       = 1;
         sq->fell_back = 1;
      } else if (import_t > 0 && late > import_t) {
         /* A reading cannot have been taken after it was imported, so the
          * later candidate is not one. Decisive, and it is the only evidence
          * available for the FIRST record of a walk. */
         pick_late = 0;
         decided   = 1;
      }

      if (pick_late) {
         s.t     = late;
         s.off   = loff;
         s.t_alt = early;
      } else {
         s.t     = early;
         s.off   = eoff;
         s.t_alt = late;
      }
      /* NOT DECIDED IS NOT THE SAME AS DECIDED. civil.h's rule supplied the
       * stamp so the import still completes, but the flag and the rejected
       * instant travel with it: dropping them here is what turned a guess
       * into a fact. */
      s.ambiguous = !decided;
   }

   if (sq) {
      sq->prev_t    = s.t;
      sq->have_prev = 1;
   }
   return s;
}
