// SPDX-License-Identifier: GPL-3.0
// linkinfo.c --- per-link telemetry and identity (see linkinfo.h)
// Copyright 2026 Jakob Kastelic

#include "linkinfo.h"
#include "bletrans.h" /* dexble_request_devinfo_link: asking the link again */
#include "clock.h"
#include "devinfo.h"   /* info_set: the process-global DIS strings */
#include "dexdriver.h" /* the session, and the per-link monotonic deadlines */
#include "log.h"
#include "meter.h"    /* a meter link's DIS strings are the meter's own */
#include "sensors.h"  /* the registry: what a link's address resolves to */
#include "settings.h" /* the process-global model/firmware/manufacturer */
#include "shell.h"
#include "store.h"  /* store_note_rssi: the retained "last known" readout */
#include "thread.h" /* two leaf mutexes; see linkinfo.h */
#include "util.h"
#include <string.h>

/* THE LIVE CONNECTION'S SIGNAL STRENGTH, as one number.
 *
 * Not per link, and deliberately: it is the main screen's single readout and
 * the value stamped onto a stored row, both of which are about "the
 * connection this reading came in on". The per-link table above is what the
 * per-device rows read. Written by the binder thread that measured it and
 * read by the binder thread that stores a reading -- one word, and a stale
 * one is last connection's dBm on one row, which is what the freshness
 * deadline beside it (driver_rssi_fresh) is for. */
static int g_conn_rssi;

int linkinfo_conn_rssi(void)
{
   return g_conn_rssi;
}

/* THE TWO DEADLINES ARE THE DRIVER'S.
 *
 * A wall-clock stamp for the RSSI freshness window, and a per-link array of
 * wall-clock stamps for the device-information re-read throttle, are
 * `realtime_s() - stamp > interval`, and a backward NTP correction makes that
 * difference NEGATIVE -- so for the whole hour it takes wall time to catch up
 * the DIS strings were never re-requested (a sensor minted with no model or
 * firmware stays that way) and every stale signal reading counted as "this
 * connection" and was stamped onto stored rows. See the liveness block in
 * scanlogic.h.
 *
 * They are driver_dis_claim / driver_rssi_note+driver_rssi_fresh now: per
 * link, monotonic, and -- for the DIS one -- a single-winner atomic claim
 * rather than a test followed by a store. */

/* Per-CGM-link DIS strings. The process-global model/firmware (settings.c) are
 * process-global and shared by every link, which is fine for the headline
 * display but WRONG for provenance -- see pancra_devinfo. Minting uses these.
 */
static char g_model_l[LINK_MAX][24], g_fw_l[LINK_MAX][24];

/* PER-LINK SIGNAL STRENGTH, retained as "last known" so it never expires
 * into "--" while readings lag. Kept HERE, where the measurement arrives
 * (pancra_rssi), and read by the frame -- it lived in model.c, so this file
 * had to include the frame builder to report a number it had just been
 * handed, and the frame builder was then part of every workflow cycle. */
/* ONE SIGNAL READING IS THREE NUMBERS, AND THEY TRAVEL TOGETHER.
 *
 * The value, whether there is one, and when it was taken. They were three
 * plain arrays written by the Bluetooth binder callback and read by the frame
 * builder on the main thread with nothing between them -- a data race in the
 * language's terms, and one whose visible form is worse than torn text: the
 * reader can take the dBm of a NEW measurement with the timestamp of the OLD
 * one and put the pair on a device row, so a signal that arrived a moment ago
 * is labelled with when the previous one did. A stale row that says it is
 * stale is honest; a fresh number wearing an old time is not.
 *
 * A LEAF LOCK, not an atomic per field. Three atomics would fix the tearing
 * of each number and not the pairing, which is the whole defect: what has to
 * be indivisible is the TUPLE. The lock is taken by these two functions only,
 * holds nothing while taken and calls nothing under it, so it cannot
 * participate in the driver -> registry -> history order at all. */
static struct mutex rssi_lk = MUTEX_INIT;
static int g_link_rssi[LINK_MAX], g_link_rssi_ok[LINK_MAX];
static long g_link_rssi_t[LINK_MAX];

