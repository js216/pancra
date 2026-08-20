/* SPDX-License-Identifier: GPL-3.0
 * gcm.c --- AES-128-GCM authenticated encryption
 * Copyright 2026 Jakob Kastelic
 *
 * See gcm.h. Built on lib/aes.c's block function; nothing here knows about
 * TLS, which is why it is a primitive in lib/ and not part of the server.
 *
 * Two things in this file exist because of a secret that must not steer a
 * branch or a length: gf_mul is written branchlessly (see below), and every
 * public entry point judges its lengths against SP 800-38D 5.2.1.1 before it
 * writes anything. gcm.h carries the derivation of both bounds and the
 * description of what the old silent counter wrap produced.
 */
#include "gcm.h"
#include "aes.h"
#include "ct.h"
#include <string.h>

/* THE BOUNDS, CHECKED BY THE COMPILER RATHER THAN BY THE COMMENT ABOVE THEM.
 * gcm.h explains each; these are the arithmetic identities that explanation
 * rests on, so that editing one constant and not the others cannot compile. */
_Static_assert(GCM_PT_MAX == GCM_CTR_BLOCKS_MAX * 16ull,
               "the plaintext bound must BE the 32-bit counter's block "
               "capacity, not merely resemble it");
_Static_assert(GCM_PT_MAX * 8ull == (1ull << 39) - 256ull,
               "the plaintext bound in bytes must equal SP 800-38D 5.2.1.1's "
               "len(P) <= 2^39 - 256 bits");
_Static_assert(GCM_AAD_MAX == UINT64_MAX / 8ull,
               "the AAD bound must be the largest byte count whose bit count "
               "still fits GHASH's 64-bit length field");
/* And the counter walk gcm.h describes, so the claim that 2^32 - 2 blocks
 * never wrap -- and that one more block lands on 0 and two more on the tag
 * mask -- is a compile-time fact and not a story. */
_Static_assert((uint32_t)(1ull + GCM_CTR_BLOCKS_MAX) == 0xFFFFFFFFu,
               "the last legal data block must use the last counter value");
_Static_assert((uint32_t)(1ull + GCM_CTR_BLOCKS_MAX + 1) == 0u,
               "one block past the bound must be where the counter wraps");
_Static_assert((uint32_t)(1ull + GCM_CTR_BLOCKS_MAX + 2) == 1u,
               "two blocks past the bound must collide with J0, the tag mask");

/* ---- GHASH ------------------------------------------------------------
 *
 * Multiplication in GF(2^128), which is what turns AES-CTR into an
 * authenticated cipher. The bit order is the awkward part: the standard
 * numbers bits from the left, so bit 0 is the top bit of byte 0, and the
 * reduction polynomial appears as 0xe1 in the first byte.
 *
 * BRANCHLESS, AND THIS ONE IS A NETWORK-OBSERVABLE LEAK RATHER THAN A CACHE
 * ONE. Both of the `if`s this loop used to contain tested a secret:
 *
 *   - `if (x[i / 8] & bit) z ^= v;` -- x is the GHASH accumulator, a value
 *     derived from the key and the data, and the number of set bits in it
 *     decided how many 16-byte XORs ran.
 *   - `if (lsb) v[0] ^= 0xe1;` -- lsb comes from v, which begins each call as
 *     H = AES(key, 0^128), the GCM authentication subkey. The sequence of 128
 *     lsb values is a function of H ALONE, so it is the same timing signature
 *     on every single block of every record of a connection, repeated as many
 *     times as there are blocks and therefore averagable down to nothing.
 *     Recovering H does not decrypt anything, and does not need to: H is the
 *     whole of GHASH, so an attacker holding it can forge a tag for a
 *     ciphertext of their choosing under a key they do not know.
 *
 * That is a worse position than lib/aes.c's S-box, which needs an attacker
 * sharing a cache with this process (see the long note in aes.h). This one is
 * reachable by anybody who can time responses over the network, which for the
 * sync server is anybody at all.
 *
 * The transformation is mechanical and exact rather than approximate, which is
 * the only reason it was in scope: both branches were already
 * `accumulator ^= constant-or-value`, and XOR against a value masked to zero is
 * the same accumulator. Both candidates are now always computed and only the
 * MASK differs, so the instruction trace and the memory access pattern are
 * identical for every input. Nothing here reads memory at a secret index,
 * which is the half a select cannot fix and the half aes.c has.
 *
 * THE STANDING CAVEAT, the same one lib/ct.h carries: this is C, and a
 * sufficiently clever compiler may re-derive a branch from a mask. Nothing in
 * this repository can test for that. What is claimed is that the SOURCE no
 * longer asks for one. */
