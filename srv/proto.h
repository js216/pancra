/* SPDX-License-Identifier: GPL-3.0
 * proto.h --- the wire protocol, and the request every handler is given
 * Copyright 2026 Jakob Kastelic
 *
 * `sync` is the third program in this repo and the only one that knows about
 * users. It holds many people's records in one sqlite file, takes them from
 * a paired phone over an authenticated channel, and shows them behind a
 * login. It runs on its OWN port and touches neither data.txt nor units.txt,
 * so the single-user viewer beside it keeps running untouched.
 *
 * THE SPECIFICATION IS lib/wirevec.h, and it is EXECUTABLE. This header is
 * the server's half of the protocol; the app implements the same protocol
 * from its own independent source (app/sync.h, app/sync.c), and neither is
 * generated from the other. That independence is the whole point -- a bug on
 * one side does not become the definition of correct -- and it is only worth
 * anything if something outside both sides pins the wire.
 *
 * wirevec.h is that something: the canonical bucket text and its hash, the
 * exact bytes a signature is taken over for a GET and for a PUT, the pairing
 * framing and its key-confirmation tags, the route grammar with the paths
 * that are NOT routes, and the wire's limits.
 *
 * AND BOTH IMPLEMENTATIONS CONSUME IT THROUGH THE CODE THAT SHIPS, which is
 * the part that is easy to get wrong: a vector is worth something only when it
 * is fed to the server's own route grammar (route.c) and signing string
 * (sigstr.c), and to the app's own canonicaliser, path builders and
 * confirmation tags. Checked against a copy of the rule written out beside the
 * vector instead, it cannot notice a change made to BOTH implementations at
 * once -- which is the only drift two independent implementations cannot catch
 * by themselves.
 *
 * The limits below are _Static_asserted against it, so a bound cannot be
 * raised here while a phone in the field goes on refusing what its own build
 * refuses.
 *
 * A vector is PERMANENT. Changing one is changing the protocol, which every
 * phone already in the field disagrees with. Do not edit a vector to make a
 * test pass.
 */
#ifndef PANCRA_PROTO_H
#define PANCRA_PROTO_H

/* A POINTER ONLY, so this header includes nothing of ours and everything may
 * include it. It was sync.h, which the whole server included for the bounds
 * below -- and which shared a node with sync.c, the dispatcher that includes
 * the database, the log store, the pairing and the auth. Every module
 * therefore appeared to depend on the program's main loop, and the graph had
 * a ring through all of them. The protocol is not the program. */
struct http_conn;

#include "pairtag.h" /* the key-confirmation tag: ONE construction */
#include "wireint.h" /* int64_t and PRIwire: the wire's scalars, exactly */
#include "wirevec.h" /* WV_LIMIT_*: the wire's own numbers, pinned */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- THE MACHINE THIS PROTOCOL ASSUMES: NONE ------------------------
 *
 * ASSUMING LP64 -- every id, timestamp and bucket a C `long`, printed and
 * parsed with %ld, with a _Static_assert(sizeof(long) >= 8) on each half to
 * make that safe by REFUSING TO BUILD anywhere else -- is sound reasoning to
 * a backwards conclusion. The wire is decimal text: it carries no widths, no
 * ABI and no data model, and the one thing an interchange format must not
 * depend on is a property of the two machines exchanging it. Such an
 * assertion buys a guarantee that a 32-bit build cannot be wrong, at the
 * price of never being able to exist -- and the failure it defends against (a
 * timestamp that stops being representable in 2038, with the code still
 * compiling and the tests still passing) is a consequence of the TYPE, not of
 * the machine.
 *
 * So these domains are int64_t, printed and parsed with PRIwire/SCNwire from
 * lib/wireint.h, which both halves include -- the app cannot use
 * <inttypes.h> at all, being freestanding, and one spelling shared by two
 * independent implementations is the point of lib/wirevec.h.
 *
 * THE WIRE AND THE FILES ARE UNAFFECTED: the text is decimal digits either
 * way, so every stored row, every signed request and every phone in the field
 * parses identically. This is a statement about C types, and the format has
 * no opinion about those.
 *
 * A leftover %ld is only VISIBLE when the protocol units are compiled for a
 * data model this program does not run on (an ILP32 or an LLP64 one): on LP64,
 * %ld and PRIwire are the same three characters and no diagnostic is
 * possible.
 *
 * The Android artifact is arm64-v8a ALONE -- the only ABI this is built and
 * tested for -- but that is a packaging decision rather than a correctness
 * one. */
