/* SPDX-License-Identifier: GPL-3.0
 * tls.c --- a TLS 1.3 server, only as much of one as this program needs
 * Copyright 2026 Jakob Kastelic
 *
 * About a thousand lines, where a general TLS library needs tens of
 * thousands. The difference is not cleverness, it is scope: a general library
 * must be able to negotiate anything, and this one negotiates exactly one
 * thing.
 *
 *     cipher suite   TLS_AES_128_GCM_SHA256   (0x1301)
 *     group          secp256r1                (0x0017)
 *     signature      ecdsa_secp256r1_sha256   (0x0403)
 *
 * That is not a compromise. TLS_AES_128_GCM_SHA256 is the one suite RFC 8446
 * requires every client to implement, so the negotiation cannot fail; and
 * offering one of each means no algorithm tables, no agility, and no code for
 * primitives nothing will ever pick. The arithmetic underneath was already
 * here for the pairing (lib/p256.c, lib/sha256.c, lib/aes.c); lib/gcm.c,
 * lib/hmac.c, lib/hkdf.c and lib/ecdsa.c add the rest, each checked against
 * the published vectors before any of it went near a socket.
 *
 * WHAT IS DELIBERATELY ABSENT: TLS 1.2 and below (refused, not negotiated
 * down), client certificates, renegotiation, 0-RTT early data, and X.509
 * parsing -- the certificate chain is read as PEM and sent as DER verbatim,
 * because a server does not need to understand its own certificate to present
 * it. Post-handshake KeyUpdate is NOT on that list -- see key_update: a
 * server that reads the message and drops it leaves a peer that rekeys on
 * schedule failing with a decryption error rather than a protocol one.
 *
 * WHAT IS PRESENT BEYOND THE MINIMUM: session resumption. A full handshake
 * costs this board roughly 170 ms of a single core against a 3 ms request, so
 * resumption is not a nicety, it is most of the page load. Tickets are
 * stateless (sealed under a key this process holds) and resumption is
 * psk_dhe_ke only, so a resumed session still has forward secrecy.
 */
#include "tls.h"
#include "ct.h"
#include "ecdsa.h"
#include "gcm.h"
#include "hkdf.h"
#include "hmac.h"
#include "p256.h"
#include "posix.h" /* the one boundary beyond ISO C -- see posix.h */
#include "rand.h"
#include "sha256.h"
#include "tlsint.h" /* the four arrays tlscred.c fills */
#include <errno.h>
#include <fcntl.h>
#include <poll.h> /* poll: each wait for room is bounded here */
#include <stdio.h>
#ifdef TLS_FAULTS
#include <stdlib.h> /* getenv, strtoull: the injected clock, and nothing else */
#endif
#include <string.h>
#include <sys/socket.h> /* recv: the read half; the write half is posix.h */
#include <time.h>
#include <unistd.h>

/* ---- the TLS 1.3 key schedule -------------------------------------------
 *
 * HKDF itself is a primitive and lives in lib/. What is here is the part
 * that is TLS and nothing else: the HkdfLabel structure of RFC 8446 7.1,
 * with its "tls13 " prefix and its length-prefixed context. A library
 * carrying this would be a TLS library, so it stays beside the protocol.
 */

/* RFC 8446 7.1 gives the structure and, with it, every field width there is:
 *
 *     struct {
 *         uint16 length = Length;
 *         opaque label<7..255> = "tls13 " + Label;
 *         opaque context<0..255> = Context;
 *     } HkdfLabel;
 *
 * AN ENCODER THAT HONOURS NONE OF THEM writes into a fixed 74-byte stack
 * buffer sized for a 32-byte label and a 32-byte context and copies whatever
 * lengths it is handed:
 *
 *   - `memcpy(info + k, label, ln)` with ln from strlen(), so a 40-character
 *     label ran off the end of `info`;
 *   - `info[k++] = (uint8_t)(6 + ln)`, so a 250-byte Label announced itself
 *     as a 0-byte one and the parser on the other side read the label bytes
 *     as the context length and then as context;
 *   - `info[k++] = (uint8_t)ctxn`, the same truncation for a 256-byte
 *     context;
 *   - `(uint8_t)(n >> 8)` and `(uint8_t)n` for the length field, with nothing
 *     checking n against 65535.
 *
 * All four were silent. Nobody derives a 250-byte label by accident, but the
 * shape of the failure is what matters: the derivation SUCCEEDS and produces
 * a key from a label that is not the one in the source, so the peer computing
 * the same schedule correctly disagrees and the handshake fails at the
 * Finished MAC with nothing in the log pointing here.
 *
 * The label vector's lower bound is real too: <7..255> means the vector
 * carries at least 7 bytes, and "tls13 " is 6 of them, so an empty Label
 * encodes a 6-byte vector that is not a legal HkdfLabel at all. */
#define TLS_LABEL_MAX 249 /* 255 - strlen("tls13 ") */
#define TLS_CTX_MAX   255

/* The local buffer is now big enough for the RFC's widest legal HkdfLabel
 * rather than for the labels this server happens to use, so the bounds
 * enforced below are the PROTOCOL's bounds and not this file's. That in turn
 * has to fit what hkdf_expand() will accept as `info`, or the encoder would
 * hand up a structure the primitive refuses -- checked here so the two
 * constants cannot drift apart in a later edit. */
