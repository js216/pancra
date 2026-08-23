// SPDX-License-Identifier: GPL-3.0
// pairing.c --- Adverts in, a registered device out (see pairing.h)
// Copyright 2026 Jakob Kastelic

#include "pairing.h"
#include "bletrans.h"
#include "clock.h"
#include "dexdriver.h"
#include "forms.h"
#include "log.h"
#include "meter.h"
#include "nav.h"
#include "paircode.h"
#include "reconcile.h"
#include "scan.h"
#include "scanlogic.h"
#include "selection.h"
#include "sensors.h"
#include "settings.h"
#include "shell.h"
#include "status.h"
#include "store.h"
#include "thread.h"
#include "uimodel.h"
#include "util.h"
#include <jni.h>
#include <jni_md.h>
#include <stdatomic.h> /* the advert path shares these with the main thread */
#include <stdio.h>
#include <string.h>

/* Sensors held in the PAIR NEW SENSOR list. */
#define MAX_DEVS 12

/* Last connect attempt per link, so a burst of adverts yields ONE connect. */
/* PER-LINK RECONNECT THROTTLE, claimed atomically.
 *
 * Read it, compare it against the clock and write it back and that is a
 * read-modify-write on a binder thread, with one such thread per link and
 * adverts arriving in bursts. Two adverts for the same sensor landing
 * together both read the same stamp, both decide the window has passed, and
 * both call dexble_pair for the SAME link: two connects race, and the second
 * resets the first mid-handshake.
 *
 * A compare-exchange makes the claim the same act as the test, so exactly one
 * advert per link per window can win it. */
static _Atomic long g_link_try[LINK_MAX];

struct dev {
   char name[9];
   char mac[18];
   int rssi;
   unsigned count;
   long long last_log_ms;
   long seen_t;
};

static struct dev g_devs[MAX_DEVS];
static int g_ndevs;
/* ALL ADVERTS HEARD -- a pipe-health counter, and an ATOMIC one.
 *
 * It is incremented on a BLE binder thread for every scan result and read by
 * the main thread for the status line. As a plain `unsigned` that is a data
 * race in the C sense however benign the number looks: the two threads have
 * no ordering between them, the compiler may keep it in a register across the
 * loop, and ThreadSanitizer reports it (which is how it was found). Relaxed
 * is enough -- nothing is published THROUGH this counter, it only counts. */
static atomic_uint g_total;

/* Guards the pairing-candidate list (g_devs / g_ndevs).
 *
 * jni_on_advert runs on a BLE binder thread and both READS and WRITES g_ndevs
 * (find-slot loop, then increment), while the main looper reads it
 * (build_model, select_candidate, commit_pair gate) AND resets it to 0 on a
 * pairing action. A release store on the writer alone does not order the
 * plain-load readers on ARM, and two threads doing read-modify-write on g_ndevs
 * (binder increment vs main reset) is a lost-update race no single atomic
 * closes. A tiny leaf lock -- taken alone, never nested inside another lock and
 * never held across a call that takes one -- fixes both. */
static struct mutex g_devlist_lk = MUTEX_INIT;

static void devlist_lock(void)
{
   mutex_lock(&g_devlist_lk);
}

static void devlist_unlock(void)
{
   mutex_unlock(&g_devlist_lk);
}

/* The device a pick proposed, awaiting the PAIRCONF YES. Copied out of
 * g_devs at pick time: the candidate list keeps churning under the scan (and
 * is reset outright by pair_scan_start), so an index would go stale but a
 * copied identity cannot. Main-thread only. */
static char g_pend_mac[20], g_pend_name[12];

/* Smart pairing (PAIR NEW SENSOR): scans for candidates while the code is
 * typed, WITHOUT touching the currently-bonded sensor. On OK, pick by
 * proximity/count (see select_candidate); ambiguous -> SCR_DEVLIST for the
 * user to choose. */
/* ATOMIC, and read as ONE decision. jni_on_advert reads it on a binder thread
 * for every advert while the main thread sets and clears it; the
 * candidate-list lock does not cover it, and taking that lock here would put
 * a scan-rate binder callback behind the same mutex the renderer holds.
 * Acquire/release so that everything the main thread arranged BEFORE arming
 * smart pairing is visible to the advert that acts on it. */
static atomic_int g_smart_pairing;

/* PENDING pairing: the code is in but no candidate is on the air yet. The
 * old flow parked the user in the device list until the sensor deigned to
 * advertise -- with g_smart_pairing suppressing every OTHER sensor's
 * reconnect the whole time, so waiting for the new sensor cost the readings
 * of the ones already worn. Instead the intent is ARMED (the type awaited;
 * 0 = none), every menu closes, and the 1 Hz tick commits the pairing the
 * moment an unambiguous candidate appears. DEVICES shows a PENDING row
 * (tappable to cancel) so the armed state is visible, not mysterious. */
static int g_pend_pairing;

/* --- Java -> C: one advertisement heard (BLE binder thread) ---
 *
 * NOT the main thread: onAdvert is delivered from ScanCallback.onScanResult
 * (Ble.java), i.e. a Bluetooth-stack binder thread, while the main looper reads
 * AND resets g_devs/g_ndevs (build_model, select_candidate, commit_pair gate,
 * the pairing reset). Registry access below reads one snapshot; the
 * candidate-list write is taken under devlist_lock, which every reader/resetter
 * of g_ndevs also holds, so the read-modify-write increment here is atomic
 * against the main-thread reset and no reader sees a counted-but-unwritten
 * row. */

