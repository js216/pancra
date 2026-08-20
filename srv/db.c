/* SPDX-License-Identifier: GPL-3.0
 * db.c --- the single sqlite file, and the schema that lives in it
 * Copyright 2026 Jakob Kastelic
 *
 * ONE table holds all synced data, and it holds rows as TEXT, verbatim.
 *
 * That is a deliberate departure from a typed table per kind of reading. The
 * protocol promises that after a sync the app and the server hold EXACTLY the
 * same data, and the only way to keep that promise for data this server does
 * not understand -- a log the app grows next year, a settings blob, a
 * presentation file -- is to store the bytes the app sent and hand them back
 * unchanged. Parsing into columns would make the server the arbiter of a
 * format it does not own, and every parse bug would become silent data loss
 * on the one copy that is supposed to be the backup.
 *
 * The web pages parse rows when they render them (readings are CSV), which
 * costs one pass over a few hundred lines per page and keeps the ONE source
 * of truth intact.
 */
#include "db.h"
#include "proto.h"
#include <dirent.h>   /* the scratch directory db_verify empties by hand */
#include <fcntl.h>    /* openat/O_NOFOLLOW: a name checked through its fd */
#include <limits.h>   /* PATH_MAX, for the canonical forms compared below */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>   /* calloc/free/realpath/mkdtemp */
#include <string.h>
#include <sys/stat.h> /* device and inode: WHICH file, not which spelling */
#include <unistd.h>

/* ONE CONNECTION PER WORKER, not one for the whole pool.
 *
 * SQLITE_THREADSAFE=1 serialises individual API CALLS, which is enough for
 * single statements and not enough for a transaction: a transaction is a
 * property of a CONNECTION, so while one worker sat between BEGIN IMMEDIATE
 * and COMMIT in h_bucket_put, every other worker's statement on the same
 * handle joined that transaction. Two consequences, both real with a phone
 * pushing while a browser reads: a second worker's BEGIN failed outright, and
 * an unrelated INSERT -- the spent-nonce row that closes the replay window,
 * or a session bump -- could be undone by a ROLLBACK it had nothing to do
 * with. The comment in web.c claiming "sqlite does its own locking" was true
 * of calls and false of transactions.
 *
 * WAL allows many connections to one file, and busy_timeout covers the case
 * where two of them want to write at once. The cost is page cache per
 * connection, so a worker's is deliberately small -- see db_conf. */
/* THE DATABASE, AS A VALUE.
 *
 * It was a process singleton with no name: a static path and a thread-local
 * handle, reachable from anywhere that included this header, so "which
 * database" was not a question any caller could answer or any signature could
 * ask. Every query took the one there was.
 *
 * Now it is an object. main() opens it and hands it to the server (through
 * the pool, into each request), and every helper that touches storage says
 * which database it means. What that buys, concretely: the backup command and
 * the server can be the same process, a test can open two, and no function
 * can reach storage it was not given.
 *
 * The per-thread CONNECTION is still per thread -- that is a sqlite
 * requirement, not a design choice (see the note above) -- but it is now a
 * cache of the connection to THIS context: switch context on a thread and the
 * old connection is closed and a new one opened. */
struct db {
   char path[512];
};

/* ONE ALLOCATION PER DATABASE, not one static. A server run opens one and a
 * CLI subcommand opens one, but "one at a time" is a fact about today's
 * callers and not a property to build in: a static here would make the second
 * db_open silently hand back the first database, which is the singleton this
 * change exists to remove, hiding behind a pointer. */
/* AT MOST ONE OF THESE IS LIVE PER THREAD, and that is the invariant every
 * open has to keep. It was not kept: db_open assigned t_conn directly, so
 * opening context B after context A overwrote A's pointer -- the connection
 * itself stayed open, its descriptors (the file, its -wal, its -shm) stayed
 * allocated, and nothing could ever reach it again. Not a switch that could
 * be switched back: a LEAK, of the one handle that also held whatever A had
 * open. A long-lived process that opens a second database per operation runs
 * out of descriptors, and never notices why.
 *
 * So conn_open() below is the ONE place either of these is assigned, and it
 * closes what is there first. A second raw sqlite3_open into t_conn is how
 * this comes back. */
static _Thread_local sqlite3 *t_conn;
static _Thread_local struct db *t_of; /* which context t_conn belongs to */

#ifdef DB_FAULTS
#include <stdatomic.h> /* the counters are shared by the whole pool */
#include <stdlib.h>    /* getenv */

/* PROCESS-WIDE, not per thread. Requests are served by a pool, so "the n-th
 * prepare" counted per thread depends on which worker happened to take the
 * request -- and a test whose target moves is a test that fails on somebody
 * else's machine. Counted across the process (and atomically, because the
 * workers really are concurrent), n is the same call every run for a
 * sequential client. */
static int fault_at(const char *var, atomic_int *counter)
{
   const char *s = getenv(var);
   if (!s || !*s)
      return 0;
   long want = strtol(s, NULL, 10);
   int n     = atomic_fetch_add(counter, 1) + 1;
   if (want <= 0 || (long)n != want)
      return 0;
   fprintf(stderr, "sync: INJECTED FAULT %s at call %d\n", var, n);
   return 1;
}

/* BY NAME, not by ordinal.
 *
 * The ordinal form is kept for the one thing it is good for -- proving a
 * binary really has the fault hooks compiled in -- but it cannot AIM. Every
 * request prepares several statements, and so does the readiness poll a test
 * makes while waiting for the server to come up: with the counter running
 * process-wide, the injected failure landed on whatever happened to be the
 * n-th prepare, which for ordinals 1..6 was the startup and readiness
 * traffic. Every asserted request then returned the plain truth and the
 * assertions passed for the wrong reason.
 *
 * Matching the SQL text lands the failure on the statement the case is about,
 * however many prepares happen before it. */
static int fault_prepare(const char *sql)
{
   static atomic_int n;
   const char *want = getenv("DB_FAIL_PREPARE_SQL");
   if (want && *want && sql && strstr(sql, want)) {
      fprintf(stderr, "sync: INJECTED FAULT DB_FAIL_PREPARE_SQL in: %s\n", sql);
      return 1;
   }
   return fault_at("DB_FAIL_PREPARE", &n);
}

static int fault_commit(const char *sql)
{
   static atomic_int n;
   if (!sql || sqlite3_strnicmp(sql, "COMMIT", 6) != 0)
      return 0;
   return fault_at("DB_FAIL_COMMIT", &n);
}

/* A STEP THAT FAILS PARTWAY THROUGH A SCAN -- the case the whole
 * DONE-versus-error rule is about, and the one a corrupt file cannot aim at:
 * damaging pages fails whichever reader happens to touch them first, so a
 * test written that way passes for the wrong reason.
 *
 * The trick is to let sqlite do it. A SQL function that always raises an
 * error is registered on every connection, and the statement under test is
 * wrapped so its own rows come first and that function is evaluated last:
 *
 *   SELECT * FROM (<the query>) UNION ALL SELECT db_fault_step()
 *
 * sqlite3_step then returns SQLITE_ROW for the real rows and SQLITE_ERROR at
 * the end -- exactly what a damaged page in the middle of a table does, and
 * exactly what a loop written as `while (step == ROW)` reads as a clean end.
 * Single-column queries only (the ones this aims at); bound parameters keep
 * their indexes through the wrapper.
 *
 * DB_FAIL_STEP_SQL=<substring> picks the statement. */
static void fault_step_fn(sqlite3_context *c, int argc, sqlite3_value **argv)
{
   (void)argc;
   (void)argv;
   sqlite3_result_error(c, "injected step failure", -1);
}

static void fault_register(sqlite3 *h)
{
   (void)sqlite3_create_function(h, "db_fault_step", 0, SQLITE_UTF8, NULL,
                                 fault_step_fn, NULL, NULL);
}

/* The statement to prepare instead, or NULL to prepare `sql` as it is. The
 * caller frees with sqlite3_free.
 *
 * THE COLUMN COUNT HAS TO MATCH. This appended `UNION ALL SELECT
 * db_fault_step()` -- one column -- so it could only ever wrap a
 * single-column query. Aimed at anything wider (the login throttle reads two)
 * the PREPARE failed with "SELECTs to the left and right of UNION ALL do not
 * have the same number of result columns", the caller saw NULL, and the test
 * passed for the wrong reason: through the prepare path, with the step branch
 * it was written for never executed. A fault that fires somewhere else is
 * worse than no fault at all.
 *
 * So the original is prepared first, purely to ask how many columns it has,
 * and the right-hand side is padded to match. */
static char *fault_step_wrap(sqlite3 *h, const char *sql)
{
   /* WHERE IN THE SCAN IT FAILS, which is two different tests.
    *
    * DB_FAIL_STEP_SQL fails AFTER the real rows: the reader has already been
    * handed answers, and the question is whether it reports what it managed
    * to read AS THE RECORD.
    *
    * DB_FAIL_STEP_FIRST_SQL fails on the FIRST step. For a reader that takes
    * one row and finalises -- the login throttle, the session lookup -- the
    * first form never fires at all: the fault row is second, and nothing ever
    * steps to it. That is exactly the branch where "no row" and "the scan
    * broke" have to be told apart, so it needs a fault it can reach. */
   int first        = 0;
   const char *want = getenv("DB_FAIL_STEP_SQL");
   if (!want || !*want || !sql || !strstr(sql, want)) {
      want  = getenv("DB_FAIL_STEP_FIRST_SQL");
      first = 1;
   }
   if (!want || !*want || !sql || !strstr(sql, want))
      return NULL;
   sqlite3_stmt *probe = NULL;
   if (sqlite3_prepare_v2(h, sql, -1, &probe, NULL) != SQLITE_OK) {
      sqlite3_finalize(probe);
      return NULL; /* it will fail to prepare on its own terms */
   }
   int ncol = sqlite3_column_count(probe);
   sqlite3_finalize(probe);
   if (ncol < 1)
      return NULL; /* not a query: nothing to step through */
   char pad[256];
   int n = snprintf(pad, sizeof pad, "db_fault_step()");
   for (int i = 1; i < ncol; i++) {
      int k = snprintf(pad + n, sizeof pad - (size_t)n, ",0");
      if (k < 0 || (size_t)k >= sizeof pad - (size_t)n)
         return NULL;
      n += k;
   }
   fprintf(stderr, "sync: INJECTED FAULT DB_FAIL_STEP_SQL in: %s\n", sql);
   if (first)
      return sqlite3_mprintf("SELECT %s UNION ALL SELECT * FROM (%s)", pad,
                             sql);
   return sqlite3_mprintf("SELECT * FROM (%s) UNION ALL SELECT %s", sql, pad);
}
#else
#define fault_prepare(sql)      0
#define fault_commit(sql)       0
#define fault_register(h)       ((void)0)
#define fault_step_wrap(h, sql) NULL
#endif

/* Pragmas every connection needs. Split out of db_open because a worker's
 * connection is opened lazily and must be configured identically. */
static int pragma_int(sqlite3 *h, const char *sql, int want)
{
   sqlite3_stmt *st = NULL;
   int ok = sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == want;
   if (!ok)
      fprintf(stderr, "sync: required database setting failed: %s (%s)\n", sql,
              sqlite3_errmsg(h));
   sqlite3_finalize(st);
   return ok;
}

static int pragma_text(sqlite3 *h, const char *sql, const char *want)
{
   sqlite3_stmt *st = NULL;
   int ok           = sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK &&
                      sqlite3_step(st) == SQLITE_ROW;
   const unsigned char *got = ok ? sqlite3_column_text(st, 0) : NULL;
   ok                       = got && !sqlite3_stricmp((const char *)got, want);
   if (!ok)
      fprintf(stderr, "sync: required database setting failed: %s (%s)\n", sql,
              sqlite3_errmsg(h));
   sqlite3_finalize(st);
   return ok;
}

static int required_exec(sqlite3 *h, const char *sql)
{
   char *e = NULL;
   int ok  = sqlite3_exec(h, sql, NULL, NULL, &e) == SQLITE_OK;
   if (!ok)
      fprintf(stderr, "sync: required database setting failed: %s (%s)\n", sql,
              e ? e : sqlite3_errmsg(h));
   sqlite3_free(e);
   return ok;
}

static void optional_exec(sqlite3 *h, const char *sql)
{
   char *e = NULL;
   if (sqlite3_exec(h, sql, NULL, NULL, &e) != SQLITE_OK)
      fprintf(stderr, "sync: optional database tuning unavailable: %s (%s)\n",
              sql, e ? e : sqlite3_errmsg(h));
   sqlite3_free(e);
}

/* ---- DURABILITY, FOR THE WRITES THAT CANNOT BE REPLAYED --------------
 *
 * PRAGMA synchronous is a property of a CONNECTION and may be changed between
 * transactions, not inside one -- so these bracket the transaction rather
 * than sitting within it. See db_conf for which writes need this and why the
 * rest deliberately do not.
 *
 * A failure to raise the level is a failure of the operation: proceeding
 * would produce exactly the acknowledgement this exists to prevent. */
