// SPDX-License-Identifier: GPL-3.0
// uidev.c --- Devices, and the screens reached from one (see uipriv.h)
// Copyright 2026 Jakob Kastelic

#include "colors.h"
#include "font.h"
#include "menuview.h"
#include "ndk.h"
#include "plot.h"
#include "sensors.h" /* sensor types, kinds, marker enum */
#include "style.h"
#include "uiact.h"
#include "uidraw.h"
#include "uifmt.h"
#include "uimenu.h"
#include "uimodel.h"
#include "uipriv.h"
#include <stdint.h>
#include <stdio.h> /* snprintf */

/* THE DEVICES LIST'S GEOMETRY, settled once and handed to the row.
 *
 * render_devices was 299 lines, and ninety of them were one row: the state
 * text, the age, the marker glyph and the primary checkbox, all reading nine
 * local variables that the loop around them had computed. The row is its own
 * function now, and this is the loop's half of what they shared. */
struct dev_geo {
   int sc, lh, gh;               /* text scale, line pitch, glyph height */
   int x, rx;                    /* margins */
   int cbs, cbx, cbh, colx, vrx; /* the primary column: box, target, values */
};

/* ONE DEVICE ROW: its marker, its name, what it is doing, and -- for a CGM
 * when there is more than one -- the primary checkbox. Returns the y the next
 * row starts at. */
