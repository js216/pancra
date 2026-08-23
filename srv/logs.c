/* SPDX-License-Identifier: GPL-3.0
 * logs.c --- the sync protocol: digests, and whole-bucket replacement
 * Copyright 2026 Jakob Kastelic
 *
 * The entire protocol is a two-level Merkle comparison. The app asks what
 * the server has, in two increasingly detailed
 * summaries, and pushes only the buckets whose summaries differ. When the top
 * summary matches, the two sides provably hold the same bytes.
 *
 * Everything here therefore depends on ONE thing being identical in both
 * implementations: the canonical text of a bucket -- rows sorted bytewise,
 * each terminated by a newline. sqlite's default TEXT collation IS bytewise,
 * so `ORDER BY line` is exactly the order the app must sort in. If that ever
 * stops being true the digests silently stop matching, which is why the
 * queries below spell out the ordering rather than relying on the primary
 * key's.
 */
#include "logs.h"
#include "db.h"
#include "http.h"
#include "oops.h"
#include "page.h"
#include "proto.h"
#include "sha256.h"
#include "util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* First 16 hex chars of a SHA-256, the protocol's hash. 64 bits is ample:
 * this compares two copies of the same honest data set, and the party who
 * could profit from a collision is the party who owns the data. */
static void hash16(struct sha256_ctx *c, char out[17])
{
   uint8_t h[32];
   char hex[65];
   sha256_final(c, h);
   hex_of(h, 32, hex);
   memcpy(out, hex, 16);
   out[16] = '\0';
}

static void hash_start(struct sha256_ctx *c)
{

   sha256_init(c);
}

static void hash_line(struct sha256_ctx *c, const char *line)
{
   sha256_update(c, (const unsigned char *)line, strlen(line));
   sha256_update(c, (const unsigned char *)"\n", 1);
}

int log_name_ok(const char *s)
{
   size_t n = strlen(s);
   if (n == 0 || n > LOGNAME_MAX)
      return 0;
   for (size_t i = 0; i < n; i++) {
      char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '.' || c == '-'))
         return 0;
   }
   return 1;
}

static void send_sb(struct req *r, struct sb *s)
{
   if (s->err)
      oops(r);
   else
      http_respond(r->c, 200, "OK", "text/plain", s->p ? s->p : "", s->n);
   sb_free(s);
}

/* GET /v1/digest -- one line per log: "<log> <rows> <hash>".
 *
 * One ordered pass over the user's rows computes every bucket hash and folds
 * each into its log's hash as it goes, so the whole answer costs a single
 * index scan and no intermediate storage. */
void h_digest(struct req *r)
{
   sqlite3_stmt *st =
       db_prep(r->db, "SELECT log,bucket,line FROM logrow"
                      " WHERE user_id=? ORDER BY log,bucket,line");
   if (!st) {
      oops(r);
      return;
   }
   sqlite3_bind_int64(st, 1, r->uid);
   struct sb out                 = {0};
   char cur_log[LOGNAME_MAX + 1] = {0};
   int64_t cur_bucket            = -1;
   int64_t rows                  = 0;
   int in_log = 0, in_bucket = 0;
   struct sha256_ctx bh, lh;

   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *log = (const char *)sqlite3_column_text(st, 0);
      int64_t bucket  = (int64_t)sqlite3_column_int64(st, 1);
      const char *ln  = (const char *)sqlite3_column_text(st, 2);
      if (!log || !ln)
         continue;
      int new_log = !in_log || strcmp(log, cur_log);
      if (new_log || bucket != cur_bucket) {
         if (in_bucket) {
            char bhex[17];
            hash16(&bh, bhex);

            char fold[64];
            int n = snprintf(fold, sizeof fold, "%" PRIwire " %s\n", cur_bucket,
                             bhex);
            sha256_update(&lh, (const unsigned char *)fold, (size_t)n);
            in_bucket = 0;
         }
      }
      if (new_log) {
         if (in_log) {
            char lhex[17];
            hash16(&lh, lhex);

            sb_add(&out, "%s %" PRIwire " %s\n", cur_log, rows, lhex);
         }
         snprintf(cur_log, sizeof cur_log, "%s", log);
         hash_start(&lh);
         in_log = 1;
         rows   = 0;
      }
      if (!in_bucket) {
         hash_start(&bh);
         in_bucket  = 1;
         cur_bucket = bucket;
      }
      hash_line(&bh, ln);
      rows++;
   }
   int done = db_finished(rc);
   sqlite3_finalize(st);
   if (!done) {
      sb_free(&out);
      oops(r);
      return;
   }
   if (in_bucket) {
      char bhex[17];
      hash16(&bh, bhex);

      char fold[64];
      int n =
          snprintf(fold, sizeof fold, "%" PRIwire " %s\n", cur_bucket, bhex);
      sha256_update(&lh, (const unsigned char *)fold, (size_t)n);
   }
   if (in_log) {
      char lhex[17];
      hash16(&lh, lhex);

      sb_add(&out, "%s %" PRIwire " %s\n", cur_log, rows, lhex);
   }
   send_sb(r, &out);
}

