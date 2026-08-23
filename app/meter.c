// SPDX-License-Identifier: GPL-3.0
// meter.c --- The OneTouch meter runtime (see meter.h)
// Copyright 2026 Jakob Kastelic

#include "meter.h"
#include "alarmlogic.h" /* CHIRP_MAX_GAP_S: the store gap this reports */
#include "blejni.h"
#include "bletrans.h"
#include "clock.h"
#include "devtag.h" /* a log may not carry an address; see there */
#include "dexdriver.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"
#include "meterlogic.h"
#include "metersess.h"
#include "meterstore.h"
#include "otble.h"
#include "sensors.h"
#include "shell.h"
#include "status.h"
#include "store.h"
#include "thread.h"
#include "tzoff.h"
#include "util.h"
#include <jni.h>
#include <stdint.h>
#include <string.h>

/* How long after a finished exchange the tick refuses to re-arm that meter.
 *
 * This is only a second line of defence: ot_drv_disconnect keeping the link
 * armed until the real disconnect is what actually stops the re-sync loop,
 * and this exists so a link that FLAPS -- drops and is immediately reachable
 * again -- cannot walk back into a sync per second.
 *
 * It was 60 s, which was too blunt. The link stays armed for the ~35 s a
 * Verio remains awake, so a 60 s gate kept biting for ~25 s AFTER the
 * teardown -- a window with no standing connect at all, during which a
 * second fingerstick was caught by nothing (the advert path carries its own
 * pre-existing 60 s per-meter throttle). 15 s is still ten times the 1.5 s
 * period of the loop this guards against, and being shorter than the awake
 * time means it has always expired by the time the link is actually free,
 * so it never delays a legitimate re-arm. */
#define METER_REARM_COOLDOWN 15

/* How long a link may sit armed waiting for a disconnect callback before the
 * watchdog assumes it was lost. See the recovery in meter_sync_watchdog. */
#define METER_TEARDOWN_MAX 180

/* Forward declaration for this file's own helper: the meter's timezone rule
 * is used by the driver hooks above its definition. */
static long meter_zone(void *ctx, long t);

/* THE SESSION -- which meter owns the one protocol exchange, on which link,
 * since when -- lives in metersess.c behind its own lock. It was five
 * file-scope variables here, written from the binder callbacks and read by
 * the watchdog on two other threads, with a test and a set either side of a
 * window that two meters waking together really do land in. See metersess.h.
 *
 * The lock is not reachable from this file, which is the point: every driver
 * and transport call below is therefore OUTSIDE it by construction, and the
 * documented driver_lk -> msess_lk order cannot be inverted by an edit. */
_Static_assert(MSESS_LINKS_MAX == LINK_MAX,
               "the session speaks about the same links the transport has");

/* The device row's phase text is a COPY of the runtime record's, so the two
 * buffers are one fact. str_snapshot makes a mismatch a silent truncation
 * rather than an overflow, which is exactly why it needs saying here. */
_Static_assert(sizeof(((struct meter_ui *)0)->stat) ==
                   sizeof(((struct meter_rt *)0)->stat),
               "the row copies the record's phase text whole");

/* This module's own reading of the session, for the many places that need
 * only the link the exchange is running on. */
static int sess_link(void)
{
   struct msess s;
   msess_get(&s);
   return s.link;
}

/* Which links carry a meter, mirroring what the transport was told. The
 * shell needs its own copy because several CGM-only passes below iterate the
 * links and must skip meters. A per-link FACT rather than a fixed link
 * number: any link can carry a meter. Written only through link_set_meter, so
 * the two copies cannot drift. */
/* (The per-link ROLE and ARMING tables moved into the driver: its own
 * callbacks read them, so they belong with its lock -- see dexdriver.h's
 * driver_link_* operations. What is left here is this module's own
 * per-link timing, which nothing on a binder thread reads.) */

/* The link a meter's standing connect is outstanding on, or -1.
 *
 * ITS OWN TABLE, not the driver session. The session's address is stamped by
 * the DEXCOM handshake, and a meter never runs one -- so reading it back
 * reported every meter as unarmed, the tick re-armed on every pass, and each
 * pass issued a fresh connectGatt: a connect per second, forever, which is
 * both a battery burn and a live risk of cancelling the connection during
 * the one second the meter is actually awake. Measured on the device before
 * this table existed: the same meter armed four times in four seconds. */

/* THE TEARDOWN-WAIT TABLE moved into the session with the rest of it
 * (msess_idle_set / msess_idle_copy); the reason it exists is unchanged:
 *
 * a link is left ARMED with no exchange running when ot_drv_disconnect asks
 * for the close and hands the link back to the transport to tear down.
 *
 * That happens because ot_drv_disconnect deliberately does NOT un-arm (that
 * un-arm was what let the 1 Hz tick reconnect a still-awake meter and re-run
 * the whole exchange 29 times in 29 seconds). Waiting for the real GATT
 * disconnect is right, but it removed the only thing that guaranteed the
 * link ever came back: meter_sync_watchdog fires only while the session is
 * set, and this state has it clear, while pancra_link_watchdog skips meter
 * links outright. A lost disconnect callback would therefore strand the link
 * armed forever and that meter would never sync again until a restart. */

/* Give a link back: un-armed, and not a METER link any more.
 *
 * CLEARING THE METER BIT IS HALF THE JOB. Set and never cleared, a link stays
 * marked "meter" for the life of the process -- and since the link pool is
 * shared, a CGM that later lands
 * on it would have had its notifications routed into the OneTouch parser.
 * Releasing is the other half of arming and has to undo both facts. */

/* (The RSSI of the sync connection is recorded PER METER, in the runtime
 * table keyed by registry id -- meter_rssi_of, and only there. A second
 * process-global copy would be three more fields written from a binder thread
 * for no reader.) */
static char g_meter_model[24], g_meter_fw[24];
/* AND THE LOCK THEY BELONG TO. These were CLEARED under the registry lock and
 * written and read under nothing at all -- a lock borrowed from another module
 * for two of the four accesses, which protects nothing. meter_set_dis runs on
 * a binder thread (pancra_devinfo) while the sync completion reads them on the
 * main thread and mints from what it reads, into a provenance row that is
 * never rewritten. A leaf: nothing else is taken while it is held. */
