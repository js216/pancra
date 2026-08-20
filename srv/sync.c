/* SPDX-License-Identifier: GPL-3.0
 * sync.c --- routing, and the handful of things done from the command line
 * Copyright 2026 Jakob Kastelic
 *
 *     ./sync [port] [datadir] [cert.pem key.pem]
 *     ./sync invite  [owner-email] [datadir]
 *     ./sync invites [datadir]
 *     ./sync revoke  <token|all> [datadir]
 *     ./sync adduser <email> [prompt|stdin|fd:N] [datadir]
 *     ./sync passwd  <email> [prompt|stdin|fd:N] [datadir]
 *     ./sync bench   [datadir]
 *
 * With a certificate and key it serves HTTPS; without, plain HTTP, which is
 * for testing on a LAN and nothing else -- the app signs its requests either
 * way, but a browser session cookie over plain HTTP is a login anybody on the
 * path can take.
 *
 * `invite` prints a signup link, which is how accounts are meant to be handed
 * out: there is no signup route, and nobody should be typing `adduser` per
 * person. With no argument the link creates an account and nothing else; with
 * an owner's email it also makes the new user a follower of that owner, which
 * is exactly what the button in the web interface mints. `adduser` remains for
 * the very first account, or for a server with no way to click a link.
 * `passwd` exists because there is no reset by email; someone locked out asks
 * the person who runs the server. Neither is a migration path -- nothing here
 * reads data.txt, and `store` and `show` keep serving it untouched.
 */
#include "auth.h"
#include "db.h"
#include "http.h"
#include "https.h"
#include "jpake.h"
#include "logs.h"
#include "pair.h"
#include "proto.h"
#include "route.h"
#include "util.h"
#include "web.h"   /* web_route: the request this dispatches */
#include <errno.h> /* EINTR: the one-byte read below must not give up on it */
#include <fcntl.h> /* open, for the backup directory's fsync */
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* S_ISDIR: a datadir is a directory that exists */
#include <termios.h>  /* the prompt types with the echo turned off */
#include <unistd.h>

#define PORT 8444

static char db_path[PATH_MAX];

/* The database sits beside the binary unless told otherwise, exactly like
 * store and show find their text files. */
static void resolve_db(const char *dir)
{
   char base[PATH_MAX - 32]; /* leaves room for the "/sync.db" below */
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
   snprintf(db_path, sizeof db_path, "%s/sync.db", base);
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
      pair_lock();         /* one exchange at a time; see pair.c */
      h_pair(r, rt.round); /* route_of bounds this to 1..4 */
      pair_unlock();
      return;
   }
   /* A pairing-shaped path that is not a round is a 404 BEFORE the signature
    * gate, so an unsigned probe of /v1/pair/9 is told the same thing whether
    * or not it could have signed. */
   if (!strncmp(r->path, "/v1/pair/", 9)) {
      http_text(r->c, 404, "Not Found", "no such pairing round\n");
      return;
   }

   r->uid = verify_signature(r);
   if (!r->uid) {
      http_text(r->c, 401, "Unauthorized", "bad or missing signature\n");
      return;
   }
   /* ONE METHOD GATE FOR EVERY ROUTE BELOW, from the declaration in route.c.
    *
    * It used to be `int get = !strcmp(r->method, "GET")` and a per-case test of
    * !get, with each case typing its own refusal string. That shape is what put
    * every unknown method on the same side as the one that WRITES: it happened
    * to be safe here only because the bucket case tested for "PUT" explicitly
    * before falling through, and because the signature covers the method -- so
    * this was one careless `else` away from the browser half's defect, on the
    * one route that replaces a whole bucket.
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
    * This is where the two-space split used to live, and it is worth naming
    * what it did: `strchr(raw, ' ')` twice, and everything after the second
    * space thrown away. "GET /\r\nHost: evil\r\n" has no version, so the
    * second space was the one inside the HOST HEADER, and this function
    * happily routed a request whose target was "/\r\nHost:" -- the origin
    * and any proxy in front of it reading the same bytes as different
    * requests, which is the entire request-smuggling primitive.
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

/* ---- THE PASSWORD IS NOT AN ARGUMENT ------------------------------------
 *
 * `sync adduser <email> <password>` put an account's password in argv, and
 * argv is public. /proc/<pid>/cmdline is world-readable, so every other login
 * on the box could read the secret out of `ps` for as long as the command
 * ran; the shell that launched it wrote the same line into .bash_history for
 * ever; and on a board with an audit log it went there too. This server has
 * no password reset by email -- that one line IS the account -- so the single
 * most sensitive string the operator ever types was the one typed in the
 * open.
 *
 * So it comes from somewhere argv cannot see:
 *
 *   (nothing)  a no-echo prompt on the terminal, typed twice. The default,
 *              because somebody creating the first account by hand should not
 *              have to know any of this.
 *   stdin      one line from standard input, for a caller that pipes it.
 *   fd:N       one line from an inherited descriptor, for a caller that
 *              already holds the secret open and whose stdin is busy.
 *
 * WHEN STDIN IS NOT A TERMINAL AND NO SOURCE WAS NAMED, THIS REFUSES. Reading
 * the pipe anyway is the friendly-looking choice and it is the wrong one:
 * `sync adduser a@b.c </dev/null` would then set an empty password with
 * nothing looking unusual, and the same command inside a shell script whose
 * stdin is the script file would take the NEXT LINE OF THE SCRIPT as the
 * password. Neither announces itself, and neither is recoverable here. Naming
 * the source costs one word and cannot do either.
 *
 * THE OLD FORM IS REFUSED, NOT DEPRECATED. A word in that position which is
 * not one of the sources above is assumed to be a password and turned away,
 * saying why. A comment advising against it would leave the exposure exactly
 * where it was, and the help would go on teaching it. */
