/* SPDX-License-Identifier: GPL-3.0
 * hkdf.c --- HKDF extract-and-expand (RFC 5869)
 * Copyright 2026 Jakob Kastelic
 *
 * The generic construction only. TLS 1.3's HkdfLabel wrapper is a TLS
 * concept, not an HKDF one, so it lives with the TLS code that defines it.
 *
 * See hkdf.h for where each bound comes from and for what the unbounded
 * version of this file used to do instead of refusing.
 */
#include "hkdf.h"
#include "hmac.h"
#include <string.h>

void hkdf_extract(const uint8_t *salt, size_t saltn, const uint8_t *ikm,
                  size_t ikmn, uint8_t out[HKDF_HASH_LEN])
{
   uint8_t zero[HKDF_HASH_LEN] = {0};
   /* RFC 5869 2.2: "if not provided, it is set to a string of HashLen
    * zeros". NOTE that this also swallows (NULL, 13), which is a caller bug
    * this signature cannot report -- see hkdf.h. */
   if (!salt) {
      salt  = zero;
      saltn = HKDF_HASH_LEN;
   }
   hmac_sha256(salt, saltn, ikm, ikmn, out);
}

enum hkdf_status hkdf_expand(const uint8_t prk[HKDF_HASH_LEN],
                             const uint8_t *info, size_t infon, uint8_t *out,
                             size_t n)
{
   /* ALL OF THE CHECKING HAPPENS HERE, before a single byte is derived, so a
    * refusal leaves `out` byte for byte as the caller left it. A partly
    * filled output would be the worse of the two failures: it looks exactly
    * like a key, and it is the shape a clamp leaves behind. */
   if (!prk || !out || (!info && infon))
      return HKDF_ERR_ARG;
   if (infon > HKDF_INFO_MAX)
      return HKDF_ERR_INFO;
   if (n == 0 || n > HKDF_L_MAX)
      return HKDF_ERR_LEN;

   uint8_t t[HKDF_HASH_LEN];
   size_t tn   = 0;
   uint8_t ctr = 1;
   while (n) {
      uint8_t in[HKDF_HASH_LEN + HKDF_INFO_MAX + 1];
      size_t k = 0;
      memcpy(in, t, tn); /* T(0) is empty, so nothing is read on the first
                          * pass: tn is still 0 here */
      k += tn;
      if (infon) { /* memcpy(_, NULL, 0) is undefined even though it copies
                    * nothing, and (NULL, 0) is a legitimate empty info --
                    * RFC 5869 test case 3 is exactly that call */
         memcpy(in + k, info, infon);
         k += infon;
      }
      /* THE COUNTER CANNOT WRAP any more: n <= 255*HashLen was checked above,
       * so this loop body runs at most 255 times and the largest value ever
       * written here is 255. That is the whole reason the bound exists. */
      in[k++] = ctr++;
      hmac_sha256(prk, HKDF_HASH_LEN, in, k, t);
      tn          = HKDF_HASH_LEN;
      size_t take = n < HKDF_HASH_LEN ? n : HKDF_HASH_LEN;
      memcpy(out, t, take);
      out += take;
      n -= take;
   }
   return HKDF_OK;
}
