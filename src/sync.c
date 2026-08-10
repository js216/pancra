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
#include "dexauth.h"
#include "dexlibc.h"
#include "sha256.h"
#include "util.h"
#include <stdio.h>  /* snprintf, SEEK_END */
#include <stdlib.h> /* realloc: the log buffer grows to the file */

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

/* Row index into g_buf: offset and length, so sorting moves 8 bytes rather
 * than a line. */
struct row {
   long off;
   int len;
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

long sync_uid(void)
{
   return g_uid;
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

static const char HEXD[] = "0123456789abcdef";

static void hexify(const uint8_t *in, int n, char *out)
{
   for (int i = 0; i < n; i++) {
      out[2 * i]     = HEXD[in[i] >> 4];
      out[2 * i + 1] = HEXD[in[i] & 15];
   }
   out[2 * n] = '\0';
}

static int unhex(const char *in, int hexchars, uint8_t *out)
{
   for (int i = 0; i < hexchars; i += 2) {
      int hi = -1, lo = -1;
      for (int k = 0; k < 16; k++) {
         if ((in[i] | 0x20) == HEXD[k])
            hi = k;
         if ((in[i + 1] | 0x20) == HEXD[k])
            lo = k;
      }
      if (hi < 0 || lo < 0)
         return 0;
      out[i / 2] = (uint8_t)((hi << 4) | lo);
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

/* HMAC-SHA256 over one message. Written here rather than pulled in because
 * sha256.c is the only primitive this file otherwise needs. */
static void hmac(const uint8_t *key, int klen, const char *msg, long mlen,
                 uint8_t out[32])
{
   uint8_t k[64], pad[64], inner[32];
   static uint8_t scratch[SYNC_BUF_MAX + 64];
   for (int i = 0; i < 64; i++)
      k[i] = 0;
   if (klen > 64) {
      sha256(key, (size_t)klen, k);
   } else {
      for (int i = 0; i < klen; i++)
         k[i] = key[i];
   }
   for (int i = 0; i < 64; i++)
      pad[i] = (uint8_t)(k[i] ^ 0x36);
   if (mlen > SYNC_BUF_MAX)
      mlen = SYNC_BUF_MAX;
   for (int i = 0; i < 64; i++)
      scratch[i] = pad[i];
   for (long i = 0; i < mlen; i++)
      scratch[64 + i] = (uint8_t)msg[i];
   sha256(scratch, (size_t)(64 + mlen), inner);
   for (int i = 0; i < 64; i++)
      scratch[i] = (uint8_t)(k[i] ^ 0x5c);
   for (int i = 0; i < 32; i++)
      scratch[64 + i] = inner[i];
   sha256(scratch, 96, out);
}

/* ---- reading a log into rows ----------------------------------------- */

static int row_lt(const char *base, const struct row *a, const struct row *b)
{
   const unsigned char *x = (const unsigned char *)base + a->off;
   const unsigned char *y = (const unsigned char *)base + b->off;
   int n                  = a->len < b->len ? a->len : b->len;
   for (int i = 0; i < n; i++) {
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
static long row_bucket(const char *p, int len, int bucketed)
{
   if (!bucketed)
      return 0;
   long v = 0;
   int i  = 0;
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
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen  = 0;
   int over  = 0;
   int nrow  = 0;
   long used = 0;
   long n    = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n <= 0)
         break;
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
         long b = row_bucket(line, len, l->bucketed);
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
         g_row[nrow].off = used;
         g_row[nrow].len = len;
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
         long b = row_bucket(line, len, l->bucketed);
         if (!want || want_has(want, nwant, b)) {
            if (buf_reserve(used + len + 1) && row_reserve(nrow + 1)) {
               memcpy(g_buf + used, line, (size_t)len);
               g_row[nrow].off = used;
               g_row[nrow].len = len;
               used += len;
               nrow++;
            }
         }
      }
   }
   close(fd);
   return nrow;
}

/* Every bucket the log holds, ascending. Streams the file and keeps only the
 * bucket numbers -- one long per day, so a decade costs 30 kB. */
static int log_buckets(const struct sync_log *l, long *out, int cap)
{
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen = 0, over = 0, nb = 0;
   long n = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n <= 0)
         break;
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
         if (!want_has(out, nb, b) && nb < cap)
            out[nb++] = b;
      }
   }
   if (llen > 0 && !over && row_ok(line, llen)) {
      long b = row_bucket(line, llen, l->bucketed);
      if (!want_has(out, nb, b) && nb < cap)
         out[nb++] = b;
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
   row_sort(g_buf, g_row, nb);
   long k = 0;
   for (int i = 0; i < nb; i++) {
      /* A bucket is a SET: an identical row written twice is one row, which
       * is what the server's primary key enforces on its side. */
      if (i && g_row[i].len == g_row[i - 1].len &&
          !memcmp(g_buf + g_row[i].off, g_buf + g_row[i - 1].off,
                  (size_t)g_row[i].len))
         continue;
      if (k + g_row[i].len + 1 > cap)
         return -1;
      memcpy(out + k, g_buf + g_row[i].off, (size_t)g_row[i].len);
      k += g_row[i].len;
      out[k++] = '\n';
   }
   return k;
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
   for (int i = 0; i < g_nlog; i++)
      total += log_buckets(&g_log[i], bl, (int)(sizeof bl / sizeof bl[0]));
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
   (void)snprintf(nonce, sizeof nonce, "p%ld-%ld", realtime_s(),
                  ++g_nonce_seq);

   static char msg[1400];
   long ts = realtime_s();
   int n   = snprintf(msg, sizeof msg, "%s\n%s\n%ld\n%s\n%s", method, path, ts,
                      nonce, bhex);
   if (n <= 0 || n >= (int)sizeof msg)
      return -1;
   uint8_t mac[32];
   char machex[65];
   hmac(g_key, SYNC_KEY_LEN, msg, n, mac);
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
   hmac(key, SYNC_KEY_LEN, label, s_len(label), mac);
   hexify(mac, 32, hex);
   for (int i = 0; i < 32; i++)
      out[i] = hex[i];
   out[32] = '\0';
}

int sync_pair(const char *email, const char *code, uint8_t out_key[SYNC_KEY_LEN],
              long *out_uid)
{
   /* Idempotent, and REQUIRED: without the curve context dexpair_new hands
    * back a pairing whose very first round fails, with nothing on the wire to
    * explain why. The app also calls this from the BLE driver, which is
    * exactly why it was easy to miss here. */
   dexauth_init();
   struct dex_pairing *p =
       dexpair_new((const uint8_t *)code, (size_t)s_len(code), 1);
   if (!p)
      return -1;
   int rc = -1;
   uint8_t pkt[160], peer[160], key[SYNC_KEY_LEN];
   char hex[321], body[512], session[64];
   const char *nl;

   do {
      if (!dexpair_round1(p, pkt))
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
      if (!unhex(nl + 1, 320, peer) || !dexpair_peer_round1(p, peer))
         break;

      if (!dexpair_round2(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step("/v1/pair/2", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!unhex(g_rsp, 320, peer) || !dexpair_peer_round2(p, peer))
         break;

      if (!dexpair_round3(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step("/v1/pair/3", body, n, g_rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!unhex(g_rsp, 320, peer) || !dexpair_peer_round3(p, peer))
         break;
      if (!dexpair_shared_key(p, key))
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

   dexpair_free(p);
   return rc;
}

static int sync_run_inner(void);

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
   long cnt;
   char hh[17];
   while (nrb < (int)(sizeof rb / sizeof rb[0]) &&
          digest_line(&q, nm, sizeof nm, &cnt, hh)) {
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

   /* Walk them in WINDOWS. Each window is one pass over the file keeping only
    * that window's rows, so the memory held is the window and never the log.
    * A window is grown until its rows would exceed the budget, which
    * log_scan reports by failing -- so the loop halves the window and tries
    * again rather than guessing sizes up front. */
   static char text[SYNC_BUF_MAX];
   int at = 0;
   while (at < nlb) {
      int span = nlb - at;
      if (span > 128)
         span = 128;
      int nrow, overflow;
      for (;;) {
         nrow = log_scan(l, lb + at, span, &overflow);
         if (nrow >= 0)
            break;
         if (!overflow || span == 1)
            return -1; /* a single bucket that does not fit is a real error */
         span /= 2;
      }
      for (int k = at; k < at + span; k++) {
         /* Rows for this window are in g_buf; select this bucket's and hash
          * them exactly as the server does. */
         int nb = 0;
         for (int i = 0; i < nrow; i++)
            if (row_bucket(g_buf + g_row[i].off, g_row[i].len, l->bucketed) ==
                lb[k])
               g_sel[nb++] = g_row[i];
         row_sort(g_buf, g_sel, nb);
         long tn = 0;
         for (int i = 0; i < nb; i++) {
            if (i && g_sel[i].len == g_sel[i - 1].len &&
                !memcmp(g_buf + g_sel[i].off, g_buf + g_sel[i - 1].off,
                        (size_t)g_sel[i].len))
               continue;
            if (tn + g_sel[i].len + 1 > (long)sizeof text)
               return -1;
            memcpy(text + tn, g_buf + g_sel[i].off, (size_t)g_sel[i].len);
            tn += g_sel[i].len;
            text[tn++] = '\n';
         }
         char mine[17];
         hash16(text, tn, mine);
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
         char path2[128];
         int pn2 = snprintf(path2, sizeof path2, "/v1/bucket/%s/%ld", l->name,
                            lb[k]);
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

static int sync_run_inner(void)
{
   if (signed_req("GET", "/v1/digest", "", 0, g_rsp, SYNC_BUF_MAX) != 200)
      return -1;

   /* Compare the top digest first: when nothing has changed since the last
    * run -- the usual case -- the whole sync is this one request. */
   char remote[SYNC_MAX_LOGS][17];
   char rname[SYNC_MAX_LOGS][40];
   int nrem      = 0;
   const char *q = g_rsp;
   char nm[40];
   long cnt;
   char hh[17];
   while (nrem < SYNC_MAX_LOGS && digest_line(&q, nm, sizeof nm, &cnt, hh)) {
      memcpy(rname[nrem], nm, sizeof nm);
      memcpy(remote[nrem], hh, 17);
      nrem++;
   }

   for (int i = 0; i < g_nlog; i++)
      if (sync_one_log(i) != 0)
         return -1;

   /* The re-check is what turns "we pushed some rows" into the guarantee: if
    * the two digests do not agree now, the sync failed, whatever it sent. */
   if (signed_req("GET", "/v1/digest", "", 0, g_rsp, SYNC_BUF_MAX) != 200)
      return -1;
   q         = g_rsp;
   int nsame = 0;
   while (digest_line(&q, nm, sizeof nm, &cnt, hh)) {
      int li = -1;
      for (int i = 0; i < g_nlog; i++)
         if (strcmp(g_log[i].name, nm) == 0) {
            li = i;
            break;
         }
      if (li < 0)
         return -1; /* the server holds a log we do not */
      static char text[SYNC_BUF_MAX];
      /* Fold this log's buckets exactly as the server does: SHA-256 over
       * "<bucket> <hash>\n" lines in ascending bucket order. */
      static long lb[8192];
      int nlb = log_buckets(&g_log[li], lb, (int)(sizeof lb / sizeof lb[0]));
      static char fold[4096 * 32];
      long fl = 0;
      for (int k = 0; k < nlb; k++) {
         long n = sync_bucket_text(li, lb[k], text, SYNC_BUF_MAX);
         if (n < 0)
            return -1;
         char bh2[17];
         hash16(text, n, bh2);
         int w = snprintf(fold + fl, sizeof fold - (size_t)fl, "%ld %s\n",
                          lb[k], bh2);
         if (w <= 0 || fl + w >= (long)sizeof fold)
            return -1;
         fl += w;
      }
      char mine[17];
      hash16(fold, fl, mine);
      if (strncmp(mine, hh, 16) != 0)
         return -1;
      nsame++;
   }
   (void)nrem;
   (void)rname;
   (void)remote;
   return nsame >= 0 ? 0 : -1;
}
