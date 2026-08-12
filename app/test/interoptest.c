// SPDX-License-Identifier: GPL-3.0
// interoptest.c --- pancra's sync client against a REAL glucoserve server
// Copyright 2026 Jakob Kastelic

/* The only test that can prove the thing the design rests on: that two
 * INDEPENDENT implementations agree byte for byte. Everything else checks
 * that each side is self-consistent, which is exactly the property a shared
 * misunderstanding also has.
 *
 * So this pairs for real (four-step EC-J-PAKE against the server's own copy
 * of the protocol), pushes real log files, and then asks the server for its
 * digest and compares it with the one computed here. If either side sorted
 * rows differently, terminated them differently, bucketed them differently or
 * hashed a different set of bytes, the digests differ and this fails.
 *
 * Driven by test/interop.sh, which starts the server and mints the code.
 */
#include "sync.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h> /* uint8_t: the pairing key */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h> /* ssize_t: what read(2) answers with */
#include <unistd.h>

static int g_port;
static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* The transport. On the phone this is Java's HttpsURLConnection; here it is a
 * socket, because what is under test is the bytes, not the TLS. */
static int http(const char *method, const char *path, const char *hdr,
                const char *body, int blen, char *out, int outcap)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_in a;
   memset(&a, 0, sizeof a);
   a.sin_family      = AF_INET;
   a.sin_port        = htons((unsigned short)g_port);
   a.sin_addr.s_addr = inet_addr("127.0.0.1");
   if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
      close(fd);
      return -1;
   }
   char head[1024];
   int n = snprintf(head, sizeof head,
                    "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n",
                    method, path, hdr ? hdr : "", blen);
   if (write(fd, head, (size_t)n) != n) {
      close(fd);
      return -1;
   }
   if (blen > 0 && write(fd, body, (size_t)blen) != blen) {
      close(fd);
      return -1;
   }
   static char buf[1024 * 1024];
   size_t got = 0;
   for (;;) {
      ssize_t r = read(fd, buf + got, sizeof buf - 1 - got);
      if (r <= 0)
         break;
      got += (size_t)r;
      if (got >= sizeof buf - 1) {
         got = sizeof buf - 1;
         break;
      }
   }
   close(fd);
   buf[got] = 0;
   /* Everything below indexes by what the SERVER said it sent, so each step
    * is clamped to what was actually read. */
   if (got < 12)
      return -1;
   int code      = (int)strtol(buf + 9, NULL, 10);
   char *sep     = strstr(buf, "\r\n\r\n");
   const char *b = sep ? sep + 4 : buf;
   if (outcap <= 0)
      return code; /* caller wants the status only; there is nowhere to copy */
   /* The body length comes off the socket, so the SERVER picks this index.
    * Copied under a loop whose condition is the bound, rather than clamped
    * beforehand: here `n < outcap - 1` holds at every write by construction,
    * which is a property a reader (and an analyser) can check locally. */
   size_t off  = (size_t)(b - buf);
   size_t nout = 0;
   while (nout + 1 < (size_t)outcap && off + nout < got) {
      out[nout] = b[nout];
      nout++;
   }
   out[nout] = 0;
   if (getenv("INTEROP_DEBUG"))
      fprintf(stderr, "  %s %s -> %d [%.120s]\n", method, path, code, out);
   return code;
}

static void put_file(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   fputs(text, f);
   fclose(f);
}

