// SPDX-License-Identifier: GPL-3.0
// uifood.c --- The FOOD TYPE picker
// Copyright 2026 Jakob Kastelic

/* THE PICKER IS A SCREEN OF ITS OWN, and it is the FIRST question rather than
 * a field inside the entry form.
 *
 * Logging food is two decisions -- which food, then how much -- and they are
 * not symmetric: the portion is a number you can always type, but the food is
 * a word you may not have entered yet. So the FOOD button opens this list
 * directly, and it returns to the entry form with the choice made. A form that
 * opened first and offered a TYPE field would put the one step that can fail
 * (the vocabulary does not have this food yet) behind the one that cannot.
 *
 * WHERE IT GOES BACK TO IS RECORDED, NEVER INFERRED. This screen is reached
 * from the ADD menu and from the entry form's TYPE row, and it will be reached
 * from elsewhere later; deciding the exit target here by looking at the app's
 * state is the bug that has recurred about six times in this codebase. The
 * origin is written down on the way in -- see nav.h -- and read on the way
 * out. */
#include "food.h"
#include "font.h" /* str_len: the page counter is centred on its width */
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h" /* fmt_date: the instant, split into YEAR / MM-DD / HH:MM */
#include "uimodel.h"
#include "uipriv.h"
#include "util.h"  /* str_snapshot */

#include <stdio.h>

/* The LOG FOOD entry form: which food, how much, and when.
 *
 * TYPE IS FIRST AND IT IS NOT A KEYPAD. Tapping it reopens the picker, which
 * returns here with the choice made -- the same screen the FOOD button opens
 * directly, so there is one way to choose a food rather than two. Everything
 * below it is a number, laid out in the order the other two forms use so the
 * three read alike. */
void render_food(struct ANativeWindow_Buffer *fb, const struct screen *m,
                 struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 26);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, m->food.food_edit ? "EDIT FOOD" : "LOG FOOD",
            0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   /* X discards -- nothing is written before an explicit CONFIRM. The band is
    * 2*lh - 2*sc rather than 2*lh because value_row's target starts at its
    * y - 4*sc, and a full band reaches into the row below it. */
   add_hit_ix(h, 0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc),
              MA_FOOD_DISCARD, 0);
   y += 2 * lh;

   /* fmt_date renders "YYYY-MM-DD HH:MM"; split it into YEAR / MM-DD / HH:MM
    * exactly as the insulin and weight forms do. */
   char dt[20];
   fmt_date(m->food.food_t, m->tz_off, dt, sizeof dt);
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

   /* NO FOOD CHOSEN READS AS A PROMPT, not as a blank. An empty value column
    * on the one row that must be filled looks like a rendering fault; the
    * word says what to do about it. */
   const char *tname = m->food.food_type != FOOD_TYPE_NONE
                           ? food_type_name(m->food.food_type)
                           : "CHOOSE...";
   uint32_t tcol =
       m->food.food_type != FOOD_TYPE_NONE ? 0xFFFFFFFF : 0xFF888888;
   y = value_row(fb, h, y, sc, "TYPE", tname, tcol, MA_FOOD_EDIT, 0);
   y += lh;
   char gval[16];
   (void)snprintf(gval, sizeof gval, "%d G", m->food.food_g);
   y = value_row(fb, h, y, sc, "GRAMS", gval, 0xFFFFFFFF, MA_FOOD_EDIT, 1);
   y += lh;
   y = value_row(fb, h, y, sc, "TIME", timep, 0xFFFFFFFF, MA_FOOD_EDIT, 2);
   y += lh;
   y = value_row(fb, h, y, sc, "DATE", datep, 0xFFFFFFFF, MA_FOOD_EDIT, 3);
   y += lh;
   y = value_row(fb, h, y, sc, "YEAR", yearp, 0xFFFFFFFF, MA_FOOD_EDIT, 4);
   y += 2 * lh;

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide rule
    * the insulin and weight forms both follow. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", 0xFFFFFFFF, MA_FOOD_DISCARD,
                   0);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "CONFIRM", 0xFF33FF88,
                     MA_FOOD_CONFIRM, 0);
}

/* THE FOOD LOG: what was eaten, when, and how much.
 *
 * The insulin log's shape, and deliberately so -- the two answer the same
 * question about different records, and a person who has read one should not
 * have to learn the other. Newest first, paginated, and the page count capped
 * by the HIT BUDGET as well as by height (render_inslog carries the argument:
 * add_hit drops targets past UI_MAX_HITS, and a dropped one draws perfectly
 * while being dead to touch).
 *
 * NO EDIT TARGET PER ROW, which is where it differs. The insulin log opens a
 * dose in its form because insulin.c can rewrite a row by content; food has
 * no such rewrite yet, so a row here would open a form whose CONFIRM appends
 * a second entry rather than amending this one. A row that looks tappable and
 * silently duplicates the record is worse than a row that does nothing, so
 * until food_update exists the rows are text. */
