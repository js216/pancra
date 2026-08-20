// SPDX-License-Identifier: GPL-3.0
// syncstattest.c --- What a sync attempt ended as (see syncstat.h)
// Copyright 2026 Jakob Kastelic
//
/* THE DEFECT THIS MODULE EXISTS TO PREVENT, and this test to keep prevented:
 * the outcome of a sync used to be an English string, carried to the screen
 * and then compared against a hand-written list of phrases to pick a colour.
 * Two of the phrases the app actually emitted were missing from that list, so
 * a restore that worked rendered in the same grey as a phone that had never
 * synced -- and nothing could have told anyone, because a list of strings has
 * no such thing as completeness.
 *
 * So the checks below are mostly completeness checks: EVERY value of the enum
 * has a label, a severity and a retry answer, all of them distinct from the
 * fallbacks, and every Java failure kind and HTTP status maps to one. */

#include "syncstat.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
   if (!cond) {
      printf("FAIL: %s\n", what);
      fails++;
   }
}

int main(void)
{
   printf("== every outcome is rendered, and rendered differently ==\n");
   {
      /* The property the string version could not have. If someone adds an
       * outcome and forgets syncstat.c, the switch there fails to compile
       * (-Wswitch-enum, no default) -- but only for the cases that ARE listed:
       * a new enumerator whose label falls through to the fallback would still
       * build. This catches that: the fallback text may appear exactly once,
       * for SYNC_FAILED itself. */
      int fallbacks = 0;
      for (int i = 0; i < SYNC_OUTCOME_N; i++) {
         const char *l = sync_outcome_label(i);
         if (!l || !l[0]) {
            ck(0, "every outcome has a label");
            continue;
         }
         ck(strlen(l) <= 13, "...that fits the LAST STATUS row");
         if (!strcmp(l, "SYNC FAILED"))
            fallbacks++;
         for (int j = 0; j < i; j++)
            ck(strcmp(l, sync_outcome_label(j)) != 0,
               "...and no two outcomes share one");
      }
      ck(fallbacks == 1, "only SYNC_FAILED reads SYNC FAILED -- a new outcome "
                         "must not fall through to it");

      /* Out of range is not a crash and not silence: an outcome that arrives
       * from the wrong side of JNI still has to render something. */
      ck(!strcmp(sync_outcome_label(-1), "SYNC FAILED"), "an impossible code "
                                                         "still renders");
      ck(!strcmp(sync_outcome_label(SYNC_OUTCOME_N), "SYNC FAILED"),
         "...including the count itself");
      ck(sync_outcome_severity(999) == SYNC_SEV_BAD, "...and reads as bad");
      ck(sync_outcome_retries(999) == 1, "...and is retried, because the app "
                                         "not trying again is the worse of "
                                         "the two mistakes");
   }

   printf("== success, and the two that used to render grey ==\n");
   {
      ck(sync_outcome_severity(SYNC_OK) == SYNC_SEV_GOOD, "a sync that worked "
                                                          "reads good");
      ck(sync_outcome_severity(SYNC_PAIRED) == SYNC_SEV_GOOD, "so does a "
                                                              "pairing");
      /* THE ORIGINAL BUG, in one line each. */
      ck(sync_outcome_severity(SYNC_RESTORED) == SYNC_SEV_GOOD,
         "a restore that brought data back reads good, not blank");
      ck(sync_outcome_severity(SYNC_NOTHING_NEW) == SYNC_SEV_GOOD,
         "a restore that found nothing missing is also success");
      ck(sync_outcome_severity(SYNC_IDLE) == SYNC_SEV_NONE,
         "nothing attempted yet is neither");
      ck(!strcmp(sync_outcome_label(SYNC_IDLE), "--"), "...and shows a dash");
   }

   printf("== what the user can act on vs what the app retries ==\n");
   {
      /* The other half of the original bug: every network failure collapsed
       * into SYNC FAILED, so a mistyped server name and a flat network were
       * the same word -- and the app backed off on both, which helps neither.
       */
      ck(!sync_outcome_retries(SYNC_NOT_PAIRED), "an unpaired phone is not "
                                                 "retried into being paired");
      ck(!sync_outcome_retries(SYNC_DNS), "a name that does not resolve will "
                                          "not resolve on the next try");
      ck(!sync_outcome_retries(SYNC_TLS), "a refused certificate is the "
                                          "user's to fix");
      ck(!sync_outcome_retries(SYNC_AUTH), "a rejected key needs re-pairing, "
                                           "not patience");
      ck(!sync_outcome_retries(SYNC_PAIR_REFUSED), "a refused pairing code is "
                                                   "typed again, not retried");
      ck(!sync_outcome_retries(SYNC_KEY_NOT_SAVED), "a key that did not "
                                                    "persist is a local "
                                                    "failure");
      ck(sync_outcome_retries(SYNC_TIMEOUT), "a timeout is retried");
      ck(sync_outcome_retries(SYNC_UNREACHABLE), "so is an unreachable "
                                                 "server");
      ck(sync_outcome_retries(SYNC_SERVER), "so is a server having trouble");
      ck(sync_outcome_retries(SYNC_BAD_REPLY), "so is an answer we could not "
                                               "use");
      ck(sync_outcome_retries(SYNC_FAILED), "and so is anything unclassified");

      /* Severity and retry are NOT the same axis, and the screen must not
       * conflate them: the failures the user can fix are amber (do
       * something), the ones the app is working on are red (it is broken but
       * being handled). */
      for (int i = 0; i < SYNC_OUTCOME_N; i++) {
         if (sync_outcome_severity(i) == SYNC_SEV_WARN)
            ck(!sync_outcome_retries(i),
               "an outcome the app keeps retrying is not the user's to fix");
      }
   }

   printf("== the Java transport's failure kinds ==\n");
   {
      /* Ble.syncHttp catches a Throwable and passes a number; these are the
       * only numbers it can pass, and each has to name a different outcome or
       * the classification was pointless. */
      ck(sync_outcome_of_net(SYNC_NET_OK) == SYNC_IDLE, "no failure reports "
                                                        "no outcome");
      ck(sync_outcome_of_net(SYNC_NET_DNS) == SYNC_DNS, "UnknownHost is a "
                                                        "name failure");
      ck(sync_outcome_of_net(SYNC_NET_TIMEOUT) == SYNC_TIMEOUT, "a timeout is "
                                                                "a timeout");
      ck(sync_outcome_of_net(SYNC_NET_TLS) == SYNC_TLS, "a handshake failure "
                                                        "is a TLS failure");
      ck(sync_outcome_of_net(SYNC_NET_UNREACHABLE) == SYNC_UNREACHABLE,
         "no route is unreachable");
      ck(sync_outcome_of_net(SYNC_NET_OTHER) == SYNC_FAILED, "anything else "
                                                             "stays generic");
      ck(sync_outcome_of_net(42) == SYNC_FAILED, "an unknown kind is generic, "
                                                 "not a crash");
   }

   printf("== what an HTTP status means ==\n");
   {
      ck(sync_outcome_of_status(200) == SYNC_IDLE, "200 is not a failure");
      ck(sync_outcome_of_status(204) == SYNC_IDLE, "nor is 204");
      /* -1 is the transport's "no answer" sentinel, NOT a status: reading it
       * as one would report BAD REPLY for a phone with no network. */
      ck(sync_outcome_of_status(-1) == SYNC_IDLE, "the no-answer sentinel is "
                                                  "not a status");
      ck(sync_outcome_of_status(0) == SYNC_IDLE, "neither is zero");
      ck(sync_outcome_of_status(401) == SYNC_AUTH, "401 means our key was "
                                                   "refused");
      ck(sync_outcome_of_status(403) == SYNC_AUTH, "so does 403");
      ck(sync_outcome_of_status(500) == SYNC_SERVER, "5xx is the server's "
                                                     "problem");
      ck(sync_outcome_of_status(503) == SYNC_SERVER, "...whichever 5xx");
      ck(sync_outcome_of_status(404) == SYNC_BAD_REPLY, "a 4xx we did not "
                                                        "expect is an answer "
                                                        "we cannot use");
      ck(sync_outcome_of_status(409) == SYNC_BAD_REPLY, "...as is a conflict");
      ck(sync_outcome_of_status(302) == SYNC_BAD_REPLY, "a redirect too: this "
                                                        "protocol has none");
   }

   printf("== a pairing tells 'refused' from 'never reached' ==\n");
   {
      /* sync_pair returns one failure for all of them, so syncjni.c decides
       * with this: a code the server turned down vs a server that never
       * answered. Getting it backwards tells a user with no network to check
       * the code they typed. */
      ck(sync_outcome_before_reply(SYNC_DNS), "a name failure happened before "
                                              "any reply");
      ck(sync_outcome_before_reply(SYNC_TLS), "so did a refused handshake");
      ck(sync_outcome_before_reply(SYNC_UNREACHABLE), "so did no route");
      ck(sync_outcome_before_reply(SYNC_TIMEOUT), "so did no answer in time");
      ck(sync_outcome_before_reply(SYNC_SERVER),
         "a 500 is the server failing, not a verdict on what we sent");
      ck(!sync_outcome_before_reply(SYNC_AUTH), "a refusal IS a verdict");
      ck(!sync_outcome_before_reply(SYNC_BAD_REPLY), "so is an answer we read "
                                                     "and could not use");
      ck(!sync_outcome_before_reply(SYNC_IDLE), "nothing attempted is not a "
                                                "transport failure");
      ck(!sync_outcome_before_reply(SYNC_FAILED), "nor is an unclassified "
                                                  "one: an unknown failure "
                                                  "must not excuse the code");

      /* Everything that answers yes must be a failure the app retries: these
       * are exactly the cases where nothing is known about what was sent. */
      for (int i = 0; i < SYNC_OUTCOME_N; i++)
         if (sync_outcome_before_reply(i))
            ck(sync_outcome_severity(i) != SYNC_SEV_GOOD,
               "a successful outcome never counts as a transport failure");
   }

   printf("\n%s\n", fails ? "SYNCSTAT TESTS FAILED"
                          : "ALL SYNCSTAT TESTS "
                            "PASSED");
   return fails ? 1 : 0;
}
