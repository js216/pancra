// SPDX-License-Identifier: GPL-3.0
// util.c --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#include "util.h"
#include "dexlibc.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"        /* LOGW: a cleanup that failed says so, once */
#include "thread.h"     /* mutex: one append at a time, see append_lk */
#include <stdatomic.h>  /* the mutation counter is written by two threads */
#include <stdio.h>

/* snprintf returns the would-be length, which can exceed the buffer on
 * truncation; clamp so write() emits only bytes actually in the buffer. */
/* CLAMP `n` INTO A BUFFER OF `cap` BYTES: 0..cap-1, and never below zero.
 *
 * `cap - 1` IS THE TRAP. With cap == 0 this returned -1 -- a negative index
 * handed back by a function whose entire job is to produce a safe one, and
 * every caller uses it to index or to length a write. Callers pass a snprintf
 * return and a sizeof today, so none of them can reach it; the next one is a
 * coin flip, and the failure is a write before the start of the buffer. A
 * capacity of zero has no valid index at all, and 0 is the only answer that
 * cannot corrupt anything. */
int clampn(int n, int cap)
{
   if (cap <= 0)
      return 0;
   if (n < 0)
      return 0;
   return n >= cap ? cap - 1 : n;
}

/* ---- THE BOUNDED BUILDER. See util.h for what it is for. ---- */
void tout_init(struct textout *t, char *buf, int cap)
{
   t->buf = buf;
   t->cap = (cap > 0) ? cap : 0;
   t->len = 0;
   t->bad = (buf && cap > 0) ? 0 : 1;
   if (!t->bad)
      buf[0] = 0;
}

char *tout_room(struct textout *t, int *room)
{
   if (!t || t->bad || !t->buf || t->len >= t->cap - 1) {
      if (room)
         *room = 0;
      /* A FULL BUFFER IS NOT A ROW THAT FITS. The caller stops here, and
       * tout_ok still answers 1 -- there was nothing left to take. Taking a
       * row is what can fail; running out before one is offered is the
       * caller's own loop ending. */
      return 0;
   }
   if (room)
      *room = t->cap - t->len;
   return t->buf + t->len;
}

void tout_took(struct textout *t, int n)
{
   if (!t || t->bad)
      return;
   int room = t->cap - t->len;
   /* n >= room is snprintf saying it TRUNCATED, which is the case the old
    * hand-rolled loops treated as "stop here and write what we have". */
   if (n <= 0 || n >= room) {
      t->bad = 1;
      return;
   }
   t->len += n;
}

int tout_ok(const struct textout *t)
{
   return t && !t->bad;
}

