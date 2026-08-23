/* SPDX-License-Identifier: GPL-3.0
 * pbkdf2.c --- PBKDF2-HMAC-SHA256 (RFC 8018)
 * Copyright 2026 Jakob Kastelic
 *
 * Deliberately the plain construction: it is memory-cheap, which is a
 * weakness against a GPU and the only thing that fits a 56 MB board.
 *
 * See pbkdf2.h for each bound's source in RFC 8018 and for what a
 * substituting implementation does rather than refusing.
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
enum pbkdf2_status pbkdf2_sha256(const uint8_t *pw, size_t pwn,
                                 const uint8_t *salt, size_t saltn,
                                 unsigned iters, uint8_t *out, size_t n)
{
   /* EVERY PARAMETER IS JUDGED BEFORE ANY WORK, so a refusal leaves `out`
    * untouched and a caller cannot mistake a rejected cost parameter for a
    * hash. The order is fixed and each rule is the only gate on its own
    * failure, so a test can isolate one bound at a time.
    *
    * pwn is not checked: hmac_sha256 streams the message and hashes an
    * over-long key per RFC 2104, so any password length is exact. */
   if (!out || (!pw && pwn) || (!salt && saltn))
      return PBKDF2_ERR_ARG;
   if (saltn > PBKDF2_SALT_MAX)
      return PBKDF2_ERR_SALT;
   if (iters == 0)
      return PBKDF2_ERR_ITERS;
   /* Cast first: on a platform with a 32-bit size_t no `n` can reach the
    * RFC's ceiling, and writing the comparison in size_t would silently make
    * this the platform's limit rather than RFC 8018's. */
   if (n == 0 ||
       (uint64_t)n > (uint64_t)PBKDF2_BLOCK_MAX * (uint64_t)PBKDF2_HASH_LEN)
      return PBKDF2_ERR_DKLEN;

   /* THE KEY IS THE PASSWORD, AND IT NEVER CHANGES. Preparing HMAC's two
    * padded key blocks once, rather than once per PRF call, is half of this
    * function's total work: every iteration would otherwise re-hash the same
    * 128 bytes before it reached the message it is actually chaining. */
   struct hmac_key prf;
   hmac_key_init(&prf, pw, pwn);

   uint32_t block = 1;
   while (n) {
      uint8_t si[PBKDF2_SALT_MAX + 4], u[PBKDF2_HASH_LEN], t[PBKDF2_HASH_LEN];
      size_t k = saltn; /* NOT clamped any more: saltn <= PBKDF2_SALT_MAX was
                         * established above, so this fits by construction */
      if (saltn)        /* memcpy(_, NULL, 0) is undefined even when it copies
                         * nothing, and an empty salt is a legal ask */
         memcpy(si, salt, k);
      si[k]     = (uint8_t)(block >> 24);
      si[k + 1] = (uint8_t)(block >> 16);
      si[k + 2] = (uint8_t)(block >> 8);
      si[k + 3] = (uint8_t)block;
      hmac_key_mac(&prf, si, k + 4, u);
      memcpy(t, u, PBKDF2_HASH_LEN);
      /* iters >= 1 was established above, so this runs iters-1 times and T is
       * the XOR of exactly `iters` PRF outputs -- U1 alone when iters == 1,
       * which is what RFC 8018 says and what the RFC 7914 vector checks. */
      for (unsigned i = 1; i < iters; i++) {
         hmac_key_mac(&prf, u, PBKDF2_HASH_LEN, u);
         for (int j = 0; j < PBKDF2_HASH_LEN; j++)
            t[j] ^= u[j];
      }
      size_t take = n < PBKDF2_HASH_LEN ? n : PBKDF2_HASH_LEN;
      memcpy(out, t, take);
      out += take;
      n -= take;
      block++;
   }
   return PBKDF2_OK;
}
