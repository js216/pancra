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
/* `repair` is threaded through from db_open_repair and does exactly one
 * thing: it suppresses the orphan-row refusal below, so the fsck verb can
 * open the file whose orphan rows it exists to remove. Every other check
 * still applies. */
static int migrate(struct db *d, int repair)
{
   sqlite3 *h = db_handle(d);
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
   if (at == 0 && !repair && !no_orphan_rows(h, d->path))
      return 0;
   int ran = 0;
   for (int i = 0; i < db_nmigrations; i++) {
      if (db_migrations[i].to <= at)
         continue;
      /* THE STEP AND THE VERSION BUMP ARE ONE TRANSACTION. Apart, a crash
       * between them leaves a database that has had the change and does not
       * say so -- and the next start runs the step again. */
      char bump[64];
      (void)snprintf(bump, sizeof bump, "PRAGMA user_version=%d;",
                     db_migrations[i].to);
      if (!db_exec(d, "BEGIN IMMEDIATE;") ||
          !db_exec(d, db_migrations[i].sql) || !db_exec(d, bump)) {
         fprintf(stderr, "sync: schema step %d failed: %s\n",
                 db_migrations[i].to, sqlite3_errmsg(h));
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      /* ...AND SO IS THE CHECK, on the last step. THIS IS WHAT "NOT STAMPED"
       * MEANS.
       *
       * Run the after-check once the final COMMIT has happened and a file it
       * refuses has nevertheless had its user_version written -- and the
       * properties only the after-check can see (the indexes, with their
       * UNIQUE flags, their partial-ness and their collations) are exactly
       * the ones that decide whether one email address in two spellings is
       * one account or two. Such a file is refused, correctly, and left
       * marked as this build's. The next start
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
      if (db_migrations[i].to == DB_SCHEMA_VERSION &&
          !schema_shapes_ok(h, d->path, 1)) {
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      if (!db_exec(d, "COMMIT;")) {
         fprintf(stderr, "sync: schema step %d failed to commit: %s\n",
                 db_migrations[i].to, sqlite3_errmsg(h));
         (void)db_exec(d, "ROLLBACK;");
         goto abandon;
      }
      at  = db_migrations[i].to;
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

/* THE ONE DOOR THAT SKIPS THE ORPHAN GATE, and nothing else skips.
 *
 * db_open refuses a database holding rows whose owner is gone -- which means
 * the repair that exists to REMOVE those rows could never open the file it is
 * for. `repair` is 1 only from db_open_repair, which the fsck verb calls and
 * nothing else does.
 *
 * It skips exactly one check. The schema shapes, the version support and
 * every pragma are applied as usual: a file of the wrong shape or from a
 * newer build is still refused, because a repair that ran against one of
 * those would be a repair guessing at a layout it does not know. */

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