int db_durable_begin(struct db *d)
{
   if (!db_exec(d, "PRAGMA synchronous=FULL;"))
      return 0;
   if (!db_exec(d, "BEGIN IMMEDIATE")) {
      (void)db_exec(d, "PRAGMA synchronous=NORMAL;");
      return 0;
   }
   return 1;
}

/* COMMIT, then drop back. The pragma is restored either way: a connection
 * left at FULL would quietly make every later bucket PUT pay an fsync. */
int db_durable_commit(struct db *d)
{
   int ok = db_exec(d, "COMMIT");
   if (!ok)
      (void)db_exec(d, "ROLLBACK");
   (void)db_exec(d, "PRAGMA synchronous=NORMAL;");
   return ok;
}

void db_durable_rollback(struct db *d)
{
   (void)db_exec(d, "ROLLBACK");
   (void)db_exec(d, "PRAGMA synchronous=NORMAL;");
}

static int db_conf(sqlite3 *h, int worker)
{
   /* ---- WHAT NORMAL ACTUALLY PROMISES, AND WHAT IT DOES NOT ----------
    *
    * WAL with synchronous=NORMAL cannot CORRUPT the file across a power cut:
    * whatever is recovered is a consistent database. That is the part this
    * comment used to claim and it is true.
    *
    * What it does NOT promise is that a COMMITTED transaction survives. WAL
    * frames are fsynced at a checkpoint, not at every commit, so a commit
    * that returned success can be gone after a power failure -- and the
    * board has no battery behind it. So an HTTP 200 sent right after such a
    * commit is an acknowledgement the storage has not earned.
    *
    * NORMAL is still the default here, deliberately: one fsync per checkpoint
    * instead of one per transaction is the difference between a bearable and
    * an unbearable SD card on this hardware, and the bulk of the write
    * traffic is bucket PUTs from a phone that pushes every few minutes.
    *
    * THE TWO KINDS OF WRITE ARE NOT THE SAME, and that is what makes this
    * safe rather than merely cheap:
    *
    *   RECONCILED writes -- the log buckets. The sync protocol is a replica
    *   protocol: the phone compares per-bucket hashes with the server on
    *   every sync and re-pushes anything the server does not have. A bucket
    *   lost to a power cut therefore comes back by itself at the next sync,
    *   without anybody noticing. That is the weaker acknowledgement this
    *   server offers on that path, and it is honest because the recovery is
    *   implemented rather than hoped for.
    *
    *   ONE-SHOT writes -- a pairing, a password change, an account, a share.
    *   Nothing re-sends these. A phone told "paired" that meets a server
    *   with no key signs every later request with a key the server never
    *   stored, and the user is left re-pairing a phone that believes it is
    *   already paired. These take db_durable_begin() (see db.h), which is FULL
    * for the length of the transaction -- rare enough that the fsync costs
    *   nothing measurable, and the only class where losing the commit cannot
    *   be undone by the protocol itself. */
   if (!pragma_text(h, "PRAGMA journal_mode=WAL;", "wal") ||
       !required_exec(h, "PRAGMA synchronous=NORMAL;") ||
       !pragma_int(h, "PRAGMA synchronous;", 1) ||
       !required_exec(h, "PRAGMA foreign_keys=ON;") ||
       !pragma_int(h, "PRAGMA foreign_keys;", 1))
      return 0;
   if (sqlite3_busy_timeout(h, 3000) != SQLITE_OK) {
      fprintf(stderr, "sync: required database busy timeout failed: %s\n",
              sqlite3_errmsg(h));
      return 0;
   }
   /* The first connection keeps the ~2 MB cache. A worker's is 256 kB: ten of
    * them at 2 MB would be twenty megabytes on a board with about nineteen
    * free, which is the whole reason the pages are serialised elsewhere. */
   optional_exec(h, worker ? "PRAGMA cache_size=-256;"
                           : "PRAGMA cache_size=-2000;");
   optional_exec(h, "PRAGMA journal_size_limit=1048576;");
   fault_register(h); /* nothing in a shipping build: see DB_FAULTS */
   return 1;
}

/* ---- THE ONE PLACE THIS THREAD'S CONNECTION CHANGES -------------------
 *
 * WHAT HAPPENED TO THE CONNECTION THAT WAS ALREADY HERE. Two answers and not
 * one, because "there was nothing to close" and "what was there is closed
 * now" are the same thing to a caller, and "it would not close" is not: on
 * that answer the old handle is STILL OPEN and t_conn still names it, so a
 * caller that carries on and assigns over it has performed exactly the leak
 * this exists to prevent. */
enum conn_take {
   CONN_TAKE_OK,  /* nothing was held, or what was held is closed */
   CONN_TAKE_BUSY /* it refused to close; it is still open and still cached */
};

/* CLOSE FAILURE IS NOT A FORMALITY HERE. sqlite3_close returns SQLITE_BUSY
 * when the connection still has unfinalized statements or an unfinished
 * backup on it -- a caller that stepped a query and never finalised it, which
 * is a real bug and one this codebase has had. Neither available alternative
 * is acceptable at a SWITCH:
 *
 *   - dropping the pointer anyway leaks the handle and its descriptors, which
 *     is the defect;
 *   - forcing it (sqlite3_close_v2) pulls the file out from under statements
 *     the caller is still holding, so the next step on one of them is a use
 *     after free.
 *
 * So the switch REFUSES: nothing changes, the old connection stays valid and
 * cached, and the open that wanted the change is told it did not happen. The
 * caller reports a database that could not be opened, which is true. */
static enum conn_take conn_release(void)
{
   if (!t_conn)
      return CONN_TAKE_OK;
   int rc = sqlite3_close(t_conn);
   if (rc != SQLITE_OK) {
      fprintf(stderr,
              "sync: this thread's connection to %s will not close: %s\n"
              "  Statements from an earlier query are still open on it, so it "
              "cannot be\n"
              "  switched to another database. This is a leaked "
              "sqlite3_stmt, not a storage fault.\n",
              t_of && t_of->path[0] ? t_of->path : "?", sqlite3_errmsg(t_conn));
      return CONN_TAKE_BUSY;
   }
   t_conn = NULL;
   t_of   = NULL;
   return CONN_TAKE_OK;
}

/* OPEN A CONNECTION TO `d` AND MAKE IT THIS THREAD'S. The only assignment to
 * t_conn/t_of in the file, so the "at most one live handle per thread"
 * invariant is a property of one function rather than a habit.
 *
 * `worker` picks the page cache (see db_conf): the connection main() opens
 * keeps the big one, a pool thread's is a tenth of it. */
static sqlite3 *conn_open(struct db *d, int worker)
{
   if (!d || !d->path[0])
      return NULL;
   if (conn_release() != CONN_TAKE_OK)
      return NULL;
   sqlite3 *h = NULL;
   if (sqlite3_open(d->path, &h) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot open %s: %s\n", d->path,
              h ? sqlite3_errmsg(h) : "?");
      sqlite3_close(h); /* sqlite allocates a handle even for a failed open */
      return NULL;
   }
   if (!db_conf(h, worker)) {
      sqlite3_close(h);
      return NULL;
   }
   t_conn = h;
   t_of   = d;
   return h;
}

/* This thread's connection to `d`, opened on first use.
 *
 * A cached connection to a DIFFERENT database is not usable: it would answer
 * a question about one database with another's data. conn_open closes it
 * before opening the one that was asked for -- and if it cannot, says so
 * rather than leaving two. */
static sqlite3 *H(struct db *d)
{
   if (!d || !d->path[0])
      return NULL;
   if (t_conn && t_of == d)
      return t_conn;
   return conn_open(d, 1);
}

/* IS A TRANSACTION ALREADY OPEN ON THIS CONNECTION? See db.h.
 *
 * sqlite has no nested BEGIN: a second `BEGIN IMMEDIATE` on a connection that
 * is already in a transaction fails with "cannot start a transaction within a
 * transaction", so a helper that opens its own is a helper that cannot be
 * called from inside somebody else's. session_new is exactly that case -- the
 * login path calls it with no transaction open, and the invitation path calls
 * it in the middle of one that also creates the account.
 *
 * THE CONNECTION, NOT THE DATABASE: the answer is per-connection and this
 * server's connections are per-thread, which is the only reason it is a
 * usable answer at all. A worker asking about its OWN transaction gets a fact
 * about itself; nobody can be inside another thread's.
 *
 * A connection that cannot be opened reports "no transaction", which is the
 * safe direction -- the caller then tries to BEGIN, and that fails too and is
 * reported. Answering "yes, in one" would have it skip the BEGIN and run its
 * statements loose. */
int db_in_transaction(struct db *d)
{
   sqlite3 *h = H(d);
   return h && !sqlite3_get_autocommit(h);
}

/* ---- DETERMINISTIC FAULTS, for the test build only -------------------
 *
 * NOT COMPILED INTO A SHIPPING BINARY: the whole block is behind -DDB_FAULTS,
 * which only `make srvfault` sets (see the Makefile). It exists because the
 * rule this file enforces -- a statement that did not finish is not a result
 * -- is otherwise only testable through failures that are hard to cause on
 * demand. A busy writer and a corrupted page can be arranged; a prepare that
 * fails, or a COMMIT that fails after every row has been inserted, cannot.
 *
 * Both counters are 1-based and come from the environment:
 *   DB_FAIL_PREPARE_SQL=<substring>  every db_prep whose SQL contains it
 *                                    returns NULL
 *   DB_FAIL_PREPARE=n   the n-th db_prep of the process returns NULL
 *   DB_FAIL_COMMIT=n    the n-th COMMIT fails
 * A COMMIT that fails is the interesting one: every row is in the
 * transaction, the client is about to be told "stored", and the answer has to
 * be that nothing was stored.
 */

int db_exec(struct db *d, const char *sql)
{
   if (fault_commit(sql))
      return 0;
   sqlite3 *h = H(d);
   if (!h) {
      fprintf(stderr, "sync: sql unavailable: database connection failed\n");
      return 0;
   }
   char *e = NULL;
   if (sqlite3_exec(h, sql, NULL, NULL, &e) == SQLITE_OK)
      return 1;
   fprintf(stderr, "sync: sql: %s\n", e ? e : "?");
   sqlite3_free(e);
   return 0;
}

int db_finished(int rc)
{
   if (rc == SQLITE_DONE)
      return 1;
   fprintf(stderr, "sync: query did not complete: %s\n", sqlite3_errstr(rc));
   return 0;
}

const char *db_errmsg(struct db *d)
{
   /* THE CONNECTION AS IT ALREADY IS, never H().
    *
    * H() is not an accessor: it opens a connection that is not there yet, and
    * closes and reopens one that belongs to a different database. Neither
    * belongs in a function whose whole job is to explain a failure that has
    * already happened, and both make it lie:
    *
    *   - the failure being explained is very often "the connection could not
    *     be opened", and calling H() again retries that open. A second
    *     "worker cannot open" line is printed from inside the error reporter,
    *     and if the retry SUCCEEDS the message handed back is the fresh
    *     connection's -- "not an error" -- for a call that certainly did;
    *   - closing this thread's connection to another database, from an error
    *     path, is a side effect no caller of an error message can expect.
    *
    * So the message is whatever THIS thread's connection to THIS database has
    * to say, and if there is no such connection that is itself the answer.
    * Every caller reaches here after a db_prep or a step on the same thread,
    * so the connection it wants is the one already cached.
    *
    * A connection that was never opened has no message of its own, and saying
    * "not an error" -- which is what sqlite3_errmsg(NULL) reports -- would be
    * the most misleading answer available. */
   sqlite3 *h = (d && t_conn && t_of == d) ? t_conn : NULL;
   return h ? sqlite3_errmsg(h) : "no database connection";
}

struct sqlite3_stmt *db_prep(struct db *d, const char *sql)
{
   if (fault_prepare(sql)) {
      fprintf(stderr, "sync: prepare: injected failure\n%s\n", sql);
      return NULL;
   }
   sqlite3 *h = H(d);
   if (!h) {
      fprintf(stderr,
              "sync: prepare unavailable: database connection failed\n");
      return NULL;
   }
   sqlite3_stmt *st = NULL;
   char *wrapped    = fault_step_wrap(h, sql);
   const char *use  = wrapped ? wrapped : sql;
   int rc           = sqlite3_prepare_v2(h, use, -1, &st, NULL);
   if (wrapped)
      sqlite3_free(wrapped);
   if (rc != SQLITE_OK) {
      fprintf(stderr, "sync: prepare: %s\n%s\n", sqlite3_errmsg(h), sql);
      return NULL;
   }
   return st;
}

enum db_get db_get_long(struct db *d, const char *sql, long arg, long *out)
{
   sqlite3_stmt *st = db_prep(d, sql);
   if (!st)
      return DB_GET_FAIL;
   sqlite3_bind_int64(st, 1, arg);
   int rc = sqlite3_step(st);
   long v = 0;
   if (rc == SQLITE_ROW)
      v = (long)sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   if (rc == SQLITE_ROW) {
      if (out)
         *out = v;
      return DB_GET_VALUE;
   }
   /* DONE IS THE ONLY ABSENCE. Every other code -- BUSY, LOCKED, IOERR,
    * CORRUPT, and a NOMEM that sqlite reports here rather than at prepare --
    * means the question was not answered, and reporting it as "no row" is
    * how a storage fault came to be rendered as a fact about the user. */
   return rc == SQLITE_DONE ? DB_GET_NONE : DB_GET_FAIL;
}

