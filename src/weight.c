// SPDX-License-Identifier: GPL-3.0
// weight.c --- Body-weight log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* See weight.h. Freestanding like insulin.c, whose shape this follows
 * deliberately: hand parsers with digit caps, every row validated on the way
 * in, and a partial write rolled back so it cannot merge with the next append
 * into one unparseable row. */
#include "weight.h"
#include "dexlibc.h"
#include "util.h"
#include <stdio.h> /* snprintf, SEEK_END */

struct wt_rec g_wt[NWT];
int g_nwt;
char g_wt_path[256];

/* Column header, so an exported log is self-describing. GRAMS is named in the
 * header because the display unit is a preference and the file must not need
 * one to be read decades later. */
static const char g_wt_hdr[] = "# unix_time,grams,tz_offset_s\n";

const char *wt_unit_name(int units)
{
   return units == WT_LB ? "LB" : "KG";
}

long wt_from_tenths(int tenths, int units)
{
   if (tenths <= 0)
      return 0;
   long g = 0;
   if (units == WT_LB) {
      /* 1 lb = 453.59237 g, so a TENTH of a pound is 45.359237 g. Scaled
       * integer arithmetic with a half added before the divide, i.e. round to
       * nearest: 1540 (154.0 lb) -> 69853 g, which wt_to_tenths renders back
       * as exactly 1540. */
      g = (((long)tenths * 4535924L) + 50000L) / 100000L;
   } else {
      g = (long)tenths * 100L; /* tenths of a kg */
   }
   if (g < WT_MIN_G || g > WT_MAX_G)
      return 0;
   return g;
}

int wt_to_tenths(long g, int units)
{
   if (g <= 0)
      return 0;
   if (units == WT_LB) /* g / 45.359237, rounded to nearest */
      return (int)(((g * 100000L) + 2267962L) / 4535924L);
   return (int)((g + 50L) / 100L);
}

