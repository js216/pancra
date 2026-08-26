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
   int tsc      = FONT_TITLE(sc);
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
   int tsc      = FONT_TITLE(sc);
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
   draw_str(px, fb, x, y, sc, "THIS CANNOT BE UNDONE.", UI_MUTED);
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
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "EXERCISE LOG", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_EXLOG_BACK, 0);
   y += 2 * lh;

   /* THE STEP COUNT'S ONE LINE, directly under the title.
    *
    * The steps are drawn on this screen's plot, so this is where the feature
    * has to account for itself: the day's total when it is running, and when
    * it is not, why the second curve is missing and how to get it. The
    * sentence is the switch -- naming a screen the user then has to go and
    * find is a direction rather than an answer, and the words promise the tap
    * turns counting on, so anything less makes the line a liar.
    *
    * TODAY IS THE LOCAL CIVIL DAY, floored the way the plot's own day buckets
    * are, so this figure and the last bar of a day-bucketed tab are the same
    * arithmetic over the same rows. */
   if (!m->food.steps_on) {
      draw_str(px, fb, x, y, sc, "STEP COUNT OFF, PRESS HERE TO ENABLE",
               UI_MUTED);
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh + (3 * sc)),
                 MA_STEPS_TOGGLE, 0);
   } else {
      const long day = ex_day_floor(m->now, m->tz_off);
      long tot       = 0;
      for (int i = 0; i < m->food.nsteps; i++) {
         const long t = m->food.steps[i].t;
         if (t >= day && t < day + 86400)
            tot += m->food.steps[i].n;
      }
      char line[32];
      (void)snprintf(line, sizeof line, "TODAY %ld STEPS", tot);
      draw_str(px, fb, x, y, sc, line, UI_MUTED);
   }
   y += 2 * lh;

   if (m->food.nexlog <= 0) {
      draw_str(px, fb, x, y, sc, "NOTHING LOGGED YET.", UI_MUTED);
      return;
   }
   /* THREE COLUMNS, and the header spaces them to where the rows put them:
    * the instant is 16 characters, the level word and its number take 11 with
    * the padding below, and the length follows. */
   /* THE SCREEN IS SPLIT: table above, minutes-per-day below, laid out from
    * the bottom exactly as the weight log's is -- the plot takes a fixed
    * share and the table gets what is left, so neither can crowd the other
    * out on a tall or a short screen. The system gesture bar is reserved for
    * the same reason it is there: this screen reaches the bottom edge. */
   int sysbar   = fb->height / 24;
   int plot_h   = (fb->height * 2) / 5;
   int tabs_h   = 2 * lh;
   int plot_top = fb->height - plot_h - sysbar;
   int tabs_y   = plot_top - tabs_h;
   int nav_y    = tabs_y - (2 * lh) - (6 * sc);

   /* THREE COLUMNS, and the header spaces them to where the rows put them:
    * the instant is 16 characters, the level word and its number take 11 with
    * the padding below, and the length follows. */
   draw_str(px, fb, x, y, sc, "TIME              LEVEL      MIN", UI_MUTED);
   y += lh;

   int avail = nav_y - y;
   int per   = (avail > 0) ? avail / lh : 1;
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
      char lvl[16];
      char durp[12];
      fmt_date(e->t, m->tz_off, when, sizeof when);
      (void)snprintf(lvl, sizeof lvl, "%s %d", ex_level_word(e->level),
                     e->level);
      /* HOW LONG IT LASTED, and the running session is not a number.
       *
       * Its length is `now` minus a start that is still moving, so printing
       * one would be a figure that is wrong a second later and that no edit
       * can correct -- the same reason the edit form refuses the field. It
       * says ACTIVE instead, in the colour the rest of the app uses for
       * something in progress.
       *
       * "--" IS NOT THE SAME ANSWER. A row that is open and is NOT the
       * running one never got an end recorded (struct ex_rec) -- the app was
       * killed, or the log predates the column -- and its length is simply
       * unknown. Showing 0 would claim a session that took no time. */
      const int running = (ti == m->food.exlog_act);
      long durm         = e->dur / 60;
      if (durm > EX_DUR_MAX / 60)
         durm = EX_DUR_MAX / 60;
      if (running)
         (void)snprintf(durp, sizeof durp, "ACTIVE");
      else if (durm > 0)
         (void)snprintf(durp, sizeof durp, "%ld", durm);
      else
         (void)snprintf(durp, sizeof durp, "--");
      (void)snprintf(row, sizeof row, "%s  %-11s", when, lvl);
      /* THE WHOLE ROW IN ITS LEVEL'S COLOUR. A log of exercise is read for
       * its shape -- when the hard days were -- and the colour is what makes
       * that visible in a column of near-identical timestamps. */
      draw_str(px, fb, x, y, sc, row, ui_ex_color(e->level, UI_TEXT_DIM));
      /* THE LENGTH IS DRAWN SEPARATELY so ACTIVE can carry its own colour --
       * the running session is the one row here whose state, not just its
       * value, is worth seeing from across the table. Every other row keeps
       * the level colour the rest of its line has. Placed at the column the
       * header names: 16 for the instant, 2 of gap, 11 for the padded level.
       */
      draw_str(px, fb, x + (29 * 6 * sc), y, sc, durp,
               running ? UI_BUSY : ui_ex_color(e->level, UI_TEXT_DIM));
      /* THE WHOLE ROW is the target, carrying the TAIL INDEX -- which the
       * dispatcher immediately turns into a copy of the row itself, because
       * an index is only good for as long as the tail is. */
      add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_EXLOG_EDIT, ti);
      y += lh;
   }

   pager_row(fb, h, x, rx, nav_y, sc, lh, page, npages, MA_EXLOG_PAGE);

   /* THE TWO SERIES AND, ON THE 24 H TAB, THE EXERCISE BAND -- see ex_points,
    * which is the only definition of either and is shared with the scrub
    * picker so the two cannot resolve against different points. */
   int tab = m->food.exlog_tab;
   if (tab < 0 || tab >= UI_EXDAY_TABS)
      tab = 0;
   struct log_pt pts[UI_LOG_PTS];
   unsigned char band[UI_LOG_PTS];
   const int by_min = ui_exday_hours[tab] > 0 && ui_exday_hours[tab] <= 24;
   long from        = 0;
   int npt = ex_points(m, pts, UI_LOG_PTS, &from, by_min ? band : NULL,
                       UI_LOG_PTS);

   /* Span tabs -- OR the scrub readout, the same swap the weight trend and
    * the glucose plot both make: while a finger is down the tab row becomes
    * the value under it, and the tabs come back when it lifts. */
   int colw  = (fb->width - (2 * x)) / UI_EXDAY_TABS;
   int trow  = 14 * sc;
   int laby  = plot_top - trow + ((trow - (7 * sc)) / 2);
   int scrub = m->log_scrub;
   if (scrub >= 0 && scrub < npt) {
      char when[24];
      char val[24];
      char rhs[24];
      fmt_date(pts[scrub].t, m->tz_off, when, sizeof when);
      char *lhs = when;
      if (by_min) {
         /* A FIVE-MINUTE BUCKET IS AN INSTANT, so the clock is what names it;
          * fmt_date lays out "YYYY-MM-DD HH:MM" and the time is the tail. */
         if (str_len(when) > 11)
            lhs = when + 11;
         /* FOUR, not five: a five-minute window holds a few hundred steps,
          * and padding out to the ten-thousand guard would spend two columns
          * of the row on space no bucket ever fills. */
         (void)snprintf(val, sizeof val, "%4ld %s", pts[scrub].v,
                        ui_steps_word(pts[scrub].v));
         /* THE BAND UNDER THE FINGER takes the right-hand zone. The strip is
          * drawn per bucket and `band` is indexed the same way the points
          * are, so the level in force at the scrubbed instant is simply
          * band[scrub] -- which is what makes the bands readable rather than
          * merely visible, the way the main plot's are. THE WORD ALONE, not
          * the word and its number: the two say the same thing. */
         /* A CONSTANT WIDTH, PADDED IN FRONT. The middle field is centred in
          * what the outer two leave, so a right-hand field that changes width
          * moves the number -- and this one vanishes entirely the moment the
          * finger leaves a band, which would jog the step count sideways
          * exactly where the reader is comparing it against the band. Leading
          * spaces keep the word flush with the row's right edge while the
          * field it sits in never changes size. */
         (void)snprintf(rhs, sizeof rhs, "%8s",
                        band[scrub] > 0 ? ex_level_word(band[scrub]) : "");
      } else {
         /* THE DATE ALONE, AND WITHOUT ITS YEAR: a point is a whole day, not
          * an instant, so the midnight it carries is not a time worth
          * printing -- and no span this plot offers is long enough for the
          * year to be the thing in doubt. */
         if (str_len(when) > 5)
            lhs = when + 5;
         if (str_len(lhs) > 5)
            lhs[5] = 0; /* "MM-DD" */
         /* BOTH SERIES FOR THE PICKED DAY, whichever curve the finger landed
          * on. They are the same day and the plot exists to be read across,
          * so answering with only the trace that was nearest would make the
          * reader scrub twice for one question. The day's index within its
          * series is what pairs them: series 0 fills the first half of the
          * array and series 1 the second, so the partner sits exactly npt/2
          * away.
          *
          * TWO READINGS, NOT A READING AND A UNIT -- so this is the tab where
          * the right-hand zone carries the second series instead. Exercise is
          * what the screen is about and takes the middle; the steps beside it
          * are the comparison it is there to invite. */
         const int half   = npt / 2;
         const int di     = (scrub >= half) ? scrub - half : scrub;
         const long mins  = (di < half) ? pts[di].v : 0;
         const long steps = (half + di < npt) ? pts[half + di].v : 0;
         /* Both numbers right-aligned in fixed fields, for the reason the
          * main plot's readout sets out: a digit gained or lost must change
          * the digits and not the position of everything around them. */
         /* BOTH FIELDS PADDED TO A CONSTANT WIDTH. The right-hand one is
          * anchored by its end, so its own digits keep their columns either
          * way -- but the middle field is centred in what the outer two
          * leave, so anything that changes THEIR width moves it. A day's step
          * count crossing from three digits to four would otherwise nudge the
          * minutes along with it. */
         (void)snprintf(val, sizeof val, "%3ld MIN", mins);
         (void)snprintf(rhs, sizeof rhs, "%5ld %s", steps,
                        ui_steps_word(steps));
      }
      log_scrub_row(px, fb, x, plot_top - trow, fb->width - (2 * x), sc, 2 * sc,
                    lhs, val, rhs);
      /* No tab targets while scrubbing: the row is not showing tabs, and a
       * target that does not match what is drawn is how a drag ends up
       * changing the span it was only trying to read. */
   } else {
      for (int i = 0; i < UI_EXDAY_TABS; i++) {
         int lw   = str_len(ui_exday_tab_lbl[i]) * 6 * sc;
         int tabx = x + (i * colw);
         draw_str(px, fb, tabx + ((colw - lw) / 2), laby, sc,
                  ui_exday_tab_lbl[i], i == tab ? UI_TEXT : UI_MUTED);
         add_hit_ix(h, ui_rect(tabx, tabs_y, colw, tabs_h), MA_EXTAB, i);
      }
   }

   int pw = fb->width - (2 * x);
   exlog_plot(px, fb, pts, npt, from, m->now, x, plot_top, pw, plot_h, sc,
              m->tz_off, scrub, by_min ? band : NULL,
              ex_bucket_for(ui_exday_hours[tab]));
   /* arg carries sc: the shell needs the SAME scale the plot was drawn at to
    * map a finger x back to an entry, and re-deriving it there would be a
    * second copy of the layout that can drift. */
   add_hit(h, ui_rect(x, plot_top, pw, plot_h), ACT_SCRUB, sc);
}

/* ================= STEP COUNT ==================================
 *
 * One control and one plot. The control is the whole feature's switch --
 * sampling is off until it is on, and the permission is requested at the
 * moment it is switched on rather than at launch, because a glucose app
 * asking for activity data on first run has no visible reason to.
 */

