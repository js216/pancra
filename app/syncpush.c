// SPDX-License-Identifier: GPL-3.0
// syncpush.c --- what the server is missing, and putting it there
// Copyright 2026 Jakob Kastelic

/* THE PUSH HALF OF A SYNC: read a log's digest, work out which buckets the
 * server lacks or holds wrongly, and PUT them. It also holds the digest
 * line parser, which both halves of the protocol read replies with.
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
#include "sync.h"
#include "syncint.h"   /* the workspace, and the operation's own context */
#include "syncrow.h"   /* struct row, row_bucket/hash16: what a row IS */
#include "util.h"      /* write_all, and the clear-generation markers */
#include <stdatomic.h> /* the progress counter crosses a thread */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* getenv: the fault switches, host builds only */
#include <string.h>

/* ---- the sync algorithm ----------------------------------------------- */

/* ---- ONE LINE OF A DIGEST REPLY ---------------------------------------
 *
 * THREE ANSWERS, BECAUSE THERE ARE THREE SITUATIONS. This returned 1 for a
 * row and 0 for everything else -- and "everything else" was the end of the
 * reply AND every possible syntax failure, reported identically.
 *
 * Both callers loop `while (digest_line(...))` and then treat what they
 * collected as the server's complete bucket list. So a reply that was cut in
 * half by a dropped connection, or whose tenth line was corrupt, stopped the
 * loop and was accepted as the whole truth about what the server holds. What
 * follows from that is not a failed sync:
 *
 *   - in sync_one_log, a bucket the server DOES hold but whose line was never
 *     parsed looks like one the server does not have, so the phone pushes it
 *     again -- harmless -- while a bucket listed after the damage that the
 *     phone has since deleted is never deleted server-side;
 *   - in sync_restore, the missing lines are buckets the phone will not pull
 *     back, and the restore then reports SUCCESS with a count. The user asked
 *     for their record and was told it came back, short.
 *
 * Framing is checked as strictly as the fields, because a truncated reply's
 * last line is usually well-formed for as far as it goes: "20000 3 abc" with
 * no newline is a plausible row and a certain sign that the rest is missing. */

/* Digits only, non-empty, and not more than a long can hold. The bucket is a
 * day number and the count a row count; a run long enough to overflow is not a
 * big number, it is a different string. */

int digest_num(const char *s, const char *e, int64_t *out)
{
   if (s >= e)
      return 0; /* an empty field is not a zero */
   int64_t v = 0;
   int nd    = 0;
   for (const char *r = s; r < e; r++) {
      if (*r < '0' || *r > '9')
         return 0;
      if (++nd > 18)
         return 0; /* eighteen digits is the widest that cannot overflow */
      v = (v * 10) + (*r - '0');
   }
   *out = v;
   return 1;
}

/* CORRUPT THE DIGEST WE JUST FETCHED, on demand.
 *
 * A real server never sends a malformed digest, so the branch that REFUSES one
 * -- the branch this whole item is about -- could be argued for but not run.
 * Every parser case can be tested by calling digest_line directly; what cannot
 * is the caller's response to DLINE_BAD, which is where the damage either
 * stops a restore or is quietly read as the end of the list.
 *
 * The corruption is truncation at the first newline, because that is what a
 * dropped connection actually leaves: a valid prefix and a last line that
 * looks complete until its newline is asked for.
 *
 * TRUNCATION AND NOTHING ELSE, deliberately. Replace the newline with a space
 * as well and the fault is quietly weakened: the surviving line ends in a
 * space, so the parser refuses it for its HASH WIDTH (seventeen bytes) and the
 * rule this fault exists to drive from a CALLER -- every row ends in a
 * newline, including the last -- is never reached. Removing the framing check
 * altogether would have left every gate green. So the bytes that survive are
 * exactly the bytes that arrived. */
#ifdef APP_FAULTS

void digest_fault(char *buf)
{
   const char *s = getenv("APP_FAIL_DIGEST");
   if (!s || !*s || *s == '0')
      return;
   char *r = strchr(buf, '\n');
   if (r)
      *r = '\0'; /* the first line's bytes, without its terminator */
}

/* A FETCHED BUCKET THAT IS NOT WHAT THE DIGEST PROMISED. The restore decides
 * which buckets it is short of from the hashes in the digest, then downloads
 * them -- and the rows it is about to write into the user's log are exactly
 * the rows nobody else still has. A body that lost bytes on the way, or came
 * from the wrong bucket, or was rewritten in between, is the one input the
 * check after this must refuse.
 *
 * A REAL SERVER NEVER SENDS ONE, which is precisely why the refusal needs a
 * fault to drive it: without this the check could be argued for but never
 * run, and deleting it would leave every gate green. One byte is changed, so
 * what differs is the HASH and nothing else -- the body is still a
 * well-formed bucket, so a check that merely looked at its shape would still
 * pass it. */
