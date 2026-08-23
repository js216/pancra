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

#include "compiler.h" /* PANCRA_MUST_USE: the annotation, portably */

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

/* THE DEPLOYMENT'S CEILING OVER EITHER OF THOSE, and it can only SHORTEN.
 *
 * Both numbers above are the transport's own, and both are the only thing
 * that ends a request whose peer declares a body and then stops sending it --
 * so they are also the only way to observe what the server does with one, and
 * observing it otherwise means waiting out the production number. Set
 * PANCRA_HTTP_DEADLINE_S to lower them; a value outside the accepted range is
 * ignored, loudly, and no value can raise them (a knob that lengthened the
 * deadline would be a way to hold every worker in the pool for as long as one
 * liked). Pass the transport's own budget and take back the one in force. */
double http_deadline_capped(double budget);

struct http_conn; /* defined below; the transport speaks on one */

/* THE TRANSPORT, AS A VALUE.
 *
 * These five were loose globals -- two function pointers, a buffered-bytes
 * hook, a per-request rearm hook and a deadline -- that https.c overwrote at
 * startup to convert the whole PROCESS to TLS. That works only because the
 * process serves exactly one scheme, and it says so nowhere: a reader of
 * http.c saw one transport-blind write name and could not tell what it
 * wrote to. Worse, the
 * comment above them claimed "one connection is served at a time, so a single
 * pair of hooks is exact" -- which stopped being true when the worker pool
 * landed.
 *
 * Named and passed, the transport is a property of the CONNECTION, which is
 * what it always was. */
struct http_transport {
   ssize_t (*read)(const struct http_conn *c, void *buf, size_t n);
   /* Must move all `n` bytes or fail. */
   ssize_t (*write)(const struct http_conn *c, const void *buf, size_t n);
   /* Whether whole records are already decrypted and waiting. Polling the
    * socket cannot see those, so a caller that sleeps on the fd must ask this
    * first or it will mistake a buffered request for an idle connection.
    * Plain HTTP never buffers. */
   int (*buffered)(const struct http_conn *c);
   /* Called before each request on a connection, so a transport with a
    * per-connection deadline can rearm it per request instead. */
   void (*new_request)(const struct http_conn *c);
   double deadline_s; /* budget for ONE request */
};

/* One connection: the socket, and how to speak on it. Everything that writes
 * a response takes this rather than a bare fd -- an fd alone does not say
 * whether the bytes need encrypting. */
struct http_conn {
   int fd;
   const struct http_transport *tp;
   /* The transport's own per-connection state, opaque here. TLS keeps its
    * keys, sequence numbers and buffered plaintext in one; plain HTTP has
    * none. It lives on the CONNECTION because that is what it belongs to --
    * as a thread-local inside tls.c that the hooks reach for by implication,
    * "which connection" is answered by whichever one this worker happened to
    * handshake last. */
   void *tp_state;
   /* HOW THIS CONNECTION IS SERVED, carried WITH it rather than looked up in
    * a process global. Policy was a static filled by an ordered setter call
    * before the first accept, which is why two servers could not exist in one
    * process and why "call this before serving" was a rule rather than a
    * type. */
   const struct http_policy *pol;
   /* The listening socket, so a connection can ask whether anyone else is
    * queued behind it (see http_others_waiting). -1 = do not check. */
   int watch_fd;
   /* Whether THIS response should say "Connection: close". Points at the
    * per-connection flag in http_handle_conn, so the header a handler emits
    * and the decision the loop made are the same fact. */
   int *last_on_conn;
};

/* The plain-socket transport, for callers that want it by name. */
extern const struct http_transport http_transport_plain;

/* Write a full response. `ctype` is a MIME type; `body` may be binary. */
void http_respond(const struct http_conn *c, int code, const char *reason,
                  const char *ctype, const void *body, size_t n);
/* The same, plus extra header lines (each already CRLF-terminated), for the
 * responses that carry a Location or a Set-Cookie. It exists so those can go
 * through the ONE response path rather than hand-writing a header block and
 * writing it themselves -- see redirect() in page.c for what that cost. */
void http_respond_hdr(const struct http_conn *c, int code, const char *reason,
                      const char *ctype, const char *extra, const void *body,
                      size_t n);
/* http_respond with text/plain and strlen(body). */
void http_text(const struct http_conn *c, int code, const char *reason,
               const char *body);

