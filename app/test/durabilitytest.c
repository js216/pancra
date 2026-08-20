// SPDX-License-Identifier: GPL-3.0
// durabilitytest.c --- "saved" has to mean saved, on every log the app keeps
// Copyright 2026 Jakob Kastelic
//
/* FOUR APPEND-ONLY FILES hold everything this app is for: the readings, the
 * doses, the weights and the provenance that says which sensor a reading came
 * from. Each is written the same way -- create with a header, append one row,
 * report success -- and each has the same three ways to lie about it:
 *
 *   1. THE HEADER. Written best-effort, a short write leaves half a line with
 *      no newline, and the row appended straight after lands on that same
 *      line: the '#' comments out the first real record, permanently, in a
 *      file that is never rewritten. The append reported success.
 *   2. THE ROLLBACK. A row that stopped halfway is truncated away so the next
 *      append cannot splice onto it -- and if the truncate itself fails, the
 *      file is DAMAGED, which is not the same as "the append failed".
 *   3. THE FLUSH. A write that returns is in the page cache, not on the card.
 *      A phone that loses power (or is killed by Android, which is routine)
 *      comes back without the record, having told the user it was saved. The
 *      directory entry needs flushing too when the file was just created:
 *      otherwise the contents are safe under a name nothing points to.
 *
 * The first is arrangeable with RLIMIT_FSIZE. The others are not -- an fsync
 * that fails, a close reporting a deferred error, a rename that fails at the
 * last step -- so app/util.c carries deliberate failures behind -DAPP_FAULTS,
 * which only this test's build sets. Each case below arms one, runs a REAL
 * append against a REAL file, and asks what the caller was told.
 *
 * Built and run by `make durabilitytest`.
 */
#include "insulin.h"
#include "sensors.h"
#include "store.h"
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h" /* LOG_OK / LOG_FAIL / LOG_DAMAGED: what an append answers */
#include "weight.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio;
   (void)tag;
   (void)fmt;
   return 0;
}

static int fails;

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* A VARIABLE, not the literal "build/app/test/durable" it used to be: this
 * suite is in both the ASan and the TSan list as well as the plain one, and
 * three processes sharing one fixture directory is a test asserting on files
 * another process replaced. app/test/testdir.h says where the value comes
 * from. Filled on the first fresh(), which every case calls. */
static char DIR[128];

static void fresh(void)
{
   test_path(DIR, sizeof DIR, "durable");
   mkdir(test_dir(), 0755);
   mkdir(DIR, 0755);
   unsetenv("APP_FAIL_FSYNC");
   unsetenv("APP_FAIL_CLOSE");
   unsetenv("APP_FAIL_RENAME");
   unsetenv("APP_FAIL_TRUNCATE");
   unsetenv("APP_FAIL_DIRSYNC");
   store_paths(DIR);
   weight_paths(DIR);
   insulin_paths(DIR);
   sensors_paths(DIR);
   (void)remove(store_path());
   (void)remove(weight_path());
   (void)remove(insulin_path());
   (void)remove(sensors_path());
   (void)remove(slots_path());
   /* The in-memory tails are module state, not file state: reload them from
    * the (now absent) files so each case starts from nothing. */
   (void)weight_load();
   (void)insulin_load();
   (void)sensors_load();
}

/* Records in a log: every line that is not the '#' header. */
static int count_rows(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   int rows = 0;
   char line[512];
   while (fgets(line, sizeof line, f))
      if (line[0] && line[0] != '#' && line[0] != '\n')
         rows++;
   fclose(f);
   return rows;
}

static long file_size(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return -1;
   return (long)st.st_size;
}

/* ---- the concurrent-failure case (see the section in main) ---------------
 *
 * Two writers, one log, DIFFERENT ROW LENGTHS -- which is what makes a
 * crossed take-back visible: truncating one writer's length off the other
 * writer's row leaves a fragment rather than a tidy removal. */
#define SEED_ROW    "1700000000,70000,0\n" /* what append_weight writes */
#define GOOD_ROW    "1700009999,70000,0\n" /* deliberately not the seed */
#define BAD_ROW     "1700000001,70001,0,xxxxxxxxxxxxxxxx\n"
#define RACE_ROUNDS 400