static int device_row(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h, const struct ui_sensor *s, int i,
                      const struct dev_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   char val[28]; /* state[4] + ' ' + ago[12], with room to spare */
   char ago[12];
   char abbuf[8]; /* dev_state_abbrev's fallback scratch */
   /* For a meter the age is its last SYNC, never its last fingerstick -- so
    * "SYNCED 2 M" means synced 2 min ago, not a datapoint 2 min old. The
    * sync time is persisted, so it survives a restart; if a meter has
    * genuinely never synced it reads NEVER rather than mislabelling a
    * datapoint age. */
   long agot = (s->have_rec && s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
   fmt_ago(m->now, agot, ago, sizeof ago);
   /* The countdown needs the sensor's OWN clock, so it is drawn only when one
    * has arrived; a sensor warming up without one still reads WARM through the
    * status string below, which sensor_in_warmup also decides. */
   /* AND ONLY WHILE THERE IS SOMETHING LEFT TO COUNT. sensor_warming_now
    * answers on the STATE byte alone, which is right for "is this sensor
    * warming up" and not enough to subtract from: a cached session
    * (sessc_restore) projects the clock forward for up to a day while keeping
    * the state it was stored with, so a sensor that went out of range in
    * warm-up comes back with state 0x02 and a clock past the hour -- and the
    * countdown below would render WARM -10:00. Past the hour the row falls
    * through to the ordinary state line, which still reads WARM. */
   if (s->have_rec && s->kind == KIND_CGM &&
       sensor_warming_now(s->sess_state, s->session_seconds) &&
       s->session_seconds > 0 && s->session_seconds < SENSOR_WARMUP_S) {
      /* Warmup: time REMAINING, not "NEVER" -- the wait is by design and
       * the countdown says when data starts. With the sensor's own clock
       * it is exact to the second (matching the official reader); off the
       * pairing instant it is an estimate, and the '~' says so. */
      /* WARM, like every other state here, then the countdown in the
       * place the age occupies on the other rows -- so the column still
       * lines up while warming up. */
      long r = SENSOR_WARMUP_S - s->session_seconds;
      (void)snprintf(val, sizeof val, "WARM %d:%02d", (int)(r / 60),
                     (int)(r % 60));
   } else {
      (void)snprintf(val, sizeof val, "%s %s",
                     dev_state_abbrev(s->status, abbuf, sizeof abbuf), ago);
   }
   /* Three leading spaces: the second cell holds this device's plot marker
    * (its shape, colour and size), so the list answers "which trace is
    * which" at a glance. No '>' for the primary any more -- the PRIMARY
    * column at the right says that now, and two marks for one fact on one
    * row is noise. */
   /* Holds the three-space prefix plus a full label (sizeof s->label, which
    * grew to 20 for the long OneTouch default names) plus the terminator.
    * Undersizing it truncated the MAC tail that tells two meters apart. */
   char name[4 + sizeof s->label];
   (void)snprintf(name, sizeof name, "   %s", s->label);
   menu_row_at(fb, h, y, g->sc, g->lh, g->vrx, name, val,
               s->connected ? UI_OK : UI_FAINT, MA_SENSOR, i);
   /* The PRIMARY checkbox, for a CGM whose session is not over. Recorded
    * AFTER the row's own target and inside it: ui_hit_idx scans backwards,
    * so the box wins its own rectangle while the rest of the row still
    * opens the device. A meter gets no box: it cannot own the big number and
    * sensor_set_primary refuses it. An EXPIRED sensor gets none either --
    * this screen's own choice, since a finished session should not be handed
    * the headline value -- though sensor_set_primary itself accepts one, so
    * the per-device PRIMARY row can still set it. What set_primary refuses is
    * a meter, a retired slot, and a slot whose provenance did not load. */
   /* CENTRED ON THE ROW'S GLYPH, and drawn at row height rather than at
    * letter height. At letter height the unticked box is a 5x7 outline in a
    * grey the eye skips past, so the only box anyone sees is the ticked one
    * -- and a radio column where only the current choice is visible offers no
    * choice at all. */
   if (s->have_rec && s->kind == KIND_CGM && !cgm_expired(s)) {
      draw_checkbox(px, fb, g->cbx, y - ((g->cbs - g->gh) / 2), g->cbs, g->sc,
                    s->primary, s->primary ? UI_OK : UI_FAINT);
      /* The target is the box's own rectangle, not a fixed line height:
       * the box is taller than a line now, and a target that stopped short
       * of it would leave its bottom edge dead. It still ends well above
       * the next row's target (pitch 24*g->sc), so no row is stolen. */
      add_hit_ix(h,
                 ui_rect(g->cbh, y - ((g->cbs - g->gh) / 2), fb->width - g->cbh,
                         g->cbs),
                 MA_PRIM_PICK, i);
   }
   if (s->marker != MARK_HIDE) { /* hidden-from-plot draws no glyph */
      /* Centred in the reserved cell (text starts at 4*g->sc; the cell is the
       * second character, 6*g->sc wide). Radius follows the configured SIZE,
       * clamped to the cell so a large marker cannot strike the label. */
      int gr = (2 * g->sc * s->size) / MARK_SIZE_DEF;
      if (gr < g->sc)
         gr = g->sc;
      if (gr > 3 * g->sc)
         gr = 3 * g->sc;
      plot_marker_glyph((struct plot_fb){px, fb->stride, fb->width, fb->height},
                        (4 * g->sc) + (9 * g->sc), y + (3 * g->sc), gr,
                        s->marker, ui_sensor_color(s->color));
   }
   /* PITCH, not g->lh: the half-line of air that separates one device from the
    * next. ui_sensor_capacity divides by this same macro. */
   y += UI_DEV_PITCH(g->sc);
   return y;
}

/* The longest line the wrapper may lay out, and the size of the buffer it
 * copies into. One number, so the two cannot disagree. */
#define EXP_LINE_MAX 63

/* THE PARAGRAPH ABOVE THE LIST, wrapped at runtime to the full text width.
 *
 * Its own function because it is a wrapper, not a layout: forty lines of
 * character arithmetic sitting in the middle of a screen renderer, sharing
 * nothing with it but `y`. Hand-breaking the lines against a guessed column
 * budget runs a quarter too narrow on every geometry -- measuring beats
 * guessing, and it self-corrects rather than being right once.
 */
static int devices_explainer(struct ANativeWindow_Buffer *fb, int x, int rx,
                             int sc, int gh, int y)
{
   uint32_t *px = fb->bits;
   /* FIVE LINES AT MOST, which is what UI_DEV_ABOVE budgets (see ui.h).
    * The narrowest this ever wraps to is the width-bound case, sc =
    * w/(33*6), where cols comes out 31 -- keep it inside five lines there
    * or the ADD NEW DEVICE button pays for the sixth. */
   /* "THE BOX AT THE RIGHT OF A ROW", not "a box". Every row carries TWO
    * squares -- the coloured plot marker at the left and the checkbox at
    * the right -- so "a box" makes the reader pick between them, and the
    * marker is the one their eye lands on first. Naming the side settles
    * it in four words. */
   static const char pexp[] =
       "WITH TWO OR MORE CGMS, TAP THE BOX AT THE RIGHT OF A ROW TO MAKE "
       "IT THE BIG NUMBER. ALARMS WATCH THEM ALL ANYWAY. METERS AND ENDED "
       "SESSIONS HAVE NO BOX.";
   int cols = (rx - x) / (6 * sc);
   if (cols < 8)
      cols = 8; /* degenerate width: draw something rather than loop */
   /* AND NO WIDER THAN THE LINE BUFFER, because the wrap advances by what it
    * TOOK and the copy below can only hold so much. In landscape the height
    * bound sets the scale and this measures 140-198 columns (2400x1080 reaches
    * 198), so every line past the 63rd character would be consumed without
    * being drawn: the paragraph would stop mid-word and the sentence that says
    * the alarms watch every sensor -- the only place that is written down --
    * would never appear. */
   if (cols > EXP_LINE_MAX)
      cols = EXP_LINE_MAX;
   int i = 0;
   while (pexp[i]) {
      /* Take up to `cols` characters, then back off to the last space so a
       * word is never split. A single word longer than the line is cut at
       * the boundary rather than dropped. */
      int take = 0;
      int last = 0;
      while (pexp[i + take] && take < cols) {
         if (pexp[i + take] == ' ')
            last = take;
         take++;
      }
      /* Back off ONLY when the cut would land mid-word. A line that ends
       * exactly where a word does is already whole, and backing off there
       * threw away a word that fitted -- "TO SWITCH." fell to the next
       * line at 38 columns while measuring 38 columns wide. */
      if (pexp[i + take] && pexp[i + take] != ' ' && last > 0)
         take = last;
      char ln[EXP_LINE_MAX + 1];
      int n = take < (int)(sizeof ln) - 1 ? take : (int)(sizeof ln) - 1;
      for (int k = 0; k < n; k++)
         ln[k] = pexp[i + k];
      ln[n] = 0;
      draw_str(px, fb, x, y, sc, ln, UI_MUTED);
      y += gh + (2 * sc);
      i += take;
      while (pexp[i] == ' ')
         i++;
   }
   y += 2 * sc;
   return y;
}

void render_devices(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Bounded by height as well as width -- see ui_devices_scale, which is
    * derived from the SAME expression ui_sensor_capacity uses. */
   int sc  = ui_devices_scale(fb->width, fb->height);
   int tsc = FONT_TITLE(sc);
   int lh  = 16 * sc;
   int gh  = 7 * sc; /* a label glyph is 7 rows tall */
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "DEVICES", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_DEVICES_BACK,
              0);
   y += 3 * lh; /* the DISPLAY menu's title gap -- the house style */

   /* UNPAIRED SENSORS, FIRST AND IN RED.
    *
    * A sensor the OS has no bond with is not a row like the others: it will
    * never deliver another reading, and the remedy is a tap only the user can
    * make. It goes above the list rather than in it, because a red word in a
    * column of ordinary rows is something to scan for, and this is the reason
    * the person came to this screen -- the big number sent them, reading ERR.
    *
    * The row is the whole target and it opens that sensor's own menu, which
    * is where RECONNECT lives: pressing it dials, the sensor asks to bond,
    * and Android raises the pairing dialog while the user is looking at the
    * phone rather than at some random moment they will miss. */
   /* THE ROWS THIS BLOCK SPENDS, counted, because the device list below has
    * to give them back.
    *
    * It is drawn ABOVE the list and is not part of UI_DEV_ABOVE -- it cannot
    * be, since reserving for its worst case would cost every screen those
    * rows on the ordinary days when nothing is unpaired. So the list's page
    * size is reduced by what this actually took, which keeps the ADD NEW
    * DEVICE button and the pager on the screen instead of pushing them off
    * the bottom, where nothing can reach them.
    *
    * AND IT IS BOUNDED, so a phone that loses several bonds at once cannot
    * spend the whole screen on warnings. Nothing is hidden by the cap: every
    * one of these devices is live, so it also has an ordinary row in the list
    * below, with the same MA_SENSOR target. */
   int unp_rows = 0;
   /* WHAT THE BLOCK MAY SPEND, in its own unit and bounded by what the list
    * can give back.
    *
    * The block advances by lh per line; the list advances by UI_DEV_PITCH per
    * row. Subtracting a count of LINES from a count of ROWS under-refunds,
    * and when the difference is clamped away the rest of the screen slides
    * down -- at 1440x2560, where the list is three rows, four unpaired
    * devices put the ADD NEW DEVICE button past the bottom of the buffer with
    * no scrolling to reach it. So the budget is computed in pixels: the block
    * may take what the list can spare while keeping one row. */
   /* THE STALE-VIEW ROW IS CHARGED THE SAME WAY, and not to UI_DEV_ABOVE.
    * It appears only when a publish could not allocate -- essentially never
    * -- and reserving a row for it in the fixed budget would cost every
    * screen a device row on every ordinary day. Counted here, it comes out of
    * the list's share only on the frames it is actually drawn.
    *
    * TAKEN OFF THE UNPAIRED BUDGET FIRST, because the two are spent from the
    * same place: the pixels the list can give back while keeping one row.
    * Budgeted separately they can add up to more than that, and the clamp
    * below then swallows the difference and the screen overflows. */
   int stale_row = sensors_view_stale() ? 1 : 0;
   /* THE READ-ONLY ROW IS A CLAIMANT TOO. Every line drawn above the list
    * comes out of the same pixels, so each one must be taken off the budget
    * before the unpaired block is sized -- budgeted separately they add up to
    * more than the list can refund and the screen overflows. */
   /* ONE ROW FOR EVERYTHING WRONG WITH THE REGISTRY FILES, and its value says
    * WHICH thing.
    *
    * Three separate answers -- slots.csv did not read, sensors.csv did not
    * read, sensors.csv read but does not cover every slot -- sharing one line
    * because four claimants above the list spend more than the list can refund
    * on a three-row screen (1440x2560).
    *
    * AND THE THIRD ONE MUST NOT SAY "RESTART". A file that did not read may
    * read on the next launch; a file that is merely incomplete is
    * self-consistent on disk and will say exactly the same thing for ever, so
    * telling the user to restart sends them somewhere that cannot help. It
    * names what is missing instead. */
   int ro_row = (sensors_writable() && sensors_provenance_loaded() &&
                 sensors_provenance_covers())
                    ? 0
                    : 1;
   /* AND WHAT THE FRAME COULD NOT CARRY. The frame holds the newest
    * UI_LIVE_MAX devices in service; past that the list is a subset, and a
    * subset drawn without a word reads as the whole list. */
   int carried_live = 0;
   for (int i = 0; i < m->dev.nsensors; i++)
      if (!m->dev.sensors[i].old)
         carried_live++;
   int win_row   = m->dev.nlive_all > carried_live ? 1 : 0;
   int extra_row = stale_row + ro_row + win_row;
   int base_cap  = ui_sensor_capacity(fb->width, fb->height);
   int unp_px    = base_cap > 1
                       ? ((base_cap - 1) * UI_DEV_PITCH(sc)) - (extra_row * lh)
                       : 0;
   if (unp_px < 0)
      unp_px = 0;
   int unp_max = unp_px / lh;
   if (unp_max > UI_UNPAIRED_MAX + 3)
      unp_max = UI_UNPAIRED_MAX + 3; /* the cap, its title, "N MORE", blank */
   int unp_tot = ui_unpaired_count(m);
   /* HOW MANY DEVICE ROWS THE BLOCK CAN ACTUALLY LIST, decided before a line
    * of it is drawn.
    *
    * `room` is what is left after the heading and the trailing blank. If the
    * list does not fit, one more line goes to "N MORE BELOW", so listing
    * anything at all then needs room for two. Worked out afterwards instead,
    * the loop bound degenerates: at a three-row list (1440x2560) it admits
    * NOTHING, and the screen draws a red heading and a "1 MORE BELOW" with
    * no device between them, having spent a third of the list to say
    * nothing.
    *
    * COMPACT RATHER THAN NOTHING when even one row will not fit: the warning
    * is why the user came to this screen, so it collapses to a single line
    * that names the count and points at the list, which costs two lines
    * instead of three and still says the true thing. */
   int room = unp_max - 2;
   if (room > UI_UNPAIRED_MAX)
      room = UI_UNPAIRED_MAX; /* the constant IS the cap, not a suggestion */
   int list = unp_tot;
   int more = 0;
   if (list > room) {
      more = 1;
      list = room - 1; /* one line goes to "N MORE BELOW" */
   }
   if (list < 0)
      list = 0;
   int compact = unp_tot > 0 && list < 1 && unp_max >= 2;
   if (compact) {
      char ut[40];
      (void)snprintf(ut, sizeof ut, "UNPAIRED DEVICES (%d) - SEE LIST",
                     unp_tot);
      draw_str(px, fb, x, y, sc, ut, UI_DANGER);
      y += lh;
      y += lh; /* a blank line before the ordinary list */
      unp_rows = 2;
   } else if (unp_tot > 0 && list >= 1 && unp_max >= 3) {
      /* THE COUNT IS IN THE TITLE, so a block that cannot list them all still
       * reports how many there are. A short screen fits one or two rows, and
       * a red heading over a partial list reads as the whole list. */
      char ut[32];
      (void)snprintf(ut, sizeof ut, "UNPAIRED DEVICES (%d)", unp_tot);
      draw_str(px, fb, x, y, sc, ut, UI_DANGER);
      y += lh;
      unp_rows  = 1;
      int drawn = 0;
      for (int i = 0; i < m->dev.nsensors && drawn < list; i++) {
         const struct ui_sensor *u = &m->dev.sensors[i];
         if (!u->have_rec || u->kind != KIND_CGM || u->old ||
             u->bond != UI_BOND_NONE)
            continue;
         draw_str(px, fb, x + (2 * 6 * sc), y, sc, u->label, UI_DANGER);
         draw_str(px, fb, rx - (str_len("RECONNECT") * 6 * sc), y, sc,
                  "RECONNECT", UI_GO);
         /* The INDEX, which is what MA_SENSOR carries everywhere else on this
          * screen -- see the device rows below. */
         add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, lh), MA_SENSOR, i);
         y += lh;
         unp_rows++;
         drawn++;
      }
      /* AND WHAT IS NOT LISTED IS SAID, not left to be inferred from a count
       * in the heading. Every one of them still has an ordinary row in the
       * list below with the same target, so this is under-display and not
       * unreachability -- but the block is the reason the user came to this
       * screen, and a silent partial list is what makes it lie. */
      int shown = unp_rows - 1;
      if (more || shown < unp_tot) {
         char mr[32];
         (void)snprintf(mr, sizeof mr, "  %d MORE BELOW", unp_tot - shown);
         draw_str(px, fb, x, y, sc, mr, UI_DANGER);
         y += lh;
         unp_rows++;
      }
      y += lh; /* a blank line before the ordinary list */
      unp_rows++;
   }

   /* THE PRIMARY COLUMN'S GEOMETRY, settled before anything that has to keep
    * clear of it: the row values below stop short of it.
    *
    * NO HEADER. The column was headed "PRIMARY", then "P", then "PRIM", each
    * time trying to make one word carry a rule that takes a sentence -- and
    * the box is now big enough to read as a control on its own, which is what
    * a header over a single column of checkboxes was really for. The
    * paragraph above says what the boxes do; a word repeating a fragment of
    * it is noise, and it cost the rows a column of width. So the column is
    * exactly the box wide, and nothing else. */
   int cbs  = 15 * sc; /* the box: a real square */
   int colx = rx - cbs;
   int cbx  = colx;
   int cbh  = colx - (2 * sc); /* its tap target starts here */
   /* A FULL CHARACTER CELL of air between the row's value and the box, on top
    * of the 4*sc that only kept them from touching. The value ends in a unit
    * letter ("119 S") and the box is a bright square: at four units apart the
    * two read as one run, and the eye had to separate a countdown from a
    * control. 6*sc is the same cell the rest of this screen spaces by. */
   int vrx = colx - (4 * sc) - (6 * sc); /* right edge left to row values */

   /* WHAT THE BOXES ARE FOR. This paragraph is now the column's ONLY label,
    * so it has to answer all three questions on its own: when the choice
    * matters (only with more than one CGM), what it changes (which reading is
    * the big number), and what it does NOT change (the alarms, which watch
    * every sensor whatever is picked -- without that line, choosing feels
    * like a risk). The last sentence explains the rows with no box, which
    * reads as a bug until you know the rule. */
   /* WRAPPED AT RUNTIME to the FULL text width, margin to margin.
    *
    * Not to the box column: the paragraph sits entirely ABOVE the first row,
    * so nothing is ever drawn beside it and there is nothing to keep clear
    * of. Stopping it at the column cost five characters a line and bought
    * nothing -- it was a habit picked up from the row layout below, where the
    * boxes really are alongside.
    *
    * The lines were hand-broken against a guessed column budget, and the guess
    * was badly low: it assumed sc == w/(33*6) exactly, but that divide FLOORS,
    * so the real sc is smaller and about a quarter more characters fit. The
    * paragraph ran short of the edge on every screen and needed a line it did
    * not need. Measuring beats guessing, and it self-corrects on any geometry
    * rather than being right on one. */
   y = devices_explainer(fb, x, rx, sc, gh, y);

   /* ONE EMPTY ROW where a column header would go. It is the air that
    * separates the paragraph from the first device, and without it the list
    * reads as one more line of prose. Kept as a row rather than shrunk to a gap
    * so UI_DEV_ABOVE still counts exactly what the renderer spends.
    *
    * Below it: a checkbox per eligible CGM at the right edge, with rows
    * keeping their value text clear (vrx) so the two never overprint. Exactly
    * one box is ever solid -- it is a radio choice spelled as checkboxes,
    * because "which one owns the big number" reads better as a column than as
    * a value buried on one row. */
   y += lh;

   /* TOO SHORT IS A VERDICT ABOUT THE SCREEN, so it is asked of the screen's
    * own capacity and not of what this frame happens to have left after the
    * warning block. A geometry that can show a list says so on every frame,
    * whatever is temporarily drawn above it. */
   int base = base_cap;
   if (base < UI_MIN_SLOTS) {
      /* Too short a screen to show even the minimum honestly. Say so rather
       * than silently truncating, which would read as "these are all of them".
       */
      draw_str(px, fb, x, y, sc, "SCREEN TOO SHORT", UI_DANGER);
      y += lh;
      draw_str(px, fb, x, y, sc, "FOR DEVICE LIST", UI_DANGER);
      return;
   }
   /* AND THEN WHAT THE WARNING BLOCK TOOK comes out of the list's share, so
    * the pager and the ADD button keep their rows. Converted: the block spent
    * `unp_rows` lines of lh, and a list row is UI_DEV_PITCH, so the refund is
    * rounded UP -- giving back a fraction of a row would leave the difference
    * on the screen. The bound above guarantees this leaves at least one. */
   int cap = base - ((((unp_rows + extra_row) * lh) + UI_DEV_PITCH(sc) - 1) /
                     UI_DEV_PITCH(sc));
   if (cap < 1)
      cap = 1;
   /* PAGINATE the live list, the same way OLD DEVICES does.
    *
    * Drawing the first `cap` devices and then a red "N MORE NOT SHOWN" row is
    * honest about the truncation and still leaves those devices genuinely
    * UNREACHABLE: there is no scrolling anywhere in this UI, so a device past
    * the cut has no row, no tap target and no way to be opened, renamed,
    * calibrated or forgotten. Naming the problem is not the same as not having
    * it. Collect the live indices first, then show one page. */
   int idxs[UI_MAX_SLOTS];
   int nlive_i = 0;
   /* THE REGISTRY'S COUNT, not a tally of what this frame carries: the frame
    * fills only the page of retired devices the OLD DEVICES list is showing,
    * so counting rows here would report a page instead of the total. */
   int nold = m->dev.nold;
   for (int i = 0; i < m->dev.nsensors && nlive_i < UI_MAX_SLOTS; i++)
      if (!m->dev.sensors[i].old)
         idxs[nlive_i++] = i;
   int npages = (nlive_i + cap - 1) / cap;
   if (npages < 1)
      npages = 1;
   int page = m->dev.dev_page;
   if (page < 0)
      page = 0;
   if (page >= npages)
      page = npages - 1;
   int first = page * cap;
   if (first < 0)
      first = 0; /* unreachable: page is clamped to >= 0 just above */
   for (int pi = first; pi < first + cap && pi < nlive_i; pi++) {
      struct dev_geo g = {sc, lh, gh, x, rx, cbs, cbx, cbh, colx, vrx};
      y = device_row(fb, m, h, &m->dev.sensors[idxs[pi]], idxs[pi], &g, y);
   }
   if (m->dev.pend_type > 0) {
      /* An ARMED pairing: registered intent, no sensor on the air yet. The
       * row is the visible promise that the code was accepted and the app is
       * watching. Tapping it opens the STOP WAITING? confirmation -- the row
       * wears a device's clothes, so a finger reaching for the new sensor
       * must not be able to throw the pairing away on the way past. */
      char pn[24];
      (void)snprintf(pn, sizeof pn, " %s", sensor_type_name(m->dev.pend_type));
      /* WHAT THE WAIT IS WAITING FOR. "PENDING..." alone cannot be acted on:
       * it reads the same whether no sensor has appeared yet or several have
       * and the app will not choose between them. The second case is the one
       * with something to do about it, so it says so. */
      char pv[16];
      if (m->dev.pend_seen >= 2)
         (void)snprintf(pv, sizeof pv, "PICK 1 OF %d", m->dev.pend_seen);
      else
         (void)snprintf(pv, sizeof pv, "PENDING...");
      menu_row(fb, h, y, sc, lh, pn, pv, UI_WARN, MA_PEND_CANCEL, 0);
      y += lh;
   }
   /* PAGE NAV, rather than an "N MORE NOT SHOWN" the user cannot act on. Same
    * shape as OLD DEVICES: "<" and ">" with the page count between them.
    *
    * DRAWN ON EVERY FRAME, including a one-page list, where all four arrows are
    * greyed and the counter reads 1/1. The row's height is part of the line
    * budget above whether it carries anything or not, so hiding it would move
    * every control below it as soon as a second page appeared. */
   pager_row(fb, h, x, rx, y, sc, lh, page, npages, MA_DEVPAGE);
   y += lh;
   /* OLD DEVICES: DISCONNECTED devices. Each keeps its full slot, so the row
    * opens the SAME per-device menu (state EXPIRED). Only shown when there is
    * at least one, so it never adds noise on a fresh install. A blank line
    * either side: it is a door to a different list, not another device, and
    * without the air it read as one more row of the list above it. */
   if (nold > 0) {
      char od[32];
      (void)snprintf(od, sizeof od, "OLD DEVICES (%d)", nold);
      y += lh;
      menu_row(fb, h, y, sc, lh, od, ">", UI_FAINT, MA_OLDDEV_OPEN, 0);
      y += 2 * lh;
   }
   /* WHAT IS LEFT, BEFORE IT RUNS OUT. Two numbers and no more, because two
    * is how many limits can refuse a pairing: a SLOT for every device the app
    * knows (a retired one keeps its slot so its history stays readable), and
    * a LINK for every device it talks to at once (a retired one holds none).
    * OS bonds are not a third: this phone has held ten at once, and the stack
    * prunes its own records.
    *
    * Shown always, not only when nearly full: a limit the user learns about
    * on the day it stops them is the failure this row exists to end. */
   {
      char capv[32];
      (void)snprintf(capv, sizeof capv, "%d/%d  LINKS %d/%d", m->dev.slots_used,
                     m->dev.slots_max, m->dev.links_used, m->dev.links_max);
      /* RED FOR A FILE THAT DID NOT READ, not for one that is merely
       * incomplete: the count this row prints is right either way, and the
       * INCOMPLETE row below says the rest. */
      uint32_t cc = (m->dev.slots_used >= m->dev.slots_max ||
                     m->dev.links_used >= m->dev.links_max ||
                     !sensors_writable() || !sensors_provenance_loaded())
                        ? UI_DANGER
                        : UI_FAINT;
      menu_row(fb, h, y, sc, lh, "SLOTS", capv, cc, -1, 0);
      y += lh;
   }
   /* THE SCREEN IS BEHIND THE DISK. Said here rather than left to the log: a
    * change that persisted but did not publish looks exactly like a tap that
    * did nothing, and the remedy (change anything again) is not guessable. */
   /* THE REGISTRY IS READ-ONLY, SAID WHERE IT IS FELT. With slots.csv unread
    * the published view is empty, so this screen shows no devices at all --
    * which reads as "you have none" rather than "the file did not load", and
    * every control on it then fails with a message about storage. The SLOTS
    * row above turns red for the same reason and ADD NEW DEVICE is withheld
    * below. */
   if (ro_row) {
      const char *label = "UNREADABLE";
      const char *which = "SENSORS.CSV - RESTART";
      if (!sensors_writable()) {
         which = sensors_provenance_loaded() ? "SLOTS.CSV - RESTART"
                                             : "BOTH FILES - RESTART";
      } else if (sensors_provenance_loaded()) {
         /* Both files read; sensors.csv simply has no row for some slot. */
         label = "INCOMPLETE";
         which = "SENSORS.CSV - NO TYPE";
      }
      menu_row(fb, h, y, sc, lh, label, which, UI_DANGER, -1, 0);
      y += lh;
   }
   if (stale_row) {
      menu_row(fb, h, y, sc, lh, "DISPLAY", "BEHIND - RETRY", UI_DANGER, -1, 0);
      y += lh;
   }
   if (win_row) {
      /* THE NEWEST ARE THE ONES SHOWN, which is the half that matters: the
       * sensor being worn is always among them. Disconnecting the ones no
       * longer in use is what brings the rest back into reach. */
      char wr[32];
      (void)snprintf(wr, sizeof wr, "NEWEST %d OF %d", carried_live,
                     m->dev.nlive_all);
      menu_row(fb, h, y, sc, lh, "IN SERVICE", wr, UI_DANGER, -1, 0);
      y += lh;
   }
   /* OFFERED WHILE THE REGISTRY HAS ROOM, asked of the same two numbers the
    * SLOTS row above prints. A tally of the devices in THIS frame cannot
    * answer it: the frame carries every live device but only a page of the
    * retired ones, and a retired device holds its slot for ever. */
   /* NOT OFFERED WHILE EITHER FILE IS UNREAD: the claim would be refused, and
    * a button that always fails is worse than one that is not there -- the row
    * above says which file. Both gates, because a mint needs the provenance
    * table and a slot needs the slot table, and a device that gets one without
    * the other is exactly the half-registered state the two files exist to
    * make impossible. Without the second gate the whole flow -- the type
    * picker, the four-digit code, the scan and the confirmation -- runs before
    * anything says no. */
   if (m->dev.slots_used < m->dev.slots_max && sensors_writable() &&
       sensors_provenance_loaded()) {
      /* A real framed button, like SYNC NOW / FORGET DEVICE, not a plain row.
       */
      y += lh; /* separate it from the device list above */
      menu_button(fb, h, x, y, fb->width - (2 * x), sc, "ADD NEW DEVICE",
                  UI_TEXT, MA_ADDSENSOR, 0);
   }
}

