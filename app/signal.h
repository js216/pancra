// SPDX-License-Identifier: GPL-3.0
// signal.h --- Freestanding <signal.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <signal.h> shim (see string.h for the rationale). */
#ifndef PANCRA_SIGNAL_H
#define PANCRA_SIGNAL_H

void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);

#define SIG_DFL ((void (*)(int))0)
/* WHAT signal() ANSWERS WHEN IT REFUSES. Bionic and glibc alike
 * define this as (void (*)(int))-1; it is here because a caller that does not
 * check it cannot tell an installed handler from one the kernel would not
 * take -- and this program's crash reporting is exactly that check. */
#define SIG_ERR ((void (*)(int)) - 1)

#endif
