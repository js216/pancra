#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# inclusions.py --- the module graph, and what it refuses to let you hide
# Copyright (c) 2026 Jakob Kastelic
"""
Generate a Graphviz inclusion graph for C projects, and fail on a cycle.

A node is a MODULE: foo.c and foo.h are one node, because they are one thing.
An edge is a dependency. A cycle means two modules cannot be read, tested,
linked or reasoned about apart, and no compiler will ever say so -- include
guards make it build perfectly. That is the whole point of the check.

The rest of this file exists because "no cycle in the quoted #includes" is
easy to satisfy dishonestly. Every dependency below is a real one and is
drawn as such:

  QUOTED INCLUDE       #include "b.h"          a solid edge.
  ANGLE INCLUDE        #include <b.h>          the same dependency with the
                       quotes swapped: identical to the compiler when the
                       directory is on -I, invisible to a naive scanner. Any
                       angle include naming a file in the input set is drawn,
                       in blue, and reported. The 24 C99 headers are ignored;
                       every OTHER system header is listed, because "what
                       non-standard thing did this pure C program end up
                       depending on" is worth knowing.
  EXTERN DECLARATION   extern int b_thing(void);   in a .c, this is a call
                       into another module with the header left out -- the
                       same coupling as an include, plus a second declaration
                       that can silently drift from the definition. The symbol
                       is resolved to the file that DEFINES it and drawn, in
                       red, dashed.

Two structural refusals, because they make the graph lie rather than fail:

  STEM COLLAPSE        two input files sharing a stem (app/util.c and
                       srv/util.c) become ONE node, and their mutual edges
                       then look like self-loops, which this script discards.
                       A cycle would vanish silently. Refused outright.
  GOD HEADER           a header whose declarations are implemented across
                       several .c files is a dumping ground: including it
                       makes the graph a star, which is acyclic and worse
                       than what it replaced. Reported, with the offenders.

Writes DOT to stdout; exits 1 if anything above is wrong.
"""

import sys
import re
from pathlib import Path
from collections import defaultdict

QUOTED_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
ANGLE_RE = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
# `extern` at the start of a declaration, capturing the declared name: the
# last identifier before '(' for a function, or before '[', '=' or ';' for a
# variable.
EXTERN_RE = re.compile(r'^\s*extern\s+[^;{]*?(\w+)\s*(?:\(|\[|=|;)',
                       re.M)  # re.M, or only a file's FIRST line is scanned
# A definition at file scope: a line starting in column 0 that names the
# symbol. Deliberately crude -- it only has to attribute a symbol to a file.
# A definition at file scope. `static` is EXCLUDED deliberately: a static
# function cannot satisfy a declaration in a header, and crediting one with it
# attributes the header to the wrong module -- notify.c has a private
# white_color, and uidraw.c has the real one.
DEF_RE = re.compile(
    r'^(?!static\b)[A-Za-z_][\w \t*]*?\b(\w+)\s*'
    r'(?:\(|\[[^\]]*\]\s*(?:=|;)|=|;)', re.M)
# ...and a PROTOTYPE is not a definition. lib/rand.c hand-declares open, read
# and close at file scope; counted as definitions they made dexlibc.h -- the
# freestanding libc declarations -- look like a header rand.c implements.
PROTO_RE = re.compile(r'\)\s*;\s*$')

# The C99 standard headers, and nothing else. Anything else in <> is either
# one of ours (a cheat) or a platform dependency worth seeing.
C99_HEADERS = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
    "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
    "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
    "stdlib.h", "string.h", "tgmath.h", "time.h", "wchar.h", "wctype.h",
}


def read_lines(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='ignore') as f:
            return f.readlines()
    except OSError as e:
        print(f"inclusions: cannot read {path}: {e}", file=sys.stderr)
        return []


def blank(s):
    """Same length, same lines, no content -- so offsets and line numbers
    survive and nothing inside can match."""
    return "".join("\n" if ch == "\n" else " " for ch in s)


