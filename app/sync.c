// SPDX-License-Identifier: GPL-3.0
// sync.c --- Cloud sync client: digests, buckets, pairing
// Copyright 2026 Jakob Kastelic

/* See sync.h for the protocol. This file is the whole client except the
 * transport, which is a hook: the bytes on the wire are decided here, so a
 * second implementation can be built from sync.h without reading this code.
 *
 * The one thing that must match the server EXACTLY is the canonical text of a
 * bucket -- rows sorted bytewise ascending, each terminated by '\n'. Bytewise
 * means unsigned byte order, not any locale's idea of order, which is why the
 * comparison below casts to unsigned char rather than using strcmp on chars
 * that may be signed.
 */
#include "sync.h"
#include "syncint.h" /* the workspace synclocal.c shares */
#include "syncrow.h" /* struct row, row_ok/row_bucket/hash16: what a row IS */
#include "wireint.h" /* PRIwire: the wire's scalars, printed exactly */
/* dexlibc.h already resolves errno for both builds; naming it here as well is
 * for the reader and the linter, which both want the header that provides a
 * symbol to be visible at the point of use. */
#if __STDC_HOSTED__
#include <errno.h>
#endif
#include "sha256.h"
#include "thread.h"    /* the config lock and the operation lock */
#include <stdatomic.h> /* the progress counters cross a thread */
#include <stdint.h>    /* uint8_t: the key, the hashes, the raw bytes */
#include <stdio.h>     /* snprintf, SEEK_END */
#include <stdlib.h>    /* realloc: the log buffer grows to the file */
#include <string.h>    /* memcpy, memcmp, strcmp: rows are bytes, not strings */

/* struct sync_log is in app/syncint.h: app/synclocal.c reads the file it
 * names, and the two halves must agree about what one is. */

/* ---- THE CONFIGURATION, AND THE LOCK THAT OWNS IT --------------------
 *
 * The transport, the account identity and the log registry are set from the
 * MAIN thread (the settings screen, the pairing worker's success path) while
 * sync_run / sync_pair / sync_restore read them on Java's push worker. They
 * were plain globals read one field at a time, so an operation could sign
 * with a new account's key against the previous server, or walk a log
 * registry being rewritten under it -- and a registration spelled as a
 * clear() followed by eight add() calls is exactly that rewrite, nine
 * separate stores wide. It is one store (sync_set_logs); see sync.h for what
 * a snapshot
 * of a PREFIX of the registry did.
 *
 * Nothing here is read directly any more. An operation takes ONE snapshot
 * under this lock and works from that, so its configuration cannot change
 * underneath it however long it runs. */
static struct mutex g_cfg_lk = MUTEX_INIT;
static struct sync_log g_log[SYNC_MAX_LOGS];
static int g_nlog;
static sync_http_fn g_http;
static int64_t g_uid;
static uint8_t g_key[SYNC_KEY_LEN];
static int g_have_key;

/* The response scratch is fixed: a reply is a digest or one bucket, both
 * bounded by the protocol. */
static char g_rsp[SYNC_BUF_MAX];

/* The LOG buffer grows to whatever the file is.
 *
 * A fixed 256 kB is a log sized as though it were about as big as one
 * bucket. A real readings.csv is years of five-minute samples -- megabytes --
 * so log_rows answers "too big" and the whole sync aborts after the very
 * first digest, silently and forever. The file has to be held whole because a
 * bucket's rows are NOT contiguous in it: a meter syncing a day late puts
 * yesterday's row after today's, and the canonical form is a sort. */
/* The workspace is declared in app/syncint.h: app/synclocal.c is the other
 * half of this module and fills these. */
char *g_buf;
int64_t g_bufcap;
struct row *g_row;
int g_rowcap;
/* A second index, for picking one bucket's rows out of a loaded window
 * without disturbing the window's own index. */
struct row *g_sel;
int g_selcap;