enum pw_src { PW_PROMPT, PW_STDIN, PW_FD };

/* 1024 like the browser form's field (srv/settings.c): the two surfaces set
 * the same column, so a passphrase one of them accepts must not be a
 * passphrase the other cannot carry. */
#define PW_BUF 1024

/* Overwritten through a volatile pointer. A plain memset over a buffer that
 * is never read again is exactly the store a compiler may delete, and what is
 * left in this process's pages afterwards is the whole point. */
static void pw_wipe(void *p, size_t n)
{
   volatile unsigned char *q = p;
   while (n--)
      *q++ = 0;
}

/* 1 when `tok` names a source, with *src (and *fd) set. */
static int pw_source_of(const char *tok, enum pw_src *src, int *fd)
{
   if (!strcmp(tok, "prompt")) {
      *src = PW_PROMPT;
      return 1;
   }
   if (!strcmp(tok, "stdin")) {
      *src = PW_STDIN;
      *fd  = STDIN_FILENO;
      return 1;
   }
   if (!strncmp(tok, "fd:", 3)) {
      char *end = NULL;
      long n    = strtol(tok + 3, &end, 10);
      if (end == tok + 3 || (end && *end) || n < 0 || n > 1024)
         return 0;
      *src = PW_FD;
      *fd  = (int)n;
      return 1;
   }
   return 0;
}

/* ONE LINE, WHOLE, OR NOTHING.
 *
 * Read a byte at a time, which is not an oversight: this reads a descriptor
 * the CALLER may still be using, so it must not swallow a single byte past
 * the newline that ends the password. It runs once per process, over a line
 * of a few dozen bytes.
 *
 * TRUNCATION IS NOT AN OPTION. A password clipped at the buffer is a
 * DIFFERENT password, stored without a word said -- and the person it belongs
 * to is then locked out by the passphrase they chose, on a server with no
 * reset. Longer than the buffer is a refusal. */
