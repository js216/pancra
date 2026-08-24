// SPDX-License-Identifier: GPL-3.0
// settings.h --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_SETTINGS_H
#define PANCRA_SETTINGS_H
#include "loadresult.h" /* enum load_result: named below, before style.h */

/* MAIN-SCREEN PINS: up to six ADD-menu actions pinned onto the main screen,
 * beside the big '+'. Stored by IDENTITY, not as positions in the ADD menu --
 * that menu's order is presentation and has changed before, and a stored
 * position would silently start launching a different action the next time a
 * button is inserted. SC_NONE = an empty slot. Kept dense (empties last) so
 * the main screen can just walk it.
 *
 * SIX, AND SIX IS A LAYOUT FACT. The main screen lays the pins out in at most
 * two rows of at most three: three or fewer take one row, four to six take
 * two. A seventh has nowhere to go that is still a fingertip wide on a narrow
 * phone, so the ceiling is the layout's and not an arbitrary round number --
 * raise them together or not at all.
 *
 * NINE, WHICH IS THREE ROWS OF THREE. It grew as space did: three in one row,
 * then six when the out-of-range banner moved off the bottom, then nine when
 * the ALARM and NUDGE rows became one. The main screen packs them two to a row
 * until there are more than six and three to a row after that, so this number
 * and that rule are the same fact -- see pin_percol in uimain.c.
 *
 * Files written before each rise hold fewer fields and load with the rest
 * empty; settings.c's positional format is what makes appending safe. */
#define SC_MAX 9

/* THE PREFERENCES, READ-ONLY.
 *
 * Every preference this module owns and persists, in one view. It is const
 * because a preference that anything can write is a preference that can be
 * changed without being SAVED -- and that difference is invisible until the app
 * restarts and reverts the user's choice. Every change goes through a
 * settings_set_* call below, which stores and persists in one step.
 *
 * The two THRESHOLD PAIRS live here together, and in the same file on disk,
 * because they are the same kind of fact and must not be able to load
 * half-applied against each other: anything that moves them must preserve
 * nudge_low >= alarm_low and nudge_high <= alarm_high, or the nudge sits
 * inside the alarm and can never fire first.
 *
 * The two SOUND pairs are separate on purpose. The alarm means "act now" and
 * the nudge means "have a look"; silencing the one you can ignore must not
 * silence the one you cannot, and a user who mutes the alarm in a meeting may
 * still want the buzz. */
struct prefs {
   char model[24], fw[24], mfr[24]; /* DIS: model / firmware / manufacturer */
   int alarm_low, alarm_high;       /* thresholds, mg/dL */
   int nudge_low, nudge_high;       /* the one-time heads-up band */
   int sound_on, vib_on;            /* alarm sound / vibration */
   int nudge_sound, nudge_vib;      /* the nudge's own pair */
   int orient;        /* 0 portrait 1 landscape 2 gravity 3 system */
   int screen_on;     /* 1 keep the screen on while open, 0 follow the OS */
   int newdata_mode;  /* an ND_* value; convert with nudge_mode_of() */
   int units;         /* glucose: 0 mg/dL, 1 mmol/L */
   int wunits;        /* weight: WT_KG / WT_LB (weight.h) */
   int disc;          /* stale-data alarm: index into disc_min */
   int plot_max;      /* plot vertical-scale top, mg/dL */
   int ins_marker[2]; /* MARK_* per insulin type (INS_SLOW/FAST) */
   int ins_color[2];  /* ui palette index per type */
   int ins_size[2];   /* marker size 1..MARK_SIZE_MAX per type */
   int statbar_val;   /* 1 = the status bar shows the value, 0 = the icon */
   int lockscr_val;   /* 1 = the notification is visible on the lock screen */
   int shortcut[SC_MAX]; /* the main-screen pins, by identity (see below) */
   /* THE LONGEST IN-RANGE RUN THIS APP HAS EVER SEEN, in seconds.
    *
    * NOT A PREFERENCE, and it is worth saying so in the struct that holds
    * nothing else like it. It is a record -- a fact about the user's data --
    * and it lives here because this file is the only durable key-value store
    * the app has; the alternative was a second file holding one integer, with
    * its own loader, its own corruption story and its own atomic write.
    *
    * It is capped at a year on the way in and on the way out, and it is only
    * ever raised. Losing it costs the user a number on the plot and nothing
    * else, which is why it is allowed to share a file whose other fields are
    * all recoverable defaults. */
   int best_streak_s;
   char code_str[16];      /* runtime pairing code (PAIR NEW SENSOR) */
   int remote_on;          /* 1 = push to the server */
   char remote_server[64]; /* hostname or IP; "" = not set */
   int remote_port;        /* server TCP port, 1..65535 */
};

