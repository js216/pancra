/* SPDX-License-Identifier: GPL-3.0
 * tls.h --- the whole of this program's TLS surface
 * Copyright 2026 Jakob Kastelic
 *
 * The whole TLS implementation is on the other side of these functions, and
 * nothing outside srv/tls.c knows how it works -- so replacing it is a
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

/* Take an accepted socket through the handshake, on this thread. Returns 1 on
 * success. A failure needs no message: port scanners and plain-HTTP probes
 * hit a public 443 constantly, and syslog is not a honeypot log. */
int tls_handshake(int fd, tls_giveup_fn giveup);

/* Read and write the connection this thread last handshook. Both return -1
 * on error or on giving up; tls_send moves all `n` bytes or fails. */
ssize_t tls_recv(void *buf, size_t n, tls_giveup_fn giveup);
ssize_t tls_send(const void *buf, size_t n, tls_giveup_fn giveup);

/* Whether whole records are already decrypted and waiting. Polling the socket
 * cannot see those, so a caller that sleeps on the fd must ask this first or
 * it will mistake a buffered request for an idle connection. */
int tls_pending(void);

/* Say goodbye, best effort: the response is already flushed and the peer may
 * well have hung up first. */
void tls_bye(void);

/* The TLS 1.3 key schedule (RFC 8446 7.1). HKDF itself is a primitive and
 * lives in lib/hkdf.c; the "tls13 " label wrapper is TLS and lives in tls.c.
 * Declared here only so the test suite can pin both to RFC 8448's published
 * handshake -- the one way to learn a key schedule is wrong without a peer
 * to disagree with you. */
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, size_t ctxn, uint8_t *out, size_t n);
void derive_secret(const uint8_t secret[32], const char *label,
                   const uint8_t *transcript, size_t tn, uint8_t out[32]);

#endif
