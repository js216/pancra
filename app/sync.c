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
#include "dexlibc.h"
/* dexlibc.h already resolves errno for both builds; naming it here as well is
 * for the reader and the linter, which both want the header that provides a
 * symbol to be visible at the point of use. */
#if __STDC_HOSTED__
#include <errno.h>
#endif
#include "hmac.h"
#include "jpake.h"
#include "sha256.h"
#include "util.h"
#include <stdint.h> /* uint8_t: the key, the hashes, the raw bytes */
#include <stdio.h>  /* snprintf, SEEK_END */
#include <stdlib.h> /* realloc: the log buffer grows to the file */
#include <string.h> /* memcpy, memcmp, strcmp: rows are bytes, not strings */

struct sync_log {
   char name[40];
   char path[256];
   int bucketed;
};

static struct sync_log g_log[SYNC_MAX_LOGS];
static int g_nlog;
static sync_http_fn g_http;
static long g_uid;
static uint8_t g_key[SYNC_KEY_LEN];
static int g_have_key;

/* Row index into g_buf: offset and length, so sorting moves 16 bytes rather
 * than a line. Unsigned because neither an offset into a buffer nor the
 * length of a line can be negative -- and while that is obvious to a reader,
 * a signed pair lets `buf + off` and `text + n` be provably out of bounds to
 * anything checking, which they then duly report. */
struct row {
   size_t off;
   size_t len;
};

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

void sync_set_http(sync_http_fn fn)
{
   g_http = fn;
}

void sync_set_key(long uid, const uint8_t key[SYNC_KEY_LEN])
{
   g_uid = uid;
   for (int i = 0; i < SYNC_KEY_LEN; i++)
      g_key[i] = key[i];
   g_have_key = uid > 0;
}

void sync_clear_logs(void)
{
   g_nlog = 0;
}

int sync_add_log(const char *name, const char *path, int bucketed)
{
   if (g_nlog >= SYNC_MAX_LOGS)
      return -1;
   struct sync_log *l = &g_log[g_nlog];
   int n              = snprintf(l->name, sizeof l->name, "%s", name);
   if (n <= 0 || n >= (int)sizeof l->name)
      return -1;
   n = snprintf(l->path, sizeof l->path, "%s", path);
   if (n <= 0 || n >= (int)sizeof l->path)
      return -1;
   l->bucketed = bucketed;
   g_nlog++;
   return 0;
}

/* ---- hex, hashing ---------------------------------------------------- */

/* strlen/strchr are not in the freestanding shim, and adding them there would
 * mean adding symbols to stub_c.c for two one-line loops. */
static long s_len(const char *p)
{
   long n = 0;
   while (p[n])
      n++;
   return n;
}

static const char *s_chr(const char *p, char c)
{
   for (; *p; p++)
      if (*p == c)
         return p;
   return 0;
}

static const char hexd[] = "0123456789abcdef";

static void hexify(const uint8_t *in, int n, char *out)
{
   for (size_t i = 0; i < (size_t)n; i++) {
      out[2 * i]       = hexd[in[i] >> 4U];
      out[(2 * i) + 1] = hexd[in[i] & 15U];
   }
   out[2 * (size_t)n] = '\0';
}

static int unhex(const char *in, int hexchars, uint8_t *out)
{
   for (int i = 0; i < hexchars; i += 2) {
      unsigned hi = 16;
      unsigned lo = 16;
      for (unsigned k = 0; k < 16; k++) {
         if (((unsigned)in[i] | 0x20U) == (unsigned char)hexd[k])
            hi = k;
         if (((unsigned)in[i + 1] | 0x20U) == (unsigned char)hexd[k])
            lo = k;
      }
      if (hi > 15 || lo > 15)
         return 0;
      out[i / 2] = (uint8_t)((hi << 4U) | lo);
   }
   return 1;
}

/* The protocol's hash: first 16 hex chars of SHA-256. */
static void hash16(const char *data, long len, char out[17])
{
   uint8_t h[32];
   char hex[65];
   sha256((const uint8_t *)data, (size_t)len, h);
   hexify(h, 32, hex);
   for (int i = 0; i < 16; i++)
      out[i] = hex[i];
   out[16] = '\0';
}

/* The app's own HMAC used to live here. It truncated the message at
 * SYNC_BUF_MAX, so it was a MAC over a PREFIX for anything longer -- while
 * lib/hmac.c pre-hashed past 512 bytes, which is a third function again. Both
 * ends of this protocol have to compute the same MAC, so there is one
 * implementation now (lib/hmac.c, streamed, pinned to the RFC 4231 vectors)
 * and this file calls it. */

