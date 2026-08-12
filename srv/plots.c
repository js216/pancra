/* SPDX-License-Identifier: GPL-3.0
 * plots.c --- the plot pages, drawn by the APP'S OWN renderer
 * Copyright 2026 Jakob Kastelic
 *
 * plot_render (lib/plot.c) is the exact code the phone draws with --
 * self-contained by design, so this server links it directly and the two
 * plots can never drift apart. It renders at the app's dark palette into an
 * RGBA framebuffer; this file then inverts luminance per pixel, which turns
 * the dark theme into the bright black-and-white look the pages want, and
 * quantises to 16 grays for the GIF.
 *
 * Its own file, not more of web.c: this is a renderer with a framebuffer, a
 * palette and an encoder, and web.c is a page builder. They share nothing but
 * the record.
 */
#include "db.h"
#include "gif.h"
#include "http.h"
#include "plot.h"
#include "sync.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMG_W  720
#define IMG_H  300
#define XSTRIP 14 /* bottom strip below the plot, for the time labels */
#define PLOT_PRAD                                                              \
   3 /* smaller than the app's: at this width the columns are ~10 px apart and \
        radius 3 keeps adjacent markers separated */
#define PTS_MAX 20000

static uint32_t fbpx[(size_t)IMG_W * IMG_H];
static uint8_t img[(size_t)IMG_W * IMG_H];
static struct plot_pt ppts[PTS_MAX];

static uint32_t trace_white(int g)
{
   (void)g;
   return 0xFFFFFFFF; /* the app's main-trace dot colour */
}

/* 3x5 digits (rows top-down, bit 2 = leftmost) for the axis labels. */
static const uint8_t dig3x5[10][5] = {
    {7, 5, 5, 5, 7},
    {2, 6, 2, 2, 7},
    {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7},
    {7, 1, 2, 2, 2},
    {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7}
};
/* '-' is not a digit, but a date label needs one ("04-22" reads as a date,
 * "04 22" reads as two numbers), so it gets the one extra glyph. */
static const uint8_t dash3x5[5] = {0, 0, 7, 0, 0};

/* Draw digit string s onto the INDEXED (post-inversion) image in black
 * (palette index 0), scale sc; width is 4*sc per digit. Clipped. */
static void img_digits(int x, int y, const char *s, int sc)
{
   for (; *s; s++, x += 4 * sc) {
      if (*s != '-' && (*s < '0' || *s > '9'))
         continue;
      const uint8_t *g = *s == '-' ? dash3x5 : dig3x5[*s - '0'];
      for (int r = 0; r < 5; r++)
         for (int c = 0; c < 3; c++) {
            if (!(g[r] & (4 >> c)))
               continue;
            for (int j = 0; j < sc; j++)
               for (int i = 0; i < sc; i++) {
                  int px = x + (c * sc) + i;
                  int py = y + (r * sc) + j;
                  if (px >= 0 && px < IMG_W && py >= 0 && py < IMG_H)
                     img[((size_t)py * IMG_W) + (size_t)px] = 0;
               }
         }
   }
}

/* Readings in (t0, t1], oldest first, into ppts. Buckets are days, so the
 * query asks only for the days the window touches however long the record
 * is. */
static int load_points(long owner, long t0, long t1)
{
   sqlite3_stmt *st = db_prep("SELECT line FROM logrow WHERE user_id=?"
                              " AND log='readings' AND bucket BETWEEN ? AND ?"
                              " ORDER BY bucket, line");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   sqlite3_bind_int64(st, 2, (t0 / 86400) - 1);
   sqlite3_bind_int64(st, 3, (t1 / 86400) + 1);
   int n = 0;
   while (n < PTS_MAX && sqlite3_step(st) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln || *ln < '0' || *ln > '9')
         continue;
      long t = strtol(ln, NULL, 10);
      if (t <= t0 || t > t1)
         continue;
      const char *c1 = strchr(ln, ',');
      if (!c1)
         continue;
      ppts[n].t      = t;
      ppts[n].glu    = (int)strtol(c1 + 1, NULL, 10);
      ppts[n].marker = 0;
      ppts[n].hidden = 0;
      ppts[n].size   = 0;
      ppts[n].col    = 0;
      n++;
   }
   sqlite3_finalize(st);
   /* oldest first by TIME: bucket order is arrival order, and a backfilled
    * row belongs where its timestamp says. */
   for (int i = 1; i < n; i++) {
      struct plot_pt tmp = ppts[i];
      int j              = i - 1;
      while (j >= 0 && ppts[j].t > tmp.t) {
         ppts[j + 1] = ppts[j];
         j--;
      }
      ppts[j + 1] = tmp;
   }
   return n;
}

