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
#include <stdatomic.h>
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

/* The request buffer, per WORKER thread: allocated on that thread's first
 * request and kept for the life of the process -- one buffer per worker, not
 * per connection. Sized from the policy's body ceiling, which is why the
 * policy has to be installed before serving starts. */
static _Thread_local char *req_buf;
static _Thread_local size_t req_cap;

/* THE DEFAULT POLICY, for a caller that passes none. Not process state: it is
 * copied into the pool, and from there into every connection, so two servers
 * in one process are two values rather than one global they take turns
 * overwriting. */
static const struct http_policy g_pol_default = {0, 2.0, 500, HTTP_BODY_MAX};

/* Set when a response could not be written whole. Per THREAD because a worker
 * serves one connection at a time -- and it is the connection, not the
 * request, that has to end: a client holding half a body cannot be sent a
 * second response on the same socket. */
static _Thread_local int resp_failed;

int http_response_failed(void)
{
   return resp_failed;
}

const char *http_conn_value(const struct http_conn *c)
{
   int keep = c && c->pol && c->pol->keepalive;
   int last = !c || !c->last_on_conn || *c->last_on_conn;
   return (keep && !last) ? "keep-alive" : "close";
}

/* ---- THE BROWSER SECURITY POLICY EVERY RESPONSE CARRIES ----------------
 *
 * ONE PLACE, NO EXCEPTIONS, AND THAT IS THE DESIGN. Every byte this server
 * sends a client leaves through http_respond_hdr: http_respond and http_text
 * call it, the browser half's web_route flushes through it, the /v1 half's
 * two dozen http_text refusals go through it, the 405 helper goes through it,
 * and the plot GIF goes through it. There is no second response writer to
 * forget these -- which is why they are stated here and not passed in. A
 * per-route opt-in is how a header ends up on the pages somebody remembered
 * and off the ones they did not, and the pages somebody forgets are the error
 * paths.
 *
 * WHAT EACH LINE IS FOR.
 *
 * frame-ancestors 'none' CARRIES THE WEIGHT. Item 48 made every state change
 * POST-only and every one of them carries a CSRF token, so cross-site FORM
 * submission is closed. What that leaves is CLICKJACKING: a page on another
 * site puts /settings in a transparent iframe, positions it under something
 * the visitor wants to click, and the visitor -- who is signed in, whose
 * cookie is SameSite=Lax and so IS sent on a top-level-ish subresource load
 * -- clicks "Unpair", "Revoke", "Delete account" or the password form THROUGH
 * it. Every one of those requests is a genuine same-site POST from the
 * browser's point of view, carries the real session cookie, and carries the
 * real CSRF token, because the token is rendered into the framed page. The
 * token cannot tell the difference; only a rule saying "this page may not be
 * framed" can. X-Frame-Options: DENY says the same thing to the browsers that
 * predate frame-ancestors, and is kept for exactly that reason (RFC 7034 is
 * informational and CSP supersedes it, but a header nobody parses costs 23
 * bytes and one that an old browser does parse prevents the same click).
 *
 * default-src 'none' with script-src ABSENT (so it falls to 'none'): this
 * server serves NO JavaScript at all -- not a file, not a <script> tag, not
 * an inline handler; grep the page builders. So the strongest possible
 * script policy is also the accurate one, and it turns any future HTML
 * injection into inert text rather than a session theft. Medical history and
 * a year-long login are what is behind these pages.
 *
 * style-src 'unsafe-inline': the pages ARE inline style -- one <style> block
 * in page.c and `style=` attributes in home.c and plotpages.c -- and no
 * stylesheet is ever served, so 'self' would buy nothing. Named as unsafe
 * because it is: it is the one relaxation here, and it is a relaxation about
 * styling on a site with no script.
 *
 * img-src 'self': the plot GIFs, which are served from this origin.
 *
 * form-action 'self' and base-uri 'none': a <base> tag or a rewritten form
 * action injected into a page could otherwise send a password or a CSRF token
 * to another origin. Neither is reachable today; both are free.
 *
 * X-Content-Type-Options: nosniff. The browser must believe Content-Type
 * rather than guessing from the bytes -- which is what stops a stored value
 * that happens to begin like HTML from being rendered as a page from this
 * origin. IT IS ALSO THE LINE MOST LIKELY TO BREAK SOMETHING: with nosniff, a
 * GIF sent under the wrong type is not rendered at all, it is refused. This
 * server names it correctly (srv/plots.c sets "image/gif" on the image and on
 * both of its failure paths, and http_respond_hdr appends charset only to
 * types beginning "text/", so it is "image/gif" and not the charset-bearing
 * "image/gif; charset=utf-8" which some
 * browsers do reject). srv/test/synctest.sh fetches a plot and asserts on the
 * type, because "the page still renders" is not something a reader can check.
 *
 * Referrer-Policy: no-referrer, and this one is specific to what these URLs
 * CONTAIN. An invitation link is /invite/<token> -- the token IS the
 * credential, it creates an account and grants a follower access to somebody
 * else's medical record. The default referrer policy would put that whole URL
 * in the Referer header of any request the invitation page makes to another
 * origin, and into the logs at the other end. The plot pages carry dates.
 * There is no analytics here that wants a referrer, so the answer is none.
 *
 * WHY THE /v1 API GETS THEM TOO. The app is not a browser and none of this
 * means anything to it. It is sent anyway because the alternative is a flag
 * saying "this response is for a browser", and a flag is a thing a call site
 * can get wrong -- on, say, the one error path that renders in a browser
 * because somebody opened an API URL in one. 240 bytes on a reply the phone
 * makes a few hundred of per sync is not a cost worth a switch.
 *
 * NO ROUTE EXCEPTIONS EXIST TODAY. If one is ever needed it belongs here,
 * named, with the reason -- not as a second response writer. */