void restore_body_fault(char *buf)
{
   const char *s = getenv("APP_FAIL_BUCKET");
   if (!s || !*s || *s == '0')
      return;
   if (*buf)
      *buf = (*buf == 'z') ? 'y' : 'z';
}
#else
/* REAL FUNCTIONS, NOT MACROS, because the two callers are in another file
 * now (app/syncrestore.c) and a macro is only a no-op in the translation unit
 * that can see it. Empty, so an ordinary build pays a call the optimiser
 * removes and nothing else. */
void digest_fault(char *buf)
{
   (void)buf;
}

void restore_body_fault(char *buf)
{
   (void)buf;
}
#endif

enum dline digest_line(const char **p, char *name, int ncap, int64_t *count,
                       char hash[17])
{
   const char *q = *p;
   if (!*q)
      return DLINE_END;

   /* FIELD ONE: the name, up to the first space. */
   const char *ns = q;
   while (*q && *q != ' ' && *q != '\n')
      q++;
   if (*q != ' ' || q == ns)
      return DLINE_BAD; /* no separator, or an empty name */
   if (q - ns > ncap - 1)
      return DLINE_BAD; /* it does not fit, so we cannot say what it was */
   int k = (int)(q - ns);
   memcpy(name, ns, (size_t)k);
   name[k] = '\0';
   q++;

   /* FIELD TWO: the count, digits only. */
   const char *cs = q;
   while (*q && *q != ' ' && *q != '\n')
      q++;
   if (*q != ' ')
      return DLINE_BAD;
   if (!digest_num(cs, q, count))
      return DLINE_BAD;
   q++;

   /* FIELD THREE: the hash, EXACTLY sixteen bytes. Taking up to sixteen and
    * then skipping whatever else is on the line pads a short hash with the
    * NUL and truncates a long one -- either way it compares equal to nothing
    * and unequal to everything, which reads as "this bucket differs" and
    * pushes it again for ever. */
   const char *hs = q;
   while (*q && *q != '\n')
      q++;
   if (q - hs != 16)
      return DLINE_BAD;
   memcpy(hash, hs, 16);
   hash[16] = '\0';

   /* FRAMING. Every row ends in a newline, including the last: a reply whose
    * final line has none was cut off, and its last row is exactly the row most
    * likely to look complete. */
   if (*q != '\n')
      return DLINE_BAD;
   q++;
   *p = q;
   return DLINE_ROW;
}

/* (push_bucket is gone: the window loop builds a bucket's canonical text and
 * PUTs it in place, so there is nothing left to hand to a helper.) */

/* Every bucket this log has locally, with its hash, compared against what the
 * server reports. Buckets the server holds and we do not are pushed EMPTY,
 * which deletes them -- the app is authoritative, so "we no longer have it"
 * has to mean "nor do you". */
