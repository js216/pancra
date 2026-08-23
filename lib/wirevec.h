/* SPDX-License-Identifier: GPL-3.0
 * wirevec.h --- THE SYNC PROTOCOL'S GOLDEN VECTORS
 * Copyright 2026 Jakob Kastelic
 *
 * TWO IMPLEMENTATIONS, ONE WIRE.
 *
 * The phone (app/sync.c) and the server (srv/logs.c, srv/auth.c) implement
 * this protocol independently and on purpose: neither is a client library
 * generated from the other, and each is written against what the wire says
 * rather than against the other's code. That is a deliberate cost, and the
 * thing it buys -- an error on one side does not automatically become the
 * definition of correct -- is only real if something PINS the wire itself.
 *
 * THIS FILE IS THAT PIN. Without it, "these constants and the app's are the
 * only record that the two sides agree" is the whole guarantee, with one
 * Makefile grep comparing a single number as the only mechanical check --
 * and the canonical bytes of a bucket, the exact signing string and what a
 * hash is taken over are agreement by inspection, while the two
 * implementations pass
 * end-to-end tests against EACH OTHER, which cannot notice that both drifted
 * the same way.
 *
 * These vectors are that record. They are INPUTS AND EXPECTED BYTES, computed
 * once from the specification below, and they are PERMANENT: a change to any
 * of them is a change to the protocol, which means every phone in the field
 * stops agreeing with the server. Do not "fix" a vector to make a test pass.
 *
 * ---- THE CONTRACT -----------------------------------------------------
 *
 * ROUTES (the app calls, the server answers):
 *   GET  /v1/digest                one line per log: "<log> <rows> <hash>"
 *   GET  /v1/digest/<log>          one line per bucket: "<bucket> <rows>
 * <hash>" GET  /v1/bucket/<log>/<n>      the canonical text of that bucket PUT
 * /v1/bucket/<log>/<n>      REPLACES that bucket with the body POST /v1/pair/1
 * .. /v1/pair/4  the J-PAKE exchange (see srv/pair.c)
 *
 * A BUCKET IS A SET OF ROWS, and its CANONICAL TEXT is:
 *   - each row is one line, its bytes unmodified (no trimming: a trimmed
 *     trailing space would be a row whose hash the other side cannot
 *     reproduce);
 *   - rows sorted BYTEWISE ascending;
 *   - duplicates removed -- the same row twice is the same fact twice;
 *   - every line terminated with '\n', including the last.
 *
 * THE HASH of a bucket is the first 16 hex characters (lower case) of the
 * SHA-256 of that canonical text. 64 bits is ample: this compares two copies
 * of one honest data set.
 *
 * A SIGNED REQUEST carries
 *   Authorization: Pancra <uid>:<ts>:<nonce>:<mac>
 * where <mac> is HMAC-SHA256, hex, lower case, over exactly
 *   <METHOD> LF <PATH> LF <TS> LF <NONCE> LF <SHA256-HEX-OF-BODY>
 * with no trailing newline. <PATH> is the request target as sent, <TS> is
 * decimal seconds, and the body hash is over the body's bytes -- of the EMPTY
 * body for a GET, which is the well-known e3b0... digest below.
 *
 * ROUTE GRAMMAR. A route is the WHOLE path, not a prefix of it. The bucket
 * routes are exactly "/v1/bucket/<log>/<n>" with <log> non-empty and at most
 * LOGNAME_MAX bytes and <n> a complete decimal number; the pairing routes are
 * exactly "/v1/pair/1".."/v1/pair/4". A NUMBER IS DIGITS AND NOTHING ELSE --
 * no leading space, no sign, no leading zero -- and decoration at either end
 * is not a spelling of the route: "/v1/pair/1junk", "/v1/pair/1/2",
 * "/v1/pair/ 1", "/v1/pair/+1", "/v1/pair/01" and "/v1/bucket/g/5x" are none
 * of them routes. This matters most on the pairing rounds, which are the only
 * routes reached WITHOUT a signature: a route that accepts decoration is a
 * route whose rate limits count something other than what they name.
 *
 * ERRORS the app must expect: 400 malformed (which includes a bucket or
 * digest route whose log name or number is unusable), 401 the signature did
 * not verify (also a replayed nonce, or a timestamp outside SIG_SKEW), 403
 * not yours, 404 NO SUCH ROUTE, 405 a known route with the wrong method, 409
 * a pairing conflict, 413 a body over BODY_MAX, 414 a request target longer
 * than the server will read, 500 the server could not answer, 501 a method it
 * does not implement, 507 the account is at USER_ROWS. A 2xx is the only
 * success.
 *
 * A LOG OR BUCKET THAT DOES NOT EXIST IS NOT AN ERROR: it is 200 with an
 * EMPTY body, and the app relies on that -- the first sync of a new phone
 * asks for every bucket it holds before the server has any of them. 404 is
 * about the ROUTE, never about the data.
 *
 * WIRE LIMITS ARE NOT CAPACITIES, and the two were being read as one thing.
 * A wire limit is what the PROTOCOL allows and what the server therefore has
 * to be able to refuse: ROW_MAX bytes in a row, BODY_MAX bytes in a body,
 * LOGNAME_MAX bytes in a log name, BUCKET_ROWS rows in a bucket, USER_LOGS
 * logs in an account. A capacity is what ONE implementation chooses to hold:
 * the phone builds at most SYNC_BUF_MAX bytes of bucket text and syncs at
 * most SYNC_MAX_LOGS logs, both far below the wire's ceiling, because it is a
 * phone. The asymmetry is DELIBERATE and legal in one direction only:
 *
 *   - an implementation may hold LESS than the wire allows, and must then
 *     decline its own request rather than truncate one;
 *   - no implementation may SEND more than the wire allows;
 *   - a limit may only be RAISED by a new route, never in place: a phone in
 *     the field goes on refusing what its own build refuses.
 *
 * The mirrored WV_LIMIT_* values below are the wire's half, and both
 * srv/proto.h and app/sync.h are checked against them at compile time.
 *
 * COMPATIBILITY: a row may GROW trailing fields (see srv/rowdec.h), so a
 * reader must not refuse a row for having more than it knows. Nothing else
 * about these bytes may change without a new route.
 */
