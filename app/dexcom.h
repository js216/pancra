// SPDX-License-Identifier: GPL-3.0
// dexcom.h --- the Dexcom-specific half of sensor authentication (API)
// Copyright 2026 Juggluco and xDrip contributors

/* Specific to Dexcom G7/Stelo transmitters. The generic half of the pairing
 * -- EC-J-PAKE -- is in lib/jpake.h and is shared with the server. */
#ifndef DEXCOM_H
#define DEXCOM_H

#include <stddef.h>
#include <stdint.h>

/* Per-connection auth hash: AES-128-ECB(key16, data8||data8)[:8]. */
int dexcom_dex8(const uint8_t key16[16], const uint8_t data8[8],
                uint8_t out8[8]);

/* Certificate key-challenge: ECDSA-P256 sign SHA256(challenge[2:18]) with the
 * embedded collector device key -> raw r||s (64 bytes). challenge must be at
 * least 18 bytes. Returns 0 if it could not sign, which the driver treats as
 * a failed challenge. */
int dexcom_getchallenge(const uint8_t *challenge, size_t clen,
                        uint8_t out64[64]);

/* The verifying side of the same challenge, against the device public key.
 * The sensor does this in production; here it is what makes the signer
 * checkable against a fixed challenge. */
int dexcom_verify_challenge(const uint8_t *challenge, size_t clen,
                            const uint8_t sig64[64]);

#endif
