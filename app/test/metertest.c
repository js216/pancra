// SPDX-License-Identifier: GPL-3.0
// metertest.c --- Host tests for the OneTouch meter driver
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for otble.c, which had none.
 *
 * This module decides which fingersticks reach the append-only log, and it has
 * a bad history in exactly the places a hand-check reads past:
 *
 *   - a timestamp gate that compared the meter's NAIVE LOCAL clock against real
 *     UTC, so at any offset past UTC+1 a fingerstick taken that second was
 *     rejected -- and because last_index advances unconditionally, the record
 *     was never requested again. Silent permanent loss of every reading, for
 *     most of the world.
 *   - a bound that accepted dates into 2039, so a phantom point pinned itself
 *     to the right edge of every plot and was re-admitted on every restart.
 *   - a walk that must ADVANCE past an unreadable record, because not advancing
 *     re-requests the same index forever and loses every later fingerstick, not
 *     just the bad one.
 *
 * The driver is transport-agnostic: it talks through the ot_drv_* hooks, which
 * this file implements. Built and run by `make metertest`.
 */
#include "clock.h"
#include "meterlogic.h"
#include "otble.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

/* ---- captured transport state ---- */
static int n_readings, n_done, n_disc, last_glu;
static long last_naive;
static int last_done_new;
static uint8_t sent[64];
static int sentlen;
/* HOW MANY frames went out, not just the last one's length. Two record
 * requests are the same length, so comparing sentlen cannot tell whether
 * another one was sent -- which made a "the walk did not advance" assertion
 * true whatever the driver did. */
static int n_writes;

void ot_drv_write(const uint8_t *d, int n)
{
   n_writes++;
   sentlen = n < (int)sizeof sent ? n : (int)sizeof sent;
   memcpy(sent, d, (size_t)sentlen);
}

void ot_drv_subscribe(void)
{
}

void ot_drv_disconnect(void)
{
   n_disc++;
}

void ot_drv_status(const char *s)
{
   (void)s;
}

/* Returns 0 to simulate the host refusing a record as implausible -- the
 * driver must then NOT persist its walk past it. */
static int host_refuses;

int ot_drv_reading(long naive, int mg_dl)
{
   if (host_refuses)
      return 0;
   n_readings++;
   last_naive = naive;
   last_glu   = mg_dl;
   return 1;
}

void ot_drv_done(int new_records)
{
   n_done++;
   last_done_new = new_records;
}

/* ---- A ZONE WITH TRANSITIONS IN IT, so a DST boundary can be crossed on
 * demand rather than waited for -----------------------------------------
 *
 * US/Pacific, by the post-2007 rule: daylight time from 02:00 local on the
 * second Sunday in March to 02:00 local on the first Sunday in November. The
 * two edges are what this suite is about:
 *
 *   SPRING FORWARD  local 02:00 PST -> 03:00 PDT. Local times in
 *                   [02:00, 03:00) that day NEVER HAPPENED.
 *   FALL BACK       local 02:00 PDT -> 01:00 PST. Local times in
 *                   [01:00, 02:00) that day happened TWICE, an hour apart.
 *
 * EVERY YEAR, not one hard-coded pair of dates: a fixture with a single
 * year's transitions in it says nothing about an edit that moves a timestamp
 * into a different year, which is exactly one of the edits the keypad makes.
 *
 * The offsets are seconds east of UTC and both are negative, which is the
 * sign that catches truncating division: -28800 is standard, -25200 daylight.
 */
#define STD (-28800L)
#define DST (-25200L)

/* Days since 1970-01-01, by COUNTING. Deliberately NOT the Hinnant formula
 * civil.c uses: ground truth computed with the code under test agrees with it
 * by construction, which is no test at all. */
static long day_of(int y, int m, int d)
{
   static const int L[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
   long n                 = 0;
   for (int yy = 1970; yy < y; yy++)
      n += 365 + ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0);
   int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
   for (int mm = 1; mm < m; mm++)
      n += L[mm - 1] + (mm == 2 && leap);
   return n + d - 1;
}

/* A local clock reading, as civil.h means it: not an instant. */
static long naive_at(int y, int m, int d, int hh, int mi)
{
   return (day_of(y, m, d) * 86400L) + (hh * 3600L) + (mi * 60L);
}

/* The n-th Sunday of a month. 1970-01-01 was a Thursday, so day 0 is weekday
 * 4 counting Sunday as 0. */
