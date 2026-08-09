// SPDX-License-Identifier: GPL-3.0
// sensors.c --- Permanent sensor registry: provenance + per-sensor preferences
// Copyright 2026 Jakob Kastelic

/* See sensors.h for why provenance and preferences live in separate files.
 * Both are plain CSV parsed by hand: this build is freestanding, so there is no
 * sscanf, and the parsers here stop at the first field they cannot read (the
 * same forgiving style as settings.c, which is what lets the schema grow). */
#include "sensors.h"
#include "dexlibc.h"
#include "util.h"
#include <stdio.h>  /* snprintf, SEEK_SET / SEEK_END */
#include <string.h> /* strcmp */

struct sensor_rec g_srec[MAX_SENSOR_RECS];
int g_nsrec;
struct sensor_slot g_slot[MAX_SLOTS];
int g_nslot;
char g_sensors_path[256], g_slots_path[256];

static const char *const type_names[SENSOR_NTYPES] = {"--", "STELO", "G7",
                                                      "ONETOUCH"};

/* ---- registry lock ----
 *
 * The registry is mutated from more than one thread: sensor_reconcile() and
 * every UI action run on the main thread, while ot_drv_done() and the DIS
 * callbacks re-mint on a binder thread. Two concurrent sensor_mint() calls
 * both scan for maxid before either appends, so both return the SAME id -- and
 * the whole design rests on an id naming exactly one physical device forever.
 * The overlapping append also interleaves two half-written provenance rows.
 *
 * Recursive, because the mutators call each other (claim -> primary -> save).
 * Same shape as driver_lock(); duplicated rather than shared because sensors.c
 * must stay free of any BLE dependency to build in the host UI harness. */
static volatile int reg_owner; /* gettid() of the holder, 0 when free */
static int reg_depth;

static void reg_lock(void);
static void reg_unlock(void);

/* Public handles on the registry lock, for callers that must hold it across a
 * multi-step read of g_slot (e.g. resolving a link to a sensor id). Take it
 * AFTER driver_lock and BEFORE hist_lock -- reg is a leaf in that order. */
void sensors_lock(void)
{
   reg_lock();
}

void sensors_unlock(void)
{
   reg_unlock();
}

static void reg_lock(void)
{
   int me = gettid();
   if (__atomic_load_n(&reg_owner, __ATOMIC_SEQ_CST) == me) {
      reg_depth++;
      return;
   }
   while (!__sync_bool_compare_and_swap(&reg_owner, 0, me))
      sched_yield();
   reg_depth = 1;
}

static void reg_unlock(void)
{
   if (--reg_depth > 0)
      return;
   __atomic_store_n(&reg_owner, 0, __ATOMIC_SEQ_CST);
}

int sensor_kind(int type)
{
   return type == SENSOR_ONETOUCH ? KIND_BGM : KIND_CGM;
}

long sensor_session_len(int type)
{
   if (type == SENSOR_STELO)
      return 15L * 86400; /* Stelo: 15 days */
   if (type == SENSOR_G7)
      return 10L * 86400; /* G7: 10 days (plus a 12 h grace period) */
   return 0;
}

long sensor_wear_seconds(int type, int wear_days, const char *model)
{
   /* The user's explicit override wins outright. */
   if (wear_days == 10 || wear_days == 15)
      return wear_days * 86400L;
   /* Dexcom sells the G7 in 10-day and 15-day versions and the sensor never
    * states its wear length in any field we parse -- only the DIS model
    * distinguishes them. Judging a 15-day G7 against the 10-day default
    * declared it ENDED five days early, while it was visibly still
    * delivering. Models learned in the field; extend as they appear. */
   if (model && !strcmp(model, "SW14758"))
      return 15L * 86400; /* G7 15 Day */
   return sensor_session_len(type);
}

const char *sensor_type_name(int type)
{
   if (type <= SENSOR_NONE || type >= SENSOR_NTYPES)
      return "--";
   return type_names[type];
}

/* ---- tiny CSV field readers (freestanding: no sscanf) ---- */

/* Read a decimal integer, honouring a leading '-'. Stops at the first byte
 * that is not a digit and leaves *p there. */