static void jni_on_advert(JNIEnv *env, jclass cls, jstring jname, jstring jmac,
                          jint rssi)
{
   (void)cls;
   const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
   const char *mac  = (*env)->GetStringUTFChars(env, jmac, NULL);
   /* GetStringUTFChars returns NULL on OOM (with an exception pending); on this
    * per-advert hot path a NULL deref would crash. Bail cleanly instead. */
   if (!name || !mac) {
      if (name)
         (*env)->ReleaseStringUTFChars(env, jname, name);
      if (mac)
         (*env)->ReleaseStringUTFChars(env, jmac, mac);
      if ((*env)->ExceptionCheck(env))
         (*env)->ExceptionClear(env);
      return;
   }

   atomic_fetch_add_explicit(&g_total, 1U, memory_order_relaxed);
   /* Which Dexcom families we will talk to at all. Stelo advertises "DX01",
    * G7 "DXCM"; both are supported. The safety property is NOT "never a G7" --
    * it is "never a sensor the user did not choose here":
    *   - once bonded, auto-connect ONLY to that exact sensor's MAC (s.mac), so
    *     a stranger's sensor in range is never touched, and
    *   - before pairing, only a device the user picks in ADD SENSOR is used.
    * PAIR NEW SENSOR (g_smart_pairing) suppresses the auto path and selects by
    * code + proximity instead.
    *
    * Note for testing, not for the code: the user's own G7 is a live medical
    * device and must not be exercised until they choose to do so themselves.
    * That is a discipline about which sensor you pair during a test, not a
    * restriction compiled into the app. */
   int is_dexcom =
       strncmp(name, "DX01", 4) == 0 || strncmp(name, "DXCM", 4) == 0;
   int is_meter = strncmp(name, "OneTouch", 8) == 0;
   /* The device list shows whichever family the user is currently adding, so a
    * meter is discoverable in ADD SENSOR -> ONETOUCH and a sensor is not
    * offered when they asked for a meter. */
   /* EXCLUDE DEVICES ALREADY IN THE REGISTRY from the pairing candidate list.
    *
    * The family filter alone let a sensor you have already paired appear as a
    * candidate -- and with just that one in range (the common case when you
    * enter the code before applying the replacement) select_candidate returns
    * it unopposed, the list never appears, and commit_pair runs on the LIVE
    * sensor's address: a J-PAKE re-pair against a sensor that is already
    * bonded and streaming, burning the link it was using. You cannot pair
    * something that is already paired, so it does not belong in the list. */
   /* THE REGISTRY, ONCE PER ADVERT. Three questions are asked of it below --
    * is this device already ours, is it a CGM we should reconnect, is it one
    * of our meters -- and one snapshot answers all three. Taken separately
    * they were three locked walks and three ~1.5 KB copies on a BINDER
    * thread, per advertisement, several a second: a measurable cost on the
    * hottest path in the app (pairingtest's concurrency case noticed it), and
    * three answers from three different instants where one device could
    * appear between two of them. */
   struct sensor_view v;
   sensors_view_get(&v);

   int known = 0;
   {
      int kidx  = -1;
      int kid   = -1;
      int kkind = KIND_BGM;
      for (int i = 0; i < v.n && kidx < 0; i++)
         if (v.have_rec[i] && !strcmp(v.rec[i].identity, mac)) {
            kidx  = i;
            kid   = v.slot[i].id;
            kkind = sensor_kind(v.rec[i].type);
         }
      if (kidx >= 0 && kkind == KIND_CGM) {
         /* A CGM is registered the moment the user COMMITS to pairing it, so
          * "has a slot" no longer implies "is paired". Exclude it from the
          * candidate list only once a BONDED session exists -- which is not
          * what a slot implies. Without this, one failed pairing (wrong code,
          * out of range) leaves the sensor registered-but-never-bonded and
          * permanently missing from ADD SENSOR: unretryable without first
          * forgetting the device, with nothing saying so. */
         int klink = link_for_sensor(kid);
         if (klink >= 0) {
            struct dex_session ks;
            /* A LINK THE DRIVER REFUSES IS NOT A BONDED SESSION. The refusal
             * zeroes ks, so `known` would come out 0 either way -- but the
             * answer is read rather than discarded because the two reasons
             * for 0 are different, and one of them means link_for_sensor
             * returned something this driver has never heard of. */
            known = driver_session_of(klink, &ks) && ks.bonded;
         }
      } else {
         known = (kidx >= 0); /* meters keep the pre-existing rule */
      }
   }
   int listed =
       !known &&
       ((sensor_kind(sel_add_type()) == KIND_BGM) ? is_meter : is_dexcom);
   /* ONE READ, ONE DECISION. Read again further down and the mode could have
    * changed in between, so one advert would be judged by two different
    * answers to the same question. */
   int smart = atomic_load_explicit(&g_smart_pairing, memory_order_acquire);
   if (is_dexcom && !smart) {
      /* Auto-connect ONLY to a sensor already in the registry, on ITS OWN
       * link. Matching against the registry rather than "the bonded sensor"
       * is what lets several CGMs stream at once: each advertises on its own
       * schedule and reconnects independently, so a stalled one cannot keep
       * another off the air. A device we never paired is ignored entirely --
       * that, not any family filter, is the safety property. */
      /* Snapshot the slot list under the registry lock before walking it.
       *
       * This runs on a BINDER thread while the main thread can be inside
       * sensor_forget's shift-down and another binder thread inside
       * srec_push's memmove. Reading slot_count() and holding a sensor_rec*
       * across the driver calls below is the exact hazard link_for_slot and
       * src_for_link were both rewritten to close -- a torn read here hands
       * dexble_pair a link resolved from a different sensor's identity, so one
       * sensor's address is bound to another's link and key file. */
      int n_ids = 0;
      int ids[MAX_SLOTS];
      int match[MAX_SLOTS];
      for (int i = 0; i < v.n && n_ids < MAX_SLOTS; i++) {
         if (v.slot[i].old) /* disconnected: never auto-reconnect */
            continue;
         if (!v.have_rec[i] || sensor_kind(v.rec[i].type) != KIND_CGM)
            continue;
         ids[n_ids]   = v.slot[i].id;
         match[n_ids] = (strcmp(v.rec[i].identity, mac) == 0);
         n_ids++;
      }
      for (int i = 0; i < n_ids; i++) {
         if (!match[i])
            continue;
         /* BY ID. This array is COMPACTED -- it skips non-CGM slots -- so
          * the row number here was never the registry's; passing `i` meant
          * that with a meter registered before a CGM (or after any forget
          * shifted one down), the CGM's advert resolved the METER's slot,
          * returned LINK_METER, found no session there and gave up: that
          * CGM's advert-driven reconnect never fired again, for the life of
          * the install, with no visible cause. Carrying the original index
          * fixed that; carrying the ID means there is no index to carry. */
         int link = link_for_sensor(ids[i]);
         if (link < 0)
            break;
         struct dex_session ls;
         /* takes the lock itself; a link it does not have is treated exactly
          * like a link with no MAC -- there is nothing to reconnect to */
         if (!driver_session_of(link, &ls))
            break;
         if (!ls.mac[0] && ls.paired) {
            /* REGISTERED AND KEYED, WITH NOTHING FOR THE LINK TO DIAL.
             *
             * The link's address cache (files/stelo.mac.<link>) is written
             * the first time a session actually STREAMS, so a pairing that
             * completed its key exchange and then lost the link before its
             * first reading leaves that link holding a key and no target --
             * and the startup recovery cannot supply one either, because it
             * asks the SYSTEM BOND LIST, which a sensor whose bond was
             * refused is not in. Nothing else revisits such a device: it
             * stays registered, unreachable, and shown as a healthy sensor
             * in warmup for as long as the install lasts.
             *
             * Everything needed to repair it is already in hand. The
             * registry holds the address this phone paired, this advert is
             * that exact device on the air (the identity matched above), and
             * ls.paired says the link holds its J-PAKE key -- so the
             * reconnect runs the saved-key path and asks for no applicator
             * code. Bind the address durably and go on to connect.
             *
             * ls.paired IS THE CONDITION, not a detail. Without a key on
             * this link a connect would demand a fresh pairing with a code
             * the user no longer has, which is why a link with neither key
             * nor address is still left to the ADD DEVICE flow below. */
            if (driver_bind_mac(link, mac) != BIND_PUBLISHED)
               break; /* target unchanged; the next advert tries again */
            if (!driver_session_of(link, &ls))
               break;
         }
         if (!ls.mac[0])
            break; /* registered but not yet paired: ADD DEVICE owns it */
         /* A sensor advertises repeatedly inside one wake window. Re-issuing
          * connect on every advert would be a connect storm -- hard on the
          * sensor's battery and a good way to strand the link -- so allow one
          * attempt per link per cycle, and none at all while it is already
          * delivering readings. */
         /* "Already streaming" must be judged from THIS sensor's own last
          * reading. g_cur_time is the global newest CGM sample, bound to the
          * PRIMARY sensor -- so a healthy primary made every other sensor look
          * live and suppressed its reconnect indefinitely. ctx->g_bonded is no
          * help either: it is set on auth and cleared only by driver_forget,
          * never on disconnect, so it stays 1 across a dropped link. */
         /* TWO CLOCKS, deliberately. `mine` is a reading's stored timestamp
          * -- a wall-clock INSTANT -- so "has this sensor produced anything
          * recently" is answered on the wall clock. The connect throttle
          * below is an INTERVAL between two events in this process, so it is
          * answered on the monotonic one: a clock correction must not let a
          * burst of adverts through, nor silence a sensor's reconnect for
          * however far the clock moved. */
         long tnow = realtime_s();
         long mnow = mono_s();
         /* THE QUESTION, ASKED: this sensor's newest CGM instant.
          * A count/index walk under a hand-taken store lock, with the
          * not-a-fingerstick rule spelled out, was the fourth copy of it. */
         long mine = hist_newest_t(ids[i]);
         if (ls.bonded && mine && tnow - mine < 300)
            break; /* this sensor really is streaming */
         /* CLAIM THE WINDOW, or lose it. Not "read, compare, write": that is
          * three steps, and a second advert on another binder thread fits
          * between any two of them. */
         long last =
             atomic_load_explicit(&g_link_try[link], memory_order_relaxed);
         if (mnow - last < 30)
            break;
         if (!atomic_compare_exchange_strong_explicit(
                 &g_link_try[link], &last, mnow, memory_order_acq_rel,
                 memory_order_relaxed))
            break; /* somebody else claimed this link's window first */
         /* The LINK, not the address or the advertised name -- both identify
          * the hardware on the wearer's arm, and this fires on every advert
          * that wins the reconnect window. */
         LOGI("a registered sensor advertised -> reconnect on link %d", link);
         /* READ HERE, not at the top: this is the per-advert hot path and
          * every scan result would otherwise pay for a copy of the whole
          * settings aggregate to reach one string on the rare branch that
          * needs it. */
         struct prefs sp;
         settings_get(&sp);
         dexble_pair(link, mac, sp.code_str);
         break;
      }
   }
   /* A OneTouch meter advertises only while the user has it switched on, so
    * seeing it IS the trigger: sync now, on its own link, without disturbing
    * the CGM link. No polling -- a meter that is off costs nothing. */
   if (is_meter && !meter_busy()) {
      /* Resolve the meter from THIS advert's address, against every registered
       * meter slot -- not against a single remembered MAC.
       *
       * sensor_reconcile binds meter_src() to the FIRST
       * OneTouch slot it finds, so with two meters registered the second could
       * never sync: its adverts fail the address test forever, silently, with
       * no user-visible cause. Matching per advert keeps the safety property
       * a fixed address test has -- a stranger's meter is still ignored,
       * because it has no slot. */
      int mid = -1;
      for (int i = 0; i < v.n && mid < 0; i++) {
         if (v.slot[i].old) /* a disconnected meter is inert */
            continue;
         if (v.have_rec[i] && v.rec[i].type == SENSOR_ONETOUCH &&
             !strcmp(v.rec[i].identity, mac))
            mid = v.slot[i].id;
      }
      /* PER-METER throttle: only rate-limit THIS meter, so one meter syncing
       * never blocks another (the global gate here made a second meter that
       * advertised alongside the first never get a turn). */
      /* MONOTONIC: this is "have we already answered this meter's adverts in
       * the last minute", an interval between two events in this process.
       * meter_seen() is the same event on the wall clock and is what the
       * screen shows; measuring the throttle against it let a forward clock
       * correction wake the meter on every advert of one wake window. */
      /* THE THROTTLE AND THE RECORD ARE ONE STEP (meter.h). This read the
       * stamp, decided, and then recorded -- three steps, and two scan
       * callbacks for the same meter could both get through and issue two
       * connects during the one second it is awake. */
      if (mid > 0 && meter_note_advert(mid, rssi, realtime_s(), 60)) {
         /* The advertisement IS the "last seen" event, and it carries an
          * RSSI -- so SIGNAL STRENGTH is stamped from the same advert, not
          * left blank until a connection completes (a meter that advertised
          * but did not finish a sync would otherwise show LAST SEEN with a
          * "--" signal). A completed sync's connection RSSI refines it
          * afterwards, and this must be recorded BEFORE the sync starts,
          * which persists it. */
         /* Already armed? Then the controller is initiating on its own and
          * re-issuing the connect would only cancel and restart it -- during
          * the one second the meter is awake. With every meter armed that is
          * the normal case, so this path is now only a fallback for a meter
          * that could not get a link of its own. */
         /* Skip, do NOT return: this is still an advertisement the device
          * list below has to record, and returning would silently drop the
          * meter out of the ADD DEVICE list. */
         if (!meter_armed(mac)) {
            /* The registry id, not the address: the id is what every other
             * meter line keys on and it is meaningless off this phone. */
            LOGI("meter id %d advertising, not armed -> connect", mid);
            meter_sync_start(mid, mac);
         }
      }
   }

   if (listed) {
      int did_log_new        = 0;
      unsigned did_log_count = 0;
      /* WHICH CANDIDATE ROW, so the two lines below can name a device
       * without naming the DEVICE. See the log calls after the unlock. */
      int log_slot = -1;
      devlist_lock();
      int i = 0;
      for (i = 0; i < g_ndevs; i++)
         if (strcmp(g_devs[i].mac, mac) == 0)
            break;
      int is_new = (i == g_ndevs && g_ndevs < MAX_DEVS);
      if (is_new || i < g_ndevs) {
         /* A NEW row starts from zero. The reset (pair_scan_start,
          * pairing_forget_candidates) only drops g_ndevs, so the array keeps
          * the previous scan's fields -- and a device heard again reused its
          * old advert count, so the pipe-health figure the screen shows was
          * the total from every scan session, not this one's. */
         if (is_new)
            g_devs[i] = (struct dev){0};
         /* Fill the slot, then publish a newly-added one by bumping g_ndevs.
          * All of this is under devlist_lock, so the find/increment is atomic
          * against the main-thread reset and readers never see a
          * counted-but-unwritten row. An existing slot is only ever rewritten
          * with its own matched mac. */
         (void)snprintf(g_devs[i].name, sizeof g_devs[i].name, "%s", name);
         (void)snprintf(g_devs[i].mac, sizeof g_devs[i].mac, "%s", mac);
         g_devs[i].rssi = rssi;
         /* MONOTONIC. This stamp is only ever used as an interval -- "is this
          * candidate still on the air" (60 s, see select_candidate) -- and an
          * armed pairing evaluates it on every tick, possibly hours after the
          * list was built. On the wall clock a correction forward made every
          * candidate stale at once (so a pairing waited for a fresh advert
          * that had already arrived), and one backward made a sensor that
          * left the room look present. */
         g_devs[i].seen_t = mono_s();
         g_devs[i].count++;
         log_slot = i;
         if (is_new) {
            g_ndevs++;
            did_log_new = 1;
         }
         /* one cadence line per device per 30 s, to time advert bursts */
         long long now = now_ms();
         if (now - g_devs[i].last_log_ms > 30000) {
            g_devs[i].last_log_ms = now;
            did_log_count         = g_devs[i].count; /* nonzero -> log below */
         }
      }
      devlist_unlock();
      /* Log OUTSIDE the lock -- LOGI is not part of the guarded state and can
       * be slow; the lock is a leaf held only across the field writes. */
      /* THE CANDIDATE SLOT, NOT THE ADDRESS OR THE ADVERTISED NAME.
       *
       * Both of these printed the BLE address of every Dexcom sensor the
       * radio could hear -- the wearer's own and every stranger's in the
       * room -- at first sight and then every 30 s for as long as scanning
       * ran. A G7/Stelo advertises as "DXCM" plus characters derived from
       * its serial, so the name is a second hardware identifier and not a
       * safer substitute for the first. The slot index is what the device
       * list, select_candidate and pairing_pick all key on, so it is what
       * makes these lines followable, and it means nothing outside this
       * process. */
      if (did_log_new)
         LOGI("new Dexcom candidate in slot %d (rssi %d)", log_slot, rssi);
      if (did_log_count)
         LOGI("dexcom adv: slot %d rssi %d count %u", log_slot, rssi,
              did_log_count);
   }

   (*env)->ReleaseStringUTFChars(env, jname, name);
   (*env)->ReleaseStringUTFChars(env, jmac, mac);
   shell_ui_dirty();
}

