// SPDX-License-Identifier: GPL-3.0
// gluhist.c --- GLUCOSE HISTORY: which day, week, month or year is shown
// Copyright 2026 Jakob Kastelic
#include "gluhist.h"
#include "blejni.h" /* dexble_env: the zone is asked of Android */
#include "civil.h"
#include "clock.h" /* realtime_s: the day the screen opens on */
#include "forms.h" /* forms_kp_open: a date part is typed on the keypad */
#include "keypad.h"
#include "nav.h"
#include "plotdata.h" /* plot_window_forget: opening reads the log afresh */
#include "tzoff.h"
#include "uiact.h"
#include <jni.h>   /* JNIEnv: the zone is asked on the drawing thread */
#include <stdio.h> /* snprintf: the refusal names the range */

#define DAY_S 86400L

/* The span last chosen, kept for the life of the process, and the chosen
 * date in days since 1970-01-01. */
static int g_span = HSPAN_DAY;
static long g_day;

/* civil.h asks the zone through a context pointer; this carries the plain
 * "offset at instant" function through it. */
struct tz_ctx {
   long (*fn)(long);
};

static long tz_tramp(void *ctx, long t)
{
   const struct tz_ctx *c = ctx;
   return c->fn(t);
}

/* The phone's zone, cached per day by tzoff.c, as every drawn date reads it. */
static long gh_tz(long t)
{
   JNIEnv *env = dexble_env();
   return env ? tz_shown_at(env, t) : tz_off_now();
}

/* The instant of local midnight starting `day`. */
static long midnight(long day, long (*tz)(long))
{
   struct tz_ctx c = {tz};
   return civil_resolve(day * DAY_S, tz_tramp, &c).t;
}

static int month_len(long y, long m)
{
   long ny = (m == 12) ? y + 1 : y;
   long nm = (m == 12) ? 1 : m + 1;
   return (int)(civil_days(ny, nm, 1) - civil_days(y, m, 1));
}

/* `day` moved by `months` calendar months, its day of the month clamped to
 * the length of the month it lands in. */
static long add_months(long day, long months)
{
   long y = 0;
   long m = 0;
   long d = 0;
   civil_ymd(day, &y, &m, &d);
   long idx = (y * 12) + (m - 1) + months;
   y        = idx / 12;
   m        = (idx % 12) + 1;
   if (d > month_len(y, m))
      d = month_len(y, m);
   return civil_days(y, m, d);
}

/* The first day after a window of `span` starting on `day`. */
static long span_end(int span, long day)
{
   if (span == HSPAN_WEEK)
      return day + 7;
   if (span == HSPAN_MONTH)
      return add_months(day, 1);
   if (span == HSPAN_QUARTER)
      return add_months(day, 3);
   if (span == HSPAN_YEAR)
      return add_months(day, 12);
   return day + 1;
}

void gluhist_window_of(int span, long day, long (*tz)(long), long *start,
                       long *end, struct ui_gluhist *out)
{
   *start    = midnight(day, tz);
   *end      = midnight(span_end(span, day), tz);
   out->span = span;
   civil_ymd(day, &out->y, &out->mo, &out->d);
}

void gluhist_window(long *start, long *end, struct ui_gluhist *out)
{
   gluhist_window_of(g_span, g_day, gh_tz, start, end, out);
}

long gluhist_step_day(long day, int step)
{
   if (step == GH_DAY_BACK)
      return day - 1;
   if (step == GH_DAY_ON)
      return day + 1;
   if (step == GH_WEEK_BACK)
      return day - 7;
   if (step == GH_WEEK_ON)
      return day + 7;
   if (step == GH_MONTH_BACK)
      return add_months(day, -1);
   if (step == GH_MONTH_ON)
      return add_months(day, 1);
   if (step == GH_YEAR_BACK)
      return add_months(day, -12);
   if (step == GH_YEAR_ON)
      return add_months(day, 12);
   return day;
}

int gluhist_set_part(int part, long v, char *err, int cap)
{
   long y = 0;
   long m = 0;
   long d = 0;
   civil_ymd(g_day, &y, &m, &d);
   if (part == GH_PART_YEAR) {
      if (v < 1970 || v > 9999) {
         (void)snprintf(err, (size_t)cap, "YEAR MUST BE 1970..9999");
         return -1;
      }
      y = v;
      if (d > month_len(y, m))
         d = month_len(y, m);
   } else if (part == GH_PART_MMDD) {
      const long nm = v / 100;
      const long nd = v % 100;
      if (nm < 1 || nm > 12) {
         (void)snprintf(err, (size_t)cap, "MONTH MUST BE 01..12");
         return -1;
      }
      if (nd < 1 || nd > month_len(y, nm)) {
         (void)snprintf(err, (size_t)cap, "DAY MUST BE 01..%d",
                        month_len(y, nm));
         return -1;
      }
      m = nm;
      d = nd;
   } else {
      return 0;
   }
   g_day = civil_days(y, m, d);
   return 0;
}

/* The local day holding the instant `t`. */
static long local_day(long t)
{
   const long local = t + gh_tz(t);
   long day         = local / DAY_S;
   if (local % DAY_S < 0)
      day--;
   return day;
}

/* THE FIRST DAY OF THE WINDOW THAT ENDS WITH TODAY, in `span`: today itself
 * for a day, and otherwise the day that many days or months back from
 * tomorrow, so today is the window's last day. */
static long ending_today(int span)
{
   long day = local_day((long)realtime_s());
   if (span == HSPAN_WEEK)
      return day - 6;
   if (span == HSPAN_MONTH)
      return add_months(day + 1, -1);
   if (span == HSPAN_QUARTER)
      return add_months(day + 1, -3);
   if (span == HSPAN_YEAR)
      return add_months(day + 1, -12);
   return day;
}

int gluhist_action(int action, int ix)
{
   if (action == MA_GLUHIST_OPEN) {
      g_day = ending_today(g_span);
      plot_window_forget();
      nav_go(SCR_GLUHIST);
   } else if (action == MA_GLUHIST_BACK) {
      nav_back();
   } else if (action == MA_GLUHIST_SPAN) {
      if (ix >= 0 && ix < HSPAN_N)
         g_span = ix;
   } else if (action == MA_GLUHIST_STEP) {
      const long first = plot_first_cgm();
      if (ix == GH_TODAY)
         g_day = ending_today(g_span);
      else if (ix == GH_FIRST && first > 0)
         g_day = local_day(first);
      else if (ix >= 0 && ix < GH_STEP_N)
         g_day = gluhist_step_day(g_day, ix);
   } else if (action == MA_GLUHIST_DATE) {
      static const enum keypad_mode kp[GH_PART_N] = {KP_GH_YEAR, KP_GH_MMDD};

      if (ix >= 0 && ix < GH_PART_N) {
         nav_go(SCR_KEYPAD);
         forms_kp_open(kp[ix], SCR_GLUHIST);
      }
   } else {
      return 0;
   }
   return 1;
}