static int nth_sunday(int y, int m, int n)
{
   int dow   = (int)(((day_of(y, m, 1) + 4) % 7 + 7) % 7);
   int first = 1 + ((7 - dow) % 7);
   return first + (7 * (n - 1));
}

/* The transition INSTANTS, expressed through the offset in force just before
 * each -- which is how a zone file states them. */
static long spring_utc(int y)
{
   return naive_at(y, 3, nth_sunday(y, 3, 2), 2, 0) - STD;
}

static long fall_utc(int y)
{
   return naive_at(y, 11, nth_sunday(y, 11, 1), 2, 0) - DST;
}

static int zone_calls;

/* WHICH YEAR an instant is in, closely enough to pick the right pair of
 * transitions: the boundaries are in March and November, so a year taken from
 * a plain division is never off by enough to matter. */
static long pacific(void *ctx, long t)
{
   (void)ctx;
   zone_calls++;
   int y = 1970 + (int)(t / (365L * 86400L));
   for (int k = y - 1; k <= y + 1; k++)
      if (t >= spring_utc(k) && t < fall_utc(k))
         return DST;
   return STD;
}

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* CRC-16/CCITT-FALSE, mirroring the driver's own so frames are well-formed. */
static unsigned crc16(const uint8_t *p, int n)
{
   unsigned c = 0xFFFFU;
   for (int i = 0; i < n; i++) {
      c ^= (unsigned)p[i] << 8U;
      for (int b = 0; b < 8; b++)
         c = (c & 0x8000U) ? ((c << 1U) ^ 0x1021U) & 0xFFFFU
                           : (c << 1U) & 0xFFFFU;
   }
   return c & 0xFFFFU;
}

/* Build a response frame around `payload` (which begins with the status byte)
 * and hand it to the driver. */
static void feed(const uint8_t *payload, int plen, int corrupt_crc)
{
   uint8_t f[64];
   int total = plen + 8;
   f[0]      = 0x01;
   f[1]      = 0x02;
   f[2]      = (uint8_t)(total - 1);
   f[3]      = 0x00;
   f[4]      = 0x0c;
   memcpy(f + 5, payload, (size_t)plen);
   f[5 + plen] = 0x03;
   unsigned c  = crc16(f + 1, plen + 5);
   if (corrupt_crc)
      c ^= 0xFFFFU;
   f[6 + plen] = (uint8_t)c;
   f[7 + plen] = (uint8_t)(c >> 8U);
   ot_on_notify(f, total);
}

/* A count response: status 0x06 then the u32 highest index. */
static void feed_count(unsigned top)
{
   uint8_t pl[5] = {0x06, (uint8_t)top, (uint8_t)(top >> 8U),
                    (uint8_t)(top >> 16U), (uint8_t)(top >> 24U)};
   feed(pl, 5, 0);
}

/* A record response: status 0x06 then ts u32, glu u16, ctrl, 4 tail bytes. */
static void feed_record(long ts, int glu, int ctrl, int tailset)
{
   uint8_t pl[12]  = {0x06};
   unsigned long t = (unsigned long)ts;
   pl[1]           = (uint8_t)t;
   pl[2]           = (uint8_t)(t >> 8U);
   pl[3]           = (uint8_t)(t >> 16U);
   pl[4]           = (uint8_t)(t >> 24U);
   pl[5]           = (uint8_t)glu;
   pl[6]           = (uint8_t)((unsigned)glu >> 8U);
   pl[7]           = (uint8_t)ctrl;
   pl[8]           = tailset ? 1 : 0;
   pl[9] = pl[10] = pl[11] = 0;
   feed(pl, 12, 0);
}

static void begin(int stored_index)
{
   n_readings = n_done = n_disc = 0;
   n_writes                     = 0;
   last_glu                     = -1;
   last_naive                   = -1;
   last_done_new                = -1;
   ot_init(stored_index);
   ot_on_connected();
   /* Drive past the session handshake the driver now opens with -- 20 02
    * (get-time) then 27 00 (R-counter), matching xDrip and the official app --
    * so the scenarios below begin at the T-counter exchange exactly as before.
    * Both are status-06 frames the driver acks and whose bodies it ignores. */
   {
      uint8_t time_resp[5] = {0x06, 0, 0, 0, 0};
      uint8_t rcnt_resp[3] = {0x06, 0, 0};
      feed(time_resp, 5, 0);
      feed(rcnt_resp, 3, 0);
   }
}

