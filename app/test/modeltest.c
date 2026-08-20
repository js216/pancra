// SPDX-License-Identifier: GPL-3.0
// modeltest.c --- what the app decides to SHOW
// Copyright 2026 Jakob Kastelic

/* uitest proves that ui_render draws a `struct screen` correctly. It builds
 * those structs BY HAND. Nothing proved the other half: that the app fills
 * one correctly from its own state. build_model is the only function that
 * does, it lives in main.c, and until this file no test binary linked main.c
 * at all -- so a bug that bound the big number to the wrong sensor, or left a
 * previous screen's value in place, drew perfectly and passed every gate.
 *
 * That is the highest-consequence bug this app can have: a glucose reading
 * that is displayed but not true.
 *
 * main.c is INCLUDED rather than linked because this file also reaches the
 * shell's own statics -- the link table, the reconcile tick -- which are
 * right to keep private to it and merely mean the test must live inside the
 * same translation unit. build_model itself now lives in model.c and is
 * linked normally. The only thing that has to be stubbed is the BLE transport
 * (the dexble_* family in bletrans.h, and the four JNI entry points in
 * blejni.h), because nothing here touches a radio or a VM.
 */
/* Named explicitly even though main.c includes them all: this file uses these
 * declarations directly, and inheriting them through a .c include hides that
 * from anything checking (and from a reader). */
#include "model.h"      /* build_model + the pre-draw snapshot */
#include "alarmlogic.h" /* AL_ENTRY_MAX: the keypad ceiling asserted below */
#include "blejni.h"
#include "bletrans.h"
#include "dexdriver.h" /* the drv_* upcalls stubbed below */
#include "forms.h"
#include "input.h" /* the press identity rule this drives directly */ /* the keypad state these tests type into */
#include "keypad.h"  /* KP_*: the keypad modes, by name */
#include "reading.h" /* pancra_glucose: the verdict the CGM driver acts on */
#include "remote.h"  /* the sync status the frame renders */
#include "selection.h"
#include "syncstat.h"
#include "testdir.h" /* test_dir: the per-mode fixture directory */
#include "ui.h"      /* struct screen, SCR_*: what build_model fills in */
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "main.c"

/* ---- the transport this test does not have ---- */
void dexble_init(const char *d)
{
   (void)d;
}

int dexble_register(JNIEnv *e, jclass b, jobject c)
{
   (void)e;
   (void)b;
   (void)c;
   return 0;
}

void dexble_set_alarm(JNIEnv *e, jclass a)
{
   (void)e;
   (void)a;
}

void dexble_pair(int l, const char *m, const char *c)
{
   (void)l;
   (void)m;
   (void)c;
}

int dexble_create_bond(const char *m)
{
   (void)m;
   return 0;
}

/* NO STUB FOR dexble_bond_state ANY MORE. It was here because the table lived
 * inside the JNI bridge, which this suite cannot link; the table is
 * app/bondtable.c now, needs nothing but a lock, and is linked for real. The
 * stub returned 0 for every address, which is exactly what the real table
 * answers for an address nothing has reported -- so this suite sees the same
 * thing it always did, from the code that ships. */

void dexble_reconnect(int l)
{
   (void)l;
}

void dexble_request_devinfo_link(int l)
{
   (void)l;
}

void dexble_link_close(int l)
{
   (void)l;
}

void dexble_subscribe(int l, const char *u, int i)
{
   (void)l;
   (void)u;
   (void)i;
}

int dexble_meter_connect(int l, const char *m)
{
   (void)l;
   (void)m;
   return 0;
}

int dexble_alarm(int k, int s, int v)
{
   (void)k;
   (void)s;
   (void)v;
   return 0;
}

void dexble_beep(void)
{
}

void dexble_chirp(int s)
{
   (void)s;
}

void dexble_nudge(int k, int s, int v)
{
   (void)k;
   (void)s;
   (void)v;
}

int dexble_alarm_silence(void)
{
   return 0;
}

void dexble_write(int link, const char *uuid, const uint8_t *d, int n, int rsp)
{
   (void)link;
   (void)uuid;
   (void)d;
   (void)n;
   (void)rsp;
}

/* dexdriver's upcalls, normally implemented in dexble.c: this test drives
 * build_model directly and never runs the driver, so they only have to
 * exist. drv_key_load/drv_mac_load report "nothing stored", which is the
 * honest answer for a test with no paired sensor. */
void drv_connect(int link, const char *mac)
{
   (void)link;
   (void)mac;
}

void drv_subscribe(int link, const char *uuid, int indicate)
{
   (void)link;
   (void)uuid;
   (void)indicate;
}

void drv_write(int link, const char *uuid, const uint8_t *data, int n,
               int no_resp)
{
   (void)link;
   (void)uuid;
   (void)data;
   (void)n;
   (void)no_resp;
}

void drv_status(const char *s)
{
   (void)s;
}

int drv_glucose(int link, int mg_dl, int trend, int age_s)
{
   (void)link;
   (void)mg_dl;
   (void)trend;
   (void)age_s;
   return 0;
}

void drv_cal_result(int link, int result, int sensor_id, int mg_dl,
                    unsigned gen)
{
   (void)link;
   (void)result;
   (void)sensor_id;
   (void)mg_dl;
   (void)gen;
}

int drv_key_load(int link, uint8_t key[16])
{
   (void)link;
   (void)key;
   return 0;
}

int drv_key_save(int link, const uint8_t key[16])
{
   (void)link;
   (void)key;
   return 0;
}

int drv_backfill(int link, int mg_dl, int trend, int age_s)
{
   (void)link;
   (void)mg_dl;
   (void)trend;
   (void)age_s;
   return 0;
}

int drv_mac_load(int link, char *mac, int n)
{
   (void)mac;
   (void)n;
   (void)link;
   return 0;
}

void drv_key_clear(int link)
{
   (void)link;
}

void drv_mac_clear(int link)
{
   (void)link;
}

int drv_mac_save(int link, const char *mac)
{
   (void)link;
   (void)mac;
   return 0;
}

jobject dexble_ctx(void)
{
   return 0;
}

JNIEnv *dexble_env(void)
{
   return 0;
}

/* ---- the test ---- */

static int g_fail;
/* HOW MANY CHECKS ACTUALLY RAN. A suite whose assertions stopped being
 * reached passes exactly as loudly as one that checked everything -- which is
 * the first thing a mutation drill runs into: delete a rule, watch the suite
 * go green, and only later notice the case that pinned it was never entered.
 * Printed at the end, so a run that checked nothing says so. */
static long g_checks;

static void ck(int cond, const char *what)
{
   g_checks++;
   if (!cond) {
      printf("  FAIL: %s\n", what);
      g_fail = 1;
   }
}

/* Every screen the enum can hold, so a new one cannot be added without
 * appearing here. */
static const enum ui_screen all_screens[] = {
    SCR_MAIN,      SCR_SETTINGS, SCR_KEYPAD,      SCR_DEVLIST,  SCR_GATE,
    SCR_SENSOR,    SCR_CAL,      SCR_CALPEND,     SCR_RESCALE,  SCR_RESCALEACT,
    SCR_SENSTYPE,  SCR_FORGET,   SCR_LABEL,       SCR_MARKPICK, SCR_COLORPICK,
    SCR_METERHELP, SCR_PAIRCONF, SCR_SYNCRESTORE, SCR_ADDMENU,  SCR_INSULIN,
    SCR_DEVICES,   SCR_PERMS,    SCR_OLDDEV,      SCR_RECONF,   SCR_REMOTE,
    SCR_INSLOG,    SCR_WEIGHT,   SCR_WTLOG,       SCR_WTDEL,    SCR_DISPLAY,
    SCR_INSDEL,    SCR_ALARM,    SCR_EXPORT,   SCR_FOOD,
    SCR_FOODTYPE,  SCR_FOODLOG};

/* The sync worker's job, as fast as a thread can do it: report a new outcome
 * while frames are being built. Two outcomes whose labels differ in length
 * and in every byte, so any mixture of the two is detectable. */
/* ATOMIC, not volatile. `volatile` orders nothing between threads and is not
 * a synchronisation primitive in C -- a stop flag written by one thread and
 * read by another through a plain volatile is a data race, which is undefined
 * rather than merely unlikely, and a test built on undefined behaviour cannot
 * be evidence about the code it is testing. */
static atomic_int rs_stop;

/* How many reports the worker has made, so the test can show that it was
 * running WHILE the frames were being built rather than before them. */
static atomic_long rs_writes;

static void *rs_writer(void *p)
{
   (void)p;
   while (!atomic_load_explicit(&rs_stop, memory_order_relaxed)) {
      remote_note_outcome(SYNC_OK);      /* "SYNCED" */
      remote_note_outcome(SYNC_TIMEOUT); /* "TIMED OUT" */
      atomic_fetch_add_explicit(&rs_writes, 2, memory_order_relaxed);
   }
   return NULL;
}

