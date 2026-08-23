// SPDX-License-Identifier: GPL-3.0
// linkhealth.h --- is this link alive, and is its history whole
// Copyright 2026 Jakob Kastelic
//
/* LIVENESS, split out of reading.c.
 *
 * Two questions, both about a LINK rather than about a reading, and both
 * answered by asking the radio for something:
 *
 *   IS IT STILL THERE. A CGM that has gone quiet is reconnected. This is the
 *   ONLY reconnect mechanism while the screen is off -- the advert path needs
 *   a scan, whose lifecycle follows on_resume/on_pause -- so it runs from the
 *   service tick as well as the activity timer.
 *
 *   IS ITS HISTORY WHOLE. A sensor buffers hours of records we may not have:
 *   once per link per launch reaching back over the available window, and
 *   again for any interior hole, retried until it is filled. Both throttled
 *   per link, because a single global flag meant the SECOND sensor never
 *   recovered its pre-launch history at all.
 *
 * WHY IT IS NOT IN reading.c. That file decides whether one number becomes a
 * permanent record, which is the most consequential judgement in the app;
 * these two dial the radio and change nothing. Sharing a file made every
 * change to the throttles read like a change to the record.
 *
 * Everything here is per link and monotonic where it measures an interval: a
 * backward NTP correction made `now - stamp` negative, and the watchdog then
 * refused every reconnect for the whole hour wall time took to catch up --
 * the one failure it exists to prevent.
 */
#ifndef LINKHEALTH_H
#define LINKHEALTH_H

/* Re-connect any CGM link that has gone quiet. Safe on any thread, and safe
 * with no activity alive -- the service heartbeat drives it, so a stranded
 * link is repaired even after the activity is destroyed.
 *
 * Named pancra_*, like every entry point the BLE side calls. */
void pancra_link_watchdog(void);

/* A reading just arrived on `link` from sensor `src`, stamped `t`, and `kept`
 * says whether it reached the record. That is the proof the link is up and
 * answering, so it is the moment to ask it to fill in whatever this sensor
 * holds and the app does not. Does nothing at all when nothing is missing. */
void linkhealth_after_reading(int link, int src, long t, int kept);

#endif
