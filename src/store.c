// SPDX-License-Identifier: GPL-3.0
// store.c --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

/* The reading data model: an in-memory newest-first history (deduped, out-of-
 * order safe for backfill) plus an append-only CSV log. The UI reads this via
 * store.h; only store_append / hist_insert mutate g_hist, always under the
 * caller's hist_lock (see main.c) so a main-thread draw sees consistent data.
 */
#include "store.h"
#include "dexlibc.h"
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "util.h"
#include <limits.h> /* LONG_MAX: saturate over-long numeric fields */
#include <stdio.h>  /* snprintf, SEEK_SET / SEEK_END */
#include <string.h> /* memcpy: the chunked replay carries partial lines */

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

/* Read one comma-separated integer field and step past its separator. Sets
 * *present (when given) to whether the field held any digits at all, which is
 * how an empty rssi column and an absent v1 column are told apart. */
static long rdfield(char **p, const char *e, int *present)
{
   char *q = *p;
   long v  = 0;
   int neg = 0;
   int got = 0;
   if (q < e && *q == '-') {
      neg = 1;
      q++;
   }
   /* CAP THE DIGITS. Signed overflow is undefined behaviour, and it happens
    * here -- during parsing -- before any downstream range check can reject
    * the value, so the guards in store_load and stat_add cannot save us. This
    * file is ours, but a corrupted or hand-edited row is exactly what a parser
    * has to survive. 18 digits cannot overflow a 64-bit long; anything longer
    * is not a field this schema ever writes, so saturate and let the callers'
    * range checks discard the row. */
   int nd = 0;
   while (q < e && *q >= '0' && *q <= '9') {
      if (nd < 18) {
         v = (v * 10) + (*q - '0');
         nd++;
      } else {
         v = LONG_MAX / 2; /* unparseable: guaranteed to fail every bound */
      }
      q++;
      got = 1;
   }
   if (q < e && *q == ',')
      q++;
   *p = q;
   if (present)
      *present = got;
   return neg ? -v : v;
}

struct reading g_hist[NHIST];
int g_nhist;
int g_cur_glu = -1, g_cur_trend;
long g_cur_time;
int g_cur_rssi, g_cur_rssi_ok;
int g_stored;
char g_store_path[256];

/* Column header for readings.csv, so an exported log is self-describing. The
 * trailing `rescale` column is the multiplicative factor applied to this row's
 * glucose (e.g. 1.040 = +4%); empty means 1.000 (not rescaled). The stored
 * glucose is ALREADY rescaled, so the factor is what makes the raw recoverable
 * (raw = glucose / rescale) -- no data is lost. */
static const char g_store_hdr[] =
    "# unix_time,glucose_mgdl,trend,rssi,recv_lag_s,sensor_id,"
    "device_time,tz_offset_s,kind,rescale\n";

void hist_refresh_current(int prime)
{
   /* Only a CGM sample may become the current reading. g_hist[0] is the newest
    * across ALL sources, so taking it blindly would let a meter fingerstick
    * become the big number -- with a trend arrow, and feeding the alarm --
    * whenever the meter synced more recently than the sensor. See the
    * invariant in store.h and sensor_set_primary(). */
   /* `prime` is resolved by the caller under the registry lock, BEFORE it takes
    * hist_lock -- reading g_slot here would be both unsynchronized and a lock
    * order inversion. -1 means "no primary", which skips pass 0. */
   /* With a primary configured, ONLY its samples qualify -- that is the
    * contract of the setting. A primary with no data yet used to fall back to
    * whichever other CGM reported last, so a freshly paired sensor promoted to
    * primary kept showing the OLD sensor's value as the big number -- data the
    * user explicitly asked to stop headlining. No data is shown as no data.
    * Only with no primary at all (no CGM registered: a pre-registry install)
    * does the newest sample of any CGM source fill in. */
   for (int i = 0; i < g_nhist; i++) {
      if (g_hist[i].kind == KIND_BGM)
         continue;
      if (prime >= 0 && g_hist[i].src != (unsigned short)prime)
         continue;
      g_cur_glu   = g_hist[i].glu;
      g_cur_trend = g_hist[i].trend;
      g_cur_time  = g_hist[i].t;
      return;
   }
   /* Nothing qualified: CLEAR the current reading rather than leaving a stale
    * value latched. Without this, switching primary to a sensor with no data
    * left the previous sensor's number (and timestamp) on screen and feeding
    * the alarm indefinitely. */
   g_cur_glu   = -1;
   g_cur_trend = 0;
   g_cur_time  = 0;
}

int hist_have_point(long t, int glu)
{
   for (int i = 0; i < g_nhist; i++)
      if (g_hist[i].t == t && g_hist[i].glu == glu)
         return 1;
   return 0;
}

