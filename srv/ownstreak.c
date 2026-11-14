/* SPDX-License-Identifier: GPL-3.0
 * ownstreak.c --- an account's in-range streak, from its synced rows
 * Copyright 2026 Jakob Kastelic
 */
#include "ownstreak.h"
#include "logs.h"   /* logs_gen: whether a cached answer is still current */
#include "rowdec.h" /* row_decode: the one reader of a reading row */
#include "streak.h"
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h> /* INT64_MIN / INT64_MAX: a range open at one end */
#include <stdlib.h>
#include <string.h>
#include <time.h> /* time: which buckets are old */

/* THE SESSION STARTS, from the sensors log: id,type,mac,serial,model,fw,
 * activation,paired. A later row for an id corrects the earlier one, as the
 * phone replays it, so the last row read for an id wins. */
#define START_MAX 4096

/* Field `k` (0-based) of a comma-separated `ln` as a non-negative number; -1
 * when the field is missing, empty or not all digits. */
static long field_num(const char *ln, int k)
{
   const char *p = ln;
   for (int i = 0; i < k; i++) {
      p = strchr(p, ',');
      if (!p)
         return -1;
      p++;
   }
   long v = 0;
   int nd = 0;
   for (; *p >= '0' && *p <= '9'; p++) {
      if (++nd > 18)
         return -1;
      v = (v * 10) + (*p - '0');
   }
   if (!nd || (*p && *p != ',' && *p != '\n' && *p != '\r'))
      return -1;
   return v;
}

static int load_starts(struct db *d, int64_t owner, struct sensor_start *s,
                       int cap)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT line FROM logrow WHERE user_id=? AND log='sensors'"
                  " ORDER BY bucket, line");
   if (!st)
      return -1;
   sqlite3_bind_int64(st, 1, owner);
   int n = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln)
         continue;
      const long id  = field_num(ln, 0);
      const long act = field_num(ln, 6);
      if (id <= 0 || id > 0xFFFF || act < 0)
         continue;
      int k = 0;
      while (k < n && s[k].id != (int)id)
         k++;
      if (k == n) {
         if (n == cap)
            continue;
         n++;
      }
      s[k] = (struct sensor_start){(int)id, act};
   }
   const int ok = db_finished(rc);
   sqlite3_finalize(st);
   return ok ? n : -1;
}

/* THE LEAD COLUMN, field 13 of a reading row: 1 is the primary, any other
 * number is not, and a missing or empty field is not recorded. */
static unsigned char row_lead(const char *ln)
{
   const long v = field_num(ln, 13);
   if (v < 0)
      return READ_LEAD_UNKNOWN;
   return v == 1 ? READ_LEAD_YES : READ_LEAD_NO;
}

static bool same_pt(const struct streak_pt *x, const struct streak_pt *y)
{
   return (bool)(x->t == y->t && x->src == y->src && x->glu == y->glu);
}

/* A grow-only array of points. */
struct pts {
   struct streak_pt *p;
   int n, cap;
};

static int pts_push(struct pts *a, struct streak_pt x)
{
   if (a->n == a->cap) {
      const int ncap      = a->cap ? a->cap * 2 : 4096;
      struct streak_pt *q = realloc(a->p, (size_t)ncap * sizeof *q);
      if (!q)
         return 0;
      a->p   = q;
      a->cap = ncap;
   }
   a->p[a->n++] = x;
   return 1;
}

/* THE CGM READINGS IN BUCKETS [lo, hi), newest first, with one copy of a row
 * stored twice. A row that states no kind is a CGM sample, as the phone loads
 * it: the column postdates the format, and every row before it was one. A
 * source id no phone can mint reads as 0, the unattributed trace, as the
 * phone reads it. 0 when the log could not be read whole, and then `out`
 * holds nothing. */
