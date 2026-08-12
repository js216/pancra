// SPDX-License-Identifier: GPL-3.0
// otble.c --- LifeScan OneTouch BLE meter driver
// Copyright 2026 Jakob Kastelic

/* See otble.h. Frame layout, verified byte-for-byte against a live capture of
 * this phone syncing its meter:
 *
 *   [0] 0x01        BLE transport prefix, excluded from the CRC
 *   [1] 0x02        STX
 *   [2] len         total frame bytes - 1
 *   [3] 0x00        link control
 *   [4] ctl         0x03/0x04 on requests (toggles), 0x0c on responses
 *   [5..] payload   response payload begins with a status byte
 *       0x03        ETX
 *       CRC16 LE    CRC-16/CCITT-FALSE over bytes [1 .. len-3]
 *
 * A record is 11 bytes: uint32 LE timestamp (meter-local, epoch 2000-01-01),
 * uint16 LE glucose in mg/dL, a control-solution flag, then four bytes that
 * are zero for a real glucose reading. Non-glucose event records come back
 * with glucose 0 and a non-zero tail, which is what that check rejects. */
#include "otble.h"
#include "util.h" /* realtime_s: bound record timestamps against now */
#include <stdint.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "pancra", __VA_ARGS__)
#define LOGW(...) __android_log_print(5, "pancra", __VA_ARGS__)

enum {
   P_IDLE = 0,
   P_SUB,    /* subscribing to the notify characteristic */
   P_TIME,   /* asked for the meter clock (20 02) -- session handshake */
   P_RCOUNT, /* asked for the R-record counter (27 00) */
   P_COUNT,  /* asked for the highest record index (0a 02 06) */
   P_READ,   /* walking records forward */
   P_DONE
};

static int phase;
static int last_index;  /* highest record index already stored */
static int want_index;  /* record currently being requested */
static int top_index;   /* highest index the meter holds */
static int new_records; /* accepted this session */
static int walked;      /* records fetched this session (battery bound) */
/* First index this session refused on TIMESTAMP grounds, or -1. See the
 * advance rule in ot_on_notify: such a refusal is usually the clock, not the
 * record, so the session rewinds to it at the end -- unless the whole window
 * was refused, which is evidence the block is not transient. */
static int retry_from;

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. */
static uint16_t crc16(const uint8_t *p, int n)
{
   unsigned crc = 0xFFFFU;
   for (int i = 0; i < n; i++) {
      crc ^= (unsigned)p[i] << 8U;
      for (int b = 0; b < 8; b++)
         crc = (crc & 0x8000U) ? ((crc << 1U) ^ 0x1021U) & 0xFFFFU
                               : (crc << 1U) & 0xFFFFU;
   }
   return (uint16_t)crc;
}

/* Wrap `payload` in a request frame and hand it to the transport. */
static void send_cmd(const uint8_t *payload, int n)
{
   uint8_t f[24];
   /* prefix + STX + len + linkctl + ctl + payload + ETX + CRC16 */
   int total = n + 8;
   if (total > (int)sizeof f)
      return;
   f[0] = 0x01;
   f[1] = 0x02;
   f[2] = (uint8_t)(total - 1);
   f[3] = 0x00;
   /* ctl is a constant 0x04 for every command the official app sends in the
    * command set we use (T-counter 0a0206, record read b3xxxx) -- verified
    * against the btsnoop pairing+sync capture 2026-07-19. An earlier version
    * alternated 0x03/0x04 per command starting at 0x03; that pattern appears in
    * NO captured session (the app uses 0x03 only for the 0a0208/0a0207 device
    * counters, which we never send), so our very first command went out with a
    * ctl byte the meter never sees from the real app. */
   f[4] = 0x04;
   for (int i = 0; i < n; i++)
      f[5 + i] = payload[i];
   f[5 + n]   = 0x03;
   uint16_t c = crc16(f + 1, n + 5);
   f[6 + n]   = (uint8_t)c;
   f[7 + n]   = (uint8_t)(c >> 8U);
   ot_drv_write(f, total);
}

