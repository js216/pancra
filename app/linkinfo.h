// SPDX-License-Identifier: GPL-3.0
// linkinfo.h --- what the app knows about ONE radio link
// Copyright 2026 Jakob Kastelic
//
/* PER-LINK TELEMETRY AND IDENTITY, split out of reading.c.
 *
 * Two CGMs are worn at once. Every fact in here was once a process-global,
 * and every one of them was WRONG for one of the two sensors: the signal
 * strength shown on a device row was whichever radio reported last, and the
 * model and firmware minted into a new sensor's permanent record were the
 * FIRST sensor's -- into an append-only provenance file that is never
 * rewritten. So the rule this module exists to hold is: indexed by link,
 * never by process.
 *
 * WHAT IS HERE: the signal-strength tuple, the per-link device-information
 * strings, the routing of an arriving DIS string to whoever owns it, and the
 * attribution question -- which registered sensor does this link's data
 * belong to. WHAT IS NOT: whether a reading is plausible, and whether a link
 * is still alive. Those are reading.c and linkhealth.c.
 *
 * TWO LEAF LOCKS, and they are leaves on purpose. Each is taken by the
 * functions below and by nothing else, and nothing is called while either is
 * held -- so neither can join the driver -> registry -> history order that
 * two phone freezes came out of (app/thread.h). There is no "_locked" variant
 * of anything here: a caller that wanted one would be a caller holding
 * another module's lock across these, which is the shape being avoided.
 */
#ifndef LINKINFO_H
#define LINKINFO_H

/* Which registry id a link's readings are stamped with, or 0 for "not yet
 * identified" -- the same id legacy pre-registry rows carry, so old and new
 * data stay consistent. -1 when the link maps to no registered slot, so the
 * caller can refuse to log rather than invent a provenance. */
int src_for_link(int link);

/* PER-LINK SIGNAL STRENGTH, last known. The measurement arrives here; the
 * frame reads it. 1 when this link has ever reported one. */
void linkinfo_note_rssi(int link, int dbm, long when);
int linkinfo_rssi(int link, int *dbm, long *when);

/* The LIVE connection's signal strength, as one number for the main screen's
 * single readout, and stamped onto a stored row when the driver says the
 * measurement belongs to the connection the reading arrived on. */
int linkinfo_conn_rssi(void);

/* This LINK's own model and firmware, learned from its GATT device-information
 * service. Copied out under this module's lock: the writer fills them
 * byte-by-byte from a binder thread, and a torn read is minted into a row
 * that is never rewritten. Process-global model/firmware (settings.h) are
 * shared by every link and are WRONG for provenance. */
void linkinfo_dis(int link, char *model, int mcap, char *fw, int fcap);
void linkinfo_forget_dis(int link);

/* Ask this link for the device-information strings again, if it is still
 * missing any -- throttled per link, on the monotonic clock. `have_mfr` is
 * whether the process-global manufacturer string is already known, which is
 * the one field of the three that is not per link.
 *
 * Called when a reading arrives, because that is the proof the link is up and
 * answering; see reading.c. */
void linkinfo_refresh_dis(int link, int have_mfr);

/* WHAT THE RADIO REPORTS ABOUT A LINK. Both arrive on BINDER threads. */
void pancra_rssi(int link, int rssi);
/* One Device Information Service string (model, firmware, manufacturer),
 * identified by its characteristic `uuid`. */
void pancra_devinfo(int link, const char *uuid, const char *val);

#endif
