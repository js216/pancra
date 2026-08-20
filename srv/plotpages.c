/* SPDX-License-Identifier: GPL-3.0
 * plotpages.c --- the plot pages
 * Copyright 2026 Jakob Kastelic
 */
#include "db.h"
#include "http.h"
#include "page.h"
#include "plots.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h> /* timegm/gmtime_r: a named day is a calendar date */

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

void h_plots(struct req *r, long owner)
{
   struct sb s = {0};
   sb_add(&s,
          "<b>LAST 24 HOURS</b>\n"
          "<a href=\"/data-24h%s\">"
          "<img src=\"/plot-24h.gif%s\" alt=\"plot\" width=%d height=%d"
          " style=\"display:block;width:100%%;max-width:%dpx;height:auto;"
          "margin:0 0 2em 0\"></a>\n",
          r->who, r->who, PLOT_IMG_W, PLOT_IMG_H, PLOT_IMG_W);

   /* One link per month with data, newest first. */
   static long days[4096];
   int nd = plot_days(r->db, owner, days, (int)(sizeof days / sizeof days[0]));
   if (nd < 0)
      nd = 0; /* an incomplete day list is not a day list */
   long prev_ym = -1;
   int first    = 1;
   for (int i = 0; i < nd; i++) {
      time_t tt = (time_t)(days[i] * 86400);
      struct tm tm;
      if (!gmtime_r(&tt, &tm))
         continue;
      long ym = ((tm.tm_year + 1900L) * 100) + tm.tm_mon + 1;
      if (ym == prev_ym)
         continue;
      prev_ym = ym;
      if (first) {
         sb_add(&s, "<b>MONTHS</b>\n");
         first = 0;
      }
      sb_add(&s,
             "<a href=\"/plots-%04d%02d%s\" style=display:block>%s %d</a>\n",
             tm.tm_year + 1900, tm.tm_mon + 1, r->who, MON[tm.tm_mon],
             tm.tm_year + 1900);
   }
   if (first)
      sb_add(&s, "<p>No days with data yet.</p>\n");
   sub_page(r, "Plots", s.p ? s.p : "");
   sb_free(&s);
}

/* /plots-YYYYMM: one plot per day of that month, newest first. */
void h_plots_month(struct req *r, long owner, int year, int mon)
{
   struct sb s = {0};
   static long days[4096];
   int nd = plot_days(r->db, owner, days, (int)(sizeof days / sizeof days[0]));
   if (nd < 0)
      nd = 0;
   int any = 0;
   for (int i = 0; i < nd; i++) {
      time_t tt = (time_t)(days[i] * 86400);
      struct tm tm;
      if (!gmtime_r(&tt, &tm))
         continue;
      if (tm.tm_year + 1900 != year || tm.tm_mon + 1 != mon)
         continue;
      any = 1;
      sb_add(&s,
             "<b>%04d-%02d-%02d</b>\n"
             "<a href=\"/day-%04d%02d%02d%s\">"
             "<img src=\"/plot-%04d%02d%02d.gif%s\" alt=\"plot\" width=%d"
             " height=%d style=\"display:block;width:100%%;max-width:%dpx;"
             "height:auto;margin:0 0 2em 0\"></a>\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, /* the heading */
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, r->who, /* /day- */
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, r->who, /* the gif */
             PLOT_IMG_W, PLOT_IMG_H, PLOT_IMG_W);
   }
   if (!any)
      sb_add(&s, "<p>No data in that month.</p>\n");
   char title[32];
   (void)snprintf(title, sizeof title, "%s %d", MON[(mon - 1) % 12], year);
   sub_page(r, title, s.p ? s.p : "");
   sb_free(&s);
}

/* The datapoints behind a plot, as text: the same list the main page shows,
 * for a chosen window. */
void h_data(struct req *r, long owner, long t0, long t1, const char *title)
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
         long t = strtol(ln, NULL, 10);
         if (t <= t0 || t > t1)
            continue;
         const char *c1 = strchr(ln, ',');
         if (!c1)
            continue;
         char when[64];
         stamp_local(t, tz, when, sizeof when);
         sb_add(&s, "%s %4ld\n", when, strtol(c1 + 1, NULL, 10));
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
 * The date fields used to go through strtol, which skips leading whitespace,
 * accepts a sign, and stops wherever it likes while reporting the prefix it
 * managed. r->path is percent-DECODED, so a client could put those characters
 * in a path: "/day-2025%2B131" arrives as "/day-2025+131", which is the right
 * length, and strtol read "+1" as January and "31" as the day. The page then
 * came up as 2025-01-31 under a URL that is not that day's URL. A date is
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
 * The bound used to be 1..31 for every month, and the date was then handed to
 * timegm, whose job is to NORMALISE: February 31 became March 3, April 31
 * became May 1, and a non-leap February 29 became March 1. The window moved;
 * the title did not, because the title was built from the digits that were
 * asked for. So /day-20250231 rendered a page headed 2025-02-31 showing March
 * 3rd's readings -- the wrong data under a confident label, which is worse
 * than no page at all.
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
void h_plot_route(struct req *r, long me)
{
   long owner = viewed_owner(r, me, NULL);
   if (!owner)
      return; /* it already answered: not shared, or no such record */
   int tz   = tz_of(r->db, owner);
   long now = (long)time(NULL);
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
      time_t midnight = timegm(&tm);
      /* The date exists; this says the ARITHMETIC survived it. timegm reports
       * failure as (time_t)-1, and gmtime_r can fail too -- neither may be
       * allowed to become a window silently labelled with the date asked
       * for. */
      struct tm back;
      if (midnight != (time_t)-1 && gmtime_r(&midnight, &back) &&
          back.tm_year + 1900 == y && back.tm_mon + 1 == mo &&
          back.tm_mday == d) {
         long day_utc = (long)midnight - ((long)tz * 60);
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
