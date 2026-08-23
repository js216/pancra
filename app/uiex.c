// SPDX-License-Identifier: GPL-3.0
// uiex.c --- The EXERCISE LOG: the table, the correction form, the delete
// Copyright 2026 Jakob Kastelic

/* WHY THE EXERCISE LOG HAS A FORM AT ALL, when nothing here can create a row.
 *
 * Every other log in this app is fed by a form: you open LOG INSULIN, you type
 * the units, you confirm. Exercise is not -- its entries are made by a button
 * that cycles 0-1-2-3 and commits whatever it is left showing (exercise.h).
 * So there is no NEW EXERCISE screen and there must not be one: a second way
 * in would be a second definition of what an exercise record means, and the
 * settling rule is the definition.
 *
 * What this file is for is the other half, which the button cannot do:
 * CORRECTING what it wrote. A level cycled one press too far and left alone
 * for ten seconds is a row nobody meant; a walk logged at the wrong hour is a
 * row in the wrong place. Those are exactly the mistakes the insulin, weight
 * and food logs already let the user fix, and there is no reason this one
 * should be the exception.
 *
 * THE LEVEL IS CYCLED HERE TOO, not typed. Tapping the LEVEL row advances it
 * 1-2-3-1, the same three values the button offers, in the same colours
 * (ui_ex_color). A keypad would accept 0, 4 and 97, all of which
 * exercise_update refuses -- so it would be a control whose whole job is to
 * collect answers that get rejected. */
#include "exercise.h"
#include "font.h"  /* str_len: the page counter is centred on its width */
#include "style.h" /* the colour roles: UI_TEXT, UI_MUTED, ... */
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h" /* fmt_date: the instant, split into YEAR / MM-DD / HH:MM */
#include "uimodel.h"
#include "uipriv.h"
#include "util.h" /* str_snapshot */

#include "ndk.h"
#include <stdint.h>
#include <stdio.h>

/* The word for a level, which is the user's own judgement and not a
 * measurement -- exercise.h is deliberate about that. The number is shown
 * beside it because the number is what the button shows and what the file
 * holds; the word is what makes a table of bare 1s and 3s readable. */
static const char *ex_level_word(int level)
{
   switch (level) {
      case 1: return "LIGHT";
      case 2: return "MODERATE";
      case 3: return "HARD";
      default:
         /* Not reachable from a loaded record -- ex_parse_rec refuses anything
          * outside the domain -- but a form field is a live value and this is
          * what an out-of-range one should look like: obviously wrong, not
          * plausibly something. */
         return "?";
   }
}

void render_exedit(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   /* ALWAYS "EDIT": this form is only ever reached from a row of the log.
    * There is no logging counterpart to be confused with -- see the top of
    * this file. */
   draw_str(px, fb, x, y, tsc, "EDIT EXERCISE", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* X discards -- nothing is written before an explicit CONFIRM. The band is
    * 2*lh - 2*sc rather than 2*lh because value_row's target starts at its
    * y - 4*sc, and a full band reaches into the row below it. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc)),
              MA_EX_DISCARD, 0);
   y += 2 * lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; split it into YEAR / MM-DD / HH:MM
    * exactly as the insulin, weight and food forms do. */
   char dt[20];
   fmt_date(m->food.ex_t, m->tz_off, dt, sizeof dt);
   char yearp[8];
   char datep[8];
   char timep[8];
   str_snapshot(yearp, sizeof yearp, dt);
   if (str_len(yearp) > 4)
      yearp[4] = 0;
   str_snapshot(datep, sizeof datep, (str_len(dt) > 5) ? dt + 5 : "");
   if (str_len(datep) > 5)
      datep[5] = 0;
   str_snapshot(timep, sizeof timep, (str_len(dt) > 11) ? dt + 11 : "");

   char lv[24];
   (void)snprintf(lv, sizeof lv, "%s %d", ex_level_word(m->food.ex_form_level),
                  m->food.ex_form_level);
   /* IN ITS LEVEL'S COLOUR, so the field says the same thing the button on the
    * ADD menu says about the same number. */
   y = value_row(fb, h, y, sc, "LEVEL", lv,
                 ui_ex_color(m->food.ex_form_level, UI_TEXT), MA_EX_EDIT, 0);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, UI_TEXT, MA_EX_EDIT, 1);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, UI_TEXT, MA_EX_EDIT, 2);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, UI_TEXT, MA_EX_EDIT, 3);
   y += lh;
   /* HOW LONG IT LASTED, in minutes, and "--" when the log does not know.
    *
    * A row is written when the level SETTLES, which is its start, so nothing
    * knows the length until the session ends -- 0 is the column's own "not
    * known" (exercise.h) and it has to read as that rather than as a
    * zero-minute session. This is the one field on this form the button
    * cannot supply, which is why it is here. */
   char durp[12];
   /* CLAMPED FOR THE RENDERER ONLY. exercise_update refuses anything past
    * EX_DUR_MAX, so a row this large cannot be written -- but the frame is
    * built from whatever the file held, and a screen must not be the place a
    * corrupted column is discovered. */
   long durm = m->food.ex_form_dur / 60;
   if (durm > EX_DUR_MAX / 60)
      durm = EX_DUR_MAX / 60;
   if (durm > 0)
      (void)snprintf(durp, sizeof durp, "%ld", durm);
   else
      (void)snprintf(durp, sizeof durp, "--");
   /* A RUNNING SESSION HAS NO LENGTH YET, and this row says so instead of
    * offering to collect one. The end press is what writes it -- so the field
    * is not a control here (action -1, no tap target) and reads ACTIVE in the
    * colour the rest of the app uses for something in progress. It becomes an
    * ordinary editable number the moment the button goes back to zero. */
   if (m->food.ex_running) {
      y = value_row(fb, h, y, sc, "MINUTES", "ACTIVE", UI_BUSY, -1, 0);
   } else {
      y = value_row(fb, h, y, sc, "MINUTES", durp, UI_TEXT, MA_EX_EDIT, 4);
   }
   /* WHY THE LAST CONFIRM WAS REFUSED, in the air that already separates the
    * fields from the buttons -- it COSTS NO ROW, so the budget above stands
    * unchanged and no label on this screen gets smaller for it.
    *
    * It has to be here at all because the global status line is drawn on the
    * main screen only: a refusal announced there while this form is open is
    * one nobody sees, and CONFIRM's only other feedback is the screen NOT
    * closing, which is indistinguishable from a tap that missed. */
   if (m->food.ex_err[0])
      draw_str(px, fb, x, y, sc, m->food.ex_err, UI_DANGER);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide rule
    * every other form here follows. DELETE (red) sits between them, and only
    * opens a confirmation; it never deletes on the tap itself. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_EX_DISCARD, 0);
   y += (3 * lh) / 2;
   y = menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_EX_DELETE, 0);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_OK, MA_EX_CONFIRM, 0);
}

