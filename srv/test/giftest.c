/* SPDX-License-Identifier: GPL-3.0
 * giftest.c --- the GIF encoder, interleaved and concurrent
 * Copyright 2026 Jakob Kastelic
 *
 * WHAT THIS PINS.
 *
 * The encoder's LZW dictionary was two file-scope arrays. Two encodes at once
 * therefore shared one dictionary: each reset wiped the other's entries and
 * each add inserted into the other's table, so both emitted streams that
 * decode to something other than the image that went in. Nothing in gif.c
 * said so -- the safety came from a mutex in web.c, three modules away, that
 * serialises PAGES for an unrelated reason. That is a renderer whose
 * correctness lives in another file, and the failure mode is the quiet one:
 * not a crash, not an error, just a plot that is wrong.
 *
 * The dictionary is now a workspace the caller owns, so:
 *
 *   1. A REFERENCE. Each image encoded alone, on its own, is the answer
 *      everything else is compared against.
 *   2. INTERLEAVED. Two encodes alternating on ONE thread with two
 *      workspaces produce exactly those bytes. (With one shared dictionary
 *      they could not.)
 *   3. CONCURRENT. The same two encodes on two threads, thousands of times,
 *      still produce exactly those bytes -- the assertion that fails if the
 *      state is shared, and the reason this test uses real threads.
 *   4. SHARING THE WORKSPACE IS THE ONLY WAY TO GET IT WRONG. Two threads
 *      deliberately handed the SAME workspace must, at least once, produce
 *      something other than the reference -- otherwise cases 2 and 3 prove
 *      nothing, because the images would encode identically either way.
 *
 * The images differ in content and in scale, so their dictionaries diverge
 * early: a flat one (long runs, few entries) and a noisy one (a new entry
 * almost every pixel), which is the pair most likely to notice a shared
 * table.
 *
 * ---- AND WHAT THE ENCODER WILL NOT ACCEPT (TODO item 104) ----------------
 *
 * gif_encode took any positive `int` for width and height, wrote the LOW
 * SIXTEEN BITS of each into both descriptors, and then walked w*h pixels
 * anyway. So a width of 65537 emitted a header saying the image is one pixel
 * wide -- measured, before the fix: `screen 1x4, header promises 4 pixels`
 * above an LZW stream carrying 262148 of them. A width of 65536 said zero.
 * Nothing returned an error, because there was no error to return.
 *
 * TWO KINDS OF CASE, BECAUSE THE BOUNDARY WILL NOT FIT IN A MACHINE.
 *
 *   * PINNED BY THE PREDICATE. 65535 x 65535 is four gigapixels; no test can
 *     allocate the pixel buffer that would let it drive gif_encode there. So
 *     the rule is gif_dims_ok, a pure function of two ints, and the corner --
 *     both dimensions at the maximum, and one past it in each direction -- is
 *     pinned by calling it. Nothing is allocated and nothing is encoded.
 *
 *   * PINNED BY DRIVING THE REAL ENCODER. Everything a machine can hold goes
 *     through gif_encode itself: 65535 and 65536 in one dimension with the
 *     other at 1, zero, negative, every NULL argument, and ncolors at 1, 2,
 *     256 and 257. A predicate that agrees with nothing is not a rule.
 *
 * AND EVERY REFUSAL IS CHECKED AGAINST A SENTINEL. The output buffer is
 * filled with 0xA5 first and asserted byte-for-byte unchanged afterwards.
 * That is the assertion an argument check placed AFTER the six signature
 * bytes would fail, and the return value alone cannot see it: a call that
 * writes "GIF89a" and then returns 0 looks exactly like one that refused.
 *
 * Built and run by `make giftest`.
 */
#include "gif.h"
#include "plot.h" /* the renderer whose pixels this encoder is handed */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int all = 1;
/* COUNTED: a suite that runs no assertions passes just as loudly as one that
 * runs them all, so the number is printed with the verdict. */
static int nck;

