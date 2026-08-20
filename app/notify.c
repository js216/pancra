// SPDX-License-Identifier: GPL-3.0
// notify.c --- The ongoing notification (see notify.h)
// Copyright 2026 Jakob Kastelic

#include "notify.h"
#include "alarmlogic.h" /* AL_FRESH_S: one definition of "stale" */
#include "blejni.h"
#include "clock.h"
#include "jbridge.h"
#include "plot.h"
#include "sensors.h"
#include "settings.h"
#include "store.h"
#include "style.h"
#include "thread.h"
#include "tzoff.h"
#include "uidraw.h"
#include "uifmt.h"
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

/* Lock-screen plot bitmap, in pixels. Taller than it was, now that the
 * reading and its time share one title line above it. */
#define NOTIF_W 512
#define NOTIF_H 232

#define UNIT_LBL (sp.units ? "MMOL/L" : "MG/DL")

/* Set on a BLE binder thread, consumed by BOTH the activity's 1 Hz timer and
 * the service tick thread -- so the test-and-clear must be atomic, or a
 * reading can be marked dirty and cleared by the other consumer without ever
 * being rendered. flag_take is that exchange. */
static struct flag g_notify_dirty = FLAG_INIT;

void notify_mark(void)
{
   flag_raise(&g_notify_dirty);
}

/* (The notification plot's dot-colour callback is uidraw.h's white_color:
 * the trace is white on black here as it is on screen, and two identical
 * private copies of one function is how the two drift apart.) */

/* Push the live value + a 3H plot into the ongoing notification (lock screen
 * / shade). Main thread only (uses g_act->env). The plot is grayscale (white
 * dots on dark), so the ARGB bitmap needs no colour swizzle. Called from
 * on_timer when a new reading has arrived, so we rebuild the bitmap at most
 * once a cycle, not every second. */
static uint32_t g_notify_px[NOTIF_W * NOTIF_H];

/* Serialises notify_update against itself.
 *
 * It is now driven from BOTH the activity's 1 Hz timer and the service's
 * tick thread, and it fills two file-static buffers (g_notify_px and the
 * plot-point array). Two threads inside it at once would interleave those.
 * Try-lock, not spin: this is an idempotent refresh, so skipping one is free
 * -- the same reasoning draw() uses for its frame guard. */
static struct flight g_notify_flight = FLIGHT_INIT;

/* 1 when the notification was actually re-rendered and re-posted, 0 when
 * this call did nothing. Both refusals below are ORDINARY -- the transport's
 * env comes and goes with the process's Java side, and the other consumer of
 * the mark can be inside here already -- so the caller has to be able to tell
 * "refreshed" from "not this time" and put the request back. */
static int notify_update(void)
{
   struct prefs sp;
   settings_get(&sp);
   /* Context and env from the transport, NOT from g_act. g_act is NULL once
    * the activity is destroyed, and this used to return early on that -- so
    * after a back-press or task-swipe the notification froze at whatever
    * value was current at that instant, while readings kept arriving. In
    * that state the notification is the ONLY glucose display the user has.
    */
   JNIEnv *e    = dexble_env();
   jobject jctx = dexble_ctx();
   if (!e || !jctx || !jb_class())
      return 0;
   if (!flight_enter(&g_notify_flight))
      return 0;
   /* 64, not 48: the title concatenates the value, the unit label, the trend
    * and HH:MM, and the compiler's worst case for those four fills 48
    * exactly. A truncated title is a truncated GLUCOSE READING. */
   char title[64];
   char text[48];
   /* ONE consistent (glucose, trend, time) triple, under the lock the writer
    * uses. hist_refresh_current stores the three separately, so reading them
    * unlocked can pair a NEW glucose with the PREVIOUS timestamp -- which
    * evaluates as stale and renders "no recent reading" at the moment a hypo
    * lands -- or the mirror, a stale in-range value stamped with a fresh
    * time. Once the activity is destroyed this notification is the only
    * glucose display the user has. The alarm path already had
    * current_reading() for exactly this; the notification path did not. */
   /* store_now_LOCKED, because the lock is already held here. store_now
    * takes it, and the store lock is NOT recursive -- taking it twice on one
    * thread is a spin that never ends. It froze the app on the phone: the
    * main thread and the service tick both sat in store_now for ever, and
    * Android killed it with "Pancra isn't responding". */
   store_lock();
   struct reading_now now_r = store_now_locked(realtime_s());
   int cur_glu              = now_r.glu;
   int cur_trend            = now_r.trend;
   long cur_time            = now_r.t;
   store_unlock();
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
      fmt_glu(cur_glu, sp.units, gv, sizeof gv);
      fmt_trend(cur_trend, tr, sizeof tr);
      fmt_hms(cur_time, g_tz_off, hm, sizeof hm);
      hm[5] = 0; /* HH:MM */
      (void)snprintf(title, sizeof title, "%s %s  %s   at %s", gv, UNIT_LBL, tr,
                     hm);
      (void)snprintf(val, sizeof val, "%s", gv);
   }
   if (!sp.statbar_val)
      val[0] = 0; /* STATUS BAR: ICON mode -- Java falls back to the glyph */
   for (int i = 0; i < NOTIF_W * NOTIF_H; i++)
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
   /* Every slot -- LIVE and OLD (disconnected) alike -- carries its own
    * marker/colour, so one pass over the slots styles the whole plot the
    * same way the main screen does. */
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n && nsty < (int)(sizeof sty / sizeof sty[0]); i++)
      sty[nsty++] = (struct notif_sty){v.slot[i].id, v.slot[i].marker,
                                       v.slot[i].color, v.slot[i].size};

   store_lock();
   /* The notification plot mirrors the MAIN screen's plot EXACTLY: every
    * datapoint from every source -- CGM traces AND meter (BGM) fingersticks
    * -- each styled with that device's own marker and colour (a HIDE-marked
    * device is dropped, just as on the main plot), so the two read the same.
    */
   int np = 0;
   for (int i = 0; i < hist_count(); i++) {
      int src      = hist_at(i).src;
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
      if (!found && (src != 0 || hist_at(i).kind == KIND_BGM)) {
         if (src != 0) {
            mk  = MARK_CROSS;
            col = 0xFF8A8AA0; /* UI_ORPHAN */
         }
      }
      if (hide)
         continue;
      pts[np].t      = hist_at(i).t;
      pts[np].glu    = hist_at(i).glu;
      pts[np].marker = mk;
      pts[np].col    = col;
      pts[np].size   = sz;
      np++;
   }
   store_unlock();
   plot_render((struct plot_fb){g_notify_px, NOTIF_W, NOTIF_W, NOTIF_H},
               (struct plot_rect){0, 0, NOTIF_W, NOTIF_H}, pts, np,
               realtime_s(), 3, (struct plot_cfg){sp.plot_max, 3}, white_color,
               -1, 0, g_tz_off);
   /* plot_render writes the SCREEN's pixel convention -- raw u32 on a
    * little-endian RGBA surface, i.e. 0xAABBGGRR -- but
    * Bitmap.createBitmap(..., ARGB_8888) reads each int as 0xAARRGGBB.
    * Swap R and B or every coloured marker is wrong in the notification:
    * white dots and gray gridlines hide the swap (R == B), a PINK meter
    * marker turned periwinkle. Same failure ui_sensor_colors documents. */
   for (int i = 0; i < NOTIF_W * NOTIF_H; i++) {
      uint32_t c = g_notify_px[i];
      g_notify_px[i] =
          (c & 0xFF00FF00U) | ((c & 0xFFU) << 16U) | ((c >> 16U) & 0xFFU);
   }
   jb_show_glucose(e, jctx, title, text, val, g_notify_px, NOTIF_W, NOTIF_H,
                   sp.lockscr_val ? 1 : 0);
   flight_leave(&g_notify_flight);
   return 1;
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
 * last_stale is deliberately RELAXED rather than ordered: the two callers run
 * on different threads, and the worst a race can do is raise the dirty flag
 * twice or one tick late, both self-correcting. It is atomic all the same,
 * because "the worst case is benign" is a claim about the VALUES, and a plain
 * int shared between two threads is a data race whatever the values are --
 * which licenses the compiler to keep it in a register across the compare and
 * make the benign case stop happening. */