int sync_one_log(const struct sync_ctx *sx, int li)
{
   const struct sync_log *l = &sx->log[li];
   char path[128];
   if (!sync_path_digest(path, sizeof path, l->name))
      return -1;
   if (signed_req(sx, "GET", path, "", 0, sx->rsp, SYNC_BUF_MAX) != 200)
      return -1;
   /* THE SAME FAULT THE RESTORE PATH ARMS, and it belongs here even more than
    * there: the loop at the bottom of this function PUTs an empty body to
    * every remote bucket the local enumeration did not find, which is the only
    * data-destroying loop in this file. A digest read as shorter than it is
    * makes buckets the server holds look absent -- so the failure mode of
    * reading damage as the end of the list is, here, "delete what you could
    * not read about".
    *
    * Without this the refusal below could be argued for but not run: a real
    * server never sends a malformed digest, and the parser cases in
    * test/app/interoptest.c prove what digest_line ANSWERS, not what this
    * caller does with the answer. Compiled out entirely unless APP_FAULTS is
    * defined, which nothing that ships defines. */
   digest_fault(sx->rsp);

   /* Remote buckets, so we can spot ones we no longer hold. */
   int64_t *rb    = sync_rb();
   char (*rh)[17] = sync_rh();
   if (!rb || !rh)
      return -1; /* no room to hold what the server has: see syncint.h */
   int nrb       = 0;
   const char *q = sx->rsp;
   char nm[40];
   int64_t cnt = 0;
   char hh[17];
   /* THE REMOTE LIST HAS A CEILING, AND CROSSING IT IS NOT SILENT.
    *
    * Past rb[]'s size this loop simply stopped parsing, so every bucket
    * beyond it had no `theirs` to compare against and was re-pushed on EVERY
    * sync, forever -- megabytes a run, as often as once a minute, with
    * nothing to say why. Both ceilings are named in sync.h now (~11 and ~22
    * years of daily buckets) and asserted against the wire's own
    * LOG_BUCKETS, rather than each being whatever a sizeof at one call site
    * happened to say.
    *
    * Refusing is the same choice log_buckets already makes for the local
    * side, and for the same reason: an incomplete picture of what the server
    * holds drives a loop that deletes. */
   for (;;) {
      enum dline d = digest_line(&q, nm, sizeof nm, &cnt, hh);
      if (d == DLINE_END)
         break;
      /* MALFORMED IS NOT FINISHED. Read as the end of the list, a damaged
       * reply makes buckets the server holds look absent -- and the loop this
       * feeds DELETES on that basis. */
      if (d == DLINE_BAD)
         return -1;
      if (nrb >= SYNC_REMOTE_BUCKETS)
         return -1;
      /* The bucket is validated as a number here, not scanned for a digit
       * prefix, under which "20000x" parses as 20000 and names a real day. */
      int64_t b = 0;
      if (!digest_num(nm, nm + s_len(nm), &b))
         return -1;
      rb[nrb] = b;
      memcpy(rh[nrb], hh, 17);
      nrb++;
   }

   /* Local buckets: enumerated by streaming, so this costs one pass and one
    * long per day rather than the whole log in memory. */
   int64_t *lb = sync_lb();
   if (!lb)
      return -1;
   int nlb = log_buckets(l, lb, SYNC_LOCAL_BUCKETS);
   /* REFUSE TO SYNC A LOG WE CANNOT READ WHOLE. Below, every remote bucket
    * missing from lb[] is PUT empty, which deletes it on the server. If this
    * list is short for any reason other than the log genuinely not holding
    * those buckets, that loop is a data-destroying loop. Failing the sync
    * costs one cycle; getting this wrong costs the backup. */
   if (nlb < 0)
      return -1;

   /* AND A LOG WE NO LONGER HAVE AT ALL DOES NOT GET TO EMPTY THE SERVER.
    *
    * `nlb == 0` is the honest answer for a log that was never written, and
    * refusing a read error (above) does not cover it: a MISSING file is not
    * an error, it is zero buckets. But zero local buckets against a server
    * that holds many is not a correction the user made, it is a phone that
    * lost its storage -- a reinstall, a cleared app, a restored handset with
    * allowBackup="false". The deletion loop at the bottom would then walk
    * every bucket the server has and PUT each one empty, erasing years of
    * readings in a few hundred requests, silently, with no undo on either
    * side.
    *
    * The app is authoritative about CHANGES it made, not about a record it
    * has simply forgotten. So this refuses, loudly and every cycle, until
    * either the log comes back or the record is pulled down (sync.h's restore
    * direction) -- the sync stays broken rather than the backup.
    *
    * UNLESS THE USER REALLY DID DELETE IT ALL, and the deletion workflow said
    * so in writing.
    *
    * The refusal above is right and its cost is real: the device registry is
    * rewritten WHOLE and holds nothing else, so removing the last device
    * leaves slots.csv zero bytes long. Without evidence that is
    * indistinguishable from a wiped phone -- so the sync refused it for ever,
    * the server went on holding devices the user had removed, and because
    * sync_run stops at the first log that fails, every OTHER log stopped
    * syncing too. A deliberate deletion that can never converge is a second
    * way to lose the record, by making the backup stop.
    *
    * So the deletion workflow leaves a durable, versioned tombstone next to
    * the log (log_note_cleared in util.h), and ONLY that tombstone authorises
    * the empty replacement below. The two states that decide whether this is
    * safe, both answered by log_clear_generation and neither of them here:
    *
    *   TOMBSTONE PRESENT, LOG NON-EMPTY -- it authorises nothing. This guard
    *   is not even reached (nlb > 0), the rows are pushed exactly as always,
    *   and the stale evidence is dropped below so it cannot speak for a
    *   LATER emptiness it knows nothing about.
    *
    *   TOMBSTONE ABSENT, LOG EMPTY OR MISSING -- refused, exactly as before.
    *   That is the storage-loss case and it stays fail-safe. A MISSING log is
    *   refused even WITH a tombstone: clearing a log leaves an empty file,
    *   losing the storage leaves a hole, and only the first is evidence. */
   if (nlb == 0 && nrb > 0 && log_clear_generation(l->path) <= 0)
      return -1;
   /* THE EVIDENCE EXPIRES THE MOMENT IT STOPS BEING TRUE. A log with rows in
    * it is not empty, deliberately or otherwise, so a tombstone left beside
    * it is answering a question nobody asked -- and would answer it wrongly
    * if the file were later truncated by something that is not the user. */
   if (nlb > 0 && log_clear_forget(l->path) != 0) {
      /* THE STALE TOMBSTONE IS STILL THERE, and it is the thing that decides
       * whether a future emptying of this log is believed. Left in place
       * beside a log that has rows, it will vouch for an emptiness that was
       * never deliberate -- so the sync stops here rather than continuing on
       * a premise it has just failed to establish. log_clear_forget has
       * already said which file and why. */
      return -1;
   }

   /* Walk them in WINDOWS. Each window is one pass over the file keeping only
    * that window's rows, so the memory held is the window and never the log.
    * A window is grown until its rows would exceed the budget, which
    * log_scan reports by failing -- so the loop halves the window and tries
    * again rather than guessing sizes up front. */
   static char text[SYNC_BUF_MAX];
   /* Unsigned window bounds. `at` and `span` only ever walk forward from 0,
    * but as ints nothing said so, and an index that MIGHT be negative makes
    * every lb[k] below a possible read before the array. */
   size_t nb_all = (size_t)nlb;
   size_t at     = 0;
   while (at < nb_all) {
      size_t span = nb_all - at;
      if (span > 128)
         span = 128;
      int nrow     = 0;
      int overflow = 0;
      for (;;) {
         nrow = log_scan(l, lb + at, (int)span, &overflow);
         if (nrow >= 0)
            break;
         if (!overflow || span == 1)
            return -1; /* a single bucket that does not fit is a real error */
         span /= 2;
      }
      for (size_t k = at; k < at + span; k++) {
         /* Rows for this window are in g_buf; select this bucket's and hash
          * them exactly as the server does. */
         int64_t tl =
             bucket_text(g_row, nrow, 1, lb[k], l->bucketed, text, sizeof text);
         if (tl < 0)
            return -1;
         size_t tn = (size_t)tl;
         char mine[17];
         hash16(text, (int64_t)tn, mine);
         const char *theirs = 0;
         for (int rr = 0; rr < nrb; rr++)
            if (rb[rr] == lb[k]) {
               theirs = rh[rr];
               break;
            }
         if (theirs && strncmp(theirs, mine, 16) == 0) {
            sync_progress_step();
            continue;
         }
         /* NEVER push an empty body for a bucket the enumeration says exists.
          *
          * The server implements a zero-length PUT as DELETE FROM logrow, so
          * this loop and the deletion loop below write the same request; only
          * the caller's intent differs. lb[] is built from rows that passed
          * row_ok, so every bucket in it holds at least one row, and tn == 0
          * here means the scan and the enumeration disagree -- the file was
          * replaced under us, the open failed, or the two readers parsed the
          * last line differently. Every one of those is a reason to stop, not
          * a reason to erase the server's copy of a day we still have.
          *
          * Deletion is legitimate in exactly one place: the loop below, which
          * walks buckets the enumeration did NOT find. */
         if (tn == 0)
            return -1;
         char path2[128];
         if (!sync_path_bucket(path2, sizeof path2, l->name, lb[k]))
            return -1;
         if (signed_req(sx, "PUT", path2, text, (int)tn, sx->rsp,
                        SYNC_BUF_MAX) != 200)
            return -1;
         sync_progress_step();
      }
      at += span;
   }

   for (int r = 0; r < nrb; r++) {
      int still = 0;
      for (int k = 0; k < nlb; k++)
         if (lb[k] == rb[r]) {
            still = 1;
            break;
         }
      if (still)
         continue;
      char p2[128];
      if (!sync_path_bucket(p2, sizeof p2, l->name, rb[r]))
         return -1;
      if (signed_req(sx, "PUT", p2, "", 0, sx->rsp, SYNC_BUF_MAX) != 200)
         return -1;
   }
   return 0;
}

