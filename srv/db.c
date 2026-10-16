/* SPDX-License-Identifier: GPL-3.0
 * db.c --- the single sqlite file: the front door
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
 *
 * WHAT IS IN THIS FILE, now that the module is four: opening a database and
 * closing it, and the migration run that an open performs. Everything else is
 * dbconn.c (connections and statements), dbschema.c (the schema and its
 * steps) and dbcheck.c (what a database must look like). dbconn.c's header
 * comment says why. */
#include "db.h"
#include "dbint.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bring the file up to DB_SCHEMA_VERSION.
 *
 * 1 when the database is at this build's version afterwards (with `repair`,
 * when it passed the version and version-0 shape checks); 0 when a step
 * failed, a check refused the file, or the file is NEWER than this build
 * understands.
 *
 * EVERY PENDING STEP, EVERY VERSION BUMP AND THE AFTER-CHECK ARE ONE
 * TRANSACTION. Some steps cannot be run twice (an ALTER TABLE ... ADD COLUMN
 * fails on a column that is already there), and the after-check -- the only
 * place the INDEXES are compared -- can run only at the final version,
 * because SHAPES and INDEXES describe the far end. Committed step by step, a
 * later failure (a full disk during step 5's table copy, an index the
 * after-check refuses) would leave earlier steps applied under a version
 * stamp that says otherwise, and every later start would re-run a step that
 * cannot be re-run. As one transaction, any failure rolls back to the file
 * exactly as it was found, and the next start asks the same questions of the
 * same file.
 *
 * `repair` is threaded through from db_open_repair: the file is checked for a
 * version this build understands and, at version 0, for its shape -- and no
 * step runs. fsck needs only sqlite's own catalogue and its foreign-key
 * check, which work at every version; the orphan refusals below are what the
 * repair exists to clear, and the ordinary open after it brings the cleaned
 * file forward. A repair that stopped half way therefore never leaves a file
 * stamped current with its orphans still in it. */
static int migrate(struct db *d, int repair)
{
   sqlite3 *h = db_handle(d);
   int at     = 0;
   if (!version_supported(h, d->path, &at))
      return 0;
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
   if (repair)
      return 1;
   /* ...and the DATA that goes with it. A file that has never been stamped by
    * this build is the only one that can already contain rows the cascades
    * say cannot exist; see no_orphan_rows for why this is not paid on every
    * start. Deliberately AFTER the shape check, so a file that is the wrong
    * shape is told about its shape rather than about the rows in it. */
   if (at == 0 && !no_orphan_rows(h, d->path))
      return 0;
   /* ...and a stamped file the steps cannot finish: step 5 copies `logrow`
    * under its foreign key, so one orphan there fails it. Asked before any
    * step, the same question db_verify asks of a backup, so the refusal says
    * `logrow` holds orphans rather than reporting a failed statement. */
   if (at > 0 && at < DB_SCHEMA_VERSION && !schema_usable(h, d->path, at))
      return 0;
   /* A file that needs no step: every table this build queries exists and
    * looks the way this build queries it. */
   if (at == DB_SCHEMA_VERSION)
      return schema_shapes_ok(h, d->path, 1);
   if (!db_exec(d, "BEGIN IMMEDIATE;"))
      return 0;
   for (int i = 0; i < db_nmigrations; i++) {
      if (db_migrations[i].to <= at)
         continue;
      char bump[64];
      (void)snprintf(bump, sizeof bump, "PRAGMA user_version=%d;",
                     db_migrations[i].to);
      if (!db_exec(d, db_migrations[i].sql) || !db_exec(d, bump)) {
         fprintf(stderr, "sync: schema step %d failed: %s\n",
                 db_migrations[i].to, sqlite3_errmsg(h));
         if (sqlite3_extended_errcode(h) == SQLITE_CONSTRAINT_FOREIGNKEY)
            fprintf(stderr, "  The file holds orphan rows. With the server "
                            "stopped, `sync fsck fix` removes them.\n");
         (void)db_exec(d, "ROLLBACK;");
         return 0;
      }
   }
   /* THE AFTER-CHECK IS INSIDE THE TRANSACTION. sqlite reports the schema as
    * this connection has just changed it, so the check sees the migrated
    * shape, and a failure rolls the whole thing -- tables, indexes and
    * version stamp -- back to the file as it was found. The properties only
    * this check can see (the indexes, with their UNIQUE flags, partial-ness
    * and collations) are the ones that decide whether one email address in
    * two spellings is one account or two, and a file that fails them must
    * not be left marked as this build's. */
   if (!schema_shapes_ok(h, d->path, 1)) {
      (void)db_exec(d, "ROLLBACK;");
      return 0;
   }
   if (!db_exec(d, "COMMIT;")) {
      fprintf(stderr, "sync: schema steps failed to commit: %s\n",
              sqlite3_errmsg(h));
      (void)db_exec(d, "ROLLBACK;");
      return 0;
   }
   return 1;
}

/* THE ONE DOOR THAT SKIPS THE ORPHAN GATE (and, with it, the schema steps).
 *
 * db_open refuses a database holding rows whose owner is gone -- which means
 * the repair that exists to REMOVE those rows could never open the file it is
 * for. `repair` is 1 only from db_open_repair, which the fsck verb calls and
 * nothing else does.
 *
 * It skips the orphan checks and runs no schema step (see migrate). The
 * version support, the version-0 shape check and every pragma are applied as
 * usual: a version-0 file of the wrong shape, or a file from a newer build,
 * is still refused, because a repair that ran against one of those would be a
 * repair guessing at a layout it does not know.
 */

static struct db *db_open_mode(const char *path, int repair)
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
   /* THROUGH THE SAME SWITCH EVERY OTHER OPEN USES. A raw sqlite3_open
    * straight into t_conn is what makes a second db_open on one thread drop
    * the first connection on the floor: still open, no
    * longer reachable, never closed. */
   if (!db_conn_first(d)) {
      free(d);
      return NULL;
   }
   if (!migrate(d, repair)) {
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

struct db *db_open(const char *path)
{
   return db_open_mode(path, 0);
}

/* FOR THE fsck VERB ONLY -- see db.h and db_open_mode above. */
struct db *db_open_repair(const char *path)
{
   return db_open_mode(path, 1);
}
