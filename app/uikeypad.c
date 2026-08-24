// SPDX-License-Identifier: GPL-3.0
// uikeypad.c --- The two screens the user types on (see uipriv.h)
// Copyright 2026 Jakob Kastelic
//
/* A NUMERIC KEYPAD AND A LETTER GRID, and they are one file because they are
 * one problem: a bounded entry field, keys that are comfortable targets on a
 * phone, and an OK that must be visibly DEAD when what has been typed cannot
 * be committed. Both ask keypad.h what a mode is, so splitting them would put
 * half the callers of that description on the other side of a boundary. */

#include "uikeypad.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: the alarm keypads' ceiling */
#include "font.h"
#include "keypad.h" /* what each mode IS: slots, title, dot, unit */
#include "ndk.h"
#include "style.h" /* the colour roles: UI_TEXT, UI_MUTED, ... */
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimodel.h"
#include "uipriv.h"
#include "weight.h" /* wt_unit_name / wt_to_tenths: the weight rows */
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* ---- rename ----
 * Labels matter most when several identical devices are paired (two meters
 * look the same in a list), so this is a plain letter grid rather than a
 * digits-only keypad. 6 columns keeps every key a comfortable target. */

/* The dot is here for host names (SERVER): without it "pancra.org" cannot
 * be typed at all, and the at-sign for the account email. Sensor labels
 * simply never use either.
 *
 * The underscore and plus are here for the same reason, found later: an owner
 * whose account is jane_smith@... or me+pancra@... could not type their own
 * address, main.c then refused to save the malformed result, and pairing was
 * impossible with nothing on screen connecting the two facts. Both are common
 * in real addresses -- '+' is the standard gmail tag separator.
 *
 * 42 characters is exactly 7 full rows of 6, so this costs no extra row. */
const char ui_label_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.@_+";
#define UI_LABEL_COLS 6

/* (No assertion is needed here that this array has not grown into MA_WTTAB's
 * action range: a key is (MA_CHAR, index), so however long this string gets
 * it cannot reach another control. With a base+index range it could -- a tap
 * on the weight plot's "1M" tab dispatching a letter into the rename
 * keypad.) */