/* ---- ONE OPERATION AT A TIME, AND WHAT IT OWNS -----------------------
 *
 * struct sync_ctx is a snapshot of the configuration plus the workspace the
 * running operation may use. It is opaque outside this file (sync.h forward-
 * declares it only), and nothing below reads a configuration global directly.
 *
 * The workspace -- the log window, its two row indexes and the response
 * buffer -- is not copied into the context; the context POINTS at the single
 * set, because only one operation may hold it. That is what g_op_lk
 * enforces. Reallocating scratch shared between two concurrent operations is
 * how a restore came to parse a run's half-loaded window: the buffers grow,
 * and a realloc under another thread's pointer is not a stale read, it is a
 * freed one.
 *
 * g_op_lk is taken ONLY by the three public operations, is never nested
 * inside g_cfg_lk (the snapshot is taken and released first), and is never
 * held across a call that takes another lock in this program. */
static struct mutex g_op_lk = MUTEX_INIT;

/* Take the operation lock and snapshot the configuration into `sx`.
 *
 * The two locks are taken in sequence, never nested: the snapshot is a copy,
 * so there is nothing to hold g_cfg_lk for once it is made. */
void sync_ctx_begin(struct sync_ctx *sx)
{
   mutex_lock(&g_op_lk);
   mutex_lock(&g_cfg_lk);
   sx->http     = g_http;
   sx->uid      = g_uid;
   sx->have_key = g_have_key;
   memcpy(sx->key, g_key, sizeof sx->key);
   sx->nlog = g_nlog;
   memcpy(sx->log, g_log, sizeof sx->log);
   mutex_unlock(&g_cfg_lk);
   sx->buf    = &g_buf;
   sx->bufcap = &g_bufcap;
   sx->row    = &g_row;
   sx->rowcap = &g_rowcap;
   sx->sel    = &g_sel;
   sx->selcap = &g_selcap;
   sx->rsp    = g_rsp;
}

/* The other half of sync_ctx_begin: one operation at a time. Every path that
 * begins a context ends it, including the ones that give up early. */
void sync_ctx_end(void)
{
   mutex_unlock(&g_op_lk);
}

/* THE MEMORY BOUND IS A WINDOW, NOT THE FILE.
 *
 * Reading the whole log into memory is fine at two megabytes and a
 * guaranteed wall at some larger number -- one that arrives silently, years
 * later, as "sync stopped working". Nothing about the protocol needs
 * the whole file -- a bucket is a day, and only a bucket has to be sorted and
 * hashed together. So the file is streamed, and only the rows of the buckets
 * in the CURRENT WINDOW are kept.
 *
 * Cost: one pass over the log per window, so a log with more buckets than fit
 * in one window is read more than once. With a 2 MB window and a day of
 * readings weighing ~25 kB, a window holds about eighty days -- a decade of
 * history is a handful of passes, and the usual case (a few recent days
 * differing) is one. */
#define WINDOW_BYTES (2L * 1024 * 1024)
#define LOG_ROWS_MAX 200000

/* ---- THE BUCKET SCRATCH -----------------------------------------------
 *
 * Four lists, one set, allocated on first use and kept for the life of the
 * process: the remote bucket numbers and their hashes, the local bucket
 * numbers, and a byte per remote bucket saying whether this phone needs it.
 * At the wire's limit that is about a megabyte, which is why it is not four
 * static arrays -- a phone that never syncs would carry all of it.
 *
 * ONE SET, because one operation runs at a time (see the operation lock
 * above); the same argument that lets the row window be shared. An
 * allocation that fails answers NULL, and every caller refuses the sync --
 * the same answer a fixed ceiling gives when a log outgrows it, for the same
 * reason: half a picture of what the server holds drives a loop that
 * deletes. */
static int64_t *g_rb;
static char (*g_rh)[17];
static int64_t *g_lb;
static unsigned char *g_need;

int64_t *sync_rb(void)
{
   if (!g_rb)
      g_rb = malloc(sizeof *g_rb * SYNC_REMOTE_BUCKETS);
   return g_rb;
}

char (*sync_rh(void))[17]
{
   if (!g_rh)
      g_rh = malloc(sizeof *g_rh * SYNC_REMOTE_BUCKETS);
   return g_rh;
}