static void ck(int cond, const char *what)
{
   nck++;
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* Two frames of different sizes -- a plot page really does render several
 * windows -- and different pixel statistics. */
#define W1 200
#define H1 120
#define W2 320
#define H2 64

static uint8_t px1[W1 * H1];
static uint8_t px2[W2 * H2];
static uint8_t pal[16][3];

#define CAP (256 * 1024)
static uint8_t ref1[CAP], ref2[CAP];
static size_t n1, n2;

/* The limit cases below get their own workspace: they run after the
 * concurrency cases and must not be reading a dictionary those left behind. */
static struct gif_ws ws2;

static void dims_cases(void);
static void encode_limit_cases(void);

/* A deterministic "noise" that needs no RNG (and no clock: the scripts that
 * run this must be reproducible). */
static uint8_t mix(unsigned i, unsigned salt)
{
   unsigned v = (i * 2654435761U) ^ (salt * 40503U);
   return (uint8_t)((v >> 13) & 15);
}

static void fixtures(void)
{
   for (int i = 0; i < 16; i++) {
      pal[i][0] = (uint8_t)(i * 17);
      pal[i][1] = (uint8_t)(255 - (i * 17));
      pal[i][2] = (uint8_t)((i * 37) & 0xFF);
   }
   /* FLAT: long runs, so the dictionary fills slowly and the codes stay
    * narrow -- the shape a plot's background has. */
   for (int i = 0; i < W1 * H1; i++)
      px1[i] = (uint8_t)((i / 97) % 3);
   /* NOISY: a new dictionary entry almost every pixel, so this one runs
    * through the width bumps and the 4096-entry reset. */
   for (int i = 0; i < W2 * H2; i++)
      px2[i] = mix((unsigned)i, 7);
}

struct job {
   int which;         /* 1 or 2 */
   struct gif_ws *ws; /* the workspace to use */
   uint8_t out[CAP];  /* where this job encodes to */
   size_t n;
   int rounds;
   int mismatches;
};

static void encode_one(struct job *j)
{
   if (j->which == 1)
      j->n = gif_encode(j->ws, j->out, CAP, px1, W1, H1, pal, 16);
   else
      j->n = gif_encode(j->ws, j->out, CAP, px2, W2, H2, pal, 16);
}

static int matches_ref(const struct job *j)
{
   const uint8_t *r = j->which == 1 ? ref1 : ref2;
   size_t rn        = j->which == 1 ? n1 : n2;
   return j->n == rn && memcmp(j->out, r, rn) == 0;
}

static void *runner(void *arg)
{
   struct job *j = arg;
   for (int i = 0; i < j->rounds; i++) {
      encode_one(j);
      if (!matches_ref(j))
         j->mismatches++;
   }
   return 0;
}

/* Two jobs, run however the caller says, reporting how many encodes came out
 * wrong. `shared` gives both the SAME workspace on purpose. */
static int race(int rounds, int shared, int threads)
{
   static struct gif_ws wsa, wsb;
   static struct job ja, jb;
   ja = (struct job){.which = 1, .ws = &wsa, .rounds = rounds};
   jb = (struct job){.which = 2, .ws = shared ? &wsa : &wsb, .rounds = rounds};
   if (threads) {
      pthread_t ta, tb;
      pthread_create(&ta, 0, runner, &ja);
      pthread_create(&tb, 0, runner, &jb);
      pthread_join(ta, 0);
      pthread_join(tb, 0);
   } else {
      /* INTERLEAVED on one thread: alternate, so each encode starts with the
       * dictionary the other one left behind. */
      for (int i = 0; i < rounds; i++) {
         ja.rounds = jb.rounds = 1;
         runner(&ja);
         runner(&jb);
      }
   }
   return ja.mismatches + jb.mismatches;
}

/* ---- THE DIMENSION DOMAIN, AS A PURE FUNCTION (TODO item 104) ---------- */

/* gif_dims_ok answers about numbers, not about memory, which is the only
 * reason the far corner is reachable at all. */
static int dims(int w, int h, size_t want)
{
   size_t got = (size_t)-1;
   if (!gif_dims_ok(w, h, &got))
      return 0;
   return got == want;
}

static int dims_refused(int w, int h)
{
   /* THE COUNT MUST BE ZEROED ON A REFUSAL, not left as it was. A caller that
    * drops the return value would otherwise bound a pixel loop with a stale
    * number, which is the failure this whole item is about. */
   size_t got = 0xDEADBEEF;
   int r      = gif_dims_ok(w, h, &got);
   return r == 0 && got == 0;
}

static void dims_cases(void)
{
   printf("== the dimensions a GIF can carry (the predicate) ==\n");

   /* THE EXACT BOUNDARY, AND ONE PAST IT IN BOTH DIRECTIONS, PER AXIS. */
   ck(dims(65535, 1, 65535), "65535 wide is the widest a GIF descriptor says");
   ck(dims_refused(65536, 1), "...and 65536 is one past it, so it is refused");
   ck(dims(65534, 1, 65534), "...while 65534, one below, is fine");
   ck(dims(1, 65535, 65535), "65535 tall is likewise the tallest");
   ck(dims_refused(1, 65536), "...and 65536 tall is refused");
   ck(dims(1, 65534, 65534), "...while 65534 tall is fine");

   /* THE CORNER, WHICH IS FOUR GIGAPIXELS AND CANNOT BE ALLOCATED. This is
    * the case that exists only because the rule is a pure function. */
   ck(dims(65535, 65535, (size_t)65535 * 65535),
      "the far corner, 65535x65535, is accepted and counts 4294836225 pixels");
   ck(dims_refused(65536, 65535), "...one past it in width is refused");
   ck(dims_refused(65535, 65536), "...one past it in height is refused");
   ck(dims_refused(65536, 65536), "...and past it in both is refused");

   /* THE BOTTOM OF THE DOMAIN. Zero is refused rather than quietly encoding
    * an empty image: the encoder reads px[0] before its loop, so a zero-pixel
    * frame is a read out of a buffer with nothing in it. */
   ck(dims(1, 1, 1), "one pixel is a legal image");
   ck(dims_refused(0, 1), "zero width is refused");
   ck(dims_refused(1, 0), "zero height is refused");
   ck(dims_refused(0, 0), "and zero in both is refused");
   ck(dims_refused(-1, 8), "a negative width is refused");
   ck(dims_refused(8, -1), "a negative height is refused");
   ck(dims_refused(-1, -1), "negative in both is refused");

   /* WHAT THE OLD CODE TOOK. Each of these was accepted, serialised as its
    * low sixteen bits, and then multiplied through a signed `long`. */
   ck(dims_refused(65537, 4),
      "65537 -- which used to write a header saying the image is ONE pixel "
      "wide");
   ck(dims_refused(70000, 4),
      "70000 -- which used to write 4464, its low sixteen bits");
   ck(dims_refused(131072, 4), "131072 -- which used to write zero");
   ck(dims_refused(2147483647, 2147483647),
      "INT_MAX by INT_MAX -- whose product overflows a 32-bit signed long");

   /* NO OUTPUT PARAMETER IS NOT A QUESTION. */
   ck(gif_dims_ok(8, 8, NULL) == 0,
      "asking without somewhere to put the answer is refused");
}

/* ---- THE SAME LIMITS, THROUGH THE REAL ENCODER -------------------------
 *
 * SENTINEL FIRST, ASSERT NOTHING WAS WRITTEN AFTER. */
#define SENT 0xA5

static uint8_t *sbuf; /* sentinel-filled output for the refusal cases */
static size_t sbuf_n;

static void sentinel_fill(void)
{
   memset(sbuf, SENT, sbuf_n);
}

static int sentinel_intact(void)
{
   for (size_t i = 0; i < sbuf_n; i++)
      if (sbuf[i] != SENT)
         return 0;
   return 1;
}

/* One refusal: the call must return 0 AND leave every byte of the output as
 * it found it. Both halves, or the case proves only that a number came back. */
static int refused(size_t n)
{
   return n == 0 && sentinel_intact();
}

static void encode_limit_cases(void)
{
   printf("== what the encoder refuses, and writes nothing for ==\n");
   sbuf_n = CAP;
   sbuf   = malloc(sbuf_n);
   /* 65536 pixels: the largest single row a GIF may have, plus one, so the
    * 65535 case and the 65536 case can use the SAME buffer. Only the accepted
    * one is ever read from it. */
   size_t pxn  = 65536;
   uint8_t *px = malloc(pxn);
   if (!sbuf || !px) {
      ck(0, "the fixtures for the limit cases could be allocated");
      return;
   }
   for (size_t i = 0; i < pxn; i++)
      px[i] = (uint8_t)((i / 97) % 3);

   /* THE WIDEST ROW A GIF CAN CARRY, ACCEPTED AND SERIALISED HONESTLY. This
    * is the positive control: without it, every refusal below is satisfied by
    * an encoder that refuses everything. */
   sentinel_fill();
   size_t n = gif_encode(&ws2, sbuf, sbuf_n, px, 65535, 1, pal, 16);
   ck(n > 0, "65535x1 -- the widest legal row -- encodes");
   ck(n > 0 && sbuf[6] == 0xFF && sbuf[7] == 0xFF && sbuf[8] == 1 &&
          sbuf[9] == 0,
      "...and the logical screen descriptor really says 65535 x 1");
   /* The image descriptor follows the 6-byte signature, 7 bytes of screen
    * descriptor and a 16-entry (48-byte) global colour table, then 0x2C and
    * four bytes of origin. */
   ck(n > 0 && sbuf[61] == 0x2C && sbuf[66] == 0xFF && sbuf[67] == 0xFF,
      "...and so does the image descriptor, which is the other 16-bit field");

   /* ONE PAST IT. The pixel buffer is real and non-NULL, and ncolors is
    * valid, so the DIMENSION is the only thing left that can refuse this --
    * otherwise the case would pass while pinning nothing. */
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 65536, 1, pal, 16)),
      "65536x1 is refused, and not one byte is written");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 65537, 1, pal, 16)),
      "65537x1 -- the width that used to claim to be 1 -- is refused");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 1, 65536, pal, 16)),
      "65536 TALL is refused too: the height field is the same 16 bits");

   /* THE TALLEST LEGAL COLUMN, so the height limit is pinned from below as
    * well as above rather than only refused. */
   sentinel_fill();
   n = gif_encode(&ws2, sbuf, sbuf_n, px, 1, 65535, pal, 16);
   ck(n > 0 && sbuf[8] == 0xFF && sbuf[9] == 0xFF,
      "1x65535 -- the tallest legal column -- encodes and says so");

   /* ZERO AND NEGATIVE. */
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 0, 8, pal, 16)),
      "a zero width is refused, and writes nothing");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 8, 0, pal, 16)),
      "a zero height is refused, and writes nothing");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, -8, 8, pal, 16)),
      "a negative width is refused, and writes nothing");

   /* EVERY POINTER. `px` was dereferenced at the head of the pixel loop and
    * `pal` while emitting the colour table, so both of these used to be a
    * segmentation fault in a request handler -- measured, not inferred. */
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, NULL, 8, 8, pal, 16)),
      "no pixels: refused rather than dereferenced (it used to segfault)");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 8, 8, NULL, 16)),
      "no palette: refused rather than read");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, NULL, sbuf_n, px, 8, 8, pal, 16)),
      "no output buffer: refused rather than written through");
   sentinel_fill();
   ck(refused(gif_encode(NULL, sbuf, sbuf_n, px, 8, 8, pal, 16)),
      "no workspace: refused, and still writes nothing");
   sentinel_fill();
   ck(refused(gif_encode(NULL, NULL, 0, NULL, 0, 0, NULL, 0)),
      "every argument at once: refused, no crash, nothing written");

   /* THE PALETTE SIZE, which was already checked -- kept here so the sentinel
    * covers it too, since these checks now sit beside the new ones and a
    * later edit could move one of them past the first put(). */
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 8, 8, pal, 1)),
      "one colour is not a palette: refused, nothing written");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 8, 8, pal, 257)),
      "257 colours will not fit a GIF colour table: refused, nothing written");
   sentinel_fill();
   ck(refused(gif_encode(&ws2, sbuf, sbuf_n, px, 8, 8, pal, 0)),
      "zero colours: refused, nothing written");

   /* A BUFFER TOO SMALL is still zero, but it is the one zero that MAY have
    * written -- the encoder discovers it has run out only by running out. It
    * is listed here so the sentinel rule above is not read as covering it. */
   sentinel_fill();
   ck(gif_encode(&ws2, sbuf, 8, px, 64, 64, pal, 16) == 0,
      "an output buffer too small to hold the file reports zero");

   free(sbuf);
   free(px);
   sbuf = NULL;
}

