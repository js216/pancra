// SPDX-License-Identifier: GPL-3.0
// sync.c --- Cloud sync client: digests, buckets, pairing
// Copyright 2026 Jakob Kastelic

/* See sync.h for the protocol. This file is the whole client except the
 * transport, which is a hook: the bytes on the wire are decided here, so a
 * second implementation can be built from sync.h without reading this code.
 *
 * The one thing that must match the server EXACTLY is the canonical text of a
 * bucket -- rows sorted bytewise ascending, each terminated by '\n'. Bytewise
 * means unsigned byte order, not any locale's idea of order, which is why the
 * comparison below casts to unsigned char rather than using strcmp on chars
 * that may be signed.
 */
#include "sync.h"
#include "clock.h"
#include "dexlibc.h"
#include "syncrow.h" /* struct row, row_ok/row_bucket/hash16: what a row IS */
/* dexlibc.h already resolves errno for both builds; naming it here as well is
 * for the reader and the linter, which both want the header that provides a
 * symbol to be visible at the point of use. */
#if __STDC_HOSTED__
#include <errno.h>
#endif
#include "hmac.h"
#include "jpake.h"
#include "rand.h" /* the nonce is entropy, not a clock */
#include "sha256.h"
#include "thread.h"    /* the config lock and the operation lock */
#include "util.h"      /* write_all / replace_finish: staged, then published */
#include <stdatomic.h> /* the progress counters cross a thread */
#include <stdint.h>    /* uint8_t: the key, the hashes, the raw bytes */
#include <stdio.h>     /* snprintf, SEEK_END */
#include <stdlib.h>    /* realloc: the log buffer grows to the file */
#include <string.h>    /* memcpy, memcmp, strcmp: rows are bytes, not strings */

struct sync_log {
   char name[40];
   char path[256];
   int bucketed;
};

/* ---- THE CONFIGURATION, AND THE LOCK THAT OWNS IT --------------------
 *
 * The transport, the account identity and the log registry are set from the
 * MAIN thread (the settings screen, the pairing worker's success path) while
 * sync_run / sync_pair / sync_restore read them on Java's push worker. They
 * were plain globals read one field at a time, so an operation could sign
 * with the new account's key against the old server, or walk a log registry
 * being rewritten under it -- and sync_clear_logs() followed by three
 * sync_add_log() calls is exactly that rewrite, four separate stores wide.
 *
 * Nothing here is read directly any more. An operation takes ONE snapshot
 * under this lock and works from that, so its configuration cannot change
 * underneath it however long it runs. */
static struct mutex g_cfg_lk = MUTEX_INIT;
static struct sync_log g_log[SYNC_MAX_LOGS];
static int g_nlog;
static sync_http_fn g_http;
static long g_uid;
static uint8_t g_key[SYNC_KEY_LEN];
static int g_have_key;

/* The response scratch is fixed: a reply is a digest or one bucket, both
 * bounded by the protocol. */
static char g_rsp[SYNC_BUF_MAX];

/* The LOG buffer grows to whatever the file is.
 *
 * It used to be a fixed 256 kB, sized as though a log were about as big as
 * one bucket. A real readings.csv is years of five-minute samples -- megabytes
 * -- so log_rows returned "too big" and the whole sync aborted after the very
 * first digest, silently and forever. The file has to be held whole because a
 * bucket's rows are NOT contiguous in it: a meter syncing a day late puts
 * yesterday's row after today's, and the canonical form is a sort. */
static char *g_buf;
static long g_bufcap;
static struct row *g_row;
static int g_rowcap;
/* A second index, for picking one bucket's rows out of a loaded window
 * without disturbing the window's own index. */
static struct row *g_sel;
static int g_selcap;

/* ---- ONE OPERATION AT A TIME, AND WHAT IT OWNS -----------------------
 *
 * struct sync_ctx is a snapshot of the configuration plus the workspace the
 * running operation may use. It is opaque outside this file (sync.h forward-
 * declares it only), and nothing below reads a configuration global directly.
 *
 * The workspace -- the log window, its two row indexes and the response
 * buffer -- is not copied into the context; the context POINTS at the single
 * set, because only one operation may hold it. That is what g_op_lk
 * enforces. Reallocating scratch shared between two concurrent operations is
 * how a restore came to parse a run's half-loaded window: the buffers grow,
 * and a realloc under another thread's pointer is not a stale read, it is a
 * freed one.
 *
 * g_op_lk is taken ONLY by the three public operations, is never nested
 * inside g_cfg_lk (the snapshot is taken and released first), and is never
 * held across a call that takes another lock in this program. */
static struct mutex g_op_lk = MUTEX_INIT;

struct sync_ctx {
   /* configuration: immutable for the life of one operation */
   sync_http_fn http;
   long uid;
   uint8_t key[SYNC_KEY_LEN];
   int have_key;
   struct sync_log log[SYNC_MAX_LOGS];
   int nlog;
   /* workspace: owned by whoever holds g_op_lk */
   char **buf;
   long *bufcap;
   struct row **row;
   int *rowcap;
   struct row **sel;
   int *selcap;
   char *rsp;
};

/* Take the operation lock and snapshot the configuration into `sx`.
 *
 * The two locks are taken in sequence, never nested: the snapshot is a copy,
 * so there is nothing to hold g_cfg_lk for once it is made. */
static void sync_ctx_begin(struct sync_ctx *sx)
{
   mutex_lock(&g_op_lk);
   mutex_lock(&g_cfg_lk);
   sx->http     = g_http;
   sx->uid      = g_uid;
   sx->have_key = g_have_key;
   memcpy(sx->key, g_key, sizeof sx->key);
   sx->nlog = g_nlog;
   memcpy(sx->log, g_log, sizeof sx->log);
   mutex_unlock(&g_cfg_lk);
   sx->buf    = &g_buf;
   sx->bufcap = &g_bufcap;
   sx->row    = &g_row;
   sx->rowcap = &g_rowcap;
   sx->sel    = &g_sel;
   sx->selcap = &g_selcap;
   sx->rsp    = g_rsp;
}

static void sync_ctx_end(void)
{
   mutex_unlock(&g_op_lk);
}

/* THE MEMORY BOUND IS A WINDOW, NOT THE FILE.
 *
 * This used to read the whole log into memory: fine at two megabytes, a
 * guaranteed wall at some larger number, and the wall would arrive silently
 * years from now as "sync stopped working". Nothing about the protocol needs
 * the whole file -- a bucket is a day, and only a bucket has to be sorted and
 * hashed together. So the file is streamed, and only the rows of the buckets
 * in the CURRENT WINDOW are kept.
 *
 * Cost: one pass over the log per window, so a log with more buckets than fit
 * in one window is read more than once. With a 2 MB window and a day of
 * readings weighing ~25 kB, a window holds about eighty days -- a decade of
 * history is a handful of passes, and the usual case (a few recent days
 * differing) is one. */
#define WINDOW_BYTES (2L * 1024 * 1024)
#define LOG_ROWS_MAX 200000

static int buf_reserve(long need)
{
   if (need <= g_bufcap)
      return 1;
   if (need > WINDOW_BYTES + (2L * SYNC_ROW_MAX))
      return 0;
   char *p = realloc(g_buf, (size_t)need);
   if (!p)
      return 0;
   g_buf    = p;
   g_bufcap = need;
   return 1;
}

static int sel_reserve(int need)
{
   if (need <= g_selcap)
      return 1;
   if (need > LOG_ROWS_MAX)
      return 0;
   int want = g_selcap ? g_selcap * 2 : 4096;
   while (want < need)
      want *= 2;
   struct row *p = realloc(g_sel, (size_t)want * sizeof *p);
   if (!p)
      return 0;
   g_sel    = p;
   g_selcap = want;
   return 1;
}

static int row_reserve(int need)
{
   if (need <= g_rowcap)
      return 1;
   if (need > LOG_ROWS_MAX)
      return 0;
   int want = g_rowcap ? g_rowcap * 2 : 4096;
   while (want < need)
      want *= 2;
   struct row *p = realloc(g_row, (size_t)want * sizeof *p);
   if (!p)
      return 0;
   g_row    = p;
   g_rowcap = want;
   return 1;
}

/* ---- CONFIGURATION SWAPS, each one atomic against a running operation --
 *
 * Every setter takes g_cfg_lk. A reader never holds it for longer than the
 * memcpy in sync_ctx_begin, so a setter cannot be delayed by a sync that is
 * waiting on a slow server. */
void sync_set_http(sync_http_fn fn)
{
   mutex_lock(&g_cfg_lk);
   g_http = fn;
   mutex_unlock(&g_cfg_lk);
}

void sync_set_key(long uid, const uint8_t key[SYNC_KEY_LEN])
{
   /* THE IDENTITY IS ONE FACT. Read field by field, an operation could sign
    * with the new uid and the old key -- a signature the server rejects, and
    * a pairing that looks broken immediately after succeeding. */
   mutex_lock(&g_cfg_lk);
   g_uid = uid;
   for (int i = 0; i < SYNC_KEY_LEN; i++)
      g_key[i] = key[i];
   g_have_key = uid > 0;
   mutex_unlock(&g_cfg_lk);
}

void sync_clear_logs(void)
{
   mutex_lock(&g_cfg_lk);
   g_nlog = 0;
   mutex_unlock(&g_cfg_lk);
}

