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
 * WHAT NAMING THEM BUYS. Nine hundred raw ABGR literals spread over eight
 * renderer files is not a palette: `0xFF888888` appears a hundred and
 * forty-eight times and means five different things -- a frame, a label, an
 * inactive tab, a secondary value, the dimmed big number -- while three
 * different literals
 * were used for "the destructive button" in three files. Nothing could be
 * changed once: a reader could not tell which greys were the same grey ON
 * PURPOSE, and a writer picking a colour had no way to ask what the last
 * person meant.
 *
 * So: a role has a name, and the name is what the renderer says. Two things
 * follow that are worth having. A colour used twice is now visibly the same
 * decision, and a colour used once with a REASON keeps its literal beside
 * that reason (the alarm banner's red, the nudge amber, the trend markers)
 * -- those are domain colours, not roles, and they stay where they are.
 *
 * THE BYTE ORDER IS STATED ONCE, HERE. The framebuffer is ABGR8888, so a
 * literal reads backwards -- 0xFF4466FF is red, not blue, and it has been
 * misread as blue in review more than once. ui_abgr spells it out. */
static inline unsigned ui_abgr(unsigned r, unsigned g, unsigned b)
{
   return 0xFF000000U | (b << 16U) | (g << 8U) | r;
}

/* Text, in the four weights the screens actually use. */
#define UI_TEXT     0xFFFFFFFFU /* primary: values, titles, active tabs */
#define UI_TEXT_DIM 0xFFCCCCCCU /* body copy, timestamps, explanations */
#define UI_MUTED    0xFF888888U /* labels, frames, inactive tabs */
#define UI_FAINT    0xFFAAAAAAU /* present but not available: disabled rows */
#define UI_RULE     0xFF555555U /* separators and the tracks under bars */

/* The three things a control can be saying. */
#define UI_OK     0xFF33FF88U /* confirmed, connected, in range */
#define UI_WARN   0xFF00CCFFU /* pending, waiting, needs attention */
#define UI_DANGER 0xFF4466FFU /* destructive: delete, forget, disconnect */
#define UI_ALERT  0xFF0000FFU /* the alarm's own red */
#define UI_BUSY   0xFF44CCFFU /* amber: a request is in flight */
#define UI_GO     0xFF00FF00U /* the affirmative half of a confirmation */

#endif
