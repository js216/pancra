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
 * main.c is INCLUDED rather than linked, because build_model and the state it
 * reads are static -- which is right for main.c and merely means the test
 * must live inside the same translation unit. The only thing that has to be
 * stubbed is the BLE transport (the dexble_* family that pancra.h declares),
 * because nothing here touches a radio.
 */
/* Named explicitly even though main.c includes them all: this file uses these
 * declarations directly, and inheriting them through a .c include hides that
 * from anything checking (and from a reader). */
#include "dexdriver.h" /* the drv_* upcalls stubbed below */
#include "pancra.h"    /* the dexble_* transport, likewise */
#include "ui.h"        /* struct screen, SCR_*: what build_model fills in */
#include <jni.h>
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

int dexble_bond_state(const char *m)
{
   (void)m;
   return 0;
}

void dexble_reconnect(int l)
{
   (void)l;
}

void dexble_request_devinfo(void)
{
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

void dexble_set_meter_link(int l, int o)
{
   (void)l;
   (void)o;
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
void drv_connect(const char *mac)
{
   (void)mac;
}

void drv_subscribe(const char *uuid, int indicate)
{
   (void)uuid;
   (void)indicate;
}

void drv_write(const char *uuid, const uint8_t *data, int n, int no_resp)
{
   (void)uuid;
   (void)data;
   (void)n;
   (void)no_resp;
}

void drv_status(const char *s)
{
   (void)s;
}

void drv_glucose(int mg_dl, int trend, int age_s)
{
   (void)mg_dl;
   (void)trend;
   (void)age_s;
}

void drv_cal_result(int result)
{
   (void)result;
}

int drv_key_load(uint8_t key[16])
{
   (void)key;
   return 0;
}

void drv_key_save(const uint8_t key[16])
{
   (void)key;
}

void drv_backfill(int mg_dl, int trend, int age_s)
{
   (void)mg_dl;
   (void)trend;
   (void)age_s;
}

int drv_mac_load(char *mac, int n)
{
   (void)mac;
   (void)n;
   return 0;
}

void drv_key_clear(void)
{
}

void drv_mac_clear(void)
{
}

void drv_mac_save(const char *mac)
{
   (void)mac;
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

static void ck(int cond, const char *what)
{
   if (!cond) {
      printf("  FAIL: %s\n", what);
      g_fail = 1;
   }
}

/* Every screen the menu enum can hold, so a new one cannot be added without
 * appearing here. */
static const enum menu_screen all_menus[] = {
    MENU_NONE,        MENU_SETTINGS,  MENU_KEYPAD,    MENU_DEVLIST,
    MENU_SENSOR,      MENU_CAL,       MENU_CALPEND,   MENU_RESCALE,
    MENU_RESCALEACT,  MENU_SENSTYPE,  MENU_FORGET,    MENU_LABEL,
    MENU_MARKPICK,    MENU_COLORPICK, MENU_METERHELP, MENU_PAIRCONF,
    MENU_SYNCRESTORE, MENU_ADD,       MENU_INSULIN,   MENU_WEIGHT,
    MENU_WTLOG,       MENU_WTDEL,     MENU_INSLOG,    MENU_DISPLAY,
    MENU_DEVICES,     MENU_PERMS,     MENU_OLDDEV,    MENU_RECONF,
    MENU_REMOTE,      MENU_INSDEL,    MENU_ALARM,     MENU_EXPORT};

int main(void)
{
   /* EVERY MENU MUST REACH ITS OWN SCREEN. This used to be a chain of thirty
    * `else if`s ending in a catch-all, so a menu whose mapping was forgotten
    * opened and silently rendered the MAIN screen. */
   int n = (int)(sizeof all_menus / sizeof all_menus[0]);
   ck(n == MENU_EXPORT + 1, "this test lists every menu the enum defines");
   int distinct = 1;
   for (int i = 0; i < n; i++) {
      int scr = menu_to_scr(all_menus[i]);
      if (all_menus[i] != MENU_NONE && scr == SCR_MAIN) {
         printf("  FAIL: menu %d falls back to the MAIN screen\n",
                (int)all_menus[i]);
         g_fail = 1;
      }
      for (int j = 0; j < i; j++)
         if (all_menus[j] != MENU_NONE && all_menus[i] != MENU_NONE &&
             menu_to_scr(all_menus[j]) == scr)
            distinct = 0;
   }
   ck(menu_to_scr(MENU_NONE) == SCR_MAIN, "no menu open means the main screen");
   ck(distinct, "no two menus share a screen");

   /* THE BIG NUMBER IS THE CURRENT READING. Everything else on the screen is
    * context; this is the thing somebody acts on. */
   struct screen m;
   g_gate      = 0;
   g_menu      = MENU_NONE;
   g_cur_glu   = 137;
   g_cur_trend = 4;
   g_cur_time  = 1700000000L;
   build_model(&m);
   ck(m.scr == SCR_MAIN, "with no menu open the model is the main screen");
   ck(m.glu == 137, "the model carries the current glucose");
   ck(m.trend == 4, "...and its trend");
   ck(m.t == 1700000000L, "...and the time it was taken");

   /* A MENU MUST NOT DISTURB THE READING: opening one changes the screen and
    * nothing else, or the number under it goes stale without saying so. */
   g_menu = MENU_SETTINGS;
   build_model(&m);
   ck(m.scr == SCR_SETTINGS, "opening a menu selects its screen");
   ck(m.glu == 137 && m.t == 1700000000L,
      "...and leaves the reading untouched");

   /* NO STALE CARRY-OVER. Asserted with a DIFFERENT reading rather than a
    * cleared one: zero is also what `*m = (struct screen){0}` leaves, so a
    * build_model that copied nothing at all would pass that. A second,
    * distinct value only appears if the model is genuinely rebuilt. */
   g_cur_glu   = 55;
   g_cur_trend = -3;
   g_cur_time  = 1700000300L;
   build_model(&m);
   ck(m.glu == 55 && m.trend == -3 && m.t == 1700000300L,
      "a new reading replaces the previous one in the next model");

   /* THE GATE OUTRANKS EVERYTHING. It is shown when the app cannot run
    * (permissions, Bluetooth off); a menu must not paint over it. */
   g_gate = 1;
   g_menu = MENU_SETTINGS;
   build_model(&m);
   ck(m.scr == SCR_GATE, "the gate screen wins over any open menu");
   g_gate = 0;

   /* THE BACK KEY MUST EXIST WHEREVER A MODAL DOES. A screen with no back
    * code leaves the X as the only way out -- which is how the three WEIGHT
    * screens shipped. */
   for (int i = 0; i < n; i++) {
      if (all_menus[i] == MENU_NONE)
         continue;
      g_menu = all_menus[i];
      if (menu_back_code() < 0) {
         printf("  FAIL: menu %d has no back code\n", (int)all_menus[i]);
         g_fail = 1;
      }
   }
   /* ...and the code is the RIGHT one, not merely present: "every screen
    * returns something" would still pass with two of them swapped, which is a
    * back key that walks you to the wrong screen. */
   g_menu = MENU_SETTINGS;
   ck(menu_back_code() == MA_CLOSE, "SETTINGS backs out to the main screen");
   g_menu = MENU_WTLOG;
   ck(menu_back_code() == MA_WTLOG_BACK, "the weight log backs to its own");
   g_menu = MENU_SENSOR;
   ck(menu_back_code() == MA_SENSOR_BACK, "a sensor screen backs to devices");
   g_menu = MENU_NONE;
   ck(menu_back_code() < 0,
      "with no modal open there is nothing to back out of");

   if (g_fail) {
      printf("modeltest: FAIL\n");
      return 1;
   }
   printf("modeltest: the model the app builds is the model it means OK\n");
   return 0;
}
