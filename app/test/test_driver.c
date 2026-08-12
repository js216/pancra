/* Offline end-to-end test of the protocol driver (dexdriver.c) with NO
 * hardware.
 *
 * A simulated Stelo runs the real J-PAKE server side (dexpair is_client=0) and
 * answers the driver's writes; the final glucose is decoded from REAL captured
 * bytes. Exercises: subscribe sequencing, round request/reassembly/chunking,
 * 02/03/04/05 auth, shared-key agreement + persistence, and EGV decode.
 *
 * Built and run by `make drivertest` (see the Makefile) -- do not hand-roll the
 * command line, or the test silently stops being built at all, which is what
 * happened for most of this file's life.
 */
#include "dexcom.h"
#include "dexdriver.h"
#include "jpake.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* driver logs go through this */
int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   va_list ap;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   printf("\n");
   va_end(ap);
   return 0;
}

/* ---- event queue (mirrors Ble's serialised delivery; avoids reentrancy) ----
 */
enum { EV_CONN, EV_WRITTEN, EV_NOTIFY, EV_DISC };

struct ev {
   int type;
   char uuid[48];
   uint8_t data[256];
   int len;
   int status;
};
static struct ev evq[1024];
static int qh, qt;

static void q_conn(void)
{
   evq[qt].type = EV_CONN;
   qt++;
}

static void q_written(const char *u, int s)
{
   struct ev *e = &evq[qt++];
   e->type      = EV_WRITTEN;
   snprintf(e->uuid, 48, "%s", u);
   e->status = s;
}

static void q_notify(const char *u, const uint8_t *d, int n)
{
   struct ev *e = &evq[qt++];
   e->type      = EV_NOTIFY;
   snprintf(e->uuid, 48, "%s", u);
   memcpy(e->data, d, n);
   e->len = n;
}

/* ---- simulated sensor ---- */
static struct jpake *sensor; /* no typedefs in this tree */
static uint8_t skey[16];
static uint8_t preset_key[16];
static int reconnect_mode;
static uint8_t drv_round[160];
static int drlen;
static int mock_round;
/* 16 bytes: the key-challenge blob the driver signs is 16 wide, and this was
 * declared as 8 while a memcpy read 16 from it -- an out-of-bounds read that
 * went unnoticed because nothing built this file. */
static const uint8_t schallenge[16] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02,
                                       0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                       0x09, 0x0a, 0x0b, 0x0c};
static int glucose_seen = -1, auth_ok = 0, key_saved_matches = -1;

/* ---- drv_* hooks: the "transport" the driver talks to ---- */
void drv_connect(const char *mac)
{
   (void)mac;
   q_conn();
}

void drv_subscribe(const char *uuid, int indicate)
{
   (void)indicate;
   q_written(uuid, 0);
}

void drv_status(const char *s)
{
   printf("   [status] %s\n", s);
}

void drv_cal_result(int result)
{
   printf("   [cal result] 0x%02x\n", result);
}

/* Capture EVERY decoded field, not just glucose. age and trend were discarded,
 * so a wrong offset in the EGV layout (or the backfill age bound) mutated
 * freely with the suite still green. */
static int glu_trend = -999, glu_age = -999;

void drv_glucose(int mg, int trend, int age)
{
   glucose_seen = mg;
   glu_trend    = trend;
   glu_age      = age;
}

static int bf_count, bf_last_age;

void drv_backfill(int mg, int trend, int age)
{
   (void)trend;
   (void)mg;
   bf_count++;
   bf_last_age = age;
}

int drv_key_load(uint8_t key[16])
{
   if (reconnect_mode) {
      memcpy(key, preset_key, 16);
      return 1;
   }
   return 0;
}

void drv_key_save(const uint8_t key[16])
{
   key_saved_matches = (memcmp(key, skey, 16) == 0);
}

/* Hooks the driver has grown since this test was last built. Keeping them here
 * (rather than letting the link fail) is the point of wiring it into `make`:
 * a new transport hook now breaks the build instead of silently going
 * untested. */
static int mac_saved, key_cleared, mac_cleared;

void drv_mac_save(const char *mac)
{
   (void)mac;
   mac_saved = 1;
}

