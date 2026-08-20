// SPDX-License-Identifier: GPL-3.0
// foodtest.c --- Host tests for the food vocabulary and entry log
// Copyright 2026 Jakob Kastelic

/* Behavioural gate for food.c.
 *
 * THE INTERESTING PART IS THE JOIN. The entry log is the weight log with one
 * more column, and it is checked the same way. What is new is that an entry
 * points at a type, across two files with two lifetimes -- so the failures
 * worth writing tests for are the ones that only exist because there are two:
 *
 *   - an entry whose type is not in the vocabulary (a meal the app cannot
 *     name), which must be reported rather than drawn as a blank row;
 *   - the load ORDER, since an entry can only be checked against a vocabulary
 *     that has already been read;
 *   - id minting after a load, which must not re-issue an id the file already
 *     used;
 *   - a name that cannot survive the CSV round trip, which must be refused at
 *     the point it is offered rather than repaired into something else.
 *
 * Built and run by `make foodtest`.
 */
#include "food.h"
#include "testdir.h" /* test_dir / test_path: the per-mode fixture directory */
#include "util.h"
#include <stdio.h>
#include <string.h>
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

static void put(const char *path, const char *body)
{
   FILE *f = fopen(path, "wb");
   if (!f) {
      printf("  [FAIL] cannot write %s\n", path);
      fails++;
      return;
   }
   fwrite(body, 1, strlen(body), f);
   fclose(f);
}