/* (prefs() and sync_creds() are gone. They handed back POINTERS INTO the
 * live aggregates, and the live aggregates are written from a binder thread
 * -- info_set, from the sensor's device-information reply -- and from
 * the sync worker. Every caller was therefore one statement away from an
 * incoherent multi-field read or a borrowed string rewritten under it, and
 * two of them were exactly that. Documenting "you should copy" enforces
 * nothing; the copy is the only way in now. Inside the settings modules the
 * state is still reached directly, under set_lk.) */

/* ONE COHERENT COPY of the whole aggregate, and the only way to read it.
 *
 * The live state is written from a binder thread (info_set, from the
 * sensor's device-information reply) and from the sync worker. A reader that
 * touched it directly could see two fields from two instants, and a
 * `const char *` taken from it could be rewritten under the borrower -- which
 * is what the device screen did with model and firmware. A copy cannot be. */
void settings_get(struct prefs *out);

/* WHAT A SETTER ANSWERS.
 *
 * Every one of them is a transaction: the value is stored, the file is
 * replaced, and if the replace fails the stored value goes back -- so a
 * caller
 * that ignores this answer will draw a screen showing a choice that was not
 * kept. SETTINGS_FULL is the pin list's own refusal (the user picked those;
 * an extra one is declined rather than evicting one of them). */
#define SETTINGS_OK      0
#define SETTINGS_UNSAVED (-1)
#define SETTINGS_FULL    (-2)

/* WHAT A PINNED SHORTCUT IS, ON DISK.
 *
 * These numbers are a FILE FORMAT. They are written into settings.csv and read
 * back on every launch, so they may never be reused for anything else and may
 * never be renumbered -- a phone that upgrades has to keep the pins its owner
 * chose.
 *
 * They live HERE, in the domain, and not in ui.h, because what a person pinned
 * is a fact about their preferences and not about the renderer. The file used
 * to store MA_* codes straight out of ui.h: "log a slow dose" was 23 on disk
 * because 23 was where that button's touch code happened to fall in a
 * hand-allocated integer namespace shared with every other control in the app.
 * Renumbering a renderer constant would then silently repoint somebody's pin
 * -- and the whole reason the codes were renumbered at all was that they kept
 * colliding.
 *
 * the renderer maps these to whatever touch code it currently uses; see
 * ui_shortcut_id() and ui_shortcut_slot_by_id(). */
enum shortcut_id {
   SC_NONE     = 0,
   SC_INS_FAST = 1,
   SC_INS_SLOW = 2,
   SC_INSLOG   = 3,
   SC_WEIGHT   = 4,
   SC_WTLOG    = 5,
   SC_FOOD     = 6,
   SC_EXERCISE = 7,
   SC_FOODLOG  = 8,
   SC_EXLOG    = 9,
   SC_ID_LAST  = SC_EXLOG /* the largest id this build defines */
};


/* Point this module at the data directory; the five filenames live here. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int settings_paths(const char *dir);

/* The two CREDENTIAL paths, for the sync client that must know which files
 * not to upload. The other three files this module owns are private to it. */

/* The alarm and settings files, for the corruption tests (see settings.c). */


/* "low high nudge_low nudge_high\n". The nudge pair was appended, so files
 * written before it exist with two fields only -- those load the alarm pair
 * and leave the nudge at its (OFF) defaults, like every other loader here. */
/* CHANGE AND PERSIST, in one call. Prefer these to writing the global and
 * remembering the save: the rule is not enforceable, and a forgotten save is
 * invisible until the app restarts and reverts the user's choice. */
int settings_set_units(int mmol);
int settings_set_wunits(int wu);
int settings_set_sound(int on);

/* The rest of the writable preferences, on the same contract: each validates
 * its own value, persists it, and puts the stored one back if the write
 * failed.
 *
 * The CYCLING ones take no argument -- "next orientation" is the operation the
 * UI actually performs, and spelling it as a read-modify-write of the stored
 * orientation at
 * the call site put the wrap arithmetic, and the knowledge of how many states
 * there are, in the renderer's dispatcher. */
int settings_cycle_orient(void);
int settings_cycle_disc(void);
int settings_cycle_newdata(void);
int settings_set_vib(int on);
int settings_set_screen_on(int on);
int settings_set_statbar(int on);
int settings_set_lockscr(int on);
int settings_set_nudge_sound(int on);
int settings_set_nudge_vib(int on);
/* Clamps to 100..400 and applies the renderer's scale, which settings_load
 * also does -- one place that knows a plot maximum has a range. */
