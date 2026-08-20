// SPDX-License-Identifier: GPL-3.0
// stats.c --- Rolling glucose stats (time-in-range / average)
// Copyright 2026 Jakob Kastelic

/* Rolling stats via hourly buckets: O(1) per reading, O(days*24) to read a
 * window, ~35 KB for 90 days at 1-hour resolution. A TRUE rolling "last N days"
 * window (not calendar-aligned), so every column lines up with the plot. The
 * bucket ring is private to this module; the UI only sees stat_window(). */
#include "stats.h"
#include "clock.h"
#include "csvcur.h" /* the shared bounded CSV cursor; the schema stays here */
#include "dexlibc.h"
#include "ingest.h"  /* STORE_GLU_MIN/MAX: what a stored reading may be */
#include "sensors.h" /* KIND_BGM / KIND_CGM: fingersticks are excluded */
#include <stdio.h>   /* SEEK_SET / SEEK_END */
#include <stdlib.h>
#include <string.h> /* memcpy: the chunked replay carries partial lines */

struct hourbucket {
   int hour, count, in_range, sum;
};
static struct hourbucket
    g_hours[STAT_HOURS];   /* ring keyed by hour % STAT_HOURS */
static long g_stat_oldest; /* oldest reading time fed in */

/* A RING BEING FILLED, so the replay can build one that is not the live one.
 *
 * Everything below writes through this rather than at g_hours directly. The
 * live path passes the live ring; stat_reload_prepare passes a private one it
 * has just allocated, fills it with no lock held, and only then does
 * stat_reload_publish copy it over the live ring under the store lock. See
 * stats.h for why the parse cannot happen under that lock at all.
 *
 * `oldest` travels with the ring because it is part of the same answer:
 * stat_window_at measures REPORTABILITY from it, so publishing a rebuilt ring
 * while keeping the old oldest would let a 90-day figure be reported off a
 * week of restored data. */
struct stat_fill {
   struct hourbucket *ring;
   long oldest;
};

/* `now` is a parameter, not a call to the clock.
 *
 * Both of these decide what to do by comparing a reading's hour against the
 * CURRENT hour, so with the clock read internally there was no way to exercise
 * the boundaries -- and the boundaries are where the bug was: a reading older
 * than the ring ALIASES onto a live bucket and erases it. The public wrappers
 * below supply realtime_s(); the tests supply their own. */
static void bucket_add(struct stat_fill *d, long t, int glu, long now)
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
   int idx                 = hour % STAT_HOURS;
   struct hourbucket *ring = d->ring;
   if (ring[idx].hour != hour) { /* recycle a slot >= STAT_HOURS old */
      ring[idx].hour     = hour;
      ring[idx].count    = 0;
      ring[idx].in_range = 0;
      ring[idx].sum      = 0;
   }
   ring[idx].count++;
   ring[idx].sum += glu;
   /* 70-180 mg/dL is the international consensus "time in range" band (ADA /
    * ATTD, 2019), which is why it is these numbers and not the user's own
    * alarm thresholds -- a TIR figure is only comparable to a published one
    * if it uses the published band. It is deliberately NOT settable for the
    * same reason. The house style is to say where a clinical number came
    * from; this one had no comment at all. */
   if (glu >= TIR_LOW_MGDL && glu <= TIR_HIGH_MGDL)
      ring[idx].in_range++;
   if (!d->oldest || t < d->oldest)
      d->oldest = t;
}

/* THE LIVE RING'S fill target. Not a copy of any state: it POINTS at the two
 * live objects, so a write through it is a write to them. */
static struct stat_fill live_fill(void)
{
   struct stat_fill d = {g_hours, g_stat_oldest};
   return d;
}

