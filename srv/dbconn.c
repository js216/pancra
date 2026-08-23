/* SPDX-License-Identifier: GPL-3.0
 * dbconn.c --- this thread's connection, and every statement through it
 * Copyright 2026 Jakob Kastelic
 *
 * ONE MODULE, FOUR FILES. srv/db.h is the interface; behind it the database
 * is split by what each part is ABOUT, because "the database" was one file
 * holding four unrelated subjects and a reader looking for any one of them
 * had to know the other three were not it:
 *
 *   dbconn.c    (this file) the per-thread connection, the pragmas it is
 *               configured with, the statement adapter every caller goes
 *               through, the durable-transaction verbs -- and, beside the
 *               adapter because that is what they instrument, the
 *               deterministic fault hooks the fault suite arms.
 *   dbschema.c  the schema itself and the ordered migration steps.
 *   dbcheck.c   what a database must LOOK like, and the checks that refuse
 *               one that does not: shapes, indexes, rowid-ness, orphan rows,
 *               and which versions this build can read.
 *   db.c        the front door: open, repair, close.
 *
 * The seams between them are srv/dbint.h and nothing else -- no file here
 * reaches into another's statics, and every one of them keeps sqlite3 handles
 * away from callers outside the module (db.h hands out none). */
#include "db.h"
#include "dbint.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* HOW LONG A WRITER WAITS FOR ANOTHER WRITER before its statement is refused
 * as busy.
 *
 * WAL lets readers run throughout, so this bounds one case only: two
 * connections wanting to write at once. Three seconds is generous for a phone
 * pushing while a browser reads, and generous is the right default -- a
 * refused write costs the caller a round trip, and on a board whose storage
 * occasionally stalls for a second the retry would be the common path.
 *
 * IT IS A DEPLOYMENT'S NUMBER, NOT AN ALGORITHM'S, so it can be set: a board
 * with slower storage may want longer, and anything that has to observe the
 * refusal -- an operator reproducing a report, a suite holding the write lock
 * on purpose -- must not have to wait out a production timeout to see it. A
 * value that is not a number in range is ignored, loudly, rather than quietly
 * becoming zero, which would refuse the first contended write there ever was.
 */
#define DB_BUSY_MS_DEFAULT 3000
#define DB_BUSY_MS_MIN     10
#define DB_BUSY_MS_MAX     60000

static int busy_ms(void)
{
   const char *v = getenv("PANCRA_DB_BUSY_MS");
   if (!v || !*v)
      return DB_BUSY_MS_DEFAULT;
   char *end = NULL;
   long n    = strtol(v, &end, 10);
   if (end && !*end && n >= DB_BUSY_MS_MIN && n <= DB_BUSY_MS_MAX)
      return (int)n;
   fprintf(stderr, "sync: PANCRA_DB_BUSY_MS='%s' is not %d..%d ms; using %d\n",
           v, DB_BUSY_MS_MIN, DB_BUSY_MS_MAX, DB_BUSY_MS_DEFAULT);
   return DB_BUSY_MS_DEFAULT;
}

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
/* struct db lives in srv/dbint.h now: dbbackup.c, the other half of this
 * module, reads d->path to work out where a backup may land. It is still not
 * in db.h -- the public interface hands out an opaque pointer, deliberately. */

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
   int64_t want = strtoll(s, NULL, 10);
   int n        = atomic_fetch_add(counter, 1) + 1;
   if (want <= 0 || (int64_t)n != want)
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

/* A ROLLBACK THAT DOES NOT HAPPEN, which is the one failure that
 * leaves a cached connection inside a transaction -- and the one a real
 * filesystem will not produce on request. Same shape as DB_FAIL_COMMIT: the
 * statement is not executed and db_exec reports a failure, so the connection
 * is still in its transaction afterwards, which is exactly the state the
 * poisoning is about. Nothing that ships defines DB_FAULTS. */