void render_foodlog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "FOOD LOG", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_FOODLOG_BACK, 0);
   y += 3 * lh;

   if (m->food.nlog <= 0) {
      draw_str(px, fb, x, y, sc, "Nothing logged yet.", 0xFF888888);
      return;
   }
   draw_str(px, fb, x, y, sc, "TIME              FOOD            G", 0xFF888888);
   y += lh;

   int avail = fb->height - y - (2 * lh);
   int per   = avail / lh;
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (m->food.nlog + per - 1) / per;
   int page   = m->food.log_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   for (int r = page * per; r < (page + 1) * per && r < m->food.nlog; r++) {
      /* the tail is oldest-first; the table shows newest first */
      int ti                   = m->food.nlog - 1 - r;
      const struct food_rec *e = &m->food.log[ti];
      char when[20];
      char row[56];
      fmt_date(e->t, m->tz_off, when, sizeof when);
      /* THE NAME, NOT THE ID. An entry whose type is missing renders as an
       * empty column rather than a number nobody can read -- food_type_name
       * answers "" for an id no type has, which is the honest look of a
       * record the vocabulary can no longer name. */
      (void)snprintf(row, sizeof row, "%s  %-14s %4ld", when,
                     food_type_name(e->type), e->g);
      draw_str(px, fb, x, y, sc, row, 0xFFCCCCCC);
      y += lh;
   }

   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", 0xFFFFFFFF);
         add_hit_ix(h, 0, navy - (3 * sc), fb->width / 3, lh + (7 * sc),
                    MA_FOODLOG_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit_ix(h, fb->width - (fb->width / 3), navy - (3 * sc),
                    fb->width / 3, lh + (7 * sc), MA_FOODLOG_NEXT, 0);
      }
   }
}

void render_foodtype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = 2 * sc;
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "FOOD", 0xFFFFFFFF);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", 0xFFFFFFFF);
   add_hit_ix(h, 0, y - (3 * sc), fb->width, 2 * lh, MA_FOODTYPE_BACK, 0);
   y += 3 * lh;

   /* NEW FOOD IS FIRST, ALWAYS, and it is the one row that is always here.
    *
    * On a fresh install the vocabulary is empty, and a screen whose only
    * content is "None yet" is a dead end: the user came here to log a meal
    * and the app has nothing to say but no. Putting the way OUT of that state
    * at the top means the empty screen is still a working screen.
    *
    * It stays at the top rather than after the list, so its position does not
    * move as the vocabulary grows -- the row you reach for to add a food is in
    * the same place on the tenth use as on the first. */
   menu_button(fb, h, x, y, rx - x, sc, "+ NEW FOOD", 0xFFCCCCCC,
               MA_FOODTYPE_NEW, 0);
   /* A BLANK LINE BELOW IT, because this row is not one of the list. It ADDS
    * to the vocabulary; everything under it PICKS from it. At the same pitch
    * as the entries it read as the first item, which is the one row it must
    * not be mistaken for -- tapping it opens a keypad rather than choosing a
    * food. The gap is what says "different kind of thing", the same way every
    * other menu here separates its sections. */
   y += 3 * lh;

   int n = m->food.ntypes;
   if (n <= 0) {
      draw_str(px, fb, x, y, sc, "No foods yet. Add one", 0xFF888888);
      y += lh;
      draw_str(px, fb, x, y, sc, "and it stays on this list.", 0xFF888888);
      return;
   }

   /* Rows that fit between the NEW FOOD button and a reserved bottom nav
    * line, capped by the HIT BUDGET as well as by height -- add_hit drops
    * everything past UI_MAX_HITS, and a dropped target draws perfectly while
    * being dead to touch. render_olddev carries the full argument; the
    * failure was reproduced there at 480x1920 and 540x2340. */
   int avail = fb->height - y - (2 * lh);
   int per   = avail / (2 * lh);
   if (per > UI_MAX_HITS - UI_LOG_FIXED)
      per = UI_MAX_HITS - UI_LOG_FIXED;
   if (per < 1)
      per = 1;
   int npages = (n + per - 1) / per;
   int page   = m->food.type_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int start = page * per;

   for (int r = start; r < start + per && r < n; r++) {
      /* THE ROW CARRIES ITS INDEX INTO THE VOCABULARY, and the dispatcher
       * turns that into an ID before anything stores it. The index is a fact
       * about this frame's snapshot -- the table can grow between frames --
       * so it must not outlive the tap that used it. */
      const struct food_type *ft = &m->food.types[r];
      /* The one already chosen reads back in white, the rest in the row
       * colour: coming here from the form's TYPE row, "which is currently
       * selected" is the question the screen has to answer at a glance. */
      int chosen      = (ft->id == m->food.food_type);
      uint32_t col    = chosen ? 0xFFFFFFFF : 0xFFAAAAAA;
      const char *val = chosen ? "*" : "";
      char name[3 + FOOD_NAME_MAX + 1];
      (void)snprintf(name, sizeof name, "  %s", ft->name);
      menu_row(fb, h, y, sc, lh, name, val, col, MA_FOODTYPE_PICK, r);
      y += 2 * lh;
   }

   if (npages > 1) {
      int navy = fb->height - lh - (4 * sc);
      if (page > 0) {
         draw_str(px, fb, x, navy, tsc, "<", 0xFFFFFFFF);
         add_hit_ix(h, 0, navy - (3 * sc), fb->width / 3, lh + (7 * sc),
                    MA_FOODPAGE_PREV, 0);
      }
      char pg[24];
      (void)snprintf(pg, sizeof pg, "%d/%d", page + 1, npages);
      draw_str(px, fb, (fb->width - (str_len(pg) * 6 * sc)) / 2, navy, sc, pg,
               0xFF888888);
      if (page < npages - 1) {
         draw_str(px, fb, rx - (1 * 6 * tsc), navy, tsc, ">", 0xFFFFFFFF);
         add_hit_ix(h, (2 * fb->width) / 3, navy - (3 * sc), fb->width / 3,
                    lh + (7 * sc), MA_FOODPAGE_NEXT, 0);
      }
   }
}
