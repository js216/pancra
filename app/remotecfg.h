// SPDX-License-Identifier: GPL-3.0
// remotecfg.h --- where this phone syncs to, and who it syncs as
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN PER HEADER. A header declaring five unrelated files'
 * worth of state -- the device's model and firmware, the alarm thresholds,
 * the display preferences, the pairing code and the remote credentials --
 * makes every file that wants one of them
 * included all five. They share a save engine (private: app/setpriv.h) and
 * the preferences aggregate; they share nothing else.
 *
 * THE NAMES MOVED WITH THE DECLARATIONS. A setter that writes THIS file is
 * named for it: what was settings_set_* and lived beside the display
 * preferences is remote_set_* here, so a reader can tell from the call which
 * file it persists to.
 */
#ifndef PANCRA_REMOTECFG_H
#define PANCRA_REMOTECFG_H

#include "loadresult.h"
#include "settings.h" /* struct prefs: the endpoint half of the snapshot */

/* The paired identity, stored in the SAME file as the server -- which is one
 * of the files that must never be synced (see sync.h): it is the secret that
 * authenticates this phone to the server. 0 = not paired. */
/* THE PAIRED IDENTITY, read-only.
 *
 * The uid and key authenticate this phone TO the server, so nothing outside
 * remotecfg.c may write them: a half-written identity (a uid without its key)
 * fails every request with nothing on screen to say why, and a rewritten one
 * silently repoints the account. Changed only by remote_forget_identity and
 * the pairing path, both of which persist what they change. */
struct sync_creds {
   long uid; /* 0 until this phone is paired */
   unsigned char key[16];
   char email[64]; /* the account being synced into; "" = not set */
};

/* WHERE THIS PHONE SYNCS, AND AS WHOM, read as ONE value.
 *
 * The endpoint lives in struct prefs and the identity in struct sync_creds,
 * and taking them with two separate acquisitions of the settings lock --
 * settings_get() then remote_creds_get() -- leaves a window: between those two
 * calls the settings screen can change the server and the pairing worker can
 * change the account -- so a request can be aimed at one server and signed
 * as an account on another, or the reverse. Both fail authentication, and
 * neither says why: the screen shows a server that is correct and an account
 * that is correct, because each half of the pair really is.
 *
 * One acquisition, one value, and it is what scheduling and request
 * construction both carry. */
struct remote_config {
   int on;          /* the sync switch */
   char server[64]; /* host name; "" = not configured */
   int port;
   long uid; /* 0 until this phone is paired */
   unsigned char key[16];
   char email[64];
};

void remote_config_get(struct remote_config *out);

/* THE IDENTITY, as a COPY and only as a copy: the sync worker rewrites it,
 * and a uid read beside a key from before it changed is an identity that
 * fails every request with nothing on screen to say why. */
void remote_creds_get(struct sync_creds *out);
/* Remember a completed pairing, durably. */
int remote_key_save(long uid, const unsigned char key[16]);

/* The sync server and account, copied in bounded and persisted. */
int remote_set_server(const char *host);
int remote_set_email(const char *addr);
/* Forget the paired identity (uid AND key), keeping the server. */
int remote_forget_identity(void);

/* Whether this phone syncs at all, and to which port. */
int remote_set_on(int on);
int remote_set_port(int port);

/* "on ip port\n" -- garbage keeps the prior values, like every loader here. */
enum load_result remote_load(void);

int remote_server_valid(const char *s);


#endif
