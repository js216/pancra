/* SPDX-License-Identifier: GPL-3.0
 * wiretest.c --- the wire contract, against permanent vectors
 * Copyright 2026 Jakob Kastelic
 *
 * The phone and the server implement this protocol independently, and their
 * end-to-end tests check them against EACH OTHER -- which cannot notice that
 * both drifted the same way, and cannot notice at all until both halves are
 * built and a server is running.
 *
 * This is the other half of that: the wire itself, pinned to fixed bytes
 * (lib/wirevec.h). What it asserts:
 *
 *   1. THE TWO HEADERS AGREE, at COMPILE time. app/sync.h and srv/proto.h
 *      state the same limits from two files on purpose; a Makefile grep used
 *      to compare exactly one of them, and it was the only mechanical check
 *      there was. The compiler compares them all now -- this is the one place
 *      both headers are included together, which is the whole reason it can.
 *   2. THE HASH OF A BUCKET is the first 16 hex of the SHA-256 of its
 *      canonical text -- over the vector's bytes, so "what the hash is taken
 *      over" is fixed rather than agreed by inspection.
 *   3. THE SIGNING STRING is exactly METHOD LF PATH LF TS LF NONCE LF
 *      BODYHASH, with no trailing newline, and the MAC over it is the vector
 *      value -- built by THE SERVER'S OWN builder (srv/sigstr.c), not by a
 *      second copy of the rule in this file. That matters: checked against a
 *      private snprintf here, the vector could not notice a newline appended
 *      to both implementations at once, which is the one drift two
 *      independent implementations cannot catch by themselves.
 *   4. THE PRIMITIVES the contract names are the ones in lib/: SHA-256 of the
 *      empty body is the digest a GET carries.
 *
 * The app's side of the same vectors is in app/test/interoptest.c, which
 * builds vector A's canonical text through the app's own log code.
 *
 * Built and run by `make wiretest`.
 */
#include "hmac.h"
#include "proto.h"
#include "route.h" /* the server's route grammar, under test */
#include "sha256.h"
#include "sigstr.h" /* ...and the bytes it signs */
#include "sync.h"   /* the APP's half of the contract, included on purpose */
#include "wirevec.h"
#include <stdio.h>
#include <string.h>

/* ---- 1: the two headers, compared by the compiler ---------------------- */
_Static_assert(SYNC_ROW_MAX == ROW_MAX,
               "a row's maximum length is the same fact on both sides: the "
               "phone would build rows the server refuses");
_Static_assert(SYNC_KEY_LEN == 16,
               "the pairing key is 16 bytes on the wire (srv/pair.c stores a "
               "16-byte blob)");
_Static_assert(SYNC_BUF_MAX <= BODY_MAX,
               "the phone must never build a bucket larger than the server "
               "will accept, or that day can never sync");
_Static_assert(SYNC_MAX_LOGS <= USER_LOGS,
               "every log the phone can hold must fit in what one user may "
               "have on the server");

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

static void hex_of_local(const unsigned char *b, int n, char *out)
{
   static const char H[] = "0123456789abcdef";
   for (int i = 0; i < n; i++) {
      out[2 * i]     = H[(b[i] >> 4) & 0xF];
      out[2 * i + 1] = H[b[i] & 0xF];
   }
   out[2 * n] = 0;
}

