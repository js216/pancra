// SPDX-License-Identifier: GPL-3.0
// reading.h --- A decoded reading becomes a stored, attributed datapoint
// Copyright 2026 Jakob Kastelic
//
/* THE PATH A NUMBER TAKES FROM THE RADIO TO THE LOG, and every gate on it.
 *
 * The driver decodes an EGV on a BINDER thread and calls in here. What
 * happens next is the most consequential code in the app, because the log is
 * APPEND-ONLY: a value written under the wrong sensor id, or a sentinel
 * written as if it were glucose, is permanent and is displayed as truth.
 *
 * The gates, each of which exists because it was missing once:
 *
 *   - PLAUSIBILITY. The 12-bit field carries 0..4095 verbatim, and a sentinel
 *     (0 during warm-up, or a sensor-error state) would otherwise become the
 *     headline number, fire a LOW alarm, and be logged.
 *   - ATTRIBUTION. A reading is stamped with the sensor its LINK is bonded
 *     to. With no registered slot for that link the sample is DROPPED rather
 *     than stamped with another sensor's id -- the next one arrives in five
 *     minutes, and a wrong attribution is forever.
 *   - THE BACKFILL. A sensor holds hours of history we may not have. It is
 *     requested once per link per launch, and again for interior gaps, both
 *     throttled per LINK -- a single global flag meant a second sensor never
 *     recovered its pre-launch history at all.
 *
 * Per-link, never per-process: every table here is indexed by link, because
 * two sensors are worn at once and the process-global version of each of
 * these facts was wrong for one of them.
 */
#ifndef READING_H
#define READING_H

/* Which registry id a link's readings are stamped with, or 0 for "not yet
 * identified" -- the same id legacy pre-registry rows carry, so old and new
 * data stay consistent. */
int src_for_link(int link);

/* The FALLBACK id, for a reading whose link resolves to no slot. */
int reading_src(void);
void reading_set_src(int id);

/* This LINK's own model and firmware, learned from its GATT device-information
 * service. Copied out under reading.c's own lock: the writer fills them
 * byte-by-byte from a binder thread, and a torn read is minted into a row
 * that is never rewritten. Process-global model/firmware (settings.h) are
 * shared by every link and are WRONG for provenance. Each of these locks for
 * itself -- there is no variant for a caller holding something. */
void reading_dis(int link, char *model, int mcap, char *fw, int fcap);
void reading_forget_dis(int link);

/* Re-connect any CGM link that has gone quiet. Safe on any thread, and safe
 * with no activity alive -- the service heartbeat drives it, so a stranded
 * link is repaired even after the activity is destroyed.
 *
 * Named pancra_*, like every entry point the BLE side calls, but declared
 * HERE with the link state it acts on: the port header should name the
 * TRANSPORT boundary, not carry every function the transport happens to
 * call. */
void pancra_link_watchdog(void);

/* Re-read every log from disk. Only the RESTORE path needs it: that writes
 * the log files directly, so nothing in memory knows the rows arrived. */
void pancra_logs_reload(void);

/* WHAT THE RADIO REPORTS ABOUT A CGM, and where it lands. These four are the
 * transport's calls INTO the app, and they are declared here, beside the
 * gates they must pass, because reading.c IMPLEMENTS them. The alternative --
 * a shared app<->transport header naming both directions -- made every
 * workflow that wanted to close a link also read the prototype of every hook
 * the radio can fire.
 *
 * All four arrive on BINDER threads. */
/* THESE TWO ANSWER: DID THE APP KEEP IT?
 *
 * 1 when the reading reached the authoritative history -- past ingest.c's
 * value and age gate, past source attribution, and INSERTED (not deduplicated
 * away) by store_record -- and 0 on every refusal. Nothing in this file needs
 * the answer; the CGM driver does. It marked a sensor as streaming, cleared
 * the failure streak that notices a sensor going bad, and persisted the
 * sensor's address as the one to reconnect to, on the strength of a frame
 * having DECODED -- while every refusal below left the screen unchanged and
 * the plot empty. See drv_glucose in dexdriver.h. */
/* One live EGV: the current reading on `link`, `age_s` seconds old. */
int pancra_glucose(int link, int mg_dl, int trend, int age_s);
/* One BACKFILLED EGV -- the same shape, but historic, so it never becomes
 * the headline number and never rings an alarm. */
int pancra_backfill(int link, int mg_dl, int trend, int age_s);
void pancra_rssi(int link, int rssi);
/* One Device Information Service string (model, firmware, manufacturer),
 * identified by its characteristic `uuid`. */
void pancra_devinfo(int link, const char *uuid, const char *val);

/* PER-LINK SIGNAL STRENGTH, last known. The measurement arrives here; the
 * frame reads it. 1 when this link has ever reported one. */
void reading_note_rssi(int link, int dbm, long when);
int reading_link_rssi(int link, int *dbm, long *when);

#endif
