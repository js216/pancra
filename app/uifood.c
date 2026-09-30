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
 * state is the mistake that has recurred about six times in this codebase.
 * The
 * origin is written down on the way in -- see nav.h -- and read on the way
 * out. */
#include "colors.h"
#include "font.h" /* str_len: the page counter is centred on its width */
#include "food.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h" /* fmt_date: the instant, split into YEAR / MM-DD / HH:MM */
#include "uimodel.h"
#include "uipriv.h"
#include "util.h" /* str_snapshot */

#include "ndk.h"
#include <stdint.h>
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
   int sc       = ui_fit_scale(fb->width, fb->height, 27);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, m->food.food_edit ? "EDIT FOOD" : "LOG FOOD",
            UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* X discards -- nothing is written before an explicit CONFIRM. The band is
    * 2*lh - 2*sc rather than 2*lh because value_row's target starts at its
    * y - 4*sc, and a full band reaches into the row below it. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, (2 * lh) - (2 * sc)),
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
   uint32_t tcol     = m->food.food_type != FOOD_TYPE_NONE ? UI_TEXT : UI_MUTED;
   /* NINE VALUE ROWS: the entry's five, then what the food is made of --
    * per gram of it, and the food's, not this entry's; CONFIRM writes them
    * to the food (food.h). Calories lead them, in the FOOD LOG's order.
    *
    * THE GAP BETWEEN ROWS SPENDS THE HEIGHT THE SCREEN HAS. It is what is
    * left above the system gesture bar once the rows and EDIT FOOD's three
    * buttons are placed, shared out over the nine gaps: never more than a
    * line, the other forms' spacing, and never less than half a line, which
    * is what the 27 rows asked of ui_fit_scale pay for. Sized for EDIT FOOD
    * in both modes, so LOG FOOD and EDIT FOOD lay their rows out alike. */
   const int vrow  = (7 * tsc) + (8 * sc);
   const int btn   = 25 * sc;
   const int fixed = (9 * vrow) + (3 * btn) + (2 * ((3 * lh) / 2));
   int gap         = (fb->height - (fb->height / 24) - y - fixed) / 9;
   if (gap > lh)
      gap = lh;
   if (gap < lh / 2)
      gap = lh / 2;
   y = value_row(fb, h, y, sc, "TYPE", tname, tcol, MA_FOOD_EDIT, 0);
   y += gap;
   char gval[16];
   (void)snprintf(gval, sizeof gval, "%d G", m->food.food_g);
   y = value_row(fb, h, y, sc, "GRAMS", gval, UI_TEXT, MA_FOOD_EDIT, 1);
   y += gap;
   y = value_row(fb, h, y, sc, "TIME", timep, UI_TEXT, MA_FOOD_EDIT, 2);
   y += gap;
   y = value_row(fb, h, y, sc, "DATE", datep, UI_TEXT, MA_FOOD_EDIT, 3);
   y += gap;
   y = value_row(fb, h, y, sc, "YEAR", yearp, UI_TEXT, MA_FOOD_EDIT, 4);
   y += gap;
   {
      static const int order[FOOD_NMACRO]        = {FOOD_KCAL, FOOD_CARBS,
                                                    FOOD_PROTEIN, FOOD_FAT};
      static const char *const mlbl[FOOD_NMACRO] = {"CARBS", "PROTEIN", "FAT",
                                                    "KCAL/G"};
      for (int r = 0; r < FOOD_NMACRO; r++) {
         const int k = order[r];
         char v[16];
         (void)food_milli_str(m->food.food_macro[k], v, (int)sizeof v);
         y = value_row(fb, h, y, sc, mlbl[k], v, UI_TEXT, MA_FOOD_EDIT, 5 + k);
         y += gap;
      }
   }

   /* Cancel on TOP, the committing button on the BOTTOM -- the app-wide rule
    * the insulin and weight forms both follow. */
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_FOOD_DISCARD, 0);
   y += (3 * lh) / 2;
   /* Editing adds DELETE (red) between CANCEL and CONFIRM, mirroring EDIT
    * WEIGHT and EDIT INSULIN. It only opens a confirmation; it never deletes
    * on the tap itself. */
   if (m->food.food_edit) {
      y = menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_FOOD_DELETE,
                      0);
      y += (3 * lh) / 2;
   }
   (void)menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_OK, MA_FOOD_CONFIRM, 0);
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
 * EVERY ROW OPENS ITS ENTRY, now that food_update exists to rewrite one by
 * content. Until it did, the rows were deliberately inert: a row that looks
 * tappable and silently appends a duplicate rather than amending the record is
 * worse than a row that does nothing. */
