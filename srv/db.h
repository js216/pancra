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

#include "proto.h" /* struct req, and the protocol constants */

/* ---- db.c ------------------------------------------------------------ */

/* THE DATABASE, AS A VALUE RATHER THAN A PROCESS-WIDE FACT.
 *
 * Opaque on purpose: what a caller needs is the right to ask this database
 * something, not its internals. main() opens it and hands it to the server,
 * which puts it in every request; a helper that touches storage takes one.
 *
 * The alternative is a static path and a thread-local handle with no name, so
 * "which database" is a question no signature can ask and no caller can
 * answer. */
struct db;

/* Open (creating and migrating if need be). NULL on failure, with the reason
 * printed. */
struct db *db_open(const char *path);
void db_close(struct db *d);

struct sqlite3_stmt *db_prep(struct db *d, const char *sql);

/* A READ THAT STOPPED IS NOT A READ THAT FINISHED.
 *
 * sqlite3_step returns SQLITE_ROW per row and SQLITE_DONE at the end -- but
 * also SQLITE_BUSY (another connection holds the write lock), SQLITE_IOERR
 * (the SD card), and SQLITE_CORRUPT. Written as `while (step == ROW)`, every
 * one of those reads exactly like "no more rows", and the caller then serves
 * a SHORT ANSWER as though it were the whole one.
 *
 * For the sync endpoints that is the worst outcome available: the phone
 * compares hashes to decide what to send, so a truncated digest makes rows
 * the server holds look absent, and a restore hands back a partial history
 * that reads as complete. For a page it is a list quietly missing entries.
 *
 * So every loop ends by asking this what kind of end it reached. It logs the
 * real
 * result and returns 0 for anything but DONE; the caller discards what it
 * built and says so. */
int db_finished(int rc);

/* WHY THE LAST CALL ON THIS CONNECTION FAILED, in sqlite's words. Never NULL.
 *
 * It exists because a caller that handles a failed write has nothing to say
 * about it otherwise: "the update failed" names neither the constraint, the
 * locked database, nor the I/O error, and the difference decides whether an
 * operator waits, frees a card, or restores. The handle itself stays private;
 * this hands out the message and nothing else.
 *
 * USE IT BEFORE THE NEXT DATABASE CALL. The string belongs to sqlite and to
 * the connection this thread holds; the next call on that connection may
 * overwrite or free it. Print it or copy it, do not keep the pointer. It is
 * safe to hand straight to fprintf, which is what every caller does.
 *
 * It reports on THIS THREAD'S connection, because that is the only one a
 * failure on this thread can have happened on -- a worker cannot be handed
 * another worker's error message by accident. */
const char *db_errmsg(struct db *d);

/* A COPY TAKEN WHILE THE SERVER IS RUNNING, through sqlite's own backup API.
 * `cp sync.db` is not a backup in WAL mode -- the recent commits are in
 * sync.db-wal -- so this is the only supported way to take one. 1 on success;
 * the copy is complete and closed when it returns.
 *
 * The destination is checked with db_backup_dest first, so no caller can
 * write a backup onto the database it is a backup OF. */
int db_backup(struct db *d, const char *out_path);

/* ---- A BACKUP MUST NOT BE WRITTEN OVER THE DATABASE IT IS OF ---------
 *
 * The failure is silent and total. `sync backup` stages its copy at
 * <dest>.part and RENAMES it onto <dest>; point <dest> at the live database
 * and the rename unlinks the inode the running server holds open. The server
 * keeps writing -- to an inode with no name, and to a -wal that now belongs to
 * a file nobody can open -- every request still succeeds, and at the next
 * restart the server opens the backup instead and every sync since the backup
 * is simply gone. Nothing reports it, because from the process's point of view
 * nothing failed.
 *
 * BY IDENTITY, NOT BY SPELLING. Two different strings name the same file in
 * every direction that matters here: a symlink in the backups directory, a
 * hard link, `../live/sync.db`, `./sync.db`, the same file under a bind mount.
 * A string comparison against the configured path refuses the one spelling an
 * operator was least likely to type, so the check resolves both sides and
 * compares device and inode as well.
 *
 * WHAT IS REFUSED: the database, its -wal, -shm and -journal sidecars, and the
 * staging names another operation renames ONTO the live database (see the
 * conventions in srv/deploy/lock.sh: `<db>.part` is what this command stages
 * into, `<db>.restoring-<op>` is what restore.sh installs from, and a
 * destination named `*.part` is what the NEXT backup to that name will
 * silently delete).
 *
 * THREE OUTCOMES, not two: a destination that could not be resolved at all is
 * neither safe nor an alias, and a caller that treats "could not resolve" as
 * "go ahead" is the whole defect again. */
