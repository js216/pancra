// SPDX-License-Identifier: GPL-3.0
// sesscache.c --- the last-known session clock, per sensor (see sesscache.h)
// Copyright 2026 Jakob Kastelic
#include "sesscache.h"
#include "dexdriver.h" /* struct dex_session: what is cached and restored */
#include "dexlibc.h"
#include "loadresult.h" /* what a load actually found */
#include "senslogic.h"  /* sens_cache_*: the rate limit on the write */
#include "sensors.h"    /* MAX_SLOTS, SENSOR_ACTIVE_S */
#include "thread.h"     /* sessc_lk / sessfile_lk: see the block below */
#include "util.h"
#if __STDC_HOSTED__
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>

/* ---- LAST-KNOWN SESSION, per sensor, across restarts -------------------
 *
 * The session clock, the state byte and the prediction all live in the DRIVER,
 * which is per-process state built from 0x4e responses. The last reading, its
 * trend and its age survive a restart because they are replayed from
 * readings.csv -- so after every launch the main screen showed a glucose value
 * and its age while PRED and the session countdown sat blank for up to a full
 * five-minute cadence, waiting for the sensor to answer. Two of the four
 * numbers on one line disappearing, repeatedly, for no reason the user can
 * see.
 *
 * So cache them. The clock is stored WITH the wall time it was read at, and
 * projected forward on load exactly as driver_get_session projects it between
 * responses -- storing the raw number would restore a countdown frozen at
 * whenever the app last ran, which is worse than blank because it looks live.
 *
 * The live driver ALWAYS wins: this is consulted only while have_reading is
 * still 0 for that link, i.e. before its first response of this process. */
struct sess_cache {
   long clock_t; /* wall clock when `clock` was read */
   long clock;   /* session seconds at that instant */
   int id, state, predicted, sequence;
};
static struct sess_cache g_sessc[MAX_SLOTS];
static int g_nsessc;
static char g_sess_path[256];
/* NOT "when the newest change happened" -- that never fires.
 *
 * driver_get_session projects the clock forward by wall time, so it reads a
 * DIFFERENT value every second and sessc_put marks a change every second. A
 * "quiet for 60 s" rule is therefore never satisfied while a sensor is
 * connected, and session.cache was never written at all -- the restore this
 * exists for silently did nothing on every launch. Rate-limit against the last
 * SAVE instead, which is what was meant. */
static struct sens_cache g_sessc_state;

/* ---- TWO LOCKS, AND WHY THE TABLE'S IS NOT THE FILE'S -----------------
 *
 * WHAT THIS FILE LOOKED LIKE BEFORE, AND WHAT IT COST.
 *
 * Every one of the five objects above was a plain global, and three threads
 * reach them:
 *
 *   MAIN     draws. build_model() calls sessc_put() for a link that has a
 *            live session and sessc_restore() for one that has not, once per
 *            row per frame. So the MAIN thread mutates the table, appends to
 *            it (g_nsessc++), and sets the dirty flag -- from the RENDER
 *            path, several times a second.
 *   MAIN     also flushes: on_timer -> sensor_reconcile -> sess_flush.
 *   SERVICE  the foreground service's "pancra-tick" HandlerThread, which
 *            outlives the activity: shell_service_tick ->
 *            pancra_reconcile_tick -> sensor_reconcile -> sess_flush. It
 *            READS the whole table to render the file and it WRITES the
 *            save-rate state.
 *
 * sensor_reconcile's single flight keeps the two flushers from overlapping
 * each other, but it does nothing at all about the render path -- and that is
 * the pairing that matters. The service tick walked g_sessc[] with snprintf
 * while the main thread was assigning into the same rows and incrementing
 * g_nsessc, with no lock and no snapshot between them. Two consequences, both
 * of which the user meets after a restart rather than when they happen:
 *
 *   A TORN FILE. A row rendered half from the session before a 0x4e response
 *   and half from the session after it -- a clock_t from one instant beside a
 *   clock from another -- restores a countdown that never existed. The
 *   projection on load then counts on from it, so the WARM-UP that has
 *   finished still reads as warming up, or a session that has hours left
 *   reads as expired. Both are numbers the user acts on.
 *
 *   MARKED SAVED WITHOUT BEING WRITTEN. sess_flush cleared the dirty flag on
 *   the strength of a render taken before a change that landed during the
 *   write. Nothing was left to say the change had not been persisted, so it
 *   waited for the next change -- and if the sensor then went out of range,
 *   for ever.
 *
 * SO: ONE LOCK OVER THE TABLE AND THE SCHEDULE TOGETHER, and a SECOND one
 * over the file.
 *
 *   sessc_lk    the table, its count, the generation and the save-rate state.
 *               A LEAF, and it is what the RENDER takes -- so it is never
 *               held across anything slow, and above all never across
 *               atomic_replace's two fsyncs and a rename. mutex_lock is a
 *               yield-spin with no timeout (thread.h), and a main thread
 *               spinning on flash is the ANR this app has been killed for.
 *   sessfile_lk the file. Taken with sessc_lk RELEASED and held across the
 *               write, so two flushers cannot interleave their renames --
 *               they share one "session.cache.tmp" -- and so an OLDER render
 *               cannot land on top of a newer one.
 *
 * Never nested in either direction; a save renders under sessc_lk, releases
 * it, and only then takes sessfile_lk. That is set_file_lk's shape exactly,
 * and app/settings.c's save_now/write_job split is the worked example this
 * follows, generation reconciliation included. See the rank table in
 * app/thread.h and app/test/lockorder.py, which check the pair. */
