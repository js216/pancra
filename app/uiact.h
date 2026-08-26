// SPDX-License-Identifier: GPL-3.0
// uiact.h --- What a tap MEANS: the hit table and the action vocabulary
// Copyright 2026 Jakob Kastelic
//
/* THE OTHER HALF OF THE PURE UI. uimodel.h is what the renderer reads; this is
 * what it writes -- one rectangle per control, each carrying the action the
 * shell should perform -- and what ui_hit() maps a later tap through.
 *
 * The vocabulary is here rather than in the renderer because BOTH sides need
 * it: the renderer records the codes and menu.c dispatches them. It was a set
 * of bare literals shared by hand, then a set of names with hand-picked VALUES
 * and reserved gaps, and that arithmetic is what turned "open sensor 8" into
 * "close the sensor screen" when MAX_SLOTS grew. See struct action.
 */
#ifndef PANCRA_UIACT_H
#define PANCRA_UIACT_H

#include "plot.h" /* struct plot_cfg: what the plot was drawn with */

/* One touch target and the action the shell acts on.
 *
 * THREE FIELDS, AND THE THIRD IS THE POINT. `code` carries NO base+index
 * arithmetic: with "sensor 3" as MA_SENSOR + 3 and "digit 7" as MA_DIGIT + 7,
 * seventeen such bases pack into one integer namespace with gaps chosen by
 * hand. Nothing checks the gaps are big enough, so raising MAX_SLOTS turns
 * "open sensor 8" into "close the sensor screen" -- a wrong branch that
 * reaches sensor_forget and the calibration write. Ten _Static_asserts would
 * exist solely to make those collisions build errors.
 *
 * With the index in its own field there are no ranges to collide: every code
 * is a plain tag, and "which one" is a separate number that cannot run into
 * the next tag however large it gets. The asserts are gone because the thing
 * they guarded against is no longer expressible.
 *
 * `arg` stays for the actions whose payload is a VALUE rather than an index
 * (a plot span in hours, a +-1 delta, a render scale). */
struct action {
   int kind, code, arg;
};

enum {
   ACT_NONE = 0,
   ACT_OPEN_SETTINGS,
   ACT_PLOT_TAB,      /* arg = plot span in hours */
   ACT_SCRUB,         /* a press inside the plot; shell resolves the point */
   ACT_GATE_CONTINUE, /* first-run rationale: request permissions */
   /* Modal screens (settings / keypad / device list) speak the shell's
    * menu_action protocol; arg carries that integer code so menu_action stays
    * the single dispatch point for their JNI/pairing side effects. */
   ACT_MENU,
};

/* The codes ACT_MENU carries: one per control, and nothing else.
 *
 * These were bare literals shared by hand between the renderer and the
 * shell's menu_action(); then they were named, but kept hand-picked VALUES
 * with gaps reserved after seventeen of them, because a code like "sensor 3"
 * was MA_SENSOR + 3 and a run could grow into its neighbour. That is what the
 * ten deleted _Static_asserts guarded, and what turned "open sensor 8" into
 * "close the sensor screen" when MAX_SLOTS grew.
 *
 * The index lives in its own field now (struct action above), so a code is a
 * plain TAG: the list below is DENSE and unnumbered, adding one is appending
 * a name, and there is no arithmetic that could reach a different control.
 * Where a comment says "ix = ...", that is the second field, not an offset.
 *
 * NOT PERSISTED. Pinned shortcuts are stored as SC_* ids (settings.h) exactly
 * so this list stays free to change; the one place an old MA_* value survives
 * is shortcut_migrate's table of literals, which reads files written before
 * that split. */
