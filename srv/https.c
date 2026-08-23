/* SPDX-License-Identifier: GPL-3.0
 * https.c --- TLS under the same request loop (see http.h)
 * Copyright 2026 Jakob Kastelic
 *
 * The handshake happens between accept() and http_handle_conn(), and the
 * read/write hooks move request bytes through TLS records rather than the raw
 * socket. A plain-HTTP build stays available for a LAN-only deployment and
 * never links a byte of this.
 *
 * WHAT IS LEFT HERE IS POLICY, NOT CRYPTO. Which library provides TLS, and
 * how its state is kept, is srv/tls.c's business; how long this server is
 * willing to wait for a peer, and when it would rather serve somebody else,
 * is this file's. As one file the waiting rules are hard to find among the
 * crypto, and the crypto is impossible to replace.
 */
#include "https.h"
#include "http.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h> /* calloc: the pool outlives this frame */
#include <sys/time.h>
#include <unistd.h> /* close */

/* Start of the current handshake-or-request, rearmed per request
 * (http_new_request) so the deadline means one slow exchange rather than one
 * slow connection. The connection as a whole is bounded by
 * http_max_per_conn and by yielding to waiting clients. */
static _Thread_local double conn_start;

/* THE CONNECTION THIS WORKER IS SERVING, for the one callback that cannot be
 * handed it: tls.c's give-up hook takes no argument (it is consulted from
 * inside a retry loop), and the waiting rule below has to know which
 * listening socket to check. A worker serves exactly one connection at a
 * time, which is what makes a per-thread pointer exact rather than an
 * assumption -- the same reason conn_start above is per-thread. */
static _Thread_local const struct http_conn *cur_conn;

/* THE WAITING RULE, handed to srv/tls.c to consult between retries.
 *
 * A peer gets the full budget while it is the only one who wants the server.
 * Once somebody else is queued, a connection that has produced nothing is
 * dropped: an unfinished handshake has carried no data in either direction,
 * so there is nothing to lose, and one silent peer otherwise costs every
 * other client eight seconds. */
static int give_up(void)
{
   return http_mono_s() - conn_start >
              http_deadline_capped((double)HTTPS_DEADLINE_S) ||
          http_give_way(cur_conn, conn_start);
}

static ssize_t tls_read_hook(const struct http_conn *c, void *buf, size_t n)
{
   return tls_recv(c->tp_state, buf, n, give_up);
}

static ssize_t tls_write_hook(const struct http_conn *c, const void *buf,
                              size_t n)
{
   /* No give_up on the way out: the response is already owed to this client,
    * and abandoning it half-written is worse for everyone than the wait. */
   return tls_send(c->tp_state, buf, n, NULL);
}

static int buffered_hook(const struct http_conn *c)
{
   return tls_pending(c->tp_state);
}

static void new_request_hook(const struct http_conn *c)
{
   cur_conn   = c;
   conn_start = http_mono_s();
}

/* THE TLS TRANSPORT, as one named value.
 *
 * Assigned into loose globals in http.c at startup, these five convert the
 * whole PROCESS to TLS and leave http.c unable to say what its own same-named
 * write writes to. Named and handed to the pool, the transport belongs to the
 * connections this server accepts. */
static const struct http_transport tls_transport = {
    .read        = tls_read_hook,
    .write       = tls_write_hook,
    .buffered    = buffered_hook,
    .new_request = new_request_hook,
    .deadline_s  = HTTPS_DEADLINE_S,
};

/* Runs on the accepted socket before any request. Doing the handshake inside
 * the pool worker is the whole point: ten can be in flight at once on a board
 * that computes one at a time, because a handshake is mostly waiting. */
static int prepare(struct http_conn *c)
{
   cur_conn   = c;
   conn_start = http_mono_s();
   /* THE ONE AMBIENT LOOKUP, and it happens once. From here the connection
    * travels as c->tp_state and every tls.c call names it. */
   c->tp_state = tls_conn_slot();
   return tls_handshake(c->tp_state, c->fd, give_up);
}

static void finish(struct http_conn *c)
{
   tls_bye(c->tp_state);
}

int https_serve(int port, const char *cert, const char *key, const char *name,
                http_handler handle, const struct http_policy *pol, void *user)
{
   if (tls_init(cert, key, name) != 0)
      return 1;

   int srv = http_listen(port, name);
   if (srv < 0)
      return 1;

   /* The policy travels with the pool, and the pool's listening socket is
    * what a connection checks for clients queued behind it -- so the request
    * loop can stop waiting on an idle keep-alive connection the moment
    * somebody else is waiting. Neither is a process global installed by a
    * setter call that has to happen in the right order before the first
    * accept. */
   /* ON THE HEAP, NOT THIS FRAME, and http_serve says why at length: the
    * workers are detached and hold this pointer for the life of the process,
    * so a pool in a frame is safe only while that frame never returns. */
   struct http_pool *p = calloc(1, sizeof *p);
   if (!p) {
      fprintf(stderr, "sync: cannot allocate the HTTPS pool\n");
      close(srv);
      return 1;
   }
   p->srv     = srv;
   p->handle  = handle;
   p->user    = user;
   p->tp      = &tls_transport;
   p->pol     = pol ? *pol : (struct http_policy){0, 2.0, 500, HTTP_BODY_MAX};
   p->prepare = prepare;
   p->finish  = finish;
   return http_run_pool(p, HTTP_WORKERS);
}