/* ---- THE PLOT THAT IS ENCODED (TODO item 142) --------------------------
 *
 * srv/plots.c fills its framebuffer with lib/plot.c -- the SAME renderer the
 * phone draws with -- and then hands those pixels to the encoder above. So
 * the server has a second boundary of exactly the kind this file already
 * guards for the encoder: plot_render used to take a stride, a width and a
 * height from its caller, clip only x and y, and then index at
 * `y * stride + x`. A stride narrower than the width overlaps the rows and
 * runs the last one past the end of the image buffer; a negative one walks
 * off the front of it. Either is an out-of-bounds WRITE into the very
 * workspace this suite then compresses.
 *
 * IMG_W x IMG_H and the plot rectangle below are srv/plots.c's own numbers,
 * repeated rather than shared because that file needs sqlite and a database
 * to be linked. If they drift, the control's hash changes and this fails --
 * which is the correct outcome for a control.
 *
 * THE HASH IS THE CONTROL, and it is the half that matters most: it was taken
 * from the renderer as it was BEFORE the geometry boundary existed, so it
 * fails if validation moved a single pixel of the image the server serves.
 * The refusals are checked against a sentinel-filled buffer, as every refusal
 * in this file is, because a check placed after the frame is drawn returns
 * the same verdict as one placed before it. */
