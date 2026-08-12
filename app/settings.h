// SPDX-License-Identifier: GPL-3.0
// settings.h --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_SETTINGS_H
#define PANCRA_SETTINGS_H

/* Config state owned by settings.c and persisted to small files. The UI reads
 * these for display and writes them from the settings menu, then calls the
 * matching *_save(). Paths are built once at startup by the app. */
extern char g_model[24], g_fw[24],
    g_mfr[24];                        /* DIS: model / fw / manufacturer */
extern int g_alarm_low, g_alarm_high; /* thresholds, mg/dL */
/* NUDGE thresholds, mg/dL -- the one-time heads-up band (alarmlogic.h). They
 * live in the SAME file as the alarm pair because they are the same kind of
 * fact and must not be able to load half-applied against each other. */
extern int g_nudge_low, g_nudge_high;
extern int g_sound_on, g_vib_on; /* alarm sound / vibration */
/* NUDGE sound / vibration. SEPARATE from the alarm's, because the two alerts
 * mean different things: the alarm is "act now" and the nudge is "have a
 * look". Silencing the one you can ignore must not silence the one you
 * cannot, and the reverse -- a user who mutes the alarm in a meeting may well
 * still want the buzz. */
extern int g_nudge_sound, g_nudge_vib;
extern int g_orient;        /* 0 portrait 1 landscape 2 gravity 3 system */
extern int g_screen_on;     /* 1 keep screen on while open, 0 follow the OS */
extern int g_newdata_mode;  /* ND_OFF / ND_BEEP / ND_CHIRP (alarmlogic.h):
                             * what a new primary-CGM datapoint sounds like */
extern int g_units;         /* glucose: 0 mg/dL, 1 mmol/L */
extern int g_wunits;        /* weight: WT_KG / WT_LB (weight.h) */
extern int g_disc;          /* stale-data alarm: index into disc_min */
extern int g_plot_max;      /* plot vertical-scale top, mg/dL */
extern int g_ins_marker[2]; /* MARK_* per insulin type (INS_SLOW/FAST) */
extern int g_ins_color[2];  /* ui palette index per insulin type */
extern int g_ins_size[2];   /* marker size 1..MARK_SIZE_MAX per type */
/* MAIN-SCREEN PINS: up to three ADD-menu actions pinned onto the main
 * screen, beside the big '+'. Stored as the MA_* action codes themselves, not
 * as positions in the ADD menu -- that menu's order is presentation and has
 * changed before, and a stored position would silently start launching a
 * different action the next time a button is inserted. 0 = an empty slot.
 * Kept dense (empties last) so the main screen can just walk it. */
#define SC_MAX 3
extern int g_shortcut[SC_MAX];
extern int g_statbar_val;        /* 1 = status bar shows the value, 0 = icon */
extern int g_lockscr_val;        /* 1 = notification visible on lock screen */
extern char g_code_str[16];      /* runtime pairing code (PAIR NEW SENSOR) */
extern int g_remote_on;          /* 1 = push each new datapoint to the server */
extern char g_remote_server[64]; /* hostname or IP; "" = not set */
extern int g_remote_port;        /* server TCP port, 1..65535 */
/* The paired identity, stored in the SAME file as the server -- which is one
 * of the files that must never be synced (see sync.h): it is the secret that
 * authenticates this phone to the server. 0 = not paired. */
extern long g_sync_uid;
extern unsigned char g_sync_key[16];
extern char g_sync_email[64]; /* the account being synced into; "" = not set */
/* Remember a completed pairing, durably. */
void sync_key_save(long uid, const unsigned char key[16]);
extern char g_info_path[256], g_alarm_path[256], g_settings_path[256],
    g_code_path[256], g_remote_path[256];

void info_save(void); /* device-info strings "model\nfw\nmfr\n" */
void info_load(void);
/* "low high nudge_low nudge_high\n". The nudge pair was appended, so files
 * written before it exist with two fields only -- those load the alarm pair
 * and leave the nudge at its (OFF) defaults, like every other loader here. */
/* CHANGE AND PERSIST, in one call. Prefer these to writing the global and
 * remembering the save: the rule is not enforceable, and a forgotten save is
 * invisible until the app restarts and reverts the user's choice. */
void settings_set_units(int mmol);
void settings_set_wunits(int wu);
void settings_set_sound(int on);
void settings_set_remote_on(int on);

void alarm_save(void);
void alarm_load(void);
/* "sound vib orient units disc plot_max screen_on\n" -- fields are read
 * positionally and the parse stops at the first missing one, so appending a
 * field keeps older config files loadable (the new field keeps its default). */
void settings_save(void);
void settings_load(void); /* also applies plot_set_max() */
void code_save(void);     /* pairing code digits */
void code_load(void);
/* "on ip port\n" -- garbage keeps the prior values, like every loader here. */
void remote_save(void);
void remote_load(void);
/* How many sensor/marker colours exist. The table itself (UI_NCOLORS) is
 * private to ui.c, so settings_load cannot bound its stored colour index
 * against it directly. This is the shared name for that count, and ui.c
 * static-asserts the two agree -- the crosschecked-by-eye version of this
 * is exactly what let MARK_SIZE_MAX drift to a stale literal here. */
#define SET_NCOLORS 7

/* 1 iff s is a well-formed dotted quad (four octets, each 0..255). Pure. */
int remote_server_valid(const char *s);

#endif
