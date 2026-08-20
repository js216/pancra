// SPDX-License-Identifier: GPL-3.0
// pairingtest.c --- Host tests for advert -> candidate -> registered device
// Copyright 2026 Jakob Kastelic
//
/* THE ONE WORKFLOW THAT TURNS A STRANGER INTO A DEVICE, and the only one
 * whose mistakes are permanent: the row it mints is append-only, so a wrong
 * type or the wrong candidate picked out of a churning list cannot be
 * corrected afterwards -- and a pairing committed against a sensor that is
 * already streaming burns the link it was using.
 *
 * It had no test of its own. pairing.c is linked into the broad test binaries
 * and reached from them only incidentally, so the candidate list, the
 * freshness window, the duplicate rule, the already-registered exclusion and
 * the armed-pairing tick were all unexercised.
 *
 * THE ADVERT PATH IS DRIVEN THROUGH JNI, exactly as Android drives it: the
 * test registers the natives with a fake JNIEnv, keeps the function pointer
 * pairing.c handed over, and calls it. Nothing here reaches into the module's
 * private state; it is all the header plus that one callback, which is the
 * whole interface the radio has.
 *
 * The clock is faked (see realtime_s below), because the rules under test are
 * about time passing: a candidate heard a minute ago must not be compared
 * against one heard now.
 *
 * Built and run by `make pairingtest`.
 */
#include "pairing.h"
#include "clock.h"
#include "forms.h"   /* the keypad entry the pairing flow reads */
#include "nav.h"     /* cur_screen / nav_go: the flow moves between screens */
#include "sensors.h" /* the registry the commit writes into */
#include "store.h"   /* g_hist: "is this sensor already streaming" */
#include "testdir.h" /* test_dir: the per-mode fixture directory */
#include "uimodel.h" /* SCR_* / CAL_ST_*: the frame's own vocabulary */
#include <jni.h>
#include <jni_md.h> /* jint: the advert callback's RSSI crosses as one */
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int all = 1;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      all = 0;
}

/* ---- the clock ---------------------------------------------------------- */

/* TWO FAKE CLOCKS, MOVED SEPARATELY. util.c is compiled with these three
 * renamed for this binary, so these are the only clocks in it -- see the
 * Makefile target.
 *
 * They used to be one variable, which made the test blind to the whole class
 * of defect it is here to catch: every interval in this file measured against
 * a clock the user (or NTP) can move. With one variable, a "wall-clock jump"
 * moved the monotonic clock too, and a candidate that should have been
 * unaffected went stale exactly as the defect would have made it.
 *
 * g_real identifies INSTANTS (a reading's timestamp, a meter's LAST SEEN);
 * g_mono measures INTERVALS (freshness, throttles, cooldowns). Nothing in the
 * app may use the first for the second. */
static long g_real = 1700000000;
static long g_mono = 4000;

long realtime_s(void)
{
   return g_real;
}

long mono_s(void)
{
   return g_mono;
}

long long now_ms(void)
{
   return (long long)g_mono * 1000;
}

/* Time really passing: both clocks advance together, which is the only way
 * they ever move in the ordinary case. */
static void tick(long secs)
{
   g_real += secs;
   g_mono += secs;
}

/* ---- the app around it -------------------------------------------------- */

#include "bletrans.h"
#include "dexdriver.h"
#include "meter.h"
#include "settings.h"
#include "shell.h"

/* The keypad's buffer, and where closing it returns to. */
/* THE KEYPAD, faked through the same small interface the app uses -- the
 * buffer itself is private to forms.c now, and a stub that redefined it would
 * be testing a shape the app no longer has. */
static char g_typed[64];
static int g_typedlen;
static enum ui_screen g_kp_ret;

int forms_kp_len(void)
{
   return g_typedlen;
}

void forms_kp_text(char *out, int cap)
{
   int n = 0;
   if (!out || cap <= 0)
      return;
   for (; n < g_typedlen && n < cap - 1; n++)
      out[n] = g_typed[n];
   out[n] = 0;
}

void forms_kp_clear(void)
{
   g_typedlen = 0;
}

void forms_kp_type(char c)
{
   if (g_typedlen < (int)sizeof g_typed - 1)
      g_typed[g_typedlen++] = c;
}

void forms_kp_return_set(enum ui_screen ret)
{
   g_kp_ret = ret;
}

enum ui_screen forms_kp_return(void)
{
   return g_kp_ret;
}

/* What a test types. */
static void kp_set(const char *s)
{
   forms_kp_clear();
   for (int i = 0; s[i]; i++)
      forms_kp_type(s[i]);
}

/* The reading tail, which the advert path consults to decide whether a sensor
 * is already streaming. EMPTY here: nothing is. Faked through the same two
 * queries the app uses, so this stub cannot drift from the real one's shape
 * (it used to be the array itself, which is private now). */
int hist_count(void)
{
   return 0;
}

struct reading hist_at(int i)
{
   struct reading z = {0};
   (void)i;
   return z;
}

void store_lock(void)
{
}

void store_unlock(void)
{
}

static enum ui_screen g_screen = SCR_MAIN;

enum ui_screen cur_screen(void)
{
   return g_screen;
}

static int g_add_type = SENSOR_STELO;

int sel_add_type(void)
{
   return g_add_type;
}