#define PLOT_IMG_W 720
#define PLOT_IMG_H 300
#define PLOT_XSTRIP                                                            \
   14 /* the label strip srv/plots.c keeps below the plot rectangle */
#define PLOT_SENT 0xA5A5A5A5u

static uint32_t g_pfb[(size_t)PLOT_IMG_W * PLOT_IMG_H];
static struct plot_pt g_ppts[600];

static uint32_t plot_trace_white(int g)
{
   (void)g;
   return 0xFFFFFFFFu;
}

static unsigned long plot_hash(const uint32_t *px, long n)
{
   /* FNV-1a over the pixel VALUES, so the digest is a statement about what
    * was drawn rather than about this host's byte order. */
   unsigned long h = 1469598103934665603UL;
   for (long i = 0; i < n; i++) {
      h ^= (unsigned long)px[i];
      h *= 1099511628211UL;
   }
   return h;
}

static long plot_dirty(void)
{
   long n = 0;
   for (long i = 0; i < (long)((size_t)PLOT_IMG_W * PLOT_IMG_H); i++)
      if (g_pfb[i] != PLOT_SENT)
         n++;
   return n;
}

static void plot_fill(uint32_t v)
{
   for (long i = 0; i < (long)((size_t)PLOT_IMG_W * PLOT_IMG_H); i++)
      g_pfb[i] = v;
}

