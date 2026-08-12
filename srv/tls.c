/* SPDX-License-Identifier: GPL-3.0
 * tls.c --- a TLS 1.3 server, only as much of one as this program needs
 * Copyright 2026 Jakob Kastelic
 *
 * About a thousand lines, where a general TLS library needs tens of
 * thousands. The difference is not cleverness, it is scope: a general library
 * must be able to negotiate anything, and this one negotiates exactly one
 * thing.
 *
 *     cipher suite   TLS_AES_128_GCM_SHA256   (0x1301)
 *     group          secp256r1                (0x0017)
 *     signature      ecdsa_secp256r1_sha256   (0x0403)
 *
 * That is not a compromise. TLS_AES_128_GCM_SHA256 is the one suite RFC 8446
 * requires every client to implement, so the negotiation cannot fail; and
 * offering one of each means no algorithm tables, no agility, and no code for
 * primitives nothing will ever pick. The arithmetic underneath was already
 * here for the pairing (lib/p256.c, lib/sha256.c, lib/aes.c); lib/gcm.c,
 * lib/hmac.c, lib/hkdf.c and lib/ecdsa.c add the rest, each checked against
 * the published vectors before any of it went near a socket.
 *
 * WHAT IS DELIBERATELY ABSENT: TLS 1.2 and below (refused, not negotiated
 * down), client certificates, renegotiation, key updates, 0-RTT early data,
 * and X.509 parsing -- the certificate chain is read as PEM and sent as DER
 * verbatim, because a server does not need to understand its own certificate
 * to present it.
 *
 * WHAT IS PRESENT BEYOND THE MINIMUM: session resumption. A full handshake
 * costs this board roughly 170 ms of a single core against a 3 ms request, so
 * resumption is not a nicety, it is most of the page load. Tickets are
 * stateless (sealed under a key this process holds) and resumption is
 * psk_dhe_ke only, so a resumed session still has forward secrecy.
 */
#include "tls.h"
#include "ct.h"
#include "ecdsa.h"
#include "gcm.h"
#include "hkdf.h"
#include "hmac.h"
#include "p256.h"
#include "rand.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- the TLS 1.3 key schedule -------------------------------------------
 *
 * HKDF itself is a primitive and lives in lib/. What is here is the part
 * that is TLS and nothing else: the HkdfLabel structure of RFC 8446 7.1,
 * with its "tls13 " prefix and its length-prefixed context. A library
 * carrying this would be a TLS library, so it stays beside the protocol.
 */
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *ctx, size_t ctxn, uint8_t *out, size_t n)
{
   uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
   size_t k  = 0;
   size_t ln = strlen(label);
   info[k++] = (uint8_t)(n >> 8);
   info[k++] = (uint8_t)n;
   info[k++] = (uint8_t)(6 + ln);
   memcpy(info + k, "tls13 ", 6);
   k += 6;
   memcpy(info + k, label, ln);
   k += ln;
   info[k++] = (uint8_t)ctxn;
   if (ctxn) {
      memcpy(info + k, ctx, ctxn);
      k += ctxn;
   }
   hkdf_expand(secret, info, k, out, n);
}

void derive_secret(const uint8_t secret[32], const char *label,
                   const uint8_t *transcript, size_t tn, uint8_t out[32])
{
   uint8_t th[32];
   sha256(transcript, tn, th);
   hkdf_expand_label(secret, label, th, 32, out, 32);
}

/* ---- wire constants ---------------------------------------------------- */
#define REC_CHANGE_CIPHER 20
#define REC_ALERT         21
#define REC_HANDSHAKE     22
#define REC_APPDATA       23

#define HS_CLIENT_HELLO  1
#define HS_SERVER_HELLO  2
#define HS_NEW_TICKET    4
#define HS_ENCRYPTED_EXT 8
#define HS_CERTIFICATE   11
#define HS_CERT_VERIFY   15
#define HS_FINISHED      20

#define EXT_SERVER_NAME       0
#define EXT_SUPPORTED_GROUPS  10
#define EXT_SIG_ALGS          13
#define EXT_ALPN              16
#define EXT_PRE_SHARED_KEY    41
#define EXT_SUPPORTED_VERSION 43
#define EXT_PSK_MODES         45
#define EXT_KEY_SHARE         51

#define GROUP_P256   0x0017
#define SUITE_AES128 0x1301
#define SIG_ECDSA256 0x0403

/* A change_cipher_spec is a valid 5-byte plaintext record carrying nothing,
 * and a peer can send them forever. Both loops that skip them are bounded by
 * this: unbounded, a stream of them held a worker without ever timing out or
 * advancing the handshake -- cheap for the peer, one of ten workers for us.
 * RFC 8446 permits them only for middlebox compatibility, so a real client
 * sends at most one. */
#define CCS_MAX 8

#define REC_MAX   16384
#define TRANS_MAX 8192 /* the whole handshake transcript; ours is ~1.5 kB */

/* HelloRetryRequest is a ServerHello whose random is this exact value -- the
 * SHA-256 of "HelloRetryRequest", fixed by RFC 8446 4.1.3. */
static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
    0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
    0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

/* ---- the shared half: certificate, key, ticket key --------------------- */

static uint8_t g_cert[4096]; /* the chain, DER, as it will go on the wire */
static size_t g_cert_n;
static uint8_t g_key[32];        /* our EC private scalar */
static uint8_t g_ticket_key[16]; /* seals resumption tickets */

/* ---- the per-connection half ------------------------------------------- */