/* Begin collecting pairing candidates WITHOUT disturbing the current sensor:
 * a passive scan only (the existing bond keeps reconnecting by MAC on its
 * own). g_smart_pairing suppresses the first-DX auto-pair so nothing is
 * touched until the user commits to a specific sensor. */
void pair_scan_start(void)
{
   atomic_store_explicit(&g_smart_pairing, 1, memory_order_release);
   devlist_lock(); /* atomic vs the binder-thread advert writer */
   g_ndevs = 0;    /* fresh candidate list */
   devlist_unlock();
   if (shell_activity())
      start_scan(shell_activity());
}

/* Abandon pairing: stop the candidate scan; the existing bond is untouched.
 */
void pair_cancel(void)
{
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
   if (shell_activity())
      stop_scan(shell_activity());
}

/* Choose which scanned sensor to pair:
 *   0 found  -> -1 (show the list; it fills as the scan continues)
 *   1 found  -> that one
 *   >1 found -> the strongest IF it beats the next by >= 20 dB (clearly the
 * one on your body); otherwise -1 (ambiguous -> let the user pick).
 */
int select_candidate(void)
{
   /* Decision in scanlogic.c so `make check` can fail on it: with the rule
    * inline here, deleting it passes the entire gate. */
   /* Only candidates heard RECENTLY qualify. The list is never pruned, so a
    * sensor that left the room an hour ago still sits there with its stale
    * RSSI -- and a pending pairing evaluates this on every tick, possibly
    * long after the list was built. Comparing a stale RSSI against a fresh
    * one under the 20 dB rule picks wrong exactly when it matters. */
   int rssi[MAX_DEVS];
   int map[MAX_DEVS];
   int n    = 0;
   long now = mono_s(); /* an INTERVAL: see the seen_t stamp in jni_on_advert */
   devlist_lock(); /* consistent (count, rssi[]) vs the binder-thread writer
                    */
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++) {
      if (g_devs[i].seen_t > 0 && now - g_devs[i].seen_t > 60)
         continue;
      rssi[n] = g_devs[i].rssi;
      map[n]  = i;
      n++;
   }
   devlist_unlock();
   int p = scan_pick_candidate(rssi, n);
   return (p < 0) ? -1 : map[p];
}