/* Session handshake. Before the T-counter, both proven OneTouch BLE
 * implementations -- xDrip's VerioHelper and the official OneTouch Reveal app
 * (btsnoop 2026-07-19) -- read the meter clock (20 02) then the R-counter
 * (27 00). Nothing captured EVER sends the T-counter cold, so we replay the
 * same opening: a bare 0a0206 on a fresh link is unproven and may be refused.
 * We do not consume either response (records carry absolute timestamps and we
 * walk the T-index, not the R-index) -- the exchange exists to open the session
 * exactly as the meter expects. */
static void ask_time(void)
{
   static const uint8_t cmd[2] = {0x20, 0x02};
   phase                       = P_TIME;
   ot_drv_status("METER: HELLO");
   send_cmd(cmd, 2);
}

static void ask_rcount(void)
{
   static const uint8_t cmd[2] = {0x27, 0x00};
   phase                       = P_RCOUNT;
   send_cmd(cmd, 2);
}

/* Highest record index the meter holds -- the T counter. Not a count: the
 * capture shows this returning 76 while records 70..76 were readable. */
static void ask_count(void)
{
   static const uint8_t cmd[3] = {0x0a, 0x02, 0x06};
   phase                       = P_COUNT;
   ot_drv_status("METER: COUNT");
   send_cmd(cmd, 3);
}

static void ask_record(int idx)
{
   uint8_t cmd[3] = {0xb3, (uint8_t)idx, (uint8_t)((unsigned)idx >> 8U)};
   want_index     = idx;
   phase          = P_READ;
   send_cmd(cmd, 3);
}

/* Every data response is acknowledged with a bare 0x81 before the next
 * request, or the meter stops answering. */
static void send_ack(void)
{
   uint8_t a = 0x81;
   ot_drv_write(&a, 1);
}

void ot_init(int last)
{
   phase       = P_IDLE;
   last_index  = last;
   want_index  = 0;
   top_index   = 0;
   new_records = 0;
   walked      = 0;
   retry_from  = -1;
}

int ot_last_index(void)
{
   return last_index;
}

void ot_on_connected(void)
{
   phase       = P_SUB;
   new_records = 0;
   walked      = 0;
   retry_from  = -1;
   ot_drv_subscribe();
   ask_time();
}

/* The retry_from rewind, shared by the clean finish and the mid-walk drop:
 * see the block comment above finish() for why each condition exists. */
static void rewind_refused(void)
{
   if (retry_from >= 0 && new_records == 0 && walked < OT_MAX_WALK &&
       retry_from - 1 < last_index) {
      LOGI("meter: rewinding stored index %d -> %d to retry a refused record",
           last_index, retry_from - 1);
      last_index = retry_from - 1;
   }
}

void ot_on_disconnected(void)
{
   if (phase != P_IDLE && phase != P_DONE) {
      /* Persist the progress made so far. Recording it only on clean
       * completion meant a mid-sync drop replayed every record next time --
       * and if the source id had changed meanwhile they were appended a second
       * time rather than deduped. */
      /* Same rewind finish() applies, same conditions. Without it a drop
       * right after a timestamp-refused record persisted last_index already
       * advanced PAST it, so the refusal -- often the PHONE's clock being
       * wrong, not the record -- was never retried and the fingerstick was
       * permanently lost. That silently defeated the header's "must not
       * persist its walk past this" contract on exactly the abnormal path
       * where it matters. */
      rewind_refused();
      LOGI("meter: link dropped mid-sync at record %d; keeping index %d",
           want_index, last_index);
      ot_drv_done(0);
   }
   phase = P_IDLE;
}

/* Finish: report, then drop the link immediately so the meter can power down
 * on its own schedule instead of being held awake by us. */