static long rdnum(char **p, const char *e)
{
   long v = 0;
   int nd = 0; /* digit cap: unbounded accumulation is UB, and it happens
                * during parsing, before the id > 0 guard can reject it. A
                * row dropped that way is exactly what makes maxid go
                * backwards and the next mint reuse a LIVE id. */
   int neg = 0;
   char *q = *p;
   if (q < e && *q == '-') {
      neg = 1;
      q++;
   }
   while (q < e && *q >= '0' && *q <= '9') {
      if (nd < 18) {
         v = (v * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   *p = q;
   return neg ? -v : v;
}

/* Copy up to the next ',' (or end of line) into dst, always NUL-terminated. */
static void rdstr(char **p, const char *e, char *dst, int n)
{
   char *q = *p;
   int k   = 0;
   while (q < e && *q != ',') {
      if (k < n - 1)
         dst[k++] = *q;
      q++;
   }
   dst[k] = 0;
   *p     = q;
}

/* Step over one field separator, if we are sitting on one. */
static void rdsep(char **p, const char *e)
{
   if (*p < e && **p == ',')
      (*p)++;
}

/* Append a provenance row to the in-memory cache, evicting the oldest row that
 * NO live slot references.
 *
 * Evicting purely by age was wrong: a meter mints once and never again, while
 * CGM sessions mint constantly, so the meter's own row aged out while the meter
 * was still in daily use -- and once it was gone the meter's readings were
 * logged as source 0, indistinguishable from pre-registry data. Provenance for
 * a sensor you still own must never be the thing that gets dropped. */
static void srec_push(const struct sensor_rec *r)
{
   /* Last row wins per id: sensor_complete() appends a corrected row for an
    * id that already has one, and on load the correction must supersede the
    * original -- in place, so one id never occupies two cache rows. Minted
    * ids are unique (maxid + 1), so this changes nothing for them. */
   for (int i = 0; i < g_nsrec; i++)
      if (g_srec[i].id == r->id) {
         g_srec[i] = *r;
         return;
      }
   if (g_nsrec < MAX_SENSOR_RECS) {
      g_srec[g_nsrec++] = *r;
      return;
   }
   int victim = -1;
   for (int i = 0; i < g_nsrec && victim < 0; i++) {
      int referenced = 0;
      for (int k = 0; k < g_nslot; k++)
         if (g_slot[k].id == g_srec[i].id)
            referenced = 1;
      if (!referenced)
         victim = i;
   }
   if (victim < 0)
      victim = 0; /* every row is pinned: drop the oldest anyway */
   for (int i = victim + 1; i < g_nsrec; i++)
      g_srec[i - 1] = g_srec[i];
   g_srec[g_nsrec - 1] = *r;
}

/* ---- lookups ---- */

const struct sensor_rec *sensor_rec_by_id(int id)
{
   for (int i = 0; i < g_nsrec; i++)
      if (g_srec[i].id == id)
         return &g_srec[i];
   return 0;
}

/* See sensors.h for why activation is the anchor and why this fails open. */
int sensor_in_warmup(int id, long t)
{
   int warm = 0;
   sensors_lock();
   const struct sensor_rec *r = sensor_rec_by_id(id);
   if (r && r->activation > 0 && t >= r->activation &&
       t < r->activation + SENSOR_WARMUP_S)
      warm = 1;
   sensors_unlock();
   return warm;
}

struct sensor_slot *sensor_slot_by_id(int id)
{
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].id == id)
         return &g_slot[i];
   return 0;
}

int sensor_slot_by_mac(const char *identity)
{
   for (int i = 0; i < g_nslot; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && !strcmp(r->identity, identity))
         return i;
   }
   return -1;
}

int sensor_hidden_ids(int *out, int max)
{
   /* The ids of slots whose marker is MARK_HIDE (the device is OFF the plot),
    * in one locked pass so a caller can flag hidden points without taking the
    * registry lock per point. There are at most MAX_SLOTS of them. */
   reg_lock();
   int n = 0;
   for (int i = 0; i < g_nslot && n < max; i++)
      if (g_slot[i].marker == MARK_HIDE)
         out[n++] = g_slot[i].id;
   reg_unlock();
   return n;
}

