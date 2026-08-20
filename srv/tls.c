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
 * it. Post-handshake KeyUpdate used to be on that list; it is handled now
 * (see key_update), because "absent" meant the message was read and dropped,
 * and a peer that rekeys on schedule then fails with a decryption error
 * rather than a protocol one.
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
#include "rand.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h> /* poll: each wait for room is bounded here */
#include <stdio.h>
#ifdef TLS_FAULTS
#include <stdlib.h> /* getenv, strtoull: the injected clock, and nothing else */
#endif
#include <string.h>
#include <sys/socket.h> /* send + MSG_DONTWAIT: no reliance on SO_SNDTIMEO */
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
 * THE OLD ENCODER HONOURED NONE OF THEM. It wrote into a fixed 74-byte stack
 * buffer sized for a 32-byte label and a 32-byte context and copied whatever
 * lengths it was handed:
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
 * asserted at compile time instead of guarded at run time. */
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

/* ---- wire constants ---------------------------------------------------- */
#define REC_CHANGE_CIPHER 20
#define REC_ALERT         21
#define REC_HANDSHAKE     22
#define REC_APPDATA       23

#define HS_CLIENT_HELLO  1
#define HS_SERVER_HELLO  2
#define HS_NEW_TICKET    4
#define HS_ENCRYPTED_EXT 8
#define HS_CERTIFICATE   11
#define HS_CERT_VERIFY   15
#define HS_FINISHED      20
#define HS_KEY_UPDATE    24

/* Alerts we send. RFC 8446 6.2: everything but close_notify and
 * user_canceled is fatal, and a fatal alert ends the connection -- which is
 * the point of sending one at all. A peer that gets an alert knows the
 * connection is over and why; a peer that gets silence has to guess, and
 * guesses wrong in the direction of "the network glitched, retry". */
#define ALERT_FATAL             2
#define ALERT_UNEXPECTED_MSG    10
#define ALERT_ILLEGAL_PARAMETER 47

#define EXT_SERVER_NAME       0
#define EXT_SUPPORTED_GROUPS  10
#define EXT_SIG_ALGS          13
#define EXT_ALPN              16
#define EXT_PRE_SHARED_KEY    41
#define EXT_SUPPORTED_VERSION 43
#define EXT_PSK_MODES         45
#define EXT_KEY_SHARE         51

#define GROUP_P256   0x0017
#define SUITE_AES128 0x1301
#define SIG_ECDSA256 0x0403

/* A change_cipher_spec is a valid 5-byte plaintext record carrying nothing,
 * and a peer can send them forever. Both loops that skip them are bounded by
 * this: unbounded, a stream of them held a worker without ever timing out or
 * advancing the handshake -- cheap for the peer, one of ten workers for us.
 * RFC 8446 permits them only for middlebox compatibility, so a real client
 * sends at most one. */
#define CCS_MAX 8

/* KeyUpdates per connection. Same reasoning as CCS_MAX and the same shape of
 * abuse: a KeyUpdate is five bytes on the wire and costs us two HKDF
 * expansions plus, when it asks for one back, a record of our own -- so an
 * unbounded stream of them is a cheap way to make one of ten workers do
 * nothing but rekey. A real peer rekeys when a sequence number is running out
 * (RFC 8446 5.5), which on connections that live for a handful of requests is
 * never; the ones seen in practice are the one or two an operator triggers by
 * hand. Thirty-two is far past any of that and far short of free. */
#define KEY_UPDATE_MAX 32

#define REC_MAX   16384
#define TRANS_MAX 8192 /* the whole handshake transcript; ours is ~1.5 kB */

/* HelloRetryRequest is a ServerHello whose random is this exact value -- the
 * SHA-256 of "HelloRetryRequest", fixed by RFC 8446 4.1.3. */
static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
    0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
    0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

/* ---- the shared half: certificate, key, ticket key --------------------- */

static uint8_t g_cert[4096]; /* the chain, DER, as it will go on the wire */
static size_t g_cert_n;
static uint8_t g_key[32];        /* our EC private scalar */
static uint8_t g_ticket_key[16]; /* seals resumption tickets */

/* ---- the per-connection half ------------------------------------------- */

struct keys {
   uint8_t key[16];
   uint8_t iv[12];
   uint64_t seq;
};

/* THE CONNECTION, named and passed.
 *
 * This was an anonymous `_Thread_local struct` called `C`, and every function
 * below reached into it by that name -- so "the connection this thread last
 * handshook" was a fact no signature stated and no caller could choose. The
 * header said as much in prose ("Read and write the connection this thread
 * last handshook"), which is the shape of documentation that exists because
 * the types do not say it.
 *
 * The STORAGE is still one per worker and reused across connections: this
 * runs on a 56 MB board and the struct is several kilobytes, so a malloc per
 * connection would be a real cost for no benefit. What changed is that the
 * pointer is now an argument -- tls.c's internals all name the connection
 * they work on, and the one remaining ambient lookup is tls_conn_slot(),
 * called once when a connection is accepted. */
struct tls_conn {
   int fd;
   uint8_t trans[TRANS_MAX]; /* handshake transcript, for the hashes */
   size_t trans_n;
   struct keys rd, wr;
   int encrypted;      /* records are protected from here on */
   uint8_t master[32]; /* master secret, for the ticket PSK */
   uint8_t res_master[32];
   /* THE APPLICATION TRAFFIC SECRETS, KEPT PAST THE HANDSHAKE.
    *
    * `rd`/`wr` above hold the key and IV in use; these hold the secret each
    * was expanded from, which is the only thing a KeyUpdate can be applied to
    * -- RFC 8446 7.2 derives the next secret from the current SECRET, not
    * from the key. Without them a KeyUpdate cannot be honoured at all, which
    * is exactly why this file used to drop the message on the floor. */
   uint8_t cts[32], sts[32]; /* client_/server_application_traffic_secret_N */
   unsigned key_updates;     /* bounded by KEY_UPDATE_MAX */
   /* one buffered record's worth of plaintext, and how much is unread */
   uint8_t plain[REC_MAX + 256];
   size_t plain_n, plain_off;
   uint8_t inbuf[REC_MAX + 512];
   int fatal;
   /* The deadline, for the WRITE side.
    *
    * The read side takes it as a parameter, which works because every reader
    * is reached from a handful of entry points. Writing is not like that: a
    * record is emitted from a dozen places in the handshake, and threading a
    * giveup through all of them would put the argument everywhere and the
    * check nowhere. It belongs to the connection, and the connection already
    * has a thread-local home. Set once per connection; read by full_write. */
   tls_giveup_fn giveup;
};

/* This worker's connection slot. Called once per accepted connection (see
 * https.c); everything after that takes the pointer explicitly. */
static _Thread_local struct tls_conn conn_slot;

struct tls_conn *tls_conn_slot(void)
{
   return &conn_slot;
}

/* ---- little helpers ---------------------------------------------------- */

static void put16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static void put24(uint8_t *p, size_t v)
{
   p[0] = (uint8_t)(v >> 16);
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)v;
}

static unsigned get16(const uint8_t *p)
{
   return ((unsigned)p[0] << 8) | p[1];
}

static size_t get24(const uint8_t *p)
{
   return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2];
}

/* lib/rand.c's, with the same contract: 1 only if every byte is real
 * entropy. This file's callers already treat 0 as fatal for the handshake --
 * an ECDHE scalar or a ticket nonce from stack garbage is the whole key. */
static int rnd(uint8_t *out, size_t n)
{
   return rand_bytes(out, n);
}

/* Everything hashed into the transcript goes through here, so no message can
 * be sent or accepted without being counted.
 *
 * 1 = counted, 0 = IT DID NOT FIT, and the caller must stop.
 *
 * It used to return void and set c->fatal, which was read at the far end of
 * the handshake -- so a transcript that had already overflowed carried on
 * through an ECDHE key exchange, an ECDSA signature over a hash of a
 * transcript missing its tail, and the certificate write, before anything
 * looked. Every one of those is attacker-triggered work done on a connection
 * that is already finished, and the signature is over a transcript that is not
 * the one the peer saw.
 *
 * c->fatal is still set, because the connection IS dead and every other exit
 * path reads it; what changed is that the caller learns immediately rather
 * than eventually. */
static int transcript(struct tls_conn *c, const uint8_t *p, size_t n)
{
   if (c->trans_n + n > sizeof c->trans) {
      c->fatal = 1;
      return 0;
   }
   memcpy(c->trans + c->trans_n, p, n);
   c->trans_n += n;
   return 1;
}

static void transcript_hash(struct tls_conn *c, uint8_t out[32])
{
   sha256(c->trans, c->trans_n, out);
}

/* ---- reading and writing records --------------------------------------- */

/* A read or write that failed: is it worth another attempt, or is this socket
 * finished?
 *
 * ONLY the timeout and the interrupt are worth retrying. full_write used to
 * retry on EVERY error, reasoning that the socket carries SO_SNDTIMEO so a
 * failure must be that timeout -- true right up until the peer sends a reset.
 * SIGPIPE is ignored process-wide (srv/http.c), so from then on write()
 * returns -1/EPIPE forever, `sent` never advances, and the loop never ends. A
 * stranger could complete a handshake, reset the connection, and pin a pool
 * worker at 100% CPU for the life of the process -- tls_bye() writes the close
 * alert through here, so merely hanging up was enough. Ten sockets is every
 * worker on a one-core board, permanently. srv/test/tlstest.sh pins it. */
