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
/* INCLUDED, not linked: this is settings.c's own unit test, and arranging a
 * case means setting a preference to a value no user could reach -- a corrupt
 * one, or half of an ordered pair -- which the public interface deliberately
 * refuses. Being inside the translation unit is what lets it write `g_p`
 * directly; everything else in the app sees the read-only view. (modeltest
 * includes main.c for exactly this reason.) */
#include "settings.c"
#include "sensors.h" /* MARK_N / MARK_SIZE_MAX */
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

/* (settings_load used to push the plot scale into a process global in
 * plot.c, so this file needed a stub for it. The scale is passed to each
 * render and each hit test now -- see lib/plot.h -- so there is nothing to
 * stub.) */

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

static void paths(void)
{
   /* The app's own path-building: these five filenames belong to settings.c,
    * and a test that spelled them itself would stop checking that they are
    * the ones the app actually reads. The DIRECTORY comes from the
    * environment (app/test/testdir.h), so the ASan build of this suite keeps
    * its files out of the plain tree. */
   settings_paths(test_dir());
}

/* EMPTY THE IN-MEMORY CREDENTIAL without touching the file under test.
 *
 * The identity is private to settings.c now -- which is the point, since
 * anything that can write half of it breaks every request -- so a test cannot
 * scribble over it to prove a value came from DISK. Loading a file that does
 * not exist does the same job through the public path: remote_load DEFINES
 * all three fields, so an absent file leaves them empty. */
