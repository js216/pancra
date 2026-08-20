// SPDX-License-Identifier: GPL-3.0
// dbmigtest.c --- the database says which schema it is, and moves one step
// Copyright 2026 Jakob Kastelic
//
/* WHAT THIS PINS.
 *
 * The server used to open its database by running one CREATE TABLE IF NOT
 * EXISTS script. That creates a file and opens an existing one with the same
 * code, which is why it lasted -- but it can only ever ADD. Changing a
 * column, a constraint or an index's meaning needs a statement that IF NOT
 * EXISTS skips on precisely the databases that need it, so the first such
 * change would have had to be a script somebody remembers to run against a
 * live board, by hand, once.
 *
 * There is a version in the file now (PRAGMA user_version) and an ordered
 * list of steps. The properties that make that worth having:
 *
 *   1. A FRESH FILE ends at this build's version, with the schema in it.
 *   2. A FILE FROM BEFORE VERSIONING -- the historical schema below, checked
 *      in as the fixture -- upgrades in place, keeps every row, and ends at
 *      the same version. This is the real upgrade the deployed board will do.
 *   3. RE-OPENING is a no-op: the steps that have run do not run again.
 *   4. A FILE FROM THE FUTURE is REFUSED. A newer server wrote it; an older
 *      binary reading it would interpret columns whose meaning it does not
 *      have, which is the case that corrupts a record silently. Failing to
 *      open is loud and reversible.
 *   5. A STEP IS A TRANSACTION: a failure leaves the version where it was,
 *      not halfway through.
 *
 * ...and then what "the same schema" MEANS, which is the second half of this
 * file. A database can hold every table, with every column, in order, with
 * every type and NOT NULL and primary key, and still be a different database:
 *
 *   6. THE COLLATIONS. `user.email` is NOCASE, on the column and in the unique
 *      index over it. Under BINARY, `jk@example.com` and `JK@example.com` are
 *      two accounts -- and since every authorisation decision here starts from
 *      a user id resolved from an email, that is an account boundary, not a
 *      cosmetic difference.
 *   7. THE FOREIGN KEYS AND THEIR CASCADES. Deleting an account is one DELETE
 *      FROM user; the sessions, share tokens, paired app and log rows go with
 *      it by ON DELETE CASCADE and by nothing else.
 *   8. THAT THE CASCADES ARE ENFORCED AT RUNTIME. Declaring them is not
 *      enough: sqlite defaults PRAGMA foreign_keys to OFF on every connection,
 *      so the assertions below delete a user and look for what is left.
 *   9. THE DEFAULTS, and WHETHER A TABLE IS `WITHOUT ROWID`.
 *  10. THAT NO ROW ALREADY POINTS AT A USER THAT IS GONE, in a file this
 *      build has never stamped.
 *
 * Each of those is asserted by building the CANONICAL schema with EXACTLY ONE
 * property changed and requiring the database to be refused -- and, first, by
 * requiring the unchanged canonical schema to be ACCEPTED. That control is
 * half the test: a gate that refuses the real schema is worse than the bug it
 * was written for, and a variant that is refused for some other reason pins
 * nothing at all.
 *
 * Built and run by `make dbmigtest`.
 */
#include "db.h"
#include <pthread.h>  /* the other server, in the TOCTOU case below */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h> /* st_ino: the SAME -wal, not merely a -wal */
#include <time.h>     /* nanosleep, to land inside the window */
#include <unistd.h>

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* THE HISTORICAL SCHEMA, exactly as the server created it before there was a
 * version -- checked in here so the upgrade is tested against what is
 * actually on the board rather than against today's script with the version
 * left off. Only the tables the assertions below touch; the point is the
 * upgrade path, and a table this fixture omits is one the baseline step
 * creates, which is the case that must also work.
 *
 * DO NOT UPDATE THIS when the schema changes. It is a record of what a
 * pre-versioning database looks like, and that does not change. */
static const char SCHEMA_V0[] =
    "CREATE TABLE IF NOT EXISTS user ("
    "  id INTEGER PRIMARY KEY,"
    "  email TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  pw_salt BLOB NOT NULL, pw_hash BLOB NOT NULL, pw_iters INTEGER NOT NULL,"
    "  tz_offset INTEGER,"
    "  display_name TEXT,"
    "  created_at INTEGER NOT NULL);"
    /* THE DEPLOYED SHAPE, copied from the running schema: `line` is the
     * row's TEXT and there is no `text` column. This fixture used to invent
     * `line INTEGER, text TEXT`, so the upgrade it tested was an upgrade of
     * a database that has never existed -- and it passed, because the
     * baseline's CREATE TABLE IF NOT EXISTS skipped the table and the
     * version was stamped over it regardless. That is now the case below. */
    "CREATE TABLE IF NOT EXISTS logrow ("
    "  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    "  log TEXT NOT NULL, bucket INTEGER NOT NULL, line TEXT NOT NULL,"
    "  PRIMARY KEY(user_id, log, bucket, line)) WITHOUT ROWID;";

/* ...and a database whose `logrow` is NOT that: same name, different columns.
 * A file from another program, or from a version of this one whose migration
 * nobody wrote. */
static const char SCHEMA_INCOMPATIBLE[] =
    "CREATE TABLE IF NOT EXISTS logrow ("
    "  user_id INTEGER NOT NULL,"
    "  log TEXT NOT NULL, bucket INTEGER NOT NULL, line INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  PRIMARY KEY(user_id, log, bucket, line));";

/* THE SAME COLUMNS, THE SAME TYPES, AND NO CONSTRAINTS. This is the shape a
 * check that compares only names and types accepts -- and it was accepted:
 * the file was stamped current and `adduser` then wrote a user whose id was
 * NULL, because `id INTEGER` without PRIMARY KEY is not a rowid alias.
 * Everything that makes this server correct under concurrency lives in the
 * constraints: the nonce primary key IS the replay check, the logrow key IS
 * bucket idempotence, and ON CONFLICT needs a unique target or it is a
 * runtime error. */