void str_snapshot(char *dst, int cap, const char *src)
{
   /* A NULL source is an empty string -- the callers copy fields that may be
    * absent -- and a NULL destination is nowhere to put one. */
   if (!dst)
      return;
   if (!src) {
      if (cap > 0)
         dst[0] = 0;
      return;
   }
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

/* AND A WAY TO HOLD THE WINDOW OPEN. What this guards is a race whose window
 * is two syscalls wide; a test that merely runs both writers hard
 * relies on luck to hit it, and a test that passes by luck is not a test.
 * When set, this runs at the exact instant the window opens -- after the
 * flush has failed and before the row is taken back -- so the other writer
 * lands in it every round. Test builds only. */
_Thread_local void (*app_fault_gap_here)(void);

/* THE SAME DEVICE FOR THE PUBLISH WINDOW. Runs inside
 * log_replace_with_tail with the append lock HELD -- after the tail is
 * captured, before the rename -- so a test can start an appender that is
 * guaranteed to be inside that window, and then assert what it observes:
 * nothing landing in the file about to be discarded. Test builds only. */
_Thread_local void (*app_fault_publish_gap)(void);

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

/* FINISH A REWRITE: sync the new contents, close, rename it over the live
 * file, and sync the directory -- checking every one of those. `fd` is
 * consumed either way; on any failure the temporary is removed and the
 * original is left untouched.
 *
 * ONE FUNCTION, because the streamed rewrites (the weight and dose logs,
 * which are too big to build in memory) otherwise do the same job by hand and
 * skip most of it: no fsync, an unchecked close, and no directory sync. A
 * rewrite that loses power between rename and sync leaves
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
 * The result is NOT discarded at the call sites: `(void)ftruncate`.
 * A truncate that fails is exactly the case that matters -- the half row is
 * still there, and the caller is about to be told the append merely failed,
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
 * Returns LOG_OK, LOG_FAIL (nothing was written; the file is untouched) or
 * LOG_DAMAGED (a partial row is still in the file). */
/* ONE APPEND AT A TIME, ACROSS ALL FOUR LOGS.
 *
 * The recovery is the reason. When the flush fails, the row is already in the
 * file and has to be taken back -- by truncating what THIS writer wrote,
 * measured from the end as it stands now, because O_APPEND means a remembered
 * offset may not be ours any more. But "the end as it stands now" is only our
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

/* ---- PUBLISHING A REBUILT LOG WITHOUT LOSING AN APPEND ------
 *
 * THE WINDOW THIS CLOSES. A restore rebuilds a log into a staging file and
 * renames it over the original. Between the last byte it copied and the
 * rename, an append can land -- a CGM reading arrives every five minutes and
 * the rebuild takes one GET per missing bucket -- and the rename then deletes
 * a row the app had already acknowledged and drawn.
 *
 * app/syncrestore.c narrowed that to "two syscalls wide" by re-reading the
 * original's tail just before the rename, and said in its own comment that
 * the residual could only be closed by holding this file's append lock across
 * the publish. That is what this is: the tail capture, the fsync, the close
 * and the rename all happen with `append_lk` held, so no log_append can
 * interleave with any of them.
 *
 * WHY IT BELONGS HERE. The lock is this file's and stays this file's -- the
 * alternative is exporting it, which makes "who may hold it, and across
 * what" a question every caller answers for itself. One operation with the
 * rule inside it cannot be got wrong from outside.
 *
 * WHAT IT COSTS: an append made while a restore is publishing waits for a
 * flush and a rename rather than for a flush. Restores are operator-initiated
 * and rare, the wait is milliseconds, and the alternative is losing the row
 * that would have waited.
 *
 * `fd` is the staging file, positioned at its end; `copied` is how many bytes
 * of `path` are already in it. On any failure the staging file is unlinked
 * and `fd` is closed, exactly as replace_finish does -- a caller that gets
 * REPLACE_FAILED has nothing left to clean up. */
enum replace_result log_replace_with_tail(const char *path, int fd,
                                          const char *tmp, long copied)
{
   mutex_lock(&append_lk);
   int ok = 1;
   int in = open(path, O_RDONLY);
   if (in >= 0) {
      if (lseek(in, copied, SEEK_SET) != copied) {
         ok = 0;
      } else {
         char tb[4096];
         for (;;) {
            long n = read(in, tb, sizeof tb);
            if (n < 0) {
               if (errno == EINTR)
                  continue;
               ok = 0;
               break;
            }
            if (n == 0)
               break;
            if (write_all(fd, tb, (int)n) != 0) {
               ok = 0;
               break;
            }
         }
      }
      close(in);
   } else if (errno != ENOENT) {
      /* The original is there and could not be read: publishing now would
       * rename a file that is missing whatever it has grown. */
      ok = 0;
   }
   if (!ok) {
      close(fd);
      (void)unlink(tmp);
      mutex_unlock(&append_lk);
      return REPLACE_FAILED;
   }
#ifdef APP_FAULTS
   /* Still holding append_lk: see app_fault_publish_gap. */
   if (app_fault_publish_gap)
      app_fault_publish_gap();
#endif
   enum replace_result rr = replace_finish(fd, tmp, path);
   mutex_unlock(&append_lk);
   return rr;
}

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
       * already holds it. Take it back, and flush THAT, so the file is left
       * at the length it started.
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
    * directory sync fails the previous file is gone and the new one is the
    * best state available. This is a CREATION, so the honest recovery is to
    * leave
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
 * A SCHEDULER CANNOT ANSWER "is there anything new?" FROM FILE SIZES. Almost
 * every change moves one of the five synced files, so sizes work almost
 * always -- and the exceptions are the ones that matter:
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
 * Stopping the copy of the directory at `cap - 32`, appending the filename on
 * top of the truncated prefix and returning nothing is worse than failing, two
 * ways:
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
   size_t dn = 0;
   size_t nn = 0;
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
   /* THE INCREMENT IS OUT OF THE CONDITION. `*p++ != '\n'` inside a chain of
    * || is a side effect whose timing depends on which earlier test
    * short-circuits -- so whether p moved at all is decided by nd and gen.
    * Nothing downstream reads p, which is why it worked; a reader still has
    * to prove that, and the next edit could make it false. */
   if (nd == 0 || gen <= 0 || *p != '\n')
      return 0;
   p++;
   if ((p - b) != n)
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
   /* THE UNLINK FAILED, AND WHY DECIDES THE ANSWER.
    *
    * ENOENT means there was nothing to forget, which is the caller's claim
    * already satisfied. EVERYTHING ELSE -- EACCES on a directory whose
    * permissions changed, EROFS on a filesystem that went read-only, EIO on
    * a card that is failing -- means the tombstone is STILL THERE, and this
    * is the function whose whole job is to say it is gone.
    *
    * It answered 0 for all of them. The check below re-opened the file and
    * read a failed open as absence, so the two cases that matter most (the
    * file exists and cannot be removed; the file exists and cannot even be
    * opened) both reported success. What follows from that is a sync client
    * refusing to replicate a deliberate emptying for ever, on the strength
    * of stale evidence this function has already told its caller it
    * removed -- and the caller, having been told, has nothing to log.
    *
    * Asked of the TOMBSTONE, not of log_clear_generation: that answers 0 for
    * a non-empty log whatever the tombstone says, and this is called exactly
    * when the log is non-empty -- so trusting it here would report "no
    * evidence remains" over a file still sitting on disk. */
   if (errno == ENOENT)
      return 0;
   int err = errno;
   int fd  = open(cp, O_RDONLY, 0);
   if (fd >= 0) {
      close(fd);
      LOGW("log_clear_forget: %s could NOT be removed (errno %d); the clear "
           "evidence is still there",
           cp, err);
      return -1;
   }
   if (errno == ENOENT)
      return 0; /* it went away between the unlink and the look */
   /* Cannot remove it and cannot look at it: the honest answer is that this
    * does not know whether the evidence remains, and "success" is the one
    * thing it must not say. */
   LOGW("log_clear_forget: %s: unlink failed (errno %d) and it cannot be "
        "opened either (errno %d)",
        cp, err, errno);
   return -1;
}

