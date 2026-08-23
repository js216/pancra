/* SPDX-License-Identifier: GPL-3.0
 * tlsrec.c --- the TLS record layer: bytes on a socket, sealed and opened
 * Copyright 2026 Jakob Kastelic
 *
 * ONE OF THE THREE FILES OF THE TLS MODULE; srv/tlsint.h says what
 * the split is and what it preserves, and srv/tls.h is the only thing outside
 * the module that sees any of it.
 *
 * WHAT IS HERE: everything between a socket and a plaintext record. The
 * bounded read and write, the AEAD seal and open with their per-record
 * nonces, the handshake transcript, the change_cipher_spec skip, and the
 * alert that ends a connection badly. What is NOT here is any decision about
 * what a record MEANS -- that is the handshake's (srv/tls.c) and the
 * tickets' (srv/tlstkt.c).
 *
 * THE FATAL FLAG IS SET HERE AND READ THERE. A record layer that swallowed a
 * protocol violation would leave the handshake encrypting to a peer that has
 * already been refused, so every failure that cannot be retried marks the
 * connection and every caller checks it.
 */
#include "ct.h"
#include "ecdsa.h"
#include "gcm.h"
#include "hkdf.h"
#include "hmac.h"
#include "p256.h"
#include "posix.h" /* the one boundary beyond ISO C -- see posix.h */
#include "rand.h"
#include "sha256.h"
#include "tls.h"
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

/* CCS_MAX, KEY_UPDATE_MAX, REC_MAX, TRANS_MAX and HRR_RANDOM are in
 * srv/tlsint.h: all three files of this module speak them. */

/* ---- little helpers ---------------------------------------------------- */

/* put16 / put24 are in srv/tlsint.h: both halves of this module write
 * big-endian lengths into wire records. */

/* get16 / get24 are in srv/tlsint.h, beside put16 / put24. */

/* lib/rand.c's, with the same contract: 1 only if every byte is real
 * entropy. This file's callers already treat 0 as fatal for the handshake --
 * an ECDHE scalar or a ticket nonce from stack garbage is the whole key. */
int tls_rec_rand(uint8_t *out, size_t n)
{
   return rand_bytes(out, n);
}

/* Everything hashed into the transcript goes through here, so no message can
 * be sent or accepted without being counted.
 *
 * 1 = counted, 0 = IT DID NOT FIT, and the caller must stop.
 *
 * A RETURN VALUE, not a `c->fatal` flag read at the far end of the handshake:
 * a transcript that has already overflowed would otherwise carry on through an
 * ECDHE key exchange, an ECDSA signature over a hash of a transcript missing
 * its tail, and the certificate write, before anything looked. Every one of
 * those is attacker-triggered work done on a connection
 * that is already finished, and the signature is over a transcript that is not
 * the one the peer saw.
 *
 * c->fatal is still set, because the connection IS dead and every other exit
 * path reads it; what changed is that the caller learns immediately rather
 * than eventually. */
int tls_rec_transcript(struct tls_conn *c, const uint8_t *p, size_t n)
{
   if (c->trans_n + n > sizeof c->trans) {
      c->fatal = 1;
      return 0;
   }
   memcpy(c->trans + c->trans_n, p, n);
   c->trans_n += n;
   return 1;
}

void tls_rec_transcript_hash(struct tls_conn *c, uint8_t out[32])
{
   sha256(c->trans, c->trans_n, out);
}

/* ---- reading and writing records --------------------------------------- */

/* A read or write that failed: is it worth another attempt, or is this socket
 * finished?
 *
 * ONLY the timeout and the interrupt are worth retrying. "The socket carries
 * SO_SNDTIMEO, so a failure must be that timeout" is true right up until the
 * peer sends a reset: SIGPIPE is ignored process-wide (srv/http.c), so from
 * then on write() returns -1/EPIPE forever, `sent` never advances, and a loop
 * that retries every error never ends. A
 * stranger could complete a handshake, reset the connection, and pin a pool
 * worker at 100% CPU for the life of the process -- tls_bye() writes the close
 * alert through here, so merely hanging up was enough. Ten sockets is every
 * worker on a one-core board, permanently. */
