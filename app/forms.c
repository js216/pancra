// SPDX-License-Identifier: GPL-3.0
// forms.c --- Typed entry (see forms.h)
// Copyright 2026 Jakob Kastelic

#include "forms.h"
#include "alarm.h"
#include "alarmlogic.h" /* the entry bounds these commits enforce */
#include "blejni.h"     /* dexble_env: a JNIEnv for THIS thread */
#include "civil.h"
#include "exercise.h" /* EX_DUR_MAX: the ceiling a typed duration meets */
#include "food.h"
#include "formsint.h" /* what the four workflow controllers owe this file */
#include "insrow.h"   /* INS_*: what a dose row can say */
#include "insulin.h"
#include "keypad.h" /* enum keypad_mode: these numbers have names */
#include "log.h"
#include "nav.h"
#include "notify.h"
#include "remote.h"
#include "remotecfg.h"
#include "selection.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "status.h"
#include "syncjni.h"
#include "tzoff.h"
#include "uimodel.h"
#include "util.h"
#include "weight.h"
#include <jni.h>
#include <stdio.h>
#include <string.h>

/* LOG WEIGHT form state. The value is held in TENTHS of the DISPLAY unit, not
 * grams: it is what the user typed and what the keypad round-trips, and it is
 * converted once, on CONFIRM. Holding grams here instead would re-render the
 * field every time the unit preference changed mid-entry. */
/* ---- ONE STRUCT PER WORKFLOW -------------------------------------------
 *
 * These were eighteen adjacent globals, and adjacency was the only thing
 * grouping them. Nothing in a name said which editor a field belonged to, so
 * nothing stopped one workflow reading another's leftovers -- and the
 * sentinels that decide "new entry" versus "editing an existing one" lived
 * mainly in comments beside the declarations, which is to say nowhere the
 * compiler could see.
 *
 * The failure that shape invites is not a crash. It is opening LOG WEIGHT
 * straight after editing one, with `edit` still holding the previous row, and
 * having the new entry silently rewrite it. Each struct below is opened
 * through one helper that sets EVERY field, so a workflow starts from a state
 * it stated in full rather than from whatever the last one left behind. */

/* WHICH FORM'S INSTANT THIS MODE EDITS, as a pointer or NULL.
 *
 * A `weight ? weight's instant : insulin's` test at each call site is a
 * two-way choice that is right for two forms and silently wrong for a third:
 * every food date would move the INSULIN form's instant, because "not weight"
 * means insulin by default.
 * keypad.h carries the full argument; what matters here is that the mapping is
 * now total, and a mode that is not a form field at all gets NULL rather than
 * somebody else's timestamp. */
static long *form_instant_of(enum keypad_mode m)
{
   switch (kp_form_of(m)) {
      case KP_FORM_WEIGHT: return form_wt_instant();
      case KP_FORM_INSULIN: return form_ins_instant();
      case KP_FORM_FOOD: return form_food_instant();
      case KP_FORM_EXERCISE: return form_ex_instant();
      case KP_FORM_NONE: break;
   }
   return 0;
}

/* ---- OPENING A DRAFT SETS ALL OF IT --------------------------------------
 *
 * EVERY FIELD, INCLUDING THE ONES THE PATH HAS NO USE FOR. Opening the weight
 * form for a new entry by setting `t`, `tenths` and `edit` alone leaves `orig`
 * holding the row of whatever was edited last.
 * That is safe only for exactly as long as every reader of `orig` remembers
 * to test `edit` first -- an invariant spread across a dozen branches, none of
 * which the compiler checks, guarding a rewrite that lands on a row the user
 * is not looking at.
 *
 * These take the whole draft, so a workflow starts from a state it stated in
 * full rather than from whatever the last one left behind. */

/* THE KEYPAD AND WHAT THE DIGITS ARE FOR.
 *
 * One editor serves every typed value in the app, so everything that says
 * which value is being typed belongs together: the buffer, the refusal
 * message, the mode, where to return on close, and the three pending values
 * that different modes commit to different places.
 *
 * 64: the widest thing typed here is an email address or a host name, both of
 * which are 63-byte fields. It was 24, which silently truncated an address at
 * 23 characters -- and the truncation only became visible when pairing failed
 * against an account that did not exist. */
