// SPDX-License-Identifier: GPL-3.0
// util.h --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_UTIL_H
#define PANCRA_UTIL_H

/* THE CLOCKS MOVED TO clock.h. They were here, in a header otherwise about
 * files, logs and strings -- which was harmless until thread.h needed a
 * millisecond clock and this module needed a mutex, and the two lowest
 * primitives in the app started including each other. */

int clampn(int n,
           int cap); /* clamp a snprintf length to [0, cap-1] for write() */
/* Copy src into a dst of `cap` bytes, always NUL-terminating. Used wherever a
 * borrowed or stack-local string has to outlive its owner. */
void str_snapshot(char *dst, int cap, const char *src);

/* Build "<dir><name>" into a bounded buffer. Every module that persists
 * something builds its OWN paths with this, from the data directory the shell
 * hands it -- so a file's name lives with the code that reads and writes it,
 * not in a list the shell has to keep in step.
 *
 * 1 when the whole path fitted, 0 when it did not -- and then `dst` is EMPTY,
 * never a truncation. A truncated path is not a failure a caller notices: it
 * is a different directory that happens to exist, or that two long names
 * collide in. See the definition. */
int data_path(char *dst, int cap, const char *dir, const char *name);
/* Replace a small snapshot atomically. The old file remains intact on every
 * failure. Returns 0 only after the replacement has reached the filesystem. */
/* All `len` bytes or -1. A short write is not a failure of write(2), it is
 * the contract -- and a caller that takes it for success writes a truncated
 * record. */
int write_all(int fd, const void *data, int len);

/* A synced record CHANGED (a reading, a dose, a weight, a sensor row, a slot).
 * Call after the write commits. The sync scheduler folds this into its
 * "anything new?" stamp, which file sizes alone cannot answer: an edit that
 * keeps the row the same length leaves every size untouched. */
/* THREAD-SAFE: a reading commits on a binder thread while a dose commits on
 * the main thread and the sync worker reads the count. */
void record_mutated(void);
long record_generation(void);

/* Undo a partial write of `by` bytes at the end of `fd`. 0 when the file is
 * clean again, -1 when it could NOT be rolled back -- which is not the same
 * as "the append failed": a half row is still there, and the next append will
 * splice onto it. Callers must report the difference. */
int rollback_tail(int fd, long by);

/* WHAT AN APPEND CAN ANSWER. The third one is the point: a partial row left
 * in the file is not the same as "nothing happened", and the caller must not
 * report it as a plain failure -- the file needs saying so, and a retry must
 * not assume it is appending to a clean end. */
#define LOG_OK      0
#define LOG_FAIL    (-1)
#define LOG_DAMAGED (-2)

/* Append one record to an append-only log, creating it (header and first row
 * together, atomically) when it does not exist yet. See util.c.
 *
 * SERIALISED against every other log_append in the process: the take-back
 * after a failed flush truncates by length from the current end, which is
 * only this writer's row while no other writer is inside. */
int log_append(const char *path, const void *hdr, int hdrlen, const void *row,
               int rowlen);

#ifdef APP_FAULTS
/* PER-THREAD fsync failure, for the concurrency case only (see util.c). The
 * environment switches are process-wide, and the race that matters needs ONE
 * writer to fail while another SUCCEEDS -- which a process-wide switch cannot
 * express. Test builds only; nothing that ships defines APP_FAULTS. */
extern _Thread_local int app_fault_fsync_here;
/* Run at the instant the take-back window opens -- after a failed flush,
 * before the row is truncated away. A race whose window is two syscalls wide
 * is not tested by running both writers hard and hoping. */
extern _Thread_local void (*app_fault_gap_here)(void);
#endif

/* Finish an append durably: flush the data, close (checked), and flush the
 * directory entry when this append CREATED the file. `fd` is consumed. 0 on
 * success. Without it a logged record lives only in the page cache, and a
 * phone that loses power comes back without it -- having said it was saved. */
int append_finish(int fd, const char *path, int created);

/* ---- WHAT A REWRITE ACTUALLY DID ------------------------------------
 *
 * THREE OUTCOMES, because the rename is the point of no return.
 *
 * Everything before the rename can be undone: the temporary is removed and
 * the original file is exactly as it was, so a caller is right to put its
 * memory back and report that nothing happened.
 *
 * The DIRECTORY FSYNC comes after. If it fails, the new pathname is already
 * visible -- the file HAS been replaced -- and only its durability across a
 * power cut is unknown. Reported as plain failure (which is what returning
 * fsync_dir_of's result did), every caller rolled its memory back to the old
 * value while the disk held the new one. The two then disagree, and the
 * disagreement is invisible until the next launch reads the file and the
 * "reverted" value comes back.
 *
 * So the caller is told which of the two happened, and REPLACE_UNSYNCED
 * means: keep the new value, it is what the file says; the only thing in
 * doubt is whether a power cut in the next moments would lose it. Callers
 * with a retry (the calibration tick, the settings generation) re-write
 * later; callers without one at least do not contradict the disk. */