enum backup_dest {
   BACKUP_DEST_OK,    /* a name of its own; a backup may be published there */
   BACKUP_DEST_ALIAS, /* it reaches the live database, a sidecar, or a staging
                         name another operation renames onto it */
   BACKUP_DEST_FAIL   /* it could not be resolved; nothing is claimed */
};

/* Whether a backup may be written at `out_path`. Prints the reason for
 * anything but BACKUP_DEST_OK. */
enum backup_dest db_backup_dest(struct db *d, const char *out_path);

/* CHECK A DATABASE FILE WITHOUT TOUCHING IT: sqlite's integrity_check, plus
 * the tables and the schema version that make it THIS server's database rather
 * than merely a valid one. The restore drill is what makes a backup a backup.
 *
 * THE FILE ITSELF IS NEVER OPENED BY SQLITE. It is copied, with its -wal if it
 * has one, into a private directory this call creates and removes, and the
 * COPY is what is opened and checked. Two reasons, and the second is the one
 * that cost data:
 *
 *   - opening a database creates a -wal and a -shm beside it. Verifying a
 *     backup must not litter the backups directory with them.
 *   - noting which sidecars are absent, opening the file, and then unlinking
 *     `<path>-wal` and `<path>-shm` BY NAME does not work: between the note
 *     and the unlink, a server starting on that database creates real ones --
 *     and verification deletes a live write-ahead log, which is every commit
 *     since the last checkpoint. restore.sh verifies a staged file in the
 *     LIVE data directory, so the window is not theoretical.
 *
 * Nothing outside the private directory is ever unlinked.
 *
 * ---- AND THE COPY IS PART OF THE ANSWER --------------------
 *
 * The scratch copy is a COMPLETE COPY OF THE DATABASE: every session cookie,
 * every password hash, every row of every user's record, sitting in a
 * world-readable directory's private subdirectory under a predictable name.
 * Removing it is not tidying up; it is the second half of the operation.
 *
 * So a cleanup that fails is its own outcome and not a footnote on stderr.
 * As a footnote -- scratch_drop returning void, db_verify returning whatever
 * the integrity check said -- `sync verify` prints "verifies" and exits 0
 * with a copy of the live database still on disk, and an operator reading the
 * exit status (which is what restore.sh and every cron wrapper read) is told
 * everything is fine.
 *
 * THE INTEGRITY VERDICT STILL WINS when both go wrong. VERIFY_BAD means the
 * file is not a usable backup, which is the thing the operator asked about
 * and the thing that must not be masked by a cleanup complaint; the leftover
 * is named on stderr in that case too. */
enum verify_result {
   VERIFY_OK,       /* a usable backup, and nothing was left behind */
   VERIFY_BAD,      /* not a usable backup (the leftover, if any, is named) */
   VERIFY_LEFTOVER, /* a usable backup -- but a copy of it is still on disk */
};

/* Verify `path`. Prints the reason for anything but VERIFY_OK, and for
 * VERIFY_LEFTOVER prints the exact path that has to be dealt with by hand. */
enum verify_result db_verify(const char *path);

/* ---- REPAIRING ROWS WHOSE OWNER IS GONE -------------------------------
 *
 * db_open REFUSES a database holding them, because an orphan `session` row is
 * a live cookie for a deleted account and the next account to be given that
 * user id inherits it. This is the supported way to clear them: explicit, run
 * by an operator with the server stopped, in one transaction, and it reports
 * every row it removes.
 *
 * `fix` = 0 lists them and changes nothing. 1 on success (including "there
 * were none"), 0 when the scan or the repair failed -- and then the file is
 * untouched. `removed` receives the count when non-NULL. Only CHILD rows are
 * ever deleted; see the definition. */
int db_fsck(struct db *d, int fix, int64_t *removed);

