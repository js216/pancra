// SPDX-License-Identifier: GPL-3.0
// style.h --- how a datapoint is drawn: the vocabulary, and nothing else
// Copyright 2026 Jakob Kastelic
/*
 * THE SHARED PRESENTATION VOCABULARY, owned by neither of its two users.
 *
 * A marker shape belongs to the REGISTRY, because a slot stores the shape the
 * user picked for that device. A colour count belongs to SETTINGS, because
 * the palette is a preference. So sensors.h declared the shapes and included
 * settings.h for the palette, and settings.h declared the palette and
 * included sensors.h for the shapes -- each reaching into the other for half
 * of one idea, and the two headers importing each other in a cycle no
 * compiler would ever complain about.
 *
 * The idea is one idea: how a point is drawn. It has no implementation, no
 * state and no dependencies, so it is a leaf that both can name. The values
 * 0..MARK_HIDE are FROZEN -- they are persisted in slots.csv -- and new
 * variants are appended so old files keep their meaning.
 */
#ifndef PANCRA_STYLE_H
#define PANCRA_STYLE_H

/* MARK_HIDE is not a plot.c shape: it means "do not draw this device's
 * points" and is handled in the renderer by skipping them. Keep it last
 * before MARK_N so the drawable shapes stay 0..MARK_TRIANGLE. */
enum {
   MARK_DOT = 0,    /* small filled dot */
   MARK_CROSS,      /* X (no fill variant) */
   MARK_SQUARE,     /* empty square */
   MARK_TRIANGLE,   /* empty triangle */
   MARK_HIDE,       /* not drawn */
   MARK_SQUARE_F,   /* filled square */
   MARK_TRIANGLE_F, /* filled triangle */
   MARK_CIRCLE,     /* empty circle */
   MARK_CIRCLE_F,   /* filled circle */
   MARK_N
};

#define MARK_SIZE_DEF 2
#define MARK_SIZE_MAX 5

/* How many colours the palette offers. ui_sensor_colors[] is private to the
 * renderer, so settings_load cannot bound a stored colour index against the
 * array itself; this is the shared name for that count, and the renderer
 * static-asserts that the two agree. The by-eye version of that agreement is
 * exactly what let MARK_SIZE_MAX drift to a stale literal once already. */
#define SET_NCOLORS 7

/* ---- THE COLOUR ROLES ------------------------------------------------
 *
 * THEY LIVE IN lib/colors.h, all of them, one line each. What naming a role
 * buys is that a colour used twice is visibly the same DECISION and a colour
 * used once keeps its reason beside it; what one file buys on top of that is
 * that the palette can be read in full without hunting. Domain colours -- the
 * threshold bands, the exercise levels, the medical scale -- are in there too
 * rather than beside their use, because "where is that blue defined" had no
 * answer while half of them were literals in renderers.
 *
 * THE BYTE ORDER IS STATED ONCE, HERE. The framebuffer is ABGR8888, so a
 * literal reads backwards -- 0xFF4466FF is red, not blue, and it has been
 * misread as blue in review more than once. ui_abgr spells it out. */
#include "colors.h"

static inline unsigned ui_abgr(unsigned r, unsigned g, unsigned b)
{
   return 0xFF000000U | (b << 16U) | (g << 8U) | r;
}

#endif
