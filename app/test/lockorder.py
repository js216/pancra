#!/usr/bin/env python3
"""THE LOCK ORDER, DERIVED FROM THE SOURCE RATHER THAN FROM THE COMMENTS.

Two deadlocks reached the phone in one day. Both were a lock taken while
another was held, in one order here and the other order there -- the kind of
mistake no reviewer catches by reading one function, because neither function
is wrong on its own. The app freezes, Android says "Pancra isn't responding",
and the sensor keeps streaming into a process that cannot draw.

So this walks every function in app/*.c, works out which locks each one takes
(directly, and through everything it calls), and reports every ORDERED PAIR
"A held while B is acquired". A cycle in those pairs is a deadlock waiting for
the right two threads: it is reported as a failure, with the path.

It is deliberately crude -- text, not a compiler -- and errs toward reporting:
a call inside a held region counts even if a condition means it never runs
there. A false report is a comment away from being silenced; a missed cycle is
a frozen phone.
"""
import re
import sys
import glob
import os

# lock name -> (acquire regexes, release regexes)
LOCKS = {
    "driver": ([r"\bdriver_lock\s*\(\)", r"\bdriver_enter\s*\("],
               [r"\bdriver_unlock\s*\(\)", r"\bdriver_leave\s*\("]),
    "registry": ([r"\breg_lock\s*\(\)"], [r"\breg_unlock\s*\(\)"]),
    "alarm": ([r"\balarm_lock\s*\(\)"], [r"\balarm_unlock\s*\(\)"]),
    "history": ([r"\bstore_lock\s*\(\)"], [r"\bstore_unlock\s*\(\)"]),
    "dis": ([r"mutex_lock\s*\(\s*&dis_lk\s*\)"],
            [r"mutex_unlock\s*\(\s*&dis_lk\s*\)"]),
    "meter-dis": ([r"mutex_lock\s*\(\s*&mdis_lk\s*\)"],
                  [r"mutex_unlock\s*\(\s*&mdis_lk\s*\)"]),
    "append": ([r"mutex_lock\s*\(\s*&append_lk\s*\)"],
               [r"mutex_unlock\s*\(\s*&append_lk\s*\)"]),
    # THE TWO METER LOCKS. Added when they were: a lock this file does not
    # know is a lock whose leaf claim is a comment nobody checks, and both of
    # these are taken from binder callbacks that already hold the driver's.
    "meter-session": ([r"mutex_lock\s*\(\s*&msess_lk\s*\)"],
                      [r"mutex_unlock\s*\(\s*&msess_lk\s*\)"]),
    "meter-rt": ([r"mutex_lock\s*\(\s*&mrt_lk\s*\)"],
                 [r"mutex_unlock\s*\(\s*&mrt_lk\s*\)"]),
    "settings": ([r"mutex_lock\s*\(\s*&set_lk\s*\)"],
                 [r"mutex_unlock\s*\(\s*&set_lk\s*\)"]),
    "calibration": ([r"mutex_lock\s*\(\s*&cal_lk\s*\)"],
                    [r"mutex_unlock\s*\(\s*&cal_lk\s*\)"]),
    # THE BOND TABLE. Written from the binder thread that receives the OS's
    # bond-state broadcast, read by the main loop while rendering. A leaf: a
    # lookup copies the state out and releases before returning.
    "bond": ([r"mutex_lock\s*\(\s*&bond_lk\s*\)"],
             [r"mutex_unlock\s*\(\s*&bond_lk\s*\)"]),
    # THE SETTINGS FILES. Taken with set_lk RELEASED and held across the
    # fsyncs and the rename, so no reader of the preferences ever waits for
    # flash. Never nested with set_lk in either direction: a save renders
    # under set_lk, the setter releases it, and only then is this taken.
    "settings-file": ([r"mutex_lock\s*\(\s*&set_file_lk\s*\)"],
                      [r"mutex_unlock\s*\(\s*&set_file_lk\s*\)"]),
    # THE SYNC ENGINE'S TWO. The operation lock is taken by sync_run /
    # sync_pair / sync_restore and held for the whole operation (it owns the
    # reallocating workspace); the config lock is taken inside it, and alone
    # by the setters. Ordered op -> config, never the reverse: the snapshot
    # copies and releases, and sync_set_key runs inside a pairing operation.
    "sync-op": ([r"mutex_lock\s*\(\s*&g_op_lk\s*\)"],
                [r"mutex_unlock\s*\(\s*&g_op_lk\s*\)"]),
    "sync-config": ([r"mutex_lock\s*\(\s*&g_cfg_lk\s*\)"],
                    [r"mutex_unlock\s*\(\s*&g_cfg_lk\s*\)"]),
    # THE SESSION CACHE. The table, its count and the save-rate state are
    # touched by the RENDER path (build_model -> sessc_put/sessc_restore, on
    # MAIN) and by the flush that the activity's timer AND the foreground
    # service's tick both drive. sessc_lk is the leaf over that state;
    # sessfile_lk is taken with it RELEASED and held across the rename, so no
    # frame ever waits for flash. Never nested in either direction.
    "sess-cache": ([r"mutex_lock\s*\(\s*&sessc_lk\s*\)"],
                   [r"mutex_unlock\s*\(\s*&sessc_lk\s*\)"]),
    "sess-file": ([r"mutex_lock\s*\(\s*&sessfile_lk\s*\)"],
                  [r"mutex_unlock\s*\(\s*&sessfile_lk\s*\)"]),
    # THE METER'S LAST-SYNC FILE. Taken OUTSIDE mrt_lk and held from the
    # render through the rename: the save serialises its callers through
    # completion, because rendering before the guard and skipping on a lost
    # race dropped the losing caller's newer state entirely.
    "meter-sync-file": ([r"mutex_lock\s*\(\s*&msync_lk\s*\)"],
                        [r"mutex_unlock\s*\(\s*&msync_lk\s*\)"]),
    # The calibration FILES, held across the two fsyncs and the rename that
    # every save ends in. Outside cal_lk on purpose: cal_lk is what a frame
    # takes, and a frame must never wait for a disk. See app/calib.c.
    "cal-file": ([r"mutex_lock\s*\(\s*&calfile_lk\s*\)"],
                 [r"mutex_unlock\s*\(\s*&calfile_lk\s*\)"]),
}

