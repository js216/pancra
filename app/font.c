// SPDX-License-Identifier: GPL-3.0
// font.c --- 5x7 bitmap glyphs for the UI
// Copyright 2026 Jakob Kastelic

/* font.c -- 5x7 bitmap glyphs for digits, A-Z, and the punctuation the UI uses.
 * See font.h. Pure data + lookup; no dependencies beyond stdint. */
#include "font.h"
#include <stdint.h>

static const uint8_t font_digit[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
};

static const uint8_t font_upper[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

static const uint8_t font_minus[7] = {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00};
static const uint8_t font_colon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
static const uint8_t font_dot[7]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
static const uint8_t font_bang[7]  = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
static const uint8_t font_slash[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
static const uint8_t font_plus[7]  = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
static const uint8_t font_lpar[7]  = {0x04, 0x08, 0x10, 0x10, 0x10, 0x08, 0x04};
static const uint8_t font_rpar[7]  = {0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x04};
static const uint8_t font_pct[7]   = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13};
static const uint8_t font_larrow[7] = {0x00, 0x04, 0x08, 0x1F,
                                       0x08, 0x04, 0x00}; /* backspace */
static const uint8_t font_uscore[7] = {0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x1F}; /* entry slot */
/* '>' marks the PRIMARY sensor in the list. Without a glyph it drew as a blank
 * cell, so the one thing that row exists to show -- which sensor owns the big
 * number -- was invisible and a primary row looked identical to any other. */
static const uint8_t font_rarrow[7] = {0x00, 0x08, 0x04, 0x1F,
                                       0x04, 0x08, 0x00};
/* ',' appears in the medical disclaimer; '?' in FORGET? and in an unknown
 * calibration-permitted state. Both rendered blank. */
/* '@': an O with a tail inside, open at the lower right. Needed because the
 * ACCOUNT field is an email address -- without a glyph the key on the text
 * editor draws BLANK, so the one character every address needs looks like a
 * dead space in the grid. */
static const uint8_t font_at[7] = {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0F};

static const uint8_t font_comma[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08};
static const uint8_t font_quest[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};

static const uint8_t font_lower[26][7] = {
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, /* a */
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}, /* b */
    {0x00, 0x00, 0x0F, 0x10, 0x10, 0x10, 0x0F}, /* c */
    {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}, /* d */
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, /* e */
    {0x06, 0x09, 0x08, 0x1E, 0x08, 0x08, 0x08}, /* f */
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, /* g */
    {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}, /* h */
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, /* i */
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}, /* j */
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, /* k */
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* l */
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15}, /* m */
    {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}, /* n */
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, /* o */
    {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}, /* p */
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}, /* q */
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, /* r */
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, /* s */
    {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}, /* t */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x0F}, /* u */
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* v */
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, /* w */
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, /* x */
    {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}, /* y */
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}, /* z */
};
/* THE PUNCTUATION THAT HAD NO GLYPH. Every one of these drew as a blank
 * cell, so a string containing it silently lost a character. */
