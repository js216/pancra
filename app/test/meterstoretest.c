// SPDX-License-Identifier: GPL-3.0
// meterstoretest.c --- Host tests for the per-meter runtime store
// Copyright 2026 Jakob Kastelic
//
/* WHAT THIS FILE PROTECTS, and why it is worth a test of its own.
 *
 * meter.idx holds "how far into this meter's memory have we read". The
 * OneTouch protocol can only be asked for records AFTER a number, so if that
 * number is lost or attributed to the wrong meter, the next sync re-reads the
 * meter's whole memory -- and those records are typically weeks old, i.e.
 * outside the in-memory dedup window, so every one of them is appended to the
 * lifetime log a SECOND time and double-counted in the statistics. If instead
 * the number is too HIGH, records the meter really did take are skipped, and
 * the skip is then persisted, which makes the loss permanent.
 *
 * Both of those shipped. meterstore.c's comments describe them at length; not
 * one of them was checked by anything. The file is linked into other test
 * binaries and never called from them.
 *
 * Built and run by `make meterstoretest`.
 */
#include "meterstore.h"
#include "dexdriver.h" /* struct dex_session: what the session cache holds */
#include "sensors.h"   /* MAX_SLOTS, and the live slots eviction consults */
#include "sesscache.h" /* the OTHER persisted table this file now pins */
#include "testdir.h"   /* test_path: the per-mode fixture directory */
#include <pthread.h>   /* the overlap below is real threads */
#include <sched.h>     /* sched_yield: the handshakes below spin */
#include <stdatomic.h> /* the fixture races on purpose; the table does not */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* THE FIXTURE DIRECTORY IS READ AT RUN TIME, not pasted in as a literal, so
 * that the ASan and TSan builds of this suite (it is in both lists) keep their
 * files in their own tree instead of all three racing in build/app/test -- see
 * app/test/testdir.h. Variables rather than the three #defines that were here:
 * a macro cannot be a run-time value, and IDX and SYNCF were built from DIR by
 * string pasting. Filled by paths_init() before the first use. */
static char DIR[128];

static char IDX[192];

static char SYNCF[192];

static void paths_init(void)
{
   test_path(DIR, sizeof DIR, "mstore");
   (void)snprintf(IDX, sizeof IDX, "%s/meter.idx", DIR);
   (void)snprintf(SYNCF, sizeof SYNCF, "%s/metersync.csv", DIR);
}

/* Both files gone, and the store pointed at them. The rt table itself is
 * static and cannot be emptied -- which the tests below take into account by
 * using fresh ids and leaving the capacity case for last. */
static void fresh_files(void)
{
   mkdir(DIR, 0755);
   unlink(IDX);
   unlink(SYNCF);
   meter_store_paths(IDX, SYNCF);
}

static void put(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   fwrite(text, 1, strlen(text), f);
   fclose(f);
}

static int line_count(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   int n = 0;
   int c = 0;
   while ((c = fgetc(f)) != EOF)
      if (c == '\n')
         n++;
   fclose(f);
   return n;
}

/* ---- THE OVERLAP THE PHONE ACTUALLY HAS -------------------------------
 *
 * Binder callbacks write a meter's phase text, its signal and its timestamps
 * while the 1 Hz watchdog reads them to decide whether to re-arm and the
 * renderer reads them to draw a device row -- and the save walks the whole
 * table to persist it. Every one of those was an unlocked read or write of a
 * shared array, and `stat` is a 24-byte string: a row could be drawn from
 * half of one phase text and half of the next.
 *
 * A single-threaded test cannot tell this implementation from that one. This
 * one runs the three roles at once and asserts the property that fails
 * without the lock: every read is of a WHOLE record.
 */
#define OVER_ID     4242
#define OVER_ROUNDS 20000

/* The phase texts a writer may install. Distinct lengths and no common
 * prefix, so a spliced read cannot accidentally equal one of them. */
static const char *const OVER_STAT[] = {"HELLO", "COUNT", "READING",
                                        "NOTHING NEW", "SYNCED"};
#define OVER_NSTAT ((int)(sizeof OVER_STAT / sizeof OVER_STAT[0]))

static atomic_int over_stop;
static atomic_int over_torn; /* reads that were not one whole record */
static atomic_int over_seen; /* reads that found something to check */

static void *over_write(void *arg)
{
   long k = (long)arg;
   for (int i = 0; i < OVER_ROUNDS; i++) {
      /* The three writes a binder thread really makes. Each carries a
       * matching (rssi, rssi_t) pair and a phase text from the list. */
      meter_rt_stat(OVER_ID, OVER_STAT[(i + k) % OVER_NSTAT], 1700000000 + i);
      meter_rt_rssi(OVER_ID, -40 - (int)((i + k) % 60), 1700000000 + i,
                    1700000000 + i);
      meter_rt_advert(OVER_ID, 1700000000 + i, i, -50, 1, 1700000000 + i);
   }
   atomic_fetch_add(&over_stop, 1);
   return 0;
}

static void *over_read(void *arg)
{
   (void)arg;
   /* ...OR UNTIL IT HAS ACTUALLY READ SOMETHING. On a loaded machine the two
    * writers can both finish before a reader is ever scheduled, and the whole
    * section then passes having observed nothing at all -- which is the one
    * way a concurrency test can be worse than no test. `over_seen` is
    * asserted below for that reason, and it went red on a busy box, so the
    * loop is now guaranteed to take at least one look. It terminates: by the
    * time over_stop is 2 both writers have created the record. */
   while (atomic_load(&over_stop) < 2 || !atomic_load(&over_seen)) {
      struct meter_rt r;
      if (!meter_rt_read(OVER_ID, &r))
         continue;
      atomic_store(&over_seen, 1);
      /* THE PHASE TEXT IS ONE OF THEM, WHOLE. A splice -- the first half of
       * "NOTHING NEW" and the tail of "SYNCED" -- matches none. */
      if (r.stat[0]) {
         int known = 0;
         for (int j = 0; j < OVER_NSTAT; j++)
            if (strcmp(r.stat, OVER_STAT[j]) == 0)
               known = 1;
         if (!known)
            atomic_fetch_add(&over_torn, 1);
      }
      /* ...and a signal that is reported as known is a plausible one, which
       * is the store's own rule. */
      if (r.rssi_ok && (r.rssi > -1 || r.rssi < -127))
         atomic_fetch_add(&over_torn, 1);
      if (r.id != OVER_ID)
         atomic_fetch_add(&over_torn, 1);
   }
   return 0;
}

/* The fourth role: the save, which walks every record and writes a file. It
 * must not see a half-written one either. */
static void *over_save(void *arg)
{
   (void)arg;
   while (atomic_load(&over_stop) < 2)
      (void)meter_sync_save();
   return 0;
}

static void overlap(void)
{
   fresh_files();
   atomic_store(&over_stop, 0);
   atomic_store(&over_torn, 0);
   atomic_store(&over_seen, 0);
   /* NOLINTNEXTLINE(misc-include-cleaner) -- see threadtest.c on pthread_t */
   pthread_t th[5];
   pthread_create(&th[0], 0, over_write, (void *)0L);
   pthread_create(&th[1], 0, over_write, (void *)3L);
   pthread_create(&th[2], 0, over_read, 0);
   pthread_create(&th[3], 0, over_read, 0);
   pthread_create(&th[4], 0, over_save, 0);
   for (int i = 0; i < 5; i++)
      pthread_join(th[i], 0);
   ck(atomic_load(&over_torn) == 0,
      "every read of a record is of ONE whole record");
   ck(atomic_load(&over_seen) == 1, "...and the readers really did read");
   /* The file the saver wrote is still parseable and names this meter. */
   struct meter_rt after;
   ck(meter_rt_read(OVER_ID, &after) && after.sync_t > 0,
      "the record survives the run");
   ck(meter_sync_save() == 0, "...and one more save still works");
}

