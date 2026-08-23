// SPDX-License-Identifier: GPL-3.0
// shellstate.c --- what survives an activity or process recreation
// Copyright 2026 Jakob Kastelic

/* SPLIT OUT OF app/main.c, and along the seam the file already had.
 *
 * main.c is the shell: the looper, the window, the activity callbacks, the
 * tick. This is the 645 lines that do something else entirely -- deciding
 * which screens and drafts are safe to carry across a process death,
 * serialising them into Android's saved-state buffer, and validating whatever
 * comes back. One workflow, with its own vocabulary and its own failure
 * modes, and the only two things the shell needs from it are "give me a blob"
 * and "here is a blob".
 *
 * It came out because main.c passed the 2000-line ceiling the build enforces,
 * and it is the piece that came out because it is the piece that was already
 * separate: it touches none of the shell's globals -- no window, no looper, no
 * atomics -- and reaches only through nav.h, forms.h and the model headers.
 * The extraction moved lines and changed none of them.
 *
 * The long comment below is the original one, kept whole: it is the record of
 * why this exists at all and what was lost before it did. */
#include "shellstate.h"
#include "insrow.h"     /* INS_*: what a dose row can say */
#include "statecodec.h" /* the blob's shape and its codec: item 306 */

#include "forms.h" /* the drafts a recreation must not silently discard */
#include "insulin.h"
#include "keypad.h"     /* enum keypad_mode: which drafts may be restored */
#include "loadresult.h" /* the four answers a persisted blob can give */
#include "log.h"
#include "nav.h"
#include "ndk.h"
#include "settings.h"
#include "uimodel.h"
#include "weight.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==== WHAT SURVIVES AN ACTIVITY OR PROCESS RECREATION ==================
 *
 * WHAT THE USER SAW. Android hands a native activity a saved-state buffer at
 * onCreate and asks for one back through onSaveInstanceState. This file took
 * neither: the two parameters were `(void)saved; (void)saved_size;` and no
 * onSaveInstanceState callback was ever installed, so the framework had
 * nothing to keep and nothing to give back. The whole of the shell's screen
 * state -- which screen is open, the route taken to reach it, and the digits
 * half typed into a form -- lived only in this process's memory.
 *
 * That memory goes away more often than it sounds. The activity declares
 * configChanges for orientation, so a rotation does NOT destroy it; what does
 * is the phone reclaiming the app under memory pressure, which is routine on
 * a device with a camera app and a browser open, and which Android hides
 * completely: the task stays in the recents list and reopening it looks like
 * resuming. A person who had typed 14 units of insulin, been interrupted by a
 * call, and come back finds the LOG INSULIN form gone and the main screen in
 * its place -- with no message, because nothing knows anything was lost. The
 * next thing they do is retype it or, worse, assume they already logged it.
 *
 * SO: A SNAPSHOT. Three properties, and none of them is decoration.
 *
 * BOUNDED. The blob travels in the activity's saved-state Bundle, which
 * crosses a Binder transaction shared with everything else the framework is
 * saving at that moment -- and a transaction over the (roughly 1 MB, shared,
 * undocumented) limit does not degrade, it throws TransactionTooLargeException
 * and takes the app with it. This state is a handful of small integers and
 * one 63-character field: the largest snapshot this build can produce is a
 * twelve-deep route (12 * 3 bytes), two instants (2 * 12), six small numbers,
 * the marker, and a 63-byte entry -- a little over 150 bytes. STATE_MAX is
 * 256, comfortably above that and far enough below the transaction limit that
 * this state can never be the reason a save fails. It is also the size of the
 * stack buffer the decoder copies into, which is the other half of "bounded":
 * the length comes from the framework, so a decoder that trusted it would be
 * taking a memcpy size from outside this process.
 *
 * VERSIONED, in the vocabulary settings.c already uses: a `v<N> ` marker at
 * the head. An older build's blob and a newer build's blob are both REFUSED
 * WHOLE rather than read as far as they parse. Reading a v2 blob with a v1
 * parser is not a partial restore, it is a field-order mismatch -- the route
 * read as a keypad mode, the keypad mode read as an instant -- and the
 * failure arrives as the app opening on the wrong screen with somebody's dose
 * in the weight field. There is no upgrade path here and there should not be:
 * the cost of refusing is one lost draft on the launch after an update.
 *
 * VALIDATED AFTER THE DURABLE DATA LOADS, which is why state_restore is
 * called below init_data rather than from ANativeActivity_onCreate's first
 * lines. Two of the checks cannot be made any earlier:
 *
 *   - the DISPLAY UNITS. The weight draft is held in tenths of the display
 *     unit (see forms.c), so the digits "1624" mean 162.4 lb or 162.4 kg
 *     depending on a preference that lives in settings.cfg. Restore them
 *     under the other unit and the user confirms a weight wrong by a factor
 *     of 2.2 -- silently, because the number on screen is the one they typed.
 *     The glucose unit governs every threshold keypad the same way. So the
 *     snapshot records both units and the whole blob is refused if either has
 *     changed, which needs settings_load() to have run;
 *   - the ROUTE. A screen is restorable only if it does not stand for a
 *     THING: SCR_SENSOR, SCR_CAL, SCR_FORGET and their neighbours are all
 *     about whichever device sel_device() points at, and that selection is a
 *     tap, not a stored fact. Restoring one of those puts the user in front
 *     of a confirmation dialog about a sensor the app can no longer identify
 *     -- which is worse, not better, than opening on the main screen. The
 *     list below is the whitelist, and its default is REFUSE.
 *
 * The vocabulary is app/loadresult.h's, so a refused blob reads the same way
 * as a refused settings file: ABSENT is a normal cold start and says nothing;
 * CORRUPT is a blob that exists and cannot be trusted.
 */