static struct mutex mdis_lk = MUTEX_INIT;

/* THE WALK'S OWN ORDER, which is the only evidence there is about which of a
 * repeated hour's two instants a fingerstick belongs to (meterlogic.h). Reset
 * where the walk position is -- beside ot_init in meter_sync_start -- and
 * carried by the binder thread that delivers the records, exactly like
 * otble.c's own walk state and under the same driver lock. One meter syncs at
 * a time (msess_claim), so one of these is enough. */
static struct meter_seq g_mseq;

/* Un-arm: this meter has no connection outstanding, so the tick may arm it
 * again. The link keeps its METER ROUTING BIT -- see below. */
void meter_unarm_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   msess_idle_set(link, 0);
   driver_link_arm(link, "");
}

/* Give the link back completely: un-armed AND unrouted from otble.
 *
 * Only correct once the GATT disconnect has actually ARRIVED. Clearing the
 * routing bit at the moment we ASK for a close is too early: the callback is
 * still in flight, and with the bit gone it lands in the CGM branch instead,
 * running the Dexcom disconnect logic on a link that has no Dexcom session --
 * which posts "CONNECTION ERROR" after every successful meter sync. */
void meter_release_link(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   msess_idle_set(link, 0); /* no longer waiting on a teardown */
   driver_link_arm(link, "");
   meter_link_set(link, 0);
}

/* The excursion verdict across EVERY registered CGM, each judged on its OWN
 * newest sample with the standard freshness gate, merged worst-first (a LOW
 * anywhere outranks anything -- alarm_zone_merge). The DISPLAY belongs to
 * the primary; the ALARM watches every sensor the user wears, so a low on
 * the non-primary sensor rings too. Stranded is merged the same way, so an
 * out-of-range sensor going silent sustains the alarm whichever one it is.
 * With no CGM registered at all (a pre-registry install) the
 * current reading -- src-0 legacy data -- is judged instead.
 *
 * Gathering takes the registry lock, then hist_lock, SEQUENTIALLY -- and
 * must complete before alarm_lock is taken: hist is non-recursive and is
 * the same flag as g_draw_busy, so nesting it inside alarm is the one edge
 * that could still close a lock cycle. */

static int meter_link_of(const char *mac)
{
   return driver_link_of_mac(mac);
}

int meter_armed(const char *mac)
{
   return meter_link_of(mac) >= 0;
}

/* A free link a meter may take, or -1.
 *
 * Shared by the arming path and by pairing a NEW meter: two callers, one
 * rule. NOT link_for_slot, whose free-link search knows nothing about armed
 * meters and will happily hand back LINK_CGM. */
int meter_alloc_link(const char *mac)
{
   /* CGMS COME FIRST. A CGM only claims its link when it next advertises,
    * while the tick arms every meter within seconds of launch -- so with more
    * devices than links the meters would take them all and a sensor that
    * streams continuously would be left with none. Count the live CGMs still
    * waiting for a link and ask the driver to leave that many free. */
   int want = 0;
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n; i++) {
      if (v.slot[i].old)
         continue;
      if (!v.have_rec[i] || sensor_kind(v.rec[i].type) != KIND_CGM)
         continue;
      if (driver_link_of_identity(v.rec[i].identity) < 0)
         want++;
   }
   /* THE SEARCH AND THE CLAIM ARE THE DRIVER'S, in one critical section: a
    * link that reads free here and is claimed by a binder thread before this
    * caller uses it would be handed to two devices at once. */
   return driver_link_claim(mac, want);
}

void meter_sync_start(int mid, const char *mac)
{
   char dt[DEVTAG_LEN];
   /* Its OWN link, from the shared pool. Every registered meter holds one, so
    * all of them can carry a standing connect at once -- with a single
    * reserved link only the last-used meter could, and the others were back
    * to catching a two-second advertisement. */
   /* Already armed? Keep the SAME link. Re-allocating would strand the
    * pending connect on the first one and hand this meter a second. */
   int link = meter_link_of(mac);
   if (link < 0) {
      /* A free link: claimed by no other meter, and carrying no CGM session.
       * NOT link_for_slot -- that ranks devices the DEXCOM session binds, and
       * a meter never runs one, so two meters would rank to the same link and
       * the second would evict the first. */
      link = meter_alloc_link(mac);
   }
   if (link < 0 || link >= LINK_MAX) {
      /* Every link is spoken for. Refuse rather than connect on someone
       * else's: routing a meter onto a CGM's link would feed sensor
       * notifications to the meter parser.
       *
       * Throttled. The tick retries every second, so an unthrottled report
       * here wrote a log line and overwrote the status banner once a second
       * for as long as the condition lasted -- burying whatever the status
       * line was actually there to say. */
      static long last_warn;
      long now = mono_s(); /* a throttle, not an instant */
      if (now - last_warn > 60) {
         last_warn = now;
         LOGI("meter id %d (dev %s): no free link", mid, devtag(mac, dt));
         set_status("NO FREE LINK");
      }
      return;
   }
   meter_link_set(link, 1);
   /* ARMED FIRST, THEN CONNECT -- the order matters.
    *
    * meter_hook_connected identifies the meter from this table, and
    * jni_connected fires on a BINDER thread. A meter switched on right next
    * to the phone connects in milliseconds, so issuing the connect first
    * left a window where the callback read an empty entry, failed to
    * identify the meter, and closed the link -- losing exactly the sync the
    * user was standing there waiting for.
    *
    * If the connect never reaches Java (no JNIEnv, Bluetooth off) the link
    * is released again, so a silent failure cannot leave the meter marked
    * armed forever with nothing behind it. */
   driver_link_arm(link, mac);
   if (!dexble_meter_connect(link, mac)) {
      LOGI("meter id %d (dev %s): connect did not reach the transport", mid,
           devtag(mac, dt));
      meter_release_link(link);
      return;
   }
   /* DO NOT STAMP LAST SYNC HERE.
    *
    * Stamping rt->sync_t is truthful on the ADVERT path -- an advertisement
    * means the meter really is switched on and in range. Arming a STANDING
    * connect means nothing of the kind: it is issued for every registered
    * meter on a timer, whether the meter is off, in another room, or a mile
    * away. Stamping here would make
    * all three read "SYNCED a few seconds ago" at once, which is a plain lie
    * about whether a fingerstick has been captured -- exactly the fact the
    * user is looking at that row to learn. The stamp belongs where contact
    * is PROVEN: the advert path (seen on air) and ot_drv_status (it
    * answered).
    */
   /* ARM ONLY. Deliberately NOT ot_init or a session claim here.
    *
    * The connect below may sit pending for hours -- that is the point -- and
    * with every meter holding one, seeding the shared otble state at arm
    * time would let arming meter B reset the protocol out from under a sync
    * already running on meter A: phase to idle mid-walk, last_index
    * replaced, and A's remaining fingersticks written to readings.csv under
    * B's id, in an append-only file that is never rewritten. The state is
    * seeded when a meter actually ANSWERS instead -- meter_hook_connected
    * -- which is the only moment exactly one meter owns it. */
   LOGI("meter id %d (dev %s) armed on link %d", mid, devtag(mac, dt), link);
}