#define HTTP_SECURITY_HDRS                                                     \
   "Content-Security-Policy: default-src 'none'; img-src 'self'; "             \
   "style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; "          \
   "frame-ancestors 'none'\r\n"                                                \
   "X-Content-Type-Options: nosniff\r\n"                                       \
   "X-Frame-Options: DENY\r\n"                                                 \
   "Referrer-Policy: no-referrer\r\n"

void http_respond(const struct http_conn *c, int code, const char *reason,
                  const char *ctype, const void *body, size_t len)
{
   http_respond_hdr(c, code, reason, ctype, NULL, body, len);
}

void http_respond_hdr(const struct http_conn *c, int code, const char *reason,
                      const char *ctype, const char *extra, const void *body,
                      size_t len)
{
   /* 1024, not 512: `extra` carries a Location and a Set-Cookie, and the
    * browser security policy below is another 240 bytes on top of them.
    * (512 replaced 256 when the Set-Cookie arrived; the rule is unchanged --
    * a header block that does not fit is refused, not truncated, see below.) */
   char hdr[1024];
   /* no-cache: the browser must revalidate on every page auto-refresh, or a
    * cached plot would show stale data. Revalidation is cheap because the
    * pages are small and the queries are indexed -- there is no server-side
    * cache, and the claim that there was outlived the code that had one. */
   /* charset only where it means something: a GIF is bytes, and
    * "image/gif; charset=utf-8" is a claim about an encoding it does not
    * have. */
   const char *cs = strncmp(ctype, "text/", 5) ? "" : "; charset=utf-8";
   int n = snprintf(hdr, sizeof hdr,
                    "HTTP/1.1 %d %s\r\n"
                    "Server: pancra\r\n"
                    "Content-Type: %s%s\r\n"
                    "Content-Length: %zu\r\n"
                    "Cache-Control: no-cache\r\n" HTTP_SECURITY_HDRS "%s"
                    "Connection: %s\r\n\r\n",
                    code, reason, ctype, cs, len, extra ? extra : "",
                    http_conn_value(c));
   /* A TRUNCATED HEADER BLOCK IS A MALFORMED RESPONSE, and snprintf truncates
    * silently. Unchecked, an over-long Location would emit a header the client
    * cannot parse and a Content-Length promising a body that then arrives
    * anyway -- which is a response-splitting shape, not merely a broken page.
    * Refuse instead: the connection closes and the client retries. */
   if (n < 0 || n >= (int)sizeof hdr) {
      resp_failed = 1;
      return;
   }
   if (c->tp->write(c, hdr, (size_t)n) < 0) {
      resp_failed = 1;
      return;
   }
   /* THE BODY RESULT IS NOT OPTIONAL. It was discarded with `(void)!`, so a
    * response that failed halfway looked exactly like one that completed --
    * and on a keep-alive connection the next request was then answered on top
    * of a body the client is still expecting bytes of. */
   if (len && c->tp->write(c, body, len) < 0)
      resp_failed = 1;
}

void http_text(const struct http_conn *c, int code, const char *reason,
               const char *body)
{
   http_respond(c, code, reason, "text/plain", body, strlen(body));
}

/* THE METHODS THIS SERVER IMPLEMENTS, and their spelling on the wire. ONE
 * table: the bit-to-name direction writes every `Allow` header and the
 * name-to-bit direction classifies every request, so a method cannot be
 * recognised on the way in and unnameable on the way out. HEAD is here so it
 * can be refused BY NAME -- see http.h for why it is refused at all. */
