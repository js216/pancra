#!/bin/bash
# Run pancra's sync client against the REAL server (srv/).
#
# This is the only check that can catch the two implementations agreeing with
# themselves but not with each other, so it starts the actual server binary,
# mints a pairing code through its actual web interface, and lets the client
# pair and sync for real.
#
# Skipped (not failed) when the server is not built: pancra must remain
# buildable on a machine that has never built the server.
set -u
# The server is part of this repository now; no sibling checkout to find.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# THE SAME TREE THE MAKEFILE BUILT IT INTO. $(TESTDIR) keys the host test
# binaries by build mode, so a hard-wired build/app/test/interoptest here would
# run the plain binary whatever the caller had just compiled. APP_TESTBIN, not
# APP_TESTDIR: the latter is a suite's per-suite FIXTURE directory
# ($(TESTDIR)/interoptest/), which is where this test's data files go and is one
# level below the binary. Same default as app/test/testdir.h.
BIN=${APP_TESTBIN:-build/app/test}/interoptest

# Same rule as tlstest: this is the ONLY test that puts the app's sync client
# and the server's implementation on the same wire, so it disappearing quietly
# means the protocol agreement is unverified while the gate stays green.
if [ ! -f "$ROOT/srv/sync.c" ]; then
  if [ "${ALLOW_SKIP:-0}" = "1" ]; then
    echo "interop: SKIP (no srv/ in this tree, ALLOW_SKIP=1)"
    exit 0
  fi
  echo "interop: srv/ is missing, so the app<->server protocol is UNTESTED."
  echo "  re-run with ALLOW_SKIP=1 to accept that."
  exit 1
fi
# Sourced only once srv/ is known to be here: the library lives in it.
. "$ROOT/srv/test/testlib.sh"
need python3 "the app<->server interop check" || exit 1
need curl "the app<->server interop check" || exit 1

# ASKED FOR, not chosen: `make check -j4` has several servers up at once. And
# asking is not holding -- the port is not settled until `serve` below has had
# the kernel confirm that the server this script started is the process
# listening on it. Answering is not the same as being ours: another suite's
# server on the same number answers too, and this whole file would then be
# checking that the app agrees with a database it never wrote to.
PORT=${PORT:-}

