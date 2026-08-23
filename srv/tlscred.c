// SPDX-License-Identifier: GPL-3.0
// tlscred.c --- reading the certificate and key this server presents
// Copyright 2026 Jakob Kastelic

/* SPLIT OUT OF srv/tls.c, along the seam that file already had.
 *
 * tls.c is the PROTOCOL: records, the key schedule, the handshake, tickets,
 * KeyUpdate. This is the 460 lines that happen ONCE, at startup, and never
 * again -- reading two files off disk, decoding base64,
 * walking DER far enough to find a public key and a private scalar, and
 * refusing to start if the two do not belong together.
 *
 * The vocabularies barely overlap. Nothing here knows what a handshake is;
 * nothing in the handshake knows what PEM is. What the two share is four
 * arrays -- the chain as it will go on the wire, our private scalar, and the
 * key that seals resumption tickets -- and those are declared in srv/tlsint.h,
 * which exists for exactly this seam and nothing wider.
 *
 * It came out because tls.c passed the 2000-line ceiling the build enforces,
 * and it is the piece that came out because it is the piece that was already
 * separate. The extraction moved lines and changed none of them. */
#include "tlsint.h"

#include "ecdsa.h"
#include "p256.h"
#include "proto.h"
#include "rand.h" /* rand_bytes: the ticket key is generated, not read */
#include "sha256.h"
#include "tls.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h> /* open/O_RDONLY: the two pem files are read here */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* read/close */

/* ---- PEM ---------------------------------------------------------------- */

static int b64val(int c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '+')
      return 62;
   if (c == '/')
      return 63;
   return -1;
}

/* Decode every base64 body between -----BEGIN/END----- in `pem`, appending
 * each as DER. `each` is called with one block at a time. */
/* `incomplete` (may be NULL) is set when a block STARTS and does not finish.
 *
 * `break` out of the loop on a "-----BEGIN" with no matching "-----END" and
 * the caller takes whatever blocks already decoded as the whole file. That is
 * precisely the shape of a truncated credential -- a copy interrupted, a
 * partial write, a file cut by a full disk -- and it is the case where
 * guessing is least acceptable: the server comes up presenting the
 * certificates that happened to be complete. Saying so lets the caller
 * refuse.
 * A file with no BEGIN at all is a different answer (*nblocks == 0), already
 * handled by the caller. */
/* `over` (may be NULL) is set when the output or the block table could not
 * hold what the file contains. Dropping the excess silently would decode a
 * chain with more certificates than the table has entries down to the first
 * few, and the server would present a partial chain that no client can build
 * a path from. The caller sizes both from the file, so this is a promise
 * being checked rather than a limit being enforced. */
static size_t pem_decode(const char *pem, uint8_t *out, size_t cap,
                         size_t *starts, size_t *lens, size_t maxblocks,
                         size_t *nblocks, int *incomplete, int *over)
{
   size_t total = 0;
   *nblocks     = 0;
   if (over)
      *over = 0;
   if (incomplete)
      *incomplete = 0;
   const char *p = pem;
   while ((p = strstr(p, "-----BEGIN")) != NULL) {
      const char *nl = strchr(p, '\n');
      if (!nl) {
         if (incomplete)
            *incomplete = 1; /* a header with no end of line: cut mid-header */
         break;
      }
      const char *end = strstr(nl, "-----END");
      if (!end) {
         if (incomplete)
            *incomplete = 1; /* a block that begins and never ends */
         break;
      }
      size_t start = total;
      uint32_t acc = 0;
      int bits     = 0;
      for (const char *q = nl; q < end; q++) {
         int v = b64val((unsigned char)*q);
         if (v < 0)
            continue;
         acc = (acc << 6) | (unsigned)v;
         bits += 6;
         if (bits >= 8) {
            bits -= 8;
            if (total < cap)
               out[total++] = (uint8_t)(acc >> bits);
            else if (over)
               *over = 1;
         }
      }
      if (*nblocks < maxblocks) {
         starts[*nblocks] = start;
         lens[*nblocks]   = total - start;
         (*nblocks)++;
      } else if (over) {
         *over = 1;
      }
      p = end + 1;
   }
   return total;
}

/* HOW MANY PEM BLOCKS ARE IN THIS TEXT, so the tables that describe them are
 * sized from the file rather than from a guess about how long a chain gets
 *. An unterminated block is counted: pem_decode reports it as a
 * truncation, and a table one entry short would turn that into an overflow
 * instead. */
static size_t pem_blocks(const char *pem)
{
   size_t n      = 0;
   const char *p = pem;
   while ((p = strstr(p, "-----BEGIN")) != NULL) {
      n++;
      p += 10;
   }
   return n;
}