static void gf_mul(uint8_t x[16], const uint8_t y[16])
{
   uint8_t z[16] = {0};
   uint8_t v[16];
   memcpy(v, y, 16);
   for (int i = 0; i < 128; i++) {
      /* Normalised to exactly 0 or 1 by the shift, because ct_mask64 negates
       * its argument to build the mask and would select neither operand for a
       * 2. `i` is a loop counter, not a secret, so indexing by it is fine. */
      const uint64_t bit = (uint64_t)(x[i / 8] >> (7 - (i % 8))) & 1u;
      const uint64_t bm  = ct_mask64(bit);
      for (int j = 0; j < 16; j++)
         z[j] ^= (uint8_t)(v[j] & bm);
      /* v = v >> 1, and if a 1 fell off the end, reduce. The shift itself was
       * always data independent; it is the reduction that told on H. */
      const uint64_t lsb = (uint64_t)v[15] & 1u;
      for (int j = 15; j > 0; j--)
         v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
      v[0] >>= 1;
      v[0] ^= (uint8_t)(0xe1u & ct_mask64(lsb));
   }
   memcpy(x, z, 16);
}

/* GHASH over `n` bytes, continuing the accumulator `y`. Whole blocks only;
 * the caller pads. */
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *data,
                  size_t n)
{
   while (n >= 16) {
      for (int i = 0; i < 16; i++)
         y[i] ^= data[i];
      gf_mul(y, h);
      data += 16;
      n -= 16;
   }
   if (n) {
      uint8_t last[16] = {0};
      memcpy(last, data, n);
      for (int i = 0; i < 16; i++)
         y[i] ^= last[i];
      gf_mul(y, h);
   }
}

static void be64(uint8_t out[8], uint64_t v)
{
   for (int i = 7; i >= 0; i--) {
      out[i] = (uint8_t)(v & 0xff);
      v >>= 8;
   }
}

/* CTR keystream starting at counter `ctr`, XORed over `n` bytes.
 *
 * THE COUNTER CANNOT WRAP ANY MORE, and that is a property of the callers
 * rather than of this loop: every public entry point has already established
 * n <= GCM_PT_MAX, which is exactly GCM_CTR_BLOCKS_MAX blocks, so starting at
 * 2 this `ctr++` reaches 0xFFFFFFFF at the very most. gcm.h's counter walk is
 * the derivation and the _Static_asserts at the top of this file are the check.
 * Left as a plain `uint32_t` increment on purpose -- inc32 modulo 2^32 IS what
 * SP 800-38D 6.2 specifies, so the arithmetic here was never the defect. The
 * defect was that nothing bounded how many times it ran. */
static void gctr(const uint8_t key[16], const uint8_t iv[12], uint32_t ctr,
                 const uint8_t *in, uint8_t *out, size_t n)
{
   uint8_t blk[16], ks[16];
   memcpy(blk, iv, 12);
   while (n) {
      blk[12] = (uint8_t)(ctr >> 24);
      blk[13] = (uint8_t)(ctr >> 16);
      blk[14] = (uint8_t)(ctr >> 8);
      blk[15] = (uint8_t)ctr;
      aes128_encrypt(key, blk, ks);
      size_t take = n < 16 ? n : 16;
      for (size_t i = 0; i < take; i++)
         out[i] = in[i] ^ ks[i];
      in += take;
      out += take;
      n -= take;
      ctr++;
   }
}

