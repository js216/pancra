// SPDX-License-Identifier: GPL-3.0
// selection.c --- what the user is currently acting on (see selection.h)
// Copyright 2026 Jakob Kastelic
#include "selection.h"
#include "sensors.h" /* SENSOR_STELO: what the ADD flow offers first */

/* MAIN THREAD ONLY, like every other part of a gesture: set by a tap in
 * menu.c or at the end of a pairing, read by the screens and the workflows
 * they drive. */
static int g_sel_id   = -1;
static int g_add_type = SENSOR_STELO;

int sel_device(void)
{
   return g_sel_id;
}

void sel_set_device(int id)
{
   g_sel_id = id;
}

int sel_add_type(void)
{
   return g_add_type;
}

void sel_set_add_type(int type)
{
   g_add_type = type;
}