/* A meter link dropped. 1 if it owned the current exchange, 0 if it was an
 * idle standing connect. Either way the link is released so the next tick
 * re-arms it -- a meter whose connection died must not stay marked armed, or
 * it never reconnects. */
static int meter_hook_disconnected(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   /* THE OWNERSHIP TEST AND THE CLEAR ARE ONE STEP. As two, a disconnect for
    * an OLD link could read "not the owner", be overtaken by a claim, and
    * then clear a session that had just started on another link. */
   int active = msess_end(link, 0);
   meter_release_link(link);
   return active;
}

/* A meter answered on `link`. Returns 1 to let the protocol run, 0 to
 * refuse.
 *
 * THIS is where the sync becomes real, so this is where the shared otble
 * state is seeded and the busy latch taken. Called from the transport's
 * connect callback, under driver_lock. */
static int meter_hook_connected(int link)
{
   char dt[DEVTAG_LEN];
   if (link < 0 || link >= LINK_MAX)
      return 0;
   /* Which meter is this? FROM THE ARMED TABLE -- the address we issued the
    * connect with.
    *
    * NOT from the driver session: drv_connect does not write it, only the
    * Dexcom handshake does, so for a meter link it is always empty. Reading
    * it here made the lookup fail every single time, which refused and
    * closed every meter connection that ever arrived -- meters could not
    * sync at all. The armed table is the only record of which meter owns a
    * link. */
   char mac[24];
   driver_link_armed_mac(link, mac, sizeof mac);
   int mid = -1;
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n && mid < 0; i++)
      if (mac[0] && v.have_rec[i] && !strcmp(v.rec[i].identity, mac) &&
          sensor_kind(v.rec[i].type) == KIND_BGM)
         mid = v.slot[i].id;
   if (mid <= 0) {
      LOGI("meter connect on link %d: no registered meter there", link);
      return 0;
   }
   /* CLAIM IT -- the busy test and the seizure in one step, because two
    * meters switched on together connect milliseconds apart on separate
    * binder threads. A refusal means another meter is mid-exchange and there
    * is only one protocol state: its standing connect is re-armed by the
    * tick, and the meter buffers its records, so nothing is lost -- only
    * deferred. */
   if (!msess_claim(link, mid, mac, mono_s() /* an INTERVAL: see util.h */)) {
      struct msess s;
      msess_get(&s);
      LOGI("meter on link %d deferred: link %d is mid-sync", link, s.link);
      return 0;
   }
   /* Seed THIS meter's own stored index. The index is per-device: sharing
    * one made each sync read the other's counter as "gone backwards", so
    * they reset each other forever and one meter's records were never
    * reached. */
   /* SEED FROM THE STORED INDEX, AND KNOW WHEN THERE IS NONE.
    * ABSENT is a first sync and -1 is exactly right; ERROR is a file that
    * exists and did not answer, and walking from the beginning then re-reads
    * a window of records already imported, holding the meter awake for a
    * sync that will import nothing. Decline instead: the next advert tries
    * again, and the file usually reads. */
   int seed             = -1;
   enum load_result how = meter_index_load(mid, &seed);
   if (how == LOAD_ERROR) {
      LOGW("meter %d: the stored index could not be read; not syncing this "
           "cycle rather than re-walking its records",
           mid);
      return 0;
   }
   ot_init(seed); /* caller holds driver_lock */
   /* The walk's timestamp evidence starts empty with the walk. Carrying the
    * previous meter's last instant into this one would let it decide a
    * repeated-hour record it has nothing to do with. The ambiguity count is
    * cleared with it so it always describes the import being looked at. */
   meter_seq_reset(&g_mseq);
   meter_rt_amb_clear(mid);
   /* Clear the DIS strings: they are process-global for a meter link, and a
    * sync that finishes before the reads land -- the common case, since
    * "nothing new" ends after one round trip -- would otherwise mint this
    * meter against the PREVIOUS meter's model and firmware. */
   mutex_lock(&mdis_lk);
   g_meter_model[0] = 0;
   g_meter_fw[0]    = 0;
   mutex_unlock(&mdis_lk);
   LOGI("meter id %d (dev %s) answered on link %d -> sync in flight", mid,
        devtag(mac, dt), link);
   return 1;
}

/* Runs on the 1 Hz tick (and the service heartbeat, so it survives the
 * activity being destroyed). Two jobs:
 *
 *   - release a sync that has WEDGED. The session is claimed only when the
 * meter answers, so this 90 s measures a real exchange rather than a
 *     standing connect's wait, and it cannot tear down a pending connect
 * that is behaving exactly as intended.
 *   - keep exactly one standing connect ARMED. This is what makes a sync
 *     survive a restart, a Bluetooth toggle, or the app being swiped away:
 *     nothing else re-establishes it, and without it the first fingerstick
 *     after any of those would be missed with no way for the user to know.
 */