struct keys {
   uint8_t key[16];
   uint8_t iv[12];
   uint64_t seq;
};

static _Thread_local struct {
   int fd;
   uint8_t trans[TRANS_MAX]; /* handshake transcript, for the hashes */
   size_t trans_n;
   struct keys rd, wr;
   int encrypted;      /* records are protected from here on */
   uint8_t master[32]; /* master secret, for the ticket PSK */
   uint8_t res_master[32];
   /* one buffered record's worth of plaintext, and how much is unread */
   uint8_t plain[REC_MAX + 256];
   size_t plain_n, plain_off;
   uint8_t inbuf[REC_MAX + 512];
   int fatal;
   /* The deadline, for the WRITE side.
    *
    * The read side takes it as a parameter, which works because every reader
    * is reached from a handful of entry points. Writing is not like that: a
    * record is emitted from a dozen places in the handshake, and threading a
    * giveup through all of them would put the argument everywhere and the
    * check nowhere. It belongs to the connection, and the connection already
    * has a thread-local home. Set once per connection; read by full_write. */
   tls_giveup_fn giveup;
} C;

/* ---- little helpers ---------------------------------------------------- */

static void put16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static void put24(uint8_t *p, size_t v)
{
   p[0] = (uint8_t)(v >> 16);
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)v;
}

static unsigned get16(const uint8_t *p)
{
   return ((unsigned)p[0] << 8) | p[1];
}

static size_t get24(const uint8_t *p)
{
   return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2];
}

/* lib/rand.c's, with the same contract: 1 only if every byte is real
 * entropy. This file's callers already treat 0 as fatal for the handshake --
 * an ECDHE scalar or a ticket nonce from stack garbage is the whole key. */
static int rnd(uint8_t *out, size_t n)
{
   return rand_bytes(out, n);
}

/* Everything hashed into the transcript goes through here, so no message can
 * be sent or accepted without being counted. */
static void transcript(const uint8_t *p, size_t n)
{
   if (C.trans_n + n > sizeof C.trans) {
      C.fatal = 1;
      return;
   }
   memcpy(C.trans + C.trans_n, p, n);
   C.trans_n += n;
}

static void transcript_hash(uint8_t out[32])
{
   sha256(C.trans, C.trans_n, out);
}

/* ---- reading and writing records --------------------------------------- */

/* A read or write that failed: is it worth another attempt, or is this socket
 * finished?
 *
 * ONLY the timeout and the interrupt are worth retrying. full_write used to
 * retry on EVERY error, reasoning that the socket carries SO_SNDTIMEO so a
 * failure must be that timeout -- true right up until the peer sends a reset.
 * SIGPIPE is ignored process-wide (srv/http.c), so from then on write()
 * returns -1/EPIPE forever, `sent` never advances, and the loop never ends. A
 * stranger could complete a handshake, reset the connection, and pin a pool
 * worker at 100% CPU for the life of the process -- tls_bye() writes the close
 * alert through here, so merely hanging up was enough. Ten sockets is every
 * worker on a one-core board, permanently. srv/test/tlstest.sh pins it. */
