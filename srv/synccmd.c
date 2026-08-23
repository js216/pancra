// SPDX-License-Identifier: GPL-3.0
// synccmd.c --- the operator CLI: eleven verbs, and where a password may come
// from Copyright 2026 Jakob Kastelic

/* ONE OF THE THREE FILES srv/syncrun.h describes. Everything here
 * runs for milliseconds and exits: create an account, mint an invitation,
 * take a backup, list what is live. None of it serves a request.
 *
 * THE PASSWORD RULES ARE THE REASON THIS IS ITS OWN FILE as much as the
 * verbs are: `sync adduser <email> <password>` would put an account's
 * password in argv, and the block below is the whole argument for why the
 * grammar refuses to.
 */
#include "authsess.h"
#include "authuser.h"
#include "compiler.h" /* PANCRA_PRINTF: the format check, portably */
#include "db.h"
#include "http.h"
#include "https.h"
#include "jpake.h"
#include "logs.h"
#include "pair.h"
#include "posix.h" /* SYS_PATH_MAX: every path this server holds */
#include "proto.h"
#include "pwcost.h" /* the password cost this deployment measured */
#include "route.h"
#include "util.h"
#include "web.h"   /* web_route: the request this dispatches */
#include <errno.h> /* EINTR: the one-byte read below must not give up on it */
#include <fcntl.h> /* open, for the backup directory's fsync */
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* S_ISDIR: a datadir is a directory that exists */
#include <termios.h>  /* the prompt types with the echo turned off */
#include <unistd.h>

#include "syncrun.h"

/* The default port, quoted in the usage line. The daemon owns the number
 * (srv/syncd.c); this is the one place the CLI has to say it. */
#define PORT 8443

/* ---- THE PASSWORD IS NOT AN ARGUMENT ------------------------------------
 *
 * `sync adduser <email> <password>` put an account's password in argv, and
 * argv is public. /proc/<pid>/cmdline is world-readable, so every other login
 * on the box could read the secret out of `ps` for as long as the command
 * ran; the shell that launched it wrote the same line into .bash_history for
 * ever; and on a board with an audit log it went there too. This server has
 * no password reset by email -- that one line IS the account -- so the single
 * most sensitive string the operator ever types was the one typed in the
 * open.
 *
 * So it comes from somewhere argv cannot see:
 *
 *   (nothing)  a no-echo prompt on the terminal, typed twice. The default,
 *              because somebody creating the first account by hand should not
 *              have to know any of this.
 *   stdin      one line from standard input, for a caller that pipes it.
 *   fd:N       one line from an inherited descriptor, for a caller that
 *              already holds the secret open and whose stdin is busy.
 *
 * WHEN STDIN IS NOT A TERMINAL AND NO SOURCE WAS NAMED, THIS REFUSES. Reading
 * the pipe anyway is the friendly-looking choice and it is the wrong one:
 * `sync adduser a@b.c </dev/null` would then set an empty password with
 * nothing looking unusual, and the same command inside a shell script whose
 * stdin is the script file would take the NEXT LINE OF THE SCRIPT as the
 * password. Neither announces itself, and neither is recoverable here. Naming
 * the source costs one word and cannot do either.
 *
 * A PASSWORD IN THAT POSITION IS REFUSED, NOT DEPRECATED. A word there which
 * is not one of the sources above is assumed to be a password and turned
 * away, saying why. Advice in a comment would leave the exposure in place and
 * let the help go on teaching it. */
enum pw_src { PW_PROMPT, PW_STDIN, PW_FD };

/* 1024 like the browser form's field (srv/settings.c): the two surfaces set
 * the same column, so a passphrase one of them accepts must not be a
 * passphrase the other cannot carry. */
#define PW_BUF 1024

/* Overwritten through a volatile pointer. A plain memset over a buffer that
 * is never read again is exactly the store a compiler may delete, and what is
 * left in this process's pages afterwards is the whole point. */
static void pw_wipe(void *p, size_t n)
{
   volatile unsigned char *q = p;
   while (n--)
      *q++ = 0;
}

/* 1 when `tok` names a source, with *src (and *fd) set. */
static int pw_source_of(const char *tok, enum pw_src *src, int *fd)
{
   if (!strcmp(tok, "prompt")) {
      *src = PW_PROMPT;
      return 1;
   }
   if (!strcmp(tok, "stdin")) {
      *src = PW_STDIN;
      *fd  = STDIN_FILENO;
      return 1;
   }
   if (!strncmp(tok, "fd:", 3)) {
      char *end = NULL;
      int64_t n = strtoll(tok + 3, &end, 10);
      if (end == tok + 3 || (end && *end) || n < 0 || n > 1024)
         return 0;
      *src = PW_FD;
      *fd  = (int)n;
      return 1;
   }
   return 0;
}

/* ONE LINE, WHOLE, OR NOTHING.
 *
 * Read a byte at a time, which is not an oversight: this reads a descriptor
 * the CALLER may still be using, so it must not swallow a single byte past
 * the newline that ends the password. It runs once per process, over a line
 * of a few dozen bytes.
 *
 * TRUNCATION IS NOT AN OPTION. A password clipped at the buffer is a
 * DIFFERENT password, stored without a word said -- and the person it belongs
 * to is then locked out by the passphrase they chose, on a server with no
 * reset. Longer than the buffer is a refusal. */
