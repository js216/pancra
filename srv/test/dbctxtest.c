// SPDX-License-Identifier: GPL-3.0
// dbctxtest.c --- A database is a VALUE, not a process-wide fact
// Copyright 2026 Jakob Kastelic
//
/* WHAT THIS PINS, and why it is worth its own binary.
 *
 * The storage layer was an implicit singleton: a static path and a
 * thread-local handle, reachable from any file that included db.h. "Which
 * database" was not a question a signature could ask or a caller could
 * answer, so every helper simply used the one that existed -- and a second
 * one in the same process was not merely unsupported, it was inexpressible.
 *
 * That is now an explicit context, passed from main() into the server and
 * from there into every request and every helper. The property below is what
 * makes that real rather than cosmetic: TWO databases open at once, in one
 * process, on one thread, each answering only for itself. If somebody
 * quietly reintroduces a static -- the easy way to write this -- the second
 * db_open hands back the first database and these assertions fail.
 *
 * Built and run by `make dbctxtest`.
 */
#include "db.h"
#include <fcntl.h> /* F_GETFD: the descriptor count below */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* One user row, so each database has something of its own to be asked about.
 */
static void add_user(struct db *d, const char *email)
{
   sqlite3_stmt *st =
       db_prep(d, "INSERT INTO user(email,pw_salt,pw_hash,pw_iters,created_at)"
                  " VALUES(?,x'00',x'00',1,0)");
   if (!st) {
      ck(0, "the insert prepared");
      return;
   }
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   if (!db_finished(sqlite3_step(st)))
      ck(0, "the insert ran");
   sqlite3_finalize(st);
}

