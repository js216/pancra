// SPDX-License-Identifier: GPL-3.0
// dbint.h --- the seam between db.c and dbbackup.c, and nothing wider
// Copyright 2026 Jakob Kastelic

/* NOT PART OF THE DATABASE'S INTERFACE. srv/db.h is that, and it deliberately
 * hands out no sqlite3 handles: a caller that holds one can bypass every
 * durability, transaction and schema rule the module exists to enforce.
 *
 * srv/db.c and srv/dbbackup.c are one module split across two files because
 * the first passed the build's line ceiling. This header is the one thing the
 * second half needs from the first, and it is scoped to say so. Nothing else
 * should include it. */
#ifndef PANCRA_DBINT_H
#define PANCRA_DBINT_H

#include <sqlite3.h>

/* THE CONTEXT, WHICH db.h KEEPS OPAQUE ON PURPOSE.
 *
 * A caller outside this module gets a pointer and no members: the path a
 * database was opened from is not something a request handler should be able
 * to read, still less rewrite. dbbackup.c needs it -- deciding whether a
 * backup destination names the live file IS a comparison of paths -- and it is
 * the same module. */
struct db {
   char path[512];
};

/* This thread's connection to `d`, opened on first use. See dbconn.c. */
sqlite3 *db_handle(struct db *d);

/* The FIRST connection for a context, taken by db_open before it validates or
 * migrates. Separate from db_handle only because it is the one call that may
 * fail into "this database cannot be opened at all". */
sqlite3 *db_conn_first(struct db *d);

/* WHAT A DATABASE MUST LOOK LIKE (srv/dbcheck.c). `after` says whether this
 * is the check that runs once the migration has finished, which is the only
 * one entitled to demand the CURRENT shape. */
int schema_shapes_ok(sqlite3 *h, const char *path, int after);

/* Rows whose owner no longer exists. 0 when the file holds any -- see the
 * fsck verb, which is the supported way to clear them. */
int no_orphan_rows(sqlite3 *h, const char *path);

/* ---- THE SCHEMA, AS THE STEPS THAT BUILD IT --------------------------
 *
 * srv/dbschema.c holds the statements; srv/db.c runs them. The two are apart
 * because they answer different questions -- "what is in this database" and
 * "how does an existing file get there" -- and a reader of either should not
 * have to read the other.
 *
 * One step. `sql` may hold several statements; they all run in the same
 * transaction as the version bump. */
struct migration {
   int to; /* the version the database is AT once this step commits */
   const char *sql;
};

/* The ordered steps, and how many. Step i takes a database to
 * db_migrations[i].to; the last one lands on DB_SCHEMA_VERSION. */
extern const struct migration db_migrations[];
extern const int db_nmigrations;

/* The version this build understands. Bump it in the same commit as the
 * migration that needs it, and never renumber: the number is written into
 * every database this server has ever opened. */
#define DB_SCHEMA_VERSION 3

/* CAN THIS SERVER OPEN A FILE AT SCHEMA VERSION `at`? Answered against the
 * shapes and the orphan rules, with the "in between" versions deliberately
 * accepted -- the migration steps are the contract for those. db_verify asks
 * it of a restored copy; db_open asks it of the live file. */
int schema_usable(sqlite3 *h, const char *path, int at);

/* WHAT VERSION IS THIS FILE, AND CAN THIS BUILD READ IT? Fills *at with the
 * stamp found. 0 when the file is from a NEWER server than this one, which is
 * the one case that is refused outright rather than migrated. */
int version_supported(sqlite3 *h, const char *path, int *at);

#endif
