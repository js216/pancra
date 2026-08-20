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
#include "style.h"   /* MARK_CROSS: the marker a test picks */
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all = 1;

/* A distinct address for the i-th sensor of a DEEP run.
 *
 * The provenance table is thousands of rows wide, and the runs below fill it,
 * so a two-octet counter is no longer enough to give every mint its own
 * identity. Two sensors sharing an address are ONE device to the registry --
 * the second mint returns the first one's id -- which would silently halve
 * any run and leave the table with room it should not have had. `pre` keeps
 * separate runs from colliding with each other. */
static void deep_mac(char *out, int n, unsigned pre, int i)
{
   (void)snprintf(out, (size_t)n, "%02X:00:%02X:%02X:%02X:%02X", pre & 0xFFU,
                  (unsigned)(i >> 24) & 0xFFU, (unsigned)(i >> 16) & 0xFFU,
                  (unsigned)(i >> 8) & 0xFFU, (unsigned)i & 0xFFU);
}

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
   /* The app's own path-building, pointed at the scratch directory: the
    * registry owns its two filenames now, so a test that wrote its own would
    * stop exercising that. */
   sensors_paths(test_dir());
   unlink(sensors_path());
   unlink(slots_path());
   sensors_load();
}

int main(void)
{
   printf("== what the registry loader REPORTS ==\n");
   /* PROVENANCE is what a short read loses here: which physical sensor each
    * historical reading came from. A loader that cannot say "I did not read
    * this whole" lets the app mint a NEW id for a sensor whose row it simply
    * failed to reach -- in an append-only file, permanently. */
   {
      sensors_paths(test_dir());
      unlink(sensors_path());
      unlink(slots_path());
      ck(sensors_load() == 0, "a first run with no files is not an error");

      fresh();
      sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:FF", "", "M", "1.2", 100);
      ck(sensors_load() == 0, "a registry that exists reads whole");

      /* open() on a directory succeeds and read() fails with EISDIR: a real
       * syscall failure rather than a mock. */
      /* A SUBDIRECTORY OF THIS SUITE'S OWN TREE. It used to be "build/app",
       * which is one path for all three build modes -- and this suite runs
       * under ASan as well as plain, so two processes were creating and
       * rmdir'ing build/app/sensors.csv at the same time. Not the fixture
       * directory itself: the name staged here has to BE a directory, while
       * the cases either side of it need a readable file of that name. */
      char eis[160];
      test_path(eis, sizeof eis, "eisdir");
      (void)mkdir(test_dir(), 0755);
      (void)mkdir(eis, 0777);
      sensors_paths(eis);
      unlink(sensors_path());
      if (mkdir(sensors_path(), 0777) == 0 ||
          access(sensors_path(), F_OK) == 0) {
         ck(sensors_load() < 0,
            "a provenance read that FAILS is reported, not taken for EOF");
         rmdir(sensors_path());
      } else {
         ck(0, "could not stage the unreadable-file case");
      }
      sensors_paths(test_dir());
      fresh();

      /* DAMAGE, not merely a failed syscall. Each of these leaves the app
       * with less provenance than the file holds -- and provenance is what
       * says which physical sensor a historical reading came from, in a file
       * that is never rewritten. Skipping the row is right; reporting success
       * afterwards is not. */
      unlink(sensors_path());
      FILE *sf = fopen(sensors_path(), "w");
      if (sf) {
         fprintf(sf, "1,1,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n");
         fprintf(sf, "0,1,AA:BB:CC:DD:EE:02,,M,1.2,100,100\n"); /* no id */
         fprintf(sf, "3,1,AA:BB:CC:DD:EE:03,,M,1.2,100,100\n");
         fclose(sf);
      }
      ck(sensors_load() < 0, "a provenance row with no id makes the load "
                             "incomplete");
      ck(sensor_rec_of(1, 0) && sensor_rec_of(3, 0), "...and the rows either "
                                                     "side are kept");

      unlink(sensors_path());
      sf = fopen(sensors_path(), "w");
      if (sf) {
         fprintf(sf, "1,1,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n");
         fprintf(sf, "3,1,AA:BB:CC:DD:EE:03,,M,1.2,100,10"); /* cut */
         fclose(sf);
      }
      ck(sensors_load() < 0, "a provenance file cut mid-row is incomplete");
      ck(sensor_rec_of(1, 0), "...and what came before it is kept");

      /* A FIELD THAT IS NOT A NUMBER, which is a different failure from a
       * field that is absent and from one that is zero. The grammar has
       * always required all four numeric fields to hold digits, and NOTHING
       * TESTED IT: the four checks could be inverted outright and this whole
       * suite still passed. That mattered the moment those flags stopped
       * being ints where non-zero meant good and became an enum whose OK
       * member is zero -- `!ok` then reads as "the field was fine" and
       * rejects precisely the rows it used to accept.
       *
       * Each row below is well-formed in every other respect: eight fields,
       * seven separators, a plausible id. Only the marked field is bad, so a
       * rejection can only be coming from the field itself.
       *
       * THEY DO NOT ALL REACH THE FLAGS, which is worth writing down rather
       * than assuming. A field of LETTERS stops the cursor dead, so the
       * separator after it is never stepped over and the row is refused by the
       * seven-separator rule or by the trailing-byte rule before any flag is
       * consulted; an empty TYPE reads 0, which is SENSOR_NONE and out of
       * range on its own. The two rows that reach the flags and nothing else
       * are the EMPTY paired time and the nineteen-digit activation, and those
       * are the two that fail if the four comparisons are deleted. The others
       * stay because a malformed field must be refused however the refusal is
       * arrived at -- and because which rule catches which row is exactly the
       * thing that moves when this parser is rearranged. */
      static const char *const badfield[] = {
          "1,1,AA:BB:CC:DD:EE:01,,M,1.2,x,100\n",   /* activation: letters */
          "1,1,AA:BB:CC:DD:EE:01,,M,1.2,100,\n",    /* paired: empty */
          "n,1,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n", /* id: letters */
          "1,,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n",  /* type: empty */
          /* Nineteen digits. The value is NOT the number written -- it is the
           * leading eighteen, which describes a different row. */
          "1,1,AA:BB:CC:DD:EE:01,,M,1.2,1234567890123456789,100\n",
      };
      for (int bi = 0; bi < (int)(sizeof badfield / sizeof badfield[0]); bi++) {
         unlink(sensors_path());
         sf = fopen(sensors_path(), "w");
         if (sf) {
            fprintf(sf, "7,1,AA:BB:CC:DD:EE:07,,M,1.2,100,100\n");
            fputs(badfield[bi], sf);
            fclose(sf);
         }
         ck(sensors_load() < 0,
            "a provenance row with a non-numeric field is refused");
         ck(sensor_rec_of(7, 0), "...and the good row before it is kept");
         ck(!sensor_rec_of(1, 0),
            "...and the bad row itself is NOT in the registry");
      }

      unlink(sensors_path());
      sf = fopen(sensors_path(), "w");
      if (sf) {
         fprintf(sf, "1,1,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n");
         for (int i = 0; i < 600; i++)
            fputc('9', sf);
         fprintf(sf, "\n3,1,AA:BB:CC:DD:EE:03,,M,1.2,100,100\n");
         fclose(sf);
      }
      ck(sensors_load() < 0, "a line longer than any row is incomplete");
      ck(sensor_rec_of(1, 0) && sensor_rec_of(3, 0),
         "...and the rows either side survive");

      /* THE SLOTS FILE has the same rule: it holds every per-device
       * preference, so a row lost in silence is a device that quietly reverts
       * to defaults. */
      unlink(sensors_path());
      unlink(slots_path());
      FILE *lf = fopen(slots_path(), "w");
      if (lf) {
         fprintf(lf, "1,MY SENSOR,1,2,1,2,0,0\n");
         fprintf(lf, "0,NOBODY,1,2,0,2,0,0\n"); /* no id */
         fclose(lf);
      }
      ck(sensors_load() < 0, "a slot row with no id makes the load "
                             "incomplete");
      ck(slot_count() == 1, "...and the real slot is kept");

      unlink(slots_path());
      lf = fopen(slots_path(), "w");
      if (lf)
         fprintf(lf, "1,MY SENSOR,1,2,1,2,0,0"); /* no trailing newline */
      if (lf)
         fclose(lf);
      ck(sensors_load() < 0, "a slots file cut mid-rewrite is incomplete");

      /* ...and a well-formed pair still reads whole, so none of the above is
       * satisfied by always answering -1. */
      unlink(sensors_path());
      unlink(slots_path());
      sf = fopen(sensors_path(), "w");
      if (sf) {
         fprintf(sf, "1,1,AA:BB:CC:DD:EE:01,,M,1.2,100,100\n");
         fclose(sf);
      }
      lf = fopen(slots_path(), "w");
      if (lf) {
         fprintf(lf, "1,MY SENSOR,1,2,1,2,0,0\n");
         fclose(lf);
      }
      ck(sensors_load() == 0, "a whole registry reads whole");
      ck(slot_count() == 1 && sensor_rec_of(1, 0), "...with its slot and its "
                                                   "provenance");

      /* ...AND SO DOES ONE THE APP WROTE ITSELF, which is the case that
       * matters most: a rule that calls the app's own files damaged would
       * warn the user on every launch, and a warning that is always there is
       * a warning nobody reads. */
      fresh();
      int wid =
          sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:77", "S", "M", "1.2", 100);
      ck(wid > 0 &&
             sensor_claim_slot(wid, SENSOR_STELO, "AA:BB:CC:DD:EE:77") >= 0,
         "a sensor is registered and takes a slot");
      ck(sensors_load() == 0, "the files the app just wrote read whole");
      ck(slot_count() == 1 && sensor_rec_of(wid, 0), "...with what it wrote");
      fresh();
   }

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
      ck(sensor_rec_of(a, 0), "...and the original row survived the load");
   }

   printf("== provenance is NEVER evicted, however many sensors follow it "
          "==\n");
   {
      /* THE MOST CONSEQUENTIAL RULE IN THIS FILE, and the one a small-registry
       * test cannot see at all.
       *
       * readings.csv is append-only and EVERY row cites a source_id. The table
       * those ids resolve through used to hold 64 rows and, to make room,
       * dropped "the oldest row that NO LIVE SLOT references" -- which is
       * precisely the set of ids that only HISTORY cites. The reading stayed
       * on disk; the app just stopped being able to say which physical sensor
       * produced it.
       *
       * At the ~61 ids a year a Stelo, a G7 and a meter actually mint, that
       * horizon was about ONE YEAR, and it moved forward for as long as the
       * app was used. What it cost: a device the user FORGOT and re-paired
       * later was no longer recognised by sensor_mint's identity scan and was
       * minted a SECOND id, splitting one physical sensor's history in two; a
       * slot-less live sensor could never have its provenance completed; and
       * sensor_in_warmup, which FAILS OPEN on an id it cannot resolve, let an
       * unresolvable sensor's uncalibrated first hour count towards
       * time-in-range and the daily average at every launch. See
       * MAX_SENSOR_RECS in sensors.h for which of those were reachable at a
       * typical mint rate and which waited for a heavier one.
       *
       * So: mint PAST the table's bound -- the state in which the old rule
       * started throwing rows away -- then ask the FIRST sensor, some
       * MAX_SENSOR_RECS devices ago, for its provenance, and require the
       * RIGHT row back. Overshooting is the whole point: a run that stops
       * exactly AT the bound never makes either rule choose anything, so it
       * would pass with the eviction still in place. */
      fresh();
      const int over_by = 6;
      char first_mac[24];
      char mid_mac[24];
      deep_mac(first_mac, sizeof first_mac, 0x5A, 0);
      deep_mac(mid_mac, sizeof mid_mac, 0x5A, 3);
      int slot_ids[MAX_SLOTS + 3];
      int first    = 0;
      int mid      = 0;
      int seen_max = 0;
      int climbed  = 1;
      int minted   = 0;
      int refused  = 0;
      for (int i = 0; i < MAX_SENSOR_RECS + over_by; i++) {
         char mac[24];
         deep_mac(mac, sizeof mac, 0x5A, i);
         /* A DISTINCT ACTIVATION PER SENSOR, so "the id resolved" can be told
          * apart from "the id resolved to the right device". Every other
          * field a deep run could key on is shared across the run, and a
          * lookup that lands on the wrong row would pass on all of them. */
         int id = sensor_mint(SENSOR_STELO, mac, "", "M", "1", 100000 + i);
         if (id < 0) {
            refused++;
            continue;
         }
         if (id <= seen_max) {
            climbed = 0;
            break;
         }
         seen_max = id;
         if (i == 0)
            first = id;
         if (i == 3)
            mid = id;
         if (i < MAX_SLOTS + 3)
            slot_ids[i] = id;
         minted++;
      }
      ck(climbed, "every mint in a deep run climbs -- no id is ever reissued");
      ck(minted == MAX_SENSOR_RECS,
         "...and a table's worth of distinct devices all fit");
      /* PAST THE BOUND THE TABLE REFUSES; IT DOES NOT FORGET. A sensor whose
       * provenance cannot be held is declined outright -- it shows as
       * unregistered and its readings are logged as source 0, unattributed
       * like pre-registry data -- rather than being made room for by dropping
       * a row that readings already cite. Unattributed is honest;
       * misattributed is not. */
      ck(refused == over_by, "...while every mint past the bound is REFUSED");
      ck(srec_count() == MAX_SENSOR_RECS, "...leaving the table full, not "
                                          "over");

      struct sensor_rec fr;
      ck(sensor_rec_of(first, &fr),
         "the OLDEST sensor still resolves after a full table -- and more --"
         " was minted on top of it");
      ck(fr.activation == 100000 && !strcmp(fr.identity, first_mac),
         "...to ITS OWN row, not to whichever row took its place");

      /* THE CONSEQUENCE THE USER FEELS, asked directly. sensor_in_warmup is
       * how stats.c decides whether a historical reading is uncalibrated
       * first-hour data, and it fails open: an id it cannot resolve is
       * COUNTED. So a dropped row does not merely blank a label on a screen
       * -- it moves time-in-range and the daily average, in a log that is
       * replayed from disk at every launch. */
      ck(sensor_in_warmup(first, 100000 + 60),
         "a reading in the oldest sensor's warmup hour is still known to be "
         "warmup");
      ck(!sensor_in_warmup(first, 100000 + SENSOR_WARMUP_S + 60),
         "...and one past that hour is still known NOT to be");

      /* ACROSS A RELOAD, which is the path a real phone actually takes: the
       * table is rebuilt from the whole file at every launch, so eviction
       * during the LOAD is what cost people their attribution in practice. */
      ck(sensors_load() == 0, "a file with that many rows reads whole");
      ck(srec_count() == MAX_SENSOR_RECS, "...with every id still held");
      ck(sensor_rec_of(first, &fr) && fr.activation == 100000 &&
             !strcmp(fr.identity, first_mac),
         "...and the oldest row is still the oldest device's row afterwards");

      /* A DEVICE THAT COMES BACK COMES BACK AS ITSELF. Minting scans these
       * same rows to recognise an address it has seen before. With the row
       * evicted that scan simply did not find the device and minted a SECOND
       * id for it -- one physical sensor, two identities, everything logged
       * before the eviction orphaned from everything logged after, in a file
       * nothing rewrites. Asked of a device with NO SLOT, because a slot
       * would answer first and prove nothing about the table. */
      ck(!sensor_slot_of(mid, 0), "the device being re-minted has no slot");
      int mid_again = sensor_mint(SENSOR_STELO, mid_mac, "", "M", "1", 7);
      ck(mid_again == mid,
         "re-pairing an address minted a whole table ago returns its ORIGINAL "
         "id");

      /* WHAT IS BOUNDED IS THE UI SNAPSHOT, NOT THE ATTRIBUTION. The user can
       * OWN ten devices; they can have owned thousands. Capping the first
       * costs nothing -- there is no eleventh row to draw -- while capping the
       * second costs the history the log exists to keep. */
      int claimed       = 0;
      int claim_refused = 0;
      for (int i = 0; i < MAX_SLOTS + 3; i++) {
         char mac[24];
         deep_mac(mac, sizeof mac, 0x5A, i);
         int at = sensor_claim_slot(slot_ids[i], SENSOR_STELO, mac);
         if (at < 0)
            claim_refused++;
         else
            claimed++;
      }
      ck(claimed == MAX_SLOTS && claim_refused == 3,
         "the SLOT table stops at MAX_SLOTS and says so");
      struct sensor_view dv;
      sensors_view_get(&dv);
      ck(dv.n == MAX_SLOTS, "...so the UI snapshot stops there too");
      ck(srec_count() > MAX_SLOTS,
         "...while attribution holds far more rows than any snapshot shows");
      ck(!sensor_slot_of(seen_max, 0) && sensor_rec_of(seen_max, 0),
         "a device with no slot at all still resolves: a slot is what the "
         "user OWNS, a row is what the log CITES");

      /* THE SAME AT LOAD, AND REPORTED. A file naming more ids than this
       * build can hold is a file it cannot fully attribute, which is the same
       * loss to the user as a row that will not parse -- so the load comes
       * back incomplete and the app warns, instead of silently resolving some
       * of their readings and not others. */
      fresh();
      FILE *df = fopen(sensors_path(), "w");
      ck(df != 0, "an over-long provenance file can be staged");
      if (df) {
         for (int i = 0; i < MAX_SENSOR_RECS + 4; i++) {
            char mac[24];
            deep_mac(mac, sizeof mac, 0x5C, i);
            fprintf(df, "%d,1,%s,,M,1,%d,200\n", i + 1, mac, 100000 + i);
         }
         fclose(df);
      }
      ck(sensors_load() < 0,
         "a provenance file naming more ids than the table holds is REPORTED, "
         "not silently trimmed");
      ck(srec_count() == MAX_SENSOR_RECS, "...having taken as many as it can");
      ck(sensor_rec_of(1, &fr) && fr.activation == 100000,
         "...and kept the rows it already had rather than rolling them over");

      /* AND THE NEXT ID COMES FROM THE HIGHEST ID, NEVER FROM THE ROW COUNT.
       * The two agree whenever ids are dense, which is almost always -- so a
       * mint that counted rows instead passes every other case in this file
       * and only starts reissuing ids the first time a registry arrives with
       * a GAP in it: a row the parser refused, a file restored from an older
       * backup, a hand-edit. The id it then hands out is one readings.csv may
       * already cite. Two rows, ids 1 and 500. */
      fresh();
      FILE *sp = fopen(sensors_path(), "w");
      ck(sp != 0, "a registry with a gap in its ids can be staged");
      if (sp) {
         fprintf(sp, "1,1,5D:00:00:00:00:01,,M,1,100,200\n");
         fprintf(sp, "500,1,5D:00:00:00:01:F4,,M,1,100,200\n");
         fclose(sp);
      }
      ck(sensors_load() == 0, "a registry with a GAP in its ids reads whole");
      int sparse =
          sensor_mint(SENSOR_STELO, "5D:00:00:00:02:00", "", "M", "1", 100);
      ck(sparse == 501,
         "the next id climbs from the HIGHEST id, not from the number of rows");

      /* AND A COMPLETION FOR AN ID THAT DOES NOT EXIST MUST NOT LAND ON THE
       * ROW THAT SITS WHERE IT WOULD GO. An id-ordered table is looked up by
       * bisection, and a bisection answers two questions at once: where this
       * id IS, and where it WOULD go. Acting on the second overwrites a real
       * device's model, firmware and session start with another id's learned
       * attributes -- and the corrected row is APPENDED to sensors.csv, so
       * the lie is durable and last-wins makes it authoritative. Id 250 lands
       * in the gap, whose insertion point is the LIVE row for id 500. */
      int n_before = srec_count();
      int gap_rc   = sensor_complete(250, "SX", "MX", "FX", 424242);
      ck(gap_rc == 0, "completing an id that falls in a GAP is a refused "
                      "no-op");
      ck(srec_count() == n_before, "...and invents no row");
      ck(!sensor_rec_of(250, 0), "...for the id it was asked about");
      struct sensor_rec nb;
      ck(sensor_rec_of(500, &nb) && !strcmp(nb.model, "M") &&
             !strcmp(nb.fw, "1") && nb.activation == 100,
         "...and leaves the row it would have displaced exactly as it was");
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
   sensor_set_primary(s2);
   ck(sensor_primary_slot() == i2, "primary moves on request");
   ck(slot_at(i1).primary == 0, "...and only ONE slot is primary");
   ck(sensor_primary_id() == s2, "primary_id agrees with primary_slot");

   printf("== forget shifts the table without disturbing the survivor ==\n");
   /* The historical bug: anything keyed on a slot's POSITION silently
    * re-pointed at a different sensor after a forget. */
   sensor_forget(s1);
   ck(slot_count() == 1, "one slot remains");
   ck(slot_at(0).id == s2, "the SURVIVOR is the one that was not forgotten");
   ck(sensor_slot_by_mac("11:11:11:11:11:11") < 0, "the forgotten one is gone");
   ck(sensor_rec_of(s1, 0),
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
   /* Through the registry's own setters: they validate and persist, which is
    * what makes the marker survive the rebind below. */
   ck(sensor_set_marker(r1, 2) == 0, "the user picks a marker");
   ck(sensor_set_color(r1, 3) == 0, "...and a colour");
   int r2 = sensor_mint(SENSOR_STELO, "44:44:44:44:44:44", "", "", "", 100);
   ck(r2 != r1 && r2 > 0, "a distinct device yields a distinct id");
   ck(sensor_rebind_slot(r1, r2) == 1, "the slot rebinds to it");
   ck(slot_at(ri).id == r2, "...pointing at the new id");
   ck(slot_at(ri).marker == 2 && slot_at(ri).color == 3,
      "...and the user's marker and colour survive the rebind");
   ck(sensor_rebind_slot(4242, r2) == 0, "rebinding an unknown id fails");

   printf("== an operation on an UNKNOWN id changes nothing ==\n");
   sensor_forget(-1);
   sensor_forget(9999);
   sensor_set_primary(-1);
   sensor_set_primary(9999);
   ck(slot_count() == 1, "an id no slot holds changes nothing");
   ck(!sensor_rec_of(0, 0), "id 0 (legacy/unregistered) resolves to no row");
   ck(!sensor_rec_of(-5, 0), "a negative id resolves to no row");

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
      struct sensor_rec cr_v;
      int cr_ok                   = sensor_rec_of(c1, &cr_v);
      const struct sensor_rec *cr = cr_ok ? &cr_v : 0;
      ck(cr && !strcmp(cr->model, "SW77") && !strcmp(cr->fw, "2.1.0") &&
             cr->activation == 7777,
         "...and the row now carries model, fw and activation");
      ck(sensor_complete(c1, "", "SW77", "2.1.0", 7777) == 0,
         "completing again is a no-op (idempotent)");
      ck(sensor_complete(c1, "", "EVIL", "9.9.9", 1) == 0,
         "a learned value is never overwritten by a later claim");
      cr_ok = sensor_rec_of(c1, &cr_v);
      ck(cr_ok && !strcmp(cr_v.model, "SW77") && cr_v.activation == 7777,
         "...so the original truth stands");
      sensors_load(); /* the corrected row must be the one that loads back */
      cr_ok = sensor_rec_of(c1, &cr_v);
      ck(cr_ok && !strcmp(cr_v.model, "SW77") && !strcmp(cr_v.fw, "2.1.0") &&
             cr_v.activation == 7777,
         "completion survives a reload (last row wins per id)");
      int dups = 0;
      for (int i = 0; i < srec_count(); i++)
         if (srec_at(i).id == c1)
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
      FILE *f = fopen(sensors_path(), "w");
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
      ck(sensor_rec_of(1, 0) && sensor_rec_of(2, 0), "well-formed rows load");
      struct sensor_rec r4_v;
      int r4_ok                   = sensor_rec_of(4, &r4_v);
      const struct sensor_rec *r4 = r4_ok ? &r4_v : 0;
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
      ck(sensor_rec_of(fresh_id, 0), "...and the new row is readable");
      int collides = 0;
      for (int i = 0; i < srec_count(); i++)
         if (srec_at(i).id == fresh_id)
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

   printf("\n== the SNAPSHOT is the walk ==\n");
   /* The registry lock used to be public, and every caller that wanted to
    * walk the slots took it by hand -- eleven files, several of which did not
    * take it at all, and one that held it across a call taking the DRIVER's
    * lock (the inverse of the documented order). The walk is one call now, so
    * this is the interface that has to be right: slot i and rec i must
    * describe the SAME device, and a slot with no provenance row must say so
    * rather than carrying the previous row's identity. */
   {
      fresh();
      int a = sensor_mint(SENSOR_STELO, "11:11:11:11:11:11", "SA", "MA", "1.0",
                          1000);
      int b = sensor_mint(SENSOR_ONETOUCH, "22:22:22:22:22:22", "SB", "MB",
                          "2.0", 2000);
      ck(sensor_claim_slot(a, SENSOR_STELO, "11:11:11:11:11:11") == 0,
         "the CGM takes the first slot");
      ck(sensor_claim_slot(b, SENSOR_ONETOUCH, "22:22:22:22:22:22") == 1,
         "the meter takes the second");

      struct sensor_view v;
      sensors_view_get(&v);
      ck(v.n == 2, "the view holds every slot");
      ck(v.slot[0].id == a && v.slot[1].id == b,
         "...in registry order, ids intact");
      ck(v.have_rec[0] && v.have_rec[1], "...each with its provenance row");
      /* PAIRED, not merely present: reading the slots and the records as two
       * separate walks is what let one sensor's address be bound to another's
       * link. */
      ck(v.rec[0].id == a && !strcmp(v.rec[0].identity, "11:11:11:11:11:11"),
         "row 0 is slot 0's device, address and all");
      ck(v.rec[1].id == b && !strcmp(v.rec[1].identity, "22:22:22:22:22:22"),
         "row 1 is slot 1's device -- the pairing is what a snapshot is FOR");
      ck(v.rec[0].type == SENSOR_STELO && v.rec[1].type == SENSOR_ONETOUCH,
         "...so a kind test on rec[i] answers for slot i");

      /* A SLOT WITHOUT PROVENANCE. have_rec is the only thing that says so;
       * without it rec[i] is a zeroed row that reads as a real one, and a
       * caller would decide "not a CGM" for a device it cannot resolve --
       * which is a different answer from "no such row". */
      ck(sensor_claim_slot(4242, SENSOR_STELO, "33:33:33:33:33:33") == 2,
         "a slot can name an id with no provenance row");
      sensors_view_get(&v);
      ck(v.n == 3, "the view grows with the registry");
      ck(!v.have_rec[2], "...and says which slot has no row");
      ck(v.have_rec[0] && v.rec[0].id == a,
         "...without disturbing the rows that do");

      /* THE SNAPSHOT IS A COPY. A caller holding it across a mint (which
       * memmoves the records) or a retire (which rewrites the slots) must see
       * what it was handed, not what the registry has since become. */
      sensor_retire(a);
      ck(v.n == 3 && v.slot[0].id == a && !v.slot[0].old,
         "a retire after the fact does not reach into a snapshot already "
         "taken");
      struct sensor_view v2;
      sensors_view_get(&v2);
      ck(v2.slot[0].old, "...while the NEXT snapshot sees it");

      /* sensor_id_is_live: the question meterstore asks before evicting a
       * row. A RETIRED slot is still the user's device -- it keeps its place
       * and its preferences -- so it must answer yes, or its meter index is
       * the first thing thrown away. */
      ck(sensor_id_is_live(a), "a retired device is still one of the user's");
      ck(sensor_id_is_live(b), "...as is a live one");
      ck(sensor_id_is_live(4242), "...and one with no provenance row");
      ck(!sensor_id_is_live(999999), "an id with no slot is not live");
      ck(!sensor_id_is_live(0), "...nor is the legacy id 0");

      /* sensor_slot_at: the ONE-CALL form of "is there a slot there, and
       * what is it". Production code has nothing else -- `make lockcheck`
       * refuses slot_count/slot_at outside this module -- because a bounds
       * check and an index read as two calls can describe two devices. */
      struct sensor_slot at;
      ck(sensor_slot_at(0, &at) && at.id == a, "slot 0 answers with its own "
                                               "device");
      ck(sensor_slot_at(2, &at) && at.id == 4242,
         "...and so does a slot whose device has no provenance row");
      ck(!sensor_slot_at(3, &at), "one past the end says NO");
      ck(at.id == 0, "...and zeroes what it was given, so a caller that "
                     "ignores the answer cannot act on the last device");
      ck(!sensor_slot_at(-1, 0), "a negative index says no");
      ck(sensor_slot_at(1, 0), "and `out` may be NULL when only the "
                               "existence is asked");

      /* SLOT ORDER IS NOT RECORD ORDER, and pairing them by position is the
       * mistake this whole interface exists to make impossible. Provenance is
       * append-only and keeps its positions for ever; the slots shift down
       * the moment a device is forgotten. Line them up by index and every
       * slot after the forgotten one is handed the WRONG device's address --
       * which is what gets fed to dexble_pair. */
      fresh();
      int p = sensor_mint(SENSOR_STELO, "AA:00:00:00:00:01", "", "", "", 10);
      int q = sensor_mint(SENSOR_STELO, "AA:00:00:00:00:02", "", "", "", 20);
      sensor_claim_slot(p, SENSOR_STELO, "AA:00:00:00:00:01");
      sensor_claim_slot(q, SENSOR_STELO, "AA:00:00:00:00:02");
      sensor_forget(p); /* slots: [q]; records: still [p, q] */
      sensors_view_get(&v);
      ck(v.n == 1 && v.slot[0].id == q, "a forgotten slot shifts the rest up");
      ck(v.have_rec[0] && v.rec[0].id == q &&
             !strcmp(v.rec[0].identity, "AA:00:00:00:00:02"),
         "rec[0] is the row that BELONGS to slot 0, not the row that sits at "
         "the same index");
   }

   printf("\n== an action follows the DEVICE, not the position ==\n");
   /* THE TOCTOU THIS INTERFACE EXISTS TO KILL. The UI reads "the selected
    * device is slot 1", the user taps PRIMARY, and in between a binder thread
    * mints or forgets -- every slot after the change has moved. An operation
    * that took the INDEX would then promote, rename, recolour or DISCONNECT
    * whichever device had slid into slot 1. Every operation takes the id, so
    * the shift is irrelevant: the id names one physical device for ever. */
   {
      fresh();
      int one = sensor_mint(SENSOR_STELO, "C0:00:00:00:00:01", "", "", "", 10);
      int two = sensor_mint(SENSOR_STELO, "C0:00:00:00:00:02", "", "", "", 20);
      int three =
          sensor_mint(SENSOR_STELO, "C0:00:00:00:00:03", "", "", "", 30);
      sensor_claim_slot(one, SENSOR_STELO, "C0:00:00:00:00:01");
      sensor_claim_slot(two, SENSOR_STELO, "C0:00:00:00:00:02");
      sensor_claim_slot(three, SENSOR_STELO, "C0:00:00:00:00:03");

      /* What the UI captured: slot 2 holds `three`. */
      struct sensor_slot picked;
      ck(sensor_slot_at(2, &picked) && picked.id == three,
         "the user is looking at the third device");
      /* ...and now the table shifts under it. */
      sensor_forget(one);
      ck(sensor_slot_at(2, 0) == 0, "the table has SHRUNK: index 2 is gone");

      /* The action, asked for by id, still lands on the device the user
       * pointed at -- which is now at index 1. */
      ck(sensor_set_marker(picked.id, MARK_CROSS) == 0,
         "the marker change is accepted");
      struct sensor_slot now2;
      ck(sensor_slot_at(1, &now2) && now2.id == three,
         "...and the device has moved to index 1");
      ck(now2.marker == MARK_CROSS, "the marker landed on the RIGHT device");
      struct sensor_slot other;
      ck(sensor_slot_at(0, &other) && other.id == two &&
             other.marker != MARK_CROSS,
         "...and not on the one that moved into its old index");

      /* The same for the destructive pair, which is where it would hurt
       * most: DISCONNECT must retire the device the user chose. */
      sensor_retire(picked.id);
      ck(sensor_slot_at(1, &now2) && now2.id == three && now2.old,
         "DISCONNECT retires the device that was picked");
      ck(sensor_slot_at(0, &other) && other.id == two && !other.old,
         "...and leaves the one that took its index alone");
   }

   printf("\n== a provenance row must be a WHOLE row ==\n");
   /* PROVENANCE IS PERMANENT AND IS NEVER REWRITTEN: it says which physical
    * sensor produced every reading in the append-only log. The parser used to
    * step over a missing separator and read an empty field as 0, so a
    * truncated or run-together row became a shorter row made of whatever text
    * remained -- accepted and pushed. A row whose type is garbage resolves to
    * KIND_CGM, which is what decides whether a value can own the big number,
    * feed the alarm and be calibrated against. */
   {
      static const char *const bad[] = {
          "1,1,AA:BB,S,M,F,100",      /* seven fields: one comma short */
          "1,1AA:BB,S,M,F,100,200",   /* a separator run together */
          ",1,AA:BB,S,M,F,100,200",   /* no id at all */
          "1,,AA:BB,S,M,F,100,200",   /* no type at all */
          "1,1,AA:BB,S,M,F,,200",     /* no activation */
          "1,1,AA:BB,S,M,F,100,",     /* no paired time */
          "1,x,AA:BB,S,M,F,100,200",  /* a type that is not a number */
          "1,0,AA:BB,S,M,F,100,200",  /* SENSOR_NONE is not a sensor */
          "1,99,AA:BB,S,M,F,100,200", /* a type this build does not know */
          "1,-1,AA:BB,S,M,F,100,200", /* nor a negative one */
          "1,1,AA:BB,S,M,F,-5,200",   /* time before the epoch */
          "-3,1,AA:BB,S,M,F,100,200", /* a negative id */
      };
      for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
         fresh();
         FILE *f = fopen(sensors_path(), "w");
         ck(f != 0, "the provenance file opens");
         if (!f)
            continue;
         fprintf(f, "%s\n", bad[i]);
         fclose(f);
         char what[96];
         (void)snprintf(what, sizeof what, "REJECTED: \"%s\"", bad[i]);
         ck(sensors_load() < 0, what);
         ck(srec_count() == 0, "...and nothing was taken from it");
      }
      /* ...AND THE WHOLE ROW IS STILL ACCEPTED. A grammar that rejects
       * everything would pass every case above and lose every real row. */
      fresh();
      FILE *g = fopen(sensors_path(), "w");
      ck(g != 0, "the provenance file opens for the good row");
      if (g) {
         fprintf(g, "%s\n", "7,1,AA:BB:CC:DD:EE:FF,SER,MOD,1.2,100,200");
         fclose(g);
         ck(sensors_load() == 0, "a WHOLE row still reads whole");
         ck(srec_count() == 1, "...and is taken");
         struct sensor_rec r;
         ck(sensor_rec_of(7, &r) && r.type == SENSOR_STELO &&
                r.activation == 100 && r.paired == 200 &&
                !strcmp(r.identity, "AA:BB:CC:DD:EE:FF"),
            "...with every field where it belongs");
      }
      /* A NINTH FIELD IS A NEWER SCHEMA, NOT A BROKEN ROW, and it has to
       * keep reading. This file grows by appending columns -- that is what
       * lets a newer build add one without orphaning every reading an older
       * one attributed. Rejecting a long row would turn the next schema
       * addition into permanent data loss on any phone not yet updated,
       * which is the opposite of what the grammar above is for: it refuses
       * rows that say LESS than they must, not rows that say more. */
      struct sensor_rec r9;
      fresh();
      FILE *n9 = fopen(sensors_path(), "w");
      if (n9) {
         fprintf(n9, "%s\n", "9,1,CC:CC:CC:CC:CC:CC,S,M,F,100,200,futurefield");
         fclose(n9);
         ck(sensors_load() == 0, "a row from a NEWER schema still reads");
         ck(sensor_rec_of(9, &r9) && r9.activation == 100 && r9.paired == 200,
            "...and every field this build knows is where it belongs");
      }

      /* ...AND JUNK IS NOT A NINTH FIELD. The two are one byte apart: a ','
       * begins a column this build does not know, and anything else is text
       * stuck to the last number. The grammar checked every field and then
       * stopped looking, so "200junk" was a valid row with the junk ignored
       * -- accepted, pushed, and permanent, describing which physical sensor
       * every reading in an append-only log came from. */
      fresh();
      FILE *nj = fopen(sensors_path(), "w");
      if (nj) {
         fprintf(nj, "%s\n", "9,1,CC:CC:CC:CC:CC:CC,S,M,F,100,200junk");
         fclose(nj);
         ck(sensors_load() != 0, "a row with junk stuck to its last number is "
                                 "REJECTED, not read as a valid prefix");
         ck(!sensor_rec_of(9, &r9), "...and nothing of it is remembered");
      }
      /* The same shape one field earlier: the activation time is numeric and
       * followed by a separator, so junk there ends the field early. */
      fresh();
      FILE *na = fopen(sensors_path(), "w");
      if (na) {
         fprintf(na, "%s\n", "9,1,CC:CC:CC:CC:CC:CC,S,M,F,100junk,200");
         fclose(na);
         ck(sensors_load() != 0, "junk inside an earlier numeric field is "
                                 "rejected too");
      }
      /* A NUMBER TOO LONG TO HOLD is not the number the digit cap leaves
       * behind. Nineteen digits and up: the accumulation stops, so what is
       * kept describes a different row -- and for the id, a smaller one,
       * which is how the next mint comes to reuse a LIVE id. */
      fresh();
      FILE *nb = fopen(sensors_path(), "w");
      if (nb) {
         fprintf(nb, "%s\n",
                 "99999999999999999999,1,CC:CC:CC:CC:CC:CC,S,M,F,100,200");
         fclose(nb);
         ck(sensors_load() != 0, "an id too long for the parser to hold is "
                                 "rejected, not truncated into a live one");
      }
      /* IN A FIELD WHERE TRUNCATION STAYS LEGAL. The id above is caught by
       * `id <= 0` however it is parsed -- a twenty-digit id truncates to a
       * negative int -- so that case cannot tell the digit rule from the
       * range check. An ACTIVATION TIME of nineteen digits truncates to a
       * large POSITIVE number that passes every other test, so this is the
       * one that fails if the rule is removed. */
      fresh();
      FILE *nc = fopen(sensors_path(), "w");
      if (nc) {
         fprintf(nc, "%s\n",
                 "9,1,CC:CC:CC:CC:CC:CC,S,M,F,1000000000000000000,200");
         fclose(nc);
         ck(sensors_load() != 0,
            "a nineteen-digit timestamp is rejected, not silently truncated "
            "to a different instant");
         ck(!sensor_rec_of(9, &r9), "...and no row of it is remembered");
      }

      /* An activation of 0 is REAL: "session start not learned yet". */
      fresh();
      FILE *h = fopen(sensors_path(), "w");
      if (h) {
         fprintf(h, "%s\n", "8,1,BB:BB:BB:BB:BB:BB,,,,0,200");
         fclose(h);
         ck(sensors_load() == 0, "an activation of 0 is a value, not a gap");
         struct sensor_rec r0;
         ck(sensor_rec_of(8, &r0) && r0.activation == 0,
            "...and reads back as 0");
      }
   }

   printf("\n== a change that could not be SAVED did not happen ==\n");
   /* THE WHOLE POINT OF A TRANSACTION. slots.csv is replaced by rename, so a
    * failed write leaves the previous file whole -- which means the honest
    * answer is to put the table back and say nothing changed. Reported as
    * success with the memory mutated, the app showed a device retired,
    * renamed or promoted that came back as it was at the next launch; and a
    * CLAIM reported as success let the pairing go on to erase a key file and
    * bond a sensor whose slot was never written. */
   {
      fresh();
      int a1 = sensor_mint(SENSOR_STELO, "F0:00:00:00:00:01", "", "", "", 10);
      int b1 = sensor_mint(SENSOR_STELO, "F0:00:00:00:00:02", "", "", "", 20);
      ck(sensor_claim_slot(a1, SENSOR_STELO, "F0:00:00:00:00:01") == 0,
         "the first device takes a slot");
      ck(sensor_claim_slot(b1, SENSOR_STELO, "F0:00:00:00:00:02") == 1,
         "...and the second");
      ck(sensor_set_marker(b1, MARK_CROSS) == SENSOR_OK, "a marker is saved");

      /* From here every replace of slots.csv fails. */
      setenv("APP_FAIL_RENAME", "1", 1);

      struct sensor_slot before;
      ck(sensor_slot_of(b1, &before), "the second device is still there");

      ck(sensor_set_marker(b1, MARK_CIRCLE) == SENSOR_UNSAVED,
         "a preference change that cannot be written says so");
      struct sensor_slot after;
      ck(sensor_slot_of(b1, &after) && after.marker == before.marker,
         "...and the marker in MEMORY is the one on disk, not the new one");

      ck(sensor_retire(b1) == SENSOR_UNSAVED, "DISCONNECT says so too");
      ck(sensor_slot_of(b1, &after) && !after.old,
         "...and the device is still live, as the file still says");

      ck(sensor_set_primary(b1) == SENSOR_UNSAVED, "so does the primary");
      ck(sensor_slot_of(a1, &after) && after.primary,
         "...and the big number still belongs to the device that had it");

      ck(sensor_forget(a1) == SENSOR_UNSAVED, "so does a forget");
      ck(slot_count() == 2, "...and the table still holds both");

      /* THE CLAIM IS THE DANGEROUS ONE: its answer is what commit_pair uses
       * to decide whether to erase a key file and bond. */
      int c1 = sensor_mint(SENSOR_STELO, "F0:00:00:00:00:03", "", "", "", 30);
      ck(sensor_claim_slot(c1, SENSOR_STELO, "F0:00:00:00:00:03") < 0,
         "a claim that was not written REFUSES, exactly as a full table does");
      ck(slot_count() == 2, "...and leaves no half-claimed slot behind");

      unsetenv("APP_FAIL_RENAME");
      ck(sensor_set_marker(b1, MARK_CIRCLE) == SENSOR_OK,
         "with the filesystem working again the same change commits");
      ck(sensor_slot_of(b1, &after) && after.marker == MARK_CIRCLE,
         "...and now memory holds it");
   }

   printf("\n%s\n", all ? "ALL REGISTRY TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
