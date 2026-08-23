// SPDX-License-Identifier: GPL-3.0
// remotecfg.c --- where this phone syncs to, and who it syncs as
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN. Five unrelated files -- the device's model and
 * firmware, the alarm thresholds, the display preferences, the pairing code
 * and the remote credentials -- behind one save engine is 1541 lines of
 * module with no subject. They share the engine
 * (app/setpriv.h) and the preferences aggregate; they share nothing else, and
 * a reader after one of them had to read past the other four.
 */
#include "remotecfg.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"
#include "setpriv.h"
#include "settings.h" /* struct prefs: the aggregate the engine holds */
#include "thread.h"   /* the lock this module's state sits behind */
#include "util.h"
#include <stdio.h>
#include <string.h>

static long g_sync_uid; /* 0 until an app is paired */
static unsigned char g_sync_key[16];
static char g_sync_email[64];
static struct sync_creds g_creds;

/* Refresh the view. Called after every write to the three above -- they are
 * the storage, this is what everyone else sees. */
static void creds_publish(void)
{
   g_creds.uid = g_sync_uid;
   for (int i = 0; i < (int)sizeof g_creds.key; i++)
      g_creds.key[i] = g_sync_key[i];
   str_snapshot(g_creds.email, sizeof g_creds.email, g_sync_email);
}

/* The identity as a COPY, for the same reason as settings_get: the sync
 * worker rewrites it, and uid-without-key is the state that fails every
 * request with nothing on screen to say why. */
/* THE TWO SETTINGS THAT WRITE THIS FILE, in the module that owns it: a
 * setter whose file belongs to another module is how one file comes to be
 * rendered from two places. */
int remote_set_on(int on)
{
   return set_int_field(&g_p.remote_on, on ? 1 : 0, set_render_remote);
}

int remote_set_port(int port)
{
   if (port < 1 || port > 65535)
      return SETTINGS_UNSAVED;
   return set_int_field(&g_p.remote_port, port, set_render_remote);
}

void remote_creds_get(struct sync_creds *out)
{
   if (!out)
      return;
   mutex_lock(&set_lk);
   *out = g_creds;
   mutex_unlock(&set_lk);
}

/* THE ENDPOINT AND THE IDENTITY TOGETHER, under ONE acquisition. See the
 * header: taken as two, a request can be aimed at one server and signed as
 * an account on another. */
void remote_config_get(struct remote_config *out)
{
   if (!out)
      return;
   mutex_lock(&set_lk);
   out->on   = g_p.remote_on;
   out->port = g_p.remote_port;
   str_snapshot(out->server, (int)sizeof out->server, g_p.remote_server);
   out->uid = g_creds.uid;
   for (int i = 0; i < (int)sizeof out->key; i++)
      out->key[i] = g_creds.key[i];
   str_snapshot(out->email, (int)sizeof out->email, g_creds.email);
   mutex_unlock(&set_lk);
}

/* 443, because the client always speaks https now: the platform's TLS is
 * free and the session cookie the server sets is Secure. */

int remote_set_server(const char *host)
{
   return set_str_field(g_p.remote_server, (int)sizeof g_p.remote_server, host,
                        set_render_remote);
}

/* The account address lives in the CREDENTIAL, not in prefs, so it needs its
 * own transaction: the published copy has to be refreshed on the way in AND
 * on the way back out. */