enum ui_menu {
   /* NOT AN ACTION. Zero is what an empty shortcut slot and an unset code
    * both read as, so it must not name a control: with MA_ORIENT sitting at 0
    * an empty pin was one stray dispatch away from cycling the screen
    * orientation. */
   MA_NONE = 0,
   MA_ORIENT,
   MA_SOUND,
   MA_VIB,
   MA_UNITS,
   MA_DISC,
   MA_SCREEN,
   MA_NEWDATA,      /* cycle the new-datapoint alert: OFF/BEEP/CHIRP */
   MA_METERSCAN,    /* start scanning from the OneTouch instructions screen */
   MA_INS_FAST,     /* ADD menu: LOG FAST INSULIN (type preset) */
   MA_INS_SLOW,     /* ADD menu: LOG SLOW INSULIN (type preset) */
   MA_INSLOG_OPEN,  /* ADD menu: open the INSULIN LOG table */
   MA_INSLOG_BACK,  /* insulin log: back to the ADD menu */
   MA_INSLOG_PAGE,  /* insulin log: go to page ix (see pager_row) */
   MA_INSMARK_OPEN, /* ix = INS_SLOW / INS_FAST: pick that type's marker */
   MA_INS_DELETE,   /* EDIT INSULIN: delete this dose (red) */
   MA_INSMARK_BACK, /* insulin marker picker: back to DISPLAY */
   MA_DISPLAY_OPEN, /* settings: open the DISPLAY submenu */
   MA_DISPLAY_BACK, /* display submenu: back to settings */
   MA_STATBAR,      /* toggle status bar: value vs app icon */
   MA_NOTIF_REOPEN, /* re-post the (swiped-away) notification */
   MA_LOCKSCR,      /* toggle lock-screen notification visibility */
   MA_INSDEL_YES,   /* delete-dose confirmation: really delete */
   MA_INSDEL_NO,    /* delete-dose confirmation: back to the form */
   MA_ALARM_LOW,    /* LOW <value> (main row / ALARM menu): keypad 10 */
   MA_ALARM_HIGH,   /* HIGH <value> (main row / ALARM menu): keypad 11 */
   MA_ALARM_OPEN,   /* open the ALARM submenu (settings row, or the
                     * main alarm row LEFT of "LOW") */
   MA_ALARM_BACK,   /* ALARM submenu: back to where it was opened */
   MA_DEVICES_BACK, /* DEVICES screen: back to where it was opened */
   /* EXPORT DATA menu: parked in the free band above the device-picker codes
    * (which end at 270) and below MA_CHAR (300). */
   MA_EXP_RANGE, /* cycle the range: 30 D / 1 Y / ALL */
   MA_EXP_GLU,   /* toggle the GLUCOSE (readings) section */
   MA_EXP_DEV,   /* toggle the DEVICES (sensors) section */
   MA_EXP_INS,   /* toggle the INSULIN (doses) section */
   MA_EXP_WT,    /* toggle the WEIGHT section */
   MA_EXP_GO,    /* build the CSV and open the share sheet */
   MA_EXP_BACK,  /* back to settings, nothing exported */
   /* NUDGE thresholds (main-screen row / ALARM menu): keypad 12 / 13. Parked
    * in the same free band, above MA_EXP_BACK and below MA_CHAR (300); the
    * only gap left next to the MA_ALARM_* codes is 129, and one is not
    * enough (it now holds MA_DEVICES_BACK). */
   MA_NUDGE_LOW,
   MA_NUDGE_HIGH,
   MA_NUDGE_SOUND, /* nudge section: toggle its sound */
   MA_NUDGE_VIB,   /* nudge section: toggle its vibration */
   /* WEIGHT log, in the same free band below MA_CHAR. */
   MA_WT_OPEN,    /* ADD menu: open the LOG WEIGHT form */
   MA_WTLOG_OPEN, /* ADD menu: open the weight table */
   MA_WTLOG_BACK, /* weight table: back to the ADD menu */
   MA_WTLOG_PAGE,
   MA_WT_CONFIRM, /* LOG WEIGHT: append the entry */
   MA_WT_DISCARD, /* LOG WEIGHT: leave without logging */
   /* LOG WEIGHT fields, mirroring MA_INS_EDIT; ix picks one: 0 weight,
    * 1 date, 2 time, 3 year. */
   MA_WT_EDIT,
   MA_WUNITS,    /* DISPLAY: toggle KG / LB */
   MA_WT_DELETE, /* EDIT WEIGHT: delete this entry (red) */
   MA_WTDEL_YES, /* delete confirmation: really delete */
   MA_WTDEL_NO,  /* delete confirmation: back to the form */
   MA_WTTAB,     /* ix = tab index */
   MA_EXTAB,     /* EXERCISE LOG plot: ix = span tab index */
   MA_INSTAB,    /* INSULIN LOG plot: ix = span tab index */
   /* WEIGHT LOG row: ix is the tail index, and opens that entry in the EDIT
    * WEIGHT form. */
   MA_WTLOG_EDIT,
   /* FOOD, in the same free band below MA_CHAR.
    *
    * The vocabulary and the entry are separate screens because they are
    * separate decisions -- WHICH food, then HOW MUCH of it -- and the picker
    * is reached from more than one place, so where it goes BACK to is
    * recorded on the way in rather than inferred here. */
   MA_FOOD_OPEN,     /* ADD menu: start logging food (opens the PICKER) */
   MA_FOODTYPE_PICK, /* picker: ix = the vocabulary index chosen */
   MA_FOODTYPE_NEW,  /* picker: type a food this list does not have yet */
   MA_FOODTYPE_BACK, /* picker: leave without choosing */
   MA_FOODPAGE, /* food picker: go to page ix */
   MA_FOOD_CONFIRM, /* LOG FOOD: append the entry */
   MA_FOOD_DISCARD, /* LOG FOOD: leave without logging */
   /* LOG FOOD fields, mirroring MA_WT_EDIT; ix picks one: 0 type, 1 grams,
    * 2 time, 3 date, 4 year. */
   MA_FOOD_EDIT,
   MA_FOODLOG_OPEN, /* ADD menu: open the food table */
   MA_FOODLOG_BACK,
   MA_FOODLOG_PAGE,
   MA_FOODLOG_EDIT, /* a row: open that entry in the form; ix = tail index */
   MA_FOOD_DELETE,  /* EDIT FOOD: delete this entry (red) */
   MA_FOODDEL_YES,
   MA_FOODDEL_NO,
   /* EXERCISE: one button on the ADD menu that cycles 0-1-2-3 and opens
    * nothing. It has no screen of its own, which is why it has exactly one
    * code here. */
   MA_EXERCISE,
   /* ...and its LOG, which does have screens: the same paginated, editable
    * table the insulin, weight and food logs have. The button that records
    * needs one code; correcting what it recorded needs the same eleven every
    * other log has. */
   MA_EXLOG_OPEN, /* ADD menu: open the exercise table */
   MA_EXLOG_BACK,
   MA_EXLOG_PAGE,
   MA_STEPS_TOGGLE, /* turn step counting on or off */
   MA_EXLOG_EDIT, /* a row: open that entry in the form; ix = tail index */
   MA_EX_CONFIRM, /* EDIT EXERCISE: rewrite the entry */
   MA_EX_DISCARD, /* EDIT EXERCISE: leave it as it was */
   /* EDIT EXERCISE fields; ix picks one: 0 level, 1 time, 2 date, 3 year.
    * LEVEL is not a keypad -- it cycles 1-2-3, the same three values the ADD
    * button offers, so there is one way to say "moderate" rather than two. */
   MA_EX_EDIT,
   MA_EX_DELETE, /* EDIT EXERCISE: delete this entry (red) */
   MA_EXDEL_YES,
   MA_EXDEL_NO,
   /* THE MODEL'S OWN SCREEN. Two ways in, deliberately: the stats table on the
    * main screen is where a number about the near future belongs, and
    * SETTINGS is where somebody goes looking for it by name. */
   MA_PERM, /* ix = permission index */
   MA_BATTERY,
   MA_BGEXEC,
   MA_PAIR_CODE,
   MA_PLOTMAX,
   MA_REMOTE_OPEN,   /* settings: open the REMOTE submenu */
   MA_REMOTE_TOGGLE, /* remote menu: enable/disable the push */
   MA_REMOTE_IP,     /* remote menu: edit the SERVER (text editor) */
   MA_REMOTE_PORT,   /* remote menu: edit the server port (keypad) */
   MA_REMOTE_BACK,   /* remote menu: back to settings */
   MA_SYNC_EMAIL,    /* remote menu: edit the account email (text) */
   MA_SYNC_PAIR,     /* remote menu: enter the 6-digit pairing code */
   MA_SYNC_UNPAIR,   /* remote menu: forget the paired identity */
   /* remote menu: pull back everything the server holds and this phone does
    * not. Manual by design -- see sync_restore in app/sync.h. */
   MA_SYNC_RESTORE,
   MA_SYNCREST_YES, /* the confirmation: really pull */
   MA_SYNCREST_NO,  /* the confirmation: back out */
   MA_SENSOR,       /* ix = the DEVICE ID; opens that sensor's screen */
   MA_SENSOR_BACK,
   MA_ADDSENSOR,
   MA_PRIMARY,
   MA_MARKER,
   MA_COLOR,
   MA_LABEL,
   MA_CAL_OPEN,
   MA_SYNC,
   MA_FORGET, /* opens the confirmation screen; does not act */
   MA_FORGET_YES,
   MA_FORGET_NO,
   MA_SIZE, /* cycle marker size */
   MA_CAL_REFRESH,
   MA_CAL_ENTER,
   MA_CAL_BACK,
   MA_CAL_REPLACE,    /* pending-cal screen: enter a new value (supersedes) */
   MA_CAL_CANCEL,     /* pending-cal screen: discard the queued calibration */
   MA_RESCALE_OPEN,   /* RESCALE row: keypad, or the active screen */
   MA_RESCALE_ENTER,  /* confirm a rescale value */
   MA_RESCALE_BACK,   /* leave a rescale screen unchanged */
   MA_RESCALE_CHANGE, /* active screen: enter a new value */
   MA_RESCALE_STOP,   /* active screen: turn rescaling off */
   MA_TYPE,           /* ix = the sensor type */
   MA_EXPORT,         /* settings: EXPORT DATA via the system share sheet */
   MA_PAIR_YES,       /* pairing confirmation: commit to the picked device */
   MA_PAIR_NO,        /* pairing confirmation: back to the device list */
   MA_ADD_OPEN,       /* main-screen '+': open the ADD menu */
   MA_INS_OPEN,       /* ADD menu: open the LOG INSULIN form */
   MA_INS_TYPE,       /* LOG INSULIN: toggle SLOW / FAST */
   /* (LOG INSULIN takes its number through the keypad, so it has no +/-
    * steppers and no codes for them. A code in this enum with no handler and
    * no button is one a tap can carry that nothing will act on;
    * `make -f test/Makefile actioncheck` refuses that shape.) */
   MA_INS_CONFIRM,  /* LOG INSULIN: append the dose */
   MA_INS_DISCARD,  /* LOG INSULIN: leave without logging */
   MA_WEAR,         /* device screen: toggle wear length 10 D / 15 D */
   MA_PEND_CANCEL,  /* DEVICES: the armed (pending) pairing row: ask */
   MA_PEND_STOP,    /* pending-pairing confirm: stop waiting */
   MA_PEND_KEEP,    /* pending-pairing confirm: leave it armed */
   MA_PERMS_OPEN,   /* settings: open the PERMISSIONS submenu */
   MA_PERMS_BACK,   /* permissions submenu: back to settings */
   MA_DEVICES_OPEN, /* open the DEVICES screen: the main screen's big
                            number, or the SETTINGS row */
   MA_OLDDEV_OPEN,  /* DEVICES: open the OLD DEVICES list */
   MA_OLDDEV_BACK,  /* OLD DEVICES list: back to DEVICES */
   MA_OLDPAGE,      /* OLD DEVICES: go to page ix */
   MA_DEVPAGE,      /* DEVICES: go to page ix of the live list */
   /* The PIN checkbox on the ADD menu; ix is a slot in the ui_shortcut_*
    * table. Nothing bounds that table -- a reserved code range would cap it
    * at 11 entries. */
   MA_SCTOGGLE,
   MA_RECONNECT, /* old device: revive it (direct if not yet expired,
                  * else via a confirmation screen) */
   MA_RECON_YES, /* reconnect-expired confirmation: do it */
   MA_RECON_NO,  /* reconnect-expired confirmation: cancel */
   MA_CLOSE,
   MA_DIGIT, /* ix = the digit */
   MA_OK,    /* keypad / label confirm */
   MA_DOT,   /* keypad '.' (remote-IP entry only) */
   MA_BACKSPACE,
   MA_KP_CLOSE,
   MA_DEV_CANCEL,
   MA_DEV_PICK,   /* ix = the candidate row */
   MA_MARK_PICK,  /* ix = the marker */
   MA_COLOR_PICK, /* ix = the colour */
   MA_SIZE_PICK,  /* ix = the size */
   /* The PRIMARY checkbox on the DEVICES screen; ix is the slot. The row
    * itself carries MA_SENSOR with the same ix, so the checkbox needs its own
    * CODE -- it is a second target inside the same row rectangle. */
   MA_PRIM_PICK,
   MA_CHAR, /* ix = index into ui_label_chars[] */
   /* LOG INSULIN fields; ix picks one: 0 units, 1 date, 2 time, 3 year.
    * Tapping the value opens the keypad for exact entry. */
   MA_INS_EDIT,
   /* INSULIN LOG row; ix is the tail index, and opens that dose in the EDIT
    * INSULIN form. */
   MA_INSLOG_EDIT,
};