/* THE BLOB'S SHAPE AND ITS CODEC ARE IN app/statecodec.h, which
 * is included above. They were here, and static, so the only way to test the
 * parsing -- the most delicate code in this file -- was to compile this whole
 * unit into the test. */

/* WHICH SCREENS MAY BE RESTORED AT ALL.
 *
 * The rule is one question: does this screen stand for a THING the app would
 * have to identify again? Every screen below is either a view of durable data
 * (the logs, the device list, the export panel) or a settings page, so it
 * means the same thing in a fresh process as it did in the one that died.
 *
 * Everything else is refused, and the asymmetry is why: the cost of refusing
 * a route is landing on the main screen; the cost of restoring a bad one is a
 * CONFIRM button about a device that is not there.
 *
 * SCR_GATE is excluded deliberately and not by oversight. It is not a place
 * the user navigated to -- it is computed at every onCreate from whether the
 * BLE permissions are actually held -- so a stored one would either duplicate
 * that answer or contradict it.
 *
 * EVERY SCREEN IS NAMED, including the ones that answer no, and the compiler
 * insists: the app is built with -Wswitch-enum, so a screen added to the enum
 * and not to this list is a build failure rather than a screen that quietly
 * became restorable, or quietly did not. The `default` underneath is still
 * load-bearing -- state_decode casts a number that arrived from outside this
 * process into this enum, and that number need not be one of the names
 * above. */
