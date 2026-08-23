/* SPDX-License-Identifier: GPL-3.0
 * plotpages.c --- the plot pages
 * Copyright 2026 Jakob Kastelic
 */
#include "db.h"
#include "http.h"
#include "oops.h" /* a scan that did not finish is not an empty archive */
#include "page.h"
#include "plots.h"
#include "posix.h" /* the one boundary beyond ISO C -- see posix.h */
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h> /* time_t, struct tm: a named day is a calendar date */

/* ---- the plot pages ----
 *
 * Same shape as the single-user version: /plots shows the last 24 hours and
 * the per-day time-in-range, then one link per MONTH that has data, so the
 * archive stays reachable while the page itself stays bounded. Each plot
 * links to its own datapoints.
 */
#define PLOT_IMG_W 720
#define PLOT_IMG_H 300

static const char *const MON[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

/* ---- THE ARCHIVE, WRITTEN A ROW AT A TIME ----------------------------
 *
 * These callbacks are what replaced a `static int64_t days[4096]`
 * in each of the two pages below: the query streams, and each row becomes a
 * line of HTML on the way past. The arrays held every day the account had
 * ever recorded -- eleven years' worth, after which the oldest months
 * silently disappeared from the navigation while the page reported itself
 * complete.
 *
 * `any` is the "did anything come back" flag the old `first`/`any` locals
 * were, carried in the context because the loop is now a callback. */
struct month_ctx {
   struct sb *s;
   const char *who;
   int any;
};

static int month_link(void *ctx, int year, int mon)
{
   struct month_ctx *m = ctx;
   if (!m->any) {
      sb_add(m->s, "<b>MONTHS</b>\n");
      m->any = 1;
   }
   sb_add(m->s, "<a href=\"/plots-%04d%02d%s\" style=display:block>%s %d</a>\n",
          year, mon, m->who, MON[(mon - 1) % 12], year);
   return 0;
}

struct day_ctx {
   struct sb *s;
   const char *who;
   int any;
};

static int day_link(void *ctx, int64_t day)
{
   struct day_ctx *d = ctx;
   struct tm tm;
   /* A day the calendar cannot express is not a day. The query bounded the
    * range, so this can only be a bucket a restore put there. */
   if (!sys_gmtime((time_t)(day * 86400), &tm))
      return 0;
   d->any = 1;
   sb_add(d->s,
          "<b>%04d-%02d-%02d</b>\n"
          "<a href=\"/day-%04d%02d%02d%s\">"
          "<img src=\"/plot-%04d%02d%02d.gif%s\" alt=\"plot\" width=%d"
          " height=%d style=\"display:block;width:100%%;max-width:%dpx;"
          "height:auto;margin:0 0 2em 0\"></a>\n",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, /* the heading */
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, d->who, /* /day- */
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, d->who, /* the gif */
          PLOT_IMG_W, PLOT_IMG_H, PLOT_IMG_W);
   return 0;
}

/* THE MONTH AS A HALF-OPEN RANGE OF BUCKETS, [*day0, *day1). 0 if the month
 * is not one the calendar has -- which is a URL somebody typed, not a state
 * this server can produce. */
static int month_days(int year, int mon, int64_t *day0, int64_t *day1)
{
   if (year < 1970 || year > 9998 || mon < 1 || mon > 12)
      return 0;
   struct tm a = {0};
   struct tm b = {0};
   a.tm_year   = year - 1900;
   a.tm_mon    = mon - 1;
   a.tm_mday   = 1;
   b.tm_year   = (mon == 12 ? year + 1 : year) - 1900;
   b.tm_mon    = (mon == 12 ? 1 : mon + 1) - 1;
   b.tm_mday   = 1;
   time_t t0   = sys_timegm(&a);
   time_t t1   = sys_timegm(&b);
   if (t0 == (time_t)-1 || t1 == (time_t)-1 || t1 <= t0)
      return 0;
   *day0 = (int64_t)t0 / 86400;
   *day1 = (int64_t)t1 / 86400;
   return 1;
}

void h_plots(struct req *r, int64_t owner)
{
   struct sb s = {0};
   sb_add(&s,
          "<b>LAST 24 HOURS</b>\n"
          "<a href=\"/data-24h%s\">"
          "<img src=\"/plot-24h.gif%s\" alt=\"plot\" width=%d height=%d"
          " style=\"display:block;width:100%%;max-width:%dpx;height:auto;"
          "margin:0 0 2em 0\"></a>\n",
          r->who, r->who, PLOT_IMG_W, PLOT_IMG_H, PLOT_IMG_W);

   /* One link per month with data, newest first -- grouped by the DATABASE
    * and written straight into the page. Nothing here holds a list
    * of days, so nothing here has a lifetime ceiling. */
   struct month_ctx mc = {&s, r->who, 0};
   if (plot_months(r->db, owner, month_link, &mc) != 0) {
      /* THE SCAN DID NOT FINISH. "No days with data yet" would be a claim
       * about this person's record that nothing here is in a position to
       * make -- see the same distinction at newest_reading in home.c. */
      sb_free(&s);
      oops(r);
      return;
   }
   if (!mc.any)
      sb_add(&s, "<p>No days with data yet.</p>\n");
   sub_page(r, "Plots", s.p ? s.p : "");
   sb_free(&s);
}

/* /plots-YYYYMM: one plot per day of that month, newest first. */
void h_plots_month(struct req *r, int64_t owner, int year, int mon)
{
   struct sb s = {0};
   /* THE MONTH IS ASKED FOR BY ITS BUCKET RANGE, rather than by walking every
    * day the account has ever had and skipping the ones that do not match
    *. A bucket IS floor(instant / 86400) in UTC, so the range is
    * exact arithmetic on the same calendar the buckets were assigned with. */
   int64_t day0 = 0, day1 = 0;
   if (!month_days(year, mon, &day0, &day1)) {
      sb_add(&s, "<p>No data in that month.</p>\n");
      char t0[32];
      (void)snprintf(t0, sizeof t0, "%s %d", MON[(mon - 1) % 12], year);
      sub_page(r, t0, s.p ? s.p : "");
      sb_free(&s);
      return;
   }
   struct day_ctx dc = {&s, r->who, 0};
   if (plot_days_in(r->db, owner, day0, day1, day_link, &dc) != 0) {
      sb_free(&s);
      oops(r);
      return;
   }
   if (!dc.any)
      sb_add(&s, "<p>No data in that month.</p>\n");
   char title[32];
   (void)snprintf(title, sizeof title, "%s %d", MON[(mon - 1) % 12], year);
   sub_page(r, title, s.p ? s.p : "");
   sb_free(&s);
}

/* The datapoints behind a plot, as text: the same list the main page shows,
 * for a chosen window. */
void h_data(struct req *r, int64_t owner, int64_t t0, int64_t t1,
            const char *title)
{
   int tz = tz_of(r->db, owner);
   sqlite3_stmt *st =
       db_prep(r->db, "SELECT line FROM logrow WHERE user_id=?"
                      " AND log='readings' AND bucket BETWEEN ? AND ?"
                      " ORDER BY bucket DESC, line DESC");
   struct sb s = {0};
   sb_add(&s, "<pre>\n");
   int n = 0;
   if (st) {
      sqlite3_bind_int64(st, 1, owner);
      sqlite3_bind_int64(st, 2, (t0 / 86400) - 1);
      sqlite3_bind_int64(st, 3, (t1 / 86400) + 1);
      int prc;
      while ((prc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *ln = (const char *)sqlite3_column_text(st, 0);
         if (!ln || *ln < '0' || *ln > '9')
            continue;
         int64_t t = strtoll(ln, NULL, 10);
         if (t <= t0 || t > t1)
            continue;
         const char *c1 = strchr(ln, ',');
         if (!c1)
            continue;
         char when[64];
         stamp_local(t, tz, when, sizeof when);
         sb_add(&s, "%s %4" PRIwire "\n", when,
                (int64_t)strtoll(c1 + 1, NULL, 10));
         n++;
      }
      /* "nothing in this window" and "the scan stopped" are different
       * answers, and a datapoint listing that quietly ends early is the one
       * that gets believed. */
      int pok = db_finished(prc);
      sqlite3_finalize(st);
      if (!pok)
         sb_add(&s, "(this window could not be read in full)\n");
   }
   if (!n)
      sb_add(&s, "(nothing in this window)\n");
   sb_add(&s, "</pre>\n");
   sub_page(r, title, s.p ? s.p : "");
   sb_free(&s);
}

/* ---- READING A DATE OUT OF A PATH -----------------------------------------
 *
 * EXACTLY `n` DIGITS, AND NOTHING STRTOL WOULD FORGIVE.
 *
 * The date fields do NOT go through strtol, which skips leading whitespace,
 * accepts a sign, and stops wherever it likes while reporting the prefix it
 * managed. r->path is percent-DECODED, so a client can put those characters
 * in a path: "/day-2025%2B131" arrives as "/day-2025+131", which is the right
 * length, and strtol reads "+1" as January and "31" as the day. The page then
 * comes up as 2025-01-31 under a URL that is not that day's URL. A date is
 * eight digits or it is not a date. */
static int digits_n(const char *s, int n, int *out)
{
   int v = 0;
   for (int i = 0; i < n; i++) {
      if (s[i] < '0' || s[i] > '9')
         return 0;
      v = (v * 10) + (s[i] - '0');
   }
   *out = v;
   return 1;
}

/* How long `mon` (1..12) is in `year` -- 0 for a month that does not exist.
 * The Gregorian rule in full: every fourth year, except centuries, except
 * every fourth century. 2000 is a leap year and 1900 was not. */
static int month_len(int year, int mon)
{
   static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
   if (mon < 1 || mon > 12)
      return 0;
   if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
      return 29;
   return len[mon - 1];
}

/* YYYYMMDD -> a date that EXISTS, or 0.
 *
 * A bound of 1..31 for every month leaves the date to timegm, whose job is to
 * NORMALISE: February 31 becomes March 3, April 31 becomes May 1, and a
 * non-leap February 29 becomes March 1. The window moves; the title does not,
 * because the title is built from the digits that were asked for. So
 * /day-20250231 renders a page headed 2025-02-31 showing March 3rd's readings
 * -- the wrong data under a confident label, which is worse than no page at
 * all.
 *
 * Month-specific bounds rather than a timegm round-trip: the bounds are the
 * calendar, decided here and the same on every platform, whereas what timegm
 * does with an out-of-range field is glibc's normalisation and what it does
 * on overflow is (time_t)-1 that also spells a real second. The round-trip is
 * still checked at the call site, but as a guard on the arithmetic rather than
 * as the definition of a valid date. */
static int day_parse(const char *ds, int *y, int *mo, int *d)
{
   int yy = 0, mm = 0, dd = 0;
   if (!digits_n(ds, 4, &yy) || !digits_n(ds + 4, 2, &mm) ||
       !digits_n(ds + 6, 2, &dd))
      return 0;
   if (yy <= 1970) /* before the epoch there is nothing to plot */
      return 0;
   int len = month_len(yy, mm);
   if (!len || dd < 1 || dd > len)
      return 0;
   *y  = yy;
   *mo = mm;
   *d  = dd;
   return 1;
}

/* YYYYMM, for the month archive: the same digit rule, no day. */
static int month_parse(const char *ms, int *y, int *mo)
{
   int yy = 0, mm = 0;
   if (!digits_n(ms, 4, &yy) || !digits_n(ms + 4, 2, &mm))
      return 0;
   if (yy <= 1970 || mm < 1 || mm > 12)
      return 0;
   *y  = yy;
   *mo = mm;
   return 1;
}

/* Which plot: /plots, the 24h image and its datapoints, a named day as either,
 * or a month of the archive. One function because they all answer the same
 * two questions first -- whose record, and in which time zone -- and differ
 * only in how they read a date out of the path. */
void h_plot_route(struct req *r, int64_t me)
{
   int64_t owner = viewed_owner(r, me, NULL);
   if (!owner)
      return; /* it already answered: not shared, or no such record */
   int tz      = tz_of(r->db, owner);
   int64_t now = (int64_t)time(NULL);
   if (!strcmp(r->path, "/plots")) {
      h_plots(r, owner);
      return;
   }
   if (!strcmp(r->path, "/plot-24h.gif")) {
      h_plot_gif(r, owner, now - (24L * 3600), now, 24, tz);
      return;
   }
   if (!strcmp(r->path, "/data-24h")) {
      h_data(r, owner, now - (24L * 3600), now, "Last 24 hours");
      return;
   }
   /* A named day: YYYYMMDD, as a plot or as its datapoints. Rendered on
    * the LOCAL day boundary, which is what the labels say. */
   int y = 0, mo = 0, d = 0;
   const char *ds = 0;
   int want_gif   = 0;
   /* The image path is "/plot-YYYYMMDD.gif" -- all of it. The length was the
    * only test, so "/plot-20250203zzzz" served the image too. */
   if (!strncmp(r->path, "/plot-", 6) && strlen(r->path) == 18 &&
       !strcmp(r->path + 14, ".gif")) {
      ds       = r->path + 6;
      want_gif = 1;
   } else if (!strncmp(r->path, "/day-", 5) && strlen(r->path) == 13)
      ds = r->path + 5;
   if (ds && day_parse(ds, &y, &mo, &d)) {
      struct tm tm;
      memset(&tm, 0, sizeof tm);
      tm.tm_year      = y - 1900;
      tm.tm_mon       = mo - 1;
      tm.tm_mday      = d;
      time_t midnight = sys_timegm(&tm);
      /* The date exists; this says the ARITHMETIC survived it. timegm reports
       * failure as (time_t)-1, and gmtime_r can fail too -- neither may be
       * allowed to become a window silently labelled with the date asked
       * for. */
      struct tm back;
      if (midnight != (time_t)-1 && sys_gmtime(midnight, &back) &&
          back.tm_year + 1900 == y && back.tm_mon + 1 == mo &&
          back.tm_mday == d) {
         int64_t day_utc = (int64_t)midnight - ((int64_t)tz * 60);
         if (want_gif)
            h_plot_gif(r, owner, day_utc, day_utc + 86400, 24, tz);
         else {
            char title[32];
            (void)snprintf(title, sizeof title, "%04d-%02d-%02d", y, mo, d);
            h_data(r, owner, day_utc, day_utc + 86400, title);
         }
         return;
      }
   }
   if (!strncmp(r->path, "/plots-", 7) && strlen(r->path) == 13 &&
       month_parse(r->path + 7, &y, &mo)) {
      h_plots_month(r, owner, y, mo);
      return;
   }
   /* NOT NORMALISED, NOT SUBSTITUTED WITH TODAY: a date that does not exist
    * is not a page. */
   page(r, 404, "Not Found", "Pancra",
        "<p>No such plot. A day is /day-YYYYMMDD, and the date has to be one"
        " the calendar has.</p>");
   return;
}