/* Decode every PEM block in `txt` into freshly allocated storage sized from
 * the text itself: base64 is four characters per three bytes, so the decoded
 * DER can never exceed three quarters of it, and the block tables can never
 * need more entries than there are BEGIN lines. Overflow is therefore not a
 * case the caller has to have an opinion about -- which is the point.
 *
 * 1 on success, and then `*der`, `*starts` and `*lens` are the caller's to
 * free. */
struct pem_blob {
   uint8_t *der;
   size_t dn;
   size_t *starts;
   size_t *lens;
   size_t nb;
   int cut; /* a block that begins and never ends */
};

static void pem_free(struct pem_blob *b)
{
   free(b->der);
   free(b->starts);
   free(b->lens);
   memset(b, 0, sizeof *b);
}

static int pem_load(const char *txt, size_t tn, struct pem_blob *out)
{
   memset(out, 0, sizeof *out);
   size_t maxb = pem_blocks(txt);
   if (!maxb)
      return 1; /* no blocks: nb == 0, which the caller reports its own way */
   size_t cap  = tn; /* an over-estimate by a quarter, and it costs nothing */
   out->der    = malloc(cap ? cap : 1);
   out->starts = malloc(maxb * sizeof *out->starts);
   out->lens   = malloc(maxb * sizeof *out->lens);
   if (!out->der || !out->starts || !out->lens) {
      pem_free(out);
      return 0;
   }
   int over = 0;
   out->dn  = pem_decode(txt, out->der, cap, out->starts, out->lens, maxb,
                         &out->nb, &out->cut, &over);
   if (over) {
      /* Not reachable: the sizes above are upper bounds on what the same text
       * can decode to. It is checked because "cannot happen" is a claim about
       * the two lines above, and a silent partial chain is what it costs when
       * a later edit makes the claim false. */
      pem_free(out);
      return 0;
   }
   return 1;
}

/* Why a credential file could not be read WHOLE. Separate answers, because
 * the operator's next action differs for each and "cannot read" covered all of
 * them. */
enum slurp_err {
   SLURP_OK = 0,
   SLURP_OPEN,   /* not there, or not permitted */
   SLURP_READ,   /* an I/O error part way through */
   SLURP_CLOSE,  /* close reported a failure -- rare on a read, still checked */
   SLURP_TOOBIG, /* did not reach EOF within the ceiling */
   SLURP_EMPTY   /* zero bytes: a file exists where a credential should be */
};

/* THE WHOLE FILE, OR NOTHING.
 *
 * ONE read into a fixed buffer, taking whatever comes back and
 * NUL-terminating it, has two failures, both silent and both intermittent:
 *
 *   - A SHORT READ IS LEGAL. read(2) may return fewer bytes than asked for
 *     with no error at all, and on a regular file it usually does not -- which
 *     is worse than if it always did, because the failure appears once in a
 *     while and looks like a corrupt certificate. The server then refuses to
 *     start, or starts with a chain missing its tail, depending on where the
 *     read stopped, and the file on disk is perfectly good.
 *   - A FILE LARGER THAN THE BUFFER WAS SILENTLY PREFIX-TRUNCATED. Not
 *     detected, not reported: a chain with several intermediates parsed to
 *     however many certificates fit, and the server presented a partial chain
 *     that clients cannot build a path from.
 *
 * So: loop to EOF, and require that EOF actually arrive within the ceiling
 * rather than assuming a full buffer is a whole file. `why` (may be NULL) says
 * which of those happened, so the diagnostic names the operator's next move. */
