// SPDX-License-Identifier: GPL-3.0
// alarm.c --- Alarm actuation (see alarm.h)
// Copyright 2026 Jakob Kastelic

#include "alarm.h"
#include "alarmlogic.h"
#include "bletrans.h"
#include "clock.h"
#include "dexdriver.h" /* LINK_MAX: the alarm walks every CGM link */
#include "log.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "store.h"
#include "thread.h"

/* Imminent hypo: ANY CGM whose latest reading predicts below PRED_LOW_MGDL, and
 * which reported within the alarm freshness window. The freshness gate is what
 * keeps an unsilenceable alarm from wedging on stale data after a sensor drops
 * out -- the prediction must be current, not a value frozen at disconnect.
 *
 * The threshold and the predicate both live in alarmlogic.h now, where a test
 * can execute them; this file keeps the per-link state and the fold. */

/* The glucose zone RIGHT NOW: 0 in range, 1 low, 2 high. Derived, never
 * latched. Call with alarm_lock held.
 *
 * This must be recomputed by the 1 Hz path as well as by the reading path.
 * When only alarm_reeval() (which runs when a reading arrives) updated the
 * zone, the freshness test below effectively always passed -- it ran
 * microseconds after a reading landed -- so a non-zero zone could never decay.
 * alarm_disc_reeval() on the 1 Hz timer now recomputes it, which is also what
 * makes a glucose alarm get raised at all now that the BLE threads do not
 * evaluate. A sensor dropping out while low then left `zone` latched at 1
 * forever, and because alarm_apply ranks zone above stale, the DISCONNECT alarm
 * could never sound: exactly the lost-alarm failure the level-based rewrite
 * existed to prevent, reintroduced through a different door. */
/* The current reading, read as ONE consistent pair.
 *
 * hist_refresh_current writes g_cur_glu and g_cur_time as separate stores
 * under hist_lock, while the alarm path read them with no lock at all, from
 * three different threads. A read that landed between the two stores paired a
 * new glucose with the previous timestamp -- so a genuine LOW could evaluate
 * as "not fresh" and not be raised on that pass -- or paired a stale
 * out-of-range value with a fresh timestamp and chimed something that then
 * silenced itself.
 *
 * Taken SEQUENTIALLY, before alarm_lock, never nested inside it: hist is the
 * innermost lock in the documented order, and nesting it under alarm would add
 * the one edge that could still form a cycle. Sequential costs nothing here --
 * the pair only has to be self-consistent, not held. */
struct alarm_reading {
   int glu;
   long t;
};

/* DISCONNECT-alarm minutes. The shortest MUST stay above AL_FRESH_S (11 min),
 * or a fresh-but-out-of-range zone outranks AL_STALE and delays the label the
 * threshold exists to produce -- see alarm_stranded in alarmlogic.h. This was
 * 10, i.e. under the window, for exactly as long as AL_FRESH_S was 6 min.
 * ui_disc_lbl (uidraw.c) prints these; the two tables move together. */
static const int disc_min[] = {0, 15, 30, 60};

/* Is the stale-data (DISCONNECT) alarm currently latched?
 *
 * ATOMIC, because it is written on the alarm path -- the main looper, a GATT
 * binder thread and the service tick, all under alarm_lk -- and READ by
 * alarm_disc_latched() from the frame builder, which holds the HISTORY lock
 * and not this one. A plain int shared that way is a C data race, which
 * ThreadSanitizer reports and which the compiler is entitled to act on; that
 * is app/thread.h rule 2, and this is exactly the shape it names: a scalar
 * latch, one meaning, several threads.
 *
 * IT IS AN ATOMIC RATHER THAN A WIDENING OF alarm_lk, and the reason is the
 * lock order. alarm_disc_latched() is called from build_model, which runs
 * with the history lock held; taking alarm_lk there would create
 * history -> alarm, and alarm_lk is held across blocking MediaPlayer JNI for
 * hundreds of milliseconds. The main looper would then stall a frame on a
 * media call while a binder thread with a reading to deliver queued behind it
 * on the history lock -- an ANR built out of a label on the screen.
 *
 * RELAXED is right: it is one self-contained scalar with no companion state
 * whose visibility must be ordered against it, and the reader is a renderer
 * that repaints several times a second, so the worst a delayed observation
 * costs is one frame of a label. */
