// SPDX-License-Identifier: GPL-3.0
// plottest.c --- the long-span plot data: nothing visible may be dropped
// Copyright 2026 Jakob Kastelic

/* The 30D plot is drawn from the LOG, downsampled, because tying its depth
 * to a point budget meant it silently shrank as sources were added -- the
 * bug that had a 30-day plot showing ten days.
 *
 * Downsampling is where such a plot lies quietly, so this pins the property
 * that matters: every reading that would occupy its OWN pixel survives. Two
 * readings that would paint the same pixel are indistinguishable on screen,
 * so keeping one is not a loss; anything more IS.
 */
#include "plot.h"     /* the renderer itself: its geometry boundary */
#include "plotdata.h"
#include "sensors.h" /* KIND_CGM / KIND_BGM */
#include "testdir.h" /* test_path: the per-mode fixture directory */
#include "uimodel.h" /* struct ui_point, in full */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail;
/* COUNTED, because a suite that runs no assertions passes. Printed at the
 * end, so "it passed" and "it checked something" are two visible facts. */
static int nck;

static void ck(int cond, const char *what)
{
   nck++;
   if (cond) {
      printf("  ok   %s\n", what);
      return;
   }
   printf("  FAIL %s\n", what);
   fail = 1;
}

/* Write a log in ARRIVAL order: recent rows first, then imported history
 * appended after them -- the shape the phone's file actually has. */
static void write_log(const char *path, long now, int recent, int imported)
{
   FILE *f = fopen(path, "w");
   if (!f)
      exit(2);
   fprintf(f, "# unix_time,glucose_mgdl,trend,rssi,recv_lag_s,sensor_id,"
              "device_time,tz_offset_s,kind,rescale\n");
   for (int i = 0; i < recent; i++) {
      long t = now - ((long)(recent - i) * 300);
      fprintf(f, "%ld,%d,3,-70,4,7,%ld,-25200,0,\n", t, 100 + (i % 80), t);
   }
   for (int i = 0; i < imported; i++) {
      long t = now - (95L * 86400) + ((long)i * 270);
      fprintf(f, "%ld,%d,127,,0,9,%ld,-25200,0,\n", t, 90 + (i % 90), t);
   }
   fclose(f);
}

/* ================= THE FRAMEBUFFER GEOMETRY (TODO item 142) =============
 *
 * WHAT WAS WRONG. plot.c wrote every pixel at `fb[(y * stride) + x]` after a
 * clip test that bounded x and y against the buffer's own width and height
 * and bounded the STRIDE not at all. The stride, the width and the height are
 * whatever the caller put in the struct, so the product was a signed int
 * multiply over three unchecked numbers -- and all three ways of getting it
 * wrong are an out-of-bounds WRITE, not a wrong picture:
 *
 *   - a NEGATIVE stride steps the rows backwards off the front of the
 *     allocation; every row but the first is outside it.
 *   - a stride SMALLER than the width overlaps the rows and runs the last one
 *     past the end. This is the one that hides: the first rows draw
 *     perfectly, so it looks like a working plot right up to the bottom.
 *   - dimensions whose PRODUCT overflows int wrap to a small or negative
 *     offset, which then passes any bound tested on the wrapped value.
 *
 * SO THE CASES BELOW ARE THE THREE SHAPES OF LIE, NOT "BAD INPUT". A
 * framebuffer that is simply too small is refused by any check at all; these
 * are the ones that a naive check waves through.
 *
 * EVERY REFUSAL IS CHECKED WITH A SENTINEL, not by the return value alone.
 * The arena is filled with 0xA5A5A5A5 first and asserted word-for-word
 * unchanged afterwards, because "refused" has to mean refused BEFORE the
 * first pixel -- a renderer that paints the frame and then returns looks
 * exactly like one that refused if you only read the verdict. The pixels
 * pointer sits in the MIDDLE of the arena, so a backwards write from a
 * negative stride lands in the sentinel where it can be seen rather than
 * outside the object where only a sanitizer could see it.
 *
 * AND THEN THE SAME REFUSALS AGAIN ON EXACTLY-SIZED HEAP BUFFERS, where the
 * write that a removed check permits IS outside the allocation. That pass
 * says the same thing to AddressSanitizer (make appasan), so the checks are
 * pinned by an assertion and by a tool that does not read this file.
 *
 * THE CORNERS THAT CANNOT BE DRIVEN ARE PINNED BY THE PREDICATE. A buffer
 * claiming 100000 x 100000 is 40 GB; no test allocates it, so the rule is
 * plot_fb_check, a pure function of four ints, asked directly -- the same
 * split gif_dims_ok makes for four-gigapixel GIFs in giftest.c.
 *
 * AND A CONTROL, because a boundary that refuses everything also passes every
 * case above: the geometries the app and the server really draw are rendered
 * and their pixels hashed. The hashes below were taken from the renderer as
 * it was BEFORE this boundary existed -- so they fail if the validation moves
 * a single pixel of a legitimate plot, which is the regression nobody would
 * notice by eye. */
