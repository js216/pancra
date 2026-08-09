// SPDX-License-Identifier: GPL-3.0
// stats.c --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

/* Rolling stats via hourly buckets: O(1) per reading, O(days*24) to read a
 * window, ~35 KB for 90 days at 1-hour resolution. A TRUE rolling "last N days"
 * window (not calendar-aligned), so every column lines up with the plot. The
 * bucket ring is private to this module; the UI only sees stat_window(). */
#include "stats.h"
#include "dexlibc.h"
#include "sensors.h" /* KIND_BGM: fingersticks are excluded from the stats */
#include "store.h"   /* STORE_GLU_MIN/MAX: the writer's actual range */
#include "util.h"
#include <limits.h> /* LONG_MAX: saturate over-long numeric fields */
#include <stdio.h>  /* SEEK_SET / SEEK_END */
#include <stdlib.h>
#include <string.h> /* memcpy: the chunked replay carries partial lines */

struct hourbucket {
   int hour, count, in_range, sum;
};
static struct hourbucket
    g_hours[STAT_HOURS];   /* ring keyed by hour % STAT_HOURS */
static long g_stat_oldest; /* oldest reading time fed in */

/* `now` is a parameter, not a call to the clock.
 *
 * Both of these decide what to do by comparing a reading's hour against the
 * CURRENT hour, so with the clock read internally there was no way to exercise
 * the boundaries -- and the boundaries are where the bug was: a reading older
 * than the ring ALIASES onto a live bucket and erases it. The public wrappers
 * below supply realtime_s(); the tests supply their own. */
void stat_add_at(long t, int glu, long now)
{
   int hour = (int)(t / 3600);
   int nowh = (int)(now / 3600);
   /* Reject anything the ring cannot represent. The ring is keyed by
    * `hour % STAT_HOURS`, so a reading older than STAT_HOURS ALIASES onto a
    * live bucket -- and because the key does not match, the branch below
    * zeroes that bucket and re-dates it. A single months-old row (the meter's
    * first sync appends weeks of fingersticks at the END of the arrival log,
    * so stat_load replays them last) silently erased a whole hour of real CGM
    * readings from TIR and average, and did it again on every restart while
    * that row stayed in the tail.
    *
    * Future timestamps are refused for the same reason: a bad sensor clock
    * would otherwise evict a live bucket from the other direction. */
   /* t <= 0 is the memory-safety guard, not a plausibility one.
    *
    * The ring index is `hour % STAT_HOURS`, and C's % keeps the sign of the
    * dividend -- so a NEGATIVE hour indexes BEFORE g_hours and both the
    * comparison and the count++ below write outside the array. A negative hour
    * is reachable without anything exotic: t is realtime_s() - age, so a device
    * whose clock is unset (a dead RTC with no network reports ~1970) or whose
    * clock_gettime failed (realtime_s() then returns 0) yields t = -age. The
    * two bounds below only caught readings too OLD or in the future relative to
    * now; neither rejects a timestamp before the epoch.
    *
    * The test is on t, NOT on hour: C truncates division toward zero, so every
    * t in (-3600, 0) gives hour == 0 and would slip past an `hour < 0` check
    * while still dragging g_stat_oldest negative -- which makes every rolling
    * window claim to be covered, since reportability is measured from it. That
    * was the first version of this guard, and the tests caught it. */
   /* COMPARE IN long, AND REJECT A NEGATIVE HOUR EXPLICITLY.
    *
    * `hour` is an int narrowed from t/3600, so a large timestamp -- one the
    * 18-digit parser cap happily admits -- overflows it to a large NEGATIVE
    * value. The old guard then computed `nowh - hour` in int, which overflows
    * a second time and wraps NEGATIVE, so `>= STAT_HOURS` silently passed and
    * `hour % STAT_HOURS` indexed BEFORE g_hours: an out-of-bounds write.
    * Demonstrated at t = 7730941136400 (hour = -2147483647, index -2047).
    * One corrupt row in readings.csv -- a file that is never rewritten -- would
    * therefore corrupt memory at every launch, forever.
    *
    * Doing the age arithmetic in long cannot overflow for any t the parser can
    * produce, and the explicit hour < 0 test makes the index provably in
    * range rather than provable only via the age bound. */
   if (t <= 0 || hour < 0 || hour > nowh ||
       (long)nowh - (long)hour >= STAT_HOURS)
      return;
   int idx = hour % STAT_HOURS;
   if (g_hours[idx].hour != hour) { /* recycle a slot >= STAT_HOURS old */
      g_hours[idx].hour     = hour;
      g_hours[idx].count    = 0;
      g_hours[idx].in_range = 0;
      g_hours[idx].sum      = 0;
   }
   g_hours[idx].count++;
   g_hours[idx].sum += glu;
   if (glu >= 70 && glu <= 180)
      g_hours[idx].in_range++;
   if (!g_stat_oldest || t < g_stat_oldest)
      g_stat_oldest = t;
}