static char *slurp(const char *path, size_t *n, enum slurp_err *why)
{
   /* ALLOCATED, NOT A STATIC ARRAY. The buffer was `static char
    * buf[16384]`, so the size of a credential this server would read was a
    * number in this function rather than a property of the credential -- and
    * a second call handed back the same storage, which is why the certificate
    * had to be finished with before the key was read. It is the caller's to
    * free now, and the ceiling is TLS_CRED_MAX, which is derived from the
    * chain size this server supports (see tlsint.h).
    *
    * GROWN, NOT STAT'ED. A stat before the read is a second answer about the
    * file that can differ from the first one -- a credential rewritten by a
    * renewal script between the two is exactly the case, and it would produce
    * a buffer sized for one file holding a prefix of another. */
   size_t cap = 8192;
   char *buf  = malloc(cap);
   if (!buf) {
      if (why)
         *why = SLURP_READ;
      return NULL;
   }
   if (why)
      *why = SLURP_OK;
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
      free(buf);
      if (why)
         *why = SLURP_OPEN;
      return NULL;
   }
   size_t off = 0;
   int eof    = 0;
   for (;;) {
      if (off + 1 >= cap) {
         if (cap >= (size_t)TLS_CRED_MAX)
            break; /* the ceiling: the probe below says so out loud */
         size_t want = cap * 2;
         if (want > (size_t)TLS_CRED_MAX)
            want = (size_t)TLS_CRED_MAX;
         char *bigger = realloc(buf, want);
         if (!bigger) {
            free(buf);
            close(fd);
            if (why)
               *why = SLURP_READ;
            return NULL;
         }
         buf = bigger;
         cap = want;
      }
      long r = read(fd, buf + off, cap - 1 - off);
      if (r < 0) {
         /* A read cut short by a signal has moved nothing. Treated as an I/O
          * error it would refuse a good file, which is the intermittent
          * failure this function exists to remove. */
         if (errno == EINTR)
            continue;
         free(buf);
         close(fd);
         if (why)
            *why = SLURP_READ;
         return NULL;
      }
      if (r == 0) {
         eof = 1;
         break;
      }
      off += (size_t)r;
   }
   /* A FULL BUFFER IS NOT EOF. One more byte is asked for: if it arrives, the
    * file is bigger than this server will read, and the prefix already in the
    * buffer is not the credential -- it is the beginning of one. */
   if (!eof) {
      for (;;) {
         char probe;
         long r = read(fd, &probe, 1);
         if (r < 0 && errno == EINTR)
            continue;
         if (r > 0) {
            free(buf);
            close(fd);
            if (why)
               *why = SLURP_TOOBIG;
            return NULL;
         }
         if (r < 0) {
            free(buf);
            close(fd);
            if (why)
               *why = SLURP_READ;
            return NULL;
         }
         break; /* r == 0: the file ended exactly at the ceiling */
      }
   }
   /* CHECKED, even on a read handle. It carries no buffered data to lose, but
    * a failure here says the descriptor was not the one we thought, and this
    * is a credential. */
   if (close(fd) != 0) {
      free(buf);
      if (why)
         *why = SLURP_CLOSE;
      return NULL;
   }
   if (off == 0) {
      free(buf);
      if (why)
         *why = SLURP_EMPTY;
      return NULL;
   }
   buf[off] = '\0';
   *n       = off;
   return buf;
}

/* One sentence naming what to do about it. */
static const char *slurp_why(enum slurp_err e, size_t cap)
{
   (void)cap;
   switch (e) {
      case SLURP_OK: return "read whole";
      case SLURP_OPEN: return "cannot be opened (missing, or not permitted)";
      case SLURP_READ: return "could not be read to the end (I/O error)";
      case SLURP_CLOSE: return "reported an error on close";
      case SLURP_TOOBIG:
         return "is larger than this server reads; the prefix is not the "
                "credential. Shorten the chain, or raise TLS_CRED_MAX (and "
                "the chain bound it is derived from) in srv/tlsint.h";
      case SLURP_EMPTY: return "is empty";
   }
   return "could not be read";
}

/* The private key: find the 32-byte scalar inside a SEC1 or PKCS#8 EC key.
 * Both wrap it in an OCTET STRING of exactly 32 bytes that is the first such
 * string in the structure, which is enough to find it without a general ASN.1
 * parser -- and a wrong guess cannot go unnoticed, because the certificate
 * would then fail to verify against it. */
static int find_ec_scalar(const uint8_t *der, size_t n, uint8_t out[32])
{
   for (size_t i = 0; i + 34 <= n; i++)
      if (der[i] == 0x04 && der[i + 1] == 0x20) {
         memcpy(out, der + i + 2, 32);
         return 1;
      }
   return 0;
}

