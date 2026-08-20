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
 * answers. pancra is the real client; srv/proto.h and the handlers it
 * declares are the contract.
 *
 *   synccli <host> <port> pair <email> <code>
 *       -> "<uid> <key hex>" on success
 *   synccli <host> <port> req <uid> <keyhex> <METHOD> <path> [body-file]
 *       -> the response body
 */
#include "hmac.h"
#include "jpake.h"
#include "proto.h"
#include "util.h"
#include <arpa/inet.h>
#include <ctype.h> /* tolower: the header search is case-insensitive */
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h> /* fstat/S_ISREG: what is about to be signed */
#include <sys/time.h> /* struct timeval: every exchange has a deadline */
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

/* ---- WRITE ALL OF IT, READ ALL OF IT, AND FRAME WHAT COMES BACK ------
 *
 * This is a test client, but a test client that is loose about framing tests
 * the wrong thing: a partial write makes the server see a truncated request
 * and answer 400, which reads as a protocol failure rather than as this
 * program's own bug. Every one of the following was wrong here:
 *
 *   - a single write() for the header and another for the body, with only the
 *     sign of the return checked. A short write on a socket is ordinary, and
 *     it silently sent a prefix.
 *   - read() treated like EOF on error, so a connection reset produced the
 *     bytes received so far and a status parsed out of them.
 *   - the response buffer doubled with no ceiling, so a server (or something
 *     answering on its port) could grow it until the allocator failed.
 *   - the status line read at a fixed offset with atoi, accepting anything
 *     12 bytes long -- "HTTP/1.1 2" and a partial line included.
 *   - no Content-Length check, so a reply cut off mid-body was indistinguish-
 *     able from a complete one.
 *
 * SO_RCVTIMEO/SO_SNDTIMEO give every exchange a deadline: without one, a
 * server that accepts and never answers hangs synctest.sh rather than failing
 * it. */
#define CLI_RSP_MAX (4L * 1024 * 1024)
#define CLI_TIMEO_S 30

/* Case-insensitive search, written out rather than strcasestr: that is a GNU
 * extension, and this file is also compiled for the board's toolchain. */
static const char *find_ci(const char *hay, const char *needle)
{
   size_t n = strlen(needle);
   for (const char *p = hay; *p; p++) {
      size_t i = 0;
      while (i < n && p[i] &&
             tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == n)
         return p;
   }
   return NULL;
}