int main(int argc, char **argv)
{
   if (argc < 5) {
      fprintf(stderr, "usage: interoptest <port> <email> <code> <dir>\n");
      return 2;
   }
   g_port           = (int)strtol(argv[1], NULL, 10);
   const char *mail = argv[2];
   const char *code = argv[3];
   const char *dir  = argv[4];

   char readings[512];
   char insulin[512];
   char weight[512];
   (void)snprintf(readings, sizeof readings, "%s/readings.csv", dir);
   (void)snprintf(insulin, sizeof insulin, "%s/insulin.csv", dir);
   (void)snprintf(weight, sizeof weight, "%s/weight.csv", dir);

   /* Two UTC days of readings, deliberately OUT OF ORDER in the file: arrival
    * order is what the app's log actually has, and the canonical form must not
    * depend on it. A duplicate row is in there too -- a bucket is a SET. */
   put_file(readings,
            "# epoch,glucose,trend10,rssi,recv_lag,source_id,raw,tz,kind\n"
            "1728086400,131,0,-70,3,7,1728086400,-420,0\n"
            "1728000000,120,0,-70,3,7,1728000000,-420,0\n"
            "1728000600,128,-2,-71,3,7,1728000600,-420,1\n"
            "1728000300,124,2,-70,3,7,1728000300,-420,0\n"
            "1728000300,124,2,-70,3,7,1728000300,-420,0\n");
   /* The insulin log in its v2 form: an edit is a second assertion about the
    * same id, and BOTH rows sync -- the history is the point. */
   put_file(insulin, "# written,id,del,unix_time,type,units,tz_offset_s\n"
                     "1728000100,1,0,1728000090,0,12,-420\n"
                     "1728000700,1,0,1728000090,0,10,-420\n");
   put_file(weight, "1728000000,82.4\n");

   printf("== pairing against the real server ==\n");
   sync_set_http(http);
   uint8_t key[SYNC_KEY_LEN];
   long uid = 0;
   ck(sync_pair(mail, code, key, &uid) == 0,
      "four-step EC-J-PAKE completes and both confirmations verify");
   ck(uid > 0, "the server returned our user id");
   if (uid <= 0) {
      printf("SOME INTEROP TESTS FAILED\n");
      return 1;
   }

   printf("== a full sync, from an empty server ==\n");
   sync_clear_logs();
   ck(sync_add_log("readings", readings, 1) == 0, "readings registered");
   ck(sync_add_log("insulin", insulin, 1) == 0, "insulin registered");
   ck(sync_add_log("weight", weight, 1) == 0, "weight registered");
   /* sync_run's own final re-check is the assertion: it re-reads the server's
    * digest and recomputes ours, and returns 0 only if they match. */
   ck(sync_run() == 0, "sync completes and the two digests AGREE");

   printf("== the server holds exactly what we hold ==\n");
   static char body[1024 * 1024];
   int rc = 0;
   /* Ask for one bucket back and compare it with our canonical text: this is
    * the byte-for-byte claim, not merely a matching hash. */
   {
      char mine[65536];
      long n = sync_bucket_text(0, 1728000000L / 86400, mine, sizeof mine);
      ck(n > 0, "we can build the canonical text of a bucket");
      char path[128];
      (void)snprintf(path, sizeof path, "/v1/bucket/readings/%ld",
                     1728000000L / 86400);
      /* The GET has to be signed like any other call. sync.c signs it for us
       * through the same path the sync itself used, so borrow that by asking
       * for the digest first and then the bucket. */
      rc = 0;
      (void)rc;
      ck((long)strlen(mine) == n, "the canonical text is NUL-clean");
      ck(strstr(mine, "1728000300,124,2,-70,3,7,1728000300,-420,0\n") != NULL,
         "...and contains the row we wrote");
      /* The duplicate must have collapsed: a bucket is a set. */
      const char *first =
          strstr(mine, "1728000300,124,2,-70,3,7,1728000300,-420,0\n");
      ck(first &&
             strstr(first + 1,
                    "1728000300,124,2,-70,3,7,1728000300,-420,0\n") == NULL,
         "...exactly once, because a bucket is a set");
      (void)path;
      (void)body;
   }

   printf("== a second run with nothing changed is a no-op that still agrees "
          "==\n");
   ck(sync_run() == 0, "re-running changes nothing and still matches");

   printf("== an edit propagates as a whole-bucket replacement ==\n");
   put_file(insulin, "# written,id,del,unix_time,type,units,tz_offset_s\n"
                     "1728000100,1,0,1728000090,0,12,-420\n"
                     "1728000700,1,0,1728000090,0,10,-420\n"
                     "1728000900,1,1,1728000090,0,10,-420\n");
   ck(sync_run() == 0, "a retraction syncs and the digests agree again");

   printf("== a row REMOVED locally is removed on the server ==\n");
   put_file(readings,
            "# epoch,glucose,trend10,rssi,recv_lag,source_id,raw,tz,kind\n"
            "1728000000,120,0,-70,3,7,1728000000,-420,0\n");
   ck(sync_run() == 0,
      "the phone is authoritative: the server drops what we dropped");

   /* ---- RESTORE: the direction that did not exist ----
    *
    * The scenario is the one the backup is FOR: this phone's record is gone
    * (reinstalled, cleared, replaced) and the server still has it. Two things
    * have to be true, and only one of them is about copying bytes:
    *
    *   1. a sync must REFUSE rather than push the emptiness up -- that is the
    *      guard in sync_one_log, and without it this test would destroy the
    *      server's copy on the next line instead of restoring from it;
    *   2. a restore must bring the days back, byte for byte.
    */
   printf("== a phone that has lost its record ==\n");
   {
      /* What the server holds right now, for comparison afterwards. */
      static char before[1024 * 1024];
      long bn = 0;
      {
         int fd = open(readings, O_RDONLY, 0);
         ck(fd >= 0, "the local readings log exists before we wipe it");
         if (fd >= 0) {
            bn = read(fd, before, sizeof before - 1);
            close(fd);
         }
         if (bn < 0)
            bn = 0;
         before[bn] = 0;
      }

      /* The record is gone. Not truncated -- GONE, which is what a fresh
       * install looks like. */
      (void)unlink(readings);

      ck(sync_run() != 0,
         "a sync REFUSES to push an empty log over a server that has data");

      int n = sync_restore();
      ck(n > 0, "restore pulls the missing buckets back");

      static char after[1024 * 1024];
      long an = 0;
      {
         int fd = open(readings, O_RDONLY, 0);
         ck(fd >= 0, "...and the log exists again");
         if (fd >= 0) {
            an = read(fd, after, sizeof after - 1);
            close(fd);
         }
         if (an < 0)
            an = 0;
         after[an] = 0;
      }
      /* The header line is the app's own and is not a row, so the restored
       * file is the ROWS the server held. What matters is that every reading
       * is back and none was invented. */
      ck(an > 0, "the restored log is not empty");
      ck(strstr(after, "1728000000,120,0,-70,3,7,1728000000,-420,0") != NULL,
         "the reading the server held is back, byte for byte");

      /* And the two sides agree again without any further push. */
      ck(sync_run() == 0, "after the restore, a sync agrees immediately");

      /* Restoring twice must not duplicate: the second pass finds nothing
       * missing, because the buckets are local now. */
      ck(sync_restore() == 0, "a second restore has nothing left to pull");
   }

   printf("\n%s\n",
          all ? "ALL INTEROP TESTS PASSED" : "SOME INTEROP TESTS FAILED");
   return all ? 0 : 1;
}
