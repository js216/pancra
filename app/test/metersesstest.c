// SPDX-License-Identifier: GPL-3.0
// metersesstest.c --- the meter session under real callback/watchdog overlap
// Copyright 2026 Jakob Kastelic

/* WHAT THIS PINS.
 *
 * The meter's protocol session -- who owns the one exchange, on which link,
 * since when -- is touched by three threads at once: BLE connect/disconnect
 * callbacks on binder threads, the activity's 1 Hz watchdog on the main
 * thread, and the same watchdog again from the foreground service. It used to
 * be five plain variables in meter.c, and the check that stopped a second
 * meter from seizing the exchange was
 *
 *     if (g_meter_busy && g_meter_link != link) refuse;
 *     g_meter_link = link; g_meter_busy = 1;
 *
 * -- a test and a set with a window between them. Two meters switched on
 * together by the same person connect milliseconds apart, and losing that
 * race seeds otble's single protocol state twice: phase reset mid-walk,
 * last_index replaced, and the second meter's fingersticks written into an
 * append-only log under the first meter's id.
 *
 * The properties asserted here are the ones that a comment cannot enforce:
 *
 *   1. EXACTLY ONE OWNER. Under many binder threads claiming concurrently,
 *      the number of successful claims that saw the session free equals the
 *      number of ends -- no two links are ever both mid-exchange.
 *   2. A SNAPSHOT IS COHERENT. msess_get taken while claims and ends run flat
 *      out never shows busy with no link, or a link with no meter behind it.
 *      This is the one that fails if the fields are read one at a time.
 *   3. A STALE DISCONNECT CANNOT CANCEL A LIVE SYNC. msess_end for a link
 *      that no longer owns the session leaves it alone.
 *   4. THE WATCHDOG SEES ONE INSTANT. Its snapshot of the session and of the
 *      teardown-wait table are each taken whole, so meter_tick_eval never
 *      judges an exchange by another exchange's start stamp.
 *
 * Real threads, deliberately: a single-threaded test of a lock asserts
 * nothing about the lock. Built and run by `make metersesstest`.
 */
#include "metersess.h"
#include "meterlogic.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- 1 and 2: many claimants, one owner ------------------------------- */

#define NCLAIM  6
#define NROUNDS 20000

/* How many claims were granted, and how many exchanges ended. Atomic because
 * the fixtures race on purpose; the STATE under test is not. */
static atomic_int n_claim, n_end;
/* The invariant breaches, counted rather than asserted on the spot so a
 * failing run prints one line instead of a hundred thousand. */
static atomic_int bad_two_owners, bad_snapshot;

/* Who the test believes owns the session, written only by the thread whose
 * claim was granted. A second granted claim while this is set is the race
 * this whole module exists to prevent. */
static atomic_int owner = -1;

static void *claimer(void *arg)
{
   int link = (int)(long)arg;
   char mac[MSESS_MAC_MAX];
   snprintf(mac, sizeof mac, "AA:BB:CC:00:00:%02d", link);
   for (int i = 0; i < NROUNDS; i++) {
      /* src is derived from the link so a snapshot can check the two agree:
       * a torn read would pair one meter's link with another's id. */
      if (!msess_claim(link, 100 + link, mac, 1000 + i))
         continue;
      atomic_fetch_add(&n_claim, 1);
      int prev = atomic_exchange(&owner, link);
      if (prev != -1 && prev != link)
         atomic_fetch_add(&bad_two_owners, 1);
      /* ...the exchange runs here...  */
      atomic_store(&owner, -1);
      if (msess_end(link, 0))
         atomic_fetch_add(&n_end, 1);
   }
   return 0;
}

/* The watchdog's side: nothing but snapshots, as fast as it can take them. */
static void *snapper(void *arg)
{
   (void)arg;
   for (int i = 0; i < NROUNDS; i++) {
      struct msess s;
      msess_get(&s);
      /* Busy with no link, or a link with no meter, is a torn read -- and so
       * is an id that is not the one THAT link claims with, which is the pair
       * that a field-at-a-time read gets wrong. */
      if (s.busy && (s.link < 0 || s.link >= MSESS_LINKS_MAX || s.src <= 0 ||
                     s.src != 100 + s.link))
         atomic_fetch_add(&bad_snapshot, 1);
      /* The watchdog also reads the wait table whole. */
      long idle[MSESS_LINKS_MAX];
      msess_idle_copy(idle, MSESS_LINKS_MAX);
   }
   return 0;
}

