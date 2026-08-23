/* SPDX-License-Identifier: GPL-3.0
 * sync.c --- routing, and the handful of things done from the command line
 * Copyright 2026 Jakob Kastelic
 *
 *     ./sync [port] [datadir] [cert.pem key.pem]
 *     ./sync invite  [owner-email] [datadir]
 *     ./sync invites [datadir]
 *     ./sync revoke  <token|all> [datadir]
 *     ./sync adduser <email> [prompt|stdin|fd:N] [datadir]
 *     ./sync passwd  <email> [prompt|stdin|fd:N] [datadir]
 *     ./sync bench   [datadir]
 *
 * With a certificate and key it serves HTTPS; without, plain HTTP, which is
 * for testing on a LAN and nothing else -- the app signs its requests either
 * way, but a browser session cookie over plain HTTP is a login anybody on the
 * path can take.
 *
 * `invite` prints a signup link, which is how accounts are meant to be handed
 * out: there is no signup route, and nobody should be typing `adduser` per
 * person. With no argument the link creates an account and nothing else; with
 * an owner's email it also makes the new user a follower of that owner, which
 * is exactly what the button in the web interface mints. Its two optional
 * arguments come in either order and are told apart by the '@' (see the
 * grammar note where they are parsed); a repeat of either is refused. `adduser`
 * remains for the very first account, or for a server with no way to click a
 * link. `passwd` exists because there is no reset by email; someone locked out
 * asks the person who runs the server. Neither is a migration path -- nothing
 * here reads data.txt, and `store` and `show` keep serving it untouched.
 */
#include "db.h"
#include "http.h"
#include "https.h"
#include "jpake.h"
#include "logs.h"
#include "pair.h"
#include "posix.h" /* SYS_PATH_MAX: every path this server holds */
#include "proto.h"
#include "route.h"
#include "util.h"
#include "web.h"   /* web_route: the request this dispatches */
#include <errno.h> /* EINTR: the one-byte read below must not give up on it */
#include <fcntl.h> /* open, for the backup directory's fsync */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* S_ISDIR: a datadir is a directory that exists */
#include <termios.h>  /* the prompt types with the echo turned off */
#include <unistd.h>

#include "syncrun.h"

/* ---- NOTHING THIS PROCESS CREATES IS READABLE BY ANOTHER ACCOUNT ------
 *
 * The state here is a diabetic's glucose history, session cookies and a TLS
 * private key, and every file holding any of it was created with whatever
 * umask the invoking shell happened to have. On the board that is 0077 today,
 * by luck rather than by decision: nothing in this program said so, nothing
 * checked, and a deploy run from a shell with a looser umask would publish a
 * world-readable database and report success.
 *
 * SET, NOT ASSUMED. umask(077) here means every file this process creates --
 * the database, its -wal and -shm, backups, the log, the pid file, anything a
 * future subcommand adds -- is private to this account no matter who started
 * it. It costs one line and removes the whole class.
 *
 * The complementary half is check_private below: a umask governs what is
 * CREATED and says nothing about a file that already exists. */
void sync_private_umask(void)
{
   umask(077);
}

/* Is this path ours, and private to us? 0 when it is not, or when the question
 * could not be answered.
 *
 * `what` names the file in the refusal, because "permissions are wrong" with
 * no path is a message that sends an operator through six files.
 *
 * ---- THREE THINGS, NOT ONE -------------------------------------------
 *
 * MODE is the obvious check and the weakest of the three: a file can be mode
 * 600 and belong to somebody else entirely, in
 * which case 600 is protecting THEIR access and not ours. On a shared host
 * that is a database another account owns and this server happily serves.
 *
 * OWNER closes that. The rule is the effective uid of this process: the
 * account the operator chose to run the service as is the account its state
 * must belong to, and anything else means the path is not what this
 * deployment thinks it is -- a leftover from another user, a symlink into
 * somebody's home, a restore run as root.
 *
 * TYPE closes the other half. A "database" that is a fifo or a device node
 * passes both of the above and is not a database; a directory where a file is
 * expected fails in a way that is much harder to read later.
 *
 * ---- AND A stat() THAT FAILS IS NOT AN ABSENCE ------------------------
 *
 * NOT `if (stat(...) != 0) return 1;`, under which every failure reads as
 * "not there yet, nothing to leak". ENOENT does mean that, and it is
 * legitimate: half of these are created on first use. EACCES does not. It
 * means a directory on the path denies the lookup, which on a private tree is
 * exactly what an intruding parent directory looks like -- and the server
 * would start anyway, having concluded from a permission error that its state
 * was safe. EIO, ELOOP (a symlink cycle), ENOTDIR: all of them would answer
 * "fine".
 *
 * Only ENOENT is absence now. Anything else is a question this program could
 * not ask, and a check that cannot ask must refuse rather than pass.
 *
 * ---- WHAT IS UNDER TEST, AND WHAT IS NOT ------------------------------
 *
 * The MODE arm is exercised by synctest.sh: a world-readable database and a
 * data directory others can enter both refuse, by name.
 *
 * The OWNER arm is not, and cannot be from a test suite that does not run as
 * root: staging it needs a file this uid does not own, inside a tree it can
 * still traverse. It is the simplest of the three to read and the one whose
 * failure is loudest (it names both uids), which is the best that can be said.
 *
 * The TYPE and EXAMINE arms are reachable in principle and not in practice,
 * because two earlier guards get there first: the certificate loader reports
 * a cert it cannot open before this runs, and sqlite refuses a database that
 * is a fifo or that lives behind a directory it cannot search. Both of those
 * are themselves fail-closed, so the server does refuse -- with somebody
 * else's message. These arms are the backstop for the paths that have no
 * earlier reader (the data directory itself, and any state file added later),
 * which is exactly where a silent pass would go unnoticed. */
