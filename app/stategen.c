// SPDX-License-Identifier: GPL-3.0
// stategen.c --- see stategen.h
// Copyright 2026 Jakob Kastelic

#include "stategen.h"
#include "syncjni.h" /* syncjni_state_stamp: the number this publishes */
#include "util.h"
#include "wireint.h" /* PRIwire: the wire's scalars, printed exactly */
#include <stdint.h>
#include <stdio.h>

static char g_path[256];
/* The last value written. Not read back from the file: this is a cache of
 * what the file says, and a mismatch after a failed write is corrected by the
 * next tick that finds a different stamp -- see the write below. */
static int64_t g_last = -1;

int stategen_paths(const char *dir)
{
   g_last = -1; /* a new directory is a new file: publish on the next tick */
   return data_path(g_path, sizeof g_path, dir, "/state.gen") != 0;
}

void stategen_tick(void)
{
   if (!g_path[0])
      return;
   int64_t now = syncjni_state_stamp();
   if (now == g_last)
      return;
   char line[32];
   int n = snprintf(line, sizeof line, "%" PRIwire "\n", now);
   if (n <= 0 || n >= (int)sizeof line)
      return;
   /* REPLACE-BY-RENAME, like every other file this app publishes: a reader
    * that catches this mid-write must see one whole generation or the other,
    * never half of either -- a torn number is a backup that thinks the phone
    * changed when it did not, or worse, that it did not when it did.
    *
    * A FAILED WRITE LEAVES g_last ALONE, so the next tick tries again. The
    * file being behind is the safe direction: a backup then reads the OLD
    * generation twice and would notice any real change through the second
    * read as well, because that read comes from the same stale file only
    * while nothing has changed at all. */
   if (atomic_replace(g_path, line, n) == REPLACE_FAILED)
      return;
   g_last = now;
}
