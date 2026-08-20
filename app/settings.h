// SPDX-License-Identifier: GPL-3.0
// settings.h --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_SETTINGS_H
#define PANCRA_SETTINGS_H

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
 * It was three, one row, until the out-of-range banner moved off the bottom of
 * the screen and left the space for a second. Files written before that hold
 * three fields and load with the last three empty; see settings.c's positional
 * format, which is what makes appending safe. */
#define SC_MAX 6

/* THE PREFERENCES, READ-ONLY.
 *
 * Everything settings.c owns and persists, in one view. It is const because a
 * preference that anything can write is a preference that can be changed
 * without being SAVED -- and that difference is invisible until the app
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
   int newdata_mode;  /* ND_OFF / ND_BEEP / ND_CHIRP (alarmlogic.h) */
   int units;         /* glucose: 0 mg/dL, 1 mmol/L */
   int wunits;        /* weight: WT_KG / WT_LB (weight.h) */
   int disc;          /* stale-data alarm: index into disc_min */
   int plot_max;      /* plot vertical-scale top, mg/dL */
   int ins_marker[2]; /* MARK_* per insulin type (INS_SLOW/FAST) */
   int ins_color[2];  /* ui palette index per type */
   int ins_size[2];   /* marker size 1..MARK_SIZE_MAX per type */
   int statbar_val;   /* 1 = the status bar shows the value, 0 = the icon */
   int lockscr_val;   /* 1 = the notification is visible on the lock screen */
   int shortcut[SC_MAX];   /* the main-screen pins, by identity (see below) */
   char code_str[16];      /* runtime pairing code (PAIR NEW SENSOR) */
   int remote_on;          /* 1 = push to the server */
   char remote_server[64]; /* hostname or IP; "" = not set */
   int remote_port;        /* server TCP port, 1..65535 */
};

/* (prefs() and sync_creds() are gone. They handed back POINTERS INTO the
 * live aggregates, and the live aggregates are written from a binder thread
 * -- settings_set_dis, from the sensor's device-information reply -- and from
 * the sync worker. Every caller was therefore one statement away from an
 * incoherent multi-field read or a borrowed string rewritten under it, and
 * two of them were exactly that. Documenting "you should copy" enforces
 * nothing; the copy is the only way in now. Inside settings.c the state is
 * still reached directly, under set_lk.) */

/* ONE COHERENT COPY of the whole aggregate, and the only way to read it.
 *
 * The live state is written from a binder thread (settings_set_dis, from the
 * sensor's device-information reply) and from the sync worker. A reader that
 * touched it directly could see two fields from two instants, and a
 * `const char *` taken from it could be rewritten under the borrower -- which
 * is what the device screen did with model and firmware. A copy cannot be. */
void settings_get(struct prefs *out);

/* WHAT A SETTER ANSWERS.
 *
 * Every one of them is a transaction: the value is stored, the file is
 * replaced, and if the replace fails the OLD value goes back -- so a caller
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
   SC_ID_LAST  = SC_FOODLOG /* the largest id this build defines */
};

/* Translate one shortcut value read from an OLD settings file.
 *
 * Files written before the pins had a schema of their own hold MA_* codes.
 * Every one of them is >= 21, and every id above is <= SC_ID_LAST, so the two
 * cannot be confused -- which is what makes this migration safe to run on
 * every load rather than gated on a format version nobody wrote down. */
int shortcut_migrate(int stored);

/* The paired identity, stored in the SAME file as the server -- which is one
 * of the files that must never be synced (see sync.h): it is the secret that
 * authenticates this phone to the server. 0 = not paired. */
/* THE PAIRED IDENTITY, read-only.
 *
 * The uid and key authenticate this phone TO the server, so nothing outside
 * settings.c may write them: a half-written identity (a uid without its key)
 * fails every request with nothing on screen to say why, and a rewritten one
 * silently repoints the account. Changed only by settings_forget_identity and
 * the pairing path, both of which persist what they change. */
struct sync_creds {
   long uid; /* 0 until this phone is paired */
   unsigned char key[16];
   char email[64]; /* the account being synced into; "" = not set */
};

/* WHERE THIS PHONE SYNCS, AND AS WHOM, read as ONE value.
 *
 * The endpoint lives in struct prefs and the identity in struct sync_creds,
 * and every caller used to take them with two separate acquisitions of the
 * settings lock: settings_get() then sync_creds_get(). Between those two
 * calls the settings screen can change the server and the pairing worker can
 * change the account -- so a request could be aimed at the OLD server signed
 * as the NEW account, or the reverse. Both fail authentication, and neither
 * says why: the screen shows a server that is correct and an account that is
 * correct, because each half of the pair really is.
 *
 * One acquisition, one value, and it is what scheduling and request
 * construction both carry. */
