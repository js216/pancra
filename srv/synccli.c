/* SPDX-License-Identifier: GPL-3.0
 * synccli.c --- a test client for the sync protocol (NOT part of the server)
 * Copyright 2026 Jakob Kastelic
 *
 * The protocol cannot be driven from curl: pairing is a three-round J-PAKE
 * and every other call carries a MAC. This is the smallest client that does
 * both, so synctest.sh can exercise the real thing end to end rather than the
 * parts that happen to be reachable with a shell.
 *
 * It is a TEST TOOL and deliberately not a reference implementation: it
 * speaks plain HTTP, keeps the key on the command line, and trusts whatever
 * answers. pancra is the real client; srv/sync.h and the handlers it
 * declares are the contract.
 *
 *   synccli <host> <port> pair <email> <code>
 *       -> "<uid> <key hex>" on success
 *   synccli <host> <port> req <uid> <keyhex> <METHOD> <path> [body-file]
 *       -> the response body
 */
#include "hmac.h"
#include "jpake.h"
#include "sync.h"
#include "util.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int dial(const char *host, int port)
{
   int fd               = socket(AF_INET, SOCK_STREAM, 0);
   struct sockaddr_in a = {0};
   a.sin_family         = AF_INET;
   a.sin_port           = htons((uint16_t)port);
   if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
      struct hostent *h = gethostbyname(host);
      if (!h) {
         fprintf(stderr, "synccli: no such host %s\n", host);
         exit(2);
      }
      memcpy(&a.sin_addr, h->h_addr, (size_t)h->h_length);
   }
   if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
      perror("connect");
      exit(2);
   }
   return fd;
}

/* Send one request, return the whole response. `out` is malloc'd. */
static int http_do(const char *host, int port, const char *method,
                   const char *path, const char *extra, const void *body,
                   size_t blen, char **out, size_t *olen)
{
   /* EVERY exit assigns the outputs. Returning -1 without touching them left
    * the caller's `rsp` uninitialised, and the callers pass it straight to
    * die() and fwrite() -- so a refused connection printed, or wrote out, a
    * wild pointer. An error return must still leave the caller holding
    * something safe to look at. */
   static char none[] =
       ""; /* not a literal: *out is char *, and -Wwrite-strings
            * is on, so it must point at writable storage */
   *out  = none;
   *olen = 0;

   int fd = dial(host, port);
   char hdr[2048];
   int n = snprintf(hdr, sizeof hdr,
                    "%s %s HTTP/1.1\r\nHost: %s\r\n%s"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                    method, path, host, extra ? extra : "", blen);
   if (n <= 0 || n >= (int)sizeof hdr)
      return close(fd), -1;
   if (write(fd, hdr, (size_t)n) < 0)
      return close(fd), -1;
   if (blen && write(fd, body, blen) < 0)
      return close(fd), -1;
   size_t cap = 65536, got = 0;
   char *buf = malloc(cap);
   if (!buf)
      return close(fd), -1;
   for (;;) {
      if (got + 4096 > cap) {
         char *bigger = realloc(buf, cap * 2);
         if (!bigger) { /* realloc leaves the old block alive; do not leak it */
            free(buf);
            return close(fd), -1;
         }
         cap *= 2;
         buf = bigger;
      }
      ssize_t r = read(fd, buf + got, cap - got - 1);
      if (r <= 0)
         break;
      got += (size_t)r;
   }
   close(fd);
   buf[got] = '\0';
   /* "HTTP/1.1 200 ..." -- the status starts at offset 9, so a reply shorter
    * than that has no code to read and atoi would run off the buffer. */
   if (got < 12) {
      free(buf);
      return -1;
   }
   char *sep = strstr(buf, "\r\n\r\n");
   int code  = atoi(buf + 9);
   if (sep) {
      *out  = sep + 4;
      *olen = got - (size_t)(sep + 4 - buf);
   } else {
      *out  = buf;
      *olen = got;
   }
   return code;
}

static void die(const char *what, const char *saw)
{
   fprintf(stderr, "synccli: %s\n%s\n", what, saw ? saw : "");
   exit(1);
}

