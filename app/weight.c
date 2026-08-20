// SPDX-License-Identifier: GPL-3.0
// weight.c --- Body-weight log: append-only CSV + in-memory tail
// Copyright 2026 Jakob Kastelic

/* See weight.h. Freestanding like insulin.c, whose shape this follows
 * deliberately: hand parsers with digit caps, every row validated on the way
 * in, and a partial write rolled back so it cannot merge with the next append
 * into one unparseable row. */
#include "weight.h"
#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf, SEEK_END */

static struct wt_rec g_wt[NWT];
static int g_nwt;

int wt_count(void)
{
   return g_nwt;
}

struct wt_rec wt_at(int i)
{
   struct wt_rec z = {0, 0};
   if (i < 0 || i >= g_nwt)
      return z;
   return g_wt[i];
}

struct wt_rec wt_newest(void)
{
   return wt_at(g_nwt - 1);
}

int wt_copy(struct wt_rec *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   for (; n < cap && n < g_nwt; n++)
      out[n] = g_wt[n];
   return n;
}

static char g_wt_path[256];

/* Column header, so an exported log is self-describing. GRAMS is named in the
 * header because the display unit is a preference and the file must not need
 * one to be read decades later. */
static const char g_wt_hdr[] = "# unix_time,grams,tz_offset_s\n";

const char *wt_unit_name(int units)
{
   return units == WT_LB ? "LB" : "KG";
}

long wt_from_tenths(int tenths, int units)
{
   if (tenths <= 0)
      return 0;
   long g = 0;
   if (units == WT_LB) {
      /* 1 lb = 453.59237 g, so a TENTH of a pound is 45.359237 g. Scaled
       * integer arithmetic with a half added before the divide, i.e. round to
       * nearest: 1540 (154.0 lb) -> 69853 g, which wt_to_tenths renders back
       * as exactly 1540. */
      g = (((long)tenths * 4535924L) + 50000L) / 100000L;
   } else {
      g = (long)tenths * 100L; /* tenths of a kg */
   }
   if (g < WT_MIN_G || g > WT_MAX_G)
      return 0;
   return g;
}

int wt_to_tenths(long g, int units)
{
   if (g <= 0)
      return 0;
   if (units == WT_LB) /* g / 45.359237, rounded to nearest */
      return (int)(((g * 100000L) + 2267962L) / 4535924L);
   return (int)((g + 50L) / 100L);
}

/* ---- OVERFLOW EVICTS THE OLDEST BY TIME, NOT THE OLDEST BY ARRIVAL ------
 *
 * This used to drop element ZERO and then let the caller's wt_sort() tidy up,
 * and element zero is whichever row was PUSHED first -- which is only the
 * oldest row when the file happens to be in chronological order. It very often
 * is not. Rows arrive in FILE order, and a user who imports a scale's history,
 * or types in a weigh-in they forgot to log last month, appends rows whose
 * timestamps are older than everything already in the tail.
 *
 * WHAT THAT LOOKED LIKE ON THE PHONE. A full tail, a backdated import, and the
 * weigh-ins from the last few days disappear from the table -- one per
 * imported row -- while the imported month-old ones sit there instead. Nothing
 * is lost from the file, so a restart brings the recent ones back for as long
 * as the tail has room again; it reads exactly like a display bug that fixes
 * itself, which is the hardest kind to be believed about.
 *
 * THE RULE NOW: the tail holds the NEWEST NWT rows BY TIME. Equivalently --
 * and this is how to read the code -- insert the row, sort, drop element zero.
 * Written out rather than actually done that way because the array has no
 * spare slot and the load pushes thousands of times:
 *
 *   - room to spare: take the row, and let the caller's sort file it.
 *   - full, and the new row is strictly older than every row held: the new row
 *     IS what the sort-then-drop would discard, so it is simply not taken. It
 *     displaces nobody.
 *   - full, otherwise: find the oldest row HELD (by time, ties resolved to the
 *     earliest arrival, exactly as a stable sort would), drop that one, take
 *     the new row.
 *
 * A tie goes to the ARRIVING row, which is the same answer "append, stable
 * sort, drop [0]" gives: among equal timestamps the stable sort leaves the
 * earlier arrival first, so the earlier arrival is the one dropped.
 *
 * The scan is linear over a 256-entry array and runs only once the tail is
 * full, which on a load is once per row past the first NWT. That is a few
 * thousand comparisons on a log of years -- far cheaper than the shift it
 * replaces did on every one of them. */