static atomic_int g_disc_alarmed;

/* NUDGE latch: which nudge band the last KNOWN reading was in (NG_*).
 *
 * State, not a derived value, because the nudge announces a CROSSING and a
 * crossing cannot be read off a single sample. Guarded by alarm_lock like
 * every other alarm state, and deliberately NOT persisted: after a restart the
 * app has no idea whether the user already heard this crossing, and the honest
 * default is "not announced yet" -- a nudge too many is a blip, a nudge too
 * few is the reminder that never arrives. */
static int g_nudge_state;

static int g_alarm_state;    /* last reading's zone: 0 ok, 1 low, 2 high */
static int g_alarm_sounding; /* an audible alarm is currently active */

/* Re-evaluate the alarm against the latest fresh reading and current
 * thresholds. Chime+vibrate once on the transition from NOT-alarmed to alarmed
 * -- whether that transition comes from a new glucose value or from moving a
 * threshold. Never re-fires while already alarmed (low<->high is not a new
 * entry); silences when the value returns in range. Safe to call on every
 * reading or threshold tap. */
/* Alarm state, and why it is shaped this way.
 *
 * alarm_apply is called from THREE kinds of thread, not one. This comment
 * used to claim "every caller runs on the MAIN looper" and "the BLE threads
 * deliberately do not evaluate alarms" -- both false, and dangerously so,
 * because they are exactly the assumptions someone would rely on when deciding
 * a lock here is unnecessary. The real callers are:
 *   - the main looper: alarm_disc_reeval() on the 1 Hz timer, alarm_reeval()
 * from a threshold tap, and the tap-to-silence path in on_input;
 *   - a GATT binder thread: jni_notify calls pancra_alarm_check AFTER
 *     releasing driver_lock (which is the property that actually matters --
 *     raising an alarm does blocking MediaPlayer work, and doing that under a
 *     no-timeout spin lock the main looper also takes is what must not happen);
 *   - the service's tick HandlerThread, via jni_tick.
 * alarm_lock is therefore genuinely contended and genuinely load-bearing.
 *
 * Two properties are load-bearing and must survive any future change:
 *
 * 1. LEVEL-based, not edge-triggered. alarm_apply recomputes what should be
 *    sounding and reconciles, so a missed transition self-corrects on the next
 *    tick. The original edge-triggered version lost a disconnect alarm
 *    permanently when a reading silenced it a microsecond after it was raised:
 *    g_disc_alarmed stayed latched at 1, so the !alarmed -> alarmed edge never
 *    happened again.
 * 2. Raise and silence are mutually exclusive END TO END, JNI call included.
 *    Splitting the decision from the call lets two threads decide in one order
 *    and call Java in the other; Alarm.trigger/silence being `synchronized`
 *    orders them against each other but cannot repair a wrong order. The
 *    result was an alarm that looped forever with nothing able to stop it.
 *
 * alarm_lock is what keeps property 2 true across those three threads. */
static struct rmutex alarm_lk = RMUTEX_INIT;

/* What we last asked Java to sound (0 = silent). Call with alarm_lock held. */
static int g_alarm_want;
/* The user has DISMISSED the level currently in g_alarm_want.
 *
 * Acknowledgement used to be recorded only as "g_alarm_want still equals this
 * level", which alarm_reactuate destroys by design -- so toggling SOUND or
 * VIBRATION restarted an alarm the user had already silenced, from a settings
 * screen they could only reach because it was silent. Recorded explicitly, it
 * survives a re-actuation and is cleared by a genuine level change, which is a
 * new alarm they have not seen. Call with alarm_lock held. */
