// SPDX-License-Identifier: GPL-3.0
// alarmcfg.c --- the two threshold pairs, and the file they live in
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN. Five unrelated files -- the device's model and
 * firmware, the alarm thresholds, the display preferences, the pairing code
 * and the remote credentials -- behind one save engine is 1541 lines of
 * module with no subject. They share the engine
 * (app/setpriv.h) and the preferences aggregate; they share nothing else, and
 * a reader after one of them had to read past the other four.
 */
#include "alarmcfg.h"
#include "alarmlogic.h" /* AL_ENTRY_MAX: the load bound = the keypad's */
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"
#include "setpriv.h"
#include "settings.h" /* struct prefs: the aggregate the engine holds */
#include "thread.h"   /* the lock this module's state sits behind */
#include "util.h"
#include <stdio.h>

int alarm_set_thresholds(int alarm_low, int alarm_high, int nudge_low,
                         int nudge_high)
{
   /* ALL FOUR AT ONCE, AND PERSISTED HERE. Four one-field setters, storing
    * without saving and leaving the ordering check and the alarm_save() to
    * whoever calls them, are three separate obligations in a header, on the
    * pair of numbers that decides whether a hypo alarm can fire. A caller
    * that meets two of the three leaves the phone with a live threshold the
    * next launch will not have.
    *
    * The ordering is still the ALARM's to decide (it needs its own lock to
    * read the partner and choose atomically); what is no longer possible is
    * storing one half, or storing both and forgetting the file. */
   struct save_job j;
   mutex_lock(&set_lk);
   int old_al     = g_p.alarm_low;
   int old_ah     = g_p.alarm_high;
   int old_nl     = g_p.nudge_low;
   int old_nh     = g_p.nudge_high;
   g_p.alarm_low  = alarm_low;
   g_p.alarm_high = alarm_high;
   g_p.nudge_low  = nudge_low;
   g_p.nudge_high = nudge_high;
   set_render_alarm(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) { /* see set_int */
         g_p.alarm_low  = old_al;
         g_p.alarm_high = old_ah;
         g_p.nudge_low  = old_nl;
         g_p.nudge_high = old_nh;
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* One insulin type's plot styling: up to THREE fields, so all three come back
 * if the file cannot be replaced. Two of them restored and one left is a
 * marker the user did not choose, in a colour they did not choose either. */
void set_render_alarm(struct save_job *j)
{
   static unsigned written;
   int n = snprintf(j->buf, sizeof j->buf, "%d %d %d %d\n", g_p.alarm_low,
                    g_p.alarm_high, g_p.nudge_low, g_p.nudge_high);
   set_job_stamp(j, g_alarm_path, &written, n, n > 0 && n < 48);
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

enum load_result alarm_load(void)
{
   /* 256, matching what alarm_save writes. The number is this file's own and
    * has no other source: the alarm line is four small integers, and nothing
    * about remote.cfg's growth bears on it. */
   char b[256];
   int n               = 0;
   enum load_result rr = read_file_exact(g_alarm_path, b, sizeof b, &n);
   if (rr != LOAD_OK)
      return rr;
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
      g_p.alarm_low  = lo;
      g_p.alarm_high = hi;
   }
   /* The nudge pair is committed SEPARATELY and by the same rules, never
    * cross-checked against the alarm pair. A nudge inside the alarm band is
    * pointless but harmless (the alarm suppresses it), while refusing to load
    * it would silently revert a threshold the user chose -- and this file's
    * whole reason for existing is that a threshold the user believes is armed
    * must actually be armed. */
   if (got_nlo && got_nhi && nlo <= AL_ENTRY_MAX && nhi <= AL_ENTRY_MAX &&
       nlo <= nhi) {
      g_p.nudge_low  = nlo;
      g_p.nudge_high = nhi;
   }
   return LOAD_OK;
}
