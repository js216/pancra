// SPDX-License-Identifier: GPL-3.0
// signal.h --- Freestanding <signal.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <signal.h> shim (see string.h for the rationale). */
#ifndef PANCRA_SIGNAL_H
#define PANCRA_SIGNAL_H

void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);

#define SIG_DFL ((void (*)(int))0)

#endif
