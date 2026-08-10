// SPDX-License-Identifier: GPL-3.0
// main.c --- Native app core: UI rendering, state, and JNI wiring
// Copyright 2026 Jakob Kastelic

/* pancra native core, plain C -- no NDK glue.
 * Implements ANativeActivity_onCreate directly; links against stub
 * libc/libandroid/liblog (stub_*.c) -- the phone binds the real bionic ones.
 * jni.h comes from the host JDK (same ABI as Android's).
 *
 * Owns the whole UI (direct struct ANativeWindow pixel rendering),
 * settings/alarm state, the reading history and stats, and the JNI wiring to
 * the BLE pipe. All rendering runs on the main looper thread (see on_main); BLE
 * binder-thread updates just mark the screen dirty for the next 1 Hz repaint.
 */
#include "alarmlogic.h"
#include "dexdriver.h"
#include "dexlibc.h"
#include "insulin.h"
#include "ndk.h"
#include "otble.h"
#include "pancra.h"
#include "plot.h"
#include "plotdata.h" /* long-span plot data, bucketed from the log */
#include "scanlogic.h"
#include "sensors.h"
#include "settings.h"
#include "sync.h"
#include "syncjni.h"
#include "stats.h"
#include "store.h"
#include "ui.h"
#include "util.h"
#include "weight.h"
#include <jni.h>
#include <jni_md.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)

/* ---- app configuration constants (tunables collected here) ---- */
#define MAX_LINES 16 /* text lines on the pre-reading status screen */
#define MAX_COLS  33 /* character columns the UI lays out to */
#define MAX_DEVS  12 /* sensors held in the PAIR NEW SENSOR list */
#define MENU_MAX  16 /* touch hit-boxes tracked per drawn menu */
#define NPERMS    3  /* runtime permissions requested at once */

/* The other half of ui.h's range assertions -- these two bases are indexed by
 * constants this file owns. See ui.h for why a collision is silent and what to
 * do if one fires. */
_Static_assert(MA_PERM + NPERMS <= MA_BATTERY, "MA_PERM range hits MA_BATTERY");
_Static_assert(MA_DEV_PICK + MAX_DEVS <= MA_CHAR,
               "MA_DEV_PICK range hits MA_CHAR");
#define NOTIFY_W 512 /* lock-screen plot bitmap width (px) */
#define NOTIFY_H                                                               \
   232 /* lock-screen plot bitmap height (px); taller now that                 \
        * the reading+time share one title line above it */

/* POSIX file I/O + kernel-timer calls. This is a FREESTANDING build with a
 * minimal stub libc (no NDK sysroot on the include path), so the system headers
 * that would declare these do not exist here -- they are hand-declared and libc
 * binds them at runtime. CLOCK_MONOTONIC is for the repaint timerfd below.
 *
 * (An editor configured with the normal Android sysroot sees BOTH this and the
 * system <time.h> and reports a redefinition of itimerspec; that is a tooling
 * mismatch, not a build defect -- the compiler here has no system header.) */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1

struct itimerspec {
   struct timespec it_interval, it_value;
};
#endif

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *nv,
                    struct itimerspec *ov);

/* --- screen model: a handful of text lines, redrawn on change --- */

struct dev {
   char name[9];
   char mac[18];
   int rssi;
   unsigned count;
   long long last_log_ms;
   long seen_t;
};

static struct ANativeActivity *g_act;
/* Render/thread sync (imperative-shell concern): the surface, the main looper
 * thread id, and a busy flag that both serializes draws and locks a BLE-thread
 * reading update against the main-thread draw. All rendering runs on the main
 * thread; a BLE reading just marks the UI dirty for the 1 Hz repaint. */
static struct ANativeWindow *volatile g_win;
static volatile int g_draw_busy;
static volatile int g_main_tid;
static volatile int g_ui_dirty;

static int on_main(void)
{
   return g_main_tid != 0 && gettid() == g_main_tid;
}

static void hist_lock(void)
{
   /* YIELD while spinning, like the other three locks.
    *
    * This is the one lock held across the longest operation in the app:
    * draw() holds it through ANativeWindow_lock, which blocks in dequeueBuffer
    * whenever the BufferQueue is full -- routine during a system animation or
    * an orientation change, and tens to hundreds of milliseconds. A GATT binder
    * thread delivering a reading meanwhile burned a full core with no
    * reschedule point for that entire window, on a battery-critical app that
    * runs 24/7, and on a contended little core it could hold the CPU against
    * the very main thread it is waiting for. */
   while (__atomic_exchange_n(&g_draw_busy, 1, __ATOMIC_SEQ_CST))
      sched_yield();
}

static void hist_unlock(void)
{
   __atomic_store_n(&g_draw_busy, 0, __ATOMIC_SEQ_CST);
}

/* Guards the pairing-candidate list (g_devs / g_ndevs).
 *
 * jni_on_advert runs on a BLE binder thread and both READS and WRITES g_ndevs
 * (find-slot loop, then increment), while the main looper reads it
 * (build_model, select_candidate, commit_pair gate) AND resets it to 0 on a
 * pairing action. A release store on the writer alone does not order the
 * plain-load readers on ARM, and two threads doing read-modify-write on g_ndevs
 * (binder increment vs main reset) is a lost-update race no single atomic
 * closes. A tiny leaf lock -- taken alone, never nested inside another lock and
 * never held across a call that takes one -- fixes both. yield while spinning,
 * like the other locks. */
static volatile int g_devlist_busy;

static void devlist_lock(void)
{
   while (__atomic_exchange_n(&g_devlist_busy, 1, __ATOMIC_SEQ_CST))
      sched_yield();
}

static void devlist_unlock(void)
{
   __atomic_store_n(&g_devlist_busy, 0, __ATOMIC_SEQ_CST);
}

static int g_scanning;
/* The activity is paused (on_pause -> on_resume). While paused the scan stays
 * down deliberately; the self-heal in on_timer must not fight that. */
static int g_paused;
/* Hold the scan down until this time, so a pairing/bonding connect gets a quiet
 * radio. Zero means "no hold". See the self-heal in on_timer. */
static long g_scan_hold_until;
/* A stop_scan that Java could not confirm; the 1 Hz timer retries it. Without
 * this, g_scanning latches at 1 with no live scan behind it. */
static int g_scan_stop_pending;
static jclass g_ble; /* global ref to com.jk.pancra.Ble */
static jmethodID g_scan, g_stop;
static jmethodID m_set_orient, m_perm_granted, m_req_perm,
    m_open_settings; /* settings-menu ops */
static jmethodID
    m_export; /* EXPORT DATA: share the CSVs via the system sheet */
static jmethodID m_batt_ok, m_req_batt, m_bucket,
    m_bg_restricted;             /* background-run ops */
static jmethodID m_show_glucose; /* push value+plot to the notification */
static jmethodID
    m_bonded_stelo; /* resolve the bonded Stelo's MAC from bond list */
/* REMOTE sync: cursor read, batch push, and the in-flight/backoff gate. */

static char g_remote_status[24]; /* last attempt's reply, for the UI */
/* Set on a BLE binder thread, consumed by BOTH the activity's 1 Hz timer and
 * the service tick thread -- so the test-and-clear must be atomic, or a
 * reading can be marked dirty and cleared by the other consumer without ever
 * being rendered. */
static volatile int g_notify_dirty;
/* Wall-clock of the last REMOTE push the server ACKNOWLEDGED (HTTP 2xx),
 * reported back by Ble.remotePush's worker thread via onRemoteOk. Runtime
 * state, not a setting: it starts at 0 ("NEVER") on every launch. A plain
 * long store/load -- worst case the settings row shows an age one frame
 * stale. */
static long g_remote_last_ok;
static char g_status[MAX_COLS + 1] = "STARTING";
/* Execution checkpoint: set to a static label at the top of each hot code path,
 * so the crash handler can record WHERE we were when a fault hit (debuggerd
 * tombstones are SELinux-locked here). volatile so it isn't optimised away. */
static volatile const char *g_where = "boot";
static struct dev g_devs[MAX_DEVS];
static int g_ndevs;
static unsigned g_total; /* all adverts heard, pipe health */
static char g_lines[MAX_LINES][MAX_COLS + 1];
static int g_nlines;
/* Last connect attempt per link, so a burst of adverts yields one connect. */
static long g_link_try[LINK_MAX];

/* dexble transport prototypes come from pancra.h; driver_* from dexdriver.h */

/* reading history + current-reading snapshot live in store.c (see store.h) */
static long g_tz_off;     /* local timezone offset, seconds */
static long g_tz_checked; /* when g_tz_off was last refreshed */
static void init_tz_offset(JNIEnv *env);
/* Per LINK, not per process: a single flag was latched by whichever CGM
 * reported first, so a second sensor never requested its once-per-launch
 * backward fill and its pre-launch history was never recovered, on every
 * launch. */
static int g_startup_bf[LINK_MAX];
/* Per-link throttle for the interior-gap backfill scan below: it re-requests
 * until the hole is filled, so it must not fire on every 5-minute reading. */
static long g_gap_bf_at[LINK_MAX];
static int g_conn_rssi;    /* live connection RSSI (readRemoteRssi) */
static long g_conn_rssi_t; /* wall-clock of that measurement */
/* Per LINK. A single global throttle meant one sensor's DIS request blocked
 * every other sensor's for 60 s, delaying the provenance completion that fills
 * in its model and firmware. Same per-link-vs-global class as g_model_l and
 * g_startup_bf. */
static long g_devinfo_req[LINK_MAX];

static int g_plot_hours = 3; /* selected plot span (hours); the tab list and
                              * its hit boxes live in ui.c */

/* alarm thresholds (mg/dL, adjustable in the UI) + their button hit boxes */

/* alarm kind passed to dexble_alarm() / Alarm.trigger() (keep in sync with
 * Alarm.java) */
/* The old ALARM_LOW/HIGH/STALE enum lived here. It is DELETED, not kept for
 * reference: ALARM_LOW was 0, which collided with the "nothing should sound"
 * sentinel and made the low-glucose alarm impossible to fire. Levels now come
 * from alarmlogic.h (AL_*), and Java's kind only ever via alarm_java_kind. */

static int g_alarm_state;    /* last reading's zone: 0 ok, 1 low, 2 high */
static int g_alarm_sounding; /* an audible alarm is currently active */

/* settings menu (opened by tapping the big-number band) */
enum {
   MENU_NONE,
   MENU_SETTINGS,
   MENU_KEYPAD,
   MENU_DEVLIST,
   MENU_SENSOR,     /* one sensor's detail screen */
   MENU_CAL,        /* that sensor's calibration panel */
   MENU_CALPEND,    /* a calibration is queued: REPLACE / CANCEL */
   MENU_RESCALE,    /* confirm a rescale value */
   MENU_RESCALEACT, /* rescaling active: CHANGE / STOP */
   MENU_SENSTYPE,   /* sensor-type picker (first step of ADD SENSOR) */
   MENU_FORGET,     /* confirm forgetting a sensor */
   MENU_LABEL,      /* rename a sensor */
   MENU_MARKPICK,   /* marker-shape picker */
   MENU_COLORPICK,  /* colour picker */
   MENU_METERHELP,  /* OneTouch: instructions + Scan */
   MENU_PAIRCONF,   /* confirm pairing the picked device: YES / NO */
   MENU_ADD,        /* main-screen '+': NEW DEVICE / INSULIN */
   MENU_INSULIN,    /* LOG INSULIN entry form */
   MENU_INSLOG,     /* INSULIN LOG: paginated dose table */
   MENU_WEIGHT,     /* LOG WEIGHT entry form */
   MENU_WTLOG,      /* WEIGHT LOG: table + trend plot */
   MENU_WTDEL,      /* confirm deleting a weight entry */
   MENU_DISPLAY,    /* display settings submenu */
   MENU_DEVICES,    /* the device registry: active, old, and ADD NEW DEVICE */
   MENU_PERMS,      /* permissions + background controls */
   MENU_OLDDEV,     /* previously-used (forgotten) devices */
   MENU_RECONF,     /* confirm reconnecting an EXPIRED old device */
   MENU_REMOTE,     /* remote push: enable/disable, server IP and port */
   MENU_INSDEL,     /* confirm deleting an insulin dose */
   MENU_ALARM,      /* alarm submenu: LOW/HIGH thresholds + outputs */
   MENU_EXPORT      /* EXPORT DATA: range + section checkboxes */
}; /* g_menu / g_kp_return values */

/* EXPORT DATA menu state (session-only; the defaults are the whole point:
 * everything, all time). Range 0 = 30 D, 1 = 1 Y, 2 = ALL. */
static int g_exp_range = 2;
static int g_exp_glu   = 1;
static int g_exp_dev   = 1;
static int g_exp_ins   = 1;
static int g_exp_wt    = 1; /* EXPORT DATA: include the weight log */

/* Where the ALARM submenu was opened from -- the settings row, or the main
 * screen's alarm row -- so its X returns exactly there (the origin rule). */
static int g_alarm_from = MENU_SETTINGS;

static int g_old_page;    /* which page the OLD DEVICES list is showing */
static int g_dev_page;    /* which page the LIVE device list is showing */
static int g_inslog_page; /* which page the INSULIN LOG is showing */
static int g_wtlog_page;  /* which page the WEIGHT LOG is showing */
/* LOG WEIGHT form state. The value is held in TENTHS of the DISPLAY unit, not
 * grams: it is what the user typed and what the keypad round-trips, and it is
 * converted once, on CONFIRM. Holding grams here instead would re-render the
 * field every time the unit preference changed mid-entry. */
static long g_wt_t;
static int g_wt_tenths;
static int g_wt_tab;        /* which span the WEIGHT LOG plot is showing */
static int g_wt_scrub = -1; /* scrubbed point in the WEIGHT LOG plot, -1 none */
static int g_wt_scrubbing;  /* a drag across the weight plot is in progress */
/* EDIT WEIGHT: which entry is being edited, and a COPY of it as it was.
 * The copy is the match key for the rewrite -- an index into the tail would
 * go stale the moment the log reloads, and rewriting by position is how an
 * edit lands on the wrong row. -1 means the form is logging a NEW entry. */
static int g_wt_edit = -1;
static struct wt_rec g_wt_orig;
/* EDIT INSULIN: which dose the form edits (-1 = none, logging new), and
 * the ORIGINAL row -- the rewrite matches on it, so edits of a stale tail
 * index can never hit the wrong dose. */
static int g_ins_edit = -1;
static struct ins_rec g_ins_orig;
static int g_markpick_ins = -1; /* INS_SLOW/INS_FAST being styled; -1 =
                                 * the picker edits a sensor's styling */
/* Where the LOG/EDIT INSULIN form was OPENED from -- every exit (X,
 * CANCEL, DELETE, CONFIRM) returns exactly there. Recorded at each entry
 * point, never inferred: inferring the return target is the recurring
 * menu-navigation bug this app keeps re-growing. */
/* WHERE THE FOUR LOGGING SCREENS WERE OPENED FROM.
 *
 * They used to hardcode MENU_ADD, which was true while the ADD menu was their
 * only door. The main screen's PIN buttons are a second door, so a
 * hardcoded return drops the user into a menu they never opened -- the exact
 * failure the record-the-origin rule exists to prevent, and one this codebase
 * has now hit on the devices screen, the sensor screen and the pairing flow.
 * Captured at open, never inferred at close. */
static int g_ins_from    = MENU_ADD;
static int g_wt_from     = MENU_ADD;
static int g_wtlog_from  = MENU_ADD;
static int g_inslog_from = MENU_ADD;

/* LOG INSULIN form state. The instant is edited as a whole (date and time
 * steppers both move g_ins_t); units re-populate from the last dose of the
 * selected type when the form opens or the type toggles. */
static long g_ins_t;
static int g_ins_type, g_ins_units;

/* The device a pick proposed, awaiting the PAIRCONF YES. Copied out of
 * g_devs at pick time: the candidate list keeps churning under the scan (and
 * is reset outright by pair_scan_start), so an index would go stale but a
 * copied identity cannot. Main-thread only. */
static char g_pend_mac[20], g_pend_name[12];

/* Smart pairing (PAIR NEW SENSOR): scans for candidates while the code is
 * typed, WITHOUT touching the currently-bonded sensor. On OK, pick by
 * proximity/count (see select_candidate); ambiguous -> MENU_DEVLIST for the
 * user to choose. */
static int g_smart_pairing;

/* PENDING pairing: the code is in but no candidate is on the air yet. The
 * old flow parked the user in the device list until the sensor deigned to
 * advertise -- with g_smart_pairing suppressing every OTHER sensor's
 * reconnect the whole time, so waiting for the new sensor cost the readings
 * of the ones already worn. Instead the intent is ARMED (the type awaited;
 * 0 = none), every menu closes, and the 1 Hz tick commits the pairing the
 * moment an unambiguous candidate appears. DEVICES shows a PENDING row
 * (tappable to cancel) so the armed state is visible, not mysterious. */
static int g_pend_pairing;

static int g_menu; /* which modal screen is open */

/* ---- act-on-RELEASE ----
 * A press only ARMS the control under the finger; the action fires on the
 * RELEASE, and only if it lands back on that same control -- so sliding the
 * finger off first is a free cancel, and nothing else can fire until the
 * next press. While armed and on-target, draw_impl lightens the control's
 * whole hit rectangle (ui_press_overlay): one consistent pressed visual for
 * every screen, with no per-renderer work. Exempt BY DESIGN: plot scrubbing
 * (the drag itself is the interaction), the alarm +- steppers (step on
 * press + auto-repeat on hold is their whole point), and silencing a
 * sounding alarm (any press must silence IMMEDIATELY -- see on_input). */
static int g_arm_kind = ACT_NONE; /* armed action; ACT_NONE = none */
static int g_arm_arg;
static int g_arm_in;         /* finger currently on the armed control */
static int g_arm_x, g_arm_y; /* where it last touched it (finds the box) */
static int g_gate;           /* first-run permission-rationale screen */
static int g_want_battery;   /* pop battery-opt prompt after perms */
/* DISCONNECT-alarm minutes. The shortest MUST stay above AL_FRESH_S (11 min),
 * or a fresh-but-out-of-range zone outranks AL_STALE and delays the label the
 * threshold exists to produce -- see alarm_stranded in alarmlogic.h. This was
 * 10, i.e. under the window, for exactly as long as AL_FRESH_S was 6 min.
 * ui_disc_lbl (ui.c) prints these; the two tables move together. */
static const int disc_min[] = {0, 15, 30, 60};
static long g_launch_t; /* for the stale-alarm grace period */
/* Per-CGM-link DIS strings. g_model/g_fw (settings.c) are process-global and
 * shared by every link, which is fine for the headline display but WRONG for
 * provenance -- see pancra_devinfo. Minting uses these. */
static char g_model_l[LINK_MAX][24], g_fw_l[LINK_MAX][24];
static int g_disc_alarmed; /* stale alarm currently latched */
/* NUDGE latch: which nudge band the last KNOWN reading was in (NG_*).
 *
 * State, not a derived value, because the nudge announces a CROSSING and a
 * crossing cannot be read off a single sample. Guarded by alarm_lock like
 * every other alarm state, and deliberately NOT persisted: after a restart the
 * app has no idea whether the user already heard this crossing, and the honest
 * default is "not announced yet" -- a nudge too many is a blip, a nudge too
 * few is the reminder that never arrives. */
static int g_nudge_state;

static const char *perms[] = {"android.permission.BLUETOOTH_SCAN",
                              "android.permission.BLUETOOTH_CONNECT",
                              "android.permission.POST_NOTIFICATIONS"};
/* Cached system states for the settings menu. JNI via the activity's env is
 * only legal on the main thread, so sys_* are never called from a render (which
 * can be requested off a BLE binder thread); sys_refresh samples them from the
 * main thread (menu open / after an action) and build_model just copies them.
 */
static int g_sys_perm[NPERMS], g_sys_batt, g_sys_bucket, g_sys_bg;
/* 64: the widest thing typed here is an email address or a host name, both of
 * which are 63-byte fields. It was 24, which silently truncated an address at
 * 23 characters -- and the truncation only became visible when pairing failed
 * against an account that did not exist. */
static char g_entry[64];
static int g_entrylen;   /* keypad entry buffer */
static int g_kp_mode;    /* keypad: 0 = pair code, 1 = plot max, 2 = cal */
/* THREE fields share the alphanumeric editor -- a sensor name, the sync
 * server and the account email -- and they commit to different places, so
 * which one opened it has to be recorded rather than guessed from the menu. */
enum { LABEL_SENSOR = 0, LABEL_SERVER = 1, LABEL_EMAIL = 2 };
static int g_label_field;
static int g_prog_shown; /* the eased sync fraction, per mille */
static int
    g_cal_pending; /* calibration value awaiting CONFIRM, mg/dL; 0 = none */
/* DURABLE calibration queue: a CONFIRMED calibration that has not yet been
 * ACCEPTED by the sensor. It is persisted to disk and retried on every stream
 * until the sensor answers -- so a calibration is NEVER lost to a reconnect gap
 * or an app restart, the way a one-shot write silently was. */
static int g_calq_mgdl;  /* queued value, mg/dL; 0 = none queued */
static int g_calq_id;    /* sensor id it is for */
static long g_calq_t;    /* realtime_s() when the user confirmed it */
static long g_calq_sent; /* realtime_s() of the last write attempt; 0 = none */
static char g_calq_status[28]; /* user-visible outcome line */
static char g_calq_path[256];  /* persistence file */
/* Last RESOLVED calibration, for the per-device LAST CAL row (persisted). */
static int g_lastcal_mgdl; /* value of the last resolved calibration */
static long g_lastcal_t;   /* realtime_s() it resolved; 0 = never */
static int
    g_lastcal_state;     /* CAL_ST_* (ui.h): applied/rejected/notsup/failed */
static int g_lastcal_id; /* sensor id it was for */
/* Give up (visibly, never silently) if the sensor has not accepted within this
 * long -- a fingerstick reference goes stale, so past this we tell the user to
 * re-enter rather than apply an old value or drop it without a word. */
#define CALQ_WINDOW_S (20L * 60)

/* RESCALE: a persistent multiplicative correction (permille; 1000 = none) the
 * user sets from a fingerstick. Applied to THIS CGM's readings whose timestamp
 * is AT OR AFTER the moment rescaling was (re)activated -- so a backfilled
 * point with an OLDER timestamp is never rescaled even though it arrives later.
 * The stored/plotted/alarmed glucose is the rescaled value; the raw stays
 * recoverable via the CSV `rescale` column. Clamped to +-25%. */
static int g_rescale_pm = 1000; /* active factor; 1000 = off */
static int g_rescale_id;        /* sensor id it applies to */
static long g_rescale_t;        /* activation instant; readings t>=this scale */
static int g_rescale_entry;     /* value awaiting CONFIRM (mg/dL); 0 = none */
/* PENDING target: a confirmed rescale that could not be computed yet because no
 * live reading was available. Held (PERSISTED, incl. its request time) until
 * the next reading for this sensor, then turned into a factor -- never silently
 * lost, and it survives an app restart. The reading it computes against must be
 * at most RESCALE_PEND_WINDOW_S newer than the request, or the fingerstick
 * reference is stale and the pending EXPIRES. */
static int g_rescale_pend_mgdl; /* target mg/dL awaiting a reading; 0 = none */
static int g_rescale_pend_id;   /* sensor id it is for */
static long g_rescale_pend_t;   /* realtime_s() when the user requested it */
#define RESCALE_PEND_WINDOW_S (15L * 60)
/* Last attempt exceeded +-25% and was REJECTED (not clamped), or a pending one
 * EXPIRED. Shown in the RESCALE line until the user sets a valid one or stops.
 * In-memory only (a transient notice). */
static int g_rescale_reject;
static int g_rescale_reject_id;
static int g_rescale_expired;
static int g_rescale_expired_id;
static char g_rescale_path[256]; /* persistence file */
static int
    g_link_raw[LINK_MAX]; /* latest RAW (pre-rescale) reading, per link */
/* Per-link signal strength: see pancra_rssi. Retained "last known", like the
 * globals it backs, so it never expires into "--" while readings lag. */
static int g_link_rssi[LINK_MAX], g_link_rssi_ok[LINK_MAX];
static long g_link_rssi_t[LINK_MAX];
#define RESCALE_MIN_PM 750  /* -25% */
#define RESCALE_MAX_PM 1250 /* +25% */

/* raw * factor, rounded. */
static int rescale_apply(int raw, int pm)
{
   return (int)((((long)raw * pm) + 500) / 1000);
}

/* The factor to apply to a reading (src, timestamp t), or 1000 (none). */
static int rescale_pm_for(int src, long t)
{
   if (g_rescale_pm != 1000 && g_rescale_id == src && t >= g_rescale_t)
      return g_rescale_pm;
   return 1000;
}

/* Turn a (target, raw) pair into an active factor for sensor `id`, effective
 * from `t`. A factor beyond +-25% is REJECTED (not clamped) -- the reading is
 * too far from the entered value to be a plausible correction -- and flagged
 * for the RESCALE line. Returns 1 if it activated, 0 if rejected or
 * uncomputable. */
static int rescale_activate(int id, int target_mgdl, int raw, long t)
{
   if (raw <= 0 || target_mgdl <= 0)
      return 0;
   int pm = (int)((((long)target_mgdl * 1000) + (raw / 2)) / raw);
   if (pm < RESCALE_MIN_PM || pm > RESCALE_MAX_PM) {
      g_rescale_reject    = 1;
      g_rescale_reject_id = id;
      LOGI("rescale REJECTED: %d mg/dL over raw %d -> %d permille exceeds "
           "+-25%%",
           target_mgdl, raw, pm);
      return 0;
   }
   g_rescale_pm     = pm;
   g_rescale_id     = id;
   g_rescale_t      = t;
   g_rescale_reject = 0; /* a valid one clears any prior rejection */
   LOGI("rescale active: %d mg/dL over raw %d -> %d permille (id %d)",
        target_mgdl, raw, pm, id);
   return 1;
}

static void rescale_save(void)
{
   int fd = open(g_rescale_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[96];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%d,%ld\n", g_rescale_id,
                    g_rescale_pm, g_rescale_t, g_rescale_pend_id,
                    g_rescale_pend_mgdl, g_rescale_pend_t);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) < 0) { /* best effort */
   }
   close(fd);
}

static void rescale_load(void)
{
   int fd = open(g_rescale_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[96];
   long n = read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]      = 0;
   long v[6] = {0, 0, 0, 0, 0, 0};
   int vi    = 0;
   int neg   = 0;
   for (char *p = b; *p && vi < 6; p++) {
      if (*p >= '0' && *p <= '9') {
         v[vi] = (v[vi] * 10) + (*p - '0');
      } else if (*p == '-') {
         neg = 1;
      } else if (*p == ',' || *p == '\n') {
         if (neg)
            v[vi] = -v[vi];
         neg = 0;
         vi++;
         if (*p == '\n')
            break;
      }
   }
   if (v[1] >= RESCALE_MIN_PM && v[1] <= RESCALE_MAX_PM && v[1] != 1000) {
      g_rescale_id = (int)v[0];
      g_rescale_pm = (int)v[1];
      g_rescale_t  = v[2];
   }
   if (v[4] > 0) { /* a target was awaiting a reading when we last ran */
      long pend_t = v[5];
      /* Restart must NOT lose a pending rescale -- BUT only honour it if its
       * request is still within the freshness window; a target set before a
       * long downtime has a stale fingerstick reference and must not silently
       * apply to a much-later reading. */
      if (pend_t > 0 && realtime_s() - pend_t <= RESCALE_PEND_WINDOW_S) {
         g_rescale_pend_id   = (int)v[3];
         g_rescale_pend_mgdl = (int)v[4];
         g_rescale_pend_t    = pend_t;
      } else {
         g_rescale_expired    = 1; /* surface it, never a silent drop */
         g_rescale_expired_id = (int)v[3];
      }
   }
}

static void calq_try_locked(void); /* defined after cal_select; used earlier */
static void calq_tick(void);
static void calq_load(void);
static int g_kp_return;    /* keypad close target: 0 = main, 1 = settings */
static int g_timerfd = -1; /* shared repaint / repeat timer */
static struct ALooper
    *g_looper;       /* main looper, to remove the timer fd on destroy */
static int g_inited; /* process-wide one-time init done (relaunch guard) */
/* press-and-hold plot scrub (the plot rect itself comes from the recorded
 * ACT_SCRUB hit box via plot_rect) */
static int g_scrub_idx = -1; /* highlighted point, -1 = none */
static int g_scrubbing;      /* a plot drag is in progress */
static int g_scrub_ins;      /* what that drag scrubs: 1 insulin, 0 glucose.
                              * Latched at the DOWN and held for the whole
                              * gesture, so a finger wandering across the
                              * band boundary (or off the plot) keeps
                              * scrubbing what it started on. */

/* fmt_glu / fmt_trend / fmt_hms are UI display formatters (declared in ui.h);
 * white_color is the notification plot's dot-colour callback. */
static uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFF;
} /* plot dots */

/* draw_str / fmt_glu / str_snapshot live in ui.c (rendering primitives) */
#define UNIT_LBL (g_units ? "MMOL/L" : "MG/DL")

static void start_scan(struct ANativeActivity *a);
static void stop_scan(struct ANativeActivity *a);
/* Force a genuine scan restart. start_scan alone CANNOT do this: it early
 * returns whenever g_scanning is set, which it always is while the UI is up. */
static void scan_restart(struct ANativeActivity *a);
static void meter_sync_start(int mid, const char *mac);
static int meter_armed(const char *mac);
static int meter_index_all(int *ids, int *vals, int cap);
static int meter_index_load(int id);
static void request_ble_permissions(struct ANativeActivity *a);

/* touch targets recorded by the last main-screen ui_render(); read by on_input
 * to map a tap to an action. Rebuilt on the main thread only. */
static struct hits g_hits;

/* Registry id of the sensor the Dexcom driver is currently bonded to, stamped
 * onto every reading it produces. 0 means "not yet identified", which is the
 * same id legacy pre-registry rows carry, so old and new data stay consistent.
 */
static int g_cur_src;
/* Registry id of the meter, and when it last synced. The meter's own record
 * index is persisted so a reconnect never re-reads what we already hold. */
static int g_meter_src;
static int g_meter_busy; /* a meter PROTOCOL SYNC is running */
/* The link that sync is running on. otble.c holds ONE protocol state, so only
 * one meter can be mid-exchange at a time -- but every meter now has its own
 * link and its own standing connect, so which link that is has to be tracked
 * rather than assumed. -1 = none. */
static int g_meter_link = -1;
/* Which links carry a meter, mirroring what the transport was told. The
 * shell needs its own copy because several CGM-only passes below iterate the
 * links and must skip meters -- a check that used to be `l == LINK_METER` and
 * is now a per-link fact. Written only through link_set_meter, so the two
 * copies cannot drift. */
static int g_link_meter[LINK_MAX];

/* The link a meter's standing connect is outstanding on, or -1.
 *
 * ITS OWN TABLE, not the driver session. The session's address is stamped by
 * the DEXCOM handshake, and a meter never runs one -- so reading it back
 * reported every meter as unarmed, the tick re-armed on every pass, and each
 * pass issued a fresh connectGatt: a connect per second, forever, which is
 * both a battery burn and a live risk of cancelling the connection during
 * the one second the meter is actually awake. Measured on the device before
 * this table existed: the same meter armed four times in four seconds. */
static char g_link_armed[LINK_MAX][24];

/* When a meter link was left ARMED with no exchange running -- i.e. when
 * ot_drv_disconnect asked for the close and handed the link back to the
 * transport to tear down. 0 means "not waiting on a teardown".
 *
 * This exists because ot_drv_disconnect deliberately does NOT un-arm (that
 * un-arm was what let the 1 Hz tick reconnect a still-awake meter and re-run
 * the whole exchange 29 times in 29 seconds). Waiting for the real GATT
 * disconnect is right, but it removed the only thing that guaranteed the
 * link ever came back: meter_sync_watchdog fires only while g_meter_busy is
 * set, and this state has it clear, while pancra_link_watchdog skips meter
 * links outright. A lost disconnect callback would therefore strand the link
 * armed forever and that meter would never sync again until a restart. */
static long g_link_idle_t[LINK_MAX];

/* Both link tables are written from the MAIN thread and read from BINDER
 * threads (pancra_meter_connected, pancra_devinfo), so every write takes
 * driver_lock -- the lock those readers already hold. g_link_armed is a
 * 24-byte string, and a str_snapshot racing a strcmp really can be read
 * half-written, which would mis-identify the meter that just connected.
 * driver_lock is recursive, and no caller holds sensors_lock here, so this
 * cannot invert the documented driver -> reg order. */
static void link_set_meter(int link, int on)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   driver_lock();
   g_link_meter[link] = on ? 1 : 0;
   dexble_set_meter_link(link, on);
   driver_unlock();
}

/* Give a link back: no longer armed, and no longer a METER link.
 *
 * Clearing the meter bit is the part that was missing. link_set_meter was
 * only ever called with 1, so a link stayed marked "meter" for the life of
 * the process -- and since the link pool is shared, a CGM that later landed
 * on it would have had its notifications routed into the OneTouch parser.
 * Releasing is the other half of arming and has to undo both facts. */
/* Un-arm: this meter no longer has a connection outstanding, so the tick may
 * arm it again. The link keeps its METER ROUTING BIT -- see below. */
static void meter_unarm_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   g_link_idle_t[link] = 0;
   driver_lock();
   g_link_armed[link][0] = 0;
   driver_unlock();
}

/* Give the link back completely: un-armed AND no longer routed to otble.
 *
 * Only correct once the GATT disconnect has actually ARRIVED. Clearing the
 * routing bit at the moment we ASK for a close is too early: the callback is
 * still in flight, and with the bit gone it lands in the CGM branch instead,
 * running the Dexcom disconnect logic on a link that has no Dexcom session --
 * which posts "CONNECTION ERROR" after every successful meter sync. */
static void meter_release_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   g_link_idle_t[link] = 0; /* no longer waiting on a teardown */
   driver_lock();
   g_link_armed[link][0] = 0;
   link_set_meter(link, 0);
   driver_unlock();
}

static long g_meter_start; /* when it started, for the stall watchdog */
/* Meter link RSSI, sampled during its sync connection (the meter is off between
 * syncs, so this is the last-sync signal strength shown in its SIGNAL row). */
static int g_meter_rssi, g_meter_rssi_ok;
static long g_meter_rssi_t;

/* PER-METER runtime state, keyed by registry id: when this meter was last
 * connected/synced and the RSSI then. In-memory (reset per launch). The global
 * g_meter_* above only ever hold the LAST meter, which with two meters made one
 * meter's sync throttle the other (a global 60 s gate) and show one meter's
 * signal/sync-time against both. */
/* How long after a finished exchange the tick refuses to re-arm that meter.
 *
 * This is only a second line of defence: ot_drv_disconnect keeping the link
 * armed until the real disconnect is what actually stops the re-sync loop,
 * and this exists so a link that FLAPS -- drops and is immediately reachable
 * again -- cannot walk back into a sync per second.
 *
 * It was 60 s, which was too blunt. The link stays armed for the ~35 s a
 * Verio remains awake, so a 60 s gate kept biting for ~25 s AFTER the
 * teardown -- a window with no standing connect at all, during which a
 * second fingerstick was caught by nothing (the advert path carries its own
 * pre-existing 60 s per-meter throttle). 15 s is still ten times the 1.5 s
 * period of the loop this guards against, and being shorter than the awake
 * time means it has always expired by the time the link is actually free,
 * so it never delays a legitimate re-arm. */
#define METER_REARM_COOLDOWN 15

/* How long a link may sit armed waiting for a disconnect callback before the
 * watchdog assumes it was lost. See the recovery in meter_sync_watchdog. */
#define METER_TEARDOWN_MAX 180

struct meter_rt {
   int id;
   long sync_t; /* last connect/sync of THIS meter (0 = never this launch) */
   /* When the last protocol exchange FINISHED. Distinct from sync_t, which
    * also moves on a bare advertisement: this one means "we actually talked
    * to it and finished", and it is what the tick's re-arm cooldown reads so
    * a meter that is still awake is not immediately synced again. */
   long synced_t;
   int rssi, rssi_ok;
   long rssi_t;
   char stat[24]; /* last driver phase text (HELLO/COUNT/READING/SYNCED/...) */
};
static struct meter_rt g_meter_rt[MAX_SLOTS];
static int g_meter_nrt;

static struct meter_rt *meter_rt_get(int id, int create)
{
   for (int i = 0; i < g_meter_nrt; i++)
      if (g_meter_rt[i].id == id)
         return &g_meter_rt[i];
   if (create &&
       g_meter_nrt < (int)(sizeof g_meter_rt / sizeof g_meter_rt[0])) {
      struct meter_rt *r = &g_meter_rt[g_meter_nrt++];
      *r                 = (struct meter_rt){0};
      r->id              = id;
      return r;
   }
   return 0;
}

static char g_meter_mac[24];
static char g_meter_model[24], g_meter_fw[24];
static char g_meter_path[256];
static char g_metersync_path[256]; /* per-meter last-sync time, persisted */

/* Persist every meter's last-sync wall-clock so "LAST SYNC" and the
 * DEVICES-list age survive a restart (rt is otherwise in-memory only and reset
 * to 0 on every launch, which made a fresh install read "OFF / NEVER" for a
 * meter that had in fact synced). Rewrite-and-rename, like meter.idx, so a
 * crash never truncates it to nothing. */
static void meter_sync_save(void)
{
   char tmp[300];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_metersync_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   int ok = 1;
   for (int i = 0; i < g_meter_nrt && ok; i++) {
      if (g_meter_rt[i].sync_t <= 0)
         continue;
      /* id, last-seen time, and the RSSI captured THEN (so SIGNAL STRENGTH is
       * the signal AT last-seen, and survives a restart -- it used to be
       * in-memory only, so a meter read "-- signal" despite a real LAST SEEN).
       */
      char b[64];
      int bn = snprintf(b, sizeof b, "%d,%ld,%d,%d\n", g_meter_rt[i].id,
                        g_meter_rt[i].sync_t, g_meter_rt[i].rssi,
                        g_meter_rt[i].rssi_ok);
      bn     = clampn(bn, sizeof b);
      if (write(fd, b, bn) != bn)
         ok = 0;
   }
   close(fd);
   if (ok) {
      if (rename(tmp, g_metersync_path) != 0)
         unlink(tmp);
   } else {
      unlink(tmp);
   }
}

/* ---- LAST-KNOWN SESSION, per sensor, across restarts -------------------
 *
 * The session clock, the state byte and the prediction all live in the DRIVER,
 * which is per-process state built from 0x4e responses. The last reading, its
 * trend and its age survive a restart because they are replayed from
 * readings.csv -- so after every launch the main screen showed a glucose value
 * and its age while PRED and the session countdown sat blank for up to a full
 * five-minute cadence, waiting for the sensor to answer. Two of the four
 * numbers on one line disappearing, repeatedly, for no reason the user can
 * see.
 *
 * So cache them. The clock is stored WITH the wall time it was read at, and
 * projected forward on load exactly as driver_get_session projects it between
 * responses -- storing the raw number would restore a countdown frozen at
 * whenever the app last ran, which is worse than blank because it looks live.
 *
 * The live driver ALWAYS wins: this is consulted only while have_reading is
 * still 0 for that link, i.e. before its first response of this process. */
struct sess_cache {
   long clock_t; /* wall clock when `clock` was read */
   long clock;   /* session seconds at that instant */
   int id, state, predicted, sequence;
};
static struct sess_cache g_sessc[MAX_SLOTS];
static int g_nsessc;
static char g_sess_path[256];
/* NOT "when the newest change happened" -- that never fires.
 *
 * driver_get_session projects the clock forward by wall time, so it reads a
 * DIFFERENT value every second and sessc_put marks a change every second. A
 * "quiet for 60 s" rule is therefore never satisfied while a sensor is
 * connected, and session.cache was never written at all -- the restore this
 * exists for silently did nothing on every launch. Rate-limit against the last
 * SAVE instead, which is what was meant. */
static int g_sess_dirty;  /* a change is pending */
static long g_sess_saved; /* wall time of the last successful save */

static struct sess_cache *sessc_get(int id, int create)
{
   for (int i = 0; i < g_nsessc; i++)
      if (g_sessc[i].id == id)
         return &g_sessc[i];
   if (create && g_nsessc < MAX_SLOTS) {
      struct sess_cache *c = &g_sessc[g_nsessc++];
      *c                   = (struct sess_cache){0};
      c->id                = id;
      return c;
   }
   return 0;
}

static void sess_save(void)
{
   char tmp[300];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_sess_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   int ok = 1;
   for (int i = 0; i < g_nsessc && ok; i++) {
      if (g_sessc[i].id <= 0 || g_sessc[i].clock_t <= 0)
         continue;
      char b[96];
      int bn = snprintf(b, sizeof b, "%d,%ld,%ld,%d,%d,%d\n", g_sessc[i].id,
                        g_sessc[i].clock_t, g_sessc[i].clock, g_sessc[i].state,
                        g_sessc[i].predicted, g_sessc[i].sequence);
      bn     = clampn(bn, sizeof b);
      if (write(fd, b, bn) != bn)
         ok = 0;
   }
   close(fd);
   if (ok) {
      if (rename(tmp, g_sess_path) != 0)
         unlink(tmp);
   } else {
      unlink(tmp);
   }
}

static void sess_load(void)
{
   int fd = open(g_sess_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[1024];
   int n = (int)read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]    = 0;
   char *p = b;
   while (*p) {
      long v[6] = {0, 0, 0, 0, 0, 0};
      int vi    = 0;
      int any   = 0;
      while (*p && *p != '\n') {
         if (*p >= '0' && *p <= '9') {
            /* Digit-capped, like every other loader here: an unbounded
             * accumulation is UB and a wrapped value would restore a
             * nonsense clock that the projection below then counts on. */
            if (v[vi] < 100000000000000000L)
               v[vi] = (v[vi] * 10) + (*p - '0');
            any = 1;
         } else if (*p == ',' && vi < 5) {
            vi++;
         }
         p++;
      }
      if (*p == '\n')
         p++;
      if (any && v[0] > 0) {
         struct sess_cache *c = sessc_get((int)v[0], 1);
         if (c) {
            c->clock_t   = v[1];
            c->clock     = v[2];
            c->state     = (int)v[3];
            c->predicted = (int)v[4];
            c->sequence  = (int)v[5];
         }
      }
   }
}

/* Record a LIVE session for `id`. Cheap enough to call every frame, but only
 * marks the file dirty when the clock actually moved -- the flush is on the
 * 1 Hz tick, so a redraw storm cannot turn this into a write storm. */
static void sessc_put(int id, const struct dex_session *s, long now)
{
   if (id <= 0 || !s->have_reading)
      return;
   struct sess_cache *c = sessc_get(id, 1);
   if (!c)
      return;
   if (c->clock == (long)s->session_seconds && c->state == s->state &&
       c->predicted == s->predicted)
      return; /* nothing new to persist */
   c->clock_t   = now;
   c->clock     = (long)s->session_seconds;
   c->state     = s->state;
   c->predicted = s->predicted;
   c->sequence  = s->sequence;
   g_sess_dirty = 1;
}

/* Fill `out` from the cache for `id`, projecting the clock forward to `now`.
 * Returns 1 if a usable cached session was restored.
 *
 * REFUSED once the cache is older than SENSOR_ACTIVE_S: past that the sensor
 * has been silent for a day and a countdown derived from it would be fiction
 * presented as a live reading. Refused for a non-positive clock too -- a
 * session that never started has nothing to project. */
static int sessc_restore(int id, long now, struct dex_session *out)
{
   struct sess_cache *c = sessc_get(id, 0);
   if (!c || c->clock_t <= 0 || c->clock <= 0)
      return 0;
   long dt = now - c->clock_t;
   if (dt < 0 || dt > SENSOR_ACTIVE_S)
      return 0;
   out->have_reading    = 1;
   out->session_seconds = (unsigned)(c->clock + dt);
   out->state           = c->state;
   out->predicted       = c->predicted;
   out->sequence        = c->sequence;
   /* NOT bonded: that is a live-link fact and nothing here can vouch for it.
    * The connected/WAITING status is driven by it, and claiming a bond we do
    * not have would paint a dead sensor green. */
   return 1;
}

static void meter_sync_load(void)
{
   int fd = open(g_metersync_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[1024];
   int n = (int)read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]    = 0;
   char *p = b;
   while (*p) {
      long v[4] = {0, 0, 0, 0}; /* id, sync_t, rssi (signed), rssi_ok */
      int vi    = 0;
      int neg   = 0;
      int any   = 0;
      while (*p && *p != '\n') {
         if (*p >= '0' && *p <= '9') {
            v[vi] = (v[vi] * 10) + (*p - '0');
            any   = 1;
         } else if (*p == '-') {
            neg = 1;
         } else if (*p == ',' && vi < 3) {
            if (neg)
               v[vi] = -v[vi];
            neg = 0;
            vi++;
         }
         p++;
      }
      if (neg)
         v[vi] = -v[vi];
      if (*p == '\n')
         p++;
      if (any && v[0] > 0) {
         struct meter_rt *rt = meter_rt_get((int)v[0], 1);
         if (rt) {
            rt->sync_t = v[1];
            if (v[3]) { /* rssi_ok: the signal at last-seen is known */
               rt->rssi    = (int)v[2];
               rt->rssi_ok = 1;
               rt->rssi_t  = v[1]; /* captured at the last-seen time */
            }
         }
      }
   }
}

/* Which sensor a detail screen is showing (index into g_slot), and the type
 * chosen in the ADD SENSOR flow. */
static int g_sel      = -1;
static int g_add_type = SENSOR_STELO;
/* Where the per-device menu was opened FROM, so closing it returns there:
 * MENU_DEVICES when reached via the device list, MENU_OLDDEV via the old-device
 * list, MENU_NONE (main screen) when reached via the STATE/STORED info-block
 * shortcut. */
static int g_sensor_from = MENU_NONE;
/* Where the DEVICES screen was opened FROM -- MENU_NONE for the main screen's
 * big number, MENU_SETTINGS for the settings row -- so its X returns exactly
 * there. Recorded, never inferred (the recurring bug). */
static int g_devices_from = MENU_NONE;
/* Where the PAIRING flow (type tap -> keypad / meter help) was entered
 * from: the ADD menu or the ADD DEVICE picker. Every abort path returns
 * exactly there -- recorded, never inferred (the recurring bug). */
static int g_pair_from = MENU_SENSTYPE;

/* Build "<dir><name>" into a bounded buffer. Six call sites used to open-code
 * this same loop. */
static void data_path(char *dst, int cap, const char *dir, const char *name)
{
   int i = 0;
   for (; dir[i] && i < cap - 32; i++)
      dst[i] = dir[i];
   for (int j = 0; name[j] && i < cap - 1; j++)
      dst[i++] = name[j];
   dst[i] = 0;
}

/* Map a sensor slot to its transport link. CGMs take LINK_CGM, then LINK_CGM2
 * and upward in slot order; meters take links from the SAME pool. Each link has
 * its
 * own GATT connection, operation queue and driver context, so sensors run
 * genuinely concurrently rather than taking turns. */
/* The CGM link whose driver context is bound to `identity`, or -1.
 * Caller must hold driver_lock (it is recursive, so nesting is fine). */
static int link_for_identity(const char *identity)
{
   if (!identity || !identity[0])
      return -1;
   int prev  = driver_link();
   int found = -1;
   /* EVERY link, including a meter's. drv_connect stamps the address into the
    * link's session whichever kind of device it is, so one lookup binds both
    * -- and reserving a link for "the meter" is exactly what stopped a second
    * and third meter from ever holding a connection of their own. */
   for (int l = 0; l < LINK_MAX && found < 0; l++) {
      driver_select(l);
      struct dex_session s;
      driver_get_session(&s);
      if (s.mac[0] && strcmp(s.mac, identity) == 0)
         found = l;
   }
   driver_select(prev);
   return found;
}

/* The (rank+1)'th CGM link with no session bound to it, or -1 if there are
 * fewer than that many free. Caller must hold driver_lock.
 *
 * The rank matters: returning simply "the lowest free link" gave every unbound
 * sensor the SAME answer, and after a restart no link has a session yet -- so
 * two registered CGMs would both be routed to LINK_CGM and fight over it, the
 * second clobbering the first. Ranking restores the distinctness the old
 * ordinal scheme had, without reintroducing its instability: a sensor that IS
 * bound never reaches here, so a forget cannot renumber a live sensor. */
static int free_cgm_link(int rank)
{
   int prev  = driver_link();
   int found = -1;
   int seen  = 0;
   for (int l = 0; l < LINK_MAX && found < 0; l++) {
      /* SKIP A LINK A METER HOLDS. drv_connect does not stamp the driver
       * session -- only the Dexcom handshake does -- so a meter's link reads
       * as having no session and looked FREE here. A CGM allocated onto it
       * would have taken over the meter's GATT client and, with the link
       * still routed to otble, fed its sensor notifications to the OneTouch
       * parser. The armed table is the only record that the link is taken. */
      if (g_link_armed[l][0] || g_link_meter[l])
         continue;
      driver_select(l);
      struct dex_session s;
      driver_get_session(&s);
      if (!s.mac[0] && seen++ == rank)
         found = l;
   }
   driver_select(prev);
   return found;
}

/* Map a sensor slot to its transport link, BY ADDRESS.
 *
 * This used to derive the link from a sensor's ORDINAL among the CGM slots,
 * which is not stable: sensor_forget_slot() shifts g_slot while the remaining
 * sensors keep their live GATT connections, driver contexts and per-link key
 * files. Forgetting the first of two CGMs therefore re-pointed the survivor at
 * an emptied context, after which commit_pair() would call driver_forget() on
 * the link the survivor was ACTUALLY using and destroy its bond; calibration
 * went to a dead context while still logging "submitted"; and the survivor's
 * adverts stopped resolving to a live link. Resolving by the session address
 * -- the one identity a shift cannot move -- removes the whole class.
 *
 * idx == g_nslot is a legitimate query ("the link a NEW sensor would take"),
 * which is what free_cgm_link answers. */
static int link_for_slot(int idx)
{
   /* COPY the identity out under the registry lock rather than holding a
    * `struct sensor_rec *` across the driver calls below: srec_push() memmoves
    * g_srec when the cache is full, from a binder thread via ot_drv_done, so a
    * retained pointer can be overwritten mid-use. This runs on a binder thread
    * itself (jni_on_advert) while the main thread may be inside
    * sensor_forget_slot's shift-down, and its result is fed to dexble_pair --
    * a torn read here connects one sensor's address on another's link, using
    * the wrong key file. */
   char ident[24];
   ident[0] = 0;
   int kind = KIND_CGM;
   int have = 0;
   sensors_lock();
   if (idx >= 0 && idx < g_nslot) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[idx].id);
      if (r) {
         str_snapshot(ident, sizeof ident, r->identity);
         kind = sensor_kind(r->type);
         have = 1;
      }
   }
   sensors_unlock();
   (void)kind; /* meters and CGMs allocate by the same rule now */
   driver_lock();
   int link = have ? link_for_identity(ident) : -1;
   if (link < 0) {
      /* Not yet bound (no session on any link -- the normal state right after
       * a restart). Rank this slot among the OTHER unbound slots so each one
       * claims a different free link.
       *
       * Every registered device is ranked, meters included: a meter needs a
       * link of its own to hold a standing connect, because it is reachable
       * only for the second or two it is switched on. Ranking by slot order
       * means a device that IS bound never reaches here, so a forget cannot
       * renumber a live one. */
      int rank = 0;
      sensors_lock(); /* driver -> reg, the documented order */
      for (int i = 0; i < idx && i < g_nslot; i++) {
         if (g_slot[i].old)
            continue; /* retired: holds no link */
         const struct sensor_rec *q = sensor_rec_by_id(g_slot[i].id);
         if (!q)
            continue;
         char qid[24];
         str_snapshot(qid, sizeof qid, q->identity);
         if (link_for_identity(qid) < 0)
            rank++;
      }
      sensors_unlock();
      link = free_cgm_link(rank);
   }
   driver_unlock();
   return link;
}

/* Per-frame REGISTRY snapshot, taken before the draw flag.
 *
 * The renderer used to read g_slot[] and hold a `const struct sensor_rec *`
 * from sensor_rec_by_id() across a run of field copies, with only the draw
 * flag held. Both are mutated from a binder thread: sensor_rebind_slot writes
 * g_slot[i].id, and srec_push memmoves all of g_srec down by one, both
 * reachable via ot_drv_done -> sensor_mint. A memmove landing mid-copy renders
 * a garbled or mixed sensor row, and snap_link_for_slot's strcmp against a
 * shifting identity can match the WRONG link, putting one sensor's session
 * age and CONNECTED state on another's row.
 *
 * It cannot be fixed by taking sensors_lock in the renderer: that would nest
 * reg INSIDE hist and invert the documented driver -> reg -> hist order. So
 * snapshot first, exactly as snap_drivers() does for the driver state, and for
 * the same reason -- the main thread must never hold one of these locks while
 * waiting for another. */
struct snap_slot {
   long paired, activation;
   int id, marker, color, primary, size, wear_days, old, type, have_rec;
   char label[20];
   char mac[24], serial[24], model[24], fw[24];
};

static struct snap_slot g_snap_slot[MAX_SLOTS];
static int g_snap_nslot;

static void snap_registry(void)
{
   sensors_lock();
   g_snap_nslot = g_nslot < MAX_SLOTS ? g_nslot : MAX_SLOTS;
   for (int i = 0; i < g_snap_nslot; i++) {
      const struct sensor_slot *sl = &g_slot[i];
      struct snap_slot *d          = &g_snap_slot[i];
      d->id                        = sl->id;
      d->marker                    = sl->marker;
      d->color                     = sl->color;
      d->primary                   = sl->primary;
      d->size                      = sl->size;
      d->wear_days                 = sl->wear_days;
      d->old                       = sl->old;
      str_snapshot(d->label, sizeof d->label, sl->label);
      const struct sensor_rec *r = sensor_rec_by_id(sl->id);
      d->have_rec                = (r != 0);
      d->type                    = r ? r->type : 0;
      d->paired                  = r ? r->paired : 0;
      d->activation              = r ? r->activation : 0;
      str_snapshot(d->mac, sizeof d->mac, r ? r->identity : "");
      str_snapshot(d->serial, sizeof d->serial, r ? r->serial : "");
      str_snapshot(d->model, sizeof d->model, r ? r->model : "");
      str_snapshot(d->fw, sizeof d->fw, r ? r->fw : "");
   }
   sensors_unlock();
}

static struct dex_session g_snap_sess[LINK_MAX];

/* Draw-path variant: resolves from the per-frame snapshot instead of the live
 * driver, because the renderer must never take driver_lock while holding the
 * draw flag -- that inversion is what caused an unrecoverable hang. */
static int snap_link_for_slot(int idx)
{
   if (idx < 0 || idx >= g_snap_nslot)
      return -1;
   const struct snap_slot *d = &g_snap_slot[idx];
   if (!d->have_rec)
      return -1;
   /* By ADDRESS for meters too: they hold their own links now, so there is
    * no single link to return for "the meter". */
   for (int l = 0; l < LINK_MAX; l++)
      if (g_snap_sess[l].mac[0] && strcmp(g_snap_sess[l].mac, d->mac) == 0)
         return l;
   return -1;
}

/* Driver state for the frame, captured before the draw flag is taken.
 *
 * build_model() used to call driver_lock() while draw() held g_draw_busy, and
 * the BLE side takes them the other way round (driver_lock -> hist_lock inside
 * driver_on_notify -> drv_glucose). Two spin locks acquired in opposite orders
 * is an unrecoverable hang, and it needed only a reading landing during a 1 Hz
 * repaint -- i.e. steady-state operation. Snapshotting here means the main
 * thread never holds one lock while waiting for the other. */
static struct dex_cal g_snap_cal;

static void snap_drivers(void)
{
   driver_lock();
   for (int l = 0; l < LINK_MAX; l++) {
      driver_select(l);
      driver_get_session(&g_snap_sess[l]);
   }
   g_snap_cal = (struct dex_cal){0};
   if (g_sel >= 0 && g_sel < g_nslot && link_for_slot(g_sel) >= 0) {
      driver_select(link_for_slot(g_sel));
      driver_get_cal(&g_snap_cal);
   }
   driver_select(LINK_CGM);
   driver_unlock();
}

/* Reconcile the driver's live session against the permanent registry: mint an
 * id for a sensor we have not recorded yet, claim a slot for it, and remember
 * that id as the source stamped onto its readings. Runs on the main thread from
 * the 1 Hz timer, so it never races a BLE-thread reading.
 *
 * Minting is keyed on identity + firmware (NOT activation -- see sensor_mint),
 * so a new physical sensor gets its own id while a live one keeps hers. */
/* The sensor id for the link a reading actually arrived on.
 *
 * A reading MUST be stamped with the sensor that produced it, not with a
 * global "current" id. jni_notify holds driver_lock across
 * driver_select(link) + dispatch, so while drv_glucose runs, driver_link() IS
 * the originating link -- that is the only trustworthy attribution available
 * at this depth. Stamping a single global instead meant two CGMs shared one
 * id, and per-source dedup (150 s window) then silently DISCARDED whichever
 * sensor's sample landed second: roughly half of one sensor's data, never
 * written to the log and never plotted.
 *
 * Returns -1 when the link maps to no registered slot, so the caller can
 * refuse to log rather than invent a provenance. */
/* How many CGMs are registered. Above one, a reading whose link resolves to no
 * slot cannot be safely attributed to the global "current source". */
static int cgm_slot_count(void)
{
   /* LIVE CGMs only -- a DISCONNECTED (old) slot is kept for its history and
    * the OLD DEVICES menu, but must not count as a streaming sensor. */
   return sensor_live_cgm_count();
}

static int src_for_link(int link)
{
   /* Match on the session ADDRESS, and walk g_slot under the registry lock.
    *
    * The address is the only identity that cannot be shifted out from under a
    * live connection: sensor_forget_slot() renumbers g_slot while the remaining
    * sensors keep streaming, so anything keyed on a slot's POSITION silently
    * re-points at a different sensor. With two CGMs that stamped one sensor's
    * id onto the other's readings -- in an append-only log that is never
    * rewritten, so the mistake is permanent. The lock matters for the same
    * reason: the main thread can be mid-shift while this runs on a BLE
    * thread. */
   /* DRIVER_LOCK IS REQUIRED, not optional.
    *
    * driver_select writes two file-statics -- g_cur_link and the ambient ctx
    * pointer every driver function dereferences -- so dexdriver.h states that
    * selection and the work following it must be one atomic step. Two of the
    * three callers reach here from inside jni_notify, which already holds the
    * (recursive) lock, which is why the omission was invisible. The third,
    * pancra_link_watchdog, runs on the main looper AND the service tick thread
    * and calls this AFTER releasing it -- so it can stomp g_cur_link and ctx
    * out from under a binder thread mid-dispatch. That thread then attributes
    * its reading to the OTHER sensor's id in the append-only log, and
    * drv_key_save/drv_key_clear pick their key file from driver_link(), so a
    * stomp during auth writes one sensor's key over another's. */
   driver_lock();
   struct dex_session s;
   int prev = driver_link();
   driver_select(link);
   driver_get_session(&s);
   driver_select(prev);
   driver_unlock();
   if (!s.mac[0])
      return -1;
   int id = -1;
   sensors_lock();
   for (int i = 0; i < g_nslot && id < 0; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && sensor_kind(r->type) == KIND_CGM && !strcmp(r->identity, s.mac))
         id = g_slot[i].id;
   }
   sensors_unlock();
   return id;
}

/* Runs from the activity's 1 Hz timer AND, once that timer is gone, from the
 * service tick. Serialised by a try-lock because those are different threads:
 * it mints ids and appends to the provenance file, and two concurrent passes
 * could mint twice for one sensor. Skipping a tick is free. */
static volatile int g_reconcile_busy;

static void sensor_reconcile(void)
{
   if (__atomic_exchange_n(&g_reconcile_busy, 1, __ATOMIC_SEQ_CST))
      return;
   meter_sync_watchdog();
   /* Flush the session cache at most once a minute. sessc_put marks it dirty
    * from the draw path, which runs far more often than the 5-minute cadence
    * that actually changes anything; rate-limiting here keeps a redraw storm
    * from becoming a write storm on a file whose only job is to survive the
    * next launch. Losing up to a minute of it costs nothing -- the clock is
    * projected forward from whatever instant was stored. */
   {
      long nowt = realtime_s();
      if (g_sess_dirty && nowt - g_sess_saved >= 60) {
         sess_save();
         g_sess_saved = nowt;
         g_sess_dirty = 0;
      }
   }

   /* Walk every CGM link so a newly bonded second sensor is registered too,
    * not just whichever one happened to connect first. */
   int primary_src = -1;
   driver_lock();
   /* ot_drv_done mutates g_slot from a binder thread (sensor_rebind_slot), so
    * this walk needs the registry lock too -- it is the walk that decides
    * g_cur_src, the fallback provenance stamped into the permanent log. */
   sensors_lock();
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].old) /* disconnected: no live session to reconcile */
         continue;
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (!r || sensor_kind(r->type) != KIND_CGM)
         continue;
      int l = link_for_slot(i);
      if (l < 0)
         continue;
      driver_select(l);
      struct dex_session ls;
      driver_get_session(&ls);
      /* Prefer the PRIMARY sensor, and stop at it. Without this the loop ran to
       * the end and left g_cur_src on whichever bonded CGM sat highest in the
       * slot table, so a second sensor's readings were stamped with the
       * first's id -- and per-source dedup then silently discarded samples
       * that collided within 150 s. */
      if (ls.bonded && ls.mac[0] && !strcmp(ls.mac, r->identity)) {
         if (primary_src < 0 || g_slot[i].primary)
            primary_src = g_slot[i].id;
         if (g_slot[i].primary)
            break;
      }
   }
   sensors_unlock();
   driver_unlock();
   /* USE the result. This loop previously assigned a local that was never
    * read, so the whole "prefer the primary" fix above was a dead store and
    * g_cur_src kept whatever the registration block below left it -- which,
    * once any CGM had a slot, was never anything at all (see there). */
   if (primary_src > 0)
      g_cur_src = primary_src;

   /* Only a CGM is registered from a dex_session. Without this guard, adding a
    * meter would leave g_add_type on ONETOUCH and the next CGM to bond would be
    * minted with the wrong type -- and a wrong type is permanent, because the
    * provenance row is never rewritten. */
   int cgm_type =
       (sensor_kind(g_add_type) == KIND_CGM) ? g_add_type : SENSOR_STELO;
   /* The link a new pairing would use. Note this must not be left selected on
    * return: the caller's stall watchdog and build_model() both read the
    * driver afterwards, and an unused link reports an empty session. */
   /* Recover the meter's id FIRST and unconditionally. It used to sit after
    * the CGM early-return below, so a meter-only user never recovered it after
    * a restart and their meter could never auto-sync again. */
   sensors_lock();
   /* Only seed this when it is not already pointing at a registered meter.
    *
    * The advert handler now selects the meter per advert (any registered one,
    * not just the first), so re-latching the first slot on every 1 Hz tick
    * would clobber that selection -- including mid-sync, which would attribute
    * one meter's fingersticks to another. This is a fallback for the case
    * where nothing has selected a meter yet, e.g. right after a restart. */
   int have_meter = 0;
   if (g_meter_src > 0) {
      const struct sensor_rec *cur = sensor_rec_by_id(g_meter_src);
      have_meter                   = (cur && cur->type == SENSOR_ONETOUCH);
   }
   for (int i = 0; i < g_nslot && !have_meter; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && r->type == SENSOR_ONETOUCH) {
         g_meter_src = g_slot[i].id;
         /* Restore the ADDRESS too. Without it g_meter_mac was empty after a
          * restart and the "is this our meter" guard accepted ANY OneTouch in
          * range -- importing a stranger's readings under our sensor id. */
         str_snapshot(g_meter_mac, sizeof g_meter_mac, r->identity);
         break;
      }
   }
   sensors_unlock();

   /* Find a CGM link carrying a live bonded session that NO slot claims yet --
    * that is the sensor which still needs registering.
    *
    * This used to probe link_for_slot(g_nslot), i.e. "the link a new pairing
    * would use". Once link resolution became address-based that became a
    * guaranteed dead end: link_for_slot for an unregistered index returns a
    * FREE link, and a free link is by definition one with no session, so
    * s.mac[0] was always 0 and this entire block stopped executing. Nothing
    * was ever minted or slotted, so every reading fell back to source id 0
    * ("pre-registry legacy") in a log that is never rewritten, and the
    * advert-driven reconnect loop -- which iterates slots -- had an empty body.
    * Scanning for the unclaimed session asks the question directly. */
   struct dex_session s;
   memset(&s, 0, sizeof s);
   int s_link = -1;
   driver_lock();
   sensors_lock();
   int prev_sel = driver_link();
   for (int l = 0; l < LINK_MAX && !s.mac[0]; l++) {
      if (g_link_meter[l])
         continue; /* CGMs only */
      driver_select(l);
      struct dex_session ls;
      driver_get_session(&ls);
      /* have_reading, not just bonded. g_bonded is set at AuthStatus, several
       * round trips BEFORE the first glucose, so session_seconds is still 0
       * then -- and activation is derived from it, so minting that early wrote
       * "session started now" for a sensor that may have been worn for days.
       * activation is not part of the id-reuse key, so it is never corrected.
       */
      if (ls.mac[0] && ls.bonded && ls.have_reading &&
          sensor_slot_by_mac(ls.mac) < 0) {
         s      = ls;
         s_link = l;
      }
   }
   driver_select(prev_sel);
   sensors_unlock();
   driver_unlock();
   if (s.mac[0] && s.bonded && s_link >= 0) {
      /* Match on ADDRESS FIRST, whatever type is currently selected in the
       * ADD SENSOR menu. g_add_type persists after the menu closes, so keying
       * only on (type, mac) let merely *browsing* the type picker re-mint an
       * already-registered sensor under the wrong type -- unrecoverable, since
       * provenance rows are never rewritten. */
      int idx = sensor_slot_by_mac(s.mac);
      if (idx < 0) {
         /* activation is an EPOCH: the session clock counts up from it, so
          * the start instant is now minus the elapsed time. Feeding the
          * elapsed value straight in wrote a duration into a field documented
          * as a timestamp, in a file that is never rewritten. */
         long activation = realtime_s() - (long)s.session_seconds;
         /* This LINK's own DIS strings, never the process-global ones -- those
          * are shared across links and persist to disk, so a second sensor
          * would inherit the first's model and firmware permanently.
          *
          * Copied under the registry lock, for the reason spelled out in the
          * second pass below: pancra_devinfo fills these byte-by-byte from a
          * binder thread, and a torn read here is minted into an append-only
          * row that is never rewritten. */
         char amodel[24] = {0};
         char afw[24]    = {0};
         sensors_lock();
         str_snapshot(amodel, sizeof amodel, g_model_l[s_link]);
         str_snapshot(afw, sizeof afw, g_fw_l[s_link]);
         sensors_unlock();
         int id = sensor_mint(cgm_type, s.mac, "", amodel, afw, activation);
         idx    = (id < 0) ? -1 : sensor_claim_slot(id, cgm_type, s.mac);
         if (idx >= 0)
            LOGI("registered sensor id=%d type=%s mac=%s fw=%s", id,
                 sensor_type_name(cgm_type), s.mac, afw);
         else if (id >= 0)
            LOGI("sensor slots full (%d); %s not listed", MAX_SLOTS, s.mac);
      }
      if (idx >= 0)
         g_cur_src = g_slot[idx].id;
   }

   /* SECOND PASS: complete provenance for an ALREADY-registered CGM whose
    * learned attributes have since arrived.
    *
    * A CGM is registered BARE the moment the user commits to pairing it (see
    * commit_pair), so its permanent row starts with no model, no firmware and
    * an unknown activation. The DIS strings land a few seconds after the
    * first connection and the activation instant is only knowable once a
    * reading has anchored the session clock -- this pass writes each the
    * moment it becomes true, via sensor_complete, which fills ONLY missing
    * fields and appends the corrected row (the file is never rewritten).
    *
    * This used to mint a second id and rebind the slot to it. Since identity
    * became MAC-only that mint always returned the SAME id and the rebind was
    * a no-op -- the pass was dead code and rows stayed bare forever. */
   /* Collect under driver_lock, ACT outside it. sensor_complete does
    * synchronous file I/O (sensors.csv), and driver_lock is a no-timeout spin
    * lock every GATT binder callback waits on -- holding it across a file
    * write burns a core out of the small binder pool. The first pass above
    * already releases it before minting for exactly this reason. */
   struct {
      char model[24];
      char fw[24];
      long act;
      int id;
   } todo[LINK_MAX];

   int ntodo = 0;
   driver_lock();
   int psel = driver_link();
   for (int l = 0; l < LINK_MAX; l++) {
      if (g_link_meter[l])
         continue; /* CGMs only */
      driver_select(l);
      struct dex_session ls;
      driver_get_session(&ls);
      if (!ls.mac[0] || !ls.bonded)
         continue;
      sensors_lock();
      /* COPY the DIS strings under the registry lock -- the same lock
       * pancra_devinfo writes them under, on a binder thread, while this runs
       * on the main thread. Testing and snapshotting them unlocked (as this
       * did) made that writer's lock inert: the emptiness test passes as soon
       * as the writer lands byte 0, so a firmware of "1.6.0.11" can be read as
       * "1", minted as a DIFFERENT id, and the slot rebound to it. The next
       * tick sees a non-empty fw, so stale_row is false and the truncated
       * value is never corrected -- in an append-only file. */
      char lmodel[24] = {0};
      char lfw[24]    = {0};
      str_snapshot(lmodel, sizeof lmodel, g_model_l[l]);
      str_snapshot(lfw, sizeof lfw, g_fw_l[l]);
      int si                      = sensor_slot_by_mac(ls.mac);
      int cur_id                  = (si >= 0) ? g_slot[si].id : 0;
      const struct sensor_rec *cr = cur_id ? sensor_rec_by_id(cur_id) : 0;
      int is_cgm                  = cr && sensor_kind(cr->type) == KIND_CGM;
      /* DIS strings only when BOTH have landed -- the same rule the meter
       * path already enforces. They are separate serialized GATT ops and the
       * sensor commonly closes the cycle before all of them land; writing
       * "model present, fw still empty" would append a correction row per
       * tick until fw arrived. sensor_complete cannot fork an id any more,
       * but the file should not carry churn either. */
      int need_mfw =
          is_cgm && (!cr->model[0] || !cr->fw[0]) && lmodel[0] && lfw[0];
      /* Activation only once a reading has anchored the session clock:
       * g_bonded is set at AuthStatus, several round trips before the first
       * glucose, when session_seconds still reads 0 -- deriving activation
       * then would write "session started now" for a sensor that may have
       * been worn for days, into a field that is completed exactly once. */
      int need_act = is_cgm && !cr->activation && ls.have_reading;
      sensors_unlock();
      if (!need_mfw && !need_act)
         continue;
      todo[ntodo].id = cur_id;
      str_snapshot(todo[ntodo].model, sizeof todo[ntodo].model,
                   need_mfw ? lmodel : "");
      str_snapshot(todo[ntodo].fw, sizeof todo[ntodo].fw, need_mfw ? lfw : "");
      /* The epoch the session STARTED, not its elapsed length (see the first
       * pass): now minus elapsed. */
      todo[ntodo].act = need_act ? realtime_s() - (long)ls.session_seconds : 0;
      ntodo++;
   }
   driver_select(psel);
   driver_unlock();
   for (int i = 0; i < ntodo; i++) {
      if (sensor_complete(todo[i].id, "", todo[i].model, todo[i].fw,
                          todo[i].act) == 1)
         LOGI("sensor provenance completed: id %d (%s / %s, act %ld)",
              todo[i].id, todo[i].model, todo[i].fw, todo[i].act);
   }

   /* Leave the driver on a link that actually exists: the caller's stall
    * watchdog and build_model() both read the session straight after this, and
    * an unused link reports an empty one. */
   driver_lock();
   driver_select(LINK_CGM);
   driver_unlock();
   __atomic_store_n(&g_reconcile_busy, 0, __ATOMIC_SEQ_CST);
}

/* The service tick's route to the registry.
 *
 * sensor_reconcile ran only from on_timer -- the ACTIVITY's looper, which
 * on_destroy tears down. Its work includes minting a newly bonded CGM and
 * completing a sensor's provenance once its DIS strings arrive, both of which
 * write the append-only file. A sensor that bonds while the activity is gone
 * was therefore never registered, and its readings were stamped with the
 * fallback source id in a log that is never rewritten. meter_sync_watchdog was
 * lifted out for exactly this reason; the rest of the function was left
 * behind. */
void pancra_reconcile_tick(void)
{
   sensor_reconcile();
}

/* Fill one ui_sensor from its slot + provenance + (if it is the live one) the
 * driver session. */
static void fill_sensor(struct ui_sensor *u, int i, long now)
{
   /* From the pre-draw snapshot, never the live registry -- see snap_registry.
    */
   if (i < 0 || i >= g_snap_nslot)
      return;
   const struct snap_slot *sl = &g_snap_slot[i];
   *u                         = (struct ui_sensor){0};
   u->id                      = sl->id;
   u->marker                  = sl->marker;
   u->color                   = sl->color;
   u->primary                 = sl->primary;
   u->old                     = sl->old;
   u->size =
       (sl->size >= 1 && sl->size <= MARK_SIZE_MAX) ? sl->size : MARK_SIZE_DEF;
   str_snapshot(u->label, sizeof u->label, sl->label);
   if (sl->have_rec) {
      u->type = sl->type;
      u->kind = sensor_kind(sl->type);
      str_snapshot(u->mac, sizeof u->mac, sl->mac);
      str_snapshot(u->serial, sizeof u->serial, sl->serial);
      str_snapshot(u->model, sizeof u->model, sl->model);
      str_snapshot(u->fw, sizeof u->fw, sl->fw);
   }
   /* This DEVICE's wear budget: override / model / type default. */
   u->wear_len = sensor_wear_seconds(u->type, sl->wear_days, sl->model);
   /* Mirrors sensor_wear_seconds' own test for "the user pinned this", and
    * must stay in step with it: anything that is not a valid pin resolves. */
   u->wear_auto  = (sl->wear_days != 10 && sl->wear_days != 15);
   u->paired     = sl->paired;
   u->activation = sl->activation;
   /* The OS bond, keyed by the address the framework knows this device by --
    * for a G7 that is the BOND IDENTITY address, which is what the snapshot's
    * mac holds (a rotating RPA would never match a bond record). */
   u->bond = dexble_bond_state(sl->mac);
   /* newest reading from this source, for the "last seen" column */
   for (int k = 0; k < g_nhist; k++)
      if (g_hist[k].src == (unsigned short)sl->id) {
         u->last  = g_hist[k].t;
         u->glu   = g_hist[k].glu;
         u->trend = g_hist[k].trend;
         break;
      }
   if (u->kind == KIND_CGM) {
      /* Every CGM has its own link and its own driver context, so each row
       * reports that sensor's real session. Read from the pre-draw snapshot --
       * taking driver_lock() here would nest it inside the draw flag. */
      int sl_link = snap_link_for_slot(i);
      struct dex_session s =
          (sl_link >= 0) ? g_snap_sess[sl_link] : (struct dex_session){0};
      /* LIVE WINS; the cache only fills the gap before this link's first 0x4e
       * of the process. Both directions in one place, so the per-device screen
       * and the top block cannot disagree about a sensor's session. */
      if (s.have_reading)
         sessc_put(sl->id, &s, now);
      else
         (void)sessc_restore(sl->id, now, &s);
      u->connected       = s.bonded && (now - u->last) < AL_FRESH_S;
      u->session_seconds = (long)s.session_seconds;
      u->sess_state      = s.state;
      u->predicted       = s.predicted;
      u->sequence        = s.sequence;
      /* THIS link's signal, not the last one measured on any link. */
      {
         int lk     = snap_link_for_slot(i);
         int haveit = lk >= 0 && lk < LINK_MAX && g_link_rssi_ok[lk];
         u->rssi    = haveit ? g_link_rssi[lk] : 0;
         u->rssi_ok = haveit;
         u->rssi_t  = haveit ? g_link_rssi_t[lk] : 0;
      }
      /* The pairing code is the GLOBAL last-entered one (g_code_str), not a
       * per-device secret we persist -- so only show it on a LIVE CGM (where
       * it is at least the code in current use). An OLD device would otherwise
       * display a live sensor's code, which is why both G7s appeared to share
       * one. */
      if (!sl->old)
         str_snapshot(u->code, sizeof u->code, g_code_str);
      /* WARMUP: a freshly paired sensor delivers nothing for about its first
       * hour BY DESIGN -- without saying so, that silence reads as broken
       * and the user hovers over a working sensor. Keyed off the pairing
       * instant, since the session clock is unknown until the first EGV. */
      /* The SENSOR'S OWN state byte is authoritative (warmup readings are
       * recorded and displayed, so "has no data" no longer implies warmup);
       * with no 4e seen yet, fall back to the session clock, then to the
       * pairing instant. */
      /* DISPLAY POLICY for a device's lifecycle, applied everywhere the
       * same: WARMUP while the sensor says so (readings recorded anyway),
       * CONNECTED/WAITING while live, ENDED once the sensor's own state
       * byte says the session is over. History always keeps the device's
       * colours; only FORGETTING a device orphans its points to grey. */
      /* AN UNANSWERED PAIRING DIALOG OUTRANKS EVERYTHING BELOW.
       *
       * It rides on the STATUS string rather than a row of its own: the
       * per-device screen is already at its row ceiling, and one more row
       * there drops ui_fit_scale over its cliff and shrinks the whole screen's
       * text (see the note at render_sensor). The state is genuinely a status
       * -- the device is registered and doing nothing, and this says why --
       * so the row that reports status is where it belongs. */
      if (u->bond == UI_BOND_BONDING)
         str_snapshot(u->status, sizeof u->status, "CONFIRM PAIRING");
      else if (u->sess_state == SENSOR_STATE_ENDED)
         str_snapshot(u->status, sizeof u->status, "ENDED");
      else if (u->sess_state == SENSOR_STATE_WARMUP ||
               (u->sess_state == 0 && u->last == 0 &&
                ((u->session_seconds > 0 &&
                  u->session_seconds < SENSOR_WARMUP_S) ||
                 (u->session_seconds == 0 && sl->paired > 0 &&
                  now - sl->paired < SENSOR_WARMUP_S))))
         str_snapshot(u->status, sizeof u->status, "WARMUP");
      else
         str_snapshot(u->status, sizeof u->status,
                      u->connected ? "CONNECTED" : "WAITING");
      /* Calibration state for the LAST CAL row: a queue entry for THIS sensor
       * takes precedence (still pending), else its last resolved outcome. */
      if (g_calq_mgdl > 0 && g_calq_id == u->id) {
         u->cal_pending = g_calq_mgdl;
      } else if (g_lastcal_t > 0 && g_lastcal_id == u->id) {
         u->cal_mgdl  = g_lastcal_mgdl;
         u->cal_state = g_lastcal_state;
         u->cal_t     = g_lastcal_t;
      }
      u->rescale_pm =
          (g_rescale_pm != 1000 && g_rescale_id == u->id) ? g_rescale_pm : 1000;
      u->rescale_pending =
          (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == u->id)
              ? g_rescale_pend_mgdl
              : 0;
      u->rescale_rejected = (g_rescale_reject && g_rescale_reject_id == u->id);
      u->rescale_expired = (g_rescale_expired && g_rescale_expired_id == u->id);
   } else {
      /* PER-METER, so syncing one meter never rewrites another's row. Only the
       * meter that currently OWNS the sync (g_meter_src) shows SYNCING and the
       * live RSSI; each meter's "last" and SYNCED/OFF come from ITS OWN reading
       * history (u->last, set above from g_hist), not the shared session
       * globals. u->last is persisted, so STATE is correct after a restart too.
       */
      struct meter_rt *rt = meter_rt_get(u->id, 0);
      int syncing         = (g_meter_busy && u->id == g_meter_src);
      u->connected        = syncing;
      /* LAST SYNC (when the app last connected THIS meter) is separate from
       * u->last, which is its last DATAPOINT (fingerstick). */
      u->meter_sync_t = rt ? rt->sync_t : 0;
      if (syncing)
         /* Live handshake step (HELLO/COUNT/READING/...) if the driver has
          * reported one this sync, else a plain SYNCING while connecting. */
         str_snapshot(u->status, sizeof u->status,
                      (rt && rt->stat[0]) ? rt->stat : "SYNCING");
      else if (rt && rt->stat[0])
         /* Terminal result of the last sync (SYNCED / NOTHING NEW / NOT PAIRED
          * / REFUSED / BAD DATA) -- more informative than a bare SYNCED. */
         str_snapshot(u->status, sizeof u->status, rt->stat);
      else if (u->meter_sync_t > 0 || u->last > 0)
         str_snapshot(u->status, sizeof u->status, "SYNCED");
      else
         str_snapshot(u->status, sizeof u->status, "OFF");
      /* This meter's OWN last RSSI (kept across the meter powering off between
       * syncs), not tied to a datapoint. */
      if (rt && rt->rssi_ok) {
         u->rssi    = rt->rssi;
         u->rssi_ok = 1;
         u->rssi_t  = rt->rssi_t;
      } else {
         u->rssi_ok = 0;
      }
   }
}

/* Snapshot the shell's mutable state into an immutable frame for the pure UI.
 * Called on the main thread just before ui_render(); the borrowed hist/dev
 * arrays are static so they outlive the render call. */
/* ---- long plot spans: bucketed from the LOG, not from the RAM window ----
 *
 * A plot is about 700 pixels wide, so a 30-day span can show at most one
 * column per hour however many readings exist. Drawing it from g_hist tied
 * the plot's DEPTH to a point budget: 5040 points is ten days once four
 * sources are logging, so the 30D plot ran out of data before it ran out of
 * axis -- and it would shrink again, silently, the day a user added a third
 * sensor.
 *
 * So for anything past the live window the points come straight out of
 * readings.csv, bucketed onto the pixel grid as they are read: for each
 * column, the newest reading of each source. Memory is then a function of
 * the SCREEN (columns x sources), not of how much history exists or how many
 * devices write it -- 30D, 90D or a year all cost the same.
 *
 * Rebuilt only when the span changes, the log grows, or the window has aged
 * a minute (on a 30-day plot one pixel is an hour, so a minute of staleness
 * is invisible). */
#define PCOL_MAX  768          /* x columns we ever draw into */
#define PSRC_MAX  8            /* distinct sources kept per column */
#define PLONG_MIN (24L * 3600) /* spans past this are drawn from the log */

static void build_model(struct screen *m)
{
   /* glucose + doses + weights. All three ride in one array because plot_render
    * and plot_hit take points in any order, and the scrub index the UI hands
    * back has to index a single list. */
#define UI_PTS_MAX (PLOT_LONG_MAX + NINS + NWT)
   static struct ui_point pts[UI_PTS_MAX];
   static struct ui_dev devs[MAX_DEVS];
   long now = realtime_s();

   *m = (struct screen){0};
   if (g_gate)
      m->scr = SCR_GATE;
   else if (g_menu == MENU_SETTINGS)
      m->scr = SCR_SETTINGS;
   else if (g_menu == MENU_KEYPAD)
      m->scr = SCR_KEYPAD;
   else if (g_menu == MENU_DEVLIST)
      m->scr = SCR_DEVLIST;
   else if (g_menu == MENU_SENSOR)
      m->scr = SCR_SENSOR;
   else if (g_menu == MENU_CAL)
      m->scr = SCR_CAL;
   else if (g_menu == MENU_CALPEND)
      m->scr = SCR_CALPEND;
   else if (g_menu == MENU_RESCALE)
      m->scr = SCR_RESCALE;
   else if (g_menu == MENU_RESCALEACT)
      m->scr = SCR_RESCALEACT;
   else if (g_menu == MENU_SENSTYPE)
      m->scr = SCR_SENSTYPE;
   else if (g_menu == MENU_FORGET)
      m->scr = SCR_FORGET;
   else if (g_menu == MENU_LABEL)
      m->scr = SCR_LABEL;
   else if (g_menu == MENU_MARKPICK)
      m->scr = SCR_MARKPICK;
   else if (g_menu == MENU_COLORPICK)
      m->scr = SCR_COLORPICK;
   else if (g_menu == MENU_METERHELP)
      m->scr = SCR_METERHELP;
   else if (g_menu == MENU_PAIRCONF)
      m->scr = SCR_PAIRCONF;
   else if (g_menu == MENU_ADD)
      m->scr = SCR_ADDMENU;
   else if (g_menu == MENU_INSULIN)
      m->scr = SCR_INSULIN;
   else if (g_menu == MENU_WEIGHT)
      m->scr = SCR_WEIGHT;
   else if (g_menu == MENU_WTLOG)
      m->scr = SCR_WTLOG;
   else if (g_menu == MENU_WTDEL)
      m->scr = SCR_WTDEL;
   else if (g_menu == MENU_INSLOG)
      m->scr = SCR_INSLOG;
   else if (g_menu == MENU_DISPLAY)
      m->scr = SCR_DISPLAY;
   else if (g_menu == MENU_DEVICES)
      m->scr = SCR_DEVICES;
   else if (g_menu == MENU_PERMS)
      m->scr = SCR_PERMS;
   else if (g_menu == MENU_OLDDEV)
      m->scr = SCR_OLDDEV;
   else if (g_menu == MENU_RECONF)
      m->scr = SCR_RECONF;
   else if (g_menu == MENU_REMOTE)
      m->scr = SCR_REMOTE;
   else if (g_menu == MENU_INSDEL)
      m->scr = SCR_INSDEL;
   else if (g_menu == MENU_ALARM)
      m->scr = SCR_ALARM;
   else if (g_menu == MENU_EXPORT)
      m->scr = SCR_EXPORT;
   else
      m->scr = SCR_MAIN;
   m->now    = now;
   m->tz_off = g_tz_off;

   m->glu          = g_cur_glu;
   m->trend        = g_cur_trend;
   m->t            = g_cur_time;
   m->rssi         = g_cur_rssi;
   m->rssi_ok      = g_cur_rssi_ok;
   m->stale        = (g_cur_glu >= 0) && (now - g_cur_time > AL_FRESH_S);
   m->disc_alarmed = g_disc_alarmed;

   /* g_hist is read WITHOUT an explicit hist_lock() here, and that is correct,
    * not an oversight: hist_lock IS g_draw_busy, and draw() holds that flag
    * across draw_impl -> build_model. Any BLE thread entering hist_insert
    * therefore spins until this frame is done. Adding hist_lock() here would
    * SELF-DEADLOCK -- unlike driver_lock and reg_lock, this one is not
    * recursive. An adversarial review flagged the missing lock; acting on it
    * would have wedged the app on the first repaint. */
   int nlong = 0;
   const struct ui_point *plong =
       plot_source_from(g_store_path, now, g_plot_hours, &nlong);
   int nh = 0;
   if (plong) {
      /* A long span: one column per pixel, straight from the log, so its
       * depth does not depend on how many points happen to fit in RAM. */
      for (int i = 0; i < nlong && nh < PLOT_LONG_MAX; i++)
         pts[nh++] = plong[i];
   } else {
      nh = g_nhist < NHIST ? g_nhist : NHIST;
      for (int i = 0; i < nh; i++) {
         pts[i].t    = g_hist[i].t;
         pts[i].glu  = g_hist[i].glu;
         pts[i].src  = g_hist[i].src;
         pts[i].kind = g_hist[i].kind;
      }
   }
   /* Insulin doses ride along as KIND_INS points (glu = units; ui.c pins
    * them to the plot's bottom edge and scrubs them as "N UNITS").
    * plot_render and plot_hit take points in any order, so appending
    * after the newest-first glucose is fine. They are NEVER in g_hist,
    * so they cannot leak into stats or the remote push. */
   for (int i = 0; i < g_nins && nh < UI_PTS_MAX; i++) {
      pts[nh].t    = g_ins[i].t;
      pts[nh].glu  = g_ins[i].units;
      pts[nh].src  = g_ins[i].type; /* the scrub shows "2U FAST" etc. */
      pts[nh].kind = KIND_INS;
      nh++;
   }
   /* Logged WEIGHTS ride along the same way, on the same bottom line, as a
    * small W. Carried in GRAMS -- the log's canonical unit -- so the scrub
    * renders them in whichever display unit is set at the time, exactly as
    * the weight table does. Like the doses these are never in g_hist, so they
    * cannot leak into TIR, the average or the remote push. */
   for (int i = 0; i < g_nwt && nh < UI_PTS_MAX; i++) {
      pts[nh].t   = g_wt[i].t;
      pts[nh].glu = (int)g_wt[i].g; /* grams; WT_MAX_G is 400000, fits an int */
      pts[nh].src = 0;
      pts[nh].kind = KIND_WT;
      nh++;
   }
   m->hist       = pts;
   m->nhist      = nh;
   m->scrub      = g_scrub_idx;
   m->plot_hours = g_plot_hours;
   m->plot_max   = g_plot_max;

   /* The PRIMARY CGM drives the top block -- resolved to ITS link, not
    * hardcoded LINK_CGM (link 0), which just belongs to whichever CGM claimed
    * it first. With the Stelo on link 0 and a G7 primary on another link,
    * the STATE/SESSION/PRED rows mixed the G7's 10-day budget with the
    * STELO's session clock: 15 days in vs 10 d + 12 h grace computed as past
    * even the grace period, so the SESSION row said ENDED while the primary
    * was nowhere near its end. A primary with no live session yet (freshly
    * committed pairing) reports an EMPTY session -- SESSION --, consistent
    * with the cleared big number, never another sensor's numbers. */
   struct dex_session s = g_snap_sess[LINK_CGM];
   for (int i = 0; i < g_snap_nslot; i++)
      if (g_snap_slot[i].primary) {
         int pl = snap_link_for_slot(i);
         s      = (pl >= 0) ? g_snap_sess[pl] : (struct dex_session){0};
         /* Same restore as the per-device rows: without it PRED and the
          * session countdown under the big number blanked for a whole
          * five-minute cadence after every launch, while the reading and its
          * age beside them came straight back from readings.csv. */
         if (!s.have_reading)
            (void)sessc_restore(g_snap_slot[i].id, now, &s);
         break;
      }
   m->bonded          = s.bonded;
   m->have_reading    = s.have_reading;
   m->predicted       = s.predicted;
   m->sequence        = s.sequence;
   m->sess_state      = s.state;
   m->session_seconds = (long)s.session_seconds;
   /* Whether ANY CGM is registered -- from the snapshot, so it is consistent
    * with the sensor rows below. The STATE/SESSION/PRED block is CGM-only;
    * with none, ui.c blanks it rather than echoing the meter's status. */
   /* A LIVE (non-old) CGM. Old/disconnected CGMs don't count -- with none
    * live the STATE/SESSION/PRED block and the big-number age blank out. */
   m->has_cgm = 0;
   for (int i = 0; i < g_snap_nslot; i++)
      if (g_snap_slot[i].have_rec && !g_snap_slot[i].old &&
          sensor_kind(g_snap_slot[i].type) == KIND_CGM) {
         m->has_cgm = 1;
         break;
      }

   m->units       = g_units;
   m->alarm_low   = g_alarm_low;
   m->alarm_high  = g_alarm_high;
   m->nudge_low   = g_nudge_low;
   m->nudge_high  = g_nudge_high;
   m->nudge_sound = g_nudge_sound;
   m->nudge_vib   = g_nudge_vib;

   /* settings + device info (globals persist; s.mac lives on our stack, so it
    * is copied into a static the borrowed pointer can safely outlast) */
   m->sound_on       = g_sound_on;
   m->vib_on         = g_vib_on;
   m->orient         = g_orient;
   m->screen_on      = g_screen_on;
   m->newdata_mode   = g_newdata_mode;
   m->remote_on      = g_remote_on;
   m->remote_server  = g_remote_server; /* global: the borrow is stable */
   m->sync_email     = g_sync_email;
   m->sync_paired    = g_sync_uid > 0;
   m->label_field    = g_label_field;
   /* SMOOTHED HERE, not in the renderer. The sync reports whole buckets, so
    * the raw fraction steps; easing it toward the target gives a bar that
    * moves continuously without ever claiming more progress than was made
    * (it only ever approaches the true value, never passes it). Keeping the
    * easing on this side leaves render_remote a pure function of the struct,
    * which is what uitest depends on. */
   {
      int pdone = 0, ptotal = 0;
      int act   = sync_progress(&pdone, &ptotal);
      int target = (ptotal > 0) ? (int)(((long)pdone * 1000) / ptotal) : 0;
      if (!act) {
         g_prog_shown = 0;
      } else if (g_prog_shown < target) {
         int step = (target - g_prog_shown) / 4;
         g_prog_shown += step > 0 ? step : 1;
         if (g_prog_shown > target)
            g_prog_shown = target;
      }
      m->sync_active   = act;
      m->sync_permille = g_prog_shown;
      /* Keep frames coming while it runs, or the bar would freeze between
       * whatever else happens to repaint the screen. */
      if (act)
         g_ui_dirty = 1;
   }
   m->remote_status  = g_remote_status;
   m->remote_port    = g_remote_port;
   m->remote_last_ok = g_remote_last_ok;
   m->disc           = g_disc;
   m->code           = g_code_str;
   m->model          = g_model;
   m->fw             = g_fw;
   m->mfr            = g_mfr;
   static char macbuf[20];
   str_snapshot(macbuf, sizeof macbuf, s.mac);
   m->mac = macbuf;
   for (int i = 0; i < NPERMS; i++)
      m->perm[i] = g_sys_perm[i];
   m->batt_ok        = g_sys_batt;
   m->standby_bucket = g_sys_bucket;
   m->bg_restricted  = g_sys_bg;

   /* keypad: mode + the digits typed so far (copied so the pointer is stable)
    */
   /* configured sensors, plus which one a detail screen is showing */
   static struct ui_sensor sens[MAX_SLOTS];
   /* Count from the SNAPSHOT, not the live g_nslot. Mixing the two means a
    * concurrent sensor_forget_slot can shrink g_nslot between this loop bound
    * and the snapshot it indexes, so the last row renders whatever the
    * previous frame left in `sens` -- a sensor the user just forgot,
    * reappearing for a frame. */
   for (int i = 0; i < g_snap_nslot; i++)
      fill_sensor(&sens[i], i, now);
   m->sensors  = sens;
   m->nsensors = g_snap_nslot;
   m->sel      = g_sel;

   struct dex_cal c = g_snap_cal;
   m->cal_have      = c.have;
   m->cal_permitted = c.permitted;
   m->cal_status    = c.status;
   m->cal_last_bg   = c.last_bg;
   m->cal_result    = c.result;
   m->cal_pending   = g_cal_pending;

   /* RESCALE screens. On the confirmation, preview the CLAMPED factor computed
    * from the entered value over the selected sensor's live raw; on the active
    * screen, show the running factor. */
   m->rescale_entry = g_rescale_entry;
   if (g_menu == MENU_RESCALEACT) {
      m->rescale_pm = g_rescale_pm;
   } else if (g_menu == MENU_RESCALE && g_rescale_entry > 0 && g_sel >= 0 &&
              g_sel < g_nslot) {
      /* Preview: UNCLAMPED (so a >25% value shows its real size, in red, and is
       * rejected on CONFIRM), or the sentinel 0 when there is no reading yet to
       * compute against (the screen then says "ON NEXT READING", not "0%"). */
      int link = link_for_slot(g_sel);
      int raw  = (link >= 0 && link < LINK_MAX) ? g_link_raw[link] : 0;
      m->rescale_pm =
          (raw > 0) ? (int)((((long)g_rescale_entry * 1000) + (raw / 2)) / raw)
                    : 0;
   } else {
      m->rescale_pm = 1000;
   }

   m->kp_mode = g_kp_mode;
   /* Type being added, for the PAIR NEW <type> / SELECT <type> titles. The
    * OneTouch shows its full name; CGMs use their short type name. */
   m->add_type = (g_add_type == SENSOR_ONETOUCH) ? "ONETOUCH VERIO"
                                                 : sensor_type_name(g_add_type);
   m->add_kind = sensor_kind(g_add_type);
   /* the picked device SCR_PAIRCONF is asking about (main-thread globals,
    * like g_code_str above) */
   m->pair_name = g_pend_name;
   m->pair_mac  = g_pend_mac;
   /* LOG INSULIN form state */
   m->ins_t       = g_ins_t;
   m->ins_type    = g_ins_type;
   m->ins_units   = g_ins_units;
   m->ins_log     = g_ins; /* global tail: the borrow is stable */
   m->ins_nlog    = g_nins;
   m->inslog_page = g_inslog_page;
   m->wt          = g_wt;
   m->nwt         = g_nwt;
   m->wt_page     = g_wtlog_page;
   m->wt_t        = g_wt_t;
   m->wt_tenths   = g_wt_tenths;
   m->wunits      = g_wunits;
   m->wt_edit     = (g_wt_edit >= 0);
   m->wt_tab      = g_wt_tab;
   m->wt_scrub    = g_wt_scrub;
   m->wt_orig_t   = g_wt_orig.t;
   m->wt_orig_g   = g_wt_orig.g;
   m->ins_edit    = (g_ins_edit >= 0);
   for (int k = 0; k < 2; k++) {
      m->ins_marker[k] = g_ins_marker[k];
      m->ins_color[k]  = g_ins_color[k];
      m->ins_size[k]   = g_ins_size[k];
   }
   m->markpick_ins = g_markpick_ins;
   m->statbar_val  = g_statbar_val;
   m->lockscr_val  = g_lockscr_val;
   m->exp_range    = g_exp_range;
   m->exp_glu      = g_exp_glu;
   m->exp_dev      = g_exp_dev;
   m->exp_ins      = g_exp_ins;
   m->exp_wt       = g_exp_wt;
   m->pend_type    = g_pend_pairing;
   m->old_page     = g_old_page;
   m->dev_page     = g_dev_page;
   for (int i = 0; i < SC_MAX; i++)
      m->shortcut[i] = g_shortcut[i];

   /* Must hold the LONGEST entry any keypad accepts, not just a PIN. The
    * rename keypad caps at min(label-1, g_entry-1) = 11 characters, so an
    * 8-byte buffer echoed only the first 7: the field froze while typing
    * continued, DEL looked dead for four presses, and OK then saved a name the
    * user had never seen. Sized from g_entry so it cannot drift again. */
   static char entrybuf[sizeof g_entry];
   int el = g_entrylen < (int)sizeof entrybuf - 1 ? g_entrylen
                                                  : (int)sizeof entrybuf - 1;
   for (int i = 0; i < el; i++)
      entrybuf[i] = g_entry[i];
   entrybuf[el] = 0;
   m->entry     = entrybuf;

   static const int win[5] = {1, 3, 7, 30, 90};
   for (int i = 0; i < 5; i++) {
      int tir = 0;
      int avg = 0;
      if (stat_window(win[i], &tir, &avg)) {
         m->stat[i].have = 1;
         m->stat[i].tir  = tir;
         m->stat[i].avg  = avg;
      }
   }

   m->stored    = g_stored;
   m->status    = g_status;
   m->adv_total = g_total;

   devlist_lock(); /* consistent snapshot vs jni_on_advert / the pairing reset
                    */
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++) {
      str_snapshot(devs[i].name, sizeof devs[i].name, g_devs[i].name);
      str_snapshot(devs[i].mac, sizeof devs[i].mac, g_devs[i].mac);
      devs[i].rssi = g_devs[i].rssi;
   }
   devlist_unlock();
   m->devs = devs;
   m->ndev = nd;
}

static void draw_impl(struct ANativeWindow *win);

/* draw() is called from the main looper (on_timer at 1 Hz, on_input) AND from
 * BLE binder threads (status/reading updates flow through set_status/
 * pancra_glucose -> draw). struct ANativeWindow (a BufferQueue producer) is NOT
 * safe for concurrent access: two threads locking the surface at once corrupts
 * the returned buffer and segfaults. Serialise with a lock-free guard -- if a
 * draw is already running on another thread, drop this frame; the 1 Hz timer
 * (and the next event) repaint the latest state immediately after.
 *
 * The guard also closes a draw-vs-destroy use-after-free: a BLE thread can hold
 * an old window pointer while on_window_destroyed runs and the framework frees
 * the surface. We take g_draw_busy first, then re-check that the window we were
 * handed is STILL the live g_win; on_window_destroyed clears g_win and spins on
 * g_draw_busy, so it cannot return (and let the framework free the surface)
 * while a draw_impl is in flight. Fixes the intermittent main-screen SIGSEGV.
 */
static void draw(struct ANativeWindow *win)
{
   if (!win)
      return;
   /* Render only on the main looper thread. A draw requested from a BLE binder
    * thread (a reading/status/advert) is coalesced into g_ui_dirty and painted
    * by the next on_timer tick -- so draw_impl and the hit-box geometry it
    * rebuilds are never touched concurrently with on_input. */
   if (!on_main()) {
      g_ui_dirty = 1;
      return;
   }
   /* SEQ_CST on both this exchange and the g_win load below (paired with the
    * SEQ_CST store/load in on_window_destroyed) forbids the store-buffer
    * outcome where this thread sees the old g_win AND the destroyer sees
    * g_draw_busy==0 -- which would let the surface be freed mid-draw. */
   /* Before the draw flag, never inside it -- see snap_drivers(). */
   snap_drivers();
   snap_registry(); /* both BEFORE the draw flag -- see snap_registry */
   if (__atomic_exchange_n(&g_draw_busy, 1, __ATOMIC_SEQ_CST))
      return;
   if (win ==
       __atomic_load_n(&g_win, __ATOMIC_SEQ_CST)) /* still the live one */
      draw_impl(win);
   __atomic_store_n(&g_draw_busy, 0, __ATOMIC_SEQ_CST);
}

static void draw_impl(struct ANativeWindow *win)
{
   struct ANativeWindow_Buffer buf;

   if (!win)
      return;
   ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBA_8888);
   if (ANativeWindow_lock(win, &buf, NULL) != 0)
      return;

   /* Every screen is the pure UI now: build the immutable frame model, render
    * it, and record the touch targets. build_model picks the screen from
    * g_gate / g_menu; ui_render clears the framebuffer itself. */
   struct screen sm;
   build_model(&sm);
   ui_render(&buf, &sm, &g_hits);
   /* Resting frames run at 13/16 intensity so the armed full-intensity
    * highlight below is visible even on white text and the green big
    * number, which have no headroom at full brightness. */
   ui_dim(&buf);
   /* Pressed-but-not-yet-fired: shade the armed control (act-on-release --
    * see g_arm_*). Re-found in the JUST-rebuilt hit boxes via the finger's
    * last on-target point, and only shaded while it still resolves to the
    * same action -- a redraw that moved or removed the control drops the
    * shade rather than lighting a stranger. */
   if (g_arm_kind != ACT_NONE && g_arm_in) {
      int bi = ui_hit_idx(&g_hits, g_arm_x, g_arm_y);
      if (bi >= 0 && g_hits.box[bi].kind == g_arm_kind &&
          g_hits.box[bi].arg == g_arm_arg)
         /* the GLOW rect, not the hit rect: usually the same, narrower for
          * controls whose hit zone out-sizes their glyph (see add_glow) */
         ui_press_overlay(&buf, g_hits.box[bi].gx, g_hits.box[bi].gy,
                          g_hits.box[bi].gw, g_hits.box[bi].gh);
   }

   ANativeWindow_unlockAndPost(win);
}

/* Rebuild the text lines; redraw only if something visible changed, and at
 * most ~5 times/second so radio chatter can't saturate the main thread. */
static void update_screen(void)
{
   /* Off the main thread (a BLE-thread status/advert update), don't rebuild the
    * shared text lines or draw -- just mark dirty; on_timer rebuilds+paints. */
   if (!on_main()) {
      g_ui_dirty = 1;
      return;
   }
   char next[MAX_LINES][MAX_COLS + 1];
   int n = 0;

   /* g_status is written by set_status on a BLE binder thread; snapshot it with
    * a bound so this read can never scan off the end during a racing write. */
   char st[MAX_COLS + 1];
   str_snapshot(st, sizeof st, g_status);
   (void)snprintf(next[n++], sizeof next[0], "PANCRA  %s", st);
   /* total rounded to 10s so ambient chatter doesn't redraw every advert.
    * g_ndevs is written by the binder-thread advert handler under devlist_lock,
    * so snapshot it under the same lock -- honouring the invariant every other
    * g_ndevs reader holds. (g_total is a benign rounded pipe-health counter
    * whose writer takes no lock; reading it unlocked is fine.) */
   devlist_lock();
   int ndev_shown = g_ndevs;
   devlist_unlock();
   (void)snprintf(next[n++], sizeof next[0], "ADV %u  DX %d", g_total / 10 * 10,
                  ndev_shown);

   int changed = (n != g_nlines);
   for (int i = 0; !changed && i < n; i++)
      changed = strcmp(next[i], g_lines[i]) != 0;
   if (!changed)
      return;

   g_nlines = n;
   for (int i = 0; i < n; i++)
      (void)snprintf(g_lines[i], sizeof g_lines[0], "%s", next[i]);

   static long long last_draw_ms;
   long long now = now_ms();
   if (now - last_draw_ms < 200)
      return; /* next change will repaint */
   last_draw_ms = now;
   draw(g_win);
}

/* --- Java -> C: one advertisement heard (BLE binder thread) ---
 *
 * NOT the main thread: onAdvert is delivered from ScanCallback.onScanResult
 * (Ble.java), i.e. a Bluetooth-stack binder thread, while the main looper reads
 * AND resets g_devs/g_ndevs (build_model, select_candidate, commit_pair gate,
 * the pairing reset). Registry access below is taken under sensors_lock; the
 * candidate-list write is taken under devlist_lock, which every reader/resetter
 * of g_ndevs also holds, so the read-modify-write increment here is atomic
 * against the main-thread reset and no reader sees a counted-but-unwritten
 * row. */

static void jni_on_advert(JNIEnv *env, jclass cls, jstring jname, jstring jmac,
                          jint rssi)
{
   (void)cls;
   const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
   const char *mac  = (*env)->GetStringUTFChars(env, jmac, NULL);
   /* GetStringUTFChars returns NULL on OOM (with an exception pending); on this
    * per-advert hot path a NULL deref would crash. Bail cleanly instead. */
   if (!name || !mac) {
      if (name)
         (*env)->ReleaseStringUTFChars(env, jname, name);
      if (mac)
         (*env)->ReleaseStringUTFChars(env, jmac, mac);
      if ((*env)->ExceptionCheck(env))
         (*env)->ExceptionClear(env);
      return;
   }

   g_total++;
   /* Which Dexcom families we will talk to at all. Stelo advertises "DX01",
    * G7 "DXCM"; both are supported. The safety property is NOT "never a G7" --
    * it is "never a sensor the user did not choose here":
    *   - once bonded, auto-connect ONLY to that exact sensor's MAC (s.mac), so
    *     a stranger's sensor in range is never touched, and
    *   - before pairing, only a device the user picks in ADD SENSOR is used.
    * PAIR NEW SENSOR (g_smart_pairing) suppresses the auto path and selects by
    * code + proximity instead.
    *
    * Note for testing, not for the code: the user's own G7 is a live medical
    * device and must not be exercised until they choose to do so themselves.
    * That is a discipline about which sensor you pair during a test, not a
    * restriction compiled into the app. */
   int is_dexcom =
       strncmp(name, "DX01", 4) == 0 || strncmp(name, "DXCM", 4) == 0;
   int is_meter = strncmp(name, "OneTouch", 8) == 0;
   /* The device list shows whichever family the user is currently adding, so a
    * meter is discoverable in ADD SENSOR -> ONETOUCH and a sensor is not
    * offered when they asked for a meter. */
   /* EXCLUDE DEVICES ALREADY IN THE REGISTRY from the pairing candidate list.
    *
    * The family filter alone let a sensor you have already paired appear as a
    * candidate -- and with just that one in range (the common case when you
    * enter the code before applying the replacement) select_candidate returns
    * it unopposed, the list never appears, and commit_pair runs on the LIVE
    * sensor's address: a J-PAKE re-pair against a sensor that is already
    * bonded and streaming, burning the link it was using. You cannot pair
    * something that is already paired, so it does not belong in the list. */
   int known = 0;
   {
      sensors_lock();
      int kidx  = sensor_slot_by_mac(mac);
      int kkind = KIND_BGM;
      if (kidx >= 0) {
         const struct sensor_rec *kr = sensor_rec_by_id(g_slot[kidx].id);
         if (kr)
            kkind = sensor_kind(kr->type);
      }
      sensors_unlock();
      if (kidx >= 0 && kkind == KIND_CGM) {
         /* A CGM is registered the moment the user COMMITS to pairing it, so
          * "has a slot" no longer implies "is paired". Exclude it from the
          * candidate list only once a BONDED session exists -- the state a
          * slot used to imply. Without this, one failed pairing (wrong code,
          * out of range) left the sensor registered-but-never-bonded and
          * permanently missing from ADD SENSOR: unretryable without first
          * forgetting the device, with nothing saying so. */
         int klink = link_for_slot(kidx);
         if (klink >= 0) {
            struct dex_session ks;
            driver_lock();
            int kprev = driver_link();
            driver_select(klink);
            driver_get_session(&ks);
            driver_select(kprev);
            driver_unlock();
            known = ks.bonded;
         }
      } else {
         known = (kidx >= 0); /* meters keep the pre-existing rule */
      }
   }
   int listed =
       !known && ((sensor_kind(g_add_type) == KIND_BGM) ? is_meter : is_dexcom);
   if (is_dexcom && !g_smart_pairing) {
      /* Auto-connect ONLY to a sensor already in the registry, on ITS OWN
       * link. Matching against the registry rather than "the bonded sensor"
       * is what lets several CGMs stream at once: each advertises on its own
       * schedule and reconnects independently, so a stalled one cannot keep
       * another off the air. A device we never paired is ignored entirely --
       * that, not any family filter, is the safety property. */
      /* Snapshot the slot list under the registry lock before walking it.
       *
       * This runs on a BINDER thread while the main thread can be inside
       * sensor_forget_slot's shift-down and another binder thread inside
       * srec_push's memmove. Reading g_nslot and holding a sensor_rec* across
       * the driver calls below is the exact hazard link_for_slot and
       * src_for_link were both rewritten to close -- a torn read here hands
       * dexble_pair a link resolved from a different sensor's identity, so one
       * sensor's address is bound to another's link and key file. */
      int n_ids = 0;
      int ids[MAX_SLOTS];
      int slotidx[MAX_SLOTS]; /* the ORIGINAL g_slot index, see below */
      int match[MAX_SLOTS];
      sensors_lock();
      for (int i = 0; i < g_nslot && n_ids < MAX_SLOTS; i++) {
         if (g_slot[i].old) /* disconnected: never auto-reconnect */
            continue;
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
         if (!r || sensor_kind(r->type) != KIND_CGM)
            continue;
         ids[n_ids]     = g_slot[i].id;
         slotidx[n_ids] = i;
         match[n_ids]   = (strcmp(r->identity, mac) == 0);
         n_ids++;
      }
      sensors_unlock();
      for (int i = 0; i < n_ids; i++) {
         if (!match[i])
            continue;
         /* link_for_slot wants a g_slot INDEX, but this array is COMPACTED --
          * it skips non-CGM slots. Passing `i` meant that with a meter
          * registered before a CGM (or after any forget shifted one down), the
          * CGM's advert resolved the METER's slot, returned LINK_METER, found
          * no session there and gave up: that CGM's advert-driven reconnect
          * never fired again, for the life of the install, with no visible
          * cause. Carry the original index. */
         int link = link_for_slot(slotidx[i]);
         if (link < 0)
            break;
         struct dex_session ls;
         driver_lock();
         driver_select(link);
         driver_get_session(&ls);
         driver_unlock();
         if (!ls.mac[0])
            break; /* registered but not yet bonded: ADD SENSOR owns it */
         /* A sensor advertises repeatedly inside one wake window. Re-issuing
          * connect on every advert would be a connect storm -- hard on the
          * sensor's battery and a good way to strand the link -- so allow one
          * attempt per link per cycle, and none at all while it is already
          * delivering readings. */
         /* "Already streaming" must be judged from THIS sensor's own last
          * reading. g_cur_time is the global newest CGM sample, bound to the
          * PRIMARY sensor -- so a healthy primary made every other sensor look
          * live and suppressed its reconnect indefinitely. ctx->g_bonded is no
          * help either: it is set on auth and cleared only by driver_forget,
          * never on disconnect, so it stays 1 across a dropped link. */
         long tnow = realtime_s();
         long mine = 0;
         hist_lock();
         for (int k = 0; k < g_nhist && !mine; k++)
            if (g_hist[k].src == (unsigned short)ids[i] &&
                g_hist[k].kind != KIND_BGM)
               mine = g_hist[k].t;
         hist_unlock();
         if (ls.bonded && mine && tnow - mine < 300)
            break; /* this sensor really is streaming */
         if (tnow - g_link_try[link] < 30)
            break;
         g_link_try[link] = tnow;
         LOGI("sensor %s %s -> reconnect on link %d", name, mac, link);
         dexble_pair(link, mac, g_code_str);
         break;
      }
   }
   /* A OneTouch meter advertises only while the user has it switched on, so
    * seeing it IS the trigger: sync now, on its own link, without disturbing
    * the CGM link. No polling -- a meter that is off costs nothing. */
   if (is_meter && !g_meter_busy) {
      /* Resolve the meter from THIS advert's address, against every registered
       * meter slot -- not against a single remembered MAC.
       *
       * sensor_reconcile latches g_meter_src/g_meter_mac from the FIRST
       * OneTouch slot it finds, so with two meters registered the second could
       * never sync: its adverts failed the address test forever, silently, with
       * no user-visible cause. Matching per advert also keeps the safety
       * property that made the old test exist -- a stranger's meter is still
       * ignored, because it has no slot. */
      int mid = -1;
      sensors_lock();
      int midx = sensor_slot_by_mac(mac);
      if (midx >= 0 && !g_slot[midx].old) { /* a disconnected meter is inert */
         const struct sensor_rec *mr = sensor_rec_by_id(g_slot[midx].id);
         if (mr && mr->type == SENSOR_ONETOUCH)
            mid = g_slot[midx].id;
      }
      sensors_unlock();
      /* PER-METER throttle: only rate-limit THIS meter, so one meter syncing
       * never blocks another (the global gate here made a second meter that
       * advertised alongside the first never get a turn). */
      struct meter_rt *rt = (mid > 0) ? meter_rt_get(mid, 1) : 0;
      long mlast          = rt ? rt->sync_t : 0;
      if (mid > 0 && realtime_s() - mlast > 60) {
         /* The advertisement IS the "last seen" event, and it carries an
          * RSSI -- so SIGNAL STRENGTH is stamped here, from the same advert,
          * not left blank until a connection completes (a meter that
          * advertised but did not finish a sync used to show LAST SEEN with a
          * "--" signal). A completed sync's connection RSSI
          * (pancra_meter_rssi) refines this afterwards. meter_sync_start
          * persists it, so it must be set BEFORE the call. */
         if (rt) {
            /* An advertisement IS proof the meter is switched on and within
             * range, so this is a real "last seen" -- and it is the only
             * place other than an actual connection that may say so. */
            rt->sync_t = realtime_s();
            if (rssi <= -1 && rssi >= -127) {
               rt->rssi    = rssi;
               rt->rssi_ok = 1;
               rt->rssi_t  = realtime_s();
            }
            meter_sync_save(); /* survives a restart */
         }
         /* Already armed? Then the controller is initiating on its own and
          * re-issuing the connect would only cancel and restart it -- during
          * the one second the meter is awake. With every meter armed that is
          * the normal case, so this path is now only a fallback for a meter
          * that could not get a link of its own. */
         /* Skip, do NOT return: this is still an advertisement the device
          * list below has to record, and returning would silently drop the
          * meter out of the ADD DEVICE list. */
         if (!meter_armed(mac)) {
            LOGI("meter %s (id %d) advertising, not armed -> connect", mac,
                 mid);
            meter_sync_start(mid, mac);
         }
      }
   }

   if (listed) {
      int did_log_new        = 0;
      unsigned did_log_count = 0;
      devlist_lock();
      int i = 0;
      for (i = 0; i < g_ndevs; i++)
         if (strcmp(g_devs[i].mac, mac) == 0)
            break;
      int is_new = (i == g_ndevs && g_ndevs < MAX_DEVS);
      if (is_new || i < g_ndevs) {
         /* Fill the slot, then publish a newly-added one by bumping g_ndevs.
          * All of this is under devlist_lock, so the find/increment is atomic
          * against the main-thread reset and readers never see a
          * counted-but-unwritten row. An existing slot is only ever rewritten
          * with its own matched mac. */
         (void)snprintf(g_devs[i].name, sizeof g_devs[i].name, "%s", name);
         (void)snprintf(g_devs[i].mac, sizeof g_devs[i].mac, "%s", mac);
         g_devs[i].rssi   = rssi;
         g_devs[i].seen_t = realtime_s();
         g_devs[i].count++;
         if (is_new) {
            g_ndevs++;
            did_log_new = 1;
         }
         /* one cadence line per device per 30 s, to time advert bursts */
         long long now = now_ms();
         if (now - g_devs[i].last_log_ms > 30000) {
            g_devs[i].last_log_ms = now;
            did_log_count         = g_devs[i].count; /* nonzero -> log below */
         }
      }
      devlist_unlock();
      /* Log OUTSIDE the lock -- LOGI is not part of the guarded state and can
       * be slow; the lock is a leaf held only across the field writes. */
      if (did_log_new)
         LOGI("new Dexcom device: %s %s %d", name, mac, rssi);
      if (did_log_count)
         LOGI("dexcom adv: %s %s rssi %d count %u", name, mac, rssi,
              did_log_count);
   }

   (*env)->ReleaseStringUTFChars(env, jname, name);
   (*env)->ReleaseStringUTFChars(env, jmac, mac);
   update_screen();
}

/* --- permissions --- */

static const char *perm_modern[] = {"android.permission.BLUETOOTH_SCAN",
                                    "android.permission.BLUETOOTH_CONNECT"};
static const char *perm_legacy[] = {"android.permission.ACCESS_FINE_LOCATION"};

static int has_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env       = a->env;
   const char **want = a->sdkVersion >= 31 ? perm_modern : perm_legacy;
   jsize n           = a->sdkVersion >= 31 ? 2 : 1;

   jclass act      = (*env)->GetObjectClass(env, a->clazz);
   jmethodID check = (*env)->GetMethodID(env, act, "checkSelfPermission",
                                         "(Ljava/lang/String;)I");
   int ok          = 1;
   for (jsize i = 0; i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      if ((*env)->CallIntMethod(env, a->clazz, check, s) != 0)
         ok = 0;
      (*env)->DeleteLocalRef(env, s);
   }
   /* See start_scan: local refs made on the native looper thread are never
    * reclaimed by a frame pop. tz_offset_at was already fixed for this; these
    * were missed, and the 30 s scan self-heal calls this one repeatedly. */
   (*env)->DeleteLocalRef(env, act);
   return ok;
}

/* Ask for every runtime permission the app wants, in one dialog sequence: the
 * BLE pair needed to reach the sensor plus notifications so alarms can alert.
 * The battery-optimisation exemption isn't a runtime permission (it's a
 * settings intent) -- g_want_battery makes on_resume pop it right afterwards.
 * The result callback never reaches native code; grant state is re-checked on
 * resume. */
static void request_ble_permissions(struct ANativeActivity *a)
{
   JNIEnv *env = a->env;
   const char *want[4];
   jsize n = 0;
   if (a->sdkVersion >= 31) {
      want[n++] = "android.permission.BLUETOOTH_SCAN";
      want[n++] = "android.permission.BLUETOOTH_CONNECT";
   } else {
      want[n++] = "android.permission.ACCESS_FINE_LOCATION";
   }
   if (a->sdkVersion >= 33)
      want[n++] = "android.permission.POST_NOTIFICATIONS";

   jclass act       = (*env)->GetObjectClass(env, a->clazz);
   jmethodID req    = (*env)->GetMethodID(env, act, "requestPermissions",
                                          "([Ljava/lang/String;I)V");
   jclass strcls    = (*env)->FindClass(env, "java/lang/String");
   jobjectArray arr = (*env)->NewObjectArray(env, n, strcls, NULL);
   for (jsize i = 0; i < n; i++) {
      jstring s = (*env)->NewStringUTF(env, want[i]);
      (*env)->SetObjectArrayElement(env, arr, i, s);
      (*env)->DeleteLocalRef(env, s);
   }
   (*env)->CallVoidMethod(env, a->clazz, req, arr, (jint)1);
   (*env)->DeleteLocalRef(env, arr);
   (*env)->DeleteLocalRef(env, strcls);
   (*env)->DeleteLocalRef(env, act);
   g_want_battery = 1; /* pop the battery-exemption prompt on the next resume */
}

/* FindClass inside struct ANativeActivity callbacks resolves via the
 * framework's class loader, which can't see app classes; go through the
 * activity's own loader instead. Takes a dotted name ("com.jk.pancra.Ble"). */
static jclass find_app_class(struct ANativeActivity *a, const char *name)
{
   JNIEnv *env       = a->env;
   jclass act_cls    = (*env)->GetObjectClass(env, a->clazz);
   jmethodID get_cl  = (*env)->GetMethodID(env, act_cls, "getClassLoader",
                                           "()Ljava/lang/ClassLoader;");
   jobject loader    = (*env)->CallObjectMethod(env, a->clazz, get_cl);
   jclass loader_cls = (*env)->GetObjectClass(env, loader);
   jmethodID load    = (*env)->GetMethodID(
       env, loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
   jstring jname = (*env)->NewStringUTF(env, name);
   jclass cls    = (*env)->CallObjectMethod(env, loader, load, jname);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      return NULL;
   }
   return cls;
}

/* --- scan lifecycle (all on main thread) --- */

static void set_status(const char *s)
{
   (void)snprintf(g_status, sizeof g_status, "%s", s);
   update_screen();
}

/* Re-evaluate the alarm against the latest fresh reading and current
 * thresholds. Chime+vibrate once on the transition from NOT-alarmed to alarmed
 * -- whether that transition comes from a new glucose value or from moving a
 * threshold. Never re-fires while already alarmed (low<->high is not a new
 * entry); silences when the value returns in range. Safe to call on every
 * reading or threshold tap. */
/* Alarm state, and why it is shaped this way.
 *
 * alarm_apply is called from THREE kinds of thread, not one. This comment
 * used to claim "every caller runs on the MAIN looper" and "the BLE threads
 * deliberately do not evaluate alarms" -- both false, and dangerously so,
 * because they are exactly the assumptions someone would rely on when deciding
 * a lock here is unnecessary. The real callers are:
 *   - the main looper: disc_reeval() on the 1 Hz timer, alarm_reeval() from a
 *     threshold tap, and the tap-to-silence path in on_input;
 *   - a GATT binder thread: jni_notify calls pancra_alarm_check AFTER
 *     releasing driver_lock (which is the property that actually matters --
 *     raising an alarm does blocking MediaPlayer work, and doing that under a
 *     no-timeout spin lock the main looper also takes is what must not happen);
 *   - the service's tick HandlerThread, via jni_tick.
 * alarm_lock is therefore genuinely contended and genuinely load-bearing.
 *
 * Two properties are load-bearing and must survive any future change:
 *
 * 1. LEVEL-based, not edge-triggered. alarm_apply recomputes what should be
 *    sounding and reconciles, so a missed transition self-corrects on the next
 *    tick. The original edge-triggered version lost a disconnect alarm
 *    permanently when a reading silenced it a microsecond after it was raised:
 *    g_disc_alarmed stayed latched at 1, so the !alarmed -> alarmed edge never
 *    happened again.
 * 2. Raise and silence are mutually exclusive END TO END, JNI call included.
 *    Splitting the decision from the call lets two threads decide in one order
 *    and call Java in the other; Alarm.trigger/silence being `synchronized`
 *    orders them against each other but cannot repair a wrong order. The
 *    result was an alarm that looped forever with nothing able to stop it.
 *
 * alarm_lock is what keeps property 2 true across those three threads. */
static volatile int alarm_owner;
static int alarm_depth;

static void alarm_lock(void)
{
   int me = gettid();
   if (__atomic_load_n(&alarm_owner, __ATOMIC_SEQ_CST) == me) {
      alarm_depth++;
      return;
   }
   while (!__sync_bool_compare_and_swap(&alarm_owner, 0, me))
      sched_yield();
   alarm_depth = 1;
}

static void alarm_unlock(void)
{
   if (--alarm_depth > 0)
      return;
   __atomic_store_n(&alarm_owner, 0, __ATOMIC_SEQ_CST);
}

/* What we last asked Java to sound (0 = silent). Call with alarm_lock held. */
static int g_alarm_want;
/* The user has DISMISSED the level currently in g_alarm_want.
 *
 * Acknowledgement used to be recorded only as "g_alarm_want still equals this
 * level", which alarm_reactuate destroys by design -- so toggling SOUND or
 * VIBRATION restarted an alarm the user had already silenced, from a settings
 * screen they could only reach because it was silent. Recorded explicitly, it
 * survives a re-actuation and is cleared by a genuine level change, which is a
 * new alarm they have not seen. Call with alarm_lock held. */
static int g_alarm_acked;

/* Most recent PREDICTED value (mg/dL) and its wall-clock, per CGM link. Written
 * in pancra_glucose as each reading is decoded (driver_lock held there); read
 * in the alarm evaluation. Plain aligned int/long, single-writer per link. */
static int g_link_pred[LINK_MAX];
static long g_link_pred_t[LINK_MAX];

/* Imminent hypo: ANY CGM whose latest reading predicts below PRED_LOW_MGDL, and
 * which reported within the alarm freshness window. The freshness gate is what
 * keeps an unsilenceable alarm from wedging on stale data after a sensor drops
 * out -- the prediction must be current, not a value frozen at disconnect. */
#define PRED_LOW_MGDL 55

static int any_pred_low(void)
{
   long now = realtime_s();
   for (int l = 0; l < LINK_MAX; l++) {
      if (g_link_meter[l])
         continue; /* CGMs only */
      if (g_link_pred[l] > 0 && g_link_pred[l] < PRED_LOW_MGDL &&
          g_link_pred_t[l] > 0 && now - g_link_pred_t[l] <= AL_FRESH_S)
         return 1;
   }
   return 0;
}

static void alarm_apply_ex(int zone, int stale, int stranded, int pred_low)
{
   int sound_on = g_sound_on;
   int vib_on   = g_vib_on;
   /* A glucose excursion outranks a stale-data warning: it is the more urgent
    * fact and the one the user must act on. */
   /* A DISMISSED level must stop sustaining.
    *
    * The sustain exists to stop a RINGING alarm being silenced by its own data
    * ageing out. Once the user has dismissed it nothing is ringing, so there
    * is nothing to protect -- but the sustain still pinned g_alarm_want, and
    * because a glucose level outranks `stale`, the level could never change
    * again without a fresh reading. The result: dismiss a hypo, have the
    * sensor come off, and the user's configured DISCONNECT alarm is silently
    * disabled forever (stranded stays 1 because the reading never ages back
    * into freshness). Enabling DISCONNECT afterwards did nothing either.
    * g_alarm_acked is readable here because this runs under alarm_lock. */
   int want = alarm_want_sustained(zone, stale, stranded && !g_alarm_acked,
                                   g_alarm_want);
   /* IMMINENT HYPO OVERRIDE. A CGM predicting < 55 mg/dL is a forced,
    * UNSILENCE- ABLE LOW: it outranks every other level, sounds even when the
    * user has the alarm sound switched off, and cannot be dismissed while the
    * prediction holds. If the user silenced it (nothing sounding) while it
    * still holds, clear the remembered level so alarm_decide sees a fresh
    * NONE->LOW edge and RE-TRIGGERS on the next 1 Hz tick -- that re-trigger is
    * what makes it unsilenceable. Freshness is enforced in any_pred_low(), so a
    * stale prediction can never pin this on. */
   if (pred_low) {
      want     = AL_LOW;
      sound_on = 1; /* must be heard regardless of the SOUND setting */
      if (!g_alarm_sounding)
         g_alarm_want = AL_NONE;
   }
   /* Decision in alarmlogic.c so `make check` can fail on it; this function
    * holds the state and performs the actuation. */
   struct alarm_out out;
   alarm_decide(want, g_alarm_want, sound_on, vib_on, &out);
   if (out.act == AL_ACT_NONE)
      return;
   /* Save EVERY field the rollback below has to undo. Missing one leaves a
    * half-applied state: rolling `want` back to a level the user had dismissed
    * while leaving `acked` cleared re-arms it for the next settings toggle,
    * which is the dismissal bug reintroduced through the failure path. */
   int prev_want     = g_alarm_want;
   int prev_sounding = g_alarm_sounding;
   int prev_acked    = g_alarm_acked;
   g_alarm_want      = out.want;
   g_alarm_acked     = out.acked;
   /* "Sounding" must mean something is ACTUALLY audible or tactile, because
    * its only other job is to make the next tap anywhere in the app silence
    * the alarm and do nothing else (see on_input). With SOUND and VIBRATION
    * both switched off, Alarm.trigger plays nothing and buzzes nothing, yet
    * this used to latch anyway -- so the user got a silent alarm AND their
    * next tap, e.g. on the big number to open settings, was swallowed with no
    * on-screen explanation of why. The LOW/HIGH/STALE banner still shows the
    * condition either way; this flag is purely about the silence gesture. */
   g_alarm_sounding = out.sounding;
   /* The JNI call stays INSIDE the lock, deliberately.
    *
    * Releasing it around the call is tempting -- Alarm.trigger does
    * setDataSource() + prepare() + start(), which is synchronous and can take
    * hundreds of milliseconds, so a concurrent evaluator spins meanwhile. But
    * dropping it opens an un-silenceable alarm:
    *
    *   binder: commits want=LOW, unlocks, enters trigger() (slow)
    *   user taps: lock is free, so silence proceeds -- g_alarm_sounding = 0,
    *              Alarm.silence() runs and stops nothing (no player yet)
    *   binder: trigger() then creates and starts a LOOPING alarm player
    *
    * The tone now plays forever: g_alarm_want is still LOW so no later
    * alarm_apply issues a silence, and g_alarm_sounding is 0 so tapping again
    * does nothing. Alarm.trigger/silence being `synchronized` orders them
    * against each other but cannot fix the wrong ORDER. Raise and silence must
    * be mutually exclusive end-to-end, so the lock spans the call. Burning a
    * binder thread for a few hundred ms on an alarm transition is a trade
    * worth making; an alarm that cannot be stopped is not. */
   int kind = alarm_java_kind(out.want);
   if (out.act == AL_ACT_TRIGGER && kind >= 0) {
      /* DO NOT keep the level committed if Java was never reached. The
       * idempotence check above would then suppress every later attempt and
       * the alarm would stay silent for its whole duration. Rolling the
       * commitment back restores the level-based self-correction that this
       * design depends on: the next tick simply tries again. */
      if (!dexble_alarm(kind, sound_on, vib_on)) {
         LOGI("alarm: actuation failed; will retry on the next evaluation");
         g_alarm_want     = prev_want;
         g_alarm_sounding = prev_sounding;
         g_alarm_acked    = prev_acked;
      }
   } else if (!dexble_alarm_silence()) {
      /* Same rollback discipline as the trigger path. Committing "silent"
       * while Java was never reached leaves a looping USAGE_ALARM player with
       * want == AL_NONE (no later apply issues another silence) and
       * sounding == 0 (every tap falls through to the UI) -- an alarm nothing
       * can stop. */
      LOGI("alarm: silence failed; will retry on the next evaluation");
      g_alarm_want     = prev_want;
      g_alarm_sounding = prev_sounding;
      g_alarm_acked    = prev_acked;
   }
}

/* The glucose zone RIGHT NOW: 0 in range, 1 low, 2 high. Derived, never
 * latched. Call with alarm_lock held.
 *
 * This must be recomputed by the 1 Hz path as well as by the reading path.
 * When only alarm_reeval() (which runs when a reading arrives) updated the
 * zone, the freshness test below effectively always passed -- it ran
 * microseconds after a reading landed -- so a non-zero zone could never decay.
 * disc_reeval() on the 1 Hz timer now recomputes it, which is also what makes
 * a glucose alarm get raised at all now that the BLE threads do not evaluate.
 * A sensor dropping out while low then left `zone` latched at 1 forever, and
 * because alarm_apply ranks zone above stale, the DISCONNECT alarm could never
 * sound: exactly the lost-alarm failure the level-based rewrite existed to
 * prevent, reintroduced through a different door. */
/* The current reading, read as ONE consistent pair.
 *
 * hist_refresh_current writes g_cur_glu and g_cur_time as separate stores
 * under hist_lock, while the alarm path read them with no lock at all, from
 * three different threads. A read that landed between the two stores paired a
 * new glucose with the previous timestamp -- so a genuine LOW could evaluate
 * as "not fresh" and not be raised on that pass -- or paired a stale
 * out-of-range value with a fresh timestamp and chimed something that then
 * silenced itself.
 *
 * Taken SEQUENTIALLY, before alarm_lock, never nested inside it: hist is the
 * innermost lock in the documented order, and nesting it under alarm would add
 * the one edge that could still form a cycle. Sequential costs nothing here --
 * the pair only has to be self-consistent, not held. */
struct alarm_reading {
   int glu;
   long t;
};

static struct alarm_reading current_reading(void)
{
   struct alarm_reading r;
   hist_lock();
   r.glu = g_cur_glu;
   r.t   = g_cur_time;
   hist_unlock();
   return r;
}

/* The excursion verdict across EVERY registered CGM, each judged on its OWN
 * newest sample with the standard freshness gate, merged worst-first (a LOW
 * anywhere outranks anything -- alarm_zone_merge). The DISPLAY belongs to
 * the primary; the ALARM watches every sensor the user wears, so a low on
 * the non-primary sensor rings too. Stranded is merged the same way, so an
 * out-of-range sensor going silent sustains the alarm no matter which one
 * it was. With no CGM registered at all (a pre-registry install) the
 * current reading -- src-0 legacy data -- is judged instead.
 *
 * Gathering takes the registry lock, then hist_lock, SEQUENTIALLY -- and
 * must complete before alarm_lock is taken: hist is non-recursive and is
 * the same flag as g_draw_busy, so nesting it inside alarm is the one edge
 * that could still close a lock cycle. */
/* Is the ALARM announcing anything right now? The input nudge_fire needs, and
 * broader than this tick's excursion zone on purpose: g_alarm_want carries the
 * DISCONNECT alarm and the stranded sustain, neither of which shows up in the
 * zone, and `pred` is the imminent-hypo override, which forces AL_LOW at any
 * current reading. Called under alarm_lock (g_alarm_want is read there), and
 * it reads the level committed by the PREVIOUS evaluation -- which is exactly
 * the alarm the user can hear right now. */
static int alarming(int zone, int pred)
{
   return zone || pred || g_alarm_want != AL_NONE;
}

static int meter_link_of(const char *mac)
{
   if (!mac || !mac[0])
      return -1;
   int found = -1;
   driver_lock();
   for (int l = 0; l < LINK_MAX && found < 0; l++)
      if (g_link_armed[l][0] && strcmp(g_link_armed[l], mac) == 0)
         found = l;
   driver_unlock();
   return found;
}

static int meter_armed(const char *mac)
{
   return meter_link_of(mac) >= 0;
}

/* Begin a meter sync on that meter's OWN link: seed the driver with its stored
 * record index, clear the shared DIS strings, take the busy latch, and connect.
 *
 * SHARED BY THE ADVERT PATH AND "SYNC NOW", and it must be, because every step
 * here is load-bearing and getting one wrong corrupts data rather than merely
 * failing. The index is PER-DEVICE: sharing one across meters made each sync
 * read the other's counter as "gone backwards", so they reset each other
 * forever and one meter's records were never reached again. The DIS strings
 * are process-global for the meter link, so a sync that finishes before the
 * reads land -- the common case, since "nothing new" ends after one round
 * trip -- would otherwise mint this meter against the PREVIOUS meter's model
 * and firmware. Two call sites doing this by hand is two chances to omit one.
 *
 * The connect itself is autoConnect=true (Ble.java), so it does NOT need the
 * meter to be advertising right now: the stack latches on as soon as the
 * device is reachable. That is what lets SYNC NOW mean something. */
/* A free link a meter may take, or -1.
 *
 * Shared by the arming path and by pairing a NEW meter. Pairing used to go
 * through link_for_slot, whose free-link search knows nothing about armed
 * meters and will happily hand back LINK_CGM. Two callers, one rule. */
static int meter_alloc_link(void)
{
   int link = -1;
   /* CGMS COME FIRST. A CGM only claims its link when it next advertises,
    * while the tick arms every meter within seconds of launch -- so with
    * more devices than links the meters would take them all and a sensor
    * that streams continuously would be left with none. Count the live
    * CGMs still waiting for a link and leave that many free. */
   driver_lock();
   int prev = driver_link();
   int want = 0;
   sensors_lock(); /* driver -> reg, the documented order */
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].old)
         continue;
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (!r || sensor_kind(r->type) != KIND_CGM)
         continue;
      char qid[24];
      str_snapshot(qid, sizeof qid, r->identity);
      if (link_for_identity(qid) < 0)
         want++;
   }
   sensors_unlock();
   /* FROM LINK_CGM + 1. Link 0 is reserved for a sensor.
    *
    * Nothing routes by link id any more, so a meter on link 0 would work
    * -- but a dozen places still say driver_select(LINK_CGM) meaning "the
    * usual sensor context", and the key-file names drop the suffix for
    * link 0 (dexble.c). At cold start no CGM is bound yet, so link 0 looks
    * free and the first meter armed would take it, quietly changing what
    * all of those refer to. There are eight links; leaving the first to
    * the sensors costs nothing and keeps every one of those assumptions
    * true. */
   int freen = 0;
   for (int l = LINK_CGM + 1; l < LINK_MAX; l++) {
      if (g_link_armed[l][0])
         continue;
      driver_select(l);
      struct dex_session ls;
      driver_get_session(&ls);
      if (!ls.mac[0])
         freen++;
   }
   if (freen > want) {
      for (int l = LINK_CGM + 1; l < LINK_MAX && link < 0; l++) {
         if (g_link_armed[l][0])
            continue; /* another meter holds it */
         driver_select(l);
         struct dex_session ls;
         driver_get_session(&ls);
         if (!ls.mac[0]) /* no CGM bound here */
            link = l;
      }
   }
   driver_select(prev);
   driver_unlock();
   return link;
}

static void meter_sync_start(int mid, const char *mac)
{
   /* Its OWN link, from the shared pool. Every registered meter holds one, so
    * all of them can carry a standing connect at once -- with a single
    * reserved link only the last-used meter could, and the others were back
    * to catching a two-second advertisement. */
   /* Already armed? Keep the SAME link. Re-allocating would strand the
    * pending connect on the old one and hand this meter a second. */
   int link = meter_link_of(mac);
   if (link < 0) {
      /* A free link: claimed by no other meter, and carrying no CGM session.
       * NOT link_for_slot -- that ranks devices the DEXCOM session binds, and
       * a meter never runs one, so two meters would rank to the same link and
       * the second would evict the first. */
      link = meter_alloc_link();
   }
   if (link < 0 || link >= LINK_MAX) {
      /* Every link is spoken for. Refuse rather than connect on someone
       * else's: routing a meter onto a CGM's link would feed sensor
       * notifications to the meter parser.
       *
       * Throttled. The tick retries every second, so an unthrottled report
       * here wrote a log line and overwrote the status banner once a second
       * for as long as the condition lasted -- burying whatever the status
       * line was actually there to say. */
      static long last_warn;
      long now = realtime_s();
      if (now - last_warn > 60) {
         last_warn = now;
         LOGI("meter %s (id %d): no free link", mac, mid);
         set_status("NO FREE LINK");
      }
      return;
   }
   link_set_meter(link, 1);
   /* ARMED FIRST, THEN CONNECT -- the order matters.
    *
    * pancra_meter_connected identifies the meter from this table, and
    * jni_connected fires on a BINDER thread. A meter switched on right next
    * to the phone connects in milliseconds, so issuing the connect first
    * left a window where the callback read an empty entry, failed to
    * identify the meter, and closed the link -- losing exactly the sync the
    * user was standing there waiting for.
    *
    * If the connect never reaches Java (no JNIEnv, Bluetooth off) the link
    * is released again, so a silent failure cannot leave the meter marked
    * armed forever with nothing behind it. */
   driver_lock(); /* the binder-thread readers hold this */
   str_snapshot(g_link_armed[link], sizeof g_link_armed[link], mac);
   driver_unlock();
   if (!dexble_meter_connect(link, mac)) {
      LOGI("meter %s (id %d): connect did not reach the transport", mac, mid);
      meter_release_link(link);
      return;
   }
   /* DO NOT STAMP LAST SYNC HERE.
    *
    * This used to set rt->sync_t, which was truthful while the only caller
    * was the advert path -- an advertisement means the meter really is
    * switched on and in range. Arming a STANDING connect means nothing of
    * the kind: it is issued for every registered meter on a timer, whether
    * the meter is off, in another room, or a mile away. Stamping here made
    * all three read "SYNCED a few seconds ago" at once, which is a plain lie
    * about whether a fingerstick has been captured -- exactly the fact the
    * user is looking at that row to learn. The stamp belongs where contact
    * is PROVEN: the advert path (seen on air) and ot_drv_status (it
    * answered).
    */
   /* ARM ONLY. Deliberately NOT ot_init / g_meter_src / g_meter_busy here.
    *
    * The connect below may sit pending for hours -- that is the point -- and
    * with every meter holding one, seeding the shared otble state at arm
    * time would let arming meter B reset the protocol out from under a sync
    * already running on meter A: phase to idle mid-walk, last_index
    * replaced, and A's remaining fingersticks written to readings.csv under
    * B's id, in an append-only file that is never rewritten. The state is
    * seeded when a meter actually ANSWERS instead -- pancra_meter_connected
    * -- which is the only moment exactly one meter owns it. */
   LOGI("meter %s (id %d) armed on link %d", mac, mid, link);
}

/* A meter link dropped. 1 if it owned the current exchange, 0 if it was an
 * idle standing connect. Either way the link is released so the next tick
 * re-arms it -- a meter whose connection died must not stay marked armed, or
 * it never reconnects. */
int pancra_meter_disconnected(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   int active = (link == g_meter_link);
   meter_release_link(link);
   if (active) {
      g_meter_link = -1;
      g_meter_busy = 0;
   }
   return active;
}

/* A meter answered on `link`. Returns 1 to let the protocol run, 0 to
 * refuse.
 *
 * THIS is where the sync becomes real, so this is where the shared otble
 * state is seeded and the busy latch taken. Called from the transport's
 * connect callback, under driver_lock. */
int pancra_meter_connected(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   /* Which meter is this? FROM THE ARMED TABLE -- the address we issued the
    * connect with.
    *
    * NOT from the driver session: drv_connect does not write it, only the
    * Dexcom handshake does, so for a meter link it is always empty. Reading
    * it here made the lookup fail every single time, which refused and
    * closed every meter connection that ever arrived -- meters could not
    * sync at all. The armed table is the only record of which meter owns a
    * link. */
   char mac[24];
   str_snapshot(mac, sizeof mac, g_link_armed[link]);
   int mid = -1;
   sensors_lock();
   int idx = mac[0] ? sensor_slot_by_mac(mac) : -1;
   if (idx >= 0) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[idx].id);
      if (r && sensor_kind(r->type) == KIND_BGM)
         mid = g_slot[idx].id;
   }
   sensors_unlock();
   if (mid <= 0) {
      LOGI("meter connect on link %d: no registered meter there", link);
      return 0;
   }
   if (g_meter_busy && g_meter_link != link) {
      /* Another meter is mid-exchange and there is only one protocol state.
       * Refuse; its standing connect is re-armed by the tick, and the meter
       * buffers its records, so nothing is lost -- only deferred. */
      LOGI("meter on link %d deferred: link %d is mid-sync", link,
           g_meter_link);
      return 0;
   }
   g_meter_link = link;
   g_meter_src  = mid;
   str_snapshot(g_meter_mac, sizeof g_meter_mac, mac);
   /* Seed THIS meter's own stored index. The index is per-device: sharing
    * one made each sync read the other's counter as "gone backwards", so
    * they reset each other forever and one meter's records were never
    * reached. */
   ot_init(meter_index_load(mid)); /* caller holds driver_lock */
   /* Clear the DIS strings: they are process-global for a meter link, and a
    * sync that finishes before the reads land -- the common case, since
    * "nothing new" ends after one round trip -- would otherwise mint this
    * meter against the PREVIOUS meter's model and firmware. */
   sensors_lock();
   g_meter_model[0] = 0;
   g_meter_fw[0]    = 0;
   sensors_unlock();
   g_meter_busy  = 1;
   g_meter_start = realtime_s();
   LOGI("meter %s (id %d) answered on link %d -> sync in flight", mac, mid,
        link);
   return 1;
}

/* Emit the one-time nudge. NG_NONE is the answer on all but a handful of the
 * ~86400 ticks in a day, so this is a no-op almost always. Best-effort, like
 * the NEW DATAPOINT beep: a missed nudge is a missed hint, not a missed
 * alarm, and it must never be able to delay or throw into the alarm path. */
static void nudge_emit(int ng)
{
   if (ng == NG_NONE)
      return;
   /* Its OWN outputs, not the alarm's: one alert means "act now" and the
    * other "have a look", so muting either must not mute the other. With
    * both off there is nothing to emit and no reason to cross into Java at
    * all -- the latch has already been committed by the caller either way,
    * so the crossing stays announced and will not fire again on the next
    * tick. */
   if (!g_nudge_sound && !g_nudge_vib)
      return;
   LOGI("nudge %s (sound=%d vib=%d)", ng == NG_LOW ? "low" : "high",
        g_nudge_sound, g_nudge_vib);
   dexble_nudge(ng == NG_LOW ? 0 : 1, g_nudge_sound, g_nudge_vib);
}

/* Gather the alarm zone, the stranded flag and the NUDGE zone across every
 * live CGM from ONE snapshot of the history. The nudge rides along rather
 * than gathering again: two passes could see different samples, and a nudge
 * evaluated against a reading the alarm never saw is a nudge that can fire
 * underneath its own alarm -- exactly what nudge_fire exists to prevent.
 * `*nzone` comes back -1 when no live CGM has a current reading. */
static void alarm_gather(long now, int *zone, int *stranded, int *nzone)
{
   struct {
      int glu;
      long t;
   } smp[MAX_SLOTS];

   int ids[MAX_SLOTS];
   int nids = 0;
   sensors_lock();
   for (int i = 0; i < g_nslot && nids < MAX_SLOTS; i++) {
      if (g_slot[i].old) /* disconnected: not part of the live alarm set */
         continue;
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && sensor_kind(r->type) == KIND_CGM)
         ids[nids++] = g_slot[i].id;
   }
   sensors_unlock();
   int ns = 0;
   hist_lock();
   for (int i = 0; i < nids; i++)
      for (int k = 0; k < g_nhist; k++)
         if (g_hist[k].src == (unsigned short)ids[i] &&
             g_hist[k].kind != KIND_BGM) {
            smp[ns].glu = g_hist[k].glu;
            smp[ns].t   = g_hist[k].t;
            ns++;
            break;
         }
   if (nids == 0) { /* pre-registry fallback: judge the current reading */
      smp[0].glu = g_cur_glu;
      smp[0].t   = g_cur_time;
      ns         = 1;
   }
   hist_unlock();
   *zone     = 0;
   *stranded = 0;
   *nzone    = -1;
   for (int i = 0; i < ns; i++) {
      *zone = alarm_zone_merge(*zone, alarm_zone(smp[i].glu, smp[i].t, now,
                                                 g_alarm_low, g_alarm_high));
      if (alarm_stranded(smp[i].glu, smp[i].t, now, g_alarm_low, g_alarm_high))
         *stranded = 1;
      /* Merged the same way the alarm zone is -- the worst band on ANY worn
       * CGM wins, and a LOW outranks a HIGH. A sensor with no current
       * reading contributes nothing rather than voting "in range", which
       * would clear the latch on a dropout and re-arm the nudge to fire
       * again. */
      int nz = nudge_zone(smp[i].glu, smp[i].t, now, g_nudge_low, g_nudge_high);
      if (nz >= 0)
         *nzone = (*nzone < 0) ? nz : alarm_zone_merge(*nzone, nz);
   }
}

static void alarm_reeval(void)
{
   long now     = realtime_s();
   int zone     = 0;
   int stranded = 0;
   int nzone    = -1;
   alarm_gather(now, &zone, &stranded, &nzone); /* BEFORE alarm_lock */
   alarm_lock();
   g_alarm_state = zone;
   int pred      = any_pred_low();
   int ng        = nudge_fire(nzone, alarming(zone, pred), g_nudge_state);
   g_nudge_state = nudge_next(nzone, g_nudge_state);
   alarm_apply_ex(g_alarm_state, g_disc_alarmed, stranded, pred);
   alarm_unlock();
   nudge_emit(ng);
}

/* Re-issue the CURRENT alarm level to Java, even though the level has not
 * changed.
 *
 * alarm_apply is idempotent on the level -- re-asserting the same one must
 * not re-chime -- but that early return also swallowed a change to whether
 * the level is PERCEPTIBLE. With SOUND and VIBRATION both off, a low reading
 * commits g_alarm_want = AL_LOW while nothing sounds; turning SOUND on then
 * left want unchanged, so dexble_alarm() was never called again and the hypo
 * stayed silent for its entire duration, becoming audible only if glucose
 * returned to range and re-crossed. Clearing g_alarm_want forces the next
 * evaluation to treat the level as new. The threshold entry (kp_mode 10-13)
 * already accepts that a settings change can change alarm state; this is the
 * same for the audible settings. */
static void alarm_reactuate(void)
{
   /* Clear AND re-evaluate under ONE hold. Releasing between them left a
    * window in which a binder thread or the service tick could run
    * pancra_alarm_check, re-commit the same g_alarm_want, and make the
    * re-evaluation early-return on `want == g_alarm_want` -- silently losing
    * the re-actuation, which is precisely the failure this function exists
    * to prevent. alarm_lock is recursive, so nesting is free.
    *
    * No current_reading() here: re-announcing the level we already hold
    * needs no fresh sample, so this function no longer touches hist_lock at
    * all -- which also removes the hist-under-alarm nesting it once had. */
   alarm_lock();
   /* RE-ANNOUNCE THE COMMITTED LEVEL. Do not clear it and recompute.
    *
    * Clearing g_alarm_want and re-evaluating looks equivalent and is not:
    * the sustain rule is keyed on the PREVIOUS level, so zeroing it deletes
    * the only thing keeping a stranded hypo alive, and the recompute then
    * yields AL_NONE. Three failures follow from that one line -- a 45 mg/dL
    * alarm that can never be made audible, a tone that keeps playing after
    * the user switches SOUND off, and no silence when glucose recovers,
    * because want == prev_want makes every later evaluation a no-op.
    * Re-issuing the level we already hold has no such dependency. */
   if (alarm_reactuate_allowed(g_alarm_acked) && g_alarm_want != AL_NONE) {
      int kind         = alarm_java_kind(g_alarm_want);
      int was_sounding = g_alarm_sounding;
      g_alarm_sounding = alarm_audible(g_alarm_want, g_sound_on, g_vib_on);
      if (kind >= 0 && !dexble_alarm(kind, g_sound_on, g_vib_on)) {
         LOGI("alarm: re-actuation failed; leaving the level committed");
         g_alarm_sounding = was_sounding;
      }
   }
   alarm_unlock();
}

/* Stale-data ("DISCONNECT") alarm: fire when the newest reading is older
 * than the chosen threshold. A freshly opened app gets a grace period equal
 * to the threshold (data may be stale until the first sync). Evaluated on
 * the 1 Hz timer because it's the ABSENCE of new data that triggers it. */
/* Evaluate and actuate the alarm. Safe to call from ANY thread, and
 * deliberately callable with no activity alive.
 *
 * This must NOT be called with driver_lock held: it can block for hundreds
 * of milliseconds inside Alarm.trigger (MediaPlayer prepare), and
 * driver_lock is a no-timeout spin lock that the main looper also takes. The
 * BLE transport therefore calls this AFTER releasing driver_lock, not from
 * inside the notify dispatch. */
/* Stranded-link watchdog. Callable from ANY thread, and deliberately
 * callable with no activity alive.
 *
 * TWO defects, both of which ended in silent, indefinite loss of monitoring:
 *
 * 1. It lived inline in on_timer, i.e. on the ACTIVITY's looper, which
 *    on_destroy tears down. A back-press or task-swipe is a documented,
 *    supported mode -- the foreground service deliberately keeps the BLE
 *    connection alive for days afterwards -- but from that moment nothing
 *    repaired a stranded link: this watchdog needed on_timer, and the
 *    advert-driven reconnect needed a scan that on_pause had already
 * stopped. The link died and stayed dead, and with the DISCONNECT alarm
 * defaulting to OFF there was no alarm of any kind to say so. The service
 * heartbeat was added so ALARMS would survive the activity; the self-heal
 * that keeps DATA arriving was left behind. It now runs from both.
 *
 * 2. It only ever looked at LINK_CGM, and judged staleness by g_cur_time --
 *    the newest sample across all sources, which hist_refresh_current binds
 * to the PRIMARY sensor. With two CGMs a healthy primary kept `age` under
 * the threshold forever, so no link was ever kicked, including LINK_CGM's
 * own. That is the same trap jni_on_advert documents and fixed for
 * reconnects. Staleness is now judged per link, from that link's OWN newest
 * reading. */
/* Meter-sync watchdog. If the link drops mid-sync or the connect never
 * lands, g_meter_busy latches and the meter never syncs again --
 * jni_on_advert gates every sync on !g_meter_busy, and dexble_link_close is
 * never reached so the GATT client stays open too.
 *
 * Called from the service tick as well as the 1 Hz timer. It used to live
 * inside sensor_reconcile, whose only caller is on_timer -- the ACTIVITY's
 * looper, which on_destroy tears down. So a sync in flight when the user
 * back-pressed or swiped the task away left the meter wedged for the whole
 * background lifetime, which is exactly the window the service exists to
 * cover, and it self-healed only when the activity was reopened. */

/* Runs on the 1 Hz tick (and the service heartbeat, so it survives the
 * activity being destroyed). Two jobs:
 *
 *   - release a sync that has WEDGED. Now that g_meter_busy is taken only
 * when the meter answers, this 90 s measures a real exchange rather than a
 *     standing connect's wait, so it can no longer tear down a pending
 * connect that is behaving exactly as intended.
 *   - keep exactly one standing connect ARMED. This is what makes a sync
 *     survive a restart, a Bluetooth toggle, or the app being swiped away:
 *     nothing else re-establishes it, and without it the first fingerstick
 *     after any of those would be missed with no way for the user to know.
 */
static void meter_sync_watchdog_locked(void)
{
   if (g_meter_busy && realtime_s() - g_meter_start > 90) {
      LOGI("meter sync timed out; releasing link %d", g_meter_link);
      if (g_meter_link >= 0)
         dexble_link_close(g_meter_link);
      /* Un-arm only, for the same reason as ot_drv_disconnect: the close is
       * in flight and its callback still has to route to otble. */
      meter_unarm_link(g_meter_link);
      g_meter_busy = 0;
      g_meter_link = -1;
   }
   /* RECOVER A LINK STRANDED WAITING FOR A TEARDOWN.
    *
    * ot_drv_disconnect asks for the close and leaves the link ARMED, because
    * un-arming there is what let the tick reconnect a still-awake meter and
    * re-run the exchange once a second. The release then depends entirely on
    * the GATT disconnect callback arriving -- and nothing else covers this
    * state: the timeout above needs g_meter_busy, which is clear by now, and
    * pancra_link_watchdog skips meter links. A callback that never lands
    * would strand the link armed for the life of the process, and that meter
    * would never sync again.
    *
    * The bound is generous on purpose. A real teardown is either immediate
    * (an app-initiated close: 0.3 s in an HCI capture) or the meter powering
    * itself off (a supervision timeout ~35 s later). Three minutes is far
    * past both, so this only ever fires on a callback that is genuinely
    * lost, and re-arming costs one connect. */
   long now = realtime_s();
   for (int l = 0; l < LINK_MAX; l++) {
      if (!g_link_idle_t[l] || g_meter_busy)
         continue;
      if (now - g_link_idle_t[l] < METER_TEARDOWN_MAX)
         continue;
      LOGI("meter link %d stranded waiting for a disconnect; releasing", l);
      meter_release_link(l); /* clears g_link_idle_t[l] too */
   }
   /* ARM EVERY REGISTERED METER, not just one.
    *
    * Each holds its own link, so all of them can wait on the controller at
    * once -- which is what makes "whichever meter I pick up" work rather
    * than only the last one used. Arming is idempotent (meter_armed), so
    * this is a no-op on every tick but the first after a restart or a
    * finished sync. */
   int ids[MAX_SLOTS];
   char macs[MAX_SLOTS][24];
   int n = 0;
   sensors_lock();
   for (int i = 0; i < g_nslot && n < MAX_SLOTS; i++) {
      if (g_slot[i].old)
         continue; /* retired: holds no link */
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (!r || sensor_kind(r->type) != KIND_BGM)
         continue;
      ids[n] = g_slot[i].id;
      str_snapshot(macs[n], sizeof macs[n], r->identity);
      n++;
   }
   sensors_unlock();
   now = realtime_s(); /* re-read: the block above can take a moment */
   for (int i = 0; i < n; i++) {
      if (!macs[i][0] || meter_armed(macs[i]))
         continue;
      /* COOLDOWN after a finished exchange.
       *
       * Second line of defence for the re-sync loop fixed in
       * ot_drv_disconnect: that one keeps the link armed until the meter
       * really goes away, which is enough on its own, but a link that flaps
       * -- drops and is immediately reachable again -- would otherwise walk
       * straight back into a sync-per-second. A meter that has genuinely
       * powered off does not come back within seconds, so nothing legitimate
       * waits on this. */
      struct meter_rt *rt = meter_rt_get(ids[i], 0);
      if (rt && rt->synced_t && now - rt->synced_t < METER_REARM_COOLDOWN)
         continue;
      /* One per tick: meter_sync_start takes both locks and issues a GATT
       * connect, and doing several in one pass would hold the main thread
       * across a burst of binder calls for no gain -- the meters are almost
       * certainly all switched off anyway. */
      meter_sync_start(ids[i], macs[i]);
      return;
   }
}

/* SELF-GUARDED. Two threads run this: the activity's 1 Hz timer reaches it
 * through sensor_reconcile (which holds g_reconcile_busy) and the service
 * tick calls it DIRECTLY, outside that guard -- so the serialisation the
 * reconcile path relies on did not actually cover both callers. Interleaving
 * could arm one meter on two links or bump a connect generation mid-connect;
 * the refusal path in pancra_meter_connected cleans up after it, so the cost
 * was transient battery and latency rather than lost data, but a function
 * whose safety depends on which caller you came from is one edit away from
 * being wrong. Guarding here covers every caller, present and future.
 * Skipping a tick is free -- the next one is a second away. */
void meter_sync_watchdog(void)
{
   static volatile int busy;
   if (__atomic_exchange_n(&busy, 1, __ATOMIC_SEQ_CST))
      return;
   meter_sync_watchdog_locked();
   __atomic_store_n(&busy, 0, __ATOMIC_SEQ_CST);
}

void pancra_link_watchdog(void)
{
   static long last_kick[LINK_MAX];
   long now = realtime_s();
   for (int l = 0; l < LINK_MAX; l++) {
      if (g_link_meter[l])
         continue; /* CGMs only */
      struct dex_session s;
      driver_lock();
      driver_select(l);
      driver_get_session(&s);
      driver_select(LINK_CGM);
      driver_unlock();
      if (!s.paired || !s.mac[0])
         continue;
      /* This link's own newest sample, not the global one. */
      int src   = src_for_link(l);
      long mine = 0;
      hist_lock();
      for (int k = 0; k < g_nhist && !mine; k++)
         if (src >= 0 && g_hist[k].src == (unsigned short)src &&
             g_hist[k].kind != KIND_BGM)
            mine = g_hist[k].t;
      hist_unlock();
      long age = mine ? now - mine : now - g_launch_t;
      /* CLAIM the throttle atomically -- only one thread may kick.
       *
       * This runs on BOTH the activity's 1 Hz timer and the service's 20 s
       * tick, and last_kick was a plain read-modify-write shared between
       * them. Both could see the interval elapsed and both call
       * dexble_reconnect on the same link: the second Ble.connect bumps the
       * link generation, so the first closes the client it had just created,
       * tearing down the very reconnect this watchdog exists to start and
       * leaving the link down for another cycle. A compare-exchange makes
       * the claim single-winner. */
      /* One missed 5-min cycle plus two minutes of slack. This is the ONLY
       * reconnect mechanism while the screen is off (the advert path needs
       * the scan, whose lifecycle follows on_resume/on_pause), so at the old
       * 700 s a link dropped with the screen dark stayed silent for TWO
       * cycles -- observed as a 13-minute gap after an app restart -- when
       * one direct connect by saved MAC heals it. Backfill recovers the data
       * either way; this recovers the latency. */
      if (age > 420) {
         /* Atomic exchange, so exactly one thread wins the throttle. Whoever
          * swaps in `now` reads the PREVIOUS stamp; only the one that finds
          * it genuinely stale kicks, and the loser reads the winner's `now`
          * and stands down. (An exchange rather than a compare-exchange
          * because the latter's `weak` parameter is a _Bool this
          * freestanding build has no stdbool.h to spell.) */
         long prev = __atomic_exchange_n(&last_kick[l], now, __ATOMIC_SEQ_CST);
         if (now - prev > 300) {
            LOGI("watchdog: link %d %ld s since its last reading -> "
                 "reconnect",
                 l, age);
            dexble_reconnect(l);
         }
      }
   }
}

void pancra_alarm_check(void)
{
   /* The DISCONNECT/stale alarm stays bound to the CURRENT reading -- the
    * primary's, i.e. the number the user is watching going stale is the fact
    * it announces. The excursion zone and the stranded sustain are gathered
    * across EVERY CGM (alarm_gather): a low on the non-primary sensor rings
    * too. All the reads happen BEFORE alarm_lock (lock-order: hist never
    * nests inside alarm). */
   struct alarm_reading cur = current_reading();
   long now                 = realtime_s();
   int zone                 = 0;
   int stranded             = 0;
   int nzone                = -1;
   alarm_gather(now, &zone, &stranded, &nzone);
   alarm_lock();
   g_alarm_state = zone;
   /* The nudge is decided under the SAME hold as the alarm, so the two see
    * one consistent zone pair and only one thread can claim a given crossing
    * -- pancra_alarm_check runs on the main looper, the service tick AND a
    * GATT binder thread. The SOUND is emitted after the unlock: it is a JNI
    * call that synthesises and plays audio, and alarm_lock is a spin lock
    * the main looper takes. Nothing depends on when it lands, unlike the
    * glucose alarm whose actuation must stay inside the lock (see
    * alarm_apply_ex). */
   int pred      = any_pred_low();
   int ng        = nudge_fire(nzone, alarming(zone, pred), g_nudge_state);
   g_nudge_state = nudge_next(nzone, g_nudge_state);
   /* Either the user's configured DISCONNECT threshold, or -- regardless of
    * that setting -- data going stale while the last reading was out of
    * range. Without the second term a ringing hypo alarm was silenced after
    * AL_FRESH_S of dropout in the DEFAULT configuration. See alarm_stranded.
    */
   g_disc_alarmed = alarm_stale(cur.glu, cur.t, now, g_launch_t,
                                (long)disc_min[(unsigned)g_disc & 3U] * 60);
   /* Stranded is passed SEPARATELY, not folded into g_disc_alarmed: it may
    * only sustain an alarm that is already sounding, never originate one and
    * never relabel it. See alarm_want_sustained. Folding it in made a stale
    * low mint a fresh "Sensor disconnected" -- including one second after a
    * cold start, off a reading store_load had just restored from the log. */
   alarm_apply_ex(g_alarm_state, g_disc_alarmed, stranded, pred);
   alarm_unlock();
   nudge_emit(ng);
}

static void disc_reeval(void)
{
   /* One implementation, shared with the transport and the service
    * heartbeat. The zone is recomputed here so it DECAYS with time -- this
    * path runs when no readings are arriving, which is exactly when a stale
    * zone would otherwise mask the disconnect alarm. */
   pancra_alarm_check();
}

/* --- hooks called by the BLE driver (dexble.c) --- */
void pancra_status(const char *s)
{
   set_status(s);
}

/* REMOTE push: hand one just-stored datapoint (timestamp + glucose) to
 * Ble.remotePush, which only ENQUEUES onto its own background thread and
 * returns -- so this is safe on the reading paths, which run on BLE binder
 * threads with driver_lock held (same rule that keeps the alarm off these
 * threads). dexble_env(), not g_act->env: a JNIEnv is only valid on its own
 * thread, and these calls arrive on binder threads and the service tick. */
/* Java -> C: Ble.remotePush got a 2xx from the server (called on its push
 * worker thread, via dexble.c's registered onRemoteOk). */
void pancra_remote_ok(void)
{
   g_remote_last_ok = realtime_s();
}

/* The app's own log is the outbox now.
 *
 * There used to be one here: a byte offset into readings.csv that the
 * server had confirmed, a second one for the live tail, an in-flight
 * marker for each, and the same again for doses -- all of it bookkeeping
 * about what the server already had. The replica protocol asks the server
 * that question directly and gets an exact answer, so the only thing still
 * worth knowing locally is whether the log has GROWN since the last look,
 * which is one number. */
/* (ob_log_size is gone: syncjni_state_stamp covers every synced file, not
 * just the readings log, and that is what decides whether to sync.) */

/* (The PULL direction is gone. It existed because the server held far more
 * history than the phone and the phone wanted it back; under the replica
 * protocol the server holds exactly what the phone gave it, so there is
 * nothing there to import that is not already here.) */

/* WHEN TO SYNC AT ALL.
 *
 * Only when something changed. This used to fire every ten seconds forever:
 * with nothing to send it still cost a TLS handshake, a request to the
 * server, and a full local pass over every log to hash it -- several times a
 * minute, on a phone battery, against a single-core board. That is not a
 * sync, it is a slow denial of service against your own server.
 *
 * So: the sizes of the synced files are the trigger. A reading, a dose, an
 * edit or a deletion changes one of them; nothing else does. On top of that
 * there is a rare safety net, because the server could have been restored
 * from an older backup while the phone had nothing new to say, and a
 * BACKOFF, because a failing sync must keep retrying without becoming the
 * ten-second loop again. */
#define REMOTE_MIN_GAP  60L          /* never more often than this */
#define REMOTE_SAFETY   (6L * 3600)  /* re-check even with nothing new */
#define REMOTE_FAIL_MIN 60L          /* first retry after a failure */
#define REMOTE_FAIL_MAX (30L * 60)   /* ...doubling up to here */

static long g_rem_next;    /* earliest next attempt */
static long g_rem_seen;    /* the state stamp when we last synced */
static long g_rem_backoff; /* current failure backoff, seconds */
static long g_rem_safety;  /* when to look again with nothing new */

static int g_sync_busy; /* single-flight guard; see pancra_remote_sync */
static void remote_sync_locked(void);

/* SINGLE-FLIGHT, because this runs on TWO threads.
 *
 * The activity's 1 Hz on_timer and the service's tick thread both drive the
 * sync, and the Java-side sBusy flag does not serialise them: remoteBusy()
 * is polled at the top while sBusy is not set until remoteBatch/remoteRange
 * is actually entered, and the whole batch build -- three open/lseek/read
 * cycles into one SHARED static body[] -- sits in between. Two threads
 * inside that window fill the same buffer from different file offsets, so
 * the POST can carry a spliced line: a fabricated <epoch> <mg/dL> pair
 * stored as a real reading, after which the acknowledged offset advances
 * past data that was never correctly sent. Every other function reachable
 * from both callers already has such a guard (g_notify_busy,
 * g_reconcile_busy, alarm_lock, the atomic on last_kick); this one was the
 * exception.
 *
 * Skipping rather than waiting is right: the loser has nothing to
 * contribute, and the next tick is at most a minute away. */
void pancra_remote_sync(void)
{
   if (__atomic_exchange_n(&g_sync_busy, 1, __ATOMIC_SEQ_CST))
      return;
   remote_sync_locked();
   __atomic_store_n(&g_sync_busy, 0, __ATOMIC_SEQ_CST);
}

static void remote_sync_locked(void)
{
   /* Ask JAVA to run a sync on its worker; the protocol itself is in sync.c.
    *
    * This used to BE the protocol -- cursors, outbox positions, per-set
    * batches, acknowledgement tags. All of that existed to make a
    * fire-and-forget push lossless. The replica protocol does not need any of
    * it: the phone and the server compare hashes and the phone pushes whole
    * buckets, so "what has the server already got" is answered by the server,
    * exactly, every time, instead of being tracked here and hoped for. */
   if (!g_remote_on || !g_remote_server[0] || !g_sync_uid)
      return;
   long now  = realtime_s();
   long sz   = syncjni_state_stamp();
   int fresh = (sz != g_rem_seen);
   if (now < g_rem_next)
      return; /* too soon whatever happened: the floor is absolute */
   if (!fresh && now < g_rem_safety)
      return; /* nothing new, and the safety net is not due */
   g_rem_seen   = sz;
   g_rem_next   = now + REMOTE_MIN_GAP;
   g_rem_safety = now + REMOTE_SAFETY;
   syncjni_sync_request();
}


/* The configured server changed: the cursors describe the OLD one, so drop
 * them rather than measure this server's history against another's. */
/* Called from the sync worker thread (see syncjni.h). Only touches values the
 * renderer reads whole, and sets the dirty flag last. */
void sync_report(int ok, const char *what)
{
   long now = realtime_s();
   if (ok) {
      g_remote_last_ok = now;
      g_rem_backoff    = 0;
   } else {
      /* Retry, but slower each time: a server that is down must not be asked
       * once a minute for ever. */
      g_rem_backoff = g_rem_backoff ? g_rem_backoff * 2 : REMOTE_FAIL_MIN;
      if (g_rem_backoff > REMOTE_FAIL_MAX)
         g_rem_backoff = REMOTE_FAIL_MAX;
      g_rem_next = now + g_rem_backoff;
      /* A failed sync has NOT caught up, so the next attempt must not be
       * skipped for lack of new data. */
      g_rem_seen = -1;
   }
   str_snapshot(g_remote_status, sizeof g_remote_status, what);
   g_ui_dirty = 1;
}

/* The configured server changed. There is no cursor to forget any more --
 * the next sync asks the NEW server what it has and finds out exactly -- but
 * the paired identity belongs to the old one and must not be offered to
 * another server. */
static void remote_forget_cursor(void)
{
   g_sync_uid = 0;
   for (int i = 0; i < 16; i++)
      g_sync_key[i] = 0;
   remote_save();
   sync_set_key(0, g_sync_key);
}

/* (Nothing pushes a single datapoint. Everything the phone holds reaches the
 * server as whole buckets whose hashes are compared first, so there is no
 * per-point path that could drop one.) */

/* Is this a usable glucose value?
 *
 * The 12-bit field carries 0..4095 verbatim, and a sentinel (0 during
 * warm-up or a sensor-error state) would otherwise become g_cur_glu -- the
 * headline number, with a trend arrow, feeding the alarm and firing a
 * spurious LOW. It would also be written to the permanent log and into the
 * stats. The meter path already bounds its values; this is the equivalent
 * for the sensor.
 *
 * The bound matches what a CGM can physically report (Dexcom reads 40..400
 * and clamps there), widened slightly so a genuine extreme is never
 * discarded. */
static int glucose_plausible(int mg_dl)
{
   return mg_dl >= 20 && mg_dl <= 600;
}

/* current reading from the 4e stream */
void pancra_glucose(int mg_dl, int trend, int age_s)
{
   g_where = "pancra_glucose";
   if (!glucose_plausible(mg_dl)) {
      LOGI("glucose %d mg/dL implausible, ignored", mg_dl);
      return;
   }
   /* BOUND THE AGE, not just the value.
    *
    * age comes straight off the wire as a full uint16 (dexdata.c:
    * le16(p+10)), and only the glucose was gated here. A frame carrying
    * age=65535 backdates the reading 18.2 hours: it enters g_hist, skews
    * stat_add's TIR and average, and is written to readings.csv -- a file
    * that is never rewritten and whose only load-time timestamp guard is t >
    * 0, so it is re-admitted on every restart. It also poisons the
    * per-sensor gap sizing below, provoking repeated 24 h backfill requests.
    *
    * A live 0x4e reading is seconds to a few minutes old (the cycle is ~5
    * min); 15 minutes is generous. Reject rather than clamp -- clamping
    * would stamp a genuinely stale reading as current, which for a glucose
    * display is the more dangerous of the two errors. The meter path has
    * bounded its timestamp all along (otble.c); this is the equivalent the
    * Dexcom path never had. */
   if (age_s < 0 || age_s > 900) {
      LOGI("glucose age %d s implausible, ignored", age_s);
      return;
   }
   long t  = realtime_s() - age_s;
   int has = (g_conn_rssi_t &&
              realtime_s() - g_conn_rssi_t < 120); /* this connection */
   /* Mutate the shared history / current-reading state under the same guard
    * the renderer holds (hist_history_lock), so a main-thread draw never
    * reads a half-shifted g_hist, torn stats, or a mismatched
    * g_cur_glu/g_cur_time. */
   /* Resolve the source BEFORE taking hist_lock: this reads the driver and
    * the registry, and the established lock order is driver -> reg -> hist.
    */
   int src = src_for_link(driver_link());
   if (src < 0) {
      /* No slot claims this link's address yet -- registration happens on
       * the next 1 Hz reconcile, and requires have_reading, so the FIRST
       * reading of any new sensor always lands here. g_cur_src belongs to
       * some ALREADY REGISTERED sensor, so borrowing it is safe only when
       * none exists (a fresh install, where g_cur_src is 0 = pre-registry
       * legacy and therefore unambiguous). The threshold is >= 1, not > 1:
       * with one sensor already registered, a second sensor's first reading
       * was written to the append-only log carrying the FIRST sensor's id --
       * and if that sensor had reported within 150 s, hist_insert deduped it
       * away entirely. Drop instead: the next sample arrives in ~5 minutes,
       * by which time the sensor is registered. A wrong attribution is
       * permanent; a missed sample is not. */
      if (cgm_slot_count() >= 1) {
         LOGI("glucose %d mg/dL from an unregistered link, deferred", mg_dl);
         return;
      }
      src = g_cur_src;
   }
   /* RESCALE. Record the RAW value (for computing a future factor), then
    * apply the active correction IF it belongs to this sensor and this
    * reading's timestamp is at/after activation. From here mg_dl is the
    * rescaled value, so history, stats, the alarm and the log all use it;
    * the factor rpm is written to the log's `rescale` column so the raw
    * stays recoverable. */
   {
      int lk = driver_link();
      if (lk >= 0 && lk < LINK_MAX)
         g_link_raw[lk] = mg_dl;
   }
   /* A rescale target was waiting for a reading to compute its factor: this
    * is that reading -- BUT only if it is at most RESCALE_PEND_WINDOW_S
    * newer than the request. Past that the fingerstick reference is stale,
    * so EXPIRE it (visibly) rather than applying it to a much-later reading.
    * Otherwise activate from THIS raw, effective from THIS timestamp, so the
    * reference reading itself shows the entered value. */
   /* Set when a rescale takes effect ON this reading: from here on mg_dl is
    * rescaled while the history is not, so any change computed against the
    * history is the calibration step, not the wearer's glucose moving. The
    * CHIRP below is the only consumer. */
   int rescale_started = 0;
   if (g_rescale_pend_mgdl > 0 && g_rescale_pend_id == src) {
      if (t - g_rescale_pend_t > RESCALE_PEND_WINDOW_S) {
         LOGI("pending rescale %d mg/dL EXPIRED (reading %ld s after "
              "request)",
              g_rescale_pend_mgdl, t - g_rescale_pend_t);
         g_rescale_expired    = 1;
         g_rescale_expired_id = src;
      } else {
         rescale_activate(src, g_rescale_pend_mgdl, mg_dl, t);
         rescale_started = 1;
      }
      g_rescale_pend_mgdl = 0;
      g_rescale_pend_id   = 0;
      g_rescale_pend_t    = 0;
      rescale_save();
   }
   int rpm = rescale_pm_for(src, t);
   if (rpm != 1000)
      mg_dl = rescale_apply(mg_dl, rpm);
   int prime = sensor_primary_id();
   /* WARMUP readings are shown but never COUNTED -- uncalibrated values skew
    * TIR and the average (see sensors.h). Resolved HERE, before hist_lock,
    * because the lock order is registry -> history. */
   int warm = sensor_in_warmup(src, t);
   hist_lock();
   /* THIS sensor's previous value, captured BEFORE the insert (afterwards
    * the newest sample from src is the one being added) and under the same
    * lock that protects g_hist. Feeds the CHIRP pitch below; -1 means this
    * source has no earlier sample, which chirps at the plain BEEP pitch. */
   int prev_glu = hist_prev_glu(t, src, t - CHIRP_MAX_GAP_S);
   int isnew    = hist_insert(t, mg_dl, trend, src, KIND_CGM);
   if (isnew) {
      /* Any non-zero result, matching what store_append persists below --
       * see store.h. Gating on HIST_NEW alone made TIR and the average
       * change across a restart. */
      if (!warm)
         stat_add(t, mg_dl);
      if (has) {
         g_cur_rssi    = g_conn_rssi;
         g_cur_rssi_ok = 1;
      } /* keep it like glu/trend */
   }
   hist_refresh_current(prime);
   hist_unlock();
   /* Capture THIS CGM link's predicted value + time for the imminent-hypo
    * (predicted-low) alarm. jni_notify holds driver_lock across this
    * dispatch and has selected this link, so the session read is safe and
    * refers to it. Stamped every reading (not just new ones) so the
    * freshness gate in any_pred_low() tracks the live prediction. */
   {
      int lk = driver_link();
      if (lk >= 0 && lk < LINK_MAX && !g_link_meter[lk]) {
         struct dex_session ps;
         driver_get_session(&ps);
         g_link_pred[lk]   = ps.predicted;
         g_link_pred_t[lk] = realtime_s();
      }
   }
   /* A reading just PROVED this sensor is streaming -- the ideal moment to
    * flush any queued calibration for it. driver_lock is held (jni_notify)
    * and the driver is selected to this link, exactly what calq_try_locked
    * needs.
    */
   if (g_calq_mgdl > 0 && src == g_calq_id)
      calq_try_locked();
   /* Persist on HIST_OLD as well: the log is the lifetime record and NHIST
    * is only how much of it fits on screen. File I/O outside the lock -- it
    * touches no draw-shared state. */
   if (isnew) {
      store_append(t, mg_dl, trend, g_conn_rssi, has, src, t, g_tz_off,
                   KIND_CGM, rpm);
      /* No per-datapoint push any more: remote_sync_step() on the tick
       * delivers this point (and anything a previous outage lost) from the
       * server's own cursor. */
   }
   LOGI("glucose %d mg/dL trend %d age %d", mg_dl, trend, age_s);

   /* NEW DATAPOINT alert: a genuinely new sample from the PRIMARY CGM only,
    * so a secondary sensor or a backfilled/duplicate reading stays silent.
    *
    * CHIRP pitches on the change since THIS sensor's own previous sample.
    * Never against another CGM's: with two sensors worn at once their
    * difference is the calibration offset between two devices, and pitching
    * on it would announce swings the wearer never had. With no previous
    * sample of its own (a fresh session) the delta is 0, which is exactly
    * the BEEP tone -- the honest sound for "no change known yet". */
   if (isnew && src == prime) {
      if (g_newdata_mode == ND_BEEP) {
         dexble_beep();
      } else if (g_newdata_mode == ND_CHIRP) {
         /* A rescale that ACTIVATED on this very reading makes the
          * comparison meaningless: mg_dl is already rescaled while every
          * sample already in g_hist is not, so the "change" would be the
          * calibration offset itself -- entering a 120 fingerstick against a
          * sensor reading 100 would chirp a full-cap rocket that never
          * happened. Treat it as having no predecessor, which is the plain
          * BEEP pitch. */
         int delta = (prev_glu >= 0 && !rescale_started) ? mg_dl - prev_glu : 0;
         dexble_chirp(chirp_semitone10(delta));
      }
   }

   /* Alarm evaluation is deliberately NOT done here.
    *
    * This runs on a BLE binder thread with driver_lock held (jni_notify
    * holds it across the whole notify dispatch), and raising an alarm calls
    * Alarm.trigger -> RingtoneManager + MediaPlayer.setDataSource/prepare/
    * start: media-server IPC, routinely hundreds of milliseconds. Holding a
    * SPIN lock across that makes the main looper burn a core in sched_yield
    * on its next snap_drivers() or watchdog tick.
    *
    * disc_reeval() on the 1 Hz main-thread timer already recomputes the zone
    * from a consistent current_reading() and calls alarm_apply, so the alarm
    * is raised within one second regardless -- immaterial against a 5-minute
    * sample interval. Leaving alarm_apply to a single thread also removes
    * the raise/silence ordering races entirely: every caller is now the main
    * thread. */
   /* Rendering is deferred to the main-thread 1 Hz timer (see on_main); just
    * mark the screen and notification dirty. */
   g_ui_dirty     = 1;
   g_notify_dirty = 1;

   /* Read the sensor's serial / firmware / software strings. Deferred to
    * here (after the first reading) so it runs post-auth, when the reads
    * succeed. The sensor closes the cycle within a few seconds, often before
    * all three reads land, so we retry each reconnect until we have them all
    * -- throttled to at most once a minute, and stopping entirely once
    * complete. */
   /* Gate on THIS LINK's strings, not the process-global ones. Keying on the
    * globals meant that once any sensor had filled them -- and they persist
    * to disk -- DIS was never re-read for any sensor again, so every later
    * sensor was minted carrying the first one's model and firmware. */
   int dlink = driver_link();
   if (dlink >= 0 && dlink < LINK_MAX &&
       (!g_model_l[dlink][0] || !g_fw_l[dlink][0] || !g_mfr[0]) &&
       realtime_s() - g_devinfo_req[dlink] > 60) {
      g_devinfo_req[dlink] = realtime_s();
      dexble_request_devinfo();
   }

   int did_bf = 0;
   /* Backward fill, at most once per launch: pull history back to the start
    * of the available window = min(24h, session age). Gating on session
    * duration means a young session stops re-requesting once we hold its
    * whole span, instead of forever chasing a 24h it can never reach. */
   int bflink = driver_link();
   if (bflink < 0 || bflink >= LINK_MAX)
      bflink = LINK_CGM;
   if (!g_startup_bf[bflink]) {
      g_startup_bf[bflink] = 1;
      struct dex_session s;
      driver_lock();
      driver_get_session(&s);
      driver_unlock();
      long target = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds < target)
         target = (long)s.session_seconds;
      /* Oldest sample FROM THIS SENSOR. g_hist[g_nhist-1] is the oldest
       * across all sources, so after store_load restored a week of merged
       * history this test was always false and the once-per-launch fill
       * never ran at all -- while g_startup_bf had already been latched. */
      long oldest = t;
      hist_lock();
      for (int i = g_nhist - 1; i >= 0; i--)
         if (g_hist[i].src == (unsigned short)src &&
             g_hist[i].kind != KIND_BGM) {
            oldest = g_hist[i].t;
            break;
         }
      hist_unlock();
      if (target > 600 && realtime_s() - oldest < target - 300) {
         LOGI("backward fill: %ld s backfill (have %ld s of %ld s window)",
              target, realtime_s() - oldest, target);
         driver_lock(); /* recursive: normally already held by the callback
                         */
         driver_request_backfill(target);
         driver_unlock();
         did_bf = 1;
      }
   }
   /* Ongoing: recover ANY interior hole in this sensor's recent buffer
    * window, retried until it is filled.
    *
    * The old rule keyed the gap off `prev` (the newest reading before this
    * one) and fired at most once per gap. That stranded interior holes: the
    * moment a single reading lands past a gap -- which the very next
    * reconnect after an outage delivers -- `prev` advances past it, every
    * later span is a normal 5-minute step, and the missing block is never
    * requested again. The once-per-launch backward fill above does not save
    * it either: it is gated off whenever we already span the window. A
    * ~15-minute reinstall gap was lost permanently this way even though the
    * sensor still held the records.
    *
    * Instead: scan this sensor's samples across the sensor's buffer window
    * (min 24h, session age) for the OLDEST >450 s hole, and request backfill
    * covering from there to now. Delivered records dedupe on insert, so a
    * wide re-request is harmless; the request is throttled per link and,
    * because it is driven by the presence of a hole rather than a one-shot
    * event, it simply repeats each cycle -- shrinking the span as records
    * arrive -- until no hole remains. This survives the short (~4 s)
    * per-cycle connect window, which a single one-shot request does not. */
   if (!did_bf && isnew) {
      struct dex_session s;
      driver_lock();
      driver_get_session(&s);
      driver_unlock();
      long window = 24L * 3600;
      if (s.have_reading && (long)s.session_seconds > 0 &&
          (long)s.session_seconds < window)
         window = (long)s.session_seconds;
      long now      = realtime_s();
      long floor_t  = now - window;
      long gap_from = 0; /* older edge of the OLDEST hole within the window */
      hist_lock();
      long newer =
          0; /* previous (newer) sample's time, walking newest->oldest */
      for (int i = 0; i < g_nhist; i++) {
         if (g_hist[i].src != (unsigned short)src || g_hist[i].kind == KIND_BGM)
            continue;
         long ts = g_hist[i].t;
         if (ts < floor_t)
            break; /* g_hist is newest-first, so we are past the window */
         if (newer && newer - ts > 450)
            gap_from = ts; /* hole between ts and newer; keep the oldest one */
         newer = ts;
      }
      hist_unlock();
      /* Throttle so a persistent hole (records genuinely gone from the
       * sensor) is retried at a sane cadence, not on every reading. */
      if (gap_from > 0 && now - g_gap_bf_at[bflink] > 120) {
         g_gap_bf_at[bflink] = now;
         long span = (now - gap_from) + 300; /* cover the hole plus a margin */
         LOGI("interior gap in window -> backfill span %ld s (retry until "
              "filled)",
              span);
         driver_lock();
         driver_request_backfill(span);
         driver_unlock();
      }
   }
}

/* live connection signal strength from readRemoteRssi (no sensor-battery
 * cost)
 */
void pancra_rssi(int rssi)
{
   /* PER LINK. jni_rssi knows which link the measurement came from, but this
    * used to drop it into globals that fill_sensor then stamped onto EVERY
    * CGM slot -- so with a Stelo and a G7 both worn, each device screen could
    * show the other's signal, which is the one number on that row the user
    * cannot sanity-check. The globals stay for the main screen's single
    * readout; the per-link copy is what the per-device row reads. */
   int lk = driver_link();
   if (lk >= 0 && lk < LINK_MAX) {
      g_link_rssi[lk]    = rssi;
      g_link_rssi_ok[lk] = 1;
      g_link_rssi_t[lk]  = realtime_s();
   }
   g_conn_rssi   = rssi;
   g_conn_rssi_t = realtime_s();
   /* Latch the CGM's last signal strength the MOMENT it is measured on
    * connect, exactly like pancra_meter_rssi does for a meter -- not gated
    * behind a fresh datapoint. Otherwise the Stelo's SIGNAL row drops to
    * "--" whenever readings lag, while a meter (which latches on connect)
    * keeps showing its last value. This is a retained "last known" display,
    * so it never expires.
    */
   g_cur_rssi    = rssi;
   g_cur_rssi_ok = 1;
   LOGI("rssi %d dbm", rssi);
   draw(g_win);
}

/* Meter link RSSI, read once per sync connection (the meter has no
 * continuous link). Stored separately from the CGM RSSI so the meter's
 * SIGNAL row shows its own last-sync strength. */
void pancra_meter_rssi(int rssi)
{
   g_meter_rssi    = rssi;
   g_meter_rssi_ok = 1;
   g_meter_rssi_t  = realtime_s();
   /* Record against THIS meter so its SIGNAL row shows its own last value,
    * and refresh its sync time -- RSSI is read on connect, i.e. an actual
    * sync. */
   struct meter_rt *rt = (g_meter_src > 0) ? meter_rt_get(g_meter_src, 1) : 0;
   if (rt) {
      rt->rssi    = rssi;
      rt->rssi_ok = 1;
      rt->rssi_t  = realtime_s();
      rt->sync_t  = realtime_s();
      meter_sync_save(); /* connect confirmed: persist the last-sync time */
   }
   LOGI("meter rssi %d dbm", rssi);
   draw(g_win);
}

/* Copy a DIS string into a 24-byte field, NEUTERING the CSV delimiters.
 *
 * These strings come off the sensor's GATT server and go straight into
 * sensors.csv as bare %s fields -- no quoting, no escaping. A model or
 * firmware value containing a COMMA shifts every following field on parse
 * (activation and paired times land in the wrong columns); one containing a
 * NEWLINE splits the row in two, and sensor_mint documents exactly where
 * that leads: an unparseable row hides an id from the loader, maxid goes
 * backwards, and the next mint REISSUES A LIVE ID -- the one failure the
 * whole provenance design exists to make impossible, and it is permanent
 * because the file is never rewritten.
 *
 * The value is attacker-controlled by anything that can present the locked
 * MAC, and merely quirky vendor firmware could do it by accident. Substitute
 * rather than truncate: an empty firmware field is itself meaningful (it
 * marks the row stale and drives the re-mint pass), so dropping characters
 * could turn a hostile string into a silent re-mint loop. */
static void devinfo_copy(char *dst, const char *src)
{
   int k = 0;
   for (; src[k] && k < 22; k++) {
      unsigned char c = (unsigned char)src[k];
      dst[k]          = (c < 0x20 || c > 0x7e || c == ',') ? '_' : (char)c;
   }
   dst[k] = 0;
}

/* device-info string (serial / firmware / software) read from DIS 0x180A */
void pancra_devinfo(int link, const char *uuid, const char *val)
{
   if (!val || !val[0] || !uuid)
      return;
   /* uuid is the full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb";
    * the 16-bit assigned number sits at offset 4. Guard the length before
    * indexing uuid+4 so a short/empty string can't read out of bounds. */
   int ulen = 0;
   while (uuid[ulen] && ulen < 8)
      ulen++;
   if (ulen < 8)
      return;
   /* A meter's identity must not land in the CGM's globals: each sensor's
    * model/firmware is part of its permanent provenance, and mixing them
    * would attribute readings to hardware that never produced them. */
   /* READ THE ROUTING BIT UNDER THE LOCK. link_set_meter's comment claims
    * this function is one of the binder-thread readers that "already hold
    * it" -- the premise the writer's own locking rests on -- and it did not.
    * The write lands on the main thread in meter_sync_start/commit_pair
    * immediately before the connect, and this read arrives on a binder
    * thread just after it, so a stale value is exactly the ordering the
    * lock's barrier exists to prevent. Snapshot and release: the rest of the
    * function takes sensors_lock, and driver -> reg is the documented order,
    * but there is no reason to hold both. */
   int is_meter = 0;
   if (link >= 0 && link < LINK_MAX) {
      driver_lock();
      is_meter = g_link_meter[link];
      driver_unlock();
   }
   char *dst = 0;
   if (is_meter) {
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         dst = g_meter_model;
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         dst = g_meter_fw;
      else
         return;
   } else if (strncmp(uuid + 4, "2a24", 4) == 0) {
      dst = g_model; /* model number      */
   } else if (strncmp(uuid + 4, "2a26", 4) == 0) {
      dst = g_fw; /* firmware revision */
   } else if (strncmp(uuid + 4, "2a29", 4) == 0) {
      dst = g_mfr; /* manufacturer name */
   }
   /* Keep a PER-LINK copy as well. g_model/g_fw are process-global and
    * shared by every CGM link, and the devinfo re-read is skipped once they
    * are non-empty (and they persist to disk), so a second sensor was minted
    * with the FIRST sensor's model and firmware -- written into an
    * append-only provenance file that is never rewritten, and used as part
    * of the id-reuse key. Pair a G7 after a Stelo and its permanent record
    * claimed Stelo hardware. Minting reads the per-link copy. */
   /* THE PER-LINK COPY IS THE MINT INPUT, so it is what needs the lock.
    *
    * An earlier version locked only the process-global g_model/g_fw below,
    * which are display-only -- the arrays sensor_mint actually reads were
    * left written byte-by-byte with no lock at all, so the fix was inert. A
    * torn read (terminator not yet written, so "1.4" over "1.2.3" reads as
    * "1.4.3") matches no stored row, mints a NEW id and rebinds the slot,
    * permanently splitting one physical sensor into two identities in an
    * append-only file. Readers hold the same lock: sensor_mint takes it
    * internally, and the reconcile passes copy under it. */
   /* REUSE THE SNAPSHOT -- do not re-read g_link_meter here.
    *
    * This read was under sensors_lock, which is the wrong lock for that
    * variable (link_set_meter writes it under driver_lock), and taking
    * driver_lock inside sensors_lock would invert the documented
    * driver -> reg order. But the deeper problem is that it was a SECOND,
    * independent read of a bit already decided above: link_set_meter landing
    * between the two makes this function pick the meter branch for `dst` and
    * the CGM branch for the per-link copy. Since the per-link copy is the
    * mint input, that writes a METER's model into the array sensor_mint
    * reads, in an append-only provenance file that is never rewritten. One
    * snapshot, one decision. */
   sensors_lock();
   if (link >= 0 && link < LINK_MAX && !is_meter) {
      char *ld = 0;
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         ld = g_model_l[link];
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         ld = g_fw_l[link];
      if (ld)
         devinfo_copy(ld, val);
   }
   if (!dst) {
      sensors_unlock();
      return;
   }
   devinfo_copy(dst, val);
   sensors_unlock();
   LOGI("devinfo %s = %s", uuid, dst);
   info_save();
   draw(g_win);
}

/* --- settings-menu system ops via Ble.java (main thread; g_act->env valid)
 * ---
 */
static void sys_set_orientation(int mode)
{
   if (!g_act || !m_set_orient)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_set_orient, g_act->clazz, (jint)mode);
}

/* EXPORT DATA: Java builds the combined CSV and opens the system share
 * sheet.
 */
static void sys_export_data(void)
{
   if (!g_act || !m_export)
      return;
   JNIEnv *e = g_act->env;
   /* Cutoff epoch: rows OLDER than this are left out (0 = keep all).
    * Sections per the EXPORT DATA checkboxes; Java filters by each row's
    * leading epoch field and keeps header lines. */
   long cutoff = 0;
   if (g_exp_range == 0)
      cutoff = realtime_s() - (30L * 86400);
   else if (g_exp_range == 1)
      cutoff = realtime_s() - (365L * 86400);
   (*e)->CallStaticVoidMethod(
       e, g_ble, m_export, g_act->clazz, (jlong)cutoff,
       (jboolean)(g_exp_glu != 0), (jboolean)(g_exp_dev != 0),
       (jboolean)(g_exp_ins != 0), (jboolean)(g_exp_wt != 0));
}

static int sys_perm_granted(const char *perm)
{
   if (!g_act || !m_perm_granted)
      return 0;
   JNIEnv *e = g_act->env;
   jstring p = (*e)->NewStringUTF(e, perm);
   jboolean r =
       (*e)->CallStaticBooleanMethod(e, g_ble, m_perm_granted, g_act->clazz, p);
   (*e)->DeleteLocalRef(e, p);
   return r;
}

static void sys_request_perm(const char *perm)
{
   if (!g_act || !m_req_perm)
      return;
   JNIEnv *e = g_act->env;
   jstring p = (*e)->NewStringUTF(e, perm);
   (*e)->CallStaticVoidMethod(e, g_ble, m_req_perm, g_act->clazz, p);
   (*e)->DeleteLocalRef(e, p);
}

static void
sys_open_settings(void) /* app details page: grant or revoke anything */
{
   if (!g_act || !m_open_settings)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_open_settings, g_act->clazz);
}

static int sys_call_bool(jmethodID m)
{
   if (!g_act || !m)
      return 0;
   JNIEnv *e = g_act->env;
   return (*e)->CallStaticBooleanMethod(e, g_ble, m, g_act->clazz);
}

static void sys_request_battery(void)
{
   if (!g_act || !m_req_batt)
      return;
   JNIEnv *e = g_act->env;
   (*e)->CallStaticVoidMethod(e, g_ble, m_req_batt, g_act->clazz);
}

static int sys_standby_bucket(void)
{
   if (!g_act || !m_bucket)
      return -1;
   JNIEnv *e = g_act->env;
   return (*e)->CallStaticIntMethod(e, g_ble, m_bucket, g_act->clazz);
}

/* Sample the system states the settings screen shows into the g_sys_* cache.
 * MAIN THREAD ONLY (JNI via the activity env): call on menu-open and after
 * an action, never from a render -- build_model just copies the cache. */
static void sys_refresh(void)
{
   for (int i = 0; i < NPERMS; i++)
      g_sys_perm[i] = sys_perm_granted(perms[i]);
   g_sys_batt   = sys_call_bool(m_batt_ok);
   g_sys_bucket = sys_standby_bucket();
   g_sys_bg     = sys_call_bool(m_bg_restricted);
}

/* Close the keypad/device-list back to wherever pairing was launched from:
 * the settings menu (g_kp_return==MENU_SETTINGS) or the main screen
 * (MENU_NONE, restoring the chosen orientation). */
static void keypad_close(void)
{
   g_menu = g_kp_return;
   if (!g_kp_return)
      sys_set_orientation(g_orient);
}

/* Begin collecting pairing candidates WITHOUT disturbing the current sensor:
 * a passive scan only (the existing bond keeps reconnecting by MAC on its
 * own). g_smart_pairing suppresses the first-DX auto-pair so nothing is
 * touched until the user commits to a specific sensor. */
static void pair_scan_start(void)
{
   g_smart_pairing = 1;
   devlist_lock(); /* atomic vs the binder-thread advert writer */
   g_ndevs = 0;    /* fresh candidate list */
   devlist_unlock();
   if (g_act)
      start_scan(g_act);
}

/* Abandon pairing: stop the candidate scan; the existing bond is untouched.
 */
static void pair_cancel(void)
{
   g_smart_pairing = 0;
   if (g_act)
      stop_scan(g_act);
}

/* Choose which scanned sensor to pair:
 *   0 found  -> -1 (show the list; it fills as the scan continues)
 *   1 found  -> that one
 *   >1 found -> the strongest IF it beats the next by >= 20 dB (clearly the
 * one on your body); otherwise -1 (ambiguous -> let the user pick).
 */
static int select_candidate(void)
{
   /* Decision in scanlogic.c so `make check` can fail on it. Deleting the
    * ambiguity rule here used to pass the entire gate. */
   /* Only candidates heard RECENTLY qualify. The list is never pruned, so a
    * sensor that left the room an hour ago still sits there with its stale
    * RSSI -- and a pending pairing evaluates this on every tick, possibly
    * long after the list was built. Comparing a stale RSSI against a fresh
    * one under the 20 dB rule picks wrong exactly when it matters. */
   int rssi[MAX_DEVS];
   int map[MAX_DEVS];
   int n    = 0;
   long now = realtime_s();
   devlist_lock(); /* consistent (count, rssi[]) vs the binder-thread writer
                    */
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++) {
      if (g_devs[i].seen_t > 0 && now - g_devs[i].seen_t > 60)
         continue;
      rssi[n] = g_devs[i].rssi;
      map[n]  = i;
      n++;
   }
   devlist_unlock();
   int p = scan_pick_candidate(rssi, n);
   return (p < 0) ? -1 : map[p];
}

/* How many candidates are FRESH on the air right now (same 60 s window as
 * select_candidate). One is the unambiguous-by-existence case: with a single
 * sensor of the requested family in range, a confirmation list of one is
 * pure ceremony. */
static int fresh_candidates(void)
{
   int n    = 0;
   long now = realtime_s();
   devlist_lock();
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++)
      if (g_devs[i].seen_t == 0 || now - g_devs[i].seen_t <= 60)
         n++;
   devlist_unlock();
   return n;
}

/* LAND ON THE NEW DEVICE after a successful pairing commit, instead of
 * dropping the user back where they started.
 *
 * Pairing used to end by simply closing the keypad, which returned to the
 * DEVICES list or the main screen with nothing to show for it -- the tap read
 * as having done nothing even though it had worked, because a fresh CGM has no
 * reading for its whole warmup hour. The per-device screen is where that hour
 * is legible: it carries the WARMUP countdown, the session state and the
 * pairing outcome, so the flow ends looking at the thing it just created.
 *
 * Call AFTER keypad_close(): that leaves g_menu at the flow's own return
 * target, and THAT is the origin recorded for the X -- never a hardcoded
 * screen. Backing out of the new device therefore lands exactly where the add
 * flow would have, which is the same record-the-origin rule MA_SENSOR follows.
 */
static void open_new_device(int slot)
{
   if (slot < 0 || slot >= g_nslot)
      return;
   g_sensor_from = g_menu;
   g_sel         = slot;
   g_menu        = MENU_SENSOR;
}

/* Commit to a specific sensor: NOW drop the old bond and pair the chosen MAC
 * with the entered code. Only reached after the code is in and a candidate
 * is chosen (auto or from the list). */
static void commit_pair(const char *mac)
{
   /* PAIRING MODE ENDS HERE, on every path.
    *
    * g_smart_pairing was cleared only on the two success paths, so any of
    * the four early returns below (meter busy, mint failed, slots full, no
    * free link) left it latched at 1 forever. jni_on_advert gates the whole
    * advert-driven reconnect on !g_smart_pairing, and the on_timer scan
    * self-heal is gated on it too -- so a single failed pairing attempt
    * stopped every already-paired CGM from ever reconnecting again, with
    * nothing on screen to say so: the advert counter keeps climbing while
    * the reading quietly stops ageing forward. The user has committed to a
    * device by the time we are called; whether it works out does not change
    * that. */
   g_smart_pairing = 0;
   g_pend_pairing  = 0; /* any commit supersedes an armed pending pairing */
   /* A meter has no key exchange: it bonds at the OS level (the meter shows
    * a passkey, Android prompts for it) the first time we touch its GATT. So
    * "pairing" one is just registering it and connecting -- the bond happens
    * as a side effect of the sync, and a refused connection reports back as
    * METER: NOT PAIRED rather than failing silently. */
   if (sensor_kind(g_add_type) == KIND_BGM) {
      /* REFUSE while another meter is mid-sync.
       *
       * The advert path gates on !g_meter_busy; this one did not, and it
       * resets the SAME otble statics. A user in ADD SENSOR -> ONETOUCH is
       * there precisely because a scan is running and adverts are flowing,
       * so meter A can be walking records on a binder thread when they tap
       * meter B here. The main thread then runs ot_init() concurrently --
       * phase to P_IDLE mid-walk, last_index replaced, g_meter_src repointed
       * -- and three things break permanently: A's remaining fingersticks
       * are written to readings.csv under B's id (append-only, never
       * rewritten); meter_index_save stores A's walk position under B's id,
       * which is the cross-meter corruption the per-meter index file exists
       * to prevent; and the interrupted sync sends no ack, wedging until the
       * 90 s watchdog. The sync is seconds long and self-clears, so refusing
       * costs the user a retry and nothing else. */
      if (g_meter_busy) {
         LOGI("refusing to pair a meter while another is mid-sync");
         set_status("METER BUSY, RETRY");
         keypad_close();
         return;
      }
      int id = sensor_mint(g_add_type, mac, "", "", "", 0);
      if (id < 0) {
         set_status("METER: REGISTER FAILED");
         keypad_close();
         return;
      }
      int idx = sensor_claim_slot(id, g_add_type, mac);
      if (idx < 0) {
         set_status("SENSOR SLOTS FULL");
         keypad_close();
         return;
      }
      g_meter_src = id;
      /* Seed THIS meter's stored index. Without it the driver kept whatever
       * last_index the previously synced meter left in its static state, so
       * a newly paired meter with a higher counter had its oldest records
       * skipped
       * -- and ot_drv_done then persisted that skipped index under the new
       * meter's id, making the loss permanent. */
      /* otble's statics are otherwise only touched under driver_lock, from
       * jni_notify / jni_disconnected on a binder thread. */
      driver_lock();
      ot_init(meter_index_load(id));
      driver_unlock();
      sensors_lock(); /* the lock pancra_devinfo writes these under */
      g_meter_model[0] = 0;
      g_meter_fw[0]    = 0;
      sensors_unlock();
      g_smart_pairing = 0;
      str_snapshot(g_meter_mac, sizeof g_meter_mac, mac);
      /* The new meter's own link, by the same allocation as every other
       * device -- link_for_slot binds it to this slot's address. */
      int mlink = meter_alloc_link();
      /* BOTH bounds, as meter_sync_start does. Only the lower one was
       * checked here, and g_link_armed is indexed by this below. */
      if (mlink < 0 || mlink >= LINK_MAX) {
         set_status("NO FREE LINK");
         keypad_close();
         return;
      }
      link_set_meter(mlink, 1);
      /* ARM THE TABLE -- the same discipline as meter_sync_start, and for
       * the same reason. pancra_meter_connected identifies the meter ONLY
       * from g_link_armed[link]; with the entry empty it finds "no
       * registered meter there" and REFUSES the very connection this path
       * just issued. Pairing then only completed by accident: an unarmed
       * link also makes meter_armed() false, so the 1 Hz tick called
       * meter_sync_start, which allocated the same meter a SECOND link and
       * armed that one properly. The pairing worked and a link leaked every
       * time. Armed before the connect, because the callback lands on a
       * binder thread and a meter this close answers in milliseconds. */
      driver_lock();
      str_snapshot(g_link_armed[mlink], sizeof g_link_armed[mlink], mac);
      driver_unlock();
      g_meter_link  = mlink;
      g_meter_busy  = 1;
      g_meter_start = realtime_s();
      /* Ask for the OS bond NOW rather than letting the first GATT touch
       * trigger it minutes from now: the meter shows a passkey and Android
       * prompts for it, and a prompt that arrives while the user is still
       * looking at this screen is one they can actually answer. */
      dexble_create_bond(mac);
      set_status("METER: PAIRING");
      LOGI("registered meter id=%d mac=%s on link %d; connecting to bond", id,
           mac, mlink);
      /* Release on every path that does not leave a connect outstanding, or
       * the link stays armed with nothing behind it -- and armed is exactly
       * what stops the tick from ever retrying it. */
      int connected = 0;
      if (g_act) {
         g_scan_hold_until = realtime_s() + 20; /* quiet radio to bond */
         stop_scan(g_act);
         connected = dexble_meter_connect(mlink, mac);
      }
      if (!connected) {
         LOGI("meter %s: pairing connect did not reach the transport", mac);
         meter_release_link(mlink);
         g_meter_busy = 0;
         g_meter_link = -1;
         set_status("METER: NOT PAIRED");
      }
      keypad_close();
      /* Even on a failed connect: the slot IS registered, and its own screen
       * is where the failure is stated and retried. Returning to the list
       * would hide the one row that explains what happened. */
      open_new_device(idx);
      return;
   }

   /* A new sensor pairs on the first free CGM link, so pairing a second
    * sensor neither disturbs nor replaces one that is already streaming. */
   int link = link_for_slot(g_nslot);
   if (link < 0) {
      set_status("NO FREE SENSOR LINK");
      LOGI("refusing to pair: all %d links in use", LINK_MAX);
      keypad_close();
      return;
   }
   /* Register the sensor NOW, before the radio work: the user has committed
    * to this device, and the DEVICES list must say so immediately -- waiting
    * for the first reading (a minute or more) reads as the tap having done
    * nothing. The row is minted BARE (activation unknown, no DIS strings
    * yet); sensor_reconcile completes those attributes in place once they
    * arrive (sensor_complete), so nothing wrong is ever written to the
    * append-only file -- only nothing-yet. Registration BEFORE driver_forget
    * below, so a refusal here leaves the existing bond untouched. */
   int newslot = -1;
   {
      int cgm_type =
          (sensor_kind(g_add_type) == KIND_CGM) ? g_add_type : SENSOR_STELO;
      int id = sensor_mint(cgm_type, mac, "", "", "", 0);
      if (id < 0) {
         set_status("SENSOR: REGISTER FAILED");
         keypad_close();
         return;
      }
      int sidx = sensor_claim_slot(id, cgm_type, mac);
      if (sidx < 0) {
         set_status("SENSOR SLOTS FULL");
         keypad_close();
         return;
      }
      newslot = sidx; /* the screen this flow ends on -- see open_new_device */
      LOGI("registered sensor id=%d type=%s mac=%s at pairing commit", id,
           sensor_type_name(cgm_type), mac);
   }
   /* select + forget must be atomic: a concurrent callback moving the
    * selection between them would have driver_forget() erase a DIFFERENT
    * sensor's key and MAC files, destroying a live pairing. */
   driver_lock();
   driver_select(link);
   driver_forget();
   driver_unlock();
   g_smart_pairing = 0;
   if (g_act) {
      g_scan_hold_until = realtime_s() + 20; /* quiet radio for the J-PAKE */
      stop_scan(g_act);
   }
   set_status("PAIRING");
   /* Ask for the OS bond BEFORE the GATT work, for the same reason as the
    * meter above: the dialog then belongs to the tap that caused it.
    *
    * Safe against the J-PAKE that follows. Ble.createBond returns immediately
    * when the device is already BONDED or already BONDING, so this cannot
    * restart a bond mid-flight; when it is neither, the request goes out and
    * dexble_pair's connectGatt (autoConnect=true) attaches to the same device.
    * The sensor asks for security itself a few seconds into the connection
    * anyway -- an HCI capture shows the full LE Secure Connections exchange
    * completing seven seconds before the first EGV -- so this only moves the
    * prompt earlier, it does not add one that was not going to happen. */
   dexble_create_bond(mac);
   if (g_act)
      dexble_pair(link, mac, g_code_str);
   keypad_close();
   /* The whole point: a fresh CGM shows nothing for an hour, so end the flow
    * on the screen that counts that hour down. */
   open_new_device(newslot);
   LOGI("pair new sensor %s with code %s on link %d", mac, g_code_str, link);
}

/* Apply the SCREEN setting. FLAG_KEEP_SCREEN_ON is what overrides the
 * display timeout while the app is open; clearing it hands the screen back
 * to the OS. Only the window flag changes -- the BLE wakelock in
 * PancraService is separate, so readings keep arriving either way. */
static void apply_screen_on(void)
{
   if (!g_act)
      return;
   if (g_screen_on)
      ANativeActivity_setWindowFlags(g_act, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
   else
      ANativeActivity_setWindowFlags(g_act, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
}

/* Point the driver at the sensor whose screen is open. Calibration reads and
 * writes go to the selected link, and the 1 Hz reconcile moves that
 * selection constantly -- without this the user could calibrate a different
 * sensor from the one named on screen, which for the most consequential
 * write in the app is not an acceptable failure. Returns 0 if the selection
 * is not usable. */
static int cal_select(void)
{
   /* Takes driver_lock() on success -- the caller MUST driver_unlock() after
    * the calibration call, or the selection it just made could be moved by
    * another thread before the write goes out. */
   if (g_sel < 0 || g_sel >= g_nslot)
      return 0;
   /* Under the registry lock: srec_push memmoves g_srec from a binder
    * thread, and this decides whether a CALIBRATION -- the most
    * consequential write in the app -- is permitted. A torn read of the type
    * could permit one against the wrong sensor. Copy the answer out; hold no
    * pointer. */
   sensors_lock();
   const struct sensor_rec *r = sensor_rec_by_id(g_slot[g_sel].id);
   int is_cgm                 = (r && sensor_kind(r->type) == KIND_CGM);
   sensors_unlock();
   if (!is_cgm)
      return 0;
   int link = link_for_slot(g_sel);
   if (link < 0)
      return 0;
   driver_lock();
   driver_select(link);
   return 1;
}

/* ---- durable calibration queue (see g_calq_* above) ---- */

static void calq_save(void)
{
   int fd = open(g_calq_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   /* queued (id,mgdl,t) then last-resolved (mgdl,t,ok,id) on one line. */
   char b[112];
   int n = snprintf(b, sizeof b, "%d,%d,%ld,%d,%ld,%d,%d\n", g_calq_id,
                    g_calq_mgdl, g_calq_t, g_lastcal_mgdl, g_lastcal_t,
                    g_lastcal_state, g_lastcal_id);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) < 0) { /* best effort: a lost persist only costs a retry
                                 across a restart, never a wrong write */
   }
   close(fd);
}

static void calq_clear(void)
{
   g_calq_mgdl = 0;
   g_calq_id   = 0;
   g_calq_t    = 0;
   g_calq_sent = 0;
   calq_save();
}

static void calq_load(void)
{
   int fd = open(g_calq_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[64];
   long n = read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]      = 0;
   long v[7] = {0, 0, 0, 0, 0, 0, 0};
   int vi    = 0;
   int neg   = 0;
   for (char *p = b; *p && vi < 7; p++) {
      if (*p >= '0' && *p <= '9') {
         v[vi] = (v[vi] * 10) + (*p - '0');
      } else if (*p == '-') {
         neg = 1;
      } else if (*p == ',' || *p == '\n') {
         if (neg)
            v[vi] = -v[vi];
         neg = 0;
         vi++;
         if (*p == '\n')
            break;
      }
   }
   /* last-resolved record (fields 4..7) survives regardless of the queue. */
   g_lastcal_mgdl  = (int)v[3];
   g_lastcal_t     = v[4];
   g_lastcal_state = (int)v[5];
   g_lastcal_id    = (int)v[6];
   if (v[1] <= 0)
      return; /* no value queued */
   g_calq_id   = (int)v[0];
   g_calq_mgdl = (int)v[1];
   g_calq_t    = v[2];
   g_calq_sent = 0;
   /* A calibration confirmed before a restart: keep retrying if it is still
    * fresh, otherwise record the failure -- never drop it silently. */
   if (realtime_s() - g_calq_t > CALQ_WINDOW_S) {
      g_lastcal_mgdl  = g_calq_mgdl;
      g_lastcal_t     = realtime_s();
      g_lastcal_state = CAL_ST_FAILED;
      g_lastcal_id    = g_calq_id;
      (void)snprintf(g_calq_status, sizeof g_calq_status, "LOST - RE-ENTER");
      calq_clear();
   } else {
      (void)snprintf(g_calq_status, sizeof g_calq_status, "PENDING %d",
                     g_calq_mgdl);
   }
}

/* Attempt the queued calibration NOW. driver_lock must be held and the
 * driver selected to the queued sensor's link (true inside pancra_glucose,
 * which is the ideal moment -- a reading just proved the sensor is
 * streaming). A refusal is not a loss: the value stays queued and the next
 * stream tries again. */
static void calq_try_locked(void)
{
   if (g_calq_mgdl <= 0)
      return;
   struct dex_cal c;
   driver_get_cal(&c);
   /* The sensor answered and does NOT permit calibration at all (a factory-
    * calibrated Stelo, say). Distinct from a value the sensor REJECTS: this
    * is "the device does not support calibration", so say NOT SUPPORTED.
    * Fail VISIBLY at once rather than leaving it PENDING until the window
    * lapses. This only drops the queued value -- it does NOT lock
    * calibration out: a later user-initiated calibration re-queues and
    * re-probes permission afresh. */
   if (c.have && !c.permitted) {
      LOGI("calibration not permitted by this sensor; queued %d mg/dL not "
           "sent",
           g_calq_mgdl);
      g_lastcal_mgdl  = g_calq_mgdl;
      g_lastcal_t     = realtime_s();
      g_lastcal_state = CAL_ST_NOTSUP;
      g_lastcal_id    = g_calq_id;
      (void)snprintf(g_calq_status, sizeof g_calq_status, "NOT SUPPORTED");
      calq_clear();
      g_ui_dirty = 1;
      return;
   }
   /* Permission not yet known: PROBE it (0x32). driver_calibrate refuses
    * without a positive answer, and nothing else sends this probe during
    * streaming, so the calibration could otherwise never proceed. The write
    * itself goes on the next stream once the reply sets cal.permitted.
    *
    * BE GENTLE with a sensor that may not want calibrations: throttle the
    * probe to at most once a minute (cal.asked is when we last asked), so a
    * Stelo that never answers is nudged only a handful of times before the
    * window lapses and it FAILS -- never hammered. A sensor that answers
    * "no" is caught by the fast-fail above and never probed again. */
   if (!c.have) {
      if (c.asked == 0 || realtime_s() - c.asked >= 60) {
         driver_cal_bounds();
         (void)snprintf(g_calq_status, sizeof g_calq_status, "PROBING %d",
                        g_calq_mgdl);
         LOGI("calibration queued: probing 0x32 permission before writing");
      }
      return;
   }
   /* Permitted: send the calibration, but only if we are not already
    * awaiting a reply from a recent send (calq_tick clears g_calq_sent after
    * 60 s of silence). One 0x34 per minute at most -- gentle, and the
    * sensor's reply normally resolves it on the first try. */
   if (g_calq_sent > 0 && realtime_s() - g_calq_sent < 60)
      return;
   if (driver_calibrate(g_calq_mgdl)) {
      g_calq_sent = realtime_s();
      (void)snprintf(g_calq_status, sizeof g_calq_status, "SENDING %d",
                     g_calq_mgdl);
      LOGI("calibration %d mg/dL submitted from queue, awaiting sensor reply",
           g_calq_mgdl);
   }
}

/* 1 Hz housekeeping: let a stuck attempt retry, and give up VISIBLY (never
 * silently) once the reference is too old to trust. */
static void calq_tick(void)
{
   if (g_calq_mgdl <= 0)
      return;
   long now = realtime_s();
   if (g_calq_sent > 0 && now - g_calq_sent > 60)
      g_calq_sent = 0; /* no reply in a minute: allow another attempt */
   if (now - g_calq_t > CALQ_WINDOW_S) {
      (void)snprintf(g_calq_status, sizeof g_calq_status, "FAILED - RE-ENTER");
      LOGI("calibration %d mg/dL never accepted within %ld s; giving up "
           "VISIBLY",
           g_calq_mgdl, CALQ_WINDOW_S);
      g_lastcal_mgdl  = g_calq_mgdl;
      g_lastcal_t     = now;
      g_lastcal_state = CAL_ST_FAILED;
      g_lastcal_id    = g_calq_id;
      /* No beep -- LAST CAL shows FAILED; the official app is silent too. */
      calq_clear();
      g_ui_dirty = 1;
   }
}

/* Driver callback: the sensor answered a calibration we sent. */
void pancra_cal_result(int result)
{
   if (g_calq_mgdl <= 0)
      return; /* unsolicited / already resolved */
   g_lastcal_mgdl = g_calq_mgdl;
   g_lastcal_t    = realtime_s();
   g_lastcal_id   = g_calq_id;
   if (result == 0) {
      LOGI("calibration %d mg/dL ACCEPTED by the sensor", g_calq_mgdl);
      (void)snprintf(g_calq_status, sizeof g_calq_status, "APPLIED %d",
                     g_calq_mgdl);
      g_lastcal_state = CAL_ST_APPLIED;
      calq_clear();
   } else {
      /* The sensor actively rejected the value -- resending it will not
       * help, so surface it (LAST CAL shows REJECTED) rather than looping or
       * dropping it silently. No beep: the official app is silent on a
       * rejection too. */
      LOGI("calibration %d mg/dL REJECTED by the sensor (result=0x%02x)",
           g_calq_mgdl, result);
      (void)snprintf(g_calq_status, sizeof g_calq_status, "REJECTED %d",
                     g_calq_mgdl);
      g_lastcal_state = CAL_ST_REJECTED;
      calq_clear();
   }
   g_ui_dirty = 1;
}

/* Is the old device in slot idx already past its wear + grace? Reconnecting
 * a pre-expiry disconnect is sensible; a post-expiry one is confirmed first.
 */
static int old_slot_expired(int idx)
{
   if (idx < 0 || idx >= g_nslot)
      return 0;
   sensors_lock();
   const struct sensor_rec *r = sensor_rec_by_id(g_slot[idx].id);
   long act                   = r ? r->activation : 0;
   long wear = sensor_wear_seconds(r ? r->type : SENSOR_STELO,
                                   g_slot[idx].wear_days, r ? r->model : "");
   sensors_unlock();
   if (act <= 0 || wear <= 0)
      return 0; /* unknown -> treat as not-yet-expired (allow direct) */
   return realtime_s() > act + wear + SENSOR_GRACE_S;
}

/* Revive slot idx and leave the per-device screen. */
static void do_reconnect(int idx)
{
   if (idx >= 0 && idx < g_nslot) {
      sensor_revive_slot(idx);
      int prime = sensor_primary_id();
      hist_lock();
      hist_refresh_current(prime);
      hist_unlock();
      g_notify_dirty = 1;
   }
   g_sel  = -1;
   g_menu = g_sensor_from;
}

/* Hinnant civil-date conversions (the same algorithm ui.c's fmt_date
 * uses), for the LOG INSULIN keypad date/time entry: the typed MMDD and
 * HHMM must recombine with the untouched half on the calendar, so the
 * date can never leak into the time or vice versa. */
static long days_from_civil(long y, long m, long d)
{
   y -= m <= 2;
   long era          = (y >= 0 ? y : y - 399) / 400;
   unsigned long yoe = (unsigned long)(y - (era * 400));
   unsigned long doy =
       (((153UL * (unsigned long)(m > 2 ? m - 3 : m + 9)) + 2) / 5) +
       (unsigned long)d - 1;
   unsigned long doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
   return (era * 146097) + (long)doe - 719468;
}

static void civil_from_days(long z, long *y, long *m, long *d)
{
   z += 719468;
   long era          = (z >= 0 ? z : z - 146096) / 146097;
   unsigned long doe = (unsigned long)(z - (era * 146097));
   unsigned long yoe =
       (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
   long yy           = (long)yoe + (era * 400);
   unsigned long doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
   unsigned long mp  = ((5 * doy) + 2) / 153;
   *d                = (long)(doy - (((153 * mp) + 2) / 5) + 1);
   *m                = (long)(mp < 10 ? mp + 3 : mp - 9);
   *y                = yy + (*m <= 2);
}

/* Every insulin-related menu action (the LOG/EDIT form, the dose log
 * table, the marker picker), split out of menu_action so neither function
 * outgrows the size gate. Returns 1 when `action` was one of ours. */
/* WEIGHT actions, split out like ins_action so menu_action stays small.
 * Returns 1 when `action` was one of ours. */
static int wt_action(int action)
{
   if (action == MA_WT_OPEN) {
      /* Pre-populate: now, and the LAST logged weight -- a weigh-in moves by
       * ounces, so the previous value is nearly always one or two keypresses
       * from the new one, and starting from zero would make every entry a
       * full retype. */
      g_wt_t      = realtime_s();
      g_wt_tenths = (g_nwt > 0) ? wt_to_tenths(g_wt[g_nwt - 1].g, g_wunits)
                                : wt_to_tenths(70000L, g_wunits);
      g_wt_edit   = -1;     /* a NEW entry, not an edit */
      g_wt_from   = g_menu; /* ADD menu, or a main-screen PIN button */
      /* STRAIGHT TO THE KEYPAD, not to the form. Logging a weight is one
       * number, and every door into this action -- the ADD menu button, the
       * pinned main-screen button -- already says which number. The form in
       * between existed only to be tapped once, on the row this opens.
       *
       * The hop is skipped, not removed: g_kp_return is the WEIGHT form, so
       * OK and X both land there with the date/time rows still editable, and
       * the form's own exits still use g_wt_from. Nothing infers an origin.
       */
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 14;
      g_kp_return = MENU_WEIGHT;
      g_entrylen  = 0;
   } else if (action >= MA_WTLOG_EDIT && action < MA_WTLOG_EDIT + NWT) {
      /* A row in the table opens that entry in the EDIT WEIGHT form. Keep a
       * COPY as the rewrite's match key -- see g_wt_orig. */
      int i = action - MA_WTLOG_EDIT;
      if (i >= 0 && i < g_nwt) {
         g_wt_orig   = g_wt[i];
         g_wt_edit   = i;
         g_wt_t      = g_wt[i].t;
         g_wt_tenths = wt_to_tenths(g_wt[i].g, g_wunits);
         g_menu      = MENU_WEIGHT;
      }
   } else if (action >= MA_WTTAB && action < MA_WTTAB + UI_WT_TABS) {
      g_wt_tab   = action - MA_WTTAB;
      g_wt_scrub = -1; /* the picked point may not be in the new span */
   } else if (action == MA_WT_DELETE) {
      if (g_wt_edit >= 0)
         g_menu = MENU_WTDEL; /* confirm first; this tap deletes nothing */
   } else if (action == MA_WTDEL_NO) {
      g_menu = MENU_WEIGHT;
   } else if (action == MA_WTDEL_YES) {
      if (g_wt_edit >= 0 && weight_delete(&g_wt_orig) == 0) {
         LOGI("weight entry deleted: %ld g at %ld", g_wt_orig.g, g_wt_orig.t);
         set_status("WEIGHT DELETED");
      } else {
         set_status("WEIGHT: DELETE FAILED");
      }
      g_wt_edit  = -1;
      g_menu     = MENU_WTLOG;
      g_ui_dirty = 1;
   } else if (action == MA_WTLOG_OPEN) {
      g_wtlog_page = 0;
      g_wtlog_from = g_menu; /* ADD menu, or a main-screen PIN button */
      g_menu       = MENU_WTLOG;
   } else if (action == MA_WTLOG_BACK) {
      g_menu = g_wtlog_from;
   } else if (action == MA_WTLOG_PREV) {
      if (g_wtlog_page > 0)
         g_wtlog_page--;
   } else if (action == MA_WTLOG_NEXT) {
      g_wtlog_page++; /* the renderer clamps to the last page */
   } else if (action >= MA_WT_EDIT && action < MA_WT_EDIT + 4) {
      /* Tapping a form value opens the keypad for EXACT entry. The weight
       * gets its own mode (14); date/time/year reuse the insulin form's
       * modes 7/8/9, which are about a calendar instant and carry no insulin
       * meaning -- the keypad returns here, so nothing crosses over. */
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = (action == MA_WT_EDIT) ? 14 : (6 + (action - MA_WT_EDIT));
      g_kp_return = MENU_WEIGHT;
      g_entrylen  = 0;
   } else if (action == MA_WT_CONFIRM) {
      /* The one write, on the explicit CONFIRM only (the calibration rule).
       */
      if (g_menu == MENU_WEIGHT) {
         long g = wt_from_tenths(g_wt_tenths, g_wunits);
         int rc = -1;
         int ed = (g_wt_edit >= 0);
         if (g > 0)
            rc = ed ? weight_update(&g_wt_orig, g_wt_t, g, g_tz_off)
                    : weight_append(g_wt_t, g, g_tz_off);
         if (rc == 0) {
            LOGI("weight %s: %ld g at %ld", ed ? "edited" : "logged", g,
                 g_wt_t);
            set_status(ed ? "WEIGHT EDITED" : "WEIGHT LOGGED");
            /* An EDIT returns to the log it was opened from; a NEW entry is
             * a completed task and lands on the main screen (the insulin
             * rule).
             */
            g_menu    = ed ? MENU_WTLOG : MENU_NONE;
            g_wt_edit = -1;
         } else {
            /* Refuse VISIBLY. A weight the user believes recorded but is not
             * is a silent hole in the only copy of that number. */
            set_status("WEIGHT: WRITE FAILED");
         }
         g_ui_dirty = 1;
      }
   } else if (action == MA_WT_DISCARD) {
      /* Back where it came from: the log for an edit, otherwise the screen
       * the form was opened from (the record-the-origin rule -- which this
       * line only claimed to follow while it named a fixed menu). */
      g_menu    = (g_wt_edit >= 0) ? MENU_WTLOG : g_wt_from;
      g_wt_edit = -1;
   } else if (action == MA_WUNITS) {
      /* Display only: the file is grams, so this re-renders history rather
       * than converting it. */
      g_wunits = (g_wunits == WT_LB) ? WT_KG : WT_LB;
      settings_save();
   } else {
      return 0; /* not ours */
   }
   return 1;
}

static int ins_action(int action)
{
   if (action == MA_INS_OPEN || action == MA_INS_FAST ||
       action == MA_INS_SLOW) {
      /* The ADD menu picks the type up front (FAST / SLOW buttons); the
       * legacy MA_INS_OPEN keeps the last-used type. Pre-populate: now
       * (whole minute) and the type's last entered amount (1 U when none
       * is known). */
      if (action == MA_INS_FAST)
         g_ins_type = INS_FAST;
      else if (action == MA_INS_SLOW)
         g_ins_type = INS_SLOW;
      g_ins_t = realtime_s();
      g_ins_t -= g_ins_t % 60;
      int lu      = insulin_last_units(g_ins_type);
      g_ins_units = (lu > 0) ? lu : 1;
      /* The screen the tap came from: the ADD menu, or the main screen when
       * this action is one of its shortcut buttons. */
      g_ins_from = g_menu;
      g_menu     = MENU_INSULIN;
   } else if (action == MA_INS_TYPE) {
      g_ins_type = (g_ins_type == INS_FAST) ? INS_SLOW : INS_FAST;
      /* A NEW dose's amount follows the type (each has its own habitual
       * dose); an EDIT keeps whatever amount is being edited. */
      if (g_ins_edit < 0) {
         int lu      = insulin_last_units(g_ins_type);
         g_ins_units = (lu > 0) ? lu : 1;
      }
   } else if (action == MA_INSLOG_OPEN) {
      g_inslog_page = 0;
      g_inslog_from = g_menu; /* ADD menu, or a main-screen shortcut */
      g_menu        = MENU_INSLOG;
   } else if (action == MA_INSLOG_PREV) {
      if (g_inslog_page > 0)
         g_inslog_page--;
   } else if (action == MA_INSLOG_NEXT) {
      g_inslog_page++; /* render clamps to the last page */
   } else if (action >= MA_INS_EDIT && action < MA_INS_EDIT + 4) {
      /* Tapping a form value opens the keypad for EXACT entry: units (2
       * digits), date (MMDD), time (HHMM) or year (YYYY). The keypad's OK
       * validates and writes back; X returns unchanged. */
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 6 + (action - MA_INS_EDIT);
      g_kp_return = MENU_INSULIN;
      g_entrylen  = 0;
   } else if (action == MA_INS_CONFIRM) {
      /* The one write, on the explicit CONFIRM only (the calibration rule).
       * Editing rewrites the matched original row; logging appends. */
      if (g_menu == MENU_INSULIN) {
         int rc = -1;
         if (g_ins_edit >= 0)
            rc = insulin_update(&g_ins_orig, g_ins_t, g_ins_type, g_ins_units,
                                g_tz_off);
         else
            rc = insulin_append(g_ins_t, g_ins_type, g_ins_units, g_tz_off);
         if (rc == 0) {
            LOGI("insulin %s: %d U %s at %ld",
                 g_ins_edit >= 0 ? "edited" : "logged", g_ins_units,
                 insulin_type_name(g_ins_type), g_ins_t);
            set_status(g_ins_edit >= 0 ? "INSULIN EDITED" : "INSULIN LOGGED");
         } else {
            /* Refuse VISIBLY -- a dose the user believes recorded but is not
             * would corrupt every judgement made on top of the log. */
            set_status("INSULIN: WRITE FAILED");
         }
         /* CONFIRM on a NEW dose lands on the MAIN screen -- logging a dose
          * is a completed task, not a detour to return from, and the status
          * banner + plot marker there ARE the confirmation. An EDIT still
          * returns to the log it was opened from (g_ins_from). */
         g_menu     = (g_ins_edit >= 0) ? g_ins_from : MENU_NONE;
         g_ins_edit = -1;
         g_ui_dirty = 1;
      }
   } else if (action == MA_INS_DELETE) {
      /* Confirm first; this action deletes nothing (the SCR_FORGET rule --
       * DELETE sat right between CANCEL and CONFIRM, one mis-tap from
       * silently losing a logged dose). */
      if (g_menu == MENU_INSULIN && g_ins_edit >= 0)
         g_menu = MENU_INSDEL;
   } else if (action == MA_INSDEL_YES) {
      /* The one deleting control, on the confirmation screen only. */
      if (g_menu == MENU_INSDEL && g_ins_edit >= 0) {
         if (insulin_delete(&g_ins_orig) == 0)
            set_status("INSULIN DELETED");
         else
            set_status("INSULIN: DELETE FAILED");
         g_menu     = g_ins_from;
         g_ins_edit = -1;
         g_ui_dirty = 1;
      }
   } else if (action == MA_INSDEL_NO) {
      if (g_menu == MENU_INSDEL)
         g_menu = MENU_INSULIN; /* back to the EDIT form, state intact */
   } else if (action >= MA_INSLOG_EDIT && action < MA_INSLOG_EDIT + NINS) {
      /* Open this dose in the EDIT form, pre-filled; remember the ORIGINAL
       * row so the eventual rewrite matches content, not a tail index that
       * may have shifted meanwhile. */
      int i = action - MA_INSLOG_EDIT;
      if (g_menu == MENU_INSLOG && i >= 0 && i < g_nins) {
         g_ins_orig  = g_ins[i];
         g_ins_edit  = i;
         g_ins_t     = g_ins[i].t;
         g_ins_type  = g_ins[i].type;
         g_ins_units = g_ins[i].units;
         g_ins_from  = MENU_INSLOG; /* opened from the dose log */
         g_menu      = MENU_INSULIN;
      }
   } else if (action == MA_INS_DISCARD) {
      if (g_menu == MENU_INSULIN) {
         g_menu     = g_ins_from;
         g_ins_edit = -1;
      }
   } else if (action >= MA_INSMARK_OPEN && action < MA_INSMARK_OPEN + 2) {
      g_markpick_ins = action - MA_INSMARK_OPEN; /* INS_SLOW / INS_FAST */
      g_menu         = MENU_MARKPICK;
   } else if (action == MA_INSMARK_BACK) {
      g_markpick_ins = -1;
      g_menu         = MENU_DISPLAY; /* the row lives on the DISPLAY menu */
   } else {
      return 0; /* not an insulin action */
   }
   return 1;
}

/* Every SUBMENU navigation/config action -- ALARM (and its two threshold-
 * keypad openers), EXPORT DATA, REMOTE, PERMISSIONS, DISPLAY, DEVICES, OLD
 * DEVICES -- split out of menu_action so it stays under the size gate (the
 * ins_action pattern). Returns 1 when `action` was one of ours. */
static int submenu_action(int action)
{
   if (action == MA_ALARM_LOW || action == MA_ALARM_HIGH ||
       action == MA_NUDGE_LOW || action == MA_NUDGE_HIGH) {
      /* "LOW <value>" / "HIGH <value>" (main-screen rows or ALARM submenu):
       * type the threshold on the keypad (display units). The keypad
       * returns to WHERE this was tapped -- the origin rule. */
      g_kp_return = (g_menu == MENU_ALARM) ? MENU_ALARM : MENU_NONE;
      g_menu      = MENU_KEYPAD;
      if (action == MA_ALARM_LOW)
         g_kp_mode = 10;
      else if (action == MA_ALARM_HIGH)
         g_kp_mode = 11;
      else if (action == MA_NUDGE_LOW)
         g_kp_mode = 12;
      else
         g_kp_mode = 13;
      g_entrylen = 0;
   } else if (action == MA_NUDGE_SOUND) {
      g_nudge_sound = !g_nudge_sound;
      settings_save();
   } else if (action == MA_NUDGE_VIB) {
      g_nudge_vib = !g_nudge_vib;
      settings_save();
   } else if (action == MA_ALARM_OPEN) {
      /* Entered from the settings row or the main alarm row; record the
       * origin so MA_ALARM_BACK returns exactly there. */
      g_alarm_from = (g_menu == MENU_SETTINGS) ? MENU_SETTINGS : MENU_NONE;
      g_menu       = MENU_ALARM;
   } else if (action == MA_ALARM_BACK) {
      g_menu = g_alarm_from;
   } else if (action == MA_EXPORT) {
      g_menu = MENU_EXPORT; /* configure first; MA_EXP_GO does the work */
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
      if (g_menu == MENU_EXPORT &&
          (g_exp_glu || g_exp_dev || g_exp_ins || g_exp_wt))
         sys_export_data(); /* Java builds the CSV per the checkboxes/range
                             * and opens the share sheet; the menu stays,
                             * its X returns to settings */
   } else if (action == MA_DISPLAY_OPEN) {
      g_menu = MENU_DISPLAY;
   } else if (action == MA_PERMS_OPEN) {
      sys_refresh(); /* fresh snapshot for the screen being opened */
      g_menu = MENU_PERMS;
   } else if (action == MA_REMOTE_OPEN) {
      g_menu = MENU_REMOTE;
   } else if (action == MA_REMOTE_TOGGLE) {
      g_remote_on = !g_remote_on;
      remote_save();
   } else if (action == MA_SYNC_EMAIL) {
      g_entrylen    = 0;
      g_label_field = LABEL_EMAIL;
      for (int i = 0; g_sync_email[i] && g_entrylen < (int)sizeof g_entry - 1;
           i++) {
         char c = g_sync_email[i];
         if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
         g_entry[g_entrylen++] = c;
      }
      g_menu      = MENU_LABEL;
      g_kp_return = MENU_REMOTE;
   } else if (action == MA_SYNC_PAIR) {
      /* Both halves must exist first: a code is meaningless without a server
       * to send it to and an account to send it for. */
      if (g_remote_server[0] && g_sync_email[0]) {
         g_menu      = MENU_KEYPAD;
         g_kp_mode   = 15;
         g_kp_return = MENU_REMOTE;
         g_entrylen  = 0;
      }
   } else if (action == MA_SYNC_UNPAIR) {
      /* Forget the identity, not the server: re-pairing is the normal way to
       * move this phone to a different account, and it needs a fresh code
       * from the server anyway. */
      g_sync_uid = 0;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = 0;
      remote_save();
      sync_set_key(0, g_sync_key);
   } else if (action == MA_REMOTE_IP) {
      /* The LABEL editor, not the keypad: a server is "pancra.org" now, not a
       * dotted quad, and the numeric pad cannot type a letter. Seeded with the
       * current value so a small correction is a small amount of typing. */
      g_entrylen    = 0;
      g_label_field = LABEL_SERVER;
      for (int i = 0; g_remote_server[i] && g_entrylen < (int)sizeof g_entry - 1;
           i++) {
         char c = g_remote_server[i];
         if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A'); /* the editor's charset is upper case */
         g_entry[g_entrylen++] = c;
      }
      g_menu      = MENU_LABEL;
      g_kp_return = MENU_REMOTE;
   } else if (action == MA_REMOTE_PORT) {
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 5;
      g_kp_return = MENU_REMOTE;
      g_entrylen  = 0;
   } else if (action == MA_DEVICES_OPEN) {
      /* Two doors lead here -- the main screen's big number and the SETTINGS
       * row -- so RECORD which one, never infer it: MA_DEVICES_BACK returns
       * exactly there. Capture only on external entry; OLD DEVICES and the
       * pairing flow re-enter MENU_DEVICES on their own way back and must not
       * overwrite the origin. */
      if (g_menu == MENU_NONE || g_menu == MENU_SETTINGS) {
         g_devices_from = g_menu;
         g_dev_page     = 0; /* a fresh entry always opens on page 1 */
      }
      g_menu = MENU_DEVICES;
   } else if (action >= MA_SCTOGGLE &&
              action < MA_SCTOGGLE + ui_shortcut_count()) {
      /* Toggle a main-screen PIN. The list is kept DENSE -- removing the
       * middle one closes the gap -- because the main screen walks it and an
       * empty slot between two live ones would draw a hole in the row.
       *
       * At SC_MAX the extra tap is REFUSED rather than evicting something: the
       * user picked those three, and silently dropping one to make room for a
       * fourth is the kind of helpfulness that loses a setting. The status line
       * says why, so the tap is not simply inert. */
      int code = ui_shortcut_code(action - MA_SCTOGGLE);
      int at   = -1;
      int n    = 0;
      for (int i = 0; i < SC_MAX; i++) {
         if (g_shortcut[i] == code)
            at = i;
         if (g_shortcut[i] > 0)
            n++;
      }
      if (at >= 0) {
         for (int i = at; i < SC_MAX - 1; i++)
            g_shortcut[i] = g_shortcut[i + 1];
         g_shortcut[SC_MAX - 1] = 0;
         settings_save();
      } else if (n < SC_MAX) {
         g_shortcut[n] = code;
         settings_save();
      } else {
         set_status("3 PINS MAX");
      }
   } else if (action == MA_DEVPAGE_PREV) {
      if (g_dev_page > 0)
         g_dev_page--;
   } else if (action == MA_DEVPAGE_NEXT) {
      g_dev_page++; /* render clamps to the last page */
   } else if (action == MA_DEVICES_BACK) {
      g_menu = g_devices_from;
   } else if (action == MA_OLDDEV_OPEN) {
      g_old_page = 0; /* always open on the first page */
      g_menu     = MENU_OLDDEV;
   } else if (action == MA_OLDPAGE_PREV) {
      if (g_old_page > 0)
         g_old_page--;
   } else if (action == MA_OLDPAGE_NEXT) {
      g_old_page++; /* render clamps to the last page */
   } else if (action == MA_OLDDEV_BACK) {
      /* OLD DEVICES hangs off the DEVICES screen, not off settings. */
      g_menu = MENU_DEVICES;
   } else if (action == MA_PERMS_BACK || action == MA_REMOTE_BACK ||
              action == MA_DISPLAY_BACK || action == MA_EXP_BACK) {
      /* All four submenus were opened FROM settings; their X returns
       * there. (The ALARM submenu is the exception -- also reachable from
       * the main screen, so its back goes through g_alarm_from above.) */
      g_menu = MENU_SETTINGS;
   } else {
      return 0; /* not ours */
   }
   return 1;
}

/* Per-device STYLING: marker shape, size, colour, and the rename keypad.
 * Split out of menu_action for the same reason ins_action and wt_action were
 * -- that function is the app's single largest, and the size gate is what
 * stops it growing without bound. Returns 1 when `action` was one of ours.
 */
static int style_action(int action)
{
   if (action == MA_MARKER) {
      if (g_sel >= 0 && g_sel < g_nslot) {
         g_markpick_ins = -1; /* this picker edits the SENSOR's styling */
         g_menu         = MENU_MARKPICK;
      }
   } else if (action >= MA_MARK_PICK && action < MA_MARK_PICK + MARK_N) {
      int mk = action - MA_MARK_PICK;
      if (g_markpick_ins >= 0) {
         /* the picker is editing an INSULIN type's marker, not a sensor's */
         g_ins_marker[g_markpick_ins] = mk;
         settings_save();
      } else if (g_sel >= 0 && g_sel < g_nslot) {
         sensors_lock();
         g_slot[g_sel].marker = mk;
         slots_save();
         sensors_unlock();
      }
      /* stay on the combined MARKER menu so shape/size/colour can be
       * adjusted together; the title-row X returns to the device menu */
   } else if (action >= MA_SIZE_PICK &&
              action <= MA_SIZE_PICK + MARK_SIZE_MAX) {
      int sz = action - MA_SIZE_PICK; /* 1..MARK_SIZE_MAX */
      if (sz >= 1 && sz <= MARK_SIZE_MAX && g_markpick_ins >= 0) {
         g_ins_size[g_markpick_ins] = sz;
         settings_save();
      } else if (sz >= 1 && sz <= MARK_SIZE_MAX && g_sel >= 0 &&
                 g_sel < g_nslot) {
         sensors_lock();
         g_slot[g_sel].size = sz;
         slots_save();
         sensors_unlock();
      }
      /* stay on the combined MARKER menu */
   } else if (action == MA_SIZE) {
      if (g_sel >= 0 && g_sel < g_nslot) {
         sensors_lock();
         /* cycle 1..MARK_SIZE_MAX */
         int nx             = g_slot[g_sel].size + 1;
         g_slot[g_sel].size = (nx > MARK_SIZE_MAX) ? 1 : nx;
         slots_save();
         sensors_unlock();
      }
   } else if (action == MA_LABEL) {
      if (g_sel >= 0 && g_sel < g_nslot) {
         /* seed the field with the current name so a small edit is a small
          * amount of typing */
         g_entrylen = 0;
         for (int i = 0;
              g_slot[g_sel].label[i] && g_entrylen < (int)sizeof g_entry - 1;
              i++)
            g_entry[g_entrylen++] = g_slot[g_sel].label[i];
         g_menu        = MENU_LABEL;
         g_label_field = LABEL_SENSOR;
         g_kp_return   = MENU_SENSOR;
      }
   } else if (action >= MA_CHAR && action < MA_CHAR + ui_label_nchars()) {
      /* Two fields share this editor: a sensor label and the sync SERVER. */
      int cap = (int)sizeof g_slot[0].label - 1;
      if (g_label_field == LABEL_SERVER)
         cap = (int)sizeof g_remote_server - 1;
      else if (g_label_field == LABEL_EMAIL)
         cap = (int)sizeof g_sync_email - 1;
      if (cap > (int)sizeof g_entry - 1)
         cap = (int)sizeof g_entry - 1;
      if (g_entrylen < cap)
         g_entry[g_entrylen++] = ui_label_chars[action - MA_CHAR];
   } else if (action == MA_COLOR) {
      if (g_sel >= 0 && g_sel < g_nslot)
         g_menu = MENU_COLORPICK; /* open the colour picker */
   } else if (action >= MA_COLOR_PICK && action < MA_COLOR_PICK + 7) {
      int ci = action - MA_COLOR_PICK;
      if (g_markpick_ins >= 0) {
         g_ins_color[g_markpick_ins] = ci;
         settings_save();
      } else if (g_sel >= 0 && g_sel < g_nslot) {
         sensors_lock();
         g_slot[g_sel].color = ci;
         slots_save();
         sensors_unlock();
      }
      /* stay on the combined MARKER menu (see MA_MARK_PICK) */
   } else {
      return 0; /* not ours */
   }
   return 1;
}

static void menu_action(int action)
{
   if (action == 0) {
      g_orient = (int)(((unsigned)g_orient + 1U) & 3U);
      settings_save();
   } /* applied on close */
   else if (action == 1) {
      g_sound_on = !g_sound_on;
      settings_save();
      alarm_reactuate(); /* an alarm may be latched but inaudible -- see
                          * there
                          */
   } else if (action == 2) {
      g_vib_on = !g_vib_on;
      settings_save();
      alarm_reactuate();
   } else if (action == 3) {
      g_units = !g_units;
      settings_save();
      /* The notification renders the value in DISPLAY units (title AND the
       * status-bar icon) but is only rebuilt on a new datapoint -- without
       * an explicit refresh the bar keeps showing the OLD units' rendering
       * (e.g. "9.4" beside a big number reading 169) for up to a full CGM
       * cadence after the toggle. */
      g_notify_dirty = 1;
      pancra_notify_refresh();
   } else if (action == 4) {
      g_disc = (int)(((unsigned)g_disc + 1U) & 3U);
      settings_save();
   } else if (action == 5) {
      g_screen_on = !g_screen_on;
      settings_save();
      apply_screen_on(); /* takes effect immediately, not on menu close */
   } else if (action == MA_NEWDATA) {
      /* OFF -> BEEP -> CHIRP -> OFF. A cycle, not a toggle, since CHIRP
       * joined: the row shows which of the three is active. */
      g_newdata_mode = (g_newdata_mode + 1) % 3;
      settings_save();
   }
   /* --- sensor registry --- */
   else if (action >= MA_SENSOR && action < MA_SENSOR + MAX_SLOTS) {
      /* Remember the origin so MA_SENSOR_BACK returns there -- but ONLY on a
       * genuine EXTERNAL entry (the DEVICES list, the OLD DEVICES list, or
       * the main screen's info-block shortcut). The marker/colour/label/cal
       * sub-screens also re-enter MENU_SENSOR via MA_SENSOR, and capturing
       * the origin then would clobber it with the sub-screen's menu. So
       * capture only on those three; any other current menu is an internal
       * round-trip and leaves the origin untouched. */
      if (g_menu == MENU_NONE || g_menu == MENU_DEVICES ||
          g_menu == MENU_OLDDEV)
         g_sensor_from = g_menu;
      g_sel  = action - MA_SENSOR;
      g_menu = (g_sel < g_nslot) ? MENU_SENSOR : MENU_DEVICES;
   } else if (action == MA_SENSOR_BACK) {
      g_menu = g_sensor_from; /* back to where it was opened from */
      g_sel  = -1;
   } else if (action == MA_ADDSENSOR) {
      /* The ADD DEVICE type picker shares MA_SENSOR_BACK for its X, so
       * record where it was opened from -- ONLY on external entry (the
       * DEVICES screen, the main-screen '+' ADD menu, or a main-screen entry
       * point). METERHELP re-enters this to go back to the picker and must
       * NOT reset the origin. */
      if (g_menu == MENU_NONE || g_menu == MENU_DEVICES || g_menu == MENU_ADD)
         g_sensor_from = g_menu;
      g_menu = MENU_SENSTYPE;
   } else if (action >= MA_TYPE && action < MA_TYPE + SENSOR_NTYPES) {
      /* The ADD menu's DEVICES section enters here DIRECTLY (no
       * MA_ADDSENSOR hop records the origin), so record it now: the flow
       * must fall back to the MAIN screen, not into settings. */
      if (g_menu == MENU_ADD)
         g_sensor_from = MENU_ADD;
      /* Every abort inside the pairing flow (keypad X, device-list
       * cancel) returns to the EXACT screen the type was tapped on. */
      g_pair_from = (g_menu == MENU_ADD) ? MENU_ADD : MENU_SENSTYPE;
      g_add_type  = action - MA_TYPE;
      /* A CGM pairs with a code on the keypad; a meter bonds at the OS
       * level, so it only has to be discovered. */
      /* Where the flow lands when it finishes (or is closed): back into the
       * DEVICES screen only if that is where it was entered from -- an
       * ADD-menu or main-screen entry must fall back to the MAIN screen, not
       * into a menu the user never opened. */
      int kp_ret = (g_sensor_from == MENU_DEVICES) ? MENU_DEVICES : MENU_NONE;
      if (sensor_kind(g_add_type) == KIND_CGM) {
         g_menu      = MENU_KEYPAD;
         g_kp_mode   = 0;
         g_kp_return = kp_ret;
         g_entrylen  = 0;
         pair_scan_start();
      } else {
         /* A meter must be woken and put into pairing mode by hand first, so
          * show instructions and DON'T scan yet -- the Scan button there
          * starts the scan (MA_METERSCAN). */
         g_menu      = MENU_METERHELP;
         g_kp_return = kp_ret;
      }
   } else if (action == MA_METERSCAN) {
      g_menu      = MENU_DEVLIST;
      g_kp_return = (g_sensor_from == MENU_DEVICES) ? MENU_DEVICES : MENU_NONE;
      pair_scan_start();
   } else if (action == MA_PRIMARY) {
      if (g_sel >= 0 && g_sel < g_nslot) {
         sensor_set_primary(g_sel);
         /* Re-bind the big number NOW, not on the next reading (up to 5 min
          * away): the primary owns it by contract, and if the new primary
          * has no data yet the display must clear rather than keep the
          * previous primary's value. Resolve the id before hist_lock (reg ->
          * hist). */
         int prime = sensor_primary_id();
         hist_lock();
         hist_refresh_current(prime);
         hist_unlock();
         g_notify_dirty = 1; /* the notification mirrors the big number */
      }
   } else if (action >= MA_PRIM_PICK && action < MA_PRIM_PICK + MAX_SLOTS) {
      /* The DEVICES screen's PRIMARY checkbox: the tap IS the switch, and the
       * box filling in is the feedback -- the screen STAYS open, unlike the
       * per-device PRIMARY row, because picking from a column invites
       * comparing before committing. sensor_set_primary re-validates the index
       * and refuses a meter or an old device, so a slot table that shifted
       * since the render cannot promote one. */
      sensor_set_primary(action - MA_PRIM_PICK);
      int prime = sensor_primary_id();
      hist_lock();
      hist_refresh_current(prime);
      hist_unlock();
      g_notify_dirty = 1; /* the notification mirrors the big number */
   } else if (action == MA_RECONNECT) {
      /* Direct revive if the sensor was pulled BEFORE expiry; otherwise a
       * confirmation, since reconnecting a dead sensor just waits forever.
       */
      if (g_sel >= 0 && g_sel < g_nslot) {
         if (old_slot_expired(g_sel))
            g_menu = MENU_RECONF;
         else
            do_reconnect(g_sel);
      }
   } else if (action == MA_RECON_YES) {
      do_reconnect(g_sel);
   } else if (action == MA_PEND_CANCEL) {
      if (g_pend_pairing) {
         LOGI("pending pairing cancelled by user");
         g_pend_pairing = 0;
         set_status("PAIRING CANCELLED");
      }
   } else if (action == MA_WEAR) {
      /* Cycle this device's wear budget: AUTO -> 10 D -> 15 D -> AUTO.
       *
       * THREE STATES, NOT TWO, AND AUTO MUST BE REACHABLE. This was a
       * 10 <-> 15 toggle, so the first tap wrote an explicit override and
       * NOTHING could ever remove it again: sensor_wear_seconds gives a pin
       * absolute priority, which permanently disabled the model resolution
       * for that device. A G7 paired on 2026-07-23 -- one day before the
       * SW14758 (G7 15 Day) model rule existed -- carried a pin of 10 from
       * that era, so once the rule landed the app went on counting a 10-day
       * budget for a sensor it could now positively identify as 15-day,
       * declaring it nearly finished with five days left. One accidental tap
       * on a full-width row was enough to cause it, and there was no way
       * back. Preferences only -- no radio, no provenance. */
      if (g_sel >= 0 && g_sel < g_nslot) {
         sensors_lock();
         int w = g_slot[g_sel].wear_days;
         if (w == 10)
            g_slot[g_sel].wear_days = 15;
         else if (w == 15)
            g_slot[g_sel].wear_days = 0; /* back to the model/type rule */
         else
            g_slot[g_sel].wear_days = 10;
         slots_save();
         sensors_unlock();
      }
   } else if (action == MA_FORGET) {
      if (g_sel >= 0 && g_sel < g_nslot)
         g_menu = MENU_FORGET; /* confirm first; this action changes nothing */
   } else if (action == MA_FORGET_YES) {
      if (g_sel >= 0 && g_sel < g_nslot) {
         /* Drops the slot only -- the provenance row stays, so historical
          * readings keep resolving to the sensor that actually made them. */
         LOGI("forgetting slot %d (id %d); history retained", g_sel,
              g_slot[g_sel].id);
         /* Release the BLE link BEFORE the slot array shifts. Leaving it
          * connected meant a forgotten sensor kept streaming on a link that
          * now belonged to a different slot ordinal -- and commit_pair would
          * later call driver_forget() on that same link, destroying the
          * SURVIVING sensor's bond. */
         const struct sensor_rec *fr = sensor_rec_by_id(g_slot[g_sel].id);
         int flink                   = link_for_slot(g_sel);
         /* BOTH bounds: flink indexes driver contexts and g_model_l below,
          * and the DIS block a few lines down already checks both. */
         if (fr && flink >= 0 && flink < LINK_MAX) {
            driver_lock();
            driver_select(flink);
            driver_forget();
            driver_select(LINK_CGM);
            driver_unlock();
            dexble_link_close(flink);
            /* Clear this link's cached DIS strings. Nothing else does, and
             * the re-read gate is "already non-empty -> never ask again" --
             * so the NEXT sensor to claim this link (there are only 4, and
             * replacing a sensor every 15 days forces reuse) would be minted
             * carrying the FORGOTTEN sensor's model and firmware, in a
             * provenance row that is never rewritten and whose fields are
             * part of the id-reuse key. */
            if (flink >= 0 && flink < LINK_MAX) {
               sensors_lock(); /* same lock the DIS callback writes under */
               g_model_l[flink][0] = 0;
               g_fw_l[flink][0]    = 0;
               sensors_unlock();
            }
            /* GIVE THE LINK BACK. Nothing else here did, and for a METER
             * that leaked the link permanently: the armed table kept the
             * forgotten meter's MAC and the routing bit stayed set, which
             * excludes the link from BOTH pools -- meter_alloc_link skips
             * any link with g_link_armed set, free_cgm_link skips
             * g_link_armed || g_link_meter. Nothing ever cleared them,
             * because the only thing that does is meter_release_link on the
             * disconnect callback, and dexble_link_close on a connect that
             * is merely PENDING (a meter is switched off between
             * fingersticks, so that is the normal state) never produces one.
             * Every meter retired therefore burned one of LINK_MAX links for
             * the life of the process.
             *
             * UN-ARM, DO NOT RELEASE. An earlier version called
             * meter_release_link here, which clears the routing bit -- and a
             * disconnect callback still in flight (the meter really was
             * connected when forgotten) then landed in the CGM branch and
             * posted a spurious CONNECTION ERROR. The split exists for
             * exactly this: un-arm frees the meter pool now, the routing bit
             * stays until the callback lands (pancra_meter_disconnected
             * releases fully), and if no callback is coming -- the normal
             * case, a PENDING connect on a switched-off meter -- the
             * teardown stamp below lets the stranded-link recovery in
             * meter_sync_watchdog give the link back after
             * METER_TEARDOWN_MAX. A 3-minute bounded hold instead of a
             * permanent leak, and no misrouting either way.
             *
             * The tick cannot re-arm meanwhile: it skips retired slots, and
             * this slot is retired two lines down.
             *
             * For a CGM all of this is a no-op: the armed entry is empty,
             * the meter bit is 0, and the stamp is gated on that bit. */
            meter_unarm_link(flink);
            driver_lock();
            int fmeter = g_link_meter[flink];
            driver_unlock();
            if (fmeter)
               g_link_idle_t[flink] = realtime_s();
         }
         /* RETIRE, do not delete: the slot is kept (marker, label, prefs)
          * and flagged old, so it moves to OLD DEVICES with its full
          * per-device menu intact. sensor_retire_slot reassigns the primary
          * to the first live CGM left. */
         sensor_retire_slot(g_sel);
         /* Re-bind the big number to the new owner immediately, clearing it
          * if no eligible live sample exists -- the disconnected sensor's
          * value must not stay latched on screen. */
         int prime = sensor_primary_id();
         hist_lock();
         hist_refresh_current(prime);
         hist_unlock();
         g_notify_dirty = 1;
         g_sel          = -1;
         /* Return to WHERE THE SENSOR SCREEN WAS OPENED FROM, not a
          * hardcoded SETTINGS: the detail screen is reachable from the
          * main-screen STATE/SESSION table (g_sensor_from == MENU_NONE) as
          * well as from the settings DEVICES list, and disconnecting from
          * the former must land back on the main screen, not somewhere the
          * user never was. */
         g_menu = g_sensor_from;
      }
   } else if (action == MA_SYNC) {
      /* SYNC NOW CONNECTS. It used to do nothing whatsoever.
       *
       * All three of its old statements were no-ops by the time they ran:
       *
       *   - it cleared g_meter_last_sync "so the next advertisement syncs
       *     immediately", but the per-meter throttle refactor moved the gate
       *     to meter_rt.sync_t and left that global read by NOTHING. Grep
       * it: one declaration, one write here, one write on completion, zero
       *     reads. The throttle it claimed to clear was untouched.
       *   - it called start_scan, which early-returns whenever g_scanning is
       *     set -- always, while the UI is up. See scan_restart.
       *   - and it never attempted a connection at all. The whole design
       * waits passively for an advertisement, so with the meter switched on
       * but between advertising bursts, the one control the user has to
       * force a sync did literally nothing, silently, however many times it
       * was pressed. Confirmed from an HCI capture on 2026-08-03: 26 minutes
       *     of taps with no radio traffic to the meter at all, then a sync
       *     1.3 s after the first advertisement finally arrived.
       *
       * A registered meter is BONDED and its address is known, and
       * dexble_meter_connect uses autoConnect=true -- so there is no reason
       * whatsoever to wait for an advertisement. Connect, and let the stack
       * latch on when the meter is reachable. The 90 s watchdog releases the
       * link if it never is. */
      int mid = -1;
      char mmac[24];
      mmac[0] = 0;
      sensors_lock();
      if (g_sel >= 0 && g_sel < g_nslot && !g_slot[g_sel].old) {
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[g_sel].id);
         if (r && sensor_kind(r->type) == KIND_BGM) {
            mid = g_slot[g_sel].id;
            str_snapshot(mmac, sizeof mmac, r->identity);
         }
      }
      sensors_unlock();
      if (mid > 0 && mmac[0]) {
         if (g_meter_busy) {
            /* SAY SO. Refusing is right -- meter_sync_start resets the otble
             * statics, and doing that under a sync in flight writes one
             * meter's records under another's id -- but refusing SILENTLY is
             * what this whole handler was already guilty of. */
            LOGI("manual sync refused: a meter sync is already in flight");
            set_status("METER BUSY, RETRY");
         } else {
            LOGI("manual sync: connecting to meter %s (id %d)", mmac, mid);
            set_status("METER: SYNCING");
            meter_sync_start(mid, mmac);
         }
      }
      /* Refresh the scan as well, for the CGM's sake and to recover one
       * Android has quietly demoted (scan_restart). Deliberately NOT
       * pair_scan_start(): that sets g_smart_pairing, which suppresses every
       * CGM's advert-driven reconnect and is never cleared from this screen,
       * so a single SYNC NOW tap used to kill CGM reconnection for the life
       * of the process. */
      devlist_lock(); /* atomic vs the binder-thread advert writer */
      g_ndevs = 0;
      devlist_unlock();
      scan_restart(g_act);
   }
   /* --- calibration: user-initiated only, never automatic --- */
   else if (action == MA_CAL_OPEN) {
      /* If a calibration for THIS sensor is still queued, show the pending
       * screen (REPLACE / CANCEL) rather than silently starting another. */
      int pend = (g_calq_mgdl > 0 && g_sel >= 0 && g_sel < g_nslot &&
                  g_calq_id == g_slot[g_sel].id);
      if (pend) {
         g_menu = MENU_CALPEND;
      } else {
         /* Straight to the value keypad (like PLOT MAX); cancel returns to
          * the device menu. The old read-only bounds panel is gone. */
         g_menu        = MENU_KEYPAD;
         g_kp_mode     = 2;
         g_kp_return   = MENU_SENSOR;
         g_entrylen    = 0;
         g_cal_pending = 0;
      }
   } else if (action == MA_CAL_REPLACE) {
      /* Enter a new value; on CONFIRM it supersedes the queued one. */
      g_menu        = MENU_KEYPAD;
      g_kp_mode     = 2;
      g_kp_return   = MENU_SENSOR;
      g_entrylen    = 0;
      g_cal_pending = 0;
   } else if (action == MA_CAL_CANCEL) {
      /* Discard the queued calibration entirely. */
      LOGI("queued calibration %d mg/dL cancelled by user", g_calq_mgdl);
      calq_clear();
      g_calq_status[0] = 0;
      g_menu           = MENU_SENSOR;
   } else if (action == MA_RESCALE_OPEN && g_sel >= 0 && g_sel < g_nslot &&
              ((g_rescale_pm != 1000 && g_rescale_id == g_slot[g_sel].id) ||
               (g_rescale_pend_mgdl > 0 &&
                g_rescale_pend_id == g_slot[g_sel].id))) {
      /* Active OR pending for this sensor: show the active/pending screen
       * (CHANGE / STOP). */
      g_menu = MENU_RESCALEACT;
   } else if (action == MA_RESCALE_OPEN || action == MA_RESCALE_CHANGE) {
      /* Not active (or explicitly changing): go to the value keypad. */
      g_menu          = MENU_KEYPAD;
      g_kp_mode       = 3;
      g_kp_return     = MENU_SENSOR;
      g_entrylen      = 0;
      g_rescale_entry = 0;
   } else if (action == MA_RESCALE_STOP) {
      LOGI("rescaling turned OFF by user (was %d permille, pend %d)",
           g_rescale_pm, g_rescale_pend_mgdl);
      g_rescale_pm        = 1000;
      g_rescale_id        = 0;
      g_rescale_t         = 0;
      g_rescale_pend_mgdl = 0; /* also discard any pending target */
      g_rescale_pend_id   = 0;
      g_rescale_pend_t    = 0;
      g_rescale_reject    = 0; /* and clear any rejection / expiry notice */
      g_rescale_expired   = 0;
      rescale_save();
      g_menu = MENU_SENSOR;
   } else if (action == MA_RESCALE_ENTER) {
      /* CONFIRM: compute the factor from the entered true value over the
       * live RAW reading, clamp to +-25%, activate for this sensor from NOW.
       * If there is NO live reading yet, HOLD the target (persisted) -- the
       * next reading for this sensor computes the factor. It is never
       * silently lost.
       */
      if (g_rescale_entry > 0 && g_sel >= 0 && g_sel < g_nslot) {
         int id   = g_slot[g_sel].id;
         int link = link_for_slot(g_sel);
         int raw  = (link >= 0 && link < LINK_MAX) ? g_link_raw[link] : 0;
         /* A fresh attempt supersedes any prior rejection / expiry notice.
          */
         g_rescale_reject  = 0;
         g_rescale_expired = 0;
         if (raw > 0) {
            rescale_activate(id, g_rescale_entry, raw, realtime_s());
            g_rescale_pend_mgdl = 0;
            g_rescale_pend_id   = 0;
            g_rescale_pend_t    = 0;
         } else {
            g_rescale_pend_mgdl = g_rescale_entry;
            g_rescale_pend_id   = id;
            g_rescale_pend_t    = realtime_s(); /* for the freshness window */
            LOGI("rescale %d mg/dL queued: awaiting a reading to compute "
                 "factor",
                 g_rescale_entry);
         }
         rescale_save();
      }
      g_rescale_entry = 0;
      g_menu          = MENU_SENSOR;
   } else if (action == MA_CAL_BACK || action == MA_FORGET_NO ||
              action == MA_RESCALE_BACK || action == MA_RECON_NO) {
      g_menu = MENU_SENSOR; /* these sub-screens back out to the sensor */
   } else if (action == MA_CAL_REFRESH) {
      if (cal_select()) {
         driver_cal_bounds();
         driver_select(LINK_CGM);
         driver_unlock();
      }
   } else if (action == MA_CAL_ENTER) {
      /* CONFIRM: QUEUE the calibration durably (persisted), then try once
       * now. It is NOT dropped if the sensor is not streaming this instant
       * -- it stays queued and every subsequent reading retries it (see
       * calq_try_locked in pancra_glucose) until the sensor accepts or the
       * freshness window lapses, and the outcome is always shown. This is
       * the fix for a confirmed calibration being silently lost to a
       * reconnect gap.
       */
      if (g_cal_pending > 0 && g_sel >= 0 && g_sel < g_nslot) {
         g_calq_mgdl = g_cal_pending;
         g_calq_id   = g_slot[g_sel].id;
         g_calq_t    = realtime_s();
         g_calq_sent = 0;
         (void)snprintf(g_calq_status, sizeof g_calq_status, "PENDING %d",
                        g_calq_mgdl);
         calq_save();
         LOGI("calibration QUEUED: %d mg/dL (slot %d, id %d)", g_calq_mgdl,
              g_sel, g_calq_id);
         if (cal_select()) { /* opportunistic first attempt while we are here
                              */
            calq_try_locked();
            driver_select(LINK_CGM);
            driver_unlock();
         }
      }
      g_cal_pending = 0;
      g_menu        = MENU_SENSOR;
   } else if (action == 20) { /* battery optimisation: request if optimised,
                                 else settings */
      if (g_sys_batt)
         sys_open_settings();
      else
         sys_request_battery();
      sys_refresh();
   } else if (action == 22) {
      sys_open_settings();
      sys_refresh();
   } /* bg-exec: change in settings */
   else if (action >= MA_PERM && action < MA_PERM + NPERMS) {
      /* denied -> request dialog; granted -> app settings (only place to
       * revoke) */
      if (g_sys_perm[action - 10])
         sys_open_settings();
      else
         sys_request_perm(perms[action - 10]);
      sys_refresh();
   } else if (action == 99) {
      g_menu = MENU_NONE;
      sys_set_orientation(g_orient);
   } /* apply orient */
   /* --- keypad (opened from settings rows: return there on close) --- */
   else if (action == 30) {
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 0;
      g_kp_return = MENU_SETTINGS;
      g_entrylen  = 0;
      pair_scan_start(); /* scan under the code entry to hide the delay */
   } else if (action == 31) {
      g_menu      = MENU_KEYPAD;
      g_kp_mode   = 1;
      g_kp_return = MENU_DISPLAY; /* PLOT MAX now lives on DISPLAY */
      g_entrylen  = 0;
   } else if (action >= MA_DIGIT && action < MA_DIGIT + 10) { /* digit */
      int cap = ui_kp_slots(g_kp_mode);
      if (g_entrylen < cap)
         g_entry[g_entrylen++] = (char)('0' + (action - 100));
   } else if (action == MA_DOT) {
      /* '.' exists on the remote-IP keypad and, in mmol/L mode only, on the
       * four threshold keypads -- ALARM LOW/HIGH and NUDGE LOW/HIGH (one
       * dot, one decimal digit -- "5.5"). The guard keeps a stale tap
       * (racing a repaint) from injecting one into any other numeric entry.
       *
       * The RANGE must match ui.c's kp_thresh(). Left at 10/11 the renderer
       * drew a dot key on the NUDGE keypads that this branch then ignored --
       * a visible, tappable, DEAD key, and with it no way to enter a nudge
       * threshold at all in mmol/L. */
      if (g_menu == MENU_KEYPAD &&
                 ((g_units && g_kp_mode >= 10 && g_kp_mode <= 13) ||
                  g_kp_mode == 14) &&
                 g_entrylen < ui_kp_slots(g_kp_mode)) {
         int seen = 0;
         for (int i = 0; i < g_entrylen; i++)
            if (g_entry[i] == '.')
               seen = 1;
         if (!seen)
            g_entry[g_entrylen++] = '.';
      }
   } else if (action == 110) {
      if (g_entrylen > 0)
         g_entrylen--;
   } /* backspace */
   else if (action == 113) {
      /* ONLY the pairing keypad backs out to the ADD DEVICE type picker. The
       * label editor and the plot-max / calibration keypads share this close
       * code -- gate on the actual menu, or renaming a device (MENU_LABEL)
       * and cal/plot-max entry wrongly landed on ADD DEVICE instead of their
       * own return target. */
      int was_pairing = (g_menu == MENU_KEYPAD && g_kp_mode == 0);
      if (g_smart_pairing)
         pair_cancel(); /* abandon pairing, keep the old bond */
      if (was_pairing)
         g_menu = g_pair_from; /* the screen the type was tapped on */
      else
         keypad_close();
   } /* X -> close */
   else if (action == 199) { /* device list: cancel -> where pairing began */
      pair_cancel();
      g_menu = g_pair_from;
   } else if (action >= MA_DEV_PICK &&
              action < MA_DEV_PICK + MAX_DEVS) { /* device list: pick */
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
      int idx = action - 200;
      devlist_lock(); /* consistent (mac, name) vs the binder-thread writer
                       */
      if (g_menu == MENU_DEVLIST && idx >= 0 && idx < g_ndevs) {
         str_snapshot(g_pend_mac, sizeof g_pend_mac, g_devs[idx].mac);
         str_snapshot(g_pend_name, sizeof g_pend_name, g_devs[idx].name);
         g_menu = MENU_PAIRCONF;
      }
      devlist_unlock();
   } else if (action == MA_PAIR_YES) {
      /* THE consequential step, reached only from an explicit YES. Gate on
       * the menu still being open, like the pick itself: a tap racing a
       * repaint must not commit twice or from nowhere. */
      if (g_menu == MENU_PAIRCONF && g_pend_mac[0]) {
         char macbuf[sizeof g_pend_mac];
         str_snapshot(macbuf, sizeof macbuf, g_pend_mac);
         g_pend_mac[0]  = 0;
         g_pend_name[0] = 0;
         commit_pair(macbuf);
      }
   } else if (action == MA_PAIR_NO) {
      if (g_menu == MENU_PAIRCONF) {
         g_pend_mac[0]  = 0;
         g_pend_name[0] = 0;
         g_menu = MENU_DEVLIST; /* nothing committed: back to the list */
      }
   } else if (action == MA_ADD_OPEN) {
      g_menu = MENU_ADD;
   } else if (action == MA_INSLOG_BACK) {
      /* The log's X returns where it opened -- which is the ADD menu, OR the
       * main screen when a shortcut button opened it. The comment here always
       * claimed "where it opened" while the code named one fixed menu. */
      g_menu = g_inslog_from;
   } else if (action == MA_STATBAR || action == MA_LOCKSCR ||
              action == MA_NOTIF_REOPEN) {
      if (action == MA_STATBAR)
         g_statbar_val = !g_statbar_val;
      else if (action == MA_LOCKSCR)
         g_lockscr_val = !g_lockscr_val;
      if (action != MA_NOTIF_REOPEN)
         settings_save();
      /* All three re-post the notification immediately: the toggles so
       * the change is visible at once, REOPEN because re-posting IS the
       * action (a swiped-away notification returns on notify()). */
      g_notify_dirty = 1;
      pancra_notify_refresh();
   } else if (ins_action(action) || wt_action(action) || style_action(action) ||
              submenu_action(action)) {
      /* handled by a split-out family (see them above menu_action):
       * LOG/EDIT INSULIN + dose log + marker picker (ins_action), or the
       * ALARM / EXPORT DATA / REMOTE / PERMISSIONS / DISPLAY / OLD DEVICES
       * submenus (submenu_action) */
   } else if (action == MA_OK) {
      if (g_menu == MENU_LABEL && g_label_field == LABEL_EMAIL) {
         /* The account email. Lower-cased for the same reason as the server,
          * and required to look like an address at all -- a typo here fails
          * pairing with a message about the code, which is the wrong thing to
          * go looking at. */
         char em[sizeof g_sync_email];
         int n = g_entrylen < (int)sizeof em - 1 ? g_entrylen
                                                 : (int)sizeof em - 1;
         int at = 0, dot_after_at = 0;
         for (int i = 0; i < n; i++) {
            char c = g_entry[i];
            em[i]  = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            if (em[i] == '@')
               at++;
            else if (em[i] == '.' && at)
               dot_after_at = 1;
         }
         em[n] = 0;
         if (n < 5 || at != 1 || !dot_after_at || em[0] == '@' ||
             em[n - 1] == '@' || em[n - 1] == '.') {
            LOGI("account '%s' does not look like an address, not saved", em);
            g_entrylen = 0;
            g_ui_dirty = 1;
            return;
         }
         for (int i = 0;; i++) {
            g_sync_email[i] = em[i];
            if (!em[i])
               break;
         }
         remote_save();
         g_entrylen = 0;
         g_menu     = MENU_REMOTE;
      } else if (g_menu == MENU_LABEL && g_label_field == LABEL_SERVER) {
         /* SERVER. Lower-cased because the editor can only type upper case and
          * a host name reads wrong shouted. Malformed input refuses VISIBLY --
          * the entry is cleared and the editor stays open -- because silently
          * storing a bad server would point every future sync at nothing. */
         char host[sizeof g_remote_server];
         int n = g_entrylen < (int)sizeof host - 1 ? g_entrylen
                                                   : (int)sizeof host - 1;
         for (int i = 0; i < n; i++) {
            char c  = g_entry[i];
            host[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
         }
         host[n] = 0;
         if (!remote_server_valid(host)) {
            LOGI("server '%s' malformed, not saved", host);
            g_entrylen = 0;
            g_ui_dirty = 1;
            return;
         }
         for (int i = 0;; i++) {
            g_remote_server[i] = host[i];
            if (!host[i])
               break;
         }
         remote_save();
         /* A DIFFERENT server holds a different record: whatever we knew about
          * what it already had is meaningless now. */
         remote_forget_cursor();
         g_entrylen = 0;
         g_menu     = MENU_REMOTE;
      } else if (g_menu == MENU_LABEL) {
         if (g_sel >= 0 && g_sel < g_nslot) {
            int k = 0;
            sensors_lock();
            for (; k < g_entrylen && k < (int)sizeof g_slot[0].label - 1; k++)
               g_slot[g_sel].label[k] = g_entry[k];
            g_slot[g_sel].label[k] = 0;
            /* an all-blank name would make the row unreadable, so fall back
             */
            if (k == 0)
               (void)snprintf(g_slot[g_sel].label, sizeof g_slot[0].label,
                              "SENSOR %d", g_slot[g_sel].id);
            slots_save();
            sensors_unlock();
         }
         g_entrylen = 0;
         g_menu     = MENU_SENSOR;
      } else if (g_kp_mode == 2) { /* CALIBRATION: entry is in display units */
         if (g_entrylen > 0) {
            /* Conversion and bound live in alarmlogic.c so `make check` can
             * fail on them; this branch only actuates. */
            /* mmol/L is entered as tenths (e.g. "78" = 7.8), so scale back
             * to mg/dL the same way the plot-max entry does. */
            int mgdl = cal_entry_mgdl(g_entry, g_entrylen, g_units);
            /* Out of range: refuse VISIBLY. Do NOT clamp -- silently
             * altering a calibration value the user typed is worse than not
             * accepting it. Previously the driver refused with only a log
             * line while the keypad closed and SCR_CAL still showed the
             * PREVIOUS result, so a rejected entry looked exactly like a
             * successful one. Staying on the keypad with the entry cleared
             * is the feedback: nothing was submitted, retype it. Easy to hit
             * in mmol/L (2.2 -> 39 mg/dL). */
            if (mgdl < 0) {
               LOGI("calibration %d mg/dL out of range 40..400, not "
                    "submitted",
                    mgdl);
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* stay on the keypad: the cleared entry IS the
                        * feedback
                        */
            }
            /* The single most consequential write in the app, so it happens
             * only here: a digit typed by the user, then an explicit OK. */
            /* Do NOT write yet -- stash the value and show a confirmation.
             * The actual (consequential) calibration write happens only on
             * the explicit CONFIRM (MA_CAL_ENTER). */
            g_entrylen    = 0;
            g_cal_pending = mgdl;
            keypad_close();
            g_menu = MENU_CAL;
         }
      } else if (g_kp_mode == 3) { /* RESCALE: a true glucose value (display
                                      units), like calibration */
         if (g_entrylen > 0) {
            int mgdl = cal_entry_mgdl(g_entry, g_entrylen, g_units);
            if (mgdl < 0) {
               LOGI("rescale %d mg/dL out of range, not submitted", mgdl);
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* stay on the keypad: cleared entry is the feedback
                        */
            }
            g_entrylen      = 0;
            g_rescale_entry = mgdl; /* factor computed on CONFIRM */
            keypad_close();
            g_menu = MENU_RESCALE;
         }
      } else if (g_kp_mode >= 10 && g_kp_mode <= 13) {
         /* ALARM LOW / HIGH (10/11) and NUDGE LOW / HIGH (12/13): entry in
          * DISPLAY units. mg/dL is a plain integer; mmol/L is LITERAL mmol
          * with an optional '.' and one decimal digit ("5.5") -- its keypad
          * shows a dot key (ui.c).
          * All four accept 0..AL_ENTRY_MAX: 0 parks LOW below any possible
          * reading and a past-the-scale HIGH above any, each threshold's
          * deliberate OFF switch. Refuse VISIBLY (stay on the keypad, entry
          * cleared) a malformed entry, an out-of-range value, or one that
          * would invert ITS OWN pair -- a silent clamp would move a
          * threshold the user never typed. Equal is allowed.
          *
          * The two pairs are checked against THEMSELVES only, never against
          * each other. A nudge inside the alarm band is pointless but
          * harmless (nudge_fire suppresses it under the alarm), and refusing
          * the entry would block the legitimate order of operations -- move
          * the nudge first, then the alarm -- for no safety gain. */
         if (g_entrylen > 0) {
            int isnudge = g_kp_mode >= 12;
            int islow   = (g_kp_mode % 2) == 0;
            int ip      = 0;
            int fd      = 0;
            int dot     = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
            int bad     = 0;
            for (int i = 0; i < g_entrylen; i++) {
               char ch = g_entry[i];
               if (ch == '.') {
                  if (dot || !g_units)
                     bad = 1; /* one dot, and only in mmol/L mode */
                  else
                     dot = 1;
               } else if (dot == 0) {
                  ip = (ip * 10) + (ch - '0');
               } else if (dot == 1) {
                  fd  = ch - '0';
                  dot = 2;
               } else {
                  bad = 1; /* a second decimal digit: not representable */
               }
            }
            int mgdl = g_units ? (((ip * 10) + fd) * 18) / 10 : ip;
            alarm_lock();
            int lo = isnudge ? g_nudge_low : g_alarm_low;
            int hi = isnudge ? g_nudge_high : g_alarm_high;
            alarm_unlock();
            /* A HIGH of 0 is refused outright, even when LOW is parked at
             * its documented 0 off-switch. `mgdl < lo` alone accepted it,
             * and HIGH=0 means every reading is above the threshold: a
             * continuous alarm the user has to work out how to undo. Loud
             * rather than silent, but the entry path is documented to refuse
             * values it cannot honour, and this is one. LOW=0 stays legal --
             * that is the off switch. */
            if (bad || mgdl > AL_ENTRY_MAX || (!islow && mgdl <= 0) ||
                (islow ? mgdl > hi : mgdl < lo)) {
               LOGI("%s %s %d mg/dL refused (0..%d, low<=high)",
                    isnudge ? "nudge" : "alarm", islow ? "low" : "high", mgdl,
                    AL_ENTRY_MAX);
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* cleared entry is the feedback; retype it */
            }
            /* Under alarm_lock: the pair is READ under it by the alarm
             * evaluators, and a mixed old/new pair can invert the range for
             * one tick (see the old alarm_adjust's rationale). The nudge
             * pair is read there too, on the same tick, so it takes the same
             * lock.
             */
            alarm_lock();
            if (isnudge && islow)
               g_nudge_low = mgdl;
            else if (isnudge)
               g_nudge_high = mgdl;
            else if (islow)
               g_alarm_low = mgdl;
            else
               g_alarm_high = mgdl;
            alarm_unlock();
            alarm_save();
            alarm_reeval(); /* a threshold move can itself enter/leave the
                             * alarmed state */
            g_entrylen = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 1) { /* PLOT MAX: entry is in the display unit */
         if (g_entrylen > 0) {
            int v = 0;
            for (int i = 0; i < g_entrylen; i++)
               v = (v * 10) + (g_entry[i] - '0');
            /* TENTHS of mmol/L, matching how the row is DISPLAYED: ui.c
             * renders plot max through fmt_glu, which prints one decimal in
             * mmol mode (300 mg/dL shows as "16.7"). Treating the entry as
             * whole mmol made the shown value impossible to re-enter --
             * typing 167 gave 3006 mg/dL (silently clamped to 400) and
             * typing 16 gave 288, not 300. The calibration entry below
             * already scales this way; this is the one that disagreed with
             * its own display. */
            int mgdl = g_units ? (v * 18) / 10 : v;
            if (mgdl < 100)
               mgdl = 100;
            if (mgdl > 400)
               mgdl = 400;
            g_plot_max = mgdl;
            plot_set_max(mgdl);
            settings_save();
            keypad_close();
            /* the notification plot shares this vertical scale; without a
             * refresh it keeps the old one until the next datapoint */
            g_notify_dirty = 1;
            pancra_notify_refresh();
         }
      } else if (g_kp_mode == 15) { /* SYNC: the server's 6-digit code */
         if (g_entrylen == 6) {
            char code[8];
            for (int i = 0; i < 6; i++)
               code[i] = g_entry[i];
            code[6] = 0;
            /* Handed to Java's worker: pairing is four round trips and must
             * not run on the UI thread. The result arrives as a changed
             * PAIRED row, because the only thing the user can do about a
             * failure is ask the server for a fresh code. */
            syncjni_pair_request(g_sync_email, code);
            g_entrylen = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 5) { /* REMOTE PORT: 1..65535 */
         if (g_entrylen > 0) {
            int v = 0;
            for (int i = 0; i < g_entrylen; i++)
               v = (v * 10) + (g_entry[i] - '0'); /* max 5 digits: no wrap */
            if (v < 1 || v > 65535) {
               LOGI("remote port %d out of range, not saved", v);
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* stay on the keypad: cleared entry is the feedback
                        */
            }
            g_remote_port = v;
            remote_save();
            remote_forget_cursor(); /* possibly a different server */
            g_entrylen = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 14) { /* WEIGHT: "162" or "162.4" */
         if (g_entrylen > 0) {
            /* THE DIGITS ARE THE WHOLE NUMBER, with an optional '.' and one
             * decimal -- exactly the alarm-threshold entry's shape.
             *
             * They used to be TENTHS, so "162" meant 16.2 lb: below the
             * minimum, refused, entry cleared, and the only way to enter 162
             * was to type "1620". Nobody would. An entry form has to accept
             * the number as it is spoken and as the row displays it. */
            int ip  = 0;
            int fd  = 0;
            int dot = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
            int bad = 0;
            for (int i = 0; i < g_entrylen; i++) {
               char ch = g_entry[i];
               if (ch == '.') {
                  if (dot)
                     bad = 1; /* one dot only */
                  else
                     dot = 1;
               } else if (dot == 0) {
                  ip = (ip * 10) + (ch - '0');
               } else if (dot == 1) {
                  fd  = ch - '0';
                  dot = 2;
               } else {
                  bad = 1; /* a second decimal digit: not representable */
               }
            }
            int tenths = (ip * 10) + fd;
            /* Validate by CONVERTING: wt_from_tenths returns 0 outside the
             * stored range, so an impossible weight is refused VISIBLY
             * rather than silently clamped into the log. */
            if (bad || ip > 999 || wt_from_tenths(tenths, g_wunits) <= 0) {
               LOGI("weight %d.%d %s refused (out of range)", tenths / 10,
                    tenths % 10, wt_unit_name(g_wunits));
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* stay: the cleared entry is the feedback */
            }
            g_wt_tenths = tenths;
            g_entrylen  = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 6) { /* INSULIN UNITS: 1..99 */
         if (g_entrylen > 0) {
            int v = 0;
            for (int i = 0; i < g_entrylen; i++)
               v = (v * 10) + (g_entry[i] - '0');
            if (v < INS_UNITS_MIN || v > INS_UNITS_MAX) {
               g_entrylen = 0;
               g_ui_dirty = 1;
               return; /* stay: cleared entry is the refusal */
            }
            g_ins_units = v;
            g_entrylen  = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 9) { /* YEAR: 4 digits */
         if (g_entrylen == 4) {
            int v = 0;
            for (int i = 0; i < 4; i++)
               v = (v * 10) + (g_entry[i] - '0');
            /* Modes 7/8/9 are about a calendar instant and carry no insulin
             * meaning, so the LOG WEIGHT form reuses them. g_kp_return says
             * which form is waiting, and therefore which instant to edit --
             * without this, typing a date on the weight form silently moved
             * the insulin form's dose instead. */
            long *tp   = (g_kp_return == MENU_WEIGHT) ? &g_wt_t : &g_ins_t;
            long local = *tp + g_tz_off;
            long secs  = local % 86400;
            long z     = local / 86400;
            if (secs < 0) {
               secs += 86400;
               z--;
            }
            long yy = 0;
            long mm = 0;
            long dd = 0;
            civil_from_days(z, &yy, &mm, &dd);
            /* a dose belongs to a human timescale; refuse typo years */
            if (v < 2000 || v > 2199) {
               g_entrylen = 0;
               g_ui_dirty = 1;
               return;
            }
            /* keep month/day/time; clamp Feb 29 out of non-leap years */
            int leap = (v % 4 == 0 && v % 100 != 0) || v % 400 == 0;
            if (mm == 2 && dd == 29 && !leap)
               dd = 28;
            *tp        = (days_from_civil(v, mm, dd) * 86400) + secs - g_tz_off;
            g_entrylen = 0;
            keypad_close();
         }
      } else if (g_kp_mode == 7 || g_kp_mode == 8) { /* MMDD / HHMM */
         if (g_entrylen == 4) {
            int a = ((g_entry[0] - '0') * 10) + (g_entry[1] - '0');
            int b = ((g_entry[2] - '0') * 10) + (g_entry[3] - '0');
            /* whichever form opened the keypad -- see mode 9 above */
            long *tp = (g_kp_return == MENU_WEIGHT) ? &g_wt_t : &g_ins_t;
            /* split the instant into local civil date + seconds */
            long local = *tp + g_tz_off;
            long secs  = local % 86400;
            long z     = local / 86400;
            if (secs < 0) {
               secs += 86400;
               z--;
            }
            long yy = 0;
            long mm = 0;
            long dd = 0;
            civil_from_days(z, &yy, &mm, &dd);
            if (g_kp_mode == 7) { /* MMDD, within the current year */
               static const int mdl[12] = {31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31};
               int leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
               int md = (a >= 1 && a <= 12) ? mdl[a - 1] + (a == 2 && leap) : 0;
               if (a < 1 || a > 12 || b < 1 || b > md) {
                  g_entrylen = 0;
                  g_ui_dirty = 1;
                  return; /* invalid date: stay, entry cleared */
               }
               *tp = (days_from_civil(yy, a, b) * 86400) + secs - g_tz_off;
            } else { /* HHMM: keep the civil date, set the time of day */
               if (a > 23 || b > 59) {
                  g_entrylen = 0;
                  g_ui_dirty = 1;
                  return; /* invalid time: stay, entry cleared */
               }
               *tp = (z * 86400) + (a * 3600L) + (b * 60L) - g_tz_off;
            }
            g_entrylen = 0;
            keypad_close();
         }
      } else if (g_entrylen == 4) { /* PAIR: code in, now pick the sensor */
         for (int i = 0; i < 4; i++)
            g_code_str[i] = g_entry[i];
         g_code_str[4] = 0;
         code_save();
         int idx = select_candidate();
         if (idx >= 0 && fresh_candidates() == 1) {
            /* Exactly ONE sensor of this family on the air: pair it NOW.
             * The code plus a lone candidate is as unambiguous as it gets --
             * a confirmation list of one is ceremony, and the J-PAKE code
             * itself rejects a wrong device. */
            char macbuf[sizeof g_devs[0].mac];
            devlist_lock();
            str_snapshot(macbuf, sizeof macbuf, g_devs[idx].mac);
            devlist_unlock();
            commit_pair(macbuf);
         } else if (idx >= 0) {
            /* Several candidates, one clear by proximity: PROPOSE it. The
             * confirmation showing name + address is what lets the user
             * catch a wrong auto-pick before it costs a bond. */
            devlist_lock();
            str_snapshot(g_pend_mac, sizeof g_pend_mac, g_devs[idx].mac);
            str_snapshot(g_pend_name, sizeof g_pend_name, g_devs[idx].name);
            devlist_unlock();
            g_menu = MENU_PAIRCONF;
         } else {
            /* No candidate on the air yet: ARM the pairing and free the
             * user. Parking them in the device list until the sensor
             * advertised also kept g_smart_pairing latched, which suppresses
             * every OTHER sensor's reconnect -- waiting for the new sensor
             * cost the readings of the ones already worn. The 1 Hz tick
             * commits the moment an unambiguous candidate appears; DEVICES
             * shows the armed state as a PENDING row. */
            g_pend_pairing  = g_add_type;
            g_smart_pairing = 0; /* other sensors reconnect freely again */
            set_status("PAIRING PENDING");
            LOGI("pairing armed: awaiting a %s candidate",
                 sensor_type_name(g_add_type));
            keypad_close();
         }
      }
   }
   if (g_win)
      draw(g_win);
}

/* The menu_action code this screen's own title-row X emits -- the system
 * back gesture is a second finger on the SAME target, so navigation stays
 * single-sourced in menu_action and every recorded g_*_from origin keeps
 * working. Returns -1 when there is nothing to close (the main screen: back
 * deliberately does nothing there). KEEP IN STEP with the add_hit(...,
 * ACT_MENU, code) each render_* records on its title row. */
static int menu_back_code(void)
{
   switch (g_menu) {
      case MENU_SETTINGS:
      case MENU_ADD: return MA_CLOSE;
      case MENU_DEVICES: return MA_DEVICES_BACK;
      case MENU_KEYPAD: /* menu_action gates on kp_mode, exactly like the X
                         */
      case MENU_LABEL: return MA_KP_CLOSE;
      case MENU_DEVLIST: return MA_DEV_CANCEL;
      case MENU_SENSOR:
      case MENU_SENSTYPE: return MA_SENSOR_BACK;
      case MENU_CAL:
      case MENU_CALPEND: return MA_CAL_BACK;
      case MENU_RESCALE:
      case MENU_RESCALEACT: return MA_RESCALE_BACK;
      case MENU_FORGET: return MA_FORGET_NO;
      case MENU_MARKPICK:
      case MENU_COLORPICK:
         /* the combined picker's X: DISPLAY for an insulin type's styling,
          * the owning sensor's screen otherwise (same as render_markpick) */
         return (g_markpick_ins >= 0) ? MA_INSMARK_BACK
                                      : MA_SENSOR + (g_sel >= 0 ? g_sel : 0);
      case MENU_METERHELP: return MA_ADDSENSOR;
      case MENU_PAIRCONF: return MA_PAIR_NO;
      case MENU_INSULIN: return MA_INS_DISCARD;
      case MENU_INSDEL: return MA_INSDEL_NO;
      /* The three WEIGHT screens, missing since they were added: default
       * returns -1, and the caller CLAIMS the key handled either way, so back
       * was silently dead on all of them and the X was the only way out. The
       * LOG WEIGHT form is now what the weight keypad returns to, which puts
       * it in the middle of the shortest path in the app. */
      case MENU_WEIGHT: return MA_WT_DISCARD;
      case MENU_WTLOG: return MA_WTLOG_BACK;
      case MENU_WTDEL: return MA_WTDEL_NO;
      case MENU_ALARM: return MA_ALARM_BACK;
      case MENU_EXPORT: return MA_EXP_BACK;
      case MENU_INSLOG: return MA_INSLOG_BACK;
      case MENU_DISPLAY: return MA_DISPLAY_BACK;
      case MENU_PERMS: return MA_PERMS_BACK;
      case MENU_OLDDEV: return MA_OLDDEV_BACK;
      case MENU_RECONF: return MA_RECON_NO;
      case MENU_REMOTE: return MA_REMOTE_BACK;
      default: return -1; /* MENU_NONE */
   }
}

/* ---- act-on-release helpers (state at g_arm_*; policy comment there) ----
 */

/* DOWN on a control: arm it and repaint so the pressed shade shows. */
static void press_arm(int kind, int arg, int x, int y)
{
   g_arm_kind = kind;
   g_arm_arg  = arg;
   g_arm_in   = 1;
   g_arm_x    = x;
   g_arm_y    = y;
   if (g_win)
      draw(g_win);
}

/* Drop any armed press without firing (a stale arm must never shade or fire
 * on a later screen -- called when a DOWN lands on nothing, and by the back
 * key, which can change the screen under a held finger). */
static void press_cancel(void)
{
   g_arm_kind = ACT_NONE;
   g_arm_in   = 0;
}

/* MOVE: is the finger still on the armed control? Repaint only when the
 * answer changes, so a drag can't saturate the main thread with draws. */
static void press_track(int x, int y)
{
   if (g_arm_kind == ACT_NONE)
      return;
   struct action a = ui_hit(&g_hits, x, y);
   int in          = (a.kind == g_arm_kind && a.arg == g_arm_arg);
   if (in != g_arm_in) {
      g_arm_in = in;
      if (in) {
         g_arm_x = x;
         g_arm_y = y;
      }
      if (g_win)
         draw(g_win);
   }
}

/* UP/CANCEL: disarm, and say whether the action should fire -- an UP that
 * lands back on the armed control. A miss repaints to clear the shade (a
 * fired action repaints through its own path). Read g_arm_kind/g_arm_arg
 * BEFORE calling: this clears them. */
static int press_release(int up, int x, int y)
{
   if (g_arm_kind == ACT_NONE)
      return 0;
   struct action a = ui_hit(&g_hits, x, y);
   int fire        = up && a.kind == g_arm_kind && a.arg == g_arm_arg;
   g_arm_kind      = ACT_NONE;
   g_arm_in        = 0;
   if (!fire && g_win)
      draw(g_win);
   return fire;
}

/* Plot rectangle recorded by the last render (the ACT_SCRUB target), so a
 * drag can resolve to a datapoint even after the finger leaves the plot. */
static int plot_rect(int *x, int *y, int *w, int *h)
{
   for (int i = 0; i < g_hits.n; i++)
      if (g_hits.box[i].kind == ACT_SCRUB) {
         *x = g_hits.box[i].x;
         *y = g_hits.box[i].y;
         *w = g_hits.box[i].w;
         *h = g_hits.box[i].h;
         return 1;
      }
   return 0;
}

/* (The +- stepper's alarm_adjust is gone: thresholds are now typed on the
 * keypad -- see the kp_mode 10-13 branch in the MA_OK handler, which keeps
 * the same lock discipline: the pair is written under alarm_lock because the
 * evaluators read it under alarm_lock, and a mixed old/new pair can invert
 * the range for one tick.) */

/* an older reading recovered via backfill: store it, place it in history,
 * but don't disturb the current value unless it turns out to be the newest
 */
/* The meter's own record index, persisted so a reconnect never re-reads what
 * we already hold -- the meter has no idea what we have kept. */

/* Per-meter record index, keyed by SENSOR ID.
 *
 * A single shared index cannot serve two meters: their counters are
 * unrelated, so each sync looked like the other meter's counter had gone
 * backwards
 * ("memory cleared"), reset to -1, re-imported, and saved its own value --
 * leaving the pair oscillating forever. One meter never reached its own new
 * records again, so every fingerstick it took was silently lost, while the
 * other re-imported records it already had (re-appending them to the log,
 * since they are outside the in-memory dedup window) and held each meter
 * awake for a full walk on every advert.
 *
 * Stored as "id,index" lines, rewritten whole -- it is at most MAX_SLOTS
 * rows.
 */
static void meter_index_save(int id, int idx)
{
   if (id <= 0)
      return;
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   int n  = meter_index_all(ids, vals, MAX_SLOTS);
   int at = -1;
   for (int i = 0; i < n && at < 0; i++)
      if (ids[i] == id)
         at = i;
   if (at < 0 && n < MAX_SLOTS) {
      at      = n++;
      ids[at] = id;
   }
   if (at < 0) {
      /* Full. Rows are never pruned -- every id a meter has ever carried
       * keeps one -- so silently skipping the write meant this meter's index
       * was NEVER persisted again, and every later advert re-walked
       * OT_MAX_WALK records, re-appending weeks-old fingersticks to the
       * lifetime log.
       *
       * Evict a row NO LIVE SLOT REFERENCES. This used to drop ids[0] with a
       * comment claiming it "belongs to a superseded id that nothing reads"
       * -- an assumption the code did not check. Rows sit in the order they
       * were first written, so the oldest row is the FIRST meter ever
       * registered, and if that meter is still in use its index was the one
       * thrown away: its next sync sees no stored index, re-walks, and
       * re-appends fingersticks that are weeks old and therefore outside the
       * dedup window -- double-counted in the stats, permanently. */
      int victim = -1;
      sensors_lock();
      for (int i = 0; i < n && victim < 0; i++) {
         int live = 0;
         for (int k = 0; k < g_nslot && !live; k++)
            if (g_slot[k].id == ids[i])
               live = 1;
         if (!live)
            victim = i;
      }
      sensors_unlock();
      /* Every row live: only possible if the table already names every slot,
       * in which case `id` matched above and we never got here. Fall back to
       * the oldest so the write still happens rather than being skipped. */
      if (victim < 0)
         victim = 0;
      for (int i = victim + 1; i < n; i++) {
         ids[i - 1]  = ids[i];
         vals[i - 1] = vals[i];
      }
      at      = n - 1;
      ids[at] = id;
   }
   vals[at] = idx;

   /* Write a fresh file and rename over the old one, rather than truncating
    * in place. O_TRUNC destroys the stored indices BEFORE the new ones are
    * written, so a crash in that window left the file empty and the next
    * sync re-imported -- those fingersticks are typically weeks old, i.e.
    * outside the dedup window, so they were appended a second time and
    * double-counted in the stats. rename() is atomic: old values or new,
    * never nothing. */
   char tmp[300];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_meter_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   int ok = 1;
   for (int i = 0; i < n && ok; i++) {
      char b[32];
      int bn = snprintf(b, sizeof b, "%d,%d\n", ids[i], vals[i]);
      bn     = clampn(bn, sizeof b);
      if (write(fd, b, bn) != bn)
         ok = 0;
   }
   close(fd);
   if (!ok) { /* leave the previous file intact rather than half-writing */
      unlink(tmp);
      return;
   }
   if (rename(tmp, g_meter_path) != 0)
      unlink(tmp);
}

/* Read every stored (id,index) pair. Returns how many were read. */
static int meter_index_all(int *ids, int *vals, int cap)
{
   int fd = open(g_meter_path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return 0;
   b[n]    = 0;
   int cnt = 0;
   char *p = b;
   while (*p && cnt < cap) {
      int id  = 0;
      int v   = 0;
      int gi  = 0;
      int gv  = 0;
      int neg = 0;
      /* Digit-capped: unbounded accumulation is UB and happens during
       * parsing, before the `id > 0` test can reject anything. This file is
       * ours, but surviving a corrupt or hand-edited row is a parser's job
       * -- and every sibling parser (store.c, stats.c, sensors.c,
       * settings.c) received this hardening. The advance stays OUTSIDE the
       * cap, which is what a sibling fix got wrong and turned into a
       * launch-time hang. */
      int nid = 0;
      while (*p >= '0' && *p <= '9') {
         if (nid < 9) {
            id = (id * 10) + (*p - '0');
            nid++;
         }
         p++;
         gi = 1;
      }
      if (*p == ',')
         p++;
      if (*p == '-') {
         neg = 1;
         p++;
      }
      int nv = 0;
      while (*p >= '0' && *p <= '9') {
         if (nv < 9) {
            v = (v * 10) + (*p - '0');
            nv++;
         }
         p++;
         gv = 1;
      }
      if (gi && gv && id > 0) {
         ids[cnt]  = id;
         vals[cnt] = neg ? -v : v;
         cnt++;
      }
      while (*p && *p != '\n')
         p++;
      if (*p == '\n')
         p++;
   }
   return cnt;
}

/* This meter's stored index, or -1 for "nothing stored yet".
 * -1, not 0: index 0 is a real record, so the sentinel must sit below every
 * valid index or the meter's first record is skipped. */
static int meter_index_load(int id)
{
   int ids[MAX_SLOTS];
   int vals[MAX_SLOTS];
   int n = meter_index_all(ids, vals, MAX_SLOTS);
   for (int i = 0; i < n; i++)
      if (ids[i] == id)
         return vals[i];
   if (n > 0)
      return -1; /* new-format file, but this meter is not in it yet */

   /* MIGRATION: an install from before this file was keyed by sensor id
    * holds a bare integer. Parsing it as "id,index" yields nothing, so the
    * index would look unset and the meter would re-import its recent window
    * -- records typically weeks old, i.e. outside the in-memory dedup
    * window, so they would be appended to the lifetime log a second time.
    * Adopt the old value for whichever meter asks first; that is exactly
    * right, because the old format could only ever describe one meter. The
    * next save rewrites the file in the new format. */
   int fd = open(g_meter_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char b[32];
   int rn = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (rn <= 0)
      return -1;
   b[rn]   = 0;
   int v   = 0;
   int got = 0;
   for (int i = 0; i < rn && b[i] >= '0' && b[i] <= '9'; i++) {
      v   = (v * 10) + (b[i] - '0');
      got = 1;
   }
   if (got)
      LOGI("meter.idx: adopting legacy index %d for id %d", v, id);
   return got ? v : -1;
}

/* ---- OneTouch meter driver hooks (otble.h) ----
 * A meter is not a CGM, so its readings take a different path into the
 * store: KIND_BGM (never deduped against a CGM sample, never the big number)
 * and a timestamp that has to be converted rather than trusted. */

/* The transport's drv_write/drv_subscribe are already UUID-generic, so the
 * meter needs no transport of its own -- only its own protocol. */
void ot_drv_write(const uint8_t *data, int n)
{
   if (g_meter_link >= 0)
      dexble_write(g_meter_link, OT_WRITE, data, n, 0);
}

void ot_drv_subscribe(void)
{
   if (g_meter_link < 0)
      return;
   dexble_subscribe(g_meter_link, OT_NOTIFY, 0);
   /* Queued on the meter's own link, so its model/firmware are known by the
    * time the sync finishes and can be written into its provenance. */
   dexble_request_devinfo_link(g_meter_link);
}

/* The protocol exchange is over. Ask for the link to close and drop the busy
 * latch -- but DO NOT un-arm, and do not treat this as the meter being gone.
 *
 * This used to call meter_unarm_link here, justified by the claim that "the
 * meter has powered itself off by now" and a gate that was described in the
 * comment but never actually written. An HCI capture falsified both. A Verio
 * stays awake about THIRTY-FIVE SECONDS after a fingerstick (observed:
 * connected 07:07:50, supervision timeout 07:08:25), and un-arming while it
 * is still connected makes meter_armed() false -- so the 1 Hz tick calls
 * meter_sync_start again, reconnects, and re-runs the whole exchange. The
 * capture shows 29 complete syncs in 29 seconds inside ONE connection, each
 * re-writing the CCCD and re-reading model, firmware and manufacturer. The
 * close never even reached the controller (no HCI Disconnect appears for
 * that address at all), so the un-arm was this function's only lasting
 * effect, and its only effect was to start the next lap.
 *
 * The link is released where the link actually dies: pancra_meter_disconnected,
 * on the real GATT disconnect -- whether that comes from the close below or,
 * as it usually does, from the meter powering itself off. Only then does the
 * tick re-arm the standing connect, which is what makes the NEXT fingerstick
 * catchable without the user touching the phone.
 *
 * A second fingerstick taken during the same power-on is therefore not picked
 * up until the meter cycles. Nothing is lost: the walk is index-based, so the
 * next sync reads both records. */
void ot_drv_disconnect(void)
{
   if (g_meter_link >= 0)
      dexble_link_close(g_meter_link);
   /* Stamp the completion BEFORE clearing g_meter_link -- meter_rt is keyed
    * by registry id, but the cooldown that keeps a still-awake meter from
    * being re-synced is read off this. */
   struct meter_rt *rt = (g_meter_src > 0) ? meter_rt_get(g_meter_src, 1) : 0;
   if (rt)
      rt->synced_t = realtime_s();
   /* Mark the link as waiting on its teardown, so the watchdog can tell a
    * legitimate wait from a lost callback. */
   if (g_meter_link >= 0 && g_meter_link < LINK_MAX)
      g_link_idle_t[g_meter_link] = realtime_s();
   g_meter_busy = 0;
   g_meter_link = -1;
}

void ot_drv_status(const char *s)
{
   set_status(s);
   /* Record the driver's live phase text against the meter that currently
    * owns the sync, so its per-device STATE row can show a descriptive step
    * ("COUNT", "READING", "NOTHING NEW") instead of a flat "SYNCING". The
    * "METER: " prefix is stripped -- the row is already known to be a meter.
    */
   struct meter_rt *rt = (g_meter_src > 0) ? meter_rt_get(g_meter_src, 1) : 0;
   if (rt) {
      const char *p = s;
      if (strncmp(p, "METER: ", 7) == 0)
         p += 7;
      str_snapshot(rt->stat, sizeof rt->stat, p);
      /* Any driver phase means we CONNECTED to this meter (the first is
       * "HELLO" on connect), so this IS a sync -- stamp LAST SYNC here, not
       * only when a datapoint or an RSSI read lands. A meter that connects
       * but yields no new record (e.g. the record read was refused)
       * otherwise stayed "OFF / NEVER" despite plainly having synced. */
      rt->sync_t = realtime_s();
   }
}

static long meter_tz_for(long naive);

int ot_drv_reading(long naive, int mg_dl)
{
   /* The meter's clock is naive local time with no zone, so the offset in
    * force AT IMPORT is what makes it an absolute instant -- and it is
    * stored alongside the raw value so a wrong conversion stays repairable.
    * Without this the reading lands 7-8 hours off, which is exactly the
    * discrepancy the capture showed. */
   long tz = meter_tz_for(naive);
   long t  = naive + OT_EPOCH - tz;
   /* THE EXACT timestamp bound lives here, not in otble.c: this is the first
    * point at which `t` is a true instant rather than a naive local clock
    * reading. A future-dated record sorts to the head of the history
    * permanently and is re-admitted on every restart, which is what the
    * meter clock is capable of producing. One hour of slack absorbs a DST
    * edge. */
   /* Generous, because this is measured against the PHONE's clock, which can
    * legitimately be wrong (a flat battery before NTP, a dead RTC, a
    * hand-set date). A tight bound here rejected perfectly good records
    * whenever the phone was slow -- and otble.c used to persist its walk
    * past the rejection, so those fingersticks were destroyed permanently.
    * It no longer does, but the bound should still only catch records wrong
    * by more than any plausible clock skew or timezone. */
   if (t <= 0 || t > realtime_s() + (15L * 3600)) {
      LOGI("meter reading at %ld (raw %ld) implausible, rejected", t, naive);
      return 0; /* the driver must not persist its walk past this */
   }
   hist_lock();
   int isnew = hist_insert(t, mg_dl, 127, g_meter_src, KIND_BGM);
   hist_unlock();
   if (isnew) { /* meters are never rescaled: factor 1000 */
      store_append(t, mg_dl, 127, 0, 0, g_meter_src, naive, tz, KIND_BGM, 1000);
      /* fingersticks ride the same cursor-driven sync (see above) */
   }
   LOGI("meter reading %d mg/dL at %ld (raw %ld)%s", mg_dl, t, naive,
        isnew ? "" : " (already stored)");
   g_ui_dirty = 1;
   return 1;
}

void ot_drv_done(int new_records)
{
   /* The meter is first registered with nothing but its address -- DIS has
    * not answered yet at pair time. Once it has, re-mint: identical fields
    * reuse the id, and a genuine difference mints a new one, which is
    * exactly the rule that keeps an id pinned to one (device, firmware) pair
    * for good. Readings taken before we knew the firmware keep citing the
    * older id, which is the truthful record of what we knew then. */
   /* BOTH, not either. The two DIS reads are separate serialized GATT ops
    * and a sync commonly ends after one round trip, so "model present, fw
    * still empty" is the normal intermediate state -- and minting against
    * (model, "") does not match the stored (model, fw), producing a NEW id
    * and a rebind, which the next complete sync mints straight back. The
    * meter oscillated between ids, appending a provenance row per flip and
    * splitting its fingerstick history across them in a file that is never
    * rewritten. */
   if (g_meter_src && g_meter_model[0] && g_meter_fw[0]) {
      /* COMPLETE the row, do not re-mint it.
       *
       * This used to mint with the model/fw filled in and rebind the slot if
       * the id came back different -- but a device is identified by (type,
       * MAC) ALONE, so the mint always returns the id we already have and
       * `id != g_meter_src` was never true. The rebind was dead code and a
       * meter's provenance row kept its empty model and firmware forever,
       * exactly contrary to what this block claims to do -- and the
       * reconcile completion pass walks CGM links, so nothing else filled
       * them either. sensor_complete is the mechanism the CGM path already
       * uses: it fills only what is missing and cannot fork an id. */
      if (sensor_complete(g_meter_src, "", g_meter_model, g_meter_fw, 0))
         LOGI("meter provenance completed: id %d (%s / %s)", g_meter_src,
              g_meter_model, g_meter_fw);
   }
   meter_index_save(g_meter_src, ot_last_index());
   LOGI("meter sync complete: %d new record(s), index now %d", new_records,
        ot_last_index());
   g_ui_dirty = 1;
}

void pancra_backfill(int mg_dl, int trend, int age_s)
{
   if (!glucose_plausible(mg_dl)) {
      LOGI("backfill %d mg/dL implausible, ignored", mg_dl);
      return;
   }
   long t = realtime_s() - age_s;
   /* Same per-link attribution as the live path (see src_for_link): a
    * backfill arrives on the link of the sensor that buffered it. */
   int src = src_for_link(driver_link());
   if (src < 0) {
      if (cgm_slot_count() >= 1) {
         LOGI("backfill %d mg/dL from an unregistered link, deferred", mg_dl);
         return;
      }
      src = g_cur_src; /* single CGM: the global is unambiguous */
   }
   /* RESCALE, timestamp-gated: a backfilled point is only rescaled if ITS
    * OWN timestamp is at/after activation. An older buffered point (t <
    * activation) predates the correction and keeps its raw value -- this is
    * the backfill care the feature calls for. g_link_raw is NOT touched
    * here: the factor is computed from the LIVE reading, not a historical
    * one. */
   int rpm = rescale_pm_for(src, t);
   if (rpm != 1000)
      mg_dl = rescale_apply(mg_dl, rpm);
   int prime = sensor_primary_id();
   /* Backfill reaches back across the warmup hour on a fresh sensor, so it
    * needs the same gate as the live path -- and before hist_lock, per the
    * registry -> history lock order (sensors.h). */
   int warm = sensor_in_warmup(src, t);
   hist_lock();
   int isnew = hist_insert(t, mg_dl, trend, src, KIND_CGM);
   if (isnew && !warm) /* any non-zero: the log and the stats must agree
                        * (store.h), minus uncalibrated warmup */
      stat_add(t, mg_dl);
   hist_refresh_current(prime);
   hist_unlock();
   if (isnew) {
      store_append(t, mg_dl, trend, 0, 0, src, t, g_tz_off, KIND_CGM,
                   rpm); /* no RSSI for backfilled points */
      /* delivered by remote_sync_step() on the tick */
   }
   LOGI("backfill reading %d mg/dL age %d -> t=%ld", mg_dl, age_s, t);
   /* A gap recovered by backfill can be the newest reading (a missed live
    * cycle); re-evaluate the alarm and refresh the notification rather than
    * waiting for the next live reading. Rendering is on the 1 Hz timer. */
   /* Alarm left to the 1 Hz main-thread path -- see pancra_glucose. */
   g_ui_dirty     = 1;
   g_notify_dirty = 1;
}

static void start_scan(struct ANativeActivity *a)
{
   if (!a || !a->env || g_scanning || !g_ble)
      return;
   JNIEnv *env = a->env;
   if (!has_ble_permissions(a)) {
      set_status("NO PERMISSION");
      return;
   }
   jstring err = (*env)->CallStaticObjectMethod(env, g_ble, g_scan, a->clazz);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      set_status("SCAN THREW");
      return;
   }
   if (err) {
      /* NULL-check (OOM leaves an exception pending), and DELETE the ref.
       * These run on the native looper thread, not inside a JNI method
       * invocation, so there is no frame pop to reclaim local refs -- they
       * accumulate for the life of the process against the ~512 ceiling,
       * which aborts when exceeded. The self-heal calls start_scan every 30
       * s while scanning is failing, which is exactly when this path runs.
       */
      const char *e = (*env)->GetStringUTFChars(env, err, NULL);
      if (e) {
         LOGI("scan: %s", e);
         set_status(e);
         (*env)->ReleaseStringUTFChars(env, err, e);
      } else if ((*env)->ExceptionCheck(env)) {
         (*env)->ExceptionClear(env);
      }
      (*env)->DeleteLocalRef(env, err);
      return;
   }
   g_scanning = 1;
   LOGI("scanning (receive-only)");
   /* only surface SCANNING before we're operational; once paired/streaming
    * the driver's own status (WAITING/CONNECTED/...) is the meaningful one
    * and the background scan must not mask it */
   struct dex_session s;
   driver_lock();
   /* SELECT explicitly. The ambient selection is left wherever the last GATT
    * callback put it (they select and never restore), so reading without
    * selecting reports whichever link a binder thread happened to touch --
    * "the lock is held" is not the same as "the right context is chosen". */
   int prev = driver_link();
   driver_select(LINK_CGM);
   driver_get_session(&s);
   driver_select(prev);
   driver_unlock();
   if (!s.paired && !s.have_reading)
      set_status("SCANNING");
}

static void stop_scan(struct ANativeActivity *a)
{
   /* Guard the activity AND its env. Callers pass g_act, which on_destroy
    * clears, and the pending-stop retry runs from the 1 Hz timer -- so a
    * teardown racing that retry would dereference a null activity here. */
   if (!a || !a->env || !g_scanning)
      return;
   JNIEnv *env      = a->env;
   jboolean stopped = (*env)->CallStaticBooleanMethod(env, g_ble, g_stop);
   if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
      stopped = 0;
   }
   /* Only believe the scan is down if Java confirmed it. Clearing g_scanning
    * unconditionally, while Ble still held a registered callback it could no
    * longer cancel, let the on_timer self-heal register a SECOND scan client
    * -- duplicate adverts, and eventually Android's scan-throttle block.
    * Leaving g_scanning set means the self-heal will not stack another one,
    * and the next stop_scan retries the cancel. */
   if (!stopped) {
      /* Mark it for RETRY. Leaving g_scanning set is right -- it stops the
       * self-heal stacking a second scan client on top of one Ble still
       * holds
       * -- but on its own it was a permanent latch: g_scanning is cleared in
       * exactly one place, reachable only from a LATER successful stop_scan,
       * and the self-heal that exists so the user need not background the
       * app is itself gated on !g_scanning. So the one mechanism that could
       * recover was structurally excluded, and nothing anywhere notices that
       * adverts have stopped arriving. The 1 Hz timer retries until Java
       * confirms. */
      g_scan_stop_pending = 1;
      LOGI("stop_scan: Ble could not confirm the scan stopped; will retry");
      return;
   }
   g_scan_stop_pending = 0;
   g_scanning          = 0;
   /* don't surface "PAUSED": stopping the background scan is an internal
    * detail (it happens on pause and on every orientation flip); the
    * driver's own connection status stays the meaningful thing to show */
}

/* Tear the scan down and bring it back up.
 *
 * WHY THIS EXISTS, and it is the bug that made SYNC NOW useless: start_scan
 * is idempotent on g_scanning, so calling it while a scan is already
 * registered does NOTHING. That is right for the self-heal -- stacking a
 * second scan client is how the app used to hit Android's scan-throttle
 * block -- but it meant the app had no way at all to REFRESH a scan that was
 * still registered yet no longer delivering.
 *
 * And Android degrades scans behind our back with no callback: send the
 * activity to the background and the stack quietly demotes SCAN_MODE_LOW_
 * LATENCY towards opportunistic, so results only arrive when some other app
 * happens to scan. Measured on 2026-08-03: ~20 advertisements a minute from
 * 3 devices while degraded, versus ~14000 a minute from 140 devices once
 * genuinely restarted -- a 700x difference, with g_scanning reading 1 and
 * the app believing all was well the entire time. A OneTouch meter that
 * advertises in short bursts is invisible in that state, which is exactly
 * how a meter switched on and sitting next to the phone went 26 minutes
 * without syncing while the user pressed SYNC NOW.
 *
 * on_pause/on_resume was the ONLY path that produced a real restart, so the
 * user's workaround was to leave the app and come back. An explicit request
 * must not require that.
 *
 * If Java cannot confirm the stop, stop_scan leaves g_scanning set on
 * purpose and start_scan will no-op -- deliberately, so we never stack a
 * second client; the 1 Hz retry finishes the job. */
static void scan_restart(struct ANativeActivity *a)
{
   if (!a || !a->env)
      return;
   stop_scan(a);
   start_scan(a);
}

/* --- input: drain the queue so the ANR watchdog stays fed --- */

/* reprogram the shared timer: first tick after `first_ms`, then every
 * `repeat_ms`. Used to switch between the 1 Hz repaint cadence and the
 * hold-to- repeat cadence -- which waits before repeating so a quick tap
 * doesn't repeat.
 */
static void timer_set(long first_ms, long repeat_ms)
{
   if (g_timerfd < 0)
      return;
   struct itimerspec its;
   its.it_value.tv_sec     = first_ms / 1000;
   its.it_value.tv_nsec    = (first_ms % 1000) * 1000000L;
   its.it_interval.tv_sec  = repeat_ms / 1000;
   its.it_interval.tv_nsec = (repeat_ms % 1000) * 1000000L;
   timerfd_settime(g_timerfd, 0, &its, 0);
}

/* Push the live value + a 3H plot into the ongoing notification (lock screen
 * / shade). Main thread only (uses g_act->env). The plot is grayscale (white
 * dots on dark), so the ARGB bitmap needs no colour swizzle. Called from
 * on_timer when a new reading has arrived, so we rebuild the bitmap at most
 * once a cycle, not every second. */
static uint32_t g_notify_px[NOTIFY_W * NOTIFY_H];

/* Serialises notify_update against itself.
 *
 * It is now driven from BOTH the activity's 1 Hz timer and the service's
 * tick thread, and it fills two file-static buffers (g_notify_px and the
 * plot-point array). Two threads inside it at once would interleave those.
 * Try-lock, not spin: this is an idempotent refresh, so skipping one is free
 * -- the same reasoning draw() uses for its frame guard. */
static volatile int g_notify_busy;

static void notify_update(void)
{
   /* Context and env from the transport, NOT from g_act. g_act is NULL once
    * the activity is destroyed, and this used to return early on that -- so
    * after a back-press or task-swipe the notification froze at whatever
    * value was current at that instant, while readings kept arriving. In
    * that state the notification is the ONLY glucose display the user has.
    */
   JNIEnv *e    = dexble_env();
   jobject jctx = dexble_ctx();
   if (!e || !jctx || !m_show_glucose || !g_ble)
      return;
   if (__atomic_exchange_n(&g_notify_busy, 1, __ATOMIC_SEQ_CST))
      return;
   char title[48];
   char text[48];
   /* ONE consistent (glucose, trend, time) triple, under the lock the writer
    * uses. hist_refresh_current stores the three separately, so reading them
    * unlocked can pair a NEW glucose with the PREVIOUS timestamp -- which
    * evaluates as stale and renders "no recent reading" at the moment a hypo
    * lands -- or the mirror, a stale in-range value stamped with a fresh
    * time. Once the activity is destroyed this notification is the only
    * glucose display the user has. The alarm path already had
    * current_reading() for exactly this; the notification path did not. */
   hist_lock();
   int cur_glu   = g_cur_glu;
   int cur_trend = g_cur_trend;
   long cur_time = g_cur_time;
   hist_unlock();
   /* Value, trend AND time all on the TITLE line, leaving the content-text
    * line empty so the BigPicture plot below gets that vertical space. */
   int stale = realtime_s() - cur_time > AL_FRESH_S;
   text[0]   = 0;
   /* val is the bare display value for the STATUS-BAR icon; EMPTY on
    * stale/no data, which is Java's cue to fall back to the app glyph --
    * the bar must never show a number the app itself would blank. */
   char val[12];
   val[0] = 0;
   if (cur_glu < 0 || stale) {
      (void)snprintf(title, sizeof title, "--- %s  no recent reading",
                     UNIT_LBL);
   } else {
      char gv[12];
      char tr[8];
      char hm[16];
      fmt_glu(cur_glu, g_units, gv, sizeof gv);
      fmt_trend(cur_trend, tr, sizeof tr);
      fmt_hms(cur_time, g_tz_off, hm, sizeof hm);
      hm[5] = 0; /* HH:MM */
      (void)snprintf(title, sizeof title, "%s %s  %s   at %s", gv, UNIT_LBL, tr,
                     hm);
      (void)snprintf(val, sizeof val, "%s", gv);
   }
   if (!g_statbar_val)
      val[0] = 0; /* STATUS BAR: ICON mode -- Java falls back to the glyph */
   for (int i = 0; i < NOTIFY_W * NOTIFY_H; i++)
      g_notify_px[i] = 0xFF000000; /* true black, matching the app screen */
   static struct plot_pt pts[NHIST];

   /* Per-device styling, SNAPSHOTTED before hist_lock: the plot must colour
    * each source with the marker/colour the user chose, exactly like the
    * main screen -- but the registry lock is taken BEFORE hist
    * (driver->reg->hist), so resolve styles here, then apply them under
    * hist_lock without nesting the two. */
   struct notif_sty {
      int id, marker, color, size;
   };
   static struct notif_sty sty[MAX_SLOTS];
   int nsty = 0;
   sensors_lock();
   /* Every slot -- LIVE and OLD (disconnected) alike -- carries its own
    * marker/colour, so one pass over the slots styles the whole plot the
    * same way the main screen does. */
   for (int i = 0; i < g_nslot && nsty < (int)(sizeof sty / sizeof sty[0]); i++)
      sty[nsty++] = (struct notif_sty){g_slot[i].id, g_slot[i].marker,
                                       g_slot[i].color, g_slot[i].size};
   sensors_unlock();

   hist_lock();
   /* The notification plot mirrors the MAIN screen's plot EXACTLY: every
    * datapoint from every source -- CGM traces AND meter (BGM) fingersticks
    * -- each styled with that device's own marker and colour (a HIDE-marked
    * device is dropped, just as on the main plot), so the two read the same.
    */
   int np = 0;
   for (int i = 0; i < g_nhist; i++) {
      int src      = g_hist[i].src;
      int mk       = 0; /* 0 + col 0 = the value-palette main trace */
      uint32_t col = 0; /* (used for src 0 legacy and unmatched primary) */
      int sz       = MARK_SIZE_DEF;
      int hide     = 0;
      int found    = 0;
      for (int k = 0; k < nsty; k++)
         if (sty[k].id == src) {
            found = 1;
            mk    = sty[k].marker;
            col   = ui_sensor_color(sty[k].color);
            sz    = sty[k].size;
            if (sty[k].marker == MARK_HIDE)
               hide = 1;
            break;
         }
      /* An unstyled fingerstick (or any forgotten source) still needs to be
       * a distinct MARKER, not a value-palette line vertex reading as CGM
       * data: give it the orphan look. src 0 is legacy CGM and keeps the
       * default. */
      if (!found && (src != 0 || g_hist[i].kind == KIND_BGM)) {
         if (src != 0) {
            mk  = MARK_CROSS;
            col = 0xFF8A8AA0; /* UI_ORPHAN */
         }
      }
      if (hide)
         continue;
      pts[np].t      = g_hist[i].t;
      pts[np].glu    = g_hist[i].glu;
      pts[np].marker = mk;
      pts[np].col    = col;
      pts[np].size   = sz;
      np++;
   }
   hist_unlock();
   plot_render(g_notify_px, NOTIFY_W, NOTIFY_W, NOTIFY_H, 0, 0, NOTIFY_W,
               NOTIFY_H, pts, np, realtime_s(), 3, 3, white_color, -1, 0,
               g_tz_off);
   /* plot_render writes the SCREEN's pixel convention -- raw u32 on a
    * little-endian RGBA surface, i.e. 0xAABBGGRR -- but
    * Bitmap.createBitmap(..., ARGB_8888) reads each int as 0xAARRGGBB.
    * Swap R and B or every coloured marker is wrong in the notification:
    * white dots and gray gridlines hide the swap (R == B), a PINK meter
    * marker turned periwinkle. Same failure ui_sensor_colors documents. */
   for (int i = 0; i < NOTIFY_W * NOTIFY_H; i++) {
      uint32_t c = g_notify_px[i];
      g_notify_px[i] =
          (c & 0xFF00FF00U) | ((c & 0xFFU) << 16U) | ((c >> 16U) & 0xFFU);
   }
   jstring jt    = (*e)->NewStringUTF(e, title);
   jstring js    = (*e)->NewStringUTF(e, text);
   jstring jv    = (*e)->NewStringUTF(e, val);
   jintArray arr = (*e)->NewIntArray(e, NOTIFY_W * NOTIFY_H);
   /* On OOM any of these is NULL with an exception pending;
    * SetIntArrayRegion on a NULL array aborts the VM, so bail and clean up
    * instead. */
   if (jt && js && jv && arr) {
      (*e)->SetIntArrayRegion(e, arr, 0, NOTIFY_W * NOTIFY_H,
                              (const jint *)g_notify_px);
      (*e)->CallStaticVoidMethod(e, g_ble, m_show_glucose, jctx, jt, js, jv,
                                 arr, (jint)NOTIFY_W, (jint)NOTIFY_H,
                                 (jint)(g_lockscr_val ? 1 : 0));
   }
   if ((*e)->ExceptionCheck(e))
      (*e)->ExceptionClear(e);
   if (jt)
      (*e)->DeleteLocalRef(e, jt);
   if (js)
      (*e)->DeleteLocalRef(e, js);
   if (jv)
      (*e)->DeleteLocalRef(e, jv);
   if (arr)
      (*e)->DeleteLocalRef(e, arr);
   __atomic_store_n(&g_notify_busy, 0, __ATOMIC_SEQ_CST);
}

/* GOING STALE IS AN EVENT, and nothing else reports it.
 *
 * notify_update decides staleness at render time, but every route into it is
 * gated on g_notify_dirty -- which is set by new data and by menu actions,
 * and by NOTHING that fires as time passes. So on a sensor dropout with the
 * activity destroyed (precisely what the foreground service exists for) the
 * last number stayed on the status bar and lock screen indefinitely, and the
 * service's own 5-minute wake re-posted that cached value. The in-app screen
 * blanks at AL_FRESH_S; the only glucose display left did not, which is the
 * exact lie the app is written not to tell -- and with the DISCONNECT alarm
 * off by default nothing else warned.
 *
 * Marking the TRANSITION rather than refreshing unconditionally keeps the
 * battery argument below intact: this costs two loads and a compare per
 * tick, and re-renders once, when the state actually changes.
 *
 * last_stale is deliberately plain: the two callers run on different
 * threads, and the worst a race can do is set the flag twice or one tick
 * late, both self-correcting. */
static void notify_stale_check(void)
{
   static volatile int last_stale = -1;
   hist_lock(); /* released before notify_update, which takes it again */
   int cur_glu   = g_cur_glu;
   long cur_time = g_cur_time;
   hist_unlock();
   int stale = (cur_glu < 0) || (realtime_s() - cur_time > AL_FRESH_S);
   if (stale != last_stale) {
      last_stale     = stale;
      g_notify_dirty = 1;
   }
}

/* Public entry: the service heartbeat's route to the notification.
 *
 * DIRTY-DRIVEN, exactly like the on_timer path. Refreshing unconditionally
 * on every 20 s tick would re-render the plot bitmap and re-post the
 * notification when nothing had changed -- pure battery and CPU cost on a
 * 24/7 app, for a display that only moves when a reading lands.
 * g_notify_dirty is set by pancra_glucose, so this fires within one tick of
 * each new reading, and by notify_stale_check when it goes stale. */
void pancra_notify_refresh(void)
{
   notify_stale_check();
   if (!__atomic_exchange_n(&g_notify_dirty, 0, __ATOMIC_SEQ_CST))
      return;
   notify_update();
}

/* timer tick: repaint so AGE / stale state stay live */
static int on_timer(int fd, int events, void *data)
{
   g_where = "on_timer";
   (void)events;
   (void)data;
   /* on_timer IS the render thread; reaffirm g_main_tid here so on_main()
    * can never be wrong about who may draw -- guarantees the 1 Hz repaint
    * always runs even if the onCreate capture were somehow off, so the UI
    * can't wedge.
    */
   g_main_tid     = gettid();
   uint64_t ticks = 0;
   read(fd, &ticks, sizeof ticks); /* single read clears the expiration count */
   /* Covers both the stale-data alarm (which depends on elapsed time) and
    * any glucose alarm a reading raised on a binder thread: the zone is
    * recomputed here, so this single main-thread caller owns every
    * transition.
    */
   disc_reeval();
   /* One REMOTE step per tick: read the server's cursor, or send the next
    * chronological batch of points newer than it. Self-throttling (Java
    * reports busy while a request is in flight or a backoff is running),
    * so a backlog drains at the server's own pace instead of being lost. */
   pancra_remote_sync();
   calq_tick(); /* retry / expire any durably-queued calibration */
   /* Expire a pending rescale that no reading answered within the window,
    * even if no datapoint arrives at all (a disconnected CGM). Surfaced,
    * never dropped. */
   if (g_rescale_pend_mgdl > 0 &&
       realtime_s() - g_rescale_pend_t > RESCALE_PEND_WINDOW_S) {
      LOGI("pending rescale %d mg/dL EXPIRED (no reading within window)",
           g_rescale_pend_mgdl);
      g_rescale_expired    = 1;
      g_rescale_expired_id = g_rescale_pend_id;
      g_rescale_pend_mgdl  = 0;
      g_rescale_pend_id    = 0;
      g_rescale_pend_t     = 0;
      rescale_save();
      g_ui_dirty = 1;
   }
   /* Refresh the UTC offset periodically. It used to be read once in
    * onCreate, but the foreground service is designed to outlive the
    * activity for days -- so across a DST transition every displayed
    * timestamp, and every subsequent meter import, stayed an hour off until
    * a cold start. */
   if (g_act && g_act->env && realtime_s() - g_tz_checked > 300) {
      g_tz_checked = realtime_s();
      init_tz_offset(g_act->env);
   }
   sensor_reconcile(); /* keep the registry in step with the live session */

   /* SELF-HEAL THE SCAN.
    *
    * Three paths tear the scan down -- pair_cancel(), and both branches of
    * commit_pair() -- and NOTHING brought it back while the app stayed in
    * the foreground; only on_resume did, i.e. the user had to background and
    * foreground the app. Every CGM's advert-driven reconnect runs off this
    * scan, so pairing a meter, or merely CANCELLING a pairing, silently
    * stopped every already-paired sensor from ever reconnecting for the rest
    * of the session. Nothing on screen says so: the last reading just stops
    * ageing forward once the current connection drops.
    *
    * MA_SYNC's comment already asserts the intended invariant -- "one is
    * already running whenever the UI is up (start_scan is idempotent via
    * g_scanning)". This is what makes that assertion true instead of
    * aspirational, and it self-heals any future path that forgets.
    *
    * THROTTLED to once every 30 s, not every tick. start_scan() sets
    * g_scanning only on success, so a persistent failure (Bluetooth off,
    * BLUETOOTH_SCAN revoked, the LE scanner unavailable) leaves the
    * condition true forever -- retrying at 1 Hz would re-enter JNI and
    * rewrite the status line every second, and repeated startScan calls are
    * exactly what trips Android's "app scanning too frequently" block (5
    * starts in 30 s), which would turn a recoverable failure into a sticky
    * one. */
   /* Retry a stop Java could not confirm, so g_scanning cannot latch. */
   if (g_scan_stop_pending && g_act)
      stop_scan(g_act);
   {
      /* Decision in scanlogic.c so `make check` can fail on it. */
      static long last_scan_retry;
      struct scan_state ss = {.have_activity = g_act != 0,
                              .paused        = g_paused,
                              .scanning      = g_scanning,
                              .pairing       = g_smart_pairing,
                              .meter_busy    = g_meter_busy,
                              .now           = realtime_s(),
                              .hold_until    = g_scan_hold_until,
                              .last_attempt  = last_scan_retry};
      if (scan_should_start(&ss)) {
         last_scan_retry = ss.now;
         LOGI("scan was down with the UI up; restarting it");
         start_scan(g_act);
      }
   }
   pancra_link_watchdog();
   /* An ARMED pairing commits itself the moment an unambiguous candidate is
    * on the air (fresh adverts only -- select_candidate's 60 s window). Main
    * thread only, like every other commit_pair caller. keypad_close() runs
    * inside commit_pair, so aim it at the CURRENT menu first -- a background
    * commit must never yank the user out of whatever screen they are on. */
   if (g_pend_pairing && sensor_kind(g_pend_pairing) == KIND_CGM &&
       g_menu != MENU_KEYPAD && g_menu != MENU_DEVLIST &&
       g_menu != MENU_PAIRCONF) {
      int pidx = select_candidate();
      if (pidx >= 0) {
         char pmac[sizeof g_devs[0].mac];
         int have = 0;
         devlist_lock();
         if (pidx < g_ndevs) {
            str_snapshot(pmac, sizeof pmac, g_devs[pidx].mac);
            have = 1;
         }
         devlist_unlock();
         if (have) {
            LOGI("pending pairing: candidate %s appeared -> committing", pmac);
            g_add_type  = g_pend_pairing; /* commit_pair branches on it */
            g_kp_return = g_menu;         /* stay on the current screen */
            commit_pair(pmac);            /* clears g_pend_pairing */
         }
      }
   }
   /* Same atomic test-and-clear the service path uses: whichever consumer
    * wins renders it, and the other does not double-render. */
   notify_stale_check();
   if (__atomic_exchange_n(&g_notify_dirty, 0, __ATOMIC_SEQ_CST))
      notify_update();
   /* Rebuild the text lines here (BLE-thread updates only marked
    * g_ui_dirty), then repaint unconditionally so AGE / stale state stay
    * live. Both run on the main thread, so the hit-box geometry can't race
    * on_input. */
   g_ui_dirty = 0;
   update_screen();
   if (g_win)
      draw(g_win);
   return 1;
}

static int on_input(int fd, int events, void *data)
{
   g_where = "on_input";
   (void)fd;
   (void)events;
   struct AInputQueue *q  = data;
   struct AInputEvent *ev = NULL;
   while (AInputQueue_getEvent(q, &ev) >= 0) {
      if (AInputQueue_preDispatchEvent(q, ev))
         continue; /* IME took it; finished elsewhere */
      int handled = 0;
      if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
         int action = (int)((unsigned)AMotionEvent_getAction(ev) &
                            (unsigned)AMOTION_EVENT_ACTION_MASK);
         int tx     = (int)AMotionEvent_getX(ev, 0);
         int ty     = (int)AMotionEvent_getY(ev, 0);
         /* A sounding alarm is silenced by ANY press anywhere in the app --
          * main screen, settings, keypad, gate -- and that press does
          * nothing else. Checked before every modal handler so it always
          * wins. */
         /* Re-test UNDER the lock, not just before it: alarm_apply on a
          * binder thread writes g_alarm_sounding, so an unsynchronized test
          * could swallow a tap meant for the UI (alarm cleared meanwhile) or
          * let a tap fall through to menu_action and toggle a setting
          * instead of silencing (alarm raised meanwhile). */
         /* Acknowledge entirely INSIDE the lock -- test, clear, and silence.
          *
          * Silencing outside it is the mirror of the alarm_apply hazard: a
          * binder thread could raise a NEW level (say LOW -> HIGH) between
          * the unlock and the silence call, and this silence would then kill
          * the alarm that had just legitimately started. g_alarm_want would
          * already be HIGH, so no later alarm_apply would re-raise it -- a
          * silently lost alarm. Raise and silence must be mutually exclusive
          * end to end on BOTH sides.
          *
          * g_alarm_want is deliberately LEFT set: it records "this level has
          * already been announced", so the next alarm_apply at the same
          * level is a no-op and the alarm stays acknowledged. Clearing it
          * would make the very next reading re-chime what the user just
          * dismissed; a genuine change of level still re-fires. */
         /* (The old mid-gesture "touch swallow" is gone: actions now fire on
          * the RELEASE, so a menu can no longer change under a still-held
          * finger -- and the back key, the one remaining way it can, cancels
          * any armed press explicitly.) */
         int was_sounding = 0;
         if (action == AMOTION_EVENT_ACTION_DOWN) {
            alarm_lock();
            was_sounding = g_alarm_sounding;
            if (was_sounding) {
               if (dexble_alarm_silence()) {
                  g_alarm_sounding = 0;
                  g_alarm_acked    = 1; /* survives re-actuation; see there */
               } else {
                  /* Java was not reached, so the tone may still be playing.
                   * Leave `sounding` set so the next tap tries again rather
                   * than falling through to the UI with an alarm still on.
                   */
                  LOGI("alarm: silence failed; leaving the gesture armed");
                  was_sounding = 0;
               }
            }
            alarm_unlock();
         }
         if (was_sounding) {
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* the first-run rationale screen is modal: CONTINUE arms on the
          * press and fires the permission request on the release (the
          * app-wide act-on-release rule); anything else is ignored */
         if (g_gate) {
            if (action == AMOTION_EVENT_ACTION_DOWN) {
               struct action a = ui_hit(&g_hits, tx, ty);
               if (a.kind == ACT_GATE_CONTINUE)
                  press_arm(a.kind, a.arg, tx, ty);
               else
                  press_cancel();
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
               press_track(tx, ty);
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
               if (press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty)) {
                  g_gate = 0;
                  if (g_act)
                     request_ble_permissions(g_act);
                  if (g_win)
                     draw(g_win);
               }
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* All modal menus (settings / keypad / device list) are pure now: a
          * press ARMS the recorded ACT_MENU target under it, and the release
          * -- back on the same target -- dispatches its menu_action code.
          * Sliding off first cancels without firing anything. */
         if (g_menu) {
            /* The WEIGHT LOG plot scrubs. It lives on a MENU screen, where
             * every other target is press-arm/release, so it is handled here
             * rather than in the main screen's gesture code: a press picks
             * the nearest point and a drag keeps picking, using the plot
             * rect the last render recorded. */
            if (g_menu == MENU_WTLOG &&
                (action == AMOTION_EVENT_ACTION_DOWN ||
                 (action == AMOTION_EVENT_ACTION_MOVE && g_wt_scrubbing))) {
               for (int i = 0; i < g_hits.n; i++) {
                  if (g_hits.box[i].kind != ACT_SCRUB)
                     continue;
                  if (action == AMOTION_EVENT_ACTION_DOWN &&
                      (tx < g_hits.box[i].x ||
                       tx >= g_hits.box[i].x + g_hits.box[i].w ||
                       ty < g_hits.box[i].y ||
                       ty >= g_hits.box[i].y + g_hits.box[i].h))
                     break; /* the press began outside the plot */
                  struct screen sm;
                  build_model(&sm);
                  int pick = ui_wt_hit(&sm, g_hits.box[i].x, g_hits.box[i].w,
                                       g_hits.box[i].arg, tx);
                  if (pick >= 0) {
                     g_wt_scrub     = pick;
                     g_wt_scrubbing = 1;
                     press_cancel(); /* scrubbing is not a button press */
                     g_ui_dirty = 1;
                     draw(g_win);
                  }
                  break;
               }
               if (g_wt_scrubbing) {
                  AInputQueue_finishEvent(q, ev, 1);
                  continue;
               }
            }
            if (action == AMOTION_EVENT_ACTION_UP ||
                action == AMOTION_EVENT_ACTION_CANCEL) {
               /* Scrubbing lasts only while a finger is down, as on the
                * glucose plot: the readout borrows the tab row, so leaving
                * it up would hide the span tabs indefinitely and leave a
                * stale value on screen with nothing touching it. */
               if (g_wt_scrubbing) {
                  g_wt_scrubbing = 0;
                  g_wt_scrub     = -1;
                  g_ui_dirty     = 1;
                  draw(g_win);
               }
            }
            if (action == AMOTION_EVENT_ACTION_DOWN) {
               struct action a = ui_hit(&g_hits, tx, ty);
               if (a.kind == ACT_MENU)
                  press_arm(a.kind, a.arg, tx, ty);
               else
                  press_cancel();
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
               press_track(tx, ty);
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
               int aarg = g_arm_arg; /* press_release clears it */
               if (press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty)) {
                  menu_action(aarg);
                  /* GENERAL rule (so no menu needs special-casing): the
                   * moment a menu action lands back on the MAIN screen,
                   * restore its chosen orientation -- menus render portrait,
                   * main follows g_orient. Without this, closing a menu left
                   * the main screen stuck in the portrait the menu-open had
                   * forced. */
                  if (g_menu == MENU_NONE)
                     sys_set_orientation(g_orient);
               }
            }
            AInputQueue_finishEvent(q, ev, 1);
            continue;
         }
         /* main screen: resolve the tap against the targets recorded by the
          * last ui_render(), then run the shell-side gesture/timer state. */
         struct action act = ui_hit(&g_hits, tx, ty);
         /* A drag begins on a press inside the plot; once begun, keep
          * scrubbing for every MOVE -- even when the finger leaves the plot
          * rectangle -- using its X position (plot_hit picks by time/X
          * only). */
         int begin =
             (action == AMOTION_EVENT_ACTION_DOWN && act.kind == ACT_SCRUB);
         int cont = (action == AMOTION_EVENT_ACTION_MOVE && g_scrubbing);
         int rx   = 0;
         int ry   = 0;
         int rw   = 0;
         int rh   = 0;
         if ((begin || cont) && plot_rect(&rx, &ry, &rw, &rh)) {
            g_scrubbing = 1;
            press_cancel(); /* scrubbing is exempt; no arm may linger */
            /* Average the current sample with the batched historical ones so
             * the pick tracks the centre of the contact, not a jittery edge.
             */
            unsigned long hs = AMotionEvent_getHistorySize(ev);
            long ax          = tx;
            long ay          = ty;
            long n           = 1;
            for (unsigned long h = 0; h < hs; h++) {
               ax += (long)AMotionEvent_getHistoricalX(ev, 0, h);
               ay += (long)AMotionEvent_getHistoricalY(ev, 0, h);
               n++;
            }
            int fx = (int)(ax / n);
            int fy = (int)(ay / n);
            static struct plot_pt pts[UI_PTS_MAX];
            /* SIZED FOR THE SAME LOOP AS pts, not for the live window.
             * This was left at NHIST when pts grew to hold a long span, so
             * scrubbing 7D or 30D wrote up to PLOT_LONG_MAX ints into a
             * 5040-int array -- 176 kB straight through the statics that
             * follow it, which is what silently emptied the stats ring
             * minutes after every restart. */
            static int psrc[UI_PTS_MAX];
            /* Under hist_lock: hist_insert memmoves g_hist from a BLE binder
             * thread, so an unlocked copy here reads a half-shifted array
             * and the scrub lands on a datapoint that was never there. Every
             * other reader takes the lock; this one was missed. */
            int nlong2                    = 0;
            const struct ui_point *plong2 = plot_source_from(
                g_store_path, realtime_s(), g_plot_hours, &nlong2);
            int np = 0;
            if (plong2) {
               /* Hit-test what was DRAWN: on a long span that is the
                * bucketed set, not the RAM window, or the scrub would
                * select points the plot never showed. */
               for (int i = 0; i < nlong2 && np < PLOT_LONG_MAX; i++) {
                  pts[np].t      = plong2[i].t;
                  pts[np].glu    = plong2[i].glu;
                  pts[np].marker = 0;
                  pts[np].col    = 0;
                  pts[np].hidden = 0;
                  psrc[np]       = plong2[i].src;
                  np++;
               }
            } else {
               hist_lock();
               np = g_nhist < NHIST ? g_nhist : NHIST;
               for (int i = 0; i < np; i++) {
                  pts[i].t      = g_hist[i].t;
                  pts[i].glu    = g_hist[i].glu;
                  pts[i].marker = 0;
                  pts[i].col    = 0;
                  pts[i].hidden = 0;
                  psrc[i]       = g_hist[i].src;
               }
               hist_unlock();
            }
            /* A HIDDEN device (marker OFF) is off the plot, so its points
             * must not be scrub-selectable either. plot_hit skips
             * pts[].hidden. Pull the (few) hidden-marker device ids in ONE
             * locked call, then flag matching points -- no per-point
             * registry lock. */
            int hid[MAX_SLOTS];
            int nhid = sensor_hidden_ids(hid, MAX_SLOTS);
            for (int i = 0; nhid > 0 && i < np; i++)
               for (int j = 0; j < nhid; j++)
                  if (psrc[i] == hid[j]) {
                     pts[i].hidden = 1;
                     break;
                  }
            /* Insulin doses ride along, in the SAME order the model
             * appends them, so the returned index maps onto m->hist. */
            int np_glu = np;
            for (int i = 0; i < g_nins && np < UI_PTS_MAX; i++) {
               pts[np].t      = g_ins[i].t;
               pts[np].glu    = 60; /* the renderer's fixed insulin y */
               pts[np].marker = 0;
               pts[np].col    = 0;
               pts[np].size   = 0;
               pts[np].hidden =
                   (g_ins_marker[g_ins[i].type == INS_FAST ? INS_FAST
                                                           : INS_SLOW] ==
                    MARK_HIDE);
               np++;
            }
            /* WEIGHTS, in the SAME order build_model appends them (glucose,
             * doses, weights) so the index plot_hit returns maps onto
             * m->hist. Missing here, they were drawn on the plot and could
             * not be scrubbed: this array -- not the model's -- is what the
             * hit test walks. They share the doses' y, so they fall inside
             * the same bottom band and the aiming rule below picks them up
             * without any extra case. */
            for (int i = 0; i < g_nwt && np < UI_PTS_MAX; i++) {
               pts[np].t      = g_wt[i].t;
               pts[np].glu    = 60; /* the renderer's fixed bottom-line y */
               pts[np].marker = 0;
               pts[np].col    = 0;
               pts[np].size   = 0;
               pts[np].hidden = 0; /* weights have no hide toggle */
               np++;
            }
            /* plot_hit picks by TIME alone, which would leave a dose
             * between two 5-minute CGM points a sliver of reachability.
             * Aim by finger HEIGHT instead: in the insulin band (twice the
             * below-70 strip, so the row is reachable without covering it
             * with the fingertip) only insulin is selectable, above it
             * only glucose -- sliding along the plot bottom walks the
             * doses. Decided ONCE, at the DOWN: the rest of the gesture
             * keeps scrubbing whichever series it began on, wherever the
             * finger wanders. With no dose in the visible window the whole
             * plot aims at glucose -- no dead band over nothing. */
            if (begin) {
               int oy70 = ry + rh;
               {
                  int oxx            = 0;
                  struct plot_pt ref = {0};
                  ref.t              = realtime_s();
                  ref.glu            = 70;
                  if (!plot_point_xy(rx, ry, rw, rh, ref, realtime_s(),
                                     g_plot_hours, &oxx, &oy70))
                     oy70 = ry + rh; /* degenerate window: all glucose */
               }
               /* A dose only claims the touch if one is actually NEAR the
                * finger. Asking merely "is there a dose in this window"
                * meant that on a 30-day span a single marker at one edge
                * captured a press at the far edge, and the scrub then
                * jumped to a dose an inch away instead of reading the
                * glucose under the fingertip. About half an inch of x
                * either side -- an eighth of the plot -- is roughly a
                * fingertip's own width. */
               int near_x   = rw / 8;
               int have_ins = 0;
               for (int i = np_glu; i < np && !have_ins; i++) {
                  int oxx = 0;
                  int oyy = 0;
                  if (pts[i].hidden ||
                      !plot_point_xy(rx, ry, rw, rh, pts[i], realtime_s(),
                                     g_plot_hours, &oxx, &oyy))
                     continue;
                  int dx = oxx - fx;
                  if (dx < 0)
                     dx = -dx;
                  if (dx <= near_x)
                     have_ins = 1;
               }
               g_scrub_ins = have_ins && (fy >= (2 * oy70) - (ry + rh));
            }
            int aim_ins = g_scrub_ins;
            for (int i = 0; i < np_glu; i++)
               if (aim_ins)
                  pts[i].hidden = 1;
            for (int i = np_glu; i < np; i++)
               if (!aim_ins)
                  pts[i].hidden = 1;
            /* SPLIT the doses line's shared columns, never the glucose
             * trace's: a dose and a weight logged in the same sitting sit on
             * one pixel, and without this only one of the two could ever be
             * scrubbed. On the glucose series a shared column is the normal
             * state of a multi-day span, and the plain nearest-in-time pick is
             * what keeps a drag to the left edge landing on the oldest sample.
             */
            int idx = plot_hit(rx, ry, rw, rh, pts, np, realtime_s(),
                               g_plot_hours, fx, fy, aim_ins);
            if (idx != g_scrub_idx) {
               g_scrub_idx = idx;
               draw(g_win);
            }
            handled = 1;
         } else if (action == AMOTION_EVENT_ACTION_DOWN) {
            if (act.kind == ACT_OPEN_SETTINGS || act.kind == ACT_MENU ||
                act.kind == ACT_PLOT_TAB) {
               /* Every real main-screen action arms here and fires on the
                * release back on the same control (press_release below). */
               press_arm(act.kind, act.arg, tx, ty);
               handled = 1;
            } else {
               press_cancel(); /* a stale arm must not survive a dead tap */
            }
         } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            if (g_arm_kind != ACT_NONE) {
               press_track(tx, ty);
               handled = 1;
            }
         } else if (action == AMOTION_EVENT_ACTION_UP ||
                    action == AMOTION_EVENT_ACTION_CANCEL) {
            g_scrubbing = 0;
            int akind   = g_arm_kind; /* press_release clears these */
            int aarg    = g_arm_arg;
            int fire = press_release(action == AMOTION_EVENT_ACTION_UP, tx, ty);
            if (g_scrub_idx >= 0) { /* release clears the highlight */
               g_scrub_idx = -1;
               draw(g_win);
               handled = 1;
            } else if (fire) {
               if (akind == ACT_OPEN_SETTINGS) {
                  sys_refresh(); /* snapshot system state before draw (main
                                    thread)*/
                  g_menu = MENU_SETTINGS;
                  sys_set_orientation(0);
                  if (g_win)
                     draw(g_win);
               } else if (akind == ACT_MENU) {
                  /* Main-screen shortcut into a menu action: the info/stats
                   * block taps straight to the primary CGM's device menu, and
                   * the big number itself opens the DEVICES screen. */
                  sys_refresh();
                  sys_set_orientation(0);
                  menu_action(aarg);
                  if (g_win)
                     draw(g_win);
               } else if (akind == ACT_PLOT_TAB) { /* tab (arg = hours) */
                  g_plot_hours = aarg;
                  draw(g_win);
               }
               handled = 1;
            }
         }
      } else if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY &&
                 AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
         /* The system back gesture/button, acting exactly like a tap on the
          * current screen's title-row X. ALWAYS claimed (handled = 1), even
          * when it does nothing: unhandled it falls through to
          * NativeActivity, which finishes the activity -- so on the main
          * screen and the gate "no effect" still has to eat the event. Act
          * on UP, the OS's own back-on-release semantics; the DOWN is
          * claimed silently. */
         if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP && !g_gate) {
            /* Same contract as a touch: a sounding alarm is silenced by ANY
             * press, and that press does nothing else (see the DOWN handler
             * above for why the test and the silence share the lock). */
            int was_sounding = 0;
            alarm_lock();
            was_sounding = g_alarm_sounding;
            if (was_sounding && dexble_alarm_silence()) {
               g_alarm_sounding = 0;
               g_alarm_acked    = 1;
            }
            alarm_unlock();
            int code = was_sounding ? -1 : menu_back_code();
            if (code >= 0) {
               press_cancel();    /* the screen changes under any held finger */
               menu_action(code); /* redraws via its trailing draw() */
               if (g_menu == MENU_NONE)
                  sys_set_orientation(g_orient); /* see the menu rule above */
            }
         }
         handled = 1;
      }
      AInputQueue_finishEvent(q, ev, handled);
   }
   return 1; /* keep the callback registered */
}

/* --- activity callbacks --- */

/* The activity can be destroyed (back-press) while the foreground service
 * keeps the process alive. Without this, the 1 Hz timer keeps firing and
 * derefs the freed g_act (watchdog / notify_update) -- a use-after-free.
 * Remove and close the timer fd and clear g_act/g_win so nothing touches the
 * dead activity; the one-time init is guarded so a later relaunch re-runs
 * onCreate cleanly. */
static void on_destroy(struct ANativeActivity *a)
{
   /* END PAIRING MODE. It is cleared only from menu_action, i.e. only with a
    * live activity, and it gates the ENTIRE advert-driven reconnect
    * (jni_on_advert) plus the scan self-heal. Leaving the pair keypad open
    * and backgrounding the app therefore killed every paired CGM's fast
    * reconnect for the whole background lifetime -- the exact window the
    * foreground service exists to cover -- recoverable only by the 700 s
    * stall watchdog against a 300 s sensor cycle, with nothing on screen to
    * say so. */
   g_smart_pairing = 0;
   (void)a;
   if (g_looper && g_timerfd >= 0)
      ALooper_removeFd(g_looper, g_timerfd);
   if (g_timerfd >= 0)
      close(g_timerfd);
   g_timerfd = -1;
   __atomic_store_n(&g_win, NULL, __ATOMIC_SEQ_CST);
   while (
       __atomic_load_n(&g_draw_busy, __ATOMIC_SEQ_CST)) /* let a draw finish */
      ;
   g_act = NULL;
}

static void on_queue_created(struct ANativeActivity *a, struct AInputQueue *q)
{
   (void)a;
   AInputQueue_attachLooper(q, ALooper_forThread(), 1, on_input, q);
}

static void on_queue_destroyed(struct ANativeActivity *a, struct AInputQueue *q)
{
   (void)a;
   AInputQueue_detachLooper(q);
}

static void on_resume(struct ANativeActivity *a)
{
   g_paused = 0;
   /* scan_restart, NOT start_scan.
    *
    * on_pause's stop_scan leaves g_scanning SET when Java cannot confirm the
    * cancel (deliberately -- it stops the self-heal stacking a second scan
    * client). start_scan then early-returns on that same flag, so the resume
    * silently failed to bring the scan back and the app went on believing
    * one was running. Coming back to the foreground is the user's own
    * recovery gesture and the only path that ever produced a real restart;
    * it must not be the one that no-ops. In the healthy case the stop below
    * is itself a no-op (on_pause already cleared g_scanning), so this costs
    * nothing. */
   scan_restart(a);
   sys_refresh();        /* a permission/settings dialog may have returned */
   if (g_want_battery) { /* first-boot: chain the battery-opt prompt once */
      g_want_battery = 0;
      if (!g_sys_batt)
         sys_request_battery();
   }
   if (g_menu && g_win)
      draw(g_win); /* so the menu reflects the new state at once */
}

static void on_pause(struct ANativeActivity *a)
{
   g_paused = 1; /* set BEFORE the stop, or on_timer could race it back up */
   timer_set(1000, 1000); /* back to the 1 Hz cadence */
   stop_scan(a);
}

static void on_window_created(struct ANativeActivity *a,
                              struct ANativeWindow *win)
{
   (void)a;
   __atomic_store_n(&g_win, win, __ATOMIC_SEQ_CST);
   g_nlines = 0;
   update_screen();
   draw(win); /* force the first frame; update_screen's throttle may skip it
               */
}

static void on_window_destroyed(struct ANativeActivity *a,
                                struct ANativeWindow *win)
{
   (void)a;
   (void)win;
   /* Stop new draws targeting this surface, then wait for any in-flight draw
    * (possibly on a BLE thread) to finish before we return -- the framework
    * frees the struct ANativeWindow once this callback returns, so returning
    * while a draw_impl still holds it would be a use-after-free. draw_impl
    * does no blocking work, so this spin is sub-millisecond. */
   __atomic_store_n(&g_win, NULL, __ATOMIC_SEQ_CST);
   while (__atomic_load_n(&g_draw_busy, __ATOMIC_SEQ_CST))
      ;
}

static void on_redraw_needed(struct ANativeActivity *a,
                             struct ANativeWindow *win)
{
   (void)a;
   draw(win);
}

static void on_window_resized(struct ANativeActivity *a,
                              struct ANativeWindow *win)
{
   (void)a;
   draw(win);
}

static char g_crash_path[256];

/* Async-signal-safe formatting for the crash logger below: pure, no library
 * calls, so nothing here is disallowed inside a signal handler. Append a
 * decimal integer / a bounded string to b[*pos], never past cap. */
static void crash_putn(char *b, int cap, int *pos, long v)
{
   char t[24];
   int i           = 0;
   unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
   do {
      t[i++] = (char)('0' + (int)(u % 10U));
      u /= 10U;
   } while (u && i < (int)sizeof t);
   if (v < 0 && *pos < cap)
      b[(*pos)++] = '-';
   while (i > 0 && *pos < cap)
      b[(*pos)++] = t[--i];
}

static void crash_puts(char *b, int cap, int *pos, const volatile char *s,
                       int max)
{
   for (int i = 0; s && s[i] && i < max && *pos < cap; i++)
      b[(*pos)++] = s[i];
}

/* native crash logger: append signal + a little app context, then re-raise
 * so the OS still records its tombstone (its tombstone carries the
 * timestamp). Retrieve with run-as cat files/crash.log.
 *
 * Everything here is async-signal-safe: the pure helpers above plus open /
 * write / close / signal / raise. NO snprintf (locale/heap) and no
 * non-trivial libc wrappers -- a signal handler may run at any instant,
 * including mid-malloc. */
static void crash_handler(int sig)
{
   char b[200];
   int p = 0;
   crash_puts(b, sizeof b, &p, "CRASH sig=", 200);
   crash_putn(b, sizeof b, &p, sig);
   crash_puts(b, sizeof b, &p, " where=", 200);
   crash_puts(b, sizeof b, &p, g_where, 40);
   crash_puts(b, sizeof b, &p, " status=", 200);
   crash_puts(b, sizeof b, &p, g_status, 24);
   crash_puts(b, sizeof b, &p, " glu=", 200);
   crash_putn(b, sizeof b, &p, g_cur_glu);
   crash_puts(b, sizeof b, &p, " menu=", 200);
   crash_putn(b, sizeof b, &p, g_menu);
   crash_puts(b, sizeof b, &p, " nhist=", 200);
   crash_putn(b, sizeof b, &p, g_nhist);
   crash_puts(b, sizeof b, &p, "\n", 200);
   int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd >= 0) {
      if (write(fd, b, p) != p) {
      }
      close(fd);
   }
   (void)signal(sig, SIG_DFL);
   (void)raise(sig);
}

static void crash_install(const char *dir)
{
   int i = 0;
   for (; dir[i] && i < 230; i++)
      g_crash_path[i] = dir[i];
   const char *f = "/crash.log";
   for (int j = 0; f[j]; j++)
      g_crash_path[i++] = f[j];
   g_crash_path[i] = 0;
   int sigs[]      = {4 /*ILL*/, 6 /*ABRT*/, 7 /*BUS*/, 8 /*FPE*/, 11 /*SEGV*/};
   for (unsigned k = 0; k < sizeof sigs / sizeof sigs[0]; k++)
      (void)signal(sigs[k], crash_handler);
}

/* Read the device's local UTC offset (seconds) into g_tz_off for on-screen
 * timestamps. Split out of ANativeActivity_onCreate; self-contained JNI. */
/* UTC offset in force at instant `at` (epoch seconds), or the current offset
 * when `at` is 0. Returns g_tz_off unchanged on any JNI failure. */
static long tz_offset_at(JNIEnv *env, long at)
{
   long off    = g_tz_off;
   jclass tzc  = (*env)->FindClass(env, "java/util/TimeZone");
   jclass sysc = (*env)->FindClass(env, "java/lang/System");
   if (tzc && sysc) {
      jmethodID get_def = (*env)->GetStaticMethodID(env, tzc, "getDefault",
                                                    "()Ljava/util/TimeZone;");
      jmethodID ctm =
          (*env)->GetStaticMethodID(env, sysc, "currentTimeMillis", "()J");
      jmethodID get_off = (*env)->GetMethodID(env, tzc, "getOffset", "(J)I");
      jobject tz        = (*env)->CallStaticObjectMethod(env, tzc, get_def);
      jlong when =
          at ? (jlong)at * 1000 : (*env)->CallStaticLongMethod(env, sysc, ctm);
      if (tz) /* getDefault can return null; don't call getOffset on it */
         off = (*env)->CallIntMethod(env, tz, get_off, when) / 1000;
      /* Release the local refs. This runs on the per-fingerstick hot path,
       * twice per record, inside jni_notify's frame -- three refs a call
       * adds up even though the frame pop eventually reclaims them. */
      if (tz)
         (*env)->DeleteLocalRef(env, tz);
   }
   if (tzc)
      (*env)->DeleteLocalRef(env, tzc);
   if (sysc)
      (*env)->DeleteLocalRef(env, sysc);
   if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
   return off;
}

/* The offset that was in force when a meter record was TAKEN, not when it
 * was imported. The meter stores naive local time and backfills weeks of
 * records, so using the import-time offset put every pre-transition record
 * exactly one hour out across a DST boundary. Converting needs an instant,
 * and the instant needs the offset, so resolve iteratively: guess with the
 * current offset, ask what the offset was then, and re-derive. Converges in
 * one step except inside the ambiguous repeated hour. */
static long meter_tz_for(long naive)
{
   /* dexble_env(), NOT g_act->env. This runs on a BLE BINDER thread
    * (jni_notify
    * -> ot_on_notify -> ot_drv_reading), and a JNIEnv is valid only on the
    * thread that produced it; g_act->env belongs to the main looper. Using
    * it here aborts under CheckJNI and corrupts the main thread's local-ref
    * frame otherwise -- on the hot path of every single fingerstick import.
    */
   JNIEnv *env = dexble_env();
   if (!env)
      return g_tz_off;
   /* Seed from a FIXED guess, not from g_tz_off: the conversion must be a
    * pure function of `naive`, or the same meter record converts to two
    * different instants depending on when it was imported (g_tz_off drifts
    * across a DST change, and is stale entirely while the activity is
    * destroyed). BGM dedup requires an EXACT timestamp match, so a
    * non-deterministic conversion means a re-imported record does not dedup
    * even well inside the display window.
    */
   long off = tz_offset_at(env, naive + OT_EPOCH);
   return tz_offset_at(env, naive + OT_EPOCH - off);
}

static void init_tz_offset(JNIEnv *env)
{
   g_tz_off = tz_offset_at(env, 0);
}

/* NDK entry point: resolved by name by the Android runtime when the .so
 * loads. Explicitly exported (we build -fvisibility=hidden); it is external
 * by necessity, which is also why it cannot be given internal linkage. */
__attribute__((visibility("default"))) void
ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved,
                         size_t saved_size)
{
   (void)saved;
   (void)saved_size;
   JNIEnv *env = activity->env;

   /* This runs on the main/UI looper thread; record it so
    * draw()/update_screen() can tell it apart from BLE binder threads (see
    * on_main). The looper callbacks (on_timer/on_input) are registered on
    * this same thread below. */
   g_main_tid = gettid();
   g_act      = activity;
   /* Stale-alarm grace starts at PROCESS start, not at every onCreate.
    *
    * Re-arming it on each activity launch meant that opening the app to see
    * why the alarm was sounding silenced it: the next heartbeat recomputed
    * grace = 1, dropped g_disc_alarmed, and alarm_apply issued a silence --
    * then refused to re-raise for the whole threshold (up to 60 min) with
    * the sensor still dead. The service keeps running across activity
    * destruction, so the grace period must not restart with the UI. */
   if (!g_launch_t)
      g_launch_t = realtime_s();
   crash_install(activity->internalDataPath ? activity->internalDataPath
                                            : "/data/local/tmp");

   /* local timezone offset (seconds), for on-screen timestamps */
   init_tz_offset(env);

   activity->callbacks->onResume                   = on_resume;
   activity->callbacks->onPause                    = on_pause;
   activity->callbacks->onNativeWindowCreated      = on_window_created;
   activity->callbacks->onNativeWindowDestroyed    = on_window_destroyed;
   activity->callbacks->onNativeWindowRedrawNeeded = on_redraw_needed;
   activity->callbacks->onNativeWindowResized      = on_window_resized;
   activity->callbacks->onInputQueueCreated        = on_queue_created;
   activity->callbacks->onInputQueueDestroyed      = on_queue_destroyed;
   activity->callbacks->onDestroy                  = on_destroy;

   /* Process-wide, one-time setup: JNI globals, the BLE driver, and the
    * loaded history/settings. The foreground service can outlive the
    * activity, so a relaunch re-enters onCreate in the same process -- guard
    * this so we don't leak the g_ble global ref, re-register natives, or
    * reload the history. */
   if (!g_inited) {
      jclass ble = find_app_class(activity, "com.jk.pancra.Ble");
      if (!ble) {
         LOGI("Ble class NOT found");
         set_status("NO BLE CLASS!");
         return;
      }
      g_ble = (*env)->NewGlobalRef(env, ble);

      /* char[] (not literals) so the char* JNINativeMethod fields need no
       * const cast */
      static char nm_advert[] = "onAdvert";
      static char sg_advert[] = "(Ljava/lang/String;Ljava/lang/String;I)V";
      static const JNINativeMethod methods[] = {
          {nm_advert, sg_advert, (void *)jni_on_advert},
      };
      if ((*env)->RegisterNatives(env, g_ble, methods, 1) != 0) {
         LOGI("RegisterNatives failed");
         set_status("JNI REG FAILED!");
         return;
      }
      g_scan = (*env)->GetStaticMethodID(
          env, g_ble, "scan", "(Landroid/content/Context;)Ljava/lang/String;");
      g_stop = (*env)->GetStaticMethodID(env, g_ble, "stop", "()Z");
      m_show_glucose =
          (*env)->GetStaticMethodID(env, g_ble, "showGlucose",
                                    "(Landroid/content/Context;Ljava/lang/"
                                    "String;Ljava/lang/String;Ljava/lang/"
                                    "String;[IIII)V");
      m_set_orient = (*env)->GetStaticMethodID(env, g_ble, "setOrientation",
                                               "(Landroid/content/Context;I)V");
      m_export = (*env)->GetStaticMethodID(env, g_ble, "exportData",
                                           "(Landroid/content/Context;JZZZZ)V");
      m_perm_granted = (*env)->GetStaticMethodID(
          env, g_ble, "permGranted",
          "(Landroid/content/Context;Ljava/lang/String;)Z");
      m_req_perm = (*env)->GetStaticMethodID(
          env, g_ble, "requestPerm",
          "(Landroid/content/Context;Ljava/lang/String;)V");
      m_open_settings = (*env)->GetStaticMethodID(
          env, g_ble, "openAppSettings", "(Landroid/content/Context;)V");
      m_batt_ok = (*env)->GetStaticMethodID(env, g_ble, "isBatteryUnrestricted",
                                            "(Landroid/content/Context;)Z");
      m_req_batt = (*env)->GetStaticMethodID(env, g_ble, "requestBatteryOpt",
                                             "(Landroid/content/Context;)V");
      m_bucket   = (*env)->GetStaticMethodID(env, g_ble, "standbyBucket",
                                             "(Landroid/content/Context;)I");
      m_bg_restricted = (*env)->GetStaticMethodID(
          env, g_ble, "isBgRestricted", "(Landroid/content/Context;)Z");
      m_bonded_stelo = (*env)->GetStaticMethodID(
          env, g_ble, "bondedSensor",
          "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;");
      /* (No remote* ids: the sync client's two entry points are registered
       * as NATIVES on the Ble class by dexble_register, and its transport is
       * looked up in syncjni_wire.) */

      /* wire up the BLE protocol driver (registers its own Ble callbacks) */
      dexble_init(activity->internalDataPath ? activity->internalDataPath
                                             : "/data/local/tmp");
      if (!dexble_register(env, g_ble, activity->clazz))
         LOGI("dexble_register failed");

      /* Robust reconnect: if we hold a key (bonded) but have no saved sensor
       * MAC -- e.g. after an app update that added MAC persistence, or if
       * the saved MAC was lost -- resolve the sensor's address from the
       * system bond list (reliable names) and lock onto it, so we never fall
       * back to guessing from adverts (whose local name is usually absent).
       *
       * The name prefix comes from the registry, i.e. from the family the
       * user actually paired, so this can never latch onto a bonded sensor
       * they did not choose. */
      /* Alarm class must come through the app's own classloader (see
       * find_app_class) */
      dexble_set_alarm(env, find_app_class(activity, "com.jk.pancra.Alarm"));

      /* persistent reading log: remember our own datapoints across restarts.
       * Internal storage; the app is debuggable, so retrieve with
       *   adb shell run-as com.jk.pancra cat files/readings.csv >
       * readings.csv
       */
      {
         const char *dir = activity->internalDataPath
                               ? activity->internalDataPath
                               : "/data/local/tmp";
         data_path(g_store_path, sizeof g_store_path, dir, "/readings.csv");
         data_path(g_info_path, sizeof g_info_path, dir, "/stelo.info");
         data_path(g_alarm_path, sizeof g_alarm_path, dir, "/alarm.cfg");
         data_path(g_settings_path, sizeof g_settings_path, dir,
                   "/settings.cfg");
         data_path(g_code_path, sizeof g_code_path, dir, "/paircode.txt");
         data_path(g_sensors_path, sizeof g_sensors_path, dir, "/sensors.csv");
         data_path(g_slots_path, sizeof g_slots_path, dir, "/slots.csv");
         data_path(g_meter_path, sizeof g_meter_path, dir, "/meter.idx");
         data_path(g_metersync_path, sizeof g_metersync_path, dir,
                   "/meter.sync");
         data_path(g_sess_path, sizeof g_sess_path, dir, "/session.cache");
         data_path(g_calq_path, sizeof g_calq_path, dir, "/cal.q");
         data_path(g_rescale_path, sizeof g_rescale_path, dir, "/rescale.cfg");
         data_path(g_remote_path, sizeof g_remote_path, dir, "/remote.cfg");
         remote_load(); /* remote-push server config */
         data_path(g_ins_path, sizeof g_ins_path, dir, "/insulin.csv");
         insulin_load(); /* doses: pre-populates the LOG INSULIN form */
         data_path(g_wt_path, sizeof g_wt_path, dir, "/weight.csv");
         weight_load();
         sensors_load(); /* before store_load: readings resolve through it */
         /* Bonded-MAC recovery runs HERE, after sensors_load().
          *
          * It asks the OS bond list for a device whose name matches the
          * primary sensor's family prefix -- but it used to run before the
          * registry was read, so g_nslot was always 0, the primary always
          * resolved to -1, and the prefix was always "DX01". A G7-only
          * user's bonded device was therefore never found and could never
          * reconnect; a user with both got LINK_CGM locked onto the Stelo's
          * address while the Stelo already owned its own link, leaving two
          * links reporting the same MAC and collapsing the address-based
          * routing. */
         {
            struct dex_session s;
            driver_lock();
            /* Select explicitly rather than trusting the ambient link (the
             * GATT callbacks select and never restore). Getting this wrong
             * here would let a second sensor's session suppress the
             * primary's MAC recovery entirely. */
            driver_select(LINK_CGM);
            driver_get_session(&s);
            driver_unlock();
            if (!s.mac[0] && s.paired && m_bonded_stelo) {
               const char *want = "DX01";
               int pidx         = sensor_primary_slot();
               if (pidx >= 0) {
                  const struct sensor_rec *pr =
                      sensor_rec_by_id(g_slot[pidx].id);
                  if (pr && pr->type == SENSOR_G7)
                     want = "DXCM";
               }
               jstring jpfx = (*env)->NewStringUTF(env, want);
               jstring jm   = (*env)->CallStaticObjectMethod(
                   env, g_ble, m_bonded_stelo, activity->clazz, jpfx);
               /* getBondedDevices() throws SecurityException if
                * BLUETOOTH_CONNECT was revoked after pairing; clear it so
                * the next JNI call (find_app_class for the Alarm class) is
                * not made with an exception pending. */
               if ((*env)->ExceptionCheck(env))
                  (*env)->ExceptionClear(env);
               if (jm) {
                  const char *bm = (*env)->GetStringUTFChars(env, jm, NULL);
                  if (bm && bm[0]) {
                     LOGI("locked to bonded sensor %s (%s)", bm, want);
                     /* dexble_register() has already run, so GATT callbacks
                      * can be firing; drv_mac_save resolves its path via
                      * driver_link(). */
                     driver_lock();
                     driver_select(LINK_CGM);
                     drv_mac_save(bm);
                     driver_lock_mac(bm);
                     driver_unlock();
                  }
                  if (bm)
                     (*env)->ReleaseStringUTFChars(env, jm, bm);
                  (*env)->DeleteLocalRef(env, jm);
               }
               if (jpfx)
                  (*env)->DeleteLocalRef(env, jpfx);
            }
         }
         /* Seeded per meter at CONNECT time (see the advert handler); this
          * is just a safe initial state before any meter is selected. */
         ot_init(-1);
         /* UNDER hist_lock: store_load rewrites g_hist wholesale, and this
          * runs AFTER dexble_register, whose own comment notes GATT
          * callbacks can already be firing. Today the first reading cannot
          * realistically land inside the load window (scan, timer and
          * service tick all start later), so this was latent rather than
          * live -- but store.h states the contract and relying on startup
          * ordering to satisfy it is exactly the kind of reasoning that goes
          * stale when the ordering changes. */
         hist_lock();
         store_load();
         hist_unlock();
         /* MIGRATION: any source that has readings on the plot but no slot
          * (a device forgotten before slot-retention existed) becomes an OLD
          * device -- a retired slot -- so it gets the full per-device menu
          * and consistent styling like everything else. Bounded by the
          * readings window and MAX_SLOTS. */
         {
            hist_lock();
            int orphans[NHIST];
            int no = 0;
            for (int i = 0; i < g_nhist; i++) {
               int src = g_hist[i].src;
               if (src <= 0)
                  continue;
               int seen = 0;
               for (int j = 0; j < no; j++)
                  if (orphans[j] == src) {
                     seen = 1;
                     break;
                  }
               if (!seen && no < NHIST)
                  orphans[no++] = src;
            }
            hist_unlock();
            for (int j = 0; j < no; j++) {
               int id = orphans[j];
               sensors_lock();
               int known                  = sensor_slot_by_id(id) != 0;
               const struct sensor_rec *r = known ? 0 : sensor_rec_by_id(id);
               int type                   = r ? r->type : SENSOR_STELO;
               char ident[24];
               str_snapshot(ident, sizeof ident, r ? r->identity : "");
               sensors_unlock();
               if (known || !r)
                  continue;
               int idx = sensor_claim_slot(id, type, ident);
               if (idx >= 0) {
                  sensors_lock();
                  g_slot[idx].old     = 1;
                  g_slot[idx].primary = 0;
                  slots_save();
                  sensors_unlock();
               }
            }
         }
         stat_load(g_store_path);
         info_load();
         alarm_load();
         /* The paired identity survives a restart; without this the first
          * sync after every launch would be unsigned and refused. */
         sync_set_key(g_sync_uid, g_sync_key);
         settings_load();
         calq_load();       /* resume any durably-queued calibration */
         rescale_load();    /* restore active rescale factor */
         meter_sync_load(); /* restore per-meter last-sync times */
         sess_load();       /* PRED + session clock survive the restart */
         code_load();
         sys_set_orientation(g_orient); /* restore last-chosen orientation */
         g_stored = store_count();
         LOGI("reading log: %s (%d in memory, %d stored)", g_store_path,
              g_nhist, g_stored);
         /* store_load restored g_cur_glu/g_cur_time -- the big number shows
          * it immediately, so the ongoing notification must too. It is
          * dirty-driven off new readings, which have not arrived yet at cold
          * start, so mark it dirty here to seed the notification text from
          * the restored reading rather than leaving it "no recent reading"
          * until the first live sample. */
         if (g_cur_glu >= 0)
            g_notify_dirty = 1;
      }
      g_inited = 1;
   }

   /* Window flags are per-window, so this must run on every onCreate -- not
    * just the first -- and only once settings_load() has supplied the user's
    * choice. Holding the screen on also keeps the foreground scan alive
    * between advertising bursts, which is why it is the default. */
   apply_screen_on();

   /* 1 Hz repaint timer so AGE / stale state stay current without a touch.
    * On an activity relaunch in the same process, tear down any prior timer
    * first so we don't leak an fd + looper callback that fires with a stale
    * g_act (see on_destroy). */
   g_looper = ALooper_forThread();
   if (g_timerfd >= 0) {
      ALooper_removeFd(g_looper, g_timerfd);
      close(g_timerfd);
      g_timerfd = -1;
   }
   {
      int tfd = timerfd_create(CLOCK_MONOTONIC, 04000 /* TFD_NONBLOCK */);
      if (tfd >= 0) {
         g_timerfd = tfd;
         struct itimerspec its;
         its.it_value.tv_sec     = 1;
         its.it_value.tv_nsec    = 0;
         its.it_interval.tv_sec  = 1;
         its.it_interval.tv_nsec = 0;
         timerfd_settime(tfd, 0, &its, 0);
         ALooper_addFd(g_looper, tfd, 3, ALOOPER_EVENT_INPUT, on_timer, 0);
      }
   }

   /* Don't fire the system permission dialog on cold start. Show the
    * rationale screen first; CONTINUE (in on_input) issues the actual
    * request. */
   if (!has_ble_permissions(activity)) {
      g_gate = 1;
      /* Force portrait, exactly as opening any other modal screen does. The
       * gate's fixed 15-line body is laid out at a width-derived scale, so
       * in landscape its CONTINUE button falls below the buffer -- and that
       * button is the screen's ONLY hit target, with on_input swallowing
       * every other press while g_gate is set. The result was an
       * unrecoverable dead screen.
       *
       * This is reachable well beyond first run: g_gate is armed whenever
       * the BLE permissions are missing, including Android's automatic
       * revocation for unused apps, and the persisted orientation is
       * restored just above. A user who had chosen LANDSCAPE would relaunch
       * straight into the wedge, and rotating the phone could not help
       * because the app requests the orientation itself. */
      sys_set_orientation(0);
   }
}
