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

/* Keep the newest NINS rows: on overflow drop the OLDEST, which is the only
 * safe choice for a chronological log (the file still has everything). */
static void ins_push(const struct ins_rec *r)
{
   if (g_nins == NINS) {
      for (int i = 1; i < NINS; i++)
         g_ins[i - 1] = g_ins[i];
      g_nins--;
   }
   g_ins[g_nins++] = *r;
}

static void ins_parse_line(const char *p, const char *e)
{
   struct ins_rec r;
   const char *q = p;
   r.t           = rdnum(&q, e);
   rdsep(&q, e);
   r.type = (int)rdnum(&q, e);
   rdsep(&q, e);
   r.units = (int)rdnum(&q, e);
   /* Validate EVERYTHING: a header line parses as t == 0 and is dropped here,
    * and a corrupt row must not load as a plausible dose. The time bounds are
    * generous -- a dose is user-entered and may honestly be backdated or even
    * slightly future-dated -- but an absurd digit run, capped by rdnum at 18
    * digits, must not slip through as a huge "positive epoch". */
   if (r.t <= 0 || r.t >= INS_T_MAX)
      return;
   if (r.type != INS_SLOW && r.type != INS_FAST)
      return;
   if (r.units < INS_UNITS_MIN || r.units > INS_UNITS_MAX)
      return;
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
   return 0;
}

int insulin_last_units(int type)
{
   /* Newest-last, and "last" means last ENTERED, not latest dose time: the
    * form pre-populates with what the user typed most recently, which is the
    * habit worth repeating even if they backdated it. */
   for (int i = g_nins - 1; i >= 0; i--)
      if (g_ins[i].type == type)
         return g_ins[i].units;
   return 0;
}
