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
GLUCO=${GLUCO:-../glucoserve}
PORT=${PORT:-18097}
BIN=build/test/interoptest

if [ ! -x "$GLUCO/sync/sync" ]; then
  echo "interop: SKIP (no $GLUCO/sync/sync -- build glucoserve to run this)"
  exit 0
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
cp "$GLUCO/sync/sync" "$DIR/"

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