/* `spoil` sends a deliberately wrong step-4 confirmation, so synctest.sh can
 * check that the SERVER refuses it -- the path an attacker who guessed wrong
 * would take, and the one that must cost a try. */
static int do_pair(const char *host, int port, const char *email,
                   const char *code, int spoil)
{
   jpake_init();
   struct jpake *p =
       jpake_new((const uint8_t *)code, strlen(code), 1 /* client */);
   if (!p)
      die("pairing alloc failed", NULL);

   uint8_t pkt[PAIR_PKT], peer[PAIR_PKT];
   char hex[2 * PAIR_PKT + 1], body[1024], *rsp;
   size_t rlen;
   char session[64] = {0};

   /* round 1 */
   if (!jpake_round1(p, pkt))
      die("round1 build failed", NULL);
   hex_of(pkt, PAIR_PKT, hex);
   int n     = snprintf(body, sizeof body, "%s\n%s\n", email, hex);
   int code1 = http_do(host, port, "POST", "/v1/pair/1", NULL, body, (size_t)n,
                       &rsp, &rlen);
   if (code1 != 200)
      die("pair round 1 refused", rsp);
   if (sscanf(rsp, "%63s", session) != 1)
      die("no session in round 1 reply", rsp);
   char *nl = strchr(rsp, '\n');
   if (!nl || !hex_to(nl + 1, 2 * PAIR_PKT, peer))
      die("bad round 1 packet", rsp);
   if (!jpake_peer_round1(p, peer))
      die("peer round 1 ZKP invalid", NULL);

   /* round 2 */
   if (!jpake_round2(p, pkt))
      die("round2 build failed", NULL);
   hex_of(pkt, PAIR_PKT, hex);
   n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
   if (http_do(host, port, "POST", "/v1/pair/2", NULL, body, (size_t)n, &rsp,
               &rlen) != 200)
      die("pair round 2 refused", rsp);
   if (!hex_to(rsp, 2 * PAIR_PKT, peer))
      die("bad round 2 packet", rsp);
   if (!jpake_peer_round2(p, peer))
      die("peer round 2 ZKP invalid", NULL);

   /* round 3: exchange the last packets. Neither side can derive the key
    * before it holds the PEER's round 3, so the key comes after this call and
    * the app's proof goes in step 4. */
   if (!jpake_round3(p, pkt))
      die("round3 build failed", NULL);
   hex_of(pkt, PAIR_PKT, hex);
   n      = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
   int c3 = http_do(host, port, "POST", "/v1/pair/3", NULL, body, (size_t)n,
                    &rsp, &rlen);
   if (c3 != 200)
      die("pair round 3 refused", rsp);
   if (!hex_to(rsp, 2 * PAIR_PKT, peer))
      die("bad round 3 packet", rsp);
   if (!jpake_peer_round3(p, peer))
      die("peer round 3 ZKP invalid", NULL);

   uint8_t key[16];
   if (!jpake_shared_key(p, key))
      die("shared key failed", NULL);

   /* Verify the SERVER before trusting the key: without this the app would
    * accept a key from whatever answered the request. */
   uint8_t mac[32];
   char want[33], machex[65], confirm[33];
   hmac_sha256(key, 16, (const uint8_t *)"pancra-confirm-server",
               strlen("pancra-confirm-server"), mac);
   hex_of(mac, 32, machex);
   snprintf(want, sizeof want, "%.32s", machex);
   char *nl2 = strchr(rsp, '\n');
   if (!spoil && (!nl2 || strncmp(nl2 + 1, want, 32)))
      die("server confirmation wrong -- not the server we paired with", rsp);

   /* step 4: prove we had the right code. */
   hmac_sha256(key, 16, (const uint8_t *)"pancra-confirm-client",
               strlen("pancra-confirm-client"), mac);
   hex_of(mac, 32, machex);
   snprintf(confirm, sizeof confirm, "%.32s", machex);
   if (spoil)
      confirm[0] = confirm[0] == 'a' ? 'b' : 'a';
   n = snprintf(body, sizeof body, "%s\n%s\n", session, confirm);
   if (http_do(host, port, "POST", "/v1/pair/4", NULL, body, (size_t)n, &rsp,
               &rlen) != 200) {
      if (spoil) { /* expected: print what the server said and stop */
         fwrite(rsp, 1, rlen, stdout);
         return 0;
      }
      die("pairing not confirmed", rsp);
   }
   long uid = strtol(rsp, NULL, 10);

   char keyhex[33];
   hex_of(key, 16, keyhex);
   printf("%ld %s\n", uid, keyhex);
   jpake_free(p);
   return 0;
}