void linkinfo_note_rssi(int link, int dbm, long when)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   mutex_lock(&rssi_lk);
   g_link_rssi[link]   = dbm;
   g_link_rssi_t[link] = when;
   /* THE VALID FLAG LAST, so that a reader can never find `ok` set for a
    * tuple that is only half written. Under the lock this is belt and braces;
    * it is written this way round because the order is the invariant, and the
    * next person to reach for a lock-free version of this function needs to
    * find it already stated. */
   g_link_rssi_ok[link] = 1;
   mutex_unlock(&rssi_lk);
}

int linkinfo_rssi(int link, int *dbm, long *when)
{
   *dbm  = 0;
   *when = 0;
   if (link < 0 || link >= LINK_MAX)
      return 0;
   mutex_lock(&rssi_lk);
   const int ok = g_link_rssi_ok[link];
   if (ok) {
      /* BOTH, INSIDE THE LOCK. Reading the flag under it and the values
       * outside would be the same defect with more ceremony. */
      *dbm  = g_link_rssi[link];
      *when = g_link_rssi_t[link];
   }
   mutex_unlock(&rssi_lk);
   return ok;
}
/* AND THEIR OWN LOCK. These strings were written and read under the REGISTRY
 * lock -- a lock belonging to another module, protecting a different thing,
 * taken here only because it happened to be public. That is how the registry
 * lock ended up held across driver calls elsewhere. They are written by a
 * binder thread byte-by-byte (devinfo_copy) and read by the mint path, so
 * they do need one; it is a leaf, taken by nothing else and holding nothing
 * else while taken. */
static struct mutex dis_lk = MUTEX_INIT;

/* ---- THERE IS NO AMBIENT SOURCE ANY MORE -------------------
 *
 * A file-static "current source" was the sensor the driver is bonded to, set by
 * the 1 Hz reconcile and read on the BLE thread as the fallback provenance for
 * a reading whose link no slot claims yet. Three things were wrong with it and
 * the third is the one that costs data:
 *
 *   - it was written by the reconcile on the main thread and read by ingest
 *     on a binder thread, with nothing between them;
 *   - nothing cleared it when its sensor was retired, so it outlived the
 *     device it named;
 *   - so the first sample from a NEW, not-yet-registered CGM could be
 *     stamped with a RETIRED sensor's id -- permanently, into an append-only
 *     log, and shown on that retired device's row for ever.
 *
 * What the fallback was for is a genuinely EMPTY registry: a fresh install
 * where nothing is registered yet and 0 -- the id legacy pre-registry rows
 * carry -- is unambiguous. That case is handled explicitly where it arises,
 * and every other unmapped link is DEFERRED: the next sample arrives in ~5
 * minutes, by which time the reconcile has registered the sensor. A missed
 * sample is recoverable; a wrong attribution is not.
 */

/* The sensor id for the link a reading actually arrived on.
 *
 * A reading MUST be stamped with the sensor that produced it, not with a
 * global "current" id. The link is carried as an argument the whole way down
 * -- the driver has no ambient "current link" any more -- so while drv_glucose
 * runs, `link` IS the originating link, and that is the only trustworthy
 * attribution available at this depth. Stamping a single global instead meant
 * two CGMs shared one id, and per-source dedup (150 s window) then silently
 * DISCARDED whichever sensor's sample landed second: roughly half of one
 * sensor's data, never written to the log and never plotted.
 *
 * Returns -1 when the link maps to no registered slot, so the caller can
 * refuse to log rather than invent a provenance. */