static int pw_readline(int fd, char *out, size_t cap)
{
   size_t n = 0;
   for (;;) {
      char ch;
      ssize_t r = read(fd, &ch, 1);
      if (r < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      if (r == 0 || ch == '\n')
         break; /* EOF ends the last line too */
      if (ch == '\r')
         continue; /* a CRLF file is a text file, not part of the secret */
      if (n + 1 >= cap)
         return 0;
      out[n++] = ch;
   }
   out[n] = '\0';
   return 1;
}

/* The terminal, with the echo off and a confirmation. */
static int pw_prompt(const char *what, char *out, size_t cap)
{
   struct termios old, quiet;
   if (tcgetattr(STDIN_FILENO, &old) != 0) {
      fprintf(stderr, "sync: this is not a terminal to type a password at\n");
      return 0;
   }
   quiet = old;
   quiet.c_lflag &= (tcflag_t) ~(ECHO);
   if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0) {
      fprintf(stderr, "sync: the terminal would not stop echoing; refusing to "
                      "read a password it would print\n");
      return 0;
   }
   char again[PW_BUF];
   /* The prompts go to STDERR: stdout is what `sync invite` pipes and what
    * `adduser` prints its one result line on. */
   fprintf(stderr, "%s: ", what);
   int ok = pw_readline(STDIN_FILENO, out, cap);
   fprintf(stderr, "\n");
   if (ok) {
      /* TYPED TWICE, because it was not echoed and there is no reset: a
       * mistyped one is an account nobody can open again. */
      fprintf(stderr, "%s again: ", what);
      ok = pw_readline(STDIN_FILENO, again, sizeof again);
      fprintf(stderr, "\n");
      if (ok && strcmp(out, again) != 0) {
         fprintf(stderr, "sync: the two did not match; nothing was changed\n");
         ok = 0;
      }
   }
   /* PUT BACK WHATEVER HAPPENED. Leaving a shell with its echo off looks to
    * the operator like a machine that has stopped responding. */
   (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
   pw_wipe(again, sizeof again);
   if (!ok)
      pw_wipe(out, cap);
   return ok;
}

static int cli_password(enum pw_src src, int fd, const char *what, char *out,
                        size_t cap)
{
   if (src == PW_PROMPT)
      return pw_prompt(what, out, cap);
   if (!pw_readline(fd, out, cap)) {
      pw_wipe(out, cap);
      fprintf(stderr,
              "sync: no password could be read from %s (and a line longer "
              "than %zu bytes is refused, never clipped)\n",
              src == PW_STDIN ? "stdin" : "that descriptor", cap - 1);
      return 0;
   }
   return 1;
}

/* THE PASSWORD VERBS' ARGUMENTS.
 *
 * <email> is required. The source and the datadir are both optional and are
 * told apart by shape -- a source is one of the three words above, a datadir
 * is a directory that exists -- so neither has to be given to give the other.
 * A word that is neither is a password typed into argv: refused here, before
 * the database is opened and before anything is written.
 *
 * The ARGUMENT COUNT is still checked by cli_needs from the table, as it is
 * for every other verb; this only says what the words mean. */
static int cli_pwargs(int argc, char **argv, const char *verb, const char **dir,
                      enum pw_src *src, int *fd)
{
   *dir         = NULL;
   *src         = PW_PROMPT;
   *fd          = -1;
   int have_src = 0;
   for (int i = 3; i < argc; i++) {
      if (!have_src && pw_source_of(argv[i], src, fd)) {
         have_src = 1;
         continue;
      }
      struct stat st;
      if (!*dir && stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
         *dir = argv[i];
         continue;
      }
      fprintf(stderr,
              "sync %s: \"%s\" is neither a password source (prompt, stdin, "
              "fd:N) nor a directory that exists.\n"
              "If it was meant to BE the password: it must not be, because "
              "every argument to this command is visible in `ps`, in "
              "/proc/%" PRIwire
              "/cmdline and in your shell history. Nothing was "
              "changed.\n"
              "usage: sync %s <email> [prompt|stdin|fd:N] [datadir]\n",
              verb, argv[i], (int64_t)getpid(), verb);
      return 0;
   }
   /* SEE THE BLOCK COMMENT ABOVE for why this refuses rather than reading
    * the pipe it has been handed. */
   if (!have_src && !isatty(STDIN_FILENO)) {
      fprintf(stderr,
              "sync %s: stdin is not a terminal, so there is nowhere to "
              "prompt -- and reading the pipe unasked would set whatever "
              "happens to be on it as the password, silently.\n"
              "Say where it comes from: `sync %s <email> stdin [datadir]` to "
              "read one line from stdin, or `fd:N` for an inherited "
              "descriptor.\n",
              verb, verb);
      return 0;
   }
   return 1;
}

/* ---- THE COMMAND TABLE, which is also the help --------------------------
 *
 * The verbs and the usage text were two lists: an if-chain of strcmp, and a
 * block of printf lines inside the "that is not a subcommand" branch. They
 * disagreed -- `logout` was implemented and not listed, several verbs'
 * optional [datadir] was missing, and `sync --help` was an ERROR (exit 2)
 * printing to stderr, which is the one thing a person types first.
 *
 * One table now. Every verb's usage line comes from it, the help comes from
 * it, and the arity check comes from it -- so a verb cannot be implemented
 * and undocumented, and its arguments cannot be described one way and checked
 * another. */
struct cli_cmd {
   const char *verb;
   const char *args; /* how the arguments read in help */
   const char *what; /* one line, lower case, no full stop */
   int need;         /* least argc this verb accepts, argv[0] included */
   /* MOST argc it accepts. A minimum alone lets surplus arguments through
    * silently, and a surplus argument is never harmless here: it is a
    * mistyped one. `sync backup out.db /data extra` ran the backup and
    * ignored "extra"; `sync verify a.db b.db` verified only a.db and said
    * nothing about b.db, which reads as "both verified". Guessing which
    * argument the user meant is the failure -- there is no reading of an
    * extra argument that is safe to assume. */
   int most;
};

static const struct cli_cmd CLI[] = {
    {"adduser", "<email> [prompt|stdin|fd:N] [datadir]", "create an account",                         3,
     5                                                                                                    },
    {"passwd",  "<email> [prompt|stdin|fd:N] [datadir]",
     "set an account's password (and sign every session out)",                                        3, 5},
    {"logout",  "<email> [datadir]",                     "sign out every session of one account",     3,
     4                                                                                                    },
    {"deluser", "<email> confirm [datadir]",
     "delete an account and everything it owns",                                                      4, 5},
    {"invite",  "[owner-email] [datadir]",               "mint a single-use invitation link",
     2,                                                                                                  4},
    {"invites", "[datadir]",                             "list the invitations that are still live",  2, 3},
    {"users",   "[datadir]",                             "list the accounts that exist",              2, 3},
    {"revoke",  "<url|token|all> [datadir]",             "withdraw an invitation",                    3, 4},
    {"backup",  "<out.db> [datadir]",                    "copy the database safely while it runs",
     3,                                                                                                  4},
    {"verify",  "<file.db>",                             "check a backup opens and holds a schema",   3, 3},
    {"fsck",    "[fix] [datadir]",
     "list rows whose owner is gone, and with `fix` remove them",                                     2, 4},
    {"bench",   "[datadir]",                             "measure and record the password cost here", 2, 3},
    {"help",    "",                                      "print this",                                2, 2},
};

#define NCLI ((int)(sizeof CLI / sizeof CLI[0]))

static const struct cli_cmd *cli_find(const char *verb)
{
   for (int i = 0; i < NCLI; i++)
      if (!strcmp(CLI[i].verb, verb))
         return &CLI[i];
   return NULL;
}

/* ---- ONE ORIGIN FOR EVERY LINK THIS INSTALLATION PRINTS ------
 *
 * WHAT WAS WRONG. An invitation link could be built three different ways in
 * one installation: the CLI printed `$SYNC_BASE_URL/invite/<token>` with the
 * variable UNVALIDATED and defaulted to the compiled pancra.org; the settings
 * page printed `https://<public_origin()>/invite/<token>` from the validated
 * PANCRA_ORIGIN; and the deployment's health check fetched PANCRA_URL, a
 * third value. A board where those disagree hands out links that point
 * somewhere the server is not, and the person who pastes one has no way to
 * tell.
 *
 * There is one public name, so there is one setting for it: PANCRA_ORIGIN,
 * validated to a host[:port] by public_origin_init (srv/util.h). This is
 * where the CLI adopts it, so the link `sync invite` prints and the link the
 * settings page renders are built from the same string by construction.
 *
 * SYNC_BASE_URL IS REFUSED RATHER THAN IGNORED. A deployment that still sets
 * it believes it is choosing the origin, and silently overriding it with
 * another value is exactly the class of surprise this item is about. Saying
 * so costs one line on stderr and leaves the operator with a single thing to
 * set. */
static void cli_origin_init(void)
{
   const char *legacy = getenv("SYNC_BASE_URL");
   public_origin_init();
   if (legacy && *legacy)
      fprintf(stderr,
              "sync: SYNC_BASE_URL is no longer read; links use "
              "PANCRA_ORIGIN (%s). Set PANCRA_ORIGIN instead -- one origin "
              "for the CLI, the settings page and the health check.\n",
              public_origin());
}

/* ---- SAYING SO IS PART OF A STATE-CHANGING COMMAND ----------
 *
 * WHAT WAS WRONG. Every verb here printed its result and returned 0 without
 * looking at whether the print worked. stdout is a pipe that can be closed,
 * a full filesystem, or a terminal that went away, and printf answers about
 * all three -- so `sync invite` could commit a token, fail to deliver the one
 * line carrying it, and exit 0. The token exists, nobody has it, and the
 * exit status says the command succeeded.
 *
 * THE ANSWER IS NOT TO ROLL BACK. The change is committed and rolling it
 * back on a broken stdout would be a second failure on top of the first. What
 * the caller needs is a DISTINCT status: the command did what was asked of
 * it and could not report the result, so re-running it is the wrong move --
 * a second invite mints a second token, a second adduser fails on the unique
 * address, a second passwd asks for the password again.
 *
 * CLI_UNREPORTED is that status. The guidance goes to stderr, which is the
 * stream that has not been established as broken; if that is gone too, the
 * status is all there is and it is enough.
 *
 * The check is a FLUSH, not the return of printf alone: stdio buffers, so a
 * write to a full disk usually fails at the flush and not at the call. */
#define CLI_UNREPORTED 4

static int cli_say(const char *fmt, ...) PANCRA_PRINTF(1, 2);

static int cli_say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int n = vprintf(fmt, ap);
   va_end(ap);
   return n >= 0;
}