NAME = re.compile(r"(\w+)\s*\($")
CALL = re.compile(r"\b(\w+)\s*\(")

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text[i] in "\"'":
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(" ")
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def functions(path):
    """(name, body) for every function defined in the file."""
    src = strip_comments(open(path).read())
    lines = src.split("\n")
    out, i = [], 0
    while i < len(lines):
        # A definition starts at column 0, opens a parameter list, and is
        # followed by a line that starts with `{`. The list may span lines.
        sig, j = None, i
        if lines[i][:1].isalpha() and "(" in lines[i] and ";" not in lines[i]:
            acc = []
            while j < len(lines) and ";" not in lines[j]:
                acc.append(lines[j].strip())
                if lines[j].rstrip().endswith(")"):
                    sig = " ".join(acc)
                    break
                j += 1
        if sig and j + 1 < len(lines) and lines[j + 1].startswith("{"):
            head = sig[:sig.index("(") + 1]
            m = NAME.search(head)
            if not m:
                i += 1
                continue
            i = j
            depth, body, j = 0, [], i + 1
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                body.append(lines[j])
                j += 1
                if depth == 0:
                    break
            out.append((m.group(1), body))
            i = j
        else:
            i += 1
    return out


def direct_locks(body):
    """Locks this function takes itself."""
    text = "\n".join(body)
    return {name for name, (acq, _) in LOCKS.items()
            if any(re.search(p, text) for p in acq)}