/* ---- permissions + background controls (opened from SETTINGS) ---- */

void render_perms(struct ANativeWindow_Buffer *fb, const struct screen *m,
                  struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "PERMISSIONS", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_PERMS_BACK, 0);
   /* 3*lh after the title and 2*lh between rows -- the DISPLAY menu's spacing,
    * which is the house style for a settings submenu. This screen was the only
    * one still packed at the bare line height, so six rows of GRANTED / DENIED
    * ran together into a block that had to be read word by word to find the
    * one that was wrong. It fits: at 22 rows the budget already covered a
    * longer list than this one, and DISPLAY carries more rows at exactly this
    * spacing. */
   y += 3 * lh;

   /* EVERY PERMISSION THE APP ASKS FOR, so this screen answers the question
    * it exists to answer. Bounded by NPERMS rather than by a count written
    * here: a permission added to menu.c's list must appear on the screen that
    * says what has been granted, and a literal is how one comes to be
    * requested, denied, and never mentioned. */
   for (int i = 0; i < NPERMS; i++) {
      int g = m->sys.perm[i];
      menu_row(fb, h, y, sc, lh, ui_perm_lbl[i], g ? "GRANTED" : "DENIED",
               g ? UI_OK : UI_DANGER, MA_PERM, i);
      y += 2 * lh;
   }
   menu_row(fb, h, y, sc, lh, "BATTERY",
            m->sys.batt_ok ? "UNRESTRICTED" : "OPTIMIZED",
            m->sys.batt_ok ? UI_OK : UI_DANGER, MA_BATTERY, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "STANDBY", ui_bucket_label(m->sys.standby_bucket),
            (m->sys.standby_bucket > 0 && m->sys.standby_bucket <= 20)
                ? UI_OK
                : UI_SYNC_STALE,
            -1, 0);
   y += 2 * lh;
   menu_row(fb, h, y, sc, lh, "BG EXEC",
            m->sys.bg_restricted ? "RESTRICTED" : "ALLOWED",
            m->sys.bg_restricted ? UI_DANGER : UI_OK, MA_BGEXEC, 0);
}