static int retry_after(int err)
{
   return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

/* Read exactly n bytes, or give up.
 *
 * THE DEADLINE IS CHECKED ON EVERY PASS, not only after a read fails. It used
 * to be consulted solely on the `r < 0` branch, on the assumption that waiting
 * only happens when a read times out. A peer that sends ONE BYTE at a time,
 * just often enough to keep the socket readable, never takes that branch: every
 * read succeeds, `got` crawls, and the whole HTTPS_DEADLINE_S budget is never
 * evaluated. The connection then holds a pool worker for as long as the peer
 * cares to keep dribbling -- ten of them is every worker on the board, and
 * unlike a spinning loop this one is invisible, because it costs no CPU.
 *
 * srv/http.c's plain-HTTP read loop has always checked its deadline every
 * iteration. This is that property, restored to the TLS path. */
static int full_read(struct tls_conn *c, uint8_t *p, size_t n,
                     tls_giveup_fn giveup)
{
   size_t got = 0;
   while (got < n) {
      if (giveup != NULL && giveup())
         return 0; /* out of time, or somebody else is waiting for a worker */
      errno  = 0;
      long r = read(c->fd, p + got, n - got);
      if (r > 0) {
         got += (size_t)r;
         continue;
      }
      /* Same rule as full_write: only a timeout or a signal is worth another
       * attempt. Anything else -- ECONNRESET, EBADF -- will not improve. */
      if (r < 0 && retry_after(errno))
         continue;
      return 0; /* r == 0 is a clean end of stream, mid-record */
   }
   return 1;
}

/* Write exactly n bytes, or give up -- AND THE DEADLINE IS CHECKED ON EVERY
 * PASS, for the same reason full_read's is.
 *
 * retry_after fixed the loud half of this: a reset peer no longer spins a
 * worker at 100% CPU. The quiet half survived. The socket carries a 1-second
 * SO_SNDTIMEO, so a peer that completes a handshake, asks for a page larger
 * than its receive window and then simply STOPS READING makes write() return
 * -1/EAGAIN once a second, forever, with no budget consulted -- a worker held
 * for as long as the peer cares to hold it, at no CPU cost at all. Ten of them
 * is every worker on a one-core board.
 *
 * It was worse than that, because web_route holds the page mutex across the
 * response write, so ONE such connection stalled every page for every user.
 *
 * srv/test/tlstest.sh missed it by asserting on CPU percentage, which a
 * zero-window stall does not move -- the very property the comment above
 * full_read names when describing the dribble attack from the other
 * direction. The write-side case asserts on WORKER AVAILABILITY instead.
 *
 * AND THE WAIT IS BOUNDED HERE, not by a socket option set somewhere else.
 * The budget above is only consulted when write(2) RETURNS, and write(2) on a
 * blocking socket returns to a stalled peer only because accepted sockets
 * carry a one-second SO_SNDTIMEO. That made a deadline out of an option: on
 * any fd that had not been through http_accept_setup the call would block in
 * the kernel and the budget would never be looked at again. MSG_DONTWAIT plus
 * a poll of at most one slice keeps the same behaviour without depending on
 * anything the fd was configured with. */
#define TLS_WRITE_SLICE_MS 200

static int full_write(struct tls_conn *c, const uint8_t *p, size_t n)
{
   size_t sent = 0;
   while (sent < n) {
      if (c->giveup != NULL && c->giveup())
         return 0; /* out of time: the response is owed, but not forever */
      errno  = 0;
      long r = send(c->fd, p + sent, n - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (r > 0) {
         sent += (size_t)r;
         continue;
      }
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         /* Wait for room for ONE SLICE, then look at the budget again. The
          * slice is what makes the loop cheap; the budget is what ends it. */
         struct pollfd pf = {.fd = c->fd, .events = POLLOUT, .revents = 0};
         (void)poll(&pf, 1, TLS_WRITE_SLICE_MS);
         continue;
      }
      if (r < 0 && errno == EINTR)
         continue; /* a signal: the peer is still there */
      return 0;    /* EPIPE, ECONNRESET, EBADF, or a zero-length write */
   }
   return 1;
}

/* The AEAD nonce: the static IV with the sequence number XORed into its tail
 * (RFC 8446 5.3). Every record must use a different one, which is why the
 * sequence number is never reset except when the keys change. */
static void nonce_of(const struct keys *k, uint8_t out[12])
{
   memcpy(out, k->iv, 12);
   for (int i = 0; i < 8; i++)
      out[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

static int send_record(struct tls_conn *c, int type, const uint8_t *body,
                       size_t n)
{
   uint8_t rec[REC_MAX + 512];
   if (!c->encrypted) {
      rec[0] = (uint8_t)type;
      put16(rec + 1, 0x0303);
      put16(rec + 3, (unsigned)n);
      memcpy(rec + 5, body, n);
      return full_write(c, rec, 5 + n);
   }
   /* Protected: the real type goes at the END of the plaintext and the outer
    * type is always application_data, so a watcher cannot tell a handshake
    * message from a page. */
   size_t inner = n + 1;
   rec[0]       = REC_APPDATA;
   put16(rec + 1, 0x0303);
   put16(rec + 3, (unsigned)(inner + 16));
   uint8_t pt[REC_MAX + 1];
   memcpy(pt, body, n);
   pt[n] = (uint8_t)type;
   uint8_t nonce[12];
   nonce_of(&c->wr, nonce);
   /* CHECKED, though it cannot refuse here. aes128_gcm_seal reports a
    * forbidden length now (item 64: past 2^32-2 blocks the counter wrapped to
    * 1 -- which is J0, the tag mask -- so guessing sixteen plaintext bytes
    * yielded the tag key for that key and IV, and one block later the
    * keystream simply repeated). A record is at most REC_MAX + 1 bytes, far
    * under the 2^36-32 limit, so this branch is unreachable. It is written
    * anyway because "unreachable" is a property of REC_MAX above it, not of
    * this call, and a sealer whose refusal is discarded would transmit an
    * unencrypted buffer. 0 is this file's failure answer -- full_write uses
    * it too. */
   if (aes128_gcm_seal(c->wr.key, nonce, rec, 5, pt, inner, rec + 5,
                       rec + 5 + inner) != GCM_OK)
      return 0;
   c->wr.seq++;
   return full_write(c, rec, 5 + inner + 16);
}

/* Read one record into c->plain. Returns its content type, or -1. */
static int read_record(struct tls_conn *c, tls_giveup_fn giveup)
{
   uint8_t hdr[5];
   if (!full_read(c, hdr, 5, giveup))
      return -1;
   size_t n = get16(hdr + 3);
   if (n > REC_MAX + 256)
      return -1;
   if (!full_read(c, c->inbuf, n, giveup))
      return -1;

   if (!c->encrypted) {
      memcpy(c->plain, c->inbuf, n);
      c->plain_n   = n;
      c->plain_off = 0;
      return hdr[0];
   }
   /* falls through to the protected path */
   if (hdr[0] == REC_CHANGE_CIPHER)
      return REC_CHANGE_CIPHER; /* middlebox compatibility: ignored */
   if (n < 17)
      return -1;
   uint8_t nonce[12];
   nonce_of(&c->rd, nonce);
   if (!aes128_gcm_open(c->rd.key, nonce, hdr, 5, c->inbuf, n - 16,
                        c->inbuf + n - 16, c->plain))
      return -1;
   c->rd.seq++;
   size_t m = n - 16;
   while (m && c->plain[m - 1] == 0) /* strip the padding */
      m--;
   if (!m)
      return -1;
   c->plain_n   = m - 1;
   c->plain_off = 0;
   return c->plain[m - 1]; /* the real content type */
}

/* ---- the key schedule -------------------------------------------------- */

/* 1, or 0 with the key set NOT usable. The sequence number is only reset once
 * both halves are derived, so a refusal cannot leave a record being sent under
 * a half-written key at seq 0. Every caller aborts the handshake. */
static int traffic_keys(const uint8_t secret[32], struct keys *k)
{
   if (!hkdf_expand_label(secret, "key", NULL, 0, k->key, 16) ||
       !hkdf_expand_label(secret, "iv", NULL, 0, k->iv, 12))
      return 0;
   k->seq = 0;
   return 1;
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
   transcript_hash(c, th);
   hmac_sha256(fk, 32, th, 32, out);
   return 1;
}

/* ---- PEM ---------------------------------------------------------------- */

static int b64val(int c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   return -1;
}

/* Decode every base64 body between -----BEGIN/END----- in `pem`, appending
 * each as DER. `each` is called with one block at a time. */
/* `incomplete` (may be NULL) is set when a block STARTS and does not finish.
 *
 * This used to `break` out of the loop on a "-----BEGIN" with no matching
 * "-----END", and the caller then took whatever blocks had already decoded as
 * the whole file. That is precisely the shape of a truncated credential -- a
 * copy interrupted, a partial write, a file cut by a full disk -- and it is the
 * case where guessing is least acceptable: the server comes up presenting the
 * certificates that happened to be complete. Saying so lets the caller refuse.
 * A file with no BEGIN at all is a different answer (*nblocks == 0), already
 * handled by the caller. */
static size_t pem_decode(const char *pem, uint8_t *out, size_t cap,
                         size_t *starts, size_t *lens, size_t maxblocks,
                         size_t *nblocks, int *incomplete)
{
   size_t total = 0;
   *nblocks     = 0;
   if (incomplete)
      *incomplete = 0;
   const char *p = pem;
   while ((p = strstr(p, "-----BEGIN")) != NULL) {
      const char *nl = strchr(p, '\n');
      if (!nl) {
         if (incomplete)
            *incomplete = 1; /* a header with no end of line: cut mid-header */
         break;
      }
      const char *end = strstr(nl, "-----END");
      if (!end) {
         if (incomplete)
            *incomplete = 1; /* a block that begins and never ends */
         break;
      }
      size_t start = total;
      uint32_t acc = 0;
      int bits     = 0;
      for (const char *q = nl; q < end; q++) {
         int v = b64val((unsigned char)*q);
         if (v < 0)
            continue;
         acc = (acc << 6) | (unsigned)v;
         bits += 6;
         if (bits >= 8) {
            bits -= 8;
            if (total < cap)
               out[total++] = (uint8_t)(acc >> bits);
         }
      }
      if (*nblocks < maxblocks) {
         starts[*nblocks] = start;
         lens[*nblocks]   = total - start;
         (*nblocks)++;
      }
      p = end + 1;
   }
   return total;
}

/* Why a credential file could not be read WHOLE. Separate answers, because
 * the operator's next action differs for each and "cannot read" covered all of
 * them. */
enum slurp_err {
   SLURP_OK = 0,
   SLURP_OPEN,   /* not there, or not permitted */
   SLURP_READ,   /* an I/O error part way through */
   SLURP_CLOSE,  /* close reported a failure -- rare on a read, still checked */
   SLURP_TOOBIG, /* did not reach EOF within the ceiling */
   SLURP_EMPTY   /* zero bytes: a file exists where a credential should be */
};

/* THE WHOLE FILE, OR NOTHING.
 *
 * This did ONE read into a 16 KiB buffer, took whatever came back, and NUL-
 * terminated it. Two failures, both silent, both intermittent:
 *
 *   - A SHORT READ IS LEGAL. read(2) may return fewer bytes than asked for
 *     with no error at all, and on a regular file it usually does not -- which
 *     is worse than if it always did, because the failure appears once in a
 *     while and looks like a corrupt certificate. The server then refuses to
 *     start, or starts with a chain missing its tail, depending on where the
 *     read stopped, and the file on disk is perfectly good.
 *   - A FILE LARGER THAN THE BUFFER WAS SILENTLY PREFIX-TRUNCATED. Not
 *     detected, not reported: a chain with several intermediates parsed to
 *     however many certificates fit, and the server presented a partial chain
 *     that clients cannot build a path from.
 *
 * So: loop to EOF, and require that EOF actually arrive within the ceiling
 * rather than assuming a full buffer is a whole file. `why` (may be NULL) says
 * which of those happened, so the diagnostic names the operator's next move. */
static char *slurp(const char *path, size_t *n, enum slurp_err *why)
{
   static char buf[16384];
   const size_t cap = sizeof buf - 1; /* room for the terminator */
   if (why)
      *why = SLURP_OK;
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
      if (why)
         *why = SLURP_OPEN;
      return NULL;
   }
   size_t off = 0;
   int eof    = 0;
   while (off < cap) {
      long r = read(fd, buf + off, cap - off);
      if (r < 0) {
         /* A read cut short by a signal has moved nothing. Treated as an I/O
          * error it would refuse a good file, which is the intermittent
          * failure this function exists to remove. */
         if (errno == EINTR)
            continue;
         close(fd);
         if (why)
            *why = SLURP_READ;
         return NULL;
      }
      if (r == 0) {
         eof = 1;
         break;
      }
      off += (size_t)r;
   }
   /* A FULL BUFFER IS NOT EOF. One more byte is asked for: if it arrives, the
    * file is bigger than this server will read, and the prefix already in the
    * buffer is not the credential -- it is the beginning of one. */
   if (!eof) {
      for (;;) {
         char probe;
         long r = read(fd, &probe, 1);
         if (r < 0 && errno == EINTR)
            continue;
         if (r > 0) {
            close(fd);
            if (why)
               *why = SLURP_TOOBIG;
            return NULL;
         }
         if (r < 0) {
            close(fd);
            if (why)
               *why = SLURP_READ;
            return NULL;
         }
         break; /* r == 0: the file ended exactly at the ceiling */
      }
   }
   /* CHECKED, even on a read handle. It carries no buffered data to lose, but
    * a failure here says the descriptor was not what we thought it was, and
    * this is a credential. */
   if (close(fd) != 0) {
      if (why)
         *why = SLURP_CLOSE;
      return NULL;
   }
   if (off == 0) {
      if (why)
         *why = SLURP_EMPTY;
      return NULL;
   }
   buf[off] = '\0';
   *n       = off;
   return buf;
}

/* One sentence naming what to do about it. */
static const char *slurp_why(enum slurp_err e, size_t cap)
{
   (void)cap;
   switch (e) {
      case SLURP_OK: return "read whole";
      case SLURP_OPEN: return "cannot be opened (missing, or not permitted)";
      case SLURP_READ: return "could not be read to the end (I/O error)";
      case SLURP_CLOSE: return "reported an error on close";
      case SLURP_TOOBIG:
         return "is larger than this server reads; the prefix is not the "
                "credential. Shorten the chain or raise the ceiling in "
                "srv/tls.c";
      case SLURP_EMPTY: return "is empty";
   }
   return "could not be read";
}

/* The private key: find the 32-byte scalar inside a SEC1 or PKCS#8 EC key.
 * Both wrap it in an OCTET STRING of exactly 32 bytes that is the first such
 * string in the structure, which is enough to find it without a general ASN.1
 * parser -- and a wrong guess cannot go unnoticed, because the certificate
 * would then fail to verify against it. */
static int find_ec_scalar(const uint8_t *der, size_t n, uint8_t out[32])
{
   for (size_t i = 0; i + 34 <= n; i++)
      if (der[i] == 0x04 && der[i + 1] == 0x20) {
         memcpy(out, der + i + 2, 32);
         return 1;
      }
   return 0;
}

/* ---- DOES THE KEY BELONG TO THE CERTIFICATE? --------------------------
 *
 * A CERTIFICATE AND A KEY THAT ARE BOTH VALID AND UNRELATED is the failure
 * this catches, and it is the worst-behaved one available: tls_init parsed the
 * chain, found a 32-byte scalar, and returned success, so the server printed
 * "listening" and accepted connections -- and then every single handshake died
 * at CertificateVerify, because the signature is made with a key the presented
 * certificate does not vouch for. Nothing local looks wrong. The pid is alive,
 * the log says it is serving, and the failure is visible only to clients, as
 * a TLS error they cannot act on. The phone shows TLS REFUSED and retries for
 * ever.
 *
 * It is also the single likeliest rotation mistake: two files, copied in two
 * steps, and copying only one of them leaves exactly this state.
 *
 * The comparison is the whole public key, X and Y, because the point is what
 * the certificate binds: deriving d*G from the scalar and finding it equal to
 * the certificate's SubjectPublicKeyInfo is proof that this key signs for this
 * certificate.
 *
 * THE SPKI IS FOUND BY PATTERN, not by a general ASN.1 walk. The bytes below
 * are the DER of the P-256 AlgorithmIdentifier followed by the BIT STRING
 * header for an uncompressed point -- a fixed, canonical encoding that every
 * producer emits identically for a named-curve P-256 key. A walker would be a
 * hundred lines of certificate parsing in a file that deliberately contains no
 * general X.509 parser, to answer one question about a fixed shape. If the
 * pattern is absent the answer is "this is not a P-256 certificate this server
 * can check", which is refused rather than skipped: a check that silently
 * passes when it cannot run is the failure mode being removed here. */
static const uint8_t P256_SPKI_PREFIX[] = {
    /* SEQUENCE (AlgorithmIdentifier), 0x13 bytes */
    0x30, 0x13,
    /* OID 1.2.840.10045.2.1  id-ecPublicKey */
    0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    /* OID 1.2.840.10045.3.1.7  prime256v1 */
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07,
    /* BIT STRING, 0x42 bytes, 0 unused bits, 0x04 = uncompressed point */
    0x03, 0x42, 0x00, 0x04};

/* The leaf's 64-byte X||Y, or 0 if this certificate does not carry a
 * named-curve P-256 public key in the canonical encoding. */
static int leaf_pubkey(const uint8_t *der, size_t n, uint8_t out[64])
{
   const size_t plen = sizeof P256_SPKI_PREFIX;
   if (n < plen + 64)
      return 0;
   for (size_t i = 0; i + plen + 64 <= n; i++) {
      if (memcmp(der + i, P256_SPKI_PREFIX, plen) != 0)
         continue;
      memcpy(out, der + i + plen, 64);
      return 1;
   }
   return 0;
}

/* 1 when `scalar` is the private key for the leaf certificate's public key. */
static int key_matches_cert(const uint8_t *leaf, size_t leaf_n,
                            const uint8_t scalar[32], const char *name,
                            const char *cert_pem, const char *key_pem)
{
   uint8_t want[64];
   if (!leaf_pubkey(leaf, leaf_n, want)) {
      fprintf(stderr,
              "%s: cannot read a P-256 public key out of the leaf certificate "
              "in %s, so it cannot be checked against %s. This server only "
              "serves P-256; a certificate it cannot check is one it will not "
              "present.\n",
              name, cert_pem, key_pem);
      return 0;
   }
   struct u256 d;
   struct jpoint Q;
   uint8_t got[65];
   p256_sc_from_be(&d, scalar);
   p256_mul_g(&Q, &d);
   p256_uncompressed(&Q, got);
   /* got[0] is the 0x04 tag; the coordinates follow. */
   if (memcmp(got + 1, want, 64) != 0) {
      fprintf(stderr,
              "%s: the private key in %s is NOT the key for the certificate "
              "in %s.\n"
              "  Both files are valid and they do not go together, which is "
              "what copying one half of a rotation leaves behind.\n"
              "  Started anyway, this server would listen, log that it is "
              "serving, and fail EVERY handshake at CertificateVerify -- "
              "visible only to clients, as an error they cannot act on.\n",
              name, key_pem, cert_pem);
      return 0;
   }
   return 1;
}

int tls_init(const char *cert_pem, const char *key_pem, const char *name)
{
   p256_init();
   size_t n;
   enum slurp_err se;
   char *txt = slurp(cert_pem, &n, &se);
   if (!txt) {
      fprintf(stderr, "%s: the certificate file %s %s\n", name, cert_pem,
              slurp_why(se, 0));
      return 1;
   }
   size_t starts[8], lens[8], nb;
   uint8_t der[4096];
   int cut   = 0;
   size_t dn = pem_decode(txt, der, sizeof der, starts, lens, 8, &nb, &cut);
   /* TRUNCATION IS CHECKED FIRST, before "no certificate at all".
    *
    * A file cut inside its ONLY block decodes to zero certificates, so the
    * generic message won the race and reported "no certificate in cert.pem"
    * about a file that plainly contains one -- sending the operator to look
    * for a missing certificate instead of a truncated copy. Both are
    * refusals; only one of them says what happened. */
   if (cut) {
      fprintf(stderr,
              "%s: the certificate file %s is TRUNCATED -- a PEM block begins "
              "and never ends. The %zu certificate(s) that decoded before it "
              "are not the whole chain.\n",
              name, cert_pem, nb);
      return 1;
   }
   if (!nb) {
      fprintf(stderr, "%s: no certificate in %s\n", name, cert_pem);
      return 1;
   }
   /* Build the Certificate message's cert_list once: for each certificate a
    * 3-byte length, the DER, and an empty extension list. */
   size_t k = 0;
   for (size_t i = 0; i < nb; i++) {
      /* -4 for the Certificate message's own framing, which send_certificate
       * prepends: the bound has to be the one the WIRE needs, not the one the
       * array happens to have, or a chain that fits here overflows there. */
      if (k + 3 + lens[i] + 2 > sizeof g_cert - 4) {
         /* REFUSE, do not truncate. This used to `break`, leaving a partial
          * chain -- or none at all, if the very first certificate did not fit
          * -- and tls_init still returned success. The server then came up
          * presenting something no client can verify, with nothing on stderr
          * to say why: the same failure mode as the RSA key below, which is
          * refused outright precisely because a server nobody can reach is
          * worse than a server that did not start. */
         fprintf(stderr,
                 "%s: certificate chain in %s does not fit in %zu bytes "
                 "(stopped at certificate %zu of %zu)\n",
                 name, cert_pem, sizeof g_cert - 4, i + 1, nb);
         return 1;
      }
      put24(g_cert + k, lens[i]);
      k += 3;
      memcpy(g_cert + k, der + starts[i], lens[i]);
      k += lens[i];
      put16(g_cert + k, 0);
      k += 2;
   }
   g_cert_n = k;
   (void)dn;

   /* THE LEAF, COPIED OUT NOW. `starts`/`lens` are about to be reused by the
    * private key's pem_decode, and `der` by nothing -- but relying on that is
    * how a later edit silently compares the key against the wrong bytes. */
   static uint8_t leaf[4096];
   size_t leaf_n = lens[0];
   if (leaf_n > sizeof leaf) {
      fprintf(stderr,
              "%s: the leaf certificate in %s is larger than %zu bytes\n", name,
              cert_pem, sizeof leaf);
      return 1;
   }
   memcpy(leaf, der + starts[0], leaf_n);

   txt = slurp(key_pem, &n, &se);
   if (!txt) {
      fprintf(stderr, "%s: the private key file %s %s\n", name, key_pem,
              slurp_why(se, 0));
      return 1;
   }
   uint8_t kder[4096];
   size_t kn = pem_decode(txt, kder, sizeof kder, starts, lens, 8, &nb, &cut);
   if (cut) {
      fprintf(stderr,
              "%s: the private key file %s is TRUNCATED -- a PEM block begins "
              "and never ends.\n",
              name, key_pem);
      return 1;
   }
   if (!nb || !find_ec_scalar(kder, kn, g_key)) {
      fprintf(stderr, "%s: no EC private key in %s\n", name, key_pem);
      return 1;
   }
   /* BEFORE ANYTHING LISTENS. Both files parsed; the question left is whether
    * they are a pair, and it is answered here rather than by every client. */
   if (!key_matches_cert(leaf, leaf_n, g_key, name, cert_pem, key_pem))
      return 1;
   if (!rnd(g_ticket_key, 16)) {
      fprintf(stderr, "%s: no entropy for the ticket key\n", name);
      return 1;
   }
   return 0;
}

/* ---- BOUNDED NESTED CURSORS -------------------------------------------
 *
 * A ClientHello is vectors inside vectors: the message holds a cipher-suite
 * vector and an extension vector, each extension holds one of its own, and
 * the pre_shared_key extension holds two. Every one of them announces its own
 * length, and the ONLY thing that makes that length trustworthy is that a
 * reader refuses to look outside the vector it is in.
 *
 * THE OLD PARSER WALKED THEM ALL WITH ONE `i` AND A HANDFUL OF ad-hoc
 * COMPARISONS, and the comparisons did not say the same thing twice:
 *
 *   - `for (j = 0; j + 1 < cs && i + j + 1 < n; j += 2)` stops at whichever
 *     bound runs out first and then advances `i += cs` regardless, so a
 *     cipher_suites vector declaring more bytes than the hello holds moved
 *     the cursor past the end and every later field was read from whatever
 *     followed;
 *   - `i += 1 + b[i]` for legacy_compression_methods was not bounded at all;
 *   - the supported_versions, signature_algorithms, psk_key_exchange_modes,
 *     identity and binder loops each ignored their OWN declared length and
 *     ran to the end of the extension body instead, so a vector could
 *     announce two bytes and be read for two hundred;
 *   - nothing required any vector to END where it said it would, so trailing
 *     rubbish inside one was silently the next thing parsed.
 *
 * None of that is a memory-safety bug on its own -- the outer `end > n` check
 * kept most of it inside the buffer -- and that is exactly why it survived:
 * the parser accepted a large family of ClientHellos that no client can spell
 * and that no two implementations would agree on. A message with more than
 * one reading is a message an attacker chooses the reading of.
 *
 * So every vector is opened as its own cursor, which knows where it ends and
 * cannot be read past it, and every one of them must be EXHAUSTED: a vector
 * with bytes left over when its contents have been read is refused, by name,
 * where it was opened. That is one rule per vector rather than one rule for
 * the message, which is what makes a loosened check show up as a failure
 * about the vector it belongs to.
 *
 * Nothing here is TLS-specific enough to live in lib/: it is length-prefixed
 * vector framing, and the sync protocol (lib/wirevec.h) is text. It stays
 * beside the only parser that has any. */
struct vec {
   const uint8_t *p; /* the vector's bytes, not counting its length prefix */
   size_t n;         /* how many there are */
   size_t i;         /* how many have been read */
};

static struct vec vec_of(const uint8_t *p, size_t n)
{
   struct vec v = {p, n, 0};
   return v;
}

/* Exhausted: every declared byte was read as something. The whole point. */
static int vec_done(const struct vec *v)
{
   return v->i == v->n;
}

/* `n` bytes, or NULL and the cursor untouched. A refused take leaves the
 * cursor where it was so a caller that ignores the answer cannot silently
 * advance past the end -- there is no partial read here. */
static const uint8_t *vec_take(struct vec *v, size_t n)
{
   if (n > v->n - v->i)
      return NULL;
   const uint8_t *p = v->p + v->i;
   v->i += n;
   return p;
}

static int vec_u8(struct vec *v, unsigned *out)
{
   const uint8_t *p = vec_take(v, 1);
   if (!p)
      return 0;
   *out = p[0];
   return 1;
}

static int vec_u16(struct vec *v, unsigned *out)
{
   const uint8_t *p = vec_take(v, 2);
   if (!p)
      return 0;
   *out = get16(p);
   return 1;
}

/* Open the nested vector whose 1- or 2-byte length prefix is next. It must
 * lie WHOLLY inside this one: a declared length that overruns the parent is
 * refused here, before a byte of the child is addressed, which is what makes
 * "read past the parent" unreachable rather than merely unlikely. */
static int vec_sub8(struct vec *v, struct vec *out)
{
   unsigned len;
   if (!vec_u8(v, &len))
      return 0;
   const uint8_t *p = vec_take(v, len);
   if (!p)
      return 0;
   *out = vec_of(p, len);
   return 1;
}

static int vec_sub16(struct vec *v, struct vec *out)
{
   unsigned len;
   if (!vec_u16(v, &len))
      return 0;
   const uint8_t *p = vec_take(v, len);
   if (!p)
      return 0;
   *out = vec_of(p, len);
   return 1;
}

/* ---- ClientHello -------------------------------------------------------
 *
 * Only the extensions that decide something are READ -- which TLS version the
 * client really wants, its key share, whether it can verify our signature
 * algorithm, and any resumption ticket. Everything else -- server name, ALPN,
 * the dozen extensions a browser sends -- is skipped, because nothing this
 * server does depends on them.
 *
 * SKIPPED IS NOT UNCHECKED. An extension this server ignores still has to be
 * FRAMED, because its declared length is how the next one is found, and it
 * still counts against the rule that no type may appear twice (RFC 8446 4.2).
 */
struct hello {
   const uint8_t *session_id;
   size_t session_id_n;
   int has_tls13;
   int suite_ok;
   int sig_ok;
   const uint8_t *peer_key; /* 65-byte uncompressed P-256 point, or NULL */
   const uint8_t *psk_ident;
   size_t psk_ident_n;
   /* WHERE THE BINDER LIST BEGINS -- its own 2-byte length field, which is
    * the point the ClientHello is truncated at for the MAC. Only the offset
    * matters, so no length travels with it: how far the list runs was settled
    * while it was framed, and a second copy of that number here would be a
    * second thing that can disagree with the first. */
   const uint8_t *binders;
   /* THE SELECTED IDENTITY'S BINDER, located while the list was framed.
    *
    * binder_ok used to find this itself, as `h->binders[2]` and
    * `h->binders + 3` -- the first entry of a list it re-read and had already
    * been told was well formed. That is where "accepts a prefix" came from:
    * the list's own length was never required to account for every identity,
    * so a binder list holding ONE entry authenticated an offer carrying five,
    * and the four unauthenticated identities were as much a part of the
    * ClientHello as the one that was checked. The framing is settled once,
    * here, and the verifier is handed the entry it is to check. */
   const uint8_t *binder;
   size_t binder_n;
   int psk_dhe;
};

/* WE SELECT IDENTITY 0 AND NOTHING ELSE. We issue exactly one ticket per
 * connection, so the offer we could accept is the first one; the rest of the
 * list is still framed and still counted, because the binder list has to
 * match it entry for entry. */
#define PSK_SELECTED 0

/* Duplicate extension types, one bit per type. RFC 8446 4.2: "There MUST NOT
 * be more than one extension of the same type in a given extension block."
 *
 * A BITMAP RATHER THAN A LIST OF THE ONES WE READ. Duplicating an extension
 * this server ignores is just as much a second reading of the message -- a
 * middlebox and this parser would disagree about which copy counts -- and a
 * ClientHello may carry a few thousand four-byte extensions, which is a
 * quadratic scan if the seen set is a list. 8 KiB of stack against a 16 KiB
 * record buffer two frames up is not the expensive thing here. */
#define EXT_SEEN_BYTES 8192

static int ext_seen(uint8_t *seen, unsigned type)
{
   uint8_t bit = (uint8_t)(1u << (type & 7));
   if (seen[type >> 3] & bit)
      return 1;
   seen[type >> 3] |= bit;
   return 0;
}

/* The pre_shared_key extension: OfferedPsks, framed exactly (RFC 8446
 * 4.2.11).
 *
 *     struct {
 *         PskIdentity identities<7..2^16-1>;
 *         PskBinderEntry binders<33..2^16-1>;
 *     } OfferedPsks;
 *
 * ...where a PskIdentity is opaque identity<1..2^16-1> followed by a uint32
 * obfuscated_ticket_age, and a PskBinderEntry is opaque<32..255>.
 *
 * THE AGE WAS NEVER SKIPPED. The old code read identity 0's length and bytes
 * and then jumped to `2 + idn` for the binder list, so it never walked the
 * identity list at all -- which is why it could not count it, and why the
 * binder list was accepted at whatever length it claimed. Four bytes per
 * entry that nothing consumed is the difference between "the first identity"
 * and "the identities".
 *
 * ONE BINDER PER IDENTITY, and the counts must be equal: RFC 8446 4.2.11.2
 * says the binders field is "a series of HMAC values, one for each value in
 * the identities list and in the same order". A shorter binder list is not a
 * partial offer to be read as far as it goes; it is an offer whose remaining
 * identities carry no proof at all. */
static int parse_psk(struct vec *v, struct hello *h)
{
   struct vec ids, bs;
   if (!vec_sub16(v, &ids) || !vec_sub16(v, &bs) || !vec_done(v))
      return 0; /* OfferedPsks is the whole extension, and nothing more */
   /* THE TRUNCATION POINT, which is what the binder is a MAC over: the
    * ClientHello up to and not including the binder list's own length field.
    * Recorded here, where the list was opened, rather than reconstructed by
    * the verifier from the first entry's address. */
   h->binders = bs.p - 2;

   unsigned nid = 0;
   while (!vec_done(&ids)) {
      struct vec one;
      if (!vec_sub16(&ids, &one) || one.n < 1 || !vec_take(&ids, 4))
         return 0; /* identity<1..>, then obfuscated_ticket_age */
      if (nid == PSK_SELECTED) {
         h->psk_ident   = one.p;
         h->psk_ident_n = one.n;
      }
      nid++;
   }

   unsigned nb = 0;
   while (!vec_done(&bs)) {
      struct vec one;
      if (!vec_sub8(&bs, &one) || one.n < 32)
         return 0; /* PskBinderEntry is opaque<32..255> */
      if (nb == PSK_SELECTED) {
         h->binder   = one.p;
         h->binder_n = one.n;
      }
      nb++;
   }
   if (nid == 0 || nid != nb)
      return 0;
   return 1;
}

static int parse_hello(const uint8_t *b, size_t n, struct hello *h)
{
   memset(h, 0, sizeof *h);
   struct vec ch = vec_of(b, n);
   if (!vec_take(&ch, 2 + 32)) /* legacy_version, random */
      return 0;

   struct vec sid;
   if (!vec_sub8(&ch, &sid) || sid.n > 32) /* legacy_session_id<0..32> */
      return 0;
   h->session_id   = sid.p;
   h->session_id_n = sid.n;

   struct vec cs; /* cipher_suites<2..2^16-2>, whole suites and no tail */
   if (!vec_sub16(&ch, &cs) || cs.n < 2)
      return 0;
   while (!vec_done(&cs)) {
      unsigned suite;
      if (!vec_u16(&cs, &suite))
         return 0;
      if (suite == SUITE_AES128)
         h->suite_ok = 1;
   }

   /* legacy_compression_methods. RFC 8446 4.1.2 leaves no room here at all:
    * "For every TLS 1.3 ClientHello, this vector MUST contain exactly one
    * byte, set to zero" -- so this is the one vector whose contents are as
    * fixed as its framing, and the old `i += 1 + b[i]` neither read nor
    * bounded it. */
   struct vec cm;
   if (!vec_sub8(&ch, &cm) || cm.n != 1 || cm.p[0] != 0)
      return 0;

   struct vec ex;
   if (!vec_sub16(&ch, &ex) || !vec_done(&ch))
      return 0; /* the extensions are the last thing in a ClientHello */

   uint8_t seen[EXT_SEEN_BYTES];
   memset(seen, 0, sizeof seen);
   const uint8_t *psk_at = NULL; /* where a pre_shared_key extension began */
   size_t psk_n          = 0;

   while (!vec_done(&ex)) {
      unsigned type;
      struct vec v;
      if (!vec_u16(&ex, &type) || !vec_sub16(&ex, &v))
         return 0;
      if (ext_seen(seen, type))
         return 0;
      switch (type) {
         case EXT_SUPPORTED_VERSION: {
            struct vec vs; /* ProtocolVersion versions<2..254> */
            if (!vec_sub8(&v, &vs) || !vec_done(&v))
               return 0;
            while (!vec_done(&vs)) {
               unsigned ver;
               if (!vec_u16(&vs, &ver))
                  return 0;
               if (ver == 0x0304)
                  h->has_tls13 = 1;
            }
            break;
         }
         case EXT_SIG_ALGS: {
            struct vec sa; /* SignatureScheme supported_signature_algorithms */
            if (!vec_sub16(&v, &sa) || !vec_done(&v))
               return 0;
            while (!vec_done(&sa)) {
               unsigned alg;
               if (!vec_u16(&sa, &alg))
                  return 0;
               if (alg == SIG_ECDSA256)
                  h->sig_ok = 1;
            }
            break;
         }
         case EXT_KEY_SHARE: {
            struct vec ks; /* KeyShareEntry client_shares<0..2^16-1> */
            if (!vec_sub16(&v, &ks) || !vec_done(&v))
               return 0;
            while (!vec_done(&ks)) {
               unsigned grp;
               struct vec pt;
               /* THE WHOLE SHARE MUST LIE INSIDE THE EXTENSION before any of
                * it is read. `v[j + 4]` alone was read when j + 4 == len --
                * one byte past the end -- and the 65 bytes it selected were
                * then handed to p256_from_xy, which reads all of them. A
                * seven-byte body (00 07 | 00 17 | 00 41 | 04) walked 64 bytes
                * off an attacker-chosen extension, before any authentication
                * had happened. vec_sub16 is that check, and it is now the
                * same check every other vector here gets. */
               if (!vec_u16(&ks, &grp) || !vec_sub16(&ks, &pt))
                  return 0;
               if (grp == GROUP_P256 && pt.n == 65 && pt.p[0] == 0x04)
                  h->peer_key = pt.p;
            }
            break;
         }
         case EXT_PSK_MODES: {
            struct vec pm; /* PskKeyExchangeMode ke_modes<1..255> */
            if (!vec_sub8(&v, &pm) || !vec_done(&v))
               return 0;
            while (!vec_done(&pm)) {
               unsigned mode;
               if (!vec_u8(&pm, &mode))
                  return 0;
               if (mode == 1) /* psk_dhe_ke */
                  h->psk_dhe = 1;
            }
            break;
         }
         case EXT_PRE_SHARED_KEY:
            psk_at = v.p;
            psk_n  = v.n;
            if (!parse_psk(&v, h))
               return 0;
            break;
         default: break; /* framed above; its contents decide nothing */
      }
   }

   /* pre_shared_key MUST be the last extension in the ClientHello (RFC 8446
    * 4.2.11), and a server that sees it elsewhere must abort. The reason is
    * structural: the binder is a MAC over the ClientHello UP TO the start of
    * the binder list, so "everything before the binders" is only a
    * well-defined span if nothing follows them. The struct used to carry a
    * psk_ext_off recorded for exactly this check, which nothing ever read --
    * the intent was in the field and the check was missing.
    *
    * The extension's own end is now the thing compared, rather than the
    * binder span's: with the identity and binder lists required to exhaust
    * OfferedPsks, "the extension ends where the hello does" says the same
    * thing and says it about the extension. */
   if (psk_at && psk_at + psk_n != b + n)
      return 0;
   return 1;
}

/* ---- tickets -----------------------------------------------------------
 *
 * Stateless: the ticket carries the PSK, sealed under a key only this process
 * holds, so nothing has to be remembered between connections and a restart
 * simply invalidates outstanding tickets.
 * Layout: nonce || AEAD(issued_at || psk) || tag.
 *
 * THE ISSUE TIME IS SEALED WITH IT, and checked on the way back in. The
 * ticket message on the wire advertises a 7200 s lifetime, and RFC 8446
 * 4.6.1 makes that binding on the server -- but the sealed blob used to be
 * the bare PSK, and ticket_open checked only its length and tag, so a ticket
 * resumed an authenticated session for as long as the PROCESS lived. On a
 * board that runs for months that is indistinguishable from forever: one
 * copied ticket is a session that never has to be re-authenticated.
 *
 * The clock is CLOCK_MONOTONIC (http_mono_s), not the wall clock. The ticket
 * key is per-process, so a ticket cannot outlive the process anyway, and
 * uptime is the one clock this board cannot get wrong -- it has no RTC, and
 * the wall clock jumps when NTP finally lands. */
#define TICKET_LIFETIME_S 7200

/* WHETHER THERE IS A CLOCK IS AN ANSWER, NOT A TIME.
 *
 * This used to be `if (clock_gettime(...) != 0) return 0;` -- a failed clock
 * spelled as second zero, which is also a real reading. Nothing downstream
 * could tell the two apart, and both of the places that ask are about the age
 * of a ticket:
 *
 *   - ticket_seal SEALED that zero as the issue time. A ticket stamped zero
 *     is a ticket whose age is `now`, so on the day the clock came back it
 *     would expire immediately -- but while the clock stays broken, `now` is
 *     zero too, and the ticket is eternally seconds old.
 *   - ticket_open then compared zero against zero and found the ticket fresh.
 *     Persistent failure therefore made every zero-issued ticket resume for
 *     as long as the process lived, which is precisely the "resumes for as
 *     long as the PROCESS lived" defect the sealed timestamp was added to
 *     close -- reintroduced through the error path.
 *
 * So the status is returned and `*out` is left alone when there is none, and
 * both callers refuse. Refusing costs a full handshake per connection, which
 * is slow; accepting costs the ticket lifetime, which is the security
 * property. CLOCK_MONOTONIC failing is a broken kernel or a bad clock id, so
 * neither cost is one this board will actually pay.
 *
 * TLS_CLOCK_OK IS ZERO, like every other typed outcome here (enum hkdf_status,
 * enum csv_field, enum db_get), so every caller compares against it BY NAME.
 * `if (!tls_mono_s(&now))` would read as "the clock failed" and mean the
 * opposite -- the mistake enum csv_field's CSV_FIELD_OK once produced at a
 * call site that spelled it `!ok`. */
enum tls_clock {
   TLS_CLOCK_OK = 0, /* *out holds seconds of uptime */
   TLS_CLOCK_NONE    /* no monotonic clock; *out is untouched */
};

/* Seconds of uptime. Written here rather than borrowed from http.c so this
 * file stays linkable on its own -- srv/test/cryptotest.c builds tls.c
 * against lib/ and nothing else, and a dependency on the HTTP layer for a
 * clock would have made the crypto vectors need a web server. */
static enum tls_clock tls_mono_s(uint64_t *out)
{
#ifdef TLS_FAULTS
   /* ---- A CLOCK THAT FAILS ON DEMAND, for the test build only ----------
    *
    * NOT COMPILED INTO A SHIPPING BINARY: the whole block is behind
    * -DTLS_FAULTS, which only the cryptotest recipe sets (see the Makefile),
    * exactly as -DDB_FAULTS gates srv/db.c's injected prepare and commit
    * failures and -DAPP_FAULTS gates the app's.
    *
    * It exists because clock_gettime(CLOCK_MONOTONIC) does not fail on any
    * machine a test runs on, so the rule above -- refuse rather than read the
    * failure as second zero -- is otherwise unreachable and would be pinned
    * by nothing.
    *
    *   TLS_FAIL_MONOTONIC=1   every ask answers TLS_CLOCK_NONE
    *   TLS_MONOTONIC_FIXED=n  every ask answers TLS_CLOCK_OK with `n`
    *
    * The second is what separates the two cases the defect confused: with
    * n = 0 the clock is WORKING and reading zero, tickets are issued, and
    * they resume -- which a build that maps failure back to zero cannot tell
    * apart from a dead clock. */
   const char *s = getenv("TLS_FAIL_MONOTONIC");
   if (s && *s == '1') {
      fprintf(stderr, "sync: INJECTED FAULT TLS_FAIL_MONOTONIC\n");
      return TLS_CLOCK_NONE;
   }
   const char *f = getenv("TLS_MONOTONIC_FIXED");
   if (f && *f) {
      *out = (uint64_t)strtoull(f, NULL, 10);
      return TLS_CLOCK_OK;
   }
#endif
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return TLS_CLOCK_NONE;
   *out = (uint64_t)ts.tv_sec;
   return TLS_CLOCK_OK;
}

static size_t ticket_seal(const uint8_t psk[32], uint8_t *out)
{
   uint8_t nonce[12];
   uint8_t pt[8 + 32];
   if (!rnd(nonce, 12))
      return 0;
   /* NO CLOCK, NO TICKET, and the refusal happens before a byte of `out` is
    * written. Sealing one anyway would need an issue time, and every
    * available spelling of "unknown" is a number ticket_open will later
    * subtract from something -- which is how a stamp of zero became a ticket
    * that never ages. A connection that gets no ticket resumes nothing and
    * pays for a full handshake next time; that is the whole cost. */
   uint64_t now;
   if (tls_mono_s(&now) != TLS_CLOCK_OK)
      return 0;
   for (int i = 0; i < 8; i++)
      pt[i] = (uint8_t)(now >> (8 * (7 - i)));
   memcpy(pt + 8, psk, 32);
   memcpy(out, nonce, 12);
   /* Same rule, same reason: `pt` is a fixed 40 bytes, so a refusal cannot
    * happen -- and a ticket whose ciphertext was never written would be handed
    * to a client as a resumable session. ticket_seal already answers 0 for a
    * failed nonce draw. */
   if (aes128_gcm_seal(g_ticket_key, nonce, NULL, 0, pt, sizeof pt, out + 12,
                       out + 12 + sizeof pt) != GCM_OK)
      return 0;
   return 12 + sizeof pt + 16;
}

static int ticket_open(const uint8_t *t, size_t n, uint8_t psk[32])
{
   uint8_t pt[8 + 32];
   /* THE AGE CHECK IS THE ONLY THING BOUNDING A TICKET'S LIFE, so a run that
    * cannot perform it does not get to skip it. Asked first, before the seal
    * is even opened: an unreadable clock is a property of this machine, not
    * of the ticket, and there is nothing to learn from the ciphertext once
    * the answer is already "no". The peer falls through to the full
    * certificate handshake, which is slower and correct. */
   uint64_t now;
   if (tls_mono_s(&now) != TLS_CLOCK_OK)
      return 0;
   if (n != 12 + sizeof pt + 16)
      return 0;
   if (!aes128_gcm_open(g_ticket_key, t, NULL, 0, t + 12, sizeof pt,
                        t + 12 + sizeof pt, pt))
      return 0;
   uint64_t issued = 0;
   for (int i = 0; i < 8; i++)
      issued = (issued << 8) | pt[i];
   /* now < issued cannot happen with a monotonic clock, but an unsigned
    * subtraction that underflowed would read as "brand new" forever, so the
    * comparison is written to refuse rather than wrap. */
   if (now < issued || now - issued > TICKET_LIFETIME_S)
      return 0;
   memcpy(psk, pt + 8, 32);
   return 1;
}

/* ---- the PSK binder ----------------------------------------------------
 *
 * THE BINDER IS THE ONLY THING THAT PROVES THE CLIENT HOLDS THE PRE-SHARED
 * KEY. A resumption offer is a ticket -- which travels in the clear, in the
 * ClientHello, where anyone on the path can copy it -- plus a MAC over the
 * handshake so far, keyed by a secret derived from the PSK sealed inside that
 * ticket. Opening the ticket proves only that WE issued it. The binder is
 * what proves the peer presenting it is the peer we issued it to.
 *
 * RFC 8446 4.2.11.2 defines the MAC and 4.2.11 states the duty: "If this
 * value is not present or does not validate, the server MUST abort the
 * handshake."
 *
 * WHAT `pre` IS, AND WHY IT IS NOT ALWAYS EMPTY. The binder covers the
 * "partial ClientHello" -- the transcript up to and not including the binder
 * list -- and RFC 8446 4.2.11.2 spells out what that transcript contains:
 *
 *     Truncate(ClientHello1)                          (no retry)
 *     ClientHello1, HelloRetryRequest, Truncate(ClientHello2)   (after one)
 *
 * ...with ClientHello1 in the second case replaced by the synthetic
 * "message_hash" message of 4.4.1, because after a HelloRetryRequest the
 * transcript never again contains ClientHello1 itself. `pre` is that prefix,
 * already in c->trans by the time a second ClientHello is parsed, and it is
 * empty for the first one. The two cases are the same computation over
 * different prefixes, which is why they are one function: a second copy of
 * this arithmetic is a second chance to check the wrong bytes.
 *
 * WHY THE SYNTHETIC message_hash IS REQUIRED and not a detail. It is what
 * makes the CH2 binder unforgeable-without-the-PSK for THIS handshake: it
 * folds in the hash of the exact ClientHello1 that provoked the retry and the
 * exact HelloRetryRequest we answered with. Verify a CH2 binder over the
 * truncated CH2 alone and the proof stops being about this handshake -- the
 * same binder would validate after any retry, in any connection.
 *
 * The comparison is ct_eq because the binder is an authenticator a client may
 * retry, and a comparison that returns early leaks how many bytes of a guess
 * were right.
 *
 * Returns 1 only if the binder is present, well formed and correct. */
static int binder_ok(const struct hello *h, const uint8_t *ch,
                     const uint8_t *pre, size_t pren, const uint8_t psk[32])
{
   /* FRAMING IS SETTLED BEFORE THIS RUNS. parse_hello walked the identity and
    * binder lists as bounded vectors, required each to exhaust its declared
    * length, and required one correctly framed binder per identity -- so
    * `h->binder` is entry PSK_SELECTED of a list that accounts for every
    * identity offered, not the first entry of a list that was allowed to stop
    * early. This used to be `h->binders[2] != 32` over a list nothing had
    * counted, which is the "accepts a prefix" defect: five identities and one
    * binder validated, and the four identities no MAC covered rode along.
    *
    * WHAT IS LEFT TO CHECK HERE IS THE WIDTH, and it belongs here rather than
    * in the framing: a PskBinderEntry is opaque<32..255> on the wire, so a
    * 48-byte binder is a well-framed offer -- for a hash that is not the one
    * this server speaks. TLS_AES_128_GCM_SHA256 means SHA-256 means 32 bytes,
    * and a comparison against anything else would be comparing our HMAC with
    * part of somebody else's. */
   if (h->binder == NULL || h->binder_n != 32)
      return 0;

   uint8_t early[32], binder_key[32], fk[32], th[32], want[32];
   hkdf_extract(NULL, 0, psk, 32, early);
   /* A refusal here is a broken key schedule, not a bad binder, but `want` is
    * then left unwritten and must not reach ct_eq as though it were a MAC --
    * so the derivation guards the comparison rather than only itself. */
   if (!derive_secret(early, "res binder", (const uint8_t *)"", 0,
                      binder_key) ||
       !hkdf_expand_label(binder_key, "finished", NULL, 0, fk, 32))
      return 0;

   /* Hashed in two pieces rather than copied into one buffer: the prefix is
    * up to TRANS_MAX and the hello up to REC_MAX, and a 24 kB stack frame on
    * a pool worker to save an incremental hash is a poor trade. */
   size_t upto = (size_t)(h->binders - ch); /* the truncation point */
   struct sha256_ctx sc;
   sha256_init(&sc);
   if (pren)
      sha256_update(&sc, pre, pren);
   sha256_update(&sc, ch, upto);
   sha256_final(&sc, th);

   hmac_sha256(fk, 32, th, 32, want);
   return ct_eq(want, h->binder, 32);
}

/* ---- the handshake ------------------------------------------------------ */

static int send_handshake(struct tls_conn *c, int type, const uint8_t *body,
                          size_t n)
{
   uint8_t msg[REC_MAX];
   if (n + 4 > sizeof msg)
      return 0;
   msg[0] = (uint8_t)type;
   put24(msg + 1, n);
   memcpy(msg + 4, body, n);
   if (!transcript(c, msg, n + 4))
      return 0; /* the transcript is full: nothing more may be sent */
   return send_record(c, REC_HANDSHAKE, msg, n + 4);
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
   } else if (!rnd(b + k, 32)) {
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
   return send_handshake(c, HS_SERVER_HELLO, b, k);
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
   transcript_hash(c, tbs + k);
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
    * p256_sc_rand, not rnd(nonce, 32). The nonce is a scalar, so it gets the
    * one generator that rejection-samples [1, n-1] (lib/p256.h). This site was
    * the least exposed of the four: ecdsa_p256_sign has always refused a zero
    * k outright, so the only thing that changes here is the 2^-32 bias in the
    * reduction it used to do. Refusing is still the right answer and there is
    * deliberately no retry -- a nonce this server cannot draw is a handshake
    * it declines, and the client reconnects. */
   if (!p256_sc_rand(nonce) || !ecdsa_p256_sign(g_key, h, nonce, r, s))
      return 0;

   uint8_t b[128];
   put16(b, SIG_ECDSA256);
   size_t sn = der_sig(r, s, b + 4);
   put16(b + 2, (unsigned)sn);
   return send_handshake(c, HS_CERT_VERIFY, b, 4 + sn);
}

/* The Certificate message: one empty request context, then the chain.
 *
 * The 4 bytes of framing are why the bound is `sizeof b - 4` and not
 * `sizeof b`. tls_init only checks the chain against sizeof g_cert, so a
 * chain of 4093..4096 bytes loaded happily and then wrote up to four bytes
 * past this buffer on EVERY handshake -- with -static -no-pie and no stack
 * protector under it. A three-certificate chain reaches that size, so it was
 * a configuration change away, not an attack away. */
static int send_certificate(struct tls_conn *c)
{
   uint8_t b[4096];
   if (g_cert_n > sizeof b - 4)
      return 0;
   b[0] = 0; /* certificate_request_context: empty */
   put24(b + 1, g_cert_n);
   memcpy(b + 4, g_cert, g_cert_n);
   return send_handshake(c, HS_CERTIFICATE, b, 4 + g_cert_n);
}

/* WHY A FAILED TICKET IS TWO DIFFERENT ANSWERS.
 *
 * This used to return int and the one caller wrote `(void)send_ticket(c)`,
 * with the comment "no ticket is a slower next connection, not a broken one".
 * That is true of exactly half the failures. A derivation or a seal that
 * refuses has put NOTHING on the wire: the connection is untouched and the
 * only cost is a full handshake next time. A failed WRITE is not that. It is
 * a full_write that gave up part way -- a timeout, a peer that stopped
 * reading, a reset -- and what it leaves behind is a PARTIAL PROTECTED RECORD
 * in the stream. From the peer's side the record layer is finished: it is
 * waiting for the rest of a record that will never arrive, and every byte
 * that follows, including the whole of the response, is framed wrong and
 * undecryptable.
 *
 * Discarding that made tls_handshake return success, and https.c's prepare()
 * then handed the connection to http_handle_conn, which read the request and
 * RAN IT -- authenticating a session, writing a row, sending a mail -- for a
 * client that could not read a single byte of the answer, and which will
 * therefore retry the same request on a new connection. A request executed
 * twice because the reply was unreadable is the kind of "worked, apparently"
 * that only shows up as duplicated state.
 *
 * So the caller is told which happened, and only the write failure is
 * fatal. */
enum ticket_fate {
   TICKET_SENT = 0,
   TICKET_NOT_ISSUED,  /* nothing reached the wire: next time is just slower */
   TICKET_WRITE_FAILED /* bytes may be on the wire: the record layer is torn */
};

static enum ticket_fate send_ticket(struct tls_conn *c)
{
   uint8_t nonce[8] = {0, 0, 0, 0, 0, 0, 0, 1};
   uint8_t psk[32];
   if (!hkdf_expand_label(c->res_master, "resumption", nonce, 8, psk, 32))
      return TICKET_NOT_ISSUED;
   uint8_t t[80];
   size_t tn = ticket_seal(psk, t);
   if (!tn)
      return TICKET_NOT_ISSUED;
   uint8_t b[128];
   size_t k = 0;
   b[k++]   = 0;
   b[k++]   = 0;
   b[k++]   = 0x1c; /* lifetime 7200 s: TICKET_LIFETIME_S, enforced above */
   b[k++]   = 0x20;
   put16(b + k, 0); /* age_add: we do not use it */
   put16(b + k + 2, 0);
   k += 4;
   b[k++] = 8;
   memcpy(b + k, nonce, 8);
   k += 8;
   put16(b + k, (unsigned)tn);
   k += 2;
   memcpy(b + k, t, tn);
   k += tn;
   put16(b + k, 0); /* no extensions */
   k += 2;
   /* From here a refusal is a WRITE that did not complete. send_handshake
    * hashes the message and hands it to send_record, and send_record's only
    * failure that is not a compile-time impossibility is full_write's -- so a
    * 0 here means some prefix of a protected record may already be in the
    * stream. */
   return send_handshake(c, HS_NEW_TICKET, b, k) ? TICKET_SENT
                                                 : TICKET_WRITE_FAILED;
}

#ifdef TLS_FAULTS
/* ---- THREE DOORS FOR THE TEST BUILD, and why they are doors at all ----
 *
 * Tickets are sealed, opened and sent by static functions, and the rule under
 * test -- no monotonic clock, no ticket, in EITHER direction -- is a property
 * of those three and of nothing a client can reach from outside. A full
 * end-to-end case would need a second implementation of TLS 1.3 to hold the
 * other end of a resumption, which is the thing srv/test/tlstest.sh borrows
 * openssl for and which cannot inject a clock failure into this process.
 *
 * So the suite is let in, in the build that already carries the injected
 * clock and in no other: everything here is behind -DTLS_FAULTS, the same
 * gate, so a shipping binary has neither the fault nor the door. tls.h
 * already declares hkdf_expand_label and derive_secret for the same reason
 * (to pin the key schedule to RFC 8448) -- the difference is that those two
 * are safe in any build and these are not.
 *
 * tls_fault_send_ticket answers the question the status alone cannot: it
 * returns whether a NewSessionTicket reached the WIRE, and the caller reads
 * the socket to confirm nothing did. */
size_t tls_fault_ticket_seal(const uint8_t psk[32], uint8_t *out)
{
   return ticket_seal(psk, out);
}

int tls_fault_ticket_open(const uint8_t *t, size_t n, uint8_t psk[32])
{
   return ticket_open(t, n, psk);
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
 * transcript() said. The caller checks that the answer is a refusal AND that
 * c->fatal was raised, which are two different claims: returning 0 without the
 * flag would leave every other exit path believing the connection is well. */
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
      ok       = transcript(c, z, k);
      left -= k;
   }
   if (fatal)
      *fatal = c->fatal;
   return ok;
}

int tls_fault_send_ticket(int fd)
{
   struct tls_conn *c = tls_conn_slot();
   memset(c, 0, sizeof *c);
   c->fd = fd;
   /* Plaintext records (c->encrypted stays 0), so the test can read the
    * NewSessionTicket off the socket without holding a key. */
   memset(c->res_master, 0x5a, sizeof c->res_master);
   return send_ticket(c) == TICKET_SENT;
}
#endif

/* Read one handshake message into `out`, skipping change_cipher_spec. */
static int read_handshake(struct tls_conn *c, uint8_t *out, size_t cap,
                          int *type, size_t *n, tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = read_record(c, giveup);
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
      if (!transcript(c, c->plain, len + 4))
         return 0;
      return 1;
   }
}