static void meter_sync_watchdog_locked(void)
{
   /* THE DECISIONS are meterlogic.c's, where a test can reach them; this
    * function does the part only the shell can -- closing GATT links and
    * clearing the runtime's state. */
   struct meter_tick mt;
   /* ONE READING of the session and one of the wait table, both taken before
    * anything is decided: the exchange this tick is judging must not be the
    * one that started between the busy flag and the start stamp. */
   struct msess s;
   msess_get(&s);
   long idle[MSESS_LINKS_MAX];
   msess_idle_copy(idle, MSESS_LINKS_MAX);
   /* MONOTONIC. These are elapsed-time decisions -- has this exchange run
    * too long, has this link waited too long for its teardown -- and a
    * wall-clock correction would fire or postpone both. */
   long now_mono = mono_s();
   /* THE PROTOCOL'S OWN DEADLINE FIRST. The watchdog below judges
    * the SESSION -- has this exchange run too long overall -- and it is
    * deliberately generous. A single request whose answer never arrives is a
    * different failure and a much shorter one, and only the protocol knows a
    * request is outstanding. Both are wanted: this ends a wedged exchange
    * while the meter is still awake to be asked again, and the watchdog is
    * still the backstop for everything it cannot see. */
   ot_tick(now_mono);
   /* THE WRITE THAT DID NOT LAND, TRIED AGAIN. Non-blocking and
    * throttled to once per 30 s, and a no-op the rest of the time -- this
    * tick runs on the MAIN thread, where meterstore.c's rule is that nobody
    * waits on the save's lock (see the note above msync_lk). A retry that
    * finds a real writer holding the file leaves it to them. */
   if (meter_sync_retry(now_mono) == SYNC_STILL_DIRTY)
      LOGW("meter last-sync file still refusing writes");
   meter_tick_eval(s.busy, s.start, idle, LINK_MAX, now_mono, &mt);
   if (mt.drop_sync) {
      /* Drop FIRST, and take the link back from the same step that cleared
       * it: reading it again afterwards would read -1. */
      int dead = msess_drop();
      LOGI("meter sync timed out; releasing link %d", dead);
      if (dead >= 0)
         dexble_link_close(dead);
      /* Un-arm only, for the same reason as ot_drv_disconnect: the close is
       * in flight and its callback still has to route to otble. */
      meter_unarm_link(dead);
   }
   /* Release anything stranded waiting for a teardown callback that never
    * came -- see meterlogic.h for why that happens and why the bound is as
    * generous as it is. */
   for (int l = 0; mt.nrelease && l < LINK_MAX; l++) {
      if (!mt.release[l])
         continue;
      LOGI("meter link %d stranded waiting for a disconnect; releasing", l);
      meter_release_link(l); /* clears its teardown-wait stamp too */
   }
   /* ARM EVERY REGISTERED METER, not just one.
    *
    * Each holds its own link, so all of them can wait on the controller at
    * once -- which is what makes "whichever meter I pick up" work rather
    * than only the last one used. Arming is idempotent (meter_armed), so
    * this is a no-op on every tick but the first after a restart or a
    * finished sync. */
   int ids[MAX_SLOTS];
   char macs[MAX_SLOTS][24];
   int n = 0;
   struct sensor_view av;
   sensors_view_get(&av);
   for (int i = 0; i < av.n && n < MAX_SLOTS; i++) {
      if (av.slot[i].old)
         continue; /* retired: holds no link */
      if (!av.have_rec[i] || sensor_kind(av.rec[i].type) != KIND_BGM)
         continue;
      ids[n] = av.slot[i].id;
      str_snapshot(macs[n], sizeof macs[n], av.rec[i].identity);
      n++;
   }
   long now =
       mono_s(); /* a COOLDOWN clock; the block above can take a moment */
   for (int i = 0; i < n; i++) {
      if (!macs[i][0] || meter_armed(macs[i]))
         continue;
      /* COOLDOWN after a finished exchange.
       *
       * Second line of defence for the re-sync loop fixed in
       * ot_drv_disconnect: that one keeps the link armed until the meter
       * really goes away, which is enough on its own, but a link that flaps
       * -- drops and is immediately reachable again -- would otherwise walk
       * straight back into a sync-per-second. A meter that has genuinely
       * powered off does not come back within seconds, so nothing legitimate
       * waits on this. */
      struct meter_rt rt;
      if (meter_rt_read(ids[i], &rt) && rt.synced_t &&
          now - rt.synced_t < METER_REARM_COOLDOWN)
         continue;
      /* One per tick: meter_sync_start takes both locks and issues a GATT
       * connect, and doing several in one pass would hold the main thread
       * across a burst of binder calls for no gain -- the meters are almost
       * certainly all switched off anyway. */
      meter_sync_start(ids[i], macs[i]);
      return;
   }
}

/* SELF-GUARDED. Two threads run this: the activity's 1 Hz timer reaches it
 * through sensor_reconcile (which holds g_reconcile_busy) and the service
 * tick calls it DIRECTLY, outside that guard -- so the serialisation the
 * reconcile path relies on did not actually cover both callers. Interleaving
 * could arm one meter on two links or bump a connect generation mid-connect;
 * the refusal path in meter_hook_connected cleans up after it, so the cost
 * was transient battery and latency rather than lost data, but a function
 * whose safety depends on which caller you came from is one edit away from
 * being wrong. Guarding here covers every caller, present and future.
 * Skipping a tick is free -- the next one is a second away. */
void meter_sync_watchdog(void)
{
   static struct flight busy = FLIGHT_INIT;
   if (!flight_enter(&busy))
      return;
   meter_sync_watchdog_locked();
   flight_leave(&busy);
}

/* Meter link RSSI, read once per sync connection (the meter has no
 * continuous link). Stored separately from the CGM RSSI so the meter's
 * SIGNAL row shows its own last-sync strength. */
void pancra_meter_rssi(int rssi)
{
   /* Record against THIS meter so its SIGNAL row shows its own last value,
    * and refresh its sync time -- RSSI is read on connect, i.e. an actual
    * sync. */
   int src = msess_src();
   if (src > 0) {
      long now = realtime_s();
      meter_rt_rssi(src, rssi, now, now);
      /* TWO DIFFERENT ANSWERS, AND BOTH ARE READ. The line above
       * says the live observation was accepted; this one says it is on disk.
       * Dropped, the refusal would make a meter whose last-sync time could
       * not be written look identical to one whose could -- until the next
       * launch, where it has never been seen at all. Saying so is all this
       * caller can do; the table stays dirty and the tick's meter_sync_retry
       * keeps trying. */
      if (meter_sync_save() != 0)
         LOGW("meter %d: last-sync time NOT SAVED -- will retry", src);
   }
   LOGI("meter rssi %d dbm", rssi);
   shell_repaint();
}

