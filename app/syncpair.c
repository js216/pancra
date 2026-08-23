// SPDX-License-Identifier: GPL-3.0
// syncpair.c --- pairing: four J-PAKE rounds and the grammar of their replies
// Copyright 2026 Jakob Kastelic

/* PAIRING, which is the one exchange that is NOT signed -- it is where the
 * key comes from. Every byte of every reply is therefore chosen by whatever
 * answered the request, and the field grammar below is what makes that
 * survivable.
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
#include "dexlibc.h"
#include "jpake.h"
#include "pairtag.h" /* the confirmation tag, built in ONE place */
#include "sync.h"
#include "syncint.h" /* the workspace, and the operation's own context */
#include "syncrow.h" /* unhex / hexify / s_len / s_chr: the wire's own text */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- pairing ---------------------------------------------------------- */

/* Four steps, not three. J-PAKE's proofs validate whatever code each side
 * used, so two parties with DIFFERENT codes finish all three rounds and
 * simply derive different keys. The key cannot be derived until the peer's
 * round 3 has arrived, so neither side can prove itself inside round 3: the
 * server proves itself in the round-3 reply, and step 4 carries ours. Saving
 * a key without checking the server's proof would mean trusting whatever
 * answered the request. */
/* ---- THE PAIRING REPLY GRAMMAR, REQUIRED EXACTLY -----------
 *
 * Pairing is the ONE exchange that is not signed -- it is where the key comes
 * from -- so every byte of every reply is chosen by whatever answered the
 * request. So no round decodes a FIXED width out of the shared response
 * buffer without first proving the reply is that long, none compares the
 * confirmation tag with strncmp (which accepts a PREFIX), and none parses the
 * user id with a loop that stops at the first non-digit and ignores the
 * rest.
 *
 * THE SHARED BUFFER IS WHAT MAKES THE FIRST ONE BITE. sx->rsp is reused by
 * every round and every sync, so a short reply leaves the TAIL OF THE
 * PREVIOUS one in place -- and those bytes are perfectly good hex. The
 * decoder stopping at a NUL is only a defence while a NUL is where the reply
 * ends, which is exactly what a short reply does not guarantee.
 *
 * So each field states its own width and must end where it says it does. A
 * field ends at a newline or at the end of the string; anything else -- a
 * character too many, a character too few -- is a refusal.
 */

/* Where the field beginning at `p` ends: its newline, or its NUL. */

static const char *pair_field_end(const char *p)
{
   const char *e = p;
   while (*e && *e != '\n')
      e++;
   return e;
}

/* EXACTLY `want` hex characters, and then the end of the field. */
static int pair_hex_field(const char *p, int want, uint8_t *out)
{
   if (!p || want <= 0 || (want % 2) != 0)
      return 0;
   const char *e = pair_field_end(p);
   if (e - p != (int64_t)want)
      return 0; /* too short, or too long: both are the wrong reply */
   return unhex(p, want, out);
}

/* The 32-character confirmation tag, compared WHOLE.
 *
 * strncmp(p, want, 32) accepted a field of any length whose first 32
 * characters matched -- so a reply could carry the right tag with anything
 * appended and still verify the server. The tag is the only thing standing
 * between this and pairing with whoever answered. */
static int pair_tag_field(const char *p, const char *want)
{
   if (!p || !want)
      return 0;
   const char *e = pair_field_end(p);
   if (e - p != 32)
      return 0;
   for (int i = 0; i < 32; i++)
      if (p[i] != want[i])
         return 0;
   return 1;
}

/* A positive user id: all digits, nothing after, and no overflow.
 *
 * A loop that stops at the first non-digit and uses what it has reads
 * "12abc" as 12 and "99999999999999999999" as whatever the multiply wraps to.
 * This is the number the app stores as its identity. */
static int pair_uid_field(const char *p, int64_t *out)
{
   if (!p || !out)
      return 0;
   const char *e = pair_field_end(p);
   if (e == p)
      return 0;
   int64_t v = 0;
   for (const char *q = p; q < e; q++) {
      if (*q < '0' || *q > '9')
         return 0; /* trailing junk is a different reply, not this one */
      if (v > (2147483647L - (*q - '0')) / 10)
         return 0; /* an id this app cannot hold is not an id it may store */
      v = (v * 10) + (*q - '0');
   }
   if (v <= 0)
      return 0;
   *out = v;
   return 1;
}

/* THE ONE UNSIGNED REQUEST IN THE MODULE, and it takes the context for the
 * transport alone: there is no key yet -- deriving one is what these four
 * rounds are for. */
static int pair_step(const struct sync_ctx *sx, const char *path,
                     const char *body, int blen, char *out, int outcap)
{
   if (!sx || !sx->http)
      return -1;
   return sx->http("POST", path, NULL, body, blen, out, outcap);
}

