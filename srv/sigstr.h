/* SPDX-License-Identifier: GPL-3.0
 * sigstr.h --- the exact bytes this server takes a request MAC over
 * Copyright 2026 Jakob Kastelic
 *
 * THE SERVER'S HALF OF THE SIGNING RULE, on its own so it can be pinned.
 *
 * It lived inside verify_signature, wrapped around a database lookup and a
 * nonce table -- so the only way to ask "what bytes does this server sign?"
 * was to start a server, pair a phone and send it a request. Nothing asked,
 * and lib/wirevec.h's vector for exactly those bytes was checked against a
 * `snprintf` in the TEST rather than against this. A trailing newline could
 * therefore be added to BOTH implementations at once with every gate green,
 * which is the one failure two independent implementations exist to prevent.
 *
 * It is still the SERVER'S OWN: the app builds the same string from its own
 * source (app/sync.c), and neither is generated from the other. What they
 * share is the vector both are now held to.
 */
#ifndef PANCRA_SIGSTR_H
#define PANCRA_SIGSTR_H

#include <stddef.h>

/* METHOD LF TARGET LF TS LF NONCE LF BODYHASH, with NO trailing newline.
 *
 * `target` is the request target EXACTLY as sent -- percent-encoding, query
 * and all -- because that is what the phone signed. `bodyhash` is the
 * lower-case hex SHA-256 of the body's bytes, of the EMPTY body for a GET.
 *
 * Returns the length written, or 0 if it would not fit (in which case
 * nothing is signed, rather than a truncated string being). */
int sig_signing_string(char *out, size_t cap, const char *method,
                       const char *target, long ts, const char *nonce,
                       const char *bodyhash);

#endif
