// SPDX-License-Identifier: GPL-3.0
// settings.c --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

/* Small config files, one concern each: device-info strings, alarm thresholds,
 * display/settings-menu prefs, and the pairing code. The UI (main.c) owns when
 * to save/load; this module owns the state and the on-disk format. */
#include "settings.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: alarm_load's bound = the keypad's */
#include "dexlibc.h"
#include "plot.h"
#include "sensors.h" /* MARK_N / MARK_SIZE_MAX: the real bounds */
#include "ui.h"      /* MA_INS_SLOW / MA_WT_OPEN: the default pinned actions */
#include "util.h"
#include "weight.h" /* WT_KG / WT_LB: the weight display unit */
#include <stdio.h>  /* snprintf */

char g_model[24], g_fw[24], g_mfr[24];
/* THE TWO BANDS, NESTED, AND THE NUDGE IS THE OUTER ONE. Chosen by the user,
 * not derived: alarm 70/300 is the conservative "act now" band that should be
 * set once and left alone, and nudge 85/250 sits outside it as the early
 * "have a look" that fires first and can be ignored.
 *
 * That nesting is the whole feature. Editing the ALARM threshold day to day --
 * down after a meal, back up when unaware -- is the habit the nudge exists to
 * retire, because its failure mode is the dangerous one: the alarm gets left
 * parked somewhere it can no longer help while the user goes on believing a
 * reminder is armed. Anything that changes these numbers must preserve
 * nudge_low >= alarm_low and nudge_high <= alarm_high, or the nudge is inside
 * the alarm and can never fire first (nudge_fire suppresses it under a
 * sounding alarm).
 *
 * Defaults only. A fresh install has no alarm.cfg; every existing file
 * overrides all four on load. */
int g_alarm_low = 70, g_alarm_high = 300;
int g_nudge_low = 85, g_nudge_high = 250;
int g_sound_on = 1, g_vib_on = 1;
/* Both ON by default: with the nudge band armed out of the box, an alert the
 * user has to go and switch on in a second place would just be a way for it
 * to be silently missing. */
int g_nudge_sound = 1, g_nudge_vib = 1;
int g_orient;
int g_screen_on = 1; /* default: hold the screen on, as the app always has */
int g_newdata_mode;  /* ND_OFF / ND_BEEP / ND_CHIRP; default silent */
int g_units;
/* Weight display unit. Pounds by default because that is what the migrated
 * log was kept in; the file itself is always grams, so this only chooses how
 * the numbers are rendered (weight.h). */
int g_wunits = WT_LB;
int g_disc;
int g_plot_max = PLOT_GLU_MAX;
/* Insulin plot styling, PER TYPE (index INS_SLOW / INS_FAST): marker
 * shape, ui palette colour, marker size. Defaults: crosses, SLOW white,
 * FAST blue (matching the log table's blue FAST rows). */
int g_ins_marker[2] = {1, 1};
int g_ins_color[2]  = {6, 1};
int g_ins_size[2]   = {2, 2};
int g_statbar_val   = 1; /* status bar shows the VALUE (0 = app icon) */
int g_lockscr_val   = 1; /* notification visible on the lock screen */
/* PINNED BY DEFAULT on a fresh install: SLOW insulin and WEIGHT.
 *
 * Those are the two things logged on a schedule rather than in reaction to
 * something -- once a day, at roughly the same hour -- so they are the two the
 * main screen can save a trip through the ADD menu for on the very first day,
 * before the user has found the PIN column to ask. FAST is deliberately not
 * among them: a correction dose is logged when it happens, not on a routine,
 * and the third slot is left free so the first pin the user chooses has
 * somewhere to go.
 *
 * These are MA_* action codes (see settings.h on why positions are not
 * stored); ui.h is included for exactly these two names. A settings file
 * written before the shortcut fields existed still loads with none pinned --
 * that is an upgrade, not a first install, and silently adding buttons to a
 * main screen someone already knows is not a default, it is a surprise. */
int g_shortcut[SC_MAX] = {MA_INS_SLOW, MA_WT_OPEN, 0};
char g_code_str[16] = "9973"; /* Stelo applicator default (rebuild to change) */
int g_remote_on;              /* push each new datapoint; default off */
/* Defaulted, not blank: a fresh install that has to be told the server before
 * it can do anything is a fresh install that mostly does not get told. Anyone
 * running their own instance edits one row. */
char g_remote_server[64] = "pancra.org";
long g_sync_uid;              /* 0 until an app is paired */
unsigned char g_sync_key[16];
char g_sync_email[64];
/* 443, because the client always speaks https now: the platform's TLS is
 * free and the session cookie the server sets is Secure. The old default of
 * 80 belonged to a plain-HTTP intake API that no longer exists. */