static int fault_rollback(const char *sql)
{
   static atomic_int n;
   if (!sql || sqlite3_strnicmp(sql, "ROLLBACK", 8) != 0)
      return 0;
   return fault_at("DB_FAIL_ROLLBACK", &n);
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
#define fault_rollback(sql)     0
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

/* ---- ENDING A TRANSACTION IS ONE OPERATION, AND IT IS CHECKED --
 *
 * WHAT WAS WRONG. Every caller ended a transaction by calling ROLLBACK (or a
 * COMMIT that fell back to one) and throwing the answer away -- `(void)
 * db_exec(d, "ROLLBACK")` in a dozen places. A ROLLBACK can fail: an I/O
 * error on the journal, a statement still stepping on this connection, or
 * SQLITE_BUSY on a hot journal. When it does, the connection is STILL INSIDE
 * A TRANSACTION and nothing anywhere knows it.
 *
 * That connection is CACHED PER THREAD (see the block above), so it is handed
 * to the next request this worker serves. Three things follow, all silent:
 *
 *   * that request's writes join a transaction it did not open, and are
 *     committed -- or rolled back -- by whatever ends that one;
 *   * it holds the write lock for the whole time, so every other worker's
 *     write blocks and then times out;
 *   * a BEGIN IMMEDIATE issued inside it fails with "cannot start a
 *     transaction within a transaction", which the caller reports as a
 *     storage failure on an operation that was fine.
 *
 * So finalization is one function, and it ASKS THE CONNECTION whether it
 * worked: sqlite3_get_autocommit() is the one true answer to "am I still in a
 * transaction", and it is not derivable from db_exec's status (a ROLLBACK
 * that reports success while a statement is still open leaves the
 * transaction open too).
 *
 * A CONNECTION THAT CANNOT BE GOT OUT OF ITS TRANSACTION IS POISONED. Nothing
 * else this file can do is honest: it cannot force the transaction away, and
 * carrying on hands the ambiguity to the next request. So it is marked, and
 * the next H() closes it and opens a fresh one -- which discards the
 * transaction with the connection, because sqlite rolls back what an
 * abandoned connection was holding. */
static _Thread_local int t_poisoned;

static void txn_poison(const char *what)
{
   t_poisoned = 1;
   fprintf(stderr,
           "sync: %s left this thread's connection inside a transaction "
           "(%s).\n"
           "  It will not be reused: the connection is discarded and the "
           "next request on\n"
           "  this thread opens a new one. Nothing was committed by it.\n",
           /* t_conn DIRECTLY, not H(): the flag above is already set, so
            * H() would close this connection and open another one -- in the
            * middle of composing a message about the one being discarded. */
           what, t_conn ? sqlite3_errmsg(t_conn) : "?");
}

/* AN I/O ERROR DISCARDS THE CONNECTION, not merely the statement that met it.
 *
 * sqlite deletes a database's -wal and -shm files when the last connection to
 * it closes. A maintenance command run while the server is up -- a backup, an
 * account removal, even a listing -- is a second process on the same file, and
 * on its way out it takes those two files with it. Every worker connection
 * that had them open is left mapped onto a file that is no longer there and
 * answers SQLITE_IOERR to everything afterwards, for the life of the process:
 * one `backup` measured seven straight failures and a server answering 503
 * until it was restarted.
 *
 * Marking it here ends that. The request that met the error still fails --
 * there is no honest way to serve it -- and the next request on this thread
 * opens a fresh connection and succeeds.
 *
 * THE PRIMARY CODE IS WHAT IS COMPARED: sqlite has a dozen extended IOERR
 * codes and every one of them means the same thing to this connection. */
static void io_poison(int rc)
{
   if ((rc & 0xff) != SQLITE_IOERR)
      return;
   t_poisoned = 1;
   fprintf(stderr, "sync: an I/O error discarded this thread's database "
                   "connection; the next request opens a fresh one\n");
}

/* END IT, AND PROVE IT ENDED. `commit` chooses which verb is tried first;
 * either way the connection must come back to autocommit and the durability
 * pragma must come back to NORMAL -- a connection left at FULL makes every
 * later bucket PUT pay an fsync.
 *
 * Returns 1 only when the intended verb succeeded AND the connection is out
 * of its transaction. */
static int txn_finish(struct db *d, int commit)
{
   int ok = 1;
   if (commit) {
      ok = db_exec(d, "COMMIT");
      if (!ok)
         (void)db_exec(d, "ROLLBACK");
   } else {
      (void)db_exec(d, "ROLLBACK");
   }
   /* THE PRAGMA BEFORE THE VERDICT, so a poisoned connection is not also a
    * connection left at FULL if it somehow survives. */
   (void)db_exec(d, "PRAGMA synchronous=NORMAL;");
   /* THIS THREAD'S CONNECTION AS IT STANDS, asked directly rather than
    * through H(): H would re-open a connection that is already poisoned, and
    * the question here is about the one the transaction was on. */
   sqlite3 *h = (t_conn && t_of == d) ? t_conn : NULL;
   if (!h || !sqlite3_get_autocommit(h)) {
      txn_poison(commit ? "a commit" : "a rollback");
      return 0;
   }
   return ok;
}

int db_durable_commit(struct db *d)
{
   return txn_finish(d, 1);
}

/* VOID, AND DELIBERATELY SO. Every caller of this is already on a failure
 * path and has nothing different to do about a rollback that did not work --
 * what matters is that the CONNECTION does not go on to serve another
 * request, and that is handled here rather than by asking each caller to
 * handle it. db_conn_poisoned() is for the tests, which do have something to
 * say about it. */
void db_durable_rollback(struct db *d)
{
   (void)txn_finish(d, 0);
}

int db_conn_poisoned(void)
{
   return t_poisoned;
}

static int db_conf(sqlite3 *h, int worker)
{
   /* ---- WHAT NORMAL ACTUALLY PROMISES, AND WHAT IT DOES NOT ----------
    *
    * WAL with synchronous=NORMAL cannot CORRUPT the file across a power cut:
    * whatever is recovered is a consistent database. That much is true.
    *
    * What it does NOT promise is that a COMMITTED transaction survives. WAL
    * frames are fsynced at a checkpoint, not at every commit, so a commit
    * that returned success can be gone after a power failure -- and the
    * board has no battery behind it. So an HTTP 200 sent right after such a
    * commit is an acknowledgement the storage has not earned.
    *
    * NORMAL is still the default here, deliberately: one fsync per checkpoint
    * rather than one per transaction is the difference between a bearable and
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
   if (sqlite3_busy_timeout(h, busy_ms()) != SQLITE_OK) {
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
 * that answer the existing handle is STILL OPEN and t_conn still names it,
 * so a
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
 * So the switch REFUSES: nothing changes, the cached connection stays valid,
 * and the open that wanted the change is told it did not happen. The
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
   /* A POISONED CONNECTION IS NOT REUSED. It is inside a
    * transaction nothing could end, so every later statement on it would
    * join that transaction. Closing it is what ends it: sqlite rolls back
    * whatever an abandoned connection held. If it will not close -- a
    * statement is still open on it -- conn_open refuses and this answers
    * NULL, which every caller already treats as a storage failure. That is
    * the right answer: there is no usable connection.
    *
    * CLEARED ONLY BY A SUCCESSFUL RE-OPEN, so a connection that will not
    * close keeps refusing rather than being handed out again on the next
    * call. */
   if (t_poisoned) {
      sqlite3 *fresh = conn_open(d, 1);
      if (!fresh)
         return NULL;
      t_poisoned = 0;
      return fresh;
   }
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
/* THE HANDLE, FOR THE OTHER HALF OF THIS MODULE.
 *
 * dbbackup.c was split out of this file (see its header) and still needs the
 * connection H() opens and caches. Exported through srv/dbint.h rather than
 * db.h: it is not part of the database's public interface -- a caller outside
 * this pair has no business holding a raw sqlite3* -- it is the seam left by
 * cutting one module in two. */
/* THE FIRST CONNECTION FOR A CONTEXT, opened by db_open before it validates
 * or migrates anything. It goes through the same switch every later open uses
 * -- a raw sqlite3_open into the thread-local is what once dropped a live
 * connection on the floor: still open, no longer reachable, never closed. */
sqlite3 *db_conn_first(struct db *d)
{
   return conn_open(d, 0);
}

sqlite3 *db_handle(struct db *d)
{
   return H(d);
}

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
 *   DB_FAIL_ROLLBACK=n  the n-th ROLLBACK is not executed
 *   DB_FAIL_COMMIT=n    the n-th COMMIT fails
 * A COMMIT that fails is the interesting one: every row is in the
 * transaction, the client is about to be told "stored", and the answer has to
 * be that nothing was stored.
 */

int db_exec(struct db *d, const char *sql)
{
   if (fault_commit(sql) || fault_rollback(sql))
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
   io_poison(sqlite3_errcode(h));
   return 0;
}

int db_finished(int rc)
{
   if (rc == SQLITE_DONE)
      return 1;
   io_poison(rc);
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

enum db_get db_get_long(struct db *d, const char *sql, int64_t arg,
                        int64_t *out)
{
   sqlite3_stmt *st = db_prep(d, sql);
   if (!st)
      return DB_GET_FAIL;
   sqlite3_bind_int64(st, 1, arg);
   int rc    = sqlite3_step(st);
   int64_t v = 0;
   if (rc == SQLITE_ROW)
      v = (int64_t)sqlite3_column_int64(st, 0);
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

int64_t db_last_id(struct db *d)
{
   return (int64_t)sqlite3_last_insert_rowid(H(d));
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