/* Render (win_start, win_end] over `hours` into a GIF. Returns its size. */
static size_t plot_gif(long owner, long win_start, long win_end, int hours,
                       int tz_min, uint8_t *out, size_t cap)
{
   int ph = IMG_H - XSTRIP; /* plot rect; labels live in the strip below */
   for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++)
      fbpx[i] = 0xFF181818; /* the app's screen background */
   int np  = load_points(owner, win_start, win_end);
   long tz = (long)tz_min * 60;
   plot_render((struct plot_fb){fbpx, IMG_W, IMG_W, IMG_H},
               (struct plot_rect){0, 0, IMG_W, ph}, ppts, np, win_end, hours,
               PLOT_PRAD, trace_white, -1, 0, tz);

   /* invert luminance -> bright mode, 16 gray levels */
   static uint8_t gray[16][3];
   for (int i = 0; i < 16; i++)
      gray[i][0] = gray[i][1] = gray[i][2] = (uint8_t)(i * 17);
   for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++) {
      uint32_t c = fbpx[i]; /* 0xAABBGGRR: low byte is red */
      int lum    = (((int)(c & 0xFF) * 299) + ((int)((c >> 8) & 0xFF) * 587) +
                    ((int)((c >> 16) & 0xFF) * 114)) /
                   1000;
      img[i]     = (uint8_t)((255 - lum) >> 4);
   }

   /* Axis labels, through the renderer's OWN mapping (plot_point_xy) so they
    * can never drift from what plot_render drew. */
   int lx, ly;
   struct plot_pt ref = {.t = win_end, .glu = 70};
   if (plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end, hours,
                     &lx, &ly))
      img_digits(4, ly - 5, "70", 2);
   ref.glu = 180;
   if (plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end, hours,
                     &lx, &ly))
      img_digits(4, ly - 5, "180", 2);
   for (long ts = 3600; ts <= (long)hours * 3600; ts += 3600) {
      ref.t    = win_end - ts;
      ref.glu  = 100; /* any in-scale value: only x matters here */
      long loc = ref.t + ((long)tz_min * 60);
      int hh   = (int)((loc % 86400 + 86400) % 86400) / 3600;
      if (hh % 3)
         continue;
      if (!plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end,
                         hours, &lx, &ly))
         continue;
      char lbl[4];
      (void)snprintf(lbl, sizeof lbl, "%02d", hh);
      img_digits(lx - 7, (IMG_H - XSTRIP) + 2, lbl, 2);
   }
   return gif_encode(out, cap, img, IMG_W, IMG_H, gray, 16);
}

static uint8_t gifbuf[256 * 1024];

void h_plot_gif(struct req *r, long owner, long win_start, long win_end,
                int hours, int tz_min)
{
   size_t n = plot_gif(owner, win_start, win_end, hours, tz_min, gifbuf,
                       sizeof gifbuf);
   if (!n) {
      http_text(r->fd, 500, "Internal Server Error", "plot failed\n");
      return;
   }
   /* gifbuf is shared scratch, so unlike a page's heap buffer it cannot
    * outlive the lock. Copy it out: the copy exists only while this response
    * is in flight, which is far cheaper than a per-worker framebuffer and is
    * what lets the write happen without holding every other page up. */
   char *copy = malloc(n);
   if (!copy) {
      http_respond(r->fd, 200, "OK", "image/gif", gifbuf, n);
      return;
   }
   memcpy(copy, gifbuf, n);
   r->resp        = copy;
   r->resp_n      = n;
   r->resp_code   = 200;
   r->resp_reason = "OK";
   r->resp_ctype  = "image/gif";
}

/* Days that hold data, newest first: one row per local day, from the buckets
 * the record actually has. Buckets ARE days, so this is an index scan and
 * never a walk of the rows. */
int plot_days(long owner, long *out, int cap)
{
   sqlite3_stmt *st = db_prep("SELECT DISTINCT bucket FROM logrow"
                              " WHERE user_id=? AND log='readings'"
                              " ORDER BY bucket DESC");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   int n = 0;
   while (n < cap && sqlite3_step(st) == SQLITE_ROW)
      out[n++] = (long)sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return n;
}
