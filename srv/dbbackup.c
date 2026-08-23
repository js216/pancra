// SPDX-License-Identifier: GPL-3.0
// dbbackup.c --- making a durable copy of the database, and checking one
// Copyright 2026 Jakob Kastelic

/* SPLIT OUT OF srv/db.c, along the seam that file already had.
 *
 * db.c is the connection, the schema and the migration -- everything about
 * TALKING to a database. This is the 500 lines that do something else:
 * making a copy of the file and deciding whether a copy somebody hands back
 * is one
 * this server can open. That workflow's vocabulary is paths, devices, inodes,
 * scratch directories and fsync; it barely mentions SQL, and the one thing it
 * needs from the core is a handle (db_handle, srv/dbint.h).
 *
 * It came out because db.c passed the 2000-line ceiling the build enforces,
 * and it is the piece that came out because it is the piece that was already
 * separate. The extraction moved lines and changed none of them.
 *
 * WHY THE PATH CODE IS SO CAREFUL is explained where it happens, immediately
 * below: a backup destination is a name the operator typed, it usually does
 * not exist yet, and it must not be allowed to name the live database by a
 * different spelling. */
#include "db.h"
#include "dbint.h" /* db_handle: the one thing this needs from db.c */
#include "proto.h"

#include "posix.h"  /* SYS_PATH_MAX: the canonical forms compared below */
#include <dirent.h> /* the scratch directory db_verify empties by hand */
#include <errno.h>  /* readdir reports a truncated walk through it, not NULL */
#include <fcntl.h>  /* openat/O_NOFOLLOW: a name checked through its fd */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h> /* calloc/free/realpath/mkdtemp */
#include <string.h>
#include <sys/stat.h> /* device and inode: WHICH file, not which spelling */
#include <unistd.h>

/* ---- WHICH FILE, NOT WHICH SPELLING ----------------------------------
 *
 * `p` in canonical form, in `out` (which must hold SYS_PATH_MAX). 1 on success.
 *
 * IT HAS TO WORK FOR A PATH THAT DOES NOT EXIST YET, because a backup
 * destination usually does not: the whole point is to resolve where the
 * rename will LAND. realpath refuses a missing final component, so the
 * DIRECTORY is resolved -- which collapses every `..`, every `.` and every
 * symlinked parent -- and the last component is put back on the end. */
static int canon(const char *p, char *out, size_t n)
{
   char real[SYS_PATH_MAX];
   if (realpath(p, real)) {
      int k = snprintf(out, n, "%s", real);
      return k > 0 && (size_t)k < n;
   }
   char copy[SYS_PATH_MAX];
   int k = snprintf(copy, sizeof copy, "%s", p);
   if (k <= 0 || (size_t)k >= sizeof copy)
      return 0;
   char *slash      = strrchr(copy, '/');
   const char *base = slash ? slash + 1 : copy;
   const char *dir  = ".";
   if (slash == copy)
      dir = "/";
   else if (slash) {
      *slash = '\0';
      dir    = copy;
   }
   /* "", "." and ".." are not names a file can be created under, so there is
    * nothing to canonicalise and nothing safe to guess. */
   if (!*base || !strcmp(base, ".") || !strcmp(base, ".."))
      return 0;
   char rdir[SYS_PATH_MAX];
   if (!realpath(dir, rdir))
      return 0;
   k = snprintf(out, n, "%s%s%s", rdir, strcmp(rdir, "/") ? "/" : "", base);
   return k > 0 && (size_t)k < n;
}

/* THE SAME FILE, asked of the filesystem rather than of two strings. A
 * symlink, a hard link, a bind mount and `../live/sync.db` all answer yes
 * here and no to strcmp. Missing files are not "the same": if either side is
 * not there, the canonical-name comparison is what decides. */