int64_t *sync_lb(void)
{
   if (!g_lb)
      g_lb = malloc(sizeof *g_lb * SYNC_LOCAL_BUCKETS);
   return g_lb;
}

unsigned char *sync_need(void)
{
   if (!g_need)
      g_need = malloc(sizeof *g_need * SYNC_REMOTE_BUCKETS);
   return g_need;
}

int buf_reserve(int64_t need)
{
   if (need <= g_bufcap)
      return 1;
   if (need > WINDOW_BYTES + (2L * SYNC_ROW_MAX))
      return 0;
   char *p = realloc(g_buf, (size_t)need);
   if (!p)
      return 0;
   g_buf    = p;
   g_bufcap = need;
   return 1;
}

int sel_reserve(int need)
{
   if (need <= g_selcap)
      return 1;
   if (need > LOG_ROWS_MAX)
      return 0;
   int want = g_selcap ? g_selcap * 2 : 4096;
   while (want < need)
      want *= 2;
   struct row *p = realloc(g_sel, (size_t)want * sizeof *p);
   if (!p)
      return 0;
   g_sel    = p;
   g_selcap = want;
   return 1;
}

int row_reserve(int need)
{
   if (need <= g_rowcap)
      return 1;
   if (need > LOG_ROWS_MAX)
      return 0;
   int want = g_rowcap ? g_rowcap * 2 : 4096;
   while (want < need)
      want *= 2;
   struct row *p = realloc(g_row, (size_t)want * sizeof *p);
   if (!p)
      return 0;
   g_row    = p;
   g_rowcap = want;
   return 1;
}

/* ---- CONFIGURATION SWAPS, each one atomic against a running operation --
 *
 * Every setter takes g_cfg_lk. A reader never holds it for longer than the
 * memcpy in sync_ctx_begin, so a setter cannot be delayed by a sync that is
 * waiting on a slow server. */
void sync_set_http(sync_http_fn fn)
{
   mutex_lock(&g_cfg_lk);
   g_http = fn;
   mutex_unlock(&g_cfg_lk);
}

void sync_set_key(int64_t uid, const uint8_t key[SYNC_KEY_LEN])
{
   /* THE IDENTITY IS ONE FACT. Read field by field, an operation can sign
    * with a new uid and a stale key -- a signature the server rejects, and a
    * pairing that looks broken immediately after succeeding. */
   mutex_lock(&g_cfg_lk);
   g_uid = uid;
   for (int i = 0; i < SYNC_KEY_LEN; i++)
      g_key[i] = key[i];
   g_have_key = uid > 0;
   mutex_unlock(&g_cfg_lk);
}

int sync_set_logs(const struct sync_log_spec *specs, int n)
{
   if (n < 0 || n > SYNC_MAX_LOGS)
      return -1;
   if (n > 0 && !specs)
      return -1;

   /* BUILT AND VALIDATED OFF THE LOCK, into a registry of our own. Nothing
    * is visible to an operation until the whole list is known to be sound --
    * which is what makes "publish or don't" expressible at all. Built in
    * place, a name that does not fit would abandon the registration halfway
    * and leave whatever had already been added as the live configuration. */
   struct sync_log built[SYNC_MAX_LOGS];
   for (int i = 0; i < n; i++) {
      if (!specs[i].name || !specs[i].path)
         return -1;
      int w =
          snprintf(built[i].name, sizeof built[i].name, "%s", specs[i].name);
      if (w <= 0 || w >= (int)sizeof built[i].name)
         return -1;
      w = snprintf(built[i].path, sizeof built[i].path, "%s", specs[i].path);
      if (w <= 0 || w >= (int)sizeof built[i].path)
         return -1;
      built[i].bucketed = specs[i].bucketed;
   }

   /* ONE LOCK ACQUISITION, both fields. The count and the array it counts
    * are one fact: a reader that saw a new count against the previous array
    * would read a slot nobody filled, and a reader between two acquisitions
    * would see a registry that never existed. */
   mutex_lock(&g_cfg_lk);
   for (int i = 0; i < n; i++)
      g_log[i] = built[i];
   g_nlog = n;
   mutex_unlock(&g_cfg_lk);
   return 0;
}