int sync_add_log(const char *name, const char *path, int bucketed)
{
   mutex_lock(&g_cfg_lk);
   if (g_nlog >= SYNC_MAX_LOGS) {
      mutex_unlock(&g_cfg_lk);
      return -1;
   }
   /* Filled first, PUBLISHED last: g_nlog is what an operation's snapshot
    * reads as the count, so a slot is only counted once it is whole. */
   struct sync_log *l = &g_log[g_nlog];
   int n              = snprintf(l->name, sizeof l->name, "%s", name);
   if (n <= 0 || n >= (int)sizeof l->name) {
      mutex_unlock(&g_cfg_lk);
      return -1;
   }
   n = snprintf(l->path, sizeof l->path, "%s", path);
   if (n <= 0 || n >= (int)sizeof l->path) {
      mutex_unlock(&g_cfg_lk);
      return -1;
   }
   l->bucketed = bucketed;
   g_nlog++;
   mutex_unlock(&g_cfg_lk);
   return 0;
}

/* Stream a log, keeping only the rows whose bucket is in `want` (or every row
 * when `want` is NULL, which is only used to enumerate buckets). Returns the
 * row count, or -1 if even the window did not fit. A missing file is empty.
 *
 * The file is read in chunks and lines are copied out of the chunk, so the
 * memory held is the WINDOW's rows -- never the file. */
static int log_scan(const struct sync_log *l, const long *want, int nwant,
                    int *overflow)
{
   if (overflow)
      *overflow = 0;
   /* The SAME discipline log_buckets has, and for the same reason.
    *
    * This function builds the body that gets PUT. An open that failed or a
    * read that stopped early yields FEWER rows, and fewer rows for a bucket
    * the enumeration listed is a smaller bucket -- which the server adopts
    * verbatim, because a PUT replaces. Hardening only the enumeration left
    * this door open: log_buckets would happily report 128 buckets and this
    * function would then scan none of them and hand back an empty body for
    * each. A missing file is still not a failure; everything else is. */
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen  = 0;
   int over  = 0;
   int nrow  = 0;
   long used = 0;
   long n    = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n == 0)
         break; /* end of file: the scan is complete */
      if (n < 0) {
         close(fd); /* a read error mid-file: the body would be short */
         return -1;
      }
      for (long i = 0; i < n; i++) {
         if (chunk[i] != '\n') {
            if (llen < (int)sizeof line - 1)
               line[llen++] = chunk[i];
            else
               over = 1; /* over-long: not a row, and not a truncation either */
            continue;
         }
         int len = llen;
         if (len && line[len - 1] == '\r')
            len--;
         llen = 0;
         /* DAMAGE IS A SCAN FAILURE, NOT A ROW TO SKIP -- see log_buckets,
          * which carries the argument in full. The two readers must agree
          * about what a row is, so they must also agree about what damage is:
          * a row this one dropped while the enumeration kept its bucket makes
          * the PUT body SHORT, and a short body is adopted verbatim by a
          * server that replaces. An EMPTY line is the one thing still
          * skipped, and it is not damage: it carries no row. */
         if (over) {
            close(fd);
            return -1;
         }
         if (len == 0)
            continue;
         if (!row_ok(line, len)) {
            close(fd);
            return -1;
         }
         long b = row_bucket(line, (size_t)len, l->bucketed);
         if (want && !want_has(want, nwant, b))
            continue;
         if (!buf_reserve(used + len + 1) || !row_reserve(nrow + 1) ||
             !sel_reserve(nrow + 1)) {
            close(fd);
            if (overflow)
               *overflow = 1;
            return -1;
         }
         memcpy(g_buf + used, line, (size_t)len);
         g_row[nrow].off = (size_t)used;
         g_row[nrow].len = (size_t)len;
         used += len;
         nrow++;
      }
   }
   /* a final line with no newline -- damaged on the same terms as any other */
   if (over) {
      close(fd);
      return -1;
   }
   if (llen > 0) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (len > 0 && !row_ok(line, len)) {
         close(fd);
         return -1;
      }
      if (len > 0) {
         long b = row_bucket(line, (size_t)len, l->bucketed);
         if (!want || want_has(want, nwant, b)) {
            /* All THREE arrays, and a failure is refused rather than dropped.
             *
             * g_sel is indexed by this same nrow at the selection loops below,
             * and this block used to reserve only two of the three: a file
             * whose single row lacked a trailing newline never entered the
             * loop above, so g_sel was still NULL and the selection wrote
             * through it; otherwise g_selcap ended one short of nrow and it
             * wrote 16 bytes past the allocation.
             *
             * The old code also SKIPPED the row when a reservation failed,
             * while the loop above returns -1 with *overflow set. A skipped
             * row makes the bucket text short, the short bucket is pushed as
             * authoritative, and the verify agrees because it reads through
             * this same function. Losing a row must fail the sync, not shrink
             * the record. */
            if (!buf_reserve(used + len + 1) || !row_reserve(nrow + 1) ||
                !sel_reserve(nrow + 1)) {
               close(fd);
               if (overflow)
                  *overflow = 1;
               return -1;
            }
            memcpy(g_buf + used, line, (size_t)len);
            g_row[nrow].off = (size_t)used;
            g_row[nrow].len = (size_t)len;
            nrow++; /* no `used +=`: this block is the last writer */
         }
      }
   }
   close(fd);
   return nrow;
}

/* Every bucket the log holds, ascending. Streams the file and keeps only the
 * bucket numbers -- one long per day, so a decade costs 30 kB.
 *
 * Returns the count, or -1 if the log could NOT BE FULLY ENUMERATED, which is
 * the difference between a backup and a deletion. A bucket the server holds
 * and this list omits is pushed as an EMPTY body, which the server implements
 * as DELETE FROM logrow -- that is how a correction or a real deletion
 * travels. So an incomplete list here is not merely inaccurate, it is an
 * instruction to destroy the server's copy.
 *
 * This used to `return 0` on any open() failure, so a permissions problem, a
 * wrong path or an exhausted fd table read as "this log is empty" and the next
 * sync erased the server's entire copy of a lifetime record -- precisely what
 * the server exists to hold. It also stopped filling `out` at `cap` without
 * saying so, so a log with more buckets than the caller's array deleted its
 * own tail on every sync.
 *
 * A MISSING FILE IS NOT A FAILURE: a log never written has no buckets, and
 * saying so is correct. Every other failure is refused. */
static int log_buckets(const struct sync_log *l, long *out, int cap)
{
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen = 0;
   int over = 0;
   int nb   = 0;
   long n   = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n == 0)
         break; /* end of file: the enumeration is complete */
      if (n < 0) {
         close(fd); /* a read error mid-file: we know less than we think */
         return -1;
      }
      for (long i = 0; i < n; i++) {
         if (chunk[i] != '\n') {
            if (llen < (int)sizeof line - 1)
               line[llen++] = chunk[i];
            else
               over = 1;
            continue;
         }
         int len = llen;
         if (len && line[len - 1] == '\r')
            len--;
         llen = 0;
         /* A ROW THE PARSER REFUSES IS NOT PERMISSION TO DELETE THE REPLICA.
          *
          * This loop used to SKIP an over-long or invalid row and carry on,
          * and then hand the surviving buckets to a caller that PUTs an empty
          * body -- the server's spelling of DELETE -- to every remote bucket
          * this list does not name. So one corrupt line was enough: if the
          * damaged row was the only one left for its day (a day with a single
          * reading, or a day whose other rows had already been corrected
          * away), that day vanished from the list, and the next sync deleted
          * the server's copy of it. The phone still HELD the data -- the
          * bytes were in the file, merely unparseable by this reader -- so
          * what the user lost was the one copy that a later fix could have
          * been reconciled against. Silently, with no undo on either side,
          * for a defect whose visible symptom was nothing at all.
          *
          * The rule is the one this function already applies to a read error
          * below: an incomplete picture must never drive a loop that deletes.
          * Refusing costs one sync cycle and says so loudly every cycle after
          * -- the sync stays broken rather than the backup.
          *
          * AN EMPTY LINE IS NOT DAMAGE. It carries no row, log_scan agrees it
          * contributes nothing, and a file that is nothing but newlines is
          * genuinely empty -- the state the caller must tell apart from this
          * one, because deliberate emptiness has its own evidence
          * (log_clear_generation) and damage has none. */
         if (over) {
            close(fd);
            return -1;
         }
         if (len == 0)
            continue;
         if (!row_ok(line, len)) {
            close(fd);
            return -1;
         }
         long b = row_bucket(line, len, l->bucketed);
         if (want_has(out, nb, b))
            continue;
         if (nb >= cap) { /* one bucket too many: see the header comment */
            close(fd);
            return -1;
         }
         out[nb++] = b;
      }
   }
   /* A final line with no newline, stripped of a trailing \r exactly as the
    * loop above does and exactly as log_scan does.
    *
    * This used to test the RAW length while log_scan stripped first. row_ok
    * rejects \r as a control byte, so a log whose last line lacked a newline
    * and ended \r produced a row log_scan admitted and this function did not.
    * If that row's bucket appeared nowhere else in the file, the bucket was
    * missing from the enumeration, was never pushed, and -- being a bucket the
    * server holds and the list omits -- was DELETED from the server. The two
    * readers must agree on what a row is, or the disagreement is destructive.
    */
   if (over) {
      close(fd);
      return -1;
   }
   if (llen > 0) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (len > 0 && !row_ok(line, len)) {
         close(fd);
         return -1;
      }
      if (len > 0) {
         long b = row_bucket(line, len, l->bucketed);
         if (!want_has(out, nb, b)) {
            if (nb >= cap) {
               close(fd);
               return -1;
            }
            out[nb++] = b;
         }
      }
   }
   close(fd);
   for (int i = 1; i < nb; i++) { /* ascending, as the server orders */
      long t = out[i];
      int j  = i - 1;
      while (j >= 0 && out[j] > t) {
         out[j + 1] = out[j];
         j--;
      }
      out[j + 1] = t;
   }
   return nb;
}