int src_for_link(int link)
{
   /* Match on the session ADDRESS, and walk g_slot under the registry lock.
    *
    * The address is the only identity that cannot be shifted out from under a
    * live connection: sensor_forget() renumbers g_slot while the remaining
    * sensors keep streaming, so anything keyed on a slot's POSITION silently
    * re-points at a different sensor. With two CGMs that stamped one sensor's
    * id onto the other's readings -- in an append-only log that is never
    * rewritten, so the mistake is permanent. The lock matters for the same
    * reason: the main thread can be mid-shift while this runs on a BLE
    * thread. */
   /* THE SESSION IS READ AS ONE SNAPSHOT, which driver_session_of does under
    * the driver's own lock.
    *
    * The caller need NOT hold driver_lock. That would be required if the
    * driver selected a link into file-statics every function dereferenced,
    * since a call from an unlocked thread could stomp them out from under a
    * binder thread mid-dispatch -- attributing a reading to the other sensor,
    * or writing one sensor's key over another's. There are no such statics: a
    * link is an argument, and no caller can move another thread's context.
    * What remains is the ordinary requirement
    * that the fields be read together, and driver_session_of does exactly
    * that. */
   struct dex_session s;
   /* A link this driver does not have has no session and therefore no
    * identity to resolve -- the same answer as a link with no MAC. */
   if (!driver_session_of(link, &s) || !s.mac[0])
      return -1;
   int id = -1;
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n && id < 0; i++)
      if (v.have_rec[i] && sensor_kind(v.rec[i].type) == KIND_CGM &&
          !strcmp(v.rec[i].identity, s.mac))
         id = v.slot[i].id;
   return id;
}

/* live connection signal strength from readRemoteRssi (no sensor-battery
 * cost)
 */
void pancra_rssi(int link, int rssi)
{
   /* PER LINK. jni_rssi knows which link the measurement came from; dropping
    * it into globals that fill_sensor stamps onto EVERY CGM slot means that
    * with a Stelo and a G7 both worn, each device screen can show the other's
    * signal, which is the one number on that row the user cannot
    * sanity-check. The globals are for the main screen's single readout; the
    * per-link copy is what the per-device row reads. */
   int lk = link;
   if (lk >= 0 && lk < LINK_MAX) {
      /* TWO STAMPS FOR ONE MEASUREMENT, and both are needed. The realtime one
       * goes to linkinfo_note_rssi because the per-device row DISPLAYS when
       * the signal was last seen; the monotonic one is the freshness DEADLINE
       * that decides whether this measurement belongs to the connection a
       * reading arrives on. A wall-clock correction moves the first (it is a
       * civil instant, and that is what it is for) and cannot move the
       * second. */
      linkinfo_note_rssi(lk, rssi, realtime_s());
      driver_rssi_note(lk);
   }
   g_conn_rssi = rssi;
   /* Latch the CGM's last signal strength the MOMENT it is measured on
    * connect, exactly like pancra_meter_rssi does for a meter -- not gated
    * behind a fresh datapoint. Otherwise the Stelo's SIGNAL row drops to
    * "--" whenever readings lag, while a meter (which latches on connect)
    * keeps showing its last value. This is a retained "last known" display,
    * so it never expires.
    */
   store_note_rssi(rssi);
   LOGI("rssi %d dbm", rssi);
   shell_repaint();
}

/* Copy a DIS string into a 24-byte field, NEUTERING the CSV delimiters.
 *
 * These strings come off the sensor's GATT server and go straight into
 * sensors.csv as bare %s fields -- no quoting, no escaping. A model or
 * firmware value containing a COMMA shifts every following field on parse
 * (activation and paired times land in the wrong columns); one containing a
 * NEWLINE splits the row in two, and sensor_mint documents exactly where
 * that leads: an unparseable row hides an id from the loader, maxid goes
 * backwards, and the next mint REISSUES A LIVE ID -- the one failure the
 * whole provenance design exists to make impossible, and it is permanent
 * because the file is never rewritten.
 *
 * The value is attacker-controlled by anything that can present the locked
 * MAC, and merely quirky vendor firmware could do it by accident. Substitute
 * rather than truncate: an empty firmware field is itself meaningful (it
 * marks the row stale and drives the re-mint pass), so dropping characters
 * could turn a hostile string into a silent re-mint loop. */
static void devinfo_copy(char *dst, const char *src)
{
   int k = 0;
   for (; src[k] && k < 22; k++) {
      unsigned char c = (unsigned char)src[k];
      dst[k]          = (c < 0x20 || c > 0x7e || c == ',') ? '_' : (char)c;
   }
   dst[k] = 0;
}