/* WHY THERE ARE NO BASE-PLUS-INDEX RANGES HERE, AND NO ASSERTS GUARDING THEM.
 *
 * A code built as a BASE with a runtime index added -- MA_SENSOR + slot,
 * MA_DIGIT + digit, MA_CHAR + letter -- packs several codes into one integer
 * namespace whose gaps have to be picked by hand, and each run then needs a
 * _Static_assert saying it has not grown into its neighbour's. The margins are
 * nothing: MA_DIGIT + 9 lands exactly on MA_BACKSPACE with nothing to spare,
 * and MA_SENSOR + slot had
 * three. A collision does not crash or warn: the dispatcher simply runs the
 * wrong branch, so raising MAX_SLOTS turned "open sensor 8" into "close the
 * sensor screen" -- and a wrong branch here reaches sensor_forget and the
 * calibration write.
 *
 * struct action now carries the index in its own field (see there), so "sensor
 * 3" is (MA_SENSOR, 3) and not MA_SENSOR + 3. There is no arithmetic to
 * overflow into the next code and no range to keep clear, which is why the
 * asserts are deleted rather than kept: an assertion about a hazard that can
 * no longer occur is a comment that costs a build step.
 *
 * The values are no longer written down at all: the enum is dense, and adding
 * a code is appending a name. Nothing persists them (see settings.h). */