/* THE CANONICAL FORM OF ONE BUCKET, and the only place it is spelled out.
 *
 * The hash the phone and the server compare is taken over THIS text, so the
 * two sides agree only while they build it identically: the bucket's rows,
 * SORTED, DE-DUPLICATED, each newline-terminated. A bucket is a SET -- an
 * identical row written twice is one row, which is what the server's primary
 * key enforces on its side.
 *
 * It was written out three times: once here and once inside each of the two
 * sync loops. Three copies of an invariant that must match a REMOTE
 * implementation is the kind of duplication that does not announce itself
 * when it drifts -- the symptom is a bucket both sides hold identically and
 * neither will stop re-sending, forever.
 *
 * `src` are the rows currently loaded (an index into g_buf). When `filter` is
 * set, only rows falling in `bucket` are taken; otherwise all of them are,
 * for a caller that has already scanned exactly one bucket. Returns the
 * length written, or -1 if it does not fit.
 *
 * NUL-TERMINATED, and one byte of `cap` is reserved for that. The length is
 * the answer the callers use -- the push signs and sends exactly `k` bytes --
 * but the buffer is also read as a C string (the interop test's strstr, and
 * anything that logs it), and an unterminated one runs off the end into
 * whatever the caller's stack held. It cost nothing to terminate and the
 * absence of it was invisible until a caller's stack happened to be
 * dirty. */
static long bucket_text(const struct row *src, int nsrc, int filter,
                        long bucket, int bucketed, char *out, size_t cap)
{
   if (!src || !out)
      return -1;
   if (!sel_reserve(nsrc))
      return -1;
   int nb = 0;
   for (int i = 0; i < nsrc; i++) {
      if (filter &&
          row_bucket(g_buf + src[i].off, src[i].len, bucketed) != bucket)
         continue;
      g_sel[nb++] = src[i];
   }
   row_sort(g_buf, g_sel, nb);
   size_t k = 0;
   for (int i = 0; i < nb; i++) {
      if (i && g_sel[i].len == g_sel[i - 1].len &&
          !memcmp(g_buf + g_sel[i].off, g_buf + g_sel[i - 1].off, g_sel[i].len))
         continue;
      /* +1 for this row's newline, and cap-1 because the terminator is
       * already spoken for. */
      if (cap == 0 || g_sel[i].len + 1 > (cap - 1) - k)
         return -1;
      memcpy(out + k, g_buf + g_sel[i].off, g_sel[i].len);
      k += g_sel[i].len;
      out[k++] = '\n';
   }
   if (cap == 0)
      return -1;
   out[k] = '\0';
   return (long)k;
}

long sync_bucket_text(int log_idx, long bucket, char *out, long cap)
{
   if (log_idx < 0 || log_idx >= g_nlog)
      return -1;
   const struct sync_log *l = &g_log[log_idx];
   long one                 = bucket;
   int nb                   = log_scan(l, &one, 1, NULL);
   if (nb < 0)
      return -1;
   if (cap < 0)
      return -1;
   /* log_scan already selected exactly this bucket, so no further filter. */
   return bucket_text(g_row, nb, 0, bucket, l->bucketed, out, (size_t)cap);
}

/* ---- signed requests -------------------------------------------------- */

/* SYNC PROGRESS, read by a thread that is not the one making it.
 *
 * sync_run() is driven from Ble.remotePush's worker thread (and from the
 * interop test's main thread); sync_progress() is called by the UI to draw the
 * bar. Three plain ints marked `volatile` said nothing to the compiler about
 * the other thread and nothing to the CPU about ordering.
 *
 * RELEASE on `active` and ACQUIRE on the read of it, because `active` is what
 * publishes the other two: a reader that sees the run as active must also see
 * the total that was computed before it was flagged, or the bar divides by a
 * zero denominator on its first frame. `done` is relaxed -- it is a counter
 * that only ever climbs, and a bar one bucket behind is not a defect. */
static atomic_int g_pdone, g_ptotal, g_pactive;

int sync_progress(int *done, int *total)
{
   int active = atomic_load_explicit(&g_pactive, memory_order_acquire);
   if (done)
      *done = atomic_load_explicit(&g_pdone, memory_order_relaxed);
   if (total)
      *total = atomic_load_explicit(&g_ptotal, memory_order_relaxed);
   return active;
}

/* Every bucket this client holds, across every log: the denominator of the
 * progress the UI draws. Counted up front, in one pass per log, because a
 * bar that discovers its own length as it goes is not a bar. */
static int count_buckets(void)
{
   static long bl[SYNC_LOCAL_BUCKETS];
   int total = 0;
   for (int i = 0; i < g_nlog; i++) {
      int n = log_buckets(&g_log[i], bl, (int)(sizeof bl / sizeof bl[0]));
      if (n > 0) /* a log we cannot enumerate contributes nothing to a count */
         total += n;
   }
   return total;
}

/* THE NONCE, AND WHAT MAKES IT UNIQUE.
 *
 * THE INVARIANT: no two signed requests this phone ever makes carry the same
 * nonce, for any two requests the server could still be holding -- and the
 * server holds them for its whole replay window (2 * SIG_SKEW).
 *
 * It used to be "p<seconds>-<counter>", with the counter a process-local
 * static, and the claim was that the counter cannot repeat within a run while
 * the timestamp cannot repeat across runs. The second half is false in two
 * ordinary situations:
 *
 *   - TWO RESTARTS IN THE SAME SECOND. The counter goes back to 1 with every
 *     process, and this process is restarted by the OS whenever the service
 *     is rebuilt, the activity is swiped away, or Android reclaims it. Two
 *     runs starting inside one second -- a crash and its immediate restart --
 *     produce "p<T>-1" twice.
 *   - A CLOCK THAT GOES BACKWARDS. realtime_s() is the WALL clock: NTP
 *     corrects it after a flat battery, and the user can set it by hand. Once
 *     it steps back, every second it re-covers can re-issue nonces that are
 *     still inside the server's window.
 *
 * A repeat is not a security hole -- the server rejects it, which is what the
 * table is for -- but the request it rejects is a LEGITIMATE one, and what
 * the user sees is a sync that fails for no visible reason and keeps failing
 * while the clock catches up.
 *
 * So: 128 bits from the OS entropy source, and nothing else. No clock, no
 * counter, no process state -- which is exactly why a restart, a same-second
 * restart, or a clock correction cannot affect it. Two nonces collide with
 * probability 2^-128 per pair; a phone that syncs every five minutes for a
 * century makes ~10^7 requests, so the chance of any collision at all is
 * around 10^-24.
 *
 * A FAILURE TO GET ENTROPY IS A FAILURE TO SIGN. rand_bytes returns 0 with
 * the buffer UNDEFINED, and an undefined nonce is stack contents: repeatable,
 * and repeatable is the one thing this may not be. The request is refused
 * instead -- the sync retries, and the alternative is a request the server
 * will reject anyway. */
int sync_nonce(char *out, int cap)
{
   if (!out || cap < 33)
      return 0;
   uint8_t b[16];
   if (!rand_bytes(b, sizeof b)) {
      out[0] = 0;
      return 0;
   }
   hexify(b, (int)sizeof b, out);
   return 1;
}

static int signed_req(const char *method, const char *path, const char *body,
                      int blen, char *out, int outcap)
{
   if (!g_http || !g_have_key)
      return -1;
   uint8_t bh[32];
   char bhex[65];
   sha256((const uint8_t *)(body ? body : ""), (size_t)blen, bh);
   hexify(bh, 32, bhex);

   char nonce[40];
   /* 128 random bits: see sync_nonce for why it is neither the clock nor a
    * counter. A phone that cannot reach its entropy source does not sign. */
   if (!sync_nonce(nonce, (int)sizeof nonce))
      return -1;

   static char msg[1400];
   long ts = realtime_s();
   int n = sync_signing_string(msg, sizeof msg, method, path, ts, nonce, bhex);
   if (n <= 0)
      return -1;
   uint8_t mac[32];
   char machex[65];
   hmac_sha256(g_key, SYNC_KEY_LEN, (const uint8_t *)msg, (size_t)n, mac);
   hexify(mac, 32, machex);
   char hdr[256];
   n = snprintf(hdr, sizeof hdr, "Authorization: Pancra %ld:%ld:%s:%s\r\n",
                g_uid, ts, nonce, machex);
   if (n <= 0 || n >= (int)sizeof hdr)
      return -1;
   return g_http(method, path, hdr, body, blen, out, outcap);
}

int sync_signing_string(char *out, size_t cap, const char *method,
                        const char *path, long ts, const char *nonce,
                        const char *bodyhash)
{
   if (!out || cap == 0)
      return 0;
   int n = snprintf(out, cap, "%s\n%s\n%ld\n%s\n%s", method, path, ts, nonce,
                    bodyhash);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}

int sync_request(const char *method, const char *path, const char *body,
                 int blen, char *out, int outcap)
{
   return signed_req(method, path, body, blen, out, outcap);
}

/* THE ROUTES, BUILT IN ONE PLACE. See sync.h for why. A log name longer than
 * the wire allows cannot produce a path here either -- the format is checked
 * against the buffer, and a path that did not fit is refused rather than
 * silently addressed at some other bucket. */
int sync_path_bucket(char *out, size_t cap, const char *log, long bucket)
{
   if (!out || !log || cap == 0)
      return 0;
   int n = snprintf(out, cap, "/v1/bucket/%s/%ld", log, bucket);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}

