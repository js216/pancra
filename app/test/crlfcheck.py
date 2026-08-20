#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# crlfcheck.py --- every first-party text file, scanned for CR line endings.
# Copyright 2026 Jakob Kastelic
"""ONE manifest of first-party text, and a CR in any of it is a failure.

WHAT THIS REPLACES, AND WHY IT WAS NOT ENOUGH.

The CRLF pass lived in the Makefile's `format` target and scanned the
formatter's file set: C, headers, the Makefile and the Android manifest. That
is the set clang-format has an opinion about, and it is not the set that
BREAKS when a carriage return gets into it. Everything executable was outside
it -- the 26 shell scripts, the Java, the Python gates, the deployment conf,
the XML resources -- and the shell scripts are the ones that cost an incident:

    #!/bin/sh<CR>

is a shebang naming an interpreter called "/bin/sh\\r", which does not exist.
The kernel refuses the exec and the shell reports "no such file or directory"
about a file that is plainly there and an interpreter that is plainly
installed. It passes `make check` -- nothing looked at it -- and fails on the
board, during a deployment, reading like a corrupted filesystem.

So the manifest is built once, here, and it is EVERYTHING FIRST-PARTY:
tracked plus untracked-but-not-ignored, which is the same enumeration
stalecheck and symcheck use, minus a stated list of binary and generated
paths. Two rules follow from the history of the checks in this tree:

  * A FILE THAT CANNOT BE READ IS A FAILURE, not a skip. The old scan's
    ancestor swallowed "No such file or directory" with `|| true` and reported
    success over a directory layout that had not existed for months. A gate
    with a silent skip has a hole exactly where something odd is happening.

  * THE GATE PROVES ON EVERY RUN THAT IT CAN STILL FAIL. selftest() below
    builds a throwaway repository containing a CRLF shell script, Java,
    Python, conf, deployment conf, Makefile, C and an untracked script, and
    requires every one of them to be refused -- and the excluded ones to be
    let through. A per-suffix mistake that quietly drops .py from the manifest
    is otherwise indistinguishable from a tree with no CRLF in its Python,
    and there is no other check anywhere that would notice.

The exclusions are STATED rather than inherited from .gitignore. Almost all of
them are ignored today, so gitignore alone would do -- until somebody commits
a build product or an icon, at which point the obvious repair is to weaken the
gate rather than to name the file. Naming them here means the list is the
thing that gets reviewed.
"""
import os
import subprocess
import sys
import tempfile

# NOT TEXT. A carriage return in any of these is a byte, not a line ending,
# and a gate that fails on it is a gate somebody switches off.
BINARY_SUFFIXES = (
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".bmp", ".webp",
    ".pdf", ".ttf", ".otf", ".woff", ".woff2",
    ".zip", ".gz", ".bz2", ".xz", ".tar", ".jar", ".apk", ".aab", ".dex",
    ".keystore", ".jks", ".p12", ".der", ".crt", ".key",
    ".so", ".o", ".a", ".class", ".pyc", ".bin", ".db", ".swp",
)

# GENERATED, or vendored and not ours to reformat. build/ is this tree's
# object and artifact directory, tools/ holds the downloaded Android SDK and
# the signing key, and lib/sqlite3.* is the upstream amalgamation -- which
# ships with CRLF in some releases and must not be rewritten to suit a gate.
# All four are in .gitignore today; they are named here so that adding one to
# the index does not silently widen what this gate has to accept.
GENERATED_PREFIXES = ("build/", "tools/", "__pycache__/")
GENERATED_PATHS = ("lib/sqlite3.c", "lib/sqlite3.h")


class GateError(Exception):
    """The check itself could not be carried out. Never a pass."""


def excluded(path):
    """Why this path is not first-party text, or None if it is."""
    low = path.lower()
    if low.endswith(BINARY_SUFFIXES):
        return "binary"
    if path in GENERATED_PATHS:
        return "generated"
    for pre in GENERATED_PREFIXES:
        if path == pre.rstrip("/") or path.startswith(pre):
            return "generated"
    return None


def manifest(root):
    """Every path git knows about here: tracked, or present and not ignored.

    The same enumeration symcheck and stalecheck use. -z because a filename
    may contain a newline, and a manifest that loses one file is a manifest
    that silently stops covering it.
    """
    p = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, capture_output=True)
    if p.returncode != 0:
        raise GateError(
            "git could not list the files in %s (exit %d): %s\n"
            "  A manifest that cannot be built is not an empty manifest."
            % (root, p.returncode,
               p.stderr.decode("utf-8", "replace").strip()))
    names = [n for n in p.stdout.decode("utf-8", "surrogateescape").split("\0")
             if n]
    if not names:
        raise GateError(
            "git listed NO files in %s, so this gate scanned nothing.\n"
            "  An empty list is a failure: it is the shape every broken\n"
            "  version of this check has had." % root)
    return names