static void plot_cases(void)
{
   long now = 1785272930;
   int n    = (int)(sizeof g_ppts / sizeof g_ppts[0]);
   for (int i = 0; i < n; i++) {
      g_ppts[i].t      = now - ((long)i * 300);
      g_ppts[i].glu    = 60 + ((i * 37) % 260);
      g_ppts[i].marker = i % 9;
      g_ppts[i].hidden = (i % 53) == 0;
      g_ppts[i].size   = (i % 5) + 1;
      g_ppts[i].col    = (i % 7) ? 0 : 0xFFFF00FFu;
   }

   printf("== the plot the server encodes ==\n");
   struct plot_fb fb   = {g_pfb, PLOT_IMG_W, PLOT_IMG_W, PLOT_IMG_H};
   struct plot_rect rc = {0, 0, PLOT_IMG_W, PLOT_IMG_H - PLOT_XSTRIP};
   struct plot_cfg cfg = {PLOT_GLU_MAX, 3}; /* srv/plots.c's PLOT_PRAD */

   ck(plot_render_check(fb, rc, cfg) == PLOT_GEOM_OK,
      "the server's own framebuffer and plot rectangle are accepted");
   plot_fill(0xFF181818u); /* the app's screen background, as plots.c does */
   plot_render(fb, rc, g_ppts, n, now, 24, cfg, plot_trace_white, -1, 0,
               -25200);
   unsigned long h = plot_hash(g_pfb, (long)((size_t)PLOT_IMG_W * PLOT_IMG_H));
   printf("       720x300 window %016lx\n", h);
   ck(h == 0xdcf3042120fcf401UL,
      "...and renders pixel for pixel what it rendered before the geometry "
      "was checked");

   /* The whole path, end to end: those pixels really do encode. */
   {
      static uint8_t img[(size_t)PLOT_IMG_W * PLOT_IMG_H];
      static uint8_t out[(size_t)PLOT_IMG_W * PLOT_IMG_H * 2];
      uint8_t gray[16][3];
      struct gif_ws ws;
      for (int i = 0; i < 16; i++)
         gray[i][0] = gray[i][1] = gray[i][2] = (uint8_t)(i * 17);
      for (long i = 0; i < (long)((size_t)PLOT_IMG_W * PLOT_IMG_H); i++)
         img[i] = (uint8_t)((g_pfb[i] & 0xFFu) >> 4);
      size_t got = gif_encode(&ws, out, sizeof out, img, PLOT_IMG_W, PLOT_IMG_H,
                              gray, 16);
      ck(got > 0 && memcmp(out, "GIF89a", 6) == 0,
         "and the rendered plot still encodes to a GIF");
   }

   printf("== a lied-about plot buffer is refused before any pixel ==\n");
   {
      /* A stride narrower than the width: rows overlap, and the last one
       * lands past the end of a buffer this size. */
      struct plot_fb narrow = {g_pfb, PLOT_IMG_W / 2, PLOT_IMG_W, PLOT_IMG_H};
      ck(plot_fb_check(narrow) == PLOT_GEOM_STRIDE,
         "a stride narrower than the image width is refused");
      plot_fill(PLOT_SENT);
      plot_render(narrow, rc, g_ppts, n, now, 24, cfg, plot_trace_white, -1, 0,
                  0);
      ck(plot_dirty() == 0, "...and nothing at all is written");

      struct plot_fb back = {g_pfb, -PLOT_IMG_W, PLOT_IMG_W, PLOT_IMG_H};
      ck(plot_fb_check(back) == PLOT_GEOM_STRIDE,
         "a negative stride is refused");
      plot_fill(PLOT_SENT);
      plot_render(back, rc, g_ppts, n, now, 24, cfg, plot_trace_white, -1, 0,
                  0);
      ck(plot_dirty() == 0, "...and nothing at all is written for it either");

      /* The rectangle, too: the label strip is 14 rows the plot may not use,
       * and a rectangle that claims them runs past the buffer's last row. */
      struct plot_rect over = {0, 0, PLOT_IMG_W, PLOT_IMG_H + 1};
      ck(plot_render_check(fb, over, cfg) == PLOT_GEOM_RECT,
         "a plot rectangle one row taller than the image is refused");
      plot_fill(PLOT_SENT);
      plot_render(fb, over, g_ppts, n, now, 24, cfg, plot_trace_white, -1, 0,
                  0);
      ck(plot_dirty() == 0, "...without drawing the rows that would fit");
   }
}