/* GET /v1/digest/<log> -- one line per bucket: "<bucket> <rows> <hash>". */
void h_digest_log(struct req *r, const char *log)
{
   if (!log_name_ok(log)) {
      http_text(r->c, 400, "Bad Request", "bad log name\n");
      return;
   }
   sqlite3_stmt *st =
       db_prep(r->db, "SELECT bucket,line FROM logrow"
                      " WHERE user_id=? AND log=? ORDER BY bucket,line");
   if (!st) {
      oops(r);
      return;
   }
   sqlite3_bind_int64(st, 1, r->uid);
   sqlite3_bind_text(st, 2, log, -1, SQLITE_STATIC);
   struct sb out = {0};
   int64_t cur = -1, rows = 0;
   int in_bucket = 0;
   struct sha256_ctx bh;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      int64_t bucket = (int64_t)sqlite3_column_int64(st, 0);
      const char *ln = (const char *)sqlite3_column_text(st, 1);
      if (!ln)
         continue;
      if (in_bucket && bucket != cur) {
         char bhex[17];
         hash16(&bh, bhex);

         sb_add(&out, "%" PRIwire " %" PRIwire " %s\n", cur, rows, bhex);
         in_bucket = 0;
      }
      if (!in_bucket) {
         hash_start(&bh);
         in_bucket = 1;
         cur       = bucket;
         rows      = 0;
      }
      hash_line(&bh, ln);
      rows++;
   }
   int done = db_finished(rc);
   sqlite3_finalize(st);
   if (!done) {
      sb_free(&out);
      oops(r);
      return;
   }
   if (in_bucket) {
      char bhex[17];
      hash16(&bh, bhex);

      sb_add(&out, "%" PRIwire " %" PRIwire " %s\n", cur, rows, bhex);
   }
   send_sb(r, &out);
}

/* GET /v1/bucket/<log>/<n> -- the canonical text itself.
 *
 * Bounded without a cap of its own: a bucket can only have got here through
 * a PUT, and a PUT body cannot exceed BODY_MAX. */
void h_bucket_get(struct req *r, const char *log, int64_t bucket)
{
   if (!log_name_ok(log) || bucket < 0) {
      http_text(r->c, 400, "Bad Request", "bad log or bucket\n");
      return;
   }
   sqlite3_stmt *st = db_prep(r->db, "SELECT line FROM logrow"
                                     " WHERE user_id=? AND log=? AND bucket=?"
                                     " ORDER BY line");
   if (!st) {
      oops(r);
      return;
   }
   sqlite3_bind_int64(st, 1, r->uid);
   sqlite3_bind_text(st, 2, log, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, bucket);
   struct sb out = {0};
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (ln)
         sb_add(&out, "%s\n", ln);
   }
   int done = db_finished(rc);
   sqlite3_finalize(st);
   if (!done) {
      sb_free(&out);
      oops(r);
      return;
   }
   send_sb(r, &out);
}

