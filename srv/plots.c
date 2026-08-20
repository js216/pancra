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
#include "plots.h"
#include "db.h"
#include "gif.h"
#include "http.h"
#include "plot.h"
#include "proto.h"
#include "rowdec.h"
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

/* ONE RENDER'S SCRATCH, ALL OF IT, IN ONE PLACE THE CALLER OWNS.
 *
 * These were four file-scope buffers -- framebuffer, palette image, points,
 * GIF output -- plus the LZW dictionary inside gif.c. Two renders at once
 * therefore drew into each other's pixels and compressed each other's
 * dictionary, and nothing in this file said so: the safety came from a mutex
 * in web.c that serialises PAGES, for an unrelated reason (see web_route).
 * A renderer whose correctness depends on a lock three modules away is one
 * refactor from being wrong, and the failure is an image that is merely
 * WRONG rather than an error.
 *
 * As a workspace the renderer is reentrant by construction: two callers with
 * two workspaces cannot interfere. It is ~1.2 MB, so it is not a stack
 * object -- h_plot_gif keeps the single instance the page lock already
 * serialises, and the tests make their own. */
struct plot_ws {
   uint32_t fbpx[(size_t)IMG_W * IMG_H];
   uint8_t img[(size_t)IMG_W * IMG_H];
   struct plot_pt ppts[PTS_MAX];
   struct gif_ws gw;
};

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
static void img_digits(struct plot_ws *ws, int x, int y, const char *s, int sc)
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
                     ws->img[((size_t)py * IMG_W) + (size_t)px] = 0;
               }
         }
   }
}

/* Readings in (t0, t1], oldest first, into ppts. Buckets are days, so the
 * query asks only for the days the window touches however long the record
 * is. */
static int load_points(struct plot_ws *ws, struct db *d, long owner, long t0,
                       long t1)
{
   sqlite3_stmt *st =
       db_prep(d, "SELECT line FROM logrow WHERE user_id=?"
                  " AND log='readings' AND bucket BETWEEN ? AND ?"
                  " ORDER BY bucket, line");
   /* A QUERY THAT WOULD NOT PREPARE IS NOT AN EMPTY WINDOW.
    *
    * This returned 0, which plot_gif read as "no readings in range" and
    * rendered: a 200 with a convincing, correctly-labelled, EMPTY graph.
    * Someone looking at a week of their own data saw a blank plot and had no
    * way to tell it from a week they had not worn the sensor. -1 is the same
    * answer a failed step gives, and the caller already turns it into a
    * 500. */
   if (!st)
      return -1;
   sqlite3_bind_int64(st, 1, owner);
   sqlite3_bind_int64(st, 2, (t0 / 86400) - 1);
   sqlite3_bind_int64(st, 3, (t1 / 86400) + 1);
   int n  = 0;
   int rc = SQLITE_DONE;
   while (n < PTS_MAX && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *ln = (const char *)sqlite3_column_text(st, 0);
      if (!ln)
         continue;
      /* THE SAME DECODER the pages use (rowdec.h). This walked commas by
       * hand and took whatever prefix strtol could find, so a corrupt field
       * was plotted as a number and a row that stopped early was plotted as
       * zeroes -- on a graph, where a reader has no way to tell. */
      struct row_reading rr;
      if (!row_decode(ln, (int)strlen(ln), &rr))
         continue;
      if (rr.t <= t0 || rr.t > t1)
         continue;
      ws->ppts[n].t      = rr.t;
      ws->ppts[n].glu    = (int)rr.glu;
      ws->ppts[n].marker = 0;
      ws->ppts[n].hidden = 0;
      ws->ppts[n].size   = 0;
      ws->ppts[n].col    = 0;
      n++;
   }
   /* A FULL BUFFER IS A LEGITIMATE END; a failed step is not. Ending early on
    * PTS_MAX means the window simply holds more points than the image can
    * show, which is a rendering limit. Ending on SQLITE_BUSY or a damaged
    * page means the plot would silently omit real readings -- so say the plot
    * cannot be drawn instead of drawing a convincing wrong one. */
   int ok = (n >= PTS_MAX) || db_finished(rc);
   sqlite3_finalize(st);
   if (!ok)
      return -1;
   /* oldest first by TIME: bucket order is arrival order, and a backfilled
    * row belongs where its timestamp says. */
   for (int i = 1; i < n; i++) {
      struct plot_pt tmp = ws->ppts[i];
      int j              = i - 1;
      while (j >= 0 && ws->ppts[j].t > tmp.t) {
         ws->ppts[j + 1] = ws->ppts[j];
         j--;
      }
      ws->ppts[j + 1] = tmp;
   }
   return n;
}