_Static_assert(2 + 1 + 6 + TLS_LABEL_MAX + 1 + TLS_CTX_MAX <= HKDF_INFO_MAX,
               "the widest RFC 8446 HkdfLabel must fit hkdf_expand's info");

/* WHY `n` IS NOT CHECKED HERE. The length field is a uint16, but HKDF's own
 * output ceiling is 255*32 = 8160 (RFC 5869 2.3) and hkdf_expand() enforces
 * it before it writes anything, so no `n` that survives the derivation can
 * overflow the field. A second bound here would be one no test could ever
 * isolate -- it would be shadowed by hkdf_expand's -- so the invariant is
 * asserted at compile time rather than guarded at run time. */
_Static_assert(HKDF_L_MAX <= 0xFFFF,
               "HkdfLabel.length is a uint16; HKDF's L ceiling must fit it");

int hkdf_expand_label(const uint8_t secret[32], const char *label,
                      const uint8_t *ctx, size_t ctxn, uint8_t *out, size_t n)
{
   if (!secret || !label || !out || (!ctx && ctxn))
      return 0;
   size_t ln = strlen(label);
   if (ln < 1 || ln > TLS_LABEL_MAX) /* opaque label<7..255>, minus "tls13 " */
      return 0;
   if (ctxn > TLS_CTX_MAX) /* opaque context<0..255> */
      return 0;

   uint8_t info[2 + 1 + 6 + TLS_LABEL_MAX + 1 + TLS_CTX_MAX];
   size_t k  = 0;
   info[k++] = (uint8_t)(n >> 8);
   info[k++] = (uint8_t)n;
   info[k++] = (uint8_t)(6 + ln); /* <= 255 by the check above */
   memcpy(info + k, "tls13 ", 6);
   k += 6;
   memcpy(info + k, label, ln);
   k += ln;
   info[k++] = (uint8_t)ctxn; /* <= 255 by the check above */
   if (ctxn) {
      memcpy(info + k, ctx, ctxn);
      k += ctxn;
   }
   /* A refused expansion leaves `out` alone, so a caller that checks this
    * return sees a buffer it can still tell apart from a key. `info` is
    * local, so an n above HKDF's ceiling costs nothing but this scratch. */
   return hkdf_expand(secret, info, k, out, n) == HKDF_OK;
}

int derive_secret(const uint8_t secret[32], const char *label,
                  const uint8_t *transcript, size_t tn, uint8_t out[32])
{
   uint8_t th[32];
   sha256(transcript, tn, th);
   return hkdf_expand_label(secret, label, th, 32, out, 32);
}

/* The wire constants live in srv/tlsint.h: this file writes them into records
 * and srv/tlshello.c reads them out of a ClientHello, and one list is the only
 * way the two can agree about what 51 means. */

/* A change_cipher_spec is a valid 5-byte plaintext record carrying nothing,
 * and a peer can send them forever. Both loops that skip them are bounded by
 * this: unbounded, a stream of them held a worker without ever timing out or
 * advancing the handshake -- cheap for the peer, one of ten workers for us.
 * RFC 8446 permits them only for middlebox compatibility, so a real client
 * sends at most one. */
/* CCS_MAX, KEY_UPDATE_MAX, REC_MAX, TRANS_MAX and HRR_RANDOM are in
 * srv/tlsint.h: all three files of this module speak them. */
const uint8_t *g_cert; /* the chain, DER, as it will go on the wire */
size_t g_cert_n;
uint8_t g_key[32];        /* our EC private scalar */
uint8_t g_ticket_key[16]; /* seals resumption tickets */

/* This worker's connection slot. Called once per accepted connection (see
 * https.c); everything after that takes the pointer explicitly. */
static _Thread_local struct tls_conn conn_slot;

struct tls_conn *tls_conn_slot(void)
{
   return &conn_slot;
}

/* 1, or 0 with `out` untouched -- which matters more here than anywhere else
 * in this file: `out` is compared against the peer's Finished with ct_eq, and
 * a silently unwritten buffer would be compared as though it were a MAC. */
static int finished_mac(struct tls_conn *c, const uint8_t secret[32],
                        uint8_t out[32])
{
   uint8_t fk[32], th[32];
   if (!hkdf_expand_label(secret, "finished", NULL, 0, fk, 32))
      return 0;
   tls_rec_transcript_hash(c, th);
   hmac_sha256(fk, 32, th, 32, out);
   return 1;
}

/* ---- the handshake ------------------------------------------------------ */

/* ONE HANDSHAKE MESSAGE, ALREADY FRAMED, ACROSS AS MANY RECORDS AS IT TAKES.
 *
 * `hdr` is the message's own framing (its type, its length, and whatever
 * fixed fields precede the payload) and `body` is a payload this function
 * will not copy -- the certificate chain, which is up to TLS_CHAIN_MAX bytes
 * and lives for the life of the process. Everything else in this file builds
 * its message in one buffer and calls tls_rec_handshake; only the chain is
 * large enough to be worth not copying, and only the chain can exceed one
 * record.
 *
 * THE TRANSCRIPT SEES THE MESSAGE, NOT THE RECORDS. RFC 8446 hashes the
 * handshake byte stream, so it is fed hdr and body in order and knows nothing
 * about where the fragments fall -- which is also why the fragmentation is
 * free to change without breaking a peer's Finished. */