static const struct {
   unsigned bit;
   const char *name;
} HTTP_METHODS[] = {
    {HTTP_M_GET,  "GET" },
    {HTTP_M_HEAD, "HEAD"},
    {HTTP_M_POST, "POST"},
    {HTTP_M_PUT,  "PUT" },
};

#define NHTTP_METHODS ((int)(sizeof HTTP_METHODS / sizeof HTTP_METHODS[0]))

unsigned http_method_bit(const char *method)
{
   if (!method)
      return 0;
   /* CASE-SENSITIVE, and exactly so: RFC 9110 9.1 says method names are
    * case-sensitive tokens, so "get" and "Get" are not GET. Matching them
    * loosely would give every route a second spelling for its safe method and
    * -- worse, given what this function gates -- a second spelling for its
    * unsafe one. */
   for (int i = 0; i < NHTTP_METHODS; i++)
      if (!strcmp(HTTP_METHODS[i].name, method))
         return HTTP_METHODS[i].bit;
   return 0; /* not implemented here, so no route can allow it */
}

void http_allow_list(unsigned allow, char *out, size_t cap)
{
   if (!cap)
      return;
   out[0]   = '\0';
   size_t n = 0;
   for (int i = 0; i < NHTTP_METHODS; i++) {
      if (!(allow & HTTP_METHODS[i].bit))
         continue;
      /* BOUNDED, and a name that does not fit whole is not written at all: a
       * truncated method name in an Allow header is a method that does not
       * exist, and a client is entitled to believe the header. */
      const char *sep = n ? ", " : "";
      size_t need     = strlen(sep) + strlen(HTTP_METHODS[i].name);
      if (n + need + 1 > cap)
         return;
      memcpy(out + n, sep, strlen(sep));
      n += strlen(sep);
      memcpy(out + n, HTTP_METHODS[i].name, strlen(HTTP_METHODS[i].name));
      n += strlen(HTTP_METHODS[i].name);
      out[n] = '\0';
   }
}

void http_method_not_allowed(const struct http_conn *c, unsigned allow)
{
   /* Every method this server knows, comma-separated, is 26 characters. */
   char list[64], extra[96], body[80];
   http_allow_list(allow, list, sizeof list);
   /* THE HEADER IS THE ANSWER; the body only repeats it for a person reading
    * with curl. Both come from `allow`, so there is no way to refuse a method
    * while advertising a set that does not match the refusal. An empty mask
    * yields "Allow:" with no methods, which RFC 9110 10.2.1 defines as "this
    * resource allows none" -- the honest answer if a route is ever declared
    * with no methods at all, rather than a header quietly left off. */
   (void)snprintf(extra, sizeof extra, "Allow: %s\r\n", list);
   (void)snprintf(body, sizeof body, "%s\n", list);
   http_respond_hdr(c, 405, "Method Not Allowed", "text/plain", extra, body,
                    strlen(body));
}

/* ---- THE REQUEST LINE, SPLIT ON ONE GRAMMAR AND NO OTHER ---------------
 *
 * See http.h for the shapes this replaces and for why a second parser in
 * front of this one makes them a smuggling primitive rather than untidiness.
 * What follows is RFC 9112 3 read literally.
 *
 * tchar, from RFC 9110 5.6.2: the characters a token may be built from. It is
 * spelled out rather than approximated with isalnum() plus a guess, because
 * the whole value of this function is that its language is EXACTLY the
 * specified one -- a parser that accepts one character the spec does not is
 * back to being a parser somebody else may disagree with. Note that isalnum()
 * is also locale-dependent, and a locale is not something a wire format has.
 */
static int is_tchar(unsigned char ch)
{
   if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
       (ch >= '0' && ch <= '9'))
      return 1;
   return strchr("!#$%&'*+-.^_`|~", (char)ch) != NULL && ch != '\0';
}