/* THE CANONICAL FORM OF ONE BUCKET, and the only place it is spelled out.
 *
 * The hash the phone and the server compare is taken over THIS text, so the
 * two sides agree only while they build it identically: the bucket's rows,
 * SORTED, DE-DUPLICATED, each newline-terminated. A bucket is a SET -- an
 * identical row written twice is one row, which is what the server's primary
 * key enforces on its side.
 *
 * It was written out three times: once here and once inside each of the two
 * sync loops. Three copies of an invariant that must match a REMOTE
 * implementation is the kind of duplication that does not announce itself
 * when it drifts -- the symptom is a bucket both sides hold identically and
 * neither will stop re-sending, forever.
 *
 * `src` are the rows currently loaded (an index into g_buf). When `filter` is
 * set, only rows falling in `bucket` are taken; otherwise all of them are,
 * for a caller that has already scanned exactly one bucket. Returns the
 * length written, or -1 if it does not fit.
 *
 * NUL-TERMINATED, and one byte of `cap` is reserved for that. The length is
 * the answer the callers use -- the push signs and sends exactly `k` bytes --
 * but the buffer is also read as a C string (the interop test's strstr, and
 * anything that logs it), and an unterminated one runs off the end into
 * whatever the caller's stack held. It cost nothing to terminate and the
 * absence of it was invisible until a caller's stack happened to be
 * dirty. */
int64_t bucket_text(const struct row *src, int nsrc, int filter, int64_t bucket,
                    int bucketed, char *out, size_t cap)
{
   if (!src || !out)
      return -1;
   if (!sel_reserve(nsrc))
      return -1;
   int nb = 0;
   for (int i = 0; i < nsrc; i++) {
      if (filter &&
          row_bucket(g_buf + src[i].off, src[i].len, bucketed) != bucket)
         continue;
      g_sel[nb++] = src[i];
   }
   row_sort(g_buf, g_sel, nb);
   size_t k = 0;
   for (int i = 0; i < nb; i++) {
      if (i && g_sel[i].len == g_sel[i - 1].len &&
          !memcmp(g_buf + g_sel[i].off, g_buf + g_sel[i - 1].off, g_sel[i].len))
         continue;
      /* +1 for this row's newline, and cap-1 because the terminator is
       * already spoken for. */
      if (cap == 0 || g_sel[i].len + 1 > (cap - 1) - k)
         return -1;
      memcpy(out + k, g_buf + g_sel[i].off, g_sel[i].len);
      k += g_sel[i].len;
      out[k++] = '\n';
   }
   if (cap == 0)
      return -1;
   out[k] = '\0';
   return (int64_t)k;
}

static int64_t sync_bucket_text_locked(const struct sync_ctx *sx, int log_idx,
                                       int64_t bucket, char *out, int64_t cap);

/* ONE BUCKET'S CANONICAL TEXT, for a caller outside an operation. Takes the
 * operation lock for the duration, exactly as a push or a pull does, so what
 * it renders is one consistent view of the log. Returns the length written,
 * or -1. */
int64_t sync_bucket_text(int log_idx, int64_t bucket, char *out, int64_t cap)
{
   static struct sync_ctx sx;
   sync_ctx_begin(&sx);
   int64_t n = sync_bucket_text_locked(&sx, log_idx, bucket, out, cap);
   sync_ctx_end();
   return n;
}

/* The body, with the operation's context already in hand. Split out because
 * the entry point above is a test helper that must take the lock, and nothing
 * inside an operation may take it twice. */
static int64_t sync_bucket_text_locked(const struct sync_ctx *sx, int log_idx,
                                       int64_t bucket, char *out, int64_t cap)
{
   if (log_idx < 0 || log_idx >= sx->nlog)
      return -1;
   const struct sync_log *l = &sx->log[log_idx];
   int64_t one              = bucket;
   int nb                   = log_scan(l, &one, 1, NULL);
   if (nb < 0)
      return -1;
   if (cap < 0)
      return -1;
   /* log_scan already selected exactly this bucket, so no further filter. */
   return bucket_text(g_row, nb, 0, bucket, l->bucketed, out, (size_t)cap);
}

