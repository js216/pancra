// SPDX-License-Identifier: GPL-3.0
// syncint.h --- the seam between sync.c and synclocal.c, and nothing wider
// Copyright 2026 Jakob Kastelic

/* NOT PART OF THE SYNC INTERFACE. app/sync.h is that. This is the seam
 * between the files ONE module is written in, and nothing outside it should
 * include this header.
 *
 * THE MODULE IS SIX FILES AND ONE COORDINATOR:
 *
 *   app/sync.c         the coordinator. Owns the configuration, the
 *                      one-operation-at-a-time lock, the workspace and the
 *                      progress counters; takes the snapshot every operation
 *                      runs from and is the only file that can.
 *   app/syncsign.c     the signature on every request, and the routes.
 *   app/syncpair.c     pairing: four J-PAKE rounds and their reply grammar.
 *   app/syncpush.c     which buckets the server lacks, and putting them there.
 *   app/syncrestore.c  pulling the record back.
 *   app/synclocal.c    the LOCAL side: streaming this phone's own log files.
 *   app/syncrow.c      what one row is, and how it sorts and hashes.
 *
 * WHY IT IS SPLIT AT ALL. It was one 1850-line file holding the configuration,
 * the workspace, the signing, the pairing grammar, the digest parser, the push
 * reconciliation and the restore -- and the split is not about length. Three
 * of those workflows read the configuration GLOBALS while the operation around
 * them worked from a snapshot, which is a difference no reader could see and
 * the compiler could not enforce: a pairing that completed mid-sync changed
 * the key that the rest of that sync signed with. Now the globals are static
 * in the coordinator and every workflow is handed a `const struct sync_ctx *`.
 * The property is structural: a workflow file cannot read live configuration
 * because it cannot NAME it.
 *
 * WHY THE WORKSPACE IS SHARED AT ALL, rather than passed. These buffers grow
 * to hold a window of rows and are reused for the whole operation; handing
 * them down as parameters would be four more arguments on every call, and
 * handing them down as a struct would be the struct that already exists
 * (sync_ctx) carrying a second copy of what the operation already owns. They
 * are guarded by the one-operation-at-a-time lock in sync.c -- which is the
 * real invariant, and it is stated there. */
#ifndef PANCRA_SYNCINT_H
#define PANCRA_SYNCINT_H

/* SYNC.H FIRST, and this refuses to be included without it. It needs
 * sync_http_fn, SYNC_KEY_LEN and SYNC_MAX_LOGS -- and including sync.h from
 * here would make the module include itself, which `make inclusions` refuses
 * for a good reason: a cycle is two files neither of which can be read on its
 * own. Every file in the module includes sync.h first anyway, because the
 * interface is what it implements. */
#ifndef PANCRA_SYNC_H
#error "include sync.h first: syncint.h is the seam behind that interface"
#endif

#include "syncrow.h" /* struct row: what these buffers hold */
#include <stdint.h>

/* ONE LOG THIS PHONE SYNCS: what it is called on the wire, where it is on
 * disk, and whether its rows carry a bucket. Here rather than in sync.h
 * because it is not part of the interface -- a caller registers a log by name
 * and path (sync_add_log) and never sees this -- but BOTH halves of the module
 * need its shape: one decides which buckets the server lacks, the other opens
 * the file and reads them. */
struct sync_log {
   char name[40];
   char path[256];
   int bucketed;
};

/* THE ROW WINDOW: the bytes, and an index into them. A log is never held
 * whole (see synclocal.c); this is one window's worth. */
extern char *g_buf;
extern int64_t g_bufcap;
extern struct row *g_row;
extern int g_rowcap;

/* A SECOND INDEX, for picking one bucket's rows out of a loaded window
 * without disturbing the window's own index. */
extern struct row *g_sel;
extern int g_selcap;

/* Grow each of the three to hold at least `need`. 1 on success, 0 when the
 * allocation failed -- and then the caller must not write to it. */
int buf_reserve(int64_t need);
int sel_reserve(int need);
int row_reserve(int need);

/* ---- WHAT THE LOCAL SIDE ANSWERS ------------------------------------
 *
 * Both stream the file rather than holding it: see app/synclocal.c. */

/* Stream `l`, keeping the rows whose bucket is in `want` (or every row when
 * `want` is NULL), into the shared window. Returns the row count, or -1 when
 * even the window did not fit. A missing file is an empty log, not an error.
 * `*overflow` is set when rows were dropped for want of room. */
