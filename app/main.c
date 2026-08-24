// SPDX-License-Identifier: GPL-3.0
// main.c --- Native app core: UI rendering, state, and JNI wiring
// Copyright 2026 Jakob Kastelic

/* pancra native core, plain C -- no NDK glue.
 * Implements ANativeActivity_onCreate directly; links against stub
 * libc/libandroid/liblog (stub_*.c) -- the phone binds the real bionic ones.
 * jni.h comes from the host JDK (same ABI as Android's).
 *
 * WHAT IS HERE, and what is not. This file owns the WIRING:
 * the looper and its 1 Hz tick, the JNI entry points, the menu action table,
 * the alarm ACTUATION (the decisions are alarmlogic.c), and the model funnel
 * that turns all of that global state into the immutable struct the renderer
 * renders from. The subjects themselves have their own files -- the renderer
 * the rendering, settings.c the persisted state, store.c/stats.c the history
 * and its figures, calib.c the corrections, sensors.c the registry, crashlog.c
 * and tzoff.c the two things a signal handler and the JVM respectively own.
 *
 * All rendering runs on the main looper thread (see on_main); BLE
 * binder-thread updates just mark the screen dirty for the next 1 Hz repaint.
 */
#include "alarm.h"
#include "alarmcfg.h"
#include "blejni.h"
#include "bletrans.h"
#include "calib.h"
#include "clock.h"
#include "crashlog.h"
#include "devinfo.h"
#include "devtag.h" /* a log may not carry an address; see there */
#include "dexdriver.h"
#include "dexlibc.h"
#include "exercise.h"
#include "food.h"
#include "input.h"
#include "insulin.h"
#include "jbridge.h"
#include "linkhealth.h" /* pancra_link_watchdog: the shell's tick drives it */
#include "loadresult.h" /* LOAD_*: init_data combines the load verdicts */
#include "log.h"
#include "menu.h"
#include "menuview.h"
#include "meter.h"
#include "model.h"
#include "nav.h"
#include "ndk.h"
#include "notify.h"
#include "otble.h"
#include "paircode.h"
#include "pairing.h"
#include "readingrec.h" /* struct reading: one stored sample */
#include "reconcile.h"  /* long-span plot data, bucketed from the log */
#include "remote.h"     /* pancra_remote_sync: ...and so does the service's */
#include "remotecfg.h"
#include "scan.h"
#include "scanlogic.h"
#include "sensors.h"
#include "sesscache.h"
#include "settings.h"
#include "shell.h"
#include "stategen.h"   /* state.gen: one generation, for a backup */
#include "stats.h"
#include "status.h"
#include "store.h"
#include "sync.h"
#include "sysabi.h" /* the kernel clock and timer ABI, declared once */
#include "thread.h" /* the ONLY cross-thread primitives; read its header */
#include "tzoff.h"
#include "ui.h"
#include "uimodel.h"
#include "util.h"
#include "weight.h"
#include <jni.h>
#include <stdatomic.h> /* the cross-thread fields; see thread.h */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- app configuration constants (tunables collected here) ---- */
#define SCR_MAX 16 /* touch hit-boxes tracked per drawn menu */
#define NPERMS  3  /* runtime permissions requested at once */
/* How long a teardown callback will wait for another thread to finish with
 * shared state before it stops waiting (thread.h, rule 5). Long enough that a
 * history append -- microseconds -- always completes; short enough that the
 * main thread is never the reason the app is slow to close. */
#define TEARDOWN_DRAIN_MS 50

/* The kernel timer this file drives is declared in app/sysabi.h, with the
 * clock ids and the timerfd flags -- one declaration of that ABI for the
 * whole app, and one list of assertions checked against the pinned NDK's own
 * headers. */

/* --- screen model: a handful of text lines, redrawn on change --- */

static struct ANativeActivity *g_act;

/* ================= THE CROSS-THREAD INVENTORY ============================
 *
 * Every field below is touched by more than one of the threads named in
 * thread.h. Each line says WHO may write it and under WHAT. Anything not in
 * this list, and not obviously derived from something in it, is single-
 * threaded state owned by MAIN -- which is most of this file.
 *
 *   g_win            MAIN writes (window created/destroyed), any thread may
 *                    read. Atomic pointer, seq_cst on both sides; see draw().
 *   g_main_tid       MAIN writes (onCreate, and re-affirmed by on_timer),
 *                    every thread reads it to answer on_main(). Atomic.
 *   g_ui_dirty       ANY thread raises, MAIN takes. `struct flag`.
 *   g_notify_dirty   ANY thread raises, MAIN or SERVICE takes -- whichever
 *                    ticks first. `struct flag`, and the take must be an
 *                    exchange or one consumer can swallow the other's work.
 *   g_remote_last_ok WORKER writes (a push the server acknowledged), MAIN
 *                    reads for the settings row. Atomic long, relaxed: one
 *                    frame of staleness in an age display is not a defect.
 *   g_where          ANY thread writes, the SIGNAL handler reads. Atomic
 *                    pointer to a string LITERAL, so the reader can never
 *                    chase a pointer to freed or half-written memory.
 *   g_hist_lk        the reading history and everything drawn from it.
 *   g_devlist_lk     g_devs / g_ndevs, the pairing-candidate list.
 *   alarm_lk         the alarm decision AND its actuation, end to end.
 *   g_*_flight       single-flight latches; see thread.h.
 *
 * ======================================================================== */

/* The surface. MAIN owns it; every other thread only ever compares against it
 * to decide whether the frame it was about to paint is still the live one. */
static _Atomic(struct ANativeWindow *) g_win;

/* The looper's thread id, so any thread can ask whether it IS the looper.
 * Written by MAIN only; read everywhere. Relaxed on the read is enough: the
 * value is either 0 (nobody has claimed it, so nothing is the main thread yet)
 * or a tid that never changes afterwards. */
static atomic_int g_main_tid;

/* Repaint wanted. Raised by whichever thread changed something visible,
 * consumed by the 1 Hz timer on MAIN. */
static struct flag g_ui_dirty = FLAG_INIT;

static int on_main(void)
{
   int tid = atomic_load_explicit(&g_main_tid, memory_order_relaxed);
   return tid != 0 && gettid() == tid;
}

/* The live surface, or NULL if there is none right now.
 *
 * Every read of g_win goes through here so the ordering is argued once:
 * ACQUIRE, paired with the releasing stores in on_window_created and
 * on_window_destroyed, so a thread that sees a window also sees everything set
 * up for it. Reading the variable directly at forty call sites is how one of
 * them ends up being the plain load that a future non-main-thread writer makes
 * wrong. */
static struct ANativeWindow *live_window(void)
{
   return atomic_load_explicit(&g_win, memory_order_acquire);
}

/* THE HISTORY LOCK lives with the history it protects (store.c). These two
 * spellings are kept because fifty call sites read better as hist_lock() than
 * as store_lock() -- and because "the history lock" is what every comment in
 * this file calls it.
 *
 * It is NOT a flag here doing three unrelated jobs at once -- guarding the
 * reading history, serialising draws, and a surface-lifetime handshake at
 * teardown. See on_window_destroyed for why the third is not a job at all,
 * and draw() for why the first two are not one critical section. */
static void hist_lock(void)
{
   store_lock();
}