/* Confirm deleting one food entry. Mirrors the weight and insulin ones: the
 * record being destroyed is spelled out, CANCEL is first and DELETE below it.
 *
 * THE ORIGINAL ENTRY, not the form's current values. Editing the portion and
 * then tapping DELETE would otherwise show the edited number while
 * food_delete removes the row that is on disk -- a confirmation naming a
 * different record than the one it destroys is worse than none. */
void render_fooddel(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 20);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DELETE?", UI_DANGER);
   y += 3 * lh;
   char fv[40];
   char when[20];
   (void)snprintf(fv, sizeof fv, "%ld G %s", m->food.food_orig_g,
                  food_type_name(m->food.food_orig_type));
   fmt_date(m->food.food_orig_t, m->tz_off, when, sizeof when);
   draw_str(px, fb, x, y, sc, fv, UI_TEXT);
   y += lh;
   draw_str(px, fb, x, y, sc, when, UI_TEXT_DIM);
   y += 2 * lh;
   draw_str(px, fb, x, y, sc, "THIS CANNOT BE UNDONE.", UI_MUTED);
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_FOODDEL_NO, 0);
   y += (3 * lh) / 2;
   (void)menu_button(fb, h, x, y, bw, sc, "DELETE", UI_DANGER, MA_FOODDEL_YES,
                     0);
}

/* TODAY RUNS FROM 03:00 LOCAL, not from midnight: a late supper belongs to
 * the day it ended, not to the one that starts while it is being digested.
 * Each total is the entries' grams times what their food is made of, now --
 * the numbers are the food's (food.h), so correcting a food corrects every
 * day it was eaten on. */
void ui_food_today(const struct screen *m, long *tot)
{
   long loc  = m->now + m->tz_off;
   long from = loc - (((loc % 86400) + 86400) % 86400) + (3L * 3600);
   if (loc < from)
      from -= 86400;
   from -= m->tz_off;
   for (int k = 0; k < FOOD_NMACRO; k++)
      tot[k] = 0;
   for (int i = 0; i < m->food.nlog; i++) {
      const struct food_rec *e = &m->food.log[i];
      if (e->t < from || e->t >= from + 86400)
         continue;
      for (int j = 0; j < m->food.ntypes; j++)
         if (m->food.types[j].id == e->type)
            for (int k = 0; k < FOOD_NMACRO; k++)
               tot[k] += e->g * (long)m->food.types[j].macro[k];
   }
}

/* THE BAR IS THE FOOD LOG'S CALORIE BAR, SHRUNK: the same track, the same
 * green filling towards the goal, the same orange once past it -- inset from
 * the frame and along the button's bottom edge, 2*sc tall, placed exactly
 * as the EXERCISE button's settling bar is. INSIDE the button's rectangle:
 * the caller's row pitch is fixed, and a bar below it would land on the next
 * control. No goal (0) is no bar. */
int ui_foodlog_button(struct ANativeWindow_Buffer *fb, struct hits *h, int x,
                      int y, int w, int sc, const struct screen *m,
                      const char *name, uint32_t col)
{
   uint32_t *px = fb->bits;
   const int below =
       menu_button(fb, h, x, y, w, sc, name, col, MA_FOODLOG_OPEN, 0);
   const int goal = m->food.goal[FOOD_KCAL];
   if (goal <= 0)
      return below;
   long tot[FOOD_NMACRO];
   ui_food_today(m, tot);
   const long v = (tot[FOOD_KCAL] + 500) / 1000;
   const int bx = x + (3 * sc);
   const int bw = w - (6 * sc);
   const int bh = 2 * sc;
   const int by = below - bh - (2 * sc);
   long fill    = (v * (long)bw) / goal;
   if (fill > bw)
      fill = bw;
   fill_rect(px, fb, bx, by, bw, bh, UI_FOOD_TRACK);
   if (fill > 0)
      fill_rect(px, fb, bx, by, (int)fill, bh,
                v > goal ? UI_FOOD_OVER : UI_FOOD_FILL);
   return below;
}

