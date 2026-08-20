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

/* ---- THE TWO CLOCKS, DRIVEN BY HAND ----
 *
 * app/clock.c is deliberately NOT linked into this suite (see DRVTEST_SRC in
 * the Makefile). The driver projects the sensor's session time forward from
 * the instant the last 0x4e was received, and the bug that projection had was
 * only expressible when the WALL clock moved and the MONOTONIC one did not --
 * a phone coming back from being off, or finding a network and correcting
 * itself by an hour. With the real clocks linked in, the two can only ever be
 * observed advancing together, which is precisely the case that never broke.
 *
 * Both start at plausible-looking values rather than 0: a zero wall clock is
 * a value several gates elsewhere treat as "never", and a test whose fixture
 * is indistinguishable from "unset" pins nothing. */
static long g_mono = 1000000;
static long g_wall = 1750000000; /* mid-2025, an ordinary epoch second */

long mono_s(void)
{
   return g_mono;
}

long realtime_s(void)
{
   return g_wall;
}

long long now_ms(void)
{
   return (long long)g_mono * 1000;
}

/* ---- EVERY LINE THE DRIVER LOGS, KEPT ----
 *
 * logcat is not private. `adb logcat` reads it from any machine the phone is
 * plugged into, and an ANR or tombstone bug report carries it off the device
 * entirely. The driver handles the two things that must therefore never reach
 * it: the J-PAKE pairing code, which is the shared secret authenticating this
 * phone to that sensor, and the 16-byte key that exchange derives. The
 * sensor's BLE address is a hardware identifier for the device on the
 * wearer's arm and belongs there no more than the code does.
 *
 * A test that merely checked a log line EXISTS would prove nothing about any
 * of that, so this keeps the whole stream and the assertions are about what
 * is NOT in it. */
static char g_log[1 << 19];
static size_t g_loglen;
static int g_log_truncated;

static void log_reset(void)
{
   g_loglen        = 0;
   g_log[0]        = 0;
   g_log_truncated = 0;
}

static int log_has(const char *needle)
{
   return strstr(g_log, needle) != 0;
}

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
   /* ...and again into the capture. A second va_list, because the first is
    * consumed. */
   va_start(ap, fmt);
   size_t room = sizeof g_log - g_loglen;
   if (room > 1) {
      int n = vsnprintf(g_log + g_loglen, room, fmt, ap);
      if (n < 0 || (size_t)n >= room) {
         g_log_truncated = 1;
         g_loglen        = sizeof g_log - 1;
      } else {
         g_loglen += (size_t)n;
         if (g_loglen + 1 < sizeof g_log) {
            g_log[g_loglen++] = '\n';
            g_log[g_loglen]   = 0;
         }
      }
   } else {
      g_log_truncated = 1;
   }
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
static int key_save_fails;

/* EVERY OUTWARD ACTION the driver takes on a sensor -- a write, a connect, a
 * subscribe. Declared up here because the drv_* hooks below are the things
 * that count it. Counting writes alone let "this operation touched nothing"
 * pass for an operation that had reconnected somebody. */
static int total_actions;

/* THE LINK EVERY OUTWARD CALL WAS AIMED AT.
 *
 * Every drv_* stub used to discard its `link` argument, so the suite could
 * not see the one thing this driver's per-link contexts exist to get right:
 * setting `dc->link` to 0 for every slot -- exactly the corruption a
 * write-once field is meant to prevent -- left the whole suite green. A
 * context can be the RIGHT one and still be addressed over the WRONG wire,
 * and only the transport sees that. */
static int last_action_link = -1;

/* ---- drv_* hooks: the "transport" the driver talks to ---- */
void drv_connect(int link, const char *mac)
{
   (void)mac;
   total_actions++;
   last_action_link = link;
   q_conn();
}

void drv_subscribe(int link, const char *uuid, int indicate)
{
   (void)indicate;
   total_actions++;
   last_action_link = link;
   q_written(uuid, 0);
}

void drv_status(const char *s)
{
   printf("   [status] %s\n", s);
}

/* THE TOKEN THE REPLY CAME BACK WITH, captured rather than printed.
 *
 * The driver's promise is that the answer names the write it answers, and a
 * harness that only printed the result byte could not tell a token that
 * travels correctly from one that is fabricated at the moment the reply
 * arrives. These are what the assertions below read. */