int log_scan(const struct sync_log *l, const int64_t *want, int nwant,
             int *overflow);

/* Every bucket `l` holds, ascending, into `out` (at most `cap`). Returns how
 * many, or -1 on a read failure. A missing file has no buckets. */
int log_buckets(const struct sync_log *l, int64_t *out, int cap);

/* ---- ONE OPERATION'S OWN WORLD ---------------------------------------
 *
 * A snapshot of the configuration plus the workspace the running operation
 * may use. Taken by the coordinator under two locks (see sync_ctx_begin) and
 * handed down; nothing below the coordinator can reach the live values, which
 * is the whole point.
 *
 * The workspace is not copied in -- the context POINTS at the single set,
 * because only one operation may hold it, which is what the operation lock
 * enforces. Reallocating scratch shared between two concurrent operations is
 * how a restore came to parse a run's half-loaded window: the buffers grow,
 * and a realloc under another thread's pointer is not a stale read, it is a
 * freed one. */
struct sync_ctx {
   /* configuration: immutable for the life of one operation */
   sync_http_fn http;
   int64_t uid;
   uint8_t key[SYNC_KEY_LEN];
   int have_key;
   struct sync_log log[SYNC_MAX_LOGS];
   int nlog;
   /* workspace: owned by whoever holds the operation lock */
   char **buf;
   int64_t *bufcap;
   struct row **row;
   int *rowcap;
   struct row **sel;
   int *selcap;
   char *rsp;
};

/* Take the operation lock and snapshot the configuration. Every public entry
 * point pairs these, and NOTHING between them may take the lock again -- it
 * is not recursive and nothing in this module needs it to be. */
void sync_ctx_begin(struct sync_ctx *sx);

void sync_ctx_end(void);
/* ---- what one workflow file asks of another --------------------------- */

/* SIGN AND SEND. Every byte this phone puts on the wire under its own
 * identity goes through here, which is why the argument checks are here and
 * not in the public wrapper. Returns the HTTP status, or negative when the
 * request could not be made. (app/syncsign.c) */
int signed_req(const struct sync_ctx *sx, const char *method, const char *path,
               const char *body, int blen, char *out, int outcap);

/* The canonical text of one bucket, built from a loaded window: rows sorted
 * bytewise ascending, each terminated by '\n'. `filter` keeps only the rows
 * whose bucket is `bucket`. Returns the length, or -1. (app/sync.c) */
int64_t bucket_text(const struct row *src, int nsrc, int filter, int64_t bucket,
                    int bucketed, char *out, size_t cap);

/* Every bucket this client holds, across every log: the denominator of the
 * progress the UI draws. (app/sync.c) */
int sync_count_buckets(const struct sync_ctx *sx);

void sync_progress_step(void);

/* Reconcile ONE log with the server: fetch its digest, push what is missing
 * or wrong, delete what this phone no longer has. 0 on success.
 * (app/syncpush.c) */
int sync_one_log(const struct sync_ctx *sx, int li);

/* The two long workflows, each already holding the operation's context.
 * (app/syncpair.c, app/syncrestore.c) */
int sync_pair_inner(const struct sync_ctx *sx, const char *email,
                    const char *code, uint8_t out_key[SYNC_KEY_LEN],
                    int64_t *out_uid);
int sync_restore_inner(const struct sync_ctx *sx);

/* THE BUCKET SCRATCH, allocated on first use and owned by the running
 * operation (see sync.h for the sizes and app/sync.c for why it is not
 * static). NULL when the allocation failed, and then the caller must refuse
 * the operation rather than work from a shorter list. */
int64_t *sync_rb(void);
char (*sync_rh(void))[17];
int64_t *sync_lb(void);
unsigned char *sync_need(void);

/* Digits only, non-empty, and within a long: one field of a digest line, and
 * of the restore's own count line. (app/syncpush.c) */
int digest_num(const char *s, const char *e, int64_t *out);

/* THE FAULT INJECTORS. Empty without APP_FAULTS; they exist so a host test
 * can damage a reply exactly where the parser is trusted -- a real server
 * never sends one of these, which is why the refusals they drive could
 * otherwise be argued for and never run. (app/syncpush.c) */
void digest_fault(char *buf);
void restore_body_fault(char *buf);

#endif