static int same_file(const char *a, const char *b)
{
   struct stat sa, sb;
   if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
      return 0;
   return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* One refusal, in the two ways a destination can reach a file: by resolving
 * to its name, or by being it. */
static int reaches(const char *dest_canon, const char *dest_raw,
                   const char *other)
{
   return !strcmp(dest_canon, other) || same_file(dest_raw, other);
}

enum backup_dest db_backup_dest(struct db *d, const char *out_path)
{
   if (!d || !d->path[0] || !out_path || !*out_path)
      return BACKUP_DEST_FAIL;
   char live[SYS_PATH_MAX], dest[SYS_PATH_MAX];
   if (!canon(d->path, live, sizeof live)) {
      fprintf(stderr, "sync: cannot resolve the live database %s\n", d->path);
      return BACKUP_DEST_FAIL;
   }
   if (!canon(out_path, dest, sizeof dest)) {
      fprintf(stderr,
              "sync: cannot resolve the backup destination %s\n"
              "  Its directory must exist before a backup can be written "
              "into it.\n",
              out_path);
      return BACKUP_DEST_FAIL;
   }
   /* THE DATABASE AND EVERY FILE SQLITE KEEPS BESIDE IT. -journal is here
    * even though this server runs WAL: a database recovered by hand, or one
    * opened by another tool, has one, and a backup written over it destroys
    * the rollback the next open needs. */
   static const char *const SIDE[] = {"", "-wal", "-shm", "-journal"};
   for (size_t i = 0; i < sizeof SIDE / sizeof SIDE[0]; i++) {
      char one[SYS_PATH_MAX];
      int k = snprintf(one, sizeof one, "%s%s", live, SIDE[i]);
      if (k <= 0 || (size_t)k >= sizeof one)
         return BACKUP_DEST_FAIL;
      if (reaches(dest, out_path, one)) {
         fprintf(stderr,
                 "sync: REFUSING to back up onto %s.\n"
                 "  It is the live database's %s (%s).\n"
                 "  Publishing a backup there renames a file over the one the "
                 "server has open:\n"
                 "  the server keeps writing to an inode with no name, every "
                 "request still\n"
                 "  succeeds, and at the next restart every sync since this "
                 "moment is gone.\n"
                 "  WHAT TO DO: back up to a directory that is not the data "
                 "directory.\n",
                 out_path, *SIDE[i] ? SIDE[i] + 1 : "own file", one);
         return BACKUP_DEST_ALIAS;
      }
   }
   /* ---- AND THE STAGING NAMES ANOTHER OPERATION OWNS ------------------
    *
    * srv/deploy/lock.sh sets the conventions these follow, and the reason
    * they are refused is the same in each case: the file at that name is
    * about to be renamed ONTO something, or deleted, by an operation that
    * has no idea a backup was published there.
    *
    *   <db>.part          what a backup of THIS database stages into. A file
    *                      published there is deleted by the next backup's
    *                      scratch_clear before it is overwritten.
    *   <db>.restoring-*   what restore.sh copies in and then installs as the
    *                      live database. A backup published there is
    *                      installed by a restore nobody aimed at it.
    *
    * ANCHORED ON THE LIVE DATABASE'S NAME, not on the suffix alone. Every
    * backup this command takes stages at `<dest>.part` and then checks THAT
    * path through this same function, so a blanket refusal of `*.part` would
    * refuse every backup there is. What makes a staging name dangerous is
    * whose file it stages FOR.
    */
   char part[SYS_PATH_MAX];
   int k = snprintf(part, sizeof part, "%s.part", live);
   if (k <= 0 || (size_t)k >= sizeof part)
      return BACKUP_DEST_FAIL;
   char restoring[SYS_PATH_MAX];
   k = snprintf(restoring, sizeof restoring, "%s.restoring-", live);
   if (k <= 0 || (size_t)k >= sizeof restoring)
      return BACKUP_DEST_FAIL;
   size_t dn = strlen(dest), rn = strlen(restoring);
   if (reaches(dest, out_path, part) ||
       (dn > rn && !strncmp(dest, restoring, rn))) {
      fprintf(stderr,
              "sync: REFUSING to back up onto %s.\n"
              "  That is a STAGING name (see srv/deploy/lock.sh): a backup, a "
              "restore or a\n"
              "  deploy renames files at those names over the live database, "
              "or deletes them\n"
              "  before writing its own. A published backup must have a name "
              "of its own.\n",
              out_path);
      return BACKUP_DEST_ALIAS;
   }
   return BACKUP_DEST_OK;
}

/* A COPY THAT IS SAFE TO TAKE WHILE THE SERVER IS RUNNING.
 *
 * Copying sync.db with cp is not a backup: in WAL mode the committed data
 * lives partly in sync.db-wal, so the copy is a database missing every
 * transaction since the last checkpoint -- and one taken mid-checkpoint can
 * be torn outright. The restore then looks fine (it opens, it queries) and is
 * quietly short of whatever was synced most recently, which is exactly the
 * data anyone restoring is trying to get back.
 *
 * sqlite3_backup does it properly: it reads through the same connection, so
 * it sees the WAL, and it restarts itself if a writer changes a page it has
 * already copied. Called from the CLI, with the server running or not.
 */
int db_backup(struct db *d, const char *out_path)
{
   sqlite3 *src = db_handle(d);
   if (!src || !out_path || !*out_path)
      return 0;
   /* CHECKED HERE, not only where the CLI parses its arguments: this is the
    * function that CREATES the file, and a caller that stages into a name it
    * chose itself (the CLI's `<dest>.part`) has a second destination nobody
    * else looked at. Both go through the same test. */
   if (db_backup_dest(d, out_path) != BACKUP_DEST_OK)
      return 0;
   sqlite3 *dst = NULL;
   if (sqlite3_open(out_path, &dst) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot create %s: %s\n", out_path,
              dst ? sqlite3_errmsg(dst) : "?");
      sqlite3_close(dst);
      return 0;
   }
   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   int ok            = 0;
   if (b) {
      /* -1: the whole database in one step, then finish. On this data (a few
       * megabytes) the copy is milliseconds, and a page-at-a-time loop would
       * only widen the window in which a writer forces a restart. */
      int rc = sqlite3_backup_step(b, -1);
      ok     = rc == SQLITE_DONE;
      sqlite3_backup_finish(b);
      if (!ok)
         fprintf(stderr, "sync: backup failed: %s\n", sqlite3_errstr(rc));
   } else {
      fprintf(stderr, "sync: backup failed: %s\n", sqlite3_errmsg(dst));
   }
   /* The copy must be CLOSED before anyone is told it exists: sqlite writes
    * the last pages on close, and a caller that renames it into place first
    * would publish a file that is still being written. */
   if (sqlite3_close(dst) != SQLITE_OK) {
      fprintf(stderr, "sync: backup could not be closed cleanly\n");
      ok = 0;
   }
   return ok;
}