static int read_points(struct db *d, int64_t owner, int64_t lo, int64_t hi,
                       struct pts *out)
{
   out->n           = 0;
   sqlite3_stmt *st = db_prep(d, "SELECT line FROM logrow WHERE user_id=?"
                                 " AND log='readings' AND bucket>=?"
                                 " AND bucket<?");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   sqlite3_bind_int64(st, 2, lo);
   sqlite3_bind_int64(st, 3, hi);
   int ok = 1;
   int rc = SQLITE_ERROR;
   while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln)
         continue;
      struct row_reading rr;
      if (!row_decode(ln, (int)strlen(ln), &rr))
         continue;
      if (rr.kind != ROW_KIND_CGM && rr.kind != ROW_KIND_NONE)
         continue;
      const int src = (rr.src < 0 || rr.src > 0xFFFF) ? 0 : rr.src;
      ok = pts_push(out, (struct streak_pt){.t    = (long)rr.t,
                                            .glu  = (short)rr.glu,
                                            .src  = (unsigned short)src,
                                            .cgm  = 1,
                                            .lead = row_lead(ln)});
   }
   ok = ok && db_finished(rc);
   sqlite3_finalize(st);
   if (!ok) {
      out->n = 0;
      return 0;
   }
   out->n = streak_sort(out->p, out->n);
   return 1;
}

/* Two newest-first runs into one, newest first, a point in both kept once. */
static int merge(const struct pts *a, const struct pts *b, struct pts *out)
{
   out->n       = 0;
   const int an = a->p ? a->n : 0;
   const int bn = b->p ? b->n : 0;
   int i        = 0;
   int j        = 0;
   while (i < an || j < bn) {
      struct streak_pt x;
      if (j >= bn || (i < an && streak_cmp(&a->p[i], &b->p[j]) <= 0))
         x = a->p[i++];
      else
         x = b->p[j++];
      if (out->n && same_pt(&x, &out->p[out->n - 1]))
         continue;
      if (!pts_push(out, x))
         return 0;
   }
   return 1;
}

/* EVERY CGM'S FIRST AND LAST READING. Source 0 is the unattributed trace and
 * has no span, as on the phone. */
static int spans_of(const struct pts *a, struct src_span *sp, int cap)
{
   int n = 0;
   for (int i = 0; i < a->n; i++) {
      const struct streak_pt *x = &a->p[i];
      if (x->src < 1)
         continue;
      int k = 0;
      while (k < n && sp[k].src != x->src)
         k++;
      if (k == n) {
         if (n == cap)
            continue;
         sp[n++] = (struct src_span){x->src, x->t, x->t};
      }
      if (x->t < sp[k].first)
         sp[k].first = x->t;
      if (x->t > sp[k].last)
         sp[k].last = x->t;
   }
   return n;
}

static long start_of(const struct sensor_start *s, int ns, int id)
{
   for (int k = 0; k < ns; k++)
      if (s[k].id == id)
         return s[k].start;
   return 0;
}

/* A number that changes when any session start does. */
static unsigned long starts_sig(const struct sensor_start *s, int ns)
{
   unsigned long h = 1469598103UL;
   for (int k = 0; k < ns; k++)
      h = (h * 1099511UL) ^ ((unsigned long)s[k].id * 2654435761UL) ^
          (unsigned long)s[k].start;
   return h ^ (unsigned long)ns;
}

/* ONE ACCOUNT'S CACHE.
 *
 * THE OLD BUCKETS, decoded once: every bucket before `cut`, read again only
 * when a write lands in an old bucket (logs_old_gen) or `cut` falls a week
 * behind. THE STREAK, as a running state (streak_acc) over every reading up
 * to the newest one applied.
 *
 * After a write, only the recent buckets are read, and the readings newer
 * than the newest applied are fed to the running state. Anything else -- a
 * reading at or before it that was not there before, a changed session
 * start, a row that does not record its lead -- rebuilds from the old
 * buckets and the recent ones together. */
#define CACHE_N   8
#define CUT_LAG_D 7

struct entry {
   int used;
   int64_t owner;
   unsigned long gen, old_gen;
   int64_t cut;
   struct pts old;
   int built; /* acc describes old + the recent buckets */
   struct streak_acc acc;
   int nupto;         /* recent readings at or before acc.seen_t */
   unsigned long sig; /* starts_sig of the starts acc was built with */
   int answered;      /* gen is the generation acc is current for */
};
static struct entry g_cache[CACHE_N];
static int g_cache_next;
static pthread_mutex_t g_cache_lk = PTHREAD_MUTEX_INITIALIZER;

