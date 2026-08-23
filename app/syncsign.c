// SPDX-License-Identifier: GPL-3.0
// syncsign.c --- the signature on every request, and the routes
// Copyright 2026 Jakob Kastelic

/* THE WIRE'S OWN LAYER: the nonce, the signing string, the HMAC, and the
 * three route builders. Everything that leaves this phone signed goes
 * through signed_req, which is why the argument checks live there rather
 * than in the public wrapper.
 *
 * ONE OF THE WORKFLOW FILES BEHIND app/sync.h. The interface has not moved
 * and neither has anything a caller can see: app/sync.c is now a coordinator
 * that owns the configuration, the one-operation-at-a-time lock and the
 * workspace, and hands a SNAPSHOT of all three (struct sync_ctx, see
 * app/syncint.h) to whichever workflow file does the work.
 *
 * NOTHING HERE READS A CONFIGURATION GLOBAL -- it cannot, they are static in
 * the coordinator. That is what the split buys and it is not cosmetic:
 * signing that reads the LIVE key and uid while the operation around it
 * works from a snapshot lets a pairing completed mid-sync sign the remainder
 * of that sync with the new account's key. */
#include "clock.h" /* realtime_s: the timestamp inside the signature */
#include "hmac.h"
#include "rand.h" /* the nonce is entropy, not a clock */
#include "sha256.h"
#include "sync.h"
#include "syncint.h" /* the workspace, and the operation's own context */
#include "syncrow.h" /* hexify / s_len: the wire's own text */
#include "wireint.h" /* PRIwire: the wire's scalars, printed exactly */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int sync_nonce(char *out, int cap)
{
   if (!out || cap < 33)
      return 0;
   uint8_t b[16];
   if (!rand_bytes(b, sizeof b)) {
      out[0] = 0;
      return 0;
   }
   hexify(b, (int)sizeof b, out);
   return 1;
}

int signed_req(const struct sync_ctx *sx, const char *method, const char *path,
               const char *body, int blen, char *out, int outcap)
{
   if (!sx || !sx->http || !sx->have_key)
      return -1;
   /* ---- THE ARGUMENTS, BEFORE ANY OF THEM IS TRUSTED ------------------
    *
    * `blen` IS SIGNED AND WAS CAST STRAIGHT TO size_t. A negative length --
    * from a caller's arithmetic going wrong, or an int overflow on a body
    * size -- became a length of nearly 2^64, and the FIRST thing done with it
    * was to hash that many bytes starting at `body`. Not a wrong signature: a
    * read of the whole address space, in the one function every outbound
    * request goes through.
    *
    * A NULL BODY WITH A NONZERO LENGTH is the same defect from the other
    * side. The `body ? body : ""` below reads as care and is the opposite of
    * it: with a null body it points the hash at a one-byte literal and then
    * reads `blen` bytes from it. Either the request has a body or it does
    * not; a length without one is a caller bug, not something to paper over.
    *
    * Checked HERE because this is the funnel: every route reaches the wire
    * through this one function, so one check covers every one of them. */
   if (!method || !path || !out || outcap <= 0)
      return -1;
   if (blen < 0 || (blen > 0 && !body))
      return -1;
   uint8_t bh[32];
   char bhex[65];
   sha256((const uint8_t *)(body ? body : ""), (size_t)blen, bh);
   hexify(bh, 32, bhex);

   char nonce[40];
   /* 128 random bits: see sync_nonce for why it is neither the clock nor a
    * counter. A phone that cannot reach its entropy source does not sign. */
   if (!sync_nonce(nonce, (int)sizeof nonce))
      return -1;

   static char msg[1400];
   int64_t ts = realtime_s();
   int n = sync_signing_string(msg, sizeof msg, method, path, ts, nonce, bhex);
   if (n <= 0)
      return -1;
   uint8_t mac[32];
   char machex[65];
   hmac_sha256(sx->key, SYNC_KEY_LEN, (const uint8_t *)msg, (size_t)n, mac);
   hexify(mac, 32, machex);
   char hdr[256];
   n = snprintf(hdr, sizeof hdr,
                "Authorization: Pancra %" PRIwire ":%" PRIwire ":%s:%s\r\n",
                sx->uid, ts, nonce, machex);
   if (n <= 0 || n >= (int)sizeof hdr)
      return -1;
   return sx->http(method, path, hdr, body, blen, out, outcap);
}

int sync_signing_string(char *out, size_t cap, const char *method,
                        const char *path, int64_t ts, const char *nonce,
                        const char *bodyhash)
{
   if (!out || cap == 0)
      return 0;
   /* EVERY FIELD IS DEREFERENCED BY %s, so every one is checked. Only the
    * output was validated here, and the four strings went straight into
    * snprintf -- where a null is undefined behaviour, not a printed "(null)".
    * glibc happens to print that and bionic need not, so the same code is a
    * harmless oddity on the host suite and a crash on the phone.
    *
    * This is the string that IS the signature's input; a field that could be
    * missing is a signature over something other than the request. */
   if (!method || !path || !nonce || !bodyhash)
      return 0;
   int n = snprintf(out, cap, "%s\n%s\n%" PRIwire "\n%s\n%s", method, path, ts,
                    nonce, bodyhash);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}

/* THE ROUTES, BUILT IN ONE PLACE. See sync.h for why. A log name longer than
 * the wire allows cannot produce a path here either -- the format is checked
 * against the buffer, and a path that did not fit is refused rather than
 * silently addressed at some other bucket. */
int sync_path_bucket(char *out, size_t cap, const char *log, int64_t bucket)
{
   if (!out || !log || cap == 0)
      return 0;
   int n = snprintf(out, cap, "/v1/bucket/%s/%" PRIwire "", log, bucket);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}

int sync_path_digest(char *out, size_t cap, const char *log)
{
   if (!out || !log || cap == 0)
      return 0;
   int n = snprintf(out, cap, "/v1/digest/%s", log);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}