enum replace_result {
   REPLACE_OK       = 0,  /* renamed, and the directory entry is on disk */
   REPLACE_FAILED   = -1, /* nothing changed; the original is intact */
   REPLACE_UNSYNCED = 1   /* REPLACED and visible; durability unknown */
};

/* Finish a rewrite: fsync the contents, close, rename over `path`, and fsync
 * the directory, checking each step. `fd` is consumed. See enum above. */
enum replace_result replace_finish(int fd, const char *tmp, const char *path);

/* Sync the directory a path lives in, so a rename to it survives power loss. */
int fsync_dir_of(const char *path);

enum replace_result atomic_replace(const char *path, const void *data, int len);

/* ---- EVIDENCE THAT A LOG IS EMPTY ON PURPOSE -------------------------
 *
 * WHY THIS EXISTS AT ALL. The phone is authoritative over the server's copy
 * of the record (see sync.h), so "we no longer hold this" is an instruction
 * to delete. That is right for a bucket the user emptied and catastrophically
 * wrong for a log the phone has merely FORGOTTEN -- a reinstall, a cleared
 * app, a restored handset -- and sync.c cannot tell the two apart by looking
 * at the file, because both look like "no rows here". It therefore refuses:
 * an empty local log against a server that holds data fails the sync, every
 * cycle, rather than erasing years of readings in a few hundred requests.
 *
 * The cost of that refusal is the case this file adds. slots_save() writes
 * the WHOLE registry and nothing else, so removing the last device leaves
 * slots.csv zero bytes long -- a deliberate, ordinary, user-visible action
 * that the sync then refuses for ever, leaving the server holding devices the
 * user removed and the sync permanently broken for every other log too.
 *
 * So the DELETION WORKFLOW leaves evidence, and only that evidence authorises
 * an empty replacement. Two properties make the evidence trustworthy:
 *
 *   IT IS DURABLE. Written stage-fsync-rename-fsync like every other record
 *   here (atomic_replace), because evidence that evaporates in a power cut
 *   authorises nothing after the reboot -- and the user's deletion silently
 *   fails to converge, which is the bug this is fixing.
 *
 *   IT LIVES NEXT TO THE LOG. Deliberately: the failure it must never
 *   authorise is storage loss, and storage loss takes the whole data
 *   directory. Evidence kept anywhere else -- a preference, another
 *   partition, the cloud -- would SURVIVE the loss it exists to be
 *   distinguished from and would then authorise deleting the backup.
 *
 * AND THE LOG MUST STILL BE THERE. log_clear_generation answers 0 unless the
 * log file itself exists and is zero bytes long. Clearing a log leaves an
 * empty FILE; losing the storage leaves a HOLE. Refusing to read a missing
 * log as "deliberately empty" is what keeps a reinstall fail-safe even if a
 * tombstone somehow outlived it. */
#define LOG_CLEAR_SUFFIX ".clear"

/* The clear generation recorded for `path`: which deliberate clear this
 * evidence was minted for, counting from 1 and rising each time the log is
 * emptied again without having regained a row in between (a log that gets
 * rows back has its evidence dropped, so the count starts over -- see
 * log_clear_forget). 0 means NO EVIDENCE, and 0 is the
 * answer for every doubt there is -- no tombstone, a tombstone this build
 * does not understand, a damaged one, or a log that is missing or non-empty.
 * A caller may treat a non-zero answer, and only that, as authorisation. */
long log_clear_generation(const char *path);

/* Record that `path` is empty ON PURPOSE. Called by the deletion workflow
 * AFTER it has durably written the empty log, and it refuses (REPLACE_FAILED)
 * unless the log is really there and really zero bytes -- evidence for a
 * state that does not exist is exactly what must not be mintable.
 *
 * REPLACE_UNSYNCED is its own answer and not a failure: the tombstone IS in
 * place and readable, only its survival across a power cut is unproven. A
 * caller that reported that as failure would leave the deletion looking
 * un-recorded while the file records it. */
enum replace_result log_note_cleared(const char *path);

/* The evidence no longer applies -- the log has rows again. Removing it is
 * what stops a tombstone minted for one deliberate clear from authorising an
 * ACCIDENTAL emptiness months later. 0 when no tombstone remains. */
int log_clear_forget(const char *path);

#endif
