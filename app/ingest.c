// SPDX-License-Identifier: GPL-3.0
// ingest.c --- is this frame a usable live glucose reading?
// Copyright 2026 Jakob Kastelic

/* See ingest.h. Two checks and a subtraction; the value is that they are
 * reachable by a test. */
#include "ingest.h"

struct ingest_out ingest_decide(int mg_dl, int age_s, long now)
{
   struct ingest_out o;
   o.t = 0;
   if (mg_dl < INGEST_GLU_MIN || mg_dl > INGEST_GLU_MAX) {
      o.verdict = INGEST_IMPLAUSIBLE;
      return o;
   }
   /* age_s < 0 is as wrong as age_s too large, and it arrives as a signed int
    * from a caller that widened a uint16 -- so a negative here means the frame
    * was mangled, not that the reading is from the future. */
   if (age_s < 0 || age_s > INGEST_AGE_MAX) {
      o.verdict = INGEST_STALE;
      return o;
   }
   o.verdict = INGEST_OK;
   o.t       = now - age_s;
   return o;
}