/* Rows changed by the most recent statement on THIS thread's connection.
 *
 * For a DELETE, sqlite3_step returns SQLITE_DONE whether it matched a
 * thousand rows or none -- so "the statement ran" and "something was deleted"
 * are different questions, and the pages that report a revocation were
 * answering the first while claiming the second. */
int db_changes(struct db *d)
{
   return sqlite3_changes(H(d));
}

long db_last_id(struct db *d)
{
   return (long)sqlite3_last_insert_rowid(H(d));
}

/* THE SCHEMA, AND HOW IT MOVES.
 *
 * V1 is the baseline below: every statement IF NOT EXISTS, so creating a file
 * and opening an existing one are the same code path. That is enough to ADD a
 * table or an index and no more -- it cannot change a column, a constraint or
 * an index's meaning, because the CREATE it would need is skipped on exactly
 * the databases that need it. A server with no version therefore could not
 * evolve at all, and the first change that needed to would have had to be
 * written as a one-off script somebody remembers to run.
 *
 * So the file carries a VERSION (PRAGMA user_version, which sqlite stores in
 * the header and costs nothing) and this is a list of ordered steps:
 *
 *   - each step runs in ITS OWN TRANSACTION, together with the bump of
 *     user_version, so a failure leaves the database at the version it was
 *     at, not halfway into a step;
 *   - steps run in order, and only the ones this file has not yet had;
 *   - a file from the FUTURE -- a version this build does not know -- is
 *     REFUSED. It was written by a newer server, and an older binary reading
 *     it would be interpreting columns whose meaning it does not have. That
 *     is the case that silently corrupts a record, so it fails to open
 *     instead, which is loud and reversible (run the newer binary).
 *
 * A file that predates versioning reports version 0 and is not empty; step 1
 * is the baseline, and every statement in it is IF NOT EXISTS, so applying it
 * to such a file changes nothing but the version. That is what makes the
 * upgrade from "before this existed" safe.
 */
static const char SCHEMA[] =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS user ("
    "  id INTEGER PRIMARY KEY,"
    "  email TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  pw_salt BLOB NOT NULL, pw_hash BLOB NOT NULL, pw_iters INTEGER NOT NULL,"
    /* minutes east of UTC; NULL means "follow the phone", which the app
     * already records per reading in its tz_off field. The board has no
     * /usr/share/zoneinfo, so a named zone could not be resolved anyway. */
    "  tz_offset INTEGER,"
    "  display_name TEXT,"
    "  created_at INTEGER NOT NULL);"

    /* The ONE paired app. Named `app`, not `device`: a device in this data is
     * a CGM sensor, which is data the app sends, not something that
     * authenticates. Conflating the two is how a sensor id ends up looking
     * like a credential. PRIMARY KEY on user_id is what enforces "one app per
     * user" structurally rather than in a check someone can forget. */
    "CREATE TABLE IF NOT EXISTS app ("
    "  user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,"
    "  key BLOB NOT NULL, label TEXT,"
    "  paired_at INTEGER NOT NULL, last_seen INTEGER);"

    "CREATE TABLE IF NOT EXISTS pairing ("
    "  user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,"
    "  code TEXT NOT NULL, expires_at INTEGER NOT NULL,"
    "  tries INTEGER NOT NULL DEFAULT 0);"

    /* The replica. `line` is part of the primary key, so re-PUTting a row the
     * server already holds is a no-op instead of a duplicate -- the same
     * idempotence store gets from its dedup scan, but as an index lookup
     * rather than a walk of the whole file. */
    "CREATE TABLE IF NOT EXISTS logrow ("
    "  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    "  log TEXT NOT NULL, bucket INTEGER NOT NULL, line TEXT NOT NULL,"
    "  PRIMARY KEY (user_id, log, bucket, line)) WITHOUT ROWID;"

    "CREATE TABLE IF NOT EXISTS share ("
    "  owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    "  viewer_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (owner_id, viewer_id));"
    "CREATE INDEX IF NOT EXISTS share_viewer ON share(viewer_id);"

    /* A share link IS the invitation: whoever opens it either signs in or
     * creates the account on the spot. There is no other way to get an
     * account, which is what "invite only" means here.
     *
     * owner_id is NULLABLE, and that is the difference between the two kinds
     * of link. With an owner it also grants a follow, which is what the web
     * button mints. Without one it is a plain signup link -- what `sync
     * invite` prints -- so the person running the server can hand out
     * accounts without attaching their own data to every one of them. */
    "CREATE TABLE IF NOT EXISTS share_token ("
    "  token TEXT PRIMARY KEY,"
    "  owner_id INTEGER REFERENCES user(id) ON DELETE CASCADE,"
    "  email TEXT, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,"
    "  used_at INTEGER);"

    /* Split-cookie sessions: the selector is the indexed lookup key, and only
     * the HASH of the validator is kept, so a stolen database contains no
     * cookie anyone can present. */
    "CREATE TABLE IF NOT EXISTS session ("
    "  selector TEXT PRIMARY KEY, verifier BLOB NOT NULL,"
    "  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    "  expires_at INTEGER NOT NULL, last_seen INTEGER);"

    "CREATE TABLE IF NOT EXISTS nonce ("
    "  user_id INTEGER NOT NULL, nonce TEXT NOT NULL, seen_at INTEGER NOT NULL,"
    "  PRIMARY KEY (user_id, nonce)) WITHOUT ROWID;"

    "CREATE TABLE IF NOT EXISTS login_fail ("
    "  email TEXT PRIMARY KEY, n INTEGER NOT NULL, first_at INTEGER NOT NULL);";

/* ---- V2: THE TWO LOOKUPS THAT BOUND THE TWO CREDENTIAL TABLES --------
 *
 * `share_token` and `session` are the two tables this server writes a
 * credential into, and until now nothing ever removed a row from either one
 * except the holder of that exact credential coming back for it. Both are now
 * pruned and capped per owner (auth.c: share_token_mint and session_new), and
 * both of those rules are a question about ONE account's rows in a table that
 * holds everybody's.
 *
 * Without these indexes each of those questions is a full scan of the table,
 * which is precisely backwards: the work of keeping a table small would grow
 * with the size of the table, and would be paid on the request path -- every
 * mint and every login. The settings page's own list of live links
 * (settings.c) is the same query with the same scan, and it is rendered on
 * every visit to that page.
 *
 * The trailing column in each is what the rule ORDERS BY, so the index
 * answers "this owner's rows, oldest first" in one seek rather than a scan
 * plus a sort:
 *
 *   share_token(owner_id, created_at)  -- the live-link count, and the
 *                                         settings page's ORDER BY created_at
 *   session(user_id, last_seen)        -- the per-user session cap's
 *                                         ORDER BY last_seen, and the
 *                                         "sign out everywhere" delete
 *
 * NEITHER IS UNIQUE. An owner may hold several live links and several
 * sessions; the caps are enforced by counting, not by a constraint, because
 * the cap is a policy number that has changed once already and a unique index
 * would make changing it a schema migration. */
static const char SCHEMA_V2[] =
    "CREATE INDEX IF NOT EXISTS share_token_owner"
    "  ON share_token(owner_id, created_at);"
    "CREATE INDEX IF NOT EXISTS session_user ON session(user_id, last_seen);";

/* The version this build understands. Bump it in the same commit as the
 * migration that needs it, and never renumber: the number is written into
 * every database this server has ever opened. */
#define DB_SCHEMA_VERSION 2

/* One step. `sql` may hold several statements; they all run in the same
 * transaction as the version bump. */
struct migration {
   int to; /* the version the database is AT once this step commits */
   const char *sql;
};

static const struct migration MIGRATIONS[] = {
    {1, SCHEMA   },
    {2, SCHEMA_V2},
    /* Add the next step here, with `to` one higher, and raise
     * DB_SCHEMA_VERSION to match. Do NOT edit an earlier step: databases in
     * the field have already run it, and changing it makes this list a
     * description of what a fresh install gets rather than a record of how
     * every install arrived. */
};

#define NMIGRATIONS ((int)(sizeof MIGRATIONS / sizeof MIGRATIONS[0]))

/* WHAT EACH TABLE MUST LOOK LIKE: column names and declared types in order,
 * and every OTHER property of the canonical schema that anything depends on --
 * the NOT NULL and primary-key marks, the DEFAULTs, the collating sequences,
 * the foreign keys with their referential actions, and whether the table is
 * WITHOUT ROWID.
 *
 * The list grew because "the right columns" turned out to be a very weak
 * statement about a database. A file can match it column for column and still
 * hold `user.email` under BINARY (two spellings of one address are then two
 * accounts) or a `session` table with no cascade (deleting the account leaves
 * its live cookies behind). Neither shows up in table_info, and neither fails
 * a query -- they just make the server quietly mean something else.
 *
 * The baseline step is CREATE TABLE IF NOT EXISTS, which is what makes
 * creating and opening one code path -- and also what makes it BLIND: a file
 * holding a table of the same name with different columns is left exactly as
 * it is, and the version is stamped over it as though it were current. Every
 * query then fails at runtime, one page at a time, on a database the server
 * has just declared up to date.
 *
 * So the shape is checked rather than assumed. This list is the schema above,
 * as sqlite reports it back; when a migration changes a table, this changes
 * with it, in the same commit. */
struct table_shape {
   const char *name;
   /* "name TYPE[!][#n][=default][~COLL]" per column, in declaration order:
    * `!` is NOT NULL, `#n` is its position in the PRIMARY KEY, `=x` is its
    * DEFAULT as sqlite records it, and `~COLL` is its collating sequence when
    * that is not the BINARY every column gets by default.
    *
    * THE CONSTRAINTS ARE THE POINT, not decoration. Three mechanisms here are
    * load-bearing on them, and each fails SILENTLY without:
    *   - replay protection is `INSERT INTO nonce` relying on the primary key
    *     to collide (auth.c). No key, no replay detection.
    *   - bucket idempotence is `INSERT OR IGNORE INTO logrow` relying on the
    *     composite key (logs.c). No key, duplicate rows.
    *   - `ON CONFLICT(user_id)` / `ON CONFLICT(email)` (pair.c, auth.c) need
    *     a matching unique constraint, or the statement is a runtime error.
    * Comparing only names and types accepted a database with all of them
    * stripped, stamped it current, and let `adduser` write a user with a
    * NULL id.
    *
    * AND SO IS THE COLLATION, for exactly one column and for the worst
    * available reason. `user.email` is NOCASE, and every authorisation
    * decision in this server begins with a user id resolved from an email
    * (auth.c user_by_email: `SELECT id FROM user WHERE email=?`, which
    * compares under the COLUMN's collation). A file whose email column is
    * BINARY instead does not fail: it quietly makes `jk@example.com` and
    * `JK@example.com` two different accounts, so an invitation addressed to
    * one can be redeemed into the other, and a password reset can land on a
    * row its owner never signed in to. That is an account-boundary bug, not a
    * tidiness one, and nothing in the running server would report it.
    *
    * The other direction matters too, which is why every column's collation is
    * compared rather than only the one that must be NOCASE. A stray NOCASE on
    * `logrow.line` -- part of that table's primary key -- makes two rows whose
    * text differs only in case COLLIDE, and `INSERT OR IGNORE INTO logrow`
    * then discards the second one. That is silent data loss on the copy that
    * is supposed to be the backup. */
   const char *cols;
   /* THE FOREIGN KEYS, "from->table.to del=X upd=Y" per key, sorted; "" for a
    * table that must have none.
    *
    * WHAT A DATABASE WITHOUT THEM SILENTLY PERMITS: `/settings/delete` is one
    * `DELETE FROM user`, and everything else that belongs to that account --
    * their sessions, their share tokens, their paired app, their pairing code,
    * and every synced log row -- is removed by ON DELETE CASCADE and by
    * nothing else. A file whose cascades were lost (a hand-rebuilt table, a
    * dump reloaded without them) turns account deletion into a no-op that
    * reports success: the rows stay, a session cookie issued before the
    * deletion still resolves to a user id, and the id is eventually reused by
    * the next account, which then inherits the deleted user's readings.
    *
    * "" IS A CLAIM, NOT AN OMISSION. `nonce` and `login_fail` are listed as
    * having no foreign key on purpose (see the schema), and a file that has
    * grown one is refused just as loudly as a file that has lost one: an
    * unexpected cascade deletes rows this server expects to keep, and an
    * unexpected RESTRICT makes a delete this server expects to succeed fail. */
   const char *fks;
   /* 1 for an ordinary rowid table, 0 for WITHOUT ROWID.
    *
    * `logrow` and `nonce` are WITHOUT ROWID because they ARE their primary
    * key: every column of logrow is in it, so a rowid table stores the whole
    * table twice -- once in the heap and once in the automatic unique index
    * that enforces the key. On a board with about nineteen megabytes free and
    * logrow the largest table by far, that is not a micro-optimisation.
    *
    * It is checked rather than assumed because it is INVISIBLE to every other
    * check here: a rowid `logrow` reports byte-identical `table_info` and an
    * automatic index over the same four columns, so a file that has silently
    * doubled the size of its biggest table would otherwise be stamped current
    * and fill the card months later, at a moment unconnected to the change. */
   int rowid;
};