int main(void)
{
   printf("wiretest: the wire is what the vectors say it is\n");

   /* ---- 2: the canonical text and its hash ---- */
   /* The vector's canonical text really is its arrival rows sorted and
    * deduplicated -- checked here rather than assumed, so the fixture cannot
    * quietly stop describing the rule it exists to describe. */
   {
      const char *p = wv_a_canon;
      int nline     = 0;
      int sorted    = 1;
      char prev[128];
      prev[0] = 0;
      while (*p) {
         const char *nl = strchr(p, '\n');
         if (!nl)
            break;
         char line[128];
         size_t len = (size_t)(nl - p);
         if (len >= sizeof line)
            break;
         memcpy(line, p, len);
         line[len] = 0;
         if (prev[0] && strcmp(prev, line) >= 0)
            sorted = 0; /* >= : ascending AND no duplicate */
         snprintf(prev, sizeof prev, "%s", line);
         nline++;
         p = nl + 1;
      }
      ck(nline == WV_A_NCANON, "the canonical text has one line per row");
      ck(sorted, "...strictly ascending bytewise: sorted, and no duplicate");
      ck(wv_a_canon[sizeof wv_a_canon - 2] == '\n',
         "...and every line is terminated, including the last");
      /* Every arrival row is present, and nothing else is. */
      int present = 1;
      for (int i = 0; i < WV_A_NROWS; i++)
         if (!strstr(wv_a_canon, wv_a_arrival[i]))
            present = 0;
      ck(present, "...and every row that arrived is in it");
   }

   unsigned char h[32];
   char hex[65];
   sha256((const unsigned char *)wv_a_canon, sizeof wv_a_canon - 1, h);
   hex_of_local(h, 32, hex);
   ck(strncmp(hex, wv_a_hash16, 16) == 0,
      "a bucket's hash is the first 16 hex of the SHA-256 of its canonical "
      "text");
   ck(strlen(wv_a_hash16) == 16, "...which is sixteen characters, not more");

   /* ---- 3 and 4: the signing string and its MAC ---- */
   sha256((const unsigned char *)wv_b_body, sizeof wv_b_body - 1, h);
   hex_of_local(h, 32, hex);
   ck(strcmp(hex, wv_b_bodyhash) == 0,
      "an empty body hashes to the digest a GET carries");

   /* THE SERVER'S OWN BUILDER, not a second copy of the rule in this file.
    * Checked against a private snprintf here, the vector could not notice a
    * change made to both implementations at once -- which is the only kind of
    * drift two independent implementations cannot catch by themselves. */
   char built[512];
   int n = sig_signing_string(built, sizeof built, wv_b_method, wv_b_path,
                              WV_B_TS, wv_b_nonce, wv_b_bodyhash);
   ck(n > 0 && (size_t)n == sizeof wv_b_signing - 1 &&
          memcmp(built, wv_b_signing, (size_t)n) == 0,
      "the signing string is METHOD LF PATH LF TS LF NONCE LF BODYHASH");
   ck(wv_b_signing[sizeof wv_b_signing - 2] != '\n',
      "...with NO trailing newline, which is the easiest one to get wrong");

   unsigned char mac[32];
   hmac_sha256(wv_b_key, sizeof wv_b_key, (const unsigned char *)built,
               (size_t)n, mac);
   hex_of_local(mac, 32, hex);
   ck(strcmp(hex, wv_b_mac) == 0, "and the MAC over it is the vector's");

   /* A DIFFERENT KEY MUST NOT PRODUCE IT. Without this, a MAC function that
    * ignored its key would pass everything above. */
   unsigned char other[16];
   memcpy(other, wv_b_key, sizeof other);
   other[0] ^= 1;
   hmac_sha256(other, sizeof other, (const unsigned char *)built, (size_t)n,
               mac);
   hex_of_local(mac, 32, hex);
   ck(strcmp(hex, wv_b_mac) != 0, "...and only with THAT key");

   /* ---- 5: a signed PUT, whose body is not empty ---- */
   /* The empty-body vector alone is satisfied by an implementation that
    * hashes "" unconditionally -- which is a real bug and looks exactly like
    * a correct GET. This one carries vector A's canonical text. */
   sha256((const unsigned char *)wv_c_body, strlen(wv_c_body), h);
   hex_of_local(h, 32, hex);
   ck(strcmp(hex, wv_c_bodyhash) == 0,
      "a PUT hashes its BODY, not the empty string");
   ck(strncmp(wv_c_bodyhash, wv_a_hash16, 16) == 0,
      "...and a bucket's hash is the first 16 hex of that same digest");
   ck(strcmp(wv_c_bodyhash, wv_b_bodyhash) != 0,
      "...which is not the empty digest, or the check above proves nothing");

   n = sig_signing_string(built, sizeof built, wv_c_method, wv_c_path, WV_C_TS,
                          wv_c_nonce, wv_c_bodyhash);
   ck(n > 0 && (size_t)n == sizeof wv_c_signing - 1 &&
          memcmp(built, wv_c_signing, (size_t)n) == 0,
      "a PUT signs the same five fields in the same order");
   hmac_sha256(wv_b_key, sizeof wv_b_key, (const unsigned char *)built,
               (size_t)n, mac);
   hex_of_local(mac, 32, hex);
   ck(strcmp(hex, wv_c_mac) == 0, "and the MAC over THAT is the vector's");
   ck(strcmp(wv_c_mac, wv_b_mac) != 0,
      "...and differs from the GET's, on the same key");

   /* ---- 6: key confirmation, the pinnable half of pairing ---- */
   /* The J-PAKE packets cannot be vectors -- they are built from fresh random
    * scalars -- but the tags that prove the derived key is shared are a plain
    * HMAC over a fixed label, and they are what the two sides must agree on
    * for pairing to mean anything. */
   char tag[65];
   hmac_sha256(wv_d_key, sizeof wv_d_key,
               (const unsigned char *)wv_d_label_server,
               strlen(wv_d_label_server), mac);
   hex_of_local(mac, 32, tag);
   tag[WV_D_CONFIRM_HEX] = '\0';
   ck(strcmp(tag, wv_d_confirm_server) == 0,
      "the SERVER's confirmation tag is the vector's");
   hmac_sha256(wv_d_key, sizeof wv_d_key,
               (const unsigned char *)wv_d_label_client,
               strlen(wv_d_label_client), mac);
   hex_of_local(mac, 32, tag);
   tag[WV_D_CONFIRM_HEX] = '\0';
   ck(strcmp(tag, wv_d_confirm_client) == 0, "...and the CLIENT's is not it");
   ck(strcmp(wv_d_confirm_server, wv_d_confirm_client) != 0,
      "...the two labels are what makes them differ");
   ck((int)strlen(wv_d_confirm_server) == WV_D_CONFIRM_HEX &&
          (int)strlen(wv_d_confirm_client) == WV_D_CONFIRM_HEX,
      "a tag is 32 hex characters -- half the digest, and all of the guess");
   /* THE SERVER'S OWN CONSTANTS, from the header the server compiles. The
    * tags above are only worth something if these are the strings the server
    * actually hashes: with the labels typed out at the call sites, both
    * implementations could be renamed together and every gate stayed
    * green. */
   ck(strcmp(CONFIRM_LABEL_SERVER, wv_d_label_server) == 0,
      "the server's SERVER label is the wire's");
   ck(strcmp(CONFIRM_LABEL_CLIENT, wv_d_label_client) == 0,
      "...and its CLIENT label too");
   ck(CONFIRM_HEX == WV_D_CONFIRM_HEX,
      "and the server truncates to exactly that many");
   ck(PAIR_PKT == WV_D_PKT, "a round packet is the length the framing says");

   /* ---- 7: the route grammar, and the paths that are not routes ---- */
   /* This is the server's OWN parser (srv/route.c), not a copy of it: the
    * vectors are worth something only if the shipping code answers them. */
   for (int i = 0; i < WV_E_NROUTES; i++) {
      const struct wv_route *v = &wv_e_routes[i];
      struct route rt;
      route_of(v->path, &rt);
      int ok = 1;
      /* What the grammar owes each vector, derived from the status the wire
       * owes it: a 404 on an unsigned pairing probe means the path is not a
       * pairing round; a 400 means the bucket SHAPE with unusable contents;
       * a 405 or a 200 means it really is that route. */
      if (v->status == 400)
         ok = (rt.kind == RT_BUCKET_BAD || rt.kind == RT_DIGEST_BAD);
      else if (v->status == 404)
         ok = rt.kind == RT_NONE;
      else if (!strncmp(v->path, "/v1/pair/", 9))
         ok = rt.kind == RT_PAIR;
      else if (!strcmp(v->path, "/v1/digest"))
         ok = rt.kind == RT_DIGEST_ALL;
      else if (!strncmp(v->path, "/v1/digest/", 11))
         ok = rt.kind == RT_DIGEST_LOG;
      else if (!strncmp(v->path, "/v1/bucket/", 11))
         ok = rt.kind == RT_BUCKET;
      ck(ok, v->why);
   }
   /* The two routes that carry values must carry the RIGHT ones -- a parser
    * that classified correctly and extracted the wrong bucket would pass
    * everything above. */
   {
      struct route rt;
      route_of("/v1/bucket/glucose/20000", &rt);
      ck(rt.kind == RT_BUCKET && !strcmp(rt.log, "glucose") &&
             rt.bucket == WV_A_BUCKET,
         "a bucket route yields its log name and its bucket number");
      route_of("/v1/digest/glucose", &rt);
      ck(rt.kind == RT_DIGEST_LOG && !strcmp(rt.log, "glucose"),
         "a per-log digest yields its log name");
      /* A log name at the wire's ceiling is a route; one byte more is not.
       * The bug this pins is a parser that TRUNCATES to its buffer, which
       * would serve one log's digest for a request naming another. */
      char at[64], over[80];
      int k = snprintf(at, sizeof at, "/v1/digest/");
      for (int i = 0; i < WV_LIMIT_LOGNAME_MAX; i++)
         at[k + i] = 'g';
      at[k + WV_LIMIT_LOGNAME_MAX] = '\0';
      route_of(at, &rt);
      ck(rt.kind == RT_DIGEST_LOG &&
             (int)strlen(rt.log) == WV_LIMIT_LOGNAME_MAX,
         "a log name at the wire's ceiling is a route");
      snprintf(over, sizeof over, "%sg", at);
      route_of(over, &rt);
      ck(rt.kind == RT_DIGEST_BAD,
         "...and one byte longer is malformed, never a SHORTER name");
   }

   /* ---- 8: the limits are the wire's, on both sides ---- */
   /* The _Static_asserts in proto.h and sync.h do the enforcing; these say
    * out loud which numbers they are, so a reader of the output knows what
    * the contract currently is. */
   ck(ROW_MAX == WV_LIMIT_ROW_MAX && BODY_MAX == WV_LIMIT_BODY_MAX &&
          LOGNAME_MAX == WV_LIMIT_LOGNAME_MAX &&
          BUCKET_ROWS == WV_LIMIT_BUCKET_ROWS &&
          USER_LOGS == WV_LIMIT_USER_LOGS,
      "the server's limits are the wire's");
   ck(SYNC_ROW_MAX == WV_LIMIT_ROW_MAX && SYNC_BUF_MAX <= WV_LIMIT_BODY_MAX &&
          SYNC_MAX_LOGS <= WV_LIMIT_USER_LOGS,
      "the phone's CAPACITIES are within them, and its row length equals one");
   ck(SYNC_BUF_MAX < WV_LIMIT_BODY_MAX && SYNC_MAX_LOGS < WV_LIMIT_USER_LOGS,
      "...and are deliberately smaller: a phone holds less than a wire allows");

   printf("%s\n", fails == 0
                      ? "wiretest: both halves are held to the same bytes"
                      : "wiretest: FAILED");
   return fails == 0 ? 0 : 1;
}
