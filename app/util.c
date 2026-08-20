// SPDX-License-Identifier: GPL-3.0
// util.c --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#include "util.h"
#include "dexlibc.h"
#include "thread.h"    /* mutex: one append at a time, see append_lk */
#include <stdatomic.h> /* the mutation counter is written by two threads */
#include <stdio.h>

/* snprintf returns the would-be length, which can exceed the buffer on
 * truncation; clamp so write() emits only bytes actually in the buffer. */
int clampn(int n, int cap)
{
   if (n < 0)
      return 0;
   return n >= cap ? cap - 1 : n;
}

void str_snapshot(char *dst, int cap, const char *src)
{
   /* cap <= 0 skipped the copy loop and then wrote dst[0] anyway -- a
    * one-byte overflow into a zero-length buffer. No caller passes 0 today
    * (every one passes a sizeof), but this is the project's single string
    * copy and it should not have a size at which it corrupts memory. */
   if (cap <= 0)
      return;
   int i = 0;
   for (; i < cap - 1 && src[i]; i++)
      dst[i] = src[i];
   dst[i] = 0;
}

/* ALL THE BYTES, OR AN ERROR. write(2) may move fewer bytes than asked --
 * that is not a failure, it is the contract -- so every caller that treats a
 * short write as success is writing a truncated record. The header writes in
 * the four CSV logs did exactly that, and a half-written "# written,id,..."
 * line comments out the first REAL row appended after it. */

/* ---- DELIBERATE FAILURES, for the durability test only ----------------
 *
 * Behind -DAPP_FAULTS, which only `make durabilitytest` sets: nothing that
 * ships carries it. It exists because the failures this file's contract is
 * ABOUT cannot be arranged on demand -- a short write can (RLIMIT_FSIZE), but
 * an fsync that fails, a close that reports a deferred write error, or a
 * rename that fails after everything else succeeded are exactly the moments
 * where "the record is safe" stops being true, and they happen on a card
 * that is dying rather than in a test.
 *
 * Each is armed by name through the environment (APP_FAIL_FSYNC=1 ...), so a
 * test arms one, runs a real append against a real file, and asks what the
 * caller was told.
 */
#ifdef APP_FAULTS
#include <stdlib.h> /* getenv */

static int fault_on(const char *var)
{
   const char *s = getenv(var);
   return s && *s && *s != '0';
}

/* ...and one that is PER THREAD, because the concurrency case needs one
 * writer to fail while another succeeds. See util.h. */
_Thread_local int app_fault_fsync_here;

/* AND A WAY TO HOLD THE WINDOW OPEN. The bug this guards is a race whose
 * window is two syscalls wide; a test that merely runs both writers hard
 * relies on luck to hit it, and a test that passes by luck is not a test.
 * When set, this runs at the exact instant the window opens -- after the
 * flush has failed and before the row is taken back -- so the other writer
 * lands in it every round. Test builds only. */
_Thread_local void (*app_fault_gap_here)(void);

#define FAULT_FSYNC    (fault_on("APP_FAIL_FSYNC") || app_fault_fsync_here)
#define FAULT_CLOSE    fault_on("APP_FAIL_CLOSE")
#define FAULT_RENAME   fault_on("APP_FAIL_RENAME")
#define FAULT_TRUNCATE fault_on("APP_FAIL_TRUNCATE")
#define FAULT_DIRSYNC  fault_on("APP_FAIL_DIRSYNC")

