// SPDX-License-Identifier: GPL-3.0
// calibtest.c --- the calibration queue and the rescale factor
// Copyright 2026 Jakob Kastelic

/* This code decides whether the number on the screen is the number the sensor
 * meant, and it used to be twenty globals inside main.c where nothing could
 * reach it. Extracting it into calib.c is what makes this file possible, so
 * this file is the point of that extraction.
 *
 * What it pins down is the behaviour that is easy to lose in a refactor and
 * expensive to lose in the field: a correction is NEVER applied from a stale
 * reference, an implausible one is REJECTED rather than quietly clamped, a
 * backfilled point older than the correction keeps its raw value, and a
 * confirmed calibration is never dropped without saying so.
 *
 * The driver is stubbed here: what the sensor answers is exactly the variable
 * these paths turn on, so the test sets it directly rather than owning a
 * radio.
 */
#include "calib.h"
#include "clock.h"
#include "dexdriver.h"
#include "testdir.h" /* test_path: the per-mode fixture directory */
#include "uimodel.h" /* SCR_* / CAL_ST_*: the frame's own vocabulary */
#include <pthread.h>
#include <sched.h> /* sched_yield: the bounded liveness wait */ /* the UI/BLE overlap below is real threads */
#include <stdatomic.h> /* the fixture's own flags, not the state under test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
/* HOW MANY ASSERTIONS ACTUALLY RAN. This suite prints only failures, so silence
 * is its success -- and silence is also what a binary that died on its first
 * line produces, what a build that never linked produces, and what a mutant
 * that failed to compile produces when the caller checked the output instead
 * of the exit status. A count printed with the verdict makes "it passed"
 * distinguishable from "it never got there", which matters most to whoever is
 * deliberately breaking a rule to check that a case notices. */
static int g_pass;

static void ck(int cond, const char *what)
{
   if (!cond) {
      printf("  FAIL: %s\n", what);
      g_fail = 1;
      return;
   }
   g_pass++;
}

/* ---- THE TWO CLOCKS, DRIVEN BY HAND -----------------------------------
 *
 * This suite supplies clock.h's implementations instead of linking app/clock.c
 * (see the calibtest rule in the Makefile), and it is the only one that does.
 * The reason is the subject: calib.c's windows and throttles are ELAPSED-TIME
 * questions, and the defect they exist to rule out is a WALL-CLOCK JUMP -- an
 * NTP step, a phone finding a network, a user fixing the date -- moving them.
 * A test that reads the real clock cannot express that at all: it cannot set
 * the wall clock, and it can only obtain elapsed time by actually waiting.
 *
 * Driven separately, the two clocks say exactly what the phone experiences:
 *
 *   rt_jump(+3600)                an hour appears on the wall clock with no
 *                                 time having passed. The correction case.
 *   mono_advance(60)              a minute really elapses.
 *   clock_advance(60)             a minute passes on both, which is what an
 *                                 undisturbed phone does.
 *
 * ATOMIC because the three-thread fixtures below call into calib.c, which
 * reads both clocks; a plain long here would be a data race in the FIXTURE,
 * and this suite runs under TSan. Relaxed order is right -- nothing
 * synchronises through these, they are just a number every thread may read. */
static atomic_long g_rt   = 1700000000; /* a plausible epoch, not zero */
static atomic_long g_mono = 100000;     /* an arbitrary origin, as a boot is */

long realtime_s(void)
{
   return atomic_load_explicit(&g_rt, memory_order_relaxed);
}

long mono_s(void)
{
   return atomic_load_explicit(&g_mono, memory_order_relaxed);
}

long long now_ms(void)
{
   return (long long)mono_s() * 1000;
}

/* THE WALL CLOCK MOVES AND NO TIME PASSES. Nothing in this app may care. */
static void rt_jump(long d)
{
   atomic_fetch_add_explicit(&g_rt, d, memory_order_relaxed);
}

/* TIME PASSES AND THE WALL CLOCK DOES NOT MOVE -- which is what a monotonic
 * deadline is supposed to be the only thing that notices. */
static void mono_advance(long d)
{
   atomic_fetch_add_explicit(&g_mono, d, memory_order_relaxed);
}

/* An ordinary, undisturbed phone. */
static void clock_advance(long d)
{
   rt_jump(d);
   mono_advance(d);
}

/* ---- the stubbed sensor ------------------------------------------------ */

static struct dex_cal g_cal;
static int g_sent; /* last value handed to driver_calibrate */
/* THE WHOLE TOKEN THAT WENT ON THE WIRE, not just the value.
 *
 * The sensor's answer names the write it answers (sensor id, value,
 * generation) and the queue resolves only on an exact match. A harness that
 * remembered only the value could not deliver "the reply to the write that
 * really happened" -- it would have to make the generation up, and a made-up
 * generation is exactly what the rule under test refuses. So the stub records
 * what the driver was handed, and the cases below reply with THAT. */
static int g_sent_id;
static unsigned g_sent_gen;
static int g_nsent;     /* how many times it was called */
static int g_nprobe;    /* how many 0x32 probes went out */
static int g_refuse;    /* make driver_calibrate refuse (not streaming) */
static int g_repainted; /* the module asked for a repaint */

/* THE DRIVER'S SIDE OF THE QUEUE, faked.
 *
 * The calibration module hands the driver a table of its own operations and
 * the driver runs them inside its own lock (see dexdriver.h): nothing in
 * calib.c takes a lock any more. The stub here does the same dispatch with a
 * depth counter instead of a mutex -- which also proves the module never
 * re-enters through the driver while it is already inside one of them. */
static const struct driver_cal_ops *g_ops;
static int g_lockdepth;
static int g_maxdepth;

void driver_set_cal_ops(const struct driver_cal_ops *ops)
{
   g_ops = ops;
}

static void ops_enter(void)
{
   g_lockdepth++;
   if (g_lockdepth > g_maxdepth)
      g_maxdepth = g_lockdepth;
}

void driver_cal_attempt(int link, int sensor_id)
{
   ops_enter();
   if (g_ops && g_ops->attempt)
      g_ops->attempt(link, sensor_id);
   g_lockdepth--;
}

int driver_cal_queue(int sensor_id, int mg_dl)
{
   ops_enter();
   int rc = (g_ops && g_ops->queue) ? g_ops->queue(sensor_id, mg_dl) : -1;
   g_lockdepth--;
   return rc;
}

int driver_cal_cancel(void)
{
   ops_enter();
   int rc = (g_ops && g_ops->cancel) ? g_ops->cancel() : -1;
   g_lockdepth--;
   return rc;
}

void driver_cal_tick(void)
{
   ops_enter();
   if (g_ops && g_ops->tick)
      g_ops->tick();
   g_lockdepth--;
}

int driver_cal_queued_for(int sensor_id)
{
   int q = 0;
   ops_enter();
   if (g_ops && g_ops->queued_for)
      q = g_ops->queued_for(sensor_id);
   g_lockdepth--;
   return q;
}

void driver_cal_of(int link, struct dex_cal *out)
{
   (void)link;
   *out = g_cal;
}

void driver_cal_bounds(int link)
{
   (void)link;
   g_nprobe++;
   /* The real driver stamps this, so the stub does too. calib.c no longer
    * READS it -- the probe throttle is its own monotonic deadline now, because
    * a wall-clock stamp made "at most once a minute" mean "on every reading"
    * after a forward correction and "never again" after a backward one. The
    * stamp is left here on purpose: a stub that quietly stopped doing what the
    * driver does would hide the day something starts reading it again. */
   g_cal.asked = realtime_s();
}

int driver_calibrate(int link, int mg_dl, int sensor_id, unsigned gen)
{
   (void)link;
   g_nsent++;
   if (g_refuse)
      return 0;
   g_sent     = mg_dl;
   g_sent_id  = sensor_id;
   g_sent_gen = gen;
   return 1;
}

/* THE SENSOR ANSWERS THE WRITE THAT WAS ACTUALLY MADE.
 *
 * Every case that used to call pancra_cal_result(r) directly now goes through
 * here, so the reply is built from what the stub above SAW go out rather than
 * from what the test happens to know is queued. That distinction is the whole
 * of item 96: a reply constructed from the live queue can never disagree with
 * the live queue, so a test written that way pins nothing at all. */
static void sensor_answers(int result)
{
   pancra_cal_result(result, g_sent_id, g_sent, g_sent_gen);
}

/* The repaint calib.c asks for. It used to be a module-specific repaint this
 * file stubbed and main.c implemented -- a second name for shell_ui_dirty(), in
 * the header of the module that merely calls it. */
void shell_ui_dirty(void)
{
   g_repainted++;
}

/* ---- fixtures ---------------------------------------------------------- */

/* Set in main from test_dir(), so an ASan or TSan build of this suite keeps
 * its fixtures in its OWN tree -- see app/test/testdir.h. It used to be the
 * literal build/app/test/calibdata, which is how `make -j2 appasan apptsan`
 * came to fail here: two processes, one directory, each reset() replacing the
 * files the other was asserting about.
 *
 * SIZED AGAINST THE BUFFERS BELOW, not guessed: every fixture path is g_dir
 * plus a suffix of at most twelve characters, and gcc's -Wformat-truncation
 * (fatal here, and the reason this gate compiles at -O2) refuses the pair if
 * the arithmetic does not work out. Generous rather than tight because
 * APP_TESTDIR is a real directory in somebody's checkout, which can be deep --
 * and test_path aborts rather than truncating, so being stingy here would turn
 * a deep source tree into a failed gate. */
