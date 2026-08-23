/* SPDX-License-Identifier: GPL-3.0
 * dbcheck.c --- what a database must look like, and the refusals
 * Copyright 2026 Jakob Kastelic
 *
 * See dbconn.c for how this module is divided. This file answers two
 * questions and nothing else: does this file have the shape this build
 * expects (columns, types, constraints, collations, foreign keys, indexes,
 * rowid-ness, no orphan rows), and is its schema version one this build may
 * open at all.
 *
 * It is separate from the migration steps deliberately: the steps say how to
 * MOVE a database, and these say what it must BE afterwards. A file that
 * conflated them would let a step be its own proof. */
#include "db.h"
#include "dbint.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
   /* ---- THE SHAPE A VERSION-0 FILE HAS --------------------
    *
    * `cols` describes the table at the LATEST schema version, and that is
    * what a migrated file is checked against. A file that has never been
    * stamped is checked BEFORE any step runs (see db.c), and it has the
    * shape the baseline created -- which is not the same thing the moment a
    * migration ADDS A COLUMN. Without this, the first such migration refuses
    * to open exactly the databases it exists to bring forward: measured, on
    * the step that added user.pw_kdf.
    *
    * NULL means "the same as `cols`", which is true of every table no
    * migration has altered. */
   const char *cols0;

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

/* THE `user` ROW CARRIES TWO SHAPES, and the order of its columns is not a
 * style choice.
 *
 * pw_kdf IS LAST because `ALTER TABLE ADD COLUMN` appends: a database that
 * has run schema step 3 has it after created_at, and so does a fresh install,
 * which runs the baseline and then that step. Writing it where it reads best
 * in the CREATE refuses every database in existence. Measured.
 *
 * AND THE SECOND SHAPE IS THE SAME ONE WITHOUT pw_kdf. That column arrives in
 * schema step 3, so a file that has never been stamped does not have it yet
 * and must not be refused for that.
 */
static const struct table_shape SHAPES[] = {
    {"user",
     "id INTEGER#1,email TEXT!~NOCASE,pw_salt BLOB!,pw_hash BLOB!,pw_iters "
     "INTEGER!,tz_offset INTEGER,display_name TEXT,created_at INTEGER!,pw_kdf "
     "INTEGER!=1",                                                                   "id INTEGER#1,email TEXT!~NOCASE,pw_salt BLOB!,pw_hash BLOB!,pw_iters "
     "INTEGER!,tz_offset INTEGER,display_name TEXT,created_at INTEGER!", "",                                            1},
    {"app",
     "user_id INTEGER#1,key BLOB!,label TEXT,paired_at INTEGER!,last_seen "
     "INTEGER",                                                                      0,    "user_id->user.id del=CASCADE upd=NO ACTION",  1},
    /* `tries INTEGER NOT NULL DEFAULT 0` -- the default is checked because it
     * is the difference between "a pairing row inserted without a try count
     * starts at zero" and "that INSERT fails on the NOT NULL". Today every
     * INSERT in pair.c names the column, so nothing depends on it at runtime;
     * it is pinned so that the file and the schema this build believes it has
     * cannot drift apart unnoticed, and so a later statement that omits the
     * column meets the default the schema promises. */
    {"pairing",
     "user_id INTEGER#1,code TEXT!,expires_at INTEGER!,tries INTEGER!=0",            0,
     "user_id->user.id del=CASCADE upd=NO ACTION",                                                                                        1},
    /* `line` is the row's TEXT, not a line number: a file whose logrow says
     * INTEGER there is the incompatible shape this check exists for. */
    {"logrow",      "user_id INTEGER!#1,log TEXT!#2,bucket INTEGER!#3,line TEXT!#4",
     0,                                                                                    "user_id->user.id del=CASCADE upd=NO ACTION",  0},
    {"share",       "owner_id INTEGER!#1,viewer_id INTEGER!#2,created_at INTEGER!",  0,
     "owner_id->user.id del=CASCADE upd=NO ACTION,"
     "viewer_id->user.id del=CASCADE upd=NO ACTION",                                                                                      1},
    {"share_token",
     "token TEXT#1,owner_id INTEGER,email TEXT,created_at INTEGER!,expires_at "
     "INTEGER!,used_at INTEGER",                                                     0,    "owner_id->user.id del=CASCADE upd=NO ACTION", 1},
    {"session",
     "selector TEXT#1,verifier BLOB!,user_id INTEGER!,expires_at "
     "INTEGER!,last_seen INTEGER",                                                   0,    "user_id->user.id del=CASCADE upd=NO ACTION",  1},
    /* NO FOREIGN KEY, deliberately. A nonce row is spent-token evidence: it
     * exists to make a replayed request collide, and it is swept by age
     * (auth.c), not by ownership. Keeping it out of the cascade also keeps the
     * replay window closed across an account being deleted and recreated with
     * the same id. The empty string SAYS that, so a file that has grown a
     * cascade here -- which would silently reopen that window -- is refused. */
    {"nonce",       "user_id INTEGER!#1,nonce TEXT!#2,seen_at INTEGER!",             0,    "",                                            0},
    /* Also no foreign key: this throttles attempts against an email ADDRESS,
     * which very often is not an account at all -- that is the whole point of
     * throttling it. A REFERENCES here would make every failed login against a
     * non-existent address a constraint error rather than a recorded attempt.
     */
    {"login_fail",  "email TEXT#1,n INTEGER!,first_at INTEGER!",                     0,    "",                                            1},
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
     * the per-user session cap and "sign out everywhere" a seek rather than a
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
   /* THE TERMINAL RESULT IS KEPT.
    *
    * "The loop either reads every index or stops, and a partial read is
    * caught by the OVERFLOW test below" is not true: the overflow test
    * catches
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
int schema_shapes_ok(sqlite3 *h, const char *path, int after)
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
      /* THE SHAPE THIS FILE SHOULD HAVE, which is not the same question
       * before and after the steps run: `after` is 0 for a file that has
       * never been stamped, and such a file has the baseline's columns. */
      const char *want =
          (!after && SHAPES[i].cols0) ? SHAPES[i].cols0 : SHAPES[i].cols;
      int ok = shape_ok(h, SHAPES[i].name, want);
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
             path, SHAPES[i].name, want);
         return 0;
      }
      /* ---- THE PROPERTIES table_info DOES NOT REPORT ------------------
       *
       * A table can have every column, in order, with every type, NOT NULL
       * and primary-key mark this build expects, and still be the wrong
       * table: the cascades that make account deletion a deletion, and the
       * rowid decision that makes logrow one copy of the data rather than
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
int no_orphan_rows(sqlite3 *h, const char *path)
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
/* NOT static: db_verify asks it of a restored copy. See srv/dbint.h. */
int version_supported(sqlite3 *h, const char *path, int *at)
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
/* NOT static: db_verify asks exactly this question of a file somebody hands
 * back, and it lives in dbbackup.c (the other half of this module). See
 * srv/dbint.h. */
int schema_usable(sqlite3 *h, const char *path, int at)
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