static int scr_restorable(enum ui_screen s)
{
   switch (s) {
      /* Views of durable data, and settings pages. Each means the same thing
       * on a fresh process as it did on the one that was killed. */
      case SCR_MAIN:
      case SCR_SETTINGS:
      case SCR_DISPLAY:
      case SCR_ALARM:
      case SCR_REMOTE:
      case SCR_PERMS:
      case SCR_DEVICES:
      case SCR_ADDMENU:
      case SCR_INSLOG:
      case SCR_WTLOG:
      /* A view of durable data, like the other two logs. */
      case SCR_FOODLOG:
      /* ...and the fourth, on the same terms. */
      case SCR_EXLOG:
      case SCR_EXPORT:
      case SCR_INSULIN:
      case SCR_WEIGHT:
      case SCR_FOOD:
      case SCR_KEYPAD: return 1;
      /* SCREENS ABOUT A DEVICE. Every one of these reads sel_device(), which
       * is a tap and not a stored fact, so restored they are panels and
       * confirmations about a sensor this process cannot name. */
      case SCR_DEVLIST:
      case SCR_SENSOR:
      case SCR_CAL:
      case SCR_CALPEND:
      case SCR_RESCALE:
      case SCR_RESCALEACT:
      case SCR_SENSTYPE:
      case SCR_FORGET:
      case SCR_LABEL:
      case SCR_MARKPICK:
      case SCR_COLORPICK:
      case SCR_METERHELP:
      case SCR_PAIRCONF:
      /* The armed pairing it asks about is held in memory only, so a
       * restored one is a STOP button over a wait that is already over. */
      case SCR_PENDCANCEL:
      case SCR_OLDDEV:
      case SCR_RECONF:
      /* THE FOOD PICKER, for the same reason as the confirmations below: what
       * it does on the way out is return to a RECORDED origin and, from the
       * entry form's side, hand back a chosen type. Neither the origin nor the
       * draft survives the process, so a restored picker is a list whose exit
       * leads nowhere in particular and whose choice lands in a form that was
       * never opened. The entry form itself (SCR_FOOD) is restorable in the
       * same sense SCR_INSULIN and SCR_WEIGHT are -- an empty form is a
       * coherent thing to come back to. */
      case SCR_FOODTYPE:
      /* CONFIRMATIONS about a row of a log, held in a draft that is not
       * restored (see forms.h) -- a YES with nothing behind it. */
      case SCR_WTDEL:
      case SCR_INSDEL:
      /* Same reason: it names a row held in a draft that is not restored. */
      case SCR_FOODDEL:
      case SCR_EXDEL:
      /* THE EXERCISE CORRECTION FORM, and it is NOT like SCR_FOOD or
       * SCR_WEIGHT. Those restore to an empty form, which is a coherent thing
       * to come back to because a new entry can be typed into one. This form
       * only ever EDITS a row, and the row it edits lives in a draft that does
       * not survive the process -- so a restored one is a CONFIRM aimed at
       * nothing, on a screen that looks like it is about a record. */
      case SCR_EXEDIT:
      case SCR_SYNCRESTORE:
      /* Computed from the permissions actually held, at every onCreate. */
      case SCR_GATE:
      /* Not a screen. */
      case SCR_N: return 0;
      default: return 0;
   }
}

/* WHICH KEYPAD FIELDS MAY BE RESTORED.
 *
 * THE TWO REFUSALS ARE THE POINT. KP_PAIR_CODE is the code printed on a
 * sensor's applicator and KP_SYNC_CODE is the one the server shows for
 * claiming an account; both are shared secrets, and this snapshot is written
 * into a Bundle that leaves this process, is held by system_server, and on
 * some configurations is written to disk as part of the task's saved state. A
 * half-typed pairing code has no business being there, and the convenience of
 * not retyping four digits does not begin to pay for it.
 *
 * KP_CALIB and KP_RESCALE are on the list and are unreachable in practice for
 * a different reason, which costs nothing to allow: they are only ever opened
 * from SCR_CAL / SCR_RESCALE, which are not restorable screens, so any route
 * holding one is truncated before the keypad. This list says what the FIELD
 * is, not what the route allows; both have to agree.
 *
 * KP_NONE is refused here on purpose: "no field" is not something to restore
 * a keypad onto, and the encoder never puts SCR_KEYPAD in a saved route
 * without a real mode beside it.
 *
 * EVERY MODE IS NAMED, for the reason scr_restorable gives: -Wswitch-enum
 * turns a mode added to keypad.h and forgotten here into a compile error
 * rather than into a field that silently started, or stopped, being carried
 * across a process death. */
static int kp_restorable(enum keypad_mode m)
{
   switch (m) {
      case KP_PLOT_MAX:
      case KP_CALIB:
      case KP_RESCALE:
      case KP_PORT:
      case KP_INS_UNITS:
      case KP_DATE:
      case KP_TIME:
      case KP_YEAR:
      case KP_ALARM_LOW:
      case KP_ALARM_HIGH:
      case KP_NUDGE_LOW:
      case KP_NUDGE_HIGH:
      case KP_WEIGHT:
      case KP_WT_DATE:
      case KP_WT_TIME:
      case KP_WT_YEAR:
      /* The LOG FOOD form's fields, on the same footing as the weight form's:
       * a portion and a civil instant, neither of them a secret. */
      case KP_FOOD_G:
      case KP_FOOD_DATE:
      case KP_FOOD_TIME:
      case KP_FOOD_YEAR: return 1;
      /* THE EXERCISE FORM'S FIELDS ARE NOT RESTORABLE, and this is the one
       * place they differ from the food form's.
       *
       * A restorable keypad has to have something to return TO. The other
       * forms restore to an empty form and a half-typed date lands in it
       * harmlessly. This keypad returns to SCR_EXEDIT, which scr_restorable
       * refuses -- it edits a row held in a draft that does not survive the
       * process. Keeping the field would put the keypad on a route whose next
       * screen down is truncated away. The duration is on exactly the same
       * footing as the three calendar fields. */
      case KP_EX_DATE:
      case KP_EX_TIME:
      case KP_EX_YEAR:
      case KP_EX_DUR: return 0;

      /* SECRETS. Never written into somebody else's process. */
      case KP_PAIR_CODE:
      case KP_SYNC_CODE:
      /* Not a field: see above. */
      case KP_NONE:
      /* Not a count. */
      case KP_NMODES: return 0;
      default: return 0;
   }
}

