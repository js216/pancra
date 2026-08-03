// SPDX-License-Identifier: GPL-3.0
// settings.c --- Persisted config: alarms, display prefs, device info, code
// Copyright 2026 Jakob Kastelic

/* Small config files, one concern each: device-info strings, alarm thresholds,
 * display/settings-menu prefs, and the pairing code. The UI (main.c) owns when
 * to save/load; this module owns the state and the on-disk format. */
#include "settings.h"
#include "alarmlogic.h" /* AL_HIGH_MAX: alarm_load's bound = the keypad's */
#include "dexlibc.h"
#include "plot.h"
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
int g_statbar_val   = 1;      /* status bar shows the VALUE (0 = app icon) */
int g_lockscr_val   = 1;      /* notification visible on the lock screen */
char g_code_str[16] = "9973"; /* Stelo applicator default (rebuild to change) */
int g_remote_on;              /* push each new datapoint; default off */
char g_remote_ip[16];         /* dotted quad; "" until the user sets one */
int g_remote_port = 80;       /* glucoserve.py's default */
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
   char b[48];
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
       "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", g_sound_on,
       g_vib_on, g_orient, g_units, g_disc, g_plot_max, g_screen_on,
       g_newdata_mode, g_ins_marker[0], g_ins_color[0], g_ins_size[0],
       g_ins_marker[1], g_ins_color[1], g_ins_size[1], g_statbar_val,
       g_lockscr_val, g_nudge_sound, g_nudge_vib, g_wunits);
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
   int v[19] = {g_sound_on,      g_vib_on,       g_orient,      g_units,
                g_disc,          g_plot_max,     g_screen_on,   g_newdata_mode,
                g_ins_marker[0], g_ins_color[0], g_ins_size[0], g_ins_marker[1],
                g_ins_color[1],  g_ins_size[1],  g_statbar_val, g_lockscr_val,
                g_nudge_sound,   g_nudge_vib,    g_wunits};
   char *q   = b;
   for (int i = 0; i < 19; i++) {
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
   /* Fields 9-14 are newer than some files on disk: out-of-range (or
    * absent, leaving the default) falls back to the defaults. Bounds:
    * 9 == MARK_N, 7 colours, 4 == MARK_SIZE_MAX; settings.c stays
    * decoupled from sensors.h, crosschecked by eye. */
   for (int k = 0; k < 2; k++) {
      int base        = 8 + (k * 3);
      int defc        = k ? 1 : 6; /* SLOW white, FAST blue */
      g_ins_marker[k] = (v[base] >= 0 && v[base] < 9) ? v[base] : 1;
      g_ins_color[k] =
          (v[base + 1] >= 0 && v[base + 1] < 7) ? v[base + 1] : defc;
      g_ins_size[k] = (v[base + 2] >= 1 && v[base + 2] <= 4) ? v[base + 2] : 2;
   }
   g_statbar_val = v[14] ? 1 : 0;
   g_lockscr_val = v[15] ? 1 : 0;
   /* Fields 17-18, newer than files already on disk. The loop above stops at
    * the first field the file does not have, so an older file leaves these at
    * their (ON) defaults -- see the header comment on the format. */
   g_nudge_sound = v[16] ? 1 : 0;
   g_nudge_vib   = v[17] ? 1 : 0;
   g_wunits      = v[18] ? WT_LB : WT_KG;
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

int remote_ip_valid(const char *s)
{
   if (!s)
      return 0;
   int octets = 0;
   while (*s) {
      if (*s < '0' || *s > '9')
         return 0; /* each group starts with a digit */
      int v  = 0;
      int nd = 0;
      while (*s >= '0' && *s <= '9') {
         if (nd >= 3)
            return 0; /* >3 digits can wrap; reject before accumulating */
         v = (v * 10) + (*s - '0');
         nd++;
         s++;
      }
      if (v > 255)
         return 0;
      octets++;
      if (*s == '.') {
         if (!s[1])
            return 0; /* trailing dot: "1.2.3." */
         s++;
      } else if (*s) {
         return 0; /* anything but a digit or a separating dot */
      }
   }
   return octets == 4;
}

void remote_save(void)
{
   int fd = open(g_remote_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   char b[48];
   int n = snprintf(b, sizeof b, "%d %s %d\n", g_remote_on ? 1 : 0,
                    g_remote_ip[0] ? g_remote_ip : "-", g_remote_port);
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
   char b[48];
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
   char ip[sizeof g_remote_ip];
   int k = 0;
   while (*q && *q != ' ' && *q != '\n') {
      if (k >= (int)sizeof ip - 1)
         return; /* an address that long cannot be valid */
      ip[k++] = *q++;
   }
   ip[k] = 0;
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
    * well-formed dotted quad, and the port must be a real TCP port. Commit all
    * three together or nothing: a half-applied file (say, a valid port with a
    * corrupt address) could silently re-point the push at the wrong host. */
   int ip_ok = (ip[0] == '-' && ip[1] == 0) || remote_ip_valid(ip);
   if (!ip_ok || port < 1 || port > 65535)
      return;
   g_remote_on   = on;
   g_remote_port = port;
   if (ip[0] == '-')
      g_remote_ip[0] = 0;
   else
      for (int i = 0;; i++) {
         g_remote_ip[i] = ip[i];
         if (!ip[i])
            break;
      }
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
