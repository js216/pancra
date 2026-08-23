/* SPDX-License-Identifier: GPL-3.0
 * dbschema.c --- the schema, and the ordered steps that move it
 * Copyright 2026 Jakob Kastelic
 *
 * See dbconn.c for how this module is divided. This file is DATA: the V1
 * baseline, each later version's statements, and the table that puts them in
 * order. It holds no logic beyond what a migration step needs to be, which is
 * why it can be read on its own to answer "what is in this database". */
#include "db.h"
#include "dbint.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THE SCHEMA, AND HOW IT MOVES.
 *
 * V1 is the baseline below: every statement IF NOT EXISTS, so creating a file
 * and opening an existing one are the same code path. That is enough to ADD a
 * table or an index and no more -- it cannot change a column, a constraint or
 * an index's meaning, because the CREATE it would need is skipped on exactly
 * the databases that need it. A server with no version therefore could not
 * evolve at all, and the first change that needed to would have had to be
 * written as a one-off script somebody remembers to run.
 *
 * So the file carries a VERSION (PRAGMA user_version, which sqlite stores in
 * the header and costs nothing) and this is a list of ordered steps:
 *
 *   - each step runs in ITS OWN TRANSACTION, together with the bump of
 *     user_version, so a failure leaves the database at a whole version, not
 *     halfway into a step;
 *   - steps run in order, and only the ones this file has not yet had;
 *   - a file from the FUTURE -- a version this build does not know -- is
 *     REFUSED. It was written by a newer server, and an older binary reading
 *     it would be interpreting columns whose meaning it does not have. That
 *     is the case that silently corrupts a record, so it fails to open
 *     instead, which is loud and reversible (run the newer binary).
 *
 * A file that predates versioning reports version 0 and is not empty; step 1
 * is the baseline, and every statement in it is IF NOT EXISTS, so applying it
 * to such a file changes nothing but the version. That is what makes the
 * upgrade of a file written before versioning safe.
 */
static const char SCHEMA[] =
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS user ("
    "  id INTEGER PRIMARY KEY,"
    "  email TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    /* (pw_kdf is NOT here: it arrives in step 3. This baseline is version 1
     * exactly, and every database in the field has already run it -- adding a
     * column here makes step 3 fail on a fresh install with "duplicate column
     * name", which is exactly what the note below this list warns about.
     * Measured, by doing it.) */
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
     * server already holds is a no-op rather than a duplicate -- the same
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

/* ---- V2: THE TWO LOOKUPS THAT BOUND THE TWO CREDENTIAL TABLES --------
 *
 * `share_token` and `session` are the two tables this server writes a
 * credential into, and the only thing that removes a row from either one is
 * the holder of that exact credential coming back for it. Both are pruned and
 * capped per owner (auth.c: share_token_mint and session_new), and
 * both of those rules are a question about ONE account's rows in a table that
 * holds everybody's.
 *
 * Without these indexes each of those questions is a full scan of the table,
 * which is precisely backwards: the work of keeping a table small would grow
 * with the size of the table, and would be paid on the request path -- every
 * mint and every login. The settings page's own list of live links
 * (settings.c) is the same query with the same scan, and it is rendered on
 * every visit to that page.
 *
 * The trailing column in each is what the rule ORDERS BY, so the index
 * answers "this owner's rows, oldest first" in one seek rather than a scan
 * plus a sort:
 *
 *   share_token(owner_id, created_at)  -- the live-link count, and the
 *                                         settings page's ORDER BY created_at
 *   session(user_id, last_seen)        -- the per-user session cap's
 *                                         ORDER BY last_seen, and the
 *                                         "sign out everywhere" delete
 *
 * NEITHER IS UNIQUE. An owner may hold several live links and several
 * sessions; the caps are enforced by counting, not by a constraint, because
 * the cap is a policy number that has changed once already and a unique index
 * would make changing it a schema migration. */
static const char SCHEMA_V2[] =
    "CREATE INDEX IF NOT EXISTS share_token_owner"
    "  ON share_token(owner_id, created_at);"
    "CREATE INDEX IF NOT EXISTS session_user ON session(user_id, last_seen);";

/* ---- V3: THE PASSWORD HASH SAYS WHICH KDF MADE IT -----------
 *
 * The rows held a salt, a hash and an iteration count, and verification was
 * hard-wired to PBKDF2-HMAC-SHA256. That is fine until the day the KDF has to
 * change -- and on that day there is no way to tell an old hash from a new
 * one, so every account would have to be migrated in one step, offline, with
 * no rolling upgrade and no way back. A column that says which function made
 * the hash is what makes the two coexist: a row is verified by ITS OWN
 * algorithm, and a successful login re-derives it under the current one
 * (authuser.c).
 *
 * DEFAULT 1 IS NOT A GUESS. Every row that exists when this step runs was
 * written by the only KDF this program has ever used, which is version 1
 * (PW_KDF_PBKDF2_SHA256 in proto.h). The parameters that version needs are
 * already in the row: pw_iters and the salt's length.
 *
 * NOT NULL, so a row cannot arrive with the question unanswered -- an
 * unknown KDF is refused at verification, and "no KDF recorded" would be a
 * third state nobody would handle. */
static const char SCHEMA_V3[] =
    "ALTER TABLE user ADD COLUMN pw_kdf INTEGER NOT NULL DEFAULT 1;";

/* The version this build understands. Bump it in the same commit as the
 * migration that needs it, and never renumber: the number is written into
 * every database this server has ever opened. */
/* PUBLISHED THROUGH dbint.h, because the file that RUNS these steps is db.c
 * and the file that says what they are is this one. The version number is
 * there too: a build that raised one without the other would migrate to a
 * stamp no step produces. */
const struct migration db_migrations[] = {
    {1, SCHEMA   },
    {2, SCHEMA_V2},
    {3, SCHEMA_V3},
    /* Add the next step here, with `to` one higher, and raise
     * DB_SCHEMA_VERSION to match. Do NOT edit an earlier step: databases in
     * the field have already run it, and changing it makes this list a
     * description of what a fresh install gets rather than a record of how
     * every install arrived. */
};

const int db_nmigrations =
    (int)(sizeof db_migrations / sizeof db_migrations[0]);