/* ---- WHICH METHODS A ROUTE ALLOWS, AND THE ONE WAY TO REFUSE THE REST ----
 *
 * WHAT THIS MAKES UNSPELLABLE. `int get = !strcmp(r->method, "GET")` with
 * !get treated as POST is not a method check: it is a two-way sort of every
 * method that has ever been invented, and everything that is not GET lands on
 * the side that CHANGES THINGS. On the browser half:
 *
 *   PUT    /login              ran the login POST: session minted, cookie set,
 *                              the login-failure counter written
 *   DELETE /logout             dropped the session (a body carrying the CSRF
 *                              token is all a DELETE needs)
 *   PATCH  /settings/password  changed the password and signed every other
 *                              browser out
 *   PUT    /settings/delete    deleted the account, cascading every table
 *   HEAD   /invite/<token>     redeemed the invitation: account created,
 *                              session issued, share row inserted, token spent
 *
 * None of that needed a signature, a new secret or a protocol flaw. It needed
 * a method nobody had thought about, which is the whole class of defect this
 * closes: a route DECLARES the methods it answers, in one table per router,
 * and everything else is refused here.
 *
 * A MASK, not a string comparison at the call site. The refusal and the
 * `Allow` header are both generated from the same mask, so the two cannot
 * drift. Hand-written refusals each type their own list ("GET\n", "GET or
 * PUT\n", "POST only\n") next to their own strcmp, which is two places to
 * state one rule, once per route, across two files.
 *
 * HEAD HAS A BIT AND NO ROUTE GRANTS IT. That is deliberate and it is a
 * decision, not an oversight -- see http_method_not_allowed below for why HEAD
 * is refused rather than served. It is recognised here so that it is refused
 * BY NAME with a correct Allow header, rather than falling through the
 * unrecognised-method path by luck.
 *
 * An unrecognised method (PATCH, DELETE, OPTIONS, TRACE, anything a stranger
 * invents) has no bit at all, so http_method_bit returns 0, `0 & allow` is 0,
 * and it is refused by the same line. There is no default-allow to forget. */
#define HTTP_M_GET  0x01u
#define HTTP_M_HEAD 0x02u
#define HTTP_M_POST 0x04u
#define HTTP_M_PUT  0x08u

/* The bit for `method`, or 0 for one this server does not implement. */
unsigned http_method_bit(const char *method);
/* The methods in `allow`, spelled as an `Allow` header value: "GET, POST".
 * Always NUL-terminates. The order is fixed (the order of the bits above) so
 * one route's Allow header does not depend on how its mask was written. */
void http_allow_list(unsigned allow, char *out, size_t cap);
/* 405 with the `Allow` header RFC 9110 9.5.5 requires, generated from the same
 * mask as the body. For the /v1 API and anything else that writes its response
 * straight to the socket; the browser half fills in struct req instead (see
 * web_method_not_allowed in web.h) because its responses are built under the
 * page lock and sent without it.
 *
 * HEAD IS REFUSED HERE, NOT SERVED, and this is the reasoning:
 *
 *   1. HEAD's contract is the headers GET would have sent, byte for byte, with
 *      no body -- which means a correct HEAD must compute the Content-Length
 *      of a body it then does not write. The browser half could honour that at
 *      one place (web_route's flush), but the /v1 half writes responses from a
 *      dozen call sites straight down the socket, and there is no single point
 *      that could guarantee it. A server that honours HEAD on half its routes
 *      and quietly returns a body on the other half is a worse answer than one
 *      that refuses it everywhere: the second is wrong in a way a client can
 *      see and handle.
 *   2. NOTHING HERE ASKS FOR IT. The app signs GET, PUT and POST and nothing
 *      else (app/sync.c), and the signature covers the method, so a HEAD could
 *      not verify even if a route accepted it. Browsers do not send HEAD for
 *      navigation, and this server has no cache to revalidate and no
 *      Content-Length anyone reads without the body.
 *   3. An accepted-but-unimplemented HEAD is EXACTLY the method that walked
 *      into the POST logic above. Refusing it by name is the answer that
 *      cannot become wrong by accident later.
 *
 * A monitor that wants to know this server is alive should GET a page. */
void http_method_not_allowed(const struct http_conn *c, unsigned allow);

