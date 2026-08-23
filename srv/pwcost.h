/* SPDX-License-Identifier: GPL-3.0
 * pwcost.h --- how expensive a password hash is HERE
 * Copyright 2026 Jakob Kastelic
 *
 * Every password this server hashed used PW_ITERS_DEFAULT, a
 * compiled constant chosen from one measurement of one board, and `sync bench`
 * timed that same constant -- so it could report that the cost was wrong and
 * had no way to make it right. A faster machine ran a cheaper KDF than it
 * could afford; a slower one stalled a worker for as long as it took.
 *
 * WHAT A COST HAS TO SATISFY, and both halves matter:
 *
 *   LATENCY. A login waits for one hash. PW_TARGET_MS is what a person may be
 *   kept waiting, and it is a budget rather than a goal -- the calibration
 *   picks the largest cost that fits inside it, because within that budget
 *   more is strictly better against a stolen database.
 *
 *   CONCURRENCY. The hash occupies one worker of a small pool (see
 *   srv/http.h) and cannot be interrupted -- and on a board with one core,
 *   the pool hashing together does not go faster, it goes W times slower.
 *   Timing one hash on an idle machine therefore measures the case that never
 *   matters. PW_BUSY_MS is the second ceiling: a login that arrives while the
 *   whole pool is hashing must still finish inside it, and the calibration
 *   MEASURES that -- it runs the pool's worth of hashes at once and times
 *   them -- rather than assuming the machine scales.
 *
 * The chosen cost is the LOWER of what the two budgets allow. On a
 * many-core machine the busy measurement is barely worse than the idle one
 * and latency decides; on a single-core board it is the pool that decides,
 * which is the board this is for.
 *
 * THE POLICY IS PERSISTED AND VERSIONED, in the data directory beside the
 * database, because a cost that is measured and then forgotten is the compiled
 * constant with extra steps. It is validated at startup: a file this build
 * cannot read is not obeyed, and the server says so and falls back to the
 * compiled default rather than guessing.
 *
 * WHAT USES IT: every hash this server WRITES -- a new account, a changed
 * password, and the re-derivation that upgrades an old credential on a
 * successful login. What it does not touch is verification of an existing
 * hash, which uses the count stored in that user's own row and always has.
 */
#ifndef PANCRA_PWCOST_H
#define PANCRA_PWCOST_H

/* The file, and what it says. Version it: the next field added to this policy
 * must not be read as one of these. */
#define PW_POLICY_NAME    "pwcost.conf"
#define PW_POLICY_VERSION 1

/* THE BUDGET, and it is the deployment's, not the algorithm's.
 *
 * 250 ms is a delay a person signing in does not notice and an attacker with
 * a stolen database pays on every guess. The pool fraction is what keeps a
 * login from being a denial of service: with a hash occupying one worker for
 * that long, a quarter of the pool is the most this server will spend on
 * password hashing at once. */
#define PW_TARGET_MS 250  /* one hash, nothing else running */
#define PW_BUSY_MS   1000 /* one hash, with the whole pool hashing too */

/* What happened when the policy was read. */
enum pwcost_load {
   PWCOST_OK,     /* a policy this build understands; it is in force */
   PWCOST_ABSENT, /* none written yet: the compiled default is in force */
   PWCOST_BAD     /* one exists and cannot be obeyed; the default is in force */
};

/* Read and validate the policy in `dir` (NULL = none). Prints a line for
 * anything but PWCOST_OK, because a server running a cost nobody chose should
 * say so where an operator sees it. Call once, at startup, before serving. */
enum pwcost_load pwcost_init(const char *dir);

/* The iteration count for a hash written from now on. Always within
 * PW_ITERS_MIN..PW_ITERS_MAX, and PW_ITERS_DEFAULT until pwcost_init says
 * otherwise. Safe to call from any thread: it is written once, before the
 * pool exists, and only read afterwards. */
int pwcost_iters(void);

/* MEASURE THIS MACHINE and write the policy. `workers` is the pool this
 * server will run, so the concurrency half of the budget is about the
 * deployment rather than about a number in a header.
 *
 * Prints what it measured and what it chose. 0 on success; 1 when the
 * measurement or the write failed, and then nothing was changed. */
int pwcost_calibrate(const char *dir, int workers);

#endif
