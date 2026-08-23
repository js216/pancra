// SPDX-License-Identifier: GPL-3.0
// syncrestore.c --- pulling the record back from the server
// Copyright 2026 Jakob Kastelic

/* RESTORE: the direction that exists for the phone that lost everything.
 * It writes to this phone's own logs, which is why every bound in here is
 * checked twice and why nothing is published except by rename.
 *
 * ONE OF THE WORKFLOW FILES BEHIND app/sync.h. The interface has not moved
 * and neither has anything a caller can see: app/sync.c is now a coordinator
 * that owns the configuration, the one-operation-at-a-time lock and the
 * workspace, and hands a SNAPSHOT of all three (struct sync_ctx, see
 * app/syncint.h) to whichever workflow file does the work.
 *
 * NOTHING HERE READS A CONFIGURATION GLOBAL -- it cannot, they are static in
 * the coordinator. That is what the split buys and it is not cosmetic:
 * signing that reads the LIVE key and uid while the operation around it
 * works from a snapshot lets a pairing completed mid-sync sign the remainder
 * of that sync with the new account's key. */
#include "dexlibc.h"
#include "sync.h"
#include "syncint.h" /* the workspace, and the operation's own context */
#include "syncrow.h" /* struct row, row_bucket/hash16: what a row IS */
#include "util.h"    /* log_replace_with_tail: staged, then published */
#if __STDC_HOSTED__
#include <errno.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* getenv: the fault switches, host only */
#include <string.h>

#ifdef APP_FAULTS
/* Append APP_RESTORE_APPEND's text to `path`, as a reading arriving
 * mid-restore would. Only the FIRST call does it, so one armed variable
 * stands for one concurrent append rather than one per log. */
static void restore_append_fault(const char *path)
{
   static int done;
   const char *s = getenv("APP_RESTORE_APPEND");
   if (done || !s || !*s)
      return;
   done   = 1;
   int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return;
   (void)write_all(fd, s, (int)s_len(s));
   close(fd);
}
#else
#define restore_append_fault(p) ((void)(p))
#endif

/* The canonical hash of ONE local bucket, into out[17]. 0, or -1 when the log
 * could not be read whole -- which is not "the bucket is empty" and must not
 * be treated as a hash that merely differs (see the caller). */
static int local_bucket_hash(const struct sync_log *l, int64_t b, char out[17])
{
   static char text[SYNC_BUF_MAX];
   int64_t one = b;
   int nrow    = log_scan(l, &one, 1, NULL);
   if (nrow < 0)
      return -1;
   /* log_scan already selected exactly this bucket, so no further filter --
    * the same call sync_bucket_text makes, and the same bytes the server
    * hashed. */
   int64_t tl = bucket_text(g_row, nrow, 0, b, l->bucketed, text, sizeof text);
   if (tl < 0)
      return -1;
   hash16(text, tl, out);
   return 0;
}

static int copy_into(int fd, const char *path, int64_t *copied)
{
   if (copied)
      *copied = 0;
   int in = open(path, O_RDONLY);
   if (in < 0)
      return (errno == ENOENT) ? 0 : -1;
   static char buf[4096];
   for (;;) {
      int64_t n = read(in, buf, sizeof buf);
      if (n < 0) {
         /* Same rule as write_all's: a read cut short by a signal has moved
          * nothing and is not a read error. Treated as one, a restore would
          * abandon a perfectly good copy. */
         if (errno == EINTR)
            continue;
         close(in);
         return -1;
      }
      if (n == 0)
         break;
      if (write_all(fd, buf, (int)n) != 0) {
         close(in);
         return -1;
      }
      if (copied)
         *copied += n;
   }
   /* The result of close() on a READ handle carries no data, so unlike the
    * staged file's close (replace_finish's job) there is nothing to check. */
   close(in);
   return 0;
}

/* The body, so the many early returns inside it do not each have to remember
 * to release the operation lock -- and so none of them RETURNS while holding
 * it, which test/app/lockorder.py is right to refuse. */