/* MAY THIS SCREEN GO INTO THE SNAPSHOT, GIVEN WHAT IS OPEN ON IT.
 *
 * scr_restorable asks a question about the screen alone. This asks it about
 * the screen AND the draft sitting on it, which is where the two
 * edit-in-progress cases live: a LOG WEIGHT or LOG INSULIN form that is
 * amending an existing row carries a copy of that row as its match key, and
 * the row may be gone by the time this comes back (see forms.h). Rather than
 * restore the form without the thing it is editing -- a screen that says EDIT
 * and would silently create a new entry -- the route is truncated before it,
 * so the user lands on whatever they had open underneath.
 *
 * The keypad is the same shape: a route may keep SCR_KEYPAD only if the field
 * it is collecting is one this build is willing to store. */
static int scr_saveable(enum ui_screen s, const struct forms_view *fv)
{
   if (!scr_restorable(s))
      return 0;
   if (s == SCR_WEIGHT && fv->wt_edit >= 0)
      return 0;
   if (s == SCR_INSULIN && fv->ins_edit >= 0)
      return 0;
   if (s == SCR_KEYPAD && !kp_restorable(fv->kp_mode))
      return 0;
   return 1;
}

/* Is `s` on the saved route? */
static int state_path_has(const struct saved_state *st, enum ui_screen s)
{
   for (int i = 0; i < st->n; i++)
      if (st->path[i] == s)
         return 1;
   return 0;
}

/* ---- THE PARSER -------------------------------------------------------
 *
 * Hand-written, in settings.c's idiom and for its reason: this reads a buffer
 * produced outside this process, so every step either consumes exactly what
 * it expects or refuses. A digit run longer than any legal value is REFUSED
 * rather than folded, because a folded number is a plausible-looking wrong
 * one. */
static int st_num(char **q, long *out)
{
   char *p = *q;
   while (*p == ' ')
      p++;
   int neg = 0;
   if (*p == '-') {
      neg = 1;
      p++;
   }
   if (*p < '0' || *p > '9')
      return 0;
   long x = 0;
   int nd = 0;
   while (*p >= '0' && *p <= '9') {
      if (nd >= 18)
         return 0; /* longer than any value this format holds */
      x = (x * 10) + (*p - '0');
      nd++;
      p++;
   }
   *out = neg ? -x : x;
   *q   = p;
   return 1;
}

/* The `v<N> ` marker settings.c uses, with one difference that matters here:
 * there is no version 0. A settings file with no marker is a real file
 * written by a deployed build, so its absence had to mean something; a saved
 * state with no marker was written by nothing this project ever shipped, so
 * it is simply refused. -1 for anything that is not a marker. */
static int st_version(char **q)
{
   char *p = *q;
   if (*p != 'v')
      return -1;
   p++;
   long v = 0;
   if (!st_num(&p, &v))
      return -1;
   if (*p != ' ')
      return -1;
   *q = p;
   return (int)v;
}

/* WHAT THE SHELL WOULD LIKE BACK, encoded into `out`.
 *
 * Returns the byte count, or 0 when there is nothing worth saving -- which
 * includes every ordinary case: a user sitting on the main screen with no
 * form open has a one-entry route and no draft on show, and storing that
 * would mean handing the framework a blob on every single pause.
 *
 * The route is TRUNCATED at the first screen that may not be saved, rather
 * than the whole snapshot being dropped. A user three screens deep into the
 * device registry with a sensor's calibration panel on top still gets back
 * the part of their route that means the same thing on a new process. */