/* WHAT A RESTORE DRILL ASKS. A backup nobody has ever restored is a hope, not
 * a backup, so this is the other half: open a copy and have sqlite verify its
 * own structure, then confirm the schema this server needs is in it.
 *
 * ---- WHY IT IS A COPY, AND WHY THE COPY IS SOMEWHERE PRIVATE ----------
 *
 * Opening a database CREATES FILES beside it: a -wal and a -shm, at minimum.
 * Verifying a backup must not leave those in the backups directory. Noting
 * which of them are absent before the open and removing those same NAMES
 * afterwards is the obvious way to do it.
 *
 * It is also a time-of-check to time-of-use hole with data loss on the far
 * side.
 * The names are `<path>-wal` and `<path>-shm`, and between the note and the
 * removal anything may create them -- most obviously a server starting on
 * that database, which is not a hypothetical: srv/deploy/restore.sh verifies
 * a staged file sitting in the LIVE data directory while the board is
 * running, and deploy.sh verifies a backup taken seconds earlier. A -wal that
 * belongs to a running server is every commit since its last checkpoint.
 * Deleting it is not litter, it is the data.
 *
 * So nothing outside a private directory is opened by sqlite or unlinked. The
 * file is copied (with its own -wal, if it has one, so a WAL database is
 * verified whole) into a directory created by mkdtemp -- a name no other
 * process has ever seen -- and the COPY is what sqlite opens. Whatever
 * sidecars that open creates land in there, and only there.
 *
 * The original is opened O_RDONLY, once, to read its bytes. Verification
 * therefore cannot migrate it, stamp it, checkpoint it or repair it -- not as
 * an argument about SQLITE_OPEN_READONLY, but as a property of never handing
 * sqlite the path at all. */

