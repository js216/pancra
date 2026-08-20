/* SPDX-License-Identifier: GPL-3.0
 * util.c --- bytes, hex, hashing, and the small parsing the web pages need
 * Copyright 2026 Jakob Kastelic
 */
#include "util.h"
#include "ct.h"
#include "hmac.h"
#include "proto.h"
#include "rand.h"
#include "sha256.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- THE CAPACITY RULE, AS A PURE FUNCTION (see util.h for the wrap) ---- */

int sb_cap_for(size_t n, size_t cap, size_t need, size_t *out)
{
   /* ANSWERED FIRST, ALWAYS -- the same rule email_canon follows below. The
    * one thing a caller does with *out is hand it to realloc, so a caller that
    * drops the return value must be left with zero rather than its stack. */
   if (out)
      *out = 0;
   if (!out)
      return 0;

   /* THE TWO ADDITIONS, ONE AT A TIME, EACH BEFORE IT IS PERFORMED.
    *
    * `n + need + 1` is what the buffer must hold: the bytes already in it, the
    * bytes about to be appended, and the NUL that sb_raw writes past them. The
    * old code evaluated that expression twice and tested neither addition.
    *
    * `need + 1` first. need == SIZE_MAX makes it zero, and that alone was
    * enough: a fresh builder (n = 0, cap = 0) computed 0 <= 0 and returned
    * "there is room" without allocating anything at all, after which sb_raw
    * ran memcpy(NULL, data, SIZE_MAX). Measured: segmentation fault. */
   if (need > SIZE_MAX - 1)
      return 0;
   /* `n + (need + 1)` second, written as a subtraction so it cannot wrap while
    * being checked. The general form of the defect is any `need` in
    * [SIZE_MAX - n - cap, SIZE_MAX]: the sum wraps to something small, the
    * <= test passes, and the builder reports room it does not have. With
    * n = 10 and cap = 8192 the sum came to 0, so the function returned 1 with
    * the capacity untouched -- and `err` stayed clear, so no caller anywhere
    * could tell. THAT is the heap overflow: it is not that the allocation
    * failed, it is that no allocation was attempted and the append went ahead
    * anyway. */
   if (n > SIZE_MAX - (need + 1))
      return 0;
   size_t want = n + need + 1;

   /* Already big enough: say so with the capacity that is already there, so
    * the caller can compare and skip the realloc. */
   if (want <= cap) {
      *out = cap;
      return 1;
   }

   /* GUARDED DOUBLING, WITH A FINAL EXACT CAPACITY.
    *
    * Doubling from 4096 is what keeps a page that appends a thousand small
    * rows from being a thousand reallocs, and that part is unchanged. What is
    * new is that the loop can now END. It used to be
    *
    *     while (cap < s->n + need + 1) cap *= 2;
    *
    * and `cap` is a size_t holding a power of two: 4096, 8192, ... 2^63, and
    * then 2^63 * 2 == 0. Zero doubled is zero, so for any target above 2^63
    * the condition 0 < target is true forever and the loop never leaves. Not a
    * wrong answer, not a crash -- a worker thread that never returns, holding
    * the page mutex, with every other user's page queued behind it. Measured
    * against the old code: need = 2^63 span forever; so did need =
    * SIZE_MAX - 4000 on a builder that already had 4096 bytes.
    *
    * So the doubling stops one step short of the wrap and takes the exact size
    * instead. The result is no longer always a power of two, which nothing
    * depends on -- and a request that large will simply fail in realloc, which
    * is a failure the sticky `err` already reports. */
   size_t c = cap ? cap : SB_MIN_CAP;
   while (c < want) {
      if (c > SIZE_MAX / 2) {
         c = want; /* the next doubling would wrap: ask for exactly enough */
         break;
      }
      c *= 2;
   }
   *out = c;
   return 1;
}

/* Grow to at least `need` more bytes. A failure is recorded and every later
 * append becomes a no-op, so callers check `err` once at the end instead of
 * after every line -- the alternative is an error check on every append,
 * which is exactly the check that gets forgotten.
 *
 * A REFUSED SIZE IS RECORDED THE SAME WAY AN ALLOCATION FAILURE IS. That is
 * deliberate and it is the reason this change touches no caller: `err` already
 * means "this buffer is not the page you asked for", page_refresh (srv/page.c)
 * and send_sb (srv/logs.c) already turn it into a 500, and an impossible size
 * is not a different kind of answer from an impossible allocation. What is new
 * is that there IS an answer -- before, an impossible size either returned
 * success and overflowed, or never returned. */