static void hist_unlock(void)
{
   store_unlock();
}

/* The activity is paused (on_pause -> on_resume). While paused the scan stays
 * down deliberately; the self-heal in on_timer must not fight that. */
static int g_paused;
/* The Ble.java class and its method ids live in jbridge.c -- see the header
 * there for why none of this belongs in the shell. */
/* REMOTE sync: cursor read, batch push, and the in-flight/backoff gate. */

/* Execution checkpoint: set to a static label at the top of each hot code path,
 * so the crash handler can record WHERE we were when a fault hit (debuggerd
 * tombstones are SELinux-locked here).
 *
 * ATOMIC, not volatile. It is written by every thread and read by the SIGNAL
 * handler, which may interrupt any of them at any instruction; volatile stops
 * the compiler discarding the store but says nothing about a torn pointer.
 * Relaxed is right: there is no other state to order against it -- the value
 * IS the message, and it always points at a string literal, so the handler can
 * never chase a pointer into memory that has gone away. */
static const char *_Atomic g_where = "boot";

/* Named so the ordering argument above is made once rather than at each of the
 * dozen checkpoints. */
static void where(const char *label)
{
   atomic_store_explicit(&g_where, label, memory_order_relaxed);
}

/* The same, under its shell.h name, for the files split out of here. */
void shell_where(const char *label)
{
   where(label);
}

/* When this PROCESS started, for the stale-alarm grace period and the link
 * watchdog's "how long has this been quiet" when nothing has ever been heard.
 * Set once and never re-armed: the service outlives the activity, and
 * re-arming it on each launch meant that opening the app to see why the alarm
 * was sounding silenced it for a whole threshold.
 *
 * MONOTONIC (mono_s()), because both users measure an ELAPSED INTERVAL and
 * neither ever names an instant. On the wall clock an NTP step -- routine
 * moments after boot, which is when this is set -- moved both at once: it
 * could end the DISCONNECT grace early and alarm over data that was simply
 * waiting for the first sync, or extend the grace and thereby disable that
 * alarm outright for the length of the skew. `make clockcheck` names this
 * variable so it cannot quietly go back to realtime_s(). */
static long g_launch_mono;

long shell_launch_mono(void)
{
   return g_launch_mono;
}

/* dexble transport prototypes come from bletrans.h and blejni.h; driver_*
 * from dexdriver.h */

/* reading history + current-reading snapshot live in store.c (see store.h) */
static long g_tz_checked; /* when tz_off_now() was last refreshed */

/* alarm thresholds (mg/dL, adjustable in the UI) + their button hit boxes */

/* alarm kind passed to dexble_alarm() / Alarm.trigger() (keep in sync with
 * Alarm.java) */
/* THE LEVELS ARE alarmlogic.h's (AL_*), and Java's kind is reached only
 * through alarm_java_kind. A second enumeration here is what put a zero-valued
 * level beside the "nothing should sound" sentinel, which is a low-glucose
 * alarm that cannot fire. */

/* ONE SCREEN IDENTITY, and only one.
 *
 * A second enumeration here with thirty-odd MENU_* names, alongside `enum
 * ui_screen` with the SAME thirty-odd screens named SCR_*, needs a
 * hand-written switch to translate one into the other -- a table whose only
 * job is to say that the weight-log entry of one enumeration and the
 * weight-log entry of the other are the same screen.
 *
 * That is not a mapping, it is a duplicate, and it has the failure duplicates
 * always have: a screen added on one side and forgotten on the other. What
 * that costs, concretely: a WEIGHT screen missing from a hand-written map has
 * a dead back key, and only the X gets out.
 *
 * So there is one enum (uimodel.h), and adding a screen is one edit. What made
 * the duplicate survivable -- naming the type so -Wswitch-enum can see every
 * map over it -- is kept: the maps that remain are still exhaustive switches
 * with no `default:`, so a screen without a back code is still a build
 * error. */

/* Where the ALARM submenu was opened from -- the settings row, or the main
 * screen's alarm row -- so its X returns exactly there (the origin rule). */

/* Where the LOG/EDIT INSULIN form was OPENED from -- every exit (X,
 * CANCEL, DELETE, CONFIRM) returns exactly there. Recorded at each entry
 * point, never inferred: inferring the return target is the recurring
 * menu-navigation bug this app keeps re-growing. */
/* WHERE THE FOUR LOGGING SCREENS WERE OPENED FROM.
 *
 * Hardcoding SCR_ADDMENU is true only while the ADD menu is their only door.
 * The main screen's PIN buttons are a second door, so a hardcoded return
 * drops the user into a menu they never opened -- the exact
 * failure the record-the-origin rule exists to prevent, and one this codebase
 * has now hit on the devices screen, the sensor screen and the pairing flow.
 * Captured at open, never inferred at close. */

static int g_gate; /* first-run permission-rationale screen */

static int g_timerfd = -1; /* shared repaint / repeat timer */
static struct ALooper
    *g_looper;       /* main looper, to remove the timer fd on destroy */
static int g_inited; /* process-wide one-time init done (relaunch guard) */

/* draw_str / fmt_glu / str_snapshot live in the renderer (rendering primitives)
 */
#define UNIT_LBL (sp.units ? "MMOL/L" : "MG/DL")

/* Where the per-device menu was opened FROM, so closing it returns there:
 * SCR_DEVICES when reached via the device list, SCR_OLDDEV via the old-device
 * list, SCR_MAIN (main screen) when reached via the STATE/STORED info-block
 * shortcut. */
/* Where the DEVICES screen was opened FROM -- SCR_MAIN for the main screen's
 * big number, SCR_SETTINGS for the settings row -- so its X returns exactly
 * there. Recorded, never inferred (the recurring bug). */

/* Map a sensor slot to its transport link. CGMs take LINK_CGM, then LINK_CGM2
 * and upward in slot order; meters take links from the SAME pool. Each link has
 * its
 * own GATT connection, operation queue and driver context, so sensors run
 * genuinely concurrently rather than taking turns. */

/* Reconcile the driver's live session against the permanent registry: mint an
 * id for a sensor we have not recorded yet, claim a slot for it, and remember
 * that id as the source stamped onto its readings. Runs on the main thread from
 * the 1 Hz timer, so it never races a BLE-thread reading.
 *
 * Minting is keyed on identity + firmware (NOT activation -- see sensor_mint),
 * so a new physical sensor gets its own id while a live one keeps hers. */

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

static void draw_impl(struct ANativeWindow *win, const struct screen *sm);