/* The next record that is not a change_cipher_spec. Clients send those for
 * middlebox compatibility, before AND after a HelloRetryRequest, and they
 * carry no meaning for us -- but they are not handshake messages either, so
 * they must be skipped rather than rejected. */
/* The next record that means something, skipping change_cipher_spec. */
static int next_record(struct tls_conn *c, tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = read_record(c, giveup);
      if (t != REC_CHANGE_CIPHER)
         return t;
      if (++skipped > CCS_MAX)
         return -1;
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
   int t = next_record(c, giveup);
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
       ticket_open(h.psk_ident, h.psk_ident_n, psk)) {
      if (!binder_ok(&h, c->plain, NULL, 0, psk))
         return 0;
      resumed = 1;
   }

   if (!transcript(c, c->plain, chn + 4))
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
      transcript_hash(c, th);
      c->trans_n       = 0;
      uint8_t synth[4] = {254, 0, 0, 32}; /* message_hash, length 32 */
      if (!transcript(c, synth, 4) || !transcript(c, th, 32))
         return 0;

      if (!send_server_hello(c, &h, NULL, 1, 0)) /* appends the HRR itself */
         return 0;
      t = next_record(c, giveup);
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
       * `resumed` and `psk` used to carry over from ClientHello 1 on the
       * reasoning that CH1's binder had been verified, so the client had
       * already proved it held the key. That reasoning does not survive
       * contact with what a binder is FOR. The MAC is over a transcript, and
       * a transcript is what ties the proof to one handshake: verified over
       * CH1 alone, the same binder authenticates any ClientHello 2 that
       * follows it, including one nobody who holds the PSK ever wrote.
       *
       * Concretely, with the old code: ClientHello 2 arrives in the clear and
       * nothing downstream reads its binder, so anyone on the path could
       * rewrite it wholesale -- its key share, its extensions, its PSK offer,
       * or delete the pre_shared_key extension entirely -- and the server
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
          ticket_open(h.psk_ident, h.psk_ident_n, psk)) {
         if (!binder_ok(&h, c->plain, c->trans, c->trans_n, psk))
            return 0;
         resumed = 1;
      }

      if (!transcript(c, c->plain, chn + 4))
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
    * THE SCALAR ITSELF used to be `rnd(priv, 32)` followed by a reduction mod
    * n, which is biased and which accepts zero (see lib/p256.h). A zero `d`
    * here makes Q the point at infinity -- so the key_share we advertise is
    * 04||0..0 -- and makes S the identity, so the ECDHE secret is a constant
    * every eavesdropper can compute and the whole handshake is unprotected. It
    * never got that far in practice, because p256_to_xy(&S) below refuses the
    * infinite point and that happens before send_server_hello; but that is a
    * property of two later lines, not of the draw, and it is not what this call
    * used to look like it guaranteed. p256_sc_rand refuses zero and the biased
    * tail at the source. The reduction stays only because `d` needs limbs, and
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
       !traffic_keys(shs, &c->wr) || !traffic_keys(chs, &c->rd))
      return 0;
   c->encrypted = 1;

   /* --- our half of the handshake, all of it encrypted --- */
   uint8_t ee[2] = {0, 0}; /* EncryptedExtensions: none */
   if (!send_handshake(c, HS_ENCRYPTED_EXT, ee, 2))
      return 0;
   if (!resumed) { /* a resumed session is already authenticated by the PSK */
      if (!send_certificate(c) || !send_cert_verify(c))
         return 0;
   }
   uint8_t fin[32];
   if (!finished_mac(c, shs, fin))
      return 0;
   if (!send_handshake(c, HS_FINISHED, fin, 32))
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
   if (!traffic_keys(sap, &c->wr) || !traffic_keys(cap_, &c->rd))
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
   if (send_ticket(c) == TICKET_WRITE_FAILED)
      return 0;
   return !c->fatal;
}