int hist_insert(long t, int glu, int trend, int src, int kind)
{
   /* CGM samples land on a ~5-min grid, so a nearby sample from the SAME
    * source is a restatement of one fact; a fingerstick is always its own
    * event and is only ever deduped on an exact timestamp match. */
   long window = (kind == KIND_BGM) ? 0 : 150;
   for (int i = 0; i < g_nhist; i++) {
      if (g_hist[i].src != (unsigned short)src)
         continue;
      /* Compare KIND too, or the invariant this header states -- that a BGM
       * fingerstick never dedups against a CGM sample -- is enforced only by
       * ids happening to be disjoint. They are not disjoint by construction:
       * id 0 is shared by legacy rows and any unregistered source. Without
       * this a CGM sample within the window would overwrite a fingerstick's
       * value in place, leave it flagged BGM, and return HIST_DUP so the CGM
       * sample was never persisted at all. */
      if (g_hist[i].kind != (unsigned char)kind)
         continue;
      long d = t - g_hist[i].t;
      if (d < 0)
         d = -d;
      if (d <= window) {
         /* Do NOT mutate. Callers persist only on a non-zero result, so an
          * in-place restatement updated memory while the append-only log kept
          * the original -- the two then disagreed about the same datapoint,
          * and after a restart the UI and the alarm silently reverted to the
          * older value. A restatement is either authoritative (and must be
          * logged) or it is not; treating the first sample as authoritative
          * keeps memory and disk identical, which is the property that makes
          * the log trustworthy. */
         return HIST_DUP;
      }
   }
   if (g_nhist == NHIST && t <= g_hist[NHIST - 1].t)
      return HIST_OLD; /* genuinely new, but off the end of the display window
                        */
   if (g_nhist < NHIST)
      g_nhist++;
   /* Insertion sort keeps g_hist newest-first, which is what makes the merged
    * multi-sensor history monotonic in memory even though the file stays in
    * arrival order. Ties break on source id so the order is total and stable
    * rather than dependent on which BLE thread got there first. */
   int i = g_nhist - 1;
   while (i > 0 &&
          (g_hist[i - 1].t < t ||
           (g_hist[i - 1].t == t && g_hist[i - 1].src > (unsigned short)src))) {
      g_hist[i] = g_hist[i - 1];
      i--;
   }
   g_hist[i].t     = t;
   g_hist[i].glu   = (short)glu;
   g_hist[i].trend = (short)trend;
   g_hist[i].src   = (unsigned short)src;
   g_hist[i].kind  = (unsigned char)kind;
   return HIST_NEW;
}