struct remote_config {
   int on;          /* the sync switch */
   char server[64]; /* host name; "" = not configured */
   int port;
   long uid; /* 0 until this phone is paired */
   unsigned char key[16];
   char email[64];
};

void remote_config_get(struct remote_config *out);

/* THE IDENTITY, as a COPY and only as a copy: the sync worker rewrites it,
 * and a uid read beside a key from before it changed is an identity that
 * fails every request with nothing on screen to say why. */
void sync_creds_get(struct sync_creds *out);
/* Remember a completed pairing, durably. */
int sync_key_save(long uid, const unsigned char key[16]);
/* Point this module at the data directory; the five filenames live here. */
/* 1 when every path this module persists to fitted; 0 when one did
 * not, and then NONE of them is usable -- see data_path in util.h. */
int settings_paths(const char *dir);

/* The two CREDENTIAL paths, for the sync client that must know which files
 * not to upload. The other three files this module owns are private to it. */
const char *code_path(void);
const char *remote_path(void);
/* The alarm and settings files, for the corruption tests (see settings.c). */
const char *alarm_path(void);
const char *settings_path(void);

int info_save(void); /* device-info strings "model\nfw\nmfr\n" */
enum load_result info_load(void);
/* "low high nudge_low nudge_high\n". The nudge pair was appended, so files
 * written before it exist with two fields only -- those load the alarm pair
 * and leave the nudge at its (OFF) defaults, like every other loader here. */
/* CHANGE AND PERSIST, in one call. Prefer these to writing the global and
 * remembering the save: the rule is not enforceable, and a forgotten save is
 * invisible until the app restarts and reverts the user's choice. */
int settings_set_units(int mmol);
int settings_set_wunits(int wu);
int settings_set_sound(int on);
int settings_set_remote_on(int on);
/* The rest of the writable preferences, on the same contract: each validates
 * its own value, persists it, and puts the old one back if the write failed.
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
/* 1..65535. SETTINGS_OK if stored; SETTINGS_UNSAVED if the port was out of
 * range or the file could not be replaced. */
int settings_set_remote_port(int port);

/* THE FOUR THRESHOLDS, STORED AND PERSISTED AS ONE.
 *
 * There is no way from here to move one of them. Four public one-field
 * setters used to exist, and each carried three unwritten obligations for the
 * caller: hold the alarm lock, check the pair is still ordered, and follow
 * with alarm_save(). A caller meeting two of the three left a live threshold
 * the next launch would not have -- on the numbers that decide whether a hypo
 * alarm can fire.
 *
 * WHICH pair to move, and whether the move keeps them ordered, is the ALARM's
 * decision: it needs its own lock to read the partner and choose atomically.
 * Callers want alarm_set_threshold() (alarm.h), which is that operation.
 * This is the storage half, and it is all-or-nothing -- SETTINGS_UNSAVED
 * leaves all four as they were. */
int settings_store_thresholds(int alarm_low, int alarm_high, int nudge_low,
                              int nudge_high);

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

/* The device-information strings the sensor reports, stored and persisted in
 * one call. Learned from the device, not chosen by the user. */
enum { SET_DIS_MODEL = 0, SET_DIS_FW = 1, SET_DIS_MFR = 2 };

int settings_set_dis(int which, const char *val);

/* The pairing code, stored and persisted in one call. */
int settings_set_code(const char *digits);

/* The sync server and account, copied in bounded and persisted. */
int settings_set_server(const char *host);
int settings_set_email(const char *addr);
/* Forget the paired identity (uid AND key), keeping the server. */
int settings_forget_identity(void);

int alarm_save(void);
enum load_result alarm_load(void);
/* "sound vib orient units disc plot_max screen_on\n" -- fields are read
 * positionally and the parse stops at the first missing one, so appending a
 * field keeps older config files loadable (the new field keeps its default). */
int settings_save(void);
enum load_result settings_load(void);
int code_save(void); /* pairing code digits */
enum load_result code_load(void);
/* "on ip port\n" -- garbage keeps the prior values, like every loader here. */
int remote_save(void);
enum load_result remote_load(void);
/* How many sensor/marker colours exist. The table itself (UI_NCOLORS) is
 * private to the renderer, so settings_load cannot bound its stored colour
 * index against it directly. The shared name for that count -- and the marker
 * shapes the insulin styles are bounded against -- is style.h, which neither
 * this header nor sensors.h owns. */
#include "loadresult.h" /* what a load actually found */
#include "style.h"

/* 1 iff s is a well-formed dotted quad (four octets, each 0..255). Pure. */
int remote_server_valid(const char *s);

#endif