void sel_set_add_type(int type)
{
   g_add_type = type;
}

static int g_selected_id = -1;

void sel_set_device(int id)
{
   g_selected_id = id;
}

static int g_nav_to = -1;

void nav_go(enum ui_screen to)
{
   g_nav_to = to;
}

static int g_keypad_closed;

void keypad_close(void)
{
   g_keypad_closed++;
}

/* THE DRIVER, faked down to what pairing.c asks it: which link a slot owns,
 * whether that link has a bonded session, and the two radio calls. */
static struct dex_session g_sess[LINK_MAX];
static int g_slot_link[MAX_SLOTS];

void driver_lock(void)
{
}

void driver_unlock(void)
{
}

void driver_session_of(int link, struct dex_session *out)
{
   if (!out)
      return;
   *out =
       (link >= 0 && link < LINK_MAX) ? g_sess[link] : (struct dex_session){0};
}

static int g_forgotten = -1;

void driver_forget(int link)
{
   g_forgotten = link;
}

/* One link per slot, in order, which is what the real allocator does for
 * CGMs. -1 means "no free link", the case commit_pair has to refuse on. */
static int link_for_slot(int idx)
{
   if (idx < 0 || idx >= MAX_SLOTS)
      return -1;
   return g_slot_link[idx];
}

/* THE PRODUCTION SHAPE: the caller names a device and the index never leaves
 * this file. sensors.c is linked, so the id resolves against the real
 * registry -- which is what makes the "a forget shifts the table" cases
 * below exercise the same resolution the app does. */
int link_for_sensor(int id)
{
   struct sensor_view v;
   sensors_view_get(&v);
   for (int i = 0; i < v.n; i++)
      if (v.slot[i].id == id)
         return link_for_slot(i);
   return -1;
}

/* THE LINK A NEW SENSOR WOULD TAKE: link_for_slot for one past the last slot.
 * The real one (reconcile.c) reads the count from the registry snapshot it
 * also walks; here the registry is real -- sensors.c is linked -- so the
 * stub asks it the same question, and pairing's "no free link" case is still
 * reached by giving the slot after the last one no link. */
int link_for_new_sensor(void)
{
   struct sensor_view v;
   sensors_view_get(&v);
   return link_for_slot(v.n);
}

/* The two radio calls, recorded rather than made. */
static int g_paired_link = -1;
static char g_paired_mac[20];
static char g_paired_code[8];
static int g_pairs;

void dexble_pair(int link, const char *mac, const char *code)
{
   g_pairs++;
   g_paired_link = link;
   snprintf(g_paired_mac, sizeof g_paired_mac, "%s", mac ? mac : "");
   snprintf(g_paired_code, sizeof g_paired_code, "%s", code ? code : "");
}

static int g_bonds;

int dexble_create_bond(const char *mac)
{
   (void)mac;
   g_bonds++;
   return 1;
}

/* The meter side, which shares the advert path. */
static int g_meter_busy_flag;
static int g_meter_syncs;
static char g_meter_sync_mac[20];
static long g_meter_last_seen;
static int g_meter_notes;

int meter_busy(void)
{
   return g_meter_busy_flag;
}

int meter_armed(const char *mac)
{
   (void)mac;
   return 0;
}

void meter_sync_start(int mid, const char *mac)
{
   (void)mid;
   g_meter_syncs++;
   snprintf(g_meter_sync_mac, sizeof g_meter_sync_mac, "%s", mac ? mac : "");
}

long meter_seen(int id)
{
   (void)id;
   return g_meter_last_seen;
}

/* The same event on the monotonic clock -- what the advert throttle actually
 * measures against (see meterstore.h). Kept separate here for the same reason
 * it is separate there: a test that returns one clock for both cannot tell a
 * throttle from a wall-clock comparison. */
static long g_meter_last_mono;

/* THE THROTTLE LIVES HERE NOW, as it does in the real store: the caller used
 * to read the last stamp, decide, and record, and two scan callbacks for one
 * meter could both pass. The stub answers the same question -- did THIS
 * advert take the turn -- so the suite still tests the caller's behaviour and
 * not a rule the caller no longer owns. */
int meter_note_advert(int id, int rssi, long now, long window)
{
   (void)id;
   (void)rssi;
   long m = mono_s();
   if (g_meter_last_mono != 0 && m - g_meter_last_mono <= window)
      return 0;
   g_meter_last_seen = now; /* the instant, as the screen shows it */
   g_meter_last_mono = m;   /* ...and the interval stamp */
   g_meter_notes++;
   return 1;
}

int meter_pair(int id, const char *mac)
{
   (void)id;
   (void)mac;
   return 0;
}

void meter_bind(int id, const char *mac)
{
   (void)id;
   (void)mac;
}

/* Preferences: only the pairing code is read here. */
static struct prefs g_prefs;

/* THE COPY, which is the only way in now: the live aggregate is private to
 * settings.c, because a pointer into it can be read incoherently and
 * rewritten under its reader (see settings.h). */
void settings_get(struct prefs *out)
{
   *out = g_prefs;
}

int settings_set_code(const char *digits)
{
   snprintf(g_prefs.code_str, sizeof g_prefs.code_str, "%s", digits);
   return SETTINGS_OK; /* this stub's storage cannot fail */
}