int remote_set_email(const char *addr)
{
   mutex_lock(&set_lk);
   char old[sizeof g_sync_email];
   str_snapshot(old, (int)sizeof old, g_sync_email);
   str_snapshot(g_sync_email, sizeof g_sync_email, addr ? addr : "");
   creds_publish();
   struct save_job j;
   set_render_remote(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) { /* see set_int */
         str_snapshot(g_sync_email, sizeof g_sync_email, old);
         creds_publish();
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* FORGET THE PAIRED IDENTITY, keeping the server: re-pairing is the normal
 * way to move this phone to a different account, and it needs a fresh code
 * from the server anyway. The key is zeroed here so no caller has to remember
 * that half a forgotten identity is worse than none -- and if the file cannot
 * be replaced the identity comes BACK WHOLE, because an app that believes it
 * is unpaired while the file still says otherwise re-pairs into a second
 * account. */
int remote_forget_identity(void)
{
   mutex_lock(&set_lk);
   long old_uid = g_sync_uid;
   unsigned char old_key[16];
   for (int i = 0; i < (int)sizeof old_key; i++)
      old_key[i] = g_sync_key[i];
   g_sync_uid = 0;
   for (int i = 0; i < (int)sizeof g_sync_key; i++)
      g_sync_key[i] = 0;
   creds_publish();
   struct save_job j;
   set_render_remote(&j);
   mutex_unlock(&set_lk);
   int bad = set_write_job(&j) != 0;
   if (bad) {
      mutex_lock(&set_lk);
      if (set_gen_now() == j.gen) { /* see set_int */
         g_sync_uid = old_uid;
         for (int i = 0; i < (int)sizeof g_sync_key; i++)
            g_sync_key[i] = old_key[i];
         creds_publish();
      }
      mutex_unlock(&set_lk);
   }
   return bad ? SETTINGS_UNSAVED : SETTINGS_OK;
}

/* THE TWO PUBLIC SAVES. Everything above persists what it changes; these are
 * for the one caller that changes a PAIR under its own lock (alarm.c) and for
 * the load path. They take set_lk; the _locked forms above assume it. */
int remote_server_valid(const char *s)
{
   if (!s || !*s)
      return 0;
   int n     = 0;
   int label = 0;
   for (const char *p = s; *p; p++) {
      if (++n > 63)
         return 0;
      if (*p == '.') {
         if (label == 0 || p[-1] == '-')
            return 0; /* empty label, or one ending in a hyphen */
         label = 0;
         continue;
      }
      int alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9');
      if (!alnum && *p != '-')
         return 0;
      if (label == 0 && *p == '-')
         return 0; /* a label may not start with a hyphen */
      if (++label > 63)
         return 0;
   }
   return label > 0 && s[n - 1] != '-';
}

void set_render_remote(struct save_job *j)
{
   static unsigned written;
   /* "on server port uid keyhex". The two trailing fields were added with
    * pairing; a file without them loads as "not paired", so an upgrade keeps
    * the server and simply asks to pair again. */
   char kh[33];
   static const char hx[] = "0123456789abcdef";
   for (size_t i = 0; i < 16; i++) {
      kh[2 * i]       = hx[g_sync_key[i] >> 4U];
      kh[(2 * i) + 1] = hx[g_sync_key[i] & 15U];
   }
   kh[32] = 0;
   int n  = snprintf(j->buf, sizeof j->buf, "v%d %d %s %d %ld %s %s\n",
                     REMOTE_VERSION, g_p.remote_on ? 1 : 0,
                     g_p.remote_server[0] ? g_p.remote_server : "-",
                     g_p.remote_port, g_sync_uid, g_sync_uid > 0 ? kh : "-",
                     g_sync_email[0] ? g_sync_email : "-");
   set_job_stamp(j, g_remote_path, &written, n, n > 0 && n < 256);
}

enum load_result remote_load(void)
{
   /* 256, matching what remote_save writes. The line holds a host NAME, a
    * port, a user id, a 32-character key and an email address; a buffer sized
    * for anything less truncates the read, so the key comes back malformed and
    * the account comes back empty, and the app looks like it has forgotten a
    * pairing it has actually stored. Both ends of this file are the same size
    * for that reason. */
   /* ONE EXACT READ: the short-read loop, EINTR, and the
    * probe that tells a full buffer from a file longer than this build
    * can hold, all in read_file_exact rather than one unchecked read whose
    * return is taken as the file's length. */
   char b[256];
   int n               = 0;
   enum load_result rr = read_file_exact(g_remote_path, b, sizeof b, &n);
   if (rr != LOAD_OK)
      return rr;
   /* The version first; a newer format is refused whole. See file_version. */
   char *q     = b;
   int filever = set_file_version(b, &q, REMOTE_VERSION);
   if (filever < 0) {
      LOGW("settings: %s is a NEWER format than this build knows; "
           "leaving it alone",
           g_remote_path);
      return LOAD_CORRUPT;
   }
   /* v0 and v1 differ only by the marker: the fields below are read the same
    * way for both, and a v0 file becomes v1 at the next save. A future
    * version that changes a FIELD adds its ordered step here. */
   if (*q != '0' && *q != '1')
      return LOAD_CORRUPT; /* garbage: keep the prior values, like every sibling
                              loader */
   int on = *q++ - '0';
   while (*q == ' ')
      q++;
   char host[sizeof g_p.remote_server];
   int k = 0;
   while (*q && *q != ' ' && *q != '\n') {
      if (k >= (int)sizeof host - 1)
         return LOAD_CORRUPT; /* an address that long cannot be valid */
      host[k++] = *q++;
   }
   host[k] = 0;
   while (*q == ' ')
      q++;
   int port = 0;
   int nd   = 0; /* see alarm_load: cap the digits, advance outside the cap */
   while (*q >= '0' && *q <= '9') {
      if (nd < 9) {
         port = (port * 10) + (*q - '0');
         nd++;
      }
      q++;
   }
   /* "-" is the saver's own empty-address marker; anything else must be a
    * well-formed host name, and the port must be a real TCP port. Commit all
    * three together or nothing: a half-applied file (say, a valid port with a
    * corrupt address) could silently re-point the push at the wrong host. */
   int host_ok = (host[0] == '-' && host[1] == 0) || remote_server_valid(host);
   if (!host_ok || port < 1 || port > 65535)
      return LOAD_CORRUPT;
   /* The paired identity and the account, read as SEQUENTIAL TOKENS.
    *
    * They were read by OFFSET -- the email was taken from 32 bytes past the
    * uid, the length of a key in hex. That is right only when a key is
    * actually there: unpaired, the saver writes "-" for the key, so the
    * offset landed inside the next field and the email loaded as "-", which
    * the check below then treated as unset. The address the user had typed
    * vanished on the next launch, with nothing to say why. Never compute a
    * position in a whitespace-separated line; walk it. */
   while (*q == ' ')
      q++;
   long uid = 0;
   int ud   = 0;
   while (*q >= '0' && *q <= '9') {
      if (ud < 18) {
         uid = (uid * 10) + (*q - '0');
         ud++;
      }
      q++;
   }
   while (*q == ' ')
      q++;
   /* the key token, however long it turns out to be */
   char keytok[80];
   int kk = 0;
   while (*q && *q != ' ' && *q != '\n' && kk < (int)sizeof keytok - 1)
      keytok[kk++] = *q++;
   keytok[kk] = 0;
   while (*q == ' ')
      q++;
   char em[sizeof g_sync_email];
   int ek = 0;
   while (*q && *q != ' ' && *q != '\n' && ek < (int)sizeof em - 1)
      em[ek++] = *q++;
   em[ek] = 0;

   unsigned char key[16];
   int keyok = 0;
   if (uid > 0 && kk == 32) {
      keyok = 1;
      for (int i = 0; i < 16 && keyok; i++) {
         int v = 0;
         for (int h = 0; h < 2; h++) {
            char c = keytok[(2 * i) + h];
            int d  = -1;
            if (c >= '0' && c <= '9')
               d = c - '0';
            else if (c >= 'a' && c <= 'f')
               d = (c - 'a') + 10;
            else if (c >= 'A' && c <= 'F')
               d = (c - 'A') + 10;
            if (d < 0) {
               keyok = 0;
               break;
            }
            v = (v * 16) + d;
         }
         key[i] = (unsigned char)v;
      }
   }
   g_p.remote_on   = on;
   g_p.remote_port = port;
   if (ek && !(em[0] == '-' && em[1] == 0)) {
      for (int i = 0;; i++) {
         g_sync_email[i] = em[i];
         if (!em[i])
            break;
      }
   } else {
      g_sync_email[0] = 0;
   }
   if (uid > 0 && keyok) {
      g_sync_uid = uid;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = key[i];
   } else {
      g_sync_uid = 0;
   }
   creds_publish();
   if (host[0] == '-')
      g_p.remote_server[0] = 0;
   else
      for (int i = 0;; i++) {
         g_p.remote_server[i] = host[i];
         if (!host[i])
            break;
      }
   return LOAD_OK;
}

int remote_key_save(long uid, const unsigned char key[16])
{
   /* UNDER set_lk, like every other mutation of this state -- and it was not.
    *
    * This writes the paired identity and then saves it, on the pairing
    * worker's thread, while the main thread reads the same fields through
    * remote_creds_get(). Every other setter in this file takes the lock; this
    * one never did, so the copy a request signs with could be assembled from
    * a uid written here and a key from before it. */
   mutex_lock(&set_lk);
   long old_uid = g_sync_uid;
   unsigned char old_key[16];
   for (int i = 0; i < 16; i++)
      old_key[i] = g_sync_key[i];
   g_sync_uid = uid;
   for (int i = 0; i < 16; i++)
      g_sync_key[i] = key[i];
   creds_publish();
   struct save_job j;
   set_render_remote(&j);
   mutex_unlock(&set_lk);
   if (set_write_job(&j) == 0)
      return 0;
   /* PUT BACK EVERY FIELD. A half-rolled-back identity -- the uid restored
    * while the key stays -- fails every request with nothing on screen to
    * say why. */
   mutex_lock(&set_lk);
   if (set_gen_now() == j.gen) { /* see set_int */
      g_sync_uid = old_uid;
      for (int i = 0; i < 16; i++)
         g_sync_key[i] = old_key[i];
      creds_publish();
   }
   mutex_unlock(&set_lk);
   return -1;
}
