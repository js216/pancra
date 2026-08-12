/* SPDX-License-Identifier: GPL-3.0
 * hkdf.c --- HKDF extract-and-expand (RFC 5869)
 * Copyright 2026 Jakob Kastelic
 *
 * The generic construction only. TLS 1.3's HkdfLabel wrapper is a TLS
 * concept, not an HKDF one, so it lives with the TLS code that defines it.
 */
#include "hkdf.h"
#include "hmac.h"
#include <string.h>

void hkdf_extract(const uint8_t *salt, size_t saltn, const uint8_t *ikm,
                  size_t ikmn, uint8_t out[32])
{
   uint8_t zero[32] = {0};
   if (!salt) {
      salt  = zero;
      saltn = 32;
   }
   hmac_sha256(salt, saltn, ikm, ikmn, out);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t infon,
                 uint8_t *out, size_t n)
{
   uint8_t t[32];
   size_t tn   = 0;
   uint8_t ctr = 1;
   while (n) {
      uint8_t in[32 + 256 + 1];
      size_t k = 0;
      memcpy(in, t, tn);
      k += tn;
      memcpy(in + k, info, infon);
      k += infon;
      in[k++] = ctr++;
      hmac_sha256(prk, 32, in, k, t);
      tn          = 32;
      size_t take = n < 32 ? n : 32;
      memcpy(out, t, take);
      out += take;
      n -= take;
   }
}