static int check_private(const char *path, const char *what)
{
   struct stat st;
   if (stat(path, &st) != 0) {
      if (errno == ENOENT)
         return 1; /* not there yet: nothing to leak */
      fprintf(stderr, "sync: %s (%s) could not be examined: %s.\n", what, path,
              strerror(errno));
      fprintf(stderr,
              "  This is NOT the same as \"it does not exist yet\", and the\n"
              "  difference matters: a permission or I/O error here means the\n"
              "  state could not be checked, not that it is safe. Refusing to\n"
              "  serve until it can be.\n");
      return 0;
   }
   /* THE TYPE FIRST, because the two checks below are meaningless on a device
    * node or a fifo -- and because a symlink is not what stat reports: it
    * follows them, so what is described here is the target, which is the
    * thing that actually holds the data. */
   if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
      fprintf(stderr,
              "sync: %s (%s) is not a regular file or a directory "
              "(mode %06o).\n",
              what, path, (unsigned)st.st_mode);
      fprintf(stderr, "  Something other than this server's state is at that "
                      "path.\n");
      return 0;
   }
   if (st.st_uid != geteuid()) {
      fprintf(stderr,
              "sync: %s (%s) is owned by uid %lu, and this server runs as "
              "uid %lu.\n",
              what, path, (unsigned long)st.st_uid, (unsigned long)geteuid());
      fprintf(stderr,
              "  A mode of 0600 on somebody else's file protects THEM, not\n"
              "  this service. Either the path belongs to another account or\n"
              "  the service is running as the wrong one; both are reasons\n"
              "  not to serve.\n");
      return 0;
   }
   if ((st.st_mode & (S_IRWXG | S_IRWXO)) == 0)
      return 1;
   fprintf(stderr,
           "sync: %s (%s) is readable or writable by other accounts on this "
           "host (mode %04o).\n",
           what, path, (unsigned)(st.st_mode & 07777));
   fprintf(stderr, "  This file holds glucose history, session cookies or a "
                   "private key.\n"
                   "  Fix it with `chmod 600` (or 700 for a directory) and "
                   "start again.\n");
   return 0;
}

/* Every piece of protected material, before the first byte is served.
 *
 * REFUSING TO START IS THE POINT. Logging a warning and serving anyway is the
 * outcome that reads as "somebody knows about it" while the material stays
 * readable for months; a server that will not start gets fixed the same
 * afternoon. */
int sync_state_is_private(const char *dir, const char *dbp, const char *cert,
                          const char *key)
{
   int ok = 1;
   char side[512];
   ok &= check_private(dir, "the data directory");
   ok &= check_private(dbp, "the database");
   for (const char *const *sfx = (const char *const[]){"-wal", "-shm", 0}; *sfx;
        sfx++) {
      int n = snprintf(side, sizeof side, "%s%s", dbp, *sfx);
      if (n > 0 && n < (int)sizeof side)
         ok &= check_private(side, "a database sidecar");
   }
   /* The certificate is public by nature -- it is sent to every client -- so
    * only the KEY is checked. Refusing to start over a world-readable
    * certificate would be theatre. */
   if (key)
      ok &= check_private(key, "the TLS private key");
   (void)cert;
   return ok;
}

/* ---- WHICH OF THE TWO PROGRAMS THIS INVOCATION IS ----------
 *
 * `sync` is a daemon and an operator CLI in one binary, which is right --
 * they share a database, a schema and a privacy posture, and two binaries
 * would be two things to deploy. What was wrong was that they shared a FILE:
 * 1600 lines in which the pairing route sat forty lines from `sync invites`.
 *
 * This is the whole of what they have in common at run time: read argv[1],
 * and hand the process to one half or the other. See srv/syncrun.h.
 *
 * A VERB THAT IS NOT A VERB IS A MISTAKE, and it must not start a server:
 * `sync status` and `sync invit` (a typo) would otherwise bind an ephemeral
 * port, print "listening on port 0", and sit there being a daemon nobody
 * asked for -- with argv[2] taken as the data directory. The daemon half
 * checks that argv[1] is a PORT; this one checks it is not a misspelt verb
 * first, because "not a number" and "not a verb" want different words. */
int main(int argc, char **argv)
{
   sync_private_umask();
   if (argc > 1 && sync_is_verb(argv[1]))
      return sync_cli_main(argc, argv);
   return sync_daemon_main(argc, argv);
}