/* device-info string (serial / firmware / software) read from DIS 0x180A */
void pancra_devinfo(int link, const char *uuid, const char *val)
{
   if (!val || !val[0] || !uuid)
      return;
   /* uuid is the full 128-bit form "0000XXXX-0000-1000-8000-00805f9b34fb";
    * the 16-bit assigned number sits at offset 4. Guard the length before
    * indexing uuid+4 so a short/empty string can't read out of bounds. */
   int ulen = 0;
   while (uuid[ulen] && ulen < 8)
      ulen++;
   if (ulen < 8)
      return;
   /* A meter's identity must not land in the CGM's globals: each sensor's
    * model/firmware is part of its permanent provenance, and mixing them
    * would attribute readings to hardware that never produced them. */
   /* READ THE ROUTING BIT UNDER THE LOCK. link_set_meter's comment claims
    * this function is one of the binder-thread readers that "already hold
    * it" -- the premise the writer's own locking rests on -- and it did not.
    * The write lands on the main thread in meter_sync_start/commit_pair
    * immediately before the connect, and this read arrives on a binder
    * thread just after it, so a stale value is exactly the ordering the
    * lock's barrier exists to prevent. Snapshot and release: the rest of the
    * function takes the DIS lock, and there is no reason to hold both. */
   /* driver_link_is_meter takes the driver lock itself: the table it
    * reads is written under that lock, and having every caller remember to is
    * how one function ended up reading it twice and getting two answers. */
   int is_meter = driver_link_is_meter(link);
   /* WHICH FIELD this UUID carries, decided once. -1 = none of ours.
    *
    * Neither branch writes anywhere itself: the meter and the settings module
    * each own their strings and sanitise what arrives, because this value
    * comes off a GATT characteristic on a binder thread and ends up in files
    * that are never rewritten. */
   int mdis = -1; /* the meter's field, when this is a meter link */
   int pref = -1; /* or the process-global preference field, when it is not */
   if (is_meter) {
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         mdis = METER_DIS_MODEL;
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         mdis = METER_DIS_FW;
      else
         return;
   } else if (strncmp(uuid + 4, "2a24", 4) == 0) {
      pref = SET_DIS_MODEL;
   } else if (strncmp(uuid + 4, "2a26", 4) == 0) {
      pref = SET_DIS_FW;
   } else if (strncmp(uuid + 4, "2a29", 4) == 0) {
      pref = SET_DIS_MFR;
   }
   /* Keep a PER-LINK copy as well. sp.model/sp.fw are
    * process-global and shared by every CGM link, and the devinfo re-read is
    * skipped once they are non-empty (and they persist to disk), so a second
    * sensor was minted with the FIRST sensor's model and firmware -- written
    * into an append-only provenance file that is never rewritten, and used as
    * part of the id-reuse key. Pair a G7 after a Stelo and its permanent record
    * claimed Stelo hardware. Minting reads the per-link copy. */
   /* THE PER-LINK COPY IS THE MINT INPUT, so it is what needs the lock.
    *
    * Locking only the process-global sp.model/sp.fw below, which are
    * display-only, is inert: the arrays sensor_mint actually reads are still
    * written byte-by-byte with no lock at all. A torn read (terminator not
    * yet written, so
    * "1.4" over "1.2.3" reads as "1.4.3") matches no stored row, mints a NEW id
    * and rebinds the slot, permanently splitting one physical sensor into two
    * identities in an append-only file. Readers hold the same lock: reading_dis
    * takes it, and it is the only way out of this file. */
   /* REUSE THE SNAPSHOT -- do not re-read g_link_meter here.
    *
    * This read was under the REGISTRY's lock, which is the wrong lock for
    * that variable (link_set_meter writes it under driver_lock), and taking
    * driver_lock inside the registry's would invert the documented
    * driver -> reg order. But the deeper problem is that it was a SECOND,
    * independent read of a bit already decided above: link_set_meter landing
    * between the two makes this function pick the meter branch for `dst` and
    * the CGM branch for the per-link copy. Since the per-link copy is the
    * mint input, that writes a METER's model into the array sensor_mint
    * reads, in an append-only provenance file that is never rewritten. One
    * snapshot, one decision. */
   mutex_lock(&dis_lk);
   if (link >= 0 && link < LINK_MAX && !is_meter) {
      char *ld = 0;
      if (strncmp(uuid + 4, "2a24", 4) == 0)
         ld = g_model_l[link];
      else if (strncmp(uuid + 4, "2a26", 4) == 0)
         ld = g_fw_l[link];
      if (ld)
         devinfo_copy(ld, val);
   }
   mutex_unlock(&dis_lk);
   if (mdis < 0 && pref < 0)
      return;
   if (mdis >= 0)
      meter_set_dis(mdis, val); /* the meter sanitises its own */
   else
      /* Stores AND persists. A failure here is not worth a banner -- these
       * are device-information strings the sensor will report again on the
       * next connection -- but it must not be reported as recorded. */
      if (info_set(pref, val) != SETTINGS_OK)
         LOGW("devinfo %s NOT saved", uuid);
   LOGI("devinfo %s = %s", uuid, val);
   shell_repaint();
}