def strip_comments(text, strings=True):
    """Blank out comments so they cannot fake a match.

    `strings` also blanks string literals, which is what the extern and
    definition scans want -- and what the INCLUDE scan must not have, since
    `#include "b.h"` is a line whose payload is a string literal. Getting
    that backwards silently erased every quoted include, which is the one
    edge this program exists to draw.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
            i = j
        elif strings and text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(blank(text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def rel(p):
    """A path the reader can paste into an editor."""
    try:
        return str(Path(p).resolve().relative_to(Path.cwd()))
    except ValueError:
        return str(p)


def node_name(path):
    """The MODULE a file belongs to: foo.c and foo.h are both `foo`."""
    return Path(path).stem


def check_stems(files):
    """Refuse an input set in which two files would become one node.

    The script merges by stem and drops self-loops, so app/util.c and
    srv/util.c handed in together would silently swallow every edge between
    the two programs -- including a cycle. There is no safe answer here: the
    node identity is genuinely ambiguous, so say so and stop.
    """
    seen = defaultdict(set)
    for f in files:
        seen[node_name(f)].add(str(Path(f).parent))
    bad = {k: sorted(v) for k, v in seen.items() if len(v) > 1}
    if not bad:
        return 0
    print("inclusions: two files would become ONE node:", file=sys.stderr)
    for stem, dirs in sorted(bad.items()):
        print(f"  {stem}: " + ", ".join(f"{d}/{stem}.*" for d in dirs),
              file=sys.stderr)
    print("  A node is a file STEM, and edges within a node are dropped as"
          "\n  self-loops -- so a cycle between these would disappear rather"
          "\n  than fail. Pass one program's files at a time, or rename.",
          file=sys.stderr)
    return 1


def is_stub(path):
    """A test double, not a module.

    app/stub_*.c stands in for the platform -- the NDK, libc, the log -- so a
    host test can link. Crediting it with the platform's declarations makes
    ndk.h and string.h look like headers that speak for another module, when
    what they declare is not ours to implement at all.
    """
    return Path(path).name.startswith("stub_")


def definition_index(files):
    """symbol -> the .c that defines it, from ONE pass over the sources.

    Built once. Asking per symbol meant rescanning every file for every
    declaration in every header, which on this tree took minutes -- a gate
    nobody will run is a gate that does not exist.
    """
    index = {}
    for f in files:
        if f.suffix != ".c" or is_stub(f):
            continue
        body = strip_comments("".join(read_lines(f)))
        for m in DEF_RE.finditer(body):
            line = body[m.start():body.find("\n", m.start())]
            if PROTO_RE.search(line):
                continue
            index.setdefault(m.group(1), f)
    return index


def build_graph(files):
    """graph[node] -> {node: kind}, plus everything worth reporting.

    kind is "include", "angle" or "extern" -- all three are dependencies; the
    last two are dependencies somebody tried not to write down.
    """
    files = [Path(f).resolve() for f in files]
    here = Path.cwd()
    path_map = {}
    for f in files:
        try:
            path_map[str(f.relative_to(here))] = f
        except ValueError:
            pass  # outside the tree; the filename map still resolves it
    filename_map = {f.name: f for f in files}

    graph = defaultdict(dict)
    foreign = defaultdict(set)   # non-C99 system header -> including modules
    angle_cheats = []            # (file, header)
    externs = []                 # (file, symbol, owner)
    defs = definition_index(files)

    def target(inc):
        if inc in path_map:
            return node_name(path_map[inc])
        if Path(inc).name in filename_map:
            return node_name(filename_map[Path(inc).name])
        return None

    for f in files:
        src = node_name(f)
        text = "".join(read_lines(f))
        body = strip_comments(text, strings=False)
        for line in body.split("\n"):
            m = QUOTED_RE.match(line)
            if m:
                tgt = target(m.group(1))
                if tgt and tgt != src:
                    graph[src].setdefault(tgt, "include")
                continue
            m = ANGLE_RE.match(line)
            if m:
                inc = m.group(1)
                if inc in C99_HEADERS:
                    continue
                tgt = target(inc)
                if tgt:
                    angle_cheats.append((f, inc))
                    if tgt != src:
                        graph[src][tgt] = "angle"
                else:
                    foreign[inc].add(src)
                continue
        if f.suffix != ".c":
            continue
        for m in EXTERN_RE.finditer(strip_comments(text)):
            sym = m.group(1)
            owner = defs.get(sym)
            if owner is None:
                externs.append((f, sym, None))
                continue
            tgt = node_name(owner)
            externs.append((f, sym, tgt))
            if tgt != src:
                graph[src][tgt] = "extern"
    return graph, foreign, angle_cheats, externs


def common_prefix(names):
    """The longest `word_` prefix shared by every name, or ''."""
    if not names:
        return ""
    parts = [n.split("_") for n in names]
    out = []
    for i in range(min(len(p) for p in parts)):
        if len({p[i] for p in parts}) != 1:
            break
        out.append(parts[i if False else 0][i])
    return "_".join(out) + "_" if out else ""


def prefixes(names):
    """The distinct `word_` heads of these symbols: draw_, fmt_, render_..."""
    return {n.split("_")[0] + "_" if "_" in n else n for n in names}


def god_headers(files):
    """Headers that speak for several modules WITHOUT declaring a contract.

    A single-purpose header is the interface of ONE module: its declarations
    are defined by its own .c, or it is pure types and macros and defines
    nothing anywhere.

    ONE EXCEPTION, and it is the pattern that BREAKS cycles rather than
    causing them: a header may declare what its module REQUIRES of a provider
    -- dexdriver.h's drv_*, otble.h's ot_drv_* -- which someone else
    implements. The owner names the contract; the provider satisfies it. What
    tells the two apart mechanically is that a contract is ONE named thing:
    every declaration implemented elsewhere shares one prefix. A dumping
    ground has no such name, because it is not one idea.
    """
    files = [Path(f).resolve() for f in files]
    # NOT `(*name)(...)`: a function-POINTER declaration's first parenthesis
    # holds the pointer, so the old pattern captured the return type -- which
    # is how util.h came to "declare" a symbol called `void`.
    decl = re.compile(r'^[A-Za-z_][\w \t*]*?\b(\w+)\s*\((?!\s*\*)[^;{]*\)\s*;',
                      re.M)
    defs = definition_index(files)
    out = []
    for h in files:
        if h.suffix != ".h":
            continue
        body = strip_comments("".join(read_lines(h)))
        owners = defaultdict(list)
        for m in decl.finditer(body):
            sym = m.group(1)
            owner = defs.get(sym)
            if owner is not None:
                owners[node_name(owner)].append(sym)
        mine = node_name(h)
        foreign = {m: s for m, s in owners.items() if m != mine}
        if len(foreign) < 1:
            continue
        # Every foreign declaration under ONE prefix is a declared contract.
        allforeign = [s for syms in foreign.values() for s in syms]
        pfx = common_prefix(allforeign)
        ownpfx = common_prefix(owners.get(mine, []))
        if pfx and pfx != ownpfx and len(allforeign) > 1:
            continue
        # A header with NO MODULE OF ITS OWN is an interface, not a
        # module's dumping ground -- it has no module. What it may still be is
        # a PILE, so it has to name what it declares: at most two contracts,
        # which is exactly a BOUNDARY (bletrans.h: what the app asks of the
        # BLE transport, one direction only) or one segregated interface
        # (menuview.h, status.h: what a workflow needs of the screen, without
        # depending on how a frame is assembled). Three or more unrelated
        # prefixes is a pile with a header around it. One module's surface,
        # however heterogeneous its names, is that module's interface --
        # status.h is what a workflow needs of the screen and nothing else.
        if mine not in owners and (len(foreign) == 1 or
                                   len(prefixes(allforeign)) <= 2):
            continue
        out.append((h, {m: owners[m] for m in owners}))
    return out


def detect_cycles(graph):
    visited, stack, cycles = set(), set(), []

    def visit(node, path):
        if node in stack:
            cycles.append(path[path.index(node):])
            return
        if node in visited:
            return
        visited.add(node)
        stack.add(node)
        for neighbour in graph.get(node, ()):
            visit(neighbour, path + [neighbour])
        stack.remove(node)

    for node in list(graph):
        visit(node, [node])
    return cycles


STYLE = {
    "include": '',
    "angle": ' [color=blue, label="<>"]',
    "extern": ' [color=red, style=dashed, label="extern"]',
}


def write_graphviz(graph, cycles):
    print("digraph inclusions {")
    print("  node [shape=box];")
    cycle_edges = set()
    for c in cycles:
        cycle_edges.update(zip(c, c[1:]))  # the path already closes on itself
    for src, targets in graph.items():
        for tgt, kind in targets.items():
            if (src, tgt) in cycle_edges:
                print(f'  "{src}" -> "{tgt}" [color=red, penwidth=3];')
            else:
                print(f'  "{src}" -> "{tgt}"{STYLE[kind]};')
    print("}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file1> <file2> ...", file=sys.stderr)
        sys.exit(1)

    files = sys.argv[1:]
    if check_stems(files):
        sys.exit(1)

    graph, foreign, angle_cheats, externs = build_graph(files)
    cycles = detect_cycles(graph)
    gods = god_headers(files)
    write_graphviz(graph, cycles)

    bad = 0

    # ADVISORY, always: what this pure C program actually rests on.
    if foreign:
        print("System headers used (outside C99):", file=sys.stderr)
        for h in sorted(foreign):
            users = ", ".join(sorted(foreign[h]))
            print(f"  <{h}>  <- {users}", file=sys.stderr)

    if angle_cheats:
        bad = 1
        print("Angle brackets around OUR OWN header:", file=sys.stderr)
        for f, inc in angle_cheats:
            print(f"  {rel(f)}: #include <{inc}>", file=sys.stderr)
        print("  Identical to the compiler, invisible to a scanner reading"
              "\n  quotes. The dependency is real: write it with quotes.",
              file=sys.stderr)

    if externs:
        bad = 1
        print("extern declarations (a header left out, not a "
              "dependency removed):", file=sys.stderr)
        for f, sym, tgt in externs:
            where = tgt if tgt else "?? (defined nowhere in this set)"
            print(f"  {rel(f)}: extern {sym} -> {where}", file=sys.stderr)
        print("  For a function this is redundant -- functions are extern"
              "\n  already -- so it can only be a way of not naming the"
              "\n  header. It adds a second declaration that no compiler"
              "\n  checks against the definition.", file=sys.stderr)

    if gods:
        bad = 1
        print("Headers that speak for several modules without naming a "
              "contract:", file=sys.stderr)
        for h, owners in gods:
            print(f"  {rel(h)}", file=sys.stderr)
            for mod in sorted(owners):
                syms = ", ".join(sorted(owners[mod])[:6])
                print(f"      {mod}: {syms}", file=sys.stderr)
        print("  A header is one module's interface. One that collects"
              "\n  several makes every includer depend on all of them --"
              "\n  acyclic, and worse than the cycle it replaced. A header MAY"
              "\n  declare what it requires of a provider (dexdriver.h's"
              "\n  drv_*), because that is one named contract: every"
              "\n  declaration implemented elsewhere then shares one prefix.",
              file=sys.stderr)

    if cycles:
        bad = 1
        print("Inclusion cycles detected:", file=sys.stderr)
        for c in cycles:
            print("  " + " -> ".join(c), file=sys.stderr)

    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