/* ---- ALARM submenu (opened from SETTINGS, or straight from the main
 * screen's alarm row left of "LOW") ---- */

/* A section caption: grey, no value, NO HIT BOX. It must not be tappable --
 * a target that looks like a row and does nothing teaches the user that rows
 * here sometimes do nothing, which is the last thing an alarm screen should
 * teach. menu_row with a negative code records none. */

/* THE ONE SCREEN'S GEOMETRY, passed rather than recomputed.
 *
 * render_sensor was 399 lines: four sections -- identity, live status, device
 * info and the action buttons -- sharing eight local variables and one
 * running `y`. Each is now its own function, and this is what they share. A
 * struct rather than eight parameters, because the parameter list is what
 * makes a helper too tedious to split out in the first place. */
struct sensor_geo {
   int sc;  /* text scale */
   int lh;  /* line pitch */
   int x;   /* left margin */
   int rx;  /* right edge for right-aligned values */
   int tsc; /* title scale */
};

/* WHAT THIS DEVICE IS and how the user has styled it: the rows that are
 * settings rather than readings. */
static int sensor_identity_rows(struct ANativeWindow_Buffer *fb,
                                const struct screen *m, struct hits *h,
                                const struct ui_sensor *s,
                                const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* Identity: type + name (+ PRIMARY for a CGM), no section title. */
   menu_row(fb, h, y, g->sc, g->lh, "TYPE", sensor_disp_name(s->type), UI_TEXT,
            -1, 0);
   y += g->lh;
   menu_row(fb, h, y, g->sc, g->lh, "NAME", s->label, UI_TEXT, MA_LABEL, 0);
   y += g->lh;
   if (s->have_rec && s->kind == KIND_CGM && !s->old) {
      /* PRIMARY only for a LIVE CGM -- a disconnected one cannot own the big
       * number. */
      menu_row(fb, h, y, g->sc, g->lh, "PRIMARY", s->primary ? "YES" : "NO",
               s->primary ? UI_OK : UI_TEXT, MA_PRIMARY, 0);
      y += g->lh;
   }
   /* One MARKER row -- shows the ACTUAL glyph (in the device's colour), not a
    * name; shape + size + colour all live in its combined menu. */
   draw_str(px, fb, g->x, y, g->sc, "MARKER", UI_TEXT_DIM);
   if (s->marker == MARK_HIDE) {
      int lw = str_len("OFF") * 6 * g->sc;
      draw_str(px, fb, g->rx - lw, y, g->sc, "OFF", UI_FAINT);
   } else {
      /* Glyph reflects the configured SIZE too (same scaling as the plot). */
      int gr = (2 * g->sc * s->size) / MARK_SIZE_DEF;
      if (gr < g->sc)
         gr = g->sc;
      if (gr > 5 * g->sc)
         gr = 5 * g->sc;
      plot_marker_glyph((struct plot_fb){px, fb->stride, fb->width, fb->height},
                        g->rx - (6 * g->sc), y + (3 * g->sc), gr, s->marker,
                        ui_sensor_color(s->color));
   }
   add_hit_ix(h, ui_rect(0, y - (3 * g->sc), fb->width, g->lh), MA_MARKER, 0);
   y += g->lh;
   return y;
}

/* WHAT IT IS DOING: session state, the clocks, and the last calibration.
 * Read-only -- every row here is the sensor's own account of itself. */