int state_encode(char *out, int cap)
{
   struct forms_view fv;
   forms_view_get(&fv);
   enum ui_screen path[NAV_MAX];
   int n    = nav_path(path, NAV_MAX);
   int keep = 0;
   while (keep < n && scr_saveable(path[keep], &fv))
      keep++;
   /* keep <= 1 is the main screen with nothing open. There is no draft to
    * carry because no form is showing one, and a blob that restores the main
    * screen onto the main screen is a blob for nothing. */
   if (keep <= 1)
      return 0;
   int haskp  = 0;
   int haswt  = 0;
   int hasins = 0;
   for (int i = 0; i < keep; i++) {
      if (path[i] == SCR_KEYPAD)
         haskp = 1;
      if (path[i] == SCR_WEIGHT)
         haswt = 1;
      if (path[i] == SCR_INSULIN)
         hasins = 1;
   }
   /* CANONICAL, so the decoder can check the blob against itself. Fields
    * belonging to a screen that is not on the saved route are written as
    * zeroes rather than as whatever the process happened to be holding: a
    * keypad return screen left over from a route that was truncated away is
    * not information, it is a leftover, and one that would have to be
    * validated for no benefit. */
   enum keypad_mode mode = haskp ? fv.kp_mode : KP_NONE;
   enum ui_screen ret    = haskp ? forms_kp_return() : SCR_MAIN;
   struct prefs sp;
   settings_get(&sp);
   int len = snprintf(out, (size_t)cap, "v%d %d %d %d", STATE_VERSION, sp.units,
                      sp.wunits, keep);
   if (len < 0 || len >= cap)
      return 0;
   for (int i = 0; i < keep; i++) {
      int k = snprintf(out + len, (size_t)(cap - len), " %d", (int)path[i]);
      if (k < 0 || k >= cap - len)
         return 0;
      len += k;
   }
   int k = snprintf(out + len, (size_t)(cap - len), " %d %d %ld %d %ld %d %d",
                    (int)mode, (int)ret, haswt ? fv.wt_t : 0L,
                    haswt ? fv.wt_tenths : 0, hasins ? fv.ins_t : 0L,
                    hasins ? fv.ins_type : 0, hasins ? fv.ins_units : 0);
   if (k < 0 || k >= cap - len)
      return 0;
   len += k;
   /* THE TYPED DIGITS, LAST, so the rest of the line is fixed-shape and this
    * one variable field cannot shift anything. '-' for an empty entry, which
    * keeps the field mandatory: a missing field and an empty one would
    * otherwise be the same bytes, and a truncated blob would then parse.
    *
    * `entrylen`, NOT strlen. The keypad's buffer is only valid up to its
    * recorded length -- clearing it resets the length and leaves the previous
    * characters in place -- so reading to the NUL would append somebody's
    * earlier typing to this entry. */
   int el = fv.entrylen;
   if (el < 0 || el > (int)sizeof fv.entry - 1)
      el = 0;
   if (!haskp)
      el = 0;
   if (len + 2 + el >= cap)
      return 0;
   out[len++] = ' ';
   if (el == 0) {
      out[len++] = '-';
      return len;
   }
   for (int i = 0; i < el; i++) {
      char c = fv.entry[i];
      /* The digit keypads collect [0-9.] and nothing else. A character
       * outside that set is not something to store and repost through the
       * framework: the entry is dropped and the route kept. */
      if (!((c >= '0' && c <= '9') || c == '.')) {
         out[len++] = '-';
         return len;
      }
      out[len++] = c;
   }
   return len;
}

/* THE OTHER HALF: a blob from the framework, checked against everything this
 * build knows and against everything the durable data now says. Fills `*st`
 * only on LOAD_OK. */