/* ---- ITEM 78: A NEWER TIME MUST NOT BE LOST BEHIND AN OLDER SAVE -------
 *
 * WHAT THE DEFECT LOOKED LIKE. meter_sync_save() rendered the whole table
 * into a buffer under mrt_lk and only THEN tried to enter a single flight. A
 * caller that found a writer already running returned 0 -- success -- on the
 * argument that the running save had already written what this caller wanted
 * written. It had not: its buffer was rendered BEFORE this caller's mutation,
 * so the newer time was in nobody's buffer and nothing ever came back for it.
 * On the phone that is two meters woken together, or an advert landing while
 * another meter's connection RSSI is being persisted -- two BLE binder
 * threads, milliseconds apart. LAST SEEN updates on screen and is not there
 * after a restart.
 *
 * WHY THIS TEST IS SHAPED THE WAY IT IS. The surviving value is always a
 * PLAUSIBLE one -- a real time this meter really was seen at, just the wrong
 * one -- so "the file parses and the numbers are in range" passes against the
 * broken implementation every time. bondtabletest learned the same lesson the
 * hard way. Two things make the loss decidable here:
 *
 *   ONE THREAD PER METER ID, so each id's stored time only ever increases.
 *   "The file went backwards" is then a fact, not a probability.
 *
 *   THE ASSERTION IS AT THE CALL, not at the end of the run. The contract the
 *   fix establishes is that when meter_sync_save() returns 0 the bytes on
 *   disk are at least as new as the table was when the call began -- so every
 *   single successful save is checked against the file it claims to have
 *   written. A single lost race fails the run.
 *
 *   THE SIGNAL IS DERIVED FROM THE TIME (rssi = -1 - i%100), so a row that
 *   pairs one round's time with another round's signal is caught too. That is
 *   the other half of the save: the render has to be one coherent instant.
 */
#define LOST_NTHREAD 4
#define LOST_ROUNDS  60
#define LOST_T0      1750000000L

/* THE FOUR SAVERS' IDS, then the churn meter's.
 *
 * The first four are ids earlier sections already created. The runtime table
 * is MAX_SLOTS long and static for the whole process, and the CEILING section
 * at the end asserts what a full table does -- so a section that consumed
 * four more seats would break it from a distance. The fifth is the one new
 * record here, and there is room for exactly it.
 *
 * WHY A FIFTH METER THAT NEVER SAVES. The four savers spend nearly all their
 * time waiting their turn to write, so at the moment one of them is rendering
 * the table the other three are usually parked rather than mutating -- and a
 * render that reads a row's fields without the lock is only wrong while
 * somebody is writing that row. On the phone that thread exists: adverts and
 * connection RSSI arrive on binder threads whether or not a save is running.
 * Here it is this one, and it is what makes the incoherent-render mutant fail
 * an assertion instead of only tripping ThreadSanitizer. */
#define LOST_NID    (LOST_NTHREAD + 1)
#define LOST_CHURNI LOST_NTHREAD
static const int LOST_ID[LOST_NID] = {4001, 4002, 4101, 4102, 4103};

static atomic_int lost_stale;   /* a save reported success, and the file held
                                 * an OLDER time than the caller had stored */
static atomic_int lost_missing; /* ...or no row for that meter at all */
static atomic_int lost_torn;    /* a row pairing one round's time with
                                 * another round's signal */
static atomic_int lost_failed;  /* the write itself reported failure */
static atomic_int lost_saves;   /* saves that reported success */
static atomic_int lost_back;    /* the file went BACKWARDS for a meter */
static atomic_int lost_watched; /* rows the watcher actually compared */
static atomic_int lost_stop;    /* the writers are all finished */

/* THE GAP THE TWO SAVE PATHS HOLD OPEN FOR US. See meter_fault_gap_here in
 * meterstore.h: the windows these sections are about are a handful of
 * instructions wide, and with no help a build with either lock deleted wrote
 * a perfectly coherent, perfectly ordered file on every single run. Installed
 * around the section that needs it and taken away again, because yielding
 * inside a render also starves the writers in the OTHER concurrency section
 * below -- which turned a 15 s suite into one that had not finished in five
 * minutes. */
static void widen_window(void)
{
   sched_yield();
}

static int lost_rssi(int round)
{
   return -1 - (round % 100); /* always a plausible signal: -1 .. -100 */
}

/* This meter's row in the file, or 0 when it has none. The file is published
 * by rename, so a reader sees the whole old file or the whole new one. */
static int lost_row(int id, long *t, int *rssi)
{
   FILE *f = fopen(SYNCF, "rb");
   if (!f)
      return 0;
   char line[128];
   int got = 0;
   while (!got && fgets(line, sizeof line, f)) {
      int rid = 0;
      long rt = 0;
      int rr  = 0;
      int rok = 0;
      if (sscanf(line, "%d,%ld,%d,%d", &rid, &rt, &rr, &rok) == 4 &&
          rid == id) {
         *t    = rt;
         *rssi = rr;
         got   = 1;
      }
   }
   fclose(f);
   return got;
}

static void *lost_writer(void *arg)
{
   int k  = (int)(long)arg;
   int id = LOST_ID[k];
   for (int i = 1; i <= LOST_ROUNDS; i++) {
      long t = LOST_T0 + i;
      if (!meter_rt_advert(id, t, 0, lost_rssi(i), 1, t))
         continue; /* table full: nothing was stored, so nothing is owed */
      if (meter_sync_save() != 0) {
         atomic_fetch_add(&lost_failed, 1);
         continue;
      }
      atomic_fetch_add(&lost_saves, 1);
      /* THE CONTRACT, checked against the file itself. */
      long ft = 0;
      int fr  = 0;
      if (!lost_row(id, &ft, &fr)) {
         atomic_fetch_add(&lost_missing, 1);
         continue;
      }
      if (ft < t)
         atomic_fetch_add(&lost_stale, 1);
      /* Whatever round the stored time belongs to, the stored signal must be
       * THAT round's. Only for rows THIS section wrote: the runtime table is
       * static for the whole process, so the first save also writes rows left
       * by earlier sections, whose signals were never derived from anything.
       */
      if (ft >= LOST_T0 && fr != lost_rssi((int)(ft - LOST_T0)))
         atomic_fetch_add(&lost_torn, 1);
   }
   atomic_fetch_add(&lost_stop, 1);
   return 0;
}

/* A BINDER THREAD THAT ONLY WRITES. It never saves, so it is never parked
 * waiting for a rename -- which is what the four savers spend most of their
 * time doing. Its meter's stored time and signal are derived from one counter
 * exactly as theirs are, so a render that reads its row without the table's
 * lock produces a pair that belongs to no round at all.
 *
 * It stops when the savers do, so nothing outlives the section. */
static void *lost_churn(void *arg)
{
   (void)arg;
   long i = 0;
   while (atomic_load(&lost_stop) < LOST_NTHREAD) {
      i++;
      long t = LOST_T0 + i;
      (void)meter_rt_advert(LOST_ID[LOST_CHURNI], t, 0, lost_rssi((int)i), 1,
                            t);
   }
   return 0;
}

/* THE RESTART, over and over, and the assertion that does not depend on
 * winning a race against an fsync.
 *
 * The per-call check above is the contract, but it is read microseconds after
 * the save returns -- and a save that rendered BEFORE it was serialised is
 * usually still inside its own rename at that instant, so the stale bytes
 * have not landed yet. That made the check a coin toss for exactly the
 * variant that renders outside the guard. This one cannot miss it: every
 * meter's stored time only ever increases, so a stored time that DECREASES is
 * an older render that was written over a newer one, and it stays on disk
 * until the next write replaces it. */
