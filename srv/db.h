/* SPDX-License-Identifier: GPL-3.0
 * db.h --- the sqlite handle and the statement helpers
 * Copyright 2026 Jakob Kastelic
 *
 * Split out of sync.h, which had grown to hold the public surface of SEVEN
 * modules -- 53 declarations that every page file inherited whole, whether it
 * touched them or not. A header that names one module can be read in one
 * sitting and tells you what depends on what.
 */
#ifndef DB_H
#define DB_H

#include "sync.h" /* struct req, and the protocol constants */

/* ---- db.c ------------------------------------------------------------ */
int db_open(const char *path);
void db_close(void);
struct sqlite3_stmt *db_prep(const char *sql);
/* Run a statement with no results; 1 on success. */
int db_exec(const char *sql);
long db_one_long(const char *sql, long arg, int *found);
long db_last_id(void);
int db_changes(void);

#endif