/* An alert, best effort, on the way out. The connection is over either way;
 * this only decides whether the peer is told why. */
static void send_alert(struct tls_conn *c, int desc)
{
   uint8_t a[2] = {ALERT_FATAL, (uint8_t)desc};
   (void)send_record(c, REC_ALERT, a, 2);
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
   return send_record(c, REC_HANDSHAKE, msg, n + 4);
}

/* ---- KeyUpdate (RFC 8446 4.6.3) ----------------------------------------
 *
 * WHAT THIS FILE USED TO DO WITH ONE: nothing at all. tls_recv cleared the
 * record and read the next, which is the worst of the three available
 * answers. The peer has ALREADY switched its sending keys by the time the
 * message arrives -- 4.6.3 is explicit that the sender updates after sending
 * -- so the very next record it sends is encrypted under a key this server no
 * longer has any way to reach. aes128_gcm_open then fails, read_record
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
 *     hkdf_expand_label and traffic_keys are here, tested against RFC 8448 by
 *     srv/test/cryptotest.c, and the derivation is one label:
 *
 *         application_traffic_secret_N+1 =
 *             HKDF-Expand-Label(application_traffic_secret_N,
 *                               "traffic upd", "", Hash.length)   (7.2)
 *
 *     with the record sequence number reset to zero at the key change (5.3),
 *     which traffic_keys already does for every other key it derives.
 *   - It is EXECUTABLE-TESTABLE either way, and the handling half is testable
 *     in both directions at once: srv/test/tlstest.sh drives a real client
 *     that sends a KeyUpdate asking for one back and then makes a request, so
 *     a wrong read-side update loses the request and a wrong write-side
 *     update loses the response.
 *
 * `request` is the peer's request_update. update_requested obliges us to send
 * a KeyUpdate of our own before our next Application Data record (4.6.3);
 * ours always says update_not_requested, because two peers that each ask for
 * one back rekey at each other for as long as the connection lasts.
 *
 * ORDER MATTERS TWICE HERE. Our KeyUpdate goes out under the CURRENT write
 * keys and only then do we advance them -- the peer is still decrypting with
 * the old ones until it has read the message telling it not to. And the read
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
      if (!traffic_keys(c->sts, &c->wr))
         return 0;
   }
   if (!hkdf_expand_label(c->cts, "traffic upd", NULL, 0, next, 32))
      return 0;
   memcpy(c->cts, next, 32);
   return traffic_keys(c->cts, &c->rd);
}

/* A post-handshake handshake record. 1 to keep reading, 0 to close -- with an
 * alert already sent, so the peer learns which message it was.
 *
 * ONE MESSAGE PER RECORD, and exactly filling it. Coalescing post-handshake
 * messages is legal and nothing does it; accepting a record with a message
 * and a tail would mean either parsing a stream here or silently dropping the
 * tail, and dropping the tail is the habit this whole function exists to
 * break. */