/* How many candidates are FRESH on the air right now (same 60 s window as
 * select_candidate). One is the unambiguous-by-existence case: with a single
 * sensor of the requested family in range, a confirmation list of one is
 * pure ceremony. */
int fresh_candidates(void)
{
   int n    = 0;
   long now = mono_s(); /* the same interval rule as select_candidate */
   devlist_lock();
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++)
      if (g_devs[i].seen_t == 0 || now - g_devs[i].seen_t <= 60)
         n++;
   devlist_unlock();
   return n;
}

/* LAND ON THE NEW DEVICE after a successful pairing commit, rather than
 * dropping the user back where they started.
 *
 * Ending by simply closing the keypad returns to the DEVICES list or the main
 * screen with nothing to show for it -- the tap reads as having done nothing
 * even though it worked, because a fresh CGM has no reading for its whole
 * warmup hour. The per-device screen is where that hour
 * is legible: it carries the WARMUP countdown, the session state and the
 * pairing outcome, so the flow ends looking at the thing it just created.
 *
 * Call AFTER keypad_close(): that leaves cur_screen() at the flow's own return
 * target, and THAT is the origin recorded for the X -- never a hardcoded
 * screen. Backing out of the new device therefore lands exactly where the add
 * flow would have, which is the same record-the-origin rule MA_SENSOR follows.
 */