static int sensor_status_rows(struct ANativeWindow_Buffer *fb,
                              const struct screen *m, struct hits *h,
                              const struct ui_sensor *s,
                              const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* --- read-only --- */
   y += g->lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, g->x, y, g->sc, "STATUS", UI_MUTED);
   y += g->lh;
   /* A disconnected device reads EXPIRED (red); otherwise its live status. */
   uint32_t stcol = UI_FAINT;
   if (s->old)
      stcol = UI_DANGER;
   else if (s->connected)
      stcol = UI_OK;
   menu_row(fb, h, y, g->sc, g->lh, "STATE", s->old ? "EXPIRED" : s->status,
            stcol, -1, 0);
   y += g->lh;
   {
      char rs[16]; /* link RSSI (moved off the main screen). No age here -- LAST
                    * SYNC sits right beside it and carries the time. */
      if (s->rssi_ok && s->rssi_t > 0)
         (void)snprintf(rs, sizeof rs, "%d DB", s->rssi);
      else
         (void)snprintf(rs, sizeof rs, "--");
      menu_row(fb, h, y, g->sc, g->lh, "SIGNAL STRENGTH", rs, UI_TEXT, -1, 0);
      y += g->lh;
   }
   {
      /* LAST SEEN: the most recent time we heard from this device -- a meter's
       * last connect/sync, a CGM's last reading. Directly under SIGNAL
       * STRENGTH, whose value is the signal captured at that same moment. */
      char when[20];
      char rel[12];
      char val[48]; /* "<date up to 19> (<rel up to 11> AGO)" + NUL */
      long seen =
          (s->have_rec && s->kind == KIND_BGM) ? s->meter_sync_t : s->last_any;
      if (seen > 0) {
         fmt_date(seen, m->tz_off, when, sizeof when);
         fmt_ago(m->now, seen, rel, sizeof rel);
         (void)snprintf(val, sizeof val, "%s (%s AGO)", when, rel);
      } else {
         (void)snprintf(val, sizeof val, "--");
      }
      menu_row(fb, h, y, g->sc, g->lh, "LAST SEEN", val, UI_TEXT, -1, 0);
      y += g->lh;
      /* A meter's fingerstick time is DISTINCT from its sync, so it keeps a
       * separate LAST DATA row; a CGM's LAST SEEN already IS its data time. */
      if (s->have_rec && s->kind == KIND_BGM) {
         if (s->last > 0) {
            fmt_date(s->last, m->tz_off, when, sizeof when);
            fmt_ago(m->now, s->last, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "%s (%s AGO)", when, rel);
         } else {
            (void)snprintf(val, sizeof val, "--");
         }
         menu_row(fb, h, y, g->sc, g->lh, "LAST DATA", val, UI_TEXT, -1, 0);
         y += g->lh;
      }
   }
   if (s->have_rec && s->kind == KIND_CGM) {
      /* Holds "<value> <unit>" for the PRED row: fmt_glu into a 12-byte buffer
       * (up to 11 chars, as the compiler sees it) + ' ' + a 6-char unit + NUL.
       */
      char b[24];
      /* Only show session timing once a real session is known. Before the first
       * reading session_seconds is 0, which otherwise renders as "started 0s
       * ago, ends in 15 days" -- misleading, so show "--" instead. */
      char when[20];
      char rel[12];
      char val[36];
      /* An OLD device has no live session clock, so its STARTED/ENDS/ELAPSED
       * come from the PERSISTED activation instant rather than `now - clock`.
       * A live one uses the running clock as before. */
      int have_session = 0;
      long began       = 0;
      if (!s->old && s->session_seconds > 0) {
         /* THE LIVE CLOCK WHEN THERE IS ONE: it comes off the sensor's own
          * 0x4e response and ticks per second, so it is exact. */
         have_session = 1;
         began        = m->now - s->session_seconds;
      } else {
         /* THE RECORDED ACTIVATION OTHERWISE, for a live sensor exactly as
          * for a retired one.
          *
          * A live sensor has no session clock until its first 0x4e of the
          * process, and it loses it again on every reconnect -- so between
          * them this screen answered STARTED, ENDS, ELAPSED and REMAINING
          * with "--" for a sensor that had been running for two weeks. The
          * instant it started is not something the link has to re-learn: it
          * is minted once and kept in the provenance row, which is the same
          * durable fact the retired-device branch has always used and is
          * where an ended session's timings come from. The live clock is
          * preferred only because it is exact to the second, not because it
          * is the only thing that knows. */
         have_session = (s->activation > 0);
         began        = s->activation;
      }
      long len = s->wear_len; /* per-device: override / model / type */
      /* STARTED shows the absolute instant only. The relative age lives in the
       * ELAPSED row, so a parenthetical "(N AGO)" here was pure duplication. */
      if (have_session)
         fmt_date(began, m->tz_off, when, sizeof when);
      else
         (void)snprintf(when, sizeof when, "--");
      menu_row(fb, h, y, g->sc, g->lh, "STARTED", when, UI_TEXT, -1, 0);
      y += g->lh;
      if (len > 0) {
         /* ENDS shows the absolute instant only; REMAINING (below) carries the
          * relative countdown, mirroring STARTED/ELAPSED. */
         /* AND NOT WHILE THE WEAR LENGTH IS A GUESS. `len` is the type
          * default until the sensor reports its model, so an instant
          * computed from it states a 10-day end for what may be a 15-day
          * sensor -- the WEAR row above refuses to name that length, and a
          * date derived from it is the same claim wearing a timestamp. */
         if (have_session && !s->wear_prov)
            fmt_date(began + len, m->tz_off, when, sizeof when);
         else
            (void)snprintf(when, sizeof when, "--");
         menu_row(fb, h, y, g->sc, g->lh, "ENDS", when,
                  (have_session && !s->wear_prov && began + len < m->now)
                      ? UI_DANGER
                      : UI_TEXT,
                  -1, 0);
         y += g->lh;
      }
      /* ELAPSED: a live device's running clock; an old device's final run
       * (last reading minus its start), which is how long it actually lasted.
       */
      /* Measured from `began`, so it follows whichever source above supplied
       * it -- with a live clock the two are the same number by construction.
       * An OLD device stopped, so its run ends at its last reading. */
      long elapsed = m->now - began;
      if (s->old)
         elapsed = (s->last > began) ? s->last - began : len;
      if (have_session)
         fmt_dur(elapsed, b, sizeof b);
      else
         (void)snprintf(b, sizeof b, "--");
      menu_row(fb, h, y, g->sc, g->lh, "ELAPSED", b, UI_TEXT, -1, 0);
      y += g->lh;
      /* REMAINING: relative time to session end, which is the question being
       * asked -- not an absolute ENDS timestamp to subtract by hand. EXPIRED
       * (red) once the session length is exceeded. */
      if (len > 0) {
         long ends     = began + len;
         uint32_t rcol = UI_TEXT;
         if (s->old) {
            /* A disconnected device is done -- no countdown, just EXPIRED. */
            (void)snprintf(val, sizeof val, "EXPIRED");
            rcol = UI_DANGER;
         } else if (!have_session || s->wear_prov) {
            /* No session yet, or no known wear length to count against (see
             * ENDS above): both are "unknown", and neither may become a
             * countdown the user plans a sensor change around. */
            (void)snprintf(val, sizeof val, "--");
         } else if (ends >= m->now) {
            /* Imminence carries colour here too: YELLOW inside the last day
             * (fmt_dur already switches to hours + minutes there), RED inside
             * the final two hours. */
            long left = ends - m->now;
            if (left < 86400)
               rcol = (left < 2L * 3600) ? UI_DANGER : UI_WARN;
            fmt_dur(left, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "%s", rel);
         } else if (s->sess_state == SENSOR_STATE_ENDED) {
            /* The sensor's own verdict, same rule as the main screen. */
            (void)snprintf(val, sizeof val, "ENDED");
            rcol = UI_DANGER;
         } else {
            /* Past the nominal end: count the grace down -- and past the
             * grace, KEEP counting into the negative (same rule as the main
             * screen's SESSION row): the sign says "past the hard stop"
             * while still showing by how much, which stays honest even when
             * the wear budget is set wrong for a sensor visibly alive. No
             * sign inside the first negative minute (never "-0 M"). */
            long gl = ends + SENSOR_GRACE_S - m->now;
            fmt_dur((gl < 0) ? -gl : gl, rel, sizeof rel);
            (void)snprintf(val, sizeof val, "GRACE %s%s",
                           (gl <= -60) ? "-" : "", rel);
            rcol = (gl < 2L * 3600) ? UI_DANGER : UI_WARN;
         }
         menu_row(fb, h, y, g->sc, g->lh, "REMAINING", val, rcol, -1, 0);
         y += g->lh;
      }
      /* Same sentinel rule as the main screen's PRED row: `predicted` is a
       * 10-bit field whose 0x3ff (1023) means "no prediction", and no real
       * value exceeds Dexcom's 400 mg/dL cap. Without the upper bound this
       * row showed a bare 1023 while the main screen showed "--" for the same
       * sensor -- two answers to the same question, one of them a number the
       * user could act on. */
      if (s->predicted > 0 && s->predicted <= 400) {
         char pv[12];
         fmt_glu(s->predicted, m->prefs.units, pv, sizeof pv);
         (void)snprintf(b, sizeof b, "%s %s", pv, UI_LBL(m->prefs.units));
         menu_row(fb, h, y, g->sc, g->lh, "PRED", b, UI_TEXT, -1, 0);
         y += g->lh;
      }
      (void)snprintf(b, sizeof b, "%d", s->sequence);
      menu_row(fb, h, y, g->sc, g->lh, "SEQ", b, UI_TEXT, -1, 0);
      y += g->lh;
   }
   return y;
}