#ifdef APP_FAULTS
void (*sess_fault_gap_here)(void);
#endif

static struct mutex sessc_lk    = MUTEX_INIT;
static struct mutex sessfile_lk = MUTEX_INIT;

/* THE GENERATION, which is what makes "marked saved" honest.
 *
 * g_sessc_gen counts CHANGES to the table and is guarded by sessc_lk; every
 * render stamps its job with the value it saw. g_sessc_written is the newest
 * generation the FILE holds and is guarded by sessfile_lk. Two rules fall out
 * of them, and each answers one of the two failures above:
 *
 *   A job that is not NEWER than what is on disk is not written. Two flushers
 *   racing can otherwise land an older render after a newer one, which is a
 *   restart reading a session state that was already superseded.
 *
 *   The cache is marked saved only if the generation has not moved since the
 *   render. If it has, a change landed during the write and this job does not
 *   contain it -- so the cache stays dirty and the next tick writes it.
 *
 * Starts at 1 against a written of 0 so that the very first save, of a table
 * that has only been loaded from disk, still writes. */
static unsigned g_sessc_gen = 1;
static unsigned g_sessc_written;

/* One render, ready to be written with nothing held. Same shape as
 * settings.c's struct save_job, for the same reason. */
struct sess_job {
   char buf[(MAX_SLOTS * 96) + 1];
   int len;
   unsigned gen;
   int ok; /* the render itself fitted */
};

/* The record, or NULL. CALLER HOLDS sessc_lk. */
static struct sess_cache *sessc_get(int id, int create)
{
   for (int i = 0; i < g_nsessc; i++)
      if (g_sessc[i].id == id)
         return &g_sessc[i];
   if (create && g_nsessc < MAX_SLOTS) {
      struct sess_cache *c = &g_sessc[g_nsessc++];
      *c                   = (struct sess_cache){0};
      c->id                = id;
      return c;
   }
   return 0;
}

/* THE COHERENT SNAPSHOT. CALLER HOLDS sessc_lk: every row is rendered from
 * the table as it stood at ONE instant, and the generation stamped on the job
 * is the generation of that instant. */
static void sess_render(struct sess_job *j)
{
   j->len = 0;
   j->ok  = 1;
   j->gen = g_sessc_gen;
   for (int i = 0; i < g_nsessc; i++) {
      const struct sess_cache *r = &g_sessc[i];
      if (r->id <= 0 || r->clock_t <= 0)
         continue;
      /* THE ROW IS READ OUT FIELD BY FIELD, then formatted, rather than being
       * six subexpressions of one snprintf. Identical under the lock; what it
       * buys is a place to put the yield below. */
      int rid      = r->id;
      long rclockt = r->clock_t;
      long rclock  = r->clock;
#ifdef APP_FAULTS
      /* WIDEN THE WINDOW, in the fault-injection build only.
       *
       * A row's six fields are read within a few instructions of each other,
       * so a render that does NOT hold sessc_lk is wrong for that handful of
       * instructions and right the rest of the time. A test cannot land
       * inside it: with no help, a build with the lock deleted here wrote a
       * perfectly coherent file on every run, and the torn-row property
       * passed against the implementation it exists to reject. Yielding
       * BETWEEN the halves of a row is what makes the interleaving ordinary
       * -- an earlier attempt yielded between whole ROWS and changed nothing,
       * because the tear was never between rows.
       *
       * The same yield runs in the correct build, under the lock, which is
       * the point: only the unlocked variant can have a put land in it.
       * settings.c's write_job carries the same device for the same reason.
       *
       * A HOOK RATHER THAN A BARE YIELD, so the suite can install it around
       * the section that needs it and take it away again: this loop is also
       * reached from paths that render continuously, and yielding under
       * sessc_lk there starves the very writers the gap exists to let in.
       * app_fault_gap_here in util.h is the same device. Nothing that ships
       * defines APP_FAULTS. */
      if (sess_fault_gap_here)
         sess_fault_gap_here();
#endif
      int rstate = r->state;
      int rpred  = r->predicted;
      int rseq   = r->sequence;
      int bn = snprintf(j->buf + j->len, sizeof j->buf - (size_t)j->len,
                        "%d,%ld,%ld,%d,%d,%d\n", rid, rclockt, rclock, rstate,
                        rpred, rseq);
      if (bn <= 0 || bn >= (int)sizeof j->buf - j->len) {
         j->ok = 0;
         break;
      }
      j->len += bn;
   }
}