static int cal_res_n, cal_res_id, cal_res_mgdl;
static unsigned cal_res_gen;

void drv_cal_result(int link, int result, int sensor_id, int mg_dl,
                    unsigned gen)
{
   (void)link;
   cal_res_n++;
   cal_res_id   = sensor_id;
   cal_res_mgdl = mg_dl;
   cal_res_gen  = gen;
   printf("   [cal result] 0x%02x for sensor %d value %d gen %u\n", result,
          sensor_id, mg_dl, gen);
}

/* Capture EVERY decoded field, not just glucose. age and trend were discarded,
 * so a wrong offset in the EGV layout (or the backfill age bound) mutated
 * freely with the suite still green. */
static int glu_trend = -999, glu_age = -999;

/* WHETHER THE HARNESS "ACCEPTS" A READING, and how many it has refused.
 *
 * The app's answer comes from the history (ingest.c's gate, then
 * store_record); here it is a switch, because what this suite pins is what
 * the DRIVER does with the answer -- specifically that a connection whose
 * every value was refused is not counted as a streaming one. Default 1, so
 * every existing case behaves as it always did. */
static int accept_readings = 1;
static int glu_refused, bf_refused;

int drv_glucose(int link, int mg, int trend, int age)
{
   (void)link;
   glucose_seen = mg;
   glu_trend    = trend;
   glu_age      = age;
   if (!accept_readings)
      glu_refused++;
   return accept_readings;
}

static int bf_count, bf_last_age;

int drv_backfill(int link, int mg, int trend, int age)
{
   (void)link;
   (void)trend;
   (void)mg;
   bf_count++;
   bf_last_age = age;
   if (!accept_readings)
      bf_refused++;
   return accept_readings;
}

int drv_key_load(int link, uint8_t key[16])
{
   (void)link;
   if (reconnect_mode) {
      memcpy(key, preset_key, 16);
      return 1;
   }
   return 0;
}

int drv_key_save(int link, const uint8_t key[16])
{
   (void)link;
   key_saved_matches = (memcmp(key, skey, 16) == 0);
   return key_save_fails ? -1 : 0;
}

/* Hooks the driver has grown since this test was last built. Keeping them here
 * (rather than letting the link fail) is the point of wiring it into `make`:
 * a new transport hook now breaks the build instead of silently going
 * untested. */
static int mac_saved, key_cleared, mac_cleared;

int drv_mac_save(int link, const char *mac)
{
   (void)link;
   (void)mac;
   mac_saved = 1;
   return 0;
}

int drv_mac_load(int link, char *mac, int n)
{
   (void)mac;
   (void)n;
   (void)link;
   return 0;
}

void drv_key_clear(int link)
{
   (void)link;
   key_cleared = 1;
}