/* ---- RESTORE: the direction that was missing ---------------------------
 *
 * The protocol is a replica: the phone is authoritative, and a bucket the
 * phone no longer holds is a correction the server must adopt. That is right
 * in steady state, and exactly wrong in the one case a backup exists for -- a
 * phone that has LOST its record. There, "I no longer hold it" is not a
 * correction, it is amnesia, and sync_one_log refuses to act on it (see the
 * nlb == 0 guard there).
 *
 * This turns that refusal into a recovery. It is DELIBERATELY NOT automatic:
 * an automatic pull would make a genuine deletion undoable by the next sync,
 * which is the same silent-wrong-direction problem in reverse. The user asks
 * for it, once, and it only ever ADDS -- every bucket the server holds and
 * this phone does not is appended to the local log, and nothing is removed.
 *
 * Returns the number of buckets restored, or -1. The caller reloads the logs
 * afterwards: this writes files, it does not touch the in-memory history. */
/* Copy `path` into `fd`, whole. 0 on success.
 *
 * The staging file starts as an exact copy of the live log, so a restore that
 * fails anywhere can be abandoned by deleting the copy -- the original has not
 * been opened for writing at all. A MISSING original is success with nothing
 * copied: a phone whose record is gone is the case this whole path exists for,
 * and there is no file to preserve. */