void open_new_device(int id)
{
   if (!sensor_slot_of(id, 0))
      return;
   sel_set_device(id);
   nav_go(SCR_SENSOR);
}

/* Commit to a specific sensor: NOW drop the existing bond and pair the chosen
 * MAC
 * with the entered code. Only reached after the code is in and a candidate
 * is chosen (auto or from the list). */
void commit_pair(const char *mac)
{
   struct prefs sp;
   settings_get(&sp);
   /* PAIRING MODE ENDS HERE, on every path.
    *
    * g_smart_pairing was cleared only on the two success paths, so any of
    * the four early returns below (meter busy, mint failed, slots full, no
    * free link) left it latched at 1 forever. jni_on_advert gates the whole
    * advert-driven reconnect on !g_smart_pairing, and the on_timer scan
    * self-heal is gated on it too -- so a single failed pairing attempt
    * stopped every already-paired CGM from ever reconnecting again, with
    * nothing on screen to say so: the advert counter keeps climbing while
    * the reading quietly stops ageing forward. The user has committed to a
    * device by the time we are called; whether it works out does not change
    * that. */
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
   g_pend_pairing = 0; /* any commit supersedes an armed pending pairing */
   /* A meter has no key exchange: it bonds at the OS level (the meter shows
    * a passkey, Android prompts for it) the first time we touch its GATT. So
    * "pairing" one is just registering it and connecting -- the bond happens
    * as a side effect of the sync, and a refused connection reports back as
    * METER: NOT PAIRED rather than failing silently. */
   if (sensor_kind(sel_add_type()) == KIND_BGM) {
      /* REFUSE while another meter is mid-sync.
       *
       * The advert path gates on !meter_busy(); this one did not, and it
       * resets the SAME otble statics. A user in ADD SENSOR -> ONETOUCH is
       * there precisely because a scan is running and adverts are flowing,
       * so meter A can be walking records on a binder thread when they tap
       * meter B here. The main thread then runs ot_init() concurrently --
       * phase to P_IDLE mid-walk, last_index replaced, meter_src() repointed
       * -- and three things break permanently: A's remaining fingersticks
       * are written to readings.csv under B's id (append-only, never
       * rewritten); meter_index_save stores A's walk position under B's id,
       * which is the cross-meter corruption the per-meter index file exists
       * to prevent; and the interrupted sync sends no ack, wedging until the
       * 90 s watchdog. The sync is seconds long and self-clears, so refusing
       * costs the user a retry and nothing else. */
      if (meter_busy()) {
         LOGI("refusing to pair a meter while another is mid-sync");
         set_status("METER BUSY, RETRY");
         keypad_close();
         return;
      }
      int id = sensor_mint(sel_add_type(), mac, "", "", "", 0);
      if (id < 0) {
         set_status("METER: REGISTER FAILED");
         keypad_close();
         return;
      }
      /* A CLAIM THAT WAS NOT WRITTEN IS NOT A CLAIM: the table is full, or
       * slots.csv could not be replaced. Either way the device would be gone
       * at the next launch, so nothing below -- the bond, the key file, the
       * connect -- may happen. */
      if (sensor_claim_slot(id, sel_add_type(), mac) < 0) {
         set_status("METER NOT REGISTERED");
         keypad_close();
         return;
      }
      meter_bind(id, mac);
      /* Everything from here -- seeding this meter's record index, arming
       * its link, asking for the OS bond and issuing the connect -- is the
       * meter runtime's, not the shell's (see meter.h). The shell only
       * quiets the radio first: bonding while a scan is running is what made
       * the passkey prompt arrive minutes late. */
      if (shell_activity()) {
         scan_hold_until(mono_s() + 20); /* quiet radio to bond: an INTERVAL */
         stop_scan(shell_activity());
      }
      if (meter_pair(id, mac))
         set_status("METER: PAIRING");
      else
         set_status("METER: NOT PAIRED");
      keypad_close();
      /* Even on a failed connect: the slot IS registered, and its own screen
       * is where the failure is stated and retried. Returning to the list
       * would hide the one row that explains what happened. */
      open_new_device(id);
      return;
   }

   /* A new sensor pairs on the first free CGM link, so pairing a second
    * sensor neither disturbs nor replaces one that is already streaming. */
   int link = link_for_new_sensor();
   if (link < 0) {
      set_status("NO FREE SENSOR LINK");
      LOGI("refusing to pair: all %d links in use", LINK_MAX);
      keypad_close();
      return;
   }
   /* Register the sensor NOW, before the radio work: the user has committed
    * to this device, and the DEVICES list must say so immediately -- waiting
    * for the first reading (a minute or more) reads as the tap having done
    * nothing. The row is minted BARE (activation unknown, no DIS strings
    * yet); sensor_reconcile completes those attributes in place once they
    * arrive (sensor_complete), so nothing wrong is ever written to the
    * append-only file -- only nothing-yet. Registration BEFORE driver_forget
    * below, so a refusal here leaves the existing bond untouched. */
   int newdev = -1; /* the DEVICE this flow ends on, by id */
   {
      int cgm_type = (sensor_kind(sel_add_type()) == KIND_CGM) ? sel_add_type()
                                                               : SENSOR_STELO;
      int id       = sensor_mint(cgm_type, mac, "", "", "", 0);
      if (id < 0) {
         set_status("SENSOR: REGISTER FAILED");
         keypad_close();
         return;
      }
      /* See the meter path above: an unwritten claim must not reach the
       * radio, because driver_forget below erases this link's key file. */
      if (sensor_claim_slot(id, cgm_type, mac) < 0) {
         set_status("SENSOR NOT REGISTERED");
         keypad_close();
         return;
      }
      newdev = id; /* the screen this flow ends on -- see open_new_device */
      /* The id and the type. The address it was registered against is in
       * slots.csv, which is private app storage; logcat is not. */
      LOGI("registered sensor id=%d type=%s at pairing commit", id,
           sensor_type_name(cgm_type));
   }
   /* The LINK is named, so there is no selection to race: this erases the key
    * and MAC files of the link being paired and no other. */
   driver_forget(link);
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
   if (shell_activity()) {
      scan_hold_until(mono_s() + 20); /* quiet radio for the J-PAKE */
      stop_scan(shell_activity());
   }
   set_status("PAIRING");
   /* Ask for the OS bond BEFORE the GATT work, for the same reason as the
    * meter above: the dialog then belongs to the tap that caused it.
    *
    * Safe against the J-PAKE that follows. Ble.createBond returns immediately
    * when the device is already BONDED or already BONDING, so this cannot
    * restart a bond mid-flight; when it is neither, the request goes out and
    * dexble_pair's connectGatt (autoConnect=true) attaches to the same device.
    * The sensor asks for security itself a few seconds into the connection
    * anyway -- an HCI capture shows the full LE Secure Connections exchange
    * completing seven seconds before the first EGV -- so this only moves the
    * prompt earlier, it does not add one that was not going to happen. */
   dexble_create_bond(mac);
   if (shell_activity())
      dexble_pair(link, mac, sp.code_str);
   keypad_close();
   /* The whole point: a fresh CGM shows nothing for an hour, so end the flow
    * on the screen that counts that hour down. */
   open_new_device(newdev);
   /* THE OUTCOME AND THE LINK -- NEVER THE CODE, NEVER THE ADDRESS.
    *
    * `pair new sensor %s with code %s on link %d` spells the sensor's BLE
    * address and the four-digit J-PAKE code out in full. That code is the
    * shared secret the whole EC-J-PAKE exchange proves
    * knowledge of: it is what authenticates THIS phone to THAT sensor, it is
    * printed on the applicator and typed once, and it is not rotatable for
    * the life of the wear. Beside it sat the address needed to find the
    * sensor on the air. logcat is not private -- `adb logcat` reads it from
    * any machine the phone is plugged into, an ANR/tombstone bug report
    * carries it off the device, and this app's own crash path writes app
    * context to a file -- so the two halves of "impersonate this wearer's
    * sensor link" were being published together at every pairing.
    *
    * What a reader of the log actually needs is that a pairing was committed
    * and which link it went to; the sensor id is already logged by the
    * registration line above. */
   LOGI("pair new sensor: committed on link %d", link);
}