/* The authentication tag over (aad, ct), for a given key and nonce. */
static void gcm_tag(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t ctn, uint8_t tag[16])
{
   uint8_t h[16] = {0}, zero[16] = {0}, y[16] = {0}, len[16];
   aes128_encrypt(key, zero, h);
   ghash(y, h, aad, aadn);
   ghash(y, h, ct, ctn);
   /* THE LENGTH BLOCK, and the multiply that used to be able to lie. Both
    * products are exact now because both operands were bounded before this
    * function was reached: aadn <= 2^61 - 1 keeps aadn * 8 inside 2^64, and
    * ctn <= 2^36 - 32 is nowhere near it. Unbounded, `(uint64_t)aadn * 8`
    * wrapped, and an AAD of 2^61 bytes encoded the same zero length block as
    * an empty one -- see gcm.h on why a non-injective length encoding stops
    * the tag from committing to anything. */
   be64(len, (uint64_t)aadn * 8);
   be64(len + 8, (uint64_t)ctn * 8);
   ghash(y, h, len, 16);
   gctr(key, iv, 1, y, tag, 16); /* counter 1 masks the tag */
}

enum gcm_status aes128_gcm_limits(uint64_t aadn, uint64_t n)
{
   /* Two comparisons and nothing else -- no key, no buffer, no allocation --
    * which is the entire point of it being a separate function. It is what a
    * test can call with 2^36 and 2^61 without owning that much memory. The
    * order is documented in gcm.h and depended upon there, so it is not free
    * to change: aadn is judged first. */
   if (aadn > GCM_AAD_MAX)
      return GCM_ERR_AAD_LEN;
   if (n > GCM_PT_MAX)
      return GCM_ERR_PT_LEN;
   return GCM_OK;
}

enum gcm_status aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                                const uint8_t *aad, size_t aadn,
                                const uint8_t *pt, size_t n, uint8_t *ct,
                                uint8_t tag[16])
{
   /* EVERYTHING IS JUDGED BEFORE ANY OUTPUT EXISTS, including before the tag.
    * A partially sealed buffer is the worse of the two failures: it looks
    * exactly like a record. And a written tag is worse still, because a caller
    * that ignored the status will hand those 16 bytes to a peer as though they
    * meant something.
    *
    * A zero-length plaintext and a zero-length AAD are both LEGAL -- GCM with
    * an empty plaintext is GMAC, and the GCM specification's own test case 1 is
    * empty/empty -- so the NULL rules are written as "a NULL that claims to
    * have bytes behind it" rather than as "a NULL". */
   if (!key || !iv || !tag || (!aad && aadn) || (!pt && n) || (!ct && n))
      return GCM_ERR_ARG;
   const enum gcm_status st = aes128_gcm_limits((uint64_t)aadn, (uint64_t)n);
   if (st != GCM_OK)
      return st;

   gctr(key, iv, 2, pt, ct, n); /* data starts at counter 2 */
   gcm_tag(key, iv, aad, aadn, ct, n, tag);
   return GCM_OK;
}

enum gcm_status aes128_gcm_unseal(const uint8_t key[16], const uint8_t iv[12],
                                  const uint8_t *aad, size_t aadn,
                                  const uint8_t *ct, size_t n,
                                  const uint8_t tag[16], uint8_t *pt)
{
   /* The same rules in the same order, so a caller sealing and opening the
    * same lengths gets the same enumerator for the same reason. gcm.h sets out
    * why an opener refuses a length at all -- briefly, because a conforming
    * sealer cannot have produced it. */
   if (!key || !iv || !tag || (!aad && aadn) || (!ct && n) || (!pt && n))
      return GCM_ERR_ARG;
   const enum gcm_status st = aes128_gcm_limits((uint64_t)aadn, (uint64_t)n);
   if (st != GCM_OK)
      return st;

   uint8_t want[16];
   gcm_tag(key, iv, aad, aadn, ct, n, want);
   /* Constant time: a comparison that returns early tells an attacker how
    * many bytes of a forged tag were right. */
   uint8_t diff = 0;
   for (int i = 0; i < 16; i++)
      diff |= (uint8_t)(want[i] ^ tag[i]);
   if (diff)
      return GCM_ERR_TAG;
   gctr(key, iv, 2, ct, pt, n);
   return GCM_OK;
}

int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t n, const uint8_t tag[16], uint8_t *pt)
{
   /* The polarity is the whole reason this wrapper exists; see gcm.h. */
   return aes128_gcm_unseal(key, iv, aad, aadn, ct, n, tag, pt) == GCM_OK;
}