int sensor_primary_id(void)
{
   /* The primary's ID, resolved under the registry lock.
    *
    * hist_refresh_current() needs this while holding hist_lock. Reading g_slot
    * there directly was unsynchronized: sensor_forget_slot's shift-down can
    * move the primary flag between the scan and the id load, yielding a
    * DIFFERENT sensor's id and binding the big number (and therefore the
    * alarm) to the wrong sensor. Taking reg_lock inside hist_lock would invert
    * the lock order instead, so callers resolve the id HERE first and pass it
    * in -- reg is a leaf, and reg-before-hist keeps the graph acyclic. */
   int id = -1;
   reg_lock();
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary) {
         id = g_slot[i].id;
         break;
      }
   reg_unlock();
   return id;
}

int sensor_primary_slot(void)
{
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary)
         return i;
   return -1;
}

void sensor_set_primary(int idx)
{
   reg_lock();
   const struct sensor_rec *r =
       (idx >= 0 && idx < g_nslot) ? sensor_rec_by_id(g_slot[idx].id) : 0;
   /* A BGM must never own the big number: a hours-old fingerstick rendered as
    * the headline value (with a trend arrow) would actively mislead. An OLD
    * (disconnected) device cannot be primary either -- it is not streaming. */
   if (r && sensor_kind(r->type) == KIND_CGM && !g_slot[idx].old) {
      for (int i = 0; i < g_nslot; i++)
         g_slot[i].primary = (i == idx);
      slots_save();
   }
   reg_unlock();
}

int sensor_live_cgm_count(void)
{
   int n = 0;
   reg_lock();
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].old)
         continue;
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (r && sensor_kind(r->type) == KIND_CGM)
         n++;
   }
   reg_unlock();
   return n;
}

/* Hand the primary to the first LIVE CGM, if the current one is gone/old. */
static void reassign_primary_locked(void)
{
   int have = 0;
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].primary && !g_slot[i].old) {
         have = 1;
         break;
      }
   if (have)
      return;
   for (int i = 0; i < g_nslot; i++)
      g_slot[i].primary = 0;
   for (int i = 0; i < g_nslot; i++) {
      const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
      if (!g_slot[i].old && r && sensor_kind(r->type) == KIND_CGM) {
         g_slot[i].primary = 1;
         break;
      }
   }
}

void sensor_retire_slot(int idx)
{
   reg_lock();
   if (idx < 0 || idx >= g_nslot) {
      reg_unlock();
      return;
   }
   g_slot[idx].old     = 1;
   g_slot[idx].primary = 0;
   reassign_primary_locked();
   slots_save();
   reg_unlock();
}

void sensor_revive_slot(int idx)
{
   reg_lock();
   if (idx < 0 || idx >= g_nslot) {
      reg_unlock();
      return;
   }
   g_slot[idx].old = 0;
   /* If nothing else is primary and this is a CGM, it takes the big number. */
   const struct sensor_rec *r = sensor_rec_by_id(g_slot[idx].id);
   if (r && sensor_kind(r->type) == KIND_CGM && sensor_primary_slot() < 0)
      g_slot[idx].primary = 1;
   slots_save();
   reg_unlock();
}

/* ---- load / save ---- */

/* Column header for sensors.csv, so an exported registry is self-describing.
 * A leading-'#' line parses to id 0 and is dropped by srec_parse_line. */
static const char g_sensors_hdr[] =
    "# id,type,mac,serial,model,fw,activation_time,paired_time\n";

/* Provenance rows are appended in id order, so reading only the tail still
 * yields the highest id -- which is all minting needs. */