int main(void)
{
   char dir[256];
   char fpath[300];
   char tpath[300];
   snprintf(dir, sizeof dir, "%s", test_dir());
   if (!food_paths(dir)) {
      printf("  [FAIL] food_paths did not fit\n");
      return 1;
   }
   snprintf(fpath, sizeof fpath, "%s", food_path());
   snprintf(tpath, sizeof tpath, "%s", food_types_path());

   printf("== the vocabulary ==\n");
   {
      (void)unlink(fpath);
      (void)unlink(tpath);
      ck(food_load() == 0, "two missing files are an empty log");
      ck(food_type_count() == 0, "...with no types");
      int a = food_type_add("PORRIDGE");
      ck(a > 0, "a name is added and gets an id");
      int b = food_type_add("TOAST");
      ck(b > 0 && b != a, "...a second gets a different one");
      /* THE SAME FOOD IS THE SAME ID. Two types spelled identically would be
       * indistinguishable in the picker and would split one food's history
       * across two ids, which no later edit could join up. */
      ck(food_type_add("PORRIDGE") == a, "adding a known name reuses its id");
      ck(food_type_count() == 2, "...and does not grow the vocabulary");
      ck(strcmp(food_type_name(a), "PORRIDGE") == 0, "an id names its food");
      ck(strcmp(food_type_name(9999), "") == 0,
         "an id with no type names nothing, rather than crashing");
      ck(food_type_index(a) == 0 && food_type_index(b) == 1,
         "types keep the order they were added in");
      ck(food_type_index(9999) == -1, "...and an unknown id has no index");
   }

   printf("== names that cannot survive the format are refused ==\n");
   {
      int before = food_type_count();
      /* ',' AND '\n' ARE THE FORMAT. A name holding either would not round
       * trip: the comma splits one name into two fields, the newline splits
       * one row into two. Refused rather than stripped -- storing a name the
       * user did not type is worse than refusing the one they did. */
      ck(food_type_add("EGGS, FRIED") == -1, "a name with a comma is refused");
      ck(food_type_add("EGGS\nFRIED") == -1, "a name with a newline is refused");
      ck(food_type_add("") == -1, "an empty name is refused");
      ck(food_type_add(0) == -1, "a null name is refused");
      /* A leading '#' would be read back as the header comment. */
      ck(food_type_add("#TOAST") == -1, "a name starting '#' is refused");
      char toolong[FOOD_NAME_MAX + 8];
      memset(toolong, 'A', sizeof toolong - 1);
      toolong[sizeof toolong - 1] = 0;
      ck(food_type_add(toolong) == -1, "a name past the field width is refused");
      ck(food_type_count() == before, "...and none of them was added");

      char exact[FOOD_NAME_MAX + 1];
      memset(exact, 'B', FOOD_NAME_MAX);
      exact[FOOD_NAME_MAX] = 0;
      ck(food_type_add(exact) > 0, "a name of exactly the field width is fine");
   }

   printf("== entries, and the type they point at ==\n");
   {
      (void)unlink(fpath);
      (void)unlink(tpath);
      ck(food_load() == 0, "start clean");
      int p = food_type_add("PORRIDGE");
      ck(food_append(1700000000L, p, 90, 0) == 0, "an entry appends");
      ck(food_count() == 1, "...and reaches the tail");
      ck(food_newest().type == p && food_newest().g == 90,
         "...with the type and the weight it was given");
      /* AN ENTRY AGAINST A TYPE NOBODY DEFINED is unnameable the moment it is
       * written, and this log is never rewritten -- so it would stay that way.
       * The picker cannot produce such an id, which is exactly why the check
       * belongs in the appender rather than in the picker. */
      ck(food_append(1700000100L, 9999, 90, 0) == -1,
         "an entry against an unknown type is refused");
      ck(food_append(1700000100L, FOOD_TYPE_NONE, 90, 0) == -1,
         "...and so is one against no type at all");
      ck(food_append(1700000100L, p, 0, 0) == -1, "an entry of 0 g is refused");
      ck(food_append(1700000100L, p, FOOD_MAX_G + 1, 0) == -1,
         "an implausible portion is refused");
      ck(food_append(0, p, 90, 0) == -1, "an instant of 0 is refused");
      ck(food_append(FOOD_T_MAX, p, 90, 0) == -1,
         "an instant past the epoch bound is refused");
      ck(food_count() == 1, "...and none of them reached the tail");
   }

   printf("== what survives a reload ==\n");
   {
      (void)unlink(fpath);
      (void)unlink(tpath);
      ck(food_load() == 0, "start clean");
      int p = food_type_add("PORRIDGE");
      int t = food_type_add("TOAST");
      ck(food_append(1700000200L, t, 40, 0) == 0, "append one");
      ck(food_append(1700000100L, p, 90, 0) == 0, "...and an EARLIER one");
      ck(food_load() == 0, "both files reload whole");
      ck(food_type_count() == 2, "...with the vocabulary intact");
      ck(food_count() == 2, "...and both entries");
      ck(food_at(0).t == 1700000100L && food_at(0).type == p,
         "oldest first, whatever order they arrived in");
      ck(food_at(1).t == 1700000200L && food_at(1).type == t, "...newest last");
      /* ID MINTING AFTER A LOAD is the join's other half: g_next_id has to end
       * up past every id the file held, or the next name added collides with
       * one already in use and two foods share an id -- which makes every
       * entry against it ambiguous, silently. */
      int n = food_type_add("EGGS");
      ck(n != p && n != t, "a name added after a load gets a FRESH id");
      ck(food_type_index(n) >= 0, "...and is in the vocabulary");
   }

   printf("== a damaged pair is REPORTED, and its good rows kept ==\n");
   {
      /* An entry naming a type that is not in the vocabulary: the join's
       * failure, and the reason the two files are loaded in one call. */
      put(tpath, "# type_id,name\n1,PORRIDGE\n");
      put(fpath, "# unix_time,type_id,grams,tz_offset_s\n"
                 "1700000100,1,90,0\n1700000200,7,40,0\n");
      ck(food_load() == -1, "an entry with an unknown type is reported");
      ck(food_count() == 1, "...and the entries that DID resolve are kept");
      ck(food_at(0).type == 1, "...namely the one whose type exists");

      put(tpath, "# type_id,name\n1,PORRIDGE\n");
      put(fpath, "# unix_time,type_id,grams,tz_offset_s\n1700000100,1,90,0");
      ck(food_load() == -1, "a final line with no newline is reported");
      ck(food_count() == 0, "...and that line is NOT taken as a row");

      /* A DUPLICATE ID makes every entry using it ambiguous, so the file is
       * damaged even though both rows parse. */
      put(tpath, "# type_id,name\n1,PORRIDGE\n1,TOAST\n");
      put(fpath, "");
      ck(food_load() == -1, "two types sharing an id is damage");
      ck(food_type_count() == 1, "...and only the first is kept");

      put(tpath, "# type_id,name\n0,NOTHING\n");
      put(fpath, "");
      ck(food_load() == -1, "a type with the reserved id 0 is damage");
      ck(food_type_count() == 0, "...and is not taken");

      /* A name in the FILE that could not have come from food_type_add: the
       * validation has to hold on the way in as well as on the way out. */
      put(tpath, "# type_id,name\n1,\n");
      put(fpath, "");
      ck(food_load() == -1, "a type with an empty name is damage");

      put(tpath, "# type_id,name\n1,PORRIDGE\n");
      put(fpath, "# unix_time,type_id,grams,tz_offset_s\n99999999999,1,90,0\n");
      ck(food_load() == -1, "an entry past the epoch bound is damage");
      ck(food_count() == 0, "...and is not taken");

      put(tpath, "# type_id,name\n1,PORRIDGE\n");
      put(fpath, "# unix_time,type_id,grams,tz_offset_s\n1700000100,1,90,0\n");
      ck(food_load() == 0, "a clean pair reports whole");
      ck(food_count() == 1 && food_type_count() == 1, "...and holds both");
   }

   printf("== the tail keeps the NEWEST entries by time ==\n");
   {
      (void)unlink(fpath);
      (void)unlink(tpath);
      /* Reloaded, not merely unlinked: the tail is a cache of what was loaded,
       * so a section that only unlinks starts with whatever the last one left
       * behind -- and this section counts evictions. */
      ck(food_load() == 0, "an unlinked pair reloads as empty");
      ck(food_count() == 0, "...and the tail is reset with it");
      int p = food_type_add("PORRIDGE");
      for (int i = 0; i < NFOOD + 10; i++)
         (void)food_append(1700000000L + i, p, 50 + (i % 10), 0);
      ck(food_count() == NFOOD, "the tail is capped");
      ck(food_at(0).t == 1700000000L + 10,
         "...and holds the newest NFOOD, so the oldest fell off the front");
      long newest = food_newest().t;
      ck(food_append(1600000000L, p, 60, 0) == 0, "an OLD entry appends");
      ck(food_newest().t == newest,
         "...and does not displace anything newer than itself");
      ck(food_at(0).t == 1700000000L + 10, "...nor take a slot at the front");
   }

   printf("== the vocabulary is bounded ==\n");
   {
      (void)unlink(fpath);
      (void)unlink(tpath);
      ck(food_load() == 0, "start clean");
      char nm[FOOD_NAME_MAX + 1];
      int added = 0;
      for (int i = 0; i < NFOODTYPE + 5; i++) {
         snprintf(nm, sizeof nm, "FOOD%04d", i);
         if (food_type_add(nm) > 0)
            added++;
      }
      ck(added == NFOODTYPE, "exactly NFOODTYPE names are accepted");
      ck(food_type_count() == NFOODTYPE, "...and the table holds that many");
      /* REFUSED, not rotated. A vocabulary that silently drops the oldest
       * food would orphan every entry logged against it -- and those entries
       * are in a file that is never rewritten. */
      snprintf(nm, sizeof nm, "ONE MORE");
      ck(food_type_add(nm) == -1, "one past the cap is refused");
      ck(food_type_index(1) >= 0, "...and the FIRST food is still there");
      /* REFUSED BEFORE IT IS WRITTEN, which the return value alone does not
       * say. food_type_add appends the row and only then takes it into
       * memory, so a cap enforced solely at the take would leave a type on
       * disk that memory refuses -- and every future load would read it,
       * refuse it again, and report the file as damaged for good. The
       * observable difference is entirely in what the NEXT load says.
       *
       * Measured: with the appender's own cap check removed, every assertion
       * above still passed. */
      ck(food_load() == 0,
         "...and the refusal left nothing unreadable in the file");
      ck(food_type_count() == NFOODTYPE, "...with the vocabulary intact");
   }

   if (fails == 0)
      printf("\nALL FOOD TESTS PASSED\n");
   else
      printf("\n%d FOOD TEST(S) FAILED\n", fails);
   return fails != 0;
}