int drv_mac_load(char *mac, int n)
{
   (void)mac;
   (void)n;
   return 0;
}

void drv_key_clear(void)
{
   key_cleared = 1;
}

void drv_mac_clear(void)
{
   mac_cleared = 1;
}

static int mock_phase; /* 0=rounds, 1=cert, 2=keychal */
static int spoof_mode; /* mock answers AuthChallenge with a bogus tokenHash */
/* Mock SKIPS AuthChallenge entirely and jumps straight to AuthStatus.
 *
 * spoof_mode only ever tested a WRONG 0x03. Nothing tested a MISSING one --
 * and the two frame handlers are independent arms, so a peer that simply never
 * sent 0x03 walked past the tokenHash check into P_STREAM. The check was half
 * a check and the suite could not see it. */
static int skip_chal_mode;
/* Every 0x34 (CALIBRATE) the driver emits. The interlocks that gate this write
 * are the app's headline safety property and had NO automated protection --
 * both could be deleted with the suite still green. */
static int cal_writes, cal_last_mgdl;
static int bounds_writes; /* 0x32 bounds requests the driver emits */

void drv_write(const char *uuid, const uint8_t *d, int n, int no_resp)
{
   (void)no_resp;
   if (!strcmp(uuid, U_CTRL) && n >= 1 && d[0] == 0x32)
      bounds_writes++;
   if (!strcmp(uuid, U_CTRL) && n >= 3 && d[0] == 0x34) {
      cal_writes++;
      cal_last_mgdl = (int)((unsigned)d[1] | ((unsigned)d[2] << 8U));
   }
   q_written(uuid, 0);
   if (!strcmp(uuid, U_AUTH) && n >= 2 && d[0] == 0x0a) { /* round request */
      mock_phase = 0;
      mock_round = d[1];
      drlen      = 0;
      uint8_t pkt[160];
      int ok = 0;
      if (mock_round == 0)
         ok = jpake_round1(sensor, pkt);
      else if (mock_round == 1)
         ok = jpake_round2(sensor, pkt);
      else
         ok = jpake_round3(sensor, pkt);
      if (!ok) {
         printf("   !! sensor round%d build failed\n", mock_round + 1);
         return;
      }
      for (int i = 0; i < 8; i++)
         q_notify(U_ROUND, pkt + ((size_t)i * 20), 20); /* sensor's round */
   } else if (!strcmp(uuid, U_ROUND) &&
              mock_phase == 0) { /* driver's round chunk */
      if (drlen + n <= 160) {
         memcpy(drv_round + drlen, d, n);
         drlen += n;
      }
      if (drlen >= 160) {
         int ok = 0;
         if (mock_round == 0)
            ok = jpake_peer_round1(sensor, drv_round);
         else if (mock_round == 1)
            ok = jpake_peer_round2(sensor, drv_round);
         else
            ok = jpake_peer_round3(sensor, drv_round);
         printf("   [sensor] driver round%d ZKP %s\n", mock_round + 1,
                ok ? "VALID" : "INVALID");
         if (mock_round == 2) {
            if (!jpake_shared_key(sensor, skey))
               printf("   !! sensor sharedkey fail\n");
         }
      }
   } else if (!strcmp(uuid, U_AUTH) && n >= 10 &&
              d[0] == 0x02) { /* AuthRequest: token */
      if (skip_chal_mode) {   /* never prove we hold the key */
         uint8_t rx[3] = {0x05, 0x01, 0x01};
         q_notify(U_AUTH, rx, 3);
         return;
      }
      uint8_t rx[17];
      rx[0] = 0x03;
      dexcom_dex8(skey, d + 1, rx + 1);
      if (spoof_mode)
         rx[1] =
             (uint8_t)(rx[1] ^ 0xffU); /* peer does NOT hold the shared key */
      memcpy(rx + 9, schallenge, 8);
      q_notify(U_AUTH, rx, 17);
   } else if (!strcmp(uuid, U_AUTH) && n >= 9 &&
              d[0] == 0x04) { /* ChallengeReply */
      uint8_t expect[8];
      dexcom_dex8(skey, schallenge, expect);
      auth_ok = (memcmp(expect, d + 1, 8) == 0);
      /* reconnect answers 05 01 01 (stream directly); pairing answers 02 (do
       * certs) */
      uint8_t rx[3] = {0x05, (uint8_t)(reconnect_mode ? 0x01 : 0x02), 0x01};
      q_notify(U_AUTH, rx, 3);
   } else if (!strcmp(uuid, U_AUTH) && n >= 2 &&
              d[0] == 0x0b) { /* certificate announce */
      mock_phase    = 1;
      uint8_t rx[7] = {0x0b, 0x00, d[1], 1,
                       0,    0,    0}; /* sensor cert idx, size=1 */
      q_notify(U_AUTH, rx, 7);
      uint8_t c = 0xab;
      q_notify(U_ROUND, &c, 1); /* 1-byte sensor cert */
   } else if (!strcmp(uuid, U_AUTH) && n >= 17 &&
              d[0] == 0x0c) { /* key-challenge announce */
      mock_phase       = 2;
      uint8_t blob[64] = {0};
      q_notify(U_ROUND, blob, 64); /* sensor's 64-byte blob (ignored) */
      uint8_t rx[18] = {0x0c, 0x00};
      memcpy(rx + 2, schallenge, 16);
      q_notify(U_AUTH, rx, 18); /* 0c 00 <16> to sign */
   } else if (!strcmp(uuid, U_AUTH) && n >= 3 &&
              d[0] == 0x0d) { /* challenge out -> accept */
      uint8_t rx[8] = {0x0d, 0x00, 0x00, 1, 2, 3, 4, 5};
      q_notify(U_AUTH, rx, 8);
   } else if (!strcmp(uuid, U_ROUND)) { /* cert/sig chunks: ignore */
                                        /* auto-acked by q_written above */
   } else if (!strcmp(uuid, U_CTRL) && n >= 1 &&
              d[0] == 0x4e) { /* getdata -> real EGV bytes */
      static const uint8_t egv[] = {0x4e, 0x00, 0x31, 0x08, 0x08, 0x00, 0xdd,
                                    0x06, 0x00, 0x01, 0x04, 0x00, 0xa5, 0x00,
                                    0x06, 0xfe, 0xa5, 0x00, 0x0f}; /* glucose
                                                                      165 */
      q_notify(U_CTRL, egv, sizeof(egv));
   }
}