static void srec_parse_line(char *p, char *e)
{
   struct sensor_rec r = {0};
   char *q             = p;
   r.id                = (int)rdnum(&q, e);
   rdsep(&q, e);
   r.type = (int)rdnum(&q, e);
   rdsep(&q, e);
   rdstr(&q, e, r.identity, (int)sizeof r.identity);
   rdsep(&q, e);
   rdstr(&q, e, r.serial, (int)sizeof r.serial);
   rdsep(&q, e);
   rdstr(&q, e, r.model, (int)sizeof r.model);
   rdsep(&q, e);
   rdstr(&q, e, r.fw, (int)sizeof r.fw);
   rdsep(&q, e);
   r.activation = rdnum(&q, e);
   rdsep(&q, e);
   r.paired = rdnum(&q, e);
   if (r.id > 0)
      srec_push(&r);
}

/* Stream the WHOLE file, one line at a time.
 *
 * This used to read only the last 8 KB, which quietly defeated srec_push's
 * pinning: pinning protects rows a live slot references from eviction, but a
 * row outside the window was never loaded at all, so there was nothing to pin.
 * A provenance row is ~70 bytes, so past ~115 rows the meter's row -- minted
 * once, early -- simply vanished. sensor_reconcile then never recovered
 * g_meter_src and the meter SILENTLY STOPPED AUTO-SYNCING, permanently, which
 * is exactly the failure the pinning was added to prevent.
 *
 * Streaming is affordable: the file grows about one row per sensor session
 * (~1.7 KB/year), so even decades of use is a single sub-100 KB scan at
 * startup, and only MAX_SENSOR_RECS rows are ever held in memory. */
static void srec_load(void)
{
   g_nsrec = 0;
   int fd  = open(g_sensors_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   char buf[1024];
   char line[256];
   int llen = 0;
   int over = 0; /* this line exceeded the buffer: skip it rather than truncate,
                  * since a truncated row parses as a DIFFERENT sensor */
   long n = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (!over)
               srec_parse_line(line, line + llen);
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0 && !over) /* final line with no trailing newline */
      srec_parse_line(line, line + llen);
   close(fd);
}

static void slots_load(void)
{
   g_nslot = 0;
   int fd  = open(g_slots_path, O_RDONLY, 0);
   if (fd < 0)
      return;
   static char buf[1025];
   long n = read(fd, buf, 1024);
   close(fd);
   if (n <= 0)
      return;
   buf[n]  = 0;
   char *p = buf;
   while (*p && g_nslot < MAX_SLOTS) {
      char *e = p;
      while (*e && *e != '\n')
         e++;
      struct sensor_slot s = {0};
      char *q              = p;
      s.id                 = (int)rdnum(&q, e);
      rdsep(&q, e);
      rdstr(&q, e, s.label, (int)sizeof s.label);
      rdsep(&q, e);
      s.marker = (int)rdnum(&q, e);
      rdsep(&q, e);
      s.color = (int)rdnum(&q, e);
      rdsep(&q, e);
      s.primary = (int)rdnum(&q, e) ? 1 : 0;
      rdsep(&q, e);
      s.size = (int)rdnum(&q, e); /* 6th field; absent in pre-size files -> 0 */
      rdsep(&q, e);
      /* 7th field; absent in older files -> 0 = resolve by model/type. */
      s.wear_days = (int)rdnum(&q, e);
      rdsep(&q, e);
      /* 8th field; absent in older files -> 0 = live (not an old device). */
      s.old = (int)rdnum(&q, e) ? 1 : 0;
      if (s.id > 0) {
         if (s.marker < 0 || s.marker >= MARK_N)
            s.marker = MARK_SQUARE_F;
         if (s.marker == MARK_DOT) /* DOT dropped -> its identical twin */
            s.marker = MARK_SQUARE_F;
         if (s.color < 0 || s.color > 6)
            s.color = 0;
         if (s.size < 1 || s.size > MARK_SIZE_MAX)
            s.size = MARK_SIZE_DEF; /* default / migrate old files */
         if (s.wear_days != 10 && s.wear_days != 15)
            s.wear_days = 0; /* anything else means "not overridden" */
         if (s.old)
            s.primary = 0; /* an old device can never be the primary */
         g_slot[g_nslot++] = s;
      }
      p = (*e == '\n') ? e + 1 : e;
   }
   /* At most one primary can survive a hand-edited file. */
   int seen = 0;
   for (int i = 0; i < g_nslot; i++) {
      if (g_slot[i].primary && seen)
         g_slot[i].primary = 0;
      if (g_slot[i].primary)
         seen = 1;
   }
}