/* ---- reading a log into rows ----------------------------------------- */

static int row_lt(const char *base, const struct row *a, const struct row *b)
{
   const unsigned char *x = (const unsigned char *)base + a->off;
   const unsigned char *y = (const unsigned char *)base + b->off;
   size_t n               = a->len < b->len ? a->len : b->len;
   for (size_t i = 0; i < n; i++) {
      if (x[i] != y[i])
         return x[i] < y[i];
   }
   return a->len < b->len;
}

/* Insertion sort: buckets are a day of rows, and this runs once per bucket
 * that actually differs, so the simple thing is the right thing. */
static void row_sort(const char *base, struct row *r, int n)
{
   for (int i = 1; i < n; i++) {
      struct row t = r[i];
      int j        = i - 1;
      while (j >= 0 && row_lt(base, &t, &r[j])) {
         r[j + 1] = r[j];
         j--;
      }
      r[j + 1] = t;
   }
}

/* A row is one line of text and nothing else. Anything that could make the
 * two sides disagree about the bytes is skipped rather than cleaned up: a
 * trimmed trailing space would be a row whose hash we could never reproduce. */
static int row_ok(const char *p, int len)
{
   if (len <= 0 || len > SYNC_ROW_MAX)
      return 0;
   if (p[len - 1] == ' ' || p[len - 1] == '\t')
      return 0;
   for (int i = 0; i < len; i++)
      if ((unsigned char)p[i] < 0x20 && p[i] != '\t')
         return 0;
   return 1;
}

/* The row's bucket: its leading decimal field divided into UTC days. A row
 * with no leading number (a '#' header) lands in bucket 0, which is stable on
 * both sides and therefore harmless. */
static long row_bucket(const char *p, size_t len, int bucketed)
{
   if (!bucketed)
      return 0;
   long v   = 0;
   size_t i = 0;
   while (i < len && p[i] >= '0' && p[i] <= '9') {
      if (i < 18)
         v = (v * 10) + (p[i] - '0');
      i++;
   }
   return v / 86400;
}

/* Stream a log, keeping only the rows whose bucket is in `want` (or every row
 * when `want` is NULL, which is only used to enumerate buckets). Returns the
 * row count, or -1 if even the window did not fit. A missing file is empty.
 *
 * The file is read in chunks and lines are copied out of the chunk, so the
 * memory held is the WINDOW's rows -- never the file. */