static void pump(void)
{
   while (qh < qt) {
      struct ev *e = &evq[qh++];
      if (e->type == EV_CONN)
         driver_on_connected();
      else if (e->type == EV_WRITTEN)
         driver_on_written(e->uuid, e->status);

      else
         driver_on_notify(e->uuid, e->data, e->len);
   }
}

int main(void)
{
   int all = 1;
   jpake_init();

   printf("========== PAIRING (no saved key) ==========\n");
   sensor = jpake_new((const uint8_t *)"9973", 4, 0); /* server side */
   qh = qt           = 0;
   glucose_seen      = -1;
   auth_ok           = 0;
   key_saved_matches = -1;
   reconnect_mode    = 0;
   driver_init();
   driver_start("F8:DA:3F:EA:B5:F0", "9973");
   pump();
   printf("---- pairing result ----\n");
   printf("  [%s] sensor accepted our ChallengeReply (mutual auth)\n",
          auth_ok ? "PASS" : "FAIL");
   all = all && auth_ok;
   printf("  [%s] saved key equals sensor's derived key (J-PAKE agreed)\n",
          key_saved_matches == 1 ? "PASS" : "FAIL");
   all = all && (key_saved_matches == 1);
   printf("  [%s] decoded glucose from stream = %d (expect 165)\n",
          glucose_seen == 165 ? "PASS" : "FAIL", glucose_seen);
   all = all && (glucose_seen == 165);
   memcpy(preset_key, skey,
          16); /* reuse the agreed key for the reconnect test */
   jpake_free(sensor);

   printf("\n========== RECONNECT (saved key, no rounds) ==========\n");
   reconnect_mode = 1;
   memcpy(skey, preset_key, 16);
   qh = qt      = 0;
   glucose_seen = -1;
   auth_ok      = 0;
   driver_init(); /* loads preset key */
   driver_start("F8:DA:3F:EA:B5:F0", "9973");
   pump();
   printf("---- reconnect result ----\n");
   printf("  [%s] authenticated with saved key (skipped J-PAKE rounds)\n",
          auth_ok ? "PASS" : "FAIL");
   all = all && auth_ok;
   printf("  [%s] decoded glucose = %d (expect 165)\n",
          glucose_seen == 165 ? "PASS" : "FAIL", glucose_seen);
   all = all && (glucose_seen == 165);
   /* The captured frame decodes to trend -2 and age 4 (see the EGV log line).
    * Asserting them pins the field OFFSETS, not just one value. */
   printf("  [%s] decoded trend = %d (expect -2)\n",
          glu_trend == -2 ? "PASS" : "FAIL", glu_trend);
   all = all && (glu_trend == -2);
   printf("  [%s] decoded age = %d (expect 4)\n",
          glu_age == 4 ? "PASS" : "FAIL", glu_age);
   all = all && (glu_age == 4);
   /* The rest of the EGV layout, via the session snapshot the UI reads. The
    * captured frame is 4e 00 31080800 dd06 0001 0400 a500 06 fe a500:
    * clock=0x00080831, sequence=0x06dd, predicted=0x00a5 & 0x3ff. Asserting
    * these pins every remaining field offset. */
   {
      struct dex_session ds;
      driver_get_session(&ds);
      int okseq = (ds.sequence == 1757);
      int okpre = (ds.predicted == 165);
      /* session_seconds is now LIVE (the decoded clock projected forward by
       * wall time since the response), so allow the seconds the test itself
       * may consume between decode and this read -- the offset being pinned
       * is still the decode, a wrong offset is off by orders of magnitude. */
      int okclk =
          (ds.session_seconds >= 526385U && ds.session_seconds <= 526385U + 5U);
      printf("  [%s] sequence = %d (expect 1757)\n", okseq ? "PASS" : "FAIL",
             ds.sequence);
      all = all && okseq;
      printf("  [%s] predicted = %d (expect 165)\n", okpre ? "PASS" : "FAIL",
             ds.predicted);
      all = all && okpre;
      printf("  [%s] session clock = %u (expect 526385)\n",
             okclk ? "PASS" : "FAIL", ds.session_seconds);
      all = all && okclk;
   }

   /* ---- a peer that does NOT hold the shared key must be refused ----
    *
    * AuthChallenge is 03 <tokenHash8> <challenge8>; tokenHash is the sensor's
    * proof it holds the key. The driver used to answer without checking it,
    * so any peer on the locked MAC could reach P_STREAM and have its frames
    * accepted as real glucose -- feeding the big number, the ALARM and the
    * permanent log. */
   printf("\n========== SPOOFED SENSOR (bad tokenHash) ==========\n");
   spoof_mode     = 1;
   reconnect_mode = 1;
   memcpy(skey, preset_key, 16);
   qh = qt      = 0;
   glucose_seen = -1;
   auth_ok      = 0;
   driver_init();
   driver_start("F8:DA:3F:EA:B5:F0", "9973");
   pump();
   {
      int refused = (glucose_seen == -1);
      printf("  [%s] spoofed peer produced NO glucose (got %d)\n",
             refused ? "PASS" : "FAIL", glucose_seen);
      all = all && refused;
   }
   spoof_mode = 0;

   /* A peer that never sends AuthChallenge at all must ALSO be refused. */
   skip_chal_mode = 1;
   reconnect_mode = 1;
   memcpy(skey, preset_key, 16);
   qh = qt      = 0;
   glucose_seen = -1;
   auth_ok      = 0;
   driver_init();
   driver_start("F8:DA:3F:EA:B5:F0", "9973");
   pump();
   {
      int refused = (glucose_seen == -1);
      printf(
          "  [%s] peer SKIPPING AuthChallenge produced NO glucose (got %d)\n",
          refused ? "PASS" : "FAIL", glucose_seen);
      all = all && refused;
   }
   skip_chal_mode = 0;

   /* Re-establish a genuine streaming session: the refusal above left the
    * driver in P_FAIL, and the calibration interlocks below require
    * P_STREAM. */
   reconnect_mode = 1;
   memcpy(skey, preset_key, 16);
   qh = qt      = 0;
   glucose_seen = -1;
   auth_ok      = 0;
   driver_init();
   driver_start("F8:DA:3F:EA:B5:F0", "9973");
   pump();
   {
      int back = (glucose_seen == 165);
      printf("  [%s] genuine sensor still accepted after a spoof attempt\n",
             back ? "PASS" : "FAIL");
      all = all && back;
   }

   /* ---- calibration interlocks (0x34 is the only write that changes how a
    * sensor reports; it must be impossible to emit blindly) ---- */
   printf("\n========== CALIBRATION INTERLOCKS ==========\n");
   {
      struct dex_cal c;
      /* The reconnect above left the driver streaming with no 0x32 reply, so
       * cal.have is 0 -- the "never asked the sensor" state. */
      driver_get_cal(&c);
      int have0 = !c.have;
      printf("  [%s] bounds unknown until 0x32 answers (have=%d)\n",
             have0 ? "PASS" : "FAIL", c.have);
      all = all && have0;

      cal_writes = 0;
      driver_calibrate(120);
      printf("  [%s] REFUSED while bounds unknown (0x34 writes=%d)\n",
             cal_writes == 0 ? "PASS" : "FAIL", cal_writes);
      all = all && (cal_writes == 0);

      /* Answer 0x32 saying calibration is NOT permitted. */
      uint8_t nb[16] = {0x32, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0, 0, 0, 0, 1, 0};
      driver_on_notify(U_CTRL, nb, 16);
      driver_get_cal(&c);
      cal_writes = 0;
      driver_calibrate(120);
      printf("  [%s] REFUSED when firmware says not permitted (permitted=%d "
             "writes=%d)\n",
             cal_writes == 0 ? "PASS" : "FAIL", c.permitted, cal_writes);
      all = all && (cal_writes == 0);

      /* Now permit it, and check the range guard and the accepted path. */
      uint8_t yb[16] = {0x32, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0, 0, 0, 0, 1, 1};
      driver_on_notify(U_CTRL, yb, 16);
      driver_get_cal(&c);
      int permitted = c.have && c.permitted;
      printf("  [%s] bounds parsed: have=%d permitted=%d\n",
             permitted ? "PASS" : "FAIL", c.have, c.permitted);
      all = all && permitted;

      cal_writes = 0;
      driver_calibrate(39);
      printf("  [%s] REFUSED below range (39)\n",
             cal_writes == 0 ? "PASS" : "FAIL");
      all        = all && (cal_writes == 0);
      cal_writes = 0;
      driver_calibrate(401);
      printf("  [%s] REFUSED above range (401)\n",
             cal_writes == 0 ? "PASS" : "FAIL");
      all = all && (cal_writes == 0);

      cal_writes    = 0;
      cal_last_mgdl = -1;
      driver_calibrate(137);
      int ok = (cal_writes == 1 && cal_last_mgdl == 137);
      printf("  [%s] ACCEPTED in range, value on the wire = %d (expect 137)\n",
             ok ? "PASS" : "FAIL", cal_last_mgdl);
      all = all && ok;
   }

   printf("========== UNSOLICITED 0x34 ==========\n");
   {
      /* The calibration reply reuses the request opcode, and handling it
       * re-reads the bounds (a 0x32 write). If ANY 0x34 triggered that,
       * firmware echoing it -- or a post-auth peer choosing to -- sustained a
       * 0x32/0x34 ping-pong at connection-interval rate for as long as the
       * link stayed up, draining both batteries and flooding the log. */
      uint8_t rep[2] = {0x34, 0x00};
      driver_lock();
      driver_select(LINK_CGM);
      /* Consume any calibration still outstanding from the section above --
       * that one IS solicited and correctly re-reads the bounds. Measure only
       * the replies after it. */
      driver_on_notify(U_CTRL, rep, 2);
      int before = bounds_writes;
      driver_on_notify(U_CTRL, rep, 2);
      driver_on_notify(U_CTRL, rep, 2);
      driver_on_notify(U_CTRL, rep, 2);
      driver_unlock();
      printf("  [%s] three unsolicited 0x34 replies emit no 0x32 (%d new)\n",
             bounds_writes == before ? "PASS" : "FAIL", bounds_writes - before);
      all = all && (bounds_writes == before);
   }

   printf("\n%s\n", all ? "ALL DRIVER TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