enum reqline http_reqline(const char *req, char *method, size_t mcap,
                          char *target, size_t tcap)
{
   /* EMPTIED FIRST, on every path. A caller that somehow ignored the return
    * -- the reason this is warn_unused_result -- then routes on "" and gets a
    * 404, rather than on whatever half of a request line happened to parse.
    * The old code left `target` holding "/\r\nHost:" in exactly that case. */
   if (method && mcap)
      method[0] = '\0';
   if (target && tcap)
      target[0] = '\0';
   /* Both buffers are REQUIRED. There is no validate-only mode, because a
    * caller that wanted only the verdict would be a caller that then splits
    * the line itself -- a second parser, which is the whole defect. */
   if (!req || !method || !mcap || !target || !tcap)
      return REQL_BAD;

   /* method = 1*tchar, then exactly ONE space. Two spaces ("GET  /") used to
    * produce an EMPTY target, because the second strchr found the space that
    * had already been consumed as the separator. */
   size_t m = 0;
   while (is_tchar((unsigned char)req[m]))
      m++;
   if (m == 0 || req[m] != ' ')
      return REQL_BAD;

   /* request-target: RFC 9112 3.2 -- for this server always origin-form, so
    * one or more visible characters with no space in them. The bound is
    * stated positively (VCHAR, 0x21..0x7E) rather than as "not a space", so a
    * control character, a DEL, a bare CR or LF, or a raw NUL ends the target
    * and is then refused by the check for the separator. That matters: a CR
    * inside the target is how the "GET /\r\nHost: x" shape got through, and a
    * high byte (0x80..0xFF) has no meaning in a target that must already be
    * percent-encoded (RFC 3986 2.1), so it is refused here rather than
    * decoded differently by two parsers. */
   const char *t = req + m + 1;
   size_t tl     = 0;
   while ((unsigned char)t[tl] >= 0x21 && (unsigned char)t[tl] <= 0x7e)
      tl++;
   if (tl == 0 || t[tl] != ' ')
      return REQL_BAD;

   /* HTTP-version = "HTTP" "/" DIGIT "." DIGIT, RFC 9112 2.3, and then CRLF
    * and nothing else at all. This is the check that did not exist: whatever
    * followed the second space was skipped to the next "\n" and never read,
    * so "HTTP/1.1 anything" and "HTTP/9.9" and "http/1.1" were all served.
    *
    * Each index is reached only after the one before it was proved not to be
    * the NUL terminator, so this walks off the end of no buffer however short
    * the request is. */
   const char *v = t + tl + 1;
   if (strncmp(v, "HTTP/", 5) != 0)
      return REQL_BAD;
   if (v[5] < '0' || v[5] > '9')
      return REQL_BAD;
   if (v[6] != '.')
      return REQL_BAD;
   if (v[7] < '0' || v[7] > '9')
      return REQL_BAD;
   /* CRLF, not "\n". A bare LF is the other half of the smuggling pair: RFC
    * 9112 2.2 permits a recipient to accept a bare LF as a line terminator,
    * and that permission is exactly what lets two implementations frame the
    * same bytes differently, so this one does not take it. */
   if (v[8] != '\r' || v[9] != '\n')
      return REQL_BAD;

   /* A WELL-FORMED VERSION THIS SERVER DOES NOT SPEAK IS ITS OWN ANSWER, and
    * it is 505 rather than 400: the client's message was not malformed, it
    * asked for something we decline. Told apart so that a probe cannot learn
    * anything from one refusal that it could not from the other, and so that
    * the test for "HTTP/1.2 is refused" cannot be satisfied by the grammar
    * check accidentally rejecting it for some other reason. */
   if (v[5] != '1' || v[7] != '1')
      return REQL_VERSION;

   /* CAPACITY LAST, so a request that is refused for its SHAPE is refused as
    * a shape: an over-long target with "HTTP/1.2" after it is a bad version,
    * not a long URI, and either answer alone would hide the other. */
   if (m >= mcap)
      return REQL_METHOD_LONG;
   if (tl >= tcap)
      return REQL_TARGET_LONG;
   memcpy(method, req, m);
   method[m] = '\0';
   memcpy(target, t, tl);
   target[tl] = '\0';
   return REQL_OK;
}

/* The refusal each answer earns, in one place: serve_one closes the
 * connection after any of them, because a request line this server could not
 * agree with a proxy about is a connection whose framing is no longer
 * trustworthy -- see RFC 9112 9.6, and see http.h for the attack. */
static void reqline_refuse(const struct http_conn *c, enum reqline rl)
{
   switch (rl) {
      case REQL_OK: return; /* not a refusal; named so -Wswitch-enum is happy */
      case REQL_METHOD_LONG:
         http_text(c, 501, "Not Implemented", "unsupported method\n");
         return;
      case REQL_TARGET_LONG:
         http_text(c, 414, "URI Too Long", "request target too long\n");
         return;
      case REQL_VERSION:
         http_text(c, 505, "HTTP Version Not Supported",
                   "this server speaks HTTP/1.1 only\n");
         return;
      case REQL_BAD:
         http_text(c, 400, "Bad Request", "bad request line\n");
         return;
   }
   http_text(c, 400, "Bad Request", "bad request line\n");
}

/* THE PLAIN TRANSPORT. Named and const, rather than four mutable globals a
 * second module reaches over and overwrites at startup. */
static ssize_t plain_read(const struct http_conn *c, void *buf, size_t n)
{
   return read(c->fd, buf, n);
}