static void *lost_watcher(void *arg)
{
   (void)arg;
   long seen[LOST_NID] = {0};
   while (atomic_load(&lost_stop) < LOST_NTHREAD) {
      for (int k = 0; k < LOST_NID; k++) {
         long ft = 0;
         int fr  = 0;
         /* Rows this section has not yet overwritten carry times from earlier
          * ones, which no rule here describes. */
         if (!lost_row(LOST_ID[k], &ft, &fr) || ft < LOST_T0)
            continue;
         atomic_fetch_add(&lost_watched, 1);
         if (fr != lost_rssi((int)(ft - LOST_T0)))
            atomic_fetch_add(&lost_torn, 1);
         if (ft < seen[k])
            atomic_fetch_add(&lost_back, 1);
         else
            seen[k] = ft;
      }
   }
   return 0;
}

static void lost_update(void)
{
   fresh_files();
   meter_fault_gap_here = widen_window;
   atomic_store(&lost_stale, 0);
   atomic_store(&lost_missing, 0);
   atomic_store(&lost_torn, 0);
   atomic_store(&lost_failed, 0);
   atomic_store(&lost_saves, 0);
   atomic_store(&lost_back, 0);
   atomic_store(&lost_watched, 0);
   atomic_store(&lost_stop, 0);
   /* NOLINTNEXTLINE(misc-include-cleaner) -- see threadtest.c on pthread_t */
   pthread_t th[LOST_NTHREAD + 2];
   for (long i = 0; i < LOST_NTHREAD; i++)
      pthread_create(&th[i], 0, lost_writer, (void *)i);
   pthread_create(&th[LOST_NTHREAD], 0, lost_watcher, 0);
   pthread_create(&th[LOST_NTHREAD + 1], 0, lost_churn, 0);
   for (int i = 0; i < LOST_NTHREAD + 2; i++)
      pthread_join(th[i], 0);

   meter_fault_gap_here = 0;

   ck(atomic_load(&lost_saves) > 0, "the race really did save something");
   ck(atomic_load(&lost_watched) > 0, "...while the file was being read");
   ck(atomic_load(&lost_failed) == 0, "...and no write reported failure");
   ck(atomic_load(&lost_back) == 0,
      "a meter's stored last-sync time never goes BACKWARDS -- a render "
      "taken before another caller's mutation is never written over it");
   ck(atomic_load(&lost_missing) == 0,
      "a save that reported success left the meter IN the file");
   ck(atomic_load(&lost_stale) == 0,
      "...holding that meter's new last-sync time, never an older one");
   ck(atomic_load(&lost_torn) == 0,
      "...beside the signal captured at that same time");

   /* And when it is all over, every meter's LAST value is the one on disk --
    * the state the user would see after a restart. */
   int settled = 0;
   for (int k = 0; k < LOST_NTHREAD; k++) {
      long ft = 0;
      int fr  = 0;
      if (lost_row(LOST_ID[k], &ft, &fr) && ft == LOST_T0 + LOST_ROUNDS)
         settled++;
   }
   ck(settled == LOST_NTHREAD,
      "after the race every meter's newest time is the one a restart reads");
}

/* ---- ITEM 79: THE SESSION CACHE, MUTATED WHILE IT IS BEING FLUSHED -----
 *
 * WHAT IT IS. session.cache is what restores a sensor's session clock, its
 * state byte and its prediction across a restart; without it the countdown
 * and PRED under the big number sit blank for a whole five-minute cadence
 * after every launch. The table, its count and the save-rate state were plain
 * globals, and three paths reach them with no lock between:
 *
 *   MAIN     build_model() -> sessc_put()/sessc_restore(), once per sensor
 *            row per frame. The RENDER path mutates the table.
 *   MAIN     on_timer -> sensor_reconcile -> sess_flush.
 *   SERVICE  the "pancra-tick" HandlerThread, which outlives the activity:
 *            shell_service_tick -> pancra_reconcile_tick -> sensor_reconcile
 *            -> sess_flush. It walks the whole table with snprintf.
 *
 * TWO FAILURES, and this section drives both:
 *
 *   A TORN FILE -- a row rendered half from before a 0x4e response and half
 *   from after it. The restored countdown then looks live and never existed,
 *   so a warm-up that has finished still reads as warming up. Made decidable
 *   here by DERIVING every field from the clock: state, predicted and
 *   sequence are functions of it, so a row assembled from two instants is
 *   arithmetic, not a judgement call.
 *
 *   MARKED SAVED WITHOUT BEING WRITTEN -- the dirty flag cleared on the
 *   strength of a render taken before a change that landed during the write.
 *   Nothing then says the change is unsaved, and it waits for the next change
 *   that moves the clock; if the sensor goes out of range there is none.
 *
 * HOW THAT SECOND ONE IS CAUGHT, because it is the subtle one. The checker
 * thread is the service heartbeat and owns the only clock that ADVANCES, so
 * its flush is always past the one-minute rate limit whenever the cache is
 * dirty; the other flusher lags it deliberately. Each round the writers run
 * flat out while the checker flushes, then STOP and are waited for, and only
 * then does the checker read the newest value the writers published, flush
 * once more, and demand it be on disk. That last flush writes if and only if
 * the cache is honestly dirty -- so a spuriously cleared flag leaves the file
 * one value behind and the assertion names it. The quiescence is the whole
 * point: with the writers still running the flag is re-dirtied instantly and
 * the bug hides.
 *
 * AND A THIRD, which the first draft of this test could not see. Two threads
 * can each render and then race to the file, so an OLDER render can land on
 * top of a newer one -- the next launch then reads a session state that had
 * already been superseded. Nothing the checker asserts after everything has
 * stopped can see that, because by then the older write has been overwritten
 * again. So a WATCHER thread reads session.cache throughout, exactly as a
 * restart would, and asserts what the generation gate is for: the file never
 * goes BACKWARDS. That is a real property of the module, not a test artefact
 * -- every put moves a clock forward, so a stored clock that decreases is a
 * write that should have been refused.
 */
#define SESS_NWRITER 2
/* the two writers, the scheduled flusher and TWO unconditional savers */
#define SESS_NHELPER (SESS_NWRITER + 3)
#define SESS_ROUNDS  20
#define SESS_T0      1750000000L
/* Puts per writer per phase. The TEARING phase wants as many as it can get.
 * The PROBED phase wants only enough to move the generation -- WHEN they land
 * is what matters there, not how many. */
#define SESS_QUOTA_FAST 3000
#define SESS_QUOTA_SLOW 32
/* How long a writer will wait for a flush to reach its write, in yields. Big
 * enough that a loaded machine still gets there, bounded so a round in which
 * no write happens at all ends rather than hanging. */
#define SESS_PROBE_SPINS 400000

static const int SESS_ID[SESS_NWRITER] = {7001, 7002};
/* A third sensor, put only by the checker, whose sole job is to make sure
 * there is something to write when phase two needs a write to happen. */
#define SESS_SEED_ID 7003

/* The clock each writer has published, i.e. what it has already put in the
 * table and expects a clean cache to imply is on disk. */
static atomic_long sess_published[SESS_NWRITER];
static atomic_int sess_run;                /* flusher and saver may work */
static atomic_int sess_done;               /* ...and everyone may now exit */
static atomic_int sess_idle[SESS_NHELPER]; /* each helper's acknowledge */
static atomic_long sess_now = SESS_T0;     /* the checker's clock */
/* THE WRITERS RUN IN BOUNDED BURSTS, not free-running, and that is what makes
 * the dirty-flag rule decidable rather than lucky. A burst that outlives a
 * flush's render but ENDS BEFORE the flush marks the cache saved leaves
 * exactly one question on the table: was the flag cleared for a state the
 * write did not contain? Free-running writers re-dirty the flag instantly and
 * the answer is hidden. */