def scan(root):
    """Report on one tree: (offenders, unreadable, scanned, skipped)."""
    offenders = []
    unreadable = []
    scanned = 0
    skipped = 0
    for name in manifest(root):
        if excluded(name):
            skipped += 1
            continue
        full = os.path.join(root, name)
        # A path git names that is not there at all is a staged or in-progress
        # deletion, which is an ordinary working state and not this gate's
        # business. A path that IS there and will not open is the failure.
        if not os.path.lexists(full):
            continue
        try:
            with open(full, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            unreadable.append((name, exc.strerror or str(exc)))
            continue
        scanned += 1
        at = data.find(b"\r")
        if at >= 0:
            offenders.append((name,
                              data.count(b"\n", 0, at) + 1,
                              "CRLF" if data[at:at + 2] == b"\r\n"
                              else "a bare CR"))
    return offenders, unreadable, scanned, skipped


def report(root):
    """Scan and print. Returns the process exit status."""
    offenders, unreadable, scanned, skipped = scan(root)
    for name, why in unreadable:
        print("crlfcheck: could not be READ: %s (%s)" % (name, why))
    if unreadable:
        print("  A file this gate cannot read is a failure, not a skip: it is")
        print("  indistinguishable from a file with nothing wrong in it, and")
        print("  a hole in a gate is exactly where the odd thing is.")
    for name, line, kind in offenders:
        print("crlfcheck: %s:%d has %s" % (name, line, kind))
    if offenders:
        print("  Carriage returns in first-party text. In a shell script the")
        print("  shebang then names an interpreter that does not exist, and")
        print("  the failure arrives at run time reading like a broken disk.")
    if offenders or unreadable:
        return 1
    print("crlfcheck: %d first-party text files, no CR (%d excluded as binary"
          " or generated)" % (scanned, skipped))
    return 0


# ------------------------------------------------------------ the gate's test
#
# Everything below exists so that a green run means something. It runs THIS
# script as a child against throwaway repositories, so what is exercised is
# the manifest, the exclusions and the reader -- not a re-implementation of
# them that could agree with a mistake.

CR = b"\r\n"

FAILURES = []
CHECKS = 0


def ck(name, cond):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(name)
        print("  FAIL %s" % name)


def git(root, *args):
    """git, deaf to this machine's configuration.

    A user-level core.excludesFile or a global gitignore would otherwise
    change which files the fixtures contain, and the fixture repositories are
    the only thing establishing that untracked-not-ignored is really covered.
    """
    env = dict(os.environ)
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["GIT_CONFIG_SYSTEM"] = os.devnull
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    p = subprocess.run(["git"] + list(args), cwd=root,
                       capture_output=True, env=env)
    if p.returncode != 0:
        raise GateError("fixture git %s failed: %s"
                        % (" ".join(args),
                           p.stderr.decode("utf-8", "replace").strip()))


def put(root, rel, data):
    """One fixture file, written as bytes so the CR survives the trip."""
    full = os.path.join(root, rel)
    parent = os.path.dirname(full)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(full, "wb") as fh:
        fh.write(data)


def run_gate(root):
    """This very script, scanning that tree. Returns (status, output)."""
    p = subprocess.run([sys.executable, os.path.abspath(__file__), "--scan"],
                       cwd=root, capture_output=True)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def selftest():
    """Break the rule on purpose, once per rule, and require the refusal."""
    tmp = tempfile.mkdtemp(prefix="crlfcheck-")
    try:
        # ---- a tree with a carriage return in every kind of first-party text
        bad = os.path.join(tmp, "bad")
        os.makedirs(bad)
        git(bad, "init", "-q")
        put(bad, ".gitignore", b"ignored.sh\n")
        # THE NAMED CASE: a shebang with a carriage return on it.
        put(bad, "run.sh", b"#!/bin/sh" + CR + b"echo hi" + CR)
        put(bad, "App.java", b"class App {}" + CR)
        put(bad, "gate.py", b"import sys" + CR)
        put(bad, "thing.conf", b"KEY=value" + CR)
        put(bad, "srv/deploy/pancra.conf", b"PANCRA_PORT=80" + CR)
        put(bad, "Makefile", b"all:" + CR)
        put(bad, "a.c", b"int main(void) { return 0; }" + CR)
        put(bad, "res.xml", b"<x/>" + CR)
        put(bad, "clean.c", b"int ok(void) { return 0; }\n")
        # Tracked, and excluded ANYWAY: the exclusions have to be real, or the
        # obvious implementation makes the gate unpassable and gets weakened.
        put(bad, "art.png", b"\x89PNG" + CR + b"\x00\x01")
        put(bad, "build/gen.c", b"generated" + CR)
        git(bad, "add", "-A")
        # Untracked and not ignored: the half of the manifest that a
        # tracked-only enumeration would miss, and where a new script starts
        # its life.
        put(bad, "untracked.sh", b"#!/bin/sh" + CR)
        # Untracked and IGNORED: a build product, which must not fail the gate.
        put(bad, "ignored.sh", b"#!/bin/sh" + CR)

        st, out = run_gate(bad)
        ck("a tree full of CRLF fails the gate", st == 1)
        for name in ("run.sh", "App.java", "gate.py", "thing.conf",
                     "srv/deploy/pancra.conf", "Makefile", "a.c", "res.xml",
                     "untracked.sh"):
            ck("a CRLF %s is refused" % name, name in out)
        ck("...and the shell case says it is a shebang problem",
           "shebang" in out)
        ck("...and names the line", "run.sh:1" in out)
        for name in ("art.png", "build/gen.c", "ignored.sh", "clean.c"):
            ck("%s is excluded, so the gate stays passable" % name,
               name not in out)

        # ---- the control: a tree with no carriage return in it passes
        good = os.path.join(tmp, "good")
        os.makedirs(good)
        git(good, "init", "-q")
        put(good, "run.sh", b"#!/bin/sh\necho hi\n")
        put(good, "gate.py", b"import sys\n")
        git(good, "add", "-A")
        st, out = run_gate(good)
        ck("a clean tree passes", st == 0)
        ck("...having actually scanned it", "2 first-party text files" in out)

        # ---- a file that exists and cannot be read is an ERROR
        #
        # A symlink to itself, because chmod 000 is not unreadable to root and
        # this gate is run in containers. It exists (lexists says so) and open
        # fails, which is the shape of every genuinely odd file: a broken
        # symlink into a gone directory, a dead mount, a permission accident.
        #
        # It is deliberately named without a suffix, so that this case stands
        # or falls on the unreadable rule alone. Called "loop.sh" it was also
        # killed by a mutant that dropped .sh from the manifest, and a case
        # that two different defects can fail is a case that names neither.
        unread = os.path.join(tmp, "unread")
        os.makedirs(unread)
        git(unread, "init", "-q")
        put(unread, "fine.c", b"int ok(void) { return 0; }\n")
        git(unread, "add", "-A")
        os.symlink("looplink", os.path.join(unread, "looplink"))
        st, out = run_gate(unread)
        ck("a file that cannot be read fails the gate", st == 1)
        ck("...saying which file, and that a skip would be a hole",
           "looplink" in out and "could not be READ" in out)

        # ---- a manifest that comes back empty is a failure, not a pass
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        git(empty, "init", "-q")
        st, out = run_gate(empty)
        ck("an empty manifest fails rather than reporting success", st == 1)
        ck("...and says it scanned nothing", "scanned nothing" in out)

        # ---- and a manifest that cannot be built at all is a failure too
        nogit = os.path.join(tmp, "nogit")
        os.makedirs(nogit)
        st, out = run_gate(nogit)
        ck("a tree git cannot enumerate fails the gate", st == 1)
    finally:
        # Only ever the directory mkdtemp just made, and only its own subtree.
        subprocess.run(["rm", "-rf", "--", tmp], check=False)


def main(argv):
    scan_only = "--scan" in argv[1:]
    if not scan_only:
        try:
            selftest()
        except GateError as exc:
            print("crlfcheck: the gate's own test could not run: %s" % exc)
            return 1
        if FAILURES:
            print("crlfcheck: the gate cannot be shown to FAIL, so a green run")
            print("  from it means nothing. Broken cases: %s"
                  % ", ".join(FAILURES))
            return 1
    try:
        st = report(os.getcwd())
    except GateError as exc:
        print("crlfcheck: %s" % exc)
        return 1
    if st == 0 and not scan_only:
        print("crlfcheck: %d self-test assertions pass, so the gate can still"
              " refuse a CRLF shebang" % CHECKS)
    return st


if __name__ == "__main__":
    sys.exit(main(sys.argv))
