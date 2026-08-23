/* SPDX-License-Identifier: GPL-3.0
 * home.c --- the record page and the dose log
 * Copyright 2026 Jakob Kastelic
 */
#include "db.h"
#include "http.h"
#include "insrow.h" /* the insulin log's one decoder */
#include "oops.h"
#include "page.h"
#include "pair.h"
#include "rowdec.h"
#include "util.h"
#include <sqlite3.h>
#include <stdarg.h> /* apnd: a bounded snprintf append */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HSLOTS 12
#define HMAX   400

struct hpt {
   int slot;
   int64_t glu;
   int ty;
};

/* Append to out[k], and NEVER return a k past cap.
 *
 * snprintf returns the length it WOULD have written, so `k += snprintf(...)`
 * walks the offset past the end of the buffer as soon as one call is
 * truncated -- and hour_row's guards were written as though it returned the
 * length it did write. " [%ld]" of a 20-digit long is 23 bytes against a
 * `k + 16 < cap` guard, so a single row carrying LONG_MIN pushed k beyond cap
 * and the caller then read past a 48 kB static into the response. row_ok
 * admits that value, so a paired app could put it there.
 *
 * Returning the clamped offset makes every guard in the function true by
 * construction rather than by arithmetic nobody re-checks. */
PANCRA_PRINTF(4, 5)
static size_t apnd(char *out, size_t cap, size_t k, const char *fmt, ...);

static size_t apnd(char *out, size_t cap, size_t k, const char *fmt, ...)
{
   if (k + 1 >= cap)
      return k; /* no room for even a NUL: leave the offset alone */
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(out + k, cap - k, fmt, ap);
   va_end(ap);
   if (n < 0)
      return k;
   size_t room = cap - k - 1; /* vsnprintf always NUL-terminates */
   return k + ((size_t)n > room ? room : (size_t)n);
}

static size_t hour_row(char *out, size_t cap, int hour, const struct hpt *hp,
                       int hn, int first)
{
   size_t k = apnd(out, cap, 0, "%02d", hour);
   int last = -1;
   for (int i = 0; i < hn; i++)
      if (hp[i].slot > last)
         last = hp[i].slot;
   for (int sl = 0; sl <= last && k + 48 < cap; sl++) {
      if (sl < first) {
         k = apnd(out, cap, k, "    ");
         continue;
      }
      int any = 0;
      /* hp is filled newest-first, so emit back-to-front for left-to-right
       * time order within the cell. */
      for (int i = hn; i-- > 0 && k + 16 < cap;) {
         if (hp[i].slot != sl)
            continue;
         k   = apnd(out, cap, k, hp[i].ty == 1 ? " [%" PRIwire "]" : " %3ld",
                    hp[i].glu);
         any = 1;
      }
      if (!any)
         k = apnd(out, cap, k, "   .");
   }
   if (k < cap)
      out[k++] = '\n';
   return k;
}

/* HOW OLD THE BIG NUMBER MAY BE, in seconds -- and it is the PHONE's window,
 * not a number of this file's own.
 *
 * The page and the app must blank at the same moment. At 900 s against the
 * app's 660, for four minutes after a sensor goes quiet the web page shows a
 * value the phone has already replaced with "---", and a reader comparing the
 * two sees the page contradict the device. The app's
 * definition lives in app/alarmlogic.h (AL_FRESH_S) with the reasoning for
 * the value; `make crosscheck` fails if these two ever drift apart again --
 * the server cannot include an app header, so the agreement is checked
 * rather than shared. */
#define WEB_FRESH_S 660

/* One reading row, newest first, as the page walks backwards. */
struct rd {
   int64_t t, glu;
   int ty;
};

/* The NEWEST reading, whatever its age: the single-user page always showed
 * its timestamp -- with a fresh value that says how current it is, and with a
 * stale one it says since when. Only the big number goes to "---". Asked
 * separately from the 24-hour window so a record that stopped a year ago
 * still says so rather than rendering blank.
 *
 * The LIMIT is not a guess at how many rows are recent: rows arrive in bucket
 * order and a backfilled row can sort ahead of them, so the newest few
 * buckets are scanned and the maximum taken by TIME. */