/* The transport's drv_write/drv_subscribe are already UUID-generic, so the
 * meter needs no transport of its own -- only its own protocol. */
void ot_drv_write(const uint8_t *data, int n)
{
   int link = sess_link();
   if (link >= 0)
      dexble_write(link, OT_WRITE, data, n, 0);
}

void ot_drv_subscribe(void)
{
   int link = sess_link();
   if (link < 0)
      return;
   dexble_subscribe(link, OT_NOTIFY, 0);
   /* Queued on the meter's own link, so its model/firmware are known by the
    * time the sync finishes and can be written into its provenance. */
   dexble_request_devinfo_link(link);
}

/* The protocol exchange is over. Ask for the link to close and drop the busy
 * latch -- but DO NOT un-arm, and do not treat this as the meter being gone.
 *
 * NOT meter_unarm_link. "The meter has powered itself off by now" is the
 * tempting justification and an HCI capture falsifies it: a Verio stays awake
 * about THIRTY-FIVE SECONDS after a fingerstick (observed: connected 07:07:50,
 * supervision timeout 07:08:25), and un-arming while it
 * is still connected makes meter_armed() false -- so the 1 Hz tick calls
 * meter_sync_start again, reconnects, and re-runs the whole exchange. The
 * capture shows 29 complete syncs in 29 seconds inside ONE connection, each
 * re-writing the CCCD and re-reading model, firmware and manufacturer. The
 * close never even reached the controller (no HCI Disconnect appears for
 * that address at all), so the un-arm was this function's only lasting
 * effect, and its only effect was to start the next lap.
 *
 * The link is released where the link actually dies: meter_hook_disconnected,
 * on the real GATT disconnect -- whether that comes from the close below or,
 * as it usually does, from the meter powering itself off. Only then does the
 * tick re-arm the standing connect, which is what makes the NEXT fingerstick
 * catchable without the user touching the phone.
 *
 * A second fingerstick taken during the same power-on is therefore not picked
 * up until the meter cycles. Nothing is lost: the walk is index-based, so the
 * next sync reads both records. */
void ot_drv_disconnect(void)
{
   /* ONE reading of the session, used for all three steps below: which link
    * to close, which meter to stamp, and which slot to mark as waiting. Read
    * separately they could name three different exchanges. */
   struct msess s;
   msess_get(&s);
   if (s.link >= 0)
      dexble_link_close(s.link);
   /* Stamp the completion BEFORE the session is cleared -- meter_rt is keyed
    * by registry id, but the cooldown that keeps a still-awake meter from
    * being re-synced is read off this. */
   if (s.src > 0)
      meter_rt_done(s.src, mono_s()); /* a COOLDOWN, not an instant */
   /* Ending the exchange and marking the link as waiting on its teardown are
    * ONE step: between them, the watchdog could see an idle link with no
    * wait stamp and re-arm the meter that is still tearing down. */
   if (s.link >= 0)
      msess_end(s.link, mono_s()); /* an INTERVAL: see util.h */
}

void ot_drv_status(const char *s)
{
   set_status(s);
   /* Record the driver's live phase text against the meter that currently
    * owns the sync, so its per-device STATE row can show a descriptive step
    * ("COUNT", "READING", "NOTHING NEW") rather than a flat "SYNCING". The
    * "METER: " prefix is stripped -- the row is already known to be a meter.
    */
   int src = msess_src();
   if (src > 0) {
      const char *p = s;
      if (strncmp(p, "METER: ", 7) == 0)
         p += 7;
      /* Any driver phase means we CONNECTED to this meter (the first is
       * "HELLO" on connect), so this IS a sync -- the text and the LAST SYNC
       * stamp land together, in one operation. A meter that connects but
       * yields no new record (e.g. the record read was refused) otherwise
       * stayed "OFF / NEVER" despite plainly having synced. */
      meter_rt_stat(src, p, realtime_s());
   }
}