/* draw() is called from the main looper (on_timer at 1 Hz, on_input) AND from
 * BLE binder threads (status/reading updates flow through set_status/
 * pancra_glucose -> draw). struct ANativeWindow (a BufferQueue producer) is NOT
 * safe for concurrent access: two threads locking the surface at once corrupts
 * the returned buffer and segfaults. Serialise with a lock-free guard -- if a
 * draw is already running on another thread, drop this frame; the 1 Hz timer
 * (and the next event) repaint the latest state immediately after.
 *
 * WHAT SERIALISES A DRAW AGAINST THE SURFACE BEING FREED is the on_main()
 * guard below, not the lock. Only the main looper ever reaches draw_impl, and
 * on_window_destroyed is a main-looper callback too, so a draw can no more be
 * in flight while the surface is torn down than a function can be running
 * twice on one thread. The re-check of g_win stays: it is what stops a frame
 * that was ALREADY under way on this thread from painting into a surface the
 * framework has since replaced.
 *
 * (No second claim is needed -- a BLE thread cannot be inside draw_impl, and
 * the spin in on_window_destroyed that would cover it is a plain main-thread
 * stall now that rendering is on the main thread. See on_window_destroyed.)
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
      flag_raise(&g_ui_dirty);
      return;
   }
   if (win != live_window())
      return; /* not the live surface any more */

   /* SNAPSHOT, THEN RENDER -- and the order is the point.
    *
    * The history must not change under a frame: build_model reads it
    * directly, so a reading landing halfway through would draw a plot from
    * two different histories. That was arranged by holding the lock across
    * the WHOLE of draw_impl -- including ANativeWindow_lock, which blocks in
    * dequeueBuffer whenever the BufferQueue is full. That is routine during a
    * system animation or an orientation change and lasts tens to hundreds of
    * milliseconds, and for every one of them a GATT binder thread with a
    * reading to deliver was spinning on this lock waiting for the compositor.
    *
    * `struct screen` is an immutable frame, which is what lets "the model is
    * consistent" and "the surface is mine" be two questions. The lock covers
    * only the model build -- microseconds of array copying, no syscall, no
    * JNI -- and the render works from the copy.
    *
    * trylock, not lock: a frame is disposable and the 1 Hz timer repaints
    * immediately after, so waiting here would queue the main thread behind a
    * binder thread for a frame nobody would miss. */
   struct screen sm;
   if (!model_frame(&sm))
      return; /* somebody else has the history; the timer will come round */
   draw_impl(win, &sm);
}

static void draw_impl(struct ANativeWindow *win, const struct screen *sm)
{
   struct ANativeWindow_Buffer buf;

   if (!win)
      return;
   ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBA_8888);
   if (ANativeWindow_lock(win, &buf, NULL) != 0)
      return;

   /* Every screen is the pure UI: render the immutable frame model the caller
    * built and record the touch targets. The model is built by draw(), under
    * the history lock and BEFORE the surface lock above -- see the comment
    * there for why those two must not be the same critical section.
    * ui_render clears the framebuffer itself. */
   ui_render(&buf, sm, input_hits());
   /* Resting frames run at 13/16 intensity so the armed full-intensity
    * highlight below is visible even on white text and the green big
    * number, which have no headroom at full brightness. */
   ui_dim(&buf);
   /* Pressed-but-not-yet-fired: shade the armed control (act-on-release --
    * see g_arm_*). Re-found in the JUST-rebuilt hit boxes via the finger's
    * last on-target point, and only shaded while it still resolves to the
    * same action -- a redraw that moved or removed the control drops the
    * shade rather than lighting a stranger. */
   input_press_overlay(&buf);

   ANativeWindow_unlockAndPost(win);
}

/* FindClass inside struct ANativeActivity callbacks resolves via the
 * framework's class loader, which can't see app classes; go through the
 * activity's own loader instead. Takes a dotted name ("com.jk.pancra.Ble").
 *
 * THE SIX JNI CALLS IT TAKES LIVE IN jb_app_class, with the rest of this
 * app's JNI marshalling. Written here as one unbroken chain checked only at
 * the end, a null loader or a pending exception from step two feeds steps
 * three through six -- an abort under CheckJNI -- and the four intermediate
 * local refs leak even when it works. Both are properties of a JNI sequence,
 * not of the
 * shell, and jbridgetest can drive them with a fake JNIEnv; nothing could
 * reach them while they were a static function in here. */
static jclass find_app_class(struct ANativeActivity *a, const char *name)
{
   jclass cls = NULL;
   if (!a)
      return NULL;
   (void)jb_app_class(a->env, a->clazz, name, &cls);
   return cls; /* NULL unless every step of the lookup answered */
}

/* The same three, under their shell.h names, for the files split out of here
 * (see that header: this list is meant to stay short). */
int shell_on_main(void)
{
   return on_main();
}

void shell_gate_arm(void)
{
   g_gate = 1;
}

int shell_gate(void)
{
   return g_gate;
}

void shell_gate_done(void)
{
   g_gate = 0;
}

void shell_repaint(void)
{
   struct ANativeWindow *w = live_window();
   if (w)
      draw(w);
}

/* THE SERVICE HEARTBEAT'S WORK, in the shell that owns the list. See shell.h
 * for why the transport does not.
 *
 * Every one of these must keep running with NO ACTIVITY ALIVE: the activity's
 * looper is destroyed on back-press or task-swipe while the foreground
 * service goes on for days, and each of these was, at some point, only on
 * that looper -- which is how a hypo came to be decoded and logged with no
 * sound, and how a phone with the app backgrounded stopped pushing until it
 * was reopened. */
void shell_service_tick(void)
{
   /* The stale-data alarm is triggered by the ABSENCE of readings, so
    * something must evaluate it on a timer rather than on a notification. */
   pancra_alarm_check();
   /* The lock-screen notification is the only glucose display left once the
    * activity is gone. */
   pancra_notify_refresh();
   /* Repair stranded links. on_timer does this too, on the activity's looper,
    * which is exactly the one that dies. */
   pancra_link_watchdog();
   /* Push. Same reason: with the app backgrounded nothing left the phone
    * until it was reopened. */
   pancra_remote_sync();
   /* Re-arm a meter's standing connect, and time out a wedged exchange. */
   meter_sync_watchdog();
   /* Register a sensor that bonded while the UI was gone. */
   pancra_reconcile_tick();
   /* THE GENERATION STAMP a backup reads before and after it pulls (item
    * 247). Here rather than only on the activity's timer for the usual
    * reason: the writes it is stamping arrive on a binder thread with no
    * activity alive, which is exactly when a backup is likely to be taken. */
   stategen_tick();
   /* The settled exercise level, for the case the activity is gone -- which
    * is the case this control is designed around. See on_timer. */
   (void)exercise_button_tick(realtime_s(), mono_s(), tz_off_now());
   /* Expire a stale calibration or rescale target, and RETRY any automatic
    * transition that could not be written. on_timer does this too, on the
    * activity's looper -- and that is exactly the looper that dies. A
    * reading arriving on a binder thread with no activity alive is the most
    * likely way to get an unsaved transition in the first place, so leaving
    * the retry on the activity's timer left it dead in the one case it
    * exists for. */
   calib_tick();
   /* WRITE OUT WHAT THE MODEL HAS LEARNED, once an hour.
    *
    * Not on every sample: it is 3200 numbers, and what a crash between two
    * saves costs is up to an hour of learning on a model that takes weeks to
    * converge -- which is nothing beside writing 80 KB every five minutes for
    * the life of the phone.
    *
    * ON THE SERVICE TICK, deliberately, because the service is the half that
    * outlives the activity: the process most likely to be killed without
    * warning is one whose UI has been gone for hours, and that is exactly
    * when the activity's timer is not running. */
}

void shell_ui_dirty(void)
{
   flag_raise(&g_ui_dirty);
}