/* THE ACTIVITY, as an opaque token. pairing.c only ever passes it to
 * start_scan/stop_scan, both faked here, so it never has to be a real one --
 * but it must not be NULL: the radio calls are conditional on there being an
 * activity, and a test with none would prove that pairing does nothing. */
static char g_act_mem[8];
static int g_activity_present = 1;

struct ANativeActivity *shell_activity(void)
{
   return g_activity_present ? (struct ANativeActivity *)g_act_mem : 0;
}

/* ATOMIC: the advert threads below call this, and so does the main one. A
 * plain int here is a race in the TEST rather than the code under test, and
 * under ThreadSanitizer that is indistinguishable from a real finding. */
static atomic_int g_dirty;

void shell_ui_dirty(void)
{
   atomic_fetch_add(&g_dirty, 1);
}

static char g_status[32];

void set_status(const char *s)
{
   snprintf(g_status, sizeof g_status, "%s", s ? s : "");
}

static long g_hold_until;

void scan_hold_until(long when)
{
   g_hold_until = when;
}

void start_scan(struct ANativeActivity *a)
{
   (void)a;
}

void stop_scan(struct ANativeActivity *a)
{
   (void)a;
}

/* ---- EVERY LINE THIS WORKFLOW LOGS, KEPT ----
 *
 * logcat is not private: `adb logcat` reads it from any machine the phone is
 * plugged into, and an ANR or tombstone bug report carries it off the device
 * whole. The commit below used to log the sensor's BLE address and the
 * four-digit J-PAKE code together on one line -- the address that finds the
 * sensor on the air, and the secret that authenticates this phone to it,
 * published side by side at every pairing. The code is printed on the
 * applicator, typed once, and cannot be rotated for the life of the wear.
 *
 * So the stub keeps the stream and the assertions are about what is NOT in
 * it. A test that only checked a line was emitted would prove nothing. */
static char g_log[1 << 16];
static size_t g_loglen;

static void log_reset(void)
{
   g_loglen = 0;
   g_log[0] = 0;
}

static int log_has(const char *needle)
{
   return strstr(g_log, needle) != 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   va_list ap;
   va_start(ap, fmt);
   size_t room = sizeof g_log - g_loglen;
   if (room > 1) {
      int n = vsnprintf(g_log + g_loglen, room, fmt, ap);
      if (n > 0 && (size_t)n < room) {
         g_loglen += (size_t)n;
         if (g_loglen + 1 < sizeof g_log) {
            g_log[g_loglen++] = '\n';
            g_log[g_loglen]   = 0;
         }
      }
   }
   va_end(ap);
   return 0;
}

/* ---- the fake JNI ------------------------------------------------------- */

/* A jstring here IS a C string: GetStringUTFChars hands the same pointer
 * back. That is enough for this callback, and it means the test can drive the
 * exact entry point Android drives, rather than a private function reached by
 * including the .c file. */
static const char *fake_get_chars(JNIEnv *e, jstring s, jboolean *copy)
{
   (void)e;
   if (copy)
      *copy = 0;
   return (const char *)s;
}

static void fake_release_chars(JNIEnv *e, jstring s, const char *c)
{
   (void)e;
   (void)s;
   (void)c;
}

static jboolean fake_exception_check(JNIEnv *e)
{
   (void)e;
   return 0;
}

static void fake_exception_clear(JNIEnv *e)
{
   (void)e;
}

/* A jstring and a jclass, from a C string. One cast, in one place, with the
 * reason written down: nothing in this test dereferences either -- the fake
 * GetStringUTFChars hands the same bytes back, and the class is only ever
 * compared against NULL. */
static jstring fake_str(const char *s)
{
   union {
      const char *s;
      jstring j;
   } u = {.s = s};

   return u.j;
}

static jclass fake_class(void)
{
   static char name[] = "Ble";

   union {
      char *s;
      jclass j;
   } u = {.s = name};

   return u.j;
}

/* The advert callback, as handed over by pairing_register. */
typedef void (*advert_fn)(JNIEnv *, jclass, jstring, jstring, jint);
static advert_fn g_advert;

static jint fake_register(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n)
{
   (void)e;
   (void)c;
   if (n != 1 || !m || strcmp(m[0].name, "onAdvert") != 0)
      return -1;
   g_advert = (advert_fn)m[0].fnPtr;
   return 0;
}

static const struct JNINativeInterface_ g_jni = {
    .GetStringUTFChars     = fake_get_chars,
    .ReleaseStringUTFChars = fake_release_chars,
    .ExceptionCheck        = fake_exception_check,
    .ExceptionClear        = fake_exception_clear,
    .RegisterNatives       = fake_register,
};
static const struct JNINativeInterface_ *g_jni_p = &g_jni;

/* One advertisement heard, delivered the way the Bluetooth stack delivers it.
 */
static void advert(const char *name, const char *mac, int rssi)
{
   JNIEnv *env = (JNIEnv *)&g_jni_p;
   g_advert(env, fake_class(), fake_str(name), fake_str(mac), (jint)rssi);
}

/* ---- helpers ------------------------------------------------------------ */

static void fresh_registry(void)
{
   sensors_paths(test_dir());
   unlink(sensors_path());
   unlink(slots_path());
   sensors_load();
   for (int i = 0; i < MAX_SLOTS; i++)
      g_slot_link[i] = i < LINK_MAX ? i : -1;
   memset(g_sess, 0, sizeof g_sess);
}