/* A WRITE THAT FAILS. The restore path stages a whole log and then publishes
 * it, and its promise is all-or-nothing: a bucket that could not be written
 * must leave the original untouched rather than publish a log short by exactly
 * the rows the user asked to get back. Nothing could arrange that from outside
 * -- a fetch failure needs a server that misbehaves on cue -- so the one branch
 * that matters most was the one no test could reach.
 *
 * ...AND A WRITE THAT FAILS PART WAY THROUGH ONE OPERATION, which is a
 * different fault and the one the all-or-nothing branch actually needs.
 *
 * APP_FAIL_WRITE fails EVERY write_all, so in a restore the FIRST write to
 * fail is the one that copies the live log into the staging file -- the copy is
 * abandoned, and the per-bucket "all of it or none of it" refusal below it is
 * never reached at all. Measured, not assumed: with only that switch,
 * replacing that refusal with `if (0)` -- publishing a staged log that is short
 * by the buckets that failed -- passed the entire interop suite.
 *
 * So APP_FAIL_WRITE_AFTER=<n> lets the first n write_all calls through and
 * fails the (n+1)th, which puts the failure AFTER the copy and after a bucket
 * or two have already been staged. The count starts when the variable is SET
 * rather than when the process starts: a test arms it immediately before the
 * one operation it means to break, and the writes it performed earlier -- other
 * restores, other logs, its own fixtures -- must not be charged against it. */
static int fault_write_after(void)
{
   /* Not atomic, and it does not need to be: while the variable is unset this
    * only READS `armed`, so the concurrent writers in durabilitytest (which
    * never set it) race with nothing. Arming is done by a single-threaded
    * test, before the thread that writes is started. */
   static int armed;
   static long seen;
   const char *s = getenv("APP_FAIL_WRITE_AFTER");
   if (!s || !*s) {
      if (armed)
         armed = 0;
      return 0;
   }
   if (!armed) {
      armed = 1;
      seen  = 0;
   }
   long want = 0;
   for (const char *p = s; *p >= '0' && *p <= '9'; p++)
      want = (want * 10) + (*p - '0');
   return seen++ >= want;
}

#define FAULT_WRITE (fault_on("APP_FAIL_WRITE") || fault_write_after())
#else
#define FAULT_FSYNC    0
#define FAULT_CLOSE    0
#define FAULT_RENAME   0
#define FAULT_TRUNCATE 0
#define FAULT_DIRSYNC  0
#define FAULT_WRITE    0
#endif

/* Placed AFTER the fault macros, not with the other small helpers above:
 * FAULT_WRITE is defined there, and a use before the definition is silently a
 * call to an undeclared function rather than an error the reader expects. */
int write_all(int fd, const void *data, int len)
{
   const unsigned char *p = data;
   int left               = len;
   if (FAULT_WRITE)
      return -1;
   while (left > 0) {
      long w = write(fd, p, (size_t)left);
      /* EINTR IS NOT A FAILED WRITE. A write interrupted by a signal before
       * it moved any bytes returns -1 and sets EINTR, and this read that as
       * fatal -- so every caller (nine of them, including the ones that
       * persist a reading and a dose) could lose a whole file to a signal
       * arriving at the wrong instant. The app takes signals: the crash
       * handler installs them, and the JVM uses them for its own purposes on
       * threads that also do this file I/O.
       *
       * Retried, and no bytes are re-sent: on EINTR the kernel has written
       * none, and a short write is already handled by the loop. */
      if (w < 0 && errno == EINTR)
         continue;
      if (w <= 0)
         return -1;
      p += w;
      left -= (int)w;
   }
   return 0;
}

/* THE DIRECTORY ENTRY IS PART OF THE FILE. A rename is durable only once the
 * DIRECTORY has been synced: without this, a save that returned success can
 * still be gone after sudden power loss, with the file's own contents safely
 * on disk under a name nothing points to. */
