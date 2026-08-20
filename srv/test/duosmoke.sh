#!/bin/sh
# duosmoke.sh --- is the thing we deployed still the thing that is running,
# and is it still answering?
#
# IT NEVER DEPLOYS AND NEVER MUTATES THE BOARD. That is the whole point: a
# smoke test that installs first can only tell you that installing works, and
# the question here is whether last week's install is still alive.
#
# Every path it uses comes from srv/deploy/pancra.conf, which is the one place
# the deployment is described. This file used to spell the board's directory
# out three times -- for the executable, the pid file and the log -- naming the
# directory the service had before it was called Pancra. Nothing else in the
# repository named those paths, so a test WAS the deployment contract, and it
# was one nobody could check.
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

LOCAL=${1:-$HERE/$PANCRA_LOCAL_BIN}
SSH="$PANCRA_SSH"
# For front_door(): this file asks the public URL, and naming what the request
# had to cross is the difference between a report an operator can act on and
# one that sends them to a server that is running perfectly.
. "$HERE/srv/deploy/health.sh"

fail() { printf 'duosmoke: FAIL: %s\n' "$*" >&2; exit 1; }
[ -f "$LOCAL" ] || fail "cross artifact not found: $LOCAL"

# THE EXACT ARTIFACT, by content. A version string would be satisfied by a
# rebuild that never made it onto the board.
local_hash=$(sha256sum "$LOCAL" | awk '{print $1}')
remote_hash=$($SSH "$PANCRA_HOST" "sha256sum '$PANCRA_BIN'" | awk '{print $1}') ||
   fail "cannot hash the deployed artifact at $PANCRA_BIN"
[ "$local_hash" = "$remote_hash" ] ||
   fail "deployed hash $remote_hash != local $local_hash (run 'make duodeploy')"

# ALIVE, AND LISTENING. The pid file alone is a file; the log line is the
# server's own statement that it got as far as accepting connections.
$SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "deployed process/listener is unhealthy"
set -eu
pid=\$(cat '$PANCRA_PID')
kill -0 "\$pid"
grep -q '$PANCRA_READY_LINE' '$PANCRA_LOG'
EOF

# ...AND IT IS THE ARTIFACT WE JUST HASHED. This verb collected the same three
# unrelated facts the deploy used to -- a hash of a file on disk, a pid that
# answers kill -0, a line somewhere in a log -- and none of them had to be
# about the same program.
#
# It matters MORE here than in a deploy. duosmoke asks about a service that
# has been running for weeks, so its pid has had weeks to die and be REUSED by
# something else, and `kill -0` succeeds on a recycled pid exactly as happily
# as on the right one. Only /proc/<pid>/exe can tell them apart.
exe_is "$local_hash" ||
   fail "the running process is NOT $local_hash -- the binary on disk is the
   one we built, but something else is serving"

# ...and THIS backend answers, not merely the public name. See health.sh.
backend_ok ||
   fail "the process is listening but the board's own listener did not answer"

# ...and reachable from OUTSIDE, over TLS, as a stranger. A server that is
# listening on the board and unreachable from the internet is down as far as
# every phone is concerned.
# THE REQUEST CROSSES THE FRONT DOOR, so a failure here is not necessarily
# this server: it is the server's TLS, or whatever maps the public port to
# PANCRA_PORT. The verb whose entire job is the public URL has to say so.
body=$(curl --fail --silent --show-error --max-time 15 "$PANCRA_URL") ||
   fail "HTTPS request to $PANCRA_URL failed (front door: $(front_door))"
printf '%s' "$body" | grep -qi "$PANCRA_URL_MARKER" ||
   fail "the response from $PANCRA_URL is not the Pancra login page"

printf 'duosmoke: exact artifact %s is live and answering HTTPS\n' "$local_hash"
