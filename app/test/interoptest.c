// SPDX-License-Identifier: GPL-3.0
// interoptest.c --- the app's sync client against the REAL server (srv/)
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
#include "clock.h" /* realtime_s: the nonce case asserts about the clock */
#include "sync.h"
#include "util.h"    /* log_note_cleared: the deletion workflow's evidence */
#include "wirevec.h" /* the protocol's permanent vectors */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h> /* uint8_t: the pairing key */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>  /* stat: a needless rewrite shows up as a new inode */
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

/* ---- THE DIGEST PARSER, DIRECTLY -------------------------------------
 *
 * There is no way to hand a real server a deliberately malformed reply, so the
 * only way to hold this parser to its contract is to call it. sync.h declares
 * it for that reason, and says so.
 *
 * Why it needs testing at all: the parser used to answer 0 for both "the reply
 * ended" and "this line is garbage", and every caller loops until 0 and then
 * treats what it has as the server's complete list. A reply cut in half was
 * therefore the whole truth, and a restore over it reported a count. */
static enum dline one(const char *text)
{
   const char *q = text;
   char nm[40];
   long cnt = 0;
   char hh[17];
   return digest_line(&q, nm, sizeof nm, &cnt, hh);
}

static void digest_cases(void)
{
   printf("== a digest line is a ROW, an END, or a REFUSAL ==\n");
   ck(one("") == DLINE_END, "an empty reply ends, cleanly");
   ck(one("20000 3 0123456789abcdef\n") == DLINE_ROW,
      "a whole row with a 16-byte hash is a row");

   /* THE CASE THE OLD PARSER COULD NOT REPORT: a final line that is
    * well-formed as far as it goes. This is what a dropped connection leaves,
    * and it is indistinguishable from a complete row until the newline is
    * asked for. */
   ck(one("20000 3 0123456789abcdef") == DLINE_BAD,
      "a last row with NO NEWLINE is truncation, not the end of the list");

   ck(one("20000 3 0123456789abcde\n") == DLINE_BAD,
      "a hash one byte short is refused, not padded");
   ck(one("20000 3 0123456789abcdef0\n") == DLINE_BAD,
      "a hash one byte long is refused, not truncated");
   ck(one("20000 x 0123456789abcdef\n") == DLINE_BAD,
      "a non-numeric count is refused");
   ck(one("20000  0123456789abcdef\n") == DLINE_BAD,
      "an EMPTY count is refused rather than read as zero");
   ck(one(" 3 0123456789abcdef\n") == DLINE_BAD, "an empty name is refused");
   ck(one("20000 3\n") == DLINE_BAD, "a row missing its hash is refused");
   ck(one("20000\n") == DLINE_BAD, "a row missing two fields is refused");
   ck(one("garbage\n") == DLINE_BAD, "a line with no separators is refused");
   /* Nineteen digits: the value is not the number written, so the row does not
    * describe the day it claims to. */
   ck(one("20000 1234567890123456789 0123456789abcdef\n") == DLINE_BAD,
      "a count too wide for a long is refused, not wrapped");
   /* AND THE OTHER SIDE OF THAT BOUNDARY, because a cap can be too tight as
    * easily as too loose and only one of the two failures is visible: a
    * digit rule narrowed to nine, or to "no leading digit that would make it
    * long", refuses a reply the server is entitled to send and the sync stops
    * with nothing to say why. Eighteen digits is the widest decimal that
    * cannot overflow a long, so eighteen must PARSE. */
   ck(one("20000 123456789012345678 0123456789abcdef\n") == DLINE_ROW,
      "...while eighteen digits, the widest that fits, is a row");
   /* A count of zero is a NUMBER, not an absent field. The rule above is that
    * an empty field is not a zero; this is the half that keeps it from
    * becoming "a zero is not a number". */
   ck(one("20000 0 0123456789abcdef\n") == DLINE_ROW,
      "a count of zero parses: an empty field is not a zero, but a zero is");

   /* ---- THE TWO SEPARATORS, PINNED WITH TWO-LINE INPUTS ----
    *
    * A field must be ended by a SPACE, and the single-line cases above do not
    * actually prove it. "20000 3\n" is refused with the separator check gone
    * too -- the parser steps over the newline, finds nothing behind it, and
    * refuses for an empty hash instead -- so the rule reads as covered while
    * being untested. Measured: with `if (*q != ' ')` after the count deleted,
    * every other case in this function still passed.
    *
    * With a SECOND line present the difference is a wrong answer rather than a
    * differently-worded refusal: the parser stitches the next line's bytes
    * into this row and consumes two lines as one, which is how a reply about
    * N buckets becomes a reply about N/2 -- and half the list is exactly what
    * the deletion loop in sync_one_log acts on. */
   ck(one("20000 3\n0123456789abcdef\n") == DLINE_BAD,
      "a count ended by a NEWLINE is refused, not completed from the next "
      "line");
   ck(one("20000\n3 0123456789abcdef\n") == DLINE_BAD,
      "...and so is a name ended by a newline");

   /* ---- A NAME THAT DOES NOT FIT THE CALLER'S BUFFER ----
    *
    * `ncap` is 40 at all three call sites and a log name is at most
    * WV_LIMIT_LOGNAME_MAX (32), so this is the case a server that has grown
    * longer names would produce -- and the answer must be a refusal rather
    * than a truncated name. A truncated one is not a name the caller cannot
    * find; for the per-log digest it is a DIFFERENT BUCKET, and "20000123"
    * cut to "20000" names a real day whose hash it then compares.
    *
    * Both sides of the boundary, because an off-by-one here refuses every
    * reply (ncap-1 rejected) or writes one byte past the buffer (ncap
    * accepted), and only the first of those is visible. */
   {
      char line[128];
      int n = 0;
      for (; n < 39; n++)
         line[n] = 'a';
      (void)snprintf(line + n, sizeof line - (size_t)n,
                     " 3 0123456789abcdef\n");
      ck(one(line) == DLINE_ROW, "a name of exactly ncap-1 bytes is a row");
      line[39] = 'a';
      (void)snprintf(line + 40, sizeof line - 40, " 3 0123456789abcdef\n");
      ck(one(line) == DLINE_BAD,
         "...and one byte longer is REFUSED, never truncated to a shorter "
         "name");
   }

   /* A VALID PREFIX FOLLOWED BY DAMAGE is the whole point: the first line must
    * parse and the second must refuse, so a caller looping to END cannot
    * mistake the damage for the end. */
   {
      const char *q = "20000 3 0123456789abcdef\n20001 4 short\n";
      char nm[40];
      long cnt = 0;
      char hh[17];
      ck(digest_line(&q, nm, sizeof nm, &cnt, hh) == DLINE_ROW,
         "a valid first line parses");
      ck(digest_line(&q, nm, sizeof nm, &cnt, hh) == DLINE_BAD,
         "...and the damaged second line REFUSES rather than ending the list");
   }
}

