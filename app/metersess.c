// SPDX-License-Identifier: GPL-3.0
// metersess.c --- The meter's protocol session, behind one lock
// Copyright 2026 Jakob Kastelic

/* See metersess.h for why this is a module rather than five variables. The
 * whole implementation is "take the lock, touch the struct, drop the lock":
 * the value is that the compound decisions -- claim, end, drop -- are inside
 * one critical section rather than being spelled out again at every caller,
 * where one of them will eventually be spelled out differently. */
#include "metersess.h"
#include "thread.h"
#include "util.h" /* str_snapshot: the address is copied, never aliased */

/* The lock. A LEAF: nothing below is called from here, so it cannot be held
 * across the driver, the transport or a file. See metersess.h. */
static struct mutex msess_lk = MUTEX_INIT;

static int g_busy;
static int g_link = -1;
static int g_src;
static long g_start;
static char g_mac[MSESS_MAC_MAX];
/* When link l asked for its teardown, on the MONOTONIC clock; 0 = not
 * waiting. Read as a whole by the watchdog, written by callbacks. */
static long g_idle[MSESS_LINKS_MAX];

static int link_ok(int link)
{
   return link >= 0 && link < MSESS_LINKS_MAX;
}

void msess_get(struct msess *out)
{
   if (!out)
      return;
   mutex_lock(&msess_lk);
   out->busy  = g_busy;
   out->link  = g_link;
   out->src   = g_src;
   out->start = g_start;
   str_snapshot(out->mac, (int)sizeof out->mac, g_mac);
   mutex_unlock(&msess_lk);
}

int msess_claim(int link, int src, const char *mac, long now)
{
   if (!link_ok(link) || src <= 0)
      return 0;
   mutex_lock(&msess_lk);
   /* THE TEST AND THE SET, TOGETHER. Two meters woken by the same person in
    * the same second connect milliseconds apart on separate binder threads;
    * as two statements this let both through and both seeded otble. */
   if (g_busy && g_link != link) {
      mutex_unlock(&msess_lk);
      return 0;
   }
   g_link  = link;
   g_src   = src;
   g_busy  = 1;
   g_start = now;
   str_snapshot(g_mac, (int)sizeof g_mac, mac ? mac : "");
   mutex_unlock(&msess_lk);
   return 1;
}

void msess_begin(int link, int src, long now)
{
   if (!link_ok(link))
      return;
   mutex_lock(&msess_lk);
   g_link  = link;
   g_busy  = 1;
   g_start = now;
   if (src > 0)
      g_src = src;
   mutex_unlock(&msess_lk);
}

int msess_end(int link, long idle_now)
{
   if (!link_ok(link))
      return 0;
   mutex_lock(&msess_lk);
   int owner = (g_link == link);
   if (idle_now)
      g_idle[link] = idle_now;
   if (owner) {
      g_busy = 0;
      g_link = -1;
   }
   mutex_unlock(&msess_lk);
   return owner;
}

int msess_drop(void)
{
   mutex_lock(&msess_lk);
   int link = g_link;
   g_busy   = 0;
   g_link   = -1;
   mutex_unlock(&msess_lk);
   return link;
}

int msess_src(void)
{
   mutex_lock(&msess_lk);
   int src = g_src;
   mutex_unlock(&msess_lk);
   return src;
}

int msess_busy(void)
{
   mutex_lock(&msess_lk);
   int busy = g_busy;
   mutex_unlock(&msess_lk);
   return busy;
}

void msess_bind(int src, const char *mac)
{
   mutex_lock(&msess_lk);
   g_src = src;
   str_snapshot(g_mac, (int)sizeof g_mac, mac ? mac : "");
   mutex_unlock(&msess_lk);
}

void msess_idle_set(int link, long when)
{
   if (!link_ok(link))
      return;
   mutex_lock(&msess_lk);
   g_idle[link] = when;
   mutex_unlock(&msess_lk);
}

void msess_idle_copy(long *out, int n)
{
   if (!out)
      return;
   mutex_lock(&msess_lk);
   for (int i = 0; i < n; i++)
      out[i] = (i < MSESS_LINKS_MAX) ? g_idle[i] : 0;
   mutex_unlock(&msess_lk);
}
