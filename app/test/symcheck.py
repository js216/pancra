#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# symcheck.py --- does a comment still name something that exists?
# Copyright 2026 Jakob Kastelic
"""stalecheck for SYMBOLS, not files.

stalecheck catches a comment naming a file that is gone. Nothing caught the
much commoner version: a comment naming a FUNCTION, a STRUCT FIELD or a GLOBAL
that is gone -- renamed in a refactor, or deleted outright while the paragraph
explaining it stayed behind. The comments in this tree carry the reasons, so a
comment that names a symbol which no longer exists is not untidy, it is a
false explanation of live code, and it reads exactly as authoritatively as a
true one.

The rule is deliberately narrow, because a wide one is a gate nobody can keep
green:

  - only BACKTICKED tokens (`foo`, `g_bar`, `wt_form_open`) and tokens written
    with call parentheses (foo()) are checked. Prose that merely mentions a
    word is not a claim about a symbol;
  - only tokens that LOOK like C identifiers from this codebase -- containing
    an underscore, or beginning g_/s_, or ending in _t -- are checked. A
    backticked `int` or `NULL` is a language word, not one of ours;
  - the token must appear SOMEWHERE in the tracked sources. Not in the same
    file, not at a definition: the check is "does this name still exist", not
    "is it in scope here", which is the difference between a rule that catches
    dead names and one that fights every cross-module reference.

ALLOW below is for names that are real but live outside the sources: Android
and JNI methods, sqlite entry points, shell and Make identifiers quoted in
deployment comments.
"""
import re
import subprocess
import sys

# Files whose comments are checked, and the corpus a name may appear in.
#
# THE CORPUS IS EVERY SOURCE, not just the C. A deployment comment naming
# PANCRA_PORT or wait_healthy_since is naming something real -- it lives in a
# shell script and a conf file, which a C-only corpus cannot see, and the gate
# would then report the whole deployment vocabulary as dead.
SRC = re.compile(r"\.(c|h|java|py|sh|md|conf)$|(^|/)Makefile$")
CODE = SRC

# TODO.md is the WORK LIST. It names symbols precisely because they are to be
# changed or removed, so a name that is gone there is the item being done, not
# a stale comment. stalecheck excludes it for the same reason.
#
# NOTES.md is the ARCHIVE. Its entire purpose is to record designs that were
# removed, by the names they had, so the code beside the live invariant does
# not have to. Checking it would forbid the one file that is supposed to name
# dead symbols.
SKIP = re.compile(r"^(TODO\.md|NOTES\.md)$")

# Real, but defined outside this tree.
ALLOW = {
    # JNI / Android / Java
    "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
    "NewByteArray", "SetByteArrayRegion", "GetByteArrayElements",
    "FindClass", "GetMethodID", "GetStaticMethodID", "GetObjectClass",
    "CallVoidMethod", "CallStaticVoidMethod", "NewObjectArray",
    "SetObjectArrayElement", "ExceptionCheck", "ExceptionDescribe",
    "ExceptionClear", "DeleteLocalRef", "NewStringUTF", "RegisterNatives",
    "connectGatt", "writeDescriptor", "writeCharacteristic",
    "readCharacteristic", "readRemoteRssi", "setCharacteristicNotification",
    "getServices", "getCharacteristic", "getDescriptor", "getValue",
    "setValue", "setWriteType", "discoverServices", "startForeground",
    "onDestroy", "onStartCommand", "onCreate", "onBind",
    # sqlite
    "sqlite3_step", "sqlite3_prepare_v2", "sqlite3_finalize", "sqlite3_errmsg",
    "sqlite3_column_int", "sqlite3_column_int64", "sqlite3_column_text",
    "sqlite3_bind_text", "sqlite3_bind_int", "sqlite3_bind_int64",
    "sqlite3_bind_blob", "sqlite3_stmt", "sqlite3_backup", "integrity_check",
    # libc / POSIX quoted in comments
    "snprintf", "vsnprintf", "strcmp", "strncmp", "strcasestr", "strstr",
    "memcpy", "memmove", "memset", "sscanf", "strtol", "strtoul", "setsid",
    "kill_0", "clock_gettime", "gettimeofday", "pthread_mutex_lock",
    "sched_yield", "openat", "fsync", "fdatasync", "renameat", "O_RDONLY",
    "S_ISREG", "SEEK_SET", "SEEK_END", "SO_RCVTIMEO", "SO_SNDTIMEO",
    "MAP_FAILED", "PROT_READ", "getenv", "_Thread_local", "_Atomic",
    "atomic_int", "atomic_uint", "atomic_load", "atomic_store",
    "atomic_fetch_add", "atomic_exchange", "atomic_compare_exchange_strong",
    "atomic_compare_exchange_strong_explicit", "__STDC_HOSTED__",
    # deployment / shell / make
    "log_mark", "wait_ready", "prep_case", "t_ok", "t_bad", "t_show",
    "ls_files", "exclude_standard", "no_print_directory",
    # Not symbols at all: uname output the Makefile matches on, a sqlite
    # build macro, part of memory_order_seq_cst, and a placeholder standing
    # in for "some global" in an explanation.
    "cygwin_nt", "mingw64_nt", "SQLITE_DEBUG", "SEQ_CST", "g_something",
}