int tls_rec_handshake_split(struct tls_conn *c, const uint8_t *hdr, size_t hn,
                            const uint8_t *body, size_t bn)
{
   if (hn > REC_MAX)
      return 0;
   if (!tls_rec_transcript(c, hdr, hn) || !tls_rec_transcript(c, body, bn))
      return 0; /* the transcript is full: nothing more may be sent */
   /* The first record carries the framing and as much of the payload as fits
    * beside it; the rest go straight out of `body` with no copy at all. */
   uint8_t first[REC_MAX];
   size_t take = REC_MAX - hn;
   if (take > bn)
      take = bn;
   memcpy(first, hdr, hn);
   memcpy(first + hn, body, take);
   if (!tls_rec_send(c, REC_HANDSHAKE, first, hn + take))
      return 0;
   for (size_t off = take; off < bn;) {
      size_t k = bn - off;
      if (k > REC_MAX)
         k = REC_MAX;
      if (!tls_rec_send(c, REC_HANDSHAKE, body + off, k))
         return 0;
      off += k;
   }
   return 1;
}

int tls_rec_handshake(struct tls_conn *c, int type, const uint8_t *body,
                      size_t n)
{
   uint8_t msg[REC_MAX];
   if (n + 4 > sizeof msg)
      return 0;
   msg[0] = (uint8_t)type;
   put24(msg + 1, n);
   memcpy(msg + 4, body, n);
   if (!tls_rec_transcript(c, msg, n + 4))
      return 0; /* the transcript is full: nothing more may be sent */
   return tls_rec_send(c, REC_HANDSHAKE, msg, n + 4);
}

/* ServerHello, or a HelloRetryRequest if `hrr` -- they are the same message
 * with a different random, which is why they share this code. */
static int send_server_hello(struct tls_conn *c, const struct hello *h,
                             const uint8_t *share, int hrr, int psk_used)
{
   uint8_t b[512];
   size_t k = 0;
   put16(b + k, 0x0303);
   k += 2;
   if (hrr) {
      memcpy(b + k, HRR_RANDOM, 32);
   } else if (!tls_rec_rand(b + k, 32)) {
      return 0;
   }
   k += 32;
   b[k++] = (uint8_t)h->session_id_n;
   memcpy(b + k, h->session_id, h->session_id_n);
   k += h->session_id_n;
   put16(b + k, SUITE_AES128);
   k += 2;
   b[k++] = 0; /* legacy_compression_method */

   size_t extlen_at = k;
   k += 2;
   put16(b + k, EXT_SUPPORTED_VERSION);
   put16(b + k + 2, 2);
   put16(b + k + 4, 0x0304);
   k += 6;
   put16(b + k, EXT_KEY_SHARE);
   if (hrr) { /* a retry names the group it wants, and sends no share */
      put16(b + k + 2, 2);
      put16(b + k + 4, GROUP_P256);
      k += 6;
   } else {
      put16(b + k + 2, 2 + 2 + 65);
      put16(b + k + 4, GROUP_P256);
      put16(b + k + 6, 65);
      memcpy(b + k + 8, share, 65);
      k += 8 + 65;
   }
   if (psk_used) {
      put16(b + k, EXT_PRE_SHARED_KEY);
      put16(b + k + 2, 2);
      put16(b + k + 4, 0); /* we only ever offer identity 0 */
      k += 6;
   }
   put16(b + extlen_at, (unsigned)(k - extlen_at - 2));
   return tls_rec_handshake(c, HS_SERVER_HELLO, b, k);
}

/* DER-encode the ECDSA (r, s) pair the way TLS wants it. */
static size_t der_sig(const uint8_t r[32], const uint8_t s[32], uint8_t *out)
{
   uint8_t body[80];
   size_t k = 0;
   for (int half = 0; half < 2; half++) {
      const uint8_t *v = half ? s : r;
      size_t skip      = 0;
      while (skip < 31 && v[skip] == 0)
         skip++;
      int pad   = (v[skip] & 0x80) ? 1 : 0; /* keep it positive */
      body[k++] = 0x02;
      body[k++] = (uint8_t)(32 - skip + pad);
      if (pad)
         body[k++] = 0;
      memcpy(body + k, v + skip, 32 - skip);
      k += 32 - skip;
   }
   out[0] = 0x30;
   out[1] = (uint8_t)k;
   memcpy(out + 2, body, k);
   return k + 2;
}

static int send_cert_verify(struct tls_conn *c)
{
   /* RFC 8446 4.4.3: 64 spaces, a context string, a zero, then the hash. */
   uint8_t tbs[64 + 34 + 1 + 32];
   size_t k = 0;
   memset(tbs, 0x20, 64);
   k = 64;
   memcpy(tbs + k, "TLS 1.3, server CertificateVerify", 33);
   k += 33;
   tbs[k++] = 0;
   tls_rec_transcript_hash(c, tbs + k);
   k += 32;

   uint8_t h[32];
   sha256(tbs, k, h);
   uint8_t nonce[32], r[32], s[32];
   /* THIS IS THE OPERATION A STRANGER CAN FORCE, and the only place the
    * server's long-term private key is used. Declining to present a session
    * ticket makes the handshake full, and a full handshake signs here.
    *
    * lib/p256.h has the current state of that: the nonce's leading zeros no
    * longer show up in the timing, which is what a lattice attack on partial
    * nonce leakage wants; its Hamming weight still does. Nothing about this
    * call site changes that either way -- it is recorded here because this is
    * where a reader asks the question.
    *
    * p256_sc_rand, not tls_rec_rand(nonce, 32). The nonce is a scalar, so it
    * gets the one generator that rejection-samples [1, n-1] (lib/p256.h). This
    * site was the least exposed of the four: ecdsa_p256_sign has always refused
    * a zero k outright; what this removes is the 2^-32 bias of reducing a raw
    * draw into range. Refusing is the right answer and there is deliberately no
    * retry -- a nonce this server cannot draw is a handshake it declines, and
    * the client reconnects. */
   if (!p256_sc_rand(nonce) || !ecdsa_p256_sign(g_key, h, nonce, r, s))
      return 0;

   uint8_t b[128];
   put16(b, SIG_ECDSA256);
   size_t sn = der_sig(r, s, b + 4);
   put16(b + 2, (unsigned)sn);
   return tls_rec_handshake(c, HS_CERT_VERIFY, b, 4 + sn);
}

