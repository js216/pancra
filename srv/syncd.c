// SPDX-License-Identifier: GPL-3.0
// syncd.c --- the daemon half: routes, the request handler, and serving
// Copyright 2026 Jakob Kastelic

/* ONE OF THE THREE FILES srv/syncrun.h describes. This is what
 * runs for months: the signed API's routes, the handler every connection is
 * given, and the startup that binds a port and does not return.
 *
 * Nothing here reads a terminal, prompts for anything, or exits after one
 * action -- that is the operator CLI, srv/synccmd.c. They are two files
 * because they are two programs wearing one name.
 */
#include "authsig.h"
#include "db.h"
#include "http.h"
#include "https.h"
#include "jpake.h"
#include "logs.h"
#include "pair.h"
#include "posix.h" /* SYS_PATH_MAX: every path this server holds */
#include "proto.h"
#include "route.h"
#include "util.h"
#include "web.h"   /* web_route: the request this dispatches */
#include <errno.h> /* EINTR: the one-byte read below must not give up on it */
#include <fcntl.h> /* open, for the backup directory's fsync */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* S_ISDIR: a datadir is a directory that exists */
#include <termios.h>  /* the prompt types with the echo turned off */
#include <unistd.h>

#include "pwcost.h" /* the password cost, read and validated at startup */
#include "syncrun.h"

#define PORT 8443

/* The directory the database lives in, kept because the privacy check has to
 * name it: a database that is 0600 inside a directory anyone can enter is
 * still a database anyone can rename. sync_resolve_db is the one writer, and
 * both halves of the program read them (srv/syncrun.h) -- the CLI operates on
 * the same file the daemon serves, or it is not the same program. */
static char g_db_path[SYS_PATH_MAX];
static char g_db_dir[SYS_PATH_MAX - 32];

const char *sync_db_path(void)
{
   return g_db_path;
}

const char *sync_db_dir(void)
{
   return g_db_dir;
}

/* The database sits beside the binary unless told otherwise, exactly like
 * store and show find their text files. */
void sync_resolve_db(const char *dir)
{
   char base[SYS_PATH_MAX - 32]; /* leaves room for the "/sync.db" below */
   if (dir && *dir) {
      snprintf(base, sizeof base, "%s", dir);
      size_t n = strlen(base);
      if (n && base[n - 1] == '/')
         base[n - 1] = '\0';
   } else {
      ssize_t n = readlink("/proc/self/exe", base, sizeof base - 1);
      if (n <= 0)
         n = 0;
      base[n]     = '\0';
      char *slash = strrchr(base, '/');
      if (slash)
         *slash = '\0';
      else
         snprintf(base, sizeof base, ".");
   }
   snprintf(g_db_dir, sizeof g_db_dir, "%s", base);
   snprintf(g_db_path, sizeof g_db_path, "%s/sync.db", base);
}

/* THE GRAMMAR IS route.c, and so is the per-route method declaration
 * (route_allow). What is left here is what needs the request: the signature
 * gate, ENFORCING the declared methods, and the handler. Which paths are
 * routes -- and, more to the point, which decorated near-misses are NOT -- is
 * a question about a string, and it is answered where a test can ask it
 * without starting a server. */