static int pw_readline(int fd, char *out, size_t cap)
{
   size_t n = 0;
   for (;;) {
      char ch;
      ssize_t r = read(fd, &ch, 1);
      if (r < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      if (r == 0 || ch == '\n')
         break; /* EOF ends the last line too */
      if (ch == '\r')
         continue; /* a CRLF file is a text file, not part of the secret */
      if (n + 1 >= cap)
         return 0;
      out[n++] = ch;
   }
   out[n] = '\0';
   return 1;
}

/* The terminal, with the echo off and a confirmation. */
static int pw_prompt(const char *what, char *out, size_t cap)
{
   struct termios old, quiet;
   if (tcgetattr(STDIN_FILENO, &old) != 0) {
      fprintf(stderr, "sync: this is not a terminal to type a password at\n");
      return 0;
   }
   quiet = old;
   quiet.c_lflag &= (tcflag_t) ~(ECHO);
   if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0) {
      fprintf(stderr, "sync: the terminal would not stop echoing; refusing to "
                      "read a password it would print\n");
      return 0;
   }
   char again[PW_BUF];
   /* The prompts go to STDERR: stdout is what `sync invite` pipes and what
    * `adduser` prints its one result line on. */
   fprintf(stderr, "%s: ", what);
   int ok = pw_readline(STDIN_FILENO, out, cap);
   fprintf(stderr, "\n");
   if (ok) {
      /* TYPED TWICE, because it was not echoed and there is no reset: a
       * mistyped one is an account nobody can open again. */
      fprintf(stderr, "%s again: ", what);
      ok = pw_readline(STDIN_FILENO, again, sizeof again);
      fprintf(stderr, "\n");
      if (ok && strcmp(out, again) != 0) {
         fprintf(stderr, "sync: the two did not match; nothing was changed\n");
         ok = 0;
      }
   }
   /* PUT BACK WHATEVER HAPPENED. Leaving a shell with its echo off looks to
    * the operator like a machine that has stopped responding. */
   (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
   pw_wipe(again, sizeof again);
   if (!ok)
      pw_wipe(out, cap);
   return ok;
}

static int cli_password(enum pw_src src, int fd, const char *what, char *out,
                        size_t cap)
{
   if (src == PW_PROMPT)
      return pw_prompt(what, out, cap);
   if (!pw_readline(fd, out, cap)) {
      pw_wipe(out, cap);
      fprintf(stderr,
              "sync: no password could be read from %s (and a line longer "
              "than %zu bytes is refused, never clipped)\n",
              src == PW_STDIN ? "stdin" : "that descriptor", cap - 1);
      return 0;
   }
   return 1;
}

/* THE PASSWORD VERBS' ARGUMENTS, which are no longer what they were.
 *
 * <email> is required. The source and the datadir are both optional and are
 * told apart by shape -- a source is one of the three words above, a datadir
 * is a directory that exists -- so neither has to be given to give the other.
 * A word that is neither is the old argv password: refused here, before the
 * database is opened and before anything is written.
 *
 * The ARGUMENT COUNT is still checked by cli_needs from the table, as it is
 * for every other verb; this only says what the words mean. */
static int cli_pwargs(int argc, char **argv, const char *verb, const char **dir,
                      enum pw_src *src, int *fd)
{
   *dir         = NULL;
   *src         = PW_PROMPT;
   *fd          = -1;
   int have_src = 0;
   for (int i = 3; i < argc; i++) {
      if (!have_src && pw_source_of(argv[i], src, fd)) {
         have_src = 1;
         continue;
      }
      struct stat st;
      if (!*dir && stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
         *dir = argv[i];
         continue;
      }
      fprintf(stderr,
              "sync %s: \"%s\" is neither a password source (prompt, stdin, "
              "fd:N) nor a directory that exists.\n"
              "If it was meant to BE the password: it must not be, because "
              "every argument to this command is visible in `ps`, in "
              "/proc/%ld/cmdline and in your shell history. Nothing was "
              "changed.\n"
              "usage: sync %s <email> [prompt|stdin|fd:N] [datadir]\n",
              verb, argv[i], (long)getpid(), verb);
      return 0;
   }
   /* SEE THE BLOCK COMMENT ABOVE for why this refuses instead of reading the
    * pipe it has been handed. */
   if (!have_src && !isatty(STDIN_FILENO)) {
      fprintf(stderr,
              "sync %s: stdin is not a terminal, so there is nowhere to "
              "prompt -- and reading the pipe unasked would set whatever "
              "happens to be on it as the password, silently.\n"
              "Say where it comes from: `sync %s <email> stdin [datadir]` to "
              "read one line from stdin, or `fd:N` for an inherited "
              "descriptor.\n",
              verb, verb);
      return 0;
   }
   return 1;
}

/* ---- THE COMMAND TABLE, which is also the help --------------------------
 *
 * The verbs and the usage text were two lists: an if-chain of strcmp, and a
 * block of printf lines inside the "that is not a subcommand" branch. They
 * disagreed -- `logout` was implemented and not listed, several verbs'
 * optional [datadir] was missing, and `sync --help` was an ERROR (exit 2)
 * printing to stderr, which is the one thing a person types first.
 *
 * One table now. Every verb's usage line comes from it, the help comes from
 * it, and the arity check comes from it -- so a verb cannot be implemented
 * and undocumented, and its arguments cannot be described one way and checked
 * another. srv/test/clitest.sh compares this table against the dispatch below
 * and against the README. */
struct cli_cmd {
   const char *verb;
   const char *args; /* how the arguments read in help */
   const char *what; /* one line, lower case, no full stop */
   int need;         /* least argc this verb accepts, argv[0] included */
   /* MOST argc it accepts. A minimum alone lets surplus arguments through
    * silently, and a surplus argument is never harmless here: it is a
    * mistyped one. `sync backup out.db /data extra` ran the backup and
    * ignored "extra"; `sync verify a.db b.db` verified only a.db and said
    * nothing about b.db, which reads as "both verified". Guessing which
    * argument the user meant is the failure -- there is no reading of an
    * extra argument that is safe to assume. */
   int most;
};

static const struct cli_cmd CLI[] = {
    {"adduser", "<email> [prompt|stdin|fd:N] [datadir]", "create an account",                        3,
     5                                                                                                   },
    {"passwd",  "<email> [prompt|stdin|fd:N] [datadir]",
     "set an account's password (and sign every session out)",                                       3, 5},
    {"logout",  "<email> [datadir]",                     "sign out every session of one account",    3,
     4                                                                                                   },
    {"invite",  "[owner-email] [datadir]",               "mint a single-use invitation link",
     2,                                                                                                 4},
    {"invites", "[datadir]",                             "list the invitations that are still live", 2, 3},
    {"revoke",  "<url|token|all> [datadir]",             "withdraw an invitation",                   3, 4},
    {"backup",  "<out.db> [datadir]",                    "copy the database safely while it runs",
     3,                                                                                                 4},
    {"verify",  "<file.db>",                             "check a backup opens and holds a schema",  3, 3},
    {"bench",   "[datadir]",                             "time one password hash on THIS machine",   2, 3},
    {"help",    "",                                      "print this",                               2, 2},
};

#define NCLI ((int)(sizeof CLI / sizeof CLI[0]))

static const struct cli_cmd *cli_find(const char *verb)
{
   for (int i = 0; i < NCLI; i++)
      if (!strcmp(CLI[i].verb, verb))
         return &CLI[i];
   return NULL;
}

/* Help on STDOUT and exit 0 when it was asked for; on stderr and exit 2 when
 * it is a correction. The distinction matters to anything that pipes it. */
static void cli_usage(FILE *out)
{
   fprintf(out, "usage: sync [port] [datadir] [cert.pem key.pem]\n");
   fprintf(out,
           "       run the server. With no arguments: port %d, the database\n"
           "       beside this binary, and NO TLS (a warning says so).\n\n",
           PORT);
   for (int i = 0; i < NCLI; i++)
      fprintf(out, "       sync %-8s %-34s %s\n", CLI[i].verb, CLI[i].args,
              CLI[i].what);
   fprintf(out,
           "\n"
           "  [datadir]  where sync.db lives. Default: the directory holding\n"
           "             this binary.\n"
           "  [port]     1..65535. Default %d.\n"
           "  cert.pem key.pem\n"
           "             enable TLS. Without them the server runs plain HTTP\n"
           "             and says so on stderr -- test use only.\n"
           "\n"
           "  SYNC_BASE_URL   the origin invitation links are printed with\n"
           "                  (default https://pancra.org)\n"
           "\n"
           "  adduser and passwd NEVER take the secret as an argument: an\n"
           "  argument is visible in `ps`, in /proc and in your shell\n"
           "  history. With no source named it is asked for at the terminal\n"
           "  with the echo off; `stdin` reads one line from standard input\n"
           "  and `fd:N` reads one line from an inherited descriptor. With\n"
           "  no terminal and no source named, they refuse.\n",
           PORT);
}

/* The arity check, from the table. */
/* BOTH ENDS, with this verb's own usage line. */
static int cli_needs(int argc, const struct cli_cmd *c)
{
   if (argc < c->need) {
      fprintf(stderr, "sync %s: not enough arguments\n", c->verb);
      fprintf(stderr, "usage: sync %s %s\n", c->verb, c->args);
      return 0;
   }
   if (argc > c->most) {
      fprintf(stderr, "sync %s: %d argument%s too many\n", c->verb,
              argc - c->most, argc - c->most == 1 ? "" : "s");
      fprintf(stderr, "usage: sync %s %s\n", c->verb, c->args);
      return 0;
   }
   return 1;
}

/* A database is THREE files while it is open (the file, its write-ahead log
 * and its shared-memory index), so removing a half-made backup means removing
 * all three -- a stray sync.db.part-wal beside a published backup is another
 * database's log, and sqlite will happily try to replay it. */
static void scratch_clear(const char *path)
{
   char side[PATH_MAX];
   (void)remove(path);
   if (snprintf(side, sizeof side, "%s-wal", path) < (int)sizeof side)
      (void)remove(side);
   if (snprintf(side, sizeof side, "%s-shm", path) < (int)sizeof side)
      (void)remove(side);
}

/* ---- THE DIRECTORY ENTRY IS PART OF THE BACKUP --------------------------
 *
 * A rename is not durable until the DIRECTORY it happened in has been synced.
 * Everything before this point was careful -- the copy is taken through
 * sqlite's backup API so it sees the write-ahead log, it is integrity-checked
 * and schema-checked while it is still called .part, and only then renamed --
 * and then the process printed "backed up ... -> ..." and exited. The file's
 * CONTENTS were on the disk (sqlite commits the destination with its own
 * fsync); the NAME was not. A power cut in the seconds after that line was
 * printed leaves a backups directory whose last entry never made it out of the
 * kernel's cache, and the artifact the operator was told they had is simply not
 * there on the next boot.
 *
 * That is the worst shape a storage bug can take here, because it is invisible
 * until the one day it matters. Backups are read exactly once -- on the day the
 * board's card died, or the day somebody restores the wrong thing -- and "I
 * took one at 02:00, the script said so" is the whole basis for reaching for
 * it. An acknowledgement that can evaporate is worse than no acknowledgement.
 *
 * app/util.c has had this shape for a while (fsync_dir_of, and the three-way
 * replace_result it feeds); this is the server's copy of it, small enough to
 * stay local to the one verb that renames a file into place.
 */
static int fsync_dir_of(const char *path)
{
   char dir[PATH_MAX];
   int n = snprintf(dir, sizeof dir, "%s", path);
   if (n <= 0 || n >= (int)sizeof dir)
      return -1;
   char *slash = strrchr(dir, '/');
   if (!slash)
      snprintf(dir, sizeof dir, ".");
   else if (slash == dir)
      dir[1] = '\0'; /* "/x" -> "/" */
   else
      *slash = '\0';
   int dfd = open(dir, O_RDONLY);
   if (dfd < 0)
      return -1;
   int ok = fsync(dfd) == 0;
   if (close(dfd) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
   /* THE DATABASE THIS RUN IS ABOUT, opened by whichever subcommand needs it
    * and handed to everything that touches storage -- the server gets it
    * through the pool (see http.h's `user`), and every helper takes it as a
    * parameter. It was a process-wide singleton reachable from any file that
    * included db.h. */
   struct db *d = NULL;
   if (argc > 1 && !strcmp(argv[1], "adduser")) {
      if (!cli_needs(argc, cli_find("adduser")))
         return 1;
      /* THE ARGUMENTS ARE READ BEFORE ANYTHING IS OPENED, so an attempted
       * argv password is refused without this process ever having held a
       * database open -- and without the secret having been anywhere but the
       * command line it is being turned away for. */
      const char *addir;
      enum pw_src asrc;
      int afd;
      if (!cli_pwargs(argc, argv, "adduser", &addir, &asrc, &afd))
         return 1;
      resolve_db(addir);
      d = db_open(db_path);
      if (!d)
         return 1;
      /* THE SAME BOUND AND THE SAME CANONICAL FORM THE WEB SURFACES USE.
       *
       * This verb creates an account, and it took whatever was on the command
       * line: an address of any length, in any case. An over-long one produced
       * a row /login can never resolve -- it refuses the address before it
       * looks anything up -- so the account existed and its owner could never
       * sign in, on a server with no password reset. A mixed-case one worked
       * (the column is NOCASE) but was STORED as typed, so `sync invites` and
       * the settings page showed a spelling that no throttle row would ever
       * match. Canonicalised here, once, and every later verb below does the
       * same, so all of them are talking about the same string. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will accept "
                 "(one '@', 3 to %d bytes, no spaces)\n",
                 argv[2], EMAIL_MAX);
         db_close(d);
         return 1;
      }
      /* AFTER the address is settled: a prompt for an account that was never
       * going to be created is a secret typed for nothing. */
      char apw[PW_BUF];
      if (!cli_password(asrc, afd, "New password", apw, sizeof apw)) {
         db_close(d);
         return 1;
      }
      long uid     = 0;
      int acreated = user_create(d, cemail, apw, &uid);
      pw_wipe(apw, sizeof apw);
      if (!acreated) {
         fprintf(stderr,
                 "sync: could not create %s (already exists, or the "
                 "password is empty)\n",
                 cemail);
         db_close(d);
         return 1;
      }
      printf("created user %ld (%s) in %s\n", uid, cemail, db_path);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "backup")) {
      if (!cli_needs(argc, cli_find("backup")))
         return 1;
      resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(db_path);
      if (!d)
         return 1;
      /* WHERE IT IS BEING PUBLISHED, CHECKED BEFORE ANYTHING IS WRITTEN.
       *
       * db_backup checks the file it CREATES (the .part below); this checks
       * the name the .part is RENAMED to, which is the one that does the
       * damage. `sync backup /data/sync.db /data` renamed a fresh copy over
       * the database the running server had open: every later request
       * succeeded, against an inode with no name, and the next restart opened
       * the backup and lost every sync taken since. See db.h -- the test is
       * by device and inode, so a symlink or a `..` cannot walk around it. */
      if (db_backup_dest(d, argv[2]) != BACKUP_DEST_OK) {
         fprintf(stderr, "sync: no backup was taken; the previous one is "
                         "untouched\n");
         db_close(d);
         return 1;
      }
      /* WRITE ASIDE, THEN RENAME, like every other durable write in this
       * repository: a backup interrupted halfway must not replace the last
       * good one with a truncated file -- which is the state you discover at
       * the only moment it matters. */
      char tmp[PATH_MAX];
      int tn = snprintf(tmp, sizeof tmp, "%s.part", argv[2]);
      if (tn <= 0 || tn >= (int)sizeof tmp) {
         fprintf(stderr, "sync: backup path too long\n");
         db_close(d);
         return 1;
      }
      scratch_clear(tmp);
      /* VERIFIED BEFORE IT IS PUBLISHED. A backup nobody has read is a hope;
       * this one is opened and integrity-checked while it is still called
       * .part, so a failure leaves the previous backup in place instead of
       * replacing it with a file that only LOOKS like one. */
      if (!db_backup(d, tmp) || !db_verify(tmp)) {
         fprintf(stderr, "sync: no backup was taken; the previous one is "
                         "untouched\n");
         scratch_clear(tmp);
         db_close(d);
         return 1;
      }
      if (rename(tmp, argv[2]) != 0) {
         fprintf(stderr, "sync: cannot publish %s\n", argv[2]);
         scratch_clear(tmp);
         db_close(d);
         return 1;
      }
      scratch_clear(tmp); /* the -wal/-shm the verify open left behind */
      /* ---- PAST THE POINT OF NO RETURN, AND IT IS ITS OWN ANSWER --------
       *
       * THREE OUTCOMES, not two, for the reason app/util.h spells out about
       * REPLACE_UNSYNCED: everything before the rename can be undone, and this
       * cannot. The rename HAS happened -- `argv[2]` names the new backup, any
       * reader opening it right now gets it, and the previous backup at that
       * name is gone whatever this fsync says. So neither "it worked" nor "it
       * failed" is true here:
       *
       *   "it worked" (what this used to print, unconditionally) tells the
       *   operator to go to bed on an artifact whose directory entry may not
       *   survive the next power cut -- and the whole value of a backup is
       *   that it is there on the morning nothing else is.
       *
       *   "it failed" -- the obvious fix, `return 1` here -- is the lie in the
       *   other direction, and a worse one for the callers: deploy.sh treats a
       *   failed backup as "refusing to deploy over unsaved data" and stops,
       *   and backup.sh moves the arrived copy aside as unverified. Both would
       *   be reacting to a backup that is present, complete and verified.
       *
       * Exit 2 is therefore its own status: PUBLISHED, DURABILITY UNKNOWN. The
       * shell layer (backup.sh, deploy.sh) has a branch for it, and neither
       * treats it as a reason to stop -- only as a reason to say so.
       *
       * The failure is not hypothetical: a backups directory whose mode has
       * been tightened to write+execute (0300) renames perfectly and cannot be
       * opened for the fsync, which is exactly this branch and is how
       * restoredrill.sh reaches it without any fault injection. */
      if (fsync_dir_of(argv[2]) != 0) {
         printf("backed up %s -> %s\n", db_path, argv[2]);
         fprintf(stderr,
                 "sync: DURABILITY UNCERTAIN: %s is written and verified and "
                 "is there NOW, but its directory entry could not be synced, "
                 "so a power loss in the next moments can erase it. The copy "
                 "is good: take another one, or copy this one somewhere "
                 "else.\n",
                 argv[2]);
         db_close(d);
         return 2;
      }
      printf("backed up %s -> %s\n", db_path, argv[2]);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "verify")) {
      if (!cli_needs(argc, cli_find("verify")))
         return 1;
      if (!db_verify(argv[2]))
         return 1;
      printf("%s verifies\n", argv[2]);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "invite")) {
      if (!cli_needs(argc, cli_find("invite")))
         return 1;
      /* "sync invite" -> a plain signup link.
       * "sync invite jk@x" -> a signup link that also follows jk@x. */
      /* Both arguments are optional and either may come first in practice,
       * so tell them apart by shape: an owner is an email address. */
      const char *owner_email = NULL;
      const char *dir         = NULL;
      for (int i = 2; i < argc; i++) {
         if (strchr(argv[i], '@'))
            owner_email = argv[i];
         else
            dir = argv[i];
      }
      resolve_db(dir);
      d = db_open(db_path);
      if (!d)
         return 1;
      long owner = 0;
      if (owner_email) {
         /* Canonicalised like every other surface, so "JK@X" and "jk@x" are
          * the same owner here as they are to the account table -- and so an
          * over-long argument is refused rather than becoming a silent
          * "no such user". */
         char cowner[EMAIL_BUF];
         if (!email_canon(owner_email, cowner, sizeof cowner)) {
            fprintf(stderr,
                    "sync: %s is not an address this server will "
                    "accept\n",
                    owner_email);
            db_close(d);
            return 1;
         }
         /* THE FAILURE OUTPUT IS READ, not discarded. user_by_email answers 0
          * for "no such account" AND for "the query did not run", and telling
          * an operator "no such user" when the database is unreadable sends
          * them to check the spelling of an address that is perfectly
          * correct. */
         int ofailed = 0;
         owner       = user_by_email(d, cowner, &ofailed);
         if (ofailed) {
            fprintf(stderr, "sync: could not look up %s: the database did "
                            "not answer\n",
                    cowner);
            db_close(d);
            return 1;
         }
         if (!owner) {
            fprintf(stderr, "sync: no such user: %s\n", cowner);
            db_close(d);
            return 1;
         }
      }
      char token[TOKEN_HEX + 1];
      rnd_hex(token, TOKEN_HEX);
      sqlite3_stmt *st = db_prep(
          d,
          "INSERT INTO share_token(token,owner_id,email,created_at,expires_at)"
          " VALUES(?,?,NULL,?,?)");
      if (!st)
         return 1;
      long now = (long)time(NULL);
      sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
      if (owner)
         sqlite3_bind_int64(st, 2, owner);
      else
         sqlite3_bind_null(st, 2);
      sqlite3_bind_int64(st, 3, now);
      sqlite3_bind_int64(st, 4, now + TOKEN_TTL);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
      if (!ok) {
         fprintf(stderr, "sync: could not create the invitation\n");
         return 1;
      }
      /* The FULL url and nothing else, so it can be pasted straight into a
       * message or piped somewhere. Anything else on stdout would have to be
       * stripped by every caller. */
      const char *base = getenv("SYNC_BASE_URL");
      if (!base || !*base)
         base = BASE_URL_DEFAULT;
      printf("%s/invite/%s\n", base, token);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "invites")) {
      /* Checked like every other verb. This one skipped cli_needs entirely --
       * its minimum of 2 is what `argc > 1` already guarantees, so there
       * seemed to be nothing to check, and the MAXIMUM went unenforced with
       * it. */
      if (!cli_needs(argc, cli_find("invites")))
         return 1;
      resolve_db(argc > 2 ? argv[2] : NULL);
      d = db_open(db_path);
      if (!d)
         return 1;
      const char *base = getenv("SYNC_BASE_URL");
      if (!base || !*base)
         base = BASE_URL_DEFAULT;
      /* Unused and unexpired only: a spent or stale token is not a link
       * anybody can still use, and listing them would just be noise to read
       * past. */
      sqlite3_stmt *st = db_prep(
          d,
          "SELECT t.token, u.email, t.expires_at FROM share_token t"
          " LEFT JOIN user u ON u.id=t.owner_id"
          " WHERE t.used_at IS NULL AND t.expires_at>? ORDER BY t.created_at");
      if (!st)
         return 1;
      sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
      int n = 0;
      int lrc;
      while ((lrc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *own = (const char *)sqlite3_column_text(st, 1);
         long left = ((long)sqlite3_column_int64(st, 2) - (long)time(NULL));
         printf("%s/invite/%s  %s  %ld days left\n", base,
                (const char *)sqlite3_column_text(st, 0),
                own ? own : "(signup only)", left / 86400);
         n++;
      }
      int lok = db_finished(lrc);
      sqlite3_finalize(st);
      if (!lok) {
         fprintf(stderr, "sync: the invitation list is INCOMPLETE\n");
         db_close(d);
         return 1;
      }
      if (!n)
         printf("no live invitations\n");
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "revoke")) {
      if (!cli_needs(argc, cli_find("revoke")))
         return 1;
      resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(db_path);
      if (!d)
         return 1;
      /* A token may be given as the bare hex or as the whole URL, because the
       * whole URL is what `invites` printed and what got pasted somewhere. */
      const char *tok   = argv[2];
      const char *slash = strrchr(tok, '/');
      if (slash)
         tok = slash + 1;
      sqlite3_stmt *st =
          strcmp(tok, "all")
              ? db_prep(d, "DELETE FROM share_token WHERE token=?")
              : db_prep(d, "DELETE FROM share_token WHERE used_at IS NULL");
      if (!st)
         return 1;
      if (strcmp(tok, "all"))
         sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      int n  = db_changes(d);
      sqlite3_finalize(st);
      if (!ok) {
         fprintf(stderr, "sync: could not revoke\n");
         return 1;
      }
      /* A CLI whose exit status does not distinguish "revoked" from "no such
       * token" cannot be scripted: `sync revoke $t || alert` never fires. */
      if (n == 0) {
         fprintf(stderr, "sync: no such invitation; nothing was revoked\n");
         db_close(d);
         return 1;
      }
      printf("revoked %d\n", n);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "passwd")) {
      if (!cli_needs(argc, cli_find("passwd")))
         return 1;
      const char *pdir;
      enum pw_src psrc;
      int pfd;
      if (!cli_pwargs(argc, argv, "passwd", &pdir, &psrc, &pfd))
         return 1;
      resolve_db(pdir);
      d = db_open(db_path);
      if (!d)
         return 1;
      /* Canonicalised, because this is the ONLY recovery there is: there is no
       * password reset by email on this server, so an operator typing the
       * address in a different case than it was created in must reach the same
       * account. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will "
                 "accept\n",
                 argv[2]);
         db_close(d);
         return 1;
      }
      int pwfailed = 0;
      long uid     = user_by_email(d, cemail, &pwfailed);
      if (pwfailed) {
         fprintf(stderr,
                 "sync: could not look up %s: the database did not answer\n",
                 cemail);
         db_close(d);
         return 1;
      }
      if (!uid) {
         fprintf(stderr, "sync: no such user: %s\n", cemail);
         db_close(d);
         return 1;
      }
      /* Nobody is asked to type a new password for an account that is not
       * there. */
      char ppw[PW_BUF];
      if (!cli_password(psrc, pfd, "New password", ppw, sizeof ppw)) {
         db_close(d);
         return 1;
      }
      /* THE SAME ONE OPERATION THE BROWSER CALLS, and there is no second step
       * here for a reason: user_set_password (srv/auth.c) sets the hash and
       * revokes every session inside ONE transaction. This verb used to do
       * the revocation itself, afterwards, as a separate statement -- so a
       * revocation that failed left the NEW password installed and the OLD
       * cookies working, which is precisely the state the person changing
       * their password believes they have just left. A copy of that logic
       * here is how the two surfaces drifted apart in the first place. */
      int pchanged = user_set_password(d, uid, ppw);
      pw_wipe(ppw, sizeof ppw);
      if (!pchanged) {
         fprintf(stderr,
                 "sync: the password was NOT changed and nothing was signed "
                 "out -- %s is exactly as it was. (It cannot be empty, and "
                 "the change is refused unless every session can be revoked "
                 "in the same transaction.)\n",
                 cemail);
         db_close(d);
         return 1;
      }
      printf("password changed for %s; all sessions signed out\n", cemail);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "logout")) {
      if (!cli_needs(argc, cli_find("logout")))
         return 1;
      /* Sign one account out of every browser.
       *
       * `passwd` already does this as a side effect, and that was the only way
       * to get it -- so "I left myself signed in on a machine I no longer
       * have" required changing the password. It is also what makes the
       * session check testable without reaching into the database with an
       * external tool. */
      if (!cli_needs(argc, cli_find("logout")))
         return 1;
      resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(db_path);
      if (!d)
         return 1;
      /* Canonicalised: a revocation aimed at the wrong spelling of an address
       * reports "no such user" and leaves every stolen cookie working, which
       * is the one answer this verb must never give by accident. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will "
                 "accept\n",
                 argv[2]);
         db_close(d);
         return 1;
      }
      int lofailed = 0;
      long uid     = user_by_email(d, cemail, &lofailed);
      if (lofailed) {
         fprintf(stderr, "sync: could not look up that account: the database "
                         "did not answer\n");
         db_close(d);
         return 1;
      }
      if (!uid) {
         fprintf(stderr, "sync: no such user\n");
         db_close(d);
         return 1;
      }
      if (!session_drop_all(d, uid)) {
         fprintf(stderr, "sync: sessions could NOT be dropped\n");
         db_close(d);
         return 1;
      }
      printf("signed out everywhere: %s\n", cemail);
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "bench")) {
      if (!cli_needs(argc, cli_find("bench")))
         return 1;
      /* Calibrate PW_ITERS_DEFAULT on the machine that will run it: the cost
       * has to be high enough to matter and low enough not to hand a
       * server with a small worker pool a denial of service. */
      uint8_t salt[PW_SALT_LEN] = {0}, out[PW_HASH_LEN];
      double t0                 = http_mono_s();
      /* CHECKED, even though it cannot fail here. pw_hash reports a refusal
       * now (item 58-60: the KDF used to substitute parameters silently -- a
       * salt truncated at 252 bytes, an iteration count of 0 computed as 1 --
       * and had no way to say so). All three arguments here are compile-time
       * constants that srv/auth.c's _Static_asserts prove cannot be refused,
       * so this branch is unreachable; it is written anyway because "provably
       * unreachable today" is a property of the constants above it, and the
       * next person to change one of them should get an error rather than a
       * benchmark timing an empty buffer. */
      if (!pw_hash("benchmark-password", salt, PW_ITERS_DEFAULT, out)) {
         fprintf(stderr, "sync: the password KDF refused its own defaults\n");
         return 1;
      }
      double dt = http_mono_s() - t0;
      printf("PBKDF2-HMAC-SHA256 %d iterations: %.0f ms\n", PW_ITERS_DEFAULT,
             dt * 1000);
      return 0;
   }

   /* HELP IS AN ANSWER, NOT AN ERROR. `sync --help` used to fall through to
    * "that is not a subcommand or a port", print to stderr and exit 2 -- the
    * first thing anybody types, treated as a mistake. All three spellings
    * work, on stdout, exit 0. */
   if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
                    !strcmp(argv[1], "-h"))) {
      /* Even help has an exact form. `sync help backup` looks like a request
       * for one verb's usage and is not one -- printing the whole help for it
       * answers a question that was not asked, and quietly. */
      if (argc > 2) {
         fprintf(stderr, "sync help: %d argument%s too many\n", argc - 2,
                 argc - 2 == 1 ? "" : "s");
         cli_usage(stderr);
         return 2;
      }
      cli_usage(stdout);
      return 0;
   }

   /* THE FALL-THROUGH IS "run the server", so anything that is not one of the
    * subcommands above arrives here as a port number -- and atoi answered 0
    * for every one of them. `sync status`, `sync invit` (a typo) each bound an
    * ephemeral port, printed "listening on port 0", and sat there being a
    * daemon nobody asked for, with argv[2] taken as the data directory. A
    * verb that is not a verb is a mistake; it should say so and exit, not
    * start a server. */
   int port = PORT;
   if (argc > 1) {
      char *end = NULL;
      long p    = strtol(argv[1], &end, 10);
      if (argv[1][0] == '\0' || !end || *end != '\0' || p < 1 || p > 65535) {
         fprintf(stderr, "sync: '%s' is not a subcommand or a port\n", argv[1]);
         cli_usage(stderr);
         return 2;
      }
      port = (int)p;
   }
   resolve_db(argc > 2 ? argv[2] : NULL);
   d = db_open(db_path);
   if (!d)
      return 1;
   jpake_init();
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
   printf("sync: database %s\n", db_path);
   fflush(stdout);
   /* BOTH TLS FILES OR NEITHER. Three arguments -- port, datadir and ONE of
    * the pair -- used to fall through to the plaintext branch, so a mistyped
    * or missing key file started a server that serves browser logins in the
    * clear. It announced "NO TLS", but on a line no one reads on a board that
    * boots unattended, and the operator's command said cert.pem: the one
    * reading that must never be guessed is "they meant to run without TLS".
    *
    * Surplus arguments are refused for the same reason as every verb above:
    * an extra one is a mistyped one. */
   if (argc == 4) {
      fprintf(stderr, "sync: a certificate needs its key -- give BOTH "
                      "cert.pem and key.pem, or neither\n");
      cli_usage(stderr);
      return 2;
   }
   if (argc > 5) {
      fprintf(stderr, "sync: %d argument%s too many\n", argc - 5,
              argc - 5 == 1 ? "" : "s");
      cli_usage(stderr);
      return 2;
   }
   if (argc == 5)
      return https_serve(port, argv[3], argv[4], "sync", handle, &pol, d);
   fprintf(stderr, "sync: NO TLS -- browser logins are exposed; test use "
                   "only\n");
   return http_serve(port, "sync", handle, &pol, d);
}