/* ---- ONE EDITABLE-LOG REWRITE, FOR THE THREE LOGS THAT HAVE ONE -------
 *
 * Weight, food and exercise each had their own copy of this, and each copy
 * carried its own comment saying it was a copy of one of the others. That is
 * the shape of a defect that gets fixed once: the version in weight.c learned
 * that a read failure mid-copy must not be published (the rename would swap
 * in whatever prefix had been copied and lose the rest of the log), that an
 * over-long row must refuse the rewrite rather than be written back
 * truncated, and that a delete which empties the file has to leave a
 * tombstone or the sync client refuses to replicate the emptiness for ever.
 * Three copies means three places for the next such lesson to reach, and the
 * two that do not get it fail in a user's data.
 *
 * WHAT IS SHARED IS THE ALGORITHM AND EVERY DURABILITY RULE. What the caller
 * supplies is the only part that is genuinely its own: which line is the row,
 * and what the replacement row says.
 *
 * TWO PASSES, because the row to edit is the LAST one that matches. The first
 * pass counts; the second copies, altering match number `nmatch` and nothing
 * else. A single pass would have to buffer the file to know which match was
 * the last one, and the file is the thing that must not be held whole. */

/* One line of `path`, without its newline, offered to the caller's matcher. */
static int edit_hit(const struct log_edit *ed, const char *line, int llen)
{
   return ed->matches && ed->matches(line, line + llen, ed->ctx);
}