static const char SCHEMA_NO_CONSTRAINTS[] =
    "CREATE TABLE user ("
    "  id INTEGER, email TEXT, pw_salt BLOB, pw_hash BLOB, pw_iters INTEGER,"
    "  tz_offset INTEGER, display_name TEXT, created_at INTEGER);"
    "CREATE TABLE nonce ("
    "  user_id INTEGER, nonce TEXT, seen_at INTEGER);";

/* ...and the same shape with a UNIQUE index on the wrong column: legitimate
 * rows are then refused, and the one the server's ON CONFLICT names is gone.
 * `CREATE INDEX IF NOT EXISTS` leaves it exactly as it is. */
static const char SCHEMA_WRONG_INDEX[] =
    "CREATE TABLE share ("
    "  owner_id INTEGER NOT NULL, viewer_id INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL, PRIMARY KEY(owner_id, viewer_id));"
    "CREATE UNIQUE INDEX share_viewer ON share(created_at);";

/* ---- THE CANONICAL SCHEMA, AS A FIXTURE ------------------------------
 *
 * A second copy of what db.c's SCHEMA builds -- deliberately, and unlike
 * SCHEMA_V0 above this one DOES track the schema. It is here so that a
 * violating database can be built as "the real thing, with exactly one
 * property changed": a fixture written from scratch per case would differ in
 * whatever the author forgot as well as in the property under test, and would
 * then be refused by some earlier check while appearing to pin a later one.
 *
 * It cannot drift silently. The very first assertion of this half builds it
 * unedited and requires db_open to ACCEPT it and stamp it current, so a change
 * to db.c's schema that is not made here fails loudly, at the control, before
 * any of the variants have a chance to pass for the wrong reason.
 *
 * The whitespace is not the server's; only the semantics are compared. */
static const char SCHEMA_CANON[] =
    "CREATE TABLE user ("
    " id INTEGER PRIMARY KEY,"
    " email TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    " pw_salt BLOB NOT NULL, pw_hash BLOB NOT NULL, pw_iters INTEGER NOT NULL,"
    " tz_offset INTEGER,"
    " display_name TEXT, created_at INTEGER NOT NULL);"

    "CREATE TABLE app ("
    " user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,"
    " key BLOB NOT NULL, label TEXT,"
    " paired_at INTEGER NOT NULL, last_seen INTEGER);"

    "CREATE TABLE pairing ("
    " user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,"
    " code TEXT NOT NULL, expires_at INTEGER NOT NULL,"
    " tries INTEGER NOT NULL DEFAULT 0);"

    "CREATE TABLE logrow ("
    " user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    " log TEXT NOT NULL, bucket INTEGER NOT NULL, line TEXT NOT NULL,"
    " PRIMARY KEY (user_id, log, bucket, line)) WITHOUT ROWID;"

    "CREATE TABLE share ("
    " owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    " viewer_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    " created_at INTEGER NOT NULL,"
    " PRIMARY KEY (owner_id, viewer_id));"
    "CREATE INDEX share_viewer ON share(viewer_id);"

    "CREATE TABLE share_token ("
    " token TEXT PRIMARY KEY,"
    " owner_id INTEGER REFERENCES user(id) ON DELETE CASCADE,"
    " email TEXT, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,"
    " used_at INTEGER);"

    "CREATE TABLE session ("
    " selector TEXT PRIMARY KEY, verifier BLOB NOT NULL,"
    " user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
    " expires_at INTEGER NOT NULL, last_seen INTEGER);"

    "CREATE TABLE nonce ("
    " user_id INTEGER NOT NULL, nonce TEXT NOT NULL, seen_at INTEGER NOT NULL,"
    " PRIMARY KEY (user_id, nonce)) WITHOUT ROWID;"

    "CREATE TABLE login_fail ("
    " email TEXT PRIMARY KEY, n INTEGER NOT NULL, first_at INTEGER NOT NULL);";

/* One textual change to the canonical schema. `from` must occur EXACTLY ONCE
 * -- see build_variant for why that matters more than it looks. */
struct edit {
   const char *from;
   const char *to;
};

/* Build a file with sqlite directly -- NOT through db_open, which is the
 * thing under test. */
static int raw_exec(const char *path, const char *sql)
{
   sqlite3 *h = NULL;
   if (sqlite3_open(path, &h) != SQLITE_OK) {
      sqlite3_close(h);
      return 0;
   }
   char *err = NULL;
   int ok    = sqlite3_exec(h, sql, NULL, NULL, &err) == SQLITE_OK;
   if (!ok)
      fprintf(stderr, "  (fixture failed: %s)\n", err ? err : "?");
   sqlite3_free(err);
   sqlite3_close(h);
   return ok;
}

static int raw_int(const char *path, const char *sql, int *out)
{
   sqlite3 *h = NULL;
   if (sqlite3_open(path, &h) != SQLITE_OK) {
      sqlite3_close(h);
      return 0;
   }
   sqlite3_stmt *st = NULL;
   int ok           = sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK &&
                      sqlite3_step(st) == SQLITE_ROW;
   if (ok)
      *out = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   sqlite3_close(h);
   return ok;
}

static void rm(const char *p)
{
   char wal[256], shm[256];
   (void)snprintf(wal, sizeof wal, "%s-wal", p);
   (void)snprintf(shm, sizeof shm, "%s-shm", p);
   (void)unlink(p);
   (void)unlink(wal);
   (void)unlink(shm);
}

/* Where every one-property variant is built. */
static const char MUT[] = "/tmp/pancra-mig-prop.db";

