// SPDX-License-Identifier: GPL-3.0
// menu.c --- Every tap that is not typing (see menu.h)
// Copyright 2026 Jakob Kastelic

#include "menu.h"
#include "alarm.h"
#include "calib.h"
#include "clock.h"
#include "device.h"
#include "devtag.h" /* a log may not carry an address; see there */
#include "dexdriver.h"
#include "food.h"
#include "forms.h"
#include "jbridge.h"
#include "keypad.h" /* the dot key and the slot count, once */
#include "log.h"
#include "menuview.h"
#include "meter.h"
#include "nav.h"
#include "notify.h"
#include "pairing.h"
#include "reconcile.h"
#include "remote.h" /* remote_retry_now: a fixed setting is tried NOW */
#include "remotecfg.h"
#include "scan.h"
#include "selection.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "status.h"
#include "store.h"
#include "style.h"
#include "sync.h"
#include "syncjni.h"
#include "uiact.h"
#include "uifmt.h"
#include "uimenu.h"
#include "uimodel.h"
#include "util.h"

/* EXPORT DATA menu state (session-only; the defaults are the whole point:
 * everything, all time). Range 0 = 30 D, 1 = 1 Y, 2 = ALL. */
/* The selection itself is selection.c's: see there for why it is not the
 * menu's, even though the taps that set it arrive here. */

static int g_exp_range = 2;
static int g_exp_glu   = 1;
static int g_exp_dev   = 1;
static int g_exp_ins   = 1;
static int g_exp_wt    = 1; /* EXPORT DATA: include the weight log */

static int g_old_page; /* which page the OLD DEVICES list is showing */
static int g_dev_page; /* which page the LIVE device list is showing */

static const char *perms[] = {"android.permission.BLUETOOTH_SCAN",
                              "android.permission.BLUETOOTH_CONNECT",
                              "android.permission.POST_NOTIFICATIONS"};
/* Cached system states for the settings menu. JNI via the activity's env is
 * only legal on the main thread, so sys_* are never called from a render (which
 * can be requested off a BLE binder thread); sys_refresh samples them from the
 * main thread (menu open / after an action) and build_model just copies them.
 */
/* The bucket starts UNKNOWN and stays that way until Java answers once.
 * Android's own buckets start at 5 (EXEMPT), so ui_bucket_label renders
 * anything <= 0 as "?" -- but the value must not be WRITTEN from a call that
 * threw, because the next successful query is what should replace it. */
static int g_sys_perm[NPERMS];
static int g_sys_batt;
static int g_sys_bucket = -1;
static int g_sys_bg;

/* Where the PAIRING flow (type tap -> keypad / meter help) was entered
 * from: the ADD menu or the ADD DEVICE picker. Every abort path returns
 * exactly there -- recorded, never inferred (the recurring bug). */
/* How deep the path was when the ADD-A-DEVICE flow started, so every abort
 * inside it lands where it began. See nav_mark(). */
/* How deep the navigation path was when the ADD-A-DEVICE flow began, so every
 * abort inside it lands where it started (see nav_mark()). Private: nothing
 * outside this file has ever read it, and it was exported anyway. */
static int g_pair_mark = 1;

/* --- settings-menu system ops (main thread; shell_activity()->env valid) ---
 *
 * The JNI is in jbridge.c; what is left here is the app's own policy: which
 * cutoff EXPORT DATA uses, and which of the g_exp_* checkboxes it passes. */
/* ---- WHETHER THE LAST EXPORT REACHED JAVA AT ALL ----------------------
 *
 * WHY THE ANSWER IS KEPT. Discard jb_export_data's answer and an activity
 * that has gone away, a method the bridge cannot resolve, and a Java
 * exception on the way in all look exactly like a successful export: the menu
 * stays, nothing is said, and no share sheet appears. The user's only
 * evidence is a sheet that did not open, and the
 * natural conclusion is that the app is slow -- so they tap again, and again.
 *
 * 0 = nothing attempted yet or the last attempt was accepted; 1 = the last
 * attempt did NOT reach Java. RETRYABLE and deliberately sticky: it is
 * cleared when a later attempt is accepted, and by leaving the screen (the
 * export menu's X), so the notice cannot outlive the workflow it describes.
 * Main-thread state, like everything else here. */
static int g_exp_failed;

/* 1 if Java accepted the request. THE CALLER MUST NOT claim the export
 * happened on any other answer. */
static int sys_export_data(void)
{
   /* Cutoff epoch: rows OLDER than this are left out (0 = keep all). Java
    * filters by each row's leading epoch field and keeps header lines. */
   long cutoff = 0;
   if (g_exp_range == 0)
      cutoff = realtime_s() - (30L * 86400);
   else if (g_exp_range == 1)
      cutoff = realtime_s() - (365L * 86400);
   return jb_export_data(shell_activity(), cutoff, g_exp_glu, g_exp_dev,
                         g_exp_ins, g_exp_wt);
}

/* ONE CONSISTENT COPY for the frame (see menuview.h). Everything here is
 * main-thread state and the frame is built on the main thread, so no lock --
 * what this buys is that a frame cannot see the selected slot change between
 * the row it draws and the panel below it. */
void menu_view_get(struct menu_view *out)
{
   if (!out)
      return;
   out->sel_id   = sel_device();
   out->add_type = sel_add_type();
   for (int i = 0; i < NPERMS; i++)
      out->perm[i] = g_sys_perm[i];
   out->batt_ok        = g_sys_batt;
   out->standby_bucket = g_sys_bucket;
   out->bg_restricted  = g_sys_bg;
   out->old_page       = g_old_page;
   out->dev_page       = g_dev_page;
   out->exp_range      = g_exp_range;
   out->exp_glu        = g_exp_glu;
   out->exp_dev        = g_exp_dev;
   out->exp_ins        = g_exp_ins;
   out->exp_wt         = g_exp_wt;
   out->exp_failed     = g_exp_failed;
}

/* Sample the system states the settings screen shows into the g_sys_* cache.
 * MAIN THREAD ONLY (JNI via the activity env): call on menu-open and after
 * an action, never from a render -- build_model just copies the cache. */
void sys_refresh(void)
{
   /* A CACHE KEEPS ITS LAST GOOD VALUE when Java could not answer.
    *
    * Taking a Java call's return value straight into the cache stores a
    * zeroed one whenever the call THREW -- so a revoked permission that makes
    * checkSelfPermission throw shows as "not granted" (the same as a real
    * denial), and a throwing getAppStandbyBucket shows as bucket 0, ACTIVE:
    * the most reassuring answer there is,
    * displayed as a fact on the screen a user opens precisely when something
    * is wrong. Leaving the previous reading in place is not perfect either,
    * but it is the difference between stale and fabricated. */
   for (int i = 0; i < NPERMS; i++)
      (void)jb_perm_granted(shell_activity(), perms[i], &g_sys_perm[i]);
   (void)jb_battery_ok(shell_activity(), &g_sys_batt);
   (void)jb_standby_bucket(shell_activity(), &g_sys_bucket);
   (void)jb_bg_restricted(shell_activity(), &g_sys_bg);
}

