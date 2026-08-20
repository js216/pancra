// SPDX-License-Identifier: GPL-3.0
// remotetest.c --- Host tests for the sync SCHEDULE (see remote.c)
// Copyright 2026 Jakob Kastelic
//
/* WHEN TO TALK TO THE SERVER, which is a decision with two bad extremes and
 * no visible symptom in between.
 *
 * Too often: this code once fired every ten seconds for ever, and an attempt
 * with nothing to send still costs a TLS handshake and a full local pass over
 * every log to hash it. On a phone battery, against a single-core board, that
 * is a slow denial of service against your own server.
 *
 * Too rarely: a failure that is never retried, or a backoff that a wall-clock
 * correction pushed hours into the future, means the phone quietly stops
 * replicating. Nothing on screen says so -- the last successful sync is still
 * displayed, just getting older.
 *
 * None of it was tested. remote.c is linked into the broad test binaries and
 * only ever reached through them by accident, so the backoff, the safety net,
 * the single-flight gate and the freshness stamp had no coverage at all.
 *
 * EVERYTHING THIS FILE NEEDS IS FAKED HERE: the two clocks, the state stamp,
 * the request dispatch into Java, the preferences and the paired identity.
 * That is what makes the schedule testable on a host with no phone -- and it
 * is only possible because remote.c reaches the outside world through those
 * few named functions.
 *
 * Built and run by `make remotetest`.
 */
#include "remote.h"
#include "clock.h"
#include "syncjni.h"
#include "syncreport.h" /* sync_report: this test drives it */
#include "syncstat.h"
#include <pthread.h>
#include <sched.h> /* the single-flight gate has to hold under real threads */
#include <stdatomic.h>
#include <stdint.h> /* uint8_t: the pairing key crosses this boundary */
#include <stdio.h>
#include <string.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- the fakes ---------------------------------------------------------- */

/* THE TWO CLOCKS, separately controllable. That is the point of the pair:
 * realtime identifies instants a user reads, monotonic measures intervals the
 * schedule waits out, and a wall-clock correction must move the first without
 * touching the second. */
/* ATOMIC because the concurrency case below drives them from five threads at
 * once; a plain long written by several threads is a race in the TEST, and a
 * test that is itself undefined proves nothing about the code. */
static _Atomic long g_mono = 1000;
static _Atomic long g_real = 1700000000;

long mono_s(void)
{
   return g_mono;
}

long realtime_s(void)
{
   return g_real;
}

/* The preferences and the identity remote.c consults before doing anything. */
#include "settings.h"

static struct prefs g_prefs;
static struct sync_creds g_creds;

/* The SNAPSHOT readers, which is what remote.c uses now: it takes one copy of
 * the preferences and one of the identity rather than reading five fields
 * that another thread can change between them. */
void settings_get(struct prefs *out)
{
   *out = g_prefs;
}

/* THE PAIR AS ONE VALUE, which is what the scheduler reads now: taken as two
 * separate reads, a push could be aimed at the old server signed as the new
 * account. The stub assembles it from the same two globals the test sets. */
void remote_config_get(struct remote_config *out)
{
   out->on   = g_prefs.remote_on;
   out->port = g_prefs.remote_port;
   for (int i = 0; i < (int)sizeof out->server; i++) {
      out->server[i] = g_prefs.remote_server[i];
      if (!g_prefs.remote_server[i])
         break;
   }
   out->uid = g_creds.uid;
   for (int i = 0; i < (int)sizeof out->key; i++)
      out->key[i] = g_creds.key[i];
   for (int i = 0; i < (int)sizeof out->email; i++) {
      out->email[i] = g_creds.email[i];
      if (!g_creds.email[i])
         break;
   }
}

void sync_creds_get(struct sync_creds *out)
{
   *out = g_creds;
}

static int g_forgot;

int settings_forget_identity(void)
{
   g_forgot++;
   g_creds.uid = 0;
   return SETTINGS_OK; /* this stub's storage cannot fail */
}

#include "sync.h"

static int g_keys_set;

void sync_set_key(long uid, const uint8_t key[SYNC_KEY_LEN])
{
   (void)key;
   g_keys_set++;
   g_creds.uid = uid;
}

/* The state stamp: what remote.c uses to answer "has anything changed since
 * the last attempt". A number the test moves when it wants to pretend a
 * reading was stored. */
static _Atomic long g_stamp = 4242;

