// SPDX-License-Identifier: GPL-3.0
// paircode.c --- the six-digit pairing code the sensor was claimed with
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN. Five unrelated files -- the device's model and
 * firmware, the alarm thresholds, the display preferences, the pairing code
 * and the remote credentials -- behind one save engine is 1541 lines of
 * module with no subject. They share the engine
 * (app/setpriv.h) and the preferences aggregate; they share nothing else, and
 * a reader after one of them had to read past the other four.
 */
#include "paircode.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"
#include "setpriv.h"
#include "settings.h" /* struct prefs: the aggregate the engine holds */
#include "util.h"
#include <stdio.h>
#include <string.h>

int code_set(const char *digits)
{
   return set_str_field(g_p.code_str, (int)sizeof g_p.code_str, digits,
                        set_render_code);
}

void set_render_code(struct save_job *j)
{
   static unsigned written;
   int n = 0;
   while (g_p.code_str[n] && n < (int)sizeof j->buf)
      n++;
   memcpy(j->buf, g_p.code_str, (size_t)n);
   set_job_stamp(j, g_code_path, &written, n, 1);
}

/* A hostname or an IPv4 address: labels of letters, digits and hyphens
 * separated by single dots, no empty label, none starting or ending with a
 * hyphen. A dotted quad ONLY is right when the server is a box on the LAN
 * and wrong the moment it gets a name.
 *
 * Deliberately permissive about the whole name -- "duo", "pancra.org" and
 * "192.168.0.243" are all things the user legitimately types -- because this
 * is a field they typed, not a security boundary; what it must not do is
 * accept something that cannot be a host at all and then silently point every
 * future sync at nothing. */
enum load_result code_load(void)
{
   /* ONE EXACT READ, like every other loader here. */
   char b[16];
   int n               = 0;
   enum load_result rr = read_file_exact(g_code_path, b, sizeof b, &n);
   if (rr != LOAD_OK)
      return rr;
   int k = 0;
   for (int i = 0; i < n && k < (int)sizeof g_p.code_str - 1; i++)
      if (b[i] >= '0' && b[i] <= '9')
         g_p.code_str[k++] = b[i];
   /* Only commit when at least one digit was parsed. A non-empty file with no
    * digits (a partial write, or a hand-edit) would otherwise wipe a working
    * code to "" -- every sibling loader preserves its prior value on garbage.
    */
   if (k > 0)
      g_p.code_str[k] = 0;
   return LOAD_OK;
}

/* THE FIVE FILES THIS MODULE OWNS. The shell hands over the data directory
 * and nothing else: a filename belongs with the code that reads and writes
 * it, so renaming one is a local change rather than an edit to the activity's
 * startup. */
