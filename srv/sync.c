/* SPDX-License-Identifier: GPL-3.0
 * sync.c --- routing, and the handful of things done from the command line
 * Copyright 2026 Jakob Kastelic
 *
 *     ./sync [port] [datadir] [cert.pem key.pem]
 *     ./sync invite  [owner-email] [datadir]
 *     ./sync invites [datadir]
 *     ./sync revoke  <token|all> [datadir]
 *     ./sync adduser <email> <password> [datadir]
 *     ./sync passwd  <email> <password> [datadir]
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
#include "sync.h"
#include "auth.h"
#include "db.h"
#include "http.h"
#include "jpake.h"
#include "logs.h"
#include "pair.h"
#include "util.h"
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* "/v1/bucket/<log>/<n>" -> log and n. Returns 0 if it is not that shape. */
static int parse_bucket_path(const char *path, char *log, size_t cap,
                             long *bucket)
{
   const char *p     = path + 11; /* past "/v1/bucket/" */
   const char *slash = strchr(p, '/');
   if (!slash)
      return 0;
   size_t n = (size_t)(slash - p);
   if (n == 0 || n >= cap)
      return 0;
   memcpy(log, p, n);
   log[n]    = '\0';
   char *end = NULL;
   *bucket   = strtol(slash + 1, &end, 10);
   if (end == slash + 1 || (end && *end))
      return 0;
   return 1;
}

static void route_api(struct req *r)
{
   /* Pairing is the one route that cannot be signed: it is where the key
    * being signed with comes from. */
   if (!strncmp(r->path, "/v1/pair/", 9)) {
      /* strtol, not atoi: atoi cannot say "that was not a number", so
       * /v1/pair/abc parsed as round 0 rather than as nonsense. */
      char *rend = NULL;
      long round = strtol(r->path + 9, &rend, 10);
      if (rend == r->path + 9)
         round = 0;
      if (round < 1 || round > 4) {
         http_text(r->fd, 404, "Not Found", "no such pairing round\n");
      } else {
         pair_lock();           /* one exchange at a time; see pair.c */
         h_pair(r, (int)round); /* bounded to 1..4 just above */
         pair_unlock();
      }
      return;
   }
   r->uid = verify_signature(r);
   if (!r->uid) {
      http_text(r->fd, 401, "Unauthorized", "bad or missing signature\n");
      return;
   }
   int get = !strcmp(r->method, "GET");
   if (!strcmp(r->path, "/v1/digest") && get) {
      h_digest(r);
      return;
   }
   if (!strncmp(r->path, "/v1/digest/", 11) && get) {
      h_digest_log(r, r->path + 11);
      return;
   }
   if (!strncmp(r->path, "/v1/bucket/", 11)) {
      char log[LOGNAME_MAX + 1];
      long bucket = 0;
      if (!parse_bucket_path(r->path, log, sizeof log, &bucket)) {
         http_text(r->fd, 400, "Bad Request",
                   "expected /v1/bucket/<log>/<n>\n");
         return;
      }
      if (get)
         h_bucket_get(r, log, bucket);
      else if (!strcmp(r->method, "PUT"))
         h_bucket_put(r, log, bucket);
      else
         http_text(r->fd, 405, "Method Not Allowed", "GET or PUT\n");
      return;
   }
   http_text(r->fd, 404, "Not Found",
             "sync: the app API.\n"
             "  POST /v1/pair/1|2|3\n"
             "  GET  /v1/digest\n"
             "  GET  /v1/digest/<log>\n"
             "  GET  /v1/bucket/<log>/<bucket>\n"
             "  PUT  /v1/bucket/<log>/<bucket>\n");
}