enum load_result state_decode(const void *blob, size_t nb,
                              struct saved_state *st)
{
   if (!blob || nb == 0)
      return LOAD_ABSENT; /* a normal cold start: nothing was saved */
   if (nb > STATE_MAX)
      return LOAD_CORRUPT; /* bigger than anything this build writes */
   char b[STATE_MAX + 1];
   memcpy(b, blob, nb);
   b[nb] = 0;
   /* PRINTABLE ASCII, checked over the WHOLE length rather than left to the
    * parser. An embedded NUL would otherwise end the parse early and a
    * truncated blob would read as a complete one. */
   for (size_t i = 0; i < nb; i++)
      if (b[i] < 0x20 || b[i] > 0x7e)
         return LOAD_CORRUPT;
   char *q = b;
   if (st_version(&q) != STATE_VERSION)
      return LOAD_CORRUPT;
   long units  = 0;
   long wunits = 0;
   long n      = 0;
   if (!st_num(&q, &units) || !st_num(&q, &wunits) || !st_num(&q, &n))
      return LOAD_CORRUPT;
   /* THE UNITS THE DRAFT WAS TYPED IN, and the reason this runs after
    * settings_load(). "1624" in the weight field is 162.4 of whichever unit
    * was showing; restore it under the other one and the user confirms a
    * weight wrong by a factor of 2.2, with the digits they typed still on
    * screen. The glucose unit governs every threshold keypad the same way. */
   struct prefs sp;
   settings_get(&sp);
   if (units != sp.units || wunits != sp.wunits)
      return LOAD_CORRUPT;
   if (n < 1 || n > NAV_MAX)
      return LOAD_CORRUPT;
   for (long i = 0; i < n; i++) {
      long v = 0;
      if (!st_num(&q, &v))
         return LOAD_CORRUPT;
      if (v < 0 || v >= SCR_N)
         return LOAD_CORRUPT;
      if (!scr_restorable((enum ui_screen)v))
         return LOAD_CORRUPT;
      st->path[i] = (enum ui_screen)v;
   }
   st->n = (int)n;
   /* THE ROOT IS THE MAIN SCREEN. Every route home ends there and nav_back
    * stops at index 0, so a route rooted anywhere else is a user who cannot
    * leave the screen they were restored onto. */
   if (st->path[0] != SCR_MAIN)
      return LOAD_CORRUPT;
   long mode      = 0;
   long ret       = 0;
   long wt_t      = 0;
   long tenths    = 0;
   long ins_t     = 0;
   long ins_type  = 0;
   long ins_units = 0;
   if (!st_num(&q, &mode) || !st_num(&q, &ret) || !st_num(&q, &wt_t) ||
       !st_num(&q, &tenths) || !st_num(&q, &ins_t) || !st_num(&q, &ins_type) ||
       !st_num(&q, &ins_units))
      return LOAD_CORRUPT;
   int haskp = 0;
   for (int i = 0; i < st->n; i++)
      if (st->path[i] == SCR_KEYPAD)
         haskp = 1;
   if (haskp) {
      if (!kp_restorable((enum keypad_mode)mode))
         return LOAD_CORRUPT;
      /* Where the keypad closes to has to be somewhere this build is willing
       * to be, or its X button lands on a screen the route was truncated to
       * avoid -- and it may not be the keypad itself, which would be a screen
       * that cannot be closed. */
      if (ret < 0 || ret >= SCR_N || ret == SCR_KEYPAD ||
          !scr_restorable((enum ui_screen)ret))
         return LOAD_CORRUPT;
   } else if (mode != KP_NONE || ret != SCR_MAIN) {
      /* The encoder writes exactly these when no keypad is on the route.
       * Anything else is a blob that does not agree with itself. */
      return LOAD_CORRUPT;
   }
   st->kp_mode = (enum keypad_mode)mode;
   st->kp_ret  = (enum ui_screen)ret;
   /* THE DRAFT NUMBERS, against the same bounds their own modules enforce. A
    * value outside them cannot have been typed here, so it came from
    * somewhere else and the blob is not ours. Deliberately NOT the full
    * domain rule (a weight of 1.6 lb is out of range, and is also what a
    * half-typed 162.4 looks like): these are the format's bounds, and the
    * commit path still applies the real ones. */
   if (wt_t < 0 || wt_t > WT_T_MAX)
      return LOAD_CORRUPT;
   if (tenths < 0 || tenths > 99999)
      return LOAD_CORRUPT;
   if (ins_t < 0 || ins_t > INS_T_MAX)
      return LOAD_CORRUPT;
   if (ins_type != INS_SLOW && ins_type != INS_FAST)
      return LOAD_CORRUPT;
   if (ins_units < 0 || ins_units > INS_UNITS_MAX)
      return LOAD_CORRUPT;
   st->wt_t      = wt_t;
   st->wt_tenths = (int)tenths;
   st->ins_t     = ins_t;
   st->ins_type  = (int)ins_type;
   st->ins_units = (int)ins_units;
   /* THE TYPED DIGITS. '-' is an empty entry; anything else must be the
    * character set the digit keypads collect, no longer than the field this
    * mode actually draws (kp_slots), and there must be a keypad on the route
    * to hold it. */
   while (*q == ' ')
      q++;
   if (*q == 0)
      return LOAD_CORRUPT; /* the field is mandatory, so a truncation shows */
   st->entry[0] = 0;
   if (!(q[0] == '-' && q[1] == 0)) {
      if (!haskp)
         return LOAD_CORRUPT;
      int el = 0;
      while (q[el]) {
         char c = q[el];
         if (!((c >= '0' && c <= '9') || c == '.'))
            return LOAD_CORRUPT;
         el++;
         if (el > (int)sizeof st->entry - 1)
            return LOAD_CORRUPT;
      }
      /* '.' costs a cell like every other character does on screen, so this
       * is the same ceiling the input path applies. */
      if (el > kp_slots((enum keypad_mode)mode))
         return LOAD_CORRUPT;
      memcpy(st->entry, q, (size_t)el);
      st->entry[el] = 0;
   }
   return LOAD_OK;
}

