// SPDX-License-Identifier: GPL-3.0
// tlscrypttest.c --- the TLS primitives against the published answers
// Copyright 2026 Jakob Kastelic

/* These are not our numbers. Every vector below is copied from the document
 * that defines the algorithm -- NIST's GCM validation set, RFC 5869 for HKDF,
 * RFC 8448 for the TLS 1.3 key schedule -- so a pass means our code agrees
 * with the specification rather than with itself.
 *
 * A cipher that is wrong in a way only a peer notices is the worst kind of
 * bug to chase, so none of this gets near a socket until this file passes.
 */
#include "ecdsa.h"
#include "gcm.h"
#include "hkdf.h"
#include "hmac.h"
#include "ct.h"
#include "jpake.h"
#include "p256.h"
#include "pbkdf2.h"
#include "rand.h"
#include "sha256.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h> /* setenv: the injected clock is read from the environment,
                     * like srv/db.c's DB_FAIL_* hooks */
#include <string.h>
/* POSIX, for the item-65 TLS case at the end: a socketpair is what lets a real
 * tls_handshake() run against a canned ClientHello with no listener, no
 * certificate and no client. Every other case in this file is pure
 * computation, which is why these two are the only non-C99 headers here. */
#include <sys/socket.h>
#include <unistd.h>

static int fails;
/* ASSERTIONS RUN, not merely assertions that did not fail. A suite whose
 * cases all stopped being reached still prints its success line, which is the
 * one failure mode a green run cannot show you -- so the count is printed and
 * a reviewer can watch it move. */
static int checks;

/* Two stand-in entropy sources. See the boundary case at the end of main. */
static int src_fail(uint8_t *buf, size_t n)
{
   (void)buf;
   (void)n;
   return 0; /* the source is unavailable */
}

static int src_short(uint8_t *buf, size_t n)
{
   /* Fills half and says so: the shape of a short read from /dev/urandom,
    * which is rare enough that a copy ignoring it looks correct for years. */
   for (size_t i = 0; i < n / 2; i++)
      buf[i] = 0xAA;
   return 0;
}

static void hex(const char *s, uint8_t *out, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      unsigned v;
      (void)sscanf(s + 2 * i, "%2x", &v);
      out[i] = (uint8_t)v;
   }
}

static void ck(int ok, const char *what)
{
   checks++;
   if (!ok) {
      printf("  FAIL: %s\n", what);
      fails = 1;
   }
}

/* A REFUSAL THAT WROTE SOMETHING IS NOT A REFUSAL. Every bounds case below
 * fills its output with a sentinel first and asks this afterwards: the failure
 * these functions used to have was not "no answer", it was a buffer holding
 * PART of a key, or holding a key derived from silently edited inputs, which
 * is indistinguishable from the real thing at the call site. */
static int unwritten(const uint8_t *p, size_t n, uint8_t sentinel)
{
   for (size_t i = 0; i < n; i++)
      if (p[i] != sentinel)
         return 0;
   return 1;
}

static void cmp(const uint8_t *got, const char *want_hex, size_t n,
                const char *what)
{
   uint8_t want[128];
   hex(want_hex, want, n);
   checks++;
   if (memcmp(got, want, n) != 0) {
      printf("  FAIL: %s\n    got  ", what);
      for (size_t i = 0; i < n; i++)
         printf("%02x", got[i]);
      printf("\n    want %s\n", want_hex);
      fails = 1;
   }
}

/* ---- A DETERMINISTIC ENTROPY SOURCE, so a WHOLE J-PAKE EXCHANGE has a known
 * ---- answer
 *
 * jpake_new draws five secret scalars per side, so an exchange is normally
 * unreproducible and the only thing a test can say about it is "the two sides
 * agreed with each other" -- which a protocol that is wrong the SAME WAY on
 * both sides satisfies just as well. Pinning the entropy turns the handshake
 * into a known-answer vector: the round-1 packets and the final 16 bytes are
 * fixed, so any change to the arithmetic shows up as a mismatch here.
 *
 * The expected values in the J-PAKE section were computed from lib/jpake.c AS
 * IT STOOD BEFORE the round-publishing fix. That is the whole point of having
 * them: they are the execution evidence that the fix changed WHEN state is
 * written and nothing about what the protocol computes.
 *
 * They are our own numbers rather than published ones. The Juggluco and xDrip
 * reference vectors need make_round3() and shared_key(), which are static
 * inside lib/jpake.c, so they live in its JPAKE_TEST block and cannot be
 * reached from another translation unit.
 *
 * Not a CSPRNG and not pretending to be one -- src_fail and src_short above
 * are the cases about what a real source failing does. This one exists only to
 * be repeatable. */
static uint64_t det_state;

static int src_det(uint8_t *buf, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      det_state = det_state * 6364136223846793005ULL + 1442695040888963407ULL;
      buf[i]    = (uint8_t)(det_state >> 33);
   }
   return 1;
}

/* A MATCHED PAIR, because a J-PAKE packet is only meaningful to the peer it was
 * built for: round 3's proof is verified against (ourA + ourB +
 * peerR1.pubkey1), so one exchange's round 3 is not a valid round 3 in another.
 * Every case below builds its own pair for that reason.
 *
 * Stated because it already caught this test out: the first draft reused one
 * client's round 3 against a freshly allocated server object as its "control",
 * the control failed, and it failed for that reason rather than for the one it
 * was written to demonstrate. A control that does not pass proves nothing about
 * the case it is controlling for. */
struct jp_pair {
   struct jpake *cli, *sen;
   uint8_t c1[160], c2[160], c3[160];
   uint8_t s1[160], s2[160], s3[160];
};

static const uint8_t JP_PIN[6] = {'4', '1', '5', '9', '2', '6'};

/* Both objects allocated and rounds 1 and 2 emitted -- the point from which
 * every case below diverges. Seeding immediately before the two jpake_new
 * calls is what makes the pair reproducible: those are the only calls in the
 * whole exchange that consume entropy. */
static int jp_open(struct jp_pair *j, uint64_t seed)
{
   memset(j, 0, sizeof *j);
   det_state = seed;
   j->cli    = jpake_new(JP_PIN, sizeof JP_PIN, 1);
   j->sen    = jpake_new(JP_PIN, sizeof JP_PIN, 0);
   if (!j->cli || !j->sen)
      return 0;
   return jpake_round1(j->cli, j->c1) == 160 &&
          jpake_round2(j->cli, j->c2) == 160 &&
          jpake_round1(j->sen, j->s1) == 160 &&
          jpake_round2(j->sen, j->s2) == 160;
}

/* Rounds 1 and 2 exchanged BOTH WAYS and every proof accepted. */
static int jp_rounds12(struct jp_pair *j)
{
   return jpake_peer_round1(j->cli, j->s1) &&
          jpake_peer_round2(j->cli, j->s2) &&
          jpake_peer_round1(j->sen, j->c1) && jpake_peer_round2(j->sen, j->c2);
}

static void jp_close(struct jp_pair *j)
{
   jpake_free(j->cli);
   jpake_free(j->sen);
   j->cli = NULL;
   j->sen = NULL;
}

/* A packet whose POINTS ARE UNTOUCHED and whose Schnorr proof scalar is not.
 * That is what makes a case built on it isolating: cert_from_bytes still
 * decodes both points, so validate_zkp is the only rule left that can refuse
 * it. A case that mangled the whole packet would be caught by the decoder and
 * would say nothing at all about the proof check. */
static void jp_spoil_proof(uint8_t out[160], const uint8_t in[160])
{
   memcpy(out, in, 160);
   out[159] ^= 0x01; /* the proof scalar is bytes 128..159 */
}

/* A packet an attacker chose that CANNOT DECODE: pubkey1 is left genuine and
 * pubkey2's coordinates are set past the field prime. In the old code
 * cert_from_bytes had already written pubkey1 into the persistent round before
 * it could discover pubkey2 was unusable -- so this is the shape that replaced
 * half of an ALREADY ACCEPTED round with peer bytes while have_r* stayed 1. */
static void jp_spoil_point(uint8_t out[160], const uint8_t in[160])
{
   memcpy(out, in, 160);
   memset(out + 64, 0xFF, 64);
}

/* ==== ITEM 65 FIXTURES: A SCRIPTED ENTROPY SOURCE, AND ONE REAL HANDSHAKE ===
 *
 * src_det above is repeatable; this one is CHOSEN. p256_sc_rand's whole job is
 * to decide which 32-byte draws are acceptable scalars, and the only way to ask
 * it about the values that matter -- zero, exactly n, n-1, one, the tail above
 * n -- is to be the source and hand them over. That is the second thing
 * rand_set_source buys, after "what happens when entropy fails": WHAT HAPPENS
 * WHEN ENTROPY IS BAD IN A PARTICULAR WAY, which is the case a working
 * /dev/urandom will not produce in the lifetime of the universe and a broken
 * one produces immediately.
 *
 * The script serves its entries in order and then REPEATS THE LAST FOREVER, so
 * "every draw is out of range" is a one-entry script and "two rejects then a
 * good value" is a three-entry script. scf_calls counts every ask, including
 * the ones a refusing source answered, because the retry cap and the
 * propagate-don't-retry rule are both statements about HOW MANY TIMES the
 * source was asked and cannot be checked any other way.
 *
 * Asks that are not 32 bytes are answered with filler and still counted: the
 * TLS cases below run a whole handshake, which also wants a ServerHello random
 * and a record nonce, and a fixture that failed those would abort the handshake
 * for a reason that has nothing to do with scalars. */
#define SCF_MAX 8
static uint8_t scf_val[SCF_MAX][32];
static int scf_n;     /* entries in the script */
static int scf_i;     /* 32-byte asks so far: the script index */
static int scf_calls; /* asks of ANY size, including refused ones */
static int scf_fail;  /* 1: refuse every ask, like a dead /dev/urandom */

static int src_scalar(uint8_t *buf, size_t n)
{
   scf_calls++;
   if (scf_fail)
      return 0;
   if (n != 32) {
      memset(buf, 0xA5, n);
      return 1;
   }
   int i = scf_i++;
   if (i >= scf_n)
      i = scf_n - 1; /* the last entry repeats */
   memcpy(buf, scf_val[i], 32);
   return 1;
}

static void scf_reset(void)
{
   memset(scf_val, 0, sizeof scf_val);
   scf_n     = 0;
   scf_i     = 0;
   scf_calls = 0;
   scf_fail  = 0;
}

static void scf_push(const char *be_hex)
{
   hex(be_hex, scf_val[scf_n], 32);
   scf_n++;
}

/* The group order and its neighbours, written out because the boundary is the
 * whole point: n itself must be refused (the old code reduced it to zero and
 * handed that back as a scalar) and n-1 must be accepted (the largest valid
 * private key there is -- a generator that refused it would be quietly
 * throwing away a scalar and nothing would ever notice). */
#define SC_N "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"
#define SC_NM1                                                                 \
   "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550"
#define SC_NP1                                                                 \
   "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632552"
#define SC_FF "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
#define SC_0  "0000000000000000000000000000000000000000000000000000000000000000"
#define SC_1  "0000000000000000000000000000000000000000000000000000000000000001"
/* Distinct from every value above, so "the accepted scalar is the LATER draw"
 * is a statement no other entry in a script could satisfy. */
#define SC_OK "1111111111111111111111111111111111111111111111111111111111111111"

/* A minimal TLS 1.3 ClientHello: exactly the four things tls_handshake insists
 * on before it draws the ECDHE scalar -- TLS 1.3 in supported_versions,
 * TLS_AES_128_GCM_SHA256 in the suite list, ecdsa_secp256r1_sha256 in
 * signature_algorithms, and a P-256 key_share -- and nothing else.
 *
 * HAND-BUILT RATHER THAN CAPTURED, deliberately. A recorded hello from a real
 * client is an opaque blob: when the case below stops reaching the ECDHE draw,
 * nothing in the file says which byte stopped it. The share is the curve's own
 * generator, so p256_from_xy accepts it without a hard-coded point that a
 * reader has to take on trust. Returns the whole record, header included. */
static size_t mk_hello(uint8_t *rec, size_t cap)
{
   uint8_t body[512];
   size_t k  = 0;
   body[k++] = 0x03; /* legacy_version: TLS 1.2, as 1.3 requires */
   body[k++] = 0x03;
   for (int i = 0; i < 32; i++)
      body[k++] = (uint8_t)(0x40 + i); /* client random: fixed, unread */
   body[k++]        = 0;               /* session_id: empty */
   body[k++]        = 0;               /* cipher_suites: one, 2 bytes */
   body[k++]        = 2;
   body[k++]        = 0x13; /* TLS_AES_128_GCM_SHA256 */
   body[k++]        = 0x01;
   body[k++]        = 1; /* legacy_compression_methods: { null } */
   body[k++]        = 0;
   size_t extlen_at = k; /* extensions length, filled in at the end */
   k += 2;
   size_t ext0 = k;

   body[k++] = 0; /* supported_versions (43) */
   body[k++] = 43;
   body[k++] = 0;
   body[k++] = 3;
   body[k++] = 2;
   body[k++] = 0x03;
   body[k++] = 0x04;

   body[k++] = 0; /* signature_algorithms (13) */
   body[k++] = 13;
   body[k++] = 0;
   body[k++] = 4;
   body[k++] = 0;
   body[k++] = 2;
   body[k++] = 0x04; /* ecdsa_secp256r1_sha256 */
   body[k++] = 0x03;

   uint8_t g[65];
   p256_uncompressed(&p256_g, g);
   body[k++] = 0; /* key_share (51) */
   body[k++] = 51;
   body[k++] = 0;
   body[k++] = (uint8_t)(2 + 4 + 65);
   body[k++] = 0;
   body[k++] = 4 + 65; /* client_shares length */
   body[k++] = 0;
   body[k++] = 0x17; /* secp256r1 */
   body[k++] = 0;
   body[k++] = 65;
   memcpy(body + k, g, 65);
   k += 65;

   size_t extn         = k - ext0;
   body[extlen_at]     = (uint8_t)(extn >> 8);
   body[extlen_at + 1] = (uint8_t)extn;

   size_t hs = 4 + k; /* handshake header + body */
   if (cap < 5 + hs)
      return 0;
   rec[0] = 22; /* record: handshake */
   rec[1] = 0x03;
   rec[2] = 0x03;
   rec[3] = (uint8_t)(hs >> 8);
   rec[4] = (uint8_t)hs;
   rec[5] = 1; /* ClientHello */
   rec[6] = (uint8_t)(k >> 16);
   rec[7] = (uint8_t)(k >> 8);
   rec[8] = (uint8_t)k;
   memcpy(rec + 9, body, k);
   return 5 + hs;
}

/* One server handshake against that hello. Returns the number of bytes the
 * server WROTE, which is the observable these cases turn on: the ECDHE scalar
 * is drawn before send_server_hello, so "the client was told nothing" is what
 * separates a refusal at the draw from anything that happens later.
 *
 * tls_init is deliberately NOT called. Nothing up to and including the
 * ServerHello needs the certificate, so the handshake reaches the draw without
 * a key pair on disk -- and with g_key left at zero the run always ends at
 * CertificateVerify, which is what keeps this from becoming a full handshake
 * that then blocks waiting for a client Finished. The write half is shut down
 * for the same reason: a later read gets EOF instead of hanging. */