void render_exdel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 20);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE?", UI_DANGER);
   y += 3 * lh;
   /* THE ORIGINAL ROW, not the form's current values. This names what
    * exercise_delete will actually remove, and the two differ the moment the
    * user edits a field and then reaches for DELETE. */
   char ev[40];
   char when[20];
   (void)snprintf(ev, sizeof ev, "%s %d", ex_level_word(m->food.ex_orig_level),
                  m->food.ex_orig_level);
   fmt_date(m->food.ex_orig_t, m->tz_off, when, sizeof when);
   draw_str(px, fb, x, y, sc, ev, ui_ex_color(m->food.ex_orig_level, UI_TEXT));
   y += lh;
   draw_str(px, fb, x, y, sc, when, UI_TEXT_DIM);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "This cannot be undone.", UI_MUTED);
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_EXDEL_NO, 0);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_EXDEL_YES, 0);
}

void render_exlog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "EXERCISE LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_EXLOG_BACK, 0);
   y += 3 * lh;

   if (m->food.nexlog <= 0) {
      draw_str(px, fb, x, y, sc, "Nothing logged yet.", UI_MUTED);
      return;
   }
   draw_str(px, fb, x, y, sc, "TIME              LEVEL", UI_MUTED);
   y += lh;

   int avail = fb->height - y - (2 * lh);
   int per   = avail / lh;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (m->food.nexlog + per - 1) / per;
   int page   = m->food.exlog_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < m->food.nexlog; r++) {
      /* the tail is oldest-first; the table shows newest first */
      int ti                 = m->food.nexlog - 1 - r;
      const struct ex_rec *e = &m->food.exlog[ti];
      char when[20];
      char row[56];
      fmt_date(e->t, m->tz_off, when, sizeof when);
      (void)snprintf(row, sizeof row, "%s  %s %d", when,
                     ex_level_word(e->level), e->level);
      /* THE WHOLE ROW IN ITS LEVEL'S COLOUR. A log of exercise is read for
       * its shape -- when the hard days were -- and the colour is what makes
       * that visible in a column of near-identical timestamps. */
      draw_str(px, fb, x, y, sc, row, ui_ex_color(e->level, UI_TEXT_DIM));
      /* THE WHOLE ROW is the target, carrying the TAIL INDEX -- which the
       * dispatcher immediately turns into a copy of the row itself, because
       * an index is only good for as long as the tail is. */
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_EXLOG_EDIT, ti);
      y += lh;
   }

   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(0, navy - (3 * sc), fb->width / 3, lh + (7 * sc)),
                    MA_EXLOG_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               UI_MUTED);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), navy, tsc, ">", UI_TEXT);
         add_hit_ix(h,
                    ui_rect(fb->width - (fb->width / 3), navy - (3 * sc),
                            fb->width / 3, lh + (7 * sc)),
                    MA_EXLOG_NEXT, 0);
      }
   }
}
