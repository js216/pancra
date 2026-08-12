/* SPDX-License-Identifier: GPL-3.0
 * http.c --- the tiny HTTP plumbing both programs sit on (see http.h)
 * Copyright 2026 Jakob Kastelic
 */
#include "http.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* strtol */
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h> /* clock_gettime: the per-connection deadline */
#include <unistd.h>

/* Set when this response is the last on its connection, so the header says
 * so. A server that answers "Connection: keep-alive" and then closes leaves
 * the client writing into a socket that is already gone -- which surfaces as
 * a broken pipe on a request that was never served, not as the orderly reuse
 * the header promised. */
static _Thread_local int last_on_conn;

const char *http_conn_value(void)
{
   return (http_keepalive && !last_on_conn) ? "keep-alive" : "close";
}

void http_respond(int fd, int code, const char *reason, const char *ctype,
                  const void *body, size_t len)
{
   char hdr[256];
   /* no-cache: the browser must revalidate on every page auto-refresh, or a
    * cached plot would show stale data. Revalidation is cheap because the
    * pages are small and the queries are indexed -- there is no server-side
    * cache, and the claim that there was outlived the code that had one. */
   /* charset only where it means something: a GIF is bytes, and
    * "image/gif; charset=utf-8" is a claim about an encoding it does not
    * have. */
   const char *cs = strncmp(ctype, "text/", 5) ? "" : "; charset=utf-8";
   int n          = snprintf(hdr, sizeof hdr,
                             "HTTP/1.1 %d %s\r\n"
                             "Server: pancra\r\n"
                             "Content-Type: %s%s\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-cache\r\n"
                             "Connection: %s\r\n\r\n",
                             code, reason, ctype, cs, len, http_conn_value());
   if (http_write(fd, hdr, (size_t)n) < 0)
      return;
   if (len)
      (void)!http_write(fd, body, len);
}

void http_text(int fd, int code, const char *reason, const char *body)
{
   http_respond(fd, code, reason, "text/plain", body, strlen(body));
}

/* The transport seam (see http.h): every connection byte moves through
 * these, and plain read(2)/write(2) is the default. https.c repoints them. */
static ssize_t plain_read(int fd, void *buf, size_t n)
{
   return read(fd, buf, n);
}

static ssize_t plain_write(int fd, const void *buf, size_t n)
{
   return write(fd, buf, n);
}

ssize_t (*http_read)(int fd, void *buf, size_t n)        = plain_read;
ssize_t (*http_write)(int fd, const void *buf, size_t n) = plain_write;
double http_deadline_s                                   = HTTP_DEADLINE_S;
/* Serve more than one request per connection.
 *
 * A TLS handshake on this hardware costs 100-600 ms; a request costs 5. With
 * one request per connection, a client pushing 76 buckets paid 76 handshakes
 * and spent 45 seconds moving two megabytes. The work was never the transfer.
 *
 * A connection still occupies a worker while it is held, so the idle wait
 * between requests is SHORT and the number of requests per connection is
 * capped: a client with more to say reconnects, and one that has gone quiet
 * is dropped rather than waited on. With a pool that is a fairness rule
 * rather than the difference between service and none. */
int http_keepalive    = 0;
double http_idle_s    = 2.0;
int http_max_per_conn = 500;

/* Seconds since an arbitrary fixed point, monotonic: never jumps when the
 * clock is set, which a deadline must not do. */
double http_mono_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

/* The body ceiling, and the one buffer every connection is read into. It is
 * allocated once and reused: one request is served at a time by design.
 *
 * A VARIABLE, not just the constant, because `sync` accepts whole sync
 * buckets in one PUT -- a busy day of readings is larger than the 64 kB that
 * bounds one batch. Growing the constant instead would enlarge the buffer
 * for every server built on this, for a body most never receive, and on a
 * 56 MB
 * board that is real memory spent on nothing. */
static size_t body_max = HTTP_BODY_MAX;
/* PER THREAD, because a request buffer belongs to the connection being served
 * and connections are served by a pool. Allocated on that thread's first
 * request and kept for the life of the process, as it was before -- one
 * buffer per worker, not per connection. */
static _Thread_local char *req_buf;
static _Thread_local size_t req_cap;

void http_set_body_max(size_t n)
{
   body_max = n; /* takes effect on the next allocation, i.e. before serving */
}

