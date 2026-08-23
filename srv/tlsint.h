// SPDX-License-Identifier: GPL-3.0
// tlsint.h --- the seam between the TLS module's five files, and nothing wider
// Copyright 2026 Jakob Kastelic

/* NOT PART OF THE TLS INTERFACE. srv/tls.h is that, and it hands out a
 * connection and nothing else -- a caller that could reach the private scalar
 * is a caller that can sign with it.
 *
 * The TLS module is five files -- tls.c (the handshake and the public API),
 * tlsrec.c (the record layer), tlstkt.c (the tickets), tlshello.c (the
 * ClientHello parser) and tlscred.c (the certificate and key, read once at
 * startup). This is everything they share and nothing else should include
 * it. The block below says what the split preserves. */
#ifndef PANCRA_TLSINT_H
#define PANCRA_TLSINT_H

/* NOT #include "tls.h". The public header and this private one would then be
 * ONE node in the include graph -- a cycle, since every file that includes
 * this one includes that one too.
 * The one thing needed from it is the deadline callback's type, which is
 * written out here: a typedef cannot be forward-declared, and repeating four
 * words is cheaper than a cycle. It is checked against tls.h by the compiler
 * at every call site. */
#include <stddef.h>
#include <stdint.h>

/* WRITTEN ONCE, AT STARTUP, by tls_init in srv/tlscred.c -- before any
 * connection exists -- and read-only for every handshake afterwards. That
 * ordering is what makes them safe to share without a lock: there is no reader
 * until the listener is up, and no writer after it. */
/* THE CHAIN, ENCODED AS IT GOES ON THE WIRE, and allocated to fit it. A fixed
 * `uint8_t g_cert[4096]` sizes the credential by the array rather than the
 * other way round: a CA that renews with one more intermediate produces a
 * perfectly valid chain this server refuses to start on -- at the moment the
 * certificate is rotated, which is the moment nobody is watching. See
 * TLS_CHAIN_MAX below for the bound that does apply. */
extern const uint8_t *g_cert;
extern size_t g_cert_n;
extern uint8_t g_key[32];        /* our EC private scalar */
extern uint8_t g_ticket_key[16]; /* seals resumption tickets */

/* BIG-ENDIAN FIELD WRITERS, shared because both halves build wire records:
 * the handshake writes lengths into every message, and the credential half
 * writes the 24-bit lengths that frame each certificate in the chain.
 *
 * `static inline` rather than exported: they are three assignments each, and
 * a call through the seam would be a worse trade than two copies the compiler
 * folds away. */
static inline void put16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)(v >> 8);
   p[1] = (uint8_t)v;
}

static inline void put24(uint8_t *p, size_t v)
{
   p[0] = (uint8_t)(v >> 16);
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)v;
}

#define CCS_MAX 8

/* KeyUpdates per connection. Same reasoning as CCS_MAX and the same shape of
 * abuse: a KeyUpdate is five bytes on the wire and costs us two HKDF
 * expansions plus, when it asks for one back, a record of our own -- so an
 * unbounded stream of them is a cheap way to make one of ten workers do
 * nothing but rekey. A real peer rekeys when a sequence number is running out
 * (RFC 8446 5.5), which on connections that live for a handful of requests is
 * never; the ones seen in practice are the one or two an operator triggers by
 * hand. Thirty-two is far past any of that and far short of free. */
#define KEY_UPDATE_MAX 32

#define REC_MAX 16384 /* TLS 1.3's own plaintext record ceiling (RFC 8446) */

