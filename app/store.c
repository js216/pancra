// SPDX-License-Identifier: GPL-3.0
// store.c --- Reading history + append-only persistent log (data model)
// Copyright 2026 Jakob Kastelic

/* The reading data model: an in-memory newest-first history (deduped, out-of-
 * order safe for backfill) plus an append-only CSV log. The UI reads this via
 * store.h; only store_append / hist_insert mutate g_hist, always under the
 * history lock -- which lives HERE now, with the data it protects, rather
 * than as a flag in the shell taken by name from another translation unit.
 *
 * store_record is the whole of recording a reading: history, statistics,
 * current value and disk, in the one order those are allowed to happen in.
 * See store.h for what each of those orderings costs when a caller gets it
 * wrong, which three of them independently did.
 */
#include "store.h"
#include "alarmlogic.h" /* AL_FRESH_S: one definition of "stale" */
#include "clock.h"
#include "dexlibc.h"
#include "ingest.h"  /* STORE_GLU_MIN/MAX: what a stored reading may be */
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "stats.h"   /* stat_add: recording a reading feeds the statistics */
#include "thread.h"  /* the history lock lives with the history */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* store_load tells a missing log from an unreadable one */
#endif
#include <limits.h> /* LONG_MAX: saturate over-long numeric fields */
#include <stdio.h>  /* snprintf, SEEK_SET / SEEK_END */
#include <stdlib.h> /* calloc/free: a reload stages a whole new history */
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

/* THE HISTORY TABLE AS A VALUE, so a reload can build one that is not the
 * live one.
 *
 * WHAT WENT WRONG WITHOUT THIS. store_load INSERTS; it does not replace. That
 * is right at startup, where it fills a table that is empty by construction,
 * and wrong the moment it is used to reload a log that has been REWRITTEN
 * underneath the app -- which is exactly what a cloud restore does. After a
 * restore the in-memory history was the UNION of what was there before and
 * what came back:
 *
 *   - a reading the user had deleted before restoring was still on the plot
 *     and in the history list, because nothing removes rows from g_hist;
 *   - and where the restored file corrected a value, hist_insert's dedup saw
 *     a sample from the same source within 150 s, declined it as a
 *     restatement (which is the right rule for a live arrival) and KEPT THE
 *     OLD ONE. The screen went on showing a number the restored record does
 *     not contain, until the app was restarted.
 *
 * Both survive as long as the process does, and neither is visible as an
 * error: the restored rows really did appear, so the restore looks like it
 * worked. Rebuilding the rolling statistics from that table -- which the
 * restore now does -- would have produced TIR and average figures matching
 * neither the file nor anything else.
 *
 * So a reload STAGES a whole new table, and swaps it in only once the entire
 * source has been read. `v` points at NHIST entries; `n` is how many are
 * live. The live pair is still two file-statics, because the crash handler
 * holds &g_nhist directly (see hist_count_ptr) and must not chase a pointer
 * on a signal stack. */
struct hist_tab {
   struct reading *v;
   int n;
};

static struct reading g_hist[NHIST];
static int g_nhist;

/* The LIVE table, as a hist_tab. It POINTS at g_hist rather than copying it,
 * so a write through it is a write to the real thing; `n` is a copy and the
 * caller writes it back. */
static struct hist_tab live_hist(void)
{
   struct hist_tab h = {g_hist, g_nhist};
   return h;
}

int hist_count(void)
{
   return g_nhist;
}

void hist_clear(void)
{
   store_lock();
   g_nhist = 0;
   store_unlock();
}

struct reading hist_at(int i)
{
   struct reading z = {0};
   if (i < 0 || i >= g_nhist)
      return z;
   return g_hist[i];
}

int hist_copy(struct reading *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   store_lock();
   for (; n < cap && n < g_nhist; n++)
      out[n] = g_hist[n];
   store_unlock();
   return n;
}

static int g_cur_glu = -1, g_cur_trend;
static long g_cur_time;
static int g_cur_rssi, g_cur_rssi_ok;