/* ---- signed requests -------------------------------------------------- */

/* ---- SYNC PROGRESS, PUBLISHED AS ONE STATE -------------------
 *
 * sync_run() is driven from Ble.remotePush's worker thread (and from the
 * interop test's main thread); the UI reads this to draw the bar. Two
 * threads, so the state has to be published rather than merely stored -- as
 * ONE word, not as three separate atomics with an ordering argument tying
 * them together, which is not the same thing.
 *
 * WHY THREE ATOMICS ARE NOT ENOUGH, and it is not a memory-ordering
 * subtlety. A reader takes them ONE AT A TIME: it can load `active` and
 * `done` from one run and `total` from the NEXT, because a run can end and
 * another begin in between. What that produces is not a stale bar, which
 * would be fine, but a bar built out of two different runs -- 900 of 12,
 * drawn as 7500% -- and the model's easing then carries the nonsense forward
 * for several frames. "At worst a stale pair, never a torn one" is a claim
 * about one pair, and there is more than one pair here.
 *
 * SO IT IS ONE WORD, and every reader gets one state:
 *
 *    bit  63      active
 *    bits 62..48  generation, incremented by each begin (wraps; it exists to
 *                 tell two runs apart, not to count them)
 *    bits 47..24  total buckets
 *    bits 23..0   buckets done
 *
 * 24 bits is 16.7 million buckets, which is four orders of magnitude past
 * anything this client holds; both counters are CLAMPED rather than
 * truncated, because a wrapped total is a plausible-looking denominator and a
 * wrapped done is a bar that goes backwards.
 *
 * THE CONSISTENCY CONTRACT, stated rather than implied: a reader sees a state
 * that EXISTED -- one run's active flag, its total, and a `done` that
 * belonged to it -- possibly a few buckets old. It never sees fields from two
 * runs. That is what the generation is for on the reader's side too: two
 * reads with the same generation describe the same run.
 *
 * WHY NO LOCK. The writer is one thread (begin, step and end are all the sync
 * worker's, in that order), and the reader is the main looper drawing a
 * frame. A lock here would be a frame waiting on a worker for a progress bar.
 * A single 64-bit atomic is lock-free on every target this runs on and the
 * read is one instruction. */
/* UNSIGNED WIDTHS. These are shift counts and mask widths, and a signed
 * operand in a bitwise expression is a different rule from the one this
 * packing is written against. */
#define PROG_CNT_BITS 24U
#define PROG_CNT_MAX  ((1U << PROG_CNT_BITS) - 1U)
#define PROG_GEN_BITS 15U
#define PROG_GEN_MASK ((1U << PROG_GEN_BITS) - 1U)

static atomic_ullong g_prog;

static unsigned prog_clamp(int v)
{
   if (v < 0)
      return 0;
   return ((unsigned)v > PROG_CNT_MAX) ? PROG_CNT_MAX : (unsigned)v;
}

static unsigned long long prog_pack(int active, unsigned gen, int total,
                                    int done)
{
   return ((unsigned long long)(active ? 1U : 0U) << 63U) |
          ((unsigned long long)(gen & PROG_GEN_MASK) << 48U) |
          ((unsigned long long)prog_clamp(total) << PROG_CNT_BITS) |
          (unsigned long long)prog_clamp(done);
}

static struct sync_prog prog_unpack(unsigned long long w)
{
   struct sync_prog p;
   p.active = (int)(w >> 63U);
   p.gen    = (unsigned)((w >> 48U) & PROG_GEN_MASK);
   p.total  = (int)((w >> PROG_CNT_BITS) & PROG_CNT_MAX);
   p.done   = (int)(w & PROG_CNT_MAX);
   return p;
}

/* THE WHOLE STATE, in one read. The word is packed so that `active`, `gen`,
 * `total` and `done` cannot be torn apart: two fields from different runs
 * would let a caller draw "3 of 7" from one run beside a total from the
 * next. */