/* TODAY'S TOTALS against the day's goals, the upper pane of the FOOD LOG.
 *
 * Today is ui_food_today's.
 *
 * One line per total -- label, today's value, a bar, the goal at the right --
 * spread evenly down the pane. THE GOAL IS THE TARGET: a tap on it opens the
 * keypad for that goal. */
static void food_day_pane(struct ANativeWindow_Buffer *fb,
                          const struct screen *m, struct hits *h, int top,
                          int height, int sc)
{
   uint32_t *px = fb->bits;
   const int x  = 4 * sc;
   const int rx = fb->width - (4 * sc);
   const int cw = 6 * sc;
   long tot[FOOD_NMACRO];
   ui_food_today(m, tot);
   static const int order[FOOD_NMACRO] = {FOOD_KCAL, FOOD_CARBS, FOOD_PROTEIN,
                                          FOOD_FAT};
   static const char *const lbl[FOOD_NMACRO] = {"CARBS", "PROTEIN", "FAT",
                                                "KCAL"};
   const int pitch                           = height / FOOD_NMACRO;
   /* columns, in cells: the label's 7, the value's 4, the goal's 4 at the
    * margin, a cell of air between each, and the bar in what is left */
   const int vx1 = x + (12 * cw) - sc; /* the value's right edge */
   const int bx0 = x + (13 * cw);
   const int bx1 = rx - (5 * cw);
   for (int r = 0; r < FOOD_NMACRO; r++) {
      const int k  = order[r];
      const int ly = top + (r * pitch) + ((pitch - (7 * sc)) / 2);
      long v       = (tot[k] + 500) / 1000;
      if (v > 9999)
         v = 9999;
      const int goal = m->food.goal[k];
      char vs[8];
      char gs[8];
      (void)snprintf(vs, sizeof vs, "%ld", v);
      (void)snprintf(gs, sizeof gs, "%d", goal);
      draw_str(px, fb, x, ly, sc, lbl[k], UI_TEXT_DIM);
      draw_str(px, fb, vx1 - (((str_len(vs) * 6) - 1) * sc), ly, sc, vs,
               UI_TEXT);
      if (bx1 > bx0) {
         const int bh = 5 * sc;
         const int by = ly + sc;
         fill_rect(px, fb, bx0, by, bx1 - bx0, bh, UI_FOOD_TRACK);
         long fill = goal > 0 ? (v * (long)(bx1 - bx0)) / goal : 0;
         if (fill > bx1 - bx0)
            fill = bx1 - bx0;
         if (fill > 0)
            fill_rect(px, fb, bx0, by, (int)fill, bh,
                      v > goal ? UI_FOOD_OVER : UI_FOOD_FILL);
      }
      draw_str(px, fb, rx - (((str_len(gs) * 6) - 1) * sc), ly, sc, gs,
               UI_TEXT);
      add_hit_ix(h, ui_rect(bx1, top + (r * pitch), fb->width - bx1, pitch),
                 MA_FOODGOAL, k);
   }
}