/* One table's indexes, as a set: see INDEXES below. */
struct table_index {
   const char *table;
   const char *idx;
};

static const struct table_shape SHAPES[] = {
    {"user",
     "id INTEGER#1,email TEXT!~NOCASE,pw_salt BLOB!,pw_hash BLOB!,pw_iters "
     "INTEGER!,tz_offset INTEGER,display_name TEXT,created_at INTEGER!",             "",                                            1},
    {"app",
     "user_id INTEGER#1,key BLOB!,label TEXT,paired_at INTEGER!,last_seen "
     "INTEGER",                                                                      "user_id->user.id del=CASCADE upd=NO ACTION",  1},
    /* `tries INTEGER NOT NULL DEFAULT 0` -- the default is checked because it
     * is the difference between "a pairing row inserted without a try count
     * starts at zero" and "that INSERT fails on the NOT NULL". Today every
     * INSERT in pair.c names the column, so nothing depends on it at runtime;
     * it is pinned so that the file and the schema this build believes it has
     * cannot drift apart unnoticed, and so a later statement that omits the
     * column meets the default the schema promises. */
    {"pairing",
     "user_id INTEGER#1,code TEXT!,expires_at INTEGER!,tries INTEGER!=0",            "user_id->user.id del=CASCADE upd=NO ACTION",  1},
    /* `line` is the row's TEXT, not a line number: a file whose logrow says
     * INTEGER there is the incompatible shape this check exists for. */
    {"logrow",      "user_id INTEGER!#1,log TEXT!#2,bucket INTEGER!#3,line TEXT!#4",
     "user_id->user.id del=CASCADE upd=NO ACTION",                                                                                  0},
    {"share",       "owner_id INTEGER!#1,viewer_id INTEGER!#2,created_at INTEGER!",
     "owner_id->user.id del=CASCADE upd=NO ACTION,"
     "viewer_id->user.id del=CASCADE upd=NO ACTION",                                                                                1},
    {"share_token",
     "token TEXT#1,owner_id INTEGER,email TEXT,created_at INTEGER!,expires_at "
     "INTEGER!,used_at INTEGER",                                                     "owner_id->user.id del=CASCADE upd=NO ACTION", 1},
    {"session",
     "selector TEXT#1,verifier BLOB!,user_id INTEGER!,expires_at "
     "INTEGER!,last_seen INTEGER",                                                   "user_id->user.id del=CASCADE upd=NO ACTION",  1},
    /* NO FOREIGN KEY, deliberately. A nonce row is spent-token evidence: it
     * exists to make a replayed request collide, and it is swept by age
     * (auth.c), not by ownership. Keeping it out of the cascade also keeps the
     * replay window closed across an account being deleted and recreated with
     * the same id. The empty string SAYS that, so a file that has grown a
     * cascade here -- which would silently reopen that window -- is refused. */
    {"nonce",       "user_id INTEGER!#1,nonce TEXT!#2,seen_at INTEGER!",             "",                                            0},
    /* Also no foreign key: this throttles attempts against an email ADDRESS,
     * which very often is not an account at all -- that is the whole point of
     * throttling it. A REFERENCES here would make every failed login against a
     * non-existent address a constraint error instead of a recorded attempt. */
    {"login_fail",  "email TEXT#1,n INTEGER!,first_at INTEGER!",                     "",                                            1},
};

/* The indexes, which carry the UNIQUE constraints -- sqlite names an implicit
 * one sqlite_autoindex_<table>_<n> -- as well as the one this schema creates
 * by hand. "name(u|n[p]):col[~COLL],col" per index, sorted by name: `u` is
 * UNIQUE and `n` is not, a trailing `p` marks a PARTIAL index, and `~COLL` is
 * the collating sequence of that column WITHIN THE INDEX when it is not
 * BINARY.
 *
 * THE INDEX'S COLLATION IS A SEPARATE FACT FROM THE COLUMN'S, and both are
 * checked because either one alone can be wrong:
 *
 *   - `CREATE UNIQUE INDEX ... ON user(email COLLATE BINARY)` over a NOCASE
 *     column enforces uniqueness case-SENSITIVELY. The column check passes,
 *     `SELECT ... WHERE email=?` still matches case-insensitively, and the one
 *     thing standing between two spellings of one address and two accounts --
 *     the unique index -- has quietly stopped doing it;
 *   - a NOCASE index over a BINARY column is the mirror image: the lookup in
 *     auth.c misses the row (it compares under the COLUMN's collation) while
 *     the INSERT that would create the second account is refused by the index,
 *     so the user is told their password is wrong and cannot sign up either.
 *
 * A PARTIAL index is the same failure wearing a different hat: `... ON
 * user(email) WHERE id > 100` is unique, is named what the schema expects, and
 * covers the right column, while enforcing nothing at all on the rows outside
 * its WHERE. `partial` is the only column of PRAGMA index_list that reports
 * it, so without this it is invisible.
 *
 * The index's SORT ORDER (index_xinfo's `desc`) is deliberately NOT compared:
 * a descending index enforces exactly the same uniqueness and answers exactly
 * the same lookups, so a difference there is not a difference this server can
 * observe. `origin` is likewise left alone -- ON CONFLICT is satisfied by a
 * unique index however it came to exist.
 *
 * app, pairing, logrow and nonce have no entry: their primary key IS the
 * table (INTEGER PRIMARY KEY, or WITHOUT ROWID), so sqlite builds no separate
 * index for it. The `#n` marks in SHAPES, and the WITHOUT ROWID flag, are what
 * say so for those. */
static const struct table_index INDEXES[] = {
    {"user",        "sqlite_autoindex_user_1(u):email~NOCASE"                   },
    /* session_user is the v2 step's index (see SCHEMA_V2): it is what makes
     * the per-user session cap and "sign out everywhere" a seek instead of a
     * scan of every account's cookies. A file that has lost it still ANSWERS
     * every one of those queries, which is exactly why it is pinned here --
     * the only symptom would be a login that gets slower as the table grows,
     * on a board where that is measured in seconds. */
    {"session",     "session_user(n):user_id,last_seen,"
                "sqlite_autoindex_session_1(u):selector"         },
    {"share",       "share_viewer(n):viewer_id,sqlite_autoindex_share_1(u):owner_id,"
              "viewer_id"                                          },
    {"share_token", "share_token_owner(n):owner_id,created_at,"
                    "sqlite_autoindex_share_token_1(u):token"},
    {"login_fail",  "sqlite_autoindex_login_fail_1(u):email"                    },
};

#define NINDEXES ((int)(sizeof INDEXES / sizeof INDEXES[0]))

#define NSHAPES ((int)(sizeof SHAPES / sizeof SHAPES[0]))

static int table_exists(sqlite3 *h, const char *name)
{
   sqlite3_stmt *st = NULL;
   /* COLLATE NOCASE: sqlite table names are case-insensitive, so a legacy
    * `LOGROW` IS the logrow table -- reported missing by a binary compare,
    * after which the baseline would skip it and the after-check would blame
    * the step for a table that was there all along. */
   if (sqlite3_prepare_v2(h,
                          "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                          "name=? COLLATE NOCASE",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
   int rc  = sqlite3_step(st);
   int yes = rc == SQLITE_ROW;
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      return -1;
   return yes;
}

/* THE COLLATING SEQUENCE OF ONE COLUMN, which no PRAGMA reports.
 *
 * table_info gives name, type, NOT NULL, default and primary key -- and not
 * the collation, which is the one property of `user.email` that decides
 * whether two spellings of an address are one account or two. index_xinfo
 * reports the collation an INDEX uses, which is a different fact (see
 * INDEXES), and only for indexed columns.
 *
 * sqlite3_table_column_metadata is the only interface that answers it.
 * SQLITE_ENABLE_COLUMN_METADATA is NOT set in this build and does not need to
 * be: that option gates the sqlite3_column_*_name family of prepared-statement
 * accessors, and this function is compiled unconditionally (sqlite 3.46).
 *
 * NULL on failure, and "BINARY" for a column that names no collation -- which
 * is what sqlite itself reports, so the two are the same answer. The string
 * belongs to sqlite; do not keep it past the next call on this connection. */
static const char *col_collation(sqlite3 *h, const char *table, const char *col)
{
   const char *coll = NULL;
   if (sqlite3_table_column_metadata(h, "main", table, col, NULL, &coll, NULL,
                                     NULL, NULL) != SQLITE_OK)
      return NULL;
   return coll ? coll : "BINARY";
}

/* Does this table have exactly the columns, in order, with the declared types
 * `want` names? 1 yes, 0 no, -1 the question could not be asked. */
static int shape_ok(sqlite3 *h, const char *table, const char *want)
{
   char sql[128];
   if (snprintf(sql, sizeof sql, "PRAGMA table_info(%s);", table) >=
       (int)sizeof sql)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   char got[768];
   size_t n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *cn = (const char *)sqlite3_column_text(st, 1);
      const char *ct = (const char *)sqlite3_column_text(st, 2);
      /* ...with its constraints: `!` NOT NULL, `#n` primary-key position,
       * `=x` DEFAULT, `~COLL` a collation that is not BINARY. See the note on
       * struct table_shape for what depends on each of them. */
      int notnull = sqlite3_column_int(st, 3);
      /* A COLUMN WITH NO DEFAULT AND A COLUMN WHOSE DEFAULT IS NULL ARE
       * DIFFERENT, and sqlite distinguishes them here: no default is SQL NULL
       * (a NULL pointer from column_text), `DEFAULT NULL` is the four-letter
       * string. Only the second gets a `=` mark. */
      const char *dflt = (const char *)sqlite3_column_text(st, 4);
      int pk           = sqlite3_column_int(st, 5);
      /* The collation is asked for by NAME, so it has to be asked while the
       * name is still ours -- column_text's buffer is invalidated by the next
       * step on this statement, and metadata runs its own query. Copy first. */
      char name[64];
      (void)snprintf(name, sizeof name, "%s", cn ? cn : "?");
      const char *coll = col_collation(h, table, name);
      if (!coll) {
         sqlite3_finalize(st);
         return -1;
      }
      char mark[96];
      int mn = 0;
      if (notnull)
         mark[mn++] = '!';
      if (pk)
         mn += snprintf(mark + mn, sizeof mark - (size_t)mn, "#%d", pk);
      if (dflt)
         mn += snprintf(mark + mn, sizeof mark - (size_t)mn, "=%s", dflt);
      if (sqlite3_stricmp(coll, "BINARY") != 0)
         mn += snprintf(mark + mn, sizeof mark - (size_t)mn, "~%s", coll);
      if (mn < 0 || (size_t)mn >= sizeof mark) {
         sqlite3_finalize(st);
         return -1;
      }
      mark[mn] = '\0';
      int k = snprintf(got + n, sizeof got - n, "%s%s %s%s", n ? "," : "", name,
                       ct ? ct : "?", mark);
      if (k < 0 || (size_t)k >= sizeof got - n) {
         sqlite3_finalize(st);
         return -1;
      }
      n += (size_t)k;
   }
   sqlite3_finalize(st);
   /* A COLUMN SCAN THAT STOPPED IS NOT A COLUMN LIST. Anything but DONE --
    * BUSY, IOERR, CORRUPT -- ends this loop looking exactly like "the table
    * has no more columns", and a table truncated to its first few columns can
    * still compare equal to a shorter expectation. db_finished names the real
    * result on the way out. */
   if (!db_finished(rc))
      return -1;
   got[n] = '\0';
   return strcmp(got, want) == 0;
}

/* THE FOREIGN KEYS ON ONE TABLE, as a sorted set, compared with what this
 * schema declares. 1 match, 0 mismatch, -1 could not ask.
 *
 * Sorted because PRAGMA foreign_key_list's order is sqlite's business and not
 * a fact this schema states -- `share` declares owner_id before viewer_id and
 * is reported viewer_id first.
 *
 * The `match` column is not compared: sqlite parses MATCH clauses and ignores
 * them, so it is always NONE and a difference there could not mean anything. */
static int fks_ok(sqlite3 *h, const char *table, const char *want)
{
   char sql[128];
   if (snprintf(sql, sizeof sql, "PRAGMA foreign_key_list(%s);", table) >=
       (int)sizeof sql)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   /* MORE KEYS THAN THE SLOTS HOLD IS A REFUSAL, not a truncation, for the
    * same reason indexes_ok refuses: a shape check that passes because it ran
    * out of room is the false green this whole gate exists against. This
    * schema's widest table has two. */
   char keys[8][128];
   int nk = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      if (nk >= (int)(sizeof keys / sizeof keys[0])) {
         sqlite3_finalize(st);
         return -1;
      }
      const char *parent = (const char *)sqlite3_column_text(st, 2);
      const char *from   = (const char *)sqlite3_column_text(st, 3);
      const char *to     = (const char *)sqlite3_column_text(st, 4);
      const char *upd    = (const char *)sqlite3_column_text(st, 5);
      const char *del    = (const char *)sqlite3_column_text(st, 6);
      /* `to` IS NULL for `REFERENCES user` without a column, which means the
       * parent's primary key. This schema always names the column, so a NULL
       * here is a shape this build does not write -- spell it so it cannot
       * compare equal to anything expected. */
      int k = snprintf(keys[nk], sizeof keys[nk], "%s->%s.%s del=%s upd=%s",
                       from ? from : "?", parent ? parent : "?",
                       to ? to : "(pk)", del ? del : "?", upd ? upd : "?");
      if (k < 0 || (size_t)k >= sizeof keys[nk]) {
         sqlite3_finalize(st);
         return -1;
      }
      nk++;
   }
   sqlite3_finalize(st);
   /* A SCAN THAT STOPPED IS NOT A SCAN THAT FINISHED. A BUSY or an IOERR
    * partway through would otherwise look like "this table has fewer foreign
    * keys than it does", which for the empty expectations below reads as a
    * perfect match -- a table whose cascades could not be read would be
    * accepted as a table that correctly has none. */
   if (!db_finished(rc))
      return -1;
   for (int i = 1; i < nk; i++) {
      char t[sizeof keys[0]];
      memcpy(t, keys[i], sizeof t);
      int j = i - 1;
      while (j >= 0 && strcmp(keys[j], t) > 0) {
         memcpy(keys[j + 1], keys[j], sizeof t);
         j--;
      }
      memcpy(keys[j + 1], t, sizeof t);
   }
   char got[512];
   size_t n = 0;
   for (int i = 0; i < nk; i++) {
      int k = snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", keys[i]);
      if (k < 0 || (size_t)k >= sizeof got - n)
         return -1;
      n += (size_t)k;
   }
   got[n] = '\0';
   return strcmp(got, want) == 0;
}

