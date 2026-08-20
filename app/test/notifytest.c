// SPDX-License-Identifier: GPL-3.0
// notifytest.c --- Host tests for the ongoing notification's refresh request
// Copyright 2026 Jakob Kastelic

/* WHAT A LOST MARK LOOKS LIKE ON THE PHONE: an old glucose, on the only
 * display left.
 *
 * notify.c is dirty-driven on purpose -- re-rendering a 512x232 bitmap and
 * re-posting the notification every tick is battery spent on a display that
 * only moves when a reading lands. The cost of that design is that the mark is
 * the ONLY record that a refresh is wanted, so consuming one without rendering
 * loses the refresh entirely: nothing retries, and the lock screen and status
 * bar keep the previous number until some unrelated event marks again.
 *
 * Both of the renderer's refusals happen exactly when it matters. The
 * transport's JNIEnv is NULL while the Java side is being rebuilt -- a
 * task-swipe or a service restart -- which is precisely the state in which the
 * notification is the app's only glucose display. And the render slot is busy
 * whenever the OTHER consumer of the same mark (the service tick, against the
 * activity's 1 Hz timer) is already inside it.
 *
 * So this suite drives both refusals separately, and asserts on the NEXT tick
 * rather than on the failing one: the question is never "did that call report
 * an error", it is "is the refresh still going to happen". The busy case takes
 * a second thread, because "someone else is already rendering" is not a state
 * one thread can be in -- and that makes the suite a TSan candidate too.
 *
 * The whole file below dexble_env is stubs: notify.c is a renderer of other
 * modules' state, and linking those would drag in the JNI bridge, the plot and
 * the registry to test a five-line decision. Every stub is declared through
 * the real header, so a signature that moves is a compile error here.
 */
#include "notify.h"
#include "blejni.h"
#include "clock.h"
#include "jbridge.h"
#include "plot.h"
#include "sensors.h"
#include "settings.h"
#include "store.h"
#include "tzoff.h"
#include "uidraw.h"
#include "uifmt.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all  = 1;
static int nass = 0;