void render_foodlog(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "FOOD LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_FOODLOG_BACK,
              0);
   y += 3 * lh;

   /* TODAY'S TOTALS ON TOP, THE TABLE BELOW THEM: see log_split_of. The
    * pane takes three tenths of what is below the column header's line, as
    * the INSULIN LOG's plot does, and the table the rest. */
   const int pane_h = ((fb->height - (y + lh) - (fb->height / 24)) * 3) / 10;
   struct log_split sp;
   log_split_of(fb->height, y, sc, 0, pane_h, &sp);
   const int nav_y = sp.nav_y;
   food_day_pane(fb, m, h, sp.plot_top, pane_h, sc);
   y = sp.hdr_y;

   if (m->food.nlog <= 0) {
      draw_str(px, fb, x, y, sc, "NOTHING LOGGED YET.", UI_MUTED);
      return;
   }
   /* THE HEADER SITS OVER THE COLUMNS THE ROWS BELOW DRAW: 16 for the
    * instant, 2 of gap, 11 for the padded name -- and the grams right-aligned
    * at the margin, which is where its own column ends. */
   draw_str(px, fb, x, y, sc, "TIME              FOOD", UI_MUTED);
   draw_str(px, fb, rx - (str_len("G") * 6 * sc), y, sc, "G", UI_MUTED);
   y += lh;

   /* Rows between the header and the pager, and no more than the hit budget
    * leaves once the pane's four goal targets are counted (see
    * render_inslog: a target past UI_MAX_HITS is dropped, silently). */
   int avail = nav_y - y;
   int per   = avail / lh;
   if (per > UI_MAX_HITS - UI_LOG_FIXED - FOOD_NMACRO)
      per = UI_MAX_HITS - UI_LOG_FIXED - FOOD_NMACRO;
   if (per < 1)
      per = 1;
   int npages = (m->food.nlog + per - 1) / per;
   int page   = m->food.log_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   /* THE NAME GETS WHAT THE ROW LEAVES, and is what gives way. fmt_date is
    * 16 characters, then 2 of gap, and the grams take the last 4 at the
    * margin; the name has the cells between, up to FOOD_NAME_MAX (20).
    * ui_fit_scale guarantees UI_COLS (33) cells, so the name always has at
    * least 11. draw_str clips silently past the edge, so a name padded past
    * its room would push the quantity -- the one number a food log is read
    * for -- off the screen without a mark; cut with an explicit precision,
    * the name loses its tail where the user can see it has one. Worked out
    * once per screen, so the column is the same on every row.
    *
    * THE GRAMS ARE DRAWN SEPARATELY, right-aligned at the margin, so their
    * column cannot be pushed anywhere by the name beside them. */
   int namew = ((rx - x) / (6 * sc)) - 16 - 2 - 4;
   if (namew > FOOD_NAME_MAX)
      namew = FOOD_NAME_MAX;
   if (namew < 0)
      namew = 0;
   for (int r = page * per; r < (page + 1) * per && r < m->food.nlog; r++) {
      /* the tail is oldest-first; the table shows newest first */
      int ti                   = m->food.nlog - 1 - r;
      const struct food_rec *e = &m->food.log[ti];
      char when[20];
      char row[56];
      char gp[8];
      fmt_date(e->t, m->tz_off, when, sizeof when);
      /* THE NAME, NOT THE ID. An entry whose type is missing renders as an
       * empty column rather than a number nobody can read -- food_type_name
       * answers "" for an id no type has, which is the honest look of a
       * record the vocabulary can no longer name. */
      (void)snprintf(row, sizeof row, "%s  %-*.*s", when, namew, namew,
                     food_type_name(e->type));
      draw_str(px, fb, x, y, sc, row, UI_TEXT_DIM);
      (void)snprintf(gp, sizeof gp, "%4ld", e->g);
      draw_str(px, fb, rx - (str_len(gp) * 6 * sc), y, sc, gp, UI_TEXT_DIM);
      /* THE WHOLE ROW is the target, carrying the TAIL INDEX -- which the
       * dispatcher immediately turns into a copy of the row itself, because
       * an index is only good for as long as the tail is. */
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_FOODLOG_EDIT,
                 ti);
      y += lh;
   }

   pager_row(fb, h, x, rx, nav_y, sc, lh, page, npages, MA_FOODLOG_PAGE);
}

void render_foodtype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "FOOD", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_FOODTYPE_BACK,
              0);
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
   menu_button(fb, h, x, y, rx - x, sc, "+ NEW FOOD", UI_TEXT_DIM,
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
      draw_str(px, fb, x, y, sc, "NO FOODS YET. ADD ONE", UI_MUTED);
      y += lh;
      draw_str(px, fb, x, y, sc, "AND IT STAYS ON THIS LIST.", UI_MUTED);
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
      uint32_t col    = chosen ? UI_TEXT : UI_FAINT;
      const char *val = chosen ? "*" : "";
      char name[3 + FOOD_NAME_MAX + 1];
      (void)snprintf(name, sizeof name, "  %s", ft->name);
      menu_row(fb, h, y, sc, lh, name, val, col, MA_FOODTYPE_PICK, r);
      y += 2 * lh;
   }

   pager_row(fb, h, x, rx, fb->height - lh - (4 * sc), sc, lh, page, npages,
             MA_FOODPAGE);
}