/* Up to this many touch targets per frame.
 *
 * Headroom is thinner than it looks: SCR_LABEL (the letter keypad) peaks at
 * 41 of these across the swept geometries, so seven more controls anywhere
 * on that screen is the ceiling. add_hit DROPS the excess, and a dropped box
 * is a control the user simply cannot tap -- drawn normally, dead to touch,
 * with nothing logged. That is the failure mode `overflow` below exists to
 * make loud; uitest asserts it stays clear at every screen and geometry. */
#define UI_MAX_HITS 48

/* Touch targets a paginated log screen spends on its OWN controls before any
 * row: the title/close band and the two pagination arrows, plus slack for a
 * per-screen extra (the weight log's span tabs). Rows are capped at
 * UI_MAX_HITS - UI_LOG_FIXED so a tall window cannot push a row -- or the
 * next-page arrow behind it -- past the budget and into add_hit's silent
 * drop. */
#define UI_LOG_FIXED 12

/* THE PLOT'S CONFIGURATION FOR THIS FRAME, recorded with the targets.
 *
 * The touch path has to reproduce the render's mapping exactly to resolve a
 * tap to the right datapoint, and both inputs to that mapping -- the vertical
 * scale and the marker radius -- are NOT process globals that plot_render
 * writes and plot_hit reads back. As globals, two plots with different
 * settings cannot exist at once, and the first touch after a scale change
 * answers against the previous one.
 *
 * It belongs HERE because this is already the per-frame record of what was
 * drawn where: the scrub rectangle is read out of it the same way (see
 * plot_rect in input.c). The renderer fills it in when it adds the ACT_SCRUB
 * target. */