struct sync_prog sync_progress_get(void)
{
   return prog_unpack(atomic_load_explicit(&g_prog, memory_order_acquire));
}

/* A NEW RUN, which is a new generation. `done` starts at zero and `total` is
 * what the run set out to examine; the generation moves so a reader can tell
 * this run's numbers from the previous run's. */
void sync_progress_begin(int total)
{
   struct sync_prog p =
       prog_unpack(atomic_load_explicit(&g_prog, memory_order_relaxed));
   atomic_store_explicit(&g_prog, prog_pack(1, p.gen + 1U, total, 0),
                         memory_order_release);
}

/* The run is over. The counts STAY: a finished run's last figures are what a
 * screen should keep showing, and `active` is what says it has stopped. */
void sync_progress_end(void)
{
   struct sync_prog p =
       prog_unpack(atomic_load_explicit(&g_prog, memory_order_relaxed));
   atomic_store_explicit(&g_prog, prog_pack(0, p.gen, p.total, p.done),
                         memory_order_release);
}

void sync_progress_step(void)
{
   /* ONE WRITER, so load-modify-store is enough and needs no CAS: begin,
    * step and end all run on the sync worker, in order. A second writer
    * would need a compare-exchange loop here, and would also mean two syncs
    * in flight, which sync_run's own single flight refuses. */
   struct sync_prog p =
       prog_unpack(atomic_load_explicit(&g_prog, memory_order_relaxed));
   atomic_store_explicit(&g_prog,
                         prog_pack(p.active, p.gen, p.total, p.done + 1),
                         memory_order_release);
}

int sync_progress(int *done, int *total)
{
   struct sync_prog p = sync_progress_get();
   if (done)
      *done = p.done;
   if (total)
      *total = p.total;
   return p.active;
}

/* Every bucket this client holds, across every log: the denominator of the
 * progress the UI draws. Counted up front, in one pass per log, because a
 * bar that discovers its own length as it goes is not a bar. */
int sync_count_buckets(const struct sync_ctx *sx)
{
   int64_t *bl = sync_lb();
   if (!bl)
      return 0; /* nothing countable: the bar shows what it can */
   int total = 0;
   for (int i = 0; i < sx->nlog; i++) {
      int n = log_buckets(&sx->log[i], bl, SYNC_LOCAL_BUCKETS);
      if (n > 0) /* a log we cannot enumerate contributes nothing to a count */
         total += n;
   }
   return total;
}

/* THE NONCE, AND WHAT MAKES IT UNIQUE.
 *
 * THE INVARIANT: no two signed requests this phone ever makes carry the same
 * nonce, for any two requests the server could still be holding -- and the
 * server holds them for its whole replay window (2 * SIG_SKEW).
 *
 * NOT "p<seconds>-<counter>" with a process-local counter, on the claim that
 * the counter cannot repeat within a run while the timestamp cannot repeat
 * across runs. The second half is false in two ordinary situations:
 *
 *   - TWO RESTARTS IN THE SAME SECOND. Such a counter goes back to 1 with
 *     every process, and this process is restarted by the OS whenever the
 *     service is rebuilt, the activity is swiped away, or Android reclaims
 *     it. Two runs starting inside one second -- a crash and its immediate
 *     restart -- produce "p<T>-1" twice.
 *   - A CLOCK THAT GOES BACKWARDS. realtime_s() is the WALL clock: NTP
 *     corrects it after a flat battery, and the user can set it by hand. Once
 *     it steps back, every second it re-covers can re-issue nonces that are
 *     still inside the server's window.
 *
 * A repeat is not a security hole -- the server rejects it, which is what the
 * table is for -- but the request it rejects is a LEGITIMATE one, and what
 * the user sees is a sync that fails for no visible reason and keeps failing
 * while the clock catches up.
 *
 * So: 128 bits from the OS entropy source, and nothing else. No clock, no
 * counter, no process state -- which is exactly why a restart, a same-second
 * restart, or a clock correction cannot affect it. Two nonces collide with
 * probability 2^-128 per pair; a phone that syncs every five minutes for a
 * century makes ~10^7 requests, so the chance of any collision at all is
 * around 10^-24.
 *
 * A FAILURE TO GET ENTROPY IS A FAILURE TO SIGN. rand_bytes returns 0 with
 * the buffer UNDEFINED, and an undefined nonce is stack contents: repeatable,
 * and repeatable is the one thing this may not be. The request is refused
 * instead -- the sync retries, and the alternative is a request the server
 * will reject anyway. */