/* The Certificate message: one empty request context, then the chain.
 *
 * SENT WITHOUT EVER HOLDING IT TWICE. This copied the whole chain
 * into a 4 KiB stack buffer on every handshake and refused -- silently,
 * mid-handshake, with the server already listening -- anything larger. Both
 * halves of that were wrong: the copy is pointless (the chain is already
 * encoded, in memory, and immutable for the life of the process), and the
 * ceiling was the buffer's rather than the protocol's.
 *
 * A HANDSHAKE MESSAGE MAY SPAN RECORDS, which is what makes this possible at
 * all: TLS 1.3 caps a record's plaintext at 2^14 bytes (REC_MAX) and says
 * nothing about how many records one message occupies. So the 9 bytes of
 * framing and the chain go out as one logical message, fragmented at the
 * record layer, and the transcript sees the message rather than the
 * fragmentation -- which is exactly what the specification hashes.
 *
 * The framing, in order: the handshake header (type + 3-byte length), then
 * the message body's own two fields -- an empty certificate_request_context
 * and the 3-byte length of the cert_list. */
static int send_certificate(struct tls_conn *c)
{
   uint8_t hdr[8];
   size_t body = 1 + 3 + g_cert_n;
   hdr[0]      = HS_CERTIFICATE;
   put24(hdr + 1, body);
   hdr[4] = 0; /* certificate_request_context: empty */
   put24(hdr + 5, g_cert_n);
   return tls_rec_handshake_split(c, hdr, 8, g_cert, g_cert_n);
}

/* THE TRANSCRIPT BOUND, REACHABLE FROM A TEST.
 *
 * The overflow is 8 kB of handshake, and a real handshake is about 1.5 kB --
 * so driving it from a client means a peer that sends kilobytes of legal
 * handshake messages, which is exactly the attacker this bound exists for and
 * exactly what tlstest's real clients will not do. Without a door the abort
 * path is unreachable from any test, and an unreachable path is one that stops
 * working without anybody noticing.
 *
 * Fills the transcript to `pre` bytes, then offers `n` more, and answers what
 * tls_rec_transcript() said. The caller checks that the answer is a refusal AND
 * that c->fatal was raised, which are two different claims: returning 0 without
 * the flag would leave every other exit path believing the connection is well.
 */
#ifdef TLS_FAULTS
size_t tls_fault_transcript_cap(void)
{
   return sizeof(((struct tls_conn *)0)->trans);
}

int tls_fault_transcript(size_t pre, size_t n, int *fatal)
{
   struct tls_conn *c = tls_conn_slot();
   memset(c, 0, sizeof *c);
   static const uint8_t z[64] = {0};
   c->trans_n                 = pre > sizeof c->trans ? sizeof c->trans : pre;
   int ok                     = 1;
   size_t left                = n;
   while (left > 0 && ok) {
      size_t k = left > sizeof z ? sizeof z : left;
      ok       = tls_rec_transcript(c, z, k);
      left -= k;
   }
   if (fatal)
      *fatal = c->fatal;
   return ok;
}
#endif

/* Read one handshake message into `out`, skipping change_cipher_spec. */
static int read_handshake(struct tls_conn *c, uint8_t *out, size_t cap,
                          int *type, size_t *n, tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = tls_rec_read(c, giveup);
      if (t < 0)
         return 0;
      if (t == REC_CHANGE_CIPHER) {
         if (++skipped > CCS_MAX) /* see next_record: they are unbounded free */
            return 0;
         continue;
      }
      if (t != REC_HANDSHAKE)
         return 0;
      if (c->plain_n < 4)
         return 0;
      size_t len = get24(c->plain + 1);
      if (len + 4 > c->plain_n || len > cap)
         return 0;
      *type = c->plain[0];
      *n    = len;
      memcpy(out, c->plain + 4, len);
      /* the message as sent, header included */
      if (!tls_rec_transcript(c, c->plain, len + 4))
         return 0;
      return 1;
   }
}

