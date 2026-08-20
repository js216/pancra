// SPDX-License-Identifier: GPL-3.0
// aes.h --- Portable AES-128 single-block encrypt (API)
// Copyright 2026 Jakob Kastelic

/* Portable AES-128 single-block encrypt.
 *
 * TABLE-DRIVEN, THEREFORE NOT CONSTANT-TIME. THIS IS THE ONE THING TO KNOW.
 *
 * aes.c is the textbook implementation: a 256-byte S-box indexed by a state
 * byte, sixteen times a round, ten rounds a block, plus sixteen more lookups
 * per block for the key schedule. Every one of those indices is derived from
 * the key. The values do not leak, but which cache lines were touched does,
 * and that is the whole of the Bernstein / Osvik-Shamir-Tromer family of
 * attacks: an observer who can see the cache-line access pattern for enough
 * blocks recovers the key, with no fault, no oracle and no protocol flaw.
 *
 * Note what does and does not follow. A cache attack needs the observer to
 * share a cache with this code -- co-resident code on the same core, another
 * container on the same host, another process on the same board. It is not a
 * network attack. lib/p256.c's problem is the opposite shape: there the leak is
 * timing, which does travel over a network. So the two are not the same risk
 * even though they are both "not constant-time".
 *
 * WHO CALLS THIS, AND WHAT EACH ONE IS EXPOSED TO:
 *
 *   srv/tls.c, via lib/gcm.c, for TLS_AES_128_GCM_SHA256 record protection.
 *   The keys are per-connection traffic secrets from the handshake, the
 *   attacker chooses much of the plaintext, and the number of blocks is
 *   unbounded -- which is everything a cache attack wants EXCEPT the shared
 *   cache. On the deployment this repo actually ships to (srv/deploy: a
 *   dedicated Milk-V Duo running this one binary, reached through a front
 *   door) there is no second tenant to be co-resident with, and an attacker
 *   who can already run code on that board does not need the AES key. On a
 *   shared VPS the same binary would be genuinely exposed, and that is a
 *   deployment decision this header cannot make.
 *
 *   app/dexcom.c dexcom_dex8, for the Dexcom per-connection auth hash over
 *   BLE. The observer would have to be malicious code on the same phone
 *   defeating the Android sandbox to watch this process's cache, at which
 *   point the sensor key is the least of the problems. This is the threat
 *   model the file was written for and it still holds.
 *
 * WHAT WOULD ACTUALLY FIX IT, and what each costs here:
 *
 *   - A hardware backend. aarch64 has the ARMv8 AES extension and the phone
 *     has it, so the app side could be made both constant-time and much
 *     faster. The riscv64 board this server runs on (C906) has no AES
 *     instructions at all, so the side that needs it most cannot have it.
 *     Two backends plus a runtime feature check plus a portable fallback is
 *     three implementations where there is now one.
 *   - A bitsliced or masked software backend. Constant-time by construction
 *     and portable, but it is a real cryptographic implementation, and a
 *     hand-rolled one that is subtly wrong is worse than this file: it would
 *     claim a property nothing in this repo can test for. No test here can
 *     detect a timing leak, which is exactly why the claim has to come from
 *     code that has been reviewed by people who do that for a living.
 *   - Vendoring a vetted implementation. The correct answer for the TLS side,
 *     and a policy question: this repo vendors nothing but sqlite.
 *
 * So the position, stated rather than implied: the app-side use is fine, the
 * TLS use is acceptable only because the server runs alone on its own board,
 * and closing it properly means adopting an outside implementation. Until that
 * decision is made, do not deploy this server anywhere it shares a CPU with
 * code you do not control. See TODO item 57. */
#ifndef DEX_AES_H
#define DEX_AES_H
#include <stdint.h>
void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16]);
#endif