void sensors_load(void)
{
   /* Slots FIRST. srec_load() evicts provenance rows and protects the ones a
    * live slot references -- but it scans g_slot, so loading it second meant
    * g_nslot was 0 throughout and nothing was ever pinned. The protection
    * silently did nothing. */
   reg_lock();
   slots_load();
   srec_load();
   reg_unlock();
}

/* ---- old-device marker store ---- */

void slots_save(void)
{
   /* Build the WHOLE file, then rename it into place.
    *
    * This used to truncate the live file and then write one line per slot, so
    * anything that stopped the process mid-loop -- an OOM kill, a crash, the
    * battery -- left a registry holding some of the sensors and not the rest.
    * These are not "preferences": a lost slot is a G7 the user has to pair
    * again, key and all. The temp-then-rename here is the same shape
    * insulin.c already uses for its own rewrite; rename is atomic, so the
    * file on disk is always either the old registry or the new one. */
   reg_lock();
   char tmp[sizeof g_slots_path + 4];
   int tn = snprintf(tmp, sizeof tmp, "%s.t", g_slots_path);
   if (tn <= 0 || tn >= (int)sizeof tmp) {
      reg_unlock();
      return;
   }
   char all[(MAX_SLOTS * 96) + 1];
   int used = 0;
   for (int i = 0; i < g_nslot && used < (int)sizeof all; i++) {
      int n =
          snprintf(all + used, sizeof all - (size_t)used,
                   "%d,%s,%d,%d,%d,%d,%d,%d\n", g_slot[i].id, g_slot[i].label,
                   g_slot[i].marker, g_slot[i].color, g_slot[i].primary,
                   g_slot[i].size, g_slot[i].wear_days, g_slot[i].old);
      if (n <= 0 || n >= (int)sizeof all - used)
         break; /* cannot describe the rest: keep the file we already have */
      used += n;
   }
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      reg_unlock();
      return;
   }
   int ok = (used == 0) || (write(fd, all, used) == used);
   close(fd);
   if (!ok || rename(tmp, g_slots_path) != 0)
      (void)unlink(tmp);
   reg_unlock();
}

/* ---- minting ---- */

/* Append one provenance row durably. 0 on success, -1 on failure -- and on a
 * short write the partial line is rolled back: left in place it would merge
 * with the next append into one unparseable row, hiding an id from the parser,
 * after which maxid goes backwards and the NEXT mint reissues a live id. */