/* See util.h. app/meterstore.c had this shape and the others did not; this is
 * that reader, moved here so there is one of it. */
enum load_result read_file_exact(const char *path, char *buf, int cap, int *len)
{
   if (len)
      *len = 0;
   if (!path || !*path || !buf || cap < 2)
      return LOAD_ERROR;
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? LOAD_ABSENT : LOAD_ERROR;
   /* INTO A LOCAL, so that a refusal leaves the caller's buffer alone: a
    * loader that is told CORRUPT must not find half a record in the buffer it
    * was going to parse. */
   char tmp[MAX_EXACT_READ];
   int room = cap - 1 < (int)sizeof tmp ? cap - 1 : (int)sizeof tmp;
   int used = 0;
   for (;;) {
      long rn = read(fd, tmp + used, (size_t)(room - used));
      if (rn < 0) {
         if (errno == EINTR)
            continue; /* a signal is not the end of the file */
         close(fd);
         return LOAD_ERROR;
      }
      if (rn == 0)
         break; /* EOF, and this time it really is one */
      used += (int)rn;
      if (used == room)
         break;
   }
   /* THE EOF PROBE. Filling the buffer proves nothing about the file's
    * length, and one byte more is the only way to tell "exactly full" from
    * "longer than anything this build can hold". */
   int extra = 0;
   if (used == room) {
      char one  = 0;
      long more = read(fd, &one, 1);
      while (more < 0 && errno == EINTR)
         more = read(fd, &one, 1);
      extra = more > 0;
   }
   close(fd);
   if (extra)
      return LOAD_CORRUPT;
   if (used == 0)
      return LOAD_CORRUPT; /* created and not written: a torn save */
   for (int i = 0; i < used; i++)
      buf[i] = tmp[i];
   buf[used] = '\0';
   if (len)
      *len = used;
   return LOAD_OK;
}