static int g_alarm_acked;

/* Most recent PREDICTED value and WHEN IT ARRIVED, per CGM link, AS ONE WORD.
 *
 * Written in pancra_glucose as each reading is decoded, on a GATT binder
 * thread with the DRIVER lock held; read in the alarm evaluation, on the main
 * looper or the service tick, under the ALARM lock. Two threads, two
 * different locks, so the pair needs a synchronisation of its own.
 *
 * IT USED TO BE TWO PLAIN ARRAYS, an int and a long, written as two separate
 * stores and read as two separate loads. That is a C data race outright --
 * ThreadSanitizer reports it as one -- and it publishes MIXED PAIRS: a reader
 * landing between the two stores gets this sample's predicted value carrying
 * the previous sample's arrival time, or the reverse. Both halves of that are
 * wrong in a way that matters: a current 48 mg/dL prediction stamped five
 * minutes ago is discarded as expired, and a recovered value stamped now
 * forces the unsilenceable LOW over a prediction that no longer says anything.
 *
 * ONE ATOMIC WORD, not a lock, and not alarm_lk in particular. The writer
 * holds the driver lock; alarm_lk is held across blocking MediaPlayer JNI, so
 * taking it here would be driver -> alarm and would park a binder thread's
 * driver lock behind a media call while the main looper spins on that same
 * driver lock. That is a freeze this app has already had (see any_pred_low),
 * and app/thread.h rule 6 says alarm_lk is taken ALONE. A single word needs
 * neither, and adds no edge to the lock order at all.
 *
 * RELAXED on both sides, deliberately. The word is self-contained: it carries
 * both halves of the only fact, and there is no other state whose visibility
 * has to be ordered against it. Atomicity is what is needed here, not
 * ordering -- see app/thread.h rule 2, which is why this is an atomic and not
 * a `volatile`.
 *
 * THE ARRIVAL STAMP IS MONOTONIC (mono_s()), and it used to be realtime. It
 * is not a record of anything: never written to a file, never shown, never
 * compared with a reading's timestamp -- its only job is to answer "did this
 * arrive recently enough to still mean something", which is an interval. On
 * the wall clock a backward correction made every stored prediction
 * indefinitely fresh. `make clockcheck` names alarm_note_pred, so the caller
 * cannot quietly go back to supplying realtime_s(). */
static _Atomic unsigned long long g_link_pred[LINK_MAX];

static void alarm_lock(void)
{
   rmutex_lock(&alarm_lk);
}

static void alarm_unlock(void)
{
   rmutex_unlock(&alarm_lk);
}

/* SILENCE A SOUNDING ALARM, and say whether that consumed the gesture.
 *
 * The test and the silence share ONE critical section: splitting them lets a
 * re-raise land in between, and then the press the user made to stop the noise
 * is spent on a UI action instead (property 2 in alarm.h).
 *
 * If Java was not reached the tone may still be playing, so `sounding` is left
 * set and 0 is returned -- the next press tries again rather than falling
 * through to the UI with an alarm still on. Acknowledgement is recorded
 * explicitly so it SURVIVES a re-actuation: a settings toggle must not restart
 * an alarm the user already dismissed. */
int alarm_silence_tap(void)
{
   int consumed = 0;
   alarm_lock();
   if (g_alarm_sounding) {
      if (dexble_alarm_silence()) {
         g_alarm_sounding = 0;
         g_alarm_acked    = 1;
         consumed         = 1;
      } else {
         LOGI("alarm: silence failed; leaving the gesture armed");
      }
   }
   alarm_unlock();
   return consumed;
}

int alarm_disc_latched(void)
{
   return atomic_load_explicit(&g_disc_alarmed, memory_order_relaxed);
}

void alarm_note_pred(int link, int pred_mgdl, long mono_now)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   /* ONE STORE. The value and its stamp become visible together or not at
    * all, so no reader can ever pair one epoch's prediction with another
    * epoch's arrival time. */
   atomic_store_explicit(&g_link_pred[link], pred_pack(pred_mgdl, mono_now),
                         memory_order_relaxed);
}