static char g_dir[192];

/* Hand-write a persisted state, so a RESTART can be tested without one. */
static void put(const char *name, const char *text);

/* Back to a known state, through the public interface only: write the two
 * files as a fresh install would leave them and load them. This works because
 * calib_load DEFINES the state rather than adding to it -- which is itself
 * worth leaning on here, since every case below depends on it. */
static void reset(void)
{
   put("cal.q", "0,0,0,0,0,0,0\n");
   put("rescale.cfg", "0,1000,0,0,0,0\n");
   calib_load();
}

/* ---- THE QUEUE / LAST CAL OVERLAP -------------------------------------
 *
 * One thread queues and resolves (what the driver's callbacks do), one
 * reloads the file (calib_load, which today runs only at startup -- the
 * point is that the loader must not assume it is alone), and one draws the
 * row.
 * The row is what the assertions are about. */
#define QRACE_ID     31
#define QRACE_ROUNDS 3000

/* ATOMIC because three threads write them. Plain ints here were a data race
 * in the fixture itself -- benign in practice, and exactly the kind of thing
 * that makes a sanitiser report the test instead of the code. */
static atomic_int g_qtorn; /* rows that could not have come from one instant */
static atomic_int g_qseen; /* rows that showed something at all */
static atomic_int g_qstop;

static void *qrace_write(void *arg)
{
   (void)arg;
   for (int i = 0; i < QRACE_ROUNDS; i++) {
      /* THE VALUE AND THE VERDICT ARE CORRELATED, and that is the whole
       * point of the numbers: 50 is even, so `100 + (i % 50)` is even
       * exactly when i is, and `i % 2` makes the sensor ACCEPT exactly when
       * i is even. So in every record this fixture can produce,
       *
       *      APPLIED  <=>  the value is even.
       *
       * A row assembled from two different resolutions breaks that half the
       * time. Without the correlation the reader could only check each field
       * against its own range -- which no interleaving can violate, so the
       * assertions passed with the locking removed entirely. */
      calib_queue(QRACE_ID, 100 + (i % 50));
      /* SENT, THEN ANSWERED, because an answer now has to name the write it
       * answers. This used to queue and resolve with nothing in between, which
       * was fine when the verdict applied to whatever was queued; a reply is
       * matched against the live entry now, so the fixture has to make a real
       * write and reply to THAT. */
      calib_try(LINK_CGM, QRACE_ID);
      /* THE VERDICT IS A FUNCTION OF THE VALUE THAT WENT ON THE WIRE, and it
       * has to be, which is worth spelling out because getting it wrong made
       * this fixture report a tear that had not happened.
       *
       * The correlation above is stated in terms of `i`, and `i` is only the
       * right answer while the value resolved is the value THIS round queued.
       * Once a real write is interposed that stops being true: the loader
       * thread can install the file between the queue and the send, so what
       * goes out is whatever line was last saved -- an EARLIER round's value --
       * and answering it with this round's parity records an odd value APPLIED.
       * That is the reader's definition of a torn record, produced by the
       * fixture rather than by the code, and it fails perhaps one run in ten
       * (ASan, being slower, loses the race more often, which is how it was
       * found).
       *
       * Asking the SENSOR STUB what it received removes the assumption
       * entirely: whatever value reached the wire is answered according to its
       * own parity, so every record this fixture can produce satisfies
       * APPLIED <=> even by construction -- and it is also what a real sensor
       * does, since its answer depends on the number it was given and not on
       * what the phone queued afterwards. */
      pancra_cal_result((g_sent % 2) == 0 ? 0 : 3, g_sent_id, g_sent,
                        g_sent_gen);
   }
   /* STOP ONLY ONCE A READER HAS LOOKED. The threads are not fair -- the
    * writer does file I/O and the readers do not -- and a run in which the
    * viewer was never scheduled proves nothing while passing. The last
    * action above leaves a resolved verdict standing, so there is always
    * something to see; the spin is bounded so a genuinely wedged reader
    * fails the liveness assertion instead of hanging the test. */
   for (int spin = 0; spin < 2000000 && !atomic_load(&g_qseen); spin++)
      sched_yield();
   atomic_store(&g_qstop, 1);
   return 0;
}

static void *qrace_load(void *arg)
{
   (void)arg;
   while (!atomic_load(&g_qstop))
      (void)calib_load(); /* a whole-record install, concurrent with both */
   return 0;
}

static void *qrace_view(void *arg)
{
   (void)arg;
   while (!atomic_load(&g_qstop)) {
      struct calib_view v;
      calib_view(QRACE_ID, &v);
      if (v.queued_mgdl || v.last_t)
         atomic_store(&g_qseen, 1);
      /* A verdict always carries a state and a value -- a row built from
       * half a record would have one without the others. */
      if (v.last_t && (!v.last_state || !v.last_mgdl))
         atomic_store(&g_qtorn, 1);
      /* THE ONE THAT CATCHES A MIXED RECORD: the value and the verdict must
       * have come from the SAME resolution. See qrace_write for why APPLIED
       * and an even value are the same fact here. */
      if (v.last_t && v.last_mgdl) {
         int applied = (v.last_state == CAL_ST_APPLIED);
         int even    = ((v.last_mgdl % 2) == 0);
         if (applied != even)
            atomic_store(&g_qtorn, 1);
      }
      /* ...and a queued value is one this test could have queued. */
      if (v.queued_mgdl && (v.queued_mgdl < 100 || v.queued_mgdl > 149))
         atomic_store(&g_qtorn, 1);
   }
   return 0;
}

/* THE FILE ITSELF, which is what item 5 was actually about: "a save can race
 * a load or a view and SERIALISE A MIXED RECORD". A row on the screen hides
 * most of a tear -- calib_view reports LAST CAL only when nothing is queued,
 * so a half-updated verdict behind a live queue is invisible there -- but the
 * file cannot hide one: it is seven numbers on one line, and the next launch
 * believes all seven.
 *
 * So this reads cal.q as fast as it can and checks the line against the same
 * correlation qrace_write builds in. A save that read the queue at one
 * instant and LAST CAL at another writes a line that breaks it. */
static void *qrace_file(void *arg)
{
   (void)arg;
   char path[256];
   (void)snprintf(path, sizeof path, "%s/cal.q", g_dir);
   while (!atomic_load(&g_qstop)) {
      FILE *f = fopen(path, "rb");
      if (!f)
         continue;
      char b[128];
      char *got = fgets(b, (int)sizeof b, f);
      fclose(f);
      if (!got)
         continue;
      /* strtol, not sscanf: sscanf cannot say "that was not a number", and
       * a field it failed to convert is left as whatever the variable held
       * -- which in a torn-read detector would read as a clean record. */
      long fld[7] = {0, 0, 0, 0, 0, 0, 0};
      int nf      = 0;
      for (const char *q = b; nf < 7; nf++) {
         char *end = NULL;
         fld[nf]   = strtol(q, &end, 10);
         if (end == q)
            break;
         q = end;
         if (*q == ',')
            q++;
         else
            break;
      }
      if (nf < 6) /* the last field has no comma after it, so 6 is a whole */
         continue;
      int qmgdl  = (int)fld[1];
      int lmgdl  = (int)fld[3];
      long lt    = fld[4];
      int lstate = (int)fld[5];
      int lid    = (int)fld[6];
      if (qmgdl && (qmgdl < 100 || qmgdl > 149))
         atomic_store(&g_qtorn, 1); /* a queue value nothing here queued */
      if (lmgdl) {
         atomic_store(&g_qseen, 1);
         if (lt <= 0 || !lstate || lid != QRACE_ID)
            atomic_store(&g_qtorn, 1); /* half a verdict, on the disk */
         int applied = (lstate == CAL_ST_APPLIED);
         int even    = ((lmgdl % 2) == 0);
         if (applied != even)
            atomic_store(&g_qtorn, 1); /* two resolutions in one record */
      }
   }
   return 0;
}

/* ---- IF THIS FIXTURE EVER FAILS, CHECK FOR A SECOND PROCESS FIRST -------
 *
 * The three checks above are the only assertions in this suite that can be
 * broken by something OUTSIDE it, and they were: a `make appasan` run reported
 * "the queue and its verdict are never seen half-swapped" while a plain `make
 * calibtest` passed, and the cause was two processes in one fixture directory,
 * not anything in calib.c.
 *
 * app/test/testdir.h keys the fixture directory by MODE and by SUITE, and its
 * comment records both lessons. What is left over is the same suite in the same
 * mode running TWICE AT ONCE -- `make calibtest` started by hand while `make
 * check` or `make appasan` is already running it, or two `make appasan`. The
 * loader thread here reads cal.q as fast as it can, so the other process's
 * record is installed as this one's: a value nothing here queued, a sensor id
 * that is not QRACE_ID, a verdict from a case in a different section. Every one
 * of those is what a torn read looks like from inside.
 *
 * REPRODUCED, so this is a note about a known cause and not a guess: two copies
 * of this binary sharing one APP_TESTDIR tore twice in six runs, and the tears
 * named values (96, 145) that this fixture cannot produce -- while ninety-four
 * runs in a directory of its own tore not once. The tell is exactly that: a
 * value outside 100..149, or unrelated cases in this suite failing in the same
 * run. A real tear can only ever show a value this fixture queued.
 *
 * Give the run its own APP_TESTDIR before looking at calib.c. */