/* ---- HOW BIG A CREDENTIAL MAY BE, SAID ONCE ----------------
 *
 * Four different numbers can end up bounding the certificate chain, and not
 * one of them is about certificates: the buffer slurp() happens to declare
 * (16 KiB), the DER array in tls_init (4 KiB), the eight-entry block table
 * beside it, and g_cert (4 KiB again) -- with the Certificate message copied
 * into a
 * fifth 4 KiB buffer on every handshake. A chain that outgrew any one of them
 * was refused, or silently truncated, or hashed into a transcript that then
 * overflowed. And a chain GROWS WITHOUT ANYBODY DECIDING IT SHOULD: a CA adds
 * a cross-signed intermediate, the renewal script fetches the new fullchain,
 * and the next restart does not come back.
 *
 * So there is one number, it is about the credential, and everything else is
 * derived from it or allocated to fit:
 *
 *   TLS_CHAIN_MAX  the encoded cert_list, exactly as sent. A real chain is a
 *                  leaf plus one or two intermediates -- four to five KiB.
 *                  This is several times that, and it is the number the
 *                  refusal message quotes.
 *   TLS_CRED_MAX   a PEM file on disk: base64 is four bytes per three, plus
 *                  headers and line breaks, so a shade over 4/3 of the DER --
 *                  rounded up, and it bounds the private key file too.
 *   TRANS_MAX      the handshake transcript has to HOLD the Certificate
 *                  message, so it is the chain plus room for every other
 *                  message in the flight (ours are ~1.5 kB in total). Stating
 *                  it as a sum is the point: a chain that fits the wire and
 *                  not the transcript would fail at CertificateVerify, on
 *                  every connection, with nothing on stderr.
 *
 * The cost is memory the process would otherwise not hold: the transcript is
 * per worker. That is the trade being made deliberately -- a board with a few
 * hundred KiB spare, against a service that does not come back after a
 * routine certificate renewal. */
#define TLS_CHAIN_MAX (24 * 1024)
#define TLS_CRED_MAX  (48 * 1024)
#define TRANS_MAX     (TLS_CHAIN_MAX + 8192)

/* HelloRetryRequest is a ServerHello whose random is this exact value -- the
 * SHA-256 of "HelloRetryRequest", fixed by RFC 8446 4.1.3. */
static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
    0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
    0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

/* ---- the shared half: certificate, key, ticket key --------------------- */

/* THE FOUR THE TWO HALVES SHARE. Filled once by tls_init (srv/tlscred.c) and
 * read by every handshake here; declared in srv/tlsint.h, which is the seam
 * left by splitting this module across two files and is not part of tls.h. */

/* ---- THE CONNECTION, AND THE THREE FILES THAT SHARE IT ----
 *
 * srv/tls.c was one file doing four jobs: the socket and record protection,
 * the key schedule, the ticket seal, and the handshake itself. It is three
 * now, behind the same srv/tls.h -- which has not changed, and is still the
 * only thing anything outside this module sees:
 *
 *   srv/tlsrec.c   the RECORD LAYER: reading and writing bytes on the socket
 *                  under the deadline, sealing and opening a record, the
 *                  transcript, and the alert that ends a connection badly.
 *   srv/tlstkt.c   the TICKETS: sealing one, opening one, and the binder that
 *                  proves a client holds the PSK inside it.
 *   srv/tls.c      the HANDSHAKE and the public API on top of both.
 *
 * WHAT DID NOT CHANGE, and the split would be a regression if it had:
 *
 *   - tls_conn OWNERSHIP. One per worker, thread-local, handed out once by
 *     tls_conn_slot and passed explicitly everywhere after that. No file
 *     below reaches for "the current connection".
 *   - THE FATAL FLAG. `fatal` is set where a protocol violation is DETECTED
 *     and read by the entry points; a record layer that hides a failure from
 *     the handshake is a handshake that carries on encrypting to nobody.
 *   - THE DEADLINE. `giveup` belongs to the connection and every write
 *     consults it; the read side takes it as an argument, because its callers
 *     are few and each has its own budget.
 *   - THE TRANSIENT SECRETS. One cleanup path, at the end of tls_handshake:
 *     the handshake secrets are wiped there and nowhere else, so there is one
 *     place to check rather than four.
 */
/* ---- the per-connection half ------------------------------------------- */

struct keys {
   uint8_t key[16];
   uint8_t iv[12];
   uint64_t seq;
};

/* THE CONNECTION, named and passed.
 *
 * As an anonymous `_Thread_local struct` called `C`, reached into by name
 * from every function below, "the connection this thread last handshook" is a
 * fact no signature states and no caller can choose. A header can only say so
 * in prose ("Read and write the connection this thread
 * last handshook"), which is the shape of documentation that exists because
 * the types do not say it.
 *
 * The STORAGE is still one per worker and reused across connections: this
 * runs on a 56 MB board and the struct is several kilobytes, so a malloc per
 * connection would be a real cost for no benefit. What changed is that the
 * pointer is now an argument -- tls.c's internals all name the connection
 * they work on, and the one remaining ambient lookup is tls_conn_slot(),
 * called once when a connection is accepted. */