/* ---- ONE EXACT REQUEST-LINE GRAMMAR, AND NOTHING ELSE -------------------
 *
 * RFC 9112 3 defines the request line as exactly
 *
 *     request-line = method SP request-target SP HTTP-version CRLF
 *
 * with method a token (RFC 9110 5.6.2), HTTP-version = "HTTP" "/" DIGIT "."
 * DIGIT (RFC 9112 2.3), and -- 9112 3 again -- "no whitespace is allowed in
 * the three components" and a recipient that receives whitespace in a
 * request-line MUST reject the message. This server parsed it by looking for
 * two spaces and ignoring everything after the second, which accepted a
 * strictly larger language than that grammar:
 *
 *   "GET / HTTP/1.1 anything\r\n"   the trailing token was never looked at
 *   "GET / HTTP/1.2\r\n"            an unsupported version was served as 1.1
 *   "GET / HTTP/9.9\r\n"            so was a version that does not exist
 *   "GET / http/1.1\r\n"            case was never checked
 *   "GET /\r\nHost: x\r\n"          NO version at all: the second space was
 *                                   found inside the *Host header*, so the
 *                                   request target became "/\r\nHost:" and
 *                                   the request was routed on it
 *   "GET  / HTTP/1.1\r\n"           two spaces: the target became empty
 *
 * WHY THAT IS A SECURITY DEFECT AND NOT UNTIDINESS. This deployment has a
 * configurable front door (PANCRA_FRONT: direct, NAT, or a named proxy), so
 * something else may parse these bytes before this server does. Request
 * smuggling is precisely two parsers disagreeing about where one request ends:
 * a fronting proxy that rejects "GET / HTTP/1.1 x" and this origin that serves
 * it -- or, far worse, a proxy that reads "GET /\r\nHost: evil\r\n" as one
 * request with a Host header while this origin read the Host line as part of
 * the TARGET and went looking for the NEXT request inside what the proxy
 * considered headers. From there an attacker prefixes bytes onto the next
 * client's request: their session cookie, their POST body, their CSRF token.
 * The answer is not to guess which reading is right; it is to refuse every
 * shape the two could read differently.
 *
 * ONLY HTTP/1.1 IS SUPPORTED, deliberately, and HTTP/1.0 is refused with 505
 * rather than served. Every response this server writes says "HTTP/1.1", and
 * 1.0's connection defaults are the opposite of 1.1's (close unless
 * "Connection: keep-alive", rather than keep-alive unless "close"). Answering
 * a 1.0 request with 1.1 keep-alive semantics is another way for two ends to
 * disagree about where the response stops. Nothing here speaks 1.0: the app
 * (app/sync.c through Ble.syncHttp), srv/synccli.c, curl and every test
 * generator send 1.1.
 *
 * ONE OPERATIONAL CONSEQUENCE, WRITTEN DOWN RATHER THAN DISCOVERED. If
 * PANCRA_FRONT is ever set to a `proxy:` front door, that proxy must speak
 * HTTP/1.1 UPSTREAM. Some do not by default -- nginx's proxy_http_version is
 * 1.0 unless it is set -- and this server now answers such a proxy 505 rather
 * than serving it. That is the refusal working: a front end talking 1.0 to an
 * origin that answers 1.1 is exactly the version disagreement above. The
 * configured value today is `unset` (srv/deploy/pancra.conf), so nothing is
 * behind a proxy yet; whoever sets one is the person this paragraph is for. */
enum reqline {
   REQL_OK = 0,      /* exactly the grammar above, version supported */
   REQL_BAD,         /* not that grammar: refuse with 400 */
   REQL_METHOD_LONG, /* a token, but longer than this server implements: 501 */
   REQL_TARGET_LONG, /* well formed and longer than we will hold: 414 */
   REQL_VERSION,     /* a well-formed HTTP-version we do not speak: 505 */
};

/* The caps struct req (srv/proto.h) holds these in. They live here as well so
 * that the request line can be validated in http.c BEFORE any header is
 * looked at, using buffers of the same size the router will later use -- a
 * validator that accepted a longer target than the router can hold would
 * move the refusal back into the router. */
#define HTTP_METHOD_MAX 8
#define HTTP_TARGET_MAX 640

/* Split `req` -- the whole request text, NUL-terminated -- on the grammar
 * above. `method` and `target` are always NUL-terminated, and are set to ""
 * for every answer but REQL_OK: a caller that ignored the return would then
 * route an empty path rather than a half-parsed one.
 *
 * warn_unused_result because the whole point is the refusal. */