int tls_handshake(struct tls_conn *c, int fd, tls_giveup_fn giveup)
{
   memset(c, 0, sizeof *c);
   c->fd = fd;
   /* The write side gets the same budget the read side is given. Set here
    * because the handshake itself writes -- a peer that stops reading during
    * the handshake stalls a worker exactly as one that stops reading during
    * the response does. */
   c->giveup = giveup;

   uint8_t body[REC_MAX];
   int type;
   size_t n;

   /* --- ClientHello. It arrives as a plaintext record. --- */
   int t = tls_rec_next(c, giveup);
   if (t != REC_HANDSHAKE || c->plain_n < 4 || c->plain[0] != HS_CLIENT_HELLO)
      return 0;
   size_t chn = get24(c->plain + 1);
   if (chn + 4 > c->plain_n)
      return 0;
   struct hello h;
   if (!parse_hello(c->plain + 4, chn, &h) || !h.has_tls13 || !h.suite_ok ||
       !h.sig_ok)
      return 0;

   /* A resumption offer is authenticated by its binder, which is computed
    * over the ClientHello UP TO the binder list -- so the transcript has to
    * be truncated at exactly that point before it can be checked. This is a
    * FIRST ClientHello, so nothing precedes it in the binder's transcript
    * (RFC 8446 4.2.11.2); binder_ok takes that prefix as an argument because
    * after a HelloRetryRequest it is not empty.
    *
    * AN OFFER WE WOULD SELECT AND CANNOT AUTHENTICATE IS FATAL. ticket_open
    * succeeding means this is a live ticket THIS process sealed, so it is a
    * PSK we would use; RFC 8446 4.2.11 then says the binder must validate or
    * the handshake is aborted. Nothing legitimate lands here -- a client that
    * holds the ticket holds the PSK and computes the MAC -- so the choices
    * are to refuse a peer that is guessing or to quietly hand it a full
    * handshake and let it guess again on the next connection. A ticket that
    * does not open at all is a different thing entirely (expired, or issued
    * by a previous run of this process) and still falls through to the full
    * handshake below. */
   uint8_t psk[32];
   int resumed = 0;
   if (h.psk_ident && h.binders && h.psk_dhe &&
       tls_tkt_open(h.psk_ident, h.psk_ident_n, psk)) {
      if (!tls_tkt_binder_ok(&h, c->plain, NULL, 0, psk))
         return 0;
      resumed = 1;
   }

   if (!tls_rec_transcript(c, c->plain, chn + 4))
      return 0;

   /* --- HelloRetryRequest, if the client offered no P-256 share. Browsers
    * lead with X25519, so this is the common path, not an edge case. --- */
   if (!h.peer_key) {
      /* RFC 8446 4.4.1: once a retry happens the transcript is NOT the two
       * ClientHellos in sequence. The first one is replaced by a synthetic
       * "message_hash" message carrying its hash, and everything after
       * follows that. Get this wrong and the handshake fails at the very
       * last step, when the Finished MACs disagree for no visible reason. */
      uint8_t th[32];
      tls_rec_transcript_hash(c, th);
      c->trans_n       = 0;
      uint8_t synth[4] = {254, 0, 0, 32}; /* message_hash, length 32 */
      if (!tls_rec_transcript(c, synth, 4) || !tls_rec_transcript(c, th, 32))
         return 0;

      if (!send_server_hello(c, &h, NULL, 1, 0)) /* appends the HRR itself */
         return 0;
      t = tls_rec_next(c, giveup);
      if (t != REC_HANDSHAKE || c->plain_n < 4 ||
          c->plain[0] != HS_CLIENT_HELLO)
         return 0;
      chn = get24(c->plain + 1);
      /* THE SECOND ClientHello IS CHECKED AS FULLY AS THE FIRST.
       *
       * parse_hello overwrites `h` wholesale, so every conclusion drawn from
       * CH1 is gone by this point -- but only peer_key was re-tested, and
       * has_tls13 / suite_ok / sig_ok were re-derived and then ignored. RFC
       * 8446 4.1.2 requires CH2 to be the same ClientHello but for the
       * permitted changes, and 4.1.4 requires the server to abort if the
       * retried hello no longer offers what it needs. A client could offer
       * TLS 1.3 and a suite we support in CH1 and neither in CH2. */
      if (chn + 4 > c->plain_n || !parse_hello(c->plain + 4, chn, &h) ||
          !h.has_tls13 || !h.suite_ok || !h.sig_ok || !h.peer_key)
         return 0;

      /* AND SO IS THE RESUMPTION OFFER -- THE WHOLE OF IT, AGAIN.
       *
       * NEITHER `resumed` NOR `psk` CARRIES OVER FROM ClientHello 1. The
       * tempting reasoning is that CH1's binder was verified, so the client
       * has already proved it holds the key -- and that does not survive
       * contact with what a binder is FOR. The MAC is over a transcript, and
       * a transcript is what ties the proof to one handshake: verified over
       * CH1 alone, the same binder authenticates any ClientHello 2 that
       * follows it, including one nobody who holds the PSK ever wrote.
       *
       * Concretely: ClientHello 2 arrives in the clear, so if nothing
       * downstream read its binder, anyone on the path could rewrite it
       * wholesale -- its key share, its extensions, its PSK offer, or delete
       * the pre_shared_key extension entirely -- and the server
       * still entered the resumed key schedule, still announced
       * selected_identity 0 against a list that may no longer have an entry
       * 0, and still skipped Certificate and CertificateVerify. A captured
       * ticket-bearing ClientHello (a browser leading with X25519 provokes
       * the retry, so this is the ordinary path and not a rare one) replayed
       * as CH1 committed this server to resuming for whatever CH2 the replayer
       * chose to send next. It could not read the result -- the key schedule
       * still folds in a PSK it does not have, so the client Finished fails --
       * but every step before that ran on an authentication that was never
       * performed for the hello being answered, which is precisely what RFC
       * 8446 4.2.11 forbids.
       *
       * So: parse the identities and binders again, and authenticate the
       * selected PSK against the COMPLETE required transcript, which after a
       * retry is message_hash(ClientHello1) || HelloRetryRequest ||
       * Truncate(ClientHello2) (RFC 8446 4.2.11.2 with 4.4.1's synthetic
       * message). c->trans holds exactly the first two of those right now --
       * the synthetic message_hash was written above and send_server_hello
       * appended the HelloRetryRequest -- and this must happen BEFORE CH2
       * joins it, or the prefix would contain the hello being truncated.
       *
       * A CH2 that offers no PSK we can open is not an error: the retry may
       * legitimately drop an offer, and the answer is the full,
       * certificate-authenticated handshake below. What is refused is a live
       * ticket with a binder that does not validate. */
      resumed = 0;                /* CH1's verdict decides nothing here */
      memset(psk, 0, sizeof psk); /* nor does CH1's key */
      if (h.psk_ident && h.binders && h.psk_dhe &&
          tls_tkt_open(h.psk_ident, h.psk_ident_n, psk)) {
         if (!tls_tkt_binder_ok(&h, c->plain, c->trans, c->trans_n, psk))
            return 0;
         resumed = 1;
      }

      if (!tls_rec_transcript(c, c->plain, chn + 4))
         return 0;
   }

   /* --- ECDHE. Our ephemeral key never leaves this function. ---
    *
    * EPHEMERAL is what makes the remaining timing leak in lib/p256.c tolerable
    * here, and it is the difference between this and the signature above. `d`
    * lives for one handshake and is never reused, so whatever a hundred timings
    * of this multiplication reveal about a hundred different scalars does not
    * accumulate into anything: there is no long-term secret behind them to
    * solve for. The attacker's own point P goes through p256_from_xy, which
    * refuses anything not on the curve, so it cannot steer the arithmetic off
    * it either -- and P-256 has prime order, so an on-curve affine point has no
    * small-subgroup trick available.
    *
    * THE SCALAR COMES FROM p256_sc_rand, not from a raw draw reduced mod n:
    * that is biased and it accepts zero (see lib/p256.h). A zero `d` here
    * makes Q the point at infinity -- so the key_share we advertise is
    * 04||0..0 -- and makes S the identity, so the ECDHE secret is a constant
    * every eavesdropper can compute and the whole handshake is unprotected. It
    * would not get that far in practice, because p256_to_xy(&S) below refuses
    * the infinite point before send_server_hello -- but that is a property of
    * two later lines rather than of the draw. p256_sc_rand refuses zero and
    * the biased tail at the source. The reduction stays only because `d`
    * needs limbs, and
    * on a value already below n it is the identity. */
   uint8_t priv[32], pub[65];
   struct u256 d;
   struct jpoint Q, P, S;
   if (!p256_sc_rand(priv))
      return 0;
   p256_sc_from_be(&d, priv);
   p256_mul_g(&Q, &d);
   p256_uncompressed(&Q, pub);
   if (!p256_from_xy(&P, h.peer_key + 1, h.peer_key + 33))
      return 0;
   p256_mul(&S, &d, &P);
   uint8_t sx[32], sy[32];
   if (!p256_to_xy(&S, sx, sy))
      return 0; /* the shared secret is the x coordinate */

   if (!send_server_hello(c, &h, pub, 0, resumed))
      return 0;

   /* --- the key schedule --- */
   uint8_t early[32], derived[32], hs[32], chs[32], shs[32];
   uint8_t zeros[32] = {0};
   hkdf_extract(NULL, 0, resumed ? psk : zeros, 32, early);
   /* EVERY STEP IS CHECKED, all the way down. Each label here is a literal
    * and each output length a constant, so none of these can fail today --
    * but the one thing worse than a handshake that dies here is one that
    * continues with an underived secret still holding stack bytes and
    * encrypts the response under it. */
   if (!derive_secret(early, "derived", (const uint8_t *)"", 0, derived))
      return 0;
   hkdf_extract(derived, 32, sx, 32, hs);
   if (!derive_secret(hs, "c hs traffic", c->trans, c->trans_n, chs) ||
       !derive_secret(hs, "s hs traffic", c->trans, c->trans_n, shs) ||
       !tls_rec_keys(shs, &c->wr) || !tls_rec_keys(chs, &c->rd))
      return 0;
   c->encrypted = 1;

   /* --- our half of the handshake, all of it encrypted --- */
   uint8_t ee[2] = {0, 0}; /* EncryptedExtensions: none */
   if (!tls_rec_handshake(c, HS_ENCRYPTED_EXT, ee, 2))
      return 0;
   if (!resumed) { /* a resumed session is already authenticated by the PSK */
      if (!send_certificate(c) || !send_cert_verify(c))
         return 0;
   }
   uint8_t fin[32];
   if (!finished_mac(c, shs, fin))
      return 0;
   if (!tls_rec_handshake(c, HS_FINISHED, fin, 32))
      return 0;

   /* --- master secret and the application keys --- */
   uint8_t derived2[32], master[32], cap_[32], sap[32];
   if (!derive_secret(hs, "derived", (const uint8_t *)"", 0, derived2))
      return 0;
   hkdf_extract(derived2, 32, zeros, 32, master);
   if (!derive_secret(master, "c ap traffic", c->trans, c->trans_n, cap_) ||
       !derive_secret(master, "s ap traffic", c->trans, c->trans_n, sap))
      return 0;
   memcpy(c->master, master, 32);

   /* --- the client's Finished, under the handshake key --- */
   uint8_t want[32];
   /* before the message joins the transcript. Checked first: an unwritten
    * `want` would go into ct_eq as though it were the MAC we expect. */
   if (!finished_mac(c, chs, want))
      return 0;
   if (!read_handshake(c, body, sizeof body, &type, &n, giveup) ||
       type != HS_FINISHED || n != 32 || !ct_eq(body, want, 32))
      return 0;

   if (!derive_secret(master, "res master", c->trans, c->trans_n,
                      c->res_master))
      return 0;

   /* --- from here the connection carries application data --- */
   if (!tls_rec_keys(sap, &c->wr) || !tls_rec_keys(cap_, &c->rd))
      return 0;
   /* The secrets themselves are kept, not just the keys expanded from them:
    * a KeyUpdate advances the SECRET (RFC 8446 7.2), so a connection that has
    * thrown these away cannot honour one. See key_update(). */
   memcpy(c->cts, cap_, 32);
   memcpy(c->sts, sap, 32);
   c->plain_n = c->plain_off = 0;

   /* A ticket, so the next connection can skip all of the above.
    *
    * NOT DISCARDED. A ticket we chose not to issue costs the next connection
    * a full handshake and nothing else; a ticket whose WRITE died part way
    * leaves a partial protected record in the stream, and from there the peer
    * cannot decrypt anything we send. Serving a request over that connection
    * is doing the work and throwing the answer away -- see enum ticket_fate.
    * The connection is closed instead, before https.c's prepare() can report
    * success and hand it to http_handle_conn. */
   if (tls_tkt_send(c) == TICKET_WRITE_FAILED)
      return 0;
   return !c->fatal;
}