/* THE WRITE, and then the reconciliation. CALLER HOLDS NOTHING.
 *
 * `mark` asks for the save-rate state to be reconciled against this job --
 * only sess_flush wants that, because only sess_flush is the scheduled write.
 * 0 when the file holds this job's state or something newer. */
static int sess_write(const struct sess_job *j, long now, int mark)
{
   if (!j->ok)
      return -1;
   int bad = 0;
   mutex_lock(&sessfile_lk);
   /* Signed difference, so the comparison survives a wrap. */
   if ((int)(j->gen - g_sessc_written) > 0) {
      /* REPLACE_UNSYNCED IS NOT A FAILURE. The rename happened, so the file
       * already holds these bytes; only the directory entry's survival of a
       * power cut is unknown. Treated as failure, the cache would stay dirty
       * and rewrite an already-correct file on every tick for ever. */
      bad = atomic_replace(g_sess_path, j->buf, j->len) == REPLACE_FAILED;
      if (!bad)
         g_sessc_written = j->gen;
   }
   mutex_unlock(&sessfile_lk);
   if (!bad && mark) {
      mutex_lock(&sessc_lk);
      /* THE RECONCILIATION. Marked saved only for the state that was
       * actually written: a sessc_put that landed while the file was being
       * replaced moved the generation, and clearing the flag on its behalf
       * is how a session state came to be dropped with nothing saying so. */
      if (g_sessc_gen == j->gen)
         sens_cache_done(&g_sessc_state, now);
      mutex_unlock(&sessc_lk);
   }
   return bad ? -1 : 0;
}

int sess_save(void)
{
   struct sess_job j;
   mutex_lock(&sessc_lk);
   sess_render(&j);
   mutex_unlock(&sessc_lk);
   /* No `now`, and no mark: this is the unconditional "write it now" entry.
    * WHEN the cache is due is sess_flush's business, and clearing the dirty
    * flag from here would silently cancel a scheduled write. */
   return sess_write(&j, 0, 0);
}

/* The session cache's own filename. */
int sess_paths(const char *dir)
{
   int ok = 1;
   mutex_lock(&sessfile_lk);
   ok &= data_path(g_sess_path, sizeof g_sess_path, dir, "/session.cache");
   /* A NEW FILE HAS NOTHING IN IT, whatever the old one held. Without this
    * the generation gate would compare the next render against what the
    * PREVIOUS path already contained and skip the write, leaving the new
    * file absent. Nothing on the phone repoints this after startup; a test
    * that runs several fixtures in one process does, and a silently skipped
    * write is precisely the defect this module was just fixed for. */
   g_sessc_written = 0;
   mutex_unlock(&sessfile_lk);
   mutex_lock(&sessc_lk);
   g_sessc_gen++;
   mutex_unlock(&sessc_lk);
   return ok;
}

enum load_result sess_load(void)
{
   int fd = open(g_sess_path, O_RDONLY, 0);
   if (fd < 0) {
      /* A first run has no file, and that is not a failure. Anything else is
       * a file that is there and cannot be opened. */
      if (errno == ENOENT)
         return LOAD_ABSENT;
      return LOAD_ERROR;
   }
   char b[1024];
   int n = (int)read(fd, b, (sizeof b) - 1);
   close(fd);
   if (n < 0)
      return LOAD_ERROR;
   if (n == 0)
      return LOAD_CORRUPT; /* created and not written: a torn save */
   b[n]    = 0;
   char *p = b;
   while (*p) {
      long v[6] = {0, 0, 0, 0, 0, 0};
      int vi    = 0;
      int any   = 0;
      while (*p && *p != '\n') {
         if (*p >= '0' && *p <= '9') {
            /* Digit-capped, like every other loader here: an unbounded
             * accumulation is UB and a wrapped value would restore a
             * nonsense clock that the projection below then counts on. */
            if (v[vi] < 100000000000000000L)
               v[vi] = (v[vi] * 10) + (*p - '0');
            any = 1;
         } else if (*p == ',' && vi < 5) {
            vi++;
         }
         p++;
      }
      if (*p == '\n')
         p++;
      if (any && v[0] > 0) {
         /* UNDER THE LOCK EVEN HERE. This runs once at startup on the MAIN
          * thread before the service tick exists, so nothing races it today
          * -- but "every touch of the table is under sessc_lk" is a rule a
          * reader can check, and "except this one, because of where it is
          * called from" is an invariant that lives in another module's call
          * graph. The lock is taken per row rather than around the parse so
          * that the early returns above stay returns. */
         mutex_lock(&sessc_lk);
         struct sess_cache *c = sessc_get((int)v[0], 1);
         if (c) {
            c->clock_t   = v[1];
            c->clock     = v[2];
            c->state     = (int)v[3];
            c->predicted = (int)v[4];
            c->sequence  = (int)v[5];
         }
         mutex_unlock(&sessc_lk);
      }
   }
   return LOAD_OK;
}

