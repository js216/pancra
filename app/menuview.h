// SPDX-License-Identifier: GPL-3.0
// menuview.h --- What a FRAME may read of the menus, and nothing else
// Copyright 2026 Jakob Kastelic
//
/* A READ-ONLY SNAPSHOT, for two reasons -- one structural, one about frames.
 *
 * THE CYCLE. menu.c changes screens and settings, so it says things on the
 * status row and asks for a repaint: it included model.h. model.c assembles
 * the frame, and the frame shows which slot the menus are acting on, which
 * type the ADD flow is offering, the sampled permission states and the EXPORT
 * checkboxes: it included menu.h. Two files, each reaching into the other's
 * header, which is the shape that makes it impossible to say which one is
 * underneath. Now: menu.c talks to the status row through status.h, and model.c
 * reads the menus through this file, which knows nothing about frames.
 *
 * THE FRAME. Those values were eight separate calls and two writable global
 * arrays, read at scattered points while build_model ran -- the same defect
 * the whole snapshot design exists to avoid, in miniature: the selected slot
 * was read eight times in one frame, and a tap arriving between two of them
 * gave one frame two different answers. It is one copy now, taken once.
 *
 * WRITABLE GLOBALS WERE ALSO EXPORTED. `extern int g_sys_perm[]`, `g_exp_glu`
 * and five siblings meant anything that included menu.h could set them, with
 * no chance to sample the system state or persist a choice. They are private
 * to menu.c now; this is how the frame sees them.
 */
#ifndef MENUVIEW_H
#define MENUVIEW_H

/* Runtime permissions requested at once. */
#define NPERMS 3

struct menu_view {
   /* WHICH DEVICE the menus are acting on (its permanent id, -1 = none) and
    * WHICH TYPE the ADD SENSOR flow is offering. Set by a tap, read by the
    * frame.
    *
    * An ID, not a slot index. The index moves whenever a sensor is minted or
    * forgotten, so a confirmation screen that remembered one could disconnect
    * a device the user never picked -- the screen would even name the right
    * one, having drawn it before the shift. */
   int sel_id;
   int add_type;

   /* The system states the settings screen shows, as sampled by sys_refresh.
    * A CACHE on purpose: reading them means JNI through the activity's env,
    * which is legal only on the main thread, and a render can be requested
    * from a BLE binder thread. */
   int perm[NPERMS];
   int batt_ok;
   int standby_bucket; /* -1 = unknown */
   int bg_restricted;

   /* WHICH PAGE the two paginated device lists are showing. Menu state, not
    * frame state: the arrows are taps. */
   int old_page, dev_page;

   /* EXPORT DATA's checkboxes (session-only; the defaults -- everything, all
    * time -- are the whole point). Range 0 = 30 D, 1 = 1 Y, 2 = ALL. */
   int exp_range, exp_glu, exp_dev, exp_ins, exp_wt;
   /* THE LAST EXPORT DID NOT REACH JAVA. Discarding jb_export_data's answer
    * makes an activity that has gone away, or a bridge method that cannot be
    * resolved, look exactly like a successful export: no share sheet, and
    * nothing said. The screen says it
    * now, and the state is retryable -- a later accepted attempt or leaving
    * the screen clears it. */
   int exp_failed;
};

/* One consistent copy. Main thread, like every other part of a frame. */
void menu_view_get(struct menu_view *out);

#endif