/* Is any CGM link forecasting a hypo?
 *
 * `is_meter` is passed IN, sampled before alarm_lock, and that is the whole
 * point of the parameter. This used to ask the driver from here, and that
 * takes the DRIVER's lock -- which made this "alarm held, driver
 * taken". The driver's lock is the OUTERMOST of the three (driver ->
 * registry -> history) and is held across GATT dispatch and Java calls that
 * block for hundreds of milliseconds; taking it from inside alarm_lock put
 * the MAIN LOOPER in a no-timeout spin holding the alarm lock, and the
 * service tick then spun on the alarm lock behind it. The phone froze and
 * Android killed the app as not responding.
 *
 * alarm_gather already samples the registry and the history before the lock
 * for exactly this reason; the routing bits are the third thing that has to
 * be read first. */
static int any_pred_low(const int *is_meter)
{
   /* MONOTONIC, matching the stamps alarm_note_pred records. The rule itself
    * is alarm_pred_low (alarmlogic.h) -- it was open-coded here, in a file no
    * test can reach, on the one alarm path that overrides a muted phone. */
   long now = mono_s();
   for (int l = 0; l < LINK_MAX; l++) {
      if (is_meter[l])
         continue; /* CGMs only */
      /* ONE LOAD, then decide from the pair it yielded. Re-reading the word
       * for the stamp would put the mixed-pair hole straight back. */
      struct link_pred p = pred_unpack(
          atomic_load_explicit(&g_link_pred[l], memory_order_relaxed));
      if (alarm_pred_low(p.mgdl, p.mono, now))
         return 1;
   }
   return 0;
}

/* WHICH LINKS ARE METERS, as one read, BEFORE the alarm lock is taken. */
static void meter_links_snapshot(int *out)
{
   for (int l = 0; l < LINK_MAX; l++)
      out[l] = driver_link_is_meter(l);
}

/* THE SHELL HALF of the alarm workflow: gather, step, actuate, commit.
 *
 * The DECIDING is alarm_actuate_step's (alarmlogic.c) and is pure. This
 * function does the three things only the shell can: read the settings, call
 * Java, and commit or roll back depending on whether Java was reached.
 *
 * It used to compose the sustain rule, the imminent-hypo override and the
 * decision BY HAND -- while alarmlogic.c held a pure, tested copy of that
 * same composition (alarm_plan_next) that nothing called. Deleting the tested
 * copy would have failed alarmtest and changed nothing on the phone, which is
 * the exact shape of gap this header set out to close.
 *
 * Call with alarm_lock held. */