struct kp_state {
   char entry[64];
   int len;
   /* Why the last entry was refused, shown under the field until the next
    * keystroke. Empty means nothing was refused. See ui.h's kp_err. */
   char err[40];
   enum keypad_mode mode;
   enum ui_screen ret; /* where the keypad returns on close */
   /* THREE fields share the alphanumeric editor -- a sensor name, the sync
    * server and the account email -- and they commit to different places, so
    * which one opened it has to be recorded rather than guessed from the
    * menu. */
   int label_field;
   /* The value presently being typed on the rescale keypad, awaiting CONFIRM.
    * Keypad state, not calibration state: calib.c is told the number only
    * once the user commits to it. */
   int rescale;
   /* The calibration value awaiting CONFIRM, mg/dL; 0 = none. Same rule. */
   int cal_pending;
   /* INS_SLOW/INS_FAST being styled; -1 = the picker edits a sensor's
    * styling. */
   int markpick_ins;
};
static struct kp_state g_kp = {{0}, 0, {0}, 0, 0, 0, 0, 0, -1};

/* Close the keypad/device-list back to wherever pairing was launched from:
 * the settings menu (g_kp.ret==SCR_SETTINGS) or the main screen
 * (SCR_MAIN, restoring the chosen orientation). */
void keypad_close(void)
{
   /* A refusal belongs to the field that refused. Leaving it set carried it to
    * the NEXT keypad the user opened, where it described nothing. */
   g_kp.err[0] = 0;
   nav_go(g_kp.ret);
   if (!g_kp.ret)
      shell_orient_apply();
}

/* THE ZONE THE FORMS EDIT IN. Every date/time entry below splits an instant
 * into a civil date and recombines it, and both halves of that need the
 * offset in force AT THE INSTANT BEING EDITED -- not tz_off_now(), which is the
 * offset TODAY.
 *
 * That is TODO 131 exactly: the keypad split and recombined with tz_off_now(),
 * so moving a dose to a date on the far side of a DST boundary persisted it an
 * hour wrong, and wrote today's offset into its tz column so nothing
 * downstream could tell it had happened. What each entry does about the
 * repeated and the skipped hour is stated once, in civil.h, and applied by
 * civil_reaim.
 *
 * dexble_env(), not g_act->env: it returns a JNIEnv valid on the CALLING
 * thread, and these run on the UI thread. tz_off_now() is the fallback when
 * there is no VM at all -- a stale offset, but never a wild one, which is
 * tzoff.h's standing rule. */
long form_zone(void *ctx, long t)
{
   (void)ctx;
   JNIEnv *env = dexble_env();
   return env ? tz_offset_at(env, t) : tz_off_now();
}

/* Render a mg/dL bound in the units the screen is showing.
 *
 * The threshold and calibration keypads ACCEPT mmol/L when the display unit is
 * set, so a refusal quoting mg/dL sends the user to check a number they never
 * typed -- the comment on the calibration branch already notes that mmol/L
 * users are the ones who hit that bound. the renderer does this correctly for
 * the MAX: line it draws one row above; this is the same conversion for the
 * refusal text. */
static void fmt_bound(char *out, int cap, int mgdl)
{
   struct prefs sp;
   settings_get(&sp);
   if (sp.units) {
      int t = (mgdl * 10) / 18;
      (void)snprintf(out, (size_t)cap, "%d.%d", t / 10, t % 10);
   } else {
      (void)snprintf(out, (size_t)cap, "%d", mgdl);
   }
}

/* WHAT A TYPED TEXT FIELD MEANT. The label screen is one editor serving three
 * different fields -- the account email, the sync server, and a device's own
 * name -- so the validation belongs to the FIELD, not to the editor. Each is
 * refused with its own message, because "invalid" on an email that failed an
 * address check and a server that failed a hostname check are different
 * problems with different fixes. */