static struct entry *entry_of(int64_t owner)
{
   for (int i = 0; i < CACHE_N; i++)
      if (g_cache[i].used && g_cache[i].owner == owner)
         return &g_cache[i];
   struct entry *e = &g_cache[g_cache_next];
   g_cache_next    = (g_cache_next + 1) % CACHE_N;
   free(e->old.p);
   *e       = (struct entry){0};
   e->owner = owner;
   return e;
}

/* How many of `r` (newest first) are at or before `t`. */
static int upto(const struct pts *r, long t)
{
   const int n = r->p ? r->n : 0;
   int k       = 0;
   while (k < n && r->p[k].t > t)
      k++;
   return n - k;
}

/* The whole history again: old and recent together. */
static int build(struct entry *e, const struct pts *recent,
                 const struct sensor_start *s, int ns, struct src_span *sp)
{
   struct pts all = {0};
   const int ok   = merge(recent, &e->old, &all);
   if (ok) {
      const int nsp = spans_of(&all, sp, STREAK_SPANS);
      streak_acc_build(&e->acc, all.p, all.n, sp, nsp, s, ns);
      e->nupto = upto(recent, e->acc.seen_t);
      e->built = 1;
   }
   free(all.p);
   return ok;
}

/* The newest of `recent`, those after acc.seen_t, applied oldest first; 0 if
 * one cannot be applied in order. */
static int advance(struct entry *e, const struct pts *recent,
                   const struct sensor_start *s, int ns)
{
   if (upto(recent, e->acc.seen_t) != e->nupto)
      return 0;
   const int nnew      = recent->p ? recent->n - e->nupto : 0;
   struct streak_acc a = e->acc;
   for (int i = nnew - 1; i >= 0; i--) {
      const struct streak_pt *x = &recent->p[i];
      if (!streak_acc_add(&a, x, start_of(s, ns, x->src), NULL, 0))
         return 0;
   }
   e->acc   = a;
   e->nupto = upto(recent, a.seen_t);
   return 1;
}

/* The answer for `e`, brought up to date with the rows; 1 when it could be. */
static int refresh(struct db *d, struct entry *e, unsigned long gen)
{
   const int64_t today      = (int64_t)time(NULL) / 86400;
   const unsigned long ogen = logs_old_gen();
   if (!e->used || e->old_gen != ogen || e->cut < today - CUT_LAG_D) {
      e->used  = 0;
      e->built = 0;
      e->cut   = today - LOGS_RECENT_DAYS;
      if (!read_points(d, e->owner, INT64_MIN, e->cut, &e->old))
         return 0;
      e->old_gen = ogen;
      e->used    = 1;
   }
   struct sensor_start *starts = malloc(START_MAX * sizeof *starts);
   struct src_span *spans      = malloc(STREAK_SPANS * sizeof *spans);
   struct pts recent           = {0};
   int ok                      = starts && spans;
   int ns                      = 0;
   if (ok)
      ns = load_starts(d, e->owner, starts, START_MAX);
   ok = ok && ns >= 0;
   ok = ok && read_points(d, e->owner, e->cut, INT64_MAX, &recent);
   if (ok) {
      const unsigned long sig = starts_sig(starts, ns);
      if (!e->built || e->sig != sig || !advance(e, &recent, starts, ns))
         ok = build(e, &recent, starts, ns, spans);
      e->sig = sig;
   }
   if (ok) {
      e->gen      = gen;
      e->answered = 1;
   }
   free(recent.p);
   free(spans);
   free(starts);
   return ok;
}

int owner_streak(struct db *d, int64_t owner, long *cur, long *best)
{
   /* THE GENERATION IS TAKEN FIRST: a write that commits while the rows are
    * read makes the next call look again rather than keep a stale answer. */
   const unsigned long gen = logs_gen();
   pthread_mutex_lock(&g_cache_lk);
   struct entry *e = entry_of(owner);
   int ok          = 1;
   if (!e->answered || e->gen != gen)
      ok = refresh(d, e, gen);
   if (ok) {
      *cur  = e->acc.cur;
      *best = e->acc.best > e->acc.cur ? e->acc.best : e->acc.cur;
   }
   pthread_mutex_unlock(&g_cache_lk);
   return ok;
}