#define GEO_SENT  0xA5A5A5A5u
#define GEO_ARENA (256 * 256)
static uint32_t g_arena[GEO_ARENA];
/* The app's notification surface, for the byte-for-byte control below. */
static uint32_t g_big[512 * 232];

static void arena_fill(void)
{
   for (long i = 0; i < GEO_ARENA; i++)
      g_arena[i] = GEO_SENT;
}

static long arena_dirty(void)
{
   long n = 0;
   for (long i = 0; i < GEO_ARENA; i++)
      if (g_arena[i] != GEO_SENT)
         n++;
   return n;
}

/* The middle of the arena, so a negative stride writes backwards INTO the
 * sentinel rather than off the end of the object. */
static uint32_t *arena_mid(void)
{
   return &g_arena[GEO_ARENA / 2];
}

static uint32_t geo_white(int glu)
{
   (void)glu;
   return 0xFFFFFFFFu;
}

static uint32_t geo_bycol(int glu)
{
   return glu < 70 ? 0xFFFF0000u : (glu > 180 ? 0xFFFFFF00u : 0xFF00FF00u);
}

/* FNV-1a over the PIXEL VALUES, not their bytes, so the digest does not
 * depend on the host's endianness -- this is a claim about what was drawn. */
static unsigned long geo_hash(const uint32_t *px, long n)
{
   unsigned long h = 1469598103934665603UL;
   for (long i = 0; i < n; i++) {
      h ^= (unsigned long)px[i];
      h *= 1099511628211UL;
   }
   return h;
}

static struct plot_pt g_gpts[600];

static int geo_points(long now)
{
   int n = (int)(sizeof g_gpts / sizeof g_gpts[0]);
   for (int i = 0; i < n; i++) {
      g_gpts[i].t   = now - ((long)i * 300);
      g_gpts[i].glu = 60 + ((i * 37) % 260); /* below the floor and above the
                                              * ceiling, so the capping rule
                                              * is drawn too */
      g_gpts[i].marker = i % 9;              /* every shape */
      g_gpts[i].hidden = (i % 53) == 0;
      g_gpts[i].size   = (i % 5) + 1; /* every per-device marker size */
      g_gpts[i].col    = (i % 7) ? 0 : 0xFFFF00FFu;
   }
   return n;
}

/* One refusal: the render must return without touching the arena. `why` is
 * the outcome the boundary is expected to report, so a rule that refuses for
 * the wrong reason is a failure rather than an accident that passes. */
static void geo_refuse_render(struct plot_fb fb, struct plot_rect r,
                              struct plot_cfg cfg, enum plot_geom why,
                              const char *what)
{
   long now = 1785272930;
   int n    = geo_points(now);
   char msg[160];
   enum plot_geom got = plot_render_check(fb, r, cfg);
   (void)snprintf(msg, sizeof msg, "%s is refused (%d)", what, (int)why);
   ck(got == why, msg);
   arena_fill();
   plot_render(fb, r, g_gpts, n, now, 24, cfg, geo_white, -1, 0, 0);
   (void)snprintf(msg, sizeof msg, "%s writes NO pixel", what);
   ck(arena_dirty() == 0, msg);
}