/* A handshake message AFTER the handshake, sent without touching c->trans.
 * The transcript ended at the client's Finished and every secret derived from
 * it is already derived; appending to it now would change nothing except the
 * odds of running TRANS_MAX out and setting c->fatal on a connection that is
 * working perfectly. */
static int send_post_handshake(struct tls_conn *c, int type,
                               const uint8_t *body, size_t n)
{
   uint8_t msg[16];
   if (n + 4 > sizeof msg)
      return 0;
   msg[0] = (uint8_t)type;
   put24(msg + 1, n);
   memcpy(msg + 4, body, n);
   return tls_rec_send(c, REC_HANDSHAKE, msg, n + 4);
}

/* ---- KeyUpdate (RFC 8446 4.6.3) ----------------------------------------
 *
 * IGNORING ONE IS THE WORST OF THE THREE AVAILABLE ANSWERS. Clear the record
 * and read the next, and the peer has ALREADY switched its sending keys by
 * the time the message arrives -- 4.6.3 is explicit that the sender updates
 * after sending -- so the very next record it sends is encrypted under a key
 * this server has no way to reach. aes128_gcm_open then fails, read_record
 * returns -1, and the connection dies as an AEAD failure: indistinguishable,
 * in a log or a packet capture, from a forged record or a corrupted stream.
 * The peer did everything the RFC asks and got a decryption error for it, and
 * the one message that said what was about to happen was the message we threw
 * away. Silently ignoring a state change is not tolerance, it is a promise to
 * fail later with the evidence deleted.
 *
 * HANDLED, NOT REJECTED, and the choice is worth stating. The item this fixes
 * allowed either -- implement it, or send unexpected_message and close -- and
 * rejecting is genuinely defensible for a server whose connections last for a
 * handful of requests and never come near a sequence number worth rotating.
 * It is not what is done here, for three reasons:
 *
 *   - The peer is CONFORMING. A KeyUpdate is a legal thing to send at any
 *     time after the handshake (4.6.3), and openssl s_client sends one on a
 *     keystroke. Killing a correct client's connection to avoid twelve lines
 *     of key schedule is a worse deal than it looks: it fails closed for the
 *     one peer that did everything right.
 *   - The update itself is small and already-built machinery.
 *     hkdf_expand_label and traffic_keys are here, pinned against RFC 8448,
 *     and the derivation is one label:
 *
 *         application_traffic_secret_N+1 =
 *             HKDF-Expand-Label(application_traffic_secret_N,
 *                               "traffic upd", "", Hash.length)   (7.2)
 *
 *     with the record sequence number reset to zero at the key change (5.3),
 *     which traffic_keys already does for every other key it derives.
 *   - It is EXECUTABLE-TESTABLE either way, and the handling half in both
 *     directions at once: a real client that sends a KeyUpdate asking for one
 *     back and then makes a request shows a wrong read-side update as a lost
 *     request and a wrong write-side update as a lost response.
 *
 * `request` is the peer's request_update. update_requested obliges us to send
 * a KeyUpdate of our own before our next Application Data record (4.6.3);
 * ours always says update_not_requested, because two peers that each ask for
 * one back rekey at each other for as long as the connection lasts.
 *
 * ORDER MATTERS TWICE HERE. Our KeyUpdate goes out under the CURRENT write
 * keys and only then do we advance them -- the peer is still decrypting with
 * the previous ones until it has read the message telling it not to. And the
 * read
 * side is advanced immediately, because the peer switched before we ever saw
 * the message. */