/* ONE QUOTA COUNT, or a definite failure.
 *
 * `log` NULL binds only the user; a `bucket` of -1 leaves that parameter
 * unbound. Returns 0 if the statement could not be prepared or did not
 * produce its row -- see the caller for why that must refuse the write rather
 * than assume zero. */
static int quota_count(struct db *d, int64_t *out, const char *sql, int64_t uid,
                       const char *log, int64_t bucket)
{
   sqlite3_stmt *st = db_prep(d, sql);
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, uid);
   if (log)
      sqlite3_bind_text(st, 2, log, -1, SQLITE_STATIC);
   if (bucket >= 0)
      sqlite3_bind_int64(st, 3, bucket);
   int ok = sqlite3_step(st) == SQLITE_ROW;
   if (ok)
      *out = (long)sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return ok;
}

/* A row is one line of text and nothing else: the bytes ARE the identity, so
 * anything that could make two implementations disagree about those bytes has
 * to be refused rather than cleaned up. Silently trimming a trailing space
 * would give the app a row whose hash it can never reproduce. */
static int row_ok(const char *line, size_t len)
{
   if (len == 0 || len > ROW_MAX)
      return 0;
   if (line[len - 1] == ' ' || line[len - 1] == '\t')
      return 0;
   for (size_t i = 0; i < len; i++)
      if ((unsigned char)line[i] < 0x20 && line[i] != '\t')
         return 0;
   return 1;
}

/* PUT /v1/bucket/<log>/<n> -- the body REPLACES the bucket.
 *
 * Whole-bucket replacement is the entire reason this protocol needs no notion
 * of editing or deleting a row: a corrected dose, a deleted one and a
 * backfilled reading all reach the server the same way, as "this bucket now
 * contains exactly these lines". The replacement is one transaction, so a
 * connection dropped mid-PUT leaves the stored bucket intact rather than
 * half a new one -- a half-written bucket would leave both sides disagreeing
 * with no way to notice.
 */