/* Render (win_start, win_end] over `hours` into a GIF. Returns its size. */
static size_t plot_gif(struct plot_ws *ws, struct db *d, long owner,
                       long win_start, long win_end, int hours, int tz_min,
                       uint8_t *out, size_t cap)
{
   int ph = IMG_H - XSTRIP; /* plot rect; labels live in the strip below */
   for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++)
      ws->fbpx[i] = 0xFF181818; /* the app's screen background */
   int np = load_points(ws, d, owner, win_start, win_end);
   if (np < 0)
      return 0; /* no image rather than one missing readings, or one that
                 * silently claims the window was empty */
   long tz = (long)tz_min * 60;
   /* THE PLOT'S CONFIGURATION, passed rather than set: the scale and the
    * marker radius were process globals, so two windows rendered at once
    * could not have different ones -- and this server renders several. */
   struct plot_cfg cfg = {PLOT_GLU_MAX, PLOT_PRAD};
   plot_render((struct plot_fb){ws->fbpx, IMG_W, IMG_W, IMG_H},
               (struct plot_rect){0, 0, IMG_W, ph}, ws->ppts, np, win_end,
               hours, cfg, trace_white, -1, 0, tz);

   /* invert luminance -> bright mode, 16 gray levels. ITS OWN, not a static:
    * the palette is written on every call, so a shared one is one more thing
    * two renders would be writing at once. It is 48 bytes. */
   uint8_t gray[16][3];
   for (int i = 0; i < 16; i++)
      gray[i][0] = gray[i][1] = gray[i][2] = (uint8_t)(i * 17);
   for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++) {
      uint32_t c = ws->fbpx[i]; /* 0xAABBGGRR: low byte is red */
      int lum    = (((int)(c & 0xFF) * 299) + ((int)((c >> 8) & 0xFF) * 587) +
                    ((int)((c >> 16) & 0xFF) * 114)) /
                   1000;
      ws->img[i] = (uint8_t)((255 - lum) >> 4);
   }

   /* Axis labels, through the renderer's OWN mapping (plot_point_xy) so they
    * can never drift from what plot_render drew. */
   int lx, ly;
   struct plot_pt ref = {.t = win_end, .glu = 70};
   if (plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end, hours,
                     cfg, &lx, &ly))
      img_digits(ws, 4, ly - 5, "70", 2);
   ref.glu = 180;
   if (plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end, hours,
                     cfg, &lx, &ly))
      img_digits(ws, 4, ly - 5, "180", 2);
   for (long ts = 3600; ts <= (long)hours * 3600; ts += 3600) {
      ref.t    = win_end - ts;
      ref.glu  = 100; /* any in-scale value: only x matters here */
      long loc = ref.t + ((long)tz_min * 60);
      int hh   = (int)((loc % 86400 + 86400) % 86400) / 3600;
      if (hh % 3)
         continue;
      if (!plot_point_xy((struct plot_rect){0, 0, IMG_W, ph}, ref, win_end,
                         hours, cfg, &lx, &ly))
         continue;
      char lbl[4];
      (void)snprintf(lbl, sizeof lbl, "%02d", hh);
      img_digits(ws, lx - 7, (IMG_H - XSTRIP) + 2, lbl, 2);
   }
   return gif_encode(&ws->gw, out, cap, ws->img, IMG_W, IMG_H, gray, 16);
}

/* THE ONE WORKSPACE THE SERVER RENDERS INTO, and the buffer it encodes to.
 *
 * Still shared, still serialised by the page lock -- but now that is a
 * STATEMENT rather than an assumption: the renderer takes a workspace, and
 * this is the instance this caller passes. A second renderer (another
 * caller, a test) brings its own and needs no lock at all. */
static struct plot_ws g_plot_ws;
static uint8_t gifbuf[256 * 1024];

void h_plot_gif(struct req *r, long owner, long win_start, long win_end,
                int hours, int tz_min)
{
   size_t n = plot_gif(&g_plot_ws, r->db, owner, win_start, win_end, hours,
                       tz_min, gifbuf, sizeof gifbuf);
   if (!n) {
      /* STAGED, not written here. http_text goes to the socket, and this runs
       * under the page lock -- so a client that stopped reading held every
       * other page for every other user while its error was written. Every
       * other exit from this function stages; this one used not to. */
      char *msg = malloc(sizeof "plot failed\n" - 1);
      if (msg) {
         memcpy(msg, "plot failed\n", sizeof "plot failed\n" - 1);
         r->resp        = msg;
         r->resp_n      = sizeof "plot failed\n" - 1;
         r->resp_code   = 500;
         r->resp_reason = "Internal Server Error";
         r->resp_ctype  = "text/plain";
      } else {
         /* Nothing to stage it in. A bodiless 500 is still a 500, and it
          * still goes out where every other response does. */
         r->resp        = NULL;
         r->resp_n      = 0;
         r->resp_code   = 500;
         r->resp_reason = "Internal Server Error";
         r->resp_ctype  = "text/plain";
      }
      return;
   }
   /* gifbuf is shared scratch, so unlike a page's heap buffer it cannot
    * outlive the lock. Copy it out: the copy exists only while this response
    * is in flight, which is far cheaper than a per-worker framebuffer and is
    * what lets the write happen without holding every other page up. */
   char *copy = malloc(n);
   if (!copy) {
      /* THE SAME RULE ON THE FAILING PATH. This wrote the image to the socket
       * from inside the lock -- at the peer's pace, with every other page
       * waiting on it -- which is the one thing the staging exists to avoid.
       * An out-of-memory 500 is staged like everything else. */
      r->resp        = NULL;
      r->resp_n      = 0;
      r->resp_code   = 500;
      r->resp_reason = "Internal Server Error";
      r->resp_ctype  = "image/gif";
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
int plot_days(struct db *d, long owner, long *out, int cap)
{
   sqlite3_stmt *st = db_prep(d, "SELECT DISTINCT bucket FROM logrow"
                                 " WHERE user_id=? AND log='readings'"
                                 " ORDER BY bucket DESC");
   if (!st)
      return 0;
   sqlite3_bind_int64(st, 1, owner);
   int n  = 0;
   int rc = SQLITE_DONE;
   while (n < cap && (rc = sqlite3_step(st)) == SQLITE_ROW)
      out[n++] = (long)sqlite3_column_int64(st, 0);
   int ok = (n >= cap) || db_finished(rc);
   sqlite3_finalize(st);
   return ok ? n : -1;
}