/* THE CANONICAL SCHEMA WITH EXACTLY THE LISTED CHANGES APPLIED, plus `extra`
 * SQL (rows, usually) if there is any. 1 on success.
 *
 * EACH `from` MUST OCCUR EXACTLY ONCE, and that is enforced rather than
 * assumed. The two ways this kind of fixture goes wrong are both silent: a
 * pattern that matches NOTHING builds the canonical schema, which the gate
 * accepts, so the case fails in a way that looks like an over-strict gate; and
 * a pattern that matches TWICE -- easy here, where four tables declare
 * `user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE` in the same
 * words -- changes two properties, so the refusal cannot be attributed to
 * either. Both are caught here, loudly, as a failed fixture. */
static int build_variant(const char *path, const struct edit *e, int n,
                         const char *extra)
{
   char sql[4096];
   if (snprintf(sql, sizeof sql, "%s", SCHEMA_CANON) >= (int)sizeof sql) {
      fprintf(stderr, "  (fixture: the canonical schema does not fit)\n");
      return 0;
   }
   for (int i = 0; i < n; i++) {
      char *at = strstr(sql, e[i].from);
      if (!at || strstr(at + 1, e[i].from)) {
         fprintf(stderr, "  (fixture: `%s` occurs %s once in the schema)\n",
                 e[i].from, at ? "more than" : "less than");
         return 0;
      }
      size_t flen = strlen(e[i].from), tlen = strlen(e[i].to);
      size_t tail = strlen(at + flen);
      if ((size_t)(at - sql) + tlen + tail + 1 > sizeof sql) {
         fprintf(stderr, "  (fixture: the edited schema does not fit)\n");
         return 0;
      }
      memmove(at + tlen, at + flen, tail + 1);
      memcpy(at, e[i].to, tlen);
   }
   if (!raw_exec(path, sql))
      return 0;
   return !extra || raw_exec(path, extra);
}

/* One property changed, and the three things that must then be true: the
 * fixture really was built, the gate REFUSED the database, and the file was
 * left UNSTAMPED -- because a refusal that has already written this build's
 * version into the file has adopted it in the only way that matters. The next
 * start would find a version-1 file and skip the version-0 gate entirely. */
static void refused(const char *what, const struct edit *e, int n,
                    const char *extra)
{
   char msg[256];
   rm(MUT);
   int built = build_variant(MUT, e, n, extra);
   (void)snprintf(msg, sizeof msg, "%s: the fixture is prepared", what);
   ck(built, msg);
   struct db *d = built ? db_open(MUT) : NULL;
   (void)snprintf(msg, sizeof msg, "...and %s is REFUSED", what);
   ck(built && d == NULL, msg);
   if (d)
      db_close(d);
   int v = -1;
   (void)snprintf(msg, sizeof msg, "...and is left UNSTAMPED, at version 0");
   ck(built && raw_int(MUT, "PRAGMA user_version;", &v) && v == 0, msg);
   rm(MUT);
}

/* OVERWRITE THE SECOND HALF OF THE FILE, leaving page 1 -- the schema -- as it
 * is. 1 if the file was big enough for that to mean anything.
 *
 * This produces the one failure the schema gate cannot otherwise be shown: a
 * scan that returns real rows and THEN fails. Every PRAGMA the shape checks
 * use reads the schema sqlite already parsed into memory, so they cannot stop
 * early from the outside -- but `PRAGMA foreign_key_check` walks the child
 * tables on disk, so a damaged page part-way through logrow ends its scan with
 * SQLITE_CORRUPT after it has already handed back rows.
 *
 * Written as `while (step == ROW)`, that is indistinguishable from a clean
 * finish, and the file would be reported as holding no orphan rows because the
 * search for them broke. */
static int clobber_tail(const char *path)
{
   FILE *f = fopen(path, "r+b");
   if (!f)
      return 0;
   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return 0;
   }
   long sz = ftell(f);
   if (sz < 8192 || fseek(f, sz / 2, SEEK_SET) != 0) {
      fclose(f);
      return 0;
   }
   for (long i = sz / 2; i < sz; i++)
      (void)fputc(0xA5, f);
   return fclose(f) == 0;
}

/* WHAT THE OLD PREFLIGHT ASKED, and nothing more: sqlite says the file is
 * structurally intact, and the three table names are present.
 *
 * Every case below that is meant to pin the NEW preflight must satisfy this
 * one first. Otherwise it pins nothing: a corrupt file is refused by the
 * integrity check that was always there, and a case built out of one would
 * pass whether the compatibility check existed or not. */
static int old_preflight_would_pass(const char *path)
{
   int intact = 0, names = 0;
   if (!raw_int(path,
                "SELECT count(*) FROM pragma_integrity_check() WHERE"
                " integrity_check='ok';",
                &intact))
      return 0;
   if (!raw_int(path,
                "SELECT count(*) FROM sqlite_master WHERE type='table'"
                " AND name IN ('user','app','logrow');",
                &names))
      return 0;
   return intact == 1 && names == 3;
}

/* Which file this is, not merely whether one is there. A -wal deleted and
 * recreated between two `access` calls passes an existence check and has lost
 * every frame that was in it. -1 when there is none. */
static long file_ino(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0 ? (long)st.st_ino : -1;
}

/* ---- THE OTHER SERVER, STARTING IN THE MIDDLE OF A VERIFY ------------
 *
 * db_verify used to check whether `<path>-wal` and `<path>-shm` were absent,
 * open the database, and then unlink those NAMES. Everything between the
 * check and the unlink is a window in which somebody else's real sidecars can
 * appear at exactly those names -- and srv/deploy/restore.sh verifies a file
 * staged in the LIVE data directory of a running board, so the somebody else
 * is the server itself.
 *
 * This thread is that server. It waits until the main thread is inside
 * db_verify, then opens the same database in WAL mode and commits a row,
 * which is what creates a real -wal, and holds the connection so the log
 * stays. It records whether the verification was still running when it
 * finished, so a run whose timing did not land in the window FAILS rather
 * than passing without having tested anything. */
