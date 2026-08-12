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
#include "sensors.h" /* MARK_N / MARK_SIZE_MAX */
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
   (void)snprintf(g_alarm_path, sizeof g_alarm_path, "build/app/test/st-alarm");
   (void)snprintf(g_settings_path, sizeof g_settings_path,
                  "build/app/test/st-set");
   (void)snprintf(g_code_path, sizeof g_code_path, "build/app/test/st-code");
   (void)snprintf(g_info_path, sizeof g_info_path, "build/app/test/st-info");
   (void)snprintf(g_remote_path, sizeof g_remote_path,
                  "build/app/test/st-remote");
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
   /* FIRST, before paths() and before anything below mutates a global: these
    * are the compiled defaults, i.e. exactly what a fresh install gets, and
    * they are only observable here while they are still untouched. */
   printf("== fresh-install defaults: two NESTED bands ==\n");
   ck(g_alarm_low == 70 && g_alarm_high == 300,
      "the alarm defaults to 70/300 -- the conservative 'act now' band");
   ck(g_nudge_low == 85 && g_nudge_high == 250,
      "the nudge defaults to 85/250 -- the early 'have a look' band");
   /* THE INVARIANT, and the reason the feature exists. The nudge must sit
    * OUTSIDE the alarm: a nudge inside the alarm band can never fire first,
    * because nudge_fire suppresses it whenever an alarm is sounding, so the
    * user would be back to editing the alarm threshold day to day -- the habit
    * whose failure mode is an alarm left parked where it can no longer help.
    * Stated as a relation, not as four literals, so it keeps meaning something
    * if the numbers are ever retuned. */
   ck(g_nudge_low >= g_alarm_low,
      "the nudge LOW is at or above the alarm's, so it warns FIRST on the way "
      "down");
   ck(g_nudge_high <= g_alarm_high, "...and its HIGH at or below, likewise");
   ck(g_nudge_low <= g_nudge_high && g_alarm_low <= g_alarm_high,
      "and neither pair is inverted (an inverted pair latches both ends)");
   ck(g_nudge_sound && g_nudge_vib,
      "both nudge outputs are on, or the armed band would be silent");

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

   /* Both thresholds accept 0..AL_ENTRY_MAX now -- 0 is LOW's deliberate
    * OFF switch (below any possible reading), so it must round-trip. */
   put(g_alarm_path, "0 200\n");
   alarm_load();
   ck(g_alarm_low == 0, "LOW 0 (that alarm's OFF switch) is accepted");
   g_alarm_low  = 80;
   g_alarm_high = 200;
   /* With 0 legal, a file that parses to NO digits must be rejected
    * explicitly: garbage would otherwise read as the valid pair 0/0 and
    * silently install both alarms OFF. */
   put(g_alarm_path, "not numbers\n");
   alarm_load();
   ck(g_alarm_low == 80 && g_alarm_high == 200,
      "a digit-free file is rejected, never read as a 0/0 pair");

   /* HIGH alone ranges past the 400 glucose scale, up to AL_HIGH_MAX (999)
    * -- parked up there it effectively disables the high alarm, which is the
    * user's call to make. 999 must load; 1000 must not. */
   put(g_alarm_path, "80 999\n");
   alarm_load();
   ck(g_alarm_high == 999, "a HIGH up to AL_HIGH_MAX (999) is accepted");
   g_alarm_low  = 80;
   g_alarm_high = 200;
   put(g_alarm_path, "80 1000\n");
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

   printf("== nudge thresholds share the alarm file ==\n");
   g_alarm_low  = 75;
   g_alarm_high = 210;
   g_nudge_low  = 100;
   g_nudge_high = 190;
   alarm_save();
   g_alarm_low = g_alarm_high = g_nudge_low = g_nudge_high = 0;
   alarm_load();
   ck(g_alarm_low == 75 && g_alarm_high == 210 && g_nudge_low == 100 &&
          g_nudge_high == 190,
      "all four thresholds round-trip");

   /* THE BACK-COMPAT CASE, and it is not hypothetical: every alarm file on
    * every phone running a build before this one has exactly two fields. If a
    * missing nudge pair rejected the whole file, the user's ALARM thresholds
    * would silently revert to the compiled defaults on the first launch after
    * the update -- the precise failure the lo<=hi comment above describes. */
   g_alarm_low  = 88;
   g_alarm_high = 199;
   g_nudge_low  = 111;
   g_nudge_high = 222;
   put(g_alarm_path, "80 250\n");
   alarm_load();
   ck(g_alarm_low == 80 && g_alarm_high == 250,
      "a two-field (pre-nudge) file still loads its alarm pair");
   ck(g_nudge_low == 111 && g_nudge_high == 222,
      "...and leaves the nudge pair untouched");

   /* The nudge pair is checked by the SAME rules, and independently: a bad
    * nudge must not take a good alarm down with it, or vice versa. */
   g_nudge_low  = 111;
   g_nudge_high = 222;
   put(g_alarm_path, "80 250 190 100\n");
   alarm_load();
   ck(g_alarm_low == 80 && g_alarm_high == 250,
      "an inverted nudge pair does not reject the alarm pair");
   ck(g_nudge_low == 111 && g_nudge_high == 222, "...but is itself refused");
   put(g_alarm_path, "80 250 100 1000\n");
   alarm_load();
   ck(g_nudge_low == 111 && g_nudge_high == 222,
      "an over-range nudge is refused, like the alarm's own bound");
   put(g_alarm_path, "80 250 0 999\n");
   alarm_load();
   ck(g_nudge_low == 0 && g_nudge_high == 999,
      "0 / AL_ENTRY_MAX -- the nudge's OFF switch -- IS accepted");
   /* A nudge INSIDE the alarm band is pointless (the alarm suppresses it) but
    * must still load: refusing it would silently revert a threshold the user
    * chose, and would block the legitimate order of operations when moving
    * both. */
   put(g_alarm_path, "100 200 40 400\n");
   alarm_load();
   ck(g_nudge_low == 40 && g_nudge_high == 400,
      "a nudge inside the alarm band loads unchanged");

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

   printf("== the nudge's own outputs round-trip, and survive old files ==\n");
   /* They are the LAST two fields, which is exactly where a save buffer too
    * small for the format silently drops them: clampn writes the prefix, the
    * loader parses what it finds and stops, and the tail reverts to its
    * default on every launch with nothing to show for it. Round-tripping the
    * last field is what proves the line was written whole. */
   g_nudge_sound = 0;
   g_nudge_vib   = 1;
   g_sound_on    = 1;
   g_vib_on      = 1;
   settings_save();
   g_nudge_sound = g_nudge_vib = 9;
   settings_load();
   ck(g_nudge_sound == 0 && g_nudge_vib == 1,
      "the nudge's sound and vibration round-trip independently");
   ck(g_sound_on == 1 && g_vib_on == 1,
      "...without disturbing the ALARM's own pair");
   /* Every settings.cfg already on a phone stops at field 16. Those files must
    * keep loading, leaving the two new fields at their ON defaults -- the same
    * append rule the format has always documented. */
   g_nudge_sound = 1;
   g_nudge_vib   = 1;
   put(g_settings_path, "1 1 0 0 0 300 1 2 1 6 2 1 1 2 1 1\n");
   settings_load();
   ck(g_nudge_sound == 1 && g_nudge_vib == 1,
      "a 16-field (pre-nudge) file leaves both at their defaults");
   ck(g_newdata_mode == 2, "...while still loading every field it does have");

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
   (void)snprintf(g_remote_server, sizeof g_remote_server, "192.168.1.42");
   g_remote_port = 8080;
   remote_save();
   g_remote_on        = 0;
   g_remote_server[0] = 0;
   g_remote_port      = 80;
   remote_load();
   ck(g_remote_on == 1 && strcmp(g_remote_server, "192.168.1.42") == 0 &&
          g_remote_port == 8080,
      "what was saved is what comes back");
   /* An UNSET address must round-trip as unset (the "-" marker), not clobber
    * itself into a literal dash the validator would then refuse forever. */
   g_remote_on        = 0;
   g_remote_server[0] = 0;
   remote_save();
   (void)snprintf(g_remote_server, sizeof g_remote_server, "10.0.0.1");
   remote_load();
   ck(g_remote_server[0] == 0, "an unset address stays unset across a reload");

   printf("== the account survives a reload when NOT paired ==\n");
   /* The email used to be read from a fixed offset past the uid, which is
    * only correct when a 32-character key is there. Unpaired, the saver
    * writes "-" for the key, the offset landed in the wrong field, and the
    * address the user had typed silently vanished on the next launch. */
   g_remote_on = 1;
   (void)snprintf(g_remote_server, sizeof g_remote_server, "pancra.org");
   g_remote_port = 443;
   (void)snprintf(g_sync_email, sizeof g_sync_email, "a.b@example.com");
   g_sync_uid = 0;
   remote_save();
   g_sync_email[0] = 0;
   remote_load();
   ck(strcmp(g_sync_email, "a.b@example.com") == 0,
      "an UNPAIRED account email round-trips");
   ck(g_sync_uid == 0, "...and it is still unpaired");
   {
      unsigned char k[16];
      for (int i = 0; i < 16; i++)
         k[i] = (unsigned char)(i + 1);
      sync_key_save(7, k);
      g_sync_email[0] = 0;
      g_sync_uid      = 0;
      remote_load();
      ck(g_sync_uid == 7 && strcmp(g_sync_email, "a.b@example.com") == 0,
         "a PAIRED identity and the email round-trip together");
      ck(g_sync_key[0] == 1 && g_sync_key[15] == 16, "...and so does the key");
   }

   printf("== a corrupt remote file keeps the prior values ==\n");
   /* Half-applying a corrupt file could silently re-point the push at the
    * wrong host, so the loader must commit all three fields or none. */
   g_remote_on = 1;
   (void)snprintf(g_remote_server, sizeof g_remote_server, "10.0.0.9");
   g_remote_port = 8080;
   /* "999.168.1.1" would once have been the malformed case; it is a perfectly
    * good host NAME, so the malformed one is now a name no resolver could
    * accept. */
   put(g_remote_path, "1 -bad.org 80\n");
   remote_load();
   ck(strcmp(g_remote_server, "10.0.0.9") == 0 && g_remote_port == 8080,
      "a malformed host is rejected whole");
   put(g_remote_path, "1 10.0.0.5 0\n");
   remote_load();
   ck(g_remote_port == 8080, "port 0 is rejected whole");
   put(g_remote_path, "1 10.0.0.5 99999\n");
   remote_load();
   ck(g_remote_port == 8080, "a port above 65535 is rejected whole");
   put(g_remote_path, "garbage\n");
   remote_load();
   ck(g_remote_on == 1 && strcmp(g_remote_server, "10.0.0.9") == 0,
      "non-numeric changes nothing");
   put(g_remote_path, "");
   remote_load();
   ck(g_remote_on == 1, "an empty file changes nothing");
   unlink(g_remote_path);
   remote_load();
   ck(g_remote_on == 1, "a missing file changes nothing");

   printf("== remote_server_valid: the editor's whole defence ==\n");
   /* The SERVER field is free text from an alphanumeric editor, so this
    * predicate is the ONLY thing standing between a mistyped entry and a sync
    * aimed at nothing. It accepts host NAMES now, not just dotted quads --
    * the server acquired a name the moment it stopped being a box on the LAN.
    */
   ck(remote_server_valid("192.168.1.1"), "a LAN address still passes");
   ck(remote_server_valid("pancra.org"), "a host name passes");
   ck(remote_server_valid("duo"), "a bare host name passes");
   ck(remote_server_valid("my-server.example.co.uk"), "hyphens and depth pass");
   ck(!remote_server_valid(""), "empty fails");
   ck(!remote_server_valid("."), "a lone dot fails");
   ck(!remote_server_valid(".pancra.org"), "a leading dot fails");
   ck(!remote_server_valid("pancra.org."), "a trailing dot fails");
   ck(!remote_server_valid("pancra..org"), "an empty label fails");
   ck(!remote_server_valid("-pancra.org"), "a label starting with - fails");
   ck(!remote_server_valid("pancra-.org"), "a label ending with - fails");
   ck(!remote_server_valid("pancra.org "), "a trailing space fails");
   ck(!remote_server_valid("panc ra.org"), "an embedded space fails");
   ck(!remote_server_valid("http://pancra.org"), "a scheme fails: this is a "
                                                 "host, not a URL");
   {
      /* Longer than the field can hold must fail rather than be truncated
       * into some OTHER host that happens to resolve. */
      char toolong[128];
      memset(toolong, 'a', sizeof toolong);
      toolong[sizeof toolong - 1] = 0;
      ck(!remote_server_valid(toolong), "a name past the field length fails");
   }

   /* EVERY VALUE THE PICKER OFFERS MUST SURVIVE A RESTART.
    *
    * settings_load bounded the insulin marker size with a literal 4 while
    * MARK_SIZE_MAX is 5, under a comment asserting "4 == MARK_SIZE_MAX;
    * crosschecked by eye". The size picker offers 1..MARK_SIZE_MAX and
    * menu_action saves what it is handed, so choosing the LARGEST size
    * applied, persisted, and was silently reset to 2 on the next launch --
    * the worst shape of settings bug, because nothing reports it. Loop the
    * whole offered range rather than pinning the one value that broke, so a
    * future off-by-one at either end fails here too. */
   for (int sz = 1; sz <= MARK_SIZE_MAX; sz++)
      for (int k = 0; k < 2; k++) {
         g_ins_size[k] = sz;
         settings_save();
         g_ins_size[k] = -1;
         settings_load();
         if (g_ins_size[k] != sz) {
            printf("  [FAIL] insulin size %d (type %d) came back as %d\n", sz,
                   k, g_ins_size[k]);
            all = 0;
         }
      }
   ck(all, "every insulin marker size 1..MARK_SIZE_MAX round-trips");
   for (int mk = 0; mk < MARK_N; mk++) {
      g_ins_marker[0] = mk;
      settings_save();
      g_ins_marker[0] = -1;
      settings_load();
      if (g_ins_marker[0] != mk) {
         printf("  [FAIL] insulin marker %d came back as %d\n", mk,
                g_ins_marker[0]);
         all = 0;
      }
   }
   ck(all, "every insulin marker 0..MARK_N-1 round-trips");

   printf("\n%s\n", all ? "ALL SETTINGS TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
