/* SPDX-License-Identifier: GPL-3.0
 * https.c --- TLS under the same request loop (see http.h)
 * Copyright 2026 Jakob Kastelic
 *
 * The handshake happens between accept() and http_handle_conn(), and the
 * read/write hooks move request bytes through TLS records instead of the raw
 * socket. A plain-HTTP build stays available for a LAN-only deployment and
 * never links a byte of this.
 *
 * WHAT IS LEFT HERE IS POLICY, NOT CRYPTO. Which library provides TLS, and
 * how its state is kept, is srv/tls.c's business; how long this server is
 * willing to wait for a peer, and when it would rather serve somebody else,
 * is this file's. They used to be the same file, which made the waiting rules
 * hard to find among the crypto, and the crypto impossible to replace.
 */
#include "http.h"
#include "tls.h"
#include <sys/time.h>

/* Start of the current handshake-or-request, rearmed per request
 * (http_new_request) so the deadline means one slow exchange rather than one
 * slow connection. The connection as a whole is bounded by
 * http_max_per_conn and by yielding to waiting clients. */
static _Thread_local double conn_start;

/* THE WAITING RULE, handed to srv/tls.c to consult between retries.
 *
 * A peer gets the full budget while it is the only one who wants the server.
 * Once somebody else is queued, a connection that has produced nothing is
 * dropped: an unfinished handshake has carried no data in either direction,
 * so there is nothing to lose, and one silent peer used to cost every other
 * client eight seconds. */
static int give_up(void)
{
   return http_mono_s() - conn_start > http_deadline_s ||
          http_give_way(conn_start);
}

static ssize_t tls_read_hook(int fd, void *buf, size_t n)
{
   (void)fd;
   return tls_recv(buf, n, give_up);
}

static ssize_t tls_write_hook(int fd, const void *buf, size_t n)
{
   (void)fd;
   /* No give_up on the way out: the response is already owed to this client,
    * and abandoning it half-written is worse for everyone than the wait. */
   return tls_send(buf, n, NULL);
}

static int buffered_hook(void)
{
   return tls_pending();
}

static void new_request_hook(void)
{
   conn_start = http_mono_s();
}

/* Runs on the accepted socket before any request. Doing the handshake inside
 * the pool worker is the whole point: ten can be in flight at once on a board
 * that computes one at a time, because a handshake is mostly waiting. */
static int prepare(int fd)
{
   conn_start = http_mono_s();
   return tls_handshake(fd, give_up);
}

static void finish(int fd)
{
   (void)fd;
   tls_bye();
}

int https_serve(int port, const char *cert, const char *key, const char *name,
                http_handler handle)
{
   if (tls_init(cert, key, name) != 0)
      return 1;

   /* From here on every connection is TLS: install the hooks once. */
   http_read        = tls_read_hook;
   http_write       = tls_write_hook;
   http_deadline_s  = HTTPS_DEADLINE_S;
   http_buffered    = buffered_hook;
   http_new_request = new_request_hook;

   int srv = http_listen(port, name);
   if (srv < 0)
      return 1;
   /* So the request loop can stop waiting on an idle keep-alive connection
    * the moment another client is queued behind it. */
   http_set_idle_watch(srv);
   struct http_pool p = {
       .srv = srv, .handle = handle, .prepare = prepare, .finish = finish};
   return http_run_pool(&p, HTTP_WORKERS);
}