static int sb_room(struct sb *s, size_t need)
{
   if (!s)
      return 0;
   if (s->err)
      return 0;
   size_t cap;
   if (!sb_cap_for(s->n, s->cap, need, &cap)) {
      s->err = 1;
      return 0;
   }
   if (cap <= s->cap)
      return 1; /* the room is already there */
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
   if (!s)
      return 0;
   /* A NULL source with a nonzero length is undefined behaviour in memcpy, not
    * a copy of nothing, and it is the shape a caller gets when a lookup it
    * forgot to check returned NULL. Refused as a builder error, so the page
    * comes out as a 500 rather than as a body with a hole in it. (data == NULL
    * with n == 0 is left alone: it copies nothing and means nothing.) */
   if (!data && n) {
      s->err = 1;
      return 0;
   }
   if (!sb_room(s, n))
      return 0;
   /* GUARDED, because memcpy(dst, NULL, 0) is undefined behaviour in the
    * standard even though every implementation copies nothing -- and this file
    * is compiled under UBSan by `make srvasan`, which says so. */
   if (n)
      memcpy(s->p + s->n, data, n);
   s->n += n;
   s->p[s->n] = '\0';
   return 1;
}

int sb_add(struct sb *s, const char *fmt, ...)
{
   if (!s)
      return 0;
   if (!fmt) {
      s->err = 1;
      return 0;
   }
   va_list ap;
   va_start(ap, fmt);
   char tmp[1024];
   int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
   va_end(ap);
   /* A FORMATTING FAILURE IS A FAILURE. This returned 0 and left `err` clear,
    * so the one thing every caller checks said the page was fine while the
    * text this call was supposed to contribute was simply missing from it.
    * (vsnprintf returns negative on an encoding error, and on this platform
    * for a result that will not fit in an int.) */
   if (n < 0) {
      s->err = 1;
      return 0;
   }
   if ((size_t)n < sizeof tmp)
      return sb_raw(s, tmp, (size_t)n);
   /* Longer than the stack scratch: format again straight into the buffer.
    * sb_room has guaranteed n + need + 1 bytes, so the n + 1 handed to
    * vsnprintf here is inside the allocation by construction. */
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
   if (!s)
      return;
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

/* ---- ONE FORM DECODER, WITH FOUR WAYS TO SAY NO -------------------------
 *
 * util.h states what each answer means and what the old single bit hid. What
 * follows is the mechanism, and it is deliberately two passes over the value:
 * MEASURE and JUDGE first, COPY only once the answer is FORM_OK. The old code
 * copied first (clipped to `cap`) and decoded afterwards, which is why an
 * over-long value became a different value rather than an error -- there was
 * nothing left to measure by the time anyone could have complained.
 *
 * `+` still means space: that is the encoding (RFC 1866 8.2.1, kept by the
 * WHATWG URL standard's form serialiser), not a leniency. */

/* How long `v[0..n)` decodes to, or -1 if it does not decode at all.
 * `*sawnul` is set when a "%00" is present -- reported separately from a bad
 * escape only so the two cannot be conflated by a later reader; both are
 * FORM_MALFORMED, because a NUL is not a character a form field may carry and
 * every caller here treats the value as a C string. */
static long decoded_len(const char *v, size_t n, int *sawnul)
{
   long out = 0;
   for (size_t i = 0; i < n; i++) {
      if (v[i] == '%') {
         uint8_t b;
         char h[3];
         /* A "%" with fewer than two characters after it inside THIS value is
          * a truncated escape. It used to decode to a literal "%" -- so a
          * value cut off mid-escape produced text the client never sent. */
         if (i + 2 >= n)
            return -1;
         h[0] = v[i + 1];
         h[1] = v[i + 2];
         h[2] = '\0';
         if (!hex_to(h, 2, &b))
            return -1; /* "%zz": not an escape, and not text either */
         if (b == 0)
            *sawnul = 1;
         i += 2;
      } else if (v[i] == '\0') {
         /* A RAW NUL IN THE BODY, not an escaped one. The body is bytes off
          * the wire and nothing has promised it is text; a caller reading the
          * result as a C string would stop here and never see the rest. */
         *sawnul = 1;
      }
      out++;
   }
   return out;
}

/* Decode `v[0..n)` into `out`, which decoded_len has already proved fits. */
static void decode_into(const char *v, size_t n, char *out)
{
   size_t w = 0;
   for (size_t i = 0; i < n; i++) {
      if (v[i] == '%') {
         uint8_t b;
         char h[3] = {v[i + 1], v[i + 2], 0};
         (void)hex_to(h, 2, &b); /* validated already */
         out[w++] = (char)b;
         i += 2;
      } else if (v[i] == '+') {
         out[w++] = ' ';
      } else {
         out[w++] = v[i];
      }
   }
   out[w] = '\0';
}

enum form_field form_field(const char *body, size_t len, const char *name,
                           char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!body || !name || !out || !cap)
      return FORM_ABSENT;
   size_t nlen     = strlen(name);
   const char *p   = body;
   const char *end = body + len;
   const char *val = NULL; /* the one match, still encoded */
   size_t vlen     = 0;
   int matches     = 0;
   while (p < end) {
      const char *amp  = memchr(p, '&', (size_t)(end - p));
      const char *stop = amp ? amp : end;
      /* The name is compared RAW, as it arrived. A percent-escaped spelling
       * of a name ("%63srf") is therefore not that name and the field reads
       * as absent -- which is a refusal, not a bypass. Decoding names before
       * matching would be the bypass: it would give every security field a
       * second spelling that only this end recognises. */
      if ((size_t)(stop - p) > nlen && !strncmp(p, name, nlen) &&
          p[nlen] == '=') {
         /* SCANNING DOES NOT STOP AT THE FIRST MATCH. That early `return 1`
          * is what made duplicates invisible; the whole body is read so that
          * "there is exactly one" is something this function KNOWS. */
         matches++;
         if (matches == 1) {
            val  = p + nlen + 1;
            vlen = (size_t)(stop - p) - nlen - 1;
         }
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   if (!matches)
      return FORM_ABSENT;
   if (matches > 1)
      return FORM_DUPLICATE;

   int sawnul = 0;
   long dl    = decoded_len(val, vlen, &sawnul);
   if (dl < 0 || sawnul)
      return FORM_MALFORMED;
   /* MEASURED, NOT CLIPPED. `>= cap` because the NUL needs the last byte. */
   if ((size_t)dl >= cap)
      return FORM_TOO_LONG;
   decode_into(val, vlen, out);
   return FORM_OK;
}

enum form_body form_body_check(const char *body, size_t len)
{
   if (!body || !len)
      return FORM_BODY_OK; /* nothing to disagree about */
   /* TWO QUESTIONS IN ONE PASS PER PAIR: is every escape in this pair real,
    * and has this name been seen before? The names are compared against each
    * other rather than against a list, so a field nobody has written a
    * handler for yet is still refused when it is sent twice. */
   const char *p   = body;
   const char *end = body + len;
   while (p < end) {
      const char *amp  = memchr(p, '&', (size_t)(end - p));
      const char *stop = amp ? amp : end;
      const char *eq   = memchr(p, '=', (size_t)(stop - p));
      const char *nm   = p;
      size_t nlen      = eq ? (size_t)(eq - p) : (size_t)(stop - p);
      /* THE WHOLE PAIR, name and value, is checked for escapes: a bad escape
       * in a NAME is exactly as much of a disagreement as one in a value, and
       * form_field would never look at that name to notice. */
      int sawnul = 0;
      if (decoded_len(p, (size_t)(stop - p), &sawnul) < 0 || sawnul)
         return FORM_BODY_MALFORMED;
      /* Every EARLIER pair, compared by name. Quadratic in the number of
       * fields, which is fine and is a deliberate choice over a hash table:
       * the largest form this server serves has six fields, and a body that
       * arrived with thousands is already bounded by HTTP_BODY_MAX and is
       * about to be refused by the duplicate rule anyway. */
      for (const char *q = body; q < p;) {
         const char *qamp  = memchr(q, '&', (size_t)(p - q));
         const char *qstop = qamp ? qamp : p;
         const char *qeq   = memchr(q, '=', (size_t)(qstop - q));
         size_t qlen       = qeq ? (size_t)(qeq - q) : (size_t)(qstop - q);
         if (qlen == nlen && nlen && !memcmp(q, nm, nlen))
            return FORM_BODY_DUPLICATE;
         if (!qamp)
            break;
         q = qamp + 1;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return FORM_BODY_OK;
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

/* ---- ONE CANONICAL EMAIL (see util.h for what went wrong without it) ---- */

int email_canon(const char *in, char *out, size_t cap)
{
   /* EMPTIED FIRST, ALWAYS. A caller that forgets the return value must not
    * be left holding whatever was on its stack -- and every caller here uses
    * `out` for the throttle key, so stack rubbish would become a row. */
   if (out && cap)
      out[0] = '\0';
   if (!in || !out || cap < EMAIL_BUF)
      return 0;

   /* TRIMMED AT THE EDGES, STRICT INSIDE. A person typing into a form, or
    * pasting from a message, brings a leading or trailing space with them
    * often enough that refusing it would be gratuitous -- and once trimmed the
    * address is EXACTLY the one they meant, so it keys the same throttle row
    * and finds the same account as when they type it cleanly. A space in the
    * MIDDLE is a different thing: it cannot be tidied into a unique answer, so
    * it is refused below with every other control byte. */
   const char *b = in;
   while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')
      b++;
   const char *e = b + strlen(b);
   while (e > b &&
          (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
      e--;
   size_t n = (size_t)(e - b);

   /* THE LENGTH BOUND, AND IT IS THE ONLY ONE IN THE PROGRAM. 254 is what RFC
    * 5321 allows a forward path to be once the angle brackets are removed;
    * three is the shortest thing that can be an address at all ("a@b"). The
    * login form has refused above 254 for a while and every other surface did
    * not, which is the hole this function closes: see util.h. */
   if (n < 3 || n > EMAIL_MAX)
      return 0;

   /* EXACTLY ONE '@', with something on each side. user_create asked only that
    * an '@' appear ANYWHERE, so "@" alone made an account. This is still not
    * an RFC 5322 parser and is not trying to be -- deliverability is not this
    * server's business -- but "one at-sign, non-empty both sides" is the shape
    * every surface can agree on cheaply, and agreement is the point. */
   const char *at = NULL;
   for (size_t i = 0; i < n; i++) {
      if (b[i] != '@')
         continue;
      if (at)
         return 0; /* a second '@' */
      at = b + i;
   }
   if (!at || at == b || at == e - 1)
      return 0;

   /* NO CONTROL BYTES, NO SPACES, NO DEL. A newline in a throttle key is a
    * row nobody can read back in a log; a space is an address the user cannot
    * have meant. Bytes at or above 0x80 are LET THROUGH untouched: sqlite's
    * NOCASE folds ASCII A-Z and nothing else, so leaving them alone is exactly
    * what keeps this function's idea of "the same address" identical to the
    * one `user.email`'s collation uses. Refusing them instead would lock an
    * existing account holder out of a server that had already accepted their
    * address, with no way back in -- `sync passwd` would refuse it too. */
   for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)b[i];
      if (c <= 0x20 || c == 0x7f)
         return 0;
   }

   /* THE CANONICAL FORM: ASCII lower case, and nothing else.
    *
    * A-Z ONLY, DELIBERATELY, because that is precisely what `user.email COLLATE
    * NOCASE` folds (see srv/db.c's schema notes, and its separate check that
    * the INDEX's collation matches the COLUMN's). Fold more here and this
    * function would call two addresses the same that the account table calls
    * different -- the throttle would then count two accounts' failures into one
    * row. Fold less and the mismatch runs the other way, which is the defect
    * being fixed: `login_fail.email` is a TEXT PRIMARY KEY with no collation,
    * so it is BINARY, and "Bob@x" and "bob@x" were two throttle rows for one
    * account. Five failures under each of a few dozen spellings and the
    * throttle never fires once, while every one of those spellings leaves a row
    * behind that a later attempt can never match again. */
   for (size_t i = 0; i < n; i++) {
      char c = b[i];
      out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
   }
   out[n] = '\0';
   return 1;
}

/* ---- THE PUBLIC ORIGIN (see util.h for why the request cannot pick it) --- */

#ifndef PANCRA_DEFAULT_ORIGIN
#define PANCRA_DEFAULT_ORIGIN "pancra.org"
#endif

int origin_ok(const char *s)
{
   if (!s || !*s)
      return 0;
   int ncolon = 0;
   int nport  = 0;
   for (const char *p = s; *p; p++) {
      if (*p == ':') {
         /* One colon, and it must be followed by digits: a second one, or a
          * colon with anything else after it, is not a host:port. This also
          * refuses the "userinfo@host" and "//host/path" shapes outright --
          * nothing here is trying to parse a URL, only to recognise the one
          * form a link is built from. */
         if (++ncolon > 1 || p == s || !p[1])
            return 0;
         continue;
      }
      if (ncolon) {
         if (*p < '0' || *p > '9')
            return 0;
         nport++;
         if (nport > 5)
            return 0;
         continue;
      }
      int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-';
      if (!ok)
         return 0;
   }
   /* A trailing dot or hyphen is not a name anyone resolves, and a leading one
    * is how a value sneaks past a naive prefix test. */
   size_t n = strlen(s);
   if (s[0] == '.' || s[0] == '-' || s[n - 1] == '.' || s[n - 1] == '-')
      return 0;
   return ncolon == 0 || nport > 0;
}

const char *public_origin(void)
{
   /* READ ONCE. A getenv per request is not the cost that matters; a value
    * that could change between the link shown and the link revoked is. */
   static const char *cached;
   if (cached)
      return cached;
   const char *env = getenv("PANCRA_ORIGIN");
   if (env && origin_ok(env)) {
      cached = env;
      return cached;
   }
   if (env && *env)
      fprintf(stderr,
              "sync: PANCRA_ORIGIN is not a host[:port]; using the compiled "
              "default %s. Share links must not name a host this server was "
              "not configured with.\n",
              PANCRA_DEFAULT_ORIGIN);
   cached = PANCRA_DEFAULT_ORIGIN;
   return cached;
}
