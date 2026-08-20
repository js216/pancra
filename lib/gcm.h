/* SPDX-License-Identifier: GPL-3.0
 * gcm.h --- AES-128-GCM authenticated encryption (NIST SP 800-38D)
 * Copyright 2026 Jakob Kastelic
 *
 * Counter mode for secrecy, GHASH for authenticity, over lib/aes.c's block
 * function. The 12-byte nonce is the only size worth supporting: it is what
 * TLS uses and the only one that needs no extra GHASH pass to build the
 * counter block.
 */
#ifndef GCM_H
#define GCM_H
#include <stddef.h>
#include <stdint.h>

/* ---- THE PER-INVOCATION LIMITS, AND WHY GCM HAS THEM AT ALL --------------
 *
 * SP 800-38D 5.2.1.1 bounds every input to authenticated encryption:
 *
 *     len(P) <= 2^39 - 256      (the plaintext, in BITS)
 *     len(A) <= 2^64 - 1        (the additional authenticated data, in BITS)
 *     1 <= len(IV) <= 2^64 - 1  (fixed at 96 bits here, so nothing to check)
 *
 * These are not throughput advice. GCM is a stream cipher construction and the
 * first of them is the only thing standing between it and KEYSTREAM REUSE.
 *
 * THE COUNTER. With a 96-bit IV the standard fixes J0 = IV || 0^31 || 1, and
 * GCTR walks the rightmost 32 bits with inc32 -- increment modulo 2^32. Counter
 * 1 is J0 and its keystream block masks the tag; the data starts at counter 2.
 * So the 1-based data block i uses counter (uint32_t)(1 + i), and:
 *
 *     i = 1              -> 2            the first block
 *     i = 2^32 - 2       -> 0xFFFFFFFF   the LAST value that has not wrapped
 *     i = 2^32 - 1       -> 0            wrapped; a counter conforming GCM
 *                                        never uses at all
 *     i = 2^32           -> 1            THE TAG MASK. This block's ciphertext
 *                                        is pt XOR S, so anyone who can guess
 *                                        those 16 plaintext bytes RECOVERS S
 *                                        and can forge tags for this (key, IV)
 *     i = 2^32 + 1       -> 2            block 1's keystream, AGAIN
 *
 * 2^39 - 256 bits is exactly 2^36 - 32 bytes is exactly 2^32 - 2 blocks -- the
 * largest count for which that walk never leaves {2, ..., 0xFFFFFFFF}. The
 * limit is not approximately the counter's capacity, it IS the counter's
 * capacity, which is why one constant below is derived from the other and the
 * two are tied together by a _Static_assert in gcm.c.
 *
 * WHAT THE OLD CODE DID, written out for whoever is reading this because their
 * traffic was decrypted. gctr() held the counter in a `uint32_t` and did
 * `ctr++` per block, and nothing anywhere compared the length against
 * anything. Past the bound the counter simply wrapped, silently, and produced
 * PERFECTLY WELL-FORMED CIPHERTEXT with a tag that verified. There was no
 * error, no log line and no return value -- aes128_gcm_seal returned void.
 * From byte 2^36 - 32 onward one message's own keystream repeats every 2^36
 * bytes, so ct[j] XOR ct[j + 2^36] hands over pt[j] XOR pt[j + 2^36] to
 * anybody holding the ciphertext, with no key recovery and no cryptanalysis:
 * one XOR and a guess at English, or at CSV, or at a glucose reading. Two
 * ciphertexts sealed under the same key and nonce give up their plaintexts the
 * same way. This is the TLS record layer for the phone-to-server sync, so the
 * plaintext is somebody's glucose history.
 *
 * REACHABILITY, stated rather than implied, because overstating it would be its
 * own kind of dishonesty: NOTHING IN THIS REPOSITORY CAN REACH EITHER BOUND
 * TODAY. srv/tls.c seals at most REC_MAX + 1 = 16385 bytes per record and its
 * session tickets are 40 bytes. The bounds are here because this file is a
 * generic primitive that says so in its own first paragraph -- it does not know
 * what a TLS record is, and the next caller may compute a length from something
 * a peer sent. A primitive whose safety rests on the arithmetic of its current
 * callers is a primitive that is one call site away from being wrong. */

/* The block-count capacity of a 32-bit counter that starts at 2 and must never
 * wrap. Spelled as a suffixed integer constant, not a cast, so it can be used
 * in #if -- srv/test/cryptotest.c needs that to decide whether this platform's
 * size_t can even express a length past the bound. */