/* WHAT A TYPED NUMBER MEANT: a sensor's pairing code. The one entry that ends
 * at the radio rather than in storage, which is why it is last and on its own:
 * it either commits a pairing now or arms one for the next candidate to
 * advertise. */
int kp_commit_pair(void)
{
   if (forms_kp_len() == 4) { /* PAIR: code in, now pick the sensor */
      char code[8];
      forms_kp_text(code, sizeof code);
      /* The code is what the J-PAKE exchange proves knowledge of: one that
       * is used but not stored pairs now and fails silently after the next
       * launch, with no way for the user to know why. */
      if (code_set(code) != SETTINGS_OK) {
         /* AND STOP. The setter rolls back, so what is stored is the
          * PREVIOUS code -- and commit_pair reads the stored one, so going
          * on would run the J-PAKE exchange with a secret the user did not
          * type. It would fail at round 3 with nothing on screen to explain
          * it, and burn one of the three attempts the code allows. */
         set_status("CODE NOT SAVED");
         return 0;
      }
      int idx = select_candidate();
      if (idx >= 0 && fresh_candidates() == 1) {
         /* Exactly ONE sensor of this family on the air: pair it NOW.
          * The code plus a lone candidate is as unambiguous as it gets --
          * a confirmation list of one is ceremony, and the J-PAKE code
          * itself rejects a wrong device. */
         char macbuf[sizeof g_devs[0].mac];
         devlist_lock();
         str_snapshot(macbuf, sizeof macbuf, g_devs[idx].mac);
         devlist_unlock();
         commit_pair(macbuf);
      } else if (idx >= 0) {
         /* Several candidates, one clear by proximity: PROPOSE it. The
          * confirmation showing name + address is what lets the user
          * catch a wrong auto-pick before it costs a bond. */
         devlist_lock();
         str_snapshot(g_pend_mac, sizeof g_pend_mac, g_devs[idx].mac);
         str_snapshot(g_pend_name, sizeof g_pend_name, g_devs[idx].name);
         devlist_unlock();
         nav_go(SCR_PAIRCONF);
      } else {
         /* No candidate on the air yet: ARM the pairing and free the
          * user. Parking them in the device list until the sensor
          * advertised also kept g_smart_pairing latched, which suppresses
          * every OTHER sensor's reconnect -- waiting for the new sensor
          * cost the readings of the ones already worn. The 1 Hz tick
          * commits the moment an unambiguous candidate appears; DEVICES
          * shows the armed state as a PENDING row. */
         g_pend_pairing = sel_add_type();
         atomic_store_explicit(
             &g_smart_pairing, 0,
             memory_order_release); /* other sensors reconnect freely again */
         set_status("PAIRING PENDING");
         LOGI("pairing armed: awaiting a %s candidate",
              sensor_type_name(sel_add_type()));
         keypad_close();
         /* END ON THE PENDING ROW, the way a completed pairing ends on the
          * new device's screen (open_new_device). Arming produces no device,
          * so the only thing that says the code was accepted is the PENDING
          * row on the device list -- and closing the keypad alone drops the
          * user back where the flow began, which from the main screen is a
          * four-digit code typed for no visible result.
          *
          * AFTER keypad_close(), for the reason open_new_device gives: that
          * call leaves cur_screen() at the flow's own return target, so THAT
          * is the origin nav_back records for this screen's X. */
         nav_go(SCR_DEVICES);
      }
   } else {
      return COMMIT_PASS;
   }
   return COMMIT_DONE;
}