static void alarm_apply_ex(int zone, int stale, int stranded, int pred_low)
{
   struct prefs sp;
   settings_get(&sp);
   struct alarm_state st = {.want     = g_alarm_want,
                            .acked    = g_alarm_acked,
                            .sounding = g_alarm_sounding};
   struct alarm_obs obs  = {.zone     = zone,
                            .stale    = stale,
                            .stranded = stranded,
                            .pred_low = pred_low,
                            .sound_on = sp.sound_on,
                            .vib_on   = sp.vib_on};
   struct alarm_state next;
   struct alarm_effect eff;
   alarm_actuate_step(&st, &obs, &next, &eff);
   if (eff.act == AL_ACT_NONE)
      return;

   /* COMMIT FIRST, ROLL BACK IF JAVA WAS NEVER REACHED.
    *
    * The state has to be committed before the call, because the call is what
    * the state describes -- but if it fails, keeping the commitment makes the
    * idempotence check in the transition suppress every later attempt, and the
    * alarm stays silent for its whole duration. Rolling back restores the
    * level-based self-correction this design depends on: the next tick simply
    * tries again. Every field is saved, because a half-applied rollback --
    * `want` restored while `acked` stays cleared -- re-arms a dismissed alarm
    * on the next settings toggle, which is the dismissal bug coming back
    * through the failure path. */
   g_alarm_want     = next.want;
   g_alarm_acked    = next.acked;
   g_alarm_sounding = next.sounding;

   /* THE JNI CALL STAYS INSIDE alarm_lock, deliberately.
    *
    * Releasing it around the call is tempting -- Alarm.trigger does
    * setDataSource() + prepare() + start(), which is synchronous and can take
    * hundreds of milliseconds. But dropping it opens an un-silenceable alarm:
    *
    *   binder: commits want=LOW, unlocks, enters trigger() (slow)
    *   user taps: lock is free, so silence proceeds -- sounding = 0, and
    *              Alarm.silence() stops nothing (no player yet)
    *   binder: trigger() then creates and starts a LOOPING alarm player
    *
    * The tone now plays forever: want is still LOW so no later apply issues a
    * silence, and sounding is 0 so tapping again does nothing.
    * Alarm.trigger/silence being `synchronized` orders them against each other
    * but cannot fix the wrong ORDER. Raise and silence must be mutually
    * exclusive end to end, so the lock spans the call. */
   int ok = (eff.act == AL_ACT_TRIGGER && eff.kind >= 0)
                ? dexble_alarm(eff.kind, eff.sound, eff.vib)
                : dexble_alarm_silence();
   if (!ok) {
      LOGI("alarm: actuation failed; will retry on the next evaluation");
      g_alarm_want     = st.want;
      g_alarm_acked    = st.acked;
      g_alarm_sounding = st.sounding;
   }
}

static struct alarm_reading current_reading(void)
{
   /* One call, one lock, one consistent triple -- see store_now. Reading the
    * three globals separately could pair a new glucose with the previous
    * timestamp, and a genuine low then evaluates as "not fresh" and is not
    * raised on that pass. */
   struct reading_now n = store_now(realtime_s());
   struct alarm_reading r;
   r.glu = n.glu;
   r.t   = n.t;
   return r;
}

/* Is the ALARM announcing anything right now? The input nudge_fire needs, and
 * broader than this tick's excursion zone on purpose: g_alarm_want carries the
 * DISCONNECT alarm and the stranded sustain, neither of which shows up in the
 * zone, and `pred` is the imminent-hypo override, which forces AL_LOW at any
 * current reading. Called under alarm_lock (g_alarm_want is read there), and
 * it reads the level committed by the PREVIOUS evaluation -- which is exactly
 * the alarm the user can hear right now. */
static int alarming(int zone, int pred)
{
   return zone || pred || g_alarm_want != AL_NONE;
}

/* Emit the one-time nudge. NG_NONE is the answer on all but a handful of the
 * ~86400 ticks in a day, so this is a no-op almost always. Best-effort, like
 * the NEW DATAPOINT beep: a missed nudge is a missed hint, not a missed
 * alarm, and it must never be able to delay or throw into the alarm path. */
static void nudge_emit(int ng)
{
   if (ng == NG_NONE)
      return;
   /* AFTER the early return: NG_NONE is the answer on all but a handful of
    * the ~86400 ticks in a day, and a copy of the whole aggregate on each of
    * them is work for nothing. */
   struct prefs sp;
   settings_get(&sp);
   /* Its OWN outputs, not the alarm's: one alert means "act now" and the
    * other "have a look", so muting either must not mute the other. With
    * both off there is nothing to emit and no reason to cross into Java at
    * all -- the latch has already been committed by the caller either way,
    * so the crossing stays announced and will not fire again on the next
    * tick. */
   if (!sp.nudge_sound && !sp.nudge_vib)
      return;
   LOGI("nudge %s (sound=%d vib=%d)", ng == NG_LOW ? "low" : "high",
        sp.nudge_sound, sp.nudge_vib);
   dexble_nudge(ng == NG_LOW ? 0 : 1, sp.nudge_sound, sp.nudge_vib);
}