int ot_drv_reading(long naive, int mg_dl)
{
   /* THE METER'S CLOCK IS A CLOCK FACE, not an instant: naive local time with
    * no zone on it, so something has to say which offset was in force when
    * the fingerstick was taken. Without that the reading lands 7-8 hours off,
    * which is exactly the discrepancy the HCI capture showed; with the wrong
    * one -- the offset at IMPORT rather than at the reading -- every record
    * from the far side of a DST boundary lands an hour off instead.
    *
    * And in the repeated hour of a fall-back there are TWO right answers, so
    * this is not a conversion at all but a decision, taken across the walk
    * rather than per record: see meterlogic.h. The walk's own order is the
    * evidence; realtime_s() is passed only as an upper bound ("a fingerstick
    * cannot have been taken after it was imported") and never as an ordering,
    * because a wall-clock correction mid-walk would then reorder the log.
    *
    * The offset is stored alongside the raw value so a wrong conversion stays
    * repairable, and an undecidable one is recorded as such rather than
    * quietly settled -- meter_rt_ambiguous, below. */
   /* SNAPSHOT, because a record that the bound below REJECTS must not become
    * the previous instant the next record's repeated-hour decision leans on.
    * An implausible timestamp is exactly the kind that would force the next
    * ambiguous record to the wrong side. */
   struct meter_seq seq_before = g_mseq;
   struct meter_stamp st =
       meter_stamp_step(&g_mseq, naive + OT_EPOCH, realtime_s(), meter_zone, 0);
   long tz = st.off;
   long t  = st.t;
   /* THE EXACT timestamp bound lives here, not in otble.c: this is the first
    * point at which `t` is a true instant rather than a naive local clock
    * reading. A future-dated record sorts to the head of the history
    * permanently and is re-admitted on every restart, which is what the
    * meter clock is capable of producing. One hour of slack absorbs a DST
    * edge. */
   /* Generous, because this is measured against the PHONE's clock, which can
    * legitimately be wrong (a flat battery before NTP, a dead RTC, a
    * hand-set date). A tight bound here rejects perfectly good records
    * whenever the phone's clock is off, so this catches only records wrong by
    * more than any plausible clock skew or timezone. */
   if (t <= 0 || t > realtime_s() + (15L * 3600)) {
      LOGI("meter reading at %ld (raw %ld) implausible, rejected", t, naive);
      g_mseq = seq_before; /* it is not evidence about anything */
      return 0;            /* the driver must not persist its walk past this */
   }
   /* THE GUESS IS RECORDED AS A GUESS. The reading is still stored -- a
    * fingerstick refused for being taken in the repeated hour is a
    * fingerstick lost, and losing one is strictly worse than storing it an
    * hour out -- but the count and the instant that was NOT chosen are kept
    * against this meter, so the stamp can be repaired rather than merely
    * doubted. Dropping this is what makes a stated guess indistinguishable
    * from a fact. */
   if (st.ambiguous) {
      LOGW("meter reading at %ld (raw %ld) is in the repeated hour: %ld is "
           "equally valid and nothing in the walk decides between them",
           t, naive, st.t_alt);
      /* NOT set_status: the driver publishes a phase text a moment later
       * ("SYNCED", "NOTHING NEW") and would overwrite this within the same
       * exchange, which is a worse signal than none. The durable place for
       * this is the meter's own runtime record, where it survives the sync
       * that produced it. */
      meter_rt_ambiguous(msess_src(), st.t_alt);
   }
   if (st.shifted)
      LOGW("meter reading raw %ld names a local time that never existed "
           "(the skipped hour); moved forward to %ld",
           naive, t);
   /* A fingerstick is a reading like any other: same operation, same order.
    * `prime` is -1 because a BGM is never the big number's source, and
    * `warm` 0 because a meter has no warm-up. Meters are never rescaled, so
    * the factor is 1000. Fingersticks ride the same cursor-driven sync. */
   struct reading_event mev  = {.t          = t,
                                .glu        = mg_dl,
                                .trend      = 127,
                                .src        = msess_src(),
                                .kind       = KIND_BGM,
                                .raw        = naive,
                                .tz         = tz,
                                .rescale_pm = 1000,
                                .prime      = sensor_primary_id()};
   struct reading_result mrr = store_record(&mev, CHIRP_MAX_GAP_S);
   /* WHAT HAPPENED TO IT, as its own type: a fingerstick that
    * lands OLDER than the display window is still a record, and a duplicate
    * is not one. Both were `isnew` and read as a truth value. */
   enum hist_insert_result got = mrr.inserted;
   if (hist_kept(got) && !mrr.persisted)
      set_status("METER: WRITE FAILED");
   LOGI("meter reading %d mg/dL at %ld (raw %ld)%s", mg_dl, t, naive,
        hist_kept(got) ? "" : " (already stored)");
   shell_ui_dirty();
   return 1;
}

void ot_drv_done(int new_records)
{
   /* The meter is first registered with nothing but its address -- DIS has
    * not answered yet at pair time. Once it has, re-mint: identical fields
    * reuse the id, and a genuine difference mints a new one, which is
    * exactly the rule that keeps an id pinned to one (device, firmware) pair
    * for good. Readings taken before we knew the firmware keep citing the
    * older id, which is the truthful record of what we knew then. */
   /* BOTH, not either. The two DIS reads are separate serialized GATT ops
    * and a sync commonly ends after one round trip, so "model present, fw
    * still empty" is the normal intermediate state -- and minting against
    * (model, "") does not match the stored (model, fw), producing a NEW id
    * and a rebind, which the next complete sync mints straight back. The
    * meter oscillated between ids, appending a provenance row per flip and
    * splitting its fingerstick history across them in a file that is never
    * rewritten. */
   char dmodel[24];
   char dfw[24];
   mutex_lock(&mdis_lk);
   str_snapshot(dmodel, sizeof dmodel, g_meter_model);
   str_snapshot(dfw, sizeof dfw, g_meter_fw);
   mutex_unlock(&mdis_lk);
   int src = msess_src();
   if (src && dmodel[0] && dfw[0]) {
      /* COMPLETE the row, do not re-mint it.
       *
       * A device is identified by (type, MAC) ALONE, so minting again with
       * the model and firmware filled in returns the id we already have --
       * the mint cannot carry them onto the existing row. Without an explicit
       * complete, a meter's provenance row keeps its empty model and firmware
       * forever,
       * exactly contrary to what this block claims to do -- and the
       * reconcile completion pass walks CGM links, so nothing else filled
       * them either. sensor_complete is the mechanism the CGM path already
       * uses: it fills only what is missing and cannot fork an id. */
      /* == 1, not merely non-zero: sensor_complete is TRI-STATE. It returns 1
       * when it filled something, 0 when there was nothing to fill, and -1
       * when the durable append FAILED -- which its own comment calls out as
       * the case that must never pass unnoticed, because a reading may then
       * cite a provenance row nobody has. Truth-testing it logged "completed"
       * at exactly the moment the row did not reach the disk. */
      int cr = sensor_complete(src, "", dmodel, dfw, 0);
      if (cr == 1)
         LOGI("meter provenance completed: id %d (%s / %s)", src, dmodel, dfw);
      else if (cr < 0)
         LOGW("meter provenance NOT saved for id %d: the row did not reach "
              "the disk",
              src);
   }
   if (meter_index_save(src, ot_last_index()) == 0)
      LOGI("meter sync complete: %d new record(s), index now %d", new_records,
           ot_last_index());
   else
      LOGW("meter sync index NOT saved; the next sync will retry records");
   shell_ui_dirty();
}

/* THE ZONE, as the one question civil.h asks: what was the offset at this
 * instant. SOLVED, not converged: civil_resolve answers it directly. A
 * fixed-point iteration -- guess with the offset at the naive value, ask again
 * with the result -- settles on ONE of the two answers inside the repeated
 * hour and cannot report that there were two.
 *
 * A PURE FUNCTION OF `t`, and it must stay one: the cached offset is only the
 * fallback, used when there is no VM to ask, because a conversion that
 * depends on WHEN it ran gives a re-imported record a different timestamp and
 * BGM dedup matches on the exact timestamp.
 *
 * dexble_env(), NOT g_act->env. This runs on a BLE BINDER thread (jni_notify
 * -> ot_on_notify -> ot_drv_reading), and a JNIEnv is valid only on the
 * thread that produced it; g_act->env belongs to the main looper. Using it
 * here aborts under CheckJNI and corrupts the main thread's local-ref frame
 * otherwise -- on the hot path of every single fingerstick import. */