struct racer {
   int ok;   /* appends this writer was told succeeded (must stay 0) */
   int runs; /* appends attempted */
};

/* Held open for the other writer to land in -- see app_fault_gap_here. */
static void widen_window(void)
{
   struct timespec ts = {0, 300000}; /* 0.3 ms */
   nanosleep(&ts, 0);
}

static void *faulty_writer(void *arg)
{
   struct racer *r = arg;
   /* THIS THREAD's fsync fails; the main thread's does not. */
   app_fault_fsync_here = 1;
   app_fault_gap_here   = widen_window;
   for (int i = 0; i < RACE_ROUNDS; i++) {
      r->runs++;
      if (log_append(weight_path(), 0, 0, BAD_ROW, (int)sizeof BAD_ROW - 1) ==
          LOG_OK)
         r->ok++;
   }
   return 0;
}

/* Count the file's lines by which writer they came from, and how many are
 * neither -- a fragment left by a take-back that cut the wrong row. */
static void count_kinds(const char *path, int *good, int *other, int *partial)
{
   *good = *other = *partial = 0;
   FILE *f                   = fopen(path, "r");
   if (!f)
      return;
   char line[512];
   while (fgets(line, sizeof line, f)) {
      if (line[0] == '#' || line[0] == '\n')
         continue;
      if (!strcmp(line, SEED_ROW))
         continue; /* the row the log was seeded with */
      if (!strcmp(line, GOOD_ROW))
         (*good)++;
      else if (!strcmp(line, BAD_ROW))
         (*other)++;
      else
         (*partial)++; /* neither, whole -- a take-back cut the wrong row */
   }
   fclose(f);
}

/* Does the file's first byte start a COMMENT that swallows a record? The
 * header is one line; if it is short (no newline) the row that follows is on
 * the same line and reads as part of the comment. */
static int first_record_commented(const char *path)
{
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return 0;
   char b[512];
   long n = read(fd, b, (long)sizeof b - 1);
   close(fd);
   if (n <= 0)
      return 0;
   b[n] = 0;
   if (b[0] != '#')
      return 0;
   char *nl = strchr(b, '\n');
   if (!nl)
      return b[1] != 0; /* a header with no newline and something after it */
   /* A newline exists: is there a record after it, or did the header run into
    * the row? */
   return 0;
}

/* ---- one append per log, so every case can be run against all four ------- */

static int append_reading(void)
{
   return store_append(1700000000, 120, 0, -70, 3, 7, 1700000000, 0, KIND_CGM,
                       1000);
}

static int append_weight(void)
{
   return weight_append(1700000000, 70000, 0);
}

static int append_dose(void)
{
   return insulin_append(1700000000, INS_FAST, 3, 0);
}

/* The mint's own answer travels: a positive id is success, and a negative one
 * is the append's code (LOG_FAIL or LOG_DAMAGED) rather than a generic -1. */
static int append_provenance(void)
{
   int id =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:01", "S", "M", "1.2", 100);
   return id > 0 ? 0 : id;
}

/* A SECOND, DISTINCT record per log, for the cases that need one append to
 * succeed and the next to fail partway. */
static int append_reading2(void)
{
   return store_append(1700000300, 121, 0, -70, 3, 7, 1700000300, 0, KIND_CGM,
                       1000);
}

static int append_weight2(void)
{
   return weight_append(1700000300, 70500, 0);
}

static int append_dose2(void)
{
   return insulin_append(1700000300, INS_SLOW, 5, 0);
}

static int append_provenance2(void)
{
   int id =
       sensor_mint(SENSOR_STELO, "AA:BB:CC:DD:EE:02", "S", "M", "1.2", 100);
   return id > 0 ? 0 : id;
}

struct logpath {
   const char *name;
   int (*append)(void);
   int (*append2)(void);
   const char *(*path)(void);
};

static const struct logpath LOGS[] = {
    {"readings",   append_reading,    append_reading2,    store_path  },
    {"weights",    append_weight,     append_weight2,     weight_path },
    {"doses",      append_dose,       append_dose2,       insulin_path},
    {"provenance", append_provenance, append_provenance2, sensors_path},
};
#define NLOGS ((int)(sizeof LOGS / sizeof LOGS[0]))

