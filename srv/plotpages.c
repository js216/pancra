/* SPDX-License-Identifier: GPL-3.0
 * plotpages.c --- the plot pages
 * Copyright 2026 Jakob Kastelic
 */
#include "db.h"
#include "http.h"
#include "page.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
   int nd       = plot_days(owner, days, (int)(sizeof days / sizeof days[0]));
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
   int nd  = plot_days(owner, days, (int)(sizeof days / sizeof days[0]));
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
   int tz           = tz_of(owner);
   sqlite3_stmt *st = db_prep("SELECT line FROM logrow WHERE user_id=?"
                              " AND log='readings' AND bucket BETWEEN ? AND ?"
                              " ORDER BY bucket DESC, line DESC");
   struct sb s      = {0};
   sb_add(&s, "<pre>\n");
   int n = 0;
   if (st) {
      sqlite3_bind_int64(st, 1, owner);
      sqlite3_bind_int64(st, 2, (t0 / 86400) - 1);
      sqlite3_bind_int64(st, 3, (t1 / 86400) + 1);
      while (sqlite3_step(st) == SQLITE_ROW) {
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
      sqlite3_finalize(st);
   }
   if (!n)
      sb_add(&s, "(nothing in this window)\n");
   sb_add(&s, "</pre>\n");
   sub_page(r, title, s.p ? s.p : "");
   sb_free(&s);
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
   int tz   = tz_of(owner);
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
   if (!strncmp(r->path, "/plot-", 6) && strlen(r->path) == 18)
      ds = r->path + 6;
   else if (!strncmp(r->path, "/day-", 5) && strlen(r->path) == 13)
      ds = r->path + 5;
   if (ds) {
      y  = (int)strtol((char[5]){ds[0], ds[1], ds[2], ds[3], 0}, NULL, 10);
      mo = (int)strtol((char[3]){ds[4], ds[5], 0}, NULL, 10);
      d  = (int)strtol((char[3]){ds[6], ds[7], 0}, NULL, 10);
      if (y > 1970 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
         struct tm tm;
         memset(&tm, 0, sizeof tm);
         tm.tm_year   = y - 1900;
         tm.tm_mon    = mo - 1;
         tm.tm_mday   = d;
         long day_utc = (long)timegm(&tm) - ((long)tz * 60);
         if (r->path[1] == 'p')
            h_plot_gif(r, owner, day_utc, day_utc + 86400, 24, tz);
         else {
            char title[32];
            (void)snprintf(title, sizeof title, "%04d-%02d-%02d", y, mo, d);
            h_data(r, owner, day_utc, day_utc + 86400, title);
         }
         return;
      }
   }
   if (!strncmp(r->path, "/plots-", 7) && strlen(r->path) == 13) {
      y = (int)strtol(
          (char[5]){r->path[7], r->path[8], r->path[9], r->path[10], 0}, NULL,
          10);
      mo = (int)strtol((char[3]){r->path[11], r->path[12], 0}, NULL, 10);
      if (y > 1970 && mo >= 1 && mo <= 12) {
         h_plots_month(r, owner, y, mo);
         return;
      }
   }
   page(r, 404, "Not Found", "Pancra", "<p>No such plot.</p>");
   return;
}
