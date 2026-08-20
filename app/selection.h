// SPDX-License-Identifier: GPL-3.0
// selection.h --- what the user is currently acting on
// Copyright 2026 Jakob Kastelic
/*
 * TWO FACTS, OWNED BY NEITHER OF THE MODULES THAT USE THEM.
 *
 * Which DEVICE the screens are acting on, and which TYPE the ADD SENSOR flow
 * is offering. Both are set by a tap and read by the workflows that carry the
 * tap out -- so they lived in menu.c, where the taps arrive, and device.c,
 * pairing.c, reconcile.c and forms.c all reached back up into the menu to ask
 * what the user had picked. The menu then called down into those same modules
 * to perform the action, and the include graph closed a ring through four
 * files: none of them could be read, tested or linked without the others.
 *
 * It is not menu state. It is the answer to "what is this about", which every
 * tier of the app needs and none of them owns. Two integers, no dependencies,
 * no behaviour: a leaf everything may name.
 *
 * A DEVICE ID, NOT A SLOT INDEX -- an index is a position and a mint or a
 * forget moves it, so a confirmation tap seconds later would act on whichever
 * sensor had slid into that row. See sensors.h.
 */
#ifndef PANCRA_SELECTION_H
#define PANCRA_SELECTION_H

/* The device every per-device screen and action is about; -1 = none. */
int sel_device(void);
void sel_set_device(int id);

/* The sensor TYPE the ADD flow is offering (a SENSOR_* value). It persists
 * after the menu closes, which is deliberate -- the pairing that follows an
 * advertisement has to know what the user asked for -- and is why matching on
 * type alone was once enough to re-mint a registered sensor under the wrong
 * one. Every reader keys on the ADDRESS first; see reconcile.c. */
int sel_add_type(void);
void sel_set_add_type(int type);

#endif