struct tls_conn {
   int fd;
   uint8_t trans[TRANS_MAX]; /* handshake transcript, for the hashes */
   size_t trans_n;
   struct keys rd, wr;
   int encrypted;      /* records are protected from here on */
   uint8_t master[32]; /* master secret, for the ticket PSK */
   uint8_t res_master[32];
   /* THE APPLICATION TRAFFIC SECRETS, KEPT PAST THE HANDSHAKE.
    *
    * `rd`/`wr` above hold the key and IV in use; these hold the secret each
    * was expanded from, which is the only thing a KeyUpdate can be applied to
    * -- RFC 8446 7.2 derives the next secret from the current SECRET, not
    * from the key. Without them a KeyUpdate cannot be honoured at all. */
   uint8_t cts[32], sts[32]; /* client_/server_application_traffic_secret_N */
   unsigned key_updates;     /* bounded by KEY_UPDATE_MAX */
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
   int (*giveup)(void);
};

struct hello; /* defined below: the parsed ClientHello */

/* ---- THE RECORD LAYER (srv/tlsrec.c) ---------------------------------- */

/* Entropy or nothing: 1 on success. */
int tls_rec_rand(uint8_t *out, size_t n);
/* Append to the handshake transcript; 0 when it would not fit, which is
 * FATAL to the connection -- a transcript that silently stops growing hashes
 * to something both sides can still agree on while describing a handshake
 * neither of them had. */
int tls_rec_transcript(struct tls_conn *c, const uint8_t *p, size_t n);
void tls_rec_transcript_hash(struct tls_conn *c, uint8_t out[32]);
/* One record out: `type` is a REC_* above. 0 on success. */
int tls_rec_send(struct tls_conn *c, int type, const uint8_t *body, size_t n);
/* One record in, into c->plain. The record's type, or -1. */
int tls_rec_read(struct tls_conn *c, int (*giveup)(void));
/* ...and the same, skipping the change_cipher_spec records a middlebox-
 * compatible client sends (bounded by CCS_MAX). */
int tls_rec_next(struct tls_conn *c, int (*giveup)(void));
/* Expand one traffic secret into the key and IV a record is sealed with. */
int tls_rec_keys(const uint8_t secret[32], struct keys *k);
/* One HANDSHAKE message, framed and sent as a record. It lives with the
 * handshake (srv/tls.c) because the framing is the handshake's, and it is
 * shared because a NewSessionTicket is a handshake message the ticket half
 * sends after the handshake is over. */
/* One handshake message, already framed, across as many records as it takes.
 * `hdr` is the framing and `body` is a payload this never copies -- the
 * certificate chain, which is the one message that can exceed a record. See
 * srv/tls.c. */
int tls_rec_handshake_split(struct tls_conn *c, const uint8_t *hdr, size_t hn,
                            const uint8_t *body, size_t bn);

int tls_rec_handshake(struct tls_conn *c, int type, const uint8_t *body,
                      size_t n);

/* An alert, best-effort: the connection is already ending. */
void tls_rec_alert(struct tls_conn *c, int desc);
/* The socket, bounded by the deadline. 0 on success; the read one answers a
 * count and 0 for a clean EOF. */
int tls_rec_write_all(struct tls_conn *c, const uint8_t *p, size_t n);
int tls_rec_read_all(struct tls_conn *c, uint8_t *p, size_t n,
                     int (*giveup)(void));

/* ---- THE TICKETS (srv/tlstkt.c) --------------------------------------- */

/* Seal one resumption ticket around `psk`; returns its length. */
size_t tls_tkt_seal(const uint8_t psk[32], uint8_t *out);
/* Open one: 1 and the psk, or 0 -- expired, tampered with, or not ours. */
int tls_tkt_open(const uint8_t *t, size_t n, uint8_t psk[32]);
/* Does the client's binder prove it holds the PSK in the ticket it offered?
 * `pre`/`pren` are the ClientHello prefix the binder is computed over. */
int tls_tkt_binder_ok(const struct hello *h, const uint8_t *ch,
                      const uint8_t *pre, size_t pren, const uint8_t psk[32]);