int label_commit(void)
{
   if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_EMAIL) {
      /* The account email. Lower-cased for the same reason as the server,
       * and required to look like an address at all -- a typo here fails
       * pairing with a message about the code, which is the wrong thing to
       * go looking at. */
      char em[sizeof(((struct sync_creds *)0)->email)];
      int n  = g_kp.len < (int)sizeof em - 1 ? g_kp.len : (int)sizeof em - 1;
      int at = 0;
      int dot_after_at = 0;
      for (int i = 0; i < n; i++) {
         char c = g_kp.entry[i];
         em[i]  = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
         if (em[i] == '@')
            at++;
         else if (em[i] == '.' && at)
            dot_after_at = 1;
      }
      em[n] = 0;
      if (n < 5 || at != 1 || !dot_after_at || em[0] == '@' ||
          em[n - 1] == '@' || em[n - 1] == '.') {
         LOGI("account '%s' does not look like an address, not saved", em);
         (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT AN EMAIL ADDRESS");
         g_kp.len = 0;
         shell_ui_dirty();
         return COMMIT_STAY;
      }
      /* A SETTER THAT DID NOT COMMIT DOES NOT START A WORKFLOW. The address
       * on screen is the one that was already stored, so retrying the sync
       * against it -- and telling the user it was saved by moving on -- would
       * both be false. Stay on the editor with the failure showing. */
      if (remote_set_email(em) != SETTINGS_OK) {
         set_status("EMAIL NOT SAVED");
         return COMMIT_STAY;
      }
      /* THE ACCOUNT WAS WHAT THE LAST FAILURE WAS ABOUT, often enough: an
       * address typed wrong is refused by the server, and the correction
       * must be tried NOW rather than after the schedule the wrong one
       * earned. The server and port paths get this through
       * remote_drop_identity; the address does not change the server, so it
       * asks directly. */
      remote_retry_now();
      g_kp.len = 0;
      nav_go(SCR_REMOTE);
   } else if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_SERVER) {
      /* SERVER. Lower-cased because the editor can only type upper case and
       * a host name reads wrong shouted. Malformed input refuses VISIBLY --
       * the entry is cleared and the editor stays open -- because silently
       * storing a bad server would point every future sync at nothing. */
      char host[sizeof(((struct prefs *)0)->remote_server)];
      int n = g_kp.len < (int)sizeof host - 1 ? g_kp.len : (int)sizeof host - 1;
      for (int i = 0; i < n; i++) {
         char c  = g_kp.entry[i];
         host[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
      }
      host[n] = 0;
      if (!remote_server_valid(host)) {
         LOGI("server '%s' malformed, not saved", host);
         (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A SERVER NAME");
         g_kp.len = 0;
         shell_ui_dirty();
         return COMMIT_STAY;
      }
      /* NOT SAVED, NOT DONE: forgetting the identity below is irreversible
       * -- it drops the paired key -- and doing it for a server change that
       * did not persist would unpair the phone from the server it is still
       * configured for. */
      if (remote_set_server(host) != SETTINGS_OK) {
         set_status("SERVER NOT SAVED");
         return COMMIT_STAY;
      }
      /* A DIFFERENT SERVER MEANS A DIFFERENT ACCOUNT, so the paired identity
       * goes with the server it belonged to -- and if the settings file
       * refuses the write,
       * it does NOT go and the user has to be told: the phone is configured
       * for one server and still paired to another, and only they can
       * re-pair. Saying nothing would leave a sync failing for a reason the
       * screen never mentioned. */
      if (remote_drop_identity() != IDENTITY_DROPPED)
         set_status("SERVER SAVED, STILL PAIRED: RE-PAIR");
      g_kp.len = 0;
      nav_go(SCR_REMOTE);
   } else if (cur_screen() == SCR_LABEL && g_kp.label_field == LABEL_FOOD) {
      /* A NEW FOOD: added to the vocabulary and CHOSEN in one step.
       *
       * Naming a food is already the act of picking it -- nobody types
       * PORRIDGE in order to then go and find PORRIDGE in a list -- so the
       * commit sets the form's type and returns to the form rather than to
       * the picker it was opened from. nav_go RETURNS to SCR_FOOD because it
       * is already on the path (form_food_action pushed it under the picker),
       * so the keypad and the picker are both discarded in one step and the
       * form's own exit still goes wherever the flow began.
       *
       * food_type_add OWNS the rules -- what a name may contain, what happens
       * to one that is already there, what happens when the vocabulary is
       * full -- and it answers with an id or -1. Restating any of that here
       * would be a second copy of the format's constraints, in a file that
       * has no business knowing them. A refusal keeps the keypad open with
       * the text still in it, because the alternative is silently discarding
       * something the user typed. */
      g_kp.entry[g_kp.len < (int)sizeof g_kp.entry ? g_kp.len : 0] = 0;
      int id = food_type_add(g_kp.entry);
      if (id <= FOOD_TYPE_NONE) {
         set_status("FOOD NAME NOT SAVED");
         return COMMIT_DONE; /* stay put: the text is still on the keypad */
      }
      forms_food_type_set(id);
      g_kp.len = 0;
      nav_go(SCR_FOOD);
   } else if (cur_screen() == SCR_LABEL) {
      /* The registry validates, renames under its own lock and persists --
       * including the blank-name fallback, which is a rule about what a
       * device row must be readable as, not about typing. */
      if (sensor_set_label(sel_device(), g_kp.entry, g_kp.len) != 0)
         set_status("NAME NOT SAVED");
      g_kp.len = 0;
      nav_go(SCR_SENSOR);
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a correction to what the sensor reports.
 * Calibration (a fingerstick the sensor should agree with) and rescale (a true
 * value the app should scale towards) are the two, and they are one family
 * because both are entered in DISPLAY units and both must be converted before
 * anything downstream sees them. */
int kp_commit_correction(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (g_kp.mode == KP_CALIB) { /* entry is in display units */
      if (g_kp.len > 0) {
         /* Conversion and bound live in alarmlogic.c so `make check` can
          * fail on them; this branch only actuates. */
         /* mmol/L is entered as tenths (e.g. "78" = 7.8), so scale back
          * to mg/dL the same way the plot-max entry does. */
         int mgdl = cal_entry_mgdl(g_kp.entry, g_kp.len, sp.units);
         /* Out of range: refuse VISIBLY. Do NOT clamp -- silently
          * altering a calibration value the user typed is worse than not
          * accepting it. A driver-side refusal with only a log line, while
          * the keypad closes and SCR_CAL still shows the PREVIOUS result,
          * makes a rejected entry look exactly like a successful one.
          * Staying on the keypad with the entry cleared
          * is the feedback: nothing was submitted, retype it. Easy to hit
          * in mmol/L (2.2 -> 39 mg/dL). */
         if (mgdl < 0) {
            LOGI("calibration %d mg/dL out of range 40..400, not "
                 "submitted",
                 mgdl);
            char lo2[8];
            char hi2[8];
            fmt_bound(lo2, sizeof lo2, CAL_MIN_MGDL);
            fmt_bound(hi2, sizeof hi2, CAL_MAX_MGDL);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "RANGE IS %s..%s", lo2,
                           hi2);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: the cleared entry IS the
                                 * feedback
                                 */
         }
         /* The single most consequential write in the app, so it happens
          * only here: a digit typed by the user, then an explicit OK. */
         /* Do NOT write yet -- stash the value and show a confirmation.
          * The actual (consequential) calibration write happens only on
          * the explicit CONFIRM (MA_CAL_ENTER). */
         g_kp.len         = 0;
         g_kp.cal_pending = mgdl;
         keypad_close();
         nav_go(SCR_CAL);
      }
   } else if (g_kp.mode == KP_RESCALE) { /* a true glucose value (display
                                   units), like calibration */
      if (g_kp.len > 0) {
         int mgdl = cal_entry_mgdl(g_kp.entry, g_kp.len, sp.units);
         if (mgdl < 0) {
            LOGI("rescale %d mg/dL out of range, not submitted", mgdl);
            char lo3[8];
            char hi3[8];
            fmt_bound(lo3, sizeof lo3, CAL_MIN_MGDL);
            fmt_bound(hi3, sizeof hi3, CAL_MAX_MGDL);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "RANGE IS %s..%s", lo3,
                           hi3);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: cleared entry is the
                                 * feedback
                                 */
         }
         g_kp.len     = 0;
         g_kp.rescale = mgdl; /* factor computed on CONFIRM */
         keypad_close();
         nav_go(SCR_RESCALE);
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: an alarm threshold. Four modes -- low, high, and
 * the two nudge levels -- and they are one family because of the ordering rule
 * between them: a low above a high is not a threshold, it is an alarm that can
 * never stop, so each commit has to re-validate the SET rather than its own
 * field. */
int kp_commit_thresholds(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (kp_is_thresh(g_kp.mode)) {
      /* ALARM LOW / HIGH and NUDGE LOW / HIGH: entry in
       * DISPLAY units. mg/dL is a plain integer; mmol/L is LITERAL mmol
       * with an optional '.' and one decimal digit ("5.5") -- its keypad
       * shows a dot key (keypad.h).
       * All four accept 0..AL_ENTRY_MAX: 0 parks LOW below any possible
       * reading and a past-the-scale HIGH above any, each threshold's
       * deliberate OFF switch. Refuse VISIBLY (stay on the keypad, entry
       * cleared) a malformed entry, an out-of-range value, or one that
       * would invert ITS OWN pair -- a silent clamp would move a
       * threshold the user never typed. Equal is allowed.
       *
       * The two pairs are checked against THEMSELVES only, never against
       * each other. A nudge inside the alarm band is pointless but
       * harmless (nudge_fire suppresses it under the alarm), and refusing
       * the entry would block the legitimate order of operations -- move
       * the nudge first, then the alarm -- for no safety gain. */
      if (g_kp.len > 0) {
         /* WHICH pair, and WHICH end -- asked of the mode rather than
          * derived from its number. `>= KP_NUDGE_LOW` and `% 2` were correct
          * and silent, and they made the enum's ORDER a fact two files
          * depended on without saying so. */
         int isnudge = kp_is_nudge(g_kp.mode);
         int islow   = kp_is_low(g_kp.mode);
         int ip      = 0;
         int fd      = 0;
         int dot     = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
         int bad     = 0;
         for (int i = 0; i < g_kp.len; i++) {
            char ch = g_kp.entry[i];
            if (ch == '.') {
               if (dot || !sp.units)
                  bad = 1; /* one dot, and only in mmol/L mode */
               else
                  dot = 1;
            } else if (dot == 0) {
               ip = (ip * 10) + (ch - '0');
            } else if (dot == 1) {
               fd  = ch - '0';
               dot = 2;
            } else {
               bad = 1; /* a second decimal digit: not representable */
            }
         }
         int mgdl = sp.units ? (((ip * 10) + fd) * 18) / 10 : ip;
         /* The RULES are alarm_set_threshold's (see there): it reads the
          * partner threshold and stores this one in ONE critical section, so
          * the pair it approved is the pair it writes. This branch's job is
          * only to say WHY when it refuses. */
         int why = bad ? -1 : alarm_set_threshold(isnudge, islow, mgdl);
         /* STORED BUT NOT WRITTEN is not a refusal: the value is live now,
          * and it is the stored one again after the next launch. Say so and
          * close, rather than clearing the entry as though it were
          * rejected -- retyping it would not help. */
         if (why == TH_NOT_SAVED) {
            set_status("THRESHOLD NOT SAVED");
            g_kp.len = 0;
            keypad_close();
            return COMMIT_DONE;
         }
         if (why != TH_OK) {
            LOGI("%s %s %d mg/dL refused (0..%d, low<=high)",
                 isnudge ? "nudge" : "alarm", islow ? "low" : "high", mgdl,
                 AL_ENTRY_MAX);
            /* THREE different refusals, THREE sentences.
             *
             * Answering 1500 with "HIGH MUST BE >= LOW" names the wrong
             * rule and sends the user to change the other number. Say
             * which rule was broken, and say it in the units the keypad is
             * accepting. */
            char bnd[8];
            fmt_bound(bnd, sizeof bnd, AL_ENTRY_MAX);
            if (bad)
               (void)snprintf(g_kp.err, sizeof g_kp.err,
                              sp.units ? "USE ONE DECIMAL, LIKE 5.5"
                                       : "DIGITS ONLY");
            else if (why == TH_TOO_BIG)
               (void)snprintf(g_kp.err, sizeof g_kp.err, "MAX IS %s", bnd);
            else if (why == TH_HIGH_ZERO)
               (void)snprintf(g_kp.err, sizeof g_kp.err, "HIGH CANNOT BE ZERO");
            else
               (void)snprintf(g_kp.err, sizeof g_kp.err,
                              islow ? "LOW MUST BE <= HIGH"
                                    : "HIGH MUST BE >= LOW");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* cleared entry is the feedback; retype it */
         }
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a plain quantity with a range. Plot maximum, the
 * server's pairing code, a port, a weight, a dose, a year. Nothing here
 * changes what the app DOES -- each one parses, bounds-checks, stores, and
 * returns to wherever the keypad was opened from. */
int kp_commit_number(void)
{
   struct prefs sp;
   settings_get(&sp);
   if (g_kp.mode == KP_PLOT_MAX) { /* entry is in the display unit */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* TENTHS of mmol/L, matching how the row is DISPLAYED: the
          * renderer draws plot max through fmt_glu, which prints one decimal in
          * mmol mode (300 mg/dL shows as "16.7"). Treating the entry as
          * whole mmol made the shown value impossible to re-enter --
          * typing 167 gave 3006 mg/dL (silently clamped to 400) and
          * typing 16 gave 288, not 300. The calibration entry below
          * already scales this way; this is the one that disagreed with
          * its own display. */
         /* The clamp and the renderer's scale belong to the setting, not to
          * the keypad that types it -- settings_load applies the same two. */
         if (settings_set_plot_max(sp.units ? (v * 18) / 10 : v) != SETTINGS_OK)
            set_status("PLOT SCALE NOT SAVED");
         keypad_close();
         /* the notification plot shares this vertical scale; without a
          * refresh it keeps the previous scale until the next datapoint */
         notify_mark();
         notify_tick();
      }
   } else if (g_kp.mode == KP_SYNC_CODE) { /* the server's 6 digits */
      if (g_kp.len == 6) {
         char code[8];
         for (int i = 0; i < 6; i++)
            code[i] = g_kp.entry[i];
         code[6] = 0;
         /* Handed to Java's worker: pairing is four round trips and must
          * not run on the UI thread. The result arrives as a changed
          * PAIRED row, because the only thing the user can do about a
          * failure is ask the server for a fresh code. */
         /* THE ACCOUNT AND THE ENDPOINT AS ONE VALUE. A pairing aimed at a
          * server the user has just changed, carrying the account they had
          * before it, fails with nothing on screen to explain which half was
          * stale. */
         struct remote_config rc;
         remote_config_get(&rc);
         syncjni_pair_request(rc.email, code);
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_PORT) { /* 1..65535 */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0'); /* max 5 digits: no wrap */
         if (v < 1 || v > 65535) {
            LOGI("remote port %d out of range, not saved", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "PORT MUST BE 1..65535");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay on the keypad: cleared entry is the
                                 * feedback
                                 */
         }
         /* range checked just above, so a refusal here is the file */
         /* Same rule as the server name: the identity is dropped below, and
          * a port that did not persist must not cost the pairing. */
         if (remote_set_port(v) != SETTINGS_OK) {
            set_status("PORT NOT SAVED");
            g_kp.len = 0;
            return COMMIT_STAY;
         }
         /* Possibly a different server, so the identity goes -- and a
          * refusal is said out loud, for the reason the server case gives. */
         if (remote_drop_identity() != IDENTITY_DROPPED)
            set_status("PORT SAVED, STILL PAIRED: RE-PAIR");
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_FOOD_G) { /* whole grams, no decimal point */
      if (g_kp.len > 0) {
         long v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* REFUSED VISIBLY, NEVER CLAMPED. Silently altering a number the
          * user typed is the trap the calibration entry above spells out: the
          * value stored is then one they did not enter and cannot reproduce.
          * The bounds are food.h's, not restated here -- what this branch
          * knows is that a portion outside them is not a portion.
          *
          * The keypad has no '.' key in this mode (kp_info), so there is no
          * fractional case to reject: grams are whole, which is the
          * resolution the format stores. */
         if (v < FOOD_MIN_G || v > FOOD_MAX_G) {
            (void)snprintf(g_kp.err, sizeof g_kp.err, "GRAMS OUT OF RANGE");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         form_food_set_grams((int)v);
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_EX_DUR) { /* whole minutes, no decimal point */
      if (g_kp.len > 0) {
         long v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* MINUTES IN, SECONDS STORED: the column is seconds (exercise.h), and
          * this is the one place the two units meet. Refused rather than
          * clamped, for the reason the grams branch above gives.
          *
          * ZERO IS ACCEPTED, and it is not "no exercise": it is the log's own
          * "how long is not known", which is what an open session reads as and
          * what a user who mistyped a duration needs a way back to. */
         long *dur = form_ex_duration();
         if (!dur) {
            (void)snprintf(g_kp.err, sizeof g_kp.err, "NO ENTRY OPEN");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         if (v < 0 || v > EX_DUR_MAX / 60) {
            (void)snprintf(g_kp.err, sizeof g_kp.err, "MINUTES OUT OF RANGE");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         *dur     = v * 60;
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_WEIGHT) { /* "162" or "162.4" */
      if (g_kp.len > 0) {
         /* THE DIGITS ARE THE WHOLE NUMBER, with an optional '.' and one
          * decimal -- exactly the alarm-threshold entry's shape.
          *
          * NOT tenths, under which "162" means 16.2 lb: below the
          * minimum, refused, entry cleared, and the only way to enter 162 is
          * to type "1620". Nobody would. An entry form has to accept the
          * number as it is spoken and as the row displays it. */
         int ip  = 0;
         int fd  = 0;
         int dot = 0; /* 0 none, 1 seen, 2 decimal digit consumed */
         int bad = 0;
         for (int i = 0; i < g_kp.len; i++) {
            char ch = g_kp.entry[i];
            if (ch == '.') {
               if (dot)
                  bad = 1; /* one dot only */
               else
                  dot = 1;
            } else if (dot == 0) {
               ip = (ip * 10) + (ch - '0');
            } else if (dot == 1) {
               fd  = ch - '0';
               dot = 2;
            } else {
               bad = 1; /* a second decimal digit: not representable */
            }
         }
         int tenths = (ip * 10) + fd;
         /* Validate by CONVERTING: wt_from_tenths returns 0 outside the
          * stored range, so an impossible weight is refused VISIBLY
          * rather than silently clamped into the log. */
         if (bad || ip > 999 || wt_from_tenths(tenths, sp.wunits) <= 0) {
            LOGI("weight %d.%d %s refused (out of range)", tenths / 10,
                 tenths % 10, wt_unit_name(sp.wunits));
            (void)snprintf(g_kp.err, sizeof g_kp.err, "WEIGHT OUT OF RANGE");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay: the cleared entry is the feedback */
         }
         form_wt_set_tenths(tenths);
         g_kp.len = 0;
         keypad_close();
      }
   } else if (g_kp.mode == KP_INS_UNITS) { /* 1..99 */
      if (g_kp.len > 0) {
         int v = 0;
         for (int i = 0; i < g_kp.len; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         if (v < INS_UNITS_MIN || v > INS_UNITS_MAX) {
            LOGI("insulin %d units out of range, not saved", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err, "UNITS MUST BE %d..%d",
                           INS_UNITS_MIN, INS_UNITS_MAX);
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY; /* stay: cleared entry is the refusal */
         }
         form_ins_set_units(v);
         g_kp.len = 0;
         keypad_close();
      }
   } else if (kp_is_year(g_kp.mode)) {
      if (g_kp.len == 4) {
         int v = 0;
         for (int i = 0; i < 4; i++)
            v = (v * 10) + (g_kp.entry[i] - '0');
         /* WHICH INSTANT, from the MODE. This read the return screen --
          * `g_kp.ret == SCR_WEIGHT` -- which is a second, unstated
          * protocol: the keypad's return can be re-aimed (menu.c does it for
          * PLOT MAX), and a typed year would then have moved the other
          * form's timestamp. */
         long *tp = form_instant_of(g_kp.mode);
         if (!tp)
            return COMMIT_PASS; /* not a form field: nothing to move */
         /* a dose belongs to a human timescale; refuse typo years */
         if (v < 2000 || v > 2199) {
            LOGI("year %d refused", v);
            (void)snprintf(g_kp.err, sizeof g_kp.err,
                           "YEAR MUST BE 2000..2199");
            g_kp.len = 0;
            shell_ui_dirty();
            return COMMIT_STAY;
         }
         /* Month, day and time of day are kept; Feb 29 out of a non-leap year
          * is clamped to the 28th (civil.h). A year change is the edit most
          * likely to cross a transition -- the same civil date half a year
          * away is a different offset -- so the recombination is the target
          * year's, never today's. */
         *tp      = civil_reaim(*tp, CIVIL_EDIT_YEAR, v, 0, form_zone, 0).t;
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* WHAT A TYPED NUMBER MEANT: a date or a time. Two modes and one family,
 * because they are the two halves of a single entry -- MMDD, then HHMM -- and
 * validating the second depends on the first having been accepted. */
int kp_commit_datetime(void)
{
   if (kp_is_date(g_kp.mode) || kp_is_time(g_kp.mode)) {
      if (g_kp.len == 4) {
         int a = ((g_kp.entry[0] - '0') * 10) + (g_kp.entry[1] - '0');
         int b = ((g_kp.entry[2] - '0') * 10) + (g_kp.entry[3] - '0');
         /* whichever form's field this MODE is -- see the year entry above */
         long *tp = form_instant_of(g_kp.mode);
         if (!tp)
            return COMMIT_PASS;       /* not a form field: nothing to move */
         if (kp_is_date(g_kp.mode)) { /* MMDD, within the current year */
            /* THE YEAR THE INSTANT IS IN, read in the offset in force at that
             * instant, because February's length depends on it. Reading it in
             * today's offset can name the wrong DAY (and, on New Year's Eve,
             * the wrong year), which is the same defect one step earlier. */
            long yy = 0;
            long mm = 0;
            long dd = 0;
            civil_at(*tp, form_zone, 0, &yy, &mm, &dd);
            static const int mdl[12] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
            int leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
            int md   = (a >= 1 && a <= 12) ? mdl[a - 1] + (a == 2 && leap) : 0;
            if (a < 1 || a > 12 || b < 1 || b > md) {
               LOGI("date %02d%02d refused", a, b);
               (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A DATE (MMDD)");
               g_kp.len = 0;
               shell_ui_dirty();
               return COMMIT_STAY; /* invalid date: stay, entry cleared */
            }
            *tp = civil_reaim(*tp, CIVIL_EDIT_MONTHDAY, a, b, form_zone, 0).t;
         } else { /* HHMM: keep the civil date, set the time of day */
            if (a > 23 || b > 59) {
               LOGI("time %02d%02d refused", a, b);
               (void)snprintf(g_kp.err, sizeof g_kp.err, "NOT A TIME (HHMM)");
               g_kp.len = 0;
               shell_ui_dirty();
               return COMMIT_STAY; /* invalid time: stay, entry cleared */
            }
            /* A time typed into the skipped hour is moved forward rather than
             * refused, and the form redisplays what was stored -- 0230
             * becomes 0330 on screen. See civil.h for why that beats a
             * refusal the user cannot act on. */
            *tp = civil_reaim(*tp, CIVIL_EDIT_TIME, a, b, form_zone, 0).t;
         }
         g_kp.len = 0;
         keypad_close();
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

void forms_kp_open(enum keypad_mode mode, enum ui_screen ret)
{
   g_kp.mode = mode;
   g_kp.ret  = ret;
   forms_kp_clear();
}

void forms_kp_return_set(enum ui_screen ret)
{
   g_kp.ret = ret;
}

enum ui_screen forms_kp_return(void)
{
   return g_kp.ret;
}

void forms_kp_clear(void)
{
   g_kp.len    = 0;
   g_kp.err[0] = 0; /* opening, or starting again, is not a refusal */
}

void forms_kp_seed(const char *text)
{
   forms_kp_clear();
   for (int i = 0; text && text[i] && g_kp.len < (int)sizeof g_kp.entry - 1;
        i++)
      g_kp.entry[g_kp.len++] = text[i];
}

void forms_kp_type(char c)
{
   if (g_kp.len < (int)sizeof g_kp.entry - 1)
      g_kp.entry[g_kp.len++] = c;
}

int forms_kp_len(void)
{
   return g_kp.len;
}

void forms_kp_text(char *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return;
   for (; n < g_kp.len && n < cap - 1; n++)
      out[n] = g_kp.entry[n];
   out[n] = 0;
}

void forms_set_label_field(int field)
{
   g_kp.label_field = field;
}

void forms_set_markpick(int ins_type)
{
   g_kp.markpick_ins = ins_type;
}

enum keypad_mode forms_kp_mode(void)
{
   return g_kp.mode;
}

void forms_kp_del(void)
{
   g_kp.err[0] = 0; /* a correction retires the last refusal */
   if (g_kp.len > 0)
      g_kp.len--;
}

void forms_kp_err_clear(void)
{
   g_kp.err[0] = 0;
}

int forms_kp_has(char c)
{
   for (int i = 0; i < g_kp.len; i++)
      if (g_kp.entry[i] == c)
         return 1;
   return 0;
}

int forms_markpick(void)
{
   return g_kp.markpick_ins;
}

void forms_kp_mode_set(enum keypad_mode mode)
{
   g_kp.mode = mode;
}

void forms_set_rescale_entry(int tenths)
{
   g_kp.rescale = tenths;
}

int forms_rescale_entry(void)
{
   return g_kp.rescale;
}

void forms_set_cal_pending(int mgdl)
{
   g_kp.cal_pending = mgdl;
}

int forms_cal_pending(void)
{
   return g_kp.cal_pending;
}

static int g_scrub = -1;

void forms_set_scrub(int idx)
{
   g_scrub = idx;
}

int forms_scrub(void)
{
   return g_scrub;
}

/* ---- ONE TAP, TRIED AGAINST EACH WORKFLOW IN TURN ---------------------
 *
 * The order is the order these were `||`-ed together in menu_action, and it
 * is preserved rather than rearranged: a few actions overlap on purpose (a
 * keypad digit and a device pick are both "a number the user touched") and
 * the first match has always won. What changes is WHERE the order lives --
 * beside the workflows it orders, in the file that owns the keypad they
 * share, rather than in the menu that merely forwards a tap. */
int forms_action(int action, int ix)
{
   return form_ins_action(action, ix) || form_wt_action(action, ix) ||
          form_food_action(action, ix) || form_ex_action(action, ix);
}

void forms_view_get(struct forms_view *out)
{
   if (!out)
      return;
   /* the keypad */
   out->kp_mode  = g_kp.mode;
   out->entrylen = g_kp.len;
   str_snapshot(out->entry, sizeof out->entry, g_kp.entry);
   str_snapshot(out->kp_err, sizeof out->kp_err, g_kp.err);
   out->markpick_ins = g_kp.markpick_ins;
   /* EACH WORKFLOW FILLS ITS OWN FIELDS, from its own file. That is what
    * makes a field in this struct traceable to one writer -- and it is the
    * whole reason the drafts are not in scope of each other any more. */
   form_ins_view(out);
   form_wt_view(out);
   form_food_view(out);
   form_ex_view(out);
   out->scrub = g_scrub;
   /* the odds and ends */
   out->label_field   = g_kp.label_field;
   out->rescale_entry = g_kp.rescale;
   out->cal_pending   = g_kp.cal_pending;
}
