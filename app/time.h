// SPDX-License-Identifier: GPL-3.0
// time.h --- Freestanding <time.h> shim
// Copyright 2026 Jakob Kastelic

/* Minimal freestanding <time.h> shim (see string.h for the rationale). */
#ifndef PANCRA_TIME_H
#define PANCRA_TIME_H

struct timespec {
   long tv_sec, tv_nsec;
};

int clock_gettime(int clk, struct timespec *ts);

#endif