static void scrub(void)
{
   char empty[160];
   settings_paths(test_path(empty, sizeof empty, "st-empty"));
   remote_load();
   paths();
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

/* Two writers of DIFFERENT fields, so neither can be blamed for the other's
 * value: whatever each ends on must be what the file says. */
static void *race_plot(void *a)
{
   (void)a;
   for (int i = 0; i < 200; i++)
      (void)settings_set_plot_max(200 + (i % 5) * 10);
   return 0;
}

static void *race_disc(void *a)
{
   (void)a;
   for (int i = 0; i < 200; i++)
      (void)settings_cycle_disc();
   return 0;
}

int main(void)
{
   /* FIRST, before paths() and before anything below mutates a global: these
    * are the compiled defaults, i.e. exactly what a fresh install gets, and
    * they are only observable here while they are still untouched. */
   printf("== fresh-install defaults: two NESTED bands ==\n");
   ck(g_p.alarm_low == 70 && g_p.alarm_high == 300,
      "the alarm defaults to 70/300 -- the conservative 'act now' band");
   ck(g_p.nudge_low == 85 && g_p.nudge_high == 250,
      "the nudge defaults to 85/250 -- the early 'have a look' band");
   /* THE INVARIANT, and the reason the feature exists. The nudge must sit
    * OUTSIDE the alarm: a nudge inside the alarm band can never fire first,
    * because nudge_fire suppresses it whenever an alarm is sounding, so the
    * user would be back to editing the alarm threshold day to day -- the habit
    * whose failure mode is an alarm left parked where it can no longer help.
    * Stated as a relation, not as four literals, so it keeps meaning something
    * if the numbers are ever retuned. */
   ck(g_p.nudge_low >= g_p.alarm_low,
      "the nudge LOW is at or above the alarm's, so it warns FIRST on the way "
      "down");
   ck(g_p.nudge_high <= g_p.alarm_high,
      "...and its HIGH at or below, likewise");
   ck(g_p.nudge_low <= g_p.nudge_high && g_p.alarm_low <= g_p.alarm_high,
      "and neither pair is inverted (an inverted pair latches both ends)");
   ck(g_p.nudge_sound && g_p.nudge_vib,
      "both nudge outputs are on, or the armed band would be silent");

   paths();

   printf("== atomic replacement preserves the old snapshot on failure ==\n");
   {
      char sbuf[160], tbuf[168];
      const char *snap = test_path(sbuf, sizeof sbuf, "st-atomic");
      const char *tmp  = test_path(tbuf, sizeof tbuf, "st-atomic.tmp");
      put(snap, "old\n");
      (void)rmdir(tmp);
      ck(mkdir(tmp, 0700) == 0, "the forced-failure fixture is installed");
      ck(atomic_replace(snap, "new\n", 4) == -1,
         "a replacement failure is reported");
      char b[16] = {0};
      FILE *f    = fopen(snap, "r");
      ck(f && fgets(b, sizeof b, f) && !strcmp(b, "old\n"),
         "the prior snapshot remains byte-for-byte intact");
      if (f)
         fclose(f);
      (void)rmdir(tmp);
      (void)unlink(snap);
   }

   printf("== alarm thresholds round-trip ==\n");
   g_p.alarm_low  = 75;
   g_p.alarm_high = 210;
   alarm_save();
   g_p.alarm_low = g_p.alarm_high = 0;
   alarm_load();
   ck(g_p.alarm_low == 75 && g_p.alarm_high == 210,
      "what was saved is what comes back");

   printf("== a corrupt alarm file must not disable or latch the alarm ==\n");
   g_p.alarm_low  = 80;
   g_p.alarm_high = 200;
   put(alarm_path(), "99999 400\n");
   alarm_load();
   ck(g_p.alarm_low == 80 && g_p.alarm_high == 200,
      "an out-of-range LOW is rejected (it would disable the low alarm)");

   put(alarm_path(), "300 100\n");
   alarm_load();
   ck(g_p.alarm_low == 80 && g_p.alarm_high == 200,
      "low > high is rejected (it would latch BOTH alarms forever)");

   /* Both thresholds accept 0..AL_ENTRY_MAX now -- 0 is LOW's deliberate
    * OFF switch (below any possible reading), so it must round-trip. */
   put(alarm_path(), "0 200\n");
   alarm_load();
   ck(g_p.alarm_low == 0, "LOW 0 (that alarm's OFF switch) is accepted");
   g_p.alarm_low  = 80;
   g_p.alarm_high = 200;
   /* With 0 legal, a file that parses to NO digits must be rejected
    * explicitly: garbage would otherwise read as the valid pair 0/0 and
    * silently install both alarms OFF. */
   put(alarm_path(), "not numbers\n");
   alarm_load();
   ck(g_p.alarm_low == 80 && g_p.alarm_high == 200,
      "a digit-free file is rejected, never read as a 0/0 pair");

   /* HIGH alone ranges past the 400 glucose scale, up to AL_HIGH_MAX (999)
    * -- parked up there it effectively disables the high alarm, which is the
    * user's call to make. 999 must load; 1000 must not. */
   put(alarm_path(), "80 999\n");
   alarm_load();
   ck(g_p.alarm_high == 999, "a HIGH up to AL_HIGH_MAX (999) is accepted");
   g_p.alarm_low  = 80;
   g_p.alarm_high = 200;
   put(alarm_path(), "80 1000\n");
   alarm_load();
   ck(g_p.alarm_high == 200,
      "a HIGH above the keypad's own maximum is rejected");

   printf("== an absurd digit run must terminate and be refused ==\n");
   {
      /* Unbounded accumulation is UB and happens before the range check, so a
       * wrapped value can land back inside [40,400] and install thresholds the
       * user never chose. The loop must also TERMINATE -- putting the cursor
       * advance inside the digit cap is what turned the same fix in sensors.c
       * into an infinite loop on every launch. */
      g_p.alarm_low  = 88;
      g_p.alarm_high = 199;
      put(alarm_path(), "99999999999999999999999 88888888888888888888\n");
      alarm_load(); /* must terminate */
      ck(g_p.alarm_low == 88 && g_p.alarm_high == 199,
         "an absurd digit run leaves the thresholds untouched");
      put(settings_path(), "1 1 2 1 2 999999999999999999999999 1\n");
      settings_load(); /* must terminate */
      ck(g_p.plot_max >= 100 && g_p.plot_max <= 400,
         "...and an absurd plot maximum still falls back into range");
   }

   printf("== but low == high IS legitimate and must load ==\n");
   /* alarm_step resolves a crossing by making the two equal, so the reader has
    * to accept everything the writer can emit -- otherwise the user's saved
    * thresholds silently revert to the compiled defaults. */
   put(alarm_path(), "150 150\n");
   alarm_load();
   ck(g_p.alarm_low == 150 && g_p.alarm_high == 150,
      "equal thresholds survive a reload");

   printf("== a missing or empty file leaves the defaults alone ==\n");
   g_p.alarm_low  = 88;
   g_p.alarm_high = 199;
   unlink(alarm_path());
   alarm_load();
   ck(g_p.alarm_low == 88 && g_p.alarm_high == 199, "no file changes nothing");
   put(alarm_path(), "");
   alarm_load();
   ck(g_p.alarm_low == 88 && g_p.alarm_high == 199,
      "an empty file changes nothing");
   put(alarm_path(), "garbage\n");
   alarm_load();
   ck(g_p.alarm_low == 88 && g_p.alarm_high == 199,
      "non-numeric changes nothing");

   printf("== nudge thresholds share the alarm file ==\n");
   g_p.alarm_low  = 75;
   g_p.alarm_high = 210;
   g_p.nudge_low  = 100;
   g_p.nudge_high = 190;
   alarm_save();
   g_p.alarm_low = g_p.alarm_high = g_p.nudge_low = g_p.nudge_high = 0;
   alarm_load();
   ck(g_p.alarm_low == 75 && g_p.alarm_high == 210 && g_p.nudge_low == 100 &&
          g_p.nudge_high == 190,
      "all four thresholds round-trip");

   /* THE BACK-COMPAT CASE, and it is not hypothetical: every alarm file on
    * every phone running a build before this one has exactly two fields. If a
    * missing nudge pair rejected the whole file, the user's ALARM thresholds
    * would silently revert to the compiled defaults on the first launch after
    * the update -- the precise failure the lo<=hi comment above describes. */
   g_p.alarm_low  = 88;
   g_p.alarm_high = 199;
   g_p.nudge_low  = 111;
   g_p.nudge_high = 222;
   put(alarm_path(), "80 250\n");
   alarm_load();
   ck(g_p.alarm_low == 80 && g_p.alarm_high == 250,
      "a two-field (pre-nudge) file still loads its alarm pair");
   ck(g_p.nudge_low == 111 && g_p.nudge_high == 222,
      "...and leaves the nudge pair untouched");

   /* The nudge pair is checked by the SAME rules, and independently: a bad
    * nudge must not take a good alarm down with it, or vice versa. */
   g_p.nudge_low  = 111;
   g_p.nudge_high = 222;
   put(alarm_path(), "80 250 190 100\n");
   alarm_load();
   ck(g_p.alarm_low == 80 && g_p.alarm_high == 250,
      "an inverted nudge pair does not reject the alarm pair");
   ck(g_p.nudge_low == 111 && g_p.nudge_high == 222,
      "...but is itself refused");
   put(alarm_path(), "80 250 100 1000\n");
   alarm_load();
   ck(g_p.nudge_low == 111 && g_p.nudge_high == 222,
      "an over-range nudge is refused, like the alarm's own bound");
   put(alarm_path(), "80 250 0 999\n");
   alarm_load();
   ck(g_p.nudge_low == 0 && g_p.nudge_high == 999,
      "0 / AL_ENTRY_MAX -- the nudge's OFF switch -- IS accepted");
   /* A nudge INSIDE the alarm band is pointless (the alarm suppresses it) but
    * must still load: refusing it would silently revert a threshold the user
    * chose, and would block the legitimate order of operations when moving
    * both. */
   put(alarm_path(), "100 200 40 400\n");
   alarm_load();
   ck(g_p.nudge_low == 40 && g_p.nudge_high == 400,
      "a nudge inside the alarm band loads unchanged");

   printf("== settings round-trip and clamping ==\n");
   g_p.sound_on  = 1;
   g_p.vib_on    = 0;
   g_p.orient    = 2;
   g_p.units     = 1;
   g_p.disc      = 3;
   g_p.plot_max  = 250;
   g_p.screen_on = 1;
   settings_save();
   g_p.orient = g_p.units = g_p.disc = g_p.plot_max = g_p.screen_on = 0;
   settings_load();
   ck(g_p.orient == 2 && g_p.units == 1 && g_p.disc == 3 && g_p.plot_max == 250,
      "settings round-trip");
   /* (The loaded scale used to be pushed into a process global in plot.c and
    * asserted here. It is passed to each render and each hit test now, so
    * "it was applied" is the round-trip above: the value the renderer will
    * be handed is g_p.plot_max.) */

   put(settings_path(), "1 1 9 1 9 9999 1\n");
   settings_load();
   ck(g_p.orient >= 0 && g_p.orient <= 3,
      "a corrupt orientation is masked to 0..3");
   ck(g_p.disc >= 0 && g_p.disc < 4, "a corrupt DISCONNECT index is clamped");
   ck(g_p.plot_max >= 100 && g_p.plot_max <= 400,
      "a corrupt plot maximum falls back into range");

   printf("== the nudge's own outputs round-trip, and survive old files ==\n");
   /* They are the LAST two fields, which is exactly where a save buffer too
    * small for the format silently drops them: clampn writes the prefix, the
    * loader parses what it finds and stops, and the tail reverts to its
    * default on every launch with nothing to show for it. Round-tripping the
    * last field is what proves the line was written whole. */
   g_p.nudge_sound = 0;
   g_p.nudge_vib   = 1;
   g_p.sound_on    = 1;
   g_p.vib_on      = 1;
   settings_save();
   g_p.nudge_sound = g_p.nudge_vib = 9;
   settings_load();
   ck(g_p.nudge_sound == 0 && g_p.nudge_vib == 1,
      "the nudge's sound and vibration round-trip independently");
   ck(g_p.sound_on == 1 && g_p.vib_on == 1,
      "...without disturbing the ALARM's own pair");
   /* Every settings.cfg already on a phone stops at field 16. Those files must
    * keep loading, leaving the two new fields at their ON defaults -- the same
    * append rule the format has always documented. */
   g_p.nudge_sound = 1;
   g_p.nudge_vib   = 1;
   put(settings_path(), "1 1 0 0 0 300 1 2 1 6 2 1 1 2 1 1\n");
   settings_load();
   ck(g_p.nudge_sound == 1 && g_p.nudge_vib == 1,
      "a 16-field (pre-nudge) file leaves both at their defaults");
   ck(g_p.newdata_mode == 2, "...while still loading every field it does have");

   printf("== the pairing code accepts only digits ==\n");
   put(code_path(), "12ab34\n");
   code_load();
   ck(strcmp(g_p.code_str, "1234") == 0, "non-digits are stripped");
   /* An empty or unreadable file must LEAVE THE EXISTING CODE ALONE, matching
    * alarm_load and settings_load. Clobbering a good pairing code to empty
    * because a file failed to read would be strictly worse than ignoring it --
    * the compiled default is a usable value. */
   put(code_path(), "");
   code_load();
   ck(strcmp(g_p.code_str, "1234") == 0, "an empty file leaves the code alone");
   unlink(code_path());
   code_load();
   ck(strcmp(g_p.code_str, "1234") == 0,
      "a missing file leaves the code alone");
   /* A NON-EMPTY file with no digits (a partial write or a hand-edit) must ALSO
    * leave the code alone -- it used to wipe the working code to "". */
   put(code_path(), "\n");
   code_load();
   ck(strcmp(g_p.code_str, "1234") == 0,
      "a non-digit non-empty file leaves the code alone");
   {
      char big[64];
      memset(big, '7', sizeof big);
      big[sizeof big - 1] = 0;
      put(code_path(), big);
      code_load();
      ck(strlen(g_p.code_str) < sizeof g_p.code_str,
         "an over-long code cannot overflow the buffer");
   }

   printf("== remote push config round-trips ==\n");
   g_p.remote_on = 1;
   (void)snprintf(g_p.remote_server, sizeof g_p.remote_server, "192.168.1.42");
   g_p.remote_port = 8080;
   remote_save();
   g_p.remote_on        = 0;
   g_p.remote_server[0] = 0;
   g_p.remote_port      = 80;
   remote_load();
   ck(g_p.remote_on == 1 && strcmp(g_p.remote_server, "192.168.1.42") == 0 &&
          g_p.remote_port == 8080,
      "what was saved is what comes back");
   /* An UNSET address must round-trip as unset (the "-" marker), not clobber
    * itself into a literal dash the validator would then refuse forever. */
   g_p.remote_on        = 0;
   g_p.remote_server[0] = 0;
   remote_save();
   (void)snprintf(g_p.remote_server, sizeof g_p.remote_server, "10.0.0.1");
   remote_load();
   ck(g_p.remote_server[0] == 0,
      "an unset address stays unset across a reload");

   printf("== the account survives a reload when NOT paired ==\n");
   /* The email used to be read from a fixed offset past the uid, which is
    * only correct when a 32-character key is there. Unpaired, the saver
    * writes "-" for the key, the offset landed in the wrong field, and the
    * address the user had typed silently vanished on the next launch. */
   g_p.remote_on = 1;
   (void)snprintf(g_p.remote_server, sizeof g_p.remote_server, "pancra.org");
   g_p.remote_port = 443;
   settings_set_email("a.b@example.com"); /* stores AND persists */
   scrub();                               /* nothing left in memory */
   remote_load();
   struct sync_creds tc;
   sync_creds_get(&tc);
   ck(strcmp(tc.email, "a.b@example.com") == 0,
      "an UNPAIRED account email round-trips");
   ck(tc.uid == 0, "...and it is still unpaired");
   {
      unsigned char k[16];
      for (int i = 0; i < 16; i++)
         k[i] = (unsigned char)(i + 1);
      sync_key_save(7, k);
      scrub();
      remote_load();
      sync_creds_get(&tc);
      ck(tc.uid == 7 && strcmp(tc.email, "a.b@example.com") == 0,
         "a PAIRED identity and the email round-trip together");
      ck(tc.key[0] == 1 && tc.key[15] == 16, "...and so does the key");
   }

   printf("== a corrupt remote file keeps the prior values ==\n");
   /* Half-applying a corrupt file could silently re-point the push at the
    * wrong host, so the loader must commit all three fields or none. */
   g_p.remote_on = 1;
   (void)snprintf(g_p.remote_server, sizeof g_p.remote_server, "10.0.0.9");
   g_p.remote_port = 8080;
   /* "999.168.1.1" would once have been the malformed case; it is a perfectly
    * good host NAME, so the malformed one is now a name no resolver could
    * accept. */
   put(remote_path(), "1 -bad.org 80\n");
   remote_load();
   ck(strcmp(g_p.remote_server, "10.0.0.9") == 0 && g_p.remote_port == 8080,
      "a malformed host is rejected whole");
   put(remote_path(), "1 10.0.0.5 0\n");
   remote_load();
   ck(g_p.remote_port == 8080, "port 0 is rejected whole");
   put(remote_path(), "1 10.0.0.5 99999\n");
   remote_load();
   ck(g_p.remote_port == 8080, "a port above 65535 is rejected whole");
   put(remote_path(), "garbage\n");
   remote_load();
   ck(g_p.remote_on == 1 && strcmp(g_p.remote_server, "10.0.0.9") == 0,
      "non-numeric changes nothing");
   put(remote_path(), "");
   remote_load();
   ck(g_p.remote_on == 1, "an empty file changes nothing");
   unlink(remote_path());
   remote_load();
   ck(g_p.remote_on == 1, "a missing file changes nothing");

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
         g_p.ins_size[k] = sz;
         settings_save();
         g_p.ins_size[k] = -1;
         settings_load();
         if (g_p.ins_size[k] != sz) {
            printf("  [FAIL] insulin size %d (type %d) came back as %d\n", sz,
                   k, g_p.ins_size[k]);
            all = 0;
         }
      }
   ck(all, "every insulin marker size 1..MARK_SIZE_MAX round-trips");
   for (int mk = 0; mk < MARK_N; mk++) {
      g_p.ins_marker[0] = mk;
      settings_save();
      g_p.ins_marker[0] = -1;
      settings_load();
      if (g_p.ins_marker[0] != mk) {
         printf("  [FAIL] insulin marker %d came back as %d\n", mk,
                g_p.ins_marker[0]);
         all = 0;
      }
   }
   ck(all, "every insulin marker 0..MARK_N-1 round-trips");

   printf("== pinned shortcuts survive the renderer being renumbered ==\n");
   /* THE PINS ARE A FILE FORMAT, and this is the test that says so.
    *
    * They used to be stored as MA_* touch codes straight out of ui.h. Those
    * codes are a renderer detail -- they have already been renumbered once,
    * because they kept colliding -- and a phone that upgrades has to keep the
    * buttons its owner chose. So the file holds enum shortcut_id (settings.h)
    * and old files are migrated on the way in. */
   ck(shortcut_migrate(0) == SC_NONE, "an empty slot stays empty");
   ck(shortcut_migrate(SC_INS_SLOW) == SC_INS_SLOW,
      "a value already in the new schema is left alone");
   /* The five legacy codes, spelled as the NUMBERS an old file holds. Naming
    * them would defeat the point: this table has to keep meaning the same
    * thing after ui.h renumbers. */
   ck(shortcut_migrate(21) == SC_INS_FAST, "legacy 21 was FAST INSULIN");
   ck(shortcut_migrate(23) == SC_INS_SLOW, "legacy 23 was SLOW INSULIN");
   ck(shortcut_migrate(25) == SC_INSLOG, "legacy 25 was the insulin log");
   ck(shortcut_migrate(281) == SC_WEIGHT, "legacy 281 was LOG WEIGHT");
   ck(shortcut_migrate(282) == SC_WTLOG, "legacy 282 was the weight log");
   ck(shortcut_migrate(9999) == SC_NONE,
      "a pin this build no longer offers is dropped, not kept as a blank");

   /* END TO END, through the loader: a file written by the OLD build must come
    * back as the same two buttons. 19 fields, then the pins. */
   put(settings_path(), "1 1 0 0 0 300 1 2 1 6 2 1 1 2 1 1 1 0 0 23 281 0\n");
   settings_load();
   ck(g_p.shortcut[0] == SC_INS_SLOW && g_p.shortcut[1] == SC_WEIGHT &&
          g_p.shortcut[2] == SC_NONE,
      "an old file's pins load as the same two buttons");
   /* ...and once saved, the file holds the new schema and still round-trips. */
   settings_save();
   g_p.shortcut[0] = g_p.shortcut[1] = g_p.shortcut[2] = SC_NONE;
   settings_load();
   ck(g_p.shortcut[0] == SC_INS_SLOW && g_p.shortcut[1] == SC_WEIGHT,
      "...and are still those two after a save/load in the new schema");

   /* ---- A CHANGE THAT COULD NOT BE WRITTEN DID NOT HAPPEN --------------
    *
    * Every setter here is a transaction: store, replace the file, and put the
    * old value back if the replace failed. They used to return void, so the
    * rollback was invisible -- the screen went on showing the choice the user
    * had just made while the file said otherwise, and the next launch
    * silently reverted it. One case per FAMILY, because each family writes a
    * different file and each rolls back differently: a scalar, three fields
    * at once, a dense list, a string, and the paired identity.
    *
    * The fault is a real one: atomic_replace's rename is made to fail (this
    * test is built with -DAPP_FAULTS, which nothing that ships carries). */
   {
      settings_load(); /* a known-good state to roll back TO */
      int old_units  = g_p.units;
      int old_marker = g_p.ins_marker[0];
      int old_color  = g_p.ins_color[0];
      int old_size   = g_p.ins_size[0];
      char old_srv[64];
      snprintf(old_srv, sizeof old_srv, "%s", g_p.remote_server);
      settings_pin_add(SC_INSLOG); /* something to remove, and to fail to */
      int pins_before = 0;
      for (int i = 0; i < SC_MAX; i++)
         if (g_p.shortcut[i] > SC_NONE)
            pins_before++;

      setenv("APP_FAIL_RENAME", "1", 1);

      ck(settings_set_units(!old_units) == SETTINGS_UNSAVED,
         "a preference that cannot be written says so");
      ck(g_p.units == old_units,
         "...and the value in MEMORY is the one on disk, not the new one");

      ck(settings_set_ins_style(0, (old_marker + 1) % 4, (old_color + 1) % 4,
                                (old_size % 3) + 1) == SETTINGS_UNSAVED,
         "an insulin style that cannot be written says so");
      ck(g_p.ins_marker[0] == old_marker && g_p.ins_color[0] == old_color &&
             g_p.ins_size[0] == old_size,
         "...and ALL THREE fields come back, not the one that was noticed");

      ck(settings_pin_remove(SC_INSLOG) == SETTINGS_UNSAVED,
         "a pin removal that cannot be written says so");
      int pins_after = 0;
      for (int i = 0; i < SC_MAX; i++)
         if (g_p.shortcut[i] > SC_NONE)
            pins_after++;
      ck(pins_after == pins_before && settings_pinned(SC_INSLOG),
         "...and the whole list comes back: a half-compacted list loses one");

      ck(settings_set_server("elsewhere.example") == SETTINGS_UNSAVED,
         "a server that cannot be written says so");
      ck(strcmp(g_p.remote_server, old_srv) == 0,
         "...and the phone still points where the file says it does");

      /* THE IDENTITY, which is the one where half a rollback is worse than
       * none: a uid without its key fails every request with nothing on
       * screen to say why. */
      unsetenv("APP_FAIL_RENAME");
      unsigned char k[16];
      for (int i = 0; i < 16; i++)
         k[i] = (unsigned char)(i + 1);
      ck(sync_key_save(4242, k) == 0, "a pairing is stored");
      setenv("APP_FAIL_RENAME", "1", 1);
      ck(settings_forget_identity() == SETTINGS_UNSAVED,
         "an unpair that cannot be written says so");
      sync_creds_get(&tc);
      ck(tc.uid == 4242 && tc.key[0] == 1 && tc.key[15] == 16,
         "...and the identity comes back WHOLE, uid and key together");

      unsetenv("APP_FAIL_RENAME");
      ck(settings_set_units(!old_units) == SETTINGS_OK,
         "with the filesystem working again the same change commits");
      ck(g_p.units == !old_units, "...and it is the value in memory too");
   }

   /* ---- ONE COHERENT COPY ---------------------------------------------- */
   /* The reason settings_get exists: prefs() hands out a pointer INTO the
    * live aggregate, and the device-information strings are written from a
    * binder thread. A copy cannot be rewritten under its reader. */
   {
      settings_load();
      settings_set_dis(SET_DIS_MODEL, "MODEL-A");
      settings_set_dis(SET_DIS_FW, "FW-1");
      struct prefs snap;
      settings_get(&snap);
      settings_set_dis(SET_DIS_MODEL, "MODEL-B");
      ck(strcmp(snap.model, "MODEL-A") == 0,
         "a snapshot is not rewritten by a later change");
      struct prefs later;
      settings_get(&later);
      ck(strcmp(later.model, "MODEL-B") == 0,
         "...while a LATER copy has moved on");
      struct sync_creds cs;
      sync_creds_get(&cs);
      sync_creds_get(&tc);
      ck(cs.uid == tc.uid, "the identity copies out too");
   }

   printf("== a path that cannot be represented is REFUSED ==\n");
   {
      /* data_path used to stop copying the directory at cap-32 and append the
       * filename on top of the truncation, returning nothing. The result is
       * not a detectable error: it is a well-formed path to somewhere ELSE,
       * which reads as empty and looks exactly like a first run -- and two
       * long directories sharing a prefix truncate to the SAME path, so two
       * data sets land on top of each other. */
      char out[32];
      ck(data_path(out, sizeof out, "/data/x", "/settings.cfg") == 1,
         "a path that fits is built");
      ck(strcmp(out, "/data/x/settings.cfg") == 0, "...and is exactly right");

      /* One byte too long. */
      char big[64];
      memset(big, 'a', sizeof big);
      big[0]              = '/';
      big[sizeof big - 1] = 0;
      ck(data_path(out, sizeof out, big, "/settings.cfg") == 0,
         "a directory too long for the buffer is REFUSED");
      ck(out[0] == 0, "...and nothing is written: never a truncation");

      /* The exact boundary: everything including the NUL must fit. */
      char exact[8];
      ck(data_path(exact, sizeof exact, "/ab", "/cd") == 1,
         "a path that fits exactly is built");
      ck(strcmp(exact, "/ab/cd") == 0, "...with the whole name");
      ck(data_path(exact, sizeof exact, "/abc", "/defg") == 0,
         "...and one byte more is refused, NUL included");

      /* The owners answer too, so startup can refuse. Their buffers are far
       * larger than `out` above, so this needs a directory to match. */
      char huge[400];
      memset(huge, 'a', sizeof huge);
      huge[0]               = '/';
      huge[sizeof huge - 1] = 0;
      ck(settings_paths(test_dir()) == 1,
         "a usable data directory initialises the settings paths");
      ck(settings_paths(huge) == 0,
         "...and an unrepresentable one is reported, not truncated");
      paths();
   }

   printf("== the file formats carry a version, and refuse the future ==\n");
   {
      /* The schema used to be inferred from HOW MANY fields parsed, which
       * cannot tell an older file from a truncated one from a NEWER one --
       * and the newer case is the damaging one: fields this build does not
       * know are dropped, then written away by the next save. */
      paths();
      char sp[160], rp[160];
      test_path(sp, sizeof sp, "settings.cfg");
      test_path(rp, sizeof rp, "remote.cfg");

      /* A DEPLOYED (version 0) FILE still loads: no marker, 22 positional
       * integers, exactly what is on phones today. */
      put(sp, "1 1 2 1 3 250 1 0 1 6 2 1 1 2 1 1 1 1 0 0 0 0\n");
      ck(settings_load() == LOAD_OK, "a version-0 settings file still loads");
      struct prefs v0;
      settings_get(&v0);
      ck(v0.orient == 2 && v0.plot_max == 250 && v0.units == 1,
         "...and its fields are read exactly as before");

      /* ...and is rewritten WITH a marker, which then round-trips. */
      ck(settings_save() == SETTINGS_OK, "it is rewritten");
      ck(settings_load() == LOAD_OK, "...and the rewritten file loads");
      struct prefs v1;
      settings_get(&v1);
      ck(v1.orient == v0.orient && v1.plot_max == v0.plot_max &&
             v1.units == v0.units,
         "...with the same values: the migration changed no field");

      /* A FILE FROM THE FUTURE IS REFUSED WHOLE. Not partly read, not
       * defaulted field by field -- refused, with memory untouched, so a
       * downgrade cannot discard what the newer build stored. */
      put(sp, "v99 1 1 0 0 0 300 1 0 1 6 2 1 1 2 1 1 1 1 0 0 0 0\n");
      struct prefs before;
      settings_get(&before);
      ck(settings_load() == LOAD_CORRUPT,
         "a settings file from a NEWER build is refused");
      struct prefs after;
      settings_get(&after);
      ck(memcmp(&before, &after, sizeof before) == 0,
         "...leaving every field exactly as it was");

      /* The same for the credentials file. */
      put(rp, "1 pancra.org 443 7 0123456789abcdef0123456789abcdef a@b.c\n");
      ck(remote_load() == LOAD_OK, "a version-0 remote file still loads");
      struct sync_creds c0;
      sync_creds_get(&c0);
      ck(c0.uid == 7, "...and its identity is read");
      put(rp,
          "v99 1 pancra.org 443 7 0123456789abcdef0123456789abcdef a@b.c\n");
      ck(remote_load() == LOAD_CORRUPT,
         "a remote file from a NEWER build is refused");
      struct sync_creds c1;
      sync_creds_get(&c1);
      ck(c1.uid == 7, "...leaving the identity it already had");
   }

   printf("== a loader says WHICH of the four things happened ==\n");
   {
      /* Absent, ok, corrupt and unreadable are four different situations and
       * every loader used to answer all four with void. Reported as "absent",
       * a settings file that could not be read leaves the app on compiled
       * defaults, looking exactly like a fresh install -- and overwriting the
       * user's choices with those defaults at the next save. */
      paths();
      char sp[160];
      test_path(sp, sizeof sp, "settings.cfg");

      (void)remove(sp);
      ck(settings_load() == LOAD_ABSENT,
         "no file at all is a FIRST RUN, not a failure");

      ck(settings_save() == SETTINGS_OK, "a settings file is written");
      ck(settings_load() == LOAD_OK, "...and reads back as understood");

      /* Created and never written: what a power loss between the two
       * leaves. */
      put(sp, "");
      ck(settings_load() == LOAD_CORRUPT,
         "an EMPTY file is a torn save, not a first run");

      /* Present, and unreadable. */
      ck(settings_save() == SETTINGS_OK, "the file is rewritten");
      if (chmod(sp, 0) == 0) {
         ck(settings_load() == LOAD_ERROR,
            "a file that exists and cannot be READ says so");
         (void)chmod(sp, 0600);
      }
      ck(settings_load() == LOAD_OK, "...and reads again once permitted");

      /* The worst of several is what startup reports. */
      ck(load_worse(LOAD_ABSENT, LOAD_OK) == LOAD_OK &&
             load_worse(LOAD_OK, LOAD_CORRUPT) == LOAD_CORRUPT &&
             load_worse(LOAD_CORRUPT, LOAD_ERROR) == LOAD_ERROR &&
             load_worse(LOAD_ERROR, LOAD_ABSENT) == LOAD_ERROR,
         "several loads reduce to the worst one");
   }

   printf("== the file ends up holding the state that WON ==\n");
   {
      /* THE ORDERING GUARD, which only concurrency can exercise.
       *
       * A save renders its bytes under set_lk and then writes them with the
       * lock released -- that is the whole point of the change, so a frame
       * never waits for flash. But it opens a window: two setters can each
       * render and then race to the file, and if the OLDER render lands last
       * the next launch reads a state the user replaced. Every job carries
       * the generation it rendered at, and a write that is not newer than
       * what is already on disk is skipped.
       *
       * Single-threaded, nothing here can fail; that is why this case exists.
       * Two threads hammer two different fields, and afterwards the file has
       * to agree with memory -- which it cannot if a stale render won.
       *
       * WHAT THIS DOES AND DOES NOT PROVE. It proves the split save keeps the
       * file consistent with memory under real concurrency, which is the
       * property the change had to preserve. It does NOT reliably catch the
       * removal of the ordering guard itself: instrumented, this fixture
       * produces about two out-of-order writes in four hundred, and only the
       * LAST write being a stale one is visible here -- so a build with the
       * guard deleted passes this most of the time. The guard is defensive
       * and cheap; it is not covered by an assertion that can fail, and
       * saying so is better than implying it is. */
      paths();
      g_p.plot_max = 300;
      g_p.disc     = 0;
      (void)settings_save();

      pthread_t th[2];
      pthread_create(&th[0], 0, race_plot, 0);
      pthread_create(&th[1], 0, race_disc, 0);
      pthread_join(th[0], 0);
      pthread_join(th[1], 0);

      int want_plot = g_p.plot_max;
      int want_disc = g_p.disc;
      settings_load();
      ck(g_p.plot_max == want_plot && g_p.disc == want_disc,
         "after concurrent saves the FILE holds what memory holds");
   }

   printf("\n%s\n", all ? "ALL SETTINGS TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