static void race(void)
{
   /* <pthread.h> IS included above. glibc defines pthread_t in
    * <bits/pthreadtypes.h> with no IWYU pragma pointing back at its public
    * header, so include-cleaner asks for a private glibc header by name --
    * including which would be the actual defect. Silenced on this one line
    * rather than for the file, as threadtest.c does. */
   /* NOLINTNEXTLINE(misc-include-cleaner) */
   pthread_t th[NCLAIM + 2];
   msess_reset();
   for (int i = 0; i < NCLAIM; i++)
      pthread_create(&th[i], 0, claimer, (void *)(long)i);
   pthread_create(&th[NCLAIM], 0, snapper, 0);
   pthread_create(&th[NCLAIM + 1], 0, snapper, 0);
   for (int i = 0; i < NCLAIM + 2; i++)
      pthread_join(th[i], 0);

   ck(atomic_load(&bad_two_owners) == 0,
      "two links are never mid-exchange at once");
   ck(atomic_load(&bad_snapshot) == 0,
      "a snapshot never shows a link and an id from different exchanges");
   /* Every granted claim ends the exchange it took. Claims can be granted to
    * the SAME link twice in a row (a re-connect), which is why this is not a
    * strict equality against NCLAIM * NROUNDS. */
   ck(atomic_load(&n_claim) == atomic_load(&n_end),
      "every claim that was granted is the one that ended it");
   ck(atomic_load(&n_claim) > 0, "...and the run really did claim something");
}

/* ---- 3: a stale disconnect ------------------------------------------- */

static void stale(void)
{
   msess_reset();
   ck(msess_claim(2, 42, "AA:BB", 500) == 1, "a free session is claimed");
   ck(msess_claim(3, 43, "CC:DD", 501) == 0,
      "...and a second meter is refused while it runs");
   /* The refused claim wrote nothing. */
   struct msess s;
   msess_get(&s);
   ck(s.link == 2 && s.src == 42 && s.start == 500,
      "...leaving the running exchange exactly as it was");
   ck(strcmp(s.mac, "AA:BB") == 0, "...including whose meter it is");

   /* Link 3's disconnect arrives late. It never owned the session and must
    * not clear it. */
   ck(msess_end(3, 0) == 0, "a disconnect for another link is not the owner");
   msess_get(&s);
   ck(s.busy == 1 && s.link == 2, "...and the live exchange survives it");

   /* The owner's does. */
   ck(msess_end(2, 900) == 1, "the owner's disconnect ends the exchange");
   msess_get(&s);
   ck(s.busy == 0 && s.link == -1, "...and the session is free again");
   /* ...and the same step marked the link as waiting for its teardown. */
   long idle[MSESS_LINKS_MAX];
   msess_idle_copy(idle, MSESS_LINKS_MAX);
   ck(idle[2] == 900, "ending an exchange stamps its teardown wait");
   /* The meter stays bound: it is still the meter the app speaks for, and
    * ot_drv_done runs after the disconnect to save its record index. */
   ck(msess_src() == 42, "...while the meter it was is still the bound one");
}

/* ---- 4: what the watchdog is handed ---------------------------------- */

static void watchdog_view(void)
{
   msess_reset();
   struct msess s;
   long idle[MSESS_LINKS_MAX];
   struct meter_tick mt;

   /* An exchange that has run too long is dropped, and the drop hands back
    * the link the caller has to close. */
   ck(msess_claim(1, 7, "EE:FF", 100) == 1, "an exchange starts at t=100");
   msess_get(&s);
   msess_idle_copy(idle, MSESS_LINKS_MAX);
   meter_tick_eval(s.busy, s.start, idle, MSESS_LINKS_MAX,
                   100 + METER_SYNC_MAX_S + 1, &mt);
   ck(mt.drop_sync == 1, "...and is judged wedged once it overruns");
   ck(msess_drop() == 1, "the drop names the link to close");
   ck(msess_busy() == 0, "...and the session is free");
   ck(msess_drop() == -1, "a second drop has no link to name");

   /* A link waiting for a teardown that never comes is released -- but only
    * while nothing is mid-exchange, which is the table's whole subtlety. */
   msess_reset();
   msess_idle_set(4, 200);
   msess_get(&s);
   msess_idle_copy(idle, MSESS_LINKS_MAX);
   meter_tick_eval(s.busy, s.start, idle, MSESS_LINKS_MAX,
                   200 + METER_TEARDOWN_MAX_S + 1, &mt);
   ck(mt.nrelease == 1 && mt.release[4],
      "a link stranded waiting for its disconnect is released");
   msess_idle_set(4, 0);
   msess_idle_copy(idle, MSESS_LINKS_MAX);
   ck(idle[4] == 0, "...and releasing it clears the wait");

   /* Out-of-range links are refused rather than written past the table. */
   msess_idle_set(-1, 5);
   msess_idle_set(MSESS_LINKS_MAX, 5);
   ck(msess_claim(MSESS_LINKS_MAX, 1, "X", 0) == 0,
      "a link outside the table cannot claim the session");
   ck(msess_claim(0, 0, "X", 0) == 0, "...nor can a meter with no registry id");
}

int main(void)
{
   printf("metersesstest: the meter session under overlap\n");
   stale();
   watchdog_view();
   race();
   printf("%s\n", all ? "metersesstest: one exchange, one owner, one instant"
                      : "metersesstest: FAILED");
   return all ? 0 : 1;
}