/* ALL THE BYTES, OR AN ERROR -- which is what http.h has always promised and
 * what tls_send already did.
 *
 * This was one write(2). On a blocking socket that is usually the whole
 * buffer, which is why it looked fine: the short write needs BACKPRESSURE.
 * Every accepted connection carries SO_SNDTIMEO of one second (see
 * http_accept_setup), so a client that stops reading -- a phone that loses
 * signal mid-response, a browser fetching a plot over a slow link -- makes
 * write() return the bytes it managed and no more. The caller then went on to
 * the next thing, so the client received a response whose body stops in the
 * middle while the Content-Length still claims the whole of it. On a
 * keep-alive connection the remainder is read as the START of the next
 * response.
 *
 * The loop is deadline-bounded rather than infinite: a peer that never drains
 * must not hold a worker for the life of the process. Each pass already
 * sleeps up to a second inside write(), so the deadline is checked between
 * passes rather than slept on separately. */
/* ALL THE BYTES, OR AN ERROR -- and every wait bounded by this function
 * itself.
 *
 * It was one write(2), which on a blocking socket usually moves everything;
 * the defect only appeared under backpressure, as a positive short count the
 * caller read as success -- a body that stops mid-sentence under a
 * Content-Length promising the rest.
 *
 * The loop that replaced it was still not self-sufficient: it retried on
 * EAGAIN, and EAGAIN only ever arrived because ACCEPTED sockets carry a
 * one-second SO_SNDTIMEO. On any fd that had not been through
 * http_accept_setup -- a socket built by hand, a transport wired up in a test,
 * an fd whose options a future change forgets -- write(2) simply blocked in
 * the kernel and the monotonic deadline below was never reached. A deadline
 * that depends on somebody else having set a socket option is not a deadline.
 *
 * So: MSG_DONTWAIT makes each send return rather than block, and poll() waits
 * for room with what is LEFT of the budget. The socket options are now an
 * optimisation, not the mechanism. MSG_NOSIGNAL because a peer that hung up
 * mid-response is an error to return, never a signal that kills the server.
 */
static ssize_t plain_write(const struct http_conn *c, const void *buf, size_t n)
{
   const unsigned char *p = buf;
   size_t sent            = 0;
   double deadline        = http_mono_s() + HTTP_DEADLINE_S;
   while (sent < n) {
      ssize_t w = send(c->fd, p + sent, n - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (w > 0) {
         sent += (size_t)w;
         continue;
      }
      /* EINTR is not backpressure: retry immediately, without spending any of
       * the budget on it. */
      if (w < 0 && errno == EINTR)
         continue;
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         /* The peer's window is full. Wait for room, for AT MOST what is left
          * of the budget -- so the total time spent here is bounded by
          * HTTP_DEADLINE_S whatever the peer does, including doing nothing at
          * all for ever. */
         double left = deadline - http_mono_s();
         if (left <= 0)
            return -1;
         struct pollfd pf = {.fd = c->fd, .events = POLLOUT, .revents = 0};
         int ms           = (int)(left * 1000.0) + 1;
         int rc           = poll(&pf, 1, ms);
         if (rc < 0 && errno == EINTR)
            continue;
         if (rc <= 0)
            return -1; /* the budget is gone, or the fd cannot be waited on */
         continue;
      }
      return -1; /* EPIPE, ECONNRESET, anything else: the connection is done */
   }
   return (ssize_t)n;
}

static int plain_buffered(const struct http_conn *c)
{
   (void)c;
   return 0; /* plain HTTP decrypts nothing, so it holds nothing */
}

static void plain_new_request(const struct http_conn *c)
{
   (void)c;
}

const struct http_transport http_transport_plain = {
    .read        = plain_read,
    .write       = plain_write,
    .buffered    = plain_buffered,
    .new_request = plain_new_request,
    .deadline_s  = HTTP_DEADLINE_S,
};

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

int http_give_way(const struct http_conn *c, double started)
{
   return http_mono_s() - started > HTTP_SILENT_GRACE_S &&
          http_others_waiting(c);
}

/* Is another client queued on the listening socket right now? The socket
 * comes from the CONNECTION, which got it from its pool -- it used to be a
 * file-scope int set by a call the caller had to remember to make before
 * serving, and forgetting it silently disabled the whole fairness rule. */