void drv_mac_clear(int link)
{
   (void)link;
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
/* EVERY write, whatever it is. The counters above are opcode-specific, so a
 * driver that emitted some OTHER packet would not move them -- and "this
 * operation touched nothing" is a claim about all traffic, not about two
 * opcodes. */
static int total_writes;

void drv_write(int link, const char *uuid, const uint8_t *d, int n, int no_resp)
{
   (void)link;
   (void)no_resp;
   total_writes++;
   total_actions++;
   last_action_link = link;
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

/* WHICH LINK this run is driving. Every driver operation names its link now,
 * so the fake transport has to as well -- one section deliberately runs on
 * link 1 (link 0 legitimately keeps its key from the section before it), and
 * feeding those events to link 0 would test a different context from the one
 * that was started. */
static int test_link = LINK_CGM;

static void pump(void)
{
   while (qh < qt) {
      struct ev *e = &evq[qh++];
      if (e->type == EV_CONN)
         driver_on_connected(test_link);
      else if (e->type == EV_WRITTEN)
         driver_on_written(test_link, e->uuid, e->status);
      else
         driver_on_notify(test_link, e->uuid, e->data, e->len);
   }
}

/* The meter protocol's state, which the driver seeds when a meter link is
 * armed (driver_meter_seed_index). This harness drives the DEXCOM half only,
 * so the stub records the call and nothing else -- what matters here is that
 * the driver, not the shell, is the one that touches it. */
static int g_ot_seeded = -1;

void ot_init(int last_index)
{
   g_ot_seeded = last_index;
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
   driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
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

   printf("\n========== PAIRING KEY WRITE FAILURE =========="
          "\n");
   sensor = jpake_new((const uint8_t *)"9973", 4, 0);
   qh = qt           = 0;
   glucose_seen      = -1;
   auth_ok           = 0;
   key_saved_matches = -1;
   key_save_fails    = 1;
   reconnect_mode    = 0;
   /* A FRESH CONTEXT: link 0 legitimately retains its key from the runs
    * above, so this case is driven on link 1. Every call below names it --
    * there is no ambient selection to set any more. */
   driver_init();
   test_link        = 1;
   last_action_link = -1;
   driver_start(test_link, "F8:DA:3F:EA:B5:F0", "9973");
   pump();
   printf("  [%s] a key that was not saved is never used to authenticate\n",
          !auth_ok && glucose_seen == -1 ? "PASS" : "FAIL");
   all = all && !auth_ok && glucose_seen == -1;
   /* ...AND IT ALL WENT OUT ON LINK 1. The context is chosen by the caller's
    * link and carries that link to the transport; if the two ever disagree,
    * the right sensor's state is updated over the wrong radio link -- and
    * drv_key_save/drv_key_clear pick their file from this same number, so a
    * disagreement during auth writes one sensor's key over another's. */
   printf("  [%s] ...and every packet for it went out on link 1 (last was "
          "%d)\n",
          last_action_link == 1 ? "PASS" : "FAIL", last_action_link);
   all            = all && (last_action_link == 1);
   key_save_fails = 0;
   test_link      = LINK_CGM;
   jpake_free(sensor);

   printf("\n========== RECONNECT (saved key, no rounds) ==========\n");
   reconnect_mode = 1;
   memcpy(skey, preset_key, 16);
   qh = qt      = 0;
   glucose_seen = -1;
   auth_ok      = 0;
   driver_init(); /* loads preset key */
   driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
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
      driver_session_of(LINK_CGM, &ds);
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
   driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
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
   driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
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
   driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
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
      driver_cal_of(LINK_CGM, &c);
      int have0 = !c.have;
      printf("  [%s] bounds unknown until 0x32 answers (have=%d)\n",
             have0 ? "PASS" : "FAIL", c.have);
      all = all && have0;

      cal_writes = 0;
      driver_calibrate(LINK_CGM, 120, 41, 1);
      printf("  [%s] REFUSED while bounds unknown (0x34 writes=%d)\n",
             cal_writes == 0 ? "PASS" : "FAIL", cal_writes);
      all = all && (cal_writes == 0);

      /* Answer 0x32 saying calibration is NOT permitted. */
      uint8_t nb[16] = {0x32, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0, 0, 0, 0, 1, 0};
      driver_on_notify(LINK_CGM, U_CTRL, nb, 16);
      driver_cal_of(LINK_CGM, &c);
      cal_writes = 0;
      driver_calibrate(LINK_CGM, 120, 41, 2);
      printf("  [%s] REFUSED when firmware says not permitted (permitted=%d "
             "writes=%d)\n",
             cal_writes == 0 ? "PASS" : "FAIL", c.permitted, cal_writes);
      all = all && (cal_writes == 0);

      /* Now permit it, and check the range guard and the accepted path. */
      uint8_t yb[16] = {0x32, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0, 0, 0, 0, 1, 1};
      driver_on_notify(LINK_CGM, U_CTRL, yb, 16);
      driver_cal_of(LINK_CGM, &c);
      int permitted = c.have && c.permitted;
      printf("  [%s] bounds parsed: have=%d permitted=%d\n",
             permitted ? "PASS" : "FAIL", c.have, c.permitted);
      all = all && permitted;

      cal_writes = 0;
      driver_calibrate(LINK_CGM, 39, 41, 3);
      printf("  [%s] REFUSED below range (39)\n",
             cal_writes == 0 ? "PASS" : "FAIL");
      all        = all && (cal_writes == 0);
      cal_writes = 0;
      driver_calibrate(LINK_CGM, 401, 41, 4);
      printf("  [%s] REFUSED above range (401)\n",
             cal_writes == 0 ? "PASS" : "FAIL");
      all = all && (cal_writes == 0);

      cal_writes    = 0;
      cal_last_mgdl = -1;
      driver_calibrate(LINK_CGM, 137, 41, 7);
      int ok = (cal_writes == 1 && cal_last_mgdl == 137);
      printf("  [%s] ACCEPTED in range, value on the wire = %d (expect 137)\n",
             ok ? "PASS" : "FAIL", cal_last_mgdl);
      all = all && ok;

      /* ---- THE ANSWER NAMES THE WRITE IT ANSWERS -----------------------
       *
       * The driver used to keep one BOOLEAN -- "a 0x34 we sent is awaiting a
       * reply" -- and hand the shell nothing but the result byte, so the shell
       * resolved whatever calibration happened to be queued when the reply
       * landed. What that did to the person holding the phone is in
       * dexdriver.h: replace 100 with 180 before the sensor answers, and 180
       * is recorded ACCEPTED without one byte of it ever going on the wire,
       * while the sensor keeps reporting against 100.
       *
       * THE DRIVER'S HALF of the fix is exactly this: whatever it was told
       * when it wrote comes back with the reply, UNCHANGED. It cannot be
       * checked by asserting that a reply arrives -- that already worked. It
       * is checked by writing a token that could not be guessed from the reply
       * and reading it back. */
      cal_res_n      = 0;
      cal_res_id     = -1;
      cal_res_mgdl   = -1;
      cal_res_gen    = 0;
      uint8_t acc[2] = {0x34, 0x00};
      driver_on_notify(LINK_CGM, U_CTRL, acc, 2);
      int tok = (cal_res_n == 1 && cal_res_id == 41 && cal_res_mgdl == 137 &&
                 cal_res_gen == 7);
      printf("  [%s] the reply carries the write's own token back "
             "(n=%d id=%d value=%d gen=%u; expect 1/41/137/7)\n",
             tok ? "PASS" : "FAIL", cal_res_n, cal_res_id, cal_res_mgdl,
             cal_res_gen);
      all = all && tok;

      /* A SECOND WRITE REPLACES THE RECORD, it does not add to it. Only one
       * 0x34 is outstanding per link at a time, and the reply must name the
       * LATEST write -- a record that kept the first would answer a live write
       * with a dead token, which the shell would then (correctly) discard, and
       * a calibration that really was sent would never resolve. */
      cal_writes = 0;
      driver_calibrate(LINK_CGM, 142, 43, 8);
      cal_res_n = 0;
      driver_on_notify(LINK_CGM, U_CTRL, acc, 2);
      int tok2 = (cal_res_n == 1 && cal_res_id == 43 && cal_res_mgdl == 142 &&
                  cal_res_gen == 8);
      printf("  [%s] a second write replaces the token (id=%d value=%d "
             "gen=%u; expect 43/142/8)\n",
             tok2 ? "PASS" : "FAIL", cal_res_id, cal_res_mgdl, cal_res_gen);
      all = all && tok2;

      /* ...AND IT REPLACES IT WHILE THE FIRST IS STILL OUTSTANDING, which is
       * the case that separates "written on every send" from "written on the
       * first send of a run". The module retries a calibration whose reply
       * never came (one write a minute), so two writes with no reply between
       * them is the ordinary retry, not a contrived state -- and the reply that
       * finally arrives must name the LATEST write. A record filled in only
       * when nothing was pending would hand back the FIRST write's token, the
       * shell would discard it as stale, and a calibration that really was sent
       * would sit PENDING until its window lapsed and then report FAILED.
       *
       * The expected token is stated here, BEFORE either write, so this case
       * cannot ask the implementation what it thinks the answer is. */
      const int want_id = 47, want_mg = 155;
      const unsigned want_gen = 12;
      cal_writes              = 0;
      driver_calibrate(LINK_CGM, 149, 46, 11); /* no reply for this one */
      driver_calibrate(LINK_CGM, want_mg, want_id, want_gen);
      cal_res_n = 0;
      driver_on_notify(LINK_CGM, U_CTRL, acc, 2);
      int tok3 = (cal_writes == 2 && cal_res_n == 1 && cal_res_id == want_id &&
                  cal_res_mgdl == want_mg && cal_res_gen == want_gen);
      printf(
          "  [%s] a resend while the first is still outstanding replaces "
          "the token (writes=%d id=%d value=%d gen=%u; expect 2/47/155/12)\n",
          tok3 ? "PASS" : "FAIL", cal_writes, cal_res_id, cal_res_mgdl,
          cal_res_gen);
      all = all && tok3;

      /* AND ONLY ONE ANSWER COMES OUT OF TWO WRITES. The record is one slot,
       * so the second reply has nothing pending to match and is treated as the
       * unsolicited frame it is -- otherwise the first write's dead token would
       * reach the shell a moment later and be discarded there, which is safe
       * but is the driver passing its own bookkeeping problem downstream. */
      cal_res_n = 0;
      driver_on_notify(LINK_CGM, U_CTRL, acc, 2);
      int tok4 = (cal_res_n == 0);
      printf("  [%s] the second reply to two writes is unsolicited, not a "
             "stale token (results=%d)\n",
             tok4 ? "PASS" : "FAIL", cal_res_n);
      all = all && tok4;

      /* AND A REFUSED WRITE LEAVES NO RECORD BEHIND. driver_calibrate refuses
       * an out-of-range value without writing anything, so there is no answer
       * owing -- and an unsolicited 0x34 after it must not be reported as the
       * refused calibration's reply. (The record is left holding the last
       * write's token, but `pending` is what decides whether it is used.) */
      cal_writes = 0;
      driver_calibrate(LINK_CGM, 401, 45, 9);
      cal_res_n = 0;
      driver_on_notify(LINK_CGM, U_CTRL, acc, 2);
      int norec = (cal_writes == 0 && cal_res_n == 0);
      printf("  [%s] a REFUSED calibration leaves nothing awaiting a reply "
             "(writes=%d results=%d)\n",
             norec ? "PASS" : "FAIL", cal_writes, cal_res_n);
      all = all && norec;
   }

   printf("========== UNSOLICITED 0x34 ==========\n");
   {
      /* The calibration reply reuses the request opcode, and handling it
       * re-reads the bounds (a 0x32 write). If ANY 0x34 triggered that,
       * firmware echoing it -- or a post-auth peer choosing to -- sustained a
       * 0x32/0x34 ping-pong at connection-interval rate for as long as the
       * link stayed up, draining both batteries and flooding the log. */
      uint8_t rep[2] = {0x34, 0x00};
      /* Consume any calibration still outstanding from the section above --
       * that one IS solicited and correctly re-reads the bounds. Measure only
       * the replies after it. */
      driver_on_notify(LINK_CGM, U_CTRL, rep, 2);
      int before = bounds_writes;
      driver_on_notify(LINK_CGM, U_CTRL, rep, 2);
      driver_on_notify(LINK_CGM, U_CTRL, rep, 2);
      driver_on_notify(LINK_CGM, U_CTRL, rep, 2);
      printf("  [%s] three unsolicited 0x34 replies emit no 0x32 (%d new)\n",
             bounds_writes == before ? "PASS" : "FAIL", bounds_writes - before);
      all = all && (bounds_writes == before);
   }

   /* ---- STREAMING IS A CLAIM ABOUT THE HISTORY, NOT ABOUT THE DECODE ----
    *
    * The driver marked a sensor as streaming -- clearing the failure streak
    * that exists to notice a sensor going bad, and PERSISTING its address as
    * the one every future launch reconnects to -- the moment a 0x4e decoded
    * or a backfill notification carried any records at all. Decoding says the
    * bytes had the right shape and nothing more. The app still refuses a
    * value outside what a sensor can report, refuses a frame whose age
    * backdates it, and drops a reading entirely when no registered slot
    * claims the link yet and another CGM is already live -- and on any of
    * those the user's screen does not move and the plot gains no point.
    *
    * THE ISOLATING CASE IS THE ALL-REFUSED ONE. Asserting that an ACCEPTED
    * batch marks the sensor streamed proves nothing: that always worked. */
   printf("\n========== A BATCH NOTHING WAS ACCEPTED FROM ==========\n");
   {
      reconnect_mode = 1;
      memcpy(skey, preset_key, 16);
      qh = qt         = 0;
      glucose_seen    = -1;
      auth_ok         = 0;
      mac_saved       = 0;
      glu_refused     = 0;
      bf_refused      = 0;
      bf_count        = 0;
      accept_readings = 0; /* the app keeps none of it */
      /* A LINK OF ITS OWN, AND A CONTEXT WIPED CLEAN BEFORE THE KEY IS PUT
       * BACK. The sections above left link 0 already streamed and already
       * remembered, and both of those are latched per context: a second
       * remember_sensor is a no-op and a second streamed=1 changes nothing,
       * so every assertion below would have been made against state that a
       * PREVIOUS connection set. driver_forget is what clears the pair of
       * them; the driver_init after it reloads the saved key, which is what
       * lets this connection take the bonded path. */
      test_link = 3;
      driver_forget(test_link);
      driver_init();
      log_reset();
      driver_start(test_link, "F8:DA:3F:EA:B5:F0", "9973");
      pump();

      /* THE FRAME REALLY DID ARRIVE AND REALLY WAS DECODED. Without this the
       * case below could pass because the sensor never got as far as sending
       * a reading -- a test that pins a rule while a LATER check refuses the
       * input anyway. */
      int reached = (glucose_seen == 165) && (glu_refused == 1);
      printf("  [%s] the EGV decoded and reached the app, which refused it\n",
             reached ? "PASS" : "FAIL");
      all = all && reached;

      printf("  [%s] ...so the sensor's address was NOT persisted\n",
             !mac_saved ? "PASS" : "FAIL");
      all = all && !mac_saved;

      /* AND THE SAME FOR A BACKFILL BATCH. Two real captured records, both
       * dated at or before the session clock the 0x4e above established, so
       * the driver's own age bounds pass them: what refuses them is the app.
       * `k > 0` used to be the whole test here. */
      static const uint8_t bfrec[18] = {0x2d, 0x08, 0x08, 0x00, 0xa5, 0x00,
                                        0x06, 0x0f, 0xfe, 0x2d, 0x08, 0x08,
                                        0x00, 0xa5, 0x00, 0x06, 0x0f, 0xfe};
      driver_on_notify(test_link, U_DATA, bfrec, 18);
      int bfreached = (bf_count == 2) && (bf_refused == 2);
      printf("  [%s] both backfill records reached the app, which refused "
             "them\n",
             bfreached ? "PASS" : "FAIL");
      all = all && bfreached;
      printf("  [%s] ...and a nonempty batch nothing was kept from still does "
             "not persist the sensor\n",
             !mac_saved ? "PASS" : "FAIL");
      all = all && !mac_saved;

      /* THE FAILURE STREAK. A connection that produced no record is a failed
       * connection, and the streak is what eventually stops the app hammering
       * a sensor that can talk but cannot deliver. */
      driver_on_disconnected(test_link, 19);
      printf("  [%s] ...and the connection counts as a FAILURE (streak 1)\n",
             log_has("fail streak 1") ? "PASS" : "FAIL");
      all = all && log_has("fail streak 1");

      /* THE CONTROL, which is only worth anything beside the case above: the
       * identical connection with the app KEEPING the reading does persist
       * the sensor and does clear the streak. */
      accept_readings = 1;
      qh = qt      = 0;
      glucose_seen = -1;
      mac_saved    = 0;
      log_reset();
      driver_start(test_link, "F8:DA:3F:EA:B5:F0", "9973");
      pump();
      printf("  [%s] control: an ACCEPTED reading does persist the sensor\n",
             mac_saved ? "PASS" : "FAIL");
      all = all && mac_saved;
      driver_on_disconnected(test_link, 19);
      printf("  [%s] ...and clears the failure streak\n",
             log_has("fail streak 0") ? "PASS" : "FAIL");
      all       = all && log_has("fail streak 0");
      test_link = LINK_CGM;
   }

   /* ---- THE SESSION CLOCK IS PROJECTED FROM ELAPSED TIME ----
    *
    * The sensor answers every ~5 minutes; the warmup countdown and the
    * end-of-session test both read its clock projected forward from that
    * answer. The projection used to be the difference of two WALL-clock
    * stamps, cast to uint32_t and added to an unsigned clock -- so a phone
    * correcting itself backwards an hour (coming back from being off, or
    * finding a network) moved the sensor's session by an hour it never had,
    * and for a young session wrapped it to ~4.29 billion seconds.
    *
    * This suite links its own clocks precisely so the two can DISAGREE. */
   printf("\n========== A WALL-CLOCK JUMP MOVES NO SESSION ==========\n");
   {
      reconnect_mode  = 1;
      accept_readings = 1;
      memcpy(skey, preset_key, 16);
      qh = qt      = 0;
      glucose_seen = -1;
      driver_init();
      driver_start(LINK_CGM, "F8:DA:3F:EA:B5:F0", "9973");
      pump();

      struct dex_session s0;
      driver_session_of(LINK_CGM, &s0);
      int base = (s0.session_seconds == 526385U);
      printf("  [%s] the captured frame's session clock is %u (expect "
             "526385)\n",
             base ? "PASS" : "FAIL", s0.session_seconds);
      all = all && base;

      /* THE ISOLATING CASE. The wall clock steps back one hour; nothing else
       * changes. The old arithmetic returned 526385 - 3600 = 522785 here, and
       * for a sensor still inside its warmup hour it wrapped instead. */
      g_wall -= 3600;
      struct dex_session s1;
      driver_session_of(LINK_CGM, &s1);
      int held = (s1.session_seconds == 526385U);
      printf("  [%s] an hour BACKWARD on the wall clock leaves the session at "
             "%u\n",
             held ? "PASS" : "FAIL", s1.session_seconds);
      all = all && held;

      /* ...and forward, which is the same correction in the direction that
       * used to end a session early. */
      g_wall += 7200;
      struct dex_session s2;
      driver_session_of(LINK_CGM, &s2);
      int held2 = (s2.session_seconds == 526385U);
      printf("  [%s] two hours FORWARD leaves it at %u too\n",
             held2 ? "PASS" : "FAIL", s2.session_seconds);
      all = all && held2;

      /* ELAPSED TIME STILL MOVES IT -- otherwise the countdown would simply
       * be frozen, which passes both cases above and is just as wrong. */
      g_mono += 300;
      struct dex_session s3;
      driver_session_of(LINK_CGM, &s3);
      int ticks = (s3.session_seconds == 526385U + 300U);
      printf("  [%s] five minutes of ELAPSED time advance it to %u\n",
             ticks ? "PASS" : "FAIL", s3.session_seconds);
      all = all && ticks;
      g_wall -= 3600; /* leave the two clocks as they started */
      g_mono -= 300;
   }

   /* ---- NOTHING SECRET, AND NOTHING IDENTIFYING, REACHES THE LOG ----
    *
    * See the capture buffer at the top of this file for why. The pairing here
    * is driven with a code eight digits long rather than the four a real
    * applicator prints: the log carries hex dumps of the J-PAKE round
    * payloads, and a four-hex-digit needle would collide with random bytes
    * often enough to make this test flaky. Eight makes a chance hit a 2e-10
    * event, so a failure here is the driver's doing and not the dice's. */
   printf("\n========== NOTHING SECRET REACHES THE LOG ==========\n");
   {
      static const char *const mac  = "F8:DA:3F:EA:B5:F1";
      static const char *const code = "99731864";
      /* A LINK OF ITS OWN, so forgetting the key below cannot disturb the
       * primary context the last section asserts against. */
      test_link = 2;
      /* NOT jpake_free(sensor) first: the reconnect sections above ran with
       * a saved key and never allocated a mock, so the handle is stale by
       * here. A leak in a test that is about to exit costs nothing; a double
       * free costs the whole suite. */
      sensor = jpake_new((const uint8_t *)code, 8, 0);
      qh = qt           = 0;
      glucose_seen      = -1;
      auth_ok           = 0;
      key_saved_matches = -1;
      reconnect_mode    = 0; /* a FRESH pairing: the rounds, and the key */
      accept_readings   = 1;
      mac_saved         = 0;
      driver_init();
      /* AND A CONTEXT WITH NO KEY IN IT. driver_init only LOADS keys, it does
       * not clear the ones a previous section left behind -- and the
       * reconnect cases above put one in every link. Without this the driver
       * takes the bonded fast path, no rounds run, no key is derived, and
       * the "the key never appears" assertion would be hunting for something
       * that was never computed: a test that cannot fail. */
      driver_forget(test_link);
      log_reset();
      driver_start(test_link, mac, code);
      pump();

      /* THE CAPTURE IS REAL, AND WHOLE. Without both of these every "does not
       * contain" below would pass on an empty buffer, which is the shape of
       * an assertion that cannot fail. */
      int logged = !g_log_truncated && log_has("driver_start") &&
                   log_has("shared key derived");
      printf("  [%s] the pairing was logged, and the capture did not "
             "overflow\n",
             logged ? "PASS" : "FAIL");
      all        = all && logged;
      int paired = (key_saved_matches == 1) && (glucose_seen == 165);
      printf("  [%s] ...and it was a real pairing that streamed\n",
             paired ? "PASS" : "FAIL");
      all = all && paired;

      printf("  [%s] the pairing CODE never appears in the log\n",
             !log_has(code) ? "PASS" : "FAIL");
      all = all && !log_has(code);

      printf("  [%s] the sensor's ADDRESS never appears in the log\n",
             !log_has(mac) ? "PASS" : "FAIL");
      all = all && !log_has(mac);

      /* THE DERIVED KEY, as loghex would have printed it. This is the exact
       * line that used to be here -- loghex("SHAREDKEY(derived)", ...) --
       * and 32 hex characters cannot collide with anything by accident. */
      char keyhex[33];
      for (int i = 0; i < 16; i++)
         snprintf(keyhex + (i * 2), 3, "%02x", skey[i]);
      printf("  [%s] the derived shared key never appears in the log\n",
             !log_has(keyhex) ? "PASS" : "FAIL");
      all = all && !log_has(keyhex);
      /* ...and the harness's own needle is a real one: it is the key the
       * sensor agreed, which the driver did save. */
      printf("  [%s] (the needle is the key that was actually agreed)\n",
             key_saved_matches == 1 ? "PASS" : "FAIL");
      all       = all && (key_saved_matches == 1);
      test_link = LINK_CGM;
   }

   printf("========== A LINK THIS DRIVER DOES NOT HAVE ==========\n");
   {
      /* An operation addressed at an out-of-range link must do NOTHING.
       *
       * It used to do something far worse than nothing: the ambient selector
       * clamped any invalid link to LINK_CGM, so every one of these calls was
       * quietly re-aimed at the PRIMARY sensor's context -- the one context
       * whose corruption cannot be recovered without re-pairing. A stray
       * driver_forget(-1) wiped the primary's key and MAC; a stray
       * driver_on_disconnected(LINK_MAX, s) advanced its reconnect state.
       * Nothing in the callback path guarantees a well-formed link: it
       * arrives from Java, indexed by whatever the framework handed back.
       *
       * The assertion is deliberately made against the PRIMARY's session,
       * because that is the context the old clamp aimed at. */
      static const int bad[] = {-1, -99, LINK_MAX, LINK_MAX + 1, LINK_MAX + 7};

      /* CALIBRATION FIRST, while the primary is still STREAMING and
       * permitted -- which is the only state in which a clamped bad link
       * would have written one. Asked after the loop below, the primary has
       * been driven to P_IDLE by the stray driver_forget(), so a clamped call
       * refuses for an honest reason and the assertion passes either way:
       * exactly the shape of a test that cannot fail. */
      int cal0    = cal_writes;
      int refused = 1;
      for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
         if (driver_calibrate(bad[i], 120, 41, 9) != 0)
            refused = 0;
      refused = refused && (cal_writes == cal0);
      printf("  [%s] driver_calibrate refuses a bad link, and writes no "
             "calibration for it\n",
             refused ? "PASS" : "FAIL");
      all = all && refused;

      struct dex_session before;
      driver_session_of(LINK_CGM, &before);
      int act0 = total_actions;
      for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
         int b = bad[i];
         driver_start(b, "AA:BB:CC:DD:EE:FF", "1234");
         driver_on_connected(b);
         driver_on_written(b, U_CTRL, 0);
         driver_on_notify(b, U_CTRL, (const uint8_t *)"\x4e\x00", 2);
         driver_on_disconnected(b, 19);
         driver_kick(b);
         driver_forget(b);
         driver_request_backfill(b, 3600);
         driver_cal_bounds(b);
         driver_lock_mac(b, "AA:BB:CC:DD:EE:FF");
      }

      struct dex_session after;
      driver_session_of(LINK_CGM, &after);
      int same = memcmp(&before, &after, sizeof before) == 0;
      printf("  [%s] a bad link leaves the PRIMARY session untouched\n",
             same ? "PASS" : "FAIL");
      all = all && same;

      int quiet = (total_actions == act0);
      printf("  [%s] ...and does nothing to any sensor (%d actions)\n",
             quiet ? "PASS" : "FAIL", total_actions - act0);
      all = all && quiet;
   }

   printf("\n%s\n", all ? "ALL DRIVER TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
