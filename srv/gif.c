/* SPDX-License-Identifier: GPL-3.0
 * gif.c --- minimal GIF89a encoder (single frame, indexed color, real LZW)
 * Copyright 2026 Jakob Kastelic
 *
 * No libraries: the GIF file format is simple enough to emit by hand --
 * header + palette + one image block whose pixel data is LZW-compressed
 * with variable-width codes (LSB-first bit packing, 255-byte sub-blocks).
 * The LZW patent expired in 2004. For flat-color plots this compresses to
 * a few KB, losslessly -- the reason this is a GIF and not a JPEG.
 */

#include "gif.h"
#include <string.h>

/* ---- output writer: bytes, LSB-first bit codes, 255-byte sub-blocks ---- */

struct wr {
   uint8_t *out;
   size_t cap, n;
   uint8_t block[255]; /* pending LZW data sub-block */
   int bn;
   uint32_t acc; /* bit accumulator, LSB first */
   int bits;
};

static void put(struct wr *w, uint8_t c)
{
   if (w->n < w->cap)
      w->out[w->n++] = c;
   else
      w->cap = 0; /* poison: overflow reported as size 0 */
}

static void flush_block(struct wr *w)
{
   if (!w->bn)
      return;
   put(w, (uint8_t)w->bn);
   for (int i = 0; i < w->bn; i++)
      put(w, w->block[i]);
   w->bn = 0;
}

static void put_code(struct wr *w, int code, int width)
{
   w->acc |= (uint32_t)code << w->bits;
   w->bits += width;
   while (w->bits >= 8) {
      w->block[w->bn++] = (uint8_t)(w->acc & 0xFF);
      w->acc >>= 8;
      w->bits -= 8;
      if (w->bn == 255)
         flush_block(w);
   }
}

/* ---- LZW dictionary: (prefix code, next byte) -> code, hashed ----
 *
 * The table belongs to the CALLER (struct gif_ws, see gif.h): as two
 * file-scope arrays, two concurrent encodes shared one dictionary and both
 * emitted streams that decode to garbage. */

static void dict_reset(struct gif_ws *ws)
{
   memset(ws->key, 0xFF, sizeof ws->key); /* -1 = empty */
}

static int dict_find(const struct gif_ws *ws, int32_t key)
{
   uint32_t i = ((uint32_t)key * 2654435761U) & (GIF_HSIZE - 1);
   while (ws->key[i] != -1) {
      if (ws->key[i] == key)
         return ws->code[i];
      i = (i + 1) & (GIF_HSIZE - 1);
   }
   return -1;
}

static void dict_add(struct gif_ws *ws, int32_t key, int code)
{
   uint32_t i = ((uint32_t)key * 2654435761U) & (GIF_HSIZE - 1);
   while (ws->key[i] != -1)
      i = (i + 1) & (GIF_HSIZE - 1);
   ws->key[i]  = key;
   ws->code[i] = (uint16_t)code;
}

/* ---- THE DIMENSION RULE, AS A FUNCTION OF NOTHING (see gif.h) ---------- */

int gif_dims_ok(int w, int h, size_t *npx)
{
   /* ANSWERED FIRST, ALWAYS. A caller that drops the return value must be
    * left with a count of zero rather than its own stack, because the one
    * thing it will do with *npx is bound a loop over somebody's pixels. */
   if (npx)
      *npx = 0;
   if (!npx)
      return 0;
   /* 1..65535 in each direction. Zero is refused rather than treated as an
    * empty image: a zero-width Image Descriptor is a file decoders disagree
    * about, and the encoder below would read px[0] out of a buffer that has no
    * pixels in it. Negative is refused because `int` is what the parameter is,
    * not because a negative size means anything. */
   if (w < 1 || w > GIF_DIM_MAX || h < 1 || h > GIF_DIM_MAX)
      return 0;
   /* CHECKED MULTIPLICATION, even though the _Static_assert in gif.h proves it
    * cannot fail at the current GIF_DIM_MAX. It costs one divide once per
    * image, and it is the line that stays correct if the limit ever moves --
    * the old code multiplied through a signed `long`, which on a 32-bit `long`
    * is undefined behaviour rather than a wrong answer. */
   if ((size_t)w > SIZE_MAX / (size_t)h)
      return 0;
   *npx = (size_t)w * (size_t)h;
   return 1;
}