/* One request. Returns 1 if the connection may carry another. */
static int watch_fd = -1;

static int no_buffer(void)
{
   return 0;
}

int (*http_buffered)(void) = no_buffer;

static void no_rearm(void)
{
}

void (*http_new_request)(void) = no_rearm;

void http_set_idle_watch(int listen_fd)
{
   watch_fd = listen_fd;
}

int http_give_way(double started)
{
   return http_mono_s() - started > HTTP_SILENT_GRACE_S &&
          http_others_waiting();
}

/* Is another client queued on the listening socket right now? */
int http_others_waiting(void)
{
   if (watch_fd < 0)
      return 0;
   struct pollfd p = {.fd = watch_fd, .events = POLLIN};
   return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

/* Wait for the first byte of a FOLLOW-UP request, up to `budget` seconds,
 * while watching for another client. Returns 1 if our client spoke, 0 if we
 * should hand the server over -- either because someone else is queued or
 * because the client went quiet.
 *
 * Only for follow-up requests: the first request on a connection has already
 * paid for a handshake and is always served. */
static int idle_wait(int fd, double budget)
{
   if (http_buffered())
      return 1; /* already decrypted and waiting; the socket looks idle */
   struct pollfd p[2];
   int n       = 1;
   p[0].fd     = fd;
   p[0].events = POLLIN;
   if (watch_fd >= 0) {
      p[1].fd     = watch_fd;
      p[1].events = POLLIN;
      n           = 2;
   }
   int ms = (int)(budget * 1000);
   int rc = poll(p, (nfds_t)n, ms < 0 ? 0 : ms);
   if (rc <= 0)
      return 0; /* nothing came: retire the connection */
   /* OUR client first when poll reports both. Yielding here would close the
    * connection on a request already sitting in the socket -- unread, so the
    * close is an RST, which can also destroy the response the client is
    * still reading. The waiting client loses nothing: this request is served
    * and the hand-off happens between requests instead. */
   if (p[0].revents & (POLLIN | POLLHUP | POLLERR))
      return 1;
   return 0; /* only the listener spoke: somebody is queued, give way */
}

static int serve_one(int fd, http_handler handle, int *saw_close, int followup);

void http_handle_conn(int fd, http_handler handle)
{
   int served = 0;
   for (;;) {
      int saw_close = 0;
      http_new_request();
      /* Decided BEFORE the request is served, because the response carries
       * the answer. An ACTIVE client must yield as well as an idle one: a
       * sync pushing a batch of buckets is hundreds of requests back to back,
       * and holding the connection for all of them kept a browser waiting for
       * the whole batch (measured: 13.7 s to first byte). Between requests is
       * the safe place to stop -- nothing is half-sent -- so a queued client
       * waits for one request, not one batch, and the syncing client is told
       * to reconnect rather than discovering the close by writing into it. */
      last_on_conn = !http_keepalive || served + 1 >= http_max_per_conn ||
                     http_others_waiting();
      if (!serve_one(fd, handle, &saw_close, served > 0)) {
         last_on_conn = 0;
         return;
      }
      served++;
      if (last_on_conn || saw_close) {
         last_on_conn = 0;
         return;
      }
   }
}

static int serve_one(int fd, http_handler handle, int *saw_close, int followup)
{
   if (!req_buf) {
      req_cap = HTTP_REQ_MAX + body_max;
      req_buf = malloc(req_cap);
      if (!req_buf)
         return 0; /* nothing can be served; the accept loop keeps going */
   }
   char *req     = req_buf;
   size_t got    = 0;
   char *hdr_end = NULL;
   /* The FIRST byte of a follow-up request may be a while coming (the client
    * is deciding what to send next); once it starts, the whole request must
    * arrive inside the usual deadline. */
   /* The idle budget belongs to a FOLLOW-UP request only. A first request is
    * one the client has already paid a handshake for, and giving it the
    * 2-second idle allowance instead of the full deadline dropped slow mobile
    * connections the board had just spent hundreds of milliseconds on.
    *
    * The clock starts AFTER the idle wait, so waiting for the client does not
    * eat the budget for reading what they then send. */
   if (followup && !idle_wait(fd, http_keepalive ? http_idle_s : 0.0))
      return 0;
   double startat = http_mono_s();
   while (got < req_cap - 1) {
      double waited = http_mono_s() - startat;
      if (waited > http_deadline_s)
         return 0; /* too slow to be honest: give the server back */
      /* Silent so far, and somebody is queued: this connection has neither
       * sent nor been sent anything, so dropping it costs it a reconnect and
       * costs the queued client nothing. A peer that has started sending
       * keeps its full budget. */
      if (got == 0 && http_give_way(startat))
         return 0;
      ssize_t r = http_read(fd, req + got, req_cap - 1 - got);
      if (r <= 0)
         return 0;
      got += (size_t)r;
      req[got] = '\0';
      hdr_end  = strstr(req, "\r\n\r\n");
      if (hdr_end)
         break;
      if (got >= HTTP_REQ_MAX)
         return 0; /* header blob: drop */
   }
   if (!hdr_end)
      return 0;
   size_t body_off = (size_t)(hdr_end - req) + 4;

   long clen = 0;
   for (char *h = req; h < hdr_end;) {
      if (!strncasecmp(h, "Content-Length:", 15))
         clen = strtol(h + 15, NULL, 10);
      h = strchr(h, '\n');
      if (!h)
         break;
      h++;
   }
   if (clen < 0)
      clen = 0;
   /* A client that says it is done gets closed, whatever we would prefer. */
   for (char *h = req; h < hdr_end;) {
      if (!strncasecmp(h, "Connection:", 11)) {
         /* Case-insensitive on the VALUE too: "Connection: Close" is as valid
          * as "close", and matching only the lower-case spelling left the
          * connection open against the client's wishes. */
         char *nl = strchr(h, '\n');
         for (char *v = h + 11; v && nl && v < nl; v++)
            if (!strncasecmp(v, "close", 5)) {
               *saw_close   = 1;
               last_on_conn = 1; /* so the reply says so, per RFC 9112 9.6 */
               break;
            }
      }
      h = strchr(h, '\n');
      if (!h)
         break;
      h++;
   }
   if (clen > (long)body_max) {
      /* Every path that answers and then returns 0 must mark the connection
       * closing FIRST: http_handle_conn decided keep-alive before the request
       * was read and cannot know about these. Answering "keep-alive" and then
       * closing is the exact confusion the flag exists to avoid -- and here
       * the body is refused UNREAD, so the close is an RST that can lose the
       * reply itself. */
      last_on_conn = 1;
      http_text(fd, 413, "Payload Too Large", "body too large\n");
      return 0;
   }
   /* Pull in the rest of the declared body. A read that merely TIMED OUT is
    * not the end of the body: the socket carries a 1 s timeout so the
    * deadline below is enforced rather than slept through, and treating that
    * EAGAIN as end-of-body used to abandon the request silently, before it
    * could be refused. Only a close or a hard error ends it early. */
   while (got - body_off < (size_t)clen && got < req_cap - 1) {
      if (http_mono_s() - startat > http_deadline_s)
         break;
      ssize_t r = http_read(fd, req + got, req_cap - 1 - got);
      if (r > 0) {
         got += (size_t)r;
         continue;
      }
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         continue;
      break;
   }
   if (got - body_off < (size_t)clen) {
      /* REFUSE it; do NOT hand the handler a short body as if it were whole.
       * h_bucket_put's contract is "this bucket now contains exactly these
       * lines", implemented as DELETE then insert -- so a body cut off by the
       * deadline used to commit as an authoritative deletion of every row
       * that had not yet arrived. A dropped connection was always safe (the
       * handler never ran); a SLOW one silently was not. */
      last_on_conn = 1; /* answering, then closing: say so (see the 413) */
      http_text(fd, 400, "Bad Request", "incomplete body\n");
      return 0;
   }
   /* END THE HEADER BLOCK, so a header lookup cannot walk into the body.
    *
    * srv/sync.h documents `hdr` as "the header block, NUL-terminated at the
    * blank line", and it was not: the only NUL was the one at req[got], past
    * everything, so hdr_get() scanned line by line straight through the blank
    * line and on into whatever the client had POSTed. Every header lookup
    * therefore also searched attacker-supplied body text.
    *
    * That is not merely untidy. Session cookies are SameSite=Lax, so a
    * cross-site POST arrives with NO real Cookie header at all -- and a body
    * line reading `Cookie: sid=...` was then the first and only match, which
    * hands the victim's browser a session of the attacker's choosing. The
    * CSRF token is derived from that same cookie, so csrf_guard agreed.
    *
    * hdr_end points at the "\r\n\r\n"; the body starts four bytes later, so
    * this terminator lands inside the separator and touches nothing else. */
   *hdr_end = '\0';
   handle(fd, req, req + body_off, (size_t)clen);
   /* Anything the client sent BEYOND this request stays unread in the socket
    * and would be misparsed as the next one, so a pipelined client is not
    * something to keep the connection for. */
   return got - body_off <= (size_t)clen;
}

int http_listen(int port, const char *name)
{
   signal(SIGPIPE, SIG_IGN);
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   if (srv < 0)
      return perror("socket"), -1;
   int one = 1;
   setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
   struct sockaddr_in addr = {0};
   addr.sin_family         = AF_INET;
   addr.sin_addr.s_addr    = htonl(INADDR_ANY);
   addr.sin_port           = htons((uint16_t)port);
   if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0)
      return perror("bind"), -1;
   /* A DEEP backlog, because requests are served one at a time. The queue is
    * where clients wait their turn; when it overflows the kernel does not
    * queue them and does not refuse them either -- it DROPS the SYN, and the
    * client discovers this only through TCP retransmission backoff, which is
    * seconds (1, then 2, then 4...). A page that should take 0.2 s then takes
    * 13, for no reason visible on the server at all. Queuing is cheap; being
    * dropped is not. The kernel clamps this to somaxconn. */
   if (listen(srv, 128) < 0)
      return perror("listen"), -1;
   printf("%s: listening on port %d\n", name, port);
   fflush(stdout);
   return srv;
}