static int write_all(int fd, const void *b, size_t n)
{
   const char *p = b;
   size_t off    = 0;
   while (off < n) {
      ssize_t w = write(fd, p + off, n - off);
      if (w <= 0)
         return -1; /* including EINTR: a test client need not retry */
      off += (size_t)w;
   }
   return 0;
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
   if (fd < 0)
      return -1;
   struct timeval tv = {CLI_TIMEO_S, 0};
   (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
   (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

   char hdr[2048];
   int n = snprintf(hdr, sizeof hdr,
                    "%s %s HTTP/1.1\r\nHost: %s\r\n%s"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                    method, path, host, extra ? extra : "", blen);
   if (n <= 0 || n >= (int)sizeof hdr)
      return close(fd), -1;
   if (write_all(fd, hdr, (size_t)n) != 0)
      return close(fd), -1;
   if (blen && write_all(fd, body, blen) != 0)
      return close(fd), -1;

   size_t cap = 65536, got = 0;
   char *buf = malloc(cap);
   if (!buf)
      return close(fd), -1;
   for (;;) {
      if (got + 4096 > cap) {
         if (cap >= (size_t)CLI_RSP_MAX) {
            free(buf);
            return close(fd), -1; /* a reply this large is not this protocol */
         }
         char *bigger = realloc(buf, cap * 2);
         if (!bigger) { /* realloc leaves the old block alive; do not leak it */
            free(buf);
            return close(fd), -1;
         }
         cap *= 2;
         buf = bigger;
      }
      ssize_t r = read(fd, buf + got, cap - got - 1);
      if (r == 0)
         break; /* orderly EOF */
      if (r < 0) {
         /* AN ERROR IS NOT AN END. Treated as EOF, a reset connection or an
          * expired timeout handed back whatever had arrived and a status
          * parsed out of it. */
         free(buf);
         return close(fd), -1;
      }
      got += (size_t)r;
   }
   close(fd);
   buf[got] = '\0';

   /* THE STATUS LINE, IN FULL. "HTTP/1.x <3 digits>" and a CRLF after it --
    * not "at least twelve bytes, then atoi". */
   char *eol = strstr(buf, "\r\n");
   if (!eol || eol - buf < 12 || strncmp(buf, "HTTP/1.", 7) != 0 ||
       buf[8] != ' ' || buf[9] < '0' || buf[9] > '9' || buf[10] < '0' ||
       buf[10] > '9' || buf[11] < '0' || buf[11] > '9') {
      free(buf);
      return -1;
   }
   int code = ((buf[9] - '0') * 100) + ((buf[10] - '0') * 10) + (buf[11] - '0');

   /* THE HEADERS MUST HAVE ENDED. Without the blank line the reply is still
    * in flight, and taking the whole buffer as a body -- which is what the
    * `else` branch here used to do -- turns a truncated response into a
    * plausible one. */
   char *sep = strstr(buf, "\r\n\r\n");
   if (!sep) {
      free(buf);
      return -1;
   }
   char *bodyp  = sep + 4;
   size_t bodyn = got - (size_t)(bodyp - buf);

   /* ...AND ALL OF THE BODY ARRIVED. Content-Length is what says how much
    * there should be; a reply cut off mid-body is otherwise identical to a
    * complete one. */
   const char *cl = find_ci(buf, "\r\ncontent-length:");
   if (cl && cl < sep) {
      long want = strtol(cl + 18, NULL, 10);
      if (want < 0 || (size_t)want != bodyn) {
         free(buf);
         return -1;
      }
   }
   *out  = bodyp;
   *olen = bodyn;
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
   /* THE ADDRESS IS CHECKED HERE TOO, AND IT IS NOT DEFENCE IN DEPTH.
    *
    * The round-1 body is "<address>\n<320 hex>\n" built with snprintf into a
    * kilobyte. snprintf TRUNCATES: an address of 800 bytes did not fail, it
    * produced a body whose J-PAKE packet was cut in half, and `n` was the
    * length the body WOULD have had -- so http_do was handed a length past the
    * end of the buffer it was given. The server then refused the request for a
    * malformed packet, which is a diagnostic pointing at the crypto for a
    * problem in the address field, and the read past the end was nobody's
    * report at all. Refusing the address first makes the truncation
    * unreachable; the explicit check on `n` below makes that a fact the
    * compiler and the reader can both see rather than an argument about
    * sizes. */
   char cemail[EMAIL_BUF];
   if (!email_canon(email, cemail, sizeof cemail))
      die("that is not an address this server will accept", email);

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
   int n = snprintf(body, sizeof body, "%s\n%s\n", cemail, hex);
   if (n <= 0 || (size_t)n >= sizeof body)
      die("the pairing body does not fit", cemail);
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
      /* WHAT IS ABOUT TO BE SIGNED, checked at every step.
       *
       * The MAC covers these bytes, so a body read short is a signature over
       * something the caller never chose -- and none of fseek, ftell or fread
       * was checked here. A directory (fread returns 0 and the request goes
       * out empty), a file that grew between the size and the read, an
       * unreadable file: each produced a confidently signed wrong request. */
      FILE *f = fopen(bodyfile, "rb");
      if (!f)
         die("cannot open body file", bodyfile);
      struct stat st;
      if (fstat(fileno(f), &st) != 0)
         die("cannot stat body file", bodyfile);
      if (!S_ISREG(st.st_mode))
         die("body file is not a regular file", bodyfile);
      if (fseek(f, 0, SEEK_END) != 0)
         die("cannot seek body file", bodyfile);
      long sz = ftell(f);
      if (sz < 0)
         die("cannot size body file", bodyfile);
      if (sz > BODY_MAX)
         die("body file is larger than the protocol allows", bodyfile);
      if (fseek(f, 0, SEEK_SET) != 0)
         die("cannot rewind body file", bodyfile);
      owned = malloc((size_t)sz + 1);
      if (!owned)
         die("out of memory reading", bodyfile);
      blen = fread(owned, 1, (size_t)sz, f);
      if (blen != (size_t)sz || ferror(f))
         die("body file was not read whole", bodyfile);
      owned[blen] = '\0';
      if (fclose(f) != 0)
         die("body file did not close cleanly", bodyfile);
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
   /* THE STATUS, where a shell can assert on it.
    *
    * Only the body reached stdout, so every assertion about the signed API was
    * a substring of the body and the status was invisible: a 500 whose text
    * happened to contain the wanted word passed, and so did a 400 where a 401
    * was the actual contract. Writing it to a named file rather than stdout
    * keeps the body clean for the callers that pipe it. */
   const char *cf = getenv("SYNCCLI_CODE_FILE");
   if (cf && *cf) {
      FILE *cfp = fopen(cf, "wb");
      if (cfp) {
         fprintf(cfp, "%d", code);
         fclose(cfp);
      }
   }
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
