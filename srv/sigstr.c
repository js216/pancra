/* SPDX-License-Identifier: GPL-3.0
 * sigstr.c --- the signing string, and nothing else
 * Copyright 2026 Jakob Kastelic
 */
#include "sigstr.h"
#include <stdio.h>

int sig_signing_string(char *out, size_t cap, const char *method,
                       const char *target, int64_t ts, const char *nonce,
                       const char *bodyhash)
{
   if (!out || cap == 0)
      return 0;
   int n = snprintf(out, cap, "%s\n%s\n%" PRIwire "\n%s\n%s", method, target,
                    ts, nonce, bodyhash);
   if (n <= 0 || (size_t)n >= cap) {
      out[0] = '\0';
      return 0;
   }
   return n;
}