/* WHY A FAILED TICKET IS TWO DIFFERENT ANSWERS.
 *
 * "No ticket is a slower next connection, not a broken one" is true of
 * exactly half the failures, which is why one status cannot carry both. A
 * derivation or a seal that refuses has put NOTHING on the wire: the
 * connection is untouched and the
 * only cost is a full handshake next time. A failed WRITE is not that. It is
 * a full_write that gave up part way -- a timeout, a peer that stopped
 * reading, a reset -- and what it leaves behind is a PARTIAL PROTECTED RECORD
 * in the stream. From the peer's side the record layer is finished: it is
 * waiting for the rest of a record that will never arrive, and every byte
 * that follows, including the whole of the response, is framed wrong and
 * undecryptable.
 *
 * Discarding that made tls_handshake return success, and https.c's prepare()
 * then handed the connection to http_handle_conn, which read the request and
 * RAN IT -- authenticating a session, writing a row, sending a mail -- for a
 * client that could not read a single byte of the answer, and which will
 * therefore retry the same request on a new connection. A request executed
 * twice because the reply was unreadable is the kind of "worked, apparently"
 * that only shows up as duplicated state.
 *
 * So the caller is told which happened, and only the write failure is
 * fatal. */
enum ticket_fate {
   TICKET_SENT = 0,
   TICKET_NOT_ISSUED,  /* nothing reached the wire: next time is just slower */
   TICKET_WRITE_FAILED /* bytes may be on the wire: the record layer is torn */
};

/* Send one NewSessionTicket after the handshake. */
enum ticket_fate tls_tkt_send(struct tls_conn *c);

/* ---- THE NUMBERS ON THE WIRE ---------------------------------------
 *
 * Shared because both halves of the module speak them: this file's handshake
 * WRITES them into records and srv/tlshello.c READS them out of a hello. Two
 * copies of this list is two things that can disagree about what 51 means. */
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
#define HS_KEY_UPDATE    24

/* Alerts we send. RFC 8446 6.2: everything but close_notify and
 * user_canceled is fatal, and a fatal alert ends the connection -- which is
 * the point of sending one at all. A peer that gets an alert knows the
 * connection is over and why; a peer that gets silence has to guess, and
 * guesses wrong in the direction of "the network glitched, retry". */
#define ALERT_FATAL             2
#define ALERT_UNEXPECTED_MSG    10
#define ALERT_ILLEGAL_PARAMETER 47

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

/* BIG-ENDIAN FIELD READERS, the mirror of the writers above. */
static inline unsigned get16(const uint8_t *p)
{
   return ((unsigned)p[0] << 8) | p[1];
}

static inline size_t get24(const uint8_t *p)
{
   return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2];
}

/* ---- THE CLIENTHELLO, AND THE PARSER THAT FILLS IT -------------------
 *
 * srv/tlshello.c reads it; the handshake in srv/tls.c acts on it. Everything
 * in it POINTS INTO the caller's buffer rather than copying: the binder is
 * computed over the original bytes, so a copy would be a second version of
 * the message that could differ from the one that was authenticated. The
 * buffer must outlive the struct.
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
   /* WHERE THE BINDER LIST BEGINS -- its own 2-byte length field, which is
    * the point the ClientHello is truncated at for the MAC. Only the offset
    * matters, so no length travels with it: how far the list runs was settled
    * while it was framed, and a second copy of that number here would be a
    * second thing that can disagree with the first. */
   const uint8_t *binders;
   /* THE SELECTED IDENTITY'S BINDER, located while the list was framed.
    *
    * binder_ok finding this itself, as `h->binders[2]` and `h->binders + 3`
    * -- the first entry of a list it re-reads and has already been told is
    * well formed -- is where "accepts a prefix" comes from:
    * the list's own length was never required to account for every identity,
    * so a binder list holding ONE entry authenticated an offer carrying five,
    * and the four unauthenticated identities were as much a part of the
    * ClientHello as the one that was checked. The framing is settled once,
    * here, and the verifier is handed the entry it is to check. */
   const uint8_t *binder;
   size_t binder_n;
   int psk_dhe;
};

/* 1 when `b`/`n` is a well-formed ClientHello and `h` describes it, 0 when it
 * is not -- and NOTHING in `h` may be read after a 0. See tlshello.c: every
 * length-prefixed vector must be exhausted, so "well formed" here is a much
 * stronger statement than "long enough". */
int parse_hello(const uint8_t *b, size_t n, struct hello *h);

#endif