static int srec_append_row(const struct sensor_rec *r)
{
   int fd = open(g_sensors_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
   if (fd < 0)
      return -1;
   if (lseek(fd, 0, SEEK_END) == 0) { /* self-describing header on a new file */
      if (write(fd, g_sensors_hdr, sizeof g_sensors_hdr - 1) <
          0) { /* best eff */
      }
   }
   char b[192];
   int n  = snprintf(b, sizeof b, "%d,%d,%s,%s,%s,%s,%ld,%ld\n", r->id, r->type,
                     r->identity, r->serial, r->model, r->fw, r->activation,
                     r->paired);
   n      = clampn(n, sizeof b);
   long w = write(fd, b, n);
   if (w != n) {
      if (w > 0)
         (void)ftruncate(fd, lseek(fd, 0, SEEK_END) - w);
      close(fd);
      return -1;
   }
   close(fd);
   return 0;
}

int sensor_mint(int type, const char *identity, const char *serial,
                const char *model, const char *fw, long activation)
{
   if (!identity || !identity[0])
      return -1;
   const char *se = serial ? serial : "";
   const char *mo = model ? model : "";
   const char *fv = fw ? fw : "";
   /* Scan, append and cache are ONE atomic step. Split apart, two threads both
    * read the same maxid and hand out the same id to two different physical
    * sensors -- which readings.csv then cites forever, with no way to tell the
    * two apart after the fact. */
   reg_lock();
   /* A physical device is identified by (type, identity/MAC) ALONE. serial,
    * model, fw and activation are LEARNED ATTRIBUTES, not identity.
    *
    * The reuse key used to also include (serial, model, fw), so the instant a
    * device's DIS was read -- which happens a few seconds AFTER its first
    * reading -- the bare mint (empty model/fw) and the with-model mint got
    * DIFFERENT ids. Everything logged under the bare id was then orphaned:
    * cited by an id no slot pointed at, drawn on the plot as gray crosses, its
    * history split off from the device the user still holds. Keying on MAC
    * alone makes an id map to one PHYSICAL device for life. A CGM session
    * always brings a new MAC, so sessions still separate cleanly; a meter keeps
    * one MAC, so all its fingersticks group under one id, forever. */
   int slotidx = sensor_slot_by_mac(identity);
   if (slotidx >= 0) {
      /* Already tracked by a live slot: ALL of this device's readings belong to
       * that slot's id, no matter what model/fw we now report. This is the pin
       * that makes the split above impossible. */
      int id = g_slot[slotidx].id;
      reg_unlock();
      return id;
   }
   int maxid = 0;
   for (int i = 0; i < g_nsrec; i++) {
      const struct sensor_rec *r = &g_srec[i];
      if (r->id > maxid)
         maxid = r->id;
      /* No slot yet, but a provenance row for this (type, MAC) already exists
       * (e.g. from an earlier launch): reuse its id rather than minting
       * another. activation/model/fw differences do NOT fork the id anymore. */
      if (r->type == type && !strcmp(r->identity, identity)) {
         int id = r->id;
         reg_unlock();
         return id;
      }
   }

   /* An id must fit the 16-bit `src` field of struct reading (see store.h).
    * Past 65535 the narrowing cast wraps and id 65536 aliases legacy id 0, so
    * readings would be silently reattributed to a different physical device --
    * the one failure this whole design exists to make impossible. Refusing to
    * mint stops new data rather than corrupting the record: the sensor shows
    * as unregistered, which is visible, instead of quietly borrowing another
    * device's identity. Unreachable in practice (a few mints per sensor per
    * year), but it is an invariant, not an estimate. */
   if (maxid + 1 > 0xFFFF) {
      reg_unlock();
      return -1;
   }

   struct sensor_rec r = {0};
   r.id                = maxid + 1;
   r.type              = type;
   r.activation        = activation;
   r.paired            = realtime_s();
   str_snapshot(r.identity, sizeof r.identity, identity);
   str_snapshot(r.serial, sizeof r.serial, se);
   str_snapshot(r.model, sizeof r.model, mo);
   str_snapshot(r.fw, sizeof r.fw, fv);

   if (srec_append_row(&r) < 0) {
      reg_unlock();
      return -1; /* provenance MUST be durable: refuse the id if it did not
                    reach the disk, or readings would cite a row nobody has */
   }

   srec_push(&r);
   reg_unlock();
   return r.id;
}

int sensor_complete(int id, const char *serial, const char *model,
                    const char *fw, long activation)
{
   reg_lock();
   /* Locate the row by INDEX and work on a copy: srec_push memmoves the array
    * when full, and the durable append must precede the in-memory update so a
    * failed write leaves the row still-incomplete and the caller retries. */
   int idx = -1;
   for (int i = 0; i < g_nsrec && idx < 0; i++)
      if (g_srec[i].id == id)
         idx = i;
   if (idx < 0) {
      reg_unlock();
      return 0; /* aged out of the cache: nothing to complete against */
   }
   struct sensor_rec r = g_srec[idx];
   int changed         = 0;
   if (!r.serial[0] && serial && serial[0]) {
      str_snapshot(r.serial, sizeof r.serial, serial);
      changed = 1;
   }
   if (!r.model[0] && model && model[0]) {
      str_snapshot(r.model, sizeof r.model, model);
      changed = 1;
   }
   if (!r.fw[0] && fw && fw[0]) {
      str_snapshot(r.fw, sizeof r.fw, fw);
      changed = 1;
   }
   if (!r.activation && activation) {
      r.activation = activation;
      changed      = 1;
   }
   if (!changed) {
      reg_unlock();
      return 0;
   }
   if (srec_append_row(&r) < 0) {
      reg_unlock();
      return -1;
   }
   g_srec[idx] = r;
   reg_unlock();
   return 1;
}

int sensor_claim_slot(int id, int type, const char *identity)
{
   if (id <= 0)
      return -1;
   reg_lock();
   struct sensor_slot *have = sensor_slot_by_id(id);
   if (have) {
      int idx = (int)(have - g_slot);
      /* Re-adding a device that was DISCONNECTED revives its existing slot --
       * keeping the marker/label/colour the user chose -- instead of leaving
       * it stranded as an old device with a duplicate live one. */
      if (have->old) {
         have->old = 0;
         if (sensor_kind(type) == KIND_CGM && sensor_primary_slot() < 0)
            have->primary = 1;
         slots_save();
      }
      reg_unlock();
      return idx;
   }
   if (g_nslot >= MAX_SLOTS) {
      reg_unlock();
      return -1;
   }
   struct sensor_slot s = {0};
   s.id                 = id;
   s.marker             = MARK_SQUARE_F; /* DOT dropped; identical to this */
   s.size               = MARK_SIZE_DEF;
   /* The primary (first) device defaults to WHITE (index 6) -- the classic
    * main-trace colour; additional devices get a distinct colour each so they
    * are told apart at a glance. */
   s.color = (g_nslot == 0) ? 6 : ((g_nslot - 1) % 6);
   /* Default label is type + the last two MAC octets, so a freshly paired
    * sensor is never nameless and two meters are told apart on sight. */
   int n = 0;
   while (identity && identity[n])
      n++;
   const char *tail = (n >= 5) ? identity + n - 5 : "";
   (void)snprintf(s.label, sizeof s.label, "%s%s%s", sensor_type_name(type),
                  tail[0] ? "-" : "", tail[0] ? tail : "");
   for (int i = 0; s.label[i]; i++)
      if (s.label[i] == ':')
         s.label[i] = '-';
   /* First CGM paired becomes primary, so the big number always has an owner.
    */
   if (sensor_kind(type) == KIND_CGM && sensor_primary_slot() < 0)
      s.primary = 1;
   g_slot[g_nslot++] = s;
   slots_save();
   int idx = g_nslot - 1;
   reg_unlock();
   return idx;
}

int sensor_rebind_slot(int old_id, int new_id)
{
   /* Repoint a slot at a newly minted provenance id as ONE atomic step.
    * Callers used to do this by hand: take a raw `struct sensor_slot *` from
    * sensor_slot_by_id() and assign through it. That pointer is an index into
    * an array sensor_forget_slot() shifts down, so a concurrent forget on the
    * main thread moved a DIFFERENT sensor under the pointer and the write
    * landed on that sensor's slot -- then slots_save() persisted it. The
    * victim's readings, label and link routing then resolved through the
    * wrong provenance row, permanently. Resolving the index under the lock we
    * mutate under is the only way this is safe. */
   int ok = 0;
   reg_lock();
   for (int i = 0; i < g_nslot; i++)
      if (g_slot[i].id == old_id) {
         g_slot[i].id = new_id;
         ok           = 1;
         break;
      }
   if (ok)
      slots_save();
   reg_unlock();
   return ok;
}

void sensor_forget_slot(int idx)
{
   reg_lock();
   if (idx < 0 || idx >= g_nslot) {
      reg_unlock();
      return;
   }
   /* NOTE: the DISCONNECT flow uses sensor_retire_slot (which keeps the slot
    * and its appearance); this hard-delete remains only for a true removal
    * and for the test suite. */
   int was_primary = g_slot[idx].primary;
   for (int i = idx + 1; i < g_nslot; i++)
      g_slot[i - 1] = g_slot[i];
   g_nslot--;
   /* Never leave the big number ownerless: hand it to the first CGM left. */
   if (was_primary)
      for (int i = 0; i < g_nslot; i++) {
         const struct sensor_rec *r = sensor_rec_by_id(g_slot[i].id);
         if (r && sensor_kind(r->type) == KIND_CGM) {
            g_slot[i].primary = 1;
            break;
         }
      }
   slots_save();
   reg_unlock();
}
