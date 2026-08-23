// SPDX-License-Identifier: GPL-3.0
// crashlog.h --- append a line of app context on a fatal signal
// Copyright 2026 Jakob Kastelic

/* A native crash writes the signal plus a little app state to files/crash.log
 * and then re-raises, so the OS still records its own tombstone. Retrieve with
 *   adb shell run-as com.jk.pancra cat files/crash.log
 *
 * Split out of main.c because it is genuinely separate from everything else in
 * there: it runs in a signal handler, it may run at any instant including
 * mid-malloc, and the rules it obeys (nothing but pure formatting plus open /
 * write / close / signal / raise -- no snprintf, no heap, no locale) apply to
 * nothing else in the program. Rules that hold for one file are easier to keep
 * than rules that hold for eighty lines somewhere inside eight thousand.
 *
 * It also makes the formatting helpers reachable by a test, which is the whole
 * argument for the split: they are pure, and they were unreachable.
 */
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdatomic.h>

/* What to record besides the signal. POINTERS to the live values, not copies:
 * the handler must read whatever is true at the moment of the crash, and it
 * cannot call anything to go and fetch it. Reading through them is a single
 * lock-free load, which is safe in a signal handler; anything cleverer would
 * not be. */
struct crash_ctx {
   /* TWO stars, and the difference is the whole point.
    *
    * `status` is a character ARRAY, so a pointer to it always reads whatever
    * the array holds now. The checkpoint label is not: it is a pointer
    * VARIABLE that gets repointed at a new string literal at each checkpoint.
    * Storing its value here copies the pointer as it stood when crash_install
    * ran -- during onCreate, before any checkpoint has executed -- and froze
    * every crash report at "boot". Storing the address of the variable makes
    * the handler read the label that is current at the moment of the crash,
    * which is the only reason the field exists. */
   /* ATOMIC, not volatile, and for a reason volatile cannot serve: the
    * checkpoint is written by every thread in the app and read here from a
    * signal handler that can interrupt any of them mid-instruction. volatile
    * keeps the compiler from discarding the store; it does not make the load
    * indivisible, and a torn pointer read in a crash handler turns a crash
    * report into a second crash. A relaxed atomic load is one instruction on
    * every target this builds for, and is async-signal-safe. */
   const char *_Atomic *where; /* -> the last checkpoint label */

   /* AND SO ARE THE SCALARS, for the same reason and not a weaker one.
    *
    * Every one of these is written by a thread other than the one that
    * crashes -- glucose and the sample count by a binder thread under the
    * history lock, the screen by the main looper -- and read HERE, from a
    * handler that can interrupt any of them mid-instruction and may not take
    * a lock to do it. `volatile` says only that the compiler must re-read the
    * object; it makes no promise about indivisibility, it is not a
    * synchronisation primitive in C11's memory model, and a program whose
    * threads share a plain `int` has undefined behaviour whatever the
    * hardware would have done with it.
    *
    * "An aligned int cannot tear on this target" is an argument about a
    * machine, made in a file whose whole subject is what to trust when the
    * program has already gone wrong. A relaxed atomic load costs exactly the
    * same instruction on every target this builds for, is async-signal-safe
    * (C11 7.14.1.1: a handler may access a lock-free atomic), and needs no
    * such argument -- so the values are published as atomics by their owners
    * and read as atomics here.
    *
    * RELAXED IS THE RIGHT ORDER. Nothing is being ordered against these: the
    * handler wants each number as a number, not as a signal that something
    * else has happened. What it must not get is a value the compiler cached
    * three functions ago, or half of one. */
   const _Atomic char *status; /* the status line on screen */
   const _Atomic int *glu;     /* current glucose, or -1 */
   const _Atomic int *menu;    /* which screen was up */
   const _Atomic int *nhist;   /* how many samples were held */
};

/* ---- WHAT WAS ACTUALLY INSTALLED ---------------------------
 *
 * crash_install was void and dropped every signal() result, so startup could
 * announce crash reporting with none of it in place -- and the failure that
 * produces is invisible by construction: the app dies, there is no
 * crash.log, and nothing says the handler was never there.
 *
 * One bit per signal, in the order crash_sig_of names them. `installed` and
 * `failed` are separate rather than one mask and its complement so that a
 * caller can tell "not asked for" from "asked for and refused" if the list
 * ever grows.
 *
 * A PARTIAL INSTALL IS KEPT. A handler that is in place writes a report for
 * its own signal; removing it because another could not be installed trades
 * a partial diagnostic for none. The caller reports what is missing. */
struct crash_install_result {
   unsigned installed; /* bit k set: CRASH signal k is covered */
   unsigned failed;    /* bit k set: the kernel refused it */
};

/* The signal number bit `k` stands for, or 0 past the end -- so a caller can
 * name what is missing without keeping its own copy of the list. */
int crash_sig_of(unsigned bit);

/* Point the handler at `dir`/crash.log and install it for the fatal signals.
 * `ctx` must outlive the process -- pass the address of file-scope state.
 * The context and the path are published BEFORE the first registration: a
 * signal arriving between two of them must find both. */
struct crash_install_result crash_install(const char *dir,
                                          const struct crash_ctx *ctx);

/* The two pure formatters, exposed so they can be tested. Append to b[*pos],
 * never past cap. crash_puts stops at max characters of s. */
void crash_putn(char *b, int cap, int *pos, long v);
void crash_puts(char *b, int cap, int *pos, const volatile char *s, int max);
/* The same, for a string whose BYTES are atomic -- the status line, which is
 * written by one thread and read by the handler (see struct crash_ctx). Each
 * character is one relaxed load. */
void crash_puts_atomic(char *b, int cap, int *pos, const _Atomic char *s,
                       int max);

/* Build the whole line the handler writes, into b (never past cap); returns
 * its length. Split out of the handler so a test can hold a crash_ctx, move
 * the values it points at, and check that the line FOLLOWS them -- the bug
 * this exists to prevent was a field that captured a value once and reported
 * it forever, which no test of the formatters alone could have caught. */
int crash_line(char *b, int cap, int sig, const struct crash_ctx *ctx);

#endif