#ifndef WIREVEC_H
#define WIREVEC_H

#include <stdint.h> /* uint8_t: the pairing key is bytes, not text */

/* ---- vector A: a bucket, in ARRIVAL order --------------------------------
 *
 * As the phone's append-only log really holds it: out of order (a backfilled
 * row lands after a newer one), with one row repeated, and with a fingerstick
 * whose RSSI field is empty.
 *
 * ALL FIVE ARE ONE UTC DAY -- bucket 20000, which begins at 1728000000. A
 * bucket is a day, so a vector that straddled midnight would be describing
 * two of them and the canonical text would be neither. */
#define WV_A_BUCKET 20000L
#define WV_A_NROWS  5
static const char *const wv_a_arrival[WV_A_NROWS] = {
    "1728000900,118,0,-70,3,7,1728000900,-420,0",
    "1728000000,120,0,-70,3,7,1728000000,-420,0", /* backfilled: it arrived
                                                   * after a newer row */
    "1728000900,118,0,-70,3,7,1728000900,-420,0", /* the same row twice */
    "1728000300,131,-1,-70,3,7,1728000300,-420,0",
    "1728000600,95,2,,0,9,1728000600,-420,1", /* a meter row: no RSSI */
};

/* ...and the canonical text those rows MUST produce: sorted, deduplicated,
 * every line newline-terminated. Four rows, because two of the five are the
 * same row. */
#define WV_A_NCANON 4
static const char wv_a_canon[] = "1728000000,120,0,-70,3,7,1728000000,-420,0\n"
                                 "1728000300,131,-1,-70,3,7,1728000300,-420,0\n"
                                 "1728000600,95,2,,0,9,1728000600,-420,1\n"
                                 "1728000900,118,0,-70,3,7,1728000900,-420,0\n";