static int retry_after(int err)
{
   return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

/* Read exactly n bytes, or give up.
 *
 * THE DEADLINE IS CHECKED ON EVERY PASS, not only after a read fails.
 * Consulted solely on the `r < 0` branch -- on the assumption that waiting
 * only happens when a read times out -- it is never reached by a peer that
 * sends ONE BYTE at a time, just often enough to keep the socket readable:
 * every read succeeds, `got` crawls, and the whole HTTPS_DEADLINE_S budget
 * goes unevaluated. The connection then holds a pool worker for as long as
 * the peer cares to keep dribbling -- ten of them is every worker on the
 * board -- and unlike a spinning loop it is invisible, because it costs no
 * CPU. srv/http.c's plain-HTTP read loop checks its deadline on every
 * iteration for the same reason. */
int tls_rec_read_all(struct tls_conn *c, uint8_t *p, size_t n,
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
 * ASSERTING ON CPU PERCENTAGE MISSES IT, because a zero-window stall does not
 * move the CPU -- the very property the comment above full_read names when
 * describing the dribble attack from the other direction. What shows this one
 * is WORKER AVAILABILITY.
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

int tls_rec_write_all(struct tls_conn *c, const uint8_t *p, size_t n)
{
   size_t sent = 0;
   while (sent < n) {
      if (c->giveup != NULL && c->giveup())
         return 0; /* out of time: the response is owed, but not forever */
      errno  = 0;
      long r = sys_send_quiet(c->fd, p + sent, n - sent);
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

int tls_rec_send(struct tls_conn *c, int type, const uint8_t *body, size_t n)
{
   uint8_t rec[REC_MAX + 512];
   if (!c->encrypted) {
      rec[0] = (uint8_t)type;
      put16(rec + 1, 0x0303);
      put16(rec + 3, (unsigned)n);
      memcpy(rec + 5, body, n);
      return tls_rec_write_all(c, rec, 5 + n);
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
    * forbidden length (past 2^32-2 blocks the counter wraps to 1 -- which is
    * J0, the tag mask -- so guessing sixteen plaintext bytes yields the tag
    * key for that key and IV, and one block later the keystream simply
    * repeats). A record is at most REC_MAX + 1 bytes, far
    * under the 2^36-32 limit, so this branch is unreachable. It is written
    * anyway because "unreachable" is a property of REC_MAX above it, not of
    * this call, and a sealer whose refusal is discarded would transmit an
    * unencrypted buffer. 0 is this file's failure answer -- full_write uses
    * it too. */
   if (aes128_gcm_seal(c->wr.key, nonce, rec, 5, pt, inner, rec + 5,
                       rec + 5 + inner) != GCM_OK)
      return 0;
   c->wr.seq++;
   return tls_rec_write_all(c, rec, 5 + inner + 16);
}

/* Read one record into c->plain. Returns its content type, or -1. */
int tls_rec_read(struct tls_conn *c, tls_giveup_fn giveup)
{
   uint8_t hdr[5];
   if (!tls_rec_read_all(c, hdr, 5, giveup))
      return -1;
   size_t n = get16(hdr + 3);
   if (n > REC_MAX + 256)
      return -1;
   if (!tls_rec_read_all(c, c->inbuf, n, giveup))
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
int tls_rec_keys(const uint8_t secret[32], struct keys *k)
{
   if (!hkdf_expand_label(secret, "key", NULL, 0, k->key, 16) ||
       !hkdf_expand_label(secret, "iv", NULL, 0, k->iv, 12))
      return 0;
   k->seq = 0;
   return 1;
}

/* The next record that is not a change_cipher_spec. Clients send those for
 * middlebox compatibility, before AND after a HelloRetryRequest, and they
 * carry no meaning for us -- but they are not handshake messages either, so
 * they must be skipped rather than rejected. */
/* The next record that means something, skipping change_cipher_spec. */
int tls_rec_next(struct tls_conn *c, tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = tls_rec_read(c, giveup);
      if (t != REC_CHANGE_CIPHER)
         return t;
      if (++skipped > CCS_MAX)
         return -1;
   }
}

/* An alert, best effort, on the way out. The connection is over either way;
 * this only decides whether the peer is told why. */
void tls_rec_alert(struct tls_conn *c, int desc)
{
   uint8_t a[2] = {ALERT_FATAL, (uint8_t)desc};
   (void)tls_rec_send(c, REC_ALERT, a, 2);
}
