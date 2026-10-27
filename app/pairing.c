// SPDX-License-Identifier: GPL-3.0
// pairing.c --- Adverts in, a registered device out (see pairing.h)
// Copyright 2026 Jakob Kastelic

#include "pairing.h"
#include "bletrans.h"
#include "clock.h"
#include "dexdriver.h"
#include "forms.h"
#include "linkinfo.h"
#include "log.h"
#include "menu.h" /* menu_pair_mark_here: the list the tick opens */
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
 * closes. A tiny leaf lock -- never held across a call that takes another --
 * fixes both.
 *
 * IT IS NESTED, INNERMOST, UNDER THE HISTORY LOCK. A frame holds the history
 * lock across build_model, and fill_sensor asks pairing_adv_name for each
 * device row, so this is taken and released inside that hold once per row.
 * That is safe because nothing under this lock reaches for another -- every
 * region below does string and clock work only -- so it cannot be half of a
 * cycle. What it does cost is a spin: struct mutex has no timeout, so the
 * main thread waits here while a binder advert thread holds it. The regions
 * are a bounded walk of a table of at most a few dozen rows, which is what
 * keeps that wait short. */
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
/* THE NAME ANDROID KNOWS A DEVICE BY, for devices we have already registered.
 *
 * The candidate list only holds sensors that are NOT ours (that is what makes
 * them candidates), so a registered sensor's advertised name is seen on every
 * advert and kept nowhere. It is the name the Bluetooth settings list shows,
 * which is the only thing tying a row in this app to a row in that one -- and
 * the difference between forgetting the right bond and the wrong one.
 *
 * In memory only: it costs one advert to relearn, and a name recorded into
 * the append-only provenance file would be a second identity for a device
 * that already has one. Under the devlist lock, like every other field the
 * binder advert path writes. */
/* SIZED BY WHAT IS ON THE AIR, not by what the registry can hold. Every
 * Dexcom in range is recorded, strangers included, and the table is searched
 * once per advertisement on a binder thread -- so its length is a cost paid
 * continuously, and a room with a hundred sensors in it must not make that
 * walk a hundred long. Thirty-two is well past the number of Dexcoms within
 * radio range of one phone.
 *
 * FULL MEANS REUSE THE OLDEST, round-robin, because there is nothing here to
 * lose: one advertisement from that device puts its name straight back. */
#define ADVNAME_MAX 32

static struct {
   char mac[20];
   char name[9];
} g_advname[ADVNAME_MAX];

static int g_nadvname;
static int g_advname_next; /* the seat the next unknown address takes */

static int g_pend_pairing;
/* WHEN the code was accepted, so the pending screen can say how long it has
 * been waiting. A wait with no elapsed time on it is indistinguishable from a
 * wait that is not happening, which is the complaint this answers. */
static long g_pend_at;
/* Whether this arm has already offered the candidate list. One arm asks once:
 * the tie usually persists for as long as both sensors are on the air, and a
 * screen that reopened every second would be unusable. Cleared when a new
 * wait begins (pairing_arm, a new code in kp_commit_pair) and when a CGM
 * commit ends the wait; a refused commit that re-arms the same wait keeps
 * it. */
static int g_amb_asked;
/* THE DEVICE A REFUSED COMMIT LEAVES WAITING: the slot this wait registered,
 * or the sensor's own slot when it already held one. 0 when the wait has
 * none. STOP on the CGM PENDING screen moves it to OLD DEVICES unless it
 * has streamed (pairing_pending_device, menu.c). */
static int g_pend_id;
/* THE SENSORS THE USER SAID NO TO on the confirmation, which the wait does
 * not then commit to on its own: only a new code, or the user's own YES,
 * pairs one. As many as the candidate list holds; past that the oldest NO
 * gives way. Main thread only. */
static char g_declined[MAX_DEVS][sizeof g_pend_mac];
static int g_ndeclined;   /* rows in use */
static int g_declined_at; /* the row reused next once all are in use */

/* Is `mac` one the user said NO to? */
static int declined(const char *mac)
{
   for (int i = 0; i < g_ndeclined; i++)
      if (strcmp(g_declined[i], mac) == 0)
         return 1;
   return 0;
}

/* Forget every NO, or (mac non-NULL) the one for `mac`. */
static void undecline(const char *mac)
{
   if (!mac) {
      g_ndeclined   = 0;
      g_declined_at = 0;
      return;
   }
   for (int i = 0; i < g_ndeclined; i++)
      if (strcmp(g_declined[i], mac) == 0)
         g_declined[i][0] = 0; /* matches no address */
}

/* Is slot `id` still a live device, not DISCONNECTED: asked of the registry
 * itself, not of a view taken earlier. */
static int slot_still_live(int id)
{
   struct sensor_slot s;
   return sensor_slot_of(id, &s) && !s.old;
}

/* THE ADVERTISED NAME PREFIX OF EACH CGM TYPE (sensors.h): a Stelo is
 * "DX01", a G7 "DXCM". NULL for a type that is not a CGM. */
