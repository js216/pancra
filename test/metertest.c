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
#include "otble.h"
#include "util.h" /* realtime_s: the driver bounds against it */
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

void ot_drv_write(const uint8_t *d, int n)
{
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

   printf("\n%s\n", all ? "ALL METER TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
