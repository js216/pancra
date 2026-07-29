// SPDX-License-Identifier: GPL-3.0
// ui.h --- On-screen rendering + touch input (the interactive UI layer)
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_UI_H
#define PANCRA_UI_H

#include "insulin.h" /* struct ins_rec: the INSULIN LOG table rows */
#include "ndk.h"
#include "plotdata.h" /* PLOT_LONG_MAX: how many points a long span yields */
#include "sensors.h"  /* sensor types/kinds the model and renderer share */
#include <stdint.h>

/* ---- pixel/text primitives (font-based, no shared state) ----
 * These render directly into a locked framebuffer, so the offline harness can
 * drive them with a plain malloc'd buffer (see test/uitest.c). */

/* Draw string s at (ox,oy), cell scale sc, in ARGB color. Clipped to buf. */
void draw_str(uint32_t *px, const struct ANativeWindow_Buffer *buf, int ox,
              int oy, int sc, const char *s, uint32_t color);

/* Draw a 1px rectangle outline (x,y,w,h) in ARGB color; no-op if off-buffer. */
void draw_frame(uint32_t *px, const struct ANativeWindow_Buffer *buf, int x,
                int y, int w, int h, uint32_t c);

/* Format glucose for display; units: 0 = mg/dL, 1 = mmol/L. Pure. */
void fmt_glu(int mgdl, int units, char *out, int n);

/* Format a trend (tenths of mg/dL per minute; 127 = unknown -> "--"). Pure. */
void fmt_trend(int tr, char *out, int n);

/* Format epoch seconds as HH:MM:SS local time (tz = offset seconds). Pure. */
void fmt_hms(long epoch, long tz, char *out, int n);

/* Format epoch seconds as "MM-DD HH:MM" local time. Pure. */
void fmt_date(long epoch, long tz, char *out, int n);

/* Format how long ago `then` was, coarsely ("234 S" / "2 H" / "6 D"), or
 * "NEVER" when then <= 0. Pure. */
void fmt_ago(long now, long then, char *out, int n);

/* Format a duration as "3 D 4 H" / "5 H 20 M" / "45 M". Pure. */
void fmt_dur(long seconds, char *out, int n);

/* ---- sensor presentation (shared by the list, the detail screen, the plot) */
#define UI_MAX_SLOTS MAX_SLOTS
/* Refuse to render a sensor list shorter than this rather than silently
 * truncating it, so a cramped screen is a visible error, not a quiet lie. */
#define UI_MIN_SLOTS 3

/* Rows the settings screen consumes ABOVE (and below) the sensor entries:
 * title (2), the four submenu rows -- DISPLAY ... / ALARM / PERMISSIONS /
 * REMOTE -- each with a blank line after it (8), the DEVICES header (1),
 * and the two trailing framed buttons -- ADD NEW DEVICE and EXPORT DATA --
 * each a separator + a ~1-row button (4 total). Keep in step with
 * render_settings.
 *
 * Exported deliberately. It used to be private to ui.c while test/uitest.c
 * carried its own literal for the same quantity, so the two could drift
 * apart silently -- and adding rows to render_settings without bumping this is
 * exactly the mistake that leaves sensor rows and their tap targets below the
 * bottom of the screen, permanently unreachable because there is no
 * scrolling. One definition, both users. */
#define UI_SET_ABOVE 16

/* How many sensor rows fit in the settings screen at this geometry, given
 * everything above the SENSORS section. The whole UI never scrolls, so this is
 * what bounds the list. Pure, and exposed so the shell can log a warning. */
/* Layout scale for the settings screen: bounded by BOTH width and height, so
 * the sensor list and ADD row stay on screen on 16:9 phones too. */
int ui_settings_scale(int w, int h);
/* Largest scale at which `rows` rows fit in height h (also bounded by width).
 * Every full-screen menu must size itself through this. */
int ui_fit_scale(int w, int h, int rows);

/* Glyph-cell clipping counter, for the offline harness. Clipping is otherwise
 * invisible: content laid out past an edge is silently dropped. Reset before a
 * render, read after; a non-zero count means text did not fit. */
void ui_clip_reset(void);
long ui_clipped(void);
int ui_sensor_capacity(int w, int h);