/* --- what the rest of the app is allowed to ask (see pairing.h) --- */

int pairing_pending(void)
{
   return g_pend_pairing;
}

int pairing_smart(void)
{
   return atomic_load_explicit(&g_smart_pairing, memory_order_acquire);
}

void pairing_arm(int type)
{
   g_pend_pairing = type;
}

const char *pairing_pend_mac(void)
{
   return g_pend_mac;
}

const char *pairing_pend_name(void)
{
   return g_pend_name;
}

void pairing_propose(const char *mac, const char *name)
{
   str_snapshot(g_pend_mac, sizeof g_pend_mac, mac);
   str_snapshot(g_pend_name, sizeof g_pend_name, name);
}

/* A COPY, under the list lock. The binder thread keeps rewriting the real
 * list, so the renderer must not walk it: a half-written entry is a device
 * shown with another's address. */
int pairing_candidates(struct pair_cand *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return 0;
   devlist_lock();
   for (int i = 0; i < g_ndevs && n < cap; i++) {
      str_snapshot(out[n].name, sizeof out[n].name, g_devs[i].name);
      str_snapshot(out[n].mac, sizeof out[n].mac, g_devs[i].mac);
      out[n].rssi   = g_devs[i].rssi;
      out[n].count  = g_devs[i].count;
      out[n].seen_t = g_devs[i].seen_t;
      n++;
   }
   devlist_unlock();
   return n;
}