int main(void)
{
   char msg[160];

   printf("== a header that cannot be written whole takes NO record with it "
          "==\n");
   /* RLIMIT_FSIZE caps how much any write may put in a file: the header is
    * cut off partway, which is the state that comments out the first real
    * record. Every one of the four is checked, because the defect was found
    * in one and fixed in one. */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      struct rlimit old;
      getrlimit(RLIMIT_FSIZE, &old);
      struct rlimit small = {8, old.rlim_max}; /* shorter than any header */
      if (setrlimit(RLIMIT_FSIZE, &small) != 0) {
         ck(0, "could not cap the file size");
         continue;
      }
      /* SIGXFSZ arrives when the cap is hit; the app must see EFBIG from
       * write(2) instead of dying. */
      signal(SIGXFSZ, SIG_IGN);
      int rc = LOGS[i].append();
      setrlimit(RLIMIT_FSIZE, &old);
      snprintf(msg, sizeof msg, "%s: a truncated header fails the append",
               LOGS[i].name);
      ck(rc != 0, msg);
      snprintf(msg, sizeof msg,
               "%s: ...and leaves nothing that could comment a record out",
               LOGS[i].name);
      ck(!first_record_commented(LOGS[i].path()), msg);
      /* NO FILE AT ALL, not an empty one. The first record is written to a
       * temporary and renamed into place, so a failure leaves nothing behind
       * -- which is what makes the retry below safe. */
      snprintf(msg, sizeof msg, "%s: ...and leaves NO file behind",
               LOGS[i].name);
      ck(file_size(LOGS[i].path()) < 0, msg);
      /* THE RETRY, which is what a caller does with a reported failure. With
       * the row written straight into a new live file, the failure left it
       * there and this second attempt produced a DUPLICATE -- a dose or a
       * reading recorded twice, in a log that is never rewritten. */
      snprintf(msg, sizeof msg, "%s: ...and a retry then writes it ONCE",
               LOGS[i].name);
      ck(LOGS[i].append() == 0, msg);
      snprintf(msg, sizeof msg, "%s: ...with exactly one record in the file",
               LOGS[i].name);
      ck(count_rows(LOGS[i].path()) == 1, msg);
   }

   printf("== a flush that fails is not a saved record ==\n");
   /* The write returned, the close returned, and the data is in the page
    * cache. If the flush fails the record may not survive the next power cut,
    * and the caller must not be told it was saved -- the in-memory tail is
    * updated from that answer, so a lie here makes the app show a dose the
    * file does not contain. */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      setenv("APP_FAIL_FSYNC", "1", 1);
      int rc = LOGS[i].append();
      unsetenv("APP_FAIL_FSYNC");
      snprintf(msg, sizeof msg, "%s: an fsync that fails fails the append",
               LOGS[i].name);
      ck(rc != 0, msg);
      /* AND LEAVES NOTHING. This is the case the item is about: with the
       * first record written straight into a new live file, the flush failed
       * AFTER the row was in it -- so the caller was told the record was not
       * saved while the file held it, and the retry below made a second
       * copy. */
      snprintf(msg, sizeof msg,
               "%s: ...leaving no file for a retry to "
               "duplicate into",
               LOGS[i].name);
      ck(file_size(LOGS[i].path()) < 0, msg);
      snprintf(msg, sizeof msg, "%s: ...so the retry writes exactly one",
               LOGS[i].name);
      ck(LOGS[i].append() == 0 && count_rows(LOGS[i].path()) == 1, msg);
   }

   printf("== a close that reports a deferred error is not a saved record "
          "==\n");
   /* A full disk or a pulled card surfaces at close(2), not at write(2). */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      setenv("APP_FAIL_CLOSE", "1", 1);
      int rc = LOGS[i].append();
      unsetenv("APP_FAIL_CLOSE");
      snprintf(msg, sizeof msg, "%s: a close that fails fails the append",
               LOGS[i].name);
      ck(rc != 0, msg);
      snprintf(msg, sizeof msg, "%s: ...with no half-created file left behind",
               LOGS[i].name);
      ck(file_size(LOGS[i].path()) < 0, msg);
   }

   printf("== a rewrite past the RENAME is changed, not failed ==\n");
   {
      /* THE POINT OF NO RETURN IS THE RENAME.
       *
       * Everything before it can be undone -- the temporary is removed and
       * the original file is untouched -- so a caller is right to put its
       * memory back. The DIRECTORY FSYNC comes after, and if it fails the new
       * pathname is already visible: the file HAS been replaced, and only its
       * survival of a power cut is unknown.
       *
       * replace_finish used to return fsync_dir_of's result directly, so both
       * looked identical to every caller. Told "failed", they rolled memory
       * back to a value the disk no longer held -- and the contradiction was
       * invisible until the next launch read the file and the reverted value
       * reappeared. */
      char pbuf[160];
      const char *pth = test_path(pbuf, sizeof pbuf, "dur-replace.txt");
      (void)remove(pth);

      /* A file to replace. */
      ck(atomic_replace(pth, "first\n", 6) == REPLACE_OK,
         "an ordinary rewrite reports OK");
      ck(file_size(pth) == 6, "...and the file holds it");

      /* Now fail only the directory sync, which happens AFTER the rename. */
      setenv("APP_FAIL_DIRSYNC", "1", 1);
      enum replace_result rr = atomic_replace(pth, "second-longer\n", 14);
      unsetenv("APP_FAIL_DIRSYNC");
      ck(rr == REPLACE_UNSYNCED,
         "a directory sync that fails AFTER the rename says CHANGED, not "
         "failed");
      ck(rr != REPLACE_FAILED,
         "...and is distinguishable from a rewrite that never landed");
      ck(file_size(pth) == 14,
         "...because the file really does hold the new contents");

      /* A failure BEFORE the rename is still a plain failure, and the file
       * is left exactly as it was. */
      setenv("APP_FAIL_RENAME", "1", 1);
      rr = atomic_replace(pth, "third\n", 6);
      unsetenv("APP_FAIL_RENAME");
      ck(rr == REPLACE_FAILED, "a rename that fails reports FAILED");
      ck(file_size(pth) == 14, "...and leaves the previous contents in place");
      (void)remove(pth);
   }

   printf("== a new file's DIRECTORY ENTRY is part of it ==\n");
   /* Without the directory flush the contents are on the card under a name
    * nothing points to, which after a power cut is the same as never having
    * been written. Only a CREATE needs it, which is exactly the first record
    * of every log. */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      setenv("APP_FAIL_DIRSYNC", "1", 1);
      int rc = LOGS[i].append();
      unsetenv("APP_FAIL_DIRSYNC");
      snprintf(msg, sizeof msg,
               "%s: a directory entry that is not flushed fails the append",
               LOGS[i].name);
      ck(rc != 0, msg);
      snprintf(msg, sizeof msg,
               "%s: ...and no file is published under a name "
               "the directory does not have",
               LOGS[i].name);
      ck(file_size(LOGS[i].path()) < 0, msg);
   }

   printf("== ...and with nothing armed, every one of them succeeds ==\n");
   /* The other half of every case above: a rule that always answers "failed"
    * would satisfy them all and make the app unusable. */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      int rc = LOGS[i].append();
      snprintf(msg, sizeof msg, "%s: an ordinary append succeeds",
               LOGS[i].name);
      ck(rc == 0, msg);
      snprintf(msg, sizeof msg,
               "%s: ...and the file has the header and the "
               "row in it",
               LOGS[i].name);
      ck(file_size(LOGS[i].path()) > 0, msg);
      snprintf(msg, sizeof msg, "%s: ...with the record not commented out",
               LOGS[i].name);
      ck(!first_record_commented(LOGS[i].path()), msg);
   }

   printf("== a rewrite that cannot be renamed leaves the ORIGINAL ==\n");
   /* The weight log is the one that is rewritten in place (an edit or a
    * delete). Every step of replace_finish is checked; the last one is the
    * rename, and a failure there must leave the file that was already there
    * rather than a temporary or nothing. */
   {
      fresh();
      ck(weight_append(1700000000, 70000, 0) == 0, "a weight is logged");
      ck(weight_append(1700000600, 70500, 0) == 0, "...and another");
      long before = file_size(weight_path());
      struct wt_rec first;
      int got = 0;
      for (int i = 0; i < wt_count(); i++)
         if (wt_at(i).t == 1700000000) {
            first = wt_at(i);
            got   = 1;
         }
      ck(got, "the first weight is in the tail");
      setenv("APP_FAIL_RENAME", "1", 1);
      int rc = got ? weight_delete(&first) : -1;
      unsetenv("APP_FAIL_RENAME");
      ck(rc != 0, "a rename that fails fails the delete");
      ck(file_size(weight_path()) == before,
         "...and the log is exactly as it was");
      ck(weight_load() == 0, "...and still reads whole");
      ck(wt_count() == 2, "...with both weights still in it");
   }

   printf("== a flush that fails on an EXISTING log takes the row back ==\n");
   /* THE CASE THE CREATION TESTS MISS. By the time the flush runs, write(2)
    * has returned and the row IS in the file. Reported as a plain failure,
    * the caller retries and the log ends up holding the record TWICE -- a
    * dose or a reading duplicated, in a file that is never rewritten. So the
    * row is taken back and the file left exactly as long as it was; if that
    * take-back cannot itself be made durable, the answer is DAMAGED rather
    * than a clean failure. */
   for (int i = 0; i < NLOGS; i++) {
      for (int f = 0; f < 2; f++) {
         const char *fault = f ? "APP_FAIL_CLOSE" : "APP_FAIL_FSYNC";
         fresh();
         snprintf(msg, sizeof msg, "%s: a first record is logged (%s)",
                  LOGS[i].name, fault);
         ck(LOGS[i].append() == 0, msg);
         long before = file_size(LOGS[i].path());
         setenv(fault, "1", 1);
         int rc = LOGS[i].append2();
         unsetenv(fault);
         snprintf(msg, sizeof msg,
                  "%s: a %s on an existing log fails the "
                  "append",
                  LOGS[i].name, fault);
         ck(rc != 0, msg);
         snprintf(msg, sizeof msg,
                  "%s: ...and the log is exactly as long as "
                  "it was",
                  LOGS[i].name);
         ck(file_size(LOGS[i].path()) == before, msg);
         snprintf(msg, sizeof msg,
                  "%s: ...with the row taken back, not left "
                  "in it",
                  LOGS[i].name);
         ck(count_rows(LOGS[i].path()) == 1, msg);
         /* THE RETRY, which is what a caller does with a reported failure. */
         snprintf(msg, sizeof msg,
                  "%s: ...so the retry leaves TWO records, "
                  "not three",
                  LOGS[i].name);
         ck(LOGS[i].append2() == 0 && count_rows(LOGS[i].path()) == 2, msg);
      }
   }

   printf("== a rollback that fails is DAMAGE, on every log ==\n");
   /* The distinction the callers now propagate: an append that failed leaves
    * the file as it was (LOG_FAIL); an append whose partial row could not be
    * truncated away leaves a half line the next append would splice onto
    * (LOG_DAMAGED). Reported as the same thing, a caller retries onto a
    * damaged tail and the two rows parse as one fabricated record. */
   for (int i = 0; i < NLOGS; i++) {
      fresh();
      snprintf(msg, sizeof msg, "%s: a first record is logged", LOGS[i].name);
      ck(LOGS[i].append() == 0, msg);
      struct rlimit old;
      getrlimit(RLIMIT_FSIZE, &old);
      long sz = file_size(LOGS[i].path());
      /* Room for a few bytes of the next row and no more. */
      struct rlimit tight = {(rlim_t)sz + 4, old.rlim_max};
      signal(SIGXFSZ, SIG_IGN);
      int armed = setrlimit(RLIMIT_FSIZE, &tight) == 0;
      setenv("APP_FAIL_TRUNCATE", "1", 1);
      int rc = armed ? LOGS[i].append2() : 0;
      unsetenv("APP_FAIL_TRUNCATE");
      setrlimit(RLIMIT_FSIZE, &old);
      snprintf(msg, sizeof msg,
               "%s: a rollback that fails answers DAMAGED, "
               "not merely FAILED",
               LOGS[i].name);
      ck(armed && rc == LOG_DAMAGED, msg);
   }

   printf("== ...and the caller does not claim the record it lost ==\n");
   /* The distinction the callers now propagate: an append that failed leaves
    * the file as it was; an append whose partial row could not be truncated
    * away leaves a half line that the NEXT append will splice onto. */
   {
      fresh();
      ck(append_weight() == 0, "a first weight is logged");
      struct rlimit old;
      getrlimit(RLIMIT_FSIZE, &old);
      long sz = file_size(weight_path());
      /* Room for a few bytes of the next row and no more. */
      struct rlimit tight = {(rlim_t)sz + 4, old.rlim_max};
      signal(SIGXFSZ, SIG_IGN);
      int armed = setrlimit(RLIMIT_FSIZE, &tight) == 0;
      setenv("APP_FAIL_TRUNCATE", "1", 1);
      int rc = armed ? weight_append(1700000600, 70500, 0) : -1;
      unsetenv("APP_FAIL_TRUNCATE");
      setrlimit(RLIMIT_FSIZE, &old);
      ck(armed, "the short-write case is armed");
      ck(rc == LOG_DAMAGED, "an append whose rollback fails says DAMAGED");
      /* The file now holds a half row -- that is the damage this reports.
       * What must NOT happen is the app claiming the weight was logged. */
      ck(wt_count() == 1, "...and the tail does not claim the weight it lost");
   }

   printf("\n== a failed append does not eat ANOTHER writer's row ==\n");
   /* THE RACE THE ROLLBACK CREATES.
    *
    * When the flush fails, the row is already in the file, so log_append
    * takes it back -- truncating `rowlen` bytes from the end AS IT STANDS,
    * because O_APPEND means an offset sampled before the write may no longer
    * be ours. Between the failed flush and the reopen there is a gap, and
    * another thread really can append into it: store_record releases the
    * history lock before store_append, so two threads reach log_append for
    * readings.csv at once. The take-back then removes the OTHER writer's
    * row -- a record that was accepted, acknowledged, and silently deleted --
    * or half of it, which splices the next append onto a partial line.
    *
    * One thread fails EVERY flush (per-thread arming: the environment switch
    * is process-wide and this case needs one writer to fail while the other
    * succeeds); the other must come through with every row it was told was
    * written. */
   {
      fresh();
      /* Seed, so the file exists and every append below takes the
       * existing-log path -- the one with the take-back in it. */
      ck(append_weight() == 0, "the log exists before the two writers start");

      /* <pthread.h> IS included above; glibc defines pthread_t in a private
       * header with no pragma back to it, so include-cleaner asks for that
       * private header by name -- the same one-line silence threadtest uses.
       */
      /* NOLINTNEXTLINE(misc-include-cleaner) */
      pthread_t th   = 0;
      struct racer r = {0, 0};
      ck(pthread_create(&th, 0, faulty_writer, &r) == 0,
         "a writer that fails every flush starts");
      int wrote = 0;
      for (int i = 0; i < RACE_ROUNDS; i++)
         if (log_append(weight_path(), 0, 0, GOOD_ROW,
                        (int)sizeof GOOD_ROW - 1) == LOG_OK)
            wrote++;
      pthread_join(th, 0);

      ck(wrote == RACE_ROUNDS, "every one of the good writer's appends was "
                               "reported as written");
      ck(r.runs == RACE_ROUNDS,
         "...having tried as many times as the good one");
      ck(r.ok == 0, "...and every one of the failing writer's was refused");
      /* WHAT IT WAS TOLD IS WHAT IS THERE. The good rows are all present and
       * whole; none of the refused rows is, and no line is a fragment of one
       * spliced onto another. */
      int good    = 0;
      int other   = 0;
      int partial = 0;
      count_kinds(weight_path(), &good, &other, &partial);
      ck(good == wrote, "every acknowledged row is in the file, once");
      ck(other == 0, "no refused row was left behind");
      ck(partial == 0, "and no line is a fragment of one");
   }

   printf("\n%s\n",
          fails ? "DURABILITY TESTS FAILED" : "ALL DURABILITY TESTS PASSED");
   return fails ? 1 : 0;
}