static void qrace_run(void)
{
   atomic_store(&g_qstop, 0);
   atomic_store(&g_qtorn, 0);
   atomic_store(&g_qseen, 0);
   /* The writer thread now really SENDS before it answers (see qrace_write),
    * and a send needs a sensor that has said it permits one. */
   g_cal.have = g_cal.permitted = 1;
   /* NOLINTNEXTLINE(misc-include-cleaner) -- see threadtest.c on pthread_t */
   pthread_t th[4];
   pthread_create(&th[0], 0, qrace_write, 0);
   pthread_create(&th[1], 0, qrace_load, 0);
   pthread_create(&th[2], 0, qrace_view, 0);
   pthread_create(&th[3], 0, qrace_file, 0);
   for (int i = 0; i < 4; i++)
      pthread_join(th[i], 0);
}

/* ---- the overlap fixture: setters on one thread, readers on others ----
 *
 * Sensor 11 carries +20% (target 120 over raw 100) and sensor 22 carries
 * -20% (80 over 100); those are the ONLY two factors this fixture can
 * produce, so a reader that sees any other number for a sensor read a
 * transition that was half-written. */
#define RACE_A_ID   11
#define RACE_B_ID   22
#define RACE_A_PM   1200
#define RACE_B_PM   800
#define RACE_ROUNDS 4000

static atomic_int g_torn; /* views that could not have come from one instant */
static atomic_int g_seen; /* views that found a factor: the run did work */
/* The readers run UNTIL THE SETTER IS DONE rather than for a fixed count.
 * With a count they finished first -- each set writes a file, so the setter
 * is far the slower thread -- and the run then proved nothing while passing:
 * "no reader ever saw a factor" is exactly what a broken lock could also
 * produce. */
static atomic_int g_race_done;

static void *race_setter(void *arg)
{
   (void)arg;
   for (int i = 0; i < RACE_ROUNDS; i++) {
      calib_rescale_set(RACE_A_ID, 100, 120);
      calib_rescale_set(RACE_B_ID, 100, 80);
   }
   /* Same reason as qrace_write: a run in which no reader was scheduled
    * proves nothing. A's factor is standing when the loop ends. */
   for (int spin = 0; spin < 2000000 && !atomic_load(&g_seen); spin++)
      sched_yield();
   atomic_store(&g_race_done, 1);
   return 0;
}

/* THE RENDERER: every frame asks what device A's row should say, and asks
 * nothing else. Its own thread, doing only this, on purpose -- a reader that
 * also called a locking function would be scheduled by that lock and would
 * only ever look at the state BETWEEN transitions, which is precisely the
 * observation this case must not be limited to. */
static void *race_render(void *arg)
{
   (void)arg;
   while (!atomic_load(&g_race_done)) {
      struct calib_view rv;
      calib_view(RACE_A_ID, &rv);
      if (rv.rescale_pm != 1000) {
         atomic_store(&g_seen, 1);
         if (rv.rescale_pm != RACE_A_PM)
            atomic_store(&g_torn, 1);
      }
   }
   return 0;
}

/* THE READING PATH: the factor applied to a sample that is then stored for
 * good, which is where a half-written transition stops being cosmetic. */
static void *race_read(void *arg)
{
   (void)arg;
   while (!atomic_load(&g_race_done)) {
      int pm  = 0;
      int adj = calib_on_backfill(RACE_A_ID, realtime_s(), 100, &pm);
      if (pm != 1000 && pm != RACE_A_PM)
         atomic_store(&g_torn, 1);
      if (adj != 100 && adj != (RACE_A_PM + 5) / 10)
         atomic_store(&g_torn, 1);
   }
   return 0;
}

static void race_run(void)
{
   /* NOLINTNEXTLINE(misc-include-cleaner) -- see threadtest.c on pthread_t */
   pthread_t th[3];
   atomic_store(&g_torn, 0);
   atomic_store(&g_seen, 0);
   atomic_store(&g_race_done, 0);
   pthread_create(&th[0], 0, race_setter, 0);
   pthread_create(&th[1], 0, race_render, 0);
   pthread_create(&th[2], 0, race_read, 0);
   for (int i = 0; i < 3; i++)
      pthread_join(th[i], 0);
}

static void put(const char *name, const char *text)
{
   char p[256];
   (void)snprintf(p, sizeof p, "%s/%s", g_dir, name);
   FILE *f = fopen(p, "w");
   if (!f) {
      printf("  FAIL: cannot write %s\n", p);
      g_fail = 1;
      return;
   }
   fputs(text, f);
   fclose(f);
}