/* Gather the alarm zone, the stranded flag and the NUDGE zone across every
 * live CGM from ONE snapshot of the history. The nudge rides along rather
 * than gathering again: two passes could see different samples, and a nudge
 * evaluated against a reading the alarm never saw is a nudge that can fire
 * underneath its own alarm -- exactly what nudge_fire exists to prevent.
 * `*nzone` comes back -1 when no live CGM has a current reading. */
static void alarm_gather(long now, int *zone, int *stranded, int *nzone)
{
   struct prefs sp;
   settings_get(&sp);

   struct {
      int glu;
      long t;
   } smp[MAX_SLOTS];

   int ids[MAX_SLOTS];
   int nids = 0;
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n && nids < MAX_SLOTS; i++) {
      if (v.slot[i].old) /* disconnected: not part of the live alarm set */
         continue;
      if (v.have_rec[i] && sensor_kind(v.rec[i].type) == KIND_CGM)
         ids[nids++] = v.slot[i].id;
   }
   int ns = 0;
   store_lock();
   for (int i = 0; i < nids; i++)
      for (int k = 0; k < hist_count(); k++)
         if (hist_at(k).src == (unsigned short)ids[i] &&
             hist_at(k).kind != KIND_BGM) {
            smp[ns].glu = hist_at(k).glu;
            smp[ns].t   = hist_at(k).t;
            ns++;
            break;
         }
   if (nids == 0) { /* pre-registry fallback: judge the current reading */
      /* Read DIRECTLY, not through store_now: the store lock is held right
       * here and it is not recursive. Holding it is also what makes these two
       * consistent, which is the same guarantee store_now gives an unlocked
       * caller. */
      struct reading_now cur = store_now_locked(realtime_s());
      smp[0].glu             = cur.glu;
      smp[0].t               = cur.t;
      ns                     = 1;
   }
   store_unlock();
   *zone     = 0;
   *stranded = 0;
   *nzone    = -1;
   for (int i = 0; i < ns; i++) {
      *zone = alarm_zone_merge(*zone, alarm_zone(smp[i].glu, smp[i].t, now,
                                                 sp.alarm_low, sp.alarm_high));
      if (alarm_stranded(smp[i].glu, smp[i].t, now, sp.alarm_low,
                         sp.alarm_high))
         *stranded = 1;
      /* Merged the same way the alarm zone is -- the worst band on ANY worn
       * CGM wins, and a LOW outranks a HIGH. A sensor with no current
       * reading contributes nothing rather than voting "in range", which
       * would clear the latch on a dropout and re-arm the nudge to fire
       * again. */
      int nz =
          nudge_zone(smp[i].glu, smp[i].t, now, sp.nudge_low, sp.nudge_high);
      if (nz >= 0)
         *nzone = (*nzone < 0) ? nz : alarm_zone_merge(*nzone, nz);
   }
}

void alarm_reeval(void)
{
   long now     = realtime_s();
   int zone     = 0;
   int stranded = 0;
   int nzone    = -1;
   alarm_gather(now, &zone, &stranded, &nzone); /* BEFORE alarm_lock */
   int is_meter[LINK_MAX];
   meter_links_snapshot(is_meter); /* the driver's lock, also BEFORE */
   alarm_lock();
   g_alarm_state = zone;
   int pred      = any_pred_low(is_meter);
   int ng        = nudge_fire(nzone, alarming(zone, pred), g_nudge_state);
   g_nudge_state = nudge_next(nzone, g_nudge_state);
   alarm_apply_ex(g_alarm_state,
                  atomic_load_explicit(&g_disc_alarmed, memory_order_relaxed),
                  stranded, pred);
   alarm_unlock();
   nudge_emit(ng);
}

