#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# synclogcheck.py --- does every synced log count towards "has anything changed?"
# Copyright 2026 Jakob Kastelic
"""Two lists in app/syncjni.c have to name the same files.

syncjni_register_logs() says WHICH FILES SYNC. syncjni_state_stamp() says
WHETHER ANYTHING HAS CHANGED, by adding up the sizes of the files it knows
about. They are written separately, a hundred lines apart, and nothing in the
compiler or the test suite connects them.

A log in the registry but NOT in the stamp is the dangerous direction, and it
is dangerous precisely because it mostly works:

  - log something in that file alone, and the stamp is unchanged, so the phone
    concludes it has nothing to send and does not sync;
  - log anything in any OTHER synced file, and the stamp moves, a sync runs,
    and the first file goes up with it, correctly.

So the log appears to sync -- it does, most of the time -- and silently fails
only for the records that were made on their own. The six-hour safety sync
eventually covers it, which turns a lost record into a delayed one and makes
the bug even harder to see.

The other direction (in the stamp, not in the registry) is harmless but is
still a mistake worth naming: it makes the phone sync when nothing that syncs
has changed.

The check is deliberately textual. It reads the two lists as they are written
rather than by building anything, because what it is guarding is a
correspondence between two pieces of source that no build can observe.
"""
import re
import sys

SRC = "app/syncjni.c"


def body_of(text, func):
    """The text between `func(` and the closing brace of its block."""
    i = text.find(func)
    if i < 0:
        return None
    depth = 0
    started = False
    out = []
    for j in range(i, len(text)):
        c = text[j]
        if c == "{":
            depth += 1
            started = True
        elif c == "}":
            depth -= 1
            if started and depth == 0:
                return "".join(out)
        if started:
            out.append(c)
    return None


def main():
    try:
        text = open(SRC, encoding="utf-8").read()
    except OSError as e:
        print("synclogcheck: cannot read %s: %s" % (SRC, e))
        return 1

    reg = body_of(text, "void syncjni_register_logs(void)")
    stamp = body_of(text, "long syncjni_state_stamp(void)")
    if reg is None or stamp is None:
        # A RENAME MUST FAIL THIS, NOT SKIP IT. A check that quietly passes
        # when it cannot find what it is checking is worse than no check: it
        # keeps reporting success for as long as the thing it guards is gone.
        print("synclogcheck: could not find both functions in %s." % SRC)
        print("  Expected syncjni_register_logs() and syncjni_state_stamp().")
        print("  If one was renamed, rename it here too -- this gate is the")
        print("  only thing keeping their two lists in step.")
        return 1

    # `sync_add_log("name", foo_path(), n)` -> foo_path
    registered = set(re.findall(r'sync_add_log\(\s*"[^"]*"\s*,\s*(\w+)\s*\(\)',
                                reg))
    # the paths[] initialiser -> every foo_path() mentioned
    stamped = set(re.findall(r"(\w+_path)\s*\(\)", stamp))

    missing = sorted(registered - stamped)
    extra = sorted(stamped - registered)
    if missing:
        print("synclogcheck: a synced log does not count towards the change")
        print("  stamp, so a record written ONLY to it does not trigger a")
        print("  sync -- and one written to any other synced file carries it")
        print("  up as a side effect. That is why this fails as 'works most")
        print("  of the time' rather than as 'never syncs'.")
        for m in missing:
            print("    registered but not stamped: %s()" % m)
    if extra:
        print("synclogcheck: the change stamp reads a file that is not")
        print("  registered for sync, so the phone can decide it has")
        print("  something to send when nothing that syncs has changed.")
        for m in extra:
            print("    stamped but not registered: %s()" % m)
    if missing or extra:
        return 1
    print("\033[1;32msynclogcheck\033[0m: all %d synced logs count towards "
          "the change stamp" % len(registered))
    return 0


if __name__ == "__main__":
    sys.exit(main())