static int do_req(const char *host, int port, const char *uid,
                  const char *keyhex, const char *method, const char *path,
                  const char *bodyfile)
{
   /* Two pointers, because the body is a string literal when there is no file
    * and a buffer we fill when there is. One `char *` serving both meant
    * casting the literal's const away and then writing through the result --
    * which compiles and is undefined to execute, and is why -Wcast-qual was
    * not on. */
   const char *body = "";
   char *owned      = NULL;
   size_t blen      = 0;
   if (bodyfile) {
      FILE *f = fopen(bodyfile, "rb");
      if (!f)
         die("cannot open body file", bodyfile);
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      owned = malloc((size_t)sz + 1);
      if (!owned)
         die("out of memory reading", bodyfile);
      blen        = fread(owned, 1, (size_t)sz, f);
      owned[blen] = '\0';
      fclose(f);
      body = owned;
   }
   uint8_t key[16];
   if (strlen(keyhex) != 32 || !hex_to(keyhex, 32, key))
      die("bad key hex", keyhex);

   /* A FIXED nonce, when the caller names one.
    *
    * The server spends each nonce once, so a replayed request must be
    * refused -- and that control was pinned by nothing: deleting the
    * nonce_fresh() call left every signed request infinitely replayable and
    * the whole suite green. Testing it needs two requests that are byte-for-
    * byte identical, which a freshly generated nonce can never produce. This
    * is a test tool (see the header), so it takes the nonce from the
    * environment when asked and generates one otherwise. */
   char nonce[24];
   const char *fixed = getenv("SYNCCLI_NONCE");
   if (fixed && *fixed)
      snprintf(nonce, sizeof nonce, "%s", fixed);
   else
      rnd_hex(nonce, 16);
   long ts = (long)time(NULL);
   char bodyhash[65];
   sha256_hex(body, blen, bodyhash);
   char signing[1200];
   int n = snprintf(signing, sizeof signing, "%s\n%s\n%ld\n%s\n%s", method,
                    path, ts, nonce, bodyhash);
   uint8_t mac[32];
   char machex[65];
   hmac_sha256(key, 16, (const uint8_t *)signing, (size_t)n, mac);
   hex_of(mac, 32, machex);
   char extra[512];
   snprintf(extra, sizeof extra, "Authorization: Pancra %s:%ld:%s:%s\r\n", uid,
            ts, nonce, machex);

   char *rsp;
   size_t rlen;
   int code = http_do(host, port, method, path, extra, body, blen, &rsp, &rlen);
   fwrite(rsp, 1, rlen, stdout);
   return code == 200 ? 0 : 1;
}

int main(int argc, char **argv)
{
   if (argc < 4) {
      fprintf(stderr, "usage: synccli <host> <port> pair <email> <code>\n"
                      "       synccli <host> <port> pairbad <email> <code>\n"
                      "       synccli <host> <port> req <uid> <keyhex> <METHOD>"
                      " <path> [body-file]\n");
      return 2;
   }
   const char *host = argv[1];
   int port         = atoi(argv[2]);
   if (!strcmp(argv[3], "pair") && argc >= 6)
      return do_pair(host, port, argv[4], argv[5], 0);
   if (!strcmp(argv[3], "pairbad") && argc >= 6)
      return do_pair(host, port, argv[4], argv[5], 1);
   if (!strcmp(argv[3], "req") && argc >= 8)
      return do_req(host, port, argv[4], argv[5], argv[6], argv[7],
                    argc > 8 ? argv[8] : NULL);
   fprintf(stderr, "synccli: bad arguments\n");
   return 2;
}