_Static_assert(sizeof(time_t) >= 8,
               "a 32-bit time_t is the 2038 problem, and this program stores "
               "instants -- note this is about the SERVER'S clock, not the "
               "wire: what travels is int64_t (lib/wireint.h)");
_Static_assert((char)-1 < 0 || 1,
               "signedness of char is not assumed anywhere here");

/* ---- bounds (PINNED by lib/wirevec.h, mirrored in app/sync.h) -------- */
#define ROW_MAX     512          /* bytes in one row */
#define BUCKET_ROWS 20000        /* rows in one bucket */
#define LOG_BUCKETS 20000        /* buckets in one log */
#define USER_LOGS   64           /* logs one user may have */
#define USER_ROWS   4000000L     /* rows one user may hold */
#define BODY_MAX    (512 * 1024) /* request body ceiling */
#define LOGNAME_MAX 32

/* THESE NUMBERS ARE THE WIRE'S, and the wire is written down. Each is checked
 * against lib/wirevec.h, which the app is checked against too -- so a limit
 * can no longer be raised on one side while the other goes on refusing what
 * it was built to refuse. Raising one for real means a new route (see the
 * asymmetry rules in wirevec.h), not a larger number here. */
_Static_assert(ROW_MAX == WV_LIMIT_ROW_MAX, "the wire's row ceiling moved");
_Static_assert(BUCKET_ROWS == WV_LIMIT_BUCKET_ROWS, "bucket rows moved");
_Static_assert(LOG_BUCKETS == WV_LIMIT_LOG_BUCKETS, "log buckets moved");
_Static_assert(USER_LOGS == WV_LIMIT_USER_LOGS, "logs per user moved");
_Static_assert(BODY_MAX == WV_LIMIT_BODY_MAX, "the body ceiling moved");
_Static_assert(LOGNAME_MAX == WV_LIMIT_LOGNAME_MAX, "log names moved");
_Static_assert(USER_ROWS == WV_LIMIT_USER_ROWS, "rows per account moved");

/* ---- pairing (the app half is app/sync.c sync_pair) ------------------ */
#define PAIR_CODE_LEN 6
#define PAIR_CODE_TTL 600 /* seconds a displayed code stays valid */
#define PAIR_TRIES    3   /* wrong attempts before the code is burned */
#define PAIR_SESS_TTL 120 /* seconds one in-flight attempt may take */
#define PAIR_PKT      160 /* bytes in a J-PAKE round packet */
/* THE KEY-CONFIRMATION TAG: 32 hex characters, half of an HMAC-SHA256 over a
 * fixed label. It is a protocol constant, not pair.c's private choice --
 * truncating differently on one side makes pairing fail against every phone
 * and succeed against a partner with the same bug, which is what the vectors
 * in lib/wirevec.h exist to catch. */
/* THE LENGTH AND THE LABELS ARE lib/pairtag.h's, and so is the
 * construction. This side and the app's had a copy each -- four places for
 * one rule -- and the copies checked nothing about each other; what checks
 * them is lib/wirevec.h, which both sides compare against. The names below
 * stay because this header is what the server's own code reads. */
#define CONFIRM_HEX          PAIR_TAG_HEX
#define CONFIRM_LABEL_SERVER PAIR_TAG_LABEL_SERVER
#define CONFIRM_LABEL_CLIENT PAIR_TAG_LABEL_CLIENT
_Static_assert(PAIR_PKT == WV_LIMIT_PAIR_PKT, "the pairing packet moved");
_Static_assert(CONFIRM_HEX == WV_D_CONFIRM_HEX,
               "the key-confirmation tag changed length");
_Static_assert(PAIR_CODE_LEN == WV_LIMIT_PAIR_CODE, "the pairing code moved");