void stat_add_at(long t, int glu, long now)
{
   struct stat_fill d = live_fill();
   bucket_add(&d, t, glu, now);
   g_stat_oldest = d.oldest;
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

/* Seed the stats from the last ~90 days of the log -- a bounded tail read, so
 * it stays O(90 days) regardless of how large the file has grown. */
static void stat_load_chunk(struct stat_fill *d, char *buf, long now);

/* Replay `readings_path` into `d`. ADDITIVE: it only ever adds rows, so the
 * ring it is handed must already be the state the caller wants to add to --
 * zeroed for a rebuild, the live one at startup.
 *
 * TAKES THE REGISTRY LOCK, through sensor_in_warmup below, and therefore must
 * never be called with the history lock held. That is the whole reason the
 * rebuild is two calls; see stats.h.
 *
 * Returns 1 when the source was seen WHOLE and 0 when a read failed partway.
 * A log that does not exist is 1: "there are no readings" is a complete
 * answer, and it is the same one store_load gives for a missing file. A read
 * that failed is not -- what the rest of the file said is unknown, so the
 * buckets are a prefix and not a summary. */
static int load_into(struct stat_fill *d, const char *readings_path)
{
   int fd = open(readings_path, O_RDONLY, 0);
   if (fd < 0)
      return 1; /* no log is not a failure; see store_load's ENOENT */
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
      return 0;
   }
   /* ONE `now` for the whole replay, read once here rather than per row.
    * Every guard in bucket_add compares a row's hour against the current
    * hour, so a load that straddles an hour boundary would judge its first
    * rows by one reference and its last by another -- and the boundary is
    * exactly where the aliasing bug lived. It also means a rebuild is
    * reproducible: the same file yields the same buckets. */
   long now = realtime_s();
   lseek(fd, 0, SEEK_SET);
   long carry = 0;
   long n     = 0;
   while ((n = read(fd, buf + carry, want - carry)) > 0 || carry > 0) {
      if (n <= 0 && carry > 0) {
         /* THE LAST BYTES OF THE FILE WITH NO NEWLINE AFTER THEM: an append
          * that did not finish -- power loss, or a full disk. The bytes may
          * parse perfectly and still be half a reading, a plausible glucose
          * at a plausible time that nobody measured, and this replay used to
          * count exactly that into TIR and the average. store_load refuses to
          * publish such a row for the same reason (see its `unterm`), so
          * counting it here put the statistics over a reading the history
          * does not hold -- the two disagreeing about one file, which every
          * other bound in this parser exists to prevent. */
         break;
      }
      long have = carry + (n > 0 ? n : 0);
      buf[have] = 0;
      long last = have;
      if (n > 0) { /* keep only whole lines; hold the remainder over */
         while (last > 0 && buf[last - 1] != '\n')
            last--;
         if (last == 0) {
            /* One line longer than the whole buffer: it cannot be a row this
             * file writes, and parsing it would parse a TRUNCATION of one --
             * a fabricated value at a fabricated time. Dropped, exactly as
             * store_load drops it. */
            buf[0] = 0;
            carry  = 0;
            continue;
         }
      }
      long nkeep = have - last;
      char keep[512];
      if (nkeep > (long)sizeof keep)
         nkeep = 0;
      if (nkeep > 0)
         memcpy(keep, buf + last, (unsigned long)nkeep);
      buf[last] = 0;
      stat_load_chunk(d, buf, now);
      if (n <= 0)
         break;
      carry = nkeep;
      if (carry > 0)
         memcpy(buf, keep, (unsigned long)carry);
   }
   close(fd);
   free(buf);
   return n >= 0;
}

void stat_load(const char *readings_path)
{
   /* STARTUP'S SEED, into the live ring. Correct only because it runs once,
    * before any other thread exists and into a ring BSS has already zeroed:
    * this call is additive, so a second one over live data would double-count
    * everything it read. A rebuild goes through the two calls below. */
   struct stat_fill d = live_fill();
   (void)load_into(&d, readings_path);
   g_stat_oldest = d.oldest;
}

/* THE PRIVATE RING A REBUILD IS BUILDING, between prepare and publish.
 *
 * NULL except during a rebuild. It is only ever touched by the sync worker
 * (pancra_logs_reload is called from syncjni_restore and nowhere else), so it
 * needs no lock of its own -- and it deliberately holds no reference to the
 * live ring, which is the property that lets a frame keep rendering the old
 * numbers, whole, for as long as the parse takes. */
static struct hourbucket *g_rebuild;
static long g_rebuild_oldest;

int stat_reload_prepare(const char *readings_path)
{
   /* A previous prepare that was never published. Nothing should produce one
    * -- prepare and publish are written as a pair -- but leaking 35 kB per
    * restore on a phone with a flaky link is not a failure mode worth
    * leaving reachable, and the newer parse is the one wanted anyway. */
   free(g_rebuild);
   g_rebuild        = 0;
   g_rebuild_oldest = 0;

   /* CALLOC, so the private ring starts in EXACTLY the state g_hours has in
    * BSS before the first launch. That is not merely "empty": the ring is
    * keyed by `hour % STAT_HOURS` and every slot carries the hour it holds,
    * so an all-zero slot claims hour 0 -- 1 January 1970 -- which no window a
    * live clock asks for can ever match. A rebuild therefore reaches
    * precisely the numbers a restart would, which is what the user is
    * comparing against: restarting the app was the workaround they were left
    * with. */
   struct hourbucket *fresh = calloc(STAT_HOURS, sizeof *fresh);
   if (!fresh)
      return 0; /* nothing prepared: the caller keeps the numbers it has */

   /* THE WHOLE LOG, not the in-memory history. g_hist holds ~7 days for the
    * plot while these buckets span ~91, so rebuilding from the history would
    * silently truncate every window past 7D to whatever the display buffer
    * happened to keep. The file is the record; the buckets summarise it.
    *
    * No lock is held here, and none may be: this reads the registry for the
    * per-sensor warm-up rule. See stats.h. */
   struct stat_fill d = {fresh, 0};
   if (!load_into(&d, readings_path)) {
      /* THE SOURCE WAS NOT SEEN WHOLE, so these buckets are a prefix of the
       * record rather than a summary of it. Publishing them would put a TIR
       * and an average on screen that describe part of a file, beside a
       * history store_load has (for the same reason) refused to replace --
       * the two numbers disagreeing again, which is the whole defect this
       * rebuild exists to remove. Nothing prepared; the caller keeps what it
       * has and says so. */
      free(fresh);
      return 0;
   }
   g_rebuild        = fresh;
   g_rebuild_oldest = d.oldest;
   return 1;
}