static void wt_push(const struct wt_rec *r)
{
   if (g_nwt < NWT) {
      g_wt[g_nwt++] = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < g_nwt; i++)
      if (g_wt[i].t < g_wt[oldest].t)
         oldest = i;
   if (r->t < g_wt[oldest].t)
      return; /* the arriving row is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < g_nwt; i++)
      g_wt[i - 1] = g_wt[i];
   g_wt[g_nwt - 1] = *r;
}

/* Oldest first. A backdated entry files into place immediately, so the table
 * never shows one row out of order until the next launch. Insertion sort: the
 * log is already sorted but for the row just added. */
static void wt_sort(void)
{
   for (int i = 1; i < g_nwt; i++) {
      struct wt_rec k = g_wt[i];
      int j           = i - 1;
      while (j >= 0 && g_wt[j].t > k.t) {
         g_wt[j + 1] = g_wt[j];
         j--;
      }
      g_wt[j + 1] = k;
   }
}

/* ONE READER FOR ONE ROW SHAPE. This and wt_parse_line held separate copies
 * of the same two fields and the same two range checks -- so a bound tightened
 * in one was a bound the other still let through, on the same file. */
static int wt_parse_rec(const char *p, const char *e, struct wt_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   /* No `why`: every answer the cursor could give about these two is already
    * a rejection here. An absent or non-numeric field reads 0, and 0 fails
    * `t <= 0` and `g < WT_MIN_G` alike; a run longer than the digit cap is
    * left holding its leading digits, which cannot land inside a plausible
    * weight or a plausible epoch. */
   r->t = csv_num(&c, 0);
   csv_sep(&c);
   r->g = csv_num(&c, 0);
   if (r->t <= 0 || r->t >= WT_T_MAX)
      return 0;
   if (r->g < WT_MIN_G || r->g > WT_MAX_G)
      return 0;
   return 1;
}

/* 1 = a record was taken, 0 = the line was the header (nothing to take),
 * -1 = A ROW WAS REJECTED.
 *
 * The third answer is the one that used to be missing. A corrupt row was
 * skipped and the load reported success, so the app showed a history with a
 * hole in it and said nothing -- and this file is never rewritten, so the
 * hole is permanent and silent. Skipping is still right (a row that does not
 * parse is not a weight), but the CALLER has to know it happened. */
static int wt_parse_line(const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0; /* the header */
   /* An empty line is not damage: a trailing newline at end of file produces
    * one, and so does an editor. */
   if (p >= e)
      return 0;
   /* Validate on the way IN. This file is loaded at every launch and never
    * rewritten, so a corrupt row admitted once is shown for good. */
   struct wt_rec r = {0, 0};
   if (!wt_parse_rec(p, e, &r))
      return -1;
   wt_push(&r);
   return 1;
}

/* Returns 0 when the file was read WHOLE -- including the first-run case where
 * there is nothing to read -- and -1 when what was loaded is INCOMPLETE.
 *
 * The distinction is the point, and "incomplete" is wider than "the read
 * failed". What is lost is not a file, it is RECORD: every weight the user has
 * logged, and the trend drawn from them. All four of these produce a short
 * history, and only the first used to be reported:
 *
 *   - read(2) failed partway (a dying card);
 *   - a row did not parse, or held a value no scale produces;
 *   - a line was longer than any row can be, so it was skipped rather than
 *     parsed as a truncation of itself;
 *   - the file ends mid-line, which means it was cut while being written.
 *
 * Whatever parsed is KEPT -- a prefix of the truth beats an empty screen --
 * and the caller says so instead of presenting a short history as complete. */
int weight_load(void)
{
   g_nwt  = 0;
   int fd = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   /* Stream the whole file a line at a time (the insulin.c pattern): the tail
    * keeps only the last NWT rows, so memory stays bounded however many years
    * the file has grown. */
   char buf[1024];
   char line[96];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0; /* anything that means "what you have is short" */
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            /* Two ways to lose a row -- longer than any row can be, or a
             * row that does not parse -- and one thing to say about both:
             * what you are looking at is short. */
            if (over || wt_parse_line(line, line + llen) < 0)
               damaged = 1;
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0) {
      /* A FINAL LINE WITH NO NEWLINE is a file that was cut while being
       * written -- power loss, or a full disk mid-append. Its bytes may parse
       * perfectly and still be half a record, so it is kept if it parses (the
       * prefix rule) and reported either way. It used to be accepted in
       * silence, and the test asserted that as correct. */
      /* NOT PARSED. The bytes may make a perfectly valid pair -- and still be
       * half a record, because what says a row is finished is the newline the
       * file does not have. Publishing it puts a weight the user never logged
       * into a log that is never rewritten. */
      damaged = 1;
   }
   close(fd);
   wt_sort();
   return (n < 0 || damaged) ? -1 : 0;
}