int sync_path_digest(char *out, size_t cap, const char *log)
{
   if (!out || !log || cap == 0)
      return 0;
   int n = snprintf(out, cap, "/v1/digest/%s", log);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}

long sync_fetch_bucket(int log_idx, long bucket, char *out, long cap)
{
   if (log_idx < 0 || log_idx >= g_nlog || !out || cap <= 0 ||
       cap > 0x7fffffffL)
      return -1;
   char path[128];
   if (!sync_path_bucket(path, sizeof path, g_log[log_idx].name, bucket) ||
       signed_req("GET", path, "", 0, out, (int)cap) != 200)
      return -1;
   long len = s_len(out);
   return len < cap ? len : -1;
}

/* ---- pairing ---------------------------------------------------------- */

/* Four steps, not three. J-PAKE's proofs validate whatever code each side
 * used, so two parties with DIFFERENT codes finish all three rounds and
 * simply derive different keys. The key cannot be derived until the peer's
 * round 3 has arrived, so neither side can prove itself inside round 3: the
 * server proves itself in the round-3 reply, and step 4 carries ours. Saving
 * a key without checking the server's proof would mean trusting whatever
 * answered the request. */
static int pair_step(const char *path, const char *body, int blen, char *out,
                     int outcap)
{
   if (!g_http)
      return -1;
   return g_http("POST", path, NULL, body, blen, out, outcap);
}

void sync_confirm_tag(const uint8_t key[SYNC_KEY_LEN], const char *label,
                      char out[33])
{
   uint8_t mac[32];
   char hex[65];
   hmac_sha256(key, SYNC_KEY_LEN, (const uint8_t *)label, (size_t)s_len(label),
               mac);
   hexify(mac, 32, hex);
   for (int i = 0; i < 32; i++)
      out[i] = hex[i];
   out[32] = '\0';
}