static int post_handshake(struct tls_conn *c)
{
   if (c->plain_n < 4 || get24(c->plain + 1) + 4 != c->plain_n) {
      send_alert(c, ALERT_UNEXPECTED_MSG);
      return 0;
   }
   if (c->plain[0] != HS_KEY_UPDATE || c->plain_n != 5) {
      /* A NewSessionTicket from a client, a post-handshake Certificate for
       * the client authentication this server does not do, a second
       * ClientHello attempting renegotiation that TLS 1.3 does not have: all
       * of them are messages we will not act on, and every one of them leaves
       * the peer believing we did. RFC 8446 6.2 has the answer for a message
       * that is not allowed here. */
      send_alert(c, ALERT_UNEXPECTED_MSG);
      return 0;
   }
   if (c->plain[4] > 1) { /* KeyUpdateRequest is an enum of exactly two */
      send_alert(c, ALERT_ILLEGAL_PARAMETER);
      return 0;
   }
   if (!key_update(c, c->plain[4] == 1)) {
      send_alert(c, ALERT_UNEXPECTED_MSG); /* past KEY_UPDATE_MAX, or no key */
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
      int t = read_record(c, giveup);
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
       * them instead, as this loop used to, is also an unbounded free loop:
       * a five-byte record a peer can send forever, each costing us a read. */
      send_alert(c, ALERT_UNEXPECTED_MSG);
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
      if (!send_record(c, REC_APPDATA, p + sent, take))
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
      (void)send_record(c, REC_ALERT, alert, 2);
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
