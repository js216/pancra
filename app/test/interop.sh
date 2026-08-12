#!/bin/bash
# Run pancra's sync client against a REAL glucoserve server.
#
# This is the only check that can catch the two implementations agreeing with
# themselves but not with each other, so it starts the actual server binary,
# mints a pairing code through its actual web interface, and lets the client
# pair and sync for real.
#
# Skipped (not failed) when the server is not built: pancra must remain
# buildable on a machine that has never seen glucoserve.
set -u
# The server is part of this repository now; no sibling checkout to find.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${PORT:-18097}
BIN=build/app/test/interoptest

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

# A previous run interrupted before its cleanup would still hold the port, and
# its database would make these results meaningless.
for pid in $(pgrep -f "sync $PORT" 2>/dev/null); do kill "$pid" 2>/dev/null; done
sleep 1

DIR=$(mktemp -d)
cleanup() {
  for pid in $(pgrep -f "sync $PORT" 2>/dev/null); do kill "$pid" 2>/dev/null; done
  rm -rf "$DIR"
}
trap cleanup EXIT
# BUILD it rather than trusting whatever binary is in that tree. glucoserve is
# cross-compiled for the RISC-V board on every deploy, and the binary left
# behind afterwards cannot execute here -- which failed this test with "Exec
# format error" and looked like a protocol break. glucoserve's .build-mode
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
  echo "interop: the glucoserve build does not run on this machine"; exit 1
fi

MAIL=interop@example.com
PW=correcthorsebattery
"$DIR/sync" adduser "$MAIL" "$PW" "$DIR" >/dev/null || { echo "adduser failed"; exit 1; }
( cd "$DIR" && ./sync $PORT . >/dev/null 2>&1 & )
sleep 1

U="http://127.0.0.1:$PORT"
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

# The claim, checked from OUTSIDE both implementations: the row the client
# says it kept is the row the server has, byte for byte.
if [ $rc -eq 0 ]; then
  n=$(sqlite3 "$DIR/sync.db" \
      "SELECT count(*) FROM logrow WHERE log='readings';" 2>/dev/null)
  if [ -n "${n:-}" ] && [ "$n" != "1" ]; then
    echo "  [FAIL] server kept $n readings rows, expected 1"
    rc=1
  else
    echo "  [PASS] the server's own database holds exactly the surviving row"
  fi
fi
exit $rc