long syncjni_state_stamp(void)
{
   return g_stamp;
}

/* THE DISPATCH. Counting these is the whole test: every case below is either
 * "one request happened" or "none did".
 *
 * It also stands in for the WINDOW the single-flight gate exists to close.
 * The real dispatch hands the work to Java's push worker and returns; the
 * batch build -- three read cycles into one SHARED static buffer -- happens
 * after the busy flag is polled and before it is set. So this fake counts how
 * many callers are inside it at once, and the gate's whole job is to keep
 * that at one. */
static atomic_int g_requests;
static atomic_int g_depth;
static atomic_int g_maxdepth;
/* Set to make the dispatch re-enter the scheduler, the way the real one can:
 * the request runs on Java's worker while the 1 Hz tick keeps ticking. */
static int g_reenter;
static int g_reentered_ok;
/* Make Java REFUSE the request, the way it does with no JNIEnv, no registered
 * class, no resolved method, or a throw from Ble.syncSoon. */
static int g_refuse;
/* Land a sync_report from "Java's worker" INSIDE the request call -- i.e.
 * between the scheduler's claim and its rollback, which is the only window
 * where the rollback's "is this still the value I wrote?" guards matter. */
static int g_report_inside;

int syncjni_sync_request(void)
{
   atomic_fetch_add(&g_requests, 1);
   int d = atomic_fetch_add(&g_depth, 1) + 1;
   int m = atomic_load(&g_maxdepth);
   while (d > m && !atomic_compare_exchange_weak(&g_maxdepth, &m, d))
      m = atomic_load(&g_maxdepth);
   if (g_reenter) {
      /* ONCE. Without the gate the second entrant would satisfy the schedule
       * for a third, and so on until the stack ran out -- a crash rather than
       * a reported failure, which is a worse test even though it is a louder
       * one. */
      g_reenter = 0;
      /* Everything the SCHEDULE would refuse on is satisfied first -- the
       * floor has expired and there is news to send -- so that the only thing
       * left to stop the second entrant is the gate itself. */
      g_mono += 3600;
      g_stamp++;
      int before = atomic_load(&g_requests);
      pancra_remote_sync();
      g_reentered_ok = (atomic_load(&g_requests) == before);
   }
   if (g_report_inside) {
      g_report_inside = 0;
      sync_report(
          SYNC_TIMEOUT); /* a previous attempt's failure, arriving now */
   }
   atomic_fetch_sub(&g_depth, 1);
   return !g_refuse;
}

/* THE STATUS IS THE MODULE'S OWN NOW, so this reads it back through the same
 * accessors the frame uses rather than stubbing the recorder. It moved here
 * from model.c: what happened to a sync belongs to the module that performs
 * it, and having the frame builder hold it made remote.c depend on the
 * screen. */
#define g_last_outcome remote_outcome()
#define g_last_ok      remote_ok_time()

static int g_dirty;

void shell_ui_dirty(void)
{
   g_dirty++;
}

/* ---- helpers ------------------------------------------------------------ */

/* A configured, paired phone with pushing switched on. */
/* remote.c's own floor between attempts (REMOTE_MIN_GAP). Restated here
 * because it is private to that file; remotetest asserts the schedule from
 * outside, so it has to name the interval it expects. */
#define MIN_GAP 60L
/* remote.c's REMOTE_SAFETY, likewise private to that file. */
#define REMOTE_SAFETY_S (6L * 3600)

static void configured(void)
{
   memset(&g_prefs, 0, sizeof g_prefs);
   g_prefs.remote_on   = 1;
   g_prefs.remote_port = 443;
   memcpy(g_prefs.remote_server, "sync.example", 13);
   g_creds.uid = 7;
}

/* Time passes, something changed, and the tick fires. Returns 1 if a request
 * went out. */
static int tick_after(long secs, int changed)
{
   g_mono += secs;
   g_real += secs;
   if (changed)
      g_stamp++;
   int before = atomic_load(&g_requests);
   pancra_remote_sync();
   return atomic_load(&g_requests) > before;
}

/* THE BASELINE every timing case starts from: a sync that has just happened.
 * Without it a case inherits whatever deadline the previous one left behind,
 * and a test that measures "how long until the next attempt" would be
 * measuring the wrong interval -- which is exactly the class of defect it is
 * here to catch. */
