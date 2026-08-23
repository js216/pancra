// SPDX-License-Identifier: GPL-3.0
// syncstat.c --- What a sync attempt ended as (see syncstat.h)
// Copyright 2026 Jakob Kastelic

#include "syncstat.h"

/* EXHAUSTIVE BY CONSTRUCTION. No default: -Wswitch-enum makes an outcome
 * without a label a build error, which is a property a string-based version
 * cannot have -- an outcome reaches the screen with no colour and nobody
 * notices. */
const char *sync_outcome_label(enum sync_outcome outcome)
{
   switch (outcome) {
      case SYNC_IDLE: return "--";
      case SYNC_OK: return "SYNCED";
      case SYNC_PAIRED: return "PAIRED";
      case SYNC_RESTORED: return "RESTORED";
      /* The '?' is the whole message: the rows are back, and whether they
       * survive a reboot is not something this phone can promise. */
      case SYNC_RESTORED_UNSYNCED: return "RESTORED?";
      case SYNC_NOTHING_NEW: return "UP TO DATE";
      case SYNC_NOT_PAIRED: return "NOT PAIRED";
      case SYNC_AUTH: return "KEY REFUSED";
      case SYNC_DNS: return "NO SUCH HOST";
      case SYNC_TLS: return "TLS REFUSED";
      case SYNC_PAIR_REFUSED: return "CODE REFUSED";
      case SYNC_UNREACHABLE: return "UNREACHABLE";
      case SYNC_TIMEOUT: return "TIMED OUT";
      case SYNC_SERVER: return "SERVER ERROR";
      case SYNC_BAD_REPLY: return "BAD REPLY";
      case SYNC_KEY_NOT_SAVED: return "KEY NOT SAVED";
      case SYNC_FAILED: return "SYNC FAILED";
      case SYNC_OUTCOME_N: break;
   }
   return "SYNC FAILED";
}

enum sync_severity sync_outcome_severity(enum sync_outcome outcome)
{
   switch (outcome) {
      case SYNC_IDLE: return SYNC_SEV_NONE;
      case SYNC_OK:
      case SYNC_PAIRED:
      case SYNC_RESTORED:
      case SYNC_NOTHING_NEW: return SYNC_SEV_GOOD;
      /* WARN, not GOOD: the restore worked. What is unproven is that the
       * storage kept it, and colouring that green is how a user comes to
       * trust a record that a reboot can still shorten. Not BAD either --
       * nothing failed and there is nothing to retry. */
      case SYNC_RESTORED_UNSYNCED: return SYNC_SEV_WARN;
      /* WARN, not BAD: the app cannot fix these by trying again, but nothing
       * is broken either -- the user has a server name to correct, a phone to
       * pair, or a certificate to look at. */
      case SYNC_NOT_PAIRED:
      case SYNC_AUTH:
      case SYNC_DNS:
      case SYNC_TLS:
      case SYNC_PAIR_REFUSED:
      case SYNC_KEY_NOT_SAVED: return SYNC_SEV_WARN;
      case SYNC_UNREACHABLE:
      case SYNC_TIMEOUT:
      case SYNC_SERVER:
      case SYNC_BAD_REPLY:
      case SYNC_FAILED: return SYNC_SEV_BAD;
      case SYNC_OUTCOME_N: break;
   }
   return SYNC_SEV_BAD;
}

int sync_outcome_retries(enum sync_outcome outcome)
{
   switch (outcome) {
      /* Retrying cannot help: the identity, the name, the certificate or the
       * key is wrong, and only the user can change any of them. */
      case SYNC_NOT_PAIRED:
      case SYNC_AUTH:
      case SYNC_DNS:
      case SYNC_TLS:
      case SYNC_PAIR_REFUSED:
      case SYNC_KEY_NOT_SAVED:
      /* And nothing to retry here either, for the opposite reason: this one
       * SUCCEEDED. The rows are in the file, so a second restore finds nothing
       * missing and could not make the first more durable anyway. It is amber
       * (see the severity above), and syncstattest holds the pair together:
       * an outcome the app keeps retrying must not also be one it shows the
       * user as theirs to act on. */
      case SYNC_RESTORED_UNSYNCED: return 0;
      case SYNC_IDLE:
      case SYNC_OK:
      case SYNC_PAIRED:
      case SYNC_RESTORED:
      case SYNC_NOTHING_NEW:
      case SYNC_UNREACHABLE:
      case SYNC_TIMEOUT:
      case SYNC_SERVER:
      case SYNC_BAD_REPLY:
      case SYNC_FAILED: return 1;
      case SYNC_OUTCOME_N: break;
   }
   return 1;
}

enum sync_outcome sync_outcome_of_net(enum sync_net_fail netfail)
{
   switch (netfail) {
      case SYNC_NET_OK: return SYNC_IDLE;
      case SYNC_NET_DNS: return SYNC_DNS;
      case SYNC_NET_TIMEOUT: return SYNC_TIMEOUT;
      case SYNC_NET_TLS: return SYNC_TLS;
      case SYNC_NET_UNREACHABLE: return SYNC_UNREACHABLE;
      case SYNC_NET_OTHER: return SYNC_FAILED;
   }
   return SYNC_FAILED;
}

enum sync_outcome sync_outcome_of_status(int status)
{
   /* Below 100 is not a status at all: it is the transport's "no answer"
    * sentinel, and the caller has a net-failure kind for that case. */
   if (status < 100)
      return SYNC_IDLE;
   if (status / 100 == 2)
      return SYNC_IDLE;
   /* 401 and 403 are the server saying our signature is not one it accepts --
    * the one failure a user fixes by pairing again, and the one worth telling
    * apart from a flat network. */
   if (status == 401 || status == 403)
      return SYNC_AUTH;
   if (status / 100 == 5)
      return SYNC_SERVER;
   return SYNC_BAD_REPLY;
}

int sync_outcome_before_reply(enum sync_outcome outcome)
{
   switch (outcome) {
      /* Nothing usable came back: a name that did not resolve, a handshake
       * that was refused, no route, no answer in time, or a server that
       * answered only to say it was broken. None of these is a verdict on
       * what was sent. */
      case SYNC_DNS:
      case SYNC_TLS:
      case SYNC_UNREACHABLE:
      case SYNC_TIMEOUT:
      case SYNC_SERVER: return 1;
      case SYNC_IDLE:
      case SYNC_OK:
      case SYNC_PAIRED:
      case SYNC_RESTORED:
      /* The reply arrived and was written; only the flush is in doubt. */
      case SYNC_RESTORED_UNSYNCED:
      case SYNC_NOTHING_NEW:
      case SYNC_NOT_PAIRED:
      case SYNC_AUTH:
      case SYNC_PAIR_REFUSED:
      case SYNC_BAD_REPLY:
      case SYNC_KEY_NOT_SAVED:
      case SYNC_FAILED: return 0;
      case SYNC_OUTCOME_N: break;
   }
   return 0;
}