static void handle(int fd, char *raw, char *body, size_t body_len)
{
   struct req r = {0};
   r.fd         = fd;
   r.body       = body;
   r.body_len   = body_len;

   /* "METHOD SP target SP HTTP/1.1\r\n" */
   const char *sp1 = strchr(raw, ' ');
   if (!sp1) {
      http_text(fd, 400, "Bad Request", "bad request line\n");
      return;
   }
   size_t mlen = (size_t)(sp1 - raw);
   if (mlen >= sizeof r.method) {
      http_text(fd, 501, "Not Implemented", "unsupported method\n");
      return;
   }
   memcpy(r.method, raw, mlen);
   r.method[mlen]  = '\0';
   const char *t   = sp1 + 1;
   const char *sp2 = strchr(t, ' ');
   if (!sp2) {
      http_text(fd, 400, "Bad Request", "bad request line\n");
      return;
   }
   size_t tlen = (size_t)(sp2 - t);
   if (tlen >= sizeof r.target)
      tlen = sizeof r.target - 1;
   memcpy(r.target, t, tlen);
   r.target[tlen] = '\0';

   /* The MAC covers the target EXACTLY as sent, so the decoded path is a
    * separate copy: decoding first and re-encoding would never round-trip. */
   snprintf(r.path, sizeof r.path, "%s", r.target);
   char *q = strchr(r.path, '?');
   if (q)
      *q = '\0';
   url_decode(r.path);
   /* A decoded path must not be able to climb out of the routes below. */
   if (strstr(r.path, "..")) {
      http_text(fd, 400, "Bad Request", "bad path\n");
      return;
   }

   char *eol = strchr(raw, '\n');
   r.hdr     = eol ? eol + 1 : raw;

   if (!strncmp(r.path, "/v1/", 4))
      route_api(&r);
   else
      web_route(&r);
}

static int cli_needs(int argc, int want, const char *usage)
{
   if (argc < want) {
      fprintf(stderr, "usage: %s\n", usage);
      return 0;
   }
   return 1;
}