unsigned pairing_adverts_seen(void)
{
   return atomic_load_explicit(&g_total, memory_order_relaxed);
}

/* Register the advert callback on the Ble class. The natives belong to the
 * workflow that implements them, not to whoever happens to hold the class. */
int pairing_register(JNIEnv *env, jclass ble)
{
   if (!env || !ble)
      return 0;
   /* char[] (not literals) so the char* JNINativeMethod fields need no const
    * cast. */
   static char nm_advert[] = "onAdvert";
   static char sg_advert[] = "(Ljava/lang/String;Ljava/lang/String;I)V";
   static const JNINativeMethod methods[] = {
       {nm_advert, sg_advert, (void *)jni_on_advert},
   };
   return (*env)->RegisterNatives(env, ble, methods, 1) == 0;
}

int pairing_candidate_count(void)
{
   devlist_lock();
   int n = g_ndevs;
   devlist_unlock();
   return n;
}

void pairing_forget_candidates(void)
{
   devlist_lock();
   g_ndevs = 0;
   devlist_unlock();
}

/* A pick PROPOSES; only the confirmation screen's YES commits. The list is
 * ordered by live RSSI, so rows reorder under the finger -- pairing on the
 * raw tap let one mis-press register the wrong device and drop a bond. The
 * (mac, name) pair is copied under the list lock, against the binder-thread
 * writer. Returns 0 when the row no longer exists. */
int pairing_pick(int idx)
{
   int ok = 0;
   devlist_lock();
   if (idx >= 0 && idx < g_ndevs) {
      str_snapshot(g_pend_mac, sizeof g_pend_mac, g_devs[idx].mac);
      str_snapshot(g_pend_name, sizeof g_pend_name, g_devs[idx].name);
      ok = 1;
   }
   devlist_unlock();
   return ok;
}

/* THE 1 Hz STEP: an armed pairing commits itself the moment an unambiguous
 * candidate is on the air. */
void pairing_tick(void)
{
   /* An ARMED pairing commits itself the moment an unambiguous candidate is
    * on the air (fresh adverts only -- select_candidate's 60 s window). Main
    * thread only, like every other commit_pair caller. keypad_close() runs
    * inside commit_pair, so aim it at the CURRENT menu first -- a background
    * commit must never yank the user out of whatever screen they are on. */
   if (g_pend_pairing && sensor_kind(g_pend_pairing) == KIND_CGM &&
       cur_screen() != SCR_KEYPAD && cur_screen() != SCR_DEVLIST &&
       cur_screen() != SCR_PAIRCONF) {
      int pidx = select_candidate();
      if (pidx >= 0) {
         char pmac[sizeof g_devs[0].mac];
         int have = 0;
         devlist_lock();
         if (pidx < g_ndevs) {
            str_snapshot(pmac, sizeof pmac, g_devs[pidx].mac);
            have = 1;
         }
         devlist_unlock();
         if (have) {
            LOGI("pending pairing: candidate in slot %d appeared -> "
                 "committing",
                 pidx);
            sel_set_add_type(g_pend_pairing);  /* commit_pair branches on it */
            forms_kp_return_set(cur_screen()); /* stay on the current screen */
            commit_pair(pmac);                 /* clears g_pend_pairing */
         }
      }
   }
}

void pairing_stop_smart(void)
{
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
}