void shell_orient_apply(void)
{
   struct prefs sp;
   settings_get(&sp);
   jb_set_orientation(g_act, sp.orient);
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

/* REMOTE push: hand one just-stored datapoint (timestamp + glucose) to
 * Ble.remotePush, which only ENQUEUES onto its own background thread and
 * returns -- so this is safe on the reading paths, which run on BLE binder
 * threads with driver_lock held (same rule that keeps the alarm off these
 * threads). dexble_env(), not g_act->env: a JNIEnv is only valid on its own
 * thread, and these calls arrive on binder threads and the service tick. */

/* The app's own log is the outbox now.
 *
 * A local outbox is a byte offset into readings.csv that the server has
 * confirmed, a second one for the live tail, an in-flight marker for each,
 * and the same again for doses -- all of it bookkeeping about what the
 * server already has. The replica protocol asks the server that question
 * directly and gets an exact answer, so the only thing worth knowing
 * locally is whether the log has GROWN since the last look, which is one
 * number. */
/* (ob_log_size is gone: syncjni_state_stamp covers every synced file, not
 * just the readings log, and that is what decides whether to sync.) */

/* (The PULL direction is gone. It existed because the server held far more
 * history than the phone and the phone wanted it back; under the replica
 * protocol the server holds exactly what the phone gave it, so there is
 * nothing there to import that is not already here.) */

/* (Nothing pushes a single datapoint. Everything the phone holds reaches the
 * server as whole buckets whose hashes are compared first, so there is no
 * per-point path that could drop one.) */

/* current reading from the 4e stream */

/* Apply the SCREEN setting. FLAG_KEEP_SCREEN_ON is what overrides the
 * display timeout while the app is open; clearing it hands the screen back
 * to the OS. Only the window flag changes -- the BLE wakelock in
 * PancraService is separate, so readings keep arriving either way. */
static void apply_screen_on(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (!g_act)
      return;
   if (sp.screen_on)
      ANativeActivity_setWindowFlags(g_act, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
   else
      ANativeActivity_setWindowFlags(g_act, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
}

void shell_apply_screen_on(void)
{
   apply_screen_on();
}

struct ANativeActivity *shell_activity(void)
{
   return g_act;
}

/* Point the driver at the sensor whose screen is open. Calibration reads and
 * writes go to the selected link, and the 1 Hz reconcile moves that
 * selection constantly -- without this the user could calibrate a different
 * sensor from the one named on screen, which for the most consequential
 * write in the app is not an acceptable failure. Returns 0 if the selection
 * is not usable. */

/* The menu_action code this screen's own title-row X emits -- the system
 * back gesture is a second finger on the SAME target, so navigation stays
 * single-sourced in menu_action and every recorded g_*_from origin keeps
 * working. Returns -1 when there is nothing to close (the main screen: back
 * deliberately does nothing there). KEEP IN STEP with the add_hit(...,
 * ACT_MENU, code) each render_* records on its title row. */

/* ---- act-on-release helpers (state at g_arm_*; policy comment there) ----
 */

/* (The +- stepper's alarm_adjust is gone: thresholds are now typed on the
 * keypad -- see the threshold branch in the MA_OK handler, which keeps
 * the same lock discipline: the pair is written under alarm_lock because the
 * evaluators read it under alarm_lock, and a mixed old/new pair can invert
 * the range for one tick.) */

/* --- input: drain the queue so the ANR watchdog stays fed --- */

/* reprogram the shared timer: first tick after `first_ms`, then every
 * `repeat_ms`. Switches between the 1 Hz repaint cadence and the hold-to-
 * repeat cadence -- which waits before repeating so a quick tap does not
 * repeat.
 */
static void timer_set(long first_ms, long repeat_ms)
{
   (void)sys_timer_arm_ms(g_timerfd, first_ms, repeat_ms);
}

/* THE NOTIFICATION lives in notify.c: it is a second, independent RENDERER
 * of the same data the screen shows, and the only one left once the activity
 * is destroyed. See its header. */

/* timer tick: repaint so AGE / stale state stay live */
static int on_timer(int fd, int events, void *data)
{
   where("on_timer");
   (void)events;
   (void)data;
   /* on_timer IS the render thread; reaffirm g_main_tid here so on_main()
    * can never be wrong about who may draw -- guarantees the 1 Hz repaint
    * always runs even if the onCreate capture were somehow off, so the UI
    * can't wedge.
    */
   atomic_store_explicit(&g_main_tid, gettid(), memory_order_relaxed);
   uint64_t ticks = 0;
   read(fd, &ticks, sizeof ticks); /* single read clears the expiration count */
   /* Covers both the stale-data alarm (which depends on elapsed time) and
    * any glucose alarm a reading raised on a binder thread: the zone is
    * recomputed here, so this single main-thread caller owns every
    * transition.
    */
   alarm_disc_reeval();
   /* One REMOTE step per tick: read the server's cursor, or send the next
    * chronological batch of points newer than it. Self-throttling (Java
    * reports busy while a request is in flight or a backoff is running),
    * so a backlog drains at the server's own pace rather than being lost. */
   pancra_remote_sync();
   calib_tick(); /* retry / expire the calibration queue and any pending
                  * rescale target */
   /* COMMIT A SETTLED EXERCISE LEVEL. Cheap and idempotent -- a no-op unless
    * a value has been sitting unchanged for its minute. Driven from here AND
    * from the service tick below, because the minute can expire with the
    * activity gone: the whole point of the delay is that the user presses the
    * button and puts the phone away. */
   if (exercise_button_tick(realtime_s(), mono_s(), tz_off_now()))
      shell_ui_dirty();
   /* Refresh the UTC offset periodically, rather than reading it once in
    * onCreate: the foreground service is designed to outlive the activity
    * for days, so across a DST transition every displayed timestamp, and
    * every subsequent meter import, would stay an hour off until a cold
    * start. */
   if (g_act && g_act->env && realtime_s() - g_tz_checked > 300) {
      g_tz_checked = realtime_s();
      tz_refresh(g_act->env);
   }
   sensor_reconcile(); /* keep the registry in step with the live session */
   stategen_tick();    /* the backup's generation stamp: see stategen.h */

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
    * scan_running())". This is what makes that assertion true rather than
    * aspirational, and it self-heals any path that forgets.
    *
    * THROTTLED to once every 30 s, not every tick. start_scan() sets
    * scan_running() only on success, so a persistent failure (Bluetooth off,
    * BLUETOOTH_SCAN revoked, the LE scanner unavailable) leaves the
    * condition true forever -- retrying at 1 Hz would re-enter JNI and
    * rewrite the status line every second, and repeated startScan calls are
    * exactly what trips Android's "app scanning too frequently" block (5
    * starts in 30 s), which would turn a recoverable failure into a sticky
    * one. */
   /* Retry a stop Java could not confirm, so scan_running() cannot latch. */
   if (scan_stop_pending() && g_act)
      stop_scan(g_act);
   {
      /* Decision in scanlogic.c so `make check` can fail on it. */
      static long last_scan_retry;
      /* MONOTONIC: every field this decision compares is an interval -- how
       * long since the last retry, how long the radio-quiet hold has left --
       * and a wall-clock correction moved both (see util.h). */
      struct scan_state ss = {.have_activity = g_act != 0,
                              .paused        = g_paused,
                              .scanning      = scan_running(),
                              .pairing       = pairing_smart(),
                              .meter_busy    = meter_busy(),
                              .now           = mono_s(),
                              .hold_until    = scan_hold_time(),
                              .last_attempt  = last_scan_retry};
      if (scan_should_start(&ss)) {
         last_scan_retry = ss.now;
         LOGI("scan was down with the UI up; restarting it");
         start_scan(g_act);
      }
   }
   pancra_link_watchdog();
   pairing_tick();
   /* Same atomic test-and-clear the service path uses: whichever consumer
    * wins renders it, and the other does not double-render. */
   notify_tick();
   /* Rebuild the text lines here (BLE-thread updates only marked
    * g_ui_dirty), then repaint unconditionally so AGE / stale state stay
    * live. Both run on the main thread, so the hit-box geometry can't race
    * on_input. */
   flag_clear(&g_ui_dirty);
   update_screen();
   if (live_window())
      draw(live_window());
   /* A TICK MUST NOT END HOLDING THE DRIVER LOCK.
    *
    * This invariant is here because breaking it is silent and total: the lock
    * is recursive, so a tick that acquires one more time than it releases
    * leaves the owner set to this thread forever. The UI keeps working -- the
    * holder is the thread that wants it -- while every GATT callback spins on
    * it, and the phone shows a connected sensor that never produces another
    * reading. It shipped exactly once, from a redundant driver_lock() left
    * above a driver_enter(), and cost three CGM cycles before anyone noticed.
    *
    * Two lines and a log at 1 Hz, against a failure whose only symptom is the
    * data quietly stopping. */
   if (driver_held())
      LOGW("BUG: on_timer ended holding the driver lock -- GATT callbacks "
           "will now spin and readings will stop");
   return 1;
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
   pairing_stop_smart();
   (void)a;
   if (g_looper && g_timerfd >= 0)
      ALooper_removeFd(g_looper, g_timerfd);
   if (g_timerfd >= 0)
      close(g_timerfd);
   g_timerfd = -1;
   atomic_store_explicit(&g_win, NULL, memory_order_release);
   /* BOUNDED, and on the main thread that is not a preference. See
    * on_window_destroyed for the whole argument; the short version is that the
    * only thing that can still be holding this lock is a binder thread part
    * way through appending a reading, and the previous unbounded, non-yielding
    * spin made closing the app wait on it with a pinned core. */
   (void)store_drain(TEARDOWN_DRAIN_MS);
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
    * on_pause's stop_scan leaves scan_running() SET when Java cannot confirm
    * the cancel (deliberately -- it stops the self-heal stacking a second scan
    * client). start_scan then early-returns on that same flag, so the resume
    * silently failed to bring the scan back and the app went on believing
    * one was running. Coming back to the foreground is the user's own
    * recovery gesture and the only path that ever produced a real restart;
    * it must not be the one that no-ops. In the healthy case the stop below
    * is itself a no-op (on_pause already cleared scan_running()), so this costs
    * nothing. */
   scan_restart(a);
   sys_refresh(); /* a permission/settings dialog may have returned */
   if (scan_take_battery_wanted()) { /* first boot: chain it once */
      struct menu_view mv;
      menu_view_get(&mv);
      if (!mv.batt_ok)
         jb_request_battery(g_act);
   }
   if (cur_screen() && live_window())
      draw(live_window()); /* so the menu reflects the new state at once */
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
   atomic_store_explicit(&g_win, win, memory_order_release);
   model_lines_reset();
   update_screen();
   draw(win); /* force the first frame; update_screen's throttle may skip it
               */
}

static void on_window_destroyed(struct ANativeActivity *a,
                                struct ANativeWindow *win)
{
   (void)a;
   (void)win;
   /* WHAT THIS PUBLISHES, AND WHAT IT DELIBERATELY DOES NOT WAIT FOR.
    *
    * Rendering happens on the main looper: draw() returns early for any thread
    * that is not the looper, and THIS callback is on the looper, so a
    * draw_impl in flight here would mean the main thread were in two places at
    * once. There is nothing to wait for, and a spin here would in fact be
    * waiting on the HISTORY lock -- held by binder threads appending readings
    * -- which is the main thread in a tight loop, burning a core for a
    * hist_insert it has no reason to care about, at the exact moment the
    * system wants the app to get out of the way. On a slow core that is an
    * ANR, and the user sees the app hang as they close it.
    *
    * What is load-bearing is publishing that this surface is gone, so the next
    * frame on this thread does not paint into it. The drain is a bounded
    * courtesy, not a correctness requirement -- it lets an in-flight history
    * append finish before teardown continues, and gives up
    * rather than holding the main thread hostage. */
   atomic_store_explicit(&g_win, NULL, memory_order_release);
   (void)store_drain(TEARDOWN_DRAIN_MS);
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

/* THE JAVA SIDE, once per process.
 *
 * Order matters twice here: the natives have to be registered on the class
 * before anything can call back into C, and the BLE driver has to be wired
 * before a GATT callback can arrive -- which it can, immediately, on a binder
 * thread. Returns 0 when a step failed, having already put the reason on the
 * status row. */
static int init_java(struct ANativeActivity *activity, JNIEnv *env)
{
   jclass ble = find_app_class(activity, "com.jk.pancra.Ble");
   if (!ble) {
      LOGI("Ble class NOT found");
      set_status("NO BLE CLASS!");
      return 0;
   }
   if (!jb_bind(env, activity->clazz, ble)) {
      LOGI("Ble bind failed");
      set_status("JNI BIND FAILED!");
      return 0;
   }

   /* The advert callback is the PAIRING workflow's entry point, so it
    * registers itself (see pairing.h). */
   if (!pairing_register(env, jb_class())) {
      LOGI("RegisterNatives failed");
      set_status("JNI REG FAILED!");
      return 0;
   }
   /* (No remote* ids: the sync client's two entry points are registered
    * as NATIVES on the Ble class by dexble_register, and its transport is
    * looked up in syncjni_wire.) */

   /* wire up the BLE protocol driver (registers its own Ble callbacks) */
   dexble_init(activity->internalDataPath ? activity->internalDataPath
                                          : "/data/local/tmp");
   /* FATAL, like every other step here.
    *
    * This one logged and carried on, and init_java returned 1 -- so the app
    * published itself as initialised with no BLE transport behind it. The
    * consequence is not a visible crash: the screen comes up, the menus work,
    * and NOTHING EVER ARRIVES. No sensor connects, no reading is logged, no
    * alarm can fire, and the sync has no transport to push through. A glucose
    * monitor that looks healthy and monitors nothing is the worst state this
    * app can be in, and it is the state a silent failure here produced.
    *
    * dexble_register installs the GATT callbacks as natives on the Ble class
    * and takes the Context global ref everything later uses; there is no
    * degraded mode to fall back to. */
   if (!dexble_register(env, jb_class(), activity->clazz)) {
      LOGI("dexble_register failed");
      set_status("BLE REG FAILED!");
      return 0;
   }

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
   return 1;
}

/* RECOVER THE BONDED SENSOR'S ADDRESS from the OS bond list, when we hold a
 * key but have no saved MAC -- after an app update that added MAC
 * persistence, or if the saved one was lost. Without it the app falls back to
 * guessing from adverts, whose local name is usually absent.
 *
 * IT MUST RUN AFTER THE REGISTRY IS READ. It asks for a device whose name
 * matches the PRIMARY sensor's family prefix, and when it ran first the slot
 * table was empty, the primary resolved to -1, and the prefix was always the
 * Stelo's: a G7-only user's bonded device was never found and could never
 * reconnect, and a user with both got LINK_CGM locked onto the Stelo's
 * address while the Stelo already owned its own link -- two links reporting
 * one MAC, collapsing the address-based resolution everything else depends
 * on. */
static void recover_bonded_mac(struct ANativeActivity *activity, JNIEnv *env)
{
   /* Bonded-MAC recovery runs HERE, after sensors_load().
    *
    * It asks the OS bond list for a device whose name matches the
    * primary sensor's family prefix -- so running it before the registry
    * is read leaves slot_count() at 0, the primary resolving to -1, and the
    * prefix always "DX01". A G7-only user's bonded device would then never
    * be found and could never reconnect; a user with both would get
    * LINK_CGM locked onto the Stelo's address while the Stelo already owned
    * its own link, leaving two links reporting the same MAC and collapsing
    * the address-based
    * routing. */
   {
      struct dex_session s;
      /* The CGM link BY NAME, not whichever link a callback last selected:
       * getting this wrong would let a second sensor's session suppress the
       * primary's MAC recovery entirely. driver_session_of takes the lock
       * itself, so wrapping this call in a hand-taken one is redundant --
       * and a redundant lock is exactly what can hold the driver's own for
       * the life of the process, which is why it is private to
       * dexlink.c. */
      if (driver_session_of(LINK_CGM, &s) && !s.mac[0] && s.paired) {
         /* The name prefix comes from the REGISTRY, i.e. from the
          * family the user actually paired, so this can never latch
          * onto a bonded sensor they did not choose. */
         const char *want = "DX01";
         /* The primary's TYPE, asked for BY ID. sensor_primary_slot gives a
          * position, and a mint moves it; sensor_primary_id names the
          * device. */
         struct sensor_rec pr;
         if (sensor_rec_of(sensor_primary_id(), &pr) && pr.type == SENSOR_G7)
            want = "DXCM";
         char bm[24] = {0};
         if (jb_bonded_sensor(env, activity->clazz, want, bm, sizeof bm)) {
            char dt[DEVTAG_LEN];
            LOGI("locked to bonded sensor dev %s (%s)", devtag(bm, dt), want);
            /* dexble_register() has already run, so GATT callbacks can be
             * firing. ONE operation, because the file and the reconnect
             * target must change together: a callback between them would see
             * the new address with the previous target still set.
             *
             * AND THE FILE GOES FIRST. This walk is the only thing that finds
             * a bonded sensor whose files/<link>.mac went missing, and it runs
             * once per launch -- so an address published to the live target
             * but not written is a sensor that works this session and is
             * gone the next, with nothing to notice. The refusal is said
             * out loud here and retried by the link watchdog; the target
             * keeps whatever it had until the write actually lands. */
            if (driver_bind_mac(LINK_CGM, bm) != BIND_PUBLISHED)
               LOGW("recovered sensor address NOT SAVED -- retrying; "
                    "the previous reconnect target still stands");
         }
      }
   }
   /* Seeded per meter at CONNECT time (see the advert handler); this
    * is just a safe initial state before any meter is selected. */
   ot_init(-1);
}

/* EVERYTHING THE DATA DIRECTORY HOLDS, and the order it must be read in.
 *
 * The load order is not free, and each constraint below cost a bug: the
 * registry resolves the readings, so it is read first; the bonded-MAC
 * recovery asks the registry which sensor FAMILY to look for, so it runs
 * after that; and the history is loaded under its own lock because GATT
 * callbacks are already live by this point. */
static void init_data(struct ANativeActivity *activity, JNIEnv *env)
{
   /* NO SNAPSHOT HERE. This function LOADS the settings and the credentials;
    * a copy taken at the top would hold the compile-time defaults, and the
    * two places below that use them are after the loads. Each takes its own,
    * where the value is used. */
   /* persistent reading log: remember our own datapoints across restarts.
    * Internal storage; the app is debuggable, so retrieve with
    *   adb shell run-as com.jk.pancra cat files/readings.csv >
    * readings.csv
    */
   {
      const char *dir = activity->internalDataPath ? activity->internalDataPath
                                                   : "/data/local/tmp";
      /* EVERY MODULE OWNS ITS OWN FILENAME. The shell hands over the
       * directory and nothing else: with all thirteen files named here,
       * renaming one means editing the activity's startup, and a module
       * cannot be moved without taking a line of main.c with it. */
      /* AND EVERY ONE OF THEM MUST FIT.
       *
       * data_path refuses a path it cannot represent rather than truncating
       * it, because a truncated path is not an error anyone notices -- it is
       * a well-formed path to somewhere ELSE, which reads as empty and looks
       * exactly like a first run. If any canonical path cannot be built,
       * persistence is not initialised: the app says so rather than running
       * on a data directory it only half addresses, appending readings to
       * one file and settings to another. */
      int pathsok = 1;
      if (!(store_paths(dir)))
         pathsok = 0;
      if (!(settings_paths(dir)))
         pathsok = 0;
      if (!(sensors_paths(dir)))
         pathsok = 0;
      if (!(insulin_paths(dir)))
         pathsok = 0;
      if (!(weight_paths(dir)))
         pathsok = 0;
      if (!(food_paths(dir))) /* food.csv AND foodtypes.csv */
         pathsok = 0;
      if (!(exercise_paths(dir)))
         pathsok = 0;
      meter_register_ops(); /* the driver routes callbacks to it: see meter.h */
      if (!(meter_paths(dir)))
         pathsok = 0;
      if (!(sess_paths(dir)))
         pathsok = 0;
      calib_register_ops(); /* the driver serialises the queue: see calib.h */
      if (!(calib_paths(dir))) /* cal.q and rescale.cfg */
         pathsok = 0;
      if (!(stategen_paths(dir))) /* state.gen: what a backup pulls first */
         pathsok = 0;
      if (!pathsok) {
         LOGW("startup: the data directory is too long to build every file "
              "path from (%s)",
              dir);
         set_status("DATA PATH TOO LONG");
      }

      /* WHAT THE STORAGE ACTUALLY GAVE BACK. Each loader answers absent /
       * ok / corrupt / unreadable, and the worst of them is what startup
       * reports: a phone that lost one file has lost data whichever file that
       * is. See app/loadresult.h. */
      enum load_result lr = LOAD_ABSENT;
      /* Load order is NOT free: the registry resolves the readings, so it
       * has to be read before them (see store_load below).
       *
       * AND THE METER'S ANSWER IS ONE OF THESE, rather than being discarded
       * by a void wrapper: a meter.sync that cannot be read must not look
       * like a first run -- LAST SEEN blank, the re-arm cooldown gone, and
       * nothing to say why. */
      lr = load_worse(lr, meter_state_load()); /* per-meter sync state */
      lr = load_worse(lr, sess_load());        /* the session clock */
      lr = load_worse(lr, remote_load());      /* remote-push server config */
      /* EVERY LOADER IS ASKED, AND EVERY ANSWER IS KEPT. Each returns 0 for
       * "read whole" -- including a first run with no file -- and -1 for a
       * read that stopped partway, having kept what it managed to parse. One
       * warning covers all of them below: three separate status lines would
       * overwrite each other, and the user needs one fact ("what you are
       * looking at is short"), not a list of filenames. */
      int lost = 0;
      if (insulin_load() < 0) /* doses: the form pre-populates from these */
         lost = 1;
      if (weight_load() < 0)
         lost = 1;
      /* Food is TWO files loaded as one -- the vocabulary and the entries --
       * because an entry can only be checked against a vocabulary that has
       * already been read. food_load does that ordering internally and
       * answers once for the pair. */
      if (food_load() < 0)
         lost = 1;
      if (exercise_load() < 0)
         lost = 1;
      if (sensors_load() < 0) /* before store_load: readings resolve here */
         lost = 1;
      recover_bonded_mac(activity, env);
      /* UNDER hist_lock: store_load rewrites g_hist wholesale, and this
       * runs AFTER dexble_register, whose own comment notes GATT
       * callbacks can already be firing. Today the first reading cannot
       * realistically land inside the load window (scan, timer and
       * service tick all start later), so the exposure is latent rather
       * than live -- but store.h states the contract, and relying on startup
       * ordering to satisfy it is exactly the kind of reasoning that goes
       * stale when the ordering changes. */
      /* The primary BEFORE the history lock: registry -> history. */
      int prime = sensor_primary_id();
      hist_lock();
      int lrc = store_load(prime);
      hist_unlock();
      /* A log that exists but could not be read whole leaves the history
       * SHORT, and everything downstream -- the plot, the 1D/3D/7D
       * statistics, the restored current reading -- then understates the
       * record while looking perfectly normal. Say so on the status line
       * rather than let a partial record pass for a complete one. */
      if (lrc < 0 || lost) {
         LOGW("startup: a log could not be read whole (readings=%d, "
              "other=%d); what is shown is INCOMPLETE",
              lrc < 0, lost);
         set_status("HISTORY INCOMPLETE");
      }
      /* MIGRATION: any source that has readings on the plot but no slot
       * (a device forgotten before slot-retention existed) becomes an OLD
       * device -- a retired slot -- so it gets the full per-device menu
       * and consistent styling like everything else. Bounded by the
       * readings window and MAX_SLOTS. */
      {
         /* ONE SNAPSHOT. Startup, single-threaded, and still
          * worth asking properly: the walk below is over a table the ingest
          * path appends to, and "we are early enough that nobody else is
          * running yet" is an argument that stops being true silently. */
         static struct reading snap[NHIST];
         int nsnap = hist_copy(snap, NHIST);
         int orphans[NHIST];
         int no = 0;
         for (int i = 0; i < nsnap; i++) {
            int src = snap[i].src;
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
         for (int j = 0; j < no; j++) {
            int id = orphans[j];
            /* Is it already one of the user's devices, and if not, what was
             * it? Two questions the registry answers for itself. A slot that
             * appears between them is a startup-only race with the binder
             * thread, and the claim below is not a no-op for one that is OLD
             * -- it REVIVES it -- so the undo has to be careful: see there. */
            int known = sensor_id_is_live(id);
            struct sensor_rec rec;
            int have = !known && sensor_rec_of(id, &rec);
            int type = have ? rec.type : SENSOR_STELO;
            char ident[24];
            str_snapshot(ident, sizeof ident, have ? rec.identity : "");
            if (known || !have)
               continue;
            /* RETIRED, through the registry's own operation: these devices are
             * being restored from the provenance file as OLD ones, and
             * sensor_retire is what "old" means (it also drops the primary
             * and persists). Setting the two fields by hand was the same
             * change with none of the rules. BY ID -- the index
             * sensor_claim_slot returns is only true until the next mint. */
            /* BOTH HALVES OR NEITHER. The claim and the retirement are one
             * restoration: a slot that is claimed and then fails to be
             * marked old is LIVE -- the alarm watches it, the big number can
             * bind to it, and the DEVICES list offers it as a working
             * sensor -- when what it actually is is history recovered from a
             * provenance file. If the second half will not persist, the
             * first is undone. */
            if (sensor_claim_slot(id, type, ident) >= 0 &&
                sensor_retire(id) != SENSOR_OK) {
               LOGW("restored device %d could not be marked old", id);
               /* THE CLAIM PERSISTED AND THE RETIREMENT DID NOT, so the slot
                * is LIVE -- the alarm watches it and the big number can bind
                * to it -- when what it is is history recovered from a
                * provenance file. Undo the claim.
                *
                * If THAT will not persist either, the file already says
                * LIVE, and it will say so on the next launch too: this loop
                * only restores devices the registry does not have, so it
                * will not try again. Say exactly that, rather than implying
                * it heals itself. The user can disconnect it by hand, which
                * is the same operation with a working filesystem. */
               if (sensor_forget(id) != SENSOR_OK)
                  LOGW("...and could not be un-claimed: device %d stays LIVE "
                       "until it is disconnected by hand",
                       id);
            }
         }
      }
      /* THE STATISTICS ARE PART OF THE LOAD VERDICT. A log that exists and
       * cannot be read is not a first run: the figures beside the plot would
       * otherwise be computed from whatever prefix came back, and look
       * exactly like a phone with less history rather than one whose record
       * is unreadable. stat_load keeps what it read and refuses to publish a
       * window; this is what makes the app SAY so. */
      lr = load_worse(lr, stat_load(store_path()));
      lr = load_worse(lr, info_load());
      lr = load_worse(lr, alarm_load());
      /* The paired identity survives a restart; without this the first sync
       * after every launch would be unsigned and refused.
       *
       * READ AFTER remote_load(), which is what parses the uid and key out
       * of the file and publishes them: a copy taken before it holds uid 0,
       * and sync_set_key(0, ...) is exactly the unsigned state this line
       * exists to prevent. */
      struct sync_creds sc;
      remote_creds_get(&sc);
      sync_set_key(sc.uid, sc.key);
      lr = load_worse(lr, settings_load());
      /* A calibration or a live rescale factor that EXISTS on disk and could
       * not be read is not a fresh install: the app would otherwise start
       * scaling every reading from that sensor by 1.000 again, or forget a
       * calibration the user confirmed, and look exactly as it does on a
       * phone that has never had either.
       *
       * SAID HERE, not folded into the `lost` flag above: that flag has
       * already been reported by this point, and this is a different fact --
       * the history is whole, the correction applied to it is not. */
      if (calib_load() != CALIB_OK) {
         LOGW("startup: the calibration or rescale state could not be read");
         set_status("CALIBRATION STATE LOST");
      }
      lr = load_worse(lr, code_load());
      /* DEGRADED STORAGE IS NOT A FIRST RUN, and this is where the two stop
       * looking alike.
       *
       * A loader returning void leaves a settings file that could not be
       * read, or one truncated by a power loss mid-write, running the app on
       * compiled defaults -- indistinguishable from a fresh install, and
       * silently overwriting the user's choices with defaults at the next
       * save. LOAD_ABSENT really is a first run and says nothing; the other
       * two are reported. */
      /* SAVED DATA, not "settings": this verdict is the worst of every load
       * above -- the session clock, the remote config, the READINGS LOG
       * behind the statistics, the alarm thresholds, the settings and the
       * pairing code. Naming one of them sends the user to the wrong screen
       * to look for the damage. */
      if (lr == LOAD_ERROR) {
         LOGW("startup: saved data could NOT BE READ (%s)",
              load_result_name(lr));
         set_status("SAVED DATA NOT READ");
      } else if (lr == LOAD_CORRUPT) {
         LOGW("startup: saved data was INCOMPLETE (%s)", load_result_name(lr));
         set_status("SAVED DATA INCOMPLETE");
      }
      /* ...and the orientation AFTER settings_load(), for the same reason:
       * a copy from before it is the default, so the phone would ignore the
       * user's choice on every cold start. */
      struct prefs sp;
      settings_get(&sp);
      jb_set_orientation(g_act, sp.orient); /* restore the last choice */
      LOGI("reading log: %s (%d in memory, %d stored)", store_path(),
           hist_in_memory(), store_appended());
      /* store_load restored g_cur_glu/g_cur_time -- the big number shows
       * it immediately, so the ongoing notification must too. It is
       * dirty-driven off new readings, which have not arrived yet at cold
       * start, so mark it dirty here to seed the notification text from
       * the restored reading rather than leaving it "no recent reading"
       * until the first live sample. */
      if (store_now(realtime_s()).glu >= 0)
         notify_mark();
   }
}

/* NDK entry point: resolved by name by the Android runtime when the .so
 * loads. Explicitly exported (we build -fvisibility=hidden); it is external
 * by necessity, which is also why it cannot be given internal linkage. */
__attribute__((visibility("default"))) void
ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved,
                         size_t saved_size)
{
   JNIEnv *env = activity->env;

   /* This runs on the main/UI looper thread; record it so
    * draw()/update_screen() can tell it apart from BLE binder threads (see
    * on_main). The looper callbacks (on_timer/on_input) are registered on
    * this same thread below. */
   atomic_store_explicit(&g_main_tid, gettid(), memory_order_relaxed);
   g_act = activity;
   /* Stale-alarm grace starts at PROCESS start, not at every onCreate.
    *
    * Re-arming it on each activity launch means opening the app to see why
    * the alarm is sounding silences it: the next heartbeat recomputes
    * grace = 1, drops g_disc_alarmed, and alarm_apply issues a silence --
    * then refuses to re-raise for the whole threshold (up to 60 min) with
    * the sensor still dead. The service keeps running across activity
    * destruction, so the grace period must not restart with the UI. */
   if (!g_launch_mono)
      g_launch_mono = mono_s();
   /* Pointers, so the handler reads what is true at the MOMENT OF THE CRASH
    * rather than a snapshot taken now. Filled at runtime because the members
    * are addresses of objects in other translation units, and cur_screen() is
    * an enum whose address needs the cast to be read as the int it is. */
   static struct crash_ctx cctx;
   /* &g_where, not g_where: the checkpoint is a pointer VARIABLE that moves,
    * so passing its value here froze every crash report at "boot" -- this
    * line was the one exception to the rule the comment above states. */
   cctx.where  = &g_where;
   cctx.status = model_status_buf();
   cctx.glu    = store_glu_ptr();
   cctx.menu   = &g_screen_now;
   cctx.nhist  = hist_count_ptr();
   /* ---- AND WHETHER THE HANDLER IS ACTUALLY THERE ----------
    *
    * This call was void and every signal() result inside it was dropped, so
    * this line "installed crash reporting" whether or not any of it took.
    * The failure that produces is invisible by construction: the app dies,
    * there is no crash.log, and nothing anywhere says the handler was never
    * installed -- which is the one fact that would explain the missing file.
    *
    * PARTIAL COVERAGE IS KEPT AND NAMED. A handler that IS in place still
    * writes a report for its own signal; what the log says is which signals
    * are uncovered, by number, so a crash with no report can be matched
    * against them. */
   struct crash_install_result ci =
       crash_install(activity->internalDataPath ? activity->internalDataPath
                                                : "/data/local/tmp",
                     &cctx);
   if (ci.failed) {
      for (unsigned b = 0; b < 32; b++)
         if (ci.failed & (1U << b))
            LOGW("startup: no crash handler for signal %d -- a crash on it "
                 "will leave no crash.log",
                 crash_sig_of(b));
      if (!ci.installed)
         set_status("NO CRASH REPORTING");
   }

   /* local timezone offset (seconds), for on-screen timestamps */
   tz_refresh(env);

   activity->callbacks->onResume                   = on_resume;
   activity->callbacks->onPause                    = on_pause;
   activity->callbacks->onNativeWindowCreated      = on_window_created;
   activity->callbacks->onNativeWindowDestroyed    = on_window_destroyed;
   activity->callbacks->onNativeWindowRedrawNeeded = on_redraw_needed;
   activity->callbacks->onNativeWindowResized      = on_window_resized;
   activity->callbacks->onInputQueueCreated        = on_queue_created;
   activity->callbacks->onInputQueueDestroyed      = on_queue_destroyed;
   activity->callbacks->onDestroy                  = on_destroy;
   /* NO onSaveInstanceState. THE APP ALWAYS OPENS ON THE MAIN SCREEN.
    *
    * Coming back to a glucose reading is the whole point of opening this app,
    * and being put back three screens deep in a log or a settings submenu --
    * where the user was when Android happened to kill the process, possibly
    * hours earlier -- puts a menu between them and the number. There is
    * nothing here worth carrying across a process death that is worth that.
    */

   /* Process-wide, one-time setup: JNI globals, the BLE driver, and the
    * loaded history/settings. The foreground service can outlive the
    * activity, so a relaunch re-enters onCreate in the same process -- guard
    * this so we don't leak the g_ble global ref, re-register natives, or
    * reload the history. */
   int cold = !g_inited;
   if (!g_inited) {
      if (!init_java(activity, env))
         return; /* the status row says which step failed */
      init_data(activity, env);
      g_inited = 1;
   }

   /* The framework's saved bundle is IGNORED, and none is ever produced --
    * see the onSaveInstanceState note above. `cold` still gates the one-time
    * setup a few lines up; nothing else is restored. */
   (void)cold;
   (void)saved;
   (void)saved_size;

   /* AND THE APP OPENS ON THE MAIN SCREEN, on EVERY launch -- not only a cold
    * one.
    *
    * Dropping the saved bundle is not enough on its own. The foreground
    * service outlives the activity, so closing the app from the task switcher
    * destroys the activity while the PROCESS survives with nav still holding
    * whatever menu was open; reopening from the notification then re-enters
    * here and lands back on it. Nothing was restored -- it was simply never
    * left.
    *
    * SAFE ON EVERY onCreate because the manifest declares configChanges for
    * orientation and screen size, so a rotation does not come through here:
    * this runs on a genuine (re)launch and nowhere else. */
   nav_home();


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
      int tfd = sys_timer_open();
      if (tfd >= 0) {
         g_timerfd = tfd;
         (void)sys_timer_arm_ms(tfd, 1000, 1000); /* 1 Hz repaint */
         ALooper_addFd(g_looper, tfd, 3, ALOOPER_EVENT_INPUT, on_timer, 0);
      }
   }

   /* Don't fire the system permission dialog on cold start. Show the
    * rationale screen first; CONTINUE (in on_input) issues the actual
    * request. */
   if (!has_ble_permissions(activity)) {
      shell_gate_arm();
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
      jb_set_orientation(g_act, 0);
   }
}