int main(int argc, char **argv)
{
   digest_cases();
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

   /* ---- THE GOLDEN VECTORS, through the APP's own code ------------------
    *
    * The rest of this file checks the phone against a running server, which
    * is the right test for "do these two agree" and the wrong one for "do
    * they agree about what the protocol SAYS": two implementations that
    * drifted the same way agree with each other perfectly. lib/wirevec.h
    * fixes the bytes, and every check below runs the APP'S OWN production
    * code against them -- the canonicaliser the push path uses, the path
    * builders the sync uses, the signing string signed_req signs, the tag
    * function pairing confirms with.
    *
    * FIRST, BEFORE THE PAIRING, on purpose. These used to sit at the end,
    * after a `return 1` that fires when pairing fails -- so a phone whose
    * confirmation tag had drifted failed to pair and never reached the
    * assertion that would have said why. The vectors exist to catch what the
    * mutual test cannot; they must not be gated on the mutual test passing.
    * Nothing here needs a server.
    *
    * The server's half of these same vectors is srv/test/wiretest.c (and
    * synctest.sh for the bucket hash). */
   {
      char vpath[600];
      (void)snprintf(vpath, sizeof vpath, "%s/wirevec.csv", dir);
      /* ARRIVAL ORDER, exactly as the vector states it: out of order, with
       * the duplicate. Canonicalisation is what this is testing. */
      char arrival[1024];
      int an = 0;
      for (int i = 0; i < WV_A_NROWS; i++)
         an += snprintf(arrival + an, sizeof arrival - (size_t)an, "%s\n",
                        wv_a_arrival[i]);
      put_file(vpath, arrival);
      sync_clear_logs();
      ck(sync_add_log("wirevec", vpath, 1) == 0, "the vector log registers");
      char canon[1024];
      long cn = sync_bucket_text(0, WV_A_BUCKET, canon, sizeof canon);
      ck(cn == (long)(sizeof wv_a_canon - 1),
         "the app's canonical text is the vector's length");
      ck(cn > 0 && memcmp(canon, wv_a_canon, (size_t)cn) == 0,
         "...and its bytes, sorted and deduplicated exactly as specified");

      /* ---- the ROUTES, as the app spells them ----
       *
       * The vectors' paths are the protocol's; these are what the app
       * actually builds. A test that compared the vector with a second
       * snprintf would be comparing two copies of the same mistake. */
      char built[128];
      ck(sync_path_bucket(built, sizeof built, "glucose", WV_A_BUCKET) > 0 &&
             strcmp(built, "/v1/bucket/glucose/20000") == 0,
         "the app builds the bucket route the vectors name");
      ck(sync_path_digest(built, sizeof built, "glucose") > 0 &&
             strcmp(built, "/v1/digest/glucose") == 0,
         "...and the per-log digest route");
      /* ---- KEY CONFIRMATION, against the vector ----
       *
       * The real pairing above already proved the app and the server agree
       * with EACH OTHER about these tags. This proves they agree with the
       * WRITTEN protocol, which is the failure the vectors exist for: two
       * implementations that truncate to 16 characters instead of 32 pair
       * perfectly, and with nothing else. */
      char tag[33];
      sync_confirm_tag(wv_d_key, wv_d_label_server, tag);
      ck(strcmp(tag, wv_d_confirm_server) == 0,
         "the app computes the SERVER's confirmation tag as the wire says");
      sync_confirm_tag(wv_d_key, wv_d_label_client, tag);
      ck(strcmp(tag, wv_d_confirm_client) == 0, "...and the CLIENT's");
      ck((int)strlen(tag) == WV_D_CONFIRM_HEX,
         "...to exactly 32 hex characters, no more and no fewer");
      /* AND THE LABELS THEMSELVES. The tags above prove the app hashes
       * correctly; these prove it hashes the right STRING -- with the labels
       * typed out at the call site, both implementations could be renamed
       * together and every gate stayed green. */
      ck(strcmp(SYNC_CONFIRM_LABEL_SERVER, wv_d_label_server) == 0,
         "the app's SERVER label is the wire's");
      ck(strcmp(SYNC_CONFIRM_LABEL_CLIENT, wv_d_label_client) == 0,
         "...and its CLIENT label too");

      /* ---- THE SIGNING STRING, through the app's own builder ----
       *
       * signed_req builds this for every request the phone makes; here it is
       * built for the vectors' inputs and compared byte for byte. A newline
       * appended at the end, the path taken without its query, the body
       * hashed twice -- each changes these bytes and fails here rather than
       * in the field. */
      char sig[512];
      int sn = sync_signing_string(sig, sizeof sig, wv_b_method, wv_b_path,
                                   WV_B_TS, wv_b_nonce, wv_b_bodyhash);
      ck(sn > 0 && (size_t)sn == sizeof wv_b_signing - 1 &&
             memcmp(sig, wv_b_signing, (size_t)sn) == 0,
         "the app signs METHOD LF PATH LF TS LF NONCE LF BODYHASH");
      ck(sn > 0 && sig[sn - 1] != '\n',
         "...with NO trailing newline, the easiest one to get wrong");
      sn = sync_signing_string(sig, sizeof sig, wv_c_method, wv_c_path, WV_C_TS,
                               wv_c_nonce, wv_c_bodyhash);
      ck(sn > 0 && (size_t)sn == sizeof wv_c_signing - 1 &&
             memcmp(sig, wv_c_signing, (size_t)sn) == 0,
         "...and a PUT signs the same five fields, over its BODY's hash");
      ck(sync_signing_string(sig, 8, wv_c_method, wv_c_path, WV_C_TS,
                             wv_c_nonce, wv_c_bodyhash) == 0 &&
             sig[0] == 0,
         "a signing string that would not fit is refused, never truncated");

      ck(sync_path_bucket(built, 12, "glucose", WV_A_BUCKET) == 0 &&
             built[0] == 0,
         "a path that would not fit is REFUSED, not truncated to another "
         "bucket");
   }

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
   /* Ask for one bucket back and compare it with our canonical text: this is
    * the byte-for-byte claim, not merely a matching hash. */
   {
      char mine[65536];
      long n = sync_bucket_text(0, 1728000000L / 86400, mine, sizeof mine);
      ck(n > 0, "we can build the canonical text of a bucket");
      ck((long)strlen(mine) == n, "the canonical text is NUL-clean");
      static char theirs[65536];
      long tn =
          sync_fetch_bucket(0, 1728000000L / 86400, theirs, sizeof theirs);
      ck(tn == n, "the server returned the same number of canonical bytes");
      ck(tn == n && memcmp(theirs, mine, (size_t)n) == 0,
         "server and app bucket text agree byte for byte");
      ck(strstr(mine, "1728000300,124,2,-70,3,7,1728000300,-420,0\n") != NULL,
         "...and contains the row we wrote");
      /* The duplicate must have collapsed: a bucket is a set. */
      const char *first =
          strstr(mine, "1728000300,124,2,-70,3,7,1728000300,-420,0\n");
      ck(first &&
             strstr(first + 1,
                    "1728000300,124,2,-70,3,7,1728000300,-420,0\n") == NULL,
         "...exactly once, because a bucket is a set");
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

   /* ---- WHEN MAY THE APP AUTHORISE DELETING THE SERVER'S COPY? --------
    *
    * One question with two halves, and neither is safe without the other.
    *
    * The phone is authoritative: sync_one_log PUTs an EMPTY body -- the
    * server's spelling of DELETE FROM logrow -- to every remote bucket the
    * LOCAL ENUMERATION did not produce. So "what did the local file contain?"
    * decides whether the server's copy of a day of the user's history
    * survives, and every way of getting that question wrong destroys data
    * that only the server still held.
    *
    *   REFUSING A ROW IS NOT AUTHORITY TO DELETE (item 112). The enumeration
    *   used to SKIP an over-long or invalid row and derive the canonical
    *   bucket set from what survived. One corrupt line in a day whose other
    *   rows had already been corrected away was therefore enough to delete
    *   that whole day from the server -- while the phone still held the bytes
    *   and merely could not parse them.
    *
    *   AND EMPTINESS IS NOT AUTHORITY EITHER, UNLESS SOMEONE SAYS IT IS
    *   (item 113). The refusal that protects a phone which lost its storage
    *   also refuses a user who really did delete everything, so a deliberate
    *   deletion could never converge and the server kept the copy for ever --
    *   with sync_run stopping at that log, taking every other log with it.
    *   Deliberate emptiness now leaves durable evidence, and only that
    *   evidence authorises the empty replacement.
    *
    * Every case below asserts on WHAT THE SERVER STILL HOLDS, fetched from
    * the server, not merely on the sync returning an error: a refusal that
    * happened after the deletion loop had already run would satisfy the
    * return code and would still have destroyed the record. The bytes that
    * must survive are captured BEFORE the call that could destroy them.
    *
    * Its own log, so that none of this can disturb the readings/insulin/
    * weight state that interop.sh checks in the server's database afterwards
    * -- and left EMPTY on the server at the end, because a log the server
    * still holds and this client no longer registers fails every later sync.
    */
   printf("== parser refusal, deliberate deletion, and the server's copy ==\n");
   {
      char guard[512];
      (void)snprintf(guard, sizeof guard, "%s/guard.csv", dir);
      /* Two UTC days: 1728000000 is day 20000, 1728086400 is day 20001. The
       * second day holds ONE row, which is what makes it deletable by a
       * single corrupt line -- exactly the case the old code got wrong. */
      static const char whole[] = "1728000000,a\n"
                                  "1728000300,b\n"
                                  "1728086400,c\n";
      put_file(guard, whole);
      int gi = 3; /* readings, insulin, weight, then this one */
      ck(sync_add_log("guard", guard, 1) == 0, "a log of our own registers");
      ck(sync_run() == 0, "...and syncs: the server holds both of its days");

      /* CAPTURED BEFORE THE DESTROYING CALL. Read back afterwards from the
       * mutated state, "the server holds day 20001" would be a claim about
       * whatever the run left there rather than about what it must not have
       * touched. */
      static char day1[4096];
      long n1 = sync_fetch_bucket(gi, 20001, day1, sizeof day1);
      ck(n1 > 0, "the server hands back the day with a single row in it");
      static char day0[4096];
      long n0 = sync_fetch_bucket(gi, 20000, day0, sizeof day0);
      ck(n0 > 0, "...and the day with several");

      static char got[4096];
      /* ---- (a) ONE MALFORMED ROW MUST NOT DELETE A VALID REPLICA ----
       *
       * A trailing space: row_ok refuses it because trimming would make a row
       * whose hash the two sides could never agree on. It is the only row of
       * day 20001, so the old enumeration dropped it, never listed 20001, and
       * the deletion loop erased the server's copy of that day. */
      put_file(guard, "1728000000,a\n"
                      "1728000300,b\n"
                      "1728086400,c \n");
      ck(sync_run() != 0, "a log holding one INVALID row fails the sync");
      long gn = sync_fetch_bucket(gi, 20001, got, sizeof got);
      ck(gn == n1 && memcmp(got, day1, (size_t)gn) == 0,
         "...and the server STILL HOLDS the day that row belonged to");

      /* The same rule for the other kind of damage, which is a different
       * branch: a line longer than a row may be, tracked by `over` rather
       * than refused by row_ok. */
      {
         static char longrow[SYNC_ROW_MAX + 64];
         int k = snprintf(longrow, sizeof longrow, "1728086400,");
         while (k < (int)sizeof longrow - 2)
            longrow[k++] = 'x';
         longrow[k++] = '\n';
         longrow[k]   = 0;
         char both[SYNC_ROW_MAX + 128];
         (void)snprintf(both, sizeof both, "1728000000,a\n1728000300,b\n%s",
                        longrow);
         put_file(guard, both);
      }
      ck(sync_run() != 0, "a log holding one OVER-LONG row fails the sync");
      gn = sync_fetch_bucket(gi, 20001, got, sizeof got);
      ck(gn == n1 && memcmp(got, day1, (size_t)gn) == 0,
         "...and that day survives on the server too");

      /* AN OVER-LONG LINE WHOSE TRUNCATION ENDS IN A CARRIAGE RETURN, which
       * is the only case where the over-long flag is doing work that row_ok's
       * length bound does not already do -- and it is the worst case in the
       * file. The reader keeps SYNC_ROW_MAX + 1 bytes of the line and the
       * last of them is a '\r', which the carriage-return strip removes,
       * leaving exactly SYNC_ROW_MAX characters: a length row_ok ACCEPTS. So
       * without the flag the phone does not merely drop the line, it hashes
       * and PUSHES the FRONT of it as a row -- a fabricated reading, at a
       * fabricated time, appearing on the server as though the user had
       * logged it, and indistinguishable from a real one ever after. */
      {
         static char frag[SYNC_ROW_MAX + 200];
         int k = snprintf(frag, sizeof frag, "1728000000,");
         while (k < SYNC_ROW_MAX)
            frag[k++] = 'x';
         frag[k++] = '\r'; /* the byte at SYNC_ROW_MAX, and the last one kept */
         while (k < (int)sizeof frag - 2)
            frag[k++] = 'y';
         frag[k++] = '\n';
         frag[k]   = 0;
         static char both2[sizeof frag + 64];
         (void)snprintf(both2, sizeof both2, "1728000000,a\n%s", frag);
         put_file(guard, both2);
         ck(sync_run() != 0, "a CR-truncated over-long line fails the sync");
         gn = sync_fetch_bucket(gi, 20000, got, sizeof got);
         ck(gn == n0 && memcmp(got, day0, (size_t)gn) == 0,
            "...and the server holds its own rows, not the front of that one");
         ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
            "...and the body builder refuses it rather than inventing a row");
         both2[(int)strlen(both2) - 1] = 0; /* the same, unterminated */
         put_file(guard, both2);
         ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
            "...the same when it is the file's unterminated last line");
      }

      /* THE SAME DAMAGE IN THE LAST LINE OF THE FILE, which both readers
       * handle in a separate block after the streaming loop -- and which the
       * two of them once disagreed about, so a row one admitted and the other
       * dropped deleted a day off the server. A log is user-copyable and its
       * last line need not be terminated. */
      put_file(guard, "1728000000,a\n1728000300,b\n1728086400,c ");
      ck(sync_run() != 0,
         "an invalid FINAL line with no newline fails the sync");
      gn = sync_fetch_bucket(gi, 20001, got, sizeof got);
      ck(gn == n1 && memcmp(got, day1, (size_t)gn) == 0,
         "...and the day it belonged to is still on the server");
      {
         static char longrow[SYNC_ROW_MAX + 64];
         int k = snprintf(longrow, sizeof longrow, "1728086400,");
         while (k < (int)sizeof longrow - 1)
            longrow[k++] = 'x';
         longrow[k] = 0;
         char both[SYNC_ROW_MAX + 128];
         (void)snprintf(both, sizeof both, "1728000000,a\n1728000300,b\n%s",
                        longrow);
         put_file(guard, both);
      }
      ck(sync_run() != 0,
         "an OVER-LONG final line with no newline fails the sync");
      gn = sync_fetch_bucket(gi, 20001, got, sizeof got);
      ck(gn == n1 && memcmp(got, day1, (size_t)gn) == 0,
         "...and that day survives it as well");

      /* THE SAME RULE IN THE BODY BUILDER, which is the reader the PUT goes
       * through and a different function from the enumeration. Here the
       * malformed row shares its day with a good one, so the enumeration
       * would list the day either way: what the old code did was hand back a
       * SHORT body for it, and a short body is adopted verbatim by a server
       * that replaces. The reading the parser could not read would then be
       * deleted from the backup as well.
       *
       * Reached directly, because with the enumeration hardened it refuses
       * first inside a full sync -- see the report: this is the only way to
       * put a malformed row in front of log_scan. */
      put_file(guard, "1728000000,a\n1728000000,zz \n");
      ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
         "the body builder REFUSES a bucket holding a malformed row rather "
         "than building it short");
      put_file(guard, "1728000000,a\n1728000000,zz ");
      ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
         "...including when the damage is the unterminated LAST line");
      {
         static char longrow[SYNC_ROW_MAX + 64];
         int k = snprintf(longrow, sizeof longrow, "1728000000,");
         while (k < (int)sizeof longrow - 2)
            longrow[k++] = 'x';
         longrow[k++] = '\n';
         longrow[k]   = 0;
         char both[SYNC_ROW_MAX + 128];
         (void)snprintf(both, sizeof both, "1728000000,a\n%s", longrow);
         put_file(guard, both);
         ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
            "...and when it is an over-long row sharing the bucket");
         both[(int)strlen(both) - 1] = 0; /* the same row, unterminated */
         put_file(guard, both);
         ck(sync_bucket_text(gi, 20000, got, (long)sizeof got) < 0,
            "...over-long and unterminated, which is a fourth branch");
      }
      /* And a genuinely empty file is still a genuinely empty answer: the
       * distinction item 112 turns on. Blank lines are not damage. */
      put_file(guard, "\n\n\n");
      ck(sync_bucket_text(gi, 0, got, (long)sizeof got) == 0,
         "...while blank lines are not damage: an empty log reads as empty");

      /* ---- ITEM 114: A BUCKET NUMBER IS NOT EVIDENCE OF A WHOLE BUCKET ----
       *
       * The third angle on the same question. 112 says a parser rejection is
       * not evidence of deletion; 113 says only a durable tombstone is
       * evidence of an intentional one; this says the mere EXISTENCE of a
       * bucket locally is not evidence that the bucket is complete.
       *
       * Restore used to skip a remote bucket the moment a bucket of that
       * number existed here, however few rows it held -- and it parsed the
       * per-bucket hash out of the digest and threw it away, so the one
       * signal that would have shown the damage was discarded on the way in.
       * A TORN bucket (right day, rows missing) was therefore never repaired.
       *
       * And it does not stay a repair problem. The phone is authoritative, so
       * the next sync PUTs that short bucket over the server's complete copy
       * and the missing rows are deleted there too: the restore that left the
       * tear had armed exactly the loss item 112 is about.
       *
       * The state below is the isolating one: bucket 20000 exists locally,
       * so the old code skipped it; it is SHORT of two rows the server has;
       * and it holds one row of its own that the server has never seen. All
       * three have to come out right, and they are three different rules. */
      put_file(guard, "1728000000,a\n"
                      "1728000300,b\n"
                      "1728000600,c\n"
                      "1728086400,d\n");
      ck(sync_run() == 0, "a two-day log syncs whole");
      /* TORN, and not by anything the server knows about: rows b and c are
       * gone from this copy, and "1728000900,mine" is a row only this phone
       * has. No sync between here and the restore, or the tear would simply
       * be pushed. */
      static const char torn[] = "1728000000,a\n"
                                 "1728000900,mine\n"
                                 "1728086400,d\n";
      put_file(guard, torn);

      /* THE HASH THE FETCH IS CHECKED AGAINST, FIRST. A body that is not what
       * the digest promised must be refused rather than merged -- otherwise
       * the decision rests on the digest's hash while the bytes written come
       * from wherever the wire delivered them. Run before the repair so the
       * log it must not touch still holds the tear. */
      setenv("APP_FAIL_BUCKET", "1", 1);
      int br = sync_restore();
      unsetenv("APP_FAIL_BUCKET");
      ck(br < 0 && br != SYNC_RESTORE_UNSYNCED,
         "a fetched bucket whose hash is not the digest's is REFUSED");
      {
         static char keep3[65536];
         long k3 = 0;
         int f3  = open(guard, O_RDONLY, 0);
         if (f3 >= 0) {
            k3 = read(f3, keep3, sizeof keep3 - 1);
            close(f3);
         }
         if (k3 < 0)
            k3 = 0;
         keep3[k3] = 0;
         ck(k3 == (long)(sizeof torn - 1) &&
                memcmp(keep3, torn, (size_t)k3) == 0,
            "...and not merged: the log is byte for byte what it was");
      }

      /* NOW THE REPAIR. */
      int tr = sync_restore();
      ck(tr > 0, "a torn bucket is repaired, not skipped for existing");
      {
         static char fixed[65536];
         long fn = 0;
         int f4  = open(guard, O_RDONLY, 0);
         if (f4 >= 0) {
            fn = read(f4, fixed, sizeof fixed - 1);
            close(f4);
         }
         if (fn < 0)
            fn = 0;
         fixed[fn] = 0;
         ck(strstr(fixed, "1728000300,b\n") != NULL,
            "...the rows the server had and this phone did not are BACK");
         ck(strstr(fixed, "1728000600,c\n") != NULL, "...both of them");
         /* THE DIRECTION MOST LIKELY TO GO WRONG. A merge that replaced the
          * bucket rather than unioning it would delete a reading the phone
          * had and the server had never been told about -- the loss item 112
          * is about, arriving through the fix. */
         ck(strstr(fixed, "1728000900,mine\n") != NULL,
            "...while the row only this phone had SURVIVED the merge");
         /* And a row both sides held is one row, not two: the file is real
          * bytes on the user's disk, and slots.csv is read by a loader that
          * does not deduplicate. */
         const char *f1 = strstr(fixed, "1728000000,a\n");
         ck(f1 && strstr(f1 + 1, "1728000000,a\n") == NULL,
            "...and a row BOTH sides held was not duplicated into the log");
         ck(strstr(fixed, "1728086400,d\n") != NULL,
            "...with the untouched day left alone");
      }
      /* A RESTORE WITH NOTHING TO ADD DOES NOT REWRITE THE LOG, and this is
       * the state that tests it: the merge above has left this phone holding
       * everything the server holds PLUS its own row, so the two hashes still
       * differ and the restore still has to look -- it stages, it fetches,
       * and it finds every row already here. Run BEFORE the push below on
       * purpose; once the push makes the hashes equal, nothing is staged at
       * all and this would be pinning the wrong branch.
       *
       * The assertion is on the file's IDENTITY, not its bytes, because a
       * needless publish produces a file with identical contents and a new
       * inode -- and spends a flash write, takes the log out from under any
       * open handle, and hands the next restore the same non-work again. */
      {
         struct stat s1;
         struct stat s2;
         int h1 = stat(guard, &s1);
         ck(sync_restore() == 0, "a restore with nothing to ADD pulls none");
         int h2 = stat(guard, &s2);
         ck(h1 == 0 && h2 == 0 && s1.st_ino == s2.st_ino,
            "...and does not rewrite the log it had nothing to add to");
      }

      /* THE REPAIR CONVERGES. The phone now holds everything the server does
       * plus its own row, so a push adds that row and the two agree. */
      ck(sync_run() == 0, "the repaired log syncs and the digests agree");
      {
         struct stat s3;
         struct stat s4;
         int h3 = stat(guard, &s3);
         ck(sync_restore() == 0, "and once both sides match, none either");
         int h4 = stat(guard, &s4);
         ck(h3 == 0 && h4 == 0 && s3.st_ino == s4.st_ino,
            "...with the log still not rewritten");
      }

      /* ---- (c) AN EMPTY LOG WITH NO EVIDENCE IS STILL REFUSED ---- */
      put_file(guard, whole);
      ck(sync_run() == 0, "the log syncs again once the damage is gone");
      put_file(guard, "");
      ck(log_clear_generation(guard) == 0,
         "an empty log on its own is not evidence of anything");
      ck(sync_run() != 0, "...so an empty log with no evidence is REFUSED");
      gn = sync_fetch_bucket(gi, 20000, got, sizeof got);
      ck(gn == n0 && memcmp(got, day0, (size_t)gn) == 0,
         "...and the server's copy is untouched: this is the lost-phone case");

      /* A TOMBSTONE THAT OUTLIVES ITS LOG AUTHORISES NOTHING. Clearing a log
       * leaves an empty FILE; losing the storage leaves a HOLE. If evidence
       * could speak for a hole, a reinstall that happened to preserve one
       * small file would delete the whole record. */
      ck(log_note_cleared(guard) == REPLACE_OK,
         "the deletion workflow records the clear, durably");
      ck(log_clear_generation(guard) == 1, "...as generation 1");
      (void)unlink(guard);
      ck(log_clear_generation(guard) == 0,
         "a MISSING log is a hole, not a deliberate clear: no authority");
      ck(sync_run() != 0, "...so a phone that lost the file is still refused");
      gn = sync_fetch_bucket(gi, 20000, got, sizeof got);
      ck(gn == n0 && memcmp(got, day0, (size_t)gn) == 0,
         "...with the server's copy untouched again");

      /* ---- (b) A DELIBERATE DELETE-ALL, WITH EVIDENCE, CONVERGES ---- */
      put_file(guard, "");
      ck(log_clear_generation(guard) == 1,
         "the evidence applies once the empty log is really there");
      ck(sync_run() == 0, "a deliberate delete-all WITH evidence converges");
      ck(sync_fetch_bucket(gi, 20000, got, sizeof got) == 0,
         "...and the server's rows really went: the day with several");
      ck(sync_fetch_bucket(gi, 20001, got, sizeof got) == 0,
         "...and the day with one");

      /* ---- WHERE THE ENUMERATION ANSWERS ALONE ----
       *
       * WHY THIS CASE HAD TO BE FOUND BY MUTATION. The rule of item 112 lives
       * in TWO readers -- log_buckets, which lists the days, and log_scan,
       * which builds the body -- and they read the same file, so in almost
       * every state each MASKS the other: revert the refusal in one and the
       * other still refuses the same bytes, and every assertion above stays
       * green. Reverting BOTH is what reproduces the original data loss
       * (measured: the server's day is deleted), and that is the honest
       * mutant for the rule. But it leaves each site individually unpinned,
       * which is how one of them silently loses its guard in a later edit.
       *
       * This is the one state where the enumeration is acted on and the body
       * builder never runs: a log the SERVER HOLDS NOTHING FOR, whose only
       * row is damaged. Zero remote buckets means no deletion loop and no
       * window to scan, so nothing else reads the file -- log_buckets alone
       * decides, and read as "this log has no days" the run reports that both
       * sides provably hold the same bytes over a log the phone cannot read.
       * That sentence is the guarantee sync.h makes; it must not be available
       * to a file nobody could parse.
       *
       * The server holds nothing for this log right now, which the case above
       * has just proved. All four damage shapes, because the enumeration
       * handles the streaming loop and the unterminated last line in separate
       * blocks and they have gone out of step before. */
      {
         static char one[SYNC_ROW_MAX + 64];
         int k = snprintf(one, sizeof one, "1728000000,");
         while (k < (int)sizeof one - 2)
            one[k++] = 'x';
         one[k++] = '\n';
         one[k]   = 0;
         put_file(guard, "1728000000,damaged \n");
         ck(sync_run() != 0,
            "a log whose only row is INVALID fails the sync rather than "
            "reporting that both sides agree");
         put_file(guard, "1728000000,damaged ");
         ck(sync_run() != 0, "...the same when it is the unterminated last "
                             "line, which the enumeration reads separately");
         put_file(guard, one);
         ck(sync_run() != 0, "...and when the only row is OVER-LONG");
         one[(int)strlen(one) - 1] = 0;
         put_file(guard, one);
         ck(sync_run() != 0, "...over-long and unterminated, the fourth "
                             "branch of the enumeration");
      }

      /* ---- THE EVIDENCE MUST NOT OUTLIVE THE EMPTINESS ----
       *
       * Rows come back. A tombstone left lying beside the log would then
       * authorise the NEXT emptiness -- which nobody asked for and which is
       * the storage loss this whole guard exists for. */
      put_file(guard, whole);
      ck(sync_run() == 0, "rows added after a clear sync normally");
      static char day0b[4096];
      long n0b = sync_fetch_bucket(gi, 20000, day0b, sizeof day0b);
      ck(n0b > 0, "...and the server holds them again");
      put_file(guard, "");
      ck(log_clear_generation(guard) == 0,
         "the old tombstone did not survive the log having rows again");
      ck(sync_run() != 0,
         "...so an emptiness nobody authorised is refused once more");
      gn = sync_fetch_bucket(gi, 20000, got, sizeof got);
      ck(gn == n0b && memcmp(got, day0b, (size_t)gn) == 0,
         "...leaving the server's copy of the re-added rows intact");

      /* A FRESH clear is authorised, and the server is left holding nothing
       * for this log -- which is also what makes it safe to stop registering
       * it below. */
      ck(log_note_cleared(guard) == REPLACE_OK,
         "the workflow can record a new clear");
      ck(log_clear_generation(guard) == 1,
         "...minted fresh: the count restarts once the log has had rows");
      ck(sync_run() == 0, "and the new deletion converges too");
      ck(sync_fetch_bucket(gi, 20000, got, sizeof got) == 0,
         "...with the server emptied for this log");

      /* ---- WHAT THE EVIDENCE ITSELF PROMISES ---- */
      ck(log_note_cleared(guard) == REPLACE_OK &&
             log_clear_generation(guard) == 2,
         "a second clear with no rows in between mints generation 2");
      /* DURABILITY IS ITS OWN ANSWER. The rename happened, so the tombstone
       * IS on disk and readable; only its survival across a power cut is
       * unproven. Reported as a failure, the deletion would look unrecorded
       * while the file records it. */
      setenv("APP_FAIL_DIRSYNC", "1", 1);
      enum replace_result ur = log_note_cleared(guard);
      unsetenv("APP_FAIL_DIRSYNC");
      ck(ur == REPLACE_UNSYNCED,
         "a clear whose directory sync fails is CHANGED-BUT-UNCERTAIN");
      ck(log_clear_generation(guard) == 3,
         "...and the evidence is really there, whatever the answer said");
      /* A rename that never happened changed nothing, and the previous
       * generation still stands. */
      setenv("APP_FAIL_RENAME", "1", 1);
      enum replace_result fr = log_note_cleared(guard);
      unsetenv("APP_FAIL_RENAME");
      ck(fr == REPLACE_FAILED, "a clear that could not be published FAILS");
      ck(log_clear_generation(guard) == 3, "...and mints nothing");
      /* AND EVIDENCE FOR A STATE THAT DOES NOT EXIST IS NOT MINTABLE. */
      put_file(guard, whole);
      ck(log_note_cleared(guard) == REPLACE_FAILED,
         "a log that still holds rows cannot be declared deliberately empty");

      (void)unlink(guard);
      {
         char cp[600];
         (void)snprintf(cp, sizeof cp, "%s%s", guard, LOG_CLEAR_SUFFIX);
         (void)unlink(cp);
      }
      /* Back to the three logs the rest of this file and interop.sh are
       * about. The server holds nothing for "guard", so its digest never
       * names a log this client no longer registers. */
      sync_clear_logs();
      ck(sync_add_log("readings", readings, 1) == 0, "readings re-registered");
      ck(sync_add_log("insulin", insulin, 1) == 0, "insulin re-registered");
      ck(sync_add_log("weight", weight, 1) == 0, "weight re-registered");
      ck(sync_run() == 0, "and the three of them still agree with the server");
   }

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

      /* ---- STAGED, SO A FAILURE CHANGES NOTHING ----------------------
       *
       * The old restore opened the live log O_APPEND and wrote each bucket
       * straight into it, dropping close()'s result and syncing nothing --
       * and its own comment admitted that a short write left a torn row,
       * which it then returned -1 over. These logs are append-only and never
       * rewritten, so that torn row is permanent.
       *
       * The rename is made to fail here, which is the one failure that can be
       * arranged from outside and stands for all of them: whatever goes wrong
       * after the copy, the live log must be byte-for-byte what it was. */
      (void)unlink(readings);
      ck(sync_restore() > 0, "a restore into a missing log works");
      static char stage_base[1024 * 1024];
      long base_n = 0;
      {
         int fd = open(readings, O_RDONLY, 0);
         if (fd >= 0) {
            base_n = read(fd, stage_base, sizeof stage_base - 1);
            close(fd);
         }
         if (base_n < 0)
            base_n = 0;
         stage_base[base_n] = 0;
      }
      ck(base_n > 0,
         "...and the log it produced is the baseline for what follows");

      (void)unlink(readings);
      setenv("APP_FAIL_RENAME", "1", 1);
      int rr = sync_restore();
      unsetenv("APP_FAIL_RENAME");
      ck(rr < 0, "a restore whose publish fails is REPORTED as a failure");
      ck(rr != SYNC_RESTORE_UNSYNCED,
         "...and not as changed-but-uncertain: nothing was published");
      {
         /* The staging file must not be left behind masquerading as data, and
          * the log must not have been created by a restore that failed. */
         int fd = open(readings, O_RDONLY, 0);
         ck(fd < 0, "...leaving the log exactly as it was: absent");
         if (fd >= 0)
            close(fd);
         char stg[600];
         int sn = snprintf(stg, sizeof stg, "%s.restore", readings);
         ck(sn > 0 && sn < (int)sizeof stg, "...the staging path fits");
         int sf = open(stg, O_RDONLY, 0);
         ck(sf < 0, "...and no staging file left behind as debris");
         if (sf >= 0)
            close(sf);
      }

      /* A WRITE THAT FAILS LEAVES THE ORIGINAL EXACTLY AS IT WAS. This is the
       * failure the old code handled worst: it appended straight into the live
       * log, so a short write tore a row in a file that is never rewritten and
       * the refusal that followed could not undo it.
       *
       * WHICH WRITE THIS ACTUALLY FAILS, precisely, because the distinction
       * matters: APP_FAIL_WRITE fails EVERY write_all, and the first one the
       * restore performs is copy_into's -- so what is exercised here is
       * "staging the copy failed", not "the third of five buckets failed".
       * Both end at the same refusal and the same untouched original, but the
       * per-bucket branch is NOT covered by this case: failing a later write
       * means letting an earlier one succeed, which this switch cannot
       * express. The COUNTED fault below covers it -- see APP_FAIL_WRITE_AFTER
       * there and in app/util.c. Said plainly rather than implied, so nobody
       * reads this case as proof of more than it shows. */
      /* A log that EXISTS and is INCOMPLETE, which is the only state in which
       * a restore stages anything: complete, it finds nothing missing and
       * never opens a staging file at all. One row of the real content, so
       * "untouched" is a claim about real data rather than an empty file. */
      static const char partial[] =
          "# epoch,glucose,trend10,rssi,recv_lag,source_id,raw,tz,kind\n";
      {
         int fd = open(readings, O_WRONLY | O_CREAT | O_TRUNC, 0600);
         ck(fd >= 0, "a partial log can be staged for the write-failure case");
         if (fd >= 0) {
            ck(write(fd, partial, sizeof partial - 1) ==
                   (long)(sizeof partial - 1),
               "...and written");
            close(fd);
         }
      }
      setenv("APP_FAIL_WRITE", "1", 1);
      int wr = sync_restore();
      unsetenv("APP_FAIL_WRITE");
      ck(wr < 0 && wr != SYNC_RESTORE_UNSYNCED,
         "a restore whose staged WRITE fails is refused outright");
      {
         static char keep[1024 * 1024];
         long kn = 0;
         int fd  = open(readings, O_RDONLY, 0);
         if (fd >= 0) {
            kn = read(fd, keep, sizeof keep - 1);
            close(fd);
         }
         if (kn < 0)
            kn = 0;
         keep[kn] = 0;
         ck(kn == (long)(sizeof partial - 1) &&
                memcmp(keep, partial, (size_t)kn) == 0,
            "...and the live log is untouched: not one torn row");
      }
      /* A DIGEST THAT ARRIVES DAMAGED. The parser refuses the line (proven
       * above by calling it directly); what is proven HERE is that the caller
       * acts on that refusal instead of reading it as the end of the list.
       * Read as the end, the restore pulls back only the buckets named in the
       * surviving prefix and reports a count -- the user is told their record
       * is back when it is short by however much the damage hid.
       *
       * The log is absent, so a restore that runs at all must create it; if
       * the damage is refused, nothing is created. */
      (void)unlink(readings);
      setenv("APP_FAIL_DIGEST", "1", 1);
      int dr = sync_restore();
      unsetenv("APP_FAIL_DIGEST");
      ck(dr < 0 && dr != SYNC_RESTORE_UNSYNCED,
         "a restore over a DAMAGED digest is refused, not partly performed");
      {
         int fd = open(readings, O_RDONLY, 0);
         ck(fd < 0, "...and no log is written from the prefix it could read");
         if (fd >= 0)
            close(fd);
      }

      /* ---- A ROW THAT LANDS WHILE THE RESTORE IS DOWNLOADING -----------
       *
       * The staged design snapshots the log, then spends one GET per missing
       * bucket -- minutes, for a year of history -- and then renames. A reading
       * appended in that window is in the original and not in the staging file,
       * so the rename would DELETE a reading the app had already acknowledged
       * and drawn. The old append-in-place restore could not lose a row that
       * way, which makes this the one regression the staging must not have.
       *
       * THE APPEND HAPPENS AFTER THE SNAPSHOT, which is the only ordering that
       * tests anything: a row already present when the copy runs is carried by
       * the copy, so asserting on one proves nothing about the catch-up.
       * APP_RESTORE_APPEND puts a row in between -- exactly where a reading
       * would land -- and the catch-up is what has to find it. Verified by
       * mutation: with the catch-up removed, the row is gone. */
      (void)unlink(readings);
      {
         static const char mine[] =
             "1799999999,142,0,-70,3,7,1799999999,-420,0\n";
         setenv("APP_RESTORE_APPEND", mine, 1);
         int rv = sync_restore();
         unsetenv("APP_RESTORE_APPEND");
         ck(rv > 0, "a restore with a row landing mid-download works");
         static char both[1024 * 1024];
         long bothn = 0;
         int bfd    = open(readings, O_RDONLY, 0);
         int fd     = bfd;
         if (fd >= 0) {
            bothn = read(fd, both, sizeof both - 1);
            close(fd);
         }
         if (bothn < 0)
            bothn = 0;
         both[bothn] = 0;
         ck(strstr(both, "1799999999,142,") != NULL,
            "...and the row that landed mid-download SURVIVED the publish");
         ck(strstr(both, "1728000000,120,0,-70,3,7,1728000000,-420,0") != NULL,
            "...alongside the rows the server restored");
      }

      /* Back to an absent log for the case below, so what it produces is
       * comparable with the baseline captured earlier. Restoring it here
       * instead would leave the log COMPLETE, the next restore would find
       * nothing missing, and the durability case would never reach the rename
       * it exists to fail -- passing for the wrong reason. */
      (void)unlink(readings);

      /* ---- PUBLISHED, BUT NOT PROVEN DURABLE ------------------------
       *
       * The directory fsync after the rename fails. The rows ARE in the file
       * -- the rename happened -- so reporting a failure would tell the user
       * nothing came back over a log that has everything in it, and reporting
       * plain success would promise the record survives a reboot. Its own
       * answer, and the rows are really there. */
      setenv("APP_FAIL_DIRSYNC", "1", 1);
      int ur = sync_restore();
      unsetenv("APP_FAIL_DIRSYNC");
      ck(ur == SYNC_RESTORE_UNSYNCED,
         "a restore whose post-rename sync fails is changed-but-UNCERTAIN");
      {
         static char now[1024 * 1024];
         long nn = 0;
         int fd  = open(readings, O_RDONLY, 0);
         if (fd >= 0) {
            nn = read(fd, now, sizeof now - 1);
            close(fd);
         }
         if (nn < 0)
            nn = 0;
         now[nn] = 0;
         ck(nn == base_n && memcmp(now, stage_base, (size_t)nn) == 0,
            "...and the rows are all there, byte for byte, despite the answer");
      }

      /* ---- ONE BUCKET ALREADY STAGED, AND THE NEXT ONE FAILING --------
       *
       * The write case above fails EVERY write, so the first failure is
       * copy_into's and the restore is refused before the per-bucket loop is
       * ever entered. That left the branch this whole design turns on -- ALL
       * OF IT OR NONE OF IT, decided when part of it is already in the
       * staging file -- with no coverage at all. Measured: replacing that
       * refusal with `if (0)`, so a staged log short by the buckets that
       * failed gets published and counted, passed every assertion in this
       * file.
       *
       * APP_FAIL_WRITE_AFTER=<n> is the counted fault that reaches it: the
       * first n writes go through and the next one fails. The server holds two
       * buckets here (the '#' header in bucket 0 and the surviving day in
       * 20000), so a local log holding NEITHER of them -- one row from a third
       * day -- makes the restore perform exactly three writes: the copy, then
       * one bucket, then the other. Two through, the third refused.
       *
       * Deliberately not fragile about that count. The local log is one short
       * row, far inside copy_into's 4096-byte chunk, so the copy is one write;
       * if that ever stopped being true the fault would land on an earlier
       * write and this case would still assert a refusal over an untouched
       * log, losing its reach into the per-bucket branch rather than turning
       * into a false pass.
       *
       * The row is a day the server does not hold, so nothing here is pushed
       * and the server's own state -- which interop.sh checks from outside
       * afterwards -- is left exactly as the cases above left it. */
      static const char otherday[] =
          "1727913600,100,0,-70,3,7,1727913600,-420,0\n";
      put_file(readings, otherday);
      setenv("APP_FAIL_WRITE_AFTER", "2", 1);
      int pr = sync_restore();
      unsetenv("APP_FAIL_WRITE_AFTER");
      ck(pr < 0 && pr != SYNC_RESTORE_UNSYNCED,
         "a restore whose LAST bucket fails to write is refused, not "
         "published short");
      {
         static char keep2[1024 * 1024];
         long k2 = 0;
         int fd  = open(readings, O_RDONLY, 0);
         if (fd >= 0) {
            k2 = read(fd, keep2, sizeof keep2 - 1);
            close(fd);
         }
         if (k2 < 0)
            k2 = 0;
         keep2[k2] = 0;
         /* Not "the log is intact" but "the log does not hold the bucket that
          * DID get written": that byte-for-byte comparison is what fails if a
          * partial restore is ever published. */
         ck(k2 == (long)(sizeof otherday - 1) &&
                memcmp(keep2, otherday, (size_t)k2) == 0,
            "...and the live log holds neither bucket, not even the first one");
         char stg[600];
         int sn2 = snprintf(stg, sizeof stg, "%s.restore", readings);
         ck(sn2 > 0 && sn2 < (int)sizeof stg, "...the staging path fits");
         int sf = open(stg, O_RDONLY, 0);
         ck(sf < 0, "...and the half-built staging file is gone, not left as "
                    "debris");
         if (sf >= 0)
            close(sf);
      }

      /* ---- THE SAME DAMAGE, AT THE CALLER THAT DELETES ----------------
       *
       * The restore case above proves a damaged digest does not become a
       * partial restore. This proves it does not become a DELETION, which is
       * the same parser answer read by the more dangerous caller: sync_one_log
       * PUTs an empty body -- the server's spelling of DELETE -- to every
       * remote bucket its local enumeration did not find. Damage read as the
       * end of the list makes buckets the server holds look absent, and the
       * phone is authoritative, so absent means "delete it".
       *
       * The state this runs in was arranged by the case above and is exactly
       * the state that makes the bug visible: locally one row from day 19999,
       * on the server the header's bucket 0 and day 20000. Read as the end of
       * the list, the run sees a server holding NOTHING, pushes 19999 (a row
       * appearing on the server that the phone never sent in a successful
       * sync, which interop.sh's external check of the database would also
       * catch) and reports agreement. Refused, it makes no request past the
       * digest at all.
       *
       * Deliberately the last thing done to this log: a run that SUCCEEDED
       * from here would legitimately delete buckets 0 and 20000, the phone
       * having really forgotten them, and the checks interop.sh makes on the
       * server's own database afterwards are about those two buckets. */
      setenv("APP_FAIL_DIGEST", "1", 1);
      int sr = sync_run();
      unsetenv("APP_FAIL_DIGEST");
      ck(sr != 0, "a SYNC over a damaged digest fails rather than reporting "
                  "that both sides agree");
      {
         static char still[65536];
         long dn =
             sync_fetch_bucket(0, 1728000000L / 86400, still, sizeof still);
         ck(dn > 0, "...and the day the server holds is still there: the "
                    "deletion loop never ran");
         long zn = sync_fetch_bucket(0, 0, still, sizeof still);
         ck(zn > 0, "...as is the header's bucket 0");
         long pn =
             sync_fetch_bucket(0, 1727913600L / 86400, still, sizeof still);
         ck(pn == 0, "...and nothing was pushed either: the run stopped at the "
                     "digest it could not read");
      }
   }

   /* ---- THE NONCE, AND WHAT IT NO LONGER DEPENDS ON -------------------
    *
    * A signed request's nonce must not repeat while the server still holds
    * it, or a LEGITIMATE request is rejected -- a sync that fails for no
    * visible reason and keeps failing. The old one was "p<wall
    * seconds>-<process counter>", which repeats in two ordinary situations:
    * two restarts inside one second (the counter restarts with the process)
    * and a clock that steps backwards (NTP after a flat battery, or a
    * hand-set date), which re-covers seconds it has already issued.
    *
    * Both cases are the SAME assertion now, because the nonce is 128 bits of
    * entropy and reads no clock and no process state at all: nothing this
    * test could do to the clock or to the process could make two of them
    * agree. So the test does what those situations do -- ask for many nonces
    * with the clock standing still, exactly as a same-second restart or a
    * rolled-back clock would -- and requires them all distinct.
    */
   {
#define NONCES 20000
      static char seen[NONCES][40];
      int dup      = 0;
      int badshape = 0;
      long t0      = realtime_s();
      for (int i = 0; i < NONCES; i++) {
         if (!sync_nonce(seen[i], (int)sizeof seen[i])) {
            badshape = 1;
            break;
         }
         /* THE WIRE'S RULE, from the wire (lib/wirevec.h): NONCE_MIN..MAX
          * characters, and only unreserved ones -- a nonce the server cannot
          * parse is a request it refuses, whatever the signature says. Read
          * from the vectors rather than typed out here, which is the same
          * mistake in miniature that the vectors exist to remove. */
         size_t ln = strlen(seen[i]);
         if (ln < WV_LIMIT_NONCE_MIN || ln > WV_LIMIT_NONCE_MAX)
            badshape = 1;
         for (size_t k = 0; k < ln; k++) {
            char c = seen[i][k];
            if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9')))
               badshape = 1;
         }
      }
      /* The whole run happens far faster than a second, so this IS the
       * same-second case; the assertion is that it does not matter. */
      long t1 = realtime_s();
      for (int i = 0; i < NONCES && !dup; i++)
         for (int j = i + 1; j < NONCES; j++)
            if (strcmp(seen[i], seen[j]) == 0) {
               dup = 1;
               break;
            }
      ck(!badshape, "every nonce is 32 hex characters the server will accept");
      ck(!dup, "...and 20000 of them, all inside one second, are all distinct");
      ck(t1 - t0 <= 1,
         "...which really was one second: the clock could not have moved");
      /* AND IT DOES NOT READ THE CLOCK AT ALL, which is what makes a
       * rollback a non-event. Two nonces taken around a clock the test
       * cannot move are still distinct -- the property above -- and nothing
       * in the generator consults the time, so there is no second case to
       * write: a rolled-back clock is this same test. */
      char a1[40];
      char a2[40];
      ck(sync_nonce(a1, sizeof a1) && sync_nonce(a2, sizeof a2) &&
             strcmp(a1, a2) != 0,
         "two consecutive nonces differ, whatever the clock says");
      ck(!sync_nonce(a1, WV_LIMIT_NONCE_MIN),
         "a buffer too small for one is refused");
   }

   /* ---- the route grammar and its boundaries, against the REAL server ----
    *
    * LAST, because two of these write. Every vector is sent as the phone
    * sends it -- signed with the app's own signing code (sync_request) when
    * the vector says signed, and with no Authorization header at all when it
    * does not -- and the status the server answers with is the one the
    * contract owes. srv/test/wiretest.c checks the same table against the
    * server's parser without a socket; this checks it against the server. */
   {
      sync_clear_logs();
      char body[2048];
      static char rsp[64 * 1024];
      int nb = snprintf(body, sizeof body, "%s", wv_a_canon);
      for (int i = 0; i < WV_E_NROUTES; i++) {
         const struct wv_route *v = &wv_e_routes[i];
         int put                  = strcmp(v->method, "PUT") == 0;
         int got =
             v->is_signed
                 ? sync_request(v->method, v->path, put ? body : "",
                                put ? nb : 0, rsp, (int)sizeof rsp)
                 : http(v->method, v->path, NULL, "", 0, rsp, (int)sizeof rsp);
         char what[160];
         (void)snprintf(what, sizeof what, "%s %s -> %d: %s", v->method,
                        v->path, v->status, v->why);
         if (got != v->status)
            (void)snprintf(what, sizeof what, "%s %s -> %d (wanted %d): %s",
                           v->method, v->path, got, v->status, v->why);
         ck(got == v->status, what);
      }
   }

   printf("\n%s\n",
          all ? "ALL INTEROP TESTS PASSED" : "SOME INTEROP TESTS FAILED");
   return all ? 0 : 1;
}