struct racer {
   const char *path;
   volatile int go;        /* the main thread is about to call db_verify */
   volatile int in_verify; /* ...and has not returned from it yet */
   int made;               /* the -wal was really created */
   int in_window;          /* ...before the verification finished */
   sqlite3 *held;          /* left open: closing it removes the -wal again */
};

static void *racer_main(void *arg)
{
   struct racer *r          = arg;
   const struct timespec ms = {0, 1000000};
   while (!r->go)
      nanosleep(&ms, NULL);
   nanosleep(&ms, NULL); /* past the check, well short of the answer */
   if (sqlite3_open(r->path, &r->held) != SQLITE_OK)
      return NULL;
   char *e = NULL;
   int ok  = sqlite3_exec(r->held, "PRAGMA journal_mode=WAL;", NULL, NULL,
                          &e) == SQLITE_OK;
   sqlite3_free(e);
   e  = NULL;
   ok = ok && sqlite3_exec(r->held,
                           "INSERT INTO logrow(user_id,log,bucket,line)"
                           " VALUES(1,'readings',30000,'the other server');",
                           NULL, NULL, &e) == SQLITE_OK;
   sqlite3_free(e);
   r->made      = ok;
   r->in_window = r->in_verify;
   return NULL;
}

/* An account and one row in every table that hangs off it. `pairing` is
 * deliberately inserted WITHOUT `tries`, so the DEFAULT 0 the schema declares
 * is exercised rather than merely inspected. */
static const char POPULATE[] =
    "INSERT INTO user(id,email,pw_salt,pw_hash,pw_iters,created_at)"
    " VALUES(1,'jk@example.com',x'00',x'00',8000,1);"
    "INSERT INTO app(user_id,key,paired_at) VALUES(1,x'01',1);"
    "INSERT INTO pairing(user_id,code,expires_at) VALUES(1,'123456',9);"
    "INSERT INTO session(selector,verifier,user_id,expires_at)"
    " VALUES('sel',x'02',1,9);"
    "INSERT INTO share_token(token,owner_id,created_at,expires_at)"
    " VALUES('tok',1,1,9);"
    "INSERT INTO logrow(user_id,log,bucket,line)"
    " VALUES(1,'readings',20000,'r');";

/* Everything that account owns, counted in one number. */
static const char BELONGINGS[] =
    "SELECT (SELECT count(*) FROM session)+(SELECT count(*) FROM app)"
    "+(SELECT count(*) FROM pairing)+(SELECT count(*) FROM share_token)"
    "+(SELECT count(*) FROM logrow);";

/* This build's version, read back from a file it just made: the test does not
 * restate the number, so bumping it does not mean editing this file. */
static int current_version(const char *tmp)
{
   int v = -1;
   rm(tmp);
   struct db *d = db_open(tmp);
   if (d)
      db_close(d);
   (void)raw_int(tmp, "PRAGMA user_version;", &v);
   rm(tmp);
   return v;
}

