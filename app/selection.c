// SPDX-License-Identifier: GPL-3.0
// selection.c --- what the user is currently acting on (see selection.h)
// Copyright 2026 Jakob Kastelic
#include "selection.h"
#include "sensors.h" /* SENSOR_STELO: what the ADD flow offers first */
#include <stdatomic.h>

/* MAIN THREAD ONLY, like every other part of a gesture: set by a tap in
 * menu.c or at the end of a pairing, read by the screens and the workflows
 * they drive. */
static int g_sel_id = -1;
/* THE ADD TYPE IS ALSO READ BY THE ADVERT THREAD (which family a pairing
 * candidate must be), so it is atomic; it is written on the main thread. */
static _Atomic int g_add_type = SENSOR_STELO;

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
   return atomic_load_explicit(&g_add_type, memory_order_relaxed);
}

void sel_set_add_type(int type)
{
   atomic_store_explicit(&g_add_type, type, memory_order_relaxed);
}