/* The protocol's hash of that text: first 16 hex of its SHA-256. */
static const char wv_a_hash16[] = "f5a2d8ab476d3f3e";

/* ---- vector B: a signed request -----------------------------------------
 *
 * A GET, so the body is empty and its hash is the well-known digest of zero
 * bytes -- the case every request that is not a bucket PUT takes. */
static const unsigned char wv_b_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                           0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                           0x0c, 0x0d, 0x0e, 0x0f};
static const char wv_b_method[]         = "GET";
static const char wv_b_path[]           = "/v1/digest";
#define WV_B_TS 1728000000L
static const char wv_b_nonce[] = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
static const char wv_b_body[]  = "";
static const char wv_b_bodyhash[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
/* The bytes the MAC is taken over, spelled out so a reader can see there is
 * no trailing newline and no separator but LF. */
static const char wv_b_signing[] =
    "GET\n"
    "/v1/digest\n"
    "1728000000\n"
    "0f1e2d3c4b5a69788796a5b4c3d2e1f0\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static const char wv_b_mac[] =
    "8b2e04d022e785f23bacf8e50f3bf6b9443ce5d589720ac94d19a6e6e7216c4e";

/* ---- vector C: a signed PUT, with a body --------------------------------
 *
 * The other half of the signing rule, and the half that actually carries
 * data: a bucket PUT. Vector B alone could be satisfied by an implementation
 * that hashed the EMPTY STRING unconditionally -- the well-known e3b0...
 * digest is what a GET wants and what a bug produces -- so the body hash was
 * pinned by a vector that could not tell the two apart. This one's body is
 * vector A's canonical text, so a signature computed over "no body", over the
 * body's LENGTH, or over the arrival-order rows rather than the canonical ones
 * all give a different MAC.
 *
 * Same key as vector B, different nonce: a nonce is never reused, and the
 * vectors should not be the one place that looks as though it may be. */
static const char wv_c_method[] = "PUT";
static const char wv_c_path[]   = "/v1/bucket/glucose/20000";
#define WV_C_TS 1728000000L
static const char wv_c_nonce[] = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
/* The body IS wv_a_canon; spelled as a pointer so the two cannot drift. */
static const char *const wv_c_body = wv_a_canon;
static const char wv_c_bodyhash[] =
    "f5a2d8ab476d3f3efa3f3e357daf745d0c1b97fac34843e7c33f3126b1e04d47";
static const char wv_c_signing[] = "PUT\n"
                                   "/v1/bucket/glucose/20000\n"
                                   "1728000000\n"
                                   "a1b2c3d4e5f60718293a4b5c6d7e8f90\n"
                                   "f5a2d8ab476d3f3efa3f3e357daf745d0c1b97fac"
                                   "34843e7c33f3126b1e04d47";
static const char wv_c_mac[] =
    "edde567e4e8c3c3024883b0e03e44fa82325558f7a89d457e8b470d533078edd";
/* THE BUCKET HASH IS NOT THE BODY HASH. Both are SHA-256 of the same bytes,
 * but one is truncated to 16 hex characters and the other is not, and a
 * reader who assumes they are unrelated numbers will not notice an
 * implementation that sends the wrong one. wv_c_bodyhash begins with
 * wv_a_hash16, and wiretest asserts exactly that. */

/* ---- vector D: pairing, the parts that are not random -------------------
 *
 * A J-PAKE round packet cannot be a vector: it is built from a fresh random
 * scalar, so the two sides never see the same 160 bytes twice and nothing
 * about them can be pinned. What CAN be pinned, and is what the two
 * implementations actually have to agree on, is everything AROUND the
 * packets:
 *
 *   - the FRAMING of each round's body and reply,
 *   - the KEY CONFIRMATION tags, which are the whole point of rounds 3 and 4:
 *     they are what proves the shared key really is shared, and they are a
 *     plain HMAC over a fixed label, so they are exactly reproducible.
 *
 * If either side truncated the tag differently, upper-cased its hex, or
 * swapped the two labels, pairing would still SUCCEED against a partner that
 * made the same mistake -- which is the failure mode these vectors exist for.
 *
 * FRAMING (all bodies and replies are text, LF-separated, trailing LF):
 *   -> POST /v1/pair/1   "<email>\n<320 hex of the round-1 packet>\n"
 *   <- 200               "<session>\n<320 hex of the server's packet>\n"
 *   -> POST /v1/pair/2   "<session>\n<320 hex>\n"
 *   <- 200               "<320 hex>\n"
 *   -> POST /v1/pair/3   "<session>\n<320 hex>\n"
 *   <- 200               "<320 hex>\n<32 hex server confirm>\n"
 *   -> POST /v1/pair/4   "<session>\n<32 hex client confirm>\n"
 *   <- 200               "<uid>\n"
 *
 * A packet is PAIR_PKT (160) bytes, so its hex is 320 characters, LOWER CASE.
 * The client verifies the SERVER's tag before it stores anything; the server
 * pairs only after the client's tag verifies. */
static const uint8_t wv_d_key[16]     = {0x9a, 0x3f, 0x21, 0x08, 0xbc, 0x4d,
                                         0x5e, 0x6f, 0x70, 0x81, 0x92, 0xa3,
                                         0xb4, 0xc5, 0xd6, 0xe7};
static const char wv_d_label_server[] = "pancra-confirm-server";
static const char wv_d_label_client[] = "pancra-confirm-client";
/* First 32 hex characters of HMAC-SHA256(key, label). Half of the tag, which
 * is 128 bits of a value an attacker gets ONE guess at. */
static const char wv_d_confirm_server[] = "e9171d82d12a7da2edbbdbed81bee1f4";
static const char wv_d_confirm_client[] = "163fb773d599b806ceae09587a38cfe4";
#define WV_D_CONFIRM_HEX 32
#define WV_D_PKT         160 /* bytes; 320 hex characters on the wire */

/* ---- vector E: the route grammar, and what is NOT a route ---------------
 *
 * Each entry is a request the app could send and the status the server owes
 * it. `signed_req` says whether the request carries a valid signature: the
 * pairing rounds are the only routes reached without one, so an unsigned
 * request to anything else is 401 BEFORE its path is ever considered -- which
 * is itself part of the contract and is pinned here.
 *
 * The rejections are the point. Every one of them was, at some time, a path
 * that the server accepted as a decorated spelling of a real route. */
struct wv_route {
   const char *method;
   const char *path;
   int is_signed; /* the request carries a valid Authorization */
   int status;    /* what the server must answer */
   const char *why;
};

#define WV_E_NROUTES 23
static const struct wv_route wv_e_routes[WV_E_NROUTES] = {
    /* the four real routes, signed, against a paired user with no data */
    {"GET",    "/v1/digest",                1, 200, "the digest of every log"             },
    {"GET",    "/v1/digest/glucose",        1, 200, "the digest of one log"               },
    {"GET",    "/v1/bucket/glucose/20000",  1, 200, "one bucket, canonical text"          },
    {"PUT",    "/v1/bucket/glucose/20000",  1, 200, "replace one bucket"                  },
    /* no signature: refused before the path matters */
    {"GET",    "/v1/digest",                0, 401, "an unsigned request is not read"     },
    {"GET",    "/v1/bucket/glucose/20000",  0, 401, "...on any route but pairing"         },
    {"GET",    "/v1/nonesuch",              0, 401, "...including one that does not exist"},
    /* the path is not a prefix */
    {"GET",    "/v1/digestx",               1, 404, "a longer word is a different route"  },
    {"GET",    "/v1/bucket/glucose",        1, 400, "a bucket route needs a number"       },
    {"GET",    "/v1/bucket/glucose/",       1, 400, "...an actual one"                    },
    {"GET",    "/v1/bucket/glucose/20000x", 1, 400, "...and the whole of it"              },
    {"GET",    "/v1/bucket//20000",         1, 400, "...and a log name that is there"     },
    {"GET",    "/v1/digest/",               1, 400, "a per-log digest needs a name"       },
    {"DELETE", "/v1/bucket/glucose/20000",  1, 405, "a bucket is GET or PUT"              },
    {"PUT",    "/v1/digest",                1, 405, "a digest is GET, and says so"        },
    /* A LOG OR BUCKET THAT DOES NOT EXIST IS NOT AN ERROR: 200, empty. The
     * app's first sync asks for both before the server holds either. */
    {"GET",    "/v1/digest/nosuchlog",      1, 200, "an unknown log is empty, not 404"    },
    {"GET",    "/v1/bucket/glucose/1",      1, 200, "...and so is an unwritten bucket"    },
    /* pairing rounds: unsigned by definition, and exactly four of them */
    {"POST",   "/v1/pair/0",                0, 404, "there is no round zero"              },
    {"POST",   "/v1/pair/5",                0, 404, "nor a fifth"                         },
    {"POST",   "/v1/pair/1junk",            0, 404, "and a decorated round is not one"    },
    /* ...nor is a round wearing anything strtol would have skipped.
     *
     * THE SPACE IS PERCENT-ENCODED BECAUSE A RAW ONE CANNOT TRAVEL. A request
     * target with a literal space in it is not a request line at all (RFC 9112
     * 3: no whitespace is permitted inside the three components, and a
     * recipient MUST reject a message that has it), so this server refuses
     * such a line with 400 before any route is consulted. A vector of
     * "/v1/pair/ 1" expecting 404 is reachable only by a parser that splits
     * on the FIRST space and silently drops the rest, which is the defect the
     * 400 exists to remove. "%20" is
     * how a client that means a space actually sends one, and it still is not
     * a round: the server percent-decodes the path and the route grammar
     * refuses " 1" exactly as before. */
    {"POST",   "/v1/pair/%201",             0, 404, "a leading space is not round 1"      },
    {"POST",   "/v1/pair/+1",               0, 404, "nor a sign"                          },
    {"POST",   "/v1/pair/01",               0, 404, "nor a leading zero"                  },
};

/* ---- vector F: the wire's limits, mirrored -------------------------------
 *
 * The PROTOCOL's numbers, not either implementation's. srv/proto.h and
 * app/sync.h each _Static_assert against these, so a limit cannot be changed
 * on one side alone. Duplicated constants are not a record of an agreement;
 * they are two guesses that happen to match.
 *
 * A capacity (the app's SYNC_BUF_MAX, SYNC_MAX_LOGS) is NOT here on purpose:
 * it is allowed to differ, and it is asserted to be no LARGER than the wire
 * allows rather than equal to it. */
#define WV_LIMIT_ROW_MAX     512
#define WV_LIMIT_BUCKET_ROWS 20000
#define WV_LIMIT_LOG_BUCKETS 20000
#define WV_LIMIT_USER_LOGS   64
/* LONG, like every other number on this wire (see the LP64 assertions in
 * srv/proto.h and app/sync.h). The app compares its own SYNC_BUF_MAX --
 * already a long, because it is measured against file offsets -- against
 * this, and an int multiplication silently widened into that comparison is
 * the shape that overflows the day somebody raises the ceiling. */
#define WV_LIMIT_BODY_MAX    (512L * 1024)
#define WV_LIMIT_LOGNAME_MAX 32
#define WV_LIMIT_PAIR_PKT    160
#define WV_LIMIT_PAIR_CODE   6
#define WV_LIMIT_SIG_SKEW    300
#define WV_LIMIT_NONCE_MIN   8
#define WV_LIMIT_NONCE_MAX   64
/* Rows ONE ACCOUNT may hold. Enforced by the server (a push past it is 507),
 * and here because it is a wire limit like the rest -- it was the one the
 * server refuses on that this block did not name. */
#define WV_LIMIT_USER_ROWS 4000000L

#endif