/* THE BIG NUMBER IS A CGM READING, never a fingerstick.
 *
 * This took the newest row of any kind, so a meter reading -- taken minutes
 * after the sensor's last sample, which is exactly when people test -- became
 * the number at the top of the page. The two are not interchangeable: a
 * fingerstick is a spot check from a different device with its own
 * calibration, and the page shows it in brackets in the table for precisely
 * that reason. The app's own big number has always resolved the primary CGM;
 * only this page conflated them.
 *
 * Returns 0 when the scan did not finish: the caller shows "--" rather than a
 * NEWEST reading that is merely the newest of the rows we managed to read. */
static int newest_reading(struct db *d, int64_t owner, int64_t *out_t,
                          int64_t *out_glu)
{
   *out_t = *out_glu = 0;
   sqlite3_stmt *st =
       db_prep(d, "SELECT line FROM logrow WHERE user_id=? AND log='readings'"
                  " ORDER BY bucket DESC, line DESC LIMIT 400");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln)
         continue;
      /* ONE DECODER, and it is the one every other reader here uses (see
       * rowdec.h). A row either decodes whole -- every field present, every
       * numeric field consumed entirely, every value in range -- or it is not
       * a row. There is no partial answer, because a partial answer is what
       * put a dose in the plot and an untyped row at the top of the page. */
      struct row_reading rr;
      if (!row_decode(ln, (int)strlen(ln), &rr))
         continue;
      if (rr.t <= *out_t)
         continue;
      /* REQUIRE AN EXPLICIT CGM. Not "anything but a fingerstick", and not
       * "a fingerstick unless the row forgot to say": KIND_INS and KIND_WT
       * are a DOSE and a BODY WEIGHT, so "8" would be printed as a blood
       * sugar of 8, and a row that does not state its kind does not say what
       * device made it. The number at the top of the page is the one a
       * person reads before deciding whether to eat or to inject; it may
       * only come from a row that says what it is. (row_decode already
       * refuses a row that stops before the kind.) */
      if (rr.kind != ROW_KIND_CGM)
         continue; /* including ROW_KIND_NONE: silence is not a claim */
      *out_t   = rr.t;
      *out_glu = rr.glu;
   }
   int ok = db_finished(rc);
   sqlite3_finalize(st);
   if (!ok) {
      *out_t = *out_glu = 0;
      return 0;
   }
   return 1;
}

/* The readings the table draws, from the last few days of buckets. Returns
 * the count, or -1 if the record could not be read at all -- which is not the
 * same as a record with nothing in it. */
static int load_recent(struct db *d, int64_t owner, int64_t today,
                       struct rd *out, int cap)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT line FROM logrow WHERE user_id=? AND log='readings'"
                  " AND bucket >= ? ORDER BY bucket DESC, line DESC");
   if (!st)
      return -1;
   sqlite3_bind_int64(st, 1, owner);
   sqlite3_bind_int64(st, 2, today - 2);

   int n = 0;
   int rc;
   while (n < cap && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln)
         continue;
      /* THE SAME DECODER the headline uses. These two disagreed: the table
       * took any numeric prefix it could find, so a row the headline refused
       * as malformed was still drawn -- one page showing what the other one
       * would not vouch for. */
      struct row_reading rr;
      if (!row_decode(ln, (int)strlen(ln), &rr))
         continue;
      out[n].t   = rr.t;
      out[n].glu = rr.glu;
      /* The table BRACKETS a fingerstick, so an untyped row is drawn like a
       * sensor reading -- which is a display choice, not a claim about the
       * device. The headline, which IS such a claim, refuses it. */
      out[n].ty = (rr.kind == ROW_KIND_NONE) ? ROW_KIND_CGM : rr.kind;
      n++;
   }
   /* WHY THE LOOP ENDED. Filling the buffer is a legitimate early exit -- the
    * page shows a few days and the array is the ceiling -- but anything else
    * that is not SQLITE_DONE is a read that STOPPED: a busy writer, an I/O
    * error, a damaged page. Returning `n` for those renders a partial day as
    * though it were the whole one, which is the failure this rule exists to
    * prevent, and the reader cannot tell: the table simply has fewer rows in
    * it than the sensor took. */
   int ok = (n >= cap) || db_finished(rc);
   sqlite3_finalize(st);
   return ok ? n : -1;
}