static int key_update(struct tls_conn *c, int request)
{
   if (++c->key_updates > KEY_UPDATE_MAX)
      return 0;
   uint8_t next[32];
   if (request) {
      uint8_t reply = 0; /* update_not_requested */
      if (!send_post_handshake(c, HS_KEY_UPDATE, &reply, 1))
         return 0;
      if (!hkdf_expand_label(c->sts, "traffic upd", NULL, 0, next, 32))
         return 0;
      memcpy(c->sts, next, 32);
      if (!tls_rec_keys(c->sts, &c->wr))
         return 0;
   }
   if (!hkdf_expand_label(c->cts, "traffic upd", NULL, 0, next, 32))
      return 0;
   memcpy(c->cts, next, 32);
   return tls_rec_keys(c->cts, &c->rd);
}

/* A post-handshake handshake record. 1 to keep reading, 0 to close -- with an
 * alert already sent, so the peer learns which message ended it.
 *
 * ONE MESSAGE PER RECORD, and exactly filling it. Coalescing post-handshake
 * messages is legal and nothing does it; accepting a record with a message
 * and a tail would mean either parsing a stream here or silently dropping the
 * tail, and dropping the tail is the habit this whole function exists to
 * break. */
static int post_handshake(struct tls_conn *c)
{
   if (c->plain_n < 4 || get24(c->plain + 1) + 4 != c->plain_n) {
      tls_rec_alert(c, ALERT_UNEXPECTED_MSG);
      return 0;
   }
   if (c->plain[0] != HS_KEY_UPDATE || c->plain_n != 5) {
      /* A NewSessionTicket from a client, a post-handshake Certificate for
       * the client authentication this server does not do, a second
       * ClientHello attempting renegotiation that TLS 1.3 does not have: all
       * of them are messages we will not act on, and every one of them leaves
       * the peer believing we did. RFC 8446 6.2 has the answer for a message
       * that is not allowed here. */
      tls_rec_alert(c, ALERT_UNEXPECTED_MSG);
      return 0;
   }
   if (c->plain[4] > 1) { /* KeyUpdateRequest is an enum of exactly two */
      tls_rec_alert(c, ALERT_ILLEGAL_PARAMETER);
      return 0;
   }
   if (!key_update(c, c->plain[4] == 1)) {
      tls_rec_alert(c,
                    ALERT_UNEXPECTED_MSG); /* past KEY_UPDATE_MAX, or no key */
      return 0;
   }
   c->plain_n = c->plain_off = 0;
   return 1;
}