static int count_users(struct db *d)
{
   sqlite3_stmt *st = db_prep(d, "SELECT count(*) FROM user");
   if (!st)
      return -1;
   int n = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
      n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

static int has_user(struct db *d, const char *email)
{
   sqlite3_stmt *st = db_prep(d, "SELECT count(*) FROM user WHERE email=?");
   if (!st)
      return -1;
   sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
   int n = 0;
   if (sqlite3_step(st) == SQLITE_ROW)
      n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

/* ---- HOW MANY DESCRIPTORS THIS PROCESS HOLDS -------------------------
 *
 * The defect this measures is invisible from the API. db_open used to assign
 * this thread's connection pointer directly, so opening context B after
 * context A did not switch away from A -- it FORGOT A. A's connection stayed
 * open with its file, its -wal and its -shm, and nothing could reach it
 * again: not a later switch back (that opens a fresh one), not db_close (it
 * only knows the cached pointer). Every assertion below about A and B still
 * passed, because both databases answer correctly; what was lost was a
 * handle, three descriptors at a time, for the life of the process.
 *
 * So the assertion has to be about something outside the API, and the
 * cheapest true thing is the descriptor table. Counted with fcntl rather than
 * read out of /proc, so the number is the kernel's answer about THIS process
 * and needs no filesystem to be mounted.
 *
 * A count read from the source ("it calls close now") is not a measurement:
 * the whole failure was code that looked like it closed. */
static int fd_count(void)
{
   int n = 0;
   for (int fd = 0; fd < 512; fd++)
      if (fcntl(fd, F_GETFD) != -1)
         n++;
   return n;
}

/* PRAGMA synchronous, as the connection currently has it: 0 OFF, 1 NORMAL,
 * 2 FULL. Read back from sqlite rather than tracked here, so the assertion is
 * about the database's state and not about this test's bookkeeping. */
static int sync_level(struct db *d)
{
   sqlite3_stmt *st = db_prep(d, "PRAGMA synchronous;");
   if (!st)
      return -1;
   int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

int main(void)
{
   const char *pa = "build/srv/test-ctx-a.db";
   const char *pb = "build/srv/test-ctx-b.db";
   char side[256];
   for (const char *const *p = (const char *const[]){pa, pb, 0}; *p; p++) {
      (void)remove(*p);
      snprintf(side, sizeof side, "%s-wal", *p);
      (void)remove(side);
      snprintf(side, sizeof side, "%s-shm", *p);
      (void)remove(side);
   }

   /* THE BASELINE IS TAKEN AFTER ONE FULL CYCLE, not at entry: sqlite's first
    * open allocates things it keeps (its mutexes, its random source), and a
    * baseline taken before them would make the first database look like a
    * leak. What is measured below is the SECOND open onwards. */
   struct db *warm = db_open(pa);
   ck(warm != 0, "a database opens at all");
   db_close(warm);
   int fd_base = fd_count();

   printf("== two databases, one process, one thread ==\n");
   struct db *a = db_open(pa);
   int fd_a     = fd_count();
   struct db *b = db_open(pb);
   int fd_b     = fd_count();
   ck(a != 0, "the first database opens");
   ck(b != 0, "the second opens too");
   /* ---- THE HANDLE THE SECOND OPEN USED TO LOSE ----------------------
    *
    * Both databases work either way, so this is the only place the leak is
    * visible. One connection per thread is the rule sqlite forces on this
    * server (see db.c); opening B therefore has to CLOSE the connection to A,
    * and switching back later reopens it. Assigning over the pointer instead
    * leaves A's connection open, unreachable and unclosable. */
   ck(fd_a > fd_base, "a connection costs descriptors, so this can be "
                      "measured at all");
   ck(fd_b == fd_a, "...and opening a SECOND database on the same thread "
                    "costs NONE: the first connection was closed, not "
                    "dropped");
   /* THE ASSERTION A STATIC FAILS. A singleton hiding behind a pointer hands
    * the same object back twice, and everything below then passes for the
    * wrong reason -- so this is checked before anything is written. */
   ck(a != b, "...and they are DIFFERENT databases, not one handed out twice");
   if (!a || !b || a == b) {
      printf("\nDBCTX TESTS FAILED\n");
      return 1;
   }

   add_user(a, "alice@example.com");
   add_user(b, "bob@example.com");
   add_user(b, "carol@example.com");

   ck(count_users(a) == 1, "the first holds only what it was given");
   ck(count_users(b) == 2, "...and the second only what IT was given");
   ck(has_user(a, "alice@example.com") == 1, "each row is in the database it "
                                             "was written to");
   ck(has_user(a, "bob@example.com") == 0, "...and in no other");
   ck(has_user(b, "alice@example.com") == 0, "...in both directions");

   /* INTERLEAVED, on one thread. The per-thread connection is a cache of the
    * connection to ONE context; switching back and forth must reopen rather
    * than answer from the wrong file. Ten alternations, because a cache that
    * is wrong only on the SECOND switch is the interesting bug. */
   printf("== switching between them, repeatedly ==\n");
   int stable = 1;
   for (int i = 0; i < 10; i++) {
      if (count_users(a) != 1 || count_users(b) != 2)
         stable = 0;
   }
   ck(stable, "alternating between the two never answers from the wrong one");

   /* db_changes is per connection, so it is per CONTEXT too: asking one
    * database what it just changed must not report the other's work. */
   ck(db_exec(a, "UPDATE user SET pw_iters=2") == 1, "a statement runs on the "
                                                     "first");
   ck(db_changes(a) == 1, "...and it reports ITS own row count");
   ck(db_exec(b, "UPDATE user SET pw_iters=2") == 1, "a statement runs on the "
                                                     "second");
   ck(db_changes(b) == 2, "...and reports its own, not the first's");

   printf("== a write nothing re-sends is committed DURABLY ==\n");
   {
      /* WAL with synchronous=NORMAL cannot corrupt the file but does not
       * promise a commit survives a power cut: the frames are fsynced at a
       * checkpoint, not at each commit. The log buckets accept that, because
       * the phone re-pushes whatever the digest says is missing. A pairing,
       * a password or an account has no such second chance, and the HTTP 200
       * is the only record it happened -- so those run at FULL.
       *
       * Both halves are asserted here: that the level really is raised, and
       * that it is put BACK. A connection left at FULL would silently make
       * every later bucket PUT pay an fsync on an SD card this gate exists to
       * protect. */
      ck(sync_level(b) == 1, "the connection starts at NORMAL, the cheap one");

      ck(db_durable_begin(b) == 1, "a durable transaction begins");
      ck(sync_level(b) == 2, "...at FULL, so the commit is fsynced");
      ck(db_durable_commit(b) == 1, "...and commits");
      ck(sync_level(b) == 1, "...leaving the connection back at NORMAL");

      /* The rollback path restores it too -- the one most easily forgotten,
       * because it is the path taken when something has already gone wrong. */
      ck(db_durable_begin(b) == 1, "another durable transaction begins");
      ck(sync_level(b) == 2, "...also at FULL");
      db_durable_rollback(b);
      ck(sync_level(b) == 1, "...and a ROLLBACK restores NORMAL as well");
   }

   printf("== closing one leaves the other alone ==\n");
   db_close(a);
   ck(count_users(b) == 2, "the surviving database still answers");
   add_user(b, "dave@example.com");
   ck(count_users(b) == 3, "...and still accepts writes");
   db_close(b);
   /* TEARDOWN CLOSES EVERY HANDLE IT OWNS. Ten switches happened above, each
    * opening a connection; if any of them had been dropped rather than
    * closed, the count would still be above the baseline here. */
   ck(fd_count() == fd_base, "...and with both contexts closed the process "
                             "holds no more descriptors than before either "
                             "was opened");

   /* A closed context is not reused: opening again is a NEW database object.
    * (Reopening the same FILE is fine and normal -- the CLI does it per
    * subcommand -- but it must not resurrect the old pointer.) */
   struct db *c = db_open(pa);
   ck(c != 0, "the file can be opened again after a close");
   ck(count_users(c) == 1, "...and holds what was written before");
   db_close(c);

   /* ---- A CLOSE THAT FAILS IS NOT A CLOSE ---------------------------
    *
    * sqlite3_close refuses (SQLITE_BUSY) while a prepared statement from this
    * connection is still alive -- a real bug in a caller, and the only way a
    * close here fails. The switch cannot pretend it happened: dropping the
    * pointer is the leak this file measures, and forcing the close would pull
    * the file out from under a statement the caller still holds and step it
    * into freed memory.
    *
    * So the OPEN is refused. Nothing changes, the old connection stays valid,
    * and the caller is told -- in the one currency db_open has -- that it did
    * not get a database.
    *
    * Arranged with a real unfinalized statement rather than an injected
    * fault: this is exactly the shape the failure takes in production. */
   printf("== an open whose predecessor will not close is refused ==\n");
   {
      struct db *e = db_open(pa);
      ck(e != 0, "a database is open on this thread");
      sqlite3_stmt *held = db_prep(e, "SELECT count(*) FROM user");
      ck(held != 0, "...with a statement still alive on its connection");
      int before      = fd_count();
      struct db *busy = db_open(pb);
      ck(busy == 0, "a second database CANNOT be opened while the first "
                    "connection refuses to close");
      ck(fd_count() == before, "...and the refused open left no descriptor "
                               "behind either");
      ck(count_users(e) == 1, "...and the connection that refused to close is "
                              "still usable, still answering for ITS file");
      sqlite3_finalize(held);
      struct db *f = db_open(pb);
      ck(f != 0, "...and once the statement is finalised the same open "
                 "succeeds");
      db_close(e);
      db_close(f);
      ck(fd_count() == fd_base, "...leaving the descriptor count where it "
                                "started");
   }

   printf("== a database that cannot be opened is NULL, not a stub ==\n");
   /* The old API returned 0/1 and left the module in a half-open state; a
    * caller that ignored the return still had "the database" to talk to. */
   ck(db_open("build/srv/no-such-dir/x.db") == 0,
      "an unopenable path yields no context at all");

   printf("\n%s\n", fails ? "DBCTX TESTS FAILED" : "ALL DBCTX TESTS PASSED");
   return fails ? 1 : 0;
}