static atomic_int sess_epoch; /* bumped to release a burst */
static atomic_int sess_started[SESS_NWRITER];
static atomic_int sess_paced; /* this burst waits for a write to be under way */
static atomic_int sess_probed; /* bursts that really did land inside one */

static atomic_int sess_behind;    /* the file was OLDER than a published
                                   * value at a moment nothing was writing */
static atomic_int sess_backwards; /* ...or older than the file itself had
                                   * already been, which is an older render
                                   * landing on a newer one */
static atomic_int sess_torn;      /* a row assembled from two instants */
static atomic_int sess_absent;    /* no row for a sensor that has published */
static atomic_int sess_checks;
static atomic_int sess_watched; /* rows the watcher actually compared */

/* Every field derived from one number, so a row is either one instant's or
 * demonstrably nobody's. */
static int sess_state_of(long clock)
{
   return (int)(clock % 7);
}

static int sess_pred_of(long clock)
{
   return 100 + (int)(clock % 13);
}

static int sess_seq_of(long clock)
{
   return 1000 + (int)clock;
}

static char SESSF[192];
static char SESSTMP[200];

/* IS A WRITE UNDER WAY RIGHT NOW?
 *
 * atomic_replace stages the new contents as "<path>.tmp" and publishes them
 * by rename, so that name exists for exactly the interval between the render
 * and the rename. That interval is the one a put has to land in for the
 * dirty-flag rule to be decidable at all -- a put before the render is in the
 * file, and a put after the flush has marked the cache saved re-dirties it and
 * hides the answer.
 *
 * Waiting for this file rather than guessing with a delay is what turned a
 * coin toss into a kill. Two earlier drafts tuned a burst length instead: one
 * yielded between puts and, on a loaded machine, outlasted the whole write;
 * the other used a few hundred plain puts and still passed one run in three
 * against an implementation that clears the flag unconditionally. A flaky kill
 * is not a kill.
 *
 * It reaches into a naming detail of app/util.c, which is worth saying out
 * loud -- but it is the only externally visible sign that a write is in
 * progress, and the alternative is an assertion that cannot be trusted. */
static int sess_write_in_flight(void)
{
   return access(SESSTMP, F_OK) == 0;
}

/* One sensor's row from session.cache: 1 when found, and `bad` set when the
 * six fields cannot all have come from the same put. */
static int sess_row(int id, long *clock, int *bad)
{
   FILE *f = fopen(SESSF, "rb");
   if (!f)
      return 0;
   char line[160];
   int got = 0;
   while (!got && fgets(line, sizeof line, f)) {
      int rid = 0;
      long rt = 0;
      long rc = 0;
      int rs  = 0;
      int rp  = 0;
      int rq  = 0;
      if (sscanf(line, "%d,%ld,%ld,%d,%d,%d", &rid, &rt, &rc, &rs, &rp, &rq) ==
              6 &&
          rid == id) {
         *clock = rc;
         *bad   = rt != SESS_T0 || rs != sess_state_of(rc) ||
                  rp != sess_pred_of(rc) || rq != sess_seq_of(rc);
         got    = 1;
      }
   }
   fclose(f);
   return got;
}

/* THE RENDER PATH. build_model calls this from the main thread for every
 * sensor row that has a live session -- so the table is mutated, and grown,
 * from the thread that draws. */
static void *sess_writer(void *arg)
{
   int k       = (int)(long)arg;
   long clock  = 0;
   int myepoch = 0;
   while (!atomic_load(&sess_done)) {
      int e = atomic_load(&sess_epoch);
      if (e == myepoch) {
         atomic_store(&sess_idle[k], 1);
         sched_yield();
         continue;
      }
      myepoch   = e;
      int paced = atomic_load(&sess_paced);
      int quota = paced ? SESS_QUOTA_SLOW : SESS_QUOTA_FAST;
      atomic_store(&sess_idle[k], 0);
      atomic_store(&sess_started[k], 1);
      if (paced) {
         /* WAIT FOR THE WRITE, then put INTO it. See sess_write_in_flight:
          * this is what puts the mutation strictly after the render and
          * strictly before the flush decides whether to mark the cache
          * saved. A round in which no write happens (nothing was due) simply
          * does nothing, which is a round that proves nothing rather than a
          * round that fails. */
         long spins = 0;
         /* A TIGHT POLL, WITH NO YIELD IN IT. An earlier draft yielded here
          * and missed the window nearly every time: on a busy box a yielded
          * thread may not be scheduled again for milliseconds, which is the
          * whole duration of the write it is waiting for. access() is a
          * syscall in the microsecond range and the window is hundreds of
          * them, so spinning catches it and yielding does not. */
         while (!sess_write_in_flight() && spins++ < SESS_PROBE_SPINS &&
                !atomic_load(&sess_done))
            ;
         if (!sess_write_in_flight())
            quota = 0;
         else
            atomic_fetch_add(&sess_probed, 1);
      }
      for (int n = 0; n < quota && !atomic_load(&sess_done); n++) {
         struct dex_session s = {0};
         s.have_reading       = 1;
         clock++;
         s.session_seconds = (unsigned)clock;
         s.state           = sess_state_of(clock);
         s.predicted       = sess_pred_of(clock);
         s.sequence        = sess_seq_of(clock);
         sessc_put(SESS_ID[k], &s, SESS_T0);
         atomic_store(&sess_published[k], clock);
         /* The render path also RESTORES, for a link with no live session
          * yet. now == clock_t, so the projection adds nothing and the
          * restored fields must still be one instant's. */
         struct dex_session r = {0};
         if (sessc_restore(SESS_ID[k ^ 1], SESS_T0, &r)) {
            long rc = (long)r.session_seconds;
            if (r.state != sess_state_of(rc) ||
                r.predicted != sess_pred_of(rc) ||
                r.sequence != sess_seq_of(rc))
               atomic_fetch_add(&sess_torn, 1);
         }
      }
      atomic_store(&sess_idle[k], 1);
   }
   return 0;
}

/* THE ACTIVITY'S TIMER: the other flusher. It reads the clock rather than
 * advancing it, so it can never mark the cache saved at an instant beyond the
 * checker's -- which is what keeps the checker's own flush past the rate
 * limit whenever there is anything to write. */
static void *sess_flusher(void *arg)
{
   (void)arg;
   while (!atomic_load(&sess_done)) {
      if (!atomic_load(&sess_run)) {
         atomic_store(&sess_idle[SESS_NWRITER], 1);
         sched_yield();
         continue;
      }
      atomic_store(&sess_idle[SESS_NWRITER], 0);
      sess_flush(atomic_load(&sess_now));
   }
   return 0;
}

/* THE UNCONDITIONAL WRITE, and there are TWO of these threads on purpose.
 *
 * "Two renders in flight at once" is the whole hazard the generation gate
 * exists for: each renders the table at its own instant, then they queue for
 * the file, and without the gate the one that rendered FIRST can land LAST and
 * put a superseded session state back on disk. sess_flush is rate-limited by
 * design and the checker performs only one per round, so with a single extra
 * writer that overlap is rare -- the gate mutant survived four runs in five.
 * With two of them the overlap is continuous and it dies every time.
 *
 * sess_save() is the module's own "write it now" entry and takes no part in
 * the schedule, so these supply the overlap without disturbing the dirty flag
 * the checker is judging. */
static void *sess_saver(void *arg)
{
   int slot = (int)(long)arg;
   while (!atomic_load(&sess_done)) {
      if (!atomic_load(&sess_run)) {
         atomic_store(&sess_idle[slot], 1);
         sched_yield();
         continue;
      }
      atomic_store(&sess_idle[slot], 0);
      (void)sess_save();
   }
   return 0;
}

/* THE RESTART, over and over. Reads session.cache the way the next launch
 * will and holds it to two rules: every row is one instant's, and no sensor's
 * stored clock ever DECREASES. The second is what an older render landing on
 * top of a newer one looks like from outside, and it is invisible to any
 * check made after the threads have stopped. */