int http_others_waiting(const struct http_conn *c)
{
   if (!c || c->watch_fd < 0)
      return 0;
   struct pollfd p = {.fd = c->watch_fd, .events = POLLIN};
   return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

/* Wait for the first byte of a FOLLOW-UP request, up to `budget` seconds,
 * while watching for another client. Returns 1 if our client spoke, 0 if we
 * should hand the server over -- either because someone else is queued or
 * because the client went quiet.
 *
 * Only for follow-up requests: the first request on a connection has already
 * paid for a handshake and is always served. */
static int idle_wait(const struct http_conn *c, double budget)
{
   if (c->tp->buffered(c))
      return 1; /* already decrypted and waiting; the socket looks idle */
   struct pollfd p[2];
   int n       = 1;
   p[0].fd     = c->fd;
   p[0].events = POLLIN;
   if (c->watch_fd >= 0) {
      p[1].fd     = c->watch_fd;
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

static int serve_one(const struct http_conn *c, http_handler handle, void *user,
                     int *saw_close, int followup);

void http_handle_conn(const struct http_conn *c, http_handler handle,
                      void *user)
{
   int served = 0;
   for (;;) {
      int saw_close = 0;
      /* The flag c->last_on_conn points at (see http_conn_value). */
      c->tp->new_request(c);
      /* Decided BEFORE the request is served, because the response carries
       * the answer. An ACTIVE client must yield as well as an idle one: a
       * sync pushing a batch of buckets is hundreds of requests back to back,
       * and holding the connection for all of them kept a browser waiting for
       * the whole batch (measured: 13.7 s to first byte). Between requests is
       * the safe place to stop -- nothing is half-sent -- so a queued client
       * waits for one request, not one batch, and the syncing client is told
       * to reconnect rather than discovering the close by writing into it. */
      *c->last_on_conn = !c->pol->keepalive ||
                         served + 1 >= c->pol->max_per_conn ||
                         http_others_waiting(c);
      if (!serve_one(c, handle, user, &saw_close, served > 0))
         return;
      served++;
      if (*c->last_on_conn || saw_close)
         return;
   }
}

static int serve_one(const struct http_conn *c, http_handler handle, void *user,
                     int *saw_close, int followup)
{
   if (!req_buf) {
      req_cap = HTTP_REQ_MAX + c->pol->body_max;
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
   if (followup && !idle_wait(c, c->pol->keepalive ? c->pol->idle_s : 0.0))
      return 0;
   double startat = http_mono_s();
   while (got < req_cap - 1) {
      double waited = http_mono_s() - startat;
      if (waited > c->tp->deadline_s)
         return 0; /* too slow to be honest: give the server back */
      /* Silent so far, and somebody is queued: this connection has neither
       * sent nor been sent anything, so dropping it costs it a reconnect and
       * costs the queued client nothing. A peer that has started sending
       * keeps its full budget. */
      if (got == 0 && http_give_way(c, startat))
         return 0;
      ssize_t r = c->tp->read(c, req + got, req_cap - 1 - got);
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

   /* ---- THE REQUEST LINE, BEFORE A SINGLE HEADER IS LOOKED AT ----
    *
    * "Before header parsing" is the load-bearing part. The header loop below
    * starts at `req`, so it reads the request line as though it were a header
    * -- and the Content-Length and Connection loops after it walk the same
    * bytes. A request line this server and a fronting proxy would frame
    * differently must be refused before any of that has an opinion about it,
    * or the refusal is taken on a message the two ends have already read
    * differently. See http.h for the grammar and for the attack.
    *
    * AND THE CONNECTION CLOSES: returning 0 ends it, and the flag is set
    * first so the refusal itself says "Connection: close" rather than
    * promising a keep-alive it is about to break (the same rule as the 413
    * and the short-body 400 below). Framing is what has just failed; there is
    * nothing on this socket that can be trusted to be a next request. */
   char rl_method[HTTP_METHOD_MAX], rl_target[HTTP_TARGET_MAX];
   enum reqline rl = http_reqline(req, rl_method, sizeof rl_method, rl_target,
                                  sizeof rl_target);
   if (rl != REQL_OK) {
      *c->last_on_conn = 1;
      reqline_refuse(c, rl);
      return 0;
   }

   size_t clen = 0;
   int have_cl = 0;
   for (char *h = req; h < hdr_end;) {
      char *nl = strchr(h, '\n');
      if (!nl || nl > hdr_end + 1 || nl == h || nl[-1] != '\r' ||
          (h != req && (*h == ' ' || *h == '\t'))) {
         *c->last_on_conn = 1;
         http_text(c, 400, "Bad Request", "malformed headers\n");
         return 0;
      }
      if (!strncasecmp(h, "Transfer-Encoding:", 18)) {
         *c->last_on_conn = 1;
         http_text(c, 400, "Bad Request", "transfer encoding unsupported\n");
         return 0;
      }
      if (!strncasecmp(h, "Content-Length:", 15)) {
         if (have_cl) {
            *c->last_on_conn = 1;
            http_text(c, 400, "Bad Request", "duplicate content length\n");
            return 0;
         }
         char *p = h + 15;
         while (p < nl - 1 && (*p == ' ' || *p == '\t'))
            p++;
         if (p == nl - 1 || *p < '0' || *p > '9') {
            *c->last_on_conn = 1;
            http_text(c, 400, "Bad Request", "bad content length\n");
            return 0;
         }
         size_t v = 0;
         while (p < nl - 1 && *p >= '0' && *p <= '9') {
            unsigned d = (unsigned)(*p++ - '0');
            if (v > (SIZE_MAX - d) / 10U) {
               *c->last_on_conn = 1;
               http_text(c, 400, "Bad Request", "bad content length\n");
               return 0;
            }
            v = (v * 10U) + d;
         }
         while (p < nl - 1 && (*p == ' ' || *p == '\t'))
            p++;
         if (p != nl - 1) {
            *c->last_on_conn = 1;
            http_text(c, 400, "Bad Request", "bad content length\n");
            return 0;
         }
         clen    = v;
         have_cl = 1;
      }
      h = nl + 1;
   }
   /* A client that says it is done gets closed, whatever we would prefer. */
   for (char *h = req; h < hdr_end;) {
      if (!strncasecmp(h, "Connection:", 11)) {
         /* Case-insensitive on the VALUE too: "Connection: Close" is as valid
          * as "close", and matching only the lower-case spelling left the
          * connection open against the client's wishes. */
         char *nl = strchr(h, '\n');
         for (char *v = h + 11; v && nl && v < nl; v++)
            if (!strncasecmp(v, "close", 5)) {
               *saw_close = 1;
               *c->last_on_conn =
                   1; /* so the reply says so, per RFC 9112 9.6 */
               break;
            }
      }
      h = strchr(h, '\n');
      if (!h)
         break;
      h++;
   }
   if (clen > c->pol->body_max) {
      /* Every path that answers and then returns 0 must mark the connection
       * closing FIRST: http_handle_conn decided keep-alive before the request
       * was read and cannot know about these. Answering "keep-alive" and then
       * closing is the exact confusion the flag exists to avoid -- and here
       * the body is refused UNREAD, so the close is an RST that can lose the
       * reply itself. */
      *c->last_on_conn = 1;
      http_text(c, 413, "Payload Too Large", "body too large\n");
      return 0;
   }
   /* Pull in the rest of the declared body. A read that merely TIMED OUT is
    * not the end of the body: the socket carries a 1 s timeout so the
    * deadline below is enforced rather than slept through, and treating that
    * EAGAIN as end-of-body used to abandon the request silently, before it
    * could be refused. Only a close or a hard error ends it early. */
   while (got - body_off < clen && got < req_cap - 1) {
      if (http_mono_s() - startat > c->tp->deadline_s)
         break;
      ssize_t r = c->tp->read(c, req + got, req_cap - 1 - got);
      if (r > 0) {
         got += (size_t)r;
         continue;
      }
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         continue;
      break;
   }
   if (got - body_off < clen) {
      /* REFUSE it; do NOT hand the handler a short body as if it were whole.
       * h_bucket_put's contract is "this bucket now contains exactly these
       * lines", implemented as DELETE then insert -- so a body cut off by the
       * deadline used to commit as an authoritative deletion of every row
       * that had not yet arrived. A dropped connection was always safe (the
       * handler never ran); a SLOW one silently was not. */
      *c->last_on_conn = 1; /* answering, then closing: say so (see the 413) */
      http_text(c, 400, "Bad Request", "incomplete body\n");
      return 0;
   }
   /* END THE HEADER BLOCK, so a header lookup cannot walk into the body.
    *
    * srv/proto.h documents `hdr` as "the header block, NUL-terminated at the
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
   *hdr_end    = '\0';
   resp_failed = 0; /* per request; the connection ends if it is set */
   handle(c, req, req + body_off, clen, user);
   /* A RESPONSE THAT DID NOT GO OUT WHOLE ENDS THE CONNECTION. The client is
    * still owed the rest of a body it has a Content-Length for, so anything
    * written next would be read as part of it. */
   if (resp_failed)
      return 0;
   /* Anything the client sent BEYOND this request stays unread in the socket
    * and would be misparsed as the next one, so a pipelined client is not
    * something to keep the connection for. */
   return got - body_off <= clen;
}

int http_listen(int port, const char *name)
{
   signal(SIGPIPE, SIG_IGN);
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   if (srv < 0)
      return perror("socket"), -1;
   int one = 1;
   if (setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
      perror("sync: optional SO_REUSEADDR");
   struct sockaddr_in addr = {0};
   addr.sin_family         = AF_INET;
   addr.sin_addr.s_addr    = htonl(INADDR_ANY);
   addr.sin_port           = htons((uint16_t)port);
   if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0)
      goto fail_bind;
   /* A DEEP backlog, because requests are served one at a time. The queue is
    * where clients wait their turn; when it overflows the kernel does not
    * queue them and does not refuse them either -- it DROPS the SYN, and the
    * client discovers this only through TCP retransmission backoff, which is
    * seconds (1, then 2, then 4...). A page that should take 0.2 s then takes
    * 13, for no reason visible on the server at all. Queuing is cheap; being
    * dropped is not. The kernel clamps this to somaxconn. */
   if (listen(srv, 128) < 0)
      goto fail_listen;
   printf("%s: listening on port %d\n", name, port);
   fflush(stdout);
   return srv;
fail_listen: {
   int e = errno;
   perror("listen");
   close(srv);
   errno = e;
   return -1;
}
fail_bind: {
   int e = errno;
   perror("bind");
   close(srv);
   errno = e;
   return -1;
}
}

int http_accept_setup(int fd)
{
   /* One second, so a blocked read returns often enough for the deadline to
    * be enforced rather than slept through. */
   struct timeval tv = {1, 0};
   if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) {
      perror("sync: required socket timeout");
      return 0;
   }

   /* Nagle off. This is a request/response protocol: every write we make is a
    * complete thought -- a handshake record, a header block, a page -- and
    * there is nothing following it to coalesce with. Left on, Nagle holds each
    * small write until the previous one is acknowledged, and the peer's
    * delayed ACK can sit on that for tens of milliseconds; a TLS handshake is
    * several such writes, so the stalls add up to more than all the curve
    * arithmetic underneath them. */
   int on = 1;
   if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) < 0)
      perror("sync: optional TCP_NODELAY");
   return 1;
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
   static _Atomic long next_accept_log;
   for (;;) {
      int fd = accept(p->srv, NULL, NULL);
      if (fd < 0) {
         int e = errno;
         if (e == EINTR || e == ECONNABORTED)
            continue;
         if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM) {
            long now  = (long)time(NULL);
            long next = atomic_load(&next_accept_log);
            if (now >= next && atomic_compare_exchange_strong(&next_accept_log,
                                                              &next, now + 30))
               fprintf(stderr, "sync: accept temporarily unavailable: %s\n",
                       strerror(e));
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
            nanosleep(&pause, NULL);
            continue;
         }
         fprintf(stderr, "sync: accept failed permanently: %s\n", strerror(e));
         return NULL;
      }
      if (!http_accept_setup(fd)) {
         close(fd);
         continue;
      }
      /* EVERYTHING THIS CONNECTION NEEDS, handed to it at accept time: the
       * transport, the policy, the socket to check for waiting clients, and
       * its own close flag. Nothing here is looked up in a global, so a
       * second pool in this process is simply a second value. */
      int last_on_conn      = 0;
      struct http_conn conn = {.fd  = fd,
                               .tp  = p->tp ? p->tp : &http_transport_plain,
                               .pol = &p->pol,
                               .watch_fd     = p->srv,
                               .last_on_conn = &last_on_conn};
      if (p->prepare && !p->prepare(&conn)) {
         close(fd);
         continue;
      }
      http_handle_conn(&conn, p->handle, p->user);
      if (p->finish)
         p->finish(&conn);
      close(fd);
   }
   return NULL;
}

int http_run_pool(struct http_pool *p, int nworkers)
{
   int started = 1; /* the calling thread below */
   for (int i = 1; i < nworkers; i++) {
      pthread_t t;
      int e = pthread_create(&t, NULL, worker, p);
      if (e == 0) {
         pthread_detach(t);
         started++;
      } else {
         fprintf(stderr, "sync: cannot start HTTP worker %d: %s\n", i + 1,
                 strerror(e));
      }
   }
   fprintf(stderr, "sync: %d of %d HTTP workers started\n", started, nworkers);
   if (started < 2) {
      fprintf(stderr, "sync: refusing to serve without two HTTP workers\n");
      close(p->srv);
      return 1;
   }
   worker(p); /* this thread is a worker too, and never returns */
   return 1;
}

static int plain_prepare(struct http_conn *c)
{
   (void)c; /* a plain socket needs no per-connection state */
   return 1;
}

int http_serve(int port, const char *name, http_handler handle,
               const struct http_policy *pol, void *user)
{
   int srv = http_listen(port, name);
   if (srv < 0)
      return 1;
   /* The policy is COPIED into the pool, and from there into each
    * connection. There is no setter and no ordering rule to remember: a
    * server that has not been given one gets the default. */
   struct http_pool p = {.srv     = srv,
                         .handle  = handle,
                         .user    = user,
                         .pol     = pol ? *pol : g_pol_default,
                         .prepare = plain_prepare,
                         .finish  = NULL};
   return http_run_pool(&p, HTTP_WORKERS);
}