/* "About now" in the meter's epoch.
 *
 * Derived from the real clock, NOT a fixed constant: the driver bounds records
 * against realtime_s(), so a hardcoded stamp drifts out of the window as the
 * calendar advances and the future-rejection case silently stops testing
 * anything. The first draft used a 2025 constant, which made "400 days ahead"
 * land in the past and the assertion fail for the wrong reason. */
static long naive_now(void)
{
   return realtime_s() - OT_EPOCH;
}

int main(void)
{
   printf("== the session opens 20 02 -> 27 00 -> 0a 02 06, never a cold count "
          "==\n");
   /* Regression guard for the handshake. Both proven OneTouch BLE drivers
    * (xDrip VerioHelper, official OneTouch Reveal app) read the clock then the
    * R-counter before the T-counter; a bare 0a0206 on a fresh link is seen in
    * NO capture and may be refused. Assert the exact opening command order. */
   ot_init(9);
   ot_on_connected();
   ck(sentlen >= 8 && sent[5] == 0x20 && sent[6] == 0x02,
      "connect sends 20 02 (get-time) first");
   ck(sent[4] == 0x04, "...with the ctl byte the meter was captured accepting");
   {
      uint8_t time_resp[5] = {0x06, 0, 0, 0, 0};
      feed(time_resp, 5, 0);
   }
   ck(sent[5] == 0x27 && sent[6] == 0x00,
      "the time response advances to 27 00 (R-counter)");
   {
      uint8_t rcnt_resp[3] = {0x06, 0, 0};
      feed(rcnt_resp, 3, 0);
   }
   ck(sent[5] == 0x0a && sent[6] == 0x02 && sent[7] == 0x06,
      "the R-counter response advances to 0a 02 06 (T-counter)");

   printf("== a well-formed record is accepted ==\n");
   begin(9);
   feed_count(10);
   feed_record(naive_now(), 105, 0, 0);
   ck(n_readings == 1 && last_glu == 105, "one reading, correct value");
   ck(last_naive == naive_now(), "the NAIVE meter clock is passed through");
   ck(ot_last_index() == 10, "the stored index advanced to the record read");

   printf("== a corrupt CRC is dropped, and drops nothing else ==\n");
   begin(9);
   feed_count(10);
   {
      uint8_t pl[12] = {0x06};
      feed(pl, 12, 1); /* bad CRC */
   }
   ck(n_readings == 0, "no reading from a CRC-invalid frame");
   ck(ot_last_index() == 9, "...and the walk position is unchanged");

   printf("== value and timestamp gates ==\n");
   begin(9);
   feed_count(10);
   feed_record(naive_now(), 19, 0, 0);
   ck(n_readings == 0, "19 mg/dL is below the plausible range");
   begin(9);
   feed_count(10);
   feed_record(naive_now(), 601, 0, 0);
   ck(n_readings == 0, "601 mg/dL is above it");
   begin(9);
   feed_count(10);
   feed_record(naive_now(), 100, 1, 0);
   ck(n_readings == 0, "a control-solution record is not a fingerstick");
   begin(9);
   feed_count(10);
   feed_record(naive_now(), 100, 0, 1);
   ck(n_readings == 0, "a non-zero tail marks a non-glucose event");

   printf("== the timestamp gate must TOLERATE any timezone ==\n");
   /* THE regression: `ts` is the meter's naive LOCAL clock, so at UTC+14 a
    * reading taken now looks up to 14 h "ahead" of real UTC. A gate with only
    * an hour of slack rejected it -- and rejection is permanent, because the
    * walk advances regardless and the index is persisted. */
   begin(9);
   feed_count(10);
   feed_record(naive_now() + (14L * 3600), 100, 0, 0);
   ck(n_readings == 1, "a reading 14 h ahead (UTC+14 local clock) is kept");
   begin(9);
   feed_count(10);
   feed_record(naive_now() + (400L * 86400), 100, 0, 0);
   ck(n_readings == 0, "a reading 400 days ahead is still rejected");

   printf("== an unreadable record must not wedge the walk ==\n");
   /* Not advancing re-requests the same index every session forever, so every
    * LATER fingerstick is lost too -- far worse than skipping one. */
   begin(9);
   feed_count(11);
   feed_record(naive_now(), 19, 0, 0); /* rejected value */
   ck(ot_last_index() == 10, "the index advances past a rejected record");

   printf("== nothing new: leave at once, do not walk ==\n");
   begin(10);
   feed_count(10);
   ck(n_readings == 0, "no records read when the counter matches");
   ck(n_done == 1 && last_done_new == 0, "the sync completes reporting 0 new");
   ck(n_disc == 1, "and the link is dropped so the meter can power down");

   printf("== a counter that went BACKWARDS means the memory was cleared ==\n");
   /* Without this the stored index stays above the counter forever, every sync
    * reports NOTHING NEW, and no fingerstick is ever recorded again. */
   begin(500);
   feed_count(3);
   ck(n_done == 0,
      "a backwards counter does not end the sync as 'nothing new'");
   feed_record(naive_now(), 111, 0, 0);
   ck(n_readings == 1, "...it restarts the import instead");

   printf("== an implausible counter is refused, not walked ==\n");
   begin(0);
   feed_count(0x10000); /* beyond the 16-bit index the request can carry */
   ck(n_done == 1 && n_readings == 0, "a >0xFFFF counter aborts the sync");

   printf("== one connection is bounded by the session cap ==\n");
   begin(-1);
   feed_count(100000 / 100); /* 1000 records available */
   {
      int guard = 0;
      while (n_done == 0 && guard++ < 500)
         feed_record(naive_now(), 100, 0, 0);
      ck(n_readings <= OT_MAX_WALK,
         "no more than OT_MAX_WALK records in one session");
      ck(n_done == 1, "...and the session finishes rather than running on");
   }

   printf("== a mid-sync drop still persists progress ==\n");
   begin(9);
   feed_count(20);
   feed_record(naive_now(), 100, 0, 0);
   ot_on_disconnected();
   ck(n_done == 1, "a drop mid-walk reports done so the index is saved");
   ck(ot_last_index() == 10, "...at the record actually reached");

   printf("== a HOST refusal rewinds the stored index at session end ==\n");
   {
      /* The host bounds the converted instant against the PHONE's clock, which
       * can legitimately be wrong. Advancing past such a rejection deletes good
       * fingersticks permanently: `from` is last_index + 1 and they are never
       * requested again. The rewind happens ONCE, at finish() -- an earlier
       * version skipped the per-record assignment instead, which failed
       * whenever a good record followed a refused one, because the assignment
       * is not a max. */
      begin(9);
      feed_count(11);
      host_refuses = 1;
      feed_record(naive_now(), 100, 0, 0); /* record 10: refused */
      host_refuses = 0;
      /* Nothing accepted at all => the phone's clock is the likely cause, so
       * the index must stay put for a later retry. Checked separately below. */
      feed_record(naive_now(), 105, 0, 0); /* record 11: accepted, ends it */
      ck(n_readings == 1, "the good record was still imported");
      /* A good record AFTER the refusal proves the phone's clock is fine, so
       * record 10 itself was bad: move past it rather than retry it forever. */
      ck(ot_last_index() == 11,
         "...and the walk moved on, because something else imported");
   }

   printf(
       "== a refused stretch with good records after it must not repeat ==\n");
   {
      /* If anything was imported, the refusals were a STRETCH (the meter's own
       * date was wrong for a while), not the phone's clock -- so the walk must
       * move past them. Rewinding here re-walks the same stretch every session
       * forever, re-delivering records already stored. */
      begin(9);
      feed_count(13);
      host_refuses = 1;
      feed_record(naive_now(), 100, 0, 0); /* 10: refused */
      host_refuses = 0;
      feed_record(naive_now(), 101, 0, 0); /* 11: imported */
      feed_record(naive_now(), 102, 0, 0); /* 12 */
      feed_record(naive_now(), 103, 0, 0); /* 13: ends the session */
      ck(n_readings == 3, "the good records imported");
      ck(ot_last_index() == 13,
         "...and the index moved PAST the refused stretch, not back to it");
   }

   printf("== but a FULL window of refusals must not wedge the walk ==\n");
   {
      /* If the meter's own date was set wrong for a stretch, the refusals are
       * persistent, not transient. Rewinding then makes every future session
       * re-walk the same window and never reach the good records behind it --
       * every later fingerstick lost, silently and permanently. A full window
       * of refusals is enough evidence to give up on them and move on. */
      begin(0);
      feed_count(1000);
      host_refuses = 1;
      for (int i = 0; i < OT_MAX_WALK + 2 && n_done == 0; i++)
         feed_record(naive_now(), 100, 0, 0);
      host_refuses = 0;
      ck(n_done == 1, "the session ended at the cap");
      ck(ot_last_index() > 0,
         "the walk made forward progress instead of wedging");
      /* And the next session must resume beyond them, not repeat them. */
      int resumed = ot_last_index();
      begin(resumed);
      feed_count(1000);
      feed_record(naive_now(), 111, 0, 0);
      ck(n_readings == 1 && ot_last_index() > resumed,
         "...so the next session moves past the refused block");
   }

   printf("== a FIRST sync reaches the newest record ==\n");
   /* The walk fetches at most OT_MAX_WALK records INCLUSIVE of its start, so
    * a first-sync window of top - OT_MAX_WALK spanned 21 records and the cap
    * stopped one short of top -- the newest record, i.e. the fingerstick
    * that prompted the pairing, arrived only on the NEXT power-on. */
   begin(-1);
   feed_count(100);
   for (int i = 0; i < OT_MAX_WALK; i++)
      feed_record(naive_now() - ((long)i * 60), 100 + i, 0, 0);
   ck(n_readings == OT_MAX_WALK, "a fresh pairing fills its whole window");
   ck(ot_last_index() == 100,
      "...and the window ENDS at the newest record, not one short of it");
   ck(n_done == 1, "the session finishes at the cap, not before");

   printf("== the meter runtime's two timeouts ==\n");
   {
      /* Both are RECOVERY rules, and both exist because a BLE callback can
       * simply not arrive. While they were `if` statements inside a function
       * that closes GATT links and takes two locks, nothing could reach
       * them -- and a wrong bound here is a meter that never syncs again. */
      struct meter_tick mt;
      long idle[METER_LINKS_MAX] = {0};

      /* A sync inside its budget is left alone. */
      meter_tick_eval(1, 1000, idle, METER_LINKS_MAX, 1000 + METER_SYNC_MAX_S,
                      &mt);
      ck(!mt.drop_sync, "a sync at exactly its budget has not overrun");
      meter_tick_eval(1, 1000, idle, METER_LINKS_MAX,
                      1000 + METER_SYNC_MAX_S + 1, &mt);
      ck(mt.drop_sync, "...and one past it is dropped");
      meter_tick_eval(0, 1000, idle, METER_LINKS_MAX, 9999999, &mt);
      ck(!mt.drop_sync, "with no sync running there is nothing to drop");

      /* A link waiting for a teardown is released only once the bound is
       * genuinely past -- a real teardown is immediate or ~35 s. */
      idle[2] = 5000;
      meter_tick_eval(0, 0, idle, METER_LINKS_MAX,
                      5000 + METER_TEARDOWN_MAX_S - 1, &mt);
      ck(mt.nrelease == 0, "a link inside the teardown bound is left alone");
      meter_tick_eval(0, 0, idle, METER_LINKS_MAX, 5000 + METER_TEARDOWN_MAX_S,
                      &mt);
      ck(mt.nrelease == 1 && mt.release[2],
         "...and released once the bound is reached");
      ck(!mt.release[0] && !mt.release[1], "...only the link that was waiting");

      /* NOT WHILE A SYNC IS RUNNING. A busy runtime owns its links, and
       * releasing one under it would tear down the very exchange the other
       * rule may be about to end properly. */
      meter_tick_eval(1, 9999999, idle, METER_LINKS_MAX, 9999999, &mt);
      ck(mt.nrelease == 0, "a busy runtime keeps its links");

      /* A link that is not waiting for anything is never released. */
      long none[METER_LINKS_MAX] = {0};
      meter_tick_eval(0, 0, none, METER_LINKS_MAX, 9999999, &mt);
      ck(mt.nrelease == 0, "a link with no teardown pending is left alone");
   }

   printf("== late, duplicate and wrong-phase frames are refused ==\n");
   {
      /* The protocol is strictly request/response, and nothing enforced it:
       * any CRC-valid frame ran the handler for whatever `phase` held. */

      /* AFTER THE SESSION IS OVER. A non-0x06 status called finish() again --
       * re-reporting the count and re-issuing the disconnect on a link that
       * may since belong to another exchange. */
      begin(5);
      feed_count(5); /* nothing new: this finishes the session */
      int done_after = n_done;
      int disc_after = n_disc;
      ck(done_after == 1, "the session completes once");

      uint8_t refused[2] = {0x07, 0}; /* "command not allowed" */
      feed(refused, 2, 0);
      ck(n_done == done_after, "a frame after the session does NOT finish it "
                               "again");
      ck(n_disc == disc_after, "...and does not re-issue the disconnect");

      /* BEFORE ANYTHING IS ASKED. ot_init leaves the driver idle; a frame
       * there is an answer to a request that was never made. */
      n_readings = n_done = n_disc = 0;
      ot_init(0);
      /* A REFUSAL status, deliberately: a 0x06 in P_IDLE matches no phase
       * branch and does nothing even without the gate, so asserting on one
       * would pass whatever the driver did. A non-0x06 is what used to reach
       * finish() from a state that never started a session. */
      uint8_t idle_refused[2] = {0x07, 0};
      feed(idle_refused, 2, 0);
      ck(n_done == 0 && n_disc == 0,
         "a frame arriving while idle does not finish a session that never "
         "began");

      /* A SECOND ANSWER WITH NOTHING OUTSTANDING. Once the session has
       * finished no command is in flight, so a repeated record frame is an
       * answer to nothing: it must not be stored and must not restart the
       * walk by acking and asking again.
       *
       * WHAT IS NOT ASSERTED, because this layer cannot do it: a duplicate
       * that arrives inside the NEXT request's window. The record response
       * carries a timestamp and a value but no index, and the frame header
       * has no transaction id, so a repeat of record 5 landing after record 6
       * was asked for is byte-identical to record 6's answer. Nothing here
       * can separate them -- the store's timestamp dedup is what limits the
       * damage. Asserting otherwise would be asserting a guarantee that does
       * not exist. */
      begin(5);
      feed_count(5); /* nothing new: the session finishes */
      int reads_before  = n_readings;
      int writes_before = n_writes;
      uint8_t rec[12]   = {0x06, 0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0};
      feed(rec, 12, 0);
      ck(n_readings == reads_before,
         "a record frame with no request outstanding stores nothing");
      ck(n_writes == writes_before,
         "...and does not restart the walk by acking and asking again");
   }

   printf("== a wall-clock jump must not move these decisions ==\n");
   /* THE CLOCK THESE TAKE IS ELAPSED TIME (mono_s), not the wall clock. The
    * module cannot tell the difference -- it takes `now` as a number -- so
    * what is pinned here is the SHAPE of the failure a wall-clock jump used
    * to cause, with the jump expressed as the caller passing a moved clock.
    *
    * A phone that has been off, or has just found a network, corrects its
    * clock by minutes or hours. On realtime that jump WAS the elapsed time. */
   {
      struct meter_tick mt;
      long idle[METER_LINKS_MAX] = {0};
      long start                 = 1000;

      /* Forward jump: a sync that started a second ago, with the clock
       * suddenly an hour ahead, looked an hour old and was torn down at
       * once -- killing an exchange that was working. */
      meter_tick_eval(1, start, idle, METER_LINKS_MAX, start + 1, &mt);
      ck(!mt.drop_sync, "a one-second-old sync is not timed out");
      meter_tick_eval(1, start, idle, METER_LINKS_MAX, start + 3600, &mt);
      ck(mt.drop_sync, "...but an hour of ELAPSED time is a wedged exchange");

      /* Backward jump: with `now` behind the start -- which the wall clock
       * allowed and monotonic does not -- elapsed time went negative and the
       * watchdog could never fire for that sync again. */
      meter_tick_eval(1, start, idle, METER_LINKS_MAX, start - 3600, &mt);
      ck(!mt.drop_sync, "a clock behind the start does not fire the watchdog");
      meter_tick_eval(1, start, idle, METER_LINKS_MAX,
                      start + METER_SYNC_MAX_S + 1, &mt);
      ck(mt.drop_sync, "...and the real timeout still works afterwards");
   }

   printf("== the repeated hour: a MONOTONIC sequence, not a fixed guess ==\n");
   /* TODO 132. A OneTouch record is a naive local clock reading. In the hour
    * a fall-back repeats, that reading names two instants an hour apart, and
    * the old code -- a fixed-point iteration over the zone -- always settled
    * on the same one. Two fingersticks taken an hour apart therefore decoded
    * to the SAME instant, and the reading log dedups a BGM by exact
    * timestamp, so the second one was not stored an hour wrong: it was not
    * stored at all.
    *
    * The evidence is the walk itself. The meter hands records over in
    * ascending index order and an index is assigned when the fingerstick is
    * taken, so the instants are known to increase even where the clock
    * readings do not. */
   {
      /* A real morning across the 2025 fall-back, in the order the meter
       * would report it. The clock reading goes 01:45 -> 01:15 because the
       * clock went back; the INSTANTS never do. */
      static const struct {
         int hh, mi;
         long off; /* the offset actually in force -- the ground truth */
      } run[] = {
          {0, 30, DST}, /* before the transition */
          {1, 15, DST}, /* first pass of the repeated hour */
          {1, 45, DST},
          {1, 15, STD}, /* the clock has gone back: second pass */
          {1, 50, STD},
          {2, 30, STD}, /* past it: unambiguous again */
      };
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long got[6];
      int nbad = 0, ncollide = 0, nback = 0;
      long import_t = naive_at(2025, 11, 3, 9, 0) - STD; /* imported next day */
      for (int i = 0; i < 6; i++) {
         long nv = naive_at(2025, 11, 2, run[i].hh, run[i].mi);
         struct meter_stamp st =
             meter_stamp_step(&sq, nv, import_t, pacific, 0);
         got[i] = st.t;
         if (st.t != nv - run[i].off)
            nbad++;
         for (int k = 0; k < i; k++)
            if (got[k] == got[i])
               ncollide++;
         if (i && got[i] <= got[i - 1])
            nback++;
      }
      ck(ncollide == 0, "no two fingersticks across the fall-back decode to "
                        "the SAME instant");
      ck(nback == 0, "...the sequence of instants is strictly increasing");
      ck(nbad == 0, "...and every one of them is the instant it was actually "
                    "taken at");
      ck(got[3] - got[2] == 1800,
         "the 01:15 AFTER the 01:45 is half an hour LATER, not half an hour "
         "earlier");
   }

   printf("== two fingersticks with the SAME clock reading ==\n");
   /* The collision in its purest form: 01:30 PDT and 01:30 PST are the same
    * four bytes in the meter's memory. A single fixed offset makes them one
    * instant, and one of the two fingersticks disappears into the log's
    * dedup. */
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long nv              = naive_at(2025, 11, 2, 1, 30);
      long import_t        = naive_at(2025, 11, 3, 9, 0) - STD;
      struct meter_stamp a = meter_stamp_step(&sq, nv, import_t, pacific, 0);
      struct meter_stamp b = meter_stamp_step(&sq, nv, import_t, pacific, 0);
      ck(a.t != b.t, "identical clock readings do not decode to one instant");
      ck(b.t - a.t == 3600, "...they are the hour apart they were taken");
      ck(a.t == nv - DST && b.t == nv - STD,
         "...the first in the first pass of the repeated hour, the second in "
         "the second");
      ck(a.off == DST && b.off == STD,
         "...each stamped with the offset that was actually in force");
      ck(!b.ambiguous, "the second is DECIDED -- by the record before it, not "
                       "by a guess");
   }

   printf("== when nothing decides it, the ambiguity is KEPT ==\n");
   /* A run of readings whose clock readings rise steadily through the
    * repeated hour is consistent with the fall-back having happened anywhere
    * in it. There is no evidence. The reading is still stored -- refusing it
    * would lose a fingerstick -- but it is stored as a stated guess, with the
    * instant that was not chosen travelling with it. */
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long import_t          = naive_at(2025, 11, 3, 9, 0) - STD;
      int flagged            = 0;
      int alt_kept           = 0;
      int rising             = 1;
      long prev              = 0;
      static const int mi[3] = {10, 20, 30};
      for (int i = 0; i < 3; i++) {
         long nv = naive_at(2025, 11, 2, 1, mi[i]);
         struct meter_stamp st =
             meter_stamp_step(&sq, nv, import_t, pacific, 0);
         if (st.ambiguous)
            flagged++;
         if (st.t_alt == nv - STD && st.t == nv - DST)
            alt_kept++;
         if (i && st.t <= prev)
            rising = 0;
         prev = st.t;
      }
      ck(flagged == 3, "an undecidable run is FLAGGED, every record of it");
      ck(alt_kept == 3, "...with the instant that was not chosen retained");
      ck(rising, "...and the sequence is still monotonic");
   }

   printf("== import time rules out the candidate in the future ==\n");
   /* The one piece of evidence available to the FIRST record of a walk: a
    * fingerstick cannot have been taken after it was imported. */
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long nv    = naive_at(2025, 11, 2, 1, 30);
      long early = nv - DST;
      long late  = nv - STD;
      struct meter_stamp st =
          meter_stamp_step(&sq, nv, early + 600, pacific, 0);
      ck(st.t == early && !st.ambiguous,
         "an import ten minutes after the earlier instant rules out the "
         "later one");
      ck(st.t_alt == late, "...and still says which one it ruled out");

      /* Without an import time the same record is undecidable, and says so
       * rather than pretending the constraint applied. */
      meter_seq_reset(&sq);
      struct meter_stamp no_t = meter_stamp_step(&sq, nv, 0, pacific, 0);
      ck(no_t.ambiguous, "with no import time there is nothing to rule it out");
   }

   printf("== the skipped hour, and the ordinary hours around it ==\n");
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long nv               = naive_at(2025, 3, 9, 2, 30); /* never happened */
      struct meter_stamp st = meter_stamp_step(
          &sq, nv, naive_at(2025, 3, 10, 9, 0) - DST, pacific, 0);
      ck(st.shifted, "a record in the skipped hour is reported as such");
      ck(st.t + st.off == naive_at(2025, 3, 9, 3, 30),
         "...and moved FORWARD by the gap, to 03:30");
      ck(!st.ambiguous, "...which is a nonexistent time, not an ambiguous one");
   }

   printf("== and every record outside a transition decodes as before ==\n");
   /* The regression a disambiguator invites. The old conversion was a
    * fixed-point iteration: take the offset at the naive value read as an
    * instant, then ask again with the result. Outside the two transition
    * hours that is correct, and the new code must agree with it EXACTLY --
    * fixing two hours a year by moving the other 8758 is not a fix. */
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      int differ = 0, flagged = 0, n = 0;
      for (int mo = 1; mo <= 12; mo++) {
         for (int hh = 0; hh < 24; hh++) {
            long nv = naive_at(2025, mo, 20, hh, 7);
            /* The OLD algorithm, spelled out here so the comparison is
             * against what shipped rather than against the new code. */
            long o1  = pacific(0, nv);
            long old = nv - pacific(0, nv - o1);
            meter_seq_reset(&sq); /* each record on its own, as before */
            struct meter_stamp st = meter_stamp_step(&sq, nv, 0, pacific, 0);
            n++;
            if (st.t != old)
               differ++;
            if (st.ambiguous || st.shifted)
               flagged++;
         }
      }
      ck(n == 288 && differ == 0,
         "288 ordinary records decode to exactly what the old conversion "
         "gave");
      ck(flagged == 0, "...and none of them is flagged as ambiguous or "
                       "shifted");
   }

   printf("== a new walk starts with no evidence carried into it ==\n");
   /* The run state IS the evidence, so it must not outlive the walk: a
    * previous meter's last instant deciding this one's repeated-hour record
    * is a guess wearing evidence's clothes. And a record that resolves
    * cleanly closes the run, so a later ambiguous one is not pushed an hour
    * late by a stale flag. */
   {
      struct meter_seq sq;
      meter_seq_reset(&sq);
      long import_t = naive_at(2026, 11, 3, 9, 0) - STD;
      long nv       = naive_at(2025, 11, 2, 1, 30);
      (void)meter_stamp_step(&sq, nv, import_t, pacific, 0);
      (void)meter_stamp_step(&sq, nv, import_t, pacific, 0); /* forces late */
      /* An ordinary record: unambiguous, and it closes the run. */
      struct meter_stamp mid = meter_stamp_step(
          &sq, naive_at(2025, 11, 2, 5, 0), import_t, pacific, 0);
      ck(!mid.ambiguous && mid.t == naive_at(2025, 11, 2, 5, 0) - STD,
         "an ordinary record after the repeated hour is unaffected by it");
      /* NEXT year's repeated hour, in the same session: the earlier instant
       * again, because the previous fall-back is not evidence about this
       * one. */
      long nv2               = naive_at(2026, 11, 1, 1, 30);
      struct meter_stamp far = meter_stamp_step(&sq, nv2, import_t, pacific, 0);
      ck(far.t == nv2 - DST,
         "a later fall-back starts from the earlier instant again");
      ck(far.ambiguous, "...and is undecided, not decided by last year's");

      /* And a reset really does clear it. */
      meter_seq_reset(&sq);
      struct meter_stamp fresh_st =
          meter_stamp_step(&sq, nv, import_t, pacific, 0);
      ck(fresh_st.t == nv - DST,
         "after meter_seq_reset the walk has no previous instant to lean on");
   }

   printf("\n%s\n", all ? "ALL METER TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