/* Decimal reader, same shape (and same digit-cap rationale) as insulin.c. */
static long rdnum(const char **p, const char *e)
{
   long v        = 0;
   int nd        = 0;
   int neg       = 0;
   const char *q = *p;
   if (q < e && *q == '-') {
      neg = 1;
      q++;
   }
   while (q < e && *q >= '0' && *q <= '9') {
      if (nd < 18) {
         v = (v * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   *p = q;
   return neg ? -v : v;
}

static void rdsep(const char **p, const char *e)
{
   if (*p < e && **p == ',')
      (*p)++;
}

/* Overflow drops the FIRST ROW IN THE TAIL, which is the oldest by ARRIVAL,
 * not necessarily the oldest by time: rows are pushed in file order and the
 * sort runs afterwards, so a backdated entry loaded near the end of the file
 * can evict a row older than itself. Display-only -- the file keeps
 * everything and the next launch re-reads it -- but the tail is a window on
 * arrival order, which is what the file is, not a strict time window. */
static void wt_push(const struct wt_rec *r)
{
   if (g_nwt == NWT) {
      for (int i = 1; i < NWT; i++)
         g_wt[i - 1] = g_wt[i];
      g_nwt--;
   }
   g_wt[g_nwt++] = *r;
}

/* Oldest first. A backdated entry files into place immediately, so the table
 * never shows one row out of order until the next launch. Insertion sort: the
 * log is already sorted but for the row just added. */
static void wt_sort(void)
{
   for (int i = 1; i < g_nwt; i++) {
      struct wt_rec k = g_wt[i];
      int j           = i - 1;
      while (j >= 0 && g_wt[j].t > k.t) {
         g_wt[j + 1] = g_wt[j];
         j--;
      }
      g_wt[j + 1] = k;
   }
}

static int wt_parse_rec(const char *p, const char *e, struct wt_rec *r)
{
   const char *q = p;
   r->t          = rdnum(&q, e);
   rdsep(&q, e);
   r->g = rdnum(&q, e);
   if (r->t <= 0 || r->t >= WT_T_MAX)
      return 0;
   if (r->g < WT_MIN_G || r->g > WT_MAX_G)
      return 0;
   return 1;
}

static void wt_parse_line(const char *p, const char *e)
{
   if (p < e && *p == '#')
      return; /* the header */
   struct wt_rec r = {0, 0};
   r.t             = rdnum(&p, e);
   rdsep(&p, e);
   r.g = rdnum(&p, e);
   /* Validate on the way IN. This file is loaded at every launch and never
    * rewritten, so a corrupt row admitted once is shown for good. */
   if (r.t <= 0 || r.t >= WT_T_MAX)
      return;
   if (r.g < WT_MIN_G || r.g > WT_MAX_G)
      return;
   wt_push(&r);
}

void weight_load(void)
{
   g_nwt  = 0;
   int fd = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* Stream the whole file a line at a time (the insulin.c pattern): the tail
    * keeps only the last NWT rows, so memory stays bounded however many years
    * the file has grown. */
   char buf[1024];
   char line[96];
   int llen = 0;
   int over = 0; /* over-long line: skip, never parse a truncation */
   long n   = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (!over)
               wt_parse_line(line, line + llen);
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0 && !over) /* final line with no trailing newline */
      wt_parse_line(line, line + llen);
   close(fd);
   wt_sort();
}

int weight_append(long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   int fd = open(g_wt_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return -1;
   if (lseek(fd, 0, SEEK_END) == 0) { /* self-describing header when new */
      if (write(fd, g_wt_hdr, sizeof g_wt_hdr - 1) < 0) { /* best effort */
      }
   }
   char b[64];
   int n  = snprintf(b, sizeof b, "%ld,%ld,%ld\n", t, g, tz);
   n      = clampn(n, sizeof b);
   long w = write(fd, b, n);
   if (w != n) {
      /* Roll a partial line back so it cannot merge with the next append into
       * one unparseable row (the sensors.csv rule). */
      if (w > 0)
         (void)ftruncate(fd, lseek(fd, 0, SEEK_END) - w);
      close(fd);
      return -1;
   }
   close(fd);
   struct wt_rec r = {t, g};
   wt_push(&r);
   wt_sort();
   return 0;
}

/* Shared worker for update/delete: stream the file, copy every line to
 * <path>.tmp except the LAST row matching `orig`, which is replaced (or
 * skipped when del). Rewrite-and-rename so a crash never truncates the log.
 * The insulin.c original, with its two-field key. */
static int wt_rewrite(const struct wt_rec *orig, int del, long t, long g,
                      long tz)
{
   /* pass 1: how many rows match? (we edit the LAST one) */
   int fd = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char buf[1024];
   char line[256];
   int llen   = 0;
   int nmatch = 0;
   long n     = 0;
   struct wt_rec r;
   while ((n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (llen < (int)sizeof line &&
                wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.g == orig->g)
               nmatch++;
            llen = 0;
         } else if (llen < (int)sizeof line) {
            line[llen++] = buf[i];
         } else {
            llen = (int)sizeof line; /* over-long: cannot match */
         }
      }
   close(fd);
   if (nmatch == 0)
      return -1;

   /* pass 2: copy, altering only match #nmatch */
   char tmp[sizeof g_wt_path + 4];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_wt_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return -1;
   fd = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (out < 0) {
      close(fd);
      return -1;
   }
   int seen = 0;
   int ok   = 1;
   llen     = 0;
   while (ok && (n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; ok && i < n; i++) {
         if (buf[i] != '\n') {
            /* -1: the newline below is appended UNCONDITIONALLY, so the last
             * byte of `line` belongs to it (the insulin.c overflow). */
            if (llen < (int)sizeof line - 1)
               line[llen++] = buf[i];
            else
               ok = 0; /* over-long row: refuse to rewrite blind */
            continue;
         }
         int hit = wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                   r.g == orig->g && ++seen == nmatch;
         if (hit && del) {
            llen = 0;
            continue; /* the deleted row is simply not copied */
         }
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%ld,%ld", t, g, tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
         llen = 0;
      }
   /* a final line with no trailing newline gets the same treatment (the
    * appender always terminates rows, but the file is user-copyable) */
   if (ok && llen > 0 && llen < (int)sizeof line) {
      int hit = wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.g == orig->g && ++seen == nmatch;
      if (!(hit && del)) {
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%ld,%ld", t, g, tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
      }
   }
   close(fd);
   close(out);
   if (!ok || rename(tmp, g_wt_path) != 0) {
      (void)unlink(tmp);
      return -1;
   }
   weight_load(); /* the tail mirrors the file again */
   return 0;
}

int weight_update(const struct wt_rec *orig, long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   return wt_rewrite(orig, 0, t, g, tz);
}

int weight_delete(const struct wt_rec *orig)
{
   return wt_rewrite(orig, 1, 0, 0, 0);
}