/* 0 when the operator has the result in their hands, CLI_UNREPORTED when the
 * change was made and could not be reported. `what` names what happened and
 * `how` says how to see it -- both go to stderr. */
static int cli_delivered(int said, const char *what, const char *how)
{
   if (said && fflush(stdout) == 0 && !ferror(stdout))
      return 0;
   clearerr(stdout);
   fprintf(stderr,
           "sync: %s -- but the result could NOT BE WRITTEN to stdout.\n"
           "  The change is committed. Do NOT re-run this command: %s\n",
           what, how);
   return CLI_UNREPORTED;
}

/* Help on STDOUT and exit 0 when it was asked for; on stderr and exit 2 when
 * it is a correction. The distinction matters to anything that pipes it. */
void sync_cli_usage(FILE *out)
{
   fprintf(out, "usage: sync [port] [datadir] [cert.pem key.pem]\n");
   fprintf(out,
           "       run the server. With no arguments: port %d, the database\n"
           "       beside this binary, and NO TLS (a warning says so).\n\n",
           PORT);
   for (int i = 0; i < NCLI; i++)
      fprintf(out, "       sync %-8s %-34s %s\n", CLI[i].verb, CLI[i].args,
              CLI[i].what);
   fprintf(out,
           "\n"
           "  [datadir]  where sync.db lives. Default: the directory holding\n"
           "             this binary.\n"
           "  [port]     1..65535. Default %d.\n"
           "  cert.pem key.pem\n"
           "             enable TLS. Without them the server runs plain HTTP\n"
           "             and says so on stderr -- test use only.\n"
           "\n"
           "  PANCRA_ORIGIN   the public host[:port] this installation is\n"
           "                  reached at (default %s). Invitation links are\n"
           "                  printed with it, the settings page renders the\n"
           "                  same, and the deployment's health check fetches\n"
           "                  the same host: one name, set once.\n"
           "\n"
           "  invite takes both of its arguments in EITHER order: the one\n"
           "  holding an '@' is the owner, the other is the datadir. Giving\n"
           "  two addresses, or two directories, is refused rather than\n"
           "  guessed.\n"
           "\n"
           "  adduser and passwd NEVER take the secret as an argument: an\n"
           "  argument is visible in `ps`, in /proc and in your shell\n"
           "  history. With no source named it is asked for at the terminal\n"
           "  with the echo off; `stdin` reads one line from standard input\n"
           "  and `fd:N` reads one line from an inherited descriptor. With\n"
           "  no terminal and no source named, they refuse.\n"
           "\n"
           "  A verb that CHANGES something and cannot write its result to\n"
           "  stdout exits 4: the change was made and could not be reported.\n"
           "  Do not re-run it -- read the stderr line, which says how to see\n"
           "  what happened.\n",
           PORT, public_origin());
}

/* The arity check, from the table. */
/* BOTH ENDS, with this verb's own usage line. */
static int cli_needs(int argc, const struct cli_cmd *c)
{
   if (argc < c->need) {
      fprintf(stderr, "sync %s: not enough arguments\n", c->verb);
      fprintf(stderr, "usage: sync %s %s\n", c->verb, c->args);
      return 0;
   }
   if (argc > c->most) {
      fprintf(stderr, "sync %s: %d argument%s too many\n", c->verb,
              argc - c->most, argc - c->most == 1 ? "" : "s");
      fprintf(stderr, "usage: sync %s %s\n", c->verb, c->args);
      return 0;
   }
   return 1;
}

/* ---- CLEARING THE STAGING NAME, AND KNOWING THAT IT IS CLEAR ----------
 *
 * A database is THREE files while it is open (the file, its write-ahead log
 * and its shared-memory index), so removing a half-made backup means removing
 * all three -- a stray sync.db.part-wal beside a published backup is another
 * database's log, and sqlite will happily try to replay it.
 *
 * WHY EVERY REMOVAL IS CHECKED. `(void)remove(...)` followed by handing that
 * pathname to sqlite3_open makes two different things possible, and neither
 * announces itself:
 *
 *   - a removal FAILED (a directory of that name, no permission, an I/O
 *     error) and the file survived. sqlite opened it, and a backup was taken
 *     into whatever was already there -- or the open failed with a message
 *     about the database rather than about the leftover.
 *   - the name was a SYMLINK. remove() unlinks the link itself, so that case
 *     is handled -- but only if the unlink happened, which is exactly what
 *     was not checked. A surviving symlink is a backup written through it,
 *     into a file the operator did not name.
 *
 * ENOENT IS THE STATE WE WANTED, and it is the ordinary one: the first backup
 * ever taken has no leftovers. Anything else means the name is not clear, and
 * this says so rather than proceeding. */
static int rm_scratch(const char *p)
{
   if (remove(p) == 0)
      return 1;
   return errno == ENOENT;
}

