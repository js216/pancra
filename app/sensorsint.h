// SPDX-License-Identifier: GPL-3.0
// sensorsint.h --- the registry's indexed reads, for its own tests
// Copyright 2026 Jakob Kastelic

/* WHY THIS IS NOT IN sensors.h.
 *
 * A position is not an identity. A mint or a forget on a binder thread moves
 * every slot after it, so "how many are there" and "what is at 3" answered by
 * two calls can describe two different devices -- and a caller that acts on
 * the answer renames, recolours, CALIBRATES or disconnects one the user was
 * not looking at. The app therefore asks neither question: it takes a
 * SNAPSHOT (sensors_view_get) or asks by ID (sensor_slot_of, sensor_rec_of,
 * sensor_id_is_live, sensor_slot_at for one device).
 *
 * These exist because a TEST arranges states no public operation can reach --
 * a corrupt row, half an ordered pair -- and must then look at the table
 * directly. Declaring them in the public header makes them an ordinary part
 * of the interface, and an interface nobody may use is one somebody
 * eventually does: `make lockcheck` is the gate, and a separate header is
 * what makes the gate agree with the declaration.
 *
 * INCLUDED BY sensors.c (which defines them) and by the registry's tests.
 * Nothing else may include it; every declaration here takes the registry lock
 * for the length of ONE call and no longer, so a walk built out of them is
 * still a walk over a table that can move between its steps.
 *
 * Deliberately absent: a whole-table copy (sensors_view_get supersedes one)
 * and an id -> position lookup, whose only possible use is to hand a position
 * to something else. */
#ifndef PANCRA_SENSORSINT_H
#define PANCRA_SENSORSINT_H

/* INCLUDE sensors.h FIRST, and this does not do it for you. Including it here
 * would make the registry's own header depend on this one through sensors.c
 * -- a cycle, which `make inclusions` refuses and is right to: a module whose
 * test view is part of its interface cannot be read without it. The types
 * below (struct sensor_slot, struct sensor_rec) are sensors.h's, so the
 * requirement is real; it is stated as an error rather than left to a
 * confusing diagnostic about an incomplete type. */
#ifndef PANCRA_SENSORS_H
#error "app/sensorsint.h: include app/sensors.h first"
#endif



#endif