void stat_reload_publish(void)
{
   if (!g_rebuild)
      return; /* nothing was prepared, or it has already been published */
   /* ONE COPY AND ONE STORE, under the caller's store lock. The oldest goes
    * with the ring because it is part of the same answer -- stat_window_at
    * measures REPORTABILITY from it, so publishing rebuilt buckets while
    * keeping the old value would let a 90-day figure be reported off a week
    * of restored data. */
   memcpy(g_hours, g_rebuild, sizeof g_hours);
   g_stat_oldest = g_rebuild_oldest;
   free(g_rebuild);
   g_rebuild        = 0;
   g_rebuild_oldest = 0;
}

/* THE WIDEST PROVENANCE ID THAT CAN EXIST. struct reading stores `src` in
 * sixteen bits (store.h), so sensor_mint refuses to issue an id past 0xFFFF
 * rather than let 65536 alias legacy id 0. A wider number in this column
 * cannot name anything on this phone, so it resolves to no sensor -- which is
 * what an unattributed row already is here. Spelled out rather than shared
 * with sensors.c for the reason plotdata.c gives beside its own copy: that
 * file's constant is a MINTING rule and this one is a READING rule. */
#define STAT_SRC_MAX 0xFFFF

/* ONE ROW OF readings.csv -> (t, glu, src, kind), or 0 if the line is not a
 * complete reading.
 *
 * THROUGH csvcur.h, NOT FOUR MORE DIGIT LOOPS. This function used to be four
 * hand-rolled accumulators and a field-skipper that treated a missing
 * separator as end-of-row, and what it produced was a statistic computed from
 * a row the app does not have. `<epoch>,100junk` was read as a glucose of 100
 * -- the leading digits of a field that says something else entirely -- and
 * folded into TIR and the average; the same walk lost the source and kind
 * columns of any row with a contaminated field, so the warm-up and fingerstick
 * exclusions silently stopped applying to it. Statistics are the one display
 * with no per-row detail behind it: a wrong TIR looks exactly like a right
 * one, so the only defence is refusing to compute one from a row that is not
 * whole.
 *
 * SO THE WHOLE ROW IS REFUSED, and refused before anything is published --
 * out-params are written only on the way out. A field with more digits than
 * the cursor holds is not a value to be clamped: it describes a different row.
 * A field with trailing junk is not a value at all.
 *
 * WHAT IS DELIBERATELY NOT REFUSED is a SHORT row. v1 rows carry five fields
 * and legitimately end early, and the rssi column is written EMPTY when the
 * link never reported one -- so an absent column leaves its default standing.
 * That is CSV_FIELD_EMPTY, and it is distinct from "no digits where digits
 * were required", which is why the two columns this file depends on are read
 * with an explicit CSV_FIELD_OK test. Note the sense: CSV_FIELD_OK is 0, so
 * `if (!why)` reads as failure and means success -- every test here names the
 * enumerator. */