int sync_pair_inner(const struct sync_ctx *sx, const char *email,
                    const char *code, uint8_t out_key[SYNC_KEY_LEN],
                    int64_t *out_uid)
{
   (void)sx; /* pairing is where the key COMES FROM: it signs nothing, so it
              * reads no identity from the snapshot. It takes the operation
              * lock all the same, because it uses the same response
              * workspace as a run and must not share it with one. */
   /* Idempotent, and REQUIRED: without the curve context jpake_new hands
    * back a pairing whose very first round fails, with nothing on the wire to
    * explain why. The app also calls this from the BLE driver, which is
    * exactly why it was easy to miss here. */
   jpake_init();
   struct jpake *p = jpake_new((const uint8_t *)code, (size_t)s_len(code), 1);
   if (!p)
      return -1;
   int rc = -1;
   uint8_t pkt[160];
   uint8_t peer[160];
   uint8_t key[SYNC_KEY_LEN];
   char hex[321];
   char body[512];
   char session[64];
   const char *nl = NULL;

   do {
      if (!jpake_round1(p, pkt))
         break;
      hexify(pkt, 160, hex);
      int n = snprintf(body, sizeof body, "%s\n%s\n", email, hex);
      if (pair_step(sx, "/v1/pair/1", body, n, sx->rsp, SYNC_BUF_MAX) != 200)
         break;
      nl = s_chr(sx->rsp, '\n');
      if (!nl || (int64_t)(nl - sx->rsp) >= (int64_t)sizeof session)
         break;
      memcpy(session, sx->rsp, (size_t)(nl - sx->rsp));
      session[nl - sx->rsp] = '\0';
      if (!pair_hex_field(nl + 1, 320, peer) || !jpake_peer_round1(p, peer))
         break;

      if (!jpake_round2(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step(sx, "/v1/pair/2", body, n, sx->rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!pair_hex_field(sx->rsp, 320, peer) || !jpake_peer_round2(p, peer))
         break;

      if (!jpake_round3(p, pkt))
         break;
      hexify(pkt, 160, hex);
      n = snprintf(body, sizeof body, "%s\n%s\n", session, hex);
      if (pair_step(sx, "/v1/pair/3", body, n, sx->rsp, SYNC_BUF_MAX) != 200)
         break;
      if (!pair_hex_field(sx->rsp, 320, peer) || !jpake_peer_round3(p, peer))
         break;
      if (!jpake_shared_key(p, key))
         break;

      /* Verify the SERVER before saving anything. */
      /* THE ANSWER IS READ. A tag that could not be built leaves `want` an
       * empty string, which no reply equals -- so the failure is a refusal to
       * pair rather than a comparison against a stack buffer. */
      char want[PAIR_TAG_HEX + 1];
      if (!pair_tag(key, SYNC_KEY_LEN, PAIR_TAG_LABEL_SERVER, want,
                    sizeof want))
         break;
      nl = s_chr(sx->rsp, '\n');
      if (!nl || !pair_tag_field(nl + 1, want))
         break;

      char mine[PAIR_TAG_HEX + 1];
      if (!pair_tag(key, SYNC_KEY_LEN, PAIR_TAG_LABEL_CLIENT, mine,
                    sizeof mine))
         break;
      n = snprintf(body, sizeof body, "%s\n%s\n", session, mine);
      if (pair_step(sx, "/v1/pair/4", body, n, sx->rsp, SYNC_BUF_MAX) != 200)
         break;

      int64_t uid = 0;
      if (!pair_uid_field(sx->rsp, &uid))
         break;
      sync_set_key(uid, key);
      for (int i = 0; i < SYNC_KEY_LEN; i++)
         out_key[i] = key[i];
      if (out_uid)
         *out_uid = uid;
      rc = 0;
   } while (0);

   jpake_free(p);
   return rc;
}

/* ---- the sync algorithm ----------------------------------------------- */

/* ---- ONE LINE OF A DIGEST REPLY ---------------------------------------
 *
 * THREE ANSWERS, BECAUSE THERE ARE THREE SITUATIONS. This returned 1 for a
 * row and 0 for everything else -- and "everything else" was the end of the
 * reply AND every possible syntax failure, reported identically.
 *
 * Both callers loop `while (digest_line(...))` and then treat what they
 * collected as the server's complete bucket list. So a reply that was cut in
 * half by a dropped connection, or whose tenth line was corrupt, stopped the
 * loop and was accepted as the whole truth about what the server holds. What
 * follows from that is not a failed sync:
 *
 *   - in sync_one_log, a bucket the server DOES hold but whose line was never
 *     parsed looks like one the server does not have, so the phone pushes it
 *     again -- harmless -- while a bucket listed after the damage that the
 *     phone has since deleted is never deleted server-side;
 *   - in sync_restore, the missing lines are buckets the phone will not pull
 *     back, and the restore then reports SUCCESS with a count. The user asked
 *     for their record and was told it came back, short.
 *
 * Framing is checked as strictly as the fields, because a truncated reply's
 * last line is usually well-formed for as far as it goes: "20000 3 abc" with
 * no newline is a plausible row and a certain sign that the rest is missing. */

/* Digits only, non-empty, and not more than a long can hold. The bucket is a
 * day number and the count a row count; a run long enough to overflow is not a
 * big number, it is a different string. */
