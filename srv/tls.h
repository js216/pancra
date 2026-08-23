/* SPDX-License-Identifier: GPL-3.0
 * tls.h --- the whole of this program's TLS surface
 * Copyright 2026 Jakob Kastelic
 *
 * The whole TLS implementation is on the other side of these functions, and
 * nothing outside the tls module knows how it works -- so replacing it is a
 * rewrite of one file rather than a search across the server.
 *
 * A session belongs to ONE connection and one thread. The configuration --
 * certificate, key, RNG and session-ticket keys -- is shared by every worker;
 * the session is not. tls_init() builds the shared half once at startup, and
 * each worker gets its own session on its first connection.
 *
 * WAITING IS THE CALLER'S POLICY, NOT THIS FILE'S. A TLS read that returns
 * "not yet" is normal, and how long to keep waiting depends on whether anyone
 * else needs the server -- which only the HTTP layer knows. So every call
 * that can block takes a `giveup` predicate and asks it between attempts.
 * Pass NULL to wait for as long as the transport allows.
 */
#ifndef PANCRA_TLS_H
#define PANCRA_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Returns nonzero when the caller has waited long enough. Consulted between
 * retries, never in the middle of one. */
typedef int (*tls_giveup_fn)(void);

/* Build the shared configuration: certificate chain, private key, RNG and
 * session tickets. Once, before any worker starts. 0 on success; the reason
 * is printed. */
int tls_init(const char *cert_pem, const char *key_pem, const char *name);

/* ONE TLS CONNECTION. Opaque: everything inside it is the module's business
 * (srv/tlsint.h, shared by its five files and nobody else).
 *
 * These five TAKE one. Acting on "the connection this thread last handshook"
 * is a sentence doing the job a parameter should: a caller cannot say which
 * connection it means, and nothing stops a hook that forgot to handshake from
 * reading a previous connection's keys. */
struct tls_conn;

/* This worker's connection slot. Storage is per WORKER and reused, because
 * the struct is several kilobytes and this runs on a 56 MB board -- but it is
 * fetched ONCE, when a connection is accepted, and passed explicitly from
 * there on. */
struct tls_conn *tls_conn_slot(void);

/* Take an accepted socket through the handshake, on this thread. Returns 1 on
 * success. A failure needs no message: port scanners and plain-HTTP probes
 * hit a public 443 constantly, and syslog is not a honeypot log. */
int tls_handshake(struct tls_conn *c, int fd, tls_giveup_fn giveup);

/* Read and write one connection. Both return -1 on error or on giving up;
 * tls_send moves all `n` bytes or fails. */
ssize_t tls_recv(struct tls_conn *c, void *buf, size_t n, tls_giveup_fn giveup);
ssize_t tls_send(struct tls_conn *c, const void *buf, size_t n,
                 tls_giveup_fn giveup);

/* Whether whole records are already decrypted and waiting. Polling the socket
 * cannot see those, so a caller that sleeps on the fd must ask this first or
 * it will mistake a buffered request for an idle connection. */
int tls_pending(struct tls_conn *c);

/* Say goodbye, best effort: the response is already flushed and the peer may
 * well have hung up first. */
void tls_bye(struct tls_conn *c);

/* The TLS 1.3 key schedule (RFC 8446 7.1). HKDF itself is a primitive and
 * lives in lib/hkdf.c; the "tls13 " label wrapper is TLS and lives in tls.c.
 * Declared here only so the test suite can pin both to RFC 8448's published
 * handshake -- the one way to learn a key schedule is wrong without a peer
 * to disagree with you.
 *
 * BOTH RETURN 1 OR 0, AND 0 LEAVES `out` UNTOUCHED. Every field of an
 * HkdfLabel has a width in RFC 8446 7.1 and every one of them is now
 * enforced; an over-long label, an over-long context, or an output length
 * HKDF cannot produce is refused rather than truncated into a derivation that
 * succeeds with the wrong inputs. Callers here pass literals and fixed sizes,
 * so a 0 means a programming error, not a peer -- but it is reported, because
 * the alternative is a session key derived from a label nobody wrote. */
int hkdf_expand_label(const uint8_t secret[32], const char *label,
                      const uint8_t *ctx, size_t ctxn, uint8_t *out, size_t n);
int derive_secret(const uint8_t secret[32], const char *label,
                  const uint8_t *transcript, size_t tn, uint8_t out[32]);

#ifdef TLS_FAULTS
/* ---- THE TEST BUILD'S DOOR ONTO SESSION TICKETS ------------------------
 *
 * Present only under -DTLS_FAULTS, alongside the injected monotonic clock
 * (TLS_FAIL_MONOTONIC, TLS_MONOTONIC_FIXED -- see srv/tls.c). A shipping
 * binary has none of this,
 * the same way nothing that ships carries srv/db.c's DB_FAULTS hooks.
 *
 * WHY A DOOR AND NOT AN END-TO-END TEST. "No monotonic clock, no ticket"
 * cannot be provoked from outside the process: clock_gettime(CLOCK_MONOTONIC)
 * does not fail on a test machine, and the peer that would have to hold the
 * other end of a resumption is openssl, in another process, where the fault
 * cannot be injected. These three let the suite seal a ticket, present it
 * back, and watch a NewSessionTicket either reach a socket or not.
 *
 * tls_fault_send_ticket runs one throwaway PLAINTEXT connection on `fd` and
 * returns 1 only when a NewSessionTicket was written, so a caller can assert
 * on the bytes as well as on the answer. It reuses this worker's connection
 * slot, which it clears first. */
size_t tls_fault_ticket_seal(const uint8_t psk[32], uint8_t *out);
int tls_fault_ticket_open(const uint8_t *t, size_t n, uint8_t psk[32]);
int tls_fault_send_ticket(int fd);
/* Fill the transcript to `pre`, offer `n` more: 1 = counted, 0 = refused.
 * Writes whether the connection was marked fatal through `fatal`. */
int tls_fault_transcript(size_t pre, size_t n, int *fatal);
/* How much the transcript holds. The size is a consequence of the largest
 * certificate chain this server will send (see TRANS_MAX in srv/tlsint.h), so
 * a suite that wants to stand one byte either side of the limit asks for it
 * rather than repeating a number that moves when the chain bound does. */
size_t tls_fault_transcript_cap(void);
#endif

#endif