static long meter_zone(void *ctx, long t)
{
   (void)ctx; /* the env is the calling thread's, so it cannot be passed in */
   JNIEnv *env = dexble_env();
   return env ? tz_offset_at(env, t) : tz_off_now();
}

/* --- what the rest of the app is allowed to ask (see meter.h) --- */

int meter_src(void)
{
   return msess_src();
}

void meter_bind(int id, const char *mac)
{
   msess_bind(id, mac);
}

int meter_busy(void)
{
   return msess_busy();
}

/* THE LOCK LIVES WITH THE TABLE, not with each caller.
 *
 * g_link_meter is written under driver_lock (link_set_meter tells the
 * transport in the same critical section), so every reader needed that lock
 * too -- and every reader took it by hand, or forgot to. One of them read it
 * TWICE in the same function under different locks and got two different
 * answers, which routed a meter's device-info into a CGM's provenance row.
 *
 * Recursive, so the callers that legitimately hold it across a longer
 * sequence pay nothing. */
int meter_link_is(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return 0;
   return driver_link_is_meter(link);
}

void meter_link_idle(int link, long when)
{
   msess_idle_set(link, when);
}

int meter_note_advert(int id, int rssi, long now, long window)
{
   /* ONE OPERATION, INCLUDING THE THROTTLE: whether this advert takes the
    * turn, the instant the user reads (persisted), the same event as an
    * INTERVAL for the throttle, and the signal the advert carried.
    *
    * The two clocks are taken here rather than passed through, so a caller
    * cannot mix them up (see meterstore.h): `now` is the wall clock the
    * screen shows, mono_s() is the interval the throttle measures. Reading
    * READ, DECIDE AND RECORD IN ONE STEP, here: as three steps in the caller,
    * two scan callbacks for one meter -- which is exactly what a meter waking
    * up delivers -- can both pass the throttle.
    *
    * Whether the advert carried a usable RSSI at all is this caller's fact
    * (a scan result can arrive with none); whether the NUMBER is a plausible
    * signal is the store's, and it applies that rule to every write. */
   if (!meter_rt_advert_turn(id, now, mono_s(), rssi, 1, now, window))
      return 0;
   /* "survives a restart" is a CLAIM, and it is this call that either makes
    * it true or does not. An advert is delivered once: there is no second
    * copy of this observation to write later, which is why the failure is
    * said out loud and why the table remembers it is owed. */
   if (meter_sync_save() != 0)
      LOGW("meter %d: advert not persisted -- will retry", id);
   return 1;
}

void meter_ui_of(int id, struct meter_ui *out)
{
   if (!out)
      return;
   /* ONE COPY, and everything the row shows comes out of it -- including the
    * signal. A second read (meter_rssi_of) of a record a binder thread is
    * writing puts a time from one instant beside a signal from another, on
    * the same row.
    *
    * The phase text is copied INTO the caller's struct for the same reason: a
    * pointer into the table is a string that changes while it is drawn. */
   struct meter_rt rt;
   int have     = meter_rt_read(id, &rt);
   out->sync_t  = have ? rt.sync_t : 0;
   out->rssi_ok = have && rt.rssi_ok;
   out->rssi    = out->rssi_ok ? rt.rssi : 0;
   out->rssi_t  = out->rssi_ok ? rt.rssi_t : 0;
   str_snapshot(out->stat, (int)sizeof out->stat, have ? rt.stat : "");
   /* Only the meter that OWNS the running sync shows SYNCING -- with two
    * meters registered, a shared flag showed both as syncing. */
   struct msess s;
   msess_get(&s);
   out->syncing = (s.busy && id == s.src);
}

/* THE METER'S OWN DEVICE-INFORMATION STRINGS, set through here.
 *
 * A SETTER, not a raw `char *` handed to the reading path: that would put the
 * sanitising rule -- and the knowledge of how long the buffer is -- in a
 * different file from the buffer. The value arrives from a
 * GATT characteristic on a binder thread: it is a stranger's bytes. A comma
 * would split the provenance row it is written into, a control character
 * would corrupt the single-line file, and either survives forever in an
 * append-only record.
 *
 * So the meter sanitises its own: printable ASCII, no commas, bounded, always
 * terminated. `which` is METER_DIS_MODEL or METER_DIS_FW. */
void meter_set_dis(int which, const char *val)
{
   char *dst = 0;
   if (which == METER_DIS_MODEL)
      dst = g_meter_model;
   else if (which == METER_DIS_FW)
      dst = g_meter_fw;
   if (!dst || !val)
      return;
   mutex_lock(&mdis_lk);
   int k = 0;
   for (; val[k] && k < 22; k++) {
      unsigned char c = (unsigned char)val[k];
      dst[k]          = (c < 0x20 || c > 0x7e || c == ',') ? '_' : (char)c;
   }
   dst[k] = 0;
   mutex_unlock(&mdis_lk);
}

/* THE TRANSPORT'S ANSWER TO A REQUEST THE METER PROTOCOL MADE.
 *
 * DROPPED FOR A METER LINK, these completions leave a write the stack refused
 * -- characteristic not found, link replaced, writeCharacteristic false --
 * with otble waiting for an answer to a request that never went out.
 *
 * ONLY OT_WRITE's OWN COMPLETIONS ARE THE PROTOCOL'S. The same link also
 * carries the CCCD write behind ot_drv_subscribe (reported under the NOTIFY
 * characteristic's uuid) and the Device Information reads queued beside it; a
 * failure on any of those is not a failed COMMAND, and treating it as one
 * would end a session that is proceeding perfectly well. A subscribe that
 * fails is reported for OT_NOTIFY and matters just as much, so it is passed
 * on too -- the protocol is in P_SUB then and has a request outstanding.
 *
 * The generation is read HERE rather than carried through Java: the transport
 * has no idea what a request is, and the only thing that can go stale between
 * the write and its completion is another exchange starting -- which bumps
 * the generation, so passing the CURRENT one means a completion arriving
 * after that is matched against a request that has gone and is ignored,
 * which is exactly the intent. */
