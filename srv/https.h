/* SPDX-License-Identifier: GPL-3.0
 * https.h --- the same server, over TLS
 * Copyright 2026 Jakob Kastelic
 *
 * Declared where https.c implements it. It was in http.h, which made the
 * plain-HTTP module's header speak for the TLS one -- and every includer of
 * http.h depend on both.
 */
#ifndef PANCRA_HTTPS_H
#define PANCRA_HTTPS_H

#include "http.h" /* http_handler, struct http_policy: the same contract */

/* http_serve over TLS (srv/https.c, on srv/tls.c).
 * `cert` is the PEM certificate chain, leaf first; `key` its private key. */
int https_serve(int port, const char *cert, const char *key, const char *name,
                http_handler handle, const struct http_policy *pol, void *user);

#endif
