// SPDX-License-Identifier: GPL-3.0
// uimodel.h --- ONE immutable frame: everything the renderer is allowed to see
// Copyright 2026 Jakob Kastelic
//
/* THE FUNCTIONAL CORE'S INPUT, and the reason the UI can be tested at all.
 *
 * The UI is a pure function of an immutable snapshot: the shell fills a
 * `struct screen` each frame (model.c), ui_render draws exactly that and
 * nothing else -- no globals, no callbacks -- and a tap is mapped back through
 * the hit table (uiact.h). So the whole UI is driven offline from app/test/:
 * feed a model, get a PPM; feed a tap, get an action.
 *
 * WHY A COPY AND NOT A VIEW. The registry and the driver sessions are mutated
 * from BINDER threads, and the lock order is driver -> registry -> history. A
 * renderer reading them live would either draw a half-shifted row -- one
 * sensor's session age on another's line -- or have to take the registry lock
 * inside the history lock and invert that order. See model.h.
 *
 * Split out of ui.h with the actions and the primitives; ui.h includes all
 * three, so no call site had to change.
 */
#ifndef PANCRA_UIMODEL_H
#define PANCRA_UIMODEL_H

#include "clock.h"
#include "insulin.h"  /* struct ins_rec: the INSULIN LOG table rows */
#include "keypad.h"   /* enum keypad_mode: what the keypad collects */
#include "sensors.h"  /* sensor types/kinds the model and renderer share */
#include "settings.h" /* SC_MAX: the main-screen PIN slots */
#include "uifmt.h"    /* UI_MAX_SLOTS and the presentation constants */
#include "food.h"     /* struct food_type: the FOOD TYPE picker rows */
#include "weight.h"   /* struct wt_rec: the WEIGHT LOG table rows */
#include <stdint.h>

/* ================= functional-core UI =================================
 * The UI is a pure function of an immutable snapshot: the shell (main.c) fills
 * a `struct screen` each frame, ui_render() draws it and records the touch
 * targets into `struct hits`, and ui_hit() maps a later tap to the action the
 * shell should perform. No globals, no callbacks -- so the whole UI is driven
 * and checked offline from test/ (feed a model -> PNG; feed a tap -> action).
 */

enum ui_screen {
   SCR_MAIN,
   SCR_SETTINGS,
   SCR_KEYPAD,
   SCR_DEVLIST,
   SCR_GATE,
   SCR_SENSOR,     /* one sensor: attributes above, actions below */
   SCR_CAL,        /* that sensor's calibration panel */
   SCR_CALPEND,    /* a calibration is already queued: REPLACE / CANCEL */
   SCR_RESCALE,    /* confirm a rescale value */
   SCR_RESCALEACT, /* rescaling active: CHANGE / STOP */
   SCR_SENSTYPE,   /* pick a sensor type when adding */
   SCR_FORGET,     /* confirm forgetting a sensor */
   SCR_LABEL,      /* rename a sensor (letter keypad) */
   SCR_MARKPICK,   /* marker-shape picker */
   SCR_COLORPICK,  /* colour picker */
   SCR_METERHELP,  /* OneTouch: how-to-connect + Scan button */
   SCR_PAIRCONF,   /* confirm pairing the picked device: YES / NO */
   /* confirm pulling the server's record back down onto this phone */
   SCR_SYNCRESTORE,
   SCR_ADDMENU, /* main-screen '+': ADD ... (NEW DEVICE / INSULIN) */
   SCR_INSULIN, /* LOG INSULIN entry form */
   SCR_DEVICES, /* the device registry: active, old, and ADD NEW DEVICE */
   SCR_PERMS,   /* permissions + background controls, moved off SETTINGS */
   SCR_OLDDEV,  /* previously-used (forgotten) devices: restyle their trace */
   SCR_RECONF,  /* confirm reconnecting an EXPIRED old device */
   SCR_REMOTE,  /* remote push: enable/disable, server IP and port */
   SCR_INSLOG,  /* insulin dose log: paginated when/type/units table */
   SCR_WEIGHT,  /* LOG WEIGHT entry form */
   SCR_WTLOG,   /* weight log: table + trend plot */
   SCR_WTDEL,   /* confirm deleting a weight entry */
   SCR_DISPLAY, /* display settings submenu (off SETTINGS) */
   SCR_INSDEL,  /* confirm deleting an insulin dose: DELETE / CANCEL */
   SCR_ALARM,   /* alarm submenu: LOW/HIGH thresholds + outputs */
   SCR_EXPORT,  /* EXPORT DATA: range + section checkboxes + the button */
   SCR_FOOD,    /* LOG FOOD entry form: type, grams, time, date, year */
   SCR_FOODTYPE, /* pick a food from the vocabulary, or add a new one */
   SCR_FOODLOG,  /* the food entries, paginated, newest first */
   SCR_N
};

