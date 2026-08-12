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
#include "sync.h"
#include <sqlite3.h>
#include <stdio.h>
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
static _Thread_local sqlite3 *db;
static sqlite3 *db_first; /* the thread that called db_open */
static char db_path[512];

/* Pragmas every connection needs. Split out of db_open because a worker's
 * connection is opened lazily and must be configured identically. */
static void db_conf(sqlite3 *h, int worker)
{
   char *e = NULL;
   /* WAL survives a power cut mid-write with the committed data intact, which
    * matters on a board with no battery behind it. NORMAL rather than FULL:
    * one fsync per checkpoint instead of one per transaction is the
    * difference between a bearable and an unbearable SD card on this
    * hardware, and WAL's NORMAL still cannot corrupt the file. */
   sqlite3_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, &e);
   sqlite3_free(e);
   e = NULL;
   sqlite3_exec(h, "PRAGMA synchronous=NORMAL;", NULL, NULL, &e);
   sqlite3_free(e);
   e = NULL;
   /* The first connection keeps the ~2 MB cache. A worker's is 256 kB: ten of
    * them at 2 MB would be twenty megabytes on a board with about nineteen
    * free, which is the whole reason the pages are serialised elsewhere. */
   sqlite3_exec(h,
                worker ? "PRAGMA cache_size=-256;" : "PRAGMA cache_size=-2000;",
                NULL, NULL, &e);
   sqlite3_free(e);
   e = NULL;
   sqlite3_exec(h, "PRAGMA journal_size_limit=1048576;", NULL, NULL, &e);
   sqlite3_free(e);
   e = NULL;
   sqlite3_exec(h, "PRAGMA busy_timeout=3000;", NULL, NULL, &e);
   sqlite3_free(e);
   e = NULL;
   sqlite3_exec(h, "PRAGMA foreign_keys=ON;", NULL, NULL, &e);
   sqlite3_free(e);
}

/* This thread's connection, opened on first use. */
static sqlite3 *H(void)
{
   if (!db && db_path[0]) {
      if (sqlite3_open(db_path, &db) != SQLITE_OK) {
         fprintf(stderr, "sync: worker cannot open %s: %s\n", db_path,
                 db ? sqlite3_errmsg(H()) : "?");
         if (db) {
            sqlite3_close(db);
            db = NULL;
         }
         return NULL;
      }
      db_conf(db, 1);
   }
   return db;
}

int db_exec(const char *sql)
{
   char *e = NULL;
   if (sqlite3_exec(H(), sql, NULL, NULL, &e) == SQLITE_OK)
      return 1;
   fprintf(stderr, "sync: sql: %s\n", e ? e : "?");
   sqlite3_free(e);
   return 0;
}

struct sqlite3_stmt *db_prep(const char *sql)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(H(), sql, -1, &st, NULL) != SQLITE_OK) {
      fprintf(stderr, "sync: prepare: %s\n%s\n", sqlite3_errmsg(H()), sql);
      return NULL;
   }
   return st;
}

long db_one_long(const char *sql, long arg, int *found)
{
   if (found)
      *found = 0;
   sqlite3_stmt *st = db_prep(sql);
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, arg);
   long v = 0;
   if (sqlite3_step(st) == SQLITE_ROW) {
      v = (long)sqlite3_column_int64(st, 0);
      if (found)
         *found = 1;
   }
   sqlite3_finalize(st);
   return v;
}

/* Rows changed by the most recent statement on THIS thread's connection.
 *
 * For a DELETE, sqlite3_step returns SQLITE_DONE whether it matched a
 * thousand rows or none -- so "the statement ran" and "something was deleted"
 * are different questions, and the pages that report a revocation were
 * answering the first while claiming the second. */
int db_changes(void)
{
   return sqlite3_changes(H());
}

long db_last_id(void)
{
   return (long)sqlite3_last_insert_rowid(H());
}

/* The schema. Every statement is IF NOT EXISTS, so opening an existing file
 * is the same code path as creating one. */
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

int db_open(const char *path)
{
   /* REQUIRED: the build sets SQLITE_OMIT_AUTOINIT, which drops the implicit
    * initialisation every other API call would otherwise do for us. Without
    * this the first sqlite3_open dereferences an uninitialised global. */
   if (sqlite3_initialize() != SQLITE_OK) {
      fprintf(stderr, "sync: sqlite3_initialize failed\n");
      return 0;
   }
   int n = snprintf(db_path, sizeof db_path, "%s", path);
   if (n <= 0 || n >= (int)sizeof db_path) {
      fprintf(stderr, "sync: database path too long\n");
      return 0;
   }
   if (sqlite3_open(path, &db) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot open %s: %s\n", path,
              db ? sqlite3_errmsg(db) : "?");
      return 0;
   }
   db_first = db;
   db_conf(db, 0);
   if (!db_exec(SCHEMA))
      return 0;
   /* Reclaim the log left by the previous run. The size limit above only
    * takes effect AFTER a checkpoint, and this server is stopped by being
    * killed rather than closed, so nothing ever checkpoints on the way out --
    * the log survives restarts at whatever size the largest sync made it.
    * Doing it here means a restart is also the cleanup. */
   db_exec("PRAGMA wal_checkpoint(TRUNCATE);");
   return 1;
}

void db_close(void)
{
   /* Closes THIS thread's connection. The workers' own handles are closed by
    * the process exiting; there is no worker teardown path to hook, and a
    * pool thread lives as long as the server does. */
   if (db) {
      sqlite3_close(db);
      if (db == db_first)
         db_first = NULL;
      db = NULL;
   }
   db_path[0] = '\0';
}
