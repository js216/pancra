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

/* ---- LZW dictionary: (prefix code, next byte) -> code, hashed ---- */

#define HSIZE 8192 /* power of two, > 4096 codes */
static int32_t h_key[HSIZE];
static uint16_t h_code[HSIZE];

static void dict_reset(void)
{
   memset(h_key, 0xFF, sizeof h_key); /* -1 = empty */
}

static int dict_find(int32_t key)
{
   uint32_t i = ((uint32_t)key * 2654435761U) & (HSIZE - 1);
   while (h_key[i] != -1) {
      if (h_key[i] == key)
         return h_code[i];
      i = (i + 1) & (HSIZE - 1);
   }
   return -1;
}

static void dict_add(int32_t key, int code)
{
   uint32_t i = ((uint32_t)key * 2654435761U) & (HSIZE - 1);
   while (h_key[i] != -1)
      i = (i + 1) & (HSIZE - 1);
   h_key[i]  = key;
   h_code[i] = (uint16_t)code;
}

size_t gif_encode(uint8_t *out, size_t cap, const uint8_t *px, int w, int h,
                  const uint8_t pal[][3], int ncolors)
{
   if (w <= 0 || h <= 0 || ncolors < 2 || ncolors > 256)
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
   dict_reset();
   put_code(&wr, clear, width);
   long npx   = (long)w * h;
   int prefix = px[0];
   for (long i = 1; i < npx; i++) {
      int c       = px[i];
      int32_t key = ((int32_t)prefix << 8) | c;
      int found   = dict_find(key);
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
         dict_add(key, next);
         next++;
      } else { /* dictionary full: start over */
         put_code(&wr, clear, width);
         dict_reset();
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
