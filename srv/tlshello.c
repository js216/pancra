// SPDX-License-Identifier: GPL-3.0
// tlshello.c --- reading a ClientHello without trusting a byte of it
// Copyright 2026 Jakob Kastelic

/* SPLIT OUT OF srv/tls.c, along the seam that file already had.
 *
 * This is the parser, and it is the one part of a TLS server that runs on
 * bytes an unauthenticated stranger chose. It has no state of its own, touches
 * no key, no connection and no certificate -- it takes a buffer and fills a
 * struct hello -- which is why it could be lifted out whole: the grep for
 * anything it shared with the rest of the module came back empty.
 *
 * THE BOUNDED CURSORS BELOW ARE THE WHOLE DEFENCE, and the long comment on
 * them is the record of what they replaced. Read it before changing anything
 * here.
 *
 * It came out because tls.c passed the 2000-line ceiling the build enforces,
 * and it is the piece that came out because it is the piece that was already
 * separate. The extraction moved lines and changed none of them. */
#include "tlsint.h"

#include "proto.h"
#include "tls.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- BOUNDED NESTED CURSORS -------------------------------------------
 *
 * A ClientHello is vectors inside vectors: the message holds a cipher-suite
 * vector and an extension vector, each extension holds one of its own, and
 * the pre_shared_key extension holds two. Every one of them announces its own
 * length, and the ONLY thing that makes that length trustworthy is that a
 * reader refuses to look outside the vector it is in.
 *
 * WALKING THEM ALL WITH ONE `i` AND A HANDFUL OF ad-hoc COMPARISONS is how
 * that goes wrong, because the comparisons do not say the same thing twice:
 *
 *   - `for (j = 0; j + 1 < cs && i + j + 1 < n; j += 2)` stops at whichever
 *     bound runs out first and then advances `i += cs` regardless, so a
 *     cipher_suites vector declaring more bytes than the hello holds moved
 *     the cursor past the end and every later field was read from whatever
 *     followed;
 *   - `i += 1 + b[i]` for legacy_compression_methods was not bounded at all;
 *   - the supported_versions, signature_algorithms, psk_key_exchange_modes,
 *     identity and binder loops each ignored their OWN declared length and
 *     ran to the end of the extension body instead, so a vector could
 *     announce two bytes and be read for two hundred;
 *   - nothing required any vector to END where it said it would, so trailing
 *     rubbish inside one was silently the next thing parsed.
 *
 * None of that is a memory-safety bug on its own -- the outer `end > n` check
 * kept most of it inside the buffer -- and that is exactly why it survived:
 * the parser accepted a large family of ClientHellos that no client can spell
 * and that no two implementations would agree on. A message with more than
 * one reading is a message an attacker chooses the reading of.
 *
 * So every vector is opened as its own cursor, which knows where it ends and
 * cannot be read past it, and every one of them must be EXHAUSTED: a vector
 * with bytes left over when its contents have been read is refused, by name,
 * where it was opened. That is one rule per vector rather than one rule for
 * the message, which is what makes a loosened check show up as a failure
 * about the vector it belongs to.
 *
 * Nothing here is TLS-specific enough to live in lib/: it is length-prefixed
 * vector framing, and the sync protocol (lib/wirevec.h) is text. It stays
 * beside the only parser that has any. */
struct vec {
   const uint8_t *p; /* the vector's bytes, not counting its length prefix */
   size_t n;         /* how many there are */
   size_t i;         /* how many have been read */
};

static struct vec vec_of(const uint8_t *p, size_t n)
{
   struct vec v = {p, n, 0};
   return v;
}

/* Exhausted: every declared byte was read as something. The whole point. */
static int vec_done(const struct vec *v)
{
   return v->i == v->n;
}

/* `n` bytes, or NULL and the cursor untouched. A refused take leaves the
 * cursor where it was so a caller that ignores the answer cannot silently
 * advance past the end -- there is no partial read here. */
static const uint8_t *vec_take(struct vec *v, size_t n)
{
   if (n > v->n - v->i)
      return NULL;
   const uint8_t *p = v->p + v->i;
   v->i += n;
   return p;
}

static int vec_u8(struct vec *v, unsigned *out)
{
   const uint8_t *p = vec_take(v, 1);
   if (!p)
      return 0;
   *out = p[0];
   return 1;
}

static int vec_u16(struct vec *v, unsigned *out)
{
   const uint8_t *p = vec_take(v, 2);
   if (!p)
      return 0;
   *out = get16(p);
   return 1;
}

/* Open the nested vector whose 1- or 2-byte length prefix is next. It must
 * lie WHOLLY inside this one: a declared length that overruns the parent is
 * refused here, before a byte of the child is addressed, which is what makes
 * "read past the parent" unreachable rather than merely unlikely. */
static int vec_sub8(struct vec *v, struct vec *out)
{
   unsigned len;
   if (!vec_u8(v, &len))
      return 0;
   const uint8_t *p = vec_take(v, len);
   if (!p)
      return 0;
   *out = vec_of(p, len);
   return 1;
}

