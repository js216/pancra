/* SPDX-License-Identifier: GPL-3.0
 * datetest.c --- a named day is a date the CALENDAR has, or it is a 404
 * Copyright 2026 Jakob Kastelic
 *
 * WHAT THIS PINS, and why it is worth its own binary.
 *
 * /day-YYYYMMDD and /plot-YYYYMMDD.gif name a day. The router used to bound
 * the day field to 1..31 for every month and hand the result to timegm, whose
 * job is to NORMALISE -- so February 31 became March 3, April 31 became May 1,
 * and February 29 of a non-leap year became March 1. The window moved. The
 * TITLE did not, because the title was printed from the digits that were
 * asked for. /day-20250231 therefore came up headed "2025-02-31" showing
 * March 3rd's readings: the wrong data under a confident label, which on this
 * project is worse than an error page.
 *
 * The fields also went through strtol, which skips whitespace, accepts a sign
 * and stops where it likes. r->path is percent-DECODED, so "/day-2025%2B131"
 * arrives as "/day-2025+131" -- the right length -- and used to resolve to
 * January 31st.
 *
 * So the properties here are:
 *   1. every date the calendar has is accepted, and
 *   2. the window rendered is the day in the TITLE, to the second, and
 *   3. every date the calendar does not have is REFUSED with a 404, with no
 *      plot and no datapoint list rendered at all, and
 *   4. eight digits means eight digits.
 *
 * It drives the real h_plot_route (srv/plotpages.c). Everything below the
 * router -- the database, the renderer, the page skeleton -- is stubbed, so
 * what a case observes is exactly what the router decided: which handler ran,
 * with which window, under which title.
 *
 * Built and run by `make datetest`.
 */
#include "db.h"
#include "page.h"
#include "plots.h"
#include "proto.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* ---- what the router did, as seen from underneath ---------------------- */

#define TZ_MIN (-420) /* the test's fixed offset: UTC-7, i.e. -420 minutes */

static struct {
   int gif;  /* h_plot_gif calls */
   int data; /* pages rendered through sub_page: h_data, h_plots_month */
   long t0, t1;
   char title[64];
   int code;
} got;

static void reset(void)
{
   memset(&got, 0, sizeof got);
}

/* ---- the stubs: everything h_plot_route reaches ------------------------ */

long viewed_owner(struct req *r, long me, int *have_own)
{
   (void)r;
   (void)me;
   if (have_own)
      *have_own = 1;
   return 1; /* always a record, so every case reaches the date */
}

int tz_of(struct db *d, long uid)
{
   (void)d;
   (void)uid;
   return TZ_MIN;
}

void h_plot_gif(struct req *r, long owner, long win_start, long win_end,
                int hours, int tz_min)
{
   (void)r;
   (void)owner;
   (void)hours;
   (void)tz_min;
   got.gif++;
   got.t0   = win_start;
   got.t1   = win_end;
   got.code = 200;
}

/* h_data and h_plots_month are the real ones in plotpages.c: they are what
 * print the heading, and the heading is half of what went wrong. Their
 * database reads go through the stubs below and find nothing, so what reaches
 * sub_page is the title the router asked for. */
void page(struct req *r, int code, const char *reason, const char *title,
          const char *body_html)
{
   (void)reason;
   (void)body_html;
   r->resp_code = code;
   got.code     = code;
   (void)snprintf(got.title, sizeof got.title, "%s", title ? title : "");
}

void sub_page(struct req *r, const char *title, const char *body_html)
{
   (void)r;
   (void)body_html;
   got.data++;
   got.code = 200;
   (void)snprintf(got.title, sizeof got.title, "%s", title ? title : "");
}

/* h_data records the window through this, since sub_page does not see it. */
void stamp_local(long t, int tz_min, char *out, size_t cap)
{
   (void)t;
   (void)tz_min;
   (void)snprintf(out, cap, "-");
}

int plot_days(struct db *d, long owner, long *out, int cap)
{
   (void)d;
   (void)owner;
   (void)out;
   (void)cap;
   return 0;
}

struct sqlite3_stmt *db_prep(struct db *d, const char *sql)
{
   (void)d;
   (void)sql;
   return 0; /* no rows: h_data prints "(nothing in this window)" */
}

int db_finished(int rc)
{
   return rc == SQLITE_DONE;
}

/* Referenced by plotpages.c's h_data but unreachable with db_prep at NULL. */
int sqlite3_bind_int64(sqlite3_stmt *s, int i, sqlite3_int64 v)
{
   (void)s;
   (void)i;
   (void)v;
   return SQLITE_OK;
}

int sqlite3_step(sqlite3_stmt *s)
{
   (void)s;
   return SQLITE_DONE;
}

const unsigned char *sqlite3_column_text(sqlite3_stmt *s, int i)
{
   (void)s;
   (void)i;
   return 0;
}

int sqlite3_finalize(sqlite3_stmt *s)
{
   (void)s;
   return SQLITE_OK;
}

/* ---- driving one request ---------------------------------------------- */

