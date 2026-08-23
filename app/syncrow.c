// SPDX-License-Identifier: GPL-3.0
// syncrow.c --- What a ROW is, and the byte helpers the protocol is built on
// Copyright 2026 Jakob Kastelic

#include "syncrow.h"
#include "wirehex.h" /* the ONE hex/hash codec both halves use */
#include <stddef.h>
#include <stdint.h>

/* ---- hex, hashing ---------------------------------------------------- */

/* strlen/strchr are not in the freestanding shim, and adding them there would
 * mean adding symbols to stub_c.c for two one-line loops. */
int64_t s_len(const char *p)
{
   int64_t n = 0;
   while (p[n])
      n++;
   return n;
}

const char *s_chr(const char *p, char c)
{
   for (; *p; p++)
      if (*p == c)
         return p;
   return 0;
}

/* ---- THE HEX AND THE HASH ARE lib/wirehex.h's --------------
 *
 * These three were written out here AND in srv/util.c: two encoders, two
 * decoders, two truncated hashes, for a protocol whose entire purpose is that
 * both sides produce the same bytes. The copies had already diverged where it
 * mattered -- this decoder wrote each byte as it went and returned 0 at the
 * first bad character, leaving a PREFIX of the caller's buffer modified,
 * while the server's validated the whole string first and left a refused
 * call's output untouched.
 *
 * They are one implementation now and it is the failure-atomic one. The names
 * below stay because a dozen call sites in this half use them, and each is a
 * line long. */
void hexify(const uint8_t *in, int n, char *out)
{
   wire_hex(in, (size_t)(n < 0 ? 0 : n), out);
}

int unhex(const char *in, int hexchars, uint8_t *out)
{
   return hexchars < 0 ? 0 : wire_unhex(in, (size_t)hexchars, out);
}

/* The protocol's hash: first 16 hex chars of SHA-256 (lib/wirehex.h). */
void hash16(const char *data, int64_t len, char out[17])
{
   wire_hash16(data, (size_t)(len < 0 ? 0 : len), out);
}

/* NO SECOND HMAC LIVES HERE. An app-local one truncating the message at
 * SYNC_BUF_MAX is a MAC over a PREFIX for anything longer, while lib/hmac.c
 * pre-hashes past 512 bytes -- a third function again. Both ends of this
 * protocol have to compute the same MAC, so there is one implementation
 * (lib/hmac.c, streamed, pinned to the RFC 4231 vectors) and this file calls
 * it. */

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
void row_sort(const char *base, struct row *r, int n)
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
int row_ok(const char *p, int len)
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
int64_t row_bucket(const char *p, size_t len, int bucketed)
{
   if (!bucketed)
      return 0;
   int64_t v = 0;
   size_t i  = 0;
   while (i < len && p[i] >= '0' && p[i] <= '9') {
      if (i < 18)
         v = (v * 10) + (p[i] - '0');
      i++;
   }
   return v / 86400;
}

int want_has(const int64_t *want, int nwant, int64_t b)
{
   for (int i = 0; i < nwant; i++)
      if (want[i] == b)
         return 1;
   return 0;
}