/* The characters the rename keypad offers, in grid order. Exposed so the shell
 * can map an MA_CHAR code back to the character that was tapped. */
extern const char ui_label_chars[];
int ui_label_nchars(void);

const char *ui_marker_name(int marker);
const char *ui_color_name(int color);
uint32_t ui_sensor_color(int color);

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
   SCR_ADDMENU,    /* main-screen '+': ADD ... (NEW DEVICE / INSULIN) */
   SCR_INSULIN,    /* LOG INSULIN entry form */
   SCR_PRIMPICK,   /* big-number tap with >1 registered CGM: choose primary */
   SCR_PERMS,      /* permissions + background controls, moved off SETTINGS */
   SCR_OLDDEV,  /* previously-used (forgotten) devices: restyle their trace */
   SCR_RECONF,  /* confirm reconnecting an EXPIRED old device */
   SCR_REMOTE,  /* remote push: enable/disable, server IP and port */
   SCR_INSLOG,  /* insulin dose log: paginated when/type/units table */
   SCR_DISPLAY, /* display settings submenu (off SETTINGS) */
   SCR_INSDEL,  /* confirm deleting an insulin dose: DELETE / CANCEL */
   SCR_ALARM,   /* alarm submenu: LOW/HIGH thresholds + outputs */
   SCR_EXPORT,  /* EXPORT DATA: range + section checkboxes + the button */
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
   int glu, trend, predicted, sequence;
   int rssi, rssi_ok, connected;
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

/* Everything a frame needs to draw itself. Built fresh by the shell; the UI
 * only reads it. Pointers are borrowed for the duration of the render call.
 * Fields are grouped widest-first (8-byte, then int, then the stats array) so
 * the struct packs without padding holes; the comments mark the logical sets.
 */
struct screen {
   /* --- 8-byte members (times, then borrowed pointers) --- */
   long now;             /* realtime_s() at frame time */
   long t, tz_off;       /* reading time; local timezone offset (seconds) */
   long session_seconds; /* current session length from the driver */
   long remote_last_ok;  /* last server-acknowledged push; 0 = never */
   const struct ui_point *hist;     /* plot history, newest-first (borrowed) */
   const struct ui_dev *devs;       /* scanned sensors (borrowed) */
   const struct ui_sensor *sensors; /* configured sensors (borrowed) */
   const char *status;              /* top status text */
   const char *mac, *model, *fw, *mfr, *code; /* device-info strings */
   const char *entry;                         /* keypad digits typed so far */
   const char *remote_ip; /* remote-push server address; "" = not set */
   /* What the last sync attempt actually got back ("200 STORED 3 OF 3",
    * "409 TOO FAR BEHIND", "TIMEOUT"...). Empty = nothing attempted yet.
    * Failures were previously invisible outside logcat. */
   const char *remote_status;

