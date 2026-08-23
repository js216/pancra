// SPDX-License-Identifier: GPL-3.0
// synclocal.c --- reading this phone's own logs: which buckets, which rows
// Copyright 2026 Jakob Kastelic
//
/* SPLIT OUT OF app/sync.c, along the seam that file already had.
 *
 * A sync has two sides. One is the WIRE -- signing a request, parsing a
 * digest, deciding what the server is missing. The other is the LOCAL FILE:
 * streaming a log off flash, finding which day-buckets it holds, and copying
 * out the rows of the ones that are wanted. Nothing in here knows there is a
 * server; it takes a struct sync_log and answers questions about a file.
 *
 * THE FILE IS NEVER HELD WHOLE, and that is the property both functions are
 * built around: readings.csv is months of five-minute samples, and a phone
 * that read it into memory to sync it would be a phone that stopped syncing
 * once the history got long. Both stream in chunks and keep only what was
 * asked for.
 *
 * It came out because app/sync.c passed the 2000-line ceiling the build
 * enforces. The extraction moved lines and changed none of them; what it
 * needs from the other side is the shared workspace, declared in
 * app/syncint.h. */
/* SYNC.H BEFORE SYNCINT.H, and syncint.h refuses the other order: it is the
 * seam behind that interface and is written in its vocabulary (the http
 * hook, the key length, the registry size). */
#include "sync.h" // IWYU pragma: keep
#include "syncint.h"
#include <stdint.h>

/* THE SAME SET app/sync.c USES, and errno is the reason to say so: this build
 * is freestanding and gets it from dexlibc.h, not from the host's <errno.h> --
 * which pulls in the glibc headers and fails to compile for Android. */
#include "dexlibc.h"
#include "syncrow.h"

#include <stdio.h>
#include <string.h>

/* Stream a log, keeping only the rows whose bucket is in `want` (or every row
 * when `want` is NULL, which is only used to enumerate buckets). Returns the
 * row count, or -1 when even the window does not fit. A missing file is
 * empty.
 *
 * The file is read in chunks and lines are copied out of the chunk, so the
 * memory held is the WINDOW's rows -- never the file. */
int log_scan(const struct sync_log *l, const int64_t *want, int nwant,
             int *overflow)
{
   if (overflow)
      *overflow = 0;
   /* The SAME discipline log_buckets has, and for the same reason.
    *
    * This function builds the body that gets PUT. An open that failed or a
    * read that stopped early yields FEWER rows, and fewer rows for a bucket
    * the enumeration listed is a smaller bucket -- which the server adopts
    * verbatim, because a PUT replaces. Hardening only the enumeration left
    * this door open: log_buckets would happily report 128 buckets and this
    * function would then scan none of them and hand back an empty body for
    * each. A missing file is still not a failure; everything else is. */
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen     = 0;
   int over     = 0;
   int nrow     = 0;
   int64_t used = 0;
   int64_t n    = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n == 0)
         break; /* end of file: the scan is complete */
      if (n < 0) {
         close(fd); /* a read error mid-file: the body would be short */
         return -1;
      }
      for (int64_t i = 0; i < n; i++) {
         if (chunk[i] != '\n') {
            if (llen < (int)sizeof line - 1)
               line[llen++] = chunk[i];
            else
               over = 1; /* over-long: not a row, and not a truncation either */
            continue;
         }
         int len = llen;
         if (len && line[len - 1] == '\r')
            len--;
         llen = 0;
         /* DAMAGE IS A SCAN FAILURE, NOT A ROW TO SKIP -- see log_buckets,
          * which carries the argument in full. The two readers must agree
          * about what a row is, so they must also agree about what damage is:
          * a row this one dropped while the enumeration kept its bucket makes
          * the PUT body SHORT, and a short body is adopted verbatim by a
          * server that replaces. An EMPTY line is the one thing still
          * skipped, and it is not damage: it carries no row. */
         if (over) {
            close(fd);
            return -1;
         }
         if (len == 0)
            continue;
         if (!row_ok(line, len)) {
            close(fd);
            return -1;
         }
         int64_t b = row_bucket(line, (size_t)len, l->bucketed);
         if (want && !want_has(want, nwant, b))
            continue;
         if (!buf_reserve(used + len + 1) || !row_reserve(nrow + 1) ||
             !sel_reserve(nrow + 1)) {
            close(fd);
            if (overflow)
               *overflow = 1;
            return -1;
         }
         memcpy(g_buf + used, line, (size_t)len);
         g_row[nrow].off = (size_t)used;
         g_row[nrow].len = (size_t)len;
         used += len;
         nrow++;
      }
   }
   /* a final line with no newline -- damaged on the same terms as any other */
   if (over) {
      close(fd);
      return -1;
   }
   if (llen > 0) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (len > 0 && !row_ok(line, len)) {
         close(fd);
         return -1;
      }
      if (len > 0) {
         int64_t b = row_bucket(line, (size_t)len, l->bucketed);
         if (!want || want_has(want, nwant, b)) {
            /* All THREE arrays, and a failure is refused rather than dropped.
             *
             * g_sel is indexed by this same nrow at the selection loops below,
             * and reserving only two of the three here leaves a file whose
             * single row lacks a trailing newline never entering the loop
             * above, so g_sel is still NULL and the selection writes through
             * it; otherwise g_selcap ends one short of nrow and it writes 16
             * bytes past the allocation.
             *
             * SKIPPING the row when a reservation fails, rather than
             * returning -1 with *overflow set as the loop above does, is
             * worse than it looks: a skipped
             * row makes the bucket text short, the short bucket is pushed as
             * authoritative, and the verify agrees because it reads through
             * this same function. Losing a row must fail the sync, not shrink
             * the record. */
            if (!buf_reserve(used + len + 1) || !row_reserve(nrow + 1) ||
                !sel_reserve(nrow + 1)) {
               close(fd);
               if (overflow)
                  *overflow = 1;
               return -1;
            }
            memcpy(g_buf + used, line, (size_t)len);
            g_row[nrow].off = (size_t)used;
            g_row[nrow].len = (size_t)len;
            nrow++; /* no `used +=`: this block is the last writer */
         }
      }
   }
   close(fd);
   return nrow;
}