static void clear_candidates(void)
{
   pairing_forget_candidates();
   pair_cancel();
   pairing_arm(0);
}

static int cand_index_of(const char *mac)
{
   struct pair_cand c[16];
   int n = pairing_candidates(c, 16);
   for (int i = 0; i < n; i++)
      if (strcmp(c[i].mac, mac) == 0)
         return i;
   return -1;
}

static unsigned cand_count_of(const char *mac)
{
   struct pair_cand c[16];
   int n = pairing_candidates(c, 16);
   for (int i = 0; i < n; i++)
      if (strcmp(c[i].mac, mac) == 0)
         return c[i].count;
   return 0;
}

/* ---- the concurrent case ------------------------------------------------ */

/* ATOMIC, not volatile: `volatile` is not a synchronisation primitive in C,
 * and a stop flag shared between threads through one is a data race -- so a
 * test built on it is undefined behaviour asserting things about defined
 * behaviour. */
static atomic_int g_race_stop;
/* Adverts actually delivered, so the reader below can show it was reading
 * WHILE the radio was writing rather than after it stopped. */
static atomic_long g_adverts_sent;

static void *advert_thread(void *p)
{
   (void)p;
   const char *macs[4] = {"AA:00:00:00:00:01", "AA:00:00:00:00:02",
                          "AA:00:00:00:00:03", "AA:00:00:00:00:04"};
   int i               = 0;
   while (!atomic_load_explicit(&g_race_stop, memory_order_relaxed)) {
      advert("DX01ABCD", macs[i % 4], -50 - (i % 8));
      atomic_fetch_add_explicit(&g_adverts_sent, 1, memory_order_relaxed);
      i++;
   }
   return 0;
}