/* Ask the link for its device-information strings again (see linkinfo.h).
 *
 * Read the sensor's serial / firmware / software strings. Deferred to
 * here (after the first reading) so it runs post-auth, when the reads
 * succeed. The sensor closes the cycle within a few seconds, often before
 * all three reads land, so we retry each reconnect until we have them all
 * -- throttled to at most once a minute, and stopping entirely once
 * complete. */
/* Gate on THIS LINK's strings, not the process-global ones. Keying on the
 * globals meant that once any sensor had filled them -- and they persist
 * to disk -- DIS was never re-read for any sensor again, so every later
 * sensor was minted carrying the first one's model and firmware. */
/* THE THROTTLE IS A DEADLINE, so it is claimed from the driver on the
 * monotonic clock (driver_dis_claim). Claimed LAST, after the "do we still
 * need these strings" test: a claim consumes the interval whether or not
 * the caller goes on to use it, so testing it first would burn the window
 * on a link that already has everything. */
void linkinfo_refresh_dis(int link, int have_mfr)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   /* THE PER-LINK COPIES ARE READ UNDER THEIR OWN LOCK, like every other
    * reader of them: the writer is a binder thread filling them byte by
    * byte, and "is this empty" asked without the lock is answered `no` the
    * instant byte 0 lands. */
   mutex_lock(&dis_lk);
   int want = !g_model_l[link][0] || !g_fw_l[link][0];
   mutex_unlock(&dis_lk);
   if ((want || !have_mfr) && driver_dis_claim(link))
      dexble_request_devinfo_link(link);
}

/* COPIED UNDER dis_lk -- the same lock pancra_devinfo writes them under, on a
 * binder thread. Testing and snapshotting them unlocked made that writer's
 * lock inert: the emptiness test passes as soon as the writer lands byte 0,
 * so a firmware of "1.6.0.11" could be read as "1" and minted that way, into
 * a file that is never rewritten.
 *
 * There is no "_locked" variant to call instead: this takes its own lock, and
 * a caller holding the registry lock around a run of these is exactly the
 * caller that would hold it across driver calls too. */
void linkinfo_dis(int link, char *model, int mcap, char *fw, int fcap)
{
   mutex_lock(&dis_lk);
   if (model && mcap > 0)
      model[0] = 0;
   if (fw && fcap > 0)
      fw[0] = 0;
   if (link < 0 || link >= LINK_MAX)
      goto out;
   if (model)
      str_snapshot(model, mcap, g_model_l[link]);
   if (fw)
      str_snapshot(fw, fcap, g_fw_l[link]);
out:
   mutex_unlock(&dis_lk);
}

void linkinfo_forget_dis(int link)
{
   if (link < 0 || link >= LINK_MAX)
      return;
   mutex_lock(&dis_lk);
   g_model_l[link][0] = 0;
   g_fw_l[link][0]    = 0;
   mutex_unlock(&dis_lk);
}