static int sync_pair_inner(const struct sync_ctx *sx, const char *email,
                           const char *code, uint8_t out_key[SYNC_KEY_LEN],
                           long *out_uid)
{
   (void)sx; /* pairing is where the key COMES FROM: it signs nothing, so it
              * reads no identity from the snapshot. It takes the operation
              * lock all the same, because it uses the same response
              * workspace as a run and must not share it with one. */
   /* Idempotent, and REQUIRED: without the curve context jpake_new hands
    * back a pairing whose very first round fails, with nothing on the wire to
    * explain why. The app also calls this from the BLE driver, which is
    * exactly why it was easy to miss here. */
   jpake_init();
   struct jpake *p = jpake_new((const uint8_t *)code, (size_t)s_len(code), 1);
   if (!p)
      return -1;
   int rc = -1;
   uint8_t pkt[160];
   uint8_t peer[160];
   uint8_t key[SYNC_KEY_LEN];
   char hex[321];
   char body[512];
   char session[64];
   const char *nl = NULL;

   do {
      if (!jpake_round1(p, pkt))
         break;
      hexify(pkt, 160, hex);
      int n = snprintf(body, sizeof body, "%s\n%s\n", email, hex);
      if (pair_step("/v1/pair/1", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;
      nl = s_chr(g_rsp, '\n');
      if (!nl || (long)(nl - g_rsp) >= (long)sizeof session)
         break;
      memcpy(session, g_rsp, (size_t)(nl - g_rsp));
      session[nl - g_rsp] = '\0';
      if (!unhex(nl + 1, 320, peer) || !jpake_peer_round1(p, peer))
         break;

      if (!jpake_round2(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step("/v1/pair/2", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!unhex(g_rsp, 320, peer) || !jpake_peer_round2(p, peer))
         break;

      if (!jpake_round3(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step("/v1/pair/3", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!unhex(g_rsp, 320, peer) || !jpake_peer_round3(p, peer))
         break;
      if (!jpake_shared_key(p, key))
         break;

      /* Verify the SERVER before saving anything. */
      char want[33];
      sync_confirm_tag(key, SYNC_CONFIRM_LABEL_SERVER, want);
      nl = s_chr(g_rsp, '\n');
      if (!nl || strncmp(nl + 1, want, 32) != 0)
         break;

      char mine[33];
      sync_confirm_tag(key, SYNC_CONFIRM_LABEL_CLIENT, mine);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, mine);
      if (pair_step("/v1/pair/4", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;

      long uid = 0;
      for (const char *q = g_rsp; *q >= '0' && *q <= '9'; q++)
         uid = (uid * 10) + (*q - '0');
      if (uid <= 0)
         break;
      sync_set_key(uid, key);
      for (int i = 0; i < SYNC_KEY_LEN; i++)
         out_key[i] = key[i];
      if (out_uid)
         *out_uid = uid;
      rc = 0;
   } while (0);

   jpake_free(p);
   return rc;
}

int sync_pair(const char *email, const char *code,
              uint8_t out_key[SYNC_KEY_LEN], long *out_uid)
{
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   int rc = sync_pair_inner(&sx, email, code, out_key, out_uid);
   sync_ctx_end();
   return rc;
}

/* ---- the sync algorithm ----------------------------------------------- */

/* ---- ONE LINE OF A DIGEST REPLY ---------------------------------------
 *
 * THREE ANSWERS, BECAUSE THERE ARE THREE SITUATIONS. This returned 1 for a
 * row and 0 for everything else -- and "everything else" was the end of the
 * reply AND every possible syntax failure, reported identically.
 *
 * Both callers loop `while (digest_line(...))` and then treat what they
 * collected as the server's complete bucket list. So a reply that was cut in
 * half by a dropped connection, or whose tenth line was corrupt, stopped the
 * loop and was accepted as the whole truth about what the server holds. What
 * follows from that is not a failed sync:
 *
 *   - in sync_one_log, a bucket the server DOES hold but whose line was never
 *     parsed looks like one the server does not have, so the phone pushes it
 *     again -- harmless -- while a bucket listed after the damage that the
 *     phone has since deleted is never deleted server-side;
 *   - in sync_restore, the missing lines are buckets the phone will not pull
 *     back, and the restore then reports SUCCESS with a count. The user asked
 *     for their record and was told it came back, short.
 *
 * Framing is checked as strictly as the fields, because a truncated reply's
 * last line is usually well-formed for as far as it goes: "20000 3 abc" with
 * no newline is a plausible row and a certain sign that the rest is missing. */

/* Digits only, non-empty, and not more than a long can hold. The bucket is a
 * day number and the count a row count; a run long enough to overflow is not a
 * big number, it is a different string. */
static int digest_num(const char *s, const char *e, long *out)
{
   if (s >= e)
      return 0; /* an empty field is not a zero */
   long v = 0;
   int nd = 0;
   for (const char *r = s; r < e; r++) {
      if (*r < '0' || *r > '9')
         return 0;
      if (++nd > 18)
         return 0; /* eighteen digits is the widest that cannot overflow */
      v = (v * 10) + (*r - '0');
   }
   *out = v;
   return 1;
}

/* CORRUPT THE DIGEST WE JUST FETCHED, on demand.
 *
 * A real server never sends a malformed digest, so the branch that REFUSES one
 * -- the branch this whole item is about -- could be argued for but not run.
 * Every parser case can be tested by calling digest_line directly; what cannot
 * is the caller's response to DLINE_BAD, which is where the damage either
 * stops a restore or is quietly read as the end of the list.
 *
 * The corruption is truncation at the first newline, because that is what a
 * dropped connection actually leaves: a valid prefix and a last line that
 * looks complete until its newline is asked for.
 *
 * TRUNCATION AND NOTHING ELSE, deliberately. An earlier version of this also
 * replaced the newline with a space, and that quietly weakened it: the
 * surviving line then ended in a space, so the parser refused it for its HASH
 * WIDTH (seventeen bytes) and the rule this fault exists to drive from a
 * CALLER -- every row ends in a newline, including the last -- was never
 * reached. Removing the framing check altogether would have left every gate
 * green. So the bytes that survive are exactly the bytes that arrived. */
#ifdef APP_FAULTS
/* Append APP_RESTORE_APPEND's text to `path`, as a reading arriving mid-restore
 * would. Only the FIRST call does it, so one armed variable stands for one
 * concurrent append rather than one per log. */
static void restore_append_fault(const char *path)
{
   static int done;
   const char *s = getenv("APP_RESTORE_APPEND");
   if (done || !s || !*s)
      return;
   done   = 1;
   int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return;
   (void)write_all(fd, s, (int)s_len(s));
   close(fd);
}

static void digest_fault(char *buf)
{
   const char *s = getenv("APP_FAIL_DIGEST");
   if (!s || !*s || *s == '0')
      return;
   char *r = strchr(buf, '\n');
   if (r)
      *r = '\0'; /* the first line's bytes, without its terminator */
}

/* A FETCHED BUCKET THAT IS NOT WHAT THE DIGEST PROMISED. The restore decides
 * which buckets it is short of from the hashes in the digest, then downloads
 * them -- and the rows it is about to write into the user's log are exactly
 * the rows nobody else still has. A body that lost bytes on the way, or came
 * from the wrong bucket, or was rewritten in between, is the one input the
 * check after this must refuse.
 *
 * A REAL SERVER NEVER SENDS ONE, which is precisely why the refusal needs a
 * fault to drive it: without this the check could be argued for but never
 * run, and deleting it would leave every gate green. One byte is changed, so
 * what differs is the HASH and nothing else -- the body is still a
 * well-formed bucket, so a check that merely looked at its shape would still
 * pass it. */
static void restore_body_fault(char *buf)
{
   const char *s = getenv("APP_FAIL_BUCKET");
   if (!s || !*s || *s == '0')
      return;
   if (*buf)
      *buf = (*buf == 'z') ? 'y' : 'z';
}
#else
#define digest_fault(b)         ((void)(b))
#define restore_append_fault(p) ((void)(p))
#define restore_body_fault(b)   ((void)(b))
#endif

enum dline digest_line(const char **p, char *name, int ncap, long *count,
                       char hash[17])
{
   const char *q = *p;
   if (!*q)
      return DLINE_END;

   /* FIELD ONE: the name, up to the first space. */
   const char *ns = q;
   while (*q && *q != ' ' && *q != '\n')
      q++;
   if (*q != ' ' || q == ns)
      return DLINE_BAD; /* no separator, or an empty name */
   if (q - ns > ncap - 1)
      return DLINE_BAD; /* it does not fit, so we cannot say what it was */
   int k = (int)(q - ns);
   memcpy(name, ns, (size_t)k);
   name[k] = '\0';
   q++;

   /* FIELD TWO: the count, digits only. */
   const char *cs = q;
   while (*q && *q != ' ' && *q != '\n')
      q++;
   if (*q != ' ')
      return DLINE_BAD;
   if (!digest_num(cs, q, count))
      return DLINE_BAD;
   q++;

   /* FIELD THREE: the hash, EXACTLY sixteen bytes. It used to take up to
    * sixteen and then skip whatever else was on the line, so a short hash was
    * silently padded with the NUL and a long one silently truncated -- either
    * way compared equal to nothing and unequal to everything, which reads as
    * "this bucket differs" and pushes it again for ever. */
   const char *hs = q;
   while (*q && *q != '\n')
      q++;
   if (q - hs != 16)
      return DLINE_BAD;
   memcpy(hash, hs, 16);
   hash[16] = '\0';

   /* FRAMING. Every row ends in a newline, including the last: a reply whose
    * final line has none was cut off, and its last row is exactly the row most
    * likely to look complete. */
   if (*q != '\n')
      return DLINE_BAD;
   q++;
   *p = q;
   return DLINE_ROW;
}

/* (push_bucket is gone: the window loop builds a bucket's canonical text and
 * PUTs it in place, so there is nothing left to hand to a helper.) */

/* Every bucket this log has locally, with its hash, compared against what the
 * server reports. Buckets the server holds and we do not are pushed EMPTY,
 * which deletes them -- the app is authoritative, so "we no longer have it"
 * has to mean "nor do you". */
static int sync_one_log(int li)
{
   const struct sync_log *l = &g_log[li];
   char path[128];
   if (!sync_path_digest(path, sizeof path, l->name))
      return -1;
   if (signed_req("GET", path, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
      return -1;
   /* THE SAME FAULT THE RESTORE PATH ARMS, and it belongs here even more than
    * there: the loop at the bottom of this function PUTs an empty body to
    * every remote bucket the local enumeration did not find, which is the only
    * data-destroying loop in this file. A digest read as shorter than it is
    * makes buckets the server holds look absent -- so the failure mode of
    * reading damage as the end of the list is, here, "delete what you could
    * not read about".
    *
    * Without this the refusal below could be argued for but not run: a real
    * server never sends a malformed digest, and the parser cases in
    * app/test/interoptest.c prove what digest_line ANSWERS, not what this
    * caller does with the answer. Compiled out entirely unless APP_FAULTS is
    * defined, which nothing that ships defines. */
   digest_fault(g_rsp);

   /* Remote buckets, so we can spot ones we no longer hold. */
   static long rb[SYNC_REMOTE_BUCKETS];
   static char rh[SYNC_REMOTE_BUCKETS][17];
   int nrb       = 0;
   const char *q = g_rsp;
   char nm[40];
   long cnt = 0;
   char hh[17];
   /* THE REMOTE LIST HAS A CEILING, AND CROSSING IT IS NOT SILENT.
    *
    * Past rb[]'s size this loop simply stopped parsing, so every bucket
    * beyond it had no `theirs` to compare against and was re-pushed on EVERY
    * sync, forever -- megabytes a run, as often as once a minute, with
    * nothing to say why. Both ceilings are named in sync.h now (~11 and ~22
    * years of daily buckets) and asserted against the wire's own
    * LOG_BUCKETS, rather than each being whatever a sizeof at one call site
    * happened to say.
    *
    * Refusing is the same choice log_buckets already makes for the local
    * side, and for the same reason: an incomplete picture of what the server
    * holds drives a loop that deletes. */
   for (;;) {
      enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
      if (d == DLINE_END)
         break;
      /* MALFORMED IS NOT FINISHED. Read as the end of the list, a damaged
       * reply makes buckets the server holds look absent -- and the loop this
       * feeds DELETES on that basis. */
      if (d == DLINE_BAD)
         return -1;
      if (nrb >= (int)(sizeof rb / sizeof rb[0]))
         return -1;
      /* The bucket is validated as a number here, not scanned for a digit
       * prefix: "20000x" used to parse as 20000 and name a real day. */
      long b = 0;
      if (!digest_num(nm, nm + s_len(nm), &b))
         return -1;
      rb[nrb] = b;
      memcpy(rh[nrb], hh, 17);
      nrb++;
   }

   /* Local buckets: enumerated by streaming, so this costs one pass and one
    * long per day rather than the whole log in memory. */
   static long lb[SYNC_LOCAL_BUCKETS];
   int nlb = log_buckets(l, lb, (int)(sizeof lb / sizeof lb[0]));
   /* REFUSE TO SYNC A LOG WE CANNOT READ WHOLE. Below, every remote bucket
    * missing from lb[] is PUT empty, which deletes it on the server. If this
    * list is short for any reason other than the log genuinely not holding
    * those buckets, that loop is a data-destroying loop. Failing the sync
    * costs one cycle; getting this wrong costs the backup. */
   if (nlb < 0)
      return -1;

   /* AND A LOG WE NO LONGER HAVE AT ALL DOES NOT GET TO EMPTY THE SERVER.
    *
    * `nlb == 0` is the honest answer for a log that was never written, and
    * refusing a read error (above) does not cover it: a MISSING file is not
    * an error, it is zero buckets. But zero local buckets against a server
    * that holds many is not a correction the user made, it is a phone that
    * lost its storage -- a reinstall, a cleared app, a restored handset with
    * allowBackup="false". The deletion loop at the bottom would then walk
    * every bucket the server has and PUT each one empty, erasing years of
    * readings in a few hundred requests, silently, with no undo on either
    * side.
    *
    * The app is authoritative about CHANGES it made, not about a record it
    * has simply forgotten. So this refuses, loudly and every cycle, until
    * either the log comes back or the record is pulled down (sync.h's restore
    * direction) -- the sync stays broken rather than the backup.
    *
    * UNLESS THE USER REALLY DID DELETE IT ALL, and the deletion workflow said
    * so in writing.
    *
    * The refusal above is right and its cost is real: slots_save() writes the
    * whole device registry and nothing else, so removing the last device
    * leaves slots.csv zero bytes long. Without evidence that is
    * indistinguishable from a wiped phone -- so the sync refused it for ever,
    * the server went on holding devices the user had removed, and because
    * sync_run stops at the first log that fails, every OTHER log stopped
    * syncing too. A deliberate deletion that can never converge is a second
    * way to lose the record, by making the backup stop.
    *
    * So the deletion workflow leaves a durable, versioned tombstone next to
    * the log (log_note_cleared in util.h), and ONLY that tombstone authorises
    * the empty replacement below. The two states that decide whether this is
    * safe, both answered by log_clear_generation and neither of them here:
    *
    *   TOMBSTONE PRESENT, LOG NON-EMPTY -- it authorises nothing. This guard
    *   is not even reached (nlb > 0), the rows are pushed exactly as always,
    *   and the stale evidence is dropped below so it cannot speak for a
    *   LATER emptiness it knows nothing about.
    *
    *   TOMBSTONE ABSENT, LOG EMPTY OR MISSING -- refused, exactly as before.
    *   That is the storage-loss case and it stays fail-safe. A MISSING log is
    *   refused even WITH a tombstone: clearing a log leaves an empty file,
    *   losing the storage leaves a hole, and only the first is evidence. */
   if (nlb == 0 && nrb > 0 && log_clear_generation(l->path) <= 0)
      return -1;
   /* THE EVIDENCE EXPIRES THE MOMENT IT STOPS BEING TRUE. A log with rows in
    * it is not empty, deliberately or otherwise, so a tombstone left beside
    * it is answering a question nobody asked -- and would answer it wrongly
    * if the file were later truncated by something that is not the user. */
   if (nlb > 0)
      (void)log_clear_forget(l->path);

   /* Walk them in WINDOWS. Each window is one pass over the file keeping only
    * that window's rows, so the memory held is the window and never the log.
    * A window is grown until its rows would exceed the budget, which
    * log_scan reports by failing -- so the loop halves the window and tries
    * again rather than guessing sizes up front. */
   static char text[SYNC_BUF_MAX];
   /* Unsigned window bounds. `at` and `span` only ever walk forward from 0,
    * but as ints nothing said so, and an index that MIGHT be negative makes
    * every lb[k] below a possible read before the array. */
   size_t nb_all = (size_t)nlb;
   size_t at     = 0;
   while (at < nb_all) {
      size_t span = nb_all - at;
      if (span > 128)
         span = 128;
      int nrow     = 0;
      int overflow = 0;
      for (;;) {
         nrow = log_scan(l, lb + at, (int)span, &overflow);
         if (nrow >= 0)
            break;
         if (!overflow || span == 1)
            return -1; /* a single bucket that does not fit is a real error */
         span /= 2;
      }
      for (size_t k = at; k < at + span; k++) {
         /* Rows for this window are in g_buf; select this bucket's and hash
          * them exactly as the server does. */
         long tl =
             bucket_text(g_row, nrow, 1, lb[k], l->bucketed, text, sizeof text);
         if (tl < 0)
            return -1;
         size_t tn = (size_t)tl;
         char mine[17];
         hash16(text, (long)tn, mine);
         const char *theirs = 0;
         for (int rr = 0; rr < nrb; rr++)
            if (rb[rr] == lb[k]) {
               theirs = rh[rr];
               break;
            }
         if (theirs && strncmp(theirs, mine, 16) == 0) {
            atomic_fetch_add_explicit(&g_pdone, 1, memory_order_relaxed);
            continue;
         }
         /* NEVER push an empty body for a bucket the enumeration says exists.
          *
          * The server implements a zero-length PUT as DELETE FROM logrow, so
          * this loop and the deletion loop below write the same request; only
          * the caller's intent differs. lb[] is built from rows that passed
          * row_ok, so every bucket in it holds at least one row, and tn == 0
          * here means the scan and the enumeration disagree -- the file was
          * replaced under us, the open failed, or the two readers parsed the
          * last line differently. Every one of those is a reason to stop, not
          * a reason to erase the server's copy of a day we still have.
          *
          * Deletion is legitimate in exactly one place: the loop below, which
          * walks buckets the enumeration did NOT find. */
         if (tn == 0)
            return -1;
         char path2[128];
         if (!sync_path_bucket(path2, sizeof path2, l->name, lb[k]))
            return -1;
         if (signed_req("PUT", path2, text, (int)tn, g_rsp, SYNC_BUF_MAX) !=
             200)
            return -1;
         atomic_fetch_add_explicit(&g_pdone, 1, memory_order_relaxed);
      }
      at += span;
   }

   for (int r = 0; r < nrb; r++) {
      int still = 0;
      for (int k = 0; k < nlb; k++)
         if (lb[k] == rb[r]) {
            still = 1;
            break;
         }
      if (still)
         continue;
      char p2[128];
      if (!sync_path_bucket(p2, sizeof p2, l->name, rb[r]))
         return -1;
      if (signed_req("PUT", p2, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
         return -1;
   }
   return 0;
}

/* ---- RESTORE: the direction that was missing ---------------------------
 *
 * The protocol is a replica: the phone is authoritative, and a bucket the
 * phone no longer holds is a correction the server must adopt. That is right
 * in steady state, and exactly wrong in the one case a backup exists for -- a
 * phone that has LOST its record. There, "I no longer hold it" is not a
 * correction, it is amnesia, and sync_one_log refuses to act on it (see the
 * nlb == 0 guard there).
 *
 * This turns that refusal into a recovery. It is DELIBERATELY NOT automatic:
 * an automatic pull would make a genuine deletion undoable by the next sync,
 * which is the same silent-wrong-direction problem in reverse. The user asks
 * for it, once, and it only ever ADDS -- every bucket the server holds and
 * this phone does not is appended to the local log, and nothing is removed.
 *
 * Returns the number of buckets restored, or -1. The caller reloads the logs
 * afterwards: this writes files, it does not touch the in-memory history. */
/* Copy `path` into `fd`, whole. 0 on success.
 *
 * The staging file starts as an exact copy of the live log, so a restore that
 * fails anywhere can be abandoned by deleting the copy -- the original has not
 * been opened for writing at all. A MISSING original is success with nothing
 * copied: a phone whose record is gone is the case this whole path exists for,
 * and there is no file to preserve. */
/* The canonical hash of ONE local bucket, into out[17]. 0, or -1 when the log
 * could not be read whole -- which is not "the bucket is empty" and must not
 * be treated as a hash that merely differs (see the caller). */
static int local_bucket_hash(const struct sync_log *l, long b, char out[17])
{
   static char text[SYNC_BUF_MAX];
   long one = b;
   int nrow = log_scan(l, &one, 1, NULL);
   if (nrow < 0)
      return -1;
   /* log_scan already selected exactly this bucket, so no further filter --
    * the same call sync_bucket_text makes, and the same bytes the server
    * hashed. */
   long tl = bucket_text(g_row, nrow, 0, b, l->bucketed, text, sizeof text);
   if (tl < 0)
      return -1;
   hash16(text, tl, out);
   return 0;
}

static int copy_into(int fd, const char *path, long *copied)
{
   if (copied)
      *copied = 0;
   int in = open(path, O_RDONLY);
   if (in < 0)
      return (errno == ENOENT) ? 0 : -1;
   static char buf[4096];
   for (;;) {
      long n = read(in, buf, sizeof buf);
      if (n < 0) {
         /* Same rule as write_all's: a read cut short by a signal has moved
          * nothing and is not a read error. Treated as one, a restore would
          * abandon a perfectly good copy. */
         if (errno == EINTR)
            continue;
         close(in);
         return -1;
      }
      if (n == 0)
         break;
      if (write_all(fd, buf, (int)n) != 0) {
         close(in);
         return -1;
      }
      if (copied)
         *copied += n;
   }
   /* The result of close() on a READ handle carries no data, so unlike the
    * staged file's close (replace_finish's job) there is nothing to check. */
   close(in);
   return 0;
}

/* The body, so the many early returns inside it do not each have to remember
 * to release the operation lock -- and so none of them RETURNS while holding
 * it, which app/test/lockorder.py is right to refuse. */
static int sync_restore_inner(const struct sync_ctx *sx)
{
   if (!sx->have_key || !sx->http)
      return -1;
   int restored = 0;
   int unsynced = 0;
   /* THE SNAPSHOT, not the live registry -- and here it is load-bearing in a
    * way it was not when this only appended. syncjni_register_logs() is
    * sync_clear_logs() plus five sync_add_log() calls, so g_log[] is rewritten
    * field by field while an operation may be inside it; a path read out of
    * that rewrite is a TORN path, and the publish below RENAMES over whatever
    * it names. Every other loop in this file has the same shape and the same
    * reason (see sync_ctx_begin); this one is the one that writes. */
   for (int li = 0; li < sx->nlog; li++) {
      const struct sync_log *l = &sx->log[li];
      char path[128];
      if (!sync_path_digest(path, sizeof path, l->name))
         return -1;
      if (signed_req("GET", path, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
         return -1;
      digest_fault(g_rsp);

      /* What the server has. */
      static long rb[SYNC_REMOTE_BUCKETS];
      static char rh[SYNC_REMOTE_BUCKETS][17];
      int nrb       = 0;
      const char *q = g_rsp;
      char nm[40];
      long cnt = 0;
      char hh[17];
      /* THE WHOLE REPLY, BEFORE ANY LOCAL FILE IS TOUCHED. This loop runs to
       * completion above every write below, so a reply that is damaged
       * anywhere refuses the restore for this log outright rather than
       * pulling back the prefix it managed to read and reporting a count. */
      for (;;) {
         enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
         if (d == DLINE_END)
            break;
         if (d == DLINE_BAD)
            return -1;
         if (nrb >= (int)(sizeof rb / sizeof rb[0]))
            return -1;
         long b = 0;
         if (!digest_num(nm, nm + s_len(nm), &b))
            return -1;
         /* THE HASH IS KEPT, and that is the whole of item 114. This loop
          * used to parse the per-bucket hash and drop it on the floor, so the
          * only question the restore could ask was "does this bucket number
          * exist locally?" -- and a bucket number is not evidence that the
          * bucket is COMPLETE. */
         rb[nrb] = b;
         memcpy(rh[nrb], hh, 17);
         nrb++;
      }

      /* What we have. A read error here must not read as "we have nothing":
       * that would pull the whole record back on top of a log we simply could
       * not open, duplicating every row. */
      static long lb[SYNC_LOCAL_BUCKETS];
      int nlb = log_buckets(l, lb, (int)(sizeof lb / sizeof lb[0]));
      if (nlb < 0)
         return -1;

      /* WHICH REMOTE BUCKETS THIS PHONE IS SHORT OF -- and "short of" is
       * decided by the CANONICAL HASH, not by whether the bucket number
       * happens to appear locally.
       *
       * THE DEFECT THIS REPLACES. The test used to be `lb[k] == rb[r]`: a
       * remote bucket was skipped the moment a bucket of that number existed
       * here, however few rows it held. So a TORN bucket -- the right day,
       * with rows missing because a write was cut short, a copy was
       * interrupted, or an older restore stopped halfway -- was never
       * repaired. It could not be: the one signal that would have shown the
       * damage, the hash the server had just sent, was parsed and thrown
       * away.
       *
       * And a torn bucket is not a stable state. It is exactly the local
       * state that the NEXT sync PUTs over the server's complete copy, the
       * phone being authoritative: the rows missing here get deleted there,
       * and the only copy that still had them is gone. A restore that
       * "succeeded" while leaving a short bucket had armed the loss.
       *
       * A hash that differs means the two sides hold different SETS. It does
       * not say which way, and does not have to: the merge below only ever
       * adds, so a bucket where this phone holds MORE costs one fetch that
       * contributes nothing. That is the right side to be wrong on.
       *
       * ONE PASS OVER THE LOG PER SHARED BUCKET. Restore is a one-shot that
       * already spends an HTTP round trip per bucket it fetches; the periodic
       * sync path is untouched and still walks its buckets in windows. */
      static unsigned char need[SYNC_REMOTE_BUCKETS];
      int missing = 0;
      for (int r = 0; r < nrb; r++) {
         char mine[17];
         if (!want_has(lb, nlb, rb[r]))
            need[r] = 1; /* not here at all: the case that always worked */
         else if (local_bucket_hash(l, rb[r], mine) != 0)
            return -1; /* cannot tell what we hold: not a restore, a refusal */
         else
            need[r] = strncmp(mine, rh[r], 16) != 0;
         missing |= need[r];
      }
      /* Asked before anything is staged, so a log that is already complete is
       * not rewritten at all -- a rewrite that changes nothing still spends
       * the flash write, and on this hardware that is the cost worth
       * avoiding. */
      if (!missing)
         continue;

      /* ---- STAGED, THEN PUBLISHED ONCE --------------------------------
       *
       * This used to open the live log O_APPEND and write each bucket
       * straight into it, with the result of close() discarded, no fsync
       * anywhere, and a comment admitting that a short write leaves a torn
       * row -- then returning -1 with the torn row still in the file. Every
       * later load of that log reads the truncated row as damage, for good,
       * because these logs are append-only and never rewritten.
       *
       * Three separate holes, and the middle one is the worst:
       *
       *   - a short write tore a row IN THE LIVE FILE, and the refusal that
       *     followed could not undo it;
       *   - close() can fail (the buffered write reaching a full or dying
       *     card is reported HERE, not by write), and its result was dropped,
       *     so a bucket could be reported restored having never landed;
       *   - nothing was fsynced, so a power loss within the writeback window
       *     lost rows the user had been told were restored.
       *
       * Now: the live log is copied into a staging file, every missing
       * bucket is appended to THAT, and the result is renamed over the
       * original only once all of it is written. A failure anywhere leaves
       * the original untouched, byte for byte. */
      /* ".restore", NOT ".new" -- and the difference is a corruption bug.
       *
       * log_append() in app/util.c stages the FIRST record of a log in
       * "<path>.new" (the header and the row go in together, atomically), and
       * every log registered here is one of the four it appends to. A reading
       * arriving from the CGM while this runs would then open, TRUNCATE and
       * rename the very file this restore is building -- and log_append takes
       * that path exactly when the log is missing or empty, which is the state
       * a restore happens in. The two writers would trade one inode: one of
       * them renames it into place while the other is still writing at its own
       * offset, and the live log ends up holding both writers' bytes on top of
       * each other, with this function reporting the failure its rename got
       * and the original "untouched" promise broken.
       *
       * A suffix of its own costs nothing and cannot collide with either
       * log_append's ".new" or atomic_replace's ".tmp". The buffer is sized
       * for the longest path sync_add_log accepts plus the suffix, so a deep
       * data directory refuses nothing. */
      char tmp[320];
      int tn = snprintf(tmp, sizeof tmp, "%s.restore", l->path);
      if (tn < 0 || tn >= (int)sizeof tmp)
         return -1; /* a truncated staging path would replace the wrong file */
      int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
         return -1;
      long copied = 0;
      if (copy_into(fd, l->path, &copied) != 0) {
         close(fd);
         unlink(tmp);
         return -1;
      }
      int added = 0;
      int ok    = 1;
      for (int r = 0; r < nrb && ok; r++) {
         if (!need[r])
            continue;
         char p2[128];
         if (!sync_path_bucket(p2, sizeof p2, l->name, rb[r])) {
            ok = 0;
            continue;
         }
         if (signed_req("GET", p2, "", 0, g_rsp, SYNC_BUF_MAX) != 200) {
            ok = 0;
            continue;
         }
         restore_body_fault(g_rsp);
         long n = s_len(g_rsp);
         if (n <= 0)
            continue; /* the server holds the bucket but it is empty */
         /* A REPLY THAT FILLED THE BUFFER IS THE FRONT OF A BUCKET, NOT A
          * BUCKET. The wire allows a body of WV_LIMIT_BODY_MAX (512 kB) and
          * this phone holds SYNC_BUF_MAX (256 kB), so the server may legally
          * hold a bucket that does not fit here -- and lib/wirevec.h's rule for
          * exactly that case is that the smaller implementation DECLINES, never
          * truncates. A body cut off at the buffer's last byte ends mid-row,
          * and writing it into the staging file publishes a torn row as a
          * successful restore, which is the one outcome this whole path was
          * rewritten to make impossible. The production transport already
          * refuses an oversized reply (see jni_http), but the refusal belongs
          * where the bytes are written too: a transport is a hook, and the
          * interop harness's own socket truncates. */
         if (n >= SYNC_BUF_MAX - 1) {
            ok = 0;
            continue;
         }
         /* WHAT ARRIVED IS WHAT THE DIGEST PROMISED, or it is not used.
          *
          * The whole decision above rests on the hash the server sent in its
          * digest; fetching the body and merging it unchecked would move the
          * trust from that hash to the wire, which is where it is least
          * deserved. A body that lost bytes in transit, arrived from the
          * wrong bucket, or was rewritten by anything between here and the
          * server hashes differently -- and the rows this restore is about to
          * write into the user's log are exactly the rows nobody else still
          * has. Item 40 settled the same point for the digest parser: damage
          * is refused, never interpreted.
          *
          * Refusing here fails the whole restore (ok = 0), which is the
          * standing rule of this function: all of it or none of it. */
         char got[17];
         hash16(g_rsp, n, got);
         if (strncmp(got, rh[r], 16) != 0) {
            ok = 0;
            continue;
         }
         /* MERGE, WHICH IS A UNION AND NEVER A REPLACEMENT.
          *
          * The staging file already holds this phone's copy of the log,
          * byte for byte, so every local row is in it before this line runs.
          * Only the rows the server sent that this phone does NOT hold are
          * appended -- so a row BOTH sides have is written once (a duplicate
          * line is real bytes on the user's disk, and slots.csv is read by a
          * loader that does not deduplicate); a row ONLY THE SERVER has is
          * the repair; and a row ONLY THIS PHONE has is untouched, which is
          * the direction most likely to go wrong -- a merge that REPLACED the
          * bucket would delete readings the phone had and the server had not
          * been told about, the loss item 112 is about arriving through the
          * fix.
          *
          * The kept lines are compacted to the front of g_rsp and written in
          * ONE call, both because it is fewer syscalls and because the
          * per-bucket failure cases below are counted in write_all calls. */
         long one = rb[r];
         int nrow = log_scan(l, &one, 1, NULL);
         if (nrow < 0) {
            ok = 0; /* cannot tell what we hold: appending blind duplicates */
            continue;
         }
         long keep     = 0;
         const char *s = g_rsp;
         while (*s) {
            const char *e = s_chr(s, '\n');
            long ln       = e ? (long)(e - s) : s_len(s);
            int held      = 0;
            for (int i = 0; i < nrow && !held; i++)
               held = g_row[i].len == (size_t)ln &&
                      memcmp(g_buf + g_row[i].off, s, (size_t)ln) == 0;
            if (!held) {
               /* MEMMOVE, NOT MEMCPY: `keep` only ever trails `s`, so the
                * ranges are disjoint or dst-below-src -- but they DO touch
                * (with nothing dropped yet the two are the same address), and
                * "may overlap" is the case memcpy's contract calls undefined
                * rather than merely unlikely. */
               memmove(g_rsp + keep, s, (size_t)ln);
               keep += ln;
               g_rsp[keep++] = '\n';
            }
            s = e ? e + 1 : s + ln;
         }
         if (keep == 0)
            continue; /* the phone already held every row of it */
         if (write_all(fd, g_rsp, (int)keep) != 0) {
            ok = 0;
            continue;
         }
         added++;
      }
      /* ALL OF IT OR NONE OF IT. A bucket that could not be fetched or
       * written is one this restore promised and does not have, and
       * publishing the staged file anyway would report a restore that is
       * short by exactly the rows the user asked for. The original is
       * untouched, so the answer is simply "no". */
      if (!ok) {
         close(fd);
         unlink(tmp);
         return -1;
      }
      /* AND NOTHING TO PUBLISH IS NOT SOMETHING TO PUBLISH. A bucket can
       * differ from the server's because this phone holds MORE of it, not
       * less -- readings logged since the last push are the ordinary case --
       * and the union then adds nothing. Renaming a byte-identical copy over
       * the log would spend a flash write, take the file's identity with it,
       * and hand every later restore the same non-work to do again. */
      if (added == 0) {
         close(fd);
         unlink(tmp);
         continue;
      }
      /* A ROW LANDING RIGHT HERE is the case the catch-up below exists for,
       * and it cannot be scheduled from a single-threaded test: the append has
       * to happen after the snapshot and before the publish. APP_RESTORE_APPEND
       * puts one there on demand, so the catch-up is exercised rather than
       * argued for. Compiled out of the phone build entirely. */
      restore_append_fault(l->path);

      /* ---- WHATEVER ARRIVED WHILE WE WERE DOWNLOADING ------------------
       *
       * A REGRESSION THE STAGED DESIGN INTRODUCES, and it has to be paid for
       * here. The copy above is a snapshot; between it and the rename below
       * there is one GET per missing bucket, which for a year of history is
       * hundreds of round trips and minutes of wall clock. A CGM reading lands
       * every five minutes. Any row appended in that window is in the original
       * and NOT in the staging file, and the rename would delete it -- a
       * reading the app already acknowledged as stored and drew on screen.
       *
       * The old append-in-place restore could not lose a row that way, so
       * shipping the staged version without this would be trading a durability
       * bug for a data-loss one.
       *
       * So the original's tail is re-read from where the copy stopped and
       * appended. `copied` is a byte offset into an append-only file, which is
       * exactly what makes this sound: bytes before it never change.
       *
       * NOT AIRTIGHT, and the residual is stated rather than implied: a row
       * appended between this read and the rename is still lost. That window
       * is two syscalls wide instead of minutes. Closing it entirely means
       * holding util.c's append lock across the publish, which would have to
       * be exported and would block a reading on a rename. */
      {
         long extra = 0;
         int tailok = 1;
         int in     = open(l->path, O_RDONLY);
         if (in >= 0) {
            if (lseek(in, copied, SEEK_SET) != copied) {
               tailok = 0;
            } else {
               static char tb[4096];
               for (;;) {
                  long n = read(in, tb, sizeof tb);
                  if (n < 0) {
                     if (errno == EINTR)
                        continue;
                     tailok = 0;
                     break;
                  }
                  if (n == 0)
                     break;
                  if (write_all(fd, tb, (int)n) != 0) {
                     tailok = 0;
                     break;
                  }
                  extra += n;
               }
            }
            close(in);
         } else if (errno != ENOENT) {
            tailok = 0;
         }
         if (!tailok) {
            close(fd);
            unlink(tmp);
            return -1; /* better no restore than a restore that deletes rows */
         }
         /* Not logged: sync.c is linked into host tests that supply no log
          * sink. The kept bytes are visible in the file, which is the claim
          * that matters. */
         (void)extra;
      }

      /* fsync, close (checked), rename, and fsync the directory. */
      enum replace_result rr = replace_finish(fd, tmp, l->path);
      if (rr == REPLACE_FAILED)
         return -1; /* it unlinked the staging file itself; see util.c */
      if (rr == REPLACE_UNSYNCED)
         unsynced = 1; /* published, durability unknown -- see below */
      restored += added;
   }
   /* CHANGED-BUT-UNCERTAIN IS ITS OWN ANSWER. The rows are in the file and
    * visible to the next load, but a post-rename fsync failed, so a power
    * loss now could still lose them. Reporting plain success would tell the
    * user their record is back when it may not be after a reboot; reporting
    * -1 would tell them nothing was restored when in fact everything was,
    * and invite them to run it again. */
   if (unsynced)
      return SYNC_RESTORE_UNSYNCED;
   return restored;
}

int sync_restore(void)
{
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   int rc = sync_restore_inner(&sx);
   sync_ctx_end();
   return rc;
}

static int sync_run_inner(void)
{
   /* Straight to the per-log work. There used to be a top-level GET /v1/digest
    * here, under a comment claiming it short-circuited an unchanged sync in
    * one request -- but its answer was parsed into two arrays and then thrown
    * away, so it was a round trip that decided nothing. The real "is there
    * anything to do" question is answered before we ever open a socket, by
    * the file sizes (see syncjni_state_stamp), and each log's digest is
    * fetched below anyway. */
   for (int i = 0; i < g_nlog; i++)
      if (sync_one_log(i) != 0)
         return -1;

   /* The re-check is what turns "we pushed some rows" into the guarantee: if
    * the two digests do not agree now, the sync failed, whatever it sent. */
   if (signed_req("GET", "/v1/digest", "", 0, g_rsp, SYNC_BUF_MAX) != 200)
      return -1;
   char nm[40];
   char hh[17];
   long cnt      = 0;
   const char *q = g_rsp;
   /* Which of OUR logs the server accounted for. The loop below walks the
    * server's answer, so on its own it can only ever prove things about logs
    * the server chose to mention -- see the end of this function. */
   int seen[SYNC_MAX_LOGS];
   for (int i = 0; i < SYNC_MAX_LOGS; i++)
      seen[i] = 0;
   int nlines = 0;
   static long lb[SYNC_LOCAL_BUCKETS];
   for (;;) {
      enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
      if (d == DLINE_END)
         break;
      /* Here the name is a LOG name, not a number, so it is validated by the
       * strcmp against g_log below rather than by digest_num -- but the line's
       * framing and its hash width are checked the same way, and a damaged
       * reply must not read as "the server holds fewer logs than it does". */
      if (d == DLINE_BAD)
         return -1;
      /* A reply cannot describe more logs than exist. Each iteration below
       * costs a full pass over a log, so an answer with a thousand lines --
       * confused server, mangled response, hostile middlebox -- would put the
       * phone into a scan loop that reads the whole record over and over. */
      if (++nlines > SYNC_MAX_LOGS)
         return -1;
      int li = -1;
      for (int i = 0; i < g_nlog; i++)
         if (strcmp(g_log[i].name, nm) == 0) {
            li = i;
            break;
         }
      if (li < 0)
         return -1; /* the server holds a log we do not */
      seen[li] = 1;
      static char text[SYNC_BUF_MAX];
      /* Fold this log's buckets exactly as the server does: SHA-256 over
       * "<bucket> <hash>\n" lines in ascending bucket order.
       *
       * TWO THINGS THIS USED TO DO WRONG, both invisible until the log got
       * long.
       *
       * It called sync_bucket_text once per bucket, and each of those is a
       * FULL PASS over the file -- so the verify cost days x filesize. At one
       * CGM that is roughly 1.6 GB read per sync in the first year and 41 GB
       * by the fifth, on a phone, as often as once a minute. It now walks the
       * buckets in the same WINDOWS sync_one_log uses: one pass per 128
       * buckets rather than one pass per bucket.
       *
       * And it accumulated the fold into a fixed 128 kB buffer, ~23 bytes per
       * bucket, so at about 5,700 buckets -- fifteen and a half years -- every
       * sync would have begun failing with no way to tell why. The fold is a
       * SHA-256 of a byte stream, so it can be fed incrementally and needs no
       * buffer at all. */
      int nlb = log_buckets(&g_log[li], lb, (int)(sizeof lb / sizeof lb[0]));
      if (nlb < 0) /* cannot enumerate: a failed verify, not an empty log */
         return -1;
      struct sha256_ctx fold;
      sha256_init(&fold);
      size_t nb_all = (size_t)nlb;
      size_t at     = 0;
      while (at < nb_all) {
         size_t span = nb_all - at;
         if (span > 128)
            span = 128;
         int nrow     = 0;
         int overflow = 0;
         for (;;) {
            nrow = log_scan(&g_log[li], lb + at, (int)span, &overflow);
            if (nrow >= 0)
               break;
            if (!overflow || span == 1)
               return -1;
            span /= 2;
         }
         for (size_t k = at; k < at + span; k++) {
            long tl = bucket_text(g_row, nrow, 1, lb[k], g_log[li].bucketed,
                                  text, sizeof text);
            if (tl < 0)
               return -1;
            size_t tn = (size_t)tl;
            char bh2[17];
            hash16(text, (long)tn, bh2);
            char line[40];
            int w = snprintf(line, sizeof line, "%ld %s\n", lb[k], bh2);
            if (w <= 0 || w >= (int)sizeof line)
               return -1;
            sha256_update(&fold, (const uint8_t *)line, (size_t)w);
         }
         at += span;
      }
      uint8_t fd32[32];
      sha256_final(&fold, fd32);
      char mine[17];
      static const char hx2[] = "0123456789abcdef";
      for (size_t i = 0; i < 8; i++) {
         mine[2 * i]       = hx2[fd32[i] >> 4U];
         mine[(2 * i) + 1] = hx2[fd32[i] & 15U];
      }
      mine[16] = 0;
      if (strncmp(mine, hh, 16) != 0)
         return -1;
   }

   /* THE OTHER HALF OF THE GUARANTEE.
    *
    * Everything above is driven by the server's answer, so it can only prove
    * that the logs the SERVER named match. A reply that parsed to zero lines
    * -- an empty body, a truncated one (see the outcap note in syncjni), a
    * response the transport mangled -- ran this loop zero times and reached
    * the end having verified nothing at all. The old code then ended with
    *
    *     return nsame >= 0 ? 0 : -1;
    *
    * where that counter started at 0 and only ever incremented, so it was an
    * unconditional `return 0`: sync_run() reported "both sides provably hold
    * the same bytes" without having compared anything. sync.h promises that
    * sentence; this is what has to be true for it.
    *
    * So: every log of ours that holds a single row must have appeared. The
    * enumeration below costs one pass per log the server did NOT mention,
    * which in the normal case is only the logs that are empty -- and an empty
    * or missing log answers immediately without reading anything. */
   for (int i = 0; i < g_nlog; i++) {
      if (seen[i])
         continue;
      int n = log_buckets(&g_log[i], lb, (int)(sizeof lb / sizeof lb[0]));
      if (n < 0)
         return -1; /* cannot tell whether it holds data: not a pass */
      if (n > 0)
         return -1; /* we hold days the server never accounted for */
   }
   /* Reaching here after a response that named nothing now means something
    * specific and checked: every log we hold is genuinely empty, proved by
    * the enumeration above, rather than merely unmentioned. */
   return 0;
}

/* The entry point, and the only place the progress counters are armed and
 * disarmed -- so a failure anywhere inside still clears the progress bar. */
int sync_run(void)
{
   /* THE OPERATION OWNS THE WORKSPACE FOR ITS WHOLE DURATION, and reads a
    * configuration that cannot change under it. Both come from the context. */
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   if (!sx.have_key || !sx.http) {
      sync_ctx_end();
      return -1;
   }
   atomic_store_explicit(&g_pdone, 0, memory_order_relaxed);
   atomic_store_explicit(&g_ptotal, count_buckets(), memory_order_relaxed);
   /* RELEASE last: the two counters above must be visible to any reader that
    * sees the run as active, or the first frame of the bar divides by zero. */
   atomic_store_explicit(&g_pactive, 1, memory_order_release);
   int rc = sync_run_inner();
   atomic_store_explicit(&g_pactive, 0, memory_order_release);
   sync_ctx_end();
   return rc;
}