DIR=$(mktemp -d)
T_TMP=$DIR
DB=$DIR/sync.db
SRVPID=
cleanup() {
  if [ -n "$SRVPID" ]; then
    kill "$SRVPID" 2>/dev/null || true
    wait "$SRVPID" 2>/dev/null || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT INT TERM
# BUILD it rather than trusting whatever binary is in that tree. The server is
# cross-compiled for the RISC-V board on every deploy, and the binary left
# behind afterwards cannot execute here -- which failed this test with "Exec
# format error" and looked like a protocol break. The server's .build-mode
# stamp makes a plain `make` rebuild natively when the last build was a cross
# one, so this is a no-op except after a deploy.
# NO RECURSIVE MAKE HERE.
#
# This used to run `make -C "$ROOT" srv` itself. Under `make check -j4` that is
# a second make process building build/srv/sync while srvcheck, srvasan and
# duocheck are building server objects too -- two makes writing the same
# targets, which fails intermittently and says only "cannot build the server".
# The Makefile now lists $(SRVBIN) as a prerequisite of interoptest, so make
# orders the build itself. Building here is the fallback for running this
# script by hand.
if [ ! -x "$ROOT/build/srv/sync" ]; then
  make -C "$ROOT" srv >/dev/null 2>&1 || {
    echo "interop: cannot build the server"; exit 1; }
fi
cp "$ROOT/build/srv/sync" "$DIR/"
# Ask the binary whether it runs HERE rather than matching an architecture
# name; the message is otherwise a confusing "adduser failed".
if ! "$DIR/sync" bench >/dev/null 2>&1; then
  echo "interop: the server build does not run on this machine"; exit 1
fi

MAIL=interop@example.com
PW=correcthorsebattery
printf '%s\n' "$PW" | "$DIR/sync" adduser "$MAIL" stdin "$DIR" >/dev/null ||
  { echo "adduser failed"; exit 1; }
interop_up() { # interop_up <port>
  "$DIR/sync" "$1" "$DIR" >/dev/null 2>&1 &
  T_PID=$!
}
if [ -n "$PORT" ]; then
  interop_up "$PORT"
  SRVPID=$T_PID
  if ! wait_ready "$SRVPID" "http://127.0.0.1:$PORT/"; then
    echo "interop: server did not become ready on port $PORT"
    exit 1
  fi
elif serve "interop" http / interop_up; then
  PORT=$T_PORT
  SRVPID=$T_PID
else
  echo "interop: server did not become ready on any port"
  exit 1
fi
U="http://127.0.0.1:$PORT"
ck_owns "the server the app is about to talk to is the one we started" \
   "$SRVPID" "$PORT"
curl -s -c "$DIR/jar" -o /dev/null -X POST \
     -d "email=$MAIL&password=$PW" "$U/login"
SET=$(curl -s -b "$DIR/jar" "$U/settings")
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
SET=$(curl -s -b "$DIR/jar" -X POST -d "csrf=$CSRF" "$U/settings/pair")
CODE=$(printf '%s' "$SET" | grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
if [ -z "${CODE:-}" ]; then
  echo "interop: could not obtain a pairing code from the server"
  exit 1
fi

./$BIN "$PORT" "$MAIL" "$CODE" "$DIR" < /dev/null
rc=$?

# THE CLAIM, CHECKED FROM OUTSIDE BOTH IMPLEMENTATIONS.
#
# Everything above is the two implementations talking to each other, so a
# shared misunderstanding satisfies all of it. This asks the server's own
# storage what it holds, and it is the last state the C test left there: the
# readings log was wiped locally and then RESTORED from the server, so the one
# row the server held is the one row it must still hold.
#
# The version of this check that stood here before piped `sqlite3` to
# /dev/null, and read an empty answer as "nothing to complain about":
#
#     n=$(sqlite3 "$DIR/sync.db" "SELECT count(*) ..." 2>/dev/null)
#     if [ -n "${n:-}" ] && [ "$n" != "1" ]; then ... else PASS
#
# On a machine with no sqlite3 -- which is most of them, and this one -- `n`
# was empty every time and the check printed PASS for months without ever
# reading the database. A check that passes because it could not run is worse
# than no check, so this one FAILS when the query fails.
if [ $rc -eq 0 ]; then
  echo "== what the server's own database holds, asked from outside =="
  # TWO rows, not one. The check that used to stand here wanted exactly one --
  # and would have FAILED every time it ran, because the log's '#' header is a
  # row like any other and lands in bucket 0 (see row_bucket in app/sync.c).
  # Nobody ever saw that, because with no sqlite3 on the machine it never ran.
  # An expectation that has never been evaluated is a guess.
  ck_db "the server kept the surviving reading and the header, and nothing else" \
     "SELECT count(*) FROM logrow WHERE log='readings'" "^2$"
  ck_db "...the reading, byte for byte" \
     "SELECT line FROM logrow WHERE log='readings' AND bucket=20000" \
     "^1728000000,120,0,-70,3,7,1728000000,-420,0$"
  ck_db "...and the header in its own bucket" \
     "SELECT line FROM logrow WHERE log='readings' AND bucket=0" \
     "^# epoch,glucose,trend10,rssi,recv_lag,source_id,raw,tz,kind$"
  # THE DELETIONS ARE REAL ON THE SERVER. The client dropped a whole UTC day
  # (1728086400) and two rows inside the surviving one; a restore that pulled
  # them back, or a push that never removed them, shows up only here -- the
  # digests agree either way if both sides made the same mistake.
  nk_db "the day the phone deleted is gone from the server" \
     "SELECT line FROM logrow WHERE log='readings'" "1728086400"
  nk_db "...and so are the rows deleted from the day that stayed" \
     "SELECT line FROM logrow WHERE log='readings'" "1728000300\|1728000600"
  ck_db "...leaving exactly two buckets: the header's and the day's" \
     "SELECT group_concat(DISTINCT bucket) FROM logrow WHERE log='readings'" \
     "^0,20000$"
  # The other two logs the client pushed are still there: a restore of one log
  # must not disturb the rest of the record.
  ck_db "the insulin log the client pushed is still held" \
     "SELECT count(*)>0 FROM logrow WHERE log='insulin'" "^1$"
  ck_db "...and the weight log too" \
     "SELECT count(*)>0 FROM logrow WHERE log='weight'" "^1$"
  [ "$fail" -eq 0 ] || rc=1
fi
t_end
[ "$fail" -eq 0 ] || rc=1
exit $rc
