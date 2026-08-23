// SPDX-License-Identifier: GPL-3.0
// devtag.h --- naming a device in the log without naming the device
// Copyright 2026 Jakob Kastelic

/* WHY A LOG MAY NOT CARRY A BLUETOOTH ADDRESS.
 *
 * logcat is not this app's private diary. Any app holding READ_LOGS, anyone
 * with the phone and adb, and every bug report the platform collects can read
 * it -- and a sensor's MAC is a stable hardware identifier for a medical
 * device on a person's arm. It says a Dexcom is present, which one it is, and
 * -- across two logs from two places -- that they were the same person.
 *
 * A PARTIAL ONE IS NOT ENOUGH. Logging the last five characters ("bond:
 * ..:B0:B4") reads as anonymised and is not: two
 * bytes of a MAC single out one device among the handful anybody owns, and
 * they are as stable as the whole address. That is why this exists rather than
 * a shorter substring.
 *
 * WHAT IS LOGGED INSTEAD. A per-RUN tag: four hex characters derived from the
 * address AND a random salt drawn once when the process starts. Inside one
 * log it behaves exactly like an address -- the same device is the same four
 * characters on every line, so a session can be followed -- and outside it, it
 * is nothing. The salt never leaves memory, so the mapping cannot be inverted
 * or replayed against another log, and the same sensor gets a different tag
 * after every restart.
 *
 * WHAT IS LOST, honestly: you cannot tell from the log alone WHICH physical
 * sensor a run was talking to, and two logs from two runs cannot be joined by
 * device. Both of those are the point. Where an enduring name is genuinely
 * needed the registry ID is the thing to log -- it is ours, it means something
 * to this app, and it identifies no hardware.
 *
 * COLLISIONS are possible and do not matter: sixteen bits over the two or
 * three devices a person owns, and the cost of one is two lines that look
 * related and are not, in a diagnostic log. Widening it would trade that for a
 * longer identifier, which is the thing being avoided. */
#ifndef PANCRA_DEVTAG_H
#define PANCRA_DEVTAG_H

/* Four hex characters for this address, in a caller-owned buffer of at least
 * DEVTAG_LEN bytes. Returns `out`, so it can be used inline in a log call.
 *
 * THE BUFFER IS THE CALLER'S because two of these can appear in one log line
 * and because the BLE callbacks that use it run on the binder thread while the
 * renderer logs from its own -- a shared static would hand one thread the
 * other's answer, which in a diagnostic is worse than no answer.
 *
 * A null or empty address answers "----": nothing to tag, said out loud rather
 * than as an empty field that reads like a missing argument. */
#define DEVTAG_LEN 5
const char *devtag(const char *mac, char out[DEVTAG_LEN]);

#endif