/* 1 when the staging name and both sidecars are GONE. */
static int scratch_clear(const char *path)
{
   char side[SYS_PATH_MAX];
   int ok = rm_scratch(path);
   if (snprintf(side, sizeof side, "%s-wal", path) < (int)sizeof side)
      ok = rm_scratch(side) && ok;
   if (snprintf(side, sizeof side, "%s-shm", path) < (int)sizeof side)
      ok = rm_scratch(side) && ok;
   return ok;
}

/* CLAIM the staging name before sqlite ever sees it: create it EXCLUSIVELY
 * and without following a symlink, so what sqlite3_open opens is provably the
 * empty plain file this process just made and not something that was already
 * there or appeared in between. O_EXCL is the proof of absence -- stronger
 * than a stat, which is a check with a window after it.
 *
 * An empty file is a valid empty sqlite database, so handing this to
 * sqlite3_open costs nothing.
 *
 * 1 on success; 0 means somebody else holds the name (EEXIST), it is a
 * symlink (ELOOP), or the directory refused it. */
static int scratch_claim(const char *path)
{
   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
   if (fd < 0)
      return 0;
   close(fd);
   return 1;
}

/* ---- THE DIRECTORY ENTRY IS PART OF THE BACKUP --------------------------
 *
 * A rename is not durable until the DIRECTORY it happened in has been synced.
 * Everything before this point was careful -- the copy is taken through
 * sqlite's backup API so it sees the write-ahead log, it is integrity-checked
 * and schema-checked while it is still called .part, and only then renamed --
 * and then the process printed "backed up ... -> ..." and exited. The file's
 * CONTENTS were on the disk (sqlite commits the destination with its own
 * fsync); the NAME was not. A power cut in the seconds after that line was
 * printed leaves a backups directory whose last entry never made it out of the
 * kernel's cache, and the artifact the operator was told they had is simply not
 * there on the next boot.
 *
 * That is the worst shape a storage bug can take here, because it is invisible
 * until the one day it matters. Backups are read exactly once -- on the day the
 * board's card died, or the day somebody restores the wrong thing -- and "I
 * took one at 02:00, the script said so" is the whole basis for reaching for
 * it. An acknowledgement that can evaporate is worse than no acknowledgement.
 *
 * app/util.c has had this shape for a while (fsync_dir_of, and the three-way
 * replace_result it feeds); this is the server's copy of it, small enough to
 * stay local to the one verb that renames a file into place.
 */