int log_edit_last(const char *path, const struct log_edit *ed)
{
   if (!path || !*path || !ed || !ed->matches)
      return -1;

   /* pass 1: how many rows match? (the LAST one is the one edited) */
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char buf[1024];
   char line[LOG_EDIT_ROW_MAX];
   int llen   = 0;
   int nmatch = 0;
   long n     = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (llen < (int)sizeof line && edit_hit(ed, line, llen))
               nmatch++;
            llen = 0;
         } else if (llen < (int)sizeof line) {
            line[llen++] = buf[i];
         } else {
            llen = (int)sizeof line; /* over-long: cannot match */
         }
      }
   int read_failed = n < 0;
   close(fd);
   /* A COUNT TAKEN FROM A FAILED READ IS NOT A COUNT. Reported as zero
    * matches it reads as "no such row", which is a different answer from "the
    * log could not be read" and sends the caller to the wrong conclusion. */
   if (read_failed || nmatch == 0)
      return -1;

   /* pass 2: copy, altering only match #nmatch */
   char tmp[LOG_EDIT_PATH_MAX];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return -1;
   fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (out < 0) {
      close(fd);
      return -1;
   }
   int seen = 0;
   int ok   = 1;
   /* HOW MANY BYTES THE REWRITE KEPT. Zero means this delete removed the last
    * line in the file -- see the tombstone at the bottom. */
   long kept = 0;
   llen      = 0;
   /* `ok` IS CLEARED BY A READ FAILURE TOO, not only by a write one. A read
    * error returns -1, which fails this loop's `> 0` test while leaving ok
    * == 1 -- so the rename below would swap in whatever prefix was copied and
    * the rest of the log would be gone. The rewrite-and-rename is what makes
    * these files crash-safe; it must not also be what truncates them. */
   while (ok && (n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; ok && i < n; i++) {
         if (buf[i] != '\n') {
            /* -1: the newline below is appended UNCONDITIONALLY, so the last
             * byte of `line` belongs to it. */
            if (llen < (int)sizeof line - 1)
               line[llen++] = buf[i];
            else
               ok = 0; /* over-long row: refuse to rewrite blind */
            continue;
         }
         int hit = edit_hit(ed, line, llen) && ++seen == nmatch;
         if (hit && !ed->format) {
            llen = 0;
            continue; /* a delete: the row is simply not copied */
         }
         if (hit) {
            int w = ed->format(line, (int)sizeof line - 1, ed->ctx);
            if (w < 0 || w >= (int)sizeof line) {
               ok = 0;
               continue;
            }
            llen = w;
         }
         line[llen++] = '\n';
         if (write(out, line, (size_t)llen) != llen)
            ok = 0;
         else
            kept += llen;
         llen = 0;
      }
   if (n < 0)
      ok = 0; /* the read failed mid-file: see the loop above */
   /* A FINAL LINE WITH NO TRAILING NEWLINE gets the same treatment. The
    * appender always terminates rows; the file is user-copyable. */
   if (ok && llen > 0 && llen < (int)sizeof line) {
      int hit = edit_hit(ed, line, llen) && ++seen == nmatch;
      if (!(hit && !ed->format)) {
         if (hit) {
            int w = ed->format(line, (int)sizeof line - 1, ed->ctx);
            if (w < 0 || w >= (int)sizeof line)
               ok = 0;
            else
               llen = w;
         }
         if (ok) {
            line[llen++] = '\n';
            if (write(out, line, (size_t)llen) != llen)
               ok = 0;
            else
               kept += llen;
         }
      }
   }
   close(fd);
   /* DURABLY, OR NOT AT ALL.
    *
    * A close(out) that goes unchecked before the rename -- no fsync of the
    * new contents, no check that close flushed them, no sync of the directory
    * the rename lands in -- leaves the log EMPTY after a power loss: the
    * rename is visible while the bytes it points at never reached the disk,
    * and the user's whole history is a file of nothing. */
   if (!ok) {
      close(out);
      (void)unlink(tmp);
      return -1;
   }
   /* Only a rename that never happened is a failure here: past it the log
    * file already holds the rewritten rows, and reporting failure would leave
    * the caller's in-memory tail disagreeing with the file it just wrote. */
   if (replace_finish(out, tmp, path) == REPLACE_FAILED)
      return -1;
   /* THE OTHER HALF OF A DELETE THAT REMOVED THE LAST LINE.
    *
    * These logs are synced, and the phone is authoritative over the server's
    * copy -- so an empty log is an instruction to delete the replica. The
    * sync client refuses that instruction unless it can tell a deliberate
    * emptying from a phone that lost its storage, and it cannot tell by
    * looking: both are a log with no rows. It would therefore refuse for
    * ever, with the user's deletion never reaching the server and every other
    * log's sync stopped behind it.
    *
    * Only the code that did the emptying knows it was meant, so it is this
    * code that says so. `kept == 0` is exactly that case and nothing else. In
    * practice a '#' header keeps these files non-empty, so the tombstone is
    * minted for a header-less log (an older build's, or one copied in by
    * hand) -- which is precisely the log whose last delete would otherwise
    * wedge the sync silently.
    *
    * The reverse is stated too, because a rewrite that still keeps lines is
    * proof the log is NOT empty, and evidence of emptiness must not outlive
    * the emptiness it describes. */
   if (kept == 0)
      (void)log_note_cleared(path);
   else
      (void)log_clear_forget(path);
   /* AN EDIT OR A DELETE IS EXACTLY THE MUTATION FILE SIZES CAN MISS: a
    * corrected value of the same length leaves the log byte-for-byte the
    * same length, so the sync watcher sees nothing to send. */
   record_mutated();
   return 0;
}