int main(void)
{
   /* EVERY SCREEN IS ITS OWN SCREEN, and now that is structural.
    *
    * This used to check that a hand-written translating switch from
    * MENU_* to SCR_* -- was a bijection, because a menu whose mapping was
    * forgotten opened and silently rendered the MAIN screen. There is one
    * enum now and no map, so the property cannot fail; what CAN still fail is
    * the model not following it, which is what this checks instead: the
    * screen the shell is on is the screen the frame says it is.
    *
    * The table is still exhaustive on purpose -- it is what makes adding a
    * screen and forgetting it here a failure rather than a silent gap. */
   int n = (int)(sizeof all_screens / sizeof all_screens[0]);
   ck(n == SCR_N, "this test lists every screen the enum defines");
   {
      struct screen probe;
      g_gate = 0;
      for (int i = 0; i < n; i++) {
         if (all_screens[i] == SCR_GATE)
            continue; /* not reachable by navigation: see the gate below */
         nav_go(all_screens[i]);
         build_model(&probe);
         if (probe.scr != all_screens[i]) {
            printf("  FAIL: screen %d renders as %d\n", (int)all_screens[i],
                   (int)probe.scr);
            g_fail = 1;
         }
      }
      /* ...and the gate outranks all of it: the first-run rationale must be
       * what is drawn whatever modal happens to be open behind it. */
      g_gate = 1;
      nav_go(SCR_SETTINGS);
      build_model(&probe);
      ck(probe.scr == SCR_GATE, "the gate outranks whatever menu is open");
      g_gate = 0;
      nav_go(SCR_MAIN);
   }

   /* THE BIG NUMBER IS THE CURRENT READING. Everything else on the screen is
    * context; this is the thing somebody acts on. */
   struct screen m;
   g_gate = 0;
   nav_home();
   /* Through the RECORDING path, not by writing the globals: they are
    * private now, and a test that sets them by hand is a test of a state the
    * app cannot reach. */
   struct reading_event ev = {.t          = 1700000000L,
                              .glu        = 137,
                              .trend      = 4,
                              .src        = 0,
                              .kind       = KIND_CGM,
                              .rescale_pm = 1000,
                              .prime      = -1};
   (void)store_record(&ev, 0);
   build_model(&m);
   ck(m.scr == SCR_MAIN, "with no menu open the model is the main screen");
   ck(m.reading.glu == 137, "the model carries the current glucose");
   ck(m.reading.trend == 4, "...and its trend");
   ck(m.reading.t == 1700000000L, "...and the time it was taken");

   /* A MENU MUST NOT DISTURB THE READING: opening one changes the screen and
    * nothing else, or the number under it goes stale without saying so. */
   nav_go(SCR_SETTINGS);
   build_model(&m);
   ck(m.scr == SCR_SETTINGS, "opening a menu selects its screen");
   ck(m.reading.glu == 137 && m.reading.t == 1700000000L,
      "...and leaves the reading untouched");

   /* NO STALE CARRY-OVER. Asserted with a DIFFERENT reading rather than a
    * cleared one: zero is also what `*m = (struct screen){0}` leaves, so a
    * build_model that copied nothing at all would pass that. A second,
    * distinct value only appears if the model is genuinely rebuilt. */
   struct reading_event ev2 = {.t          = 1700000300L,
                               .glu        = 55,
                               .trend      = -3,
                               .src        = 0,
                               .kind       = KIND_CGM,
                               .rescale_pm = 1000,
                               .prime      = -1};
   (void)store_record(&ev2, 0);
   build_model(&m);
   ck(m.reading.glu == 55 && m.reading.trend == -3 &&
          m.reading.t == 1700000300L,
      "a new reading replaces the previous one in the next model");

   /* ---- WHAT pancra_glucose ANSWERS, which is what the CGM driver acts on.
    *
    * The driver marks a sensor as streaming -- clearing the failure streak
    * that notices a sensor going bad, and persisting its address as the one
    * every future launch reconnects to -- only when a reading actually
    * entered the history. That verdict comes from here, and this is the only
    * suite that can reach this function with a real store behind it: the
    * driver's own harness stubs the hook out.
    *
    * The refusals are the isolating cases. "An accepted reading returns 1"
    * proves nothing on its own; a function that returned 1 unconditionally
    * would pass it. */
   {
      /* The registry is still empty here, so src_for_link's unregistered-link
       * path resolves to the legacy source rather than deferring -- which is
       * what makes the ACCEPTED case below reachable at all. */
      int impl = pancra_glucose(0, 9999, 0, 5);
      ck(impl == 0, "a value no sensor could report is not accepted");
      int old = pancra_glucose(0, 137, 0, 65535);
      ck(old == 0, "a frame old enough to backdate the reading is not "
                   "accepted");
      /* A value distinct from everything above, so a stale current reading
       * cannot be mistaken for this one. */
      int kept = pancra_glucose(0, 142, 2, 5);
      ck(kept == 1, "a usable reading IS accepted");
      /* ...AND ONLY ONCE. "Accepted" means a row entered the append-only
       * history, not merely that the frame passed the gate: the identical
       * sample arriving again is deduplicated away, so nothing was recorded
       * and the connection that delivered it recovered nothing. */
      int again = pancra_glucose(0, 142, 2, 5);
      ck(again == 0, "...and the same sample again is a duplicate, not a "
                     "second record");
   }

   /* THE GATE OUTRANKS EVERYTHING. It is shown when the app cannot run
    * (permissions, Bluetooth off); a menu must not paint over it. */
   g_gate = 1;
   nav_go(SCR_SETTINGS);
   build_model(&m);
   ck(m.scr == SCR_GATE, "the gate screen wins over any open menu");
   g_gate = 0;

   /* THE BACK KEY MUST EXIST WHEREVER A MODAL DOES. A screen with no back
    * code leaves the X as the only way out -- which is how the three WEIGHT
    * screens shipped. */
   int bix = 0;
   for (int i = 0; i < n; i++) {
      /* SCR_MAIN is "no modal open" and SCR_GATE is the first-run rationale,
       * which has nowhere behind it -- neither has a back code by design. */
      if (all_screens[i] == SCR_MAIN || all_screens[i] == SCR_GATE)
         continue;
      nav_go(all_screens[i]);
      if (menu_back_code(&bix) < 0) {
         printf("  FAIL: screen %d has no back code\n", (int)all_screens[i]);
         g_fail = 1;
      }
   }
   /* ...and the code is the RIGHT one, not merely present: "every screen
    * returns something" would still pass with two of them swapped, which is a
    * back key that walks you to the wrong screen. */
   nav_go(SCR_SETTINGS);
   ck(menu_back_code(&bix) == MA_CLOSE,
      "SETTINGS backs out to the main screen");
   nav_go(SCR_WTLOG);
   ck(menu_back_code(&bix) == MA_WTLOG_BACK, "the weight log backs to its own");
   nav_go(SCR_SENSOR);
   ck(menu_back_code(&bix) == MA_SENSOR_BACK,
      "a sensor screen backs to devices");
   nav_go(SCR_MAIN);
   ck(menu_back_code(&bix) < 0,
      "with no modal open there is nothing to back out of");

   /* ---- THE FOOD FLOW: PICKER FIRST, AND THE WAY BACK IS DERIVED ----
    *
    * The rule this pins is the one that has regressed about six times in this
    * codebase: where a screen goes when it closes is the entry BELOW it on the
    * path, never a target computed at the exit. Food is the first flow with
    * three levels (origin -> form -> picker), which is exactly where an
    * inferred target starts landing on the wrong screen.
    *
    * Driven through menu_action, not by calling nav_go directly, because the
    * dispatcher is what a tap actually reaches -- and it is where the form
    * gets pushed under the picker. A test that navigated by hand would prove
    * the navigation stack works and say nothing about the flow. */
   {
      /* From the ADD menu. */
      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      menu_action(MA_FOOD_OPEN, 0);
      ck(cur_screen() == SCR_FOODTYPE,
         "FOOD opens the picker, not the entry form");
      /* THE FORM IS ON THE PATH UNDERNEATH, unrendered. Without it the
       * picker's exit and the form's exit chase each other and nothing
       * reaches the main screen -- the failure wt_action records. */
      ck(nav_has(SCR_FOOD), "...with the entry form underneath it");
      menu_action(MA_FOODTYPE_BACK, 0);
      ck(cur_screen() == SCR_FOOD, "leaving the picker lands on the form");
      menu_action(MA_FOOD_DISCARD, 0);
      ck(cur_screen() == SCR_ADDMENU,
         "...and leaving the form returns to the ADD menu it came from");

      /* From the MAIN screen, via a pinned shortcut. The SAME two actions
       * must end up somewhere else -- which is the whole point of deriving
       * the target rather than naming it. */
      nav_go(SCR_MAIN);
      menu_action(MA_FOOD_OPEN, 0);
      ck(cur_screen() == SCR_FOODTYPE, "the pinned button opens the picker");
      menu_action(MA_FOODTYPE_BACK, 0);
      menu_action(MA_FOOD_DISCARD, 0);
      ck(cur_screen() == SCR_MAIN,
         "...and the same exit returns to the MAIN screen instead");
   }

   /* ---- NEW FOOD: NAMED AND CHOSEN IN ONE STEP ----
    *
    * Naming a food IS picking it -- nobody types PORRIDGE in order to then go
    * and find PORRIDGE in a list -- so the commit has to land on the entry
    * form with the type set, not back on the picker with the work still to
    * do. And the refusal path matters as much as the success one: a name the
    * format cannot hold must leave the text on the keypad rather than
    * discarding what the user typed.
    *
    * Driven through label_commit, which is what MA_KP_OK reaches. */
   {
      char fdir[256];
      snprintf(fdir, sizeof fdir, "%s", test_dir());
      if (!food_paths(fdir)) {
         printf("  FAIL: food_paths did not fit\n");
         g_fail = 1;
      }
      (void)unlink(food_path());
      (void)unlink(food_types_path());
      (void)food_load();

      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      menu_action(MA_FOOD_OPEN, 0);
      menu_action(MA_FOODTYPE_NEW, 0);
      ck(cur_screen() == SCR_LABEL, "NEW FOOD opens the letter keypad");

      forms_kp_seed("");
      forms_kp_type('T');
      forms_kp_type('O');
      forms_kp_type('A');
      forms_kp_type('S');
      forms_kp_type('T');
      ck(label_commit() == COMMIT_DONE, "the name commits");
      ck(food_type_count() == 1, "...and joins the vocabulary");
      ck(cur_screen() == SCR_FOOD,
         "...landing on the entry form, not back on the picker");
      {
         struct forms_view fv;
         forms_view_get(&fv);
         ck(fv.food_type != FOOD_TYPE_NONE,
            "...with the new food already chosen");
         ck(strcmp(food_type_name(fv.food_type), "TOAST") == 0,
            "...and it is the one that was typed");
      }

      /* THE SAME NAME AGAIN IS THE SAME FOOD. food_type_add owns that rule;
       * what is checked here is that the form ends up pointing at the
       * existing id rather than the vocabulary growing a duplicate. */
      menu_action(MA_FOODTYPE_NEW, 0);
      forms_kp_seed("");
      forms_kp_type('T');
      forms_kp_type('O');
      forms_kp_type('A');
      forms_kp_type('S');
      forms_kp_type('T');
      ck(label_commit() == COMMIT_DONE, "typing a known name commits");
      ck(food_type_count() == 1, "...without adding a second TOAST");

      /* A REFUSED NAME KEEPS THE KEYPAD OPEN. An empty one is the reachable
       * case -- the comma the format also refuses is not on this keypad. */
      menu_action(MA_FOODTYPE_NEW, 0);
      forms_kp_seed("");
      ck(label_commit() == COMMIT_DONE, "an empty name is handled");
      ck(cur_screen() == SCR_LABEL,
         "...and the keypad stays open rather than silently discarding it");
      ck(food_type_count() == 1, "...with nothing added");
      nav_go(SCR_MAIN);
   }

   /* ---- THE LOG FOOD FORM COMMITS, AND REFUSES VISIBLY ----
    *
    * The two refusals are the point. food_append is the authority on what an
    * entry may be, but it is reached only after the user has left the screen
    * that could fix the problem -- so the form checks what it can name and
    * STAYS. A CONFIRM that silently does nothing, or that navigates away
    * having written nothing, are the two failures worth pinning. */
   {
      char fdir[256];
      snprintf(fdir, sizeof fdir, "%s", test_dir());
      (void)food_paths(fdir);
      (void)unlink(food_path());
      (void)unlink(food_types_path());
      (void)food_load();

      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      menu_action(MA_FOOD_OPEN, 0);
      menu_action(MA_FOODTYPE_BACK, 0); /* to the form, no food chosen */
      ck(cur_screen() == SCR_FOOD, "the form is open");

      /* NO FOOD: refused, and the form stays so it can be fixed.
       *
       * THE MESSAGE IS THE ASSERTION, not just the refusal. food_append would
       * refuse a FOOD_TYPE_NONE entry anyway, so deleting the form's own
       * check changes no count and no screen -- measured: that mutant
       * survived everything else here. What it changes is what the user is
       * told, and "FOOD NOT SAVED" for a form nobody has chosen a food on
       * describes a write failure that did not happen. */
      set_status("");
      menu_action(MA_FOOD_CONFIRM, 0);
      ck(food_count() == 0, "CONFIRM with no food chosen writes nothing");
      ck(cur_screen() == SCR_FOOD, "...and stays on the form");
      ck(strcmp(model_status_buf(), "CHOOSE A FOOD FIRST") == 0,
         "...and says what is missing, not that a write failed");

      /* A food, but no portion. */
      menu_action(MA_FOODTYPE_NEW, 0);
      forms_kp_seed("");
      forms_kp_type('R');
      forms_kp_type('I');
      forms_kp_type('C');
      forms_kp_type('E');
      (void)label_commit();
      ck(cur_screen() == SCR_FOOD, "naming a food returns to the form");
      set_status("");
      menu_action(MA_FOOD_CONFIRM, 0);
      ck(food_count() == 0, "CONFIRM with no grams writes nothing");
      ck(cur_screen() == SCR_FOOD, "...and stays on the form");
      ck(strcmp(model_status_buf(), "ENTER HOW MANY GRAMS") == 0,
         "...and names the field that is empty");

      /* GRAMS through the keypad, which is the only way in. */
      menu_action(MA_FOOD_EDIT, 1);
      ck(cur_screen() == SCR_KEYPAD, "the GRAMS row opens the keypad");
      ck(forms_kp_mode() == KP_FOOD_G, "...in the food form's own mode");
      forms_kp_type('9');
      forms_kp_type('0');
      (void)kp_commit_number();
      {
         struct forms_view fv;
         forms_view_get(&fv);
         ck(fv.food_g == 90, "the portion is taken");
      }
      menu_action(MA_FOOD_CONFIRM, 0);
      ck(food_count() == 1, "a complete entry is written");
      ck(food_newest().g == 90, "...with the portion typed");
      ck(cur_screen() == SCR_ADDMENU,
         "...and the form returns to where the flow began");

      /* ---- A FAILED WRITE KEEPS THE DRAFT AND THE SCREEN ----
       *
       * This is items 136-138's rule, in a form written after them: the
       * navigation and the clear happen only after DURABLE success, or a
       * retry means re-entering everything. Without a forced failure the
       * else-branch is never taken, and a mutant that navigates away on
       * failure passes the whole suite -- measured.
       *
       * APP_FAIL_FSYNC, not APP_FAIL_WRITE: log_append writes the row with a
       * raw write(2) and makes it durable in append_finish, so the write
       * switch -- which guards write_all -- never reaches this path at all.
       * Measured: with APP_FAIL_WRITE armed the append succeeded normally.
       * The flush is the step that can fail here, and a failed flush is taken
       * back (util.c), so the file is left as long as it was and the retry
       * below is a real retry. */
      menu_action(MA_FOOD_OPEN, 0);
      menu_action(MA_FOODTYPE_BACK, 0);
      menu_action(MA_FOODTYPE_NEW, 0);
      forms_kp_seed("");
      forms_kp_type('O');
      forms_kp_type('A');
      forms_kp_type('T');
      forms_kp_type('S');
      (void)label_commit();
      menu_action(MA_FOOD_EDIT, 1);
      forms_kp_type('5');
      forms_kp_type('0');
      (void)kp_commit_number();
      {
         int before = food_count();
         setenv("APP_FAIL_FSYNC", "1", 1);
         set_status("");
         menu_action(MA_FOOD_CONFIRM, 0);
         unsetenv("APP_FAIL_FSYNC");
         ck(food_count() == before, "a failed write logs nothing");
         ck(cur_screen() == SCR_FOOD,
            "...and the form STAYS, so the draft can be retried");
         ck(strcmp(model_status_buf(), "FOOD NOT SAVED") == 0,
            "...and says so");
         struct forms_view fv;
         forms_view_get(&fv);
         ck(fv.food_g == 50, "...with the portion still in the form");
         ck(fv.food_type != FOOD_TYPE_NONE, "...and the food still chosen");
         /* AND THE RETRY WORKS, which is the whole point of keeping them. */
         menu_action(MA_FOOD_CONFIRM, 0);
         ck(food_count() == before + 1, "a retry commits the same draft");
         ck(food_newest().g == 50, "...with the portion that was typed");
      }

      /* THE TYPE ROW REOPENS THE PICKER, not a keypad: one way to choose a
       * food, not two. */
      menu_action(MA_FOOD_OPEN, 0);
      menu_action(MA_FOODTYPE_BACK, 0);
      menu_action(MA_FOOD_EDIT, 0);
      ck(cur_screen() == SCR_FOODTYPE, "the TYPE row reopens the picker");
      nav_go(SCR_MAIN);
   }

   /* ---- A FOOD DATE MOVES THE FOOD FORM'S INSTANT, NOBODY ELSE'S ----
    *
    * kp_form_of replaced a boolean that read "weight, else insulin". With a
    * third form that default is silent corruption: typing a date on the LOG
    * FOOD screen would have moved the INSULIN draft's timestamp, and neither
    * screen would show anything wrong until an insulin dose was logged at the
    * time somebody ate. */
   {
      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      menu_action(MA_FOOD_OPEN, 0);
      menu_action(MA_FOODTYPE_BACK, 0);
      struct forms_view before;
      forms_view_get(&before);
      long ins_before  = before.ins_t;
      long wt_before   = before.wt_t;
      long food_before = before.food_t;

      menu_action(MA_FOOD_EDIT, 3); /* DATE */
      ck(forms_kp_mode() == KP_FOOD_DATE, "the DATE row is the food form's");
      forms_kp_type('0');
      forms_kp_type('7');
      forms_kp_type('0');
      forms_kp_type('4');
      (void)kp_commit_datetime();

      struct forms_view after;
      forms_view_get(&after);
      ck(after.food_t != food_before, "the FOOD instant moved");
      ck(after.ins_t == ins_before,
         "...and the insulin form's did NOT, which the old boolean could not "
         "promise");
      ck(after.wt_t == wt_before, "...nor the weight form's");
      nav_go(SCR_MAIN);
   }

   /* ---- THE EXERCISE BUTTON: PRESSES ARE FREE, TIME IS WHAT COMMITS ----
    *
    * The rule lives in exercise.c and exercisetest pins it as arithmetic.
    * What is checked HERE is the wiring: that a tap reaches it, that the tick
    * writes the record, and above all that cycling through values costs
    * nothing -- which is the behaviour the whole delay exists for and the one
    * a naive implementation (write on every press) would get wrong while
    * looking perfectly correct on screen. */
   {
      char edir[256];
      snprintf(edir, sizeof edir, "%s", test_dir());
      (void)exercise_paths(edir);
      (void)unlink(exercise_path());
      (void)exercise_load();

      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      long t0 = 1700000000L;
      /* THE PRESS READS THE REAL MONOTONIC CLOCK (menu_action -> mono_s), so
       * the ticks below have to be measured from the same one. Driving them
       * from a made-up origin makes the elapsed interval negative, which
       * ex_tick correctly treats as "no time has passed" -- so the test would
       * fail against perfectly good code, for a reason that is entirely the
       * test's. */
      long m0 = 0; /* taken AFTER the presses -- see below */

      /* CYCLING PAST VALUES WRITES NOTHING. Three presses in a second reach
       * 3, having shown 1 and 2 on the way -- neither of which was ever a
       * statement about exercise. */
      menu_action(MA_EXERCISE, 0);
      menu_action(MA_EXERCISE, 0);
      menu_action(MA_EXERCISE, 0);
      /* SAMPLED AFTER THE PRESSES, not before. ex_press stamps `since` from
       * the real monotonic clock, so a second ticking over between the sample
       * and the last press eats a second of the settling period. That was
       * invisible slack at 60 s and is a fifth of the margin at 10 -- exactly
       * the kind of timing assumption that fails on a loaded machine and
       * nowhere else. Taken here, the elapsed interval below is measured from
       * at or after the stamp, never before it. */
      m0 = mono_s();
      ck(ex_count() == 0, "three presses in a row write nothing");
      {
         int lv = -1;
         int rem = -1;
         exercise_button_get(m0, &lv, &rem);
         ck(lv == 3, "...and the button shows the value reached");
         ck(rem > 0, "...with the settling period still running");
      }
      /* A TICK BEFORE THE MINUTE IS UP writes nothing either. */
      ck(exercise_button_tick(t0, m0 + EX_SETTLE_S - 1, 0) == 0,
         "a tick one second short commits nothing");
      ck(ex_count() == 0, "...and the log is still empty");

      /* AND THEN IT COMMITS, once, on its own. */
      ck(exercise_button_tick(t0, m0 + EX_SETTLE_S + 1, 0) == 1,
         "the tick past the settling mark commits");
      ck(ex_count() == 1, "...writing exactly one record");
      ck(ex_newest().level == 3, "...at the level that settled");
      /* AND NOT AGAIN. A control that re-committed every tick would fill the
       * log with one row per second, which is the failure mode of forgetting
       * to disarm. */
      ck(exercise_button_tick(t0 + 100, m0 + EX_SETTLE_S + 100, 0) == 0,
         "a later tick does not write it a second time");
      ck(ex_count() == 1, "...the log still holds one record");
      {
         int lv = -1;
         int rem = -1;
         exercise_button_get(m0 + EX_SETTLE_S + 100, &lv, &rem);
         ck(lv == 3, "the button still shows what was recorded");
         ck(rem == 0, "...and the pending bar is gone, which is the receipt");
      }

      /* CYCLING BACK TO REST IS THE CANCEL, and it must not write a 0. */
      menu_action(MA_EXERCISE, 0); /* 3 -> 0 */
      {
         int lv  = -1;
         int rem = -1;
         exercise_button_get(mono_s(), &lv, &rem);
         ck(lv == 0, "a fourth press returns to rest");
         /* AND NO PENDING BAR AT REST. The bar means "this has not been
          * written yet", and at rest there is nothing to write -- a countdown
          * ticking away under a button that will never record anything is a
          * promise the app does not keep. It is also the only place the
          * arming flag is observable in the drawing: after a commit the
          * elapsed time is past the period anyway, so a remaining that
          * ignored `armed` would still read 0 there and only go wrong
          * here. */
         ck(rem == 0, "...and shows no pending bar, having nothing pending");
      }
      ck(exercise_button_tick(t0 + 500, m0 + 500 + EX_SETTLE_S, 0) == 0,
         "...and no amount of waiting records the resting position");
      ck(ex_count() == 1, "...the log is unchanged");
      nav_go(SCR_MAIN);
   }

   /* ---- THE FOOD LOG OPENS AND RETURNS TO WHERE IT WAS OPENED FROM ---- */
   {
      nav_go(SCR_MAIN);
      nav_go(SCR_ADDMENU);
      menu_action(MA_FOODLOG_OPEN, 0);
      ck(cur_screen() == SCR_FOODLOG, "VIEW FOOD LOG opens the table");
      menu_action(MA_FOODLOG_BACK, 0);
      ck(cur_screen() == SCR_ADDMENU, "...and returns to the ADD menu");
      /* And from the main screen, pinned: the SAME exit lands elsewhere,
       * which is the whole point of deriving the target from the path. */
      nav_go(SCR_MAIN);
      menu_action(MA_FOODLOG_OPEN, 0);
      menu_action(MA_FOODLOG_BACK, 0);
      ck(cur_screen() == SCR_MAIN, "...or to the main screen, as pinned");
      nav_go(SCR_MAIN);
   }

   /* ---- THE DRIVER LOCK IS GIVEN BACK ----
    *
    * This is the test for the bug that got onto the phone: a redundant
    * `driver_lock()` left above an operation that locks itself made one
    * function acquire
    * the recursive lock twice and release it once. The depth returned to 1,
    * the owner stayed set, and the thread that ran it held the driver lock
    * for the rest of the process. Every GATT callback then spun forever --
    * the sensor connected, discovered services, and never delivered another
    * reading.
    *
    * Nothing could see it. The textual lock/unlock counts balance (the extra
    * acquisition is inside the operation), no test drove the timer, and the
    * UI
    * stayed responsive because the holder was the thread that wanted it. What
    * CAN see it is the invariant: whatever these functions do, they must not
    * return holding the lock.
    *
    * The function that actually shipped the bug (start_scan) needs a live
    * JNIEnv and cannot be called from here, which is why `make lockcheck`
    * exists as well -- it greps for the SHAPE. These cover the callers a test
    * can reach; the gate covers the ones it cannot.
    */
   {
      ck(!driver_held(), "the driver lock starts free on this thread");
      /* Each of these takes and releases the driver lock internally. */
      (void)link_for_sensor(1);
      ck(!driver_held(), "link_for_sensor gives the driver lock back");
      model_snapshot();
      ck(!driver_held(), "the pre-draw snapshot gives the driver lock back");
      pancra_link_watchdog();
      ck(!driver_held(), "the link watchdog gives the driver lock back");
      meter_sync_watchdog();
      ck(!driver_held(), "the meter watchdog gives the driver lock back");
      pancra_reconcile_tick();
      ck(!driver_held(), "the reconcile tick gives the driver lock back");
      /* ...and the balance survives NESTING, which is the whole reason the
       * lock is recursive: a driver operation runs another inside itself all
       * the time. This is asserted through a public operation that really
       * does nest -- driver_snapshot takes the lock once and calls
       * driver_session_of, which takes it again, LINK_MAX times -- rather
       * than by taking the lock here. The lock is private to dexdriver.c
       * now -- `make lockcheck` refuses it in any header -- and a test that
       * reached for it would be the first caller outside the driver to do so
       * again. */
      struct dex_session snap[LINK_MAX];
      struct dex_cal snapcal;
      driver_snapshot(snap, LINK_CGM, &snapcal);
      ck(!driver_held(), "a nested driver operation gives the lock back");
      driver_snapshot(snap, -1, NULL);
      ck(!driver_held(), "...however it is called");
   }

   /* ---- A THRESHOLD IS SET AS A PAIR ----
    *
    * The four thresholds are two ORDERED pairs, and a low above its high is
    * an alarm that can never stop. The rule used to live inside the keypad's
    * commit branch, which read the partner under alarm_lock, RELEASED it,
    * validated, and took it again to write -- so the pair it approved was not
    * necessarily the pair it stored. alarm_set_threshold owns the rule and
    * does the whole thing in one critical section; this pins the rule.
    */
   {
      /* A WRITABLE PATH, because alarm_set_threshold reports whether the
       * file was replaced now (TH_NOT_SAVED) -- these two numbers decide
       * whether a hypo alarm can fire, so "stored but not written" is not
       * something the user may be told is fine. With no path set every call
       * would answer TH_NOT_SAVED and this case would be testing the
       * fixture. THIS SUITE'S OWN TREE, not "/tmp" as it used to be: modeltest
       * is in the plain, the ASan and the TSan list, so all three were writing
       * /tmp/settings.cfg at once -- and writing into a directory shared with
       * every other process on the machine besides. */
      settings_paths(test_dir());
      (void)settings_store_thresholds(70, 300, 85, 250);
      /* A COPY: the live aggregate is private to settings.c now, because a
       * pointer into it can be read incoherently and rewritten under the
       * reader. Re-taken after each call, since each one changes it. */
      struct prefs mp;
      /* isnudge, islow */
      ck(alarm_set_threshold(0, 1, 80) == TH_OK,
         "a low inside the pair is set");
      settings_get(&mp);
      ck(mp.alarm_low == 80, "...and stored");
      ck(alarm_set_threshold(0, 1, 400) == TH_ORDER,
         "a low ABOVE the high is refused");
      settings_get(&mp);
      ck(mp.alarm_low == 80, "...and stores nothing");
      ck(alarm_set_threshold(0, 0, 50) == TH_ORDER,
         "a high BELOW the low is refused too");
      settings_get(&mp);
      ck(mp.alarm_high == 300, "...and stores nothing");
      ck(alarm_set_threshold(0, 0, 0) == TH_HIGH_ZERO,
         "a HIGH of zero is refused: it alarms on every reading forever");
      ck(alarm_set_threshold(0, 1, 0) == TH_OK,
         "...but a LOW of zero is the documented off switch");
      ck(alarm_set_threshold(0, 1, AL_ENTRY_MAX + 1) == TH_TOO_BIG,
         "a value past the entry maximum is refused");
      /* The NUDGE pair is validated against ITS OWN partner, not the alarm's
       * -- they are separate bands and one used to be checked against the
       * other by whichever branch happened to run. */
      /* Only the alarm low moves; the other three keep the values the
       * fixture above established. */
      settings_get(&mp);
      (void)settings_store_thresholds(70, mp.alarm_high, mp.nudge_low,
                                      mp.nudge_high);
      ck(alarm_set_threshold(1, 1, 240) == TH_OK,
         "a nudge low below the NUDGE high is accepted");
      settings_get(&mp);
      ck(mp.nudge_low == 240, "...and stored");
      ck(alarm_set_threshold(1, 1, 260) == TH_ORDER,
         "...while one above the nudge high is refused");
      (void)settings_store_thresholds(70, 300, 85, 250);
   }

   /* ---- AN INDEXED ACTION CARRIES ITS INDEX ----
    *
    * Nothing tested this, and the gap was expensive. When the touch codes
    * stopped being base+index and grew a separate index field, four call
    * sites still derived the index by subtracting a bare LITERAL base --
    * `action - 100` for a keypad digit, `action - 10` for a permission,
    * `action - 200` for a device pick. With `action` now always equal to the
    * base, every one of them computed ZERO.
    *
    * uitest could not see it: it checks the hit boxes the RENDERER records,
    * and those were right. The renderer said "digit, 5" and the shell heard
    * "digit" and threw the 5 away. It took a phone: tapping 8 then 5 on the
    * ALARM LOW keypad typed "00".
    *
    * So this drives menu_action the way a finger does, and asserts on what
    * the entry buffer actually holds. */
   {
      nav_home();
      nav_go(SCR_KEYPAD);
      forms_kp_mode_set(KP_ALARM_LOW); /* a plain numeric entry */
      forms_kp_clear();
      menu_action(MA_DIGIT, 8);
      menu_action(MA_DIGIT, 5);
      char typed[64];
      forms_kp_text(typed, sizeof typed);
      ck(forms_kp_len() == 2 && typed[0] == '8' && typed[1] == '5',
         "two digit taps type those two digits");
      if (!(forms_kp_len() == 2 && typed[0] == '8' && typed[1] == '5'))
         printf("  (entry is \"%s\")\n", typed);
      /* Every digit, so a wrong index cannot hide in the one that is 0. */
      int okd = 1;
      for (int d = 0; d <= 9; d++) {
         forms_kp_clear();
         menu_action(MA_DIGIT, d);
         char one[8];
         forms_kp_text(one, sizeof one);
         if (forms_kp_len() != 1 || one[0] != (char)('0' + d))
            okd = 0;
      }
      ck(okd, "...and every digit 0-9 types itself");
      /* An index outside the keypad must be refused rather than written. */
      forms_kp_clear();
      menu_action(MA_DIGIT, 99);
      ck(forms_kp_len() == 0, "an out-of-range digit index types nothing");
      forms_kp_clear();
      nav_home();
   }

   /* ---- THE NAVIGATION PATH ----
    *
    * This replaced eight hand-maintained "where did I come from" globals, so
    * it is worth pinning the properties they kept getting wrong. Every case
    * below is one that used to be a condition somebody had to remember to
    * write, and each of them has been a real bug in this app.
    */
   nav_home();
   ck(cur_screen() == SCR_MAIN, "the path starts at the main screen");
   ck(!nav_has(SCR_DEVICES), "...with nothing else on it");

   /* 1. A CLOSE RETURNS TO THE OPENER, whichever opener it was. The same
    *    screen is reachable from the settings list and from the main screen's
    *    own info block, and the back key has to differ. */
   nav_go(SCR_SETTINGS);
   nav_go(SCR_DEVICES);
   nav_go(SCR_SENSOR);
   nav_back();
   ck(cur_screen() == SCR_DEVICES, "a sensor opened from DEVICES backs there");
   nav_home();
   nav_go(SCR_SENSOR); /* straight off the main screen this time */
   nav_back();
   ck(cur_screen() == SCR_MAIN, "...and opened from the main screen, there");

   /* 2. RE-ENTERING A SCREEN FROM ITS OWN SUB-SCREEN IS A RETURN, not a new
    *    opening. This is the whole of what the "capture the origin only on
    *    external entry" conditions existed for: without it, SCR_SENSOR would
    *    record SCR_CAL as its origin and closing it would go back down into
    *    the sub-screen it just came out of -- a loop with no way out. */
   nav_home();
   nav_go(SCR_DEVICES);
   nav_go(SCR_SENSOR);
   nav_go(SCR_CAL);
   nav_go(SCR_SENSOR); /* the sub-screen's own way back */
   ck(cur_screen() == SCR_SENSOR, "re-entering an open screen lands on it");
   ck(!nav_has(SCR_CAL), "...and drops what was above it");
   nav_back();
   ck(cur_screen() == SCR_DEVICES,
      "...leaving the ORIGINAL origin intact, not the sub-screen");

   /* 3. A MARK RETURNS TO WHERE A MULTI-STEP FLOW BEGAN, however many screens
    *    it went through and whichever route it took in. */
   nav_home();
   nav_go(SCR_ADDMENU);
   {
      int mark = nav_mark();
      nav_go(SCR_SENSTYPE);
      nav_go(SCR_KEYPAD);
      nav_go(SCR_DEVLIST);
      nav_return_to(mark);
      ck(cur_screen() == SCR_ADDMENU, "a flow aborts to where it began");
   }
   /* ...and by the route that SKIPS the type picker, which is why this is a
    * depth and not a screen name. */
   nav_home();
   nav_go(SCR_ADDMENU);
   {
      int mark = nav_mark();
      nav_go(SCR_KEYPAD);
      nav_return_to(mark);
      ck(cur_screen() == SCR_ADDMENU, "...by the shorter route too");
   }

   /* 4. THE PATH IS BOUNDED, and overflowing it must not strand the user: the
    *    root has to survive so the main screen is always reachable. */
   nav_home();
   for (int i = 0; i < 40; i++)
      nav_go((enum ui_screen)(SCR_SETTINGS + (i % (SCR_N - SCR_SETTINGS))));
   ck(nav_has(SCR_MAIN), "the main screen survives an overflowing path");
   for (int i = 0; i < NAV_MAX + 4; i++)
      nav_back();
   ck(cur_screen() == SCR_MAIN, "...and enough backs always reach it");

   /* 5. The crash handler reads the current screen through a POINTER, so the
    *    mirror it points at must track the path. A stale one reports the
    *    wrong screen in every crash report after the first navigation. */
   nav_home();
   nav_go(SCR_REMOTE);
   ck(g_screen_now == SCR_REMOTE, "the crash mirror follows the path");
   nav_back();
   ck(g_screen_now == SCR_MAIN, "...in both directions");

   nav_home();

   printf("== a frame is not a window onto another thread's buffer ==\n");
   /* THE RACE, run for real. sync_report -- on Java's push worker -- writes
    * the last sync reply into a module-owned buffer, and the frame used to
    * BORROW that pointer, which makes an "immutable frame" a window onto
    * memory another thread is rewriting. A renderer reading across the write
    * gets a mixture of two replies, or a string whose terminator has not
    * landed yet. */
   {
      pthread_t th;
      atomic_store(&rs_stop, 0);
      if (pthread_create(&th, NULL, rs_writer, NULL) != 0) {
         ck(0, "the sync-worker stand-in started");
         return 1;
      }
      /* ONE frame, read MANY times -- which is what a frame is for. It is
       * built on the main thread and then rendered from, glyph by glyph,
       * while the rest of the app carries on. The property under test is the
       * one struct screen claims: once built, it does not change. A borrowed
       * pointer into a buffer the sync worker rewrites breaks that, and no
       * amount of care in the renderer can fix it. */
      /* WAIT FOR THE WRITER TO ACTUALLY BE WRITING. pthread_create returns
       * long before the new thread runs, and a frame built in that window
       * holds SYNC_IDLE -- which then fails the assertions below for a reason
       * that has nothing to do with the property under test. Hoping (a fixed
       * number of frames) was the previous version and it was flaky; this is
       * a handoff, and it has a bound so a thread that never starts fails
       * with a name rather than hanging. */
      int waited = 0;
      while (atomic_load(&rs_writes) == 0 && waited < 500000) {
         sched_yield();
         waited++;
      }
      ck(atomic_load(&rs_writes) > 0, "the sync-worker stand-in is reporting");
      struct screen probe;
      build_model(&probe);
      for (int i = 0; i < 1000 && probe.sync.remote_outcome == SYNC_IDLE; i++)
         build_model(&probe);
      int torn = 0;
      /* VOLATILE, or there is no test here. The bytes are read repeatedly
       * with nothing else touching them as far as the compiler can see, so at
       * -O2 it hoists the load out of the loop and the walk observes a single
       * cached value however long it runs. */
      volatile const char *s = (volatile const char *)probe.sync.remote_status;
      const char *want       = sync_outcome_label(probe.sync.remote_outcome);
      /* THE LABEL AND THE CODE AGREE, and both belong to the same report.
       * The frame carries a code and derives the label from it, so what used
       * to be a window onto a buffer another thread rewrites is now a
       * pointer to a string literal -- but only as long as the label is
       * derived from the frame's OWN copy of the code. Deriving it from the
       * module's live value instead would put the tear straight back. */
      for (int i = 0; i < 2000000 && !torn; i++) {
         if (!s || !s[0])
            continue;
         for (int k = 0;; k++) {
            if (s[k] != want[k]) {
               torn = 1;
               break;
            }
            if (!want[k])
               break;
         }
      }
      /* AND THE SAME PROPERTY ACROSS MANY FRAMES, which catches the subtler
       * version: a frame that copies the code under the lock and then derives
       * its label from the module's LIVE value -- two reads of a moving
       * target, agreeing almost always and disagreeing exactly while a user
       * watches a sync change state. */
      int disagreed = 0;
      for (int i = 0; i < 20000 && !disagreed; i++) {
         struct screen f;
         build_model(&f);
         if (f.sync.remote_status != sync_outcome_label(f.sync.remote_outcome))
            disagreed = 1;
      }
      /* THE WORKER IS STOPPED AND JOINED. It used to be left running: the
       * test went on to other cases, and eventually main() returned, with a
       * thread still calling into the model. Everything after this point was
       * therefore sharing state with a writer nobody was accounting for --
       * and any failure it caused would have been attributed to whatever case
       * happened to be running. */
      long wrote_by_end = atomic_load(&rs_writes);
      atomic_store(&rs_stop, 1);
      pthread_join(th, NULL);
      ck(!torn, "every frame holds ONE whole status, never a mixture");
      ck(!disagreed, "...and its words are the words of ITS code, not of "
                     "whatever the worker has reported since");
      ck(probe.sync.remote_outcome == SYNC_OK ||
             probe.sync.remote_outcome == SYNC_TIMEOUT,
         "...and it is one of the outcomes actually reported");
      /* THE THREADS REALLY DID OVERLAP. Without this the case passes just as
       * happily when the worker finished before the first frame was built --
       * which is not a concurrency test, it is a sequence. */
      ck(wrote_by_end > 1000,
         "...while the worker was reporting throughout (it reported "
         "thousands of times during the reads)");
   }

   printf("== the workflow follows the DEVICE, not the row ==\n");
   /* THE WHOLE POINT OF CARRYING AN ID. A device workflow is several taps
    * seconds apart -- open the device, then DISCONNECT, then confirm -- and
    * a sensor minted or forgotten on a binder thread in between moves every
    * row after it. Held as an INDEX, the confirmation acted on whoever had
    * slid into that position while the screen went on naming the device the
    * user chose. This drives it the way a finger does and shifts the
    * registry underneath, which is the case no amount of checking the
    * spelling of an argument can catch. */
   {
      sensors_paths(test_dir());
      (void)remove(sensors_path());
      (void)remove(slots_path());
      (void)sensors_load();

      int one = sensor_mint(SENSOR_STELO, "D0:00:00:00:00:01", "", "", "", 10);
      int two = sensor_mint(SENSOR_STELO, "D0:00:00:00:00:02", "", "", "", 20);
      int three =
          sensor_mint(SENSOR_STELO, "D0:00:00:00:00:03", "", "", "", 30);
      sensor_claim_slot(one, SENSOR_STELO, "D0:00:00:00:00:01");
      sensor_claim_slot(two, SENSOR_STELO, "D0:00:00:00:00:02");
      sensor_claim_slot(three, SENSOR_STELO, "D0:00:00:00:00:03");

      /* The frame the user is looking at. */
      struct screen probe;
      nav_home();
      model_snapshot();
      build_model(&probe);
      ck(probe.dev.nsensors == 3, "three devices are drawn");

      /* The tap: DOWN on row 2, which is the third device, then UP on it.
       * The row becomes a device at the DOWN, which is the whole point. */
      int val = -1;
      input_arm_row(MA_SENSOR, 2);
      ck(input_row_value(MA_SENSOR, 2, &val) && val == three,
         "the row the finger landed on IS the third device");
      menu_action(MA_SENSOR, val);
      ck(cur_screen() == SCR_SENSOR, "the per-device screen opens");
      ck(sel_device() == three, "...and that device is selected");

      /* ...and now a forget on another thread shifts everything up. */
      sensor_forget(one);
      model_snapshot();
      build_model(&probe);
      ck(probe.dev.nsensors == 2, "a device is gone");
      ck(probe.dev.sel == 1, "the selection follows its device to its NEW row");
      ck(probe.dev.sensors[probe.dev.sel].id == three,
         "...which is still the device the user opened");

      /* The confirmation, arriving after the shift. It must retire the
       * device that was picked -- not the one now sitting where it was. */
      menu_action(MA_FORGET, 0);
      ck(cur_screen() == SCR_FORGET, "DISCONNECT asks first");
      menu_action(MA_FORGET_YES, 0);
      struct sensor_slot sl;
      ck(sensor_slot_of(three, &sl) && sl.old,
         "the device the user chose is the one disconnected");
      ck(sensor_slot_of(two, &sl) && !sl.old,
         "...and the one that took its row is untouched");
   }

   printf("== a redraw between touch-down and touch-up ==\n");
   /* THE GESTURE IS NOT AN INSTANT. The press ARMS a control and the action
    * fires on the RELEASE; in between, the 1 Hz timer redraws. A mint or a
    * forget on a binder thread in that window renumbers every row after it,
    * and the release lands on the same row NUMBER -- which is a different
    * device. The tap then opened, or made primary, a sensor the user never
    * pointed at, and the screen had named the right one all along. */
   {
      sensors_paths(test_dir());
      (void)remove(sensors_path());
      (void)remove(slots_path());
      (void)sensors_load();
      int a1 = sensor_mint(SENSOR_STELO, "E0:00:00:00:00:01", "", "", "", 10);
      int b1 = sensor_mint(SENSOR_STELO, "E0:00:00:00:00:02", "", "", "", 20);
      int c1 = sensor_mint(SENSOR_STELO, "E0:00:00:00:00:03", "", "", "", 30);
      sensor_claim_slot(a1, SENSOR_STELO, "E0:00:00:00:00:01");
      sensor_claim_slot(b1, SENSOR_STELO, "E0:00:00:00:00:02");
      sensor_claim_slot(c1, SENSOR_STELO, "E0:00:00:00:00:03");
      model_snapshot();

      /* DOWN on row 1 -- the second device. */
      int val = -1;
      input_arm_row(MA_SENSOR, 1);
      ck(input_row_value(MA_SENSOR, 1, &val) && val == b1,
         "with no redraw, the release fires on the device that was pressed");

      /* ...and now the registry shifts and the frame is rebuilt while the
       * finger is still down. Row 1 is a different sensor. */
      sensor_forget(a1);
      model_snapshot();
      ck(!input_row_value(MA_SENSOR, 1, &val),
         "a row that changed device between DOWN and UP does not fire");

      /* The same for the PRIMARY checkbox, which is the other action whose
       * index is a row -- and the one that would silently move the big
       * number to a device the user never picked. */
      model_snapshot();
      input_arm_row(MA_PRIM_PICK, 1);
      ck(input_row_value(MA_PRIM_PICK, 1, &val) && val == c1,
         "PRIMARY fires on the device under the finger");
      sensor_forget(b1);
      model_snapshot();
      ck(!input_row_value(MA_PRIM_PICK, 1, &val),
         "...and not on whoever replaced it");

      /* An action whose index is a VALUE, not a row, is untouched by any of
       * this: a digit is a digit however the sensor table moves. */
      input_arm_row(MA_DIGIT, 7);
      ck(input_row_value(MA_DIGIT, 7, &val) && val == 7,
         "an ordinary index still means what it says");
   }

   printf("== logging a new weight after editing one ==\n");
   /* A FORM THAT WAS OPENED FOR AN EDIT MUST NOT STILL NAME THAT ROW.
    *
    * The weight form carries two things that decide what CONFIRM does: an
    * `edit` index and a COPY of the row being edited, which is the key
    * weight_update matches on. They were two loose globals beside a dozen
    * others, and opening the form for a NEW entry set the index and said
    * nothing about the copy -- so the copy went on naming whatever was edited
    * last, and the whole workflow was safe only for as long as every reader
    * remembered to test the index first. That is an invariant spread across a
    * dozen branches and checked by nothing.
    *
    * The failure it invites is the worst kind this app has in a log: not a
    * crash, but LOG WEIGHT quietly rewriting a previous weigh-in instead of
    * recording today's, which loses a number the user cannot recover. So this
    * drives the two taps in the order that would do it -- edit an old row,
    * then open LOG WEIGHT and confirm -- and asserts on the log itself: the
    * new weight is an EXTRA row, and the edited one still says what it said.
    */
   {
      weight_paths(test_dir());
      (void)remove(weight_path());
      (void)weight_load();
      ck(weight_append(1700000000L, 70000L, 0) == 0 &&
             weight_append(1700003600L, 71000L, 0) == 0,
         "two weigh-ins are on the log to start with");
      ck(wt_count() == 2, "...and only those two");

      nav_home();
      menu_action(MA_WTLOG_EDIT, 0); /* the OLDER of the two */
      struct forms_view fv;
      forms_view_get(&fv);
      ck(fv.wt_edit == 0 && fv.wt_orig.t == 1700000000L &&
             fv.wt_orig.g == 70000L,
         "editing a row names THAT row as the rewrite's key");

      /* ...and now LOG WEIGHT, which is a NEW entry and must inherit
       * nothing. */
      menu_action(MA_WT_OPEN, 0);
      forms_view_get(&fv);
      ck(fv.wt_edit < 0, "a new weight form is not editing anything");
      ck(fv.wt_orig.t == 0 && fv.wt_orig.g == 0,
         "...and holds no previous row as its rewrite key");

      /* The tap that writes. MA_WT_OPEN opens the keypad ON TOP of the form,
       * so the form is already on the path and this returns to it. */
      nav_go(SCR_WEIGHT);
      menu_action(MA_WT_CONFIRM, 0);
      ck(wt_count() == 3, "CONFIRM on a new form APPENDS a third weigh-in");
      ck(wt_at(0).t == 1700000000L && wt_at(0).g == 70000L,
         "...and the row that was edited a moment ago is untouched");
      ck(wt_at(1).t == 1700003600L && wt_at(1).g == 71000L,
         "...as is the other one");
      nav_home();
   }

   /* ==== THE SAVED STATE AN ACTIVITY OR PROCESS RECREATION HANDS BACK ====
    *
    * Android kills this app for memory while a dose is half typed, then puts
    * the user back in front of a task that looks exactly as if it had been
    * resumed. Before item 106 the shell took neither half of the framework's
    * offer -- `(void)saved; (void)saved_size;` at onCreate, and no
    * onSaveInstanceState callback at all -- so the screen, the route and the
    * draft were simply gone, with nothing anywhere to say so.
    *
    * THE REFUSALS ARE THE INTERESTING HALF. A blob this build wrote a moment
    * ago will of course decode; a function that returned LOAD_OK for
    * everything would pass that. What has to be shown is that a blob from
    * another version, a blob that has been damaged, and a blob describing a
    * screen about a device that no longer exists are all REFUSED, and that a
    * refusal leaves the app exactly where a fresh launch would.
    *
    * Every case below is built from the SAME valid blob with ONE field
    * changed, so nothing but the rule under test can be what refused it.
    * Where that is not achievable the comment says so rather than implying
    * a pin that is not there. */
   {
      /* The fixed head of a blob, agreeing with the settings this process
       * actually has -- so the unit check (which is the point of validating
       * after the durable loads) passes for every case except the one that
       * deliberately breaks it. */
      struct prefs stp;
      settings_get(&stp);

      char valid[STATE_MAX + 64];
      int vlen = snprintf(valid, sizeof valid,
                          "v%d %d %d 3 %d %d %d %d %d %ld %d %ld %d %d %s",
                          STATE_VERSION, stp.units, stp.wunits, (int)SCR_MAIN,
                          (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                          (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(vlen > 0 && vlen < STATE_MAX,
         "the reference blob fits inside the byte cap it is bounded by");

      struct saved_state sst;
      ck(state_decode(valid, (size_t)vlen, &sst) == LOAD_OK,
         "the reference blob is accepted, or every refusal below proves "
         "nothing");

      /* ---- REFUSALS -------------------------------------------------
       *
       * ST_BAD rebuilds the reference blob with one field replaced. The body
       * after the units is spelled out each time so the case is readable
       * beside its assertion. */
#define ST_BAD(body_fmt, ...)                                                  \
   (snprintf(bad, sizeof bad, "v%d %d %d " body_fmt, STATE_VERSION, stp.units, \
             stp.wunits, __VA_ARGS__))
      char bad[STATE_MAX + 400];
      int bl;

      /* NOTHING SAVED is not a failure: it is what a first launch, and every
       * launch the user left from the main screen, looks like. */
      ck(state_decode(0, 0, &sst) == LOAD_ABSENT,
         "no saved blob at all is ABSENT, not corrupt");
      ck(state_decode(valid, 0, &sst) == LOAD_ABSENT,
         "a zero-length blob is ABSENT too");

      /* BOUNDED, IN ISOLATION. Padding the blob with trailing junk would be
       * refused by the entry's character rule instead, which would look like
       * a pin on the byte cap and be nothing of the kind. Spaces BETWEEN two
       * fields are skipped by the parser, so this blob is valid in every
       * respect except its length -- and only the cap can refuse it. */
      {
         char pad[400];
         int i = 0;
         for (; i < 300; i++)
            pad[i] = ' ';
         pad[i] = 0;
         bl     = snprintf(
             bad, sizeof bad, "v%d %d %d 3 %d %d %d %d %d %ld %d %ld %d %d%s%s",
             STATE_VERSION, stp.units, stp.wunits, (int)SCR_MAIN,
             (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT, (int)SCR_WEIGHT,
             1700000000L, 1624, 0L, 0, 0, pad, "1624");
         ck(bl > STATE_MAX, "the over-long case really is over-long");
         ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
            "a blob larger than the byte cap is refused WITHOUT being parsed "
            "-- the length comes from outside this process (bound rule)");
      }

      /* VERSIONED. A newer build's blob and an older one are both refused
       * WHOLE: read with the wrong field order, the route parses as a keypad
       * mode and the mode as an instant, and the app opens on the wrong
       * screen with somebody's dose in the weight field. */
      bl = snprintf(bad, sizeof bad,
                    "v%d %d %d 3 %d %d %d %d %d %ld %d %ld "
                    "%d %d %s",
                    STATE_VERSION + 1, stp.units, stp.wunits, (int)SCR_MAIN,
                    (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                    (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a NEWER version marker is refused whole (version rule)");
      bl = snprintf(bad, sizeof bad,
                    "v%d %d %d 3 %d %d %d %d %d %ld %d %ld "
                    "%d %d %s",
                    STATE_VERSION - 1, stp.units, stp.wunits, (int)SCR_MAIN,
                    (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                    (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "so is an OLDER one -- there is no partial read of a blob whose "
         "field order this build does not know (version rule)");
      /* No marker at all: the whole line is otherwise the reference blob. */
      bl = snprintf(bad, sizeof bad,
                    "%d %d 3 %d %d %d %d %d %ld %d %ld %d %d "
                    "%s",
                    stp.units, stp.wunits, (int)SCR_MAIN, (int)SCR_WEIGHT,
                    (int)SCR_KEYPAD, (int)KP_WEIGHT, (int)SCR_WEIGHT,
                    1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a blob with no version marker is refused: nothing this project "
         "shipped ever wrote one (version rule)");

      /* PRINTABLE ASCII, IN ISOLATION. The NUL replaces the LAST byte, so
       * without the scan the parser reads a shorter but perfectly legal
       * entry ("162") and answers LOAD_OK -- which is the whole danger: a
       * damaged blob that parses is worse than one that does not. */
      {
         char nul[STATE_MAX + 64];
         memcpy(nul, valid, (size_t)vlen);
         nul[vlen - 1] = 0;
         ck(state_decode(nul, (size_t)vlen, &sst) == LOAD_CORRUPT,
            "an embedded NUL is refused rather than silently truncating the "
            "blob into a different, valid one (printable rule)");
      }

      /* THE UNITS, WHICH IS WHY THIS IS VALIDATED AFTER THE DURABLE LOADS.
       * The weight draft is tenths of the DISPLAY unit, so "1624" is 162.4
       * lb or 162.4 kg depending on a preference read from settings.cfg. */
      bl = snprintf(bad, sizeof bad,
                    "v%d %d %d 3 %d %d %d %d %d %ld %d %ld "
                    "%d %d %s",
                    STATE_VERSION, stp.units, !stp.wunits, (int)SCR_MAIN,
                    (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                    (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a draft typed under the OTHER weight unit is refused: restoring it "
         "would confirm a weight wrong by a factor of 2.2, with the digits "
         "the user typed still on screen (unit rule)");
      bl = snprintf(bad, sizeof bad,
                    "v%d %d %d 3 %d %d %d %d %d %ld %d %ld "
                    "%d %d %s",
                    STATE_VERSION, !stp.units, stp.wunits, (int)SCR_MAIN,
                    (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                    (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "and so is one typed under the other GLUCOSE unit (unit rule)");

      /* THE ROUTE. Depth, root, and what each screen stands for. */
      bl = ST_BAD("0 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a zero-deep route has no current screen at all (depth floor -- "
         "NOT isolated: with the floor removed the root test reads a path "
         "entry that was never written, and refuses this too, which is why "
         "the floor is what stops that read rather than something a test "
         "can pin)");
      /* DEEPER THAN NAV_MAX, with every entry a screen that IS restorable
       * and a root that IS the main screen -- so only the depth test stands
       * between this and a write past the end of the path array. */
      bl = ST_BAD("13 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %ld %d "
                  "%ld %d %d %s",
                  (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN,
                  (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN,
                  (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN, (int)SCR_MAIN,
                  (int)SCR_MAIN, (int)KP_NONE, (int)SCR_MAIN, 0L, 0, 0L, 0, 0,
                  "-");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a route deeper than NAV_MAX is refused before a single entry is "
         "written into a path that could not hold it (depth rule)");
      /* ROOTED SOMEWHERE ELSE. Every screen here is restorable and the depth
       * is legal; what is wrong is that nav_back cannot reach the main
       * screen from it, so the user could not leave. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_SETTINGS,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a route whose root is not the main screen is refused: back would "
         "never reach home (root rule)");
      /* A SCREEN THAT STANDS FOR A DEVICE. SCR_SENSOR is about whichever
       * sensor sel_device() names, and that is a tap, not a stored fact --
       * so a restored one is a detail screen about a device the app can no
       * longer identify. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_SENSOR, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a route through a per-device screen is refused: it names a thing "
         "this process can no longer identify (restorable-screen rule)");
      /* A NUMBER THAT IS NOT A SCREEN. NOTED HONESTLY: this cannot be
       * isolated. scr_restorable's `default` refuses 99 as well, so deleting
       * the range test on its own leaves every assertion here passing. It is
       * kept because it is what makes the cast to enum ui_screen legal, and
       * the comment in main.c says the same. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN, 99,
                  (int)SCR_KEYPAD, (int)KP_WEIGHT, (int)SCR_WEIGHT, 1700000000L,
                  1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a number that is no screen at all is refused (range rule -- and "
         "the whitelist refuses it too, so nothing pins this in isolation)");

      /* A SCREEN THAT IS ABOUT A ROW OF A LOG. SCR_WTDEL is a YES/NO over
       * one weigh-in, and the row it names lives in the weight draft's
       * `orig` -- which is never saved (see forms.h), and which the reloaded
       * log may not contain any more. Restored, it is a DELETE button with
       * nothing behind it: the most dangerous kind of dangling screen,
       * because the user's tap looks like it did something. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WTLOG, (int)SCR_WTDEL, (int)KP_NONE, (int)SCR_MAIN,
                  0L, 0, 0L, 0, 0, "-");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a route ending on the CONFIRM-DELETE screen is refused: the row it "
         "would delete is not in the snapshot and may not be in the log "
         "(restorable-screen rule, the record-bound case)");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_INSLOG, (int)SCR_INSDEL, (int)KP_NONE, (int)SCR_MAIN,
                  0L, 0, 0L, 0, 0, "-");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "and so is the dose log's (restorable-screen rule)");

      /* THE KEYPAD. Which field, where it closes to, and whether there is a
       * keypad on the route to hold either. */
      /* A SHARED SECRET. The blob goes into a Bundle held by system_server
       * and, on some configurations, written to disk with the task. A
       * half-typed sensor pairing code must never be in it. This route and
       * every other field are valid; only the mode is refused. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_DEVICES, (int)SCR_KEYPAD, (int)KP_PAIR_CODE,
                  (int)SCR_DEVICES, 0L, 0, 0L, 0, 0, "1234");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a saved SENSOR PAIRING CODE is refused -- it is a shared secret "
         "and this blob leaves the process (secret-mode rule)");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_DEVICES, (int)SCR_KEYPAD, (int)KP_SYNC_CODE,
                  (int)SCR_DEVICES, 0L, 0, 0L, 0, 0, "123456");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "and so is the SERVER pairing code (secret-mode rule)");
      /* A KEYPAD THAT CLOSES ONTO A SCREEN THE ROUTE RULE JUST EXCLUDED.
       * Its X button would land exactly where the truncation was avoiding. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_SENSOR, 1700000000L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a keypad that returns to a per-device screen is refused: its X "
         "would land on the screen the route rule excluded (return rule)");
      /* A KEYPAD MODE WITH NO KEYPAD ON THE ROUTE. The blob does not agree
       * with itself; the encoder writes KP_NONE and SCR_MAIN there. */
      bl = ST_BAD("2 %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)KP_WEIGHT, (int)SCR_MAIN, 1700000000L,
                  1624, 0L, 0, 0, "-");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a keypad FIELD with no keypad on the route is a blob that does not "
         "agree with itself (self-consistency rule)");
      /* TYPED TEXT WITH NO KEYPAD TO HOLD IT, and the mode/return pair the
       * encoder really would have written -- so the rule above cannot be
       * what refuses this one. */
      bl = ST_BAD("2 %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)KP_NONE, (int)SCR_MAIN, 1700000000L,
                  1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "typed text with no keypad on the route is refused (entry-needs-a-"
         "keypad rule -- NOT isolated, and honestly so: a route with no "
         "keypad must carry KP_NONE, whose field draws zero cells, so the "
         "entry-length rule below refuses this input as well. The guard is "
         "kept because it states the reason; a mutation drill proved no "
         "assertion can fail on its removal)");
      /* THE CHARACTER SET. The digit keypads collect [0-9.] and nothing
       * else, so anything else came from somewhere that is not this app. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "16a4");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a letter in the typed entry is refused (entry character rule)");
      /* LONGER THAN THE FIELD DRAWS. KP_CALIB has three cells; four
       * characters is an entry the keypad itself could never have produced,
       * and the renderer would draw it off the end of the row. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_CALIB,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0, "1234");
      ck(kp_slots(KP_CALIB) == 3,
         "the mode used for the length case really has three cells");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "an entry longer than the field this mode draws is refused (entry "
         "length rule)");
      /* THE FIELD IS MANDATORY, so a blob cut short at the last space shows
       * up as missing rather than as an empty entry. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, 0);
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a blob with the entry field missing is refused, not read as an "
         "empty entry (mandatory-field rule)");

      /* THE DRAFT'S OWN NUMBERS, against the bounds their modules define. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, WT_T_MAX + 1L, 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a weight instant past the end of the calendar is refused");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 100000, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a weight the keypad could not hold is refused");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, INS_T_MAX + 1L, 0, 0,
                  "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "so is a dose instant past it");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 7, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "an insulin type that is neither SLOW nor FAST is refused: it would "
         "reach the marker tables as an index");
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0, INS_UNITS_MAX + 1,
                  "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "and a dose above the largest this app will log");
      /* A DIGIT RUN LONGER THAN ANY VALUE THIS FORMAT HOLDS is refused
       * rather than folded: a folded number is a plausible wrong one. */
      bl = ST_BAD("3 %d %d %d %d %d %s %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, "123456789012345678901234", 1624, 0L, 0, 0,
                  "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a twenty-four digit number is refused (and this case does not "
         "isolate the rule: folded, it would still be outside the calendar)");
      /* THE ISOLATING ONE, and it took some finding. Every numeric field
       * here is range-checked, so an over-long run usually folds to
       * something the range test refuses anyway -- which looks like a pin on
       * the digit rule and is nothing of the kind. This run is 24 digits
       * whose leading eighteen are 000000000000000012: fold it and the field
       * holds 12, a perfectly legal dose that sails through every check
       * after it. Only refusing the run itself catches it. */
      bl = ST_BAD("3 %d %d %d %d %d %ld %d %ld %d %s %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, 1700000000L, 1624, 0L, 0,
                  "000000000000000012345678", "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a digit run longer than the format holds is REFUSED, not folded "
         "into the plausible legal value hiding in its leading digits "
         "(digit-run rule, in isolation)");
      /* A FIELD THAT IS NOT A NUMBER AT ALL. */
      bl = ST_BAD("3 %d %d %d %d %d %s %d %ld %d %d %s", (int)SCR_MAIN,
                  (int)SCR_WEIGHT, (int)SCR_KEYPAD, (int)KP_WEIGHT,
                  (int)SCR_WEIGHT, "xyz", 1624, 0L, 0, 0, "1624");
      ck(state_decode(bad, (size_t)bl, &sst) == LOAD_CORRUPT,
         "a field that is not a number is refused");
#undef ST_BAD

      /* ---- A REFUSAL LEAVES THE APP AT A SAFE DEFAULT ----------------
       *
       * The three claims -- refused, refused, and restored -- are separate
       * assertions because they are separate claims. And the sentinel below
       * is written and its VALUES NAMED HERE, before the call that could
       * destroy them: an assertion that read the draft back out of the state
       * state_restore may have rewritten would be an assertion about
       * whatever it found, not about what had to survive. */
      {
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         forms_wt_restore(4242L, 777);
         forms_ins_restore(1111L, INS_FAST, 33);

         /* A CORRUPT BLOB: the reference blob with its tail chewed off, the
          * shape a truncated Bundle write really has. */
         state_restore(valid, (size_t)(vlen - 6));
         struct forms_view fv;
         forms_view_get(&fv);
         ck(cur_screen() == SCR_MAIN,
            "a CORRUPT saved state leaves the app on the main screen");
         ck(fv.wt_t == 4242L && fv.wt_tenths == 777,
            "...and does not half-apply itself over the state that is there");
         ck(fv.kp_mode == KP_NONE && fv.entrylen == 0,
            "...and opens no keypad");

         /* AN INCOMPATIBLE VERSION: a blob from a build that is not this
          * one. Same three questions, because "refused because it was
          * damaged" and "refused because it is a stranger" are different
          * claims about different code. */
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         forms_wt_restore(4242L, 777);
         forms_ins_restore(1111L, INS_FAST, 33);
         {
            char other[STATE_MAX + 64];
            int ol = snprintf(other, sizeof other,
                              "v%d %d %d 3 %d %d %d %d %d %ld %d %ld %d %d %s",
                              STATE_VERSION + 1, stp.units, stp.wunits,
                              (int)SCR_MAIN, (int)SCR_WEIGHT, (int)SCR_KEYPAD,
                              (int)KP_WEIGHT, (int)SCR_WEIGHT, 1700000000L,
                              1624, 0L, 0, 0, "1624");
            state_restore(other, (size_t)ol);
         }
         forms_view_get(&fv);
         ck(cur_screen() == SCR_MAIN,
            "an INCOMPATIBLE-VERSION saved state leaves the app on the main "
            "screen");
         ck(fv.wt_t == 4242L && fv.wt_tenths == 777, "...and changes no draft");
         ck(fv.kp_mode == KP_NONE && fv.entrylen == 0,
            "...and opens no keypad");

         /* A ROUTE ABOUT A RECORD THAT IS NOT IN THE SNAPSHOT, end to end.
          * This is the case the whole whitelist exists for: the blob is
          * well-formed, this version, the right units and a legal depth, and
          * it asks to be put back on a DELETE THIS WEIGH-IN confirmation
          * whose row was never saved and whose log has been reloaded from
          * disk since. The app must land on the main screen, not on a YES
          * button with nothing behind it. */
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         forms_wt_restore(4242L, 777);
         {
            char dangling[STATE_MAX + 64];
            int dl =
                snprintf(dangling, sizeof dangling,
                         "v%d %d %d 3 %d %d %d %d %d %ld %d %ld %d %d %s",
                         STATE_VERSION, stp.units, stp.wunits, (int)SCR_MAIN,
                         (int)SCR_WTLOG, (int)SCR_WTDEL, (int)KP_NONE,
                         (int)SCR_MAIN, 0L, 0, 0L, 0, 0, "-");
            state_restore(dangling, (size_t)dl);
         }
         forms_view_get(&fv);
         ck(cur_screen() == SCR_MAIN,
            "a saved route ending on a confirmation about a record the "
            "snapshot does not carry lands on the MAIN SCREEN, not on the "
            "dangling screen");
         ck(fv.wt_t == 4242L && fv.wt_tenths == 777,
            "...and touches no draft on the way past");

         /* AND THE VALID ONE RESTORES. Different claim, separate assertion:
          * a decoder that refused everything would pass both cases above.
          *
          * THE FORM IS PUT INTO EDIT MODE FIRST, so `wt_edit < 0` below is a
          * claim about what the restore DID rather than about what happened
          * to be true already: the draft that comes back must be a NEW entry
          * whatever the process was in the middle of. */
         menu_action(MA_WTLOG_EDIT, 0);
         forms_view_get(&fv);
         ck(fv.wt_edit >= 0,
            "the form really is editing a row before the restore, or the "
            "assertion below pins nothing");
         state_restore(valid, (size_t)vlen);
         forms_view_get(&fv);
         char typed[32];
         forms_kp_text(typed, (int)sizeof typed);
         ck(cur_screen() == SCR_KEYPAD,
            "a VALID saved state puts the user back on the screen they left");
         ck(fv.wt_t == 1700000000L && fv.wt_tenths == 1624,
            "...with the weight draft they had typed");
         ck(fv.wt_edit < 0,
            "...as a NEW entry: a restored draft may never rewrite a row the "
            "log reloaded without");
         ck(fv.kp_mode == KP_WEIGHT && fv.entrylen == 4 &&
                strcmp(typed, "1624") == 0,
            "...and the digits still in the field");
         /* THE ROUTE, not just the screen: the way out has to work too. */
         nav_back();
         ck(cur_screen() == SCR_WEIGHT,
            "closing the keypad returns to the form beneath it");
         nav_back();
         ck(cur_screen() == SCR_MAIN, "...and that returns home");
      }

      /* ---- THE ENCODER: WHAT IT REFUSES TO WRITE DOWN ----------------- */
      {
         char enc[STATE_MAX];
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         ck(state_encode(enc, (int)sizeof enc) == 0,
            "the main screen with nothing open saves NO blob: a snapshot "
            "that restores home onto home is a snapshot for nothing");

         /* A REAL ROUTE ROUND-TRIPS. This is the one case that proves the
          * two halves agree; every refusal above is about a blob this build
          * would not have written. */
         nav_go(SCR_WEIGHT);
         nav_go(SCR_KEYPAD);
         forms_wt_restore(1700000600L, 815);
         forms_kp_mode_set(KP_WEIGHT);
         forms_kp_return_set(SCR_WEIGHT);
         forms_kp_seed("815");
         int el = state_encode(enc, (int)sizeof enc);
         ck(el > 0 && el < (int)sizeof enc,
            "a route with a half-typed weight on it is worth saving");
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         forms_wt_restore(1L, 1);
         state_restore(enc, (size_t)el);
         {
            struct forms_view fv;
            char typed[32];
            forms_view_get(&fv);
            forms_kp_text(typed, (int)sizeof typed);
            ck(cur_screen() == SCR_KEYPAD && fv.kp_mode == KP_WEIGHT &&
                   fv.wt_t == 1700000600L && fv.wt_tenths == 815 &&
                   strcmp(typed, "815") == 0,
               "what the shell writes at onSaveInstanceState is what it "
               "reads back at the next onCreate");
         }

         /* AN EDIT IN PROGRESS IS TRUNCATED AWAY. The form carries a copy of
          * the row it is amending as the rewrite's match key, and that row
          * may not survive the process; the user lands on the log beneath
          * instead of on a form that says EDIT about nothing. */
         nav_home();
         nav_go(SCR_WTLOG);
         nav_go(SCR_WEIGHT);
         nav_go(SCR_KEYPAD);
         forms_kp_mode_set(KP_WEIGHT);
         forms_kp_return_set(SCR_WEIGHT);
         forms_kp_seed("700");
         menu_action(MA_WTLOG_EDIT, 0); /* puts the form INTO edit mode */
         nav_go(SCR_KEYPAD);
         el = state_encode(enc, (int)sizeof enc);
         ck(el > 0, "the route above the edit is still worth saving");
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
         state_restore(enc, (size_t)el);
         {
            struct forms_view fv;
            forms_view_get(&fv);
            ck(cur_screen() == SCR_WTLOG,
               "a form that was EDITING a row is truncated out of the saved "
               "route: the user lands on the log beneath it");
            ck(fv.kp_mode == KP_NONE && fv.entrylen == 0,
               "...and the keypad above it goes with it");
         }

         /* A SHARED SECRET IS NEVER IN THE BYTES. Not merely refused on the
          * way back in -- absent from what leaves this process. */
         nav_home();
         nav_go(SCR_DEVICES);
         nav_go(SCR_KEYPAD);
         forms_kp_mode_set(KP_PAIR_CODE);
         forms_kp_return_set(SCR_DEVICES);
         forms_kp_seed("9137");
         el = state_encode(enc, (int)sizeof enc);
         ck(el > 0, "the route below the pairing keypad is still saved");
         enc[el] = 0;
         ck(strstr(enc, "9137") == 0,
            "a half-typed PAIRING CODE never reaches the framework's Bundle "
            "(secret-mode rule, at the writing end)");
         nav_home();
         forms_kp_mode_set(KP_NONE);
         forms_kp_seed("");
      }
   }

   if (g_fail) {
      printf("modeltest: FAIL (%ld checks ran)\n", g_checks);
      return 1;
   }
   if (g_checks <= 0) {
      printf("modeltest: NO CHECKS RAN\n");
      return 1;
   }
   printf("modeltest: the model the app builds is the model it means OK "
          "(%ld checks)\n",
          g_checks);
   return 0;
}