   /* --- int members --- */
   enum ui_screen scr;
   /* current reading (glu < 0 => no reading yet) */
   int glu, trend, rssi, rssi_ok, stale;
   int disc_alarmed; /* stale-data alarm latched (drives the STALE banner) */
   /* plot: point count, scrub cursor (-1 = none), span, vertical max */
   int nhist, scrub, plot_hours, plot_max;
   /* session facts from the driver (sess_state: the PRIMARY's SENSOR_STATE_*
    * from its last 4e; the sensor's own verdict outranks arithmetic) */
   int bonded, paired, have_reading, predicted, sequence, sess_state;
   /* 1 when at least one CGM is registered. The STATE/SESSION/PRED block
    * describes a CGM, so with none it must blank to dashes -- never inherit
    * the global status line, which a meter sync leaves reading "SYNCED". */
   int has_cgm;
   /* settings (values, not the module's globals) */
   int units, alarm_low, alarm_high, sound_on, vib_on, orient, disc;
   int screen_on; /* 1 = hold the screen awake while open, 0 = follow the OS */
   int newdata_beep;           /* 1 = beep on each new primary-CGM datapoint */
   int remote_on, remote_port; /* remote push enabled; server TCP port */
   /* sensor registry: the list in settings, and which one a detail screen is
    * showing (sel indexes `sensors`; -1 when no detail screen is open) */
   int nsensors, sel;
   /* last 0x32/0x34 answers for the selected sensor */
   int cal_have, cal_permitted, cal_status, cal_last_bg, cal_result;
   int cal_pending; /* value awaiting CONFIRM, mg/dL; 0 = none */
   /* RESCALE screens: the value being confirmed (mg/dL), and the factor to show
    * (permille) -- the clamped preview on SCR_RESCALE, the active one on
    * SCR_RESCALEACT. */
   int rescale_entry;
   int rescale_pm;
   /* modal/UX state */
   int stored, ndev;
   int kp_mode; /* keypad: 0 = pairing code, 1 = plot-max entry */
   /* Type being added (ADD SENSOR flow), for the PAIR / SELECT titles. */
   const char *add_type; /* display name, e.g. "STELO", "G7" */
   int add_kind;         /* KIND_CGM / KIND_BGM of that type */
   /* The picked device awaiting the SCR_PAIRCONF yes/no (borrowed). */
   const char *pair_name, *pair_mac;
   /* LOG INSULIN form state (ins_t rendered via tz_off). ins_edit means
    * the form edits an existing dose (title EDIT..., red DELETE button). */
   long ins_t;
   int ins_type, ins_units, ins_edit;
   /* INSULIN LOG table (borrowed; oldest first, like insulin.h's tail) */
   const struct ins_rec *ins_log;
   int ins_nlog, inslog_page;
   /* insulin plot styling per type (index INS_SLOW / INS_FAST), and which
    * insulin type the marker picker is editing (-1 = a sensor's) */
   int ins_marker[2], ins_color[2], ins_size[2], markpick_ins;
   /* status bar shows the value (vs icon); lock screen shows the notif */
   int statbar_val, lockscr_val;
   /* EXPORT DATA menu state: range 0 = 30 D, 1 = 1 Y, 2 = ALL; the three
    * section checkboxes (1 = included) */
   int exp_range, exp_glu, exp_dev, exp_ins;
   /* An ARMED pairing awaiting its sensor: the SENSOR_* type, 0 = none.
    * DEVICES shows it as a tappable PENDING row (MA_PEND_CANCEL); the
    * choose-primary screen offers it too (MA_PRIM_PEND), and pend_primary
    * marks that it will take the big number when it lands. */
   int pend_type, pend_primary;
   int old_page; /* OLD DEVICES: which page of the list is showing */
   unsigned adv_total;
   /* system snapshot for the settings screen (perm[]: BT scan/connect/notify)
    */
   int perm[3], batt_ok, bg_restricted, standby_bucket;

   /* rolling stats: 1d / 3d / 7d / 30d / 90d */
   struct ui_stat stat[5];
};

/* One touch target and the action code the shell acts on. `arg` carries a tab
 * index / device index / +-1 delta as needed. */
struct action {
   int kind, arg;
};

enum {
   ACT_NONE = 0,
   ACT_OPEN_SETTINGS,
   ACT_PLOT_TAB,      /* arg = plot span in hours */
   ACT_SCRUB,         /* a press inside the plot; shell resolves the point */
   ACT_GATE_CONTINUE, /* first-run rationale: request permissions */
   ACT_PICK_PRIMARY,  /* big-number tap: choose the primary (shell decides
                         whether >1 active CGM makes it worth asking) */
   /* Modal screens (settings / keypad / device list) speak the shell's
    * menu_action protocol; arg carries that integer code so menu_action stays
    * the single dispatch point for their JNI/pairing side effects. */
   ACT_MENU,
};

/* The integer codes ACT_MENU carries. These were bare literals shared by hand
 * between the renderer and the shell's menu_action(); naming them keeps the two
 * ends honest now that there are enough to be easy to collide. Ranges (MA_PERM,
 * MA_SENSOR, MA_DIGIT, MA_TYPE, MA_DEV_PICK) are bases with an index added, so
 * leave the gaps after them alone. Values are historical -- do not renumber. */
