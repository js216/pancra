// SPDX-License-Identifier: GPL-3.0
// devtag.c --- naming a device in the log without naming the device
// Copyright 2026 Jakob Kastelic

/* See devtag.h for why a log may not carry a Bluetooth address at all, not
 * even part of one. This is the whole implementation: a salted hash, four hex
 * characters wide. */
#include "devtag.h"
#include "rand.h"
#include "thread.h"
#include <stdint.h>

/* THE SALT, drawn once and never shown to anybody.
 *
 * Without it this would be a pure function of the address: the same sensor
 * would tag identically in every log on every phone for ever, which is a
 * stable hardware identifier with extra steps -- and one anybody could invert
 * by hashing the few hundred addresses a Dexcom can have.
 *
 * DRAWN LAZILY, under a leaf lock, because the first caller is a BLE callback
 * on the binder thread and the second may be the renderer: two threads racing
 * to initialise it would tag the same device two different ways in one run,
 * which is the one property this has to keep.
 *
 * A FAILED DRAW IS NOT FATAL AND NOT SILENT-ZERO. rand_bytes can fail (no
 * entropy source, a sandbox without /dev/urandom), and a zero salt would make
 * the tags reproducible -- exactly the thing being avoided. The fallback mixes
 * addresses that differ per process and per run: the salt is then weaker than
 * random but still not a constant, and it never becomes a published mapping. */
static struct mutex tag_lk = MUTEX_INIT;
static uint64_t g_salt;
static int g_have_salt;

static uint64_t salt(void)
{
   mutex_lock(&tag_lk);
   if (!g_have_salt) {
      uint8_t b[8];
      if (rand_bytes(b, sizeof b)) {
         for (int i = 0; i < 8; i++)
            g_salt = (g_salt << 8U) | b[i];
      } else {
         /* Two things a reader of the log cannot know: where this process's
          * stack landed and where this function did. */
         uintptr_t a  = (uintptr_t)&g_have_salt;
         uintptr_t b2 = (uintptr_t)&tag_lk;
         g_salt = ((uint64_t)a << 17U) ^ (uint64_t)b2 ^ 0x9E3779B97F4A7C15ULL;
      }
      g_have_salt = 1;
   }
   uint64_t s = g_salt;
   mutex_unlock(&tag_lk);
   return s;
}

const char *devtag(const char *mac, char out[DEVTAG_LEN])
{
   static const char hex[] = "0123456789abcdef";
   if (!mac || !mac[0]) {
      /* NOTHING TO TAG, said as such. An empty field in a log line reads as a
       * missing argument, which sends the reader looking for a bug here. */
      out[0] = '-';
      out[1] = '-';
      out[2] = '-';
      out[3] = '-';
      out[4] = 0;
      return out;
   }
   /* FNV-1a over the salt and then the address. Not a cryptographic hash and
    * it does not need to be: what it must resist is somebody with the LOG
    * recovering the address, and they do not have the salt. */
   uint64_t h = 0xCBF29CE484222325ULL ^ salt();
   for (int i = 0; mac[i]; i++) {
      h ^= (uint64_t)(unsigned char)mac[i];
      h *= 0x100000001B3ULL;
   }
   out[0] = hex[(h >> 12U) & 0xFU];
   out[1] = hex[(h >> 8U) & 0xFU];
   out[2] = hex[(h >> 4U) & 0xFU];
   out[3] = hex[h & 0xFU];
   out[4] = 0;
   return out;
}