/* THE PRIVATE DIRECTORY, held open as a descriptor.
 *
 * Every later operation on it goes through `fd` with openat/unlinkat, so a
 * rename of one of its parents cannot redirect a single one of them at
 * another file. `dev` is the filesystem it lives on: anything in it that
 * claims to be somewhere else is not ours to delete. */
struct scratch {
   char dir[SYS_PATH_MAX];
   int fd;
   dev_t dev;
};

/* Create it beside `near` if that directory will take it, otherwise under
 * TMPDIR. BESIDE FIRST on purpose: the board this runs on has a tmpfs /tmp
 * of a few megabytes and a database larger than that, so a copy into /tmp
 * would fail exactly on the machine the backups are of. */
static int scratch_make(const char *near, struct scratch *s)
{
   char base[SYS_PATH_MAX];
   int k = snprintf(base, sizeof base, "%s", near);
   if (k <= 0 || (size_t)k >= sizeof base)
      return 0;
   char *slash = strrchr(base, '/');
   if (slash == base)
      base[1] = '\0';
   else if (slash)
      *slash = '\0';
   else
      (void)snprintf(base, sizeof base, ".");
   const char *tmp     = getenv("TMPDIR");
   const char *where[] = {base, tmp && *tmp ? tmp : "/tmp"};
   for (size_t i = 0; i < sizeof where / sizeof where[0]; i++) {
      k = snprintf(s->dir, sizeof s->dir, "%s/.pancra-verify-XXXXXX", where[i]);
      if (k <= 0 || (size_t)k >= sizeof s->dir)
         continue;
      if (!mkdtemp(s->dir))
         continue;
      s->fd = open(s->dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      struct stat st;
      if (s->fd >= 0 && fstat(s->fd, &st) == 0) {
         s->dev = st.st_dev;
         return 1;
      }
      if (s->fd >= 0)
         close(s->fd);
      (void)rmdir(s->dir);
   }
   fprintf(stderr,
           "sync: cannot make a scratch directory to verify %s in\n"
           "  (tried beside it and $TMPDIR; verification never opens the file "
           "itself)\n",
           near);
   return 0;
}

#ifdef DB_FAULTS
/* A CLEANUP THAT FAILS, ON DEMAND.
 *
 * The failure this guards is a filesystem refusing to unlink a file the
 * process just created, which no real filesystem will do to order -- and a
 * path that cannot be reached is a path nobody has ever run. With
 * DB_KEEP_SCRATCH set the copy is left where it is, so the rmdir below fails
 * for the true reason (the directory is not empty) and the whole answer, up
 * to the CLI's exit status, is the one a real failure would produce.
 *
 * Nothing that ships defines DB_FAULTS. */
static int fault_keep_scratch(void)
{
   const char *v = getenv("DB_KEEP_SCRATCH");
   return v && *v && *v != '0';
}
#else
#define fault_keep_scratch() 0
#endif

/* EMPTY IT, AND ONLY IT.
 *
 * Every entry is opened THROUGH the directory descriptor with O_NOFOLLOW
 * first, and unlinked only if the descriptor says it is a plain file, on this
 * directory's own filesystem, with exactly one name. That last condition is
 * the one that matters: a file with a second hard link is a file somebody
 * else can still see, and this call did not create it.
 *
 * Anything that fails a check is LEFT, with a line saying so, and the
 * directory then refuses to rmdir -- which is the loud version of the failure
 * and infinitely preferable to unlinking a stranger's file.
 *
 * AND THE CALLER IS TOLD. This returned void, so every one of the
 * failures below was a line on stderr underneath a command that exited 0.
 * What is left when one of them happens is a COMPLETE COPY OF THE DATABASE --
 * every session cookie and password hash in it -- so "could not remove" is an
 * operational failure of the verification, not a note about housekeeping.
 *
 * 1 when the directory and everything in it is gone. 0 when something is
 * left, and `left` (SYS_PATH_MAX bytes) then names exactly what, because an
 * operator cannot delete a path they have to reconstruct from three stderr
 * lines. */
static int scratch_drop(struct scratch *s, char *left)
{
   if (left)
      left[0] = 0;
   int walk = dup(s->fd);
   DIR *dp  = walk >= 0 ? fdopendir(walk) : NULL;
   if (!dp) {
      if (walk >= 0)
         close(walk);
      close(s->fd);
      fprintf(stderr, "sync: scratch directory %s could not be read back\n",
              s->dir);
      /* The directory is still there and so is the copy inside it; nothing
       * was enumerated, so the directory itself is what has to be dealt
       * with. */
      if (left)
         (void)snprintf(left, SYS_PATH_MAX, "%s", s->dir);
      return 0;
   }
   int clean = 1;
   struct dirent *e;
   /* readdir REPORTS ITS OWN FAILURE THROUGH errno, and it has to be asked:
    * NULL means "the end of the directory" and "the directory could not be
    * read any further" alike, and the second one leaves entries this loop
    * never saw -- copies of the database that the rmdir below then fails on
    * for a reason nothing has explained. */
   errno = 0;
   while ((e = readdir(dp))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      int fd = openat(s->fd, e->d_name, O_RDONLY | O_NOFOLLOW);
      struct stat st;
      if (fd < 0 || fstat(fd, &st) != 0) {
         if (fd >= 0)
            close(fd);
         fprintf(stderr, "sync: leaving %s/%s: it could not be identified\n",
                 s->dir, e->d_name);
         clean = 0;
         errno = 0;
         continue;
      }
      int mine = S_ISREG(st.st_mode) && st.st_dev == s->dev && st.st_nlink == 1;
      /* A close that fails on a descriptor opened for READING has told us
       * nothing about the file, and the unlink below is what removes it --
       * so it is noted and does not change the verdict. */
      (void)close(fd);
      if (!mine) {
         fprintf(stderr,
                 "sync: leaving %s/%s: it is not a file this verification "
                 "created\n",
                 s->dir, e->d_name);
         clean = 0;
         errno = 0;
         continue;
      }
      if (fault_keep_scratch() || unlinkat(s->fd, e->d_name, 0) != 0) {
         fprintf(stderr, "sync: leaving %s/%s: it could not be removed\n",
                 s->dir, e->d_name);
         clean = 0;
      }
      errno = 0;
   }
   if (errno != 0) {
      fprintf(stderr,
              "sync: scratch directory %s could not be read to the end; "
              "anything past that point is still there\n",
              s->dir);
      clean = 0;
   }
   if (closedir(dp) != 0) /* also closes `walk` */
      fprintf(stderr, "sync: scratch directory %s did not close cleanly\n",
              s->dir);
   (void)close(s->fd);
   /* THE rmdir IS THE PROOF. It can only succeed on an empty directory, so a
    * file this loop failed to remove -- or never saw -- makes it fail, and a
    * clean=1 that reaches here with something still inside is corrected by
    * the answer of the kernel rather than by this function's bookkeeping. */
   if (rmdir(s->dir) != 0) {
      fprintf(stderr, "sync: scratch directory %s could not be removed\n",
              s->dir);
      clean = 0;
   }
   if (!clean && left)
      (void)snprintf(left, SYS_PATH_MAX, "%s", s->dir);
   return clean;
}

/* Copy `src` into the scratch directory under `name`. `must` is whether its
 * absence is a failure -- the database itself must be there, its -wal need
 * not be. */
static int scratch_copy(struct scratch *s, const char *src, const char *name,
                        int must)
{
   int in = open(src, O_RDONLY);
   if (in < 0) {
      if (must)
         fprintf(stderr, "sync: cannot read %s\n", src);
      return !must;
   }
   int out = openat(s->fd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
   if (out < 0) {
      close(in);
      fprintf(stderr, "sync: cannot write the scratch copy of %s\n", src);
      return 0;
   }
   char buf[65536];
   int ok = 1;
   for (;;) {
      ssize_t r = read(in, buf, sizeof buf);
      if (r == 0)
         break;
      if (r < 0) {
         ok = 0;
         break;
      }
      for (ssize_t off = 0; off < r;) {
         ssize_t w = write(out, buf + off, (size_t)(r - off));
         if (w <= 0) {
            ok = 0;
            break;
         }
         off += w;
      }
      if (!ok)
         break;
   }
   if (close(out) != 0)
      ok = 0;
   close(in);
   if (!ok)
      fprintf(stderr, "sync: %s could not be copied for verification\n", src);
   return ok;
}

/* The checks themselves, on `copy`. Every diagnostic names `as` -- the path
 * the operator asked about -- because the scratch copy is an implementation
 * detail and naming it in a refusal sends somebody looking for a file that no
 * longer exists. */
static int verify_copy(const char *copy, const char *as)
{
   sqlite3 *h = NULL;
   /* READWRITE, on a copy that is deleted moments from now: a WAL database
    * needs to recover its log to be read at all, and that recovery is exactly
    * what a restore would do. Nothing here writes on purpose; the file it
    * could write to is the copy. */
   if (sqlite3_open_v2(copy, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
      fprintf(stderr, "sync: cannot open %s: %s\n", as,
              h ? sqlite3_errmsg(h) : "?");
      sqlite3_close(h);
      return 0;
   }
   sqlite3_stmt *st = NULL;
   int ok = sqlite3_prepare_v2(h, "PRAGMA integrity_check;", -1, &st, NULL) ==
            SQLITE_OK;
   const unsigned char *got = NULL;
   if (ok && sqlite3_step(st) == SQLITE_ROW)
      got = sqlite3_column_text(st, 0);
   ok = got && !sqlite3_stricmp((const char *)got, "ok");
   if (!ok)
      fprintf(stderr, "sync: %s FAILED integrity check: %s\n", as,
              got ? (const char *)got : "no answer");
   sqlite3_finalize(st);
   /* Structure is not enough: an empty file passes integrity_check. The
    * accounts, the registered phones and the synced rows are what make this
    * a Pancra database rather than a well-formed one. */
   if (ok) {
      st    = NULL;
      int q = sqlite3_prepare_v2(
          h,
          "SELECT count(*) FROM sqlite_master WHERE type='table'"
          " AND name IN ('user','app','logrow')",
          -1, &st, NULL);
      int have = 0;
      if (q == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
         have = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
      if (have != 3) {
         fprintf(stderr, "sync: %s is a database, but not this server's\n", as);
         ok = 0;
      }
   }
   /* ---- AND THE SAME COMPATIBILITY QUESTION db_open ASKS ---------------
    *
    * THREE TABLE NAMES WERE NOT A COMPATIBILITY CHECK. Everything above is
    * satisfied by a database that is perfectly intact, perfectly well formed,
    * and unusable by this build: one written by a NEWER server, or one whose
    * `logrow` has different columns, or whose cascades are gone. Every one of
    * those passes integrity_check and has all three names.
    *
    * WHAT THIS PREFLIGHT IS FOR decides how bad that is. It is what restore
    * asks BEFORE the backup DISPLACES the live database. So the sequence was:
    * verify says yes, the good database is replaced by the unusable one, and
    * the server then fails to start -- turning a refusal that costs nothing
    * into an outage that has to be recovered from, with the operator's own
    * data now the thing needing rescue. A preflight weaker than the open it
    * precedes is a promise it is not entitled to make.
    *
    * READ-ONLY IN THE ONLY SENSE THAT MATTERS: version_supported and
    * schema_usable only run PRAGMAs and SELECTs, and the file they run
    * against is a private copy of the operator's, so verifying a backup
    * cannot migrate it, stamp it or repair it. A backup that needs a
    * migration is still a valid backup -- db_open will migrate it once it is
    * in place -- which is why schema_usable accepts an older version rather
    * than demanding the current one. */
   if (ok) {
      int at = 0;
      if (!version_supported(h, as, &at) || !schema_usable(h, as, at)) {
         fprintf(stderr,
                 "sync: %s is intact, but this build cannot use it.\n"
                 "  REFUSING IT AS A BACKUP. It would replace a working "
                 "database with one the server\n"
                 "  cannot open, and the failure would appear at startup, "
                 "after the good copy was gone.\n"
                 "  WHAT TO DO: use a different backup, or run the server "
                 "build that wrote this one.\n",
                 as);
         ok = 0;
      }
   }
   sqlite3_close(h);
   return ok;
}

enum verify_result db_verify(const char *path)
{
   if (!path || !*path)
      return VERIFY_BAD;
   if (sqlite3_initialize() != SQLITE_OK)
      return VERIFY_BAD;
   struct scratch s;
   if (!scratch_make(path, &s))
      return VERIFY_BAD;
   char copy[SYS_PATH_MAX], wal[SYS_PATH_MAX];
   int k  = snprintf(copy, sizeof copy, "%s/db", s.dir);
   int kw = snprintf(wal, sizeof wal, "%s-wal", path);
   int ok =
       k > 0 && (size_t)k < sizeof copy && kw > 0 && (size_t)kw < sizeof wal;
   /* The -wal is copied when it is there and skipped when it is not; it is
    * never created, never touched and never removed. */
   if (ok)
      ok =
          scratch_copy(&s, path, "db", 1) && scratch_copy(&s, wal, "db-wal", 0);
   if (ok)
      ok = verify_copy(copy, path);
   /* THE CLEANUP IS PART OF THE ANSWER, AND THE VERDICT COMES FIRST (item
    * 307). A file that is not a usable backup is what the operator asked
    * about; saying "there is a leftover" instead would answer a question
    * nobody put. The leftover is named on stderr either way, so nothing is
    * lost by ranking them. */
   char left[SYS_PATH_MAX];
   int clean = scratch_drop(&s, left);
   if (!clean)
      fprintf(stderr,
              "sync: A COPY OF %s IS STILL ON DISK, at %s\n"
              "  It holds every session cookie and password hash the "
              "database holds.\n"
              "  WHAT TO DO: remove that directory by hand, then run this "
              "again.\n",
              path, left[0] ? left : s.dir);
   if (!ok)
      return VERIFY_BAD;
   return clean ? VERIFY_OK : VERIFY_LEFTOVER;
}

/* ---- REMOVING ORPHAN ROWS, WHEN AN OPERATOR ASKS -----------------------
 *
 * WHY THIS IS A VERB AND NOT AUTOMATIC.
 *
 * db_open refuses a database holding rows whose owner no longer exists, and
 * it is right to: an orphan `session` row is a live cookie for a deleted
 * account, and the moment the freed user id is handed to the next account
 * that account inherits it. Repairing it silently at startup would delete
 * rows nobody asked about, in a process that is about to serve traffic, and
 * would hide the fact that something produced them.
 *
 * But the obvious advice for that refusal -- "open the file with the sqlite3
 * shell and DELETE the rows PRAGMA foreign_key_check names" -- means
 * hand-written SQL against a live database, on a board that may not have the
 * sqlite3 binary. That is a worse thing to ask of somebody at seven in the
 * morning with the service down.
 *
 * So: the operator asks, explicitly, with the server stopped, and gets a
 * report of exactly what went. ONE TRANSACTION, so a failure part-way leaves
 * the file as it was rather than half-repaired.
 *
 * IT NEVER TOUCHES A PARENT ROW. Every deletion here is a CHILD that
 * references something absent; the accounts, invitations and readings that do
 * have owners are not what this is about, and a repair that could remove one
 * would be a far worse tool than the problem it fixes. */
int db_fsck(struct db *d, int fix, int64_t *removed)
{
   if (removed)
      *removed = 0;
   if (!d)
      return 0;
   sqlite3 *h = db_handle(d);
   if (!h)
      return 0;

   /* PRAGMA foreign_key_check names (child table, rowid, parent, index). The
    * rowid is what makes a targeted delete possible: it identifies the ONE
    * row, so nothing is matched by content and nothing else can be caught. */
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(h, "PRAGMA foreign_key_check;", -1, &st, NULL) !=
       SQLITE_OK) {
      fprintf(stderr, "sync: cannot check for orphan rows: %s\n",
              sqlite3_errmsg(h));
      return 0;
   }

   /* Collected FIRST, then deleted: stepping a PRAGMA while deleting the rows
    * it is walking is not something to rely on. */
   struct orphan {
      char table[64];
      sqlite3_int64 rowid;
   };

   enum { ORPHAN_MAX = 4096 };
   static struct orphan found[ORPHAN_MAX];
   int64_t n = 0;
   int over  = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      if (n >= ORPHAN_MAX) {
         over = 1;
         break;
      }
      const char *child = (const char *)sqlite3_column_text(st, 0);
      if (!child)
         continue;
      snprintf(found[n].table, sizeof found[n].table, "%s", child);
      found[n].rowid = sqlite3_column_int64(st, 1);
      n++;
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE && !over) {
      fprintf(stderr, "sync: the orphan scan did not finish: %s\n",
              sqlite3_errmsg(h));
      return 0;
   }
   if (over) {
      fprintf(stderr,
              "sync: more than %d orphan rows -- refusing to guess at this "
              "scale.\n  A file this damaged wants a restore from backup, not "
              "a repair.\n",
              ORPHAN_MAX);
      return 0;
   }

   if (n == 0) {
      printf("fsck: no orphan rows\n");
      return 1;
   }
   for (int64_t i = 0; i < n; i++)
      printf("fsck: orphan in `%s` rowid %lld\n", found[i].table,
             (long long)found[i].rowid);
   if (!fix) {
      printf("fsck: %" PRIwire
             " orphan row%s found; re-run with `fix` to remove "
             "them\n",
             n, n == 1 ? "" : "s");
      return 1;
   }

   /* ONE TRANSACTION. Half a repair is a database in a state nobody chose. */
   if (!db_durable_begin(d))
      return 0;
   int64_t gone = 0;
   for (int64_t i = 0; i < n; i++) {
      char sql[160];
      /* The table name comes from sqlite's own catalogue, not from a user --
       * but it is still spliced into SQL, so it is quoted as an identifier
       * with any embedded quote doubled, which is what sqlite's %w does. */
      char *q = sqlite3_mprintf("DELETE FROM \"%w\" WHERE rowid = %lld;",
                                found[i].table, (long long)found[i].rowid);
      if (!q) {
         db_durable_rollback(d);
         return 0;
      }
      snprintf(sql, sizeof sql, "%s", q);
      sqlite3_free(q);
      if (!db_exec(d, sql)) {
         fprintf(stderr, "sync: could not remove the orphan in `%s`: %s\n",
                 found[i].table, db_errmsg(d));
         db_durable_rollback(d);
         return 0;
      }
      gone += db_changes(d);
   }
   if (!db_durable_commit(d)) {
      db_durable_rollback(d);
      return 0;
   }
   printf("fsck: removed %" PRIwire " orphan row%s\n", gone,
          gone == 1 ? "" : "s");
   if (removed)
      *removed = gone;
   return 1;
}