static int want_has(const long *want, int nwant, long b)
{
   for (int i = 0; i < nwant; i++)
      if (want[i] == b)
         return 1;
   return 0;
}

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
         if (over) {
            over = 0;
            continue;
         }
         if (!row_ok(line, len))
            continue;
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
   /* a final line with no newline */
   if (llen > 0 && !over) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (row_ok(line, len)) {
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
         if (over) {
            over = 0;
            continue;
         }
         if (!row_ok(line, len))
            continue;
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
   if (llen > 0 && !over) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (row_ok(line, len)) {
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
   row_sort(g_buf, g_row, nb);
   size_t k    = 0;
   size_t room = (size_t)cap;
   for (int i = 0; i < nb; i++) {
      /* A bucket is a SET: an identical row written twice is one row, which
       * is what the server's primary key enforces on its side. */
      if (i && g_row[i].len == g_row[i - 1].len &&
          !memcmp(g_buf + g_row[i].off, g_buf + g_row[i - 1].off, g_row[i].len))
         continue;
      if (g_row[i].len + 1 > room - k)
         return -1;
      memcpy(out + k, g_buf + g_row[i].off, g_row[i].len);
      k += g_row[i].len;
      out[k++] = '\n';
   }
   return (long)k;
}

/* ---- signed requests -------------------------------------------------- */

static volatile int g_pdone, g_ptotal, g_pactive;

int sync_progress(int *done, int *total)
{
   if (done)
      *done = g_pdone;
   if (total)
      *total = g_ptotal;
   return g_pactive;
}

/* Every bucket this client holds, across every log: the denominator of the
 * progress the UI draws. Counted up front, in one pass per log, because a
 * bar that discovers its own length as it goes is not a bar. */
static int count_buckets(void)
{
   static long bl[8192];
   int total = 0;
   for (int i = 0; i < g_nlog; i++) {
      int n = log_buckets(&g_log[i], bl, (int)(sizeof bl / sizeof bl[0]));
      if (n > 0) /* a log we cannot enumerate contributes nothing to a count */
         total += n;
   }
   return total;
}

static long g_nonce_seq;

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
   /* Unique per request without needing entropy per call: the counter cannot
    * repeat within a run, and the timestamp cannot repeat across runs. */
   (void)snprintf(nonce, sizeof nonce, "p%ld-%ld", realtime_s(), ++g_nonce_seq);

   static char msg[1400];
   long ts = realtime_s();
   int n   = snprintf(msg, sizeof msg, "%s\n%s\n%ld\n%s\n%s", method, path, ts,
                      nonce, bhex);
   if (n <= 0 || n >= (int)sizeof msg)
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

static void confirm_of(const uint8_t key[SYNC_KEY_LEN], const char *label,
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

int sync_pair(const char *email, const char *code,
              uint8_t out_key[SYNC_KEY_LEN], long *out_uid)
{
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
      confirm_of(key, "pancra-confirm-server", want);
      nl = s_chr(g_rsp, '\n');
      if (!nl || strncmp(nl + 1, want, 32) != 0)
         break;

      char mine[33];
      confirm_of(key, "pancra-confirm-client", mine);
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

/* ---- the sync algorithm ----------------------------------------------- */

/* One "<name> <count> <hash>" line of a digest reply. */
static int digest_line(const char **p, char *name, int ncap, long *count,
                       char hash[17])
{
   const char *q = *p;
   if (!*q)
      return 0;
   int k = 0;
   while (*q && *q != ' ' && *q != '\n' && k < ncap - 1)
      name[k++] = *q++;
   name[k] = '\0';
   if (*q != ' ')
      return 0;
   q++;
   long c = 0;
   while (*q >= '0' && *q <= '9')
      c = (c * 10) + (*q++ - '0');
   *count = c;
   if (*q != ' ')
      return 0;
   q++;
   k = 0;
   while (*q && *q != '\n' && k < 16)
      hash[k++] = *q++;
   hash[k] = '\0';
   while (*q && *q != '\n')
      q++;
   if (*q == '\n')
      q++;
   *p = q;
   return 1;
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
   int pn = snprintf(path, sizeof path, "/v1/digest/%s", l->name);
   if (pn <= 0 || pn >= (int)sizeof path)
      return -1;
   if (signed_req("GET", path, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
      return -1;

   /* Remote buckets, so we can spot ones we no longer hold. */
   static long rb[4096];
   static char rh[4096][17];
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
    * nothing to say why. That is ~11 years of daily buckets away, and the
    * local enumeration hard-fails at ~22, so the two ends disagree about what
    * a full log even is. Neither is a decision anyone made.
    *
    * Refusing is the same choice log_buckets already makes for the local
    * side, and for the same reason: an incomplete picture of what the server
    * holds drives a loop that deletes. */
   while (digest_line(&q, nm, sizeof nm, &cnt, hh)) {
      if (nrb >= (int)(sizeof rb / sizeof rb[0]))
         return -1;
      long b = 0;
      for (const char *r = nm; *r >= '0' && *r <= '9'; r++)
         b = (b * 10) + (*r - '0');
      rb[nrb] = b;
      memcpy(rh[nrb], hh, 17);
      nrb++;
   }

   /* Local buckets: enumerated by streaming, so this costs one pass and one
    * long per day rather than the whole log in memory. */
   static long lb[8192];
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
    * direction) -- the sync stays broken rather than the backup. */
   if (nlb == 0 && nrb > 0)
      return -1;

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
         int nb = 0;
         for (int i = 0; i < nrow; i++)
            if (row_bucket(g_buf + g_row[i].off, g_row[i].len, l->bucketed) ==
                lb[k])
               g_sel[nb++] = g_row[i];
         row_sort(g_buf, g_sel, nb);
         size_t tn = 0;
         for (int i = 0; i < nb; i++) {
            if (i && g_sel[i].len == g_sel[i - 1].len &&
                !memcmp(g_buf + g_sel[i].off, g_buf + g_sel[i - 1].off,
                        g_sel[i].len))
               continue;
            if (g_sel[i].len + 1 > sizeof text - tn)
               return -1;
            memcpy(text + tn, g_buf + g_sel[i].off, g_sel[i].len);
            tn += g_sel[i].len;
            text[tn++] = '\n';
         }
         char mine[17];
         hash16(text, (long)tn, mine);
         const char *theirs = 0;
         for (int rr = 0; rr < nrb; rr++)
            if (rb[rr] == lb[k]) {
               theirs = rh[rr];
               break;
            }
         if (theirs && strncmp(theirs, mine, 16) == 0) {
            g_pdone++;
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
         int pn2 =
             snprintf(path2, sizeof path2, "/v1/bucket/%s/%ld", l->name, lb[k]);
         if (pn2 <= 0 || pn2 >= (int)sizeof path2)
            return -1;
         if (signed_req("PUT", path2, text, (int)tn, g_rsp, SYNC_BUF_MAX) !=
             200)
            return -1;
         g_pdone++;
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
      int n2 = snprintf(p2, sizeof p2, "/v1/bucket/%s/%ld", l->name, rb[r]);
      if (n2 <= 0 || n2 >= (int)sizeof p2)
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
int sync_restore(void)
{
   if (!g_have_key || !g_http)
      return -1;
   int restored = 0;
   for (int li = 0; li < g_nlog; li++) {
      const struct sync_log *l = &g_log[li];
      char path[128];
      int pn = snprintf(path, sizeof path, "/v1/digest/%s", l->name);
      if (pn <= 0 || pn >= (int)sizeof path)
         return -1;
      if (signed_req("GET", path, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
         return -1;

      /* What the server has. */
      static long rb[4096];
      int nrb       = 0;
      const char *q = g_rsp;
      char nm[40];
      long cnt = 0;
      char hh[17];
      while (digest_line(&q, nm, sizeof nm, &cnt, hh)) {
         if (nrb >= (int)(sizeof rb / sizeof rb[0]))
            return -1;
         long b = 0;
         for (const char *r = nm; *r >= '0' && *r <= '9'; r++)
            b = (b * 10) + (*r - '0');
         rb[nrb++] = b;
      }

      /* What we have. A read error here must not read as "we have nothing":
       * that would pull the whole record back on top of a log we simply could
       * not open, duplicating every row. */
      static long lb[8192];
      int nlb = log_buckets(l, lb, (int)(sizeof lb / sizeof lb[0]));
      if (nlb < 0)
         return -1;

      for (int r = 0; r < nrb; r++) {
         int have = 0;
         for (int k = 0; k < nlb && !have; k++)
            if (lb[k] == rb[r])
               have = 1;
         if (have)
            continue;
         char p2[128];
         int n2 = snprintf(p2, sizeof p2, "/v1/bucket/%s/%ld", l->name, rb[r]);
         if (n2 <= 0 || n2 >= (int)sizeof p2)
            return -1;
         if (signed_req("GET", p2, "", 0, g_rsp, SYNC_BUF_MAX) != 200)
            return -1;
         long n = s_len(g_rsp);
         if (n <= 0)
            continue; /* the server holds the bucket but it is empty */
         int fd = open(l->path, O_WRONLY | O_CREAT | O_APPEND, 0600);
         if (fd < 0)
            return -1;
         long w = write(fd, g_rsp, (size_t)n);
         close(fd);
         if (w != n)
            return -1; /* a short write leaves a torn row: stop, do not guess */
         restored++;
      }
   }
   return restored;
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
   static long lb[8192];
   while (digest_line(&q, nm, sizeof nm, &cnt, hh)) {
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
            int nb = 0;
            for (int i = 0; i < nrow; i++)
               if (row_bucket(g_buf + g_row[i].off, g_row[i].len,
                              g_log[li].bucketed) == lb[k])
                  g_sel[nb++] = g_row[i];
            row_sort(g_buf, g_sel, nb);
            size_t tn = 0;
            for (int i = 0; i < nb; i++) {
               if (i && g_sel[i].len == g_sel[i - 1].len &&
                   !memcmp(g_buf + g_sel[i].off, g_buf + g_sel[i - 1].off,
                           g_sel[i].len))
                  continue;
               if (g_sel[i].len + 1 > sizeof text - tn)
                  return -1;
               memcpy(text + tn, g_buf + g_sel[i].off, g_sel[i].len);
               tn += g_sel[i].len;
               text[tn++] = '\n';
            }
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
   if (!g_have_key || !g_http)
      return -1;
   g_pdone   = 0;
   g_ptotal  = count_buckets();
   g_pactive = 1;
   int rc    = sync_run_inner();
   g_pactive = 0;
   return rc;
}
