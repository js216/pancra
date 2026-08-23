// SPDX-License-Identifier: GPL-3.0
// meterstore.h --- What the meter runtime remembers about each meter
// Copyright 2026 Jakob Kastelic
//
/* TWO FILES, ONE PER FACT, AND BOTH KEYED BY REGISTRY ID.
 *
 * THE RECORD INDEX is the one that matters. The OneTouch protocol can only
 * be asked for "records after N", so N has to survive a restart -- without it
 * every reconnect re-reads the meter's whole memory and duplicates every
 * fingerstick in the log. And it must be PER METER: one shared index meant a
 * second meter's walk position overwrote the first's, so one meter re-read
 * everything and the other skipped records it had never seen. That skip was
 * then persisted, which made the loss permanent.
 *
 * THE LAST-SYNC TIME is what the device row shows and what the re-arm
 * throttle reads. Also per meter -- a global one made one meter's sync
 * throttle the other, and showed one meter's signal against both.
 *
 * The rest of the per-meter runtime (signal, phase text) is in memory only
 * and resets each launch, which is right: it describes a connection, not a
 * device.
 */
#ifndef METERSTORE_H
#define METERSTORE_H
#include "compiler.h"   /* PANCRA_MUST_USE: an answer no caller may drop */
#include "loadresult.h" /* what a load actually found */

/* One meter's runtime record. In memory except sync_t, which is persisted. */
struct meter_rt {
   int id;
   long sync_t; /* last connect/sync of THIS meter (0 = never this launch) */
   /* When the last protocol exchange FINISHED. Distinct from sync_t, which
    * also moves on a bare advertisement: this one means "we actually talked
    * to it and finished", and it is what the re-arm cooldown reads so a meter
    * that is still awake is not immediately synced again. */
   long synced_t; /* MONOTONIC (mono_s): a cooldown, never persisted */
   /* When this meter last ADVERTISED, on the monotonic clock. Distinct from
    * sync_t, which is the same event on the wall clock: sync_t is what the
    * user reads ("LAST SEEN 3 min ago") and what is persisted, while this one
    * is what the once-a-minute advert throttle measures. A throttle on the
    * wall clock is a throttle a time correction can cancel or extend -- a
    * forward jump lets every advert through, a backward one silences the
    * meter until the clock catches up. Never persisted: an interval that
    * spans a restart means nothing. */
   long advert_mono;
   int rssi, rssi_ok;
   long rssi_t;
   char stat[24]; /* last driver phase text (HELLO/COUNT/READING/SYNCED/...) */
   /* WHAT THE IMPORT COULD NOT DECIDE. A fingerstick taken in the repeated
    * hour of a fall-back names two instants an hour apart, and the meter's
    * record carries nothing that distinguishes them (see meterlogic.h). When
    * the walk's own record order does not settle it either, the reading is
    * still stored -- refusing it would lose a fingerstick outright -- but the
    * fact that it was a GUESS is kept here rather than thrown away, together
    * with the instant that was not chosen, so the stamp can be repaired
    * rather than merely doubted.
    *
    * Per meter and per import: cleared when a walk begins, so the count
    * describes the last import rather than accumulating for the life of the
    * process. In memory only, like `stat` -- it describes an exchange. */
   int amb_n;    /* ambiguous records in the last import; 0 = none */
   long amb_alt; /* the rejected instant of the last such record */
};