void http_accept_setup(int fd)
{
   /* One second, so a blocked read returns often enough for the deadline to
    * be enforced rather than slept through. */
   struct timeval tv = {1, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

   /* Nagle off. This is a request/response protocol: every write we make is a
    * complete thought -- a handshake record, a header block, a page -- and
    * there is nothing following it to coalesce with. Left on, Nagle holds each
    * small write until the previous one is acknowledged, and the peer's
    * delayed ACK can sit on that for tens of milliseconds; a TLS handshake is
    * several such writes, so the stalls add up to more than all the curve
    * arithmetic underneath them. */
   int on = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

/* THE POOL.
 *
 * Every worker blocks in accept() on the SAME listening socket and the kernel
 * hands each connection to one of them. No dispatcher, no queue, no handoff:
 * a thread that finishes a connection goes straight back to accept.
 *
 * The point is NOT parallel computation -- the board has one core, and the
 * page handlers still take a lock because they share big scratch buffers.
 * The point is that the server spends almost all of its time WAITING: on a
 * TLS handshake, on a client that is slow, on one that has stopped talking
 * altogether. Serially, every one of those waits was the whole service
 * stopping; a silent peer cost everyone 8 seconds. With a pool the waiting
 * overlaps, and only the small serial parts remain serial. */
static void *worker(void *arg)
{
   struct http_pool *p = arg;
   for (;;) {
      int fd = accept(p->srv, NULL, NULL);
      if (fd < 0)
         continue;
      http_accept_setup(fd);
      if (p->prepare && !p->prepare(fd)) {
         close(fd);
         continue;
      }
      http_handle_conn(fd, p->handle);
      if (p->finish)
         p->finish(fd);
      close(fd);
   }
   return NULL;
}

int http_run_pool(struct http_pool *p, int nworkers)
{
   http_set_idle_watch(p->srv);
   for (int i = 1; i < nworkers; i++) {
      pthread_t t;
      if (pthread_create(&t, NULL, worker, p) == 0)
         pthread_detach(t);
      /* A pool that came up short still serves; it just serves fewer at
       * once. Refusing to start would be the worse failure. */
   }
   worker(p); /* this thread is a worker too, and never returns */
   return 1;
}

static int plain_prepare(int fd)
{
   (void)fd;
   return 1;
}

int http_serve(int port, const char *name, http_handler handle)
{
   int srv = http_listen(port, name);
   if (srv < 0)
      return 1;
   struct http_pool p = {
       .srv = srv, .handle = handle, .prepare = plain_prepare, .finish = NULL};
   return http_run_pool(&p, HTTP_WORKERS);
}
