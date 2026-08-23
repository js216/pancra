// SPDX-License-Identifier: GPL-3.0
// syncstat.h --- What a sync attempt ENDED AS, as a value
// Copyright 2026 Jakob Kastelic
//
/* THE OUTCOME IS A CODE, NOT A SENTENCE.
 *
 * AN ENGLISH STRING carried to the screen and pattern-matched to decide a
 * colour -- a pair of string comparisons against "SYNCED" and "PAIRED" --
 * goes wrong in two ways, both of the kind that never announce themselves:
 *
 *   - RESTORED and NOTHING TO RESTORE matched nothing, so a restore that
 *     worked rendered in the same grey as a screen that had never synced;
 *   - every network failure collapsed into one string. A wrong server name, a
 *     dead network, an expired certificate, a rejected key and a server
 *     returning 500 all read "SYNC FAILED", which tells the user nothing about
 *     which of those they can do something about -- and three of them they
 *     can.
 *
 * So the outcome travels as a code with a severity, the label is derived from
 * the code in ONE place, and the renderer colours by severity. Adding an
 * outcome without a label is a build error: the mapping is a switch over this
 * enum with no default, under -Wswitch-enum.
 */
#ifndef SYNCSTAT_H
#define SYNCSTAT_H

enum sync_outcome {
   /* Nothing has been attempted yet this launch. */
   SYNC_IDLE = 0,
   /* Worked. */
   SYNC_OK,
   SYNC_PAIRED,
   SYNC_RESTORED,
   SYNC_NOTHING_NEW, /* a restore that found nothing missing */
   /* RESTORED, BUT NOT PROVEN DURABLE. The rows are in the log and the next
    * load reads them; a durability step after the rename failed, so a power
    * loss could still lose them. Its own outcome because the two honest
    * alternatives are both lies: "RESTORED" promises the record survives a
    * reboot, and a failure says nothing came back when everything did. */
   SYNC_RESTORED_UNSYNCED,
   /* The user has to do something. */
   SYNC_NOT_PAIRED,   /* no identity: pair the phone first */
   SYNC_AUTH,         /* the server rejected our signature */
   SYNC_DNS,          /* the server name does not resolve */
   SYNC_TLS,          /* certificate or handshake refused */
   SYNC_PAIR_REFUSED, /* the server turned the pairing code down */
   /* Transient: worth retrying, and the app does. */
   SYNC_UNREACHABLE, /* no route / connection refused */
   SYNC_TIMEOUT,
   SYNC_SERVER,    /* 5xx: the server is having trouble */
   SYNC_BAD_REPLY, /* an answer we could not use */
   /* Local failures. */
   SYNC_KEY_NOT_SAVED, /* paired, but the identity did not persist */
   SYNC_FAILED,        /* anything else */
   SYNC_OUTCOME_N
};

/* How a reader should FEEL about an outcome, which is what the screen
 * colours by. Deliberately three: "it worked", "it did not and you can help",
 * "it did not and the app will keep trying". */
enum sync_severity {
   SYNC_SEV_NONE = 0,
   SYNC_SEV_GOOD,
   SYNC_SEV_WARN,
   SYNC_SEV_BAD
};

/* WHAT THE JAVA TRANSPORT SAW. Ble.syncHttp catches a Throwable and cannot
 * hand a C enum back across JNI, so it classifies the exception into one of
 * these and passes the number; the mapping to an outcome is the switch below,
 * in C, where it can be tested. These values are mirrored by
 * NetPolicy.NET_* and the two are compared by `make javacheck` -- a
 * renumbering on one side alone would silently turn a timeout into a DNS
 * failure on the screen. */
enum sync_net_fail {
   SYNC_NET_OK = 0,
   SYNC_NET_DNS,
   SYNC_NET_TIMEOUT,
   SYNC_NET_TLS,
   SYNC_NET_UNREACHABLE,
   SYNC_NET_OTHER
};

/* ---- THE TYPE TRAVELS --------------------------------------
 *
 * Every one of these declared `int` and every caller kept the answer in an
 * `int`, so the enum above described a domain nothing enforced: a status
 * code, a net-failure code and an outcome are three unrelated numbering
 * schemes, all of them assignable to each other without a word from the
 * compiler. sync_outcome_of_status(SYNC_NET_TLS) compiles perfectly, and
 * what it produces is a label about something else.
 *
 * The types are end to end now. The raw `int` remains at exactly two
 * boundaries and both are named: the HTTP status, which is a number off the
 * wire, and the Java NET_* code, which crosses JNI. Each converts through the
 * function below that takes it. */
enum sync_outcome sync_outcome_of_net(enum sync_net_fail netfail);

/* The outcome an HTTP status means, or SYNC_IDLE for a status that is not a
 * failure. A status is the server ANSWERING, so it outranks anything the
 * transport guessed. */
enum sync_outcome sync_outcome_of_status(int status);

/* The one place a code becomes words. Bounded, upper-case, and short enough
 * for the LAST STATUS row (12 characters). */
const char *sync_outcome_label(enum sync_outcome outcome);

/* Whether the failure happened BEFORE the server gave a usable answer, so it
 * says nothing about what was sent. A pairing uses this to tell "your code
 * was refused" from "the server was never reached", which look identical from
 * inside sync.c. */
int sync_outcome_before_reply(enum sync_outcome outcome);

/* Its severity, for the renderer. */
enum sync_severity sync_outcome_severity(enum sync_outcome outcome);

/* Whether the app will retry this on its own. A user reading "TIMEOUT" should
 * not have to wonder; a user reading "NOT PAIRED" should not wait for a retry
 * that will never help. */
int sync_outcome_retries(enum sync_outcome outcome);

#endif