static const uint8_t font_quote[7] = {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}; /* '"' */
static const uint8_t font_hash[7] = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}; /* '#' */
static const uint8_t font_dollar[7] = {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}; /* '$' */
static const uint8_t font_amp[7] = {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}; /* '&' */
static const uint8_t font_tick[7] = {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}; /* '\'' */
static const uint8_t font_star[7] = {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}; /* '*' */
static const uint8_t font_semi[7] = {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08}; /* ';' */
static const uint8_t font_equals[7] = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}; /* '=' */
static const uint8_t font_lbrack[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}; /* '[' */
static const uint8_t font_bslash[7] = {0x10, 0x10, 0x08, 0x04, 0x04, 0x02, 0x01}; /* '\\' */
static const uint8_t font_rbrack[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}; /* ']' */
static const uint8_t font_caret[7] = {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}; /* '^' */
static const uint8_t font_grave[7] = {0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}; /* '`' */
static const uint8_t font_lbrace[7] = {0x06, 0x04, 0x04, 0x08, 0x04, 0x04, 0x06}; /* '{' */
static const uint8_t font_pipe[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; /* '|' */
static const uint8_t font_rbrace[7] = {0x0C, 0x04, 0x04, 0x02, 0x04, 0x04, 0x0C}; /* '}' */
static const uint8_t font_tilde[7] = {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00}; /* '~' */

/* EVERY VISIBLE ASCII CHARACTER, indexed by `c - ' '`.
 *
 * A TABLE, NOT A CHAIN OF ifs. The chain this replaces answered for about a
 * third of the printable range and returned NULL for the rest -- and a NULL
 * is a blank cell, so a string containing an unlisted character lost it
 * silently, which is how '?' and ',' were once missing from a confirmation
 * and a disclaimer without anybody noticing. Laid out by character, the
 * question "is every visible character here" is answered by reading the
 * table, and a gap is a hole you can see.
 *
 * SPACE IS THE ONE DELIBERATE NULL: a blank cell is what a space IS.
 *
 * LOWERCASE IS ITS OWN SET now, rather than being folded onto the capitals.
 * The prose this app draws -- the gate's disclaimer, the device list's
 * instructions -- was written in mixed case and rendered in shouting
 * capitals; a sentence that looks like a warning when it is an explanation
 * is a small lie the font was telling. Callers that WANT capitals uppercase
 * their own text (the label editor does), which is a decision about the
 * text rather than about the alphabet. */
static const uint8_t *const font_ascii['~' - ' ' + 1] = {
    [' ' - ' '] = 0,  /* a blank cell, and the only one that is deliberate */
    ['0' - ' '] = font_digit[0],
    ['1' - ' '] = font_digit[1],
    ['2' - ' '] = font_digit[2],
    ['3' - ' '] = font_digit[3],
    ['4' - ' '] = font_digit[4],
    ['5' - ' '] = font_digit[5],
    ['6' - ' '] = font_digit[6],
    ['7' - ' '] = font_digit[7],
    ['8' - ' '] = font_digit[8],
    ['9' - ' '] = font_digit[9],
    ['A' - ' '] = font_upper[0],
    ['B' - ' '] = font_upper[1],
    ['C' - ' '] = font_upper[2],
    ['D' - ' '] = font_upper[3],
    ['E' - ' '] = font_upper[4],
    ['F' - ' '] = font_upper[5],
    ['G' - ' '] = font_upper[6],
    ['H' - ' '] = font_upper[7],
    ['I' - ' '] = font_upper[8],
    ['J' - ' '] = font_upper[9],
    ['K' - ' '] = font_upper[10],
    ['L' - ' '] = font_upper[11],
    ['M' - ' '] = font_upper[12],
    ['N' - ' '] = font_upper[13],
    ['O' - ' '] = font_upper[14],
    ['P' - ' '] = font_upper[15],
    ['Q' - ' '] = font_upper[16],
    ['R' - ' '] = font_upper[17],
    ['S' - ' '] = font_upper[18],
    ['T' - ' '] = font_upper[19],
    ['U' - ' '] = font_upper[20],
    ['V' - ' '] = font_upper[21],
    ['W' - ' '] = font_upper[22],
    ['X' - ' '] = font_upper[23],
    ['Y' - ' '] = font_upper[24],
    ['Z' - ' '] = font_upper[25],
    ['a' - ' '] = font_lower[0],
    ['b' - ' '] = font_lower[1],
    ['c' - ' '] = font_lower[2],
    ['d' - ' '] = font_lower[3],
    ['e' - ' '] = font_lower[4],
    ['f' - ' '] = font_lower[5],
    ['g' - ' '] = font_lower[6],
    ['h' - ' '] = font_lower[7],
    ['i' - ' '] = font_lower[8],
    ['j' - ' '] = font_lower[9],
    ['k' - ' '] = font_lower[10],
    ['l' - ' '] = font_lower[11],
    ['m' - ' '] = font_lower[12],
    ['n' - ' '] = font_lower[13],
    ['o' - ' '] = font_lower[14],
    ['p' - ' '] = font_lower[15],
    ['q' - ' '] = font_lower[16],
    ['r' - ' '] = font_lower[17],
    ['s' - ' '] = font_lower[18],
    ['t' - ' '] = font_lower[19],
    ['u' - ' '] = font_lower[20],
    ['v' - ' '] = font_lower[21],
    ['w' - ' '] = font_lower[22],
    ['x' - ' '] = font_lower[23],
    ['y' - ' '] = font_lower[24],
    ['z' - ' '] = font_lower[25],
    ['!' - ' '] = font_bang,
    ['"' - ' '] = font_quote,
    ['#' - ' '] = font_hash,
    ['$' - ' '] = font_dollar,
    ['%' - ' '] = font_pct,
    ['&' - ' '] = font_amp,
    ['\'' - ' '] = font_tick,
    ['(' - ' '] = font_lpar,
    [')' - ' '] = font_rpar,
    ['*' - ' '] = font_star,
    ['+' - ' '] = font_plus,
    [',' - ' '] = font_comma,
    ['-' - ' '] = font_minus,
    ['.' - ' '] = font_dot,
    ['/' - ' '] = font_slash,
    [':' - ' '] = font_colon,
    [';' - ' '] = font_semi,
    ['<' - ' '] = font_larrow,
    ['=' - ' '] = font_equals,
    ['>' - ' '] = font_rarrow,
    ['?' - ' '] = font_quest,
    ['@' - ' '] = font_at,
    ['[' - ' '] = font_lbrack,
    ['\\' - ' '] = font_bslash,
    [']' - ' '] = font_rbrack,
    ['^' - ' '] = font_caret,
    ['_' - ' '] = font_uscore,
    ['`' - ' '] = font_grave,
    ['{' - ' '] = font_lbrace,
    ['|' - ' '] = font_pipe,
    ['}' - ' '] = font_rbrace,
    ['~' - ' '] = font_tilde,
};

const uint8_t *glyph_for(char c)
{
   /* UNSIGNED, because `char` is signed on this target: a byte above 0x7F
    * arrives negative, and a signed compare would let it past the range test
    * and index the table from before its start. */
   const unsigned char u = (unsigned char)c;
   if (u < ' ' || u > '~')
      return 0; /* control byte or high bit set: not something we draw */
   return font_ascii[u - ' '];
}

int str_len(const char *s)
{
   int n = 0;
   while (s[n])
      n++;
   return n;
}

/* ---- 5x7 status icons: same format as the glyphs (7 rows, MSB = leftmost
 * of the low 5 bits), kept here so all the bitmap art lives in one file.
 * uirender.c's draw_icon blits them. ---- */

/* Speaker: driver box left, cone opening right. */
const uint8_t icon_speaker[7] = {0x01, 0x03, 0x1F, 0x1F, 0x1F, 0x03, 0x01};
/* A phone outline with a shake line each side. */
const uint8_t icon_vibrate[7] = {0x0E, 0x0A, 0x1B, 0x1B, 0x1B, 0x0A, 0x0E};
/* A pencil, tip lower-left: the universally read "edit this row" mark. */
const uint8_t icon_pencil[7] = {0x03, 0x07, 0x0E, 0x1C, 0x18, 0x10, 0x00};
/* A small filled disc: the NEW DATAPOINT beep indicator. */
const uint8_t icon_dot[7] = {0x00, 0x0E, 0x1F, 0x1F, 0x1F, 0x0E, 0x00};
/* UP AND DOWN ARROWS. Symbols, not letters: the alarm row marks its four
 * thresholds with them -- two down for the low alarm, one down for the low
 * nudge, one up for the high nudge, two up for the high alarm -- so which
 * band a number belongs to is readable without a word for it. They sit here
 * with the speaker and the dot because that is what they are; the font is for
 * text, and an arrow is not a character anybody types. */
const uint8_t icon_arrow_up[7] = {0x04, 0x0E, 0x1F, 0x04, 0x04, 0x04, 0x00};
const uint8_t icon_arrow_dn[7] = {0x00, 0x04, 0x04, 0x04, 0x1F, 0x0E, 0x04};

/* THE PAGER'S FOUR BUTTONS, as SOLID TRIANGLES rather than the font's '<'
 * and '>'.
 *
 * The end-stop pair were drawn as two characters, ">|", and that is what they
 * looked like: an arrow standing next to a bar, which reads as two controls
 * crowded together rather than one that means "the last page". Joined into a
 * single glyph -- the apex meeting the wall it stops against -- the pair say
 * what they do without being read twice.
 *
 * The plain pair are triangles too, so all four are one family. The font's
 * thin '<' keeps its own job as the keypad's backspace, where it is a
 * character in a label rather than a button. */
const uint8_t icon_pg_prev[7]  = {0x01, 0x03, 0x07, 0x0F, 0x07, 0x03, 0x01};
const uint8_t icon_pg_next[7]  = {0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10};
const uint8_t icon_pg_first[7] = {0x11, 0x13, 0x17, 0x1F, 0x17, 0x13, 0x11};
const uint8_t icon_pg_last[7]  = {0x11, 0x19, 0x1D, 0x1F, 0x1D, 0x19, 0x11};

/* A slashed circle -- "no data": the DISCONNECT (stale-data) alarm. */
const uint8_t icon_nolink[7] = {0x00, 0x0E, 0x13, 0x15, 0x19, 0x0E, 0x00};
/* Checkbox, empty and checked (an X filling the interior). */
const uint8_t icon_box[7]   = {0x00, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0x00};
const uint8_t icon_boxck[7] = {0x00, 0x1F, 0x1B, 0x15, 0x1B, 0x1F, 0x00};
/* Checkbox, SOLID. The PRIMARY column is a radio choice, not a set of
 * independent options, so its "on" state is a filled square rather than the
 * crossed box -- the two read as different kinds of answer at a glance. */
const uint8_t icon_boxfill[7] = {0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00};
