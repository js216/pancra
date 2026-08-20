// SPDX-License-Identifier: GPL-3.0
// jpake.h --- EC-J-PAKE over P-256 (API)
// Copyright 2026 Juggluco and xDrip contributors

/* Password-authenticated key agreement: two parties who share a short secret
 * end up with the same 16-byte key, and a recording of the exchange does not
 * let anyone test guesses at that secret offline.
 *
 * Nothing here is specific to any device or protocol. Both halves of this
 * project use it -- the app to pair with a sensor, the server to pair with
 * the app -- and it depends on nothing but lib/p256, lib/sha256 and lib/rand.
 */
#ifndef JPAKE_H
#define JPAKE_H

#include <stddef.h>
#include <stdint.h>

/* Initialise the curve context. Call once before anything else. Returns 1. */
int jpake_init(void);

/* Exchange 160-byte round packets with the peer, then derive the shared key.
 * is_client selects the signer identity: the two sides MUST disagree about
 * it, or the Schnorr proofs are computed over the same label and the protocol
 * loses the property that makes it worth using.
 *
 * The peer_* calls return 1 only if the received zero-knowledge proof
 * validates. A caller that ignores that has an unauthenticated key exchange:
 * the proofs are the entire defence against a man in the middle.
 *
 * A CRYPTOGRAPHIC FAILURE ENDS THE EXCHANGE. Any of these returning 0
 * BECAUSE A PACKET WAS PARSED AND FOUND WRONG -- a point off the curve, a
 * Schnorr proof that does not hold, a combined point at infinity -- leaves
 * the object permanently refusing: it will not emit another round, accept
 * another round, or produce a key, and the rounds it had already accepted are
 * cleared. There is no way to retry a round in place -- allocate a fresh
 * struct jpake and start again from round 1. That is deliberate, and the
 * reasoning (including what an accepted-but-unproven round used to be worth
 * to an attacker) is at the top of struct jpake in jpake.c.
 *
 * A PACKET ARRIVING OUT OF PHASE IS DIFFERENT, and it is the one refusal that
 * is NOT terminal. See jpake_phase below: a duplicate or out-of-order round is
 * refused before it is decoded, so nothing about it has been believed, and the
 * exchange is left exactly as it was -- still holding the round it had already
 * accepted, still able to finish. Poisoning there would hand any peer that
 * repeats a packet a way to kill an otherwise healthy pairing, and it would
 * make "the accepted round survives a duplicate" untestable, since everything
 * would be cleared either way.
 *
 * A peer round is also never PUBLISHED until its proof holds, so a caller that
 * does keep using the object after a failure -- against the rule above -- still
 * cannot find peer-chosen values in it.
 *
 * NULL is a legal argument to every function here: it reads as a poisoned
 * exchange, so an allocation failure from jpake_new cannot turn into a crash
 * at a call site that forgot to check. */
struct jpake;

/* THE PASSWORD: EXACTLY WHAT THESE BYTES ARE, because two callers disagreeing
 * about it is a pairing that fails with nothing to look at.
 *
 * ENCODING. `pass` is passlen RAW BYTES -- not a C string, not text in any
 * particular character set, and NOT null-terminated (the terminator is never
 * read and must not be counted in passlen). They are placed RIGHT-JUSTIFIED in
 * a 32-byte big-endian buffer, zero-padded on the LEFT, and that 256-bit
 * unsigned integer is reduced mod the group order n to become the J-PAKE
 * password scalar. So the four ASCII bytes "1155" become the integer
 * 0x0000...0031313535 and NOT 0x31313535 followed by 28 zeros: a peer that
 * left-justifies agrees with this implementation about nothing.
 *
 * A LEADING ZERO BYTE IS THEREFORE INVISIBLE: {0x00, 'a'} and {'a'} are the
 * same password. That falls out of reading the bytes as a number; it is stated
 * rather than fixed, because every caller in this repo passes printable
 * pairing codes.
 *
 * DOMAIN. 1 <= passlen <= JPAKE_PASS_MAX, `pass` non-NULL, and the resulting
 * scalar nonzero. Everything else returns NULL:
 *
 *   - passlen > JPAKE_PASS_MAX. This used to CLIP TO THE FIRST 32 BYTES
 *     (`if (passlen > 32) passlen = 32;` before a memcpy of passlen bytes), so
 *     a 33-byte password and its 32-byte PREFIX were the same pairing secret
 *     with no way for either side to notice -- two users who chose different
 *     passphrases sharing a key, reported to nobody. A password this
 *     construction cannot represent is a caller bug, and pairing on a
 *     different secret is the worst available answer to it.
 *   - passlen == 0, or pass == NULL with passlen != 0. The first used to
 *     produce the all-zero scalar below; the second was a memcpy from NULL.
 *   - a scalar that reduces to ZERO: all-zero password bytes, or bytes equal
 *     to n. Zero is not a J-PAKE password -- x2s = privB*pass and the key
 *     exponent x2*pass both collapse, so both sides would derive from a point
 *     that no longer depends on the shared secret at all, which is the one
 *     outcome this protocol exists to prevent.
 *
 * NOT REFUSED, and worth knowing: bytes at or above n are reduced, so two
 * 32-byte binary passwords differing by exactly n are the same secret. Every
 * caller here passes printable codes, which cannot reach n (32 printable ASCII
 * bytes stay below 2^255), so this is documented rather than enforced --
 * unlike the ECDSA scalars in lib/ecdsa.c, where the aliased value is chosen
 * by an attacker and the reduction was a forgery. */
