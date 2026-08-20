/* SPDX-License-Identifier: GPL-3.0
 * gif.h --- minimal GIF89a encoder (single frame, indexed color)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef GIF_H
#define GIF_H

#include <stddef.h>
#include <stdint.h>

/* THE LZW DICTIONARY, WHICH THE CALLER OWNS.
 *
 * It was two file-scope arrays, so two encodes at once shared one dictionary:
 * each reset wiped the other's entries and each add inserted into the other's
 * table, and the streams they emitted decoded to garbage -- silently, because
 * a GIF that decodes wrong is still a GIF. Nothing in this file said so; the
 * safety came from a mutex in web.c, three modules away, that exists for an
 * unrelated reason.
 *
 * Making it a parameter is what makes the encoder reentrant BY CONSTRUCTION:
 * two callers with two workspaces cannot interfere, whatever locks either of
 * them holds. It is ~48 KB, so it is not something to put on a small thread's
 * stack by accident -- callers keep one per renderer. */
#define GIF_HSIZE 8192 /* power of two, > 4096 codes */

struct gif_ws {
   int32_t key[GIF_HSIZE];   /* -1 = empty */
   uint16_t code[GIF_HSIZE]; /* the code stored at that slot */
};

/* ---- THE DIMENSIONS A GIF CAN ACTUALLY CARRY -------------------------
 *
 * WHAT THE FORMAT SAYS. In GIF89a (CompuServe, 31 July 1990) both the Logical
 * Screen Descriptor and the Image Descriptor spell width and height as
 * "Unsigned" -- a two-byte, little-endian, 16-bit field. There is no wider
 * encoding anywhere in the format, so 65535 is not this encoder's opinion of a
 * sensible size, it is the largest number the file can say.
 *
 * WHAT THE ENCODER USED TO DO WITH A BIGGER ONE. It took any positive `int`
 * and serialised `w & 0xFF` then `w >> 8`, i.e. the low sixteen bits, into
 * both descriptors -- and then walked w*h pixels anyway. Measured, on this
 * machine, against the code as it stood:
 *
 *     w=65535 h=4  -> screen 65535x4, 262140 pixels promised, 262140 sent
 *     w=65536 h=4  -> screen     0x4,      0 pixels promised, 262144 sent
 *     w=65537 h=4  -> screen     1x4,      4 pixels promised, 262148 sent
 *     w=70000 h=4  -> screen  4464x4,  17856 pixels promised, 280000 sent
 *
 * So a width one past the domain does not produce a big image or an error: it
 * produces a header that says the image is FOUR PIXELS and an LZW stream
 * holding a quarter of a million of them. A decoder reads the four it was
 * promised and stops; everything after is trailing rubbish where the block
 * terminator and the 0x3B trailer should have been. The file is corrupt, and
 * corrupt in the quiet way -- it is still a GIF, it still has a signature, and
 * nothing returned an error. A width of exactly 65536 is worse still: the
 * header says zero, which most decoders reject outright and some divide by.
 *
 * AND THE PIXEL COUNT. The loop bound was `long npx = (long)w * h`. On LP64
 * that is merely a count of pixels nobody asked for; where `long` is 32 bits
 * -- and this server cross-builds for a board -- two ints near INT_MAX
 * multiply to signed overflow, which is undefined behaviour, not a big number.
 *
 * THE PREDICATE IS EXPOSED because the boundary cannot be reached any other
 * way. 65535x65535 is four gigapixels: the test cannot allocate the pixel
 * buffer that would let it drive gif_encode at the corner, so the rule about
 * the corner is a pure function it can call with any numbers it likes. (This
 * is what lib/gcm.c does with aes128_gcm_limits, and for the same reason.)
 * Everything the test CAN allocate -- 65535 and 65536 in one dimension, zero,
 * negative -- is driven through gif_encode itself as well.
 *
 * Returns 1 and writes w*h to *npx when both dimensions are in 1..65535 and
 * the product is representable as a size_t; otherwise returns 0 and writes 0.
 * *npx is written FIRST in every case, so a caller that ignores the return
 * value is left holding zero rather than whatever was on its stack. */
#define GIF_DIM_MAX 65535 /* GIF89a: the descriptors are 16-bit unsigned */

/* The multiplication below is checked at run time all the same, but on any
 * platform this program builds for the corner fits: 65535*65535 is 4294836225,
 * which is 1070 less than a 32-bit SIZE_MAX. If GIF_DIM_MAX is ever widened,
 * this is the assertion that stops it silently. */
_Static_assert(SIZE_MAX / GIF_DIM_MAX >= GIF_DIM_MAX,
               "GIF_DIM_MAX squared must be representable as a size_t");
_Static_assert(GIF_DIM_MAX == 0xFFFF,
               "GIF_DIM_MAX is the 16-bit descriptor field, not a policy");

int gif_dims_ok(int w, int h, size_t *npx) __attribute__((warn_unused_result));

/* Encode one w*h frame of palette indices (px[y*w+x], each < ncolors) as a
 * complete GIF89a file into out (at most cap bytes). pal holds ncolors RGB
 * triplets, 2 <= ncolors <= 256. `ws` is scratch, owned by the caller and
 * used only during the call. Returns the file size, or 0.
 *
 * ZERO MEANS NO FILE, and it means it for every reason: it did not fit in cap,
 * a pointer was NULL, ncolors was outside 2..256, or the dimensions were not
 * ones a GIF can carry (see gif_dims_ok above). A caller that wants to tell a
 * refusal from a short buffer can ask gif_dims_ok itself before calling; no
 * caller in this program needs to, because both answers are the same 500.
 *
 * NOTHING IS WRITTEN TO `out` WHEN THE ANSWER IS ZERO FOR ANY REASON OTHER
 * THAN CAPACITY. Every argument is checked before the first byte of the
 * signature goes down, which is the property srv/test/giftest.c fills the
 * output with a sentinel to check -- an argument check placed after the header
 * is written passes every test that only looks at the return value. */
size_t gif_encode(struct gif_ws *ws, uint8_t *out, size_t cap,
                  const uint8_t *px, int w, int h, const uint8_t pal[][3],
                  int ncolors) __attribute__((warn_unused_result));

#endif