static int fsync_dir_of(const char *path)
{
   char dir[SYS_PATH_MAX];
   int n = snprintf(dir, sizeof dir, "%s", path);
   if (n <= 0 || n >= (int)sizeof dir)
      return -1;
   char *slash = strrchr(dir, '/');
   if (!slash)
      snprintf(dir, sizeof dir, ".");
   else if (slash == dir)
      dir[1] = '\0'; /* "/x" -> "/" */
   else
      *slash = '\0';
   int dfd = open(dir, O_RDONLY);
   if (dfd < 0)
      return -1;
   int ok = fsync(dfd) == 0;
   if (close(dfd) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

int sync_cli_main(int argc, char **argv)
{
   sync_private_umask();
   /* THE DATABASE THIS RUN IS ABOUT, opened by whichever subcommand needs it
    * and handed to everything that touches storage -- the server gets it
    * through the pool (see http.h's `user`), and every helper takes it as a
    * parameter. It was a process-wide singleton reachable from any file that
    * included db.h. */
   struct db *d = NULL;
   if (argc > 1 && !strcmp(argv[1], "adduser")) {
      if (!cli_needs(argc, cli_find("adduser")))
         return 1;
      /* THE ARGUMENTS ARE READ BEFORE ANYTHING IS OPENED, so an attempted
       * argv password is refused without this process ever having held a
       * database open -- and without the secret having been anywhere but the
       * command line it is being turned away for. */
      const char *addir;
      enum pw_src asrc;
      int afd;
      if (!cli_pwargs(argc, argv, "adduser", &addir, &asrc, &afd))
         return 1;
      sync_resolve_db(addir);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* THE SAME BOUND AND THE SAME CANONICAL FORM THE WEB SURFACES USE.
       *
       * This verb creates an account, and it took whatever was on the command
       * line: an address of any length, in any case. An over-long one produced
       * a row /login can never resolve -- it refuses the address before it
       * looks anything up -- so the account existed and its owner could never
       * sign in, on a server with no password reset. A mixed-case one worked
       * (the column is NOCASE) but was STORED as typed, so `sync invites` and
       * the settings page showed a spelling that no throttle row would ever
       * match. Canonicalised here, once, and every later verb below does the
       * same, so all of them are talking about the same string. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will accept "
                 "(one '@', 3 to %d bytes, no spaces)\n",
                 argv[2], EMAIL_MAX);
         db_close(d);
         return 1;
      }
      /* AFTER the address is settled: a prompt for an account that was never
       * going to be created is a secret typed for nothing. */
      char apw[PW_BUF];
      if (!cli_password(asrc, afd, "New password", apw, sizeof apw)) {
         db_close(d);
         return 1;
      }
      int64_t uid                      = 0;
      enum user_create_result acreated = user_create(d, cemail, apw, &uid);
      pw_wipe(apw, sizeof apw);
      /* EACH ANSWER GETS ITS OWN SENTENCE. "already exists, or the
       * password is empty" was printed for a database that did not answer
       * too, so an operator with a full disk was told to pick a different
       * address. */
      if (acreated != USER_CREATE_OK) {
         switch (acreated) {
            case USER_CREATE_OK: break; /* not reachable: tested above */
            case USER_CREATE_EXISTS:
               fprintf(stderr,
                       "sync: %s already has an account. `sync users` lists "
                       "them.\n",
                       cemail);
               break;
            case USER_CREATE_INVALID:
               fprintf(stderr,
                       "sync: %s was not created: an account needs an address "
                       "this server accepts and a password that is not "
                       "empty.\n",
                       cemail);
               break;
            case USER_CREATE_FAIL:
            default:
               fprintf(stderr,
                       "sync: %s was NOT created: the server could not carry "
                       "the request out.\n"
                       "  Nothing was written. This is not a problem with the "
                       "address or the password --\n"
                       "  check the disk and the database (`sync verify "
                       "%s`), then try again.\n",
                       cemail, sync_db_path());
               break;
         }
         db_close(d);
         return 1;
      }
      int rc = cli_delivered(
          cli_say("created user %" PRIwire " (%s) in %s\n", uid, cemail,
                  sync_db_path()),
          "the account was created",
          "a second `adduser` for the same address is refused as a "
          "duplicate. `sync users` lists it.");
      db_close(d);
      return rc;
   }
   if (argc > 1 && !strcmp(argv[1], "backup")) {
      if (!cli_needs(argc, cli_find("backup")))
         return 1;
      sync_resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* WHERE IT IS BEING PUBLISHED, CHECKED BEFORE ANYTHING IS WRITTEN.
       *
       * db_backup checks the file it CREATES (the .part below); this checks
       * the name the .part is RENAMED to, which is the one that does the
       * damage. `sync backup /data/sync.db /data` renamed a fresh copy over
       * the database the running server had open: every later request
       * succeeded, against an inode with no name, and the next restart opened
       * the backup and lost every sync taken since. See db.h -- the test is
       * by device and inode, so a symlink or a `..` cannot walk around it. */
      if (db_backup_dest(d, argv[2]) != BACKUP_DEST_OK) {
         fprintf(stderr, "sync: no backup was taken; the previous one is "
                         "untouched\n");
         db_close(d);
         return 1;
      }
      /* WRITE ASIDE, THEN RENAME, like every other durable write in this
       * repository: a backup interrupted halfway must not replace the last
       * good one with a truncated file -- which is the state you discover at
       * the only moment it matters. */
      char tmp[SYS_PATH_MAX];
      int tn = snprintf(tmp, sizeof tmp, "%s.part", argv[2]);
      if (tn <= 0 || tn >= (int)sizeof tmp) {
         fprintf(stderr, "sync: backup path too long\n");
         db_close(d);
         return 1;
      }
      /* THE STAGING NAME IS CLEARED AND THEN CLAIMED, and both are checked
       *: sqlite is not handed a pathname whose current state
       * nobody established. */
      if (!scratch_clear(tmp)) {
         fprintf(stderr,
                 "sync: %s (or its -wal/-shm) is in the way and could not "
                 "be removed; no backup was taken and the previous one is "
                 "untouched\n",
                 tmp);
         db_close(d);
         return 1;
      }
      if (!scratch_claim(tmp)) {
         fprintf(stderr,
                 "sync: could not create %s exclusively -- something else "
                 "holds that name, or it is a symlink. No backup was "
                 "taken.\n",
                 tmp);
         db_close(d);
         return 1;
      }
      /* VERIFIED BEFORE IT IS PUBLISHED. A backup nobody has read is a hope;
       * this one is opened and integrity-checked while it is still called
       * .part, so a failure leaves the previous backup in place rather than
       * replacing it with a file that only LOOKS like one. */
      if (!db_backup(d, tmp) || db_verify(tmp) != VERIFY_OK) {
         fprintf(stderr, "sync: no backup was taken; the previous one is "
                         "untouched\n");
         (void)scratch_clear(tmp); /* best effort: the run is already lost */
         db_close(d);
         return 1;
      }
      if (rename(tmp, argv[2]) != 0) {
         fprintf(stderr, "sync: cannot publish %s\n", argv[2]);
         (void)scratch_clear(tmp); /* best effort: the run is already lost */
         db_close(d);
         return 1;
      }
      (void)scratch_clear(tmp); /* the -wal/-shm the verify open left */
      /* ---- PAST THE POINT OF NO RETURN, AND IT IS ITS OWN ANSWER --------
       *
       * THREE OUTCOMES, not two, for the reason app/util.h spells out about
       * REPLACE_UNSYNCED: everything before the rename can be undone, and this
       * cannot. The rename HAS happened -- `argv[2]` names the new backup, any
       * reader opening it right now gets it, and the previous backup at that
       * name is gone whatever this fsync says. So neither "it worked" nor "it
       * failed" is true here:
       *
       *   "it worked", printed unconditionally, tells the operator to go to
       *   bed on an artifact whose directory entry may not
       *   survive the next power cut -- and the whole value of a backup is
       *   that it is there on the morning nothing else is.
       *
       *   "it failed" -- the obvious fix, `return 1` here -- is the lie in the
       *   other direction, and a worse one for the callers: deploy.sh treats a
       *   failed backup as "refusing to deploy over unsaved data" and stops,
       *   and backup.sh moves the arrived copy aside as unverified. Both would
       *   be reacting to a backup that is present, complete and verified.
       *
       * Exit 2 is therefore its own status: PUBLISHED, DURABILITY UNKNOWN. The
       * shell layer (backup.sh, deploy.sh) has a branch for it, and neither
       * treats it as a reason to stop -- only as a reason to say so.
       *
       * The failure is not hypothetical: a backups directory whose mode has
       * been tightened to write+execute (0300) renames perfectly and cannot be
       * opened for the fsync, which is exactly this branch and is how
       * restoredrill.sh reaches it without any fault injection. */
      if (fsync_dir_of(argv[2]) != 0) {
         printf("backed up %s -> %s\n", sync_db_path(), argv[2]);
         fprintf(stderr,
                 "sync: DURABILITY UNCERTAIN: %s is written and verified and "
                 "is there NOW, but its directory entry could not be synced, "
                 "so a power loss in the next moments can erase it. The copy "
                 "is good: take another one, or copy this one somewhere "
                 "else.\n",
                 argv[2]);
         db_close(d);
         return 2;
      }
      int rc = cli_delivered(
          cli_say("backed up %s -> %s\n", sync_db_path(), argv[2]),
          "the backup was written and verified",
          "the file named on the command line is the backup; check it with "
          "`sync verify <file>`.");
      db_close(d);
      return rc;
   }
   if (argc > 1 && !strcmp(argv[1], "verify")) {
      if (!cli_needs(argc, cli_find("verify")))
         return 1;
      /* THREE OUTCOMES, TWO OF WHICH ARE FAILURES. VERIFY_LEFTOVER
       * says the file is a good backup AND that a complete copy of it is
       * still on disk -- so the exit status has to be a failure, or the
       * wrapper that reads it moves on and the copy stays there for ever.
       * db_verify has already named the path; saying "verifies" underneath
       * that would be the sentence the operator remembers. */
      switch (db_verify(argv[2])) {
         case VERIFY_OK: break;
         case VERIFY_LEFTOVER:
            fprintf(stderr,
                    "sync: %s verifies, but this run did not clean up after "
                    "itself\n",
                    argv[2]);
            return 1;
         case VERIFY_BAD:
         default: return 1;
      }
      printf("%s verifies\n", argv[2]);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "invite")) {
      if (!cli_needs(argc, cli_find("invite")))
         return 1;
      /* "sync invite" -> a plain signup link.
       * "sync invite jk@x" -> a signup link that also follows jk@x.
       *
       * THE GRAMMAR, WHICH IS THE ONE THE HELP PRINTS.
       *
       * Both arguments are optional, and two optional positionals cannot be
       * told apart by position at all: with `[owner-email] [datadir]` read
       * strictly, a lone `/srv/pancra` would be an owner address, and there
       * would be no way to say "this directory, no owner" -- which is the
       * commonest call. So the shape decides, and ORDER DOES NOT MATTER: an
       * argument containing '@' is the owner, anything else is the datadir.
       * That is a real rule, not a guess, and the help says so.
       *
       * What is NOT allowed is a second argument of the same kind. Accepted,
       * `sync invite a@x b@x` would silently mint an invitation for b@x and
       * discard the first address, and `sync invite /a /b` would use /b. Both
       * are a typed mistake, and the only safe reading of a repeated argument
       * is to refuse it -- exactly the reasoning behind `most` above. */
      const char *owner_email = NULL;
      const char *dir         = NULL;
      for (int i = 2; i < argc; i++) {
         int mail        = strchr(argv[i], '@') != NULL;
         const char *had = mail ? owner_email : dir;
         if (had) {
            fprintf(stderr,
                    "sync: two %s given (%s and %s). An invitation "
                    "follows one owner and reads one database.\n",
                    mail ? "owner addresses" : "data directories", had,
                    argv[i]);
            return 1;
         }
         if (mail)
            owner_email = argv[i];
         else
            dir = argv[i];
      }
      sync_resolve_db(dir);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      int64_t owner = 0;
      if (owner_email) {
         /* Canonicalised like every other surface, so "JK@X" and "jk@x" are
          * the same owner here as they are to the account table -- and so an
          * over-long argument is refused rather than becoming a silent
          * "no such user". */
         char cowner[EMAIL_BUF];
         if (!email_canon(owner_email, cowner, sizeof cowner)) {
            fprintf(stderr,
                    "sync: %s is not an address this server will "
                    "accept\n",
                    owner_email);
            db_close(d);
            return 1;
         }
         /* THE FAILURE OUTPUT IS READ, not discarded. user_by_email answers 0
          * for "no such account" AND for "the query did not run", and telling
          * an operator "no such user" when the database is unreadable sends
          * them to check the spelling of an address that is perfectly
          * correct. */
         int ofailed = 0;
         owner       = user_by_email(d, cowner, &ofailed);
         if (ofailed) {
            fprintf(stderr,
                    "sync: could not look up %s: the database did "
                    "not answer\n",
                    cowner);
            db_close(d);
            return 1;
         }
         if (!owner) {
            fprintf(stderr, "sync: no such user: %s\n", cowner);
            db_close(d);
            return 1;
         }
      }
      char token[TOKEN_HEX + 1];
      if (!rnd_hex(token, sizeof token, TOKEN_HEX)) {
         fprintf(stderr, "sync: could not draw an invitation token\n");
         db_close(d);
         return 1;
      }
      sqlite3_stmt *st = db_prep(
          d,
          "INSERT INTO share_token(token,owner_id,email,created_at,expires_at)"
          " VALUES(?,?,NULL,?,?)");
      if (!st)
         return 1;
      int64_t now = (int64_t)time(NULL);
      sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
      if (owner)
         sqlite3_bind_int64(st, 2, owner);
      else
         sqlite3_bind_null(st, 2);
      sqlite3_bind_int64(st, 3, now);
      sqlite3_bind_int64(st, 4, now + TOKEN_TTL);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
      if (!ok) {
         fprintf(stderr, "sync: could not create the invitation\n");
         return 1;
      }
      /* The FULL url and nothing else, so it can be pasted straight into a
       * message or piped somewhere. Anything else on stdout would have to be
       * stripped by every caller. */
      cli_origin_init();
      int rc = cli_delivered(
          cli_say("https://%s/invite/%s\n", public_origin(), token),
          "the invitation was minted",
          "the link is not printed twice, and a second `invite` mints a "
          "SECOND token against the same quota. `sync invites` lists every "
          "live one, with its link.");
      db_close(d);
      return rc;
   }
   /* ---- fsck: THE SUPPORTED WAY TO CLEAR ORPHAN ROWS ------------------
    *
    * db_open refuses a database holding rows whose owner is gone.
    * The alternative advice -- "open it with the sqlite3 shell and DELETE
    * what PRAGMA foreign_key_check names" -- is hand-written SQL against a
    * live database, on a board that may not have sqlite3 at all, asked of
    * somebody whose service is down.
    *
    * WITHOUT `fix` IT CHANGES NOTHING, which is the default on purpose: the
    * first thing to do with a damaged file is look at it. `fix` deletes, in
    * one transaction, reporting every row.
    *
    * Opened through db_open_repair -- the one door that skips the orphan
    * refusal, because this is the tool that removes them. */
   if (argc > 1 && !strcmp(argv[1], "fsck")) {
      if (!cli_needs(argc, cli_find("fsck")))
         return 1;
      int fix         = (argc > 2 && !strcmp(argv[2], "fix"));
      const char *dir = NULL;
      if (fix)
         dir = (argc > 3) ? argv[3] : NULL;
      else if (argc > 2)
         dir = argv[2];
      if (argc > (fix ? 4 : 3)) {
         fprintf(stderr, "sync: too many arguments for fsck\n");
         sync_cli_usage(stderr);
         return 2;
      }
      sync_resolve_db(dir);
      d = db_open_repair(sync_db_path());
      if (!d)
         return 1;
      int64_t removed = 0;
      int ok          = db_fsck(d, fix, &removed);
      db_close(d);
      return ok ? 0 : 1;
   }
   if (argc > 1 && !strcmp(argv[1], "invites")) {
      /* Checked like every other verb. This one skipped cli_needs entirely --
       * its minimum of 2 is what `argc > 1` already guarantees, so there
       * seemed to be nothing to check, and the MAXIMUM went unenforced with
       * it. */
      if (!cli_needs(argc, cli_find("invites")))
         return 1;
      sync_resolve_db(argc > 2 ? argv[2] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      cli_origin_init();
      /* Unused and unexpired only: a spent or stale token is not a link
       * anybody can still use, and listing them would just be noise to read
       * past. */
      sqlite3_stmt *st = db_prep(
          d,
          "SELECT t.token, u.email, t.expires_at FROM share_token t"
          " LEFT JOIN user u ON u.id=t.owner_id"
          " WHERE t.used_at IS NULL AND t.expires_at>? ORDER BY t.created_at");
      if (!st)
         return 1;
      sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
      int n = 0;
      int lrc;
      while ((lrc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *own = (const char *)sqlite3_column_text(st, 1);
         int64_t left =
             ((int64_t)sqlite3_column_int64(st, 2) - (int64_t)time(NULL));
         printf("https://%s/invite/%s  %s  %" PRIwire " days left\n",
                public_origin(), (const char *)sqlite3_column_text(st, 0),
                own ? own : "(signup only)", left / 86400);
         n++;
      }
      int lok = db_finished(lrc);
      sqlite3_finalize(st);
      if (!lok) {
         fprintf(stderr, "sync: the invitation list is INCOMPLETE\n");
         db_close(d);
         return 1;
      }
      if (!n)
         printf("no live invitations\n");
      db_close(d);
      return 0;
   }
   /* THE ACCOUNTS THAT EXIST, which nothing could ask for.
    *
    * Every other account verb takes an email and acts on it -- adduser,
    * passwd, logout -- so an operator who does not already KNOW the address
    * had no supported way to find it. What they did instead was open sync.db
    * with the sqlite3 shell, on a live server, as root, to read a column: a
    * writable handle held open beside a running writer, for a question that
    * is read-only. `passwd jo@example.com` answering "no such account" when
    * the row says `Jo@Example.com` is the moment that happens, and recovery
    * is exactly when it is least wanted.
    *
    * THE CANONICAL FORM IS THE ANSWER. What is stored is email_canon's
    * output (srv/util.h) -- trimmed and ASCII-folded -- and it is what every
    * lookup on this server is about. Printing the stored text, rather than
    * anything derived from it here, is the whole point of the command: it is
    * the string to paste into the next verb.
    *
    * AND NOTHING ELSE. The id, because it is what the other tables reference
    * and what a support conversation can say out loud; the address, because
    * it is the argument. Not the salt, not the hash, not the iteration count
    * -- an inventory that scrolls a credential across a terminal is a
    * credential in a scrollback buffer, on a screen somebody may be sharing.
    * Not the display name or the timezone either: they are the user's, they
    * are not needed to operate anything, and a list that grows columns is one
    * nobody can parse the next time. */
   if (argc > 1 && !strcmp(argv[1], "users")) {
      if (!cli_needs(argc, cli_find("users")))
         return 1;
      sync_resolve_db(argc > 2 ? argv[2] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* ORDERED BY ID, which is the order they were created and never
       * changes. Ordering by email would reshuffle the whole list whenever an
       * account is added, so two runs a minute apart could not be diffed --
       * and diffing two runs is what an operator does during recovery. */
      sqlite3_stmt *st = db_prep(d, "SELECT id, email FROM user ORDER BY id");
      if (!st) {
         db_close(d);
         return 1;
      }
      int n = 0;
      int lrc;
      while ((lrc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *em = (const char *)sqlite3_column_text(st, 1);
         printf("%lld  %s\n", (long long)sqlite3_column_int64(st, 0),
                em ? em : "(no address)");
         n++;
      }
      int lok = db_finished(lrc);
      sqlite3_finalize(st);
      if (!lok) {
         /* A LIST CUT SHORT BY AN I/O ERROR LOOKS EXACTLY LIKE A SHORT LIST,
          * and this one is read during recovery -- when "that account is not
          * in here" is a conclusion somebody acts on. Said on stderr and with
          * a non-zero exit, so a script cannot mistake it either. */
         fprintf(stderr, "sync: the account list is INCOMPLETE\n");
         db_close(d);
         return 1;
      }
      if (!n)
         printf("no accounts\n");
      db_close(d);
      return 0;
   }
   if (argc > 1 && !strcmp(argv[1], "revoke")) {
      if (!cli_needs(argc, cli_find("revoke")))
         return 1;
      sync_resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* A token may be given as the bare hex or as the whole URL, because the
       * whole URL is what `invites` printed and what got pasted somewhere. */
      const char *tok   = argv[2];
      const char *slash = strrchr(tok, '/');
      if (slash)
         tok = slash + 1;
      sqlite3_stmt *st =
          strcmp(tok, "all")
              ? db_prep(d, "DELETE FROM share_token WHERE token=?")
              : db_prep(d, "DELETE FROM share_token WHERE used_at IS NULL");
      if (!st)
         return 1;
      if (strcmp(tok, "all"))
         sqlite3_bind_text(st, 1, tok, -1, SQLITE_STATIC);
      int ok = sqlite3_step(st) == SQLITE_DONE;
      int n  = db_changes(d);
      sqlite3_finalize(st);
      if (!ok) {
         fprintf(stderr, "sync: could not revoke\n");
         return 1;
      }
      /* A CLI whose exit status does not distinguish "revoked" from "no such
       * token" cannot be scripted: `sync revoke $t || alert` never fires. */
      if (n == 0) {
         fprintf(stderr, "sync: no such invitation; nothing was revoked\n");
         db_close(d);
         return 1;
      }
      int rc = cli_delivered(cli_say("revoked %d\n", n),
                             "the invitations were revoked",
                             "they are already gone; `sync invites` lists "
                             "what is still live.");
      db_close(d);
      return rc;
   }
   if (argc > 1 && !strcmp(argv[1], "passwd")) {
      if (!cli_needs(argc, cli_find("passwd")))
         return 1;
      const char *pdir;
      enum pw_src psrc;
      int pfd;
      if (!cli_pwargs(argc, argv, "passwd", &pdir, &psrc, &pfd))
         return 1;
      sync_resolve_db(pdir);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* Canonicalised, because this is the ONLY recovery there is: there is no
       * password reset by email on this server, so an operator typing the
       * address in a different case than it was created in must reach the same
       * account. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will "
                 "accept\n",
                 argv[2]);
         db_close(d);
         return 1;
      }
      int pwfailed = 0;
      int64_t uid  = user_by_email(d, cemail, &pwfailed);
      if (pwfailed) {
         fprintf(stderr,
                 "sync: could not look up %s: the database did not answer\n",
                 cemail);
         db_close(d);
         return 1;
      }
      if (!uid) {
         fprintf(stderr, "sync: no such user: %s\n", cemail);
         db_close(d);
         return 1;
      }
      /* Nobody is asked to type a new password for an account that is not
       * there. */
      char ppw[PW_BUF];
      if (!cli_password(psrc, pfd, "New password", ppw, sizeof ppw)) {
         db_close(d);
         return 1;
      }
      /* THE SAME ONE OPERATION THE BROWSER CALLS, and there is no second step
       * here for a reason: user_set_password (srv/authuser.c) sets the hash and
       * revokes every session inside ONE transaction. Doing the revocation
       * here, afterwards, as a separate statement means a revocation that
       * fails leaves the new password installed and the previous cookies
       * working, which is precisely the state the person changing it
       * believes they have just left. A copy of that logic here is also how
       * the two surfaces drift apart. */
      int pchanged = user_set_password(d, uid, ppw);
      pw_wipe(ppw, sizeof ppw);
      if (!pchanged) {
         fprintf(stderr,
                 "sync: the password was NOT changed and nothing was signed "
                 "out -- %s is exactly as it was. (It cannot be empty, and "
                 "the change is refused unless every session can be revoked "
                 "in the same transaction.)\n",
                 cemail);
         db_close(d);
         return 1;
      }
      int rc = cli_delivered(
          cli_say("password changed for %s; all sessions signed out\n", cemail),
          "the password WAS changed and every session was signed out",
          "the new password is the one you just entered; re-running would "
          "ask for another one.");
      db_close(d);
      return rc;
   }
   if (argc > 1 && !strcmp(argv[1], "logout")) {
      if (!cli_needs(argc, cli_find("logout")))
         return 1;
      /* Sign one account out of every browser.
       *
       * `passwd` also does this as a side effect. As a verb of its own, "I
       * left myself signed in on a machine I do not have any more" does not
       * require changing the password. It is also what makes the
       * session check testable without reaching into the database with an
       * external tool. */
      if (!cli_needs(argc, cli_find("logout")))
         return 1;
      sync_resolve_db(argc > 3 ? argv[3] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* Canonicalised: a revocation aimed at the wrong spelling of an address
       * reports "no such user" and leaves every stolen cookie working, which
       * is the one answer this verb must never give by accident. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr,
                 "sync: %s is not an address this server will "
                 "accept\n",
                 argv[2]);
         db_close(d);
         return 1;
      }
      int lofailed = 0;
      int64_t uid  = user_by_email(d, cemail, &lofailed);
      if (lofailed) {
         fprintf(stderr, "sync: could not look up that account: the database "
                         "did not answer\n");
         db_close(d);
         return 1;
      }
      if (!uid) {
         fprintf(stderr, "sync: no such user\n");
         db_close(d);
         return 1;
      }
      if (!session_drop_all(d, uid)) {
         fprintf(stderr, "sync: sessions could NOT be dropped\n");
         db_close(d);
         return 1;
      }
      int rc = cli_delivered(
          cli_say("signed out everywhere: %s\n", cemail),
          "every session for that account was signed out",
          "the sessions are already gone; re-running is harmless but says "
          "the same thing.");
      db_close(d);
      return rc;
   }
   if (argc > 1 && !strcmp(argv[1], "deluser")) {
      if (!cli_needs(argc, cli_find("deluser")))
         return 1;
      /* ---- DELETING AN ACCOUNT FROM THE COMMAND LINE --------
       *
       * The web has had this behind a typed confirmation; the CLI had
       * nothing, so an operator whose user could not sign in -- and this
       * server has no password reset by email -- had to open the database
       * with sqlite3 and work out what a delete has to touch. The operation
       * itself is auth.c's (user_delete_account), the same one the browser
       * calls, so neither surface can be the one that forgets a table.
       *
       * THE CONFIRMATION IS A LITERAL WORD, not a flag: this cascades every
       * row the account owns -- the readings, the pairing, the sessions, the
       * shares in both directions -- and there is no undo but a backup. A
       * flag is something a shell history repeats by accident; typing
       * `confirm` after the address is not.
       *
       * IT IS THE SECOND ARGUMENT, so a mistyped address cannot be deleted by
       * a command that was correct for a different one: the whole line has to
       * be right. */
      if (strcmp(argv[3], "confirm") != 0) {
         fprintf(stderr,
                 "sync deluser: say `sync deluser %s confirm` -- this "
                 "deletes the account and EVERYTHING it owns (readings,\n"
                 "  pairing, sessions and shares), and the only undo is a "
                 "backup.\n",
                 argv[2]);
         return 1;
      }
      sync_resolve_db(argc > 4 ? argv[4] : NULL);
      d = db_open(sync_db_path());
      if (!d)
         return 1;
      /* Canonicalised like every other surface: an address that differs only
       * in case names the same account, and a delete aimed at a spelling the
       * table does not hold would report "no such user" for an account that
       * is very much there. */
      char cemail[EMAIL_BUF];
      if (!email_canon(argv[2], cemail, sizeof cemail)) {
         fprintf(stderr, "sync: %s is not an address this server will accept\n",
                 argv[2]);
         db_close(d);
         return 1;
      }
      int dfailed = 0;
      int64_t uid = user_by_email(d, cemail, &dfailed);
      if (dfailed) {
         fprintf(stderr, "sync: could not look up that account: the database "
                         "did not answer\n");
         db_close(d);
         return 1;
      }
      if (!uid) {
         fprintf(stderr, "sync: no such user: %s\n", cemail);
         db_close(d);
         return 1;
      }
      switch (user_delete_account(d, uid)) {
         case USER_DELETED: {
            int rc = cli_delivered(
                cli_say("deleted %s and everything it owned\n", cemail),
                "the account and everything it owned were deleted",
                "it is gone; `sync users` no longer lists it.");
            db_close(d);
            return rc;
         }
         case USER_NO_SUCH:
            /* Between the lookup and the delete. Nothing was written, and
             * the account is gone either way -- which is what was asked. */
            printf("%s was already gone\n", cemail);
            db_close(d);
            return 0;
         case USER_DELETE_FAIL:
         default:
            fprintf(stderr, "sync: the account was NOT deleted: the database "
                            "did not answer. Nothing was changed.\n");
            db_close(d);
            return 1;
      }
   }
   if (argc > 1 && !strcmp(argv[1], "bench")) {
      if (!cli_needs(argc, cli_find("bench")))
         return 1;
      /* MEASURE AND WRITE, rather than time a constant. This used
       * to hash once at PW_ITERS_DEFAULT and print the milliseconds, which
       * told an operator the cost was wrong and gave them nothing to do about
       * it but edit a header and rebuild. It now chooses the cost this
       * machine can carry inside the deployment's budget and writes it down;
       * the server validates and obeys it at startup. See srv/pwcost.h. */
      sync_resolve_db(argc > 2 ? argv[2] : NULL);
      return pwcost_calibrate(sync_db_dir(), HTTP_WORKERS);
   }

   /* HELP IS AN ANSWER, NOT AN ERROR. Falling through to "that is not a
    * subcommand or a port", printed to stderr with exit 2, treats the first
    * thing anybody types as a mistake. All three spellings work, on stdout,
    * exit 0. */
   if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
                    !strcmp(argv[1], "-h"))) {
      /* Even help has an exact form. `sync help backup` looks like a request
       * for one verb's usage and is not one -- printing the whole help for it
       * answers a question that was not asked, and quietly. */
      if (argc > 2) {
         fprintf(stderr, "sync help: %d argument%s too many\n", argc - 2,
                 argc - 2 == 1 ? "" : "s");
         sync_cli_usage(stderr);
         return 2;
      }
      sync_cli_usage(stdout);
      return 0;
   }

   /* NOT A VERB THIS FILE KNOWS. main() asked sync_is_verb() before calling
    * here, so this is unreachable -- and it returns 2 rather than falling
    * through to anything, because "the dispatcher and this table disagree"
    * is a bug, not an invocation. */
   fprintf(stderr, "sync: '%s' is not a subcommand\n", argc > 1 ? argv[1] : "");
   sync_cli_usage(stderr);
   return 2;
}

/* THE TABLE IS THE ANSWER (see srv/syncrun.h): main() asks this before it
 * decides whether the process is a daemon, and the same table generates the
 * help -- so a verb cannot be dispatchable and undocumented, or the reverse. */
int sync_is_verb(const char *word)
{
   if (!word)
      return 0;
   /* THE TWO SPELLINGS OF "help" THAT ARE NOT IN THE TABLE. `--help` and `-h`
    * are what somebody types first, and they are the CLI's business even
    * though no row names them: without this they fall through to the daemon,
    * which reports them as "not a subcommand or a port" -- on stderr, exiting
    * 2, which is the exact defect clitest exists to refuse. (`help` itself IS
    * a row, so that one needs no special case.) */
   if (!strcmp(word, "--help") || !strcmp(word, "-h"))
      return 1;
   return cli_find(word) != NULL;
}
