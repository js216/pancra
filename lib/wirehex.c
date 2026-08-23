/* SPDX-License-Identifier: GPL-3.0
 * wirehex.c --- see wirehex.h
 * Copyright 2026 Jakob Kastelic
 */
#include "wirehex.h"
#include "sha256.h"
#include <stddef.h>
#include <stdint.h>

static const char hexd[] = "0123456789abcdef";

void wire_hex(const uint8_t *in, size_t n, char *out)
{
   if (!out)
      return;
   if (!in) {
      out[0] = '\0';
      return;
   }
   for (size_t i = 0; i < n; i++) {
      out[2 * i]       = hexd[in[i] >> 4U];
      out[(2 * i) + 1] = hexd[in[i] & 15U];
   }
   out[2 * n] = '\0';
}

/* 0..15, or 16 for a character that is not hex. Arithmetic rather than a
 * search: there is no strchr here (see the header), and a table walk would
 * make the cost depend on the value, which on a MAC is a signal. */
static unsigned nibble(char c)
{
   unsigned u = (unsigned char)c;
   if (u >= '0' && u <= '9')
      return u - '0';
   u |= 0x20U; /* fold case: the wire is lower case, a peer may not be */
   if (u >= 'a' && u <= 'f')
      return (u - 'a') + 10U;
   return 16U;
}

int wire_unhex(const char *in, size_t hexchars, uint8_t *out)
{
   if (hexchars % 2)
      return 0;
   if (hexchars == 0)
      return 1; /* nothing asked for is nothing to refuse */
   if (!in || !out)
      return 0;
   /* EVERY CHARACTER FIRST, THEN EVERY BYTE. This is the whole of the
    * failure-atomic promise: the loop that writes cannot be reached until the
    * input is known good, so a refusal has written nothing. The NUL check is
    * part of it -- `in` may be a C string shorter than `hexchars` claims, and
    * without this a short string decodes as far as it goes and then fails
    * with the caller's buffer half changed. */
   for (size_t i = 0; i < hexchars; i++)
      if (nibble(in[i]) > 15U)
         return 0;
   for (size_t i = 0; i < hexchars; i += 2)
      out[i / 2] = (uint8_t)((nibble(in[i]) << 4U) | nibble(in[i + 1]));
   return 1;
}

void wire_sha256_hex(const void *in, size_t n, char *out65)
{
   uint8_t h[32];
   sha256((const uint8_t *)in, n, h);
   wire_hex(h, 32, out65);
}

void wire_hash16(const void *data, size_t len, char out17[17])
{
   char hex[65];
   wire_sha256_hex(data, len, hex);
   for (int i = 0; i < 16; i++)
      out17[i] = hex[i];
   out17[16] = '\0';
}
