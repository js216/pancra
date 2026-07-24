// SPDX-License-Identifier: GPL-3.0
// settingstest.c --- Host tests for the settings/alarm-threshold persistence
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for settings.c, which had none.
 *
 * alarm_load is the last line of defence for the two numbers that decide
 * whether a hypo alarm can fire at all, and its validation has to reject the
 * two corruptions that are dangerous while accepting everything the writer can
 * legitimately emit:
 *
 *   - a low threshold out of range silently DISABLES the low alarm (nothing is
 *     ever below 99999);
 *   - low > high latches BOTH alarms permanently, because every reading is
 *     simultaneously below low and above high;
 *   - but low == high is a state alarm_step can legitimately produce (a
 *     crossing is resolved by making the two equal), so rejecting it reverted
 *     the user's saved thresholds to the compiled defaults on the next launch
 *     -- values they never chose.
 *
 * Built and run by `make settingstest`.
 */
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

/* plot.c owns this; settings_load calls it. Stubbed so the test links without
 * dragging in the renderer. */
static int stub_plot_max;

void plot_set_max(int mgdl)
{
   stub_plot_max = mgdl;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

static void paths(void)
{
   (void)snprintf(g_alarm_path, sizeof g_alarm_path, "tmp/uitest/st-alarm");
   (void)snprintf(g_settings_path, sizeof g_settings_path, "tmp/uitest/st-set");
   (void)snprintf(g_code_path, sizeof g_code_path, "tmp/uitest/st-code");
   (void)snprintf(g_info_path, sizeof g_info_path, "tmp/uitest/st-info");
   (void)snprintf(g_remote_path, sizeof g_remote_path, "tmp/uitest/st-remote");
}

/* Write raw bytes to a settings file, so corruption can be simulated exactly.
 */
static void put(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(text, f);
      fclose(f);
   }
}