int settings_set_plot_max(int mgdl);
/* Raise the recorded best in-range run. A value that is not an improvement is
 * accepted and does nothing -- callers see this every time a reading lands
 * during an ordinary streak -- so it never writes the file for a value it
 * already holds. */
int settings_set_best_streak(int seconds);
/* The longest run a streak may claim, and the bound the loader applies. A
 * year: beyond it the number stops being a fact about a person and starts
 * being a fact about a corrupt file. */
#define BEST_STREAK_MAX (365L * 86400)
/* 1..65535. SETTINGS_OK if stored; SETTINGS_UNSAVED if the port was out of
 * range or the file could not be replaced. */

/* THE FOUR THRESHOLDS, STORED AND PERSISTED AS ONE.
 *
 * There is no way from here to move one of them. A public one-field setter
 * per threshold carries three unwritten obligations for the caller: hold the
 * alarm lock, check the pair is still ordered, and follow with alarm_save().
 * A caller meeting two of the three leaves a live threshold the next launch
 * would not have -- on the numbers that decide whether a hypo alarm can
 * fire.
 *
 * WHICH pair to move, and whether the move keeps them ordered, is the ALARM's
 * decision: it needs its own lock to read the partner and choose atomically.
 * Callers want alarm_set_threshold() (alarm.h), which is that operation.
 * This is the storage half, and it is all-or-nothing -- SETTINGS_UNSAVED
 * leaves all four as they were. */

/* One insulin type's plot styling; -1 (or 0 for size) leaves a field alone. */
int settings_set_ins_style(int type, int marker, int color, int size);

/* The pinned shortcuts, as the operations the UI performs. The list is DENSE
 * -- the button row stops at the first empty slot -- so removal closes the
 * gap. Add returns SETTINGS_FULL when the list is full: the extra tap is
 * REFUSED rather than evicting a pin the user chose. Removing something that
 * is not pinned is SETTINGS_OK -- there was nothing to store and nothing to
 * lose. */
int settings_pin_add(int id);
int settings_pin_remove(int id);
int settings_pinned(int id);

/* "sound vib orient units disc plot_max screen_on\n" -- fields are read
 * positionally and the parse stops at the first missing one, so appending a
 * field keeps older config files loadable (the new field keeps its default). */
enum load_result settings_load(void);


/* (THE OTHER FOUR FILES ARE NOT HERE. The device information, the alarm
 * thresholds, the pairing code and the remote credentials each have their own
 * module and their own header --
 * app/devinfo.h, app/alarmcfg.h, app/paircode.h, app/remotecfg.h -- and the
 * save engine all five share is private to them (app/setpriv.h). What is
 * left here is the preferences file.
 *
 * THE RAW PER-FILE WRITES live in those headers beside the transactions they
 * belong to, because a caller who has one of these headers already has the
 * setter that should be used instead. `make settingscheck` refuses a
 * production call to a raw save: every setting is changed through a setter
 * that persists it and puts the stored value back if the write failed.) */
/* How many sensor/marker colours exist. The table itself (UI_NCOLORS) is
 * private to the renderer, so settings_load cannot bound its stored colour
 * index against it directly. The shared name for that count -- and the marker
 * shapes the insulin styles are bounded against -- is style.h, which neither
 * this header nor sensors.h owns. */
#include "style.h"

/* 1 iff s is a well-formed dotted quad (four octets, each 0..255). Pure. */

/* A pin id read from a file written by an older build, mapped to this
 * build's own. SC_NONE for one this build no longer offers. */
int shortcut_migrate(int stored);

#ifdef APP_FAULTS
/* ---- THE SAVE'S OWN WINDOW, OPENED BY A TEST ----------------
 *
 * A save renders its bytes under set_lk and writes them with the lock
 * released, so a frame never waits for flash -- and that opens a window in
 * which an OLDER render can reach the file after a newer one. Every job
 * carries the generation it rendered at and a write that is not newer than
 * what is on disk is skipped; this hook is how that skip is made to happen
 * on purpose rather than by luck.
 *
 * Called between the render and the write, on the saving thread, when it is
 * set. The test blocks one thread in it while another completes a newer
 * save. Never compiled into the app: nothing that ships defines APP_FAULTS.
 */
extern void (*settings_fault_gap_here)(void);

/* The generation of settings.cfg that is KNOWN TO BE ON DISK. A write whose
 * render is older than this is skipped, which is the guard the hook above
 * exists to make fire; this is how a test states that it did not go
 * backwards. 0 before anything has been written. */
unsigned settings_fault_written_gen(void);

#endif

#endif