/* ---- THE RUNTIME TABLE IS THIS FILE'S, AND SO IS ITS LOCK ---------------
 *
 * It does NOT hand out `struct meter_rt *` into a file-scope array. Binder
 * callbacks write a meter's RSSI, its phase text and its timestamps while the
 * watchdog reads them to decide whether to re-arm and the renderer reads them
 * to draw a row -- through pointers, such reads are neither locked nor even
 * copies. `stat` is a 24-byte string: a device row would be drawn from one
 * half of a phase text and the other half of the next.
 *
 * So the pointer does not leave: every read takes a COPY and every write is a
 * named operation. Each locks for itself.
 *
 * THREE LOCKS, and they protect different things:
 *   mrt_lk   the runtime table below, and a LEAF. Taken from binder callbacks
 *            that already hold the driver's, so the order is driver -> mrt_lk
 *            and never the reverse. The save builds its text under it and
 *            writes the FILE outside it: it is a spinning mutex, and the main
 *            thread waits on it while holding the history lock, so holding it
 *            across an fsync is the shape this app has been killed for.
 *   msync_lk the last-sync FILE, held from the render right through the
 *            rename so that two saves cannot interleave -- and, far more
 *            importantly, so that a save cannot render the table, wait, and
 *            then write a state that is older than the one a concurrent
 *            caller has already put in the table. As a single flight entered
 *            AFTER the render, a caller that loses it is told its meter's
 *            last-sync time was persisted when no buffer anywhere contained
 *            it. Taken OUTSIDE mrt_lk, like
 *            calfile_lk around cal_lk; no reader ever takes it.
 *   idx_lk   the record-index file, which is a read-modify-write and was
 *            serialised only by the accident that its one writer runs under
 *            the driver's lock.
 * No path takes idx_lk with either of the other two held. `make lockcheck`
 * runs test/app/lockorder.py, which knows all three -- a leaf claim no tool
 * checks is a comment.
 *
 * NOT a claim that this file calls nothing: meter_index_save asks the
 * registry which ids are still live (sensor_id_is_live), inside idx_lk. That
 * pair is in the checker's output, which is where a reader should look
 * rather than trusting this paragraph.
 *
 * 1 when the meter has a record and `out` was filled, 0 when it has none.
 * `out` may be NULL to ask only whether it exists. */
int meter_rt_read(int id, struct meter_rt *out);

/* THE WRITES, one per thing that happens. Each creates the record if the
 * meter has none, and answers 1 when it landed (0 = the table is full).
 *
 * The times are the caller's, so the clock that belongs to each fact stays
 * the caller's business: `sync_t` and `rssi_t` are instants a person reads
 * (realtime_s), while `synced_mono` and `advert_mono` are intervals a
 * throttle measures (mono_s). See the field comments above -- getting those
 * two the wrong way round is what a wall-clock correction then cancels or
 * extends. */

/* An advertisement: seen on air, with the signal it carried. `rssi_ok` 0
 * leaves the stored signal alone. */
int meter_rt_advert(int id, long sync_t, long advert_mono, int rssi,
                    int rssi_ok, long rssi_t);

/* THE SAME, THROTTLED, AS ONE STEP. 1 when this advert TOOK THE TURN (the
 * last one was more than `window` seconds ago, on the monotonic clock) and
 * the record was updated; 0 when it was too soon, or the table is full.
 *
 * One operation because the caller's version was a check and an act with a
 * gap: read the last advert time, decide, then record. Two scan callbacks for
 * one meter -- which is what a meter waking up produces -- could both read
 * the same stamp and both decide to wake it, issuing two connects during
 * the
 * one second it is listening. */
int meter_rt_advert_turn(int id, long sync_t, long advert_mono, int rssi,
                         int rssi_ok, long rssi_t, long window);
/* A connection's signal strength, which is also proof of a sync. */
int meter_rt_rssi(int id, int rssi, long rssi_t, long sync_t);
/* The driver's live phase text, which is also proof of a sync. */
int meter_rt_stat(int id, const char *stat, long sync_t);
/* A protocol exchange FINISHED (the re-arm cooldown reads this). */
int meter_rt_done(int id, long synced_mono);
/* A record whose instant the import could not determine. `alt` is the
 * instant that was NOT chosen. Counts up within one import. */
int meter_rt_ambiguous(int id, long alt);
/* A walk is starting: forget the previous import's ambiguities, so the count
 * always describes the import a reader is looking at. */
int meter_rt_amb_clear(int id);

/* Where the two files live. Call once, at startup, before loading. */
void meter_store_paths(const char *index_path, const char *sync_path);

/* The last-sync times: persist all of them, or restore them.
 *
 * meter_sync_save() answers about THE FILE and nothing else: 0 means these
 * bytes are on disk. It is not the same question as whether the live
 * observation was accepted -- that is what meter_rt_rssi and its siblings
 * answer -- and a caller that treats one as the other is claiming a restart
 * will remember something it will not. */