/* Is this an ordinary rowid table (1) or a WITHOUT ROWID one (0)? -1 could
 * not ask.
 *
 * There is no pragma for it, and sqlite_master's SQL text is the wrong thing
 * to parse (a comment or a different spelling of the same table would fool
 * it). What the property IS, operationally, is whether the table has a rowid,
 * so that is what gets asked: `rowid` resolves on a rowid table and is "no
 * such column" on a WITHOUT ROWID one. LIMIT 0 so nothing is read; in fact the
 * statement is never even stepped, because PREPARE is where the name is
 * resolved.
 *
 * This would misread a table that had a REAL column called `rowid`. No table
 * in this schema does, and shape_ok has already compared every column name by
 * the time anybody acts on this answer. It would likewise misread a table that
 * is not there at all ("no such table" is the same SQLITE_ERROR), so the only
 * caller asks table_exists first. */
static int rowid_ok(sqlite3 *h, const char *table)
{
   char sql[128];
   if (snprintf(sql, sizeof sql, "SELECT rowid FROM %s LIMIT 0;", table) >=
       (int)sizeof sql)
      return -1;
   sqlite3_stmt *st = NULL;
   int rc           = sqlite3_prepare_v2(h, sql, -1, &st, NULL);
   sqlite3_finalize(st);
   if (rc == SQLITE_OK)
      return 1;
   if (rc == SQLITE_ERROR)
      return 0; /* "no such column: rowid" -- a WITHOUT ROWID table */
   return -1;   /* NOMEM, BUSY on the schema read, a corrupt header */
}

/* The indexes on one table as "name(u|n):col,col" joined by commas and sorted
 * by name, compared against what this schema builds. 1 match, 0 mismatch, -1
 * could not ask.
 *
 * This is where the UNIQUE constraints are: sqlite implements one as an
 * index it names sqlite_autoindex_<table>_<n>, so an ON CONFLICT target that
 * has quietly lost its UNIQUE shows up here and nowhere else. */
static int indexes_ok(sqlite3 *h, const char *table, const char *want)
{
   char sql[128];
   if (snprintf(sql, sizeof sql, "PRAGMA index_list(%s);", table) >=
       (int)sizeof sql)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   char names[8][64];
   int uniq[8];
   int part[8];
   int nidx = 0;
   /* THE TERMINAL RESULT IS KEPT, and this comment used to say the opposite.
    *
    * It claimed the loop "either reads every index or stops, and a partial
    * read is caught by the OVERFLOW test below". The overflow test catches
    * TOO MANY rows. It cannot catch too FEW, which is what every failure
    * mode of sqlite3_step actually produces: SQLITE_BUSY (another connection
    * holds the write lock), SQLITE_IOERR (the SD card), SQLITE_CORRUPT (a
    * damaged page). Written as `while (step == ROW)`, all three are
    * indistinguishable from "there are no more indexes", so a scan that
    * broke after the expected prefix compared EQUAL to the expected string
    * and reported the schema as matching.
    *
    * That is the worst outcome this gate has available: a database whose
    * shape could not be read is stamped as this build's and opened, and the
    * refusal that should have happened is spent instead on the next start,
    * or never. db.h states the rule for the server's read loops; it applies
    * with more force here, where the thing being read is what decides
    * whether the file may be used at all. */
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      /* MORE INDEXES THAN THE SLOTS HOLD IS A REFUSAL, not a truncation.
       * Stopping at eight and comparing what fitted would report a table as
       * matching its expected shape while an index this function never saw
       * was missing from it -- a shape check that passes because it ran out
       * of room is the false green this whole schema gate exists against. */
      if (nidx >= (int)(sizeof names / sizeof names[0])) {
         sqlite3_finalize(st);
         return -1;
      }
      const char *nm = (const char *)sqlite3_column_text(st, 1);
      snprintf(names[nidx], sizeof names[nidx], "%s", nm ? nm : "?");
      uniq[nidx] = sqlite3_column_int(st, 2);
      /* PARTIAL: an index with a WHERE clause enforces its uniqueness only on
       * the rows that clause selects, and is otherwise indistinguishable from
       * the real one -- same name, same column, same unique flag. See the note
       * on INDEXES for what that permits on `user(email)`. */
      part[nidx] = sqlite3_column_int(st, 4);
      nidx++;
   }
   sqlite3_finalize(st);
   if (!db_finished(rc))
      return -1;
   /* SORTED BY NAME, because PRAGMA index_list's order is sqlite's business
    * and not a fact this schema states. */
   for (int i = 1; i < nidx; i++) {
      /* memcpy, not snprintf: these are fixed-size slots being SHUFFLED, and
       * a formatted copy of a same-sized buffer is a truncation the compiler
       * is right to warn about. */
      char tn[sizeof names[0]];
      memcpy(tn, names[i], sizeof tn);
      int tu = uniq[i];
      int tp = part[i];
      int j  = i - 1;
      while (j >= 0 && strcmp(names[j], tn) > 0) {
         memcpy(names[j + 1], names[j], sizeof tn);
         uniq[j + 1] = uniq[j];
         part[j + 1] = part[j];
         j--;
      }
      memcpy(names[j + 1], tn, sizeof tn);
      uniq[j + 1] = tu;
      part[j + 1] = tp;
   }
   char got[512];
   size_t n = 0;
   for (int i = 0; i < nidx; i++) {
      int k = snprintf(got + n, sizeof got - n, "%s%s(%c%s):", n ? "," : "",
                       names[i], uniq[i] ? 'u' : 'n', part[i] ? "p" : "");
      if (k < 0 || (size_t)k >= sizeof got - n)
         return -1;
      n += (size_t)k;
      /* index_xinfo, NOT index_info: the extended form is the only one that
       * reports the COLLATING SEQUENCE each column is indexed under, which is
       * where "two spellings of one address are one account" is decided. It
       * also lists the index's auxiliary columns -- the rowid on an ordinary
       * table, the remaining table columns on a WITHOUT ROWID one -- which are
       * sqlite's bookkeeping rather than anything this schema states, so only
       * the KEY columns (column 5) are compared. */
      char isql[160];
      if (snprintf(isql, sizeof isql, "PRAGMA index_xinfo(%s);", names[i]) >=
          (int)sizeof isql)
         return -1;
      sqlite3_stmt *ist = NULL;
      if (sqlite3_prepare_v2(h, isql, -1, &ist, NULL) != SQLITE_OK)
         return -1;
      int first = 1;
      int irc;
      /* ...and the same rule for the INNER scan, which is the easier one to
       * overlook and the more dangerous one to get wrong: an index whose
       * column list stops early after the first of two key columns reads as a
       * one-column index, and a one-column unique index over `owner_id` is a
       * very different promise from a two-column one over (owner_id,
       * viewer_id). */
      while ((irc = sqlite3_step(ist)) == SQLITE_ROW) {
         if (!sqlite3_column_int(ist, 5))
            continue; /* auxiliary, not part of the key */
         const char *cn   = (const char *)sqlite3_column_text(ist, 2);
         const char *coll = (const char *)sqlite3_column_text(ist, 4);
         const char *cs   = coll ? coll : "?";
         int binary       = sqlite3_stricmp(cs, "BINARY") == 0;
         k     = snprintf(got + n, sizeof got - n, "%s%s%s%s", first ? "" : ",",
                          cn ? cn : "?", binary ? "" : "~", binary ? "" : cs);
         first = 0;
         if (k < 0 || (size_t)k >= sizeof got - n) {
            sqlite3_finalize(ist);
            return -1;
         }
         n += (size_t)k;
      }
      sqlite3_finalize(ist);
      if (!db_finished(irc))
         return -1;
   }
   got[n] = '\0';
   return strcmp(got, want) == 0;
}

/* Every table this build knows: present and the right shape, or not present
 * at all (the baseline is about to create it).
 *
 * `after` says which side of the migration we are on -- before it, a missing
 * table is ordinary; after it, a missing table means the step did not do what
 * it said. */
static int schema_shapes_ok(sqlite3 *h, const char *path, int after)
{
   for (int i = 0; i < NSHAPES; i++) {
      int have = table_exists(h, SHAPES[i].name);
      if (have < 0) {
         fprintf(stderr, "sync: %s: cannot ask whether `%s` exists: %s\n", path,
                 SHAPES[i].name, sqlite3_errmsg(h));
         return 0;
      }
      if (!have) {
         if (!after)
            continue; /* the baseline will create it */
         fprintf(stderr,
                 "sync: %s has no `%s` table, and no schema step made one.\n"
                 "  The baseline creates every table it knows, so this is a "
                 "database something else\n"
                 "  has changed underneath. Restore a backup.\n",
                 path, SHAPES[i].name);
         return 0;
      }
      int ok = shape_ok(h, SHAPES[i].name, SHAPES[i].cols);
      if (ok < 0) {
         fprintf(stderr, "sync: %s: cannot read the shape of `%s`\n", path,
                 SHAPES[i].name);
         return 0;
      }
      if (ok == 0) {
         fprintf(
             stderr,
             "sync: %s holds a `%s` table this build does not understand.\n"
             "  Expected: %s\n"
             "  (`!` NOT NULL, `#n` primary-key position, `=x` DEFAULT, "
             "`~C` collating sequence.)\n"
             "  Refusing to open it: CREATE TABLE IF NOT EXISTS would leave "
             "that table exactly as it is\n"
             "  and stamp the file as current, after which every query "
             "against it fails one page at a time.\n"
             "  Restore a backup, or migrate the table deliberately.\n",
             path, SHAPES[i].name, SHAPES[i].cols);
         return 0;
      }
      /* ---- THE PROPERTIES table_info DOES NOT REPORT ------------------
       *
       * A table can have every column, in order, with every type, NOT NULL
       * and primary-key mark this build expects, and still be the wrong
       * table: the cascades that make account deletion a deletion, and the
       * rowid decision that makes logrow one copy of the data instead of
       * two, are invisible to the comparison above. Both are asked for
       * explicitly, per table, here. */
      int fk = fks_ok(h, SHAPES[i].name, SHAPES[i].fks);
      if (fk < 0) {
         fprintf(stderr, "sync: %s: cannot read the foreign keys of `%s`: %s\n",
                 path, SHAPES[i].name, sqlite3_errmsg(h));
         return 0;
      }
      if (!fk) {
         fprintf(stderr,
                 "sync: %s: the foreign keys on `%s` are not this schema's.\n"
                 "  Expected: %s\n"
                 "  Refusing to open it. Deleting an account is one `DELETE "
                 "FROM user`, and every row\n"
                 "  that belongs to it -- sessions, share tokens, the paired "
                 "app, the pairing code, every\n"
                 "  synced log row -- goes with it by ON DELETE CASCADE and by "
                 "nothing else. Without the\n"
                 "  cascade, /settings/delete reports success and deletes "
                 "almost nothing, and the freed\n"
                 "  user id is reused by the next account, which inherits "
                 "those rows.\n"
                 "  WHAT TO DO: this cannot be repaired in place -- sqlite "
                 "cannot add a foreign key to an\n"
                 "  existing table. Restore a backup, or rebuild the table "
                 "deliberately (CREATE a correct\n"
                 "  one, INSERT SELECT into it, DROP and rename) with the "
                 "server stopped.\n",
                 path, SHAPES[i].name,
                 SHAPES[i].fks[0] ? SHAPES[i].fks : "(none)");
         return 0;
      }
      int rid = rowid_ok(h, SHAPES[i].name);
      if (rid < 0) {
         fprintf(stderr, "sync: %s: cannot tell whether `%s` has a rowid: %s\n",
                 path, SHAPES[i].name, sqlite3_errmsg(h));
         return 0;
      }
      if (rid != SHAPES[i].rowid) {
         fprintf(stderr,
                 "sync: %s: `%s` is %s and this schema declares it %s.\n"
                 "  Refusing to open it. WITHOUT ROWID is how `%s` stores its "
                 "rows once instead of\n"
                 "  twice -- every column of it is in the primary key, so an "
                 "ordinary table keeps a\n"
                 "  second full copy in the automatic index that enforces "
                 "that key. On a board with\n"
                 "  about nineteen megabytes free that is the difference "
                 "between fitting and not,\n"
                 "  and it would show up as a full card months after the "
                 "change that caused it.\n"
                 "  WHAT TO DO: this cannot be altered in place either. "
                 "Restore a backup, or rebuild\n"
                 "  the table deliberately with the server stopped.\n",
                 path, SHAPES[i].name,
                 rid ? "an ordinary rowid table" : "WITHOUT ROWID",
                 SHAPES[i].rowid ? "an ordinary rowid table" : "WITHOUT ROWID",
                 SHAPES[i].name);
         return 0;
      }
   }
   /* THE INDEXES, after the step that creates them. A unique index on the
    * wrong column refuses rows the server should accept; a missing one
    * silently removes the collision every ON CONFLICT and the replay check
    * depend on. */
   if (!after)
      return 1;
   for (int i = 0; i < NINDEXES; i++) {
      int ok = indexes_ok(h, INDEXES[i].table, INDEXES[i].idx);
      if (ok == 1)
         continue;
      fprintf(stderr,
              "sync: %s: the indexes on `%s` are not this schema's.\n"
              "  Expected: %s\n"
              "  (`u`/`n` UNIQUE or not, a trailing `p` PARTIAL, `~C` the "
              "collating sequence.)\n"
              "  A UNIQUE index the server does not expect refuses rows it "
              "should accept; a missing\n"
              "  one removes the collision ON CONFLICT and the nonce replay "
              "check rely on. A unique\n"
              "  index that is PARTIAL, or that indexes `user.email` under "
              "BINARY rather than NOCASE,\n"
              "  enforces nothing on the rows it does not cover -- which is "
              "how one address in two\n"
              "  spellings becomes two accounts.\n"
              "  WHAT TO DO: an index, unlike a constraint, CAN be rebuilt in "
              "place. With the server\n"
              "  stopped, DROP the offending index and re-create it as the "
              "line above spells it; if it\n"
              "  is a sqlite_autoindex_* (one sqlite made for a UNIQUE or "
              "PRIMARY KEY constraint) it\n"
              "  cannot be dropped, and the table has to be rebuilt or a "
              "backup restored.\n",
              path, INDEXES[i].table, INDEXES[i].idx);
      return 0;
   }
   return 1;
}