#define GCM_CTR_BLOCKS_MAX 0xFFFFFFFEull /* 2^32 - 2 */

/* SP 800-38D 5.2.1.1's len(P) <= 2^39 - 256 bits, in BYTES: 68719476704, which
 * is 2^36 - 32, which is GCM_CTR_BLOCKS_MAX * 16. About 64 GiB. */
#define GCM_PT_MAX (GCM_CTR_BLOCKS_MAX * 16ull)

/* SP 800-38D 5.2.1.1's len(A) <= 2^64 - 1 bits, in BYTES: 2^61 - 1.
 *
 * This one is not about the counter -- AAD is never encrypted -- it is about
 * the LENGTH BLOCK. GHASH's final input is the 128-bit pair
 * (len(A) in bits, len(C) in bits), and that is what makes a tag commit to how
 * the authenticated bytes were divided between AAD and ciphertext. gcm.c builds
 * it with `(uint64_t)aadn * 8`, a multiply that wraps modulo 2^64, so at
 * aadn = 2^61 the encoded length is 0 -- the same length block an EMPTY AAD
 * produces. The encoding stops being injective, and a tag that does not
 * determine the lengths it covers is not GCM's tag. The ciphertext half of the
 * pair cannot wrap once GCM_PT_MAX is enforced (2^36 * 8 is nowhere near
 * 2^64), so this bound is the only thing keeping the length block honest. */
#define GCM_AAD_MAX 0x1FFFFFFFFFFFFFFFull /* 2^61 - 1 */

/* Typed, for the same reason hkdf.h and pbkdf2.h are: "it refused" sends the
 * next reader to the wrong one of four unrelated causes. An oversized
 * plaintext is a caller that did not chunk; an oversized AAD is a length that
 * came from somewhere it should not have; a NULL is a plain bug; and a failed
 * tag is not a bug at all, it is the function doing its job. */
enum gcm_status {
   GCM_OK = 0,
   GCM_ERR_ARG,     /* a NULL where bytes were promised */
   GCM_ERR_AAD_LEN, /* aadn > GCM_AAD_MAX (SP 800-38D 5.2.1.1) */
   GCM_ERR_PT_LEN,  /* n > GCM_PT_MAX (SP 800-38D 5.2.1.1) */
   GCM_ERR_TAG      /* unseal only: the tag did not verify */
};

/* THE LENGTH RULE ON ITS OWN, as a pure function of two byte counts.
 *
 * It is split out because it is the only part of this file a test can actually
 * DRIVE TO ITS BOUNDARY. GCM_PT_MAX is 64 GiB and GCM_AAD_MAX is 2 EiB; no
 * test allocates those, so "a plaintext of exactly GCM_PT_MAX is accepted" can
 * never be asserted against the sealer. Asserted against this, it can --
 * exactly, and one byte either side of exactly.
 *
 * Both parameters are uint64_t and NOT size_t on purpose. The bounds belong to
 * SP 800-38D, not to this platform's address space, so they are stated in the
 * width the standard's own arithmetic needs; a 32-bit size_t would otherwise
 * silently reinterpret them as "whatever fits". aes128_gcm_seal and
 * aes128_gcm_unseal widen their size_t arguments and call this, so the sealer
 * and the test are pinned to the same predicate rather than to two copies of a
 * comparison. Read the test's claims accordingly: the sealer's REFUSALS are
 * executed, and so is this predicate at both boundaries, but the sealer has
 * never been run at a legal 64 GiB and says so.
 *
 * aadn is judged before n, so a call that breaks both reports GCM_ERR_AAD_LEN.
 * Fixed order, stated here, because a test that cannot predict which rule fires
 * cannot isolate either one. */
enum gcm_status aes128_gcm_limits(uint64_t aadn, uint64_t n)
    __attribute__((warn_unused_result));