/* WHAT IT IS: the identifiers, which never change while it is worn. */
static int sensor_info_rows(struct ANativeWindow_Buffer *fb,
                            const struct screen *m, struct hits *h,
                            const struct ui_sensor *s,
                            const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   y += g->lh; /* blank line between sections, matching the SETTINGS menu */
   draw_str(px, fb, g->x, y, g->sc, "DEVICE INFO", UI_MUTED);
   y += g->lh;
   if (s->code[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "CODE", s->code, UI_TEXT, -1, 0);
      y += g->lh;
   }
   /* ONE ROW FOR "WHICH DEVICE IS THIS ON THE RADIO", and the OS name wins it
    * when there is one. The address identifies the sensor to this app; the
    * name is what the phone's own Bluetooth list shows, so it is the only
    * thing that says which entry there belongs to this row -- which is what a
    * user needs when a stale bond has to be forgotten. The address is still
    * in the logs and in sensors.csv.
    *
    * SHARING the row rather than taking another: 31 rows is this screen's
    * ceiling, not a budget (see render_sensor), and a 32nd drops the whole
    * screen's text a size on a 720x1600 phone. */
   const char *radio_lbl = s->adv_name[0] ? "BT NAME" : "MAC";
   const char *radio_val = s->mac[0] ? s->mac : "--";
   if (s->adv_name[0])
      radio_val = s->adv_name;
   menu_row(fb, h, y, g->sc, g->lh, radio_lbl, radio_val, UI_TEXT, -1, 0);
   y += g->lh;
   if (s->serial[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "SN", s->serial, UI_TEXT, -1, 0);
      y += g->lh;
   }
   if (s->model[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "SW", s->model, UI_TEXT, -1, 0);
      y += g->lh;
   }
   if (s->fw[0]) {
      menu_row(fb, h, y, g->sc, g->lh, "FW", s->fw, UI_TEXT, -1, 0);
      y += g->lh;
   }
   if (s->have_rec && s->kind == KIND_CGM) {
      /* WEAR belongs with the device facts: the nominal budget the
       * countdown judges against. Dexcom sells 10- and 15-day G7s that
       * are indistinguishable on the air, so when the auto-resolution
       * guesses wrong this row is the correction.
       *
       * AUTO IS NAMED, AND A PIN IS COLOURED. Printing the same bare
       * "10 DAYS" for both makes a device whose model says 15 and whose
       * override says 10 look exactly like one correctly resolved to 10 --
       * a countdown five days short with nothing on screen to explain it.
       * Green for a pin matches every other row here where green means
       * "the user changed this from the default". */
      /* AND AN UNREAD MODEL IS BLANK, not a number. A G7 that has not yet
       * reported its DIS model could be either version, and the type default
       * standing in for it is a guess -- so this row says "--", the same
       * thing every other unknown field on this screen says, rather than
       * naming a length the sensor has never claimed. It fills in the moment
       * the model arrives. The row stays tappable throughout: pinning 10 or
       * 15 is how the user answers the question ahead of the sensor. */
      char wd[24];
      int wdays = (int)(s->wear_len / 86400);
      if (s->wear_prov)
         (void)snprintf(wd, sizeof wd, "--");
      else if (s->wear_auto)
         (void)snprintf(wd, sizeof wd, "AUTO %d D", wdays);
      else
         (void)snprintf(wd, sizeof wd, "%d DAYS", wdays);
      menu_row(fb, h, y, g->sc, g->lh, "WEAR", wd,
               s->wear_auto ? UI_TEXT : UI_OK, MA_WEAR, 0);
      y += g->lh;
   }
   /* RESCALE: the active multiplicative correction as a signed percentage, or
    * (NONE). Tapping opens the value keypad, or -- if already active -- the
    * CHANGE / STOP screen. Sits just above LAST CAL. */
   if (s->have_rec && s->kind == KIND_CGM) {
      char rv[32]; /* "PENDING " + value(<=11) + ' ' + unit(<=6) + NUL */
      uint32_t rcol = UI_TEXT;
      if (s->rescale_pending > 0) {
         char gv[12];
         fmt_glu(s->rescale_pending, m->prefs.units, gv, sizeof gv);
         (void)snprintf(rv, sizeof rv, "PENDING %s %s", gv,
                        UI_LBL(m->prefs.units));
         rcol = UI_BUSY;
      } else if (s->rescale_rejected) {
         (void)snprintf(rv, sizeof rv, "REJECTED >25%%");
         rcol = UI_DANGER; /* red */
      } else if (s->rescale_expired) {
         (void)snprintf(rv, sizeof rv, "EXPIRED - RE-ENTER");
         rcol = UI_DANGER; /* red */
      } else if (s->rescale_pm != 1000) {
         int d = s->rescale_pm - 1000; /* tenths of a percent */
         int a = (d < 0) ? -d : d;
         (void)snprintf(rv, sizeof rv, "%c%d.%d%%", (d < 0) ? '-' : '+', a / 10,
                        a % 10);
         rcol = UI_BUSY; /* amber: active */
      } else {
         (void)snprintf(rv, sizeof rv, "(NONE)");
      }
      /* AHEAD OF THE DISK. The factor is real -- it is scaling every reading
       * on this screen -- but the file has not taken it yet, and a restart
       * would show something else. Said in the row rather than left to be
       * discovered at the next launch; calib_tick keeps retrying, so this is
       * normally on screen for one tick.
       *
       * IT REPLACES THE VALUE RATHER THAN BEING APPENDED TO IT. The layout
       * targets UI_COLS (33) characters and this row already spends most of
       * them; " NOT SAVED" on the end of the longest value overflowed the
       * label -- and clipped to the row's own buffer it read "NOT SAVE",
       * which is worse than saying nothing. The value comes back the moment
       * the retry lands.
       *
       * ONLY WHEN THE ROW HAS SOMETHING TO LOSE: the flag is per-FILE, not
       * per-sensor, so a second CGM with no factor of its own would
       * otherwise announce that its "(NONE)" was unsaved. */
      if (s->rescale_unsaved &&
          (s->rescale_pending > 0 || s->rescale_rejected ||
           s->rescale_expired || s->rescale_pm != 1000)) {
         (void)snprintf(rv, sizeof rv, "NOT SAVED - RETRYING");
         rcol = UI_DANGER; /* red */
      }
      menu_row(fb, h, y, g->sc, g->lh, "RESCALE", rv, rcol, MA_RESCALE_OPEN, 0);
      y += g->lh;
   }
   /* LAST CAL: sits right above the CALIBRATION button so the outcome of the
    * last calibration is next to where you start a new one. Shows (NONE), a
    * PENDING queue entry, or the resolved outcome -- the accepted mg/dL value,
    * or FAIL. */
   if (s->have_rec && s->kind == KIND_CGM) {
      char cv[40];
      char gv[12];
      uint32_t ccol = UI_TEXT;
      if (s->cal_pending > 0) {
         fmt_glu(s->cal_pending, m->prefs.units, gv, sizeof gv);
         (void)snprintf(cv, sizeof cv, "PENDING %s %s", gv,
                        UI_LBL(m->prefs.units));
         ccol = UI_BUSY; /* amber: in progress */
      } else if (s->cal_t > 0) {
         char cd[20];
         fmt_date(s->cal_t, m->tz_off, cd, sizeof cd);
         if (s->cal_state == CAL_ST_APPLIED) {
            fmt_glu(s->cal_mgdl, m->prefs.units, gv, sizeof gv);
            (void)snprintf(cv, sizeof cv, "%s %s %s", cd, gv,
                           UI_LBL(m->prefs.units));
            ccol = UI_SENS_GREEN; /* green: accepted */
         } else {
            /* Distinct failure kinds so REJECTED (bad value) is not confused
             * with NOT SUPPORTED (sensor forbids calibration). */
            const char *w = "FAILED";
            if (s->cal_state == CAL_ST_REJECTED)
               w = "REJECTED";
            else if (s->cal_state == CAL_ST_NOTSUP)
               w = "NOT SUPPORTED";
            (void)snprintf(cv, sizeof cv, "%s %s", cd, w);
            ccol = UI_DANGER; /* red */
         }
      } else {
         (void)snprintf(cv, sizeof cv, "(NONE)");
      }
      /* See the RESCALE row above, for the replacement and for the guard: an
       * ACCEPTED or REJECTED verdict that could not be written stands (the
       * sensor cannot be asked twice), so the row says the next launch may
       * disagree -- but only on a row that is showing a verdict or a queue in
       * the first place. */
      if (s->cal_unsaved && (s->cal_pending > 0 || s->cal_t > 0)) {
         (void)snprintf(cv, sizeof cv, "NOT SAVED - RETRYING");
         ccol = UI_DANGER; /* red */
      }
      /* While a calibration is queued, the row itself is a shortcut into the
       * CAL PENDING menu (REPLACE / DELETE); otherwise it is display-only. */
      menu_row(fb, h, y, g->sc, g->lh, "LAST CAL", cv, ccol,
               s->cal_pending > 0 ? MA_CAL_OPEN : -1, 0);
      y += g->lh;
   }
   return y;
}

/* THE ACTIONS, kept together at the bottom and framed, with the destructive
 * one well clear of the others rather than one fat finger below. */
static int sensor_action_buttons(struct ANativeWindow_Buffer *fb,
                                 const struct screen *m, struct hits *h,
                                 const struct ui_sensor *s,
                                 const struct sensor_geo *g, int y)
{
   uint32_t *px = fb->bits;
   (void)px;
   (void)m;
   /* --- actions as framed buttons, kept together at the bottom --- */
   int bw = fb->width - (2 * g->x);
   y += 2 * g->lh;
   if (s->old) {
      /* A DISCONNECTED device: no live actions (calibrate/sync need a link).
       * RECONNECT revives it -- sensible for a sensor pulled BEFORE it expired
       * (still within its wear window). The handler shows a confirmation first
       * when the sensor is already expired, since reconnecting a dead sensor
       * rarely makes sense. */
      menu_button(fb, h, g->x, y, bw, g->sc, "RECONNECT", UI_GO, MA_RECONNECT,
                  0);
      return y;
   }
   /* THREE CASES, because the action belongs to a KIND and a row with no
    * provenance has none. A two-way test sends it to the meter arm -- `kind`
    * is zero-initialised and KIND_CGM is 0 -- and offers SYNC NOW on a CGM,
    * where the handler finds no meter, does nothing, and says nothing. No
    * button at all is the honest answer: the device's type is unknown until
    * sensors.csv reads. */
   if (!s->have_rec)
      y += 0; /* no action can be named for a device with no type */
   else if (s->kind == KIND_CGM)
      y = menu_button(fb, h, g->x, y, bw, g->sc, "CALIBRATION", UI_TEXT,
                      MA_CAL_OPEN, 0);
   else
      y = menu_button(fb, h, g->x, y, bw, g->sc, "SYNC NOW", UI_TEXT, MA_SYNC,
                      0);
   /* DISCONNECT is destructive: red, and well clear of the action above it
    * rather than one fat finger below. It only opens a confirmation. */
   y += 2 * g->lh;
   menu_button(fb, h, g->x, y, bw, g->sc, "DISCONNECT", UI_ALERT, MA_FORGET, 0);
   return y;
}