static int hs_bytes_written(int *rc_out)
{
   uint8_t rec[1024];
   size_t rn = mk_hello(rec, sizeof rec);
   int sv[2];
   *rc_out = -1;
   if (rn == 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
      return -2;
   if (write(sv[1], rec, rn) != (ssize_t)rn) {
      close(sv[0]);
      close(sv[1]);
      return -2;
   }
   shutdown(sv[1], SHUT_WR);
   struct tls_conn *c = tls_conn_slot();
   *rc_out            = tls_handshake(c, sv[0], NULL);
   uint8_t back[4096];
   ssize_t got = recv(sv[1], back, sizeof back, MSG_DONTWAIT);
   close(sv[0]);
   close(sv[1]);
   return got > 0 ? (int)got : 0;
}

/* ---- A CLIENTHELLO BUILT ONE FLAW AT A TIME (item 133) -----------------
 *
 * WHY THE FLAWS ARE KNOBS AND NOT PATCHED BYTES. The obvious way to test a
 * parser's bounds is to take a good message and corrupt a length byte. It
 * does not isolate anything: a cipher_suites vector that declares one byte
 * more than it holds moves every field after it, so the hello is refused --
 * by the extension loop, or the compression check, or nothing in particular
 * -- and the assertion passes whatever the cipher-suite cursor does. A
 * review that loosened each vector's exhaustion check in turn would find the
 * suite still red and learn nothing about which rule it broke.
 *
 * So each knob below makes ONE vector genuinely malformed and leaves every
 * other byte of the message exact: the extra byte inside cipher_suites is
 * REALLY there and REALLY declared, the extension block that follows it
 * starts where it says it does, and the only rule the hello breaks is the one
 * the case is named after. That is what makes "loosen this one check" fail
 * this one assertion.
 *
 * THE OBSERVABLE IS THAT NOTHING WAS WRITTEN. tls_handshake returns 0 for
 * every failure it has, including the CertificateVerify at the end that
 * always fails here (no certificate is loaded), so `rc == 0` says almost
 * nothing. A hello refused by the parser is refused BEFORE the ServerHello,
 * so the peer is told nothing at all -- and a hello that got as far as the
 * key exchange has left bytes on the wire whatever happened afterwards. Zero
 * bytes written is the answer that separates them. */
struct ch_flaw {
   int cs_tail;    /* cipher_suites carries a byte that is not a whole suite */
   int comp_two;   /* two compression methods (RFC 8446 4.1.2 allows one, 0) */
   int ver_tail;   /* supported_versions' own list leaves an odd byte */
   int sig_tail;   /* signature_algorithms' own list leaves an odd byte */
   int ks_tail;    /* client_shares leaves a byte after the last share */
   int ks_over;    /* client_shares declares past the end of key_share */
   int modes_tail; /* a byte after ke_modes, inside psk_key_exchange_modes */
   int ext_tail;   /* a stray byte after the last extension, inside the block */
   int ext_over;   /* the extension block declares past the end of the hello */
   int hello_tail; /* a stray byte after the extension block */
   int dup;        /* signature_algorithms sent twice */
   int psk;        /* offer a pre_shared_key -- last, as the RFC requires */
   int idents;     /* identities to write (0 means one) */
   int binders;    /* binders to write (0 means one) */
   int ident_tail; /* a stray byte inside the identity list */
   int binder_tail; /* a stray byte inside the binder list */
   int binder_len;  /* bytes in each binder (0 means 32, which is SHA-256's) */
   const uint8_t *ticket; /* identity 0's bytes; a dummy when NULL */
   size_t ticket_n;
};

/* A writer with no bounds checks on purpose: every message here is a few
 * hundred bytes into a buffer sized for thousands, and a bound would be a
 * second thing to get wrong in the fixture. */
struct wr {
   uint8_t *p;
   size_t k;
};

static void w8(struct wr *w, unsigned v)
{
   w->p[w->k++] = (uint8_t)v;
}

static void w16(struct wr *w, unsigned v)
{
   w8(w, v >> 8);
   w8(w, v & 0xff);
}

static void wn(struct wr *w, const uint8_t *b, size_t n)
{
   memcpy(w->p + w->k, b, n);
   w->k += n;
}

/* Build the whole record. *binder_at is the offset WITHIN THE RECORD of the
 * binder list's 2-byte length -- the truncation point the binder is a MAC
 * over -- or 0 when no pre_shared_key was offered. */
static size_t mk_ch(const struct ch_flaw *f, uint8_t *rec, size_t cap,
                    size_t *binder_at)
{
   uint8_t body[2048];
   struct wr w        = {body, 0};
   int idents         = f->idents ? f->idents : 1;
   int binders        = f->binders ? f->binders : 1;
   size_t blen        = f->binder_len ? (size_t)f->binder_len : 32;
   size_t binder_body = 0;

   *binder_at = 0;
   w16(&w, 0x0303); /* legacy_version: TLS 1.2, as 1.3 requires */
   for (int i = 0; i < 32; i++)
      w8(&w, 0x40 + i); /* client random: fixed, unread */
   w8(&w, 0);           /* legacy_session_id: empty */

   w16(&w, f->cs_tail ? 3 : 2); /* cipher_suites */
   w16(&w, 0x1301);             /* TLS_AES_128_GCM_SHA256 */
   if (f->cs_tail)
      w8(&w, 0);

   if (f->comp_two) { /* legacy_compression_methods */
      w8(&w, 2);
      w8(&w, 0);
      w8(&w, 0);
   } else {
      w8(&w, 1);
      w8(&w, 0);
   }

   size_t extlen_at = w.k;
   w.k += 2;
   size_t ext0 = w.k;

   w16(&w, 43); /* supported_versions */
   w16(&w, f->ver_tail ? 4 : 3);
   w8(&w, f->ver_tail ? 3 : 2);
   w16(&w, 0x0304);
   if (f->ver_tail)
      w8(&w, 0);

   for (int rep = 0; rep < (f->dup ? 2 : 1); rep++) {
      w16(&w, 13); /* signature_algorithms */
      w16(&w, f->sig_tail ? 5 : 4);
      w16(&w, f->sig_tail ? 3 : 2);
      w16(&w, 0x0403); /* ecdsa_secp256r1_sha256 */
      if (f->sig_tail)
         w8(&w, 0);
   }

   uint8_t g[65]; /* the curve's own generator: p256_from_xy accepts it */
   p256_uncompressed(&p256_g, g);
   size_t inner = 4 + 65 + (f->ks_tail ? 1u : 0u);
   w16(&w, 51); /* key_share */
   w16(&w, 2 + inner);
   w16(&w, inner + (f->ks_over ? 8u : 0u)); /* client_shares */
   w16(&w, 0x0017);                         /* secp256r1 */
   w16(&w, 65);
   wn(&w, g, 65);
   if (f->ks_tail)
      w8(&w, 0);

   if (f->psk) {
      w16(&w, 45); /* psk_key_exchange_modes */
      w16(&w, 2 + (f->modes_tail ? 1u : 0u));
      w8(&w, 1);
      w8(&w, 1); /* psk_dhe_ke */
      if (f->modes_tail)
         w8(&w, 0);

      /* A ticket this server never sealed. Every framing case below is
       * decided before the seal is opened, so the bytes only have to be
       * plausible; the resumption cases pass a real one. */
      uint8_t dummy[80];
      memset(dummy, 0x77, sizeof dummy);
      const uint8_t *tk = f->ticket ? f->ticket : dummy;
      size_t tn         = f->ticket ? f->ticket_n : sizeof dummy;
      size_t idlen = (size_t)idents * (2 + tn + 4) + (f->ident_tail ? 1u : 0u);
      size_t blist = (size_t)binders * (1 + blen) + (f->binder_tail ? 1u : 0u);

      w16(&w, 41); /* pre_shared_key, and it must be the last extension */
      w16(&w, 2 + idlen + 2 + blist);
      w16(&w, idlen);
      for (int i = 0; i < idents; i++) {
         w16(&w, tn);
         wn(&w, tk, tn);
         w16(&w, 0); /* obfuscated_ticket_age, which we do not use */
         w16(&w, 0);
      }
      if (f->ident_tail)
         w8(&w, 0);
      binder_body = w.k;
      w16(&w, blist);
      for (int i = 0; i < binders; i++) {
         w8(&w, blen);
         for (size_t j = 0; j < blen; j++)
            w8(&w, 0); /* filled in by fix_binder when it has to verify */
      }
      if (f->binder_tail)
         w8(&w, 1);
   }

   if (f->ext_tail)
      w8(&w, 0);
   size_t extn         = w.k - ext0 + (f->ext_over ? 8u : 0u);
   body[extlen_at]     = (uint8_t)(extn >> 8);
   body[extlen_at + 1] = (uint8_t)extn;
   if (f->hello_tail)
      w8(&w, 0);

   size_t hs = 4 + w.k;
   if (cap < 5 + hs)
      return 0;
   rec[0] = 22; /* record: handshake */
   rec[1] = 0x03;
   rec[2] = 0x03;
   rec[3] = (uint8_t)(hs >> 8);
   rec[4] = (uint8_t)hs;
   rec[5] = 1; /* ClientHello */
   rec[6] = (uint8_t)(w.k >> 16);
   rec[7] = (uint8_t)(w.k >> 8);
   rec[8] = (uint8_t)w.k;
   memcpy(rec + 9, body, w.k);
   if (binder_body)
      *binder_at = 9 + binder_body;
   return 5 + hs;
}

/* The binder for identity 0, computed the way RFC 8446 4.2.11.2 says: the
 * "res binder" key schedule over the PSK, HMACed over Truncate(ClientHello)
 * -- the handshake message, header included, up to and not including the
 * binder list. Written straight into the message. */
static void fix_binder(uint8_t *rec, size_t binder_at, const uint8_t psk[32])
{
   uint8_t early[32], bk[32], fk[32], th[32];
   hkdf_extract(NULL, 0, psk, 32, early);
   ck(derive_secret(early, "res binder", (const uint8_t *)"", 0, bk) &&
          hkdf_expand_label(bk, "finished", NULL, 0, fk, 32),
      "the fixture can derive a binder key");
   sha256(rec + 5, binder_at - 5, th);
   hmac_sha256(fk, 32, th, 32, rec + binder_at + 3);
}

/* What the server did with one hello. */
struct hs_out {
   int rc;           /* tls_handshake's answer */
   int wrote;        /* bytes it put on the wire */
   int server_hello; /* a plaintext ServerHello came back */
   int resumed;      /* ...and it carried pre_shared_key */
};

static void run_ch(const uint8_t *rec, size_t rn, struct hs_out *o)
{
   int sv[2];
   memset(o, 0, sizeof *o);
   o->rc = -1;
   if (rn == 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
      return;
   if (write(sv[1], rec, rn) != (ssize_t)rn) {
      close(sv[0]);
      close(sv[1]);
      return;
   }
   /* The write half is shut down so the read of the client's Finished gets
    * EOF instead of hanging: every case here is decided before that. */
   shutdown(sv[1], SHUT_WR);
   o->rc = tls_handshake(tls_conn_slot(), sv[0], NULL);
   uint8_t back[8192];
   ssize_t got = recv(sv[1], back, sizeof back, MSG_DONTWAIT);
   close(sv[0]);
   close(sv[1]);
   o->wrote = got > 0 ? (int)got : 0;
   /* The ServerHello is PLAINTEXT, which is the whole reason this can be
    * read without holding a key -- and why "did it resume" is visible. */
   if (got < 11 || back[0] != 22 || back[5] != 2)
      return;
   o->server_hello = 1;
   size_t n        = (size_t)got;
   size_t i        = 9 + 2 + 32; /* body, past legacy_version and random */
   if (i >= n)
      return;
   i += 1 + back[i]; /* legacy_session_id_echo */
   i += 2 + 1;       /* cipher_suite, legacy_compression_method */
   if (i + 2 > n)
      return;
   size_t end = i + 2 + (((size_t)back[i] << 8) | back[i + 1]);
   i += 2;
   while (i + 4 <= end && i + 4 <= n) {
      size_t type = ((size_t)back[i] << 8) | back[i + 1];
      size_t len  = ((size_t)back[i + 2] << 8) | back[i + 3];
      if (type == 41) /* pre_shared_key: the server selected an identity */
         o->resumed = 1;
      i += 4 + len;
   }
}

/* Run send_ticket on a throwaway plaintext connection and report what
 * arrived. *sent is tls.c's own answer; the return is the record's length,
 * 0 when nothing came, and -2 when something came that is not a
 * NewSessionTicket -- because "a status was returned" is not the assertion
 * item 134 asks for. THE BYTES ARE THE CLAIM. */
static int ticket_on_wire(int *sent)
{
   int sv[2];
   *sent = -1;
   if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
      return -1;
   *sent = tls_fault_send_ticket(sv[0]);
   uint8_t back[512];
   ssize_t got = recv(sv[1], back, sizeof back, MSG_DONTWAIT);
   close(sv[0]);
   close(sv[1]);
   if (got <= 0)
      return 0;
   if (got > 5 && back[0] == 22 &&
       back[5] == 4) /* handshake, NewSessionTicket */
      return (int)got;
   return -2;
}

/* A hello that breaks exactly one rule must be refused before anything
 * reaches the wire. */
static void ck_refused(const struct ch_flaw *f, const char *what)
{
   uint8_t rec[2048];
   size_t at;
   struct hs_out o;
   size_t rn = mk_ch(f, rec, sizeof rec, &at);
   run_ch(rec, rn, &o);
   ck(rn > 0 && o.rc == 0 && o.wrote == 0 && !o.server_hello, what);
}

int main(void)
{
   /* ---- AES-128-GCM, NIST gcmEncryptExtIV128 ---- */
   {
      /* Test case 3 of the original GCM specification: 16-byte key, 12-byte
       * IV, 64 bytes of plaintext, no AAD. */
      uint8_t key[16], iv[12], pt[64], ct[64], tag[16];
      hex("feffe9928665731c6d6a8f9467308308", key, 16);
      hex("cafebabefacedbaddecaf888", iv, 12);
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39"
          "1aafd255",
          pt, 64);
      ck(aes128_gcm_seal(key, iv, NULL, 0, pt, 64, ct, tag) == GCM_OK,
         "NIST case 3 seals");
      cmp(ct,
          "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
          "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
          "473f5985",
          64, "GCM ciphertext (NIST case 3)");
      cmp(tag, "4d5c2af327cd64a62cf35abd2ba6fab4", 16, "GCM tag (case 3)");

      /* And it must come back. */
      uint8_t back[64];
      ck(aes128_gcm_open(key, iv, NULL, 0, ct, 64, tag, back),
         "GCM open accepts a good tag");
      ck(memcmp(back, pt, 64) == 0, "GCM round trip returns the plaintext");

      /* A single flipped bit in the tag must be refused. */
      uint8_t bad[16];
      memcpy(bad, tag, 16);
      bad[0] ^= 1;
      ck(!aes128_gcm_open(key, iv, NULL, 0, ct, 64, bad, back),
         "GCM open REFUSES a forged tag");
   }

   /* ---- GCM with AAD, NIST case 4 (the shape TLS actually uses) ---- */
   {
      uint8_t key[16], iv[12], pt[64], aad[20], ct[64], tag[16];
      hex("feffe9928665731c6d6a8f9467308308", key, 16);
      hex("cafebabefacedbaddecaf888", iv, 12);
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
          pt, 60);
      hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
      ck(aes128_gcm_seal(key, iv, aad, 20, pt, 60, ct, tag) == GCM_OK,
         "NIST case 4 seals");
      cmp(tag, "5bc94fbc3221a5db94fae95ae7121a47", 16,
          "GCM tag covers the additional data");
   }

   /* ---- HKDF-SHA256, RFC 5869 test case 1 ---- */
   {
      uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
      hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, 22);
      hex("000102030405060708090a0b0c", salt, 13);
      hex("f0f1f2f3f4f5f6f7f8f9", info, 10);
      hkdf_extract(salt, 13, ikm, 22, prk);
      cmp(prk,
          "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
          32, "HKDF-Extract (RFC 5869 case 1)");
      ck(hkdf_expand(prk, info, 10, okm, 42) == HKDF_OK,
         "HKDF-Expand accepts RFC 5869 case 1");
      cmp(okm,
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865",
          42, "HKDF-Expand (RFC 5869 case 1)");
   }

   /* ---- HKDF-SHA256, RFC 5869 test case 3: EMPTY salt and EMPTY info ----
    *
    * Here as a published vector rather than a hand-written "zero length is
    * accepted" assertion, because it proves both halves at once: infon == 0
    * is a legal ask AND the answer is the RFC's. It is also the one call that
    * reaches `memcpy(in + k, info, 0)` with info == NULL, which is undefined
    * behaviour the sanitiser builds do flag, so the guard around that copy is
    * executed by a vector rather than by inspection. */
   {
      uint8_t ikm[22], prk[32], okm[42];
      hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, 22);
      hkdf_extract((const uint8_t *)"", 0, ikm, 22, prk);
      cmp(prk,
          "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
          32, "HKDF-Extract, zero-length salt (RFC 5869 case 3)");
      ck(hkdf_expand(prk, NULL, 0, okm, 42) == HKDF_OK,
         "HKDF-Expand accepts a zero-length info");
      cmp(okm,
          "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
          "9d201395faa4b61a96c8",
          42, "HKDF-Expand, zero-length info (RFC 5869 case 3)");
   }

   /* ---- THE HKDF BOUNDS: RFC 5869 2.3's L <= 255*HashLen, and info ----
    *
    * Each case below asserts the EXACT status, not merely "not OK". That is
    * what makes it an isolating test: hkdf_expand checks arguments, then
    * info, then length, so a case that fired on a later rule than the one it
    * names would report a different enumerator and fail here. This project
    * has been bitten by the opposite -- a case that looked like it pinned a
    * rule while actually being refused further down, proving nothing.
    *
    * The output buffer is filled with a sentinel first and checked after,
    * because "refused" and "refused after writing half a key" are the two
    * outcomes the whole change is about telling apart. */
   {
      uint8_t prk[32], info[HKDF_INFO_MAX + 1];
      static uint8_t big[HKDF_L_MAX];
      uint8_t small[64];
      hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
          prk, 32);
      for (size_t i = 0; i < sizeof info; i++)
         info[i] = (uint8_t)i;

      /* THE OUTPUT CEILING, exactly. 8160 bytes is 255 blocks, the last one
       * keyed with counter 255 -- one short of the wrap. The expected tail is
       * from an independent HMAC-SHA256 implementation, so this pins the
       * counter's final value and not just "it returned OK": a counter that
       * had already wrapped would still fill the buffer. */
      memset(big, 0x5A, sizeof big);
      ck(hkdf_expand(prk, NULL, 0, big, HKDF_L_MAX) == HKDF_OK,
         "HKDF-Expand accepts exactly 255*HashLen (RFC 5869 2.3)");
      cmp(big + HKDF_L_MAX - 32,
          "d04e8fae1746cb1e02b9c99dc0b28eb8cf20fcc51711744feb0631bc65c26161",
          32, "...and block 255 is keyed with counter 255, not 0");

      /* ONE PAST IT. The old code returned void and 8161 bytes, the last of
       * them from HMAC(PRK, T(255) || info || 0x00). Nothing is written now.
       * `small` is deliberately far too short for the ask: the point is that
       * the refusal happens before any write, so no buffer of that size is
       * needed to ask the question. */
      memset(small, 0x5A, sizeof small);
      ck(hkdf_expand(prk, NULL, 0, small, HKDF_L_MAX + 1) == HKDF_ERR_LEN,
         "HKDF-Expand REFUSES 255*HashLen + 1 with HKDF_ERR_LEN");
      ck(unwritten(small, sizeof small, 0x5A),
         "...and wrote nothing at all while refusing");

      /* ZERO OUTPUT. Not a derivation, and not a clamp to nothing either --
       * the old code's while(n) simply never ran and said so to nobody. */
      memset(small, 0x5A, sizeof small);
      ck(hkdf_expand(prk, NULL, 0, small, 0) == HKDF_ERR_LEN,
         "HKDF-Expand REFUSES an output length of 0");
      ck(unwritten(small, sizeof small, 0x5A), "...and writes nothing");

      /* THE INFO CAPACITY, exactly. 514 bytes is the widest HkdfLabel RFC
       * 8446 7.1 can encode, and it must fit: if this were refused the TLS
       * label encoder's bounds below could never all be reachable. n and the
       * arguments are valid, so HKDF_ERR_INFO is the only rule that can fire
       * on the next case -- that is the isolation. */
      memset(small, 0x5A, sizeof small);
      ck(hkdf_expand(prk, info, HKDF_INFO_MAX, small, 32) == HKDF_OK,
         "HKDF-Expand accepts exactly HKDF_INFO_MAX bytes of info");
      ck(!unwritten(small, 32, 0x5A),
         "...and did write the 32 bytes asked for");

      /* ONE PAST IT. This is the case that used to write one byte past a
       * 289-byte stack array with no diagnostic of any kind. */
      memset(small, 0x5A, sizeof small);
      ck(hkdf_expand(prk, info, HKDF_INFO_MAX + 1, small, 32) == HKDF_ERR_INFO,
         "HKDF-Expand REFUSES HKDF_INFO_MAX + 1 with HKDF_ERR_INFO");
      ck(unwritten(small, sizeof small, 0x5A),
         "...and does not overrun its own scratch to do it");

      /* A NULL WITH A LENGTH is a caller bug, not an empty input, and it is
       * reported as a different failure from either bound above. */
      memset(small, 0x5A, sizeof small);
      ck(hkdf_expand(prk, NULL, 8, small, 32) == HKDF_ERR_ARG,
         "HKDF-Expand REFUSES a NULL info with a nonzero length");
      ck(hkdf_expand(prk, info, 8, NULL, 32) == HKDF_ERR_ARG,
         "HKDF-Expand REFUSES a NULL output");
      ck(unwritten(small, sizeof small, 0x5A), "...writing nothing either way");
   }

   /* ---- HMAC-SHA256, RFC 4231 test case 2 (key shorter than a block) ---- */
   {
      uint8_t out[32];
      hmac_sha256((const uint8_t *)"Jefe", 4,
                  (const uint8_t *)"what do ya want for nothing?", 28, out);
      cmp(out,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          32, "HMAC-SHA256 (RFC 4231 case 2)");
   }

   /* ---- the TLS 1.3 key schedule, RFC 8448 ----
    *
    * The first step of the schedule: with no PSK, Early Secret is
    * Extract(0, 0). If this disagrees, every key after it is wrong. */
   {
      uint8_t zeros[32] = {0}, early[32];
      hkdf_extract(NULL, 0, zeros, 32, early);
      cmp(early,
          "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a",
          32, "Early Secret with no PSK (RFC 8448)");
   }

   /* HkdfLabel framing: the label really does get the "tls13 " prefix, and
    * the length goes in first. Checked against RFC 8448's derived secret. */
   {
      uint8_t early[32], derived[32], zeros[32] = {0};
      hkdf_extract(NULL, 0, zeros, 32, early);
      ck(derive_secret(early, "derived", (const uint8_t *)"", 0, derived) == 1,
         "Derive-Secret succeeds on the RFC 8448 inputs");
      cmp(derived,
          "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba",
          32, "Derive-Secret(early, \"derived\", \"\") (RFC 8448)");
   }

   /* ---- THE HkdfLabel FIELD WIDTHS (RFC 8446 7.1) ----
    *
    *     opaque label<7..255> = "tls13 " + Label     ->  Label is 1..249
    *     opaque context<0..255> = Context            ->  Context is 0..255
    *
    * hkdf_expand_label returns 1/0, so unlike the HKDF cases above it cannot
    * name which rule refused it. The isolation is therefore in the INPUTS:
    * every case below leaves all the other fields legal, so exactly one rule
    * can be the reason. That matters here more than usual -- an over-long
    * label ALSO makes the encoded info too long for hkdf_expand, so a case
    * that only checked "0 was returned" with an over-long everything would
    * pass whether the label bound existed or not.
    *
    * The success case is the load-bearing one: 249 + 255 encodes to exactly
    * HKDF_INFO_MAX bytes, so it proves the buffer really does reach the RFC's
    * widths and that neither bound is off by one in the safe direction. */
   {
      uint8_t secret[32], ctx[256], out[32];
      char label[251];
      memset(secret, 0xA5, sizeof secret);
      for (size_t i = 0; i < sizeof ctx; i++)
         ctx[i] = (uint8_t)i;
      memset(label, 'x', sizeof label - 1);
      label[sizeof label - 1] = 0; /* 250 characters */

      /* BOTH FIELDS AT THEIR MAXIMUM, together. */
      memset(out, 0x5A, sizeof out);
      label[249] = 0; /* 249 characters: the longest legal Label */
      ck(hkdf_expand_label(secret, label, ctx, 255, out, 32) == 1,
         "HkdfLabel accepts a 249-byte label with a 255-byte context");
      ck(!unwritten(out, sizeof out, 0x5A), "...and derives 32 bytes from it");

      /* ONE PAST THE LABEL WIDTH, with the context legal. The old encoder
       * wrote `(uint8_t)(6 + 250)` = 0, announcing a zero-length label, and
       * then ran 250 bytes into a 74-byte stack buffer. */
      memset(out, 0x5A, sizeof out);
      label[249] = 'x'; /* put the terminator back at 250, not at 249 */
      label[250] = 0;
      ck(strlen(label) == 250, "the 250-byte label really is 250 bytes");
      ck(hkdf_expand_label(secret, label, ctx, 32, out, 32) == 0,
         "HkdfLabel REFUSES a 250-byte label (label<7..255> minus \"tls13 \")");
      ck(unwritten(out, sizeof out, 0x5A), "...without writing the output");

      /* THE LABEL'S LOWER BOUND. label<7..255> carries at least 7 bytes and
       * "tls13 " is 6 of them, so an empty Label is not a legal vector.
       * Everything else is legal, so this is the only rule in play. */
      memset(out, 0x5A, sizeof out);
      ck(hkdf_expand_label(secret, "", ctx, 32, out, 32) == 0,
         "HkdfLabel REFUSES an empty label (the vector's minimum is 7)");
      ck(unwritten(out, sizeof out, 0x5A), "...without writing the output");

      /* THE CONTEXT WIDTH, exactly and one past, with a SHORT label so the
       * encoded info is nowhere near HKDF_INFO_MAX. Without that the "256 is
       * refused" case would be satisfied by the info bound instead and would
       * prove nothing about context<0..255>. */
      memset(out, 0x5A, sizeof out);
      ck(hkdf_expand_label(secret, "key", ctx, 255, out, 32) == 1,
         "HkdfLabel accepts a 255-byte context");
      memset(out, 0x5A, sizeof out);
      ck(hkdf_expand_label(secret, "key", ctx, 256, out, 32) == 0,
         "HkdfLabel REFUSES a 256-byte context (context<0..255>)");
      ck(unwritten(out, sizeof out, 0x5A), "...without writing the output");

      /* An empty context is the normal case for "key" and "iv", and it is
       * also the (NULL, 0) call, so it has to keep working. */
      memset(out, 0x5A, sizeof out);
      ck(hkdf_expand_label(secret, "iv", NULL, 0, out, 12) == 1,
         "HkdfLabel accepts a zero-length context");
      ck(!unwritten(out, 12, 0x5A), "...and derives the 12 bytes asked for");

      /* THE OUTPUT LENGTH is HKDF's rule, not the label encoder's, and it is
       * deliberately not checked twice -- see the static assertions in tls.c.
       * It must still be REPORTED through this function, which is what these
       * two check: the refusal reaches the caller and the buffer is intact. */
      memset(out, 0x5A, sizeof out);
      ck(hkdf_expand_label(secret, "key", NULL, 0, out, 0) == 0,
         "HkdfLabel reports HKDF's refusal of a zero output length");
      ck(hkdf_expand_label(secret, "key", NULL, 0, out, HKDF_L_MAX + 1) == 0,
         "HkdfLabel reports HKDF's refusal of an output above 255*HashLen");
      ck(unwritten(out, sizeof out, 0x5A), "...writing nothing either way");

      /* NULLs, so no field is guessed at. */
      ck(hkdf_expand_label(secret, "key", NULL, 8, out, 32) == 0,
         "HkdfLabel REFUSES a NULL context with a nonzero length");
      ck(hkdf_expand_label(secret, NULL, NULL, 0, out, 32) == 0,
         "HkdfLabel REFUSES a NULL label");

      /* And the happy path still agrees with RFC 8448 after all of that --
       * the same derived secret as above, reached through the encoder that
       * now bounds every field. */
      uint8_t early[32], derived[32], zeros[32] = {0};
      hkdf_extract(NULL, 0, zeros, 32, early);
      ck(derive_secret(early, "derived", (const uint8_t *)"", 0, derived) == 1,
         "Derive-Secret reports success");
      cmp(derived,
          "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba",
          32, "...and still matches RFC 8448 with the bounds in place");
   }

   /* ---- ECDSA P-256, NIST CAVP vector (P-256, SHA-256) ----
    *
    * Signing needs a per-signature secret k, so a signature is only
    * reproducible if k is fixed. This vector fixes it, which makes the whole
    * (r, s) pair a known answer rather than something only we can check. */
   {
      p256_init();
      /* The field reduction is a fast path specific to P-256's prime; make it
       * agree with the textbook long division before trusting any curve
       * result that rides on it. */
      ck(p256_selftest() == 0, "P-256 fast reduction matches long division");
      uint8_t d[32], k[32], h[32], r[32], s[32], qx[32], qy[32];
      hex("c477f9f65c22cce20657faa5b2d1d8122336f851a508a1ed04e479c34985bf96", d,
          32);
      hex("7a1a7e52797fc8caaa435d2a4dace39158504bf204fbe19f14dbb427faee50ae", k,
          32);
      hex("a41a41a12a799548211c410c65d8133afde34d28bdd542e4b680cf2899c8a8c4", h,
          32);
      hex("b7e08afdfe94bad3f1dc8c734798ba1c62b3a0ad1e9ea2a38201cd0889bc7a19",
          qx, 32);
      hex("3603f747959dbf7a4bb226e41928729063adc7ae43529e61b563bbc606cc5e09",
          qy, 32);
      ck(ecdsa_p256_sign(d, h, k, r, s), "ECDSA signing succeeds");
      cmp(r, "2b42f576d07f4165ff65d1f3b1500f81e44c316f1f0b3ef57325b69aca46104f",
          32, "ECDSA r (NIST P-256/SHA-256 vector)");
      cmp(s, "dc42c2122d6392cd3e3a993a89502a8198c1886fe69d262c4b329bdb6b63faf1",
          32, "ECDSA s (NIST P-256/SHA-256 vector)");
      ck(ecdsa_p256_verify(qx, qy, h, r, s),
         "...and our own verify accepts it");
      uint8_t bad[32];
      memcpy(bad, s, 32);
      bad[31] ^= 1;
      ck(!ecdsa_p256_verify(qx, qy, h, r, bad),
         "...and REFUSES a tampered signature");

      /* THE SAME VECTOR'S KEY GENERATION, which is the published known answer
       * for a scalar multiplication by a full-width secret: d*G must be the Q
       * the vector names. Signing already depends on this, but only via r, so a
       * wrong point could in principle still produce a matching r. */
      struct u256 dsc;
      struct jpoint dg;
      uint8_t gx[32], gy[32];
      p256_sc_from_be(&dsc, d);
      p256_mul_g(&dg, &dsc);
      ck(p256_to_xy(&dg, gx, gy), "d*G is an affine point");
      cmp(gx,
          "b7e08afdfe94bad3f1dc8c734798ba1c62b3a0ad1e9ea2a38201cd0889bc7a19",
          32, "d*G x equals the vector's public key");
      cmp(gy,
          "3603f747959dbf7a4bb226e41928729063adc7ae43529e61b563bbc606cc5e09",
          32, "d*G y equals the vector's public key");

      /* SMALL SCALARS, which is where the ladder spends its first 250-odd
       * steps DOUBLING THE POINT AT INFINITY.
       *
       * jdouble used to return early for infinity; it no longer does, because
       * that shortcut was what let the timing of a signature report the leading
       * zero count of its nonce (see lib/p256.h). The general formula gives
       * Z' = 2YZ = 0 from Z == 0, so it still answers infinity -- but with
       * meaningless X and Y instead of the canonical (1, 1, 0), and every
       * consumer has to keep treating Z == 0 alone as infinity.
       *
       * The CAVP vector above cannot catch a regression in that, because its
       * nonce is full width and its ladder leaves infinity on the first step.
       * These scalars stay in the prefix for hundreds of steps. Checked against
       * repeated addition rather than against a memorised constant, so the two
       * sides reach m*G by genuinely different routes: the ladder walks 256
       * doublings, the reference does m-1 additions and never sees infinity
       * after the first. */
      struct jpoint refp = p256_g;
      for (unsigned m = 2; m <= 8; m++) {
         struct jpoint t;
         p256_padd(&t, &refp, &p256_g);
         refp           = t;
         struct u256 ks = {
             {0, 0, 0, 0}
         };
         ks.v[0] = m;
         struct jpoint lad;
         p256_mul_g(&lad, &ks);
         char what[64];
         snprintf(what, sizeof what,
                  "%u*G by ladder equals %u*G by repeated addition", m, m);
         ck(p256_eq(&lad, &refp), what);
      }

      /* And the boundary of the prefix from the other side: the zero scalar
       * never leaves infinity at all, so the doubling formula is applied to
       * infinity 256 times and must still yield something p256_is_inf agrees
       * with and p256_to_xy refuses. */
      struct u256 kz = {
          {0, 0, 0, 0}
      };
      struct jpoint zg;
      uint8_t zx[32], zy[32];
      memset(zx, 0x5A, 32);
      memset(zy, 0x5A, 32);
      p256_mul_g(&zg, &kz);
      ck(p256_is_inf(&zg), "0*G is the point at infinity");
      ck(!p256_to_xy(&zg, zx, zy), "...which has no affine coordinates");
      ck(unwritten(zx, 32, 0) && unwritten(zy, 32, 0),
         "...and the refused output was zeroed, not left as stack");
   }

   /* ---- ECDSA SCALARS ARE CANONICAL OR THEY ARE REFUSED (item 139) ----
    *
    * d, k, r and s must each be a 32-byte big-endian value in [1, n-1]. They
    * used to be REDUCED instead, by the same p256_sc_from_be the message hash
    * goes through, and that is not input hygiene -- it is signature
    * malleability. r and r + n reduce to the same scalar, so a verifier that
    * reduces cannot tell them apart, and a signature the signer never
    * produced verifies as one it did.
    *
    * WHY THIS NEEDS ITS OWN VECTOR AND NOT THE CAVP ONE ABOVE. The alias
    * r + n only EXISTS as 32 bytes when r < 2^256 - n, which is just over
    * 2^223; the CAVP vector's r and s are both full width, so r + n wraps and
    * is a different scalar entirely. Finding a real (d, k) whose r lands in
    * that 2^-32 band would take 2^32 signatures.
    *
    * So the vector below was CONSTRUCTED, and it is worth saying exactly how,
    * because a hand-made signature is worthless if it is not genuinely valid.
    * Verification computes R = (z/s)G + (r/s)Q and checks x(R) mod n == r, so
    * for a chosen r and s: take R to be the curve point whose x is r (r was
    * searched upward from a fixed start until r^3 - 3r + b was a square mod
    * p), pick z, set u1 = z/s and u2 = r/s, and solve for the public key
    * Q = (R - u1*G) / u2. Every one of those is an ordinary field or group
    * operation, and the result is a signature that verifies under the
    * ordinary rule -- the CONTROL below is what proves that, and without it
    * the two refusals afterwards would prove nothing at all. Both r and s are
    * under 2^216, so both aliases exist.
    *
    * Measured against this tree before the check went in: all three of
    * (r, s), (r + n, s) and (r, s + n) verified. */
   {
      uint8_t qx[32], qy[32], h[32], r[32], s[32], rn[32], sn[32];
      hex("c14e0960ae38094507b5f80ac98e8297be7338e5ced897c3483892aca41d12bf",
          qx, 32);
      hex("3eb052da345514b3dce6d862c55fe6ec9db31854851bb91a2b75417ed2e169ca",
          qy, 32);
      hex("7f3ac2109de5b8461c0d93a7e2f5148b60cc9d3721ae4f8506b1d29c3e7a5041", h,
          32);
      hex("000000000000000000009f3b7c2ad145e908cc31be47a0d5f61c2ea8b93d7746", r,
          32);
      hex("00000000000000000000c31d8f4a72be5510e33a9d64f7182bc0a95e3d61847b", s,
          32);
      /* r + n and s + n, exactly: the arithmetic is done here rather than
       * written out, so the two constants cannot drift apart from the vector
       * they are derived from and a reader can see that nothing else changed
       * about the signature. */
      hex("ffffffff0000000100009f3b7c2ad145a5efc6df655f3f5ae9d5f96bb5a09c97",
          rn, 32);
      hex("ffffffff000000010000c31d8f4a72be11f7dde8447c959d1f7a742139c4a9cc",
          sn, 32);

      /* THE CONTROL, and it comes first deliberately: a canonicality check
       * that refused every input would pass every negative case below. */
      ck(ecdsa_p256_verify(qx, qy, h, r, s),
         "ECDSA control: the constructed small-r/small-s signature verifies");

      /* THE ITEM. Same key, same hash, same signature -- re-encoded. */
      ck(!ecdsa_p256_verify(qx, qy, h, rn, s),
         "ECDSA REFUSES r + n, the noncanonical alias of a valid r");
      ck(!ecdsa_p256_verify(qx, qy, h, r, sn),
         "ECDSA REFUSES s + n, the noncanonical alias of a valid s");
      ck(!ecdsa_p256_verify(qx, qy, h, rn, sn), "...and refuses both at once");

      /* THE ENDS OF THE INTERVAL, on the same vector so the control above
       * still applies to everything but the one value being changed.
       *
       * WHAT THESE CANNOT SHOW, said plainly: r == 0 and s == 0 are refused
       * by the curve arithmetic too -- a zero r makes u2 zero, a zero s makes
       * w zero and the combined point infinite -- so these four assertions
       * would have passed before the decoder existed. They are here as
       * boundary coverage for the DECODER, and the case that actually needs
       * it is the signing side below, where a zero scalar is not caught by
       * anything downstream. */
      uint8_t zero[32], nn[32];
      hex(SC_0, zero, 32);
      hex(SC_N, nn, 32);
      ck(!ecdsa_p256_verify(qx, qy, h, zero, s), "ECDSA REFUSES r == 0");
      ck(!ecdsa_p256_verify(qx, qy, h, r, zero), "ECDSA REFUSES s == 0");
      ck(!ecdsa_p256_verify(qx, qy, h, nn, s),
         "ECDSA REFUSES r == n (which used to be reduced to zero)");
      ck(!ecdsa_p256_verify(qx, qy, h, r, nn), "ECDSA REFUSES s == n");
   }

   /* ---- THE SAME RULE ON THE SIGNING SIDE: d AND k (item 139) ----
    *
    * This is where the reduction was worst. d == n and k == n both folded to
    * zero one layer below the refusal, so a caller that supplied the group
    * order was told its key was zero; and a d of zero, had the zero test ever
    * been removed, signs perfectly well under the public key at infinity. */
   {
      uint8_t d[32], k[32], h[32], r[32], s[32], qx[32], qy[32];
      hex("c477f9f65c22cce20657faa5b2d1d8122336f851a508a1ed04e479c34985bf96", d,
          32);
      hex("7a1a7e52797fc8caaa435d2a4dace39158504bf204fbe19f14dbb427faee50ae", k,
          32);
      hex("a41a41a12a799548211c410c65d8133afde34d28bdd542e4b680cf2899c8a8c4", h,
          32);
      hex("b7e08afdfe94bad3f1dc8c734798ba1c62b3a0ad1e9ea2a38201cd0889bc7a19",
          qx, 32);
      hex("3603f747959dbf7a4bb226e41928729063adc7ae43529e61b563bbc606cc5e09",
          qy, 32);

      /* THE CONTROL. Signing still works and still produces something our own
       * verifier accepts -- the assertion that says the decoder did not
       * simply start refusing everything. */
      memset(r, 0x5A, 32);
      memset(s, 0x5A, 32);
      ck(ecdsa_p256_sign(d, h, k, r, s) == 1,
         "ECDSA control: a canonical d and k still sign");
      ck(ecdsa_p256_verify(qx, qy, h, r, s),
         "...and the signature they produce still verifies");

      uint8_t bad[32];
      hex(SC_0, bad, 32);
      ck(ecdsa_p256_sign(bad, h, k, r, s) == 0,
         "ECDSA signing REFUSES a private key of 0");
      hex(SC_N, bad, 32);
      ck(ecdsa_p256_sign(bad, h, k, r, s) == 0,
         "ECDSA signing REFUSES a private key of exactly n");
      hex(SC_0, bad, 32);
      ck(ecdsa_p256_sign(d, h, bad, r, s) == 0,
         "ECDSA signing REFUSES a nonce of 0");
      hex(SC_N, bad, 32);
      ck(ecdsa_p256_sign(d, h, bad, r, s) == 0,
         "ECDSA signing REFUSES a nonce of exactly n");

      /* n+1, WHICH IS THE ALIAS OF A PERFECTLY GOOD KEY, and the case that
       * separates "checked" from "reduced, then tested for zero". Reduction
       * maps n+1 onto 1, so the old code did not refuse this at all: it
       * signed, successfully and silently, UNDER THE PRIVATE KEY 1 -- a key
       * the caller never chose and does not hold. Zero-testing after the
       * reduction (the shape this defect is most likely to be re-introduced
       * as) catches n and misses this. */
      hex(SC_NP1, bad, 32);
      ck(ecdsa_p256_sign(bad, h, k, r, s) == 0,
         "ECDSA signing REFUSES a private key of n+1 rather than signing "
         "under the key 1");
      ck(ecdsa_p256_sign(d, h, bad, r, s) == 0,
         "ECDSA signing REFUSES a nonce of n+1 rather than using the nonce 1");

      /* THE CONTROL FOR BOTH: 1 itself is a legal scalar, so what the two
       * cases above refused is the ENCODING and not the value. Without this
       * they would also be satisfied by an implementation that had simply
       * stopped accepting small scalars. */
      uint8_t one[32];
      hex(SC_1, one, 32);
      ck(ecdsa_p256_sign(one, h, k, r, s) == 1,
         "ECDSA control: a private key of 1 IS accepted");
      ck(ecdsa_p256_sign(d, h, one, r, s) == 1,
         "ECDSA control: and so is a nonce of 1");

      /* THE TOP OF THE INTERVAL IS INSIDE IT. n-1 is the largest legal
       * private key there is, and a check written as `>=` where `>` was meant
       * would throw it away while passing every case above. Its public key is
       * computed here rather than quoted, so the control is self-contained:
       * (n-1)*G = -G, which is G with its y negated. */
      uint8_t nm1[32], gx[32], gy[32];
      hex(SC_NM1, nm1, 32);
      struct u256 nm1s;
      struct jpoint nm1g;
      p256_sc_from_be(&nm1s, nm1);
      p256_mul_g(&nm1g, &nm1s);
      ck(p256_to_xy(&nm1g, gx, gy), "(n-1)*G is an affine point");
      ck(ecdsa_p256_sign(nm1, h, k, r, s) == 1,
         "ECDSA control: a private key of n-1 IS accepted");
      ck(ecdsa_p256_verify(gx, gy, h, r, s),
         "...and its signature verifies under (n-1)*G");
      ck(ecdsa_p256_sign(d, h, nm1, r, s) == 1,
         "ECDSA control: a nonce of n-1 IS accepted");
      ck(ecdsa_p256_verify(qx, qy, h, r, s),
         "...and that signature verifies too");
   }

   /* ---- streaming SHA-256 must equal the one-shot, fed any which way ---- */
   {
      const char *msg =
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
      uint8_t one[32], many[32];
      sha256((const uint8_t *)msg, strlen(msg), one);
      cmp(one,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          32, "SHA-256 one-shot (FIPS 180-2 vector)");
      struct sha256_ctx c;
      sha256_init(&c);
      for (const char *q = msg; *q;
           q++) /* a byte at a time is the worst case */
         sha256_update(&c, (const uint8_t *)q, 1);
      sha256_final(&c, many);
      ck(memcmp(one, many, 32) == 0,
         "streaming SHA-256 agrees with the one-shot");

      /* and across a block boundary, where the padding logic actually bites */
      uint8_t big[200], a1[32], a2[32];
      for (int i = 0; i < 200; i++)
         big[i] = (uint8_t)i;
      sha256(big, 200, a1);
      sha256_init(&c);
      sha256_update(&c, big, 61);
      sha256_update(&c, big + 61, 139);
      sha256_final(&c, a2);
      ck(memcmp(a1, a2, 32) == 0, "...including across block boundaries");
   }

   /* ---- PBKDF2-HMAC-SHA256, RFC 7914 vector ----
    *
    * This is also the ITERATION-COUNT BOUNDARY from below: c = 1 is the
    * smallest count RFC 8018 defines, it must be accepted, and the answer is
    * published. The refusal of c = 0 further down is only meaningful next to
    * this -- the old code computed the two identically. */
   {
      uint8_t out[40];
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                       1, out, 40) == PBKDF2_OK,
         "PBKDF2 accepts the smallest legal iteration count");
      cmp(out,
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
          40, "PBKDF2-HMAC-SHA256, 1 iteration (RFC 7914)");
   }

   /* ---- THE PBKDF2 PARAMETER BOUNDS ----
    *
    * Same rule as the HKDF cases: assert the EXACT status. pbkdf2_sha256
    * checks arguments, then salt, then iterations, then dkLen, so each case
    * leaves every rule but its own satisfied and would report a different
    * enumerator if it were being caught by a later check.
    *
    * That is not hypothetical here. "A 253-byte salt is refused" would be
    * true even without a salt bound if the case also passed iters == 0 --
    * which is exactly the trap of proving a rule with an input that fails
    * another one. Every case below is minimal in that sense. */
   {
      uint8_t out[32];
      uint8_t s253[253];
      for (size_t i = 0; i < sizeof s253; i++)
         s253[i] = (uint8_t)(i * 7 + 3);

      /* THE SALT CAPACITY, exactly: 252 bytes must work, and the answer comes
       * from an independent implementation (Python's hashlib.pbkdf2_hmac) so
       * this is a known answer and not a self-check. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, s253, PBKDF2_SALT_MAX, 2,
                       out, 32) == PBKDF2_OK,
         "PBKDF2 accepts a salt of exactly PBKDF2_SALT_MAX");
      cmp(out,
          "7d98ee64480965579c4b195842007d419701119a4c7c606fc5085b0f77b421d7",
          32, "...and derives what an independent PBKDF2 does");

      /* ONE PAST IT, and this is the isolating case for the truncation bug:
       * s253's first 252 bytes ARE the salt just used, so the old code cut it
       * down and returned the very bytes checked immediately above -- a
       * different salt producing an identical key, in silence. It must now be
       * refused with the buffer untouched, which is also why the sentinel
       * check is not redundant: "unchanged" and "the 252-byte answer" are the
       * two outcomes being told apart. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, s253, sizeof s253, 2, out,
                       32) == PBKDF2_ERR_SALT,
         "PBKDF2 REFUSES a salt one byte over capacity, not truncates it");
      ck(unwritten(out, sizeof out, 0x5A),
         "...and does not leave the truncated salt's key behind");

      /* ZERO ITERATIONS. RFC 8018 5.2 makes c a positive integer; the old
       * `for (i = 1; i < iters; i++)` made c = 0 into c = 1, so a row with a
       * zero cost parameter verified successfully against a hash with no work
       * factor at all. Salt and dkLen are legal here, so nothing else can be
       * the reason for the refusal. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                       0, out, 32) == PBKDF2_ERR_ITERS,
         "PBKDF2 REFUSES zero iterations rather than computing one");
      ck(unwritten(out, sizeof out, 0x5A),
         "...and does not leave the 1-iteration answer behind");

      /* ZERO dkLen. RFC 8018's dkLen is a positive integer too; the old loop
       * simply did not run and returned void. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                       1, out, 0) == PBKDF2_ERR_DKLEN,
         "PBKDF2 REFUSES a zero-length derived key");
      ck(unwritten(out, sizeof out, 0x5A), "...writing nothing");

      /* THE dkLen CEILING, one past. RFC 8018 5.2 step 1 stops at
       * (2^32 - 1) * hLen because the block index is a 32-bit INT(i); past it
       * `block` wraps to 0 and the output starts repeating. Asked with a
       * 32-byte buffer ON PURPOSE: the refusal is decided before any write,
       * so the question does not need 128 GiB to ask. The limit itself cannot
       * be tested from the other side for the same reason it cannot be
       * reached -- see the note in the report. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                       1, out,
                       (size_t)PBKDF2_BLOCK_MAX * PBKDF2_HASH_LEN + 1) ==
             PBKDF2_ERR_DKLEN,
         "PBKDF2 REFUSES a dkLen above (2^32 - 1) * hLen (RFC 8018 5.2)");
      ck(unwritten(out, sizeof out, 0x5A), "...before touching the output");

      /* An EMPTY salt is legal (and is the (NULL, 0) call), an empty password
       * is legal, and neither may be confused with the NULL cases below. */
      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256((const uint8_t *)"", 0, NULL, 0, 1, out, 32) ==
             PBKDF2_OK,
         "PBKDF2 accepts an empty password and an empty salt");
      ck(!unwritten(out, sizeof out, 0x5A), "...and derives 32 bytes");

      memset(out, 0x5A, sizeof out);
      ck(pbkdf2_sha256(NULL, 6, (const uint8_t *)"salt", 4, 1, out, 32) ==
             PBKDF2_ERR_ARG,
         "PBKDF2 REFUSES a NULL password with a nonzero length");
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, NULL, 4, 1, out, 32) ==
             PBKDF2_ERR_ARG,
         "PBKDF2 REFUSES a NULL salt with a nonzero length");
      ck(pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4,
                       1, NULL, 32) == PBKDF2_ERR_ARG,
         "PBKDF2 REFUSES a NULL output");
      ck(unwritten(out, sizeof out, 0x5A), "...writing nothing in any of them");
   }

   /* ---- THE ENTROPY BOUNDARY, and what happens when it fails ----
    *
    * These cases could not be written before. rand.c opened /dev/urandom
    * itself, so "what does this do when the entropy source fails?" needed a
    * machine whose /dev/urandom was broken -- and the failure paths that
    * guard an ECDSA nonce and a J-PAKE scalar had therefore never once been
    * executed. A provider that can be swapped is what makes the question
    * askable at all; that is most of why the boundary is worth having. */
   {
      uint8_t b[32];

      /* The real provider fills what it is asked for. */
      memset(b, 0, sizeof b);
      ck(rand_bytes(b, sizeof b) == 1, "the platform provider fills a buffer");

      /* A REFUSING SOURCE MUST NOT LOOK LIKE SUCCESS. */
      rand_source_fn prev = rand_set_source(src_fail);
      ck(rand_bytes(b, sizeof b) == 0, "a source that refuses reports failure");

      /* A SOURCE THAT FILLS ONLY PART OF THE BUFFER AND CLAIMS SUCCESS is
       * the shape a short read has. The provider contract says every byte or
       * none, and a caller must never key with the remainder -- which here
       * is whatever the stack held. */
      (void)rand_set_source(src_short);
      ck(rand_bytes(b, sizeof b) == 0,
         "a source that reports failure after a SHORT fill is not success");

      /* Restoring is by the handle the setter returned, so a test cannot
       * leave the process without entropy for whatever runs after it. */
      (void)rand_set_source(prev);
      ck(rand_bytes(b, sizeof b) == 1, "the platform provider is restored");

      /* Degenerate asks, so no provider has to answer them. */
      ck(rand_bytes(NULL, 8) == 0, "a NULL buffer is refused, not written to");
      ck(rand_bytes(b, 0) == 1, "zero bytes is nothing to fail at");
   }

   /* ---- J-PAKE: A PEER ROUND IS NOT ACCEPTED UNTIL ITS PROOF PASSES ----
    *
    * lib/jpake.c's have_r1/have_r2/have_r3 are the exchange's record that a
    * peer's zero-knowledge proof was checked and held; jpake_round3 and
    * jpake_shared_key gate on them. They used to be raised BEFORE the proof was
    * checked, on all three rounds, with the 160 wire bytes decoded straight
    * into the persistent round. A rejected packet therefore left the object
    * looking exactly like one holding an accepted round whose two ephemeral
    * public keys and proof scalar the peer had chosen freely.
    *
    * THE RETURN CODE WAS ALWAYS RIGHT. That is why every case below asserts the
    * STATE -- jpake_accepted() and jpake_poisoned(), which exist because this
    * was previously invisible from outside the object. A test that only read
    * return codes passed against the defective code.
    *
    * SEQUENCING MATTERS IN THESE ASSERTIONS and it is not a style point: the
    * first draft wrote ck(f(p) == 0 && jpake_accepted(p, 1) == 0, ...) and the
    * arguments were evaluated right to left, so the state was read BEFORE the
    * call that was supposed to change it and every case "passed". The mutating
    * call is on its own line here, always. */
   {
      /* Idempotent, and required before jpake_new: without the curve context
       * every round comes back wrong with nothing to explain why. The ECDSA
       * section above has already called p256_init(), which is what this does
       * -- saying it here anyway, because a section that depends on an earlier
       * one having run is a section that breaks when the earlier one moves. */
      ck(jpake_init() == 1, "J-PAKE curve init reports success");
      rand_source_fn prev = rand_set_source(src_det);
      struct jp_pair j;
      int rc;

      /* (1) THE HAPPY PATH, AS A KNOWN-ANSWER VECTOR. Fixed entropy, so this
       * is not "the two sides agree" -- it is "the exchange produces these
       * exact bytes", and the bytes come from the code before the fix. */
      ck(jp_open(&j, 0x5EED1234ULL),
         "J-PAKE opens a pair and emits rounds 1 and 2");
      cmp(j.c1,
          "eeadbff32523c74825adc429839deb0834b2127f35cb9c82cf9ec352dbb459b2",
          32,
          "J-PAKE client round 1 carries the expected ephemeral public key");
      cmp(j.s1,
          "7aa9a3942503d2568c5f70ebc2708272d32985406f7eae9dbaa85ea9e65d8995",
          32,
          "J-PAKE server round 1 carries the expected ephemeral public key");
      ck(jp_rounds12(&j), "J-PAKE rounds 1 and 2 verify in both directions");
      ck(jpake_accepted(j.cli, 1) && jpake_accepted(j.cli, 2),
         "...and the client records both as ACCEPTED, not merely arrived");
      ck(jpake_accepted(j.sen, 1) && jpake_accepted(j.sen, 2),
         "...and so does the server");
      ck(jpake_round3(j.cli, j.c3) == 160 && jpake_round3(j.sen, j.s3) == 160,
         "J-PAKE round 3 is emitted by both sides");
      ck(jpake_peer_round3(j.cli, j.s3) && jpake_peer_round3(j.sen, j.c3),
         "J-PAKE round 3 proofs verify in both directions");
      ck(jpake_accepted(j.cli, 3) && jpake_accepted(j.sen, 3),
         "...and round 3 is recorded as accepted on both sides");
      ck(!jpake_poisoned(j.cli) && !jpake_poisoned(j.sen),
         "...and neither exchange is poisoned after a clean run");
      {
         uint8_t kc[16], ks[16];
         memset(kc, 0x5A, sizeof kc);
         memset(ks, 0x5A, sizeof ks);
         ck(jpake_shared_key(j.cli, kc) == 1 &&
                jpake_shared_key(j.sen, ks) == 1,
            "J-PAKE derives a key on both sides");
         ck(memcmp(kc, ks, 16) == 0, "...and the two keys agree");
         cmp(kc, "fffe1a5492d3c4b45b8f4fe75f345f3c", 16,
             "...and it is the key this exchange produced before the fix");
      }
      jp_close(&j);

      /* (2) A ROUND-1 PACKET WHOSE PROOF IS INVALID MUST NOT BE ACCEPTED.
       * This is the item's own assertion. `accepted1 == 0` is the line that
       * failed against the old code; rc == 0 did not. */
      ck(jp_open(&j, 0x5EED1234ULL), "J-PAKE pair opens for the round-1 case");
      {
         uint8_t bad[160];
         jp_spoil_proof(bad, j.s1);
         rc = jpake_peer_round1(j.cli, bad);
         ck(rc == 0, "J-PAKE REFUSES a round-1 packet with a broken proof");
         ck(jpake_accepted(j.cli, 1) == 0,
            "...and does NOT record that round as accepted");
         ck(jpake_poisoned(j.cli) == 1, "...and the exchange is now poisoned");

         /* (3) TERMINAL: the GENUINE packet, offered next, on the same object.
          * The chosen design refuses it -- there is no retry in place -- and
          * this is the case that pins that choice rather than inferring it. */
         rc = jpake_peer_round1(j.cli, j.s1);
         ck(rc == 0, "J-PAKE REFUSES a legitimate round 1 after a rejected one "
                     "(failure is terminal)");
         ck(jpake_accepted(j.cli, 1) == 0,
            "...and the legitimate round is not accepted either");

         /* ...and the object cannot be used for anything else. A caller that
          * ignores every return code above still gets nothing out of it, which
          * is what "unusable in a way a caller cannot miss" has to mean. */
         uint8_t out[160], k[16];
         memset(k, 0x5A, sizeof k);
         ck(jpake_round1(j.cli, out) == 0,
            "...a poisoned exchange will not emit round 1");
         ck(jpake_round2(j.cli, out) == 0, "...nor round 2");
         ck(jpake_round3(j.cli, out) == 0, "...nor round 3");
         ck(jpake_peer_round2(j.cli, j.s2) == 0,
            "...nor accept a peer round 2");
         ck(jpake_shared_key(j.cli, k) == 0, "...nor derive a key");
         ck(unwritten(k, sizeof k, 0x5A),
            "...leaving the key buffer untouched");
      }
      jp_close(&j);

      /* (4) THE SAME RULE ON ROUND 2, WITH ROUND 1 ALREADY ACCEPTED. Two
       * things at once, and the second is the one only this case can show: the
       * failure CLEARS the round that had legitimately been accepted before it.
       * Under the old code have_r1 stayed 1 and have_r2 became 1 as well. */
      ck(jp_open(&j, 0x1234ABCDULL), "J-PAKE pair opens for the round-2 case");
      {
         uint8_t bad[160];
         rc = jpake_peer_round1(j.sen, j.c1);
         ck(rc == 1, "J-PAKE accepts a genuine round 1 (the control)");
         ck(jpake_accepted(j.sen, 1) == 1, "...and records it as accepted");
         jp_spoil_proof(bad, j.c2);
         rc = jpake_peer_round2(j.sen, bad);
         ck(rc == 0, "J-PAKE REFUSES a round-2 packet with a broken proof");
         ck(jpake_accepted(j.sen, 2) == 0, "...and round 2 is not accepted");
         ck(jpake_accepted(j.sen, 1) == 0,
            "...and the ALREADY-ACCEPTED round 1 is explicitly cleared");
      }
      jp_close(&j);

      /* (5) THE ISOLATING CASE FOR THE WHOLE ITEM. Rounds 1 and 2 are
       * accepted, so a rejected round 3 is the ONLY thing standing between
       * this object and a derived key -- nothing later in the file can be the
       * reason the key is refused. Against the old code have_r3 was set, r3
       * held the attacker's point, and jpake_shared_key returned 1 and wrote
       * sixteen bytes that the attacker had a term in. */
      ck(jp_open(&j, 0x0BADF00DULL), "J-PAKE pair opens for the round-3 case");
      {
         uint8_t bad[160], k[16], out[160];
         ck(jp_rounds12(&j), "J-PAKE rounds 1 and 2 accepted both ways");
         ck(jpake_round3(j.cli, j.c3) == 160 &&
                jpake_round3(j.sen, j.s3) == 160,
            "...and both round 3s are emitted");
         jp_spoil_proof(bad, j.c3);
         rc = jpake_peer_round3(j.sen, bad);
         ck(rc == 0, "J-PAKE REFUSES a round-3 packet with a broken proof");
         ck(jpake_accepted(j.sen, 3) == 0, "...and round 3 is not accepted");
         memset(k, 0x5A, sizeof k);
         rc = jpake_shared_key(j.sen, k);
         ck(rc == 0, "...so no key can be derived from the rejected round 3");
         ck(unwritten(k, sizeof k, 0x5A),
            "...and no attacker-influenced bytes were written to the key");
         ck(jpake_round3(j.sen, out) == 0,
            "...and the exchange will not emit a round 3 either");
         ck(jpake_accepted(j.sen, 1) == 0 && jpake_accepted(j.sen, 2) == 0,
            "...and rounds 1 and 2 were cleared with it");
      }
      /* THE CONTROL, on the same pair the case above spoiled a packet from.
       * The client half was never touched, so it still completes -- which is
       * what says the fixture was capable of completing and the refusal above
       * was the rule and not a broken setup. */
      {
         uint8_t k[16];
         memset(k, 0x5A, sizeof k);
         rc = jpake_peer_round3(j.cli, j.s3);
         ck(rc == 1, "J-PAKE control: the genuine round 3 IS accepted");
         ck(jpake_accepted(j.cli, 3) == 1, "...and recorded as accepted");
         rc = jpake_shared_key(j.cli, k);
         ck(rc == 1, "...and the control does derive a key");
         ck(!unwritten(k, sizeof k, 0x5A),
            "...and did write all sixteen bytes");
      }
      jp_close(&j);

      /* (6) A PACKET THAT CANNOT EVEN DECODE MUST NOT REPLACE AN ACCEPTED
       * ROUND. cert_from_bytes writes pubkey1 before it can find out pubkey2
       * is off the curve, so the old code answered 0 while having overwritten
       * half of a verified round with peer bytes AND left its flag raised.
       * This is why the fix decodes into a whole PCert temporary rather than
       * merely reordering the flag.
       *
       * ON ROUND 2, not round 1, since the phase machine went in: a second
       * round-1 packet is now refused as a DUPLICATE before it is decoded, so
       * asking this question on round 1 would no longer reach the decoder at
       * all and would silently stop testing it. Round 2 is the same shape --
       * an accepted round 1 already in the object, an undecodable packet in
       * the phase that is genuinely open -- and it does reach it. */
      ck(jp_open(&j, 0x00C0FFEEULL), "J-PAKE pair opens for the decode case");
      {
         uint8_t bad[160];
         rc = jpake_peer_round1(j.sen, j.c1);
         ck(rc == 1, "J-PAKE accepts a genuine round 1 (the control)");
         ck(jpake_accepted(j.sen, 1) == 1, "...and records it as accepted");
         jp_spoil_point(bad, j.c2);
         rc = jpake_peer_round2(j.sen, bad);
         ck(rc == 0, "J-PAKE REFUSES a round 2 whose pubkey2 is off the curve");
         ck(jpake_accepted(j.sen, 1) == 0,
            "...and the verified round it would have half-overwritten is gone");
         ck(jpake_poisoned(j.sen) == 1, "...and the exchange is poisoned");
      }
      jp_close(&j);

      /* (7) A DUPLICATE ROUND 1 IS REFUSED, AND THE FIRST ONE IS STILL IN
       * FORCE (item 141).
       *
       * This is the item's own case. A second round-1 packet used to
       * OVERWRITE the first: publishing was gated on the proof, but a
       * different packet with a VALID proof is something anyone can build --
       * the Schnorr proof shows knowledge of a scalar the sender chose, not
       * of the password -- so a peer could replace peer round 1's pubkey1 at
       * any point before derivation. That value is a base point of round 3
       * and a term in the derived key.
       *
       * THE SECOND PACKET IS GENUINE, from an unrelated exchange, which is
       * what makes this isolating: it decodes, its proof holds, and the only
       * rule that can refuse it is the phase. A malformed one would have been
       * refused by the decoder and would say nothing about duplicates.
       *
       * AND THE ASSERTION IS NOT MERELY "IT RETURNED 0". Overwriting and then
       * reporting an error is the same defect in a new place, so the case
       * carries the exchange all the way to a KEY and demands the exact 16
       * bytes the clean run in (1) produced from this seed. If the duplicate
       * had been published, sen's round-3 base would no longer match the one
       * cli built its proof over and this would not derive at all. */
      ck(jp_open(&j, 0x5EED1234ULL),
         "J-PAKE pair opens for the duplicate case (the seed from case 1)");
      {
         struct jp_pair other;
         ck(jp_open(&other, 0x0A0B0C0DULL),
            "...and an unrelated pair supplies a second GENUINE round 1");
         ck(memcmp(j.c1, other.c1, 160) != 0,
            "...which really is a different packet");

         /* The client half runs normally; the duplicate is aimed at the
          * server half, whose peer is the client. */
         ck(jpake_peer_round1(j.cli, j.s1) && jpake_peer_round2(j.cli, j.s2),
            "J-PAKE client accepts the server's rounds 1 and 2");

         rc = jpake_peer_round1(j.sen, j.c1);
         ck(rc == 1, "J-PAKE accepts the first round 1 (the control)");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R2,
            "...and the transcript advances to expect round 2");

         rc = jpake_peer_round1(j.sen, other.c1);
         ck(rc == 0, "J-PAKE REFUSES a SECOND, valid round-1 packet");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R2,
            "...leaving the transcript exactly where it was");
         ck(jpake_accepted(j.sen, 1) == 1,
            "...with the ORIGINAL round 1 still accepted");
         ck(jpake_poisoned(j.sen) == 0,
            "...and the exchange NOT poisoned, because nothing was parsed");

         /* And it finishes, on the original round 1. */
         ck(jpake_peer_round2(j.sen, j.c2) == 1,
            "...the exchange still accepts round 2 afterwards");
         ck(jpake_round3(j.cli, j.c3) == 160 &&
                jpake_round3(j.sen, j.s3) == 160,
            "...and both sides still emit round 3");
         ck(jpake_peer_round3(j.sen, j.c3) == 1,
            "...and the client's round-3 proof still verifies against it");
         {
            uint8_t ks[16];
            memset(ks, 0x5A, sizeof ks);
            ck(jpake_shared_key(j.sen, ks) == 1, "...and a key is derived");
            cmp(ks, "fffe1a5492d3c4b45b8f4fe75f345f3c", 16,
                "...and it is BYTE FOR BYTE the key the clean run produced: "
                "the duplicate changed nothing");
         }
         jp_close(&other);
      }
      jp_close(&j);

      /* (8) OUT OF ORDER IS REFUSED BEFORE THE PACKET IS PARSED (item 141).
       *
       * Round 3 used to be accepted whenever round 1 was, so it could arrive
       * before round 2; and round 3 before round 1 was refused only AFTER the
       * bytes had been through cert_from_bytes.
       *
       * THE PACKET USED HERE CANNOT DECODE, and that is the whole trick. If
       * the phase test ran after the decode, the decode would fail and the
       * exchange would be POISONED -- so `poisoned == 0` afterwards is a
       * direct observation that the 160 bytes were never looked at. Nothing
       * else in this file can see the ordering of two checks inside one
       * function. */
      ck(jp_open(&j, 0x13571357ULL), "J-PAKE pair opens for the ordering case");
      {
         uint8_t bad[160];
         jp_spoil_point(bad, j.c3); /* c3 is empty here; the points are junk */
         memset(bad, 0xFF, 64); /* both points off the curve, beyond doubt */

         rc = jpake_peer_round3(j.sen, bad);
         ck(rc == 0, "J-PAKE REFUSES a round 3 before round 1 was accepted");
         ck(jpake_poisoned(j.sen) == 0,
            "...WITHOUT parsing it, which is why the exchange is not poisoned");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R1,
            "...and the transcript is still empty");

         rc = jpake_peer_round2(j.sen, j.c2);
         ck(rc == 0, "J-PAKE REFUSES a round 2 before round 1 as well");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R1, "...still empty");

         rc = jpake_peer_round1(j.sen, j.c1);
         ck(rc == 1, "J-PAKE control: round 1 is accepted when it is its turn");
         rc = jpake_peer_round3(j.sen, bad);
         ck(rc == 0, "J-PAKE REFUSES a round 3 that skips round 2");
         ck(jpake_poisoned(j.sen) == 0, "...again without parsing it");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R2,
            "...and round 1 is still the only accepted round");
      }
      jp_close(&j);

      /* (9) A KEY NEEDS EXACTLY ONE ACCEPTED PACKET OF EVERY ROUND
       * (item 141). The old gate read two of the three flags, so it could not
       * distinguish "round 3 never arrived" from "the round-1 packet arrived
       * twice and round 3 never did". */
      ck(jp_open(&j, 0x24682468ULL),
         "J-PAKE pair opens for the derivation-gate case");
      {
         uint8_t ks[16];
         struct jp_pair other;
         ck(jp_open(&other, 0x99887766ULL),
            "...with an unrelated pair for the duplicate packet");
         ck(jpake_peer_round1(j.cli, j.s1) && jpake_peer_round2(j.cli, j.s2),
            "J-PAKE client accepts the server's rounds 1 and 2");

         ck(jpake_peer_round1(j.sen, j.c1) == 1, "server accepts round 1");
         ck(jpake_peer_round1(j.sen, other.c1) == 0,
            "...refuses a second round 1");
         ck(jpake_peer_round2(j.sen, j.c2) == 1, "...and accepts round 2");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_R3,
            "...so the transcript counts TWO rounds, not three: the refused "
            "duplicate did not stand in for the missing one");

         memset(ks, 0x5A, sizeof ks);
         rc = jpake_shared_key(j.sen, ks);
         ck(rc == 0, "J-PAKE REFUSES to derive with round 3 missing");
         ck(unwritten(ks, sizeof ks, 0x5A), "...writing nothing");
         ck(jpake_poisoned(j.sen) == 0,
            "...and without destroying the exchange, since nothing was wrong "
            "with it -- the caller simply asked early");

         /* THE CONTROL: the same object, one packet later, does derive. */
         ck(jpake_round3(j.cli, j.c3) == 160 &&
                jpake_round3(j.sen, j.s3) == 160,
            "...both sides emit round 3");
         ck(jpake_peer_round3(j.sen, j.c3) == 1, "...round 3 is accepted");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_DONE,
            "...completing the transcript");
         ck(jpake_shared_key(j.sen, ks) == 1,
            "J-PAKE control: and NOW it derives a key");
         ck(!unwritten(ks, sizeof ks, 0x5A), "...having written all 16 bytes");

         /* A SECOND round 3 is a duplicate like any other, and the key is
          * already derived, so this is the case that says the transcript
          * cannot be extended past DONE. */
         ck(jpake_peer_round3(j.sen, j.c3) == 0,
            "...and a repeat of round 3 is refused too");
         ck(jpake_phase(j.sen) == JPAKE_PHASE_DONE, "...leaving it complete");
         jp_close(&other);
      }
      jp_close(&j);

      /* (10) NULL READS AS A POISONED EXCHANGE. jpake_new returns NULL when
       * entropy fails, and app/dexdriver.c and srv/pair.c each have a check
       * for that -- but a check at four call sites is a convention, and this
       * makes it the object's answer. */
      {
         uint8_t out[160], k[16];
         memset(k, 0x5A, sizeof k);
         ck(jpake_poisoned(NULL) == 1, "J-PAKE: a NULL exchange is poisoned");
         ck(jpake_accepted(NULL, 1) == 0, "...and holds no accepted round");
         ck(jpake_round1(NULL, out) == 0, "...and emits nothing");
         ck(jpake_peer_round1(NULL, out) == 0, "...and accepts nothing");
         ck(jpake_shared_key(NULL, k) == 0, "...and derives no key");
         ck(unwritten(k, sizeof k, 0x5A), "...writing nothing while refusing");
         ck(jpake_accepted(NULL, 9) == 0, "...and round 9 does not exist");
      }

      /* (11) THE PASSWORD DOMAIN (item 140).
       *
       * jpake_new used to REPAIR every input it could not represent. Above 32
       * bytes it clipped -- `if (passlen > 32) passlen = 32;` -- so a 33-byte
       * passphrase and its 32-byte PREFIX became one password, and since the
       * two sides never compare passwords, only derived keys, the symptom of
       * two different secrets colliding is that the pairing SUCCEEDS. Empty
       * and all-zero inputs became the zero scalar, which is not a password at
       * all: x2s = privB*pass and the key exponent x2*pass both collapse, so
       * the derived point stops depending on the secret.
       *
       * Every case here pairs with a CONTROL one byte or one value away, so
       * "it refuses" cannot be satisfied by refusing everything. */
      {
         uint8_t pw[JPAKE_PASS_MAX + 1];
         struct jpake *p;
         for (size_t i = 0; i < sizeof pw; i++)
            pw[i] = (uint8_t)('a' + (i % 26));

         /* THE BOUND, and one past it. These two inputs share all 32 of the
          * first password's bytes, which is exactly the pair the clip made
          * indistinguishable. */
         p = jpake_new(pw, JPAKE_PASS_MAX, 1);
         ck(p != NULL, "J-PAKE control: a password of exactly 32 bytes is "
                       "accepted");
         jpake_free(p);
         p = jpake_new(pw, JPAKE_PASS_MAX + 1, 1);
         ck(p == NULL, "J-PAKE REFUSES a 33-byte password rather than "
                       "silently pairing on its 32-byte prefix");
         jpake_free(p);

         /* NOTHING AT ALL, in both of its shapes. */
         ck(jpake_new(pw, 0, 1) == NULL, "J-PAKE REFUSES an empty password");
         ck(jpake_new(NULL, 4, 1) == NULL,
            "J-PAKE REFUSES a NULL password with a nonzero length");
         ck(jpake_new(NULL, 0, 1) == NULL, "...and a NULL with no length");
         p = jpake_new(pw, 1, 1);
         ck(p != NULL,
            "J-PAKE control: one byte is the shortest password there is, and "
            "it is inside the domain");
         jpake_free(p);

         /* ZERO AFTER REDUCTION, by both routes that reach it. The bytes
          * differ completely; the scalar is the same invalid one, which is
          * why the test is on the scalar and not on the bytes. */
         uint8_t zpw[4] = {0, 0, 0, 0};
         ck(jpake_new(zpw, sizeof zpw, 1) == NULL,
            "J-PAKE REFUSES an all-zero password");
         uint8_t npw[32];
         hex(SC_N, npw, 32);
         ck(jpake_new(npw, 32, 1) == NULL,
            "J-PAKE REFUSES a password equal to the group order, which "
            "reduces to the same invalid zero");
         hex(SC_NM1, npw, 32);
         p = jpake_new(npw, 32, 1);
         ck(p != NULL, "J-PAKE control: n-1 is a perfectly good password");
         jpake_free(p);
         hex(SC_1, npw, 32);
         p = jpake_new(npw, 32, 1);
         ck(p != NULL, "J-PAKE control: so is 1");
         jpake_free(p);

         /* THE LEADING ZERO IS INVISIBLE, which the header states and which
          * is asserted here so the statement is executed rather than
          * believed: right-justified big-endian means a zero byte in front
          * changes nothing. Both are accepted, and both are the same
          * password. */
         uint8_t lead[3] = {0x00, 'a', 'b'};
         p               = jpake_new(lead, 3, 1);
         ck(p != NULL, "J-PAKE accepts a password with a leading zero byte");
         jpake_free(p);
      }

      /* Restore by the handle, so nothing after this section runs on the
       * deterministic stream. */
      (void)rand_set_source(prev);
   }

   /* ---- THE GCM PER-INVOCATION LIMITS (SP 800-38D 5.2.1.1), ITEM 64 ----
    *
    * WHAT THIS SECTION CAN AND CANNOT DRIVE, said first because the honest
    * scope of it is not obvious and an assertion that implies more than it
    * checks is worse than no assertion.
    *
    * The bounds are 2^36 - 32 bytes of plaintext and 2^61 - 1 bytes of AAD --
    * 64 GiB and 2 EiB. Nothing here allocates either. So lib/gcm.c splits the
    * length rule into aes128_gcm_limits(), a pure predicate over two uint64_t
    * byte counts with no buffer and no key, and that predicate is what the
    * boundary assertions below pin: EXACTLY at the bound, and one past it, on
    * both sides. The sealer and the opener call the same predicate rather than
    * carrying their own copy of the comparison, so what is proven about it is
    * proven about the rule they enforce -- but it is NOT the same as having run
    * the sealer on 64 GiB, and nothing below should be read as claiming that.
    *
    * What IS driven through the real sealer and opener: every REFUSAL. That
    * works precisely because the refusal is decided before any output byte is
    * produced, so a bogus length can be handed to seal() alongside a 64-byte
    * buffer and no memory is ever touched -- the same trick the PBKDF2 dkLen
    * case above uses. The ACCEPTANCE at the exact bound is the one thing only
    * the predicate can be asked about.
    *
    * Every refusal fills BOTH the ciphertext buffer AND THE TAG with a sentinel
    * first and asserts neither moved. The tag is the point: a caller that
    * ignored the status is likeliest to go on and treat those sixteen bytes as
    * meaningful, so a refusal that wrote a tag would be a refusal nobody could
    * act on. */
   {
      uint8_t key[16], iv[12], aad[20], pt[64], ct[64], tag[16], back[64];
      hex("feffe9928665731c6d6a8f9467308308", key, 16);
      hex("cafebabefacedbaddecaf888", iv, 12);
      hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
      memset(pt, 0x11, sizeof pt);

      /* (1) THE COUNTER WALK THE BOUND IS DERIVED FROM.
       *
       * Pure integer arithmetic on a uint32_t, which is the type gctr() holds
       * the counter in and therefore the whole of the defect. Data block i
       * (1-based) uses counter (uint32_t)(1 + i), so this says, in order: the
       * last legal block is the last counter value; one block past the bound is
       * where the counter WRAPS; two past it collides with J0, whose keystream
       * masks the tag -- so that block's ciphertext is pt XOR S and hands over
       * S, which is tag forgery for this (key, IV); three past it repeats
       * block 1's keystream outright, which is where ct[j] XOR ct[k] starts
       * giving up pt[j] XOR pt[k].
       *
       * This is a model of gctr's counter, not gctr itself -- gctr is static
       * and cannot be reached from here, and running it that far would take
       * 64 GiB. What it pins is the arithmetic GCM_PT_MAX is DERIVED from, so
       * that the constant and its justification cannot drift apart silently.
       * lib/gcm.c carries the same four facts as _Static_asserts, which is the
       * belt to this braces. */
      {
         uint64_t blocks = GCM_PT_MAX / 16;
         ck(blocks == GCM_CTR_BLOCKS_MAX,
            "GCM: the plaintext bound is exactly the counter's block capacity");
         ck((uint32_t)(1ull + blocks) == 0xFFFFFFFFu,
            "GCM: the last legal block uses the last counter value");
         ck((uint32_t)(1ull + blocks + 1) == 0u,
            "GCM: one block past the bound WRAPS the 32-bit counter");
         ck((uint32_t)(1ull + blocks + 2) == 1u,
            "GCM: two past it collides with J0 -- the TAG MASK leaks");
         ck((uint32_t)(1ull + blocks + 3) == 2u,
            "GCM: three past it repeats the message's own first keystream");
         /* And the bound really is the bit figure the standard prints. */
         ck(GCM_PT_MAX * 8ull == (1ull << 39) - 256ull,
            "GCM: the byte bound is SP 800-38D 5.2.1.1's 2^39 - 256 bits");
         ck(GCM_AAD_MAX * 8ull == 0xFFFFFFFFFFFFFFF8ull,
            "GCM: the AAD bound is the largest byte count whose bit count "
            "fits");
      }

      /* (2) THE PREDICATE AT BOTH BOUNDS, EXACTLY AND ONE PAST.
       *
       * This is the only place the ACCEPTED side of either bound is asserted,
       * for the reason set out at the top of the section. Each case leaves the
       * other length legal, so exactly one rule can be the reason -- the same
       * isolation discipline as the HKDF and PBKDF2 cases above, and it matters
       * here for the same reason: "a huge plaintext is refused" would be true
       * of a build with no AAD bound at all. */
      ck(aes128_gcm_limits(0, 0) == GCM_OK,
         "GCM limits: empty plaintext and empty AAD are legal");
      ck(aes128_gcm_limits(0, GCM_PT_MAX) == GCM_OK,
         "GCM limits: accepts a plaintext of EXACTLY 2^36 - 32 bytes");
      ck(aes128_gcm_limits(0, GCM_PT_MAX + 1) == GCM_ERR_PT_LEN,
         "GCM limits: REFUSES one byte more, the byte that wraps the counter");
      ck(aes128_gcm_limits(GCM_AAD_MAX, 0) == GCM_OK,
         "GCM limits: accepts an AAD of EXACTLY 2^61 - 1 bytes");
      ck(aes128_gcm_limits(GCM_AAD_MAX + 1, 0) == GCM_ERR_AAD_LEN,
         "GCM limits: REFUSES one byte more, where aadn * 8 wraps to zero");

      /* THE ORDER, which gcm.h documents and every case above depends on: a
       * call that breaks both rules must report the AAD one. Without this the
       * two "one past" cases could each be firing on the other's rule and
       * nothing would say so. */
      ck(aes128_gcm_limits(GCM_AAD_MAX + 1, GCM_PT_MAX + 1) == GCM_ERR_AAD_LEN,
         "GCM limits: aadn is judged before n, so a doubly-illegal call names "
         "the AAD");

      /* (3) THE SAME REFUSALS THROUGH THE REAL SEALER AND OPENER.
       *
       * Asked with a 64-byte buffer on purpose. If the length were inspected
       * anywhere after the first write this would segfault instead of failing
       * an assertion, which is itself a distinction worth having: the sentinel
       * checks below can only be reached by a build that refused early.
       *
       * Skipped where size_t cannot express a length past the bound -- on a
       * 32-bit platform no size_t reaches 2^36, so the question is not askable
       * through this API at all and the predicate above is the whole of the
       * coverage. Stated rather than silently compiled out. */
#if SIZE_MAX > GCM_PT_MAX
      memset(ct, 0x5A, sizeof ct);
      memset(tag, 0x5A, sizeof tag);
      ck(aes128_gcm_seal(key, iv, NULL, 0, pt, (size_t)GCM_PT_MAX + 1, ct,
                         tag) == GCM_ERR_PT_LEN,
         "GCM seal REFUSES a plaintext past 2^39 - 256 bits with "
         "GCM_ERR_PT_LEN");
      ck(unwritten(ct, sizeof ct, 0x5A),
         "...without producing a single ciphertext byte");
      ck(unwritten(tag, sizeof tag, 0x5A),
         "...and WITHOUT WRITING THE TAG, which is the byte a caller trusts");

      memset(ct, 0x5A, sizeof ct);
      memset(tag, 0x5A, sizeof tag);
      ck(aes128_gcm_seal(key, iv, aad, (size_t)GCM_AAD_MAX + 1, pt, 64, ct,
                         tag) == GCM_ERR_AAD_LEN,
         "GCM seal REFUSES an AAD past 2^64 - 1 bits with GCM_ERR_AAD_LEN");
      ck(unwritten(ct, sizeof ct, 0x5A) && unwritten(tag, sizeof tag, 0x5A),
         "...writing neither ciphertext nor tag");

      /* THE OPENER REFUSES THE SAME LENGTHS, and this is the case that says
       * why: a ciphertext longer than the bound cannot have been produced by a
       * conforming sealer, so accepting it is accepting something no honest
       * peer sent -- and two copies of the OLD code would have agreed with each
       * other perfectly while both wrapped the counter. */
      memset(back, 0x5A, sizeof back);
      ck(aes128_gcm_unseal(key, iv, NULL, 0, ct, (size_t)GCM_PT_MAX + 1, tag,
                           back) == GCM_ERR_PT_LEN,
         "GCM unseal REFUSES an over-long ciphertext no sealer could produce");
      ck(unwritten(back, sizeof back, 0x5A), "...writing no plaintext");
      ck(aes128_gcm_unseal(key, iv, aad, (size_t)GCM_AAD_MAX + 1, ct, 64, tag,
                           back) == GCM_ERR_AAD_LEN,
         "GCM unseal REFUSES an over-long AAD");
      ck(unwritten(back, sizeof back, 0x5A), "...writing no plaintext either");

      /* And through the boolean wrapper, whose widened contract is the reason
       * srv/tls.c's two call sites stay correct without being edited: 0 now
       * covers a forbidden length as well as a bad tag, and both sites reject
       * on 0. */
      memset(back, 0x5A, sizeof back);
      ck(!aes128_gcm_open(key, iv, NULL, 0, ct, (size_t)GCM_PT_MAX + 1, tag,
                          back),
         "GCM open reports 0 for a length SP 800-38D forbids, not only for a "
         "bad tag");
      ck(unwritten(back, sizeof back, 0x5A), "...and writes no plaintext");
#endif

      /* (4) THE ZERO-LENGTH CASES, WHICH ARE LEGAL AND MUST STAY SO. A bound
       * that also refused an empty input would be caught by nothing above --
       * every case so far asks about the top end. */

      /* EMPTY PLAINTEXT WITH EMPTY AAD: the GCM specification's own test case
       * 1, so this is a published answer and not a "it returned OK". GCM with
       * no plaintext is GMAC, and the tag is the entire output. */
      {
         uint8_t zk[16] = {0}, zi[12] = {0}, ztag[16];
         memset(ztag, 0x5A, sizeof ztag);
         ck(aes128_gcm_seal(zk, zi, NULL, 0, NULL, 0, NULL, ztag) == GCM_OK,
            "GCM seal accepts an empty plaintext AND an empty AAD");
         cmp(ztag, "58e2fccefa7e3061367f1d57a4e7455a", 16,
             "...and the tag is GCM test case 1's");
         ck(aes128_gcm_unseal(zk, zi, NULL, 0, NULL, 0, ztag, NULL) == GCM_OK,
            "...and unseal verifies it with no ciphertext at all");
      }

      /* EMPTY PLAINTEXT WITH A 20-BYTE AAD. There is no published AES-128
       * vector for this shape in the GCM specification's set (its cases go
       * empty/empty, then plaintext-only, then both), so the answer comes from
       * an INDEPENDENT implementation -- Python's cryptography AESGCM, checked
       * on the same run against test cases 1 and 2 above so it is not being
       * trusted blind. */
      memset(tag, 0x5A, sizeof tag);
      ck(aes128_gcm_seal(key, iv, aad, 20, NULL, 0, NULL, tag) == GCM_OK,
         "GCM seal accepts a 20-byte AAD with an EMPTY plaintext");
      cmp(tag, "346434fd51d5cd0c5887ec63e39b907a", 16,
          "...and agrees with an independent GCM on the GMAC tag");

      /* AND THE OTHER WAY ROUND -- a plaintext with an EMPTY AAD -- asserted
       * here through the status-returning API on the published NIST case 3
       * vector at the top of this file. The happy path returning GCM_OK is not
       * something the older cases can say, because they were written when seal
       * returned void. */
      hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39"
          "1aafd255",
          pt, 64);
      memset(ct, 0x5A, sizeof ct);
      memset(tag, 0x5A, sizeof tag);
      ck(aes128_gcm_seal(key, iv, NULL, 0, pt, 64, ct, tag) == GCM_OK,
         "GCM seal REPORTS GCM_OK on the NIST case 3 vector");
      cmp(ct,
          "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
          "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
          "473f5985",
          64,
          "...and the ciphertext is still NIST's, with the bounds in place");
      cmp(tag, "4d5c2af327cd64a62cf35abd2ba6fab4", 16, "...and so is the tag");

      /* (5) THE TAG FAILURE IS NAMED, not merely reported as "not OK". A
       * forged tag and a forbidden length are different events and only
       * aes128_gcm_unseal can tell a caller which happened. Everything else in
       * this call is legal, so GCM_ERR_TAG is the only rule that can fire. */
      {
         uint8_t bad[16];
         memcpy(bad, tag, 16);
         bad[0] ^= 1;
         memset(back, 0x5A, sizeof back);
         ck(aes128_gcm_unseal(key, iv, NULL, 0, ct, 64, bad, back) ==
                GCM_ERR_TAG,
            "GCM unseal names GCM_ERR_TAG for a forged tag");
         ck(unwritten(back, sizeof back, 0x5A),
            "...and writes no plaintext for it");
         ck(aes128_gcm_unseal(key, iv, NULL, 0, ct, 64, tag, back) == GCM_OK,
            "GCM unseal accepts the genuine tag (the control)");
         ck(memcmp(back, pt, 64) == 0, "...and returns the plaintext");
         /* THE WRAPPER'S POLARITY, BOTH WAYS. If aes128_gcm_open had been
          * turned into the enum directly, GCM_OK == 0 would have inverted
          * srv/tls.c's two `if (!open(...))` sites into rejecting every good
          * record, and no compiler warns about `!` on an enum. These two lines
          * are what stands in for that missing diagnostic. */
         ck(aes128_gcm_open(key, iv, NULL, 0, ct, 64, tag, back) == 1,
            "GCM open returns 1 -- not 0 -- for an authentic record");
         ck(aes128_gcm_open(key, iv, NULL, 0, ct, 64, bad, back) == 0,
            "GCM open returns 0 for a forged one");
      }

      /* (6) A LONGER MESSAGE, so the counter is actually walked. 4096 bytes is
       * 256 blocks and counters 2 through 257, which is the only case in this
       * file where the counter crosses a byte boundary in the counter block --
       * a `uint8_t` counter, or a byte written in the wrong order, would still
       * pass every 64-byte vector above. Independent answer again. */
      {
         static uint8_t big[4096], bigct[4096], bigback[4096];
         uint8_t bigtag[16];
         for (size_t i = 0; i < sizeof big; i++)
            big[i] = (uint8_t)(i * 7 + 3);
         ck(aes128_gcm_seal(key, iv, aad, 20, big, sizeof big, bigct, bigtag) ==
                GCM_OK,
            "GCM seal accepts 4096 bytes with a 20-byte AAD");
         cmp(bigct, "98b83dffc6d55ff5d56961227c7b976a", 16,
             "...first block agrees with an independent GCM");
         cmp(bigct + sizeof big - 16, "85859a6b66da025e3104606729a456ef", 16,
             "...and so does block 256, at counter 257");
         cmp(bigtag, "ca2d0c52fcf656592d5aa49b87f97d0b", 16,
             "...and so does the tag over all 256 blocks");
         ck(aes128_gcm_unseal(key, iv, aad, 20, bigct, sizeof big, bigtag,
                              bigback) == GCM_OK,
            "...and it unseals");
         ck(memcmp(bigback, big, sizeof big) == 0,
            "...back to the same 4096 bytes");
      }

      /* (7) NULLS. A NULL with a length behind it is a caller bug and is
       * reported as a DIFFERENT failure from either bound, so a test cannot
       * mistake one for the other. A NULL with a zero length is not a bug at
       * all -- case 1 above is exactly that call -- which is why the rules are
       * written as pairs and why both halves need asserting. */
      memset(ct, 0x5A, sizeof ct);
      memset(tag, 0x5A, sizeof tag);
      ck(aes128_gcm_seal(NULL, iv, NULL, 0, pt, 64, ct, tag) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL key");
      ck(aes128_gcm_seal(key, NULL, NULL, 0, pt, 64, ct, tag) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL nonce");
      ck(aes128_gcm_seal(key, iv, NULL, 20, pt, 64, ct, tag) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL AAD with a nonzero length");
      ck(aes128_gcm_seal(key, iv, NULL, 0, NULL, 64, ct, tag) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL plaintext with a nonzero length");
      ck(aes128_gcm_seal(key, iv, NULL, 0, pt, 64, NULL, tag) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL ciphertext with a nonzero length");
      ck(aes128_gcm_seal(key, iv, NULL, 0, pt, 64, ct, NULL) == GCM_ERR_ARG,
         "GCM seal REFUSES a NULL tag -- there is nowhere to put the answer");
      ck(unwritten(ct, sizeof ct, 0x5A) && unwritten(tag, sizeof tag, 0x5A),
         "...and not one of the six wrote a ciphertext or a tag byte");

      memset(back, 0x5A, sizeof back);
      ck(aes128_gcm_unseal(NULL, iv, NULL, 0, ct, 64, tag, back) == GCM_ERR_ARG,
         "GCM unseal REFUSES a NULL key");
      ck(aes128_gcm_unseal(key, iv, NULL, 20, ct, 64, tag, back) == GCM_ERR_ARG,
         "GCM unseal REFUSES a NULL AAD with a nonzero length");
      ck(aes128_gcm_unseal(key, iv, NULL, 0, NULL, 64, tag, back) ==
             GCM_ERR_ARG,
         "GCM unseal REFUSES a NULL ciphertext with a nonzero length");
      ck(aes128_gcm_unseal(key, iv, NULL, 0, ct, 64, NULL, back) == GCM_ERR_ARG,
         "GCM unseal REFUSES a NULL tag rather than verifying against nothing");
      ck(aes128_gcm_unseal(key, iv, NULL, 0, ct, 64, tag, NULL) == GCM_ERR_ARG,
         "GCM unseal REFUSES a NULL plaintext with a nonzero length");
      ck(unwritten(back, sizeof back, 0x5A),
         "...and none of the five wrote a plaintext byte");
      ck(!aes128_gcm_open(key, iv, NULL, 0, ct, 64, NULL, back),
         "GCM open reports 0 for a NULL tag, not 1");
   }

   /* ==== ITEM 65: ONE SECRET-SCALAR GENERATOR, AND THE VALUES IT REFUSES ====
    *
    * lib/p256.h has the rule and lib/p256.c the arithmetic. What this section
    * exists to say is that the rule is EXECUTED, at the boundary, on the values
    * a working entropy source will never produce.
    *
    * WHAT WAS THERE BEFORE. Four call sites -- lib/jpake.c's five scalars per
    * exchange, srv/tls.c's ECDHE scalar, srv/tls.c's CertificateVerify nonce
    * and app/dexcom.c's key-challenge nonce -- each drew 32 bytes and reduced
    * them mod n with p256_sc_from_be. Two defects in one line: the reduction is
    * biased (the bottom 2^-32 of the interval comes out twice as often), and 0
    * and n both reduce to 0 and were handed back as ordinary scalars.
    *
    * WHY THE ZERO HALF IS THE INTERESTING HALF, and how far it actually got.
    * A zero ECDHE scalar makes the shared point the identity; a zero J-PAKE
    * proof nonce turns the Schnorr proof into -H*priv, which publishes the
    * witness. Neither was reachable in THIS tree, and the reason is worth being
    * exact about because it is not a reason anybody chose: a zero scalar makes
    * the emitted point infinite, an infinite point has no affine encoding, and
    * p256_to_xy refuses it -- so jpake's cert_byteify and tls.c's shared-secret
    * step both failed before a byte was transmitted. Two unrelated functions
    * were holding the line. The cases below are written knowing that, and the
    * ones that would still pass against the old code are marked as such rather
    * than left to imply more than they check.
    *
    * The generator's failure return is checked at every call, because a
    * generator that refuses correctly while a caller carries on is the same bug
    * one function further down. */
   {
      uint8_t sc[32];
      rand_source_fn prev = rand_set_source(src_scalar);

      /* (1) ZERO IS REFUSED, NOT ACCEPTED AS THE SCALAR 0. A source stuck at
       * zero -- an unseeded PRNG, a stub, a device that reads as holes -- is
       * the shape this is really about, and it is the one the old reduction was
       * happiest with. */
      scf_reset();
      scf_push(SC_0);
      ck(p256_sc_rand(sc) == 0, "a scalar of all zero bytes is REFUSED");
      ck(scf_calls == P256_SC_RAND_TRIES,
         "...having redrawn to the cap rather than accepting it");

      /* (2) EXACTLY n IS REFUSED. n is the one out-of-range value the old fold
       * turned into zero, so this is both boundary cases at once.
       *
       * READ THIS ONE CAREFULLY: it does NOT isolate the range check. Replace
       * the range test with the old reduction and n still comes back refused --
       * because it reduces to zero and case (1)'s test catches it. Cases (5)
       * and (6) are what pin "reject, do not reduce". This case pins the
       * boundary itself, and the third assertion is what makes it worth
       * writing: the output is zeroed, so a caller that ignores the return does
       * not proceed with n's own bytes. */
      scf_reset();
      scf_push(SC_N);
      memset(sc, 0x5A, sizeof sc);
      ck(p256_sc_rand(sc) == 0, "exactly n is REFUSED, not folded to zero");
      ck(scf_calls == P256_SC_RAND_TRIES, "...redrawing to the cap");
      cmp(sc, SC_0, 32,
          "...and a refusal leaves zeros, not the rejected value");

      /* (3) n-1 IS ACCEPTED, UNCHANGED. The largest valid private key there is.
       * An interval that is exclusive at the wrong end would refuse it and no
       * caller would ever notice; `cmp` is here because "accepted" is not the
       * claim -- "accepted and handed back as drawn" is. */
      scf_reset();
      scf_push(SC_NM1);
      ck(p256_sc_rand(sc) == 1, "n-1 is ACCEPTED");
      ck(scf_calls == 1, "...on the first draw");
      cmp(sc, SC_NM1, 32, "...and comes back exactly as drawn");

      /* (4) ONE IS ACCEPTED. The other end of the interval. A generator that
       * "played safe" by refusing small scalars would be biased in the opposite
       * direction and would still pass every case above. */
      scf_reset();
      scf_push(SC_1);
      ck(p256_sc_rand(sc) == 1, "the scalar 1 is ACCEPTED");
      cmp(sc, SC_1, 32, "...and comes back exactly as drawn");

      /* (5) THE RETRY PATH, AND THE ISOLATING ASSERTION FOR "REJECT, DO NOT
       * REDUCE". Two draws above n, then a good one. The value returned must be
       * the LATER one: a reduction would have returned n+1 - n == 1 from the
       * FIRST draw, which is a perfectly plausible-looking scalar and the exact
       * shape of the bug being removed. */
      scf_reset();
      scf_push(SC_NP1);
      scf_push(SC_FF);
      scf_push(SC_OK);
      ck(p256_sc_rand(sc) == 1, "two out-of-range draws are followed by a good "
                                "one, and the result is a success");
      ck(scf_calls == 3, "...after exactly three asks");
      cmp(sc, SC_OK, 32,
          "...and the accepted scalar is the LATER draw, which "
          "is what says it was rejected and not reduced");

      /* (6) A SOURCE THAT IS ALWAYS OUT OF RANGE FAILS CLEANLY, WITHIN THE CAP.
       * The bounded failure is the requirement: a rejection sampler with no
       * bound is a `while (1)` the first time a stuck source hands it 0xff..ff,
       * and a worker that stops answering is worse than a handshake that fails.
       * The cap is asserted TWICE on purpose -- once as "the loop ran exactly
       * that many times" and once as "that many is the documented 64" --
       * because the first assertion alone is satisfied by any cap, including
       * one small enough to fail against a healthy source. */
      scf_reset();
      scf_push(SC_FF);
      memset(sc, 0x5A, sizeof sc);
      ck(p256_sc_rand(sc) == 0,
         "a source whose every draw is out of range fails, and RETURNS");
      ck(scf_calls == P256_SC_RAND_TRIES, "...at the retry cap");
      ck(P256_SC_RAND_TRIES == 64,
         "...and the cap is the documented 64 draws (2^-2048 by chance)");
      cmp(sc, SC_0, 32, "...leaving zeros, not the out-of-range draw");

      /* (7) A SOURCE THAT FAILS OUTRIGHT PROPAGATES, AND IS NOT ASKED AGAIN.
       * The distinction matters: an out-of-range value is bad luck and gets
       * another draw, a source that says no is fatal (rand.h) and redrawing
       * from it is how a caller ends up spinning 64 times on a dead
       * /dev/urandom. */
      scf_reset();
      scf_push(SC_OK);
      scf_fail = 1;
      ck(p256_sc_rand(sc) == 0, "a source that refuses is propagated");
      ck(scf_calls == 1, "...and is not asked a second time");

      /* (8) THE FAILURE REACHES THE J-PAKE CALLER. jpake_new draws five
       * scalars; every one of them goes through the generator and any failure
       * must come back as NULL, which app/dexdriver.c, app/sync.c, srv/pair.c
       * and srv/synccli.c all already treat as a dead pairing.
       *
       * THE FIRST OF THESE IS THE ISOLATING CASE FOR THE WHOLE ITEM at the
       * caller level. Against the old code a zero-filled source produced a
       * perfectly ordinary non-NULL exchange with all five secrets set to zero,
       * and the pairing only came apart later, inside cert_byteify. The second
       * assertion would have passed before this item as well -- rand_bytes
       * already reported a dead source and rand_scalar already propagated it --
       * and is here so the two failure modes are not confused. */
      scf_reset();
      scf_push(SC_0);
      struct jpake *jz = jpake_new(JP_PIN, sizeof JP_PIN, 1);
      ck(jz == NULL, "jpake_new REFUSES to build an exchange when every draw "
                     "is the zero scalar");
      jpake_free(jz);
      scf_reset();
      scf_fail         = 1;
      struct jpake *jf = jpake_new(JP_PIN, sizeof JP_PIN, 1);
      ck(jf == NULL, "jpake_new refuses when the source itself fails");
      jpake_free(jf);

      /* (9) THE FAILURE REACHES THE TLS ECDHE CALLER -- as far as it can be
       * shown, which is not all the way, and the limit is stated rather than
       * hidden.
       *
       * A real tls_handshake runs against the canned ClientHello. With every
       * draw out of range the handshake must fail having sent NOTHING: the
       * scalar is drawn before send_server_hello, so a client that hears
       * anything at all heard it from a server that got a scalar. The draw
       * count is asserted because it is what proves the fixture reached the
       * ECDHE block: a hello this server rejected for some parsing reason would
       * also write nothing, and would say nothing about scalars.
       *
       * WHAT THIS CANNOT SHOW: whether srv/tls.c actually CHECKS the return.
       * Delete the check and the handshake still fails having written nothing,
       * because p256_sc_rand zeroes its output, 0*G and 0*P are the point at
       * infinity, and p256_to_xy refuses to encode either -- still before
       * send_server_hello. The two are indistinguishable from outside, and that
       * is a property of the zeroing being fail-safe, not an assertion that
       * could be written better. See the report. */
      {
         int rc = -1;
         scf_reset();
         scf_push(SC_FF);
         int wrote = hs_bytes_written(&rc);
         ck(rc == 0, "a TLS handshake FAILS when no ECDHE scalar can be drawn");
         ck(wrote == 0, "...and the client is told nothing at all");
         ck(scf_calls == P256_SC_RAND_TRIES,
            "...and it was the ECDHE draw that ran out of retries");

         /* THE CONTROL, on the platform source. The same hello, the same
          * fixture, and now the server does reach ServerHello -- which is what
          * says the refusal above was the rule and not a broken hello. It still
          * ends in failure, at CertificateVerify, because tls_init was never
          * called and the signing key is all zeros. */
         (void)rand_set_source(prev);
         rc         = -1;
         int wrote2 = hs_bytes_written(&rc);
         ck(wrote2 > 0, "control: the same hello DOES reach ServerHello with a "
                        "working source");
         (void)rand_set_source(src_scalar);

         /* (10) THE CertificateVerify NONCE IS DRAWN THROUGH THE SAME
          * GENERATOR. Two good draws -- the ECDHE scalar, then the ServerHello
          * random -- and then rubbish, so the third scalar-sized ask is the
          * signing nonce and it is out of range.
          *
          * The observable is the ASK COUNT, and it is the only one available:
          * with the signing key at zero the handshake ends at
          * ecdsa_p256_sign either way, so `rc` and the byte count cannot tell
          * the two apart. 2 + 64 asks says the third one was rejected and
          * redrawn to the cap; the old `rnd(nonce, 32)` would have taken it on
          * the first ask, for 3 in total. If a future change adds another
          * entropy draw between the hello and the signature, this is the
          * assertion that will say so. */
         scf_reset();
         scf_push(SC_OK);
         scf_push(SC_OK);
         scf_push(SC_FF);
         rc         = -1;
         int wrote3 = hs_bytes_written(&rc);
         ck(rc == 0,
            "a TLS handshake fails when no CertificateVerify nonce can "
            "be drawn");
         ck(wrote3 > 0, "...after the ServerHello, not before it");
         ck(scf_calls == 2 + P256_SC_RAND_TRIES,
            "...and the signing nonce was rejected to the cap, which is what "
            "says it goes through the generator and not through raw bytes");
      }

      /* Restored by the handle, so nothing after this runs on the script. */
      (void)rand_set_source(prev);
      uint8_t live[32];
      ck(p256_sc_rand(live) == 1,
         "and the platform source still yields a scalar once restored");
      ck(!unwritten(live, sizeof live, 0),
         "...which is not the all-zero failure value");
   }

   /* ---- ITEM 133: EVERY VECTOR IN A CLIENTHELLO MUST EXHAUST ----------
    *
    * One case per vector, each breaking that vector's own rule and nothing
    * else, so a loosened check fails the assertion that names it rather than
    * some neighbour's. The controls come first: if a well-formed hello were
    * refused too, every line below would pass for the wrong reason. */
   {
      struct ch_flaw f;
      struct hs_out o;
      uint8_t rec[2048];
      size_t at;

      memset(&f, 0, sizeof f);
      run_ch(rec, mk_ch(&f, rec, sizeof rec, &at), &o);
      ck(o.server_hello && o.wrote > 0,
         "CONTROL: a well-formed ClientHello is answered");
      ck(!o.resumed, "...with no PSK, having offered none");

      memset(&f, 0, sizeof f);
      f.psk = 1; /* well framed, but a ticket this server never sealed */
      run_ch(rec, mk_ch(&f, rec, sizeof rec, &at), &o);
      ck(o.server_hello && o.wrote > 0,
         "CONTROL: a well-framed PSK offer we cannot open is a full handshake");
      ck(!o.resumed, "...and is not resumed");

      memset(&f, 0, sizeof f);
      f.cs_tail = 1;
      ck_refused(&f, "a cipher_suites vector with a trailing byte is refused");

      memset(&f, 0, sizeof f);
      f.comp_two = 1;
      ck_refused(&f, "legacy_compression_methods other than the one zero byte "
                     "is refused");

      memset(&f, 0, sizeof f);
      f.ver_tail = 1;
      ck_refused(&f,
                 "a supported_versions vector with a trailing byte is refused");

      memset(&f, 0, sizeof f);
      f.sig_tail = 1;
      ck_refused(
          &f, "a signature_algorithms vector with a trailing byte is refused");

      memset(&f, 0, sizeof f);
      f.ks_tail = 1;
      ck_refused(&f, "a key_share client_shares vector with a trailing byte is "
                     "refused");

      memset(&f, 0, sizeof f);
      f.ks_over = 1;
      ck_refused(&f, "a client_shares length that overruns key_share is "
                     "refused");

      memset(&f, 0, sizeof f);
      f.modes_tail = 1;
      f.psk        = 1;
      ck_refused(&f, "a psk_key_exchange_modes extension with a trailing byte "
                     "is refused");

      memset(&f, 0, sizeof f);
      f.ext_tail = 1;
      ck_refused(&f, "trailing bytes after the last extension are refused");

      memset(&f, 0, sizeof f);
      f.ext_over = 1;
      ck_refused(&f, "an extension block that overruns the ClientHello is "
                     "refused");

      memset(&f, 0, sizeof f);
      f.hello_tail = 1;
      ck_refused(&f, "trailing bytes after the extension block are refused");

      memset(&f, 0, sizeof f);
      f.dup = 1;
      ck_refused(&f, "a duplicated extension is refused");

      memset(&f, 0, sizeof f);
      f.psk        = 1;
      f.ident_tail = 1;
      ck_refused(&f, "a PSK identity list with a trailing byte is refused");

      memset(&f, 0, sizeof f);
      f.psk         = 1;
      f.binder_tail = 1;
      ck_refused(&f, "a PSK binder list with a trailing byte is refused");

      /* THE SHARP ONE. A binder list that stops short is not a partial offer
       * to be read as far as it goes: the identities it does not cover are
       * carried by no MAC at all, and the server used to select identity 0
       * and never notice the other four. */
      memset(&f, 0, sizeof f);
      f.psk     = 1;
      f.idents  = 2;
      f.binders = 1;
      ck_refused(&f, "a binder list SHORTER than the identity list is refused");

      memset(&f, 0, sizeof f);
      f.psk     = 1;
      f.idents  = 1;
      f.binders = 2;
      ck_refused(&f, "...and one LONGER than it is refused too");

      memset(&f, 0, sizeof f);
      f.psk        = 1;
      f.binder_len = 20; /* PskBinderEntry is opaque<32..255> */
      ck_refused(&f, "a binder shorter than the RFC's minimum is refused");
   }

   /* ---- ITEM 133 and 134: A REAL TICKET, AND THE CLOCK UNDER IT --------
    *
    * Everything above is decided while the message is being framed. These
    * cases go the whole way: a ticket this process really sealed, a binder
    * this fixture really computes, and a resumption the server really
    * performs -- which is the only control that can say the refusals below
    * are rules and not a server that stopped resuming.
    *
    * tls_init is never called here, so the ticket key is the all-zero static
    * it starts as. That is a fixed key rather than a secret one, which is
    * exactly what a fixture wants: seal and open agree, run after run. */
   {
      uint8_t psk[32];
      uint8_t tick[80];
      uint8_t rec[2048];
      size_t at;
      struct ch_flaw f;
      struct hs_out o;
      memset(psk, 0xA5, sizeof psk);

      /* nonce || AEAD(issued_at || psk) || tag, as srv/tls.c seals it. Spelled
       * out rather than "whatever came back", so a ticket that quietly stopped
       * carrying its issue time would be a failure here. */
      size_t want_tn = 12 + 8 + 32 + 16;
      size_t tn      = tls_fault_ticket_seal(psk, tick);
      ck(tn == want_tn, "a ticket seals under a working clock");
      uint8_t got[32];
      ck(tls_fault_ticket_open(tick, tn, got) && memcmp(got, psk, 32) == 0,
         "...and opens back to the PSK it sealed");

      memset(&f, 0, sizeof f);
      f.psk      = 1;
      f.ticket   = tick;
      f.ticket_n = tn;
      size_t rn  = mk_ch(&f, rec, sizeof rec, &at);
      fix_binder(rec, at, psk);
      run_ch(rec, rn, &o);
      ck(o.resumed, "CONTROL: a live ticket with a correct binder resumes");

      /* ONE BIT OF THE BINDER. Nothing else in the message reads that byte,
       * so this separates "the binder was checked" from "the hello was
       * malformed" -- and RFC 8446 4.2.11 makes it a MUST abort, not a
       * fallback, so nothing may reach the wire. */
      uint8_t spoilt[2048];
      memcpy(spoilt, rec, rn);
      spoilt[rn - 1] ^= 0x01;
      run_ch(spoilt, rn, &o);
      ck(o.wrote == 0 && !o.resumed,
         "a live ticket whose binder is wrong is refused outright");

      /* A binder that is framed legally and is the wrong WIDTH for the only
       * hash this server speaks. parse_hello accepts opaque<32..255>; the
       * verifier is where 48 bytes stops being a SHA-256 MAC. */
      memset(&f, 0, sizeof f);
      f.psk        = 1;
      f.ticket     = tick;
      f.ticket_n   = tn;
      f.binder_len = 48;
      run_ch(rec, mk_ch(&f, rec, sizeof rec, &at), &o);
      ck(o.wrote == 0 && !o.resumed,
         "a legally framed binder of the wrong width is refused");

      /* ---- ITEM 134: NO MONOTONIC CLOCK, NO TICKET ---------------------
       *
       * Both directions, because the defect was in both: a failed
       * clock_gettime read as second zero was SEALED as the issue time and
       * then COMPARED against second zero on the way back in, so a
       * persistently broken clock made every ticket eternally fresh. */
      setenv("TLS_FAIL_MONOTONIC", "1", 1);
      uint8_t out[80];
      memset(out, 0xEE, sizeof out);
      ck(tls_fault_ticket_seal(psk, out) == 0,
         "no ticket is sealed when the monotonic clock is unavailable");
      ck(unwritten(out, sizeof out, 0xEE),
         "...and not a byte of one was written");

      int sent    = -1;
      int on_wire = ticket_on_wire(&sent);
      ck(sent == 0 && on_wire == 0,
         "no NewSessionTicket reaches the wire when the clock is unavailable");

      /* The ticket was sealed above, under a working clock, and is seconds
       * old. It is the ACCEPTANCE that has no clock. */
      memset(&f, 0, sizeof f);
      f.psk      = 1;
      f.ticket   = tick;
      f.ticket_n = tn;
      rn         = mk_ch(&f, rec, sizeof rec, &at);
      fix_binder(rec, at, psk);
      run_ch(rec, rn, &o);
      ck(!o.resumed, "a live, correctly bound ticket does not resume when the "
                     "clock is unavailable");
      ck(o.server_hello && o.wrote > 0,
         "...it falls back to a full handshake rather than dropping the peer");
      unsetenv("TLS_FAIL_MONOTONIC");

      sent    = -1;
      on_wire = ticket_on_wire(&sent);
      ck(sent == 1 && on_wire > 0,
         "CONTROL: a NewSessionTicket does reach the wire with a clock");

      /* SECOND ZERO IS A TIME, NOT A FAILURE, and telling them apart is the
       * whole of this item: the old code spelled both as 0. With the clock
       * WORKING and reading zero, a ticket is issued and it resumes. */
      setenv("TLS_MONOTONIC_FIXED", "0", 1);
      uint8_t z[80];
      size_t zn = tls_fault_ticket_seal(psk, z);
      ck(zn == want_tn, "a ticket issued at a genuine second zero IS issued");
      memset(&f, 0, sizeof f);
      f.psk      = 1;
      f.ticket   = z;
      f.ticket_n = zn;
      rn         = mk_ch(&f, rec, sizeof rec, &at);
      fix_binder(rec, at, psk);
      run_ch(rec, rn, &o);
      ck(o.resumed, "...and it resumes, which a failed clock must not");

      /* ...and the stamp is really read: the same ticket, one second past
       * the lifetime, is refused. Without this, "resumes at zero" would also
       * be satisfied by a server that never checked the age. */
      setenv("TLS_MONOTONIC_FIXED", "7201", 1);
      run_ch(rec, rn, &o);
      ck(!o.resumed && o.server_hello,
         "...and the same ticket one second past its lifetime does not");
      unsetenv("TLS_MONOTONIC_FIXED");

      /* THE DEFECT, EXACTLY AS IT WAS. A ticket stamped second zero,
       * presented to a run whose clock has failed. Reading the failure AS
       * zero makes the arithmetic 0 - 0 = 0 -- brand new, every time, for as
       * long as the process lives -- which is the "zero-issued tickets pass
       * the age check forever" half of item 134 and the one case a
       * real-clock ticket cannot reach: against a ticket stamped with real
       * uptime, a `now` of zero is simply in the past and gets refused for
       * the wrong reason. `rec` still holds the hello built on `z`. */
      setenv("TLS_FAIL_MONOTONIC", "1", 1);
      run_ch(rec, rn, &o);
      ck(!o.resumed,
         "a ZERO-STAMPED ticket does not resume when the clock has failed");
      unsetenv("TLS_FAIL_MONOTONIC");
   }

   printf("== a secret is wiped by something the compiler may not delete ==\n");
   {
      /* WHAT THIS CAN AND CANNOT PROVE.
       *
       * It cannot prove a compiler did not elide the store -- that is a claim
       * about code generation, and reading the buffer afterwards is exactly
       * the thing that makes the store live and therefore un-elidable. Any
       * test that checks "the bytes are zero" has, by checking, removed the
       * condition it meant to test.
       *
       * What it pins is the CONTRACT: ct_wipe zeroes every byte of the range
       * and nothing outside it, and tolerates a null. That is what the callers
       * depend on. The non-elidability is the volatile qualifier's job, and
       * the argument for it is in ct.h; what a test can do is stop somebody
       * "simplifying" ct_wipe into a memset, which would compile, pass any
       * value check, and silently give the guarantee up.
       *
       * The guard bytes are the half that catches an off-by-one: a wipe that
       * runs one byte long is a buffer overrun into whatever follows a secret,
       * which is usually more secret. */
      unsigned char buf[64];
      memset(buf, 0xA5, sizeof buf);
      ct_wipe(buf + 8, 16);
      int spilled = 0;
      int cleared = 1;
      for (size_t i = 0; i < sizeof buf; i++) {
         if (i >= 8 && i < 24) {
            if (buf[i] != 0)
               cleared = 0;
         } else if (buf[i] != 0xA5) {
            spilled = 1;
         }
      }
      ck(cleared, "every byte of the range is zeroed");
      ck(!spilled, "...and not one byte outside it is touched");

      /* Zero length must touch nothing -- the loop bound is the only thing
       * standing between `ct_wipe(p, 0)` and a wipe of the whole address
       * space if it were ever written as a do/while. */
      memset(buf, 0x5A, sizeof buf);
      ct_wipe(buf, 0);
      int untouched = 1;
      for (size_t i = 0; i < sizeof buf; i++)
         if (buf[i] != 0x5A)
            untouched = 0;
      ck(untouched, "a zero-length wipe touches nothing");

      /* A null pointer is a caller that had nothing to clear, which happens
       * on the free path of a failed allocation. */
      ct_wipe(NULL, 32);
      ck(1, "a null pointer is not a crash");

      /* HOW THESE ACTUALLY DIE, recorded because it is weaker evidence than
       * a named assertion and should not be mistaken for one.
       *
       * Measured: a wipe running one byte long ABORTS the suite rather than
       * failing the guard-byte check -- ct_wipe is used on the whole
       * thread-local tls_conn in tls_bye, and one byte past that overruns it
       * into the stack protector. Removing the null guard SEGFAULTS. Both are
       * unambiguous failures, so the mutants are killed either way, but they
       * are killed by the process dying and not by anything here noticing. */
   }

   printf("== the transcript bound stops the handshake AT the overflow ==\n");
   {
      /* WHY THIS NEEDS A DOOR AT ALL: 8 kB of handshake is not something a
       * real client sends, so no end-to-end test reaches this path. It is
       * reached by a peer that means to, which is the whole reason the bound
       * exists.
       *
       * The defect was not the bound -- that was always there -- it was WHEN
       * it was read. transcript() set c->fatal and returned void, and the flag
       * was consulted at the far end of the handshake, so an overflowed
       * transcript still went through an ECDHE key exchange, an ECDSA
       * signature and the certificate write first. The signature is over a
       * hash of a transcript that is MISSING ITS TAIL -- not the one the peer
       * saw. */
      int fatal = -1;
      ck(tls_fault_transcript(0, 64, &fatal) == 1,
         "a message that fits is counted");
      ck(fatal == 0, "...and the connection is not marked fatal");

      /* EXACTLY FULL still fits: the bound is on what would NOT fit, and an
       * off-by-one here refuses a legal handshake. */
      fatal = -1;
      ck(tls_fault_transcript(8192 - 64, 64, &fatal) == 1,
         "a message that exactly fills the transcript is counted");
      ck(fatal == 0, "...and is not fatal either");

      /* ONE BYTE OVER is refused, AT the append. */
      fatal = -1;
      ck(tls_fault_transcript(8192 - 64, 65, &fatal) == 0,
         "one byte past the transcript is REFUSED");
      ck(fatal == 1, "...and the connection is marked fatal");

      fatal = -1;
      ck(tls_fault_transcript(8192, 1, &fatal) == 0,
         "and so is a single byte offered to a full transcript");
      ck(fatal == 1, "...also fatal");
   }

   if (fails) {
      /* THE COUNT IS PRINTED ON THIS PATH TOO. A failing run is exactly when
       * somebody needs to know how many cases actually ran -- a mutant that
       * stops a whole block from being reached shows up as a smaller number,
       * not as a different verdict. */
      printf("tlscrypttest: FAIL (%d assertions run)\n", checks);
      return 1;
   }
   printf("tlscrypttest: GCM, HMAC, HKDF, J-PAKE, the TLS 1.3 schedule, the "
          "ClientHello grammar and the ticket clock\n"
          "  agree with the specs (%d assertions)\n",
          checks);
   return 0;
}