static int stat_row(const char *ln, const char *e, long *t, long *glu,
                    int *src, long *kind)
{
   struct csv_cur c;
   csv_open(&c, ln, e);
   /* A leading digit is what makes a line a datapoint at all: a blank line,
    * the column header and any prose fail here. Explicit rather than leaning
    * on CSV_FIELD_EMPTY, because csv_num also accepts a leading '-' and no
    * timestamp this file has ever written starts with one. */
   if (c.p >= c.e || *c.p < '0' || *c.p > '9')
      return 0;

   enum csv_field why;
   long tv = csv_num(&c, &why);
   if (why != CSV_FIELD_OK)
      return 0;
   if (!csv_sep(&c))
      return 0; /* `<epoch>100,...`: two columns run into one */
   long gv = csv_num(&c, &why);
   if (why != CSV_FIELD_OK)
      return 0; /* an empty glucose column is not a glucose of 0 */

   /* Nothing published yet; these are the row's answers, held back until the
    * whole row has been read and passed. */
   long srcv  = 0;
   long kindv = KIND_CGM;

   /* fields: t,glu,trend,rssi,lag,src,device_time,tz,kind,rescale
    *
    * THE LAST COLUMN IS A DECIMAL, and this loop used to reject the whole row
    * because of it. `rescale` is written as "0.830"; csv_num reads the 0 and
    * stops AT the '.', so the next csv_sep found a '.' where it wanted a ','
    * and returned 0 -- discarding a perfectly good reading.
    *
    * What that cost: every row written since a rescale factor was first
    * stored was dropped from the statistics. The plot was unaffected (it
    * parses only the columns it needs and never walks the tail), so the
    * screen showed a full trace beside a TIR computed from whatever the LIVE
    * path had added since launch -- an hour of data reported as "1D", which
    * read as 100% on a day that was really 90%. Measured on a real log:
    * 42,000 rows, 4,189 of them with a fractional rescale, and the 1D/3D/7D
    * columns wrong for as long as the app had been running.
    *
    * A fraction is SKIPPED, not parsed: no column this function reads is
    * fractional, and the integer part is the answer for any that ever were.
    * Skipping is also what keeps the row-shape check meaningful -- the
    * separator test below still fires on genuinely malformed rows. */
   int f = 2;
   while (!csv_at_end(&c)) {
      if (!csv_sep(&c))
         return 0; /* junk trailing a column, or fields run together */
      f++;
      long n = csv_num(&c, &why);
      if (why == CSV_FIELD_OVERFLOW)
         return 0; /* more digits than any column of this file can mean */
      /* A fractional part, if the column carries one. */
      if (c.p < c.e && *c.p == '.') {
         c.p++;
         while (c.p < c.e && *c.p >= '0' && *c.p <= '9')
            c.p++;
      }
      if (why == CSV_FIELD_EMPTY)
         continue; /* an absent column leaves its default standing */
      if (f == 6)
         srcv = n;
      else if (f == 9)
         kindv = n;
   }

   /* Both bounds are applied on the WIDE side, before anything narrows: an
    * id that cannot name a device on this phone resolves to 0, which is the
    * unattributed source every pre-registry row already carries, and the
    * warm-up rule below then fails open for it exactly as it does for those.
    * Dropping the reading instead would delete real glucose from TIR because
    * a decoration column was wrong. */
   *t    = tv;
   *glu  = gv;
   *src  = (srcv >= 0 && srcv <= STAT_SRC_MAX) ? (int)srcv : 0;
   *kind = kindv;
   return 1;
}

/* Parse one chunk of WHOLE lines (see load_into). */
static void stat_load_chunk(struct stat_fill *d, char *buf, long now)
{
   char *p = buf;
   while (*p) {
      char *e = p;
      while (*e && *e != '\n')
         e++;
      /* WHAT SAYS A ROW IS FINISHED IS THE NEWLINE. load_into hands over
       * whole lines only, so this cannot trigger today -- stating it here is
       * what makes "a torn final row is not a reading" a property of the
       * parser rather than of its one caller. */
      if (*e != '\n')
         return;
      long t    = 0;
      long glu  = 0;
      long kind = 0;
      int src   = 0;
      /* '#' is the header / a comment, and it is not damage. */
      if (*p != '#' && stat_row(p, e, &t, &glu, &src, &kind)) {
         /* This predicate MUST match the live path's exactly (see
          * glucose_plausible in reading.c). When replay used `glu > 0` while
          * the live path had no bound at all, an implausible sample was
          * counted before a restart and skipped after it, so TIR and average
          * silently changed value across a restart of the same log.
          *
          * Compared as longs, before anything narrows: a value that would
          * wrap into a plausible range on the way into an int must fail here,
          * not there.
          *
          * A meter fingerstick must NOT feed the stats: time-in-range is
          * time-weighted (each CGM sample stands for ~5 min) while a
          * fingerstick is an irregular point sample, and people test
          * precisely when they suspect a low or high -- so meter values skew
          * toward the extremes. The live path never calls stat_add for a BGM
          * either, and without this the numbers silently CHANGED across a
          * restart, when the log was replayed and the fingersticks counted.
          *
          * WARMUP is excluded for the same reason and by the same both-paths
          * rule: the value is uncalibrated, so counting it skews TIR and the
          * average. sensors_load() runs before stat_load (main.c), so the
          * activation this resolves against is already on hand. */
         if (t > 0 && glu >= STORE_GLU_MIN && glu <= STORE_GLU_MAX &&
             kind != KIND_BGM && !sensor_in_warmup(src, t))
            bucket_add(d, t, (int)glu, now);
      }
      p = e + 1;
   }
}
