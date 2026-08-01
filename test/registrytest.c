// SPDX-License-Identifier: GPL-3.0
// registrytest.c --- Host tests for the sensor provenance registry
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for sensors.c, which had none.
 *
 * This is the most consequential data structure in the app. An id names one
 * physical device forever, and readings.csv cites those ids in rows that are
 * never rewritten -- so if an id is ever reused, one sensor's history is
 * permanently merged into another's, and there is no way to tell them apart
 * after the fact. sensors.c's own comments call that "the one failure this
 * whole design exists to make impossible".
 *
 * Everything below is an invariant those comments state. None of them was
 * checked by anything: sensors.c is LINKED into other test binaries, but only
 * sensor_kind / sensor_type_name are ever called, so mint, claim, forget,
 * rebind and the primary rule had zero coverage.
 *
 * Built and run by `make registrytest`.
 */
#include "sensors.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* Start from an empty registry backed by real files, because the durability
 * rules (append-only, reload, maxid) are half the point. */
static void fresh(void)
{
   (void)snprintf(g_sensors_path, sizeof g_sensors_path,
                  "tmp/uitest/rt-sensors.csv");
   (void)snprintf(g_slots_path, sizeof g_slots_path, "tmp/uitest/rt-slots.csv");
   unlink(g_sensors_path);
   unlink(g_slots_path);
   sensors_load();
}