/* GCM_OK, or a status with NEITHER `ct` NOR `tag` TOUCHED.
 *
 * ct may alias pt. tag is written separately, never appended.
 *
 * REFUSES, NEVER CLAMPS, and this is the one place in lib/ where clamping
 * would be actively catastrophic rather than merely confusing: sealing the
 * first GCM_PT_MAX bytes of an over-long plaintext produces a valid,
 * verifying, perfectly readable record that silently drops the rest. Every
 * bound is checked before the first byte of `ct` and, separately worth saying,
 * before the first byte of `tag` -- the tag is the value a caller is likeliest
 * to treat as meaningful whatever the return code said, so a refusal that
 * wrote 16 bytes there would be a refusal a caller could not act on.
 *
 * NO warn_unused_result, DELIBERATELY, AND THIS IS A LOOSE END rather than a
 * decision I am happy with. Adding it is a one-line change and the attribute
 * belongs here; what stops it is srv/tls.c:451, which calls this as a bare
 * statement and would fail to compile under -Werror. srv/tls.c is mid-audit
 * for TODO items 56-57 and is not mine to edit, so the attribute waits for the
 * patch quoted in the report for item 64. Note what is and is not at risk in
 * the meantime: the void return is gone, the limits are enforced, and a refused
 * seal writes nothing -- so the keystream reuse is closed. What remains is that
 * that one call site cannot NOTICE a refusal, and would transmit a record built
 * from an uninitialised stack buffer if one ever happened. It cannot happen
 * there: `inner` is at most REC_MAX + 1. */
enum gcm_status aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                                const uint8_t *aad, size_t aadn,
                                const uint8_t *pt, size_t n, uint8_t *ct,
                                uint8_t tag[16])
    __attribute__((warn_unused_result));

/* GCM_OK with `pt` written, or a status with `pt` LEFT EXACTLY AS IT WAS.
 * The tag comparison is constant time.
 *
 * WHAT AN OPENER HAS TO REJECT, AND WHY IT IS NOT OBVIOUS. The instinct is
 * that decryption needs no length bound, because an attacker who feeds us a
 * 100 GiB ciphertext learns nothing from our keystream repeating -- they would
 * need the plaintext to XOR against. That instinct is wrong for three separate
 * reasons and the third is the load-bearing one:
 *
 *   - SP 800-38D 5.2.1.1 bounds C to the same length as P. A ciphertext past
 *     GCM_PT_MAX therefore CANNOT HAVE BEEN PRODUCED BY A CONFORMING SEALER.
 *     Accepting it is agreeing to process something no honest peer ever sent,
 *     and doing so is what turns our own defect into interoperability: two
 *     copies of the old code would have agreed with each other perfectly, both
 *     wrapping the counter identically, and the tag would have verified. A bug
 *     both ends share is a bug no test over the wire can see.
 *   - The tag we would compute for it is not GCM's tag, because its length
 *     block is the wrapped one described above. "Verified" would not mean what
 *     the word means.
 *   - It is 64 GiB of AES and GHASH, and a decision to spend it, on behalf of a
 *     peer who has not yet authenticated anything. Refusing before the first
 *     block is also refusing the denial of service.
 *
 * So the same two bounds, the same fixed order, checked before any work. */
enum gcm_status aes128_gcm_unseal(const uint8_t key[16], const uint8_t iv[12],
                                  const uint8_t *aad, size_t aadn,
                                  const uint8_t *ct, size_t n,
                                  const uint8_t tag[16], uint8_t *pt)
    __attribute__((warn_unused_result));

/* 1 if the ciphertext was authentic AND acceptable, 0 otherwise -- and on 0 the
 * plaintext is NOT written, so a caller that ignores the result still cannot
 * act on forged bytes.
 *
 * A ONE-LINE WRAPPER OVER aes128_gcm_unseal, kept because its polarity is load
 * bearing at two call sites this change is not allowed to touch. srv/tls.c:482
 * and srv/tls.c:1129 both read `if (!aes128_gcm_open(...)) reject;`. Had the
 * typed status simply replaced the int, GCM_OK == 0 would have inverted both of
 * them into rejecting every good record -- and no compiler diagnoses `!` on an
 * enum, so -Werror would have said nothing and the two tests that would have
 * caught it are a shell script and a live handshake. A silent polarity flip in
 * a tag check is the same class of bug as the one this whole item is about, so
 * the boolean keeps its name, its type and its meaning, and the reason is
 * written down rather than remembered.
 *
 * The contract WIDENS, which is why both existing call sites remain correct
 * without being edited: 0 now also covers "a length SP 800-38D forbids" and "a
 * NULL argument", not only "the tag did not match". Both sites reject on 0.
 * Callers wanting to tell a forgery from a caller bug -- for a log line, or a
 * metric -- should call aes128_gcm_unseal and read the status. */
int aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t *aad, size_t aadn, const uint8_t *ct,
                    size_t n, const uint8_t tag[16], uint8_t *pt)
    __attribute__((warn_unused_result));

#endif