struct hits {
   struct {
      int x, y, w, h, kind, code, arg;
      /* Pressed-highlight (glow) rect: what ui_press_overlay lights while
       * this control is armed. add_hit defaults it to the hit rect; a
       * control with a deliberately oversized hit zone (the settings
       * hamburger) narrows it to its visible glyph via add_glow, so
       * pressing it never lights unrelated pixels caught in the zone. */
      int gx, gy, gw, gh;
   } box[UI_MAX_HITS];

   /* What plot_render was called with for the ACT_SCRUB box above. */
   struct plot_cfg plot;

   int n;
   /* Set once add_hit has had to drop a target. Never reset by add_hit --
    * ui_render clears it with n at the start of a frame -- so one drop
    * anywhere in a frame is still visible after the frame is built. */
   int overflow;
};

/* DOES THIS ACTION'S ix NAME A DEVICE ROW?
 *
 * Two of them do, and they are the only place in the UI where an index means
 * a POSITION in the sensor table rather than a value. A position is exactly
 * what a redraw can renumber, so the row is turned into a device id at
 * touch-DOWN -- against the frame the finger landed on -- and the release
 * checks it still holds that device before firing. See input.c.
 *
 * Listed here, beside the enum, so a third device-row action has one obvious
 * place to register itself rather than a rule to remember. */
static inline int ma_names_device_row(int action)
{
   return action == MA_SENSOR || action == MA_PRIM_PICK;
}

#endif