def held_calls(body):
    """(lock, callee) for every call made while `lock` is held here."""
    held, out = [], []
    for line in body:
        for name, (acq, rel) in LOCKS.items():
            if any(re.search(p, line) for p in acq):
                held.append(name)
            if any(re.search(p, line) for p in rel) and name in held:
                held.remove(name)
        if not held:
            continue
        for c in CALL.findall(line):
            if c in KEYWORDS:
                continue
            for h in held:
                out.append((h, c))
    return out


# CALLBACKS ARE CALLS. The driver dispatches through an ops table
# (driver_meter_ops, and the callbacks it invokes from inside its own lock),
# so the function named in a struct initialiser is reachable from the
# dispatcher even though no line in the source names it there. Missing that is
# missing exactly the edge that matters: the driver's lock is held ACROSS the
# dispatch, and what the callback goes on to take is what completes a cycle.
OPS = re.compile(r"^\s*\.\w+\s*=\s*(\w+)\s*,?\s*$")


def ops_targets(path):
    return set(OPS.findall(strip_comments(open(path).read())))


# The acquire/release primitives themselves: they return holding (or not
# holding) on purpose, which is what they are for.
# THE ACQUIRE/RELEASE PAIRS THEMSELVES. Each of these exists to take or give
# back a lock on its caller's behalf, so "ends still holding one" is their
# contract rather than a leak. Every one is a matched pair, and the checker
# still holds their CALLERS to the rule.
PRIMITIVES = {"driver_enter", "driver_leave", "driver_lock", "driver_unlock",
              "reg_lock", "reg_unlock", "alarm_lock", "alarm_unlock",
              "store_lock", "store_unlock", "hist_lock", "hist_unlock",
              "sync_ctx_begin", "sync_ctx_end"}


def leaks(path):
    """Returns that leave a lock held, and bodies that end still holding.

    A LEAKED lock is worse than a cycle: the owner walks away, and with a
    recursive lock its depth never falls to zero, so every other thread spins
    on it for the life of the process. Three `return`s inside one driver
    callback did exactly that -- the commonest was an ordinary stray write-ack
    during subscription -- and the phone froze mid-reconnect.
    """
    out = []
    for name, body in functions(path):
        if name in PRIMITIVES:
            continue
        depth = 0
        for line in body:
            for _, (acq, rel) in LOCKS.items():
                if any(re.search(p2, line) for p2 in acq):
                    depth += 1
                if any(re.search(p2, line) for p2 in rel):
                    depth -= 1
            if depth > 0 and re.search(r"\breturn\b", line):
                out.append((name, line.strip(), "returns holding a lock"))
        if depth > 0:
            out.append((name, "", "ends still holding a lock"))
    return out


