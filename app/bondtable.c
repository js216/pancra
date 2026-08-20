// SPDX-License-Identifier: GPL-3.0
// bondtable.c --- what the OS last said about each address's bond
// Copyright 2026 Jakob Kastelic

/* SPLIT OUT OF dexble.c SO IT CAN BE TESTED.
 *
 * This table is written from a binder thread (the OS's bond-state broadcast)
 * and read by the main loop while it renders. It lived in the middle of the
 * JNI bridge, which is one translation unit holding every native method and a
 * JavaVM -- so linking it into a host suite was not practical, and the
 * synchronisation below was argued in a comment rather than demonstrated.
 *
 * Here it needs nothing but a lock and string comparison, so bondtabletest
 * runs a reader and a writer at it under ThreadSanitizer and the argument is
 * checked. */
#include "bondtable.h"
#include "thread.h"

#include <stdio.h>  /* snprintf */
#include <string.h> /* strcmp */

/* The last bond state the receiver reported, per address.
 *
 * A tiny fixed table rather than a field on the sensor slot: the bond is a
 * property of the ADDRESS as the OS sees it, the broadcast arrives on a binder
 * thread with nothing but a MAC, and a device can be bonding before it has a
 * slot at all (the meter path connects to a bond it is still forming). Sized
 * for LINK_MAX devices plus slack; past that it is the LAST slot that is
 * reused, over and over -- not the oldest, which is what this said before:
 * entries 0..BOND_SLOTS-2 are never evicted once written. That is tolerable
 * only because the size is chosen so overflow needs more devices than the app
 * has links for, and because only the newest transition per device is ever
 * interesting.
 */
#define BOND_SLOTS 12

static struct {
   char mac[20];
   int state;
} g_bond[BOND_SLOTS];

static int g_bond_n;

/* ITS OWN LOCK, AND THE TABLE IS NEVER READ WITHOUT IT.
 *
 * This used to be unsynchronised, with a comment arguing that the worst case
 * was "one frame of a stale label": the state is one int, the writer is a
 * binder thread, the reader is the main loop, and a torn int is a wrong
 * status string for a moment.
 *
 * THAT ARGUMENT WAS ABOUT THE WRONG FIELD, and there are two races here
 * rather than the one it dismissed.
 *
 * THE COUNT. Insertion read `g_bond_n < BOND_SLOTS`, incremented it, and only
 * then wrote the MAC -- so between those two statements the table advertised
 * a slot that was still being built, and a concurrent dexble_bond_state
 * walked straight into it and handed that buffer to strcmp(). The slot in
 * question has never held a device (the count only grows and the slots fill
 * in order), so what the reader is walking is static zeroing turning into an
 * address one byte at a time.
 *
 * THE SLOT ITSELF, which the count can say nothing about. Every write to an
 * ALREADY-COUNTED entry -- an update in place, and the recycle of the last
 * slot once the table is full -- rewrites `mac` and then `state` underneath a
 * reader that is walking exactly those bytes. That reader can compare against
 * an address that is half the outgoing device's and half the incoming one's,
 * or match the incoming MAC and read the outgoing device's state, and report
 * one device's bond as another's. Neither race is a torn int, and neither is
 * cosmetic.
 *
 * A LEAF: taken innermost, held for a handful of instructions, and never
 * across a call into another module -- nothing runs under it but strcmp and
 * one snprintf, so no second lock can be taken while this one is held. It IS
 * taken with the DRIVER's lock held, which is a different thing and is the
 * documented direction rather than a violation: a repaint driven from inside
 * a driver operation reaches the lookup with driver_lk still down
 * (driver_kick -> drv_connect -> set_status -> update_screen -> draw ->
 * build_model -> fill_sensor -> dexble_bond_state, all on the main thread
 * from on_timer's watchdog). app/test/lockorder.py ranks this lock below the
 * driver's for that reason and reports the pair. The cost the old comment
 * worried about -- a BLE callback waiting on the DRIVER mutex for a cosmetic
 * value -- is still not what this is: the only thing a callback can wait on
 * here is another lookup. */
static struct mutex bond_lk = MUTEX_INIT;

int dexble_bond_state(const char *mac)
{
   if (!mac || !mac[0])
      return 0;
   /* A COPY, returned after the lock is released. Handing back a pointer into
    * the table, or reading it again after unlocking, would put the caller
    * back where this started. */
   int state = 0;
   mutex_lock(&bond_lk);
   for (int i = 0; i < g_bond_n; i++) {
      if (strcmp(g_bond[i].mac, mac) == 0) {
         state = g_bond[i].state;
         break; /* not `return`: one exit, so the unlock cannot be skipped */
      }
   }
   mutex_unlock(&bond_lk);
   return state;
}

void bond_state_set(const char *mac, int state)
{
   mutex_lock(&bond_lk);
   int found = 0;
   for (int i = 0; i < g_bond_n; i++) {
      if (strcmp(g_bond[i].mac, mac) == 0) {
         g_bond[i].state = state;
         found           = 1;
         break;
      }
   }
   if (!found) {
      int i = (g_bond_n < BOND_SLOTS) ? g_bond_n : (BOND_SLOTS - 1);
      (void)snprintf(g_bond[i].mac, sizeof g_bond[i].mac, "%s", mac);
      g_bond[i].state = state;
      /* THE COUNT LAST. Inside the lock this is belt and braces; it is
       * written this way anyway because the order is the invariant -- a slot
       * enters the count only once it fully describes a device.
       *
       * Note what that does NOT buy. When the table is full the count does
       * not move at all: `i` is BOND_SLOTS - 1, an entry every reader can
       * already see, and it is rewritten in place. Ordering protects the
       * GROWING case; the LOCK is what protects the recycled one, and there
       * is no publication trick that would make an in-place overwrite safe
       * to read unlocked. */
      if (g_bond_n < BOND_SLOTS)
         g_bond_n = i + 1;
   }
   mutex_unlock(&bond_lk);
}