static int retry_after(int err)
{
   return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

/* Read exactly n bytes, or give up.
 *
 * THE DEADLINE IS CHECKED ON EVERY PASS, not only after a read fails. It used
 * to be consulted solely on the `r < 0` branch, on the assumption that waiting
 * only happens when a read times out. A peer that sends ONE BYTE at a time,
 * just often enough to keep the socket readable, never takes that branch: every
 * read succeeds, `got` crawls, and the whole HTTPS_DEADLINE_S budget is never
 * evaluated. The connection then holds a pool worker for as long as the peer
 * cares to keep dribbling -- ten of them is every worker on the board, and
 * unlike a spinning loop this one is invisible, because it costs no CPU.
 *
 * srv/http.c's plain-HTTP read loop has always checked its deadline every
 * iteration. This is that property, restored to the TLS path. */
static int full_read(uint8_t *p, size_t n, tls_giveup_fn giveup)
{
   size_t got = 0;
   while (got < n) {
      if (giveup != NULL && giveup())
         return 0; /* out of time, or somebody else is waiting for a worker */
      errno  = 0;
      long r = read(C.fd, p + got, n - got);
      if (r > 0) {
         got += (size_t)r;
         continue;
      }
      /* Same rule as full_write: only a timeout or a signal is worth another
       * attempt. Anything else -- ECONNRESET, EBADF -- will not improve. */
      if (r < 0 && retry_after(errno))
         continue;
      return 0; /* r == 0 is a clean end of stream, mid-record */
   }
   return 1;
}

/* Write exactly n bytes, or give up -- AND THE DEADLINE IS CHECKED ON EVERY
 * PASS, for the same reason full_read's is.
 *
 * retry_after fixed the loud half of this: a reset peer no longer spins a
 * worker at 100% CPU. The quiet half survived. The socket carries a 1-second
 * SO_SNDTIMEO, so a peer that completes a handshake, asks for a page larger
 * than its receive window and then simply STOPS READING makes write() return
 * -1/EAGAIN once a second, forever, with no budget consulted -- a worker held
 * for as long as the peer cares to hold it, at no CPU cost at all. Ten of them
 * is every worker on a one-core board.
 *
 * It was worse than that, because web_route holds the page mutex across the
 * response write, so ONE such connection stalled every page for every user.
 *
 * srv/test/tlstest.sh missed it by asserting on CPU percentage, which a
 * zero-window stall does not move -- the very property the comment above
 * full_read names when describing the dribble attack from the other
 * direction. The write-side case asserts on WORKER AVAILABILITY instead. */
static int full_write(const uint8_t *p, size_t n)
{
   size_t sent = 0;
   while (sent < n) {
      if (C.giveup != NULL && C.giveup())
         return 0; /* out of time: the response is owed, but not forever */
      errno  = 0;
      long r = write(C.fd, p + sent, n - sent);
      if (r > 0) {
         sent += (size_t)r;
         continue;
      }
      if (r < 0 && retry_after(errno))
         continue; /* the send timeout, or a signal: the peer is still there */
      return 0;    /* EPIPE, ECONNRESET, EBADF, or a zero-length write */
   }
   return 1;
}

/* The AEAD nonce: the static IV with the sequence number XORed into its tail
 * (RFC 8446 5.3). Every record must use a different one, which is why the
 * sequence number is never reset except when the keys change. */
static void nonce_of(const struct keys *k, uint8_t out[12])
{
   memcpy(out, k->iv, 12);
   for (int i = 0; i < 8; i++)
      out[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

static int send_record(int type, const uint8_t *body, size_t n)
{
   uint8_t rec[REC_MAX + 512];
   if (!C.encrypted) {
      rec[0] = (uint8_t)type;
      put16(rec + 1, 0x0303);
      put16(rec + 3, (unsigned)n);
      memcpy(rec + 5, body, n);
      return full_write(rec, 5 + n);
   }
   /* Protected: the real type goes at the END of the plaintext and the outer
    * type is always application_data, so a watcher cannot tell a handshake
    * message from a page. */
   size_t inner = n + 1;
   rec[0]       = REC_APPDATA;
   put16(rec + 1, 0x0303);
   put16(rec + 3, (unsigned)(inner + 16));
   uint8_t pt[REC_MAX + 1];
   memcpy(pt, body, n);
   pt[n] = (uint8_t)type;
   uint8_t nonce[12];
   nonce_of(&C.wr, nonce);
   aes128_gcm_seal(C.wr.key, nonce, rec, 5, pt, inner, rec + 5,
                   rec + 5 + inner);
   C.wr.seq++;
   return full_write(rec, 5 + inner + 16);
}

/* Read one record into C.plain. Returns its content type, or -1. */
static int read_record(tls_giveup_fn giveup)
{
   uint8_t hdr[5];
   if (!full_read(hdr, 5, giveup))
      return -1;
   size_t n = get16(hdr + 3);
   if (n > REC_MAX + 256)
      return -1;
   if (!full_read(C.inbuf, n, giveup))
      return -1;

   if (!C.encrypted) {
      memcpy(C.plain, C.inbuf, n);
      C.plain_n   = n;
      C.plain_off = 0;
      return hdr[0];
   }
   /* falls through to the protected path */
   if (hdr[0] == REC_CHANGE_CIPHER)
      return REC_CHANGE_CIPHER; /* middlebox compatibility: ignored */
   if (n < 17)
      return -1;
   uint8_t nonce[12];
   nonce_of(&C.rd, nonce);
   if (!aes128_gcm_open(C.rd.key, nonce, hdr, 5, C.inbuf, n - 16,
                        C.inbuf + n - 16, C.plain))
      return -1;
   C.rd.seq++;
   size_t m = n - 16;
   while (m && C.plain[m - 1] == 0) /* strip the padding */
      m--;
   if (!m)
      return -1;
   C.plain_n   = m - 1;
   C.plain_off = 0;
   return C.plain[m - 1]; /* the real content type */
}

/* ---- the key schedule -------------------------------------------------- */

static void traffic_keys(const uint8_t secret[32], struct keys *k)
{
   hkdf_expand_label(secret, "key", NULL, 0, k->key, 16);
   hkdf_expand_label(secret, "iv", NULL, 0, k->iv, 12);
   k->seq = 0;
}

static void finished_mac(const uint8_t secret[32], uint8_t out[32])
{
   uint8_t fk[32], th[32];
   hkdf_expand_label(secret, "finished", NULL, 0, fk, 32);
   transcript_hash(th);
   hmac_sha256(fk, 32, th, 32, out);
}

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
static size_t pem_decode(const char *pem, uint8_t *out, size_t cap,
                         size_t *starts, size_t *lens, size_t maxblocks,
                         size_t *nblocks)
{
   size_t total  = 0;
   *nblocks      = 0;
   const char *p = pem;
   while ((p = strstr(p, "-----BEGIN")) != NULL) {
      const char *nl = strchr(p, '\n');
      if (!nl)
         break;
      const char *end = strstr(nl, "-----END");
      if (!end)
         break;
      size_t start = total;
      int acc = 0, bits = 0;
      for (const char *q = nl; q < end; q++) {
         int v = b64val((unsigned char)*q);
         if (v < 0)
            continue;
         acc = (acc << 6) | v;
         bits += 6;
         if (bits >= 8) {
            bits -= 8;
            if (total < cap)
               out[total++] = (uint8_t)(acc >> bits);
         }
      }
      if (*nblocks < maxblocks) {
         starts[*nblocks] = start;
         lens[*nblocks]   = total - start;
         (*nblocks)++;
      }
      p = end + 1;
   }
   return total;
}

static char *slurp(const char *path, size_t *n)
{
   static char buf[16384];
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return NULL;
   long r = read(fd, buf, sizeof buf - 1);
   close(fd);
   if (r <= 0)
      return NULL;
   buf[r] = '\0';
   *n     = (size_t)r;
   return buf;
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

int tls_init(const char *cert_pem, const char *key_pem, const char *name)
{
   p256_init();
   size_t n;
   char *txt = slurp(cert_pem, &n);
   if (!txt) {
      fprintf(stderr, "%s: cannot read %s\n", name, cert_pem);
      return 1;
   }
   size_t starts[8], lens[8], nb;
   uint8_t der[4096];
   size_t dn = pem_decode(txt, der, sizeof der, starts, lens, 8, &nb);
   if (!nb) {
      fprintf(stderr, "%s: no certificate in %s\n", name, cert_pem);
      return 1;
   }
   /* Build the Certificate message's cert_list once: for each certificate a
    * 3-byte length, the DER, and an empty extension list. */
   size_t k = 0;
   for (size_t i = 0; i < nb; i++) {
      /* -4 for the Certificate message's own framing, which send_certificate
       * prepends: the bound has to be the one the WIRE needs, not the one the
       * array happens to have, or a chain that fits here overflows there. */
      if (k + 3 + lens[i] + 2 > sizeof g_cert - 4) {
         /* REFUSE, do not truncate. This used to `break`, leaving a partial
          * chain -- or none at all, if the very first certificate did not fit
          * -- and tls_init still returned success. The server then came up
          * presenting something no client can verify, with nothing on stderr
          * to say why: the same failure mode as the RSA key below, which is
          * refused outright precisely because a server nobody can reach is
          * worse than a server that did not start. */
         fprintf(stderr,
                 "%s: certificate chain in %s does not fit in %zu bytes "
                 "(stopped at certificate %zu of %zu)\n",
                 name, cert_pem, sizeof g_cert - 4, i + 1, nb);
         return 1;
      }
      put24(g_cert + k, lens[i]);
      k += 3;
      memcpy(g_cert + k, der + starts[i], lens[i]);
      k += lens[i];
      put16(g_cert + k, 0);
      k += 2;
   }
   g_cert_n = k;
   (void)dn;

   txt = slurp(key_pem, &n);
   if (!txt) {
      fprintf(stderr, "%s: cannot read %s\n", name, key_pem);
      return 1;
   }
   uint8_t kder[4096];
   size_t kn = pem_decode(txt, kder, sizeof kder, starts, lens, 8, &nb);
   if (!nb || !find_ec_scalar(kder, kn, g_key)) {
      fprintf(stderr, "%s: no EC private key in %s\n", name, key_pem);
      return 1;
   }
   if (!rnd(g_ticket_key, 16)) {
      fprintf(stderr, "%s: no entropy for the ticket key\n", name);
      return 1;
   }
   return 0;
}

/* ---- ClientHello -------------------------------------------------------
 *
 * Only the four extensions that decide anything are read: which TLS version
 * the client really wants, its key share, whether it can verify our signature
 * algorithm, and any resumption ticket. Everything else -- server name, ALPN,
 * the dozen extensions a browser sends -- is skipped, because nothing this
 * server does depends on them.
 */
struct hello {
   const uint8_t *session_id;
   size_t session_id_n;
   int has_tls13;
   int suite_ok;
   int sig_ok;
   const uint8_t *peer_key; /* 65-byte uncompressed P-256 point, or NULL */
   const uint8_t *psk_ident;
   size_t psk_ident_n;
   const uint8_t *binders; /* start of the binder list, for the transcript */
   size_t binders_n;
   int psk_dhe;
};

static int parse_hello(const uint8_t *b, size_t n, struct hello *h)
{
   memset(h, 0, sizeof *h);
   size_t i = 0;
   if (n < 38)
      return 0;
   i += 2 + 32; /* legacy_version, random */
   size_t sid = b[i++];
   if (i + sid > n)
      return 0;
   h->session_id   = b + i;
   h->session_id_n = sid;
   i += sid;
   if (i + 2 > n)
      return 0;
   size_t cs = get16(b + i);
   i += 2;
   for (size_t j = 0; j + 1 < cs && i + j + 1 < n; j += 2)
      if (get16(b + i + j) == SUITE_AES128)
         h->suite_ok = 1;
   i += cs;
   if (i >= n)
      return 0;
   i += 1 + b[i]; /* legacy_compression_methods */
   if (i + 2 > n)
      return 0;
   size_t extn = get16(b + i);
   i += 2;
   size_t end = i + extn;
   if (end > n)
      return 0;

   while (i + 4 <= end) {
      unsigned type    = get16(b + i);
      size_t len       = get16(b + i + 2);
      const uint8_t *v = b + i + 4;
      if (i + 4 + len > end)
         return 0;
      switch (type) {
         case EXT_SUPPORTED_VERSION:
            for (size_t j = 1; j + 1 < len; j += 2)
               if (get16(v + j) == 0x0304)
                  h->has_tls13 = 1;
            break;
         case EXT_SIG_ALGS:
            for (size_t j = 2; j + 1 < len; j += 2)
               if (get16(v + j) == SIG_ECDSA256)
                  h->sig_ok = 1;
            break;
         case EXT_KEY_SHARE: {
            size_t j = 2; /* client_shares length */
            while (j + 4 <= len) {
               unsigned grp = get16(v + j);
               size_t kl    = get16(v + j + 2);
               /* THE WHOLE SHARE MUST LIE INSIDE THE EXTENSION before any of
                * it is read. `v[j + 4]` alone was read when j + 4 == len --
                * one byte past the end -- and the 65 bytes it selected were
                * then handed to p256_from_xy, which reads all of them. A
                * seven-byte body (00 07 | 00 17 | 00 41 | 04) walked 64 bytes
                * off an attacker-chosen extension, before any authentication
                * had happened. */
               if (j + 4 + kl > len)
                  break;
               if (grp == GROUP_P256 && kl == 65 && v[j + 4] == 0x04)
                  h->peer_key = v + j + 4;
               j += 4 + kl;
            }
            break;
         }
         case EXT_PSK_MODES:
            for (size_t j = 1; j < len; j++)
               if (v[j] == 1) /* psk_dhe_ke */
                  h->psk_dhe = 1;
            break;
         case EXT_PRE_SHARED_KEY: {
            if (len <
                2) /* get16 below reads two bytes; an empty one had none */
               break;
            size_t idn = get16(v);
            size_t j   = 2;
            if (j + 2 <= len) { /* first identity only: we issue one ticket */
               size_t il = get16(v + j);
               if (j + 2 + il <= len) {
                  h->psk_ident   = v + j + 2;
                  h->psk_ident_n = il;
               }
            }
            j = 2 + idn;
            if (j + 2 <= len) {
               h->binders   = v + j;
               h->binders_n = len - j;
            }
            break;
         }
         default: break;
      }
      i += 4 + len;
   }
   /* pre_shared_key MUST be the last extension in the ClientHello (RFC 8446
    * 4.2.11), and a server that sees it elsewhere must abort. The reason is
    * structural: the binder is a MAC over the ClientHello UP TO the start of
    * the binder list, so "everything before the binders" is only a
    * well-defined span if nothing follows them. The struct used to carry a
    * psk_ext_off recorded for exactly this check, which nothing ever read --
    * the intent was in the field and the check was missing. The binder span
    * answers it directly, so the field is gone.
    *
    * `i` is the offset just past the final extension, so a pre_shared_key
    * that ended anywhere but there was not last. */
   if (h->binders != NULL && (size_t)(h->binders - b) + h->binders_n != i)
      return 0;
   return 1;
}

/* ---- tickets -----------------------------------------------------------
 *
 * Stateless: the ticket carries the PSK, sealed under a key only this process
 * holds, so nothing has to be remembered between connections and a restart
 * simply invalidates outstanding tickets.
 * Layout: nonce || AEAD(issued_at || psk) || tag.
 *
 * THE ISSUE TIME IS SEALED WITH IT, and checked on the way back in. The
 * ticket message on the wire advertises a 7200 s lifetime, and RFC 8446
 * 4.6.1 makes that binding on the server -- but the sealed blob used to be
 * the bare PSK, and ticket_open checked only its length and tag, so a ticket
 * resumed an authenticated session for as long as the PROCESS lived. On a
 * board that runs for months that is indistinguishable from forever: one
 * copied ticket is a session that never has to be re-authenticated.
 *
 * The clock is CLOCK_MONOTONIC (http_mono_s), not the wall clock. The ticket
 * key is per-process, so a ticket cannot outlive the process anyway, and
 * uptime is the one clock this board cannot get wrong -- it has no RTC, and
 * the wall clock jumps when NTP finally lands. */
#define TICKET_LIFETIME_S 7200

/* Seconds of uptime. Written here rather than borrowed from http.c so this
 * file stays linkable on its own -- srv/test/cryptotest.c builds tls.c
 * against lib/ and nothing else, and a dependency on the HTTP layer for a
 * clock would have made the crypto vectors need a web server. */
static uint64_t tls_mono_s(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec;
}

static size_t ticket_seal(const uint8_t psk[32], uint8_t *out)
{
   uint8_t nonce[12];
   uint8_t pt[8 + 32];
   if (!rnd(nonce, 12))
      return 0;
   uint64_t now = tls_mono_s();
   for (int i = 0; i < 8; i++)
      pt[i] = (uint8_t)(now >> (8 * (7 - i)));
   memcpy(pt + 8, psk, 32);
   memcpy(out, nonce, 12);
   aes128_gcm_seal(g_ticket_key, nonce, NULL, 0, pt, sizeof pt, out + 12,
                   out + 12 + sizeof pt);
   return 12 + sizeof pt + 16;
}

static int ticket_open(const uint8_t *t, size_t n, uint8_t psk[32])
{
   uint8_t pt[8 + 32];
   if (n != 12 + sizeof pt + 16)
      return 0;
   if (!aes128_gcm_open(g_ticket_key, t, NULL, 0, t + 12, sizeof pt,
                        t + 12 + sizeof pt, pt))
      return 0;
   uint64_t issued = 0;
   for (int i = 0; i < 8; i++)
      issued = (issued << 8) | pt[i];
   uint64_t now = tls_mono_s();
   /* now < issued cannot happen with a monotonic clock, but an unsigned
    * subtraction that underflowed would read as "brand new" forever, so the
    * comparison is written to refuse rather than wrap. */
   if (now < issued || now - issued > TICKET_LIFETIME_S)
      return 0;
   memcpy(psk, pt + 8, 32);
   return 1;
}

/* ---- the handshake ------------------------------------------------------ */

static int send_handshake(int type, const uint8_t *body, size_t n)
{
   uint8_t msg[REC_MAX];
   if (n + 4 > sizeof msg)
      return 0;
   msg[0] = (uint8_t)type;
   put24(msg + 1, n);
   memcpy(msg + 4, body, n);
   transcript(msg, n + 4);
   return send_record(REC_HANDSHAKE, msg, n + 4);
}

/* ServerHello, or a HelloRetryRequest if `hrr` -- they are the same message
 * with a different random, which is why they share this code. */
static int send_server_hello(const struct hello *h, const uint8_t *share,
                             int hrr, int psk_used)
{
   uint8_t b[512];
   size_t k = 0;
   put16(b + k, 0x0303);
   k += 2;
   if (hrr) {
      memcpy(b + k, HRR_RANDOM, 32);
   } else if (!rnd(b + k, 32)) {
      return 0;
   }
   k += 32;
   b[k++] = (uint8_t)h->session_id_n;
   memcpy(b + k, h->session_id, h->session_id_n);
   k += h->session_id_n;
   put16(b + k, SUITE_AES128);
   k += 2;
   b[k++] = 0; /* legacy_compression_method */

   size_t extlen_at = k;
   k += 2;
   put16(b + k, EXT_SUPPORTED_VERSION);
   put16(b + k + 2, 2);
   put16(b + k + 4, 0x0304);
   k += 6;
   put16(b + k, EXT_KEY_SHARE);
   if (hrr) { /* a retry names the group it wants, and sends no share */
      put16(b + k + 2, 2);
      put16(b + k + 4, GROUP_P256);
      k += 6;
   } else {
      put16(b + k + 2, 2 + 2 + 65);
      put16(b + k + 4, GROUP_P256);
      put16(b + k + 6, 65);
      memcpy(b + k + 8, share, 65);
      k += 8 + 65;
   }
   if (psk_used) {
      put16(b + k, EXT_PRE_SHARED_KEY);
      put16(b + k + 2, 2);
      put16(b + k + 4, 0); /* we only ever offer identity 0 */
      k += 6;
   }
   put16(b + extlen_at, (unsigned)(k - extlen_at - 2));
   return send_handshake(HS_SERVER_HELLO, b, k);
}

/* DER-encode the ECDSA (r, s) pair the way TLS wants it. */
static size_t der_sig(const uint8_t r[32], const uint8_t s[32], uint8_t *out)
{
   uint8_t body[80];
   size_t k = 0;
   for (int half = 0; half < 2; half++) {
      const uint8_t *v = half ? s : r;
      size_t skip      = 0;
      while (skip < 31 && v[skip] == 0)
         skip++;
      int pad   = (v[skip] & 0x80) ? 1 : 0; /* keep it positive */
      body[k++] = 0x02;
      body[k++] = (uint8_t)(32 - skip + pad);
      if (pad)
         body[k++] = 0;
      memcpy(body + k, v + skip, 32 - skip);
      k += 32 - skip;
   }
   out[0] = 0x30;
   out[1] = (uint8_t)k;
   memcpy(out + 2, body, k);
   return k + 2;
}

static int send_cert_verify(void)
{
   /* RFC 8446 4.4.3: 64 spaces, a context string, a zero, then the hash. */
   uint8_t tbs[64 + 34 + 1 + 32];
   size_t k = 0;
   memset(tbs, 0x20, 64);
   k = 64;
   memcpy(tbs + k, "TLS 1.3, server CertificateVerify", 33);
   k += 33;
   tbs[k++] = 0;
   transcript_hash(tbs + k);
   k += 32;

   uint8_t h[32];
   sha256(tbs, k, h);
   uint8_t nonce[32], r[32], s[32];
   if (!rnd(nonce, 32) || !ecdsa_p256_sign(g_key, h, nonce, r, s))
      return 0;

   uint8_t b[128];
   put16(b, SIG_ECDSA256);
   size_t sn = der_sig(r, s, b + 4);
   put16(b + 2, (unsigned)sn);
   return send_handshake(HS_CERT_VERIFY, b, 4 + sn);
}

/* The Certificate message: one empty request context, then the chain.
 *
 * The 4 bytes of framing are why the bound is `sizeof b - 4` and not
 * `sizeof b`. tls_init only checks the chain against sizeof g_cert, so a
 * chain of 4093..4096 bytes loaded happily and then wrote up to four bytes
 * past this buffer on EVERY handshake -- with -static -no-pie and no stack
 * protector under it. A three-certificate chain reaches that size, so it was
 * a configuration change away, not an attack away. */
static int send_certificate(void)
{
   uint8_t b[4096];
   if (g_cert_n > sizeof b - 4)
      return 0;
   b[0] = 0; /* certificate_request_context: empty */
   put24(b + 1, g_cert_n);
   memcpy(b + 4, g_cert, g_cert_n);
   return send_handshake(HS_CERTIFICATE, b, 4 + g_cert_n);
}

static int send_ticket(void)
{
   uint8_t nonce[8] = {0, 0, 0, 0, 0, 0, 0, 1};
   uint8_t psk[32];
   hkdf_expand_label(C.res_master, "resumption", nonce, 8, psk, 32);
   uint8_t t[80];
   size_t tn = ticket_seal(psk, t);
   if (!tn)
      return 0;
   uint8_t b[128];
   size_t k = 0;
   b[k++]   = 0;
   b[k++]   = 0;
   b[k++]   = 0x1c; /* lifetime 7200 s: TICKET_LIFETIME_S, enforced above */
   b[k++]   = 0x20;
   put16(b + k, 0); /* age_add: we do not use it */
   put16(b + k + 2, 0);
   k += 4;
   b[k++] = 8;
   memcpy(b + k, nonce, 8);
   k += 8;
   put16(b + k, (unsigned)tn);
   k += 2;
   memcpy(b + k, t, tn);
   k += tn;
   put16(b + k, 0); /* no extensions */
   k += 2;
   return send_handshake(HS_NEW_TICKET, b, k);
}

/* Read one handshake message into `out`, skipping change_cipher_spec. */
static int read_handshake(uint8_t *out, size_t cap, int *type, size_t *n,
                          tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = read_record(giveup);
      if (t < 0)
         return 0;
      if (t == REC_CHANGE_CIPHER) {
         if (++skipped > CCS_MAX) /* see next_record: they are unbounded free */
            return 0;
         continue;
      }
      if (t != REC_HANDSHAKE)
         return 0;
      if (C.plain_n < 4)
         return 0;
      size_t len = get24(C.plain + 1);
      if (len + 4 > C.plain_n || len > cap)
         return 0;
      *type = C.plain[0];
      *n    = len;
      memcpy(out, C.plain + 4, len);
      transcript(C.plain, len + 4); /* the message as sent, header included */
      return 1;
   }
}

/* The next record that is not a change_cipher_spec. Clients send those for
 * middlebox compatibility, before AND after a HelloRetryRequest, and they
 * carry no meaning for us -- but they are not handshake messages either, so
 * they must be skipped rather than rejected. */
/* The next record that means something, skipping change_cipher_spec. */
static int next_record(tls_giveup_fn giveup)
{
   int skipped = 0;
   for (;;) {
      int t = read_record(giveup);
      if (t != REC_CHANGE_CIPHER)
         return t;
      if (++skipped > CCS_MAX)
         return -1;
   }
}

int tls_handshake(int fd, tls_giveup_fn giveup)
{
   memset(&C, 0, sizeof C);
   C.fd = fd;
   /* The write side gets the same budget the read side is given. Set here
    * because the handshake itself writes -- a peer that stops reading during
    * the handshake stalls a worker exactly as one that stops reading during
    * the response does. */
   C.giveup = giveup;

   uint8_t body[REC_MAX];
   int type;
   size_t n;

   /* --- ClientHello. It arrives as a plaintext record. --- */
   int t = next_record(giveup);
   if (t != REC_HANDSHAKE || C.plain_n < 4 || C.plain[0] != HS_CLIENT_HELLO)
      return 0;
   size_t chn = get24(C.plain + 1);
   if (chn + 4 > C.plain_n)
      return 0;
   struct hello h;
   if (!parse_hello(C.plain + 4, chn, &h) || !h.has_tls13 || !h.suite_ok ||
       !h.sig_ok)
      return 0;

   /* A resumption offer is authenticated by its binder, which is computed
    * over the ClientHello UP TO the binder list -- so the transcript has to
    * be truncated at exactly that point before it can be checked. */
   uint8_t psk[32];
   int resumed = 0;
   if (h.psk_ident && h.binders && h.psk_dhe &&
       ticket_open(h.psk_ident, h.psk_ident_n, psk)) {
      /* The binder covers the ClientHello TRUNCATED at the binder list --
       * everything before the list's own 2-byte length, header included.
       * h.binders points at that length field, so the truncation point is
       * exactly its offset. */
      size_t upto = (size_t)(h.binders - C.plain);
      uint8_t early[32], binder_key[32], fk[32], th[32], want[32];
      hkdf_extract(NULL, 0, psk, 32, early);
      derive_secret(early, "res binder", (const uint8_t *)"", 0, binder_key);
      hkdf_expand_label(binder_key, "finished", NULL, 0, fk, 32);
      sha256(C.plain, upto, th);
      hmac_sha256(fk, 32, th, 32, want);
      /* The binder list is: 2-byte list length, then 1-byte entry length. */
      /* ct_eq, not memcmp: the binder is an authenticator a client may
       * retry, so a comparison that returns early leaks how many bytes of a
       * guess were right. */
      if (h.binders_n >= 3 + 32 && h.binders[2] == 32 &&
          ct_eq(want, h.binders + 3, 32))
         resumed = 1;
   }

   transcript(C.plain, chn + 4);

   /* --- HelloRetryRequest, if the client offered no P-256 share. Browsers
    * lead with X25519, so this is the common path, not an edge case. --- */
   if (!h.peer_key) {
      /* RFC 8446 4.4.1: once a retry happens the transcript is NOT the two
       * ClientHellos in sequence. The first one is replaced by a synthetic
       * "message_hash" message carrying its hash, and everything after
       * follows that. Get this wrong and the handshake fails at the very
       * last step, when the Finished MACs disagree for no visible reason. */
      uint8_t th[32];
      transcript_hash(th);
      C.trans_n        = 0;
      uint8_t synth[4] = {254, 0, 0, 32}; /* message_hash, length 32 */
      transcript(synth, 4);
      transcript(th, 32);

      if (!send_server_hello(&h, NULL, 1, 0)) /* appends the HRR itself */
         return 0;
      t = next_record(giveup);
      if (t != REC_HANDSHAKE || C.plain_n < 4 || C.plain[0] != HS_CLIENT_HELLO)
         return 0;
      chn = get24(C.plain + 1);
      /* THE SECOND ClientHello IS CHECKED AS FULLY AS THE FIRST.
       *
       * parse_hello overwrites `h` wholesale, so every conclusion drawn from
       * CH1 is gone by this point -- but only peer_key was re-tested, and
       * has_tls13 / suite_ok / sig_ok were re-derived and then ignored. RFC
       * 8446 4.1.2 requires CH2 to be the same ClientHello but for the
       * permitted changes, and 4.1.4 requires the server to abort if the
       * retried hello no longer offers what it needs. A client could offer
       * TLS 1.3 and a suite we support in CH1 and neither in CH2.
       *
       * `resumed` and `psk` deliberately carry over: they were established
       * from CH1's binder, which WAS verified. CH2's binder is not re-checked
       * here, and the retry does not create a new resumption offer. */
      if (chn + 4 > C.plain_n || !parse_hello(C.plain + 4, chn, &h) ||
          !h.has_tls13 || !h.suite_ok || !h.sig_ok || !h.peer_key)
         return 0;
      transcript(C.plain, chn + 4);
   }

   /* --- ECDHE. Our ephemeral key never leaves this function. --- */
   uint8_t priv[32], pub[65];
   struct u256 d;
   struct jpoint Q, P, S;
   if (!rnd(priv, 32))
      return 0;
   p256_sc_from_be(&d, priv);
   p256_mul_g(&Q, &d);
   p256_uncompressed(&Q, pub);
   if (!p256_from_xy(&P, h.peer_key + 1, h.peer_key + 33))
      return 0;
   p256_mul(&S, &d, &P);
   uint8_t sx[32], sy[32];
   if (!p256_to_xy(&S, sx, sy))
      return 0; /* the shared secret is the x coordinate */

   if (!send_server_hello(&h, pub, 0, resumed))
      return 0;

   /* --- the key schedule --- */
   uint8_t early[32], derived[32], hs[32], chs[32], shs[32];
   uint8_t zeros[32] = {0};
   hkdf_extract(NULL, 0, resumed ? psk : zeros, 32, early);
   derive_secret(early, "derived", (const uint8_t *)"", 0, derived);
   hkdf_extract(derived, 32, sx, 32, hs);
   derive_secret(hs, "c hs traffic", C.trans, C.trans_n, chs);
   derive_secret(hs, "s hs traffic", C.trans, C.trans_n, shs);
   traffic_keys(shs, &C.wr);
   traffic_keys(chs, &C.rd);
   C.encrypted = 1;

   /* --- our half of the handshake, all of it encrypted --- */
   uint8_t ee[2] = {0, 0}; /* EncryptedExtensions: none */
   if (!send_handshake(HS_ENCRYPTED_EXT, ee, 2))
      return 0;
   if (!resumed) { /* a resumed session is already authenticated by the PSK */
      if (!send_certificate() || !send_cert_verify())
         return 0;
   }
   uint8_t fin[32];
   finished_mac(shs, fin);
   if (!send_handshake(HS_FINISHED, fin, 32))
      return 0;

   /* --- master secret and the application keys --- */
   uint8_t derived2[32], master[32], cap_[32], sap[32];
   derive_secret(hs, "derived", (const uint8_t *)"", 0, derived2);
   hkdf_extract(derived2, 32, zeros, 32, master);
   derive_secret(master, "c ap traffic", C.trans, C.trans_n, cap_);
   derive_secret(master, "s ap traffic", C.trans, C.trans_n, sap);
   memcpy(C.master, master, 32);

   /* --- the client's Finished, under the handshake key --- */
   uint8_t want[32];
   finished_mac(chs, want); /* before the message joins the transcript */
   if (!read_handshake(body, sizeof body, &type, &n, giveup) ||
       type != HS_FINISHED || n != 32 || !ct_eq(body, want, 32))
      return 0;

   derive_secret(master, "res master", C.trans, C.trans_n, C.res_master);

   /* --- from here the connection carries application data --- */
   traffic_keys(sap, &C.wr);
   traffic_keys(cap_, &C.rd);
   C.plain_n = C.plain_off = 0;

   /* A ticket, so the next connection can skip all of the above. */
   (void)send_ticket();
   return !C.fatal;
}

ssize_t tls_recv(void *buf, size_t n, tls_giveup_fn giveup)
{
   for (;;) {
      if (C.plain_off < C.plain_n) {
         size_t take = C.plain_n - C.plain_off;
         if (take > n)
            take = n;
         memcpy(buf, C.plain + C.plain_off, take);
         C.plain_off += take;
         return (ssize_t)take;
      }
      int t = read_record(giveup);
      if (t < 0)
         return -1;
      if (t == REC_APPDATA)
         continue; /* the loop above hands it back */
      if (t == REC_ALERT)
         return 0; /* close_notify, or anything else: we are done */
      /* A post-handshake message (a KeyUpdate we do not do, a ticket the
       * client should not send): ignore it and keep reading. */
      C.plain_n = C.plain_off = 0;
   }
}

ssize_t tls_send(const void *buf, size_t n, tls_giveup_fn giveup)
{
   /* Honoured now, rather than discarded. tls.h promises this function
    * "moves all n bytes or fails"; without a budget it could do neither. */
   if (giveup != NULL)
      C.giveup = giveup;
   const uint8_t *p = buf;
   size_t sent      = 0;
   while (sent < n) {
      size_t take = n - sent;
      if (take > REC_MAX - 64)
         take = REC_MAX - 64;
      if (!send_record(REC_APPDATA, p + sent, take))
         return -1;
      sent += take;
   }
   return (ssize_t)n;
}

int tls_pending(void)
{
   return C.plain_off < C.plain_n;
}

void tls_bye(void)
{
   uint8_t alert[2] = {1, 0}; /* warning, close_notify */
   if (C.encrypted)
      (void)send_record(REC_ALERT, alert, 2);
}
