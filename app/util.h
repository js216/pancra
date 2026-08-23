// SPDX-License-Identifier: GPL-3.0
// util.h --- Small dependency-free time/format helpers
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_UTIL_H
#define PANCRA_UTIL_H

#include "loadresult.h" /* enum load_result: what a read of a file means */

/* THE CLOCKS MOVED TO clock.h. They were here, in a header otherwise about
 * files, logs and strings -- which was harmless until thread.h needed a
 * millisecond clock and this module needed a mutex, and the two lowest
 * primitives in the app started including each other. */

int clampn(int n,
           int cap); /* clamp a snprintf length to [0, cap-1] for write() */

/* ---- A BOUNDED TEXT BUILDER THAT FAILS CLOSED ----------------
 *
 * WHAT THIS EXISTS TO STOP. Several files build a whole file's text in one
 * buffer and then replace the file atomically, row by row, like this:
 *
 *     for (each row) {
 *        int n = snprintf(out + used, cap - used, "...", ...);
 *        if (n <= 0 || n >= cap - used)
 *           break;                 <-- and the caller writes what it HAS
 *        used += n;
 *     }
 *
 * The break is the bug. What follows it is an atomic replace of the file with
 * the rows that DID fit and a report of success, so a registry of eight
 * sensors becomes a registry of six -- permanently, because the file is the
 * record and nothing ever notices the two missing rows. That is a paired
 * sensor the user has to pair again, key and all.
 *
 * So the rule is written once, here, and it is: a builder that could not take
 * a row is BAD FROM THEN ON, and a caller may only publish what it built when
 * the builder is still good. Nothing is written after the first failure --
 * the buffer's contents stop being a file anybody should see.
 *
 * The append itself stays at the call site (each row's format string is that
 * module's business, and this codebase has no vsnprintf); what is shared is
 * the accounting, which is the part that was got wrong. */
struct textout {
   char *buf;
   int cap;
   int len;
   int bad; /* sticky: once set, the buffer is not publishable */
};

void tout_init(struct textout *t, char *buf, int cap);

/* Where the next row goes and how much room it has, or NULL when the builder
 * is already bad or full -- in which case the loop must stop. `*room` is the
 * size to pass to snprintf, NUL included. */
char *tout_room(struct textout *t, int *room);

/* Account for the snprintf that just wrote at tout_room()'s pointer: `n` is
 * its return value. A row that could not be formatted, or that did not fit,
 * sets the sticky error. */
void tout_took(struct textout *t, int n);

/* 1 when every row so far was taken whole. The caller publishes ONLY on 1. */
int tout_ok(const struct textout *t);
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
/* Replace a small snapshot atomically. The existing file remains intact on
 * every failure. Returns 0 only after the replacement has reached the
 * filesystem. */
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
/* Run inside log_replace_with_tail with the append lock held, between the
 * tail capture and the rename. A claim that a lock closes a window is only
 * worth what a test that enters the window can say about it. */
extern _Thread_local void (*app_fault_publish_gap)(void);
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
 * the original file is untouched, so a caller is right to put its memory
 * back and report that nothing happened.
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

/* ---- PUBLISH A REBUILT LOG, WITH NO APPEND LOST -------------
 *
 * A rebuild (a restore) stages a whole log and renames it over the original.
 * Between the last byte it copied and the rename, an append can land -- and
 * the rename then deletes a row the app had already acknowledged and drawn on
 * screen.
 *
 * This closes that window: the tail of `path` past byte `copied` is appended
 * to `fd`, and the fsync, close and rename happen with the append lock still
 * held, so no log_append can interleave. The lock stays private to util.c;
 * what is exported is the OPERATION, because "who may hold the append lock,
 * and across what" is not a question a caller should have to answer.
 *
 * `fd` is the staging file at its end, `tmp` its path. Answers as
 * replace_finish does, and on REPLACE_FAILED the staging file is already
 * unlinked and `fd` closed. */
enum replace_result log_replace_with_tail(const char *path, int fd,
                                          const char *tmp, long copied);

/* Sync the directory a path lives in, so a rename to it survives power loss. */
int fsync_dir_of(const char *path);

enum replace_result atomic_replace(const char *path, const void *data, int len);

/* ---- EVIDENCE THAT A LOG IS EMPTY ON PURPOSE -------------------------
 *
 * WHY THIS EXISTS AT ALL. The phone is authoritative over the server's copy
 * of the record (see sync.h), so "this is not something we hold" is an
 * instruction to delete. That is right for a bucket the user emptied and
 * catastrophically wrong for a log the phone has merely FORGOTTEN -- a
 * reinstall, a cleared app, a restored handset -- and sync.c cannot tell the
 * two apart by looking at the file, because both look like "no rows here". It
 * therefore refuses: an empty local log against a server that holds data fails
 * the sync, every cycle, rather than erasing years of readings in a few hundred
 * requests.
 *
 * The cost of that refusal is the case this file adds. The device registry is
 * rewritten WHOLE and holds nothing else, so removing the last device leaves
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
 *   fails to converge.
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

/* The evidence has stopped applying -- the log has rows again. Removing it is
 * what stops a tombstone minted for one deliberate clear from authorising an
 * ACCIDENTAL emptiness months later. 0 when no tombstone remains. */