static void baseline(void)
{
   configured();
   remote_retry_now();
   int before = atomic_load(&g_requests);
   pancra_remote_sync();
   if (atomic_load(&g_requests) != before + 1)
      ck(0, "the baseline sync fired");
}

/* Advance the clock by `secs` with something new to send, and report whether
 * an attempt went out. Cumulative on purpose: a real phone's schedule is a
 * sequence, and each attempt moves the next deadline. */
static int at(long secs)
{
   g_mono += secs;
   g_real += secs;
   g_stamp++;
   int before = atomic_load(&g_requests);
   pancra_remote_sync();
   return atomic_load(&g_requests) > before;
}

/* The deadline a failure just set, measured as "not yet, then yes". Two
 * probes rather than a search, because a probe that fires MOVES the schedule
 * -- a bisection over it measures its own footprints. */
static void waits_exactly(long secs, const char *what)
{
   ck(!at(secs - 1), what);
   ck(at(1), "...and is tried the moment it expires");
}

/* ---- the concurrent case ------------------------------------------------ */

/* ATOMIC, not volatile. `volatile` orders nothing between threads and is not
 * a synchronisation primitive in C: a stop flag written by one thread and
 * read by another through a plain volatile is a data race, and a concurrency
 * test whose own fixtures are undefined behaviour is not evidence about the
 * code under test. */
static atomic_int g_race_stop;
/* How many rounds each side actually completed, so the test can show that the
 * two really overlapped rather than merely ran one after the other. */
static atomic_long g_ticker_rounds;

static void *ticker(void *p)
{
   (void)p;
   while (!atomic_load_explicit(&g_race_stop, memory_order_relaxed)) {
      g_stamp++;
      pancra_remote_sync();
      atomic_fetch_add_explicit(&g_ticker_rounds, 1, memory_order_relaxed);
   }
   return 0;
}