int sync_restore_inner(const struct sync_ctx *sx)
{
   if (!sx->have_key || !sx->http)
      return -1;
   int restored = 0;
   int unsynced = 0;
   /* THE SNAPSHOT, not the live registry -- and here it is load-bearing in a
    * way it was not when this only appended. A re-registration REPLACES
    * sx->log[] wholesale (sync_set_logs), so a path read from the live array
    * while that happens is a TORN path, and the publish below RENAMES over
    * whatever it names. Every other loop in this file has the same shape and
    * the same reason (see sync_ctx_begin); this one is the one that
    * writes. */
   for (int li = 0; li < sx->nlog; li++) {
      const struct sync_log *l = &sx->log[li];
      char path[128];
      if (!sync_path_digest(path, sizeof path, l->name))
         return -1;
      if (signed_req(sx, "GET", path, "", 0, sx->rsp, SYNC_BUF_MAX) != 200)
         return -1;
      digest_fault(sx->rsp);

      /* What the server has. */
      int64_t *rb    = sync_rb();
      char (*rh)[17] = sync_rh();
      if (!rb || !rh)
         return -1;
      int nrb       = 0;
      const char *q = sx->rsp;
      char nm[40];
      int64_t cnt = 0;
      char hh[17];
      /* THE WHOLE REPLY, BEFORE ANY LOCAL FILE IS TOUCHED. This loop runs to
       * completion above every write below, so a reply that is damaged
       * anywhere refuses the restore for this log outright rather than
       * pulling back the prefix it managed to read and reporting a count. */
      for (;;) {
         enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
         if (d == DLINE_END)
            break;
         if (d == DLINE_BAD)
            return -1;
         if (nrb >= SYNC_REMOTE_BUCKETS)
            return -1;
         int64_t b = 0;
         if (!digest_num(nm, nm + s_len(nm), &b))
            return -1;
         /* THE HASH IS KEPT, and it is the whole of what makes a repair
          * possible. Parsing the per-bucket hash and dropping it leaves the
          * restore able to ask
          * only "does this bucket number exist locally?" -- and a bucket
          * number is not evidence that the bucket is COMPLETE. */
         rb[nrb] = b;
         memcpy(rh[nrb], hh, 17);
         nrb++;
      }

      /* What we have. A read error here must not read as "we have nothing":
       * that would pull the whole record back on top of a log we simply could
       * not open, duplicating every row. */
      int64_t *lb = sync_lb();
      if (!lb)
         return -1;
      int nlb = log_buckets(l, lb, SYNC_LOCAL_BUCKETS);
      if (nlb < 0)
         return -1;

      /* WHICH REMOTE BUCKETS THIS PHONE IS SHORT OF -- and "short of" is
       * decided by the CANONICAL HASH, not by whether the bucket number
       * happens to appear locally.
       *
       * WHAT A NUMBER COMPARISON MISSES. Under `lb[k] == rb[r]` a remote
       * bucket is
       * skipped the moment a bucket of that number exists here, however few
       * rows it holds. A TORN bucket -- the right day, with rows missing
       * because a write was cut short, a copy was interrupted, or an older
       * restore stopped halfway -- can then never be repaired: the one
       * signal that would show the damage is the hash the server just sent.
       *
       * And a torn bucket is not a stable state. It is exactly the local
       * state that the NEXT sync PUTs over the server's complete copy, the
       * phone being authoritative: the rows missing here get deleted there,
       * and the only copy that still had them is gone. A restore that
       * "succeeds" while leaving a short bucket has armed the loss.
       *
       * A hash that differs means the two sides hold different SETS. It does
       * not say which way, and does not have to: the merge below only ever
       * adds, so a bucket where this phone holds MORE costs one fetch that
       * contributes nothing. That is the right side to be wrong on.
       *
       * ONE PASS OVER THE LOG PER SHARED BUCKET. Restore is a one-shot that
       * already spends an HTTP round trip per bucket it fetches; the periodic
       * sync path is untouched and still walks its buckets in windows. */
      unsigned char *need = sync_need();
      if (!need)
         return -1;
      int missing = 0;
      for (int r = 0; r < nrb; r++) {
         char mine[17];
         if (!want_has(lb, nlb, rb[r]))
            need[r] = 1; /* not here at all: the case that always worked */
         else if (local_bucket_hash(l, rb[r], mine) != 0)
            return -1; /* cannot tell what we hold: not a restore, a refusal */
         else
            need[r] = strncmp(mine, rh[r], 16) != 0;
         if (need[r])
            missing = 1;
      }
      /* Asked before anything is staged, so a log that is already complete is
       * not rewritten at all -- a rewrite that changes nothing still spends
       * the flash write, and on this hardware that is the cost worth
       * avoiding. */
      if (!missing)
         continue;

      /* ---- STAGED, THEN PUBLISHED ONCE --------------------------------
       *
       * Opening the live log O_APPEND and writing each bucket straight into
       * it opens three holes, and the middle one is the worst:
       *
       *   - a short write tears a row IN THE LIVE FILE, and the refusal that
       *     follows cannot undo it -- every later load of that log reads the
       *     truncated row as damage, for good, because these logs are
       *     append-only and never rewritten;
       *   - close() can fail (the buffered write reaching a full or dying
       *     card is reported HERE, not by write), so a dropped result means a
       *     bucket reported restored having never landed;
       *   - without an fsync, a power loss within the writeback window loses
       *     rows the user has been told were restored.
       *
       * So: the live log is copied into a staging file, every missing bucket
       * is appended to THAT, and the result is renamed over the original only
       * once all of it is written. A failure anywhere leaves the original
       * untouched, byte for byte. */
      /* ".restore", NOT ".new" -- and the difference is a corruption bug.
       *
       * log_append() in app/util.c stages the FIRST record of a log in
       * "<path>.new" (the header and the row go in together, atomically), and
       * every log registered here is one of the four it appends to. A reading
       * arriving from the CGM while this runs would then open, TRUNCATE and
       * rename the very file this restore is building -- and log_append takes
       * that path exactly when the log is missing or empty, which is the state
       * a restore happens in. The two writers would trade one inode: one of
       * them renames it into place while the other is still writing at its own
       * offset, and the live log ends up holding both writers' bytes on top of
       * each other, with this function reporting the failure its rename got
       * and the original "untouched" promise broken.
       *
       * A suffix of its own costs nothing and cannot collide with either
       * log_append's ".new" or atomic_replace's ".tmp". The buffer is sized
       * for the longest path sync_set_logs accepts plus the suffix, so a
       * deep data directory refuses nothing. */
      char tmp[320];
      int tn = snprintf(tmp, sizeof tmp, "%s.restore", l->path);
      if (tn < 0 || tn >= (int)sizeof tmp)
         return -1; /* a truncated staging path would replace the wrong file */
      int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
         return -1;
      int64_t copied = 0;
      if (copy_into(fd, l->path, &copied) != 0) {
         close(fd);
         unlink(tmp);
         return -1;
      }
      int added = 0;
      int ok    = 1;
      for (int r = 0; r < nrb && ok; r++) {
         if (!need[r])
            continue;
         char p2[128];
         if (!sync_path_bucket(p2, sizeof p2, l->name, rb[r])) {
            ok = 0;
            continue;
         }
         if (signed_req(sx, "GET", p2, "", 0, sx->rsp, SYNC_BUF_MAX) != 200) {
            ok = 0;
            continue;
         }
         restore_body_fault(sx->rsp);
         int64_t n = s_len(sx->rsp);
         if (n <= 0)
            continue; /* the server holds the bucket but it is empty */
         /* A REPLY THAT FILLED THE BUFFER IS THE FRONT OF A BUCKET, NOT A
          * BUCKET. The wire allows a body of WV_LIMIT_BODY_MAX (512 kB) and
          * this phone holds SYNC_BUF_MAX (256 kB), so the server may legally
          * hold a bucket that does not fit here -- and lib/wirevec.h's rule for
          * exactly that case is that the smaller implementation DECLINES, never
          * truncates. A body cut off at the buffer's last byte ends mid-row,
          * and writing it into the staging file publishes a torn row as a
          * successful restore, which is the one outcome this whole path was
          * rewritten to make impossible. The production transport already
          * refuses an oversized reply (see jni_http), but the refusal belongs
          * where the bytes are written too: a transport is a hook, and the
          * interop harness's own socket truncates. */
         if (n >= SYNC_BUF_MAX - 1) {
            ok = 0;
            continue;
         }
         /* WHAT ARRIVED IS WHAT THE DIGEST PROMISED, or it is not used.
          *
          * The whole decision above rests on the hash the server sent in its
          * digest; fetching the body and merging it unchecked would move the
          * trust from that hash to the wire, which is where it is least
          * deserved. A body that lost bytes in transit, arrived from the
          * wrong bucket, or was rewritten by anything between here and the
          * server hashes differently -- and the rows this restore is about to
          * write into the user's log are exactly the rows nobody else still
          * has. The digest parser answers the same way: damage is refused,
          * never interpreted.
          *
          * Refusing here fails the whole restore (ok = 0), which is the
          * standing rule of this function: all of it or none of it. */
         char got[17];
         hash16(sx->rsp, n, got);
         if (strncmp(got, rh[r], 16) != 0) {
            ok = 0;
            continue;
         }
         /* MERGE, WHICH IS A UNION AND NEVER A REPLACEMENT.
          *
          * The staging file already holds this phone's copy of the log,
          * byte for byte, so every local row is in it before this line runs.
          * Only the rows the server sent that this phone does NOT hold are
          * appended -- so a row BOTH sides have is written once (a duplicate
          * line is real bytes on the user's disk, and slots.csv is read by a
          * loader that does not deduplicate); a row ONLY THE SERVER has is
          * the repair; and a row ONLY THIS PHONE has is untouched, which is
          * the direction most likely to go wrong -- a merge that REPLACED the
          * bucket would delete readings the phone had and the server had not
          * been told about -- the very loss a restore exists to prevent,
          * arriving through the repair.
          *
          * The kept lines are compacted to the front of sx->rsp and written in
          * ONE call, both because it is fewer syscalls and because the
          * per-bucket failure cases below are counted in write_all calls. */
         int64_t one = rb[r];
         int nrow    = log_scan(l, &one, 1, NULL);
         if (nrow < 0) {
            ok = 0; /* cannot tell what we hold: appending blind duplicates */
            continue;
         }
         int64_t keep  = 0;
         const char *s = sx->rsp;
         while (*s) {
            const char *e = s_chr(s, '\n');
            int64_t ln    = e ? (int64_t)(e - s) : s_len(s);
            int held      = 0;
            for (int i = 0; i < nrow && !held; i++)
               held = g_row[i].len == (size_t)ln &&
                      memcmp(g_buf + g_row[i].off, s, (size_t)ln) == 0;
            if (!held) {
               /* MEMMOVE, NOT MEMCPY: `keep` only ever trails `s`, so the
                * ranges are disjoint or dst-below-src -- but they DO touch
                * (with nothing dropped yet the two are the same address), and
                * "may overlap" is the case memcpy's contract calls undefined
                * rather than merely unlikely. */
               memmove(sx->rsp + keep, s, (size_t)ln);
               keep += ln;
               sx->rsp[keep++] = '\n';
            }
            s = e ? e + 1 : s + ln;
         }
         if (keep == 0)
            continue; /* the phone already held every row of it */
         if (write_all(fd, sx->rsp, (int)keep) != 0) {
            ok = 0;
            continue;
         }
         added++;
      }
      /* ALL OF IT OR NONE OF IT. A bucket that could not be fetched or
       * written is one this restore promised and does not have, and
       * publishing the staged file anyway would report a restore that is
       * short by exactly the rows the user asked for. The original is
       * untouched, so the answer is simply "no". */
      if (!ok) {
         close(fd);
         unlink(tmp);
         return -1;
      }
      /* AND NOTHING TO PUBLISH IS NOT SOMETHING TO PUBLISH. A bucket can
       * differ from the server's because this phone holds MORE of it, not
       * less -- readings logged since the last push are the ordinary case --
       * and the union then adds nothing. Renaming a byte-identical copy over
       * the log would spend a flash write, take the file's identity with it,
       * and hand every later restore the same non-work to do again. */
      if (added == 0) {
         close(fd);
         unlink(tmp);
         continue;
      }
      /* A ROW LANDING RIGHT HERE is the case the catch-up below exists for,
       * and it cannot be scheduled from a single-threaded test: the append has
       * to happen after the snapshot and before the publish. APP_RESTORE_APPEND
       * puts one there on demand, so the catch-up is exercised rather than
       * argued for. Compiled out of the phone build entirely. */
      restore_append_fault(l->path);

      /* ---- WHATEVER ARRIVED WHILE WE WERE DOWNLOADING ------------------
       *
       * A REGRESSION THE STAGED DESIGN INTRODUCES, and it has to be paid for
       * here. The copy above is a snapshot; between it and the rename below
       * there is one GET per missing bucket, which for a year of history is
       * hundreds of round trips and minutes of wall clock. A CGM reading lands
       * every five minutes. Any row appended in that window is in the original
       * and NOT in the staging file, and the rename would delete it -- a
       * reading the app already acknowledged as stored and drew on screen.
       *
       * An append-in-place restore cannot lose a row that way, so
       * shipping the staged version without this would be trading a durability
       * bug for a data-loss one.
       *
       * So the original's tail is re-read from where the copy stopped and
       * appended. `copied` is a byte offset into an append-only file, which is
       * exactly what makes this sound: bytes before it never change.
       *
       * AND THE LAST TWO SYSCALLS ARE COVERED TOO. Doing the catch-up here and
       * then renaming leaves a row appended between the two lost -- a window
       * "two syscalls wide rather than minutes". Narrow is not closed: an
       * append lands every five minutes for years, and the row it loses is a
       * reading the app already acknowledged. log_replace_with_tail does the
       * catch-up, the fsync and
       * the rename with the append lock held, so there is no instant at which
       * an append can be accepted into a file about to be discarded. The lock
       * stays private to util.c; see util.h for why the OPERATION is what is
       * exported rather than the lock. */
      enum replace_result rr =
          log_replace_with_tail(l->path, fd, tmp, (long)copied);
      if (rr == REPLACE_FAILED)
         return -1; /* it unlinked the staging file itself; see util.c */
      if (rr == REPLACE_UNSYNCED)
         unsynced = 1; /* published, durability unknown -- see below */
      restored += added;
   }
   /* CHANGED-BUT-UNCERTAIN IS ITS OWN ANSWER. The rows are in the file and
    * visible to the next load, but a post-rename fsync failed, so a power
    * loss now could still lose them. Reporting plain success would tell the
    * user their record is back when it may not be after a reboot; reporting
    * -1 would tell them nothing was restored when in fact everything was,
    * and invite them to run it again. */
   if (unsynced)
      return SYNC_RESTORE_UNSYNCED;
   return restored;
}