struct reading_rssi store_rssi_locked(void)
{
   struct reading_rssi r;
   r.dbm = g_cur_rssi;
   r.ok  = g_cur_rssi_ok;
   return r;
}

struct reading_rssi store_rssi(void)
{
   struct reading_rssi r;
   store_lock();
   r = store_rssi_locked();
   store_unlock();
   return r;
}

void store_note_rssi(int dbm)
{
   store_lock();
   g_cur_rssi    = dbm;
   g_cur_rssi_ok = 1;
   store_unlock();
}

static int g_stored;

int store_appended(void)
{
   return g_stored;
}

/* POINTERS, for the crash handler ONLY.
 *
 * It runs on a signal stack after the process has already gone wrong: it may
 * not lock, may not allocate, and may not call back into a module whose state
 * it is trying to describe. So it is given the addresses once, at startup,
 * and reads them directly -- a torn read there is acceptable, because the
 * alternative is no context at all in the report. Nothing else may use
 * these. */
const int *store_glu_ptr(void)
{
   return &g_cur_glu;
}

const int *hist_count_ptr(void)
{
   return &g_nhist;
}

static char g_store_path[256];

/* Column header for readings.csv, so an exported log is self-describing. The
 * trailing `rescale` column is the multiplicative factor applied to this row's
 * glucose (e.g. 1.040 = +4%); empty means 1.000 (not rescaled). The stored
 * glucose is ALREADY rescaled, so the factor is what makes the raw recoverable
 * (raw = glucose / rescale) -- no data is lost. */
static const char g_store_hdr[] =
    "# unix_time,glucose_mgdl,trend,rssi,recv_lag_s,sensor_id,"
    "device_time,tz_offset_s,kind,rescale\n";

/* ONE DEFINITION OF "THE CURRENT READING IS STALE", used by both forms below.
 *
 * data_fresh, not `now - r.t > AL_FRESH_S`. Both are realtime -- g_cur_time is
 * the reading's own timestamp, which is its identity in the log and the time
 * shown next to it on screen, so it stays realtime and must not become
 * monotonic. What changed is that a NEGATIVE age no longer counts as fresh.
 *
 * A clock rollback (a timezone fix, an NTP step, a date set by hand) leaves
 * every stored reading dated in the future. The old test then reported the
 * last known value as current indefinitely: the big number stopped ageing and
 * kept asserting, say, 58 mg/dL from three hours ago as the reading right
 * now. That is worse than blanking, because the person deciding whether to
 * treat a low has no way to tell a live number from a frozen one -- the
 * screen looks exactly the same.
 *
 * It also has to agree with alarm_zone, which now refuses the same stamps: a
 * screen showing a live value beside an alarm that has decided there is no
 * current reading is two answers to one question. */
static int cur_stale(int glu, long t, long now)
{
   return (glu < 0) || !data_fresh(now, t, AL_FRESH_S);
}

struct reading_now store_now_locked(long now)
{
   struct reading_now r;
   r.glu   = g_cur_glu;
   r.trend = g_cur_trend;
   r.t     = g_cur_time;
   r.stale = cur_stale(r.glu, r.t, now);
   return r;
}

struct reading_now store_now(long now)
{
   struct reading_now r;
   store_lock();
   r.glu   = g_cur_glu;
   r.trend = g_cur_trend;
   r.t     = g_cur_time;
   store_unlock();
   r.stale = cur_stale(r.glu, r.t, now);
   return r;
}

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