int log_clear_forget(const char *path);

/* ---- EDITING ONE ROW OF A LOG THAT ALLOWS IT -------------------------
 *
 * Weight, food and exercise are editable: a row can be corrected or removed.
 * All three did it the same way -- stream the file, copy every line to a
 * temporary except the LAST one that matches, publish by rename -- in three
 * copies, each of whose comments named one of the others. Every durability
 * rule in that algorithm had to be learned three times, and the copies that
 * did not learn it failed in somebody's data: a read that fails mid-copy must
 * not be published, an over-long row must refuse the rewrite rather than be
 * written back truncated, and a delete that empties the file must leave a
 * tombstone or the sync client refuses to replicate the emptiness for ever.
 *
 * There is one copy now, and the caller supplies only the part that is its
 * own: which line is the row, and what the replacement says. */

/* THE LONGEST ROW THESE LOGS HAVE. Every editable log's rows are a handful of
 * numbers or a short name; a line longer than this cannot be one this app
 * wrote, and the rewrite refuses rather than write back a truncation. */
#define LOG_EDIT_ROW_MAX 256
/* Room for a log's path plus ".tmp". */
#define LOG_EDIT_PATH_MAX 300

struct log_edit {
   /* IS THIS THE ROW? `line`..`end` is one row of the file without its
    * newline. Called for every line, on both passes, so it must be pure. */
   int (*matches)(const char *line, const char *end, void *ctx);
   /* THE REPLACEMENT ROW, without its newline, into `out` (at most `cap`
    * bytes). Returns its length, or negative to abandon the rewrite with the
    * original untouched.
    *
    * NULL MEANS DELETE, which is why it is a null check rather than a flag:
    * "there is no replacement row" and "the row goes away" are the same
    * statement, and a flag lets a caller say one and mean the other. */
   int (*format)(char *out, int cap, void *ctx);
   void *ctx;
};

/* ---- READ A WHOLE SMALL FILE, EXACTLY ---------------------
 *
 * Every state file this app keeps -- the settings, the device info, the
 * calibration queue, the rescale factor, the session cache, the meter's two
 * -- is small, fixed-shape, and read in one go. Each loader had its own
 * version of that, and most of them were ONE `read(fd, b, sizeof b - 1)` with
 * the result used as the file's length. Three things are wrong with that, and
 * they are wrong in the direction of PUBLISHING SOMETHING:
 *
 *   - a short read is not the end of the file. read() may return fewer bytes
 *     than asked for at any time (a signal, a filesystem that felt like it),
 *     and the loader then parses a PREFIX of the record and publishes it.
 *   - EINTR is a failure to READ, not a file that ends there. Unhandled, a
 *     signal during startup truncates whatever was being loaded.
 *   - a full buffer proves nothing about the length. A file LONGER than the
 *     buffer is not one this app wrote -- it is damage or another program's
 *     -- and it decodes as a valid prefix.
 *
 * ONE reader, and app/meterstore.c's (which had all three right) is where it
 * comes from. It loops over EINTR and short reads, probes for a byte past the
 * end, and NUL-terminates.
 *
 *   LOAD_ABSENT   no file. A first run: the defaults are correct.
 *   LOAD_OK       `*len` bytes, and the file ended exactly there.
 *   LOAD_CORRUPT  the file exists and is not one this build can hold: empty
 *                 (created and never written -- a torn save) or longer than
 *                 `cap - 1`.
 *   LOAD_ERROR    it could not be read. Nothing is known about its contents.
 *
 * `buf` is written only on LOAD_OK; every other answer leaves it alone, so a
 * caller cannot half-publish a file it was told to refuse. */
/* The largest file this reader will take in one go. Every caller's buffer is
 * smaller than this (the biggest is the session cache's kilobyte); the bound
 * exists so the staging copy inside is a fixed local rather than an
 * allocation, and a caller asking for more than this gets its own buffer's
 * size and no more -- which the EOF probe then reports as CORRUPT if the file
 * really is longer. */
#define MAX_EXACT_READ 4096

enum load_result read_file_exact(const char *path, char *buf, int cap,
                                 int *len);

/* Rewrite `path`, replacing (or deleting) the LAST row `ed` matches.
 *
 * Returns 0 when the file now holds the edit, -1 when it does not -- and on
 * -1 the original is untouched: nothing is published until the whole
 * copy has succeeded. -1 also covers "no row matched" and "the log could not
 * be read", which are the same thing to a caller: the edit did not happen.
 *
 * On success the caller still owns its own in-memory tail: reload it. */
int log_edit_last(const char *path, const struct log_edit *ed);

#endif