size_t gif_encode(struct gif_ws *ws, uint8_t *out, size_t cap,
                  const uint8_t *px, int w, int h, const uint8_t pal[][3],
                  int ncolors)
{
   /* EVERY CHECK BEFORE EVERY WRITE. `struct wr` is not even constructed until
    * the arguments are known good, so there is no path on which the six bytes
    * of "GIF89a" reach `out` and the call then decides it cannot continue.
    * That ordering is the whole point: a refusal that has already written a
    * header is indistinguishable, to the buffer's owner, from a success.
    *
    * The NULL tests are new and were reachable. `px` was dereferenced
    * unconditionally at the head of the pixel loop (`int prefix = px[0]`), so
    * gif_encode(ws, out, cap, NULL, 8, 8, pal, 16) segfaulted -- measured, not
    * inferred. `pal` was read while emitting the global colour table, and
    * `out` was written through by put() whenever cap was nonzero. Only `ws`
    * was ever checked. */
   if (!ws || !out || !px || !pal)
      return 0;
   if (ncolors < 2 || ncolors > 256)
      return 0;
   /* THE DIMENSIONS, AND THE PIXEL COUNT, IN ONE PLACE. See gif.h for what a
    * width above 65535 used to put on the wire; the short version is a header
    * claiming four pixels above a stream carrying 262148 of them. */
   size_t npx;
   if (!gif_dims_ok(w, h, &npx))
      return 0;
   /* palette size field: 2^(pbits) entries, smallest that fits */
   int pbits = 1;
   while ((1 << pbits) < ncolors)
      pbits++;
   struct wr wr = {.out = out, .cap = cap};

   /* header + logical screen descriptor + global color table */
   static const char sig[6] = {'G', 'I', 'F', '8', '9', 'a'};
   for (int i = 0; i < 6; i++)
      put(&wr, (uint8_t)sig[i]);
   put(&wr, (uint8_t)(w & 0xFF));
   put(&wr, (uint8_t)(w >> 8));
   put(&wr, (uint8_t)(h & 0xFF));
   put(&wr, (uint8_t)(h >> 8));
   put(&wr, (uint8_t)(0x80 | ((pbits - 1) << 4) | (pbits - 1)));
   put(&wr, 0); /* background = index 0 */
   put(&wr, 0); /* no aspect ratio */
   for (int i = 0; i < (1 << pbits); i++)
      for (int c = 0; c < 3; c++)
         put(&wr, i < ncolors ? pal[i][c] : 0);

   /* image descriptor: full frame, no local table, no interlace */
   put(&wr, 0x2C);
   for (int i = 0; i < 4; i++)
      put(&wr, 0); /* left, top */
   put(&wr, (uint8_t)(w & 0xFF));
   put(&wr, (uint8_t)(w >> 8));
   put(&wr, (uint8_t)(h & 0xFF));
   put(&wr, (uint8_t)(h >> 8));
   put(&wr, 0);

   /* LZW-compressed indices */
   int mcs = pbits < 2 ? 2 : pbits; /* spec: min code size >= 2 */
   put(&wr, (uint8_t)mcs);
   const int clear = 1 << mcs;
   const int eoi   = clear + 1;
   int next        = eoi + 1;
   int width       = mcs + 1;
   dict_reset(ws);
   put_code(&wr, clear, width);
   /* npx came from gif_dims_ok above, as a size_t: `long npx = (long)w * h`
    * was signed and could overflow where `long` is 32 bits. */
   int prefix = px[0];
   for (size_t i = 1; i < npx; i++) {
      int c       = px[i];
      int32_t key = ((int32_t)prefix << 8) | c;
      int found   = dict_find(ws, key);
      if (found >= 0) {
         prefix = found;
         continue;
      }
      put_code(&wr, prefix, width);
      /* DEFERRED width bump, matching every decoder (giflib, PIL, the
       * browsers): decoders lag the encoder's dictionary by exactly one
       * entry, so the first code at a new width is the one AFTER the
       * boundary entry's emit. Bumping before this emit (the obvious
       * spot, and the original bug) desynchronizes the stream at every
       * width boundary. */
      if (next == (1 << width) && width < 12)
         width++;
      if (next < 4096) {
         dict_add(ws, key, next);
         next++;
      } else { /* dictionary full: start over */
         put_code(&wr, clear, width);
         dict_reset(ws);
         next  = eoi + 1;
         width = mcs + 1;
      }
      prefix = c;
   }
   put_code(&wr, prefix, width);
   if (next == (1 << width) && width < 12)
      width++;
   put_code(&wr, eoi, width);
   if (wr.bits > 0)
      wr.block[wr.bn++] = (uint8_t)(wr.acc & 0xFF);
   flush_block(&wr);
   put(&wr, 0);    /* end of image data */
   put(&wr, 0x3B); /* trailer */
   return wr.cap ? wr.n : 0;
}