int alarm_set_threshold(int isnudge, int islow, int mgdl)
{
   if (mgdl > AL_ENTRY_MAX)
      return TH_TOO_BIG;
   if (!islow && mgdl <= 0)
      return TH_HIGH_ZERO;

   /* ONE critical section: read the partner, decide, and store. The copy is
    * taken HERE, inside it -- taken at the top of the function it is a read
    * that happened before the lock, so two commits could both see the old
    * partner and store an inverted pair, which is the one thing this pair
    * may never be (settings.h). */
   alarm_lock();
   struct prefs sp;
   settings_get(&sp);
   int lo  = isnudge ? sp.nudge_low : sp.alarm_low;
   int hi  = isnudge ? sp.nudge_high : sp.alarm_high;
   int bad = islow ? (mgdl > hi) : (mgdl < lo);
   /* THE WHOLE QUADRUPLE, built here and stored in one call. Which one moves
    * is this function's decision -- it is the only place that holds the lock
    * the partner was read under -- but the STORE is all four together, so
    * there is no moment at which one has moved and the others have not, and
    * no way to store without persisting. */
   int al = sp.alarm_low, ah = sp.alarm_high;
   int nl = sp.nudge_low, nh = sp.nudge_high;
   if (!bad) {
      if (isnudge && islow)
         nl = mgdl;
      else if (isnudge)
         nh = mgdl;
      else if (islow)
         al = mgdl;
      else
         ah = mgdl;
   }
   alarm_unlock();
   if (bad)
      return TH_ORDER;

   /* THE WRITE IS PART OF THE ANSWER. It was dropped, so a threshold that
    * never reached the disk was reported to the user as accepted and came
    * back to its old value at the next launch -- on the two numbers that
    * decide whether a hypo alarm can fire. */
   int saved = settings_store_thresholds(al, ah, nl, nh) == SETTINGS_OK;
   alarm_reeval(); /* a threshold move can itself enter or leave the alarm */
   return saved ? TH_OK : TH_NOT_SAVED;
}

/* Re-issue the CURRENT alarm level to Java, even though the level has not
 * changed.
 *
 * alarm_apply is idempotent on the level -- re-asserting the same one must
 * not re-chime -- but that early return also swallowed a change to whether
 * the level is PERCEPTIBLE. With SOUND and VIBRATION both off, a low reading
 * commits g_alarm_want = AL_LOW while nothing sounds; turning SOUND on then
 * left want unchanged, so dexble_alarm() was never called again and the hypo
 * stayed silent for its entire duration, becoming audible only if glucose
 * returned to range and re-crossed. Clearing g_alarm_want forces the next
 * evaluation to treat the level as new. The threshold entry (the four
 * kp_is_thresh modes) already accepts that a settings change can change alarm
 * state; this is the same for the audible settings. */
void alarm_reactuate(void)
{
   struct prefs sp;
   settings_get(&sp);
   /* Clear AND re-evaluate under ONE hold. Releasing between them left a
    * window in which a binder thread or the service tick could run
    * pancra_alarm_check, re-commit the same g_alarm_want, and make the
    * re-evaluation early-return on `want == g_alarm_want` -- silently losing
    * the re-actuation, which is precisely the failure this function exists
    * to prevent. alarm_lock is recursive, so nesting is free.
    *
    * No current_reading() here: re-announcing the level we already hold
    * needs no fresh sample, so this function no longer touches hist_lock at
    * all -- which also removes the hist-under-alarm nesting it once had. */
   alarm_lock();
   /* RE-ANNOUNCE THE COMMITTED LEVEL. Do not clear it and recompute.
    *
    * Clearing g_alarm_want and re-evaluating looks equivalent and is not:
    * the sustain rule is keyed on the PREVIOUS level, so zeroing it deletes
    * the only thing keeping a stranded hypo alive, and the recompute then
    * yields AL_NONE. Three failures follow from that one line -- a 45 mg/dL
    * alarm that can never be made audible, a tone that keeps playing after
    * the user switches SOUND off, and no silence when glucose recovers,
    * because want == prev_want makes every later evaluation a no-op.
    * Re-issuing the level we already hold has no such dependency. */
   if (alarm_reactuate_allowed(g_alarm_acked) && g_alarm_want != AL_NONE) {
      int kind         = alarm_java_kind(g_alarm_want);
      int was_sounding = g_alarm_sounding;
      g_alarm_sounding = alarm_audible(g_alarm_want, sp.sound_on, sp.vib_on);
      if (kind >= 0 && !dexble_alarm(kind, sp.sound_on, sp.vib_on)) {
         LOGI("alarm: re-actuation failed; leaving the level committed");
         g_alarm_sounding = was_sounding;
      }
   }
   alarm_unlock();
}