static int vec_sub16(struct vec *v, struct vec *out)
{
   unsigned len;
   if (!vec_u16(v, &len))
      return 0;
   const uint8_t *p = vec_take(v, len);
   if (!p)
      return 0;
   *out = vec_of(p, len);
   return 1;
}

/* ---- ClientHello -------------------------------------------------------
 *
 * Only the extensions that decide something are READ -- which TLS version the
 * client really wants, its key share, whether it can verify our signature
 * algorithm, and any resumption ticket. Everything else -- server name, ALPN,
 * the dozen extensions a browser sends -- is skipped, because nothing this
 * server does depends on them.
 *
 * SKIPPED IS NOT UNCHECKED. An extension this server ignores still has to be
 * FRAMED, because its declared length is how the next one is found, and it
 * still counts against the rule that no type may appear twice (RFC 8446 4.2).
 */

/* WE SELECT IDENTITY 0 AND NOTHING ELSE. We issue exactly one ticket per
 * connection, so the offer we could accept is the first one; the rest of the
 * list is still framed and still counted, because the binder list has to
 * match it entry for entry. */
#define PSK_SELECTED 0

/* Duplicate extension types, one bit per type. RFC 8446 4.2: "There MUST NOT
 * be more than one extension of the same type in a given extension block."
 *
 * A BITMAP RATHER THAN A LIST OF THE ONES WE READ. Duplicating an extension
 * this server ignores is just as much a second reading of the message -- a
 * middlebox and this parser would disagree about which copy counts -- and a
 * ClientHello may carry a few thousand four-byte extensions, which is a
 * quadratic scan if the seen set is a list. 8 KiB of stack against a 16 KiB
 * record buffer two frames up is not the expensive thing here. */
#define EXT_SEEN_BYTES 8192

static int ext_seen(uint8_t *seen, unsigned type)
{
   uint8_t bit = (uint8_t)(1u << (type & 7));
   if (seen[type >> 3] & bit)
      return 1;
   seen[type >> 3] |= bit;
   return 0;
}

/* The pre_shared_key extension: OfferedPsks, framed exactly (RFC 8446
 * 4.2.11).
 *
 *     struct {
 *         PskIdentity identities<7..2^16-1>;
 *         PskBinderEntry binders<33..2^16-1>;
 *     } OfferedPsks;
 *
 * ...where a PskIdentity is opaque identity<1..2^16-1> followed by a uint32
 * obfuscated_ticket_age, and a PskBinderEntry is opaque<32..255>.
 *
 * THE AGE MUST BE SKIPPED. Reading identity 0's length and bytes and then
 * jumping to `2 + idn` for the binder list never walks the identity list at
 * all -- which is why such a parser cannot count it, and why the binder list
 * is accepted at whatever length it claims. Four bytes per
 * entry that nothing consumed is the difference between "the first identity"
 * and "the identities".
 *
 * ONE BINDER PER IDENTITY, and the counts must be equal: RFC 8446 4.2.11.2
 * says the binders field is "a series of HMAC values, one for each value in
 * the identities list and in the same order". A shorter binder list is not a
 * partial offer to be read as far as it goes; it is an offer whose remaining
 * identities carry no proof at all. */
static int parse_psk(struct vec *v, struct hello *h)
{
   struct vec ids, bs;
   if (!vec_sub16(v, &ids) || !vec_sub16(v, &bs) || !vec_done(v))
      return 0; /* OfferedPsks is the whole extension, and nothing more */
   /* THE TRUNCATION POINT, which is what the binder is a MAC over: the
    * ClientHello up to and not including the binder list's own length field.
    * Recorded here, where the list was opened, rather than reconstructed by
    * the verifier from the first entry's address. */
   h->binders = bs.p - 2;

   unsigned nid = 0;
   while (!vec_done(&ids)) {
      struct vec one;
      if (!vec_sub16(&ids, &one) || one.n < 1 || !vec_take(&ids, 4))
         return 0; /* identity<1..>, then obfuscated_ticket_age */
      if (nid == PSK_SELECTED) {
         h->psk_ident   = one.p;
         h->psk_ident_n = one.n;
      }
      nid++;
   }

   unsigned nb = 0;
   while (!vec_done(&bs)) {
      struct vec one;
      if (!vec_sub8(&bs, &one) || one.n < 32)
         return 0; /* PskBinderEntry is opaque<32..255> */
      if (nb == PSK_SELECTED) {
         h->binder   = one.p;
         h->binder_n = one.n;
      }
      nb++;
   }
   if (nid == 0 || nid != nb)
      return 0;
   return 1;
}