PANCRA_MUST_USE enum reqline http_reqline(const char *req, char *method,
                                          size_t mcap, char *target,
                                          size_t tcap);

/* One connection: read the request, hand it to `handle`.
 *   req      NUL-terminated request text (headers, and the body that
 *            arrived with them)
 *   body     start of the body within req, and body_len its length as the
 *            Content-Length header declared (0 for GET)
 * The callback owns the reply. */
/* `user` is whatever was handed to http_serve/https_serve at startup -- the
 * database, here. A callback with no way to be given anything can only reach
 * globals, which is how the database came to be one. */
typedef void (*http_handler)(const struct http_conn *c, char *req, char *body,
                             size_t body_len, void *user);

/* THE SERVER'S POLICY, fixed for the life of a run.
 *
 * It was three mutable globals plus a setter, which meant the rules a
 * connection is served under could change WHILE connections were being served
 * -- and one of them (the body ceiling) sizes a buffer that is allocated once,
 * so a later change silently did not apply. Passed in at startup and copied,
 * the policy is a fact about the run rather than a variable anyone can move.
 *
 * KEEP-ALIVE MUST NOT BECOME A LOCK ON THE SERVER: requests are served one at
 * a time, so a client holding a connection open and saying nothing kept every
 * other client waiting for the whole budget. That is why the idle wait is
 * SHORT and the requests per connection are capped -- a client with more to
 * say reconnects, and one that has gone quiet is dropped rather than waited
 * on. */
struct http_policy {
   int keepalive;    /* 0 = one request per connection */
   double idle_s;    /* how long to wait for a follow-up request */
   int max_per_conn; /* requests before the connection is retired */
   size_t body_max;  /* the largest body a handler will be handed */
};

/* Bind `port` and serve forever, calling `handle` per request. `name` is only
 * for the startup log line. `pol` is copied; NULL takes the defaults (no
 * keep-alive, HTTP_BODY_MAX). Returns non-zero on a setup failure. */
int http_serve(int port, const char *name, http_handler handle,
               const struct http_policy *pol, void *user);

/* The value a response's `Connection:` header must carry right now, for a
 * handler that writes its own header block rather than calling http_respond.
 * Hand-writing "close" there bypasses the connection accounting and retires a
 * pooled connection the server was willing to keep. */
const char *http_conn_value(const struct http_conn *c);
/* Is another client queued on the listening socket right now? */
int http_others_waiting(const struct http_conn *c);
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
int http_give_way(const struct http_conn *c, double started);
/* The two halves of http_serve, so https.c can put a handshake between
 * them: bind+listen (returns the socket, or -1 with the error printed),
 * and read-one-request-and-dispatch on an accepted connection. */
int http_listen(int port, const char *name);
void http_handle_conn(const struct http_conn *c, http_handler handle,
                      void *user);

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
   /* Passed to every call of `handle`: what the server was started WITH. */
   void *user;
   /* How every connection this pool accepts is spoken on. NULL means plain
    * HTTP. It is here rather than in a global because it is a property of the
    * server, and a server that had to overwrite four globals to become HTTPS
    * could never have been both. */
   const struct http_transport *tp;
   /* The policy every connection this pool accepts is served under, and the
    * listening socket they check for waiting clients. Both are copied into
    * each http_conn at accept time, so a second pool in the same process is
    * simply a second value. */
   struct http_policy pol;
   /* Run on each accepted connection, before any request and after the last.
    * `prepare` is where a transport builds its per-connection state. */
   int (*prepare)(struct http_conn *c);
   void (*finish)(struct http_conn *c);
};

/* Start `nworkers - 1` threads and become the last worker. Does not return. */
int http_run_pool(struct http_pool *p, int nworkers);
/* Socket options every accepted connection gets. */
int http_accept_setup(int fd);
/* Monotonic seconds, for deadlines that must not jump with the clock. */
double http_mono_s(void);

/* Did the last response fail to reach the client whole? The connection must
 * then be CLOSED rather than reused: the client is still owed the rest of a
 * body, and anything written next would be read as part of it. */
int http_response_failed(void);
/* (https_serve -- the same server over TLS -- is https.h: it is another
 * module, and declaring it here made this header speak for it.) */

#endif