int main(void)
{
   paths();

   printf("== alarm thresholds round-trip ==\n");
   g_alarm_low  = 75;
   g_alarm_high = 210;
   alarm_save();
   g_alarm_low = g_alarm_high = 0;
   alarm_load();
   ck(g_alarm_low == 75 && g_alarm_high == 210,
      "what was saved is what comes back");

   printf("== a corrupt alarm file must not disable or latch the alarm ==\n");
   g_alarm_low  = 80;
   g_alarm_high = 200;
   put(g_alarm_path, "99999 400\n");
   alarm_load();
   ck(g_alarm_low == 80 && g_alarm_high == 200,
      "an out-of-range LOW is rejected (it would disable the low alarm)");

   put(g_alarm_path, "300 100\n");
   alarm_load();
   ck(g_alarm_low == 80 && g_alarm_high == 200,
      "low > high is rejected (it would latch BOTH alarms forever)");

   put(g_alarm_path, "20 200\n");
   alarm_load();
   ck(g_alarm_low == 80, "a LOW below the keypad's own minimum is rejected");

   put(g_alarm_path, "80 500\n");
   alarm_load();
   ck(g_alarm_high == 200, "a HIGH above the keypad's own maximum is rejected");

   printf("== an absurd digit run must terminate and be refused ==\n");
   {
      /* Unbounded accumulation is UB and happens before the range check, so a
       * wrapped value can land back inside [40,400] and install thresholds the
       * user never chose. The loop must also TERMINATE -- putting the cursor
       * advance inside the digit cap is what turned the same fix in sensors.c
       * into an infinite loop on every launch. */
      g_alarm_low  = 88;
      g_alarm_high = 199;
      put(g_alarm_path, "99999999999999999999999 88888888888888888888\n");
      alarm_load(); /* must terminate */
      ck(g_alarm_low == 88 && g_alarm_high == 199,
         "an absurd digit run leaves the thresholds untouched");
      put(g_settings_path, "1 1 2 1 2 999999999999999999999999 1\n");
      settings_load(); /* must terminate */
      ck(g_plot_max >= 100 && g_plot_max <= 400,
         "...and an absurd plot maximum still falls back into range");
   }

   printf("== but low == high IS legitimate and must load ==\n");
   /* alarm_step resolves a crossing by making the two equal, so the reader has
    * to accept everything the writer can emit -- otherwise the user's saved
    * thresholds silently revert to the compiled defaults. */
   put(g_alarm_path, "150 150\n");
   alarm_load();
   ck(g_alarm_low == 150 && g_alarm_high == 150,
      "equal thresholds survive a reload");

   printf("== a missing or empty file leaves the defaults alone ==\n");
   g_alarm_low  = 88;
   g_alarm_high = 199;
   unlink(g_alarm_path);
   alarm_load();
   ck(g_alarm_low == 88 && g_alarm_high == 199, "no file changes nothing");
   put(g_alarm_path, "");
   alarm_load();
   ck(g_alarm_low == 88 && g_alarm_high == 199,
      "an empty file changes nothing");
   put(g_alarm_path, "garbage\n");
   alarm_load();
   ck(g_alarm_low == 88 && g_alarm_high == 199, "non-numeric changes nothing");

   printf("== settings round-trip and clamping ==\n");
   g_sound_on  = 1;
   g_vib_on    = 0;
   g_orient    = 2;
   g_units     = 1;
   g_disc      = 3;
   g_plot_max  = 250;
   g_screen_on = 1;
   settings_save();
   g_orient = g_units = g_disc = g_plot_max = g_screen_on = 0;
   settings_load();
   ck(g_orient == 2 && g_units == 1 && g_disc == 3 && g_plot_max == 250,
      "settings round-trip");
   ck(stub_plot_max == 250, "...and the plot scale is applied on load");

   put(g_settings_path, "1 1 9 1 9 9999 1\n");
   settings_load();
   ck(g_orient >= 0 && g_orient <= 3,
      "a corrupt orientation is masked to 0..3");
   ck(g_disc >= 0 && g_disc < 4, "a corrupt DISCONNECT index is clamped");
   ck(g_plot_max >= 100 && g_plot_max <= 400,
      "a corrupt plot maximum falls back into range");

   printf("== the pairing code accepts only digits ==\n");
   put(g_code_path, "12ab34\n");
   code_load();
   ck(strcmp(g_code_str, "1234") == 0, "non-digits are stripped");
   /* An empty or unreadable file must LEAVE THE EXISTING CODE ALONE, matching
    * alarm_load and settings_load. Clobbering a good pairing code to empty
    * because a file failed to read would be strictly worse than ignoring it --
    * the compiled default is a usable value. */
   put(g_code_path, "");
   code_load();
   ck(strcmp(g_code_str, "1234") == 0, "an empty file leaves the code alone");
   unlink(g_code_path);
   code_load();
   ck(strcmp(g_code_str, "1234") == 0, "a missing file leaves the code alone");
   /* A NON-EMPTY file with no digits (a partial write or a hand-edit) must ALSO
    * leave the code alone -- it used to wipe the working code to "". */
   put(g_code_path, "\n");
   code_load();
   ck(strcmp(g_code_str, "1234") == 0,
      "a non-digit non-empty file leaves the code alone");
   {
      char big[64];
      memset(big, '7', sizeof big);
      big[sizeof big - 1] = 0;
      put(g_code_path, big);
      code_load();
      ck(strlen(g_code_str) < sizeof g_code_str,
         "an over-long code cannot overflow the buffer");
   }

   printf("== remote push config round-trips ==\n");
   g_remote_on = 1;
   (void)snprintf(g_remote_ip, sizeof g_remote_ip, "192.168.1.42");
   g_remote_port = 8080;
   remote_save();
   g_remote_on    = 0;
   g_remote_ip[0] = 0;
   g_remote_port  = 80;
   remote_load();
   ck(g_remote_on == 1 && strcmp(g_remote_ip, "192.168.1.42") == 0 &&
          g_remote_port == 8080,
      "what was saved is what comes back");
   /* An UNSET address must round-trip as unset (the "-" marker), not clobber
    * itself into a literal dash the validator would then refuse forever. */
   g_remote_on    = 0;
   g_remote_ip[0] = 0;
   remote_save();
   (void)snprintf(g_remote_ip, sizeof g_remote_ip, "10.0.0.1");
   remote_load();
   ck(g_remote_ip[0] == 0, "an unset address stays unset across a reload");

   printf("== a corrupt remote file keeps the prior values ==\n");
   /* Half-applying a corrupt file could silently re-point the push at the
    * wrong host, so the loader must commit all three fields or none. */
   g_remote_on = 1;
   (void)snprintf(g_remote_ip, sizeof g_remote_ip, "10.0.0.9");
   g_remote_port = 8080;
   put(g_remote_path, "1 999.168.1.1 80\n");
   remote_load();
   ck(strcmp(g_remote_ip, "10.0.0.9") == 0 && g_remote_port == 8080,
      "an out-of-range octet is rejected whole");
   put(g_remote_path, "1 10.0.0.5 0\n");
   remote_load();
   ck(g_remote_port == 8080, "port 0 is rejected whole");
   put(g_remote_path, "1 10.0.0.5 99999\n");
   remote_load();
   ck(g_remote_port == 8080, "a port above 65535 is rejected whole");
   put(g_remote_path, "garbage\n");
   remote_load();
   ck(g_remote_on == 1 && strcmp(g_remote_ip, "10.0.0.9") == 0,
      "non-numeric changes nothing");
   put(g_remote_path, "");
   remote_load();
   ck(g_remote_on == 1, "an empty file changes nothing");
   unlink(g_remote_path);
   remote_load();
   ck(g_remote_on == 1, "a missing file changes nothing");

   printf("== remote_ip_valid: the keypad's whole defence ==\n");
   /* The IP keypad offers only digits and dots, so this predicate is the ONLY
    * thing standing between a mistyped entry and a push aimed at nothing. */
   ck(remote_ip_valid("192.168.1.1"), "a normal LAN address passes");
   ck(remote_ip_valid("255.255.255.255"), "all-255 passes");
   ck(remote_ip_valid("0.0.0.0"), "all-zero is well-formed (if useless)");
   ck(!remote_ip_valid(""), "empty fails");
   ck(!remote_ip_valid("1.2.3"), "three octets fail");
   ck(!remote_ip_valid("1.2.3.4.5"), "five octets fail");
   ck(!remote_ip_valid("256.1.1.1"), "an octet above 255 fails");
   ck(!remote_ip_valid("1..2.3"), "an empty octet fails");
   ck(!remote_ip_valid(".1.2.3"), "a leading dot fails");
   ck(!remote_ip_valid("1.2.3."), "a trailing dot fails");
   ck(!remote_ip_valid("1111.2.3.4"), "a four-digit octet fails before it "
                                      "can wrap");

   printf("\n%s\n", all ? "ALL SETTINGS TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
