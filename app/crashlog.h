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

/* What to record besides the signal. POINTERS to the live values, not copies:
 * the handler must read whatever is true at the moment of the crash, and it
 * cannot call anything to go and fetch it. Reading through them is a plain
 * load, which is safe in a signal handler; anything cleverer would not be. */
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
   const volatile char **where; /* -> the last checkpoint label */
   const volatile char *status; /* the status line on screen */
   const volatile int *glu;     /* current glucose, or -1 */
   const volatile int *menu;    /* which screen was up */
   const volatile int *nhist;   /* how many samples were held */
};

/* Point the handler at `dir`/crash.log and install it for the fatal signals.
 * `ctx` must outlive the process -- pass the address of file-scope state. */
void crash_install(const char *dir, const struct crash_ctx *ctx);

/* The two pure formatters, exposed so they can be tested. Append to b[*pos],
 * never past cap. crash_puts stops at max characters of s. */
void crash_putn(char *b, int cap, int *pos, long v);
void crash_puts(char *b, int cap, int *pos, const volatile char *s, int max);

/* Build the whole line the handler writes, into b (never past cap); returns
 * its length. Split out of the handler so a test can hold a crash_ctx, move
 * the values it points at, and check that the line FOLLOWS them -- the bug
 * this exists to prevent was a field that captured a value once and reported
 * it forever, which no test of the formatters alone could have caught. */
int crash_line(char *b, int cap, int sig, const struct crash_ctx *ctx);

#endif