static void *sess_watcher(void *arg)
{
   (void)arg;
   long seen[SESS_NWRITER] = {0};
   while (!atomic_load(&sess_done)) {
      for (int k = 0; k < SESS_NWRITER; k++) {
         long clock = 0;
         int bad    = 0;
         if (!sess_row(SESS_ID[k], &clock, &bad))
            continue;
         atomic_fetch_add(&sess_watched, 1);
         if (bad)
            atomic_fetch_add(&sess_torn, 1);
         if (clock < seen[k]) {
            fprintf(stderr, "BACKWARDS id=%d seen=%ld now=%ld bad=%d\n",
                    SESS_ID[k], seen[k], clock, bad);
            atomic_fetch_add(&sess_backwards, 1);
         } else
            seen[k] = clock;
      }
   }
   return 0;
}

/* RELEASE A BURST OF PUTS, and do not come back until both writers have
 * really started one.
 *
 * The first draft of this test had no such wait, and a round could set the
 * flag, flush, and clear the flag again before any writer ever looked -- every
 * assertion below then passed against a run in which nothing raced. `the race
 * really ran` is what caught it, which is why that counter is asserted rather
 * than printed. Watching `idle` alone is not enough either: a whole burst can
 * finish before the checker looks, and then idle is 1 again and the wait never
 * ends. A per-burst `started` flag says "this burst happened", once. */
static void sess_burst(int paced)
{
   for (int i = 0; i < SESS_NWRITER; i++)
      atomic_store(&sess_started[i], 0);
   atomic_store(&sess_paced, paced);
   atomic_fetch_add(&sess_epoch, 1);
   for (int i = 0; i < SESS_NWRITER; i++)
      while (!atomic_load(&sess_started[i]))
         sched_yield();
}

static void sess_writers_done(void)
{
   for (int i = 0; i < SESS_NWRITER; i++)
      while (!atomic_load(&sess_idle[i]))
         sched_yield();
}

/* THE OTHER TWO WRITERS OF THE FILE stopped, each past the end of whatever
 * call it was inside -- a helper sets its flag only at the top of its loop.
 * That is what makes the check that follows a check of the DIRTY FLAG rather
 * than a race against a thread that might yet rescue the file. The watcher is
 * not in this list: it only reads. */
static void sess_helpers_stop(void)
{
   atomic_store(&sess_run, 0);
   for (int i = SESS_NWRITER; i < SESS_NHELPER; i++)
      while (!atomic_load(&sess_idle[i]))
         sched_yield();
}

static void sess_race(void)
{
   mkdir(DIR, 0755);
   (void)snprintf(SESSF, sizeof SESSF, "%s/session.cache", DIR);
   (void)snprintf(SESSTMP, sizeof SESSTMP, "%s.tmp", SESSF);
   unlink(SESSF);
   ck(sess_paths(DIR) == 1, "the session cache has somewhere to live");
   sess_fault_gap_here = widen_window;

   atomic_store(&sess_behind, 0);
   atomic_store(&sess_backwards, 0);
   atomic_store(&sess_torn, 0);
   atomic_store(&sess_absent, 0);
   atomic_store(&sess_checks, 0);
   atomic_store(&sess_watched, 0);
   atomic_store(&sess_probed, 0);
   atomic_store(&sess_done, 0);
   atomic_store(&sess_run, 0);
   for (int i = 0; i < SESS_NHELPER; i++)
      atomic_store(&sess_idle[i], 1);

   /* NOLINTNEXTLINE(misc-include-cleaner) -- see threadtest.c on pthread_t */
   pthread_t th[SESS_NHELPER + 1];
   for (long i = 0; i < SESS_NWRITER; i++)
      pthread_create(&th[i], 0, sess_writer, (void *)i);
   pthread_create(&th[SESS_NWRITER], 0, sess_flusher, 0);
   pthread_create(&th[SESS_NWRITER + 1], 0, sess_saver,
                  (void *)(long)(SESS_NWRITER + 1));
   pthread_create(&th[SESS_NWRITER + 2], 0, sess_saver,
                  (void *)(long)(SESS_NWRITER + 2));
   pthread_create(&th[SESS_NHELPER], 0, sess_watcher, 0);

   long seed = 0;
   for (int round = 0; round < SESS_ROUNDS; round++) {
      /* PHASE ONE -- EVERYTHING AT ONCE, for the two rules that are about the
       * FILE. Two writers mutating the table flat out, a scheduled flusher, an
       * unconditional saver and this checker's flush all in flight together;
       * the watcher reads session.cache throughout. What it is looking for is
       * a row assembled from two instants, and a stored clock that goes
       * backwards because an older render landed on a newer one. Neither is
       * visible from a check made after everything has stopped. */
      atomic_store(&sess_run, 1);
      sess_burst(0);
      sess_flush(atomic_fetch_add(&sess_now, 100) + 100);
      sess_writers_done();
      sess_helpers_stop();

      /* PHASE TWO -- THE DIRTY FLAG, ALONE. Nothing writes the file except
       * this thread, so nothing can rescue it, and the writers put only while
       * the flush below is inside its write. That leaves exactly one question:
       * was the flag cleared for a state the write did not contain?
       *
       * With free-running writers this rule is untestable: the flag is
       * re-dirtied within microseconds of being wrongly cleared, the next
       * flush writes, and the implementation that clears it unconditionally
       * looks identical to the one that reconciles a generation.
       *
       * The seed put is what guarantees there IS a write to land inside. The
       * cache may be perfectly clean at this point -- phase one's last write
       * may have captured everything -- and then the flush below would not be
       * due, no file would be staged, and the writers would wait for a write
       * that never came. A third sensor id, so it cannot disturb the two the
       * assertions are about. */
      seed++;
      {
         struct dex_session sd = {0};
         sd.have_reading       = 1;
         sd.session_seconds    = (unsigned)seed;
         sd.state              = sess_state_of(seed);
         sd.predicted          = sess_pred_of(seed);
         sd.sequence           = sess_seq_of(seed);
         sessc_put(SESS_SEED_ID, &sd, SESS_T0);
      }
      sess_burst(1);
      sess_flush(atomic_fetch_add(&sess_now, 100) + 100);
      sess_writers_done();
      /* One more flush, with a clock past every mark anyone can have made. If
       * the cache was honestly left dirty this writes; if it was marked saved
       * for a state it never wrote, this does nothing and the file is behind.
       */
      sess_flush(atomic_fetch_add(&sess_now, 100) + 100);
      for (int k = 0; k < SESS_NWRITER; k++) {
         long want = atomic_load(&sess_published[k]);
         if (want <= 0)
            continue;
         long have = 0;
         int bad   = 0;
         atomic_fetch_add(&sess_checks, 1);
         if (!sess_row(SESS_ID[k], &have, &bad))
            atomic_fetch_add(&sess_absent, 1);
         else {
            if (have < want)
               atomic_fetch_add(&sess_behind, 1);
            if (bad)
               atomic_fetch_add(&sess_torn, 1);
         }
      }
   }
   atomic_store(&sess_done, 1);
   atomic_store(&sess_run, 1); /* release anyone parked in the idle branch */
   for (int i = 0; i <= SESS_NHELPER; i++)
      pthread_join(th[i], 0);

   sess_fault_gap_here = 0;

   /* A CONCURRENCY TEST THAT OBSERVED NOTHING CANNOT FAIL, so both counters
    * of observations are asserted rather than printed. */
   ck(atomic_load(&sess_checks) > 0, "the session-cache race really ran");
   ck(atomic_load(&sess_watched) > 0, "...and the file was read while it did");
   ck(atomic_load(&sess_probed) > 0,
      "...and a session change really did land inside a write in progress");
   ck(atomic_load(&sess_absent) == 0,
      "a sensor whose session was cached has a row in the file");
   ck(atomic_load(&sess_torn) == 0,
      "every session row is ONE instant: clock, state, prediction and "
      "sequence agree");
   ck(atomic_load(&sess_backwards) == 0,
      "a stored session clock never goes BACKWARDS -- an older render is "
      "refused, not written over a newer one");
   ck(atomic_load(&sess_behind) == 0,
      "a quiet cache means the newest session state is really on disk -- "
      "never marked saved for a write that did not contain it");

   /* The file it left behind is loadable, which is what a restart does. */
   ck(sess_load() == LOAD_OK, "...and the file a restart reads parses whole");
}