static void notify_stale_check(void)
{
   static atomic_int last_stale = -1;
   store_lock(); /* released before notify_update, which takes it again */
   struct reading_now now_r =
       store_now_locked(realtime_s()); /* HELD: see above */
   int cur_glu   = now_r.glu;
   long cur_time = now_r.t;
   store_unlock();
   int stale = (cur_glu < 0) || (realtime_s() - cur_time > AL_FRESH_S);
   if (stale != atomic_load_explicit(&last_stale, memory_order_relaxed)) {
      atomic_store_explicit(&last_stale, stale, memory_order_relaxed);
      flag_raise(&g_notify_dirty);
   }
}

/* The two consumers' single entry point: the activity's 1 Hz timer and the
 * service heartbeat.
 *
 * DIRTY-DRIVEN, exactly like the on_timer path. Refreshing unconditionally
 * on every 20 s tick would re-render the plot bitmap and re-post the
 * notification when nothing had changed -- pure battery and CPU cost on a
 * 24/7 app, for a display that only moves when a reading lands.
 * The mark is set by every writer of the displayed data (see notify_mark),
 * so this fires within one tick of each new reading, and by
 * notify_stale_check when it goes stale. */
/* THE MARK IS A REQUEST, AND A REQUEST THAT WAS NOT SERVED IS STILL PENDING.
 *
 * flag_take consumes the only record that anything wants rendering, and both
 * of notify_update's early returns are reachable at exactly the moments this
 * display matters: the transport's env is NULL while the Java side is being
 * rebuilt (a task-swipe, a service restart), and the flight is busy whenever
 * the other consumer of the same mark got there first. The request was then
 * gone. Nothing retries on a timer -- the whole path is dirty-driven, on
 * purpose, for the battery -- so the lock screen and the status bar kept
 * showing the PREVIOUS glucose until some unrelated event happened to raise
 * the flag again, which on a quiet sensor is five minutes and on a
 * disconnected one is never. That is the same lie notify_stale_check exists to
 * stop, arriving by a different route.
 *
 * TAKE BEFORE RENDERING, RE-RAISE AFTER FAILING -- never clear after a
 * successful one. A writer that marks WHILE a render is in flight has produced
 * data that render did not see, and the flag must survive to describe it;
 * taking first means such a mark is still standing when this returns.
 * Re-raising after a failure is safe in the same direction: flag_raise is a
 * store of 1, so a concurrent writer's mark cannot be lost by it, and the
 * worst it can cost is one redundant render of an idempotent refresh. */
void notify_tick(void)
{
   notify_stale_check();
   if (!flag_take(&g_notify_dirty))
      return;
   if (!notify_update())
      flag_raise(&g_notify_dirty);
}

/* The same tick under its pancra_* name: what the BLE service thread calls
 * (see dexble.c). Kept distinct from notify_tick only because that is the
 * app's external surface and this file is not. */
void pancra_notify_refresh(void)
{
   notify_tick();
}