static void route_api(struct req *r)
{
   struct route rt;
   route_of(r->path, &rt);
   unsigned m = http_method_bit(r->method);

   /* Pairing is the one route that cannot be signed: it is where the key
    * being signed with comes from. Which is exactly why its spelling is
    * exact -- see route.c. */
   if (rt.kind == RT_PAIR) {
      /* THE METHOD IS CHECKED BEFORE THE PAIRING LOCK IS TAKEN. h_pair checks
       * it too (belt and braces: it is reachable only from here, and it is the
       * one handler that runs unauthenticated), but it checked it *inside* the
       * lock -- so a stranger sending GET /v1/pair/1 in a loop serialised
       * every real pairing round behind a request that was going to be
       * refused. */
      if (!(m & route_allow(RT_PAIR))) {
         http_method_not_allowed(r->c, route_allow(RT_PAIR));
         return;
      }
      /* h_pair SERIALISES ITSELF: the lock is private to pair.c
       * now, so a route cannot forget to take it. */
      h_pair(r, rt.round); /* route_of bounds this to 1..4 */
      return;
   }
   /* A pairing-shaped path that is not a round is a 404 BEFORE the signature
    * gate, so an unsigned probe of /v1/pair/9 is told the same thing whether
    * or not it could have signed. */
   if (!strncmp(r->path, "/v1/pair/", 9)) {
      http_text(r->c, 404, "Not Found", "no such pairing round\n");
      return;
   }

   /* ---- FOUR ANSWERS, AND ONE OF THEM IS NOT THE APP'S FAULT --------
    *
    * This read `if (!r->uid) 401`, and 0 meant three unrelated things: the
    * signature did not verify, the app-key lookup could not be prepared, and
    * the nonce could not be inserted. The last two are the DATABASE failing,
    * and answering 401 for them tells a correctly paired phone that its key
    * is invalid -- the one answer it must not retry, because retrying a
    * rejected credential is how an app locks itself out. The operator
    * meanwhile sees authentication failures rather than a database that
    * cannot be read.
    *
    * 503 with Retry-After for the store fault: it is transient, the caller
    * SHOULD come back, and every sync the app does is idempotent.
    *
    * REPLAY AND BAD LOOK IDENTICAL ON THE WIRE, deliberately. They are
    * different events in a log -- a verified signature with a spent nonce is
    * a retry or an attack, not a misconfigured key -- but telling them apart
    * over the network would say whether a signature was valid, which is
    * exactly what an attacker replaying captured requests wants to know. */
   switch (verify_signature(r, &r->uid)) {
      case SIG_OK: break;
      case SIG_ERROR: {
         static const char body[] =
             "the credential store could not be read; retry\n";
         http_respond_hdr(r->c, 503, "Service Unavailable", "text/plain",
                          "Retry-After: 2\r\n", body, sizeof body - 1);
      }
         return;
      case SIG_BAD:
      case SIG_REPLAY:
         http_text(r->c, 401, "Unauthorized", "bad or missing signature\n");
         return;
   }
   /* ONE METHOD GATE FOR EVERY ROUTE BELOW, from the declaration in route.c.
    *
    * NOT `int get = !strcmp(r->method, "GET")` and a per-case test of !get,
    * with each case typing its own refusal string. That shape puts every
    * unknown method on the same side as the one that WRITES, and is safe only
    * as long as the bucket case tests for "PUT" explicitly before falling
    * through -- one careless `else` away from the browser half's defect, on
    * the one route that replaces a whole bucket.
    *
    * AFTER the signature gate above, deliberately: a request that could not
    * sign is told 401 and nothing more, whatever method it used, so an unsigned
    * probe cannot learn which methods a route takes. */
   if (route_allow(rt.kind) && !(m & route_allow(rt.kind))) {
      http_method_not_allowed(r->c, route_allow(rt.kind));
      return;
   }
   int get = m == HTTP_M_GET;
   switch (rt.kind) {
      case RT_DIGEST_ALL: h_digest(r); return;
      case RT_DIGEST_LOG: h_digest_log(r, rt.log); return;
      case RT_BUCKET:
         /* GET or PUT, and nothing else has reached this line. */
         if (get)
            h_bucket_get(r, rt.log, rt.bucket);
         else
            h_bucket_put(r, rt.log, rt.bucket);
         return;
      case RT_BUCKET_BAD:
         http_text(r->c, 400, "Bad Request", "expected /v1/bucket/<log>/<n>\n");
         return;
      case RT_DIGEST_BAD:
         http_text(r->c, 400, "Bad Request", "expected /v1/digest/<log>\n");
         return;
      case RT_PAIR:
      case RT_NONE: break;
   }
   http_text(r->c, 404, "Not Found",
             "sync: the app API.\n"
             "  POST /v1/pair/1|2|3|4\n"
             "  GET  /v1/digest\n"
             "  GET  /v1/digest/<log>\n"
             "  GET  /v1/bucket/<log>/<bucket>\n"
             "  PUT  /v1/bucket/<log>/<bucket>\n");
}

/* THE VALIDATOR AND THE ROUTER MUST AGREE ABOUT CAPACITY.
 *
 * http.c refuses an over-long method or target BEFORE any header is parsed,
 * using buffers sized by http.h. If struct req held smaller ones, a target
 * that passed there would be refused here instead -- the refusal back in the
 * router, which is where it was -- and if it held larger ones, http.c would
 * refuse targets this half could have served. Neither can happen silently
 * now: this is a compile error. */
