// SPDX-License-Identifier: GPL-3.0
// bondtabletest.c --- the bond table, read and written at the same time
// Copyright 2026 Jakob Kastelic
//
/* THE RACE THIS EXISTS FOR IS NOT A TORN INT.
 *
 * The table was unsynchronised, and the comment defending that said the worst
 * case was one frame of a stale status label: the state is a single int, and
 * a torn int is a wrong label for a moment.
 *
 * The argument was about the wrong field. Insertion read the count,
 * INCREMENTED IT, and only then wrote the address -- so between those two
 * statements the table advertised a slot whose `mac` was whatever the array
 * held, and a concurrent lookup walked into it and handed that buffer to
 * strcmp(). On a never-used slot that is static zeroing (an empty string,
 * which merely fails to match); on a RECYCLED slot it is a stale address that
 * can match the wrong device and report its bond state as this one's.
 *
 * None of that is observable from a single thread, and until this file the
 * table could not be reached from a host suite at all -- it lived inside the
 * JNI bridge. So the checks below are deliberately two-threaded, and the one
 * that matters is run under ThreadSanitizer by `make apptsan`: an unsynchro-
 * nised version of this table is a REPORTED race there, not a rare wrong
 * label somebody has to notice on a phone. */

#include "bondtable.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* Enough addresses to fill the table and force the recycling path, which is
 * where a lockless reader is exposed to an in-place rewrite. */
#define NMAC 20
static char macs[NMAC][20];

static void make_macs(void)
{
   for (int i = 0; i < NMAC; i++)
      snprintf(macs[i], sizeof macs[i], "AA:BB:CC:DD:EE:%02X", i);
}

/* ATOMIC, NOT `volatile`. volatile orders nothing between threads -- it tells
 * the compiler not to cache the load and says nothing to the hardware or to a
 * race detector -- so a volatile flag written by main and read by the worker
 * is itself a data race, and ThreadSanitizer reported this test's own flag
 * before it reported anything about the table. A test whose scaffolding races
 * cannot make a claim about whether the code under it does. */
static atomic_int stop;

/* THE STATE ENCODES WHICH ADDRESS IT WAS RECORDED FOR, and that is what makes
 * the assertions below able to fail at all.
 *
 * The obvious fixture writes 1, 2 or 3 to every address and has the reader
 * check that what comes back is in 1..3, or 0 for an address the table has not
 * been told about. That check CANNOT FAIL. `state` is one aligned int, so no
 * interleaving of this code produces a value outside the set that was written
 * -- and the failure the lock exists for is not an out-of-range value, it is
 * ONE DEVICE'S BOND REPORTED AS ANOTHER'S: a reader that matches the incoming
 * MAC of a slot being recycled and then loads the outgoing device's state.
 * Every address holding the same handful of values makes that failure
 * invisible by construction. Measured: an entirely unsynchronised version of
 * bondtable.c passed the in-range form of this test 200 times out of 200.
 *
 * So the state written for address i is unique to i (`round` only picks
 * between two values, both of which still name i). A reader that answers with
 * a different address's state now answers with a number that address never
 * had, and says so without a sanitizer. Measured: the unsynchronised version
 * fails this form 100 times out of 100. */
static int st(int i, int round)
{
   return 1 + i + NMAC * (round & 1);
}

static void *writer(void *a)
{
   (void)a;
   for (int round = 0; !atomic_load(&stop) && round < 4000; round++)
      for (int i = 0; i < NMAC; i++)
         bond_state_set(macs[i], st(i, round));
   return 0;
}

/* THE READER IS NOT ALLOWED TO SEE A STATE THAT WAS NEVER SET FOR THIS
 * ADDRESS. The only two values ever written for address i are st(i, 0) and
 * st(i, 1); a slot the reader has not been told about reads 0. Anything else
 * means it read a half-written entry, or matched one device's address and
 * loaded another's state -- which is the failure in its observable form, as
 * opposed to the sanitizer's. */
static void *reader(void *a)
{
   int *bad = a;
   for (int round = 0; !atomic_load(&stop) && round < 4000; round++) {
      for (int i = 0; i < NMAC; i++) {
         int s = dexble_bond_state(macs[i]);
         if (s != 0 && s != st(i, 0) && s != st(i, 1))
            (*bad)++;
      }
   }
   return 0;
}

int main(void)
{
   make_macs();

   printf("== one thread: what the table says it does ==\n");
   {
      bond_state_set("AA:BB:CC:DD:EE:01", 2);
      ck(dexble_bond_state("AA:BB:CC:DD:EE:01") == 2,
         "a recorded address reads back its state");
      bond_state_set("AA:BB:CC:DD:EE:01", 3);
      ck(dexble_bond_state("AA:BB:CC:DD:EE:01") == 3,
         "...and a later transition replaces it, in place");
      ck(dexble_bond_state("AA:BB:CC:DD:EE:99") == 0,
         "an address never reported reads 0");
      ck(dexble_bond_state("") == 0, "an empty address is not a lookup");
      ck(dexble_bond_state(0) == 0, "and neither is a null one");
   }

   printf("== the table full, and past full ==\n");
   {
      /* Twenty addresses into twelve slots. The point is not that all twenty
       * survive -- they cannot -- but that every answer is one that was
       * actually recorded for THAT address, never another's. Which is a claim
       * only because st() gives each address its own value: with a single
       * shared state this loop passes even against a lookup rigged to return
       * the neighbouring slot's state. */
      for (int i = 0; i < NMAC; i++)
         bond_state_set(macs[i], st(i, 0));
      int wrong = 0;
      for (int i = 0; i < NMAC; i++) {
         int s = dexble_bond_state(macs[i]);
         if (s != 0 && s != st(i, 0))
            wrong++;
      }
      ck(wrong == 0, "past capacity, no address reports a state it never had");
   }

   printf("== a reader and a writer, at the same time ==\n");
   {
      int bad = 0;
      pthread_t w, r;
      atomic_store(&stop, 0);
      pthread_create(&w, 0, writer, 0);
      pthread_create(&r, 0, reader, &bad);
      pthread_join(w, 0);
      atomic_store(&stop, 1);
      pthread_join(r, 0);
      /* BOTH HALVES ARE LOAD-BEARING, and they fail for different reasons.
       *
       * The assertion below only fires when the reader actually lands in the
       * window, which is why the state has to name its address: rigged as an
       * in-range check it never fired at all, and that is exactly how the
       * original "a torn int costs one frame of a stale label" comment
       * survived. Naming the address is what turns "narrow window" from
       * "unobservable" into "observed every run so far" -- but it is still a
       * window, and a future table with a shorter one could go quiet again.
       *
       * TSan does not depend on hitting the window: it reports the unordered
       * pair of accesses whether or not the reader ever sees a wrong value.
       * `make apptsan` runs this suite for that reason. */
      ck(bad == 0, "a concurrent reader never sees another address's state");
   }

   if (fails == 0)
      printf("\nALL BONDTABLE TESTS PASSED\n");
   else
      printf("\n%d BONDTABLE TEST(S) FAILED\n", fails);
   return fails != 0;
}