int sync_pair(const char *email, const char *code,
              uint8_t out_key[SYNC_KEY_LEN], int64_t *out_uid)
{
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   int rc = sync_pair_inner(&sx, email, code, out_key, out_uid);
   sync_ctx_end();
   return rc;
}

int sync_restore(void)
{
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   int rc = sync_restore_inner(&sx);
   sync_ctx_end();
   return rc;
}

static int sync_run_inner(const struct sync_ctx *sx)
{
   /* Straight to the per-log work, with NO top-level GET /v1/digest. Such a
    * request looks like it short-circuits an unchanged sync in one round
    * trip, but nothing consumes its answer: the real "is there anything to
    * do" question is answered before we ever open a socket, by the file sizes
    * (see syncjni_state_stamp), and each log's digest is fetched below
    * anyway. */
   for (int i = 0; i < sx->nlog; i++)
      if (sync_one_log(sx, i) != 0)
         return -1;

   /* The re-check is what turns "we pushed some rows" into the guarantee: if
    * the two digests do not agree now, the sync failed, whatever it sent. */
   if (signed_req(sx, "GET", "/v1/digest", "", 0, sx->rsp, SYNC_BUF_MAX) != 200)
      return -1;
   char nm[40];
   char hh[17];
   int64_t cnt   = 0;
   const char *q = sx->rsp;
   /* Which of OUR logs the server accounted for. The loop below walks the
    * server's answer, so on its own it can only ever prove things about logs
    * the server chose to mention -- see the end of this function. */
   int seen[SYNC_MAX_LOGS];
   for (int i = 0; i < SYNC_MAX_LOGS; i++)
      seen[i] = 0;
   int nlines  = 0;
   int64_t *lb = sync_lb();
   if (!lb)
      return -1;
   for (;;) {
      enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
      if (d == DLINE_END)
         break;
      /* Here the name is a LOG name, not a number, so it is validated by the
       * strcmp against g_log below rather than by digest_num -- but the line's
       * framing and its hash width are checked the same way, and a damaged
       * reply must not read as "the server holds fewer logs than it does". */
      if (d == DLINE_BAD)
         return -1;
      /* A reply cannot describe more logs than exist. Each iteration below
       * costs a full pass over a log, so an answer with a thousand lines --
       * confused server, mangled response, hostile middlebox -- would put the
       * phone into a scan loop that reads the whole record over and over. */
      if (++nlines > SYNC_MAX_LOGS)
         return -1;
      int li = -1;
      for (int i = 0; i < sx->nlog; i++)
         if (strcmp(sx->log[i].name, nm) == 0) {
            li = i;
            break;
         }
      if (li < 0)
         return -1; /* the server holds a log we do not */
      seen[li] = 1;
      static char text[SYNC_BUF_MAX];
      /* Fold this log's buckets exactly as the server does: SHA-256 over
       * "<bucket> <hash>\n" lines in ascending bucket order.
       *
       * TWO THINGS THAT GO WRONG THE OBVIOUS WAY, both invisible until the
       * log gets long.
       *
       * Calling sync_bucket_text once per bucket makes each of those a
       * FULL PASS over the file -- so the verify cost days x filesize. At one
       * CGM that is roughly 1.6 GB read per sync in the first year and 41 GB
       * by the fifth, on a phone, as often as once a minute. It now walks the
       * buckets in the same WINDOWS sync_one_log uses: one pass per 128
       * buckets rather than one pass per bucket.
       *
       * And it accumulated the fold into a fixed 128 kB buffer, ~23 bytes per
       * bucket, so at about 5,700 buckets -- fifteen and a half years -- every
       * sync would have begun failing with no way to tell why. The fold is a
       * SHA-256 of a byte stream, so it can be fed incrementally and needs no
       * buffer at all. */
      int nlb = log_buckets(&sx->log[li], lb, SYNC_LOCAL_BUCKETS);
      if (nlb < 0) /* cannot enumerate: a failed verify, not an empty log */
         return -1;
      struct sha256_ctx fold;
      sha256_init(&fold);
      size_t nb_all = (size_t)nlb;
      size_t at     = 0;
      while (at < nb_all) {
         size_t span = nb_all - at;
         if (span > 128)
            span = 128;
         int nrow     = 0;
         int overflow = 0;
         for (;;) {
            nrow = log_scan(&sx->log[li], lb + at, (int)span, &overflow);
            if (nrow >= 0)
               break;
            if (!overflow || span == 1)
               return -1;
            span /= 2;
         }
         for (size_t k = at; k < at + span; k++) {
            int64_t tl = bucket_text(g_row, nrow, 1, lb[k],
                                     sx->log[li].bucketed, text, sizeof text);
            if (tl < 0)
               return -1;
            size_t tn = (size_t)tl;
            char bh2[17];
            hash16(text, (int64_t)tn, bh2);
            char line[40];
            int w =
                snprintf(line, sizeof line, "%" PRIwire " %s\n", lb[k], bh2);
            if (w <= 0 || w >= (int)sizeof line)
               return -1;
            sha256_update(&fold, (const uint8_t *)line, (size_t)w);
         }
         at += span;
      }
      uint8_t fd32[32];
      sha256_final(&fold, fd32);
      char mine[17];
      static const char hx2[] = "0123456789abcdef";
      for (size_t i = 0; i < 8; i++) {
         mine[2 * i]       = hx2[fd32[i] >> 4U];
         mine[(2 * i) + 1] = hx2[fd32[i] & 15U];
      }
      mine[16] = 0;
      if (strncmp(mine, hh, 16) != 0)
         return -1;
   }

   /* THE OTHER HALF OF THE GUARANTEE.
    *
    * Everything above is driven by the server's answer, so it can only prove
    * that the logs the SERVER named match. A reply that parsed to zero lines
    * -- an empty body, a truncated one (see the outcap note in syncjni), a
    * response the transport mangled -- ran this loop zero times and reached
    * the end having verified nothing at all. Ending with
    *
    *     return nsame >= 0 ? 0 : -1;
    *
    * would be an unconditional `return 0` -- that counter starts at 0 and
    * only ever increments -- so sync_run() would report "both sides provably
    * hold the same bytes" without having compared anything. sync.h promises
    * that sentence; this is what has to be true for it.
    *
    * So: every log of ours that holds a single row must have appeared. The
    * enumeration below costs one pass per log the server did NOT mention,
    * which in the normal case is only the logs that are empty -- and an empty
    * or missing log answers immediately without reading anything. */
   for (int i = 0; i < sx->nlog; i++) {
      if (seen[i])
         continue;
      int n = log_buckets(&sx->log[i], lb, SYNC_LOCAL_BUCKETS);
      if (n < 0)
         return -1; /* cannot tell whether it holds data: not a pass */
      if (n > 0)
         return -1; /* we hold days the server never accounted for */
   }
   /* Reaching here after a response that named nothing now means something
    * specific and checked: every log we hold is genuinely empty, proved by
    * the enumeration above, rather than merely unmentioned. */
   return 0;
}

/* The entry point, and the only place the progress counters are armed and
 * disarmed -- so a failure anywhere inside still clears the progress bar. */
int sync_run(void)
{
   /* THE OPERATION OWNS THE WORKSPACE FOR ITS WHOLE DURATION, and reads a
    * configuration that cannot change under it. Both come from the context. */
   struct sync_ctx sx;
   sync_ctx_begin(&sx);
   if (!sx.have_key || !sx.http) {
      sync_ctx_end();
      return -1;
   }
   sync_progress_begin(sync_count_buckets(&sx));
   int rc = sync_run_inner(&sx);
   sync_progress_end();
   sync_ctx_end();
   return rc;
}