/* THE SUBMENU DISPATCHERS, one per screen family.
 *
 * ONE FUNCTION PER FAMILY, each returning 1 when the action was its own. A
 * single chain of thirty-four `else if`s covering the alarm, the export
 * sheet, the sync server, the device lists and four separate back buttons is
 * a size-gate problem answered by moving heterogeneity around rather than by
 * removing it. This is the way menu.c dispatches
 * per screen family, and what is left here is the navigation between them.
 */
/* THE ALARM AND NUDGE SUBMENU: the two thresholds, the two nudge bands, and
 * the outputs. Each threshold row opens the keypad and RECORDS the screen it
 * is tapped on, because this screen is reachable from the main screen as well
 * as from settings. */
static int alarm_menu_action(int action)
{
   struct prefs sp;
   settings_get(&sp);
   if (action == MA_ALARM_LOW || action == MA_ALARM_HIGH ||
       action == MA_NUDGE_LOW || action == MA_NUDGE_HIGH) {
      /* "LOW <value>" / "HIGH <value>" (main-screen rows or ALARM submenu):
       * type the threshold on the keypad (display units). The keypad
       * returns to the screen the row is tapped on -- the origin rule. */
      int mode = KP_NUDGE_HIGH;
      if (action == MA_ALARM_LOW)
         mode = KP_ALARM_LOW;
      else if (action == MA_ALARM_HIGH)
         mode = KP_ALARM_HIGH;
      else if (action == MA_NUDGE_LOW)
         mode = KP_NUDGE_LOW;
      nav_go(SCR_KEYPAD);
      forms_kp_open(mode, cur_screen() == SCR_ALARM ? SCR_ALARM : SCR_MAIN);
   } else if (action == MA_NUDGE_SOUND) {
      if (settings_set_nudge_sound(!sp.nudge_sound) != SETTINGS_OK)
         set_status("NOT SAVED");
   } else if (action == MA_NUDGE_VIB) {
      if (settings_set_nudge_vib(!sp.nudge_vib) != SETTINGS_OK)
         set_status("NOT SAVED");
   } else if (action == MA_ALARM_OPEN) {
      /* Entered from the settings row or the main alarm row; record the
       * origin so MA_ALARM_BACK returns exactly there. */
      nav_go(SCR_ALARM);
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* EXPORT DATA: a range, four checkboxes and the button. Session state, and
 * deliberately so -- the defaults (everything, all time) are the whole
 * point. */
static int export_menu_action(int action)
{
   if (action == MA_EXPORT) {
      nav_go(SCR_EXPORT); /* configure first; MA_EXP_GO does the work */
   } else if (action == MA_EXP_RANGE) {
      g_exp_range = (g_exp_range + 1) % 3; /* 30 D -> 1 Y -> ALL -> ... */
   } else if (action == MA_EXP_GLU) {
      g_exp_glu = !g_exp_glu;
   } else if (action == MA_EXP_DEV) {
      g_exp_dev = !g_exp_dev;
   } else if (action == MA_EXP_INS) {
      g_exp_ins = !g_exp_ins;
   } else if (action == MA_EXP_WT) {
      g_exp_wt = !g_exp_wt;
   } else if (action == MA_EXP_GO) {
      if (cur_screen() == SCR_EXPORT &&
          (g_exp_glu || g_exp_dev || g_exp_ins || g_exp_wt)) {
         /* Java builds the CSV per the checkboxes/range and opens the share
          * sheet; the menu stays, its X returns to settings. The ANSWER is
          * read: only Java accepting the call means the export was
          * dispatched, and a refusal is shown on the screen the user is
          * looking at rather than swallowed. */
         g_exp_failed = !sys_export_data();
      }
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* REMOTE PUSH: the server, the pairing, the restore, and unpairing. The
 * longest of these dispatchers, and the one whose actions reach furthest --
 * pairing and restore run on Java's worker and come back as an outcome (see
 * syncstat.h). */
static int remote_menu_action(int action)
{
   struct sync_creds sc;
   remote_creds_get(&sc);
   struct prefs sp;
   settings_get(&sp);
   if (action == MA_REMOTE_OPEN || action == MA_SYNCREST_NO) {
      /* Both mean "show the REMOTE screen": opening it from settings, and
       * backing out of the restore confirmation it launched. */
      nav_go(SCR_REMOTE);
   } else if (action == MA_REMOTE_TOGGLE) {
      /* THE VALUE BEING SET, not a re-read afterwards: `sp` is the snapshot
       * this dispatch began with, so asking it again would answer with the
       * state before the toggle -- and the retry below would then fire on
       * the way OFF and not on the way on. */
      int want = !sp.remote_on;
      if (remote_set_on(want) != SETTINGS_OK)
         set_status("SYNC SETTING NOT SAVED");
      /* TURNING IT BACK ON IS A REQUEST TO SYNC. Whatever schedule the last
       * failure earned belongs to the settings that failed; the user has
       * just changed one. Without this, switching sync off and on again --
       * the first thing anybody tries -- did nothing visible for up to five
       * minutes. */
      else if (want)
         remote_retry_now();
   } else if (action == MA_SYNC_EMAIL) {
      forms_set_label_field(LABEL_EMAIL);
      forms_kp_clear();
      for (int i = 0; sc.email[i]; i++) {
         char c = sc.email[i];
         if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A'); /* the editor's charset is upper case */
         forms_kp_type(c);
      }
      nav_go(SCR_LABEL);
      forms_kp_return_set(SCR_REMOTE);
   } else if (action == MA_SYNC_PAIR) {
      /* Both halves must exist first: a code is meaningless without a server
       * to send it to and an account to send it for -- and they are read as
       * ONE value, because "server set" and "account set" taken separately
       * can each be true of a different configuration. */
      struct remote_config rc;
      remote_config_get(&rc);
      if (rc.server[0] && rc.email[0]) {
         nav_go(SCR_KEYPAD);
         forms_kp_open(KP_SYNC_CODE, SCR_REMOTE);
      }
   } else if (action == MA_SYNC_RESTORE) {
      /* Ask first. It is not destructive -- restore only adds -- but it can
       * pull years of history over a mobile connection, and a row labelled
       * RESTORE on a settings screen is exactly the kind of thing tapped to
       * see what it does. */
      nav_go(SCR_SYNCRESTORE);
   } else if (action == MA_SYNCREST_YES) {
      /* Handed to the worker: a restore is one request per missing bucket and
       * must not run on the UI thread. The result arrives as a changed LAST
       * STATUS, like a sync. */
      syncjni_restore_request();
      nav_go(SCR_REMOTE);
   } else if (action == MA_SYNC_UNPAIR) {
      /* Forget the identity, not the server: re-pairing is the normal way to
       * move this phone to a different account, and it needs a fresh code
       * from the server anyway. */
      /* THE RUNTIME KEY GOES ONLY IF THE FILE DID. On failure the stored
       * identity is rolled back whole -- so dropping the key anyway leaves a
       * phone that has stopped syncing while the screen still says PAIRED,
       * and the next launch loads the identity straight back. */
      if (remote_forget_identity() != SETTINGS_OK)
         set_status("UNPAIR NOT SAVED");
      else
         sync_set_key(0, sc.key);
   } else if (action == MA_REMOTE_IP) {
      /* The LABEL editor, not the keypad: a server is "pancra.org" now, not a
       * dotted quad, and the numeric pad cannot type a letter. Seeded with the
       * current value so a small correction is a small amount of typing. */
      forms_set_label_field(LABEL_SERVER);
      forms_kp_clear();
      for (int i = 0; sp.remote_server[i]; i++) {
         char c = sp.remote_server[i];
         if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A'); /* the editor's charset is upper case */
         forms_kp_type(c);
      }
      nav_go(SCR_LABEL);
      forms_kp_return_set(SCR_REMOTE);
   } else if (action == MA_REMOTE_PORT) {
      nav_go(SCR_KEYPAD);
      forms_kp_open(KP_PORT, SCR_REMOTE);
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* THE DEVICE LISTS: opening them, paging them, and the pinned-shortcut
 * checkboxes that live on the same screen. */
static int devlist_menu_action(int action, int ix)
{
   if (action == MA_DEVICES_OPEN) {
      /* Two doors lead here -- the main screen's big number and the SETTINGS
       * row -- so RECORD which one, never infer it: MA_DEVICES_BACK returns
       * exactly there. Capture only on external entry; OLD DEVICES and the
       * pairing flow re-enter SCR_DEVICES on their own way back and must not
       * overwrite the origin. */
      if (cur_screen() == SCR_MAIN || cur_screen() == SCR_SETTINGS) {
         g_dev_page = 0; /* a fresh entry always opens on page 1 */
      }
      nav_go(SCR_DEVICES);
   } else if (action == MA_SCTOGGLE) {
      /* Toggle a main-screen PIN. The list is kept DENSE -- removing the
       * middle one closes the gap -- because the main screen walks it and an
       * empty slot between two live ones would draw a hole in the row.
       *
       * At SC_MAX the extra tap is REFUSED rather than evicting something:
       * the user picked those, and silently dropping one to make room is the
       * kind of helpfulness that loses a setting. The status line says why,
       * so the tap is not simply inert -- and it says the LIMIT rather than a
       * number typed here, which was still "3 PINS MAX" two rises after the
       * limit stopped being three. */
      /* The STABLE id, not the touch code: this goes to disk. See
       * enum shortcut_id in settings.h. */
      int id = ui_shortcut_id(ix);
      if (settings_pinned(id)) {
         if (settings_pin_remove(id) != SETTINGS_OK)
            set_status("PIN NOT SAVED");
      } else {
         int pr = settings_pin_add(id);
         if (pr == SETTINGS_FULL) {
            char full[24];
            (void)snprintf(full, sizeof full, "%d PINS MAX", SC_MAX);
            set_status(full);
         }
         else if (pr != SETTINGS_OK)
            set_status("PIN NOT SAVED");
      }
   } else if (action == MA_DEVPAGE) {
      g_dev_page = ix;
   } else if (action == MA_OLDDEV_OPEN) {
      g_old_page = 0; /* always open on the first page */
      nav_go(SCR_OLDDEV);
   } else if (action == MA_OLDPAGE) {
      g_old_page = ix;
   } else if (action == MA_OLDDEV_BACK) {
      /* OLD DEVICES hangs off the DEVICES screen, not off settings. */
      nav_go(SCR_DEVICES);
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* Every submenu action: tried against each family in turn. Returns 1 when
 * `action` was one of ours. */
static int submenu_action(int action, int ix)
{
   if (alarm_menu_action(action) || export_menu_action(action) ||
       remote_menu_action(action) || devlist_menu_action(action, ix))
      return 1;
   if (action == MA_DISPLAY_OPEN) {
      nav_go(SCR_DISPLAY);
   } else if (action == MA_PERMS_OPEN) {
      sys_refresh(); /* fresh snapshot for the screen being opened */
      nav_go(SCR_PERMS);
   } else if (action == MA_PERMS_BACK || action == MA_REMOTE_BACK ||
              action == MA_DISPLAY_BACK || action == MA_EXP_BACK) {
      /* All four submenus were opened FROM settings; their X returns
       * there. (The ALARM submenu is the exception -- also reachable from
       * the main screen, so its back goes through g_alarm_from above.) */
      nav_go(SCR_SETTINGS);
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* Per-device STYLING: marker shape, size, colour, and the rename keypad.
 * Split out of menu_action for the same reason the typed-entry families were
 * -- that function is the app's single largest, and the size gate is what
 * stops it growing without bound. Returns 1 when `action` was one of ours.
 */
static int style_action(int action, int ix)
{
   /* THE SELECTED DEVICE'S ID, read once. Every change below is asked for by
    * id: a mint or a forget between the read and the call would otherwise
    * recolour, rename or resize whichever device had moved into the slot. */
   struct sensor_slot sel;
   int have_sel = sensor_slot_of(sel_device(), &sel);
   if (action == MA_MARKER) {
      if (have_sel) {
         forms_set_markpick(-1); /* this picker edits the SENSOR's styling */
         nav_go(SCR_MARKPICK);
      }
   } else if (action == MA_MARK_PICK) {
      int mk = ix;
      if (forms_markpick() >= 0) {
         /* the picker is editing an INSULIN type's marker, not a sensor's */
         if (settings_set_ins_style(forms_markpick(), mk, -1, 0) != SETTINGS_OK)
            set_status("MARKER NOT SAVED");
      } else if (sensor_set_marker(sel.id, mk) != 0) {
         set_status("MARKER NOT SAVED");
      }
      /* stay on the combined MARKER menu so shape/size/colour can be
       * adjusted together; the title-row X returns to the device menu */
   } else if (action == MA_SIZE_PICK) {
      int sz = ix; /* 1..MARK_SIZE_MAX */
      if (sz >= 1 && sz <= MARK_SIZE_MAX && forms_markpick() >= 0) {
         if (settings_set_ins_style(forms_markpick(), -1, -1, sz) !=
             SETTINGS_OK)
            set_status("SIZE NOT SAVED");
      } else if (sensor_set_size(sel.id, sz) != 0) {
         set_status("SIZE NOT SAVED");
      }
      /* stay on the combined MARKER menu */
   } else if (action == MA_SIZE) {
      if (sensor_cycle_size(sel.id) != 0)
         set_status("SIZE NOT SAVED");
   } else if (action == MA_LABEL) {
      if (have_sel) {
         /* seed the field with the current name so a small edit is a small
          * amount of typing */
         forms_kp_seed(sel.label);
         nav_go(SCR_LABEL);
         forms_set_label_field(LABEL_SENSOR);
         forms_kp_return_set(SCR_SENSOR);
      }
   } else if (action == MA_CHAR) {
      /* FOUR fields share this editor now: a sensor label, the sync SERVER,
       * the account email, and a new FOOD name. */
      struct forms_view fv;
      forms_view_get(&fv);
      /* THE FIELD'S CAPACITY IS A PROPERTY OF THE TYPE, not of a copy of the
       * current value: taking a whole settings snapshot to measure a member
       * is work for an answer the compiler already knows.
       *
       * The cap has to be the DESTINATION's, per field, and a field left off
       * this list does not fail loudly -- it silently gets the sensor label's
       * capacity. For a food name that is 20 characters too many: the keypad
       * would accept them and food_type_add would then refuse the name, so
       * the refusal arrives after the typing rather than in place of it. */
      int cap = (int)sizeof(((struct sensor_slot *)0)->label) - 1;
      if (fv.label_field == LABEL_SERVER)
         cap = (int)sizeof(((struct prefs *)0)->remote_server) - 1;
      else if (fv.label_field == LABEL_EMAIL)
         cap = (int)sizeof(((struct sync_creds *)0)->email) - 1;
      else if (fv.label_field == LABEL_FOOD)
         cap = FOOD_NAME_MAX;
      if (forms_kp_len() < cap)
         forms_kp_type(ui_label_chars[ix]);
   } else if (action == MA_COLOR) {
      if (have_sel)
         nav_go(SCR_COLORPICK); /* open the colour picker */
   } else if (action == MA_COLOR_PICK) {
      int ci = ix;
      if (forms_markpick() >= 0) {
         if (settings_set_ins_style(forms_markpick(), -1, ci, 0) != SETTINGS_OK)
            set_status("COLOUR NOT SAVED");
      } else if (sensor_set_color(sel.id, ci) != 0) {
         set_status("COLOUR NOT SAVED");
      }
      /* stay on the combined MARKER menu (see MA_MARK_PICK) */
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* SETTINGS TOGGLES: the rows on the SETTINGS screen that flip one stored
 * preference and nothing else. No navigation, no radio, no storage beyond
 * settings_save() -- which is what makes this the smallest family and the one
 * to copy when adding another switch. */
static int settings_action(int action, int ix)
{
   struct prefs sp;
   settings_get(&sp);
   (void)ix; /* no indexed action in this family */
   if (action == MA_ORIENT) {
      if (settings_cycle_orient() != SETTINGS_OK)
         set_status("NOT SAVED");
   } /* applied on close */
   else if (action == MA_SOUND) {
      if (settings_set_sound(!sp.sound_on) != SETTINGS_OK)
         set_status("NOT SAVED");
      alarm_reactuate(); /* an alarm may be latched but inaudible -- see
                          * there
                          */
   } else if (action == MA_VIB) {
      if (settings_set_vib(!sp.vib_on) != SETTINGS_OK)
         set_status("NOT SAVED");
      alarm_reactuate();
   } else if (action == MA_UNITS) {
      if (settings_set_units(!sp.units) != SETTINGS_OK)
         set_status("UNITS NOT SAVED");
      /* The notification renders the value in DISPLAY units (title AND the
       * status-bar icon) but is only rebuilt on a new datapoint -- without
       * an explicit refresh the bar keeps showing the previous units'
       * rendering (e.g. "9.4" beside a big number reading 169) for up to a
       * full CGM cadence after the toggle. */
      notify_mark();
      notify_tick();
   } else if (action == MA_DISC) {
      if (settings_cycle_disc() != SETTINGS_OK)
         set_status("NOT SAVED");
   } else if (action == MA_SCREEN) {
      if (settings_set_screen_on(!sp.screen_on) != SETTINGS_OK)
         set_status("NOT SAVED");
      shell_apply_screen_on(); /* takes effect immediately, not on menu close */
   } else if (action == MA_NEWDATA) {
      /* OFF -> BEEP -> CHIRP -> OFF. A cycle, not a toggle, since CHIRP
       * joined: the row shows which of the three is active. */
      if (settings_cycle_newdata() != SETTINGS_OK)
         set_status("NOT SAVED");
   } else {
      return 0;
   }
   return 1;
}

/* THE DEVICE REGISTRY, one dispatcher per stage of a device's life.
 *
 * Discovery, pairing, the primary choice, the wear budget, retirement,
 * revival and the meter's on-demand sync are fourteen actions with nothing in
 * common but the word "device", so each stage is its own function and
 * registry_action is the order they are tried in.
 */
/* OPENING one device's own screen, and leaving it. The origin is RECORDED on
 * a genuine external entry and never inferred -- the rule this file's header
 * says the codebase keeps re-learning. */
static int device_open_action(int action, int ix)
{
   if (action == MA_SENSOR) {
      /* Remember the origin so MA_SENSOR_BACK returns there -- but ONLY on a
       * genuine EXTERNAL entry (the DEVICES list, the retired-devices list, or
       * the main screen's info-block shortcut). The marker/colour/label/cal
       * sub-screens also re-enter SCR_SENSOR via MA_SENSOR. No listed
       * condition guards the capture -- "capture only when the current screen
       * is one of these three, because any other is an internal round-trip"
       * -- because nav_go treats a screen already on the path as a RETURN, so
       * re-entering SCR_SENSOR from its own sub-screen pops back to it and
       * cannot record the sub-screen as its origin. The rule is derived from
       * the route, not listed. */
      /* `ix` IS THE DEVICE ID. The row the finger landed on became one at
       * touch-down, in input.c, against the frame that drew it -- a redraw
       * between down and up renumbers rows, and this dispatch happens on the
       * release. A negative id means "keep whatever is selected", which is
       * how the sub-screens' X returns here without re-picking. */
      if (ix >= 0)
         sel_set_device(ix);
      nav_go(sensor_slot_of(sel_device(), 0) ? SCR_SENSOR : SCR_DEVICES);
   } else if (action == MA_SENSOR_BACK) {
      nav_back(); /* back to whatever opened it -- the path knows */
      sel_set_device(-1);
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* ADDING one: the type picker, the pairing keypad, and the meter's scan.
 * Everything up to the moment a stranger becomes a registered device
 * (pairing.h owns what happens after). */
static int device_add_action(int action, int ix)
{
   if (action == MA_ADDSENSOR) {
      /* The ADD DEVICE type picker shares MA_SENSOR_BACK for its X. It used
       * to need the same "external entry only" condition as MA_SENSOR above,
       * for the same reason -- METERHELP re-enters this to get back to the
       * picker and must not become its origin. nav_go handles it: SCR_SENSTYPE
       * is already on the path in that case, so this is a return. */
      nav_go(SCR_SENSTYPE);
   } else if (action == MA_TYPE) {
      /* Every abort inside the pairing flow (keypad X, device-list cancel)
       * returns to where the flow BEGAN, which is here. A mark, not a screen:
       * the ADD menu enters this without going through the type picker, so
       * "the screen the type was tapped on" is ADDMENU by one route and
       * SENSTYPE by another -- and the depth is right for both, including any
       * third route added later. */
      g_pair_mark = nav_mark();
      sel_set_add_type(ix);
      /* A CGM pairs with a code on the keypad; a meter bonds at the OS
       * level, so it only has to be discovered. */
      /* Where the flow lands when it finishes (or is closed): back into the
       * DEVICES screen only if that is where it was entered from -- an
       * ADD-menu or main-screen entry must fall back to the MAIN screen, not
       * into a menu the user never opened. */
      int kp_ret = nav_has(SCR_DEVICES) ? SCR_DEVICES : SCR_MAIN;
      if (sensor_kind(sel_add_type()) == KIND_CGM) {
         nav_go(SCR_KEYPAD);
         forms_kp_open(KP_PAIR_CODE, kp_ret);
         pair_scan_start();
      } else {
         /* A meter must be woken and put into pairing mode by hand first, so
          * show instructions and DON'T scan yet -- the Scan button there
          * starts the scan (MA_METERSCAN). */
         nav_go(SCR_METERHELP);
         forms_kp_return_set(kp_ret);
      }
   } else if (action == MA_METERSCAN) {
      nav_go(SCR_DEVLIST);
      forms_kp_return_set(nav_has(SCR_DEVICES) ? SCR_DEVICES : SCR_MAIN);
      pair_scan_start();
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* WHICH SENSOR OWNS THE BIG NUMBER. Two actions, and both re-bind the
 * displayed reading at once rather than at the next sample -- up to five
 * minutes away -- because the primary owns it by contract. */
static int device_primary_action(int action, int ix)
{
   struct sensor_slot psel;
   if (action == MA_PRIMARY) {
      if (sensor_slot_of(sel_device(), &psel)) {
         if (sensor_set_primary(psel.id) != SENSOR_OK)
            set_status("PRIMARY NOT SAVED");
         /* Re-bind the big number NOW, not on the next reading (up to 5 min
          * away): the primary owns it by contract, and if the new primary
          * has no data yet the display must clear rather than keep the
          * previous primary's value. Resolve the id before hist_lock (reg ->
          * hist). */
         int prime = sensor_primary_id();
         store_lock();
         hist_refresh_current(prime);
         store_unlock();
         notify_mark(); /* the notification mirrors the big number */
      }
   } else if (action == MA_PRIM_PICK) {
      /* The DEVICES screen's PRIMARY checkbox: the tap IS the switch, and the
       * box filling in is the feedback -- the screen STAYS open, unlike the
       * per-device PRIMARY row, because picking from a column invites
       * comparing before committing. `ix` IS THE DEVICE ID, resolved from
       * the drawn row at touch-down (see input.c); sensor_set_primary still
       * refuses a meter or an old device. */
      if (sensor_set_primary(ix) != SENSOR_OK)
         set_status("PRIMARY NOT SAVED");
      int prime = sensor_primary_id();
      store_lock();
      hist_refresh_current(prime);
      store_unlock();
      notify_mark(); /* the notification mirrors the big number */
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* THE REST OF A DEVICE'S LIFE: reconnecting an old one, its wear budget,
 * retiring it, and syncing a meter on demand. */
static int device_life_action(int action)
{
   char dt[DEVTAG_LEN];
   if (action == MA_RECONNECT) {
      /* Direct revive if the sensor was pulled BEFORE expiry; otherwise a
       * confirmation, since reconnecting a dead sensor just waits forever.
       */
      /* BY ID, all three steps: the check, the expiry question and the
       * revival. This screen and its confirmation are separate taps, and a
       * mint in between would otherwise reconnect a different device. */
      int rid = sel_device();
      if (sensor_slot_of(rid, 0)) {
         if (old_sensor_expired(rid))
            nav_go(SCR_RECONF);
         else
            do_reconnect(rid);
      }
   } else if (action == MA_RECON_YES) {
      do_reconnect(sel_device());
   } else if (action == MA_PEND_CANCEL) {
      /* CONFIRM FIRST; this action changes nothing.
       *
       * The armed-pairing row sits in the device list wearing a device's
       * clothes -- a name on the left, a state on the right -- and every
       * other row there opens that device's own screen. A finger reaching
       * for the new sensor lands on the one row that instead threw the
       * pairing away, and threw it away in silence: the status line is drawn
       * on the main screen, so from the device list the row simply vanished
       * with nothing said. */
      if (pairing_pending())
         nav_go(SCR_PENDCANCEL);
   } else if (action == MA_PEND_KEEP) {
      nav_go(SCR_DEVICES); /* leave it armed, exactly as it was */
   } else if (action == MA_PEND_STOP) {
      if (pairing_pending()) {
         LOGI("pending pairing cancelled by user");
         pairing_arm(0);
         set_status("PAIRING CANCELLED");
      }
      nav_go(SCR_DEVICES);
   } else if (action == MA_WEAR) {
      /* Cycle this device's wear budget: AUTO -> 10 D -> 15 D -> AUTO.
       *
       * THREE STATES, NOT TWO, AND AUTO MUST BE REACHABLE. A 10 <-> 15
       * toggle would mean the first tap writes an explicit override that
       * nothing can remove: sensor_wear_seconds gives a pin absolute
       * priority, which permanently disables the model resolution for that
       * device. A G7 paired on 2026-07-23 -- one day before the
       * SW14758 (G7 15 Day) model rule existed -- carried a pin of 10 from
       * that era, so once the rule landed the app went on counting a 10-day
       * budget for a sensor it could now positively identify as 15-day,
       * declaring it nearly finished with five days left. One accidental tap
       * on a full-width row was enough to cause it, and there was no way
       * back. Preferences only -- no radio, no provenance. */
      struct sensor_slot wsel;
      if (!sensor_slot_of(sel_device(), &wsel) ||
          sensor_cycle_wear(wsel.id) != 0)
         set_status("WEAR BUDGET NOT SAVED");
   } else if (action == MA_FORGET) {
      if (sensor_slot_of(sel_device(), 0))
         nav_go(SCR_FORGET); /* confirm first; this action changes nothing */
   } else if (action == MA_FORGET_YES) {
      device_retire(sel_device());
   } else if (action == MA_SYNC) {
      /* SYNC NOW CONNECTS, and that is the whole of what this control is.
       *
       * Clearing a throttle and restarting the scan is NOT a sync: start_scan
       * early-returns whenever scan_running() is set -- always, while the UI
       * is up (see scan_restart) -- and the rest of the design waits
       * passively for an advertisement. With the meter switched on but
       * between advertising bursts, that leaves the one control the user has
       * to force a sync doing nothing at all, silently, however many times it
       * is pressed. Measured from an HCI capture: twenty-six minutes of taps
       * with no radio traffic to the meter at all, then a sync 1.3 s after
       * the first advertisement arrived on its own.
       *
       * A registered meter is BONDED and its address is known, and
       * dexble_meter_connect uses autoConnect=true -- so there is no reason
       * whatsoever to wait for an advertisement. Connect, and let the stack
       * latch on when the meter is reachable. The 90 s watchdog releases the
       * link if it never is. */
      int mid = -1;
      char mmac[24];
      mmac[0] = 0;
      struct sensor_slot msl;
      struct sensor_rec mrec;
      if (sensor_slot_of(sel_device(), &msl) && !msl.old &&
          sensor_rec_of(msl.id, &mrec) && sensor_kind(mrec.type) == KIND_BGM) {
         mid = msl.id;
         str_snapshot(mmac, sizeof mmac, mrec.identity);
      }
      if (mid > 0 && mmac[0]) {
         if (meter_busy()) {
            /* SAY SO. Refusing is right -- meter_sync_start resets the otble
             * statics, and doing that under a sync in flight writes one
             * meter's records under another's id -- but refusing SILENTLY is
             * what this whole handler was already guilty of. */
            LOGI("manual sync refused: a meter sync is already in flight");
            set_status("METER BUSY, RETRY");
         } else {
            LOGI("manual sync: connecting to meter id %d (dev %s)", mid,
                 devtag(mmac, dt));
            set_status("METER: SYNCING");
            meter_sync_start(mid, mmac);
         }
      }
      /* Refresh the scan as well, for the CGM's sake and to recover one
       * Android has quietly demoted (scan_restart). Deliberately NOT
       * pair_scan_start(): that sets g_smart_pairing, which suppresses every
       * CGM's advert-driven reconnect and is never cleared from this screen,
       * so a single SYNC NOW tap through it kills CGM reconnection for the
       * life of the process. */
      pairing_forget_candidates(); /* atomic vs the binder-thread writer */
      scan_restart(shell_activity());
   } else {
      return 0; /* not ours */
   }
   return 1;
}

static int registry_action(int action, int ix)
{
   return device_open_action(action, ix) || device_add_action(action, ix) ||
          device_primary_action(action, ix) || device_life_action(action);
}

/* CALIBRATION AND RESCALE, which are the two ways a person overrides what the
 * sensor says. Kept together and apart from the registry because they change
 * the NUMBER a sensor reports rather than which sensor is used, and because
 * both are user-initiated only -- nothing in here may ever run
 * automatically. */
static int calib_action(int action, int ix)
{
   (void)ix; /* no indexed action in this family */
   /* THE SELECTED DEVICE, ONCE, AS ONE LOCKED READ -- and then only its id.
    * Calibration is the most consequential write the app makes, and an index
    * re-read after a mint or a forget on a binder thread names a DIFFERENT
    * device. Everything below is keyed by sel.id, which never moves. */
   struct sensor_slot sel;
   int have_sel = sensor_slot_of(sel_device(), &sel);
   if (action == MA_CAL_OPEN) {
      /* If a calibration for THIS sensor is still queued, show the pending
       * screen (REPLACE / CANCEL) rather than silently starting another. */
      int pend = have_sel && calib_queued_for(sel.id) > 0;
      if (pend) {
         nav_go(SCR_CALPEND);
      } else {
         /* Straight to the value keypad (like PLOT MAX); cancel returns to
          * the device menu. */
         nav_go(SCR_KEYPAD);
         forms_kp_open(KP_CALIB, SCR_SENSOR);
         forms_set_cal_pending(0);
      }
   } else if (action == MA_CAL_REPLACE) {
      /* Enter a new value; on CONFIRM it supersedes the queued one. */
      nav_go(SCR_KEYPAD);
      forms_kp_open(KP_CALIB, SCR_SENSOR);
      forms_set_cal_pending(0);
   } else if (action == MA_CAL_CANCEL) {
      /* Discard the queued calibration entirely. */
      if (calib_cancel() != CALIB_OK)
         set_status("CANCEL NOT SAVED");
      nav_go(SCR_SENSOR);
   } else if (action == MA_RESCALE_OPEN && have_sel &&
              calib_rescale_engaged(sel.id)) {
      /* Active OR pending for this sensor: show the active/pending screen
       * (CHANGE / STOP). */
      nav_go(SCR_RESCALEACT);
   } else if (action == MA_RESCALE_OPEN || action == MA_RESCALE_CHANGE) {
      /* Not active (or explicitly changing): go to the value keypad. */
      nav_go(SCR_KEYPAD);
      forms_kp_open(KP_RESCALE, SCR_SENSOR);
      forms_set_rescale_entry(0);
   } else if (action == MA_RESCALE_STOP) {
      if (calib_rescale_stop() != CALIB_OK)
         set_status("RESCALE STOP NOT SAVED");
      nav_go(SCR_SENSOR);
   } else if (action == MA_RESCALE_ENTER) {
      /* CONFIRM: compute the factor from the entered true value over the
       * live RAW reading, clamp to +-25%, activate for this sensor from NOW.
       * If there is NO live reading yet, HOLD the target (persisted) -- the
       * next reading for this sensor computes the factor. It is never
       * silently lost.
       */
      /* AND THE ENTERED VALUE OUTLIVES A REFUSED WRITE. calib_rescale_set
       * puts every part of the transition back when its one-line file cannot
       * be replaced (CALIB_UNSAVED means nothing changed, in memory or on
       * disk), so the only thing standing between the user and a retry is
       * this screen: clearing the value and leaving anyway turns a one-press
       * retry into a re-entry on a keypad three screens away.
       *
       * A value of 0, or no selected sensor, is not a failed write: there is
       * nothing to hold on to and nothing to retry, so those leave as
       * before. */
      enum draft_fate rfate = DRAFT_DONE;
      if (forms_rescale_entry() > 0 && have_sel &&
          calib_rescale_set(sel.id, calib_raw_on_link(link_for_sensor(sel.id)),
                            forms_rescale_entry()) != CALIB_OK) {
         set_status("RESCALE NOT SAVED");
         rfate = DRAFT_RETRY;
      }
      if (rfate == DRAFT_DONE) {
         forms_set_rescale_entry(0);
         nav_go(SCR_SENSOR);
      }
   } else if (action == MA_CAL_BACK || action == MA_FORGET_NO ||
              action == MA_RESCALE_BACK || action == MA_RECON_NO) {
      nav_go(SCR_SENSOR); /* these sub-screens back out to the sensor */
   } else if (action == MA_CAL_REFRESH) {
      int callink = cal_link();
      if (callink >= 0)
         driver_cal_bounds(callink);
   } else if (action == MA_CAL_ENTER) {
      /* CONFIRM: QUEUE the calibration durably (persisted), then try once
       * now. It is NOT dropped if the sensor is not streaming this instant
       * -- it stays queued and every subsequent reading retries it (see
       * calib_try in pancra_glucose) until the sensor accepts or the
       * freshness window lapses, and the outcome is always shown -- which is
       * what keeps a confirmed calibration from being lost to a reconnect
       * gap.
       */
      /* ...AND THE VALUE IS KEPT WHEN THE QUEUE COULD NOT BE WRITTEN. It is a
       * fingerstick: a number that was true a minute ago and cannot be
       * re-read by typing it again, so a confirmation screen that clears it
       * and leaves is asking for a fresh blood test. CALIB_UNSAVED means
       * nothing changed anywhere, so the confirmation is still exactly the
       * confirmation the user was looking at and YES retries it. */
      enum draft_fate cfate = DRAFT_DONE;
      if (forms_cal_pending() > 0 && have_sel) {
         /* THE QUEUE IS THE DURABLE PART. If it did not reach the disk the
          * calibration is not queued at all -- so say so, and do not try to
          * send it: an accepted write would rewrite every reading from this
          * sensor against a value the app will have forgotten by the next
          * launch. */
         if (calib_queue(sel.id, forms_cal_pending()) != CALIB_OK) {
            set_status("CALIBRATION NOT SAVED");
            cfate = DRAFT_RETRY;
         } else {
            int callink = cal_link();
            if (callink >= 0)
               calib_try(callink, sel.id); /* opportunistic first attempt */
         }
      }
      if (cfate == DRAFT_DONE) {
         forms_set_cal_pending(0);
         nav_go(SCR_SENSOR);
      }
   } else {
      return 0;
   }
   return 1;
}

/* THE ANDROID SYSTEM SURFACE: battery optimisation, runtime permissions, the
 * app-settings page, and the rows that lead to them. Everything in this family
 * ends in a JNI call that hands control to the OS, which is exactly why it is
 * worth having them in one place -- they are the actions that can return with
 * the world changed underneath. */
static int system_action(int action, int ix)
{
   struct prefs sp;
   settings_get(&sp);
   (void)ix;                   /* no indexed action in this family */
   if (action == MA_BATTERY) { /* request if optimised, else settings */
      if (g_sys_batt)
         jb_open_settings(shell_activity());
      else
         jb_request_battery(shell_activity());
      sys_refresh();
   } else if (action == MA_BGEXEC) {
      jb_open_settings(shell_activity());
      sys_refresh();
   } /* bg-exec: change in settings */
   else if (action == MA_PERM) {
      /* denied -> request dialog; granted -> app settings (only place to
       * revoke) */
      if (g_sys_perm[ix])
         jb_open_settings(shell_activity());
      else
         jb_request_perm(shell_activity(), perms[ix]);
      sys_refresh();
   } else if (action == MA_CLOSE) {
      nav_go(SCR_MAIN);
      jb_set_orientation(shell_activity(), sp.orient);
   } /* apply orient */
   /* --- keypad (opened from settings rows: return there on close) --- */
   else if (action == MA_PAIR_CODE) {
      nav_go(SCR_KEYPAD);
      forms_kp_open(KP_PAIR_CODE, SCR_SETTINGS);
      pair_scan_start(); /* scan under the code entry to hide the delay */
   } else if (action == MA_PLOTMAX) {
      nav_go(SCR_KEYPAD);
      forms_kp_mode_set(KP_PLOT_MAX);
      forms_kp_return_set(SCR_DISPLAY); /* PLOT MAX now lives on DISPLAY */
      forms_kp_clear();
   } else {
      return 0;
   }
   return 1;
}

/* THE NUMERIC KEYPAD: digits, the decimal point, and the two ways out. The
 * keypad is shared by the pairing code, the alarm thresholds, the plot
 * maximum, a dose and a weight, so it owns no meaning of its own -- g_kp.mode
 * says who asked for it and MA_OK (in menu_action) decides what the digits
 * meant. */
static int keypad_action(int action, int ix)
{
   struct prefs sp;
   settings_get(&sp);
   if (action == MA_DIGIT) { /* ix IS the digit */
      int cap = kp_slots(forms_kp_mode());
      forms_kp_err_clear(); /* a new keystroke retires the last refusal */
      if (forms_kp_len() < cap && ix >= 0 && ix <= 9)
         forms_kp_type((char)('0' + ix));
   } else if (action == MA_DOT) {
      /* '.' exists where the VALUE has a decimal point: the four thresholds
       * in mmol/L ("5.5") and a weight always ("162.4"). The guard keeps a
       * stale tap (racing a repaint) from injecting one into any other
       * numeric entry.
       *
       * ONE PREDICATE, shared with the renderer that draws the key
       * (kp_has_dot). Spelled out here as a range, it drifts from the one
       * the renderer draws the key for -- 10..11 against 10..13 leaves the
       * NUDGE keypads a visible, tappable, DEAD '.', and with it no way to
       * enter a nudge threshold at all in mmol/L. */
      if (cur_screen() == SCR_KEYPAD && kp_has_dot(forms_kp_mode(), sp.units) &&
          forms_kp_len() < kp_slots(forms_kp_mode())) {
         if (!forms_kp_has('.')) /* one decimal point per number */
            forms_kp_type('.');
      }
   } else if (action == MA_BACKSPACE) {
      forms_kp_del();
   } else if (action == MA_KP_CLOSE) {
      /* ONLY the pairing keypad backs out to the ADD DEVICE type picker. The
       * label editor and the plot-max / calibration keypads share this close
       * code -- gate on the actual menu, or renaming a device (SCR_LABEL)
       * and cal/plot-max entry would land on ADD DEVICE rather than on their
       * own return target. */
      int was_pairing =
          (cur_screen() == SCR_KEYPAD && forms_kp_mode() == KP_PAIR_CODE);
      if (pairing_smart())
         pair_cancel(); /* abandon pairing, keep the old bond */
      if (was_pairing)
         nav_return_to(g_pair_mark); /* where the flow began */
      else
         keypad_close();
   } else { /* X -> close */
      return 0;
   }
   return 1;
}

/* PAIRING A DISCOVERED DEVICE: the scan list, the confirmation, and the two
 * answers to it. Separate from the registry family because this is the one
 * flow driven by what the RADIO found rather than by what the user already
 * owns, and because every exit from it has to return to where the flow began
 * (g_pair_mark). */
static int pair_action(int action, int ix)
{
   (void)ix;                      /* no indexed action in this family */
   if (action == MA_DEV_CANCEL) { /* device list: cancel -> where pairing
                                     began */
      pair_cancel();
      nav_return_to(g_pair_mark);
   } else if (action == MA_DEV_PICK) { /* device list: pick */
      /* Only honour a device-pick while the list is actually open and the
       * index is a real device. The hit-box array is rebuilt by draw()
       * (which can run on a BLE thread), so a tap racing a repaint could
       * otherwise map to a phantom pick and commit_pair -> driver_forget
       * would drop the live bond.
       */
      /* A pick PROPOSES; only the PAIRCONF YES commits. The list is ordered
       * by live RSSI, so rows can reorder under the finger -- pairing on the
       * raw tap let one mis-press register the wrong device and drop a bond.
       */
      if (cur_screen() == SCR_DEVLIST && pairing_pick(ix))
         nav_go(SCR_PAIRCONF);
   } else if (action == MA_PAIR_YES) {
      /* THE consequential step, reached only from an explicit YES. Gate on
       * the menu still being open, like the pick itself: a tap racing a
       * repaint must not commit twice or from nowhere. */
      if (cur_screen() == SCR_PAIRCONF && pairing_pend_mac()[0]) {
         char macbuf[20];
         str_snapshot(macbuf, sizeof macbuf, pairing_pend_mac());
         pairing_propose("", ""); /* consumed: a second YES commits nothing */
         commit_pair(macbuf);
      }
   } else if (action == MA_PAIR_NO) {
      if (cur_screen() == SCR_PAIRCONF) {
         pairing_propose("", "");
         nav_go(SCR_DEVLIST); /* nothing committed: back to the list */
      }
   } else {
      return 0;
   }
   return 1;
}

/* THE SHORTCUTS OUT OF THE MAIN SCREEN, plus the two notification switches.
 * Small, and a family rather than four loose branches because each one is a
 * single hop with no state of its own -- the sort of action that otherwise
 * accretes in the middle of a thousand-line chain. */
static int shortcut_action(int action, int ix)
{
   struct prefs sp;
   settings_get(&sp);
   (void)ix; /* no indexed action in this family */
   if (action == MA_ADD_OPEN) {
      nav_go(SCR_ADDMENU);
   } else if (action == MA_INSLOG_BACK || action == MA_WTLOG_BACK ||
              action == MA_ALARM_BACK || action == MA_DEVICES_BACK) {
      /* Every X that simply pops to wherever its screen was opened FROM. One
       * branch because it is one rule: nav_back() reads the recorded origin,
       * so there is nothing per-screen left to say -- and four identical
       * bodies scattered down the chain is exactly how hardcoded exit
       * targets drift apart -- each log is reachable from the ADD menu OR
       * from a main-screen shortcut, so a comment claiming "where it opened"
       * over code naming one fixed menu is the failure mode. */
      nav_back();
   } else if (action == MA_STATBAR || action == MA_LOCKSCR ||
              action == MA_NOTIF_REOPEN) {
      if (action == MA_STATBAR) {
         if (settings_set_statbar(!sp.statbar_val) != SETTINGS_OK)
            set_status("NOT SAVED");
      } else if (action == MA_LOCKSCR) {
         if (settings_set_lockscr(!sp.lockscr_val) != SETTINGS_OK)
            set_status("NOT SAVED");
      }
      /* All three re-post the notification immediately: the toggles so
       * the change is visible at once, REOPEN because re-posting IS the
       * action (a swiped-away notification returns on notify()). */
      notify_mark();
      notify_tick();
   } else {
      return 0;
   }
   return 1;
}

/* The back key's action for the current screen, plus its index in *ix.
 *
 * *ix exists for the one screen whose back target is INDEXED -- the combined
 * marker/colour picker returns to the sensor it was opened for. With the code
 * and the index in one integer that was MA_SENSOR + slot; separated, it is a
 * second output rather than arithmetic that can land on a neighbour. */
int menu_back_code(int *ix)
{
   *ix = 0;
   switch (cur_screen()) {
      case SCR_SETTINGS:
      case SCR_ADDMENU: return MA_CLOSE;
      case SCR_DEVICES: return MA_DEVICES_BACK;
      case SCR_KEYPAD: /* menu_action gates on kp_mode, exactly like the X
                        */
      case SCR_LABEL: return MA_KP_CLOSE;
      case SCR_DEVLIST: return MA_DEV_CANCEL;
      case SCR_SENSOR:
      case SCR_SENSTYPE: return MA_SENSOR_BACK;
      case SCR_CAL:
      case SCR_CALPEND: return MA_CAL_BACK;
      case SCR_RESCALE:
      case SCR_RESCALEACT: return MA_RESCALE_BACK;
      case SCR_FORGET: return MA_FORGET_NO;
      case SCR_FOOD: return MA_FOOD_DISCARD;
      case SCR_FOODTYPE: return MA_FOODTYPE_BACK;
      case SCR_FOODLOG: return MA_FOODLOG_BACK;
      case SCR_FOODDEL: return MA_FOODDEL_NO;
      case SCR_EXLOG: return MA_EXLOG_BACK;
      case SCR_EXEDIT: return MA_EX_DISCARD;
      case SCR_EXDEL: return MA_EXDEL_NO;
      case SCR_MARKPICK:
      case SCR_COLORPICK:
         /* the combined picker's X: DISPLAY for an insulin type's styling,
          * the owning sensor's screen otherwise (same as render_markpick) */
         if (forms_markpick() >= 0)
            return MA_INSMARK_BACK;
         *ix = -1; /* keep the selection: see MA_SENSOR */
         return MA_SENSOR;
      case SCR_METERHELP: return MA_ADDSENSOR;
      case SCR_SYNCRESTORE: return MA_SYNCREST_NO;
      case SCR_PAIRCONF: return MA_PAIR_NO;
      case SCR_PENDCANCEL: return MA_PEND_KEEP;
      case SCR_INSULIN: return MA_INS_DISCARD;
      case SCR_INSDEL: return MA_INSDEL_NO;
      /* The three WEIGHT screens, missing since they were added: default
       * returns -1, and the caller CLAIMS the key handled either way, so back
       * was silently dead on all of them and the X was the only way out. The
       * LOG WEIGHT form is now what the weight keypad returns to, which puts
       * it in the middle of the shortest path in the app. */
      case SCR_WEIGHT: return MA_WT_DISCARD;
      case SCR_WTLOG: return MA_WTLOG_BACK;
      case SCR_WTDEL: return MA_WTDEL_NO;
      case SCR_ALARM: return MA_ALARM_BACK;
      case SCR_EXPORT: return MA_EXP_BACK;
      case SCR_INSLOG: return MA_INSLOG_BACK;
      case SCR_DISPLAY: return MA_DISPLAY_BACK;
      case SCR_PERMS: return MA_PERMS_BACK;
      case SCR_OLDDEV: return MA_OLDDEV_BACK;
      case SCR_RECONF: return MA_RECON_NO;
      case SCR_REMOTE: return MA_REMOTE_BACK;
      /* NO `default:`. Every screen is listed, so -Wswitch-enum turns adding
       * one and forgetting its back code into a build error rather than a
       * key that silently does nothing -- which is exactly how a screen ships
       * without a back key. SCR_MAIN is "no modal is
       * open", which has no back code by definition.
       *
       * SCR_GATE and SCR_N appeared here the moment the two screen enums
       * became one, and that is the unification paying for itself: this map
       * was exhaustive over a private list that did not contain them, so it
       * was only ever exhaustive over the screens somebody had remembered to
       * copy into it. Neither has a back code -- the gate is the first-run
       * rationale and the only way past it is CONTINUE (there is nowhere
       * behind it to go), and SCR_N is the count, not a screen. */
      case SCR_MAIN:
      case SCR_GATE:
      case SCR_N: break;
   }
   return -1;
}

/* Every insulin-related menu action (the LOG/EDIT form, the dose log
 * table, the marker picker), split out of menu_action so neither function
 * outgrows the size gate. Returns 1 when `action` was one of ours. */

void menu_action(int action, int ix)
{
   if (settings_action(action, ix) || registry_action(action, ix) ||
       calib_action(action, ix) || system_action(action, ix) ||
       keypad_action(action, ix) || pair_action(action, ix) ||
       shortcut_action(action, ix) || forms_action(action, ix) ||
       style_action(action, ix) || submenu_action(action, ix)) {
      /* HANDLED BY A NAMED FAMILY, and EXACTLY ONE of them.
       *
       * Every family is defined immediately above this function or in
       * app/form*.c, and each states in a sentence what it is a family OF.
       * THE ORDER IS NOT LOAD-BEARING, and that is a property worth having:
       * with first-match-wins, two families claiming one action produce a
       * control whose behaviour depends on how this file happens to be
       * written, and an action claimed by NONE of them produces a button that
       * draws perfectly and does nothing.
       *
       * `make -f test/Makefile actioncheck` now proves neither can happen: it
       * attributes every MA_* to the function that tests it, across all six
       * dispatch files, and refuses a duplicate or an orphan. With ownership
       * unique the chain answers the same whatever order it is written in --
       * so the order below is a reading convenience rather than a rule, and
       * adding a family cannot change what an existing control does. (It
       * found six actions nothing dispatched, on its first run.) */
   } else if (action == MA_OK) {
      /* OK MEANS "I HAVE FINISHED TYPING", and nothing more. The keypad and
       * the label editor are one widget each, shared by every field that needs
       * one, so this is where an entry stops being characters and becomes a
       * threshold, a dose, a hostname or a pairing code. Each family below
       * answers "what did the digits mean" for one kind of field. */
      int r = label_commit();
      if (!r)
         r = kp_commit_correction();
      if (!r)
         r = kp_commit_thresholds();
      if (!r)
         r = kp_commit_number();
      if (!r)
         r = kp_commit_datetime();
      if (!r)
         r = kp_commit_pair();
      if (r == COMMIT_STAY)
         return; /* the family deliberately left the screen alone */
   }
   shell_repaint();
}