_Static_assert(sizeof(((struct req *)0)->method) == HTTP_METHOD_MAX,
               "struct req.method must be HTTP_METHOD_MAX");
_Static_assert(sizeof(((struct req *)0)->target) == HTTP_TARGET_MAX,
               "struct req.target must be HTTP_TARGET_MAX");

static void handle(const struct http_conn *c, char *raw, char *body,
                   size_t body_len, void *user)
{
   struct req r = {0};
   r.db         = user; /* what main() opened; see http.h */
   r.c          = c;
   r.body       = body;
   r.body_len   = body_len;

   /* THE REQUEST LINE IS SPLIT IN ONE PLACE, http_reqline, and this is not a
    * second opinion about it: http.c has ALREADY refused every line that does
    * not match RFC 9112 3 exactly, before it looked at a header, and closed
    * the connection when it did (see the grammar and the smuggling shape in
    * http.h). What is left here is the copy into struct req.
    *
    * In particular there is no two-space split here -- `strchr(raw, ' ')`
    * twice with everything after the second space thrown away. "GET
    * /\r\nHost: evil\r\n" has no version, so the second space is the one
    * inside the HOST HEADER, and such a split routes a request whose target
    * is "/\r\nHost:" -- the origin and any proxy in front of it reading the
    * same bytes as different requests, which is the entire request-smuggling
    * primitive.
    *
    * The refusals are still written rather than replaced with an assertion:
    * unreachable is a claim about another file, and a 400 costs nothing. */
   enum reqline rl =
       http_reqline(raw, r.method, sizeof r.method, r.target, sizeof r.target);
   switch (rl) {
      case REQL_OK: break;
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

   /* The MAC covers the target EXACTLY as sent, so the decoded path is a
    * separate copy: decoding first and re-encoding would never round-trip. */
   snprintf(r.path, sizeof r.path, "%s", r.target);
   for (const char *p = r.path; *p; p++) {
      if (*p == '%') {
         if (!p[1] || !p[2]) {
            http_text(c, 400, "Bad Request", "bad path encoding\n");
            return;
         }
         /* Through unsigned char: the bytes come off the wire, so `char` is
          * signed here and a high byte would arrive as a negative int. The
          * comparisons below would still reject it, but only by accident --
          * and the same pattern one line to the left is how a hostile byte
          * indexes an array from before its start. */
         int a = (unsigned char)p[1], b = (unsigned char)p[2];
         int ah = (a >= '0' && a <= '9') || (a >= 'a' && a <= 'f') ||
                  (a >= 'A' && a <= 'F');
         int bh = (b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') ||
                  (b >= 'A' && b <= 'F');
         if (!ah || !bh || (p[1] == '0' && p[2] == '0')) {
            http_text(c, 400, "Bad Request", "bad path encoding\n");
            return;
         }
         p += 2;
      }
   }
   char *q = strchr(r.path, '?');
   if (q)
      *q = '\0';
   url_decode(r.path);
   /* A decoded path must not be able to climb out of the routes below. */
   if (strstr(r.path, "..")) {
      http_text(c, 400, "Bad Request", "bad path\n");
      return;
   }

   char *eol = strchr(raw, '\n');
   r.hdr     = eol ? eol + 1 : raw;

   if (!strncmp(r.path, "/v1/", 4))
      route_api(&r);
   else
      web_route(&r);
}

/* THE DAEMON. main() (srv/sync.c) sends everything that is not an operator
 * verb here, so argv[1] -- if there is one -- is a PORT.
 *
 * IT IS PARSED, NOT atoi'd. atoi answered 0 for every non-number, so `sync
 * status` and `sync invit` (a typo) each bound an ephemeral port, printed
 * "listening on port 0", and sat there being a daemon nobody asked for, with
 * argv[2] taken as the data directory. A word that is not a number is a
 * mistake; it says so and exits. */
/* Detach from the terminal. The board starts this from an ssh session that
 * then closes, and a process still holding that session dies with it. */
static void detach(void)
{
   if (fork() > 0)
      _exit(0);
   setsid();
   char path[SYS_PATH_MAX];
   snprintf(path, sizeof path, "%s/sync.log", sync_db_dir());
   int log = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (log >= 0) {
      dup2(log, 1);
      dup2(log, 2);
      if (log > 2)
         close(log);
   }
   int nul = open("/dev/null", O_RDONLY);
   if (nul >= 0) {
      dup2(nul, 0);
      if (nul > 2)
         close(nul);
   }
}

int sync_daemon_main(int argc, char **argv)
{
   struct db *d = NULL;
   int port     = PORT;
   if (argc > 1) {
      char *end = NULL;
      int64_t p = strtoll(argv[1], &end, 10);
      if (argv[1][0] == '\0' || !end || *end != '\0' || p < 1 || p > 65535) {
         fprintf(stderr, "sync: '%s' is not a subcommand or a port\n", argv[1]);
         sync_cli_usage(stderr);
         return 2;
      }
      port = (int)p;
   }
   sync_resolve_db(argc > 2 ? argv[2] : NULL);
   d = db_open(sync_db_path());
   if (!d)
      return 1;
   jpake_init();
   /* THE PUBLIC ORIGIN, SETTLED HERE: on the one thread that
    * exists, before the pool is started, so every handler afterwards reads an
    * immutable string -- and the diagnostic about a misconfigured
    * PANCRA_ORIGIN is printed once, at startup, where an operator sees it. */
   public_origin_init();
   /* THE PASSWORD COST, VALIDATED BEFORE ANYTHING IS SERVED. A
    * policy this build cannot read is not obeyed: the compiled default stands
    * and the reason is printed here, at startup, where an operator sees it
    * rather than in the log of the first login. */
   (void)pwcost_init(sync_db_dir());
   /* THIS SERVER'S POLICY, stated once and fixed for the run.
    *
    * KEEP-ALIVE, because a sync is dozens of small requests in a row from ONE
    * client and each would otherwise pay a fresh TLS handshake -- on this
    * hardware a hundred times the cost of the request itself.
    *
    * A LARGER BODY, because one PUT carries a whole sync bucket. It is not
    * the default: on a 56 MB board a server that never receives one should
    * not pay for the buffer. */
   const struct http_policy pol = {.keepalive    = 1,
                                   .idle_s       = 2.0,
                                   .max_per_conn = 500,
                                   .body_max     = BODY_MAX};
   printf("sync: database %s\n", sync_db_path());
   fflush(stdout);
   /* BOTH TLS FILES OR NEITHER. Letting three arguments -- port, datadir and
    * ONE of the pair -- fall through to the plaintext branch means a mistyped
    * or missing key file starts a server that serves browser logins in the
    * clear. It would announce "NO TLS", but on a line no one reads on a board
    * that boots unattended, and the operator's command said cert.pem: the one
    * reading that must never be guessed is "they meant to run without TLS".
    *
    * Surplus arguments are refused for the same reason as every verb above:
    * an extra one is a mistyped one. */
   if (argc == 4) {
      fprintf(stderr, "sync: a certificate needs its key -- give BOTH "
                      "cert.pem and key.pem, or neither\n");
      sync_cli_usage(stderr);
      return 2;
   }
   if (argc > 5) {
      fprintf(stderr, "sync: %d argument%s too many\n", argc - 5,
              argc - 5 == 1 ? "" : "s");
      sync_cli_usage(stderr);
      return 2;
   }
   /* ---- AND NOT A BYTE IS SERVED OVER MATERIAL OTHERS CAN READ --------
    *
    * Checked HERE, at the last moment before the listener, rather than at
    * open time: this is the point past which the files are exposed to the
    * network, and it is the one place both the plaintext and the TLS branch
    * pass through. The subcommands (adduser, invite, backup) are deliberately
    * NOT gated -- they are an operator working on their own box, and refusing
    * to let somebody fix an account because a mode is wrong would make the
    * check the outage. */
   if (!sync_state_is_private(sync_db_dir(), sync_db_path(),
                              argc == 5 ? argv[3] : NULL,
                              argc == 5 ? argv[4] : NULL))
      return 1;
   detach();
   if (argc == 5)
      return https_serve(port, argv[3], argv[4], "sync", handle, &pol, d);
   fprintf(stderr, "sync: NO TLS -- browser logins are exposed; test use "
                   "only\n");
   return http_serve(port, "sync", handle, &pol, d);
}
