/* SPDX-License-Identifier: GPL-3.0
 * tlstkt.c --- resumption tickets: sealing one, opening one, proving one
 * Copyright 2026 Jakob Kastelic
 *
 * ONE OF THE THREE FILES OF THE TLS MODULE; srv/tlsint.h says what
 * the split is and what it preserves.
 *
 * A TICKET IS A PSK THE SERVER GAVE OUT AND CANNOT REMEMBER. This server
 * keeps no session database: the ticket carries the secret, sealed under a
 * key only this process holds (g_ticket_key), with the instant it was issued
 * inside the sealed bytes. Opening one is therefore the whole of "do I know
 * this client" -- there is nothing else to look it up in -- and the binder is
 * how the client proves it holds what the ticket contains rather than merely
 * having copied the ticket from someone who did.
 *
 * THE CLOCK IS MONOTONIC, not the wall clock, and the note above tls_mono_s
 * says at length what a wall-clock ticket lifetime does on a board whose time
 * arrives from the network minutes after boot.
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

/* ---- tickets -----------------------------------------------------------
 *
 * Stateless: the ticket carries the PSK, sealed under a key only this process
 * holds, so nothing has to be remembered between connections and a restart
 * simply invalidates outstanding tickets.
 * Layout: nonce || AEAD(issued_at || psk) || tag.
 *
 * THE ISSUE TIME IS SEALED WITH IT, and checked on the way back in. The
 * ticket message on the wire advertises a 7200 s lifetime, and RFC 8446
 * 4.6.1 makes that binding on the server. Sealing the bare PSK and checking
 * only its length and tag lets a ticket resume an authenticated session for as
 * long as the PROCESS lives -- on a board that runs for months that is
 * indistinguishable from forever, and one copied ticket is a session that
 * never has to be re-authenticated.
 *
 * The clock is CLOCK_MONOTONIC (http_mono_s), not the wall clock. The ticket
 * key is per-process, so a ticket cannot outlive the process anyway, and
 * uptime is the one clock this board cannot get wrong -- it has no RTC, and
 * the wall clock jumps when NTP finally lands. */
#define TICKET_LIFETIME_S 7200

/* WHETHER THERE IS A CLOCK IS AN ANSWER, NOT A TIME.
 *
 * A FAILED CLOCK IS NOT SECOND ZERO. Spelling it that way makes it a real
 * reading nothing downstream can tell apart, and both of the places that ask
 * are about the age of a ticket:
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
 * file stays linkable on its own -- the TLS units build against lib/ and
 * nothing else, and a dependency on the HTTP layer for a clock would drag a
 * web server into the crypto vectors. */
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

size_t tls_tkt_seal(const uint8_t psk[32], uint8_t *out)
{
   uint8_t nonce[12];
   uint8_t pt[8 + 32];
   if (!tls_rec_rand(nonce, 12))
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

int tls_tkt_open(const uint8_t *t, size_t n, uint8_t psk[32])
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
int tls_tkt_binder_ok(const struct hello *h, const uint8_t *ch,
                      const uint8_t *pre, size_t pren, const uint8_t psk[32])
{
   /* FRAMING IS SETTLED BEFORE THIS RUNS. parse_hello walked the identity and
    * binder lists as bounded vectors, required each to exhaust its declared
    * length, and required one correctly framed binder per identity -- so
    * `h->binder` is entry PSK_SELECTED of a list that accounts for every
    * identity offered, not the first entry of a list that was allowed to stop
    * early. Checking one binder's width over a list nothing has counted is the
    * "accepts a prefix" defect: five identities offered, one binder validated,
    * and the four identities no MAC covered ride along.
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

enum ticket_fate tls_tkt_send(struct tls_conn *c)
{
   uint8_t nonce[8] = {0, 0, 0, 0, 0, 0, 0, 1};
   uint8_t psk[32];
   if (!hkdf_expand_label(c->res_master, "resumption", nonce, 8, psk, 32))
      return TICKET_NOT_ISSUED;
   uint8_t t[80];
   size_t tn = tls_tkt_seal(psk, t);
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
   return tls_rec_handshake(c, HS_NEW_TICKET, b, k) ? TICKET_SENT
                                                    : TICKET_WRITE_FAILED;
}

#ifdef TLS_FAULTS
/* ---- THREE DOORS FOR THE TEST BUILD, and why they are doors at all ----
 *
 * Tickets are sealed, opened and sent by static functions, and the rule under
 * test -- no monotonic clock, no ticket, in EITHER direction -- is a property
 * of those three and of nothing a client can reach from outside. A full
 * end-to-end case would need a second implementation of TLS 1.3 to hold the
 * other end of a resumption -- which is what an openssl client is borrowed
 * for, and it cannot inject a clock failure into this process.
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
   return tls_tkt_seal(psk, out);
}

int tls_fault_ticket_open(const uint8_t *t, size_t n, uint8_t psk[32])
{
   return tls_tkt_open(t, n, psk);
}

int tls_fault_send_ticket(int fd)
{
   struct tls_conn *c = tls_conn_slot();
   memset(c, 0, sizeof *c);
   c->fd = fd;
   /* Plaintext records (c->encrypted stays 0), so the test can read the
    * NewSessionTicket off the socket without holding a key. */
   memset(c->res_master, 0x5a, sizeof c->res_master);
   return tls_tkt_send(c) == TICKET_SENT;
}
#endif