int main(int argc, char **argv)
{
   if (argc > 1 && !strcmp(argv[1], "adduser")) {
      if (!cli_needs(argc, 4, "sync adduser <email> <password> [datadir]"))
         return 1;
      resolve_db(argc > 4 ? argv[4] : NULL);
      if (!db_open(db_path))
         return 1;
      long uid = 0;
      if (!user_create(argv[2], argv[3], &uid)) {
         fprintf(stderr,
                 "sync: could not create %s (already exists, or the "
                 "password is empty)\n",
                 argv[2]);
         return 1;
      }
      printf("created user %ld (%s) in %s\n", uid, argv[2], db_path);
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "invite")) {
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
      if (!db_open(db_path))
         return 1;
      long owner = 0;
      if (owner_email) {
         owner = user_by_email(owner_email);
         if (!owner) {
            fprintf(stderr, "sync: no such user: %s\n", owner_email);
            return 1;
         }
      }
      char token[TOKEN_HEX + 1];
      rnd_hex(token, TOKEN_HEX);
      sqlite3_stmt *st = db_prep(
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
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "invites")) {
      resolve_db(argc > 2 ? argv[2] : NULL);
      if (!db_open(db_path))
         return 1;
      const char *base = getenv("SYNC_BASE_URL");
      if (!base || !*base)
         base = BASE_URL_DEFAULT;
      /* Unused and unexpired only: a spent or stale token is not a link
       * anybody can still use, and listing them would just be noise to read
       * past. */
      sqlite3_stmt *st = db_prep(
          "SELECT t.token, u.email, t.expires_at FROM share_token t"
          " LEFT JOIN user u ON u.id=t.owner_id"
          " WHERE t.used_at IS NULL AND t.expires_at>? ORDER BY t.created_at");
      if (!st)
         return 1;
      sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
      int n = 0;
      while (sqlite3_step(st) == SQLITE_ROW) {
         const char *own = (const char *)sqlite3_column_text(st, 1);
         long left = ((long)sqlite3_column_int64(st, 2) - (long)time(NULL));
         printf("%s/invite/%s  %s  %ld days left\n", base,
                (const char *)sqlite3_column_text(st, 0),
                own ? own : "(signup only)", left / 86400);
         n++;
      }
      sqlite3_finalize(st);
      if (!n)
         printf("no live invitations\n");
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "revoke")) {
      if (!cli_needs(argc, 3, "sync revoke <token|all> [datadir]"))
         return 1;
      resolve_db(argc > 3 ? argv[3] : NULL);
      if (!db_open(db_path))
         return 1;
      /* A token may be given as the bare hex or as the whole URL, because the
       * whole URL is what `invites` printed and what got pasted somewhere. */
      const char *tok   = argv[2];
      const char *slash = strrchr(tok, '/');
      if (slash)
         tok = slash + 1;
      sqlite3_stmt *st =
          strcmp(tok, "all")
              ? db_prep("DELETE FROM share_token WHERE token=?")
              : db_prep("DELETE FROM share_token WHERE used_at IS NULL");
      if (!st)
         return 1;
      if (strcmp(tok, "all"))
         sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      int n  = db_changes();
      sqlite3_finalize(st);
      if (!ok) {
         fprintf(stderr, "sync: could not revoke\n");
         return 1;
      }
      /* A CLI whose exit status does not distinguish "revoked" from "no such
       * token" cannot be scripted: `sync revoke $t || alert` never fires. */
      if (n == 0) {
         fprintf(stderr, "sync: no such invitation; nothing was revoked\n");
         db_close();
         return 1;
      }
      printf("revoked %d\n", n);
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "passwd")) {
      if (!cli_needs(argc, 4, "sync passwd <email> <password> [datadir]"))
         return 1;
      resolve_db(argc > 4 ? argv[4] : NULL);
      if (!db_open(db_path))
         return 1;
      long uid = user_by_email(argv[2]);
      if (!uid || !user_set_password(uid, argv[3])) {
         fprintf(stderr, "sync: no such user, or the password was empty\n");
         return 1;
      }
      /* A password change that left the old cookies working would not be a
       * password change: the usual reason to want one is that somebody else
       * may be signed in. */
      session_drop_all(uid);
      printf("password changed for %s; all sessions signed out\n", argv[2]);
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "logout")) {
      /* Sign one account out of every browser.
       *
       * `passwd` already does this as a side effect, and that was the only way
       * to get it -- so "I left myself signed in on a machine I no longer
       * have" required changing the password. It is also what makes the
       * session check testable without reaching into the database with an
       * external tool. */
      if (!cli_needs(argc, 3, "sync logout <email> [datadir]"))
         return 1;
      resolve_db(argc > 3 ? argv[3] : NULL);
      if (!db_open(db_path))
         return 1;
      long uid = user_by_email(argv[2]);
      if (!uid) {
         fprintf(stderr, "sync: no such user\n");
         return 1;
      }
      session_drop_all(uid);
      printf("signed out everywhere: %s\n", argv[2]);
      db_close();
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "bench")) {
      /* Calibrate PW_ITERS_DEFAULT on the machine that will run it: the cost
       * has to be high enough to matter and low enough not to hand a
       * server with a small worker pool a denial of service. */
      uint8_t salt[PW_SALT_LEN] = {0}, out[PW_HASH_LEN];
      double t0                 = http_mono_s();
      pw_hash("benchmark-password", salt, PW_ITERS_DEFAULT, out);
      double dt = http_mono_s() - t0;
      printf("PBKDF2-HMAC-SHA256 %d iterations: %.0f ms\n", PW_ITERS_DEFAULT,
             dt * 1000);
      return 0;
   }

   /* THE FALL-THROUGH IS "run the server", so anything that is not one of the
    * subcommands above arrives here as a port number -- and atoi answered 0
    * for every one of them. `sync --help`, `sync status`, `sync invit` (a
    * typo) each bound an ephemeral port, printed "listening on port 0", and
    * sat there being a daemon nobody asked for, with argv[2] taken as the
    * data directory. A verb that is not a verb is a mistake; it should say so
    * and exit, not start a server. */
   int port = PORT;
   if (argc > 1) {
      char *end = NULL;
      long p    = strtol(argv[1], &end, 10);
      if (argv[1][0] == '\0' || !end || *end != '\0' || p < 1 || p > 65535) {
         fprintf(stderr, "sync: '%s' is not a subcommand or a port\n", argv[1]);
         fprintf(stderr, "usage: sync [port] [datadir] [cert.pem key.pem]\n"
                         "       sync invite [owner-email] [datadir]\n"
                         "       sync invites [datadir]\n"
                         "       sync revoke <url|token|all> [datadir]\n"
                         "       sync adduser <email> <password> [datadir]\n"
                         "       sync passwd <email> <password> [datadir]\n"
                         "       sync bench [datadir]\n");
         return 2;
      }
      port = (int)p;
   }
   resolve_db(argc > 2 ? argv[2] : NULL);
   if (!db_open(db_path))
      return 1;
   jpake_init();
   /* One PUT carries a whole sync bucket; see http.h. */
   http_set_body_max(BODY_MAX);
   /* A sync is dozens of small requests in a row from ONE client. Without
    * this each of them pays a fresh TLS handshake, which on this hardware is
    * a hundred times the cost of the request itself. */
   http_keepalive = 1;
   printf("sync: database %s\n", db_path);
   fflush(stdout);
   if (argc > 4)
      return https_serve(port, argv[3], argv[4], "sync", handle);
   fprintf(stderr, "sync: NO TLS -- browser logins are exposed; test use "
                   "only\n");
   return http_serve(port, "sync", handle);
}