void stat_add(long t, int glu)
{
   stat_add_at(t, glu, realtime_s());
}

int stat_window_at(int days, int *tir, int *avg, long now)
{
   if (!g_stat_oldest || now - g_stat_oldest < ((long)days * 86400) - 3600)
      return 0;
   int nowh  = (int)(now / 3600);
   int hours = days * 24;
   long cnt  = 0;
   long inr  = 0;
   long sum  = 0;
   for (int h = 0; h < hours; h++) {
      int hour = nowh - h;
      /* Same signed-modulo hazard as stat_add_at. The early return above makes
       * nowh >= hours - 1 for any data the app itself recorded, so this is
       * unreachable there -- but it rests on a chain of reasoning about a
       * separate function's predicate, and being wrong costs an out-of-bounds
       * read. Bound it here where the index is formed. */
      if (hour < 0)
         break;
      int idx = hour % STAT_HOURS;
      if (g_hours[idx].hour == hour) {
         cnt += g_hours[idx].count;
         inr += g_hours[idx].in_range;
         sum += g_hours[idx].sum;
      }
   }
   if (cnt == 0)
      return 0;
   *tir = (int)(100 * inr / cnt);
   *avg = (int)(sum / cnt);
   return 1;
}

int stat_window(int days, int *tir, int *avg)
{
   return stat_window_at(days, tir, avg, realtime_s());
}

/* Step past `n` more comma-separated fields, stopping at end of line. A line
 * that ends early leaves p on its terminator, so an absent field reads as 0 --
 * which is how v1 rows (5 fields, pre-registry) still parse correctly. */
static char *skip_fields(char *p, int n)
{
   while (n-- > 0) {
      while (*p && *p != ',' && *p != '\n')
         p++;
      if (*p != ',')
         return p;
      p++;
   }
   return p;
}

/* Advance p past the current line (to the char after the next '\n', or to the
 * terminating NUL). Pure pointer walk (local copy; store.c has its own). */
static char *skip_line(char *p)
{
   while (*p && *p != '\n')
      p++;
   if (*p == '\n')
      p++;
   return p;
}

/* Seed the stats from the last ~90 days of the log -- a bounded tail read, so
 * it stays O(90 days) regardless of how large the file has grown. */
static void stat_load_chunk(char *buf);