int main(void)
{
   printf("dbmigtest: the database says which schema it is\n");
   const char *fresh  = "/tmp/pancra-mig-fresh.db";
   const char *old    = "/tmp/pancra-mig-v0.db";
   const char *future = "/tmp/pancra-mig-future.db";
   const char *probe  = "/tmp/pancra-mig-probe.db";

   int VER = current_version(probe);
   ck(VER >= 1, "this build has a schema version at all");

   /* ---- 1: a fresh file ---- */
   rm(fresh);
   struct db *d = db_open(fresh);
   ck(d != NULL, "a fresh database opens");
   if (d)
      db_close(d);
   int v = -1;
   ck(raw_int(fresh, "PRAGMA user_version;", &v) && v == VER,
      "...and is stamped with this build's schema version");
   int n = -1;
   ck(raw_int(fresh,
              "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
              "name='logrow';",
              &n) &&
          n == 1,
      "...with the schema actually in it");

   /* ---- 2: the file that predates versioning ---- */
   rm(old);
   ck(raw_exec(old, SCHEMA_V0), "the historical schema builds a v0 fixture");
   ck(raw_exec(old, "INSERT INTO user(id,email,pw_salt,pw_hash,pw_iters,"
                    "created_at) VALUES(1,'jk@example.com',x'00',x'00',8000,1);"
                    "INSERT INTO logrow(user_id,log,bucket,line)"
                    " VALUES(1,'readings',20000,'1700000000,120,0,-70,3,7,"
                    "1700000000,-420,0');"),
      "...with a user and a reading in it");
   ck(raw_int(old, "PRAGMA user_version;", &v) && v == 0,
      "...and it reports version 0, as an unversioned file does");

   d = db_open(old);
   ck(d != NULL, "an unversioned database opens");
   if (d)
      db_close(d);
   ck(raw_int(old, "PRAGMA user_version;", &v) && v == VER,
      "...and comes up at this build's version");
   ck(raw_int(old, "SELECT count(*) FROM logrow;", &n) && n == 1,
      "...WITH ITS ROWS: an upgrade that loses data is not an upgrade");
   /* AND THE SERVER'S OWN QUERY WORKS on it -- the shape, not just the row
    * count. `line` is the row's text, and this is the query every page and
    * every digest runs. */
   ck(raw_int(old,
              "SELECT count(*) FROM logrow WHERE user_id=1 AND log='readings'"
              " AND bucket=20000 AND line LIKE '1700000000,%';",
              &n) &&
          n == 1,
      "...and the query the server actually runs still finds it");
   ck(raw_int(old, "SELECT count(*) FROM user;", &n) && n == 1,
      "...and its accounts");
   /* The tables the v0 fixture never had are the ones the baseline step
    * creates -- the case that proves the step really ran. */
   ck(raw_int(old,
              "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
              "name='nonce';",
              &n) &&
          n == 1,
      "...and the tables it did not have are there now");

   /* ---- 3: re-opening changes nothing ---- */
   d = db_open(old);
   ck(d != NULL, "the upgraded database opens again");
   if (d)
      db_close(d);
   ck(raw_int(old, "PRAGMA user_version;", &v) && v == VER,
      "...at the same version, having run no step twice");

   /* ---- 4: a file from the future is refused ---- */
   rm(future);
   d = db_open(future);
   ck(d != NULL, "a database to age forward opens first");
   if (d)
      db_close(d);
   char bump[64];
   (void)snprintf(bump, sizeof bump, "PRAGMA user_version=%d;", VER + 7);
   ck(raw_exec(future, bump), "...and is stamped by a NEWER server");
   d = db_open(future);
   ck(d == NULL, "a database from the future is REFUSED, not opened");
   if (d)
      db_close(d);
   ck(raw_int(future, "PRAGMA user_version;", &v) && v == VER + 7,
      "...and is left exactly as the newer server wrote it");

   /* ---- 5: an INCOMPATIBLE shape is refused, and not stamped ---- */
   /* The case the baseline cannot fix and must not paper over: a `logrow`
    * with the same name and different columns. CREATE TABLE IF NOT EXISTS
    * leaves it exactly as it is, so before this check the file was stamped
    * as current and every query against it failed afterwards, one page at a
    * time, on a database the server had just declared up to date. */
   rm(old);
   ck(raw_exec(old, SCHEMA_INCOMPATIBLE),
      "a database with an incompatible logrow is prepared");
   d = db_open(old);
   ck(d == NULL, "...and is REFUSED, not opened");
   if (d)
      db_close(d);
   ck(raw_int(old, "PRAGMA user_version;", &v) && v == 0,
      "...and its version is NOT advanced: nothing was migrated");
   ck(raw_int(old,
              "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
              "name='nonce';",
              &n) &&
          n == 0,
      "...and no table of ours was created beside it");

   /* ---- 6: the same columns with NO CONSTRAINTS ---- */
   rm(old);
   ck(raw_exec(old, SCHEMA_NO_CONSTRAINTS),
      "a database with the right columns and no constraints is prepared");
   d = db_open(old);
   ck(d == NULL, "...and is REFUSED: the constraints are the mechanism");
   if (d)
      db_close(d);
   ck(raw_int(old, "PRAGMA user_version;", &v) && v == 0,
      "...with its version left at 0");

   /* ---- 7: a UNIQUE index on the wrong column ---- */
   rm(old);
   ck(raw_exec(old, SCHEMA_WRONG_INDEX),
      "a database with a unique index on the wrong column is prepared");
   d = db_open(old);
   ck(d == NULL, "...and is REFUSED: it would reject rows the server accepts");
   if (d)
      db_close(d);

   /* ---- 8: THE CONTROL. The canonical schema is ACCEPTED ---- */
   /* Everything below builds this schema with one property changed and
    * requires a refusal. That means nothing unless the unchanged schema gets
    * through: an over-strict gate would refuse every variant too, and would
    * look exactly like a complete pass. It also catches this fixture drifting
    * away from db.c's, which is the way a copy of a schema normally rots. */
   rm(MUT);
   ck(build_variant(MUT, NULL, 0, NULL),
      "the canonical schema builds a fixture");
   d = db_open(MUT);
   ck(d != NULL, "...and the gate ACCEPTS it, unedited");
   if (d)
      db_close(d);
   ck(raw_int(MUT, "PRAGMA user_version;", &v) && v == VER,
      "...and stamps it with this build's version");
   rm(MUT);

   /* ---- 9: the COLLATIONS, which decide what counts as one account ---- */
   /* THE COLUMN under BINARY while the unique index stays NOCASE. auth.c looks
    * an account up with `WHERE email=?`, which compares under the COLUMN's
    * collation: the row for `jk@example.com` is then invisible to somebody who
    * typed `JK@example.com`, who is told their password is wrong -- and cannot
    * sign up either, because the index still refuses the second row. */
   static const struct edit E_COL_COLL[] = {
       {" email TEXT NOT NULL UNIQUE COLLATE NOCASE,",       " email TEXT NOT NULL,"},
       {" display_name TEXT, created_at INTEGER NOT NULL);",
        " display_name TEXT, created_at INTEGER NOT NULL,"
        " UNIQUE(email COLLATE NOCASE));"                                           },
   };
   refused("`user.email` held under BINARY (its unique index still NOCASE)",
           E_COL_COLL, 2, NULL);

   /* ...and the mirror image: the COLUMN keeps NOCASE and the UNIQUE INDEX
    * over it is BINARY. This is the account-takeover shape. The lookup still
    * matches case-insensitively, so everything looks normal, and the one thing
    * that stopped two spellings of one address becoming two accounts -- the
    * unique index -- has quietly stopped doing it. */
   static const struct edit E_IDX_COLL[] = {
       {" email TEXT NOT NULL UNIQUE COLLATE NOCASE,",
        " email TEXT NOT NULL COLLATE NOCASE,"},
       {" display_name TEXT, created_at INTEGER NOT NULL);",
        " display_name TEXT, created_at INTEGER NOT NULL,"
        " UNIQUE(email COLLATE BINARY));"     },
   };
   refused("`user.email` indexed under BINARY (the column still NOCASE)",
           E_IDX_COLL, 2, NULL);

   /* A collation that appears where none was asked for is the same class of
    * fault pointing the other way. `logrow.line` is part of that table's
    * primary key, and under NOCASE two readings whose text differs only in
    * case COLLIDE -- so `INSERT OR IGNORE INTO logrow` silently discards the
    * second one, on the copy that is meant to be the backup. */
   static const struct edit E_STRAY_COLL[] = {
       {" log TEXT NOT NULL, bucket INTEGER NOT NULL, line TEXT NOT NULL,",
        " log TEXT NOT NULL, bucket INTEGER NOT NULL,"
        " line TEXT NOT NULL COLLATE NOCASE,"},
   };
   refused("a stray NOCASE on `logrow.line`", E_STRAY_COLL, 1, NULL);

   /* A UNIQUE index that is PARTIAL enforces nothing outside its WHERE, while
    * reporting the right name, the right column and the right unique flag.
    * Only PRAGMA index_list's `partial` column says otherwise. */
   static const struct edit E_PARTIAL[] = {
       {"CREATE INDEX share_viewer ON share(viewer_id);",
        "CREATE INDEX share_viewer ON share(viewer_id) WHERE viewer_id > 0;"},
   };
   refused("a PARTIAL `share_viewer` index", E_PARTIAL, 1, NULL);

   /* ---- 10: the FOREIGN KEYS and their referential actions ---- */
   /* A session row with no owner reference: deleting the account leaves a live
    * cookie behind, and the freed user id is handed to the next account. */
   static const struct edit E_FK_GONE[] = {
       {" verifier BLOB NOT NULL, user_id INTEGER NOT NULL REFERENCES user(id)"
        " ON DELETE CASCADE,", " verifier BLOB NOT NULL, user_id INTEGER NOT NULL,"},
   };
   refused("a `session` table with no foreign key at all", E_FK_GONE, 1, NULL);

   /* THE KEY IS DECLARED AND THE ACTION IS NOT. This is the subtler half: the
    * reference is there, so a check that only asked "does it point at user"
    * would pass -- and ON DELETE defaults to NO ACTION, which makes deleting
    * the account fail outright instead of taking the paired app with it. */
   static const struct edit E_FK_ACTION[] = {
       {" user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,"
        " key BLOB NOT NULL,", " user_id INTEGER PRIMARY KEY REFERENCES user(id), key BLOB NOT NULL,"},
   };
   refused("an `app` foreign key that does not CASCADE", E_FK_ACTION, 1, NULL);

   /* A CASCADE WHERE THE SCHEMA DECLARES NONE is refused just as loudly.
    * `nonce` is spent-token evidence, swept by age and not by ownership;
    * cascading it would reopen the replay window across an account being
    * deleted and recreated with the same id. */
   static const struct edit E_FK_EXTRA[] = {
       {" user_id INTEGER NOT NULL, nonce TEXT NOT NULL,",
        " user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,"
        " nonce TEXT NOT NULL,"},
   };
   refused("a `nonce` table that has GROWN a cascade", E_FK_EXTRA, 1, NULL);

   /* ---- 11: the DEFAULT, and WITHOUT ROWID ---- */
   static const struct edit E_DEFAULT[] = {
       {" tries INTEGER NOT NULL DEFAULT 0);", " tries INTEGER NOT NULL);"},
   };
   refused("`pairing.tries` with its DEFAULT 0 missing", E_DEFAULT, 1, NULL);

   /* logrow as an ordinary rowid table. Its table_info is byte-identical and
    * its automatic index covers the same four columns, so nothing else here
    * can see it -- while the largest table on the board quietly stores every
    * row twice. */
   static const struct edit E_ROWID[] = {
       {" PRIMARY KEY (user_id, log, bucket, line)) WITHOUT ROWID;",
        " PRIMARY KEY (user_id, log, bucket, line));"},
   };
   refused("a `logrow` that is an ordinary rowid table", E_ROWID, 1, NULL);

   /* ...and the other direction, so the check is not simply "WITHOUT ROWID is
    * always fine": `share` is an ordinary table and must stay one. */
   static const struct edit E_NOROWID[] = {
       {" PRIMARY KEY (owner_id, viewer_id));",
        " PRIMARY KEY (owner_id, viewer_id)) WITHOUT ROWID;"},
   };
   refused("a `share` that has become WITHOUT ROWID", E_NOROWID, 1, NULL);

   /* ---- 12: rows that already point at a user who is gone ---- */
   /* The schema is exactly canonical; only the DATA is wrong. This is what a
    * file written by anything that did not turn PRAGMA foreign_keys on looks
    * like -- a dump reloaded, a table hand-repaired -- and adopting it would
    * hand those readings to whoever next gets user id 9. */
   refused("a `logrow` whose owner does not exist", NULL, 0,
           "INSERT INTO logrow(user_id,log,bucket,line)"
           " VALUES(9,'readings',20000,'orphan');");

   /* ---- 13: the cascades are ENFORCED, not merely declared ---- */
   /* Everything above checks what the schema SAYS. sqlite defaults PRAGMA
    * foreign_keys to OFF on every connection, so a schema full of correct
    * cascades still deletes nothing unless the server turns it on -- which
    * makes this the assertion that the declarations are load-bearing at all.
    * The database is the server's own, built by db_open. */
   rm(MUT);
   d = db_open(MUT);
   ck(d != NULL, "the server builds its own database to delete an account in");
   if (d) {
      ck(db_exec(d, POPULATE),
         "...with an account, its app, its pairing, its session, its share "
         "token and a reading");
      ck(db_exec(d, "DELETE FROM user WHERE id=1;"),
         "...and the account is deleted, exactly as /settings/delete deletes "
         "it");
      db_close(d);
   }
   ck(raw_int(MUT, BELONGINGS, &n) && n == 0,
      "...and NOTHING of that account is left behind: the cascades really run");

   /* THE PRAGMA IS WHAT MAKES THAT HAPPEN, and this is the proof: the same
    * schema and the same DELETE on a connection opened the ordinary way, with
    * foreign_keys at sqlite's default of OFF, leaves every one of those rows
    * in place. That is what any other tool touching this file does by
    * default, and it is why db_conf refuses a connection it could not turn
    * the pragma on for. */
   rm(MUT);
   d = db_open(MUT);
   ck(d != NULL, "a second such database opens");
   if (d) {
      ck(db_exec(d, POPULATE), "...populated the same way");
      db_close(d);
   }
   ck(raw_exec(MUT, "DELETE FROM user WHERE id=1;"),
      "...and the account is deleted on a connection with foreign_keys at its "
      "DEFAULT");
   ck(raw_int(MUT, BELONGINGS, &n) && n == 5,
      "...and every row SURVIVES: a declared cascade does nothing until the "
      "pragma is on");
   rm(MUT);

   /* ---- 14: A SCAN THAT STOPPED IS NOT A SCAN THAT FINISHED ---- */
   /* The shape gate is a set of loops over PRAGMA results, and sqlite3_step
    * has more ways to end than "no more rows": BUSY, IOERR, CORRUPT. Written
    * as `while (step == ROW)`, every one of them reads as a clean finish, so a
    * database whose shape COULD NOT BE READ is reported as a database whose
    * shape matches -- and stamped current on the strength of it.
    *
    * `PRAGMA foreign_key_check` is the one scan here that walks data on disk
    * rather than the schema sqlite has already parsed, so it is the one that
    * can be made to fail part-way through from outside the process. */
   rm(MUT);
   ck(build_variant(MUT, NULL, 0, NULL),
      "a canonical database is prepared, to be damaged");
   ck(raw_exec(
          MUT,
          "INSERT INTO user(id,email,pw_salt,pw_hash,pw_iters,created_at)"
          " VALUES(1,'jk@example.com',x'00',x'00',8000,1);"
          "INSERT INTO logrow(user_id,log,bucket,line) SELECT 1,'readings',"
          "20000+n, n || ',120,0,-70,3,7,0,-420,0' FROM (WITH RECURSIVE"
          " c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3000)"
          " SELECT n FROM c);"),
      "...with enough readings in it to span many pages");
   ck(clobber_tail(MUT),
      "...and the second half of the file is overwritten, sparing the schema");
   d = db_open(MUT);
   ck(d == NULL,
      "...and it is REFUSED: a scan that broke cannot report the file clean");
   if (d)
      db_close(d);
   ck(raw_int(MUT, "PRAGMA user_version;", &v) && v == 0,
      "...and is left UNSTAMPED, at version 0");
   rm(MUT);

   /* ---- 15: A BACKUP IS CHECKED THE WAY A DATABASE IS OPENED ---- */
   /* db_verify is the preflight a RESTORE runs before the backup displaces
    * the live database. It used to ask two questions -- is the file intact,
    * and does it have three table names -- and every case below satisfies
    * both. That is the point: each one would have been ACCEPTED, replaced a
    * working database, and then failed at startup, with the good copy already
    * gone. */

   /* The control first: a real backup of a real database must still verify.
    * A preflight that refuses good backups is worse than one that is too
    * lenient, because it fires on the day the operator needs it most. */
   rm(MUT);
   d = db_open(MUT);
   ck(d != NULL, "a current, populated database exists to verify");
   if (d) {
      ck(db_exec(d, POPULATE), "...with an account and its rows in it");
      db_close(d);
   }
   ck(db_verify(MUT), "...and db_verify ACCEPTS it");

   /* A DATABASE FROM THE FUTURE. Intact, all three names, every table in
    * place -- and written by a server whose columns mean something this build
    * does not know. This is the isolating case for the version half. */
   char vbump[64];
   (void)snprintf(vbump, sizeof vbump, "PRAGMA user_version=%d;", VER + 7);
   ck(raw_exec(MUT, vbump), "...it is then stamped by a NEWER server");
   ck(old_preflight_would_pass(MUT),
      "...and it still passes integrity_check with all three table names");
   ck(!db_verify(MUT), "...but db_verify REFUSES it: this build cannot use it");
   rm(MUT);

   /* AN INCOMPATIBLE SHAPE. Intact, all three names, current version, and a
    * `session` table with no foreign key -- so restoring it would produce a
    * server whose account deletion silently leaves live cookies behind. The
    * isolating case for the schema half. */
   ck(build_variant(MUT, E_FK_GONE, 1, NULL),
      "a backup whose `session` has lost its foreign key is prepared");
   (void)snprintf(vbump, sizeof vbump, "PRAGMA user_version=%d;", VER);
   ck(raw_exec(MUT, vbump), "...stamped with this build's own version");
   ck(old_preflight_would_pass(MUT),
      "...and it too passes integrity_check with all three table names");
   ck(!db_verify(MUT), "...but db_verify REFUSES it");
   rm(MUT);

   /* AN OLD BACKUP IS STILL A BACKUP. A version-0 file whose tables match is
    * one db_open would migrate in place, so the preflight must not demand the
    * current version -- that would refuse every backup taken before the next
    * schema step. */
   ck(build_variant(MUT, NULL, 0, NULL),
      "a version-0 backup with a matching schema is prepared");
   ck(db_verify(MUT), "...and db_verify ACCEPTS it: db_open would migrate it");

   /* ...but not one that already holds rows whose owner is gone. */
   ck(raw_exec(MUT, "INSERT INTO logrow(user_id,log,bucket,line)"
                    " VALUES(9,'readings',20000,'orphan');"),
      "...until an orphan row is put in it");
   ck(old_preflight_would_pass(MUT),
      "...which changes nothing about integrity_check or the table names");
   ck(!db_verify(MUT), "...and then db_verify REFUSES it");
   rm(MUT);

   /* The check that was already there still works: an empty file is well
    * formed and is not this server's database. */
   ck(raw_exec(MUT, "CREATE TABLE something_else(x);"),
      "a well-formed database that is not ours is prepared");
   ck(!db_verify(MUT), "...and is refused, as it always was");
   rm(MUT);

   /* ---- 16: VERIFICATION NEVER UNLINKS A SIDECAR IT DID NOT CREATE ---- */
   /* Opening a database creates a -wal and a -shm beside it, so verifying a
    * backup had to clean up after itself. It did that by NAME: note which
    * were absent, open the file, remove those names. The names belong to
    * whoever else is using that database, and removing a live -wal is not
    * tidying up -- it is every commit since the last checkpoint. */
   rm(MUT);
   d = db_open(MUT);
   ck(d != NULL, "a database to be verified while somebody else uses it");
   if (d) {
      ck(db_exec(d, POPULATE), "...populated");
      db_close(d);
   }
   char mwal[256], mshm[256];
   (void)snprintf(mwal, sizeof mwal, "%s-wal", MUT);
   (void)snprintf(mshm, sizeof mshm, "%s-shm", MUT);

   /* THE SIDECAR THAT WAS ALREADY THERE. The plainest form: a server is
    * running on this database when somebody verifies it. */
   {
      sqlite3 *other = NULL;
      ck(sqlite3_open(MUT, &other) == SQLITE_OK,
         "another connection opens the same database");
      (void)sqlite3_exec(other, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
      ck(sqlite3_exec(other,
                      "INSERT INTO logrow(user_id,log,bucket,line)"
                      " VALUES(1,'readings',29000,'live');",
                      NULL, NULL, NULL) == SQLITE_OK,
         "...and commits a row, which is what creates a real -wal");
      long was = file_ino(mwal);
      ck(was >= 0, "...so a live write-ahead log exists beside the file");
      (void)db_verify(MUT); /* the verdict is not the point; the -wal is */
      ck(file_ino(mwal) == was,
         "...and verifying that database leaves the SAME -wal in place");
      ck(file_ino(mshm) >= 0, "...and its shared-memory index too");
      sqlite3_close(other);
   }
   rm(MUT);

   /* THE SIDECAR CREATED IN THE WINDOW, which is the defect itself: absent
    * when verification looked, real by the time it unlinked the name. */
   d = db_open(MUT);
   ck(d != NULL, "the database is rebuilt for the timing case");
   if (d) {
      ck(db_exec(d, POPULATE), "...populated again");
      /* Big enough that verifying it takes long enough to be interrupted --
       * and, under the old code, long enough for the window between the
       * check and the unlink to be a real interval rather than a theory. */
      ck(db_exec(d, "INSERT INTO logrow(user_id,log,bucket,line)"
                    " SELECT 1,'readings',20000+n, n || ',120,0,-70,3,7,0,"
                    "-420,0' FROM (WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL"
                    " SELECT n+1 FROM c WHERE n<20000) SELECT n FROM c);"),
         "...with enough rows that a verification takes a moment");
      db_close(d);
   }
   ck(file_ino(mwal) < 0, "...and no write-ahead log beside it to begin with");
   {
      struct racer r = {MUT, 0, 0, 0, 0, NULL};
      pthread_t th;
      ck(pthread_create(&th, NULL, racer_main, &r) == 0,
         "another server is about to start on it");
      r.in_verify = 1;
      r.go        = 1;
      (void)db_verify(MUT);
      r.in_verify = 0;
      pthread_join(th, NULL);
      ck(r.made, "...and it created a real -wal at exactly the name "
                 "verification cleans up");
      ck(r.in_window, "...while the verification was still running: the "
                      "window this is about is the one that was tested");
      ck(file_ino(mwal) >= 0, "...and the -wal is STILL THERE: verification "
                              "does not delete files it did not create");
      ck(file_ino(mshm) >= 0, "...and so is the -shm");
      sqlite3_close(r.held);
   }
   rm(MUT);

   /* ---- 17: AND IT STILL FAILS A BACKUP THAT IS ACTUALLY BROKEN ---- */
   /* The whole preflight is worthless if it stopped asking sqlite. This file
    * opens, holds all three table names, reports this build's version and
    * describes the right schema -- everything the checks around
    * integrity_check ask -- and its data pages are gone. */
   rm(MUT);
   d = db_open(MUT);
   ck(d != NULL, "a database that will be corrupted is prepared");
   if (d) {
      ck(db_exec(d, POPULATE), "...populated");
      ck(db_exec(d, "INSERT INTO logrow(user_id,log,bucket,line)"
                    " SELECT 1,'readings',20000+n, n || ',120,0,-70,3,7,0,"
                    "-420,0' FROM (WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL"
                    " SELECT n+1 FROM c WHERE n<3000) SELECT n FROM c);"),
         "...over enough pages for the damage to miss the schema");
      db_close(d);
   }
   ck(clobber_tail(MUT), "...and the second half of the file is overwritten");
   ck(raw_int(MUT, "SELECT count(*) FROM sqlite_master WHERE type='table'"
                   " AND name IN ('user','app','logrow');",
              &n) &&
          n == 3,
      "...leaving all three table names readable");
   ck(raw_int(MUT, "PRAGMA user_version;", &v) && v == VER,
      "...and this build's own schema version");
   ck(!db_verify(MUT),
      "...and db_verify REFUSES it: integrity_check is still asked, and is "
      "still the only thing that sees this");
   rm(MUT);

   rm(fresh);
   rm(old);
   rm(future);
   rm(probe);
   printf("%s\n", fails == 0
                      ? "dbmigtest: every database says which schema it is"
                      : "dbmigtest: FAILED");
   return fails == 0 ? 0 : 1;
}