int main(void)
{
   paths_init();
   fresh_files();

   printf("== a record index belongs to ONE meter ==\n");
   {
      /* THE DEFECT, in four lines. A single shared index made each sync look
       * like the other meter's counter had gone backwards ("memory
       * cleared"), so each reset, re-imported, and saved its own value --
       * leaving the pair oscillating for ever: one meter never reached its
       * own new records again, and the other re-appended records it already
       * had. */
      ck(meter_index_save(101, 4200) == 0, "one meter's index is stored");
      ck(meter_index_save(202, 17) == 0, "and so is another's");
      ck(meter_index_load(101) == 4200, "the first meter keeps ITS index");
      ck(meter_index_load(202) == 17, "the second keeps its own");
      ck(meter_index_save(101, 4300) == 0, "the first advances");
      ck(meter_index_load(202) == 17, "...without moving the second");
      ck(meter_index_load(101) == 4300, "...and its own value is the new one");

      /* -1, NOT 0. Index 0 is a real record, so "nothing stored" has to sit
       * below every valid index or a new meter's first record is skipped --
       * and a skip is persisted, which makes it permanent. */
      ck(meter_index_load(303) == -1, "a meter not in the file reads as "
                                      "UNSET, not as record zero");
      ck(meter_index_save(303, 0) == 0, "...and zero is storable");
      ck(meter_index_load(303) == 0, "...and reads back as zero, not unset");
   }

   printf("== the file survives what could be in it ==\n");
   {
      /* Ours to write, but a corrupt or hand-edited row must not take the
       * whole file down with it: everything after a bad row would otherwise
       * be lost, and losing a row means re-importing a meter's memory. */
      fresh_files();
      put(IDX, "101,7\nrubbish\n,\n0,99\n-5,3\n202,11\n");
      ck(meter_index_load(101) == 7, "a good row before the rubbish is read");
      ck(meter_index_load(202) == 11, "...and one after it too");
      ck(meter_index_load(0) == -1, "id 0 is not a meter");
      ck(meter_index_load(5) == -1, "a negative id is not adopted as "
                                    "positive");

      /* No trailing newline: the last row is still a row. A parser that only
       * commits on '\n' loses the most recently written index, which is the
       * one that matters. */
      fresh_files();
      put(IDX, "404,88");
      ck(meter_index_load(404) == 88, "a file with no trailing newline still "
                                      "has its last row");

      /* A digit run long enough to overflow: capped rather than wrapped, and
       * the row is still consumed so the rest of the file is read. */
      fresh_files();
      put(IDX, "9999999999999999999,1\n505,6\n");
      ck(meter_index_load(505) == 6, "a row with an absurd id does not eat "
                                     "the file");

      /* NEGATIVE indices round-trip: -1 is the protocol's own "walk from the
       * beginning", and storing it as 1 would skip the first record. */
      fresh_files();
      ck(meter_index_save(606, -1) == 0, "a negative index is stored");
      ck(meter_index_load(606) == -1, "...and read back negative");
   }

   printf("== an install from before the file was per-meter ==\n");
   {
      /* The old format was a bare integer, because there was only ever one
       * meter. Reading it as "id,index" yields nothing, the index looks
       * unset, and the meter re-imports its recent window -- weeks-old
       * records, outside the dedup window, appended twice. */
      fresh_files();
      put(IDX, "12345\n");
      ck(meter_index_load(777) == 12345, "a legacy bare index is adopted by "
                                         "whichever meter asks");
      /* And the adoption does not turn into an id: after a save the file is
       * in the new format and other meters read unset again. */
      ck(meter_index_save(777, 12346) == 0, "the next save rewrites it in the "
                                            "new format");
      ck(meter_index_load(888) == -1, "...after which a different meter is "
                                      "correctly unset");
      ck(meter_index_load(777) == 12346, "...and the adopting meter keeps its "
                                         "value");

      /* A legacy file is only consulted when the new format holds NOTHING. A
       * populated file that simply lacks this meter must read unset, or every
       * new meter would adopt the first row's digits. */
      fresh_files();
      put(IDX, "111,9\n");
      ck(meter_index_load(222) == -1, "a populated new-format file is never "
                                      "read as a legacy integer");
   }

   /* WHAT THE LEGACY LOADER PUBLISHES, not merely what it returns as a code.
    *
    * It ran `v = v * 10 + digit` into a signed int with no cutoff, at
    * STARTUP, over a file a user can edit and a torn write can leave
    * half-formed. The undefined behaviour is the headline, but the damage is
    * quieter than that: the number that came out was frequently a LEGAL
    * record index, so nothing downstream had any reason to object.
    *
    * Measured on the shipped parser, all four of these are just digits:
    *
    *   4294967297           -> published 1
    *   4294967296           -> published 0
    *   2147483648           -> published -2147483648
    *   99999999999999999999 -> published 1661992959
    *
    * The first two are the ones that matter and the reason each assertion
    * below names the VALUE rather than checking a return code. Publishing 1
    * tells the driver "records 0 and 1 are already imported" for a meter it
    * has never read, so the meter's two oldest fingersticks are never
    * fetched; publishing 0 loses one the same way. Both are then written back
    * in the new format by the next save and believed for ever. A silent,
    * permanent, one-way loss of real measurements, from a file nobody looked
    * at.
    *
    * -1 is the right answer to every one of them: "nothing stored, walk from
    * the beginning". It costs one bounded re-import; a guessed index costs
    * records. */
   printf("== a legacy index that cannot be trusted is not adopted ==\n");
   {
      /* WHAT THE INDEX IS BEFORE THE CORRUPT FILE ARRIVES, READ FIRST.
       *
       * An assertion that reads its subject out of the state the mutation
       * changed is not an assertion about the mutation. So the published
       * index is captured from a meter with NO file at all -- the honest
       * "nothing stored, walk from the beginning" -- and every corrupt file
       * below is then required to leave it exactly there. The constant -1 is
       * spelled once, here, and everything after it compares against a value
       * observed before the damage rather than against a literal that would
       * still read true if the meaning of -1 changed underneath it. */
      fresh_files();
      int unset = meter_index_load(1100);
      ck(unset == -1, "a meter with no index file at all is UNSET, and that "
                      "is the value a corrupt file must not move");

      /* THE ISOLATING CASE. Ten digits, so it is not refused for length; it
       * is refused, or not, purely on whether the bound sits on the wide side
       * of the cast. Pre-fix this published 1 -- a value no later check could
       * possibly object to, because 1 is a real index. */
      fresh_files();
      put(IDX, "4294967297\n");
      ck(meter_index_load(1101) == unset,
         "2^32 + 1 does not become record index 1 -- the meter's first two "
         "fingersticks are still fetched");

      fresh_files();
      put(IDX, "4294967296\n");
      ck(meter_index_load(1102) == unset,
         "...and 2^32 does not become record index 0");

      /* The exact int boundaries, one either side. */
      fresh_files();
      put(IDX, "2147483647\n");
      ck(meter_index_load(1103) == unset, "INT_MAX is outside the record-index "
                                          "domain and is refused");
      fresh_files();
      put(IDX, "2147483648\n");
      ck(meter_index_load(1104) == unset, "INT_MAX + 1 is refused rather than "
                                          "wrapped to INT_MIN");
      fresh_files();
      put(IDX, "-2147483648\n");
      ck(meter_index_load(1105) == unset, "INT_MIN is refused");
      fresh_files();
      put(IDX, "-2147483649\n");
      ck(meter_index_load(1106) == unset, "INT_MIN - 1 is refused");

      /* Nineteen digits: one past what the shared cursor holds. */
      fresh_files();
      put(IDX, "9999999999999999999\n");
      ck(meter_index_load(1107) == unset, "19 digits is not an index");
      fresh_files();
      put(IDX, "99999999999999999999\n");
      ck(meter_index_load(1108) == unset, "and neither is 20");

      /* THE DOMAIN ITSELF. otble.c refuses a record counter above 0xFFFF
       * because the index goes out to the meter in sixteen bits, so an index
       * above it names no record that can ever be asked for. Both sides of
       * the edge, so a change to either is a decision. */
      fresh_files();
      put(IDX, "65535\n");
      ck(meter_index_load(1109) == 65535,
         "the highest index the protocol can express is still adopted");
      fresh_files();
      put(IDX, "65536\n");
      ck(meter_index_load(1110) == unset,
         "...and the first one past it is not");

      /* FULL CANONICAL INPUT. The old loop stopped at the first non-digit and
       * kept what it had, so a file that is not this format at all had its
       * leading digits adopted as a walk position -- in a migration path that
       * runs once, silently, and whose answer is then written back and
       * believed for ever. */
      fresh_files();
      put(IDX, "123abc\n");
      ck(meter_index_load(1111) == unset,
         "trailing rubbish is not stripped off "
         "and the digits kept");
      fresh_files();
      put(IDX, "0012345\n");
      ck(meter_index_load(1112) == unset, "leading zeros are not this format");
      fresh_files();
      put(IDX, "12 34\n");
      ck(meter_index_load(1113) == unset,
         "an embedded space is not this format");
      fresh_files();
      put(IDX, "\n");
      ck(meter_index_load(1114) == unset,
         "a file of one newline holds no index");
      fresh_files();
      put(IDX, "-\n");
      ck(meter_index_load(1115) == unset,
         "a sign with no number behind it holds "
         "no index either");
      /* A file too long to have been read whole is a PREFIX, and a prefix of
       * a number is a different number. */
      fresh_files();
      put(IDX, "1111111111111111111111111111111111111111\n");
      ck(meter_index_load(1116) == unset,
         "a file longer than the read buffer is not adopted from its first "
         "31 bytes");

      /* ...and the honest legacy file still migrates, which is the whole
       * point of the path: tightening it into uselessness would re-import a
       * meter's window on every upgrade. */
      fresh_files();
      put(IDX, "0\n");
      ck(meter_index_load(1117) == 0, "a legacy index of zero is adopted as "
                                      "zero, not as unset");
      fresh_files();
      put(IDX, "12345");
      ck(meter_index_load(1118) == 12345, "a legacy file with no trailing "
                                          "newline is still adopted");
      fresh_files();
      put(IDX, "12345\r\n");
      ck(meter_index_load(1119) == 12345, "...and so is one written with CRLF");
   }

   printf("== a full index file evicts a row NOTHING is using ==\n");
   {
      /* Rows are never pruned -- every id a meter has ever carried keeps one
       * -- so the file does fill. Skipping the write (what it used to do)
       * meant this meter's index was never persisted again. Dropping the
       * OLDEST row (what it did next) drops the FIRST meter ever registered,
       * which is usually still in use. */
      fresh_files();
      sensors_paths(DIR);
      unlink(sensors_path());
      unlink(slots_path());
      sensors_load();

      /* Row 0 is a live meter; the rest are ids nothing references. */
      for (int i = 0; i < MAX_SLOTS; i++)
         ck(meter_index_save(1000 + i, i) == 0, "the file fills up");
      ck(sensor_claim_slot(1000, 4, "AA:BB:CC:DD:EE:01") >= 0,
         "the first meter is still in use");

      ck(meter_index_save(2000, 42) == 0, "a new meter's index is stored even "
                                          "so");
      ck(meter_index_load(2000) == 42, "...and it is the value asked for");
      ck(meter_index_load(1000) == 0, "THE LIVE METER KEEPS ITS INDEX -- "
                                      "losing it re-imports weeks of "
                                      "fingersticks into the lifetime log");
      ck(line_count(IDX) == MAX_SLOTS, "the file did not grow past its "
                                       "capacity");

      /* One of the unreferenced rows is gone, and exactly one. */
      int gone = 0;
      for (int i = 1; i < MAX_SLOTS; i++)
         if (meter_index_load(1000 + i) == -1)
            gone++;
      ck(gone == 1, "exactly one unused row was evicted");
   }

   printf("== a path that cannot be written REPORTS it ==\n");
   {
      /* Silence here is the whole family of defects above: the caller
       * believes the walk position is persisted and advances past records
       * that were never recorded. */
      char bad[224];
      (void)snprintf(bad, sizeof bad, "%s/nosuchdir/meter.idx", DIR);
      meter_store_paths(bad, SYNCF);
      ck(meter_index_save(909, 5) != 0, "a save into a missing directory "
                                        "fails loudly");
      ck(meter_index_save(0, 5) != 0, "and an id of zero is refused outright");
      meter_store_paths(IDX, SYNCF);
   }

   printf("== the per-meter runtime record ==\n");
   {
      /* THE POINTER DOES NOT LEAVE. Every read is a copy and every write is
       * a named operation, because binder callbacks write these fields while
       * the watchdog and the renderer read them -- `stat` is a 24-byte
       * string a device row was being drawn from through a borrowed
       * pointer. */
      struct meter_rt a;
      ck(!meter_rt_read(4001, &a), "an unknown meter has no record");
      ck(meter_rt_done(4001, 12345), "a write creates one on demand");
      ck(meter_rt_read(4001, &a), "...and it can be read back");
      ck(a.id == 4001, "...keyed by the registry id");
      ck(a.synced_t == 12345, "...holding what was written");
      ck(a.sync_t == 0 && a.rssi_ok == 0, "...and nothing that was not");
      ck(!meter_rt_read(4002, 0), "a READ still never creates a record");
      /* Each operation writes ITS OWN fields and leaves the rest alone. */
      ck(meter_rt_stat(4001, "COUNT", 1700000000), "a phase text is stored");
      ck(meter_rt_read(4001, &a) && strcmp(a.stat, "COUNT") == 0 &&
             a.sync_t == 1700000000 && a.synced_t == 12345,
         "...with its sync stamp, and the cooldown untouched");
      ck(meter_rt_rssi(4001, -70, 1700000001, 1700000002),
         "a connection's signal is stored");
      ck(meter_rt_read(4001, &a) && a.rssi == -70 && a.rssi_ok == 1 &&
             a.rssi_t == 1700000001 && a.sync_t == 1700000002 &&
             strcmp(a.stat, "COUNT") == 0,
         "...without disturbing the phase text");
      /* An advert with no plausible signal must not invent one. */
      ck(meter_rt_advert(4001, 1700000009, 42, 0, 0, 1700000009),
         "an advert with no signal is recorded");
      ck(meter_rt_read(4001, &a) && a.sync_t == 1700000009 &&
             a.advert_mono == 42 && a.rssi == -70,
         "...and leaves the last real signal alone");
   }

   printf("== an ambiguous fingerstick stamp is RETAINED, not settled ==\n");
   /* TODO 132's other half. A fingerstick taken in the repeated hour of a
    * fall-back names two instants an hour apart, and when the walk's own
    * record order does not settle which (meterlogic.h) the reading is still
    * stored -- refusing it would lose a fingerstick outright -- but the fact
    * that the stamp was a GUESS is kept, with the instant that was not
    * chosen, so it can be repaired rather than merely doubted. Dropping it
    * here is what makes a guess indistinguishable from a fact. */
   {
      struct meter_rt a;
      ck(meter_rt_read(4001, &a) && a.amb_n == 0,
         "the meter above starts with nothing ambiguous about it");
      ck(meter_rt_ambiguous(4001, 1762075800L),
         "an undecidable record is recorded as such");
      ck(meter_rt_read(4001, &a) && a.amb_n == 1 && a.amb_alt == 1762075800L,
         "...counted, and with the instant that was NOT chosen kept");
      ck(meter_rt_ambiguous(4001, 1762079400L), "a second one in the same "
                                                "import");
      ck(meter_rt_read(4001, &a) && a.amb_n == 2 && a.amb_alt == 1762079400L,
         "...counts up, keeping the most recent rejected instant");
      /* PER IMPORT, not for the life of the process: a count that accumulates
       * describes no particular sync, so a reader cannot tell whether the one
       * in front of them had any ambiguity in it. */
      ck(meter_rt_amb_clear(4001), "a new walk clears the previous import's");
      ck(meter_rt_read(4001, &a) && a.amb_n == 0 && a.amb_alt == 0,
         "...leaving nothing to misread as this import's");
      ck(!meter_rt_amb_clear(4002),
         "clearing a meter with no record creates none");
      ck(!meter_rt_read(4002, 0), "...so the table is not filled by clearing");
   }

   printf("== last-sync times survive a restart ==\n");
   {
      /* Without this the DEVICES list read "OFF / NEVER" for a meter that had
       * in fact synced, and SIGNAL STRENGTH showed "--" against a real LAST
       * SEEN: the runtime is in-memory and reset every launch. */
      fresh_files();
      struct meter_rt a;
      struct meter_rt b;
      meter_rt_advert(4001, 1700000000, 0, -72, 1, 1700000000);
      /* seen, but the signal was never sampled */
      meter_rt_advert(4002, 1700000060, 0, 0, 0, 0);
      meter_rt_done(4003, 1); /* never SEEN this launch: no sync_t */
      ck(meter_sync_save() == 0, "the times are written");
      ck(line_count(SYNCF) == 2, "a meter never seen writes no row");

      /* Clear the in-memory copies the way a relaunch would, then reload. */
      meter_rt_advert(4001, 0, 0, 0, 0, 0);
      meter_rt_advert(4002, 0, 0, 0, 0, 0);
      meter_sync_load();
      ck(meter_rt_read(4001, &a) && a.sync_t == 1700000000,
         "the first meter's last-sync comes back");
      ck(meter_rt_read(4002, &b) && b.sync_t == 1700000060,
         "...and the second's, unmixed");
      /* NEGATIVE: an RSSI is always negative, so a parser that drops the
       * sign turns -72 dBm into a signal stronger than physics allows. */
      ck(a.rssi == -72 && a.rssi_ok == 1, "the signal AT last-seen comes "
                                          "back, sign and all");
      ck(a.rssi_t == 1700000000, "...stamped with when it was captured");
      ck(b.rssi_ok == 0, "a meter whose signal was never sampled does not "
                         "acquire one");

      /* A garbled file must not invent times: a wrong LAST SEEN is read as a
       * meter that is present when it is not. */
      put(SYNCF, "junk\n0,1700000000,-50,1\n4001,1700000123,-60,1\n");
      meter_rt_advert(4001, 0, 0, 0, 0, 0);
      meter_sync_load();
      ck(meter_rt_read(4001, &a) && a.sync_t == 1700000123,
         "a good row after a bad one is still read");
      ck(!meter_rt_read(0, 0), "id 0 never becomes a record");

      /* No file at all is a first run, not a failure. */
      unlink(SYNCF);
      meter_rt_advert(4001, 5, 0, 0, 0, 0);
      meter_sync_load();
      ck(meter_rt_read(4001, &a) && a.sync_t == 5,
         "a missing file leaves the runtime alone");
   }

   printf("== the advert throttle is one step, not three ==\n");
   {
      /* Two scan callbacks for one meter is what a meter waking up delivers.
       * Reading the last stamp, deciding, and recording were three steps in
       * the caller, so both could pass and both issue a connect during the
       * one second the meter is listening. */
      fresh_files();
      ck(meter_rt_advert_turn(4200, 1700000000, 1000, -55, 1, 1700000000, 60),
         "the first advert since launch takes the turn");
      ck(!meter_rt_advert_turn(4200, 1700000005, 1005, -55, 1, 1700000005, 60),
         "...and a second one five seconds later does NOT");
      struct meter_rt tv;
      ck(meter_rt_read(4200, &tv) && tv.sync_t == 1700000000 &&
             tv.advert_mono == 1000,
         "...and the refused one changed nothing");
      ck(meter_rt_advert_turn(4200, 1700000100, 1100, -55, 1, 1700000100, 60),
         "one past the window takes the turn again");
      ck(meter_rt_read(4200, &tv) && tv.advert_mono == 1100,
         "...and stamps it");
   }

   printf("== a forgotten meter does not keep its seat for ever ==\n");
   {
      /* The table is MAX_SLOTS long and the loader creates a record per row
       * in the file, so a forgotten meter's row used to be
       * self-perpetuating: written back on every save, loaded again on every
       * launch, and holding a seat no live meter could then have.
       *
       * The prune only runs when the registry has something live to say --
       * "nothing is live" is also what an unread registry looks like, and
       * acting on it would wipe every meter's last-sync because slots.csv
       * failed to load. This suite has no registry, so the guard is what it
       * checks: the records SURVIVE. */
      fresh_files();
      meter_rt_advert(4101, 1700000000, 0, -60, 1, 1700000000);
      meter_rt_advert(4102, 1700000060, 0, -61, 1, 1700000060);
      ck(meter_sync_save() == 0, "a save with no registry loaded works");
      struct meter_rt s1;
      struct meter_rt s2;
      ck(meter_rt_read(4101, &s1) && meter_rt_read(4102, &s2),
         "...and keeps every record: an unread registry is not evidence "
         "that a meter is gone");
      /* Both of THESE rows are in the file -- the table also still holds
       * the records earlier sections made, which is why this is not a line
       * count. Reload from disk and ask for them by id. */
      meter_rt_advert(4101, 0, 0, 0, 0, 0);
      meter_rt_advert(4102, 0, 0, 0, 0, 0);
      meter_sync_load();
      ck(meter_rt_read(4101, &s1) && s1.sync_t == 1700000000 &&
             meter_rt_read(4102, &s2) && s2.sync_t == 1700000060,
         "...and both come back from the file it wrote");
   }

   printf("== a save that loses a race must not lose the reading ==\n");
   lost_update();

   printf("== the session cache under render/heartbeat overlap ==\n");
   sess_race();

   /* BEFORE the ceiling section, which deliberately fills the table for the
    * rest of the process -- a full table cannot create the record this one
    * writes to. */
   printf("== the table under real callback/watchdog/model overlap ==\n");
   overlap();

   printf("== the runtime table has a CEILING ==\n");
   {
      /* Last, because it fills the static table for the rest of the process.
       * A full table must refuse rather than write past the end: the array is
       * MAX_SLOTS long and the ids come from a file. */
      int made = 0;
      for (int i = 0; i < MAX_SLOTS + 5; i++)
         if (meter_rt_done(9000 + i, 1))
            made++;
      ck(made < MAX_SLOTS + 5, "a full table refuses new records instead of "
                               "overrunning");
      ck(meter_rt_read(9000, 0), "the records it did make are still there");
   }

   printf("\n%s\n",
          all ? "ALL METERSTORE TESTS PASSED" : "METERSTORE TESTS FAILED");
   return all ? 0 : 1;
}
