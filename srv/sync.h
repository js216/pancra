/* SPDX-License-Identifier: GPL-3.0
 * sync.h --- the multi-user replica server: shared declarations
 * Copyright 2026 Jakob Kastelic
 *
 * `sync` is the third program in this repo and the only one that knows about
 * users. The single-user viewer this replaced served ONE person's text files
 * with no notion of who is asking; sync holds many people's records in one
 * sqlite file, takes them from a paired phone over an authenticated channel,
 * and shows them behind a login. It runs on its OWN port and touches neither
 * data.txt nor units.txt, so the old pair keeps running untouched until this
 * one has earned the swap.
 *
 * THIS HEADER IS THE WIRE PROTOCOL. There is no separate specification
 * document. The app implements the same protocol from its own independent
 * source (app/sync.h, app/sync.c), so everything the two must agree on --
 * the bounds below, the canonical bucket text in logs.c, the pairing packets
 * in pair.c, the signing string in auth.c -- lives in two places by design
 * and must be changed in both halves in the same edit. When you touch any of
 * them, open the app's copy in the other window: these constants and those
 * are the only record that the two sides agree.
 */
#ifndef SYNC_H
#define SYNC_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- bounds (mirrored in app/sync.h) --------------------------------- */
#define ROW_MAX     512          /* bytes in one row */
#define BUCKET_ROWS 20000        /* rows in one bucket */
#define LOG_BUCKETS 20000        /* buckets in one log */
#define USER_LOGS   64           /* logs one user may have */
#define USER_ROWS   4000000L     /* rows one user may hold */
#define BODY_MAX    (512 * 1024) /* request body ceiling */
#define LOGNAME_MAX 32

/* ---- pairing (the app half is app/sync.c sync_pair) ------------------ */
#define PAIR_CODE_LEN 6
#define PAIR_CODE_TTL 600 /* seconds a displayed code stays valid */
#define PAIR_TRIES    3   /* wrong attempts before the code is burned */
#define PAIR_SESS_TTL 120 /* seconds one in-flight attempt may take */
#define PAIR_PKT      160 /* bytes in a J-PAKE round packet */

/* ---- request signing (the app half is app/sync.c) -------------------- */
#define SIG_SKEW  300 /* seconds a signed request may be off by */
#define NONCE_MIN 8
#define NONCE_MAX 64

/* ---- web sessions ---------------------------------------------------- */
#define SESS_TTL      (365 * 24 * 3600L) /* the "type it once" cookie */
#define SESS_BUMP     (24 * 3600L)       /* refresh the expiry at most daily */
#define SELECTOR_HEX  16 /* chars: the indexed half of the cookie */
#define VALIDATOR_HEX 32 /* chars: the half only its hash is kept */

/* ---- sharing --------------------------------------------------------- */
#define MAX_FOLLOWERS 10
#define TOKEN_HEX     32
/* What `sync invite` prefixes a token with. The server cannot discover its own
 * public name -- nothing in a request is trustworthy enough to build a link
 * people click -- so it is a build-time default, overridable at run time with
 * SYNC_BASE_URL for anyone running their own instance. */
#define BASE_URL_DEFAULT "https://pancra.org"
#define TOKEN_TTL        (14 * 24 * 3600L)

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
#define PW_SALT_LEN      16
#define PW_HASH_LEN      32
/* Failed logins are throttled, because PBKDF2 on one core is exactly what an
 * attacker would like to make the server do. */
#define LOGIN_FAIL_MAX 5
#define LOGIN_FAIL_WIN 300

/* ---- one parsed request ---------------------------------------------- */
struct req {
   int fd;
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
   char method[8];
   char target[640]; /* path?query exactly as sent -- what the MAC covers */
   char path[640];   /* percent-decoded, no query (decoding only shrinks) */
   char *hdr;        /* the header block, NUL-terminated at the blank line */
   char *body;
   size_t body_len;
   long uid; /* authenticated user, or 0 */
   /* "?who=<id>" when this request is viewing SOMEONE ELSE'S record, else "".
    * Every link that must stay on that record appends it. Without it a
    * follower lost the record at the first click: /units and /plots resolved
    * to the viewer's own (empty) record and showed nothing. */
   char who[32]; /* fits "?who=" + any long */
};

/* ---- plots.c ---------------------------------------------------------- */
/* Serve one plot as a GIF: the window (win_start, win_end] over `hours`,
 * rendered by pancra's own plot.c so the web plot cannot drift from the
 * app's. */
void h_plot_gif(struct req *r, long owner, long win_start, long win_end,
                int hours, int tz_min);
/* Days (UTC-day bucket numbers) that hold readings, newest first. */
int plot_days(long owner, long *out, int cap);

/* ---- web.c ----------------------------------------------------------- */
void web_route(struct req *r);
/* Shared by the web pages: the signed-in user, or 0. */
long web_user(struct req *r, char *cookie, size_t cap);
void redirect(struct req *r, const char *to, const char *set_cookie);
/* The one answer for "the database said no": a 500 with nothing leaked. */
void oops(struct req *r);
void page(struct req *r, int code, const char *reason, const char *title,
          const char *body_html);

#endif