PANCRA_MUST_USE int meter_sync_save(void);

/* ---- THE WRITE THAT DID NOT LAND ----------------------------
 *
 * Every meter observation the file carries -- when it was last seen, and the
 * signal at that instant -- is written by a BLE callback that has no way to
 * try again: a scan result is delivered once, and the meter it came from may
 * not advertise again for hours. So a refused write would be the end of that
 * observation, silently, with the screen still showing it.
 *
 * The table therefore tracks a generation: it knows which of its own state
 * the file is known to hold, and a tick retries until the two agree.
 *
 *   SYNC_CLEAN        the file holds everything the table has.
 *   SYNC_DEFERRED     something is owed and this call did not attempt it --
 *                     throttled, or another writer holds the file (whose
 *                     write covers the same rows).
 *   SYNC_STILL_DIRTY  it was attempted and the file system refused again.
 *
 * `now` is a MONOTONIC second (the throttle measures an interval; a wall
 * clock correction must not postpone a retry by an hour). */
enum sync_retry { SYNC_CLEAN, SYNC_DEFERRED, SYNC_STILL_DIRTY };
PANCRA_MUST_USE enum sync_retry meter_sync_retry(long now);

/* Does the table hold anything the file is not known to? For the retry, for
 * the tests, and for anything that wants to say so on the screen. */
PANCRA_MUST_USE int meter_sync_dirty(void);
enum load_result meter_sync_load(void);

int meter_index_save(int id, int idx);

/* ---- THIS METER'S STORED INDEX, AND WHY THE ANSWER IS TYPED --
 *
 * The index is how far through a meter's records the app has already walked.
 * Getting it wrong in the "I know nothing" direction is not free: the walk
 * starts from the beginning, re-reads a window of records that were imported
 * weeks ago, and re-offers every one of them. They dedup on an exact
 * timestamp, so the history survives -- but the sync is long, it holds the
 * meter awake, and any record whose timestamp shifted (a meter whose clock
 * was set) lands twice.
 *
 * As one int, -1 means BOTH "this meter has no stored index" and "the file
 * could not be read". Those want opposite actions: the
 * first is a first sync and walking from the beginning is exactly right; the
 * second is a file that exists and did not answer, where walking from the
 * beginning is a guess with a cost.
 *
 *   LOAD_ABSENT   no index file at all -- a first run. *out is -1.
 *   LOAD_OK       the file was read. *out is this meter's index, or -1 if
 *                 the file does not mention it (which for a file that IS
 *                 there is still "walk from the beginning", but knowingly).
 *   LOAD_CORRUPT  the file was read and some of it is not this format. Rows
 *                 that DID parse are still answered from -- losing every
 *                 meter's progress over one bad row is the harm, not the
 *                 fix -- and the caller is told the file is incomplete.
 *   LOAD_ERROR    it could not be read. *out is -1 and the caller must NOT
 *                 treat that as "nothing stored".
 */
enum load_result meter_index_load(int id, int *out);


#ifdef APP_FAULTS
/* HELD OPEN ON DEMAND, twice inside meter_sync_save: once between the two
 * halves of a row, and once between the finished render and the write.
 *
 * Both windows are a handful of instructions wide on real hardware, and both
 * are what the two locks in this file exist for -- so a build with either
 * lock removed wrote a perfectly coherent, perfectly ordered file on every
 * run of the concurrency suite, and the assertions passed against the very
 * implementations they were written to reject. A race whose window is four
 * instructions wide is not tested by running two threads hard and hoping.
 *
 * A pointer rather than a bare yield because the same loop is reached from a
 * second concurrency section, where yielding under mrt_lk starves the writers
 * it is meant to let in. The suite installs it around the section that needs
 * it and takes it away again. app_fault_gap_here in util.h is the same
 * device. Test builds only; nothing that ships defines APP_FAULTS. */
extern void (*meter_fault_gap_here)(void);
#endif

#endif