static int tab_insert(struct hist_tab *h, long t, int glu, int trend, int src,
                      int kind)
{
   /* NORMALISE THE KIND FIRST. Two callers pass a value PARSED FROM A FILE
    * -- the readings.csv loader and the import path -- and neither bounds
    * it, though both bound the timestamp and the glucose beside it with
    * comments explaining why. The kind was simply cast through.
    *
    * A row carrying anything but 0 or 1 (the log is append-only and never
    * rewritten, and the loader's own comments cite "a spliced row from a
    * pre-rollback partial write" twice as a real occurrence) then breaks
    * four things at once, permanently, on every launch:
    *   - build_model copies this byte straight into the plot model, and
    *     the renderer draws kind == KIND_INS along the bottom edge -- so a 2
    * renders as a phantom INSULIN DOSE on the glucose plot;
    *   - it is not KIND_BGM, so stats count it toward TIR, AVG and A1C;
    *   - the watchdogs read it as a live CGM sample and hold off a
    *     reconnect;
    *   - and the dedup below compares kind for equality, so it will not
    *     dedup against the real row it duplicates.
    * KIND_BGM or KIND_CGM, nothing else, which is exactly what store.h
    * declares this field to be. One place, so both parsing callers are
    * covered. */
   kind = (kind == KIND_BGM) ? KIND_BGM : KIND_CGM;
   /* CGM samples land on a ~5-min grid, so a nearby sample from the SAME
    * source is a restatement of one fact; a fingerstick is always its own
    * event and is only ever deduped on an exact timestamp match. */
   long window = (kind == KIND_BGM) ? 0 : 150;
   for (int i = 0; i < h->n; i++) {
      if (h->v[i].src != (unsigned short)src)
         continue;
      /* Compare KIND too, or the invariant this header states -- that a BGM
       * fingerstick never dedups against a CGM sample -- is enforced only by
       * ids happening to be disjoint. They are not disjoint by construction:
       * id 0 is shared by legacy rows and any unregistered source. Without
       * this a CGM sample within the window would overwrite a fingerstick's
       * value in place, leave it flagged BGM, and return HIST_DUP so the CGM
       * sample was never persisted at all. */
      if (h->v[i].kind != (unsigned char)kind)
         continue;
      long d = t - h->v[i].t;
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
   if (h->n == NHIST && t <= h->v[NHIST - 1].t)
      return HIST_OLD; /* genuinely new, but off the end of the display window
                        */
   if (h->n < NHIST)
      h->n++;
   /* Insertion sort keeps g_hist newest-first, which is what makes the merged
    * multi-sensor history monotonic in memory even though the file stays in
    * arrival order. Ties break on source id so the order is total and stable
    * rather than dependent on which BLE thread got there first. */
   int i = h->n - 1;
   while (i > 0 &&
          (h->v[i - 1].t < t ||
           (h->v[i - 1].t == t && h->v[i - 1].src > (unsigned short)src))) {
      h->v[i] = h->v[i - 1];
      i--;
   }
   h->v[i].t     = t;
   h->v[i].glu   = (short)glu;
   h->v[i].trend = (short)trend;
   h->v[i].src   = (unsigned short)src;
   h->v[i].kind  = (unsigned char)kind;
   return HIST_NEW;
}

int hist_insert(long t, int glu, int trend, int src, int kind)
{
   /* THE LIVE TABLE. Every rule above is the same for a staged one; the only
    * difference is which array it lands in. */
   struct hist_tab h = live_hist();
   int r             = tab_insert(&h, t, glu, trend, src, kind);
   g_nhist           = h.n;
   return r;
}

int hist_prev_glu(long t, int src, long not_before)
{
   /* g_hist is newest-first, so the first match older than `t` is the one
    * wanted. KIND_BGM is skipped: a fingerstick is a different instrument
    * with its own offset, and the chirp describes the CGM's own movement. */
   for (int i = 0; i < g_nhist; i++) {
      if (g_hist[i].src != (unsigned short)src)
         continue;
      if (g_hist[i].kind == KIND_BGM)
         continue;
      if (g_hist[i].t < t)
         /* Inside the window it is the previous READING; older than that it
          * is the far side of a GAP, and the difference across a gap is not a
          * rate. Returning -1 there is what keeps the chirp honest. */
         return (g_hist[i].t >= not_before) ? g_hist[i].glu : -1;
   }
   return -1;
}

/* 0 on success, -1 if the row did NOT reach the disk.
 *
 * It used to return void and swallow the open() failure outright: a reading
 * that could not be written was displayed, alarmed on, counted in
 * time-in-range and then simply absent after a restart, with nothing said to
 * anyone. app/insulin.c has always refused visibly for the same class of
 * failure -- "a dose the user believes recorded but is not would corrupt
 * every judgement made on top of the log" -- and a glucose reading is the
 * same argument with more of the log resting on it. The short-write path
 * already rolled back and logged; only the caller could not be told. */
int store_append(long t, int glu, int trend, int rssi, int has_rssi, int src,
                 long raw, long tz, int kind, int rescale_pm)
{
   /* THE HEADER AND THE FIRST ROW TOGETHER, and the whole append in one
    * operation (see log_append). This was open / lseek / write the header /
    * write the row / close, spelled out here and in three other files, with
    * the same two holes in each: a first record that was not atomic (a
    * failure after the row reached the file made the caller's retry write it
    * twice) and a failed rollback reported as a clean failure. */
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
   int rc = log_append(g_store_path, g_store_hdr, (int)sizeof g_store_hdr - 1,
                       line, n);
   if (rc == LOG_DAMAGED) {
      /* A PARTIAL ROW IS STILL IN THE FILE. Distinct from "nothing was
       * written": the next append would splice onto it, and the log now needs
       * saying so rather than being retried blindly. */
      LOGW("store: a partial row could NOT be rolled back; %s is damaged",
           g_store_path);
      return rc;
   }
   if (rc != LOG_OK) {
      LOGW("store: the reading was not written");
      return rc;
   }
   g_stored++;
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
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

/* What a load is building, carried across chunks. */
struct load_ctx {
   struct hist_tab tab;
   long best_t;   /* timestamp of the newest row seen anywhere in the file */
   int best_rssi; /* ...and the link RSSI recorded with it */
   int best_ok;
};

static int store_load_chunk(struct load_ctx *c, char *buf);

int store_load(int prime)
{
   int fd = open(g_store_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   /* STAGE, VALIDATE, SWAP -- and the staging is the point.
    *
    * This function used to insert straight into the live table, which is
    * correct exactly once: at startup, into a table that is empty by
    * construction. Used to RELOAD a log that has been rewritten underneath
    * the app -- which is what a cloud restore does -- inserting produced the
    * UNION of the old table and the restored file. See struct hist_tab for
    * the two ways that showed on the screen; both lasted until the next
    * restart, and neither looked like an error.
    *
    * ~79 kB (NHIST x 16 B), and while it is held the phone has TWO histories
    * in memory. That is the price of never being between them: the previous
    * table stays whole and answerable -- the frame builder and the alarm both
    * read it under the same lock this runs under -- right up to the single
    * memcpy that replaces it. It is freed before this function returns, so
    * the cost is the duration of one load and not a permanent doubling.
    * malloc rather than a second static array for the same reason. */
   struct reading *stage = calloc(NHIST, sizeof *stage);
   if (!stage) {
      /* NOTHING IS TOUCHED. The old table, the old current reading and the
       * old row count all still describe the log the app was showing a moment
       * ago, which is the whole contract of the failure path: a restore that
       * leaves the user with neither their previous history nor the restored
       * one is worse than the bug this replaces. */
      close(fd);
      return -1;
   }
   struct load_ctx ctx = {
       {stage, 0},
       0, 0, 0
   };
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
   long cap    = (long)sizeof buf - 1;
   long carry  = 0; /* bytes of a partial line held over */
   long n      = 0;
   int damaged = 0;
   int unterm  = 0; /* the file ended without a newline */
   while ((n = read(fd, buf + carry, cap - carry)) > 0 || carry > 0) {
      if (n <= 0 && carry > 0) {
         /* THE LAST BYTES OF THE FILE WITH NO NEWLINE AFTER THEM -- what is
          * left over once every complete line has been processed. The file is
          * append-only and every finished row ends in a newline, so this is a
          * write that did not finish: power loss, or a full disk mid-append.
          * The bytes may parse perfectly and still be half a reading -- a
          * plausible glucose at a plausible time that nobody measured -- so
          * they are NOT published. They used to be, with the load then
          * returning success, which is the one combination that puts an
          * invented reading into the plot and the alarms. */
         unterm = 1;
         break;
      }
      long have = carry + (n > 0 ? n : 0);
      buf[have] = 0;
      long last = have;
      if (n > 0) { /* trim to the last complete line */
         while (last > 0 && buf[last - 1] != '\n')
            last--;
         if (last == 0) {
            /* One line longer than the whole buffer: it cannot be a reading,
             * and parsing it would parse a TRUNCATION of one -- a fabricated
             * value at a fabricated time. Drop it and say so. */
            damaged = 1;
            buf[0]  = 0;
            carry   = 0;
            continue;
         }
      }
      char keep[512];
      long nkeep = have - last;
      if (nkeep > (long)sizeof keep)
         nkeep = 0;
      if (nkeep > 0)
         memcpy(keep, buf + last, (size_t)nkeep);
      buf[last] = 0;
      if (!store_load_chunk(&ctx, buf))
         damaged = 1;
      if (n <= 0)
         break;
      carry = nkeep;
      if (carry > 0)
         memcpy(buf, keep, (size_t)carry);
   }
   close(fd);
   if (unterm)
      damaged = 1;

   /* THE SOURCE WAS NEVER SEEN WHOLE. A read() that FAILED is not end of
    * file: we do not know what the rest of the log said, so the staged table
    * is not a table, it is a prefix. Throw it away and leave everything as it
    * was -- history, current reading and row count all still describe the log
    * the app was showing, and the caller is told the load failed.
    *
    * A DAMAGED ROW IS NOT THIS, and the distinction is the one a later reader
    * will get wrong. `damaged` means individual rows were rejected -- a line
    * that does not parse, a glucose or a time no reading can have, a final
    * write cut short by a power loss. The whole file was still read, so the
    * staged table IS the record minus rows that were never readings, and the
    * user keeps everything that survived. Discarding a person's entire
    * history because one row of an append-only log got spliced would be a far
    * worse answer than the understatement the -1 already reports. */
   if (n < 0) {
      free(stage);
      return -1;
   }

   /* THE SWAP. One memcpy and one store, with the history lock already held
    * by the caller (see store.h): nothing can observe a table that is half
    * this load and half the last one. Only `ctx.tab.n` entries are live, so
    * only they are copied -- the rest of g_hist is unreachable through
    * hist_at/hist_copy, both of which bound on g_nhist. */
   memcpy(g_hist, stage, (size_t)ctx.tab.n * sizeof *g_hist);
   g_nhist = ctx.tab.n;
   free(stage);

   /* PUBLISHED AFTER THE SWAP, both of them. hist_refresh_current derives the
    * big number from g_hist, so running it per chunk (as it used to) bound the
    * current reading to a table that was still being built. */
   hist_refresh_current(prime);
   if (ctx.best_ok) {
      g_cur_rssi    = ctx.best_rssi;
      g_cur_rssi_ok = 1;
   }
   /* THE ROW COUNT IS THE FILE'S, not the tail's: the settings screen reports
    * how many readings are stored, and the tail holds only the last NHIST of
    * them. Counted here rather than by the caller, which had to know to do it
    * and which counter it was setting. */
   g_stored = store_count();
   /* What is lost when this returns -1 is not a file -- it is history: g_hist
    * drives the plot, the time-in-range figures and (through
    * g_cur_glu/g_cur_time) the alarm's idea of the current reading. Coming up
    * with a silently short record looks exactly like a working app that has
    * simply not seen much, so the caller must say so rather than render a
    * partial record as a complete one. A never-written log is still fine: the
    * open above answers that case with 0. */
   return damaged ? -1 : 0;
}

/* Parse one chunk of WHOLE lines from the log (see store_load). Chunks are
 * line-aligned by the caller, so there is no partial first line to skip. */
/* Returns 1 when every row in the chunk was taken or was a comment, 0 when
 * one was REJECTED -- a row that does not parse, or holds a glucose or a time
 * no reading can have. Skipping it is right; reporting success afterwards is
 * not, because this log is the record and it is never rewritten. */
static int store_load_chunk(struct load_ctx *c, char *buf)
{
   int damaged = 0;
   char *p     = buf;
   long now    = realtime_s();
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
      /* now, not now + 3600: stat_add_at rejects any hour past now, so a
       * row in that one-hour margin entered the plot and not the statistics
       * -- the two disagreeing about one file, which is exactly what the
       * bound above it was widened to prevent. */
      if (t > 0 && t <= now && glu >= STORE_GLU_MIN && glu <= STORE_GLU_MAX) {
         /* INTO THE STAGED TABLE, never the live one: nothing this function
          * does may be visible until the whole source has been read. */
         tab_insert(&c->tab, t, (int)glu, (int)tr, (int)src, (int)kind);
         /* THE NEWEST ROW IN THE WHOLE FILE, not in this chunk. best_t used
          * to be a local reset on every chunk, so the last chunk that
          * happened to carry an RSSI won -- and the log is in ARRIVAL order,
          * so that is not the newest row. Carried in the context, it means
          * what it says. */
         if (t >= c->best_t) {
            c->best_t    = t;
            c->best_rssi = (int)rssi;
            c->best_ok   = have_rssi;
         }
      } else if (e != p) {
         damaged = 1; /* a line that is not a reading */
      }
      p = (*e == '\n') ? e + 1 : e;
   }
   /* NO hist_refresh_current AND NO g_cur_rssi HERE. Both publish, and this
    * function is halfway through reading a file; store_load does them once,
    * after the swap. */
   return !damaged;
}

/* ---- the history lock ---- */

static struct mutex g_hist_lk = MUTEX_INIT;

void store_lock(void)
{
   mutex_lock(&g_hist_lk);
}

void store_unlock(void)
{
   mutex_unlock(&g_hist_lk);
}

int store_trylock(void)
{
   return mutex_trylock(&g_hist_lk);
}

int store_drain(int ms)
{
   return mutex_drain(&g_hist_lk, ms);
}

/* ---- recording one reading ---- */

struct reading_result store_record(const struct reading_event *ev, long gap)
{
   struct reading_result r = {HIST_DUP, 0, -1};
   if (!ev)
      return r;

   store_lock();
   /* BEFORE the insert: afterwards the newest sample from this source is the
    * one being added, and prev_glu would report the reading against itself. */
   r.prev_glu = hist_prev_glu(ev->t, ev->src, ev->t - gap);
   r.inserted = hist_insert(ev->t, ev->glu, ev->trend, ev->src, ev->kind);
   /* ANY non-zero result, not HIST_NEW alone -- see store.h. */
   if (r.inserted && !ev->warm)
      stat_add(ev->t, ev->glu);
   hist_refresh_current(ev->prime);
   store_unlock();

   /* OUTSIDE the lock: file I/O touches no draw-shared state, and holding a
    * lock the main thread also takes across a write is how a draw ends up
    * waiting on an SD card. Persisted on HIST_OLD as well -- the log is the
    * lifetime record and NHIST is only how much of it fits on screen. */
   if (r.inserted)
      r.persisted =
          store_append(ev->t, ev->glu, ev->trend, ev->rssi, ev->has_rssi,
                       ev->src, ev->raw, ev->tz, ev->kind, ev->rescale_pm) == 0;
   return r;
}

/* The reading log's own filename. The shell hands over the data directory
 * and nothing else -- it has no business knowing what this file is called. */
int store_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_store_path, sizeof g_store_path, dir, "/readings.csv");
   return ok;
}

const char *store_path(void)
{
   return g_store_path;
}