enum ui_menu {
   MA_ORIENT    = 0,
   MA_SOUND     = 1,
   MA_VIB       = 2,
   MA_UNITS     = 3,
   MA_DISC      = 4,
   MA_SCREEN    = 5,
   MA_NEWDATA   = 6,  /* toggle the new-datapoint beep */
   MA_METERSCAN = 7,  /* start scanning from the OneTouch instructions screen */
   MA_INS_FAST  = 21, /* ADD menu: LOG FAST INSULIN (type preset) */
   MA_INS_SLOW  = 23, /* ADD menu: LOG SLOW INSULIN (type preset) */
   MA_INSLOG_OPEN  = 25,  /* ADD menu: open the INSULIN LOG table */
   MA_INSLOG_BACK  = 26,  /* insulin log: back to the ADD menu */
   MA_INSLOG_PREV  = 27,  /* insulin log: previous page */
   MA_INSLOG_NEXT  = 28,  /* insulin log: next page */
   MA_INSMARK_OPEN = 114, /* + INS_SLOW/INS_FAST: pick that type's marker */
   MA_INS_DELETE   = 116, /* EDIT INSULIN: delete this dose (red) */
   MA_INSMARK_BACK = 117, /* insulin marker picker: back to DISPLAY */
   MA_DISPLAY_OPEN = 118, /* settings: open the DISPLAY submenu */
   MA_DISPLAY_BACK = 119, /* display submenu: back to settings */
   MA_STATBAR      = 120, /* toggle status bar: value vs app icon */
   MA_NOTIF_REOPEN = 121, /* re-post the (swiped-away) notification */
   MA_LOCKSCR      = 122, /* toggle lock-screen notification visibility */
   MA_INSDEL_YES   = 123, /* delete-dose confirmation: really delete */
   MA_INSDEL_NO    = 124, /* delete-dose confirmation: back to the form */
   MA_ALARM_LOW    = 125, /* LOW <value> (main row / ALARM menu): keypad 10 */
   MA_ALARM_HIGH   = 126, /* HIGH <value> (main row / ALARM menu): keypad 11 */
   MA_ALARM_OPEN   = 127, /* open the ALARM submenu (settings row, or the
                           * main alarm row LEFT of "LOW") */
   MA_ALARM_BACK = 128,   /* ALARM submenu: back to where it was opened */
   /* EXPORT DATA menu: parked in the free band ABOVE MA_PRIM_PICK's range
    * end (260 + MAX_SLOTS = 270) and below MA_CHAR (300); the existing
    * MA_PRIM_PICK assert covers the lower neighbour. */
   MA_EXP_RANGE     = 271, /* cycle the range: 30 D / 1 Y / ALL */
   MA_EXP_GLU       = 272, /* toggle the GLUCOSE (readings) section */
   MA_EXP_DEV       = 273, /* toggle the DEVICES (sensors) section */
   MA_EXP_INS       = 274, /* toggle the INSULIN (doses) section */
   MA_EXP_GO        = 275, /* build the CSV and open the share sheet */
   MA_EXP_BACK      = 276, /* back to settings, nothing exported */
   MA_PERM          = 10,  /* + permission index (0..2) */
   MA_BATTERY       = 20,
   MA_BGEXEC        = 22,
   MA_PAIR_CODE     = 30,
   MA_PLOTMAX       = 31,
   MA_REMOTE_OPEN   = 32, /* settings: open the REMOTE submenu */
   MA_REMOTE_TOGGLE = 33, /* remote menu: enable/disable the push */
   MA_REMOTE_IP     = 34, /* remote menu: edit the server IP (keypad) */
   MA_REMOTE_PORT   = 35, /* remote menu: edit the server port (keypad) */
   MA_REMOTE_BACK   = 36, /* remote menu: back to settings */
   /* Rebased from 40 when MAX_SLOTS grew to 10: the old base had only 8
    * values of room before MA_SENSOR_BACK. These are runtime touch codes,
    * never persisted, so the move is safe; the assert below now guards the
    * new neighbourhood. */
   MA_SENSOR      = 130, /* + slot index; opens that sensor's screen */
   MA_SENSOR_BACK = 48,
   MA_ADDSENSOR   = 49,
   MA_PRIMARY     = 50,
   MA_MARKER      = 51,
   MA_COLOR       = 52,
   MA_LABEL       = 53,
   MA_CAL_OPEN    = 54,
   MA_SYNC        = 55,
   MA_FORGET      = 56, /* opens the confirmation screen; does not act */
   MA_FORGET_YES  = 57,
   MA_FORGET_NO   = 58,
   MA_SIZE        = 59, /* cycle marker size */
   MA_CAL_REFRESH = 60,
   MA_CAL_ENTER   = 61,
   MA_CAL_BACK    = 62,
   MA_CAL_REPLACE = 63, /* pending-cal screen: enter a new value (supersedes) */
   MA_CAL_CANCEL  = 64, /* pending-cal screen: discard the queued calibration */
   MA_RESCALE_OPEN   = 65, /* RESCALE row: keypad, or the active screen */
   MA_RESCALE_ENTER  = 66, /* confirm a rescale value */
   MA_RESCALE_BACK   = 67, /* leave a rescale screen unchanged */
   MA_RESCALE_CHANGE = 68, /* active screen: enter a new value */
   MA_RESCALE_STOP   = 69, /* active screen: turn rescaling off */
   MA_TYPE           = 70, /* + sensor type (SENSOR_STELO..) */
   MA_EXPORT      = 75,  /* settings: EXPORT DATA via the system share sheet */
   MA_PAIR_YES    = 76,  /* pairing confirmation: commit to the picked device */
   MA_PAIR_NO     = 77,  /* pairing confirmation: back to the device list */
   MA_ADD_OPEN    = 78,  /* main-screen '+': open the ADD menu */
   MA_INS_OPEN    = 79,  /* ADD menu: open the LOG INSULIN form */
   MA_INS_TYPE    = 80,  /* LOG INSULIN: toggle SLOW / FAST */
   MA_INS_UMINUS  = 81,  /* LOG INSULIN: units - 1 */
   MA_INS_UPLUS   = 82,  /* LOG INSULIN: units + 1 */
   MA_INS_DMINUS  = 83,  /* LOG INSULIN: date - 1 day */
   MA_INS_DPLUS   = 84,  /* LOG INSULIN: date + 1 day */
   MA_INS_TMINUS  = 85,  /* LOG INSULIN: time - 5 min */
   MA_INS_TPLUS   = 86,  /* LOG INSULIN: time + 5 min */
   MA_INS_CONFIRM = 87,  /* LOG INSULIN: append the dose */
   MA_INS_DISCARD = 88,  /* LOG INSULIN: leave without logging */
   MA_WEAR        = 89,  /* device screen: toggle wear length 10 D / 15 D */
   MA_PEND_CANCEL = 90,  /* DEVICES: cancel the armed (pending) pairing */
   MA_PERMS_OPEN  = 91,  /* settings: open the PERMISSIONS submenu */
   MA_PERMS_BACK  = 92,  /* permissions submenu: back to settings */
   MA_PRIM_PEND   = 93,  /* choose-primary: the PENDING sensor becomes primary
                            the moment its pairing commits */
   MA_OLDDEV_OPEN  = 94, /* settings DEVICES: open the OLD DEVICES list */
   MA_OLDDEV_BACK  = 95, /* OLD DEVICES list: back to settings */
   MA_OLDPAGE_PREV = 97, /* OLD DEVICES: previous page */
   MA_OLDPAGE_NEXT = 98, /* OLD DEVICES: next page */
   MA_RECONNECT    = 96, /* old device: revive it (direct if not yet expired,
                          * else via a confirmation screen) */
   MA_RECON_YES  = 8,    /* reconnect-expired confirmation: do it */
   MA_RECON_NO   = 9,    /* reconnect-expired confirmation: cancel */
   MA_CLOSE      = 99,
   MA_DIGIT      = 100, /* + digit 0..9 */
   MA_OK         = 111, /* keypad / label confirm */
   MA_DOT        = 112, /* keypad '.' (remote-IP entry only) */
   MA_BACKSPACE  = 110,
   MA_KP_CLOSE   = 113,
   MA_DEV_CANCEL = 199,
   MA_DEV_PICK   = 200, /* + scanned-device index */
   MA_MARK_PICK  = 220, /* + marker enum value */
   MA_COLOR_PICK = 240, /* + colour index */
   MA_SIZE_PICK  = 250, /* + size 1..MARK_SIZE_MAX */
   MA_PRIM_PICK  = 260, /* + slot index: make that CGM the primary */
   MA_CHAR       = 300, /* + index into ui_label_chars[] */
   /* LOG INSULIN fields: + 0 units, + 1 date, + 2 time, + 3 year. Tapping
    * the value opens the keypad for exact entry (kp_mode 6/7/8/9). */
   MA_INS_EDIT = 400,
   /* INSULIN LOG rows: + tail index (0..NINS-1) opens that dose in the
    * EDIT INSULIN form. Nothing above this range. */
   MA_INSLOG_EDIT = 500
};