int main(void)
{
   printf("== an id is stable for the same physical device ==\n");
   fresh();
   int a =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:01", "", "SW1", "1.0", 100);
   ck(a > 0, "the first mint yields a real id");
   int again =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:01", "", "SW1", "1.0", 999);
   ck(again == a, "re-minting the same device returns the SAME id");
   /* Activation is deliberately NOT part of the reuse key -- it is derived
    * from a live session clock that drifts between reads, so including it
    * would mint a fresh id on almost every reconcile and split one sensor's
    * history across many. Assert it from a THIRD mint with a third activation,
    * rather than re-testing the same expression as the line above. */
   int again3 =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:01", "", "SW1", "1.0", 55555);
   ck(again3 == a,
      "...even with a different activation (it is not in the key)");

   printf("== identity is (type, address); learned attributes are NOT ==\n");
   /* model/fw/serial are LEARNED ATTRIBUTES of a device, not its identity.
    * Keying on them split one physical device across ids the instant its DIS
    * was read AFTER its first reading, orphaning every reading logged between
    * -- so the same (type, address) must return the SAME id regardless of them.
    */
   int fw2 =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:01", "", "SW1", "1.1", 100);
   ck(fw2 == a, "a firmware change keeps the SAME id (it is not identity)");
   int mac2 =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:02", "", "SW1", "1.0", 100);
   ck(mac2 != a, "a different address is a different identity");
   int type2 =
       sensor_mint(SENSOR_G7, "AA:BB:CC:DD:EE:01", "", "SW1", "1.0", 100);
   ck(type2 != a, "a different type is a different identity");

   printf("== ids are NEVER reused, even across a reload ==\n");
   /* The failure mode: an id vanishing from the file makes maxid go backwards
    * and the next mint reissues an id readings.csv already cites. */
   {
      int highest = a;
      if (fw2 > highest)
         highest = fw2;
      if (mac2 > highest)
         highest = mac2;
      if (type2 > highest)
         highest = type2;
      sensors_load(); /* reload from disk */
      int next =
          sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:99", "", "SW9", "9.9", 100);
      ck(next > highest, "a mint after reload is above every existing id");
      ck(sensor_rec_by_id(a) != 0, "...and the original row survived the load");
   }

   printf("== ids keep climbing after the in-memory cache evicts ==\n");
   {
      /* THE case that matters, and the one a small-registry test cannot see.
       * g_srec holds at most MAX_SENSOR_RECS rows; past that, srec_push evicts.
       * If a new id were derived from the CACHE SIZE rather than from the
       * highest id ever issued, it would stop climbing the moment eviction
       * began and start reissuing ids that readings.csv already cites --
       * merging one sensor's history into another's, permanently.
       *
       * Verified by mutation: with ids taken from g_nsrec + 1 the whole rest
       * of this file still passed, because below the cache limit the two
       * schemes agree. */
      fresh();
      int seen_max = 0;
      int nmint    = MAX_SENSOR_RECS + 6;
      for (int i = 0; i < nmint; i++) {
         char mac[24];
         (void)snprintf(mac, sizeof mac, "5A:00:00:00:%02X:%02X", i / 256,
                        i % 256);
         int id = sensor_mint(SENSOR_STELO, mac, "", "M", "1", 100);
         if (id <= seen_max) {
            ck(0, "an id was reissued or went backwards after eviction");
            seen_max = -1;
            break;
         }
         seen_max = id;
      }
      ck(seen_max > MAX_SENSOR_RECS,
         "ids keep climbing past the cache size once eviction starts");
      ck(g_nsrec <= MAX_SENSOR_RECS, "...while the cache itself stays bounded");
   }

   printf("== an empty identity is refused ==\n");
   fresh();
   ck(sensor_mint(SENSOR_STELO, "", "", "SW1", "1.0", 100) < 0,
      "minting without an address is refused");
   ck(sensor_mint(SENSOR_STELO, 0, "", "SW1", "1.0", 100) < 0,
      "...and a NULL address does not crash");

   printf("== slots: claim, lookup, and the primary rule ==\n");
   fresh();
   int s1 = sensor_mint(SENSOR_STELO, "11:11:11:11:11:11", "", "M", "1", 100);
   int s2 = sensor_mint(SENSOR_G7, "22:22:22:22:22:22", "", "M", "1", 100);
   int i1 = sensor_claim_slot(s1, SENSOR_STELO, "11:11:11:11:11:11");
   int i2 = sensor_claim_slot(s2, SENSOR_G7, "22:22:22:22:22:22");
   ck(i1 == 0 && i2 == 1, "slots are claimed in order");
   ck(sensor_claim_slot(s1, SENSOR_STELO, "11:11:11:11:11:11") == i1,
      "re-claiming an id returns its existing slot, not a second one");
   ck(sensor_slot_by_mac("22:22:22:22:22:22") == i2, "lookup by address works");
   ck(sensor_slot_by_mac("99:99:99:99:99:99") < 0, "an unknown address is -1");
   ck(sensor_primary_slot() == 0, "the first CGM becomes primary");
   sensor_set_primary(i2);
   ck(sensor_primary_slot() == i2, "primary moves on request");
   ck(g_slot[i1].primary == 0, "...and only ONE slot is primary");
   ck(sensor_primary_id() == s2, "primary_id agrees with primary_slot");

   printf("== forget shifts the table without disturbing the survivor ==\n");
   /* The historical bug: anything keyed on a slot's POSITION silently
    * re-pointed at a different sensor after a forget. */
   sensor_forget_slot(i1);
   ck(g_nslot == 1, "one slot remains");
   ck(g_slot[0].id == s2, "the SURVIVOR is the one that was not forgotten");
   ck(sensor_slot_by_mac("11:11:11:11:11:11") < 0, "the forgotten one is gone");
   ck(sensor_rec_by_id(s1) != 0,
      "...but its PROVENANCE row remains, so old readings still resolve");

   printf("== rebind moves a slot to a new id, keeping its preferences ==\n");
   /* Completing the DIS strings no longer mints a new id (identity is the MAC),
    * so the bare->DIS re-mint that USED to drive a rebind is gone -- that was
    * the orphan bug. The rebind MECHANISM is still live in production (a slot
    * whose device is re-resolved to another id, e.g. legacy data that carried
    * two ids for one address), so it is exercised here with two distinct ids.
    */
   fresh();
   int r1 = sensor_mint(SENSOR_STELO, "33:33:33:33:33:33", "", "", "", 100);
   int ri = sensor_claim_slot(r1, SENSOR_STELO, "33:33:33:33:33:33");
   g_slot[ri].marker = 2;
   g_slot[ri].color  = 3;
   int r2 = sensor_mint(SENSOR_STELO, "44:44:44:44:44:44", "", "", "", 100);
   ck(r2 != r1 && r2 > 0, "a distinct device yields a distinct id");
   ck(sensor_rebind_slot(r1, r2) == 1, "the slot rebinds to it");
   ck(g_slot[ri].id == r2, "...pointing at the new id");
   ck(g_slot[ri].marker == 2 && g_slot[ri].color == 3,
      "...and the user's marker and colour survive the rebind");
   ck(sensor_rebind_slot(4242, r2) == 0, "rebinding an unknown id fails");

   printf("== out-of-range slot operations are refused, not crashes ==\n");
   sensor_forget_slot(-1);
   sensor_forget_slot(99);
   sensor_set_primary(-1);
   sensor_set_primary(99);
   ck(g_nslot == 1, "bad indices changed nothing");
   ck(sensor_rec_by_id(0) == 0,
      "id 0 (legacy/unregistered) resolves to no row");
   ck(sensor_rec_by_id(-5) == 0, "a negative id resolves to no row");

   printf("== completion fills missing attributes without forking the id ==\n");
   /* A sensor is now registered the moment the user commits to pairing it, so
    * its row is minted BARE (no model/fw, activation unknown) and completed
    * later, when the DIS strings and the session clock arrive. The mechanism
    * must fill only what is missing, survive a reload, and never touch the
    * id -- the old completion path (mint a second id + rebind) is exactly the
    * orphan bug the MAC-only identity key was built to end. */
   {
      fresh();
      int c1 = sensor_mint(SENSOR_G7, "55:55:55:55:55:55", "", "", "", 0);
      ck(c1 > 0, "a bare mint (no attributes yet) succeeds");
      ck(sensor_complete(c1, "", "SW77", "2.1.0", 7777) == 1,
         "completing the bare row reports work done");
      const struct sensor_rec *cr = sensor_rec_by_id(c1);
      ck(cr && !strcmp(cr->model, "SW77") && !strcmp(cr->fw, "2.1.0") &&
             cr->activation == 7777,
         "...and the row now carries model, fw and activation");
      ck(sensor_complete(c1, "", "SW77", "2.1.0", 7777) == 0,
         "completing again is a no-op (idempotent)");
      ck(sensor_complete(c1, "", "EVIL", "9.9.9", 1) == 0,
         "a learned value is never overwritten by a later claim");
      cr = sensor_rec_by_id(c1);
      ck(cr && !strcmp(cr->model, "SW77") && cr->activation == 7777,
         "...so the original truth stands");
      sensors_load(); /* the corrected row must be the one that loads back */
      cr = sensor_rec_by_id(c1);
      ck(cr && !strcmp(cr->model, "SW77") && !strcmp(cr->fw, "2.1.0") &&
             cr->activation == 7777,
         "completion survives a reload (last row wins per id)");
      int dups = 0;
      for (int i = 0; i < g_nsrec; i++)
         if (g_srec[i].id == c1)
            dups++;
      ck(dups == 1, "...as ONE cache row, not a duplicate id");
      int after = sensor_mint(SENSOR_G7, "66:66:66:66:66:66", "", "", "", 0);
      ck(after > c1, "minting after a completion still climbs past every id");
      ck(sensor_complete(4242, "", "M", "1", 1) == 0,
         "completing an unknown id is a refused no-op");
   }

   printf("== the FILE parsers: rows this process did not write ==\n");
   /* Everything above round-trips rows the app itself wrote, so the parsers
    * were never exercised on anything unexpected -- 16 of 18 mutants of
    * rdnum/rdstr/srec_load survived, including moving the cursor advance
    * inside rdnum's digit cap, which is the exact infinite loop that shipped
    * in this file. A parser that runs at every launch and no test executes is
    * how that became possible. */
   {
      fresh();
      FILE *f = fopen(g_sensors_path, "w");
      if (f) {
         /* id,type,identity,serial,model,fw,activation,paired */
         fprintf(f, "1,1,AA:00:00:00:00:01,,M1,1.0,100,200\n");
         fprintf(f, "2,1,AA:00:00:00:00:02,,M1,1.0,100,200\n");
         /* An absurd digit run in the id: must terminate, and must not wrap
          * into a plausible id that could collide with a live one. */
         fprintf(f, "99999999999999999999999,1,AA:00:00:00:00:03,,M,1,1,1\n");
         /* An over-long identity field must not overflow its 24-byte slot. */
         fprintf(f, "4,1,");
         for (int i = 0; i < 200; i++)
            fputc('B', f);
         fprintf(f, ",,M,1,1,1\n");
         fclose(f);
      }
      sensors_load(); /* must terminate */
      ck(sensor_rec_by_id(1) != 0 && sensor_rec_by_id(2) != 0,
         "well-formed rows load");
      const struct sensor_rec *r4 = sensor_rec_by_id(4);
      if (r4)
         ck(strlen(r4->identity) < sizeof r4->identity,
            "an over-long identity is truncated inside its slot, not past it");
      else
         ck(1, "the over-long row was dropped (also acceptable)");

      /* THE invariant: whatever those rows did, a new mint must not reuse an
       * id that any loaded row already holds. */
      int fresh_id =
          sensor_mint(SENSOR_STELO, "CC:00:00:00:00:09", "", "M9", "9", 100);
      ck(fresh_id > 0, "a mint still succeeds after a corrupt file");
      ck(sensor_rec_by_id(fresh_id) != 0, "...and the new row is readable");
      int collides = 0;
      for (int i = 0; i < g_nsrec; i++)
         if (g_srec[i].id == fresh_id)
            collides++;
      ck(collides == 1, "...and its id is unique across every loaded row");
   }

   printf("== wear budget: the model rule, and the pin that overrules it ==\n");
   {
      /* THE REGRESSION. Dexcom's 10-day and 15-day G7s are indistinguishable
       * on the air; only the DIS model tells them apart. A device paired
       * before that model was recognised carried a PIN of 10 from that era,
       * and a pin wins outright -- so once the rule landed the app kept
       * counting a 10-day budget for a sensor it could now positively
       * identify as 15-day, declaring it nearly over with five days to run.
       *
       * The rule below is what makes AUTO correct; the WEAR row's third
       * state (main.c) is what makes AUTO reachable again. Both are needed:
       * either alone leaves the sensor short. */
      ck(sensor_wear_seconds(SENSOR_G7, 0, "SW14758") == 15L * 86400,
         "AUTO on a G7 15 Day (SW14758) resolves to 15 days");
      ck(sensor_wear_seconds(SENSOR_G7, 0, "") == 10L * 86400,
         "AUTO on a G7 of unknown model falls back to 10 -- under-promising "
         "is the safe direction for a wear countdown");
      /* A pin beats the rule, in BOTH directions. That is the point of a pin
       * and also exactly how the wrong answer survived, so pin both ways. */
      ck(sensor_wear_seconds(SENSOR_G7, 10, "SW14758") == 10L * 86400,
         "a pin of 10 overrules even a model that says 15");
      ck(sensor_wear_seconds(SENSOR_G7, 15, "") == 15L * 86400,
         "...and a pin of 15 overrules a type default of 10");
      /* Anything that is not a valid pin must RESOLVE, not be trusted as a
       * literal. slots_load already normalises junk to 0; this is the second
       * line of defence, and it is what build_model's wear_auto mirrors. */
      ck(sensor_wear_seconds(SENSOR_G7, 12, "SW14758") == 15L * 86400,
         "a nonsense pin resolves rather than becoming a 12-day budget");
      ck(sensor_wear_seconds(SENSOR_G7, -1, "SW14758") == 15L * 86400,
         "...including a negative one");
   }

   printf("\n%s\n", all ? "ALL REGISTRY TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