/* Put a validated snapshot back. Nothing here can fail: everything it writes
 * was checked by state_decode against this build's rules and this phone's
 * loaded settings. */
static void state_apply(const struct saved_state *st)
{
   /* THE DRAFTS FIRST, then the route, then the keypad -- so the screen the
    * user lands on is already showing the values it is about. Only the drafts
    * whose form is actually on the route: the others were written as zeroes,
    * and restoring those would overwrite a form the user has not opened yet
    * with a 1970 timestamp. */
   if (state_path_has(st, SCR_WEIGHT))
      forms_wt_restore(st->wt_t, st->wt_tenths);
   if (state_path_has(st, SCR_INSULIN))
      forms_ins_restore(st->ins_t, st->ins_type, st->ins_units);
   nav_set_path(st->path, st->n);
   if (st->kp_mode != KP_NONE) {
      forms_kp_mode_set(st->kp_mode);
      forms_kp_return_set(st->kp_ret);
      forms_kp_seed(st->entry);
   }
}

/* THE FRAMEWORK IS ASKING FOR THE STATE. The buffer must be malloc'd: the
 * NativeActivity contract is that the framework free()s it, so a pointer to
 * anything else here is a free() of a static or a stack address.
 *
 * Answering NULL with *outsz = 0 is the ordinary case, not a failure -- see
 * state_encode: most pauses happen with nothing worth carrying. */
void *shellstate_save(struct ANativeActivity *a, size_t *outsz)
{
   (void)a;
   if (!outsz)
      return 0;
   *outsz = 0;
   char buf[STATE_MAX];
   int n = state_encode(buf, (int)sizeof buf);
   if (n <= 0)
      return 0;
   char *heap = malloc((size_t)n);
   if (!heap)
      return 0;
   memcpy(heap, buf, (size_t)n);
   *outsz = (size_t)n;
   return heap;
}

/* THE FRAMEWORK IS HANDING THE STATE BACK. Called from onCreate AFTER
 * init_data, because two of the checks inside are against data that has to be
 * on disk and in memory first (see the header comment above). */
void shellstate_restore(const void *saved, size_t nb)
{
   struct saved_state st;
   enum load_result r = state_decode(saved, nb, &st);
   if (r == LOAD_OK) {
      state_apply(&st);
      /* THE DEPTH IS AT LEAST ONE by state_decode's contract -- it refuses
       * n < 1 outright -- but the invariant lives in that function and this
       * line indexes on it. Said here so a reader does not have to go and
       * check, and so the analyzer does not have to reason across the call:
       * it flagged this as a possible out-of-bounds read, and it was right
       * that nothing local ruled it out. */
      int top = (st.n > 0) ? (int)st.path[st.n - 1] : -1;
      LOGI("startup: restored screen state (%d deep, screen %d)", st.n, top);
      return;
   }
   if (r == LOAD_ABSENT)
      return; /* nothing was saved: an ordinary cold start */
   /* REFUSED, and the user is on the main screen. Logged rather than put on
    * the status line: nothing durable was lost -- the readings, the doses and
    * the settings are all on disk -- and the one thing that is gone, a draft
    * the user had not confirmed, is not something to raise an alarm about on
    * a launch that is otherwise healthy. The most common cause is the launch
    * straight after an update, where the version marker moved. */
   LOGW("startup: the saved screen state was REFUSED (%s, %d bytes)",
        load_result_name(r), (int)nb);
}