int main(void)
{
   printf("== nothing to say, nothing sent ==\n");
   {
      baseline();
      /* THE TEN-SECOND LOOP, which is what this rule replaced. An attempt
       * with nothing to send still costs a handshake and a full hash of every
       * log, so a quiet phone must send NOTHING -- for hours, not for a
       * cycle. */
      int before = atomic_load(&g_requests);
      for (int i = 0; i < 4000; i++) {
         g_mono++;
         g_real++;
         pancra_remote_sync();
      }
      ck(atomic_load(&g_requests) == before,
         "a phone with nothing new sends nothing, for "
         "an hour of ticks");

      /* And the floor is a RATE LIMIT, not a freshness test: news inside the
       * minimum gap waits its turn rather than going out at once. */
      baseline();
      ck(!at(1), "a change one second after a sync waits");
      ck(!at(58), "...and is still waiting a second before the gap is up");
      ck(at(1), "...and goes out the moment it is");
   }

   printf("== who is allowed to sync at all ==\n");
   {
      configured();
      g_prefs.remote_on = 0;
      ck(!tick_after(3600, 1), "pushing switched off sends nothing");
      configured();
      g_prefs.remote_server[0] = 0;
      ck(!tick_after(3600, 1), "no server configured sends nothing");
      configured();
      g_creds.uid = 0;
      ck(!tick_after(3600, 1), "an unpaired phone sends nothing");
      configured();
      ck(tick_after(3600, 1), "a configured, paired phone with news sends");
   }

   printf("== a request JAVA NEVER TOOK is not a sync that happened ==\n");
   {
      /* syncjni_sync_request can drop the call outright: no JNIEnv on a
       * thread that never attached, no class before registration, no method
       * if the lookup failed, or a throw from Ble.syncSoon. None of those
       * starts the worker, so NO sync_report() is ever coming.
       *
       * The scheduler used to advance its stamp and both deadlines before
       * asking, and keep them when the ask was refused -- so the phone
       * believed a sync was under way and did not try again until the SIX
       * HOUR safety interval expired. On a phone whose activity had been
       * destroyed, that is the whole failure: sync silently stops for the
       * rest of the day. */
      baseline();
      g_refuse = 1;
      /* The floor is served and there is news, so the only thing that can
       * stop the next attempt is the scheduler's own state. */
      ck(tick_after(MIN_GAP + 1, 1), "a refused request is still made");
      /* ...and because it was refused, the very next eligible tick must try
       * again -- not in six hours. */
      ck(tick_after(MIN_GAP + 1, 0),
         "...and the next attempt comes at the ordinary floor, not the "
         "six-hour safety net");
      ck(tick_after(MIN_GAP + 1, 0),
         "...and again, for as long as Java keeps refusing");
      /* THE FLOOR STILL APPLIES: a refusal must not become a spin. */
      ck(!tick_after(1, 1), "...but never faster than the minimum gap");

      /* And when Java starts accepting again, the stamp is committed, so an
       * unchanged phone goes quiet instead of re-sending the same state. */
      g_refuse = 0;
      ck(tick_after(MIN_GAP + 1, 1), "an accepted request is made");
      ck(!tick_after(MIN_GAP + 1, 0),
         "...and with nothing new, the next tick sends nothing");
      g_refuse = 0;
   }

   printf("== a SAFETY fire that Java refused still retries promptly ==\n");
   {
      /* THE ONE CASE THE STAMP ROLLBACK CANNOT COVER, and the one the
       * six-hour stall actually happens in.
       *
       * When the fire is a SAFETY fire -- nothing new, but the safety
       * interval has elapsed -- the stamp has not changed, so restoring it is
       * a no-op and only the g_rem_safety rollback stands between a refused
       * request and another six hours of silence. Without the safety
       * rollback, this case passes every other assertion in the file. */
      baseline();
      g_refuse = 1;
      ck(tick_after(REMOTE_SAFETY_S + 1, 0),
         "with nothing new, the safety interval still fires");
      g_refuse = 0;
      ck(tick_after(MIN_GAP + 1, 0),
         "...and a refused safety fire is retried at the floor, not six "
         "hours later");
   }

   printf(
       "== a rollback never overwrites a result that arrived meanwhile ==\n");
   {
      /* The scheduler claims, asks Java, and rolls back if Java refused. A
       * sync_report from a PREVIOUS attempt can land in that window, on
       * Java's worker thread, and it knows more than the rollback does: it
       * has just set a real backoff. Restoring blindly would throw that away
       * and retry at the floor against a server already in trouble.
       *
       * The rollback only reverts values it still recognises as its own. */
      baseline();
      g_refuse        = 1;
      g_report_inside = 1;
      (void)tick_after(MIN_GAP + 1, 1);
      /* SYNC_TIMEOUT's first backoff is a minute, so the floor and the
       * backoff coincide; what must NOT happen is the deadline being cleared
       * altogether. One second later is too soon either way. */
      ck(!tick_after(1, 1),
         "a result that landed during the request keeps its own deadline");
      g_refuse = 0;
   }

   printf("== a failing server is asked less and less often ==\n");
   {
      /* Doubling from a minute to a half-hour ceiling. Without a ceiling the
       * retry interval runs away; without doubling it is the ten-second loop
       * again, aimed at a server that is already in trouble. */
      baseline();
      sync_report(SYNC_TIMEOUT);
      waits_exactly(60, "the first retry is a minute away");
      sync_report(SYNC_TIMEOUT);
      waits_exactly(120, "the second is two minutes");
      sync_report(SYNC_TIMEOUT);
      waits_exactly(240, "then four");
      for (int i = 0; i < 12; i++) {
         sync_report(SYNC_TIMEOUT);
         if (i < 11)
            (void)at(30L * 60); /* serve the wait and fail again */
      }
      waits_exactly(30L * 60, "and it stops doubling at half an hour");

      /* SUCCESS RESETS IT. A phone that failed for an hour and then worked
       * must not keep waiting half an hour between syncs. */
      sync_report(SYNC_OK);
      ck(g_last_ok == g_real, "a success stamps WHEN the server accepted "
                              "something -- on the wall clock, because that "
                              "is an instant a user reads");
      ck(at(60), "...and clears the backoff, so the next news waits a minute "
                 "and not half an hour");
   }

   printf("== a failure means we have NOT caught up ==\n");
   {
      /* The freshness stamp is claimed when the attempt starts. If a failure
       * left it claimed, the next tick would decide there was nothing new and
       * skip the retry entirely -- the backoff would expire against a
       * schedule that never fires. */
      baseline();
      sync_report(SYNC_TIMEOUT);
      g_mono += 60;
      g_real += 60;
      int before = atomic_load(&g_requests);
      pancra_remote_sync(); /* NOTHING new since the failed attempt */
      ck(atomic_load(&g_requests) == before + 1,
         "a retry happens even though nothing "
         "changed since the failed attempt");
   }

   printf("== failures only a REPAIR can end ==\n");
   {
      /* A rejected key, a name that does not resolve, a refused certificate.
       * Trying harder cannot help -- so the schedule does not CLIMB, because
       * there is nothing to climb away from; the attempts are not the
       * problem. It does not go to the slowest setting either, which is what
       * it used to do: the repair is often at the OTHER end (a certificate
       * renewed on the server), the phone has to notice it, and half an hour
       * of silence is indistinguishable from a phone that has given up.
       *
       * srv/deploy/README.md tells whoever rotates a certificate that a
       * botched rotation shows up on the phone and a fixed one is picked up
       * "within five minutes" -- this is the code that has to be true of.
       * (That paragraph used to say the phone "does not back off", which was
       * the opposite of what the code then did; it was corrected, and this
       * comment quoted the retired wording for long enough afterwards to be
       * worth saying so.) Five minutes, steady. */
      baseline();
      sync_report(SYNC_AUTH);
      waits_exactly(5L * 60, "a refused key is retried on the fixed "
                             "five-minute schedule, not the slowest one");
      baseline();
      sync_report(SYNC_DNS);
      waits_exactly(5L * 60, "so is a name that does not resolve");
      baseline();
      sync_report(SYNC_TLS);
      waits_exactly(5L * 60, "and a refused certificate");

      /* IT DOES NOT CLIMB. Three failures in a row is still five minutes --
       * the difference between this and the ordinary backoff. */
      baseline();
      sync_report(SYNC_TLS);
      g_mono += 5L * 60;
      pancra_remote_sync();
      sync_report(SYNC_TLS);
      g_mono += 5L * 60;
      pancra_remote_sync();
      sync_report(SYNC_TLS);
      waits_exactly(5L * 60, "...and a third refusal still waits five "
                             "minutes, not twenty");

      /* THE FIX IS IMMEDIATE. A corrected server name that takes half an
       * hour to be tried looks exactly like a name that is still wrong. */
      baseline();
      sync_report(SYNC_DNS);
      remote_retry_now();
      int before = atomic_load(&g_requests);
      pancra_remote_sync(); /* no clock movement, nothing new */
      ck(atomic_load(&g_requests) == before + 1,
         "correcting the setting is tried at once: "
         "backoff and freshness stamp both "
         "cleared");
   }

   printf("== a pairing is followed by a sync, not by a wait ==\n");
   {
      baseline();
      sync_report(SYNC_TIMEOUT); /* earn a backoff first */
      sync_report(SYNC_PAIRED);
      int before = atomic_load(&g_requests);
      pancra_remote_sync();
      ck(atomic_load(&g_requests) == before + 1,
         "a fresh pairing syncs immediately -- the "
         "first thing a user does is look for "
         "their data");
      ck(g_last_outcome == SYNC_PAIRED, "...and the screen says PAIRED");
   }

   printf("== changing the server drops the identity ==\n");
   {
      baseline();
      sync_report(SYNC_TIMEOUT);
      int was = g_forgot;
      remote_forget_cursor();
      ck(g_forgot == was + 1, "the paired identity is forgotten");
      ck(g_keys_set > 0, "...and the stored key with it");
      /* And the new server is asked at once rather than waiting out a backoff
       * the OLD one earned. */
      g_creds.uid = 9;
      int before  = g_requests;
      pancra_remote_sync();
      ck(atomic_load(&g_requests) == before + 1,
         "the new server is tried without serving "
         "the old one's backoff");
   }

   printf("== the six-hour safety look ==\n");
   {
      /* The server could have been restored from an older backup while this
       * phone had nothing new to say. Without this the two would stay out of
       * step until the next reading, which on a phone with the sensor off is
       * indefinitely. */
      baseline();
      sync_report(SYNC_OK);
      g_mono += (6L * 3600) - 2;
      g_real += (6L * 3600) - 2;
      int before = atomic_load(&g_requests);
      pancra_remote_sync(); /* nothing new, and not yet six hours */
      ck(atomic_load(&g_requests) == before, "...does not come early");
      g_mono += 2;
      g_real += 2;
      pancra_remote_sync();
      ck(atomic_load(&g_requests) == before + 1,
         "...and does happen with nothing new at "
         "all");
   }

   printf("== the schedule measures INTERVALS, not wall-clock instants ==\n");
   {
      /* A backoff on the wall clock is a backoff a time correction can
       * cancel or postpone: forward past the safety sync, backward by
       * however far it moved. Both shipped. */
      baseline();
      sync_report(SYNC_TIMEOUT);
      g_real -= 3L * 3600; /* the wall clock jumps BACKWARD three hours */
      g_stamp++;
      int before = atomic_load(&g_requests);
      g_mono += 60;
      pancra_remote_sync();
      ck(atomic_load(&g_requests) == before + 1,
         "a backward wall-clock jump does not "
         "postpone the retry");

      baseline();
      g_real += 12L * 3600; /* ...and FORWARD twelve */
      before = g_requests;
      pancra_remote_sync();
      ck(atomic_load(&g_requests) == before,
         "a forward jump does not skip the minimum gap "
         "either");
   }

   printf("== two threads cannot both be syncing ==\n");
   {
      /* The activity's 1 Hz timer and the service's tick thread both drive
       * this, and the Java-side busy flag does not serialise them: the whole
       * batch build sits between the poll and the flag. Two threads inside
       * that window fill the same shared buffer from different offsets, and
       * the POST carries a spliced line -- a fabricated <epoch> <mg/dL> pair
       * stored as a real reading. */
      configured();
      g_reenter      = 1;
      g_reentered_ok = 0;
      g_stamp++;
      g_mono += 3600;
      pancra_remote_sync();
      ck(g_reentered_ok, "a sync started while one is in flight is REFUSED, "
                         "not stacked");
      ck(atomic_load(&g_maxdepth) == 1, "...so only ever one caller is inside "
                                        "the dispatch");
      g_reenter = 0;

      /* And under real threads, which is where the gate has to hold: the
       * counter below is only touched inside the gate, so a second entrant
       * would be visible as a lost or doubled count. */
      /* <pthread.h> IS included above. glibc defines pthread_t in a private
       * header with no pragma pointing back at the public one, so
       * include-cleaner asks for that private header by name -- including
       * which would be the actual defect. */
      /* NOLINTNEXTLINE(misc-include-cleaner) */
      pthread_t th[4];
      atomic_store(&g_race_stop, 0);
      atomic_store(&g_ticker_rounds, 0);
      for (int i = 0; i < 4; i++)
         if (pthread_create(&th[i], 0, ticker, 0) != 0)
            ck(0, "a ticker thread started");
      /* WAIT UNTIL THEY ARE ACTUALLY TICKING. pthread_create returns long
       * before the thread runs, and a main loop that finishes first turns
       * this into four threads run one after another -- which is not the
       * case being tested. */
      int waited = 0;
      while (atomic_load(&g_ticker_rounds) == 0 && waited < 500000) {
         sched_yield();
         waited++;
      }
      ck(atomic_load(&g_ticker_rounds) > 0, "the tickers are running");
      long before_main = atomic_load(&g_ticker_rounds);
      for (int i = 0; i < 200000; i++) {
         g_stamp++;
         g_mono += 61;
         pancra_remote_sync();
      }
      long during_main = atomic_load(&g_ticker_rounds) - before_main;
      atomic_store(&g_race_stop, 1);
      for (int i = 0; i < 4; i++)
         pthread_join(th[i], 0);
      /* THE OVERLAP, ASSERTED. Without this the case passes when the tickers
       * happened to be scheduled entirely before or entirely after the main
       * loop -- five threads that never met, proving nothing about a gate
       * that only matters when they do. */
      ck(during_main > 1000, "the tickers ran THROUGHOUT the main loop, not "
                             "before or after it");
      ck(atomic_load(&g_maxdepth) == 1,
         "five threads hammering the scheduler never put two syncs in flight "
         "-- two inside the shared batch buffer splice a fabricated reading "
         "into the upload");
   }

   printf("== what the screen is told ==\n");
   {
      configured();
      sync_report(SYNC_RESTORED);
      ck(g_last_outcome == SYNC_RESTORED, "the outcome reaches the model as "
                                          "itself, not as a sentence");
      int d = g_dirty;
      sync_report(SYNC_NOTHING_NEW);
      ck(g_dirty > d, "...and the screen is repainted, or the user sees the "
                      "previous state for ever");
      ck(g_last_outcome == SYNC_NOTHING_NEW, "...with the new outcome");
   }

   printf("\n%s\n", all ? "ALL REMOTE TESTS PASSED" : "REMOTE TESTS FAILED");
   return all ? 0 : 1;
}