static void finish(void)
{
   /* REWIND to the first timestamp refusal, so a later sync retries it -- the
    * phone's clock may simply have been wrong. But only if we did NOT fill the
    * session window: a full window of refusals is evidence the block is
    * persistent (a meter whose own date was set wrong for a stretch), and
    * rewinding then would re-walk the same records every session forever and
    * never reach the good ones behind them. */
   /* Rewind only if NOTHING was accepted this session.
    *
    * `walked < OT_MAX_WALK` was the wrong test: it asks whether the window
    * filled, not whether the refusals were transient, so a session that
    * refused a stretch and then imported good records still rewound -- and
    * re-walked the same stretch next time, forever, re-delivering records it
    * had already stored.
    *
    * What actually separates the two causes: a wrong PHONE clock refuses every
    * record including the newest, so nothing is accepted and retrying later is
    * right. A meter whose own date was wrong for a stretch refuses only that
    * stretch, and the records after it import fine -- so new_records > 0 means
    * the block is persistent and the walk must move past it.
    *
    * The window-fullness test stays as the second guard: if a whole session's
    * worth of records is refused with nothing accepted, retrying them forever
    * would never reach the good ones behind them. Both conditions are needed
    * -- dropping either one reintroduces a different permanent data loss. */
   rewind_refused();
   phase = P_DONE;
   ot_drv_status(new_records ? "METER: SYNCED" : "METER: NOTHING NEW");
   ot_drv_done(new_records);
   ot_drv_disconnect();
}