ssize_t tls_recv(struct tls_conn *c, void *buf, size_t n, tls_giveup_fn giveup)
{
   for (;;) {
      if (c->plain_off < c->plain_n) {
         size_t take = c->plain_n - c->plain_off;
         if (take > n)
            take = n;
         memcpy(buf, c->plain + c->plain_off, take);
         c->plain_off += take;
         return (ssize_t)take;
      }
      int t = tls_rec_read(c, giveup);
      if (t < 0)
         return -1;
      if (t == REC_APPDATA)
         continue; /* the loop above hands it back */
      if (t == REC_ALERT)
         return 0; /* close_notify, or anything else: we are done */
      if (t == REC_HANDSHAKE) {
         if (!post_handshake(c))
            return -1;
         continue;
      }
      /* EVERYTHING ELSE IS REFUSED, change_cipher_spec included. RFC 8446 5.1
       * permits an unencrypted CCS only up to the point where the peer's
       * Finished has been received, and requires an unexpected_message alert
       * for one after that -- and the handshake is long over here. Skipping
       * them instead is also an unbounded free loop: a five-byte record a
       * peer can send forever, each costing us a read. */
      tls_rec_alert(c, ALERT_UNEXPECTED_MSG);
      return -1;
   }
}

ssize_t tls_send(struct tls_conn *c, const void *buf, size_t n,
                 tls_giveup_fn giveup)
{
   /* Honoured now, rather than discarded. tls.h promises this function
    * "moves all n bytes or fails"; without a budget it could do neither. */
   if (giveup != NULL)
      c->giveup = giveup;
   const uint8_t *p = buf;
   size_t sent      = 0;
   while (sent < n) {
      size_t take = n - sent;
      if (take > REC_MAX - 64)
         take = REC_MAX - 64;
      if (!tls_rec_send(c, REC_APPDATA, p + sent, take))
         return -1;
      sent += take;
   }
   return (ssize_t)n;
}

int tls_pending(struct tls_conn *c)
{
   return c->plain_off < c->plain_n;
}

void tls_bye(struct tls_conn *c)
{
   uint8_t alert[2] = {1, 0}; /* warning, close_notify */
   if (c->encrypted)
      (void)tls_rec_send(c, REC_ALERT, alert, 2);
   /* AND THE SECRETS GO NOW, not when the next connection starts.
    *
    * The slot is _Thread_local and reused: tls_handshake clears it on the way
    * IN, so without this the finished connection's application traffic
    * secrets, its resumption master secret and its handshake transcript sat
    * in the worker's slot for as long as that worker was idle -- which on a
    * quiet board is indefinitely, and is exactly when a process image is most
    * likely to be dumped or swapped.
    *
    * ct_wipe rather than memset because this is the shape a compiler is
    * entitled to delete: the struct is not read again on any path it can see
    * -- tls_bye is the last thing http.c's transport calls -- so a plain
    * clear here is a dead store over secret material. */
   ct_wipe(c, sizeof *c);
}