void pancra_alarm_check(void)
{
   struct prefs sp;
   settings_get(&sp);
   /* The DISCONNECT/stale alarm stays bound to the CURRENT reading -- the
    * primary's, i.e. the number the user is watching going stale is the fact
    * it announces. The excursion zone and the stranded sustain are gathered
    * across EVERY CGM (alarm_gather): a low on the non-primary sensor rings
    * too. All the reads happen BEFORE alarm_lock (lock-order: hist never
    * nests inside alarm). */
   struct alarm_reading cur = current_reading();
   long now                 = realtime_s();
   int zone                 = 0;
   int stranded             = 0;
   int nzone                = -1;
   alarm_gather(now, &zone, &stranded, &nzone);
   int is_meter[LINK_MAX];
   meter_links_snapshot(is_meter); /* the driver's lock, BEFORE the alarm's */
   alarm_lock();
   g_alarm_state = zone;
   /* The nudge is decided under the SAME hold as the alarm, so the two see
    * one consistent zone pair and only one thread can claim a given crossing
    * -- pancra_alarm_check runs on the main looper, the service tick AND a
    * GATT binder thread. The SOUND is emitted after the unlock: it is a JNI
    * call that synthesises and plays audio, and alarm_lock is a spin lock
    * the main looper takes. Nothing depends on when it lands, unlike the
    * glucose alarm whose actuation must stay inside the lock (see
    * alarm_apply_ex). */
   int pred      = any_pred_low(is_meter);
   int ng        = nudge_fire(nzone, alarming(zone, pred), g_nudge_state);
   g_nudge_state = nudge_next(nzone, g_nudge_state);
   /* Either the user's configured DISCONNECT threshold, or -- regardless of
    * that setting -- data going stale while the last reading was out of
    * range. Without the second term a ringing hypo alarm was silenced after
    * AL_FRESH_S of dropout in the DEFAULT configuration. See alarm_stranded.
    */
   /* FOUR TIME ARGUMENTS, TWO CLOCKS. cur.t and `now` are realtime because
    * cur.t is the reading's own timestamp -- the record's identity, and the
    * time printed beside it. The launch grace is an in-process interval, so
    * it is measured monotonically and a wall-clock correction cannot end it
    * early or extend it. See alarm_stale in alarmlogic.h. */
   int disc = alarm_stale(cur.glu, cur.t, now, mono_s(), shell_launch_mono(),
                          (long)disc_min[(unsigned)sp.disc & 3U] * 60);
   atomic_store_explicit(&g_disc_alarmed, disc, memory_order_relaxed);
   /* Stranded is passed SEPARATELY, not folded into g_disc_alarmed: it may
    * only sustain an alarm that is already sounding, never originate one and
    * never relabel it. See alarm_want_sustained. Folding it in made a stale
    * low mint a fresh "Sensor disconnected" -- including one second after a
    * cold start, off a reading store_load had just restored from the log. */
   alarm_apply_ex(g_alarm_state, disc, stranded, pred);
   alarm_unlock();
   nudge_emit(ng);
}

void alarm_disc_reeval(void)
{
   /* One implementation, shared with the transport and the service
    * heartbeat. The zone is recomputed here so it DECAYS with time -- this
    * path runs when no readings are arriving, which is exactly when a stale
    * zone would otherwise mask the disconnect alarm. */
   pancra_alarm_check();
}