void ot_on_notify(const uint8_t *buf, int n)
{
   if (n < 8 || buf[0] != 0x01 || buf[1] != 0x02)
      return;
   int total = (int)buf[2] + 1;
   /* Bound total from BOTH ends. Without the lower bound a frame declaring
    * len 0 gives total 1 and buf[total - 2] reads before the buffer -- an
    * out-of-bounds stack read reachable from any peer on this link. */
   if (total < 9 || total > n)
      return;
   uint16_t want =
       (uint16_t)((unsigned)buf[total - 2] | ((unsigned)buf[total - 1] << 8U));
   if (crc16(buf + 1, total - 3) != want) {
      LOGI("meter: CRC mismatch, frame dropped");
      return;
   }
   int status = buf[5];
   /* 0x09 = "invalid record index" -- an EMPTY or non-existent slot, NOT a dead
    * meter. It is per-INDEX, so it must be SKIPPED, never fatal: different
    * Verio firmwares number their records differently (RI_02.02.00 errors on
    * index 0, RI_01.09.04 errors on index 1 with a highest-index of 3), and
    * aborting on the first 0x09 wedged such a meter forever -- every sync
    * re-requested the same bad index, got 0x09, and finished with 0 records,
    * its stored index never advancing. Treat it like a short/non-glucose frame:
    * ack, step the index past it, and keep walking to top_index. Because we
    * start the walk at index 0 (see below), whichever base the meter uses is
    * covered. */
   if (status == 0x09 && phase == P_READ) {
      LOGI("meter: record %d invalid (0x09), skipping", want_index);
      send_ack();
      last_index =
          want_index; /* permanent: an empty slot never becomes valid */
      walked++;
      if (want_index >= top_index || walked >= OT_MAX_WALK) {
         finish();
         return;
      }
      ask_record(want_index + 1);
      return;
   }
   if (status != 0x06) {
      /* 0x07 "command not allowed" is what an unbonded link reports, which is
       * the failure worth naming: everything else looks like a dead meter. */
      LOGI("meter: status 0x%02x%s", status,
           status == 0x07 ? " (not bonded -- pair the meter first)" : "");
      ot_drv_status(status == 0x07 ? "METER: NOT PAIRED" : "METER: REFUSED");
      finish();
      return;
   }
   const uint8_t *body = buf + 6;
   int blen            = total - 9; /* payload after the status byte */

   /* Handshake responses (20 02, 27 00): value unused -- ack and step to the
    * next command, exactly as the official app and xDrip do. A short or absent
    * body is fine; we never read it. */
   if (phase == P_TIME) {
      send_ack();
      ask_rcount();
      return;
   }
   if (phase == P_RCOUNT) {
      send_ack();
      ask_count();
      return;
   }

   if (phase == P_COUNT) {
      if (blen < 4) {
         finish();
         return;
      }
      unsigned raw = (unsigned)body[0] | ((unsigned)body[1] << 8U) |
                     ((unsigned)body[2] << 16U) | ((unsigned)body[3] << 24U);
      /* The record index goes out as 16 bits, so a counter above 0xFFFF can
       * never be satisfied: want_index would climb while the meter kept
       * answering index mod 65536, re-importing the same records until the
       * watchdog fired. Refuse rather than walk. */
      if (raw > 0xFFFFU) {
         LOGI("meter: implausible record counter %u, aborting sync", raw);
         ot_drv_status("METER: BAD DATA");
         finish();
         return;
      }
      top_index = (int)raw;
      send_ack();
      LOGI("meter: highest record index %d, stored through %d", top_index,
           last_index);
      /* The meter's counter going BACKWARDS means its memory was cleared
       * (OneTouch meters offer "delete all results"). Without this the stored
       * index stayed above the counter forever, every sync reported NOTHING
       * NEW, and no fingerstick was ever recorded again -- with no
       * user-reachable way to recover. Treat it as a fresh meter. */
      if (top_index < last_index) {
         LOGI("meter: counter went backwards (%d < %d); memory cleared, "
              "restarting import",
              top_index, last_index);
         last_index = -1;
      }
      /* Nothing new: leave at once rather than reading records we already
       * have. This is the common case -- most connections end here. */
      if (top_index <= last_index) {
         finish();
         return;
      }
      /* First ever sync would otherwise walk the meter's whole memory; take
       * only a recent window so a new pairing is not a multi-minute session. */
      int from = last_index + 1;
      /* First ever sync: take only a recent window rather than the meter's
       * whole memory, so a new pairing is not a multi-minute session. */
      /* + 1: the walk advance stops at `walked >= OT_MAX_WALK`, so a
       * session fetches at most OT_MAX_WALK records INCLUSIVE of `from`.
       * Starting at top - OT_MAX_WALK spans OT_MAX_WALK + 1 records, and
       * the cap fired one short of top_index -- the NEWEST record, i.e.
       * the fingerstick that prompted the pairing, was never requested
       * that session (it arrived only on the next power-on, because
       * last_index had persisted at top - 1). */
      if (last_index < 0 && top_index > OT_MAX_WALK)
         from = top_index - OT_MAX_WALK + 1;
      /* Start the walk at index 0, NOT 1. Verio firmwares disagree on the base:
       * RI_02.02.00 errors (0x09) on index 0 and stores from 1, but RI_01.09.04
       * errors on index 1 with valid records elsewhere -- so a fixed clamp to 1
       * misses a 0-indexed meter's first record. An invalid base index now just
       * returns 0x09, which the read handler SKIPS (see above) and walks past,
       * so whichever base the meter uses, the walk lands on its real records.
       */
      if (from < 0)
         from = 0;
      LOGI("meter: importing records %d..%d", from, top_index);
      ask_record(from);
      return;
   }

   if (phase == P_READ) {
      /* Refused on TIMESTAMP grounds, which is often the PHONE's clock being
       * wrong rather than the record -- see the advance rule below. */
      int ts_reject = 0;
      if (blen >= 11) {
         long ts =
             (long)((unsigned long)body[0] | ((unsigned long)body[1] << 8U) |
                    ((unsigned long)body[2] << 16U) |
                    ((unsigned long)body[3] << 24U));
         int glu = (int)((unsigned)body[4] | ((unsigned)body[5] << 8U));
         int ctrl_solution = body[6];
         unsigned tail_set = (unsigned)body[7] | (unsigned)body[8] |
                             (unsigned)body[9] | (unsigned)body[10];
         /* Plausibility bound. The frame only has to pass CRC, and the field
          * is a full u16 -- a corrupt or hostile record could deliver 65535,
          * which store_append would keep verbatim and which then skews TIR and
          * the average forever. LifeScan meters report roughly 20-600 mg/dL;
          * anything outside that is not a reading. */
         int sane = (glu >= 20 && glu <= 600);
         if (!sane && glu > 0)
            LOGI("meter: record %d glucose %d out of range, rejected",
                 want_index, glu);
         /* Bound the TIMESTAMP too, not only the value. ts is a full uint32
          * straight out of the frame; 0xFFFFFFFF converts to year ~2136, sorts
          * to the head of the history permanently (evicting a real reading),
          * persists to the log, and is re-admitted on every restart -- a
          * phantom point pinned to the right edge of every plot. Accept only a
          * plausible meter clock: 2000-01-01 (its epoch) to ~40 years on. */
         /* A LOOSE bound only, because `ts` is the meter's NAIVE LOCAL clock
          * (see otble.h) and this layer cannot convert it -- ot_drv_reading
          * does that, with meter_tz_for, and applies the exact bound there.
          *
          * A previous version compared ts + OT_EPOCH against real UTC now with
          * an hour of slack. That is local-time-read-as-UTC versus UTC, so at
          * any offset past UTC+1 a fingerstick taken THIS SECOND failed the
          * test -- all of continental Europe, Africa east of Greenwich, Asia,
          * Australia, and the UK on summer time. And the rejection is not a
          * dropped sample: last_index advances unconditionally below and is
          * persisted, so the record is never requested again and the sync
          * still reports NOTHING NEW. Every fingerstick the user ever took
          * would have been destroyed, silently.
          *
          * +/-15 h covers every real zone (UTC-12..UTC+14) with room to spare,
          * which is all this layer can honestly assert. */
         long ts_abs = ts + OT_EPOCH;
         int ts_ok   = (ts >= 0 && ts_abs <= realtime_s() + (15L * 3600));
         if (!ts_ok) {
            ts_reject = 1;
            LOGW("meter: record %d timestamp %ld implausible, rejected",
                 want_index, ts);
         }
         if (ts_ok && sane && !ctrl_solution && !tail_set) {
            /* The host applies the EXACT bound, against the converted instant
             * and its own clock; a refusal there is the same transient class.
             */
            if (ot_drv_reading(ts, glu))
               new_records++;
            else
               ts_reject = 1;
         } else {
            LOGI("meter: record %d is not a glucose reading, skipped",
                 want_index);
         }
      }
      send_ack();
      /* Advance even on a short (but CRC-valid) frame, and say so loudly.
       *
       * NOT advancing wedges the import permanently: `from` is last_index + 1,
       * so the next session asks for the same index, gets the same short frame,
       * and finishes again having made zero progress -- every later fingerstick
       * is then lost forever, not just this one. An empty or deleted record
       * slot is a plausible persistent cause. Skipping one record is strictly
       * better than skipping all of them; the log line is what makes the loss
       * visible rather than silent. */
      if (blen < 11)
         LOGW("meter: record %d short frame (%d payload bytes), SKIPPED",
              want_index, blen);
      /* ADVANCE ONLY PAST A PERMANENT PROBLEM.
       *
       * Skipping forward is right for a record that will never be readable --
       * a short frame, a control-solution result, a non-glucose event, an
       * out-of-range value. Not advancing there re-requests the same index
       * every session forever and loses every LATER fingerstick too.
       *
       * A timestamp rejection is different: it is usually the PHONE that is
       * wrong, not the record. The host bounds the converted instant against
       * realtime_s(), so a phone whose clock is slow (flat battery before NTP,
       * a dead RTC, a hand-set date) rejects perfectly good records -- and
       * persisting the advance would delete them permanently, which is the
       * same silent total data loss this gate has now caused twice. Leave the
       * index where it is so a later sync, with a correct clock, picks them
       * up. The session still walks forward, so this cannot loop. */
      /* Always advance the WALK; the rewind happens once, at finish(). An
       * earlier version skipped the assignment on a rejection, which was wrong
       * twice over: `last_index = want_index` is an assignment rather than a
       * max, so a rejection followed by any good record advanced past it
       * anyway (the retry promise only ever held for the LAST record of a
       * session), and a run of rejections longer than OT_MAX_WALK made every
       * future session re-walk the same window forever, losing every later
       * fingerstick permanently. */
      last_index = want_index;
      if (ts_reject && retry_from < 0) {
         retry_from = want_index;
         LOGW("meter: record %d timestamp refused; will rewind to it unless "
              "the whole window is refused",
              want_index);
      }
      walked++;
      if (want_index >= top_index) {
         finish();
         return;
      }
      /* Bound how long ONE connection holds the meter awake. Previously only a
       * first-ever sync was windowed, so a stale stored index against a large
       * counter meant tens of thousands of sequential round-trips -- the
       * opposite of the connect-check-disconnect battery policy in otble.h.
       * Stopping here rather than skipping forward loses nothing: last_index
       * has advanced, so the next advertisement resumes exactly where this one
       * stopped. */
      if (walked >= OT_MAX_WALK) {
         LOGI("meter: session cap %d reached at record %d, resuming next time",
              OT_MAX_WALK, want_index);
         finish();
         return;
      }
      ask_record(want_index + 1);
   }
}