void h_home(struct req *r, int64_t me, const char *cookie)
{
   int have_own  = 0;
   int64_t owner = viewed_owner(r, me, &have_own);
   if (!owner)
      return;
   int foot_attr = 0;
   char myemail[256], owneremail[256];
   /* A BLANK IDENTITY IS NOT A RENDERABLE ANSWER. This page says whose data
    * it is showing -- in the nav bar, and in the "shared by" line when the
    * owner is somebody else -- so an address the database could not produce
    * must not appear as an empty string beside data it is labelling. 503,
    * because a database that cannot answer this is expected to answer it in a
    * minute; the alternative is a page that quietly attributes one person's
    * glucose to a blank. */
   if (email_of(r->db, me, myemail, sizeof myemail) == DB_GET_FAIL ||
       email_of(r->db, owner, owneremail, sizeof owneremail) == DB_GET_FAIL) {
      oops_busy(r);
      return;
   }
   int tz = tz_of(r->db, owner);

   struct sb s = {0};
   nav(&s, myemail, cookie);
   if (owner != me) {
      char esc[300] = {0};
      html_esc(esc, sizeof esc, owneremail);
      /* "back to mine" only when there IS a mine. A follower with no readings
       * and no paired app has nothing to go back to, and the link just leads
       * to an empty page wearing their own name. They still get told whose
       * record they are reading. */
      if (have_own || strstr(r->target, "who="))
         sb_add(&s,
                "<div>Viewing <b>%s</b> &mdash; "
                "<a href=\"/\">back to mine</a></div>\n",
                esc);
      else
         /* Deferred to the foot of the page: for a follower this IS their
          * page, so the reading belongs at the top where their eye goes, and
          * whose reading it is belongs where a caption goes. */
         foot_attr = 1;
   }

   /* The list of records shared with this viewer -- but not when it would
    * name the one record they are already looking at. A follower with a
    * single share gets that record AS their page and its owner in the
    * caption at the foot; a "Shared with you: <the same address>" line above
    * it says the same thing twice. */
   sqlite3_stmt *sh = foot_attr
                          ? NULL
                          : db_prep(r->db, "SELECT u.id,u.email FROM share s"
                                           " JOIN user u ON u.id=s.owner_id"
                                           " WHERE s.viewer_id=? ORDER BY"
                                           " u.email");
   if (sh) {
      sqlite3_bind_int64(sh, 1, me);
      int any = 0;
      int shrc;
      while ((shrc = sqlite3_step(sh)) == SQLITE_ROW) {
         if (!any)
            sb_add(&s, "<div>Shared with you: ");
         any           = 1;
         char esc[300] = {0};
         html_esc(esc, sizeof esc, (const char *)sqlite3_column_text(sh, 1));
         sb_add(&s, "<a href=\"/?who=%lld\">%s</a> ",
                (long long)sqlite3_column_int64(sh, 0), esc);
      }
      if (any)
         sb_add(&s, "</div>\n");
      /* A list that stopped early is a list of people the page does not
       * mention -- say so rather than quietly drop them. */
      if (!db_finished(shrc))
         sb_add(&s,
                "<div>(the shared-with-you list could not be read)</div>\n");
      sqlite3_finalize(sh);
   }

   int64_t now   = (int64_t)time(NULL);
   int64_t today = now / 86400;
   int64_t newest_t, newest_glu;
   /* A scan that did not finish leaves both at 0, which renders as "--": the
    * page says it does not know rather than naming the newest row it happened
    * to reach. */
   /* A SCAN THAT DID NOT FINISH IS NOT A RECORD WITH NOTHING IN IT. Ignoring
    * this printed "--" over a real reading, and -- with the recent table
    * empty for the same reason -- the page went on to say "No readings yet",
    * which is a statement about the user's data that was not true. */
   if (!newest_reading(r->db, owner, &newest_t, &newest_glu)) {
      sb_free(&s);
      oops(r);
      return;
   }

   static struct rd rd[4096];
   int nrd =
       load_recent(r->db, owner, today, rd, (int)(sizeof rd / sizeof rd[0]));
   if (nrd < 0) {
      sb_free(&s);
      oops(r);
      return;
   }

   /* Newest by TIME, not by bucket order: a backfilled row sorts by its own
    * timestamp, which is what the reader is looking at. */
   for (int i = 1; i < nrd; i++) {
      struct rd tmp = rd[i];
      int j         = i - 1;
      while (j >= 0 && rd[j].t < tmp.t) {
         rd[j + 1] = rd[j];
         j--;
      }
      rd[j + 1] = tmp;
   }

   /* The big number is the latest value, and its stamp is shown whether or
    * not it is fresh: with a fresh value it says how current it is, and when
    * stale it says since when. */
   char big[16] = "---";
   char stamp[40];
   snprintf(stamp, sizeof stamp, "%s", "-");
   char title[64];
   snprintf(title, sizeof title, "Pancra");
   if (newest_t > 0) {
      stamp_local(newest_t, tz, stamp, sizeof stamp);
      if (now - newest_t <= WEB_FRESH_S)
         snprintf(big, sizeof big, "%" PRIwire "", newest_glu);
      /* "HH:MM value", so a pinned tab is a glanceable readout -- and the
       * value blanks with the big number, so the tab can never show a
       * reading the page itself refuses to. */
      snprintf(title, sizeof title, "%s %s", stamp + 11, big);
   }
   sb_add(&s, "<div>%s</div>\n", stamp);
   sb_add(&s,
          "<a href=\"/\" style=\"text-decoration:none;color:inherit\">"
          "<div style=\"font-size:10em\">%s</div></a>\n"
          "<pre style=\"font-size:min(1em,calc((100vw - 20px)/31))\">\n",
          big);

   static char pre[48 * 1024];
   size_t k = 0;
   /* THE WINDOW STARTS AT THE TOP OF AN HOUR, not exactly 24h ago.
    *
    * Taken literally, "the last 24 hours" cuts the oldest hour wherever the
    * current minute happens to fall, and that hour then renders as a row of
    * leading blanks with a few readings on the end:
    *
    *     18                     149 140 134 138 145 151 154
    *
    * which reads as missing data rather than as a window boundary. Rounding
    * DOWN to the hour shows a little MORE than 24 hours -- at most 59 minutes
    * more -- and every row is then a complete hour. Showing slightly more
    * history than advertised is the cheaper error: the alternative is a
    * ragged first row on every page load except the one at the top of the
    * hour. */
   int64_t win_from = now - (24L * 3600);
   win_from -= win_from % 3600;
   int64_t prev_day = -1, cur_key = -1;
   int cur_hour = 0, have = 0, cur_first = 0;
   static struct hpt hp[HMAX];
   int hn = 0;
   for (int i = 0; i < nrd; i++) {
      if (rd[i].t <= win_from || rd[i].t > now)
         continue;
      int64_t local = rd[i].t + ((int64_t)tz * 60);
      int64_t days  = local / 86400;
      int hour      = (int)((local % 86400) / 3600);
      int minute    = (int)((local % 3600) / 60);
      int64_t key   = (days * 100) + hour;
      if (key != cur_key) {
         if (have && k + 120 < sizeof pre)
            k += hour_row(pre + k, sizeof pre - k, cur_hour, hp, hn, cur_first);
         cur_key  = key;
         cur_hour = hour;
         {
            int64_t hour_start =
                rd[i].t - ((int64_t)minute * 60) - (rd[i].t % 60);
            cur_first = 0;
            if (hour_start < win_from)
               cur_first = (int)((win_from - hour_start + 299) / 300);
            if (cur_first > HSLOTS)
               cur_first = HSLOTS;
         }
         hn   = 0;
         have = 1;
         if (days != prev_day && k + 60 < sizeof pre) {
            if (prev_day != -1)
               pre[k++] = '\n';
            char dstamp[40];
            stamp_local(rd[i].t, tz, dstamp, sizeof dstamp);
            dstamp[10] = '\0';
            k          = apnd(pre, sizeof pre, k, "<b>%s</b>\n", dstamp);
            prev_day   = days;
         }
      }
      if (hn < HMAX) {
         hp[hn].slot = minute / 5;
         hp[hn].glu  = rd[i].glu;
         hp[hn].ty   = rd[i].ty;
         hn++;
      }
   }
   if (have && k + 120 < sizeof pre)
      k += hour_row(pre + k, sizeof pre - k, cur_hour, hp, hn, cur_first);
   pre[k < sizeof pre ? k : sizeof pre - 1] = '\0';
   sb_raw(&s, pre, k);
   sb_add(&s,
          "</pre>\n"
          "<a href=\"/units%s\">Units ...</a>\n"
          "<br>\n"
          "<a href=\"/plots%s\">Plots ...</a>\n",
          r->who, r->who);
   if (foot_attr) {
      char esc[300] = {0};
      html_esc(esc, sizeof esc, owneremail);
      sb_add(&s, "<div>Viewing <b>%s</b></div>\n", esc);
   }

   if (!newest_t) {
      /* Only reachable once the two scans above have SUCCEEDED, so this
       * really is "nothing has synced", not "we could not tell". */
      /* THREE ANSWERS, NOT TWO. Read as a boolean, a database
       * that could not be asked rendered as "an app is paired, nothing has
       * synced" -- a sentence about a state nobody established, on the page
       * a user checks when something is wrong. */
      int paired = pair_is_paired(r->db, owner);
      if (paired < 0)
         sb_add(&s, "<div>No readings yet, and the pairing state could not "
                    "be read.</div>\n");
      else if (paired)
         sb_add(&s, "<div>An app is paired. Nothing has synced yet.</div>\n");
      else
         sb_add(&s, "<div>No readings yet. Pair the app from "
                    "<a href=\"/settings\">settings</a>.</div>\n");
   }
   /* REFRESH JUST AFTER THE NEXT SAMPLE IS DUE, not every two minutes.
    *
    * A CGM reports every five minutes, so a fixed 120 s poll is wrong twice
    * over: it fetches a page that cannot have changed, and when it does land
    * after a new sample it is on average two and a half minutes stale. Aiming
    * at the sample itself makes at most one request per reading AND shows it
    * within seconds of arriving.
    *
    * 45 SECONDS AFTER the due time, because the reading has to travel:
    * sensor to phone over BLE, then phone to here over the sync push. A
    * histogram of arrivals puts the great majority in the 3..45 s window
    * after the reading's own timestamp, so this waits out the far end of
    * that window rather than the near one. Landing on the due instant
    * itself would reliably miss the sample and then wait a full cycle for
    * it -- and so, nearly as often, did five seconds: the page reloaded,
    * showed the same reading, and the new one then sat unseen for five
    * minutes.
    *
    * WHAT THE BOUNDS ARE FOR. A record that is stale -- the sensor is off,
    * the phone is away, the session ended -- has no "next sample" to aim at,
    * and computing one from a timestamp hours old gives a due time long past.
    * That would clamp to the floor and turn a dead page into a hot loop
    * against the server, which is the opposite of what the fixed poll got
    * wrong. So: if the due instant has already passed, fall back to a slow
    * poll rather than a fast one. */
   {
      int64_t due  = newest_t + 300 + 45; /* next sample, plus travel */
      int64_t secs = due - now;
      int refresh_s;
      if (newest_t <= 0 || secs <= 0) {
         /* Nothing to wait for: poll slowly and let the next real sample
          * re-aim it. */
         refresh_s = 120;
      } else if (secs > 300) {
         /* A clock disagreement, or a backdated row: never wait longer than
          * one whole cadence, or a page could sit still through several. */
         refresh_s = 300;
      } else {
         refresh_s = (int)secs;
      }
      page_refresh(r, 200, "OK", title, s.p ? s.p : "", refresh_s);
   }
   sb_free(&s);
}

