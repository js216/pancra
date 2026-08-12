/* SPDX-License-Identifier: GPL-3.0
 * http.h --- the tiny HTTP plumbing both programs sit on
 * Copyright 2026 Jakob Kastelic
 *
 * Deliberately generic and deliberately dull: an accept loop and a response
 * writer, nothing about glucose. Keeping it general keeps the server
 * genuinely small, and it is the part of them least likely ever to change.
 *
 * A small pool of workers (HTTP_WORKERS), because a CGM produces a datapoint
 * every five minutes and there is one reader. Correctness beats concurrency.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <sys/types.h> /* ssize_t */

#define HTTP_REQ_MAX  8192        /* request header bytes accepted */
#define HTTP_BODY_MAX (64 * 1024) /* body bytes accepted */
/* Seconds ONE connection may occupy the server. Requests are served one at a
 * time, so without a ceiling a client that connects and then says nothing --
 * a stalled phone, a port scanner, an attacker -- holds the whole service for
 * as long as the socket timeout allows, and a stream of them holds it for
 * ever. A client on the same LAN sends its request immediately, so this can
 * be short without ever cutting off honest traffic. */
#define HTTP_DEADLINE_S 3
/* The TLS budget is longer: the handshake alone is several round trips, and
 * an HTTPS client is by definition NOT on the LAN. */
#define HTTPS_DEADLINE_S 8

/* Write a full response. `ctype` is a MIME type; `body` may be binary. */
void http_respond(int fd, int code, const char *reason, const char *ctype,
                  const void *body, size_t n);
/* http_respond with text/plain and strlen(body). */
void http_text(int fd, int code, const char *reason, const char *body);

/* One connection: read the request, hand it to `handle`.
 *   req      NUL-terminated request text (headers, and the body that
 *            arrived with them)
 *   body     start of the body within req, and body_len its length as the
 *            Content-Length header declared (0 for GET)
 * The callback owns the reply. */
typedef void (*http_handler)(int fd, char *req, char *body, size_t body_len);

/* Bind `port` and serve forever, calling `handle` per request. `name` is
 * only for the startup log line. Returns non-zero on a setup failure. */
int http_serve(int port, const char *name, http_handler handle);

/* --- the seam https.c plugs into (plain HTTP callers can ignore) --------
 * One connection is served at a time, so a single pair of transport hooks
 * is exact: http.c moves every connection byte through these, and they
 * default to plain read(2)/write(2). https.c repoints them at the TLS
 * layer, once, at startup. `http_write` must move all `n` bytes or fail;
 * `http_deadline_s` is the per-connection budget the request loop enforces. */
extern ssize_t (*http_read)(int fd, void *buf, size_t n);
extern ssize_t (*http_write)(int fd, const void *buf, size_t n);
extern double http_deadline_s; /* budget for ONE request (https.c rearms
                                * it per request, not per connection) */
/* Serve several requests per connection. OFF by default -- the cost it saves
 * is a TLS handshake, and the cost it adds is a worker held by
 * one client, so only a server that expects many small requests from a paired
 * client should turn it on. */
extern int http_keepalive;
extern double http_idle_s;    /* how long to wait for a follow-up request */
extern int http_max_per_conn; /* requests before the connection is retired */

/* KEEP-ALIVE MUST NOT BECOME A LOCK ON THE SERVER.
 *
 * Requests are served one at a time, so a client holding a connection open
 * and saying nothing kept every other client waiting for the whole
 * connection budget -- measured at 7-8 s to first byte on a page that
 * normally takes 0.2 s, with the entire delay in the TLS handshake of the
 * client stuck behind it. Keep-alive is an optimisation, never an
 * entitlement: given the listening socket, the request loop stops waiting
 * for a follow-up the instant somebody else is queued, and closes instead.
 * Set to -1 (the default) to disable the check. */
void http_set_idle_watch(int listen_fd);
/* The value a response's `Connection:` header must carry right now, for a
 * handler that writes its own header block instead of calling http_respond.
 * Hand-writing "close" there bypasses the connection accounting and retires a
 * pooled connection the server was willing to keep. */
const char *http_conn_value(void);
/* Is another client queued on the listening socket right now? */
int http_others_waiting(void);
/* A PEER THAT HAS SENT NOTHING MUST NOT HOLD THE SERVER.
 *
 * Requests are served one at a time, and the handshake and request-read
 * budgets are seconds long because an honest client on a bad link may need
 * them. A peer that connects to a public port and then says nothing -- a
 * scanner, a probe, a half-open connection -- consumed that whole budget
 * while everyone else waited: one of them turned a 0.2 s page into 8.6 s, and
 * two of them into 16. Nothing has been received and nothing sent, so there
 * is nothing to lose by dropping it the moment somebody real is queued. This
 * grace period is how long such a peer is given before that applies. */
#define HTTP_SILENT_GRACE_S 1.0
/* True when `started` is far enough back AND another client is waiting. */
int http_give_way(double started);
/* Whether the transport is holding already-decrypted bytes. Polling the
 * socket cannot see those, so without this a buffered pipelined request
 * would look like an idle connection and be dropped. Plain HTTP never
 * buffers; https.c points this at the TLS layer's own pending count. */
extern int (*http_buffered)(void);
/* Called before each request on a connection, so a transport with a
 * per-connection deadline can rearm it per request instead. */
extern void (*http_new_request)(void);

/* The two halves of http_serve, so https.c can put a handshake between
 * them: bind+listen (returns the socket, or -1 with the error printed),
 * and read-one-request-and-dispatch on an accepted connection. */
int http_listen(int port, const char *name);
void http_handle_conn(int fd, http_handler handle);

/* How many connections may be in flight at once. Sized for what the server
 * is actually doing: waiting. One core means the computation was always
 * going to be serial, but a browser opens several connections at once and a
 * phone syncs while somebody reads a page, and none of them should wait
 * behind a client that has merely gone quiet. */
#define HTTP_WORKERS 10

/* A pool of workers, all accepting on one listening socket. `prepare` runs
 * on the accepted fd before any request (https.c does its handshake there)
 * and returns 0 to drop the connection; `finish` runs after the last one.
 * Both may be NULL. */
struct http_pool {
   int srv;
   http_handler handle;
   int (*prepare)(int fd);
   void (*finish)(int fd);
};

/* Start `nworkers - 1` threads and become the last worker. Does not return. */
int http_run_pool(struct http_pool *p, int nworkers);
/* Socket options every accepted connection gets. */
void http_accept_setup(int fd);
/* Monotonic seconds, for deadlines that must not jump with the clock. */
double http_mono_s(void);
/* Raise the accepted body size above HTTP_BODY_MAX. Call BEFORE serving:
 * the buffer is allocated once, on the first connection. `sync` needs it
 * because one PUT carries a whole day of readings, and on a 56 MB board a
 * server that never receives one should not pay for the buffer. */
void http_set_body_max(size_t n);

/* http_serve over TLS (srv/https.c, on srv/tls.c).
 * `cert` is the PEM certificate chain, leaf first; `key` its private key. */
int https_serve(int port, const char *cert, const char *key, const char *name,
                http_handler handle);

#endif
