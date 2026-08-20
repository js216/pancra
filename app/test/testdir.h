// SPDX-License-Identifier: GPL-3.0
// testdir.h --- where a host suite keeps its fixtures, per mode and per suite
// Copyright 2026 Jakob Kastelic

/* ONE OWNER OF THE FIXTURE DIRECTORY, READ AT RUN TIME.
 *
 * The Makefile keys every test BINARY by build mode ($(TESTDIR):
 * build/app/test, -asan, -tsan), because `make check` lists the ordinary host
 * suites AND appasan AND apptsan as independent prerequisites and all three
 * used to write and execute one path. The fixtures had the same defect and
 * outlived the fix: the data directory was written out as the literal string
 * "build/app/test" in twelve suites, so a sanitizer binary read and wrote the
 * PLAIN tree. Six suites (calibtest, durabilitytest, metersesstest,
 * meterstoretest, modeltest, threadtest) are in both sanitizer lists and the
 * plain one, so `make -j check` put two or three processes in one fixture
 * directory: `make -j2 appasan apptsan` failed in calibtest on assertions
 * about state another process had replaced underneath it, while each passed
 * alone. `make appasan` alone failed in storetest for the mirror reason --
 * the literal build/app/test need not exist at all when only the sanitizer
 * tree has been built.
 *
 * KEYED BY SUITE TOO, and that half was learned second. Per-mode alone still
 * left every suite of ONE mode in one directory, and `make -j8` hands the
 * jobserver to the sanitizer submake as well, so a dozen same-mode suites run
 * at once -- and they do not have distinct filenames. registrytest, statstest,
 * modeltest and pairingtest each point sensors_paths() at their fixture
 * directory and then unlink and re-mint sensors.csv and slots.csv;
 * settingstest and modeltest share settings.cfg and remote.cfg; storetest,
 * weighttest and insulintest each share a log with modeltest. `make -j8 uitest
 * appasan apptsan` failed in statstest on "a sensor with a known activation is
 * registered" -- a mint into a registry file another process had just
 * truncated. So $(TESTDIR)/fixtures/<suite> rather than $(TESTDIR), which the
 * Makefile derives from the target name; nothing here changes, because a suite
 * only ever asks where its OWN directory is.
 *
 * ENVIRONMENT, NOT A -D. A compile-time -DAPP_TESTDIR=... would work for the
 * gates, but it makes the binary's fixture path invisible from the outside
 * and unchangeable without a rebuild: running ./build/app/test-asan/calibtest
 * by hand from a debugger would still point at whatever tree the last build
 * named. Read from the environment with the plain tree as the default, a
 * suite run by hand behaves exactly as it did before this existed, and the
 * Makefile selects the tree the same way it selects TESTDIR -- one variable,
 * at the point where the binary is launched.
 *
 * Include it as "testdir.h": quoted includes search the directory of the
 * including file first, so app/test/ needs no -I of its own.
 */
#ifndef TESTDIR_H
#define TESTDIR_H

#include <stdio.h>
#include <stdlib.h>

/* The base directory. Never NULL and never empty, so a caller may snprintf
 * into it without a guard. An empty APP_TESTDIR is treated as unset rather
 * than as the root, because "" would silently build absolute paths ("/cal.q")
 * -- the same class of accident that an empty $(TESTDIR) caused in the
 * Makefile. */
static inline const char *test_dir(void)
{
   const char *d = getenv("APP_TESTDIR");

   return (d && *d) ? d : "build/app/test";
}

/* base "/" rel, in the caller's buffer. The caller owns the storage on
 * purpose: several suites build fixture paths from more than one thread, and a
 * shared static would hand them each other's answers.
 *
 * A PATH THAT DOES NOT FIT KILLS THE RUN, LOUDLY. Truncation here would point
 * the suite at some other directory that happens to be a prefix of the one it
 * asked for -- which is not a hypothetical: proving this mechanism worked, a
 * 96-byte buffer and a 96-character APP_TESTDIR silently dropped the last
 * character and the fixtures landed in a NEIGHBOURING directory of an earlier
 * run, with every assertion still passing. A test that quietly reads and writes
 * the wrong tree is the whole defect this file exists to remove. */
static inline const char *test_path(char *buf, size_t n, const char *rel)
{
   int len = snprintf(buf, n, "%s/%s", test_dir(), rel);

   if (len < 0 || (size_t)len >= n) {
      (void)fprintf(stderr,
                    "test_path: %s/%s does not fit in %zu bytes.\n"
                    "  Either APP_TESTDIR is longer than this suite's buffers "
                    "or the buffer is too small; both are bugs here, and\n"
                    "  truncating would silently point the fixtures at another "
                    "directory.\n",
                    test_dir(), rel, n);
      exit(1);
   }

   return buf;
}

#endif
