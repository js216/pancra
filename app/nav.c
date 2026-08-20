// SPDX-License-Identifier: GPL-3.0
// nav.c --- The navigation stack (see nav.h)
// Copyright 2026 Jakob Kastelic

#include "nav.h"
#include "uimodel.h"

static enum ui_screen g_nav[NAV_MAX] = {SCR_MAIN};

static int g_nav_n = 1;
/* The current screen as a plain int at a FIXED address, for the crash handler
 * -- which reads its context through pointers and cannot call anything to
 * derive a value (see crashlog.h). It is a MIRROR, not a second source of
 * truth: nav_depth() below is the only thing that writes it, so it cannot
 * drift from the path. */
int g_screen_now = SCR_MAIN;

/* The one writer. Every change to the path goes through here, which is what
 * keeps the mirror above honest. */
static void nav_depth(int n)
{
   g_nav_n      = n;
   g_screen_now = (int)g_nav[n - 1];
}

/* The screen showing now. SCR_MAIN means no modal is open. */
enum ui_screen cur_screen(void)
{
   return g_nav[g_nav_n - 1];
}

/* Go to `to`. Opens it on top of the current screen, or -- if it is already
 * on the path -- returns to it, discarding everything above. */
void nav_go(enum ui_screen to)
{
   for (int i = 0; i < g_nav_n; i++)
      if (g_nav[i] == to) {
         nav_depth(i + 1);
         return;
      }
   if (g_nav_n < NAV_MAX) {
      g_nav[g_nav_n] = to;
      nav_depth(g_nav_n + 1);
   } else {
      /* Deeper than any real route. Replace the top rather than drop the
       * root: losing the root would strand the user with no way back to the
       * main screen, which is the one failure worse than a wrong back
       * target. */
      g_nav[g_nav_n - 1] = to;
      nav_depth(g_nav_n);
   }
}

/* Close the current screen, returning to whatever opened it. */
void nav_back(void)
{
   if (g_nav_n > 1)
      nav_depth(g_nav_n - 1);
}

/* Abandon the whole path (a flow that ends at the main screen). */
void nav_home(void)
{
   nav_depth(1);
}

/* 1 when `s` is on the path -- the user came THROUGH it to get here.
 *
 * This is what the old code was really asking when it compared an origin
 * global against a screen ("was this reached from DEVICES?"). Asking the path
 * answers it for every route, including ones added later. */
int nav_has(enum ui_screen s)
{
   for (int i = 0; i < g_nav_n; i++)
      if (g_nav[i] == s)
         return 1;
   return 0;
}

/* The path as it stands, root first: at most `cap` entries, and how many were
 * copied. See nav.h for why this is exported at all. */
int nav_path(enum ui_screen *out, int cap)
{
   int n = g_nav_n < cap ? g_nav_n : cap;
   if (!out || cap <= 0)
      return 0;
   for (int i = 0; i < n; i++)
      out[i] = g_nav[i];
   return n;
}

/* Replace the path wholesale. REFUSED, not clamped, when the length is not a
 * path this module could have produced: a zero-length path has no current
 * screen at all, and one longer than NAV_MAX would be read out of the array
 * it is copied into. A refusal leaves the user where they already were, which
 * is the main screen at startup -- the one place every route home starts. */
void nav_set_path(const enum ui_screen *p, int n)
{
   if (!p || n < 1 || n > NAV_MAX)
      return;
   for (int i = 0; i < n; i++)
      g_nav[i] = p[i];
   nav_depth(n);
}

/* A BOOKMARK, for flows that span several screens.
 *
 * The pairing flow is type picker -> keypad -> device list -> confirmation,
 * and cancelling from any of them returns not to the previous screen but to
 * wherever the flow was ENTERED. One `nav_back()` cannot express that and a
 * "go to screen X" cannot either -- X depends on the route in.
 *
 * The depth does. Take a mark when the flow starts, return to it when it
 * ends, and the landing place is right for every route, including ones that
 * skip a step (the ADD menu enters the type flow without the type picker). */
int nav_mark(void)
{
   return g_nav_n;
}

void nav_return_to(int mark)
{
   if (mark >= 1 && mark <= g_nav_n)
      nav_depth(mark);
}
