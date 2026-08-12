/* SPDX-License-Identifier: GPL-3.0
 * gif.h --- minimal GIF89a encoder (single frame, indexed color)
 * Copyright 2026 Jakob Kastelic
 */
#ifndef GIF_H
#define GIF_H

#include <stddef.h>
#include <stdint.h>

/* Encode one w*h frame of palette indices (px[y*w+x], each < ncolors) as a
 * complete GIF89a file into out (at most cap bytes). pal holds ncolors RGB
 * triplets, 2 <= ncolors <= 256. Returns the file size, or 0 if it did not
 * fit in cap. */
size_t gif_encode(uint8_t *out, size_t cap, const uint8_t *px, int w, int h,
                  const uint8_t pal[][3], int ncolors);

#endif