/* Open a database that db_open would REFUSE for holding orphan rows, so they
 * can be removed. The only caller is the fsck verb; every other check --
 * schema shape, version support, the pragmas -- is applied exactly as usual,
 * because a repair against a layout this build does not know would be a
 * repair guessing. */
struct db *db_open_repair(const char *path);

/* Run a statement with no results; 1 on success. */
int db_exec(struct db *d, const char *sql);

/* ---- A TRANSACTION WHOSE COMMIT SURVIVES A POWER CUT -----------------
 *
 * The database runs WAL with synchronous=NORMAL, which cannot corrupt the
 * file but does NOT guarantee that a committed transaction survives a power
 * failure -- WAL frames are fsynced at a checkpoint, not at each commit.
 *
 * For the log buckets that is deliberate and safe: the phone re-pushes
 * anything the server's digest does not report, so a lost bucket returns by
 * itself. For a write nothing re-sends -- a pairing, a password, an account,
 * a share -- it is not, because the acknowledgement is the only record that
 * it happened.
 *
 * Use these three for that second kind. They raise synchronous to FULL for
 * the length of the transaction and drop it back afterwards, on every path.
 * db_durable_begin returns 0 without having started anything. */
int db_durable_begin(struct db *d);
int db_durable_commit(struct db *d);
void db_durable_rollback(struct db *d);

/* ---- IS THIS THREAD'S CONNECTION UNUSABLE -------------------
 *
 * 1 when a transaction could not be finalized on it -- a ROLLBACK that
 * failed, or one that reported success while sqlite3_get_autocommit still
 * says the connection is inside a transaction. Such a connection is never
 * handed to another request: the next db call on this thread closes it and
 * opens a fresh one, which is what discards the transaction.
 *
 * Production has nothing to do with this answer (the recovery is automatic
 * and the caller has already failed); it exists so a test can state that the
 * poisoning happened, and that the connection afterwards is a working one. */
int db_conn_poisoned(void);

/* 1 when a transaction is ALREADY open on this connection.
 *
 * For the one shape sqlite does not support: a helper that must be atomic in
 * its own right, and is also called from inside a larger transaction. There
 * is no nested BEGIN, so such a helper asks this and brackets itself only
 * when it is the outermost writer -- otherwise it just adds its statements to
 * the transaction it was called inside, and the caller's commit or rollback
 * decides all of them together, which is the right answer either way.
 *
 * session_new is the case: /login calls it with nothing open, and the
 * invitation POST calls it between the account being created and that same
 * transaction committing. Written to always BEGIN, it would have failed every
 * invitation redemption; written to never BEGIN, a plain login's prune, cap
 * and insert would be three separate transactions and a crash between them
 * could leave a cookie in the browser with no row behind it. */
int db_in_transaction(struct db *d);

/* ---- ONE NUMBER, AND WHY THERE IS OR IS NOT ONE ----------------------
 *
 * THREE OUTCOMES, NOT TWO. "The value, and a found flag" makes a prepare
 * that failed, a step that errored (BUSY, LOCKED, CORRUPT, IOERR) and a query
 * that simply matched nothing one answer: not found, value 0.
 *
 * That is the difference between "this user has no time zone" and "the
 * database could not be read", and the pages acted on the first while the
 * second was true: a time zone silently became UTC, an owner lookup that
 * failed made the viewer their own owner, and a "do you have readings" probe
 * that errored reported an empty account. Every one of those renders a
 * plausible page stating something false, at exactly the moment the operator
 * most needs to be told the storage is in trouble.
 *
 * A caller that genuinely does not care can still ignore DB_GET_FAIL -- but
 * it has to do so in writing. */
enum db_get {
   DB_GET_VALUE, /* a row; *out holds it */
   DB_GET_NONE,  /* the query ran to SQLITE_DONE and matched nothing */
   DB_GET_FAIL   /* the database could not answer; *out is untouched */
};

/* `sql` takes one bound integer parameter and selects one column.
 * DB_GET_NONE requires SQLITE_DONE: anything else is DB_GET_FAIL. */
enum db_get db_get_long(struct db *d, const char *sql, int64_t arg,
                        int64_t *out);
int64_t db_last_id(struct db *d);
int db_changes(struct db *d);

#endif
