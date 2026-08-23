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
 *   - DEDUPLICATION, which store_record owns: a value the history already
 *     holds is not a second reading, and every path here reports "kept" only
 *     when a row was actually added.
 *
 * WHAT IS DELIBERATELY NOT HERE: the reconnect watchdog, the backfill
 * scans, the signal-strength tuples, the device-information routing and the
 * post-restore reload -- five things that dial the radio or re-read files and
 * none of which decide
 * whether a number becomes a permanent record. They are linkhealth.h,
 * linkinfo.h and logsload.h. What is left is the gate, and it is short enough
 * to read in one sitting, which is the only review a decision this permanent
 * ever really gets.
 */
#ifndef READING_H
#define READING_H

/* THE TWO GLUCOSE ARRIVALS, and where they land. They are the transport's
 * calls INTO the app, and they are declared here, beside the gates they must
 * pass, because reading.c IMPLEMENTS them. The alternative -- a shared
 * app<->transport header naming both directions -- made every workflow that
 * wanted to close a link also read the prototype of every hook the radio can
 * fire.
 *
 * The other two things the radio reports about a link -- its signal strength
 * and its device-information strings -- are in linkinfo.h now, and its
 * liveness is in linkhealth.h. Neither is a reading, and neither passes any
 * of the gates above.
 *
 * Both arrive on BINDER threads. */
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

#endif