/* ---- ROWS THAT POINT AT NOTHING --------------------------------------
 *
 * The cascades above are a property of the SCHEMA; this is a property of the
 * DATA, and the two come apart in exactly one situation. sqlite defaults
 * PRAGMA foreign_keys to OFF per connection -- db_conf turns it on for every
 * connection this server opens, and refuses to hand back a connection where
 * it could not be turned on -- so no row this server has ever written can be
 * an orphan. A file written by something else can be full of them: a dump
 * reloaded with the pragma at its default, a table hand-repaired after a
 * corruption, a partially applied migration.
 *
 * What an orphan row IS, here: a logrow, session, share or share_token whose
 * user no longer exists. The pages join through user_id, so such rows are
 * mostly invisible -- until an id is reused by the next account created, at
 * which point that account silently inherits a stranger's readings. A session
 * row is worse: it is a live cookie for a user that is gone.
 *
 * ONLY AT VERSION ZERO, deliberately. This scans every child table once, and
 * a database this build has already stamped cannot have acquired an orphan
 * since (see the pragma above) -- so paying for it on every start would buy
 * nothing. A version-0 file is precisely the one that came from somewhere
 * else, and this is the moment before it is adopted. */
static int no_orphan_rows(sqlite3 *h, const char *path)
{
   sqlite3_stmt *st = NULL;
   if (!h || sqlite3_prepare_v2(h, "PRAGMA foreign_key_check;", -1, &st,
                                NULL) != SQLITE_OK) {
      fprintf(stderr, "sync: %s: cannot check for orphan rows: %s\n", path,
              h ? sqlite3_errmsg(h) : "no connection");
      sqlite3_finalize(st);
      return 0;
   }
   int bad = 0;
   int rc;
   /* Each row is one violation: the child table, its rowid, the parent table,
    * and which foreign key. A handful of examples is enough to act on; the
    * count is what says how big the problem is. */
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      if (bad < 5) {
         const char *child  = (const char *)sqlite3_column_text(st, 0);
         const char *parent = (const char *)sqlite3_column_text(st, 2);
         fprintf(stderr,
                 "sync: %s: orphan row in `%s`: it references a `%s` that does "
                 "not exist\n",
                 path, child ? child : "?", parent ? parent : "?");
      }
      bad++;
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      /* A SCAN THAT STOPPED IS NOT A CLEAN ONE. Reporting "no orphans"
       * because the read hit BUSY or a damaged page is the same false green
       * the shape gate exists against. */
      fprintf(stderr,
              "sync: %s: the orphan-row check did not finish (%s).\n"
              "  Refusing to open it: a check that stopped early cannot say "
              "the file is clean.\n",
              path, sqlite3_errstr(rc));
      return 0;
   }
   if (!bad)
      return 1;
   fprintf(stderr,
           "sync: %s holds %d row%s whose owner does not exist.\n"
           "  Refusing to adopt it. This file was written with foreign keys "
           "unenforced (sqlite\n"
           "  defaults PRAGMA foreign_keys to OFF, and this server turns it on "
           "for every connection\n"
           "  it opens), so rows outlived the accounts they belong to. Once "
           "the freed user id is\n"
           "  reused by the next account, that account inherits them; an "
           "orphan `session` row is a\n"
           "  live cookie for a user who is gone.\n"
           "  WHAT TO DO: with the server stopped, open the file with the "
           "sqlite3 shell, run\n"
           "  `PRAGMA foreign_key_check;` for the full list, and DELETE the "
           "rows it names -- or\n"
           "  restore a backup taken before whatever produced them. Do not "
           "simply re-stamp the\n"
           "  version: the rows do not become owned by anybody when the "
           "schema says they should be.\n",
           path, bad, bad == 1 ? "" : "s");
   return 0;
}

static int schema_version(sqlite3 *h, int *out)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(h, "PRAGMA user_version;", -1, &st, NULL) !=
       SQLITE_OK)
      return 0;
   int ok = sqlite3_step(st) == SQLITE_ROW;
   if (ok)
      *out = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return ok;
}

/* WHAT VERSION IS THIS FILE, AND MAY THIS BUILD TOUCH IT? 1 yes, with the
 * version in *at; 0 no, with the reason printed.
 *
 * Split out so that OPENING a database and VERIFYING a backup ask the same
 * question in the same words. They are the same question: a file written by a
 * newer server is one an older binary would read with the wrong meanings, and
 * that is no more acceptable in a backup about to DISPLACE the live database
 * than it is in the live database itself. */
static int version_supported(sqlite3 *h, const char *path, int *at)
{
   *at = 0;
   if (!schema_version(h, at)) {
      fprintf(stderr, "sync: cannot read the database schema version: %s\n",
              sqlite3_errmsg(h));
      return 0;
   }
   if (*at > DB_SCHEMA_VERSION) {
      fprintf(stderr,
              "sync: %s was written by a NEWER server (schema version %d; this "
              "build knows %d).\n"
              "  Refusing it: an older binary would be reading columns "
              "whose meaning it does not have.\n"
              "  Run the newer server, or use a backup taken before the "
              "upgrade.\n",
              path, *at, DB_SCHEMA_VERSION);
      return 0;
   }
   return 1;
}

/* EVERYTHING db_open CHECKS ABOUT A FILE IT DID NOT WRITE, and nothing that
 * writes to it. 1 when this build may use the database as it stands.
 *
 * `at` is the version the file reports. The three cases are the same ones
 * migrate() reasons about, and the reasoning is copied here rather than
 * approximated because a preflight that is WEAKER than the open it precedes is
 * not a preflight at all -- it is a promise that the thing it just approved
 * will start, and it was not entitled to make it.
 *
 *   version 0        -- a file that predates versioning, or came from
 *                       somewhere else entirely. SHAPES describes the latest
 *                       schema, and the baseline is all IF NOT EXISTS, so
 *                       every table it DOES have must already match, and no
 *                       row may already be an orphan.
 *   the current one  -- the full check, indexes included.
 *   in between       -- the migration steps are the contract, and they have
 *                       not run yet; SHAPES describes the far end, so testing
 *                       against it would refuse exactly the files a future
 *                       migration exists to bring forward. Version 1 is now
 *                       such a file: it has every table but neither of the
 *                       v2 indexes, and INDEXES names both.
 */
static int schema_usable(sqlite3 *h, const char *path, int at)
{
   if (at == 0)
      return schema_shapes_ok(h, path, 0) && no_orphan_rows(h, path);
   if (at == DB_SCHEMA_VERSION)
      return schema_shapes_ok(h, path, 1);
   return 1;
}

/* PUT THE VERSION STAMP BACK WHERE THE RUN FOUND IT, after a run that is
 * being abandoned. See the call site in migrate().
 *
 * Best-effort on purpose, and it prints if it cannot: the alternative to a
 * failed restore is not a correct database, it is the same refused file with
 * a stamp that is one version too high -- which is what this exists to
 * prevent, so there is nothing better to do than say so. */
static void unstamp(struct db *d, int to)
{
   char bump[64];
   (void)snprintf(bump, sizeof bump, "PRAGMA user_version=%d;", to);
   if (db_exec(d, "BEGIN IMMEDIATE;") && db_exec(d, bump) &&
       db_exec(d, "COMMIT;"))
      return;
   (void)db_exec(d, "ROLLBACK;");
   fprintf(stderr,
           "sync: %s was refused, and its schema version could NOT be put back "
           "to %d.\n"
           "  It is now marked as further along than it is. Restore a backup, "
           "or set\n"
           "  `PRAGMA user_version=%d;` by hand with the server stopped, "
           "before the next start\n"
           "  reads it as a file that has already passed the checks it has "
           "not.\n",
           d->path, to, to);
}

/* Bring the file up to DB_SCHEMA_VERSION, one transaction per step.
 *
 * 1 when the database is at this build's version afterwards; 0 when a step
 * failed or when the file is NEWER than this build understands.
 *
 * A REFUSED FILE IS LEFT AT THE VERSION THE RUN STARTED FROM, not at the last
 * step that happened to commit, and that distinction only became visible when
 * there was more than one step. Every step's SQL is transactional with its own
 * version bump, but the after-check -- the only place the INDEXES are compared
 * -- can run only at the final version, because SHAPES and INDEXES describe
 * the far end. So with steps 1 and 2, a file whose `user(email)` index is
 * wrong sails through step 1 (all IF NOT EXISTS, nothing to notice), commits,
 * and is stamped version 1; step 2 then fails the index check and rolls
 * itself back, leaving the file marked as having passed a gate it has not.
 * The NEXT start reads a version-1 file, skips the `at == 0` branch below --
 * which is where the column collations and the orphan-row scan live -- and
 * refuses it again on the index alone, having lost the ability to say
 * anything about the rest. That is exactly the failure the note on the
 * after-check describes, arriving by a different door once the list grew.
 *
 * So an abandoned run puts the stamp back. What it CANNOT undo is a table an
 * earlier step created in a file that had none -- those steps committed and
 * are not being replayed backwards. That is harmless and is the honest limit
 * of this: every statement in the baseline is IF NOT EXISTS, so the next start
 * re-runs it as a no-op, re-runs the version-0 checks in full, and refuses in
 * the same place with the same reason. The claim is "not adopted", not "byte
 * for byte as found". */
