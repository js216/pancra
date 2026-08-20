// SPDX-License-Identifier: GPL-3.0
// syncrow.c --- What a ROW is, and the byte helpers the protocol is built on
// Copyright 2026 Jakob Kastelic

#include "syncrow.h"
#include "sha256.h"

/* ---- hex, hashing ---------------------------------------------------- */

/* strlen/strchr are not in the freestanding shim, and adding them there would
 * mean adding symbols to stub_c.c for two one-line loops. */
long s_len(const char *p)
{
   long n = 0;
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

static const char hexd[] = "0123456789abcdef";

void hexify(const uint8_t *in, int n, char *out)
{
   for (size_t i = 0; i < (size_t)n; i++) {
      out[2 * i]       = hexd[in[i] >> 4U];
      out[(2 * i) + 1] = hexd[in[i] & 15U];
   }
   out[2 * (size_t)n] = '\0';
}

int unhex(const char *in, int hexchars, uint8_t *out)
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
void hash16(const char *data, long len, char out[17])
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
long row_bucket(const char *p, size_t len, int bucketed)
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

int want_has(const long *want, int nwant, long b)
{
   for (int i = 0; i < nwant; i++)
      if (want[i] == b)
         return 1;
   return 0;
}