int g_remote_port = 443;
char g_info_path[256], g_alarm_path[256], g_settings_path[256],
    g_code_path[256], g_remote_path[256];

void info_save(void)
{
   int fd = open(g_info_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[96];
   int n = snprintf(b, sizeof b, "%s\n%s\n%s\n", g_model, g_fw, g_mfr);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void info_load(void)
{
   int fd = open(g_info_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[96];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]         = 0;
   char *p      = b;
   char *dst[3] = {g_model, g_fw, g_mfr};
   for (int i = 0; i < 3 && p; i++) {
      char *nl = p;
      while (*nl && *nl != '\n')
         nl++;
      int len = (int)(nl - p);
      if (len > 22)
         len = 22;
      for (int j = 0; j < len; j++)
         dst[i][j] = p[j];
      dst[i][len] = 0;
      p           = *nl ? nl + 1 : 0;
   }
}

void alarm_save(void)
{
   int fd = open(g_alarm_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[48];
   int n = snprintf(b, sizeof b, "%d %d %d %d\n", g_alarm_low, g_alarm_high,
                    g_nudge_low, g_nudge_high);
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

/* One unsigned decimal field at *q, skipping leading spaces and advancing past
 * the digits. Returns 1 iff at least one digit was consumed.
 *
 * DIGIT-CAPPED. Unbounded accumulation is undefined behaviour, and it happens
 * during parsing -- before any range check can reject anything. A wrapped
 * value can land back inside a plausible range and silently install thresholds
 * the user never chose, on the numbers that decide whether a hypo alarm can
 * fire at all. store.c, stats.c and sensors.c all received this hardening.
 *
 * The advance is OUTSIDE the cap, deliberately: putting it inside is what
 * turned the same fix in sensors.c into an infinite loop.
 *
 * Extracted when the nudge pair was appended: four hand-inlined copies of this
 * loop is four places for the cap to be dropped from. */
static int parse_field(char **q, int *out)
{
   char *p = *q;
   while (*p == ' ')
      p++;
   int v  = 0;
   int nd = 0;
   while (*p >= '0' && *p <= '9') {
      if (nd < 9) {
         v = (v * 10) + (*p - '0');
         nd++;
      }
      p++;
   }
   *q   = p;
   *out = v;
   return nd > 0;
}

void alarm_load(void)
{
   int fd = open(g_alarm_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* 256, matching what remote_save writes. It was 48, sized for
    * "1 1.2.3.4 8080" -- and the line has since grown a host NAME, a user id,
    * a 32-character key and an email address. The read simply truncated, so
    * the key came back malformed and the account came back empty, and the app
    * looked like it had forgotten a pairing it had actually stored. Both ends
    * of this file are now the same size for that reason. */
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]       = 0;
   int lo     = 0;
   int hi     = 0;
   int nlo    = 0;
   int nhi    = 0;
   char *q    = b;
   int got_lo = parse_field(&q, &lo);
   int got_hi = parse_field(&q, &hi);
   /* Fields 3 and 4 are NEWER than files already on disk: an alarm file
    * written before the nudge existed has two fields, and must keep loading
    * its alarm pair rather than being rejected wholesale. Absent => the nudge
    * keeps its OFF defaults, which is the safe direction (no sound the user
    * did not ask for). */
   int got_nlo = parse_field(&q, &nlo);
   int got_nhi = parse_field(&q, &nhi);
   /* Range-check, do not merely test for non-zero. A corrupt or hand-edited
    * file with lo=99999 silently DISABLES the low alarm (nothing is ever below
    * it) and lo>hi leaves both alarms permanently latched -- the two ways this
    * file can fail dangerously. Bounds match the keypad's own limits, so a
    * value that could not be typed cannot be loaded either: both thresholds
    * 0..AL_ENTRY_MAX (each end is that alarm's deliberate OFF switch -- see
    * alarmlogic.h). */
   /* got_lo/got_hi: with 0 now LEGAL, a file that parses to no digits at all
    * must be rejected explicitly -- otherwise any garbage reads as the valid
    * pair 0/0 and silently installs both alarms OFF, thresholds the user
    * never chose. */
   /* lo <= hi, not lo < hi: a threshold entry refuses a crossing, but the
    * old steppers could set the two EQUAL, and equal pairs exist in saved
    * files. Rejecting one silently reverted the user's thresholds to the
    * compiled defaults on the next launch -- values they never chose. The
    * predicate must accept everything the writer can emit. */
   if (got_lo && got_hi && lo <= AL_ENTRY_MAX && hi <= AL_ENTRY_MAX &&
       lo <= hi) {
      g_alarm_low  = lo;
      g_alarm_high = hi;
   }
   /* The nudge pair is committed SEPARATELY and by the same rules, never
    * cross-checked against the alarm pair. A nudge inside the alarm band is
    * pointless but harmless (the alarm suppresses it), while refusing to load
    * it would silently revert a threshold the user chose -- and this file's
    * whole reason for existing is that a threshold the user believes is armed
    * must actually be armed. */
   if (got_nlo && got_nhi && nlo <= AL_ENTRY_MAX && nhi <= AL_ENTRY_MAX &&
       nlo <= nhi) {
      g_nudge_low  = nlo;
      g_nudge_high = nhi;
   }
}

void settings_save(void)
{
   int fd = open(g_settings_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   /* 128, matching settings_load's reader. At 96 the two disagreed the moment
    * the field count grew, and a truncated line does not fail loudly: clampn
    * writes the prefix, the loader parses what it finds and stops, and the
    * tail fields silently revert to their defaults on every launch. */
   char b[128];
   int n = snprintf(
       b, sizeof b,
       "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
       g_sound_on, g_vib_on, g_orient, g_units, g_disc, g_plot_max, g_screen_on,
       g_newdata_mode, g_ins_marker[0], g_ins_color[0], g_ins_size[0],
       g_ins_marker[1], g_ins_color[1], g_ins_size[1], g_statbar_val,
       g_lockscr_val, g_nudge_sound, g_nudge_vib, g_wunits, g_shortcut[0],
       g_shortcut[1], g_shortcut[2]);
   n = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void settings_load(void)
{
   int fd = open(g_settings_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[128];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]      = 0;
   int v[22] = {g_sound_on,      g_vib_on,       g_orient,      g_units,
                g_disc,          g_plot_max,     g_screen_on,   g_newdata_mode,
                g_ins_marker[0], g_ins_color[0], g_ins_size[0], g_ins_marker[1],
                g_ins_color[1],  g_ins_size[1],  g_statbar_val, g_lockscr_val,
                g_nudge_sound,   g_nudge_vib,    g_wunits,      g_shortcut[0],
                g_shortcut[1],   g_shortcut[2]};
   char *q   = b;
   for (int i = 0; i < 22; i++) {
      while (*q == ' ')
         q++;
      if (*q < '0' || *q > '9')
         break;
      int x  = 0;
      int nd = 0; /* see alarm_load: cap the digits, advance outside the cap */
      while (*q >= '0' && *q <= '9') {
         if (nd < 9) {
            x = (x * 10) + (*q - '0');
            nd++;
         }
         q++;
      }
      v[i] = x;
   }
   g_sound_on  = v[0];
   g_vib_on    = v[1];
   g_orient    = (int)((unsigned)v[2] & 3U);
   g_units     = v[3] ? 1 : 0;
   g_disc      = (v[4] >= 0 && v[4] < 4) ? v[4] : 0;
   g_plot_max  = (v[5] >= 100 && v[5] <= 400) ? v[5] : PLOT_GLU_MAX;
   g_screen_on = v[6] ? 1 : 0;
   /* Was a 0/1 flag; CHIRP added a third value. Old files hold 0 or 1 and
    * still mean exactly what they meant, and anything else falls back to
    * silent rather than to a noise the user never chose. */
   g_newdata_mode = (v[7] >= ND_OFF && v[7] <= ND_CHIRP) ? v[7] : ND_OFF;
   /* Fields 9-14 are newer than some files on disk: out-of-range (or absent,
    * leaving the default) falls back to the defaults.
    *
    * THE BOUNDS COME FROM sensors.h NOW. They used to be literals, with a
    * comment saying "9 == MARK_N, 7 colours, 4 == MARK_SIZE_MAX; settings.c
    * stays decoupled from sensors.h, crosschecked by eye" -- and the eye is
    * what failed: MARK_SIZE_MAX is 5, not 4. The size picker offers 1..5 and
    * menu_action saves whatever it is handed, so choosing the LARGEST insulin
    * marker worked, persisted to disk, and was then silently reset to 2 by
    * this line on the next launch. A setting that quietly forgets itself
    * across a restart is worse than one that refuses the value outright.
    *
    * The colour count still has to be a literal: UI_NCOLORS is private to
    * ui.c, so the static assert below is what keeps it honest instead. */
   for (int k = 0; k < 2; k++) {
      int base        = 8 + (k * 3);
      int defc        = k ? 1 : 6; /* SLOW white, FAST blue */
      g_ins_marker[k] = (v[base] >= 0 && v[base] < MARK_N) ? v[base] : 1;
      g_ins_color[k] =
          (v[base + 1] >= 0 && v[base + 1] < SET_NCOLORS) ? v[base + 1] : defc;
      g_ins_size[k] =
          (v[base + 2] >= 1 && v[base + 2] <= MARK_SIZE_MAX) ? v[base + 2] : 2;
   }
   g_statbar_val = v[14] ? 1 : 0;
   g_lockscr_val = v[15] ? 1 : 0;
   /* Fields 17-18, newer than files already on disk. The loop above stops at
    * the first field the file does not have, so an older file leaves these at
    * their (ON) defaults -- see the header comment on the format. */
   g_nudge_sound = v[16] ? 1 : 0;
   g_nudge_vib   = v[17] ? 1 : 0;
   g_wunits      = v[18] ? WT_LB : WT_KG;
   /* Fields 19-21: the main-screen shortcuts, appended after them. An older
    * file stops the loop before these and leaves the (empty) defaults, which
    * is the whole point of the positional format. Compacted so a hole in the
    * middle -- which nothing writes, but a hand-edited file could -- cannot
    * leave a live slot stranded behind an empty one. */
   {
      int n2 = 0;
      for (int i = 0; i < SC_MAX; i++)
         if (v[19 + i] > 0)
            g_shortcut[n2++] = v[19 + i];
      while (n2 < SC_MAX)
         g_shortcut[n2++] = 0;
   }
   plot_set_max(g_plot_max);
}

void code_save(void)
{
   int fd = open(g_code_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   int n = 0;
   while (g_code_str[n])
      n++;
   if (write(fd, g_code_str, n) != n) {
   }
   close(fd);
}

/* A hostname or an IPv4 address: labels of letters, digits and hyphens
 * separated by single dots, no empty label, none starting or ending with a
 * hyphen. It used to accept a dotted quad ONLY, which was right when the
 * server was a box on the LAN and wrong the moment it got a name.
 *
 * Deliberately permissive about the whole name -- "duo", "pancra.org" and
 * "192.168.0.243" are all things the user legitimately types -- because this
 * is a field they typed, not a security boundary; what it must not do is
 * accept something that cannot be a host at all and then silently point every
 * future sync at nothing. */
int remote_server_valid(const char *s)
{
   if (!s || !*s)
      return 0;
   int n     = 0;
   int label = 0;
   for (const char *p = s; *p; p++) {
      if (++n > 63)
         return 0;
      if (*p == '.') {
         if (label == 0 || p[-1] == '-')
            return 0; /* empty label, or one ending in a hyphen */
         label = 0;
         continue;
      }
      int alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9');
      if (!alnum && *p != '-')
         return 0;
      if (label == 0 && *p == '-')
         return 0; /* a label may not start with a hyphen */
      if (++label > 63)
         return 0;
   }
   return label > 0 && s[n - 1] != '-';
}

void remote_save(void)
{
   int fd = open(g_remote_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   /* "on server port uid keyhex". The two trailing fields were added with
    * pairing; a file without them loads as "not paired", so an upgrade keeps
    * the server and simply asks to pair again. */
   char kh[33];
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 16; i++) {
      kh[2 * i]     = hx[g_sync_key[i] >> 4];
      kh[2 * i + 1] = hx[g_sync_key[i] & 15];
   }
   kh[32] = 0;
   char b[256];
   int n = snprintf(b, sizeof b, "%d %s %d %ld %s %s\n", g_remote_on ? 1 : 0,
                    g_remote_server[0] ? g_remote_server : "-", g_remote_port,
                    g_sync_uid, g_sync_uid > 0 ? kh : "-",
                    g_sync_email[0] ? g_sync_email : "-");
   n     = clampn(n, sizeof b);
   if (write(fd, b, n) != n) {
   }
   close(fd);
}

void remote_load(void)
{
   int fd = open(g_remote_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   /* 256, matching what remote_save writes. It was 48, sized for
    * "1 1.2.3.4 8080" -- and the line has since grown a host NAME, a user id,
    * a 32-character key and an email address. The read simply truncated, so
    * the key came back malformed and the account came back empty, and the app
    * looked like it had forgotten a pairing it had actually stored. Both ends
    * of this file are now the same size for that reason. */
   char b[256];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   b[n]    = 0;
   char *q = b;
   if (*q != '0' && *q != '1')
      return; /* garbage: keep the prior values, like every sibling loader */
   int on = *q++ - '0';
   while (*q == ' ')
      q++;
   char host[sizeof g_remote_server];
   int k = 0;
   while (*q && *q != ' ' && *q != '\n') {
      if (k >= (int)sizeof host - 1)
         return; /* an address that long cannot be valid */
      host[k++] = *q++;
   }
   host[k] = 0;
   while (*q == ' ')
      q++;
   int port = 0;
   int nd   = 0; /* see alarm_load: cap the digits, advance outside the cap */
   while (*q >= '0' && *q <= '9') {
      if (nd < 9) {
         port = (port * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   /* "-" is the saver's own empty-address marker; anything else must be a
    * well-formed host name, and the port must be a real TCP port. Commit all
    * three together or nothing: a half-applied file (say, a valid port with a
    * corrupt address) could silently re-point the push at the wrong host. */
   int host_ok = (host[0] == '-' && host[1] == 0) || remote_server_valid(host);
   if (!host_ok || port < 1 || port > 65535)
      return;
   /* The paired identity and the account, read as SEQUENTIAL TOKENS.
    *
    * They were read by OFFSET -- the email was taken from 32 bytes past the
    * uid, the length of a key in hex. That is right only when a key is
    * actually there: unpaired, the saver writes "-" for the key, so the
    * offset landed inside the next field and the email loaded as "-", which
    * the check below then treated as unset. The address the user had typed
    * vanished on the next launch, with nothing to say why. Never compute a
    * position in a whitespace-separated line; walk it. */
   while (*q == ' ')
      q++;
   long uid = 0;
   int ud   = 0;
   while (*q >= '0' && *q <= '9') {
      if (ud < 18) {
         uid = (uid * 10) + (*q - '0');
         ud++;
      }
      q++;
   }
   while (*q == ' ')
      q++;
   /* the key token, however long it turns out to be */
   char keytok[80];
   int kk = 0;
   while (*q && *q != ' ' && *q != '\n' && kk < (int)sizeof keytok - 1)
      keytok[kk++] = *q++;
   keytok[kk] = 0;
   while (*q == ' ')
      q++;
   char em[sizeof g_sync_email];
   int ek = 0;
   while (*q && *q != ' ' && *q != '\n' && ek < (int)sizeof em - 1)
      em[ek++] = *q++;
   em[ek] = 0;

   unsigned char key[16];
   int keyok = 0;
   if (uid > 0 && kk == 32) {
      keyok = 1;
      for (int i = 0; i < 16 && keyok; i++) {
         int v = 0;
         for (int h = 0; h < 2; h++) {
            char c = keytok[(2 * i) + h];
            int d  = -1;
            if (c >= '0' && c <= '9')
               d = c - '0';
            else if (c >= 'a' && c <= 'f')
               d = (c - 'a') + 10;
            else if (c >= 'A' && c <= 'F')
               d = (c - 'A') + 10;
            if (d < 0) {
               keyok = 0;
               break;
            }
            v = (v * 16) + d;
         }
         key[i] = (unsigned char)v;
      }
   }
   g_remote_on   = on;
   g_remote_port = port;
   if (ek && !(em[0] == '-' && em[1] == 0)) {
      for (int i = 0;; i++) {
         g_sync_email[i] = em[i];
         if (!em[i])
            break;
      }
   } else {
      g_sync_email[0] = 0;
   }
   if (uid > 0 && keyok) {
      g_sync_uid = uid;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = key[i];
   } else {
      g_sync_uid = 0;
   }
   if (host[0] == '-')
      g_remote_server[0] = 0;
   else
      for (int i = 0;; i++) {
         g_remote_server[i] = host[i];
         if (!host[i])
            break;
      }
}

void sync_key_save(long uid, const unsigned char key[16])
{
   g_sync_uid = uid;
   for (int i = 0; i < 16; i++)
      g_sync_key[i] = key[i];
   remote_save();
}

void code_load(void)
{
   int fd = open(g_code_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char b[16];
   int n = (int)read(fd, b, sizeof b - 1);
   close(fd);
   if (n <= 0)
      return;
   int k = 0;
   for (int i = 0; i < n && k < (int)sizeof g_code_str - 1; i++)
      if (b[i] >= '0' && b[i] <= '9')
         g_code_str[k++] = b[i];
   /* Only commit when at least one digit was parsed. A non-empty file with no
    * digits (a partial write, or a hand-edit) would otherwise wipe a working
    * code to "" -- every sibling loader preserves its prior value on garbage.
    */
   if (k > 0)
      g_code_str[k] = 0;
}