static void ask(const char *path)
{
   reset();
   static struct req r;
   memset(&r, 0, sizeof r);
   (void)snprintf(r.method, sizeof r.method, "GET");
   (void)snprintf(r.path, sizeof r.path, "%s", path);
   (void)snprintf(r.target, sizeof r.target, "%s", path);
   h_plot_route(&r, 1);
   if (r.resp) {
      free(r.resp);
      r.resp = 0;
   }
}

/* The UTC midnight of y-m-d, computed independently of the code under test:
 * days since the epoch by Howard Hinnant's days-from-civil algorithm (the
 * one app/civil.c uses on the phone), so a bug shared with timegm cannot hide
 * here. */
static long civil_midnight(int y, int m, int d)
{
   long yy = y;
   yy -= m <= 2;
   long era  = (yy >= 0 ? yy : yy - 399) / 400;
   long yoe  = yy - era * 400;
   long doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
   long doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   long days = era * 146097 + doe - 719468;
   return days * 86400;
}

/* A day the calendar HAS: accepted, titled with the date that was asked for,
 * and -- the property the old code broke -- rendering THAT day's window.
 *
 * The title is observed on /day- (h_data prints it through sub_page) and the
 * window on /plot-....gif (h_plot_gif is handed it directly). They are the two
 * branches of one `if`, fed by one parse and one day_utc: a date that had been
 * normalised away from the digits in the URL would show up as a window that is
 * not the titled day's midnight. */
static void ok_day(int y, int m, int d, const char *why)
{
   char path[64], want[64], what[200];
   (void)snprintf(path, sizeof path, "/day-%04d%02d%02d", y, m, d);
   ask(path);
   (void)snprintf(what, sizeof what, "%s accepted (%s)", path, why);
   ck(got.data == 1 && got.code == 200, what);
   (void)snprintf(want, sizeof want, "%04d-%02d-%02d", y, m, d);
   (void)snprintf(what, sizeof what, "%s is titled %s", path, want);
   ck(!strcmp(got.title, want), what);

   /* The window: local midnight of that very date, for exactly 86400
    * seconds. civil_midnight is computed here, without timegm. */
   long t0 = civil_midnight(y, m, d) - (long)TZ_MIN * 60;
   (void)snprintf(path, sizeof path, "/plot-%04d%02d%02d.gif", y, m, d);
   ask(path);
   (void)snprintf(what, sizeof what, "%s renders %s and no other day", path,
                  want);
   ck(got.gif == 1 && got.t0 == t0, what);
   (void)snprintf(what, sizeof what, "%s spans exactly one day", path);
   ck(got.t1 - got.t0 == 86400, what);
}

/* Some cases ARE a control character, so the label may not be the path
 * verbatim: a tab or a newline in the middle of a result line makes the log
 * unreadable, which is a bad way to report a failure. */
static void printable(const char *in, char *out, size_t cap)
{
   size_t k = 0;
   for (const char *p = in; *p && k + 5 < cap; p++) {
      if (*p == '\t' || *p == '\n' || *p == '\r') {
         out[k++] = '\\';
         out[k++] = *p == '\t' ? 't' : (*p == '\n' ? 'n' : 'r');
      } else if ((unsigned char)*p < 0x20) {
         k += (size_t)snprintf(out + k, cap - k, "\\x%02x", (unsigned char)*p);
      } else
         out[k++] = *p;
   }
   out[k] = '\0';
}

/* A day the calendar does NOT have: 404, and NOTHING rendered. */
static void bad_day(const char *path, const char *why)
{
   ask(path);
   char shown[128], what[240];
   printable(path, shown, sizeof shown);
   (void)snprintf(what, sizeof what, "%s is refused: %s", shown, why);
   ck(got.code == 404, what);
   (void)snprintf(what, sizeof what, "...and %s renders no day at all", shown);
   ck(got.data == 0 && got.gif == 0, what);
}

