// SPDX-License-Identifier: GPL-3.0
// syncrun.h --- the seam between the daemon, the operator CLI, and main()
// Copyright 2026 Jakob Kastelic

/* srv/sync.c was one file doing three unrelated jobs: routing HTTP
 * requests for a daemon that runs for months, executing eleven operator
 * commands that run for milliseconds and then exit, and deciding which of the
 * two this invocation is. They shared a file and almost nothing else -- the
 * verbs never route a request, the daemon never reads a password off a
 * terminal -- and the file was 1600 lines in which a change to the pairing
 * route sat forty lines from a change to `sync invites`.
 *
 * Three files now:
 *
 *   srv/syncd.c    THE DAEMON: the API routes, the request handler, and the
 *                  startup that binds a port and serves until killed.
 *   srv/synccmd.c  THE OPERATOR CLI: where a password may come from, the verb
 *                  table that IS the help, and the eleven verbs.
 *   srv/sync.c     WHICH OF THE TWO, and nothing else.
 *
 * WHAT IS SHARED, and why each of these is here rather than duplicated:
 *
 *   the DATABASE PATH. Both halves resolve it the same way from the same
 *   optional [datadir], and a CLI that resolved it differently from the
 *   daemon would operate on a different file than the one being served.
 *
 *   the PRIVACY POSTURE. A umask that makes everything this process creates
 *   unreadable by other accounts, and the check that says so about files that
 *   already exist. Both halves create files; both must be private.
 *
 *   the USAGE TEXT. `sync` with no arguments and `sync <not a verb>` both
 *   print it, and the second is main()'s decision -- so the dispatcher has to
 *   be able to ask for it.
 *
 * NOT PUBLIC: nothing outside these three files includes this.
 */
#ifndef PANCRA_SYNCRUN_H
#define PANCRA_SYNCRUN_H

#include <stdio.h>

struct db;

/* THE DATABASE THIS RUN IS ABOUT. Resolved once from an optional directory
 * (NULL = beside the binary), then read through sync_db_path(). */
void sync_resolve_db(const char *dir);
const char *sync_db_path(void);
const char *sync_db_dir(void);

/* Nothing this process creates is readable by another account. */
void sync_private_umask(void);
/* Is the state private to us? Prints what is wrong and answers 0 if not. */
int sync_state_is_private(const char *dir, const char *dbp, const char *cert,
                          const char *key);

/* THE HELP, which is generated from the verb table in srv/synccmd.c. On
 * stdout with exit 0 when it was asked for; on stderr when it is a
 * correction. */
void sync_cli_usage(FILE *out);

/* Is `argv[1]` one of the operator verbs? main() asks before deciding whether
 * this invocation is a daemon or a command. */
int sync_is_verb(const char *word);

/* Run the operator command in argv. The process exits with what this
 * returns; it never falls through to the daemon. */
int sync_cli_main(int argc, char **argv);

/* Run the daemon: bind, serve, and return only when it stops. */
int sync_daemon_main(int argc, char **argv);

#endif