static int migrate(struct db *d)
{
   sqlite3 *h = H(d);
   int at     = 0;
   if (!version_supported(h, d->path, &at))
      return 0;
   const int began = at; /* what a refusal must leave behind */
   /* BEFORE ANYTHING IS STAMPED. A file that already holds a table of a
    * shape this build does not understand is not a version-0 file waiting
    * for the baseline -- it is a database from somewhere else, and the
    * baseline would skip its tables and record the result as current. */
   /* `at == 0`, not `at < DB_SCHEMA_VERSION`: SHAPES describes the schema at
    * the LATEST version, so checking it against a file two versions behind
    * would refuse exactly the databases a future migration exists to bring
    * forward -- and that migration could then never run. Between versions the
    * steps themselves are the contract; this check is for files that never
    * had one. */
   if (at == 0 && !schema_shapes_ok(h, d->path, 0))
      return 0;
   /* ...and the DATA that goes with it. A file that has never been stamped by
    * this build is the only one that can already contain rows the cascades
    * say cannot exist; see no_orphan_rows for why this is not paid on every
    * start. Deliberately AFTER the shape check, so a file that is the wrong
    * shape is told about its shape rather than about the rows in it. */
   if (at == 0 && !no_orphan_rows(h, d->path))
      return 0;
   int ran = 0;
   for (int i = 0; i < NMIGRATIONS; i++) {
      if (MIGRATIONS[i].to <= at)
         continue;
      /* THE STEP AND THE VERSION BUMP ARE ONE TRANSACTION. Apart, a crash
       * between them leaves a database that has had the change and does not
       * say so -- and the next start runs the step again. */
      char bump[64];
      (void)snprintf(bump, sizeof bump, "PRAGMA user_version=%d;",
                     MIGRATIONS[i].to);
      if (!db_exec(d, "BEGIN IMMEDIATE;") || !db_exec(d, MIGRATIONS[i].sql) ||
          !db_exec(d, bump)) {
         fprintf(stderr, "sync: schema step %d failed: %s\n", MIGRATIONS[i].to,
                 sqlite3_errmsg(h));
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      /* ...AND SO IS THE CHECK, on the last step. THIS IS WHAT "NOT STAMPED"
       * MEANS.
       *
       * The after-check used to run once the final COMMIT had already
       * happened, so a file it refused had nevertheless had its user_version
       * written -- and the properties only the after-check can see (the
       * indexes, with their UNIQUE flags, their partial-ness and their
       * collations) are exactly the ones that decide whether one email
       * address in two spellings is one account or two. Such a file was
       * refused, correctly, and left marked as this build's. The next start
       * then found a version-1 file, skipped the version-0 gate entirely --
       * which is where the column collations and the orphan-row scan live --
       * and refused it again for the index alone, having lost the ability to
       * say anything about the rest.
       *
       * Inside the transaction, sqlite reports the schema as this connection
       * has just changed it, so the check sees the migrated shape and a
       * failure rolls the whole thing -- tables, indexes and version stamp --
       * back to the file as it was found. A database that does not match is
       * then exactly as unadopted as before it was opened. */
      if (MIGRATIONS[i].to == DB_SCHEMA_VERSION &&
          !schema_shapes_ok(h, d->path, 1)) {
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      if (!db_exec(d, "COMMIT;")) {
         fprintf(stderr, "sync: schema step %d failed to commit: %s\n",
                 MIGRATIONS[i].to, sqlite3_errmsg(h));
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      at  = MIGRATIONS[i].to;
      ran = 1;
   }
   /* ...and after, for a file that needed no step at all: every table this
    * build queries exists and looks the way this build queries it. (When a
    * step did run, the same check has already run inside its transaction --
    * repeating it here would only ask the same question of the same file.) */
   if (!ran && !schema_shapes_ok(h, d->path, 1))
      return 0;
   return at == DB_SCHEMA_VERSION;

abandon:
   /* Only when an EARLIER step has already committed. Otherwise `at` is still
    * `began` and the write would be a no-op that could only fail. */
   if (at != began)
      unstamp(d, began);
   return 0;
}

struct db *db_open(const char *path)
{
   /* REQUIRED: the build sets SQLITE_OMIT_AUTOINIT, which drops the implicit
    * initialisation every other API call would otherwise do for us. Without
    * this the first sqlite3_open dereferences an uninitialised global. */
   if (sqlite3_initialize() != SQLITE_OK) {
      fprintf(stderr, "sync: sqlite3_initialize failed\n");
      return NULL;
   }
   struct db *d = calloc(1, sizeof *d);
   if (!d) {
      fprintf(stderr, "sync: out of memory opening %s\n", path);
      return NULL;
   }
   int n = snprintf(d->path, sizeof d->path, "%s", path);
   if (n <= 0 || n >= (int)sizeof d->path) {
      fprintf(stderr, "sync: database path too long\n");
      free(d);
      return NULL;
   }
   /* THROUGH THE SAME SWITCH EVERY OTHER OPEN USES. This was a raw
    * sqlite3_open straight into t_conn, which is what made a second db_open
    * on one thread drop the first connection on the floor: still open, no
    * longer reachable, never closed. */
   if (!conn_open(d, 0)) {
      free(d);
      return NULL;
   }
   if (!migrate(d)) {
      db_close(d);
      return NULL;
   }
   /* Reclaim the log left by the previous run. The size limit above only
    * takes effect AFTER a checkpoint, and this server is stopped by being
    * killed rather than closed, so nothing ever checkpoints on the way out --
    * the log survives restarts at whatever size the largest sync made it.
    * Doing it here means a restart is also the cleanup. */
   if (!db_exec(d, "PRAGMA wal_checkpoint(TRUNCATE);"))
      fprintf(stderr, "sync: optional startup WAL checkpoint failed\n");
   return d;
}

/* ---- WHICH FILE, NOT WHICH SPELLING ----------------------------------
 *
 * `p` in canonical form, in `out` (which must hold PATH_MAX). 1 on success.
 *
 * IT HAS TO WORK FOR A PATH THAT DOES NOT EXIST YET, because a backup
 * destination usually does not: the whole point is to resolve where the
 * rename will LAND. realpath refuses a missing final component, so the
 * DIRECTORY is resolved -- which collapses every `..`, every `.` and every
 * symlinked parent -- and the last component is put back on the end. */
static int canon(const char *p, char *out, size_t n)
{
   char real[PATH_MAX];
   if (realpath(p, real)) {
      int k = snprintf(out, n, "%s", real);
      return k > 0 && (size_t)k < n;
   }
   char copy[PATH_MAX];
   int k = snprintf(copy, sizeof copy, "%s", p);
   if (k <= 0 || (size_t)k >= sizeof copy)
      return 0;
   char *slash      = strrchr(copy, '/');
   const char *base = slash ? slash + 1 : copy;
   const char *dir  = ".";
   if (slash == copy)
      dir = "/";
   else if (slash) {
      *slash = '\0';
      dir    = copy;
   }
   /* "", "." and ".." are not names a file can be created under, so there is
    * nothing to canonicalise and nothing safe to guess. */
   if (!*base || !strcmp(base, ".") || !strcmp(base, ".."))
      return 0;
   char rdir[PATH_MAX];
   if (!realpath(dir, rdir))
      return 0;
   k = snprintf(out, n, "%s%s%s", rdir, strcmp(rdir, "/") ? "/" : "", base);
   return k > 0 && (size_t)k < n;
}

/* THE SAME FILE, asked of the filesystem rather than of two strings. A
 * symlink, a hard link, a bind mount and `../live/sync.db` all answer yes
 * here and no to strcmp. Missing files are not "the same": if either side is
 * not there, the canonical-name comparison is what decides. */
static int same_file(const char *a, const char *b)
{
   struct stat sa, sb;
   if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
      return 0;
   return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* One refusal, in the two ways a destination can reach a file: by resolving
 * to its name, or by being it. */
static int reaches(const char *dest_canon, const char *dest_raw,
                   const char *other)
{
   return !strcmp(dest_canon, other) || same_file(dest_raw, other);
}

enum backup_dest db_backup_dest(struct db *d, const char *out_path)
{
   if (!d || !d->path[0] || !out_path || !*out_path)
      return BACKUP_DEST_FAIL;
   char live[PATH_MAX], dest[PATH_MAX];
   if (!canon(d->path, live, sizeof live)) {
      fprintf(stderr, "sync: cannot resolve the live database %s\n", d->path);
      return BACKUP_DEST_FAIL;
   }
   if (!canon(out_path, dest, sizeof dest)) {
      fprintf(stderr,
              "sync: cannot resolve the backup destination %s\n"
              "  Its directory must exist before a backup can be written "
              "into it.\n",
              out_path);
      return BACKUP_DEST_FAIL;
   }
   /* THE DATABASE AND EVERY FILE SQLITE KEEPS BESIDE IT. -journal is here
    * even though this server runs WAL: a database recovered by hand, or one
    * opened by another tool, has one, and a backup written over it destroys
    * the rollback the next open needs. */
   static const char *const SIDE[] = {"", "-wal", "-shm", "-journal"};
   for (size_t i = 0; i < sizeof SIDE / sizeof SIDE[0]; i++) {
      char one[PATH_MAX];
      int k = snprintf(one, sizeof one, "%s%s", live, SIDE[i]);
      if (k <= 0 || (size_t)k >= sizeof one)
         return BACKUP_DEST_FAIL;
      if (reaches(dest, out_path, one)) {
         fprintf(stderr,
                 "sync: REFUSING to back up onto %s.\n"
                 "  It is the live database's %s (%s).\n"
                 "  Publishing a backup there renames a file over the one the "
                 "server has open:\n"
                 "  the server keeps writing to an inode with no name, every "
                 "request still\n"
                 "  succeeds, and at the next restart every sync since this "
                 "moment is gone.\n"
                 "  WHAT TO DO: back up to a directory that is not the data "
                 "directory.\n",
                 out_path, *SIDE[i] ? SIDE[i] + 1 : "own file", one);
         return BACKUP_DEST_ALIAS;
      }
   }
   /* ---- AND THE STAGING NAMES ANOTHER OPERATION OWNS ------------------
    *
    * srv/deploy/lock.sh sets the conventions these follow, and the reason
    * they are refused is the same in each case: the file at that name is
    * about to be renamed ONTO something, or deleted, by an operation that
    * has no idea a backup was published there.
    *
    *   <db>.part          what a backup of THIS database stages into. A file
    *                      published there is deleted by the next backup's
    *                      scratch_clear before it is overwritten.
    *   <db>.restoring-*   what restore.sh copies in and then installs as the
    *                      live database. A backup published there is
    *                      installed by a restore nobody aimed at it.
    *
    * ANCHORED ON THE LIVE DATABASE'S NAME, not on the suffix alone. Every
    * backup this command takes stages at `<dest>.part` and then checks THAT
    * path through this same function, so a blanket refusal of `*.part` would
    * refuse every backup there is. What makes a staging name dangerous is
    * whose file it stages FOR.
    */
   char part[PATH_MAX];
   int k = snprintf(part, sizeof part, "%s.part", live);
   if (k <= 0 || (size_t)k >= sizeof part)
      return BACKUP_DEST_FAIL;
   char restoring[PATH_MAX];
   k = snprintf(restoring, sizeof restoring, "%s.restoring-", live);
   if (k <= 0 || (size_t)k >= sizeof restoring)
      return BACKUP_DEST_FAIL;
   size_t dn = strlen(dest), rn = strlen(restoring);
   if (reaches(dest, out_path, part) ||
       (dn > rn && !strncmp(dest, restoring, rn))) {
      fprintf(stderr,
              "sync: REFUSING to back up onto %s.\n"
              "  That is a STAGING name (see srv/deploy/lock.sh): a backup, a "
              "restore or a\n"
              "  deploy renames files at those names over the live database, "
              "or deletes them\n"
              "  before writing its own. A published backup must have a name "
              "of its own.\n",
              out_path);
      return BACKUP_DEST_ALIAS;
   }
   return BACKUP_DEST_OK;
}

/* A COPY THAT IS SAFE TO TAKE WHILE THE SERVER IS RUNNING.
 *
 * Copying sync.db with cp is not a backup: in WAL mode the committed data
 * lives partly in sync.db-wal, so the copy is a database missing every
 * transaction since the last checkpoint -- and one taken mid-checkpoint can
 * be torn outright. The restore then looks fine (it opens, it queries) and is
 * quietly short of whatever was synced most recently, which is exactly the
 * data anyone restoring is trying to get back.
 *
 * sqlite3_backup does it properly: it reads through the same connection, so
 * it sees the WAL, and it restarts itself if a writer changes a page it has
 * already copied. Called from the CLI, with the server running or not.
 */
int db_backup(struct db *d, const char *out_path)
{
   sqlite3 *src = H(d);
   if (!src || !out_path || !*out_path)
      return 0;
   /* CHECKED HERE, not only where the CLI parses its arguments: this is the
    * function that CREATES the file, and a caller that stages into a name it
    * chose itself (the CLI's `<dest>.part`) has a second destination nobody
    * else looked at. Both go through the same test. */
   if (db_backup_dest(d, out_path) != BACKUP_DEST_OK)
      return 0;
   sqlite3 *dst = NULL;
   if (sqlite3_open(out_path, &dst) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot create %s: %s\n", out_path,
              dst ? sqlite3_errmsg(dst) : "?");
      sqlite3_close(dst);
      return 0;
   }
   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   int ok            = 0;
   if (b) {
      /* -1: the whole database in one step, then finish. On this data (a few
       * megabytes) the copy is milliseconds, and a page-at-a-time loop would
       * only widen the window in which a writer forces a restart. */
      int rc = sqlite3_backup_step(b, -1);
      ok     = rc == SQLITE_DONE;
      sqlite3_backup_finish(b);
      if (!ok)
         fprintf(stderr, "sync: backup failed: %s\n", sqlite3_errstr(rc));
   } else {
      fprintf(stderr, "sync: backup failed: %s\n", sqlite3_errmsg(dst));
   }
   /* The copy must be CLOSED before anyone is told it exists: sqlite writes
    * the last pages on close, and a caller that renames it into place first
    * would publish a file that is still being written. */
   if (sqlite3_close(dst) != SQLITE_OK) {
      fprintf(stderr, "sync: backup could not be closed cleanly\n");
      ok = 0;
   }
   return ok;
}

/* WHAT A RESTORE DRILL ASKS. A backup nobody has ever restored is a hope, not
 * a backup, so this is the other half: open a copy and have sqlite verify its
 * own structure, then confirm the schema this server needs is in it.
 *
 * ---- WHY IT IS A COPY, AND WHY THE COPY IS SOMEWHERE PRIVATE ----------
 *
 * Opening a database CREATES FILES beside it: a -wal and a -shm, at minimum.
 * Verifying a backup must not leave those in the backups directory, so this
 * used to note which of them were absent before the open and remove those
 * same NAMES afterwards.
 *
 * That is a time-of-check to time-of-use hole with data loss on the far side.
 * The names are `<path>-wal` and `<path>-shm`, and between the note and the
 * removal anything may create them -- most obviously a server starting on
 * that database, which is not a hypothetical: srv/deploy/restore.sh verifies
 * a staged file sitting in the LIVE data directory while the board is
 * running, and deploy.sh verifies a backup taken seconds earlier. A -wal that
 * belongs to a running server is every commit since its last checkpoint.
 * Deleting it is not litter, it is the data.
 *
 * So nothing outside a private directory is opened by sqlite or unlinked. The
 * file is copied (with its own -wal, if it has one, so a WAL database is
 * verified whole) into a directory created by mkdtemp -- a name no other
 * process has ever seen -- and the COPY is what sqlite opens. Whatever
 * sidecars that open creates land in there, and only there.
 *
 * The original is opened O_RDONLY, once, to read its bytes. Verification
 * therefore cannot migrate it, stamp it, checkpoint it or repair it, which
 * used to be an argument about SQLITE_OPEN_READONLY and is now a property of
 * never handing sqlite the path at all. */

/* THE PRIVATE DIRECTORY, held open as a descriptor.
 *
 * Every later operation on it goes through `fd` with openat/unlinkat, so a
 * rename of one of its parents cannot redirect a single one of them at
 * another file. `dev` is the filesystem it lives on: anything in it that
 * claims to be somewhere else is not ours to delete. */
struct scratch {
   char dir[PATH_MAX];
   int fd;
   dev_t dev;
};

/* Create it beside `near` if that directory will take it, otherwise under
 * TMPDIR. BESIDE FIRST on purpose: the board this runs on has a tmpfs /tmp
 * of a few megabytes and a database larger than that, so a copy into /tmp
 * would fail exactly on the machine the backups are of. */
static int scratch_make(const char *near, struct scratch *s)
{
   char base[PATH_MAX];
   int k = snprintf(base, sizeof base, "%s", near);
   if (k <= 0 || (size_t)k >= sizeof base)
      return 0;
   char *slash = strrchr(base, '/');
   if (slash == base)
      base[1] = '\0';
   else if (slash)
      *slash = '\0';
   else
      (void)snprintf(base, sizeof base, ".");
   const char *tmp = getenv("TMPDIR");
   const char *where[] = {base, tmp && *tmp ? tmp : "/tmp"};
   for (size_t i = 0; i < sizeof where / sizeof where[0]; i++) {
      k = snprintf(s->dir, sizeof s->dir, "%s/.pancra-verify-XXXXXX",
                   where[i]);
      if (k <= 0 || (size_t)k >= sizeof s->dir)
         continue;
      if (!mkdtemp(s->dir))
         continue;
      s->fd = open(s->dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      struct stat st;
      if (s->fd >= 0 && fstat(s->fd, &st) == 0) {
         s->dev = st.st_dev;
         return 1;
      }
      if (s->fd >= 0)
         close(s->fd);
      (void)rmdir(s->dir);
   }
   fprintf(stderr,
           "sync: cannot make a scratch directory to verify %s in\n"
           "  (tried beside it and $TMPDIR; verification never opens the file "
           "itself)\n",
           near);
   return 0;
}

/* EMPTY IT, AND ONLY IT.
 *
 * Every entry is opened THROUGH the directory descriptor with O_NOFOLLOW
 * first, and unlinked only if the descriptor says it is a plain file, on this
 * directory's own filesystem, with exactly one name. That last condition is
 * the one that matters: a file with a second hard link is a file somebody
 * else can still see, and this call did not create it.
 *
 * Anything that fails a check is LEFT, with a line saying so, and the
 * directory then refuses to rmdir -- which is the loud version of the failure
 * and infinitely preferable to unlinking a stranger's file. */
static void scratch_drop(struct scratch *s)
{
   int walk = dup(s->fd);
   DIR *dp  = walk >= 0 ? fdopendir(walk) : NULL;
   if (!dp) {
      if (walk >= 0)
         close(walk);
      close(s->fd);
      fprintf(stderr, "sync: scratch directory %s could not be read back\n",
              s->dir);
      return;
   }
   struct dirent *e;
   while ((e = readdir(dp))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      int fd = openat(s->fd, e->d_name, O_RDONLY | O_NOFOLLOW);
      struct stat st;
      if (fd < 0 || fstat(fd, &st) != 0) {
         if (fd >= 0)
            close(fd);
         fprintf(stderr, "sync: leaving %s/%s: it could not be identified\n",
                 s->dir, e->d_name);
         continue;
      }
      int mine = S_ISREG(st.st_mode) && st.st_dev == s->dev && st.st_nlink == 1;
      close(fd);
      if (!mine) {
         fprintf(stderr,
                 "sync: leaving %s/%s: it is not a file this verification "
                 "created\n",
                 s->dir, e->d_name);
         continue;
      }
      (void)unlinkat(s->fd, e->d_name, 0);
   }
   closedir(dp); /* also closes `walk` */
   close(s->fd);
   if (rmdir(s->dir) != 0)
      fprintf(stderr, "sync: scratch directory %s could not be removed\n",
              s->dir);
}

/* Copy `src` into the scratch directory under `name`. `must` is whether its
 * absence is a failure -- the database itself must be there, its -wal need
 * not be. */
static int scratch_copy(struct scratch *s, const char *src, const char *name,
                        int must)
{
   int in = open(src, O_RDONLY);
   if (in < 0) {
      if (must)
         fprintf(stderr, "sync: cannot read %s\n", src);
      return !must;
   }
   int out = openat(s->fd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
   if (out < 0) {
      close(in);
      fprintf(stderr, "sync: cannot write the scratch copy of %s\n", src);
      return 0;
   }
   char buf[65536];
   int ok = 1;
   for (;;) {
      ssize_t r = read(in, buf, sizeof buf);
      if (r == 0)
         break;
      if (r < 0) {
         ok = 0;
         break;
      }
      for (ssize_t off = 0; off < r;) {
         ssize_t w = write(out, buf + off, (size_t)(r - off));
         if (w <= 0) {
            ok = 0;
            break;
         }
         off += w;
      }
      if (!ok)
         break;
   }
   if (close(out) != 0)
      ok = 0;
   close(in);
   if (!ok)
      fprintf(stderr, "sync: %s could not be copied for verification\n", src);
   return ok;
}

/* The checks themselves, on `copy`. Every diagnostic names `as` -- the path
 * the operator asked about -- because the scratch copy is an implementation
 * detail and naming it in a refusal sends somebody looking for a file that no
 * longer exists. */
static int verify_copy(const char *copy, const char *as)
{
   sqlite3 *h = NULL;
   /* READWRITE, on a copy that is deleted moments from now: a WAL database
    * needs to recover its log to be read at all, and that recovery is exactly
    * what a restore would do. Nothing here writes on purpose; the file it
    * could write to is the copy. */
   if (sqlite3_open_v2(copy, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot open %s: %s\n", as,
              h ? sqlite3_errmsg(h) : "?");
      sqlite3_close(h);
      return 0;
   }
   sqlite3_stmt *st = NULL;
   int ok = sqlite3_prepare_v2(h, "PRAGMA integrity_check;", -1, &st, NULL) ==
            SQLITE_OK;
   const unsigned char *got = NULL;
   if (ok && sqlite3_step(st) == SQLITE_ROW)
      got = sqlite3_column_text(st, 0);
   ok = got && !sqlite3_stricmp((const char *)got, "ok");
   if (!ok)
      fprintf(stderr, "sync: %s FAILED integrity check: %s\n", as,
              got ? (const char *)got : "no answer");
   sqlite3_finalize(st);
   /* Structure is not enough: an empty file passes integrity_check. The
    * accounts, the registered phones and the synced rows are what make this
    * a Pancra database rather than a well-formed one. */
   if (ok) {
      st    = NULL;
      int q = sqlite3_prepare_v2(
          h,
          "SELECT count(*) FROM sqlite_master WHERE type='table'"
          " AND name IN ('user','app','logrow')",
          -1, &st, NULL);
      int have = 0;
      if (q == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
         have = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
      if (have != 3) {
         fprintf(stderr, "sync: %s is a database, but not this server's\n", as);
         ok = 0;
      }
   }
   /* ---- AND THE SAME COMPATIBILITY QUESTION db_open ASKS ---------------
    *
    * THREE TABLE NAMES WERE NOT A COMPATIBILITY CHECK. Everything above is
    * satisfied by a database that is perfectly intact, perfectly well formed,
    * and unusable by this build: one written by a NEWER server, or one whose
    * `logrow` has different columns, or whose cascades are gone. Every one of
    * those passes integrity_check and has all three names.
    *
    * WHAT THIS PREFLIGHT IS FOR decides how bad that is. It is what restore
    * asks BEFORE the backup DISPLACES the live database. So the sequence was:
    * verify says yes, the good database is replaced by the unusable one, and
    * the server then fails to start -- turning a refusal that costs nothing
    * into an outage that has to be recovered from, with the operator's own
    * data now the thing needing rescue. A preflight weaker than the open it
    * precedes is a promise it is not entitled to make.
    *
    * READ-ONLY IN THE ONLY SENSE THAT MATTERS: version_supported and
    * schema_usable only run PRAGMAs and SELECTs, and the file they run
    * against is a private copy of the operator's, so verifying a backup
    * cannot migrate it, stamp it or repair it. A backup that needs a
    * migration is still a valid backup -- db_open will migrate it once it is
    * in place -- which is why schema_usable accepts an older version rather
    * than demanding the current one. */
   if (ok) {
      int at = 0;
      if (!version_supported(h, as, &at) || !schema_usable(h, as, at)) {
         fprintf(stderr,
                 "sync: %s is intact, but this build cannot use it.\n"
                 "  REFUSING IT AS A BACKUP. It would replace a working "
                 "database with one the server\n"
                 "  cannot open, and the failure would appear at startup, "
                 "after the good copy was gone.\n"
                 "  WHAT TO DO: use a different backup, or run the server "
                 "build that wrote this one.\n",
                 as);
         ok = 0;
      }
   }
   sqlite3_close(h);
   return ok;
}

int db_verify(const char *path)
{
   if (!path || !*path)
      return 0;
   if (sqlite3_initialize() != SQLITE_OK)
      return 0;
   struct scratch s;
   if (!scratch_make(path, &s))
      return 0;
   char copy[PATH_MAX], wal[PATH_MAX];
   int k  = snprintf(copy, sizeof copy, "%s/db", s.dir);
   int kw = snprintf(wal, sizeof wal, "%s-wal", path);
   int ok = k > 0 && (size_t)k < sizeof copy && kw > 0 &&
            (size_t)kw < sizeof wal;
   /* The -wal is copied when it is there and skipped when it is not; it is
    * never created, never touched and never removed. */
   if (ok)
      ok = scratch_copy(&s, path, "db", 1) &&
           scratch_copy(&s, wal, "db-wal", 0);
   if (ok)
      ok = verify_copy(copy, path);
   scratch_drop(&s);
   return ok;
}

void db_close(struct db *d)
{
   /* EVERY HANDLE THIS THREAD OWNS FOR `d`, which is at most one: conn_open
    * is the only thing that assigns t_conn and it closes what was there
    * first, so a context cannot have accumulated a second connection on this
    * thread. A connection to `d` on ANOTHER thread is that thread's to close,
    * and the workers' are closed by the process exiting -- there is no worker
    * teardown path to hook, and a pool thread lives as long as the server. */
   if (!d)
      return;
   if (t_conn && t_of == d && conn_release() != CONN_TAKE_OK) {
      /* THE ONE PLACE FORCING IS THE LESSER EVIL. `d` is about to be freed,
       * so t_of cannot be left pointing at it and the handle cannot be left
       * cached; the choice is between leaking the connection for the life of
       * the process and handing it to sqlite to reap. sqlite3_close_v2 makes
       * it a zombie: the statements the caller leaked keep working, and the
       * descriptors go when the last of them is finalised. The complaint
       * conn_release already printed names the real bug. */
      (void)sqlite3_close_v2(t_conn);
      t_conn = NULL;
      t_of   = NULL;
   }
   free(d);
}