int weight_append(long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   char b[64];
   int n = snprintf(b, sizeof b, "%ld,%ld,%ld\n", t, g, tz);
   n     = clampn(n, sizeof b);
   /* ONE OPERATION for the whole append, including the header on a new file:
    * see log_append. This used to be open/lseek/write-header/write-row/close
    * here, with the same two holes every other log had -- a first record that
    * was not atomic, and a failed rollback reported as a clean failure. */
   int rc = log_append(g_wt_path, g_wt_hdr, (int)sizeof g_wt_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct wt_rec r = {t, g};
   wt_push(&r);
   wt_sort();
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* Shared worker for update/delete: stream the file, copy every line to
 * <path>.tmp except the LAST row matching `orig`, which is replaced (or
 * skipped when del). Rewrite-and-rename so a crash never truncates the log.
 * The insulin.c original, with its two-field key. */
static int wt_rewrite(const struct wt_rec *orig, int del, long t, long g,
                      long tz)
{
   /* pass 1: how many rows match? (we edit the LAST one) */
   int fd = open(g_wt_path, O_RDONLY, 0);
   if (fd < 0)
      return -1;
   char buf[1024];
   char line[256];
   int llen   = 0;
   int nmatch = 0;
   long n     = 0;
   struct wt_rec r;
   while ((n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (llen < (int)sizeof line &&
                wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.g == orig->g)
               nmatch++;
            llen = 0;
         } else if (llen < (int)sizeof line) {
            line[llen++] = buf[i];
         } else {
            llen = (int)sizeof line; /* over-long: cannot match */
         }
      }
   close(fd);
   if (nmatch == 0)
      return -1;

   /* pass 2: copy, altering only match #nmatch */
   char tmp[sizeof g_wt_path + 4];
   int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g_wt_path);
   if (tn <= 0 || tn >= (int)sizeof tmp)
      return -1;
   fd = open(g_wt_path, O_RDONLY, 0);
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
    * line in the file -- see the tombstone at the bottom of this function. */
   long kept = 0;
   llen      = 0;
   /* `ok` used to be cleared only by a WRITE failure. A read error returns -1,
    * which fails this loop's `> 0` test and left ok == 1 -- so the rename
    * below swapped in whatever prefix had been copied and the rest of the
    * weight log was gone. The rewrite-and-rename is what makes this file
    * crash-safe; it must not also be what truncates it. */
   while (ok && (n = read(fd, buf, sizeof buf)) > 0)
      for (long i = 0; ok && i < n; i++) {
         if (buf[i] != '\n') {
            /* -1: the newline below is appended UNCONDITIONALLY, so the last
             * byte of `line` belongs to it (the insulin.c overflow). */
            if (llen < (int)sizeof line - 1)
               line[llen++] = buf[i];
            else
               ok = 0; /* over-long row: refuse to rewrite blind */
            continue;
         }
         int hit = wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                   r.g == orig->g && ++seen == nmatch;
         if (hit && del) {
            llen = 0;
            continue; /* the deleted row is simply not copied */
         }
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%ld,%ld", t, g, tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
         else
            kept += llen;
         llen = 0;
      }
   if (n < 0)
      ok = 0; /* the read failed mid-file: see the loop above */
   /* a final line with no trailing newline gets the same treatment (the
    * appender always terminates rows, but the file is user-copyable) */
   if (ok && llen > 0 && llen < (int)sizeof line) {
      int hit = wt_parse_rec(line, line + llen, &r) && r.t == orig->t &&
                r.g == orig->g && ++seen == nmatch;
      if (!(hit && del)) {
         if (hit)
            llen = snprintf(line, sizeof line, "%ld,%ld,%ld", t, g, tz);
         line[llen++] = '\n';
         if (write(out, line, llen) != llen)
            ok = 0;
         else
            kept += llen;
      }
   }
   close(fd);
   /* DURABLY, or not at all.
    *
    * This used to close(out) unchecked and rename -- no fsync of the new
    * contents, no check that close flushed them, no sync of the directory the
    * rename lands in. A power loss in that window leaves the log EMPTY: the
    * rename is visible while the bytes it points at never reached the disk,
    * and the user's whole weight history is a file of nothing. Every other
    * rewrite in the app goes through this same finish (see util.h). */
   if (!ok) {
      close(out);
      (void)unlink(tmp);
      return -1;
   }
   /* Only a rename that never happened is a failure here: past it the log
    * file already holds the rewritten rows, and reporting failure would
    * leave the caller's tail disagreeing with the file it just wrote. */
   if (replace_finish(out, tmp, g_wt_path) == REPLACE_FAILED)
      return -1;
   /* THE OTHER HALF OF A DELETE THAT REMOVED THE LAST LINE.
    *
    * This log is synced, and the phone is authoritative over the server's
    * copy of it -- so an empty log is an instruction to delete the replica.
    * The sync client refuses that instruction unless it can tell a deliberate
    * emptying from a phone that lost its storage, and it cannot tell by
    * looking: both are a log with no rows. It therefore refuses, and would go
    * on refusing for ever, with the user's deletion never reaching the server
    * and every other log's sync stopped behind it.
    *
    * Only the code that did the emptying knows it was meant, so it is this
    * code that says so. `kept == 0` is exactly that case and nothing else: a
    * delete that removed the only line. In practice the '#' header keeps this
    * file non-empty, so the tombstone is minted for a header-less log (one
    * written by an older build, or copied in by hand) -- which is precisely
    * the log whose last delete would otherwise wedge the sync silently.
    *
    * The reverse is stated too, because a rewrite that still keeps lines is
    * proof the log is NOT empty, and evidence of emptiness must not outlive
    * the emptiness it describes. */
   if (kept == 0)
      (void)log_note_cleared(g_wt_path);
   else
      (void)log_clear_forget(g_wt_path);
   /* An EDIT or a DELETE is exactly the mutation file sizes can miss: a
    * corrected weight of the same length leaves the log byte-for-byte as long
    * as it was. */
   record_mutated();
   /* The tail mirrors the file again. A read that fails here leaves memory
    * holding the pre-edit rows, which is the honest state: the FILE is
    * correct and the next load will agree with it. */
   (void)weight_load();
   return 0;
}

int weight_update(const struct wt_rec *orig, long t, long g, long tz)
{
   if (t <= 0 || t >= WT_T_MAX)
      return -1;
   if (g < WT_MIN_G || g > WT_MAX_G)
      return -1;
   return wt_rewrite(orig, 0, t, g, tz);
}

int weight_delete(const struct wt_rec *orig)
{
   return wt_rewrite(orig, 1, 0, 0, 0);
}

/* ---- the weight-entry form (see weight.h) ---- */

/* What a form with NO history offers. A first weigh-in has nothing to start
 * from, and an empty field is worse than a plausible one: 70 kg is a number
 * the user edits, whereas 0 is one they must type from scratch. */
#define WT_FORM_DEFAULT_G 70000L

void wt_form_open(struct wt_form *f, long last_g, int units, long now)
{
   if (!f)
      return;
   f->t      = now;
   f->tenths = wt_to_tenths(last_g > 0 ? last_g : WT_FORM_DEFAULT_G, units);
   f->edit   = -1;
}

/* The weight log's filename. */
int weight_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_wt_path, sizeof g_wt_path, dir, "/weight.csv");
   return ok;
}

const char *weight_path(void)
{
   return g_wt_path;
}