int fsync_dir_of(const char *path)
{
   char dir[320];
   int slash = -1;
   for (int i = 0; path[i] && i < (int)sizeof dir - 1; i++) {
      dir[i]     = path[i];
      dir[i + 1] = 0;
      if (path[i] == '/')
         slash = i;
   }
   if (slash < 0) {
      dir[0] = '.';
      dir[1] = 0;
   } else if (slash == 0) {
      dir[1] = 0;
   } else {
      dir[slash] = 0;
   }
   int dfd = open(dir, O_RDONLY, 0);
   if (dfd < 0)
      return -1;
   int ok = fsync(dfd) == 0 && !FAULT_DIRSYNC;
   if (close(dfd) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

/* FINISH A REWRITE: sync the new contents, close, rename over the old file,
 * and sync the directory -- checking every one of those. `fd` is consumed
 * either way; on any failure the temporary is removed and the original is
 * left exactly as it was.
 *
 * Extracted from atomic_replace because the streamed rewrites (the weight and
 * dose logs, which are too big to build in memory) were doing the same job by
 * hand and skipping most of it: no fsync, an unchecked close, and no
 * directory sync. A rewrite that loses power between rename and sync leaves
 * the log EMPTY -- the temp file's data was never on disk. */
enum replace_result replace_finish(int fd, const char *tmp, const char *path)
{
   int ok = fsync(fd) == 0 && !FAULT_FSYNC;
   if (close(fd) != 0 || FAULT_CLOSE)
      ok = 0;
   /* The fault is checked BEFORE the rename, not after: `rename(...) ||
    * FAULT` would perform the rename and then report failure, which is a
    * state this function promises never to leave -- the caller would be told
    * the rewrite failed while the file had already been replaced. */
   if (ok && (FAULT_RENAME || rename(tmp, path) != 0))
      ok = 0;
   if (!ok) {
      (void)unlink(tmp);
      return REPLACE_FAILED; /* the rename never happened */
   }
   /* PAST THE POINT OF NO RETURN. The rename succeeded, so `path` already
    * names the new contents whatever happens here; the caller must not undo
    * anything on the strength of this. */
   return fsync_dir_of(path) == 0 ? REPLACE_OK : REPLACE_UNSYNCED;
}

/* UNDO A PARTIAL WRITE, and SAY whether the undo worked.
 *
 * A row that stopped halfway has no newline, so the next append lands on the
 * same line and the pair parses as ONE row of spliced fields -- a fabricated
 * reading at a fabricated time, indistinguishable from a real one for ever
 * after. Truncating back is the only way to keep the log parseable.
 *
 * The result was previously discarded at every call site: `(void)ftruncate`.
 * A truncate that fails is exactly the case that matters -- the half row is
 * still there, and the caller was about to be told the append merely failed,
 * which reads as "nothing happened". It did not; the file is damaged, and the
 * caller must say so. `by` is what THIS writer wrote, measured from the end as
 * it stands now rather than from an offset sampled before the write: with
 * O_APPEND another writer can append in between, and truncating to the stale
 * offset would delete that writer's complete row too. */
int rollback_tail(int fd, long by)
{
   if (by <= 0)
      return 0;
   long end = lseek(fd, 0, SEEK_END);
   if (end < by)
      return -1;
   if (FAULT_TRUNCATE)
      return -1;
   return ftruncate(fd, end - by) == 0 ? 0 : -1;
}

/* FINISH AN APPEND DURABLY. Flush this file's data, close it (checked), and
 * -- when the append CREATED the file -- flush the directory entry too.
 *
 * Each of the four append paths did part of this: the write was checked and
 * the close was checked, but nothing was ever flushed. A record the user
 * logged is then "written" only in the page cache, and a phone that loses
 * power (or is killed and restarted by Android, which is routine) can come
 * back with the file exactly as it was -- the dose they took, gone, with the
 * screen having said it was saved.
 *
 * The directory sync matters only for a NEW file, and it matters absolutely:
 * without it the file's contents can be safely on disk under a name nothing
 * points to, which is the same as not existing. */
int append_finish(int fd, const char *path, int created)
{
   int ok = fsync(fd) == 0 && !FAULT_FSYNC;
   /* A CLOSE THAT FAILS MEANS THE ROW MAY NOT BE THERE. Deferred write errors
    * (a full disk, a card pulled out) surface at close, not at write. */
   if (close(fd) != 0 || FAULT_CLOSE)
      ok = 0;
   if (ok && created && fsync_dir_of(path) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

/* ONE RECORD INTO AN APPEND-ONLY LOG, with the first one atomic.
 *
 * All four logs -- readings, weights, doses, provenance -- had their own copy
 * of this, and all four had the same two holes:
 *
 *   THE FIRST RECORD WAS NOT ATOMIC. A missing file was created and the
 *   header written straight into it, then the row. Anything that failed after
 *   the row reached the file still reported failure, so the caller retried --
 *   and the retry appended the row a SECOND time, to a file that already had
 *   it. A duplicate dose in the log the user reads is not a cosmetic defect.
 *   Now the header and the first row go into a temporary and are renamed into
 *   place: either the file exists with both, or it does not exist at all, and
 *   a retry starts from nothing.
 *
 *   A FAILED ROLLBACK WAS INDISTINGUISHABLE from a clean failure. A row that
 *   stopped halfway is truncated away so the next append cannot splice onto
 *   it; when that truncate ALSO fails, the file is damaged and the caller was
 *   told the same thing as "nothing happened". They are different facts and
 *   they need different answers -- see the return codes.
 *
 * Returns LOG_OK, LOG_FAIL (nothing was written; the file is as it was) or
 * LOG_DAMAGED (a partial row is still in the file). */
/* ONE APPEND AT A TIME, ACROSS ALL FOUR LOGS.
 *
 * The recovery is the reason. When the flush fails, the row is already in the
 * file and has to be taken back -- by truncating what THIS writer wrote,
 * measured from the end as it stands now, because O_APPEND means a remembered
 * offset may no longer be ours. But "the end as it stands now" is only our
 * row if nobody appended in the gap between the failed flush and the reopen,
 * and something can: store_record releases the history lock before
 * store_append, so two threads reach here for readings.csv concurrently. The
 * rollback would then delete the OTHER writer's complete row -- a reading
 * that was accepted, acknowledged and silently dropped.
 *
 * So the whole operation -- write, flush, and any take-back -- is one
 * critical section. It is coarse on purpose: one lock for every log rather
 * than one per path, because the appends are a few dozen bytes each and a
 * table of per-path locks is a second thing to get right for no measured
 * gain. A leaf lock: nothing else is taken while it is held. */
static struct mutex append_lk = MUTEX_INIT;

static int log_append_locked(const char *path, const void *hdr, int hdrlen,
                             const void *row, int rowlen);

int log_append(const char *path, const void *hdr, int hdrlen, const void *row,
               int rowlen)
{
   mutex_lock(&append_lk);
   int rc = log_append_locked(path, hdr, hdrlen, row, rowlen);
   mutex_unlock(&append_lk);
   return rc;
}

static int log_append_locked(const char *path, const void *hdr, int hdrlen,
                             const void *row, int rowlen)
{
   if (!path || !*path || !row || rowlen <= 0)
      return LOG_FAIL;
   int fd   = open(path, O_WRONLY | O_APPEND, 0600);
   long end = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
   if (fd >= 0 && end > 0) {
      /* THE ORDINARY CASE: the log exists and has something in it. */
      long w = write(fd, row, rowlen);
      if (w != rowlen) {
         int rb = rollback_tail(fd, w > 0 ? w : 0);
         close(fd);
         return rb == 0 ? LOG_FAIL : LOG_DAMAGED;
      }
      /* THE FLUSH, and what to do when it fails.
       *
       * The row is in the file by now -- write(2) returned -- so a failed
       * fsync or close cannot be reported as "nothing happened": the caller
       * retries, and the retry writes the row a SECOND time into a log that
       * already holds it. Take it back instead, and flush THAT, so the file
       * is left as long as it was.
       *
       * If the take-back cannot itself be made durable, the file's state is
       * genuinely unknown -- the row may be there, or not -- and the caller
       * is told so rather than being told a clean failure. */
      if (append_finish(fd, path, 0) == 0)
         return LOG_OK;
#ifdef APP_FAULTS
      /* THE WINDOW, held open on demand: see app_fault_gap_here. */
      if (app_fault_gap_here)
         app_fault_gap_here();
#endif
      int back = open(path, O_WRONLY, 0600);
      if (back < 0)
         return LOG_DAMAGED;
      /* By exactly what WE wrote. No other writer can be inside this
       * function (append_lk), so the last `rowlen` bytes are ours -- which is
       * what makes measuring from the CURRENT end safe rather than merely
       * likely. */
      int undone = rollback_tail(back, rowlen) == 0;
      if (undone && append_finish(back, path, 0) == 0)
         return LOG_FAIL; /* the log is exactly as long as it was */
      if (!undone)
         close(back);
      return LOG_DAMAGED;
   }
   if (fd >= 0)
      close(fd); /* it exists but is empty: still needs its header */

   /* THE FIRST RECORD, atomically. */
   char tmp[320];
   int tn = snprintf(tmp, sizeof tmp, "%s.new", path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return LOG_FAIL;
   (void)unlink(tmp);
   int t = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (t < 0)
      return LOG_FAIL;
   if ((hdrlen > 0 && write_all(t, hdr, hdrlen) != 0) ||
       write_all(t, row, rowlen) != 0) {
      close(t);
      (void)unlink(tmp);
      return LOG_FAIL;
   }
   /* fsync, close, rename and the directory sync, each checked -- spelled out
    * here rather than through replace_finish because the LAST step needs a
    * different answer. replace_finish is for REPLACING a file: if its
    * directory sync fails the old file is gone and the new one is the best
    * state available. This is a CREATION, so the honest recovery is to leave
    * nothing: the caller is being told the record was not saved, and if the
    * file survives holding that record, the caller's retry writes it twice. */
   int ok = fsync(t) == 0 && !FAULT_FSYNC;
   if (close(t) != 0 || FAULT_CLOSE)
      ok = 0;
   if (!ok) {
      (void)unlink(tmp);
      return LOG_FAIL;
   }
   if (FAULT_RENAME || rename(tmp, path) != 0) {
      (void)unlink(tmp);
      return LOG_FAIL;
   }
   if (fsync_dir_of(path) != 0) {
      /* The name is in the directory but the entry may not survive a power
       * cut. "Failed" and "the file exists holding the record" cannot both be
       * true, so take the name back -- and the take-back has to be durable
       * too, or the caller is told a clean failure while a file holding the
       * record may still appear after a power cut. */
      if (unlink(path) != 0 || fsync_dir_of(path) != 0)
         return LOG_DAMAGED;
      return LOG_FAIL;
   }
   return LOG_OK;
}

enum replace_result atomic_replace(const char *path, const void *data, int len)
{
   if (!path || !*path || !data || len < 0)
      return REPLACE_FAILED;
   char tmp[320];
   int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
   if (n <= 0 || n >= (int)sizeof tmp)
      return REPLACE_FAILED;
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return REPLACE_FAILED;
   if (write_all(fd, data, len) != 0) {
      close(fd);
      (void)unlink(tmp);
      return REPLACE_FAILED;
   }
   return replace_finish(fd, tmp, path);
}

/* EVERY COMMITTED CHANGE TO A SYNCED RECORD, counted.
 *
 * The sync scheduler used to decide "is there anything new?" by adding up the
 * SIZES of the five synced files. Almost every change moves one of them, so it
 * worked almost always -- and the exceptions are the ones that matter:
 *
 *   - an EQUAL-LENGTH edit. A dose corrected from 12 to 13 units, a weight
 *     from 70.4 to 70.5, a sensor renamed to a name of the same length: the
 *     row is rewritten in place and the file is exactly as long as before.
 *   - CANCELLING deltas. One row deleted and another appended between two
 *     ticks, where the bytes happen to balance.
 *
 * In those cases the phone believed it had nothing to send. The correction
 * then waited for the six-hour safety sync -- and if the phone was restarted
 * or the app closed in that window, until whatever happened next changed a
 * size. A record the user has already corrected, sitting wrong on the server
 * for hours, is exactly the failure the sync exists to prevent.
 *
 * So a mutation announces itself. This counter is bumped by the code that
 * COMMITS one -- after the write succeeded, never before -- and folded into
 * the stamp. Within a process it catches everything, whatever the sizes do.
 * Across a restart it resets, and the file sizes it is folded with carry the
 * history instead: a fresh process has no "last synced" stamp either, so its
 * first tick syncs regardless.
 *
 * ATOMIC, because the writers are not one thread. A reading commits on a
 * BINDER thread, a dose or a weight on the MAIN thread, and the sync worker
 * READS this while both are running. `g++` on a plain long is a load, an add
 * and a store: two threads inside that window produce one increment, not two,
 * and a lost increment is a mutation the stamp never mentions -- exactly the
 * missed sync this counter exists to prevent, and one the file sizes cannot
 * catch either, because the case it covers is the edit whose sizes do not
 * move. (It is also a C data race, which is undefined rather than merely
 * unlucky.)
 *
 * RELAXED is enough: nothing is published THROUGH this counter. The reader
 * only asks whether the number differs from the one it last saw, and the
 * record itself was already committed to a file before the bump. */
static atomic_long g_record_gen;

void record_mutated(void)
{
   atomic_fetch_add_explicit(&g_record_gen, 1, memory_order_relaxed);
}

long record_generation(void)
{
   return atomic_load_explicit(&g_record_gen, memory_order_relaxed);
}

/* Build "<dir><name>", or REFUSE.
 *
 * This used to stop copying the directory at `cap - 32`, append the filename
 * on top of the truncated prefix, and return nothing. Two ways that is worse
 * than failing:
 *
 *   - the result is a DIFFERENT, WELL-FORMED PATH. A directory whose name is
 *     one byte too long silently becomes its own truncation, so the app
 *     reads and writes an entirely different location -- and finds it empty,
 *     which looks exactly like a first run.
 *   - two long directories that share a prefix truncate to the SAME path, so
 *     two data sets land on top of each other.
 *
 * And no caller could tell, because there was no answer to look at. The
 * length is computed exactly now: everything fits, or nothing is written and
 * the caller is told. */
int data_path(char *dst, int cap, const char *dir, const char *name)
{
   if (!dst || cap <= 0)
      return 0;
   dst[0] = 0;
   if (!dir || !name)
      return 0;
   size_t dn = 0, nn = 0;
   while (dir[dn])
      dn++;
   while (name[nn])
      nn++;
   /* The NUL is part of what has to fit. */
   if (dn + nn + 1 > (size_t)cap)
      return 0;
   for (size_t i = 0; i < dn; i++)
      dst[i] = dir[i];
   for (size_t j = 0; j < nn; j++)
      dst[dn + j] = name[j];
   dst[dn + nn] = 0;
   return 1;
}

/* ---- EVIDENCE THAT A LOG IS EMPTY ON PURPOSE -------------------------
 *
 * See util.h for why this exists. What follows is the FORMAT, and the format
 * is the whole of the safety: one reader, one writer, in one file, so the two
 * cannot drift apart the way log_scan and log_buckets once did in sync.c --
 * where the two readers disagreeing about what a row was deleted a day off
 * the server.
 *
 *     pancra-clear <version> <generation>\n
 *
 * and nothing else: the file is that line and ends there. VERSIONED because
 * this authorises deletion and a misread must never look like a valid
 * authorisation. A NEWER build that adds a field bumps the version and this
 * build answers "no evidence" -- refusing the empty push, which is the
 * fail-safe direction. An OLDER build predates the file entirely and never
 * looks at it, so it keeps refusing too. Neither can be talked into deleting
 * by bytes it does not understand.
 *
 * The GENERATION counts deliberate clears of this log, from 1. It is what
 * distinguishes evidence minted for THIS clear from a tombstone left over
 * from an earlier one: a second clear writes 2, so a caller (and a test) can
 * see that fresh evidence was really minted rather than an old file being
 * silently reused. */
#define CLEAR_MAGIC "pancra-clear"
#define CLEAR_VER   1

static int clear_path_of(char *out, int cap, const char *path)
{
   if (!out || cap <= 0)
      return 0;
   out[0] = 0;
   if (!path || !*path)
      return 0;
   int n = snprintf(out, (size_t)cap, "%s%s", path, LOG_CLEAR_SUFFIX);
   /* Never a truncation: a truncated tombstone path names a DIFFERENT file,
    * and the one it names might be another log's evidence. */
   if (n <= 0 || n >= cap) {
      out[0] = 0;
      return 0;
   }
   return 1;
}

/* The log's own length, or -1 when it cannot be answered at all (missing,
 * unreadable, unseekable). -1 is not "empty": see util.h. */
static long log_len_of(const char *path)
{
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   long n = lseek(fd, 0, SEEK_END);
   close(fd);
   return n < 0 ? -1 : n;
}

long log_clear_generation(const char *path)
{
   /* THE LOG FIRST, because this is the check that keeps a reinstall
    * fail-safe. A missing log is a HOLE, not a deliberate clear, and no
    * tombstone may speak for it; a log with bytes in it is not empty at all,
    * so there is nothing for evidence of emptiness to authorise. */
   if (log_len_of(path) != 0)
      return 0;
   char cp[320];
   if (!clear_path_of(cp, (int)sizeof cp, path))
      return 0;
   int fd = open(cp, O_RDONLY, 0);
   if (fd < 0)
      return 0;
   char b[64];
   long n = read(fd, b, sizeof b);
   close(fd);
   if (n <= 0 || n >= (long)sizeof b)
      return 0; /* empty, unreadable, or longer than this format can be */
   b[n] = 0;
   /* EXACT, field by field. Nothing here is scanned for a prefix or skipped
    * over: "pancra-clearish 1 1" and "pancra-clear 1 7x" must both read as no
    * evidence, because the answer authorises deleting the user's history. */
   const char *p     = b;
   const char *magic = CLEAR_MAGIC " ";
   for (int i = 0; magic[i]; i++)
      if (*p++ != magic[i])
         return 0;
   long ver = 0;
   int nd   = 0;
   for (; *p >= '0' && *p <= '9'; p++, nd++) {
      if (ver > 99)
         return 0;
      ver = (ver * 10) + (*p - '0');
   }
   if (nd == 0 || ver != CLEAR_VER || *p++ != ' ')
      return 0;
   long gen = 0;
   nd       = 0;
   for (; *p >= '0' && *p <= '9'; p++, nd++) {
      if (gen > 1000000000L)
         return 0;
      gen = (gen * 10) + (*p - '0');
   }
   /* The line ends, and the FILE ends with it: trailing bytes mean this is
    * not the file this format describes, whatever its first line says. */
   if (nd == 0 || gen <= 0 || *p++ != '\n' || (p - b) != n)
      return 0;
   return gen;
}

enum replace_result log_note_cleared(const char *path)
{
   /* EVIDENCE FOR A STATE THAT DOES NOT EXIST IS THE ONE THING THIS MUST NOT
    * MINT. The caller has just written the empty log; if it did not, or the
    * write did not land, there is nothing here to authorise and saying so is
    * the whole point. */
   if (log_len_of(path) != 0)
      return REPLACE_FAILED;
   char cp[320];
   if (!clear_path_of(cp, (int)sizeof cp, path))
      return REPLACE_FAILED;
   /* log_clear_generation has already established the log is present and
    * empty, so this reads the previous generation, or 0 when this is the
    * first deliberate clear. */
   long gen = log_clear_generation(path) + 1;
   char line[64];
   int n =
       snprintf(line, sizeof line, "%s %d %ld\n", CLEAR_MAGIC, CLEAR_VER, gen);
   if (n <= 0 || n >= (int)sizeof line)
      return REPLACE_FAILED;
   /* DURABLY, through the same stage-fsync-rename-fsync every record here
    * uses. Its three answers travel unchanged: REPLACE_UNSYNCED means the
    * tombstone is in place and readable and only a power cut in the next
    * moments could lose it, which is not the same as not having recorded the
    * deletion at all. */
   return atomic_replace(cp, line, n);
}

int log_clear_forget(const char *path)
{
   char cp[320];
   if (!clear_path_of(cp, (int)sizeof cp, path))
      return -1;
   /* ENOENT is success: the caller's claim is "no evidence remains", and
    * there being none to start with satisfies it. Nothing is fsynced -- a
    * tombstone that comes back after a power cut is refused by the emptiness
    * check anyway, because by then the log has rows in it again. */
   if (unlink(cp) == 0)
      return 0;
   /* Asked of the TOMBSTONE, not of log_clear_generation: that answers 0 for
    * a non-empty log whatever the tombstone says, and this is called exactly
    * when the log is non-empty -- so trusting it here would report "no
    * evidence remains" over a file still sitting on disk. */
   int fd = open(cp, O_RDONLY, 0);
   if (fd < 0)
      return 0; /* there was none to forget */
   close(fd);
   return -1;
}
