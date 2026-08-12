/* SPDX-License-Identifier: GPL-3.0
 * util.c --- bytes, hex, hashing, and the small parsing the web pages need
 * Copyright 2026 Jakob Kastelic
 */
#include "util.h"
#include "ct.h"
#include "hmac.h"
#include "rand.h"
#include "sha256.h"
#include "sync.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Grow to at least `need` more bytes. A failure is recorded and every later
 * append becomes a no-op, so callers check `err` once at the end instead of
 * after every line -- the alternative is an error check on every append,
 * which is exactly the check that gets forgotten. */
static int sb_room(struct sb *s, size_t need)
{
   if (s->err)
      return 0;
   if (s->n + need + 1 <= s->cap)
      return 1;
   size_t cap = s->cap ? s->cap : 4096;
   while (cap < s->n + need + 1)
      cap *= 2;
   char *p = realloc(s->p, cap);
   if (!p) {
      s->err = 1;
      return 0;
   }
   s->p   = p;
   s->cap = cap;
   return 1;
}

int sb_raw(struct sb *s, const void *data, size_t n)
{
   if (!sb_room(s, n))
      return 0;
   memcpy(s->p + s->n, data, n);
   s->n += n;
   s->p[s->n] = '\0';
   return 1;
}

int sb_add(struct sb *s, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char tmp[1024];
   int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
   va_end(ap);
   if (n < 0)
      return 0;
   if ((size_t)n < sizeof tmp)
      return sb_raw(s, tmp, (size_t)n);
   /* Longer than the stack scratch: format again straight into the buffer. */
   if (!sb_room(s, (size_t)n))
      return 0;
   va_start(ap, fmt);
   vsnprintf(s->p + s->n, (size_t)n + 1, fmt, ap);
   va_end(ap);
   s->n += (size_t)n;
   return 1;
}

void sb_free(struct sb *s)
{
   free(s->p);
   s->p = NULL;
   s->n = s->cap = 0;
   s->err        = 0;
}

/* Every secret in this program starts here, so a short read is fatal rather
 * than something to paper over: a session cookie or pairing code built from
 * stack garbage is worse than no server at all. */
/* The READ is lib/rand.c's; what stays here is this program's POLICY about a
 * failure, which is to die. Keeping the policy at the caller and the syscall
 * in one place is the split lib/rand.h asks for -- there were four copies of
 * this loop, and the one in the app did a single read with no retry. */
void rnd_bytes(uint8_t *out, size_t n)
{
   if (!rand_bytes(out, n)) {
      fprintf(stderr, "sync: no entropy available; refusing to continue\n");
      _exit(1);
   }
}

static const char HEX[] = "0123456789abcdef";

void hex_of(const uint8_t *in, size_t n, char *out)
{
   for (size_t i = 0; i < n; i++) {
      out[2 * i]     = HEX[in[i] >> 4];
      out[2 * i + 1] = HEX[in[i] & 15];
   }
   out[2 * n] = '\0';
}

void rnd_hex(char *out, size_t hexlen)
{
   uint8_t b[64];
   size_t n = hexlen / 2;
   if (n > sizeof b)
      n = sizeof b;
   rnd_bytes(b, n);
   hex_of(b, n, out);
}

int hex_to(const char *in, size_t hexchars, uint8_t *out)
{
   if (hexchars % 2)
      return 0;
   for (size_t i = 0; i < hexchars; i += 2) {
      const char *p = strchr(HEX, in[i] | 0x20);
      const char *q = strchr(HEX, in[i + 1] | 0x20);
      if (!p || !q || !in[i] || !in[i + 1])
         return 0;
      out[i / 2] = (uint8_t)(((p - HEX) << 4) | (q - HEX));
   }
   return 1;
}

/* Compare without an early exit. Used on password hashes and MACs, where the
 * time taken by memcmp would tell an attacker how many leading bytes of a
 * guess were right and turn 2^256 into 32 x 256. */
void sha256_hex(const void *in, size_t n, char *out64)
{
   uint8_t h[32];
   sha256(in, n, h);
   hex_of(h, 32, out64);
}

/* hmac_sha256 comes from lib/hmac.c -- one implementation, checked against
 * RFC 4231, rather than a wrapper around a library we no longer link. (It
 * lived in a srv/crypto.c for a while; that file is gone, and everything in
 * it that was a primitive rather than protocol moved to lib/.) */

size_t url_decode(char *s)
{
   char *w = s;
   for (char *r = s; *r; r++) {
      if (*r == '+') {
         *w++ = ' ';
      } else if (*r == '%' && r[1] && r[2]) {
         uint8_t b;
         char h[3] = {r[1], r[2], 0};
         if (hex_to(h, 2, &b)) {
            *w++ = (char)b;
            r += 2;
         } else {
            *w++ = *r;
         }
      } else {
         *w++ = *r;
      }
   }
   *w = '\0';
   return (size_t)(w - s);
}

void html_esc(char *out, size_t cap, const char *in)
{
   /* NOT strlen(out): this used to append, and every one of its callers
    * passes a fresh zeroed buffer, so the only thing the behaviour bought was
    * that a forgotten "= {0}" turned into an out-of-bounds write at a garbage
    * offset -- in the HTML escaping path, on a request handler's stack. */
   size_t k = 0;
   for (; *in && k + 8 < cap; in++) {
      const char *rep = NULL;
      switch (*in) {
         case '&': rep = "&amp;"; break;
         case '<': rep = "&lt;"; break;
         case '>': rep = "&gt;"; break;
         case '"': rep = "&quot;"; break;
         case '\'': rep = "&#39;"; break;
         default: break;
      }
      if (rep) {
         size_t n = strlen(rep);
         memcpy(out + k, rep, n);
         k += n;
      } else {
         out[k++] = *in;
      }
   }
   out[k] = '\0';
}

int form_get(const char *body, size_t len, const char *name, char *out,
             size_t cap)
{
   size_t nlen     = strlen(name);
   const char *p   = body;
   const char *end = body + len;
   while (p < end) {
      const char *amp  = memchr(p, '&', (size_t)(end - p));
      const char *stop = amp ? amp : end;
      if ((size_t)(stop - p) > nlen && !strncmp(p, name, nlen) &&
          p[nlen] == '=') {
         size_t vlen = (size_t)(stop - p) - nlen - 1;
         if (vlen >= cap)
            vlen = cap - 1;
         memcpy(out, p + nlen + 1, vlen);
         out[vlen] = '\0';
         url_decode(out);
         return 1;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   out[0] = '\0';
   return 0;
}

int hdr_get(const char *hdr, const char *name, char *out, size_t cap)
{
   size_t nlen = strlen(name);
   for (const char *h = hdr; h && *h;) {
      if (!strncasecmp(h, name, nlen) && h[nlen] == ':') {
         const char *v = h + nlen + 1;
         while (*v == ' ' || *v == '\t')
            v++;
         size_t k = 0;
         while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < cap) {
            out[k] = v[k];
            k++;
         }
         out[k] = '\0';
         return 1;
      }
      h = strchr(h, '\n');
      if (h)
         h++;
   }
   out[0] = '\0';
   return 0;
}