def main():
    files = sorted(glob.glob("app/*.c"))
    direct, calls, where = {}, {}, {}
    heldpairs = []
    dispatched = set()
    for f in files:
        dispatched |= ops_targets(f)
    for f in files:
        for name, body in functions(f):
            direct.setdefault(name, set()).update(direct_locks(body))
            where[name] = f
            cs = set()
            for line in body:
                for c in CALL.findall(line):
                    if c not in KEYWORDS:
                        cs.add(c)
            calls.setdefault(name, set()).update(cs)
            for h, c in held_calls(body):
                heldpairs.append((h, c, f, name))
    # Every dispatcher that holds a lock across a callback: driver_route_* and
    # the ops the driver owns. Treated as calling ALL registered callbacks,
    # because which one runs is a run-time decision the source cannot show.
    for name in list(calls):
        if name.startswith("driver_route_") or name.startswith("driver_cal_"):
            for cb in dispatched:
                heldpairs.append(("driver", cb, "app/dexdriver.c", name))

    leaked = [(f, x) for f in files for x in leaks(f)]
    if leaked:
        for f, (fn, line, why) in leaked:
            print(f"lockorder: {f}:{fn} {why}")
            if line:
                print(f"    {line}")
        print("  A lock let go of by RETURNING is never released. The driver's")
        print("  and the registry's are recursive and have no timeout, so the")
        print("  owner's depth never reaches zero and every other thread --")
        print("  the main looper first -- spins on it until Android kills the")
        print("  app. Leave through one exit (`goto out`) that unlocks.")
        return 1

    # locks reachable from a function, through everything it calls
    memo = {}

    def reach(fn, seen=None):
        if fn in memo:
            return memo[fn]
        seen = seen or set()
        if fn in seen or fn not in calls:
            return direct.get(fn, set())
        seen = seen | {fn}
        got = set(direct.get(fn, set()))
        for c in calls.get(fn, ()):  # noqa: B007
            got |= reach(c, seen)
        if not (seen - {fn}):
            memo[fn] = got
        return got

    edges = {}
    for h, c, f, fn in heldpairs:
        for got in reach(c):
            if got != h:
                edges.setdefault((h, got), set()).add(f"{f}:{fn} -> {c}()")

    # a cycle among the ordered pairs is a deadlock
    order = sorted({a for a, _ in edges} | {b for _, b in edges})
    bad = []
    for a in order:
        for b in order:
            if a < b and (a, b) in edges and (b, a) in edges:
                bad.append((a, b))
    if bad:
        for a, b in bad:
            print(f"lockorder: {a} and {b} are taken in BOTH orders --")
            print(f"  two threads, one holding each, spin for ever.")
            for s in sorted(edges[(a, b)]):
                print(f"    {a} held, {b} taken: {s}")
            for s in sorted(edges[(b, a)]):
                print(f"    {b} held, {a} taken: {s}")
        return 1
    # AND THE DOCUMENTED ORDER, not merely the absence of a cycle. The three
    # big locks are ranked driver -> registry -> history (dexdriver.h,
    # sensors.h, store.h); the small ones are leaves and are taken last. An
    # edge that runs backwards up this list is a cycle that has not happened
    # yet -- the phone froze twice in one day on exactly that.
    RANK = {"driver": 0, "registry": 1, "history": 2,
            "alarm": 3, "dis": 4, "meter-dis": 4, "append": 4,
            # The leaves added since: each is taken innermost and never held
            # across another module's call. Same rank as the other leaves, so
            # a pair of them taken in both orders is still reported.
            "meter-session": 4, "meter-rt": 4, "settings": 4,
            # cal-file is taken OUTSIDE cal_lk (rank 3, not 4): it guards the
            # write, which the state lock must not be held across.
            "cal-file": 3,
            "settings-file": 3,
            # The two file locks added with the lost-update fixes. Rank 3 for
            # the same reason cal-file is: each guards the WRITE, and the
            # state lock beside it must not be held across an fsync.
            # meter-sync-file is held ACROSS mrt_lk being taken, which is the
            # one nesting among these three and is why it must rank above it.
            "sess-file": 3,
            "meter-sync-file": 3,
            "sess-cache": 4,
            "sync-op": 3,
            "sync-config": 4,
            "calibration": 4, "bond": 4}
    back = [(a, b) for (a, b) in edges
            if RANK.get(a, 9) > RANK.get(b, 9)]
    if back:
        for (a, b) in sorted(back):
            print(f"lockorder: {a} is held while {b} is taken, and the order")
            print(f"  is {b} before {a}. Sample the {b} side BEFORE taking")
            print(f"  {a} -- alarm_gather and meter_links_snapshot are the")
            print(f"  pattern: read what you need, release, then lock.")
            for s in sorted(edges[(a, b)]):
                print(f"    {s}")
        return 1
    print("\033[1;32mlockorder\033[0m: %d ordered pairs, none backwards"
          % len(edges))
    for (a, b) in sorted(edges):
        print(f"    {a} -> {b}: %s" % sorted(edges[(a, b)])[0])
    return 0


if __name__ == "__main__":
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.."))
    sys.exit(main())