int main(void)
{
   JNIEnv *env = (JNIEnv *)&g_jni_p;
   ck(pairing_register(env, fake_class()) == 1,
      "the advert callback registers on the Ble class");
   ck(g_advert != 0, "...and the radio has something to call");
   ck(pairing_register(0, fake_class()) == 0, "a null env registers "
                                              "nothing");
   fresh_registry();

   printf("== which advertisements become candidates ==\n");
   {
      clear_candidates();
      advert("DX01ABCD", "AA:BB:CC:DD:EE:01", -55);
      ck(pairing_candidate_count() == 1, "a Stelo advert is a candidate");
      advert("DXCM1234", "AA:BB:CC:DD:EE:02", -60);
      ck(pairing_candidate_count() == 2, "so is a G7");
      /* A stranger's phone, watch or headset. Not a family filter for safety
       * -- that comes from the code and the registry -- but the list must
       * still be the list of things it makes sense to pair. */
      advert("SomeSpeaker", "AA:BB:CC:DD:EE:03", -40);
      ck(pairing_candidate_count() == 2, "an unrelated device is not offered "
                                         "as a sensor");
      /* ...and a meter is not offered while the user is adding a SENSOR. */
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:04", -45);
      ck(pairing_candidate_count() == 2, "a meter is not a candidate in ADD "
                                         "SENSOR");

      /* EVERY advert is counted, including the ones not listed: that counter
       * is how the screen says whether the radio is delivering anything at
       * all, which is the difference between "no sensor in range" and "the
       * scan died". */
      unsigned seen = pairing_adverts_seen();
      advert("SomeSpeaker", "AA:BB:CC:DD:EE:03", -40);
      ck(pairing_adverts_seen() == seen + 1, "an ignored advert still counts "
                                             "as radio traffic");
   }

   printf("== the same sensor heard again is the same candidate ==\n");
   {
      clear_candidates();
      advert("DX01ABCD", "AA:BB:CC:DD:EE:01", -70);
      advert("DX01ABCD", "AA:BB:CC:DD:EE:01", -55);
      advert("DX01ABCD", "AA:BB:CC:DD:EE:01", -50);
      ck(pairing_candidate_count() == 1, "three adverts from one sensor are "
                                         "ONE candidate");
      /* From ZERO, not from whatever the previous scan left in the row: the
       * list reset only drops the count of rows, so a device heard again
       * resumed its old total and the pipe-health figure described every
       * scan session at once. */
      ck(cand_count_of("AA:BB:CC:DD:EE:01") == 3, "...with THIS scan's "
                                                  "adverts counted, from "
                                                  "zero");
      struct pair_cand c[4];
      int n = pairing_candidates(c, 4);
      /* THE LATEST signal, not the first: the list is ordered and chosen by
       * proximity, so a stale -70 dBm from when the sensor was across the
       * room decides the wrong way. */
      ck(n == 1 && c[0].rssi == -50, "...and the LATEST signal, not the first "
                                     "one heard");
      ck(n == 1 && c[0].seen_t == g_mono, "...stamped with when it was last "
                                          "heard");
   }

   printf("== a candidate list has a ceiling ==\n");
   {
      clear_candidates();
      char mac[20];
      for (int i = 0; i < 40; i++) {
         snprintf(mac, sizeof mac, "AA:BB:CC:00:%02d:%02d", i / 100, i % 100);
         advert("DX01ABCD", mac, -60 - i);
      }
      int n = pairing_candidate_count();
      ck(n > 0 && n <= 12, "forty sensors in range do not overrun a list of "
                           "twelve");
      /* And a caller with a smaller buffer than the list gets exactly what it
       * asked for -- the renderer's array is the one that would be overrun. */
      struct pair_cand c[3];
      ck(pairing_candidates(c, 3) == 3, "a copy respects the caller's "
                                        "capacity");
      ck(pairing_candidates(c, 0) == 0, "...and a zero capacity copies "
                                        "nothing");
      ck(pairing_candidates(0, 3) == 0, "...and no buffer at all is not a "
                                        "crash");
   }

   printf("== only sensors on the air RIGHT NOW are chosen between ==\n");
   {
      /* The list is never pruned, so a sensor that left the room an hour ago
       * still sits in it with its old RSSI -- and an armed pairing evaluates
       * the choice on every tick, possibly long after the list was built.
       * Comparing a stale strong signal against a fresh weak one picks wrong
       * exactly when it matters: the sensor being applied is the one in the
       * room. */
      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:10", -40); /* strong, and leaving */
      tick(61);
      advert("DX01BBBB", "AA:BB:CC:DD:EE:11", -75); /* weak, but here */
      ck(pairing_candidate_count() == 2, "both are still IN the list");
      ck(fresh_candidates() == 1, "...but only one is on the air");
      int idx = select_candidate();
      ck(idx == cand_index_of("AA:BB:CC:DD:EE:11"),
         "the sensor in the room wins over a stronger one that has gone");

      /* Nothing fresh at all: no choice, and above all not a guess. */
      tick(61);
      ck(fresh_candidates() == 0, "an hour later nothing is on the air");
      ck(select_candidate() == -1, "...and no candidate is chosen");
   }

   printf("== a wall-clock correction changes NOTHING here ==\n");
   {
      /* EVERY INTERVAL IN THIS FILE IS MONOTONIC, and this is what says so.
       * A phone's wall clock moves: NTP corrects it, a timezone database
       * update shifts it, the user sets it by hand. Each of these used to
       * move a decision:
       *   - the candidate freshness window, so a jump FORWARD retired every
       *     candidate at once and an armed pairing then waited for an advert
       *     it had already had, while a jump BACKWARD kept a sensor that had
       *     left the room "on the air";
       *   - the meter's once-a-minute advert throttle, so a jump forward woke
       *     the meter on every advert of one wake window;
       *   - the per-link reconnect throttle, the same way.
       */
      fresh_registry();
      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:A0", -55);
      ck(fresh_candidates() == 1, "a candidate is on the air");

      g_real += 86400; /* the clock jumps a day FORWARD; no time has passed */
      ck(fresh_candidates() == 1, "...and a day of wall clock does not retire "
                                  "it");
      ck(select_candidate() == 0, "...so the pairing still has its candidate");

      g_real -= 3L * 86400; /* ...and three days BACKWARD */
      ck(fresh_candidates() == 1, "...nor does the clock going backwards");

      /* REAL time passing still retires it: the rule is "measure intervals on
       * the monotonic clock", not "ignore time". */
      tick(61);
      ck(fresh_candidates() == 0, "...but a minute of real time does");
      ck(select_candidate() == -1, "...and there is nothing to pair with");
   }

   printf("== a meter's throttle is an interval too ==\n");
   {
      fresh_registry();
      clear_candidates();
      g_meter_last_seen = 0;
      g_meter_last_mono = 0;
      int id   = sensor_mint(SENSOR_ONETOUCH, "AA:BB:CC:DD:EE:A1", "", "", "",
                             g_real - 3600);
      int slot = sensor_claim_slot(id, SENSOR_ONETOUCH, "AA:BB:CC:DD:EE:A1");
      ck(id > 0 && slot >= 0, "a meter is registered");
      int syncs = g_meter_syncs;
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:A1", -60);
      ck(g_meter_syncs == syncs + 1, "its first advert syncs");
      syncs = g_meter_syncs;
      g_real += 86400; /* the wall clock jumps a day forward */
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:A1", -60);
      ck(g_meter_syncs == syncs, "a clock jump does not re-open the throttle "
                                 "mid-wake");
      tick(61); /* a minute really passes */
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:A1", -60);
      ck(g_meter_syncs == syncs + 1, "...and a minute of real time does");
   }

   printf("== an ambiguous choice is the user's ==\n");
   {
      /* Two sensors close in signal is the case where an automatic pick costs
       * a bond on the wrong device -- so it is refused and the list is shown
       * instead. The rule itself lives in scanlogic.c; this is that it is
       * actually consulted. */
      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:20", -55);
      advert("DX01BBBB", "AA:BB:CC:DD:EE:21", -58);
      ck(select_candidate() == -1, "two sensors of similar strength are not "
                                   "chosen between");

      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:20", -35);
      advert("DX01BBBB", "AA:BB:CC:DD:EE:21", -80);
      ck(select_candidate() == cand_index_of("AA:BB:CC:DD:EE:20"),
         "one clearly nearer is chosen");

      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:20", -55);
      ck(select_candidate() == 0, "a lone candidate is chosen unopposed");
   }

   printf("== a pick PROPOSES, it does not pair ==\n");
   {
      /* The list reorders by live signal under the finger, so pairing on the
       * raw tap let one mis-press register the wrong device and drop a bond.
       * A pick copies the identity; only the confirmation screen commits. */
      clear_candidates();
      advert("DX01AAAA", "AA:BB:CC:DD:EE:30", -55);
      int pairs = g_pairs;
      ck(pairing_pick(0) == 1, "a row that exists can be picked");
      ck(strcmp(pairing_pend_mac(), "AA:BB:CC:DD:EE:30") == 0,
         "...and its address is what is proposed");
      ck(strcmp(pairing_pend_name(), "DX01AAAA") == 0, "...with its name, for "
                                                       "the confirmation to "
                                                       "show");
      ck(g_pairs == pairs, "...and NOTHING was paired by the pick itself");
      ck(pairing_pick(7) == 0, "a row that has gone cannot be picked");
      ck(pairing_pick(-1) == 0, "...nor can a negative one");
   }

   printf("== a sensor already bonded is not offered again ==\n");
   {
      /* With just that one in range -- the common case when the code is
       * entered before the replacement is applied -- it would be chosen
       * unopposed, and the commit would run a J-PAKE re-pair against a sensor
       * that is already bonded and streaming, burning the link it was using.
       */
      fresh_registry();
      clear_candidates();
      int id = sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:40", "", "", "",
                           g_real - 3600);
      ck(id > 0, "a sensor is registered");
      int slot = sensor_claim_slot(id, SENSOR_STELO, "AA:BB:CC:DD:EE:40");
      ck(slot >= 0, "...and takes a slot");
      g_slot_link[slot] = 0;
      snprintf(g_sess[0].mac, sizeof g_sess[0].mac, "AA:BB:CC:DD:EE:40");
      g_sess[0].bonded = 1;

      advert("DX01AAAA", "AA:BB:CC:DD:EE:40", -50);
      ck(pairing_candidate_count() == 0, "a BONDED sensor is not a pairing "
                                         "candidate");

      /* ...but one registered and NOT yet bonded must still be offerable, or
       * a single failed pairing (wrong code, out of range) leaves it
       * permanently missing from ADD SENSOR with nothing saying why. */
      g_sess[0].bonded = 0;
      g_sess[0].mac[0] = 0;
      advert("DX01AAAA", "AA:BB:CC:DD:EE:40", -50);
      ck(pairing_candidate_count() == 1,
         "a registered-but-never-bonded sensor can be tried again");
   }

   printf("== the code, then the sensor ==\n");
   {
      /* Exactly one sensor on the air plus a four-digit code is as
       * unambiguous as it gets, so it pairs at once: a confirmation list of
       * one is ceremony, and the J-PAKE code itself rejects a wrong device. */
      fresh_registry();
      clear_candidates();
      g_add_type = SENSOR_STELO;
      advert("DX01AAAA", "AA:BB:CC:DD:EE:50", -55);
      kp_set("1234");
      int pairs = g_pairs;
      int r     = kp_commit_pair();
      ck(r == COMMIT_DONE, "a four-digit code commits");
      ck(g_pairs == pairs + 1, "...and the lone candidate is paired");
      ck(strcmp(g_paired_mac, "AA:BB:CC:DD:EE:50") == 0, "...at ITS address");
      ck(strcmp(g_paired_code, "1234") == 0, "...with the code that was "
                                             "typed");
      ck(g_selected_id > 0 && g_nav_to == SCR_SENSOR,
         "...and the flow ends on the new device's own screen -- selected by "
         "ID, so a mint on another thread cannot redirect it");
      ck(!pairing_smart(), "pairing mode is over, so every other sensor "
                           "reconnects freely again");

      /* A code of the wrong length is not this field's business at all. */
      kp_set("123"); /* three digits are not a pairing code */
      ck(kp_commit_pair() == COMMIT_PASS, "three digits are not a pairing "
                                          "code");
   }

   printf("== the pairing code never reaches the log ==\n");
   {
      /* THE ONE LINE THIS PINS used to read
       *
       *   "pair new sensor %s with code %s on link %d"
       *
       * with the address and the code spelled out in full, on the SUCCESS
       * path -- so every pairing this app ever performed wrote the wearer's
       * sensor address and its shared secret into a log any connected machine
       * can read.
       *
       * The address is deliberately unlike anything else that can appear
       * (colons, uppercase hex) and the code is deliberately not "1234",
       * which is a substring of ordinary counters and ids. */
      fresh_registry();
      clear_candidates();
      g_add_type = SENSOR_STELO;
      advert("DX01AAAA", "AA:BB:CC:DD:EE:51", -55);
      kp_set("9973");
      log_reset();
      int pairs = g_pairs;
      ck(kp_commit_pair() == COMMIT_DONE, "the pairing commits");

      /* IT REALLY HAPPENED, AND IT REALLY LOGGED. Without these two the
       * "does not contain" assertions below would pass on an empty buffer,
       * which is the shape of an assertion that cannot fail. */
      ck(g_pairs == pairs + 1, "...and reached the radio");
      ck(strcmp(g_paired_code, "9973") == 0, "...with the code that was "
                                             "typed, so the code IS in play");
      ck(log_has("pair new sensor"), "...and the commit was logged");

      ck(!log_has("9973"), "the pairing CODE is not in the log");
      ck(!log_has("AA:BB:CC:DD:EE:51"), "the sensor's ADDRESS is not in the "
                                        "log");
   }

   printf("== a code with nothing on the air ARMS the pairing ==\n");
   {
      /* The old flow parked the user in the device list until the sensor
       * deigned to advertise -- with the smart scan suppressing every OTHER
       * sensor's reconnect the whole time, so waiting for a new sensor cost
       * the readings of the ones already worn. */
      fresh_registry();
      clear_candidates();
      g_add_type = SENSOR_STELO;
      kp_set("4321");
      int pairs  = g_pairs;
      int closed = g_keypad_closed;
      ck(kp_commit_pair() == COMMIT_DONE, "the code is accepted");
      ck(g_pairs == pairs, "...and nothing is paired yet");
      ck(pairing_pending() == SENSOR_STELO, "...the pairing is ARMED for the "
                                            "type asked for");
      ck(!pairing_smart(), "...the suppressing scan is off");
      ck(g_keypad_closed > closed, "...and the user is freed, not parked");

      /* The tick commits it the moment an unambiguous candidate appears. */
      g_screen = SCR_MAIN;
      pairing_tick();
      ck(g_pairs == pairs, "the tick with nothing on the air does nothing");
      advert("DX01AAAA", "AA:BB:CC:DD:EE:60", -55);
      pairing_tick();
      ck(g_pairs == pairs + 1, "...and commits as soon as one advertises");
      ck(strcmp(g_paired_mac, "AA:BB:CC:DD:EE:60") == 0, "...that one");
      ck(pairing_pending() == 0, "...and the armed intent is spent");
   }

   printf("== an armed pairing does not yank the user out of a screen ==\n");
   {
      fresh_registry();
      clear_candidates();
      pairing_arm(SENSOR_STELO);
      advert("DX01AAAA", "AA:BB:CC:DD:EE:70", -55);
      int pairs = g_pairs;

      static const enum ui_screen midflow[] = {SCR_KEYPAD, SCR_DEVLIST,
                                               SCR_PAIRCONF};

      for (int i = 0; i < 3; i++) {
         g_screen = midflow[i];
         pairing_tick();
      }
      ck(g_pairs == pairs, "a commit waits while the user is mid-flow on the "
                           "keypad, the device list or a confirmation");
      g_screen = SCR_MAIN;
      pairing_tick();
      ck(g_pairs == pairs + 1, "...and happens once they have left");

      /* An armed METER is not committed by this path: the meter flow syncs on
       * its own advert and has no J-PAKE code to commit. */
      clear_candidates();
      pairing_arm(SENSOR_ONETOUCH);
      g_add_type = SENSOR_ONETOUCH;
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:71", -55);
      pairs = g_pairs;
      pairing_tick();
      ck(g_pairs == pairs, "an armed meter is not paired as a sensor");
      g_add_type = SENSOR_STELO;
   }

   printf("== a commit that cannot happen still ENDS pairing mode ==\n");
   {
      /* Pairing mode suppresses every OTHER sensor's advert-driven reconnect.
       * It used to be cleared only on the two success paths, so any refusal
       * left it latched at 1 for ever: one failed attempt stopped every
       * already-paired CGM from reconnecting again, with nothing on screen to
       * say so -- the advert counter keeps climbing while the reading quietly
       * stops ageing forward. */
      fresh_registry();
      clear_candidates();
      g_slot_link[0] = -1; /* every link in use */
      pair_scan_start();
      ck(pairing_smart(), "pairing mode is on");
      pairing_arm(SENSOR_STELO);
      int pairs = g_pairs;
      commit_pair("AA:BB:CC:DD:EE:99");
      ck(g_pairs == pairs, "with no free link, nothing is paired");
      ck(!pairing_smart(), "...and pairing mode is NOT left latched, which "
                           "would stop every paired sensor reconnecting");
      ck(!pairing_pending(), "...and no armed intent survives the commit");
      ck(strcmp(g_status, "NO FREE SENSOR LINK") == 0, "...and the screen "
                                                       "says why");
   }

   printf("== cancelling ==\n");
   {
      clear_candidates();
      pair_scan_start();
      ck(pairing_smart(), "the smart scan is running");
      advert("DX01AAAA", "AA:BB:CC:DD:EE:80", -55);
      ck(pairing_candidate_count() == 1, "...and collecting");
      pair_scan_start();
      ck(pairing_candidate_count() == 0, "starting again begins from an empty "
                                         "list, not from stale adverts");
      pair_cancel();
      ck(!pairing_smart(), "cancelling stops it");
      /* The scan SUPPRESSES every other sensor's reconnect, so it must not
       * outlive the screen that started it -- separately from cancelling the
       * pairing itself. */
      pair_scan_start();
      pairing_stop_smart();
      ck(!pairing_smart(), "and the scan can be stopped without cancelling "
                           "anything else");
   }

   printf("== a registered meter's advert IS its sync trigger ==\n");
   {
      /* A OneTouch advertises only while the user has it switched on, so
       * hearing it is the whole trigger. A stranger's meter has no slot and
       * is ignored. */
      fresh_registry();
      clear_candidates();
      g_meter_last_seen = 0;
      g_meter_last_mono = 0;
      int id   = sensor_mint(SENSOR_ONETOUCH, "AA:BB:CC:DD:EE:90", "", "", "",
                             g_real - 3600);
      int slot = sensor_claim_slot(id, SENSOR_ONETOUCH, "AA:BB:CC:DD:EE:90");
      ck(id > 0 && slot >= 0, "a meter is registered");
      int syncs = g_meter_syncs;
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:90", -60);
      ck(g_meter_syncs == syncs + 1, "its advert starts a sync");
      ck(strcmp(g_meter_sync_mac, "AA:BB:CC:DD:EE:90") == 0, "...on its own "
                                                             "address");
      ck(g_meter_notes > 0, "...and the signal is recorded from the advert, "
                            "not left blank until a sync completes");

      /* Throttled: a meter awake for a minute advertises many times, and one
       * sync per wake is the point. */
      syncs = g_meter_syncs;
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:90", -60);
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:90", -60);
      ck(g_meter_syncs == syncs, "a burst of adverts is ONE sync");
      tick(61);
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:90", -60);
      ck(g_meter_syncs == syncs + 1, "...and a later wake is another");

      /* A stranger's meter: no slot, no sync. */
      syncs = g_meter_syncs;
      advert("OneTouch Ultra", "FF:FF:FF:FF:FF:FF", -50);
      ck(g_meter_syncs == syncs, "a meter that is not ours is ignored");

      /* And nothing at all while a protocol exchange is running. */
      g_meter_busy_flag = 1;
      tick(61);
      advert("OneTouch Ultra", "AA:BB:CC:DD:EE:90", -60);
      ck(g_meter_syncs == syncs, "no second sync while one is in flight");
      g_meter_busy_flag = 0;
   }

   printf("== the list is written by the radio and read by the screen ==\n");
   {
      /* jni_on_advert runs on a Bluetooth binder thread while the main looper
       * reads the list and resets it. A half-written entry is a device shown
       * with another device's address -- and the pick that follows pairs
       * against it. */
      fresh_registry();
      clear_candidates();
      /* <pthread.h> IS included above. glibc defines pthread_t in a private
       * header with no pragma pointing back at the public one, so
       * include-cleaner asks for that private header by name -- including
       * which would be the actual defect. */
      /* NOLINTNEXTLINE(misc-include-cleaner) */
      pthread_t th[3];
      atomic_store(&g_race_stop, 0);
      atomic_store(&g_adverts_sent, 0);
      for (int i = 0; i < 3; i++)
         if (pthread_create(&th[i], 0, advert_thread, 0) != 0)
            ck(0, "an advert thread started");
      /* WAIT FOR THE RADIO TO BE DELIVERING. pthread_create returns before
       * the thread runs; a reader that finishes first has read a list nobody
       * was writing, which is the one arrangement this case must not be. */
      int waited = 0;
      while (atomic_load(&g_adverts_sent) == 0 && waited < 500000) {
         sched_yield();
         waited++;
      }
      ck(atomic_load(&g_adverts_sent) > 0, "the binder threads are "
                                           "advertising");
      long before  = atomic_load(&g_adverts_sent);
      int bad      = 0;
      int saw_rows = 0;
      /* READ UNTIL THE OVERLAP HAS ACTUALLY HAPPENED, rather than a fixed
       * number of iterations and a hope. The property is "the writers were
       * writing throughout the reads"; expressed as a race between a
       * fixed-size read loop and whatever the radio managed in that time, it
       * passes or fails according to how loaded the machine is -- this case
       * went from green to red and back on three runs in a row while another
       * build was in flight, which makes it useless as a gate either way. So
       * the loop RUNS UNTIL the writers have delivered enough to prove the
       * overlap. The iteration cap is a HANG GUARD, not a timing assertion:
       * a writer that died must not spin this for ever, and if the cap is
       * what ends the loop the assertion below says so. (The fake clock in
       * this file does not advance on its own, so it cannot be a deadline.)
       */
      long need = 2000; /* adverts that must land DURING the reads */
      long want = 1000; /* reads that must see a populated list */
      for (long i = 0;
           atomic_load(&g_adverts_sent) - before < need || saw_rows < want;
           i++) {
         if (i > 20000000)
            break;
         struct pair_cand c[16];
         int n = pairing_candidates(c, 16);
         if (n > 0)
            saw_rows++;
         for (int k = 0; k < n; k++) {
            /* Every published row is WHOLE: a name, an address of the right
             * shape, and a count that was set before the row was published. */
            if (strncmp(c[k].name, "DX01", 4) != 0 || strlen(c[k].mac) != 17 ||
                c[k].count == 0)
               bad = 1;
         }
         if (i % 1000 == 0)
            pairing_forget_candidates(); /* the main thread's reset */
      }
      long during = atomic_load(&g_adverts_sent) - before;
      atomic_store(&g_race_stop, 1);
      for (int i = 0; i < 3; i++)
         pthread_join(th[i], 0);
      ck(!bad, "no reader ever sees a counted-but-unwritten candidate");
      /* THE TWO SIDES REALLY OVERLAPPED, and the reader really had something
       * to look at. Either alone passes trivially: a reader that saw an empty
       * list every time inspects nothing, and adverts that all arrived before
       * the reads are not a race. */
      ck(during >= need, "the radio kept advertising THROUGHOUT the reads");
      ck(saw_rows >= want, "...and the reader saw populated lists, "
                           "repeatedly, between the resets");
   }

   printf("\n%s\n", all ? "ALL PAIRING TESTS PASSED" : "PAIRING TESTS FAILED");
   return all ? 0 : 1;
}