void render_sensor(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 31 ROWS AT A 14*sc PITCH, and both halves of that are load-bearing.
    *
    * THE COUNT IS THE WORST CASE, not the usual one: every optional attribute
    * present -- ENDS, REMAINING, PRED, SN and CODE -- which is the NORMAL state
    * once DIS has answered and a pairing code is stored, plus the DISCONNECT
    * button under them. Sized for fewer, the button is drawn below the buffer
    * on 1080x1920, 1080x2400 and 2560x1440: the screen's one consequential
    * action, off the surface and dead to touch, exactly when the device is
    * fully described.
    *
    * THE PITCH IS WHAT KEEPS THE TEXT READABLE. Asking ui_fit_scale for 31 rows
    * at the usual 16*sc floors sc to 2 where the rest of the app gets 3, which
    * reads as microscopic; 31*14 is the same height as 27*16, so the tighter
    * pitch buys the rows without the cliff.
    *
    * AND 31 IS A CEILING, NOT A BUDGET. One more row drops sc from 3 to 2 on a
    * 720x1600 phone -- ui_fit_scale is a floor divide, so its steps are cliffs
    * -- and shrinks this whole screen's text. A new fact rides on a row that
    * already exists, the way the PAIRING state is folded into SESSION below. */
   int sc  = ui_fit_scale(fb->width, fb->height, 31);
   int tsc = FONT_TITLE(sc);
   int lh  = 14 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   /* Record the way OUT before the range guard.
    *
    * Returning early left this screen with ZERO hit targets -- ui_render has
    * already cleared them, and on_input swallows every tap while a menu is
    * open. A stale selection (MA_SENSOR_BACK sets sel = -1, and several paths
    * then re-open a menu without setting it) therefore produced a blank screen
    * that ignored all input: force-stop required. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_SENSOR_BACK,
              0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors) {
      /* SAY SO RATHER THAN DRAWING NOTHING. The escape above keeps this from
       * being a lockout, but an all-black screen with one invisible tap
       * target reads as a crashed app -- and this screen is the last thing
       * on the path when a device stops existing under it. A line of text
       * and a named way out is the difference between "it died" and "there
       * is nothing here". */
      draw_str(px, fb, x, y, tsc, "NO DEVICE", UI_MUTED);
      draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
      return;
   }
   /* AND A SURFACE TOO SHORT FOR THE ROWS GETS THE SAME TREATMENT. ui_fit_scale
    * floors at 1, so on a short landscape surface (480 tall and under, which
    * ORIENTATION in DISPLAY is one tap away from) this screen lays 31 rows out
    * past the bottom and DISCONNECT is drawn nowhere. Truncating in silence
    * reads as a screen with no disconnect on it; saying so names the remedy,
    * and the escape recorded above is still the way back. */
   if (ui_screen_too_short(fb->width, fb->height, 31)) {
      draw_str(px, fb, x, y, tsc, m->dev.sensors[m->dev.sel].label, UI_TEXT);
      draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
      y += 2 * lh;
      draw_str(px, fb, x, y, sc, "SCREEN TOO SHORT", UI_DANGER);
      y += lh;
      draw_str(px, fb, x, y, sc, "FOR THIS DEVICE", UI_DANGER);
      return;
   }
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   draw_str(px, fb, x, y, tsc, s->label, UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* No second registration here: the one above the range guard covers the
    * identical rect with the identical action, and a duplicate target burns a
    * slot of the UI_MAX_HITS budget and shows up in any reachability audit as
    * a shadowed control, masking a real one. */
   y += 2 * lh;

   struct sensor_geo geo = {sc, lh, x, rx, tsc};
   y                     = sensor_identity_rows(fb, m, h, s, &geo, y);
   y                     = sensor_status_rows(fb, m, h, s, &geo, y);
   y                     = sensor_info_rows(fb, m, h, s, &geo, y);
   sensor_action_buttons(fb, m, h, s, &geo, y);
}

/* ---- calibration confirmation ---- */

void render_cal(struct ANativeWindow_Buffer *fb, const struct screen *m,
                struct hits *h)
{
   uint32_t *px = fb->bits;
   /* 25 rows: what this screen spends at its tallest -- the title band, the
    * reading it is calibrating against, the entry field and the two buttons,
    * with room for the rows that appear only while a calibration is pending.
    * Sized for fewer, the buttons at the bottom are laid out below the surface,
    * where draw_str clips them silently and they answer no tap. */
   int sc  = ui_fit_scale(fb->width, fb->height, 25);
   int tsc = FONT_TITLE(sc);
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   /* Record the way OUT before the range guard.
    *
    * Returning early left this screen with ZERO hit targets -- ui_render has
    * already cleared them, and on_input swallows every tap while a menu is
    * open. A stale selection (MA_SENSOR_BACK sets sel = -1, and several paths
    * then re-open a menu without setting it) therefore produced a blank screen
    * that ignored all input: force-stop required. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_CAL_BACK, 0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];

   /* Confirmation for the value just typed on the keypad. The write is the most
    * consequential in the app, so it happens ONLY on CONFIRM below. */
   draw_str(px, fb, x, y, tsc, "CONFIRM", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* No second registration here: the one above the range guard covers the
    * identical rect with the identical action, and a duplicate target burns a
    * slot of the UI_MAX_HITS budget and shows up in any reachability audit as
    * a shadowed control, masking a real one. */
   y += 2 * lh;

   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, UI_TEXT, -1, 0);
   y += lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->cal.cal_pending, m->prefs.units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->prefs.units));
      menu_row(fb, h, y, sc, lh, "CALIBRATE TO", v, UI_OK, -1, 0);
      y += lh;
   }
   y += 2 * lh;

   int bw = fb->width - (2 * x);
   y      = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_CAL_BACK, 0);
   y += 3 * lh; /* wide gap so CONFIRM is deliberate */
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_OK, MA_CAL_ENTER, 0);
}

/* Confirm a rescale: shows the target value and the clamped percentage, applied
 * only on CONFIRM. Mirrors render_cal. */
void render_rescale(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_RESCALE_BACK,
              0);
   if (m->dev.sel < 0 || m->dev.sel >= m->dev.nsensors)
      return;
   const struct ui_sensor *s = &m->dev.sensors[m->dev.sel];
   draw_str(px, fb, x, y, tsc, "RESCALE", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* Title gap and row spacing follow the DISPLAY menu, like every other
    * settings screen: these three rows were packed at the bare line height
    * and read as one paragraph rather than three separate facts. */
   y += 3 * lh;
   menu_row(fb, h, y, sc, lh, "DEVICE", s->label, UI_TEXT, -1, 0);
   y += 2 * lh;
   {
      char b[16];
      char v[24];
      fmt_glu(m->cal.rescale_entry, m->prefs.units, b, sizeof b);
      (void)snprintf(v, sizeof v, "%s %s", b, UI_LBL(m->prefs.units));
      menu_row(fb, h, y, sc, lh, "TARGET", v, UI_OK, -1, 0);
      y += 2 * lh;
   }
   {
      char v[20];
      uint32_t vc = UI_BUSY;
      if (m->cal.rescale_pm == 0) {
         /* No reading yet to compute against -- do not show a bogus 0%. */
         (void)snprintf(v, sizeof v, "ON NEXT READING");
         vc = UI_FAINT;
      } else {
         fmt_rescale_pct(m->cal.rescale_pm, v, sizeof v);
         /* Beyond +-25% will be REJECTED on CONFIRM -- flag it red. */
         if (m->cal.rescale_pm < 750 || m->cal.rescale_pm > 1250)
            vc = UI_DANGER;
      }
      menu_row(fb, h, y, sc, lh, "RESCALE BY", v, vc, -1, 0);
      y += 2 * lh;
   }
   y += lh;
   int bw = fb->width - (2 * x);
   y = menu_button(fb, h, x, y, bw, sc, "CANCEL", UI_TEXT, MA_RESCALE_BACK, 0);
   y += 3 * lh;
   menu_button(fb, h, x, y, bw, sc, "CONFIRM", UI_OK, MA_RESCALE_ENTER, 0);
}

/* ---- ADD menu: the main-screen '+' lands here ---- */

/* THE PINNABLE-ACTION TABLE, in ADD-menu order.
 *
 * These are the actions the ADD menu's PIN column offers, and the ones the
 * main screen draws as buttons when pinned.
 *
 * The device types are deliberately absent. A pin is for something done
 * often -- logging a dose, logging a weight, glancing at a log -- and adding a
 * device is done a handful of times in a sensor's life, from a menu that also
 * asks for a pairing code. Promoting it would spend one of three scarce slots
 * on the rarest action here.
 *
 * `abbrev` must fit a third of the '+' row, so keep them to seven characters.
 * They are NOT generated by truncating the full label: "VIEW INSULIN LOG" cut
 * to seven is "VIEW IN", which names nothing. */

void render_olddev(struct ANativeWindow_Buffer *fb, const struct screen *m,
                   struct hits *h)
{
   uint32_t *px = fb->bits;
   int sc       = ui_fit_scale(fb->width, fb->height, 22);
   int tsc      = FONT_TITLE(sc);
   int lh       = 16 * sc;
   int x        = 4 * sc;
   int rx       = fb->width - (4 * sc);
   int y        = (fb->height / 20) + (8 * sc);

   draw_str(px, fb, x, y, tsc, "OLD DEVICES", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_OLDDEV_BACK,
              0);
   y += 3 * lh;

   /* Collect the retired slots' indices (into m->dev.sensors) in list
    * order. */
   /* The retired devices THIS FRAME CARRIES -- the page the model filled for
    * m->dev.old_page. The registry's total is m->dev.nold, which is what the
    * page arithmetic below counts with. */
   /* THE PAGE, AND ONLY THE PAGE. The frame emits the page's retired devices
    * in list order and may append ONE more -- the selected device, when it is
    * retired and off this page -- for the per-device screen to draw. Taking
    * every retired row here would put that one in a row the page owns. */
   int idxs[UI_MAX_SLOTS];
   int nwin = 0;
   for (int i = 0; i < m->dev.nsensors && nwin < m->dev.nold_win; i++)
      if (m->dev.sensors[i].old)
         idxs[nwin++] = i;
   int nold = m->dev.nold;
   if (nold <= 0) {
      draw_str(px, fb, x, y, sc, "NONE YET. DISCONNECTED", UI_MUTED);
      y += lh;
      draw_str(px, fb, x, y, sc, "DEVICES APPEAR HERE.", UI_MUTED);
      return;
   }