int main(void)
{
   printf("datetest: a named day is a date the calendar has\n");

   /* ---- 1. every month's real length is accepted, and its length+1 is
    * not. This is the whole table, both ways round. ---- */
   static const int LEN[13] = {0,  31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
   for (int m = 1; m <= 12; m++) {
      ok_day(2025, m, LEN[m], "the last day of the month");
      char path[64], why[80];
      (void)snprintf(path, sizeof path, "/day-2025%02d%02d", m, LEN[m] + 1);
      (void)snprintf(why, sizeof why, "month %d has only %d days", m, LEN[m]);
      bad_day(path, why);
   }

   /* ---- 2. February, in a leap year and out of one ---- */
   ok_day(2024, 2, 28, "February 28th of a leap year");
   ok_day(2024, 2, 29, "February 29th of a leap year");
   bad_day("/day-20240230", "February never has 30 days");
   ok_day(2025, 2, 28, "February 28th of a common year");
   bad_day("/day-20250229", "2025 is not a leap year");
   bad_day("/day-20250230", "nor does any February have a 30th");
   /* The century rule, both exceptions -- and all of them AFTER the epoch, so
    * the year floor cannot be what refuses them. (1900, the textbook
    * non-leap century, is refused by the floor instead and would prove
    * nothing about the leap rule.) */
   ok_day(2000, 2, 29, "2000 IS a leap year: divisible by 400");
   ok_day(2400, 2, 29, "2400 is, for the same reason 2000 is");
   bad_day("/day-21000229", "2100 is a century and not a fourth one: not leap");
   bad_day("/day-22000229", "nor is 2200");
   bad_day("/day-23000229", "nor 2300");

   /* ---- 3. the specific normalisations the old code performed ---- */
   bad_day("/day-20250231", "timegm would have made this March 3rd");
   bad_day("/day-20250431", "timegm would have made this May 1st");
   bad_day("/day-20250631", "timegm would have made this July 1st");
   bad_day("/day-20250931", "timegm would have made this October 1st");
   bad_day("/day-20251131", "timegm would have made this December 1st");

   /* ---- 4. month and day out of range at all ---- */
   bad_day("/day-20250015", "month 00 does not exist");
   bad_day("/day-20251315", "month 13 does not exist");
   bad_day("/day-20259915", "nor does month 99");
   bad_day("/day-20250100", "day 00 does not exist");
   bad_day("/day-20250199", "nor does day 99");
   bad_day("/day-19700101", "the epoch year is the floor, and it is exclusive");
   bad_day("/day-00000101", "year 0000 is not a year this serves");

   /* ---- 5. EIGHT DIGITS. Not seven, not nine, not "a number strtol found
    * somewhere in there". ---- */
   /* Seven digits, chosen so that a parser reading only what is there would
    * find a PLAUSIBLE date (2025-01-1) rather than a day of zero: a case that
    * is refused for the wrong reason proves nothing. */
   bad_day("/day-2025011", "seven digits is not a date, plausible or not");
   bad_day("/day-2025010", "...nor these seven");
   bad_day("/day-202501011", "nine digits is not a date either");
   bad_day("/day-", "and neither is nothing at all");
   bad_day("/day-2025-1-1", "dashes are not digits");
   bad_day("/day-2025011a", "a letter in the day field");
   bad_day("/day-2025a101", "a letter in the month field");
   bad_day("/day-20o50101", "a letter in the year field");
   bad_day("/day-2025+131", "a SIGN, which strtol accepts and a URL can send");
   bad_day("/day-2025 131", "a leading SPACE, which strtol also skips");
   bad_day("/day-20250101 ", "trailing whitespace");
   bad_day("/day- 0250101", "leading whitespace where the year begins");
   bad_day("/day-2025.1.1", "dots");
   bad_day("/day-2025\t131", "a TAB, which strtol skips as well");
   bad_day("/day-0x250101", "a hex prefix");
   bad_day("/day-2025011\n", "a newline in the eighth position");

   /* ---- 6. the same rules on the IMAGE path, which shares the parse ---- */
   ask("/plot-20250115.gif");
   ck(got.gif == 1 && got.code == 200, "/plot-20250115.gif renders a plot");
   {
      long t0 = civil_midnight(2025, 1, 15) - (long)TZ_MIN * 60;
      ck(got.t0 == t0 && got.t1 == t0 + 86400,
         "...for the local day it names, and no other");
   }
   bad_day("/plot-20250231.gif", "February 31st has no image either");
   bad_day("/plot-20250229.gif", "nor does a non-leap February 29th");
   bad_day("/plot-2025011a.gif", "nor does a non-digit");
   bad_day("/plot-20250115.giff", "the suffix is part of the name");
   bad_day("/plot-20250115zzzz", "...and the length alone was never the name");

   /* ---- 7. the month archive shares the digit rule. Its heading came from
    * MON[(mon - 1) % 12], so month 13 used to be titled "January". ---- */
   ask("/plots-202502");
   ck(got.data == 1 && !strcmp(got.title, "February 2025"),
      "/plots-202502 lists February 2025");
   ask("/plots-202513");
   ck(got.code == 404 && got.data == 0, "/plots-202513: month 13 is refused");
   ask("/plots-202500");
   ck(got.code == 404 && got.data == 0, "/plots-202500: month 00 is refused");
   ask("/plots-2025+2");
   ck(got.code == 404 && got.data == 0,
      "/plots-2025+2: a sign is refused there too");
   ask("/plots-2025 2");
   ck(got.code == 404 && got.data == 0, "/plots-2025 2: and a space");
   ask("/plots-196912");
   ck(got.code == 404 && got.data == 0,
      "/plots-196912: before the epoch there is nothing to list");
   ask("/plots-2025021");
   ck(got.code == 404 && got.data == 0,
      "/plots-2025021: seven digits is not a month either");
   ask("/plots-20250");
   ck(got.code == 404 && got.data == 0, "/plots-20250: nor is five");

   /* ---- 8. the routes that carry no date still work ---- */
   ask("/plots");
   ck(got.code == 200, "/plots still answers");
   ask("/plot-24h.gif");
   ck(got.gif == 1, "/plot-24h.gif still renders");
   ask("/data-24h");
   ck(got.data == 1 && !strcmp(got.title, "Last 24 hours"),
      "/data-24h still lists its window");

   if (fails) {
      printf("datetest: %d FAILURES\n", fails);
      return 1;
   }
   printf("datetest: ALL DATE TESTS PASSED\n");
   return 0;
}