/* Resolved calibration kinds, for the LAST CAL row (ui_sensor.cal_state). */
enum {
   CAL_ST_NONE = 0,
   CAL_ST_APPLIED,  /* accepted: cal_mgdl is the value */
   CAL_ST_REJECTED, /* the sensor rejected this VALUE (0x34 result) */
   CAL_ST_NOTSUP,   /* the sensor does not permit calibration (0x32) */
   CAL_ST_FAILED    /* never accepted within the retry window */
};

struct ui_point {
   long t;
   int glu;
   int src;  /* sensor id, for marker/colour lookup */
   int kind; /* KIND_CGM plots as a line vertex, KIND_BGM as a marker */
}; /* one plotted reading */

/* One configured sensor: everything the list row AND the detail screen need,
 * so the shell hands over a single self-contained snapshot per sensor. */
struct ui_sensor {
   /* Longs first, then ints, then char arrays -- packed to avoid alignment
    * padding (clang-analyzer-optin.performance.Padding). */
   long last;            /* last reading (CGM) or sync (BGM); 0 = never */
   long session_seconds; /* CGM session length */
   long wear_len;        /* nominal wear budget, seconds (0 for a meter) */
   long paired;          /* when this device was registered (warmup display) */
   long rssi_t;       /* wall-clock of the RSSI sample, for its "N M AGO" age */
   long meter_sync_t; /* meter only: when the app last synced it (vs last
                         datapoint) */
   long cal_t;        /* when the last calibration RESOLVED; 0 = never */
   long activation;   /* provenance session start (epoch); for an OLD device,
                       * STARTED/ENDS come from this, not the live clock */
   int sess_state;    /* SENSOR_STATE_* from its link's last 4e; 0 unknown */
   int id, type, kind;
   int color, marker, primary, size;
   int old; /* 1 = DISCONNECTED: shown under OLD DEVICES, state EXPIRED, but
             * the SAME full per-device menu; excluded from the live list */
   /* 1 = wear_len was RESOLVED from the model/type, 0 = the user pinned it.
    *
    * The WEAR row must show which, because the two behave differently over
    * time: a resolved length improves when a new model is recognised, a
    * pinned one never does. A G7 paired before the SW14758 (15 Day) model was
    * known, then pinned to 10 by one tap of what used to be a two-state
    * toggle, went on reporting a 10-day budget for a 15-day sensor with
    * nothing on screen to say the model rule had been overruled. */
   int wear_auto;
   int glu, trend, predicted, sequence;
   int rssi, rssi_ok, connected;
   /* OS bond state, in the framework's own constants: 0 = never heard,
    * 10 = NONE, 11 = BONDING, 12 = BONDED (dexble_bond_state).
    *
    * Distinct from `connected` and from the Dexcom app-layer auth: a device
    * can be registered, reachable and still unbonded because the user never
    * answered the system pairing dialog. That state was previously invisible
    * -- the row just never came alive and nothing said why -- which is the
    * whole reason the bond-state receiver exists. */
   int bond;
   /* Calibration state for this CGM's LAST CAL row. cal_pending!=0 means a
    * calibration is queued and not yet accepted (cal_pending is its mg/dL);
    * otherwise cal_t>0 gives the last RESOLVED calibration, whose kind is
    * cal_state (CAL_ST_*) and value cal_mgdl. */
   int cal_pending;
   int cal_mgdl;
   int cal_state;        /* CAL_ST_* */
   int rescale_pm;       /* active rescale factor (permille); 1000 = none */
   int rescale_pending;  /* target mg/dL awaiting a reading; 0 = none */
   int rescale_rejected; /* last attempt exceeded +-25% and was rejected */
   int rescale_expired;  /* a pending target timed out awaiting a reading */
   /* THE ROW IS AHEAD OF THE DISK. An automatic transition that could not be
    * written stands (there is no second sample to recompute it from), so the
    * row says NOT SAVED until calib_tick's retry lands. See calib.h. */
   int cal_unsaved;
   int rescale_unsaved;
   /* label must hold a full sensor_slot.label (sensors.h) -- at 12 it truncated
    * the default meter name "ONETOUCH-AB:CD" to "ONETOUCH-AB", cutting off
    * exactly the MAC tail that tells two meters apart. */
   char label[20];
   char status[12];
   char mac[20], model[24], fw[24], serial[24], code[8];
};