int main(void)
{
   calib_register_ops(); /* the driver runs the queue: see calib.h */
   /* THE CLOCK IS READ FRESH AT EVERY USE, not captured once here.
    *
    * The module stamps its own realtime_s() when a rescale is requested or
    * activated, and the cases below hand it a time of their own. Captured
    * once at the top, that time drifts behind the module's stamps by however
    * long the run takes -- under `make check`, with the machine busy, far
    * enough to put a reading on the wrong side of a freshness window. Four
    * assertions then failed in the gate and passed on every standalone run,
    * which is the worst way for a test to be wrong. */
#define now (realtime_s())
   /* MAKE OUR OWN DATA DIRECTORY. Every fixture below writes into it, and
    * without it `put` fails on every call -- which showed up as a dozen
    * unrelated assertion failures with no hint of the cause. It existed only
    * because a previous run had left it behind, so this test passed until the
    * first `make clean`, then failed in a way that read like a real bug in
    * the calibration queue. */
   test_path(g_dir, sizeof g_dir, "calibdata");
   if (mkdir(g_dir, 0777) != 0 && access(g_dir, W_OK) != 0) {
      printf("FAIL: cannot create the test data directory %s\n", g_dir);
      return 1;
   }
   calib_paths(g_dir);

   /* ---- the factor itself ---- */
   ck(calib_rescale_preview(120, 100) == 1200, "120 over raw 100 is +20%");
   ck(calib_rescale_preview(100, 100) == 1000, "a matching value is no change");
   ck(calib_rescale_preview(120, 0) == 0,
      "no reading to divide by yields the 'not yet' sentinel, not a factor");

   /* ---- an implausible correction is REJECTED, not clamped ---- */
   reset();
   calib_rescale_set(7, 100, 200); /* +100%: far outside +-25% */
   ck(calib_rescale_pm() == 1000, "a >25%% correction does NOT become active");
   struct calib_view v;
   calib_view(7, &v);
   ck(v.rescale_rejected, "...and the rejection is surfaced, not swallowed");
   ck(v.rescale_pm == 1000, "...leaving no factor behind");

   /* Clamping instead of rejecting would silently apply 125% of a reading the
    * user just said was 100% wrong -- the case this asserts against. */
   int adj = calib_on_reading(7, 0, now, 100, NULL, NULL);
   ck(adj == 100, "a rejected correction leaves readings untouched");

   /* ---- THE BAND ITSELF, at its edges ---- */
   /* Tested AT the boundary, not merely well inside it: "+100% is rejected
    * and +10% is accepted" holds for any band between them, so it pins down
    * nothing about where the limit actually is. */
   reset();
   calib_rescale_set(7, 100, 125); /* exactly +25%: the last allowed */
   ck(calib_rescale_pm() == RESCALE_MAX_PM, "the +25% edge is allowed");
   reset();
   calib_rescale_set(7, 100, 126); /* one step past it */
   ck(calib_rescale_pm() == 1000, "...and one step past it is refused");
   reset();
   calib_rescale_set(7, 100, 75); /* exactly -25% */
   ck(calib_rescale_pm() == RESCALE_MIN_PM, "the -25% edge is allowed");
   reset();
   calib_rescale_set(7, 100, 74);
   ck(calib_rescale_pm() == 1000, "...and one step below it is refused");

   /* ---- a plausible one applies, to the right sensor only ---- */
   reset();
   calib_rescale_set(7, 100, 110); /* +10% */
   ck(calib_rescale_pm() == 1100, "110 over raw 100 activates as +10%");
   int pm = 0;
   adj    = calib_on_reading(7, 0, now + 10, 100, &pm, NULL);
   ck(adj == 110 && pm == 1100, "the sensor it was set for is rescaled");
   adj = calib_on_reading(8, 1, now + 10, 100, &pm, NULL);
   ck(adj == 100 && pm == 1000, "a DIFFERENT sensor is not");

   /* ---- the timestamp gate: history predating the correction is raw ---- */
   adj = calib_on_backfill(7, now - 3600, 100, &pm);
   ck(adj == 100 && pm == 1000,
      "a backfilled point OLDER than the activation keeps its raw value");
   adj = calib_on_backfill(7, now + 3600, 100, &pm);
   ck(adj == 110 && pm == 1100, "...and a newer one is rescaled");

   /* ---- persistence: a factor survives a restart ---- */
   calib_rescale_stop();
   ck(calib_rescale_pm() == 1000, "STOP turns it off");
   calib_rescale_set(7, 100, 110);
   calib_load(); /* what a restart does */
   ck(calib_rescale_pm() == 1100, "an active factor survives a restart");

   /* ---- a target with no reading yet is HELD, not lost ---- */
   reset();
   calib_rescale_set(7, 0, 130); /* raw 0: nothing to compute against */
   ck(calib_rescale_pm() == 1000, "with no reading there is no factor yet");
   calib_view(7, &v);
   ck(v.rescale_pending == 130, "...the target is held, visibly");
   ck(calib_rescale_engaged(7), "...and the sensor counts as engaged");
   /* 130 over 100 is +30%, outside the band, so the held path must REJECT it
    * exactly as the direct one does -- a target does not become acceptable by
    * having waited. */
   adj = calib_on_reading(7, 0, now, 100, &pm, NULL);
   ck(calib_rescale_pm() == 1000, "a held target obeys the same +-25% bound");
   ck(adj == 100 && pm == 1000, "...so the reading it resolved on is raw");
   calib_view(7, &v);
   ck(v.rescale_pending == 0,
      "...and the target is consumed, not left hanging");
   ck(v.rescale_rejected, "...with the rejection surfaced");

   reset();
   calib_rescale_set(7, 0, 110);
   int started = 0;
   adj         = calib_on_reading(7, 0, now, 100, &pm, &started);
   ck(calib_rescale_pm() == 1100, "a plausible held target activates");
   ck(started, "...and says so, so the chirp is not a phantom swing");
   ck(adj == 110, "...effective on the very reading that resolved it");

   /* ---- AND IT BELONGS TO ONE SENSOR ----
    *
    * Every case in this file used sensor id 7, so `g_rescale_id == src` could
    * be relaxed to `>=` and nothing failed: one sensor's correction would
    * silently rescale another's readings, which with two sensors on the arm
    * is a wrong number on the screen and in the log. The factor above is
    * live for id 7; id 8 must be untouched by it. */
   {
      int pm8  = 0;
      int adj8 = calib_on_reading(8, 1, now, 100, &pm8, NULL);
      ck(pm8 == 1000, "another sensor's reading is not rescaled");
      ck(adj8 == 100, "...and comes through at its raw value");
      int pm7 = 0;
      ck(calib_on_reading(7, 0, now, 100, &pm7, NULL) == 110 && pm7 == 1100,
         "...while the sensor it belongs to still is");
   }

   /* Rounding is half-up at the boundary, and it is applied to the value the
    * user reads: 1005 pm of 100 is 100.5, which must not truncate to 100. */
   {
      reset();
      calib_rescale_set(7, 200, 201); /* 1005 pm */
      int pmr = 0;
      calib_on_reading(7, 0, now, 200, &pmr, NULL);
      ck(pmr == 1005, "a 0.5% correction is representable");
      int r = calib_on_reading(7, 0, now + 300, 100, &pmr, NULL);
      ck(r == 101, "...and rounds half-up rather than truncating");
   }

   /* ---- a STALE reference expires instead of applying ----
    *
    * STALE MEANS THE USER HAS BEEN WAITING, which is elapsed time, so the
    * clock that has to move is the monotonic one. This case used to hand
    * calib_on_reading a reading timestamp a window past the request instead --
    * the reading's own instant minus the app's wall-clock stamp, a difference
    * between two numbers from two different sources that is not an elapsed
    * time at all and that any clock correction moves. See calib.c. */
   reset();
   calib_rescale_set(7, 0, 110);
   clock_advance(RESCALE_PEND_WINDOW_S + 1);
   started = 0;
   adj     = calib_on_reading(7, 0, now, 100, &pm, &started);
   ck(calib_rescale_pm() == 1000,
      "a reading past the window does NOT apply the stale target");
   ck(!started && adj == 100, "...and the reading itself is untouched");
   calib_view(7, &v);
   ck(v.rescale_expired, "...the expiry is surfaced, never a silent drop");

   /* A pending target written before a long downtime must not spring to life
    * on restart either. Fields: id,pm,t,pend_id,pend_mgdl,pend_t */
   reset();
   char line[128];
   (void)snprintf(line, sizeof line, "0,1000,0,7,110,%ld\n",
                  now - RESCALE_PEND_WINDOW_S - 60);
   put("rescale.cfg", line);
   calib_load();
   ck(calib_rescale_pm() == 1000, "a stale pending target does not survive");
   calib_view(7, &v);
   ck(v.rescale_pending == 0 && v.rescale_expired,
      "...it is reported EXPIRED rather than resumed");

   /* ---- the calibration queue ---- */
   reset();
   calib_load();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.result = -1;
   calib_queue(7, 120);
   ck(calib_queued_for(7) == 120, "a confirmed calibration is queued");
   ck(calib_queued_for(8) == 0, "...for that sensor only");
   calib_view(7, &v);
   ck(v.queued_mgdl == 120, "...and shown as pending");

   /* Permission unknown: PROBE, do not write. */
   g_nsent = g_nprobe = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 0, "nothing is written before the sensor grants permission");
   ck(g_nprobe == 1, "...a permission probe goes out instead");
   calib_try(LINK_CGM, 7);
   ck(g_nprobe == 1, "...and is throttled, never hammered");

   /* Permitted: write it. */
   g_cal.have = g_cal.permitted = 1;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1 && g_sent == 120, "once permitted, the value is written");
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1, "...and not resent while a reply is still awaited");
   ck(calib_queued_for(7) == 120,
      "...it stays queued until the sensor answers");

   /* The sensor accepts. */
   g_repainted = 0;
   sensor_answers(0);
   ck(calib_queued_for(7) == 0, "an accepted calibration leaves the queue");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 120,
      "...and is recorded as APPLIED");
   ck(v.queued_mgdl == 0, "...with nothing still pending");
   ck(g_repainted > 0, "...and the screen is told to update");

   /* The sensor rejects: surfaced, not retried forever. */
   reset();
   calib_queue(7, 300);
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   sensor_answers(3);
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_REJECTED, "a rejected value is recorded as such");
   ck(calib_queued_for(7) == 0, "...and dropped rather than resent");

   /* ---- THE REPLY TO A CALIBRATION THAT WAS REPLACED BEFORE IT ARRIVED ----
    *
    * THIS IS THE CASE THE WHOLE TOKEN EXISTS FOR, and it is the only one here
    * that can tell the fix from what it replaced. "The right one resolves" is
    * every case above and it passed before the change too.
    *
    * WHAT IT LOOKED LIKE TO THE PERSON HOLDING THE PHONE. They take a
    * fingerstick, misread the meter, type 100 and confirm. The app writes 0x34
    * for 100. Before the sensor answers -- one connection interval at best, a
    * whole reconnect at worst -- they look again, see 180, and re-enter it.
    * The sensor's ACCEPTED for the 100 lands. The driver knew only "a write is
    * outstanding", so the shell resolved whatever was queued: the row said
    * LAST CAL 180 APPLIED, nothing was left pending, and 180 was never sent.
    * The sensor was calibrated to 100 and went on reporting against 100 for
    * the rest of the session, with the screen insisting otherwise.
    *
    * THREE THINGS ARE ASSERTED, and the first two are the ones that matter:
    * 180 is NOT marked resolved, and it is STILL QUEUED (a fix that merely
    * dropped the reply and the queue with it would be a different bug). The
    * third is that it then goes out, so a discarded reply costs the user
    * nothing but the round trip.
    *
    * NOTE WHAT IS NOT ASSERTED: the HTTP-status equivalent, "the reply did not
    * crash". A reply that is refused for some later, unrelated reason would
    * satisfy that and prove nothing, which is why every assertion below names
    * the state of the 180. */
   reset();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.have = g_cal.permitted = 1;
   ck(calib_queue(7, 100) == CALIB_OK, "the user confirms 100");
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1 && g_sent == 100, "...and 100 goes on the wire");
   /* Remember the token of the write that really happened, because queueing
    * 180 is about to overwrite the module's idea of what is live. */
   int a_id       = g_sent_id;
   int a_mgdl     = g_sent;
   unsigned a_gen = g_sent_gen;
   ck(calib_queue(7, 180) == CALIB_OK, "the user replaces it with 180");
   ck(calib_queued_for(7) == 180, "...so 180 is what is queued");
   /* A's reply, delivered late. Built from A's own token -- NOT from the live
    * queue, which is the whole point. */
   g_repainted = 0;
   pancra_cal_result(0, a_id, a_mgdl, a_gen);
   ck(calib_queued_for(7) == 180,
      "the late reply to 100 leaves 180 QUEUED: it has not been sent, so the "
      "sensor has not answered it");
   calib_view(7, &v);
   ck(v.last_state != CAL_ST_APPLIED,
      "...and 180 is NOT recorded as accepted by a sensor that never saw it");
   ck(v.last_mgdl != 180,
      "...and no verdict at all is recorded against the value 180");
   ck(v.queued_mgdl == 180, "...the row still shows 180 PENDING");
   /* And it really is sent, rather than being stuck behind the throttle that
    * the discarded write left standing. calq_attempt_locked stamps the resend
    * deadline only against the entry it read, so 180's first attempt is free.
    */
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1 && g_sent == 180,
      "...and 180 is written on the next attempt, not silently dropped");
   /* Now answer THAT write, and the ordinary path still works. */
   sensor_answers(0);
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 180,
      "the reply to the write that was actually made resolves it");
   ck(calib_queued_for(7) == 0, "...and the queue is empty");
   /* THE SAME VALUE RE-ENTERED IS A DIFFERENT CALIBRATION. Without a
    * generation, (sensor, value) alone would match -- so cancelling 150 and
    * typing 150 again would let the first write's reply resolve the second
    * entry, which is the same defect with the value coincidentally equal. */
   reset();
   g_cal.have = g_cal.permitted = 1;
   calib_queue(7, 150);
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1, "150 is written");
   int b_id       = g_sent_id;
   int b_mgdl     = g_sent;
   unsigned b_gen = g_sent_gen;
   calib_queue(7, 150); /* the user re-enters the very same number */
   pancra_cal_result(0, b_id, b_mgdl, b_gen);
   ck(calib_queued_for(7) == 150,
      "a re-entered 150 is a NEW calibration, and the old write's reply does "
      "not resolve it");
   calib_view(7, &v);
   ck(v.last_state != CAL_ST_APPLIED,
      "...nothing is recorded APPLIED on the strength of the earlier write");
   /* A REPLY FOR ANOTHER SENSOR IS NOT THIS SENSOR'S ANSWER. Two CGMs is a
    * supported arrangement here, and a verdict crossing between them would
    * record one sensor's fingerstick against the other's. */
   reset();
   g_cal.have = g_cal.permitted = 1;
   calib_queue(7, 130);
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1, "130 is written for sensor 7");
   pancra_cal_result(0, 8 /* a different sensor */, g_sent, g_sent_gen);
   ck(calib_queued_for(7) == 130,
      "a reply naming another sensor does not resolve this one's queue");
   calib_view(7, &v);
   ck(v.last_state != CAL_ST_APPLIED, "...and records no verdict for it");
   /* AND THE VALUE IS CHECKED IN ITS OWN RIGHT, not merely carried.
    *
    * Worth saying why this case exists, because it is the one place the three
    * fields are not interchangeable. In every interleaving the app can produce
    * today the GENERATION already tells the entries apart, so removing the
    * value from the match leaves every other case here green -- the redundancy
    * is real and it was measured. What the value pins is the OTHER direction:
    * that a reply must agree with the queue about WHAT WAS WRITTEN, so a
    * driver whose record went stale, or a peer answering with a number nobody
    * sent, cannot resolve a calibration by naming the right generation. The
    * item asks for all three; this is the assertion that makes the third one
    * load-bearing rather than decorative. */
   pancra_cal_result(0, 7, g_sent + 1 /* a value nobody wrote */, g_sent_gen);
   ck(calib_queued_for(7) == 130,
      "a reply whose VALUE is not the one written does not resolve the queue, "
      "even with the right sensor and generation");
   calib_view(7, &v);
   ck(v.last_state != CAL_ST_APPLIED,
      "...and no verdict is recorded on the strength of it");

   /* A sensor that permits nothing fails FAST and VISIBLY. */
   reset();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.have      = 1;
   g_cal.permitted = 0;
   calib_queue(7, 120);
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 0, "a factory-calibrated sensor is never written to");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_NOTSUP, "...and says NOT SUPPORTED at once");
   ck(calib_queued_for(7) == 0, "...without waiting out the whole window");

   /* ---- a queue that outlived its window ---- */
   /* Fields: id,mgdl,t,last_mgdl,last_t,last_state,last_id */
   reset();
   (void)snprintf(line, sizeof line, "7,120,%ld,0,0,0,0\n",
                  now - CALQ_WINDOW_S - 60);
   put("cal.q", line);
   calib_load();
   ck(calib_queued_for(7) == 0, "a calibration older than the window is over");
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_FAILED,
      "...and reported FAILED, never silently forgotten");

   /* One still inside its window resumes and keeps trying. */
   reset();
   (void)snprintf(line, sizeof line, "7,120,%ld,0,0,0,0\n", now - 60);
   put("cal.q", line);
   calib_load();
   ck(calib_queued_for(7) == 120,
      "a fresh calibration survives a restart and stays queued");

   /* ---- THE MIRROR CASE: A DISCARD THAT WOULD BE WRONG ------------------
    *
    * Every case above is about a reply that must NOT be applied. This is the
    * one where refusing it would be the bug, and it is what stops the rule
    * from being satisfied by simply discarding everything.
    *
    * A queue that could not be SAVED did not happen: calib.h's transaction
    * rule puts the previous entry back, exactly as it was. So if the user
    * queues 180 over a 100 that is already on the wire and the file will not
    * write, the 100 is live again -- and the write outstanding for it is still
    * the write outstanding for the live entry. Its reply must resolve.
    *
    * That only works if the ROLLBACK restores the generation with the rest of
    * the queue. Restore the value and drop the identity and the sensor's answer
    * to a calibration that really is queued is thrown away: the row says
    * PENDING for the rest of the window and then FAILED, on a value the sensor
    * accepted twenty minutes earlier. */
   reset();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.have = g_cal.permitted = 1;
   ck(calib_queue(7, 100) == CALIB_OK, "100 is queued");
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1 && g_sent == 100, "...and written");
   int r_id       = g_sent_id;
   int r_mgdl     = g_sent;
   unsigned r_gen = g_sent_gen;
   setenv("APP_FAIL_RENAME", "1", 1);
   ck(calib_queue(7, 180) == CALIB_UNSAVED,
      "a replacement that cannot be written is refused");
   unsetenv("APP_FAIL_RENAME");
   ck(calib_queued_for(7) == 100, "...and 100 is live again, unchanged");
   pancra_cal_result(0, r_id, r_mgdl, r_gen);
   calib_view(7, &v);
   ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 100,
      "the reply to the write that is STILL outstanding resolves it -- a "
      "rolled-back queue keeps its identity");
   ck(calib_queued_for(7) == 0, "...and the queue is empty");

   /* ---- A LOAD DEFINES THE STATE, SO IT ALSO DEFINES THE IDENTITY -------
    *
    * The queue that comes off the disk is a NEW entry, even when the file says
    * exactly what was queued a moment ago. It has to be: a reply is owed to
    * the entry that was live when the write went out, and a load replaces the
    * live entry wholesale -- that is the one moment where "the same value for
    * the same sensor" is emphatically NOT the same calibration.
    *
    * The generation is deliberately not persisted (a counter from a previous
    * process means nothing here), so this is the case that says a restored
    * entry gets a fresh one rather than the 0 that "nothing is queued" uses.
    * Without it, two loads either side of a send would hand the same identity
    * to two different entries and the stale reply would resolve the new one --
    * the defect this whole token exists to prevent, reached by the loader.
    *
    * The token of the write is captured BEFORE the second load, which is the
    * only order that proves anything: read afterwards it would be whatever the
    * implementation had just decided, which is always self-consistent. */
   reset();
   memset(&g_cal, 0, sizeof g_cal);
   g_cal.have = g_cal.permitted = 1;
   (void)snprintf(line, sizeof line, "7,120,%ld,0,0,0,0\n", now - 60);
   put("cal.q", line);
   calib_load();
   g_nsent = 0;
   calib_try(LINK_CGM, 7);
   ck(g_nsent == 1 && g_sent == 120, "a resumed calibration is written");
   int l_id       = g_sent_id;
   int l_mgdl     = g_sent;
   unsigned l_gen = g_sent_gen;
   calib_load(); /* the state is defined again, from the same file */
   ck(calib_queued_for(7) == 120, "...and the reload puts it back");
   pancra_cal_result(0, l_id, l_mgdl, l_gen);
   ck(calib_queued_for(7) == 120,
      "a reply owed to the entry a RELOAD replaced does not resolve the one "
      "that came off the disk");
   calib_view(7, &v);
   ck(v.last_state != CAL_ST_APPLIED,
      "...and records no verdict against it either");

   /* ---- the row shows one thing at a time ---- */
   reset();
   (void)snprintf(line, sizeof line, "0,0,0,96,%ld,%d,7\n", now - 300,
                  CAL_ST_APPLIED);
   put("cal.q", line);
   calib_load();
   calib_view(7, &v);
   ck(v.last_mgdl == 96 && v.last_state == CAL_ST_APPLIED,
      "with nothing queued the row shows the last result");
   calib_queue(7, 130);
   calib_view(7, &v);
   ck(v.queued_mgdl == 130, "once a new one is queued the row shows that");
   ck(v.last_mgdl == 0, "...and NOT the superseded result beside it");

   reset();
   /* EVERY PATH OUT, not just the ones the cases above happened to take. The
    * early return -- nothing queued, or queued for a different sensor -- is
    * the one a hand-written unlock forgets, and a driver lock left held
    * wedges every GATT callback in the process. The module cannot forget any
    * more (the driver runs its operations inside its own lock), so what this
    * pins is that the module never re-enters the driver from inside one --
    * which would be the same wedge from the other direction. */
   calib_try(0, 999999); /* no calibration queued for that sensor */
   calib_try(0, 0);
   ck(g_lockdepth == 0, "every operation returned to the driver");
   ck(g_maxdepth == 1, "...and none of them re-entered it while inside one");

   printf("== a transition that could not be SAVED did not happen ==\n");
   /* THE MOST CONSEQUENTIAL WRITE THIS APP MAKES. A calibration rewrites
    * every reading the sensor produces afterwards, so "queued" has to mean
    * queued -- on the disk, where the next launch will find it. Both files
    * are replaced by rename, so a failed write leaves the previous file
    * whole; the honest answer is to put the state back and say nothing
    * happened. Reported as success with memory mutated, the screen showed a
    * calibration queued that was gone at the next launch, and a rescale
    * turned OFF that came back on and went on scaling every reading. */
   {
      reset();
      ck(calib_queue(7, 120) == CALIB_OK, "a calibration queues");
      ck(calib_queued_for(7) == 120, "...and is the one queued");

      setenv("APP_FAIL_RENAME", "1", 1); /* every replace fails from here */

      ck(calib_queue(7, 150) != CALIB_OK,
         "a queue that cannot be written says so");
      ck(calib_queued_for(7) == 120,
         "...and the calibration in MEMORY is the one on disk, not the new "
         "one");
      ck(calib_cancel() != CALIB_OK, "a cancel that cannot be written says so");
      ck(calib_queued_for(7) == 120, "...and the queue is still there, as the "
                                     "file still says");

      ck(calib_rescale_set(7, 200, 210) != CALIB_OK,
         "a rescale that cannot be written says so");
      ck(!calib_rescale_engaged(7), "...and no factor was left engaged");
      unsetenv("APP_FAIL_RENAME");
      ck(calib_rescale_set(7, 200, 210) == CALIB_OK,
         "with the filesystem working the same rescale commits");
      ck(calib_rescale_engaged(7), "...and now it is engaged");
      setenv("APP_FAIL_RENAME", "1", 1);
      ck(calib_rescale_stop() != CALIB_OK,
         "a STOP that cannot be written says so");
      ck(calib_rescale_engaged(7),
         "...and the factor is still on, which is what the file says -- the "
         "opposite would scale every reading after a restart with the screen "
         "saying it was off");
      unsetenv("APP_FAIL_RENAME");
      ck(calib_rescale_stop() == CALIB_OK, "and stops when it can be written");
      ck(!calib_rescale_engaged(7), "...for good");
   }

   printf("== a state file that cannot be READ is not a fresh install ==\n");
   /* An absent file and an unreadable one used to be the same answer, so a
    * calibration the user confirmed, or a rescale factor scaling every
    * reading from a sensor, could vanish at a restart and leave the app
    * looking exactly like a phone that had never had either. */
   {
      reset();
      ck(calib_load() == CALIB_OK, "two readable files load whole");

      char p[256];
      (void)snprintf(p, sizeof p, "%s/cal.q", g_dir);
      (void)remove(p);
      (void)snprintf(p, sizeof p, "%s/rescale.cfg", g_dir);
      (void)remove(p);
      ck(calib_load() == CALIB_OK,
         "a first run -- neither file present -- is not a failure");

      /* A DIRECTORY where the file should be: open succeeds, read fails with
       * EISDIR. A real syscall failure rather than a mock. */
      (void)snprintf(p, sizeof p, "%s/cal.q", g_dir);
      ck(mkdir(p, 0777) == 0, "the queue's path is made unreadable");
      ck(calib_load() != CALIB_OK,
         "a queue that EXISTS and cannot be read is reported, not taken for "
         "a fresh install");
      (void)rmdir(p);

      (void)snprintf(p, sizeof p, "%s/rescale.cfg", g_dir);
      ck(mkdir(p, 0777) == 0, "...and the rescale's path");
      ck(calib_load() != CALIB_OK, "a rescale factor lost this way is "
                                   "reported too");
      (void)rmdir(p);
      reset();
   }

   /* ---- THE QUEUE AND LAST CAL ARE ONE RECORD --------------------------
    *
    * They share one line in one file and were under two different locks: the
    * queue behind the driver's, the verdict behind cal_lk. So a save could
    * serialise a queue from one instant beside a verdict from another, and
    * calq_load -- which installs a whole record in one go --
    * could be seen halfway through.
    *
    * The property: a device row NEVER shows a live queued value and a
    * last-resolved verdict for the same sensor at once, and never shows a
    * verdict older than the resolution that replaced it. */
   reset();
   qrace_run();
   ck(atomic_load(&g_qtorn) == 0,
      "the queue and its verdict are never seen half-swapped");
   ck(atomic_load(&g_qseen) > 0,
      "...and the reader really did see both states");
   reset();

   /* ---- TWO THREADS, ONE FACTOR ------------------------------------- */
   /* A rescale factor is not one number: it is (permille, sensor id,
    * effective-from), written field by field. The user sets one on the MAIN
    * thread while the reading path reads it on a BINDER thread for every
    * sample that arrives, and the renderer reads it again for every frame --
    * so a reader between the permille and the id got the NEW factor with the
    * OLD sensor's id, which is one sensor's correction applied to another
    * sensor's stored readings.
    *
    * The two setters below activate for DIFFERENT sensors with DIFFERENT
    * targets, so each sensor has exactly one factor it can legitimately
    * carry. A view of sensor A that reports anything other than A's factor
    * or none at all saw a transition half-done. */
   reset();
   race_run();
   ck(atomic_load(&g_torn) == 0,
      "a rescale transition is never seen half-applied");
   ck(atomic_load(&g_seen) > 0,
      "...and the readers really did see active factors");
   reset();

   printf("== an AUTOMATIC transition stands, says so, and is retried ==\n");
   /* THE OTHER HALF OF "a transition that could not be SAVED did not
    * happen". A user-driven change is undone when the write fails: the input
    * can be given again, so nothing happening is the honest answer.
    *
    * An automatic one cannot be undone. A rescale activation is computed from
    * ONE SAMPLE that will not come again; an expiry is a deadline already
    * passed; ACCEPTED and REJECTED are the sensor's answer, and there is no
    * way to ask it twice. Rolling any of them back would leave a pending
    * target for the NEXT reading to activate from a different raw -- a
    * different factor, silently -- or resend a value the sensor has already
    * answered.
    *
    * So the transition stands, `unsaved` says the file is behind, the service
    * tick retries the write, and a restart after the retry finds the state
    * the screen was showing. Each case below is checked all the way to the
    * RESTART, because "it was displayed" is not the property that matters. */
   {
      /* --- ACTIVATION: the factor computed from a sample --- */
      reset();
      calib_rescale_set(7, 0, 110); /* raw 0: held, awaiting a reading */
      calib_view(7, &v);
      ck(v.rescale_pending == 110, "a target is held awaiting a reading");
      setenv("APP_FAIL_RENAME", "1", 1);
      int pm4 = 0;
      (void)calib_on_reading(7, 0, now, 100, &pm4, NULL);
      ck(pm4 == 1100, "the sample activates the factor even though the write "
                      "fails -- that sample does not come again");
      calib_view(7, &v);
      ck(v.rescale_pm == 1100, "...the factor is live");
      ck(v.rescale_unsaved == 1, "...and the row says it is NOT on the disk");
      ck(v.cal_unsaved == 0,
         "...while the calibration file, which nothing wrote, is not blamed");
      /* The retry, still failing, keeps saying so rather than forgetting. */
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_unsaved == 1 && v.rescale_pm == 1100,
         "a retry that also fails changes nothing and still says so");
      unsetenv("APP_FAIL_RENAME");
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_unsaved == 0, "the first retry that can write clears it");
      calib_load(); /* what a restart does */
      ck(calib_rescale_pm() == 1100,
         "...and the restart finds the factor the screen was showing");

      /* --- EXPIRY: a deadline that has already passed --- */
      reset();
      calib_rescale_set(7, 0, 130);
      /* Past the window in ELAPSED time -- see the stale-reference case above
       * for why the reading's own timestamp is not what decides this. */
      clock_advance(RESCALE_PEND_WINDOW_S + 1);
      setenv("APP_FAIL_RENAME", "1", 1);
      (void)calib_on_reading(7, 0, now, 100, &pm4, NULL);
      calib_view(7, &v);
      ck(v.rescale_expired == 1, "a stale target EXPIRES, write or no write");
      ck(v.rescale_pending == 0, "...and is not left looking live");
      ck(v.rescale_unsaved == 1, "...with the file behind, visibly");
      unsetenv("APP_FAIL_RENAME");
      calib_tick();
      calib_load();
      calib_view(7, &v);
      ck(v.rescale_pending == 0,
         "after the retry a restart does not resurrect the expired target");

      /* --- ACCEPTED: the sensor's own answer --- */
      reset();
      ck(calib_queue(7, 120) == CALIB_OK, "a calibration queues");
      g_cal.have = g_cal.permitted = 1;
      g_nsent                      = 0;
      calib_try(LINK_CGM, 7);
      setenv("APP_FAIL_RENAME", "1", 1);
      sensor_answers(0);
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 120,
         "an ACCEPTED verdict is recorded even when it cannot be written");
      ck(v.queued_mgdl == 0,
         "...and the queue is gone: the sensor cannot be asked twice");
      ck(v.cal_unsaved == 1, "...with the row saying it is not on the disk");
      unsetenv("APP_FAIL_RENAME");
      calib_tick();
      calib_view(7, &v);
      ck(v.cal_unsaved == 0, "the retry writes it");
      calib_load();
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_APPLIED && v.last_mgdl == 120 &&
             v.queued_mgdl == 0,
         "...and a restart agrees with the screen: APPLIED, nothing queued");

      /* --- REJECTED: the same, and it must not be resent --- */
      reset();
      calib_queue(7, 300);
      g_cal.have = g_cal.permitted = 1;
      g_nsent                      = 0;
      calib_try(LINK_CGM, 7);
      setenv("APP_FAIL_RENAME", "1", 1);
      sensor_answers(3);
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_REJECTED,
         "a REJECTED verdict is recorded even when it cannot be written");
      ck(calib_queued_for(7) == 0, "...and is NOT left queued to be resent");
      ck(v.cal_unsaved == 1, "...with the file behind");
      g_nsent = 0;
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 0, "...and nothing is written to the sensor again");
      unsetenv("APP_FAIL_RENAME");
      calib_tick();
      calib_load();
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_REJECTED && v.queued_mgdl == 0,
         "after the retry the restart still says REJECTED, still not queued");

      /* --- THE CLOCK-DRIVEN EXPIRY, which is a different code path ---
       *
       * calib_on_reading expires a stale target when a reading finally
       * arrives; calib_tick expires one when NO reading ever does, which is
       * the case a sensor that has gone quiet produces and the one a user
       * actually waits through. It has its own save, and its own way of
       * discarding a failure.
       *
       * Reaching it needs a pending target older than the window, and the
       * test cannot move the clock -- but the LOAD honours a target at
       * exactly the window (`<=`) while the TICK expires past it (`>`), so a
       * target stamped exactly one window ago survives the load and is stale
       * to the very next tick a second later. */
      reset();
      {
         char line[96];
         long stamp = now - RESCALE_PEND_WINDOW_S;
         (void)snprintf(line, sizeof line, "0,1000,0,%d,%d,%ld\n", 7, 130,
                        stamp);
         put("rescale.cfg", line);
         calib_load();
         calib_view(7, &v);
         ck(v.rescale_pending == 130,
            "a target exactly at the window survives a restart");
         /* One second, so the tick sees it as PAST the window. It used to be
          * a spin on the real clock -- a whole second of CPU, in a gate that
          * runs this file twice more under two sanitisers. */
         clock_advance(1);
         setenv("APP_FAIL_RENAME", "1", 1);
         calib_tick();
         calib_view(7, &v);
         ck(v.rescale_expired == 1 && v.rescale_pending == 0,
            "the tick expires a target no reading ever came for");
         ck(v.rescale_unsaved == 1,
            "...and an expiry it could not write says so, like every other "
            "automatic transition");
         unsetenv("APP_FAIL_RENAME");
         calib_tick();
         calib_view(7, &v);
         ck(v.rescale_unsaved == 0, "...until the retry lands it");
         calib_load();
         calib_view(7, &v);
         ck(v.rescale_pending == 0,
            "...and the restart does not resurrect the target");
      }

      /* --- THE THREE REMAINING AUTOMATIC RESOLVES ---
       *
       * ACCEPTED and REJECTED are covered above. A queue can also end
       * without the sensor saying anything: it can be refused outright (the
       * sensor does not permit calibration), or the window can lapse. Both
       * are resolutions the user cannot re-enter and neither may be rolled
       * back -- a queue restored by a failed write is a value that goes on
       * being sent to a sensor that has already refused it. */
      reset();
      calib_queue(7, 120);
      memset(&g_cal, 0, sizeof g_cal);
      g_cal.have      = 1; /* the sensor answered... */
      g_cal.permitted = 0; /* ...and it does not permit calibration */
      setenv("APP_FAIL_RENAME", "1", 1);
      g_nsent = 0;
      calib_try(LINK_CGM, 7);
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_NOTSUP,
         "a sensor that refuses calibration resolves the queue, write or no "
         "write");
      ck(calib_queued_for(7) == 0, "...and the queue is gone, not restored");
      ck(v.cal_unsaved == 1, "...with the file behind, visibly");
      ck(g_nsent == 0, "...and nothing was sent to the sensor");
      unsetenv("APP_FAIL_RENAME");
      calib_tick();
      calib_load();
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_NOTSUP && v.queued_mgdl == 0,
         "after the retry the restart agrees: NOT SUPPORTED, nothing queued");

      /* THE WINDOW LAPSING, which the tick decides. A queue stamped a full
       * window ago is loaded and then given up on. */
      reset();
      {
         char line[96];
         long stamp = now - CALQ_WINDOW_S - 1;
         (void)snprintf(line, sizeof line, "%d,%d,%ld,0,0,0,0\n", 7, 140,
                        stamp);
         put("cal.q", line);
         setenv("APP_FAIL_RENAME", "1", 1);
         calib_load();
         calib_view(7, &v);
         ck(v.last_state == CAL_ST_FAILED,
            "a queue older than its window is given up on at load");
         ck(v.queued_mgdl == 0, "...and is not left to be resent forever");
         ck(v.cal_unsaved == 1, "...with the verdict not yet on the disk");
         unsetenv("APP_FAIL_RENAME");
         calib_tick();
         calib_load();
         calib_view(7, &v);
         ck(v.last_state == CAL_ST_FAILED && v.queued_mgdl == 0,
            "...and the retry makes the restart agree");
      }

      /* ...and the same lapse decided by the TICK rather than by the load,
       * which is a second code path with its own save. Exactly at the window
       * the load keeps the queue (`>` is false); one second later the tick
       * gives up on it. */
      reset();
      {
         char line[96];
         long stamp = now - CALQ_WINDOW_S;
         (void)snprintf(line, sizeof line, "%d,%d,%ld,0,0,0,0\n", 7, 145,
                        stamp);
         put("cal.q", line);
         calib_load();
         ck(calib_queued_for(7) == 145,
            "a queue exactly at its window survives a restart");
         clock_advance(1); /* see above: one second, on both clocks */
         setenv("APP_FAIL_RENAME", "1", 1);
         calib_tick();
         calib_view(7, &v);
         ck(v.last_state == CAL_ST_FAILED && v.last_mgdl == 145,
            "the tick gives up on a queue the sensor never answered");
         ck(calib_queued_for(7) == 0, "...and does not restore it on a failed "
                                      "write, which would resend it forever");
         ck(v.cal_unsaved == 1, "...saying the verdict is not on the disk");
         unsetenv("APP_FAIL_RENAME");
         calib_tick();
         calib_load();
         calib_view(7, &v);
         ck(v.last_state == CAL_ST_FAILED && v.queued_mgdl == 0,
            "...and the retry makes the restart agree");
      }

      /* --- AN UNREADABLE FILE DEFINES NOTHING ---
       *
       * The three-way answer (READ_NONE / READ_OK / READ_FAIL) exists so an
       * unreadable file is not mistaken for a first run. The loader was
       * applying it backwards: it wiped all eight fields and only then
       * looked at the answer, so a cal.q that could not be read destroyed a
       * live queue and a live verdict -- and a rescale.cfg that could not be
       * read turned off a factor that was scaling every reading. */
      reset();
      calib_queue(7, 118);
      calib_rescale_set(7, 100, 110);
      ck(calib_queued_for(7) == 118 && calib_rescale_pm() == 1100,
         "a queue and a factor are live");
      {
         char p2[256];
         (void)snprintf(p2, sizeof p2, "%s/cal.q", g_dir);
         ck(chmod(p2, 0) == 0, "cal.q is made unreadable");
         (void)snprintf(p2, sizeof p2, "%s/rescale.cfg", g_dir);
         ck(chmod(p2, 0) == 0, "...and so is rescale.cfg");
         ck(calib_load() != CALIB_OK, "the load reports the loss");
         ck(calib_queued_for(7) == 118,
            "...and the queue it could not read is still there");
         ck(calib_rescale_pm() == 1100,
            "...and the factor still scaling, as the unread files still say");
         (void)snprintf(p2, sizeof p2, "%s/cal.q", g_dir);
         (void)chmod(p2, 0600);
         (void)snprintf(p2, sizeof p2, "%s/rescale.cfg", g_dir);
         (void)chmod(p2, 0600);
      }

      /* --- A LOAD DEFINES THE STATE, so it cannot leave a stale retry --- */
      reset();
      calib_rescale_set(7, 0, 110);
      setenv("APP_FAIL_RENAME", "1", 1);
      (void)calib_on_reading(7, 0, now, 100, &pm4, NULL);
      calib_view(7, &v);
      ck(v.rescale_unsaved == 1, "an unsaved factor is pending a retry");
      unsetenv("APP_FAIL_RENAME");
      reset(); /* which loads both files afresh */
      calib_view(7, &v);
      ck(v.rescale_unsaved == 0 && v.cal_unsaved == 0,
         "a load clears the retry: writing it back would undo the load");
      ck(calib_rescale_pm() == 1000,
         "...and the state is the file's, not the one that never landed");
   }
   reset();

   printf("== A WALL-CLOCK JUMP DECIDES NOTHING ==\n");
   /* THE DEFECT, in the words of the person holding the phone: they take a
    * fingerstick, type 118, confirm, and the row says PENDING. Then the phone
    * finds a network, or crosses a date change, or they fix the clock by hand.
    *
    * Forward, every window in this module was over at once: the next tick
    * declared the calibration FAILED -- seconds after they entered it, on a
    * sensor that was streaming perfectly -- and a pending rescale expired
    * before the reading it was waiting for could arrive. Backward, the mirror
    * and the quieter half: every difference went negative, so the give-up
    * window never lapsed, the one retry that would have got the value into the
    * sensor was never made, and the row said PENDING until the process died.
    * Either way the number the user typed never reached the sensor.
    *
    * Every case below moves the two clocks INDEPENDENTLY, which is exactly
    * what a correction does and what no amount of waiting can simulate.
    *
    * THE MUTATING CALL IS ALWAYS ON ITS OWN LINE, never folded into the same
    * ck() as the state it changes: C does not order the evaluation of a call's
    * arguments, so `ck(tick(), queued_for(7) == 120)` may read the state
    * BEFORE the tick and pass whatever the tick does. */
   {
      /* --- THE QUEUE'S GIVE-UP WINDOW --- */
      reset();
      ck(calib_queue(7, 118) == CALIB_OK, "a calibration is confirmed");
      rt_jump(CALQ_WINDOW_S * 3); /* an NTP step of an hour; no time passes */
      calib_tick();
      ck(calib_queued_for(7) == 118,
         "a wall clock that jumps FORWARD past the window does not expire a "
         "calibration the user just entered");
      calib_view(7, &v);
      ck(v.last_state != CAL_ST_FAILED,
         "...and the row does not report a sensor refusal that never happened");
      /* ...and the window still ends, on elapsed time, exactly where it did. */
      mono_advance(CALQ_WINDOW_S);
      calib_tick();
      ck(calib_queued_for(7) == 118,
         "AT the window it is still queued (the boundary is unmoved)");
      mono_advance(1);
      calib_tick();
      ck(calib_queued_for(7) == 0, "...and one second past it, it is over");
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_FAILED, "...reported FAILED, visibly");

      /* BACKWARD: the window must still end. This is the "indefinitely
       * postponed" half -- with realtime arithmetic `now - g_calq_t` stays
       * negative for the whole length of the correction, so the queue is
       * retried for ever and a fingerstick from an hour ago goes on being
       * offered to the sensor. */
      reset();
      calib_queue(7, 119);
      rt_jump(-(CALQ_WINDOW_S * 3));
      mono_advance(CALQ_WINDOW_S + 1);
      calib_tick();
      ck(calib_queued_for(7) == 0,
         "a wall clock that jumps BACKWARD does not postpone the give-up: the "
         "window is elapsed time");
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_FAILED, "...and it still reports FAILED");

      /* --- THE 0x34 RESEND THROTTLE --- */
      reset();
      calib_queue(7, 120);
      memset(&g_cal, 0, sizeof g_cal);
      g_cal.have = g_cal.permitted = 1;
      g_nsent                      = 0;
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 1, "the first attempt goes out at once");
      rt_jump(3600);
      calib_try(LINK_CGM, 7);
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 1,
         "a forward jump does not let the sensor be written to again: the "
         "throttle is a minute of ELAPSED time, not of wall time");
      mono_advance(59);
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 1, "...still within the minute");
      mono_advance(1);
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 2, "...and at the minute the retry is allowed");

      /* BACKWARD, which is the one that loses the calibration outright. The
       * old gate subtracted a wall-clock stamp of the last send from a fresh
       * `realtime_s()` and refused while the difference was under sixty; an
       * hour backwards makes it negative for an hour, so the retry never
       * happens and the queue lapses having been written to the sensor exactly
       * once. */
      reset();
      calib_queue(7, 121);
      g_nsent = 0;
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 1, "one attempt has gone out");
      rt_jump(-3600);
      mono_advance(60);
      calib_try(LINK_CGM, 7);
      ck(g_nsent == 2,
         "a backward jump does not silence the retry for the length of the "
         "correction");

      /* --- THE 0x32 PERMISSION PROBE --- */
      reset();
      calib_queue(7, 122);
      memset(&g_cal, 0, sizeof g_cal); /* the sensor has never answered */
      g_cal.result = -1;
      g_nprobe     = 0;
      calib_try(LINK_CGM, 7);
      ck(g_nprobe == 1, "a permission probe goes out");
      rt_jump(3600);
      calib_try(LINK_CGM, 7);
      calib_try(LINK_CGM, 7);
      ck(g_nprobe == 1,
         "a forward jump does not hammer a sensor that is declining to answer");
      rt_jump(-7200);
      mono_advance(60);
      calib_try(LINK_CGM, 7);
      ck(g_nprobe == 2,
         "...and a backward jump does not silence the probe either, so the "
         "calibration is not stuck behind a permission it never re-asks for");

      /* --- A PENDING RESCALE'S EXPIRY --- */
      reset();
      calib_rescale_set(7, 0, 110); /* no reading yet: the target is held */
      rt_jump(RESCALE_PEND_WINDOW_S * 3);
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_pending == 110,
         "a forward jump does not expire a rescale target that is still "
         "waiting for its first reading");
      ck(!v.rescale_expired, "...and raises no expiry notice");
      /* The reading path is a second, separate decision, and it used to
       * compare the READING's timestamp against the request's wall clock. */
      started = 0;
      adj     = calib_on_reading(7, 0, now, 100, &pm, &started);
      ck(started && calib_rescale_pm() == 1100,
         "...and the reading it was waiting for still resolves it");
      ck(adj == 110, "...effective on that very reading");

      reset();
      calib_rescale_set(7, 0, 110);
      rt_jump(-(RESCALE_PEND_WINDOW_S * 3));
      mono_advance(RESCALE_PEND_WINDOW_S + 1);
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_pending == 0 && v.rescale_expired,
         "a backward jump does not leave a stale target looking live for ever");

      /* --- RESTART RECONCILIATION: THE PERSISTED AGE ---
       *
       * The one subtraction that must stay on the wall clock, because nothing
       * monotonic survives a reboot. Fields: id,mgdl,t,last_mgdl,last_t,
       * last_state,last_id. */

      /* WHAT IS LEFT, NOT A FRESH WINDOW. A queue stamped so that two minutes
       * of its window remain must have two minutes, or a phone that relaunches
       * the app every few minutes keeps a twenty-minute-old fingerstick queued
       * for ever. */
      reset();
      (void)snprintf(line, sizeof line, "7,123,%ld,0,0,0,0\n",
                     now - (CALQ_WINDOW_S - 120));
      put("cal.q", line);
      calib_load();
      ck(calib_queued_for(7) == 123, "a queue with time left resumes");
      mono_advance(120);
      calib_tick();
      ck(calib_queued_for(7) == 123,
         "...and it is still live at the end of what REMAINED");
      mono_advance(1);
      calib_tick();
      ck(calib_queued_for(7) == 0,
         "...and over one second later: the restart granted the remainder, "
         "not a whole new window");

      /* A NEGATIVE AGE WITHIN THE TOLERANCE: the stamp is in the future by a
       * couple of seconds, which is an NTP nudge and nothing else. The user's
       * work is not thrown away for it. */
      reset();
      (void)snprintf(line, sizeof line, "7,124,%ld,0,0,0,0\n",
                     now + (CLOCK_SKEW_TOL_S / 2));
      put("cal.q", line);
      calib_load();
      ck(calib_queued_for(7) == 124,
         "a stamp a few seconds in the future is a clock nudge, and the "
         "calibration survives it");
      mono_advance(CALQ_WINDOW_S);
      calib_tick();
      ck(calib_queued_for(7) == 124, "...with a full window granted from now");
      mono_advance(1);
      calib_tick();
      ck(calib_queued_for(7) == 0, "...and no more than one window");

      /* A NEGATIVE AGE BEYOND IT: the clock really moved, so how old this
       * fingerstick is cannot be known. It EXPIRES VISIBLY -- the user is
       * told to enter it again -- rather than being pushed into the sensor
       * with an unknowable age. */
      reset();
      (void)snprintf(line, sizeof line, "7,125,%ld,0,0,0,0\n",
                     now + CLOCK_SKEW_TOL_S + 60);
      put("cal.q", line);
      calib_load();
      ck(calib_queued_for(7) == 0,
         "a stamp far in the future is an unknowable age, not a fresh "
         "calibration");
      calib_view(7, &v);
      ck(v.last_state == CAL_ST_FAILED,
         "...and it is reported FAILED, never dropped in silence");

      /* THE SAME TWO ANSWERS FOR A PENDING RESCALE. Fields:
       * id,pm,t,pend_id,pend_mgdl,pend_t */
      reset();
      (void)snprintf(line, sizeof line, "0,1000,0,7,110,%ld\n",
                     now + (CLOCK_SKEW_TOL_S / 2));
      put("rescale.cfg", line);
      calib_load();
      calib_view(7, &v);
      ck(v.rescale_pending == 110,
         "a pending rescale stamped a few seconds ahead survives the nudge");
      mono_advance(RESCALE_PEND_WINDOW_S + 1);
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_pending == 0 && v.rescale_expired,
         "...and still expires one window later, not never");

      reset();
      (void)snprintf(line, sizeof line, "0,1000,0,7,110,%ld\n",
                     now + CLOCK_SKEW_TOL_S + 60);
      put("rescale.cfg", line);
      calib_load();
      calib_view(7, &v);
      ck(v.rescale_pending == 0 && v.rescale_expired,
         "a pending rescale stamped far in the future is EXPIRED, visibly");

      /* AND WHAT IS LEFT, for the rescale too. */
      reset();
      (void)snprintf(line, sizeof line, "0,1000,0,7,110,%ld\n",
                     now - (RESCALE_PEND_WINDOW_S - 60));
      put("rescale.cfg", line);
      calib_load();
      calib_view(7, &v);
      ck(v.rescale_pending == 110, "a pending rescale with a minute left "
                                   "resumes");
      mono_advance(60);
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_pending == 110, "...still live at the end of the remainder");
      mono_advance(1);
      calib_tick();
      calib_view(7, &v);
      ck(v.rescale_pending == 0 && v.rescale_expired,
         "...and expired one second later, on the remainder and not on a "
         "fresh window");
   }
   reset();

   /* THE VERDICT COMES LAST, after every assertion above it.
    *
    * It used to sit before the two lock-depth checks and be followed by an
    * unconditional `return 0`, so a failure in either of them printed FAIL
    * and exited SUCCESSFULLY -- and the gate believed the test. Any
    * assertion added after the verdict would have been decorative in the
    * same way. */
   if (g_fail) {
      printf("calibtest: FAIL (%d assertions passed)\n", g_pass);
      return 1;
   }
   printf(
       "calibtest: calibration queue and rescale factor OK (%d assertions)\n",
       g_pass);
   return 0;
}