/* ---- request signing (the app half is app/sync.c) -------------------- */
#define SIG_SKEW  300 /* seconds a signed request may be off by */
#define NONCE_MIN 8
#define NONCE_MAX 64
_Static_assert(SIG_SKEW == WV_LIMIT_SIG_SKEW, "the signing skew moved");
_Static_assert(NONCE_MIN == WV_LIMIT_NONCE_MIN, "the nonce floor moved");
_Static_assert(NONCE_MAX == WV_LIMIT_NONCE_MAX, "the nonce ceiling moved");

/* ---- web sessions ---------------------------------------------------- */
#define SESS_TTL      (365 * 24 * 3600L) /* the "type it once" cookie */
#define SESS_BUMP     (24 * 3600L)       /* refresh the expiry at most daily */
#define SELECTOR_HEX  16 /* chars: the indexed half of the cookie */
#define VALIDATOR_HEX 32 /* chars: the half only its hash is kept */
/* HOW MANY LIVE COOKIES ONE ACCOUNT MAY HOLD AT ONCE, and why this number.
 *
 * A session row is a bearer credential that lasts a YEAR, and the only
 * thing that removes one is the browser that holds it presenting it again --
 * so signing in from a machine you never open again would leave a live
 * credential behind for twelve months, invisible to its owner. The cap is
 * what turns "sessions I have forgotten about" into a bounded set.
 *
 * EIGHT, because the thing being counted is COOKIES, not devices, and cookies
 * are minted far more often than devices are acquired. A realistic upper
 * bound on devices somebody actually signs in from is about five -- phone,
 * tablet, laptop, desktop, one machine at work -- and each of those can hold
 * more than one (Safari and Chrome on the same phone are two), while clearing
 * a cookie jar or using a private window mints a row without retiring the one
 * it stands in for. Five is therefore too low: it would sign people out of
 * devices they are still using, which is the failure that makes a cap worse
 * than none. Much higher and it bounds nothing a year of ordinary use would
 * ever reach, which is a cap that exists only in the source.
 *
 * WHICH ONE GOES is decided by `last_seen`, so the row revoked is literally
 * the credential that has gone longest without being used -- the closest this
 * server can get to "the one you have forgotten about". */
#define MAX_SESSIONS 8

/* ---- sharing --------------------------------------------------------- */
#define MAX_FOLLOWERS 10
/* HOW MANY UNUSED INVITATION LINKS ONE OWNER MAY HAVE OUTSTANDING.
 *
 * Deliberately the SAME number as the follower cap, and defined in terms of
 * it so the two cannot drift apart. A live share link is a follower in
 * waiting: every one of them that gets redeemed becomes a `share` row, and
 * MAX_FOLLOWERS of those is all the account will ever accept. So more than
 * MAX_FOLLOWERS unredeemed links is, by construction, more invitations than
 * can ever be taken up -- there is no legitimate use for the eleventh.
 *
 * Only LIVE links count. Expired and redeemed rows are pruned in the same
 * transaction that does the counting (see share_token_mint), so the number
 * compared here is the number the settings page renders, and a link that
 * aged out two weeks ago cannot keep its owner from minting a new one. */
#define MAX_LIVE_TOKENS MAX_FOLLOWERS
#define TOKEN_HEX       32
/* (THE INVITATION LINK'S ORIGIN IS NOT HERE: one installation has one public
 * name, and it is public_origin() in srv/util.h, read from PANCRA_ORIGIN and
 * validated once at startup. Every link printed anywhere is built from it.) */
#define TOKEN_TTL (14 * 24 * 3600L)

/* Password hashing. PBKDF2-HMAC-SHA256 because the board has ~19 MB free:
 * argon2 at any honest memory cost does not fit, and the server is
 * served by a small pool, so an expensive KDF is also a denial-of-service
 * surface
 * against the app's own pushes. Iterations are stored PER USER so the cost
 * can be raised later without invalidating anyone's password. Calibrate with
 * `sync bench` on the target board: this default is ~250 ms on a Duo. */
/* Calibrated with `sync bench` ON THE BOARD: 24000 took 767 ms there, which
 * is three-quarters of a second in which a worker answers
 * nobody -- including the app trying to push. 8000 lands near 250 ms, which
 * is enough to make a stolen database expensive to attack and short enough
 * that a login does not stall the service. Stored per user, so raising it
 * later does not invalidate anyone's password. */
