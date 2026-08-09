// SPDX-License-Identifier: GPL-3.0
// insulin.c --- Insulin dose log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* See insulin.h. Freestanding like sensors.c: no sscanf, hand parsers that
 * stop at the first field they cannot read, and every row is validated on the
 * way in -- a log that is loaded at every launch and never rewritten must not
 * be able to wedge the app or resurrect a corrupt row forever. */
#include "insulin.h"
#include "dexlibc.h"
#include "util.h"
#include <stdio.h> /* snprintf, SEEK_END */

struct ins_rec g_ins[NINS];
int g_nins;
char g_ins_path[256];

/* Column header, so an exported log is self-describing. tz_offset_s is the
 * offset assumed when the dose was entered, same rationale as readings.csv:
 * a bad local-time conversion stays repairable decades later. */
static const char g_ins_hdr[] = "# unix_time,type,units,tz_offset_s\n";

const char *insulin_type_name(int type)
{
   return type == INS_FAST ? "FAST" : "SLOW";
}

/* Decimal reader, same shape (and same digit cap rationale) as sensors.c. */
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
static void ins_push(const struct ins_rec *r)
{
   if (g_nins == NINS) {
      for (int i = 1; i < NINS; i++)
         g_ins[i - 1] = g_ins[i];
      g_nins--;
   }
   g_ins[g_nins++] = *r;
}

/* Keep the tail sorted by DOSE TIME (stable insertion sort; the tail is
 * tiny). FILE order is arrival order, and users legitimately backdate,
 * edit timestamps, and import history -- everything that displays or
 * evicts "oldest first" must mean oldest BY TIME, not by arrival. */
static void ins_sort(void)
{
   for (int i = 1; i < g_nins; i++) {
      struct ins_rec r = g_ins[i];
      int j            = i - 1;
      while (j >= 0 && g_ins[j].t > r.t) {
         g_ins[j + 1] = g_ins[j];
         j--;
      }
      g_ins[j + 1] = r;
   }
}

/* Parse one row into *r; 1 = a valid dose row. Validate EVERYTHING: a
 * header line parses as t == 0 and is rejected here, and a corrupt row
 * must not load as a plausible dose. The time bounds are generous -- a
 * dose is user-entered and may honestly be backdated or even slightly
 * future-dated -- but an absurd digit run, capped by rdnum at 18 digits,
 * must not slip through as a huge "positive epoch". */
static int ins_parse_rec(const char *p, const char *e, struct ins_rec *r)
{
   const char *q = p;
   r->t          = rdnum(&q, e);
   rdsep(&q, e);
   r->type = (int)rdnum(&q, e);
   rdsep(&q, e);
   r->units = (int)rdnum(&q, e);
   if (r->t <= 0 || r->t >= INS_T_MAX)
      return 0;
   if (r->type != INS_SLOW && r->type != INS_FAST)
      return 0;
   if (r->units < INS_UNITS_MIN || r->units > INS_UNITS_MAX)
      return 0;
   return 1;
}

static void ins_parse_line(const char *p, const char *e)
{
   struct ins_rec r;
   if (ins_parse_rec(p, e, &r))
      ins_push(&r);
}