static void ck(int cond, const char *what)
{
   nass++;
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- the world notify.c renders from ---- */

/* A JNIEnv and a Context that are not real: notify.c only ever passes them
 * through to jb_show_glucose, which is stubbed below, so their only meaningful
 * property is whether they are NULL. That IS the property under test -- the
 * transport hands back NULL while the Java side is down. */
static int g_have_jni = 1;
static long g_now     = 1700000000L;
long g_tz_off         = 0;

JNIEnv *dexble_env(void)
{
   static JNIEnv *fake = (JNIEnv *)0x1;
   return g_have_jni ? fake : 0;
}

jobject dexble_ctx(void)
{
   static jobject fake = (jobject)0x2;
   return g_have_jni ? fake : 0;
}

jclass jb_class(void)
{
   static jclass fake = (jclass)0x3;
   return g_have_jni ? fake : 0;
}

long realtime_s(void)
{
   return g_now;
}

/* THE REAL LOCK'S SHAPE, not a no-op. notify_stale_check takes it on the
 * calling thread while the other thread is inside notify_update, which takes
 * it too -- that overlap is the fixture, so a stub that locks nothing would
 * hide a race rather than let TSan see it. */
static pthread_mutex_t g_store_lk = PTHREAD_MUTEX_INITIALIZER;

void store_lock(void)
{
   pthread_mutex_lock(&g_store_lk);
}

void store_unlock(void)
{
   pthread_mutex_unlock(&g_store_lk);
}

struct reading_now store_now_locked(long now)
{
   /* One fresh, in-range reading, constant for the whole run: every tick
    * calls notify_stale_check, and a CHANGING staleness would raise the dirty
    * flag by itself and make every assertion below unable to tell a preserved
    * mark from a fresh one. */
   struct reading_now r = {100, 0, now, 0};
   return r;
}

int hist_count(void)
{
   return 0;
}

struct reading hist_at(int i)
{
   struct reading r;
   (void)i;
   memset(&r, 0, sizeof r);
   return r;
}

void sensors_view_get(struct sensor_view *out)
{
   memset(out, 0, sizeof *out);
}

void settings_get(struct prefs *out)
{
   memset(out, 0, sizeof *out);
   out->plot_max = 400;
}

uint32_t ui_sensor_color(int color)
{
   (void)color;
   return 0xFFFFFFFFU;
}

uint32_t white_color(int g)
{
   (void)g;
   return 0xFFFFFFFFU;
}

void plot_render(struct plot_fb fb, struct plot_rect r,
                 const struct plot_pt *pts, int npts, long now, int hours,
                 struct plot_cfg cfg, uint32_t (*color)(int glu), int hi_idx,
                 uint32_t hi_color, long tz)
{
   (void)fb;
   (void)r;
   (void)pts;
   (void)npts;
   (void)now;
   (void)hours;
   (void)cfg;
   (void)color;
   (void)hi_idx;
   (void)hi_color;
   (void)tz;
}

void fmt_glu(int mgdl, int units, char *out, int n)
{
   (void)units;
   (void)snprintf(out, (unsigned long)n, "%d", mgdl);
}

void fmt_trend(int tr, char *out, int n)
{
   (void)tr;
   (void)snprintf(out, (unsigned long)n, "--");
}

void fmt_hms(long epoch, long tz, char *out, int n)
{
   (void)epoch;
   (void)tz;
   (void)snprintf(out, (unsigned long)n, "00:00:00");
}

/* ---- the render itself: counted, and stoppable ---- */

/* THE ONE OBSERVABLE. Every assertion here is about how many times the
 * notification was actually re-posted, because that is what the user sees --
 * "the tick returned an error" would pass just as well with the request
 * thrown away, which is the bug. */
static atomic_int g_renders;

/* The fixture for "someone else is already rendering": with g_hold set, the
 * rendering thread parks INSIDE notify_update -- holding the single-flight
 * slot -- until the main thread releases it. There is no other way to be in
 * that state, and it is the state the service tick and the 1 Hz timer are in
 * whenever they collide. */
static pthread_mutex_t g_hold_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hold_cv  = PTHREAD_COND_INITIALIZER;
static int g_hold;    /* park in the render */
static int g_holding; /* a thread is parked there now */
static int g_release; /* it may leave */

void jb_show_glucose(JNIEnv *env, jobject ctx, const char *title,
                     const char *text, const char *val, const uint32_t *px,
                     int w, int h, int lockscr)
{
   (void)env;
   (void)ctx;
   (void)title;
   (void)text;
   (void)val;
   (void)px;
   (void)w;
   (void)h;
   (void)lockscr;
   atomic_fetch_add(&g_renders, 1);
   pthread_mutex_lock(&g_hold_lk);
   if (g_hold) {
      g_holding = 1;
      pthread_cond_broadcast(&g_hold_cv);
      while (!g_release)
         pthread_cond_wait(&g_hold_cv, &g_hold_lk);
      g_holding = 0;
      pthread_cond_broadcast(&g_hold_cv);
   }
   pthread_mutex_unlock(&g_hold_lk);
}

static int renders(void)
{
   return atomic_load(&g_renders);
}

static void reset_renders(void)
{
   atomic_store(&g_renders, 0);
}

static void *hold_thread(void *arg)
{
   (void)arg;
   notify_mark();
   notify_tick(); /* renders, and parks inside the render */
   return 0;
}

int main(void)
{
   /* SETTLE THE STALENESS TRANSITION FIRST. notify_stale_check raises the
    * dirty flag when staleness CHANGES, and its first call always changes it
    * (the remembered value starts at -1). Without this the first assertion
    * below would be measuring that transition rather than its own mark. */
   notify_tick();
   ck(renders() >= 1, "the first tick renders: staleness became known");
   reset_renders();
   notify_tick();
   ck(renders() == 0, "a tick with nothing marked renders nothing");

   printf("== a served mark is consumed exactly once ==\n");
   {
      /* THE OTHER DEFECT WITH THE SAME TEST OUTCOME. A fix that simply never
       * clears the flag makes every failure case below pass, and turns every
       * tick into a full bitmap re-render and a re-post -- on the 1 Hz timer,
       * for ever. Nothing else in this file can tell the two apart. */
      reset_renders();
      notify_mark();
      notify_tick();
      ck(renders() == 1, "a marked tick renders once");
      notify_tick();
      notify_tick();
      ck(renders() == 1, "...and later ticks do not render again");
   }

   printf("== a mark survives a render with no JNI env ==\n");
   {
      /* The transport's env is NULL while the Java side is being rebuilt --
       * a task-swipe, a service restart. The refresh that was wanted at that
       * instant must still happen. */
      reset_renders();
      g_have_jni = 0;
      notify_mark();
      notify_tick();
      ck(renders() == 0, "with no JNI env nothing is rendered");
      g_have_jni = 1;
      notify_tick();
      ck(renders() == 1, "...and the NEXT tick renders the request");
      notify_tick();
      ck(renders() == 1, "...once, not on every tick after it");
   }

   printf("== a mark survives a render the single-flight slot refused ==\n");
   {
      /* A DIFFERENT REFUSAL, and a fix for the one above can leave this one
       * losing the request: it returns before any JNI is touched. Two
       * threads, because that is what the state is -- the service tick and
       * the activity's 1 Hz timer both hold this mark's only record. */
      reset_renders();
      pthread_mutex_lock(&g_hold_lk);
      g_hold    = 1;
      g_release = 0;
      pthread_mutex_unlock(&g_hold_lk);

      pthread_t th;
      pthread_create(&th, 0, hold_thread, 0);

      pthread_mutex_lock(&g_hold_lk);
      while (!g_holding)
         pthread_cond_wait(&g_hold_cv, &g_hold_lk);
      pthread_mutex_unlock(&g_hold_lk);
      ck(renders() == 1, "the other thread is inside the render");

      /* A reading lands here: the mark is raised while the slot is taken. */
      notify_mark();
      notify_tick();
      ck(renders() == 1, "a tick that cannot enter the render does not render");

      pthread_mutex_lock(&g_hold_lk);
      g_hold    = 0;
      g_release = 1;
      pthread_cond_broadcast(&g_hold_cv);
      pthread_mutex_unlock(&g_hold_lk);
      pthread_join(th, 0);

      notify_tick();
      ck(renders() == 2, "...and once the slot is free the NEXT tick renders");
      notify_tick();
      ck(renders() == 2, "...once, not on every tick after it");
   }

   printf("\n%d assertions\n", nass);
   printf("%s\n", all ? "ALL NOTIFY TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