   /* Rows that fit between the title and a reserved bottom nav line. Each row
    * takes 2*lh (a blank line between). At least one, always. */
   /* CAP THE ROWS BY THE HIT BUDGET, not just by height.
    *
    * Each row records a touch target, and add_hit silently DROPS everything
    * past UI_MAX_HITS -- so a window tall enough for more rows than the
    * budget allows drew them all and left the trailing ones, including the
    * next-page arrow, dead to touch. Reproduced at 480x1920 and 540x2340
    * with a full log. Reserve UI_LOG_FIXED for this screen's own controls
    * (title/close plus the pagination pair) and let the pages absorb the
    * rest: fewer rows per page is a visible, honest consequence; an
    * untappable control is not. */
   /* THE PAGE SIZE IS THE MODEL'S, not this screen's height. The frame filled
    * exactly the retired devices on page m->dev.old_page, so the rows drawn
    * here are that window and nothing else -- a height-derived page size
    * would index past what was filled. UI_OLD_PAGE is chosen to fit the
    * shortest supported screen (see uifmt.h); the hit budget is checked
    * anyway, because a row nothing can tap is worse than a row not drawn. */
   /* WHAT THIS WINDOW CAN ACTUALLY HOLD, measured, and then TOLD TO THE MODEL
    * for the next frame (menu_old_rows_set). A page is drawn from the rows
    * the frame carries, so the two numbers must be one number: measuring here
    * and filling UI_OLD_PAGE there loses the tail of every page to rows that
    * are never painted and cannot be tapped. Reporting it instead makes a
    * short screen show SMALLER pages, with every device still reachable. */
   int fits = UI_OLD_PAGE;
   if (fits > UI_MAX_HITS - UI_LOG_FIXED)
      fits = UI_MAX_HITS - UI_LOG_FIXED;
   int avail = fb->height - y - (2 * lh); /* reserve the nav line */
   if (fits > avail / (2 * lh))
      fits = avail / (2 * lh);
   if (fits < 1)
      fits = 1;
   menu_old_rows_set(fits);

   /* THE PAGE THE FRAME WAS BUILT WITH, not what was just measured: this
    * frame carries the devices the model chose with the size it was given,
    * and paging by anything else would step over devices it did not fill. A
    * geometry change therefore costs ONE frame at the old page size, and the
    * next frame is built with the new one. */
   int page_rows = m->dev.old_rows > 0 ? m->dev.old_rows : UI_OLD_PAGE;
   int per       = page_rows;
   if (per > fits)
      per = fits; /* never paint more rows than the window holds */
   /* THE PAGE COUNT COMES FROM THE SIZE THE FRAME WAS PAGED WITH, not from
    * what fits right now. They are the same number except on the one frame
    * after a geometry change, and on that frame the page INDEX means pages of
    * `page_rows` -- so counting them any other way would label the window
    * with a position it does not have. The next frame is built with the new
    * size and the two agree again. */
   int npages = (nold + page_rows - 1) / page_rows;
   if (npages < 1)
      npages = 1;
   /* ALREADY CLAMPED, by the model that filled the window (see there): this
    * screen draws the page the frame carries, so re-deriving it here is how
    * the two come to disagree. */
   int page = m->dev.old_page;

   for (int r = 0; r < per && r < nwin; r++) {
      const struct ui_sensor *s = &m->dev.sensors[idxs[r]];
      char val[20];
      long agot =
          (s->have_rec && s->kind == KIND_BGM) ? s->meter_sync_t : s->last;
      if (agot > 0)
         fmt_date(agot, m->tz_off, val, sizeof val);
      else
         (void)snprintf(val, sizeof val, "--");
      char name[3 + sizeof s->label];
      (void)snprintf(name, sizeof name, "  %s", s->label);
      menu_row(fb, h, y, sc, lh, name, val, UI_FAINT, MA_SENSOR, idxs[r]);
      if (s->marker != MARK_HIDE) {
         int gr = (2 * sc * s->size) / MARK_SIZE_DEF;
         if (gr < sc)
            gr = sc;
         if (gr > 3 * sc)
            gr = 3 * sc;
         plot_marker_glyph(
             (struct plot_fb){px, fb->stride, fb->width, fb->height},
             (4 * sc) + (3 * sc), y + (3 * sc), gr, s->marker,
             ui_sensor_color(s->color));
      }
      y += 2 * lh; /* blank line between rows */
   }

   /* Bottom navigation, only when there is more than one page. "<" and ">"
    * are generous tap targets on the left and right; the page count is
    * centred between them. */
   pager_row(fb, h, x, rx, fb->height - lh - (4 * sc), sc, lh, page, npages,
             MA_OLDPAGE);
}

/* ---- MARKER / COLOR pickers: a full list of options, each with a live glyph,
 * so the user sees every choice before selecting. The title row returns to the
 * device's own menu (MA_SENSOR + its slot). ---- */

void render_devlist(struct ANativeWindow_Buffer *fb, const struct screen *m,
                    struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded (see ui_fit_scale). Left on
    * width-only scaling, this screen's controls were laid out past the
    * bottom in landscape -- and render_forget records no close target, so
    * it became a dead end with no way back. */
   int sc  = ui_fit_scale(fb->width, fb->height, 26);
   int tsc = FONT_TITLE(sc);
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);

   char sel_title[24];
   if (m->dev.add_kind == KIND_BGM)
      (void)snprintf(sel_title, sizeof sel_title, "SELECT METER");
   else
      (void)snprintf(sel_title, sizeof sel_title, "SELECT %s",
                     m->dev.add_type ? m->dev.add_type : "SENSOR");
   (void)draw_title_fit(px, fb, x, y, tsc, sel_title, UI_TEXT,
                        rx - x - (7 * tsc));
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 8 * tsc), MA_DEV_CANCEL,
              0);
   y += 2 * lh;

   if (m->dev.ndev <= 0) {
      draw_str(px, fb, x, y, sc, "SEARCHING FOR SENSORS...", UI_MUTED);
      return;
   }
   draw_str(px, fb, x, y, sc, "NEAREST FIRST -- TAP YOURS:", UI_MUTED);
   y += 2 * lh;

   /* selection sort by RSSI, strongest first (the model owns index -> device)
    */
   int order[16];
   /* devs may be null with a stale ndev; every other list here is guarded the
    * same way. The renderer is a pure function of the model and must not
    * trust two fields to agree. */
   int n = (m->dev.devs && m->dev.ndev > 0) ? m->dev.ndev : 0;
   if (n > 16)
      n = 16;
   for (int i = 0; i < n; i++)
      order[i] = i;
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         if (m->dev.devs[order[j]].rssi > m->dev.devs[order[i]].rssi) {
            int t    = order[i];
            order[i] = order[j];
            order[j] = t;
         }
   for (int kk = 0; kk < n; kk++) {
      int i = order[kk];
      char rs[12];
      (void)snprintf(rs, sizeof rs, "%d dBm", m->dev.devs[i].rssi);
      menu_row(fb, h, y, sc, lh, m->dev.devs[i].name, rs, UI_TEXT, MA_DEV_PICK,
               i);
      y += lh;
   }
}

/* First-run permission rationale + a CONTINUE button (records
 * ACT_GATE_CONTINUE). All static copy; the model is unused beyond the
 * framebuffer size. */

/* ---- OneTouch: how to wake the meter, then a Scan button ---- */
void render_meterhelp(struct ANativeWindow_Buffer *fb, const struct screen *m,
                      struct hits *h)
{
   uint32_t *px = fb->bits;
   (void)m;
   int sc  = ui_fit_scale(fb->width, fb->height, 20);
   int tsc = FONT_TITLE(sc);
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   draw_str(px, fb, x, y, tsc, "ADD ONETOUCH", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   /* X returns to the ADD DEVICE type picker. */
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_ADDSENSOR, 0);
   y += 3 * lh;

   static const char *const steps[] = {
       "1. TURN THE METER ON.",
       "",
       "2. PRESS ITS UP ARROW AND",
       "   OK AT THE SAME TIME SO",
       "   THE BLUETOOTH SYMBOL",
       "   SHOWS.",
       "",
       "3. THEN TAP SCAN BELOW.",
   };
   for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
      draw_str(px, fb, x, y, sc, steps[i], UI_TEXT_DIM);
      y += lh;
   }
   y += 2 * lh;
   int bw = fb->width - (2 * x);
   menu_button(fb, h, x, y, bw, sc, "SCAN", UI_OK, MA_METERSCAN, 0);
}

/* ---- sensor-type picker (first step of adding a sensor) ---- */

void render_senstype(struct ANativeWindow_Buffer *fb, const struct screen *m,
                     struct hits *h)
{
   uint32_t *px = fb->bits;
   /* Height-bounded as well as width-bounded (see ui_fit_scale). Left on
    * width-only scaling, this screen's controls were laid out past the
    * bottom in landscape -- and render_forget records no close target, so
    * it became a dead end with no way back. */
   int sc  = ui_fit_scale(fb->width, fb->height, 20);
   int tsc = FONT_TITLE(sc);
   int lh  = 16 * sc;
   int x   = 4 * sc;
   int rx  = fb->width - (4 * sc);
   int y   = (fb->height / 20) + (8 * sc);
   (void)m;

   draw_str(px, fb, x, y, tsc, "ADD DEVICE", UI_TEXT);
   draw_str(px, fb, rx - (6 * tsc), y, tsc, "X", UI_TEXT);
   add_hit_ix(h, ui_rect(0, y - (3 * sc), fb->width, 2 * lh), MA_SENSOR_BACK,
              0);
   y += 4 * lh; /* generous gap below the title */

   /* Each type is a standard menu_button -- the SAME control every other
    * screen uses, at the same height, so buttons look and feel identical
    * across the app -- bespoke, taller buttons here would not. The OneTouch
    * shows its full name (the stored type name
    * stays "ONETOUCH" so the 16-char device label does not truncate). */
   int bw = fb->width - (2 * x);
   for (int t = SENSOR_STELO; t < SENSOR_NTYPES; t++) {
      y = menu_button(fb, h, x, y, bw, sc, sensor_disp_name(t), UI_TEXT,
                      MA_TYPE, t);
      y += lh;
   }
}