/* ---- DOES THE KEY BELONG TO THE CERTIFICATE? --------------------------
 *
 * A CERTIFICATE AND A KEY THAT ARE BOTH VALID AND UNRELATED is the failure
 * this catches, and it is the worst-behaved one available: tls_init parsed the
 * chain, found a 32-byte scalar, and returned success, so the server printed
 * "listening" and accepted connections -- and then every single handshake died
 * at CertificateVerify, because the signature is made with a key the presented
 * certificate does not vouch for. Nothing local looks wrong. The pid is alive,
 * the log says it is serving, and the failure is visible only to clients, as
 * a TLS error they cannot act on. The phone shows TLS REFUSED and retries for
 * ever.
 *
 * It is also the single likeliest rotation mistake: two files, copied in two
 * steps, and copying only one of them leaves exactly this state.
 *
 * The comparison is the whole public key, X and Y, because the point is what
 * the certificate binds: deriving d*G from the scalar and finding it equal to
 * the certificate's SubjectPublicKeyInfo is proof that this key signs for this
 * certificate.
 *
 * THE SPKI IS FOUND BY PATTERN, not by a general ASN.1 walk. The bytes below
 * are the DER of the P-256 AlgorithmIdentifier followed by the BIT STRING
 * header for an uncompressed point -- a fixed, canonical encoding that every
 * producer emits identically for a named-curve P-256 key. A walker would be a
 * hundred lines of certificate parsing in a file that deliberately contains no
 * general X.509 parser, to answer one question about a fixed shape. If the
 * pattern is absent the answer is "this is not a P-256 certificate this server
 * can check", which is refused rather than skipped: a check that silently
 * passes when it cannot run is the failure mode being removed here. */
static const uint8_t P256_SPKI_PREFIX[] = {
    /* SEQUENCE (AlgorithmIdentifier), 0x13 bytes */
    0x30, 0x13,
    /* OID 1.2.840.10045.2.1  id-ecPublicKey */
    0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
    /* OID 1.2.840.10045.3.1.7  prime256v1 */
    0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07,
    /* BIT STRING, 0x42 bytes, 0 unused bits, 0x04 = uncompressed point */
    0x03, 0x42, 0x00, 0x04};

/* The leaf's 64-byte X||Y, or 0 if this certificate does not carry a
 * named-curve P-256 public key in the canonical encoding. */
static int leaf_pubkey(const uint8_t *der, size_t n, uint8_t out[64])
{
   const size_t plen = sizeof P256_SPKI_PREFIX;
   if (n < plen + 64)
      return 0;
   for (size_t i = 0; i + plen + 64 <= n; i++) {
      if (memcmp(der + i, P256_SPKI_PREFIX, plen) != 0)
         continue;
      memcpy(out, der + i + plen, 64);
      return 1;
   }
   return 0;
}

/* 1 when `scalar` is the private key for the leaf certificate's public key. */
static int key_matches_cert(const uint8_t *leaf, size_t leaf_n,
                            const uint8_t scalar[32], const char *name,
                            const char *cert_pem, const char *key_pem)
{
   uint8_t want[64];
   if (!leaf_pubkey(leaf, leaf_n, want)) {
      fprintf(stderr,
              "%s: cannot read a P-256 public key out of the leaf certificate "
              "in %s, so it cannot be checked against %s. This server only "
              "serves P-256; a certificate it cannot check is one it will not "
              "present.\n",
              name, cert_pem, key_pem);
      return 0;
   }
   struct u256 d;
   struct jpoint Q;
   uint8_t got[65];
   p256_sc_from_be(&d, scalar);
   p256_mul_g(&Q, &d);
   p256_uncompressed(&Q, got);
   /* got[0] is the 0x04 tag; the coordinates follow. */
   if (memcmp(got + 1, want, 64) != 0) {
      fprintf(stderr,
              "%s: the private key in %s is NOT the key for the certificate "
              "in %s.\n"
              "  Both files are valid and they do not go together, which is "
              "what copying one half of a rotation leaves behind.\n"
              "  Started anyway, this server would listen, log that it is "
              "serving, and fail EVERY handshake at CertificateVerify -- "
              "visible only to clients, as an error they cannot act on.\n",
              name, key_pem, cert_pem);
      return 0;
   }
   return 1;
}