static void plot_geometry_tests(void)
{
   long now = 1785272930;
   int n    = geo_points(now);

   printf("== a framebuffer's geometry is checked before any pixel ==\n");
   /* THE CONTROL FIRST. A legitimate buffer with a stride WIDER than its
    * width -- the ordinary case on a phone surface -- must draw, and must
    * draw only inside its own w x h window. Without this the refusals below
    * would be satisfied by a renderer that draws nothing at all. */
   {
      struct plot_fb fb = {g_arena, 256, 128, 128};
      arena_fill();
      plot_render(fb, (struct plot_rect){0, 0, 128, 128}, g_gpts, n, now, 24,
                  (struct plot_cfg){300, 2}, geo_white, -1, 0, 0);
      long inside  = 0;
      long outside = 0;
      for (long i = 0; i < GEO_ARENA; i++) {
         if (g_arena[i] == GEO_SENT)
            continue;
         if ((i / 256) < 128 && (i % 256) < 128)
            inside++;
         else
            outside++;
      }
      printf("       %ld pixels drawn inside the window, %ld outside\n", inside,
             outside);
      ck(plot_fb_check(fb) == PLOT_GEOM_OK, "a stride wider than the width is "
                                            "the ordinary case, and is fine");
      ck(inside > 1000, "a legitimate plot still draws");
      ck(outside == 0, "...and every pixel of it lands inside its own w x h");
   }

   /* A NEGATIVE STRIDE. The clip test bounds x and y, so every one of these
    * pixels "is inside the buffer" by the only test that used to exist; the
    * offset they are written at is not. */
   {
      struct plot_fb fb = {arena_mid(), -128, 128, 128};
      ck(plot_fb_check(fb) == PLOT_GEOM_STRIDE, "a negative stride is refused");
      geo_refuse_render(fb, (struct plot_rect){0, 0, 128, 128},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_STRIDE,
                        "a negative stride");
      struct plot_fb fb0 = {arena_mid(), 0, 128, 128};
      ck(plot_fb_check(fb0) == PLOT_GEOM_STRIDE,
         "a zero stride is refused too: every row would be row 0");
   }

   /* A STRIDE NARROWER THAN THE WIDTH -- the quiet one. Rows 0..15 of this
    * buffer land perfectly; it is row 63 that runs 31 pixels past the end. */
   {
      struct plot_fb fb = {arena_mid(), 32, 64, 64};
      ck(plot_fb_check(fb) == PLOT_GEOM_STRIDE,
         "a stride narrower than the width is refused");
      geo_refuse_render(fb, (struct plot_rect){0, 0, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_STRIDE,
                        "a stride narrower than the width");
      struct plot_fb eq = {arena_mid(), 64, 64, 64};
      ck(plot_fb_check(eq) == PLOT_GEOM_OK,
         "...and stride == width, one pixel from it, is legitimate");
   }

   /* DIMENSIONS WHOSE PRODUCT OVERFLOWS. Pinned by the predicate, because the
    * buffer these describe cannot be allocated -- and pinned as a PAIR either
    * side of INT_MAX with the same height, which is what tells a checked
    * multiply apart from a ceiling somebody chose: stride 2 x 1073741823 is
    * 2147483646 and legal, stride 4 x the same height is 4294967292 and is
    * not. A ceiling low enough to catch the second would refuse the first. */
   {
      struct plot_fb ok  = {g_arena, 2, 2, 1073741823};
      struct plot_fb bad = {g_arena, 4, 4, 1073741823};
      ck(plot_fb_check(ok) == PLOT_GEOM_OK,
         "2 x 1073741823 pixels is 2147483646: inside int, and accepted");
      ck(plot_fb_check(bad) == PLOT_GEOM_OVERFLOW,
         "4 x 1073741823 pixels is 4294967292: refused by the checked "
         "multiply");
      struct plot_fb huge = {g_arena, 100000, 100000, 100000};
      ck(plot_fb_check(huge) == PLOT_GEOM_OVERFLOW,
         "100000 x 100000 -- 10^10 pixels, which wraps to 1410065408 -- is "
         "refused");
   }

   /* IMPOSSIBLE DIMENSIONS AND NO PIXELS AT ALL. */
   {
      struct plot_fb nopx = {NULL, 128, 128, 128};
      struct plot_fb negw = {g_arena, 128, -1, 128};
      struct plot_fb negh = {g_arena, 128, 128, -1};
      ck(plot_fb_check(nopx) == PLOT_GEOM_PIXELS,
         "a framebuffer with no pixels is refused");
      ck(plot_fb_check(negw) == PLOT_GEOM_SIZE, "a negative width is refused");
      ck(plot_fb_check(negh) == PLOT_GEOM_SIZE, "a negative height is refused");
      /* A negative width converted to size_t FIRST is 2^64 - 1, which is
       * larger than any stride -- so a "stride >= width" test made on the
       * converted values refuses it for the wrong reason, and a "width <=
       * buffer" test made the same way lets it through. The sign is tested
       * while these are still ints, which is what this pins. */
      struct plot_fb negwide = {g_arena, INT_MAX, -1, 1};
      ck(plot_fb_check(negwide) == PLOT_GEOM_SIZE,
         "a negative width is refused as a SIZE, not mistaken for a huge one");
   }

   printf("== an impossible rectangle is refused, not clipped ==\n");
   {
      struct plot_fb fb = {arena_mid(), 128, 128, 128};
      ck(plot_render_check(fb, (struct plot_rect){0, 0, 128, 128},
                           (struct plot_cfg){300, 2}) == PLOT_GEOM_OK,
         "the rectangle that exactly fills the buffer is fine");
      geo_refuse_render(fb, (struct plot_rect){-4, 0, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle starting left of the buffer");
      geo_refuse_render(fb, (struct plot_rect){0, -4, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle starting above the buffer");
      geo_refuse_render(fb, (struct plot_rect){100, 0, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle running past the right edge");
      geo_refuse_render(fb, (struct plot_rect){0, 100, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle running past the bottom");
      geo_refuse_render(fb, (struct plot_rect){0, 0, -64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a negative rectangle width");
      geo_refuse_render(fb, (struct plot_rect){0, 0, 64, -64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a negative rectangle height");
      geo_refuse_render(fb, (struct plot_rect){0, 0, 3, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle too narrow to hold its own frame");
      /* x + w AS AN ADDITION THAT CAN OVERFLOW. In int this wraps negative
       * and reads as "well inside the buffer"; the sum is formed in size_t
       * only after both terms are known non-negative. */
      geo_refuse_render(fb, (struct plot_rect){INT_MAX - 2, 0, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle whose x + w overflows an int");
      geo_refuse_render(fb, (struct plot_rect){0, INT_MAX - 2, 64, 64},
                        (struct plot_cfg){300, 2}, PLOT_GEOM_RECT,
                        "a rectangle whose y + h overflows an int");
   }

   printf("== an impossible radius is refused ==\n");
   {
      struct plot_fb fb = {arena_mid(), 128, 128, 128};
      geo_refuse_render(fb, (struct plot_rect){0, 0, 128, 128},
                        (struct plot_cfg){300, -1}, PLOT_GEOM_RADIUS,
                        "a negative marker radius");
      geo_refuse_render(fb, (struct plot_rect){0, 0, 128, 128},
                        (struct plot_cfg){300, 129}, PLOT_GEOM_RADIUS,
                        "a marker radius larger than the buffer");
      /* NOT refused: plot.h documents a zeroed configuration as "the
       * default", and cfg_radius turns 0 into 1 for the render AND the hit
       * test. Refusing it would blank a plot that draws correctly today. */
      ck(plot_render_check(fb, (struct plot_rect){0, 0, 128, 128},
                           (struct plot_cfg){0, 0}) == PLOT_GEOM_OK,
         "a zeroed configuration still means 'the default', not a refusal");

      /* The free-standing glyph has the same rules and one of its own: the
       * centre may be anywhere, including off the edge, but cx +/- r must
       * stay inside an int -- the step is arithmetic long before the clip
       * test can throw the pixel away. */
      ck(plot_glyph_check(fb, 60, 60, -1) == PLOT_GEOM_RADIUS,
         "a glyph with a negative radius is refused");
      ck(plot_glyph_check(fb, 60, 60, 129) == PLOT_GEOM_RADIUS,
         "a glyph radius larger than the buffer is refused");
      ck(plot_glyph_check(fb, INT_MAX - 1, 60, 4) == PLOT_GEOM_RECT,
         "a glyph centre whose cx + r overflows an int is refused");
      ck(plot_glyph_check(fb, 60, INT_MIN + 1, 4) == PLOT_GEOM_RECT,
         "...and so is one whose cy - r does");
      ck(plot_glyph_check(fb, -20, -20, 8) == PLOT_GEOM_OK,
         "a glyph half off the edge is still drawn, clipped, as it always "
         "was");
      arena_fill();
      plot_marker_glyph(fb, 60, 60, -1, 0, 0xFFFFFFFFu);
      plot_marker_glyph(fb, 60, 60, 129, 7, 0xFFFFFFFFu);
      plot_marker_glyph((struct plot_fb){arena_mid(), -128, 128, 128}, 60, 60,
                        4, 0, 0xFFFFFFFFu);
      ck(arena_dirty() == 0, "a refused glyph writes NO pixel either");
      arena_fill();
      plot_marker_glyph(fb, 60, 60, 6, 8, 0xFFFFFFFFu);
      ck(arena_dirty() > 0, "...while a legitimate one still draws");
   }

   /* THE SAME TWO REFUSALS ON EXACTLY-SIZED HEAP BUFFERS.
    *
    * The arena says "no pixel was written"; these say "and the pixel a
    * missing check would have written is outside the allocation", which is
    * the sentence AddressSanitizer reads. Under `make appasan` a build with
    * either stride check removed reports a heap overflow (or underflow) here
    * without any assertion being involved. */
   {
      uint32_t *heap = malloc((size_t)32 * 64 * sizeof *heap);
      if (!heap)
         exit(2);
      struct plot_fb narrow = {heap, 32, 64, 64};
      plot_render(narrow, (struct plot_rect){0, 0, 64, 64}, g_gpts, n, now, 24,
                  (struct plot_cfg){300, 2}, geo_white, -1, 0, 0);
      ck(plot_fb_check(narrow) == PLOT_GEOM_STRIDE,
         "on a buffer that is exactly as big as it claims, the narrow stride "
         "is still refused");
      free(heap);
      heap = malloc((size_t)64 * 64 * sizeof *heap);
      if (!heap)
         exit(2);
      struct plot_fb back = {heap, -64, 64, 64};
      plot_render(back, (struct plot_rect){0, 0, 64, 64}, g_gpts, n, now, 24,
                  (struct plot_cfg){300, 2}, geo_white, -1, 0, 0);
      ck(plot_fb_check(back) == PLOT_GEOM_STRIDE,
         "...and so is the negative one, whose rows would run off the front");
      free(heap);
   }

   printf("== and every legitimate plot draws exactly what it drew ==\n");
   /* THE REGRESSION THIS CANNOT HAVE. Two scenes the app really renders --
    * the notification's 512x232 surface, and the marker previews the menus
    * draw -- hashed pixel by pixel. The two constants were taken from the
    * renderer BEFORE the geometry boundary was added to it, so they say the
    * validation moved nothing: not a marker, not a gridline, not a clipped
    * edge. The scene deliberately includes readings above and below the
    * scale, every marker shape, every per-device size, a highlighted point
    * and glyphs half off two edges. */
   {
      for (long i = 0; i < 512 * 232; i++)
         g_big[i] = 0xFF000000u;
      plot_render((struct plot_fb){g_big, 512, 512, 232},
                  (struct plot_rect){0, 0, 512, 232}, g_gpts, n, now, 3,
                  (struct plot_cfg){260, 3}, geo_bycol, 4, 0xFF00FFFFu, -25200);
      unsigned long h1 = geo_hash(g_big, 512 * 232);
      printf("       notification scene %016lx\n", h1);
      ck(h1 == 0x9e725221243718abUL,
         "the notification plot is pixel for pixel what it was");

      for (long i = 0; i < 512 * 232; i++)
         g_big[i] = 0u;
      for (int sh = 0; sh <= 8; sh++)
         for (int r = 1; r <= 9; r++)
            plot_marker_glyph((struct plot_fb){g_big, 512, 512, 232},
                              30 + (sh * 50), 20 + (r * 20), r, sh,
                              0xFF00FF00u + (uint32_t)sh);
      plot_marker_glyph((struct plot_fb){g_big, 512, 512, 232}, 480, 200, 6,
                        PLOT_MARK_W, 0xFFFFFFFFu);
      plot_marker_glyph((struct plot_fb){g_big, 512, 512, 232}, 2, 2, 8, 0,
                        0xFFFFFFFFu);
      plot_marker_glyph((struct plot_fb){g_big, 512, 512, 232}, 510, 230, 8, 7,
                        0xFFFFFFFFu);
      unsigned long h2 = geo_hash(g_big, 512 * 232);
      printf("       glyph scene        %016lx\n", h2);
      ck(h2 == 0x92de382bbd510577UL,
         "every marker glyph, including the clipped ones, is unchanged");
   }
}

int main(void)
{
   char pbuf[160];
   const char *path = test_path(pbuf, sizeof pbuf, "plot-readings.csv");
   long now         = 1785272930;
   write_log(path, now, 5000, 30000);

   printf("== a short span defers to the live buffer ==\n");
   int n                    = -1;
   const struct ui_point *p = plot_source_from(path, now, 3, &n);
   ck(p == NULL, "3H returns NULL so the caller uses g_hist");

   printf("== a long span is served from the log ==\n");
   p = plot_source_from(path, now, 24 * 30, &n);
   ck(p != NULL && n > 0, "30D returns points");
   if (!p) {
      printf("plottest: FAIL (no points to check)\n");
      return 1;
   }

   printf("== and it reaches back the whole span ==\n");
   long oldest = now;
   long newest = 0;
   for (int i = 0; i < n; i++) {
      if (p[i].t < oldest)
         oldest = p[i].t;
      if (p[i].t > newest)
         newest = p[i].t;
   }
   double covered = (double)(newest - oldest) / 86400.0;
   printf("       covers %.1f of 30 days, %d points\n", covered, n);
   ck(covered > 29.0, "the 30D window is filled, not just its right-hand end");

   printf("== the LEFT of the window is populated, not just the right ==\n");
   {
      /* The log is in ARRIVAL order with the imported history LAST, so a
       * reader that stops when its buffer fills never reaches the old rows
       * and leaves the left of the plot empty -- which is exactly what a
       * 30-day plot did. Count points in each half of the window. */
      long span = 30L * 86400;
      long mid  = now - (span / 2);
      int left  = 0;
      int right = 0;
      for (int i = 0; i < n; i++) {
         if (p[i].t < mid)
            left++;
         else
            right++;
      }
      printf("       %d points in the older half, %d in the newer\n", left,
             right);
      ck(left > 1000, "the older half of the span is drawn");
   }

   printf("== nothing VISIBLE is dropped ==\n");
   /* Re-read the log and check that every row inside the window shares a
    * (column, value) cell with some point that survived. That is exactly
    * "no reading the screen could have distinguished is missing". */
   {
      static unsigned char seen[(768 * 512) / 8];
      long span = 30L * 86400;
      long from = now - span;
      for (int i = 0; i < n; i++) {
         int col = (int)(((p[i].t - from) * 767) / span);
         unsigned long cell =
             ((unsigned long)col * 512) + (unsigned long)p[i].glu;
         seen[cell >> 3U] |= (unsigned char)(1U << (cell & 7U));
      }
      FILE *f = fopen(path, "r");
      if (!f)
         return 2;
      char ln[256];
      long missing = 0;
      long inwin   = 0;
      while (fgets(ln, sizeof ln, f)) {
         long t   = 0;
         int glu  = 0;
         int src  = 0;
         int kind = 0;
         if (!plot_store_row(ln, &t, &glu, &src, &kind))
            continue;
         if (t <= from || t > now || glu >= 512)
            continue;
         inwin++;
         int col            = (int)(((t - from) * 767) / span);
         unsigned long cell = ((unsigned long)col * 512) + (unsigned long)glu;
         if (!(seen[cell >> 3U] & (unsigned char)(1U << (cell & 7U))))
            missing++;
      }
      fclose(f);
      printf("       %ld rows in window, %ld with no pixel drawn\n", inwin,
             missing);
      ck(missing == 0, "every distinguishable reading is represented");
   }

   printf("== memory is bounded by the SCREEN, not the history ==\n");
   {
      /* Ten times the readings must not produce ten times the points. */
      write_log(path, now, 5000, 300000);
      int n2 = 0;
      (void)plot_source_from(path, now + 1, 24 * 30, &n2);
      long span2               = 30L * 86400;
      long mid2                = now - (span2 / 2);
      int left2                = 0;
      const struct ui_point *q = plot_source_from(path, now + 1, 24 * 30, &n2);
      for (int i = 0; i < n2; i++)
         if (q[i].t < mid2)
            left2++;
      printf("       10x the log -> %d points (was %d), %d in the older half\n",
             n2, n, left2);
      ck(left2 > 1000, "...and a log that overflows the buffer still draws "
                       "the older half");
      ck(n2 <= 768 * 40, "the point count stays inside the fixed buffer");
   }

   /* KIND IS NORMALISED, not taken from the file.
    *
    * plot_store_row is the SECOND reader of readings.csv (hist_insert is the
    * other, which had the same gap). Its return guard bounds t and glu and
    * used to let any digit run through as a kind: a fuzz of it accepted 9,
    * 15, 149, 363 and 2312. That kind is copied into the ui_point handed to
    * the long-span plot, and the renderer draws kind == KIND_INS along the
    * bottom
    * edge -- so a corrupt 2 renders as an insulin dose that never happened,
    * and anything else is not KIND_BGM so it draws as a CGM line vertex.
    * readings.csv is append-only, so one bad row is redrawn at every launch. */
   {
      long t   = 0;
      int glu  = 0;
      int src  = 0;
      int kind = 0;
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "a KIND_INS row parses with kind normalised to KIND_CGM");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2312,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "an out-of-range kind becomes KIND_CGM");
      /* A real fingerstick must survive: forcing everything to CGM would
       * silently redraw every meter reading as a line vertex. */
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,1,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_BGM,
         "KIND_BGM is preserved");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,0,\n", &t, &glu, &src,
                        &kind) == 1 &&
             kind == KIND_CGM,
         "KIND_CGM is preserved");
   }

   /* EVERY COLUMN IS BOUNDED BEFORE IT IS ACCUMULATED, AND BOUNDED ON THE
    * WIDE SIDE OF THE CAST.
    *
    * Two separate defects lived in the loop that reads the columns after
    * glucose, and only the first is the one the eye goes to:
    *
    *   - it accumulated `n = n * 10 + digit` with NO cap, so a long digit run
    *     in ANY column -- including the ones this function discards -- was
    *     signed overflow, i.e. undefined behaviour, during PARSING. Not a
    *     wrong number: undefined behaviour at -O2 in the translation unit the
    *     30D and 90D glucose plots are drawn from. readings.csv is
    *     append-only, so a torn write or a hand-edit reaches it from a file.
    *
    *   - the checks that DID exist ran downstream of the wrap, which makes
    *     them not checks. `4294967297` wraps to exactly 1 in an int. In the
    *     kind column that is KIND_BGM, so the normalisation immediately above
    *     -- which exists precisely to stop a corrupt kind -- passed it, and
    *     the row drew as a fingerstick nobody took. In the source column it
    *     is device slot 1, so the point took a real device's colour and name.
    *     Both were measured on the shipped parser before this block existed:
    *     src came back 1, kind came back 1.
    *
    * So the isolating inputs below are not "obvious rubbish" -- rubbish is
    * refused by any check at all. They are the inputs that land, after the
    * wrap, on a perfectly legal value. */
   printf("== a corrupt column cannot wrap into a legal value ==\n");
   {
      long t   = 0;
      int glu  = 0;
      int src  = 0;
      int kind = 0;

      /* THE ISOLATING PAIR. 4294967297 is 2^32 + 1: ten digits, so it fits a
       * long and is not refused for length -- it is refused, or not, purely
       * on which side of the cast the bound sits. */
      ck(plot_store_row("1700000000,120,0,-70,3,4294967297,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "a source id past the 16-bit domain reads UNATTRIBUTED, not as the "
         "device its low bits name");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,4294967297,\n", &t, &glu,
                        &src, &kind) == 1 &&
             kind == KIND_CGM,
         "a kind that wraps to KIND_BGM is still not a fingerstick");
      /* And 2^32 + 2 in the same column, item 63's other worked example. */
      ck(plot_store_row("1700000000,120,0,-70,3,4294967298,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "...and so does the next one up");

      /* THE GLUCOSE COLUMN, whose bound is `> 0 && < 2000`. 4294968296 wraps
       * to 1000 -- a plausible severe high, drawn on the plot and counted by
       * anything that re-reads this row. */
      ck(plot_store_row("1700000000,4294968296,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "a glucose that wraps into the legal band is refused, not drawn "
         "as 1000");

      /* THE EXACT INT BOUNDARIES, one either side, for every column that is
       * narrowed to an int on the way out. */
      ck(plot_store_row("1700000000,2147483647,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "glucose INT_MAX is refused");
      ck(plot_store_row("1700000000,2147483648,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "glucose INT_MAX + 1 is refused");
      ck(plot_store_row("1700000000,120,0,-70,3,2147483647,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "source INT_MAX reads unattributed");
      ck(plot_store_row("1700000000,120,0,-70,3,2147483648,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "source INT_MAX + 1 reads unattributed");
      ck(plot_store_row("1700000000,120,0,-70,3,-2147483648,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "source INT_MIN reads unattributed");
      ck(plot_store_row("1700000000,120,0,-70,3,-2147483649,0,0,0,\n", &t, &glu,
                        &src, &kind) == 1 &&
             src == 0,
         "source INT_MIN - 1 reads unattributed");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2147483647,\n", &t, &glu,
                        &src, &kind) == 1 &&
             kind == KIND_CGM,
         "kind INT_MAX normalises to KIND_CGM");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,2147483648,\n", &t, &glu,
                        &src, &kind) == 1 &&
             kind == KIND_CGM,
         "kind INT_MAX + 1 normalises to KIND_CGM");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,-2147483648,\n", &t, &glu,
                        &src, &kind) == 1 &&
             kind == KIND_CGM,
         "kind INT_MIN normalises to KIND_CGM");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,-2147483649,\n", &t, &glu,
                        &src, &kind) == 1 &&
             kind == KIND_CGM,
         "kind INT_MIN - 1 normalises to KIND_CGM");
      ck(plot_store_row("1700000000,-2147483648,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "glucose INT_MIN is refused");
      ck(plot_store_row("1700000000,-2147483649,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "glucose INT_MIN - 1 is refused");
      /* 65535 is the LAST id sensor_mint will ever issue, and 65536 the first
       * it refuses; the reader has to agree with the minter on both. */
      ck(plot_store_row("1700000000,120,0,-70,3,65535,0,0,0,\n", &t, &glu, &src,
                        &kind) == 1 &&
             src == 65535,
         "the highest id the registry can mint still resolves");
      ck(plot_store_row("1700000000,120,0,-70,3,65536,0,0,0,\n", &t, &glu, &src,
                        &kind) == 1 &&
             src == 0,
         "...and the first one past it does not");

      /* NINETEEN DIGITS -- one past what the shared cursor holds -- in every
       * column, INCLUDING the ones this function throws away. The discarded
       * ones matter most: nothing downstream ever looks at trend, rssi, lag,
       * device_time, tz or rescale, so an unbounded accumulation there was
       * undefined behaviour with no observable value at all to hint at it. */
      ck(plot_store_row("9999999999999999999,120,0,-70,3,7,0,0,0,\n", &t, &glu,
                        &src, &kind) == 0,
         "19 digits of timestamp refuse the row");
      ck(plot_store_row("1700000000,9999999999999999999,0,-70,3,7,0,0,0,\n", &t,
                        &glu, &src, &kind) == 0,
         "19 digits of glucose refuse the row");
      ck(plot_store_row("1700000000,120,9999999999999999999,-70,3,7,0,0,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of trend refuse the row");
      ck(plot_store_row("1700000000,120,0,-9999999999999999999,3,7,0,0,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of rssi refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,9999999999999999999,7,0,0,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of lag refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,3,9999999999999999999,0,0,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of source refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,3,7,9999999999999999999,0,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of device_time refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,-9999999999999999999,0,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of tz refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,9999999999999999999,\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of kind refuse the row");
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,0,9999999999999999999\n",
                        &t, &glu, &src, &kind) == 0,
         "19 digits of rescale refuse the row");
      /* Eighteen is what the cursor holds exactly, so it is a value, not a
       * refusal -- the row is then dropped by its own column's rules rather
       * than by the parser. Pinned so a future tightening of the cap is a
       * deliberate act and not a silent one. */
      ck(plot_store_row("1700000000,120,0,-70,3,999999999999999999,0,0,0,\n",
                        &t, &glu, &src, &kind) == 1 &&
             src == 0,
         "18 digits is a value the cursor holds, and it is out of the id "
         "domain");
   }

   /* NOTHING IS PUBLISHED UNTIL THE WHOLE ROW HAS PASSED.
    *
    * The old reader wrote *t, *glu, *src and *kind as it went and only THEN
    * returned its verdict, so a refused row left three of the four out-params
    * holding numbers taken from a line the function had just declared not to
    * be a datapoint. plong_build happens to ignore them, which is exactly the
    * kind of safety that survives until the next caller. */
   printf("== a refused row leaves the caller's variables alone ==\n");
   {
      long t   = -7;
      int glu  = -7;
      int src  = -7;
      int kind = -7;
      ck(plot_store_row("1700000000,120,0,-70,3,7,0,0,9999999999999999999,\n",
                        &t, &glu, &src, &kind) == 0 &&
             t == -7 && glu == -7 && src == -7 && kind == -7,
         "an overflowing column refuses the row without publishing any part "
         "of it");
      ck(plot_store_row("1700000000,4000,0,-70,3,7,0,0,1,\n", &t, &glu, &src,
                        &kind) == 0 &&
             t == -7 && glu == -7 && src == -7 && kind == -7,
         "an out-of-range glucose refuses the row without publishing any "
         "part of it");
      ck(plot_store_row("# unix_time,glucose_mgdl,trend\n", &t, &glu, &src,
                        &kind) == 0 &&
             t == -7 && glu == -7,
         "the header line is not a datapoint and writes nothing");
   }

   plot_geometry_tests();

   printf("plottest: %d assertions\n", nck);
   printf(fail ? "plottest: FAIL\n" : "ALL PLOT TESTS PASSED\n");
   return fail;
}