static void meter_hook_written(const char *uuid, int status)
{
   if (!uuid)
      return;
   if (strcmp(uuid, OT_WRITE) != 0 && strcmp(uuid, OT_NOTIFY) != 0)
      return;
   ot_on_written(ot_request_gen(), status == 0);
}

/* The two files the meter runtime owns (see meterstore.h). */
/* THE METER'S HALF OF THE ROUTING, registered once so the driver can dispatch
 * a callback to whichever state machine owns the link without knowing what a
 * OneTouch is -- and, more importantly, without the transport having to hold
 * the driver's lock across a decision it made itself. See dexdriver.h. */
static const struct driver_meter_ops g_meter_ops = {
    .connected       = meter_hook_connected,
    .disconnected    = meter_hook_disconnected,
    .on_connected    = ot_on_connected,
    .on_disconnected = ot_on_disconnected,
    .on_notify       = ot_on_notify,
    .on_written      = meter_hook_written,
};

void meter_register_ops(void)
{
   driver_set_meter_ops(&g_meter_ops);
}

int meter_paths(const char *dir)
{
   int ok = 1;
   char idx[256];
   char sync[256];
   if (!(data_path(idx, sizeof idx, dir, "/meter.idx")))
      ok = 0;
   if (!(data_path(sync, sizeof sync, dir, "/meter.sync")))
      ok = 0;
   meter_store_paths(idx, sync);
   return ok;
}

/* ---- WHAT THE METER'S OWN FILES SAID ------------------------
 *
 * THE TYPED ANSWER REACHES STARTUP. A wrapper that swallowed it -- `void
 * meter_state_load(void) { meter_sync_load(); }` -- would make a meter.sync
 * that could not be read look exactly like a first run. What that costs: LAST
 * SEEN and the signal beside it come back blank or wrong, the re-arm throttle
 * loses its cooldown and re-syncs a meter
 * that has just been synced, and nothing anywhere says the file was the
 * problem.
 *
 * The answer travels now, and startup folds it into the same load_worse
 * aggregate every other file's answer goes through (main.c). */
enum load_result meter_state_load(void)
{
   return meter_sync_load();
}

/* PAIR a meter that has just been registered: seed its record index, take a
 * link, arm it, ask for the OS bond and connect.
 *
 * Returns 1 when a connect is outstanding. On every other path the link is
 * RELEASED -- an armed link with nothing behind it is exactly what stops the
 * tick from ever retrying it.
 *
 * Split out of commit_pair, which had grown its own copy of the arming
 * discipline. That copy left the arm table EMPTY, and
 * meter_hook_connected identifies the meter only from that table -- so it
 * refused the very connection this path had just issued. Pairing then worked
 * only by accident: an unarmed link also makes meter_armed() false, so the
 * 1 Hz tick called meter_sync_start, which allocated the meter a SECOND link
 * and armed that one properly. The pairing succeeded and a link leaked every
 * time. */
int meter_pair(int id, const char *mac)
{
   char dt[DEVTAG_LEN];
   /* Seed THIS meter's stored index. Without it the driver keeps whatever
    * last_index the last synced meter left in its static state, so a
    * newly paired meter with a higher counter has its oldest records skipped
    * -- and ot_drv_done then persists that skipped index under the new
    * meter's id, making the loss permanent.
    *
    * otble's statics are otherwise only touched under driver_lock, from
    * jni_notify / jni_disconnected on a binder thread. */
   {
      /* The same distinction as meter_sync_start's: a file that
       * could not be read is not "nothing stored". Here the seed only
       * REPLACES what otble already holds for this link, so an unreadable
       * file leaves the driver's current index alone rather than resetting
       * it to -1. */
      int seed             = -1;
      enum load_result how = meter_index_load(id, &seed);
      if (how != LOAD_ERROR)
         driver_meter_seed_index(seed);
      else
         LOGW("meter %d: the stored index could not be read; the driver "
              "keeps the index it has",
              id);
   }
   mutex_lock(&mdis_lk); /* the lock pancra_devinfo writes these under */
   g_meter_model[0] = 0;
   g_meter_fw[0]    = 0;
   mutex_unlock(&mdis_lk);

   int mlink = meter_alloc_link(mac);
   /* BOTH bounds, as meter_sync_start does: the arm table is indexed by this
    * below, so a value at or past LINK_MAX is a write off the end. */
   if (mlink < 0 || mlink >= LINK_MAX) {
      LOGI("meter dev %s: no free link to pair on", devtag(mac, dt));
      return 0;
   }
   meter_link_set(mlink, 1);
   /* Armed BEFORE the connect: the callback lands on a binder thread and a
    * meter this close answers in milliseconds. */
   driver_link_arm(mlink, mac);
   /* Claimed outright rather than tested: the user has just registered this
    * meter and is standing in front of it, and the link was allocated for it
    * one line ago. */
   msess_begin(mlink, id, mono_s()); /* an INTERVAL: see util.h */
   /* Ask for the OS bond NOW rather than letting the first GATT touch
    * trigger it minutes from now: the meter shows a passkey and Android
    * prompts for it, and a prompt that arrives while the user is still
    * looking at this screen is one they can actually answer. */
   dexble_create_bond(mac);
   LOGI("registered meter id=%d dev %s on link %d; connecting to bond", id,
        devtag(mac, dt), mlink);
   if (dexble_meter_connect(mlink, mac))
      return 1;
   LOGI("meter dev %s: pairing connect did not reach the transport",
        devtag(mac, dt));
   meter_release_link(mlink);
   msess_end(mlink, 0);
   return 0;
}

/* THE LINK'S ROLE, which decides where its GATT callbacks go. Kept by the
 * driver because the driver's own callbacks read it -- a str_snapshot racing
 * a strcmp really can be read half-written, which would mis-identify the
 * meter that just connected. */
void meter_link_set(int link, int on)
{
   driver_link_set_meter(link, on);
}