void store_append(long t, int glu, int trend, int rssi, int has_rssi, int src,
                  long raw, long tz, int kind, int rescale_pm)
{
   int fd = open(g_store_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return;
   /* One-line column header on a brand-new file, so an exported readings.csv is
    * self-describing. Only ever written when the file is empty (never prepended
    * to the existing append-only log); both loaders skip leading-'#' lines. */
   if (lseek(fd, 0, SEEK_END) == 0) {
      if (write(fd, g_store_hdr, sizeof g_store_hdr - 1) <
          0) { /* best effort */
      }
   }
   /* Where this row starts, so a short write can be undone. O_APPEND makes the
    * write itself atomic against other writers, but not against a full disk:
    * a partial row leaves no newline, so the NEXT append concatenates onto it
    * and the pair parses as one row of spliced fields -- a fabricated reading
    * at a fabricated time, indistinguishable from a real one forever after.
    * Truncating back is the only way to keep the log parseable. */
   long lag = realtime_s() - t; /* arrival delay from the datapoint stamp */
   char line[160];
   /* Rescale factor column: a decimal (1.040) when this glucose was rescaled,
    * empty when it was not (1.000). Loaders read positionally through `kind`
    * and ignore this trailing field, so it is pure provenance. */
   char rs[12];
   if (rescale_pm > 0 && rescale_pm != 1000)
      (void)snprintf(rs, sizeof rs, "%d.%03d", rescale_pm / 1000,
                     rescale_pm % 1000);
   else
      rs[0] = 0;
   /* The file is an ARRIVAL log, never re-sorted -- that is what keeps it
    * append-only and crash-safe. Monotonic order is a property of the sorted
    * in-memory view (see hist_insert), not of the bytes on disk. */
   int n =
       has_rssi
           ? snprintf(line, sizeof line, "%ld,%d,%d,%d,%ld,%d,%ld,%ld,%d,%s\n",
                      t, glu, trend, rssi, lag, src, raw, tz, kind, rs)
           : snprintf(line, sizeof line, "%ld,%d,%d,,%ld,%d,%ld,%ld,%d,%s\n", t,
                      glu, trend, lag, src, raw, tz, kind, rs);
   n      = clampn(n, sizeof line);
   long w = write(fd, line, n);
   if (w != n) {
      /* Roll back by exactly what WE wrote, measured from the end as it stands
       * now -- not to a start offset sampled before the write. With O_APPEND
       * another writer can append in between, and truncating to the stale
       * offset would delete that writer's COMPLETE row as well. Same pattern
       * as sensor_mint. */
      if (w > 0)
         ftruncate(fd, lseek(fd, 0, SEEK_END) - w);
      close(fd);
      LOGW("store: short write (%ld of %d), row rolled back", w, n);
      return;
   }
   close(fd);
   g_stored++;
}

int store_count(void)
{
   int fd = open(g_store_path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char b[4096];
   long n            = 0;
   int c             = 0;
   int at_line_start = 1;
   int comment       = 0; /* current line is a leading-'#' header/comment */
   while ((n = read(fd, b, sizeof b)) > 0)
      for (long i = 0; i < n; i++) {
         if (at_line_start) {
            comment       = (b[i] == '#');
            at_line_start = 0;
         }
         if (b[i] == '\n') {
            if (!comment)
               c++;
            at_line_start = 1;
         }
      }
   close(fd);
   return c;
}

static void store_load_chunk(char *buf);

void store_load(void)
{
   int fd = open(g_store_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* THE WHOLE FILE, not a tail.
    *
    * The log is in ARRIVAL order, which is not time order, so "the last N
    * bytes" is not "the newest N readings". Importing months of server
    * history appends thousands of OLD rows at the end -- after which the
    * tail held April while July sat further up the file, and a restart came
    * back with an empty plot and empty 1D/3D/7D statistics from a log that
    * was completely intact.
    *
    * hist_insert keeps the newest NHIST BY TIMESTAMP whatever order they
    * arrive in, so one pass over everything is both correct and simple. It
    * costs a parse of a file measured in megabytes, once, at startup. */
   static char buf[STORE_TAIL + 1];
   long cap   = (long)sizeof buf - 1;
   long carry = 0; /* bytes of a partial line held over */
   long n     = 0;
   while ((n = read(fd, buf + carry, cap - carry)) > 0 || carry > 0) {
      long have = carry + (n > 0 ? n : 0);
      buf[have] = 0;
      long last = have;
      if (n > 0) { /* trim to the last complete line */
         while (last > 0 && buf[last - 1] != '\n')
            last--;
         if (last == 0)
            last = have; /* one absurd line: process it anyway */
      }
      char keep[512];
      long nkeep = have - last;
      if (nkeep > (long)sizeof keep)
         nkeep = 0;
      if (nkeep > 0)
         memcpy(keep, buf + last, (size_t)nkeep);
      buf[last] = 0;
      store_load_chunk(buf);
      if (n <= 0)
         break;
      carry = nkeep;
      if (carry > 0)
         memcpy(buf, keep, (size_t)carry);
   }
   close(fd);
}

/* Parse one chunk of WHOLE lines from the log (see store_load). Chunks are
 * line-aligned by the caller, so there is no partial first line to skip. */
static void store_load_chunk(char *buf)
{
   char *p       = buf;
   long now      = realtime_s();
   long best_t   = 0;
   int best_rssi = 0;
   int best_ok   = 0; /* rssi of the newest row */
   while (*p) {
      if (*p == '#') { /* header / comment line */
         while (*p && *p != '\n')
            p++;
         if (*p == '\n')
            p++;
         continue;
      }
      char *e = p;
      while (*e && *e != '\n')
         e++;
      char *q = p;
      /* Fields are read positionally and a missing one yields present = 0, so
       * v1 rows (5 fields, pre-registry) still load: src and kind fall back to
       * 0, which is exactly "legacy Stelo, continuous". */
      int have_rssi = 0;
      long t        = rdfield(&q, e, 0);
      long glu      = rdfield(&q, e, 0);
      long tr       = rdfield(&q, e, 0);
      long rssi     = rdfield(&q, e, &have_rssi);
      (void)rdfield(&q, e, 0); /* recv_lag: diagnostics only */
      long src = rdfield(&q, e, 0);
      (void)rdfield(&q, e, 0); /* raw_time: kept for later re-conversion */
      (void)rdfield(&q, e, 0); /* tz_off:   ditto */
      long kind = rdfield(&q, e, 0);
      /* Same plausibility bound the live path applies (glucose_plausible in
       * main.c) and stat_load applies on replay. Without it, rows written
       * before that gate existed -- a 0 mg/dL warm-up sentinel, or a spliced
       * row from a pre-rollback partial write -- are re-admitted to g_hist on
       * every restart, become g_cur_glu, and fire a spurious LOW alarm on a
       * displayed value of 0. stat_load would meanwhile exclude the same row,
       * so the plot and the statistics would disagree about one file. */
      /* Bound the timestamp at BOTH ends, like the glucose above it. Gating
       * only at zero let any future-dated row -- from a bad meter clock, or a
       * spliced row from a pre-rollback partial write -- back into g_hist on
       * every restart of a file that is never rewritten, where it sorts to the
       * head and pins a phantom point to the right edge of every plot.
       * stat_add_at already rejects hour > now, so without this the plot and
       * the statistics disagreed about the same file. */
      if (t > 0 && t <= now + 3600 && glu >= 20 && glu <= 600) {
         hist_insert(t, (int)glu, (int)tr, (int)src, (int)kind);
         if (t >= best_t) {
            best_t    = t;
            best_rssi = (int)rssi;
            best_ok   = have_rssi;
         }
      }
      p = (*e == '\n') ? e + 1 : e;
   }
   hist_refresh_current(sensor_primary_id());
   if (best_ok) {
      g_cur_rssi    = best_rssi;
      g_cur_rssi_ok = 1;
   }
}