struct ui_dev {
   char name[12];
   char mac[20];
   int rssi;
}; /* a scanned sensor */

struct ui_stat {
   int have, tir, avg;
}; /* one rolling-window stat column */

/* THE FRAME MODEL, IN COHESIVE PIECES.
 *
 * `struct screen` was one flat run of about ninety fields. Everything a frame
 * could possibly need was in scope for every renderer, so nothing said which
 * fields the weight table actually reads, or that the keypad renderer has no
 * business with the sensor registry -- and a field added for one screen was
 * indistinguishable from a field the next screen depends on.
 *
 * The pieces below are the answer. Each is a whole, self-contained thing a
 * renderer can be handed just its own part of the model (the weight log's
 * renderer takes `&m->wt`, rather
 * than the entire model), and each is IMMUTABLE for the life of a render call:
 * the shell fills them, the UI only reads them.
 *
 * Field names are unchanged, deliberately. Grouping and renaming at once would
 * have made every one of the ~800 call sites a place to introduce a silent
 * mistake -- and a silent mistake here is a number drawn from the wrong field,
 * which looks like a plausible reading. The grouping is the change; the names
 * stay so the move can be checked mechanically (and was: uitest renders
 * fourteen screens to PPM, and every one is byte-identical across it).
 */

/* The current reading and what the sensor says about itself. */
struct ui_reading {
   long t;               /* reading time */
   long session_seconds; /* current session length from the driver */
   /* current reading (glu < 0 => no reading yet) */
   int glu, trend, rssi, rssi_ok, stale;
   int disc_alarmed; /* stale-data alarm latched (drives the STALE banner) */
   /* session facts from the driver (sess_state: the PRIMARY's SENSOR_STATE_*
    * from its last 4e; the sensor's own verdict outranks arithmetic) */
   int bonded, paired, have_reading, predicted, sequence, sess_state;
   /* 1 when at least one CGM is registered. The STATE/SESSION/PRED block
    * describes a CGM, so with none it must blank to dashes -- never inherit
    * the global status line, which a meter sync leaves reading "SYNCED". */
   int has_cgm;
};

/* The plot and the rolling statistics under it. */
struct ui_plotview {
   const struct ui_point *hist; /* plot history, newest-first (borrowed) */
   /* plot: point count, scrub cursor (-1 = none), span, vertical max */
   int nhist, scrub, plot_hours, plot_max;
   struct ui_stat stat[5]; /* rolling stats: 1d / 3d / 7d / 30d / 90d */
};

/* Stored preferences, as VALUES -- never the settings module's globals, so a
 * frame renders the same way whatever happens to them mid-render. */
struct ui_prefs {
   int units, alarm_low, alarm_high, sound_on, vib_on, orient, disc;
   int nudge_low, nudge_high;  /* the one-time heads-up band (alarmlogic.h) */
   int nudge_sound, nudge_vib; /* the nudge's OWN outputs, not the alarm's */
   int wunits;                 /* weight display unit: WT_KG / WT_LB */
   int screen_on; /* 1 = hold the screen awake while open, 0 = follow the OS */
   int newdata_mode; /* ND_OFF / ND_BEEP / ND_CHIRP: what a new primary-CGM
                      * datapoint sounds like */
   /* status bar shows the value (vs icon); lock screen shows the notif */
   int statbar_val, lockscr_val;
   /* The MA_* codes promoted to the main screen's '+' row, dense, 0 = empty.
    * Mirrors g_shortcut (settings.h). */
   int shortcut[SC_MAX];
};

/* THE TWO SHARED EDITORS: the numeric keypad and the text field. Neither owns
 * a meaning -- kp_mode / label_field say who asked for it. */
struct ui_entry {
   const char *entry; /* keypad digits typed so far */
   /* Which field the text editor is editing: 0 sensor name, 1 server,
    * 2 account. The editor is shared, and a title that always said NAME made
    * the server and account screens look like the wrong screen. */
   int label_field;
   /* WHICH VALUE the keypad is collecting. The TYPE, not an int: keypad.h
    * includes nothing at all, so carrying the enum across this boundary
    * costs the renderer no dependency and buys a compiler that checks it.
    * What each mode draws -- title, digit slots, unit suffix, decimal point
    * -- is that table's business, not this comment's: it used to say "0 =
    * pairing code, 1 = plot-max entry" and stopped there, while sixteen
    * modes existed. */
   enum keypad_mode kp_mode;
   /* Why the last entry was refused, or empty. The keypad used to answer a
    * bad value by clearing the field and saying nothing -- "the cleared entry
    * is the feedback" -- which is indistinguishable from a mistyped key, and
    * is exactly how a nudge threshold that was never accepted looked like a
    * broken nudge for weeks. A refusal that does not say why is a refusal the
    * user will repeat. */
   char kp_err[40];
};