def tracked():
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True)
    return [f for f in out.stdout.splitlines() if f]


def comments(text, path):
    """Every comment body in one file, as (line number, text)."""
    out = []
    if path.endswith((".py", ".sh", ".conf")) or path.endswith("Makefile"):
        for n, line in enumerate(text.splitlines(), 1):
            i = line.find("#")
            if i >= 0:
                out.append((n, line[i:]))
        return out
    if path.endswith(".md"):
        return [(n, l) for n, l in enumerate(text.splitlines(), 1)]
    # C and Java: /* ... */ and // ...
    for m in re.finditer(r"/\*.*?\*/|//[^\n]*", text, re.S):
        out.append((text.count("\n", 0, m.start()) + 1, m.group(0)))
    return out


def strip_comments(text, path):
    """The file with its comments removed, so only real declarations remain."""
    if path.endswith(".md"):
        return ""  # prose end to end: it defines nothing
    if path.endswith((".py", ".sh", ".conf")) or path.endswith("Makefile"):
        out = []
        for line in text.splitlines():
            i = line.find("#")
            out.append(line if i < 0 else line[:i])
        return "\n".join(out)
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)


def main():
    files = tracked()
    corpus = []
    for f in files:
        if not CODE.search(f):
            continue
        try:
            corpus.append(strip_comments(
                open(f, encoding="utf-8", errors="replace").read(), f))
        except OSError:
            pass
    # WITH THE COMMENTS STRIPPED OUT, which is the whole difference between a
    # gate and a decoration. A comment is part of its file's text, so a corpus
    # built from raw file contents contains every name the comments mention --
    # including the dead ones, each of which then finds ITSELF and passes. The
    # first version of this check was green over the entire tree for exactly
    # that reason.
    live = set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", "\n".join(corpus)))

    # A token worth checking: one of ours by shape.
    ours = re.compile(r"^(g_[a-z0-9_]+|[a-z][a-z0-9]*_[a-z0-9_]+|[A-Z][A-Za-z0-9]*_[A-Z0-9_]+)$")
    bad = []
    for f in files:
        if not SRC.search(f) or SKIP.match(f):
            continue
        try:
            text = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for line, body in comments(text, f):
            # EVERY IDENTIFIER INSIDE A BACKTICK SPAN, not only spans that
            # are one identifier. A comment quoting a whole expression is
            # making a claim about every name in it, and the first version of
            # this gate matched a lone `foo` only -- so a quoted expression
            # full of dead names sailed through while a bare mention of one
            # was caught. keypad.h had exactly that: three retired globals in
            # one backticked conditional, invisible to the first rule.
            toks = set()
            for span in re.findall(r"`([^`\n]{1,200})`", body):
                toks |= set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", span))
            toks |= set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\(\)", body))
            for tk in toks:
                if tk in ALLOW or tk in live or not ours.match(tk):
                    continue
                bad.append((f, line, tk))

    if bad:
        print("symcheck: a comment names a symbol that does not exist.")
        print("  The comments here carry the REASONS, so one naming a")
        print("  function or global that was renamed or deleted is not")
        print("  untidy -- it is a false explanation of live code, and it")
        print("  reads exactly as authoritatively as a true one.")
        print("  Fix the name, or drop the sentence if what it explained is")
        print("  gone. If the name is real but lives outside this tree, add")
        print("  it to ALLOW in app/test/symcheck.py and say why.")
        for f, line, tk in sorted(set(bad)):
            print("    %s:%d: `%s`" % (f, line, tk))
        return 1
    print("\033[1;32msymcheck\033[0m: every symbol a comment names still exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