void render_label(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded. Left on width-only scaling, the row pitch this produces is
    * spent on height: the key grid's cell height collapses and pad_key then
    * subtracts from it with no floor, yielding NEGATIVE-height hit boxes. The
    * keys still DRAW, so the screen looks normal -- but ui_hit needs
    * `y < by + h`, which a negative h makes unsatisfiable, so none of them
    * respond. In landscape that meant no pairing code, no calibration value and
    * no plot max could be entered at all. */
   int sc  = ui_fit_scale(fb->width, fb->height, 30);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   int ty  = y;

   /* What the text keypad is collecting. Named here rather than in the call,
    * where three nested conditionals hid which was the default. */
   const char *what = "NAME";
   if (m->entry.label_field == 1)
      what = "SERVER";
   else if (m->entry.label_field == 2)
      what = "EMAIL";
   draw_str(px, fb, x, y, tsc, what, UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   y += 2 * lh;

   /* What has been typed so far, with a caret so an empty field is still
    * obviously an entry field.
    *
    * WRAPPED, not shrunk. One line in a 16-byte buffer is fine for a sensor
    * name and useless for an email address: anything past 15 characters is
    * simply not shown. Shrinking the text to fit would
    * make a long address unreadable exactly when the user most needs to check
    * it letter by letter, so the text keeps its size and takes as many lines
    * as it needs. */
   /* Zeroed: the analyser cannot see that the fill below stops at the cursor
    * and that str_len then bounds every read to it. */
   char shown[80] = {0};
   const char *en = m->entry.entry ? m->entry.entry : "";
   int k          = 0;
   while (en[k] && k < (int)sizeof shown - 2) {
      shown[k] = en[k];
      k++;
   }
   shown[k]     = '_';
   shown[k + 1] = 0;
   int dsc      = 2 * sc;
   int percol   = fb->width / (6 * dsc); /* characters that fit on one line */
   if (percol < 8)
      percol = 8;
   int shownlen = str_len(shown);
   for (int off = 0; off < shownlen; off += percol) {
      char line[80];
      int len = shownlen - off;
      if (len > percol)
         len = percol;
      for (int i = 0; i < len; i++)
         line[i] = shown[off + i];
      line[len] = 0;
      int dw    = len * 6 * dsc;
      draw_str(px, fb, (fb->width - dw) / 2, y, dsc, line, UI_OK);
      y += 7 * dsc;
   }
   y += 8 * sc;
   add_hit_ix(h, ui_rect(0, ty - (3 * sc), fb->width, y - (ty - (3 * sc))),
              MA_KP_CLOSE, 0);

   int n      = ui_label_nchars();
   int rows   = (n + UI_LABEL_COLS - 1) / UI_LABEL_COLS;
   int gm     = fb->width / 24;
   int gw     = fb->width - (2 * gm);
   int cw     = gw / UI_LABEL_COLS;
   int bottom = fb->height - (fb->height / 20);
   int ch     = (bottom - y) / (rows + 1); /* +1 row for DEL / OK */
   int ksc    = (ch - (4 * sc)) / 7;
   if (ksc < sc)
      ksc = sc;
   if (ksc > 3 * sc)
      ksc = 3 * sc;
   for (int i = 0; i < n; i++) {
      int cx      = gm + ((i % UI_LABEL_COLS) * cw);
      int cy      = y + ((i / UI_LABEL_COLS) * ch);
      char lbl[2] = {ui_label_chars[i], 0};
      draw_str(px, fb, cx + ((cw - (6 * ksc)) / 2), cy + ((ch - (7 * ksc)) / 2),
               ksc, lbl, UI_TEXT);
      add_hit_ix(h, ui_rect(cx, cy, cw, ch), MA_CHAR, i);
   }
   /* DEL and OK share the last row */
   int cy = y + (rows * ch);
   int hw = gw / 2;
   draw_frame(px, fb, gm + (2 * sc), cy + (2 * sc), hw - (4 * sc),
              ch - (4 * sc), UI_RULE);
   draw_str(px, fb, gm + ((hw - (3 * 6 * ksc)) / 2),
            cy + ((ch - (7 * ksc)) / 2), ksc, "DEL", UI_TEXT);
   add_hit_ix(h, ui_rect(gm, cy, hw, ch), MA_BACKSPACE, 0);
   draw_frame(px, fb, gm + hw + (2 * sc), cy + (2 * sc), hw - (4 * sc),
              ch - (4 * sc), UI_RULE);
   draw_str(px, fb, gm + hw + ((hw - (2 * 6 * ksc)) / 2),
            cy + ((ch - (7 * ksc)) / 2), ksc, "OK", UI_OK);
   add_hit_ix(h, ui_rect(gm + hw, cy, hw, ch), MA_OK, 0);
}

/* One keypad key: framed cell, centred label (in `col`), full-cell ACT_MENU
 * target. code < 0 = a DISABLED key: drawn but recording no target, so it
 * cannot fire (the grayed-out OK of an over-limit entry). */
static void pad_key(struct ANativeWindow_Buffer *fb, struct hits *h, int cx,
                    int cy, int cw, int ch, int ksc, const char *lab, int code,
                    int ix, uint32_t col)
{
   uint32_t *px = fb->bits;
   draw_frame(px, fb, cx, cy, cw, ch, UI_RULE);
   int lw  = str_len(lab) * 6 * ksc;
   int lhh = 7 * ksc;
   draw_str(px, fb, cx + ((cw - lw) / 2), cy + ((ch - lhh) / 2), ksc, lab, col);
   if (code >= 0)
      add_hit_ix(h, ui_rect(cx, cy, cw, ch), code, ix);
}

/* Pairing / plot-max keypad: a title, a fixed-width entry field, and a 3x4
 * digit grid. Every key carries a NAMED code and, separately, which key it
 * is. Spelling the digits as the bare integers 100..109 is the base+index
 * namespace at its least visible: nothing in "101, 102, 103" says "digit one,
 * two, three", and nothing complains if MA_BACKSPACE is renumbered on top of
 * them. */

void render_keypad(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded. Left on width-only scaling, the row pitch this produces is
    * spent on height: the key grid's cell height collapses and pad_key then
    * subtracts from it with no floor, yielding NEGATIVE-height hit boxes. The
    * keys still DRAW, so the screen looks normal -- but ui_hit needs
    * `y < by + h`, which a negative h makes unsatisfiable, so none of them
    * respond. In landscape that meant no pairing code, no calibration value and
    * no plot max could be entered at all. */
   int sc  = ui_fit_scale(fb->width, fb->height, 30);
   int tsc = 2 * sc;
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   int ty = y;
   char pair_title[24];
   const char *kp_title = pair_title;
   /* TABLE-DRIVEN, AND THE FALLBACK IS AN ERROR, NOT A SCREEN.
    *
    * An if/else chain ending in `else -> pairing keypad` renders a REAL,
    * PLAUSIBLE screen for "no title for this mode": a mode added without a
    * title silently becomes PAIR NEW <sensor>, so tapping a weight opens the
    * sensor-pairing flow and
    * nothing about it looked wrong. A default branch must never name a
    * different feature. Pairing is KP_PAIR_CODE and ONLY that; anything unknown
    * says so, in red, where it cannot be mistaken for working. */
   uint32_t title_col = UI_TEXT;
   if (m->entry.kp_mode == KP_PAIR_CODE) {
      (void)snprintf(pair_title, sizeof pair_title, "PAIR NEW %s",
                     m->dev.add_type ? m->dev.add_type : "SENSOR");
   } else if (kp_info(m->entry.kp_mode)->title) {
      kp_title = kp_info(m->entry.kp_mode)->title;
   } else {
      (void)snprintf(pair_title, sizeof pair_title, "BAD KP MODE %d",
                     m->entry.kp_mode);
      title_col = UI_DANGER; /* red: a bug, not a feature */
   }
   /* Leave room for the X, which is right-aligned at 6*tsc. */
   (void)draw_title_fit(px, fb, x, y, tsc, kp_title, title_col,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   y += 2 * lh;
   if (kp_is_thresh(m->entry.kp_mode)) {
      /* the accepted ceiling, in the entry's own units, then a blank row;
       * the key grid below sizes itself into whatever height remains */
      char mv[8];
      char mx[24];
      fmt_glu(AL_ENTRY_MAX, m->prefs.units, mv, sizeof mv);
      (void)snprintf(mx, sizeof mx, "MAX: %s %s", mv, UI_LBL(m->prefs.units));
      draw_str(px, fb, x, y, sc, mx, UI_MUTED);
      y += 2 * lh;
   }

   /* WHY the last entry was refused, in red, where the eye already is.
    * Answering a rejected value by clearing the field and saying nothing at
    * all reads exactly like a mistyped key, so the user retypes the same
    * rejected value -- weeks of a nudge threshold that is never accepted
    * looking like a nudge that no longer works. */
   if (m->entry.kp_err[0]) {
      draw_str(px, fb, x, y, sc, m->entry.kp_err, UI_SYNC_ERR);
      y += 2 * lh;
   }

   /* Entry field: one underscore per slot, replaced by digits as typed, so the
    * width never shifts. Plot-max shows the unit after the value; pair shows
    * the 4 code digits; the remote IP gets a full dotted quad's 15 slots and
    * the remote port a TCP port's 5. dsc is sized for the widest label so the
    * field -- and the keypad below -- is identical across modes. */
   int nslots = kp_slots(m->entry.kp_mode);
   /* which unit suffix this mode carries, if any -- the table decides */
   enum kp_unit un = kp_info(m->entry.kp_mode)->unit;
   const char *en  = m->entry.entry ? m->entry.entry : "";
   char shown[24];
   int k = 0;
   /* Both bounds are belt-and-braces: the widest mode is 15 slots plus
    * " MMOL/L". snprintf returns what it WOULD have written, so an unclamped
    * k would index past `shown` the moment a longer unit or a wider keypad
    * appeared -- a silent stack overrun for a one-character change. */
   for (int i = 0; i < nslots && k < (int)sizeof shown - 1; i++)
      shown[k++] = *en ? *en++ : '_'; /* typed digits, then '_' for the rest */
   /* The suffix comes from the mode's own row, never from a guess about what
    * kind of number this is -- see enum kp_unit. No default: a new unit must
    * fail to compile here rather than inherit somebody else's label. */
   const char *ul = 0;
   switch (un) {
      case KP_UNIT_NONE: break;
      case KP_UNIT_GLU: ul = UI_LBL(m->prefs.units); break;
      case KP_UNIT_WT: ul = wt_unit_name(m->prefs.wunits); break;
      case KP_UNIT_G: ul = "G"; break;
   }
   if (ul) {
      int w = snprintf(shown + k, sizeof shown - k, " %s", ul);
      if (w > 0)
         k += (w < (int)sizeof shown - k) ? w : (int)sizeof shown - k - 1;
   }
   shown[k] = 0;
   /* Size the field from what is actually shown ("___ MMOL/L" = 10 cells, the
    * 15-slot IP is wider still), so no mode overflows the margins. */
   int fcells = str_len(shown);
   if (fcells < 10)
      fcells = 10;
   int dsc = (fb->width - (8 * sc)) / (fcells * 6);
   if (dsc > 4 * sc)
      dsc = 4 * sc;
   if (dsc < sc)
      dsc = sc;
   int dw = str_len(shown) * 6 * dsc;
   draw_str(px, fb, (fb->width - dw) / 2, y, dsc, shown, UI_OK);
   y += (7 * dsc) + (12 * sc);

   /* Generous close target: the whole area above the keypad closes it. */
   add_hit_ix(h, ui_rect(0, ty - (3 * sc), fb->width, y - (ty - (3 * sc))),
              MA_KP_CLOSE, 0);

   /* 3x4 grid: digits, then 0 / DEL / OK. The title's X cancels. The IP mode
    * -- and the two alarm-threshold modes in mmol/L, where the value carries
    * a decimal ("5.5") -- need a 13th key ('.'), so they swap to a 5-row
    * layout: the dot takes 0's old cell, 0 and DEL shift right, and OK
    * becomes a full-width bottom row (which also makes the confirm harder to
    * fat-finger from DEL). */
   int dotkey = kp_has_dot(m->entry.kp_mode, m->prefs.units);
   int iprows = dotkey ? 5 : 4;
   int gm     = fb->width / 12;
   int gw     = fb->width - (2 * gm);
   int cw     = gw / 3;
   int bottom = fb->height - (fb->height / 20);
   int ch     = (bottom - y) / iprows;
   int wfit   = (cw - (4 * sc)) / (3 * 6); /* widest label "DEL" fits width */
   int hfit   = (ch - (4 * sc)) / 7;
   int ksc    = wfit < hfit ? wfit : hfit;
   if (ksc < sc)
      ksc = sc;
   static const char *keys[12]   = {"7", "8", "9", "4", "5",   "6",
                                    "1", "2", "3", "0", "DEL", "OK"};
   static const int acts[12]     = {MA_DIGIT, MA_DIGIT, MA_DIGIT,     MA_DIGIT,
                                    MA_DIGIT, MA_DIGIT, MA_DIGIT,     MA_DIGIT,
                                    MA_DIGIT, MA_DIGIT, MA_BACKSPACE, MA_OK};
   static const int actix[12]    = {7, 8, 9, 4, 5, 6, 1, 2, 3, 0, 0, 0};
   static const char *ipkeys[12] = {"7", "8", "9", "4", "5", "6",
                                    "1", "2", "3", ".", "0", "DEL"};
   static const int ipacts[12]   = {MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_DIGIT,
                                    MA_DIGIT, MA_DIGIT, MA_DIGIT, MA_DIGIT,
                                    MA_DIGIT, MA_DOT,   MA_DIGIT, MA_BACKSPACE};
   static const int ipactix[12]  = {7, 8, 9, 4, 5, 6, 1, 2, 3, 0, 0, 0};
   const char **kk               = dotkey ? ipkeys : keys;
   const int *aa                 = dotkey ? ipacts : acts;
   const int *ai                 = dotkey ? ipactix : actix;
   /* A value too LARGE for this mode's field DISABLES OK -- drawn gray,
    * recording no target, so an over-limit entry cannot even be submitted;
    * deleting digits re-enables it. Only modes with a numeric ceiling
    * participate (glucose entries 400, the alarm thresholds AL_ENTRY_MAX,
    * the TCP port 65535); a code/IP/date entry never disables here. */
   long okmax = 0; /* this mode's ceiling in comparable units; 0 = none */
   long okval = 0;
   {
      long ipart = 0;
      int frac   = -1;
      int sdot   = 0;
      /* NOT `en` -- the entry-field loop above consumed that pointer */
      for (const char *p = m->entry.entry ? m->entry.entry : ""; *p; p++) {
         if (*p == '.') {
            sdot = 1;
         } else if (*p >= '0' && *p <= '9') {
            if (!sdot)
               ipart = (ipart * 10) + (*p - '0');
            else if (frac < 0)
               frac = *p - '0';
         }
      }
      if (m->entry.kp_mode == KP_PLOT_MAX || m->entry.kp_mode == KP_CALIB ||
          m->entry.kp_mode == KP_RESCALE) {
         /* display units, mmol as TENTHS (no dot key in these modes) */
         okval = m->prefs.units ? (ipart * 18) / 10 : ipart;
         okmax = 400;
      } else if (kp_is_thresh(m->entry.kp_mode)) {
         long tenths = (ipart * 10) + (frac > 0 ? frac : 0);
         okval       = m->prefs.units ? (tenths * 18) / 10 : ipart;
         okmax       = AL_ENTRY_MAX;
      } else if (m->entry.kp_mode == KP_PORT) {
         okval = ipart;
         okmax = 65535;
      }
   }
   int ok_dead = okmax > 0 && okval > okmax;
   for (int r = 0; r < 4; r++)
      for (int col = 0; col < 3; col++) {
         int idx      = (r * 3) + col;
         int code     = aa[idx];
         uint32_t kcl = UI_TEXT;
         if (code == MA_OK && ok_dead) {
            code = -1;
            kcl  = UI_RULE;
         }
         pad_key(fb, h, gm + (col * cw), y + (r * ch), cw - (2 * sc),
                 ch - (2 * sc), ksc, kk[idx], code, ai[idx], kcl);
      }
   if (dotkey)
      pad_key(fb, h, gm, y + (4 * ch), (3 * cw) - (2 * sc), ch - (2 * sc), ksc,
              "OK", ok_dead ? -1 : MA_OK, 0, ok_dead ? UI_RULE : UI_TEXT);
}

/* Pairing candidate picker: scanned sensors strongest-first; a tap pairs one
 * (MA_DEV_PICK with the index), the X cancels (MA_DEV_CANCEL). */

int ui_label_nchars(void)
{
   return (int)(sizeof ui_label_chars) - 1;
}