/* The sensor registry and the pairing flow over it. */
struct ui_devview {
   const struct ui_dev *devs;       /* scanned sensors (borrowed) */
   const struct ui_sensor *sensors; /* configured sensors (borrowed) */
   const char *mac, *model, *fw, *mfr, *code; /* device-info strings */
   /* Type being added (ADD SENSOR flow), for the PAIR / SELECT titles. */
   const char *add_type; /* display name, e.g. "STELO", "G7" */
   /* The picked device awaiting the SCR_PAIRCONF yes/no (borrowed). */
   const char *pair_name, *pair_mac;
   /* sensor registry: the list in settings, and which one a detail screen is
    * showing (sel indexes `sensors`; -1 when no detail screen is open) */
   int nsensors, sel;
   int stored, ndev;
   int add_kind; /* KIND_CGM / KIND_BGM of that type */
   /* An ARMED pairing awaiting its sensor: the SENSOR_* type, 0 = none. The
    * DEVICES screen shows it as a tappable PENDING row (MA_PEND_CANCEL). */
   int pend_type;
   int old_page; /* OLD DEVICES: which page of the list is showing */
   int dev_page; /* DEVICES: which page of the LIVE device list is showing */
   unsigned adv_total;
};

/* The two ways a person overrides the number: calibration and rescale. */
struct ui_calview {
   /* last 0x32/0x34 answers for the selected sensor */
   int cal_have, cal_permitted, cal_status, cal_last_bg, cal_result;
   int cal_pending; /* value awaiting CONFIRM, mg/dL; 0 = none */
   /* RESCALE screens: the value being confirmed (mg/dL), and the factor to show
    * (permille) -- the clamped preview on SCR_RESCALE, the active one on
    * SCR_RESCALEACT. */
   int rescale_entry;
   int rescale_pm;
};

/* The dose form, the dose log, and how doses are drawn on the plot. */
struct ui_insview {
   /* LOG INSULIN form state (ins_t rendered via tz_off). ins_edit means
    * the form edits an existing dose (title EDIT..., red DELETE button). */
   long ins_t;
   /* INSULIN LOG table (borrowed; oldest first, like insulin.h's tail) */
   const struct ins_rec *ins_log;
   int ins_type, ins_units, ins_edit;
   int ins_nlog, inslog_page;
   /* insulin plot styling per type (index INS_SLOW / INS_FAST), and which
    * insulin type the marker picker is editing (-1 = a sensor's) */
   int ins_marker[2], ins_color[2], ins_size[2], markpick_ins;
};

/* WEIGHT log table (SCR_WTLOG) and entry form (SCR_WEIGHT). `wt` is the
 * shell's tail, oldest first; wt_page is the table's page. The form's value is
 * in TENTHS of the display unit, the shape the keypad accepts. */
struct ui_wtview {
   const struct wt_rec *wt;
   /* The entry EDIT WEIGHT is editing, as it was on disk. The delete
    * confirmation must name THIS, not the form's current (possibly edited)
    * value -- it is what weight_delete will actually remove. */
   long wt_orig_t, wt_orig_g;
   long wt_t;
   int nwt, wt_page;
   int wt_edit;  /* 1 = the form is EDITING an entry, not logging a new one */
   int wt_tab;   /* index into ui_wt_days: the plot's span */
   int wt_scrub; /* index into wt of the scrubbed point, -1 = none */
   int wt_tenths;
};

/* FOOD: the vocabulary the picker shows (SCR_FOODTYPE) and the entry form
 * (SCR_FOOD). `types` is a frame-owned snapshot, in the order they were added.
 *
 * `food_type` is the id the form currently holds, NOT an index into `types`:
 * adding a food from the picker grows that table, and an index would then name
 * a different food than the one that was chosen. The id is stable for the life
 * of the vocabulary, which is exactly the property a form needs to hold on to
 * across a trip through the picker. */