/* Every bucket the log holds, ascending. Streams the file and keeps only the
 * bucket numbers -- one long per day, so a decade costs 30 kB.
 *
 * Returns the count, or -1 if the log could NOT BE FULLY ENUMERATED, which is
 * the difference between a backup and a deletion. A bucket the server holds
 * and this list omits is pushed as an EMPTY body, which the server implements
 * as DELETE FROM logrow -- that is how a correction or a real deletion
 * travels. So an incomplete list here is not merely inaccurate, it is an
 * instruction to destroy the server's copy.
 *
 * `return 0` on any open() failure makes a permissions problem, a wrong path
 * or an exhausted fd table read as "this log is empty", and the next sync
 * erases the server's entire copy of a lifetime record -- precisely what the
 * server exists to hold. Stopping at `cap` without saying so is the same
 * defect one step in: a log with more buckets than the caller's array deletes
 * its own tail on every sync.
 *
 * A MISSING FILE IS NOT A FAILURE: a log never written has no buckets, and
 * saying so is correct. Every other failure is refused. */
int log_buckets(const struct sync_log *l, int64_t *out, int cap)
{
   int fd = open(l->path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char chunk[16384];
   char line[SYNC_ROW_MAX + 2];
   int llen  = 0;
   int over  = 0;
   int nb    = 0;
   int64_t n = 0;
   for (;;) {
      n = read(fd, chunk, sizeof chunk);
      if (n == 0)
         break; /* end of file: the enumeration is complete */
      if (n < 0) {
         close(fd); /* a read error mid-file: we know less than we think */
         return -1;
      }
      for (int64_t i = 0; i < n; i++) {
         if (chunk[i] != '\n') {
            if (llen < (int)sizeof line - 1)
               line[llen++] = chunk[i];
            else
               over = 1;
            continue;
         }
         int len = llen;
         if (len && line[len - 1] == '\r')
            len--;
         llen = 0;
         /* A ROW THE PARSER REFUSES IS NOT PERMISSION TO DELETE THE REPLICA.
          *
          * SKIPPING an over-long or invalid row and carrying on hands the
          * surviving buckets to a caller that PUTs an empty body -- the
          * server's spelling of DELETE -- to every remote bucket this list
          * does not name. One corrupt line is enough: if the
          * damaged row was the only one left for its day (a day with a single
          * reading, or a day whose other rows had already been corrected
          * away), that day vanished from the list, and the next sync deleted
          * the server's copy of it. The phone still HELD the data -- the
          * bytes were in the file, merely unparseable by this reader -- so
          * what the user lost was the one copy that a later fix could have
          * been reconciled against. Silently, with no undo on either side,
          * for a defect whose visible symptom was nothing at all.
          *
          * The rule is the one this function already applies to a read error
          * below: an incomplete picture must never drive a loop that deletes.
          * Refusing costs one sync cycle and says so loudly every cycle after
          * -- the sync stays broken rather than the backup.
          *
          * AN EMPTY LINE IS NOT DAMAGE. It carries no row, log_scan agrees it
          * contributes nothing, and a file that is nothing but newlines is
          * genuinely empty -- the state the caller must tell apart from this
          * one, because deliberate emptiness has its own evidence
          * (log_clear_generation) and damage has none. */
         if (over) {
            close(fd);
            return -1;
         }
         if (len == 0)
            continue;
         if (!row_ok(line, len)) {
            close(fd);
            return -1;
         }
         int64_t b = row_bucket(line, len, l->bucketed);
         if (want_has(out, nb, b))
            continue;
         if (nb >= cap) { /* one bucket too many: see the header comment */
            close(fd);
            return -1;
         }
         out[nb++] = b;
      }
   }
   /* A final line with no newline, stripped of a trailing \r exactly as the
    * loop above does and exactly as log_scan does.
    *
    * Testing the RAW length here while log_scan strips first is what makes the
    * two disagree: row_ok rejects \r as a control byte, so a log whose last
    * line lacks a newline and ends \r is a row log_scan admits and this
    * function does not.
    * If that row's bucket appeared nowhere else in the file, the bucket was
    * missing from the enumeration, was never pushed, and -- being a bucket the
    * server holds and the list omits -- was DELETED from the server. The two
    * readers must agree on what a row is, or the disagreement is destructive.
    */
   if (over) {
      close(fd);
      return -1;
   }
   if (llen > 0) {
      int len = llen;
      if (len && line[len - 1] == '\r')
         len--;
      if (len > 0 && !row_ok(line, len)) {
         close(fd);
         return -1;
      }
      if (len > 0) {
         int64_t b = row_bucket(line, len, l->bucketed);
         if (!want_has(out, nb, b)) {
            if (nb >= cap) {
               close(fd);
               return -1;
            }
            out[nb++] = b;
         }
      }
   }
   close(fd);
   for (int i = 1; i < nb; i++) { /* ascending, as the server orders */
      int64_t t = out[i];
      int j     = i - 1;
      while (j >= 0 && out[j] > t) {
         out[j + 1] = out[j];
         j--;
      }
      out[j + 1] = t;
   }
   return nb;
}