int parse_hello(const uint8_t *b, size_t n, struct hello *h)
{
   memset(h, 0, sizeof *h);
   struct vec ch = vec_of(b, n);
   if (!vec_take(&ch, 2 + 32)) /* legacy_version, random */
      return 0;

   struct vec sid;
   if (!vec_sub8(&ch, &sid) || sid.n > 32) /* legacy_session_id<0..32> */
      return 0;
   h->session_id   = sid.p;
   h->session_id_n = sid.n;

   struct vec cs; /* cipher_suites<2..2^16-2>, whole suites and no tail */
   if (!vec_sub16(&ch, &cs) || cs.n < 2)
      return 0;
   while (!vec_done(&cs)) {
      unsigned suite;
      if (!vec_u16(&cs, &suite))
         return 0;
      if (suite == SUITE_AES128)
         h->suite_ok = 1;
   }

   /* legacy_compression_methods. RFC 8446 4.1.2 leaves no room here at all:
    * "For every TLS 1.3 ClientHello, this vector MUST contain exactly one
    * byte, set to zero" -- so this is the one vector whose contents are as
    * fixed as its framing, and the old `i += 1 + b[i]` neither read nor
    * bounded it. */
   struct vec cm;
   if (!vec_sub8(&ch, &cm) || cm.n != 1 || cm.p[0] != 0)
      return 0;

   struct vec ex;
   if (!vec_sub16(&ch, &ex) || !vec_done(&ch))
      return 0; /* the extensions are the last thing in a ClientHello */

   uint8_t seen[EXT_SEEN_BYTES];
   memset(seen, 0, sizeof seen);
   const uint8_t *psk_at = NULL; /* where a pre_shared_key extension began */
   size_t psk_n          = 0;

   while (!vec_done(&ex)) {
      unsigned type;
      struct vec v;
      if (!vec_u16(&ex, &type) || !vec_sub16(&ex, &v))
         return 0;
      if (ext_seen(seen, type))
         return 0;
      switch (type) {
         case EXT_SUPPORTED_VERSION: {
            struct vec vs; /* ProtocolVersion versions<2..254> */
            if (!vec_sub8(&v, &vs) || !vec_done(&v))
               return 0;
            while (!vec_done(&vs)) {
               unsigned ver;
               if (!vec_u16(&vs, &ver))
                  return 0;
               if (ver == 0x0304)
                  h->has_tls13 = 1;
            }
            break;
         }
         case EXT_SIG_ALGS: {
            struct vec sa; /* SignatureScheme supported_signature_algorithms */
            if (!vec_sub16(&v, &sa) || !vec_done(&v))
               return 0;
            while (!vec_done(&sa)) {
               unsigned alg;
               if (!vec_u16(&sa, &alg))
                  return 0;
               if (alg == SIG_ECDSA256)
                  h->sig_ok = 1;
            }
            break;
         }
         case EXT_KEY_SHARE: {
            struct vec ks; /* KeyShareEntry client_shares<0..2^16-1> */
            if (!vec_sub16(&v, &ks) || !vec_done(&v))
               return 0;
            while (!vec_done(&ks)) {
               unsigned grp;
               struct vec pt;
               /* THE WHOLE SHARE MUST LIE INSIDE THE EXTENSION before any of
                * it is read. `v[j + 4]` alone was read when j + 4 == len --
                * one byte past the end -- and the 65 bytes it selected were
                * then handed to p256_from_xy, which reads all of them. A
                * seven-byte body (00 07 | 00 17 | 00 41 | 04) walked 64 bytes
                * off an attacker-chosen extension, before any authentication
                * had happened. vec_sub16 is that check, and it is now the
                * same check every other vector here gets. */
               if (!vec_u16(&ks, &grp) || !vec_sub16(&ks, &pt))
                  return 0;
               if (grp == GROUP_P256 && pt.n == 65 && pt.p[0] == 0x04)
                  h->peer_key = pt.p;
            }
            break;
         }
         case EXT_PSK_MODES: {
            struct vec pm; /* PskKeyExchangeMode ke_modes<1..255> */
            if (!vec_sub8(&v, &pm) || !vec_done(&v))
               return 0;
            while (!vec_done(&pm)) {
               unsigned mode;
               if (!vec_u8(&pm, &mode))
                  return 0;
               if (mode == 1) /* psk_dhe_ke */
                  h->psk_dhe = 1;
            }
            break;
         }
         case EXT_PRE_SHARED_KEY:
            psk_at = v.p;
            psk_n  = v.n;
            if (!parse_psk(&v, h))
               return 0;
            break;
         default: break; /* framed above; its contents decide nothing */
      }
   }

   /* pre_shared_key MUST be the last extension in the ClientHello (RFC 8446
    * 4.2.11), and a server that sees it elsewhere must abort. The reason is
    * structural: the binder is a MAC over the ClientHello UP TO the start of
    * the binder list, so "everything before the binders" is only a
    * well-defined span if nothing follows them. A psk_ext_off recorded for
    * exactly this check and never read puts the intent in a field and leaves
    * the check missing.
    *
    * The extension's own end is the thing compared, rather than the
    * binder span's: with the identity and binder lists required to exhaust
    * OfferedPsks, "the extension ends where the hello does" says the same
    * thing and says it about the extension. */
   if (psk_at && psk_at + psk_n != b + n)
      return 0;
   return 1;
}