struct ui_foodview {
   const struct food_type *types;
   int ntypes, type_page;
   long food_t;   /* the entry instant the form holds */
   int food_type; /* the chosen type id; FOOD_TYPE_NONE = nothing chosen yet */
   int food_g;    /* grams */
   int food_edit; /* 1 = editing an existing entry, not logging a new one */
   /* The EXERCISE button: what it shows, and how much of the settling period
    * is left. `ex_remaining` is 0 when nothing is pending, which is also what
    * makes the progress bar disappear. */
   int ex_level, ex_remaining;
   /* THE FOOD LOG table (SCR_FOODLOG). A frame-owned snapshot, newest last
    * like the tail it comes from; the table renders it newest FIRST. */
   const struct food_rec *log;
   int nlog, log_page;
};

/* Cloud sync: the account, the server, and what the last attempt got back. */
struct ui_syncview {
   long remote_last_ok;       /* last server-acknowledged push; 0 = never */
   const char *remote_server; /* sync server, name or IP; "" = not set */
   const char *sync_email;    /* the account being paired to; "" = not set */
   /* HOW THE LAST ATTEMPT ENDED: an enum sync_outcome (syncstat.h) and the
    * label derived from it. SYNC_IDLE = nothing attempted yet. Failures were
    * once invisible outside logcat; then they were English strings the
    * renderer pattern-matched, and two of them matched nothing. The screen
    * colours by sync_outcome_severity(), never by reading the words. */
   int remote_outcome;
   const char *remote_status;
   int sync_paired;            /* 1 once an app identity is stored */
   int sync_active;            /* 1 while a sync is in flight */
   int sync_permille;          /* 0..1000, ALREADY SMOOTHED by main.c so the
                                * renderer stays a pure function of this struct
                                * (uitest renders it deterministically) */
   int remote_on, remote_port; /* remote push enabled; server TCP port */
};

/* What Android is currently allowing us to do, plus the export menu. */
struct ui_sysview {
   /* system snapshot for the settings screen (perm[]: BT scan/connect/notify)
    */
   int perm[3], batt_ok, bg_restricted, standby_bucket;
   /* EXPORT DATA menu state: range 0 = 30 D, 1 = 1 Y, 2 = ALL; the three
    * section checkboxes (1 = included) */
   int exp_range, exp_glu, exp_dev, exp_ins, exp_wt;
};

/* Everything a frame needs to draw itself. Built fresh by the shell; the UI
 * only reads it. Pointers are borrowed for the duration of the render call. */
struct screen {
   /* The four things that belong to the FRAME rather than to any one part of
    * it: which screen, when it is, the zone to render times in, and the one
    * line of status across the top. */
   enum ui_screen scr;
   long now;           /* realtime_s() at frame time */
   long tz_off;        /* local timezone offset (seconds) */
   const char *status; /* top status text */

   struct ui_reading reading;
   struct ui_plotview plot;
   struct ui_prefs prefs;
   struct ui_entry entry;
   struct ui_devview dev;
   struct ui_calview cal;
   struct ui_insview ins;
   struct ui_wtview wt;
   struct ui_foodview food;
   struct ui_syncview sync;
   struct ui_sysview sys;
};

/* ---- THE PLOT'S GEOMETRY, which is the model's, not the reader's -------
 *
 * These sat in plotdata.h, which is the module that READS the log and fills
 * points in. But what they size is the frame's array, and what they count is
 * this file's struct ui_point -- so plotdata.h had to declare `struct
 * ui_point;` and refuse to include this header, with a comment explaining
 * that including it back "would be circular". It was. The vocabulary belongs
 * with the type it describes; plotdata.h includes this one now, one way. */

/* Spans up to this are drawn from the live RAM window instead. */
#define PLONG_MIN (24L * 3600)

/* The MOST points a long span can return. Every consumer must size its
 * arrays for this: copying the result into an NHIST-sized buffer truncates
 * it, and because the log is in arrival order the survivors are all recent,
 * so the old half of a 30-day plot vanishes -- the same truncation bug one
 * level up from where it was fixed. */
#define PLOT_COLS 768 /* x columns we ever draw into */
/* 64: a 56-minute column holds roughly two dozen readings with two CGMs and
 * a meter, so this is headroom rather than a guess -- and plottest asserts
 * that no distinguishable reading is dropped, which is exactly what fails if
 * it is ever too small. It was 24, and dropped points wherever two sensors
 * overlapped. */
#define PLOT_PERCOL   64 /* distinct values kept per column */
#define PLOT_LONG_MAX (PLOT_COLS * PLOT_PERCOL)

/* Points one frame can carry: the long-span glucose ceiling plus every dose
 * and weight, because all three ride in ONE array -- plot_render and plot_hit
 * take points in any order, and the scrub index the UI hands back has to
 * index a single list. */
#define UI_PTS_MAX (PLOT_LONG_MAX + NINS + NWT)

#endif
