// SPDX-License-Identifier: GPL-3.0
// devinfo.c --- the device's model, firmware and maker, as the sensor said
// Copyright 2026 Jakob Kastelic

/* ONE PERSISTED DOMAIN. Five unrelated files -- the device's model and
 * firmware, the alarm thresholds, the display preferences, the pairing code
 * and the remote credentials -- behind one save engine is 1541 lines of
 * module with no subject. They share the engine
 * (app/setpriv.h) and the preferences aggregate; they share nothing else, and
 * a reader after one of them had to read past the other four.
 */
#include "devinfo.h"
#include "loadresult.h" /* the four answers a stored file can give */
#include "log.h"
#include "setpriv.h"
#include "settings.h" /* struct prefs: the aggregate the engine holds */
#include "util.h"
#include <stdio.h>

void set_render_info(struct save_job *j)
{
   static unsigned written;
   int n = snprintf(j->buf, sizeof j->buf, "%s\n%s\n%s\n", g_p.model, g_p.fw,
                    g_p.mfr);
   set_job_stamp(j, g_info_path, &written, n, n > 0 && n < 96);
}

enum load_result info_load(void)
{
   /* ONE EXACT READ: the loop over short reads and EINTR, the EOF
    * probe that tells "exactly full" from "longer than this build holds", and
    * the three answers, all in one place -- see read_file_exact, rather than
    * a single unchecked read whose return is used as the file's length. */
   char b[96];
   int n               = 0;
   enum load_result rr = read_file_exact(g_info_path, b, sizeof b, &n);
   if (rr != LOAD_OK)
      return rr;
   b[n]         = 0;
   char *p      = b;
   char *dst[3] = {g_p.model, g_p.fw, g_p.mfr};
   for (int i = 0; i < 3 && p; i++) {
      char *nl = p;
      while (*nl && *nl != '\n')
         nl++;
      int len = (int)(nl - p);
      if (len > 22)
         len = 22;
      for (int j = 0; j < len; j++)
         dst[i][j] = p[j];
      dst[i][len] = 0;
      p           = *nl ? nl + 1 : 0;
   }
   return LOAD_OK;
}

int info_set(int which, const char *val)
{
   char *dst = 0;
   if (which == SET_DIS_MODEL)
      dst = g_p.model;
   else if (which == SET_DIS_FW)
      dst = g_p.fw;
   else if (which == SET_DIS_MFR)
      dst = g_p.mfr;
   if (!dst || !val)
      return SETTINGS_UNSAVED;
   char clean[24];
   int i = 0;
   for (; val[i] && i < 23 && val[i] >= 0x20; i++)
      clean[i] = val[i];
   clean[i] = 0;
   return set_str_field(dst, 24, clean, set_render_info);
}
