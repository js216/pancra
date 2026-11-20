// SPDX-License-Identifier: GPL-3.0
// streak.c --- which readings an in-range streak counts, and how long it is
// Copyright 2026 Jakob Kastelic

/* See streak.h. Pure: no globals, no clock, no locks. */
#include "streak.h"

#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* qsort */

bool streak_first_points(long start, long t, int n)
{
   return (bool)(start > 0 && n <= STREAK_N_SKIP && t >= start &&
                 t < start + STREAK_SKIP_S);
}

static long start_of(const struct sensor_start *s, int ns, int id)
{
   for (int k = 0; k < ns; k++)
      if (s[k].id == id)
         return s[k].start;
   return 0;
}

bool read_lead(int recorded, int src, long t, const struct src_span *sp,
               int nsp)
{
   if (recorded == READ_LEAD_YES)
      return true;
   if (recorded == READ_LEAD_NO)
      return false;
   long own = 0;
   for (int k = 0; k < nsp; k++)
      if (sp[k].src == src)
         own = sp[k].first;
   if (own <= 0)
      return true;
   for (int k = 0; k < nsp; k++) {
      if (sp[k].src == src || t < sp[k].first || t > sp[k].last)
         continue;
      /* Running at `t` and started earlier: that one was the primary. Ties
       * go to the lower id, so exactly one of two sensors leads. */
      if (sp[k].first < own || (sp[k].first == own && sp[k].src < src))
         return false;
   }
   return true;
}

int streak_cmp(const void *a, const void *b)
{
   const struct streak_pt *x = a;
   const struct streak_pt *y = b;
   if (x->t != y->t)
      return x->t < y->t ? 1 : -1;
   if (x->src != y->src)
      return x->src < y->src ? 1 : -1;
   return (x->glu > y->glu) - (x->glu < y->glu);
}

int streak_sort(struct streak_pt *r, int n)
{
   if (!r || n <= 0)
      return 0;
   qsort(r, (size_t)n, sizeof *r, streak_cmp);
   int u = 0;
   for (int i = 0; i < n; i++)
      if (!u || r[i].t != r[u - 1].t || r[i].src != r[u - 1].src ||
          r[i].glu != r[u - 1].glu)
         r[u++] = r[i];
   return u;
}

void streak_acc_init(struct streak_acc *a)
{
   *a = (struct streak_acc){0};
}

/* The reading's place among its sensor's readings since `start`, the oldest
 * being 1, counting this one -- or 0 when it is past its first STREAK_SKIP_S
 * and the count does not matter. */
static int first_rank(struct streak_acc *a, int src, long start, long t)
{
   if (start <= 0 || t < start || t >= start + STREAK_SKIP_S)
      return 0;
   int k    = -1;
   int idle = -1;
   for (int i = 0; i < STREAK_FIRST_N; i++) {
      if (a->first[i].start > 0 && a->first[i].src == src &&
          a->first[i].start == start)
         k = i;
      /* A slot whose sensor is past its first readings is free. */
      if (idle < 0 &&
          (a->first[i].start <= 0 || t >= a->first[i].start + STREAK_SKIP_S))
         idle = i;
   }
   if (k < 0) {
      k                 = idle >= 0 ? idle : 0;
      a->first[k].src   = src;
      a->first[k].start = start;
      a->first[k].n     = 0;
   }
   return ++a->first[k].n;
}

/* The current run, if any, is over: into the count and the total. */
static void run_end(struct streak_acc *a)
{
   if (!a->run_start)
      return;
   a->runs++;
   a->run_sum += a->run_t - a->run_start;
   a->run_start = 0;
}

long streak_acc_mean(const struct streak_acc *a)
{
   const long n   = a->runs + (a->run_start ? 1 : 0);
   const long sum = a->run_sum + (a->run_start ? a->run_t - a->run_start : 0);
   return n > 0 ? sum / n : -1;
}

int streak_acc_add(struct streak_acc *a, const struct streak_pt *x, long start,
                   const struct src_span *sp, int nsp)
{
   if (x->t < a->seen_t)
      return 0;
   if (!x->cgm)
      return 1;
   if (x->lead == READ_LEAD_UNKNOWN && !sp)
      return 0;
   a->seen_t = x->t;
   /* COUNTED BEFORE THE LEAD IS ASKED: a sensor's first readings are its
    * first whether or not it was the primary when they came. */
   const int rank = first_rank(a, x->src, start, x->t);
   if (!read_lead(x->lead, x->src, x->t, sp, nsp))
      return 1;
   if (rank > 0 && streak_first_points(start, x->t, rank))
      return 1;
   if (x->glu < TIR_LOW_MGDL || x->glu > TIR_HIGH_MGDL) {
      run_end(a);
      a->cur = 0;
      return 1;
   }
   if (a->run_start && x->t - a->run_t > STREAK_GAP_S)
      run_end(a);
   if (!a->run_start)
      a->run_start = x->t;
   a->run_t = x->t;
   a->cur   = x->t - a->run_start;
   if (a->cur > a->best)
      a->best = a->cur;
   return 1;
}

void streak_acc_build(struct streak_acc *a, const struct streak_pt *r, int n,
                      const struct src_span *sp, int nsp,
                      const struct sensor_start *s, int ns)
{
   streak_acc_init(a);
   for (int i = n - 1; i >= 0; i--)
      (void)streak_acc_add(a, &r[i], start_of(s, ns, r[i].src), sp, nsp);
}

void streak_len(long s, char *out, size_t cap)
{
   if (s < 0)
      s = 0;
   if (s >= 100L * 86400)
      (void)snprintf(out, cap, "%ldD", s / 86400);
   else if (s >= 86400)
      (void)snprintf(out, cap, "%ldD %ldH", s / 86400, (s % 86400) / 3600);
   else if (s >= 3600)
      (void)snprintf(out, cap, "%ld H", s / 3600);
   else
      (void)snprintf(out, cap, "%ld MIN", s / 60);
}

void streak_line(long cur, long best, char *out, size_t cap)
{
   if (cur > 365L * 86400)
      cur = 365L * 86400;
   if (best > 365L * 86400)
      best = 365L * 86400;
   /* IS THIS THE BEST ONE YET? Then it says so and nothing else: a record
    * being broken is the whole news. `>=` because equalling the record is
    * reaching it. */
   char c[8];
   char b[8];
   streak_len(cur, c, sizeof c);
   streak_len(best, b, sizeof b);
   if (best <= 0 || cur >= best)
      (void)snprintf(out, cap, "BEST STREAK %s", c);
   else
      (void)snprintf(out, cap, "STREAK %s (BEST %s)", c, b);
}