/* THE RANGES MUST NOT OVERLAP, and the compiler is the only thing that will
 * notice if they start to.
 *
 * Five of these codes are a base with a runtime index added, and the bases were
 * chosen by hand with gaps that are not obviously big enough: MA_SENSOR + slot
 * has three spare before MA_SENSOR_BACK, and MA_DIGIT + digit lands EXACTLY on
 * MA_BACKSPACE with none. A collision does not crash or warn -- menu_action
 * simply dispatches the wrong branch, so bumping MAX_SLOTS from 5 to 9 would
 * silently turn "open sensor 8" into "close the sensor screen", and a wrong
 * branch here reaches sensor_forget_slot and the calibration write.
 *
 * This is the same failure the alarm levels had: two numbering spaces sharing
 * one range, agreeing only by luck, with nothing checking. Assert it instead.
 * If one of these fires, MOVE THE LATER BASE UP -- do not renumber history. */
/* MA_PERM and MA_DEV_PICK are asserted in main.c, which owns NPERMS and
 * MAX_DEVS. */
_Static_assert(MA_SENSOR + MAX_SLOTS <= MA_DEV_CANCEL,
               "MA_SENSOR range hits MA_DEV_CANCEL");
_Static_assert(MA_SENSOR > MA_KP_CLOSE, "MA_SENSOR range hits the keypad");
_Static_assert(MA_TYPE + SENSOR_NTYPES <= MA_CLOSE,
               "MA_TYPE range hits MA_CLOSE");