int tls_init(const char *cert_pem, const char *key_pem, const char *name)
{
   p256_init();
   size_t n;
   enum slurp_err se;
   char *txt = slurp(cert_pem, &n, &se);
   if (!txt) {
      fprintf(stderr, "%s: the certificate file %s %s\n", name, cert_pem,
              slurp_why(se, 0));
      return 1;
   }
   /* SIZED FROM THE FILE. A `uint8_t der[4096]` and an eight-entry block
    * table quietly drop whatever does not fit -- a chain with a fourth
    * certificate, or a leaf with a long SAN list, presented as however much
    * of itself fitted. */
   struct pem_blob cert;
   if (!pem_load(txt, n, &cert)) {
      fprintf(stderr, "%s: not enough memory to read %s\n", name, cert_pem);
      free(txt);
      return 1;
   }
   free(txt);
   /* TRUNCATION IS CHECKED FIRST, before "no certificate at all".
    *
    * A file cut inside its ONLY block decodes to zero certificates, so the
    * generic message won the race and reported "no certificate in cert.pem"
    * about a file that plainly contains one -- sending the operator to look
    * for a missing certificate rather than a truncated copy. Both are
    * refusals; only one of them says what happened. */
   if (cert.cut) {
      fprintf(stderr,
              "%s: the certificate file %s is TRUNCATED -- a PEM block begins "
              "and never ends. The %zu certificate(s) that decoded before it "
              "are not the whole chain.\n",
              name, cert_pem, cert.nb);
      pem_free(&cert);
      return 1;
   }
   if (!cert.nb) {
      fprintf(stderr, "%s: no certificate in %s\n", name, cert_pem);
      pem_free(&cert);
      return 1;
   }
   /* Build the Certificate message's cert_list once: for each certificate a
    * 3-byte length, the DER, and an empty extension list.
    *
    * MEASURED, THEN ALLOCATED, THEN FILLED. The chain is as long as the
    * credential says it is; the only ceiling on it is TLS_CHAIN_MAX, which is
    * about what a handshake can carry and hash (see tlsint.h) rather than
    * about the size of an array this file happened to declare. */
   size_t need = 0;
   for (size_t i = 0; i < cert.nb; i++)
      need += 3 + cert.lens[i] + 2;
   if (need > (size_t)TLS_CHAIN_MAX) {
      /* REFUSE, do not truncate. A chain cut short here is one no client can
       * build a path from, and tls_init returning success would mean the
       * server comes up presenting it with nothing on stderr to say why: the
       * same failure mode as the RSA key below, which is refused outright
       * precisely because a server nobody can reach is worse than a server
       * that did not start. */
      fprintf(stderr,
              "%s: the certificate chain in %s is %zu bytes, and this server "
              "carries at most %d (%zu certificates).\n"
              "  RAISE TLS_CHAIN_MAX in srv/tlsint.h -- the transcript bound "
              "is derived from it, so the two cannot drift apart.\n",
              name, cert_pem, need, TLS_CHAIN_MAX, cert.nb);
      pem_free(&cert);
      return 1;
   }
   uint8_t *chain = malloc(need);
   if (!chain) {
      fprintf(stderr, "%s: not enough memory for the chain in %s\n", name,
              cert_pem);
      pem_free(&cert);
      return 1;
   }
   size_t k = 0;
   for (size_t i = 0; i < cert.nb; i++) {
      put24(chain + k, cert.lens[i]);
      k += 3;
      memcpy(chain + k, cert.der + cert.starts[i], cert.lens[i]);
      k += cert.lens[i];
      put16(chain + k, 0);
      k += 2;
   }
   /* THE LEAF is read straight out of the decoded DER, which is still alive:
    * the private key below gets its OWN blob now, so there is no reuse to
    * copy defensively around. */
   const uint8_t *leaf = cert.der + cert.starts[0];
   size_t leaf_n       = cert.lens[0];

   txt = slurp(key_pem, &n, &se);
   if (!txt) {
      fprintf(stderr, "%s: the private key file %s %s\n", name, key_pem,
              slurp_why(se, 0));
      free(chain);
      pem_free(&cert);
      return 1;
   }
   struct pem_blob key;
   if (!pem_load(txt, n, &key)) {
      fprintf(stderr, "%s: not enough memory to read %s\n", name, key_pem);
      free(txt);
      free(chain);
      pem_free(&cert);
      return 1;
   }
   free(txt);
   int bad = 0;
   if (key.cut) {
      fprintf(stderr,
              "%s: the private key file %s is TRUNCATED -- a PEM block begins "
              "and never ends.\n",
              name, key_pem);
      bad = 1;
   } else if (!key.nb || !find_ec_scalar(key.der, key.dn, g_key)) {
      fprintf(stderr, "%s: no EC private key in %s\n", name, key_pem);
      bad = 1;
   } else if (!key_matches_cert(leaf, leaf_n, g_key, name, cert_pem, key_pem)) {
      /* BEFORE ANYTHING LISTENS. Both files parsed; the question left is
       * whether they are a pair, and it is answered here rather than by every
       * client. */
      bad = 1;
   }
   pem_free(&key);
   pem_free(&cert);
   if (bad) {
      free(chain);
      return 1;
   }
   if (!rand_bytes(g_ticket_key, 16)) {
      fprintf(stderr, "%s: no entropy for the ticket key\n", name);
      free(chain);
      return 1;
   }
   /* PUBLISHED LAST, and never freed: this runs once, before the listener
    * exists, and every handshake for the life of the process reads it without
    * a lock precisely because nothing writes it again (see tlsint.h). */
   g_cert   = chain;
   g_cert_n = k;
   return 0;
}
