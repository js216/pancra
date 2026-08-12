/* SPDX-License-Identifier: GPL-3.0
 * pbkdf2.c --- PBKDF2-HMAC-SHA256 (RFC 8018)
 * Copyright 2026 Jakob Kastelic
 *
 * Deliberately the plain construction: it is memory-cheap, which is a
 * weakness against a GPU and the only thing that fits a 56 MB board.
 */
#include "pbkdf2.h"
#include "hmac.h"
#include <string.h>

/* ---- PBKDF2-HMAC-SHA256 ------------------------------------------------
 *
 * Password hashing, and the only thing here that is meant to be SLOW: the
 * iteration count is chosen so a guess costs real time on the machine that
 * will run it (see PW_ITERS_DEFAULT).
 */
void pbkdf2_sha256(const uint8_t *pw, size_t pwn, const uint8_t *salt,
                   size_t saltn, unsigned iters, uint8_t *out, size_t n)
{
   uint32_t block = 1;
   while (n) {
      uint8_t si[256], u[32], t[32];
      size_t k = saltn > sizeof si - 4 ? sizeof si - 4 : saltn;
      memcpy(si, salt, k);
      si[k]     = (uint8_t)(block >> 24);
      si[k + 1] = (uint8_t)(block >> 16);
      si[k + 2] = (uint8_t)(block >> 8);
      si[k + 3] = (uint8_t)block;
      hmac_sha256(pw, pwn, si, k + 4, u);
      memcpy(t, u, 32);
      for (unsigned i = 1; i < iters; i++) {
         hmac_sha256(pw, pwn, u, 32, u);
         for (int j = 0; j < 32; j++)
            t[j] ^= u[j];
      }
      size_t take = n < 32 ? n : 32;
      memcpy(out, t, take);
      out += take;
      n -= take;
      block++;
   }
}