_Static_assert(MA_DIGIT + 10 <= MA_BACKSPACE,
               "MA_DIGIT range hits MA_BACKSPACE");
_Static_assert(MA_PRIM_PICK + MAX_SLOTS <= MA_CHAR,
               "MA_PRIM_PICK range hits MA_CHAR");
_Static_assert(MA_PRIM_PICK + MAX_SLOTS <= MA_EXP_RANGE,
               "MA_PRIM_PICK range hits the EXPORT DATA codes");

/* Up to this many touch targets per frame. */
#define UI_MAX_HITS 48

struct hits {
   struct {
      int x, y, w, h, kind, arg;
      /* Pressed-highlight (glow) rect: what ui_press_overlay lights while
       * this control is armed. add_hit defaults it to the hit rect; a
       * control with a deliberately oversized hit zone (the settings
       * hamburger) narrows it to its visible glyph via add_glow, so
       * pressing it never lights unrelated pixels caught in the zone. */
      int gx, gy, gw, gh;
   } box[UI_MAX_HITS];

   int n;
};

/* Render model `m` into framebuffer `fb`, recording touch targets into `h`. */
void ui_render(struct ANativeWindow_Buffer *fb, const struct screen *m,
               struct hits *h);
/* Map a tap at (x,y) against the targets from the last render. Pure. */
struct action ui_hit(const struct hits *h, int x, int y);
/* As ui_hit, but returns the index of the winning box (-1 = none), so the
 * shell can shade the pressed control's own rectangle. Pure. */
int ui_hit_idx(const struct hits *h, int x, int y);
/* Pressed-but-not-yet-fired highlight: every drawn (non-black) pixel in the
 * rectangle brightens to the same hue at full intensity; the background
 * stays black. Drawn by the shell over the armed control after ui_render --
 * actions fire on RELEASE, and this is the one consistent "armed" visual. */
void ui_press_overlay(struct ANativeWindow_Buffer *fb, int x, int y, int w,
                      int h);
/* Whole-frame dim to 13/16 intensity, applied by the shell AFTER ui_render
 * (so the offline harness still sees exact colours). This is what makes the
 * pressed highlight visible on already-saturated foregrounds -- white text
 * and the green big number have no headroom at full intensity, so the
 * resting frame gives some up. */
void ui_dim(struct ANativeWindow_Buffer *fb);

#endif
