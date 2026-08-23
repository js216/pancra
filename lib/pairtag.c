/* SPDX-License-Identifier: GPL-3.0
 * pairtag.c --- see pairtag.h
 * Copyright 2026 Jakob Kastelic
 */
#include "pairtag.h"
#include "hmac.h"
#include <stddef.h>
#include <stdint.h>

/* Its own, because the app half of this protocol is freestanding and has no
 * strlen. Bounded: a label is a compile-time constant here, and a bound is
 * what stops this walking off the end if that ever stops being true. */
static size_t label_len(const char *s)
{
   size_t n = 0;
   while (n < 256 && s[n])
      n++;
   return n;
}

int pair_tag(const uint8_t *key, size_t keylen, const char *label, char *out,
             size_t outcap)
{
   if (!out || outcap < PAIR_TAG_HEX + 1)
      return 0;
   out[0] = '\0';
   if (!key || !keylen || !label)
      return 0;
   size_t ln = label_len(label);
   if (!ln || ln >= 256)
      return 0; /* an empty label is not one of the two, and 256 is the bound
                 * label_len stops at: a string with no NUL in it */
   uint8_t mac[32];
   hmac_sha256(key, keylen, (const uint8_t *)label, ln, mac);
   static const char hex[] = "0123456789abcdef";
   /* HALF THE DIGEST, LOWER CASE, and the truncation is at a BYTE boundary --
    * 32 hex characters is the first 16 bytes, not 16 and a nibble. */
   for (int i = 0; i < PAIR_TAG_HEX / 2; i++) {
      /* UNSIGNED THROUGHOUT. `mac[i]` promotes to int, and a shift or mask on
       * a signed operand is a different rule from the one this is written
       * against; the digest is bytes, so it is indexed as bytes. */
      unsigned b                       = mac[i];
      out[(size_t)2 * (size_t)i]       = hex[(b >> 4U) & 0x0FU];
      out[((size_t)2 * (size_t)i) + 1] = hex[b & 0x0FU];
   }
   out[PAIR_TAG_HEX] = '\0';
   return 1;
}