void insulin_load(void)
{
   g_nins = 0;
   int fd = open(g_ins_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* Stream the whole file a line at a time (sensors.c pattern): the tail
    * buffer keeps only the last NINS rows, so memory stays bounded no matter
    * how many years the file has grown. */
   char buf[1024];
   char line[96];
   int llen = 0;
   int over = 0; /* over-long line: skip, never parse a truncation */
   long n   = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (!over)
               ins_parse_line(line, line + llen);
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
      ins_parse_line(line, line + llen);
   close(fd);
   ins_sort();
}

int insulin_append(long t, int type, int units, long tz)
{
   if (t <= 0 || t >= INS_T_MAX)
      return -1;
   if (type != INS_SLOW && type != INS_FAST)
      return -1;
   if (units < INS_UNITS_MIN || units > INS_UNITS_MAX)
      return -1;
   int fd = open(g_ins_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return -1;
   if (lseek(fd, 0, SEEK_END) == 0) { /* self-describing header when new */
      if (write(fd, g_ins_hdr, sizeof g_ins_hdr - 1) < 0) { /* best effort */
      }
   }
   char b[64];
   int n  = snprintf(b, sizeof b, "%ld,%d,%d,%ld\n", t, type, units, tz);
   n      = clampn(n, sizeof b);
   long w = write(fd, b, n);
   if (w != n) {
      /* Roll a partial line back so it cannot merge with the next append
       * into one unparseable row (the sensors.csv rule). */
      if (w > 0)
         (void)ftruncate(fd, lseek(fd, 0, SEEK_END) - w);
      close(fd);
      return -1;
   }
   close(fd);
   struct ins_rec r = {t, type, units};
   ins_push(&r);
   ins_sort(); /* a backdated dose files into place immediately */
   return 0;
}

/* Shared worker for update/delete: stream the file, copy every line to
 * <path>.tmp except the LAST row matching `orig`, which is replaced (or
 * skipped when del). Rewrite-and-rename so a crash never truncates the
 * log; the tail reloads from the new file. Matching is by CONTENT, not
 * position, so a stale tail index can never touch the wrong dose. */
static int ins_rewrite(const struct ins_rec *orig, int del, long t, int type,
                       int units, long tz)
{
   /* pass 1: how many rows match? (we edit the LAST one) */
   int fd = open(g_ins_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char buf[1024];
   char line[256];
   int llen   = 0;
   int nmatch = 0;
   long n     = 0;
   struct ins_rec r;
   while ((n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (llen < (int)sizeof line &&
                ins_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.type == orig->type && r.units == orig->units)
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
   char tmp[sizeof g_ins_path + 4];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_ins_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return -1;
   fd = open(g_ins_path, O_RDONLY, 0);
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
             * byte of `line` belongs to it. Filling to sizeof line here let a
             * row of exactly 256 characters run llen to 256 and then write
             * line[256] -- one byte past the array, with the following write()
             * shipping 257 bytes out of it. Pass 1 and the trailing-line
             * branch below both already reserve that byte. */
            if (llen < (int)sizeof line - 1)
               line[llen++] = buf[i];
            else
               ok = 0; /* over-long row: refuse to rewrite blind */
            continue;
         }
         int hit = ins_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                   r.type == orig->type && r.units == orig->units &&
                   ++seen == nmatch;
         if (hit && del) {
            llen = 0;
            continue; /* the deleted row is simply not copied */
         }
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%d,%d,%ld", t, type, units,
                            tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
         llen = 0;
      }
   /* a final line with no trailing newline gets the same treatment (the
    * appender always terminates rows, but the file is user-copyable) */
   if (ok && llen > 0 && llen < (int)sizeof line) {
      int hit = ins_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.type == orig->type && r.units == orig->units &&
                ++seen == nmatch;
      if (!(hit && del)) {
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%d,%d,%ld", t, type, units,
                            tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
      }
   }
   close(fd);
   close(out);
   if (!ok || rename(tmp, g_ins_path) != 0) {
      (void)unlink(tmp);
      return -1;
   }
   insulin_load(); /* the tail mirrors the file again */
   return 0;
}

int insulin_update(const struct ins_rec *orig, long t, int type, int units,
                   long tz)
{
   if (t <= 0 || t >= INS_T_MAX)
      return -1;
   if (type != INS_SLOW && type != INS_FAST)
      return -1;
   if (units < INS_UNITS_MIN || units > INS_UNITS_MAX)
      return -1;
   return ins_rewrite(orig, 0, t, type, units, tz);
}

int insulin_delete(const struct ins_rec *orig)
{
   return ins_rewrite(orig, 1, 0, 0, 0, 0);
}

int insulin_last_units(int type)
{
   /* The tail is time-sorted, so this is the units of the LATEST dose of
    * `type` -- the value the form pre-populates with. */
   for (int i = g_nins - 1; i >= 0; i--)
      if (g_ins[i].type == type)
         return g_ins[i].units;
   return 0;
}