int main(void)
{
   printf("giftest: one dictionary per encode\n");
   fixtures();

   /* 1. the reference: each alone. */
   static struct gif_ws ws;
   n1 = gif_encode(&ws, ref1, CAP, px1, W1, H1, pal, 16);
   n2 = gif_encode(&ws, ref2, CAP, px2, W2, H2, pal, 16);
   ck(n1 > 0 && n2 > 0, "both frames encode on their own");
   ck(n1 != n2, "...to different files, as two different images should");
   /* The header is the one part that can be read without a decoder. */
   ck(memcmp(ref1, "GIF89a", 6) == 0, "...and the output really is a GIF89a");

   /* 2. interleaved on one thread. */
   ck(race(200, 0, 0) == 0,
      "interleaved encodes are byte-for-byte the same as alone");

   /* 3. concurrent on two threads. */
   ck(race(2000, 0, 1) == 0, "...and so are concurrent ones");

   /* 4. the control: sharing one workspace must be visibly wrong, or the two
    * cases above would pass whatever the encoder did. */
   ck(race(2000, 1, 1) > 0,
      "sharing ONE workspace does corrupt the output (so the test can tell)");

   /* An encode with no workspace is refused rather than crashing on it. */
   ck(gif_encode(NULL, ref1, CAP, px1, W1, H1, pal, 16) == 0,
      "an encode with no workspace is refused");

   dims_cases();
   encode_limit_cases();
   plot_cases();

   printf("giftest: %d assertions\n", nck);
   printf("%s\n", all ? "giftest: every encode carries its own dictionary"
                      : "giftest: FAILED");
   return all ? 0 : 1;
}