static const char *cgm_prefix(int type)
{
   switch (type) {
      case SENSOR_STELO: return "DX01";
      case SENSOR_G7: return "DXCM";
      default: return 0;
   }
}

/* Does the device at `mac` advertise the name of CGM type `type`? Asked of
 * its candidate row, else of the names its adverts recorded. */
static int cgm_named(const char *mac, int type)
{
   const char *pfx = cgm_prefix(type);
   if (!pfx)
      return 0;
   char nm[16] = "";
   devlist_lock();
   for (int i = 0; i < g_ndevs && !nm[0]; i++)
      if (strcmp(g_devs[i].mac, mac) == 0)
         str_snapshot(nm, sizeof nm, g_devs[i].name);
   devlist_unlock();
   if (!nm[0])
      (void)pairing_adv_name(mac, nm, (int)sizeof nm);
   return strncmp(nm, pfx, 4) == 0;
}

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
    * of our meters -- and one snapshot answers all three. Asked separately they
    * are three locked walks and three ~1.5 KB copies on a BINDER thread, per
    * advertisement, several a second: a measurable cost on the hottest path in
    * the app, and three answers from three different instants, where one device
    * can appear between two of them. */
   /* BY REFERENCE, not by copy: this is the advert path and it runs several
    * times a second. The picture is still one instant and still cannot shift
    * while the driver calls below run -- it is simply the instant the
    * registry last changed, handed out rather than rebuilt. */
   const struct sensor_view *vp = sensors_view_ref();

   int known = 0;
   {
      /* THE INDEX, not a walk: this runs on every advertisement, and a walk
       * here compares every device ever registered. */
      int kidx  = sensors_view_find_mac(vp, mac);
      int kid   = -1;
      int kkind = KIND_BGM;
      if (kidx >= 0) {
         kid   = vp->slot[kidx].id;
         kkind = sensor_kind(vp->rec[kidx].type);
      }
      if (kidx >= 0 && kkind == KIND_CGM) {
         /* A CGM is registered the moment the user COMMITS to pairing it, so
          * "has a slot" does not imply "is paired". Exclude it from the
          * candidate list once a BONDED session exists, or its link holds the
          * key a pairing completed with: "bonded" lives in memory only, and
          * the key is what says so across a restart. A slot alone is not
          * enough. Without this, one failed pairing (wrong code,
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
            /* ...or holds THIS sensor's own saved code: a pairing cut short
             * is finished by the advert path with that code, and committing
             * another code to it would replace the right one. A refused code
             * is saved empty, so that sensor stays a candidate. */
            char kcode[12];
            known =
                driver_session_of(klink, &ks) &&
                (ks.bonded || ks.paired ||
                 (dexble_code_for(mac, kcode, (int)sizeof kcode) && kcode[0]));
         }
      } else {
         known = (kidx >= 0); /* meters keep the pre-existing rule */
      }
      /* A DEXCOM THAT ONCE RAN A SESSION WITH THIS PHONE IS NEVER A NEW
       * SENSOR, unless it holds a live slot it has not yet bonded on -- the
       * one being paired. A sensor keeps advertising for weeks after its
       * session ends, and offering an old one as a candidate pairs the new
       * code to it. One that never streamed stays a candidate even when its
       * slot was moved to OLD DEVICES (a wait stopped before it was ever
       * dialled): committing it revives that slot (sensor_claim_slot). */
      if (!known && is_dexcom) {
         int live = kidx >= 0 && kidx < vp->n_live;
         if (!live && sensor_ever_streamed(mac))
            known = 1;
      }
   }
   /* RECORDED FOR EVERY DEXCOM, ours or not: `known` decides whether it is a
    * pairing candidate, not whether its name is worth knowing -- and it is
    * precisely the registered ones whose name no other table keeps. */
   if (is_dexcom && name[0]) {
      devlist_lock();
      int a = 0;
      for (; a < g_nadvname; a++)
         if (strcmp(g_advname[a].mac, mac) == 0)
            break;
      if (a == g_nadvname) {
         if (g_nadvname < ADVNAME_MAX) {
            g_nadvname++;
         } else {
            a              = g_advname_next;
            g_advname_next = (g_advname_next + 1) % ADVNAME_MAX;
         }
      }
      str_snapshot(g_advname[a].mac, sizeof g_advname[a].mac, mac);
      str_snapshot(g_advname[a].name, sizeof g_advname[a].name, name);
      devlist_unlock();
   }
   /* THE FAMILY BEING ADDED, and for a CGM its own type: a Stelo is not
    * offered when a G7 is being added, nor a G7 for a Stelo. */
   const int atype = sel_add_type();
   int listed      = 0;
   if (!known) {
      if (sensor_kind(atype) == KIND_BGM)
         listed = is_meter;
      else if (cgm_prefix(atype))
         listed = strncmp(name, cgm_prefix(atype), 4) == 0;
   }
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
       * srec_push's memmove. Reading a live count and holding a sensor_rec*
       * across the driver calls below is the exact hazard link_for_sensor and
       * src_for_link both resolve through a snapshot to close -- a torn read
       * here hands
       * dexble_pair a link resolved from a different sensor's identity, so one
       * sensor's address is bound to another's link and key file. */
      /* ONLY THE SLOTS THIS ADVERT IS FROM, which is what bounds the array:
       * an advert carries ONE address, and a slot is collected only when the
       * registry says that address is its own. */
      int n_ids = 0;
      int ids[LINK_MAX];
      for (int i = 0; i < vp->n_live; i++) {
         if (!vp->have_rec[i] || sensor_kind(vp->rec[i].type) != KIND_CGM)
            continue;
         if (strcmp(vp->rec[i].identity, mac) != 0)
            continue;
         if (n_ids >= (int)(sizeof ids / sizeof ids[0])) {
            /* Unreachable while one address belongs to one slot per kind --
             * so reaching it means that is no longer true, which is worth
             * saying rather than dropping the match. */
            LOGW("advert: more slots claim one address than there are links; "
                 "slot %d not reconnected",
                 vp->slot[i].id);
            break;
         }
         ids[n_ids++] = vp->slot[i].id;
      }
      for (int i = 0; i < n_ids; i++) {
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
         /* THE LINK AS THIS ADVERT FOUND IT: the bind and the dial below are
          * refused if a DISCONNECT forgets the link in between. */
         const unsigned gen = ls.forget_gen;
         /* READ HERE ONLY FOR A LINK WITHOUT AN ADDRESS, which needs it to
          * decide whether it can be dialled at all: this is the per-advert
          * path. A link with an address reads it just before it dials. */
         char own[12] = "";
         int have_own =
             !ls.mac[0] && dexble_code_load(link, mac, own, sizeof own);
         /* THE KEY IS THIS SENSOR'S only when no code file names another:
          * a link without an address is placed by rank, and the key on it
          * can be the one another sensor paired with. */
         int key_mine = 0;
         if (!ls.mac[0] && ls.paired) {
            char kown[24];
            key_mine = !dexble_code_owner(link, kown, (int)sizeof kown) ||
                       strcmp(kown, mac) == 0;
         }
         if (!ls.mac[0] && (key_mine || have_own)) {
            /* REGISTERED, WITH A KEY OR A CODE, AND NOTHING FOR THE LINK TO
             * DIAL.
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
             * the link holds either its J-PAKE key -- the reconnect then runs
             * the saved-key path -- or the code it was paired with. Bind the
             * address durably and go on to connect.
             *
             * A KEY OR THIS SENSOR'S OWN CODE IS THE CONDITION, not a
             * detail. A pairing cut short -- the app restarted before the
             * sensor next came on the air -- left no key, but its code was
             * saved with the link at the commit, so the J-PAKE can run again
             * with it. A link with neither would demand a fresh pairing with
             * a code nobody has, and is left to the ADD DEVICE flow below. */
            /* The registry, asked again: `vp` is the instant this advert
             * began, and a DISCONNECT on the main thread may have retired the
             * slot and cleared the link since. */
            if (!slot_still_live(ids[i]) ||
                driver_bind_mac_if(link, mac, gen) != BIND_PUBLISHED)
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
         /* THIS SENSOR'S OWN CODE when one was saved with its link: the
          * settings hold the code typed last, which may be another sensor's,
          * and a sensor whose key was lost must re-pair with its own. */
         /* THE CODE TYPED LAST ONLY FOR A LINK WITH NO CODE FILE AT ALL. A
          * file that names another sensor, or this one with its code refused
          * (an empty code), is not a gap to fill with a guess: each wrong try
          * spends one of the sensor's attempts. */
         if (!own[0] && !dexble_code_load(link, mac, own, sizeof own)) {
            char owner[24];
            if (dexble_code_owner(link, owner, (int)sizeof owner))
               break;
            str_snapshot(own, sizeof own, sp.code_str);
         }
         /* ASKED AGAIN JUST BEFORE THE DIAL, for the reason given at the
          * bind above. */
         if (!slot_still_live(ids[i]))
            break;
         dexble_pair_if(link, mac, own, gen);
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
      for (int i = 0; i < vp->n_live && mid < 0; i++) {
         if (vp->have_rec[i] && vp->rec[i].type == SENSOR_ONETOUCH &&
             !strcmp(vp->rec[i].identity, mac))
            mid = vp->slot[i].id;
      }
      /* PER-METER throttle: only rate-limit THIS meter, so one meter syncing
       * never blocks another (the global gate here made a second meter that
       * advertised alongside the first never get a turn). */
      /* MONOTONIC: this is "have we already answered this meter's adverts in
       * the last minute", an interval between two events in this process.
       * meter_note_advert() is the same event on the wall clock and is what the
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

   sensors_view_put(vp);
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
/* A COMMIT FAILURE THAT PERSISTS SAYS SO ONCE A MINUTE, NOT ONCE A SECOND.
 *
 * The arm deliberately survives a refused commit, and pairing_tick re-enters
 * commit_pair on every tick while a candidate is on the air -- so a failure
 * that is not going to clear (no free link, an unwritable registry) would
 * otherwise fill the log at 1 Hz for the life of the process, burying the one
 * line that explains it. The reason is the key: a DIFFERENT failure is news
 * and prints immediately. */
static void pair_fail_log(int reason)
{
   static int last_reason;
   static long last_at;
   long now = mono_s();
   if (reason == last_reason && last_at && now - last_at < 60)
      return;
   last_reason = reason;
   last_at     = now;
   switch (reason) {
      case 1: LOGI("refusing to pair: all %d links in use", LINK_MAX); break;
      case 2:
         LOGW("pairing commit: the provenance row could not be minted");
         break;
      case 3: LOGW("pairing commit: the sensor could not claim a slot"); break;
      case 4:
         LOGI("pairing commit: registered, but every link went while it was "
              "being written");
         break;
      default: break;
   }
}

/* THE CANDIDATE THE PICK SETTLES ON, from those heard in the last 60 s.
 * The decision is in scanlogic.c, which is pure and decidable on its own;
 * this only actuates it.
 *
 * Only candidates heard RECENTLY qualify. Entries are not aged out of the
 * list, so a sensor that left the room an hour ago still sits there with its
 * stale RSSI -- and a pending pairing evaluates this on every tick, possibly
 * long after the list was built. Comparing a stale RSSI against a fresh one
 * under the 20 dB rule picks wrong exactly when it matters.
 *
 * With `skip_declined` set the sensors the user said NO to are left out as
 * well, before any choice or count is made: a declined sensor loudest on the
 * air would otherwise win every pick and never be committed, so the wait
 * would neither pair the sensor beside it nor offer the list. */
static int pick_fresh(int skip_declined)
{
   int rssi[MAX_DEVS];
   int map[MAX_DEVS];
   int n    = 0;
   long now = mono_s();
   devlist_lock();
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++) {
      if (g_devs[i].seen_t > 0 && now - g_devs[i].seen_t > 60)
         continue;
      if (skip_declined && declined(g_devs[i].mac))
         continue;
      rssi[n] = g_devs[i].rssi;
      map[n]  = i;
      n++;
   }
   devlist_unlock();
   int p = scan_pick_candidate(rssi, n);
   return (p < 0) ? -1 : map[p];
}

/* How many candidates pick_fresh chooses among, by the same rules. */
static int count_fresh(int skip_declined)
{
   int n    = 0;
   long now = mono_s();
   devlist_lock();
   int nd = g_ndevs < MAX_DEVS ? g_ndevs : MAX_DEVS;
   for (int i = 0; i < nd; i++)
      if ((g_devs[i].seen_t == 0 || now - g_devs[i].seen_t <= 60) &&
          !(skip_declined && declined(g_devs[i].mac)))
         n++;
   devlist_unlock();
   return n;
}

/* pick_fresh over every fresh candidate, declined ones included. */
int select_candidate(void)
{
   return pick_fresh(0);
}

/* How many candidates are FRESH on the air right now (same 60 s window as
 * select_candidate). One is the unambiguous-by-existence case: with a single
 * sensor of the requested family in range, a confirmation list of one is
 * pure ceremony. */
int fresh_candidates(void)
{
   return count_fresh(0);
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
   /* WHEN THIS WAIT BEGAN, kept across a commit that fails and re-arms: the
    * tick retries every second, and the PENDING row counts from the first
    * arming, not from the latest refusal. */
   const long armed = g_pend_at > 0 ? g_pend_at : realtime_s();
   /* AND THE DEVICE IT REGISTERED, kept by every refusal that re-arms it. */
   const int pend_id = g_pend_id;
   undecline(mac); /* the user chose it after all */
   /* AND WHETHER IT HAS OFFERED THE CANDIDATE LIST: a refusal re-arms the
    * same wait, which has asked once already. */
   const int asked = g_amb_asked;
   /* A CGM IS THE TYPE BEING ADDED BY ITS ADVERTISED NAME: "DX01" for a
    * Stelo, "DXCM" for a G7. The candidate list is built for one add type on
    * another thread, so a device heard just as another flow ended can sit in
    * it; registered and dialled as this type it would be a device of the
    * wrong kind that the user never chose. The name is the candidate row's
    * own, or else the one its adverts recorded. Without the right name the
    * candidate is dropped and the wait goes on. */
   if (sensor_kind(sel_add_type()) == KIND_CGM &&
       !cgm_named(mac, sel_add_type())) {
      atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
      pairing_forget_candidate(mac);
      g_pend_pairing = sel_add_type();
      g_pend_at      = armed; /* the same wait: its id and list flag stand */
      LOGI("pairing: candidate has no Dexcom name -- still waiting");
      set_status("PAIRING PENDING");
      const int to_list = nav_has(SCR_KEYPAD) || cur_screen() == SCR_PAIRCONF;
      keypad_close();
      if (to_list)
         nav_go(SCR_DEVICES);
      return;
   }
   /* NOT A DISCONNECTED DEVICE THAT ONCE STREAMED, asked of the registry as
    * it is now. The candidate list drops such a device when it is retired,
    * but an advert already past its registry check can put it back; picked
    * here, the commit would revive the old slot and pair it with the new
    * code. So the candidate is dropped and the wait goes on. A retired slot
    * that never streamed is revived by the commit (see jni_on_advert). */
   if (sensor_kind(sel_add_type()) == KIND_CGM) {
      const struct sensor_view *rv = sensors_view_ref();
      const int ridx               = sensors_view_find_mac(rv, mac);
      const int retired = ridx >= rv->n_live && sensor_ever_streamed(mac);
      sensors_view_put(rv);
      if (retired) {
         /* Pairing mode ends here too: see the block below. */
         atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
         pairing_forget_candidate(mac);
         g_pend_pairing = sel_add_type();
         g_pend_at      = armed; /* the same wait: g_amb_asked stands */
         LOGI("pairing: candidate is a disconnected device -- still waiting");
         set_status("PAIRING PENDING");
         /* FROM THE CODE ENTRY OR A CONFIRMATION, END ON THE PENDING ROW,
          * as an armed wait does in kp_commit_pair: the confirmation's
          * proposal is used up, so it has nothing left to show. From the
          * 1 Hz tick, stay where the user is. */
         const int to_list =
             nav_has(SCR_KEYPAD) || cur_screen() == SCR_PAIRCONF;
         keypad_close();
         if (to_list)
            nav_go(SCR_DEVICES);
         return;
      }
   }
   /* PAIRING MODE ENDS HERE, on every path, the early returns below (meter
    * busy, mint failed, slots full, no free link) included. jni_on_advert
    * gates the whole advert-driven reconnect on !g_smart_pairing, and the
    * on_timer scan self-heal is gated on it too, so a flag left latched
    * would stop every paired CGM reconnecting with nothing on screen to say
    * so: the advert counter keeps climbing while the reading quietly stops
    * ageing forward. The user has committed to a device by the time we are
    * called; whether it works out does not change that. */
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
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
         set_status_refused("METER BUSY, RETRY");
         keypad_close();
         return;
      }
      int id = sensor_mint(sel_add_type(), mac, "", "", "", 0);
      if (id < 0) {
         set_status_refused("METER: REGISTER FAILED");
         keypad_close();
         return;
      }
      /* THE LINK FIRST, BEFORE THE SLOT. Registering the meter makes it
       * invisible to the CGM link ranking while its link is still unclaimed,
       * so an advertisement arriving during the registration, the bind and
       * the JNI scan-stop below can hand a CGM the link this meter is about
       * to want -- and the pairing then fails for want of one. Claiming here
       * closes that: driver_link_claim arms the link under the driver's own
       * lock, so nothing else can take it from this point on. */
      int mlink = meter_alloc_link(mac);
      if (mlink < 0) {
         set_status_refused("METER: NO FREE LINK");
         keypad_close();
         return;
      }
      /* A CLAIM THAT WAS NOT WRITTEN IS NOT A CLAIM: the table is full, or
       * slots.csv could not be replaced. Either way the device would be gone
       * at the next launch, so nothing below -- the bond, the key file, the
       * connect -- may happen. */
      if (sensor_claim_slot(id, sel_add_type(), mac) < 0) {
         meter_release_link(mlink); /* taken above, and now unused */
         set_status_refused(sensors_writable()
                                ? "METER NOT REGISTERED"
                                : "REGISTRY UNREADABLE: RESTART");
         keypad_close();
         return;
      }
      /* THE BIND CAN STILL BE REFUSED HERE. meter_busy() was tested above,
       * before the mint and the slot claim -- two flash writes -- and a meter
       * already on the air can start its own sync on a binder thread in that
       * window. Pairing into a live exchange resets the SAME otble statics the
       * refusal above exists to protect, so it stops here: the sensor IS
       * registered (nothing is undone) and its own screen is where the retry
       * lives. */
      if (!meter_bind(id, mac)) {
         meter_release_link(mlink); /* taken above, and now unused */
         LOGI("meter registered, but another meter began a sync mid-commit");
         set_status_refused("METER BUSY, RETRY");
         keypad_close();
         open_new_device(id);
         return;
      }
      /* Everything from here -- seeding this meter's record index, arming
       * its link, asking for the OS bond and issuing the connect -- is the
       * meter runtime's, not the shell's (see meter.h). The shell only
       * quiets the radio first: bonding while a scan is running is what made
       * the passkey prompt arrive minutes late. */
      if (shell_activity()) {
         scan_hold_until(mono_s() + 20); /* quiet radio to bond: an INTERVAL */
         stop_scan(shell_activity());
      }
      if (meter_pair(id, mac, mlink))
         set_status("METER: PAIRING");
      else
         set_status_refused("METER: NOT PAIRED");
      keypad_close();
      /* Even on a failed connect: the slot IS registered, and its own screen
       * is where the failure is stated and retried. Returning to the list
       * would hide the one row that explains what happened. */
      open_new_device(id);
      return;
   }

   /* A CGM COMMIT SUPERSEDES AN ARMED CGM WAIT; a meter's, above, leaves it
    * waiting. */
   g_pend_pairing = 0;
   g_amb_asked    = 0;
   g_pend_at      = 0;
   g_pend_id      = 0;
   /* A new sensor pairs on the first free CGM link, so pairing a second
    * sensor neither disturbs nor replaces one that is already streaming. A
    * sensor that already holds a live slot -- a code typed again, or the
    * tick's retry after a refusal -- has a link of its own, and that is the
    * one asked about. */
   int link = -1;
   int oid  = -1;
   {
      const struct sensor_view *ov = sensors_view_ref();
      const int oidx = sensors_view_find_mac_kind(ov, mac, KIND_CGM);
      oid = (oidx >= 0 && oidx < ov->n_live) ? ov->slot[oidx].id : -1;
      sensors_view_put(ov);
      if (oid >= 0)
         link = link_for_sensor(oid);
   }
   /* THE DEVICE A REFUSAL BELOW LEAVES WAITING: the one this wait already
    * registered, or this sensor's own slot when it holds one. */
   int wait_id = pend_id;
   if (wait_id <= 0)
      wait_id = (oid > 0) ? oid : 0;
   if (link < 0)
      link = link_for_new_sensor();
   if (link < 0) {
      /* THE ARM SURVIVES THE REFUSAL, and the message names the way out.
       *
       * Every CGM failure below does the same -- the METER paths do not, and
       * deliberately: a meter pairing carries no typed code to preserve, so
       * there is nothing an arm would be holding for the user. Dropping the
       * arm here would take the code the user typed with it, leaving a status
       * line the next one replaces as the only thing ever said about it. Left
       * armed, the wait continues and a link freed by a disconnect is picked
       * up by the tick with something to re-enter. */
      /* THE WAIT IS STAMPED HERE TOO. commit_pair cleared it on the way in,
       * and a restored arm with no instant on it shows a CGM PENDING screen
       * that cannot say how long it has been waiting -- which is the one
       * thing that screen exists to answer. */
      g_pend_pairing = sel_add_type();
      g_pend_at      = armed;
      g_pend_id      = wait_id;
      g_amb_asked    = asked;
      set_status_refused("NO FREE LINK: DISCONNECT ONE");
      pair_fail_log(1);
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
         /* SAID OUT LOUD. A commit that stops here leaves the user on the
          * device list with an unchanged screen and a status line that is
          * gone by the time they look, so the log is the only record that
          * the pairing they asked for went nowhere. */
         pair_fail_log(2);
         g_pend_pairing = sel_add_type();
         g_pend_at      = armed;
         g_pend_id      = wait_id;
         g_amb_asked    = asked;
         /* THE THREE REASONS A MINT FAILS ARE THREE DIFFERENT MESSAGES.
          * "STORAGE?" for the one that really is the disk; a registry file that
          * did not read is not a disk fault and is not fixed by retrying, and
          * the id space being spent is neither. A mint reads BOTH tables, so
          * either file can be the one to name. */
         const char *why = "REGISTER FAILED: STORAGE?";
         if (!sensors_slots_whole())
            why = "SLOTS.CSV UNREADABLE: RESTART";
         else if (!sensors_provenance_loaded())
            why = "SENSORS.CSV UNREADABLE: RESTART";
         set_status_refused(why);
         keypad_close();
         return;
      }
      /* See the meter path above: an unwritten claim must not reach the
       * radio, because driver_forget below erases this link's key file. */
      if (sensor_claim_slot(id, cgm_type, mac) < 0) {
         /* The slot table is full, or slots.csv could not be replaced. Same
          * reason as the mint above: nothing else records that this failed. */
         pair_fail_log(3);
         g_pend_pairing = sel_add_type();
         g_pend_at      = armed;
         g_pend_id      = wait_id;
         g_amb_asked    = asked;
         /* THE TWO REASONS A CLAIM IS REFUSED ARE NOT THE SAME MESSAGE. A
          * registry that did not load is also an EMPTY list, so naming a
          * device to disconnect points at rows the screen is not showing and
          * an action that cannot help.
          *
          * AND THE ACTION NAMED IS THE ONE THAT EXISTS. Nothing in this app
          * deletes a device -- DISCONNECT retires the slot and keeps it -- so
          * a full table is not freed by anything the user can tap. What
          * disconnecting does buy is a LINK, which is the limit they will
          * actually meet. */
         set_status_refused(sensors_writable()
                                ? "LIST FULL: DISCONNECT ONE"
                                : "REGISTRY UNREADABLE: RESTART");
         keypad_close();
         return;
      }
      newdev = id; /* the screen this flow ends on -- see open_new_device */
      /* The id and the type. The address it was registered against is in
       * slots.csv, which is private app storage; logcat is not. */
      LOGI("registered sensor id=%d type=%s at pairing commit", id,
           sensor_type_name(cgm_type));
   }
   /* THE LINK IS RESOLVED AGAIN, AFTER THE REGISTRATION, and this answer is
    * the one the radio work below uses.
    *
    * The first answer was taken before the mint, because a refusal there must
    * not register anything. But the free-link pool is shared with the METER
    * runtime, and meter_alloc_link leaves free only as many links as there are
    * live CGMs still waiting for one -- counted from the published view. A
    * meter advert arriving on a binder thread while the two flash writes above
    * are in progress counts a table this sensor is not yet in, so it may arm
    * the last free link: the very one resolved above. Connecting a CGM on a
    * link a meter has armed seizes that meter's GATT client.
    *
    * Asked after the registration, the count includes this sensor, so a
    * concurrent meter claim leaves its link alone -- and the rank the ranking
    * gives it is the rank it will keep. */
   {
      int relink = link_for_sensor(newdev);
      if (relink < 0) {
         /* Every free link went while this sensor was being written. It IS
          * registered, so the pairing continues from its own screen as soon as
          * a link frees -- sensor_reconcile connects a registered sensor
          * whenever one does. Nothing here is undone. */
         pair_fail_log(4);
         /* THE ARM SURVIVES, like the refusal before the mint: the J-PAKE code
          * lives only in this flow, so a registered sensor that never got a
          * link cannot be connected by sensor_reconcile -- it needs
          * pairing_tick to re-enter this commit once a link frees. The mint
          * recognises the address it already holds, so the retry lands on the
          * same id rather than a second one. */
         g_pend_pairing = sel_add_type();
         g_pend_at      = armed;
         g_pend_id      = newdev;
         g_amb_asked    = asked;
         set_status_refused("NO FREE LINK: DISCONNECT ONE");
         keypad_close();
         open_new_device(newdev);
         return;
      }
      link = relink;
   }
   /* The LINK is named, so there is no selection to race: this erases the key,
    * MAC and code files of the link being paired and no other. */
   driver_forget(link);
   linkinfo_forget_rssi(link); /* the signal of whatever held the link last */
   /* THIS COMMIT IS THE LINK'S DIAL, and it claims the advert path's window
    * as one would: an advert of the same sensor in the next 30 s would
    * otherwise start the handshake over while it is running. */
   atomic_store_explicit(&g_link_try[link], mono_s(), memory_order_release);
   atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
   if (shell_activity()) {
      scan_hold_until(mono_s() + 20); /* quiet radio for the J-PAKE */
      stop_scan(shell_activity());
   }
   set_status("PAIRING");
   /* THIS SENSOR'S CODE, kept with its link: the settings hold the code typed
    * last, which stops being this sensor's the moment another is added.
    * Saved BEFORE the dial, so a refusal of the code (which empties the file)
    * always comes after the save. */
   (void)dexble_code_save(link, mac, sp.code_str);
   /* THE J-PAKE AND THE CERTIFICATE EXCHANGE RUN FIRST; the OS pairing
    * dialog the user answers is the sensor's own request, raised after
    * them. */
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
         set_status_refused("CODE NOT SAVED");
         return 0;
      }
      /* A NEW CODE IS A NEW WAIT: a pairing still armed from before counts
       * from this moment, and so does a commit refused below. It is for the
       * device type being added, and has not offered the candidate list
       * yet. */
      g_pend_at = g_pend_pairing ? realtime_s() : 0;
      undecline(0); /* a new code may be for any sensor on the air */
      if (g_pend_pairing) {
         /* A WAIT FOR ANOTHER TYPE does not inherit the device the earlier
          * one registered: that device is a sensor of the other type. */
         if (g_pend_pairing != sel_add_type())
            g_pend_id = 0;
         g_pend_pairing = sel_add_type();
         g_amb_asked    = 0;
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
      } else if (fresh_candidates() >= 2) {
         /* SEVERAL ON THE AIR AND NONE CLEAR OF THE REST. The app has a code
          * and no way to tell which sensor it belongs to: every candidate
          * answers to the same family, and proximity -- the one signal there
          * is -- has already declined to separate them (scan_pick_candidate).
          *
          * So the user is asked. Guessing here pairs the wrong sensor, and
          * that is not a recoverable mistake: commit_pair drops the bond and
          * key of whatever it aims at. The list is ordered by live signal and
          * a row PROPOSES rather than commits, so the confirmation still
          * names the device before anything is touched.
          *
          * The pairing is armed as well, so backing out of the list without
          * choosing leaves the wait running rather than discarding the code
          * the user just typed. */
         g_pend_pairing = sel_add_type();
         /* ASKED, AND RECORDED AS ASKED. Cleared here, the tick's own
          * ambiguity check offers the list again on the very next second --
          * the repeating screen this flag exists to prevent. */
         g_amb_asked = 1;
         g_pend_at   = realtime_s();
         atomic_store_explicit(&g_smart_pairing, 0, memory_order_release);
         set_status("PICK THE SENSOR");
         LOGI("pairing: %d candidates and none clear -- asking the user",
              fresh_candidates());
         keypad_close();
         nav_go(SCR_DEVLIST);
      } else {
         /* No candidate on the air yet: ARM the pairing and free the
          * user. Parking them in the device list until the sensor
          * advertised also kept g_smart_pairing latched, which suppresses
          * every OTHER sensor's reconnect -- waiting for the new sensor
          * cost the readings of the ones already worn. The 1 Hz tick
          * commits the moment an unambiguous candidate appears; DEVICES
          * shows the armed state as a PENDING row. */
         g_pend_pairing = sel_add_type();
         g_amb_asked    = 0;
         g_pend_at      = realtime_s();
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

long pairing_pend_since(void)
{
   return g_pend_at;
}

int pairing_adv_name(const char *mac, char *out, int cap)
{
   if (out && cap > 0)
      out[0] = 0;
   if (!mac || !mac[0] || !out || cap <= 0)
      return 0;
   int got = 0;
   devlist_lock();
   for (int a = 0; a < g_nadvname; a++)
      if (strcmp(g_advname[a].mac, mac) == 0) {
         str_snapshot(out, cap, g_advname[a].name);
         got = out[0] != 0;
         break;
      }
   devlist_unlock();
   return got;
}

int pairing_candidates_waiting(void)
{
   return g_pend_pairing ? count_fresh(1) : 0;
}

int pairing_smart(void)
{
   return atomic_load_explicit(&g_smart_pairing, memory_order_acquire);
}

void pairing_arm(int type)
{
   g_pend_pairing = type;
   g_amb_asked    = 0;
   g_pend_at      = type ? realtime_s() : 0; /* 0: nothing is waiting */
   g_pend_id      = 0;
}

int pairing_pending_device(void)
{
   return g_pend_pairing ? g_pend_id : 0;
}

const char *pairing_pend_mac(void)
{
   return g_pend_mac;
}

const char *pairing_pend_name(void)
{
   return g_pend_name;
}

void pairing_decline(const char *mac)
{
   if (!mac || !mac[0] || declined(mac))
      return;
   int at = g_ndeclined;
   if (at < MAX_DEVS) {
      g_ndeclined++;
   } else {
      at            = g_declined_at;
      g_declined_at = (g_declined_at + 1) % MAX_DEVS;
   }
   str_snapshot(g_declined[at], sizeof g_declined[at], mac);
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

void pairing_forget_candidate(const char *mac)
{
   if (!mac || !mac[0])
      return;
   devlist_lock();
   int k = 0;
   for (int i = 0; i < g_ndevs; i++)
      if (strcmp(g_devs[i].mac, mac) != 0)
         g_devs[k++] = g_devs[i];
   g_ndevs = k;
   devlist_unlock();
}

/* A pick PROPOSES; only the confirmation screen's YES commits. The list is
 * ordered by live RSSI, so rows reorder under the finger -- pairing on the
 * raw tap would let one mis-press register the wrong device and drop a
 * bond. The (mac, name) pair is copied under the list lock, against the
 * binder-thread writer. Returns 0 when the row no longer exists. */
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
    * on the air (fresh adverts only -- pick_fresh's 60 s window). Main
    * thread only, like every other commit_pair caller. keypad_close() runs
    * inside commit_pair, so aim it at the CURRENT menu first -- a background
    * commit must never yank the user out of whatever screen they are on.
    * Nor is it made while the user is answering KEEP WAITING or STOP (the
    * answer comes first), typing into the text editor or the food picker,
    * or reading the meter help: those flows hold the keypad's entry, its
    * return target and the add type, which a commit would overwrite. */
   if (g_pend_pairing && sensor_kind(g_pend_pairing) == KIND_CGM &&
       cur_screen() != SCR_KEYPAD && cur_screen() != SCR_DEVLIST &&
       cur_screen() != SCR_PAIRCONF && cur_screen() != SCR_PENDCANCEL &&
       cur_screen() != SCR_LABEL && cur_screen() != SCR_FOODTYPE &&
       cur_screen() != SCR_METERHELP) {
      /* THE CANDIDATE LIST IS FOR THE ADD TYPE IT WAS BUILT FOR: a flow
       * left behind for another type (a meter, or the other CGM) fills it
       * with that type's devices. The wait's own type is restored and the
       * list starts again from its adverts. */
      if (sel_add_type() != g_pend_pairing) {
         sel_set_add_type(g_pend_pairing);
         pairing_forget_candidates();
         return;
      }
      int pidx = pick_fresh(1);
      /* AMBIGUOUS, NOT ABSENT. An armed pairing that keeps waiting while two
       * sensors answer is waiting for something that will not happen: the
       * tie is not going to break itself, and the user is the only one who
       * knows which sensor they just applied. Shown ONCE per arm, so the
       * list is offered rather than forced back over whatever they do next. */
      if (pidx < 0 && count_fresh(1) >= 2 && !g_amb_asked) {
         g_amb_asked = 1;
         set_status("PICK THE SENSOR");
         LOGI("pending pairing: %d candidates and none clear -- asking",
              count_fresh(1));
         /* THE PICK COMMITS AS THIS WAIT, and ends where the user was: the
          * add type and the keypad's return target are whatever the user
          * last did elsewhere, so both are set from the wait and from here. */
         sel_set_add_type(g_pend_pairing);
         forms_kp_return_set(cur_screen());
         menu_pair_mark_here(); /* a cancel of the list returns here too */
         nav_go(SCR_DEVLIST);
      }
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