/* Record a LIVE session for `id`. Cheap enough to call every frame, but only
 * marks the file dirty when the clock actually moved -- the flush is on the
 * 1 Hz tick, so a redraw storm cannot turn this into a write storm. */
void sessc_put(int id, const struct dex_session *s, long now)
{
   if (id <= 0 || !s->have_reading)
      return;
   mutex_lock(&sessc_lk);
   struct sess_cache *c = sessc_get(id, 1);
   /* ONE CONDITION AND ONE EXIT, rather than the three early returns this
    * used to be: a `return` from inside the lock walks away holding a
    * yield-spin with no timeout, and app/test/lockorder.py refuses one. */
   if (c && !(c->clock == (long)s->session_seconds && c->state == s->state &&
              c->predicted == s->predicted)) {
      c->clock_t   = now;
      c->clock     = (long)s->session_seconds;
      c->state     = s->state;
      c->predicted = s->predicted;
      c->sequence  = s->sequence;
      /* THE CHANGE AND ITS GENERATION IN THE SAME CRITICAL SECTION. A
       * renderer that saw the new row but the old generation would stamp its
       * job as containing a change it does not, and then mark the cache
       * saved for it. */
      g_sessc_gen++;
      sens_cache_touch(&g_sessc_state);
   }
   mutex_unlock(&sessc_lk);
}

/* Fill `out` from the cache for `id`, projecting the clock forward to `now`.
 * Returns 1 if a usable cached session was restored.
 *
 * REFUSED once the cache is older than SENSOR_ACTIVE_S: past that the sensor
 * has been silent for a day and a countdown derived from it would be fiction
 * presented as a live reading. Refused for a non-positive clock too -- a
 * session that never started has nothing to project. */
int sessc_restore(int id, long now, struct dex_session *out)
{
   /* A COPY TAKEN UNDER THE LOCK, and the decisions made outside it. The
    * five fields have to come from ONE instant: read one at a time they can
    * pair a clock_t from before a 0x4e response with the clock from after
    * it, and the projection below then adds a real elapsed time to a stale
    * base -- a countdown that looks live and never existed. */
   struct sess_cache c;
   mutex_lock(&sessc_lk);
   const struct sess_cache *r = sessc_get(id, 0);
   if (r)
      c = *r;
   else
      c = (struct sess_cache){0};
   mutex_unlock(&sessc_lk);
   if (c.clock_t <= 0 || c.clock <= 0)
      return 0;
   long dt = now - c.clock_t;
   if (dt < 0 || dt > SENSOR_ACTIVE_S)
      return 0;
   out->have_reading    = 1;
   out->session_seconds = (unsigned)(c.clock + dt);
   out->state           = c.state;
   out->predicted       = c.predicted;
   out->sequence        = c.sequence;
   /* NOT bonded: that is a live-link fact and nothing here can vouch for it.
    * The connected/WAITING status is driven by it, and claiming a bond we do
    * not have would paint a dead sensor green. */
   return 1;
}

/* Write the cache at most once a minute: sessc_put marks it dirty from the
 * DRAW path, which runs far more often than the 5-minute cadence that
 * actually changes anything. Losing up to a minute costs nothing -- the
 * stored clock is projected forward from whatever instant it holds. */
void sess_flush(long now)
{
   /* THE DECISION AND THE SNAPSHOT IN ONE CRITICAL SECTION, the write outside
    * it, and the flag reconciled against the generation afterwards. This used
    * to be three unlocked steps -- ask whether it is due, walk the table, and
    * clear the flag -- with a change from the render path able to land
    * between any two of them. */
   struct sess_job j;
   j.ok  = 0; /* nothing rendered: sess_write does nothing with it */
   j.len = 0;
   j.gen = 0;
   mutex_lock(&sessc_lk);
   if (sens_cache_due(&g_sessc_state, now))
      sess_render(&j);
   mutex_unlock(&sessc_lk);
   (void)sess_write(&j, now, 1);
}