#define PW_ITERS_DEFAULT 8000
/* THE SUPPORTED RANGE, and a stored count outside it is a STORAGE FAULT.
 *
 * pw_iters is whatever integer the column holds, and it went straight into
 * PBKDF2 as an unsigned count. A corrupt row -- or a negative value, which
 * casts to something near four billion -- would put one worker into a loop
 * of billions of rounds answering nobody, on a board with one core and a
 * hundred and twenty-eight megabytes; and zero is not a hash at all, it is
 * the salt returned unchanged, so a "password check" against it would pass
 * for any password whose stored hash was made the same way.
 *
 * The ceiling is generous on purpose: it is not a policy about how slow a
 * login may be, it is the line past which the value cannot have been written
 * by this program. The floor is the smallest count anyone could deliberately
 * have configured, and it is still far above zero. */
/* ---- WHICH KDF MADE A STORED HASH ---------------------------
 *
 * The row carries this so a future algorithm can coexist with today's during
 * a rolling upgrade: each row is verified by ITS OWN function, and a
 * successful login re-derives it under the current one. Never renumber -- the
 * number is in every user row this server has ever written.
 *
 *   1  PBKDF2-HMAC-SHA256, pw_iters rounds, PW_SALT_LEN salt, PW_HASH_LEN out
 */
#define PW_KDF_PBKDF2_SHA256 1
#define PW_KDF_CURRENT       PW_KDF_PBKDF2_SHA256

#define PW_ITERS_MIN 1000
#define PW_ITERS_MAX 1000000
#define PW_SALT_LEN  16
#define PW_HASH_LEN  32
/* Failed logins are throttled, because PBKDF2 on one core is exactly what an
 * attacker would like to make the server do. */
#define LOGIN_FAIL_MAX 5
#define LOGIN_FAIL_WIN 300

/* ---- one parsed request ---------------------------------------------- */
struct db; /* db.h; a request names the database it is about */

struct req {
   /* WHICH DATABASE this request is about. Handed to the server at startup
    * and copied in here per request, so a handler cannot reach storage
    * nothing gave it. Held process-wide instead, there is no way for a handler
    * to name any database but the one that exists. */
   struct db *db;
   /* THE CONNECTION, not just its socket. A bare fd does not say whether the
    * bytes written to it need encrypting, and a process-wide global answers
    * it for every connection at once. */
   const struct http_conn *c;
   /* A RESPONSE BUILT UNDER THE PAGE LOCK AND SENT WITHOUT IT.
    *
    * web.c serialises page rendering behind one mutex because the pages share
    * large scratch buffers that a 56 MB board cannot afford per worker. That
    * is fine -- rendering is microseconds -- but the socket write was inside
    * the lock too, and a write goes at the peer's pace. One client that asks
    * for a page and stops reading therefore stalled EVERY page for EVERY
    * user, for as long as it cared to.
    *
    * So a page handler leaves its bytes here instead, and web_route sends
    * them after unlocking. resp is heap-owned and freed by that flush. */
   char *resp;
   size_t resp_n;
   int resp_code;
   const char *resp_reason;
   const char *resp_ctype;
   /* Extra header lines for this response (Location, Set-Cookie), already
    * CRLF-terminated. Inline rather than a pointer because a Set-Cookie is
    * built in a caller's local buffer and would not outlive it. */
   char resp_extra[256];
   char method[8];
   char target[640]; /* path?query exactly as sent -- what the MAC covers */
   char path[640];   /* percent-decoded, no query (decoding only shrinks) */
   char *hdr;        /* the header block, NUL-terminated at the blank line */
   char *body;
   size_t body_len;
   int64_t uid; /* authenticated user, or 0 */
   /* "?who=<id>" when this request is viewing SOMEONE ELSE'S record, else "".
    * Every link that must stay on that record appends it. Without it a
    * follower lost the record at the first click: /units and /plots resolved
    * to the viewer's own (empty) record and showed nothing. */
   char who[32]; /* fits "?who=" + any int64_t */
};

#endif