/* ---- the dose log: raw records, newest first, and nothing else ----
 *
 * Deliberately plain, like the single-user version: a table of what was
 * entered, in the order it happened. No plot, no totals.
 *
 * The log is append-only ASSERTIONS now (see pancra's insulin.h): each row
 * names a dose by id, and a later row for the same id corrects or retracts
 * it. So the file is replayed rather than read -- last assertion per id
 * wins, del removes -- which is also why an edit made today can change what
 * a row from March says. */
struct dose {
   int64_t id, t;
   int type, units;
};

void h_units(struct req *r, int64_t owner)
{
   int tz = tz_of(r->db, owner);
   /* NO LIFETIME CEILING. With a `static struct dose d[4096]` the replay
    * below simply SKIPS a dose once it is full -- so an account with more
    * than 4096 live doses (four a day is eleven years, and a pump user
    * reaches it far sooner) quietly stops showing the newest
    * ones, on a page that presented itself as the whole log. The array grows
    * instead, and the one thing that can still stop it -- the allocator
    * saying no -- is answered with an error rather than a short list. */
   struct dose *d = 0;
   int nd = 0, cap = 0, oom = 0;

   sqlite3_stmt *st =
       db_prep(r->db, "SELECT line FROM logrow WHERE user_id=?"
                      " AND log='insulin' ORDER BY bucket, line");
   if (st) {
      sqlite3_bind_int64(st, 1, owner);
      int64_t legacy = 0;
      int irc;
      while ((irc = sqlite3_step(st)) == SQLITE_ROW) {
         const char *ln = (const char *)sqlite3_column_text(st, 0);
         if (!ln || *ln < '0' || *ln > '9')
            continue;
         /* ---- ONE DECODER, THE PHONE'S ---------------------
          *
          * A hand-written clone of app/insulin.c's reader -- same dialect
          * select, same field order, same negative ids for the legacy rows --
          * drifts from it in ways that change what a reader is shown:
          *
          *   strtoll takes a numeric PREFIX, so "12abc" is 12 and "abc" is
          *   0, while the phone requires a field to be exactly a number;
          *
          *   treating every nonzero `del` as a retraction where the phone
          *   accepts only 1 makes a row with del=2 a live dose on one screen
          *   and a retracted one on the other.
          *
          * A viewer of a medical record must not invent entries its own
          * writer would not accept, and must not disagree with the device
          * about what was injected. lib/insrow.c is the one reader, and the
          * ranges live there with it. */
         struct ins_row row;
         if (!ins_row_decode(ln, ln + strlen(ln), -(legacy + 1), &row))
            continue;
         if (row.id < 0)
            legacy++; /* the four-field dialect: ids by file order */
         int64_t id = row.id;
         int del    = row.del;
         int64_t t  = row.t;
         int type   = row.type;
         int units  = row.units;
         int at     = -1;
         for (int i = 0; i < nd; i++)
            if (d[i].id == id) {
               at = i;
               break;
            }
         if (del) {
            if (at >= 0) {
               for (int i = at + 1; i < nd; i++)
                  d[i - 1] = d[i];
               nd--;
            }
            continue;
         }
         if (at < 0) {
            if (nd == cap) {
               /* Doubling, from a page's worth: the same policy sb uses, for
                * the same reason -- a log with years in it must not be
                * quadratic to replay. */
               int ncap        = cap ? cap * 2 : 256;
               struct dose *nx = realloc(d, (size_t)ncap * sizeof *d);
               if (!nx) {
                  oom = 1;
                  break;
               }
               d   = nx;
               cap = ncap;
            }
            at = nd++;
         }
         d[at].id    = id;
         d[at].t     = t;
         d[at].type  = type;
         d[at].units = units;
      }
      if (!db_finished(irc) && !oom)
         nd = 0; /* an incomplete dose list is not a dose list */
      sqlite3_finalize(st);
   }
   /* THE ONE REMAINING TRUNCATION, SAID OUT LOUD. A partial medical record
    * rendered as a complete one is the failure this whole item is about, so
    * the page is not produced at all. */
   if (oom) {
      free(d);
      oops(r);
      return;
   }

   /* newest first, like every other list here */
   for (int i = 1; i < nd; i++) {
      struct dose tmp = d[i];
      int j           = i - 1;
      while (j >= 0 && d[j].t < tmp.t) {
         d[j + 1] = d[j];
         j--;
      }
      d[j + 1] = tmp;
   }

   struct sb s = {0};
   sb_add(&s, "<pre>\n");
   if (!nd)
      sb_add(&s, "(nothing logged)\n");
   /* ONE RECORD PER LINE: date, time, amount, kind. Every line carries its
    * own date, so none of it depends on a heading further up. */
   for (int i = 0; i < nd; i++) {
      char when[64];
      stamp_local(d[i].t, tz, when, sizeof when);
      sb_add(&s, "%s %4d  %s\n", when, d[i].units,
             d[i].type == 1 ? "fast" : "slow");
   }
   sb_add(&s, "</pre>\n");
   sub_page(r, "Units", s.p ? s.p : "");
   sb_free(&s);
   free(d);
}