#define JPAKE_PASS_MAX 32
struct jpake *jpake_new(const uint8_t *pass, size_t passlen, int is_client);
int jpake_round1(struct jpake *p, uint8_t out[160]);
int jpake_round2(struct jpake *p, uint8_t out[160]);
int jpake_round3(struct jpake *p, uint8_t out[160]);
int jpake_peer_round1(struct jpake *p, const uint8_t in[160]);
int jpake_peer_round2(struct jpake *p, const uint8_t in[160]);
int jpake_peer_round3(struct jpake *p, const uint8_t in[160]);
int jpake_shared_key(struct jpake *p, uint8_t out16[16]);
void jpake_free(struct jpake *p);

/* ---- THE TRANSCRIPT PHASE: WHICH PEER PACKET COMES NEXT ----------------
 *
 * An EC-J-PAKE transcript is three peer packets, in one order, each exactly
 * once. That was previously nowhere in the object. Three independent flags
 * recorded which rounds had been accepted and each entry point tested whichever
 * subset it happened to need, so the object accepted transcripts the protocol
 * has no meaning for: a second round-1 packet OVERWROTE the first one's
 * ephemeral public key -- which is a base point of round 3 and a term in the
 * derived key -- and a round-3 packet handed in before round 2 was decoded and
 * verified against a base nobody had proven anything about.
 *
 * This counter replaces the flags. It only ever counts up, one round at a
 * time, and every entry point names the single phase it belongs to, so a
 * duplicate and an out-of-order packet are the same refusal and both happen
 * before the 160 bytes are looked at.
 *
 * The value IS the number of peer rounds accepted, which is why it doubles as
 * the derivation gate: JPAKE_PHASE_DONE means exactly one accepted packet of
 * every required round, and jpake_shared_key demands precisely that instead of
 * testing two flags out of three.
 *
 * A poisoned or NULL exchange answers JPAKE_PHASE_R1 -- it holds nothing.
 * WATCH THE SENSE: JPAKE_PHASE_R1 is 0 and means the EMPTY transcript, not
 * "round 1 is in". */
enum jpake_phase {
   JPAKE_PHASE_R1 = 0, /* nothing accepted; peer round 1 is the only packet */
   JPAKE_PHASE_R2,     /* peer round 1 accepted; peer round 2 is next */
   JPAKE_PHASE_R3,     /* rounds 1 and 2 accepted; peer round 3 is next */
   JPAKE_PHASE_DONE    /* all three accepted, once each: a key can be derived */
};

enum jpake_phase jpake_phase(const struct jpake *p);

/* WHAT STATE IS THIS EXCHANGE IN? These exist because the bug they guard
 * against was invisible from outside: the object's own record of "this round is
 * done and its proof passed" was not observable, so nothing could assert on it
 * and nothing did. `round` is 1, 2 or 3; anything else answers 0.
 *
 * jpake_accepted() means PROVEN, not merely received -- it reads the phase
 * above, which the key derivation gates on. jpake_poisoned() answers 1 for a
 * NULL pointer too, because a pairing that could not be allocated is a pairing
 * that must not proceed. Neither is required for the protocol: the entry points
 * enforce all of this themselves. They are for tests, for logging, and for a
 * caller that wants to know rather than infer. */
int jpake_accepted(const struct jpake *p, int round);
int jpake_poisoned(const struct jpake *p);

#endif
