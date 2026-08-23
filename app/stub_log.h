// SPDX-License-Identifier: GPL-3.0
// stub_log.h --- liblog.so stub declarations
// Copyright 2026 Jakob Kastelic

/* Declaration for the symbol defined in stub_log.c. This is the ABI the stub
 * liblog.so exports; the phone's real liblog binds it at runtime.
 *
 * IT IS THE SAME DECLARATION app/log.h MAKES, and it is included
 * from there rather than written out again: this file exists to say what the
 * stub .so exports, log.h to say what production calls, and a stub whose
 * prototype has drifted from the caller's is a link that succeeds and an
 * argument list that does not match. One declaration, two readers. */
#ifndef STUB_LOG_H
#define STUB_LOG_H

#include "log.h"

#endif