void h_bucket_put(struct req *r, const char *log, int64_t bucket)
{
   if (!log_name_ok(log) || bucket < 0 || bucket > 0x7fffffffL) {
      http_text(r->c, 400, "Bad Request", "bad log or bucket\n");
      return;
   }
   /* Parse and vet the WHOLE body before touching the database: a partially
    * applied PUT is the one outcome the protocol cannot express. */
   size_t nlines = 0;
   for (size_t i = 0; i < r->body_len;) {
      size_t j = i;
      while (j < r->body_len && r->body[j] != '\n')
         j++;
      size_t len = j - i;
      if (len && r->body[i + len - 1] == '\r')
         len--;
      if (len) {
         if (!row_ok(r->body + i, len)) {
            http_text(r->c, 400, "Bad Request", "malformed row\n");
            return;
         }
         nlines++;
      }
      i = j + 1;
   }
   if (nlines > BUCKET_ROWS) {
      http_text(r->c, 400, "Bad Request", "too many rows in one bucket\n");
      return;
   }

   /* Caps that bound what one account can make the SD card hold. Checked
    * against the state BEFORE the replacement, minus what this bucket
    * already contributes, so re-PUTting an unchanged bucket can never be
    * refused for being too big. */
   /* THE QUOTA IS COUNTED INSIDE THE TRANSACTION.
    *
    * These counts decide whether the write is allowed, and the write happens
    * below; taken outside the transaction they were a time-of-check against a
    * time-of-use, and two concurrent PUTs could each see room for the last
    * rows and both take it. BEGIN IMMEDIATE takes the write lock now, so the
    * numbers this reads are the numbers the INSERT obeys. */
   if (!db_exec(r->db, "BEGIN IMMEDIATE")) {
      oops(r);
      return;
   }
   /* EVERY COUNT IS MANDATORY.
    *
    * These four queries decide whether the write is allowed. Wrapped in
    * `if (st)` with the count left at 0 when prepare fails, and with a step
    * that is not a row leaving it at 0 too, a database error does not refuse
    * the write -- it grants UNLIMITED quota: zero rows held, zero buckets,
    * zero logs, every cap passed. The one path where the
    * server cannot see what it is holding is exactly the path where it must
    * not decide it has room. */
   int64_t have_rows = 0, have_here = 0, have_logs = 0, have_buckets = 0;
   int counted = 1;
   counted     = counted && quota_count(r->db, &have_rows,
                                        "SELECT count(*) FROM logrow"
                                        " WHERE user_id=?",
                                        r->uid, NULL, -1);
   counted = counted && quota_count(r->db, &have_here,
                                    "SELECT count(*) FROM logrow"
                                    " WHERE user_id=? AND log=? AND bucket=?",
                                    r->uid, log, bucket);
   counted = counted && quota_count(r->db, &have_logs,
                                    "SELECT count(DISTINCT log) FROM logrow"
                                    " WHERE user_id=?",
                                    r->uid, NULL, -1);
   counted = counted && quota_count(r->db, &have_buckets,
                                    "SELECT count(DISTINCT bucket) FROM logrow"
                                    " WHERE user_id=? AND log=?",
                                    r->uid, log, -1);
   if (!counted) {
      db_exec(r->db, "ROLLBACK");
      oops(r);
      return;
   }
   if (have_rows - have_here + (int64_t)nlines > USER_ROWS) {
      db_exec(r->db, "ROLLBACK");
      http_text(r->c, 507, "Insufficient Storage", "at the row cap\n");
      return;
   }
   if (have_here == 0 && nlines && have_buckets >= LOG_BUCKETS) {
      db_exec(r->db, "ROLLBACK");
      http_text(r->c, 400, "Bad Request", "too many buckets in one log\n");
      return;
   }
   if (have_here == 0 && nlines && have_buckets == 0 &&
       have_logs >= USER_LOGS) {
      db_exec(r->db, "ROLLBACK");
      http_text(r->c, 400, "Bad Request", "too many logs\n");
      return;
   }
   sqlite3_stmt *del = db_prep(
       r->db, "DELETE FROM logrow WHERE user_id=? AND log=? AND bucket=?");
   sqlite3_stmt *ins = db_prep(r->db, "INSERT OR IGNORE INTO"
                                      " logrow(user_id,log,bucket,line)"
                                      " VALUES(?,?,?,?)");
   if (!del || !ins) {
      if (del)
         sqlite3_finalize(del);
      if (ins)
         sqlite3_finalize(ins);
      db_exec(r->db, "ROLLBACK");
      oops(r);
      return;
   }
   sqlite3_bind_int64(del, 1, r->uid);
   sqlite3_bind_text(del, 2, log, -1, SQLITE_STATIC);
   sqlite3_bind_int64(del, 3, bucket);
   int ok = sqlite3_step(del) == SQLITE_DONE;
   sqlite3_finalize(del);

   int64_t stored = 0;
   for (size_t i = 0; ok && i < r->body_len;) {
      size_t j = i;
      while (j < r->body_len && r->body[j] != '\n')
         j++;
      size_t len = j - i;
      if (len && r->body[i + len - 1] == '\r')
         len--;
      if (len) {
         sqlite3_reset(ins);
         sqlite3_bind_int64(ins, 1, r->uid);
         sqlite3_bind_text(ins, 2, log, -1, SQLITE_STATIC);
         sqlite3_bind_int64(ins, 3, bucket);
         sqlite3_bind_text(ins, 4, r->body + i, (int)len, SQLITE_STATIC);
         if (sqlite3_step(ins) != SQLITE_DONE)
            ok = 0;
         else
            stored++;
      }
      i = j + 1;
   }
   sqlite3_finalize(ins);
   if (!ok) {
      db_exec(r->db, "ROLLBACK");
      oops(r);
      return;
   }
   if (!db_exec(r->db, "COMMIT")) {
      (void)db_exec(r->db, "ROLLBACK");
      oops(r);
      return;
   }
   char msg[64];
   snprintf(msg, sizeof msg, "stored %" PRIwire " rows\n", stored);
   http_text(r->c, 200, "OK", msg);
}