void stat_load(const char *readings_path)
{
   int fd = open(readings_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* THE WHOLE FILE, streamed. Reading only a tail assumed the newest rows
    * sit at the end -- true of a pure arrival log, FALSE once months of
    * older history are imported and appended. When that happened the last
    * megabyte held April, so the 1D/3D/7D columns came back empty from a
    * log that contained every reading. Buckets are keyed by age, not by
    * position, so order does not matter; completeness does. */
   long want = 256L * 1024; /* read buffer, not a limit */
   char *buf = malloc((unsigned long)want + 1);
   if (!buf) {
      close(fd);
      return;
   }
   lseek(fd, 0, SEEK_SET);
   long carry = 0;
   long n     = 0;
   while ((n = read(fd, buf + carry, want - carry)) > 0 || carry > 0) {
      long have = carry + (n > 0 ? n : 0);
      buf[have] = 0;
      long last = have;
      if (n > 0) { /* keep only whole lines; hold the remainder over */
         while (last > 0 && buf[last - 1] != '\n')
            last--;
         if (last == 0)
            last = have;
      }
      long nkeep = have - last;
      char keep[512];
      if (nkeep > (long)sizeof keep)
         nkeep = 0;
      if (nkeep > 0)
         memcpy(keep, buf + last, (unsigned long)nkeep);
      buf[last] = 0;
      stat_load_chunk(buf);
      if (n <= 0)
         break;
      carry = nkeep;
      if (carry > 0)
         memcpy(buf, keep, (unsigned long)carry);
   }
   close(fd);
   free(buf);
}

/* Parse one chunk of WHOLE lines (see stat_load). */
static void stat_load_chunk(char *buf)
{
   char *p = buf;
   while (*p) {
      if (*p == '#') { /* header / comment line */
         p = skip_line(p);
         continue;
      }
      long t  = 0;
      int glu = 0;
      char *q = p;
      /* Digit-capped: unbounded signed accumulation is UB, and it occurs
       * during parsing, before stat_add_at's range guards can reject anything.
       * A saturated value fails those guards, which is the wanted outcome. */
      int nd = 0;
      while (*q >= '0' && *q <= '9') {
         if (nd < 18) {
            t = (t * 10) + (*q - '0');
            nd++;
         } else {
            t = LONG_MAX / 2;
         }
         q++;
      }
      if (*q == ',')
         q++;
      nd = 0;
      while (*q >= '0' && *q <= '9') {
         if (nd < 9) {
            glu = (glu * 10) + (*q - '0');
            nd++;
         }
         q++;
      }
      /* kind is field 9; q sits on the separator after field 2, so seven more
       * separators reach it. A meter fingerstick must NOT feed the stats:
       * time-in-range is time-weighted (each CGM sample stands for ~5 min)
       * while a fingerstick is an irregular point sample, and people test
       * precisely when they suspect a low or high -- so meter values skew
       * toward the extremes. The live path never calls stat_add for a BGM
       * either, and without this the numbers silently CHANGED across a
       * restart, when the log was replayed and the fingersticks counted. */
      int kind = 0;
      char *k  = skip_fields(q, 7);
      /* Digit-capped like the two loops above. This one was missed: it is the
       * only uncapped accumulation left in stat_load, and a wrapped result can
       * land on KIND_BGM, silently excluding a real CGM row from TIR and the
       * average on every restart. Signed overflow is UB and happens during
       * parsing, before any check can reject the value. */
      int knd = 0;
      while (*k >= '0' && *k <= '9') {
         if (knd < 9)
            kind = (kind * 10) + (*k - '0');
         knd++;
         k++;
      }
      /* source_id is field 6, four separators on from where q sits (the one
       * after field 2) -- the same walk `kind` makes to field 9. Needed here
       * for the WARMUP rule below, which is per-sensor. Digit-capped like
       * every other accumulation in this parser: a wrapped id would resolve to
       * the wrong sensor's activation, or to none at all. */
      int src = 0;
      char *s = skip_fields(q, 4);
      int sd  = 0;
      while (*s >= '0' && *s <= '9') {
         if (sd < 9)
            src = (src * 10) + (*s - '0');
         sd++;
         s++;
      }
      /* This predicate MUST match the live path's exactly (see
       * glucose_plausible in main.c). When replay used `glu > 0` while the
       * live path had no bound at all, an implausible sample was counted
       * before a restart and skipped after it, so TIR and average silently
       * changed value across a restart of the same log. */
      /* WARMUP is excluded for the same reason BGM is, and by the same
       * both-paths rule: the value is uncalibrated, so counting it skews TIR
       * and the average. sensors_load() runs before stat_load (main.c), so the
       * activation this resolves against is already on hand. */
      if (t > 0 && glu >= STORE_GLU_MIN && glu <= STORE_GLU_MAX &&
          kind != KIND_BGM && !sensor_in_warmup(src, t))
         stat_add(t, glu);
      p = skip_line(p);
   }
}
